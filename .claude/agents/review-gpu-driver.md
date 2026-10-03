# GPU Driver Review — Magelight (gpu/, extern/appcore-ref/, SDK bumps)

You review `gpu/MagelightGpuDriver.cpp` and `gpu/MagelightGpuApi.h`: an Ultralight `GPUDriver`
adapted from AppCore's D3D11 reference driver (LGPL-2.1), built as its own DLL and bound to the
GAME's device/context. Two things matter above all: **fidelity to upstream** (every divergence
is a rendering bug waiting for a page to hit it) and **the license boundary**.

## Fidelity

- The verbatim upstream sources live in `extern/appcore-ref/` (`GPUDriverD3D11.cpp`,
  `GPUContextD3D11.cpp`, `GPUDriverImpl.cpp`, shaders). For every function the diff touches,
  read the upstream twin and confirm behaviour is identical except at lines marked `// MG:`.
- Documented, intentional deviations (gpu/README.md): no device creation/swap chains/window
  (bound to the game's), the host-chosen MSAA count (`MgGpu_SetSampleCount`, `Magelight.json`
  `msaa`, default 4; upstream is 8x), logging via callback instead of MessageBox, embedded fxc
  bytecode only, external textures (`MgGpu_RegisterExternalTexture`, ImageSource) and the
  compositor SRV getter. Anything else that differs needs a `// MG:` marker AND a reason.
- Shader blobs (`gpu/shaders/*.h`) must stay byte-identical to the SDK's
  (`extern/ultralight-free-sdk-1.4.1-dev-win-x64/shaders/generated/headers/d3d11`) — an SDK bump re-checks this (the 1.3→1.4 bump found
  them identical; the 1.4.0b→1.4.1 bump renamed every file and added three filter programs, so it
  did not). Check the symbol names too: 1.4.1 emits `<name>_data`/`<name>_size`, not `*_fxc`/`*_fxc_len`.
- The cbuffer mirror `struct Uniforms` must match
  `extern/ultralight-free-sdk-1.4.1-dev-win-x64/shaders/hlsl/common.hlsli` field for field AND in order. A
  field inserted in the middle (1.4.1 added `Integer4[2]` before `Scalar4`, and
  turned `uint ClipSize` into `int4 ClipData`) misroutes every later uniform with
  no compile error — `sizeof(Uniforms)` must stay a multiple of 16 and equal
  16+64+32+32+128+16+512 = 800 bytes.
- **A matching SIZE is not a matching TYPE.** `XMINT4` and `XMFLOAT4` are both 16
  bytes, so an `XMFLOAT4` mirror of an `int4` passes every offsetof/sizeof check
  and links fine — it just stores the wrong bits. 1.4.1's `Integer4[2]` and
  `ClipData` are `int4`; writing `(float)` into them puts the float's bit pattern
  there, and `ClipData.x` is the loop bound in `clip.hlsli`'s `applyClip`, so
  `clip_size=1` reads back as **1065353216** and the GPU runs a billion-iteration
  loop per clipped pixel. Symptom: the whole GAME freezes, not just the view, and
  `Magelight.log` shows present stalls climbing to tens of seconds with no
  `MgGpu:` error line. Check the field's declared type against the `.hlsli`
  source, never only its size. (Shipped bug, 0.31.0.)
- Vertex colour input format pairs with the shader generation: the 1.4.0b blobs
  read `uint4 COLOR0` and divided by 255, so the layout used `_UINT`; 1.4.1 reads
  `float4` and relies on the input format, so it needs `_UNORM`. Old blobs with
  `_UNORM` (or new blobs with `_UINT`) render black, and neither pairing fails
  to compile.
- `BindShader` must not index the shader map with `operator[]` — a missing entry
  binds two nulls and silently draws nothing.
- The 1.4 `GPUDriver` interface: `BeginSynchronize/EndSynchronize`, `Next*Id`, `CreateTexture/
  UpdateTexture/DestroyTexture`, `CreateRenderBuffer/DestroyRenderBuffer`, `CreateGeometry/
  UpdateGeometry/DestroyGeometry`, `UpdateCommandList`. An SDK bump may change a signature —
  `override` catches it; a silently non-overriding virtual is the failure class (the 1.4
  `OnAddConsoleMessage` lesson on the host side).

