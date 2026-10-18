// NovaChangJieLM: per-user background helper that scores sentences with the
// neural language model (nova-lm.onnx, see tools/train-lm.py) for sentence
// mode. Runs at logon (installed under the Run key), one instance per user.
//
// The model runs here, not in the input method, so apps do not load ONNX
// Runtime or the model. It is loaded on the first request and released
// after kIdleUnloadMs without requests, so the helper stays small when idle.
//
//   NovaChangJieLM.exe            serve until logoff
//   NovaChangJieLM.exe --score    read sentences from stdin, print scores (testing)
#include <windows.h>
#include <sddl.h>

#include <onnxruntime_cxx_api.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "LmClient.h"
#include "LmProtocol.h"
#include "NovaIds.h"
#include "Unicode.h"

using namespace nova;

namespace {

constexpr ULONGLONG kIdleUnloadMs = 10 * 60 * 1000;
constexpr int64_t kPad = 0, kStart = 1, kUnknown = 2;

std::wstring ModuleDir() {
    wchar_t path[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    std::wstring dir(path, n);
    size_t slash = dir.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : dir.substr(0, slash + 1);
}

class Model {
public:
    // log P(sentence) for each sentence; empty if the model cannot be loaded.
    std::vector<float> Score(const std::vector<std::u16string>& sentences) {
        lastUse_ = GetTickCount64();
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            if (session_) return Run(sentences);
            if (failed_) return {};
        }
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (!session_ && !failed_) Load();
        lock.unlock();
        std::shared_lock<std::shared_mutex> shared(mutex_);
        return session_ ? Run(sentences) : std::vector<float>();
    }

    void UnloadIfIdle() {
        if (GetTickCount64() - lastUse_ < kIdleUnloadMs) return;
        std::unique_lock<std::shared_mutex> lock(mutex_);
        session_.reset();
    }

private:
    void Load() {
        const std::wstring dir = ModuleDir();
        try {
            if (!env_) {
                env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_ERROR, "NovaChangJieLM");
                env_->DisableTelemetryEvents();
            }
            if (vocab_.empty() && !LoadVocab(dir + L"nova-lm.vocab")) {
                failed_ = true;
                return;
            }
            Ort::SessionOptions options;
            options.SetIntraOpNumThreads(1);
            options.SetInterOpNumThreads(1);
            options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            session_ = std::make_unique<Ort::Session>(*env_, (dir + L"nova-lm.onnx").c_str(), options);
        } catch (const Ort::Exception&) {
            failed_ = true;
            session_.reset();
        }
    }

    bool LoadVocab(const std::wstring& path) {
        std::ifstream in(path, std::ios::binary);
        std::string line;
        int64_t id = 3;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::vector<char32_t> cps = CodePoints(Utf8ToUtf16(line));
            if (cps.size() == 1) vocab_.emplace(cps[0], id);
            ++id;
        }
        return !vocab_.empty();
    }

    std::vector<float> Run(const std::vector<std::u16string>& sentences) {
        const int64_t batch = static_cast<int64_t>(sentences.size());
        std::vector<std::vector<char32_t>> cps;
        int64_t width = 1;
        for (const std::u16string& s : sentences) {
            cps.push_back(CodePoints(s));
            width = std::max<int64_t>(width, static_cast<int64_t>(cps.back().size()) + 1);
        }
        std::vector<int64_t> ids(static_cast<size_t>(batch * width), kPad), lengths(static_cast<size_t>(batch));
        for (int64_t b = 0; b < batch; ++b) {
            int64_t* row = ids.data() + b * width;
            row[0] = kStart;
            for (size_t t = 0; t < cps[b].size(); ++t) {
                auto it = vocab_.find(cps[b][t]);
                row[t + 1] = it == vocab_.end() ? kUnknown : it->second;
            }
            lengths[b] = static_cast<int64_t>(cps[b].size()) + 1;
        }
        try {
            Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            const int64_t idsShape[] = {batch, width};
            const int64_t lengthsShape[] = {batch};
            Ort::Value inputs[] = {
                Ort::Value::CreateTensor<int64_t>(memory, ids.data(), ids.size(), idsShape, 2),
                Ort::Value::CreateTensor<int64_t>(memory, lengths.data(), lengths.size(), lengthsShape, 1),
            };
            const char* inputNames[] = {"ids", "lengths"};
            const char* outputNames[] = {"score"};
            auto outputs = session_->Run(Ort::RunOptions{nullptr}, inputNames, inputs, 2, outputNames, 1);
            const float* scores = outputs[0].GetTensorData<float>();
            return std::vector<float>(scores, scores + batch);
        } catch (const Ort::Exception&) {
            return {};
        }
    }

    std::shared_mutex mutex_;
    std::unique_ptr<Ort::Env> env_;
    std::unique_ptr<Ort::Session> session_;
    std::unordered_map<char32_t, int64_t> vocab_;
    std::atomic<ULONGLONG> lastUse_{0};
    bool failed_ = false;
};

