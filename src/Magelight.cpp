// Magelight UI — spike core. See Magelight.h for the threading model.

#include "Magelight.h"
#include "MagelightPresenter.h"
#include "MagelightVR.h"
#include "MagelightApi4.h"
#include "MagelightSound.h"
#include "MagelightManifest.h"
#include "MagelightDevWatch.h"
#include "MagelightCursorArt.h"
#include "HotkeyNames.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include "MagelightGameTask.h"   // after CommonLib, which must precede windows.h

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <intrin.h>
#include <MinHook.h>
#include <imm.h>   // IME (0.27.0)
#include <windowsx.h>
#include <tlhelp32.h>
#include <chrono>
#include <thread>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <JavaScriptCore/JavaScript.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <AppCore/Platform.h>
#include <Ultralight/ConsoleMessage.h>
#include <Ultralight/ImageSource.h>
#include <Ultralight/RenderTarget.h>
#include <Ultralight/Ultralight.h>
#include <Ultralight/platform/Config.h>
#include <Ultralight/platform/Logger.h>
#include <Ultralight/platform/Platform.h>
#include <Ultralight/platform/Surface.h>

namespace Magelight {

    // ── Host settings (Magelight.json; all optional) ────────────────────────
    // Location: My Games/Skyrim Special Edition/SKSE/Magelight.json (user
    // data, install-independent — beside the log and the cache), falling back
    // to the runtime dir for hand-installed setups.
    // {
    //   "toggleKey": 201,     // DirectInput scancode for the UI-mode toggle
    //   "demoViews": false,   // create the badge + playground demo views
    //   "imageProbe": false   // create the ImageSource probe page (views/probe)
    //   "devMode": false      // hot reload every v4 page folder, JS error overlay (reload/inspector keys unbound for now)
    //   "fontHinting": "normal", "fontGamma": 1.8   // text rasterization: smooth|normal|monochrome|none, gamma 1.0-3.0
    //   "vr": { "enabled": true, "submitViews": true, "mirror": true, "alpha": "straight",
    //            "beam": true, "beamAlpha": 0.55, "cursorScale": 0.012, "cursorDot": true,   // laser-end pointer: a point (0.26.12); false = the arrow art
    //            "suppressRuntimeLaser": true, "aimUseTip": true, "aimPitchDeg": -35,
    //            "runtimeKeyboard": false, "keyboard": true,
    //            "panelWidth": 1600, "panelHeight": 900,   // fullscreen views in VR (NOT the mirror's aspect)
    //            "panelFollow": false,   // true = panels glide back to centre when you look away
    //            "panelDistanceM": 1.2, "panelWidthM": 1.6, "panelHeightOffsetM": -0.1,  // the page layer's quad
    //            "followAngleDeg": 45, "followMinDeg": 80, "followDistM": 0.5 }
    //                                                  // lazy-follow deadzone: margin past the panel EDGE,
    //                                                  // with an absolute floor in degrees
    //                                                  // Skyrim VR presenter (docs/VR_PRESENTER.md)
    //   "logLevel": "info"    // trace | debug | info | warn | error — Magelight.log verbosity
    //   "stallWatchdog": true, "stallThresholdMs": 1500   // 0.28.3: log the stalled thread's stack when no
    //                                                     // frame presents this long (250-60000)
    //   "hotkeys": { "ModId/viewName": "F7", "Other/hud": 0 }   // rebind (or 0 = disable) any mod's hotkey
    // }
    // Toggle key default: PAGE UP (201). Moved off Home in 0.3.1: Home sits
    // in hotkey territory other UI mods use, and two systems doing their
    // cursor/controls/window dance in the same instant is a crash recipe we
    // don't need to be part of. demoViews defaults FALSE now that a real
    // consumer (SeverActions) rides the host — the always-on badge and the
    // Page Up playground are dev surfaces, not something end users should
    // see; flip the flag to field-test the host by itself.
    static std::atomic<std::uint32_t> s_toggleKey{ 201 };
    static std::atomic<bool> s_devMode{ false };   // Magelight.json "devMode": hot reload + JS error overlay
    // Magelight.json "forceCpu": skip the GPU driver and run Ultralight's CPU
    // rasterizer (the surface path) — a diagnostic A/B and a user escape
    // hatch when the accelerated path misrenders on some load order.
    static std::atomic<bool> s_forceCpu{ false };
    static bool s_demoViews = false;
    // Text rasterization (Magelight.json "fontHinting": "smooth" | "normal" |
    // "monochrome" | "none", "fontGamma": number). Ultralight draws grayscale
    // anti-aliased text with a 1.8 gamma; Chromium-hosted UIs had
    // subpixel text, so the same page reads softer here (field 2026-09-05,
    // flat 1440p). Exposed so the difference can be A/B'd in one session
    // instead of a rebuild per guess.
    static std::string s_fontHinting = "normal";
    static double      s_fontGamma   = 1.8;
    // Cursor art: the flat cursor is drawn in code (MagelightCursorArt.h) and follows the page's CSS cursor.
    // "cursorFile" (relative to the runtime dir, or absolute; default none) replaces it with a still image,
    // "cursorHeight" is the drawn height in px at 1080p (scaled with resolution) and "cursorHotspotX/Y"
    // (normalized 0..1, the pixel that IS the pointer) place a cursorFile image. A missing or unreadable file
    // keeps the drawn cursor.
    static std::string s_cursorFile;
    // 24 at 1080p is ~1.2x the Windows arrow at 100% scaling (~20px) and stays so at any resolution (48 at
    // 4K, against ~40 at Windows' 200%). VR sizes its pointer from the panel (vr.cursorScale).
    static float       s_cursorHeight = 24.0f;
    static float       s_cursorHotX = 0.0f, s_cursorHotY = 0.0f;
    static bool        s_cursorCustom = false;   // custom art loaded (render thread)
    static int         s_cursorImgW = 0, s_cursorImgH = 0;
    // "imageProbe": true — the ImageSource probe (views/probe): a host-repainted
    // texture placed eight ways in a page. Dev-only; see TickImageProbe.
    static bool s_imageProbe = false;
    // (LoadHostSettings is defined after s_runtimeDir, below.)

