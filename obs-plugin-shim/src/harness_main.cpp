#include "shim_runtime.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <cstdlib>

namespace {

#ifdef _WIN32
constexpr const char* kCmdPipe = R"(\\.\pipe\aegis_cmd_v1)";
constexpr const char* kEvtPipe = R"(\\.\pipe\aegis_evt_v1)";
constexpr std::uint32_t kMaxFrameSize = 64 * 1024;

std::uint64_t NowUnixMs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

std::string NewHarnessId(const char* prefix) {
    static std::atomic<std::uint64_t> seq{1};
    std::ostringstream oss;
    oss << (prefix ? prefix : "h") << "-" << NowUnixMs() << "-" << seq.fetch_add(1);
    return oss.str();
}

void MpWriteU16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(v & 0xff));
}

void MpWriteU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xff));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xff));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(v & 0xff));
}

void MpWriteU64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xff));
    }
}

void MpWriteString(std::vector<std::uint8_t>& out, const std::string& s) {
    const auto len = s.size();
    if (len <= 31) {
        out.push_back(static_cast<std::uint8_t>(0xa0 | len));
    } else if (len <= 0xff) {
        out.push_back(0xd9);
        out.push_back(static_cast<std::uint8_t>(len));
    } else if (len <= 0xffff) {
        out.push_back(0xda);
        MpWriteU16(out, static_cast<std::uint16_t>(len));
    } else {
        out.push_back(0xdb);
        MpWriteU32(out, static_cast<std::uint32_t>(len));
    }
    out.insert(out.end(), s.begin(), s.end());
}

void MpWriteUInt(std::vector<std::uint8_t>& out, std::uint64_t v) {
    if (v <= 0x7f) {
        out.push_back(static_cast<std::uint8_t>(v));
    } else if (v <= 0xff) {
        out.push_back(0xcc);
        out.push_back(static_cast<std::uint8_t>(v));
    } else if (v <= 0xffff) {
        out.push_back(0xcd);
        MpWriteU16(out, static_cast<std::uint16_t>(v));
    } else if (v <= 0xffffffffULL) {
        out.push_back(0xce);
        MpWriteU32(out, static_cast<std::uint32_t>(v));
    } else {
        out.push_back(0xcf);
        MpWriteU64(out, v);
    }
}

void MpWriteStringMapHeader(std::vector<std::uint8_t>& out, std::size_t count) {
    if (count <= 15) {
        out.push_back(static_cast<std::uint8_t>(0x80 | count));
    } else {
        out.push_back(0xde);
        MpWriteU16(out, static_cast<std::uint16_t>(count));
    }
}

std::vector<std::uint8_t> BuildSwitchSceneEnvelope(
    const std::string& request_id,
    const std::string& scene_name,
    const std::string& reason) {
    std::vector<std::uint8_t> out;
    MpWriteStringMapHeader(out, 6);
    MpWriteString(out, "v");
    MpWriteUInt(out, 1);
    MpWriteString(out, "id");
    MpWriteString(out, NewHarnessId("mock"));
    MpWriteString(out, "ts_unix_ms");
    MpWriteUInt(out, NowUnixMs());
    MpWriteString(out, "type");
    MpWriteString(out, "switch_scene");
    MpWriteString(out, "priority");
    MpWriteString(out, "high");
    MpWriteString(out, "payload");
    MpWriteStringMapHeader(out, 3);
    MpWriteString(out, "request_id");
    MpWriteString(out, request_id);
    MpWriteString(out, "scene_name");
    MpWriteString(out, scene_name);
    MpWriteString(out, "reason");
    MpWriteString(out, reason);
    return out;
}

