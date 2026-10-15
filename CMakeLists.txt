cmake_minimum_required(VERSION 3.21)

# Set vcpkg triplet before project() if not already set (mirrors SeverActionsNative)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()
if(NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "")
endif()

project(
    Magelight
    VERSION 0.32.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif()


find_package(directxmath CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(minhook CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(cpuinfo CONFIG REQUIRED)
# CommonLibSSE requires DirectXTK to be found here.
find_package(directxtk CONFIG REQUIRED)
find_package(CommonLibSSE CONFIG REQUIRED)

set(PLUGIN_NAME "${PROJECT_NAME}")
set(PLUGIN_AUTHOR "Severause")
set(PLUGIN_VERSION "${PROJECT_VERSION}")

include(cmake/CompilerFlags.cmake)

# Both DLLs get a version resource from the single project VERSION. The GPU
# driver needs its own because the packaging lint compares MagelightGPU.dll's
# FileVersion against CMake's VERSION (tools/check_stage.ps1).
magelight_version_resource(RC_HOST_MAGELIGHT
    Magelight "${PLUGIN_NAME} SKSE Plugin")
magelight_version_resource(RC_HOST_MAGELIGHTGPU
    MagelightGPU "Magelight UI GPU driver (LGPL-2.1, AppCore-derived)")

# Vendored Ultralight SDK. The folder name is the SDK's own archive name, so
# what is on disk is what the download page gave us (2.0 is fetched from
# https://ultralig.ht/download — extern/README.md has the procedure). Do not
# shorten this back to extern/ultralight: the explicit name is what proves
# which build is vendored.
set(ULTRALIGHT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/ul-sdk-2.0.0-beta.2-win-x64-free")
if(NOT EXISTS "${ULTRALIGHT_DIR}/include/Ultralight/Ultralight.h")
    message(FATAL_ERROR "Ultralight SDK not found at ${ULTRALIGHT_DIR} — fetch it per extern/README.md, then run build.ps1.")
endif()


add_library(${PROJECT_NAME} SHARED
    src/main.cpp
    src/Magelight.cpp
    src/host/Telemetry.cpp   # crash VEH, stall watchdog, runtime preload, GPU backend binding
    src/host/Views.cpp       # the view + image registries (s_views, s_images, MlView, MlImage)
    src/host/Platform.cpp    # Ultralight platform bindings: file system (invariant 5), network sandbox, clipboard
    src/host/KeyboardMute.cpp # device-level keyboard mute while UI mode is on (invariant 17)
    src/host/Input.cpp       # the window proc, UI mode, the IME, the engine-side sinks (invariants 2, 17)
    src/host/Render.cpp      # the D3D resources, the UI-pass composite, the render-thread view pass, FrameWork
    src/host/Draw.cpp        # device + runtime bring-up: CreateDeviceResources, CreateUltralight
    src/host/Frame.cpp       # the per-frame pass: DrainInputQueue, DrawQuadTo, DrawOverlay, UploadSurfaceIfDirty
    src/host/Hooks.cpp       # the DXGI hooks: ResizeBuffers, Present/Present1, the late detours, InstallHook
    src/host/HardwareInfo.cpp # CPU / RAM / OS / GPU identification, logged once at startup
    src/app/Api.cpp          # the public host surface (CreateView, ShowView, RegisterTextureImage, ...)
    src/app/Bridge.cpp       # the JS <-> C++ page bridge (MlNativeDispatch, InstallBridgeShims, own keyboard)
    src/MagelightApiExport.cpp
    src/MagelightApi4/Registry.cpp   # v4 registry state + lock-held helpers (owns s_mutex)
    src/MagelightApi4/Api.cpp        # the v4 public surface
    src/MagelightApi4/Api018.cpp     # 0.18.0 (VR-4)
    src/MagelightApi4/Api028.cpp     # 0.28.0 onward
    src/MagelightManifest.cpp
    src/MagelightSound.cpp       # host-played UI sounds
    src/MagelightDevWatch.cpp
    src/MagelightPapyrus.cpp
    src/MagelightVR/Runtime.cpp    # Skyrim VR presenter (docs/VR_PRESENTER.md): binding, state, pose snapshot
    src/MagelightVR/Input.cpp      # engine controller edges in (game thread)
    src/MagelightVR/Bindings.cpp   # hotkeys, chords, press-to-bind capture
    src/MagelightVR/Overlays.cpp   # the overlay record's lifecycle
    src/MagelightVR/Laser.cpp      # ray hit-testing, beams, keyboard, grab-and-move
    src/MagelightVR/Lifecycle.cpp  # Init / SubmitFrame / ReleaseView / Shutdown
    ${RC_HOST_MAGELIGHT}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/api
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include/magelight
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    CommonLibSSE::CommonLibSSE
    minhook::minhook
    nlohmann_json::nlohmann_json
    cpuinfo::cpuinfo
    WebCore
    AppCore
    d3d11
    d3dcompiler
    windowscodecs   # cursor art (WIC PNG decode)
    imm32           # IME composition (0.27.0)
    ole32
    delayimp
)

magelight_ultralight_deps(${PROJECT_NAME})

# CommonLib must precede every Windows header or REX aborts the TU. As a PCH
# that ordering is structural rather than a convention each source repeats.
target_precompile_headers(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/PCH.h)

# The Ultralight runtime DLLs live in Data/SKSE/Plugins/Magelight/, NOT next to
# the game exe - the loader would fail our import table at plugin-load time.
# Delay-load all four; Magelight::PreloadRuntime() LoadLibrary's them from the
# right directory before any Ultralight symbol is touched, and the delay-load
# thunks then resolve against the already-loaded modules.
target_link_options(${PROJECT_NAME} PRIVATE
    "/DELAYLOAD:Ultralight.dll"
    "/DELAYLOAD:UltralightCore.dll"
    "/DELAYLOAD:WebCore.dll"
    "/DELAYLOAD:AppCore.dll"
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    PLUGIN_NAME="${PLUGIN_NAME}"
    PLUGIN_AUTHOR="${PLUGIN_AUTHOR}"
    PLUGIN_VERSION="${PLUGIN_VERSION}"
    # SKSE declaration version derives from CMake - the single-source rule
    # SeverActionsNative adopted after its declaration sat at 3.0.7.0 for ~30
    # releases. Bake the lesson in from day one.
    PLUGIN_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    PLUGIN_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    PLUGIN_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

magelight_common_flags(${PROJECT_NAME})
magelight_host_flags(${PROJECT_NAME})
magelight_shared_defaults(${PROJECT_NAME} "${PLUGIN_NAME}")

# MagelightGPU.dll - the LGPL-isolated GPU backend (see gpu/README.md)
# Adapted from AppCore v1.3.0's D3D11 reference GPUDriver. Built as its OWN
# dynamically-loaded DLL so the LGPL obligations stay contained; the host talks
# to it only through the C ABI in gpu/MagelightGpuApi.h and falls back to the
# CPU surface path when it is absent. No CommonLib dependency - which is why it
# gets no PCH and no magelight_host_flags. No /DELAYLOAD: the host loads it AFTER
# preloading the (namespaced) runtime, and the stage step patches this DLL's
# Ultralight import names like every other binary.
add_library(MagelightGPU SHARED
    gpu/MagelightGpuDriver.cpp
    gpu/MagelightGpuApi.h
    ${RC_HOST_MAGELIGHTGPU}
)

target_include_directories(MagelightGPU PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/gpu
)
target_link_libraries(MagelightGPU PRIVATE d3d11)
magelight_ultralight_deps(MagelightGPU)

# MGGPU_BUILD switches MagelightGpuApi.h between dllexport and dllimport.
target_compile_definitions(MagelightGPU PRIVATE MGGPU_BUILD)

magelight_common_flags(MagelightGPU)
magelight_shared_defaults(MagelightGPU "MagelightGPU")

# Release keeps full PDBs for both DLLs; the crash telemetry's offsets are only
# decodable against the shipped build's symbols.
foreach(_mgl_target ${PROJECT_NAME} MagelightGPU)
    magelight_release_pdb(${_mgl_target})
endforeach()

# ── Stage a ready-to-install layout under <binary>/stage ──────────────────────
# cmake/Stage.cmake owns the layout; this target just sequences it after the link.
# A separate target rather than POST_BUILD on the DLLs because it depends on BOTH,
# and POST_BUILD cannot see the other target.
#
# The staged MagelightGPU.dll is byte-patched (cmake/stage/Runtime.cmake); the
# desktop harness wants an UNPATCHED one and reads it from the build dir, which
# the stage never touches.
add_custom_target(stage
    COMMAND ${CMAKE_COMMAND}
            -DSTAGE_DIR=${CMAKE_BINARY_DIR}/stage
            -DMAGELIGHT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DMAGELIGHT_EXAMPLES=${MAGELIGHT_EXAMPLES}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Stage.cmake
    DEPENDS ${PROJECT_NAME} MagelightGPU
    COMMENT "Staging installable layout to ${CMAKE_BINARY_DIR}/stage"
    VERBATIM
)
