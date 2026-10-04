# Magelight.CppPanel — a minimal C++ plugin example

The smallest complete SKSE plugin that drives a Magelight page from C++: it
registers a mod, creates one fullscreen panel, opens it on a hotkey, sends the
page a value, receives a click back, and logs when the user closes it. Copy it
as the starting point for a C++-tier mod.

## Files

- `src/plugin.cpp` — the whole plugin (~100 lines). Uses the header-only
  wrapper `api/MagelightUI_Mod.h`; the raw-ABI equivalent is noted in comments.
- `views/panel/index.html` — the page. Talks to the plugin with
  `window.magelight.send` / `.on`.
- The two headers this depends on are the repo's `api/MagelightUI_API.h` and
  `api/MagelightUI_Mod.h` — copy both next to `plugin.cpp` (or add `api/` to
  your include path).

## Build

This is an ordinary CommonLibSSE-NG SKSE plugin — build it with your usual
plugin template (the same CMake + vcpkg + CommonLibSSE-NG setup the host uses,
or any CommonLibSSE-NG starter). There is nothing Magelight-specific in the
build: the two `api/` headers are header-only and reached at runtime by
`GetProcAddress`, so you need **no** import library and **no** link against
Magelight. Just compile `plugin.cpp` with the two headers on the include path.

## Install & run

1. Put the built `MagelightCppPanel.dll` in `Data\SKSE\Plugins\`.
2. Put `views\panel\index.html` at `Data\Magelight\MyMod\views\panel\index.html`
   (the `modId` in `plugin.cpp` is `MyMod` — the folder must match).
3. Install Magelight UI (0.16.0+).
4. In game, press **F13** (DirectInput scancode 0x64; change it in `plugin.cpp`). The panel
   opens with the cursor up; it shows a number the plugin sent, a button that
   sends a click back (watch `Magelight.log`), and Escape closes it.

## What to change first

- `modId` / `displayName` in `acquire(...)` and the `Data\Magelight\<modId>\`
  path in `PagePath()` — they must match.
- The hotkey scancode in `BindHotkey`.
- The page — it's plain HTML/CSS/JS; wire more `send`/`on` channels and push
  data from C++ with `InvokeJS` / `EvalJS`.
- The cursor colours in the `SetViewCursorTint` block (Magelight UI 0.31.1+),
  or delete the block to keep the host's.

See [../../docs/CPP.md](../../docs/CPP.md) for the full C++ tier guide and the
threading/versioning rules.