std::vector<std::uint8_t> BuildSwitchSceneEnvelopeCustom(
    bool include_request_id,
    const std::string& request_id,
    bool include_scene_name,
    const std::string& scene_name,
    bool include_reason,
    const std::string& reason) {
    std::size_t payload_fields = 0;
    if (include_request_id) ++payload_fields;
    if (include_scene_name) ++payload_fields;
    if (include_reason) ++payload_fields;

    std::vector<std::uint8_t> out;
    MpWriteStringMapHeader(out, 6);
    MpWriteString(out, "v");
    MpWriteUInt(out, 1);
    MpWriteString(out, "id");
    MpWriteString(out, NewHarnessId("mock"));
    MpWriteString(out, "ts_unix_ms");
    MpWriteUInt(out, NowUnixMs());
    MpWriteString(out, "type");
    MpWriteString(out, "switch_scene");
    MpWriteString(out, "priority");
    MpWriteString(out, "high");
    MpWriteString(out, "payload");
    MpWriteStringMapHeader(out, payload_fields);
    if (include_request_id) {
        MpWriteString(out, "request_id");
        MpWriteString(out, request_id);
    }
    if (include_scene_name) {
        MpWriteString(out, "scene_name");
        MpWriteString(out, scene_name);
    }
    if (include_reason) {
        MpWriteString(out, "reason");
        MpWriteString(out, reason);
    }
    return out;
}

struct MpReader {
    const std::vector<std::uint8_t>& buf;
    std::size_t pos = 0;

    bool ReadByte(std::uint8_t& out) {
        if (pos >= buf.size()) {
            return false;
        }
        out = buf[pos++];
        return true;
    }

    bool PeekByte(std::uint8_t& out) const {
        if (pos >= buf.size()) {
            return false;
        }
        out = buf[pos];
        return true;
    }

    bool ReadN(std::size_t n, const std::uint8_t*& out) {
        if (pos + n > buf.size()) {
            return false;
        }
        out = buf.data() + pos;
        pos += n;
        return true;
    }
};

bool MpReadString(MpReader& r, std::string& out) {
    out.clear();
    std::uint8_t b = 0;
    if (!r.ReadByte(b)) return false;
    std::size_t len = 0;
    if ((b & 0xe0) == 0xa0) {
        len = b & 0x1f;
    } else if (b == 0xd9) {
        std::uint8_t l = 0;
        if (!r.ReadByte(l)) return false;
        len = l;
    } else if (b == 0xda) {
        const std::uint8_t* p = nullptr;
        if (!r.ReadN(2, p)) return false;
        len = (static_cast<std::size_t>(p[0]) << 8) | p[1];
    } else if (b == 0xdb) {
        const std::uint8_t* p = nullptr;
        if (!r.ReadN(4, p)) return false;
        len = (static_cast<std::size_t>(p[0]) << 24) | (static_cast<std::size_t>(p[1]) << 16) |
              (static_cast<std::size_t>(p[2]) << 8) | p[3];
    } else {
        return false;
    }
    const std::uint8_t* p = nullptr;
    if (!r.ReadN(len, p)) return false;
    out.assign(reinterpret_cast<const char*>(p), len);
    return true;
}

bool MpReadBool(MpReader& r, bool& out) {
    std::uint8_t b = 0;
    if (!r.ReadByte(b)) return false;
    if (b == 0xc2) {
        out = false;
        return true;
    }
    if (b == 0xc3) {
        out = true;
        return true;
    }
    return false;
}

bool MpReadUInt(MpReader& r, std::uint64_t& out) {
    std::uint8_t b = 0;
    if (!r.ReadByte(b)) return false;
    if (b <= 0x7f) {
        out = b;
        return true;
    }
    const std::uint8_t* p = nullptr;
    if (b == 0xcc) {
        if (!r.ReadN(1, p)) return false;
        out = p[0];
        return true;
    }
    if (b == 0xcd) {
        if (!r.ReadN(2, p)) return false;
        out = (static_cast<std::uint64_t>(p[0]) << 8) | p[1];
        return true;
    }
    if (b == 0xce) {
        if (!r.ReadN(4, p)) return false;
        out = (static_cast<std::uint64_t>(p[0]) << 24) | (static_cast<std::uint64_t>(p[1]) << 16) |
              (static_cast<std::uint64_t>(p[2]) << 8) | p[3];
        return true;
    }
    if (b == 0xcf) {
        if (!r.ReadN(8, p)) return false;
        out = 0;
        for (int i = 0; i < 8; ++i) out = (out << 8) | p[i];
        return true;
    }
    return false;
}

