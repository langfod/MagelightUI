# cmake/CompilerFlags.cmake
#
# Common compiler/linker settings for both DLLs, kept out of CMakeLists.txt so
# the main file stays short and the per-target differences are visible.
#
# Every function here applies settings to ONE target. Nothing appends to the
# global CMAKE_C_FLAGS / CMAKE_CXX_FLAGS / CMAKE_<CONFIG>_FLAGS variables: the
# host and the LGPL driver have different rules (CommonLibSSE + static CRT vs
# neither), so they cannot share one global flag string, and a global string is
# invisible in the CMake Tools target view.
#
# ── Deliberately NOT set, and why ────────────────────────────────────────────
# /arch:AVX2    The shipped DLL must load on whatever CPU the player's machine
#               has, including pre-AVX2 hardware. MSVC's x64 default (/arch:AVX)
#               is what we build against.
# /EHa          The host relies on SEH unwinding on purpose: SetUIMode is an
#               SEH-wrapped entry point and the crash vectored handler walks the
#               stack through it. /EHa makes SEH exceptions C++-visible and
#               changes that contract.
# /MD -> /MT    Done per target with MSVC_RUNTIME_LIBRARY, not by rewriting the
#               CMAKE_<CONFIG>_FLAGS string. CommonLibSSE's static build needs
#               /MT, and the Ultralight .lib import libraries are CRT-agnostic.
# /GL /LTCG     Already applied once, globally, as
#               CMAKE_INTERPROCEDURAL_OPTIMIZATION for Release in the root
#               CMakeLists.txt. Setting it per target would only restate it.
# /d2ReducedOptimizeHugeFunctions, /Gy, /Gw, /Zc:inline, /Z7
#               Inherited from another project's template, never validated here.
#               /Z7 additionally conflicts with the /Zi + /DEBUG PDB block below.
#
# The commented-out /Zc: block below is deliberately left as comments rather than
# option() declarations. They are conformance checks to hand-enable when chasing
# a compiler warning, not policy this build asserts.
#   /Zc:inline, /Zc:ternary, /Zc:throwingNew — if you try these, they change
#   overload/inline/allocation-failure semantics, and this code leans on
#   function-like macros heavily.

# Settings shared by both DLLs.
#
#   /W4           warnings at level 4
#   /permissive-  no Microsoft extensions
#   /utf-8        source and execution charset (SDK headers and the JS bridge
#                 literals contain non-ASCII text)
function(magelight_common_flags target)
    target_compile_definitions(${target} PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        # CommonLibSSE still reaches for the pre-C++17 codecvt headers.
        # Silence that one deprecation, nothing else.
        _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
    )
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    endif()
endfunction()

# Host-only additions on top of magelight_common_flags.
#
#   /Zc:preprocessor  conforming preprocessor (__VA_ARGS__ stringisation,
#                     __VA_OPT__). The JS bridge shims are built through macros.
function(magelight_host_flags target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /Zc:preprocessor)
    endif()
endfunction()

# Release builds keep full PDBs for both DLLs. The crash telemetry's offsets are
# only decodable against the shipped build's symbols, and package.ps1 emits the
# PDBs as a separate symbols zip that players never see. /OPT:REF and /OPT:ICF
# are restated because /DEBUG alone turns them off.
function(magelight_release_pdb target)
    target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(${target} PRIVATE
        $<$<CONFIG:Release>:/DEBUG>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
endfunction()

# Properties both DLLs need so their output files are the names the stage step,
# the packaging lint and the desktop harness expect.
#
#   PREFIX ""             Magelight.dll, not libMagelight.dll
#   MSVC_RUNTIME_LIBRARY  static CRT, matching CommonLibSSE's static build
#                         (otherwise LNK2038)
function(magelight_shared_defaults target output_name)
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "${output_name}"
        PREFIX ""
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endfunction()

# Both DLLs include and link the same vendored Ultralight SDK. The .lib files are
# IMPORT libraries (pure thunks, CRT-agnostic), so linking them alongside the
# static CRT is safe.
function(magelight_ultralight_deps target)
    target_include_directories(${target} PRIVATE ${ULTRALIGHT_DIR}/include)
    target_link_directories(${target} PRIVATE ${ULTRALIGHT_DIR}/lib)
    target_link_libraries(${target} PRIVATE Ultralight UltralightCore)
endfunction()

# One version resource per DLL, from the single project VERSION. The packaging
# lint compares each staged DLL's FileVersion against CMake's VERSION, so both
# resources have to track project(... VERSION).
#   magelight_version_resource(<out_var> <dll name> <file description>)
function(magelight_version_resource out_var dll_name description)
    # version.rc.in substitutes @PROJECT_NAME@, @PROJECT_VERSION*@,
    # @PLUGIN_AUTHOR@ and @RC_DESCRIPTION@. The two that vary per DLL are set in
    # this function scope, which is where configure_file reads them.
    set(PROJECT_NAME "${dll_name}")
    set(RC_DESCRIPTION "${description}")
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/${dll_name}.rc
        @ONLY
    )
    set(${out_var} ${CMAKE_CURRENT_BINARY_DIR}/${dll_name}.rc PARENT_SCOPE)
endfunction()