    // Milestone-3 playground: hover styling, click counters, a text field, and
    // a scrollable list — one widget per input path (mouse move/down/up, key,
    // char, wheel). The clock keeps proving the JS engine ticks.
    static constexpr const char* kBannerHTML = R"HTML(
<!DOCTYPE html>
<html><head><style>
  html, body { margin:0; background:transparent; overflow:hidden;
               font-family:Georgia,serif; color:#3a2d17; }
  .card {
    box-sizing:border-box; width:100%; height:100%;
    background:linear-gradient(160deg,#efe3c8,#e2d2ae);
    border:2px solid #8a6d3b; border-radius:10px;
    padding:16px 20px; box-shadow:inset 0 0 40px rgba(120,90,40,.25);
  }
  h1 { margin:0 0 2px; font-size:24px; }
  .sub { font-style:italic; color:#6b5527; font-size:13px; margin-bottom:10px; }
  .row { display:flex; gap:10px; align-items:center; margin-top:10px; }
  button {
    font:inherit; font-size:14px; padding:6px 14px; cursor:pointer;
    background:#e8d9b5; color:#3a2d17; border:1px solid #8a6d3b; border-radius:6px;
  }
  button:hover { background:#8a6d3b; color:#f4ecd8; }
  button:active { transform:translateY(1px); }
  input[type=text] {
    font:inherit; font-size:14px; flex:1; padding:6px 10px;
    background:#f7efdb; border:1px solid #8a6d3b; border-radius:6px; color:#3a2d17;
  }
  #list { margin-top:10px; height:110px; overflow-y:auto; border:1px solid #b39a67;
          border-radius:6px; background:rgba(255,250,235,.55); padding:4px 10px; font-size:13px; }
  #list div { padding:2px 0; border-bottom:1px dotted #c9b184; }
  .foot { display:flex; justify-content:space-between; margin-top:10px; font-size:12px;
          color:#6b5527; }
  .ok { color:#3f6d2a; font-weight:bold; }
  #clock { font-variant-numeric:tabular-nums; }
</style></head>
<body><div class="card">
  <h1>&#10022; Magelight UI &mdash; input playground</h1>
  <div class="sub">GPU-rendered by Ultralight, input routed by our own host. ESC or Page Up closes.</div>
  <div class="row">
    <button id="btn">Click me</button>
    <span id="clicks">0 clicks</span>
    <span id="hover" style="margin-left:auto">hover: &mdash;</span>
  </div>
  <div class="row">
    <input id="txt" type="text" placeholder="type here to test the keyboard&hellip;">
  </div>
  <div class="row"><span id="echo" style="font-size:13px;color:#6b5527">echo: &mdash;</span></div>
  <div id="list"></div>
  <div class="foot">
    <span>Renderer: <span class="ok">GPU</span> &middot; JS: <span class="ok" id="js">warming&hellip;</span></span>
    <span id="clock">--:--:--</span>
  </div>
</div>
<script>
  var clicks = 0;
  var btn = document.getElementById('btn');
  btn.addEventListener('click', function(){
    clicks++;
    document.getElementById('clicks').textContent = clicks + ' click' + (clicks===1?'':'s');
  });
  btn.addEventListener('mouseenter', function(){ document.getElementById('hover').textContent = 'hover: button'; });
  btn.addEventListener('mouseleave', function(){ document.getElementById('hover').textContent = 'hover: —'; });
  document.getElementById('txt').addEventListener('input', function(e){
    document.getElementById('echo').textContent = 'echo: ' + e.target.value;
  });
  var list = document.getElementById('list');
  for (var i = 1; i <= 40; i++) {
    var d = document.createElement('div');
    d.textContent = 'Scroll test row ' + i + ' — wheel over this box';
    list.appendChild(d);
  }
  document.getElementById('js').textContent = 'executing';
  setInterval(function(){
    document.getElementById('clock').textContent = new Date().toLocaleTimeString();
  }, 1000);
</script>
</body></html>
)HTML";

    // ── State ───────────────────────────────────────────────────────────────
    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

    static std::atomic<bool> s_runtimeReady{ false };
    static std::atomic<bool> s_hookInstalled{ false };
    // Set at kPostLoadGame/kNewGame. Ultralight is NOT created until the world
    // is up: Skyrim presents main-menu frames from a DIFFERENT thread than
    // in-game frames (field logs 0.3.4/0.3.5 — two distinct present TIDs per
    // session), and WebKit binds its "main thread" at Renderer::Create, then
    // RELEASE_ASSERTs isMainThread() in focus/event dispatch and thread-pool
    // startup. Its CRASH() is a fastfail: no exception dispatch, no VEH, no
    // crashlog — the entire milestone-3 CTD family. Creating on the in-game
    // render thread, and skipping frames from any other thread, is the fix.
    static std::atomic<bool> s_worldReady{ false };
    static DWORD             s_ulThreadId = 0;    // thread Ultralight was created on
    static DWORD             s_mainThreadId = 0;  // captured at plugin load (game main thread)
    // Stall watchdog (0.28.3): the present hook stamps every frame; a thread
    // notices when the stamps stop and samples the stuck thread's stack. See
    // StallWatchdog below. Magelight.json "stallWatchdog": false disables.
    static std::atomic<long long> s_lastPresentMs{ 0 };
    static std::atomic<DWORD>     s_lastPresentTid{ 0 };
    static std::atomic<bool>      s_stallWatchdog{ true };
    static std::atomic<int>       s_stallThresholdMs{ 1500 };
    static std::atomic<bool> s_renderDead{ false };  // latched on any init failure
    static PresentFn         s_origPresent = nullptr;

    // Why the overlay died, for the RenderDead event, RequestUIMode's refusal and the log. Written once and
    // never allocated (it is filled from inside an SEH filter): s_renderDeadReasonState goes 0 -> 1 (writing)
    // -> 2 (readable, release), and readers check for 2 (acquire).
    static char             s_renderDeadReason[192] = {};
    static std::atomic<int> s_renderDeadReasonState{ 0 };

    static void NoteRenderFailure(const char* what, const char* detail = nullptr)
    {
        int expected = 0;
        if (!s_renderDeadReasonState.compare_exchange_strong(expected, 1)) return;
        std::size_t n = 0;
        const auto append = [&n](const char* t) {
            for (; t && *t && n + 1 < sizeof(s_renderDeadReason); ++t) s_renderDeadReason[n++] = *t;
        };
        append(what);
        if (detail && *detail) { append(": "); append(detail); }
        s_renderDeadReason[n] = '\0';
        s_renderDeadReasonState.store(2, std::memory_order_release);
    }

    static const char* RenderDeadReasonText()
    {
        return s_renderDeadReasonState.load(std::memory_order_acquire) == 2 ? s_renderDeadReason : "";
    }

    // Swapchain trust. A game swapchain that is not a full dxgi D3D11 swapchain (Community Shaders' frame
    // generation proxy implements only IDXGISwapChain and sits on Direct3D 12; a wrapper whose GetDevice fails)
    // marks the chain untrusted: no Present1 patch, no late composite, and a present target that never over-
    // releases (DrawOverlay). Latched: once set it is never cleared.
    static std::atomic<bool> s_untrustedChain{ false };
    static std::atomic<bool> s_deviceFromEngine{ false };   // s_device came from the engine, not the swapchain
    static std::atomic<bool> s_lateComposite{ false };       // composite inside dxgi's own Present (InstallLatePresent)
    static std::atomic<bool> s_deadNoticePending{ false };   // the render-death HUD notice waits for the HUD

    // Render-thread-owned (created lazily inside the hook; never touched
    // elsewhere after that).
    static bool                           s_renderInit = false;
    static std::atomic<bool>              s_rendererUp{ false };   // EnsureRenderInit succeeded (any-thread read)
    static ultralight::RefPtr<ultralight::Renderer> s_ulRenderer;
    static ID3D11Device*                  s_device = nullptr;   // from the swapchain's GetDevice; never Release (a wrapper may not AddRef)
    static ID3D11DeviceContext*           s_context = nullptr;  // owned ref
    static ID3D11VertexShader*            s_vs = nullptr;
    static ID3D11PixelShader*             s_ps = nullptr;
    static ID3D11PixelShader*             s_psStraight = nullptr;   // VR copy pass (premultiplied -> straight)
    static ID3D11InputLayout*             s_layout = nullptr;
    static ID3D11Buffer*                  s_vb = nullptr;
    static ID3D11Buffer*                  s_cb = nullptr;   // PS b0: the cutout rect
    static ID3D11SamplerState*            s_sampler = nullptr;
    static ID3D11BlendState*              s_blend = nullptr;
    static ID3D11BlendState*              s_blendStraight = nullptr;   // VR copy pass: straight-alpha 'over'
    static ID3D11Texture2D*               s_cursorTex = nullptr;
    static ID3D11ShaderResourceView*      s_cursorSrv = nullptr;
    static ID3D11Texture2D*               s_dotTex = nullptr;      // VR laser-end pointer (0.26.12)
    static ID3D11ShaderResourceView*      s_dotSrv = nullptr;

    // Cursor position mirrored from MenuCursor by the input sink (game side);
    // the render thread reads it to draw our own cursor sprite. The vanilla
    // cursor is drawn in the game's UI pass — BEFORE Present — so it can never
    // appear above our overlay; we draw our own on top instead.
    static std::atomic<int> s_cursorPosX{ 0 };
    static std::atomic<int> s_cursorPosY{ 0 };
    // The cursor a page asks for (MlView::pageCursor, from ViewListener::OnChangeCursor) picks the drawn cursor's
    // state; the left button's state (input sink, game thread) its press.
    enum class PageCursor : int { Arrow = 0, Clickable = 1, Text = 2 };
    static std::atomic<bool> s_mouseDown{ false };
    // Backbuffer size for the cursor clamp (render thread writes, input sink reads).
    static std::atomic<int> s_backbufferW{ 0 }, s_backbufferH{ 0 };

    // ── IME (0.27.0): native composition for CJK input ──────────────────
    // The window's IME context is ATTACHED only while a text field in the
    // UI-mode view has focus (the bridge's '__textfocus' signal) and DETACHED
    // otherwise, so gameplay keys never raise a composition. The pre-edit is
    // drawn INLINE in the field (it rides as the field's selection, so the
    // commit — delivered as ordinary characters — replaces it and a cancel
    // deletes it), the OS candidate list is placed at the caret the page
    // reports, and keys the IME consumed (VK_PROCESSKEY) never reach the
    // page. Text-field focus is detected automatically from the page's own
    // focus events (focusin / focusout on real inputs), so a page needs no
    // focus signalling of its own. VR is exempt: the OS IME UI is invisible in a headset
    // and the runtime keyboard owns text there.
    static constexpr UINT kImeCtlMsg = WM_APP + 0x4F46;   // wparam: 1 attach, 0 detach, 2 reposition candidates
    static std::atomic<bool> s_imeAttached{ false };
    static std::atomic<bool> s_imeComposing{ false };
    static std::atomic<int>  s_imeCaretX{ -1 }, s_imeCaretY{ -1 }, s_imeCaretH{ 0 };   // client px; x<0 = unknown

    static std::filesystem::path s_runtimeDir;  // Data/SKSE/Plugins/Magelight
    // Magelight.json "presentHook": "auto" (late composite only behind an earlier Present hook on a plain dxgi
    // swapchain; never behind a proxy, never on VR), "late" (always try it), "vtable" (never). See
    // InstallLatePresent.
    static std::string s_presentHookMode = "auto";
    // Magelight.json "composite": where views are drawn onto the frame. "present" draws them over the back buffer
    // at Present; "ui" draws them in the game's UI pass (MagelightOverlayMenu::PostDisplay), so a mod that keeps
    // the game's UI apart from the scene (Skyrim Upscaler's HUD Fix) keeps ours with it. "auto" picks "ui" when
    // Skyrim Upscaler is loaded or the game swapchain is NVIDIA Streamline's (sl.interposer.dll: Skyrim
    // Upscaler, Community Shaders' and Open Shaders' upscaling, whose frame generation drops what is drawn at
    // Present), never on VR. See UiPassActive.
    static std::string s_compositeMode = "auto";

    // Read Magelight.json from the runtime dir (see the settings block at the
    // top of the file for the schema). Missing/corrupt file = defaults.
    static void LoadHostSettings()
    {
        try {
            // User config lives in the SKSE user-data dir (My Games/.../SKSE/
            // Magelight.json — beside the log and the cache, independent of
            // how or where the mod was installed; a mod-folder copy is only
            // valid for the ONE mod folder that happens to be active). The
            // runtime dir is the fallback for hand-installed setups.
            std::vector<std::filesystem::path> candidates;
            if (auto dir = SKSE::log::log_directory()) candidates.push_back(*dir / L"Magelight.json");
            candidates.push_back(s_runtimeDir / L"Magelight.json");
            std::filesystem::path p;
            std::error_code ec;
            for (const auto& c : candidates)
                if (std::filesystem::exists(c, ec)) { p = c; break; }
            if (p.empty()) {
                SKSE::log::info("Magelight: no Magelight.json (looked in {} and {}) — defaults in effect",
                    candidates.front().string(), candidates.back().string());
                return;
            }
            SKSE::log::info("Magelight: settings file {}", p.string());
            std::ifstream f(p);
            nlohmann::json j;
            f >> j;
            if (!j.is_object()) return;
            if (auto it = j.find("toggleKey"); it != j.end() && it->is_number_unsigned())
                s_toggleKey.store(it->get<std::uint32_t>());
            if (auto it = j.find("forceCpu"); it != j.end() && it->is_boolean())
                s_forceCpu.store(it->get<bool>());
            if (auto it = j.find("presentHook"); it != j.end() && it->is_string())
                s_presentHookMode = HotkeyNames::Lower(it->get<std::string>());
            if (auto it = j.find("composite"); it != j.end() && it->is_string())
                s_compositeMode = HotkeyNames::Lower(it->get<std::string>());
            if (auto it = j.find("devMode"); it != j.end() && it->is_boolean())
                s_devMode.store(it->get<bool>());
            if (auto it = j.find("fontHinting"); it != j.end() && it->is_string())
                s_fontHinting = HotkeyNames::Lower(it->get<std::string>());
            if (auto it = j.find("fontGamma"); it != j.end() && it->is_number())
                s_fontGamma = std::clamp(it->get<double>(), 1.0, 3.0);
            if (auto it = j.find("logLevel"); it != j.end() && it->is_string()) {
                const std::string l = HotkeyNames::Lower(it->get<std::string>());
                const auto lvl = l == "trace" ? spdlog::level::trace : l == "debug" ? spdlog::level::debug
                               : l == "warn" ? spdlog::level::warn : l == "error" ? spdlog::level::err
                               : spdlog::level::info;
                if (auto lg = spdlog::default_logger()) lg->set_level(lvl);
                SKSE::log::info("Magelight: log level {}", l);
            }
            if (auto it = j.find("hotkeys"); it != j.end() && it->is_object()) {
                for (auto hk = it->begin(); hk != it->end(); ++hk) {
                    std::string err;
                    const std::uint32_t code = HotkeyNames::Parse(hk.value(), err);
                    if (!err.empty()) { SKSE::log::warn("Magelight: hotkeys['{}'] ignored — {}", hk.key(), err); continue; }
                    Api4::SetHotkeyOverride(hk.key().c_str(), code);
                }
            }
            if (auto it = j.find("vr"); it != j.end() && it->is_object()) {
                VR::Settings vs;
                if (auto v = it->find("enabled"); v != it->end() && v->is_boolean()) vs.enabled = v->get<bool>();
                if (auto v = it->find("submitViews"); v != it->end() && v->is_boolean()) vs.submitViews = v->get<bool>();
                if (auto v = it->find("mirror"); v != it->end() && v->is_boolean()) vs.mirror = v->get<bool>();
                if (auto v = it->find("alpha"); v != it->end() && v->is_string())
                    vs.straightAlpha = HotkeyNames::Lower(v->get<std::string>()) != "premultiplied";
                if (auto v = it->find("beam"); v != it->end() && v->is_boolean()) vs.beam = v->get<bool>();
                if (auto v = it->find("beamAlpha"); v != it->end() && v->is_number()) vs.beamAlpha = v->get<float>();
                if (auto v = it->find("cursorScale"); v != it->end() && v->is_number()) vs.cursorScale = v->get<float>();
                if (auto v = it->find("cursorDot"); v != it->end() && v->is_boolean()) vs.cursorDot = v->get<bool>();
                if (auto v = it->find("panelWidth"); v != it->end() && v->is_number_integer())
                    vs.panelWidth = v->get<int>();
                if (auto v = it->find("panelHeight"); v != it->end() && v->is_number_integer())
                    vs.panelHeight = v->get<int>();
                if (auto v = it->find("panelFollow"); v != it->end() && v->is_boolean())
                    vs.panelFollow = v->get<bool>();
                if (auto v = it->find("panelDistanceM"); v != it->end() && v->is_number())
                    vs.panelDistanceM = v->get<float>();
                if (auto v = it->find("panelWidthM"); v != it->end() && v->is_number())
                    vs.panelWidthM = v->get<float>();
                if (auto v = it->find("panelHeightOffsetM"); v != it->end() && v->is_number())
                    vs.panelHeightOffsetM = v->get<float>();
                if (auto v = it->find("followAngleDeg"); v != it->end() && v->is_number())
                    vs.followAngleDeg = v->get<float>();
                if (auto v = it->find("followMinDeg"); v != it->end() && v->is_number())
                    vs.followMinDeg = v->get<float>();
                if (auto v = it->find("followDistM"); v != it->end() && v->is_number())
                    vs.followDistM = v->get<float>();
                if (auto v = it->find("suppressRuntimeLaser"); v != it->end() && v->is_boolean())
                    vs.suppressRuntimeLaser = v->get<bool>();
                if (auto v = it->find("aimUseTip"); v != it->end() && v->is_boolean()) vs.aimUseTip = v->get<bool>();
                if (auto v = it->find("aimPitchDeg"); v != it->end() && v->is_number()) vs.aimPitchDeg = v->get<float>();
                if (auto v = it->find("runtimeKeyboard"); v != it->end() && v->is_boolean())
                    vs.runtimeKeyboard = v->get<bool>();
                if (auto v = it->find("keyboard"); v != it->end() && v->is_boolean())
                    vs.ownKeyboard = v->get<bool>();
                VR::Configure(vs);
            }
            if (auto it = j.find("demoViews"); it != j.end() && it->is_boolean())
                s_demoViews = it->get<bool>();
            if (auto it = j.find("imageProbe"); it != j.end() && it->is_boolean())
                s_imageProbe = it->get<bool>();
            if (auto it = j.find("stallWatchdog"); it != j.end() && it->is_boolean())
                s_stallWatchdog.store(it->get<bool>());
            if (auto it = j.find("stallThresholdMs"); it != j.end() && it->is_number())
                s_stallThresholdMs.store(std::clamp(it->get<int>(), 250, 60000));
            if (auto it = j.find("cursorFile"); it != j.end() && it->is_string())
                s_cursorFile = it->get<std::string>();
            if (auto it = j.find("cursorHeight"); it != j.end() && it->is_number())
                s_cursorHeight = std::clamp(it->get<float>(), 8.0f, 256.0f);
            if (auto it = j.find("cursorHotspotX"); it != j.end() && it->is_number())
                s_cursorHotX = std::clamp(it->get<float>(), 0.0f, 1.0f);
            if (auto it = j.find("cursorHotspotY"); it != j.end() && it->is_number())
                s_cursorHotY = std::clamp(it->get<float>(), 0.0f, 1.0f);
            SKSE::log::info("Magelight: settings loaded (toggleKey={}, demoViews={}, imageProbe={}, devMode={})",
                s_toggleKey.load(), s_demoViews, s_imageProbe, s_devMode.load());
        } catch (...) {
            SKSE::log::warn("Magelight: Magelight.json unreadable — defaults in effect");
        }
    }

    // ── GPU backend (MagelightGPU.dll — optional; CPU surface is the fallback)
    // Resolved via GetProcAddress after the runtime preload. See
    // gpu/MagelightGpuApi.h for the contract.
    using MgGpuLogFn      = void (*)(const char*);
    using MgGpuCreateFn   = void* (*)(ID3D11Device*, ID3D11DeviceContext*, MgGpuLogFn);
    using MgGpuDestroyFn  = void (*)(void*);
    using MgGpuGetDrvFn   = void* (*)(void*);
    using MgGpuHasFn      = int (*)(void*);
    using MgGpuDrawFn     = void (*)(void*);
    using MgGpuSrvFn      = void* (*)(void*, std::uint32_t);
    // External textures (ImageSource) — optional exports; absent = images unsupported.
    using MgGpuRegExtFn   = std::uint32_t (*)(void*, ID3D11ShaderResourceView*);
    using MgGpuSetExtFn   = int (*)(void*, std::uint32_t, ID3D11ShaderResourceView*);
    using MgGpuUnregExtFn = void (*)(void*, std::uint32_t);

    static MgGpuCreateFn  s_gpuCreate = nullptr;
    static MgGpuDestroyFn s_gpuDestroy = nullptr;
    static MgGpuGetDrvFn  s_gpuGetDriver = nullptr;
    static MgGpuHasFn     s_gpuHas = nullptr;
    static MgGpuDrawFn    s_gpuDraw = nullptr;
    static MgGpuSrvFn     s_gpuSrv = nullptr;
    static MgGpuRegExtFn   s_gpuRegExt = nullptr;
    static MgGpuSetExtFn   s_gpuSetExtSrv = nullptr;
    static MgGpuUnregExtFn s_gpuUnregExt = nullptr;
    static void*          s_gpu = nullptr;       // backend handle (render thread)
    static bool           s_gpuActive = false;   // accelerated path in use

    static void GpuLogBridge(const char* msg)
    {
        SKSE::log::error("Magelight: {}", msg ? msg : "?");
    }

    // ── View registry + JS <-> C++ bridge (per-view) ───────────────────────
    // Entries are created from ANY thread (CreateView queues intent); the
    // render thread materializes the Ultralight view once the renderer is up
    // and owns all `ul`/texture members after that. Everything else is
    // guarded by s_viewsMutex — contention is a couple of short lock holds
    // per frame plus API calls, so one mutex is plenty.
    // JS -> C++: one native dispatcher (__mlNative) is exposed to each page;
    // per-listener window shims forward (viewId, name, arg) to it — installed
    // at OnWindowObjectReady, BEFORE the page's own scripts run (the contract
    // SeverActions' frontend assumes).
    // C++ -> JS: InteropCall queues {fn, arg} on the view; the render thread
    // drains per frame and calls window.fn(arg) through JavaScriptCore with
    // the payload as a REAL string argument (no JS-source inlining).
    struct OutboundCall { std::string fn; std::string arg; bool raw; };
    static std::string MlJsonQuote(const std::string& in);
    static std::string PathToFileUrl(const std::filesystem::path& p);
    struct PendingEval { std::string script; JsResultFnInternal fn; void* user; };
    struct MlView {
        ViewId id = 0;
        std::string htmlPath;   // relative to the runtime dir; "" = inline banner
        // Host pinning: the ONE folder file:/// requests from this page may
        // read (its mod folder under Data\Magelight\<Mod>, or the runtime
        // dir for host pages). Everything else is refused in
        // MlNetworkListener — see PageRootFor.
        std::filesystem::path root;
        int x = 0, y = 0;       // negative = anchored from right/bottom edge
        int w = 0, h = 0;
        bool visible = false;
        bool clickThrough = false;
        bool isInspector = false;   // a hosted Web Inspector: visible only while UI mode is active
        int pageCursor = 0;         // PageCursor: the CSS cursor under the pointer, as the page last reported it
        float cutX = 0, cutY = 0, cutW = 0, cutH = 0;   // compositor cutout, view pixels (w<=0 = none)
        std::uint32_t hibernateMs = 0;  // 0 = never; else release the View after this long hidden
        std::uint64_t hiddenSince = 0;  // GetTickCount64 at the last hide (0 = visible / never hidden)
        bool dormant = false;           // hibernated: no View until shown again
        bool domReady = false;          // main frame reached DOM ready since its last load
        bool fullscreen = false;    // w/h track the backbuffer (0,0 at CreateView)
        // Ultralight device scale: CSS px -> view px. The page lays out and
        // rasterizes at this scale (real DPI — sharp), instead of a consumer
        // CSS transform that resamples a 1x raster (soft). 1 = 100%.
        float deviceScale = 1.0f;
        // View::needs_paint() sampled BEFORE Renderer::Render() clears it —
        // "this view's pixels change this frame". The VR submitter uses it to
        // skip a copy + compositor upload the runtime does not need.
        bool repaintedThisFrame = true;
        bool boundsDirty = false;   // render thread applies Resize
        bool scaleDirty = false;    // render thread applies set_device_scale (0.28.5)
        bool shimsDirty = false;    // render thread re-installs bridge shims
        DomReadyFn onDomReady = nullptr;
        struct Listener { JsListenerFn fn = nullptr; JsListenerExFn fnEx = nullptr; void* user = nullptr; };
        std::map<std::string, Listener> listeners;
        std::vector<OutboundCall> outbound;
        std::vector<PendingEval> evals;     // EvalJS requests; drained with outbound
        // v4: z-order = (layer, order); lifecycle intents the render thread applies.
        int layer = 1;
        std::uint64_t order = 0;
        bool escapeCapture = false;   // 0.28.0: the page owns Escape (delivered as a key, no UI-mode exit)
        NetLevel netLevel = NetLevel::File;   // the owning mod's NetworkPolicy; File when no mod owns the view
        int  scrollStep = 40;         // 0.28.0: px per wheel notch
        std::string soundOpen, soundClose;   // 0.29.0: host-played on UI-mode enter/exit for this view ("" = none)
        bool destroyPending = false;
        bool reloadPending = false;
        bool reloadedFlag = false;      // tag the next DOM-ready as a ViewReloaded
        std::string navigateUrl;        // non-empty = LoadURL next frame
        std::string sessionName;        // "" = default session; else a per-mod persistent session
        // Render-thread-owned:
        ultralight::RefPtr<ultralight::View> ul;
        ID3D11Texture2D* tex = nullptr;             // CPU-surface path only
        ID3D11ShaderResourceView* srv = nullptr;
        int texW = 0, texH = 0;
    };
    static std::mutex s_viewsMutex;
    static std::vector<std::unique_ptr<MlView>> s_views;  // z-order = (layer, order), see SortViewsLocked
    static std::atomic<std::uint64_t> s_nextViewId{ 1 };
    static std::atomic<std::uint64_t> s_uiModeView{ 0 };  // UI mode's show/focus target
    // The view the TOGGLE KEY opens — set only by SetUIModeView (the host's
    // own/demo UI). EnterUIMode deliberately does NOT set it: a consumer's
    // (SA's) view must never be re-opened by our key behind the consumer's
    // open bookkeeping — the key stays exit-only for consumer views.
    static std::atomic<std::uint64_t> s_toggleView{ 0 };
    static std::atomic<std::uint64_t> s_nextViewOrder{ 1 };
    static std::atomic<std::uint64_t> s_inspectorRequest{ 0 };   // inspector toggle request: page (or inspector) view id; render thread consumes (no key bound yet)
    static std::map<ViewId, ViewId> s_inspectorOf;               // page -> hosted inspector view (render thread only)
    static std::atomic<std::uint64_t> s_inspectorVisibleFor{ 0 };   // page whose inspector is showing (0 = none); any thread reads
    static std::atomic<std::uint64_t> s_inspectorHideRequest{ 0 }; // 0.28.0: page whose inspector should hide; render thread
    static std::atomic<HostEventFn> s_hostEventSink{ nullptr };

    static void Emit(HostEvent type, ViewId view, int x = 0, int y = 0, const char* detail = "")
    {
        if (HostEventFn fn = s_hostEventSink.load()) fn(type, view, x, y, detail ? detail : "");
    }

    // s_viewsMutex must be held. Keeps the registry in z-order.
    static void SortViewsLocked()
    {
        std::stable_sort(s_views.begin(), s_views.end(),
            [](const std::unique_ptr<MlView>& a, const std::unique_ptr<MlView>& b) {
                return a->layer != b->layer ? a->layer < b->layer : a->order < b->order;
            });
    }

    // s_viewsMutex must be held.
    static MlView* FindViewLocked(ViewId id)
    {
        for (auto& v : s_views)
            if (v->id == id) return v.get();
        return nullptr;
    }
    static MlView* FindViewByUlLocked(ultralight::View* ul)
    {
        for (auto& v : s_views)
            if (v->ul.get() == ul) return v.get();
        return nullptr;
    }

    // ── Texture-backed image registry (Ultralight ImageSource) ──────────────
    // Registered from ANY thread (RegisterTextureImage queues intent); the
    // render thread materializes (driver id + ImageSource + provider entry)
    // and drains invalidations in FrameWork, exactly like the view registry.
    // Entries are never erased (an unregistered image is tombstoned) so
    // ImageUrl's pointer and the driver id stay valid for pages that still
    // reference them.
    struct MlImage {
        ImageId id = 0;
        std::string name;
        std::uint32_t w = 0, h = 0;
        ID3D11ShaderResourceView* srv = nullptr;         // our own AddRef'd ref
        ID3D11ShaderResourceView* pendingSrv = nullptr;  // UpdateTextureImage, applied on the render thread
        bool dirty = false;
        bool unregister = false;
        bool removed = false;
        std::string url;
        // Render-thread-owned:
        std::uint32_t texId = 0;
        ultralight::RefPtr<ultralight::ImageSource> src;
    };
    static std::mutex s_imagesMutex;
    static std::vector<std::unique_ptr<MlImage>> s_images;
    static std::atomic<std::uint32_t> s_nextImageId{ 1 };

    static MlImage* FindImageLocked(ImageId id)
    {
        for (auto& i : s_images)
            if (i->id == id) return i.get();
        return nullptr;
    }

    // Resolve the anchored position against the current backbuffer size.
    static void EffectivePos(const MlView& v, float bw, float bh, float& outX, float& outY)
    {
        outX = (v.x >= 0) ? static_cast<float>(v.x) : bw + static_cast<float>(v.x);
        outY = (v.y >= 0) ? static_cast<float>(v.y) : bh + static_cast<float>(v.y);
    }

    // True when any view wants pixels this frame.
    static bool AnyViewVisible()
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        for (const auto& v : s_views)
            if (v->visible) return true;
        return false;
    }

    // The cursor the page under (x, y) asks for: the topmost visible interactive view there, else the UI-mode view
    // (DrainInputQueue's hit order, so it is the page the mouse events go to).
    static PageCursor PageCursorAt(int x, int y, float bw, float bh)
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        for (auto it = s_views.rbegin(); it != s_views.rend(); ++it) {
            const MlView& v = **it;
            if (!v.ul || !v.visible || v.clickThrough) continue;
            float ex = 0, ey = 0;
            EffectivePos(v, bw, bh, ex, ey);
            if (x >= ex && y >= ey && x < ex + v.w && y < ey + v.h) return static_cast<PageCursor>(v.pageCursor);
        }
        if (const MlView* u = FindViewLocked(static_cast<ViewId>(s_uiModeView.load())))
            return static_cast<PageCursor>(u->pageCursor);
        return PageCursor::Arrow;
    }

    // A visible view the player can click: the UI-pass carrier opens only for one of these (see
    // MagelightOverlayMenu), so a HUD view that stays up never holds it open over gameplay.
    static bool AnyInteractiveViewVisible()
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        for (const auto& v : s_views)
            if (v->visible && !v->clickThrough) return true;
        return false;
    }

    static std::string JSStringToStd(JSStringRef s)
    {
        if (!s) return {};
        const size_t cap = JSStringGetMaximumUTF8CStringSize(s);
        std::string out(cap, '\0');
        const size_t n = JSStringGetUTF8CString(s, out.data(), cap);
        out.resize(n ? n - 1 : 0);
        return out;
    }

    static std::string JsonEscape(const std::string& s)
    {
        std::string out;
        out.reserve(s.size() + 8);
        for (const char c : s) {
            switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04X", c);
                    out += buf;
                } else {
                    out += c;
                }
            }
        }
        return out;
    }

    // Render thread (JS execution context). Handlers fire HERE — game-state
    // work must marshal via GameTask::Post (MagelightGameTask.h).
    // Args: (viewIdString, listenerName, payload).
    // ── Our own virtual keyboard (0.20.0) ───────────────────────────────
    // A host page (views/keyboard) the VR laser clicks, raised when a text
    // field in ANOTHER view takes focus. It never takes UI mode, so the page
    // being typed into keeps key focus and the keys we inject land there —
    // DrainInputQueue routes keys to the UI-mode view, and the laser's clicks
    // hit-test to whichever view is under the ray. Runtime-independent: no
    // ShowKeyboardForOverlay (it steals input focus), no export lookup.
    static std::atomic<std::uint64_t> s_kbViewId{ 0 };
    // CreateView / SetViewLayer / ShowView / GetDisplaySize are the public
    // namespace functions declared in Magelight.h — do NOT redeclare them
    // static here, that gives them internal linkage and breaks every other
    // translation unit that calls them. Only the file-local queue needs a fwd.
    static void QueueInput(UINT msg, WPARAM w, LPARAM l);

    static void EnsureKeyboardView()
    {
        if (s_kbViewId.load()) return;
        int bw = 0, bh = 0;
        GetDisplaySize(bw, bh);
        constexpr int kKbW = 980, kKbH = 330;
        const int x = (bw > kKbW + 40) ? (bw - kKbW) / 2 : 20;
        const int y = (bh > kKbH + 100) ? (bh - kKbH - 70) : 20;
        const ViewId id = CreateView("views/keyboard/index.html", x, y, kKbW, kKbH, nullptr, false, false);
        if (!id) return;
        SetViewLayer(id, 3);   // System: above the panel it serves
        s_kbViewId.store(id);
        VR::Placement p;
        // Placed, not following — same reasoning as the page layer. Left at
        // the host default so the vr.panelFollow setting covers it too.
        p.mode = VR::DefaultPanelMode();
        // In FRONT of the page, not level with it. The panel layer sits at
        // panelDistanceM (1.2 m by default) — at the keyboard's old 1.1 m the
        // two quads were 10 cm apart with overlapping vertical extents, which
        // sort order hides but which reads as one surface clipping another.
        // 0.9 m keeps it plainly nearer than the page it serves and inside
        // easy pointing reach.
        p.distanceMeters = 0.9f;
        p.widthMeters = 1.0f;
        p.heightOffset = -0.42f;  // below the panel, angled into reach
        VR::SetPlacement(id, p);
        SKSE::log::info("Magelight: own keyboard view {} created ({}x{} at {},{})", id, kKbW, kKbH, x, y);
    }

    static void ShowOwnKeyboard(bool on)
    {
        if (on) {
            if (!VR::OwnKeyboardEnabled()) return;   // flat types on the real keyboard
            if (!IsUIModeActive()) return;            // nothing is open to type into
            EnsureKeyboardView();
        }
        if (const ViewId id = static_cast<ViewId>(s_kbViewId.load())) ShowView(id, on);
    }

    // A key from that page: {"c":"a"} a character, {"vk":8} a virtual key,
    // {"close":true} dismiss. Queued exactly like a typed key, so it reaches
    // the focused page through the ordinary path.
    static void OnOwnKeyboardKey(const std::string& json)
    {
        try {
            const auto j = nlohmann::json::parse(json);
            if (j.value("close", false)) { ShowOwnKeyboard(false); return; }
            if (auto d = j.find("drag"); d != j.end() && d->is_string()) {
                const std::string what = d->get<std::string>();
                const ViewId kb = static_cast<ViewId>(s_kbViewId.load());
                if (what == "start") VR::BeginViewDrag(kb);
                else VR::EndViewDrag();
                return;
            }
            if (auto it = j.find("vk"); it != j.end() && it->is_number_integer()) {
                const int vk = it->get<int>();
                if (vk <= 0 || vk > 0xFF) return;
                const LPARAM scan = static_cast<LPARAM>(
                    MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC) & 0xFF) << 16;
                QueueInput(WM_KEYDOWN, static_cast<WPARAM>(vk), scan);
                if (vk == VK_RETURN || vk == VK_TAB)   // these also produce text
                    QueueInput(WM_CHAR, static_cast<WPARAM>(vk == VK_RETURN ? L'\r' : L'\t'), 0);
                QueueInput(WM_KEYUP, static_cast<WPARAM>(vk), scan | (1 << 30) | (1 << 31));
                return;
            }
            if (auto it = j.find("c"); it != j.end() && it->is_string()) {
                const std::string c = it->get<std::string>();
                wchar_t wide[8]{};
                const int n = MultiByteToWideChar(CP_UTF8, 0, c.c_str(), -1, wide, 7);
                for (int i = 0; i < n && wide[i]; ++i)
                    QueueInput(WM_CHAR, static_cast<WPARAM>(wide[i]), 0);
            }
        } catch (...) {
            SKSE::log::warn("Magelight: unreadable key payload from the keyboard page");
        }
    }

    // IME (0.27.0) — bodies live after the window statics they touch.
    static void ImeOnCaret(ViewId id, const std::string& arg);
    static void ImeOnTextFocus(bool focused);

    static void PlayPageSound(ViewId id, const std::string& arg);   // fwd (defined after the dispatcher)

    // `function` is the page's own __mlNative, which carries its view id as
    // private data (InstallBridgeShims). The id a page passes is only checked
    // against it: trusting it would let any page fire another view's
    // listeners, sounds or keys as that view.
    static JSValueRef MlNativeDispatch(JSContextRef ctx, JSObjectRef function, JSObjectRef,
        size_t argc, const JSValueRef argv[], JSValueRef*)
    {
        auto argToStr = [&](size_t i) -> std::string {
            if (argc <= i) return {};
            JSStringRef js = JSValueToStringCopy(ctx, argv[i], nullptr);
            if (!js) return {};
            std::string out = JSStringToStd(js);
            JSStringRelease(js);
            return out;
        };
        const std::string idStr = argToStr(0);
        const std::string name = argToStr(1);
        const std::string arg = argToStr(2);
        const ViewId id = static_cast<ViewId>(reinterpret_cast<std::uintptr_t>(JSObjectGetPrivate(function)));
        if (!id || std::strtoull(idStr.c_str(), nullptr, 10) != id) {
            static std::atomic<int> s_forgedLogged{ 0 };
            if (s_forgedLogged.fetch_add(1) < 32) {
                SKSE::log::warn("Magelight: view {} called '{}' as view '{}' — refused (a page acts only as itself)",
                    id, name, idStr);
            }
            return JSValueMakeUndefined(ctx);
        }

        // Reserved channels: the '__' prefix is the host's, so none of these
        // ever reaches a mod's listeners.
        if (name == "__escapecapture") {   // 0.28.0: the page owns Escape ('1') or hands it back ('0')
            SetViewEscapeCapture(id, arg == "1");
            return JSValueMakeUndefined(ctx);
        }
        // The IME, the caret and the host keyboard serve the page the user is
        // in: a background page's focus change, blur or caret must not
        // attach, cancel or move them under it, nor hide the keyboard it raised.
        const bool fromUiView = IsUIModeActive() && id == static_cast<ViewId>(s_uiModeView.load()) &&
                                id != static_cast<ViewId>(s_kbViewId.load());
        if (name == "__imecaret") {   // IME (0.27.0): the page's caret -> candidate placement
            if (fromUiView) ImeOnCaret(id, arg);
            return JSValueMakeUndefined(ctx);
        }
        if (name == "__textfocus") {
            const bool focused = (arg == "1");
            // Only the UI-mode view claims the runtime keyboard (typed keys go to it); a
            // blur from any view clears only that view's own claim.
            if (!focused || fromUiView) VR::NoteTextFocus(id, focused);
            if (fromUiView) {
                ImeOnTextFocus(focused);   // IME (0.27.0)
                ShowOwnKeyboard(focused);
            }
            return JSValueMakeUndefined(ctx);
        }
        if (name == "__key") {   // the host's own keyboard page only: the keys go to the UI-mode view
            if (id == static_cast<ViewId>(s_kbViewId.load())) OnOwnKeyboardKey(arg);
            return JSValueMakeUndefined(ctx);
        }
        if (name == "__sound") {   // 0.29.0: page-driven UI sound; "hover:<name>" is throttled
            PlayPageSound(id, arg);
            return JSValueMakeUndefined(ctx);
        }

        MlView::Listener l;
        {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            if (MlView* v = FindViewLocked(id)) {
                auto it = v->listeners.find(name);
                if (it != v->listeners.end()) l = it->second;
            }
        }
        if (l.fnEx) l.fnEx(id, arg.c_str(), l.user);
        else if (l.fn) l.fn(arg.c_str());
        else SKSE::log::warn("Magelight: JS called unregistered listener '{}' (view {})", name, id);
        return JSValueMakeUndefined(ctx);
    }

    // 0.29.0, JS thread: validate here, play on the game thread.
    // Only a VISIBLE view may play — a hidden or hibernated page that fires a
    // sound on load must not be heard (the hazard the '__textfocus' gate
    // closed for the keyboard). 'hover:<name>' is throttled to one per ~60 ms
    // per view so a mouse sweep over a list cannot spam the engine.
    static void PlayPageSound(ViewId id, const std::string& arg)
    {
        std::string name = arg;
        bool hover = false;
        if (name.rfind("hover:", 0) == 0) { hover = true; name.erase(0, 6); }
        if (name.empty() || name.size() > 128) return;
        {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            MlView* v = FindViewLocked(id);
            if (!v || !v->visible || v->dormant) return;
        }
        if (hover && !Sound::HoverAllowed(id)) return;
        GameTask::Post([id, name]() { Sound::Play(name, id); });
    }

    // Render thread. full (OnWindowObjectReady, a fresh window object): the
    // native dispatcher, the page core (__MAGELIGHT__, window.magelight and
    // the document listeners) and one shim per listener. !full (shimsDirty, a
    // listener registered after load): a fresh __mlNative and every listener
    // shim, early ones included; the page keeps its window.magelight and every
    // subscription on it. The core also guards itself with window.__mlCore:
    // it runs once per window, never twice.
    static void InstallBridgeShims(MlView& v, bool full)
    {
        if (!v.ul) return;
        auto ctxScope = v.ul->LockJSContext();
        JSContextRef ctx = (*ctxScope);
        JSObjectRef global = JSContextGetGlobalObject(ctx);

        // A callable object of a class, not a plain function: only a class
        // instance can carry private data, and that is how the dispatcher
        // knows which page called it.
        static const JSClassRef s_nativeClass = [] {
            JSClassDefinition def{};   // zeroed, the same as kJSClassDefinitionEmpty
            def.className = "MagelightNative";
            def.callAsFunction = &MlNativeDispatch;
            return JSClassCreate(&def);
        }();
        JSStringRef dispName = JSStringCreateWithUTF8CString("__mlNative");
        JSObjectRef fnObj = JSObjectMake(ctx, s_nativeClass,
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(v.id)));
        // A class instance inherits Object.prototype: without Function.prototype
        // the SDK's typed-as-a-function __mlNative has no call/apply/bind.
        {
            JSStringRef fnName = JSStringCreateWithUTF8CString("Function");
            JSStringRef protoName = JSStringCreateWithUTF8CString("prototype");
            const JSValueRef ctor = JSObjectGetProperty(ctx, global, fnName, nullptr);
            if (ctor && JSValueIsObject(ctx, ctor)) {
                const JSValueRef proto = JSObjectGetProperty(ctx, JSValueToObject(ctx, ctor, nullptr), protoName, nullptr);
                if (proto && JSValueIsObject(ctx, proto)) JSObjectSetPrototype(ctx, fnObj, proto);
            }
            JSStringRelease(protoName);
            JSStringRelease(fnName);
        }
        JSObjectSetProperty(ctx, global, dispName, fnObj, kJSPropertyAttributeNone, nullptr);
        JSStringRelease(dispName);

        // Host marker FIRST — pages detect which host they run on at their
        // first line (runtime host detection for the SA frontend; the prior
        // host installed no page global, so presence == Magelight).
        // __MAGELIGHT__ = who am I / what can the host do; magelight = the two
        // verbs every page gets (send/on/off) with pre-subscribe buffering,
        // so a payload that lands before the app mounts is replayed, not
        // lost. InteropCall to a channel with no window shim routes into
        // _dispatch (see DrainBridgeQueues) — plain HTML and the SDK share it.
        const std::string idStr0 = std::to_string(v.id);
        std::string shims;
        if (full) shims =
            "if(!window.__mlCore){Object.defineProperty(window,'__mlCore',{value:true});"
            "window.__MAGELIGHT__={version:'" PLUGIN_VERSION "',versionNumber:" +
            std::to_string(PLUGIN_VERSION_MAJOR * 10000 + PLUGIN_VERSION_MINOR * 100 + PLUGIN_VERSION_PATCH) +
            ",viewId:" + idStr0 + "," + Api4::ViewIdentityJson(v.id) +
            ",capabilities:" + Api4::CapabilitiesJson() +
            ",dev:" + (s_devMode.load() ? "true" : "false") +
            ",runtimeUrl:" + MlJsonQuote(PathToFileUrl(s_runtimeDir) + "/") + "};"
            "window.magelight=(function(){var subs={},buf={},id='" + idStr0 + "';"
            "function dispatch(ch,arg){var s=subs[ch];if(!s||!s.length){(buf[ch]=buf[ch]||[]).push(arg);return;}"
            "for(var i=0;i<s.length;i++){try{s[i](arg);}catch(e){console.error('[magelight] handler for '+ch+' threw',e);}}}"
            "return{send:function(ch,p){var a=p==null?'':(typeof p==='string'?p:JSON.stringify(p));__mlNative(id,ch,a);},"
            // 0.29.0: magelight.sound('click') -> the reserved '__sound' channel (host plays it through the game's audio)
            "sound:function(n){try{__mlNative(id,'__sound',n==null?'':String(n));}catch(e){}},"
            "on:function(ch,fn){(subs[ch]=subs[ch]||[]).push(fn);var b=buf[ch];if(b){delete buf[ch];for(var i=0;i<b.length;i++){try{fn(b[i]);}catch(e){console.error('[magelight] replay for '+ch+' threw',e);}}}"
            "return function(){window.magelight.off(ch,fn);};},"
            "off:function(ch,fn){var s=subs[ch];if(!s)return;var i=s.indexOf(fn);if(i>=0)s.splice(i,1);},"
            "_dispatch:dispatch,pending:function(ch){return (buf[ch]||[]).length;}};})();"
            // Text-field focus -> the reserved '__textfocus' channel. The VR
            // presenter raises the runtime's virtual keyboard on it (there is
            // no keyboard in a headset), and it costs a flat host nothing.
            "(function(){function isText(e){if(!e)return false;if(e.isContentEditable)return true;"
            "var t=(e.tagName||'').toUpperCase();if(t==='TEXTAREA')return true;if(t!=='INPUT')return false;"
            "var y=(e.type||'text').toLowerCase();"
            "return ['text','search','url','tel','email','password','number'].indexOf(y)>=0;}"
            "window.__mlIsText=isText;"   // shared with the IME block below (one predicate)
            "function send(f){try{__mlNative('" + idStr0 + "','__textfocus',f?'1':'0');}catch(e){}}"
            "document.addEventListener('focusin',function(ev){if(isText(ev.target))send(true);},true);"
            "document.addEventListener('focusout',function(){setTimeout(function(){send(isText(document.activeElement));},0);},true);"
            "if(isText(document.activeElement))send(true);})();"
            // UI sounds (0.29.0): the markup convention. data-ml-sound="click"
            // plays on click (keyboard activation raises click too), data-ml-sound-hover
            // ="focus" on entering the element. Delegated on document so a manifest-only
            // mod gets button sounds from one attribute; 'mouseover' (bubbles) with a
            // last-element latch, since 'mouseenter' does not bubble. The host throttles
            // hover per view.
            "(function(){var id='" + idStr0 + "',last=null;"
            "function up(e,a){while(e&&e.getAttribute){if(e.hasAttribute(a))return e;e=e.parentNode;}return null;}"
            "document.addEventListener('click',function(ev){var t=up(ev.target,'data-ml-sound');if(!t)return;"
            // A <label> wrapping its control re-dispatches the click on the control: play that one only.
            "if(t.control&&!t.control.disabled&&!t.control.contains(ev.target)&&t.contains(t.control))return;"
            "var n=t.getAttribute('data-ml-sound');if(n)try{__mlNative(id,'__sound',n);}catch(e){}},true);"
            "document.addEventListener('mouseover',function(ev){var t=up(ev.target,'data-ml-sound-hover');if(t===last)return;last=t;if(!t)return;"
            "var n=t.getAttribute('data-ml-sound-hover');if(n)try{__mlNative(id,'__sound','hover:'+n);}catch(e){}},true);"
            "})();"
            // IME (0.27.0): caret reporting ('__imecaret', CSS px), the inline pre-edit
            // (the composition rides as the field's SELECTION so the commit — ordinary
            // characters — replaces it and a cancel deletes it) and a 'magelight:ime'
            // window event for pages that want an indicator.
            "(function(){var id='" + idStr0 + "',ime={el:null,base:null,pre:'',start:0,end:0};"
            "var isText=window.__mlIsText;"
            "function caret(){var e=document.activeElement;if(!isText(e))return;var r=e.getBoundingClientRect(),x=r.left,y=r.top,h=r.height;"
            "try{if(e.isContentEditable){var s=window.getSelection();if(s&&s.rangeCount){var rr=s.getRangeAt(0).getBoundingClientRect();"
            "if(rr.left||rr.top){x=rr.left;y=rr.top;h=rr.height||h;}}}"
            "else{var cs=getComputedStyle(e),cv=document.createElement('canvas').getContext('2d');cv.font=cs.font;"
            "var v=e.value||'',p=e.selectionStart||0,line=v.slice(0,p),nl=line.lastIndexOf('\\n');if(nl>=0)line=line.slice(nl+1);"
            "var lh=parseFloat(cs.lineHeight);if(!(lh>0))lh=(parseFloat(cs.fontSize)||14)*1.3;h=lh;"
            "x=r.left+(parseFloat(cs.paddingLeft)||0)+(parseFloat(cs.borderLeftWidth)||0)+cv.measureText(line).width-(e.scrollLeft||0);"
            "if(e.tagName.toUpperCase()==='TEXTAREA'){var ln=v.slice(0,p).split('\\n').length;y=r.top+(parseFloat(cs.paddingTop)||0)+(ln-1)*lh-(e.scrollTop||0);}"
            "else{y=r.top+(r.height-lh)/2;}}}catch(err){}"
            "try{__mlNative(id,'__imecaret',Math.round(x)+','+Math.round(y)+','+Math.round(h));}catch(err){}}"
            "document.addEventListener('focusin',function(ev){if(isText(ev.target))setTimeout(caret,0);},true);"
            "document.addEventListener('input',function(){caret();},true);"
            "document.addEventListener('selectionchange',function(){caret();});"
            "document.addEventListener('click',function(){setTimeout(caret,0);},true);"
            "function sel(e,a,b){try{e.setSelectionRange(a,b);}catch(x){}}"
            "function preeditStanding(e){return ime.el===e&&ime.base!=null&&e.value===ime.base.slice(0,ime.start)+ime.pre+ime.base.slice(ime.end)"
            "&&e.selectionStart===ime.start&&e.selectionEnd===ime.start+ime.pre.length;}"
            "function clearPreedit(e){if(preeditStanding(e)){e.value=ime.base.slice(0,ime.start)+ime.base.slice(ime.end);sel(e,ime.start,ime.start);}}"
            "if(window.magelight){window.magelight.on('__ime',function(a){var m;try{m=JSON.parse(a);}catch(e){return;}"
            "var e=document.activeElement,t=(e&&e.tagName||'').toUpperCase(),plain=!!e&&(t==='INPUT'||t==='TEXTAREA')&&!e.isContentEditable;"
            "if(m.state==='start'){ime.pre='';if(plain){ime.el=e;ime.base=e.value;ime.start=e.selectionStart||0;ime.end=e.selectionEnd||ime.start;}else{ime.el=null;ime.base=null;}}"
            "else if(m.state==='compose'){if(plain&&ime.el===e&&ime.base!=null){ime.pre=m.text||'';e.value=ime.base.slice(0,ime.start)+ime.pre+ime.base.slice(ime.end);sel(e,ime.start,ime.start+ime.pre.length);}caret();}"
            "else if(m.state==='commit'){if(plain)clearPreedit(e);ime.base=null;ime.pre='';}"
            "else if(m.state==='end'){if(plain)clearPreedit(e);ime.el=null;ime.base=null;ime.pre='';}"
            "try{window.dispatchEvent(new CustomEvent('magelight:ime',{detail:m}));}catch(x){}});}"
            "})();"
            "}";
        std::size_t count = 0;
        {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            const std::string idStr = idStr0;
            for (const auto& [n, l] : v.listeners) {
                (void)l;
                // JsonQuote: the listener name is mod-supplied — a quote in it
                // must not break (or break out of) the generated script.
                shims += "window[" + JsonQuote(n) + "]=function(a){__mlNative('" + idStr +
                         "'," + JsonQuote(n) + ",a==null?'':String(a));};";
            }
            count = v.listeners.size();
            v.shimsDirty = false;  // this install covers everything registered so far
        }
        v.ul->EvaluateScript(shims.c_str());
        if (full) SKSE::log::info("Magelight: bridge installed on view {} ({} JS listeners)", v.id, count);
        else SKSE::log::info("Magelight: {} JS listener shim(s) refreshed on view {}", count, v.id);
    }

    class MlLoadListener final : public ultralight::LoadListener {
    public:
        // NOTE: no OnBeginLoading override. Ultralight fires it SYNCHRONOUSLY
        // inside LoadURL, which MaterializeViews calls while holding
        // s_viewsMutex — taking the lock there is a self-deadlock that the
        // SEH guard turned into a dead overlay (0.16.0 field log). The
        // domReady flag is cleared where loads are REQUESTED instead
        // (MaterializeViews, ReloadView, NavigateView).
        void OnWindowObjectReady(ultralight::View* caller, uint64_t, bool is_main_frame,
            const ultralight::String&) override
        {
            if (!is_main_frame) return;
            MlView* v = nullptr;
            {
                std::lock_guard<std::mutex> lk(s_viewsMutex);
                v = FindViewByUlLocked(caller);
            }
            if (v) InstallBridgeShims(*v, true);
        }
        void OnDOMReady(ultralight::View* caller, uint64_t, bool is_main_frame,
            const ultralight::String&) override
        {
            if (!is_main_frame) return;
            ViewId id = 0;
            DomReadyFn cb = nullptr;
            {
                std::lock_guard<std::mutex> lk(s_viewsMutex);
                if (MlView* v = FindViewByUlLocked(caller)) {
                    id = v->id;
                    cb = v->onDomReady;
                    v->domReady = true;   // outbound calls held during the load flow now
                }
            }
            SKSE::log::info("Magelight: DOM ready (view {})", id);
            if (cb) cb(id);
            Emit(HostEvent::ViewDomReady, id);
            bool reloaded = false;
            {
                std::lock_guard<std::mutex> lk(s_viewsMutex);
                if (MlView* v = FindViewLocked(id)) { reloaded = v->reloadedFlag; v->reloadedFlag = false; }
            }
            if (reloaded) Emit(HostEvent::ViewReloaded, id);
        }
        void OnFailLoading(ultralight::View* caller, uint64_t, bool is_main_frame,
            const ultralight::String& url, const ultralight::String& description,
            const ultralight::String& error_domain, int error_code) override
        {
            if (!is_main_frame) return;
            ViewId id = 0;
            {
                std::lock_guard<std::mutex> lk(s_viewsMutex);
                if (MlView* v = FindViewByUlLocked(caller)) id = v->id;
            }
            const std::string detail = std::string(url.utf8().data()) + ": " + description.utf8().data() +
                " (" + error_domain.utf8().data() + ":" + std::to_string(error_code) + ")";
            SKSE::log::error("Magelight: view {} FAILED to load - {}", id, detail);
            Emit(HostEvent::ViewLoadFailed, id, error_code, 0, detail.c_str());
        }
    };
    static MlLoadListener s_loadListener;

    // Render thread, once per frame: deliver queued C++ -> JS calls per view.
    static void DrainBridgeQueues()
    {
        // Snapshot (view, calls) pairs under the lock; deliver outside it.
        std::vector<std::pair<ultralight::RefPtr<ultralight::View>, std::vector<OutboundCall>>> work;
        struct EvalWork { ultralight::RefPtr<ultralight::View> ul; ViewId id; std::vector<PendingEval> evals; };
        std::vector<EvalWork> evalWork;
        {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            for (auto& v : s_views) {
                if (!v->ul || !v->domReady) continue;   // held until the page can take them
                if (!v->outbound.empty()) {
                    work.emplace_back(v->ul, std::move(v->outbound));
                    v->outbound.clear();
                }
                if (!v->evals.empty()) {
                    evalWork.push_back(EvalWork{ v->ul, v->id, std::move(v->evals) });
                    v->evals.clear();
                }
            }
        }
        for (auto& ew : evalWork) {
            for (const auto& e : ew.evals) {
                ultralight::String exc;
                const ultralight::String result = ew.ul->EvaluateScript(e.script.c_str(), &exc);
                if (e.fn) e.fn(ew.id, result.utf8().data(), exc.utf8().data(), e.user);
            }
        }
        for (auto& [ul, calls] : work) {
            auto ctxScope = ul->LockJSContext();
            JSContextRef ctx = (*ctxScope);
            JSObjectRef global = JSContextGetGlobalObject(ctx);
            for (const auto& c : calls) {
                if (c.raw) {
                    ul->EvaluateScript(c.fn.c_str());  // fn carries the script
                    continue;
                }
                JSStringRef fnName = JSStringCreateWithUTF8CString(c.fn.c_str());
                JSValueRef fnVal = JSObjectGetProperty(ctx, global, fnName, nullptr);
                JSStringRelease(fnName);
                JSObjectRef fnObj = (fnVal && JSValueIsObject(ctx, fnVal))
                    ? JSValueToObject(ctx, fnVal, nullptr) : nullptr;
                if (!fnObj || !JSObjectIsFunction(ctx, fnObj)) {
                    // No window shim for this channel: hand it to the page
                    // core, which buffers until someone subscribes.
                    ul->EvaluateScript(("window.magelight&&window.magelight._dispatch(" + MlJsonQuote(c.fn) + "," +
                                        MlJsonQuote(c.arg) + ");").c_str());
                    continue;
                }
                JSStringRef argStr = JSStringCreateWithUTF8CString(c.arg.c_str());
                JSValueRef argVal = JSValueMakeString(ctx, argStr);
                JSStringRelease(argStr);
                JSValueRef exc = nullptr;
                JSObjectCallAsFunction(ctx, fnObj, nullptr, 1, &argVal, &exc);
                if (exc) SKSE::log::warn("Magelight: InteropCall('{}') threw in JS", c.fn);
            }
        }
    }

    // ── Milestone-4 demo handlers (the playground page) ─────────────────────
    // Prove both directions plus the game-thread marshalling pattern the real
    // bridge will use for every SeverActions page.
    static ViewId s_playgroundId = 0;
    static ViewId s_badgeId = 0;
    static ViewId s_probeViewId = 0;

    static void OnRequestPageData(const char* arg)
    {
        SKSE::log::info("Magelight: requestPageData('{}') — gathering on game thread",
            arg ? arg : "");
        GameTask::Post([]() {
            std::string name = "?", loc;
            int gold = 0, level = 0, hp = 0, hpMax = 0;
            if (auto* pc = RE::PlayerCharacter::GetSingleton()) {
                if (const char* n = pc->GetDisplayFullName()) name = n;
                gold = pc->GetGoldAmount();
                level = static_cast<int>(pc->GetLevel());
                if (auto* avo = pc->AsActorValueOwner()) {
                    hp = static_cast<int>(avo->GetActorValue(RE::ActorValue::kHealth));
                    hpMax = static_cast<int>(avo->GetPermanentActorValue(RE::ActorValue::kHealth));
                }
                if (auto* l = pc->GetCurrentLocation()) {
                    if (l->GetName()) loc = l->GetName();
                }
                if (loc.empty()) {
                    if (auto* cell = pc->GetParentCell()) {
                        if (cell->GetFullName()) loc = cell->GetFullName();
                    }
                }
            }
            const std::string json =
                "{\"name\":\"" + JsonEscape(name) +
                "\",\"level\":" + std::to_string(level) +
                ",\"gold\":" + std::to_string(gold) +
                ",\"health\":" + std::to_string(hp) +
                ",\"healthMax\":" + std::to_string(hpMax) +
                ",\"location\":\"" + JsonEscape(loc) + "\"}";
            InteropCall(s_playgroundId, "receivePageData", json);
        });
    }

    static void OnSettingChanged(const char* arg)
    {
        // Render-thread handler answering directly — the synchronous pattern.
        const std::string payload = arg ? arg : "";
        SKSE::log::info("Magelight: settingChanged: {}", payload);
        InteropCall(s_playgroundId, "showNotification",
            "C++ received setting payload (" + std::to_string(payload.size()) + " bytes)");
    }

    // ── UI mode + input routing (milestone 3) ───────────────────────────────
    // The toggle key (Page Up by default) toggles UI MODE: overlay shown +
    // focused, vanilla cursor up, the game's own controls suspended
    // (ToggleControls with storeState=false so NOTHING persists into saves —
    // the classic stuck-controls trap). A
    // WndProc hook on the game window captures real Windows messages (the only
    // way to get properly translated WM_CHAR text) and queues them; the render
    // thread — the only thread allowed to touch Ultralight — drains the queue
    // into the view each frame. ESC exits UI mode.
    struct InputMsg {
        UINT msg;       // WM_*; 0 = control marker (wparam 1 = focus, 0 = unfocus)
        WPARAM wparam;
        LPARAM lparam;
    };
    static HWND                  s_hwnd = nullptr;
    static WNDPROC               s_origWndProc = nullptr;
    static std::atomic<WNDPROC>  s_ansiShimPrev{ nullptr };   // HookWndProc as an A-callable value (AnsiShimWndProc)
    static std::atomic<bool>     s_focused{ false };
    static std::mutex            s_inputMutex;
    static std::vector<InputMsg> s_inputQueue;
    // The SA-port close hook (Magelight.h): fired on the game thread on
    // every UI-mode exit — the host's own menu-close notification.
    // MULTICAST (0.9.1): every embedding plugin registers its own hook and
    // hears every exit with the exiting view, filtering by the views it
    // owns. The 0.9.0 single slot silently replaced the previous
    // registrant the moment a second consumer loaded.
    static std::mutex                s_uiModeExitMutex;
    static std::vector<UIModeExitFn> s_uiModeExitCbs;

    // ── Our own engine menu ─────────────────────────────────────────────
    // Pushed on UI-mode entry, popped on exit. Owning a real IMenu is what
    // makes the engine cooperate with an overlay: the engine drives
    // MenuCursor only while a kUsesCursor menu is topmost (field-found 2026-
    // 09-01 — SA's item-carrier menu froze the cursor the moment it became
    // the only real menu on the stack), kUsesMenuContext puts input into the
    // menu context natively (gameplay controls off, gamepad included), the
    // engine's Cancel user event (Escape / gamepad B) lands in ProcessMessage,
    // MenuOpenCloseEvent fires for anyone sinking menu state, and
    // kPausesGame is a per-open flag — pause-on-open for free. We still draw
    // our own cursor sprite (the engine's is a pre-Present Scaleform menu,
    // always under our overlay) and keep the delta-integration cursor
    // fallback as belt-and-suspenders. The movie is a blank SWF shipped at
    // Data/Interface/magelightfocus.swf; if it's missing the menu is skipped
    // and UI mode runs exactly as before (no engine menu).
    static void SetUIMode(bool on, bool hideView = true, bool pauseGame = false, bool noTextEntry = false);   // fwd (below)
    static bool EscapeCapturedByUiView();                                               // fwd (below)
    static std::filesystem::path GameRoot();                                            // fwd (below)
    static std::filesystem::path ResolveFileUrl(const std::string& url);                // fwd (below MlFileSystem)
    static bool PathIsUnder(const std::filesystem::path& p, const std::filesystem::path& base);   // fwd (below)
    static void CursorMenuWatchdog();                                                   // fwd (below)
    static std::atomic<bool> s_focusMenuPause{ false };   // flag for the NEXT open
    static bool              s_focusMenuRegistered = false;

    class MagelightFocusMenu final : public RE::IMenu
    {
    public:
        static constexpr const char* MENU_NAME = "MagelightFocus";

        MagelightFocusMenu()
        {
            using F = RE::UI_MENU_FLAGS;
            depthPriority = 10;  // above SA's carrier (0), below the CursorMenu
            inputContext  = RE::UserEvents::INPUT_CONTEXT_ID::kMenuMode;
            menuFlags.set(F::kUsesCursor, F::kUsesMenuContext, F::kDontHideCursorWhenTopmost,
                          F::kCustomRendering, F::kAllowSaving);
            if (s_focusMenuPause.load()) menuFlags.set(F::kPausesGame);
            if (auto* sm = RE::BSScaleformManager::GetSingleton()) {
                sm->LoadMovie(this, uiMovie, "magelightfocus");
            }
        }

        static RE::IMenu* Create() { return new MagelightFocusMenu(); }

        RE::UI_MESSAGE_RESULTS ProcessMessage(RE::UIMessage& msg) override
        {
            GameTask::Scope pumpScope;   // posts from here go through the pump (MagelightGameTask.h)
            if (msg.type == RE::UI_MESSAGE_TYPE::kUserEvent) {
                if (auto* d = static_cast<RE::BSUIMessageData*>(msg.data)) {
                    if (d->fixedStr == "Cancel") {
                        // Escape / gamepad B through the engine: exit UI mode
                        // (idempotent with the WndProc Escape path). A page
                        // that captured Escape (0.28.0) keeps it on THIS path
                        // too: on VR the keyboard poll is not muted, so a
                        // physical Escape reaches the engine's MenuControls as
                        // Cancel and closed the UI under a modal that had
                        // asked for the key (review 2026-09-09).
                        if (!EscapeCapturedByUiView())
                            GameTask::Post([]() { SetUIMode(false); });
                        return RE::UI_MESSAGE_RESULTS::kHandled;
                    }
                    // Every other menu-mode user event (Backspace, Enter,
                    // arrows, F-keys — the controlmap's 0x10 group) is the
                    // page's business and was already delivered through the
                    // window proc. Swallow it here so it cannot fall through
                    // to whatever engine menu sits beneath us (field
                    // 2026-09-03: the cursor froze right after Backspace).
                    static std::unordered_set<std::string> s_seen;
                    const std::string ev = d->fixedStr.c_str() ? d->fixedStr.c_str() : "";
                    if (s_seen.insert(ev).second)
                        SKSE::log::info("Magelight: menu user event '{}' swallowed by the focus menu", ev);
                    return RE::UI_MESSAGE_RESULTS::kHandled;
                }
            } else if (msg.type == RE::UI_MESSAGE_TYPE::kHide) {
                // Engine-initiated close (loading screen, force-close): UI
                // mode must follow. Our own SetUIModeImpl(false) clears
                // s_focused BEFORE queuing this hide, so it never re-enters.
                if (s_focused.load()) {
                    GameTask::Post([]() { SetUIMode(false); });
                }
            }
            return RE::IMenu::ProcessMessage(msg);
        }

        void PostDisplay() override {}  // kCustomRendering: nothing to draw

        // ── Skyrim VR object layout (0.17.2) ─────────────────────────────
        // IMenu is 0x30 bytes on SE/AE but 0x40 on VR: the VR engine keeps
        // unk30 (set through the VR-only vfunc 09), unk34 and a
        // BSFixedString menuName at 0x38 on EVERY menu. CommonLibSSE-NG's
        // cross-runtime build leaves those out of the struct and reaches
        // them by relocation (GetVRRuntimeData -> this+0x30), so an object
        // allocated at the SE size hands the VR engine garbage past its end:
        // opening this menu crashed SkyrimVR.exe reading menuName on the
        // first F10 in the headset (2026-09-03, VR-2 field pass). These
        // members reproduce the VR tail exactly; on SE they are dead weight
        // the engine never looks at.
        REX::EnumSet<RE::UI_MENU_Unk09, std::uint32_t> vrUnk30{ RE::UI_MENU_Unk09::kNone };   // 30
        std::byte                                      vrUnk34{ 1 };                          // 34
        RE::BSFixedString                              vrMenuName{ MENU_NAME };               // 38
    };
    static_assert(sizeof(MagelightFocusMenu) == 0x40, "MagelightFocusMenu must match the VR IMenu size (0x40)");

    static void RegisterFocusMenu()
    {
        std::error_code ec;
        const auto swf = GameRoot() / L"Data" / L"Interface" / L"magelightfocus.swf";
        if (!std::filesystem::exists(swf, ec)) {
            SKSE::log::warn("Magelight: {} missing — UI mode runs without an engine menu",
                swf.string());
            return;
        }
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->Register(MagelightFocusMenu::MENU_NAME, MagelightFocusMenu::Create);
            s_focusMenuRegistered = true;
            SKSE::log::info("Magelight: engine menu '{}' registered", MagelightFocusMenu::MENU_NAME);
        }
    }

    static LONG LogSehAndDisable(const char* where, unsigned long code, void* addr);  // fwd (frame section)

    // ── UI-pass composite ──────────────────────────────────────────────────
    // With the composite in the game's UI pass, Present still updates and renders the views (into their own
    // textures) and records the quads it would have drawn; the carrier menu's PostDisplay, which the engine
    // calls inside its UI render with the UI's render target bound, draws them there one frame later.
    struct QueuedQuad {
        ID3D11ShaderResourceView* srv = nullptr;   // owned ref
        float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
        float x = 0, y = 0, w = 0, h = 0;
        bool  hasCut = false;
        float cut[4] = { 0, 0, 0, 0 };
    };
    static std::mutex              s_uiPassMutex;
    static std::vector<QueuedQuad> s_uiPassQuads;            // the last published frame
    static float                   s_uiPassBackW = 0, s_uiPassBackH = 0;   // the back buffer they were laid out for
    static std::chrono::steady_clock::time_point s_uiPassPublished{};   // a set older than 250 ms is not drawn
    static std::atomic<std::uint64_t> s_compositeFrames{ 0 };          // FrameWork composites, deferred or not
    static std::atomic<std::uint64_t> s_lastUiPassFrame{ 0 };          // s_compositeFrames at the last PostDisplay
    static std::atomic<bool>          s_uiPassSeen{ false };
    static bool                       s_overlayMenuRegistered = false;

    static void ReleaseQuads(std::vector<QueuedQuad>& qs)
    {
        for (auto& q : qs) if (q.srv) q.srv->Release();
        qs.clear();
    }

    // Present defers to the UI pass only while the carrier menu is actually drawing: its PostDisplay ran within
    // the last two composites. Menus hidden (the console's tm, a photo mode, loading screens) fall back to
    // Present at once, so the views never vanish with them.
    static bool UiPassActive()
    {
        if (!s_uiPassSeen.load()) return false;
        return s_compositeFrames.load() - s_lastUiPassFrame.load() <= 2;
    }

    static void UiPassComposite();   // fwd (frame section)

    // The carrier: an invisible menu that exists for its PostDisplay, open only while an interactive view is
    // visible. In the menu stack it reads to the engine as an open menu, so Escape no longer opens the Journal
    // while it is up: it must never stay open over plain gameplay, and a click-through HUD view draws at
    // Present instead. It takes no input; user events pass on.
    class MagelightOverlayMenu final : public RE::IMenu
    {
    public:
        static constexpr const char* MENU_NAME = "MagelightOverlay";

        MagelightOverlayMenu()
        {
            using F = RE::UI_MENU_FLAGS;
            depthPriority = 9;  // over the game's menus (3) and HUD, under the focus menu (10), which stays topmost
            menuFlags.set(F::kAllowSaving);
            if (auto* sm = RE::BSScaleformManager::GetSingleton()) {
                sm->LoadMovie(this, uiMovie, "magelightfocus");
            }
        }

        static RE::IMenu* Create() { return new MagelightOverlayMenu(); }

        RE::UI_MESSAGE_RESULTS ProcessMessage(RE::UIMessage& msg) override
        {
            if (msg.type == RE::UI_MESSAGE_TYPE::kUserEvent) return RE::UI_MESSAGE_RESULTS::kPassOn;
            return RE::IMenu::ProcessMessage(msg);
        }

        void PostDisplay() override { UiPassComposite(); }

        // VR object layout: see MagelightFocusMenu.
        REX::EnumSet<RE::UI_MENU_Unk09, std::uint32_t> vrUnk30{ RE::UI_MENU_Unk09::kNone };   // 30
        std::byte                                      vrUnk34{ 1 };                          // 34
        RE::BSFixedString                              vrMenuName{ MENU_NAME };               // 38
    };
    static_assert(sizeof(MagelightOverlayMenu) == 0x40, "MagelightOverlayMenu must match the VR IMenu size (0x40)");

    // `streamlineSwapChain`: the game swapchain's vtable is in sl.interposer.dll (InstallHook reads it).
    static bool UiCompositeWanted(bool streamlineSwapChain)
    {
        if (REL::Module::IsVR()) return false;
        if (s_compositeMode == "ui") return true;
        if (s_compositeMode == "present") return false;
        return streamlineSwapChain || GetModuleHandleW(L"SkyrimUpscaler.dll") != nullptr;
    }

    static void RegisterOverlayMenu(bool streamlineSwapChain)
    {
        if (!UiCompositeWanted(streamlineSwapChain)) {
            SKSE::log::info("Magelight: composite '{}' - views draw at Present", s_compositeMode);
            return;
        }
        std::error_code ec;
        if (!std::filesystem::exists(GameRoot() / L"Data" / L"Interface" / L"magelightfocus.swf", ec)) {
            SKSE::log::warn("Magelight: magelightfocus.swf missing - views draw at Present");
            return;
        }
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->Register(MagelightOverlayMenu::MENU_NAME, MagelightOverlayMenu::Create);
            s_overlayMenuRegistered = true;
            SKSE::log::info("Magelight: composite '{}' - views draw in the game's UI pass ('{}')", s_compositeMode,
                MagelightOverlayMenu::MENU_NAME);
        }
    }

    // Opens the carrier while a view is visible and closes it a second after the last one hides. Called from
    // FrameWork on the main thread. A show the engine drops (a load closes every menu) is re-queued after two
    // seconds; a hide waits so a view flickering between pages does not open and close the menu every frame.
    static void SyncOverlayMenu(bool wanted)
    {
        if (!s_overlayMenuRegistered) return;
        auto* ui = RE::UI::GetSingleton();
        auto* q = RE::UIMessageQueue::GetSingleton();
        if (!ui || !q) return;
        using Clock = std::chrono::steady_clock;
        static Clock::time_point s_nextShow{}, s_nextHide{}, s_lastWanted{};
        const auto now = Clock::now();
        const bool open = ui->IsMenuOpen(MagelightOverlayMenu::MENU_NAME);
        if (wanted) {
            s_lastWanted = now;
            s_nextHide = Clock::time_point{};
            if (!open && now >= s_nextShow) {
                s_nextShow = now + std::chrono::seconds(2);
                q->AddMessage(MagelightOverlayMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kShow, nullptr);
            }
        } else if (open && now - s_lastWanted >= std::chrono::seconds(1) && now >= s_nextHide) {
            s_nextHide = now + std::chrono::seconds(2);
            s_nextShow = Clock::time_point{};
            q->AddMessage(MagelightOverlayMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
        }
    }

    static void QueueInput(UINT msg, WPARAM w, LPARAM l)
    {
        std::lock_guard<std::mutex> lk(s_inputMutex);
        if (s_inputQueue.size() < 512) s_inputQueue.push_back({ msg, w, l });
    }

    // ── Typing on VR: the engine's keyboard device, not the window ──────
    // In a headset the game window almost never holds OS focus, so it receives
    // NO keyboard messages at all — field-verified 2026-09-03: with a page
    // focused and both a physical keyboard and the runtime's virtual keyboard
    // in use, not one WM_CHAR, WM_KEYDOWN or private message arrived. The flat
    // host types through the window proc; on VR that path is simply dead.
    //
    // The SKSE input sink DOES see the engine's keyboard device (that is how
    // Escape and the hotkeys work in VR), so translate its DirectInput
    // scancode into exactly the WM_* the window would have produced and queue
    // it. DrainInputQueue is unchanged. This also picks up any keyboard the
    // RUNTIME synthesises at the driver level (OpenComposite's virtual
    // keyboard sends scancodes alongside its own delivery paths).
    //
    // Modifier state comes from GetAsyncKeyState, which is global and does not
    // need window focus. Called from the input sink (game thread).
    static void QueueScancodeAsText(std::uint32_t scan, bool down)
    {
        if (scan == 0 || scan > 0xFF) return;
        const UINT vk = MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX);
        if (!vk) return;
        const LPARAM base = static_cast<LPARAM>(scan) << 16;
        if (!down) {
            QueueInput(WM_KEYUP, vk, base | (1 << 30) | (1 << 31));
            return;
        }
        QueueInput(WM_KEYDOWN, vk, base);
        // Printable? Ask the active layout, with the live modifier state.
        BYTE ks[256]{};
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   ks[VK_SHIFT]   = 0x80;
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) ks[VK_CONTROL] = 0x80;
        if (GetAsyncKeyState(VK_MENU) & 0x8000)    ks[VK_MENU]    = 0x80;
        if (GetKeyState(VK_CAPITAL) & 1)           ks[VK_CAPITAL] = 1;
        wchar_t buf[8]{};
        const int n = ToUnicode(vk, scan, ks, buf, 7, 0);
        for (int i = 0; i < n && i < 7; ++i) {
            const wchar_t ch = buf[i];
            if (ch >= 0x20 || ch == L'\t' || ch == L'\r')
                QueueInput(WM_CHAR, static_cast<WPARAM>(ch), 0);
        }
        static std::atomic<int> s_typeLog{ 0 };
        if (s_typeLog.fetch_add(1) < 12)
            SKSE::log::info("Magelight: VR typing — scancode 0x{:02X} -> vk 0x{:02X}, {} char(s)", scan, vk, n);
    }

    // Public bridge (declared in Magelight.h with stdint types): the VR laser
    // injects WM_MOUSEMOVE / WM_LBUTTON* through the same queue.
    void QueueSyntheticInput(unsigned int msg, std::uintptr_t wparam, std::intptr_t lparam)
    {
        QueueInput(static_cast<UINT>(msg), static_cast<WPARAM>(wparam), static_cast<LPARAM>(lparam));
    }

    // Game thread only (a posted task). Idempotent. Step-logged (0.3.2 diagnostics:
    // the teardown is the crash neighborhood) and SEH-guarded by the caller.
    // hideView applies only to the OFF path: key-driven / load-boundary
    // exits hide the UI-mode view (the demo + SA close behavior); the public
    // ExitUIMode passes false so a view can stay rendered while the game
    // owns input (SA's Free Look).
    // Game thread. The CursorMenu stays OPEN while UI mode is on (it drives the MenuCursor position) but its sprite
    // must not draw: at Present it lands under the overlay, but in the UI pass the CursorMenu draws after us, on top.
    // SetCursorVisibility only hides the Windows cursor; the sprite is drawn by the menu's movie. Its root is made
    // transparent (flat only: VR keeps its own pointer path): the engine re-shows the movie itself every frame, which
    // made GFxMovieView::SetVisible blink, but never touches the root's own _alpha/_visible, and a reskinned cursor
    // movie hides the same way. FrameWork re-asserts this shortly after entry and every second (the movie only exists
    // once the menu's show has processed); exit restores both.
    static void HideVanillaCursor(bool hide)
    {
        if (auto* mc = RE::MenuCursor::GetSingleton()) mc->SetCursorVisibility(!hide);
        if (REL::Module::IsVR()) return;
        if (auto* ui = RE::UI::GetSingleton()) {
            if (const auto menu = ui->GetMenu(RE::CursorMenu::MENU_NAME); menu && menu->uiMovie) {
                menu->uiMovie->SetVariableDouble("_root._alpha", hide ? 0.0 : 100.0);
                menu->uiMovie->SetVariable("_root._visible", RE::GFxValue(!hide));
            }
        }
    }

    static void SetUIModeImpl(bool on, bool hideView, bool pauseGame, bool noTextEntry)
    {
        if (s_focused.load() == on) return;
        const ViewId uiView = static_cast<ViewId>(s_uiModeView.load());
        if (on && !s_rendererUp.load()) {
            // No renderer = nothing to show: entering UI mode here would only
            // suspend the game's controls over an empty screen (field
            // 2026-09-03, the missed-kNewGame session). Refuse, loudly.
            SKSE::log::warn("Magelight: UI mode refused — renderer not initialised yet (world not ready)");
            Emit(HostEvent::UIModeRefused, uiView, 0, 0, "renderer not ready");
            return;
        }
        SKSE::log::info("Magelight: SetUIMode({}{}) begin", on, (on && pauseGame) ? ", paused" : "");
        s_focused.store(on);
        if (!on && s_hwnd) { s_imeCaretX.store(-1); PostMessageW(s_hwnd, kImeCtlMsg, 0, 0); }   // IME follows UI mode off
        // UI mode shows its target view; other views (HUD badges etc.) keep
        // their own visibility.
        if (on) ShowView(uiView, true);
        else if (hideView) ShowView(uiView, false);
        if (!on) ShowOwnKeyboard(false);
        if (!on) {
            // Inspectors live only inside UI mode: Escape on the inspector (or
            // on its page) would otherwise leave it painted over the lower
            // half of the screen with nothing focused (field log 2026-09-02).
            std::vector<ViewId> inspectors;
            {
                std::lock_guard<std::mutex> lk(s_viewsMutex);
                for (auto& vp : s_views) if (vp->isInspector && vp->visible) inspectors.push_back(vp->id);
            }
            for (ViewId i : inspectors) ShowView(i, false);
        }

        if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
            queue->AddMessage(RE::CursorMenu::MENU_NAME,
                on ? RE::UI_MESSAGE_TYPE::kShow : RE::UI_MESSAGE_TYPE::kHide, nullptr);
            // Our engine menu rides the same queue; pause is decided per open
            // (the flag is read by the menu's constructor on show).
            if (s_focusMenuRegistered) {
                if (on) s_focusMenuPause.store(pauseGame);
                queue->AddMessage(MagelightFocusMenu::MENU_NAME,
                    on ? RE::UI_MESSAGE_TYPE::kShow : RE::UI_MESSAGE_TYPE::kHide, nullptr);
            }
        }
        SKSE::log::info("Magelight: cursor-menu {} queued{}", on ? "show" : "hide",
            s_focusMenuRegistered ? " (+ engine menu)" : "");
        // We draw our own cursor topmost; the show message above processes async, so FrameWork re-asserts this.
        HideVanillaCursor(on);
        if (!on) s_mouseDown.store(false);
        if (auto* cm = RE::ControlMap::GetSingleton()) {
            using UEFlag = RE::ControlMap::UEFlag;
            // Scoped enum without a free operator| — combine the bits directly.
            const auto flags = static_cast<UEFlag>(
                std::to_underlying(UEFlag::kMovement) | std::to_underlying(UEFlag::kLooking) |
                std::to_underlying(UEFlag::kActivate) | std::to_underlying(UEFlag::kMenu) |
                std::to_underlying(UEFlag::kFighting) | std::to_underlying(UEFlag::kPOVSwitch) |
                std::to_underlying(UEFlag::kWheelZoom));
            // storeState=false: a transient toggle that can never bake into a
            // save. Console stays reachable as an escape hatch.
            cm->ToggleControls(flags, !on, false);
            // Text-entry flag while UI mode is on (the engine's own text
            // fields do the same through SkyUI's AllowTextInput). SKSE
            // suppresses Papyrus OnKeyDown while it is raised, and the
            // well-behaved native mods skip their hotkeys too — so a key
            // typed into a page can no longer double as another mod's
            // hotkey (field 2026-09-03: Backspace in a search field opened
            // a third mod's display manager and froze the cursor).
            // Balanced: raised once per focus, lowered once per unfocus.
            // kUIModeFlagNoTextEntry: a page with nothing to type into asks
            // us not to raise it — on Skyrim VR the raise starts the engine's
            // virtual keyboard device, which OpenComposite hooks to raise its
            // own keyboard over the page (field 2026-09-05, SA's quick wheel).
            static bool s_textEntryRaised = false;
            if (on && !s_textEntryRaised && !noTextEntry) {
                cm->AllowTextInput(true);
                s_textEntryRaised = true;
            } else if (!on && s_textEntryRaised) {
                cm->AllowTextInput(false);
                s_textEntryRaised = false;
            }
        }
        SKSE::log::info("Magelight: controls {}", on ? "suspended" : "restored");
        // OpenComposite's own menu laser: its SKSE plugin sets the window
        // property OC_MENU_ACTIVE whenever an engine menu is open (ours
        // counts), and OCU draws a second beam over our panels. Clear it while
        // we hold focus — OCU's own MenuOpenCloseEvent watcher re-asserts it
        // the moment a real menu opens or ours closes, so we never leave it
        // wrong. Their laser could not point at our overlays anyway.
        if (on && s_hwnd && VR::SuppressRuntimeLaser()) {
            SetPropW(s_hwnd, L"OC_MENU_ACTIVE", reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(0)));
            SKSE::log::info("Magelight: cleared OC_MENU_ACTIVE — the runtime's own menu laser stands down");
        }
        QueueInput(0, on ? 1 : 0, 0);  // focus/unfocus marker for the render thread
        SKSE::log::info("Magelight: UI mode {}", on ? "ON (cursor up, game controls suspended)" : "OFF");
        // Close hook: every exit path lands here on the game thread — the
        // embedding mod's (SA's) close bookkeeping hangs off this.
        if (!on) {
            std::vector<UIModeExitFn> cbs;
            {
                std::lock_guard<std::mutex> lock(s_uiModeExitMutex);
                cbs = s_uiModeExitCbs;
            }
            for (UIModeExitFn cb : cbs) cb(uiView);
        }
        // 0.29.0: the view's manifest/API open/close sound. Game
        // thread already — play directly, after the exit hooks so a close
        // sound never precedes the consumer's own close bookkeeping.
        {
            std::string snd;
            {
                std::lock_guard<std::mutex> lock(s_viewsMutex);
                if (MlView* v = FindViewLocked(uiView)) snd = on ? v->soundOpen : v->soundClose;
            }
            if (!snd.empty()) Sound::Play(snd, uiView);
        }
        Emit(on ? HostEvent::UIModeEntered : HostEvent::UIModeExited, uiView);
        InvokeJS(uiView, on ? "window.magelight&&window.magelight._dispatch('__uimode','1');"
                            : "window.magelight&&window.magelight._dispatch('__uimode','0');");
    }

    // SEH wrapper — no locals with destructors allowed in here.
    static void SetUIMode(bool on, bool hideView, bool pauseGame, bool noTextEntry)
    {
        __try {
            SetUIModeImpl(on, hideView, pauseGame, noTextEntry);
        } __except (LogSehAndDisable("SetUIMode", GetExceptionCode(),
                        (GetExceptionInformation())->ExceptionRecord->ExceptionAddress)) {
        }
    }

    // OpenComposite Unleashed's virtual keyboard posts every character to the
    // GAME WINDOW as a private message (VRKeyboard.cpp: WM_OC_CHAR = WM_APP +
    // 0x4F45; wParam = wchar_t, lParam 0 = printable character, 1 = a virtual
    // key such as Backspace/Enter/Tab/Escape). We already subclass that window,
    // so OCU's keyboard reaches a Magelight page with NO change on their side —
    // their dedicated export path (which resolves another host's DLL by name and can
    // never find us) is not the only road in. Only consumed while a page holds
    // focus; otherwise it passes through to the console/Scaleform as usual.
    static constexpr UINT kOcCharMsg = WM_APP + 0x4F45;

    // Returns true when the message was consumed (swallow at the window proc).
    // Focused-window hotkey path. The engine input sink does not deliver
    // keyboard button events while our menu-context engine menu is up (F10
    // opened the manifest panel every time and never closed it, 2026-09-02),
    // but the window still receives WM_KEYDOWN — that is how the page gets
    // its typing. The lparam scancode is the DirectInput code; extended keys
    // (arrows, Page Up/Down, Home/End/Insert/Delete) carry bit 24 and map to
    // the 0x80+ range. Auto-repeat (bit 30) never re-fires.
    static void PeekHotkey(LPARAM lparam)
    {
        if (lparam & (1 << 30)) return;
        const std::uint32_t scan = static_cast<std::uint32_t>((lparam >> 16) & 0xFF);
        if (!scan) return;
        const std::uint32_t dx = scan | ((lparam & (1 << 24)) ? 0x80u : 0u);
        Api4::DispatchHotkey(dx, "window");
    }

    static std::string WideToUtf8(const std::wstring& w)
    {
        if (w.empty()) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<std::size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
        return out;
    }

    static std::wstring ImeGetString(HIMC hImc, DWORD what)
    {
        const LONG bytes = ImmGetCompositionStringW(hImc, what, nullptr, 0);
        if (bytes <= 0) return {};
        std::wstring out(static_cast<std::size_t>(bytes) / sizeof(wchar_t), L'\0');
        ImmGetCompositionStringW(hImc, what, out.data(), static_cast<DWORD>(bytes));
        return out;
    }

    // -> the UI-mode page's reserved '__ime' channel ({state, text}); the
    // bridge script draws the pre-edit and re-emits it as 'magelight:ime'.
    static void ImeNotifyPage(const char* state, const std::wstring& text)
    {
        const std::string json = std::string("{\"state\":\"") + state + "\",\"text\":" + MlJsonQuote(WideToUtf8(text)) + "}";
        InteropCall(static_cast<ViewId>(s_uiModeView.load()), "__ime", json);
    }

    static void ImeRepositionCandidates()   // window thread
    {
        const int x = s_imeCaretX.load(), y = s_imeCaretY.load(), h = s_imeCaretH.load();
        if (x < 0 || !s_hwnd) return;
        if (HIMC hImc = ImmGetContext(s_hwnd)) {
            CANDIDATEFORM cf{};
            cf.dwIndex = 0; cf.dwStyle = CFS_CANDIDATEPOS; cf.ptCurrentPos = { x, y + h };
            ImmSetCandidateWindow(hImc, &cf);
            COMPOSITIONFORM cpf{};
            cpf.dwStyle = CFS_POINT; cpf.ptCurrentPos = { x, y };
            ImmSetCompositionWindow(hImc, &cpf);
            ImmReleaseContext(s_hwnd, hImc);
        }
    }

    static void ImeAttach(bool on)   // window thread only (kImeCtlMsg)
    {
        if (!s_hwnd || s_imeAttached.load() == on) return;
        if (on) {
            const BOOL ok = ImmAssociateContextEx(s_hwnd, nullptr, IACE_DEFAULT);
            // Diagnostics: did the window actually get a context, is the IME open,
            // and is it in a native (hiragana/kana) conversion mode or alphanumeric?
            // Field 2026-09-06: keys reached the page as plain letters with no
            // composition — this line tells 'A mode' apart from 'no context'.
            if (HIMC hImc = ImmGetContext(s_hwnd)) {
                // OPEN the IME: the on/off state is per window, and a freshly
                // attached context starts CLOSED — every key passed through
                // untouched even in hiragana mode (field 2026-09-06 18:00:
                // open=false, conversion=native). Opening on focus means the
                // user never has to hit the IME on/off key inside the game.
                if (!ImmGetOpenStatus(hImc)) ImmSetOpenStatus(hImc, TRUE);
                DWORD conv = 0, sent = 0;
                ImmGetConversionStatus(hImc, &conv, &sent);
                SKSE::log::info("Magelight: IME attach ok={} context=yes open={} conversion=0x{:X} ({})",
                    ok != FALSE, ImmGetOpenStatus(hImc) != FALSE, conv,
                    (conv & IME_CMODE_NATIVE) ? "native/hiragana" : "alphanumeric — composition will not start");
                ImmReleaseContext(s_hwnd, hImc);
            } else {
                SKSE::log::warn("Magelight: IME attach ok={} but the window has NO context — IME disabled on this thread?", ok != FALSE);
            }
        } else {
            if (HIMC hImc = ImmGetContext(s_hwnd)) {
                if (s_imeComposing.load()) ImmNotifyIME(hImc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
                ImmReleaseContext(s_hwnd, hImc);
            }
            ImmAssociateContext(s_hwnd, nullptr);
            s_imeComposing.store(false);
        }
        s_imeAttached.store(on);
        SKSE::log::info("Magelight: IME context {}", on ? "attached (text field focused)" : "detached");
    }

    // The page's caret, "x,y,h" in its CSS px -> client px of the game window,
    // so the OS candidate list opens at the caret rather than the corner.
    static void ImeOnCaret(ViewId id, const std::string& arg)
    {
        float cx = 0.f, cy = 0.f, ch = 0.f;
        if (sscanf_s(arg.c_str(), "%f,%f,%f", &cx, &cy, &ch) == 3) {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            if (MlView* v = FindViewLocked(id)) {
                float ex = 0.f, ey = 0.f;
                EffectivePos(*v, static_cast<float>(s_backbufferW.load()), static_cast<float>(s_backbufferH.load()), ex, ey);
                const float ds = v->deviceScale > 0.f ? v->deviceScale : 1.f;
                s_imeCaretX.store(static_cast<int>(ex + cx * ds));
                s_imeCaretY.store(static_cast<int>(ey + cy * ds));
                s_imeCaretH.store(static_cast<int>(ch * ds));
            }
        }
        if (s_imeComposing.load() && s_hwnd) PostMessageW(s_hwnd, kImeCtlMsg, 2, 0);
    }

    // The window's IME context follows text-field focus in the UI-mode view
    // (the caller filters other views); on VR the runtime keyboard owns text
    // and the OS IME UI is invisible.
    static void ImeOnTextFocus(bool focused)
    {
        if (VR::IsLive() || !s_hwnd) return;
        if (!focused) s_imeCaretX.store(-1);
        PostMessageW(s_hwnd, kImeCtlMsg, focused ? 1 : 0, 0);
    }

    // 0.28.0: does the UI-mode page own Escape right now? (SetEscapeCapture /
    // the '__escapecapture' channel — a modal or an editing control closes
    // itself instead of the whole UI.)
    static bool EscapeCapturedByUiView()
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        const MlView* v = FindViewLocked(static_cast<ViewId>(s_uiModeView.load()));
        return v && v->escapeCapture;
    }

    static bool HandleFocusedMsg(UINT msg, WPARAM wparam, LPARAM lparam)
    {
        switch (msg) {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_MOUSEWHEEL:
            // Swallow but do NOT queue: mouse rides the SKSE sink + MenuCursor
            // (0.3.5) — Skyrim's DirectInput never delivers WM mouse messages
            // here anyway (zero arrived across every field run), and queueing
            // both sources would double-fire if one ever did.
            return true;
        case WM_KEYDOWN: case WM_KEYUP:
        case WM_CHAR:
            if ((msg == WM_KEYDOWN || msg == WM_KEYUP) && wparam == VK_PROCESSKEY) return true;   // the IME owns this key
            QueueInput(msg, wparam, lparam);
            // Editing-key trace (Backspace/Delete): a field report of
            // "Backspace does nothing in text fields" needs to know whether
            // the key reaches the window at all (rate-limited).
            if (msg == WM_KEYDOWN && (wparam == VK_BACK || wparam == VK_DELETE) && !(lparam & (1 << 30))) {
                static std::atomic<int> s_n{ 0 };
                if (s_n.fetch_add(1) < 8)
                    SKSE::log::info("Magelight: WM_KEYDOWN vk=0x{:02X} scan=0x{:02X} queued", wparam, (lparam >> 16) & 0x1FF);
            }
            if (msg == WM_KEYDOWN && wparam == VK_ESCAPE && !EscapeCapturedByUiView()) {   // captured: the page gets the key instead (queued above)
                GameTask::Post([]() { SetUIMode(false); });
            }
            if (msg == WM_KEYDOWN) PeekHotkey(lparam);
            // Dev-loop keys (manual reload / inspector toggle) are deliberately
            // UNBOUND as of 0.17.0 — the owner wants F5/F12 free for now. The
            // machinery stays (ReloadView, s_inspectorRequest -> the inspector
            // host); a binding comes back later, probably via Magelight.json.
            return true;
        case kOcCharMsg: {
            const wchar_t wch = static_cast<wchar_t>(wparam);
            if (lparam == 1) {   // control key delivered as a virtual key
                const LPARAM scan = static_cast<LPARAM>(
                    MapVirtualKeyW(static_cast<UINT>(wch), MAPVK_VK_TO_VSC) & 0xFF) << 16;
                QueueInput(WM_KEYDOWN, static_cast<WPARAM>(wch), scan);
                QueueInput(WM_KEYUP, static_cast<WPARAM>(wch), scan | (1 << 30) | (1 << 31));
            } else if (wch >= 0x20 || wch == L'\t' || wch == L'\r') {
                QueueInput(WM_CHAR, static_cast<WPARAM>(wch), 0);
            } else if (wch == L'\b') {
                QueueInput(WM_KEYDOWN, VK_BACK, 0);
                QueueInput(WM_KEYUP, VK_BACK, 0);
            }
            static std::atomic<int> s_ocLog{ 0 };
            if (s_ocLog.fetch_add(1) < 6)
                SKSE::log::info("Magelight: OpenComposite keyboard char 0x{:04X} (mode {}) delivered to view {}",
                    static_cast<unsigned>(wch), static_cast<int>(lparam),
                    static_cast<std::uint64_t>(s_uiModeView.load()));
            return true;   // ours now: don't let Scaleform also eat it
        }
        case WM_SYSKEYDOWN: case WM_SYSKEYUP:
            // Let the system combo (Alt+F4 etc.) proceed; nothing to the view —
            // but F10 and Alt+key arrive HERE, so bound hotkeys are still peeked.
            if (msg == WM_SYSKEYDOWN) PeekHotkey(lparam);
            return false;
        // ── IME (0.27.0) ──
        case WM_IME_STARTCOMPOSITION:
            s_imeComposing.store(true);
            SKSE::log::info("Magelight: IME composition started");
            ImeNotifyPage("start", L"");
            ImeRepositionCandidates();
            return true;   // no DefWindowProc: it would open the OS composition window; the pre-edit lives in the page
        case WM_IME_COMPOSITION: {
            if (HIMC hImc = ImmGetContext(s_hwnd)) {
                if (lparam & GCS_RESULTSTR) {
                    const std::wstring r = ImeGetString(hImc, GCS_RESULTSTR);
                    ImeNotifyPage("commit", r);
                    for (wchar_t ch : r) QueueInput(WM_CHAR, static_cast<WPARAM>(ch), 0);   // pairs re-join in the drain
                    static std::atomic<int> s_n{ 0 };
                    if (s_n.fetch_add(1) < 6) SKSE::log::info("Magelight: IME commit ({} UTF-16 unit(s))", r.size());
                }
                if (lparam & GCS_COMPSTR) ImeNotifyPage("compose", ImeGetString(hImc, GCS_COMPSTR));
                ImmReleaseContext(s_hwnd, hImc);
            }
            ImeRepositionCandidates();
            return true;   // never DefWindowProc: it would re-deliver the result as WM_CHARs (double insert)
        }
        case WM_IME_ENDCOMPOSITION:
            s_imeComposing.store(false);
            ImeNotifyPage("end", L"");
            return true;
        case WM_IME_SETCONTEXT:
            // Keep the OS candidate list; drop the OS composition window (the pre-edit is inline).
            DefWindowProcW(s_hwnd, msg, wparam, lparam & ~static_cast<LPARAM>(ISC_SHOWUICOMPOSITIONWINDOW));
            return true;
        default:
            return false;
        }
    }

    // SEH wrapper — no locals with destructors allowed in here.
    static bool GuardedHandleFocusedMsg(UINT msg, WPARAM wparam, LPARAM lparam)
    {
        __try {
            return HandleFocusedMsg(msg, wparam, lparam);
        } __except (LogSehAndDisable("wndproc", GetExceptionCode(),
                        (GetExceptionInformation())->ExceptionRecord->ExceptionAddress)) {
        }
        return false;
    }

    static LRESULT CALLBACK HookWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
    {
        GameTask::Scope pumpScope;   // posts from here go through the pump (MagelightGameTask.h)
        if (msg == kImeCtlMsg) {   // IME control, marshalled to the window's thread
            if (wparam == 2) ImeRepositionCandidates();
            else ImeAttach(wparam != 0);
            return 0;
        }
        if (s_focused.load() && !s_renderDead.load()) {
            if (GuardedHandleFocusedMsg(msg, wparam, lparam)) return 0;
        }
        if (!s_origWndProc) return DefWindowProcW(hwnd, msg, wparam, lparam);
        return CallWindowProcW(s_origWndProc, hwnd, msg, wparam, lparam);
    }

    // The game's window, its message loop and most SKSE plugins' subclasses are ANSI. A plugin that subclasses
    // after us with SetWindowLongPtrA and calls the proc it replaced directly, not through CallWindowProcA, is
    // handed a USER32 thunk handle for our W proc and executes 0xFFFFxxxx. Sitting on top as an ANSI proc gives it
    // a real address; CallWindowProcA converts A->W exactly as DispatchMessageA did, so HookWndProc sees no change.
    static LRESULT CALLBACK AnsiShimWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
    {
        const WNDPROC next = s_ansiShimPrev.load();
        if (!next) return DefWindowProcA(hwnd, msg, wparam, lparam);
        return CallWindowProcA(next, hwnd, msg, wparam, lparam);
    }

    // ── Runtime preload ─────────────────────────────────────────────────────

    static std::filesystem::path GameRoot()
    {
        wchar_t buf[MAX_PATH]{};
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        return std::filesystem::path(buf).parent_path();
    }

    // ── Crash telemetry (0.3.2 diagnostics) ─────────────────────────────────
    // A first-chance vectored handler that LOGS severe exceptions from ANY
    // thread — code, address, owning module + offset — then continues the
    // search (log-only, never swallows). The field CTD produces no crashlog
    // and doesn't trip our SEH nets, so it lives on a thread/path we don't
    // guard; this names it. Rate-limited; known-handled families skipped.
    // module+offset for an address, module BASENAME: one resolver and one
    // format for the VEH logger and the stall watchdog (review 2026-09-09).
    // `path` is scratch the returned pointer points into.
    static const char* ModuleOffsetOf(void* addr, char (&path)[MAX_PATH], std::uintptr_t& off)
    {
        HMODULE mod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(addr), &mod);
        path[0] = '?'; path[1] = 0;
        off = 0;
        if (!mod) return path;
        GetModuleFileNameA(mod, path, MAX_PATH);
        off = reinterpret_cast<std::uintptr_t>(addr) - reinterpret_cast<std::uintptr_t>(mod);
        const char* base = path;
        for (const char* p = path; *p; ++p) if (*p == '\\' || *p == '/') base = p + 1;
        return base;
    }

    // The watchdog thread is walking a stack: VectoredLogger leaves its caught faults alone.
    static thread_local bool t_walkingStack = false;

    static LONG CALLBACK VectoredLogger(EXCEPTION_POINTERS* ep)
    {
        if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
        const auto code = ep->ExceptionRecord->ExceptionCode;
        if (code < 0xC0000000u) return EXCEPTION_CONTINUE_SEARCH;                       // not severe
        if (code == 0xC06D007Eu || code == 0xC06D007Fu) return EXCEPTION_CONTINUE_SEARCH;  // delay-load internals
        if (code == 0xE06D7363u) return EXCEPTION_CONTINUE_SEARCH;                      // C++ EH in flight
        if (t_walkingStack) return EXCEPTION_CONTINUE_SEARCH;                           // the watchdog's own, caught

        static std::atomic<int> s_veCount{ 0 };
        if (s_veCount.fetch_add(1) >= 8) return EXCEPTION_CONTINUE_SEARCH;

        void* addr = ep->ExceptionRecord->ExceptionAddress;
        char modPath[MAX_PATH]; std::uintptr_t offset = 0;
        const char* modName = ModuleOffsetOf(addr, modPath, offset);
        SKSE::log::error(
            "Magelight VEH: exception 0x{:08X} at {} (module {} +0x{:X}) on thread {}",
            code, addr, modName, offset, GetCurrentThreadId());
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // ── Stall watchdog (0.28.3) ──────────────────────────────────────────
    // A hard freeze leaves no crash log and the tester cannot always produce
    // a dump; a soft stall (one frame every few seconds) leaves no trace at
    // all beyond our own cadence lines. Both reports of 2026-09-07 — the
    // outfit-page freeze and the quick wheel starving the loop in VR — ended
    // as "somewhere on the game thread, we do not know where". This thread
    // notices when the present hook has not stamped a frame for
    // stallThresholdMs while the world is live, suspends the thread that
    // presented the last frame (and the main thread when that is a different
    // one), walks its stack with the OS unwinder, resumes it, and logs the
    // frames as module+offset — what a process dump would show, captured
    // automatically. Rules that keep it safe:
    //   - the ONLY call made while the thread is suspended is
    //     GetThreadContext. The walk runs after ResumeThread: it is not
    //     lock-free (RtlLookupFunctionEntry takes ntdll's function-table
    //     locks and runs dynamic-table callbacks — JavaScriptCore registers
    //     JIT tables on the present thread), so a thread stalled inside one
    //     of those would have parked the watchdog with the target still
    //     suspended: a 1.5 s stall turned permanent by its own diagnostic
    //     (review 2026-09-09). Walking the LIVE stack after resume is sound
    //     for the case that matters — a truly stalled thread has not moved
    //     — and a thread that did move yields a garbage tail the SEH net
    //     absorbs. Module names are resolved after resume as before;
    //   - RtlLookupFunctionEntry/RtlVirtualUnwind are what exception
    //     dispatch itself uses on any thread, no dbghelp, no symbols;
    //   - the walk is SEH-netted in a function with no C++ objects;
    //   - rate-limited (one sample per 5 s, 12 per session) so a genuine
    //     freeze cannot flood the log, and a long stall is re-sampled so the
    //     log shows whether the stack MOVES (starvation) or not (deadlock).
    // Walks from a context captured under suspension; the thread is RUNNING
    // again by the time this is called (see the rules above).
    // NtQueryInformationThread's ThreadBasicInformation (class 0): enough to find a thread's TEB, whose NT_TIB holds
    // the committed part of its stack.
    struct MlThreadBasicInfo {
        LONG ExitStatus;
        PVOID TebBaseAddress;
        struct { HANDLE UniqueProcess; HANDLE UniqueThread; } ClientId;
        ULONG_PTR AffinityMask;
        LONG Priority;
        LONG BasePriority;
    };

    static bool ThreadStackRange(HANDLE thread, std::uintptr_t& low, std::uintptr_t& high)
    {
        using QueryFn = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
        static const auto query = reinterpret_cast<QueryFn>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
        MlThreadBasicInfo info{};
        if (!query || query(thread, 0, &info, sizeof(info), nullptr) < 0 || !info.TebBaseAddress) return false;
        const auto* tib = static_cast<const NT_TIB*>(info.TebBaseAddress);
        low  = reinterpret_cast<std::uintptr_t>(tib->StackLimit);
        high = reinterpret_cast<std::uintptr_t>(tib->StackBase);
        return high > low;
    }

    // Walks only inside [stackLow, stackHigh), the committed stack: a thread that ran on after its context was taken
    // leaves a stale one, and following it past the stack could clear another thread's guard page.
    static int UnwindFromContext(CONTEXT* ctx, void** frames, int cap, std::uintptr_t stackLow, std::uintptr_t stackHigh)
    {
        int n = 0;
        __try {
            while (n < cap && ctx->Rip) {
                frames[n++] = reinterpret_cast<void*>(ctx->Rip);
                if (ctx->Rsp < stackLow || ctx->Rsp + 8 > stackHigh) break;
                DWORD64 imageBase = 0;
                auto* fn = RtlLookupFunctionEntry(ctx->Rip, &imageBase, nullptr);
                if (!fn) {
                    // Leaf function: the return address sits at RSP.
                    ctx->Rip = *reinterpret_cast<DWORD64*>(ctx->Rsp);
                    ctx->Rsp += 8;
                    continue;
                }
                void*   handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx->Rip, fn, ctx, &handlerData, &establisher, nullptr);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return n > 0 ? n : -2;
        }
        return n;
    }

    static constexpr int kMaxStackFrames = 40;

    static void LogThreadStack(DWORD tid, const char* label, long long gapMs, int maxFrames = kMaxStackFrames)
    {
        void* frames[kMaxStackFrames]{};
        int   n = -3;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
        if (!h) {
            SKSE::log::warn("Magelight stall: cannot open {} thread {} (error {})", label, tid, GetLastError());
            return;
        }
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        bool haveCtx = false;
        if (SuspendThread(h) != static_cast<DWORD>(-1)) {
            haveCtx = GetThreadContext(h, &ctx) != 0;   // the only call under suspension
            ResumeThread(h);
        }
        std::uintptr_t stackLow = 0, stackHigh = 0;
        const bool haveStack = ThreadStackRange(h, stackLow, stackHigh);
        CloseHandle(h);
        if (haveCtx) {
            // Without the stack range, only the frame the context names.
            t_walkingStack = true;
            n = UnwindFromContext(&ctx, frames, haveStack ? (std::min)(maxFrames, kMaxStackFrames) : 1, stackLow, stackHigh);
            t_walkingStack = false;
        } else {
            n = -1;
        }
        if (n <= 0) {
            SKSE::log::warn("Magelight stall: no stack for {} thread {} (code {})", label, tid, n);
            return;
        }
        SKSE::log::warn("Magelight stall: no frame presented for {} ms — {} thread {} stack ({} frames):", gapMs, label, tid, n);
        for (int i = 0; i < n; ++i) {
            char path[MAX_PATH]; std::uintptr_t off = 0;
            const char* base = ModuleOffsetOf(frames[i], path, off);
            SKSE::log::warn("Magelight stall:   #{:02} {}+0x{:X}", i, base, off);
        }
    }

    // At most 64 threads, 24 frames each; the watchdog's own thread is skipped.
    static void LogOtherThreadStacks(DWORD presentTid, DWORD mainTid, long long gapMs)
    {
        const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) {
            SKSE::log::warn("Magelight stall: cannot list threads (error {})", GetLastError());
            return;
        }
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        int logged = 0;
        for (BOOL ok = Thread32First(snap, &te); ok && logged < 64; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid) continue;
            const DWORD tid = te.th32ThreadID;
            if (tid == self || tid == presentTid || tid == mainTid) continue;
            LogThreadStack(tid, "other", gapMs, 24);
            ++logged;
        }
        CloseHandle(snap);
        SKSE::log::warn("Magelight stall: {} other thread(s) sampled", logged);
    }

    static void StallWatchdogLoop()
    {
        using namespace std::chrono;
        long long lastReportMs = 0;
        int       reports      = 0;
        for (;;) {
            std::this_thread::sleep_for(milliseconds(250));
            if (!s_stallWatchdog.load() || reports >= 12) continue;
            const long long lastMs = s_lastPresentMs.load();
            if (!lastMs || !s_worldReady.load()) continue;   // nothing to watch yet
            const long long now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
            const long long gap = now - lastMs;
            if (gap < s_stallThresholdMs.load()) continue;
            if (now - lastReportMs < 5000) continue;
            lastReportMs = now;
            ++reports;
            const DWORD presentTid = s_lastPresentTid.load();
            const DWORD mainTid    = s_mainThreadId;
            SKSE::log::warn("Magelight stall: sample {} — uiMode={} view={} (present thread {}, main thread {})",
                            reports, s_focused.load(), s_uiModeView.load(), presentTid, mainTid);
            if (presentTid) LogThreadStack(presentTid, "present", gap);
            if (mainTid && mainTid != presentTid) LogThreadStack(mainTid, "main", gap);
            // Once per session: every other thread too. A frame stuck on a lock names the waiter; these name the
            // owner (an SKSE task drain on a VR job thread, a mod's worker).
            if (reports == 1) LogOtherThreadStacks(presentTid, mainTid, gap);
        }
    }

    void InstallCrashTelemetry()
    {
        AddVectoredExceptionHandler(1, &VectoredLogger);
        SKSE::log::info("Magelight: vectored exception telemetry armed");
        std::thread(&StallWatchdogLoop).detach();
        SKSE::log::info("Magelight: stall watchdog armed (threshold {} ms; Magelight.json \"stallWatchdog\": false disables)",
                        s_stallThresholdMs.load());
    }

    bool PreloadRuntime()
    {
        // SKSEPluginLoad runs on the game's MAIN thread — the same thread that
        // presents gameplay frames (menus/loading present from a separate
        // render thread). Recorded here so init can target it deterministically.
        s_mainThreadId = GetCurrentThreadId();
        SKSE::log::info("Magelight: main thread {}", s_mainThreadId);

        s_runtimeDir = GameRoot() / L"Data" / L"SKSE" / L"Plugins" / L"Magelight";
        // NAMESPACED runtime (coexistence with other Ultralight-based UI mods,
        // field-found 2026-08-31): another such host can ship the same four
        // Ultralight module names, and Windows binds imports by BASE NAME —
        // whoever loads "Ultralight.dll" first poisons the other host with the
        // wrong build (SKSE loads plugins alphabetically, so Magelight's preload
        // broke the other host's load). Our copies are renamed at stage time and
        // every import-name string inside them is patched to match (same-length
        // PE edit — see build.ps1), so both Ultralight runtimes coexist under
        // different module names.
        // Dependency order matters: core first, WebCore before Ultralight.
        const wchar_t* dlls[] = { L"Magelight1Core.dll", L"MgWCore.dll",
                                  L"Magelight1.dll", L"MgACore.dll" };
        for (const auto* name : dlls) {
            const auto full = s_runtimeDir / name;
            if (!LoadLibraryW(full.c_str())) {
                SKSE::log::error("Magelight: failed to load {} (GetLastError={})",
                    std::filesystem::path(name).string(), GetLastError());
                return false;
            }
        }
        if (!std::filesystem::exists(s_runtimeDir / L"resources" / L"icudt67l.dat")) {
            SKSE::log::error("Magelight: resources/icudt67l.dat missing under {}",
                s_runtimeDir.string());
            return false;
        }

        // Optional GPU backend. Loaded AFTER the runtime so its (namespaced)
        // Ultralight imports bind to the modules already in the process.
        // Missing/failed = CPU surface fallback, never fatal.
        if (HMODULE gpu = LoadLibraryW((s_runtimeDir / L"MagelightGPU.dll").c_str())) {
            s_gpuCreate    = reinterpret_cast<MgGpuCreateFn>(GetProcAddress(gpu, "MgGpu_Create"));
            s_gpuDestroy   = reinterpret_cast<MgGpuDestroyFn>(GetProcAddress(gpu, "MgGpu_Destroy"));
            s_gpuGetDriver = reinterpret_cast<MgGpuGetDrvFn>(GetProcAddress(gpu, "MgGpu_GetGPUDriver"));
            s_gpuHas       = reinterpret_cast<MgGpuHasFn>(GetProcAddress(gpu, "MgGpu_HasCommandsPending"));
            s_gpuDraw      = reinterpret_cast<MgGpuDrawFn>(GetProcAddress(gpu, "MgGpu_DrawCommandList"));
            s_gpuSrv       = reinterpret_cast<MgGpuSrvFn>(GetProcAddress(gpu, "MgGpu_GetTextureSRV"));
            s_gpuRegExt    = reinterpret_cast<MgGpuRegExtFn>(GetProcAddress(gpu, "MgGpu_RegisterExternalTexture"));
            s_gpuSetExtSrv = reinterpret_cast<MgGpuSetExtFn>(GetProcAddress(gpu, "MgGpu_SetExternalTextureSRV"));
            s_gpuUnregExt  = reinterpret_cast<MgGpuUnregExtFn>(GetProcAddress(gpu, "MgGpu_UnregisterExternalTexture"));
            if (!(s_gpuRegExt && s_gpuSetExtSrv && s_gpuUnregExt)) {
                s_gpuRegExt = nullptr;  // all-or-nothing: texture images unsupported
                SKSE::log::warn("Magelight: GPU backend predates external textures — texture images unavailable");
            }
            if (s_gpuCreate && s_gpuDestroy && s_gpuGetDriver && s_gpuHas && s_gpuDraw && s_gpuSrv) {
                SKSE::log::info("Magelight: GPU backend loaded (MagelightGPU.dll)");
            } else {
                SKSE::log::error("Magelight: MagelightGPU.dll is missing exports — CPU fallback");
                s_gpuCreate = nullptr;
            }
        } else {
            SKSE::log::info("Magelight: no GPU backend (MagelightGPU.dll not found, GetLastError={}) — CPU fallback",
                GetLastError());
        }

        s_runtimeReady.store(true);
        SKSE::log::info("Magelight: Ultralight runtime preloaded from {}", s_runtimeDir.string());
        return true;
    }

    // ── Ultralight platform bridges ─────────────────────────────────────────

    class SpdLogger final : public ultralight::Logger {
    public:
        void LogMessage(ultralight::LogLevel, const ultralight::String& message) override
        {
            SKSE::log::info("[UL] {}", message.utf8().data());
        }
    };
    static SpdLogger s_ulLogger;

    // Our own FileSystem instead of AppCore's: the stock Windows one resolves
    // MIME types from the REGISTRY, where .js is frequently text/plain or
    // absent — and MODULE scripts hard-require a JavaScript MIME (classic
    // scripts don't care), so a Vite bundle loads as a fully transparent,
    // silently-empty page (the 0.5.1 field report). A fixed extension table
    // removes the machine-dependence entirely.
    // Win32 clipboard for WebKit's editing commands (Ctrl+C/V/X, context
    // paste). Ultralight never sets one itself — without it every text field
    // silently ignores paste. Called on the render thread; OpenClipboard is
    // cheap and synchronous. Pages can only READ it through a user-initiated
    // paste (this WebKit build has no JS clipboard API).
    class MlClipboard final : public ultralight::Clipboard {
    public:
        void Clear() override
        {
            if (!OpenClipboard(s_hwnd)) return;
            EmptyClipboard();
            CloseClipboard();
        }
        ultralight::String ReadPlainText() override
        {
            ultralight::String out;
            if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(s_hwnd)) return out;
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                if (auto* w = static_cast<const wchar_t*>(GlobalLock(h))) {
                    const std::size_t len = wcsnlen(w, GlobalSize(h) / sizeof(wchar_t));
                    ultralight::String16 s16(reinterpret_cast<const ultralight::Char16*>(w), len);
                    out = ultralight::String(s16);
                    GlobalUnlock(h);
                }
            }
            CloseClipboard();
            return out;
        }
        void WritePlainText(const ultralight::String& text) override
        {
            const ultralight::String16 s16 = text.utf16();
            const std::size_t len = s16.length();
            if (!OpenClipboard(s_hwnd)) return;
            EmptyClipboard();
            if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (len + 1) * sizeof(wchar_t))) {
                if (auto* dst = static_cast<wchar_t*>(GlobalLock(h))) {
                    std::memcpy(dst, s16.data(), len * sizeof(wchar_t));
                    dst[len] = L'\0';
                    GlobalUnlock(h);
                    if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
                } else {
                    GlobalFree(h);
                }
            }
            CloseClipboard();
        }
    };
    static MlClipboard s_clipboard;

    // Network sandbox: a page's reach is its view's NetLevel, the owning mod's
    // NetworkPolicy (File unless the mod opted in). Tightening a default later
    // breaks mods, which is why the default is the tightest level.
    // "http://127.0.0.1:8080/x", "http://localhost/x", "http://[::1]:1/x" -> true.
    // 127.0.0.0/8 as a DOTTED QUAD only: a "127." prefix test let the legal
    // hostname 127.attacker.example through the sandbox (review 2026-09-09).
    static bool IsLoopbackV4(const std::string& host)
    {
        int octets = 0, value = 0, digits = 0;
        for (std::size_t i = 0; i <= host.size(); ++i) {
            const char c = i < host.size() ? host[i] : '.';
            if (c >= '0' && c <= '9') { if (++digits > 3) return false; value = value * 10 + (c - '0'); continue; }
            if (c != '.' || digits == 0 || value > 255) return false;
            if (octets == 0 && value != 127) return false;
            ++octets; value = 0; digits = 0;
        }
        return octets == 4;
    }
    static bool IsLoopbackUrl(const std::string& url)
    {
        const auto p = url.find("://");
        if (p == std::string::npos) return false;
        std::string host = url.substr(p + 3);
        const auto end = host.find_first_of("/?#");
        if (end != std::string::npos) host.resize(end);
        if (const auto at = host.rfind('@'); at != std::string::npos) host.erase(0, at + 1);
        if (!host.empty() && host[0] == '[') {
            const auto rb = host.find(']');
            host = (rb == std::string::npos) ? host : host.substr(0, rb + 1);
        } else if (const auto c = host.rfind(':'); c != std::string::npos) {
            host.resize(c);
        }
        for (auto& ch : host) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return host == "localhost" || host == "[::1]" || IsLoopbackV4(host);
    }

    // OnNetworkRequest runs on a WebKit thread: it reads the view registry
    // only under s_viewsMutex.
    class MlNetworkListener final : public ultralight::NetworkListener {
    public:
        bool OnNetworkRequest(ultralight::View* caller, ultralight::NetworkRequest& req) override
        {
            const std::string proto = req.urlProtocol().utf8().data();
            if (proto == "data" || proto == "about" || proto == "blob") return true;
            if (proto == "file") {
                // Host pinning: a page reads its OWN mod folder and the host
                // runtime dir (resources, cursor, host pages), nothing else —
                // no other mod's folder, no traversal out of Data, no
                // arbitrary drive path. The idea is MeridianUI's mod:// scheme;
                // Ultralight has no custom schemes, so the pin lives on the
                // request instead of the URL (0.26.4).
                std::filesystem::path root;
                ViewId id = 0;
                {
                    std::lock_guard<std::mutex> lk(s_viewsMutex);
                    if (MlView* v = FindViewByUlLocked(caller)) { root = v->root; id = v->id; }
                }
                const std::filesystem::path target = ResolveFileUrl(req.url().utf8().data());
                const bool ok = !target.empty() &&
                    (PathIsUnder(target, s_runtimeDir) || (!root.empty() && PathIsUnder(target, root)));
                if (ok) return true;
                static std::atomic<int> s_pinLogged{ 0 };
                if (s_pinLogged.fetch_add(1) < 32) {
                    SKSE::log::warn("Magelight: view {} refused file read outside its folder — {} (root '{}')",
                        id, target.string(), root.string());
                }
                return false;
            }
            // Beyond files, the owning mod decides (SetNetworkPolicy or the
            // manifest's "network"): Loopback lets a page talk to a server on
            // this machine, which can itself relay anywhere, so it is an
            // opt-in too; Any is for pages that talk to the internet, where a
            // host allowlist would break with every new host.
            NetLevel level = NetLevel::File;
            ViewId id = 0;
            {
                std::lock_guard<std::mutex> lk(s_viewsMutex);
                if (const MlView* v = FindViewByUlLocked(caller)) { level = v->netLevel; id = v->id; }
            }
            if (level == NetLevel::Any) return true;
            if (level == NetLevel::Loopback && (proto == "http" || proto == "https") &&
                IsLoopbackUrl(req.url().utf8().data()))
                return true;
            static std::atomic<int> s_logged{ 0 };
            if (s_logged.fetch_add(1) < 32) {
                SKSE::log::warn("Magelight: view {} blocked network request {} {} (protocol '{}') — its mod's network policy is {}",
                    id, req.httpMethod().utf8().data(), req.url().utf8().data(), proto,
                    level == NetLevel::Loopback ? "loopback (http(s) to this machine only)" : "file only");
            }
            return false;
        }
    };
    static MlNetworkListener s_networkListener;

    // Page roots outside Data\Magelight (an absolute page pins to its own
    // directory — PageRootFor). Append-only and never pruned: a root is a
    // directory, and keeping a destroyed view's folder readable costs
    // nothing, whereas pruning would race a worker-thread read against the
    // destroy. Own mutex, not s_viewsMutex: MlFileSystem::Resolve runs on
    // WebKit's worker threads while the game thread may hold s_viewsMutex.
    static std::mutex s_pageRootsMutex;
    static std::vector<std::filesystem::path> s_pageRoots;
    static void NotePageRoot(const std::filesystem::path& root)
    {
        if (root.empty()) return;
        std::lock_guard<std::mutex> lk(s_pageRootsMutex);
        for (const auto& r : s_pageRoots) if (r == root) return;
        s_pageRoots.push_back(root);
    }
    static bool IsUnderRegisteredPageRoot(const std::filesystem::path& p)
    {
        std::lock_guard<std::mutex> lk(s_pageRootsMutex);
        for (const auto& r : s_pageRoots) if (PathIsUnder(p, r)) return true;
        return false;
    }

    class MlFileSystem final : public ultralight::FileSystem {
    public:
        // Minimal percent-decode: WebKit encodes spaces (and friends) in
        // file:/// URLs, and game installs live under paths like
        // "Program Files (x86)".
        static std::string UrlDecode(const std::string& in)
        {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            std::string out;
            out.reserve(in.size());
            for (std::size_t i = 0; i < in.size(); ++i) {
                if (in[i] == '%' && i + 2 < in.size()) {
                    const int hi = hex(in[i + 1]), lo = hex(in[i + 2]);
                    if (hi >= 0 && lo >= 0) {
                        out += static_cast<char>((hi << 4) | lo);
                        i += 2;
                        continue;
                    }
                }
                out += in[i];
            }
            return out;
        }

        // file:///-URL → filesystem path. Beyond the demo's needs, the SA
        // port adds three shapes: QUERY STRINGS (?v=N cache-busters on
        // images the native side rewrites in place — stat/open must ignore
        // them), percent-encoding, and ABSOLUTE paths (SA's HTML ships in
        // SA's own mod folder; a drive-letter path resolves as-is instead
        // of against our runtime dir).
        std::filesystem::path Resolve(const ultralight::String& file_path)
        {
            std::string p = file_path.utf8().data();
            if (const auto q = p.find('?'); q != std::string::npos) p.resize(q);
            if (const auto h = p.find('#'); h != std::string::npos) p.resize(h);
            if (p.rfind("file:///", 0) == 0) p = p.substr(8);
            p = UrlDecode(p);
            while (!p.empty() && (p.front() == '/' || p.front() == '\\')) p.erase(p.begin());
            std::filesystem::path out = (p.size() > 1 && p[1] == ':')
                ? std::filesystem::path(p).make_preferred()
                : (s_runtimeDir / std::filesystem::path(p).make_preferred());
            // Defence in depth under the per-view pin: nothing outside the
            // host runtime dir, Data\Magelight or a registered page's own
            // root is ever served, and ".." is normalised away before the
            // test so it cannot climb out. This layer is view-blind (WebKit
            // asks for files from a worker thread with no View in hand), so
            // it accepts the UNION of page roots — the per-view pin in
            // MlNetworkListener is what keeps one page out of another's
            // folder. 0.28.1: the union used to be just Data\Magelight,
            // which refused every absolute page living elsewhere even though
            // the pin above had already accepted it — SkyrimNet's dashboard
            // under a folder outside Data\Magelight loaded as an empty view.
            out = out.lexically_normal();
            static const std::filesystem::path s_modsRoot = (GameRoot() / L"Data" / L"Magelight").lexically_normal();
            if (PathIsUnder(out, s_runtimeDir) || PathIsUnder(out, s_modsRoot) || IsUnderRegisteredPageRoot(out)) return out;
            static std::atomic<int> s_logged{ 0 };
            if (s_logged.fetch_add(1) < 32)
                SKSE::log::warn("Magelight: file read outside Data\\Magelight or any page root refused — {}", out.string());
            return {};
        }
        bool FileExists(const ultralight::String& file_path) override
        {
            const auto p = Resolve(file_path);
            if (p.empty()) return false;
            std::error_code ec;
            return std::filesystem::is_regular_file(p, ec);
        }
        ultralight::String GetFileMimeType(const ultralight::String& file_path) override
        {
            std::string ext = Resolve(file_path).extension().string();
            for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext == ".html" || ext == ".htm") return "text/html";
            if (ext == ".js" || ext == ".mjs") return "application/javascript";
            if (ext == ".css") return "text/css";
            if (ext == ".json" || ext == ".map") return "application/json";
            if (ext == ".svg") return "image/svg+xml";
            if (ext == ".png") return "image/png";
            if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
            if (ext == ".gif") return "image/gif";
            if (ext == ".webp") return "image/webp";
            if (ext == ".ico") return "image/x-icon";
            if (ext == ".woff") return "font/woff";
            if (ext == ".woff2") return "font/woff2";
            if (ext == ".ttf") return "font/ttf";
            if (ext == ".otf") return "font/otf";
            if (ext == ".txt") return "text/plain";
            if (ext == ".wasm") return "application/wasm";
            return "application/octet-stream";
        }
        ultralight::String GetFileCharset(const ultralight::String&) override
        {
            return "utf-8";
        }
        ultralight::RefPtr<ultralight::Buffer> OpenFile(const ultralight::String& file_path) override
        {
            const auto path = Resolve(file_path);
            if (path.empty()) return nullptr;   // refused by the pin (already logged)
            std::ifstream f(path, std::ios::binary | std::ios::ate);
            if (!f) {
                SKSE::log::warn("Magelight: OpenFile failed: {}", path.string());
                return nullptr;
            }
            const std::streamsize size = f.tellg();
            f.seekg(0, std::ios::beg);
            std::vector<char> data(static_cast<size_t>(size));
            if (size > 0 && !f.read(data.data(), size)) {
                SKSE::log::warn("Magelight: OpenFile read failed: {}", path.string());
                return nullptr;
            }
            return ultralight::Buffer::CreateFromCopy(data.data(), data.size());
        }
    };
    static MlFileSystem s_fileSystem;

    static std::filesystem::path ResolveFileUrl(const std::string& url)
    {
        return s_fileSystem.Resolve(ultralight::String(url.c_str()));
    }

    // Case-insensitive prefix test on normalised native paths (Windows). Pure
    // string work — safe on WebKit's thread, no filesystem calls.
    static bool PathIsUnder(const std::filesystem::path& p, const std::filesystem::path& base)
    {
        if (p.empty() || base.empty()) return false;
        std::wstring a = p.lexically_normal().native();
        std::wstring b = base.lexically_normal().native();
        while (!b.empty() && (b.back() == L'\\' || b.back() == L'/')) b.pop_back();
        if (a.size() < b.size()) return false;
        for (std::size_t i = 0; i < b.size(); ++i)
            if (std::towlower(a[i]) != std::towlower(b[i])) return false;
        return a.size() == b.size() || a[b.size()] == L'\\' || a[b.size()] == L'/';
    }

    // The folder a page may read from (MlView::root). A drive-letter page
    // under Data\Magelight\<Mod>\... pins to that mod folder; any other
    // absolute page pins to its own directory; a relative (host) page pins
    // to the runtime dir. Host resources are always readable on top.
    static std::filesystem::path PageRootFor(const std::string& htmlPath)
    {
        const bool absolute = htmlPath.size() > 1 && htmlPath[1] == ':';
        if (!absolute) return s_runtimeDir;
        const std::filesystem::path page = std::filesystem::path(htmlPath).lexically_normal();
        const std::filesystem::path modsRoot = (GameRoot() / L"Data" / L"Magelight").lexically_normal();
        if (PathIsUnder(page, modsRoot)) {
            // modsRoot / <Mod> — the first component below Data\Magelight
            const std::filesystem::path rel = page.lexically_relative(modsRoot);
            auto it = rel.begin();
            if (it != rel.end()) return modsRoot / *it;
        }
        return page.parent_path();
    }

    // Page console output into Magelight.log — a JS error, refused module, or
    // missing asset names itself instead of rendering as a blank panel.
    // 1.4: the virtual collapsed from 7 args to (View*, const ConsoleMessage&).
    // Without `override` the old signature would still compile as an unrelated
    // function and console logging would silently vanish — keep `override` on
    // every listener method, always. (MessageLevel/MessageSource also
    // RENUMBERED in 1.4 — compare by enum NAME only, never by stored int.)
    static ultralight::RefPtr<ultralight::View> HostInspectorView(ultralight::View* caller);

    static std::string MlJsonQuote(const std::string& in)
    {
        return JsonQuote(in);   // shared with the Api4 layer (Magelight.h)
    }

    // devMode: a JS error paints a banner on the page itself, so a broken
    // handler is seen where it happened instead of only in the log. Fixed,
    // click-through, self-removing; the page's own DOM is untouched.
    static void ShowDevErrorOverlay(ViewId view, const std::string& text, const std::string& source, std::uint32_t line)
    {
        std::string where = source;
        if (auto slash = where.find_last_of("/\\"); slash != std::string::npos) where = where.substr(slash + 1);
        const std::string msg = text + "\n" + where + ":" + std::to_string(line);
        InvokeJS(view,
            "(function(){var t=" + MlJsonQuote(msg) + ";var d=document.getElementById('__ml_err');"
            "if(!d){d=document.createElement('div');d.id='__ml_err';d.style.cssText='position:fixed;left:0;right:0;top:0;"
            "z-index:2147483647;background:#5a1414;color:#ffd9d9;font:12px/1.4 monospace;padding:6px 10px;"
            "border-bottom:2px solid #ff5c5c;white-space:pre-wrap;max-height:40%;overflow:auto;pointer-events:none';"
            "(document.body||document.documentElement).appendChild(d);}"
            "d.textContent='[Magelight devMode] JS error\\n'+t;clearTimeout(d.__t);"
            "d.__t=setTimeout(function(){d.remove();},12000);})();");
    }

    class MlViewListener final : public ultralight::ViewListener {
    public:
        // Render thread, from inside a view's update: the CSS cursor under the pointer.
        void OnChangeCursor(ultralight::View* caller, ultralight::Cursor cursor) override
        {
            // try_lock: no path is known to fire this under s_viewsMutex, but the core is closed, and a missed cursor
            // change only waits for the next mouse move.
            PageCursor kind = PageCursor::Arrow;
            switch (cursor) {
            case ultralight::kCursor_Hand:
            case ultralight::kCursor_Grab:
            case ultralight::kCursor_Grabbing:
                kind = PageCursor::Clickable;
                break;
            case ultralight::kCursor_IBeam:
            case ultralight::kCursor_VerticalText:
                kind = PageCursor::Text;
                break;
            default:
                break;
            }
            std::unique_lock<std::mutex> lk(s_viewsMutex, std::try_to_lock);
            if (!lk) return;
            if (MlView* v = FindViewByUlLocked(caller)) v->pageCursor = static_cast<int>(kind);
        }
        // Render thread, synchronously inside View::CreateLocalInspectorView.
        ultralight::RefPtr<ultralight::View> OnCreateInspectorView(ultralight::View* caller, bool,
                                                                   const ultralight::String&) override
        {
            return HostInspectorView(caller);
        }
    public:
        void OnAddConsoleMessage(ultralight::View* caller,
            const ultralight::ConsoleMessage& msg) override
        {
            ViewId id = 0;
            {
                std::lock_guard<std::mutex> lk(s_viewsMutex);
                if (MlView* v = FindViewByUlLocked(caller)) id = v->id;
            }
            const auto level = msg.level();
            const char* lvl = (level == ultralight::kMessageLevel_Error) ? "ERROR"
                : (level == ultralight::kMessageLevel_Warning) ? "warn" : "log";
            // Every console argument, not just the first: console.log('x', obj)
            // used to reach the log as "x". Objects come out as JSON.
            std::string text;
            if (JSContextRef ctx = msg.num_arguments() ? msg.argument_context() : nullptr) {
                for (std::uint32_t i = 0; i < msg.num_arguments(); ++i) {
                    JSValueRef v = msg.argument_at(i);
                    JSStringRef js = nullptr;
                    if (v && JSValueIsObject(ctx, v)) js = JSValueCreateJSONString(ctx, v, 0, nullptr);
                    if (!js && v) js = JSValueToStringCopy(ctx, v, nullptr);
                    if (i) text += ' ';
                    text += js ? JSStringToStd(js) : "undefined";
                    if (js) JSStringRelease(js);
                }
            } else {
                text = msg.message().utf8().data();
            }
            const std::string source = msg.source_id().utf8().data();
            SKSE::log::info("Magelight: [view {} console/{}] {} ({}:{})", id, lvl, text, source, msg.line_number());
            const int lvlNum = (level == ultralight::kMessageLevel_Error) ? 2
                : (level == ultralight::kMessageLevel_Warning) ? 1 : 0;
            Emit(HostEvent::ConsoleMessage, id, lvlNum, static_cast<int>(msg.line_number()), text.c_str());
            if (lvlNum == 2 && s_devMode.load() && id) ShowDevErrorOverlay(id, text, source, msg.line_number());
        }
    };
    static MlViewListener s_viewListener;

    // ── D3D helpers ─────────────────────────────────────────────────────────

    static constexpr const char* kShaderSrc = R"HLSL(
