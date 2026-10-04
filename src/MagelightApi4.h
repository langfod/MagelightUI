#pragma once
// Internal declarations for the v4 mod registry (src/MagelightApi4.cpp).
// The public ABI lives in api/MagelightUI_API.h; MagelightApiExport.cpp
// adapts these to the C function-pointer table.

// The host never needs the consumer-side loader (and CommonLib forbids a
// Windows API include ahead of it).
#ifndef MAGELIGHT_API_NO_LOADER
#  define MAGELIGHT_API_NO_LOADER
#endif
#include "../api/MagelightUI_API.h"

#include <cstdint>
#include <string>

namespace Magelight::Api4 {

    using MAGELIGHT_API::ImageId;
    using MAGELIGHT_API::ModDesc;
    using MAGELIGHT_API::ModId;
    using MAGELIGHT_API::Result;
    using MAGELIGHT_API::ViewDesc;
    using MAGELIGHT_API::ViewId;
    using MAGELIGHT_API::CallbackThread;
    using MAGELIGHT_API::EventFn;
    using MAGELIGHT_API::LogFn;

    Result RegisterMod(const ModDesc* desc, ModId* outMod);
    void   UnregisterMod(ModId mod);
    Result CreateViewEx(ModId mod, const ViewDesc* desc, ViewId* outView);
    Result DestroyView(ViewId view);
    Result ReloadView(ViewId view);
    Result Navigate(ViewId view, const char* url);
    Result RegisterJSListenerEx(ViewId view, const char* name, MAGELIGHT_API::JsListenerFn4 cb, void* user);
    Result RequestUIMode(ViewId view, std::uint32_t flags);
    Result ReleaseUIMode(ModId mod);
    ModId  GetUIModeOwner();
    Result RaiseView(ViewId view);
    Result GetViewInfo(ViewId view, ViewDesc* out);
    void   GetDisplaySize(std::int32_t* w, std::int32_t* h);
    std::int32_t QueryCapability(const char* name);
    const char* GetLastErrorMessage(ModId mod);
    Result BindHotkey(ViewId view, std::uint32_t dxScancode, std::uint32_t action);
    // 0.18.0 (VR-4)
    Result SetViewVRPlacement(ViewId view, const MAGELIGHT_API::VRPlacementDesc* desc);
    Result GetViewVRPlacement(ViewId view, MAGELIGHT_API::VRPlacementDesc* out);
    Result RecenterVRView(ViewId view);
    Result BindVRHotkey(ViewId view, const MAGELIGHT_API::VRHotkeyDesc* desc);
    Result BindVRHotkeyCallback(ViewId view, const MAGELIGHT_API::VRHotkeyDesc* desc,
                                MAGELIGHT_API::VRHotkeyFn cb, void* user);
    Result SetVRButtonListener(MAGELIGHT_API::VRButtonEdgeFn cb, void* user);
    Result RegisterTextureImageEx(ModId mod, const char* name, ID3D11ShaderResourceView* srv,
                                  std::uint32_t width, std::uint32_t height, ImageId* out);
    std::uint32_t HostVersionNumber();

    // The host's own probe view, created through the v4 path (dogfood).
    ViewId CreateHostProbeView(const char* htmlPath, int x, int y, int w, int h);

    // Page globals (InstallBridgeShims): the view's identity and the host's
    // capability table as JSON fragments for window.__MAGELIGHT__.
    std::string ViewIdentityJson(ViewId view);   // "modId":"..","viewName":".." ("" both for v1-v3 views)
    std::string CapabilitiesJson();              // {"gpu":1,"textureImage":1,...}

    // 0.16.0
    Result SetViewCutout(ViewId view, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h);
    Result SetViewHibernate(ViewId view, std::uint32_t idleMs);

    // 0.14.0: evaluate with a result, cb on the mod's callback thread.
    Result EvalJS(ViewId view, const char* script, MAGELIGHT_API::JsResultFn cb, void* user);
    // Magelight.json "hotkeys": {"ModId/viewName": code} — consulted by BindHotkey
    // (0 = the user disabled that hotkey). Key is case-insensitive.
    void SetHotkeyOverride(const char* modSlashView, std::uint32_t dxScancode);

