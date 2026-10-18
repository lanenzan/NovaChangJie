// NovaChangJie - client of the NovaChangJieLM helper: sends sentence mode's
// best few conversions and gets neural language model scores back.
//
// Each call connects, asks and disconnects within a short deadline, so a
// missing, busy or stuck helper only means sentence mode keeps its own order.
// If the helper is not running, the client can start it (see SetHelperPath).
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "Engine.h"

namespace nova {

class LmClient : public SentenceScorer {
public:
    explicit LmClient(DWORD timeoutMs = 60) : timeoutMs_(timeoutMs) {}
    bool Score(const std::vector<std::u16string>& sentences, std::vector<float>* logProbs) override;
    // NovaChangJieLM.exe to start when the helper is not running. It is only
    // started from an ordinary process: not elevated, not an app container,
    // medium integrity (so it never runs with more or fewer rights than the user).
    void SetHelperPath(std::wstring path) { helperPath_ = std::move(path); }

private:
    bool Init();
    void StartHelper();

    DWORD timeoutMs_;
    bool initialized_ = false;
    std::wstring pipeName_;
    std::vector<BYTE> userSid_;
    ULONGLONG retryAfter_ = 0;  // Do not try again before this tick count.
    std::wstring helperPath_;
    ULONGLONG startAfter_ = 0;  // Do not start the helper again before this.
};

// The current user's SID (from the process token) as a string and as bytes.
bool CurrentUserSid(std::wstring* text, std::vector<BYTE>* sid);

}  // namespace nova