Model g_model;

bool ParseRequest(const BYTE* data, DWORD size, std::vector<std::u16string>* sentences) {
    auto get32 = [&](DWORD at, uint32_t* v) {
        if (at + 4 > size) return false;
        std::memcpy(v, data + at, 4);
        return true;
    };
    uint32_t magic, count;
    if (!get32(0, &magic) || magic != kLmMagic || !get32(4, &count) || count == 0 || count > kLmMaxSentences)
        return false;
    DWORD at = 8;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t length;
        if (!get32(at, &length) || length > kLmMaxLength || at + 4 + length * 2 > size) return false;
        std::u16string s(length, u'\0');
        std::memcpy(s.data(), data + at + 4, length * 2);
        sentences->push_back(std::move(s));
        at += 4 + length * 2;
    }
    return at == size;
}

DWORD WINAPI ServeClient(void* param) {
    HANDLE pipe = param;
    BYTE request[kLmMaxRequest];
    DWORD read = 0;
    // A client may send several requests on one connection.
    while (ReadFile(pipe, request, sizeof(request), &read, nullptr)) {
        std::vector<std::u16string> sentences;
        std::vector<float> scores;
        if (ParseRequest(request, read, &sentences)) scores = g_model.Score(sentences);
        BYTE reply[kLmMaxReply];
        uint32_t magic = kLmMagic, count = static_cast<uint32_t>(scores.size());
        std::memcpy(reply, &magic, 4);
        std::memcpy(reply + 4, &count, 4);
        std::memcpy(reply + 8, scores.data(), count * 4);
        DWORD written = 0;
        if (!WriteFile(pipe, reply, 8 + count * 4, &written, nullptr)) break;
    }
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    return 0;
}

DWORD WINAPI IdleWatch(void*) {
    for (;;) {
        Sleep(60 * 1000);
        g_model.UnloadIfIdle();
    }
}

int Serve() {
    std::wstring sid;
    if (!CurrentUserSid(&sid, nullptr)) return 1;
    const std::wstring name = std::wstring(kLmPipePrefix) + sid;
    // This user (and SYSTEM) only, plus app containers (Store apps) so their
    // input method can connect; low integrity processes may connect too.
    // The owner is set to the user even when running elevated (which would
    // make it Administrators): clients only trust pipes owned by themselves.
    const std::wstring sddl = L"O:" + sid + L"D:P(A;;GA;;;SY)(A;;GA;;;" + sid +
                              L")(A;;GRGWRC;;;AC)(A;;GRGWRC;;;S-1-15-2-2)S:(ML;;NW;;;LW)";
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) return 1;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), sd, FALSE};

    CloseHandle(CreateThread(nullptr, 0, IdleWatch, nullptr, 0, nullptr));
    bool first = true;
    for (;;) {
        DWORD openMode = PIPE_ACCESS_DUPLEX | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
        HANDLE pipe = CreateNamedPipeW(name.c_str(), openMode,
                                       PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                       PIPE_UNLIMITED_INSTANCES, kLmMaxReply, kLmMaxRequest, 0, &sa);
        if (pipe == INVALID_HANDLE_VALUE) {
            if (first) return 0;  // Already running for this user.
            Sleep(1000);
            continue;
        }
        first = false;
        if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            HANDLE thread = CreateThread(nullptr, 64 * 1024, ServeClient, pipe, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
            if (thread) {
                CloseHandle(thread);
                continue;
            }
        }
        CloseHandle(pipe);
    }
}

// Test mode: one sentence per line on stdin (UTF-8), "score<TAB>sentence" out.
int ScoreStdin() {
    char line[4096];
    while (std::fgets(line, sizeof(line), stdin)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        std::vector<float> scores = g_model.Score({Utf8ToUtf16(s)});
        if (scores.empty()) {
            std::fprintf(stderr, "model not available\n");
            return 1;
        }
        std::printf("%.4f\t%s\n", scores[0], s.c_str());
    }
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmdLine, int) {
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (cmdLine && std::wcscmp(cmdLine, L"--score") == 0) {
        // Use the parent's console unless input and output are redirected.
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        DWORD type = in && in != INVALID_HANDLE_VALUE ? GetFileType(in) : FILE_TYPE_UNKNOWN;
        if ((type == FILE_TYPE_UNKNOWN || type == FILE_TYPE_CHAR) && AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* f = nullptr;
            freopen_s(&f, "CONIN$", "r", stdin);
            freopen_s(&f, "CONOUT$", "w", stdout);
            freopen_s(&f, "CONOUT$", "w", stderr);
        }
        return ScoreStdin();
    }
    return Serve();
}
