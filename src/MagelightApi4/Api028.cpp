// v4 API — 0.28.0 onward: view chrome, network policy, UI sounds, eval, hotkeys.
//
// One file per ABI release, as with Api018.cpp. The `// ── 0.28.0 ──` marker
// this file replaces is the durable record of which release introduced what.
//
// This is the newest tier, so unlike Api.cpp it has no consumer-facing contract
// to stay byte-identical to beyond the entry points themselves — but they are
// still append-only (CLAUDE.md invariant 10).

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include "Magelight.h"
#include "MagelightApi4.h"
#include "MagelightGameTask.h"
#include "MagelightSound.h"
#include "MagelightApi4/Registry.h"

#include <windows.h>

namespace Magelight::Api4 {

    using namespace MAGELIGHT_API;

    namespace {
        // The eval trampoline's context: EvalJS allocates it, the trampoline
        // (which the host calls back on the render thread) owns and frees it.
        struct EvalCtx { Sink sink; MAGELIGHT_API::JsResultFn cb; void* user; };

        // A mod's task under an SEH net, like its event handler. POD-only frame; 0 = it ran clean.
        DWORD RunGuardedTask(MAGELIGHT_API::GameTaskFn fn, void* user)
        {
            __try {
                fn(user);
                return 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return GetExceptionCode();
            }
        }

        // The file name of the module holding `address` ("?" when none), to name a faulting mod.
        std::string ModuleNameAt(const void* address)
        {
            HMODULE mod = nullptr;
            char path[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   static_cast<LPCSTR>(address), &mod))
                GetModuleFileNameA(mod, path, MAX_PATH);
            const std::string full(path);
            const auto slash = full.find_last_of("\\/");
            return slash == std::string::npos ? full : full.substr(slash + 1);
        }

