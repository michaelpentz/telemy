cmake_minimum_required(VERSION 3.20)
project(aegis_obs_plugin_shim LANGUAGES CXX)

option(AEGIS_BUILD_OBS_PLUGIN "Build OBS plugin target (requires OBS Studio SDK)" OFF)
option(AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST "Enable OBS plugin browser-dock host wiring scaffold" OFF)
option(AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_OBS_CEF "Enable OBS browser-panel/QCef-backed dock host implementation" OFF)
option(AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_QT_WEBENGINE "Enable Qt WebEngine-backed OBS browser dock host implementation" OFF)
option(AEGIS_STAGE_DOCK_BRIDGE_ASSETS "Stage dock bridge JS assets next to OBS plugin build outputs" ON)
set(AEGIS_DOCK_BRIDGE_ASSET_SOURCE_ROOT "" CACHE PATH "Optional override root containing aegis-dock-bridge*.js assets")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(aegis_shim_core STATIC
  src/ipc_client.cpp
  src/shim_runtime.cpp
)

target_include_directories(aegis_shim_core PUBLIC src)
target_compile_definitions(aegis_shim_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

if (WIN32)
  target_link_libraries(aegis_shim_core PRIVATE ws2_32)
endif()

add_executable(aegis_plugin_shim_harness
  src/harness_main.cpp
)
target_link_libraries(aegis_plugin_shim_harness PRIVATE aegis_shim_core)

if (AEGIS_BUILD_OBS_PLUGIN)
  if (NOT OBS_INCLUDE_DIRS)
    message(FATAL_ERROR "AEGIS_BUILD_OBS_PLUGIN=ON requires OBS_INCLUDE_DIRS (path(s) containing obs-module.h / obs-frontend-api.h).")
  endif()
  if (NOT OBS_LIBRARIES)
    message(FATAL_ERROR "AEGIS_BUILD_OBS_PLUGIN=ON requires OBS_LIBRARIES (e.g. obs + obs-frontend-api import libs).")
  endif()

  add_library(aegis-obs-shim MODULE src/obs_plugin_entry.cpp)
  target_link_libraries(aegis-obs-shim PRIVATE aegis_shim_core)
  target_include_directories(aegis-obs-shim PRIVATE ${OBS_INCLUDE_DIRS})
  if (OBS_LIBRARY_DIRS)
    target_link_directories(aegis-obs-shim PRIVATE ${OBS_LIBRARY_DIRS})
  endif()
  target_link_libraries(aegis-obs-shim PRIVATE ${OBS_LIBRARIES})
  target_compile_definitions(aegis-obs-shim PRIVATE AEGIS_OBS_PLUGIN_BUILD=1 WIN32_LEAN_AND_MEAN NOMINMAX)

  if (AEGIS_STAGE_DOCK_BRIDGE_ASSETS)
    set(_aegis_dock_asset_root "")
    set(_aegis_dock_bridge_core_name "")
    if (AEGIS_DOCK_BRIDGE_ASSET_SOURCE_ROOT)
      set(_aegis_dock_asset_root "${AEGIS_DOCK_BRIDGE_ASSET_SOURCE_ROOT}")
      if (EXISTS "${_aegis_dock_asset_root}/aegis-dock-bridge.js")
        set(_aegis_dock_bridge_core_name "aegis-dock-bridge.js")
      elseif (EXISTS "${_aegis_dock_asset_root}/aegis-dock-bridge.global.js")
        set(_aegis_dock_bridge_core_name "aegis-dock-bridge.global.js")
      endif()
    else()
      foreach(_candidate
          "${CMAKE_CURRENT_LIST_DIR}/.."
          "${CMAKE_CURRENT_LIST_DIR}/../..")
        if ((EXISTS "${_candidate}/aegis-dock-bridge.global.js"
             OR EXISTS "${_candidate}/aegis-dock-bridge.js")
            AND EXISTS "${_candidate}/aegis-dock-bridge-host.js"
            AND EXISTS "${_candidate}/aegis-dock-browser-host-bootstrap.js")
          set(_aegis_dock_asset_root "${_candidate}")
          if (EXISTS "${_candidate}/aegis-dock-bridge.js")
            set(_aegis_dock_bridge_core_name "aegis-dock-bridge.js")
          else()
            set(_aegis_dock_bridge_core_name "aegis-dock-bridge.global.js")
          endif()
          break()
        endif()
      endforeach()
    endif()

    if (_aegis_dock_asset_root AND _aegis_dock_bridge_core_name)
      file(TO_CMAKE_PATH "${_aegis_dock_asset_root}" _aegis_dock_asset_root_norm)
      set(_aegis_dock_html_source "")
      foreach(_dock_html_candidate
          "${_aegis_dock_asset_root}/aegis-dock.html"
          "${CMAKE_CURRENT_LIST_DIR}/../aegis-dock.html"
          "${CMAKE_CURRENT_LIST_DIR}/../../aegis-dock.html")
        if (EXISTS "${_dock_html_candidate}")
          set(_aegis_dock_html_source "${_dock_html_candidate}")
          break()
        endif()
      endforeach()
      set(_aegis_dock_stage_dir "$<TARGET_FILE_DIR:aegis-obs-shim>/data/obs-plugins/aegis-obs-shim")
      add_custom_command(
        TARGET aegis-obs-shim POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_aegis_dock_stage_dir}"
        VERBATIM)
      foreach(_asset_name
          "${_aegis_dock_bridge_core_name}"
          "aegis-dock-bridge-host.js"
          "aegis-dock-browser-host-bootstrap.js")
        add_custom_command(
          TARGET aegis-obs-shim POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_aegis_dock_asset_root_norm}/${_asset_name}"
            "${_aegis_dock_stage_dir}/${_asset_name}"
          VERBATIM)
      endforeach()
      if (_aegis_dock_bridge_core_name STREQUAL "aegis-dock-bridge.js")
        add_custom_command(
          TARGET aegis-obs-shim POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E rm -f "${_aegis_dock_stage_dir}/aegis-dock-bridge.global.js"
          VERBATIM)
      elseif (_aegis_dock_bridge_core_name STREQUAL "aegis-dock-bridge.global.js")
        add_custom_command(
          TARGET aegis-obs-shim POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E rm -f "${_aegis_dock_stage_dir}/aegis-dock-bridge.js"
          VERBATIM)
      endif()
      if (_aegis_dock_html_source)
        file(TO_CMAKE_PATH "${_aegis_dock_html_source}" _aegis_dock_html_source_norm)
        add_custom_command(
          TARGET aegis-obs-shim POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_aegis_dock_html_source_norm}"
            "${_aegis_dock_stage_dir}/aegis-dock.html"
          VERBATIM)
      endif()
      message(STATUS "Aegis OBS shim dock assets will be staged from: ${_aegis_dock_asset_root_norm}")
    else()
      message(WARNING "AEGIS_STAGE_DOCK_BRIDGE_ASSETS=ON but dock bridge JS assets were not found; set AEGIS_DOCK_BRIDGE_ASSET_SOURCE_ROOT or rely on AEGIS_DOCK_BRIDGE_ROOT at runtime.")
    endif()
  endif()

  if (AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST)
    target_sources(aegis-obs-shim PRIVATE src/obs_browser_dock_host_scaffold.cpp)
    target_compile_definitions(aegis-obs-shim PRIVATE AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST=1)
    if (AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_OBS_CEF)
      find_package(Qt6 COMPONENTS Core Widgets QUIET)
      set(_obs_browser_panel_include "")
      if (EXISTS "${CMAKE_CURRENT_LIST_DIR}/../third_party/obs-studio/plugins/obs-browser/panel/browser-panel.hpp")
        set(_obs_browser_panel_include "${CMAKE_CURRENT_LIST_DIR}/../third_party/obs-studio/plugins/obs-browser/panel")
      elseif (EXISTS "${CMAKE_CURRENT_LIST_DIR}/third_party/obs-studio/plugins/obs-browser/panel/browser-panel.hpp")
        set(_obs_browser_panel_include "${CMAKE_CURRENT_LIST_DIR}/third_party/obs-studio/plugins/obs-browser/panel")
      endif()

      if (Qt6_FOUND AND _obs_browser_panel_include)
        target_compile_definitions(aegis-obs-shim PRIVATE AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_OBS_CEF=1)
        target_include_directories(aegis-obs-shim PRIVATE "${_obs_browser_panel_include}")
        target_link_libraries(aegis-obs-shim PRIVATE
          Qt6::Core
          Qt6::Widgets
        )
      else()
        message(WARNING "AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_OBS_CEF=ON but Qt6 Core/Widgets and/or obs-browser panel headers were not found; building scaffold fallback only.")
      endif()
    endif()
    if (AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_QT_WEBENGINE)
      find_package(Qt6 COMPONENTS Core Widgets WebEngineCore WebEngineWidgets QUIET)
      if (Qt6_FOUND)
        target_compile_definitions(aegis-obs-shim PRIVATE AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_QT_WEBENGINE=1)
        target_link_libraries(aegis-obs-shim PRIVATE
          Qt6::Core
          Qt6::Widgets
          Qt6::WebEngineCore
          Qt6::WebEngineWidgets
        )
      else()
        message(WARNING "AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_QT_WEBENGINE=ON but Qt6 WebEngine components were not found; building scaffold fallback only.")
      endif()
    endif()
  endif()
endif()
