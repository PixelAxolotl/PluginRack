cmake_minimum_required(VERSION 3.28)
project(PluginRack VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()
add_subdirectory(core)
add_subdirectory(tools)
add_subdirectory(tests)

option(PLUGINRACK_BUILD_APP "Build the JUCE desktop app when its SDKs are available" ON)
set(JUCE_DIR "$ENV{JUCE_DIR}" CACHE PATH "Path to the JUCE source tree")
set(VST3_SDK_DIR "$ENV{VST3_SDK_DIR}" CACHE PATH "Path to the VST3 SDK")

# PR-04: recorded SDK versions. The app is only tested against these; other
# versions may build but are not supported. See README "Supported versions".
set(PLUGINRACK_JUCE_VERSION "9.0.4")
set(PLUGINRACK_JUCE_COMMIT "454fe891acc8204f99f4ef1e3a28c84b1a4da145")
set(PLUGINRACK_VST3_VERSION "3.8.1")
set(PLUGINRACK_VST3_COMMIT "3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96")

# Warn (do not fail) when the SDKs present do not match the recorded ones:
# a different checkout can behave differently, and the user should know.
if(EXISTS "${JUCE_DIR}/modules/juce_core/juce_core.h")
    file(READ "${JUCE_DIR}/modules/juce_core/juce_core.h" juce_core_h)
    string(REGEX MATCH "version:[ \t]*([0-9]+\\.[0-9]+\\.[0-9]+)" _ "${juce_core_h}")
    if(CMAKE_MATCH_1 AND NOT CMAKE_MATCH_1 STREQUAL PLUGINRACK_JUCE_VERSION)
        message(WARNING "JUCE version ${CMAKE_MATCH_1} does not match the recorded "
                        "${PLUGINRACK_JUCE_VERSION} (commit ${PLUGINRACK_JUCE_COMMIT})")
    endif()
endif()
if(EXISTS "${VST3_SDK_DIR}/CMakeLists.txt")
    file(READ "${VST3_SDK_DIR}/CMakeLists.txt" vst3_cmake)
    string(REGEX MATCH "VERSION[ \t]+([0-9]+\\.[0-9]+\\.[0-9]+)" _ "${vst3_cmake}")
    if(CMAKE_MATCH_1 AND NOT CMAKE_MATCH_1 STREQUAL PLUGINRACK_VST3_VERSION)
        message(WARNING "VST3 SDK version ${CMAKE_MATCH_1} does not match the recorded "
                        "${PLUGINRACK_VST3_VERSION} (commit ${PLUGINRACK_VST3_COMMIT})")
    endif()
endif()

# The opt-in WASAPI probe cap needs a one-function patch in the external JUCE
# tree. Applying it is idempotent and non-fatal: without the patch the setting
# is inert and builds still work.
option(PLUGINRACK_PATCH_JUCE_WASAPI_PROBE
       "Apply PluginRack's opt-in WASAPI probe cap to JUCE_DIR at configure time" ON)
if(PLUGINRACK_BUILD_APP AND PLUGINRACK_PATCH_JUCE_WASAPI_PROBE)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
                "-DJUCE_DIR=${JUCE_DIR}"
                "-DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tools/juce-patches/0001-wasapi-probe-cap.patch"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/juce-patches/apply-wasapi-probe-cap.cmake")
endif()

if(PLUGINRACK_BUILD_APP)
    if(EXISTS "${JUCE_DIR}/CMakeLists.txt"
       AND EXISTS "${VST3_SDK_DIR}/pluginterfaces/base/fplatform.h")
        add_subdirectory(app)
    else()
        message(WARNING "Skipping pluginrack_app: set JUCE_DIR and VST3_SDK_DIR to valid SDK paths")
    endif()
endif()