    // Host input sink → hotkey registry (game thread, key-down only). True
    // when a binding handled the scancode. The host's own toggle key never
    // reaches here.
    // Two sources feed it: the engine input sink (world) and the focused
    // window procedure (a page holding key focus — the engine sink goes quiet
    // for our menu-context engine menu). A repeat of the same scancode within
    // the debounce window is swallowed so a key seen by both cannot close and
    // reopen in one press.
    bool DispatchHotkey(std::uint32_t dxScancode, const char* source = "sink");
    // The gated toggle a hotkey performs, keyed by VIEW — marshals to the game
    // thread and applies the engine-menu/console/text-entry gates and the
    // "while focused only this view's own key closes" rule. Shared by the
    // keyboard hotkeys and the VR controller bindings. Any thread.
    void DispatchViewAction(ViewId view, std::uint32_t action, const char* source);

    // In-host consumers: RegisterMod without a ModDesc. A manifest mod with
    // the slug is adopted. script = the Papyrus tier: the mod is script-owned,
    // a slug a script already registered returns that mod (Ok), and a slug a
    // plugin registered is Denied. Otherwise a taken slug is InvalidMod.
    Result RegisterModEx(const char* modId, const char* displayName, std::uint32_t version, CallbackThread thread,
                         EventFn onEvent, LogFn onLog, void* user, const char* sessionName, ModId* outMod,
                         bool script = false);
    // What the Papyrus tier may do with a mod or a view's mod: act only when
    // Allowed. jar (optional) receives the mod's session name ("default" for
    // the shared jar).
    enum class ScriptAccess {
        Allowed,     // script-owned, in a storage jar no plugin's pages use
        PluginMod,   // registered by a plugin, or a manifest mod a DLL adopted
        PluginJar,   // script-owned, but its jar is the default one or a plugin's session (this session's or on record)
        Unknown,     // no such mod, or a view no mod owns (v1-v3)
    };
    ScriptAccess ScriptAccessOfMod(ModId mod, std::string* jar = nullptr);
    ScriptAccess ScriptAccessOfView(ViewId view, std::string* jar = nullptr);
    ModId FindMod(const char* modId);                       // 0 = not registered
    ViewId FindViewByName(ModId mod, const char* name);     // 0 = none
    Result FindView(ModId mod, const char* name, ViewId* out);   // 0.26.8 C-ABI wrapper
    Result SetViewScale(ViewId view, float scale);   // 0.26.9 device scale
    // 0.28.0
    Result SetEscapeCapture(ViewId view, bool capture);
    Result ShowInspector(ViewId view, bool show);
    Result IsInspectorVisible(ViewId view, bool* outVisible);
    Result SetViewOrder(ViewId view, std::uint64_t order);
    Result GetViewOrder(ViewId view, std::uint64_t* outOrder);
    Result SetScrollStep(ViewId view, int px);               // 0.28.0 px per wheel notch
    Result SetNetworkPolicy(ModId mod, MAGELIGHT_API::NetworkPolicy policy);   // per-mod network reach (FileOnly by default)
    Result PlayUISound(ViewId view, const char* name);                        // 0.29.0 host-played UI sound
    Result SetViewSounds(ViewId view, const char* open, const char* close);   // 0.29.0 open/close sounds for a view
    Result PostGameTask(MAGELIGHT_API::GameTaskFn fn, void* user);              // 0.30.1 game-thread task, safe in a frame
    Result SetViewFreezeWorld(ViewId view, bool freeze);                        // 0.31.0 world freeze behind a paused page
    Result SetViewLoadOnShow(ViewId view, bool onShow);                         // 0.31.0 first load waits for a show
    // Mods refused with HostTooOld, as the version gate's payload:
    // {"host":"x.y.z","mods":[{"modId":"..","needs":packed},..]}; "" when none.
    std::string TooOldJson();

    // Manifest mods (MagelightManifest.cpp): registered without a sink; a
    // later RegisterMod with the same modId ADOPTS the entry instead of
    // being refused. sessionName: nullptr = isolated (the slug), "default" =
    // the shared session. manifestDir: the mod's folder, for the log.
    Result RegisterManifestMod(const char* modId, const char* displayName, std::uint32_t version,
                               std::uint32_t minHostVersion, const char* sessionName, const char* manifestDir,
                               ModId* outMod);

}  // namespace Magelight::Api4
