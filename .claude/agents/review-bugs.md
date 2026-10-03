# Bug Review — Magelight

You hunt bugs in Magelight: an SKSE plugin (CommonLibSSE-NG, C++23) hosting Ultralight 1.4 on
Skyrim's D3D11 device, plus a Vite/React demo frontend and a desktop harness. Read `CLAUDE.md`
first. These classes have each shipped or been caught in the field — treat matches as
high-confidence.

## Host (src/Magelight.cpp)

- **D3D state hygiene**: `StateBackup::Capture/Neutralize/Restore` bracket everything we do to
  the game's immediate context per frame. A new draw or a new state set (shaders, RTs, samplers,
  blend, rasterizer, depth-stencil, GS/HS/DS/CS, stream-out, predication, viewports, scissor)
  must be captured AND restored, and the Neutralize list must cover what the driver assumes clean.
  Anything left dirty shows up as a game-render glitch, never as our bug.
- **COM refcounts**: every `Get*` on a D3D context returns an AddRef'd pointer — release it.
  `ComPtr` in the driver; raw pointers with explicit `Release()` in the host's StateBackup only.
- **Lifetime across the C ABI**: `const char*` returned to consumers must point at storage that
  outlives the call (static / registry-owned, never a temporary `std::string`). Arguments received
  are valid only during the call — copy before queuing.
- **Tombstones**: images are never erased (pages may still reference the URL); views ARE erased
  by `ApplyPendingLifecycle` — every lookup by id must tolerate "gone" (`FindViewLocked` null).
- **Intent flags**: `boundsDirty`, `shimsDirty`, `destroyPending`, `reloadPending`,
  `navigateUrl`, `reloadedFlag`, image `dirty/unregister/pendingSrv` — each is set on one thread
  and consumed on the render thread. A flag consumed and never cleared repaints/reloads forever;
  a flag cleared before it is acted on drops the request.
- **Fullscreen tracking**: a `w==0&&h==0` view never materializes until the backbuffer size is
  known; a code path that materializes before the size pass runs creates a 0x0 view.
- **URL/path handling**: `PathToFileUrl` (percent-encode), `MlFileSystem::Resolve` (query strip,
  percent-decode, absolute drive-letter paths). Any new path source must go through them; a
  space in a mod-folder path is the classic failure.
- **Settings parsing**: `Magelight.json` fields are type-checked (`is_number`, `is_boolean`)
  and clamped; a new key must be, too, and must be logged in the "settings loaded" line.
- **Cursor**: hotspot math uses normalized hotspot × drawn size at the backbuffer scale; custom
  art is premultiplied BGRA (WIC `PBGRA`). A non-premultiplied upload halos.
- **UI mode**: `SetUIModeImpl` is idempotent on `s_focused`; every exit path (Escape, toggle,
  load boundary, render death, menu hide) must land there so the exit callbacks/events fire.
  `ExitUIMode` does NOT hide (SA's Free Look); `ToggleVisible` exit does.
- **Render death**: after `s_renderDead`, no Ultralight work, API calls must no-op or return
  `RenderDead`, `IsUIModeActive` reports false.
- **Logging discipline**: new failure modes get a log line naming the view/mod/file (invariant 9).

## v4 registry (src/MagelightApi4/)

- Validate `size` prefixes (`desc->size < sizeof(...)` → InvalidArgument), null pointers, slugs.
- `s_uiOwner` clears on EVERY UIModeExited (including host-initiated exits); RequestUIMode
  must treat `IsUIModeActive() || s_uiOwner != 0` as busy (the host's own toggle view can hold
  UI mode with no owner).
- Event routing: a view event for an unowned view (v1-v3 consumers) is dropped silently — fine;
  a v4 view must always find its owner until ViewDestroyed erases the map entry.
- `lastError` per mod; `GetLastErrorMessage` returns registry-owned storage.
- `UnregisterMod` releases UI mode only if that mod owns it and destroys only its views.

## GPU driver (gpu/) — see review-gpu-driver.md for fidelity; here: plain bugs

- `Map/Unmap` pairs, `ByteWidth == vertices.size`, `RowPitch != row_bytes` path, format of SRV
  equals texture format, `ResolveSubresource`/`CopyResource` only between matching descs.

## Frontend / pages

- `window.__MAGELIGHT__` is installed BEFORE page scripts (window-object-ready); pages must not
  assume listeners exist before DOM ready. Outbound calls target `window[name]`; a missing shim is
  logged as "not a function" — a new C++→JS channel needs its `window` function on the page.
- Ultralight rendering pitfalls are the frontend lens's job (translucent borders, clip-path,
  inset shadows, `zoom`).

## Harness / tools

- `tools/desktop-harness` must keep loading the UNPATCHED driver and the stock SDK DLLs; the
  file-system root must contain `resources/`.

## Scoring

| Confidence | Meaning |
|------------|---------|
| 95-100 | Provably wrong — crash, corruption, or wrong result |
| 85-94 | Very likely; the edge case is routine (load screen, resize, second view, revert) |
| 75-84 | Probable; depends on ordering or a consumer's usage |
| 60-74 | Possible; worth a look |

## Output

JSON array then a short summary:
```json
[{"agent":"bugs","file":"src/Magelight.cpp","line":0,"severity":"high","confidence":85,"category":"lifetime","description":"...","suggestion":"..."}]
```
`[]` if nothing found. Verify every cited line against the real file before reporting.
