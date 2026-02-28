#pragma once

#if defined(AEGIS_OBS_PLUGIN_BUILD) && defined(AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST)

#include "dock_js_bridge_api.h"

// Compile-gated scaffold for future OBS browser dock (Qt/CEF) embedding.
// Current implementation is a no-op that preserves the JS executor ABI seam.

void aegis_obs_browser_dock_host_scaffold_initialize();
void aegis_obs_browser_dock_host_scaffold_shutdown();

// Helpers for the future dock page lifecycle wiring.
void aegis_obs_browser_dock_host_scaffold_set_js_executor(
    aegis_dock_js_execute_fn fn,
    void* user_data);
void aegis_obs_browser_dock_host_scaffold_on_page_ready();
void aegis_obs_browser_dock_host_scaffold_on_page_unloaded();
bool aegis_obs_browser_dock_host_scaffold_show_dock();

#endif
