#include "Dictionary.h"

#include <algorithm>
#include <cstring>

#include "Unicode.h"

namespace nova {

std::string_view CodeView(const char (&code)[kCodeBytes]) {
    size_t n = 0;
    while (n < kCodeBytes && code[n] != '\0') ++n;
    return std::string_view(code, n);
}

bool WildcardMatch(std::string_view pattern, std::string_view code) {
    // Iterative glob matching with single-star backtracking.
    size_t p = 0, c = 0, starP = std::string_view::npos, starC = 0;
    while (c < code.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            starP = p++;
            starC = c;
        } else if (p < pattern.size() && pattern[p] == code[c]) {
            ++p;
            ++c;
        } else if (starP != std::string_view::npos) {
            p = starP + 1;
            c = ++starC;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

namespace {

template <typename T>
bool SectionOk(const DictSection& s, size_t fileSize) {
    if (s.offset % alignof(uint32_t) != 0) return false;
    uint64_t end = static_cast<uint64_t>(s.offset) + static_cast<uint64_t>(s.count) * sizeof(T);
    return end <= fileSize;
}

bool CodeOk(const char (&code)[kCodeBytes]) {
    if (code[kCodeBytes - 1] != '\0') return false;
    for (char ch : code) {
        if (ch == '\0') break;
        if (!IsCodeLetter(ch)) return false;
    }
    return true;
}

int CompareCode(const CodeIndexItem& item, std::string_view code) {
    return CodeView(item.code).compare(code);
}

}  // namespace

bool Dictionary::Load(const void* data, size_t size, std::string* error) {
    auto fail = [&](const char* why) {
        header_ = nullptr;
        if (error) *error = why;
        return false;
    };
    if (data == nullptr || size < sizeof(DictHeader)) return fail("file too small");
    const auto* h = static_cast<const DictHeader*>(data);
    if (std::memcmp(h->magic, kDictMagic, 4) != 0) return fail("bad magic");
    if (h->version != kDictVersion) return fail("unsupported version");
    if (h->fileSize != size) return fail("size mismatch");
    if (!SectionOk<char16_t>(h->strings, size) || !SectionOk<DictEntry>(h->entries, size) ||
        !SectionOk<CharIndexItem>(h->byChar, size) || !SectionOk<AssocItem>(h->assoc, size) ||
        !SectionOk<WordPair>(h->pairs, size) || !SectionOk<float>(h->backoff, size))
        return fail("bad section");
    for (const DictSection& s : h->indices)
        if (!SectionOk<CodeIndexItem>(s, size)) return fail("bad index section");

    base_ = static_cast<const char*>(data);
    strings_ = reinterpret_cast<const char16_t*>(base_ + h->strings.offset);
    stringCount_ = h->strings.count;
    entries_ = reinterpret_cast<const DictEntry*>(base_ + h->entries.offset);
    entryCount_ = h->entries.count;
    for (int i = 0; i < kIndexCount; ++i) {
        index_[i] = reinterpret_cast<const CodeIndexItem*>(base_ + h->indices[i].offset);
        indexCount_[i] = h->indices[i].count;
    }
    byChar_ = reinterpret_cast<const CharIndexItem*>(base_ + h->byChar.offset);
    byCharCount_ = h->byChar.count;
    assoc_ = reinterpret_cast<const AssocItem*>(base_ + h->assoc.offset);
    assocCount_ = h->assoc.count;
    pairs_ = reinterpret_cast<const WordPair*>(base_ + h->pairs.offset);
    pairCount_ = h->pairs.count;
    backoff_ = reinterpret_cast<const float*>(base_ + h->backoff.offset);
    backoffCount_ = h->backoff.count;

    for (uint32_t i = 0; i < entryCount_; ++i) {
        const DictEntry& e = entries_[i];
        if (static_cast<uint64_t>(e.text) + e.textLen > stringCount_ || e.textLen == 0)
            return fail("bad entry text");
        if (static_cast<uint64_t>(e.simp) + e.simpLen > stringCount_)
            return fail("bad entry simplified text");
        if (!CodeOk(e.cj5) || !CodeOk(e.cj3)) return fail("bad entry code");
    }
    for (int i = 0; i < kIndexCount; ++i)
        for (uint32_t k = 0; k < indexCount_[i]; ++k)
            if (index_[i][k].entry >= entryCount_ || !CodeOk(index_[i][k].code))
                return fail("bad index item");
    for (uint32_t k = 0; k < byCharCount_; ++k)
        if (byChar_[k].entry >= entryCount_) return fail("bad char item");
    for (uint32_t k = 0; k < assocCount_; ++k)
        if (assoc_[k].entry >= entryCount_) return fail("bad assoc item");
    if (backoffCount_ != 0 && backoffCount_ != entryCount_ + 1) return fail("bad backoff section");
    for (uint32_t k = 0; k < pairCount_; ++k) {
        const WordPair& p = pairs_[k];
        if (p.prev > entryCount_ || p.next >= entryCount_) return fail("bad word pair");
        if (k > 0 && (pairs_[k - 1].prev > p.prev || (pairs_[k - 1].prev == p.prev && pairs_[k - 1].next >= p.next)))
            return fail("word pairs not sorted");
    }

    header_ = h;
    return true;
}

std::u16string_view Dictionary::Text(const DictEntry& e) const {
    return std::u16string_view(strings_ + e.text, e.textLen);
}

std::u16string_view Dictionary::Simplified(const DictEntry& e) const {
    if (e.simpLen == 0) return {};
    return std::u16string_view(strings_ + e.simp, e.simpLen);
}

Dictionary::IndexRange Dictionary::All(IndexId index) const {
    if (!header_) return {nullptr, nullptr};
    return {index_[index], index_[index] + indexCount_[index]};
}

Dictionary::IndexRange Dictionary::Exact(IndexId index, std::string_view code) const {
    auto [b, e] = All(index);
    if (!b) return {nullptr, nullptr};
    auto lo = std::lower_bound(b, e, code, [](const CodeIndexItem& item, std::string_view c) {
        return CompareCode(item, c) < 0;
    });
    auto hi = std::upper_bound(lo, e, code, [](std::string_view c, const CodeIndexItem& item) {
        return CompareCode(item, c) > 0;
    });
    return {lo, hi};
}

Dictionary::IndexRange Dictionary::Longer(IndexId index, std::string_view prefix) const {
    auto [b, e] = All(index);
    if (!b) return {nullptr, nullptr};
    // Items with exactly `prefix` sort first; skip past them.
    auto lo = std::upper_bound(b, e, prefix, [](std::string_view c, const CodeIndexItem& item) {
        return CompareCode(item, c) > 0;
    });
    auto hi = std::partition_point(lo, e, [&](const CodeIndexItem& item) {
        std::string_view code = CodeView(item.code);
        return code.size() > prefix.size() && code.compare(0, prefix.size(), prefix) == 0;
    });
    return {lo, hi};
}

uint32_t Dictionary::FindCharId(char32_t codePoint) const {
    if (!header_) return UINT32_MAX;
    auto e = byChar_ + byCharCount_;
    auto it = std::lower_bound(byChar_, e, codePoint,
                               [](const CharIndexItem& item, char32_t cp) { return item.codePoint < cp; });
    if (it == e || it->codePoint != codePoint) return UINT32_MAX;
    return it->entry;
}

const DictEntry* Dictionary::FindChar(char32_t codePoint) const {
    uint32_t id = FindCharId(codePoint);
    return id == UINT32_MAX ? nullptr : &entries_[id];
}

bool Dictionary::PairLogProb(uint32_t prev, uint32_t next, float* logProb) const {
    const WordPair* e = pairs_ + pairCount_;
    const WordPair* it = std::lower_bound(pairs_, e, std::make_pair(prev, next),
                                          [](const WordPair& p, const std::pair<uint32_t, uint32_t>& key) {
                                              return p.prev != key.first ? p.prev < key.first : p.next < key.second;
                                          });
    if (it == e || it->prev != prev || it->next != next) return false;
    *logProb = it->logProb;
    return true;
}

float Dictionary::Backoff(uint32_t prev) const { return prev < backoffCount_ ? backoff_[prev] : 0.0f; }

Dictionary::AssocRange Dictionary::Associations(char32_t firstChar) const {
    if (!header_) return {nullptr, nullptr};
    auto e = assoc_ + assocCount_;
    auto lo = std::lower_bound(assoc_, e, firstChar,
                               [](const AssocItem& item, char32_t cp) { return item.firstChar < cp; });
    auto hi = std::upper_bound(lo, e, firstChar,
                               [](char32_t cp, const AssocItem& item) { return cp < item.firstChar; });
    return {lo, hi};
}

}  // namespace nova
