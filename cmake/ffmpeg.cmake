include(FetchContent)
if(WIN32)
    set(FFMPEG_ARCHIVE ffmpeg-n9.0.2-33-g4cf298e66a-win64-lgpl-shared-9.0.zip)
    set(FFMPEG_HASH 14d2da681ed1999a6232de61f99d869a46df437dd910d69c1fb5c9c83172242e)
else()
    set(FFMPEG_ARCHIVE ffmpeg-n9.0.2-33-g4cf298e66a-linux64-lgpl-shared-9.0.tar.xz)
    set(FFMPEG_HASH e61b204fd8fff979f5091a5c89213de9afbc7c8ee5f6832667793f176adde112)
endif()
FetchContent_Declare(ffmpeg
    URL "https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-10-22-13-06/${FFMPEG_ARCHIVE}"
    URL_HASH "SHA256=${FFMPEG_HASH}"
    SOURCE_SUBDIR unused
)
FetchContent_MakeAvailable(ffmpeg)
foreach(COMPONENT avformat avcodec swresample avutil)
    add_library(FFmpeg::${COMPONENT} SHARED IMPORTED GLOBAL)
    set_target_properties(FFmpeg::${COMPONENT} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${ffmpeg_SOURCE_DIR}/include")
    if(WIN32)
        file(GLOB COMPONENT_DLL "${ffmpeg_SOURCE_DIR}/bin/${COMPONENT}-*.dll")
        set_target_properties(FFmpeg::${COMPONENT} PROPERTIES IMPORTED_IMPLIB "${ffmpeg_SOURCE_DIR}/lib/${COMPONENT}.lib" IMPORTED_LOCATION "${COMPONENT_DLL}")
    else()
        set_target_properties(FFmpeg::${COMPONENT} PROPERTIES IMPORTED_LOCATION "${ffmpeg_SOURCE_DIR}/lib/lib${COMPONENT}.so")
    endif()
endforeach()
