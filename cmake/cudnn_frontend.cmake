include(FetchContent)
set(CUDNN_FRONTEND_DOWNLOAD "${PROJECT_SOURCE_DIR}/assets/downloads/cudnn-frontend-v1.31.0.tar.gz")
set(CUDNN_FRONTEND_HASH 130f3e98e3f07defd60bf39b250ae7d96c61ca99443782ce77fd470964fbd899)
file(MAKE_DIRECTORY "${PROJECT_SOURCE_DIR}/assets/downloads")
file(DOWNLOAD
    https://github.com/NVIDIA/cudnn-frontend/archive/refs/tags/v1.31.0.tar.gz
    "${CUDNN_FRONTEND_DOWNLOAD}"
    EXPECTED_HASH "SHA256=${CUDNN_FRONTEND_HASH}"
    TLS_VERIFY ON SHOW_PROGRESS)
FetchContent_Declare(cudnn_frontend
    URL "${CUDNN_FRONTEND_DOWNLOAD}"
    URL_HASH "SHA256=${CUDNN_FRONTEND_HASH}"
    SOURCE_SUBDIR unused
)
FetchContent_MakeAvailable(cudnn_frontend)
set(CUDNN_HEURISTICS_HEADER "${cudnn_frontend_SOURCE_DIR}/include/cudnn_frontend_Heuristics.h")
file(READ "${CUDNN_HEURISTICS_HEADER}" CUDNN_HEURISTICS_SOURCE)
string(REPLACE "static inline" "inline" CUDNN_HEURISTICS_MODULE_SOURCE "${CUDNN_HEURISTICS_SOURCE}")
if(NOT CUDNN_HEURISTICS_MODULE_SOURCE STREQUAL CUDNN_HEURISTICS_SOURCE)
    file(WRITE "${CUDNN_HEURISTICS_HEADER}" "${CUDNN_HEURISTICS_MODULE_SOURCE}")
endif()
add_library(cudnn_frontend INTERFACE)
target_include_directories(cudnn_frontend SYSTEM INTERFACE "${cudnn_frontend_SOURCE_DIR}/include")
target_compile_definitions(cudnn_frontend INTERFACE CUDNN_FRONTEND_SKIP_JSON_LIB)
target_link_libraries(cudnn_frontend INTERFACE cudnn CUDA::cudart CUDA::nvrtc)
