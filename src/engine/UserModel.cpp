#include "UserModel.h"

#include <algorithm>
#include <cstdlib>

#include "PhraseCode.h"
#include "Unicode.h"

namespace nova {

namespace {

std::vector<std::string_view> SplitLines(std::string_view s) {
    std::vector<std::string_view> lines;
    size_t start = 0;
    while (start <= s.size()) {
        size_t nl = s.find('\n', start);
        if (nl == std::string_view::npos) nl = s.size();
        std::string_view line = s.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        lines.push_back(line);
        start = nl + 1;
    }
    return lines;
}

bool IsSpace(char c) { return c == ' ' || c == '\t'; }

std::string_view Trim(std::string_view s) {
    while (!s.empty() && IsSpace(s.front())) s.remove_prefix(1);
    while (!s.empty() && IsSpace(s.back())) s.remove_suffix(1);
    return s;
}

bool IsCode(std::string_view s) {
    if (s.empty() || s.size() >= static_cast<size_t>(kCodeBytes)) return false;
    for (char c : s)
        if (!IsCodeLetter(c)) return false;
    return true;
}

std::vector<std::string_view> SplitTokens(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    size_t start = 0;
    while (start < s.size()) {
        size_t end = start;
        while (end < s.size() && s[end] != sep && !(sep == ' ' && IsSpace(s[end]))) ++end;
        if (end > start) out.push_back(s.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

// `chosen` optionally gives each character's code (empty = its primary code).
std::string DeriveFor(std::u16string_view word, const Dictionary& dict, bool cangjie3,
                      const std::vector<std::string_view>& chosen = {}) {
    std::vector<std::string_view> codes;
    size_t i = 0;
    for (char32_t cp : CodePoints(word)) {
        if (i < chosen.size() && IsCode(chosen[i])) {
            codes.push_back(chosen[i]);
        } else {
            const DictEntry* e = dict.FindChar(cp);
            if (!e) return {};
            codes.push_back(CodeView(cangjie3 ? e->cj3 : e->cj5));
        }
        ++i;
    }
    if (codes.empty()) return {};
    if (codes.size() == 1) return std::string(codes[0]);
    return DerivePhraseCode(codes);
}

}  // namespace

void UserModel::Record(std::u16string_view text, int64_t now) {
    if (text.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    Usage& u = history_[std::u16string(text)];
    ++u.count;
    u.lastUsed = std::max(u.lastUsed, now);
    ++unsaved_;
}

bool UserModel::Lookup(std::u16string_view text, Usage* out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = history_.find(std::u16string(text));
    if (it == history_.end()) return false;
    if (out) *out = it->second;
    return true;
}

void UserModel::ClearHistory() {
    std::lock_guard<std::mutex> lock(mutex_);
    history_.clear();
    unsaved_ = 0;
}

std::string UserModel::SerializeHistory() const {
    std::vector<std::pair<std::u16string, Usage>> items;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        items.assign(history_.begin(), history_.end());
    }
    // Keep the most recently used entries when the history grows too large.
    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
        return a.second.lastUsed > b.second.lastUsed;
    });
    if (items.size() > kMaxHistory) items.resize(kMaxHistory);
    std::string out = "# NovaChangJie usage history\n";
    for (const auto& [text, u] : items) {
        out += std::to_string(u.count);
        out += '\t';
        out += std::to_string(u.lastUsed);
        out += '\t';
        out += Utf16ToUtf8(text);
        out += '\n';
    }
    return out;
}

void UserModel::MergeHistory(std::string_view utf8) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::string_view line : SplitLines(utf8)) {
        if (line.empty() || line[0] == '#') continue;
        size_t t1 = line.find('\t');
        size_t t2 = t1 == std::string_view::npos ? t1 : line.find('\t', t1 + 1);
        if (t2 == std::string_view::npos || t2 + 1 >= line.size()) continue;
        std::string countStr(line.substr(0, t1));
        std::string timeStr(line.substr(t1 + 1, t2 - t1 - 1));
        std::u16string text = Utf8ToUtf16(line.substr(t2 + 1));
        Usage& u = history_[text];
        u.count = std::max<uint32_t>(u.count, static_cast<uint32_t>(std::strtoul(countStr.c_str(), nullptr, 10)));
        u.lastUsed = std::max<int64_t>(u.lastUsed, std::strtoll(timeStr.c_str(), nullptr, 10));
    }
}

uint32_t UserModel::UnsavedChanges() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return unsaved_;
}

void UserModel::MarkSaved() {
    std::lock_guard<std::mutex> lock(mutex_);
    unsaved_ = 0;
}

std::vector<UserPhrase> UserModel::ParseUserPhrases(std::string_view utf8, const Dictionary& dict) {
    std::vector<UserPhrase> out;
    if (utf8.size() >= 3 && utf8.substr(0, 3) == "\xEF\xBB\xBF") utf8.remove_prefix(3);
    for (std::string_view line : SplitLines(utf8)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#') continue;
        // "word [shortcut] [@5:code,code,...] [@3:code,code,...]"
        std::vector<std::string_view> tokens = SplitTokens(line, ' ');
        UserPhrase p;
        p.text = Utf8ToUtf16(tokens[0]);
        if (p.text.empty()) continue;
        std::vector<std::string_view> chosen5, chosen3;
        for (size_t i = 1; i < tokens.size(); ++i) {
            std::string_view t = tokens[i];
            if (t.substr(0, 3) == "@5:") chosen5 = SplitTokens(t.substr(3), ',');
            else if (t.substr(0, 3) == "@3:") chosen3 = SplitTokens(t.substr(3), ',');
            else if (IsCode(t)) p.shortcut = std::string(t);
        }
        p.code5 = DeriveFor(p.text, dict, false, chosen5);
        p.code3 = DeriveFor(p.text, dict, true, chosen3);
        if (p.code5.empty() && p.code3.empty() && p.shortcut.empty()) continue;  // Cannot be typed.
        out.push_back(std::move(p));
    }
    return out;
}

void UserModel::SetUserPhrases(std::vector<UserPhrase> phrases) {
    std::lock_guard<std::mutex> lock(mutex_);
    phrases_ = std::move(phrases);
}

std::vector<UserPhrase> UserModel::UserPhrasesForCode(std::string_view code, bool cangjie3) const {
    std::vector<UserPhrase> out;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& p : phrases_) {
        const std::string& c = cangjie3 ? p.code3 : p.code5;
        if ((!c.empty() && c == code) || (!p.shortcut.empty() && p.shortcut == code)) out.push_back(p);
    }
    return out;
}

std::vector<UserPhrase> UserModel::AllUserPhrases() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return phrases_;
}

}  // namespace nova
