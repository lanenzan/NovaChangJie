#include "DictBuilder.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "DictFormat.h"
#include "PhraseCode.h"
#include "Unicode.h"

namespace nova {

namespace {

std::vector<std::string> SplitCodes(const std::string& field) {
    std::vector<std::string> out;
    if (field == "NA") return out;
    std::stringstream ss(field);
    std::string code;
    while (std::getline(ss, code, ',')) {
        bool ok = !code.empty() && code.size() < kCodeBytes;
        for (char c : code) ok = ok && IsCodeLetter(c);
        if (ok) out.push_back(code);
    }
    return out;
}

void AppendUnique(std::vector<std::string>& dst, const std::vector<std::string>& src) {
    for (const auto& s : src)
        if (std::find(dst.begin(), dst.end(), s) == dst.end()) dst.push_back(s);
}

// First code usable for word building, falling back to the first code.
std::string PrimaryCode(const std::vector<std::string>& codes) {
    for (const auto& c : codes)
        if (UsableForPhrase(c)) return c;
    return codes.empty() ? std::string() : codes.front();
}

void CopyCode(char (&dst)[kCodeBytes], const std::string& src) {
    std::memset(dst, 0, kCodeBytes);
    std::memcpy(dst, src.data(), std::min<size_t>(src.size(), kCodeBytes - 1));
}

uint32_t Align4(uint32_t v) { return (v + 3u) & ~3u; }

double UnigramLogProb(uint32_t weight) { return std::log(static_cast<double>(weight) + 1.0) - kLogCorpus; }

constexpr double kPairDiscount = 0.75;  // Absolute discounting of pair counts.

}  // namespace

DictBuilder::CharRec& DictBuilder::CharFor(const std::u16string& text) {
    auto it = charByText_.find(text);
    if (it != charByText_.end()) return chars_[it->second];
    size_t id = chars_.size();
    chars_.push_back(CharRec{text});
    charByText_.emplace(text, id);
    auto cps = CodePoints(text);
    if (cps.size() == 1) charByCodePoint_.emplace(cps[0], id);
    return chars_.back();
}

const DictBuilder::CharRec* DictBuilder::LookupCodePoint(char32_t cp) const {
    auto it = charByCodePoint_.find(cp);
    return it == charByCodePoint_.end() ? nullptr : &chars_[it->second];
}

void DictBuilder::AddChar(const std::u16string& text, const std::vector<std::string>& cj5,
                          const std::vector<std::string>& cj3, uint16_t flags, uint32_t order,
                          uint32_t weight, const std::u16string& simplified) {
    CharRec& rec = CharFor(text);
    AppendUnique(rec.cj5, cj5);
    AppendUnique(rec.cj3, cj3);
    rec.flags |= flags;
    rec.order = std::max(rec.order, order);
    rec.weight = std::max(rec.weight, weight);
    if (rec.simp.empty() && simplified != text) rec.simp = simplified;
}

void DictBuilder::AddPhrase(const std::u16string& text, uint32_t weight) {
    auto it = phraseByText_.find(text);
    if (it != phraseByText_.end()) {
        phrases_[it->second].weight = std::max(phrases_[it->second].weight, weight);
        return;
    }
    phraseByText_.emplace(text, phrases_.size());
    phrases_.push_back(PhraseRec{text, weight});
}

bool DictBuilder::LoadCharTable(std::istream& in, std::string* error) {
    std::string line;
    size_t lineNo = 0, loaded = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::stringstream ss(line);
        std::vector<std::string> f;
        std::string tok;
        while (ss >> tok) f.push_back(tok);
        if (f.size() != 15) {
            if (error) *error = "table line " + std::to_string(lineNo) + ": expected 15 columns";
            return false;
        }
        auto flag = [&](int col) { return f[col] == "1"; };
        uint16_t flags = 0;
        if (flag(2)) flags |= kEntryHan;
        if (flag(3)) flags |= kEntryBig5;
        if (flag(4)) flags |= kEntryHkscs;
        if (flag(5)) flags |= kEntryZhuyin;
        if (flag(7) || flag(8)) flags |= kEntryKana;
        if (flag(9)) flags |= kEntryPunct;
        if (flag(10)) flags |= kEntrySymbol;
        std::vector<std::string> cj3 = SplitCodes(f[11]);
        std::vector<std::string> cj5 = SplitCodes(f[12]);
        if (cj3.empty() && cj5.empty()) continue;  // Short-code-only punctuation.
        uint32_t order = static_cast<uint32_t>(std::strtoul(f[14].c_str(), nullptr, 10));
        std::u16string simp = f[1] == "NA" ? std::u16string() : Utf8ToUtf16(f[1]);
        AddChar(Utf8ToUtf16(f[0]), cj5, cj3, flags, order, 0, simp);
        ++loaded;
    }
    if (loaded == 0) {
        if (error) *error = "table contained no characters";
        return false;
    }
    return true;
}

