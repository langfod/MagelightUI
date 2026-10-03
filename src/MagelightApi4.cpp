// API v4 — the mod registry behind api/MagelightUI_API.h's MagelightApi4.
//
// Everything a consumer creates is scoped to a ModId: views (owner map),
// listeners (user pointer), texture images (namespaced name), UI-mode
// ownership, the per-mod error string and the per-mod event sink.
//
// Ownership of UI mode is decided where entry is decided — on the game
// thread, by the host — and mirrored here from the UIModeEntered/Exited/
// Refused events. RequestUIMode only latches a PENDING request so a second
// caller in the posted-entry window is told Busy; a refusal clears the latch and
// tells the requester (Event::UIModeRefused). This layer therefore never
// holds an ownership claim the host does not agree with (0.10.1 review fix:
// the 0.10.0 skeleton claimed before entry and could wedge every mod Busy).
//
// Delivery threads: GameThread mods get events as posted SKSE tasks (never
// inside a lock). RenderThread mods are called inline on the EMITTING
// thread (render thread for page/console/resize events; game thread for
// UI-mode events; the requester's thread for FocusDenied) under a small SEH
// guard, so one mod's fault disables that mod's sink, not the overlay.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include "Magelight.h"
#include "MagelightApi4.h"
#include "MagelightSound.h"
#include "MagelightVR.h"
#include "MagelightDevWatch.h"
#include "MagelightGameTask.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <deque>
#include <filesystem>
#include <fstream>
#include <set>

namespace Magelight::Api4 {

    using namespace MAGELIGHT_API;

    namespace {

        constexpr std::size_t kMaxSlugLen = 32;   // leaves room for "<slug>.<image>" under the 64-char image-name cap
        constexpr std::size_t kMaxImageNameLen = 64;

        struct ViewRec {
            std::string name;
            std::string htmlPath;   // registry-owned storage behind GetViewInfo's pointers
        };

        struct Mod {
            ModId id = 0;
            std::string modId;
            std::string displayName;
            std::uint32_t version = 0;
            CallbackThread thread = CallbackThread::GameThread;
            EventFn onEvent = nullptr;
            LogFn onLog = nullptr;
            void* user = nullptr;
            std::string lastError;
            std::string sessionName;   // "" = shared default session; else per-mod persistent
            bool manifestOnly = false; // registered from a manifest, no DLL attached yet
            std::string manifestDir;   // the folder a manifest mod came from ("" otherwise)
            // Papyrus has no caller identity, so the script tier may act only
            // on mods no plugin registered: set for a Papyrus registration and
            // a manifest mod, cleared when a DLL adopts the manifest mod.
            bool scriptOwned = false;
            Magelight::NetLevel net = Magelight::NetLevel::File;   // NetworkPolicy, pushed to every view of the mod
            std::map<ViewId, ViewRec> views;
            // Generation token: queued game-thread deliveries check it, so an
            // event queued just before UnregisterMod never reaches a freed user.
            std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
        };

        // What Deliver needs — snapshotted under the lock, used outside it.
        struct Sink {
            ModId id = 0;
            std::string modId;
            CallbackThread thread = CallbackThread::GameThread;
            EventFn fn = nullptr;
            void* user = nullptr;
            std::shared_ptr<std::atomic<bool>> alive;
        };

        std::mutex s_mutex;
        std::map<ModId, Mod> s_mods;
        std::map<ViewId, ModId> s_viewOwner;
        ModId s_nextMod = 1;
        ModId s_uiOwner = 0;      // mirrored from UIModeEntered/Exited (0 = none / a v1-v3 or host view)
        ViewId s_uiPending = 0;   // a RequestUIMode whose game-thread entry has not been decided yet
        bool s_sinkInstalled = false;

        // kUIModeFlagQueue: requests waiting for the holder to release, FIFO.
        struct Queued { ViewId view; std::uint32_t flags; };
        std::deque<Queued> s_uiQueue;
        void DropQueuedLocked(ViewId view)
        {
            for (auto it = s_uiQueue.begin(); it != s_uiQueue.end();)
                it = (it->view == view) ? s_uiQueue.erase(it) : it + 1;
        }

