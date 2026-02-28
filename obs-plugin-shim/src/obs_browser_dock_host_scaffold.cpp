#include "obs_browser_dock_host_scaffold.h"
#include "dock_js_bridge_api.h"

#if defined(AEGIS_OBS_PLUGIN_BUILD) && defined(AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST)

#include <obs-module.h>
#include <obs-frontend-api.h>

#if defined(AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_OBS_CEF)
#include <QtCore/QByteArray>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QMainWindow>
#include <mutex>
#include <string>
#include <vector>
#include <array>
#include <browser-panel.hpp>
#endif

#if defined(AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_QT_WEBENGINE) || defined(AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_OBS_CEF)
#include <QtCore/QMetaObject>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QUrl>
#include <QtCore/QVariant>
#include <QtCore/QDir>
#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#if defined(AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_QT_WEBENGINE)
#include <QtWebEngineCore/QWebEnginePage>
#include <QtWebEngineWidgets/QWebEngineView>
#endif
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QWidget>
#include <mutex>
#include <string>
#include <vector>
#include <array>
#if defined(_WIN32)
#include <windows.h>
#endif
#endif

namespace {

constexpr const char* kDockId = "aegis_obs_core_ipc_dock";
constexpr const char* kDockTitle = "Aegis Dock (Telemy v0.0.3)";
constexpr const char* kEnvDockBridgeRoot = "AEGIS_DOCK_BRIDGE_ROOT";
constexpr const char* kDockActionTitlePrefix = "__AEGIS_DOCK_ACTION__:";
constexpr const char* kDockReadyTitlePrefix = "__AEGIS_DOCK_READY__:";

#if defined(AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_QT_WEBENGINE) || defined(AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_OBS_CEF)

constexpr const char* kDockValidationBootstrapHtml = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8" />
  <meta http-equiv="Content-Security-Policy" content="default-src 'self' 'unsafe-inline' data: blob:;">
  <title>Aegis Dock Host Bootstrap</title>
  <style>
    html, body { margin: 0; padding: 0; background: #101820; color: #d9e2ec; font-family: Consolas, monospace; }
    #root { padding: 10px; }
    #status { font-weight: 700; margin-bottom: 8px; }
    #log { white-space: pre-wrap; font-size: 12px; line-height: 1.35; opacity: 0.92; }
  </style>
</head>
<body>
  <div id="root">
    <div id="status">Aegis Dock Host Bootstrap (validation page)</div>
    <div id="log">waiting...</div>
  </div>
  <script>
    (function () {
      const logEl = document.getElementById("log");
      const lines = [];
      function log(msg) {
        const line = "[" + new Date().toISOString() + "] " + String(msg);
        lines.push(line);
        while (lines.length > 40) lines.shift();
        if (logEl) logEl.textContent = lines.join("\\n");
        try { console.log("[aegis-dock-host]", msg); } catch (_e) {}
      }
      function ok(name, payload) {
        log(name + ": " + payload);
        return true;
      }
      window.aegisDockNative = {
        receiveSceneSnapshotJson(jsonText) { return ok("receiveSceneSnapshotJson", jsonText); },
        receiveIpcEnvelopeJson(jsonText) { return ok("receiveIpcEnvelopeJson", jsonText); },
        receivePipeStatus(status, reason) { return ok("receivePipeStatus", JSON.stringify({ status, reason })); },
        receiveCurrentScene(sceneName) { return ok("receiveCurrentScene", JSON.stringify(sceneName)); },
        receiveSceneSwitchCompletedJson(jsonText) { return ok("receiveSceneSwitchCompletedJson", jsonText); },
        receiveDockActionResultJson(jsonText) { return ok("receiveDockActionResultJson", jsonText); }
      };
      try {
        window.dispatchEvent(new CustomEvent("aegis:dock:native-ready", { detail: { ok: true } }));
      } catch (_e) {}
      log("window.aegisDockNative ready");
    })();
  </script>
</body>
</html>
)HTML";

struct DockBridgeAssets {
    bool complete = false;
    bool uses_scaffold_fallback = false;
    QString dock_html_path;
    QString dock_app_js_path;
    QString bridge_js_path;
    QString bridge_host_js_path;
    QString browser_bootstrap_js_path;
    QString dock_html;
    QString dock_app_js;
    QString bridge_js;
    QString bridge_host_js;
    QString browser_bootstrap_js;
    QString resolution_note;
};

#if defined(AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_QT_WEBENGINE)
struct QtDockJsExecutorState {
    std::mutex mu;
    QPointer<QWebEnginePage> page;
};

struct QtDockHostState {
    QPointer<QDockWidget> dock_widget;
    QPointer<QWebEngineView> web_view;
    bool dock_registered = false;
    bool page_ready_notified = false;
};

QtDockJsExecutorState g_qt_executor_state;
QtDockHostState g_qt_dock_state;
#endif

QString HtmlScriptSafe(QString text) {
    text.replace(QStringLiteral("</script"), QStringLiteral("<\\/script"), Qt::CaseInsensitive);
    return text;
}

bool ReadTextFileUtf8(const QString& path, QString* out_text) {
    if (!out_text) {
        return false;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray bytes = f.readAll();
    *out_text = QString::fromUtf8(bytes.constData(), static_cast<int>(bytes.size()));
    return true;
}

#if defined(_WIN32)
QString CurrentPluginModuleDir() {
    HMODULE module_handle = nullptr;
    const auto ok = GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&aegis_obs_browser_dock_host_scaffold_initialize),
        &module_handle);
    if (!ok || !module_handle) {
        return {};
    }

    std::array<wchar_t, 4096> buf{};
    const DWORD len = GetModuleFileNameW(module_handle, buf.data(), static_cast<DWORD>(buf.size()));
    if (len == 0 || len >= buf.size()) {
        return {};
    }
    return QFileInfo(QString::fromWCharArray(buf.data(), static_cast<int>(len))).absolutePath();
}
#else
QString CurrentPluginModuleDir() {
    return {};
}
#endif

