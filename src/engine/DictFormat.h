// NovaChangJie - compiled dictionary file format.
//
// The dictionary is a single read-only little-endian blob that is memory
// mapped by every process hosting the input method, so all structures are
// plain-old-data and addressed by offsets from the start of the file.
#pragma once

#include <cstdint>

namespace nova {

constexpr char kDictMagic[4] = {'N', 'C', 'J', 'D'};
constexpr uint32_t kDictVersion = 3;  // 2: adds the 速成 indices. 3: adds word pairs.
constexpr int kCodeBytes = 8;  // Codes are at most 7 ASCII letters + NUL.
// Unigram score shared by the engine and the builder: log P(word) is
// log(weight + 1) - kLogCorpus (rime-essay counts are per ~1.8e8 words).
inline constexpr double kLogCorpus = 19.0;

enum EntryFlags : uint16_t {
    kEntryPhrase = 1 << 0,  // Multi-character word.
    kEntryHan = 1 << 1,     // Chinese character (or a word made only of them).
    kEntryBig5 = 1 << 2,    // Every character exists in Big5.
    kEntryHkscs = 1 << 3,   // At least one character is from HKSCS.
    kEntryPunct = 1 << 4,   // Punctuation mark.
    kEntrySymbol = 1 << 5,  // Miscellaneous symbol.
    kEntryKana = 1 << 6,    // Japanese kana.
    kEntryZhuyin = 1 << 7,  // Bopomofo.
};

enum IndexId : uint32_t {
    kIndexCj5 = 0,     // Single characters by Cangjie 5 code.
    kIndexCj3 = 1,     // Single characters by Cangjie 3 code.
    kIndexPhrase = 2,  // Words by derived phrase code.
    kIndexQuick5 = 3,  // Single characters by 速成 code (first + last of the Cangjie 5 code).
    kIndexQuick3 = 4,  // Same, from the Cangjie 3 code.
    kIndexCount = 5,
};

#pragma pack(push, 4)

struct DictSection {
    uint32_t offset;  // Byte offset from start of file.
    uint32_t count;   // Number of items.
};

struct DictHeader {
    char magic[4];
    uint32_t version;
    uint32_t fileSize;
    uint32_t reserved;
    DictSection strings;               // char16_t pool.
    DictSection entries;               // DictEntry[].
    DictSection indices[kIndexCount];  // CodeIndexItem[], sorted by code then rank.
    DictSection byChar;                // CharIndexItem[], sorted by code point.
    DictSection assoc;                 // AssocItem[], sorted by first char then weight desc.
    // Word pair (bigram) model for sentence mode; both may be empty.
    DictSection pairs;                 // WordPair[], sorted by (prev, next).
    DictSection backoff;               // float[entries.count + 1]: log back-off weight per
                                       // previous word; the last one is the sentence start.
};

struct DictEntry {
    uint32_t text;      // Offset into string pool (char16_t units).
    uint16_t textLen;   // Length in char16_t units.
    uint16_t flags;     // EntryFlags.
    uint32_t simp;      // Simplified form in string pool, or 0 when same.
    uint16_t simpLen;
    uint16_t reserved;
    uint32_t weight;    // Corpus frequency; higher is more common.
    uint32_t order;     // Traditional (Big5-style) order; higher sorts first.
    char cj5[kCodeBytes];  // Primary code (phrase code for words).
    char cj3[kCodeBytes];
};

struct CodeIndexItem {
    char code[kCodeBytes];
    uint32_t entry;
};

struct CharIndexItem {
    uint32_t codePoint;
    uint32_t entry;
};

struct AssocItem {
    uint32_t firstChar;  // First code point of the word.
    uint32_t entry;
};

// log P(next | prev) for a word pair seen in the training text. Pairs that
// are not listed score backoff[prev] + log P(next), the unigram estimate.
// `prev` is an entry id, or entries.count for the start of a sentence.
struct WordPair {
    uint32_t prev;
    uint32_t next;
    float logProb;
};

#pragma pack(pop)

static_assert(sizeof(DictEntry) == 40, "DictEntry layout");
static_assert(sizeof(CodeIndexItem) == 12, "CodeIndexItem layout");
static_assert(sizeof(WordPair) == 12, "WordPair layout");

}  // namespace nova
