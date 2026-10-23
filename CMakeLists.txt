# ============================================================================
# Project.
# ============================================================================
cmake_minimum_required(VERSION 4.4)
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD f35a9ac6-8463-4d38-8eec-5d6008153e7d)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/VERSION" MUSICIA_VERSION LIMIT_COUNT 1)
project(MUSICIA VERSION "${MUSICIA_VERSION}" LANGUAGES CXX CUDA)

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

add_library(musicia_core STATIC musicia/audio.cpp musicia/analysis.cpp musicia/sheetsage2/model.cpp musicia/sheetsage2/network.cpp musicia/sheetsage2/network.cu)
target_sources(musicia_core PUBLIC FILE_SET CXX_MODULES FILES musicia/audio.ixx musicia/analysis.ixx musicia/sheetsage2/model.ixx musicia/sheetsage2/network.ixx)
target_link_libraries(musicia_core PRIVATE FFmpeg::avformat FFmpeg::avcodec FFmpeg::swresample FFmpeg::avutil CUDA::cublas CUDA::cublasLt CUDA::cufft cudnn_frontend)
target_compile_options(musicia_core PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:--expt-relaxed-constexpr;--extended-lambda;--fmad=false>)
set_target_properties(musicia_core PROPERTIES CXX_MODULE_STD ON)
if(WIN32)
    target_compile_options(musicia_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8;/W4;/permissive->)
endif()

add_executable(musicia musicia/main.cpp)
add_dependencies(musicia sheetsage2_weights)
target_link_libraries(musicia PRIVATE musicia_core)
target_compile_definitions(musicia PRIVATE MUSICIA_MODEL_DIRECTORY="${MUSICIA_MODEL_DIRECTORY}")
set_target_properties(musicia PROPERTIES CXX_MODULE_STD ON)
if(WIN32)
    target_compile_options(musicia PRIVATE /utf-8 /W4 /permissive-)
    add_custom_command(TARGET musicia POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:musicia> $<TARGET_FILE_DIR:musicia> COMMAND_EXPAND_LISTS)
    file(GLOB CUDNN_RUNTIME "${cudnn_SOURCE_DIR}/bin/x64/*.dll")
    add_custom_command(TARGET musicia POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CUDNN_RUNTIME} ${CUDA_RUNTIME} $<TARGET_FILE_DIR:musicia> COMMAND_EXPAND_LISTS)
endif()

# ============================================================================
# Executables
# ============================================================================
option(MUSICIA_BUILD_EDITOR "Build the Windows editor" ${WIN32})
option(MUSICIA_BUILD_MINIDAW "Build the Windows MiniDAW tool" ${WIN32})
if(MUSICIA_BUILD_EDITOR OR MUSICIA_BUILD_MINIDAW)
    include(cmake/vulkan.cmake)
    include(cmake/slang.cmake)
endif()

if(MUSICIA_BUILD_EDITOR)
    foreach(SHADER ui wave)
        foreach(STAGE vert frag)
            if(STAGE STREQUAL "vert")
                set(ENTRY vertex_main)
            else()
                set(ENTRY fragment_main)
            endif()
            set(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shaders/${SHADER}.${STAGE}.spv")
            add_custom_command(OUTPUT "${OUTPUT}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
                COMMAND "${SLANG_COMPILER}" "${CMAKE_CURRENT_SOURCE_DIR}/editor/shaders/${SHADER}.slang" -entry ${ENTRY} -target spirv -profile spirv_1_6 -O3 -o "${OUTPUT}"
                DEPENDS "editor/shaders/${SHADER}.slang" "${SLANG_COMPILER}" VERBATIM)
            list(APPEND MUSICIA_SHADERS "${OUTPUT}")
        endforeach()
    endforeach()
    add_custom_target(musicia_shaders DEPENDS ${MUSICIA_SHADERS})

    add_executable(editor WIN32 editor/main.cpp editor/renderer.cpp editor/window.cpp editor/playback.cpp editor/waveform.cpp)
    target_sources(editor PRIVATE FILE_SET CXX_MODULES FILES editor/renderer.ixx editor/window.ixx editor/playback.ixx editor/waveform.ixx editor/timeline.ixx)
    target_link_libraries(editor PRIVATE musicia_core vulkan_module dwrite dwmapi ole32 shell32 user32 gdi32 avrt)
    target_compile_definitions(editor PRIVATE UNICODE _UNICODE MUSICIA_MODEL_DIRECTORY="${MUSICIA_MODEL_DIRECTORY}")
    target_compile_options(editor PRIVATE /utf-8 /W4 /permissive-)
    set_target_properties(editor PROPERTIES CXX_MODULE_STD ON)
    add_dependencies(editor musicia_shaders sheetsage2_weights)
    add_custom_command(TARGET editor POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:editor> $<TARGET_FILE_DIR:editor> COMMAND_EXPAND_LISTS)
    add_custom_command(TARGET editor POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CUDNN_RUNTIME} ${CUDA_RUNTIME} $<TARGET_FILE_DIR:editor> COMMAND_EXPAND_LISTS)
endif()

if(MUSICIA_BUILD_MINIDAW)
    include(cmake/vst3.cmake)

    foreach(STAGE vert frag)
        if(STAGE STREQUAL "vert")
            set(ENTRY vertex_main)
        else()
            set(ENTRY fragment_main)
        endif()
        set(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shaders/minidaw/ui.${STAGE}.spv")
        add_custom_command(OUTPUT "${OUTPUT}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders/minidaw"
            COMMAND "${SLANG_COMPILER}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/minidaw/shaders/ui.slang" -entry ${ENTRY} -target spirv -profile spirv_1_6 -O3 -o "${OUTPUT}"
            DEPENDS "tools/minidaw/shaders/ui.slang" "${SLANG_COMPILER}" VERBATIM)
        list(APPEND MINIDAW_SHADERS "${OUTPUT}")
    endforeach()
    add_custom_target(minidaw_shaders DEPENDS ${MINIDAW_SHADERS})

    add_executable(minidaw WIN32 tools/minidaw/main.cpp tools/minidaw/document.cpp tools/minidaw/engine.cpp tools/minidaw/renderer.cpp tools/minidaw/window.cpp)
    target_sources(minidaw PRIVATE FILE_SET CXX_MODULES FILES tools/minidaw/document.ixx tools/minidaw/engine.ixx tools/minidaw/renderer.ixx tools/minidaw/window.ixx)
    target_link_libraries(minidaw PRIVATE sdk_hosting vulkan_module dwrite dwmapi ole32 uuid shell32 user32 gdi32 avrt)
    target_compile_definitions(minidaw PRIVATE UNICODE _UNICODE)
    target_compile_options(minidaw PRIVATE /utf-8 /W4 /permissive-)
    set_target_properties(minidaw PROPERTIES CXX_MODULE_STD ON)
    add_dependencies(minidaw minidaw_shaders)
endif()
