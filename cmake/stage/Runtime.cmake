# cmake/stage/Runtime.cmake
#
# The namespaced Ultralight runtime, and the byte patch that makes it ours.
#
# COEXISTENCE (field-found 2026-08-31): another Ultralight-based UI host ships the
# same four Ultralight module names, and the Windows loader binds imports by BASE
# NAME - whichever host loads "Ultralight.dll" first poisons the other with the
# wrong build. So our copies are RENAMED and every occurrence of the old names
# inside each binary - import descriptors, delay-load descriptors, any literal
# LoadLibrary strings, ASCII and UTF-16 - is patched to the new name.
#
# All replacements are SAME-LENGTH, so the PE layout is untouched and no code
# changes. Renaming Ultralight 1.4.0b's DLLs and rewriting their module-name
# strings is done with the written permission of Ultralight, Inc.
#
# This map is the single owner of the names. tools/patch-module-names.ps1 is a
# dumb byte replacer and is handed these pairs, so the names are not written twice.
#
# The map is handed over as a FILE, not as command-line arguments:
#   - PowerShell's script parameter binder will not absorb a bare list into a
#     [string[]] parameter ("-Map a b" binds one element and rejects the second
#     as a stray positional).
#   - '=' inside an argument is read as a parameter assignment, so 'a.dll=b.dll'
#     does not survive as one argument.
#   - '|' and ';' are BOTH list separators to CMake, so neither survives being
#     expanded into file(WRITE). A space is inert, so the pair is space-separated
#     on disk and the script splits on whitespace.
# Writing it out also leaves a readable record of what was patched, in the build
# tree, for when a stage needs explaining.
set(MAGELIGHT_RUNTIME_RENAMES
    "UltralightCore.dll Magelight1Core.dll"
    "Ultralight.dll Magelight1.dll"
    "WebCore.dll MgWCore.dll"
    "AppCore.dll MgACore.dll"
)
string(REPLACE ";" "\n" _mg_renames_text "${MAGELIGHT_RUNTIME_RENAMES}")
file(WRITE "${CMAKE_BINARY_DIR}/runtime-renames.txt" "${_mg_renames_text}\n")

function(mg_stage_runtime)
    set(rt "${MAGELIGHT_STAGE_PLUGIN_DIR}")
    set(sdk_bin "${MAGELIGHT_ULTRALIGHT_DIR}/bin")
    set(to_patch "")

    # The host plugin sits at the SKSE plugins root.
    set(host "${CMAKE_BINARY_DIR}/Magelight.dll")
    if(NOT EXISTS "${host}")
        message(FATAL_ERROR "Magelight.dll missing at ${host} - the stage ran before the link?")
    endif()
    # MAGELIGHT_STAGE_SKSE is ALREADY <stage>/SKSE/Plugins, so the host lands
    # there directly. Appending a second "Plugins" here stages it at
    # SKSE/Plugins/Plugins/Magelight.dll, where SKSE never looks and
    # tools/check_stage.ps1 reports it missing (found 2026-10-03).
    configure_file("${host}" "${MAGELIGHT_STAGE_SKSE}/Magelight.dll" COPYONLY)
    list(APPEND to_patch "${MAGELIGHT_STAGE_SKSE}/Magelight.dll")

    # The GPU backend rides in the runtime dir, NOT the plugins root, so SKSE never
    # tries to load it as a plugin. Its Ultralight imports get the same patch.
    set(gpu "${CMAKE_BINARY_DIR}/MagelightGPU.dll")
    if(EXISTS "${gpu}")
        file(MAKE_DIRECTORY "${rt}")
        configure_file("${gpu}" "${rt}/MagelightGPU.dll" COPYONLY)
        list(APPEND to_patch "${rt}/MagelightGPU.dll")
    else()
        message(STATUS "MagelightGPU.dll not built - CPU fallback only")
    endif()

    # The vendored runtime, copied straight to its new name.
    foreach(pair IN LISTS MAGELIGHT_RUNTIME_RENAMES)
        string(REPLACE " " ";" parts "${pair}")
        list(GET parts 0 from)
        list(GET parts 1 to)
        if(NOT EXISTS "${sdk_bin}/${from}")
            message(FATAL_ERROR "Ultralight runtime missing: ${sdk_bin}/${from}")
        endif()
        file(MAKE_DIRECTORY "${rt}")
        configure_file("${sdk_bin}/${from}" "${rt}/${to}" COPYONLY)
        list(APPEND to_patch "${rt}/${to}")
    endforeach()

    set(MAGELIGHT_PATCH_FILES "${to_patch}" PARENT_SCOPE)
endfunction()

function(mg_patch_runtime_files)
    if(NOT MAGELIGHT_PATCH_FILES)
        return()
    endif()
    # Both the map and the file list go through FILES, for the same binder reason:
    # a bare list is not absorbed by a PowerShell [string[]] script parameter.
    # runtime-patch-files.csv exists to make the patched set reviewable.
    set(_csv "${CMAKE_BINARY_DIR}/runtime-patch-files.csv")
    set(_rows "")
    foreach(f IN LISTS MAGELIGHT_PATCH_FILES)
        string(APPEND _rows "${f}\n")
    endforeach()
    file(WRITE "${_csv}" "${_rows}")

    execute_process(
        COMMAND "${MAGELIGHT_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
                -File "${MAGELIGHT_SOURCE_DIR}/tools/patch-module-names.ps1"
                -MapFile "${CMAKE_BINARY_DIR}/runtime-renames.txt"
                -FilesCsv "${_csv}"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err
    )
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "namespaced-runtime patch failed (${rc}):\n${out}\n${err}")
    endif()
    string(STRIP "${out}" out)
    if(out)
        message(STATUS "${out}")
    endif()
endfunction()