        void DrainUIQueue();
        std::vector<std::pair<std::string, std::uint32_t>> s_tooOld;   // HostTooOld refusals (version gate)

        std::map<std::string, std::uint32_t> s_hotkeyOverrides;   // "modid/view" (lower) -> code, 0 = disabled

        // BindHotkey registry: one binding per DX scancode, process-wide.
        struct Hotkey { ViewId view; std::uint32_t action; };
        std::map<std::uint32_t, Hotkey> s_hotkeys;
        void DropHotkeysLocked(ViewId view)
        {
            for (auto it = s_hotkeys.begin(); it != s_hotkeys.end();)
                it = (it->second.view == view) ? s_hotkeys.erase(it) : std::next(it);
        }

        constexpr std::uint32_t kHostVersionNumber =
            PackVersion(PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR, PLUGIN_VERSION_PATCH);

        bool ValidSlug(const char* s)
        {
            if (!s || !*s) return false;
            std::size_t n = 0;
            bool anyAlnum = false;
            for (const char* p = s; *p; ++p) {
                const unsigned char c = static_cast<unsigned char>(*p);
                if (!(std::isalnum(c) || c == '_' || c == '.' || c == '-')) return false;
                if (std::isalnum(c)) anyAlnum = true;
                if (++n > kMaxSlugLen) return false;
            }
            // A slug becomes a directory name (persistent session storage
            // under cache_path/<slug>): refuse the spellings that are not
            // names on Windows — dot-only ("." / ".." is traversal, reachable
            // from a manifest modId), a trailing dot (the OS strips it, so
            // "mod" and "mod." would collide), and the device basenames
            // (matched on the part before the first dot, case-insensitively).
            if (!anyAlnum || s[n - 1] == '.') return false;
            static const char* const kReserved[] = {
                "con", "prn", "aux", "nul",
                "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
                "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"
            };
            std::size_t baseLen = 0;
            while (baseLen < n && s[baseLen] != '.') ++baseLen;
            for (const char* r : kReserved) {
                std::size_t i = 0;
                for (; i < baseLen && r[i]; ++i)
                    if (std::tolower(static_cast<unsigned char>(s[i])) != r[i]) break;
                if (i == baseLen && !r[i]) return false;
            }
            return true;
        }

