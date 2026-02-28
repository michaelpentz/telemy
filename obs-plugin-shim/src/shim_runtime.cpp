#include "shim_runtime.h"

#include <iostream>
#include <utility>

namespace aegis {

ShimRuntime::ShimRuntime() {
    ipc_.SetLogger([](const std::string& msg) {
        std::cout << "[aegis-shim] " << msg << std::endl;
    });
}

ShimRuntime::~ShimRuntime() {
    Stop();
}

void ShimRuntime::Start() {
    ipc_.Start();
}

void ShimRuntime::Stop() {
    ipc_.Stop();
}

bool ShimRuntime::IsRunning() const {
    return ipc_.IsRunning();
}

void ShimRuntime::SetLogger(LogFn logger) {
    ipc_.SetLogger(std::move(logger));
}

void ShimRuntime::SetIpcCallbacks(IpcCallbacks callbacks) {
    ipc_.SetCallbacks(std::move(callbacks));
}

void ShimRuntime::SetAutoAckSwitchScene(bool enabled) {
    ipc_.SetAutoAckSwitchScene(enabled);
}

void ShimRuntime::QueueRequestStatus() {
    ipc_.QueueRequestStatus();
}

void ShimRuntime::QueueSetModeRequest(const std::string& mode) {
    ipc_.QueueSetModeRequest(mode);
}

void ShimRuntime::QueueSetSettingRequest(const std::string& key, bool value) {
    ipc_.QueueSetSettingRequest(key, value);
}

void ShimRuntime::QueueSceneSwitchResult(
    const std::string& request_id,
    bool ok,
    const std::string& error) {
    ipc_.QueueSceneSwitchResult(request_id, ok, error);
}

void ShimRuntime::QueueObsShutdownNotice(const std::string& reason) {
    ipc_.QueueObsShutdownNotice(reason);
}

} // namespace aegis
