# Download LingTTS 0.2.0-rc2 into firmware/third_party/lingtts when it is not already there.
# Called from the project CMakeLists before project(), and only for esp32s3.

function(ink_ensure_lingtts dest)
    set(component "${dest}/components/lingtts/CMakeLists.txt")
    set(model "${dest}/assets/lingtts.bin")
    set(frontend "${dest}/assets/lingtts_fe.bin")
    if(EXISTS "${component}" AND EXISTS "${model}" AND EXISTS "${frontend}")
        return()
    endif()

    set(version "0.2.0-rc2")
    set(url "https://github.com/LingCoreX-AI/LingTTS/releases/download/v${version}/lingtts-sdk-${version}-community.tar.gz")
    set(archive "${CMAKE_BINARY_DIR}/lingtts-sdk-${version}-community.tar.gz")
    message(STATUS "Downloading LingTTS SDK ${version}")
    file(DOWNLOAD "${url}" "${archive}"
        EXPECTED_HASH "SHA256=54a9add4b8a12ced3b33648262dcd9a09509cc6749683b34525933dca0f69206"
        STATUS dl_status
        TLS_VERIFY ON)
    list(GET dl_status 0 dl_code)
    if(NOT dl_code EQUAL 0)
        list(GET dl_status 1 dl_msg)
        message(FATAL_ERROR "LingTTS SDK download failed: ${dl_msg}\n${url}")
    endif()

    find_program(INK_TAR tar)
    if(NOT INK_TAR)
        message(FATAL_ERROR "tar is required to unpack the LingTTS SDK")
    endif()
    file(REMOVE_RECURSE "${dest}")
    file(MAKE_DIRECTORY "${dest}")
    execute_process(
        COMMAND "${INK_TAR}" -xzf "${archive}" -C "${dest}" --strip-components=1
        RESULT_VARIABLE extract_code
        ERROR_VARIABLE extract_err)
    if(NOT extract_code EQUAL 0)
        message(FATAL_ERROR "LingTTS SDK extract failed: ${extract_err}")
    endif()
    if(NOT EXISTS "${component}" OR NOT EXISTS "${model}" OR NOT EXISTS "${frontend}")
        message(FATAL_ERROR "LingTTS SDK archive is missing components/lingtts or assets")
    endif()
endfunction()
