#include "dock_js_bridge_api.h"
#include "shim_runtime.h"

#if defined(AEGIS_OBS_PLUGIN_BUILD)
#if defined(AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST)
#include "obs_browser_dock_host_scaffold.h"
#endif
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPalette>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("aegis-obs-shim", "en-US")

static aegis::ShimRuntime g_runtime;

namespace {

struct PendingSwitchRequest {
    std::string request_id;
    std::string scene_name;
    std::string reason;
};

std::mutex g_pending_switch_requests_mu;
std::vector<PendingSwitchRequest> g_pending_switch_requests;
std::mutex g_pending_request_status_action_ids_mu;
std::vector<std::string> g_pending_request_status_action_ids;
struct PendingSetModeAction {
    std::string request_id;
    std::string mode;
    std::chrono::steady_clock::time_point queued_at;
};
struct PendingSetSettingAction {
    std::string request_id;
    std::string key;
    bool value = false;
    std::chrono::steady_clock::time_point queued_at;
};
std::mutex g_pending_set_mode_actions_mu;
std::vector<PendingSetModeAction> g_pending_set_mode_actions;
std::mutex g_pending_set_setting_actions_mu;
std::vector<PendingSetSettingAction> g_pending_set_setting_actions;
constexpr std::chrono::milliseconds kDockActionCompletionTimeoutMs(3000);
constexpr std::chrono::milliseconds kDockActionDuplicateWindowMs(1500);
std::uint64_t g_local_dock_action_seq = 0;
std::mutex g_recent_dock_actions_mu;
std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_recent_dock_actions;
bool g_obs_timer_registered = false;
bool g_frontend_event_callback_registered = false;
bool g_frontend_exit_seen = false;
bool g_tools_menu_show_dock_registered = false;
float g_switch_pump_accum_seconds = 0.0f;
float g_theme_poll_accum_seconds = 0.0f;
using DockSceneSnapshotEmitterFn = std::function<void(const std::string&)>;
using DockBrowserJsExecuteFn = std::function<bool(const std::string&)>;
std::mutex g_dock_scene_snapshot_emitter_mu;
DockSceneSnapshotEmitterFn g_dock_scene_snapshot_emitter;
std::mutex g_dock_browser_js_execute_mu;
DockBrowserJsExecuteFn g_dock_browser_js_execute;
struct DockJsDeliveryValidationState {
    bool page_ready = false;
    bool js_sink_registered = false;
    bool logged_receive_ipc_envelope_json = false;
    bool logged_receive_scene_snapshot_json = false;
    bool logged_receive_scene_switch_completed_json = false;
    bool logged_receive_dock_action_result_json = false;
    std::uint32_t fallback_pipe_status_count = 0;
    std::uint32_t fallback_ipc_envelope_count = 0;
    std::uint32_t fallback_scene_snapshot_count = 0;
    std::uint32_t fallback_scene_switch_completed_count = 0;
    std::uint32_t fallback_dock_action_result_count = 0;
};
std::mutex g_dock_js_delivery_validation_mu;
DockJsDeliveryValidationState g_dock_js_delivery_validation;
struct DockReplayCache {
    static constexpr size_t kRecentIpcEventEnvelopeLimit = 8;
    std::string ipc_hello_ack_envelope_json;
    std::string ipc_pong_envelope_json;
    std::string ipc_status_snapshot_envelope_json;
    std::vector<std::string> recent_ipc_event_envelope_jsons;
    bool has_scene_snapshot = false;
    std::string scene_snapshot_json;
    bool has_pipe_status = false;
    std::string pipe_status;
    std::string pipe_reason;
    bool has_current_scene = false;
    std::string current_scene_name;
    bool has_scene_switch_completed = false;
    std::string scene_switch_completed_json;
    bool has_dock_action_result = false;
    std::string dock_action_result_json;
};
std::mutex g_dock_replay_cache_mu;
DockReplayCache g_dock_replay_cache;
bool g_dock_action_selftest_attempted = false;

struct DockJsSinkProbeState {
    bool js_sink_registered = false;
    bool page_ready = false;
};

std::string TryExtractEnvelopeTypeFromJson(const std::string& envelope_json);
bool EmitDockNativeJsonArgCall(const char* method_name, const std::string& payload_json);
void CacheDockIpcEnvelopeForReplay(const std::string& envelope_json);
void ReemitDockStatusSnapshotWithCurrentTheme(const char* reason);
void EmitDockActionResult(const std::string& action_type,
                          const std::string& request_id,
                          const std::string& status,
                          bool ok,
                          const std::string& error,
                          const std::string& detail);

struct ObsDockThemeSlots {
    std::string bg;
    std::string surface;
    std::string panel;
    std::string text;
    std::string textMuted;
    std::string accent;
    std::string border;
    std::string scrollbar;
    bool valid = false;
};
std::mutex g_obs_dock_theme_mu;
ObsDockThemeSlots g_obs_dock_theme_cache;
std::string g_obs_dock_theme_signature;

DockJsSinkProbeState GetDockJsSinkProbeState() {
    std::lock_guard<std::mutex> lock(g_dock_js_delivery_validation_mu);
    DockJsSinkProbeState out;
    out.js_sink_registered = g_dock_js_delivery_validation.js_sink_registered;
    out.page_ready = g_dock_js_delivery_validation.page_ready;
    return out;
}

QString ColorToCssHex(const QColor& color) {
    if (!color.isValid()) {
        return QStringLiteral("#000000");
    }
    return color.name(QColor::HexRgb);
}

QColor BlendTowardWhite(const QColor& color, double ratio) {
    if (!color.isValid()) {
        return QColor(0, 0, 0);
    }
    const double clamped = std::max(0.0, std::min(1.0, ratio));
    const int r = static_cast<int>(color.red() + (255 - color.red()) * clamped);
    const int g = static_cast<int>(color.green() + (255 - color.green()) * clamped);
    const int b = static_cast<int>(color.blue() + (255 - color.blue()) * clamped);
    return QColor(r, g, b);
}

QColor BlendTowardBlack(const QColor& color, double ratio) {
    if (!color.isValid()) {
        return QColor(0, 0, 0);
    }
    const double clamped = std::max(0.0, std::min(1.0, ratio));
    const int r = static_cast<int>(color.red() * (1.0 - clamped));
    const int g = static_cast<int>(color.green() * (1.0 - clamped));
    const int b = static_cast<int>(color.blue() * (1.0 - clamped));
    return QColor(r, g, b);
}

QColor DerivedAccentLike(const QColor& base, double ratio) {
    int h = 0;
    int s = 0;
    int l = 0;
    int a = 255;
    if (!base.isValid()) {
        return QColor(96, 128, 160);
    }
    base.getHsl(&h, &s, &l, &a);
    if (l < 128) {
        return BlendTowardWhite(base, ratio);
    }
    return BlendTowardBlack(base, ratio);
}

double SrgbToLinear01(double c) {
    if (c <= 0.04045) {
        return c / 12.92;
    }
    return std::pow((c + 0.055) / 1.055, 2.4);
}

double RelativeLuminance(const QColor& c) {
    if (!c.isValid()) {
        return 0.0;
    }
    const double r = SrgbToLinear01(static_cast<double>(c.red()) / 255.0);
    const double g = SrgbToLinear01(static_cast<double>(c.green()) / 255.0);
    const double b = SrgbToLinear01(static_cast<double>(c.blue()) / 255.0);
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

double ContrastRatio(const QColor& a, const QColor& b) {
    const double l1 = RelativeLuminance(a);
    const double l2 = RelativeLuminance(b);
    const double hi = std::max(l1, l2);
    const double lo = std::min(l1, l2);
    return (hi + 0.05) / (lo + 0.05);
}

double MinContrastAgainst(const QColor& fg, const std::vector<QColor>& bgs) {
    if (!fg.isValid() || bgs.empty()) {
        return 0.0;
    }
    double best = 1e9;
    for (const auto& bg : bgs) {
        if (!bg.isValid()) {
            continue;
        }
        best = std::min(best, ContrastRatio(fg, bg));
    }
    return (best == 1e9) ? 0.0 : best;
}

QColor PickReadableTextColor(
    const std::vector<QColor>& candidates,
    const std::vector<QColor>& backgrounds,
    double min_ratio) {
    QColor best = QColor(0, 0, 0);
    double best_score = -1.0;
    for (const auto& c : candidates) {
        if (!c.isValid()) {
            continue;
        }
        const double score = MinContrastAgainst(c, backgrounds);
        if (score >= min_ratio) {
            return c;
        }
        if (score > best_score) {
            best_score = score;
            best = c;
        }
    }
    const QColor black(0, 0, 0);
    const QColor white(255, 255, 255);
    const double black_score = MinContrastAgainst(black, backgrounds);
    const double white_score = MinContrastAgainst(white, backgrounds);
    return (black_score >= white_score) ? black : white;
}

ObsDockThemeSlots qt_palette_to_theme() {
    ObsDockThemeSlots out;
    const QApplication* app = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (!app) {
        return out;
    }

    const QPalette pal = app->palette();
    const QColor bg = pal.color(QPalette::Window);
    const QColor surface = pal.color(QPalette::Base);
    const QColor panel = pal.color(QPalette::Button);
    const QColor raw_window_text = pal.color(QPalette::WindowText);
    const QColor raw_text = pal.color(QPalette::Text);
    const QColor raw_button_text = pal.color(QPalette::ButtonText);
    const std::vector<QColor> text_bgs = {bg, surface, panel};
    const QColor text = PickReadableTextColor(
        {raw_window_text, raw_text, raw_button_text},
        text_bgs,
        4.5);
    QColor text_muted = pal.color(QPalette::PlaceholderText);
    if (!text_muted.isValid() || text_muted.alpha() == 0) {
        text_muted = text;
        text_muted.setAlpha(153); // ~60%
    }
    // Some themes expose placeholder text with poor contrast; derive a safer muted color from text if needed.
    if (MinContrastAgainst(text_muted, text_bgs) < 2.4) {
        text_muted = (RelativeLuminance(text) < 0.5)
            ? BlendTowardWhite(text, 0.35)
            : BlendTowardBlack(text, 0.35);
    }
    const QColor accent = pal.color(QPalette::Highlight);
    const QColor border = DerivedAccentLike(bg, 0.10);
    const QColor scrollbar = DerivedAccentLike(surface, 0.15);

    out.bg = ColorToCssHex(bg).toStdString();
    out.surface = ColorToCssHex(surface).toStdString();
    out.panel = ColorToCssHex(panel).toStdString();
    out.text = ColorToCssHex(text).toStdString();
    out.textMuted = ColorToCssHex(text_muted).toStdString();
    out.accent = ColorToCssHex(accent).toStdString();
    out.border = ColorToCssHex(border).toStdString();
    out.scrollbar = ColorToCssHex(scrollbar).toStdString();
    out.valid = true;
    return out;
}

QJsonObject QtThemeToJsonObject(const ObsDockThemeSlots& theme) {
    QJsonObject obj;
    if (!theme.valid) {
        return obj;
    }
    obj.insert(QStringLiteral("bg"), QString::fromStdString(theme.bg));
    obj.insert(QStringLiteral("surface"), QString::fromStdString(theme.surface));
    obj.insert(QStringLiteral("panel"), QString::fromStdString(theme.panel));
    obj.insert(QStringLiteral("text"), QString::fromStdString(theme.text));
    obj.insert(QStringLiteral("textMuted"), QString::fromStdString(theme.textMuted));
    obj.insert(QStringLiteral("accent"), QString::fromStdString(theme.accent));
    obj.insert(QStringLiteral("border"), QString::fromStdString(theme.border));
    obj.insert(QStringLiteral("scrollbar"), QString::fromStdString(theme.scrollbar));
    return obj;
}

ObsDockThemeSlots GetCachedObsDockTheme() {
    std::lock_guard<std::mutex> lock(g_obs_dock_theme_mu);
    return g_obs_dock_theme_cache;
}

void RefreshCachedObsDockThemeFromQt(const char* reason) {
    const ObsDockThemeSlots theme = qt_palette_to_theme();
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_obs_dock_theme_mu);
        const std::string next_sig = theme.valid
            ? (theme.bg + "|" + theme.surface + "|" + theme.panel + "|" + theme.text + "|" +
               theme.textMuted + "|" + theme.accent + "|" + theme.border + "|" + theme.scrollbar)
            : std::string();
        changed = (next_sig != g_obs_dock_theme_signature);
        g_obs_dock_theme_cache = theme;
        g_obs_dock_theme_signature = next_sig;
    }
    blog(
        (theme.valid && changed) ? LOG_INFO : LOG_DEBUG,
        "[aegis-obs-shim] obs dock theme cache refresh: valid=%s changed=%s reason=%s",
        theme.valid ? "true" : "false",
        changed ? "true" : "false",
        reason ? reason : "unknown");
}