bool MpReadMapHeader(MpReader& r, std::size_t& count) {
    std::uint8_t b = 0;
    if (!r.ReadByte(b)) return false;
    if ((b & 0xf0) == 0x80) {
        count = b & 0x0f;
        return true;
    }
    const std::uint8_t* p = nullptr;
    if (b == 0xde) {
        if (!r.ReadN(2, p)) return false;
        count = (static_cast<std::size_t>(p[0]) << 8) | p[1];
        return true;
    }
    if (b == 0xdf) {
        if (!r.ReadN(4, p)) return false;
        count = (static_cast<std::size_t>(p[0]) << 24) | (static_cast<std::size_t>(p[1]) << 16) |
                (static_cast<std::size_t>(p[2]) << 8) | p[3];
        return true;
    }
    return false;
}

bool MpSkip(MpReader& r);

bool MpSkip(MpReader& r) {
    std::uint8_t b = 0;
    if (!r.PeekByte(b)) return false;
    if ((b & 0xe0) == 0xa0 || b == 0xd9 || b == 0xda || b == 0xdb) {
        std::string s;
        return MpReadString(r, s);
    }
    if ((b & 0xf0) == 0x80 || b == 0xde || b == 0xdf) {
        std::size_t n = 0;
        if (!MpReadMapHeader(r, n)) return false;
        for (std::size_t i = 0; i < n; ++i) {
            if (!MpSkip(r)) return false;
            if (!MpSkip(r)) return false;
        }
        return true;
    }
    if (b <= 0x7f || b == 0xcc || b == 0xcd || b == 0xce || b == 0xcf) {
        std::uint64_t u = 0;
        return MpReadUInt(r, u);
    }
    if (b == 0xc2 || b == 0xc3) {
        bool v = false;
        return MpReadBool(r, v);
    }
    if (b == 0xc0) {
        std::uint8_t tmp = 0;
        return r.ReadByte(tmp);
    }
    return false;
}

struct SceneSwitchResultInfo {
    bool parsed = false;
    std::string request_id;
    bool ok = false;
    bool has_ok = false;
    bool error_is_nil = false;
    std::string error;
};

SceneSwitchResultInfo TryDecodeSceneSwitchResult(const std::vector<std::uint8_t>& frame) {
    SceneSwitchResultInfo out;
    MpReader r{frame};
    std::size_t n = 0;
    if (!MpReadMapHeader(r, n)) return out;

    std::vector<std::uint8_t> payload_bytes;
    std::string type;
    for (std::size_t i = 0; i < n; ++i) {
        std::string key;
        if (!MpReadString(r, key)) return out;
        if (key == "type") {
            if (!MpReadString(r, type)) return out;
        } else if (key == "payload") {
            const std::size_t start = r.pos;
            if (!MpSkip(r)) return out;
            payload_bytes.assign(frame.begin() + static_cast<std::ptrdiff_t>(start),
                                 frame.begin() + static_cast<std::ptrdiff_t>(r.pos));
        } else {
            if (!MpSkip(r)) return out;
        }
    }

    if (type != "scene_switch_result" || payload_bytes.empty()) {
        return out;
    }

    MpReader pr{payload_bytes};
    std::size_t pn = 0;
    if (!MpReadMapHeader(pr, pn)) return out;
    for (std::size_t i = 0; i < pn; ++i) {
        std::string key;
        if (!MpReadString(pr, key)) return out;
        if (key == "request_id") {
            if (!MpReadString(pr, out.request_id)) return out;
        } else if (key == "ok") {
            if (!MpReadBool(pr, out.ok)) return out;
            out.has_ok = true;
        } else if (key == "error") {
            std::uint8_t b = 0;
            if (!pr.PeekByte(b)) return out;
            if (b == 0xc0) {
                if (!pr.ReadByte(b)) return out;
                out.error_is_nil = true;
            } else {
                if (!MpReadString(pr, out.error)) return out;
            }
        } else {
            if (!MpSkip(pr)) return out;
        }
    }
    out.parsed = true;
    return out;
}

