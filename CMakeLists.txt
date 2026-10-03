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
    VERSION 0.30.5
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif()

# CommonLibSSE-NG (same pinned overlay port + colorglass registry as
# SeverActionsNative). The fork's exported targets re-export DirectXTK /
# DirectXMath / fmt / spdlog / xbyak as interface dependencies, so each must be
# findable here; rapidcsv rides in vcpkg.json for the port's internal sources.
find_package(directxtk CONFIG REQUIRED)
find_package(directxtex CONFIG REQUIRED)
find_package(directxmath CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(minhook CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(xbyak CONFIG REQUIRED)
find_package(CommonLibSSE CONFIG REQUIRED)

set(PLUGIN_NAME "${PROJECT_NAME}")
set(PLUGIN_AUTHOR "Severause")
set(PLUGIN_VERSION "${PROJECT_VERSION}")

set(RC_DESCRIPTION "${PLUGIN_NAME} SKSE Plugin")
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in
    ${CMAKE_CURRENT_BINARY_DIR}/version.rc
    @ONLY
)
# The GPU driver carries its own resource (same single version source): the
# packaging lint checks MagelightGPU.dll's FileVersion against CMake's VERSION.
set(RC_DESCRIPTION "Magelight UI GPU driver (LGPL-2.1, AppCore-derived)")
set(_host_project_name "${PROJECT_NAME}")
set(PROJECT_NAME "MagelightGPU")
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in
    ${CMAKE_CURRENT_BINARY_DIR}/version_gpu.rc
    @ONLY
)
set(PROJECT_NAME "${_host_project_name}")

# Vendored Ultralight SDK (extern/ultralight â€” fetched by setup, see build.ps1).
set(ULTRALIGHT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/ultralight")
if(NOT EXISTS "${ULTRALIGHT_DIR}/include/Ultralight/Ultralight.h")
    message(FATAL_ERROR "Ultralight SDK not found at ${ULTRALIGHT_DIR} â€” fetch it per extern/README.md, then run build.ps1.")
endif()

add_library(${PROJECT_NAME} SHARED
    src/main.cpp
    src/Magelight.cpp
    src/MagelightApiExport.cpp
    src/MagelightApi4.cpp
    src/MagelightManifest.cpp
    src/MagelightSound.cpp       # host-played UI sounds
    src/MagelightDevWatch.cpp
    src/MagelightPapyrus.cpp
    src/MagelightVR.cpp          # Skyrim VR presenter (docs/VR_PRESENTER.md); runtime-gated, no openvr link
    ${CMAKE_CURRENT_BINARY_DIR}/version.rc
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${ULTRALIGHT_DIR}/include
)

target_link_directories(${PROJECT_NAME} PRIVATE ${ULTRALIGHT_DIR}/lib)

target_link_libraries(${PROJECT_NAME} PRIVATE
    CommonLibSSE::CommonLibSSE
    minhook::minhook
    nlohmann_json::nlohmann_json
    Ultralight
    UltralightCore
    WebCore
    AppCore
    d3d11
    d3dcompiler
    windowscodecs   # cursor art (WIC PNG decode)
    imm32           # IME composition (0.27.0)
    ole32
    delayimp
)

# The Ultralight runtime DLLs live in Data/SKSE/Plugins/Magelight/, NOT next to
# the game exe â€” the loader would fail our import table at plugin-load time.
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
    # SKSE declaration version derives from CMake â€” the single-source rule
    # SeverActionsNative adopted after its declaration sat at 3.0.7.0 for ~30
    # releases. Bake the lesson in from day one.
    PLUGIN_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    PLUGIN_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    PLUGIN_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /permissive- /utf-8 /Zc:preprocessor)
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "${PLUGIN_NAME}"
    PREFIX ""
    # Static CRT to match CommonLibSSE's x64-windows-static build (the LNK2038
    # RuntimeLibrary mismatch otherwise). The Ultralight .libs are IMPORT libs
    # (pure thunks, CRT-agnostic), so /MT is safe alongside them.
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# â”€â”€ MagelightGPU.dll â€” the LGPL-isolated GPU backend (see gpu/README.md) â”€â”€â”€â”€â”€
# Adapted from AppCore v1.3.0's D3D11 reference GPUDriver. Built as its OWN
# dynamically-loaded DLL so the LGPL obligations stay contained; the host talks
# to it only through the C ABI in gpu/MagelightGpuApi.h and falls back to the
# CPU surface path when it is absent. No CommonLib dependency. No /DELAYLOAD:
# the host loads it AFTER preloading the (namespaced) runtime, and the stage
# step patches this DLL's Ultralight import names like every other binary.
add_library(MagelightGPU SHARED gpu/MagelightGpuDriver.cpp ${CMAKE_CURRENT_BINARY_DIR}/version_gpu.rc)
target_include_directories(MagelightGPU PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/gpu
    ${ULTRALIGHT_DIR}/include
)
target_link_directories(MagelightGPU PRIVATE ${ULTRALIGHT_DIR}/lib)
target_link_libraries(MagelightGPU PRIVATE Ultralight UltralightCore d3d11)
target_compile_definitions(MagelightGPU PRIVATE MGGPU_BUILD NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(MagelightGPU PRIVATE /W4 /permissive- /utf-8)
endif()
set_target_properties(MagelightGPU PROPERTIES
    OUTPUT_NAME "MagelightGPU"
    PREFIX ""
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# Release keeps full PDBs for both DLLs — the crash telemetry's offsets are
# only decodable against the shipped build's symbols. package.ps1 emits
# them as a separate symbols zip; players never see them. Before 0.28.6 Release
# builds had no PDBs at all. (/OPT:REF+ICF are re-stated because /DEBUG alone
# would turn them off.)
foreach(_mgl_target ${PROJECT_NAME} MagelightGPU)
    target_compile_options(${_mgl_target} PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(${_mgl_target} PRIVATE
        $<$<CONFIG:Release>:/DEBUG>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
endforeach()
