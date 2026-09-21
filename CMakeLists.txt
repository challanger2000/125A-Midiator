cmake_minimum_required(VERSION 3.25)

project(Midiator VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "Run VST3 validator after building" FORCE)
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "Create VST3 plugin link" FORCE)
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "Do not build SDK plug-in examples" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES ON CACHE BOOL "Enable SDK hosting support" FORCE)
set(SMTG_ENABLE_VSTGUI_SUPPORT ON CACHE BOOL "Enable VSTGUI support" FORCE)

include(FetchContent)

FetchContent_Declare(vst3sdk
    GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
    GIT_TAG v3.8.1_build_84
)
FetchContent_MakeAvailable(vst3sdk)

enable_testing()
add_subdirectory(src)
add_subdirectory(tests)
