// Magelight.CppPanel — a minimal Magelight C++ consumer.
// One fullscreen panel, opened on a hotkey, with a value pushed to the page
// and a click received back. Build as an ordinary CommonLibSSE-NG SKSE plugin;
// copy api/MagelightUI_API.h and api/MagelightUI_Mod.h next to this file.

#include <SKSE/SKSE.h>               // CommonLib before the Magelight header: it includes <windows.h>
#include "MagelightUI_Mod.h"        // pulls in MagelightUI_API.h

#include <filesystem>
#include <string>

namespace {
    MAGELIGHT_API::MagelightMod g_mod;
    MAGELIGHT_API::ViewId       g_view = 0;
    int                         g_counter = 41;

    // Pages live under Data\Magelight\<modId>\ — resolve the ABSOLUTE path from
    // the running exe (the host loads a file:/// URL, and pins reads to this
    // folder). modId here is "MyMod"; the folder must match.
    std::string PagePath()
    {
        wchar_t buf[MAX_PATH]{};
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        auto p = std::filesystem::path(buf).parent_path()
               / L"Data" / L"Magelight" / L"MyMod" / L"views" / L"panel" / L"index.html";
        return p.string();
    }

    // A click from the page: window.magelight.send("panelClick", "...").
    // JS listeners run inline on the RENDER thread, not the callback thread
    // chosen in acquire(). Magelight calls are fine here (any thread); to touch
    // RE::, copy `arg` and hand the work to g_mod.v4()->PostGameTask, never SKSE's
    // AddTask: on VR that can hang the game from inside a frame. This example accepts
    // host 0.16.0+, so gate it on hostVersionNumber >= 3001 (docs/CPP.md shows how).
    // JsListenerFn4: (view, argument, user). The name is fixed per
    // registration ("panelClick"), so it is not an argument.
    void OnPanelClick(MAGELIGHT_API::ViewId view, const char* arg, void*)
    {
        SKSE::log::info("MyMod: page sent panelClick = '{}'", arg ? arg : "");
        // Push a fresh number back to the page.
        const std::string js = "window.setNumber && window.setNumber(" + std::to_string(++g_counter) + ")";
        g_mod.v4()->InvokeJS(view, js.c_str());
    }

    void OnDataLoaded()
    {
        // false = Magelight absent or older than 0.16.0 (it logs the reason).
        if (!g_mod.acquire("MyMod", "My Mod", 0, 16, 0)) {
            SKSE::log::warn("MyMod: Magelight UI 0.16.0+ not present — UI disabled");
            return;
        }

        const std::string page = PagePath();
        g_view = g_mod.createView("panel", page.c_str(),
                                  MAGELIGHT_API::Layer::Panel,
                                  { .fullscreen = true });
        if (!g_view) {
            SKSE::log::error("MyMod: view refused — {}", g_mod.v4()->GetLastErrorMessage(g_mod.id()));
            return;
        }

        // Optional (0.31.1+): the host's drawn cursor in your own colours (here a blue steel) over this view.
        // Older hosts lack the call, so gate it on the version.
        if (g_mod.v4()->hostVersionNumber >= 3101) {
            MAGELIGHT_API::CursorTint tint{ sizeof(tint) };
            tint.lit = 0xFFDCE6F0;    // 0xAARRGGBB; alpha 0 keeps the host's colour
            tint.shade = 0xFF6E8296;
            tint.ink = 0xFF0B1118;
            tint.glow = 0xFF7CB2CE;
            tint.ibeam = 0xFFDCE6F0;
            g_mod.v4()->SetViewCursorTint(g_view, &tint);
        }

        // Receive clicks; log closes. (Raw ABI: demux onEvent for UIModeExited
        // yourself; the wrapper's onClose does that.)
        g_mod.v4()->RegisterJSListenerEx(g_view, "panelClick", &OnPanelClick, nullptr);
        g_mod.onClose(g_view, [](MAGELIGHT_API::ViewId) { SKSE::log::info("MyMod: panel closed"); });

        // Seed the number. InvokeJS on a view whose page has not loaded yet is
        // held by the host until the DOM is ready, so this is safe to send
        // right after createView; OnPanelClick re-pushes on every click.
        g_mod.v4()->InvokeJS(g_view, ("window.setNumber && window.setNumber(" + std::to_string(g_counter) + ")").c_str());

        // F13 (DX scancode 0x64) toggles the panel: a Panel-layer view takes UI
        // mode (cursor + suspended controls) when its key fires.
        g_mod.v4()->BindHotkey(g_view, 0x64, MAGELIGHT_API::kHotkeyActionToggleUIMode);
        SKSE::log::info("MyMod: panel ready — press F13");
    }

    void OnMessage(SKSE::MessagingInterface::Message* m)
    {
        if (m && m->type == SKSE::MessagingInterface::kDataLoaded) OnDataLoaded();
    }
}

extern "C" __declspec(dllexport) bool SKSEPlugin_Load(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);
    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
    return true;
}