        std::string Lower(std::string s)
        {
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        // s_mutex held.
        Mod* FindLocked(ModId id)
        {
            auto it = s_mods.find(id);
            return it == s_mods.end() ? nullptr : &it->second;
        }
        // s_mutex held.
        ModId OwnerLocked(ViewId view)
        {
            auto it = s_viewOwner.find(view);
            return it == s_viewOwner.end() ? 0 : it->second;
        }
        ModId OwnerOf(ViewId view)
        {
            std::lock_guard<std::mutex> lk(s_mutex);
            return OwnerLocked(view);
        }
        Sink SinkOf(const Mod& m)
        {
            return Sink{ m.id, m.modId, m.thread, m.onEvent, m.user, m.alive };
        }

        // Pushes a mod's network level to views outside s_mutex (the host's
        // view lock must never nest in it), then repeats with the stored
        // level while a concurrent SetNetworkPolicy changed it meanwhile, so
        // the last level each view receives is the one the mod holds.
        void PushNetLevel(ModId mod, std::vector<ViewId> views, Magelight::NetLevel level)
        {
            for (;;) {
                for (const ViewId v : views) Magelight::SetViewNetworkLevel(v, level);
                std::lock_guard<std::mutex> lk(s_mutex);
                const Mod* m = FindLocked(mod);
                if (!m || m->net == level) return;
                level = m->net;
                views.clear();
                for (const auto& [id, rec] : m->views) views.push_back(id);
            }
        }

        // Every storage jar a plugin has used, kept across sessions: the jar's
        // data stays on disk, so it stays off limits to scripts in a session
        // without that plugin. Lowercase names. s_jarMutex may be taken while
        // s_mutex is held, never the reverse; the file is read and appended
        // with neither held.
        std::mutex s_jarMutex;
        std::set<std::string> s_pluginJars;
        std::once_flag s_pluginJarsLoaded;
        std::mutex s_jarFileMutex;

        // The space keeps the name out of the slug alphabet, so no jar
        // directory under the cache dir can collide with it.
        std::filesystem::path PluginJarsFile()
        {
            const std::filesystem::path dir = Magelight::CacheDirPath();
            return dir.empty() ? dir : dir / L"plugin jars.txt";
        }

        // Never with s_mutex held.
        void LoadPluginJars()
        {
            std::call_once(s_pluginJarsLoaded, [] {
                std::set<std::string> jars;
                if (const auto file = PluginJarsFile(); !file.empty()) {
                    std::ifstream f(file);
                    std::string line;
                    while (std::getline(f, line)) {
                        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
                        if (ValidSlug(line.c_str())) jars.insert(Lower(line));
                    }
                }
                if (!jars.empty())
                    SKSE::log::info("Magelight[v4]: {} plugin storage jar(s) on record; scripts stay out of them", jars.size());
                std::lock_guard<std::mutex> lk(s_jarMutex);
                s_pluginJars.insert(jars.begin(), jars.end());
            });
        }

        bool IsRecordedPluginJar(const std::string& lowerJar)
        {
            std::lock_guard<std::mutex> lk(s_jarMutex);
            return s_pluginJars.count(lowerJar) != 0;
        }

        // Never with s_mutex held (file IO). "" (the default jar) needs no record.
        void RecordPluginJar(const std::string& jar)
        {
            if (jar.empty()) return;
            LoadPluginJars();
            const std::string lower = Lower(jar);
            {
                std::lock_guard<std::mutex> lk(s_jarMutex);
                if (!s_pluginJars.insert(lower).second) return;
            }
            const auto file = PluginJarsFile();
            if (file.empty()) return;
            std::lock_guard<std::mutex> lk(s_jarFileMutex);
            std::error_code ec;
            std::filesystem::create_directories(file.parent_path(), ec);
            std::ofstream f(file, std::ios::app);
            if (f << lower << '\n')
                SKSE::log::info("Magelight[v4]: storage jar '{}' recorded as a plugin's", lower);
            else
                SKSE::log::warn("Magelight[v4]: could not record storage jar '{}' in {}", lower, file.string());
        }

        // s_mutex held. A script-owned mod whose storage jar a plugin's pages
        // also use (now or, per the record, in an earlier session) is off
        // limits to scripts: through its views a script could read that
        // storage. The default jar is always such a jar (v1-v3 views and
        // plugins choosing "default" keep theirs there).
        ScriptAccess AccessLocked(const Mod* m)
        {
            if (!m) return ScriptAccess::Unknown;
            if (!m->scriptOwned) return ScriptAccess::PluginMod;
            if (m->sessionName.empty()) return ScriptAccess::PluginJar;
            const std::string jar = Lower(m->sessionName);
            for (const auto& [id, o] : s_mods)
                if (!o.scriptOwned && Lower(o.sessionName) == jar) return ScriptAccess::PluginJar;
            if (IsRecordedPluginJar(jar)) return ScriptAccess::PluginJar;
            return ScriptAccess::Allowed;
        }

        // Record + log a failure. The consumer's onLog runs OUTSIDE the lock
        // (it may call back into this API).
        void SetError(ModId id, const std::string& msg)
        {
            LogFn fn = nullptr;
            void* user = nullptr;
            {
                std::lock_guard<std::mutex> lk(s_mutex);
                if (Mod* m = FindLocked(id)) {
                    m->lastError = msg;
                    fn = m->onLog;
                    user = m->user;
                }
            }
            if (id) SKSE::log::warn("Magelight[v4]: mod {} — {}", id, msg);
            if (fn) fn(2, msg.c_str(), user);
        }

        // "<call>: the overlay is disabled for this session (<why>)", the reason when one is known.
        std::string DeadMessage(const char* call)
        {
            const char* why = Magelight::RenderDeadReason();
            std::string msg = std::string(call) + ": the overlay is disabled for this session";
            if (why && *why) msg += std::string(" (") + why + ")";
            return msg;
        }

        Result Fail(ModId id, Result r, const std::string& msg)
        {
            SetError(id, msg);
            return r;
        }

        // SEH boundary for inline (RenderThread) delivery: POD-only frame.
        LONG FilterLog(unsigned long code, ModId id)
        {
            SKSE::log::error("Magelight[v4]: mod {} event handler faulted (0x{:08X}) — its sink is disabled",
                id, code);
            return EXCEPTION_EXECUTE_HANDLER;
        }
        bool CallGuardedResult(MAGELIGHT_API::JsResultFn fn, ViewId view, const char* result, const char* exception,
                               void* user, ModId id)
        {
            __try {
                fn(view, result, exception, user);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                SKSE::log::error("Magelight[v4]: mod {} EvalJS callback raised an SEH exception — its sink is disabled", id);
                return false;
            }
        }

        bool CallGuarded(EventFn fn, const EventData* ev, void* user, ModId id)
        {
            __try {
                fn(ev, user);
                return true;
            } __except (FilterLog(GetExceptionCode(), id)) {
                return false;
            }
        }
        void DisableSink(ModId id)
        {
            std::lock_guard<std::mutex> lk(s_mutex);
            if (Mod* m = FindLocked(id)) m->onEvent = nullptr;
        }

        // Deliver one event to one mod on its chosen thread. Never called
        // with s_mutex held. Strings are copied so a queued task owns them.
        void Deliver(const Sink& s, Event type, ViewId view, std::int32_t x, std::int32_t y,
                     std::string detail)
        {
            if (!s.fn || !s.alive || !s.alive->load()) return;
            if (s.thread == CallbackThread::RenderThread) {
                EventData ev{ type, view, x, y, detail.c_str() };
                if (!CallGuarded(s.fn, &ev, s.user, s.id)) DisableSink(s.id);
                return;
            }
            Magelight::GameTask::Post(
                [fn = s.fn, user = s.user, alive = s.alive, id = s.id, type, view, x, y, d = std::move(detail)]() {
                    if (!alive->load()) return;
                    EventData ev{ type, view, x, y, d.c_str() };
                    if (!CallGuarded(fn, &ev, user, id)) DisableSink(id);
                });
        }

        // The host's event sink (render thread for page/console/resize/render
        // death, game thread for UI mode). Routes to the owning mod, the
        // UI-mode owner, or everyone.
        void HostSink(HostEvent type, ViewId view, int x, int y, const char* detail)
        {
            const std::string d = detail ? detail : "";
            std::vector<std::pair<Sink, Event>> targets;
            bool drainQueue = false;
            {
                std::lock_guard<std::mutex> lk(s_mutex);
                auto ownerSink = [&](ViewId v, Event ev) {
                    if (Mod* m = FindLocked(OwnerLocked(v))) targets.emplace_back(SinkOf(*m), ev);
                };
                switch (type) {
                case HostEvent::ViewDomReady:   ownerSink(view, Event::ViewDomReady); break;
                case HostEvent::ViewLoadFailed: ownerSink(view, Event::ViewLoadFailed); break;
                case HostEvent::ViewReloaded:   ownerSink(view, Event::ViewReloaded); break;
                case HostEvent::ConsoleMessage: ownerSink(view, Event::ConsoleMessage); break;
                case HostEvent::ViewDestroyed:
                    if (Mod* m = FindLocked(OwnerLocked(view))) {
                        targets.emplace_back(SinkOf(*m), Event::ViewDestroyed);
                        m->views.erase(view);
                    }
                    s_viewOwner.erase(view);
                    if (s_uiPending == view) s_uiPending = 0;
                    DropQueuedLocked(view);
                    DropHotkeysLocked(view);
                    break;
                case HostEvent::UIModeSwitched:
                    // Same owner by construction (RequestUIMode only switches within a mod).
                    s_uiOwner = OwnerLocked(view);
                    ownerSink(view, Event::UIModeEntered);
                    break;
                case HostEvent::UIModeEntered:
                    // The host decided: the entered view's owner (0 for a host/v3 view) holds UI mode.
                    s_uiOwner = OwnerLocked(view);
                    if (s_uiPending == view || s_uiPending != 0) s_uiPending = 0;
                    if (Mod* m = FindLocked(s_uiOwner)) targets.emplace_back(SinkOf(*m), Event::UIModeEntered);
                    break;
                case HostEvent::UIModeExited:
                    if (Mod* m = FindLocked(s_uiOwner)) targets.emplace_back(SinkOf(*m), Event::UIModeExited);
                    s_uiOwner = 0;
                    s_uiPending = 0;
                    if (!s_uiQueue.empty()) drainQueue = true;
                    break;
                case HostEvent::UIModeRefused:
                    if (s_uiPending == view) s_uiPending = 0;
                    ownerSink(view, Event::UIModeRefused);
                    break;
                case HostEvent::DisplayResized:
                    for (auto& [id, m] : s_mods) targets.emplace_back(SinkOf(m), Event::DisplayResized);
                    break;
                case HostEvent::RenderDead:
                    for (auto& [id, m] : s_mods) targets.emplace_back(SinkOf(m), Event::RenderDead);
                    break;
                }
            }
            for (auto& [s, ev] : targets) Deliver(s, ev, view, x, y, d);
            // The next queued request enters on a later game-thread task, so
            // the leaving mod's UIModeExited lands first and the host's exit
            // has fully settled.
            if (drainQueue) Magelight::GameTask::Post([]() { DrainUIQueue(); });
        }

        void DrainUIQueue()
        {
            while (true) {
                Queued q{};
                {
                    std::lock_guard<std::mutex> lk(s_mutex);
                    if (s_uiQueue.empty() || Magelight::IsUIModeActive() || s_uiPending) return;
                    q = s_uiQueue.front();
                    s_uiQueue.pop_front();
                }
                const std::string detail = "queued request for view " + std::to_string(q.view);
                if (RequestUIMode(q.view, q.flags & ~kUIModeFlagQueue) == Result::Ok) return;
                SKSE::log::info("Magelight[v4]: {} skipped — {}", detail, GetLastErrorMessage(OwnerOf(q.view)));
            }
        }

        void EnsureSink()
        {
            bool install = false;
            {
                std::lock_guard<std::mutex> lk(s_mutex);
                if (!s_sinkInstalled) { s_sinkInstalled = true; install = true; }
            }
            if (install) SetHostEventSink(&HostSink);
        }

    }  // namespace