struct VSIn  { float2 pos : POS; float2 uv : TEX; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs_main(VSIn i) { VSOut o; o.pos = float4(i.pos, 0.0, 1.0); o.uv = i.uv; return o; }
Texture2D    tex0 : register(t0);
SamplerState smp0 : register(s0);
cbuffer Cut : register(b0) { float4 cut; };   // uv rect (x0,y0,x1,y1); x1<=x0 = none
float4 ps_main(VSOut i) : SV_Target {
    if (cut.z > cut.x && i.uv.x >= cut.x && i.uv.x < cut.z && i.uv.y >= cut.y && i.uv.y < cut.w) discard;
    return tex0.Sample(smp0, i.uv);
}
// VR copy pass: Ultralight's targets are PREMULTIPLIED; an OpenVR overlay
// texture is composited as straight alpha (SteamVR's default without
// VROverlayFlags_IsPremultiplied, which IVROverlay_016 predates). Divide out.
float4 ps_straight(VSOut i) : SV_Target {
    if (cut.z > cut.x && i.uv.x >= cut.x && i.uv.x < cut.z && i.uv.y >= cut.y && i.uv.y < cut.w) discard;
    float4 c = tex0.Sample(smp0, i.uv);
    if (c.a > 0.0001) c.rgb /= c.a;
    return c;
}
)HLSL";

    struct Vertex { float x, y, u, v; };

    // Classic arrow, 12x19: 'X' = black outline, 'o' = white fill, '.' =
    // transparent. Baked into an immutable premultiplied-BGRA texture and
    // drawn as the topmost quad at the MenuCursor position (hotspot 0,0).
    static constexpr int kCursorW = 12;
    static constexpr int kCursorH = 19;
    static constexpr const char* kCursorMask[kCursorH] = {
        "X...........",
        "XX..........",
        "XoX.........",
        "XooX........",
        "XoooX.......",
        "XooooX......",
        "XoooooX.....",
        "XooooooX....",
        "XoooooooX...",
        "XooooooooX..",
        "XoooooXXXXX.",
        "XooXooX.....",
        "XoX.XooX....",
        "XX..XooX....",
        "X....XooX...",
        ".....XooX...",
        "......XooX..",
        "......XooX..",
        ".......XX...",
    };

    static bool CreateCursorTexture()
    {
        std::uint32_t px[kCursorW * kCursorH];
        for (int y = 0; y < kCursorH; ++y) {
            for (int x = 0; x < kCursorW; ++x) {
                const char c = kCursorMask[y][x];
                px[y * kCursorW + x] =
                    (c == 'X') ? 0xFF000000u : (c == 'o') ? 0xFFFFFFFFu : 0u;
            }
        }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = kCursorW; td.Height = kCursorH;
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sub{ px, kCursorW * sizeof(std::uint32_t), 0 };
        if (FAILED(s_device->CreateTexture2D(&td, &sub, &s_cursorTex))) return false;
        return SUCCEEDED(s_device->CreateShaderResourceView(s_cursorTex, nullptr, &s_cursorSrv));
    }

    // Decode a PNG (any WIC-readable image) into a premultiplied-BGRA
    // immutable texture — the same encoding the baked arrow and every
    // Ultralight surface use, so the compositor's ONE/INV_SRC_ALPHA blend
    // applies unchanged. Capped at 512² (a cursor, not a wallpaper).
    // The VR laser-end pointer: a small white disc with a dark rim, alpha
    // antialiased, hotspot at the centre. An arrow made sense as a desktop
    // cursor; at the end of a laser it read as a big distracting sprite
    // (field 2026-09-06). 32x32 so it stays crisp when drawn at 8-16px.
    static constexpr int kDotSize = 32;
    static bool CreateDotTexture()
    {
        std::uint32_t px[kDotSize * kDotSize];
        const float c = (kDotSize - 1) * 0.5f, rCore = 9.0f, rRim = 12.0f;
        for (int y = 0; y < kDotSize; ++y) {
            for (int x = 0; x < kDotSize; ++x) {
                const float d = std::sqrt((x - c) * (x - c) + (y - c) * (y - c));
                float a = std::clamp(rRim + 0.5f - d, 0.0f, 1.0f);          // outer edge, antialiased
                const float core = std::clamp(rCore + 0.5f - d, 0.0f, 1.0f);  // white core over the dark rim
                const std::uint8_t A = static_cast<std::uint8_t>(a * 255.0f + 0.5f);
                const std::uint8_t V = static_cast<std::uint8_t>(core * 255.0f + 0.5f);   // 0 = rim (black), 255 = core
                px[y * kDotSize + x] = (static_cast<std::uint32_t>(A) << 24) | (V << 16) | (V << 8) | V;
            }
        }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = kDotSize; td.Height = kDotSize;
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sub{ px, kDotSize * sizeof(std::uint32_t), 0 };
        if (FAILED(s_device->CreateTexture2D(&td, &sub, &s_dotTex))) return false;
        return SUCCEEDED(s_device->CreateShaderResourceView(s_dotTex, nullptr, &s_dotSrv));
    }

    static bool LoadCursorImage(const std::filesystem::path& path,
                                ID3D11Texture2D** outTex, ID3D11ShaderResourceView** outSrv,
                                int& outW, int& outH)
    {
        using Microsoft::WRL::ComPtr;
        // The game already initialized COM on this (main) thread; S_FALSE /
        // RPC_E_CHANGED_MODE are both fine and we never uninitialize.
        (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ComPtr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory))))
            return false;
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnLoad, &decoder)))
            return false;
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, &frame))) return false;
        ComPtr<IWICFormatConverter> conv;
        if (FAILED(factory->CreateFormatConverter(&conv))) return false;
        if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                    WICBitmapDitherTypeNone, nullptr, 0.0,
                                    WICBitmapPaletteTypeCustom)))
            return false;
        UINT w = 0, h = 0;
        if (FAILED(conv->GetSize(&w, &h)) || !w || !h || w > 512 || h > 512) return false;
        std::vector<std::uint8_t> px(static_cast<size_t>(w) * h * 4);
        if (FAILED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(px.size()), px.data())))
            return false;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h;
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sub{ px.data(), w * 4, 0 };
        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(s_device->CreateTexture2D(&td, &sub, &tex))) return false;
        ComPtr<ID3D11ShaderResourceView> srv;
        if (FAILED(s_device->CreateShaderResourceView(tex.Get(), nullptr, &srv))) return false;
        *outTex = tex.Detach();
        *outSrv = srv.Detach();
        outW = static_cast<int>(w);
        outH = static_cast<int>(h);
        return true;
    }

    // "cursorFile": an image replaces the drawn cursor when it loads.
    static void TryLoadCustomCursor()
    {
        if (s_cursorFile.empty()) return;
        const bool absolute = s_cursorFile.size() > 1 && s_cursorFile[1] == ':';
        const std::filesystem::path p = absolute
            ? std::filesystem::path(s_cursorFile)
            : (s_runtimeDir / std::filesystem::path(s_cursorFile).make_preferred());
        std::error_code ec;
        if (!std::filesystem::exists(p, ec)) {
            SKSE::log::info("Magelight: no cursor art at {} — the drawn cursor stays", p.string());
            return;
        }
        ID3D11Texture2D* tex = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        int w = 0, h = 0;
        if (!LoadCursorImage(p, &tex, &srv, w, h)) {
            SKSE::log::warn("Magelight: cursor art {} failed to load — the drawn cursor stays", p.string());
            return;
        }
        if (s_cursorSrv) s_cursorSrv->Release();
        if (s_cursorTex) s_cursorTex->Release();
        s_cursorTex = tex;
        s_cursorSrv = srv;
        s_cursorImgW = w;
        s_cursorImgH = h;
        s_cursorCustom = true;
        SKSE::log::info("Magelight: cursor art {} ({}x{}, hotspot {:.2f},{:.2f}, height {}px@1080p)",
            p.string(), w, h, s_cursorHotX, s_cursorHotY, s_cursorHeight);
    }

    // ── The drawn cursor (MagelightCursorArt.h) ─────────────────────────────
    // Built on the render thread at the pixel height it is drawn at, and rebuilt when that height changes (a
    // resolution change). kArtGlowSteps arrow textures share one size and hotspot: the hover glow fades by stepping
    // through them, with no blend state of its own.
    static constexpr int kArtGlowSteps = 4;
    struct CursorArtTex
    {
        ID3D11Texture2D*          tex = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        int w = 0, h = 0;
        float hotX = 0, hotY = 0;
    };
    static CursorArtTex s_artArrow[kArtGlowSteps];
    static CursorArtTex s_artIBeam;
    static int          s_artHeight = 0;     // the pixel height the textures were built for; 0 = none
    static bool         s_artFailed = false; // a texture creation failed: the baked arrow draws instead

    static bool UploadCursorArt(const CursorArt::Image& img, CursorArtTex& out)
    {
        if (out.srv) { out.srv->Release(); out.srv = nullptr; }
        if (out.tex) { out.tex->Release(); out.tex = nullptr; }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(img.w); td.Height = static_cast<UINT>(img.h);
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sub{ img.px.data(), static_cast<UINT>(img.w) * sizeof(std::uint32_t), 0 };
        if (FAILED(s_device->CreateTexture2D(&td, &sub, &out.tex))) return false;
        if (FAILED(s_device->CreateShaderResourceView(out.tex, nullptr, &out.srv))) {
            out.tex->Release();
            out.tex = nullptr;
            return false;
        }
        out.w = img.w; out.h = img.h; out.hotX = img.hotX; out.hotY = img.hotY;
        return true;
    }

    // Render thread. True when the drawn cursor is ready at `height` pixels.
    static bool EnsureCursorArt(int height)
    {
        if (s_artFailed || !s_device) return false;
        if (height == s_artHeight) return true;
        bool ok = UploadCursorArt(CursorArt::IBeam(height), s_artIBeam);
        for (int i = 0; ok && i < kArtGlowSteps; ++i)
            ok = UploadCursorArt(CursorArt::Arrow(height, static_cast<float>(i) / (kArtGlowSteps - 1)), s_artArrow[i]);
        if (!ok) {
            s_artFailed = true;
            SKSE::log::error("Magelight: drawn cursor - texture creation failed at {}px; the baked arrow draws instead", height);
            return false;
        }
        s_artHeight = height;
        SKSE::log::info("Magelight: drawn cursor built at {}px", height);
        return true;
    }

    // The device the game renders with (BSGraphics renderer data), for a swapchain that will not hand one out.
    static ID3D11Device* EngineDevice()
    {
        auto* data = RE::BSGraphics::Renderer::GetRendererDataSingleton();
        return data ? reinterpret_cast<ID3D11Device*>(data->forwarder) : nullptr;
    }

    // Logs a failed device-resource step and records it as the render-death reason.
    static bool DeviceStepFailed(const char* step, HRESULT hr)
    {
        char detail[64];
        std::snprintf(detail, sizeof(detail), "HRESULT 0x%08X", static_cast<unsigned>(hr));
        SKSE::log::error("Magelight: {} failed (0x{:08X})", step, static_cast<unsigned>(hr));
        NoteRenderFailure(step, detail);
        return false;
    }

    static bool CreateDeviceResources(IDXGISwapChain* sc)
    {
        // The swapchain first: on every setup that works it answers, and the device it gives is the one its
        // back buffer lives on. The engine's device only stands in when it refuses (a Streamline wrapper over a
        // Direct3D 12 frame-generation swapchain answers E_NOINTERFACE). Never Released: a wrapper may not AddRef.
        ID3D11Device* dev = nullptr;
        const HRESULT devHr = sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev));
        if (SUCCEEDED(devHr) && dev) {
            s_device = dev;
        } else if (ID3D11Device* engine = EngineDevice()) {
            engine->AddRef();
            s_device = engine;
            s_deviceFromEngine.store(true);
            s_untrustedChain.store(true);
            s_lateComposite.store(false);
            SKSE::log::warn("Magelight: the game swapchain refused its device (0x{:08X}) - using the engine's device",
                static_cast<unsigned>(devHr));
        } else {
            return DeviceStepFailed("the game swapchain refused its device and the engine has none", devHr);
        }
        s_device->GetImmediateContext(&s_context);
        SKSE::log::info("Magelight: device from the {}", s_deviceFromEngine.load() ? "engine" : "swapchain");

        ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
        if (FAILED(D3DCompile(kShaderSrc, std::strlen(kShaderSrc), nullptr, nullptr, nullptr,
                "vs_main", "vs_4_0", 0, 0, &vsb, &err))) {
            SKSE::log::error("Magelight: VS compile failed: {}",
                err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            if (err) err->Release();
            NoteRenderFailure("the vertex shader did not compile");
            return false;
        }
        if (FAILED(D3DCompile(kShaderSrc, std::strlen(kShaderSrc), nullptr, nullptr, nullptr,
                "ps_main", "ps_4_0", 0, 0, &psb, &err))) {
            SKSE::log::error("Magelight: PS compile failed: {}",
                err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            if (err) err->Release();
            vsb->Release();
            NoteRenderFailure("the pixel shader did not compile");
            return false;
        }
        HRESULT hr = s_device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &s_vs);
        bool ok = SUCCEEDED(hr) || DeviceStepFailed("CreateVertexShader", hr);
        if (ok) {
            hr = s_device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &s_ps);
            ok = SUCCEEDED(hr) || DeviceStepFailed("CreatePixelShader", hr);
        }
        // The straight-alpha variant is only needed by the VR presenter; a
        // compile failure here degrades to the premultiplied copy, never to
        // a dead overlay.
        {
            ID3DBlob *sb = nullptr, *serr = nullptr;
            if (SUCCEEDED(D3DCompile(kShaderSrc, std::strlen(kShaderSrc), nullptr, nullptr, nullptr,
                    "ps_straight", "ps_4_0", 0, 0, &sb, &serr))) {
                s_device->CreatePixelShader(sb->GetBufferPointer(), sb->GetBufferSize(), nullptr, &s_psStraight);
                sb->Release();
            } else {
                SKSE::log::warn("Magelight: ps_straight compile failed: {}",
                    serr ? static_cast<const char*>(serr->GetBufferPointer()) : "?");
            }
            if (serr) serr->Release();
        }
        if (ok) {
            const D3D11_INPUT_ELEMENT_DESC il[] = {
                { "POS", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEX", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            };
            hr = s_device->CreateInputLayout(il, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &s_layout);
            ok = SUCCEEDED(hr) || DeviceStepFailed("CreateInputLayout", hr);
        }
        vsb->Release();
        psb->Release();
        if (!ok) return false;

        // CPU-surface textures are per-view now, created lazily on first
        // upload (see EnsureCpuTexture).
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(Vertex) * 4;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(hr = s_device->CreateBuffer(&bd, nullptr, &s_vb))) return DeviceStepFailed("CreateBuffer (vertices)", hr);
        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = 16;
        cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(hr = s_device->CreateBuffer(&cbd, nullptr, &s_cb))) return DeviceStepFailed("CreateBuffer (constants)", hr);

        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        if (FAILED(hr = s_device->CreateSamplerState(&sd, &s_sampler))) return DeviceStepFailed("CreateSamplerState", hr);

        // Ultralight surfaces are premultiplied BGRA: ONE / INV_SRC_ALPHA.
        D3D11_BLEND_DESC bld{};
        bld.RenderTarget[0].BlendEnable = TRUE;
        bld.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(hr = s_device->CreateBlendState(&bld, &s_blend))) return DeviceStepFailed("CreateBlendState", hr);
        // Straight-alpha 'over' for compositing INTO the VR copy target (whose
        // pixels are straight after ps_straight): rgb = src*a + dst*(1-a).
        {
            D3D11_BLEND_DESC sb = bld;
            sb.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
            sb.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            if (FAILED(s_device->CreateBlendState(&sb, &s_blendStraight))) s_blendStraight = nullptr;
        }

        if (!CreateCursorTexture()) {
            NoteRenderFailure("the cursor texture could not be created");
            return false;
        }
        if (!CreateDotTexture()) SKSE::log::warn("Magelight: VR pointer dot texture failed — the arrow art stands in");
        TryLoadCustomCursor();  // optional; keeps the drawn cursor on any failure
        return true;
    }

    static bool CreateUltralight()
    {
        ultralight::Config cfg;
        // resource_path_prefix defaults to "resources/" resolved against the
        // platform file system's base dir — our runtime dir carries the ICU
        // data + cacert there.
        //
        // cache_path: without one, WebKit keeps localStorage in memory and
        // every session forgets it. SA's frontend stores real user data
        // there (margin notes, UI scale, pins), so park it beside the SKSE
        // logs — a writable, mod-manager-free location. Failure = empty
        // path = the old in-memory behavior, never fatal.
        if (const auto cache = CacheDirPath(); !cache.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(cache, ec);
            if (!ec) {
                const auto u8 = cache.u8string();
                cfg.cache_path = ultralight::String(reinterpret_cast<const char*>(u8.c_str()));
            }
        }
        cfg.font_hinting = s_fontHinting == "smooth"     ? ultralight::FontHinting::Smooth
                         : s_fontHinting == "monochrome" ? ultralight::FontHinting::Monochrome
                         : s_fontHinting == "none"       ? ultralight::FontHinting::None
                                                         : ultralight::FontHinting::Normal;
        cfg.font_gamma = s_fontGamma;
        SKSE::log::info("Magelight: font hinting '{}', gamma {:.2f}", s_fontHinting, s_fontGamma);
        ultralight::Platform::instance().set_config(cfg);
        ultralight::Platform::instance().set_font_loader(ultralight::GetPlatformFontLoader());
        // Our MlFileSystem, not AppCore's registry-MIME one — see the class
        // comment (module scripts silently refuse a non-JS MIME).
        ultralight::Platform::instance().set_file_system(&s_fileSystem);
        ultralight::Platform::instance().set_logger(&s_ulLogger);
        ultralight::Platform::instance().set_clipboard(&s_clipboard);

        // Milestone 2: GPU rasterization. The driver must be registered BEFORE
        // Renderer::Create. Created against the GAME's device/context (both
        // fetched from the swapchain in CreateDeviceResources). Any failure
        // falls back to the CPU surface path.
        if (s_forceCpu.load()) {
            SKSE::log::info("Magelight: forceCpu set — GPU driver skipped, CPU rasterizer in use");
        } else if (s_gpuCreate && s_device && s_context) {
            s_gpu = s_gpuCreate(s_device, s_context, &GpuLogBridge);
            if (s_gpu) {
                if (auto* drv = static_cast<ultralight::GPUDriver*>(s_gpuGetDriver(s_gpu))) {
                    ultralight::Platform::instance().set_gpu_driver(drv);
                    s_gpuActive = true;
                }
            }
            if (!s_gpuActive) {
                SKSE::log::error("Magelight: GPU driver creation failed — CPU fallback");
            }
        }

        s_ulRenderer = ultralight::Renderer::Create();
        if (!s_ulRenderer) {
            SKSE::log::error("Magelight: the Ultralight renderer could not be created");
            NoteRenderFailure("the Ultralight renderer could not be created");
            return false;
        }
        SKSE::log::info("Magelight: renderer created ({} path)", s_gpuActive ? "GPU" : "CPU");
        return true;
    }

    // Absolute path → file:/// URL: forward slashes + the minimal percent
    // set (space/%/#/?) so game-root paths like Program Files (x86) survive
    // WebKit's URL parser. Inverse of MlFileSystem::UrlDecode.
    static std::string PathToFileUrl(const std::filesystem::path& p)
    {
        std::string s = p.string();
        for (auto& c : s)
            if (c == '\\') c = '/';
        std::string out = "file:///";
        for (const char c : s) {
            switch (c) {
            case ' ': out += "%20"; break;
            case '%': out += "%25"; break;
            case '#': out += "%23"; break;
            case '?': out += "%3F"; break;
            default:  out += c;
            }
        }
        return out;
    }

    // Render thread, per frame: give every registry entry that lacks one an
    // Ultralight view. Views registered from the game thread before the
    // renderer existed materialize here on the correct (owning) thread.
    // Render thread. Persistent Ultralight Sessions by name — one per mod
    // (localStorage/IndexedDB/cookies isolated under cache_path/<name>).
    static std::map<std::string, ultralight::RefPtr<ultralight::Session>> s_sessions;
    static ultralight::RefPtr<ultralight::Session> GetOrCreateSession(const std::string& name)
    {
        if (name.empty()) return nullptr;   // the renderer's default session
        auto it = s_sessions.find(name);
        if (it != s_sessions.end()) return it->second;
        auto session = s_ulRenderer->CreateSession(true, name.c_str());
        if (session) SKSE::log::info("Magelight: session '{}' created (persistent, isolated storage)", name);
        else SKSE::log::error("Magelight: session '{}' could not be created — falling back to default", name);
        s_sessions[name] = session;
        return session;
    }

    static void MaterializeViews()
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        for (auto& vp : s_views) {
            MlView& v = *vp;
            if (v.ul || v.w <= 0 || v.h <= 0) continue;
            if (v.dormant && !v.visible) continue;   // hibernated: wakes on ShowView(true)
            const bool waking = v.dormant;
            v.dormant = false;
            v.domReady = false;
            ultralight::ViewConfig vc;
            vc.is_accelerated = s_gpuActive;
            vc.is_transparent = true;
            vc.initial_device_scale = v.deviceScale;
            v.pageCursor = 0;   // a new View has reported nothing yet
            v.ul = s_ulRenderer->CreateView(
                static_cast<std::uint32_t>(v.w), static_cast<std::uint32_t>(v.h), vc,
                GetOrCreateSession(v.sessionName));
            if (!v.ul) {
                SKSE::log::error("Magelight: CreateView failed for view {}", v.id);
                continue;
            }
            v.ul->set_load_listener(&s_loadListener);
            v.ul->set_view_listener(&s_viewListener);
            v.ul->set_network_listener(&s_networkListener);
            // Prefer the on-disk page (the path the React frontend ships
            // through). Relative paths resolve against s_runtimeDir via the
            // platform file system; a drive-letter path loads as-is (SA's
            // HTML lives in SA's own mod folder). Inline banner fallback.
            const bool absolute = v.htmlPath.size() > 1 && v.htmlPath[1] == ':';
            const std::filesystem::path onDisk =
                absolute ? std::filesystem::path(v.htmlPath) : (s_runtimeDir / v.htmlPath);
            if (!v.htmlPath.empty() && std::filesystem::exists(onDisk)) {
                SKSE::log::info("Magelight: view {} loading from disk ({})", v.id, v.htmlPath);
                const std::string url =
                    absolute ? PathToFileUrl(onDisk) : ("file:///" + v.htmlPath);
                v.ul->LoadURL(url.c_str());
            } else {
                SKSE::log::warn("Magelight: view {} page '{}' missing — inline banner fallback",
                    v.id, v.htmlPath);
                v.ul->LoadHTML(kBannerHTML);
            }
            if (waking) {
                SKSE::log::info("Magelight: view {} woke from hibernation", v.id);
                // If it is the UI-mode target, the focus marker ran against
                // a null View — re-run it so keys reach the new page.
                if (s_focused.load() && v.id == static_cast<ViewId>(s_uiModeView.load())) QueueInput(0, 1, 0);
            }
        }
    }

    // Render thread, before MaterializeViews: release the View + texture of
    // any view hidden longer than its hibernate budget. The record stays;
    // everything the page needs to come back (path, listeners, session,
    // layer, cutout) is on it. Never the UI-mode view, never an inspector.
    static void HibernateIdleViews()
    {
        const std::uint64_t now = GetTickCount64();
        std::vector<ultralight::RefPtr<ultralight::View>> released;
        {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            const ViewId uiView = static_cast<ViewId>(s_uiModeView.load());
            for (auto& vp : s_views) {
                MlView& v = *vp;
                if (!v.ul || v.visible || !v.hibernateMs || !v.hiddenSince || v.destroyPending || v.isInspector) continue;
                if (v.id == uiView && s_focused.load()) continue;
                if (now - v.hiddenSince < v.hibernateMs) continue;
                // overlay + host copy go BEFORE the texture (lifetime rule);
                // hibernating=true keeps the view's controller binding (0.28.4)
                VR::ReleaseView(v.id, /*hibernating=*/true);
                if (v.srv) { v.srv->Release(); v.srv = nullptr; }
                if (v.tex) { v.tex->Release(); v.tex = nullptr; }
                v.texW = v.texH = 0;
                v.dormant = true;
                v.domReady = false;
                released.push_back(std::move(v.ul));
                v.ul = nullptr;
                SKSE::log::info("Magelight: view {} hibernated after {} ms hidden (texture released)", v.id, v.hibernateMs);
            }
        }
        released.clear();   // WebKit teardown outside the lock, like a destroy
    }

    // Render thread: apply a pending SetViewBounds resize.
    static void ApplyPendingResizes()
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        for (auto& vp : s_views) {
            MlView& v = *vp;
            if (!v.ul || !v.boundsDirty) continue;
            v.boundsDirty = false;
            if (v.scaleDirty) {   // 0.26.9 SetViewScale, applied here on the render thread
                v.scaleDirty = false;
                v.ul->set_device_scale(v.deviceScale > 0.f ? v.deviceScale : 1.f);
            }
            if (v.w > 0 && v.h > 0) {
                v.ul->Resize(static_cast<std::uint32_t>(v.w), static_cast<std::uint32_t>(v.h));
                // CPU-path texture no longer matches — recreate lazily.
                if (v.srv) { v.srv->Release(); v.srv = nullptr; }
                if (v.tex) { v.tex->Release(); v.tex = nullptr; }
                v.texW = v.texH = 0;
            }
        }
    }

    // Render thread: reloads, navigations and destroys queued by the API.
    // Destroy erases the registry entry and releases the Ultralight view -
    // the listeners key on View*, so erase BEFORE the RefPtr drops (WebKit
    // may recycle the address). Runs outside Update()/Render(), where no
    // listener can be mid-call.
    static void ApplyPendingLifecycle()
    {
        // Collect under the lock, ACT OUTSIDE IT: LoadURL/Reload on a live
        // page and dropping a View's last RefPtr run WebKit synchronously
        // (beforeunload/unload handlers, cancelled provisional loads), which
        // re-enters the console / load-failed listeners — and those take
        // s_viewsMutex. Same shape as DrainBridgeQueues.
        struct Work { ultralight::RefPtr<ultralight::View> ul; ViewId id; std::string url; bool reload; };
        std::vector<Work> work;
        std::vector<std::pair<ViewId, ultralight::RefPtr<ultralight::View>>> destroyed;
        // EvalJS requests still queued on a view being destroyed: their fn is
        // the EvalCtx-owning trampoline, so if we drop them the EvalCtx leaks
        // and the mod's callback never fires. Flush them (with a failure)
        // OUTSIDE the lock (review 2026-09-05, finding 2).
        std::vector<std::pair<ViewId, PendingEval>> orphanEvals;
        {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            for (auto it = s_views.begin(); it != s_views.end();) {
                MlView& v = **it;
                if (v.destroyPending) {
                    const ViewId id = v.id;
                    VR::ReleaseView(id);   // overlay destroyed before the View RefPtr drops
                    if (v.srv) { v.srv->Release(); v.srv = nullptr; }
                    if (v.tex) { v.tex->Release(); v.tex = nullptr; }
                    for (auto& e : v.evals) orphanEvals.emplace_back(id, std::move(e));
                    v.evals.clear();
                    destroyed.emplace_back(id, std::move(v.ul));   // released after the erase, outside the lock
                    it = s_views.erase(it);
                    continue;
                }
                if (v.ul && !v.navigateUrl.empty()) {
                    work.push_back({ v.ul, v.id, v.navigateUrl, false });
                    v.htmlPath = v.navigateUrl.rfind("file:///", 0) == 0 ? v.navigateUrl.substr(8) : v.navigateUrl;
                    v.navigateUrl.clear();
                    v.reloadedFlag = true;
                } else if (v.ul && v.reloadPending) {
                    work.push_back({ v.ul, v.id, {}, true });
                    v.reloadedFlag = true;
                }
                v.reloadPending = false;
                ++it;
            }
        }
        for (auto& w : work) {
            if (w.reload) {
                SKSE::log::info("Magelight: view {} reloading", w.id);
                w.ul->Reload();
            } else {
                SKSE::log::info("Magelight: view {} navigating to {}", w.id, w.url);
                w.ul->LoadURL(w.url.c_str());
            }
        }
        for (auto& [id, e] : orphanEvals)
            if (e.fn) e.fn(id, "", "Magelight: view destroyed before its eval ran", e.user);
        for (auto& [id, ul] : destroyed) {
            ul = nullptr;   // the registry entry is already gone; a recycled View* can't alias a live view
            SKSE::log::info("Magelight: view {} destroyed", id);
            Emit(HostEvent::ViewDestroyed, id);
        }
    }

    // ── Frame work (render thread) ──────────────────────────────────────────

    // Modifier state via GetAsyncKeyState — the render thread has no message
    // queue, so thread-local GetKeyState would lie here.
    static unsigned CurrentKeyModifiers()
    {
        unsigned mods = 0;
        if (GetAsyncKeyState(VK_MENU) & 0x8000)    mods |= ultralight::KeyEvent::kMod_AltKey;
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= ultralight::KeyEvent::kMod_CtrlKey;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   mods |= ultralight::KeyEvent::kMod_ShiftKey;
        if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000)
            mods |= ultralight::KeyEvent::kMod_MetaKey;
        return mods;
    }

    // Translate queued Windows messages into Ultralight events. Render thread
    // only — the sole thread allowed to touch views.
    // Routing: keys/text -> the UI-mode (focused) view; mouse -> the topmost
    // visible non-clickThrough view under that event's coordinates, falling
    // back to the UI-mode view so drags that leave the panel keep working.
    static void DrainInputQueue(float bw, float bh)
    {
        std::vector<InputMsg> msgs;
        {
            std::lock_guard<std::mutex> lk(s_inputMutex);
            msgs.swap(s_inputQueue);
        }
        if (msgs.empty()) return;

        ultralight::RefPtr<ultralight::View> uiUl;
        int uiX = 0, uiY = 0;
        {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            if (MlView* v = FindViewLocked(static_cast<ViewId>(s_uiModeView.load()))) {
                uiUl = v->ul;
                float ex = 0, ey = 0;
                EffectivePos(*v, bw, bh, ex, ey);
                uiX = static_cast<int>(ex);
                uiY = static_cast<int>(ey);
            }
        }

        // Topmost visible interactive view containing (mx,my); falls back to
        // the UI-mode view. Outputs the target's screen origin.
        // Also hands back the target's device scale and wheel step (0.28.5):
        // one lock and one scan per event — two lambdas used to re-lock and
        // re-scan for what this loop already had in hand (review 2026-09-09).
        auto hitTest = [&](int mx, int my, int& tx, int& ty, float& ds, int& step) -> ultralight::RefPtr<ultralight::View> {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            auto take = [&](const MlView& v) {
                ds   = v.deviceScale > 0.f ? v.deviceScale : 1.f;
                step = v.scrollStep > 0 ? v.scrollStep : 40;   // 0.28.0 per-view wheel step
            };
            for (auto it = s_views.rbegin(); it != s_views.rend(); ++it) {
                MlView& v = **it;
                if (!v.ul || !v.visible || v.clickThrough) continue;
                float ex = 0, ey = 0;
                EffectivePos(v, bw, bh, ex, ey);
                if (mx >= ex && my >= ey && mx < ex + v.w && my < ey + v.h) {
                    tx = static_cast<int>(ex);
                    ty = static_cast<int>(ey);
                    take(v);
                    return v.ul;
                }
            }
            tx = uiX; ty = uiY;
            if (const MlView* u = FindViewLocked(static_cast<ViewId>(s_uiModeView.load()))) take(*u);
            return uiUl;
        };

        // Ultralight takes mouse coordinates in LOGICAL (CSS) pixels: AppCore
        // converts window pixels with its DPI scale before FireMouseEvent, so
        // a view rendered at deviceScale S needs (px / S). Backbuffer-space px
        // reach here from both the Win32 sink and the VR laser (it posts into
        // the same queue), so this one spot covers both. `ds` below is the
        // hit view's scale, from hitTest.

        using UL = ultralight::MouseEvent;
        for (const auto& e : msgs) {
            switch (e.msg) {
            case 0:  // focus marker from SetUIMode — targets the UI-mode view
                if (!uiUl) break;
                if (e.wparam) uiUl->Focus();
                else uiUl->Unfocus();
                SKSE::log::info("Magelight: view {}", e.wparam ? "focused" : "unfocused");
                break;
            case WM_MOUSEMOVE: {
                int tx = 0, ty = 0, step = 40; float ds = 1.f;
                auto target = hitTest(GET_X_LPARAM(e.lparam), GET_Y_LPARAM(e.lparam), tx, ty, ds, step);
                if (!target) break;
                UL evt{ UL::kType_MouseMoved,
                        static_cast<int>((GET_X_LPARAM(e.lparam) - tx) / ds), static_cast<int>((GET_Y_LPARAM(e.lparam) - ty) / ds),
                        UL::kButton_None };
                target->FireMouseEvent(evt);
                break;
            }
            case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: {
                const auto btn = (e.msg == WM_MBUTTONDOWN) ? UL::kButton_Middle
                    : (e.msg == WM_RBUTTONDOWN) ? UL::kButton_Right : UL::kButton_Left;
                int tx = 0, ty = 0, step = 40; float ds = 1.f;
                auto target = hitTest(GET_X_LPARAM(e.lparam), GET_Y_LPARAM(e.lparam), tx, ty, ds, step);
                if (!target) break;
                UL evt{ UL::kType_MouseDown,
                        static_cast<int>((GET_X_LPARAM(e.lparam) - tx) / ds), static_cast<int>((GET_Y_LPARAM(e.lparam) - ty) / ds), btn };
                target->FireMouseEvent(evt);
                break;
            }
            case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
                const auto btn = (e.msg == WM_MBUTTONUP) ? UL::kButton_Middle
                    : (e.msg == WM_RBUTTONUP) ? UL::kButton_Right : UL::kButton_Left;
                int tx = 0, ty = 0, step = 40; float ds = 1.f;
                auto target = hitTest(GET_X_LPARAM(e.lparam), GET_Y_LPARAM(e.lparam), tx, ty, ds, step);
                if (!target) break;
                UL evt{ UL::kType_MouseUp,
                        static_cast<int>((GET_X_LPARAM(e.lparam) - tx) / ds), static_cast<int>((GET_Y_LPARAM(e.lparam) - ty) / ds), btn };
                target->FireMouseEvent(evt);
                break;
            }
            case WM_MOUSEWHEEL: {
                // ~40 px per notch (WHEEL_DELTA = 120). Scrolls whatever sits
                // under the cursor, like a real browser.
                int tx = 0, ty = 0, step = 40; float ds = 1.f;
                auto target = hitTest(GET_X_LPARAM(e.lparam), GET_Y_LPARAM(e.lparam), tx, ty, ds, step);
                if (!target) break;
                const int delta = GET_WHEEL_DELTA_WPARAM(e.wparam) * step / WHEEL_DELTA;   // default 40px per notch
                ultralight::ScrollEvent evt{ ultralight::ScrollEvent::kType_ScrollByPixel, 0, delta };
                target->FireScrollEvent(evt);
                break;
            }
            // Key fields are populated manually (0.3.4) rather than via the
            // SDK's Windows KeyEvent ctor. (The 0.3.1-0.3.5 fastfail family
            // was the wrong-thread WebKit main-thread assert — see
            // s_worldReady — not the events themselves.)
            case WM_KEYDOWN: case WM_SYSKEYDOWN: {
                if (!uiUl) break;
                ultralight::KeyEvent evt;
                evt.type = ultralight::KeyEvent::kType_RawKeyDown;
                evt.virtual_key_code = static_cast<int>(e.wparam);
                evt.native_key_code = static_cast<int>((e.lparam >> 16) & 0x1FF);
                // key_identifier is what the page's e.key / e.keyIdentifier
                // reads for non-printing keys (arrows, Escape, Enter) —
                // without it they are "Unidentified" and focus navigation
                // in pages breaks.
                ultralight::GetKeyIdentifierFromVirtualKeyCode(evt.virtual_key_code, evt.key_identifier);
                evt.modifiers = CurrentKeyModifiers();
                evt.is_system_key = (e.msg == WM_SYSKEYDOWN);
                evt.is_auto_repeat = (e.lparam & (1 << 30)) != 0;
                evt.is_keypad = false;
                uiUl->FireKeyEvent(evt);
                if ((e.wparam == VK_BACK || e.wparam == VK_DELETE) && !evt.is_auto_repeat) {
                    static std::atomic<int> s_n{ 0 };
                    if (s_n.fetch_add(1) < 8)
                        SKSE::log::info("Magelight: RawKeyDown vk=0x{:02X} id='{}' fired at view {}",
                            evt.virtual_key_code, ultralight::String(evt.key_identifier).utf8().data(),
                            static_cast<std::uint64_t>(s_uiModeView.load()));
                }
                break;
            }
            case WM_KEYUP: case WM_SYSKEYUP: {
                if (!uiUl) break;
                ultralight::KeyEvent evt;
                evt.type = ultralight::KeyEvent::kType_KeyUp;
                evt.virtual_key_code = static_cast<int>(e.wparam);
                evt.native_key_code = static_cast<int>((e.lparam >> 16) & 0x1FF);
                ultralight::GetKeyIdentifierFromVirtualKeyCode(evt.virtual_key_code, evt.key_identifier);
                evt.modifiers = CurrentKeyModifiers();
                evt.is_system_key = (e.msg == WM_SYSKEYUP);
                evt.is_keypad = false;
                uiUl->FireKeyEvent(evt);
                break;
            }
            case WM_CHAR: {
                if (!uiUl) break;
                const ultralight::Char16 wch = static_cast<ultralight::Char16>(e.wparam);
                // Control characters (Ctrl+C = 0x03, Ctrl+V = 0x16, ...) are
                // editing commands already delivered by the RawKeyDown with
                // kMod_CtrlKey; as text they would insert garbage. Tab and
                // Enter stay text (textarea newline / tab).
                if (wch < 0x20 && wch != L'\t' && wch != L'\r' && wch != L'\n') break;
                // A character outside the BMP (emoji, rare ideographs) arrives as two
                // WM_CHARs; a lone half fired on its own is U+FFFD in the page. Hold the
                // high half and fire the pair together (0.27.0).
                static ultralight::Char16 s_pendingHigh = 0;
                ultralight::Char16 units[2]; unsigned n = 0;
                if (wch >= 0xD800 && wch <= 0xDBFF) { s_pendingHigh = wch; break; }
                if (wch >= 0xDC00 && wch <= 0xDFFF) { if (!s_pendingHigh) break; units[n++] = s_pendingHigh; s_pendingHigh = 0; }
                units[n++] = wch;
                ultralight::String16 s16(units, n);
                ultralight::KeyEvent evt;
                evt.type = ultralight::KeyEvent::kType_Char;
                evt.modifiers = CurrentKeyModifiers();
                evt.text = ultralight::String(s16);
                evt.unmodified_text = evt.text;
                uiUl->FireKeyEvent(evt);
                break;
            }
            default:
                break;
            }
        }
    }

    // CPU-surface path: (re)create the view's dynamic texture to match its
    // current size. Render thread.
    static bool EnsureCpuTexture(MlView& v)
    {
        if (v.tex && v.texW == v.w && v.texH == v.h) return true;
        if (v.srv) { v.srv->Release(); v.srv = nullptr; }
        if (v.tex) { v.tex->Release(); v.tex = nullptr; }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(v.w);
        td.Height = static_cast<UINT>(v.h);
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(s_device->CreateTexture2D(&td, nullptr, &v.tex))) return false;
        if (FAILED(s_device->CreateShaderResourceView(v.tex, nullptr, &v.srv))) {
            v.tex->Release(); v.tex = nullptr;
            return false;
        }
        v.texW = v.w; v.texH = v.h;
        return true;
    }

    static void UploadSurfaceIfDirty(MlView& v)
    {
        auto* surface = static_cast<ultralight::BitmapSurface*>(v.ul->surface());
        if (!surface) return;
        if (surface->dirty_bounds().IsEmpty()) return;
        if (!EnsureCpuTexture(v)) return;

        ultralight::RefPtr<ultralight::Bitmap> bmp = surface->bitmap();
        if (!bmp) return;
        const void* px = bmp->LockPixels();
        if (px) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(s_context->Map(v.tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                const auto rowBytes = bmp->row_bytes();
                const auto rows = bmp->height();
                const auto copyBytes = (rowBytes < mapped.RowPitch) ? rowBytes : mapped.RowPitch;
                auto* src = static_cast<const std::uint8_t*>(px);
                auto* dst = static_cast<std::uint8_t*>(mapped.pData);
                for (std::uint32_t y = 0; y < rows; ++y) {
                    std::memcpy(dst + y * mapped.RowPitch, src + y * rowBytes, copyBytes);
                }
                s_context->Unmap(v.tex, 0);
            }
        }
        bmp->UnlockPixels();
        surface->ClearDirtyBounds();
    }

    // Everything we touch gets saved and restored — the game's renderer must
    // never notice we were here.
    struct StateBackup {
        D3D11_PRIMITIVE_TOPOLOGY topo{};
        ID3D11InputLayout* layout = nullptr;
        ID3D11Buffer* vb = nullptr; UINT vbStride = 0, vbOffset = 0;
        ID3D11VertexShader* vs = nullptr;
        ID3D11PixelShader* ps = nullptr;
        ID3D11GeometryShader* gs = nullptr;
        ID3D11HullShader* hs = nullptr;
        ID3D11DomainShader* ds = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        ID3D11SamplerState* sampler = nullptr;
        ID3D11Buffer* psCb = nullptr;
        ID3D11BlendState* blend = nullptr; FLOAT blendFactor[4]{}; UINT sampleMask = 0;
        ID3D11DepthStencilState* depth = nullptr; UINT stencilRef = 0;
        ID3D11RasterizerState* rs = nullptr;
        ID3D11RenderTargetView* rtvs[8]{}; ID3D11DepthStencilView* dsv = nullptr;
        ID3D11ComputeShader* cs = nullptr;
        ID3D11Buffer* soTargets[4]{};
        ID3D11Predicate* predicate = nullptr; BOOL predicateValue = FALSE;
        UINT vpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};

        void Capture(ID3D11DeviceContext* c)
        {
            c->IAGetPrimitiveTopology(&topo);
            c->IAGetInputLayout(&layout);
            c->IAGetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
            c->VSGetShader(&vs, nullptr, nullptr);
            c->PSGetShader(&ps, nullptr, nullptr);
            c->GSGetShader(&gs, nullptr, nullptr);
            c->HSGetShader(&hs, nullptr, nullptr);
            c->DSGetShader(&ds, nullptr, nullptr);
            c->PSGetShaderResources(0, 1, &srv);
            c->PSGetSamplers(0, 1, &sampler);
            c->PSGetConstantBuffers(0, 1, &psCb);
            c->OMGetBlendState(&blend, blendFactor, &sampleMask);
            c->OMGetDepthStencilState(&depth, &stencilRef);
            c->RSGetState(&rs);
            c->OMGetRenderTargets(8, rtvs, &dsv);
            c->RSGetViewports(&vpCount, vps);
            c->CSGetShader(&cs, nullptr, nullptr);
            c->SOGetTargets(4, soTargets);
            c->GetPredication(&predicate, &predicateValue);
        }
        // Put the pipeline in the state Ultralight's reference driver takes
        // for granted (AppCore owns a pristine context; we borrow the game's
        // mid-frame). The driver rebinds VS/PS/IA/RT/blend/rasterizer per
        // draw but never touches GS/HS/DS, depth-stencil, stream-output or
        // predication — any of which the game may leave armed. Field
        // symptom (2026-09-01): path-tessellated borders (a box whose sides
        // differ) drew as huge wedge triangles in-game while the same page
        // rendered correctly through AppCore on the same SDK.
        static void Neutralize(ID3D11DeviceContext* c)
        {
            c->GSSetShader(nullptr, nullptr, 0);
            c->HSSetShader(nullptr, nullptr, 0);
            c->DSSetShader(nullptr, nullptr, 0);
            c->CSSetShader(nullptr, nullptr, 0);
            c->OMSetDepthStencilState(nullptr, 0);
            ID3D11Buffer* noSO[4] = {};
            UINT          soOff[4] = {};
            c->SOSetTargets(4, noSO, soOff);
            c->SetPredication(nullptr, FALSE);
        }
        void Restore(ID3D11DeviceContext* c)
        {
            c->IASetPrimitiveTopology(topo);
            c->IASetInputLayout(layout);
            c->IASetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
            c->VSSetShader(vs, nullptr, 0);
            c->PSSetShader(ps, nullptr, 0);
            c->GSSetShader(gs, nullptr, 0);
            c->HSSetShader(hs, nullptr, 0);
            c->DSSetShader(ds, nullptr, 0);
            c->PSSetShaderResources(0, 1, &srv);
            c->PSSetSamplers(0, 1, &sampler);
            c->PSSetConstantBuffers(0, 1, &psCb);
            c->OMSetBlendState(blend, blendFactor, sampleMask);
            c->OMSetDepthStencilState(depth, stencilRef);
            c->RSSetState(rs);
            c->CSSetShader(cs, nullptr, 0);
            UINT soOff[4] = {};
            c->SOSetTargets(4, soTargets, soOff);
            c->SetPredication(predicate, predicateValue);
            c->OMSetRenderTargets(8, rtvs, dsv);
            if (vpCount) c->RSSetViewports(vpCount, vps);
            // Release the refs the Get* calls added.
            if (layout) layout->Release();
            if (vb) vb->Release();
            if (vs) vs->Release();
            if (ps) ps->Release();
            if (gs) gs->Release();
            if (hs) hs->Release();
            if (ds) ds->Release();
            if (srv) srv->Release();
            if (sampler) sampler->Release();
            if (psCb) psCb->Release();
            if (blend) blend->Release();
            if (depth) depth->Release();
            if (rs) rs->Release();
            for (auto* r : rtvs) if (r) r->Release();
            if (dsv) dsv->Release();
            if (cs) cs->Release();
            for (auto* t : soTargets) if (t) t->Release();
            if (predicate) predicate->Release();
        }
    };

    // Composite one textured quad over the backbuffer. `srv` is the view's
    // pixels — the CPU path's uploaded texture (uv 0..1) or the GPU path's
    // render-target texture (uv from RenderTarget::uv_coords, since the target
    // may be padded). Pipeline state save/restore is the CALLER's job.
    static void DrawQuadTo(ID3D11RenderTargetView* rtv, float bw, float bh, ID3D11ShaderResourceView* srv,
                           float u0, float v0, float u1, float v1,
                           float dx, float dy, float dw, float dh,
                           const float* cutUV);

    // Whether a D3D object lives on the device we draw with (ours, or the engine's, which it stands in for).
    static bool OnOurDeviceObject(ID3D11DeviceChild* obj)
    {
        ID3D11Device* dev = nullptr;
        obj->GetDevice(&dev);
        if (!dev) return false;
        bool same = dev == s_device || dev == EngineDevice();
        if (!same && s_device) {
            IUnknown *a = nullptr, *b = nullptr;
            dev->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&a));
            s_device->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&b));
            same = a && a == b;
            if (a) a->Release();
            if (b) b->Release();
        }
        dev->Release();
        return same;
    }

    // The engine's framebuffer render target and its size, or null when it is missing or not on our device.
    // Engine-owned: never Released.
    static ID3D11RenderTargetView* EngineFramebufferRtv(float& w, float& h)
    {
        auto* data = RE::BSGraphics::Renderer::GetRendererDataSingleton();
        if (!data) return nullptr;
        const auto& fb = data->renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER];
        auto* rtv = reinterpret_cast<ID3D11RenderTargetView*>(fb.RTV);
        if (!rtv || !OnOurDeviceObject(rtv)) return nullptr;
        // The view's own resource, not fb.texture: a plugin may point the view elsewhere and leave the texture.
        ID3D11Resource* res = nullptr;
        rtv->GetResource(&res);
        ID3D11Texture2D* tex = nullptr;
        if (res) res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
        if (res) res->Release();
        if (!tex) return nullptr;
        D3D11_TEXTURE2D_DESC td{};
        tex->GetDesc(&td);
        tex->Release();
        if (!td.Width || !td.Height) return nullptr;
        w = static_cast<float>(td.Width);
        h = static_cast<float>(td.Height);
        return rtv;
    }

    // Whether GetBuffer added a reference to `first`: a second GetBuffer that raises the count by one did.
    // The second call's reference is dropped here when there is one. Measured once per swapchain (render thread):
    // a wrapper's GetBuffer does not change its semantics, and fewer reads leave less room for a race.
    static bool GetBufferGaveReference(IDXGISwapChain* sc, ID3D11Texture2D* first)
    {
        static IDXGISwapChain* s_probedChain = nullptr;
        static bool            s_probedOwned = false;
        if (sc == s_probedChain) return s_probedOwned;
        first->AddRef();
        const ULONG before = first->Release();
        ID3D11Texture2D* second = nullptr;
        if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&second))) || !second)
            return false;   // cannot tell: leaking a reference beats freeing the game's buffer
        bool owned = true;
        if (second != first) {
            second->Release();   // a fresh object per call is a fresh reference
        } else {
            second->AddRef();
            const ULONG after = second->Release();
            owned = after == before + 1;
            if (owned) second->Release();
        }
        s_probedChain = sc;
        s_probedOwned = owned;
        SKSE::log::info("Magelight: the swapchain's GetBuffer {} a reference", owned ? "adds" : "does not add");
        return owned;
    }

    static void WarnNothingToDrawOn(const char* why)
    {
        static std::atomic<bool> s_warned{ false };
        if (!s_warned.exchange(true))
            SKSE::log::warn("Magelight: nothing to draw the overlay on ({}) - pages run but are not shown", why);
    }

    static void DrawOverlay(IDXGISwapChain* sc, ID3D11ShaderResourceView* srv,
                            float u0, float v0, float u1, float v1,
                            float dx, float dy, float dw, float dh,
                            const float* cutUV = nullptr)   // {x0,y0,x1,y1} in this quad's uv space
    {
        DXGI_SWAP_CHAIN_DESC scd{};
        if (FAILED(sc->GetDesc(&scd))) return;
        const float bw = static_cast<float>(scd.BufferDesc.Width);
        const float bh = static_cast<float>(scd.BufferDesc.Height);
        if (bw <= 0.0f || bh <= 0.0f || !srv) return;
        const bool untrusted = s_untrustedChain.load();
        if (untrusted && !REL::Module::IsVR()) {
            // The engine's own framebuffer view: what the game's UI draws into this frame (Community Shaders points
            // it at its UI buffer while frame generation composites), so the overlay is UI to whatever presents it.
            float tw = 0.0f, th = 0.0f;
            if (ID3D11RenderTargetView* engineRtv = EngineFramebufferRtv(tw, th)) {
                const float sx = tw / bw, sy = th / bh;
                DrawQuadTo(engineRtv, tw, th, srv, u0, v0, u1, v1, dx * sx, dy * sy, dw * sx, dh * sy, cutUV);
                return;
            }
        }
        ID3D11Texture2D* back = nullptr;
        if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) {
            WarnNothingToDrawOn("the swapchain gave no back buffer");
            return;
        }
        // A wrapper may hand its buffer out without a reference (Community Shaders before 1.8.4): Release only
        // what was given.
        const bool owned = !untrusted || GetBufferGaveReference(sc, back);
        if (untrusted && !OnOurDeviceObject(back)) {
            if (owned) back->Release();
            WarnNothingToDrawOn("the back buffer is on another device");
            return;
        }
        ID3D11RenderTargetView* rtv = nullptr;
        const HRESULT rtvHr = s_device->CreateRenderTargetView(back, nullptr, &rtv);
        if (owned) back->Release();
        if (FAILED(rtvHr)) return;
        DrawQuadTo(rtv, bw, bh, srv, u0, v0, u1, v1, dx, dy, dw, dh, cutUV);
        rtv->Release();
    }

    // One textured quad into `rtv`, a bw x bh target. Pipeline state save/restore is the CALLER's job.
    static void DrawQuadTo(ID3D11RenderTargetView* rtv, float bw, float bh, ID3D11ShaderResourceView* srv,
                           float u0, float v0, float u1, float v1,
                           float dx, float dy, float dw, float dh,
                           const float* cutUV)
    {
        if (!rtv || bw <= 0.0f || bh <= 0.0f || !srv) return;

        // Quad in NDC at (dx, dy), dw x dh pixels.
        const float x0 = -1.0f + 2.0f * (dx / bw);
        const float y0 =  1.0f - 2.0f * (dy / bh);
        const float x1 = -1.0f + 2.0f * ((dx + dw) / bw);
        const float y1 =  1.0f - 2.0f * ((dy + dh) / bh);
        const Vertex quad[4] = {
            { x0, y0, u0, v0 },
            { x1, y0, u1, v0 },
            { x0, y1, u0, v1 },
            { x1, y1, u1, v1 },
        };
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(s_context->Map(s_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
        std::memcpy(mapped.pData, quad, sizeof(quad));
        s_context->Unmap(s_vb, 0);

        const UINT stride = sizeof(Vertex), offset = 0;
        const FLOAT bf[4] = { 0, 0, 0, 0 };
        D3D11_VIEWPORT vp{ 0, 0, bw, bh, 0.0f, 1.0f };
        s_context->OMSetRenderTargets(1, &rtv, nullptr);
        s_context->RSSetViewports(1, &vp);
        s_context->RSSetState(nullptr);
        s_context->OMSetBlendState(s_blend, bf, 0xFFFFFFFF);
        s_context->OMSetDepthStencilState(nullptr, 0);
        s_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        s_context->IASetInputLayout(s_layout);
        s_context->IASetVertexBuffers(0, 1, &s_vb, &stride, &offset);
        s_context->VSSetShader(s_vs, nullptr, 0);
        s_context->PSSetShader(s_ps, nullptr, 0);
        s_context->GSSetShader(nullptr, nullptr, 0);
        s_context->HSSetShader(nullptr, nullptr, 0);
        s_context->DSSetShader(nullptr, nullptr, 0);
        s_context->PSSetShaderResources(0, 1, &srv);
        s_context->PSSetSamplers(0, 1, &s_sampler);
        {
            const float none[4] = { 0, 0, 0, 0 };
            D3D11_MAPPED_SUBRESOURCE cm{};
            if (SUCCEEDED(s_context->Map(s_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &cm))) {
                std::memcpy(cm.pData, cutUV ? cutUV : none, sizeof(none));
                s_context->Unmap(s_cb, 0);
            }
            s_context->PSSetConstantBuffers(0, 1, &s_cb);
        }
        s_context->Draw(4, 0);
    }

    // VR copy pass (docs/VR_PRESENTER.md §2): draw one presented view into a
    // HOST-OWNED render target at 1:1 — resolves the driver target's uv
    // padding, applies the cutout, and (straightAlpha) divides the
    // premultiplied colour out. No blending: the target is cleared first and
    // the quad covers it. Brackets its own pipeline state; Ultralight thread.
    void CopyViewToTarget(const PresentedView& pv, ID3D11RenderTargetView* rtv, int w, int h, bool straightAlpha,
                          const CursorMark* marks, int markCount)
    {
        if (!rtv || !pv.srv || w <= 0 || h <= 0 || !s_context) return;
        StateBackup backup;
        backup.Capture(s_context);
        StateBackup::Neutralize(s_context);

        const float clear[4] = { 0, 0, 0, 0 };
        s_context->ClearRenderTargetView(rtv, clear);
        const Vertex quad[4] = {
            { -1.0f,  1.0f, pv.u0, pv.v0 },
            {  1.0f,  1.0f, pv.u1, pv.v0 },
            { -1.0f, -1.0f, pv.u0, pv.v1 },
            {  1.0f, -1.0f, pv.u1, pv.v1 },
        };
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(s_context->Map(s_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            std::memcpy(mapped.pData, quad, sizeof(quad));
            s_context->Unmap(s_vb, 0);
            const UINT stride = sizeof(Vertex), offset = 0;
            D3D11_VIEWPORT vp{ 0, 0, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f };
            s_context->OMSetRenderTargets(1, &rtv, nullptr);
            s_context->RSSetViewports(1, &vp);
            s_context->RSSetState(nullptr);
            s_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);   // overwrite, no blend
            s_context->OMSetDepthStencilState(nullptr, 0);
            s_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            s_context->IASetInputLayout(s_layout);
            s_context->IASetVertexBuffers(0, 1, &s_vb, &stride, &offset);
            s_context->VSSetShader(s_vs, nullptr, 0);
            s_context->PSSetShader((straightAlpha && s_psStraight) ? s_psStraight : s_ps, nullptr, 0);
            s_context->GSSetShader(nullptr, nullptr, 0);
            s_context->HSSetShader(nullptr, nullptr, 0);
            s_context->DSSetShader(nullptr, nullptr, 0);
            ID3D11ShaderResourceView* srv = pv.srv;
            s_context->PSSetShaderResources(0, 1, &srv);
            s_context->PSSetSamplers(0, 1, &s_sampler);
            const float none[4] = { 0, 0, 0, 0 };
            D3D11_MAPPED_SUBRESOURCE cm{};
            if (SUCCEEDED(s_context->Map(s_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &cm))) {
                std::memcpy(cm.pData, pv.hasCutout ? pv.cutUV : none, sizeof(none));
                s_context->Unmap(s_cb, 0);
            }
            s_context->PSSetConstantBuffers(0, 1, &s_cb);
            s_context->Draw(4, 0);

            // Cursor marks (VR laser hits): the host's own cursor sprite, hotspot
            // on the hit pixel, composited over the (now straight-alpha) page.
            if (marks && markCount > 0 && s_cursorSrv) {
                // VR pointer sprite: a FRACTION of the panel, not the flat
                // cursor's 1080p-relative size. The old 24px floor was ~10% of a
                // 240px panel and read as a huge arrow stuck on the beam.
                // The laser-end pointer: a centred dot by default (0.26.12),
                // the cursor art (arrow / custom PNG, its own hotspot) if
                // "cursorDot": false or the dot texture failed.
                const bool dot = VR::CursorDot() && s_dotSrv;
                const float ch = std::clamp(static_cast<float>(h) * VR::CursorScale(), dot ? 4.0f : 8.0f, 32.0f);
                float cw = ch, hotX = 0.0f, hotY = 0.0f;
                if (dot) {
                    hotX = hotY = 0.5f;
                } else if (s_cursorCustom && s_cursorImgH > 0) {
                    cw = ch * (static_cast<float>(s_cursorImgW) / s_cursorImgH);
                    hotX = s_cursorHotX; hotY = s_cursorHotY;
                } else {
                    cw = ch * (static_cast<float>(kCursorW) / kCursorH);
                }
                const FLOAT bf[4] = { 0, 0, 0, 0 };
                s_context->OMSetBlendState((straightAlpha && s_blendStraight) ? s_blendStraight : s_blend, bf, 0xFFFFFFFF);
                s_context->PSSetShaderResources(0, 1, dot ? &s_dotSrv : &s_cursorSrv);
                if (SUCCEEDED(s_context->Map(s_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &cm))) {
                    std::memcpy(cm.pData, none, sizeof(none));
                    s_context->Unmap(s_cb, 0);
                }
                for (int i = 0; i < markCount; ++i) {
                    const float px = marks[i].u * w - hotX * cw;
                    const float py = marks[i].v * h - hotY * ch;
                    const float x0 = -1.0f + 2.0f * (px / w), y0 = 1.0f - 2.0f * (py / h);
                    const float x1 = -1.0f + 2.0f * ((px + cw) / w), y1 = 1.0f - 2.0f * ((py + ch) / h);
                    const Vertex cq[4] = { { x0, y0, 0, 0 }, { x1, y0, 1, 0 }, { x0, y1, 0, 1 }, { x1, y1, 1, 1 } };
                    if (SUCCEEDED(s_context->Map(s_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                        std::memcpy(mapped.pData, cq, sizeof(cq));
                        s_context->Unmap(s_vb, 0);
                        s_context->Draw(4, 0);
                    }
                }
            }
            // Unbind the target so the driver never samples a bound RT.
            ID3D11RenderTargetView* nullRtv = nullptr;
            s_context->OMSetRenderTargets(1, &nullRtv, nullptr);
        }
        backup.Restore(s_context);
    }

    // ── Texture-backed images: render-thread materialization ────────────────
    // Runs every frame right before Renderer::Update(): registers pending
    // images with the driver + ImageSourceProvider, applies SRV re-points,
    // tombstones unregistered ones, and Invalidate()s dirty ones (Ultralight
    // only repaints an image that was invalidated).
    static void MaterializeImages()
    {
        if (!s_gpuActive || !s_gpuRegExt) return;
        std::lock_guard<std::mutex> lk(s_imagesMutex);
        for (auto& ip : s_images) {
            MlImage& im = *ip;
            if (im.removed) continue;
            if (!im.texId) {
                im.texId = s_gpuRegExt(s_gpu, im.srv);
                if (!im.texId) {
                    SKSE::log::error("Magelight: image '{}' — driver refused the external texture", im.name);
                    im.removed = true;
                    continue;
                }
                im.src = ultralight::ImageSource::CreateFromTexture(
                    im.w, im.h, im.texId, ultralight::Rect{ 0.0f, 0.0f, 1.0f, 1.0f });
                ultralight::ImageSourceProvider::instance().AddImageSource(im.name.c_str(), im.src);
                SKSE::log::info("Magelight: image '{}' registered (driver texture {}, {}x{}) -> {}",
                    im.name, im.texId, im.w, im.h, im.url);
            }
            if (im.pendingSrv) {
                if (s_gpuSetExtSrv(s_gpu, im.texId, im.pendingSrv)) {
                    im.srv->Release();
                    im.srv = im.pendingSrv;
                } else {
                    im.pendingSrv->Release();
                }
                im.pendingSrv = nullptr;
                im.dirty = true;
            }
            if (im.unregister) {
                ultralight::ImageSourceProvider::instance().RemoveImageSource(im.name.c_str());
                im.src = nullptr;
                // Tombstone: the driver entry and our SRV ref stay — a page
                // element may still resolve to this id, and a missing id leaves
                // whatever SRV was last bound in place.
                im.removed = true;
                SKSE::log::info("Magelight: image '{}' unregistered", im.name);
                continue;
            }
            if (im.dirty && im.src) {
                im.src->Invalidate();
                im.dirty = false;
            }
        }
    }

    // ── ImageSource probe (Magelight.json "imageProbe": true) ───────────────
    // A 512x512 dynamic texture the host repaints on the CPU every frame
    // (moving stripes + a red disc), registered as image "probe" and placed
    // eight ways by views/probe/index.html. Proves the whole chain without a
    // consumer: driver id, .imgsrc resolution, per-frame Invalidate, and
    // which CSS features the GPU path composites (clip/scroll/transform/
    // opacity) versus which fall back to a bitmap it doesn't have (filter,
    // canvas). Render thread.
    static ID3D11Texture2D*          s_probeTex = nullptr;
    static ID3D11ShaderResourceView* s_probeSrv = nullptr;
    static ImageId                   s_probeImage = 0;
    static std::uint32_t             s_probeFrame = 0;
    static constexpr std::uint32_t   kProbeSize = 512;

    static void TickImageProbe()
    {
        if (!s_imageProbe || !s_gpuActive || !s_device) return;
        if (!s_probeTex) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = kProbeSize;
            td.Height = kProbeSize;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DYNAMIC;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(s_device->CreateTexture2D(&td, nullptr, &s_probeTex)) ||
                FAILED(s_device->CreateShaderResourceView(s_probeTex, nullptr, &s_probeSrv))) {
                SKSE::log::error("Magelight: image probe — texture creation failed; probe disabled");
                s_imageProbe = false;
                return;
            }
            s_probeImage = RegisterTextureImage("probe", s_probeSrv, kProbeSize, kProbeSize);
            SKSE::log::info("Magelight: image probe texture queued as image {}", s_probeImage);
            if (!s_probeImage) {
                s_imageProbe = false;
                return;
            }
        }
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(s_context->Map(s_probeTex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            const std::uint32_t f = s_probeFrame++;
            const float cx = kProbeSize * (0.5f + 0.35f * std::cos(f * 0.020f));
            const float cy = kProbeSize * (0.5f + 0.35f * std::sin(f * 0.031f));
            for (std::uint32_t y = 0; y < kProbeSize; ++y) {
                auto* row = reinterpret_cast<std::uint32_t*>(
                    static_cast<std::uint8_t*>(m.pData) + y * m.RowPitch);
                for (std::uint32_t x = 0; x < kProbeSize; ++x) {
                    const bool stripe = (((x + y + f * 2) / 32) & 1) != 0;
                    std::uint32_t c = stripe ? 0xFF3A2D17u : 0xFFEFE3C8u;  // ink / parchment, opaque
                    const float dx = static_cast<float>(x) - cx;
                    const float dy = static_cast<float>(y) - cy;
                    if (dx * dx + dy * dy < 60.0f * 60.0f) c = 0xFFB33A2Fu;  // the red disc
                    row[x] = c;
                }
            }
            s_context->Unmap(s_probeTex, 0);
        }
        InvalidateImage(s_probeImage);
    }

    static void EnsureRenderInit(IDXGISwapChain* sc)
    {
        if (s_renderInit || s_renderDead.load()) return;
        s_renderInit = true;
        s_ulThreadId = GetCurrentThreadId();
        if (!CreateDeviceResources(sc) || !CreateUltralight()) {
            NoteRenderFailure("render-thread init failed");   // a no-op when a step already said why
            s_renderDead.store(true);
            SKSE::log::error("Magelight: render-thread init failed — overlay disabled");
            return;
        }
        SKSE::log::info("Magelight: render-thread init complete (owning thread {})", s_ulThreadId);
        s_rendererUp.store(true);
        // VR presenter (docs/VR_PRESENTER.md): same thread, after the device
        // resources exist. Not a VR runtime = dormant, one log line.
        VR::Init(s_device, s_context);
    }

    // SEH telemetry (0.3.1 diagnostics): a fault anywhere in our frame or
    // input path becomes a LOG LINE + graceful overlay shutdown instead of a
    // silent CTD (crash loggers routinely miss faults raised inside foreign
    // frames like a Present hook or a window proc).
    static LONG LogSehAndDisable(const char* where, unsigned long code, void* addr)
    {
        SKSE::log::error(
            "Magelight: SEH exception 0x{:08X} at {} in {} — overlay disabled, UI mode will drop",
            code, addr, where);
        NoteRenderFailure("an exception in the frame or input path");   // a static literal: no allocation in a handler
        s_renderDead.store(true);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    // ── Dev loop: Web Inspector ───────────────────────────────────────────
    // The inspector is an ordinary Ultralight view WE host: Ultralight asks
    // for one (OnCreateInspectorView) and loads file:///inspector/Main.html
    // from the FileSystem root, so the SDK's inspector/ folder ships under the
    // runtime dir. It takes UI mode while open (input routes to one view), so
    // the inspector toggle flips focus between the page and its inspector; the page keeps
    // running underneath, and the console can drive it.
    static ultralight::RefPtr<ultralight::View> HostInspectorView(ultralight::View* caller)
    {
        ViewId page = 0;
        std::filesystem::path pageRoot;
        {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            for (auto& vp : s_views) if (vp->ul && vp->ul.get() == caller) { page = vp->id; pageRoot = vp->root; break; }
        }
        int bw = 0, bh = 0;
        GetDisplaySize(bw, bh);
        if (bw <= 0 || bh <= 0 || !s_ulRenderer) return nullptr;
        ultralight::ViewConfig vc;
        vc.is_accelerated = s_gpuActive;
        vc.is_transparent = false;
        const int h = bh / 2;
        auto ul = s_ulRenderer->CreateView(static_cast<std::uint32_t>(bw), static_cast<std::uint32_t>(h), vc, nullptr);
        if (!ul) return nullptr;
        // Sandboxed like any page, at File level: the runtime dir (where
        // file:///inspector/ lives) and the inspected page's own folder.
        ul->set_network_listener(&s_networkListener);
        auto v = std::make_unique<MlView>();
        v->id = s_nextViewId.fetch_add(1);
        v->htmlPath = "<inspector>";
        v->root = pageRoot;
        v->isInspector = true;
        v->x = 0; v->y = bh - h; v->w = bw; v->h = h;
        v->visible = true;
        v->layer = 3;   // System
        v->order = s_nextViewOrder.fetch_add(1);
        v->ul = ul;
        const ViewId id = v->id;
        {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            s_views.push_back(std::move(v));
            SortViewsLocked();
        }
        s_inspectorOf[page] = id;
        s_inspectorVisibleFor.store(page);
        SKSE::log::info("Magelight[dev]: inspector view {} hosted for view {} ({}x{}, lower half)", id, page, bw, h);
        return ul;
    }

    // Render thread, after MaterializeViews (the page must have its View).
    static void ProcessInspectorRequest()
    {
        if (const ViewId hidePage = static_cast<ViewId>(s_inspectorHideRequest.exchange(0))) {   // 0.28.0 ShowInspector(false)
            if (auto it = s_inspectorOf.find(hidePage); it != s_inspectorOf.end()) {
                ShowView(it->second, false);
                SwitchUIModeView(hidePage);
                s_inspectorVisibleFor.store(0);
            }
        }
        const ViewId target = static_cast<ViewId>(s_inspectorRequest.exchange(0));
        if (!target) return;
        for (const auto& [page, insp] : s_inspectorOf) {
            if (insp != target) continue;
            // A toggle on the inspector itself: back to the page, inspector hidden.
            ShowView(insp, false);
            SwitchUIModeView(page);
            s_inspectorVisibleFor.store(0);
            SKSE::log::info("Magelight[dev]: inspector {} hidden — focus back to view {}", insp, page);
            return;
        }
        if (auto it = s_inspectorOf.find(target); it != s_inspectorOf.end() && IsViewValid(it->second)) {
            ShowView(it->second, true);
            SwitchUIModeView(it->second);
            s_inspectorVisibleFor.store(target);
            return;
        }
        if (!InspectorAvailable()) {
            SKSE::log::warn("Magelight[dev]: inspector assets missing — expected {}", (s_runtimeDir / "inspector" / "Main.html").string());
            return;
        }
        ultralight::RefPtr<ultralight::View> ul;
        {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            if (MlView* v = FindViewLocked(target)) ul = v->ul;
        }
        if (!ul) return;
        ul->CreateLocalInspectorView();   // synchronous: OnCreateInspectorView -> HostInspectorView
        if (auto it = s_inspectorOf.find(target); it != s_inspectorOf.end()) SwitchUIModeView(it->second);
    }

    static void FrameWork(IDXGISwapChain* sc)
    {
        // Diagnostic: name every present-thread change (bounded — a session
        // sees at most a couple). This is what proved the two-thread pattern.
        const DWORD tid = GetCurrentThreadId();
        static DWORD lastTid = 0;
        static int tidLogs = 0;
        if (tid != lastTid && tidLogs < 8) {
            SKSE::log::info("Magelight: present thread now {} (was {})", tid, lastTid);
            lastTid = tid;
            ++tidLogs;
        }

        // Init on the MAIN thread only. Menus and loading screens present from
        // a separate render thread; gameplay presents from the game's main
        // loop — in both field runs the in-game present TID equals the TID
        // that loaded the plugin (11664 in 0.3.6, 16452 in 0.3.7). Heuristics
        // failed twice here: kPostLoadGame fires while the loading screen is
        // still presenting (0.3.6), and that loading screen presents many
        // SECONDS of stable frames, so a stability streak passes on the wrong
        // thread too (0.3.7). Thread identity is the deterministic signal.
        if (!s_renderInit) {
            // World-ready FALLBACK (0.17.9): SKSEVR never delivered kNewGame for
            // an Alternate Perspective new game (field 2026-09-03: SKSE dispatched
            // 4 and 5 only; no 3, no 7), so the renderer never started and every
            // hotkey suspended the controls over nothing. The player standing in
            // a loaded cell with no main/loading menu up IS the world being
            // ready; the thread-identity gate below still decides WHEN to init.
            // Main-thread frames only (never the loading-screen thread), polled
            // about twice a second.
            if (!s_worldReady.load() && tid == s_mainThreadId) {
                static int s_wrPolls = 0;
                if ((++s_wrPolls % 30) == 0) {
                    auto* pc = RE::PlayerCharacter::GetSingleton();
                    auto* ui = RE::UI::GetSingleton();
                    if (pc && ui && pc->GetParentCell() && pc->Is3DLoaded() &&
                        !ui->IsMenuOpen(RE::MainMenu::MENU_NAME) && !ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) {
                        SKSE::log::info("Magelight: world ready (fallback: player in a loaded cell — no kPostLoadGame/kNewGame arrived)");
                        NotifyWorldReady();
                    }
                }
            }
            if (!s_worldReady.load() || tid != s_mainThreadId) return;
            EnsureRenderInit(sc);
        }
        if (s_renderDead.load()) return;
        if (tid != s_ulThreadId) {
            // A frame from a foreign thread (loading screens etc.): touching
            // Ultralight here is the fastfail. Skip the whole frame.
            static bool warned = false;
            if (!warned) {
                warned = true;
                SKSE::log::warn("Magelight: frame on foreign thread {} (owner {}) — skipping Ultralight work",
                    tid, s_ulThreadId);
            }
            return;
        }
        DXGI_SWAP_CHAIN_DESC scd{};
        float bw = 0, bh = 0;
        if (SUCCEEDED(sc->GetDesc(&scd))) {
            bw = static_cast<float>(scd.BufferDesc.Width);
            bh = static_cast<float>(scd.BufferDesc.Height);
            const int prevW = s_backbufferW.exchange(static_cast<int>(bw));  // for the cursor clamp (input sink)
            const int prevH = s_backbufferH.exchange(static_cast<int>(bh));
            if (prevW > 0 && prevH > 0 && (prevW != static_cast<int>(bw) || prevH != static_cast<int>(bh)))
                Emit(HostEvent::DisplayResized, 0, static_cast<int>(bw), static_cast<int>(bh));
        }

        // Fullscreen views track the backbuffer: sized here BEFORE
        // materialization (a 0x0 view never materializes), re-Resized via
        // the boundsDirty path if the backbuffer ever changes.
        if (bw > 0 && bh > 0) {
            // In VR the backbuffer is the desktop mirror window, whose aspect
            // is unrelated to the panel in the headset — take the configured
            // panel resolution there instead (see VR::Settings::panelWidth).
            int ibw = static_cast<int>(bw), ibh = static_cast<int>(bh);
            int panelW = 0, panelH = 0;
            if (VR::PanelSizeForFullscreen(panelW, panelH)) { ibw = panelW; ibh = panelH; }
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            for (auto& vp : s_views) {
                MlView& v = *vp;
                if (!v.fullscreen || (v.w == ibw && v.h == ibh)) continue;
                v.w = ibw;
                v.h = ibh;
                if (v.ul) v.boundsDirty = true;
            }
        }

        HibernateIdleViews();
        MaterializeViews();
        ApplyPendingResizes();
        Dev::Tick();                 // hot reload: queues ReloadView for changed folders
        ProcessInspectorRequest();   // inspector toggle (unbound for now)
        ApplyPendingLifecycle();
        // Listeners registered AFTER a page loaded (the OnDomReady pattern)
        // get their window shims here, a frame later — the listener shims
        // only; the page core stays as the page loaded it. Raw MlView*
        // snapshots are safe here because the registry is erased ONLY by
        // ApplyPendingLifecycle, on this thread,
        // which already ran this frame — nothing erases between the collect
        // and the install. InstallBridgeShims takes the lock itself, so
        // collect first, install outside.
        {
            std::vector<MlView*> dirty;
            {
                std::lock_guard<std::mutex> lk(s_viewsMutex);
                for (auto& vp : s_views)
                    if (vp->ul && vp->shimsDirty) dirty.push_back(vp.get());
            }
            for (MlView* v : dirty) InstallBridgeShims(*v, false);
        }
        DrainInputQueue(bw, bh);
        DrainBridgeQueues();
        // The CursorMenu's kShow processes a frame or two after SetUIMode, and its movie only exists once it has:
        // hide the vanilla cursor again just after and once the menu has settled.
        static int uiFrames = 0;
        if (s_focused.load()) {
            ++uiFrames;
            if (uiFrames == 3 || uiFrames == 30) {
                GameTask::Post([]() {
                    if (!s_focused.load()) return;   // an exit landed first and restored the cursor
                    HideVanillaCursor(true);
                });
            }
            // Every ~second: the menus we depend on are still up and the vanilla cursor still hidden (see
            // CursorMenuWatchdog). Game thread, cheap.
            if (uiFrames > 30 && (uiFrames % 60) == 0) {
                GameTask::Post([]() { CursorMenuWatchdog(); });
            }
        } else {
            uiFrames = 0;
        }
        // Texture images: repaint the probe (if on), then register/re-point/
        // invalidate — BEFORE Update() so this frame's Render() sees them.
        TickImageProbe();
        MaterializeImages();
        // ── Present-path stage timing ────────────────────────────────
        // Four rounds of reasoning about VR frame cost have now been wrong,
        // twice confidently. The submit gate is provably working (0% upload
        // on an idle panel) and the game still stalls when the UI is up, so
        // the cost is somewhere this has never measured. Time each stage and
        // print the worst frame once a second: cheap, and it ends the
        // guessing rather than adding to it.
        using ClockT = std::chrono::steady_clock;
        struct StageAcc {
            double ulUpdate = 0, ulRender = 0, composite = 0, vrSubmit = 0;
            double worst = 0;
            int    frames = 0;
            ClockT::time_point lastReport{};
        };
        static StageAcc s_acc;
        const auto stageT0 = ClockT::now();
        auto stageMs = [](ClockT::time_point a, ClockT::time_point b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };

        s_ulRenderer->Update();
        // 1.4 pump contract: requestAnimationFrame, CSS animations/transitions
        // and smooth scrolling are driven per-display by RefreshDisplay —
        // Update()+Render() alone still paints, but every animation silently
        // freezes without this call. Our views all ride default display 0.
        s_ulRenderer->RefreshDisplay(0);
        const auto stageAfterUpdate = ClockT::now();
        double frameUlRender = 0.0;   // THIS frame's render, for the composite subtraction
        PresentedFrame frame;   // this frame's views, for every presenter (flat + VR)
        // While the UI pass is live the composite records its quads for it instead of drawing them over the
        // back buffer; published below every frame, an empty set included, so a hidden view leaves no trace.
        SyncOverlayMenu(AnyInteractiveViewVisible());
        s_compositeFrames.fetch_add(1);
        const bool defer = s_overlayMenuRegistered && UiPassActive();
        // The first deferred frame also draws here: the UI pass already ran this frame with nothing queued.
        static bool s_deferredLast = false;
        const bool drawNow = !defer || !s_deferredLast;
        s_deferredLast = defer;
        std::vector<QueuedQuad> queued;
        auto emit = [&](ID3D11ShaderResourceView* qsrv, float u0, float v0, float u1, float v1,
                        float x, float y, float w, float h, const float* cutUV) {
            if (drawNow) DrawOverlay(sc, qsrv, u0, v0, u1, v1, x, y, w, h, cutUV);
            if (!defer || !qsrv) return;
            QueuedQuad q;
            q.srv = qsrv; qsrv->AddRef();
            q.u0 = u0; q.v0 = v0; q.u1 = u1; q.v1 = v1; q.x = x; q.y = y; q.w = w; q.h = h;
            if (cutUV) { q.hasCut = true; std::memcpy(q.cut, cutUV, sizeof(q.cut)); }
            queued.push_back(q);
        };
        if (AnyViewVisible()) {
            // RenderOnly(visible), not Render(): Render() paints EVERY view
            // whose page marked itself dirty, hidden ones included — a closed
            // popup with a spinner, a closed dashboard with a blinking caret,
            // repainted every frame for as long as some OTHER view (a HUD
            // badge, SkyrimNet's chat) kept the loop rendering. The docs'
            // RenderOnly takes the subset; hidden pages keep their dirty flag
            // and paint on their next Show. needs_paint is sampled first —
            // rendering clears it — and only for the views we hand over.
            std::vector<ultralight::View*> toRender;
            {
                std::lock_guard<std::mutex> lk(s_viewsMutex);
                toRender.reserve(s_views.size());
                for (auto& vp : s_views) {
                    MlView& v = *vp;
                    const bool paint = v.visible && v.ul;
                    v.repaintedThisFrame = paint && v.ul->needs_paint();
                    if (paint) toRender.push_back(v.ul.get());
                }
            }
            if (!toRender.empty()) s_ulRenderer->RenderOnly(toRender.data(), toRender.size());
            frameUlRender = stageMs(stageAfterUpdate, ClockT::now());
            s_acc.ulRender += frameUlRender;
            // One state capture brackets everything we do to the pipeline this
            // frame — the GPU driver's command list (which freely rebinds
            // RTs/shaders/scissor) and every composite draw alike.
            StateBackup backup;
            backup.Capture(s_context);
            StateBackup::Neutralize(s_context);
            if (s_gpuActive && s_gpuHas(s_gpu)) s_gpuDraw(s_gpu);
            // Composite visible views in z-order ((layer, order) — the
            // registry is kept sorted by SortViewsLocked). Registry
            // mutation only happens on our own threads — the brief lock walks
            // are safe against the API surface.
            {
                std::lock_guard<std::mutex> lk(s_viewsMutex);
                for (auto& vp : s_views) {
                    MlView& v = *vp;
                    if (!v.visible || !v.ul) continue;
                    float ex = 0, ey = 0;
                    EffectivePos(v, bw, bh, ex, ey);
                    // Presenter seam: the same reading the VR submitter gets.
                    PresentedView pv;
                    pv.id = v.id; pv.ul = v.ul; pv.x = ex; pv.y = ey; pv.w = v.w; pv.h = v.h;
                    pv.layer = v.layer; pv.isInspector = v.isInspector; pv.gpuPath = s_gpuActive;
                    pv.clickThrough = v.clickThrough;   // B3: the laser skips click-through views like hitTest does
                    pv.repainted = v.repaintedThisFrame;
                    // Cutout: view pixels -> this quad's uv range (the GPU
                    // target may be padded, so map through its uv rect).
                    float cut[4] = { 0, 0, 0, 0 };
                    const float* cutPtr = nullptr;
                    if (v.cutW > 0 && v.cutH > 0 && v.w > 0 && v.h > 0) cutPtr = cut;
                    auto mapCut = [&](float u0, float v0, float u1, float v1) {
                        const float fx0 = v.cutX / v.w, fy0 = v.cutY / v.h;
                        const float fx1 = (v.cutX + v.cutW) / v.w, fy1 = (v.cutY + v.cutH) / v.h;
                        cut[0] = u0 + fx0 * (u1 - u0); cut[1] = v0 + fy0 * (v1 - v0);
                        cut[2] = u0 + fx1 * (u1 - u0); cut[3] = v0 + fy1 * (v1 - v0);
                    };
                    // Mirror fit: a VR panel is sized for the headset, not
                    // for the desktop window, so scale it to FIT the mirror
                    // rather than drawing it 1:1 and cropping the right edge.
                    float mx = static_cast<float>(ex), my = static_cast<float>(ey);
                    float mw = static_cast<float>(v.w), mh = static_cast<float>(v.h);
                    if (VR::IsLive() && v.fullscreen && bw > 0 && bh > 0 && v.w > 0 && v.h > 0 &&
                        (v.w != static_cast<int>(bw) || v.h != static_cast<int>(bh))) {
                        const float s = std::min(static_cast<float>(bw) / v.w,
                                                 static_cast<float>(bh) / v.h);
                        mw = v.w * s; mh = v.h * s;
                        mx = (static_cast<float>(bw) - mw) * 0.5f;
                        my = (static_cast<float>(bh) - mh) * 0.5f;
                    }
                    if (s_gpuActive) {
                        const auto rt = v.ul->render_target();
                        auto* srv = static_cast<ID3D11ShaderResourceView*>(
                            s_gpuSrv(s_gpu, rt.texture_id));
                        if (cutPtr) mapCut(rt.uv_coords.left, rt.uv_coords.top, rt.uv_coords.right, rt.uv_coords.bottom);
                        if (VR::MirrorEnabled())
                            emit(srv, rt.uv_coords.left, rt.uv_coords.top,
                                 rt.uv_coords.right, rt.uv_coords.bottom,
                                 mx, my, mw, mh, cutPtr);
                        pv.srv = srv;
                        pv.u0 = rt.uv_coords.left; pv.v0 = rt.uv_coords.top;
                        pv.u1 = rt.uv_coords.right; pv.v1 = rt.uv_coords.bottom;
                    } else {
                        UploadSurfaceIfDirty(v);
                        if (cutPtr) mapCut(0.0f, 0.0f, 1.0f, 1.0f);
                        if (VR::MirrorEnabled())
                            emit(v.srv, 0.0f, 0.0f, 1.0f, 1.0f, mx, my, mw, mh, cutPtr);
                        pv.srv = v.srv;
                    }
                    if (cutPtr) { pv.hasCutout = true; std::memcpy(pv.cutUV, cut, sizeof(cut)); }
                    frame.push_back(std::move(pv));
                }
            }
            // Our own cursor, topmost. The vanilla cursor sprite is hidden while
            // UI mode is up (HideVanillaCursor): at Present it would land under
            // the overlay, but in the UI pass the CursorMenu draws after us.
            // This quad tracks the same MenuCursor position, sized from
            // cursorHeight at 1080p and scaled with the back buffer's height.
            if (s_focused.load() && s_cursorSrv && bh > 0 && !VR::IsLive()) {
                const float px = static_cast<float>(s_cursorPosX.load());
                const float py = static_cast<float>(s_cursorPosY.load());
                const int artHeight = static_cast<int>(std::lround(s_cursorHeight * (bh / 1080.0f)));
                if (!(s_cursorCustom && s_cursorImgH > 0) && EnsureCursorArt(artHeight)) {
                    // The drawn cursor: the glow fades in over a clickable element and a press shrinks the arrow
                    // about its tip. At rest it sits on whole pixels (1:1 with its texture when the target is the back buffer).
                    static float glow = 0.0f, press = 0.0f;
                    static auto last = std::chrono::steady_clock::now();
                    const auto now = std::chrono::steady_clock::now();
                    const float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
                    last = now;
                    const auto approach = [dt](float v, float target, float seconds) {
                        const float step = dt / seconds;
                        return target > v ? std::min(target, v + step) : std::max(target, v - step);
                    };
                    const PageCursor kind = PageCursorAt(static_cast<int>(px), static_cast<int>(py), bw, bh);
                    glow  = approach(glow, kind == PageCursor::Clickable ? 1.0f : 0.0f, 0.12f);
                    press = approach(press, s_mouseDown.load() ? 1.0f : 0.0f, 0.06f);
                    const bool text = kind == PageCursor::Text;
                    const CursorArtTex& t = text ? s_artIBeam
                        : s_artArrow[std::clamp(static_cast<int>(std::lround(glow * (kArtGlowSteps - 1))), 0, kArtGlowSteps - 1)];
                    const float scale = text ? 1.0f : 1.0f - 0.14f * press;
                    float x = px - t.hotX * scale, y = py - t.hotY * scale;
                    if (scale == 1.0f) { x = std::round(x); y = std::round(y); }
                    emit(t.srv, 0.0f, 0.0f, 1.0f, 1.0f, x, y, t.w * scale, t.h * scale, nullptr);
                } else if (s_cursorCustom && s_cursorImgH > 0) {
                    // Custom art: configured height at 1080p scaled with the
                    // backbuffer, aspect preserved, drawn so the hotspot
                    // pixel sits exactly on the cursor position.
                    const float h = s_cursorHeight * (bh / 1080.0f);
                    const float w = h * (static_cast<float>(s_cursorImgW) / s_cursorImgH);
                    emit(s_cursorSrv, 0.0f, 0.0f, 1.0f, 1.0f,
                         px - s_cursorHotX * w, py - s_cursorHotY * h, w, h, nullptr);
                } else {
                    const float h = s_cursorHeight * (bh / 1080.0f);
                    emit(s_cursorSrv, 0.0f, 0.0f, 1.0f, 1.0f,
                         px, py, h * (static_cast<float>(kCursorW) / kCursorH), h, nullptr);
                }
            }
            backup.Restore(s_context);
        }
        {
            std::lock_guard<std::mutex> lk(s_uiPassMutex);
            ReleaseQuads(s_uiPassQuads);
            if (defer) {
                s_uiPassQuads = std::move(queued);
                s_uiPassBackW = bw;
                s_uiPassBackH = bh;
                s_uiPassPublished = std::chrono::steady_clock::now();
            }
        }
        // VR presenter: runs every frame (an empty frame HIDES stale overlays);
        // no-op unless the runtime is live. Same thread as everything above.
        const auto stageBeforeVR = ClockT::now();
        VR::SubmitFrame(frame, s_focused.load(), static_cast<ViewId>(s_uiModeView.load()));
        const auto stageEnd = ClockT::now();

        s_acc.ulUpdate  += stageMs(stageT0, stageAfterUpdate);
        // Subtract THIS frame's render, not the running accumulator — the
        // latter grew every frame and drove the reported composite deeply
        // negative, which made the one number meant to localise cost useless.
        s_acc.composite += (std::max)(0.0, stageMs(stageAfterUpdate, stageBeforeVR) - frameUlRender);
        s_acc.vrSubmit  += stageMs(stageBeforeVR, stageEnd);
        const double whole = stageMs(stageT0, stageEnd);
        if (whole > s_acc.worst) s_acc.worst = whole;
        ++s_acc.frames;
        if (s_acc.lastReport.time_since_epoch().count() == 0) s_acc.lastReport = stageEnd;
        if (stageEnd - s_acc.lastReport >= std::chrono::seconds(2) && s_acc.frames > 0) {
            const double n = static_cast<double>(s_acc.frames);
            SKSE::log::info(
                "Magelight: present cost over {} frames — ul.update {:.2f}ms, ul.render {:.2f}ms, "
                "composite(+mirror) {:.2f}ms, vr.submit {:.2f}ms, total avg {:.2f}ms, WORST {:.2f}ms "
                "(uiMode={}, views={})",
                s_acc.frames, s_acc.ulUpdate / n, s_acc.ulRender / n, s_acc.composite / n,
                s_acc.vrSubmit / n,
                (s_acc.ulUpdate + s_acc.ulRender + s_acc.composite + s_acc.vrSubmit) / n,
                s_acc.worst, s_focused.load(), frame.size());
            s_acc = StageAcc{};
            s_acc.lastReport = stageEnd;
        }
    }

    // The carrier's PostDisplay: draw the published quads into the render target the game's UI pass has bound,
    // scaled from the back buffer they were laid out for. Main thread, inside the engine's UI render.
    static void UiPassCompositeImpl()
    {
        s_lastUiPassFrame.store(s_compositeFrames.load());
        s_uiPassSeen.store(true);
        if (!s_context || !s_device || s_renderDead.load()) return;
        std::vector<QueuedQuad> quads;
        float backW = 0, backH = 0;
        {
            std::lock_guard<std::mutex> lk(s_uiPassMutex);
            // Present stopped composing (a load, a stalled hook): draw nothing rather than a frozen frame.
            if (s_uiPassQuads.empty() ||
                std::chrono::steady_clock::now() - s_uiPassPublished > std::chrono::milliseconds(250)) return;
            quads = s_uiPassQuads;
            for (auto& q : quads) q.srv->AddRef();
            backW = s_uiPassBackW;
            backH = s_uiPassBackH;
        }
        StateBackup backup;
        backup.Capture(s_context);
        ID3D11RenderTargetView* target = backup.rtvs[0];
        float tw = 0, th = 0;
        if (target) {
            ID3D11Resource* res = nullptr;
            target->GetResource(&res);
            ID3D11Texture2D* tex = nullptr;
            if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex) {
                D3D11_TEXTURE2D_DESC td{};
                tex->GetDesc(&td);
                tw = static_cast<float>(td.Width);
                th = static_cast<float>(td.Height);
                tex->Release();
                // Which target the UI pass draws into is the whole question on an unfamiliar setup: log each change.
                static void* s_lastTarget = nullptr;
                static int   s_targetLogs = 0;
                if (res != s_lastTarget && s_targetLogs < 10) {
                    s_lastTarget = res;
                    ++s_targetLogs;
                    SKSE::log::info("Magelight: UI pass draws into a {}x{} target, format {} (back buffer {}x{})",
                        td.Width, td.Height, static_cast<int>(td.Format), backW, backH);
                }
            }
            if (res) res->Release();
        }
        if (target && tw > 0 && th > 0 && backW > 0 && backH > 0) {
            StateBackup::Neutralize(s_context);
            const float sx = tw / backW, sy = th / backH;
            for (const auto& q : quads)
                DrawQuadTo(target, tw, th, q.srv, q.u0, q.v0, q.u1, q.v1,
                           q.x * sx, q.y * sy, q.w * sx, q.h * sy, q.hasCut ? q.cut : nullptr);
        } else {
            static std::atomic<int> s_noTargetLogs{ 0 };
            if (s_noTargetLogs.fetch_add(1) < 3)
                SKSE::log::warn("Magelight: UI pass had no usable render target bound - nothing drawn this frame");
        }
        backup.Restore(s_context);
        ReleaseQuads(quads);
    }

    static void UiPassComposite()
    {
        __try {
            UiPassCompositeImpl();
        } __except (LogSehAndDisable("UI pass", GetExceptionCode(),
                        (GetExceptionInformation())->ExceptionRecord->ExceptionAddress)) {
        }
    }

    // SEH wrapper — no locals with destructors allowed in here.
    static void GuardedFrameWork(IDXGISwapChain* sc)
    {
        __try {
            FrameWork(sc);
        } __except (LogSehAndDisable("frame work", GetExceptionCode(),
                        (GetExceptionInformation())->ExceptionRecord->ExceptionAddress)) {
        }
    }

    // IDXGISwapChain::ResizeBuffers (vtable slot 13): resolution / fullscreen
    // changes. We hold NO backbuffer references between frames (DrawOverlay
    // takes and releases its RTV per draw), so nothing must be dropped here;
    // fullscreen views re-size themselves off the new backbuffer next frame
    // and DisplayResized fires from that size change. This hook exists so a
    // resize is VISIBLE in the log and so a failed ResizeBuffers (some other
    // hook holding a reference) is named instead of silently breaking the
    // game's present.
    using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    static ResizeBuffersFn s_origResizeBuffers = nullptr;
    static HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h,
                                                       DXGI_FORMAT fmt, UINT flags)
    {
        const HRESULT hr = s_origResizeBuffers(sc, count, w, h, fmt, flags);
        static std::atomic<int> s_logged{ 0 };
        if (s_logged.fetch_add(1) < 16) {
            if (SUCCEEDED(hr))
                SKSE::log::info("Magelight: swapchain resized to {}x{} (buffers {}, format {})", w, h, count,
                    static_cast<int>(fmt));
            else
                SKSE::log::error("Magelight: ResizeBuffers({}x{}) FAILED 0x{:08X} — a swapchain reference is "
                    "outstanding somewhere (not ours: we hold none between frames)", w, h,
                    static_cast<unsigned>(hr));
        }
        return hr;
    }

    // ── Present diagnostics: who presents, and what ──────────────────────────
    // A page that draws (ul.render, composite in the cost line) but never shows means something presents
    // another image after us: frame generation, an upscaler or ENB proxy, a second swapchain. These lines
    // name it from the log alone.

    static std::string ModuleOf(const void* addr)
    {
        HMODULE mod = nullptr;
        if (!addr || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                         static_cast<LPCWSTR>(addr), &mod) || !mod)
            return fmt::format("{:p} (no module)", addr);
        char path[MAX_PATH] = {};
        GetModuleFileNameA(mod, path, MAX_PATH);
        const char* name = std::strrchr(path, '\\');
        return fmt::format("{}+0x{:X}", name ? name + 1 : path,
            reinterpret_cast<std::uintptr_t>(addr) - reinterpret_cast<std::uintptr_t>(mod));
    }

    static std::string DescribeSwapChain(IDXGISwapChain* sc)
    {
        DXGI_SWAP_CHAIN_DESC d{};
        if (!sc || FAILED(sc->GetDesc(&d))) return "GetDesc failed";
        return fmt::format("{}x{} format {} buffers {} swapEffect {} flags 0x{:X} windowed {} hwnd {:p}",
            d.BufferDesc.Width, d.BufferDesc.Height, static_cast<int>(d.BufferDesc.Format), d.BufferCount,
            static_cast<int>(d.SwapEffect), d.Flags, d.Windowed ? 1 : 0, static_cast<void*>(d.OutputWindow));
    }

    static IDXGISwapChain*                 s_gameSwapChain = nullptr;   // the one InstallHook hooked (renderWindows[0])
    static std::mutex                      s_seenSwapChainsMutex;
    static std::vector<IDXGISwapChain*>    s_seenSwapChains;            // bounded: kMaxSeenSwapChains
    static std::atomic<IDXGISwapChain*>    s_lastPresenter{ nullptr };
    static std::atomic<std::uint64_t>      s_presentsGame{ 0 }, s_presentsOther{ 0 };
    static constexpr std::size_t           kMaxSeenSwapChains = 8;

    // Once per swapchain: its description and the module that called Present on it. A periodic line counts
    // presents of other swapchains, logged only while there are some.
    static void NotePresenter(IDXGISwapChain* sc, const void* caller)
    {
        (sc == s_gameSwapChain ? s_presentsGame : s_presentsOther).fetch_add(1, std::memory_order_relaxed);
        if (s_lastPresenter.exchange(sc) != sc) {
            std::lock_guard<std::mutex> lk(s_seenSwapChainsMutex);
            if (std::find(s_seenSwapChains.begin(), s_seenSwapChains.end(), sc) == s_seenSwapChains.end()
                && s_seenSwapChains.size() < kMaxSeenSwapChains) {
                s_seenSwapChains.push_back(sc);
                SKSE::log::info("Magelight: Present on swapchain {:p} ({}) from {} - {}", static_cast<void*>(sc),
                    sc == s_gameSwapChain ? "the game's" : "NOT the game's", ModuleOf(caller), DescribeSwapChain(sc));
            }
        }
        static std::atomic<long long> s_nextSummaryMs{ 0 };
        const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        long long due = s_nextSummaryMs.load();
        if (now >= due && s_nextSummaryMs.compare_exchange_strong(due, now + 10000) && due != 0) {
            const auto other = s_presentsOther.exchange(0);
            const auto game = s_presentsGame.exchange(0);
            if (other > 0)
                SKSE::log::warn("Magelight: presents in the last 10 s - game swapchain {}, other swapchains {}", game, other);
        }
    }

    // Where the frame is composited. The vtable hooks below are installed at kDataLoaded, so a plugin that
    // hooked Present earlier (an upscaler, a proxy) runs AFTER our composite and can draw over it. Late
    // composite moves the draw into a detour on dxgi's own Present, the last code before the frame is queued.
    // One composite per present per thread: a wrapper's Present that calls Present1, or a vtable hook above
    // the late detour, must not draw twice (t_presentDepth / t_composited).
    static std::atomic<int>  s_lateMisses{ 0 };   // vtable presents whose chain never reached the late detour
    static thread_local int  t_presentDepth = 0;
    static thread_local bool t_composited = false;

    static void StampPresent(IDXGISwapChain* sc, const void* caller)
    {
        NotePresenter(sc, caller);
        // Stall watchdog heartbeat: EVERY present counts, from whichever thread (loading screens and menus
        // present from a foreign thread and FrameWork returns at its top).
        s_lastPresentMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch()).count());
        s_lastPresentTid.store(GetCurrentThreadId());
    }

    // A swapchain on another device (a frame-generation backend's own D3D11 or D3D12 swapchain) must never be
    // drawn into with our device's resources. Skipping without marking the present composited lets the late
    // fallback count it as a miss. Asked only of dxgi's own swapchains, inside the detours: a wrapper's
    // GetDevice (Community Shaders') can hand out the device without a reference, and our Release then
    // freed the game's device within a second.
    static ID3D11Device* s_gameDevice = nullptr;   // the game swapchain's device, read at hook install; never Release (a wrapper may not AddRef)

    static bool OnOurDevice(IDXGISwapChain* sc)
    {
        ID3D11Device* const ours = s_device ? s_device : s_gameDevice;
        ID3D11Device* dev = nullptr;
        if (!ours || FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) || !dev) return false;
        const bool same = dev == ours;
        dev->Release();
        return same;
    }

    // `inDetour`: called from a dxgi detour, where `sc` is dxgi's own swapchain; from a vtable hook it is the
    // game's object, which may be a wrapper, so it is compared, never asked.
    static void ComposeOnce(IDXGISwapChain* sc, bool fromLateDetour, bool inDetour)
    {
        if (t_composited || fromLateDetour != s_lateComposite.load()) return;
        if (inDetour ? !OnOurDevice(sc) : sc != s_gameSwapChain) return;
        t_composited = true;
        if (s_runtimeReady.load() && !s_renderDead.load()) GuardedFrameWork(sc);
    }

    static void AfterCompose()
    {
        // Render death is announced once, outside the SEH guard (FrameWork is never entered again after the
        // kill switch), and a player is never left with suspended controls by a dead renderer.
        if (s_renderDead.load()) {
            static std::atomic<bool> s_deadNotified{ false };
            if (!s_deadNotified.exchange(true)) {
                VR::Shutdown();   // overlays must not outlive a dead overlay host
                Emit(HostEvent::RenderDead, 0, 0, 0, RenderDeadReasonText());
                s_deadNoticePending.store(true);
                if (s_overlayMenuRegistered)
                    GameTask::Post([]() {
                        if (auto* q = RE::UIMessageQueue::GetSingleton())
                            q->AddMessage(MagelightOverlayMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
                    });
            }
            if (s_focused.load()) GameTask::Post([]() { SetUIMode(false); });
            // A death at start lands at the main menu, where the HUD is closed and a notice would be lost: it
            // waits for the HUD (asked every 60 presents).
            static int s_deadNoticeFrames = 0;
            if (s_deadNoticePending.load() && ++s_deadNoticeFrames >= 60) {
                s_deadNoticeFrames = 0;
                auto* ui = RE::UI::GetSingleton();
                if (ui && ui->IsMenuOpen(RE::HUDMenu::MENU_NAME) && s_deadNoticePending.exchange(false))
                    GameTask::Post([]() {
                        RE::SendHUDMessage::ShowHUDMessage("Magelight UI could not draw and is off for this session - see Magelight.log");
                    });
            }
        }
    }

    // The outermost present of a thread ends here; the next one may composite again. In late mode a vtable
    // present whose chain never reached the detour (the plugin under us presented another way) is a miss, and
    // 30 in a row fall back to compositing in the vtable hook, so the UI is never lost for good.
    static void EndPresent(bool vtableHook)
    {
        if (--t_presentDepth > 0) return;
        if (vtableHook && s_lateComposite.load()) {
            if (t_composited) {
                s_lateMisses.store(0);
            } else if (s_lateMisses.fetch_add(1) + 1 >= 30) {
                s_lateComposite.store(false);
                SKSE::log::warn("Magelight: late composite did not draw in 30 frames (dxgi's Present not reached, or not on the game's device) - compositing in the vtable hook again");
            }
        }
        t_composited = false;
    }

    static HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
    {
        GameTask::Scope pumpScope;   // posts from here go through the pump (MagelightGameTask.h)
        ++t_presentDepth;
        StampPresent(sc, _ReturnAddress());
        ComposeOnce(sc, false, false);
        AfterCompose();
        const HRESULT hr = s_origPresent(sc, sync, flags);
        EndPresent(true);
        return hr;
    }

    using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    static Present1Fn s_origPresent1 = nullptr;

    // Flip-model presenters (frame generation, some upscalers and overlays) can present through Present1,
    // which never passes through Present.
    static std::atomic<bool> s_latePresent1Hooked{ false };   // dxgi's Present1 carries a late detour

    static HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                                  const DXGI_PRESENT_PARAMETERS* params)
    {
        GameTask::Scope pumpScope;
        ++t_presentDepth;
        StampPresent(sc, _ReturnAddress());
        // Without a Present1 detour, late mode has nowhere later to draw a Present1 frame: draw it here.
        ComposeOnce(sc, s_lateComposite.load() && !s_latePresent1Hooked.load(), false);
        AfterCompose();
        const HRESULT hr = s_origPresent1(sc, sync, flags, params);
        EndPresent(true);
        return hr;
    }

    static PresentFn  s_latePresentNext = nullptr;    // MinHook trampolines into dxgi's own code
    static Present1Fn s_latePresent1Next = nullptr;

    static HRESULT STDMETHODCALLTYPE LatePresent(IDXGISwapChain* sc, UINT sync, UINT flags)
    {
        GameTask::Scope pumpScope;
        ++t_presentDepth;
        ComposeOnce(sc, true, true);
        AfterCompose();
        const HRESULT hr = s_latePresentNext(sc, sync, flags);
        EndPresent(false);
        return hr;
    }

    static HRESULT STDMETHODCALLTYPE LatePresent1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                                  const DXGI_PRESENT_PARAMETERS* params)
    {
        GameTask::Scope pumpScope;
        ++t_presentDepth;
        ComposeOnce(sc, true, true);
        AfterCompose();
        const HRESULT hr = s_latePresent1Next(sc, sync, flags, params);
        EndPresent(false);
        return hr;
    }

    static const IMAGE_NT_HEADERS* NtHeaders(const void* base)
    {
        const auto* b = static_cast<const std::uint8_t*>(base);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(b);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(b + dos->e_lfanew);
        return nt->Signature == IMAGE_NT_SIGNATURE ? nt : nullptr;
    }

    // dxgi's own function for a vtable slot, whatever was patched into the slot since. The slot's bytes are
    // read from the dxgi.dll FILE: an image mapping shares the live module's (relocated, patched) pages. The
    // file holds preferred base + RVA, unrelocated.
    static void* OriginalVtableSlot(HMODULE mod, void* const* vtbl, int slot)
    {
        wchar_t path[MAX_PATH] = {};
        if (!GetModuleFileNameW(mod, path, MAX_PATH)) return nullptr;
        const auto* liveNt = NtHeaders(mod);
        if (!liveNt) return nullptr;
        const std::uint64_t rva = static_cast<std::uint64_t>(
            reinterpret_cast<const std::uint8_t*>(&vtbl[slot]) - reinterpret_cast<const std::uint8_t*>(mod));
        std::ifstream f(path, std::ios::binary);
        if (!f) return nullptr;
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS64 nt{};
        if (!f.read(reinterpret_cast<char*>(&dos), sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
        f.seekg(dos.e_lfanew);
        if (!f.read(reinterpret_cast<char*>(&nt), sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt.OptionalHeader.SizeOfImage != liveNt->OptionalHeader.SizeOfImage ||
            nt.FileHeader.TimeDateStamp != liveNt->FileHeader.TimeDateStamp ||
            nt.OptionalHeader.CheckSum != liveNt->OptionalHeader.CheckSum)
            return nullptr;
        f.seekg(dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader);
        for (WORD i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
            IMAGE_SECTION_HEADER sh{};
            if (!f.read(reinterpret_cast<char*>(&sh), sizeof(sh))) return nullptr;
            if (rva < sh.VirtualAddress || rva + sizeof(std::uint64_t) > std::uint64_t{ sh.VirtualAddress } + sh.SizeOfRawData) continue;
            std::uint64_t onDisk = 0;
            f.seekg(sh.PointerToRawData + (rva - sh.VirtualAddress));
            if (!f.read(reinterpret_cast<char*>(&onDisk), sizeof(onDisk))) return nullptr;
            const std::uint64_t fnRva = onDisk - nt.OptionalHeader.ImageBase;
            return fnRva < nt.OptionalHeader.SizeOfImage ? reinterpret_cast<std::uint8_t*>(mod) + fnRva : nullptr;
        }
        return nullptr;
    }

    static HMODULE ModuleAt(const void* p)
    {
        HMODULE mod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(p), &mod);
        return mod;
    }

    // `mod`'s file name is `name`, whichever folder it loaded from (Streamline ships one per backend).
    static bool ModuleBaseNameIs(HMODULE mod, const wchar_t* name)
    {
        wchar_t path[MAX_PATH] = {};
        if (!mod || !GetModuleFileNameW(mod, path, MAX_PATH)) return false;
        const wchar_t* file = std::wcsrchr(path, L'\\');
        return _wcsicmp(file ? file + 1 : path, name) == 0;
    }

    // What a known module in the present chain is, for the log: "" for anything else.
    static const char* KnownPresentLayer(const void* addr)
    {
        const HMODULE mod = ModuleAt(addr);
        if (ModuleBaseNameIs(mod, L"NvPresent64.dll")) return " (NVIDIA driver present layer, used by Smooth Motion)";
        if (ModuleBaseNameIs(mod, L"sl.interposer.dll")) return " (NVIDIA Streamline)";
        if (ModuleBaseNameIs(mod, L"CommunityShaders.dll")) return " (Community Shaders frame-generation proxy)";
        if (ModuleBaseNameIs(mod, L"SkyrimUpscaler.dll")) return " (Skyrim Upscaler)";
        return "";
    }

    // Whether `p` points into committed executable memory. A non-canonical value (vtable slot past the end of a
    // short vtable, read as data) fails VirtualQuery and answers false.
    static bool IsExecutableAddress(const void* p)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!p || !VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
        constexpr DWORD kExec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        return (mbi.Protect & kExec) != 0 && (mbi.Protect & PAGE_GUARD) == 0;
    }

    // `mod` is the system directory's `name`: a same-named proxy in the game folder (ReShade or a frame-gen
    // layer installed as dxgi.dll) is not dxgi's own code.
    static bool IsSystemModule(HMODULE mod, const wchar_t* name)
    {
        wchar_t path[MAX_PATH] = {}, sys[MAX_PATH] = {};
        if (!mod || !GetModuleFileNameW(mod, path, MAX_PATH) || !GetSystemDirectoryW(sys, MAX_PATH)) return false;
        return _wcsicmp(path, (std::wstring(sys) + L"\\" + name).c_str()) == 0;
    }

    // dxgi's swapchain vtable when the game's swapchain is a proxy (ENB's d3d11.dll wraps it): a throwaway
    // WARP device and swapchain made through the SYSTEM d3d11.dll, so a d3d11.dll proxy does not see them. A
    // dxgi.dll proxy would (system d3d11 binds to the loaded dxgi.dll); the caller's IsSystemModule refuses it.
    // The vtable is shared by every dxgi swapchain; only its address is kept.
    static void* const* DxgiSwapChainVtable()
    {
        wchar_t sys[MAX_PATH] = {};
        if (!GetSystemDirectoryW(sys, MAX_PATH)) return nullptr;
        HMODULE d3d = LoadLibraryW((std::wstring(sys) + L"\\d3d11.dll").c_str());
        if (!d3d) return nullptr;
        void* const* out = nullptr;
        auto create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(d3d, "D3D11CreateDevice"));
        HWND wnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 8, 8, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ID3D11Device* dev = nullptr;
        if (create && wnd && SUCCEEDED(create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, nullptr))) {
            IDXGIDevice* dxgiDev = nullptr;
            IDXGIAdapter* adapter = nullptr;
            IDXGIFactory* factory = nullptr;
            if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDev))) &&
                SUCCEEDED(dxgiDev->GetAdapter(&adapter)) &&
                SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory)))) {
                DXGI_SWAP_CHAIN_DESC d{};
                d.BufferCount = 1;
                d.BufferDesc.Width = 8;
                d.BufferDesc.Height = 8;
                d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                d.OutputWindow = wnd;
                d.SampleDesc.Count = 1;
                d.Windowed = TRUE;
                IDXGISwapChain* sc = nullptr;
                if (SUCCEEDED(factory->CreateSwapChain(dev, &d, &sc)) && sc) {
                    out = *reinterpret_cast<void***>(sc);
                    sc->Release();
                }
            }
            if (factory) factory->Release();
            if (adapter) adapter->Release();
            if (dxgiDev) dxgiDev->Release();
            dev->Release();
        }
        if (wnd) DestroyWindow(wnd);
        return out;
    }

    // "auto" turns it on only behind an earlier Present hook on a plain dxgi swapchain, never on VR. Behind a
    // proxy (ENB's or ReShade's d3d11.dll, Skyrim Upscaler) only "late" looks for dxgi's own Present: the
    // throwaway device it takes for that killed the game at start behind ENB.
    static void InstallLatePresent(void* const* vtbl, void* const* vtbl1)
    {
        const std::string& mode = s_presentHookMode;
        if (mode == "vtable") {
            SKSE::log::info("Magelight: presentHook 'vtable' - compositing in the vtable hook");
            return;
        }
        if (s_untrustedChain.load()) {
            // Its swapchain is not a dxgi D3D11 one (frame generation on Direct3D 12): dxgi's Present never draws
            // on our device, and the throwaway swapchain the search takes is not worth the risk.
            SKSE::log::info("Magelight: presentHook '{}' - the game swapchain is untrusted, compositing in the vtable hook", mode);
            return;
        }
        const bool earlierHook = ModuleAt(reinterpret_cast<const void*>(s_origPresent)) != ModuleAt(vtbl);
        if (mode != "late" && (!earlierHook || REL::Module::IsVR())) {
            SKSE::log::info("Magelight: presentHook 'auto' - {}, compositing in the vtable hook",
                REL::Module::IsVR() ? "VR" : "no earlier Present hook");
            return;
        }
        HMODULE dxgi = ModuleAt(vtbl);
        if (!IsSystemModule(dxgi, L"dxgi.dll")) {
            if (mode != "late") {
                SKSE::log::info("Magelight: presentHook 'auto' - the game swapchain is a proxy ({}), compositing in "
                                "the vtable hook", ModuleOf(vtbl));
                return;
            }
            // A proxy's swapchain wraps a real dxgi one: detour dxgi's own functions, found through a throwaway
            // dxgi swapchain.
            SKSE::log::info("Magelight: the game swapchain is a proxy ({}); finding dxgi's own swapchain", ModuleOf(vtbl));
            vtbl = DxgiSwapChainVtable();
            vtbl1 = vtbl;
            dxgi = vtbl ? ModuleAt(vtbl) : nullptr;
            if (!vtbl || !IsSystemModule(dxgi, L"dxgi.dll")) {
                SKSE::log::warn("Magelight: late composite unavailable - no dxgi swapchain vtable found");
                return;
            }
        }
        void* present = OriginalVtableSlot(dxgi, vtbl, 8);
        void* present1 = (vtbl1 && ModuleAt(vtbl1) == dxgi) ? OriginalVtableSlot(dxgi, vtbl1, 22) : nullptr;
        if (!present || ModuleAt(present) != dxgi) {
            SKSE::log::warn("Magelight: late composite unavailable - dxgi's own Present not found");
            return;
        }
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
            SKSE::log::warn("Magelight: late composite unavailable - MinHook init: {}", MH_StatusToString(init));
            return;
        }
        MH_STATUS st = MH_CreateHook(present, reinterpret_cast<void*>(&LatePresent), reinterpret_cast<void**>(&s_latePresentNext));
        if (st == MH_OK) st = MH_EnableHook(present);
        if (st != MH_OK) {
            SKSE::log::warn("Magelight: late composite unavailable - hooking dxgi's Present at {} failed: {}",
                ModuleOf(present), MH_StatusToString(st));
            return;
        }
        if (present1 && ModuleAt(present1) == dxgi) {
            MH_STATUS st1 = MH_CreateHook(present1, reinterpret_cast<void*>(&LatePresent1), reinterpret_cast<void**>(&s_latePresent1Next));
            if (st1 == MH_OK) st1 = MH_EnableHook(present1);
            if (st1 == MH_OK)
                s_latePresent1Hooked.store(true);
            else
                SKSE::log::warn("Magelight: late composite: dxgi's Present1 at {} not hooked ({}) - Present1 frames composite in the vtable hook",
                    ModuleOf(present1), MH_StatusToString(st1));
        }
        s_lateComposite.store(true);
        SKSE::log::info("Magelight: late composite ON ('{}') - drawing inside dxgi's Present at {}, after {}",
            mode, ModuleOf(present), ModuleOf(reinterpret_cast<const void*>(s_origPresent)));
    }

    // ── Hook install (game thread, kDataLoaded) ─────────────────────────────

    void InstallHook()
    {
        bool expected = false;
        if (!s_hookInstalled.compare_exchange_strong(expected, true)) return;
        GameTask::Start();   // before the first present, so no thread is created inside one

        auto* data = RE::BSGraphics::Renderer::GetRendererDataSingleton();
        if (!data) {
            SKSE::log::error("Magelight: no renderer data — hook not installed");
            return;
        }
        auto* sc = reinterpret_cast<IDXGISwapChain*>(data->renderWindows[0].swapChain);
        if (!sc) {
            SKSE::log::error("Magelight: no swapchain — hook not installed");
            return;
        }

        void** vtbl = *reinterpret_cast<void***>(sc);
        constexpr int kPresentSlot = 8;
        DWORD oldProt = 0;
        if (!VirtualProtect(&vtbl[kPresentSlot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt)) {
            SKSE::log::error("Magelight: VirtualProtect failed — hook not installed");
            return;
        }
        s_origPresent = reinterpret_cast<PresentFn>(vtbl[kPresentSlot]);
        s_gameSwapChain = sc;
        const HRESULT devHr = sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&s_gameDevice));
        if (FAILED(devHr) || !s_gameDevice) {
            s_gameDevice = EngineDevice();   // borrowed like the swapchain's: never Released
            s_untrustedChain.store(true);
            SKSE::log::warn("Magelight: the game swapchain refused its device (0x{:08X}); the engine's is {:p}",
                static_cast<unsigned>(devHr), static_cast<void*>(s_gameDevice));
        }
        vtbl[kPresentSlot] = reinterpret_cast<void*>(&HookPresent);
        VirtualProtect(&vtbl[kPresentSlot], sizeof(void*), oldProt, &oldProt);
        // dxgi.dll here is a plain swapchain; anything else (an ENB d3d11.dll, an upscaler or frame-generation
        // proxy) wraps it, and what it presents is not necessarily the buffer we draw into.
        SKSE::log::info("Magelight: hooked Present was {}{} (vtable in {}{}); game swapchain {}",
            ModuleOf(reinterpret_cast<const void*>(s_origPresent)), KnownPresentLayer(reinterpret_cast<const void*>(s_origPresent)),
            ModuleOf(vtbl), KnownPresentLayer(vtbl), DescribeSwapChain(sc));
        void** vtbl1 = nullptr;
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&sc1))) && sc1) {
            vtbl1 = *reinterpret_cast<void***>(sc1);
            constexpr int kPresent1Slot = 22;
            // A wrapper may answer IDXGISwapChain1 with an IDXGISwapChain-only vtable (Community Shaders' frame
            // generation proxy has 18 slots): slots 18-22 are then whatever follows it, and writing slot 22
            // corrupted the IID its GetDevice compares. Patch only when IDXGISwapChain1's five slots are code.
            // Slots 18-22 span at most the vtable's own page and slot 22's, so slot 22's page decides readability.
            MEMORY_BASIC_INFORMATION slotMem{};
            const bool slotsReadable = VirtualQuery(&vtbl1[kPresent1Slot], &slotMem, sizeof(slotMem))
                && slotMem.State == MEM_COMMIT && (slotMem.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
            bool slotsAreCode = slotsReadable;
            for (int i = 18; slotsAreCode && i <= kPresent1Slot; ++i) slotsAreCode = IsExecutableAddress(vtbl1[i]);
            if (!slotsAreCode) {
                s_untrustedChain.store(true);
                if (slotsReadable)
                    SKSE::log::warn("Magelight: IDXGISwapChain1 vtable in {} is short (slots 18-22: {:p} {:p} {:p} {:p} {:p}) - "
                        "Present1 not hooked, the chain is untrusted", ModuleOf(vtbl1), vtbl1[18], vtbl1[19], vtbl1[20],
                        vtbl1[21], vtbl1[22]);
                else
                    SKSE::log::warn("Magelight: IDXGISwapChain1 vtable in {} ends before slot 22 (unreadable memory) - "
                        "Present1 not hooked, the chain is untrusted", ModuleOf(vtbl1));
                vtbl1 = nullptr;
            } else if (VirtualProtect(&vtbl1[kPresent1Slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt)) {
                s_origPresent1 = reinterpret_cast<Present1Fn>(vtbl1[kPresent1Slot]);
                vtbl1[kPresent1Slot] = reinterpret_cast<void*>(&HookPresent1);
                VirtualProtect(&vtbl1[kPresent1Slot], sizeof(void*), oldProt, &oldProt);
            }
            sc1->Release();
        }
        constexpr int kResizeBuffersSlot = 13;
        if (VirtualProtect(&vtbl[kResizeBuffersSlot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt)) {
            s_origResizeBuffers = reinterpret_cast<ResizeBuffersFn>(vtbl[kResizeBuffersSlot]);
            vtbl[kResizeBuffersSlot] = reinterpret_cast<void*>(&HookResizeBuffers);
            VirtualProtect(&vtbl[kResizeBuffersSlot], sizeof(void*), oldProt, &oldProt);
        } else {
            SKSE::log::warn("Magelight: ResizeBuffers hook not installed (VirtualProtect failed)");
        }

        SKSE::log::info("Magelight: Present hook installed (swapchain {:p})",
            static_cast<void*>(sc));

        // Milestone 3: subclass the game window for input. Real WM messages
        // are the only clean source of translated text (WM_CHAR); everything
        // is queued to the render thread, nothing touches Ultralight here.
        DXGI_SWAP_CHAIN_DESC scd{};
        if (SUCCEEDED(sc->GetDesc(&scd)) && scd.OutputWindow) {
            s_hwnd = scd.OutputWindow;
            s_origWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                s_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&HookWndProc)));
            if (s_origWndProc) {
                SKSE::log::info("Magelight: window subclassed for input (hwnd {:p})",
                    static_cast<void*>(s_hwnd));
                // Read before installing, so the shim never runs with no next proc (see AnsiShimWndProc).
                s_ansiShimPrev.store(reinterpret_cast<WNDPROC>(GetWindowLongPtrA(s_hwnd, GWLP_WNDPROC)));
                const auto replaced = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(
                    s_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&AnsiShimWndProc)));
                if (replaced) {
                    s_ansiShimPrev.store(replaced);
                    SKSE::log::info("Magelight: ANSI shim on top of the input subclass");
                } else {
                    SKSE::log::warn("Magelight: ANSI shim not installed (GetLastError={}); a plugin that subclasses "
                        "after Magelight with SetWindowLongPtrA and calls its previous proc directly will crash",
                        GetLastError());
                }
            } else {
                SKSE::log::error("Magelight: window subclass failed (GetLastError={}) — no view input",
                    GetLastError());
            }
        } else {
            SKSE::log::error("Magelight: no output window on swapchain — no view input");
        }

        // Host settings (toggle key, demo views) — s_runtimeDir is set by
        // PreloadRuntime, well before kDataLoaded lands here.
        LoadHostSettings();
        InstallLatePresent(vtbl, vtbl1);
        RegisterOverlayMenu(ModuleBaseNameIs(ModuleAt(vtbl), L"sl.interposer.dll"));
        RegisterFocusMenu();
        // Manifest mods (Data/Magelight/<ModId>/manifest.json): folders that
        // are mods. Registered through the v4 path like any DLL consumer.
        Manifest::LoadAll();

        // Demo views — DEV surfaces, created only when Magelight.json sets
        // "demoViews": true. With a real consumer (SeverActions) riding the
        // host, the always-on badge and the toggle-key playground are noise
        // for end users; with the flag on they still exercise the whole
        // pipeline (badge = click-through HUD anchor, playground = the
        // Vite/React file:/// shape the SA frontend ships in). Registered
        // before Ultralight initializes — they materialize on the render
        // thread once the world is up.
        if (s_demoViews) {
            s_playgroundId = CreateView("views/app/index.html", 48, 48, 720, 460);
            SetUIModeView(s_playgroundId);
            RegisterJSListener(s_playgroundId, "requestPageData", &OnRequestPageData);
            RegisterJSListener(s_playgroundId, "settingChanged", &OnSettingChanged);
            s_badgeId = CreateView("views/badge.html", -228, 12, 216, 44,
                                   nullptr, /*clickThrough=*/true, /*startVisible=*/true);
        }
        // ImageSource probe page — registered last so the toggle key opens it
        // when both dev flags are on. The texture itself is created lazily on
        // the render thread (TickImageProbe).
        if (s_imageProbe) {
            // Dogfood: the probe rides the v4 path (RegisterMod + CreateViewEx + events).
            s_probeViewId = Api4::CreateHostProbeView("views/probe/index.html", 40, 40, 1180, 860);
            SetUIModeView(s_probeViewId);
        }
    }

    // ── Input sink (toggle key + ALL mouse input) ───────────────────────────
    // Skyrim reads the mouse through exclusive DirectInput — the game window
    // never receives WM mouse messages (field-proven: zero across every run).
    // So while UI mode is up, mouse input comes from the game's own event
    // stream: kMouseMove events read the vanilla CursorMenu's position
    // (MenuCursor — the very cursor the user is looking at, in render-target
    // pixels) and mouse ButtonEvents map to synthetic WM_* entries in the same
    // queue the render thread already drains.

    // Cursor position source. MenuCursor is authoritative while the engine
    // drives it — but the engine stops updating it whenever a foreign engine
    // menu without kDontHideCursorWhenTopmost is topmost (SA's item-carrier
    // IMenu during the live mirror: field report 2026-09-01, cursor frozen on
    // the Outfits page). The device deltas keep arriving at the input sink
    // regardless, so: learn the engine's delta→pixel scale while MenuCursor
    // moves, detect a stall (deltas without movement), and integrate the raw
    // deltas ourselves until MenuCursor moves again. Input-sink thread only.
    static int   s_ownCursorX = 0, s_ownCursorY = 0;
    static int   s_lastMenuX = INT_MIN, s_lastMenuY = INT_MIN;
    static int   s_stallEvents = 0;
    static bool  s_integrating = false;
    static float s_deltaScale = 1.0f;                 // engine px per raw count, learned

    static LPARAM PackCursorPos(int dx = 0, int dy = 0)
    {
        int mx = 0, my = 0;
        if (auto* mc = RE::MenuCursor::GetSingleton()) {
            const auto& rd = mc->GetRuntimeData();
            mx = static_cast<int>(rd.cursorPosX);
            my = static_cast<int>(rd.cursorPosY);
        }
        const bool menuMoved = (mx != s_lastMenuX || my != s_lastMenuY);
        if (menuMoved) {
            // Learn the scale from real movement (both axes, EMA, clamped).
            if (s_lastMenuX != INT_MIN && (dx != 0 || dy != 0)) {
                const float rawLen  = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                const float menuLen = std::sqrt(static_cast<float>(
                    (mx - s_lastMenuX) * (mx - s_lastMenuX) + (my - s_lastMenuY) * (my - s_lastMenuY)));
                if (rawLen > 0.0f && menuLen > 0.0f) {
                    const float r = std::clamp(menuLen / rawLen, 0.1f, 10.0f);
                    s_deltaScale = 0.85f * s_deltaScale + 0.15f * r;
                }
            }
            s_lastMenuX = mx;
            s_lastMenuY = my;
            s_stallEvents = 0;
            if (s_integrating) {
                s_integrating = false;
                SKSE::log::info("Magelight: MenuCursor moving again — engine cursor authoritative");
            }
            s_ownCursorX = mx;
            s_ownCursorY = my;
        } else if (dx != 0 || dy != 0) {
            if (!s_integrating && ++s_stallEvents >= 12) {
                s_integrating = true;
                SKSE::log::info("Magelight: MenuCursor stalled under mouse input — integrating raw deltas (scale {:.2f})",
                    s_deltaScale);
            }
            if (s_integrating) {
                const int bw = s_backbufferW.load(), bh = s_backbufferH.load();
                s_ownCursorX = std::clamp(s_ownCursorX + static_cast<int>(std::lround(dx * s_deltaScale)),
                                          0, std::max(0, bw - 1));
                s_ownCursorY = std::clamp(s_ownCursorY + static_cast<int>(std::lround(dy * s_deltaScale)),
                                          0, std::max(0, bh - 1));
            }
        }
        // Mirror for the render thread's own cursor sprite.
        s_cursorPosX.store(s_ownCursorX);
        s_cursorPosY.store(s_ownCursorY);
        return MAKELPARAM(static_cast<WORD>(s_ownCursorX), static_cast<WORD>(s_ownCursorY));
    }

    class InputSink final : public RE::BSTEventSink<RE::InputEvent*> {
    public:
        static InputSink* GetSingleton()
        {
            static InputSink sink;
            return &sink;
        }
        RE::BSEventNotifyControl ProcessEvent(
            RE::InputEvent* const* a_events, RE::BSTEventSource<RE::InputEvent*>*) override
        {
            GameTask::Scope pumpScope;   // posts from here go through the pump (MagelightGameTask.h)
            if (!a_events) return RE::BSEventNotifyControl::kContinue;
            const bool focused = s_focused.load() && !s_renderDead.load();
            // While UI mode is on, keyboard button events END here: the page
            // already received the key through the window proc, and every
            // sink registered after ours (other mods' hotkeys) would
            // otherwise act on a key that was typed into a text field. The
            // event chain is relinked in place — the engine's own sinks run
            // before ours and are unaffected (menu context handles them).
            // Gamepad and mouse events pass through untouched.
            RE::InputEvent* prev = nullptr;
            for (auto* e = *a_events; e; ) {
                RE::InputEvent* next = e->next;
                const auto type = e->GetEventType();
                if (type == RE::INPUT_EVENT_TYPE::kButton) {
                    const auto* b = e->AsButtonEvent();
                    if (!b) { prev = e; e = next; continue; }
                    if (b->GetDevice() == RE::INPUT_DEVICE::kKeyboard) {
                        const auto code = b->GetIDCode();
                        bool consumed = false;   // toggle key / Escape / a bound hotkey: never typed
                        if (b->IsDown()) {
                            if (code == s_toggleKey.load()) { ToggleVisible(); consumed = true; }
                            else if (focused && code == 0x01) {
                                // Escape through the SINK: the window-proc
                                // Escape needs OS focus (VR mirror window,
                                // alt-tabbed game). Idempotent with it.
                                if (EscapeCapturedByUiView()) {
                                    // The page owns Escape (0.28.0). On VR the sink IS the
                                    // key path, so hand it the key; on flat the window proc
                                    // already queued it.
                                    if (VR::IsVRRuntime()) {
                                        QueueInput(WM_KEYDOWN, VK_ESCAPE, 0);
                                        QueueInput(WM_KEYUP, VK_ESCAPE, (1 << 30) | (1 << 31));
                                    }
                                } else {
                                    SKSE::log::info("Magelight: Escape via sink — leaving UI mode");
                                    GameTask::Post([]() { SetUIMode(false); });
                                }
                                consumed = true;
                            }
                            else consumed = Api4::DispatchHotkey(code);   // v4 / manifest hotkey bindings
                        } else if (code == s_toggleKey.load() || code == 0x01) {
                            consumed = true;   // the release of a key we swallowed on press
                        }
                        // VR only: the window gets no key messages, so the page
                        // is typed from the engine device instead. On flat the
                        // window proc already does this — never both.
                        if (focused && !consumed && VR::IsLive())
                            QueueScancodeAsText(code, b->IsDown());
                        if (focused) {
                            // unlink: keys typed into a page stay in the page
                            if (prev) prev->next = next;
                            else *const_cast<RE::InputEvent**>(a_events) = next;
                            e = next;
                            continue;
                        }
                        prev = e; e = next;
                        continue;
                    }
                    // VR controllers (kVivePrimary..kWMRSecondary): record the
                    // edge for the VR presenter's laser (VR-3) and give the
                    // headset a way OUT of UI mode — B/Y on either hand closes,
                    // the VR twin of Escape. Events pass through untouched.
                    {
                        const int dev = static_cast<int>(b->GetDevice());
                        if (dev >= static_cast<int>(RE::INPUT_DEVICE::kVivePrimary) &&
                            dev <= static_cast<int>(RE::INPUT_DEVICE::kWMRSecondary)) {
                            const auto vrcode = b->GetIDCode();
                            VR::NoteButton(dev, vrcode, b->Value());   // analog LEVEL, not IsDown (see MagelightVR.h)
                            if (focused && b->IsDown() && vrcode == RE::BSOpenVRControllerDevice::Keys::kBY) {
                                // A mod that bound B/Y (bare or chorded) for the
                                // UI-mode view owns open/close on it — its callback
                                // fires from the hotkey tick, and running our exit
                                // as well turned one Grip+B/Y into close + re-open
                                // (SA, VR field log 2026-09-12 14:20). Everyone else
                                // keeps the built-in way out.
                                const auto uiView = static_cast<ViewId>(s_uiModeView.load());
                                if (VR::ViewBindsButton(uiView, VR::kButtonApplicationMenu)) {
                                    SKSE::log::info("Magelight: controller B/Y (device {}) — view {} binds it; built-in exit skipped", dev, uiView);
                                } else {
                                    SKSE::log::info("Magelight: controller B/Y (device {}) — leaving UI mode", dev);
                                    GameTask::Post([]() { SetUIMode(false); });
                                }
                            }
                            // M7: while a page is focused the trigger CLICKS the
                            // panel (via the laser's queued WM_LBUTTON*) and must
                            // not also swing the weapon — unlink it so no later
                            // sink sees it. B/Y stays linked (its exit is queued
                            // above); every other control passes through.
                            if (focused && vrcode == RE::BSOpenVRControllerDevice::Keys::kTrigger) {
                                if (prev) prev->next = next;
                                else *const_cast<RE::InputEvent**>(a_events) = next;
                                e = next;
                                continue;
                            }
                            prev = e; e = next;
                            continue;
                        }
                    }
                    if (!focused || b->GetDevice() != RE::INPUT_DEVICE::kMouse) { prev = e; e = next; continue; }
                    if (VR::IsLive()) { prev = e; e = next; continue; }   // VR: the laser is the only pointer
                    using MK = RE::BSWin32MouseDevice::Key;
                    const LPARAM pos = PackCursorPos();
                    switch (b->GetIDCode()) {
                    case MK::kLeftButton:
                        if (b->IsDown()) { QueueInput(WM_LBUTTONDOWN, 0, pos); s_mouseDown.store(true); }
                        else if (b->IsUp()) { QueueInput(WM_LBUTTONUP, 0, pos); s_mouseDown.store(false); }
                        break;
                    case MK::kRightButton:
                        if (b->IsDown()) QueueInput(WM_RBUTTONDOWN, 0, pos);
                        else if (b->IsUp()) QueueInput(WM_RBUTTONUP, 0, pos);
                        break;
                    case MK::kMiddleButton:
                        if (b->IsDown()) QueueInput(WM_MBUTTONDOWN, 0, pos);
                        else if (b->IsUp()) QueueInput(WM_MBUTTONUP, 0, pos);
                        break;
                    case MK::kWheelUp:
                        if (b->IsDown()) QueueInput(WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), pos);
                        break;
                    case MK::kWheelDown:
                        if (b->IsDown())
                            QueueInput(WM_MOUSEWHEEL,
                                MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), pos);
                        break;
                    default:
                        break;
                    }
                } else if (focused && type == RE::INPUT_EVENT_TYPE::kMouseMove && !VR::IsLive()) {
                    const auto* mm = e->AsMouseMoveEvent();
                    QueueInput(WM_MOUSEMOVE, 0,
                        PackCursorPos(mm ? mm->mouseInputX : 0, mm ? mm->mouseInputY : 0));
                } else if (type == RE::INPUT_EVENT_TYPE::kThumbstick) {
                    if (const auto* ts = e->AsThumbstickEvent()) {
                        const int dev = static_cast<int>(ts->GetDevice());
                        if (dev >= static_cast<int>(RE::INPUT_DEVICE::kVivePrimary) &&
                            dev <= static_cast<int>(RE::INPUT_DEVICE::kWMRSecondary)) {
                            VR::NoteThumbstick(dev, ts->xValue, ts->yValue);
                            // M7: don't let the stick turn/move the player while a
                            // page is focused (VR-3.1 will map it to scroll).
                            if (focused) {
                                if (prev) prev->next = next;
                                else *const_cast<RE::InputEvent**>(a_events) = next;
                                e = next;
                                continue;
                            }
                        }
                    }
                }
                prev = e; e = next;
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // Menu traffic while UI mode is on: which engine menus open and close
    // around us. The cursor only moves while a kUsesCursor menu is topmost,
    // so a stray open/close is the first thing to look at when it freezes.
    class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
    public:
        static MenuSink* GetSingleton() { static MenuSink s; return &s; }
        RE::BSEventNotifyControl ProcessEvent(
            const RE::MenuOpenCloseEvent* e, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            if (e && s_focused.load()) {
                SKSE::log::info("Magelight: menu '{}' {} while UI mode is on",
                    e->menuName.c_str() ? e->menuName.c_str() : "?", e->opening ? "OPENED" : "CLOSED");
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // Game thread. If the engine dropped the CursorMenu under us while UI
    // mode is on, put it back (and say so) — without it the MenuCursor
    // position never updates and our cursor sprite stands still. Also keeps
    // the vanilla cursor hidden (HideVanillaCursor).
    static void CursorMenuWatchdog()
    {
        if (!s_focused.load()) return;
        auto* ui = RE::UI::GetSingleton();
        auto* q  = RE::UIMessageQueue::GetSingleton();
        if (!ui || !q) return;
        if (!ui->IsMenuOpen(RE::CursorMenu::MENU_NAME)) {
            SKSE::log::warn("Magelight: CursorMenu is not open while UI mode is on — re-showing it");
            q->AddMessage(RE::CursorMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kShow, nullptr);
        }
        if (s_focusMenuRegistered && !ui->IsMenuOpen(MagelightFocusMenu::MENU_NAME)) {
            SKSE::log::warn("Magelight: focus menu is not open while UI mode is on — re-showing it");
            q->AddMessage(MagelightFocusMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kShow, nullptr);
        }
        HideVanillaCursor(true);
        // OCU re-sets OC_MENU_ACTIVE on every menu open/close; keep it clear
        // for as long as our page owns the pointer.
        if (s_hwnd && VR::SuppressRuntimeLaser())
            SetPropW(s_hwnd, L"OC_MENU_ACTIVE", reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(0)));
    }

    // ── Keyboard mute while UI mode is on ───────────────────────────────
    // The sink unlink above shields only the sinks registered after ours.
    // Mods with a "priority input hook" (Immersive Equipment Displays hooks
    // the dispatcher itself and opens its UI on Backspace, EnableInMenus or
    // not — field 2026-09-03) see the keys before any sink. The engine's
    // keyboard device turns DirectInput state into button events in its
    // Poll (BSIInputDevice vfunc 02); while a page is focused that poll is
    // skipped, so NO keyboard button event exists engine-wide — nothing for
    // a hook, a sink, or a Papyrus OnKeyDown to act on. The page keeps
    // typing through the window proc (WM_KEYDOWN / WM_CHAR are the OS, not
    // DirectInput), and Escape / bound hotkeys keep working through the
    // same path. Mouse and gamepad devices are untouched. While muted the
    // hook still DRAINS DirectInput's buffered queue into the engine's state
    // arrays (see the body) — skipping the read outright let the typed keys
    // replay into the game on unfocus.
    namespace {
        using KeyboardPollFn = void(*)(RE::BSWin32KeyboardDevice*, float);
        KeyboardPollFn s_origKeyboardPoll = nullptr;

        void Hook_KeyboardPoll(RE::BSWin32KeyboardDevice* self, float dt)
        {
            // VR (0.17.3): the mute is NOT applied. Typing, Escape and the
            // close hotkey ride the window proc, which only delivers while
            // the game window owns OS focus — and the SkyrimVR desktop mirror
            // rarely does (field 2026-09-03: F10 opened the panel through the
            // sink, the mute then silenced the only path that could close
            // it). On VR the sink is the keyboard path: it handles Escape
            // and the view's own hotkey below and still unlinks every key
            // for the sinks after ours.
            if (s_focused.load() && !s_renderDead.load() && !VR::IsVRRuntime()) {
                // MUTED — but never idle. The engine reads DirectInput's BUFFERED
                // event queue (GetDeviceData, ten at a time) and applies it to
                // curState; a skipped Poll leaves every key typed into a page
                // queued in DirectInput, and the first real Poll after unfocus
                // replayed the lot into the game (field 2026-09-06: search-box
                // keystrokes opened journal/map/etc. the moment the menu closed).
                // Drain the queue ourselves and apply each event to BOTH state
                // arrays: the engine's held-key picture stays truthful, its
                // unmute diff has nothing to fire, and no button event is ever
                // generated for a key typed into a page. Both arrays carry
                // static offset asserts in CommonLib. (0.27.0)
                if (self) {
                    auto& rd = self->GetRuntimeData();
                    if (auto* dev = reinterpret_cast<REX::W32::IDirectInputDevice8A*>(rd.dInputDevice)) {
                        REX::W32::DIDEVICEOBJECTDATA buf[32];
                        for (int guard = 0; guard < 16; ++guard) {
                            std::uint32_t n = 32;
                            const auto hr = dev->GetDeviceData(sizeof(REX::W32::DIDEVICEOBJECTDATA), buf, &n, 0);
                            if (hr < 0 || n == 0) break;   // not acquired / input lost: nothing to drain
                            for (std::uint32_t i = 0; i < n; ++i) {
                                const std::uint32_t k = buf[i].ofs;
                                if (k < 0x100) rd.curState[k] = rd.prevState[k] = (buf[i].data & 0x80) ? 0x80 : 0;
                            }
                            if (n < 32) break;
                        }
                    }
                }
                return;
            }
            if (s_origKeyboardPoll) s_origKeyboardPoll(self, dt);
        }

        void InstallKeyboardMute()
        {
            static bool s_done = false;
            if (s_done) return;
            s_done = true;
            // NOT ON VR (0.19.2). The mute exists to stop a key typed into a
            // page reaching other mods, and on VR it is already skipped in
            // Hook_KeyboardPoll (0.17.3: the window gets no key messages there,
            // so the sink IS the typing path). Installing it anyway left a
            // permanent vtable patch on a VR runtime for no behaviour at all —
            // and a vtable write is the one thing in our always-on path that
            // could corrupt memory if the address-library id resolved wrongly.
            // The sink's unlink still contains keys while a page is focused.
            if (REL::Module::IsVR()) {
                SKSE::log::info("Magelight: keyboard device poll NOT hooked (VR — the sink is the typing path)");
                return;
            }
            REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_BSWin32KeyboardDevice[0] };
            s_origKeyboardPoll = reinterpret_cast<KeyboardPollFn>(vtbl.write_vfunc(0x2, &Hook_KeyboardPoll));
            SKSE::log::info("Magelight: keyboard device poll hooked (muted while UI mode is on)");
        }
    }

    void RegisterInputSink()
    {
        InstallKeyboardMute();
        // Registered as early as the device manager exists (kInputLoaded):
        // sinks run in registration order, and the keyboard unlink above
        // only shields the sinks that come AFTER ours.
        static bool s_registered = false;
        if (s_registered) return;
        if (auto* mgr = RE::BSInputDeviceManager::GetSingleton()) {
            mgr->AddEventSink(InputSink::GetSingleton());
            s_registered = true;
            SKSE::log::info("Magelight: input sink registered (Page Up toggles UI mode)");
        }
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->AddEventSink(MenuSink::GetSingleton());
        }
    }

    void ToggleVisible()
    {
        // UI-mode toggle (visible + focused + cursor + suspended game
        // controls, all together). Marshalled to the game thread — the sink
        // fires on the input-dispatch path. With no UI-mode view registered
        // (demo views off, no consumer called SetUIModeView) entering would
        // suspend controls over an empty screen — refuse instead. Exits
        // always go through so the key can never strand a stuck mode.
        GameTask::Post([]() {
            if (s_focused.load()) {
                SetUIMode(false);  // exit always works — never a stuck mode
                return;
            }
            const ViewId target = static_cast<ViewId>(s_toggleView.load());
            if (!IsViewValid(target)) {
                SKSE::log::info("Magelight: toggle ignored — no toggle view registered");
                return;
            }
            s_uiModeView.store(target);
            SetUIMode(true);
        });
    }

    void ForceExitUIMode()
    {
        // Safety valve for load boundaries: never carry a suspended-controls /
        // cursor-up state across a save load. Game thread.
        SetUIMode(false);
    }

    void EnterUIMode(ViewId view) { EnterUIModeEx(view, false); }

    void EnterUIModeEx(ViewId view, bool pauseGame, bool noTextEntry)
    {
        GameTask::Post([view, pauseGame, noTextEntry]() {
            if (s_focused.load()) {
                // One surface at a time — callers gate on IsUIModeActive()
                // (SA's mutual-exclusion contract); refuse rather than steal.
                SKSE::log::warn("Magelight: EnterUIMode({}) refused — UI mode already active on view {}",
                    view, s_uiModeView.load());
                Emit(HostEvent::UIModeRefused, view, 0, 0, "UI mode already active");
                return;
            }
            const char* refusal = nullptr;
            {
                std::lock_guard<std::mutex> lk(s_viewsMutex);
                MlView* v = FindViewLocked(view);
                if (!v || v->destroyPending) refusal = "no such view";
                else if (v->clickThrough) refusal = "click-through views cannot take UI mode";
            }
            if (refusal) {
                SKSE::log::warn("Magelight: EnterUIMode({}) — {}", view, refusal);
                Emit(HostEvent::UIModeRefused, view, 0, 0, refusal);
                return;
            }
            s_uiModeView.store(view);
            SetUIMode(true, true, pauseGame, noTextEntry);
        });
    }

    void ExitUIMode()
    {
        // Deliberately does NOT hide the view (Magelight.h contract — SA's
        // Free Look keeps the view rendered while the game owns input;
        // callers that want a hide pair this with ShowView(view, false)).
        GameTask::Post([]() { SetUIMode(false, /*hideView=*/false); });
    }

    bool IsUIModeActive()
    {
        return s_focused.load() && !s_renderDead.load();
    }

    ViewId GetUIModeView()
    {
        return static_cast<ViewId>(s_uiModeView.load());
    }

    std::uint32_t GetToggleKey()
    {
        return s_toggleKey.load();
    }

    std::filesystem::path RuntimeDirPath()
    {
        return s_runtimeDir;
    }

    std::filesystem::path CacheDirPath()
    {
        if (auto dir = SKSE::log::log_directory()) return *dir / "Magelight-cache";
        return {};
    }

    bool DevModeEnabled()
    {
        return s_devMode.load();
    }

    bool InspectorAvailable()
    {
        std::error_code ec;
        return std::filesystem::exists(s_runtimeDir / "inspector" / "Main.html", ec);
    }

    // ── Version gate ──────────────────────────────────────────────────────
    static std::atomic<std::uint64_t> s_gateView{ 0 };
    static void GateDomReady(ViewId view) { InteropCall(view, "setGate", Api4::TooOldJson()); }
    static void GateDismiss(const char*) { if (const ViewId v = static_cast<ViewId>(s_gateView.load())) ShowView(v, false); }

    void ShowVersionGateIfNeeded()
    {
        static bool shown = false;
        if (shown) return;
        const std::string json = Api4::TooOldJson();
        if (json.empty()) return;
        shown = true;
        const ViewId id = CreateView("views/gate/index.html", 40, 120, 640, 200, &GateDomReady,
                                     /*clickThrough=*/true, /*startVisible=*/true);
        if (!id) return;
        SetViewLayer(id, 3);
        RegisterJSListener(id, "gateDismiss", &GateDismiss);
        s_gateView.store(id);
        SKSE::log::warn("Magelight: version gate shown — {}", json);
    }

    void SetUIModePause(bool pause)
    {
        // IN PLACE — never hide/re-show the focus menu (0.26.10 did, and the
        // holder's close detection read the CLOSED as the engine taking the
        // menu away: SeverActions' Live Stage auto-closed the whole UI the
        // moment it asked to unpause, field 2026-09-06). The engine's pause is
        // UI::numPausesGame, bumped by menus carrying kPausesGame on open and
        // dropped on close — so flip the flag on the LIVE menu and move the
        // counter by hand; the eventual close then balances against the flag
        // it sees at that moment.
        const bool was = s_focusMenuPause.exchange(pause);
        if (!s_focused.load() || was == pause) return;   // unfocused: the next Create picks it up
        // The RE::UI part on the GAME thread (review 2026-09-09): RequestUIMode
        // is any-thread by contract and its sibling posts a task; walking the
        // menu map and moving numPausesGame from a worker thread would race the
        // engine's own open/close bookkeeping. Queued AFTER a view switch on the
        // same request, so the pause lands on the view that ends up holding.
        GameTask::Post([pause]() {
            auto* ui = RE::UI::GetSingleton();
            if (!ui) return;
            auto menu = ui->GetMenu(MagelightFocusMenu::MENU_NAME);
            if (!menu) { SKSE::log::info("Magelight: UI mode pause -> {} (focus menu not open; next show carries it)", pause); return; }
            using F = RE::UI_MENU_FLAGS;
            const bool has = menu->menuFlags.all(F::kPausesGame);
            if (pause && !has) {
                menu->menuFlags.set(F::kPausesGame);
                ++ui->numPausesGame;
            } else if (!pause && has) {
                menu->menuFlags.reset(F::kPausesGame);
                if (ui->numPausesGame > 0) --ui->numPausesGame;
            }
            SKSE::log::info("Magelight: UI mode pause -> {} in place (numPausesGame={})", pause, ui->numPausesGame);
        });
    }

    void SwitchUIModeView(ViewId view)
    {
        GameTask::Post([view]() {
            if (!s_focused.load()) {
                Emit(HostEvent::UIModeRefused, view, 0, 0, "switch: UI mode is not active");
                return;
            }
            const char* refusal = nullptr;
            {
                std::lock_guard<std::mutex> lk(s_viewsMutex);
                MlView* v = FindViewLocked(view);
                if (!v || v->destroyPending) refusal = "switch: no such view";
                else if (v->clickThrough) refusal = "switch: click-through views cannot take UI mode";
            }
            if (refusal) {
                SKSE::log::warn("Magelight: SwitchUIModeView({}) — {}", view, refusal);
                Emit(HostEvent::UIModeRefused, view, 0, 0, refusal);
                return;
            }
            const ViewId old = static_cast<ViewId>(s_uiModeView.load());
            if (old == view) { Emit(HostEvent::UIModeSwitched, view); return; }
            s_uiModeView.store(view);
            ShowView(view, true);
            QueueInput(0, 1, 0);   // re-run the focus marker: key focus follows s_uiModeView
            InvokeJS(old, "window.magelight&&window.magelight._dispatch('__uimode','0');");
            InvokeJS(view, "window.magelight&&window.magelight._dispatch('__uimode','1');");
            SKSE::log::info("Magelight: UI mode switched from view {} to view {}", old, view);
            Emit(HostEvent::UIModeSwitched, view);
        });
    }

    void SetUIModeExitCallback(UIModeExitFn cb)
    {
        // Registrations accumulate (one per function pointer); v1-v3 have
        // no unregister — the v4 per-mod event sink replaces this surface.
        if (!cb) return;
        std::lock_guard<std::mutex> lock(s_uiModeExitMutex);
        if (std::find(s_uiModeExitCbs.begin(), s_uiModeExitCbs.end(), cb) ==
            s_uiModeExitCbs.end()) {
            s_uiModeExitCbs.push_back(cb);
        }
    }

    void NotifyWorldReady()
    {
        if (!s_worldReady.exchange(true)) {
            SKSE::log::info("Magelight: world ready — Ultralight will initialize on the next in-game frame");
        }
    }

    ViewId CreateView(const char* htmlPath, int x, int y, int w, int h,
                      DomReadyFn onDomReady, bool clickThrough, bool startVisible,
                      const char* sessionName, NetLevel netLevel)
    {
        // w == 0 && h == 0 = FULLSCREEN: the view tracks the backbuffer size
        // (sized on the render thread before materialization, for SA's
        // implicitly-fullscreen self-scaling pages).
        const bool fullscreen = (w == 0 && h == 0);
        if (!fullscreen && (w <= 0 || h <= 0)) return 0;
        auto v = std::make_unique<MlView>();
        v->id = s_nextViewId.fetch_add(1);
        v->htmlPath = htmlPath ? htmlPath : "";
        v->root = PageRootFor(v->htmlPath);
        NotePageRoot(v->root);   // 0.28.1: MlFileSystem serves it too
        v->x = x; v->y = y; v->w = w; v->h = h;
        v->fullscreen = fullscreen;
        v->visible = startVisible;
        v->clickThrough = clickThrough;
        v->onDomReady = onDomReady;
        v->sessionName = sessionName ? sessionName : "";
        v->netLevel = netLevel;
        const ViewId id = v->id;
        v->order = s_nextViewOrder.fetch_add(1);
        {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            s_views.push_back(std::move(v));
            SortViewsLocked();
        }
        if (fullscreen)
            SKSE::log::info("Magelight: view {} registered ('{}' fullscreen{})",
                id, htmlPath ? htmlPath : "", clickThrough ? ", click-through" : "");
        else
            SKSE::log::info("Magelight: view {} registered ('{}' {}x{} at {},{}{})",
                id, htmlPath ? htmlPath : "", w, h, x, y, clickThrough ? ", click-through" : "");
        return id;
    }

    bool IsViewValid(ViewId view)
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        MlView* v = FindViewLocked(view);
        return v && !v->destroyPending;
    }

    void ShowView(ViewId view, bool show)
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) {
            if (v->visible != show) v->hiddenSince = show ? 0 : GetTickCount64();
            v->visible = show;
            if (!show) v->pageCursor = 0;   // reopened, the page reports again on the first mouse move
        }
    }

    void SetViewCutout(ViewId view, int x, int y, int w, int h)
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) {
            if (w <= 0 || h <= 0) { v->cutW = v->cutH = 0; return; }
            v->cutX = static_cast<float>(x); v->cutY = static_cast<float>(y);
            v->cutW = static_cast<float>(w); v->cutH = static_cast<float>(h);
        }
    }

    void SetViewScale(ViewId view, float scale)
    {
        // Below 1 Ultralight clips the page to scale squared of the view (a 0.8 scale draws only the top-left
        // 64%), so a smaller page takes a CSS transform instead.
        if (scale < 1.0f)
            SKSE::log::info("Magelight: view {} device scale {:.3f} raised to 1.000 (below 1 clips the page)", view, scale);
        scale = std::clamp(scale, 1.0f, 3.0f);
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        MlView* v = FindViewLocked(view);
        if (!v || v->destroyPending) return;
        v->deviceScale = scale;
        if (v->ul) {
            // Applied by the render thread in ApplyPendingResizes:
            // set_device_scale re-lays out the page synchronously, and this
            // is the API caller's thread (review 2026-09-09) — the same
            // intent-flag shape as a resize.
            v->scaleDirty  = true;
            v->boundsDirty = true;            // the Resize that follows re-rasterises
        }
        SKSE::log::info("Magelight: view {} device scale {:.3f}", view, scale);
    }

    void SetViewHibernate(ViewId view, std::uint32_t idleMs)
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) v->hibernateMs = idleMs;
    }

    void SetViewBounds(ViewId view, int x, int y, int w, int h)
    {
        if (w <= 0 || h <= 0) return;
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) {
            v->fullscreen = false;  // explicit bounds end backbuffer tracking
            v->x = x; v->y = y;
            if (v->w != w || v->h != h) {
                v->w = w; v->h = h;
                v->boundsDirty = true;  // render thread applies the Resize
            }
        }
    }

    // Names the host's page script owns (see RegisterJSListener in Magelight.h).
    static bool RefuseReservedListenerName(ViewId view, const char* name)
    {
        if (!(name[0] == '_' && name[1] == '_') && std::strcmp(name, "magelight") != 0) return false;
        static std::mutex s_mx;
        static std::set<std::string> s_logged;
        bool log = false;
        {
            std::lock_guard<std::mutex> lk(s_mx);
            log = s_logged.size() < 64 && s_logged.insert(name).second;
        }
        if (log) SKSE::log::warn("Magelight: listener name '{}' refused (view {}) — 'magelight' and names starting "
                                 "with '__' belong to the host's page script (logged once per name)", name, view);
        return true;
    }

    bool RegisterJSListener(ViewId view, const char* name, JsListenerFn callback)
    {
        if (!name || !*name || !callback) return false;
        if (RefuseReservedListenerName(view, name)) return false;
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) {
            v->listeners[name] = MlView::Listener{ callback, nullptr, nullptr };
            // A registration after window-object-ready (SA's, from OnDomReady)
            // marks the view: the render thread re-creates __mlNative and every
            // listener shim, leaving window.magelight and its subscriptions alone.
            v->shimsDirty = true;
            return true;
        }
        return false;
    }

    void InteropCall(ViewId view, const char* functionName, const std::string& argument)
    {
        if (!functionName || !*functionName) return;
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) {
            if (v->outbound.size() < 256) v->outbound.push_back({ functionName, argument, false });
        }
    }

    void EvalJS(ViewId view, const std::string& script, JsResultFnInternal fn, void* user)
    {
        bool queued = false;
        {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            if (MlView* v = FindViewLocked(view); v && !v->destroyPending && v->evals.size() < 64) {
                v->evals.push_back({ script, fn, user });
                queued = true;
            }
        }
        if (!queued && fn) fn(view, "", "Magelight: view is gone or its eval queue is full", user);
    }

    void InvokeJS(ViewId view, const std::string& script)
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) {
            if (v->outbound.size() < 256) v->outbound.push_back({ script, {}, true });
        }
    }

    void SetUIModeView(ViewId view)
    {
        s_uiModeView.store(view);
        s_toggleView.store(view);  // the toggle key opens this view
    }

    // ── v4 internals ────────────────────────────────────────────────────────

    bool RegisterJSListenerEx(ViewId view, const char* name, JsListenerExFn callback, void* user)
    {
        if (!name || !*name || !callback) return false;
        if (RefuseReservedListenerName(view, name)) return false;
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) {
            v->listeners[name] = MlView::Listener{ nullptr, callback, user };
            v->shimsDirty = true;
            return true;
        }
        return false;
    }

    bool DestroyView(ViewId view)
    {
        // Refuse while UI mode is up on this view. A queued EnterUIModeEx
        // that lands after this point sees destroyPending and refuses, so
        // s_uiModeView can never name a destroyed id.
        if (s_focused.load() && s_uiModeView.load() == view) return false;
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        MlView* v = FindViewLocked(view);
        if (!v || v->destroyPending) return false;
        v->visible = false;
        v->destroyPending = true;
        if (s_toggleView.load() == view) s_toggleView.store(0);
        if (s_uiModeView.load() == view) s_uiModeView.store(0);
        return true;
    }

    bool ReloadView(ViewId view)
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        MlView* v = FindViewLocked(view);
        if (!v || v->destroyPending) return false;
        v->reloadPending = true;
        v->domReady = false;   // hold outbound calls for the new page
        return true;
    }

    bool NavigateView(ViewId view, const char* urlOrPath)
    {
        if (!urlOrPath || !*urlOrPath) return false;
        std::string u = urlOrPath;
        if (u.rfind("file://", 0) != 0) {
            const bool absolute = u.size() > 1 && u[1] == ':';
            u = absolute ? PathToFileUrl(std::filesystem::path(u)) : ("file:///" + u);
        }
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        MlView* v = FindViewLocked(view);
        if (!v || v->destroyPending) return false;
        v->navigateUrl = u;
        v->domReady = false;   // hold outbound calls for the new page
        return true;
    }

    void SetViewLayer(ViewId view, int layer)
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) {
            v->layer = layer;
            v->order = s_nextViewOrder.fetch_add(1);
            SortViewsLocked();
        }
    }

    // ── 0.28.0 ──
    void SetViewEscapeCapture(ViewId view, bool capture)
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) v->escapeCapture = capture;
    }
    void SetViewNetworkLevel(ViewId view, NetLevel level)
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) v->netLevel = level;
    }

    void ShowInspectorFor(ViewId page, bool show)
    {
        if (show) { if (s_inspectorVisibleFor.load() != page) s_inspectorRequest.store(page); }
        else      { s_inspectorHideRequest.store(page); }
    }

    bool IsInspectorVisibleFor(ViewId page) { return s_inspectorVisibleFor.load() == page; }

    void SetViewOrder(ViewId view, std::uint64_t order)
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) {
            v->order = order;
            // keep RaiseView's "above everything" promise
            std::uint64_t cur = s_nextViewOrder.load();
            while (cur <= order && !s_nextViewOrder.compare_exchange_weak(cur, order + 1)) {}
            SortViewsLocked();
        }
    }

    std::uint64_t GetViewOrder(ViewId view)
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        const MlView* v = FindViewLocked(view);
        return v ? v->order : 0;
    }

    void SetViewScrollStep(ViewId view, int px)
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) v->scrollStep = std::clamp(px, 1, 1000);
    }

    void SetViewSounds(ViewId view, const char* open, const char* close)
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) {
            v->soundOpen  = open  ? open  : "";
            v->soundClose = close ? close : "";
        }
    }

    void RaiseView(ViewId view)
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) {
            v->order = s_nextViewOrder.fetch_add(1);
            SortViewsLocked();
        }
    }

    bool GetViewInfo(ViewId view, ViewInfo& out)
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        MlView* v = FindViewLocked(view);
        if (!v || v->destroyPending) return false;
        out.x = v->x; out.y = v->y; out.w = v->w; out.h = v->h;
        out.visible = v->visible; out.fullscreen = v->fullscreen; out.clickThrough = v->clickThrough;
        out.layer = v->layer;
        out.htmlPath = v->htmlPath;
        out.deviceScale = v->deviceScale;
        return true;
    }

    void GetDisplaySize(int& w, int& h)
    {
        w = s_backbufferW.load();
        h = s_backbufferH.load();
    }

    bool IsRenderDead()
    {
        return s_renderDead.load();
    }

    const char* RenderDeadReason()
    {
        return RenderDeadReasonText();
    }

    std::filesystem::path GameRootPath()
    {
        return GameRoot();
    }

    bool PathIsUnderGameRoot(const std::filesystem::path& p)
    {
        return PathIsUnder(p, GameRoot());
    }

    bool PathIsUnderDir(const std::filesystem::path& p, const std::filesystem::path& base)
    {
        return PathIsUnder(p, base);
    }

    void SetHostEventSink(HostEventFn fn)
    {
        s_hostEventSink.store(fn);
    }

    // ── Texture-backed images (public API) ──────────────────────────────────

    bool IsGpuAccelerated()
    {
        return s_gpuActive;
    }

    // The name becomes a file name AND a page-visible identifier — keep it
    // to a safe alphabet.
    static bool SafeImageName(const std::string& n)
    {
        if (n.empty() || n.size() > 64) return false;
        for (const char c : n) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
            if (!ok) return false;
        }
        return true;
    }

    ImageId RegisterTextureImage(const char* name, ID3D11ShaderResourceView* srv,
                                 std::uint32_t width, std::uint32_t height)
    {
        const std::string n = name ? name : "";
        if (!SafeImageName(n) || !srv || !width || !height) return 0;
        if (!s_gpuRegExt) {
            SKSE::log::warn("Magelight: RegisterTextureImage('{}') — GPU backend lacks external-texture support", n);
            return 0;
        }
        // The .imgsrc the page references in place of an image URL: two
        // lines, magic + identifier (ImageSource.h).
        std::error_code ec;
        const auto dir = s_runtimeDir / L"images";
        std::filesystem::create_directories(dir, ec);
        const auto file = dir / (n + ".imgsrc");
        {
            std::ofstream f(file, std::ios::binary | std::ios::trunc);
            if (!f) {
                SKSE::log::error("Magelight: RegisterTextureImage('{}') — cannot write {}", n, file.string());
                return 0;
            }
            f << "IMGSRC-V1\n" << n;
        }
        srv->AddRef();
        auto im = std::make_unique<MlImage>();
        im->id = s_nextImageId.fetch_add(1);
        im->name = n;
        im->w = width;
        im->h = height;
        im->srv = srv;
        im->url = PathToFileUrl(file);
        const ImageId id = im->id;
        {
            std::lock_guard<std::mutex> lk(s_imagesMutex);
            s_images.push_back(std::move(im));
        }
        SKSE::log::info("Magelight: image '{}' queued as image {} ({}x{})", n, id, width, height);
        return id;
    }

    void UpdateTextureImage(ImageId image, ID3D11ShaderResourceView* srv)
    {
        if (!srv) return;
        std::lock_guard<std::mutex> lk(s_imagesMutex);
        MlImage* im = FindImageLocked(image);
        if (!im || im->removed) return;
        srv->AddRef();
        if (im->pendingSrv) im->pendingSrv->Release();
        im->pendingSrv = srv;
    }

    void InvalidateImage(ImageId image)
    {
        std::lock_guard<std::mutex> lk(s_imagesMutex);
        if (MlImage* im = FindImageLocked(image)) im->dirty = true;
    }

    void UnregisterImage(ImageId image)
    {
        std::lock_guard<std::mutex> lk(s_imagesMutex);
        if (MlImage* im = FindImageLocked(image)) im->unregister = true;
    }

    const char* ImageUrl(ImageId image)
    {
        std::lock_guard<std::mutex> lk(s_imagesMutex);
        MlImage* im = FindImageLocked(image);
        return im ? im->url.c_str() : "";
    }

}  // namespace Magelight