std::string GuessFrameKind(const std::vector<std::uint8_t>& payload) {
    const std::string s(payload.begin(), payload.end());
    if (s.find("hello") != std::string::npos) return "hello";
    if (s.find("request_status") != std::string::npos) return "request_status";
    if (s.find("ping") != std::string::npos) return "ping";
    if (s.find("scene_switch_result") != std::string::npos) return "scene_switch_result";
    if (s.find("obs_shutdown_notice") != std::string::npos) return "obs_shutdown_notice";
    return "unknown";
}

class MockCore {
public:
    ~MockCore() {
        Stop();
    }

    void Start() {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) {
            Log("already running");
            return;
        }
        worker_ = std::thread([this] { WorkerLoop(); });
        Log("started");
    }

    void Stop() {
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false)) {
            return;
        }
        drop_session_requested_.store(true);
        for (int i = 0; i < 10; ++i) {
            NudgeConnectWaiters();
            CloseSessionPipes();
            CancelAllPipeIo();
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        if (worker_.joinable()) {
            CancelSynchronousIo(static_cast<HANDLE>(worker_.native_handle()));
            worker_.join();
        }
        CloseListenerPipes();
        Log("stopped");
    }

    void DropSession() {
        if (!running_.load()) {
            Log("drop ignored (not running)");
            return;
        }
        drop_session_requested_.store(true);
        CloseSessionPipes();
        Log("requested session drop");
    }

    void SendSwitchScene(const std::string& scene_name) {
        if (scene_name.empty()) {
            Log("core-switch ignored empty scene name");
            return;
        }

        HANDLE evt = nullptr;
        {
            std::lock_guard<std::mutex> lock(mu_);
            evt = evt_session_;
        }
        if (!evt) {
            Log("core-switch ignored (no active session)");
            return;
        }

        const std::string request_id = NewHarnessId("switch");
        const std::vector<std::uint8_t> payload =
            BuildSwitchSceneEnvelope(request_id, scene_name, "harness_manual");
        const std::uint32_t len = static_cast<std::uint32_t>(payload.size());
        if (!WriteAll(evt, &len, sizeof(len)) || !WriteAll(evt, payload.data(), len)) {
            Log("core-switch send failed");
            return;
        }
        FlushFileBuffers(evt);

        std::lock_guard<std::mutex> lock(log_mu_);
        std::cout << "[mock-core] tx evt frame kind=switch_scene request_id=" << request_id
                  << " scene=" << scene_name << "\n";
    }

    void SendSwitchSceneMissingSceneName() {
        SendEvtFrame(
            BuildSwitchSceneEnvelopeCustom(
                true,
                NewHarnessId("switch"),
                false,
                "",
                true,
                "harness_missing_scene"),
            "switch_scene(missing_scene_name)");
    }

    void SendSwitchSceneMissingRequestId(const std::string& scene_name) {
        SendEvtFrame(
            BuildSwitchSceneEnvelopeCustom(
                false,
                "",
                true,
                scene_name.empty() ? "DemoScene" : scene_name,
                true,
                "harness_missing_request_id"),
            "switch_scene(missing_request_id)");
    }

    void SendMalformedEvtFrame() {
        // Intentionally invalid/incomplete MessagePack map payload.
        std::vector<std::uint8_t> payload{0x81, 0xa4, 't', 'y', 'p', 'e', 0xaB, 's', 'w', 'i', 't', 'c'};
        SendEvtFrame(payload, "malformed_evt");
    }

