include(FetchContent)
if(WIN32)
    set(CUDNN_ARCHIVE cudnn/windows-x86_64/cudnn-windows-x86_64-9.27.0.42_cuda13-archive.zip)
    set(CUDNN_HASH 15595c7b52896262a0e590f02b3b3851c6a2c9046a72047526deaf59a9cbf7d5)
else()
    set(CUDNN_ARCHIVE cudnn/linux-x86_64/cudnn-linux-x86_64-9.27.0.42_cuda13-archive.tar.xz)
    set(CUDNN_HASH dd3245ba3aca8593934fc946d08649ad3a5ee8500e8da3870bc23bf7ddccd7f9)
endif()
FetchContent_Declare(cudnn
    URL "https://developer.download.nvidia.com/compute/cudnn/redist/${CUDNN_ARCHIVE}"
    URL_HASH "SHA256=${CUDNN_HASH}"
    SOURCE_SUBDIR unused
)
FetchContent_MakeAvailable(cudnn)
add_library(cudnn SHARED IMPORTED GLOBAL)
set_target_properties(cudnn PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${cudnn_SOURCE_DIR}/include")
if(WIN32)
    set_target_properties(cudnn PROPERTIES IMPORTED_IMPLIB "${cudnn_SOURCE_DIR}/lib/x64/cudnn.lib" IMPORTED_LOCATION "${cudnn_SOURCE_DIR}/bin/x64/cudnn64_9.dll")
else()
    set_target_properties(cudnn PROPERTIES IMPORTED_LOCATION "${cudnn_SOURCE_DIR}/lib/libcudnn.so")
endif()