std::vector<QString> CandidateDockAssetRoots() {
    std::vector<QString> roots;

    const QString env_root = qEnvironmentVariable(kEnvDockBridgeRoot);
    if (!env_root.isEmpty()) {
        roots.push_back(QDir::fromNativeSeparators(env_root));
    }

    const std::array<const char*, 3> module_asset_dirs = {
        ".",
        "data",
        "../../data/obs-plugins/aegis-obs-shim",
    };
    for (const char* rel : module_asset_dirs) {
        char* raw = obs_module_file(rel);
        if (!raw) {
            continue;
        }
        const QString path = QDir::fromNativeSeparators(QString::fromUtf8(raw));
        bfree(raw);
        if (!path.isEmpty()) {
            roots.push_back(path);
        }
    }

    const QString module_dir = CurrentPluginModuleDir();
    if (!module_dir.isEmpty()) {
        QDir dir(module_dir);
        for (int i = 0; i < 10; ++i) {
            roots.push_back(QDir::fromNativeSeparators(dir.absolutePath()));
            if (!dir.cdUp()) {
                break;
            }
        }
    }

    const QString app_dir = QCoreApplication::applicationDirPath();
    if (!app_dir.isEmpty()) {
        roots.push_back(QDir::fromNativeSeparators(app_dir));
    }

    return roots;
}

bool TryResolveDockAssetInExplicitEnvRoot(const char* file_name, QString* out_path) {
    if (!file_name || !out_path) {
        return false;
    }
    const QString env_root = qEnvironmentVariable(kEnvDockBridgeRoot);
    if (env_root.isEmpty()) {
        return false;
    }
    const QString root = QDir::fromNativeSeparators(env_root);
    const QString candidate = QDir(root).filePath(QString::fromUtf8(file_name));
    if (QFileInfo::exists(candidate) && QFileInfo(candidate).isFile()) {
        *out_path = QDir::fromNativeSeparators(QFileInfo(candidate).absoluteFilePath());
        return true;
    }
    return false;
}

bool TryResolveDockAsset(const char* file_name, QString* out_path) {
    if (!file_name || !out_path) {
        return false;
    }

    for (const QString& root : CandidateDockAssetRoots()) {
        if (root.isEmpty()) {
            continue;
        }
        const QString candidate = QDir(root).filePath(QString::fromUtf8(file_name));
        if (QFileInfo::exists(candidate) && QFileInfo(candidate).isFile()) {
            *out_path = QDir::fromNativeSeparators(QFileInfo(candidate).absoluteFilePath());
            return true;
        }
    }
    return false;
}

bool TryExtractJsonStringFieldQt(
    const QString& json_text,
    const QString& field_name,
    QString* out_value) {
    if (field_name.isEmpty() || !out_value) {
        return false;
    }
    const QString needle = QStringLiteral("\"") + field_name + QStringLiteral("\"");
    const int key_pos = json_text.indexOf(needle);
    if (key_pos < 0) {
        return false;
    }
    int colon_pos = json_text.indexOf(QChar(':'), key_pos + needle.size());
    if (colon_pos < 0) {
        return false;
    }
    int pos = colon_pos + 1;
    while (pos < json_text.size() && json_text.at(pos).isSpace()) {
        pos += 1;
    }
    if (pos >= json_text.size() || json_text.at(pos) != QChar('"')) {
        return false;
    }
    pos += 1;

    QString value;
    bool escaping = false;
    for (; pos < json_text.size(); ++pos) {
        const QChar ch = json_text.at(pos);
        if (escaping) {
            value.append(ch);
            escaping = false;
            continue;
        }
        if (ch == QChar('\\')) {
            escaping = true;
            continue;
        }
        if (ch == QChar('"')) {
            *out_value = value;
            return true;
        }
        value.append(ch);
    }
    return false;
}

bool TryHandleDockActionPrefixedMessage(const char* source_tag, const QString& prefixed_text) {
    if (prefixed_text.isEmpty()) {
        return false;
    }
    const QString strict_prefix = QString::fromUtf8(kDockActionTitlePrefix);
    QString encoded;
    const int strict_pos = prefixed_text.indexOf(strict_prefix, 0, Qt::CaseInsensitive);
    if (strict_pos >= 0) {
        encoded = prefixed_text.mid(strict_pos + strict_prefix.size());
    } else {
        const int token_pos =
            prefixed_text.indexOf(QStringLiteral("aegis_dock_action"), 0, Qt::CaseInsensitive);
        if (token_pos < 0) {
            return false;
        }
        const int colon_pos = prefixed_text.indexOf(QChar(':'), token_pos);
        if (colon_pos < 0) {
            return false;
        }
        encoded = prefixed_text.mid(colon_pos + 1);
    }
    while (!encoded.isEmpty()) {
        const QChar ch = encoded.at(0);
        if (ch == QChar('#') || ch == QChar('_') || ch == QChar(':') || ch.isSpace()) {
            encoded.remove(0, 1);
            continue;
        }
        break;
    }
    if (encoded.isEmpty()) {
        blog(
            LOG_DEBUG,
            "[aegis-obs-shim] browser dock scaffold action %s parse skipped: empty encoded payload",
            source_tag ? source_tag : "unknown");
        return false;
    }
    if (!(encoded.startsWith(QStringLiteral("%7B"), Qt::CaseInsensitive) ||
          encoded.startsWith(QStringLiteral("%257B"), Qt::CaseInsensitive) ||
          encoded.startsWith(QStringLiteral("{")))) {
        const QByteArray encoded_bytes = encoded.left(160).toUtf8();
        blog(
            LOG_DEBUG,
            "[aegis-obs-shim] browser dock scaffold action %s parse skipped: unexpected prefix sample=%s",
            source_tag ? source_tag : "unknown",
            encoded_bytes.constData());
        return false;
    }

    QString decoded_text = encoded;
    for (int i = 0; i < 3; ++i) {
        const QString pass = QUrl::fromPercentEncoding(decoded_text.toUtf8());
        if (pass == decoded_text) {
            break;
        }
        decoded_text = pass;
        if (decoded_text.startsWith(QChar('{')) && decoded_text.contains(QStringLiteral("\"type\""))) {
            break;
        }
    }

    const QByteArray decoded_bytes = decoded_text.toUtf8();
    if (decoded_bytes.isEmpty()) {
        blog(
            LOG_WARNING,
            "[aegis-obs-shim] browser dock scaffold action %s decode failed",
            source_tag ? source_tag : "unknown");
        return true;
    }

    QString action_type;
    QString request_id;
    (void)TryExtractJsonStringFieldQt(decoded_text, QStringLiteral("type"), &action_type);
    if (!TryExtractJsonStringFieldQt(decoded_text, QStringLiteral("requestId"), &request_id)) {
        (void)TryExtractJsonStringFieldQt(decoded_text, QStringLiteral("request_id"), &request_id);
    }
    blog(
        LOG_INFO,
        "[aegis-obs-shim] browser dock scaffold action %s decode: type=%s request_id=%s bytes=%d",
        source_tag ? source_tag : "unknown",
        action_type.isEmpty() ? "" : action_type.toUtf8().constData(),
        request_id.isEmpty() ? "" : request_id.toUtf8().constData(),
        static_cast<int>(decoded_bytes.size()));

    const bool ok = aegis_obs_shim_receive_dock_action_json(decoded_bytes.constData());
    blog(
        ok ? LOG_INFO : LOG_DEBUG,
        "[aegis-obs-shim] browser dock scaffold dock action %s forwarded ok=%s type=%s request_id=%s bytes=%d",
        source_tag ? source_tag : "unknown",
        ok ? "true" : "false",
        action_type.isEmpty() ? "" : action_type.toUtf8().constData(),
        request_id.isEmpty() ? "" : request_id.toUtf8().constData(),
        static_cast<int>(decoded_bytes.size()));
    return true;
}