void PollObsThemeChangesOnObsThread() {
    const ObsDockThemeSlots before = GetCachedObsDockTheme();
    RefreshCachedObsDockThemeFromQt("tick_poll");
    const ObsDockThemeSlots after = GetCachedObsDockTheme();
    if (!after.valid) {
        return;
    }
    const bool changed = !before.valid ||
        before.bg != after.bg ||
        before.surface != after.surface ||
        before.panel != after.panel ||
        before.text != after.text ||
        before.textMuted != after.textMuted ||
        before.accent != after.accent ||
        before.border != after.border ||
        before.scrollbar != after.scrollbar;
    if (changed) {
        ReemitDockStatusSnapshotWithCurrentTheme("tick_poll");
    }
}

std::string MaybeAugmentStatusSnapshotEnvelopeWithObsTheme(const std::string& envelope_json) {
    if (TryExtractEnvelopeTypeFromJson(envelope_json) != "status_snapshot") {
        return envelope_json;
    }

    const ObsDockThemeSlots theme = GetCachedObsDockTheme();
    if (!theme.valid) {
        return envelope_json;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(envelope_json));
    if (!doc.isObject()) {
        return envelope_json;
    }
    QJsonObject envelope = doc.object();
    QJsonObject payload = envelope.value(QStringLiteral("payload")).toObject();
    payload.insert(QStringLiteral("theme"), QtThemeToJsonObject(theme));
    envelope.insert(QStringLiteral("payload"), payload);
    return QJsonDocument(envelope).toJson(QJsonDocument::Compact).toStdString();
}

void ReemitDockStatusSnapshotWithCurrentTheme(const char* reason) {
    std::string snapshot_envelope_json;
    {
        std::lock_guard<std::mutex> lock(g_dock_replay_cache_mu);
        snapshot_envelope_json = g_dock_replay_cache.ipc_status_snapshot_envelope_json;
    }
    if (snapshot_envelope_json.empty()) {
        blog(LOG_DEBUG, "[aegis-obs-shim] theme refresh skipped: no cached status_snapshot (reason=%s)",
             reason ? reason : "unknown");
        return;
    }
    const std::string themed = MaybeAugmentStatusSnapshotEnvelopeWithObsTheme(snapshot_envelope_json);
    CacheDockIpcEnvelopeForReplay(themed);
    const bool delivered = EmitDockNativeJsonArgCall("receiveIpcEnvelopeJson", themed);
    blog(
        delivered ? LOG_INFO : LOG_DEBUG,
        "[aegis-obs-shim] dock theme refresh status_snapshot re-emitted: delivered=%s reason=%s bytes=%d",
        delivered ? "true" : "false",
        reason ? reason : "unknown",
        static_cast<int>(themed.size()));
}

std::string CurrentSceneName() {
    obs_source_t* current = obs_frontend_get_current_scene();
    if (!current) {
        return {};
    }

    const char* current_name = obs_source_get_name(current);
    std::string out = current_name ? std::string(current_name) : std::string();
    obs_source_release(current);
    return out;
}

std::vector<std::string> SnapshotSceneNames() {
    obs_frontend_source_list sources = {};
    obs_frontend_get_scenes(&sources);

    std::vector<std::string> names;
    names.reserve(sources.sources.num);
    for (size_t i = 0; i < sources.sources.num; ++i) {
        obs_source_t* src = sources.sources.array[i];
        if (!src) {
            continue;
        }
        const char* name = obs_source_get_name(src);
        names.push_back(name ? std::string(name) : std::string());
    }
    obs_frontend_source_list_free(&sources);
    return names;
}

std::string JsonEscape(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (char ch : input) {
        switch (ch) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out.push_back(ch);
            break;
        }
    }
    return out;
}

std::string JsStringLiteral(const std::string& input) {
    return std::string("\"") + JsonEscape(input) + "\"";
}

std::string BuildDockSceneSnapshotPayloadJson(
    const char* reason,
    const std::vector<std::string>& scene_names,
    const std::string& current_scene_name) {
    std::ostringstream os;
    os << "{";
    os << "\"reason\":\"" << JsonEscape(reason ? reason : "unknown") << "\",";
    os << "\"sceneNames\":[";
    for (size_t i = 0; i < scene_names.size(); ++i) {
        if (i > 0) {
            os << ",";
        }
        os << "\"" << JsonEscape(scene_names[i]) << "\"";
    }
    os << "],";
    os << "\"currentSceneName\":";
    if (current_scene_name.empty()) {
        os << "null";
    } else {
        os << "\"" << JsonEscape(current_scene_name) << "\"";
    }
    os << "}";
    return os.str();
}

void SetDockSceneSnapshotEmitter(DockSceneSnapshotEmitterFn emitter) {
    std::lock_guard<std::mutex> lock(g_dock_scene_snapshot_emitter_mu);
    g_dock_scene_snapshot_emitter = std::move(emitter);
}

void SetDockBrowserJsExecuteSink(DockBrowserJsExecuteFn execute_fn) {
    const bool has_sink = static_cast<bool>(execute_fn);
    std::lock_guard<std::mutex> lock(g_dock_browser_js_execute_mu);
    g_dock_browser_js_execute = std::move(execute_fn);
    {
        std::lock_guard<std::mutex> validation_lock(g_dock_js_delivery_validation_mu);
        g_dock_js_delivery_validation.js_sink_registered = has_sink;
        if (!has_sink) {
            g_dock_js_delivery_validation.page_ready = false;
            g_dock_js_delivery_validation.fallback_pipe_status_count = 0;
            g_dock_js_delivery_validation.fallback_ipc_envelope_count = 0;
            g_dock_js_delivery_validation.fallback_scene_snapshot_count = 0;
            g_dock_js_delivery_validation.fallback_scene_switch_completed_count = 0;
            g_dock_js_delivery_validation.fallback_dock_action_result_count = 0;
        }
    }
}

