#include "LmClient.h"

#include <aclapi.h>
#include <sddl.h>

#include <cstring>

#include "LmProtocol.h"
#include "NovaIds.h"

namespace nova {

namespace {

constexpr ULONGLONG kRetryMissingMs = 3000;  // Helper not running.
constexpr ULONGLONG kRetryFailedMs = 1000;   // Timeout or bad reply.
constexpr ULONGLONG kRetryForeignMs = 60000;  // Pipe owned by someone else.
constexpr ULONGLONG kRestartMs = 60000;  // Between attempts to start the helper.

class Handle {
public:
    explicit Handle(HANDLE h = nullptr) : h_(h) {}
    ~Handle() {
        if (h_ && h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return h_; }
    bool valid() const { return h_ && h_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE h_;
};

// Overlapped read or write that gives up at `deadline` (GetTickCount64).
bool Transfer(HANDLE pipe, bool write, void* data, DWORD size, DWORD* done, ULONGLONG deadline) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid()) return false;
    OVERLAPPED ov = {};
    ov.hEvent = event.get();
    BOOL ok = write ? WriteFile(pipe, data, size, nullptr, &ov) : ReadFile(pipe, data, size, nullptr, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING) return false;
    ULONGLONG now = GetTickCount64();
    DWORD wait = now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
    if (WaitForSingleObject(ov.hEvent, wait) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &ov);
        GetOverlappedResult(pipe, &ov, done, TRUE);  // Wait for the cancellation.
        return false;
    }
    return GetOverlappedResult(pipe, &ov, done, FALSE) != FALSE;
}

// Whether this process runs with the user's ordinary rights.
bool OrdinaryProcess() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    Handle tokenHandle(token);
    DWORD size = 0, appContainer = 0;
    TOKEN_ELEVATION_TYPE elevation = TokenElevationTypeDefault;
    if (!GetTokenInformation(token, TokenIsAppContainer, &appContainer, sizeof(appContainer), &size) || appContainer)
        return false;
    if (!GetTokenInformation(token, TokenElevationType, &elevation, sizeof(elevation), &size) ||
        elevation == TokenElevationTypeFull)
        return false;
    alignas(TOKEN_MANDATORY_LABEL) BYTE buffer[64];
    if (!GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer), &size)) return false;
    PSID label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer)->Label.Sid;
    DWORD rid = *GetSidSubAuthority(label, *GetSidSubAuthorityCount(label) - 1);
    return rid == SECURITY_MANDATORY_MEDIUM_RID;
}

}  // namespace

bool CurrentUserSid(std::wstring* text, std::vector<BYTE>* sid) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    Handle tokenHandle(token);
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    if (size == 0 || !GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) return false;
    PSID user = reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid;
    wchar_t* str = nullptr;
    if (!ConvertSidToStringSidW(user, &str)) return false;
    if (text) *text = str;
    LocalFree(str);
    if (sid) sid->assign(static_cast<BYTE*>(user), static_cast<BYTE*>(user) + GetLengthSid(user));
    return true;
}

bool LmClient::Init() {
    if (initialized_) return !pipeName_.empty();
    initialized_ = true;
    std::wstring sid;
    if (!CurrentUserSid(&sid, &userSid_)) return false;
    pipeName_ = std::wstring(kLmPipePrefix) + sid;
    return true;
}

void LmClient::StartHelper() {
    ULONGLONG now = GetTickCount64();
    if (helperPath_.empty() || now < startAfter_) return;
    startAfter_ = now + kRestartMs;
    if (GetFileAttributesW(helperPath_.c_str()) == INVALID_FILE_ATTRIBUTES || !OrdinaryProcess()) return;
    std::wstring dir = helperPath_.substr(0, helperPath_.find_last_of(L'\\') + 1);
    std::wstring cmd = L"\"" + helperPath_ + L"\"";
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    // Out of the app's job if allowed, so closing the app does not end it.
    for (DWORD flags : {DWORD(CREATE_BREAKAWAY_FROM_JOB | CREATE_NEW_PROCESS_GROUP), DWORD(CREATE_NEW_PROCESS_GROUP)}) {
        if (CreateProcessW(helperPath_.c_str(), cmd.data(), nullptr, nullptr, FALSE, flags, nullptr, dir.c_str(), &si,
                           &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return;
        }
    }
}

bool LmClient::Score(const std::vector<std::u16string>& sentences, std::vector<float>* logProbs) {
    if (sentences.empty() || sentences.size() > kLmMaxSentences || !Init()) return false;
    ULONGLONG now = GetTickCount64();
    if (now < retryAfter_) return false;
    const ULONGLONG deadline = now + timeoutMs_;

    // Identification level only: a pipe server cannot act as this user.
    const DWORD flags = FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION;
    const DWORD access = GENERIC_READ | GENERIC_WRITE | READ_CONTROL;
    HANDLE raw = CreateFileW(pipeName_.c_str(), access, 0, nullptr, OPEN_EXISTING, flags, nullptr);
    if (raw == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY && WaitNamedPipeW(pipeName_.c_str(), 20))
        raw = CreateFileW(pipeName_.c_str(), access, 0, nullptr, OPEN_EXISTING, flags, nullptr);
    Handle pipe(raw);
    if (!pipe.valid()) {
        retryAfter_ = now + kRetryMissingMs;
        if (GetLastError() == ERROR_FILE_NOT_FOUND) StartHelper();
        return false;
    }

    // Only talk to a helper started by this user: anyone can create a pipe
    // with this name, but not one owned by our SID.
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    bool ours = GetSecurityInfo(pipe.get(), SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr,
                                nullptr, &sd) == ERROR_SUCCESS &&
                owner && EqualSid(owner, userSid_.data());
    if (sd) LocalFree(sd);
    if (!ours) {
        retryAfter_ = now + kRetryForeignMs;
        return false;
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr);

    std::vector<BYTE> request;
    auto put32 = [&](uint32_t v) {
        const BYTE* p = reinterpret_cast<const BYTE*>(&v);
        request.insert(request.end(), p, p + 4);
    };
    put32(kLmMagic);
    put32(static_cast<uint32_t>(sentences.size()));
    for (const std::u16string& s : sentences) {
        uint32_t length = static_cast<uint32_t>(std::min<size_t>(s.size(), kLmMaxLength));
        put32(length);
        const BYTE* p = reinterpret_cast<const BYTE*>(s.data());
        request.insert(request.end(), p, p + length * sizeof(char16_t));
    }

    BYTE reply[kLmMaxReply];
    DWORD done = 0;
    bool ok = Transfer(pipe.get(), true, request.data(), static_cast<DWORD>(request.size()), &done, deadline) &&
              done == request.size() && Transfer(pipe.get(), false, reply, sizeof(reply), &done, deadline);
    uint32_t magic = 0, count = 0;
    if (ok && done >= 8) {
        std::memcpy(&magic, reply, 4);
        std::memcpy(&count, reply + 4, 4);
    }
    if (!ok || magic != kLmMagic || count != sentences.size() || done != 8 + count * 4) {
        retryAfter_ = now + (ok ? kRetryMissingMs : kRetryFailedMs);
        return false;
    }
    logProbs->resize(count);
    std::memcpy(logProbs->data(), reply + 8, count * 4);
    return true;
}

}  // namespace nova
