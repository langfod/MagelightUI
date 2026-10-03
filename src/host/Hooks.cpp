// Hooks - the DXGI surface: ResizeBuffers, the Present/Present1 vtable
// hooks, the late MinHook detours, and the hook install (stage 4.8).
//
// One TU, like src/host/Input.cpp and src/host/Render.cpp: the four present
// entry points share t_presentDepth / t_composited, which is how a wrapper
// whose Present calls Present1 (or a vtable hook sitting above the late
// detour) is made to composite exactly once per present. Splitting the hooks
// would put that pair of thread_locals and the one-composite-per-present rule
// in two files, and the rule is the whole reason these four are siblings.
//
// This file also owns the Magelight.json parser. That is a drag-along, not a
// design choice: InstallHook is its ONLY caller, and s_presentHookMode is read
// only by InstallLatePresent. Leaving the parser in Magelight.cpp would mean
// this file reached back for its own body. stage 7 (see
// local_projects/stage-7-dev-surfaces.md) is where the dev surfaces - the
// demo views, the probe - get compile-time gating; they are already out of the
// main pipeline here.
//
// Invariants this file carries:
//   1   no Ultralight work here. ComposeOnce only calls into the frame pass,
//       which gates on s_ulThreadId itself.
//   2   every present entry point holds GameTask::Scope, so posts made from
//       here go through the pump; InstallHook calls GameTask::Start() BEFORE
//       the first present so no thread is created inside one.
//   15  nothing here opens UI mode; DispatchHotkey owns that decision.
//   13  views are erased only in ApplyPendingLifecycle (src/host/Render.cpp),
//       never below.
//
// The bodies below were moved verbatim from src/Magelight.cpp. What crossed a
// boundary, and why:
//
//   - LoadHostSettings and s_presentHookMode came along (see above).
//   - The demo handlers came along: only InstallHook registers them.
//   - Nothing here had to be de-staticed except InstallHook, which was already
//     external (Magelight.h). The rest are file-local on purpose: exporting a
//     hook to the world is how the C4211 trap gets set (see host/State.h).

#include "Magelight.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <windows.h>
#include <intrin.h>
#include <MinHook.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <d3d11.h>
#include <dxgi1_2.h>

#include "HotkeyNames.h"
#include "MagelightApi4.h"
#include "MagelightGameTask.h"
#include "MagelightManifest.h"
#include "MagelightVR.h"
#include "host/Input.h"
#include "host/Render.h"
#include "host/State.h"
#include "host/Views.h"

namespace Magelight {

// Hooks, moved verbatim from src/Magelight.cpp. The banner comments are kept
// deliberately - several record a field-proven measurement.

    // ── settings ──
    // Magelight.json "presentHook": "auto" (late composite only behind an earlier Present hook on a plain dxgi
    // swapchain; never behind a proxy, never on VR), "late" (always try it), "vtable" (never). See
    // InstallLatePresent.
    static std::string s_presentHookMode = "auto";
    // Magelight.json "composite": where views are drawn onto the frame. "present" draws them over the back buffer
    // at Present; "ui" draws them in the game's UI pass (MagelightOverlayMenu::PostDisplay), so a mod that keeps
    // the game's UI apart from the scene (Skyrim Upscaler's HUD Fix) keeps ours with it. "auto" uses "ui" only
    // once the composite is SEEN failing at Present (s_uiPassFallback), never on VR. See UiCompositeWanted.
    //
    // DEFINED in src/host/Render.cpp (stage 4.7), declared in host/Render.h:
    // this file only writes it (LoadHostSettings), the render side reads it.
    // The same shape as s_toggleKey - do NOT re-add a file-local copy here.
    // (C4211, fixed 2026-10-03, commit 2c41fa7.)
    extern std::string s_compositeMode;   // defined in host/Render.cpp

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

    // ── demo ──
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