bool TryExecuteDockBrowserJs(const std::string& js_code) {
    DockBrowserJsExecuteFn exec_copy;
    {
        std::lock_guard<std::mutex> lock(g_dock_browser_js_execute_mu);
        exec_copy = g_dock_browser_js_execute;
    }
    if (!exec_copy) {
        return false;
    }
    return exec_copy(js_code);
}

std::string TryExtractEnvelopeTypeFromJson(const std::string& envelope_json);
bool TryExtractJsonStringField(
    const std::string& json_text,
    const char* field_name,
    std::string* out_value);
bool TryExtractJsonBoolField(
    const std::string& json_text,
    const char* field_name,
    bool* out_value);
bool IsEnvEnabled(const char* name);
bool IsRecognizedDockMode(const std::string& mode);
bool IsRecognizedDockSettingKey(const std::string& key);

enum class DockFallbackLogKind {
    PipeStatus,
    IpcEnvelopeJson,
    SceneSnapshotJson,
    SceneSwitchCompletedJson,
    DockActionResultJson,
};

bool ShouldLogDockFallbackPayload(
    DockFallbackLogKind kind,
    const char** out_phase,
    std::uint32_t* out_attempt) {
    if (out_phase) {
        *out_phase = "unknown";
    }
    if (out_attempt) {
        *out_attempt = 0;
    }

    std::lock_guard<std::mutex> lock(g_dock_js_delivery_validation_mu);
    std::uint32_t* count = nullptr;
    switch (kind) {
    case DockFallbackLogKind::PipeStatus:
        count = &g_dock_js_delivery_validation.fallback_pipe_status_count;
        break;
    case DockFallbackLogKind::IpcEnvelopeJson:
        count = &g_dock_js_delivery_validation.fallback_ipc_envelope_count;
        break;
    case DockFallbackLogKind::SceneSnapshotJson:
        count = &g_dock_js_delivery_validation.fallback_scene_snapshot_count;
        break;
    case DockFallbackLogKind::SceneSwitchCompletedJson:
        count = &g_dock_js_delivery_validation.fallback_scene_switch_completed_count;
        break;
    case DockFallbackLogKind::DockActionResultJson:
        count = &g_dock_js_delivery_validation.fallback_dock_action_result_count;
        break;
    }

    if (!count) {
        return true;
    }

    *count += 1;
    if (out_attempt) {
        *out_attempt = *count;
    }

    if (!g_dock_js_delivery_validation.js_sink_registered) {
        if (out_phase) {
            *out_phase = "no_js_sink";
        }
        return true;
    }

    if (!g_dock_js_delivery_validation.page_ready) {
        if (out_phase) {
            *out_phase = "pre_page_ready";
        }
        return (*count <= 3) || ((*count % 20) == 0);
    }

    if (out_phase) {
        *out_phase = "post_page_ready_sink_miss";
    }
    return (*count == 1) || ((*count % 50) == 0);
}

bool EmitDockNativeJsonArgCall(const char* method_name, const std::string& payload_json) {
    if (!method_name || payload_json.empty()) {
        return false;
    }
    std::ostringstream js;
    js << "if (window.aegisDockNative && typeof window.aegisDockNative." << method_name
       << " === 'function') { window.aegisDockNative." << method_name << "("
       << JsStringLiteral(payload_json) << "); }";
    const bool delivered = TryExecuteDockBrowserJs(js.str());
    if (delivered) {
        std::lock_guard<std::mutex> lock(g_dock_js_delivery_validation_mu);
        if (g_dock_js_delivery_validation.page_ready &&
            g_dock_js_delivery_validation.js_sink_registered) {
            bool* already_logged = nullptr;
            if (std::string(method_name) == "receiveIpcEnvelopeJson") {
                already_logged = &g_dock_js_delivery_validation.logged_receive_ipc_envelope_json;
            } else if (std::string(method_name) == "receiveSceneSnapshotJson") {
                already_logged = &g_dock_js_delivery_validation.logged_receive_scene_snapshot_json;
            } else if (std::string(method_name) == "receiveSceneSwitchCompletedJson") {
                already_logged =
                    &g_dock_js_delivery_validation.logged_receive_scene_switch_completed_json;
            } else if (std::string(method_name) == "receiveDockActionResultJson") {
                already_logged =
                    &g_dock_js_delivery_validation.logged_receive_dock_action_result_json;
            }
            if (already_logged && !*already_logged) {
                *already_logged = true;
                const std::string envelope_type =
                    (std::string(method_name) == "receiveIpcEnvelopeJson")
                        ? TryExtractEnvelopeTypeFromJson(payload_json)
                        : std::string();
                if (!envelope_type.empty()) {
                    blog(
                        LOG_INFO,
                        "[aegis-obs-shim] dock js sink delivery validated post-page-ready: method=%s payload_bytes=%d envelope_type=%s",
                        method_name,
                        static_cast<int>(payload_json.size()),
                        envelope_type.c_str());
                } else {
                    blog(
                        LOG_INFO,
                        "[aegis-obs-shim] dock js sink delivery validated post-page-ready: method=%s payload_bytes=%d",
                        method_name,
                        static_cast<int>(payload_json.size()));
                }
            }
        }
    }
    return delivered;
}

bool EmitDockNativePipeStatus(const char* status, const char* reason) {
    if (!status) {
        return false;
    }
    std::ostringstream js;
    js << "if (window.aegisDockNative && typeof window.aegisDockNative.receivePipeStatus === "
          "'function') { window.aegisDockNative.receivePipeStatus("
       << JsStringLiteral(status) << ",";
    if (reason && *reason) {
        js << JsStringLiteral(reason);
    } else {
        js << "null";
    }
    js << "); }";
    return TryExecuteDockBrowserJs(js.str());
}

bool EmitDockNativeCurrentScene(const std::string& scene_name) {
    std::ostringstream js;
    js << "if (window.aegisDockNative && typeof window.aegisDockNative.receiveCurrentScene === "
          "'function') { window.aegisDockNative.receiveCurrentScene(";
    if (scene_name.empty()) {
        js << "null";
    } else {
        js << JsStringLiteral(scene_name);
    }
    js << "); }";
    return TryExecuteDockBrowserJs(js.str());
}

void CacheDockSceneSnapshotForReplay(const std::string& payload_json) {
    std::lock_guard<std::mutex> lock(g_dock_replay_cache_mu);
    g_dock_replay_cache.has_scene_snapshot = !payload_json.empty();
    g_dock_replay_cache.scene_snapshot_json = payload_json;
}

void CacheDockPipeStatusForReplay(const char* status, const char* reason) {
    std::lock_guard<std::mutex> lock(g_dock_replay_cache_mu);
    g_dock_replay_cache.has_pipe_status = (status != nullptr && *status != '\0');
    g_dock_replay_cache.pipe_status = status ? status : "";
    g_dock_replay_cache.pipe_reason = (reason && *reason) ? reason : "";
}

void CacheDockCurrentSceneForReplay(const std::string& scene_name) {
    std::lock_guard<std::mutex> lock(g_dock_replay_cache_mu);
    g_dock_replay_cache.has_current_scene = true;
    g_dock_replay_cache.current_scene_name = scene_name;
}

void CacheDockSceneSwitchCompletedForReplay(const std::string& payload_json) {
    std::lock_guard<std::mutex> lock(g_dock_replay_cache_mu);
    g_dock_replay_cache.has_scene_switch_completed = !payload_json.empty();
    g_dock_replay_cache.scene_switch_completed_json = payload_json;
}

void CacheDockActionResultForReplay(const std::string& payload_json) {
    std::lock_guard<std::mutex> lock(g_dock_replay_cache_mu);
    g_dock_replay_cache.has_dock_action_result = !payload_json.empty();
    g_dock_replay_cache.dock_action_result_json = payload_json;
}

std::string TryExtractEnvelopeTypeFromJson(const std::string& envelope_json) {
    const std::string needle = "\"type\":\"";
    const std::size_t start = envelope_json.find(needle);
    if (start == std::string::npos) {
        return {};
    }
    const std::size_t value_start = start + needle.size();
    const std::size_t end = envelope_json.find('"', value_start);
    if (end == std::string::npos || end <= value_start) {
        return {};
    }
    return envelope_json.substr(value_start, end - value_start);
}

bool TryExtractJsonStringField(
    const std::string& json_text,
    const char* field_name,
    std::string* out_value) {
    if (!field_name || !out_value) {
        return false;
    }

    std::ostringstream needle;
    needle << "\"" << field_name << "\"";
    const std::string needle_str = needle.str();
    const std::size_t key_pos = json_text.find(needle_str);
    if (key_pos == std::string::npos) {
        return false;
    }

    const std::size_t colon_pos = json_text.find(':', key_pos + needle_str.size());
    if (colon_pos == std::string::npos) {
        return false;
    }

    std::size_t pos = colon_pos + 1;
    while (pos < json_text.size() && std::isspace(static_cast<unsigned char>(json_text[pos])) != 0) {
        pos += 1;
    }
    if (pos >= json_text.size() || json_text[pos] != '"') {
        return false;
    }
    pos += 1;

    std::string value;
    value.reserve(64);
    bool escaping = false;
    for (; pos < json_text.size(); ++pos) {
        const char ch = json_text[pos];
        if (escaping) {
            switch (ch) {
            case '"':
            case '\\':
            case '/':
                value.push_back(ch);
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            default:
                value.push_back(ch);
                break;
            }
            escaping = false;
            continue;
        }
        if (ch == '\\') {
            escaping = true;
            continue;
        }
        if (ch == '"') {
            *out_value = std::move(value);
            return true;
        }
        value.push_back(ch);
    }
    return false;
}

