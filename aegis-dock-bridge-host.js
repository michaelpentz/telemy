"use strict";

// Host-side adapter around aegis-dock-bridge.js for browser-dock integration.
// Defines stable entry points for plugin/local callbacks and IPC envelope delivery.

let bridgeModule = null;

function loadBridgeModule() {
  if (bridgeModule) return bridgeModule;

  if (typeof window !== "undefined" && window.AegisDockBridge) {
    bridgeModule = window.AegisDockBridge;
    return bridgeModule;
  }

  if (typeof require === "function") {
    try {
      bridgeModule = require("./aegis-dock-bridge.js");
      return bridgeModule;
    } catch (_err) {
      // In embedded browser contexts a `require` symbol may exist but not be usable for local assets.
      // Fall through to the final error so browser-global injection can be preferred when available.
    }
  }

  throw new Error("aegis-dock-bridge module not available");
}

function createAegisDockBridgeHost(options) {
  const opts = options || {};
  const bridgeApi = loadBridgeModule();
  const bridge = opts.bridge || bridgeApi.createAegisDockBridge(opts.bridgeOptions || {});
  const listeners = new Set();

  function emit(eventName, payload) {
    listeners.forEach((fn) => {
      try {
        fn(eventName, payload, bridge.getState());
      } catch (_err) {
        // Swallow listener errors to avoid breaking dock updates.
      }
    });
  }

  function normalizeSceneSnapshot(payload) {
    if (!payload || typeof payload !== "object") return null;
    const names = Array.isArray(payload.sceneNames)
      ? payload.sceneNames
      : Array.isArray(payload.scenes)
      ? payload.scenes
      : [];
    return {
      reason: payload.reason || "unknown",
      sceneNames: names.map((s) => (typeof s === "string" ? s : (s && s.name) || "")).filter(Boolean),
      currentSceneName:
        payload.currentSceneName || payload.current_scene_name || payload.activeSceneName || null,
    };
  }

  const host = {
    bridge,

    getState() {
      return bridge.getState();
    },

    subscribe(listener) {
      if (typeof listener !== "function") return function noop() {};
      listeners.add(listener);
      return function unsubscribe() {
        listeners.delete(listener);
      };
    },

    subscribeDockState(listener) {
      return bridge.subscribe(listener);
    },

    receiveIpcEnvelope(envelope) {
      const ok = bridge.receiveEnvelope(envelope);
      emit("ipc_envelope", envelope);
      return ok;
    },

    receiveIpcEnvelopeJson(jsonText) {
      try {
        const envelope = JSON.parse(String(jsonText));
        return this.receiveIpcEnvelope(envelope);
      } catch (_err) {
        bridge.pushEvent({
          source: "bridge-host",
          type: "error",
          msg: "Invalid IPC envelope JSON",
        });
        emit("ipc_envelope_json_error", { jsonText: String(jsonText) });
        return false;
      }
    },

    setObsSceneSnapshot(payload) {
      const snap = normalizeSceneSnapshot(payload);
      if (!snap) return false;
      bridge.setObsSceneNames(snap.sceneNames);
      if (snap.currentSceneName !== null) {
        bridge.setObsActiveSceneName(snap.currentSceneName);
      }
      bridge.pushEvent({
        source: "obs",
        type: "info",
        msg:
          "OBS scene snapshot (" +
          snap.reason +
          "): " +
          String(snap.sceneNames.length) +
          " scenes" +
          (snap.currentSceneName ? ", current=" + snap.currentSceneName : ""),
      });
      emit("obs_scene_snapshot", snap);
      return true;
    },

    setObsSceneSnapshotJson(jsonText) {
      try {
        const payload = JSON.parse(String(jsonText));
        return this.setObsSceneSnapshot(payload);
      } catch (_err) {
        bridge.pushEvent({
          source: "bridge-host",
          type: "error",
          msg: "Invalid OBS scene snapshot JSON",
        });
        emit("obs_scene_snapshot_json_error", { jsonText: String(jsonText) });
        return false;
      }
    },

    setObsCurrentScene(sceneName) {
      bridge.setObsActiveSceneName(sceneName || null);
      emit("obs_current_scene", { sceneName: sceneName || null });
      return true;
    },

    setEngineState(engineState) {
      bridge.setEngineState(engineState || null);
      emit("engine_state", { engineState: engineState || null });
      return true;
    },

    setPipeStatus(status, reason) {
      const ok = bridge.setPipeStatus(status, reason);
      emit("pipe_status", { status, reason: reason || null });
      return ok;
    },

    setConnectionTelemetry(items) {
      bridge.setConnectionTelemetry(items || []);
      emit("connection_telemetry", { count: Array.isArray(items) ? items.length : 0 });
      return true;
    },

    setLiveInfo(liveInfo) {
      bridge.setLiveInfo(liveInfo || {});
      emit("live_info", liveInfo || {});
      return true;
    },

    setBitrateThresholds(thresholds) {
      bridge.setBitrateThresholds(thresholds || {});
      emit("bitrate_thresholds", thresholds || {});
      return true;
    },

    setSettings(settings) {
      bridge.setSettings(settings || {});
      emit("settings", settings || {});
      return true;
    },

    notifySceneSwitchCompleted(result) {
      bridge.notifySceneSwitchCompleted(result || {});
      emit("scene_switch_completed", result || {});
      return true;
    },

    notifySceneSwitchCompletedJson(jsonText) {
      try {
        const result = JSON.parse(String(jsonText));
        return this.notifySceneSwitchCompleted(result);
      } catch (_err) {
        bridge.pushEvent({
          source: "bridge-host",
          type: "error",
          msg: "Invalid scene-switch-completed JSON",
        });
        emit("scene_switch_completed_json_error", { jsonText: String(jsonText) });
        return false;
      }
    },

    notifyDockActionResult(result) {
      bridge.notifyDockActionResult(result || {});
      emit("dock_action_result", result || {});
      return true;
    },

    notifyDockActionResultJson(jsonText) {
      try {
        const result = JSON.parse(String(jsonText));
        return this.notifyDockActionResult(result);
      } catch (_err) {
        bridge.pushEvent({
          source: "bridge-host",
          type: "error",
          msg: "Invalid dock-action-result JSON",
        });
        emit("dock_action_result_json_error", { jsonText: String(jsonText) });
        return false;
      }
    },

    sendDockAction(action) {
      const out = bridge.sendAction(action);
      emit("dock_action", { action, result: out });
      return out;
    },
  };

  return host;
}

function attachAegisDockBridgeHostToWindow(targetWindow, host, options) {
  const win = targetWindow || (typeof window !== "undefined" ? window : null);
  if (!win) return null;
  const opts = options || {};
  const key = opts.key || "aegisDockHost";
  win[key] = host;
  return host;
}

function createWindowAegisDockBridgeHost(options) {
  const host = createAegisDockBridgeHost(options);
  if (typeof window !== "undefined") {
    attachAegisDockBridgeHostToWindow(window, host, options);
  }
  return host;
}

const exported = {
  createAegisDockBridgeHost,
  createWindowAegisDockBridgeHost,
  attachAegisDockBridgeHostToWindow,
};

if (typeof module !== "undefined" && module.exports) {
  module.exports = exported;
}

if (typeof window !== "undefined") {
  window.AegisDockBridgeHost = exported;
}
