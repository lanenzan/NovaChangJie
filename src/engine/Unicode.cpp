#include "Unicode.h"

namespace nova {

std::u16string Utf8ToUtf16(std::string_view s) {
    std::u16string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp;
        int extra;
        if (c < 0x80) {
            cp = c;
            extra = 0;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            extra = 3;
        } else {
            cp = 0xFFFD;
            extra = 0;
        }
        ++i;
        for (int k = 0; k < extra; ++k, ++i) {
            if (i >= s.size() || (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
                cp = 0xFFFD;
                break;
            }
            cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
        }
        AppendCodePoint(out, cp);
    }
    return out;
}

std::string Utf16ToUtf8(std::u16string_view s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (char32_t cp : CodePoints(s)) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

void AppendCodePoint(std::u16string& out, char32_t cp) {
    if (cp >= 0x10000 && cp <= 0x10FFFF) {
        cp -= 0x10000;
        out += static_cast<char16_t>(0xD800 + (cp >> 10));
        out += static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
    } else {
        out += static_cast<char16_t>(cp);
    }
}

std::vector<char32_t> CodePoints(std::u16string_view s) {
    std::vector<char32_t> out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char16_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 &&
            s[i + 1] <= 0xDFFF) {
            out.push_back(0x10000 + ((static_cast<char32_t>(c) - 0xD800) << 10) +
                          (s[i + 1] - 0xDC00));
            ++i;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

char32_t FirstCodePoint(std::u16string_view s) {
    if (s.empty()) return 0;
    if (s.size() >= 2 && s[0] >= 0xD800 && s[0] <= 0xDBFF && s[1] >= 0xDC00 && s[1] <= 0xDFFF)
        return 0x10000 + ((static_cast<char32_t>(s[0]) - 0xD800) << 10) + (s[1] - 0xDC00);
    return s[0];
}

char32_t LastCodePoint(std::u16string_view s) {
    if (s.empty()) return 0;
    size_t n = s.size();
    if (n >= 2 && s[n - 1] >= 0xDC00 && s[n - 1] <= 0xDFFF && s[n - 2] >= 0xD800 &&
        s[n - 2] <= 0xDBFF)
        return 0x10000 + ((static_cast<char32_t>(s[n - 2]) - 0xD800) << 10) + (s[n - 1] - 0xDC00);
    return s[n - 1];
}

}  // namespace nova
