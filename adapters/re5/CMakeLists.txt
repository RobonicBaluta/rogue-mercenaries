# RE5 adapter: builds dinput8.dll (Win32 only). Included from the top-level
# CMakeLists.txt when configuring a 32-bit build; can also be configured on
# its own (diagnostic build without the core).

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.25)
    project(RogueMercenariesRe5Adapter LANGUAGES C CXX)
endif()
enable_language(C)

set(RM_THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party)

add_library(rm_minhook STATIC
    ${RM_THIRD_PARTY}/minhook/src/buffer.c
    ${RM_THIRD_PARTY}/minhook/src/hook.c
    ${RM_THIRD_PARTY}/minhook/src/trampoline.c
    ${RM_THIRD_PARTY}/minhook/src/hde/hde32.c)
target_include_directories(rm_minhook PUBLIC ${RM_THIRD_PARTY}/minhook/include)
set_target_properties(rm_minhook PROPERTIES LINKER_LANGUAGE C)
target_compile_options(rm_minhook PRIVATE /W0)

add_library(rm_imgui STATIC
    ${RM_THIRD_PARTY}/imgui/imgui.cpp
    ${RM_THIRD_PARTY}/imgui/imgui_draw.cpp
    ${RM_THIRD_PARTY}/imgui/imgui_tables.cpp
    ${RM_THIRD_PARTY}/imgui/imgui_widgets.cpp
    ${RM_THIRD_PARTY}/imgui/backends/imgui_impl_dx9.cpp
    ${RM_THIRD_PARTY}/imgui/backends/imgui_impl_win32.cpp)
target_include_directories(rm_imgui PUBLIC ${RM_THIRD_PARTY}/imgui ${RM_THIRD_PARTY}/imgui/backends)
target_compile_features(rm_imgui PUBLIC cxx_std_17)
target_compile_options(rm_imgui PRIVATE /W0)

set(RM_RE5_SOURCES
    src/dllmain.cpp
    src/proxy.cpp
    src/log.cpp
    src/crash.cpp
    src/memory.cpp
    src/engine.cpp
    src/hooks_combat.cpp
    src/hooks_merc.cpp
    src/hooks_input.cpp
    src/hooks_d3d9.cpp
    src/overlay.cpp
    src/icons.cpp
    src/portraits.cpp
    src/settings.cpp
    src/dev_audio.cpp
    src/dev_capture.cpp
    src/probe.cpp
    src/menu_probe.cpp
    src/native_menu.cpp
    src/autopilot.cpp
    src/match_ui.cpp
    src/itembox_guard.cpp
    src/director.cpp
    src/resources.cpp
    src/world.cpp
    src/host.cpp
    dinput8.def)

if(TARGET rm_core AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/mod.cpp)
    list(APPEND RM_RE5_SOURCES src/mod.cpp)
    set(RM_RE5_WITH_CORE ON)
else()
    list(APPEND RM_RE5_SOURCES src/mod_diag.cpp)
    set(RM_RE5_WITH_CORE OFF)
endif()
message(STATUS "RE5 adapter: roguelite core linked = ${RM_RE5_WITH_CORE}")

add_library(rm_re5 SHARED ${RM_RE5_SOURCES})
set_target_properties(rm_re5 PROPERTIES OUTPUT_NAME dinput8)
target_compile_features(rm_re5 PRIVATE cxx_std_20)
target_compile_definitions(rm_re5 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(rm_re5 PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_include_directories(rm_re5 PRIVATE src ${RM_THIRD_PARTY}/nlohmann/include)
target_link_libraries(rm_re5 PRIVATE rm_minhook rm_imgui d3d9 dxguid ole32 shell32 user32 windowscodecs)
if(RM_RE5_WITH_CORE)
    target_link_libraries(rm_re5 PRIVATE rm_core)
endif()
# Keep the PDB for crash triage; it never ships inside the game folder.
target_link_options(rm_re5 PRIVATE /DEBUG /OPT:REF /OPT:ICF)
