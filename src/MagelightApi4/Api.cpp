// v4 API — the public surface served through MagelightApiExport.cpp.
//
// This is the layer between a consumer's ModId-scoped call and the host's own
// functions. Ownership of UI mode is decided where entry is decided — on the
// game thread, by the host — and mirrored into the registry from the
// UIModeEntered/Exited/Refused events. RequestUIMode only latches a PENDING
// request so a second caller in the posted-entry window is told Busy; a
// refusal clears the latch and tells the requester. This layer therefore never
// holds an ownership claim the host does not agree with.
//
// Delivery threads: GameThread mods get events as posted SKSE tasks (never
// inside a lock). RenderThread mods are called inline on the EMITTING thread
// under a small SEH guard, so one mod's fault disables that mod's sink, not the
// overlay. Registry.cpp owns that machinery; see it for the lock discipline.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include "Magelight.h"
#include "MagelightApi4.h"
#include "MagelightDevWatch.h"
#include "MagelightGameTask.h"
#include "MagelightVR.h"
#include "MagelightApi4/Registry.h"

#include <algorithm>

namespace Magelight::Api4 {

    using namespace MAGELIGHT_API;

    Result RegisterMod(const ModDesc* desc, ModId* outMod)
    {
        if (outMod) *outMod = 0;
        if (!desc || desc->size < sizeof(ModDesc) || !outMod) return Result::InvalidArgument;
        return RegisterImpl(desc->modId, desc->displayName, desc->modVersion, desc->minHostVersion,
                            desc->callbackThread, desc->onEvent, desc->onLog, desc->user, desc->sessionName,
                            Registrar::Plugin, nullptr, outMod);
    }

    Result RegisterManifestMod(const char* modId, const char* displayName, std::uint32_t version,
                               std::uint32_t minHostVersion, const char* sessionName, const char* manifestDir,
                               ModId* outMod)
    {
        return RegisterImpl(modId, displayName, version, minHostVersion, CallbackThread::GameThread,
                            nullptr, nullptr, nullptr, sessionName, Registrar::Manifest, manifestDir, outMod);
    }

    Result RegisterModEx(const char* modId, const char* displayName, std::uint32_t version, CallbackThread thread,
                         EventFn onEvent, LogFn onLog, void* user, const char* sessionName, ModId* outMod,
                         bool script)
    {
        return RegisterImpl(modId, displayName, version, 0, thread, onEvent, onLog, user, sessionName,
                            script ? Registrar::Script : Registrar::Plugin, nullptr, outMod);
    }

    ScriptAccess ScriptAccessOfMod(ModId mod, std::string* jar)
    {
        LoadPluginJars();
        std::lock_guard<std::mutex> lk(s_mutex);
        const Mod* m = FindLocked(mod);
        if (m && jar) *jar = m->sessionName.empty() ? "default" : m->sessionName;
        return AccessLocked(m);
    }

    ScriptAccess ScriptAccessOfView(ViewId view, std::string* jar)
    {
        LoadPluginJars();
        std::lock_guard<std::mutex> lk(s_mutex);
        const Mod* m = FindLocked(OwnerLocked(view));
        if (m && jar) *jar = m->sessionName.empty() ? "default" : m->sessionName;
        return AccessLocked(m);
    }

    ModId FindMod(const char* modId)
    {
        if (!modId || !*modId) return 0;
        const std::string lower = Lower(modId);
        std::lock_guard<std::mutex> lk(s_mutex);
        for (const auto& [id, m] : s_mods) if (Lower(m.modId) == lower) return id;
        return 0;
    }

    ViewId FindViewByName(ModId mod, const char* name)
    {
        if (!name || !*name) return 0;
        std::lock_guard<std::mutex> lk(s_mutex);
        const Mod* m = FindLocked(mod);
        if (!m) return 0;
        for (const auto& [v, rec] : m->views) if (rec.name == name) return v;
        return 0;
    }

    // 0.26.8 C-ABI wrapper over FindViewByName (the Papyrus tier already had
    // this; the struct was missing it).
    Result FindView(ModId mod, const char* name, ViewId* out)
    {
        if (out) *out = 0;
        const ViewId v = FindViewByName(mod, name);
        if (!v) return Result::InvalidView;
        if (out) *out = v;
        return Result::Ok;
    }