bool TryExtractJsonBoolField(
    const std::string& json_text,
    const char* field_name,
    bool* out_value) {
    if (!field_name || !out_value) {
        return false;
    }

    std::ostringstream needle;
    needle << "\"" << field_name << "\"";
    const std::string needle_str = needle.str();
    const std::size_t key_pos = json_text.find(needle_str);
    if (key_pos == std::string::npos) {
        return false;
    }

    const std::size_t colon_pos = json_text.find(':', key_pos + needle_str.size());
    if (colon_pos == std::string::npos) {
        return false;
    }

    std::size_t pos = colon_pos + 1;
    while (pos < json_text.size() && std::isspace(static_cast<unsigned char>(json_text[pos])) != 0) {
        pos += 1;
    }
    if (json_text.compare(pos, 4, "true") == 0) {
        *out_value = true;
        return true;
    }
    if (json_text.compare(pos, 5, "false") == 0) {
        *out_value = false;
        return true;
    }
    return false;
}

bool IsEnvEnabled(const char* name) {
    if (!name || *name == '\0') {
        return false;
    }
    const char* raw = std::getenv(name);
    if (!raw || *raw == '\0') {
        return false;
    }
    std::string value(raw);
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return !(value == "0" || value == "false" || value == "no" || value == "off");
}

bool IsRecognizedDockMode(const std::string& mode) {
    return mode == "studio" || mode == "irl";
}

bool IsRecognizedDockSettingKey(const std::string& key) {
    return key == "auto_scene_switch" ||
           key == "low_quality_fallback" ||
           key == "manual_override" ||
           key == "chat_bot" ||
           key == "alerts";
}

bool ShouldDeduplicateDockActionByRequestId(
    const std::string& action_type,
    const std::string& request_id) {
    if (request_id.empty() || action_type.empty()) {
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    const std::string dedupe_key = action_type + "|" + request_id;
    std::lock_guard<std::mutex> lock(g_recent_dock_actions_mu);

    for (auto it = g_recent_dock_actions.begin(); it != g_recent_dock_actions.end();) {
        if ((now - it->second) > kDockActionDuplicateWindowMs) {
            it = g_recent_dock_actions.erase(it);
        } else {
            ++it;
        }
    }

    auto found = g_recent_dock_actions.find(dedupe_key);
    if (found != g_recent_dock_actions.end()) {
        return true;
    }
    g_recent_dock_actions.emplace(dedupe_key, now);
    return false;
}

void TrackPendingDockRequestStatusAction(const std::string& request_id) {
    if (request_id.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_pending_request_status_action_ids_mu);
    g_pending_request_status_action_ids.push_back(request_id);
}

std::string ConsumePendingDockRequestStatusActionId() {
    std::lock_guard<std::mutex> lock(g_pending_request_status_action_ids_mu);
    if (g_pending_request_status_action_ids.empty()) {
        return {};
    }
    std::string request_id = g_pending_request_status_action_ids.front();
    g_pending_request_status_action_ids.erase(g_pending_request_status_action_ids.begin());
    return request_id;
}

void TrackPendingDockSetModeAction(const std::string& request_id, const std::string& mode) {
    if (request_id.empty() || mode.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_pending_set_mode_actions_mu);
    g_pending_set_mode_actions.push_back(
        PendingSetModeAction{request_id, mode, std::chrono::steady_clock::now()});
}

void TrackPendingDockSetSettingAction(
    const std::string& request_id,
    const std::string& key,
    bool value) {
    if (request_id.empty() || key.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_pending_set_setting_actions_mu);
    g_pending_set_setting_actions.push_back(
        PendingSetSettingAction{request_id, key, value, std::chrono::steady_clock::now()});
}

struct StatusSnapshotProjection {
    bool valid = false;
    bool has_mode = false;
    std::string mode;
    bool has_auto_scene_switch = false;
    bool auto_scene_switch = false;
    bool has_low_quality_fallback = false;
    bool low_quality_fallback = false;
    bool has_manual_override = false;
    bool manual_override = false;
    bool has_chat_bot = false;
    bool chat_bot = false;
    bool has_alerts = false;
    bool alerts = false;
};

bool TryProjectStatusSnapshot(const std::string& envelope_json, StatusSnapshotProjection* out) {
    if (!out) {
        return false;
    }
    *out = StatusSnapshotProjection{};
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(envelope_json));
    if (!doc.isObject()) {
        return false;
    }
    const QJsonObject envelope = doc.object();
    const QJsonValue type_val = envelope.value(QStringLiteral("type"));
    if (!type_val.isString() || type_val.toString() != QStringLiteral("status_snapshot")) {
        return false;
    }
    const QJsonObject payload = envelope.value(QStringLiteral("payload")).toObject();
    if (payload.isEmpty()) {
        return false;
    }
    out->valid = true;
    const QJsonValue mode_val = payload.value(QStringLiteral("mode"));
    if (mode_val.isString()) {
        out->has_mode = true;
        out->mode = mode_val.toString().toStdString();
    }
    const QJsonObject settings = payload.value(QStringLiteral("settings")).toObject();
    auto read_setting_bool = [&settings](const char* key, bool* has, bool* value) {
        if (!key || !has || !value) {
            return;
        }
        const QJsonValue v = settings.value(QString::fromUtf8(key));
        if (!v.isBool()) {
            return;
        }
        *has = true;
        *value = v.toBool();
    };
    read_setting_bool("auto_scene_switch", &out->has_auto_scene_switch, &out->auto_scene_switch);
    read_setting_bool("low_quality_fallback", &out->has_low_quality_fallback, &out->low_quality_fallback);
    read_setting_bool("manual_override", &out->has_manual_override, &out->manual_override);
    read_setting_bool("chat_bot", &out->has_chat_bot, &out->chat_bot);
    read_setting_bool("alerts", &out->has_alerts, &out->alerts);
    return true;
}

bool TryGetStatusSnapshotSettingBool(
    const StatusSnapshotProjection& snap,
    const std::string& key,
    bool* out_value) {
    if (!out_value) {
        return false;
    }
    if (key == "auto_scene_switch" && snap.has_auto_scene_switch) {
        *out_value = snap.auto_scene_switch;
        return true;
    }
    if (key == "low_quality_fallback" && snap.has_low_quality_fallback) {
        *out_value = snap.low_quality_fallback;
        return true;
    }
    if (key == "manual_override" && snap.has_manual_override) {
        *out_value = snap.manual_override;
        return true;
    }
    if (key == "chat_bot" && snap.has_chat_bot) {
        *out_value = snap.chat_bot;
        return true;
    }
    if (key == "alerts" && snap.has_alerts) {
        *out_value = snap.alerts;
        return true;
    }
    return false;
}

void ResolvePendingDockActionCompletionsFromStatusSnapshot(const std::string& envelope_json) {
    StatusSnapshotProjection snap;
    if (!TryProjectStatusSnapshot(envelope_json, &snap) || !snap.valid) {
        return;
    }

    std::vector<std::string> completed_mode_ids;
    std::vector<std::string> completed_setting_ids;
    {
        std::lock_guard<std::mutex> lock(g_pending_set_mode_actions_mu);
        auto it = g_pending_set_mode_actions.begin();
        while (it != g_pending_set_mode_actions.end()) {
            if (snap.has_mode && it->mode == snap.mode) {
                completed_mode_ids.push_back(it->request_id);
                it = g_pending_set_mode_actions.erase(it);
                continue;
            }
            ++it;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_pending_set_setting_actions_mu);
        auto it = g_pending_set_setting_actions.begin();
        while (it != g_pending_set_setting_actions.end()) {
            bool current_value = false;
            if (TryGetStatusSnapshotSettingBool(snap, it->key, &current_value) &&
                current_value == it->value) {
                completed_setting_ids.push_back(it->request_id);
                it = g_pending_set_setting_actions.erase(it);
                continue;
            }
            ++it;
        }
    }

    for (const auto& request_id : completed_mode_ids) {
        EmitDockActionResult(
            "set_mode",
            request_id,
            "completed",
            true,
            "",
            "status_snapshot_applied");
    }
    for (const auto& request_id : completed_setting_ids) {
        EmitDockActionResult(
            "set_setting",
            request_id,
            "completed",
            true,
            "",
            "status_snapshot_applied");
    }
}

void DrainExpiredPendingDockActions() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::string> timed_out_set_mode_ids;
    std::vector<std::string> timed_out_set_setting_ids;
    {
        std::lock_guard<std::mutex> lock(g_pending_set_mode_actions_mu);
        auto it = g_pending_set_mode_actions.begin();
        while (it != g_pending_set_mode_actions.end()) {
            if (now - it->queued_at >= kDockActionCompletionTimeoutMs) {
                timed_out_set_mode_ids.push_back(it->request_id);
                it = g_pending_set_mode_actions.erase(it);
                continue;
            }
            ++it;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_pending_set_setting_actions_mu);
        auto it = g_pending_set_setting_actions.begin();
        while (it != g_pending_set_setting_actions.end()) {
            if (now - it->queued_at >= kDockActionCompletionTimeoutMs) {
                timed_out_set_setting_ids.push_back(it->request_id);
                it = g_pending_set_setting_actions.erase(it);
                continue;
            }
            ++it;
        }
    }
    for (const auto& request_id : timed_out_set_mode_ids) {
        EmitDockActionResult(
            "set_mode",
            request_id,
            "failed",
            false,
            "completion_timeout",
            "status_snapshot_not_observed");
    }
    for (const auto& request_id : timed_out_set_setting_ids) {
        EmitDockActionResult(
            "set_setting",
            request_id,
            "failed",
            false,
            "completion_timeout",
            "status_snapshot_not_observed");
    }
}

