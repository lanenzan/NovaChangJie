// NovaChangJie - per-user adaptive data: usage history (for learning) and
// user-defined words (自建詞). Shared by every thread in a process.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Dictionary.h"

namespace nova {

struct UserPhrase {
    std::u16string text;
    std::string code5;  // Code used with Cangjie 5.
    std::string code3;  // Code used with Cangjie 3.
    std::string shortcut;  // Optional extra code (捷徑), typed with either version.
};

class UserModel {
public:
    struct Usage {
        uint32_t count = 0;
        int64_t lastUsed = 0;  // Seconds since the Unix epoch.
    };

    static constexpr size_t kMaxHistory = 20000;

    // `text` is a word, or two words separated by a space (a word pair typed
    // in sentence mode).
    void Record(std::u16string_view text, int64_t now);
    bool Lookup(std::u16string_view text, Usage* out) const;
    void ClearHistory();

    // History file: UTF-8 lines "count<TAB>lastUsed<TAB>text".
    std::string SerializeHistory() const;
    // Merges another history (taking the max of each field), e.g. the file
    // written by another process since we loaded it.
    void MergeHistory(std::string_view utf8);

    // Number of Record() calls since the last MarkSaved().
    uint32_t UnsavedChanges() const;
    void MarkSaved();

    // User words file: UTF-8 lines "word [shortcut] [@5:c,c,..] [@3:c,c,..]";
    // '#' starts a comment. The word code rule always applies, using each
    // character's primary code unless an @5/@3 list picks another one.
    static std::vector<UserPhrase> ParseUserPhrases(std::string_view utf8, const Dictionary& dict);
    void SetUserPhrases(std::vector<UserPhrase> phrases);
    std::vector<UserPhrase> UserPhrasesForCode(std::string_view code, bool cangjie3) const;
    std::vector<UserPhrase> AllUserPhrases() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::u16string, Usage> history_;
    uint32_t unsaved_ = 0;
    std::vector<UserPhrase> phrases_;
};

}  // namespace nova
