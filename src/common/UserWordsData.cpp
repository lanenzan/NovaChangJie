#include "UserWordsData.h"

#include <algorithm>
#include <cstdlib>
#include <unordered_set>

#include "Engine.h"
#include "SettingsStore.h"
#include "Unicode.h"

namespace nova {

namespace {

std::vector<std::string> SplitList(std::string_view s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t comma = s.find(',', start);
        if (comma == std::string_view::npos) comma = s.size();
        out.emplace_back(s.substr(start, comma - start));
        start = comma + 1;
    }
    return out;
}

std::string JoinList(const std::vector<std::string>& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) out += (i ? "," : "") + v[i];
    return out;
}

// Calls fn(line) for every line without its line break.
template <typename Fn>
void ForEachLine(std::string_view data, Fn&& fn) {
    size_t start = 0;
    while (start < data.size()) {
        size_t nl = data.find('\n', start);
        if (nl == std::string_view::npos) nl = data.size();
        std::string_view line = data.substr(start, nl - start);
        start = nl + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        fn(line);
    }
}

// "count<TAB>lastUsed<TAB>text": the text, or empty if malformed.
std::string_view HistoryText(std::string_view line) {
    size_t t1 = line.find('\t'), t2 = t1 == std::string_view::npos ? t1 : line.find('\t', t1 + 1);
    return t2 == std::string_view::npos ? std::string_view() : line.substr(t2 + 1);
}

}  // namespace

bool CodeTable::Load(const std::wstring& dictPath) {
    HANDLE h = CreateFileW(dictPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < 512ll * 1024 * 1024;
    if (ok) {
        data_.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(h, data_.data(), static_cast<DWORD>(data_.size()), &read, nullptr) && read == data_.size();
    }
    CloseHandle(h);
    if (!ok || !dict_.Load(data_.data(), data_.size())) return false;
    Index(kIndexCj5, codes5_);
    Index(kIndexCj3, codes3_);
    return true;
}

const std::vector<std::string>* CodeTable::Codes(char32_t cp, bool cj3) const {
    const auto& map = cj3 ? codes3_ : codes5_;
    auto it = map.find(cp);
    return it == map.end() || it->second.empty() ? nullptr : &it->second;
}

void CodeTable::Index(IndexId id, std::unordered_map<char32_t, std::vector<std::string>>& map) {
    auto [b, e] = dict_.All(id);
    for (auto it = b; it != e; ++it) {
        const DictEntry& entry = dict_.Entry(it->entry);
        if (!(entry.flags & kEntryHan)) continue;
        std::u16string_view text = dict_.Text(entry);
        if (CodePoints(text).size() != 1) continue;
        std::vector<std::string>& codes = map[FirstCodePoint(text)];
        std::string code(CodeView(it->code));
        if (std::find(codes.begin(), codes.end(), code) != codes.end()) continue;
        std::string primary(CodeView(id == kIndexCj3 ? entry.cj3 : entry.cj5));
        if (code == primary) codes.insert(codes.begin(), code); else codes.push_back(code);
    }
}

bool IsCodeText(std::string_view s) {
    if (s.empty() || s.size() >= static_cast<size_t>(kCodeBytes)) return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return c >= 'a' && c <= 'z'; });
}

std::u16string RadicalsOf(std::string_view code) {
    std::u16string out;
    for (char c : code) out += Engine::RadicalFor(c);
    return out;
}

std::u16string WordRadicals(const UserWord& w, const CodeTable& table, bool cj3) {
    std::u16string out;
    const auto& chosen = cj3 ? w.chosen3 : w.chosen5;
    size_t i = 0;
    for (char32_t cp : CodePoints(w.text)) {
        if (!out.empty()) out += u"  ";
        std::string code;
        if (i < chosen.size() && IsCodeText(chosen[i])) code = chosen[i];
        else if (const auto* codes = table.Codes(cp, cj3)) code = codes->front();
        out += code.empty() ? u"？" : RadicalsOf(code);
        ++i;
    }
    return out;
}

std::string WordReading(const UserWord& w, const CodeTable& table, bool cj3) {
    std::string out;
    const auto& chosen = cj3 ? w.chosen3 : w.chosen5;
    size_t i = 0;
    for (char32_t cp : CodePoints(w.text)) {
        if (i < chosen.size() && IsCodeText(chosen[i])) out += chosen[i];
        else if (const auto* codes = table.Codes(cp, cj3)) out += codes->front();
        out += ' ';
        ++i;
    }
    return out;
}

