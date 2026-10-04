#pragma once
// Magelight UI — public C ABI of the GPU backend DLL (MagelightGPU.dll).
//
// This header is part of the HOST project (not LGPL): it only DESCRIBES the
// boundary. The backend implementation behind it is adapted from Ultralight's
// AppCore reference driver (LGPL-2.1) and is deliberately isolated in its own
// dynamically-loaded DLL so the host stays license-clean — see gpu/README.md.
//
// The host resolves these via GetProcAddress after preloading the (namespaced)
// Ultralight runtime; MagelightGPU.dll's own imports bind against the modules
// already in the process.

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

#ifdef MGGPU_BUILD
#  define MGGPU_API __declspec(dllexport)
#else
#  define MGGPU_API
#endif

extern "C" {

    // Create a driver bound to the GAME's device/context (not owned). `log`
    // receives human-readable errors (may be null). Returns an opaque handle,
    // or null on failure.
    typedef void (*MgGpuLogFn)(const char* message);
    MGGPU_API void* MgGpu_Create(ID3D11Device* device, ID3D11DeviceContext* context, MgGpuLogFn log);

    MGGPU_API void MgGpu_Destroy(void* handle);

    // The ultralight::GPUDriver* to hand to Platform::set_gpu_driver. Valid
    // for the handle's lifetime. (A C++ interface pointer is safe across this
    // boundary: both DLLs are built together with the same toolchain and link
    // the same Ultralight module.)
    MGGPU_API void* MgGpu_GetGPUDriver(void* handle);

    // Frame execution: after Renderer::Render() has buffered commands.
    MGGPU_API int  MgGpu_HasCommandsPending(void* handle);
    MGGPU_API void MgGpu_DrawCommandList(void* handle);

    // MSAA sample count (1, 2, 4 or 8) for render targets created from now on;
    // a count the device cannot do steps down. Returns the count in effect,
    // 0 for a null handle. Optional export: a backend without it draws
    // without MSAA. Render thread, before the first view is best.
    MGGPU_API int MgGpu_SetSampleCount(void* handle, int samples);

    // ID3D11ShaderResourceView* for a driver texture id (a View's
    // render_target().texture_id) — what the host's compositor samples.
    // Null if unknown. Not a plain getter: with MSAA on, this call records
    // the resolve on the immediate context, so its contents are current only
    // as of this call. Render thread only, after DrawCommandList, once per
    // frame whose pixels you read; the pointer is stable for the texture's
    // life, but never reuse it across frames without calling again (a cached
    // or before-draw read shows a stale frame).
    MGGPU_API void* MgGpu_GetTextureSRV(void* handle, std::uint32_t texture_id);

    // ── External textures (Ultralight ImageSource) ──────────────────────
    // Register an SRV the HOST (or an embedding mod) owns under a fresh
    // driver texture id — the id an ImageSource::CreateFromTexture refers
    // to. Ultralight never uploads, resizes or destroys it; its FillType_
    // Image draws simply bind this SRV. The driver AddRef's the SRV. All
    // three run on the render thread only (the driver's texture map is not
    // locked). Returns 0 on failure.
    MGGPU_API std::uint32_t MgGpu_RegisterExternalTexture(void* handle, ID3D11ShaderResourceView* srv);
    // Re-point an external id at a new SRV (resize/recreate). 1 = ok.
    MGGPU_API int MgGpu_SetExternalTextureSRV(void* handle, std::uint32_t texture_id, ID3D11ShaderResourceView* srv);
    // Drop the driver entry. Only safe once no page can still reference the id.
    MGGPU_API void MgGpu_UnregisterExternalTexture(void* handle, std::uint32_t texture_id);

}  // extern "C"
