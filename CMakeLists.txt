cmake_minimum_required(VERSION 3.22)

project(Midiator VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "Run VST3 validator after building" FORCE)
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "Create VST3 plugin link" FORCE)
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "Do not build SDK plug-in examples" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "Do not build SDK host examples" FORCE)

include(FetchContent)

FetchContent_Declare(vst3sdk
    GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
    GIT_TAG 3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96
)
FetchContent_MakeAvailable(vst3sdk)

enable_testing()
add_subdirectory(src)
add_subdirectory(tests)