bool TryHandleDockActionTitleMessage(const QString& title_text) {
    return TryHandleDockActionPrefixedMessage("title", title_text);
}

bool ContainsDockReadySignal(const QString& text) {
    if (text.isEmpty()) {
        return false;
    }
    const QString marker = QString::fromUtf8(kDockReadyTitlePrefix);
    return text.contains(marker, Qt::CaseInsensitive);
}

bool TryHandleDockActionUrlMessage(const QString& url_text) {
    if (url_text.isEmpty()) {
        return false;
    }

    const QString prefix = QString::fromUtf8(kDockActionTitlePrefix);
    const QString hash_prefix = QStringLiteral("#") + prefix;
    QString candidate;

    const int hash_prefix_pos = url_text.lastIndexOf(hash_prefix);
    if (hash_prefix_pos >= 0) {
        candidate = url_text.mid(hash_prefix_pos + 1);
    }

    if (candidate.isEmpty() && !url_text.startsWith(QStringLiteral("data:"), Qt::CaseInsensitive)) {
        const QUrl parsed(url_text);
        const QString fragment = parsed.fragment(QUrl::FullyEncoded);
        if (!fragment.isEmpty()) {
            if (fragment.startsWith(prefix)) {
                candidate = fragment;
            } else if (fragment.startsWith(QStringLiteral("/") + prefix)) {
                candidate = fragment.mid(1);
            }
        }
    }

    return TryHandleDockActionPrefixedMessage("url", candidate);
}

DockBridgeAssets LoadDockBridgeAssets() {
    DockBridgeAssets assets;

    // Keep the asset stack consistent when AEGIS_DOCK_BRIDGE_ROOT is set: prefer that root for all
    // bridge assets before falling back to staged/module-data files.
    const bool env_bridge_found =
        TryResolveDockAssetInExplicitEnvRoot("aegis-dock-bridge.js", &assets.bridge_js_path) ||
        TryResolveDockAssetInExplicitEnvRoot("aegis-dock-bridge.global.js", &assets.bridge_js_path);
    if (!env_bridge_found &&
        !TryResolveDockAsset("aegis-dock-bridge.js", &assets.bridge_js_path) &&
        !TryResolveDockAsset("aegis-dock-bridge.global.js", &assets.bridge_js_path)) {
        assets.resolution_note =
            QStringLiteral("bridge core asset not found (preferred: aegis-dock-bridge.js; fallback: aegis-dock-bridge.global.js)");
        return assets;
    }

    const bool env_host_found =
        TryResolveDockAssetInExplicitEnvRoot("aegis-dock-bridge-host.js", &assets.bridge_host_js_path);
    const bool env_bootstrap_found =
        TryResolveDockAssetInExplicitEnvRoot(
            "aegis-dock-browser-host-bootstrap.js", &assets.browser_bootstrap_js_path);

    if ((!env_host_found && !TryResolveDockAsset("aegis-dock-bridge-host.js", &assets.bridge_host_js_path)) ||
        (!env_bootstrap_found &&
         !TryResolveDockAsset("aegis-dock-browser-host-bootstrap.js", &assets.browser_bootstrap_js_path))) {
        assets.resolution_note =
            QStringLiteral("bridge assets not found (set %1 or deploy into module data dir)")
                .arg(QString::fromUtf8(kEnvDockBridgeRoot));
        return assets;
    }

    if (!ReadTextFileUtf8(assets.bridge_js_path, &assets.bridge_js) ||
        !ReadTextFileUtf8(assets.bridge_host_js_path, &assets.bridge_host_js) ||
        !ReadTextFileUtf8(assets.browser_bootstrap_js_path, &assets.browser_bootstrap_js)) {
        assets.resolution_note = QStringLiteral("bridge assets resolved but failed to read");
        return assets;
    }

    if (TryResolveDockAssetInExplicitEnvRoot("aegis-dock.html", &assets.dock_html_path) ||
        TryResolveDockAsset("aegis-dock.html", &assets.dock_html_path)) {
        (void)ReadTextFileUtf8(assets.dock_html_path, &assets.dock_html);
    }
    if (TryResolveDockAssetInExplicitEnvRoot("aegis-dock-app.js", &assets.dock_app_js_path) ||
        TryResolveDockAsset("aegis-dock-app.js", &assets.dock_app_js_path)) {
        (void)ReadTextFileUtf8(assets.dock_app_js_path, &assets.dock_app_js);
    }

    assets.complete = true;
    const QString bridge_core_name = QFileInfo(assets.bridge_js_path).fileName();
    if (!assets.dock_html_path.isEmpty() && !assets.dock_html.isEmpty()) {
        assets.resolution_note =
            QStringLiteral("bridge assets loaded (core=%1, html=ok) from %2 | %3 | %4 | %5")
                .arg(
                    bridge_core_name,
                    assets.bridge_js_path,
                    assets.bridge_host_js_path,
                    assets.browser_bootstrap_js_path,
                    assets.dock_html_path);
    } else if (!assets.dock_html_path.isEmpty()) {
        assets.uses_scaffold_fallback = true;
        assets.resolution_note =
            QStringLiteral("bridge assets loaded (core=%1, html=read_failed -> scaffold_fallback) from %2 | %3 | %4 | %5")
                .arg(
                    bridge_core_name,
                    assets.bridge_js_path,
                    assets.bridge_host_js_path,
                    assets.browser_bootstrap_js_path,
                    assets.dock_html_path);
    } else {
        assets.uses_scaffold_fallback = true;
        assets.resolution_note =
            QStringLiteral("bridge assets loaded (core=%1, html=missing -> scaffold_fallback) from %2 | %3 | %4")
                .arg(
                    bridge_core_name,
                    assets.bridge_js_path,
                    assets.bridge_host_js_path,
                    assets.browser_bootstrap_js_path);
    }
    return assets;
}

