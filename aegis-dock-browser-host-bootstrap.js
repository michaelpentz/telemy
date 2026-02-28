"use strict";

// Browser dock bootstrap exposing the plugin-call surface:
//
// Inbound (native -> dock):
//   window.aegisDockNative.receiveIpcEnvelopeJson(...)
//   window.aegisDockNative.receiveSceneSnapshotJson(...)
//   window.aegisDockNative.receivePipeStatus(...)
//   window.aegisDockNative.receiveCurrentScene(...)
//   window.aegisDockNative.receiveSceneSwitchCompletedJson(...)
//
// Outbound (dock -> native):
//   window.aegisDockNative.sendDockAction({ type, ...payload })
//   window.aegisDockNative.getState()
//   window.aegisDockNative.getCapabilities()

(function initAegisDockBrowserHostBootstrap(globalObj) {
  const g = globalObj || (typeof window !== "undefined" ? window : globalThis);
  if (!g) return;

  function parseJsonSafe(jsonText) {
    try {
      return { ok: true, value: JSON.parse(String(jsonText)) };
    } catch (_e) {
      return { ok: false, value: null };
    }
  }

  function createFallbackHostFromBridge() {
    var bridgeApi = g.AegisDockBridge;
    if (!bridgeApi || typeof bridgeApi.createAegisDockBridgeHost !== "function") {
      return null;
    }
    var bridge = (g.__AEGIS_DOCK_BRIDGE__ && typeof g.__AEGIS_DOCK_BRIDGE__.getState === "function")
      ? g.__AEGIS_DOCK_BRIDGE__
      : bridgeApi.createAegisDockBridgeHost();
    if (typeof bridgeApi.attachAegisDockBridgeToWindow === "function") {
      bridgeApi.attachAegisDockBridgeToWindow(bridge);
    } else {
      g.__AEGIS_DOCK_BRIDGE__ = bridge;
    }

    var fallbackHost = {
      getState: function () { return bridge.getState(); },
      sendDockAction: function (action) { return bridge.sendAction(action); },
      receiveIpcEnvelope: function (envelope) {
        if (typeof bridge.handleIpcEnvelope === "function") {
          bridge.handleIpcEnvelope(envelope);
          return true;
        }
        return false;
      },
      receiveIpcEnvelopeJson: function (jsonText) {
        var parsed = parseJsonSafe(jsonText);
        return parsed.ok ? fallbackHost.receiveIpcEnvelope(parsed.value) : false;
      },
      setObsSceneSnapshot: function (payload) {
        var ok = true;
        if (typeof bridge.setObsSceneNames === "function") {
          bridge.setObsSceneNames(Array.isArray(payload && payload.sceneNames) ? payload.sceneNames : []);
        }
        if (typeof bridge.setObsActiveSceneName === "function") {
          bridge.setObsActiveSceneName(payload && payload.currentSceneName != null ? String(payload.currentSceneName) : null);
        }
        return ok;
      },
      setObsSceneSnapshotJson: function (jsonText) {
        var parsed = parseJsonSafe(jsonText);
        return parsed.ok ? fallbackHost.setObsSceneSnapshot(parsed.value) : false;
      },
      setPipeStatus: function (status) {
        if (typeof bridge.setPipeStatus === "function") {
          bridge.setPipeStatus(status || null);
          return true;
        }
        return false;
      },
      setObsCurrentScene: function (sceneName) {
        if (typeof bridge.setObsActiveSceneName === "function") {
          bridge.setObsActiveSceneName(sceneName == null ? null : String(sceneName));
          return true;
        }
        return false;
      },
      notifySceneSwitchCompleted: function (result) {
        if (typeof bridge.notifySceneSwitchCompleted === "function") {
          bridge.notifySceneSwitchCompleted(result || {});
          return true;
        }
        return false;
      },
      notifySceneSwitchCompletedJson: function (jsonText) {
        var parsed = parseJsonSafe(jsonText);
        return parsed.ok ? fallbackHost.notifySceneSwitchCompleted(parsed.value) : false;
      },
    };
    return fallbackHost;
  }

  function ensureHost() {
    if (g.aegisDockHost && typeof g.aegisDockHost.getState === "function") {
      return g.aegisDockHost;
    }
    // Prefer fallback host that directly matches AegisDockBridge global API.
    // The separate AegisDockBridgeHost adapter may be present but out-of-sync
    // with bridge method names; fallback is safer for OBS embedded runtime.
    var fallbackHost = createFallbackHostFromBridge();
    if (fallbackHost) {
      g.aegisDockHost = fallbackHost;
      dispatch("aegis:dock:host-fallback", { ok: true, source: "AegisDockBridge" });
      return fallbackHost;
    }

    const exports = g.AegisDockBridgeHost;
    if (!exports || typeof exports.createWindowAegisDockBridgeHost !== "function") {
      throw new Error("No compatible dock bridge host is available");
    }
    try {
      var host = exports.createWindowAegisDockBridgeHost();
      g.aegisDockHost = host;
      return host;
    } catch (_e) {
      throw new Error("Failed to initialize dock bridge host");
    }
  }

  function dispatch(name, detail) {
    if (typeof g.dispatchEvent !== "function" || typeof g.CustomEvent !== "function") return;
    try {
      g.dispatchEvent(new g.CustomEvent(name, { detail: detail || {} }));
    } catch (_e) {}
  }

  function parseJson(jsonText, errorMsg) {
    try {
      return { ok: true, value: JSON.parse(String(jsonText)) };
    } catch (_e) {
      dispatch("aegis:dock:error", {
        message: errorMsg || "Invalid JSON",
        jsonText: String(jsonText),
      });
      return { ok: false, value: null };
    }
  }

  function tryForwardDockActionJsonToNativeViaTitle(actionJson) {
    if (typeof actionJson !== "string" || !actionJson.length) return false;
    if (typeof document === "undefined" || typeof document.title !== "string") return false;
    if (typeof encodeURIComponent !== "function") return false;
    try {
      var actionTitle = "__AEGIS_DOCK_ACTION__:" + encodeURIComponent(actionJson);
      document.title = actionTitle;
      return true;
    } catch (_e) {
      return false;
    }
  }

  function tryForwardDockActionJsonToNativeViaHash(actionJson) {
    if (typeof actionJson !== "string" || !actionJson.length) return false;
    if (typeof location === "undefined") return false;
    if (typeof encodeURIComponent !== "function") return false;
    try {
      var actionHash = "__AEGIS_DOCK_ACTION__:" + encodeURIComponent(actionJson);
      if (location.hash === "#" + actionHash) {
        location.hash = "";
      }
      location.hash = actionHash;
      return true;
    } catch (_e) {
      return false;
    }
  }

  function tryForwardDockActionJsonToNative(actionJson) {
    var sentViaTitle = tryForwardDockActionJsonToNativeViaTitle(actionJson);
    var sentViaHash = tryForwardDockActionJsonToNativeViaHash(actionJson);
    return sentViaTitle || sentViaHash;
  }

  function signalDockReadyToNative() {
    if (typeof encodeURIComponent !== "function") return false;
    var marker = "__AEGIS_DOCK_READY__:" + encodeURIComponent(String(Date.now()));
    var sent = false;
    try {
      if (typeof document !== "undefined" && typeof document.title === "string") {
        document.title = marker;
        sent = true;
      }
    } catch (_e) {}
    try {
      if (typeof location !== "undefined") {
        if (location.hash === "#" + marker) {
          location.hash = "";
        }
        location.hash = marker;
        sent = true;
      }
    } catch (_e) {}
    return sent;
  }

  const nativeApi = {
    ensureHost,

    getState() {
      return ensureHost().getState();
    },

    receiveIpcEnvelope(envelope) {
      const ok = ensureHost().receiveIpcEnvelope(envelope);
      dispatch("aegis:dock:ipc-envelope", { ok, envelope: envelope || null });
      return ok;
    },

    receiveIpcEnvelopeJson(jsonText) {
      const ok = ensureHost().receiveIpcEnvelopeJson(jsonText);
      dispatch("aegis:dock:ipc-envelope-json", { ok });
      return ok;
    },

    receiveSceneSnapshot(payload) {
      const ok = ensureHost().setObsSceneSnapshot(payload);
      dispatch("aegis:dock:scene-snapshot", { ok, payload: payload || null });
      return ok;
    },

    receiveSceneSnapshotJson(jsonText) {
      const ok = ensureHost().setObsSceneSnapshotJson(jsonText);
      dispatch("aegis:dock:scene-snapshot-json", { ok });
      return ok;
    },

    receivePipeStatus(status, reason) {
      const ok = ensureHost().setPipeStatus(status, reason);
      dispatch("aegis:dock:pipe-status", { ok, status: status || null, reason: reason || null });
      return ok;
    },

    receiveCurrentScene(sceneName) {
      const ok = ensureHost().setObsCurrentScene(sceneName);
      dispatch("aegis:dock:current-scene", { ok, sceneName: sceneName || null });
      return ok;
    },

    receiveSceneSwitchCompleted(result) {
      const ok = ensureHost().notifySceneSwitchCompleted(result);
      dispatch("aegis:dock:scene-switch-completed", { ok, result: result || null });
      return ok;
    },

    receiveSceneSwitchCompletedJson(jsonText) {
      const ok = ensureHost().notifySceneSwitchCompletedJson(jsonText);
      dispatch("aegis:dock:scene-switch-completed-json", { ok });
      return ok;
    },

    // --- Outbound: dock UI -> bridge host ---

    sendDockAction(action) {
      var host = ensureHost();
      var jsonText = "";
      try {
        jsonText = JSON.stringify(action || {});
      } catch (_e) {
        jsonText = "";
      }
      var forwarded = tryForwardDockActionJsonToNative(jsonText);
      var hostResult = null;
      if (typeof host.sendDockAction === "function") {
        try {
          hostResult = host.sendDockAction(action);
        } catch (err) {
          dispatch("aegis:dock:error", {
            message: "Host sendDockAction threw",
            error: String((err && err.message) || err || ""),
          });
        }
      } else {
        dispatch("aegis:dock:action-unsupported", { action: action || null });
      }
      return hostResult != null ? hostResult : forwarded;
    },

    sendDockActionJson(jsonText) {
      var parsed = parseJson(jsonText, "Invalid dock action JSON");
      if (!parsed.ok) return false;
      var forwarded = tryForwardDockActionJsonToNative(String(jsonText));
      var host = ensureHost();
      var hostOk = false;
      if (typeof host.sendDockAction === "function") {
        try {
          host.sendDockAction(parsed.value);
          hostOk = true;
        } catch (err) {
          dispatch("aegis:dock:error", {
            message: "Host sendDockActionJson threw",
            error: String((err && err.message) || err || ""),
          });
        }
      }
      return forwarded || hostOk;
    },

    getCapabilities() {
      var host = ensureHost();
      // Return capabilities object describing what the bridge supports.
      // The host may provide its own; otherwise derive from available methods.
      if (typeof host.getCapabilities === "function") {
        return host.getCapabilities();
      }
      return {
        switchScene: typeof host.sendDockAction === "function",
        setMode: typeof host.sendDockAction === "function",
        setSetting: typeof host.sendDockAction === "function",
        getState: typeof host.getState === "function",
      };
    },
  };

  g.aegisDockNative = nativeApi;
  signalDockReadyToNative();
  dispatch("aegis:dock:native-ready", { ok: true });
})(typeof window !== "undefined" ? window : undefined);