void CacheDockIpcEnvelopeForReplay(const std::string& envelope_json) {
    if (envelope_json.empty()) {
        return;
    }
    const std::string type = TryExtractEnvelopeTypeFromJson(envelope_json);
    if (type.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_dock_replay_cache_mu);
    if (type == "hello_ack") {
        g_dock_replay_cache.ipc_hello_ack_envelope_json = envelope_json;
    } else if (type == "pong") {
        g_dock_replay_cache.ipc_pong_envelope_json = envelope_json;
    } else if (type == "status_snapshot") {
        g_dock_replay_cache.ipc_status_snapshot_envelope_json = envelope_json;
    } else if (type == "user_notice" || type == "protocol_error" || type == "switch_scene") {
        auto& items = g_dock_replay_cache.recent_ipc_event_envelope_jsons;
        items.push_back(envelope_json);
        if (items.size() > DockReplayCache::kRecentIpcEventEnvelopeLimit) {
            items.erase(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(
                items.size() - DockReplayCache::kRecentIpcEventEnvelopeLimit));
        }
    }
}

void ClearDockReplayCache() {
    std::lock_guard<std::mutex> lock(g_dock_replay_cache_mu);
    g_dock_replay_cache = DockReplayCache{};
}

void ReplayDockStateToJsSinkIfAvailable() {
    DockReplayCache snapshot;
    {
        std::lock_guard<std::mutex> lock(g_dock_replay_cache_mu);
        snapshot = g_dock_replay_cache;
    }
    const DockJsSinkProbeState sink_state = GetDockJsSinkProbeState();

    if (snapshot.has_pipe_status) {
        EmitDockNativePipeStatus(snapshot.pipe_status.c_str(), snapshot.pipe_reason.c_str());
    }
    if (!snapshot.ipc_hello_ack_envelope_json.empty()) {
        EmitDockNativeJsonArgCall("receiveIpcEnvelopeJson", snapshot.ipc_hello_ack_envelope_json);
    }
    if (!snapshot.ipc_pong_envelope_json.empty()) {
        EmitDockNativeJsonArgCall("receiveIpcEnvelopeJson", snapshot.ipc_pong_envelope_json);
    }
    if (!snapshot.ipc_status_snapshot_envelope_json.empty()) {
        EmitDockNativeJsonArgCall("receiveIpcEnvelopeJson", snapshot.ipc_status_snapshot_envelope_json);
    }
    for (const auto& event_envelope_json : snapshot.recent_ipc_event_envelope_jsons) {
        if (!event_envelope_json.empty()) {
            EmitDockNativeJsonArgCall("receiveIpcEnvelopeJson", event_envelope_json);
        }
    }
    if (snapshot.has_scene_snapshot && !snapshot.scene_snapshot_json.empty()) {
        const bool delivered =
            EmitDockNativeJsonArgCall("receiveSceneSnapshotJson", snapshot.scene_snapshot_json);
        blog(
            delivered ? LOG_INFO : LOG_WARNING,
            "[aegis-obs-shim] dock replay scene snapshot: delivered=%s bytes=%d js_sink=%s page_ready=%s",
            delivered ? "true" : "false",
            static_cast<int>(snapshot.scene_snapshot_json.size()),
            sink_state.js_sink_registered ? "true" : "false",
            sink_state.page_ready ? "true" : "false");
    } else {
        blog(
            LOG_INFO,
            "[aegis-obs-shim] dock replay scene snapshot: skipped (cached_scene_snapshot=%s) js_sink=%s page_ready=%s",
            snapshot.has_scene_snapshot ? "empty_payload" : "none",
            sink_state.js_sink_registered ? "true" : "false",
            sink_state.page_ready ? "true" : "false");
    }
    if (snapshot.has_current_scene) {
        EmitDockNativeCurrentScene(snapshot.current_scene_name);
    }
    if (snapshot.has_scene_switch_completed && !snapshot.scene_switch_completed_json.empty()) {
        EmitDockNativeJsonArgCall(
            "receiveSceneSwitchCompletedJson",
            snapshot.scene_switch_completed_json);
    }
    if (snapshot.has_dock_action_result && !snapshot.dock_action_result_json.empty()) {
        EmitDockNativeJsonArgCall("receiveDockActionResultJson", snapshot.dock_action_result_json);
    }
}

void RegisterDockBrowserJsExecuteSink(DockBrowserJsExecuteFn execute_fn) {
    SetDockBrowserJsExecuteSink(std::move(execute_fn));
    ReplayDockStateToJsSinkIfAvailable();
}

void MaybeRunDockActionSelfTestAfterPageReady() {
    if (g_dock_action_selftest_attempted) {
        return;
    }
    g_dock_action_selftest_attempted = true;

    if (!IsEnvEnabled("AEGIS_DOCK_ENABLE_SELFTEST")) {
        return;
    }

    const char* raw_action_json = std::getenv("AEGIS_DOCK_SELFTEST_ACTION_JSON");
    if (!raw_action_json || !*raw_action_json) {
        blog(
            LOG_INFO,
            "[aegis-obs-shim] dock selftest enabled but no action json provided (AEGIS_DOCK_SELFTEST_ACTION_JSON)");
        return;
    }

    const std::string action_json(raw_action_json);
    const char* raw_direct = std::getenv("AEGIS_DOCK_SELFTEST_DIRECT_PLUGIN_INTAKE");
    const bool direct_intake = (raw_direct && *raw_direct && std::string(raw_direct) != "0");
    if (direct_intake) {
        const bool accepted = aegis_obs_shim_receive_dock_action_json(action_json.c_str());
        blog(
            accepted ? LOG_INFO : LOG_WARNING,
            "[aegis-obs-shim] dock selftest direct plugin intake ok=%s json=%s",
            accepted ? "true" : "false",
            action_json.c_str());
        return;
    }

    std::ostringstream js;
    js << "(function(){"
          "var payload="
       << JsStringLiteral(action_json)
       << ";"
          "var sent=false;"
          "if(window.aegisDockNative&&typeof window.aegisDockNative.sendDockActionJson==='function'){"
          "  try{ window.aegisDockNative.sendDockActionJson(payload); sent=true; }catch(_e){}"
          "}"
          "if(typeof document!=='undefined'&&typeof document.title==='string'&&typeof encodeURIComponent==='function'){"
          "  try{ document.title='__AEGIS_DOCK_ACTION__:'+encodeURIComponent(payload); sent=true; }catch(_e){}"
          "}"
          "if(typeof location!=='undefined'&&typeof location.hash==='string'&&typeof encodeURIComponent==='function'){"
          "  try{ location.hash='__AEGIS_DOCK_ACTION__:'+encodeURIComponent(payload); sent=true; }catch(_e){}"
          "}"
          "return sent; })();";

    const bool dispatched = TryExecuteDockBrowserJs(js.str());
    blog(
        dispatched ? LOG_INFO : LOG_WARNING,
        "[aegis-obs-shim] dock selftest action dispatch page_ready ok=%s json=%s (path=native_api_plus_title_hash)",
        dispatched ? "true" : "false",
        action_json.c_str());
}

std::string BuildSceneSwitchCompletedJson(const std::string& request_id,
                                          const std::string& scene_name,
                                          bool ok,
                                          const std::string& error,
                                          const std::string& reason) {
    std::ostringstream os;
    os << "{";
    os << "\"requestId\":";
    if (request_id.empty()) {
        os << "null";
    } else {
        os << "\"" << JsonEscape(request_id) << "\"";
    }
    os << ",";
    os << "\"sceneName\":";
    if (scene_name.empty()) {
        os << "null";
    } else {
        os << "\"" << JsonEscape(scene_name) << "\"";
    }
    os << ",";
    os << "\"ok\":" << (ok ? "true" : "false") << ",";
    os << "\"error\":";
    if (ok || error.empty()) {
        os << "null";
    } else {
        os << "\"" << JsonEscape(error) << "\"";
    }
    os << ",";
    os << "\"reason\":";
    if (reason.empty()) {
        os << "null";
    } else {
        os << "\"" << JsonEscape(reason) << "\"";
    }
    os << "}";
    return os.str();
}

std::string BuildDockActionResultJson(const std::string& action_type,
                                      const std::string& request_id,
                                      const std::string& status,
                                      bool ok,
                                      const std::string& error,
                                      const std::string& detail) {
    std::ostringstream os;
    os << "{";
    os << "\"actionType\":";
    if (action_type.empty()) {
        os << "null";
    } else {
        os << "\"" << JsonEscape(action_type) << "\"";
    }
    os << ",";
    os << "\"requestId\":";
    if (request_id.empty()) {
        os << "null";
    } else {
        os << "\"" << JsonEscape(request_id) << "\"";
    }
    os << ",";
    os << "\"status\":\"" << JsonEscape(status.empty() ? "unknown" : status) << "\",";
    os << "\"ok\":" << (ok ? "true" : "false") << ",";
    os << "\"error\":";
    if (error.empty()) {
        os << "null";
    } else {
        os << "\"" << JsonEscape(error) << "\"";
    }
    os << ",";
    os << "\"detail\":";
    if (detail.empty()) {
        os << "null";
    } else {
        os << "\"" << JsonEscape(detail) << "\"";
    }
    os << "}";
    return os.str();
}