        // A hotkey must never OPEN over an engine menu, the console, or a text
        // field the game owns; closing is always allowed (CLAUDE.md invariant 15).
        bool EngineBlocksHotkey()
        {
            if (auto* ui = RE::UI::GetSingleton()) {
                if (ui->GameIsPaused() || ui->IsMenuOpen(RE::Console::MENU_NAME)) return true;
            }
            if (auto* cm = RE::ControlMap::GetSingleton()) {
                if (cm->GetRuntimeData().textEntryCount > 0) return true;
            }
            return false;
        }
    }

    // ── 0.28.0: view chrome ─────────────────────────────────────────────────

    Result SetEscapeCapture(ViewId view, bool capture)
    {
        if (const Result g = GateView(view, "SetEscapeCapture"); g != Result::Ok) return g;
        Magelight::SetViewEscapeCapture(view, capture);
        return Result::Ok;
    }

    Result ShowInspector(ViewId view, bool show)
    {
        if (const Result g = GateView(view, "ShowInspector"); g != Result::Ok) return g;
        Magelight::ShowInspectorFor(view, show);
        return Result::Ok;
    }

    Result IsInspectorVisible(ViewId view, bool* outVisible)
    {
        if (!outVisible) return Fail(OwnerOf(view), Result::InvalidArgument, "IsInspectorVisible: outVisible is null");
        if (const Result g = GateView(view, "IsInspectorVisible"); g != Result::Ok) { *outVisible = false; return g; }
        *outVisible = Magelight::IsInspectorVisibleFor(view);
        return Result::Ok;
    }

    Result SetViewOrder(ViewId view, std::uint64_t order)
    {
        if (const Result g = GateView(view, "SetViewOrder"); g != Result::Ok) return g;
        Magelight::SetViewOrder(view, order);
        return Result::Ok;
    }

    Result GetViewOrder(ViewId view, std::uint64_t* outOrder)
    {
        if (!outOrder) return Fail(OwnerOf(view), Result::InvalidArgument, "GetViewOrder: outOrder is null");
        if (const Result g = GateView(view, "GetViewOrder"); g != Result::Ok) { *outOrder = 0; return g; }
        *outOrder = Magelight::GetViewOrder(view);
        return Result::Ok;
    }

    Result SetScrollStep(ViewId view, int px)
    {
        if (const Result g = GateView(view, "SetScrollStep"); g != Result::Ok) return g;
        Magelight::SetViewScrollStep(view, px);
        return Result::Ok;
    }

    // Stored on the mod (so views created later inherit it) and pushed to the
    // views it already has (PushNetLevel). Logged only when the level changes,
    // so a script re-applying it on every load stays quiet.
    Result SetNetworkPolicy(ModId mod, MAGELIGHT_API::NetworkPolicy policy)
    {
        using Magelight::NetLevel;
        NetLevel level = NetLevel::File;
        switch (policy) {
        case MAGELIGHT_API::NetworkPolicy::FileOnly:     level = NetLevel::File; break;
        case MAGELIGHT_API::NetworkPolicy::LoopbackOnly: level = NetLevel::Loopback; break;
        case MAGELIGHT_API::NetworkPolicy::Any:          level = NetLevel::Any; break;
        default:
            return Fail(mod, Result::InvalidArgument,
                        "SetNetworkPolicy: unknown policy " + std::to_string(static_cast<int>(policy)));
        }
        std::vector<ViewId> views;
        std::string name;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(s_mutex);
            Mod* m = FindLocked(mod);
            if (!m) return Result::InvalidMod;
            changed = (m->net != level);
            m->net = level;
            name = m->modId;
            for (const auto& [id, rec] : m->views) views.push_back(id);
        }
        const std::size_t viewCount = views.size();
        PushNetLevel(mod, std::move(views), level);
        if (changed) {
            SKSE::log::info("Magelight[v4]: mod {} ('{}') network policy {} ({} view(s))", mod, name,
                level == NetLevel::Any      ? "ANY — its pages may reach the internet" :
                level == NetLevel::Loopback ? "loopback — its pages may also reach http(s) servers on this machine" :
                                              "file only — its pages reach nothing over the network",
                viewCount);
        }
        return Result::Ok;
    }

    // 0.29.0. Any thread: the play is a game-thread task
    // (BSAudioManager). No visibility gate on this route — a DLL playing
    // `cancel` on a refused action is exactly what the API call is for.
    Result PlayUISound(ViewId view, const char* name)
    {
        if (const Result g = GateView(view, "PlayUISound"); g != Result::Ok) return g;
        if (!name || !*name) return Fail(OwnerOf(view), Result::InvalidArgument, "PlayUISound: empty name");
        const std::string n(name);
        Magelight::GameTask::Post([view, n]() { Magelight::Sound::Play(n, view); });
        return Result::Ok;
    }

    Result SetViewSounds(ViewId view, const char* open, const char* close)
    {
        if (const Result g = GateView(view, "SetViewSounds"); g != Result::Ok) return g;
        Magelight::SetViewSounds(view, open, close);
        return Result::Ok;
    }

    // ── Game tasks (0.30.1) ────────────────────────────────────────────────

    // Any thread; inside one of the host's scopes (a callback inside a frame,
    // its input, window-message, engine-menu, SKSE-message or Papyrus handling)
    // the post goes through the pump, elsewhere straight to SKSE
    // (MagelightGameTask.h).
    Result PostGameTask(MAGELIGHT_API::GameTaskFn fn, void* user)
    {
        if (!fn) {
            SKSE::log::warn("Magelight[v4]: PostGameTask refused - null function");
            return Result::InvalidArgument;
        }
        if (!SKSE::GetTaskInterface()) return Result::NotReady;
        Magelight::GameTask::Post([fn, user]() {
            if (const DWORD code = RunGuardedTask(fn, user); code != 0)
                SKSE::log::error("Magelight[v4]: a PostGameTask function in {} faulted (0x{:08X}) - logged and skipped",
                                 ModuleNameAt(reinterpret_cast<const void*>(fn)), code);
        });
        return Result::Ok;
    }

    // ── EvalJS (0.14.0) ────────────────────────────────────────────────────

    // Render thread, right after the script ran. Hands the strings to the mod
    // on ITS thread; the alive token drops a result queued for a mod that
    // unregistered meanwhile.
    void EvalTrampoline(ViewId view, const char* result, const char* exception, void* user)
    {
        std::unique_ptr<EvalCtx> ctx(static_cast<EvalCtx*>(user));
        if (!ctx || !ctx->cb || !ctx->sink.alive || !ctx->sink.alive->load()) return;
        if (ctx->sink.thread == CallbackThread::RenderThread) {
            if (!CallGuardedResult(ctx->cb, view, result, exception, ctx->user, ctx->sink.id)) DisableSink(ctx->sink.id);
            return;
        }
        Magelight::GameTask::Post([cb = ctx->cb, u = ctx->user, alive = ctx->sink.alive, id = ctx->sink.id,
                                           view, r = std::string(result ? result : ""),
                                           e = std::string(exception ? exception : "")]() {
            if (!alive->load()) return;
            if (!CallGuardedResult(cb, view, r.c_str(), e.c_str(), u, id)) DisableSink(id);
        });
    }

    Result EvalJS(ViewId view, const char* script, MAGELIGHT_API::JsResultFn cb, void* user)
    {
        const ModId owner = OwnerOf(view);
        if (!owner) return Result::InvalidView;
        if (!script || !cb) return Fail(owner, Result::InvalidArgument, "EvalJS: script and callback are required");
        if (!Magelight::IsViewValid(view)) return Fail(owner, Result::InvalidView, "EvalJS: the view is gone");
        Sink sink;
        {
            std::lock_guard<std::mutex> lk(s_mutex);
            const Mod* m = FindLocked(owner);
            if (!m) return Result::InvalidMod;
            sink = SinkOf(*m);
        }
        Magelight::EvalJS(view, script, &EvalTrampoline, new EvalCtx{ sink, cb, user });
        return Result::Ok;
    }

    // ── Hotkeys ────────────────────────────────────────────────────────────

    bool DispatchHotkey(std::uint32_t dxScancode, const char* source)
    {
        static std::uint32_t s_lastCode = 0;
        static std::uint64_t s_lastTick = 0;
        Hotkey hk{};
        {
            std::lock_guard<std::mutex> lk(s_mutex);
            auto it = s_hotkeys.find(dxScancode);
            if (it == s_hotkeys.end()) return false;
            hk = it->second;
            const std::uint64_t now = GetTickCount64();
            if (dxScancode == s_lastCode && now - s_lastTick < 250) return true;   // second source, same press
            s_lastCode = dxScancode;
            s_lastTick = now;
        }
        DispatchViewAction(hk.view, hk.action, source);
        return true;
    }

    void DispatchViewAction(ViewId view, std::uint32_t action, const char* source)
    {
        const Hotkey hk{ view, action };
        const std::string src = source ? source : "sink";
        Magelight::GameTask::Post([hk, src]() {
            Magelight::ViewInfo info;
            if (!Magelight::GetViewInfo(hk.view, info)) return;
            const bool active = Magelight::IsUIModeActive();
            SKSE::log::info("Magelight[v4]: hotkey for view {} pressed via {} (action {}, UI mode {}{})",
                hk.view, src, hk.action, active ? "active on view " : "off",
                active ? std::to_string(Magelight::GetUIModeView()) : std::string());
            if (hk.action == kHotkeyActionToggleVisible) {
                Magelight::ShowView(hk.view, !info.visible);
                return;
            }
            if (active) {
                // While a page has key focus only ITS OWN key closes it — a
                // letter typed into a text field must never open another
                // mod's panel. Other bindings are swallowed.
                if (Magelight::GetUIModeView() == hk.view) {
                    const Result r = ReleaseUIMode(OwnerOf(hk.view));
                    if (r != Result::Ok)
                        SKSE::log::warn("Magelight[v4]: hotkey close refused — {}", GetLastErrorMessage(OwnerOf(hk.view)));
                    // A toggle key's close means HIDE. ReleaseUIMode alone keeps
                    // the page rendered (the Free Look contract: leaving UI mode
                    // never hides), which is right for a mod that asked for it
                    // and wrong for a key the user pressed to make the panel go away.
                    else Magelight::ShowView(hk.view, false);
                }
                return;
            }
            if (EngineBlocksHotkey()) { SKSE::log::info("Magelight[v4]: hotkey ignored — an engine menu, the console or text entry is open"); return; }
            RequestUIMode(hk.view, hk.action == kHotkeyActionToggleUIModePaused ? kUIModeFlagPause : kUIModeFlagNone);
        });
    }

    // ── Texture images ─────────────────────────────────────────────────────

    Result RegisterTextureImageEx(ModId mod, const char* name, ID3D11ShaderResourceView* srv,
                                  std::uint32_t width, std::uint32_t height, ImageId* out)
    {
        if (out) *out = 0;
        if (!out || !name || !*name || !srv) return Result::InvalidArgument;
        std::string prefix;
        {
            std::lock_guard<std::mutex> lk(s_mutex);
            Mod* m = FindLocked(mod);
            if (!m) return Result::InvalidMod;
            prefix = m->modId;
        }
        if (!Magelight::IsGpuAccelerated()) return Fail(mod, Result::Unsupported, "RegisterTextureImageEx: CPU path — no texture images");
        const std::string full = prefix + "." + name;
        if (full.size() > kMaxImageNameLen)
            return Fail(mod, Result::InvalidArgument, "RegisterTextureImageEx: '<modId>.<name>' exceeds 64 characters");
        const ImageId id = Magelight::RegisterTextureImage(full.c_str(), srv, width, height);
        if (!id) return Fail(mod, Result::InvalidArgument, "RegisterTextureImageEx: name rejected (use [A-Za-z0-9_.-])");
        *out = id;
        return Result::Ok;
    }

    // ── Still to wire ──────────────────────────────────────────────────────
    // Nothing pending in this tier; the block is here so the next addition has
    // an obvious home and the ABI order has a visible tail.

}  // namespace Magelight::Api4