// ── VR keyboard delivery hook (optional) ────────────────────────────────────
// The host's own on-panel VR keyboard (raised on text-field focus), the
// SteamVR overlay keyboard, and OpenComposite's WM_OC_CHAR window message all
// type into a focused page already — this export needs NONE of them and is a
// generic hook on top: an external tool (a custom keyboard, an accessibility
// aid, a VR runtime that prefers a named entry point) can resolve
// GetProcAddress(GetModuleHandle("Magelight.dll"), "Magelight_DeliverChar" /
// "Magelight_DeliverVKey") and feed characters in (printable chars as wchar_t;
// Backspace/Enter/arrows as a Windows VK). They land on the host's ordinary
// queued-input path (the same one WM_CHAR / WM_KEYDOWN take from the window
// proc), on the UI-mode view, exactly like typed keys. Gated: only while a
// page is focused AND the VR presenter is live; dropped otherwise. Any thread
// (the queue is mutex-guarded).
namespace {
    void DeliverCharImpl(wchar_t ch)
    {
        if (!Magelight::VR::IsLive()) return;
        if (!Magelight::s_focused.load() || Magelight::s_renderDead.load()) return;
        if (ch < 0x20 && ch != L'\t' && ch != L'\r' && ch != L'\n') return;
        // RawKeyDown (unknown vk) -> Char -> KeyUp.
        Magelight::QueueInput(WM_KEYDOWN, 0, 0);
        Magelight::QueueInput(WM_CHAR, static_cast<WPARAM>(ch), 0);
        Magelight::QueueInput(WM_KEYUP, 0, static_cast<LPARAM>(1u << 30) | static_cast<LPARAM>(1u << 31));
    }
    void DeliverVKeyImpl(int vk)
    {
        if (!Magelight::VR::IsLive()) return;
        if (!Magelight::s_focused.load() || Magelight::s_renderDead.load()) return;
        if (vk <= 0 || vk > 0xFF) return;
        const LPARAM scan = static_cast<LPARAM>(MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC) & 0xFF) << 16;
        Magelight::QueueInput(WM_KEYDOWN, static_cast<WPARAM>(vk), scan);
        Magelight::QueueInput(WM_KEYUP, static_cast<WPARAM>(vk), scan | static_cast<LPARAM>(1u << 30) | static_cast<LPARAM>(1u << 31));
    }
}
extern "C" __declspec(dllexport) void Magelight_DeliverChar(wchar_t ch) { DeliverCharImpl(ch); }
extern "C" __declspec(dllexport) void Magelight_DeliverVKey(int vkCode) { DeliverVKeyImpl(vkCode); }