void EmitDockActionResult(const std::string& action_type,
                          const std::string& request_id,
                          const std::string& status,
                          bool ok,
                          const std::string& error,
                          const std::string& detail) {
    const std::string payload_json =
        BuildDockActionResultJson(action_type, request_id, status, ok, error, detail);
    blog(
        LOG_INFO,
        "[aegis-obs-shim] dock action result: action_type=%s request_id=%s status=%s ok=%s error=%s detail=%s",
        action_type.empty() ? "" : action_type.c_str(),
        request_id.empty() ? "" : request_id.c_str(),
        status.empty() ? "" : status.c_str(),
        ok ? "true" : "false",
        error.empty() ? "" : error.c_str(),
        detail.empty() ? "" : detail.c_str());
    CacheDockActionResultForReplay(payload_json);
    if (!EmitDockNativeJsonArgCall("receiveDockActionResultJson", payload_json)) {
        const char* phase = nullptr;
        std::uint32_t attempt = 0;
        if (ShouldLogDockFallbackPayload(DockFallbackLogKind::DockActionResultJson, &phase, &attempt)) {
            blog(
                LOG_DEBUG,
                "[aegis-obs-shim] dock bridge fallback payload phase=%s attempt=%u receiveDockActionResultJson=%s",
                phase ? phase : "unknown",
                attempt,
                payload_json.c_str());
        }
    }
}

void EmitDockSceneSwitchCompleted(const std::string& request_id,
                                  const std::string& scene_name,
                                  bool ok,
                                  const std::string& error,
                                  const std::string& reason) {
    const std::string payload_json =
        BuildSceneSwitchCompletedJson(request_id, scene_name, ok, error, reason);
    CacheDockSceneSwitchCompletedForReplay(payload_json);
    if (!EmitDockNativeJsonArgCall("receiveSceneSwitchCompletedJson", payload_json)) {
        const char* phase = nullptr;
        std::uint32_t attempt = 0;
        if (ShouldLogDockFallbackPayload(
                DockFallbackLogKind::SceneSwitchCompletedJson, &phase, &attempt)) {
            blog(
                LOG_INFO,
                "[aegis-obs-shim] dock bridge fallback payload phase=%s attempt=%u receiveSceneSwitchCompletedJson=%s",
                phase ? phase : "unknown",
                attempt,
                payload_json.c_str());
        }
    }
}

bool EmitDockSceneSnapshotPayload(const std::string& payload_json) {
    CacheDockSceneSnapshotForReplay(payload_json);
    DockSceneSnapshotEmitterFn emitter_copy;
    {
        std::lock_guard<std::mutex> lock(g_dock_scene_snapshot_emitter_mu);
        emitter_copy = g_dock_scene_snapshot_emitter;
    }
    bool delivered = false;
    if (!emitter_copy) {
        delivered = EmitDockNativeJsonArgCall("receiveSceneSnapshotJson", payload_json);
    } else {
        emitter_copy(payload_json);
        delivered = true;
    }
    const DockJsSinkProbeState sink_state = GetDockJsSinkProbeState();
    blog(
        delivered ? LOG_DEBUG : LOG_INFO,
        "[aegis-obs-shim] dock scene snapshot dispatch: delivered=%s via=%s bytes=%d js_sink=%s page_ready=%s",
        delivered ? "true" : "false",
        emitter_copy ? "emitter" : "native_js_call",
        static_cast<int>(payload_json.size()),
        sink_state.js_sink_registered ? "true" : "false",
        sink_state.page_ready ? "true" : "false");
    return delivered;
}

void EmitDockIpcEnvelopeJson(const std::string& envelope_json) {
    const std::string themed_envelope_json = MaybeAugmentStatusSnapshotEnvelopeWithObsTheme(envelope_json);
    const std::string envelope_type = TryExtractEnvelopeTypeFromJson(themed_envelope_json);
    CacheDockIpcEnvelopeForReplay(themed_envelope_json);
    if (!EmitDockNativeJsonArgCall("receiveIpcEnvelopeJson", themed_envelope_json)) {
        const char* phase = nullptr;
        std::uint32_t attempt = 0;
        if (ShouldLogDockFallbackPayload(DockFallbackLogKind::IpcEnvelopeJson, &phase, &attempt)) {
            blog(
                LOG_DEBUG,
                "[aegis-obs-shim] dock bridge fallback payload phase=%s attempt=%u receiveIpcEnvelopeJson=%s",
                phase ? phase : "unknown",
                attempt,
                themed_envelope_json.c_str());
        }
    }
    if (envelope_type == "status_snapshot") {
        ResolvePendingDockActionCompletionsFromStatusSnapshot(themed_envelope_json);
        const std::string request_id = ConsumePendingDockRequestStatusActionId();
        if (!request_id.empty()) {
            EmitDockActionResult(
                "request_status",
                request_id,
                "completed",
                true,
                "",
                "status_snapshot_received");
        }
    }
}

#if defined(AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST)
void InitializeBrowserDockHostBridge() {
    // Delegate to a dedicated scaffold module so future Qt/CEF embedding can evolve
    // without expanding this plugin entry file.
    aegis_obs_browser_dock_host_scaffold_initialize();
}

void ShutdownBrowserDockHostBridge() {
    aegis_obs_browser_dock_host_scaffold_shutdown();
}
#else
void InitializeBrowserDockHostBridge() {
    RegisterDockBrowserJsExecuteSink({});
    blog(LOG_INFO, "[aegis-obs-shim] browser dock host scaffold disabled (build flag off)");
}

void ShutdownBrowserDockHostBridge() {
    SetDockBrowserJsExecuteSink({});
}
#endif

void LogSceneSnapshot(const char* reason) {
    const auto names = SnapshotSceneNames();
    const std::string current = CurrentSceneName();
    const std::string dock_payload_json =
        BuildDockSceneSnapshotPayloadJson(reason, names, current);

    blog(
        LOG_INFO,
        "[aegis-obs-shim] obs scene snapshot: reason=%s current=\"%s\" count=%d",
        reason ? reason : "unknown",
        current.empty() ? "" : current.c_str(),
        static_cast<int>(names.size()));
    if (!EmitDockSceneSnapshotPayload(dock_payload_json)) {
        const char* phase = nullptr;
        std::uint32_t attempt = 0;
        if (ShouldLogDockFallbackPayload(
                DockFallbackLogKind::SceneSnapshotJson, &phase, &attempt)) {
            blog(
                LOG_INFO,
                "[aegis-obs-shim] dock bridge fallback payload phase=%s attempt=%u setObsSceneSnapshot=%s",
                phase ? phase : "unknown",
                attempt,
                dock_payload_json.c_str());
        }
    }

    for (size_t i = 0; i < names.size(); ++i) {
        blog(LOG_DEBUG, "[aegis-obs-shim] scene[%d]=\"%s\"", static_cast<int>(i), names[i].c_str());
    }
}

const char* FrontendEventName(enum obs_frontend_event event) {
    switch (event) {
    case OBS_FRONTEND_EVENT_SCENE_CHANGED:
        return "SCENE_CHANGED";
    case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
        return "SCENE_LIST_CHANGED";
    case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
        return "SCENE_COLLECTION_CHANGED";
    case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
        return "SCENE_COLLECTION_CHANGING";
    case OBS_FRONTEND_EVENT_FINISHED_LOADING:
        return "FINISHED_LOADING";
    case OBS_FRONTEND_EVENT_THEME_CHANGED:
        return "THEME_CHANGED";
    case OBS_FRONTEND_EVENT_EXIT:
        return "EXIT";
    default:
        return nullptr;
    }
}

void OnFrontendEvent(enum obs_frontend_event event, void*) {
    const char* event_name = FrontendEventName(event);
    if (!event_name) {
        return;
    }

    blog(LOG_INFO, "[aegis-obs-shim] frontend event: %s", event_name);

    switch (event) {
    case OBS_FRONTEND_EVENT_SCENE_CHANGED:
    case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
    case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
    case OBS_FRONTEND_EVENT_FINISHED_LOADING:
        LogSceneSnapshot(event_name);
        RefreshCachedObsDockThemeFromQt(event_name);
        break;
    case OBS_FRONTEND_EVENT_THEME_CHANGED:
        RefreshCachedObsDockThemeFromQt(event_name);
        ReemitDockStatusSnapshotWithCurrentTheme(event_name);
        break;
    case OBS_FRONTEND_EVENT_EXIT:
        // OBS shutdown is in progress; by module unload time the frontend callback
        // registry may already be gone. Avoid a noisy remove callback warning.
        g_frontend_exit_seen = true;
        // Tear down the browser dock host early while frontend/obs-browser are still in a
        // healthier state, rather than waiting until module unload during shutdown.
        ShutdownBrowserDockHostBridge();
        break;
    default:
        break;
    }
}

void OnToolsMenuShowDock(void*) {
#if defined(AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST)
    const bool ok = aegis_obs_browser_dock_host_scaffold_show_dock();
    blog(
        ok ? LOG_INFO : LOG_WARNING,
        "[aegis-obs-shim] tools menu action: show dock -> %s",
        ok ? "ok" : "no_dock_widget");
#else
    blog(LOG_WARNING, "[aegis-obs-shim] tools menu action: show dock unavailable (dock host disabled)");
#endif
}