bool IsLearnedWord(std::u16string_view text) {
    std::vector<char32_t> cps = CodePoints(text);
    if (cps.size() < 2) return false;
    return std::all_of(cps.begin(), cps.end(), [](char32_t c) {
        return (c >= 0x3400 && c <= 0x9FFF) || (c >= 0x20000 && c <= 0x323AF) || (c >= 0xF900 && c <= 0xFAFF);
    });
}

std::vector<UserWord> ParseUserWords(std::string_view utf8) {
    std::vector<UserWord> out;
    if (utf8.size() >= 3 && utf8.substr(0, 3) == "\xEF\xBB\xBF") utf8.remove_prefix(3);
    ForEachLine(utf8, [&](std::string_view line) {
        std::vector<std::string_view> tokens;
        size_t i = 0;
        while (i < line.size()) {
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
            size_t j = i;
            while (j < line.size() && line[j] != ' ' && line[j] != '\t') ++j;
            if (j > i) tokens.push_back(line.substr(i, j - i));
            i = j;
        }
        if (tokens.empty() || tokens[0][0] == '#') return;
        UserWord w;
        w.text = Utf8ToUtf16(tokens[0]);
        for (size_t t = 1; t < tokens.size(); ++t) {
            std::string_view tok = tokens[t];
            if (tok.substr(0, 3) == "@5:") w.chosen5 = SplitList(tok.substr(3));
            else if (tok.substr(0, 3) == "@3:") w.chosen3 = SplitList(tok.substr(3));
            else if (IsCodeText(tok)) w.shortcut = std::string(tok);
        }
        if (!w.text.empty()) out.push_back(std::move(w));
    });
    return out;
}

std::string SerializeUserWords(const std::vector<UserWord>& words) {
    std::string out =
        "# NovaChangJie 使用者造詞 (user words). Edited by the 使用者造詞工具.\n"
        "# Each line: word [shortcut] [@5:codes] [@3:codes]\n";
    auto chosen = [](const std::vector<std::string>& v) {
        return std::any_of(v.begin(), v.end(), [](const std::string& c) { return !c.empty(); });
    };
    for (const UserWord& w : words) {
        out += Utf16ToUtf8(w.text);
        if (!w.shortcut.empty()) out += " " + w.shortcut;
        if (chosen(w.chosen5)) out += " @5:" + JoinList(w.chosen5);
        if (chosen(w.chosen3)) out += " @3:" + JoinList(w.chosen3);
        out += "\n";
    }
    return out;
}

std::vector<UserWord> LoadUserWords() {
    std::wstring path = UserPhrasesFile(false);
    std::string data;
    if (path.empty() || !ReadFileUtf8(path, data)) return {};
    return ParseUserWords(data);
}

bool SaveUserWords(const std::vector<UserWord>& words) {
    std::wstring path = UserPhrasesFile(true);
    if (path.empty() || !WriteFileUtf8(path, SerializeUserWords(words))) return false;
    GrantAppContainerRead();
    return true;
}

std::vector<UserWord> LoadLearnedWords() {
    std::vector<UserWord> out;
    std::wstring dir = UserDataDirectory(false);
    std::string data;
    if (dir.empty() || !ReadFileUtf8(dir + L"history.txt", data)) return out;
    ForEachLine(data, [&](std::string_view line) {
        std::string_view text = HistoryText(line);
        if (text.empty()) return;
        UserWord w;
        w.text = Utf8ToUtf16(text);
        w.count = static_cast<uint32_t>(std::strtoul(std::string(line).c_str(), nullptr, 10));
        if (IsLearnedWord(w.text)) out.push_back(std::move(w));
    });
    std::stable_sort(out.begin(), out.end(), [](const UserWord& a, const UserWord& b) { return a.count > b.count; });
    return out;
}

bool RemoveLearnedWords(const std::vector<std::u16string>& words) {
    if (words.empty()) return true;
    std::wstring dir = UserDataDirectory(true);
    std::string data, out;
    if (!dir.empty() && ReadFileUtf8(dir + L"history.txt", data)) {
        std::unordered_set<std::u16string> removed(words.begin(), words.end());
        ForEachLine(data, [&](std::string_view line) {
            std::string_view text = HistoryText(line);
            if (!text.empty() && removed.count(Utf8ToUtf16(text))) return;
            if (!line.empty()) out += std::string(line) + "\n";
        });
        if (!WriteFileUtf8(dir + L"history.txt", out)) return false;
    }
    BumpHistoryEpoch();  // Running text services reload the file.
    GrantAppContainerRead();
    return true;
}

}  // namespace nova
