// Papyrus tier — see MagelightPapyrus.h and docs/PAPYRUS.md.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include "Magelight.h"
#include "MagelightGameTask.h"
#include "MagelightApi4.h"
#include "MagelightPapyrus.h"

#include <cctype>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace Magelight::Papyrus {

    using namespace MAGELIGHT_API;

    namespace {

        constexpr const char* kScript = "Magelight";

        std::string Lower(std::string s)
        {
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        Layer ParseLayer(const std::string& s)
        {
            const std::string l = Lower(s);
            if (l == "hud") return Layer::Hud;
            if (l == "popup") return Layer::Popup;
            if (l == "system") return Layer::System;
            return Layer::Panel;
        }

        Anchor ParseAnchor(const std::string& s)
        {
            const std::string l = Lower(s);
            if (l == "top-right" || l == "topright") return Anchor::TopRight;
            if (l == "bottom-left" || l == "bottomleft") return Anchor::BottomLeft;
            if (l == "bottom-right" || l == "bottomright") return Anchor::BottomRight;
            return Anchor::TopLeft;
        }

        void SendModEvent(const std::string& name, const std::string& str, float num)
        {
            SKSE::ModCallbackEvent e{ RE::BSFixedString(name.c_str()), RE::BSFixedString(str.c_str()), num, nullptr };
            if (auto* src = SKSE::GetModCallbackEventSource()) src->SendEvent(&e);
        }

        // Game thread (CallbackThread::GameThread). `user` is the mod's slug
        // from SlugHandle (mods do not unregister from Papyrus).
        void PapyrusEvent(const EventData* ev, void* user)
        {
            const std::string& modId = *static_cast<const std::string*>(user);
            const char* name = nullptr;
            switch (ev->type) {
            case Event::ViewDomReady:   name = "ViewDomReady"; break;
            case Event::ViewLoadFailed: name = "ViewLoadFailed"; break;
            case Event::ViewReloaded:   name = "ViewReloaded"; break;
            case Event::ViewDestroyed:  name = "ViewDestroyed"; break;
            case Event::UIModeEntered:  name = "UIModeEntered"; break;
            case Event::UIModeExited:   name = "UIModeExited"; break;
            case Event::FocusDenied:    name = "FocusDenied"; break;
            case Event::DisplayResized: name = "DisplayResized"; break;
            case Event::RenderDead:     name = "RenderDead"; break;
            case Event::UIModeRefused:  name = "UIModeRefused"; break;
            default: return;   // console spam and reserved events stay native-only
            }
            std::string str = modId;
            if (ev->detail && *ev->detail) { str += '|'; str += ev->detail; }
            SendModEvent(std::string("Magelight_") + name, str, static_cast<float>(ev->view));
        }

        // Render thread (JS context) -> game thread -> ModEvent. `user` is the
        // listener's name from NameHandle.
        void JsToPapyrus(ViewId view, const char* argument, void* user)
        {
            const std::string name = *static_cast<const std::string*>(user);
            const std::string arg = argument ? argument : "";
            Magelight::GameTask::Post([name, arg, view]() {
                SendModEvent("Magelight_JS_" + name, arg, static_cast<float>(view));
            });
        }

        std::string Str(const RE::BSFixedString& s) { return s.empty() ? std::string() : std::string(s.c_str()); }

        // Render thread -> game thread -> Magelight_JSResult / Magelight_JSError.
        void EvalToPapyrus(ViewId view, const char* result, const char* exception, void* user)
        {
            std::unique_ptr<std::string> token(static_cast<std::string*>(user));
            const std::string tok = token ? *token : "";
            const std::string r = result ? result : "";
            const std::string e = exception ? exception : "";
            Magelight::GameTask::Post([tok, r, e, view]() {
                if (e.empty()) SendModEvent("Magelight_JSResult", tok + "|" + r, static_cast<float>(view));
                else SendModEvent("Magelight_JSError", tok + "|" + e, static_cast<float>(view));
            });
        }

        // One string per slug for the session: PapyrusEvent's `user`, which a
        // mod keeps, and which every CreateView would otherwise allocate anew.
        std::string* SlugHandle(const std::string& modId)
        {
            static std::mutex s_mx;
            static std::map<std::string, std::string> s_handles;   // lowercase slug -> first spelling
            std::lock_guard<std::mutex> lk(s_mx);
            return &s_handles.try_emplace(Lower(modId), modId).first->second;
        }

        // One string per listener name for the session, never freed: a
        // listener re-registered on every load replaces the old `user`
        // without freeing it, so a fresh allocation per call would leak.
        std::string* NameHandle(const std::string& name)
        {
            static std::mutex s_mx;
            static std::map<std::string, std::string> s_handles;   // exact spelling (JS names are case-sensitive)
            std::lock_guard<std::mutex> lk(s_mx);
            return &s_handles.try_emplace(name, name).first->second;
        }

        // Each refusal is logged once per native and target (a slug, a view,
        // a policy word), not per call: a script retrying on every load must
        // not flood the log, and each distinct refusal still gets its line.
        // Capped so a script cycling through targets cannot grow the set forever.
        void RefuseOnce(const std::string& key, const std::string& msg)
        {
            static std::mutex s_mx;
            static std::set<std::string> s_logged;
            {
                std::lock_guard<std::mutex> lk(s_mx);
                if (s_logged.size() >= 256 || !s_logged.insert(Lower(key)).second) return;
            }
            SKSE::log::warn("Magelight[papyrus]: {} (logged once)", msg);
        }

        void RefusePluginMod(const char* native, const std::string& modId)
        {
            RefuseOnce(std::string(native) + "|mod|" + modId,
                       std::string(native) + "('" + modId + "') refused — that mod is registered by a plugin; "
                       "scripts may act only on mods a script registered or a manifest declares");
        }

        void RefusePluginJar(const char* native, const std::string& what, const std::string& jar)
        {
            RefuseOnce(std::string(native) + "|jar|" + what,
                       std::string(native) + "(" + what + ") refused — its storage jar ('" + jar + "') is one a plugin's "
                       "pages also use; scripts may act only on mods with a jar of their own");
        }

        // Papyrus has no caller identity, so a script may act only on views of
        // script-owned mods in a jar of their own. A view that no longer
        // exists is refused silently.
        bool ScriptMayDrive(std::int32_t view, const char* native)
        {
            if (view <= 0) return false;
            const ViewId id = static_cast<ViewId>(view);
            std::string jar;
            const Api4::ScriptAccess a = Api4::ScriptAccessOfView(id, &jar);
            if (a == Api4::ScriptAccess::Allowed) return true;
            const std::string what = "view " + std::to_string(view);
            if (a == Api4::ScriptAccess::PluginJar) {
                RefusePluginJar(native, what, jar);
            } else if (Magelight::IsViewValid(id)) {
                RefuseOnce(std::string(native) + "|view|" + std::to_string(view),
                           std::string(native) + "(" + what + ") refused — the view belongs to a plugin; "
                           "scripts may act only on views of mods a script registered or a manifest declares");
            }
            return false;
        }

        // The mod for a slug: the script's own mod, an adopted manifest mod
        // (its views become the script's) or a fresh Papyrus registration.
        // A plugin's slug, or a mod in a jar a plugin uses, is refused (0).
        ModId EnsureMod(const std::string& modId, const std::string& displayName, const char* native)
        {
            if (modId.empty()) return 0;
            std::string jar;
            // Checked before registering: adopting a manifest mod in a plugin's
            // jar would take it away from the DLL that is meant to adopt it.
            if (const ModId known = Api4::FindMod(modId.c_str());
                known && Api4::ScriptAccessOfMod(known, &jar) == Api4::ScriptAccess::PluginJar) {
                RefusePluginJar(native, "'" + modId + "'", jar);
                return 0;
            }
            ModId id = 0;
            const Result r = Api4::RegisterModEx(modId.c_str(), displayName.empty() ? modId.c_str() : displayName.c_str(),
                                                 0, CallbackThread::GameThread, &PapyrusEvent, nullptr,
                                                 SlugHandle(modId), nullptr, &id, true);
            if (r == Result::Ok) {
                if (Api4::ScriptAccessOfMod(id, &jar) == Api4::ScriptAccess::Allowed) return id;
                RefusePluginJar(native, "'" + modId + "'", jar);
                return 0;
            }
            if (r == Result::Denied) { RefusePluginMod(native, modId); return 0; }
            SKSE::log::warn("Magelight[papyrus]: {}('{}') refused ({})", native, modId, static_cast<int>(r));
            return 0;
        }

        // ── natives ─────────────────────────────────────────────────────

        std::int32_t GetVersion(RE::StaticFunctionTag*)
        {
            return static_cast<std::int32_t>(PackVersion(PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR, PLUGIN_VERSION_PATCH));
        }

        RE::BSFixedString GetVersionString(RE::StaticFunctionTag*) { return PLUGIN_VERSION; }

        bool IsReady(RE::StaticFunctionTag*) { return !Magelight::IsRenderDead(); }

        bool RegisterMod(RE::StaticFunctionTag*, RE::BSFixedString modId, RE::BSFixedString displayName)
        {
            return EnsureMod(Str(modId), Str(displayName), "RegisterMod") != 0;
        }

        std::int32_t CreateView(RE::StaticFunctionTag*, RE::BSFixedString modId, RE::BSFixedString name,
                                RE::BSFixedString htmlPath, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h,
                                RE::BSFixedString layer, RE::BSFixedString anchor, bool clickThrough, bool startVisible)
        {
            const std::string slug = Str(modId);
            const ModId mod = EnsureMod(slug, "", "CreateView");
            if (!mod) return 0;
            std::string path = Str(htmlPath);
            const bool absolute = path.size() > 1 && path[1] == ':';
            if (absolute) {
                // The script tier stays inside the game install: an absolute
                // page outside the game root would pin its whole parent
                // folder as page-readable (PageRootFor). The C++ tier keeps
                // unrestricted absolute paths — a DLL can read what it wants
                // anyway; a script may not.
                if (!Magelight::PathIsUnderGameRoot(std::filesystem::path(path))) {
                    SKSE::log::warn("Magelight[papyrus]: CreateView('{}') — absolute htmlPath outside the game root refused: {}",
                        slug, path);
                    return 0;
                }
            } else {
                // Not drive-lettered does not mean relative: a root-relative
                // path (\Windows\x.html), a UNC path or a /-rooted one has a
                // root component, and path::operator/ REPLACES the base with
                // such an rhs instead of appending — the probe below would
                // then hand back C:\Windows\x.html and pin that folder as
                // readable (review 2026-09-10). A ".." component would escape
                // Data/Magelight/<modId>/ the same way.
                const std::filesystem::path rel(path);
                if (rel.has_root_name() || rel.has_root_directory()) {
                    SKSE::log::warn("Magelight[papyrus]: CreateView('{}') — htmlPath is neither drive-lettered nor relative, refused: {}",
                        slug, path);
                    return 0;
                }
                for (const auto& comp : rel) {
                    if (comp == "..") {
                        SKSE::log::warn("Magelight[papyrus]: CreateView('{}') — relative htmlPath with '..' refused: {}",
                            slug, path);
                        return 0;
                    }
                }
                // Papyrus mods use the manifest layout: Data/Magelight/<modId>/<path>.
                // A page that is not there is refused rather than passed on:
                // the host would resolve it against its own runtime folder.
                const auto modDir = Magelight::GameRootPath() / L"Data" / L"Magelight" / slug;
                const auto inMod = modDir / rel;
                std::error_code ec;
                if (!std::filesystem::is_regular_file(inMod, ec)) {
                    SKSE::log::warn("Magelight[papyrus]: CreateView('{}') — no page at Data/Magelight/{}/{}, refused",
                        slug, slug, path);
                    return 0;
                }
                // Belt over the checks above: whatever the join produced must
                // still sit inside the mod folder before it becomes the page
                // (and, via PageRootFor, a readable root).
                if (!Magelight::PathIsUnderDir(inMod, modDir)) {
                    SKSE::log::warn("Magelight[papyrus]: CreateView('{}') — htmlPath resolved outside Data/Magelight/{}, refused: {}",
                        slug, slug, path);
                    return 0;
                }
                path = inMod.string();
            }
            const std::string vname = Str(name);
            ViewDesc vd{};
            vd.size = sizeof(ViewDesc);
            vd.name = vname.c_str();
            vd.htmlPath = path.c_str();
            vd.anchor = ParseAnchor(Str(anchor));
            vd.x = x; vd.y = y; vd.w = w; vd.h = h;
            vd.fullscreen = (w <= 0 && h <= 0);
            vd.clickThrough = clickThrough;
            vd.startVisible = startVisible;
            vd.layer = ParseLayer(Str(layer));
            ViewId id = 0;
            if (Api4::CreateViewEx(mod, &vd, &id) != Result::Ok) {
                SKSE::log::warn("Magelight[papyrus]: CreateView('{}'/'{}') — {}", slug, vname, Api4::GetLastErrorMessage(mod));
                return 0;
            }
            return static_cast<std::int32_t>(id);
        }

        std::int32_t FindView(RE::StaticFunctionTag*, RE::BSFixedString modId, RE::BSFixedString name)
        {
            const ModId mod = Api4::FindMod(Str(modId).c_str());
            return mod ? static_cast<std::int32_t>(Api4::FindViewByName(mod, Str(name).c_str())) : 0;
        }

        void DestroyView(RE::StaticFunctionTag*, std::int32_t view)
        {
            if (ScriptMayDrive(view, "DestroyView")) Api4::DestroyView(static_cast<ViewId>(view));
        }

        void ShowView(RE::StaticFunctionTag*, std::int32_t view, bool show)
        {
            if (ScriptMayDrive(view, "ShowView")) Magelight::ShowView(static_cast<ViewId>(view), show);
        }

        bool IsVisible(RE::StaticFunctionTag*, std::int32_t view)
        {
            Magelight::ViewInfo info;
            return view > 0 && Magelight::GetViewInfo(static_cast<ViewId>(view), info) && info.visible;
        }

        bool RequestUIMode(RE::StaticFunctionTag*, std::int32_t view, bool pauseGame)
        {
            GameTask::Scope pumpScope;   // a main-thread native (MagelightGameTask.h)
            if (!ScriptMayDrive(view, "RequestUIMode")) return false;
            return Api4::RequestUIMode(static_cast<ViewId>(view), pauseGame ? kUIModeFlagPause : kUIModeFlagNone) == Result::Ok;
        }

        bool ReleaseUIMode(RE::StaticFunctionTag*, RE::BSFixedString modId)
        {
            GameTask::Scope pumpScope;   // a main-thread native (MagelightGameTask.h)
            const std::string slug = Str(modId);
            const ModId mod = Api4::FindMod(slug.c_str());
            if (!mod) return false;
            std::string jar;
            switch (Api4::ScriptAccessOfMod(mod, &jar)) {
            case Api4::ScriptAccess::Allowed:   break;
            case Api4::ScriptAccess::PluginMod: RefusePluginMod("ReleaseUIMode", slug); return false;
            case Api4::ScriptAccess::PluginJar: RefusePluginJar("ReleaseUIMode", "'" + slug + "'", jar); return false;
            default:                            return false;
            }
            return Api4::ReleaseUIMode(mod) == Result::Ok;
        }

        bool IsUIModeActive(RE::StaticFunctionTag*) { return Magelight::IsUIModeActive(); }

        std::int32_t GetUIModeView(RE::StaticFunctionTag*)
        {
            return Magelight::IsUIModeActive() ? static_cast<std::int32_t>(Magelight::GetUIModeView()) : 0;
        }

        void ReloadView(RE::StaticFunctionTag*, std::int32_t view)
        {
            if (ScriptMayDrive(view, "ReloadView")) Api4::ReloadView(static_cast<ViewId>(view));
        }

        void Call(RE::StaticFunctionTag*, std::int32_t view, RE::BSFixedString function, RE::BSFixedString argument)
        {
            if (!function.empty() && ScriptMayDrive(view, "Call"))
                Magelight::InteropCall(static_cast<ViewId>(view), function.c_str(), Str(argument));
        }

        void Eval(RE::StaticFunctionTag*, std::int32_t view, RE::BSFixedString script)
        {
            if (!script.empty() && ScriptMayDrive(view, "Eval")) Magelight::InvokeJS(static_cast<ViewId>(view), Str(script));
        }

        void EvalAsync(RE::StaticFunctionTag*, std::int32_t view, RE::BSFixedString script, RE::BSFixedString token)
        {
            GameTask::Scope pumpScope;   // a main-thread native (MagelightGameTask.h)
            if (script.empty() || !ScriptMayDrive(view, "EvalAsync")) return;
            Magelight::EvalJS(static_cast<ViewId>(view), Str(script), &EvalToPapyrus, new std::string(Str(token)));
        }

        void SetCutout(RE::StaticFunctionTag*, std::int32_t view, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h)
        {
            if (ScriptMayDrive(view, "SetCutout")) Api4::SetViewCutout(static_cast<ViewId>(view), x, y, w, h);
        }

        void SetHibernate(RE::StaticFunctionTag*, std::int32_t view, std::int32_t idleMs)
        {
            if (idleMs >= 0 && ScriptMayDrive(view, "SetHibernate"))
                Api4::SetViewHibernate(static_cast<ViewId>(view), static_cast<std::uint32_t>(idleMs));
        }

        // 0.31.0: freeze the world behind the view while it holds UI mode paused (see SetViewFreezeWorld).
        bool SetFreezeWorld(RE::StaticFunctionTag*, std::int32_t view, bool freeze)
        {
            if (!ScriptMayDrive(view, "SetFreezeWorld")) return false;
            return Api4::SetViewFreezeWorld(static_cast<ViewId>(view), freeze) == Result::Ok;
        }

        // 0.29.0: a UI sound through the game's audio — see PlayUISound.
        void PlaySound(RE::StaticFunctionTag*, std::int32_t view, RE::BSFixedString name)
        {
            GameTask::Scope pumpScope;   // a main-thread native (MagelightGameTask.h)
            if (name.c_str() && *name.c_str() && ScriptMayDrive(view, "PlaySound"))
                Api4::PlayUISound(static_cast<ViewId>(view), name.c_str());
        }

        bool BindHotkey(RE::StaticFunctionTag*, std::int32_t view, std::int32_t dxScancode, std::int32_t action)
        {
            if (dxScancode < 0 || action < 0 || !ScriptMayDrive(view, "BindHotkey")) return false;
            return Api4::BindHotkey(static_cast<ViewId>(view), static_cast<std::uint32_t>(dxScancode),
                                    static_cast<std::uint32_t>(action)) == Result::Ok;
        }

        bool RegisterListener(RE::StaticFunctionTag*, std::int32_t view, RE::BSFixedString name)
        {
            if (name.empty() || !ScriptMayDrive(view, "RegisterListener")) return false;
            const ViewId id = static_cast<ViewId>(view);
            if (!Magelight::IsViewValid(id)) return false;
            return Magelight::RegisterJSListenerEx(id, name.c_str(), &JsToPapyrus, NameHandle(name.c_str()));
        }

        RE::BSFixedString GetLastError(RE::StaticFunctionTag*, RE::BSFixedString modId)
        {
            const ModId mod = Api4::FindMod(Str(modId).c_str());
            return mod ? Api4::GetLastErrorMessage(mod) : "";
        }

        // "file" or "loopback" for a script-owned mod (registered here if it
        // is not yet); internet reach ("any") is a plugin's call.
        bool SetNetworkPolicy(RE::StaticFunctionTag*, RE::BSFixedString modId, RE::BSFixedString policy)
        {
            const std::string slug = Str(modId);
            const std::string p = Lower(Str(policy));
            NetworkPolicy np = NetworkPolicy::FileOnly;
            if (p == "loopback") {
                np = NetworkPolicy::LoopbackOnly;
            } else if (p != "file") {
                RefuseOnce("SetNetworkPolicy|policy|" + slug + "|" + p, "SetNetworkPolicy('" + slug + "', '" + Str(policy) +
                           "') refused — a script may choose \"file\" or \"loopback\"; internet reach needs the mod's DLL");
                return false;
            }
            const ModId mod = EnsureMod(slug, "", "SetNetworkPolicy");
            return mod && Api4::SetNetworkPolicy(mod, np) == Result::Ok;
        }

        bool RegisterFuncs(RE::BSScript::IVirtualMachine* vm)
        {
            if (!vm) return false;
            vm->RegisterFunction("GetVersion", kScript, GetVersion);
            vm->RegisterFunction("GetVersionString", kScript, GetVersionString);
            vm->RegisterFunction("IsReady", kScript, IsReady);
            vm->RegisterFunction("RegisterMod", kScript, RegisterMod);
            vm->RegisterFunction("CreateView", kScript, CreateView);
            vm->RegisterFunction("FindView", kScript, FindView);
            vm->RegisterFunction("DestroyView", kScript, DestroyView);
            vm->RegisterFunction("ShowView", kScript, ShowView);
            vm->RegisterFunction("IsVisible", kScript, IsVisible);
            vm->RegisterFunction("RequestUIMode", kScript, RequestUIMode);
            vm->RegisterFunction("ReleaseUIMode", kScript, ReleaseUIMode);
            vm->RegisterFunction("IsUIModeActive", kScript, IsUIModeActive);
            vm->RegisterFunction("GetUIModeView", kScript, GetUIModeView);
            vm->RegisterFunction("ReloadView", kScript, ReloadView);
            vm->RegisterFunction("Call", kScript, Call);
            vm->RegisterFunction("Eval", kScript, Eval);
            vm->RegisterFunction("EvalAsync", kScript, EvalAsync);
            vm->RegisterFunction("BindHotkey", kScript, BindHotkey);
            vm->RegisterFunction("SetCutout", kScript, SetCutout);
            vm->RegisterFunction("SetHibernate", kScript, SetHibernate);
            vm->RegisterFunction("SetFreezeWorld", kScript, SetFreezeWorld);
            vm->RegisterFunction("RegisterListener", kScript, RegisterListener);
            vm->RegisterFunction("GetLastError", kScript, GetLastError);
            vm->RegisterFunction("PlaySound", kScript, PlaySound);
            vm->RegisterFunction("SetNetworkPolicy", kScript, SetNetworkPolicy);
            SKSE::log::info("Magelight[papyrus]: 25 natives registered on script '{}'", kScript);
            return true;
        }

    }  // namespace

    void Register()
    {
        if (auto* papyrus = SKSE::GetPapyrusInterface()) papyrus->Register(RegisterFuncs);
        else SKSE::log::warn("Magelight[papyrus]: no Papyrus interface — script natives unavailable");
    }

}  // namespace Magelight::Papyrus