bool DictBuilder::LoadCorpus(std::istream& in, const BuildOptions& options, std::string* error) {
    std::string line;
    size_t used = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t tab = line.find('\t');
        if (line.empty() || line[0] == '#' || tab == std::string::npos) continue;
        std::u16string word = Utf8ToUtf16(line.substr(0, tab));
        uint32_t weight = static_cast<uint32_t>(std::strtoul(line.c_str() + tab + 1, nullptr, 10));
        auto cps = CodePoints(word);
        if (cps.size() == 1) {
            auto it = charByCodePoint_.find(cps[0]);
            if (it != charByCodePoint_.end()) {
                chars_[it->second].weight = std::max(chars_[it->second].weight, weight);
                ++used;
            }
            continue;
        }
        if (cps.size() > options.maxPhraseChars || weight < options.minPhraseWeight) continue;
        bool allHan = true;
        for (char32_t cp : cps) {
            const CharRec* rec = LookupCodePoint(cp);
            if (!rec || !(rec->flags & kEntryHan)) {
                allHan = false;
                break;
            }
        }
        if (!allHan) continue;
        AddPhrase(word, weight);
        ++used;
    }
    if (used == 0) {
        if (error) *error = "corpus contained no usable words";
        return false;
    }
    return true;
}

bool DictBuilder::LoadExtraWords(std::istream& in, const BuildOptions& options, std::string* error) {
    std::string line;
    size_t added = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t tab = line.find('\t');
        if (line.empty() || line[0] == '#' || tab == std::string::npos) continue;
        std::u16string word = Utf8ToUtf16(line.substr(0, tab));
        uint32_t weight = static_cast<uint32_t>(std::strtoul(line.c_str() + tab + 1, nullptr, 10));
        auto cps = CodePoints(word);
        if (cps.size() < 2 || cps.size() > options.maxPhraseChars || weight < options.minExtraWeight) continue;
        if (phraseByText_.count(word)) continue;
        bool allHan = true;
        for (char32_t cp : cps) {
            const CharRec* rec = LookupCodePoint(cp);
            allHan = allHan && rec && (rec->flags & kEntryHan);
        }
        if (!allHan) continue;
        AddPhrase(word, std::max<uint32_t>(1, static_cast<uint32_t>(weight * options.extraWordScale)));
        ++added;
    }
    if (added == 0) {
        if (error) *error = "word list added no words";
        return false;
    }
    return true;
}