bool IsCurrentSceneName(const std::string& expected_scene_name) {
    if (expected_scene_name.empty()) {
        return false;
    }
    return expected_scene_name == CurrentSceneName();
}

bool IsDockUiActionReason(const std::string& reason) {
    return reason == "dock_ui";
}

void HandleSwitchSceneRequestOnObsThread(
    const std::string& request_id,
    const std::string& scene_name,
    const std::string& reason) {
    if (scene_name.empty()) {
        blog(
            LOG_WARNING,
            "[aegis-obs-shim] switch_scene request missing scene_name (request_id=%s reason=%s)",
            request_id.c_str(),
            reason.c_str());
        if (!request_id.empty()) {
            g_runtime.QueueSceneSwitchResult(request_id, false, "missing_scene_name");
        }
        if (!request_id.empty() && IsDockUiActionReason(reason)) {
            EmitDockActionResult(
                "switch_scene", request_id, "failed", false, "missing_scene_name", "scene_name missing");
        }
        EmitDockSceneSwitchCompleted(request_id, scene_name, false, "missing_scene_name", reason);
        return;
    }

    obs_source_t* scene_source = obs_get_source_by_name(scene_name.c_str());
    if (!scene_source) {
        blog(
            LOG_WARNING,
            "[aegis-obs-shim] switch_scene target not found: request_id=%s scene=%s reason=%s",
            request_id.c_str(),
            scene_name.c_str(),
            reason.c_str());
        if (!request_id.empty()) {
            g_runtime.QueueSceneSwitchResult(request_id, false, "scene_not_found");
        }
        if (!request_id.empty() && IsDockUiActionReason(reason)) {
            EmitDockActionResult(
                "switch_scene", request_id, "failed", false, "scene_not_found", "");
        }
        EmitDockSceneSwitchCompleted(request_id, scene_name, false, "scene_not_found", reason);
        return;
    }

    blog(
        LOG_INFO,
        "[aegis-obs-shim] switch_scene applying: request_id=%s scene=%s reason=%s",
        request_id.c_str(),
        scene_name.c_str(),
        reason.c_str());

    obs_frontend_set_current_scene(scene_source);
    obs_source_release(scene_source);

    if (!request_id.empty()) {
        if (IsCurrentSceneName(scene_name)) {
            g_runtime.QueueSceneSwitchResult(request_id, true, "");
            CacheDockCurrentSceneForReplay(scene_name);
            EmitDockNativeCurrentScene(scene_name);
            if (IsDockUiActionReason(reason)) {
                EmitDockActionResult(
                    "switch_scene", request_id, "completed", true, "", "scene_switch_applied");
            }
            EmitDockSceneSwitchCompleted(request_id, scene_name, true, "", reason);
        } else {
            blog(
                LOG_WARNING,
                "[aegis-obs-shim] switch_scene verify failed: request_id=%s scene=%s reason=%s",
                request_id.c_str(),
                scene_name.c_str(),
                reason.c_str());
            g_runtime.QueueSceneSwitchResult(request_id, false, "switch_verify_failed");
            if (IsDockUiActionReason(reason)) {
                EmitDockActionResult(
                    "switch_scene", request_id, "failed", false, "switch_verify_failed", "");
            }
            EmitDockSceneSwitchCompleted(request_id, scene_name, false, "switch_verify_failed", reason);
        }
    }
}

void EnqueueSwitchSceneRequest(
    const std::string& request_id,
    const std::string& scene_name,
    const std::string& reason) {
    std::lock_guard<std::mutex> lock(g_pending_switch_requests_mu);
    g_pending_switch_requests.push_back(PendingSwitchRequest{request_id, scene_name, reason});
}

void DrainSwitchSceneRequestsOnObsThread() {
    std::vector<PendingSwitchRequest> pending;
    {
        std::lock_guard<std::mutex> lock(g_pending_switch_requests_mu);
        if (g_pending_switch_requests.empty()) {
            return;
        }
        pending.swap(g_pending_switch_requests);
    }

    for (const auto& req : pending) {
        HandleSwitchSceneRequestOnObsThread(req.request_id, req.scene_name, req.reason);
    }
}

void SwitchScenePumpTick(void*, float seconds) {
    if (seconds > 0.0f) {
        g_switch_pump_accum_seconds += seconds;
        g_theme_poll_accum_seconds += seconds;
    }
    DrainExpiredPendingDockActions();
    if (g_theme_poll_accum_seconds >= 0.5f) {
        g_theme_poll_accum_seconds = 0.0f;
        PollObsThemeChangesOnObsThread();
    }
    if (g_switch_pump_accum_seconds < 0.05f) {
        return;
    }
    g_switch_pump_accum_seconds = 0.0f;
    DrainSwitchSceneRequestsOnObsThread();
}

} // namespace

extern "C" void aegis_obs_shim_register_dock_js_executor(
    aegis_dock_js_execute_fn fn,
    void* user_data) {
#if defined(AEGIS_OBS_PLUGIN_BUILD)
    if (!fn) {
        RegisterDockBrowserJsExecuteSink({});
        return;
    }
    RegisterDockBrowserJsExecuteSink(
        [fn, user_data](const std::string& js_code) -> bool {
            return fn(js_code.c_str(), user_data);
        });
#else
    (void)fn;
    (void)user_data;
#endif
}

extern "C" void aegis_obs_shim_clear_dock_js_executor(void) {
#if defined(AEGIS_OBS_PLUGIN_BUILD)
    RegisterDockBrowserJsExecuteSink({});
#endif
}

extern "C" void aegis_obs_shim_replay_dock_state(void) {
#if defined(AEGIS_OBS_PLUGIN_BUILD)
    ReplayDockStateToJsSinkIfAvailable();
#endif
}

extern "C" void aegis_obs_shim_notify_dock_page_ready(void) {
#if defined(AEGIS_OBS_PLUGIN_BUILD)
    {
        std::lock_guard<std::mutex> lock(g_dock_js_delivery_validation_mu);
        g_dock_js_delivery_validation.page_ready = true;
        g_dock_js_delivery_validation.logged_receive_ipc_envelope_json = false;
        g_dock_js_delivery_validation.logged_receive_scene_snapshot_json = false;
        g_dock_js_delivery_validation.logged_receive_scene_switch_completed_json = false;
        g_dock_js_delivery_validation.logged_receive_dock_action_result_json = false;
        g_dock_js_delivery_validation.fallback_pipe_status_count = 0;
        g_dock_js_delivery_validation.fallback_ipc_envelope_count = 0;
        g_dock_js_delivery_validation.fallback_scene_snapshot_count = 0;
        g_dock_js_delivery_validation.fallback_scene_switch_completed_count = 0;
        g_dock_js_delivery_validation.fallback_dock_action_result_count = 0;
    }
    ReplayDockStateToJsSinkIfAvailable();
    g_runtime.QueueRequestStatus();
    MaybeRunDockActionSelfTestAfterPageReady();
#endif
}

extern "C" void aegis_obs_shim_notify_dock_page_unloaded(void) {
#if defined(AEGIS_OBS_PLUGIN_BUILD)
    {
        std::lock_guard<std::mutex> lock(g_dock_js_delivery_validation_mu);
        g_dock_js_delivery_validation.page_ready = false;
    }
    RegisterDockBrowserJsExecuteSink({});
#endif
}

