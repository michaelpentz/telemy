"use strict";

// Classic-script compatibility bridge for browser-dock embedding.
// Exposes a subset-compatible `window.AegisDockBridge` API so the plugin host can
// run without ESM/module loading support in the embedded page.

(function initAegisDockBridgeGlobal(globalObj) {
  const g = globalObj || (typeof window !== "undefined" ? window : globalThis);
  if (!g) return;

  function nowMs() {
    return Date.now();
  }

  let localRequestSeq = 1;
  function newLocalRequestId(prefix) {
    return String(prefix || "ui") + "-" + String(Date.now()) + "-" + String(localRequestSeq++);
  }

  function pushRing(list, item, max) {
    list.unshift(item);
    if (list.length > max) list.length = max;
  }

  function formatHms(tsUnixMs) {
    if (!tsUnixMs) return "--:--:--";
    const d = new Date(tsUnixMs);
    const hh = String(d.getHours()).padStart(2, "0");
    const mm = String(d.getMinutes()).padStart(2, "0");
    const ss = String(d.getSeconds()).padStart(2, "0");
    return hh + ":" + mm + ":" + ss;
  }

  function resolveSceneIdByName(scenes, sceneName) {
    if (!sceneName || !Array.isArray(scenes)) return null;
    for (let i = 0; i < scenes.length; i += 1) {
      const s = scenes[i];
      if (s && s.name === sceneName) return s.id || null;
    }
    const target = String(sceneName).toLowerCase();
    for (let i = 0; i < scenes.length; i += 1) {
      const s = scenes[i];
      if (s && String(s.name || "").toLowerCase() === target) return s.id || null;
    }
    return null;
  }

  function mapNoticeLevel(level) {
    return level === "warn" ? "warning" : (level || "info");
  }

  function normalizeOutputBitrates(raw) {
    if (!Array.isArray(raw)) return [];
    return raw
      .map((item, idx) => {
        if (!item || typeof item !== "object") return null;
        const platform = item.platform || item.name || item.host || ("Output " + String(idx + 1));
        const kbpsRaw = item.kbps != null ? item.kbps : (item.bitrate_kbps != null ? item.bitrate_kbps : item.bitrate);
        const kbps = Number(kbpsRaw);
        return {
          platform: String(platform),
          kbps: Number.isFinite(kbps) ? kbps : null,
          status: item.status || null,
        };
      })
      .filter(Boolean);
  }

  function sanitizeTheme(theme) {
    return theme && typeof theme === "object" ? Object.assign({}, theme) : null;
  }

  function projectDockState(cache, cfg) {
    const session = cache.session || {};
    const core = cache.core || {};
    const plugin = cache.plugin || {};
    const snapshot = core.statusSnapshot || {};
    const pongStaleMs = (cfg && cfg.pongStaleMs) || 3500;
    const connected = !!session.connected;
    const pongAge = session.lastPongMs ? (nowMs() - session.lastPongMs) : Number.POSITIVE_INFINITY;
    const pipeStatus = plugin.pipeForcedStatus || (!connected ? "down" : (pongAge > pongStaleMs ? "degraded" : "ok"));
    const scenes = Array.isArray(plugin.scenes) ? plugin.scenes : [];
    const activeSceneId = plugin.activeSceneId || resolveSceneIdByName(scenes, plugin.activeSceneName) || null;
    const pendingSceneId =
      (plugin.pendingScene && plugin.pendingScene.sceneId) ||
      resolveSceneIdByName(scenes, plugin.pendingScene && plugin.pendingScene.sceneName) ||
      null;
    const snapshotSettings =
      snapshot && snapshot.settings && typeof snapshot.settings === "object"
        ? snapshot.settings
        : {};
    const autoSceneSwitchEnabled =
      typeof snapshotSettings.auto_scene_switch === "boolean"
        ? snapshotSettings.auto_scene_switch
        : null;
    const manualOverrideEnabled =
      typeof snapshotSettings.manual_override === "boolean"
        ? snapshotSettings.manual_override
        : !!snapshot.override_enabled;
    const autoSwitchArmed =
      manualOverrideEnabled
        ? false
        : (typeof autoSceneSwitchEnabled === "boolean" ? autoSceneSwitchEnabled : true);
    const lowQualityFallback =
      typeof snapshotSettings.low_quality_fallback === "boolean"
        ? snapshotSettings.low_quality_fallback
        : null;
    const chatBot =
      typeof snapshotSettings.chat_bot === "boolean"
        ? snapshotSettings.chat_bot
        : null;
    const alerts =
      typeof snapshotSettings.alerts === "boolean"
        ? snapshotSettings.alerts
        : null;
    const snapshotTheme = sanitizeTheme(snapshot && snapshot.theme);
    const theme = snapshotTheme || sanitizeTheme(core.lastTheme);

    const state = {
      header: {
        mode: snapshot.mode || "studio",
        version: session.coreVersion || "v0.0.3",
      },
      live: {
        isLive: typeof plugin.isLive === "boolean" ? plugin.isLive : (snapshot.health || "offline") !== "offline",
        elapsedSec: plugin.elapsedSec || 0,
      },
      scenes: {
        items: scenes,
        activeSceneId: activeSceneId,
        pendingSceneId: pendingSceneId,
        autoSwitchArmed: autoSwitchArmed,
        autoSwitchEnabled: autoSceneSwitchEnabled,
        manualOverrideEnabled: manualOverrideEnabled,
      },
      connections: {
        items: Array.isArray(plugin.connections) ? plugin.connections : [],
      },
      bitrate: {
        bondedKbps: Number(snapshot.bitrate_kbps || 0),
        relayBondedKbps: Number(
          snapshot.relay_bonded_kbps ||
          (snapshot.relay && (snapshot.relay.bonded_kbps || snapshot.relay.ingest_bonded_kbps)) ||
          snapshot.bitrate_kbps ||
          0
        ),
        outputs: normalizeOutputBitrates(
          snapshot.multistream_outputs ||
          snapshot.output_ingests ||
          (snapshot.multistream && snapshot.multistream.outputs) ||
          []
        ),
      },
      relay: {
        enabled: (snapshot.mode === "irl") || !!(snapshot.relay && snapshot.relay.status && snapshot.relay.status !== "inactive"),
        status: (snapshot.relay && snapshot.relay.status) || "inactive",
        region: snapshot.relay && snapshot.relay.region || null,
        latencyMs: snapshot.rtt_ms == null ? null : snapshot.rtt_ms,
        graceRemainingSeconds: snapshot.relay && snapshot.relay.grace_remaining_seconds != null
          ? snapshot.relay.grace_remaining_seconds
          : null,
      },
      failover: {
        health: snapshot.health || "offline",
        state: (snapshot.mode === "irl") ? "IRL" : "STUDIO",
        responseBudgetMs: 800,
      },
      settings: {
        items: Array.isArray(plugin.settings) && plugin.settings.length
          ? plugin.settings
          : [
              { key: "auto_scene_switch", label: "Auto Scene Switch", value: autoSceneSwitchEnabled, color: "#2ea043" },
              { key: "low_quality_fallback", label: "Low Bitrate Fallback", value: lowQualityFallback, color: "#d29922" },
              { key: "manual_override", label: "Manual Override", value: manualOverrideEnabled, color: "#5ba3f5" },
              { key: "chat_bot", label: "Chat Bot Integration", value: chatBot, color: "#8b8f98" },
              { key: "alerts", label: "Alert on Disconnect", value: alerts, color: "#2d7aed" },
            ],
      },
      events: cache.events.slice(0, 12),
      pipe: {
        status: pipeStatus,
        label: pipeStatus === "ok" ? "PIPE OK" : (pipeStatus === "degraded" ? "PIPE DEGRADED" : "PIPE DOWN"),
      },
      _bridge: {
        protocolVersion: session.protocolVersion || null,
        protocolErrorsRecent: session.protocolErrorsRecent || 0,
        lastEnvelopeType: cache.debug.lastEnvelopeType || null,
        compat: "global",
      },
    };
    if (theme) {
      state.theme = theme;
    }
    return state;
  }

  function createAegisDockBridgeHost(options) {
    const listeners = new Set();
    const cfg = Object.assign({ eventLimit: 50, pongStaleMs: 3500 }, options || {});
    const cache = {
      session: {
        connected: false,
        lastHelloAck: null,
        lastPongMs: null,
        protocolErrorsRecent: 0,
        protocolVersion: null,
        coreVersion: null,
      },
      core: { statusSnapshot: null },
      plugin: {
        scenes: [],
        activeSceneId: null,
        activeSceneName: null,
        pendingScene: null,
        pipeForcedStatus: null,
        connections: null,
        settings: null,
        isLive: null,
        elapsedSec: null,
      },
      events: [],
      debug: {
        lastEnvelopeType: null,
      },
    };

    function emit() {
      const state = projectDockState(cache, cfg);
      listeners.forEach((fn) => {
        try { fn(state); } catch (_e) {}
      });
    }

    function addEvent(type, msg, tsUnixMs) {
      if (!msg) return;
      pushRing(cache.events, {
        id: String((tsUnixMs || nowMs())) + "-" + Math.random().toString(16).slice(2, 8),
        time: formatHms(tsUnixMs || nowMs()),
        type: type || "info",
        msg: String(msg),
      }, cfg.eventLimit);
    }

    return {
      getState() {
        return projectDockState(cache, cfg);
      },

      async sendAction(action) {
        if (!action || !action.type) return false;
        if (action.type === "switch_scene") {
          cache.plugin.pendingScene = {
            requestId: newLocalRequestId("manual"),
            sceneName: action.sceneName || null,
            sceneId: action.sceneId || resolveSceneIdByName(cache.plugin.scenes, action.sceneName),
            tsUnixMs: nowMs(),
          };
          addEvent("info", "Manual scene switch queued: " + String(action.sceneName || action.sceneId || "unknown"));
          emit();
          return true;
        }
        addEvent("info", "Unhandled dock action: " + String(action.type));
        emit();
        return false;
      },

      subscribe(listener) {
        if (typeof listener !== "function") return function noop() {};
        listeners.add(listener);
        return function unsubscribe() {
          listeners.delete(listener);
        };
      },

      handleIpcEnvelope(envelope) {
        if (!envelope || typeof envelope !== "object") return;
        const type = envelope.type || null;
        const payload = envelope.payload || {};
        const ts = envelope.ts_unix_ms || nowMs();
        cache.debug.lastEnvelopeType = type;

        if (type === "hello_ack") {
          cache.session.connected = true;
          cache.session.lastHelloAck = ts;
          cache.session.protocolVersion = payload.protocol_version ?? cache.session.protocolVersion;
          cache.session.coreVersion = payload.core_version ?? cache.session.coreVersion;
          addEvent("info", "IPC handshake complete", ts);
        } else if (type === "pong") {
          cache.session.connected = true;
          cache.session.lastPongMs = ts;
        } else if (type === "status_snapshot") {
          cache.session.connected = true;
          const mergedSnapshot = Object.assign({}, cache.core.statusSnapshot || {}, payload);
          const payloadTheme = sanitizeTheme(payload && payload.theme);
          if (payloadTheme) {
            cache.core.lastTheme = payloadTheme;
            mergedSnapshot.theme = payloadTheme;
          } else if (!sanitizeTheme(mergedSnapshot.theme) && sanitizeTheme(cache.core.lastTheme)) {
            mergedSnapshot.theme = sanitizeTheme(cache.core.lastTheme);
          }
          cache.core.statusSnapshot = mergedSnapshot;
        } else if (type === "switch_scene") {
          cache.plugin.pendingScene = {
            requestId: payload.request_id || null,
            sceneName: payload.scene_name || null,
            sceneId: resolveSceneIdByName(cache.plugin.scenes, payload.scene_name),
            tsUnixMs: ts,
          };
          addEvent("warning", "Core requested scene switch: " + String(payload.scene_name || "unknown"), ts);
        } else if (type === "user_notice") {
          addEvent(mapNoticeLevel(payload.level), payload.message || "User notice", ts);
        } else if (type === "protocol_error") {
          cache.session.protocolErrorsRecent += 1;
          addEvent("error", "IPC protocol error", ts);
        }
        emit();
      },

      setPipeStatus(status) {
        cache.plugin.pipeForcedStatus = status || null;
        emit();
      },

      setObsSceneNames(sceneNames) {
        const names = Array.isArray(sceneNames) ? sceneNames : [];
        cache.plugin.scenes = names.map((name, index) => ({ id: "scene-" + String(index + 1), name: String(name) }));
        if (cache.plugin.activeSceneName) {
          cache.plugin.activeSceneId = resolveSceneIdByName(cache.plugin.scenes, cache.plugin.activeSceneName);
        }
        emit();
      },

      setObsActiveSceneName(sceneName) {
        cache.plugin.activeSceneName = sceneName == null ? null : String(sceneName);
        cache.plugin.activeSceneId = resolveSceneIdByName(cache.plugin.scenes, cache.plugin.activeSceneName);
        if (cache.plugin.pendingScene && cache.plugin.pendingScene.sceneName && cache.plugin.activeSceneName &&
            cache.plugin.pendingScene.sceneName === cache.plugin.activeSceneName) {
          cache.plugin.pendingScene = null;
        }
        emit();
      },

      notifySceneSwitchCompleted(arg) {
        const result = arg || {};
        const ok = !!result.ok;
        if (cache.plugin.pendingScene) {
          cache.plugin.pendingScene = null;
        }
        addEvent(ok ? "success" : "error",
          ok
            ? ("Scene switch applied" + (result.sceneName ? ": " + String(result.sceneName) : ""))
            : ("Scene switch failed" + (result.error ? ": " + String(result.error) : "")));
        emit();
      },

      setConnectionTelemetry(items) {
        cache.plugin.connections = Array.isArray(items) ? items.slice() : [];
        emit();
      },

      setSettings(items) {
        cache.plugin.settings = Array.isArray(items) ? items.slice() : [];
        emit();
      },

      setLiveInfo(info) {
        const v = info || {};
        if (typeof v.isLive === "boolean") cache.plugin.isLive = v.isLive;
        if (typeof v.elapsedSec === "number") cache.plugin.elapsedSec = v.elapsedSec;
        emit();
      },
    };
  }

  function attachAegisDockBridgeToWindow(bridge, key) {
    g[key || "__AEGIS_DOCK_BRIDGE__"] = bridge;
    return bridge;
  }

  const exported = {
    createAegisDockBridgeHost: createAegisDockBridgeHost,
    attachAegisDockBridgeToWindow: attachAegisDockBridgeToWindow,
  };

  if (typeof module !== "undefined" && module.exports) {
    module.exports = exported;
  }
  g.AegisDockBridge = exported;
})(typeof window !== "undefined" ? window : undefined);