    // ── Public (served through MagelightApiExport.cpp) ──────────────────────

    namespace {
        // ModDesc.sessionName / manifest "session": nullptr or "" or
        // "isolated" = the mod's own jar (named by its slug); "default" =
        // the shared session v1-v3 views use.
        std::string ResolveSession(const char* requested, const std::string& slug)
        {
            if (!requested || !*requested) return slug;
            const std::string r = Lower(requested);
            if (r == "default") return "";
            if (r == "isolated") return slug;
            return ValidSlug(requested) ? std::string(requested) : slug;
        }
    }

    enum class Registrar { Plugin, Script, Manifest };

    static Result RegisterImpl(const char* modId, const char* displayName, std::uint32_t version,
                               std::uint32_t minHost, CallbackThread thread, EventFn onEvent, LogFn onLog,
                               void* user, const char* sessionName, Registrar by, const char* manifestDir,
                               ModId* outMod)
    {
        const bool manifest = (by == Registrar::Manifest);
        if (outMod) *outMod = 0;
        if (!outMod) return Result::InvalidArgument;
        if (!ValidSlug(modId)) {
            SKSE::log::warn("Magelight[v4]: RegisterMod refused — modId must be 1..{} chars of [A-Za-z0-9_.-] and a valid folder name (not dot-only, dot-terminated or a device name)",
                kMaxSlugLen);
            return Result::InvalidArgument;
        }
        if (minHost > kHostVersionNumber) {
            SKSE::log::warn("Magelight[v4]: RegisterMod('{}') — needs host {} but this is {}",
                modId, minHost, kHostVersionNumber);
            std::lock_guard<std::mutex> lk(s_mutex);
            s_tooOld.emplace_back(modId, minHost);
            return Result::HostTooOld;
        }
        EnsureSink();
        LoadPluginJars();
        Sink sink;
        std::string recordJar;   // a plugin's jar, recorded once the lock is gone
        {
            std::lock_guard<std::mutex> lk(s_mutex);
            const std::string lower = Lower(modId);
            for (auto& [id, m] : s_mods) {
                if (Lower(m.modId) != lower) continue;
                if (m.manifestOnly && !manifest) {
                    // A DLL or a script adopting a manifest mod: attach the sink, keep the views/session.
                    m.manifestOnly = false;
                    m.scriptOwned = (by == Registrar::Script);
                    m.displayName = (displayName && *displayName) ? displayName : m.displayName;
                    m.version = version ? version : m.version;
                    m.thread = thread;
                    m.onEvent = onEvent;
                    m.onLog = onLog;
                    m.user = user;
                    *outMod = m.id;
                    SKSE::log::info("Magelight[v4]: mod {} ('{}') adopted by a {} — {} manifest view(s) from {}, events on the {} thread",
                        m.id, m.modId, m.scriptOwned ? "script" : "plugin", m.views.size(), m.manifestDir,
                        m.thread == CallbackThread::RenderThread ? "emitting" : "game");
                    if (by == Registrar::Plugin) recordJar = m.sessionName;
                    sink = SinkOf(m);
                    break;
                }
                // A script re-registering its own mod (every load) gets it back;
                // a plugin's mod is refused to scripts (the Papyrus tier logs it).
                if (by == Registrar::Script) {
                    if (!m.scriptOwned) return Result::Denied;
                    *outMod = m.id;
                    return Result::Ok;
                }
                SKSE::log::warn("Magelight[v4]: RegisterMod('{}') refused — already registered as mod {}", modId, id);
                return Result::InvalidMod;
            }
            if (!*outMod) {
                // Jars are shared by name, so a plugin's custom session name
                // and a script-tier slug can meet. A manifest arriving second
                // is refused; a plugin arriving second is warned (script-owned
                // pages may already run there), and AccessLocked keeps scripts out.
                const std::string session = ResolveSession(sessionName, modId);
                if (!session.empty() && by != Registrar::Script) {
                    const std::string jar = Lower(session);
                    for (const auto& [oid, o] : s_mods) {
                        if (Lower(o.sessionName) != jar || o.scriptOwned == manifest) continue;
                        if (manifest) {
                            SKSE::log::warn("Magelight[v4]: manifest mod '{}' refused — its storage jar '{}' belongs to plugin mod '{}'",
                                modId, session, o.modId);
                            return Result::Denied;
                        }
                        SKSE::log::warn("Magelight[v4]: RegisterMod('{}') — session '{}' is also the storage jar of script-owned mod '{}', "
                                        "whose pages run in it; scripts can no longer drive that mod's views", modId, session, o.modId);
                        break;
                    }
                }
                Mod m;
                m.id = s_nextMod++;
                m.modId = modId;
                m.displayName = (displayName && *displayName) ? displayName : modId;
                m.version = version;
                m.thread = thread;
                m.onEvent = onEvent;
                m.onLog = onLog;
                m.user = user;
                m.sessionName = session;
                m.manifestOnly = manifest;
                if (manifest && manifestDir) m.manifestDir = manifestDir;
                m.scriptOwned = (by != Registrar::Plugin);
                if (by == Registrar::Plugin) recordJar = session;
                *outMod = m.id;
                SKSE::log::info("Magelight[v4]: mod {} registered — '{}' ({}) v{}{}, session '{}' (events on the {} thread{})",
                    m.id, m.modId, m.displayName, m.version,
                    manifest ? " [manifest]" : by == Registrar::Script ? " [script]" : "",
                    m.sessionName.empty() ? "default" : m.sessionName,
                    m.thread == CallbackThread::RenderThread ? "emitting" : "game",
                    m.onEvent ? "" : ", no event sink");
                sink = SinkOf(m);
                s_mods.emplace(m.id, std::move(m));
            }
        }
        RecordPluginJar(recordJar);
        // A host that already died says so at once, so the mod can no-op.
        if (Magelight::IsRenderDead()) Deliver(sink, Event::RenderDead, 0, 0, 0, Magelight::RenderDeadReason());
        return Result::Ok;
    }

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
    // this; the struct was missing it — review 2026-09-05).
    Result FindView(ModId mod, const char* name, ViewId* out)
    {
        if (out) *out = 0;
        const ViewId v = FindViewByName(mod, name);
        if (!v) return Result::InvalidView;
        if (out) *out = v;
        return Result::Ok;
    }