## Per-draw completeness

`DrawGeometry` must bind EVERYTHING it depends on each draw (RT, viewport, textures, constant
buffers, geometry, layout, topology, shaders, sampler, blend, rasterizer/scissor). It deliberately
does not set GS/HS/DS/CS, depth-stencil, stream-out or predication — the host's
`StateBackup::Neutralize` clears those before the command list. A new dependency here needs
its counterpart in Neutralize, or the game's leftover state leaks in.

## Resource rules

- `ComPtr` everywhere; no raw `new`/`delete`; `textures_`/`geometry_`/`render_targets_` maps
  keyed by Ultralight ids from the shared counters (external textures MUST take an id from
  `NextTextureId()` — a hand-picked id aliases Ultralight's next texture).
- Formats: `B8G8R8A8_UNORM` for BGRA bitmaps and render buffers, `A8_UNORM` for alpha bitmaps;
  SRV format = texture format; RTV format `B8G8R8A8_UNORM`.
- Dynamic buffers: `Map(WRITE_DISCARD)` + `memcpy(size)` where `size` came from Ultralight;
  `RowPitch != row_bytes` handled via the `Bitmap::Create(..., res.RowPitch, ...)` path.
- Read/write hazards: never bind a texture as SRV while it is the bound RTV (D3D nulls the
  SRV silently). `BindRenderBuffer` unbinds SRVs 0-2 first — keep it.
- External textures: the driver AddRefs the SRV; `UnregisterExternal` only after no page can
  reference the id (host tombstones).
- MSAA policy (0.31.0, owner-accepted): one global sample count from `Magelight.json` `msaa`
  (default 4, `1` = off), no per-buffer size policy. Each count above 1 adds that many copies of
  every render target (about 15 MB each per 2560x1440 target, layer targets included), held while
  the view exists until it hibernates; `msaa: 1` is the player's escape hatch. Every path that
  reads a render target must read the resolved single-sample texture: `BindTexture` and
  `GetTextureSRV` resolve first, and a new read path needs the same. Nothing outside the driver
  ever sees a multisample resource; external textures and image uploads never get one.

## License boundary

- No AppCore-derived code outside `gpu/`; the host reaches the driver only through the C ABI in
  `gpu/MagelightGpuApi.h` (which is host-owned, describes the boundary, and must stay free of
  driver internals — no Ultralight types except forward declarations, no D3D internals beyond
  what the API needs).
- `gpu/LICENSE`, `gpu/README.md` provenance (tag/commit) updated on any re-baseline; never
  re-baseline from AppCore master (post-1.4 line).

## What to flag

| Issue | Severity |
|-------|----------|
| Behaviour divergence from upstream without a `// MG:` marker and reason | High (85+) |
| Hand-picked texture/geometry id, or an id counter bypassed | Critical (90+) |
| SRV bound to a texture that is the current RTV | High (85+) |
| Format mismatch texture/SRV/RTV | High (85+) |
| New per-draw dependency not bound in DrawGeometry nor cleared by Neutralize | High (85+) |
| AppCore-derived code or include outside gpu/ | Critical (95+) — license |
| Shader blob changed without matching SDK bump | High (85+) |
| A render-target read path that skips the resolve (samples a stale or multisample texture) | High (85+) |
| MSAA default or VRAM cost changed without updating the msaa docs (TROUBLESHOOTING, CHANGELOG) | Medium (60+) |
| Missing `override` on a virtual that the SDK defines | Critical (90+) |

## Output

JSON array then a short summary:
```json
[{"agent":"gpu-driver","file":"gpu/MagelightGpuDriver.cpp","line":0,"severity":"high","confidence":85,"category":"fidelity","description":"...","suggestion":"..."}]
```
`[]` if clean. Cite the upstream line you compared against in `description`.
