# cmake/stage/Content.cmake
#
# Everything in the stage that is copied verbatim: the SDK's resources/ and
# license/, the views/, the loose assets, the Scaleform SWF, and the inspector.
#
# The examples are NOT here — they need npm, so tools/stage-examples.ps1 owns
# them and Stage.cmake calls it after this.

# Recreate-then-copy. Copy-Item -Recurse onto an EXISTING destination nests the
# source dir inside it (the historical resources\resources bug); CMake's
# copy_directory_tree does not, but a stale file from a previous run would
# survive, so we remove first to keep the stage a faithful copy of the source.
function(mg_replace_dir src dst)
    if(NOT IS_DIRECTORY "${src}")
        return()
    endif()
    file(REMOVE_RECURSE "${dst}")
    file(MAKE_DIRECTORY "${dst}")
    file(COPY "${src}/" DESTINATION "${dst}")
endfunction()

function(mg_stage_content)
    set(rt "${MAGELIGHT_STAGE_PLUGIN_DIR}")
    set(ul "${MAGELIGHT_ULTRALIGHT_DIR}")

    file(MAKE_DIRECTORY "${rt}")

    # ICU data and cacert: the host resolves resource_path_prefix against the
    # platform file system's base dir, which is the runtime dir. Without
    # icudt67l.dat Ultralight fails to start and the overlay silently dies.
    mg_replace_dir("${ul}/resources" "${rt}/resources")
    if(NOT EXISTS "${rt}/resources/icudt67l.dat")
        message(FATAL_ERROR "resources staging failed: ${rt}/resources/icudt67l.dat missing")
    endif()

    # Views: the on-disk HTML frontend, loaded via file:/// against the runtime dir.
    # Players get the host's own pages only. The dev pages — probe (imageProbe:true),
    # app + badge (demoViews:true), playground (unreferenced) — stage with -Examples.
    file(REMOVE_RECURSE "${rt}/views")
    file(MAKE_DIRECTORY "${rt}/views")
    foreach(v IN ITEMS gate keyboard)
        if(IS_DIRECTORY "${MAGELIGHT_SOURCE_DIR}/views/${v}")
            file(COPY "${MAGELIGHT_SOURCE_DIR}/views/${v}" DESTINATION "${rt}/views")
        endif()
    endforeach()

    if(MAGELIGHT_EXAMPLES)
        foreach(v IN ITEMS probe app)
            if(IS_DIRECTORY "${MAGELIGHT_SOURCE_DIR}/views/${v}")
                file(COPY "${MAGELIGHT_SOURCE_DIR}/views/${v}" DESTINATION "${rt}/views")
            endif()
        endforeach()
        file(GLOB loose_html "${MAGELIGHT_SOURCE_DIR}/views/*.html")
        if(loose_html)
            file(COPY ${loose_html} DESTINATION "${rt}/views")
        endif()
    endif()

    # Loose assets (cursor.png etc.) land in the runtime dir root.
    if(IS_DIRECTORY "${MAGELIGHT_SOURCE_DIR}/assets")
        file(GLOB assets "${MAGELIGHT_SOURCE_DIR}/assets/*")
        if(assets)
            file(COPY ${assets} DESTINATION "${rt}")
        endif()
    endif()

    # The engine menu's blank Scaleform movie -> Data\Interface.
    if(IS_DIRECTORY "${MAGELIGHT_SOURCE_DIR}/interface")
        file(MAKE_DIRECTORY "${MAGELIGHT_STAGE_INTERFACE}")
        file(GLOB iface "${MAGELIGHT_SOURCE_DIR}/interface/*")
        if(iface)
            file(COPY ${iface} DESTINATION "${MAGELIGHT_STAGE_INTERFACE}")
        endif()
    endif()

    # Licence texts -> runtime dir\license. NOTICES.txt names each file, and the
    # Ultralight EULA has to travel with the runtime.
    #
    # Renames use configure_file(... COPYONLY), not file(COPY): file(COPY) treats
    # its destination as a DIRECTORY and appends the source's own name, so
    # "RENAME" does not exist and naming a file there nests it.
    set(lic "${rt}/license")
    file(MAKE_DIRECTORY "${lic}")
    configure_file("${MAGELIGHT_SOURCE_DIR}/LICENSE" "${lic}/LICENSE.txt" COPYONLY)
    configure_file("${MAGELIGHT_SOURCE_DIR}/gpu/LICENSE" "${lic}/LGPL-2.1.txt" COPYONLY)
    file(COPY "${MAGELIGHT_SOURCE_DIR}/NOTICES.txt" DESTINATION "${lic}")
    file(COPY "${MAGELIGHT_SOURCE_DIR}/THIRD_PARTY_LICENSES.txt" DESTINATION "${lic}")
    if(IS_DIRECTORY "${ul}/license")
        file(COPY "${ul}/license/EULA.txt" DESTINATION "${lic}")
        configure_file("${ul}/license/LICENSE.txt" "${lic}/Ultralight-LICENSE.txt" COPYONLY)
        configure_file("${ul}/license/NOTICES.md" "${lic}/Ultralight-NOTICES.md" COPYONLY)
    else()
        message(WARNING "Ultralight licence folder missing - EULA NOT staged")
    endif()

    # Web Inspector assets (SDK inspector\) -> runtime dir; used only with devMode.
    if(IS_DIRECTORY "${ul}/inspector")
        mg_replace_dir("${ul}/inspector" "${rt}/inspector")
    endif()

    mg_stage_papyrus()
    mg_stage_examples()
    mg_prune_stage_readmes()