private:
    void WorkerLoop() {
        while (running_.load()) {
            if (!CreateListeners()) {
                SleepMs(250);
                continue;
            }

            if (!WaitForClientSession()) {
                CloseListenerPipes();
                continue;
            }

            SessionLoop();
            CloseSessionPipes();
            CloseListenerPipes();
        }
    }

    bool CreateListeners() {
        std::lock_guard<std::mutex> lock(mu_);
        CloseHandleIfValid(cmd_listen_);
        CloseHandleIfValid(evt_listen_);

        SECURITY_DESCRIPTOR sd;
        if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION)) {
            LogWinErr("InitializeSecurityDescriptor");
            return false;
        }
        if (!SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE)) {
            LogWinErr("SetSecurityDescriptorDacl");
            return false;
        }
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = &sd;
        sa.bInheritHandle = FALSE;

        cmd_listen_ = CreateNamedPipeA(
            kCmdPipe,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,
            kMaxFrameSize,
            kMaxFrameSize,
            0,
            &sa);
        if (cmd_listen_ == INVALID_HANDLE_VALUE) {
            cmd_listen_ = nullptr;
            LogWinErr("CreateNamedPipe(cmd)");
            return false;
        }

        evt_listen_ = CreateNamedPipeA(
            kEvtPipe,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,
            kMaxFrameSize,
            kMaxFrameSize,
            0,
            &sa);
        if (evt_listen_ == INVALID_HANDLE_VALUE) {
            evt_listen_ = nullptr;
            CloseHandleIfValid(cmd_listen_);
            LogWinErr("CreateNamedPipe(evt)");
            return false;
        }

        Log("listening on aegis_cmd_v1 + aegis_evt_v1");
        return true;
    }

    bool WaitForClientSession() {
        HANDLE cmd = nullptr;
        HANDLE evt = nullptr;
        {
            std::lock_guard<std::mutex> lock(mu_);
            cmd = cmd_listen_;
            evt = evt_listen_;
        }
        if (!cmd || !evt) {
            return false;
        }

        if (!ConnectPipe(cmd, "cmd")) {
            return false;
        }
        if (!running_.load()) {
            return false;
        }
        if (!ConnectPipe(evt, "evt")) {
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mu_);
            cmd_session_ = cmd_listen_;
            evt_session_ = evt_listen_;
            cmd_listen_ = nullptr;
            evt_listen_ = nullptr;
        }
        drop_session_requested_.store(false);
        Log("client session connected");
        return true;
    }

    bool ConnectPipe(HANDLE pipe, const char* name) {
        BOOL ok = ConnectNamedPipe(pipe, nullptr);
        if (ok) {
            return true;
        }

        DWORD err = GetLastError();
        if (err == ERROR_PIPE_CONNECTED) {
            return true;
        }
        if (!running_.load()) {
            return false;
        }

        std::lock_guard<std::mutex> lock(log_mu_);
        std::cout << "[mock-core] ConnectNamedPipe(" << name << ") failed err=" << err << "\n";
        return false;
    }

    void SessionLoop() {
        for (;;) {
            if (!running_.load()) {
                return;
            }
            if (drop_session_requested_.exchange(false)) {
                Log("dropping active session");
                return;
            }

            HANDLE cmd = nullptr;
            {
                std::lock_guard<std::mutex> lock(mu_);
                cmd = cmd_session_;
            }
            if (!cmd) {
                return;
            }

            DWORD avail = 0;
            if (!PeekNamedPipe(cmd, nullptr, 0, nullptr, &avail, nullptr)) {
                Log("cmd pipe disconnected");
                return;
            }

            if (avail >= sizeof(std::uint32_t)) {
                std::uint32_t len = 0;
                if (!ReadExact(cmd, &len, sizeof(len))) {
                    Log("cmd read failed (len)");
                    return;
                }
                if (len == 0 || len > kMaxFrameSize) {
                    Log("cmd invalid frame length");
                    return;
                }
                std::vector<std::uint8_t> payload(len);
                if (!ReadExact(cmd, payload.data(), len)) {
                    Log("cmd read failed (payload)");
                    return;
                }
                std::lock_guard<std::mutex> lock(log_mu_);
                std::cout << "[mock-core] rx cmd frame len=" << len
                          << " kind=" << GuessFrameKind(payload);
                const SceneSwitchResultInfo ssr = TryDecodeSceneSwitchResult(payload);
                if (ssr.parsed) {
                    std::cout << " request_id=" << ssr.request_id;
                    if (ssr.has_ok) {
                        std::cout << " ok=" << (ssr.ok ? "true" : "false");
                    }
                    if (ssr.error_is_nil) {
                        std::cout << " error=nil";
                    } else if (!ssr.error.empty()) {
                        std::cout << " error=" << ssr.error;
                    }
                }
                std::cout << "\n";
            } else {
                SleepMs(50);
            }
        }
    }

    bool ReadExact(HANDLE pipe, void* dst, std::uint32_t len) {
        auto* out = static_cast<std::uint8_t*>(dst);
        std::uint32_t total = 0;
        while (running_.load() && total < len) {
            DWORD got = 0;
            if (!ReadFile(pipe, out + total, len - total, &got, nullptr) || got == 0) {
                return false;
            }
            total += got;
        }
        return total == len;
    }

    bool WriteAll(HANDLE pipe, const void* src, std::uint32_t len) {
        const auto* in = static_cast<const std::uint8_t*>(src);
        std::uint32_t total = 0;
        while (running_.load() && total < len) {
            DWORD wrote = 0;
            if (!WriteFile(pipe, in + total, len - total, &wrote, nullptr) || wrote == 0) {
                return false;
            }
            total += wrote;
        }
        return total == len;
    }

    void SendEvtFrame(const std::vector<std::uint8_t>& payload, const char* label) {
        if (payload.empty()) {
            Log("evt send ignored empty payload");
            return;
        }

        HANDLE evt = nullptr;
        {
            std::lock_guard<std::mutex> lock(mu_);
            evt = evt_session_;
        }
        if (!evt) {
            Log("evt send ignored (no active session)");
            return;
        }

        const std::uint32_t len = static_cast<std::uint32_t>(payload.size());
        if (!WriteAll(evt, &len, sizeof(len)) || !WriteAll(evt, payload.data(), len)) {
            Log("evt send failed");
            return;
        }
        FlushFileBuffers(evt);

        std::lock_guard<std::mutex> lock(log_mu_);
        std::cout << "[mock-core] tx evt frame kind=" << (label ? label : "unknown")
                  << " len=" << len << "\n";
    }

    void NudgeConnectWaiters() {
        // Best-effort: connect as a client to unblock pending ConnectNamedPipe waits.
        TryOpenAndClose(kCmdPipe);
        TryOpenAndClose(kEvtPipe);
    }

    void TryOpenAndClose(const char* pipe_name) {
        HANDLE h = CreateFileA(
            pipe_name,
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
        }
    }

    void CloseSessionPipes() {
        std::lock_guard<std::mutex> lock(mu_);
        CancelIoIfValid(cmd_session_);
        CancelIoIfValid(evt_session_);
        CloseHandleIfValid(cmd_session_);
        CloseHandleIfValid(evt_session_);
    }

    void CloseListenerPipes() {
        std::lock_guard<std::mutex> lock(mu_);
        CancelIoIfValid(cmd_listen_);
        CancelIoIfValid(evt_listen_);
        CloseHandleIfValid(cmd_listen_);
        CloseHandleIfValid(evt_listen_);
    }

    void CancelAllPipeIo() {
        std::lock_guard<std::mutex> lock(mu_);
        CancelIoIfValid(cmd_listen_);
        CancelIoIfValid(evt_listen_);
        CancelIoIfValid(cmd_session_);
        CancelIoIfValid(evt_session_);
    }

    void CancelIoIfValid(HANDLE h) {
        if (h && h != INVALID_HANDLE_VALUE) {
            CancelIoEx(h, nullptr);
        }
    }

    void CloseHandleIfValid(HANDLE& h) {
        if (h && h != INVALID_HANDLE_VALUE) {
            DisconnectNamedPipe(h);
            CloseHandle(h);
            h = nullptr;
        }
    }

    void SleepMs(int ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }

    void LogWinErr(const char* what) {
        DWORD err = GetLastError();
        std::lock_guard<std::mutex> lock(log_mu_);
        std::cout << "[mock-core] " << what << " failed err=" << err << "\n";
    }

    void Log(const char* msg) {
        std::lock_guard<std::mutex> lock(log_mu_);
        std::cout << "[mock-core] " << msg << "\n";
    }

    std::atomic<bool> running_{false};
    std::atomic<bool> drop_session_requested_{false};
    std::thread worker_;
    std::mutex mu_;
    std::mutex log_mu_;
    HANDLE cmd_listen_ = nullptr;
    HANDLE evt_listen_ = nullptr;
    HANDLE cmd_session_ = nullptr;
    HANDLE evt_session_ = nullptr;
};
#endif

} // namespace