bool DictBuilder::LoadText(std::istream& in, double weight, const BuildOptions& options, std::string* error) {
    // Words sentence mode can produce: characters, and words with a word code.
    std::vector<bool> usable(phrases_.size());
    for (size_t j = 0; j < phrases_.size(); ++j) {
        std::vector<std::string> codes;
        for (char32_t cp : CodePoints(phrases_[j].text)) {
            const CharRec* rec = LookupCodePoint(cp);
            codes.push_back(rec ? PrimaryCode(rec->cj5) : std::string());
        }
        std::vector<std::string_view> views(codes.begin(), codes.end());
        usable[j] = !DerivePhraseCode(views).empty();
    }
    const uint32_t phraseBase = static_cast<uint32_t>(chars_.size());

    std::string line;
    size_t words = 0;
    std::vector<double> best;
    std::vector<uint32_t> from, key;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::u16string text = Utf8ToUtf16(line);
        std::vector<char32_t> cps = CodePoints(text);
        // Split at characters the dictionary does not know.
        size_t runStart = 0;
        for (size_t end = 0; end <= cps.size(); ++end) {
            if (end < cps.size() && LookupCodePoint(cps[end])) continue;
            const size_t n = end - runStart;
            if (n > 0) {
                // Most likely split into words (unigram), as sentence mode does.
                best.assign(n + 1, -1e300);
                from.assign(n + 1, 0);
                key.assign(n + 1, 0);
                best[0] = 0;
                for (size_t i = 0; i < n; ++i) {
                    if (best[i] <= -1e299) continue;
                    std::u16string word;
                    for (size_t len = 1; len <= options.maxPhraseChars && i + len <= n; ++len) {
                        AppendCodePoint(word, cps[runStart + i + len - 1]);
                        uint32_t k;
                        uint32_t w;
                        if (len == 1) {
                            k = static_cast<uint32_t>(charByCodePoint_.at(cps[runStart + i]));
                            w = chars_[k].weight;
                        } else {
                            auto it = phraseByText_.find(word);
                            if (it == phraseByText_.end() || !usable[it->second]) continue;
                            k = phraseBase + static_cast<uint32_t>(it->second);
                            w = phrases_[it->second].weight;
                        }
                        double s = best[i] + UnigramLogProb(w);
                        if (s > best[i + len]) {
                            best[i + len] = s;
                            from[i + len] = static_cast<uint32_t>(i);
                            key[i + len] = k;
                        }
                    }
                }
                std::vector<uint32_t> seq;
                for (size_t p = n; p > 0; p = from[p]) seq.push_back(key[p]);
                uint32_t prev = kStartKey;
                for (auto it = seq.rbegin(); it != seq.rend(); ++it) {
                    pairCounts_[(static_cast<uint64_t>(prev) << 32) | *it] += weight;
                    contextCounts_[prev] += weight;
                    prev = *it;
                    ++words;
                }
            }
            runStart = end + 1;
        }
    }
    if (words == 0) {
        if (error) *error = "text contained no known characters";
        return false;
    }
    return true;
}

std::vector<char> DictBuilder::Build() const { return Build(BuildOptions()); }

