// NovaChangJie - small UTF-8 / UTF-16 helpers shared by the engine and tools.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace nova {

std::u16string Utf8ToUtf16(std::string_view s);
std::string Utf16ToUtf8(std::u16string_view s);

// Appends one code point as UTF-16.
void AppendCodePoint(std::u16string& out, char32_t cp);

// Decodes code points from UTF-16. Unpaired surrogates are returned as-is.
std::vector<char32_t> CodePoints(std::u16string_view s);

// First / last code point, or 0 for an empty string.
char32_t FirstCodePoint(std::u16string_view s);
char32_t LastCodePoint(std::u16string_view s);

inline bool IsCodeLetter(char c) { return c >= 'a' && c <= 'z'; }

}  // namespace nova