int main() {
    std::cout << "Aegis OBS plugin shim harness\n";
#ifdef _WIN32
    std::cout << "Commands: start, stop, sleep <ms>, spam-mode <count>, spam-setting <key> <count>, core-start, core-stop, core-drop, core-switch <scene>, core-switch-missing-scene, core-switch-missing-request [scene], core-send-malformed, quit\n";
#else
    std::cout << "Commands: start, stop, sleep <ms>, quit\n";
#endif

    aegis::ShimRuntime runtime;
#ifdef _WIN32
    MockCore mock_core;
#endif
    std::string line;

    while (true) {
        std::cout << "> ";
        if (!std::getline(std::cin, line)) {
            break;
        }
        if (line == "start") {
            runtime.Start();
            std::cout << "started\n";
        } else if (line == "stop") {
            runtime.Stop();
            std::cout << "stopped\n";
        } else if (line.rfind("sleep ", 0) == 0) {
            const std::string ms_str = line.substr(std::string("sleep ").size());
            const int ms = std::atoi(ms_str.c_str());
            const int safe_ms = ms < 0 ? 0 : ms;
            std::this_thread::sleep_for(std::chrono::milliseconds(safe_ms));
            std::cout << "slept " << safe_ms << "ms\n";
        } else if (line.rfind("spam-mode ", 0) == 0) {
            const std::string count_str = line.substr(std::string("spam-mode ").size());
            int count = std::atoi(count_str.c_str());
            if (count < 1) count = 1;
            for (int i = 0; i < count; ++i) {
                runtime.QueueSetModeRequest((i % 2) == 0 ? "irl" : "studio");
            }
            std::cout << "queued spam-mode count=" << count << "\n";
        } else if (line.rfind("spam-setting ", 0) == 0) {
            std::istringstream iss(line.substr(std::string("spam-setting ").size()));
            std::string key;
            int count = 0;
            iss >> key >> count;
            if (key.empty()) {
                std::cout << "usage: spam-setting <key> <count>\n";
                continue;
            }
            if (count < 1) count = 1;
            for (int i = 0; i < count; ++i) {
                runtime.QueueSetSettingRequest(key, (i % 2) == 0);
            }
            std::cout << "queued spam-setting key=" << key << " count=" << count << "\n";
#ifdef _WIN32
        } else if (line == "core-start") {
            mock_core.Start();
            std::cout << "core started\n";
        } else if (line == "core-stop") {
            mock_core.Stop();
            std::cout << "core stopped\n";
        } else if (line == "core-drop") {
            mock_core.DropSession();
            std::cout << "core drop requested\n";
        } else if (line.rfind("core-switch ", 0) == 0) {
            const std::string scene = line.substr(std::string("core-switch ").size());
            mock_core.SendSwitchScene(scene);
            std::cout << "core switch requested\n";
        } else if (line == "core-switch-missing-scene") {
            mock_core.SendSwitchSceneMissingSceneName();
            std::cout << "core missing-scene switch requested\n";
        } else if (line.rfind("core-switch-missing-request", 0) == 0) {
            std::string scene = "DemoScene";
            const auto prefix = std::string("core-switch-missing-request ");
            if (line.size() > prefix.size() && line.rfind(prefix, 0) == 0) {
                scene = line.substr(prefix.size());
            }
            mock_core.SendSwitchSceneMissingRequestId(scene);
            std::cout << "core missing-request switch requested\n";
        } else if (line == "core-send-malformed") {
            mock_core.SendMalformedEvtFrame();
            std::cout << "core malformed frame requested\n";
#endif
        } else if (line == "quit" || line == "exit") {
            break;
        } else if (line.empty()) {
            continue;
        } else {
            std::cout << "unknown command\n";
        }
    }

    runtime.Stop();
#ifdef _WIN32
    mock_core.Stop();
#endif
    return 0;
}
