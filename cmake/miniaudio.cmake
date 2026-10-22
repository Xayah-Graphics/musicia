include(FetchContent)
FetchContent_Declare(miniaudio
    URL https://github.com/mackron/miniaudio/archive/9634bedb5b5a2ca38c1ee7108a9358a4e233f14d.zip
    URL_HASH SHA256=d9fc392f394fba13c4d71436fea38a4af384292871d59aa7d728c11fbc4ae978
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    # The implementation is compiled in music.audio; skip the upstream targets.
    SOURCE_SUBDIR extras
)
FetchContent_MakeAvailable(miniaudio)

add_library(miniaudio INTERFACE)
target_include_directories(miniaudio SYSTEM INTERFACE "${miniaudio_SOURCE_DIR}")
target_compile_definitions(miniaudio INTERFACE MA_NO_ENGINE MA_NO_RESOURCE_MANAGER MA_NO_NODE_GRAPH MA_NO_ENCODING MA_NO_GENERATION MA_ENABLE_ONLY_SPECIFIC_BACKENDS)
if(WIN32)
    target_compile_definitions(miniaudio INTERFACE MA_ENABLE_WASAPI NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_definitions(miniaudio INTERFACE MA_ENABLE_PULSEAUDIO)
    target_link_libraries(miniaudio INTERFACE dl pthread m)
endif()
