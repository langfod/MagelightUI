# cmake/Stage.cmake — build a ready-to-install mod layout under <binary>/stage.
#
# Run in script mode, after the DLLs are linked:
#   cmake -DSTAGE_DIR=<dir> [-DMAGELIGHT_EXAMPLES=ON] -P cmake/Stage.cmake
#
# The build wires this as a custom target that depends on both DLLs (see the
# bottom of CMakeLists.txt), so `cmake --build` stages automatically. Running it
# by hand re-stages without a rebuild, which is what you want after editing a
# page under views/ or an example manifest.
#
# Layout produced:
#   stage\SKSE\Plugins\Magelight.dll          the host plugin
#   stage\SKSE\Plugins\Magelight\             runtime dir: namespaced Ultralight
#                                             DLLs, MagelightGPU.dll, resources\,
#                                             views\, license\, inspector\
#   stage\SKSE\Plugins\Magelight\<ModId>\     examples (opt-in)
#   stage\Interface\                          the Scaleform focus-menu SWF
#   stage\Scripts\                            Magelight.pex
#
# The per-step files under cmake/stage/ each own one concern. Steps that need real
# logic (byte patching, Steam discovery, npm) live in tools/*.ps1 and are invoked
# from here — CMake cannot express them, but it can sequence them.

cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED STAGE_DIR)
    get_filename_component(STAGE_DIR "${CMAKE_BINARY_DIR}/stage" ABSOLUTE)
endif()
get_filename_component(STAGE_DIR "${STAGE_DIR}" ABSOLUTE)

# Where the vendored SDK lives, relative to the source root.
if(NOT DEFINED MAGELIGHT_SOURCE_DIR)
    get_filename_component(MAGELIGHT_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

set(MAGELIGHT_STAGE_SKSE    "${STAGE_DIR}/SKSE/Plugins")
set(MAGELIGHT_STAGE_PLUGIN_DIR "${MAGELIGHT_STAGE_SKSE}/Magelight")
set(MAGELIGHT_STAGE_INTERFACE "${STAGE_DIR}/Interface")
set(MAGELIGHT_STAGE_SCRIPTS  "${STAGE_DIR}/Scripts")
set(MAGELIGHT_STAGE_SOURCE   "${STAGE_DIR}/Source/Scripts")

set(MAGELIGHT_ULTRALIGHT_DIR "${MAGELIGHT_SOURCE_DIR}/extern/ultralight-free-sdk-1.4.1-dev-win-x64")

# Papyrus needs the GAME folder, not the repo. tools/compile-papyrus.ps1 resolves
# it (env var, then Steam), so a caller who already knows it can pass it through.
if(NOT DEFINED MAGELIGHT_SKYRIM_DIR)
    set(MAGELIGHT_SKYRIM_DIR "$ENV{MAGELIGHT_SKYRIM_DIR}")
endif()

# Examples are opt-in: a build without -Examples must not carry what an earlier
# -Examples run staged. build.ps1 / build2.ps1 pass this through.
if(NOT DEFINED MAGELIGHT_EXAMPLES)
    set(MAGELIGHT_EXAMPLES OFF)
endif()

# The link output dir. Under a single-config generator (Ninja) both DLLs land in
# CMAKE_BINARY_DIR; under a multi-config one they land in a per-config subdir.
if(CMAKE_CONFIGURATION_TYPES)
    set(MAGELIGHT_DLL_DIR "${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}")
else()
    set(MAGELIGHT_DLL_DIR "${CMAKE_BINARY_DIR}")
endif()

set(MAGELIGHT_POWERSHELL "powershell")

include("${CMAKE_CURRENT_LIST_DIR}/stage/Runtime.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/stage/Content.cmake")

message(STATUS "Staging to ${STAGE_DIR}")

# Order matters: the runtime and its byte patch come first so a failure stops
# before anything else lands half-staged.
mg_stage_runtime()
mg_patch_runtime_files()
mg_stage_content()

# The namespaced copy in the stage is the shipped artifact. The desktop harness
# deliberately wants an UNPATCHED MagelightGPU.dll to compare against, so the
# build tree keeps the original next to the DLLs.
