// NovaChangJie text service - data shared by every thread in a process:
// the memory-mapped dictionary and the user's adaptive data.
#pragma once

#include "Globals.h"

#include <mutex>

#include "Dictionary.h"
#include "UserModel.h"

namespace nova {

class SharedData {
public:
    static SharedData& Get();

    // nullptr when nova.dict is missing or invalid.
    const Dictionary* Dict();
    UserModel& User() { return user_; }

    // Saves learning data when enough has changed (or always if `force`).
    void SaveHistory(bool force);
    // Reloads user_phrases.txt if it changed on disk.
    void RefreshUserPhrases();
    // Drops in-memory history after "clear learning data" in settings.
    void CheckHistoryEpoch();

    ~SharedData();

private:
    SharedData() = default;
    void LoadDictionary();
    void LoadHistory();

    std::once_flag dictOnce_;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
    const void* view_ = nullptr;
    Dictionary dict_;

    UserModel user_;
    std::mutex ioMutex_;
    FILETIME phrasesTime_ = {};
    DWORD historyEpoch_ = 0;
};

}  // namespace nova