QString BuildRealDockPageHtml(const DockBridgeAssets& assets) {
    QString injected_scripts;
    injected_scripts.reserve(
        assets.bridge_js.size() + assets.bridge_host_js.size() + assets.browser_bootstrap_js.size() + 256);
    injected_scripts += QStringLiteral("<script>\n");
    injected_scripts += HtmlScriptSafe(assets.bridge_js);
    injected_scripts += QStringLiteral("\n</script><script>\n");
    injected_scripts += HtmlScriptSafe(assets.bridge_host_js);
    injected_scripts += QStringLiteral("\n</script><script>\n");
    injected_scripts += HtmlScriptSafe(assets.browser_bootstrap_js);
    injected_scripts += QStringLiteral("\n</script>");

    QString dock_app_inline_script;
    if (!assets.dock_app_js.isEmpty()) {
        dock_app_inline_script.reserve(assets.dock_app_js.size() + 32);
        dock_app_inline_script += QStringLiteral("<script>\n");
        dock_app_inline_script += HtmlScriptSafe(assets.dock_app_js);
        dock_app_inline_script += QStringLiteral("\n</script>");
    }

    if (!assets.dock_html.isEmpty()) {
        QString html = assets.dock_html;
        const QString marker = QStringLiteral("<!-- AEGIS_DOCK_HOST_SCRIPTS -->");
        if (html.contains(marker)) {
            html.replace(marker, injected_scripts);
            if (!dock_app_inline_script.isEmpty()) {
                html.replace(QStringLiteral("<script src=\"aegis-dock-app.js\"></script>"), dock_app_inline_script);
            }
            return html;
        }
        const int body_close = html.lastIndexOf(QStringLiteral("</body>"), -1, Qt::CaseInsensitive);
        if (body_close >= 0) {
            html.insert(body_close, injected_scripts);
            if (!dock_app_inline_script.isEmpty()) {
                html.replace(QStringLiteral("<script src=\"aegis-dock-app.js\"></script>"), dock_app_inline_script);
            }
            return html;
        }
        html += injected_scripts;
        if (!dock_app_inline_script.isEmpty()) {
            html.replace(QStringLiteral("<script src=\"aegis-dock-app.js\"></script>"), dock_app_inline_script);
        }
        return html;
    }

    QString html;
    html.reserve(assets.bridge_js.size() + assets.bridge_host_js.size() +
                 assets.browser_bootstrap_js.size() + 8192);
    html += QStringLiteral(
        "<!doctype html><html><head><meta charset=\"utf-8\" />"
        "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'self' 'unsafe-inline' data: blob:;\">"
        "<title>Aegis Dock Host</title>"
        "<style>"
        "html,body{margin:0;padding:0;background:#0e141b;color:#d6e3f0;font-family:Consolas,monospace;}"
        "#app{padding:10px;display:grid;gap:10px;}"
        "#status{font-weight:700;color:#a7d7ff;}"
        "#meta{font-size:12px;opacity:.85;}"
        "#state{background:#0b1015;border:1px solid #22303c;padding:8px;white-space:pre-wrap;min-height:120px;}"
        "#events{background:#0b1015;border:1px solid #22303c;padding:8px;white-space:pre-wrap;min-height:140px;font-size:12px;line-height:1.35;}"
        "</style></head><body>"
        "<div id=\"app\">"
        "<div id=\"status\">Aegis Dock Host (real bridge/bootstrap JS)</div>"
        "<div id=\"meta\"></div>"
        "<div id=\"state\">state pending...</div>"
        "<div id=\"events\">events pending...</div>"
        "</div>");
    html += injected_scripts;
    html += QStringLiteral("<script>\n");
    html += QStringLiteral(
        R"JS(
(function () {
  const statusEl = document.getElementById("status");
  const metaEl = document.getElementById("meta");
  const stateEl = document.getElementById("state");
  const eventsEl = document.getElementById("events");
  const lines = [];
  function log(msg) {
    const line = "[" + new Date().toISOString() + "] " + String(msg);
    lines.push(line);
    while (lines.length > 80) lines.shift();
    if (eventsEl) eventsEl.textContent = lines.join("\n");
    try { console.log("[aegis-dock-host]", msg); } catch (_e) {}
  }
  function renderState() {
    try {
      const s = window.aegisDockNative && window.aegisDockNative.getState
        ? window.aegisDockNative.getState()
        : null;
      if (stateEl) stateEl.textContent = JSON.stringify(s, null, 2);
    } catch (e) {
      if (stateEl) stateEl.textContent = "state render error: " + String(e && e.message || e);
    }
  }
  if (metaEl) {
    metaEl.textContent = "Bridge/host/bootstrap loaded (Qt WebEngine)";
  }
  if (statusEl) {
    statusEl.textContent = "Aegis Dock Host (native bootstrap pending)";
  }
  window.addEventListener("aegis:dock:native-ready", function () {
    if (statusEl) statusEl.textContent = "Aegis Dock Host (native ready)";
    log("native-ready");
    renderState();
  });
  [
    "aegis:dock:ipc-envelope-json",
    "aegis:dock:scene-snapshot-json",
    "aegis:dock:pipe-status",
    "aegis:dock:current-scene",
    "aegis:dock:scene-switch-completed-json"
  ].forEach(function (name) {
    window.addEventListener(name, function (ev) {
      log(name + " " + JSON.stringify(ev && ev.detail || {}));
      renderState();
    });
  });
  renderState();
})();
)JS");
    html += QStringLiteral("\n</script></body></html>");
    return html;
}

#if defined(AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_QT_WEBENGINE)
bool QtDockExecuteJs(const char* js_utf8, void* user_data) {
    auto* state = static_cast<QtDockJsExecutorState*>(user_data);
    if (!state || !js_utf8) {
        return false;
    }

    QPointer<QWebEnginePage> page;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        page = state->page;
    }
    if (!page) {
        return false;
    }

    const QString script = QString::fromUtf8(js_utf8);
    return QMetaObject::invokeMethod(
        page.data(),
        [page, script]() {
            if (!page) {
                return;
            }
            page->runJavaScript(script);
        },
        Qt::QueuedConnection);
}

void SetQtExecutorPage(QWebEnginePage* page) {
    std::lock_guard<std::mutex> lock(g_qt_executor_state.mu);
    g_qt_executor_state.page = page;
}

