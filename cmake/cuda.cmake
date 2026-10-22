find_package(CUDAToolkit 13.4 REQUIRED)
if(WIN32)
    file(GLOB CUDA_RUNTIME
        "${CUDAToolkit_BIN_DIR}/x64/cudart64_*.dll"
        "${CUDAToolkit_BIN_DIR}/x64/cublas64_*.dll"
        "${CUDAToolkit_BIN_DIR}/x64/cublasLt64_*.dll"
        "${CUDAToolkit_BIN_DIR}/x64/cufft64_*.dll"
        "${CUDAToolkit_BIN_DIR}/x64/nvrtc*.dll")
endif()
