// NovaChangJie - phrase (word) code rule.
//
// A word is typed with at most five keys, taken from the Cangjie codes of
// its characters ("first" = first key, "last" = last key of a character):
//
//   2 characters:  first+last of 1st,  first+last of 2nd        e.g. 倉頡 oiar+grmbc -> orgc
//   3 characters:  first of 1st, first of 2nd, first+last of 3rd
//   4+ characters: first of 1st..4th, last of the final character
//
// Codes beginning with 'x' (difficult-character marker) or 'z' (symbols) are
// never used to build a word code.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace nova {

inline bool UsableForPhrase(std::string_view code) {
    return !code.empty() && code[0] != 'x' && code[0] != 'z';
}

// Returns an empty string when the word cannot be encoded.
inline std::string DerivePhraseCode(const std::vector<std::string_view>& chars) {
    for (std::string_view c : chars)
        if (!UsableForPhrase(c)) return {};
    std::string out;
    const size_t n = chars.size();
    if (n == 2) {
        out += chars[0].front();
        out += chars[0].back();
        out += chars[1].front();
        out += chars[1].back();
    } else if (n == 3) {
        out += chars[0].front();
        out += chars[1].front();
        out += chars[2].front();
        out += chars[2].back();
    } else if (n >= 4) {
        for (size_t i = 0; i < 4; ++i) out += chars[i].front();
        out += chars[n - 1].back();
    }
    return out;
}

}  // namespace nova