void ClearQtExecutorPage() {
    std::lock_guard<std::mutex> lock(g_qt_executor_state.mu);
    g_qt_executor_state.page.clear();
}

void ProbeDockNativeReadyAsync() {
    if (!g_qt_dock_state.web_view) {
        return;
    }
    QWebEnginePage* page = g_qt_dock_state.web_view->page();
    if (!page) {
        return;
    }

    page->runJavaScript(
        QStringLiteral(
            "(function(){ return !!(window.aegisDockNative && "
            "typeof window.aegisDockNative.receiveIpcEnvelopeJson === 'function' && "
            "typeof window.aegisDockNative.receiveSceneSnapshotJson === 'function' && "
            "typeof window.aegisDockNative.receivePipeStatus === 'function' && "
            "typeof window.aegisDockNative.receiveCurrentScene === 'function' && "
            "typeof window.aegisDockNative.receiveSceneSwitchCompletedJson === 'function' && "
            "typeof window.aegisDockNative.receiveDockActionResultJson === 'function'); })();"),
        [page](const QVariant& result) {
            const bool ready = result.toBool();
            if (!ready) {
                blog(LOG_DEBUG, "[aegis-obs-shim] browser dock scaffold native-ready probe=false");
                return;
            }
            if (g_qt_dock_state.page_ready_notified) {
                return;
            }
            g_qt_dock_state.page_ready_notified = true;
            blog(LOG_INFO, "[aegis-obs-shim] browser dock scaffold bootstrap/native-ready");
            aegis_obs_browser_dock_host_scaffold_on_page_ready();
            (void)page;
        });
}

QDockWidget* CreateDockWidgetForObsMainWindow() {
    auto* main_window = static_cast<QMainWindow*>(obs_frontend_get_main_window());
    if (!main_window) {
        blog(LOG_WARNING, "[aegis-obs-shim] browser dock scaffold missing OBS main window");
        return nullptr;
    }

    auto* dock = new QDockWidget(QString::fromUtf8(kDockTitle), main_window);
    dock->setObjectName(QString::fromUtf8(kDockId));
    dock->setAllowedAreas(
        Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea | Qt::TopDockWidgetArea |
        Qt::BottomDockWidgetArea);

    auto* view = new QWebEngineView(dock);
    dock->setWidget(view);

    QObject::connect(view, &QWebEngineView::loadFinished, dock, [](bool ok) {
        if (!ok) {
            blog(LOG_WARNING, "[aegis-obs-shim] browser dock scaffold page load failed");
            g_qt_dock_state.page_ready_notified = false;
            aegis_obs_browser_dock_host_scaffold_on_page_unloaded();
            return;
        }
        blog(LOG_INFO, "[aegis-obs-shim] browser dock scaffold page load finished");
        ProbeDockNativeReadyAsync();
    });

    QObject::connect(view, &QObject::destroyed, dock, []() {
        g_qt_dock_state.page_ready_notified = false;
        ClearQtExecutorPage();
        aegis_obs_browser_dock_host_scaffold_on_page_unloaded();
    });

    QObject::connect(dock, &QObject::destroyed, main_window, []() {
        g_qt_dock_state.dock_widget = nullptr;
        g_qt_dock_state.web_view = nullptr;
        g_qt_dock_state.dock_registered = false;
        g_qt_dock_state.page_ready_notified = false;
        ClearQtExecutorPage();
    });

    g_qt_dock_state.dock_widget = dock;
    g_qt_dock_state.web_view = view;
    g_qt_dock_state.page_ready_notified = false;
    SetQtExecutorPage(view->page());

    return dock;
}

void LoadBootstrapPage() {
    if (!g_qt_dock_state.web_view) {
        return;
    }
    const DockBridgeAssets assets = LoadDockBridgeAssets();
    if (assets.complete) {
        blog(
            assets.uses_scaffold_fallback ? LOG_WARNING : LOG_INFO,
            "[aegis-obs-shim] browser dock scaffold %s",
            assets.resolution_note.toUtf8().constData());
        const QString html = BuildRealDockPageHtml(assets);
        g_qt_dock_state.web_view->setHtml(html, QUrl(QStringLiteral("about:blank")));
        return;
    }

    blog(LOG_WARNING, "[aegis-obs-shim] browser dock scaffold %s", assets.resolution_note.toUtf8().constData());
    const QString html = QString::fromUtf8(kDockValidationBootstrapHtml);
    g_qt_dock_state.web_view->setHtml(html, QUrl(QStringLiteral("about:blank")));
}
#endif

#endif

#if defined(AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_OBS_CEF)

constexpr const char* kCefDockValidationHtml = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8" />
  <title>Aegis Dock Host (OBS/CEF)</title>
  <style>
    html, body { margin: 0; padding: 0; background: #101820; color: #d9e2ec; font-family: Consolas, monospace; }
    #root { padding: 10px; }
    #status { font-weight: 700; margin-bottom: 8px; }
    #log { white-space: pre-wrap; font-size: 12px; line-height: 1.35; opacity: 0.92; }
  </style>
</head>
<body>
  <div id="root">
    <div id="status">Aegis Dock Host (OBS/CEF validation page)</div>
    <div id="log">waiting...</div>
  </div>
  <script>
    (function () {
      const logEl = document.getElementById("log");
      const lines = [];
      function log(msg) {
        const line = "[" + new Date().toISOString() + "] " + String(msg);
        lines.push(line);
        while (lines.length > 60) lines.shift();
        if (logEl) logEl.textContent = lines.join("\\n");
      }
      function ok(name, payload) { log(name + ": " + payload); return true; }
      window.aegisDockNative = {
        receiveSceneSnapshotJson(jsonText) { return ok("receiveSceneSnapshotJson", jsonText); },
        receiveIpcEnvelopeJson(jsonText) { return ok("receiveIpcEnvelopeJson", jsonText); },
        receivePipeStatus(status, reason) { return ok("receivePipeStatus", JSON.stringify({ status, reason })); },
        receiveCurrentScene(sceneName) { return ok("receiveCurrentScene", JSON.stringify(sceneName)); },
        receiveSceneSwitchCompletedJson(jsonText) { return ok("receiveSceneSwitchCompletedJson", jsonText); },
        receiveDockActionResultJson(jsonText) { return ok("receiveDockActionResultJson", jsonText); }
      };
      try { window.dispatchEvent(new CustomEvent("aegis:dock:native-ready", { detail: { ok: true } })); } catch (_e) {}
      log("window.aegisDockNative ready");
    })();
  </script>
</body>
</html>
)HTML";