    void UnregisterMod(ModId mod)
    {
        std::vector<ViewId> views;
        bool ownsUi = false;
        {
            std::lock_guard<std::mutex> lk(s_mutex);
            Mod* m = FindLocked(mod);
            if (!m) return;
            m->alive->store(false);   // queued deliveries drop
            for (const auto& [v, rec] : m->views) { views.push_back(v); DropQueuedLocked(v); DropHotkeysLocked(v); }
            ownsUi = (s_uiOwner == mod);
            if (ownsUi) { s_uiOwner = 0; s_uiPending = 0; }
            SKSE::log::info("Magelight[v4]: mod {} ('{}') unregistered — {} view(s) queued for destruction",
                mod, m->modId, views.size());
            s_mods.erase(mod);
            // s_viewOwner rows stay until ViewDestroyed erases them (a row
            // pointing at a gone mod routes nowhere, which is the intent).
        }
        // Exit is a game-thread task; the destroys must run AFTER it or the
        // host refuses to destroy the active UI-mode view. Tasks are FIFO.
        if (ownsUi) Magelight::ExitUIMode();
        Magelight::GameTask::Post([views]() {
            for (ViewId v : views) {
                if (!Magelight::DestroyView(v))
                    SKSE::log::warn("Magelight[v4]: UnregisterMod — view {} could not be destroyed", v);
            }
        });
    }

    Result CreateViewEx(ModId mod, const ViewDesc* desc, ViewId* outView)
    {
        if (outView) *outView = 0;
        if (!desc || desc->size < sizeof(ViewDesc) || !outView) return Result::InvalidArgument;
        std::string session;
        Magelight::NetLevel net = Magelight::NetLevel::File;
        {
            std::lock_guard<std::mutex> lk(s_mutex);
            const Mod* m = FindLocked(mod);
            if (!m) return Result::InvalidMod;
            session = m->sessionName;
            net = m->net;
        }
        if (Magelight::IsRenderDead()) return Fail(mod, Result::RenderDead, "CreateViewEx: the overlay is disabled for this session");
        if (!desc->htmlPath || !*desc->htmlPath) return Fail(mod, Result::InvalidArgument, "CreateViewEx: htmlPath is empty");
        if (!desc->fullscreen && (desc->w <= 0 || desc->h <= 0))
            return Fail(mod, Result::InvalidArgument, "CreateViewEx: w/h must be positive unless fullscreen");

        int x = desc->x, y = desc->y, w = desc->w, h = desc->h;
        if (desc->fullscreen) {
            x = y = w = h = 0;
        } else {
            // v1 geometry: a negative x/y anchors that edge from the right/bottom.
            switch (desc->anchor) {
            case Anchor::TopLeft: break;
            case Anchor::TopRight:    x = -(desc->x + desc->w); break;
            case Anchor::BottomLeft:  y = -(desc->y + desc->h); break;
            case Anchor::BottomRight: x = -(desc->x + desc->w); y = -(desc->y + desc->h); break;
            }
        }
        const ViewId id = Magelight::CreateView(desc->htmlPath, x, y, w, h, desc->onDomReady,
                                                desc->clickThrough, desc->startVisible, session.c_str(), net);
        if (!id) return Fail(mod, Result::Internal, "CreateViewEx: host refused the view");
        if (desc->uiScale > 0.0f) Magelight::SetViewScale(id, desc->uiScale);   // 0.26.9: real device scale
        Magelight::SetViewLayer(id, static_cast<int>(desc->layer));
        {
            std::lock_guard<std::mutex> lk(s_mutex);
            s_viewOwner[id] = mod;
            if (Mod* m = FindLocked(mod)) {
                net = m->net;   // a SetNetworkPolicy since the read above did not list this view
                ViewRec rec;
                rec.name = (desc->name && *desc->name) ? desc->name : std::to_string(id);
                rec.htmlPath = desc->htmlPath;
                SKSE::log::info("Magelight[v4]: view {} '{}' belongs to mod {} ('{}'), layer {}",
                    id, rec.name, mod, m->modId, static_cast<int>(desc->layer));
                m->views[id] = std::move(rec);
            }
        }
        PushNetLevel(mod, { id }, net);
        if (Magelight::DevModeEnabled()) {
            const std::filesystem::path page = desc->htmlPath;
            Dev::WatchView(id, (page.is_absolute() ? page : Magelight::RuntimeDirPath() / page).parent_path());
        }
        *outView = id;
        return Result::Ok;
    }

