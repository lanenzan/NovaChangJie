#include "SettingsStore.h"

#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>

#include <algorithm>
#include <type_traits>

namespace nova {

namespace {

DWORD ReadDword(HKEY key, const wchar_t* name, DWORD fallback) {
    DWORD value = 0, size = sizeof(value), type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) ==
            ERROR_SUCCESS &&
        type == REG_DWORD)
        return value;
    return fallback;
}

void WriteDword(HKEY key, const wchar_t* name, DWORD value) {
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

template <typename Fn>
void ForEachValue(Options& o, Fn&& fn) {
    auto e = [&](const wchar_t* name, auto& field) {
        DWORD v = static_cast<DWORD>(field);
        fn(name, v);
        using T = std::decay_t<decltype(field)>;
        field = static_cast<T>(v);
    };
    auto b = [&](const wchar_t* name, bool& field) {
        DWORD v = field ? 1 : 0;
        fn(name, v);
        field = v != 0;
    };
    e(L"CangjieVersion", o.version);
    e(L"Charset", o.charset);
    e(L"CandidateOrder", o.order);
    e(L"CandidateLayout", o.layout);
    e(L"PageSize", o.pageSize);
    e(L"FontSize", o.fontSize);
    b(L"SentenceMode", o.sentenceMode);
    b(L"CodeCandidates", o.codeCandidates);
    b(L"Association", o.association);
    b(L"Prediction", o.prediction);
    b(L"AutoCommitUnique", o.autoCommitUnique);
    b(L"AutoCommitOnOverflow", o.autoCommitOnOverflow);
    b(L"Learning", o.learning);
    b(L"ShowLetters", o.showLetters);
    b(L"OutputSimplified", o.outputSimplified);
    b(L"ShiftToggles", o.shiftToggles);
    b(L"StartInChinese", o.startInChinese);
    b(L"StartFullShape", o.startFullShape);
    b(L"ChinesePunctuation", o.chinesePunctuation);
    b(L"ShowCodeHints", o.showCodeHints);
    b(L"PrefixHints", o.prefixHints);
    b(L"WordPairs", o.wordPairs);
    b(L"NeuralRerank", o.neuralRerank);
}

}  // namespace

Options LoadOptions() {
    Options o;
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return o;
    ForEachValue(o, [&](const wchar_t* name, DWORD& v) { v = ReadDword(key, name, v); });
    RegCloseKey(key);
    // Clamp anything a hand-edited registry could break.
    if (static_cast<uint32_t>(o.version) > 2) o.version = CangjieVersion::V5;
    if (static_cast<uint32_t>(o.charset) > 2) o.charset = Charset::All;
    if (static_cast<uint32_t>(o.order) > 1) o.order = CandidateOrder::Frequency;
    if (static_cast<uint32_t>(o.layout) > 1) o.layout = CandidateLayout::Vertical;
    o.pageSize = std::clamp<uint32_t>(o.pageSize, 3, 9);
    o.fontSize = std::clamp<uint32_t>(o.fontSize, 10, 36);
    return o;
}

bool SaveOptions(const Options& options) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_READ | KEY_WRITE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    Options o = options;
    ForEachValue(o, [&](const wchar_t* name, DWORD& v) { WriteDword(key, name, v); });
    WriteDword(key, L"Revision", ReadDword(key, L"Revision", 0) + 1);
    RegCloseKey(key);
    return true;
}

namespace {

DWORD ReadSettingsDword(const wchar_t* name) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return 0;
    DWORD value = ReadDword(key, name, 0);
    RegCloseKey(key);
    return value;
}

constexpr char kPhrasesTemplate[] =
    "\xEF\xBB\xBF# NovaChangJie \xE8\x87\xAA\xE5\xBB\xBA\xE8\xA9\x9E (user words)\r\n"
    "#\r\n"
    "# One word per line. Optionally follow it with a space and your own code:\r\n"
    "#   \xE6\x96\xB0\xE5\x80\x89\xE9\xA0\xA1\r\n"
    "#   \xE9\xA6\x99\xE6\xB8\xAF\xE7\x89\xB9\xE5\x88\xA5\xE8\xA1\x8C\xE6\x94\xBF\xE5\x8D\x80 hksar\r\n"
    "# Without a code, the word code rule is used (e.g. 2 characters: first and\r\n"
    "# last key of each character).\r\n";

}  // namespace

DWORD OptionsRevision() { return ReadSettingsDword(L"Revision"); }

DWORD HistoryEpoch() { return ReadSettingsDword(L"HistoryEpoch"); }

void BumpHistoryEpoch() {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        return;
    WriteDword(key, L"HistoryEpoch", ReadDword(key, L"HistoryEpoch", 0) + 1);
    WriteDword(key, L"Revision", ReadDword(key, L"Revision", 0) + 1);
    RegCloseKey(key);
}

std::wstring UserPhrasesFile(bool create) {
    std::wstring dir = UserDataDirectory(create);
    if (dir.empty()) return {};
    std::wstring path = dir + L"user_phrases.txt";
    if (create && GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        WriteFileUtf8(path, kPhrasesTemplate);
    return path;
}

std::wstring UserDataDirectory(bool create) {
    PWSTR base = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &base))) return {};
    std::wstring dir = std::wstring(base) + L"\\NovaChangJie\\";
    CoTaskMemFree(base);
    DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        if (!create || !CreateDirectoryW(dir.c_str(), nullptr)) return {};
    } else if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        return {};
    }
    return dir;
}

void GrantAppContainerRead() {
    PSID sid = nullptr;
    if (!ConvertStringSidToSidW(L"S-1-15-2-1", &sid)) return;  // ALL APPLICATION PACKAGES

    auto grant = [&](const wchar_t* object, SE_OBJECT_TYPE type, DWORD rights) {
        PACL oldDacl = nullptr, newDacl = nullptr;
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (GetNamedSecurityInfoW(object, type, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                  &oldDacl, nullptr, &sd) != ERROR_SUCCESS)
            return;
        EXPLICIT_ACCESSW ea = {};
        ea.grfAccessPermissions = rights;
        ea.grfAccessMode = GRANT_ACCESS;
        ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        ea.Trustee.ptstrName = static_cast<LPWSTR>(sid);
        if (SetEntriesInAclW(1, &ea, oldDacl, &newDacl) == ERROR_SUCCESS) {
            SetNamedSecurityInfoW(const_cast<LPWSTR>(object), type, DACL_SECURITY_INFORMATION,
                                  nullptr, nullptr, newDacl, nullptr);
            LocalFree(newDacl);
        }
        LocalFree(sd);
    };

    std::wstring regPath = std::wstring(L"CURRENT_USER\\") + kSettingsKey;
    grant(regPath.c_str(), SE_REGISTRY_KEY, KEY_READ);
    std::wstring dir = UserDataDirectory(true);
    if (!dir.empty()) {
        dir.pop_back();  // No trailing backslash for the security API.
        grant(dir.c_str(), SE_FILE_OBJECT, GENERIC_READ | GENERIC_EXECUTE);
    }
    LocalFree(sid);
}

bool ReadFileUtf8(const std::wstring& path, std::string& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart < 64 * 1024 * 1024;
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = out.empty() || (ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) &&
                             read == out.size());
    }
    CloseHandle(f);
    return ok;
}

bool WriteFileUtf8(const std::wstring& path, const std::string& data) {
    std::wstring tmp = path + L".tmp" + std::to_wstring(GetCurrentProcessId());
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
              written == data.size();
    CloseHandle(f);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

}  // namespace nova
