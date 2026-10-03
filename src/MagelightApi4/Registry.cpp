// v4 registry internals — the state, the lock-held helpers, and registration.
//
// This file owns s_mutex. Everything documented "s_mutex held" in Registry.h
// lives here or is inlined by its single caller, so the lock discipline stays
// readable in one place (the same rule the view registry follows in the host).
//
// The public surface these support is split across Api.cpp, Api018.cpp and
// Api028.cpp; the ABI history those files preserve is in api/MagelightUI_API.h.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include "Magelight.h"
#include "MagelightApi4.h"
#include "MagelightGameTask.h"
#include "MagelightApi4/Registry.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace Magelight::Api4 {

    // The whole v4 layer names public ABI types unqualified (Anchor, Layer,
    // Result, kUIModeFlag*); this directive carried that from the original
    // single TU and must stay for every file in the module.
    using namespace MAGELIGHT_API;

    std::mutex s_mutex;
    std::map<ModId, Mod> s_mods;
    std::map<ViewId, ModId> s_viewOwner;
    ModId s_nextMod = 1;
    ModId s_uiOwner = 0;
    ViewId s_uiPending = 0;
    bool s_sinkInstalled = false;
    std::deque<Queued> s_uiQueue;
    std::vector<std::pair<std::string, std::uint32_t>> s_tooOld;
    std::map<std::string, std::uint32_t> s_hotkeyOverrides;
    std::map<std::uint32_t, Hotkey> s_hotkeys;

    std::mutex s_jarMutex;
    std::mutex s_jarFileMutex;
    std::set<std::string> s_pluginJars;
    std::once_flag s_pluginJarsLoaded;

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

    // ── s_mutex held ────────────────────────────────────────────────────────

    Mod* FindLocked(ModId id)
    {
        auto it = s_mods.find(id);
        return it == s_mods.end() ? nullptr : &it->second;
    }

    ModId OwnerLocked(ViewId view)
    {
        auto it = s_viewOwner.find(view);
        return it == s_viewOwner.end() ? 0 : it->second;
    }

    void DropQueuedLocked(ViewId view)
    {
        for (auto it = s_uiQueue.begin(); it != s_uiQueue.end();)
            it = (it->view == view) ? s_uiQueue.erase(it) : it + 1;
    }

    void DropHotkeysLocked(ViewId view)
    {
        for (auto it = s_hotkeys.begin(); it != s_hotkeys.end();)
            it = (it->second.view == view) ? s_hotkeys.erase(it) : std::next(it);
    }

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

    // ── lock released before returning ──────────────────────────────────────

    ModId OwnerOf(ViewId view)
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        return OwnerLocked(view);
    }

    Sink SinkOf(const Mod& m)
    {
        return Sink{ m.id, m.modId, m.thread, m.onEvent, m.user, m.alive };
    }

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

    Result Fail(ModId id, Result r, const std::string& msg)
    {
        SetError(id, msg);
        return r;
    }

    // ── Plugin storage jars (never with s_mutex held: file IO) ──────────────

    std::filesystem::path PluginJarsFile()
    {
        const std::filesystem::path dir = Magelight::CacheDirPath();
        return dir.empty() ? dir : dir / L"plugin jars.txt";
    }

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

    // ── Event delivery ──────────────────────────────────────────────────────

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
    static void HostSink(HostEvent type, ViewId view, int x, int y, const char* detail)
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
        if (drainQueue) DeliverUiQueueDrain();
    }

    void DeliverUiQueueDrain()
    {
        Magelight::GameTask::Post([]() { DrainUIQueue(); });
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

    // ── Registration ────────────────────────────────────────────────────────

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

    Result RegisterImpl(const char* modId, const char* displayName, std::uint32_t version,
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
        if (Magelight::IsRenderDead()) Deliver(sink, Event::RenderDead, 0, 0, 0, "");
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

    std::uint32_t HostVersionNumber() { return kHostVersionNumber; }

}  // namespace Magelight::Api4