struct CefDockJsExecutorState {
    std::mutex mu;
    QPointer<QCefWidget> widget;
};

struct CefDockHostState {
    QPointer<QDockWidget> dock_widget;
    QPointer<QCefWidget> cef_widget;
    bool dock_registered = false;
    bool page_ready_notified = false;
    QPointer<QTimer> ready_probe_timer;
    QPointer<QTimer> init_retry_timer;
    int init_retry_attempts = 0;
};

CefDockJsExecutorState g_cef_executor_state;
CefDockHostState g_cef_dock_state;
QCef* g_obs_cef = nullptr;

bool CefDockExecuteJs(const char* js_utf8, void* user_data) {
    auto* state = static_cast<CefDockJsExecutorState*>(user_data);
    if (!state || !js_utf8) {
        return false;
    }

    QPointer<QCefWidget> widget;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        widget = state->widget;
    }
    if (!widget) {
        return false;
    }

    widget->executeJavaScript(std::string(js_utf8));
    return true;
}

void SetCefExecutorWidget(QCefWidget* widget) {
    std::lock_guard<std::mutex> lock(g_cef_executor_state.mu);
    g_cef_executor_state.widget = widget;
}

void ClearCefExecutorWidget() {
    std::lock_guard<std::mutex> lock(g_cef_executor_state.mu);
    g_cef_executor_state.widget.clear();
}

void MarkCefPageReadyOnce(const char* reason) {
    if (g_cef_dock_state.page_ready_notified) {
        blog(LOG_DEBUG, "[aegis-obs-shim] browser dock scaffold CEF page ready already notified; skipping (%s)", reason ? reason : "unknown");
        return;
    }
    g_cef_dock_state.page_ready_notified = true;
    blog(LOG_INFO, "[aegis-obs-shim] browser dock scaffold CEF page ready (%s)", reason ? reason : "unknown");
    aegis_obs_browser_dock_host_scaffold_on_page_ready();
}

void StopCefReadyProbeTimer() {
    if (g_cef_dock_state.ready_probe_timer) {
        g_cef_dock_state.ready_probe_timer->stop();
        g_cef_dock_state.ready_probe_timer->deleteLater();
        g_cef_dock_state.ready_probe_timer = nullptr;
    }
}

void StopCefInitRetryTimer() {
    if (g_cef_dock_state.init_retry_timer) {
        g_cef_dock_state.init_retry_timer->stop();
        g_cef_dock_state.init_retry_timer->deleteLater();
        g_cef_dock_state.init_retry_timer = nullptr;
    }
    g_cef_dock_state.init_retry_attempts = 0;
}

QDockWidget* CreateCefDockWidgetForObsMainWindow() {
    auto* main_window = static_cast<QMainWindow*>(obs_frontend_get_main_window());
    if (!main_window) {
        blog(LOG_WARNING, "[aegis-obs-shim] browser dock scaffold missing OBS main window");
        return nullptr;
    }

    if (!g_obs_cef) {
        g_obs_cef = obs_browser_init_panel();
        if (!g_obs_cef) {
            blog(LOG_WARNING, "[aegis-obs-shim] browser dock scaffold CEF panel init unavailable (obs-browser missing?)");
            return nullptr;
        }
    }

    if (!g_obs_cef->initialized()) {
        (void)g_obs_cef->init_browser();
        (void)g_obs_cef->wait_for_browser_init();
    }
    if (!g_obs_cef->initialized()) {
        blog(LOG_WARNING, "[aegis-obs-shim] browser dock scaffold CEF init failed");
        return nullptr;
    }

    auto* dock = new QDockWidget(QString::fromUtf8(kDockTitle), main_window);
    dock->setObjectName(QString::fromUtf8(kDockId));
    dock->setAllowedAreas(
        Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea | Qt::TopDockWidgetArea |
        Qt::BottomDockWidgetArea);

    QString html;
    const DockBridgeAssets assets = LoadDockBridgeAssets();
    if (assets.complete) {
        blog(
            assets.uses_scaffold_fallback ? LOG_WARNING : LOG_INFO,
            "[aegis-obs-shim] browser dock scaffold %s",
            assets.resolution_note.toUtf8().constData());
        html = BuildRealDockPageHtml(assets);
    } else {
        blog(LOG_WARNING, "[aegis-obs-shim] browser dock scaffold %s", assets.resolution_note.toUtf8().constData());
        html = QString::fromUtf8(kCefDockValidationHtml);
    }

    const QByteArray encoded = QUrl::toPercentEncoding(html);
    const std::string data_url =
        std::string("data:text/html;charset=utf-8,") + encoded.constData();
    const QByteArray html_utf8 = html.toUtf8();
    blog(
        LOG_INFO,
        "[aegis-obs-shim] browser dock scaffold CEF bootstrap html prepared mode=%s html_bytes=%d data_url_bytes=%d app_inline=%s",
        assets.complete ? "real_bridge_assets" : "validation_fallback",
        static_cast<int>(html_utf8.size()),
        static_cast<int>(data_url.size()),
        (!assets.dock_app_js.isEmpty()) ? "true" : "false");

    QCefWidget* view = g_obs_cef->create_widget(dock, data_url, nullptr);
    if (!view) {
        blog(LOG_WARNING, "[aegis-obs-shim] browser dock scaffold CEF create_widget failed");
        dock->deleteLater();
        return nullptr;
    }
    dock->setWidget(view);
    const QMetaObject* view_meta = view->metaObject();
    const int sig_title_idx = view_meta ? view_meta->indexOfSignal("titleChanged(QString)") : -1;
    const int sig_url_idx = view_meta ? view_meta->indexOfSignal("urlChanged(QString)") : -1;
    blog(
        LOG_INFO,
        "[aegis-obs-shim] browser dock scaffold CEF signal introspection: class=%s titleChanged_idx=%d urlChanged_idx=%d",
        view_meta ? view_meta->className() : "null",
        sig_title_idx,
        sig_url_idx);

    QObject::connect(view, &QObject::destroyed, dock, []() {
        g_cef_dock_state.page_ready_notified = false;
        StopCefReadyProbeTimer();
        ClearCefExecutorWidget();
        aegis_obs_browser_dock_host_scaffold_on_page_unloaded();
    });
    QObject::connect(dock, &QObject::destroyed, main_window, []() {
        g_cef_dock_state.dock_widget = nullptr;
        g_cef_dock_state.cef_widget = nullptr;
        g_cef_dock_state.dock_registered = false;
        g_cef_dock_state.page_ready_notified = false;
        StopCefReadyProbeTimer();
        StopCefInitRetryTimer();
        ClearCefExecutorWidget();
    });
    // Bridge QCefWidget's non-exported custom titleChanged signal into an exported Qt slot/signal path.
    // We can't link directly against QCefWidget::titleChanged with function-pointer syntax from the plugin.
    const auto title_bridge_conn =
        QObject::connect(view, SIGNAL(titleChanged(QString)), dock, SLOT(setWindowTitle(QString)));
    const auto url_bridge_conn =
        QObject::connect(view, SIGNAL(urlChanged(QString)), dock, SLOT(setWindowTitle(QString)));
    if (!title_bridge_conn) {
        blog(LOG_WARNING, "[aegis-obs-shim] browser dock scaffold titleChanged bridge connect failed");
    }
    if (!url_bridge_conn) {
        blog(LOG_WARNING, "[aegis-obs-shim] browser dock scaffold urlChanged bridge connect failed");
    }
    QObject::connect(dock, &QWidget::windowTitleChanged, dock, [dock](const QString& title) {
        if (!title.isEmpty()) {
            const QByteArray sample = title.left(180).toUtf8();
            blog(
                LOG_DEBUG,
                "[aegis-obs-shim] browser dock scaffold windowTitleChanged bytes=%d sample=%s",
                static_cast<int>(title.toUtf8().size()),
                sample.constData());
        }
        if (title.contains(QStringLiteral("#__AEGIS_DOCK_ACTION__:"), Qt::CaseInsensitive)) {
            blog(
                LOG_INFO,
                "[aegis-obs-shim] browser dock scaffold observed window title transport payload bytes=%d",
                static_cast<int>(title.toUtf8().size()));
        }
        if (ContainsDockReadySignal(title)) {
            MarkCefPageReadyOnce("bridge_ready_signal");
            dock->setWindowTitle(QString::fromUtf8(kDockTitle));
            return;
        }
        const bool handled =
            TryHandleDockActionTitleMessage(title) || TryHandleDockActionUrlMessage(title);
        if (handled) {
            // Keep dock chrome title stable even when action transport uses title signaling.
            dock->setWindowTitle(QString::fromUtf8(kDockTitle));
        }
    });

    auto* ready_timer = new QTimer(dock);
    ready_timer->setSingleShot(true);
    QObject::connect(ready_timer, &QTimer::timeout, dock, []() {
        blog(LOG_DEBUG, "[aegis-obs-shim] browser dock scaffold CEF ready probe timer fired");
        if (!g_cef_dock_state.page_ready_notified) {
            // Timer-based page ready probe avoids direct linkage to QCefWidget Qt signal symbols,
            // which are not exported for plugin linking.
            MarkCefPageReadyOnce("timer");
        }
    });

    g_cef_dock_state.dock_widget = dock;
    g_cef_dock_state.cef_widget = view;
    g_cef_dock_state.page_ready_notified = false;
    g_cef_dock_state.ready_probe_timer = ready_timer;
    SetCefExecutorWidget(view);

    blog(LOG_DEBUG, "[aegis-obs-shim] browser dock scaffold CEF ready probe timer start delay_ms=500");
    ready_timer->start(500);
    return dock;
}