    std::string TooOldJson()
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        if (s_tooOld.empty()) return {};
        std::string out = "{\"host\":\"" PLUGIN_VERSION "\",\"mods\":[";
        bool first = true;
        for (const auto& [id, needs] : s_tooOld) {
            if (!first) out += ',';
            first = false;
            out += "{\"modId\":\"" + id + "\",\"needs\":" + std::to_string(needs) + "}";
        }
        return out + "]}";
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
        if (Magelight::IsRenderDead()) return Fail(mod, Result::RenderDead, DeadMessage("CreateViewEx"));
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
        if (Magelight::IsRenderDead()) return Fail(owner, Result::RenderDead, DeadMessage("RequestUIMode"));
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
        out->uiScale = info.deviceScale;   // 0.26.9: round-trips (review 2026-09-09)
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

    namespace {
        // A hotkey must never OPEN over an engine menu, the console, or a
        // text field the game owns; closing is always allowed.
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
        // PAGE-side keys). MUST match the header doc and the SDK mock — it
        // drifted twice: pagebridge missing (review 2026-09-05), then the
        // 0.27/0.28 names missing from the other two and networkPolicy from
        // here (review 2026-09-09). Add to all three or to none.
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

    // ── 0.18.0 (VR-4): per-view VR placement + controller bindings ─────────
    Result SetViewVRPlacement(ViewId view, const MAGELIGHT_API::VRPlacementDesc* desc)
    {
        if (!Magelight::IsViewValid(view)) return Result::InvalidView;
        if (!desc) {   // documented "restore the layer default" — do exactly that (finding 6)
            Magelight::VR::ClearPlacement(view);
            return Result::Ok;
        }
        if (desc->size < sizeof(MAGELIGHT_API::VRPlacementDesc)) return Result::InvalidArgument;
        Magelight::VR::Placement p;
        p.mode = static_cast<Magelight::VR::Mode>(desc->mode);
        if (desc->distanceMeters > 0.0f) p.distanceMeters = std::clamp(desc->distanceMeters, 0.2f, 20.0f);
        if (desc->widthMeters > 0.0f)    p.widthMeters    = std::clamp(desc->widthMeters, 0.05f, 20.0f);
        p.heightOffset = std::clamp(desc->heightOffset, -5.0f, 5.0f);
        Magelight::VR::SetPlacement(view, p);
        return Result::Ok;
    }

