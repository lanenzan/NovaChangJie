// NovaChangJie - read-only view over a compiled dictionary blob.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "DictFormat.h"

namespace nova {

// Returns true if `code` (NUL-padded, at most kCodeBytes) matches `pattern`,
// where '*' matches any run of letters (including none).
bool WildcardMatch(std::string_view pattern, std::string_view code);

std::string_view CodeView(const char (&code)[kCodeBytes]);

class Dictionary {
public:
    using IndexRange = std::pair<const CodeIndexItem*, const CodeIndexItem*>;
    using AssocRange = std::pair<const AssocItem*, const AssocItem*>;

    // `data` must stay valid for the lifetime of the Dictionary. Validates
    // every offset so a corrupt file can never cause out-of-bounds reads.
    bool Load(const void* data, size_t size, std::string* error = nullptr);
    bool IsLoaded() const { return header_ != nullptr; }

    uint32_t EntryCount() const { return entryCount_; }
    const DictEntry& Entry(uint32_t id) const { return entries_[id]; }
    std::u16string_view Text(const DictEntry& e) const;
    std::u16string_view Simplified(const DictEntry& e) const;  // Empty if same.

    // All items whose code equals `code`.
    IndexRange Exact(IndexId index, std::string_view code) const;
    // All items whose code starts with `prefix` and is strictly longer.
    IndexRange Longer(IndexId index, std::string_view prefix) const;
    // Whole index, for wildcard scans.
    IndexRange All(IndexId index) const;

    const DictEntry* FindChar(char32_t codePoint) const;
    uint32_t FindCharId(char32_t codePoint) const;  // UINT32_MAX if missing.
    AssocRange Associations(char32_t firstChar) const;

    // Word pair model (empty in dictionaries built without training text).
    bool HasWordPairs() const { return pairCount_ != 0; }
    uint32_t SentenceStartId() const { return entryCount_; }
    uint32_t EntryId(const DictEntry& e) const { return static_cast<uint32_t>(&e - entries_); }
    // Whether the pair was seen in the training text; if so, sets *logProb
    // to log P(next | prev).
    bool PairLogProb(uint32_t prev, uint32_t next, float* logProb) const;
    float Backoff(uint32_t prev) const;  // 0 (no change) when unknown.

private:
    const char* base_ = nullptr;
    const DictHeader* header_ = nullptr;
    const char16_t* strings_ = nullptr;
    uint32_t stringCount_ = 0;
    const DictEntry* entries_ = nullptr;
    uint32_t entryCount_ = 0;
    const CodeIndexItem* index_[kIndexCount] = {};
    uint32_t indexCount_[kIndexCount] = {};
    const CharIndexItem* byChar_ = nullptr;
    uint32_t byCharCount_ = 0;
    const AssocItem* assoc_ = nullptr;
    uint32_t assocCount_ = 0;
    const WordPair* pairs_ = nullptr;
    uint32_t pairCount_ = 0;
    const float* backoff_ = nullptr;
    uint32_t backoffCount_ = 0;
};

}  // namespace nova