    Result DestroyView(ViewId view)
    {
        const ModId owner = OwnerOf(view);
        if (!owner) return Result::InvalidView;
        if (!Magelight::DestroyView(view))
            return Fail(owner, Result::Busy, "DestroyView: the view is the active UI-mode view — release UI mode first");
        return Result::Ok;   // ViewDestroyed follows from the render thread
    }

    Result ReloadView(ViewId view)
    {
        if (!Magelight::ReloadView(view)) return Result::InvalidView;
        return Result::Ok;
    }

    Result Navigate(ViewId view, const char* url)
    {
        if (!url || !*url) return Result::InvalidArgument;
        const std::string u = url;
        if (u.rfind("http://", 0) == 0 || u.rfind("https://", 0) == 0)
            return Fail(OwnerOf(view), Result::Denied, "Navigate: remote URLs are not allowed (file:/// only)");
        if (!Magelight::NavigateView(view, url)) return Result::InvalidView;
        return Result::Ok;
    }

    Result RegisterJSListenerEx(ViewId view, const char* name, JsListenerFn4 cb, void* user)
    {
        if (!name || !*name || !cb) return Result::InvalidArgument;
        if (!Magelight::IsViewValid(view)) return Result::InvalidView;
        if (!Magelight::RegisterJSListenerEx(view, name, cb, user)) {
            if (!Magelight::IsViewValid(view)) return Result::InvalidView;
            return Fail(OwnerOf(view), Result::InvalidArgument, std::string("RegisterJSListenerEx: '") + name +
                        "' is the host's ('magelight' and names starting with '__' are reserved)");
        }
        return Result::Ok;
    }

    Result RequestUIMode(ViewId view, std::uint32_t flags)
    {
        const ModId owner = OwnerOf(view);
        if (!owner) return Result::InvalidView;
        if (Magelight::IsRenderDead()) return Fail(owner, Result::RenderDead, "RequestUIMode: the overlay is disabled for this session");
        Magelight::ViewInfo info;
        if (!Magelight::GetViewInfo(view, info)) return Fail(owner, Result::InvalidView, "RequestUIMode: the view is gone");
        if (info.clickThrough || info.layer == static_cast<int>(Layer::Hud))
            return Fail(owner, Result::Denied, "RequestUIMode: a Hud / click-through view cannot take UI mode");

        Sink holder;
        bool busy = false, haveHolder = false, switching = false, queued = false;
        {
            std::lock_guard<std::mutex> lk(s_mutex);
            const bool active = Magelight::IsUIModeActive();
            if (active && s_uiOwner == owner && s_uiPending == 0) {
                switching = true;   // intra-mod: retarget the mode you already hold
            } else if (active || s_uiOwner != 0 || s_uiPending != 0) {
                busy = true;
                if (Mod* h = FindLocked(s_uiOwner)) { holder = SinkOf(*h); haveHolder = true; }
                if (flags & kUIModeFlagQueue) {
                    queued = true;
                    bool dup = false;
                    for (const auto& q : s_uiQueue) dup = dup || (q.view == view);
                    if (!dup) s_uiQueue.push_back(Queued{ view, flags });
                }
            } else {
                s_uiPending = view;   // decided by the host on the game thread; Entered/Refused clears it
            }
        }
        if (switching) {
            // 0.26.10: a re-request carries the flags the holder wants NOW —
            // the pause flag is retargeted even when the view is unchanged
            // (before, a same-view re-request returned Ok and silently kept
            // the old pause: SA's Live Stage asked to unpause and ran frozen).
            const bool wantPause = (flags & kUIModeFlagPause) != 0;
            if (Magelight::GetUIModeView() != view) Magelight::SwitchUIModeView(view);
            Magelight::SetUIModePause(wantPause);
            return Result::Ok;   // UIModeEntered (detail "switched") confirms a view change
        }
        if (busy) {
            std::string requester;
            { std::lock_guard<std::mutex> lk(s_mutex); if (Mod* r = FindLocked(owner)) requester = r->modId; }
            if (haveHolder && holder.id != owner) Deliver(holder, Event::FocusDenied, view, 0, 0, requester);
            if (queued) {
                SKSE::log::info("Magelight[v4]: RequestUIMode({}) queued behind {}", view,
                    haveHolder ? holder.modId : std::string("the host or a v3 consumer"));
                return Result::Ok;   // UIModeEntered arrives when the holder releases
            }
            return Fail(owner, Result::Busy, "RequestUIMode: UI mode is held by " +
                (haveHolder ? holder.modId : std::string("the host or a v3 consumer")));
        }
        Magelight::EnterUIModeEx(view, (flags & kUIModeFlagPause) != 0, (flags & kUIModeFlagNoTextEntry) != 0);
        return Result::Ok;   // UIModeEntered confirms; UIModeRefused reports a lost race
    }

