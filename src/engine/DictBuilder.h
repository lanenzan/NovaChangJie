// NovaChangJie - compiles source tables into the binary dictionary format.
#pragma once

#include <cstdint>
#include <istream>
#include <string>
#include <unordered_map>
#include <vector>

namespace nova {

struct BuildOptions {
    uint32_t minPhraseWeight = 300;  // Corpus words below this are dropped.
    size_t maxPhraseChars = 8;
    double minPairCount = 2;          // Word pairs seen less often are dropped.
    double extraWordScale = 0.1;      // LoadExtraWords: weight multiplier.
    uint32_t minExtraWeight = 100;    // LoadExtraWords: smaller (unscaled) weights are dropped.
};


class DictBuilder {
public:
    // libcangjie "table.txt" (15 whitespace separated columns).
    bool LoadCharTable(std::istream& in, std::string* error);
    // "word<TAB>weight" corpus such as rime-essay. Single characters update
    // character frequencies; longer Han words become phrases.
    bool LoadCorpus(std::istream& in, const BuildOptions& options, std::string* error);
    // A second "word<TAB>weight" list (e.g. rime-cantonese) that only adds
    // words missing from the dictionary, with weights scaled by
    // options.extraWordScale. Character frequencies are left alone, so it
    // cannot reorder the candidates of a Cangjie code.
    bool LoadExtraWords(std::istream& in, const BuildOptions& options, std::string* error);
    // Running text, one phrase per line (see tools/prepare-corpus.py), for the
    // word pair model. Call after LoadCharTable and LoadCorpus. Each phrase is
    // split into dictionary words the way sentence mode would split it, and
    // every pair of neighbouring words is counted `weight` times.
    bool LoadText(std::istream& in, double weight, const BuildOptions& options, std::string* error);

    // Direct construction, mainly for tests. `flags` uses EntryFlags.
    void AddChar(const std::u16string& text, const std::vector<std::string>& cj5,
                 const std::vector<std::string>& cj3, uint16_t flags, uint32_t order,
                 uint32_t weight = 0, const std::u16string& simplified = {});
    void AddPhrase(const std::u16string& text, uint32_t weight);

    std::vector<char> Build() const;
    std::vector<char> Build(const BuildOptions& options) const;

    size_t CharCount() const { return chars_.size(); }
    size_t PhraseCount() const { return phrases_.size(); }
    size_t PairCount() const { return pairCounts_.size(); }

private:
    struct CharRec {
        std::u16string text;
        std::u16string simp;
        std::vector<std::string> cj5, cj3;
        uint16_t flags = 0;
        uint32_t order = 0;
        uint32_t weight = 0;
    };
    struct PhraseRec {
        std::u16string text;
        uint32_t weight = 0;
    };

    CharRec& CharFor(const std::u16string& text);
    const CharRec* LookupCodePoint(char32_t cp) const;

    std::vector<CharRec> chars_;
    std::unordered_map<std::u16string, size_t> charByText_;
    std::unordered_map<char32_t, size_t> charByCodePoint_;
    std::vector<PhraseRec> phrases_;
    std::unordered_map<std::u16string, size_t> phraseByText_;

    // Word pair counts by word key: chars_ index, or chars_.size() + phrases_
    // index; kStartKey is the start of a phrase.
    static constexpr uint32_t kStartKey = UINT32_MAX;
    std::unordered_map<uint64_t, double> pairCounts_;
    std::unordered_map<uint32_t, double> contextCounts_;
};

}  // namespace nova
