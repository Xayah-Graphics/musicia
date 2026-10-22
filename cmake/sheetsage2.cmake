if(CMAKE_SCRIPT_MODE_FILE)
    file(MAKE_DIRECTORY "${OUTPUT_DIRECTORY}")
    file(DOWNLOAD
        https://huggingface.co/m-a-p/SheetSage2/resolve/b83c3d79a6d98bcffaf8f9ed112c90aa887887bc/model.safetensors
        "${OUTPUT_DIRECTORY}/sheetsage2.safetensors"
        EXPECTED_HASH SHA256=b235f68091a5f5b644000f2b5acb57d1e70432aca2b34ab1b9cf27236e1f4274
        TLS_VERIFY ON SHOW_PROGRESS)
    file(DOWNLOAD
        https://huggingface.co/m-a-p/MERT-v2-FullSong/resolve/d8ba1c745e733b3908ce6ad16ebeb17ac7600a42/model.safetensors
        "${OUTPUT_DIRECTORY}/mert.safetensors"
        EXPECTED_HASH SHA256=e6dd2ab187d6dd62b6521cd7d8f932e237acf0c5757745a7232082e28391350d
        TLS_VERIFY ON SHOW_PROGRESS)
else()
    set(MUSIC_MODEL_DIRECTORY "${CMAKE_BINARY_DIR}/models/sheetsage2" CACHE PATH "Directory containing the official model weights")
    add_custom_command(
        OUTPUT "${MUSIC_MODEL_DIRECTORY}/sheetsage2.safetensors" "${MUSIC_MODEL_DIRECTORY}/mert.safetensors"
        COMMAND ${CMAKE_COMMAND} "-DOUTPUT_DIRECTORY=${MUSIC_MODEL_DIRECTORY}" -P "${CMAKE_CURRENT_LIST_FILE}"
        DEPENDS "${CMAKE_CURRENT_LIST_FILE}"
        COMMENT "Download the pinned official SheetSage2 and MERT safetensors"
        USES_TERMINAL VERBATIM)
    add_custom_target(sheetsage2_weights DEPENDS "${MUSIC_MODEL_DIRECTORY}/sheetsage2.safetensors" "${MUSIC_MODEL_DIRECTORY}/mert.safetensors")
endif()
