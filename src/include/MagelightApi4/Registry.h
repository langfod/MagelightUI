#pragma once
// Shared internals of the v4 mod registry, split across src/MagelightApi4/*.cpp.
//
// Why this header exists: the registry's state was one anonymous namespace in a
// single TU. Once the layer became several TUs, an anonymous namespace would
// give each file its own copy of s_mutex and s_mods — the split would still
// compile and would be silently broken. The types and lock-held helpers below
// are the state plus the helpers that require s_mutex, so every caller and every
// helper stays in one place.
//
// Rule that keeps this correct (CLAUDE.md invariant 10's cousin): the registry
// owns s_mutex; its data and the helpers documented "s_mutex held" travel
// together. A helper marked "Never with s_mutex held" may not appear in this
// header unless it also says why.

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "Magelight.h"
#include "MagelightApi4.h"


namespace Magelight::Api4 {

    using MAGELIGHT_API::CallbackThread;
    using MAGELIGHT_API::Event;
    using MAGELIGHT_API::EventData;
    using MAGELIGHT_API::EventFn;
    using MAGELIGHT_API::LogFn;
    using MAGELIGHT_API::ModId;
    using MAGELIGHT_API::Result;
    using MAGELIGHT_API::ViewId;

    inline constexpr std::size_t kMaxSlugLen = 32;   // leaves room for "<slug>.<image>" under the 64-char image-name cap
    inline constexpr std::size_t kMaxImageNameLen = 64;

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

    // kUIModeFlagQueue: requests waiting for the holder to release, FIFO.
    struct Queued { ViewId view; std::uint32_t flags; };

    // BindHotkey registry: one binding per DX scancode, process-wide.
    struct Hotkey { ViewId view; std::uint32_t action; };

    // ── Registry state. Defined once, in Registry.cpp. ──────────────────────

    extern std::mutex s_mutex;
    extern std::map<ModId, Mod> s_mods;
    extern std::map<ViewId, ModId> s_viewOwner;
    extern ModId s_nextMod;
    extern ModId s_uiOwner;        // mirrored from UIModeEntered/Exited (0 = none / a v1-v3 or host view)
    extern ViewId s_uiPending;     // a RequestUIMode whose game-thread entry has not been decided yet
    extern bool s_sinkInstalled;
    extern std::deque<Queued> s_uiQueue;
    extern std::vector<std::pair<std::string, std::uint32_t>> s_tooOld;   // HostTooOld refusals (version gate)
    extern std::map<std::string, std::uint32_t> s_hotkeyOverrides;         // "modid/view" (lower) -> code, 0 = disabled
    extern std::map<std::uint32_t, Hotkey> s_hotkeys;

    // Every storage jar a plugin has used, kept across sessions: the jar's
    // data stays on disk, so it stays off limits to scripts in a session
    // without that plugin. Lowercase names. s_jarMutex may be taken while
    // s_mutex is held, never the reverse; the file is read and appended
    // with neither held.
    extern std::mutex s_jarMutex;
    extern std::mutex s_jarFileMutex;
    extern std::set<std::string> s_pluginJars;
    extern std::once_flag s_pluginJarsLoaded;

    inline constexpr std::uint32_t kHostVersionNumber =
        MAGELIGHT_API::PackVersion(PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR, PLUGIN_VERSION_PATCH);

    // ── Helpers. "s_mutex held" means the caller already holds it. ──────────

    Mod* FindLocked(ModId id);
    ModId OwnerLocked(ViewId view);
    ModId OwnerOf(ViewId view);                       // takes s_mutex
    Sink SinkOf(const Mod& m);
    void DropQueuedLocked(ViewId view);
    void DropHotkeysLocked(ViewId view);
    ScriptAccess AccessLocked(const Mod* m);
    std::string Lower(std::string s);

    // Never called with s_mutex held: the consumer's onLog may re-enter the API,
    // so it runs after the lock is released.
    void SetError(ModId id, const std::string& msg);
    Result Fail(ModId id, Result r, const std::string& msg);

    // Inline (RenderThread) delivery runs under an SEH guard so one mod's fault
    // disables that mod's sink, not the overlay. POD-only frames.
    bool CallGuarded(EventFn fn, const EventData* ev, void* user, ModId id);
    bool CallGuardedResult(MAGELIGHT_API::JsResultFn fn, ViewId view, const char* result, const char* exception,
                           void* user, ModId id);
    void DisableSink(ModId id);
    void Deliver(const Sink& s, Event type, ViewId view, std::int32_t x, std::int32_t y, std::string detail);
    void DeliverUiQueueDrain();                        // posts DrainUIQueue on the game thread
    void EnsureSink();
    void DrainUIQueue();

    // Registry.cpp, but reachable from the API layer: Validate, jar bookkeeping.
    bool ValidSlug(const char* s);
    std::filesystem::path PluginJarsFile();
    void LoadPluginJars();
    bool IsRecordedPluginJar(const std::string& lowerJar);
    void RecordPluginJar(const std::string& jar);

    // Pushes a mod's network level to views outside s_mutex (the host's view
    // lock must never nest in it), then repeats with the stored level while a
    // concurrent SetNetworkPolicy changed it meanwhile, so the last level each
    // view receives is the one the mod holds.
    void PushNetLevel(ModId mod, std::vector<ViewId> views, Magelight::NetLevel level);

    // API.cpp: the shared gate every per-view setter uses (a v4 view, alive).
    Result GateView(ViewId view, const char* fn);

    enum class Registrar { Plugin, Script, Manifest };
    Result RegisterImpl(const char* modId, const char* displayName, std::uint32_t version,
                        std::uint32_t minHost, CallbackThread thread, EventFn onEvent, LogFn onLog,
                        void* user, const char* sessionName, Registrar by, const char* manifestDir,
                        ModId* outMod);

}  // namespace Magelight::Api4