    Result GetViewVRPlacement(ViewId view, MAGELIGHT_API::VRPlacementDesc* out)
    {
        if (!out || out->size < sizeof(MAGELIGHT_API::VRPlacementDesc)) return Result::InvalidArgument;
        if (!Magelight::IsViewValid(view)) return Result::InvalidView;
        Magelight::VR::Placement p;
        if (!Magelight::VR::GetPlacement(view, p)) return Result::NotReady;   // no overlay yet
        out->mode = static_cast<std::uint8_t>(p.mode);
        out->distanceMeters = p.distanceMeters;
        out->widthMeters = p.widthMeters;
        out->heightOffset = p.heightOffset;
        out->curvature = p.curvature;
        out->autoCloseMeters = p.autoCloseMeters;
        return Result::Ok;
    }

    Result RecenterVRView(ViewId view)
    {
        if (!Magelight::IsViewValid(view)) return Result::InvalidView;
        Magelight::VR::Recenter(view);
        return Result::Ok;
    }

    Result BindVRHotkey(ViewId view, const MAGELIGHT_API::VRHotkeyDesc* desc)
    {
        if (!Magelight::IsViewValid(view)) return Result::InvalidView;
        if (!desc) { Magelight::VR::BindHotkey(view, 0, 0, 0, 0); return Result::Ok; }
        if (desc->size < sizeof(MAGELIGHT_API::VRHotkeyDesc)) return Result::InvalidArgument;
        if (desc->hand > 2) return Result::InvalidArgument;
        Magelight::VR::BindHotkey(view, desc->button, desc->modifier, desc->hand, desc->action);
        return Result::Ok;
    }

