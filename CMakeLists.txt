cmake_minimum_required(VERSION 3.25)
project(RogueMercenaries VERSION 0.1.0 LANGUAGES CXX)

option(RM_BUILD_TESTS "Build the rm_tests Catch2 suite and register it with CTest" ON)
option(RM_BUILD_SIM "Build the rm_sim headless run simulator" ON)
option(RM_WARNINGS_AS_ERRORS "Treat compiler warnings in project code as errors" ON)

enable_testing()

# Static CRT everywhere: the RE5 adapter DLL must not depend on whichever
# Visual C++ runtime the player has installed, and every linked library has to
# use the same runtime flavour.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Warning policy for project-owned targets (third-party code is compiled separately).
function(rm_project_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
        if(RM_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wno-sign-conversion)
        if(RM_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

if(WIN32)
    add_executable(proxy_smoke tools/proxy_smoke.cpp)
    target_compile_features(proxy_smoke PRIVATE cxx_std_17)
    target_compile_definitions(proxy_smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# --- third party -----------------------------------------------------------------------------
add_library(rm_json INTERFACE)
target_include_directories(rm_json SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann/include)

# --- portable core ---------------------------------------------------------------------------
# Game-independent rules library. Must build for Win32 (linked into the RE5 adapter DLL) and
# x64 (tests/simulator). No windows.h in public headers, no game addresses.
add_library(rm_core STATIC
    core/common/capabilities.cpp
    core/common/rarity.cpp
    core/common/status.cpp
    core/rng/rng.cpp
    core/content/content_loader.cpp
    core/characters/characters.cpp
    core/perks/perks.cpp
    core/weapons/weapons.cpp
    core/economy/ledger.cpp
    core/economy/shop.cpp
    core/encounters/routes.cpp
    core/rewards/rewards.cpp
    core/runs/run_controller.cpp
    core/director/director.cpp
    core/runs/combat.cpp
    core/runs/snapshot.cpp
    core/persistence/profile.cpp
    core/telemetry/events.cpp
    core/telemetry/jsonl_sink.cpp
    core/diagnostics/zip.cpp
    core/diagnostics/export.cpp
)
target_include_directories(rm_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core/include)
target_compile_features(rm_core PUBLIC cxx_std_20)
target_link_libraries(rm_core PRIVATE rm_json)
rm_project_warnings(rm_core)

# --- tests -----------------------------------------------------------------------------------
if(RM_BUILD_TESTS)
    add_library(rm_catch2 STATIC third_party/catch2/catch_amalgamated.cpp)
    target_include_directories(rm_catch2 SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/catch2)
    target_compile_features(rm_catch2 PUBLIC cxx_std_20)
    if(MSVC)
        target_compile_options(rm_catch2 PRIVATE /W0 /utf-8)
    endif()

    add_executable(rm_tests
        tests/test_support.cpp
        tests/test_rng.cpp
        tests/test_content.cpp
        tests/test_perks.cpp
        tests/test_weapons.cpp
        tests/test_economy.cpp
        tests/test_rewards.cpp
        tests/test_run_flow.cpp
        tests/test_persistence.cpp
        tests/test_telemetry.cpp
        tests/test_characters.cpp
        tests/test_skins.cpp
        tests/test_items.cpp
        tests/test_director.cpp
    )
    target_link_libraries(rm_tests PRIVATE rm_core rm_json rm_catch2)
    target_compile_definitions(rm_tests PRIVATE RM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    rm_project_warnings(rm_tests)

    set(RM_TEST_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/test_output)
    foreach(tag rng content perks weapons economy shop rewards routes run persistence telemetry characters items director)
        add_test(NAME rm_tests.${tag} COMMAND rm_tests "[${tag}]" --reporter compact)
    endforeach()
    # Safety net: any test without one of the tags above still runs.
    add_test(NAME rm_tests.untagged
        COMMAND rm_tests "~[rng]~[content]~[perks]~[weapons]~[economy]~[shop]~[rewards]~[routes]~[run]~[persistence]~[telemetry]~[zip]"
                --allow-running-no-tests --reporter compact)
    # The ZIP tests also leave archives behind for an independent reader (Python zipfile).
    add_test(NAME rm_tests.zip COMMAND rm_tests "[zip]" --reporter compact)
    set_tests_properties(rm_tests.zip PROPERTIES
        ENVIRONMENT "RM_TEST_OUTPUT_DIR=${RM_TEST_OUTPUT_DIR}"
        FIXTURES_SETUP rm_zip_files)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME rm_zip_python_reader
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_zip.py
                    ${RM_TEST_OUTPUT_DIR}/stored.zip ${RM_TEST_OUTPUT_DIR}/diagnostics.zip)
        set_tests_properties(rm_zip_python_reader PROPERTIES FIXTURES_REQUIRED rm_zip_files)
    else()
        message(STATUS "Python3 not found: rm_zip_python_reader test skipped")
    endif()
endif()

# --- simulator -------------------------------------------------------------------------------
if(RM_BUILD_SIM)
    add_executable(rm_sim simulation/rm_sim.cpp)
    target_link_libraries(rm_sim PRIVATE rm_core rm_json)
    rm_project_warnings(rm_sim)
endif()

# --- RE5 adapter (x86 only; maintained separately) -------------------------------------------
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4 AND EXISTS ${CMAKE_SOURCE_DIR}/adapters/re5/CMakeLists.txt)
    add_subdirectory(adapters/re5)
endif()