endfunction()

# Papyrus tier: papyrus\Magelight.psc -> stage\Scripts\Magelight.pex (+ the source,
# so the pex can be traced). Needs the GAME folder for PapyrusCompiler.exe and the
# flags file, which is why it is a script and not plain CMake.
function(mg_stage_papyrus)
    execute_process(
        COMMAND "${MAGELIGHT_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
                -File "${MAGELIGHT_SOURCE_DIR}/tools/compile-papyrus.ps1"
                -RepoRoot "${MAGELIGHT_SOURCE_DIR}"
                -StageDir "${STAGE_DIR}"
                -SkyrimDir "${MAGELIGHT_SKYRIM_DIR}"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err
    )
    string(STRIP "${out}" out)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "Papyrus compile failed (${rc}):\n${out}\n${err}")
    endif()
    if(NOT EXISTS "${MAGELIGHT_STAGE_SCRIPTS}/Magelight.pex")
        # Not fatal: a machine without Skyrim installed cannot run PapyrusCompiler.
        # The pex is only needed by mods calling our natives.
        message(WARNING "Magelight.pex not staged (no Papyrus compiler found).")
    elseif(out)
        message(STATUS "${out}")
    endif()
endfunction()

# Example manifest mods (examples\<ModId>\) -> stage\Magelight\<ModId>\. Opt-in:
# a build without -Examples must not carry what an earlier -Examples run staged.
# Note the destination is stage\Magelight\, a SIBLING of SKSE\ — mods load their
# Data from Data\Magelight\<ModId>, not from the plugins tree.
function(mg_stage_examples)
    if(NOT MAGELIGHT_EXAMPLES)
        file(REMOVE_RECURSE "${STAGE_DIR}/Magelight")
        return()
    endif()

    execute_process(
        COMMAND "${MAGELIGHT_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
                -File "${MAGELIGHT_SOURCE_DIR}/tools/stage-examples.ps1"
                -RepoRoot "${MAGELIGHT_SOURCE_DIR}"
                -StageDir "${STAGE_DIR}"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err
    )
    string(STRIP "${out}" out)
    if(NOT rc EQUAL 0)
        message(WARNING "example staging failed (${rc}): ${out} ${err}")
    elseif(out)
        message(STATUS "${out}")
    endif()
endfunction()

# Folder READMEs are for the repo, not the player's Data folder. The ones under
# license\ are load-bearing (NOTICES.txt names them) and stay.
function(mg_prune_stage_readmes)
    if(NOT IS_DIRECTORY "${STAGE_DIR}")
        return()
    endif()
    file(GLOB_RECURSE readmes "${STAGE_DIR}/*/README.md" "${STAGE_DIR}/*/*/README.md"
                              "${STAGE_DIR}/*/*/*/README.md")
    foreach(f IN LISTS readmes)
        get_filename_component(dir "${f}" DIRECTORY)
        get_filename_component(parent "${dir}" NAME)
        if(NOT parent STREQUAL "license")
            file(REMOVE "${f}")
        endif()
    endforeach()
endfunction()