    Result ReleaseUIMode(ModId mod)
    {
        bool active = Magelight::IsUIModeActive();
        {
            std::lock_guard<std::mutex> lk(s_mutex);
            Mod* m = FindLocked(mod);
            if (!m) return Result::InvalidMod;
            // A release also withdraws every queued request of this mod.
            for (const auto& [v, rec] : m->views) DropQueuedLocked(v);
            if (!active) {
                // Nothing to exit; make sure no stale claim survives.
                if (s_uiOwner == mod) s_uiOwner = 0;
                if (s_uiPending && OwnerLocked(s_uiPending) == mod) s_uiPending = 0;
                return Result::Ok;
            }
            if (s_uiOwner != mod) {
                m->lastError = "ReleaseUIMode: this mod does not own UI mode";
                return Result::Denied;
            }
        }
        Magelight::ExitUIMode();   // UIModeExited clears the owner
        return Result::Ok;
    }

    ModId GetUIModeOwner()
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        return s_uiOwner;
    }

    Result RaiseView(ViewId view)
    {
        if (!Magelight::IsViewValid(view)) return Result::InvalidView;
        Magelight::RaiseView(view);
        return Result::Ok;
    }

    Result GetViewInfo(ViewId view, ViewDesc* out)
    {
        if (!out || out->size < sizeof(ViewDesc)) return Result::InvalidArgument;
        Magelight::ViewInfo info;
        if (!Magelight::GetViewInfo(view, info)) return Result::InvalidView;
        std::lock_guard<std::mutex> lk(s_mutex);
        Mod* m = FindLocked(OwnerLocked(view));
        if (!m) return Result::InvalidView;   // v4-owned views only (the strings live in the registry)
        auto it = m->views.find(view);
        if (it == m->views.end()) return Result::InvalidView;
        ViewRec& rec = it->second;
        rec.htmlPath = info.htmlPath;   // follows Navigate
        out->name = rec.name.c_str();
        out->htmlPath = rec.htmlPath.c_str();
        // Reconstruct the anchor from the host's signed geometry so the
        // ViewDesc round-trips into CreateViewEx.
        const bool right = info.x < 0, bottom = info.y < 0;
        out->anchor = right ? (bottom ? Anchor::BottomRight : Anchor::TopRight)
                            : (bottom ? Anchor::BottomLeft : Anchor::TopLeft);
        out->x = right ? (-info.x - info.w) : info.x;
        out->y = bottom ? (-info.y - info.h) : info.y;
        out->w = info.w; out->h = info.h;
        out->fullscreen = info.fullscreen;
        out->clickThrough = info.clickThrough;
        out->startVisible = info.visible;   // current visibility on read-back
        out->layer = static_cast<Layer>(std::clamp(info.layer, static_cast<int>(Layer::Hud),
                                                   static_cast<int>(Layer::System)));
        out->uiScale = info.deviceScale;   // 0.26.9: round-trips
        out->onDomReady = nullptr;
        return Result::Ok;
    }

    void GetDisplaySize(std::int32_t* w, std::int32_t* h)
    {
        int dw = 0, dh = 0;
        Magelight::GetDisplaySize(dw, dh);
        if (w) *w = dw;
        if (h) *h = dh;
    }

    std::int32_t QueryCapability(const char* name)
    {
        if (!name) return 0;
        const std::string n = Lower(name);
        if (n == "gpu") return Magelight::IsGpuAccelerated() ? 1 : 0;
        if (n == "textureimage") return Magelight::IsGpuAccelerated() ? 1 : 0;
        if (n == "clippathhole") return 0;   // even-odd polygon clips: CPU path only, never on the GPU host
        if (n == "pause") return 1;
        if (n == "events") return 1;
        if (n == "clipboard") return 1;
        if (n == "networkdeny") return 1;   // network requests are refused unless the mod opted in
                                            // (SetNetworkPolicy; file-only by default since 0.30.0)
        if (n == "sessions") return 1;      // per-mod persistent Sessions (isolated storage)
        if (n == "manifest") return 1;      // Data/Magelight/<ModId>/manifest.json mods
        if (n == "http") return 0;
        if (n == "vr") return Magelight::VR::IsLive() ? 1 : 0;   // the VR presenter is live (0.17.0)
        if (n == "hotkeys") return 1;       // BindHotkey registry (0.12.0)
        if (n == "ime") return 1;           // native IME composition for CJK input (0.27.0)
        if (n == "loopback") return 1;      // http(s) to 127.x|localhost|[::1] at LoopbackOnly (0.28.0; opt-in since 0.30.0)
        if (n == "escapecapture") return 1; // SetEscapeCapture / __escapecapture (0.28.0)
        if (n == "vieworder") return 1;     // SetViewOrder / GetViewOrder (0.28.0)
        if (n == "scrollstep") return 1;    // SetScrollStep (0.28.0)
        if (n == "networkpolicy") return 1; // SetNetworkPolicy (0.28.2)
        if (n == "sound") return 1;         // host-played UI sounds: PlayUISound / __sound / data-ml-sound (0.29.0)
        if (n == "evaljs") return 1;        // EvalJS with result/exception (0.14.0)
        if (n == "pagebridge") return 1;    // window.magelight core installed on every page (0.15.0)
        if (n == "cutout") return 1;        // compositor cutout rect (0.16.0)
        if (n == "hibernate") return 1;     // hidden views release their texture (0.16.0)
        if (n == "inspector") return (Magelight::DevModeEnabled() && Magelight::InspectorAvailable()) ? 1 : 0;
        return 0;
    }

    const char* GetLastErrorMessage(ModId mod)
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        Mod* m = FindLocked(mod);
        return m ? m->lastError.c_str() : "";
    }

    Result BindHotkey(ViewId view, std::uint32_t dxScancode, std::uint32_t action)
    {
        const ModId owner = OwnerOf(view);
        if (!owner) return Result::InvalidView;
        if (action != kHotkeyActionUnbind) {
            // The user's Magelight.json wins over what the mod asked for.
            std::string key;
            std::uint32_t overrideCode = 0;
            bool overridden = false;
            {
                std::lock_guard<std::mutex> lk(s_mutex);
                if (const Mod* m = FindLocked(owner)) {
                    if (auto v = m->views.find(view); v != m->views.end()) key = Lower(m->modId + "/" + v->second.name);
                }
                if (auto o = s_hotkeyOverrides.find(key); !key.empty() && o != s_hotkeyOverrides.end()) {
                    overridden = true;
                    overrideCode = o->second;
                }
            }
            if (overridden) {
                if (overrideCode == 0) {
                    std::lock_guard<std::mutex> lk(s_mutex);
                    DropHotkeysLocked(view);
                    SKSE::log::info("Magelight[v4]: hotkey for '{}' disabled by Magelight.json", key);
                    return Result::Ok;
                }
                SKSE::log::info("Magelight[v4]: hotkey for '{}' rebound {} -> {} by Magelight.json", key, dxScancode, overrideCode);
                dxScancode = overrideCode;
            }
        }
        if (action == kHotkeyActionUnbind) {
            std::lock_guard<std::mutex> lk(s_mutex);
            if (dxScancode == 0) DropHotkeysLocked(view);
            else if (auto it = s_hotkeys.find(dxScancode); it != s_hotkeys.end() && it->second.view == view)
                s_hotkeys.erase(it);
            return Result::Ok;
        }
        if (action > kHotkeyActionToggleVisible) return Fail(owner, Result::InvalidArgument, "BindHotkey: unknown action");
        if (dxScancode == 0 || dxScancode > 0xFF) return Fail(owner, Result::InvalidArgument, "BindHotkey: scancode must be a DirectInput code 1..255");
        if (dxScancode == Magelight::GetToggleKey())
            return Fail(owner, Result::Denied, "BindHotkey: that scancode is the host's own toggle key");
        Magelight::ViewInfo info;
        if (!Magelight::GetViewInfo(view, info)) return Fail(owner, Result::InvalidView, "BindHotkey: the view is gone");
        if (action != kHotkeyActionToggleVisible && (info.clickThrough || info.layer == static_cast<int>(Layer::Hud)))
            return Fail(owner, Result::Denied, "BindHotkey: a Hud / click-through view can only bind ToggleVisible");
        std::string holder;
        {
            std::lock_guard<std::mutex> lk(s_mutex);
            auto it = s_hotkeys.find(dxScancode);
            if (it != s_hotkeys.end() && it->second.view != view) {
                if (Mod* h = FindLocked(OwnerLocked(it->second.view))) holder = h->modId;
                else holder = "another mod";
            } else {
                s_hotkeys[dxScancode] = Hotkey{ view, action };
            }
        }
        if (!holder.empty()) return Fail(owner, Result::Busy, "BindHotkey: scancode " + std::to_string(dxScancode) + " is bound by " + holder);
        SKSE::log::info("Magelight[v4]: hotkey {} -> view {} ({})", dxScancode, view,
            action == kHotkeyActionToggleVisible ? "toggle visible" : action == kHotkeyActionToggleUIModePaused ? "toggle UI mode, paused" : "toggle UI mode");
        return Result::Ok;
    }

    std::string ViewIdentityJson(ViewId view)
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        if (const Mod* m = FindLocked(OwnerLocked(view))) {
            if (auto v = m->views.find(view); v != m->views.end())
                return "\"modId\":" + Magelight::JsonQuote(m->modId) +
                       ",\"viewName\":" + Magelight::JsonQuote(v->second.name);
        }
        return "\"modId\":\"\",\"viewName\":\"\"";
    }

    std::string CapabilitiesJson()
    {
        // The canonical capability list (QueryCapability is the value
        // authority; it lowercases, so the camelCase spellings here are the
        // PAGE-side keys). MUST match the header doc and the SDK mock — add to
        // all three or to none.
        static const char* kNames[] = { "gpu", "textureImage", "clipPathHole", "pause", "events", "clipboard",
                                        "networkDeny", "sessions", "manifest", "http", "vr", "hotkeys", "evaljs",
                                        "pagebridge", "cutout", "hibernate", "inspector", "ime",
                                        "loopback", "escapeCapture", "viewOrder", "scrollStep", "networkPolicy",
                                        "sound" };
        std::string out = "{";
        bool first = true;
        for (const char* n : kNames) {
            if (!first) out += ',';
            first = false;
            out += "\"" + std::string(n) + "\":" + std::to_string(QueryCapability(n));
        }
        return out + "}";
    }

    // Per-view setters share one gate (0.26.9 review): the view must be a v4
    // view (OwnerOf — the same gate SetViewCutout/SetViewHibernate use; the
    // C ABI carries no caller id, so this is API-created, not caller-owned) and
    // alive, and every refusal records a message for GetLastErrorMessage and
    // the mod's onLog instead of a bare Result.
    Result GateView(ViewId view, const char* fn)
    {
        const ModId owner = OwnerOf(view);
        if (!owner) return Fail(0, Result::InvalidView, std::string(fn) + ": not a v4 view");
        if (!Magelight::IsViewValid(view)) return Fail(owner, Result::InvalidView, std::string(fn) + ": view is gone");
        return Result::Ok;
    }

    Result SetViewScale(ViewId view, float scale)
    {
        if (const Result g = GateView(view, "SetViewScale"); g != Result::Ok) return g;
        if (!(scale > 0.0f) || scale != scale)
            return Fail(OwnerOf(view), Result::InvalidArgument, "SetViewScale: scale must be a positive number");
        Magelight::SetViewScale(view, scale);
        return Result::Ok;
    }

    Result SetViewCutout(ViewId view, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h)
    {
        if (!OwnerOf(view)) return Result::InvalidView;
        if (!Magelight::IsViewValid(view)) return Result::InvalidView;
        Magelight::SetViewCutout(view, x, y, w, h);
        return Result::Ok;
    }

    Result SetViewHibernate(ViewId view, std::uint32_t idleMs)
    {
        if (!OwnerOf(view)) return Result::InvalidView;
        if (!Magelight::IsViewValid(view)) return Result::InvalidView;
        Magelight::SetViewHibernate(view, idleMs);
        return Result::Ok;
    }

    // ── Dogfood: the host's own probe view rides the v4 path ─────────────────
    // This is a HOST feature (devMode's probe page) that deliberately consumes
    // the consumer API end to end — the same code path a third-party mod uses,
    // which is the point of it. It lives with the API surface rather than in
    // the host because it needs the registry to hold a ModId for the host.

    static void HostProbeEvent(const EventData* ev, void*)
    {
        const char* name = "?";
        switch (ev->type) {
        case Event::ViewDomReady: name = "ViewDomReady"; break;
        case Event::ViewLoadFailed: name = "ViewLoadFailed"; break;
        case Event::ViewReloaded: name = "ViewReloaded"; break;
        case Event::ViewDestroyed: name = "ViewDestroyed"; break;
        case Event::UIModeEntered: name = "UIModeEntered"; break;
        case Event::UIModeExited: name = "UIModeExited"; break;
        case Event::FocusDenied: name = "FocusDenied"; break;
        case Event::DisplayResized: name = "DisplayResized"; break;
        case Event::ConsoleMessage: return;   // already logged by the host
        case Event::RenderDead: name = "RenderDead"; break;
        case Event::HostShutdown: name = "HostShutdown"; break;
        case Event::UIModeRefused: name = "UIModeRefused"; break;
        }
        SKSE::log::info("Magelight[v4]: host probe event {} (view {}, {}x{}{}{})",
            name, ev->view, ev->x, ev->y, ev->detail && *ev->detail ? " — " : "",
            ev->detail ? ev->detail : "");
    }

    ViewId CreateHostProbeView(const char* htmlPath, int x, int y, int w, int h)
    {
        static ModId s_hostMod = 0;
        if (!s_hostMod) {
            ModDesc md{};
            md.size = sizeof(ModDesc);
            md.modId = "Magelight";
            md.displayName = "Magelight UI (host)";
            md.modVersion = kHostVersionNumber;
            md.minHostVersion = kHostVersionNumber;
            md.callbackThread = CallbackThread::GameThread;
            md.onEvent = &HostProbeEvent;
            if (RegisterMod(&md, &s_hostMod) != Result::Ok) return 0;
        }
        ViewDesc vd{};
        vd.size = sizeof(ViewDesc);
        vd.name = "probe";
        vd.htmlPath = htmlPath;
        vd.anchor = Anchor::TopLeft;
        vd.x = x; vd.y = y; vd.w = w; vd.h = h;
        vd.layer = Layer::Panel;
        ViewId id = 0;
        if (CreateViewEx(s_hostMod, &vd, &id) != Result::Ok) {
            SKSE::log::warn("Magelight[v4]: host probe view failed — {}", GetLastErrorMessage(s_hostMod));
            return 0;
        }
        return id;
    }

}  // namespace Magelight::Api4