    Result BindVRHotkeyCallback(ViewId view, const MAGELIGHT_API::VRHotkeyDesc* desc,
                                MAGELIGHT_API::VRHotkeyFn cb, void* user)
    {
        if (!Magelight::IsViewValid(view)) return Result::InvalidView;
        if (!desc || !cb) { Magelight::VR::BindHotkeyCallback(view, 0, 0, 0, nullptr, nullptr); return Result::Ok; }
        if (desc->size < sizeof(MAGELIGHT_API::VRHotkeyDesc)) return Result::InvalidArgument;
        if (desc->hand > 2) return Result::InvalidArgument;
        Magelight::VR::BindHotkeyCallback(view, desc->button, desc->modifier, desc->hand,
                                          reinterpret_cast<Magelight::VR::HotkeyFn>(cb), user);
        return Result::Ok;
    }

    Result SetVRButtonListener(MAGELIGHT_API::VRButtonEdgeFn cb, void* user)
    {
        // No view: this is process-wide capture mode, not a per-view binding.
        Magelight::VR::SetButtonListener(reinterpret_cast<Magelight::VR::ButtonEdgeFn>(cb), user);
        return Result::Ok;
    }

    void SetHotkeyOverride(const char* modSlashView, std::uint32_t dxScancode)
    {
        if (!modSlashView || !*modSlashView) return;
        std::lock_guard<std::mutex> lk(s_mutex);
        s_hotkeyOverrides[Lower(modSlashView)] = dxScancode;
    }