    // ── hooks ──
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
    static std::atomic<bool> s_lateComposite{ false };
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
                Emit(HostEvent::RenderDead, 0);
                // HideOverlayCarrier (src/host/Render.cpp) owns the carrier's
                // name and posts the hide through the game task pump, so this
                // stays a one-liner and the menu class stays file-local there.
                HideOverlayCarrier();
            }
            if (s_focused.load()) GameTask::Post([]() { SetUIMode(false); });
        }
    }

    // The outermost present of a thread ends here; the next one may composite again. In late mode a vtable
    // present whose chain never reached the detour (the plugin under us presented another way) is a miss, and
    // 30 in a row fall back to compositing in the vtable hook, so the UI is never lost for good.
    //
    // But a vtable composite only helps when it REACHES the screen. Under a frame generator it does not (the
    // last present is the generator's own, on another device), and behind Skyrim Upscaler's HUD Fix it does not
    // either. Both show up here identically — the detour installs and never composites — so on that fallback
    // the carrier menu is brought up as well and the views move into the game's UI pass, where they are drawn
    // as part of the menus and survive being composited downstream. 30 misses is proof enough of "not
    // reaching the screen" to act on, and it is measured from the DX state rather than from any mod's name.
    static void EndPresent(bool vtableHook)
    {
        if (--t_presentDepth > 0) return;
        if (vtableHook && s_lateComposite.load()) {
            if (t_composited) {
                s_lateMisses.store(0);
            } else if (s_lateMisses.fetch_add(1) + 1 >= 30) {
                s_lateComposite.store(false);
                SKSE::log::warn("Magelight: late composite did not draw in 30 frames (dxgi's Present not reached, or not on the game's device) - compositing in the vtable hook again");
                EnableUiPassOnFallback();
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

    // Is a d3d11.dll proxy loaded (ENB, ReShade)? DxgiSwapChainVtable reaches for the system d3d11.dll BY FULL
    // PATH, but LoadLibrary resolves a basename against the loaded-module list FIRST — so with a proxy of that
    // name already in the process it hands back the PROXY, not the system DLL. That is why 0.30.2 crashed the
    // game at start behind ENB and why "auto" refused every proxy: the refusal was aimed at the wrong layer.
    // A dxgi-layer proxy (Community Shaders' CommunityShaders.dll / sl.interposer.dll, Skyrim Upscaler) leaves
    // d3d11.dll alone, so the throwaway WARP device is made through genuine system code and is safe.
    static bool D3D11Proxied()
    {
        return !IsSystemModule(GetModuleHandleW(L"d3d11.dll"), L"d3d11.dll");
    }

    // "auto" arms the late detour behind an earlier Present hook, and now also behind a swapchain proxy that has
    // not replaced d3d11.dll (see D3D11Proxied). Behind a d3d11.dll proxy it still declines: the throwaway
    // device the detour needs for dxgi's own swapchain cannot be made safely there. Never on VR.
    static void InstallLatePresent(void* const* vtbl, void* const* vtbl1)
    {
        const std::string& mode = s_presentHookMode;
        if (mode == "vtable") {
            SKSE::log::info("Magelight: presentHook 'vtable' - compositing in the vtable hook");
            return;
        }
        const bool earlierHook = ModuleAt(reinterpret_cast<const void*>(s_origPresent)) != ModuleAt(vtbl);
        HMODULE dxgi = ModuleAt(vtbl);
        const bool proxy = !IsSystemModule(dxgi, L"dxgi.dll");
        // Behind a proxy the detour is the ONLY place our quads can land last, so it is armed whether or not
        // another mod hooked Present first: the proxy itself is what will otherwise draw over them.
        if (mode != "late" && !proxy && (!earlierHook || REL::Module::IsVR())) {
            SKSE::log::info("Magelight: presentHook 'auto' - {}, compositing in the vtable hook",
                REL::Module::IsVR() ? "VR" : "no earlier Present hook");
            return;
        }
        if (proxy) {
            if (mode != "late" && D3D11Proxied()) {
                SKSE::log::info("Magelight: presentHook 'auto' - the game swapchain is a proxy ({}) behind a "
                                "d3d11.dll proxy, compositing in the vtable hook", ModuleOf(vtbl));
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
        sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&s_gameDevice));
        vtbl[kPresentSlot] = reinterpret_cast<void*>(&HookPresent);
        VirtualProtect(&vtbl[kPresentSlot], sizeof(void*), oldProt, &oldProt);
        // dxgi.dll here is a plain swapchain; anything else (an ENB d3d11.dll, an upscaler or frame-generation
        // proxy) wraps it, and what it presents is not necessarily the buffer we draw into.
        SKSE::log::info("Magelight: hooked Present was {} (vtable in {}); game swapchain {}",
            ModuleOf(reinterpret_cast<const void*>(s_origPresent)), ModuleOf(vtbl), DescribeSwapChain(sc));
        void** vtbl1 = nullptr;
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&sc1))) && sc1) {
            vtbl1 = *reinterpret_cast<void***>(sc1);
            constexpr int kPresent1Slot = 22;
            if (VirtualProtect(&vtbl1[kPresent1Slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt)) {
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
        RegisterOverlayMenu();   // inert until a view needs it; see RegisterOverlayMenu
        LogCompositeMode();      // which stage views start at, before any observation
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

}  // namespace Magelight
