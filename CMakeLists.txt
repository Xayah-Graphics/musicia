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
include(cmake/miniaudio.cmake)

add_library(music_core STATIC music/audio.cpp)
target_sources(music_core PUBLIC FILE_SET CXX_MODULES FILES music/audio.ixx music/timeline.ixx)
target_link_libraries(music_core PUBLIC miniaudio)
set_target_properties(music_core PROPERTIES CXX_MODULE_STD ON)

# ============================================================================
# Executables
# ============================================================================
if(WIN32)
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
                COMMAND "${SLANG_COMPILER}" "${CMAKE_CURRENT_SOURCE_DIR}/music/shaders/${SHADER}.slang" -entry ${ENTRY} -target spirv -profile spirv_1_6 -O3 -o "${OUTPUT}"
                DEPENDS "music/shaders/${SHADER}.slang" "${SLANG_COMPILER}" VERBATIM)
            list(APPEND MUSIC_SHADERS "${OUTPUT}")
        endforeach()
    endforeach()
    add_custom_target(music_shaders DEPENDS ${MUSIC_SHADERS})

    add_executable(music WIN32 main.cpp music/renderer.cpp music/window.cpp)
    target_sources(music PRIVATE FILE_SET CXX_MODULES FILES music/renderer.ixx music/window.ixx)
    target_link_libraries(music PRIVATE music_core vulkan_module dwrite dwmapi ole32 shell32 user32 gdi32)
    target_compile_definitions(music PRIVATE UNICODE _UNICODE)
    target_compile_options(music PRIVATE /utf-8 /W4 /permissive-)
    target_compile_options(music_core PRIVATE /utf-8 /W4 /permissive-)
    set_target_properties(music PROPERTIES CXX_MODULE_STD ON)
    add_dependencies(music music_shaders)
endif()