bool TryRegisterCefDockHost() {
    if (g_cef_dock_state.dock_registered && g_cef_dock_state.dock_widget) {
        return true;
    }

    QDockWidget* dock = CreateCefDockWidgetForObsMainWindow();
    if (!dock) {
        return false;
    }

    const bool added = obs_frontend_add_custom_qdock(kDockId, dock);
    if (!added) {
        blog(
            LOG_WARNING,
            "[aegis-obs-shim] browser dock scaffold failed to register dock id=%s (already exists?)",
            kDockId);
        StopCefReadyProbeTimer();
        ClearCefExecutorWidget();
        dock->deleteLater();
        return false;
    }

    g_cef_dock_state.dock_registered = true;
    aegis_obs_browser_dock_host_scaffold_set_js_executor(&CefDockExecuteJs, &g_cef_executor_state);
    // Validation-friendly behavior: force the custom dock visible so it is not lost in a saved layout.
    dock->setFloating(true);
    dock->resize(420, 720);
    dock->show();
    dock->raise();
    dock->activateWindow();
    blog(
        LOG_INFO,
        "[aegis-obs-shim] browser dock scaffold initialize id=%s title=%s (OBS/CEF host active)",
        kDockId,
        kDockTitle);
    return true;
}

void EnsureCefInitRetryTimerStarted() {
    if (g_cef_dock_state.dock_registered) {
        StopCefInitRetryTimer();
        return;
    }

    auto* main_window = static_cast<QMainWindow*>(obs_frontend_get_main_window());
    if (!main_window) {
        blog(LOG_WARNING, "[aegis-obs-shim] browser dock scaffold CEF retry unavailable (no OBS main window)");
        return;
    }

    if (g_cef_dock_state.init_retry_timer) {
        if (!g_cef_dock_state.init_retry_timer->isActive()) {
            g_cef_dock_state.init_retry_timer->start(500);
        }
        return;
    }

    auto* timer = new QTimer(main_window);
    timer->setInterval(500);
    timer->setSingleShot(false);
    QObject::connect(timer, &QTimer::timeout, main_window, [timer]() {
        if (g_cef_dock_state.dock_registered) {
            StopCefInitRetryTimer();
            return;
        }

        g_cef_dock_state.init_retry_attempts += 1;
        blog(
            LOG_DEBUG,
            "[aegis-obs-shim] browser dock scaffold CEF init retry attempt=%d",
            g_cef_dock_state.init_retry_attempts);
        if (TryRegisterCefDockHost()) {
            StopCefInitRetryTimer();
            return;
        }

        if (g_cef_dock_state.init_retry_attempts >= 10) {
            blog(
                LOG_WARNING,
                "[aegis-obs-shim] browser dock scaffold CEF retry exhausted after %d attempts",
                g_cef_dock_state.init_retry_attempts);
            StopCefInitRetryTimer();
        }
        (void)timer;
    });

    g_cef_dock_state.init_retry_timer = timer;
    g_cef_dock_state.init_retry_attempts = 0;
    blog(LOG_INFO, "[aegis-obs-shim] browser dock scaffold scheduling OBS/CEF init retry");
    timer->start();
}

#endif

} // namespace

