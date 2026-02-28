#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace aegis {

class IpcClient {
public:
    using LogFn = std::function<void(const std::string&)>;
    using PipeStateFn = std::function<void(bool connected)>;
    using MessageTypeFn = std::function<void(const std::string&)>;
    using IncomingEnvelopeJsonFn = std::function<void(const std::string&)>;
    using SwitchSceneRequestFn = std::function<void(
        const std::string& request_id,
        const std::string& scene_name,
        const std::string& reason)>;

    struct Callbacks {
        PipeStateFn on_pipe_state;
        MessageTypeFn on_message_type;
        IncomingEnvelopeJsonFn on_incoming_envelope_json;
        SwitchSceneRequestFn on_switch_scene_request;
    };

    IpcClient();
    ~IpcClient();

    IpcClient(const IpcClient&) = delete;
    IpcClient& operator=(const IpcClient&) = delete;

    void Start();
    void Stop();
    bool IsRunning() const;

    void SetLogger(LogFn logger);
    void SetCallbacks(Callbacks callbacks);
    void SetAutoAckSwitchScene(bool enabled);
    void QueueRequestStatus();
    void QueueSetModeRequest(const std::string& mode);
    void QueueSetSettingRequest(const std::string& key, bool value);
    void QueueSceneSwitchResult(const std::string& request_id, bool ok, const std::string& error);
    void QueueObsShutdownNotice(const std::string& reason);

private:
    enum class ReadFrameResult {
        Timeout,
        Frame,
        Disconnected,
    };
    enum class PipeReadReadyResult {
        Timeout,
        Ready,
        Disconnected,
    };

    void WorkerLoop();
    void ConnectedSessionLoop();
    bool ConnectPipes();
    void DisconnectPipes();
    void SleepInterruptible(int ms);
    PipeReadReadyResult WaitForPipeReadable(void* pipe_handle, int timeout_ms);
    bool ReadExact(void* pipe_handle, void* dst, std::uint32_t len);
    bool WriteAll(void* pipe_handle, const void* src, std::uint32_t len);
    ReadFrameResult TryReadFrame(std::vector<std::uint8_t>& out_payload, int timeout_ms);
    bool WriteFrame(const std::vector<std::uint8_t>& payload);

    bool SendHello();
    bool SendRequestStatus();
    bool SendPing();
    bool SendSetModeRequest(const std::string& mode);
    bool SendSetSettingRequest(const std::string& key, bool value);
    bool SendSceneSwitchResult(const std::string& request_id, bool ok, const std::string& error);
    bool SendObsShutdownNotice(const std::string& reason);
    bool SendSceneSwitchResultOk(const std::string& request_id);
    void DrainPendingSetModeRequests();
    void DrainPendingSetSettingRequests();
    void DrainPendingSceneSwitchResults();
    void DrainPendingShutdownNotices();
    bool HandleIncomingFrame(const std::vector<std::uint8_t>& payload);
    bool TryExtractStringField(const std::vector<std::uint8_t>& payload, const char* field_name, std::string& out_value);

    void Log(const std::string& msg) const;

    std::atomic<bool> running_{false};
    std::thread worker_;
    LogFn logger_;
    Callbacks callbacks_;
    bool auto_ack_switch_scene_ = true;
    std::atomic<bool> pending_request_status_{false};
    std::mutex pending_set_mode_mu_;
    std::vector<std::string> pending_set_modes_;
    std::mutex pending_set_setting_mu_;
    std::vector<std::pair<std::string, bool>> pending_set_settings_;
    std::mutex pending_scene_results_mu_;
    std::vector<std::tuple<std::string, bool, std::string>> pending_scene_results_;
    std::mutex pending_shutdown_notices_mu_;
    std::vector<std::string> pending_shutdown_notices_;

    bool handshake_sent_ = false;
    bool request_status_sent_ = false;

    void* cmd_pipe_ = nullptr; // HANDLE
    void* evt_pipe_ = nullptr; // HANDLE
};

} // namespace aegis
