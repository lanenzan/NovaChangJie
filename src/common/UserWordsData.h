// NovaChangJie - data behind the 使用者造詞工具, shared by the classic Win32
// tool and the WinUI settings app.
//
// 使用者造詞 is %APPDATA%\NovaChangJie\user_phrases.txt, lines
// "word [shortcut] [@5:code,..] [@3:code,..]" (see UserModel.h).
// 常用新詞 are the words of two or more characters in history.txt, which the
// text service learns from what is typed; removing them rewrites the file and
// bumps the history epoch so running text services reload it.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Dictionary.h"

namespace nova {

inline constexpr size_t kMaxCodedChars = 9;  // Longer words need a shortcut.

struct UserWord {
    std::u16string text;
    std::string shortcut;
    std::vector<std::string> chosen5, chosen3;  // Per-character codes; empty = primary.
    uint32_t count = 0;                          // 常用新詞 only.
};

// Each character's Cangjie codes from nova.dict, primary code first.
class CodeTable {
public:
    bool Load(const std::wstring& dictPath);
    bool Loaded() const { return dict_.IsLoaded(); }
    const std::vector<std::string>* Codes(char32_t cp, bool cj3) const;

private:
    void Index(IndexId id, std::unordered_map<char32_t, std::vector<std::string>>& map);

    std::vector<char> data_;
    Dictionary dict_;
    std::unordered_map<char32_t, std::vector<std::string>> codes5_, codes3_;
};

bool IsCodeText(std::string_view s);
// "日月金" for "abc".
std::u16string RadicalsOf(std::string_view code);
// A word's radicals per character ("人戈口  一口月山金"), "？" where a
// character has no code; `chosen` overrides the primary code per character.
std::u16string WordRadicals(const UserWord& w, const CodeTable& table, bool cj3);
// The codes joined with spaces, for sorting by reading.
std::string WordReading(const UserWord& w, const CodeTable& table, bool cj3);
// Whether a learned history entry is a word worth listing (2+ Han characters).
bool IsLearnedWord(std::u16string_view text);

std::vector<UserWord> ParseUserWords(std::string_view utf8);
std::string SerializeUserWords(const std::vector<UserWord>& words);

// user_phrases.txt; empty when missing.
std::vector<UserWord> LoadUserWords();
bool SaveUserWords(const std::vector<UserWord>& words);
// Learned words from history.txt, most used first.
std::vector<UserWord> LoadLearnedWords();
// Removes words from history.txt and tells running text services.
bool RemoveLearnedWords(const std::vector<std::u16string>& words);

}  // namespace nova