void aegis_obs_browser_dock_host_scaffold_initialize() {
#if defined(AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_OBS_CEF)
    if (g_cef_dock_state.dock_registered && g_cef_dock_state.dock_widget) {
        blog(LOG_INFO, "[aegis-obs-shim] browser dock scaffold initialize skipped (CEF host already active)");
        return;
    }

    if (!TryRegisterCefDockHost()) {
        blog(
            LOG_INFO,
            "[aegis-obs-shim] browser dock scaffold initialize deferred (OBS/CEF host not ready yet)");
        EnsureCefInitRetryTimerStarted();
        aegis_obs_browser_dock_host_scaffold_on_page_unloaded();
        return;
    }
#elif defined(AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_QT_WEBENGINE)
    if (g_qt_dock_state.dock_registered && g_qt_dock_state.dock_widget) {
        blog(LOG_INFO, "[aegis-obs-shim] browser dock scaffold initialize skipped (already active)");
        return;
    }

    QDockWidget* dock = CreateDockWidgetForObsMainWindow();
    if (!dock) {
        blog(
            LOG_WARNING,
            "[aegis-obs-shim] browser dock scaffold initialize fallback (Qt/WebEngine host create failed)");
        aegis_obs_browser_dock_host_scaffold_on_page_unloaded();
        return;
    }

    const bool added = obs_frontend_add_custom_qdock(kDockId, dock);
    if (!added) {
        blog(
            LOG_WARNING,
            "[aegis-obs-shim] browser dock scaffold failed to register dock id=%s (already exists?)",
            kDockId);
        ClearQtExecutorPage();
        dock->deleteLater();
        aegis_obs_browser_dock_host_scaffold_on_page_unloaded();
        return;
    }

    g_qt_dock_state.dock_registered = true;
    aegis_obs_browser_dock_host_scaffold_set_js_executor(&QtDockExecuteJs, &g_qt_executor_state);
    blog(
        LOG_INFO,
        "[aegis-obs-shim] browser dock scaffold initialize id=%s title=%s (Qt/WebEngine host active)",
        kDockId,
        kDockTitle);

    LoadBootstrapPage();
#else
    blog(
        LOG_INFO,
        "[aegis-obs-shim] browser dock scaffold initialize id=%s title=%s (Qt/CEF embedding TODO)",
        kDockId,
        kDockTitle);

    // Future implementation responsibilities:
    // - create/load browser dock widget/page
    // - inject/load aegis-dock-bridge-host.js + aegis-dock-browser-host-bootstrap.js
    // - call aegis_obs_browser_dock_host_scaffold_set_js_executor(...)
    // - call aegis_obs_browser_dock_host_scaffold_on_page_ready() after bootstrap is ready
    aegis_obs_browser_dock_host_scaffold_on_page_unloaded();
#endif
}

void aegis_obs_browser_dock_host_scaffold_shutdown() {
#if defined(AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_OBS_CEF)
    blog(LOG_INFO, "[aegis-obs-shim] browser dock scaffold shutdown (OBS/CEF host)");

    aegis_obs_browser_dock_host_scaffold_on_page_unloaded();
    StopCefInitRetryTimer();
    StopCefReadyProbeTimer();
    ClearCefExecutorWidget();

    if (g_cef_dock_state.cef_widget) {
        const int panel_version = obs_browser_qcef_version();
        if (panel_version >= 2) {
            blog(LOG_INFO, "[aegis-obs-shim] browser dock scaffold CEF closeBrowser (panel_version=%d)", panel_version);
            g_cef_dock_state.cef_widget->closeBrowser();
        }
    }

    // Do not actively remove the CEF dock during plugin shutdown. In practice this can race
    // OBS/obs-browser teardown on EXIT and trigger crashes inside CEF close/join paths.
    // Let OBS own the dock/widget destruction during application shutdown.
    g_cef_dock_state.dock_registered = false;
    g_cef_dock_state.cef_widget = nullptr;
    g_cef_dock_state.dock_widget = nullptr;
    g_cef_dock_state.page_ready_notified = false;
#elif defined(AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_QT_WEBENGINE)
    blog(LOG_INFO, "[aegis-obs-shim] browser dock scaffold shutdown (Qt/WebEngine host)");

    aegis_obs_browser_dock_host_scaffold_on_page_unloaded();

    if (g_qt_dock_state.dock_registered) {
        obs_frontend_remove_dock(kDockId);
        g_qt_dock_state.dock_registered = false;
    }

    ClearQtExecutorPage();

    if (g_qt_dock_state.web_view) {
        g_qt_dock_state.web_view->deleteLater();
        g_qt_dock_state.web_view = nullptr;
    }
    if (g_qt_dock_state.dock_widget) {
        g_qt_dock_state.dock_widget->deleteLater();
        g_qt_dock_state.dock_widget = nullptr;
    }
    g_qt_dock_state.page_ready_notified = false;
#else
    blog(LOG_INFO, "[aegis-obs-shim] browser dock scaffold shutdown");
    aegis_obs_browser_dock_host_scaffold_on_page_unloaded();
#endif
}

void aegis_obs_browser_dock_host_scaffold_set_js_executor(
    aegis_dock_js_execute_fn fn,
    void* user_data) {
    blog(
        LOG_INFO,
        "[aegis-obs-shim] browser dock scaffold set_js_executor: %s",
        fn ? "registered" : "cleared");
    aegis_obs_shim_register_dock_js_executor(fn, user_data);
}

void aegis_obs_browser_dock_host_scaffold_on_page_ready() {
    blog(LOG_INFO, "[aegis-obs-shim] browser dock scaffold page ready");
    aegis_obs_shim_notify_dock_page_ready();
}

void aegis_obs_browser_dock_host_scaffold_on_page_unloaded() {
    blog(LOG_INFO, "[aegis-obs-shim] browser dock scaffold page unloaded");
    aegis_obs_shim_notify_dock_page_unloaded();
}

bool aegis_obs_browser_dock_host_scaffold_show_dock() {
#if defined(AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_OBS_CEF)
    if (g_cef_dock_state.dock_widget) {
        g_cef_dock_state.dock_widget->setFloating(true);
        g_cef_dock_state.dock_widget->show();
        g_cef_dock_state.dock_widget->raise();
        g_cef_dock_state.dock_widget->activateWindow();
        return true;
    }
#endif
#if defined(AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_QT_WEBENGINE)
    if (g_qt_dock_state.dock_widget) {
        g_qt_dock_state.dock_widget->setFloating(true);
        g_qt_dock_state.dock_widget->show();
        g_qt_dock_state.dock_widget->raise();
        g_qt_dock_state.dock_widget->activateWindow();
        return true;
    }
#endif
    return false;
}

#endif
