include(FetchContent)
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(vst3
    EXCLUDE_FROM_ALL
    GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
    GIT_TAG 3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96
    GIT_SUBMODULES base cmake pluginterfaces public.sdk
    GIT_SUBMODULES_RECURSE OFF
    GIT_PROGRESS TRUE)
FetchContent_MakeAvailable(vst3)
target_sources(sdk_hosting PRIVATE
    "${vst3_SOURCE_DIR}/public.sdk/source/common/memorystream.cpp"
    "${vst3_SOURCE_DIR}/public.sdk/source/vst/hosting/module_win32.cpp"
    "${vst3_SOURCE_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp")
target_link_libraries(sdk_hosting PUBLIC shlwapi)
