# Generate device HTML with luci-lite.css and theme.js inlined.
get_filename_component(INK_WEBUI_DIR "${CMAKE_CURRENT_LIST_FILE}" DIRECTORY)

function(ink_webui_inject template css js output)
    ink_webui_inject_impl("${template}" "${css}" "${js}" "${output}" "")
endfunction()

function(ink_webui_inject_tool template css js output)
    ink_webui_inject_impl("${template}" "${css}" "${js}" "${output}" "--tool")
endfunction()

function(ink_webui_inject_impl template css js output extra_arg)
    find_program(_ink_webui_python NAMES python3 python)
    if(NOT _ink_webui_python)
        message(FATAL_ERROR "python3 is required to generate LuCI web UI HTML")
    endif()

    set(_script "${INK_WEBUI_DIR}/inject_webui.py")
    set(_cmd "${_ink_webui_python}" "${_script}" "${template}" "${css}" "${js}" "${output}")
    if(extra_arg)
        list(APPEND _cmd "${extra_arg}")
    endif()

    # File must exist at configure time for EMBED_TXTFILES.
    execute_process(
        COMMAND ${_cmd}
        RESULT_VARIABLE _ink_webui_rc
    )
    if(NOT _ink_webui_rc EQUAL 0)
        message(FATAL_ERROR "inject_webui.py failed for ${template}")
    endif()

    # Rebuild when the template or shared chrome files change. Do not use
    # CMAKE_CONFIGURE_DEPENDS on luci-lite.css / theme.js: Ninja treats those
    # as RERUN_CMAKE outputs and errors if two components list the same file.
    # add_custom_command is not available while ESP-IDF scans requirements
    # in script mode (-P).
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        set(_deps "${template}" "${css}" "${js}" "${_script}")
        foreach(_extra IN ITEMS chrome-header.html sonic.js)
            if(EXISTS "${INK_WEBUI_DIR}/${_extra}")
                list(APPEND _deps "${INK_WEBUI_DIR}/${_extra}")
            endif()
        endforeach()
        add_custom_command(
            OUTPUT "${output}"
            COMMAND ${_cmd}
            DEPENDS ${_deps}
            COMMENT "Generating web UI ${output}"
            VERBATIM
        )
    endif()
endfunction()