    namespace {
        struct EvalCtx { Sink sink; MAGELIGHT_API::JsResultFn cb; void* user; };
        // Render thread, right after the script ran. Hands the strings to the
        // mod on ITS thread; the alive token drops a result queued for a mod
        // that unregistered meanwhile.
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
    }

    Result SetViewCutout(ViewId view, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h)
    {
        if (!OwnerOf(view)) return Result::InvalidView;
        if (!Magelight::IsViewValid(view)) return Result::InvalidView;
        Magelight::SetViewCutout(view, x, y, w, h);
        return Result::Ok;
    }

    // Per-view setters share one gate (review 2026-09-09): the view must be a
    // v4 view (OwnerOf — the same gate SetViewCutout/SetViewHibernate use; the
    // C ABI carries no caller id, so this is API-created, not caller-owned)
    // and alive, and every refusal records a message for GetLastErrorMessage
    // and the mod's onLog instead of a bare Result.
    static Result GateView(ViewId view, const char* fn)
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

    // ── 0.28.0 ──
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
    // views it already has (PushNetLevel). Logged only when the level
    // changes, so a script re-applying it on every load stays quiet.
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

    namespace {
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
    }

    // 0.30.1. Any thread; inside one of the host's scopes (a callback inside a frame, its input, window-message,
    // engine-menu, SKSE-message or Papyrus handling) the post goes through the pump, elsewhere straight to SKSE
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

    Result SetViewSounds(ViewId view, const char* open, const char* close)
    {
        if (const Result g = GateView(view, "SetViewSounds"); g != Result::Ok) return g;
        Magelight::SetViewSounds(view, open, close);
        return Result::Ok;
    }

    Result SetViewHibernate(ViewId view, std::uint32_t idleMs)
    {
        if (!OwnerOf(view)) return Result::InvalidView;
        if (!Magelight::IsViewValid(view)) return Result::InvalidView;
        Magelight::SetViewHibernate(view, idleMs);
        return Result::Ok;
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

    std::uint32_t HostVersionNumber() { return kHostVersionNumber; }

    // ── Dogfood: the host's own probe view rides the v4 path ─────────────────
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
