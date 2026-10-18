#include "SharedData.h"

#include "SettingsStore.h"

namespace nova {

namespace {

constexpr uint32_t kSaveEvery = 20;  // Learned commits between automatic saves.

}  // namespace

SharedData& SharedData::Get() {
    static SharedData instance;
    return instance;
}

SharedData::~SharedData() {
    if (view_) UnmapViewOfFile(view_);
    if (mapping_) CloseHandle(mapping_);
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
}

void SharedData::LoadDictionary() {
    std::wstring path = FindInstalledFile(L"nova.dict");
    if (path.empty()) return;
    // FILE_SHARE_DELETE lets an upgrade move the in-use file aside and put
    // the new one in place; without it the installer defers both to a
    // restart, and the deferred delete removes the new dictionary.
    file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file_, &size) || size.QuadPart == 0 || size.QuadPart > 512ll * 1024 * 1024) return;
    mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping_) return;
    view_ = MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0);
    if (!view_) return;
    if (!dict_.Load(view_, static_cast<size_t>(size.QuadPart))) {
        OutputDebugStringW(L"NovaChangJie: nova.dict is invalid\n");
    }
}

const Dictionary* SharedData::Dict() {
    std::call_once(dictOnce_, [this] {
        LoadDictionary();
        if (dict_.IsLoaded()) {
            LoadHistory();
            RefreshUserPhrases();
        }
    });
    return dict_.IsLoaded() ? &dict_ : nullptr;
}

void SharedData::LoadHistory() {
    historyEpoch_ = HistoryEpoch();
    std::wstring dir = UserDataDirectory(false);
    if (dir.empty()) return;
    std::string data;
    if (ReadFileUtf8(dir + L"history.txt", data)) user_.MergeHistory(data);
    user_.MarkSaved();
}

void SharedData::SaveHistory(bool force) {
    if (!dict_.IsLoaded()) return;
    if (!force && user_.UnsavedChanges() < kSaveEvery) return;
    if (user_.UnsavedChanges() == 0) return;
    std::lock_guard<std::mutex> lock(ioMutex_);
    std::wstring dir = UserDataDirectory(true);
    if (dir.empty()) return;  // Sandboxed: keep learning in memory only.
    std::wstring path = dir + L"history.txt";
    // Merge what other processes saved meanwhile, then write the union.
    std::string existing;
    if (ReadFileUtf8(path, existing)) user_.MergeHistory(existing);
    if (WriteFileUtf8(path, user_.SerializeHistory())) user_.MarkSaved();
}

void SharedData::CheckHistoryEpoch() {
    DWORD epoch = HistoryEpoch();
    std::lock_guard<std::mutex> lock(ioMutex_);
    if (epoch == historyEpoch_) return;
    historyEpoch_ = epoch;
    // Cleared or edited elsewhere (settings, user words tool): start over
    // from the file, which no longer has the removed entries.
    user_.ClearHistory();
    std::wstring dir = UserDataDirectory(false);
    std::string data;
    if (!dir.empty() && ReadFileUtf8(dir + L"history.txt", data)) user_.MergeHistory(data);
    user_.MarkSaved();
}

void SharedData::RefreshUserPhrases() {
    if (!dict_.IsLoaded()) return;
    std::wstring path = UserPhrasesFile(false);
    if (path.empty()) return;
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return;
    {
        std::lock_guard<std::mutex> lock(ioMutex_);
        if (CompareFileTime(&info.ftLastWriteTime, &phrasesTime_) == 0) return;
        phrasesTime_ = info.ftLastWriteTime;
    }
    std::string data;
    if (ReadFileUtf8(path, data)) user_.SetUserPhrases(UserModel::ParseUserPhrases(data, dict_));
}

}  // namespace nova
