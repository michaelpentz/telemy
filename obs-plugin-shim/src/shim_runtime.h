#pragma once

#include "ipc_client.h"

namespace aegis {

class ShimRuntime {
public:
    using IpcCallbacks = IpcClient::Callbacks;
    using LogFn = IpcClient::LogFn;

    ShimRuntime();
    ~ShimRuntime();

    void Start();
    void Stop();
    bool IsRunning() const;
    void SetLogger(LogFn logger);
    void SetIpcCallbacks(IpcCallbacks callbacks);
    void SetAutoAckSwitchScene(bool enabled);
    void QueueRequestStatus();
    void QueueSetModeRequest(const std::string& mode);
    void QueueSetSettingRequest(const std::string& key, bool value);
    void QueueSceneSwitchResult(const std::string& request_id, bool ok, const std::string& error);
    void QueueObsShutdownNotice(const std::string& reason);

private:
    IpcClient ipc_;
};

} // namespace aegis