std::vector<char> DictBuilder::Build(const BuildOptions& options) const {
    std::u16string pool(1, u'\0');  // Offset 0 is reserved for "no string".
    auto addString = [&](const std::u16string& s) {
        uint32_t off = static_cast<uint32_t>(pool.size());
        pool += s;
        return off;
    };

    std::vector<DictEntry> entries;
    std::vector<CodeIndexItem> idx[kIndexCount];
    std::vector<CharIndexItem> byChar;
    std::vector<AssocItem> assoc;
    entries.reserve(chars_.size() + phrases_.size());

    for (const CharRec& c : chars_) {
        DictEntry e{};
        e.text = addString(c.text);
        e.textLen = static_cast<uint16_t>(c.text.size());
        if (!c.simp.empty()) {
            e.simp = addString(c.simp);
            e.simpLen = static_cast<uint16_t>(c.simp.size());
        }
        e.flags = c.flags;
        e.weight = c.weight;
        e.order = c.order;
        CopyCode(e.cj5, PrimaryCode(c.cj5));
        CopyCode(e.cj3, PrimaryCode(c.cj3));
        uint32_t id = static_cast<uint32_t>(entries.size());
        entries.push_back(e);
        for (const auto& code : c.cj5) {
            CodeIndexItem item{};
            CopyCode(item.code, code);
            item.entry = id;
            idx[kIndexCj5].push_back(item);
        }
        for (const auto& code : c.cj3) {
            CodeIndexItem item{};
            CopyCode(item.code, code);
            item.entry = id;
            idx[kIndexCj3].push_back(item);
        }
        // 速成 (Quick): the first and last radical of each Cangjie code.
        auto addQuick = [&](const std::vector<std::string>& codes, IndexId index) {
            std::vector<std::string> seen;
            for (const auto& code : codes) {
                if (code.empty()) continue;
                std::string quick = code.size() == 1 ? code : std::string{code.front(), code.back()};
                if (std::find(seen.begin(), seen.end(), quick) != seen.end()) continue;
                seen.push_back(quick);
                CodeIndexItem item{};
                CopyCode(item.code, quick);
                item.entry = id;
                idx[index].push_back(item);
            }
        };
        addQuick(c.cj5, kIndexQuick5);
        addQuick(c.cj3, kIndexQuick3);
        auto cps = CodePoints(c.text);
        if (cps.size() == 1) byChar.push_back({static_cast<uint32_t>(cps[0]), id});
    }

    std::vector<uint32_t> phraseIds(phrases_.size(), UINT32_MAX);
    for (const PhraseRec& p : phrases_) {
        auto cps = CodePoints(p.text);
        std::vector<std::string> codes;
        std::u16string simp;
        bool simpDiffers = false, big5 = true, hkscs = false, ok = true;
        for (char32_t cp : cps) {
            const CharRec* rec = LookupCodePoint(cp);
            if (!rec) {
                ok = false;
                break;
            }
            codes.push_back(PrimaryCode(rec->cj5));
            big5 = big5 && (rec->flags & (kEntryBig5 | kEntryHkscs));
            hkscs = hkscs || (rec->flags & kEntryHkscs);
            if (!rec->simp.empty()) {
                simp += rec->simp;
                simpDiffers = true;
            } else {
                simp += rec->text;
            }
        }
        if (!ok) continue;
        std::vector<std::string_view> views(codes.begin(), codes.end());
        std::string code = DerivePhraseCode(views);
        if (code.empty()) continue;

        DictEntry e{};
        e.text = addString(p.text);
        e.textLen = static_cast<uint16_t>(p.text.size());
        if (simpDiffers) {
            e.simp = addString(simp);
            e.simpLen = static_cast<uint16_t>(simp.size());
        }
        e.flags = kEntryPhrase | kEntryHan;
        if (big5) e.flags |= hkscs ? kEntryHkscs : kEntryBig5;
        e.weight = p.weight;
        CopyCode(e.cj5, code);
        uint32_t id = static_cast<uint32_t>(entries.size());
        phraseIds[static_cast<size_t>(&p - phrases_.data())] = id;
        entries.push_back(e);
        CodeIndexItem item{};
        CopyCode(item.code, code);
        item.entry = id;
        idx[kIndexPhrase].push_back(item);
        assoc.push_back({static_cast<uint32_t>(cps[0]), id});
    }

    // Within one code: traditional order, then frequency, then stable id.
    auto rankLess = [&](uint32_t a, uint32_t b) {
        const DictEntry& x = entries[a];
        const DictEntry& y = entries[b];
        if (x.order != y.order) return x.order > y.order;
        if (x.weight != y.weight) return x.weight > y.weight;
        return a < b;
    };
    for (auto& v : idx) {
        std::sort(v.begin(), v.end(), [&](const CodeIndexItem& a, const CodeIndexItem& b) {
            int c = std::strncmp(a.code, b.code, kCodeBytes);
            if (c != 0) return c < 0;
            return rankLess(a.entry, b.entry);
        });
    }
    std::sort(byChar.begin(), byChar.end(), [](const CharIndexItem& a, const CharIndexItem& b) {
        return a.codePoint != b.codePoint ? a.codePoint < b.codePoint : a.entry < b.entry;
    });
    byChar.erase(std::unique(byChar.begin(), byChar.end(),
                             [](const CharIndexItem& a, const CharIndexItem& b) {
                                 return a.codePoint == b.codePoint;
                             }),
                 byChar.end());
    std::sort(assoc.begin(), assoc.end(), [&](const AssocItem& a, const AssocItem& b) {
        if (a.firstChar != b.firstChar) return a.firstChar < b.firstChar;
        return rankLess(a.entry, b.entry);
    });

    // Word pairs: absolute discounting, interpolated with the unigram
    // estimate the engine uses for words without a listed pair.
    std::vector<WordPair> pairs;
    std::vector<float> backoff;
    if (!pairCounts_.empty()) {
        const uint32_t start = static_cast<uint32_t>(entries.size());
        auto entryFor = [&](uint32_t key) -> uint32_t {
            if (key == kStartKey) return start;
            if (key < chars_.size()) return key;
            return phraseIds[key - chars_.size()];
        };
        std::unordered_map<uint32_t, double> distinct;  // Number of different next words.
        for (const auto& [k, c] : pairCounts_) distinct[static_cast<uint32_t>(k >> 32)] += 1;
        backoff.assign(entries.size() + 1, 0.0f);
        std::unordered_map<uint32_t, double> alpha;
        for (const auto& [key, total] : contextCounts_) {
            uint32_t id = entryFor(key);
            if (id == UINT32_MAX) continue;
            double a = kPairDiscount * distinct[key] / total;
            alpha[key] = a;
            backoff[id] = static_cast<float>(std::log(a));
        }
        for (const auto& [k, c] : pairCounts_) {
            if (c < options.minPairCount) continue;
            uint32_t prevKey = static_cast<uint32_t>(k >> 32), nextKey = static_cast<uint32_t>(k);
            uint32_t prev = entryFor(prevKey), next = entryFor(nextKey);
            if (prev == UINT32_MAX || next == UINT32_MAX) continue;
            double uni = std::min(1.0, std::exp(UnigramLogProb(entries[next].weight)));
            double a = alpha[prevKey];
            double p = std::max(c - kPairDiscount, 0.0) / contextCounts_.at(prevKey) + a * uni;
            if (p <= a * uni * 1.05) continue;  // No better than the back-off estimate.
            pairs.push_back({prev, next, static_cast<float>(std::log(p))});
        }
        std::sort(pairs.begin(), pairs.end(), [](const WordPair& a, const WordPair& b) {
            return a.prev != b.prev ? a.prev < b.prev : a.next < b.next;
        });
    }

    // Layout: header | strings | entries | indices | byChar | assoc | pairs | backoff.
    DictHeader h{};
    std::memcpy(h.magic, kDictMagic, 4);
    h.version = kDictVersion;
    uint32_t off = Align4(sizeof(DictHeader));
    h.strings = {off, static_cast<uint32_t>(pool.size())};
    off = Align4(off + static_cast<uint32_t>(pool.size() * sizeof(char16_t)));
    h.entries = {off, static_cast<uint32_t>(entries.size())};
    off += static_cast<uint32_t>(entries.size() * sizeof(DictEntry));
    for (int i = 0; i < kIndexCount; ++i) {
        h.indices[i] = {off, static_cast<uint32_t>(idx[i].size())};
        off += static_cast<uint32_t>(idx[i].size() * sizeof(CodeIndexItem));
    }
    h.byChar = {off, static_cast<uint32_t>(byChar.size())};
    off += static_cast<uint32_t>(byChar.size() * sizeof(CharIndexItem));
    h.assoc = {off, static_cast<uint32_t>(assoc.size())};
    off += static_cast<uint32_t>(assoc.size() * sizeof(AssocItem));
    h.pairs = {off, static_cast<uint32_t>(pairs.size())};
    off += static_cast<uint32_t>(pairs.size() * sizeof(WordPair));
    h.backoff = {off, static_cast<uint32_t>(backoff.size())};
    off += static_cast<uint32_t>(backoff.size() * sizeof(float));
    h.fileSize = off;

    std::vector<char> blob(off, 0);
    auto put = [&](uint32_t at, const void* src, size_t bytes) {
        if (bytes) std::memcpy(blob.data() + at, src, bytes);
    };
    put(0, &h, sizeof(h));
    put(h.strings.offset, pool.data(), pool.size() * sizeof(char16_t));
    put(h.entries.offset, entries.data(), entries.size() * sizeof(DictEntry));
    for (int i = 0; i < kIndexCount; ++i)
        put(h.indices[i].offset, idx[i].data(), idx[i].size() * sizeof(CodeIndexItem));
    put(h.byChar.offset, byChar.data(), byChar.size() * sizeof(CharIndexItem));
    put(h.assoc.offset, assoc.data(), assoc.size() * sizeof(AssocItem));
    put(h.pairs.offset, pairs.data(), pairs.size() * sizeof(WordPair));
    put(h.backoff.offset, backoff.data(), backoff.size() * sizeof(float));
    return blob;
}

}  // namespace nova