extern "C" bool aegis_obs_shim_receive_dock_action_json(const char* action_json_utf8) {
#if defined(AEGIS_OBS_PLUGIN_BUILD)
    if (!action_json_utf8 || *action_json_utf8 == '\0') {
        EmitDockActionResult("", "", "rejected", false, "empty_action_json", "");
        return false;
    }

    const std::string action_json(action_json_utf8);
    std::string action_type;
    if (!TryExtractJsonStringField(action_json, "type", &action_type) || action_type.empty()) {
        blog(LOG_WARNING, "[aegis-obs-shim] dock action parse rejected: missing type");
        EmitDockActionResult("", "", "rejected", false, "missing_action_type", "");
        return false;
    }

    std::string request_id;
    (void)TryExtractJsonStringField(action_json, "requestId", &request_id);
    if (request_id.empty()) {
        (void)TryExtractJsonStringField(action_json, "request_id", &request_id);
    }
    blog(
        LOG_INFO,
        "[aegis-obs-shim] dock action parse: type=%s request_id=%s bytes=%d",
        action_type.c_str(),
        request_id.empty() ? "" : request_id.c_str(),
        static_cast<int>(action_json.size()));

    auto ensure_request_id = [&request_id]() {
        if (!request_id.empty()) {
            return;
        }
        const auto now_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        {
            std::lock_guard<std::mutex> lock(g_pending_switch_requests_mu);
            g_local_dock_action_seq += 1;
            request_id = "dock_" + std::to_string(now_ms) + "_" + std::to_string(g_local_dock_action_seq);
        }
    };
    ensure_request_id();
    if (ShouldDeduplicateDockActionByRequestId(action_type, request_id)) {
        blog(
            LOG_DEBUG,
            "[aegis-obs-shim] dock action deduplicated: type=%s request_id=%s",
            action_type.c_str(),
            request_id.c_str());
        return true;
    }

    if (action_type == "switch_scene") {
        std::string scene_name;
        (void)TryExtractJsonStringField(action_json, "sceneName", &scene_name);
        if (scene_name.empty()) {
            (void)TryExtractJsonStringField(action_json, "scene_name", &scene_name);
        }
        if (scene_name.empty()) {
            blog(
                LOG_WARNING,
                "[aegis-obs-shim] dock action rejected: type=switch_scene request_id=%s error=missing_scene_name",
                request_id.c_str());
            EmitDockActionResult(action_type, request_id, "rejected", false, "missing_scene_name", "");
            return false;
        }

        blog(
            LOG_INFO,
            "[aegis-obs-shim] dock action queued: type=switch_scene request_id=%s scene=%s",
            request_id.c_str(),
            scene_name.c_str());
        EnqueueSwitchSceneRequest(request_id, scene_name, "dock_ui");
        EmitDockActionResult(action_type, request_id, "queued", true, "", "queued_for_obs_thread");
        return true;
    }

    if (action_type == "request_status") {
        blog(
            LOG_INFO,
            "[aegis-obs-shim] dock action queued: type=request_status request_id=%s",
            request_id.c_str());
        TrackPendingDockRequestStatusAction(request_id);
        g_runtime.QueueRequestStatus();
        EmitDockActionResult(action_type, request_id, "queued", true, "", "queued_request_status");
        return true;
    }

    if (action_type == "set_mode") {
        std::string mode;
        (void)TryExtractJsonStringField(action_json, "mode", &mode);
        if (!IsRecognizedDockMode(mode)) {
            blog(
                LOG_WARNING,
                "[aegis-obs-shim] dock action rejected: type=set_mode request_id=%s mode=%s error=invalid_mode",
                request_id.c_str(),
                mode.empty() ? "" : mode.c_str());
            EmitDockActionResult(action_type, request_id, "rejected", false, "invalid_mode", "");
            return false;
        }
        blog(
            LOG_INFO,
            "[aegis-obs-shim] dock action queued: type=set_mode request_id=%s mode=%s detail=queued_core_ipc",
            request_id.c_str(),
            mode.c_str());
        TrackPendingDockSetModeAction(request_id, mode);
        g_runtime.QueueSetModeRequest(mode);
        EmitDockActionResult(action_type, request_id, "queued", true, "", "queued_core_ipc");
        return true;
    }

    if (action_type == "set_setting") {
        std::string key;
        bool value = false;
        const bool has_value = TryExtractJsonBoolField(action_json, "value", &value);
        (void)TryExtractJsonStringField(action_json, "key", &key);
        if (key.empty()) {
            blog(
                LOG_WARNING,
                "[aegis-obs-shim] dock action rejected: type=set_setting request_id=%s error=missing_setting_key",
                request_id.c_str());
            EmitDockActionResult(action_type, request_id, "rejected", false, "missing_setting_key", "");
            return false;
        }
        if (!has_value) {
            blog(
                LOG_WARNING,
                "[aegis-obs-shim] dock action rejected: type=set_setting request_id=%s key=%s error=missing_setting_value",
                request_id.c_str(),
                key.c_str());
            EmitDockActionResult(action_type, request_id, "rejected", false, "missing_setting_value", "");
            return false;
        }
        if (!IsRecognizedDockSettingKey(key)) {
            blog(
                LOG_WARNING,
                "[aegis-obs-shim] dock action rejected: type=set_setting request_id=%s key=%s error=unsupported_setting_key",
                request_id.c_str(),
                key.c_str());
            EmitDockActionResult(
                action_type, request_id, "rejected", false, "unsupported_setting_key", key);
            return false;
        }
        blog(
            LOG_INFO,
            "[aegis-obs-shim] dock action queued: type=set_setting request_id=%s key=%s value=%s detail=queued_core_ipc",
            request_id.c_str(),
            key.c_str(),
            value ? "true" : "false");
        TrackPendingDockSetSettingAction(request_id, key, value);
        g_runtime.QueueSetSettingRequest(key, value);
        EmitDockActionResult(action_type, request_id, "queued", true, "", "queued_core_ipc");
        return true;
    }

    blog(
        LOG_INFO,
        "[aegis-obs-shim] dock action rejected: type=%s request_id=%s error=unsupported_action_type",
        action_type.c_str(),
        request_id.empty() ? "" : request_id.c_str());
    EmitDockActionResult(action_type, request_id, "rejected", false, "unsupported_action_type", "");
    return false;
#else
    (void)action_json_utf8;
    return false;
#endif
}

bool obs_module_load(void) {
    blog(LOG_INFO, "[aegis-obs-shim] module load");

    if (!g_obs_timer_registered) {
        obs_add_tick_callback(SwitchScenePumpTick, nullptr);
        g_obs_timer_registered = true;
        g_switch_pump_accum_seconds = 0.0f;
        g_theme_poll_accum_seconds = 0.0f;
        blog(LOG_INFO, "[aegis-obs-shim] registered switch-scene pump timer");
    }

    // Browser dock host/CEF embedding is still pending. These pluggable sinks let a future
    // Qt/CEF layer execute JS against the dock page and reuse the existing plugin callbacks.
    SetDockSceneSnapshotEmitter({});
    InitializeBrowserDockHostBridge();
    g_frontend_exit_seen = false;

    g_runtime.SetLogger([](const std::string& msg) {
        const bool noisy_frame =
            (msg.find("received frame type=status_snapshot") != std::string::npos) ||
            (msg.find("received frame type=pong") != std::string::npos);
        blog(noisy_frame ? LOG_DEBUG : LOG_INFO, "[aegis-shim] %s", msg.c_str());
    });

    g_runtime.SetAutoAckSwitchScene(false);
    aegis::ShimRuntime::IpcCallbacks callbacks{};
    callbacks.on_pipe_state = [](bool connected) {
        blog(
            LOG_INFO,
            "[aegis-obs-shim] ipc pipe state: %s",
            connected ? "connected" : "disconnected");
        CacheDockPipeStatusForReplay(
            connected ? "ok" : "down",
            connected ? "IPC connected" : "IPC disconnected");
        if (!EmitDockNativePipeStatus(connected ? "ok" : "down",
                                      connected ? "IPC connected" : "IPC disconnected")) {
            const char* phase = nullptr;
            std::uint32_t attempt = 0;
            if (ShouldLogDockFallbackPayload(DockFallbackLogKind::PipeStatus, &phase, &attempt)) {
                blog(
                    LOG_DEBUG,
                    "[aegis-obs-shim] dock bridge fallback pipe status phase=%s attempt=%u status=%s",
                    phase ? phase : "unknown",
                    attempt,
                    connected ? "ok" : "down");
            }
        }
    };
    callbacks.on_message_type = [](const std::string& type) {
        blog(LOG_DEBUG, "[aegis-obs-shim] ipc message type=%s", type.c_str());
    };
    callbacks.on_incoming_envelope_json = [](const std::string& envelope_json) {
        EmitDockIpcEnvelopeJson(envelope_json);
    };
    callbacks.on_switch_scene_request = [](const std::string& request_id,
                                           const std::string& scene_name,
                                           const std::string& reason) {
        EnqueueSwitchSceneRequest(request_id, scene_name, reason);
    };
    g_runtime.SetIpcCallbacks(std::move(callbacks));

    if (!g_frontend_event_callback_registered) {
        obs_frontend_add_event_callback(OnFrontendEvent, nullptr);
        g_frontend_event_callback_registered = true;
        blog(LOG_INFO, "[aegis-obs-shim] registered frontend event callback");
    }
    if (!g_tools_menu_show_dock_registered) {
        obs_frontend_add_tools_menu_item("Show Aegis Dock (Telemy)", OnToolsMenuShowDock, nullptr);
        g_tools_menu_show_dock_registered = true;
        blog(LOG_INFO, "[aegis-obs-shim] registered Tools menu item: Show Aegis Dock (Telemy)");
    }
    LogSceneSnapshot("module_load");

    g_runtime.Start();
    return true;
}

void obs_module_unload(void) {
    blog(LOG_INFO, "[aegis-obs-shim] module unload");
    if (g_frontend_event_callback_registered) {
        if (!g_frontend_exit_seen) {
            obs_frontend_remove_event_callback(OnFrontendEvent, nullptr);
        } else {
            blog(
                LOG_INFO,
                "[aegis-obs-shim] skipping frontend callback remove after EXIT event");
        }
        g_frontend_event_callback_registered = false;
    }
    g_tools_menu_show_dock_registered = false;
    if (g_obs_timer_registered) {
        obs_remove_tick_callback(SwitchScenePumpTick, nullptr);
        g_obs_timer_registered = false;
        g_switch_pump_accum_seconds = 0.0f;
        g_theme_poll_accum_seconds = 0.0f;
    }
    {
        std::lock_guard<std::mutex> lock(g_pending_switch_requests_mu);
        g_pending_switch_requests.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_pending_request_status_action_ids_mu);
        g_pending_request_status_action_ids.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_pending_set_mode_actions_mu);
        g_pending_set_mode_actions.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_pending_set_setting_actions_mu);
        g_pending_set_setting_actions.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_recent_dock_actions_mu);
        g_recent_dock_actions.clear();
    }
    g_dock_action_selftest_attempted = false;
    SetDockSceneSnapshotEmitter({});
    ShutdownBrowserDockHostBridge();
    ClearDockReplayCache();
    g_runtime.QueueObsShutdownNotice("obs_module_unload");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    g_runtime.Stop();
}

const char* obs_module_description(void) {
    return "Aegis OBS plugin shim (v0.0.3 skeleton)";
}

#else
int aegis_obs_plugin_entry_placeholder() {
    return 0;
}
#endif
