# ============================================================================
# Project.
# ============================================================================
cmake_minimum_required(VERSION 4.4)
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD f35a9ac6-8463-4d38-8eec-5d6008153e7d)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/VERSION" MUSIC_VERSION LIMIT_COUNT 1)
project(MUSIC VERSION "${MUSIC_VERSION}" LANGUAGES CXX CUDA)

# ============================================================================
# Project Configuration
# ============================================================================
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CUDA_STANDARD 20)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_EXTENSIONS OFF)
set(CMAKE_CUDA_RUNTIME_LIBRARY Shared)
set(CMAKE_CUDA_ARCHITECTURES 120a-real)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================================
# Dependencies
# ============================================================================
include(cmake/cuda.cmake)
include(cmake/ffmpeg.cmake)
include(cmake/cudnn.cmake)
include(cmake/cudnn_frontend.cmake)
include(cmake/sheetsage2.cmake)

add_library(music_core STATIC music/audio.cpp music/analysis.cpp music/sheetsage2/model.cpp music/sheetsage2/network.cpp music/sheetsage2/network.cu)
target_sources(music_core PUBLIC FILE_SET CXX_MODULES FILES music/audio.ixx music/analysis.ixx music/sheetsage2/model.ixx music/sheetsage2/network.ixx)
target_link_libraries(music_core PRIVATE FFmpeg::avformat FFmpeg::avcodec FFmpeg::swresample FFmpeg::avutil CUDA::cublas CUDA::cublasLt CUDA::cufft cudnn_frontend)
target_compile_options(music_core PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:--expt-relaxed-constexpr;--extended-lambda;--fmad=false>)
target_compile_definitions(music_core PUBLIC MUSIC_MODEL_DIRECTORY="${MUSIC_MODEL_DIRECTORY}")
set_target_properties(music_core PROPERTIES CXX_MODULE_STD ON)
if(WIN32)
    target_compile_options(music_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8;/W4;/permissive->)
endif()

add_executable(music-analyze main.cpp)
add_dependencies(music-analyze sheetsage2_weights)
target_link_libraries(music-analyze PRIVATE music_core)
set_target_properties(music-analyze PROPERTIES CXX_MODULE_STD ON)
if(WIN32)
    target_compile_options(music-analyze PRIVATE /utf-8 /W4 /permissive-)
    add_custom_command(TARGET music-analyze POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:music-analyze> $<TARGET_FILE_DIR:music-analyze> COMMAND_EXPAND_LISTS)
    file(GLOB CUDNN_RUNTIME "${cudnn_SOURCE_DIR}/bin/x64/*.dll")
    add_custom_command(TARGET music-analyze POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CUDNN_RUNTIME} ${CUDA_RUNTIME} $<TARGET_FILE_DIR:music-analyze> COMMAND_EXPAND_LISTS)
endif()

# ============================================================================
# Executables
# ============================================================================
option(MUSIC_BUILD_EDITOR "Build the Windows editor" ${WIN32})
if(MUSIC_BUILD_EDITOR)
    include(cmake/vulkan.cmake)
    include(cmake/slang.cmake)

    foreach(SHADER ui wave)
        foreach(STAGE vert frag)
            if(STAGE STREQUAL "vert")
                set(ENTRY vertexMain)
            else()
                set(ENTRY fragmentMain)
            endif()
            set(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shaders/${SHADER}.${STAGE}.spv")
            add_custom_command(OUTPUT "${OUTPUT}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
                COMMAND "${SLANG_COMPILER}" "${CMAKE_CURRENT_SOURCE_DIR}/editor/shaders/${SHADER}.slang" -entry ${ENTRY} -target spirv -profile spirv_1_6 -O3 -o "${OUTPUT}"
                DEPENDS "editor/shaders/${SHADER}.slang" "${SLANG_COMPILER}" VERBATIM)
            list(APPEND MUSIC_SHADERS "${OUTPUT}")
        endforeach()
    endforeach()
    add_custom_target(music_shaders DEPENDS ${MUSIC_SHADERS})

    add_executable(music WIN32 editor/main.cpp editor/renderer.cpp editor/window.cpp editor/playback.cpp editor/waveform.cpp)
    target_sources(music PRIVATE FILE_SET CXX_MODULES FILES editor/renderer.ixx editor/window.ixx editor/playback.ixx editor/waveform.ixx editor/timeline.ixx)
    target_link_libraries(music PRIVATE music_core vulkan_module dwrite dwmapi ole32 shell32 user32 gdi32 avrt)
    target_compile_definitions(music PRIVATE UNICODE _UNICODE)
    target_compile_options(music PRIVATE /utf-8 /W4 /permissive-)
    set_target_properties(music PROPERTIES CXX_MODULE_STD ON)
    add_dependencies(music music_shaders sheetsage2_weights)
    add_custom_command(TARGET music POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:music> $<TARGET_FILE_DIR:music> COMMAND_EXPAND_LISTS)
    add_custom_command(TARGET music POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CUDNN_RUNTIME} ${CUDA_RUNTIME} $<TARGET_FILE_DIR:music> COMMAND_EXPAND_LISTS)
endif()
