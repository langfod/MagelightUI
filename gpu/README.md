# gpu/ — the Magelight GPU backend (MagelightGPU.dll)

**License: LGPL-2.1** (see `LICENSE`) — this component is adapted from the
Direct3D 11 reference GPUDriver of [ultralight-ux/AppCore](https://github.com/ultralight-ux/AppCore)
at tag `v1.4.0b` (commit `52d4b918`; originally adapted at `v1.3.0` — the only
upstream driver change between the tags is a `_DEBUG`→`UL_DEBUG` macro in
device-creation code this adaptation removed, and the shader bytecode is
byte-identical) (`src/win/d3d11/GPUDriverD3D11.*`, `GPUContextD3D11.*`,
`src/common/GPUDriverImpl.*`, and the precompiled fill/fill_path shader
bytecode under `shaders/`). Copyright the Ultralight authors; modifications
copyright Severause. `extern/appcore-ref/` holds the verbatim upstream
sources at that tag. Do NOT re-baseline from AppCore master — it tracks a
post-1.4 SDK (filter shaders, blend-state cache, d3d12).

It is deliberately built as its OWN dynamically-loaded DLL so the LGPL
obligations stay contained to this component (LGPL §6 dynamic-linking route):
the host (`Magelight.dll`) talks to it only through the C ABI in
`MagelightGpuApi.h`, loads it at runtime, and falls back to the CPU surface
path when it is absent. Anyone may modify/rebuild this DLL and drop it in.

**Distribution note:** whenever a build containing MagelightGPU.dll is
distributed, this folder is the corresponding source and must remain available
(publishing this repo folder, or including it in the download, satisfies that).

Modifications vs upstream (marked `// MG:` in source):
- The driver binds to the GAME's existing device/context (no device creation,
  no swap chains, no window) — Skyrim owns the device; we render offscreen.
- MSAA is the host's choice (`MgGpu_SetSampleCount`, from Magelight.json `msaa`), where upstream fixes 8x at
  compile time; an MSAA target draws into its own multisample surface and resolves into the plain texture
  everything samples, so the host's SRV and `BindTexture` never see a multisample resource.
- MessageBox error reporting replaced with a host-provided log callback.
- Shaders always load from the embedded fxc bytecode (no file-system path).
- Everything else is kept as close to upstream as practical for diffability.
