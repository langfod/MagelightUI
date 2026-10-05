// Host — telemetry and startup: the crash VEH, the stall watchdog, and the
// runtime preload that binds the namespaced Ultralight modules plus the
// optional GPU backend.
//
// This is the only part of the host that runs BEFORE the renderer exists:
// PreloadRuntime is called from SKSEPluginLoad, before any frame. Everything it
// touches is therefore state that has to be visible to the sections that run
// later — see State.h.
//
// The two diagnostics here share one shape on purpose: they are the only way
// to learn about a failure the game will not report. A hard freeze produces no
// crash log, and a first-chance exception in a mod's code produces no crashlog
// unless someone walks it. Both are log-only and neither ever swallows or
// blocks.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <tlhelp32.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include "Magelight.h"
#include "host/State.h"

namespace Magelight {

    // ── State this file owns (defined once, shared by all host TUs) ─────────

    std::atomic<bool> s_runtimeReady{ false };
    std::atomic<bool> s_hookInstalled{ false };
    std::atomic<bool> s_worldReady{ false };
    std::atomic<bool> s_renderDead{ false };
    char            s_renderDeadReason[192] = {};
    std::atomic<int> s_renderDeadReasonState{ 0 };

    // First caller wins (see the header). Compare-and-set on 0 -> 1 makes the
    // earliest failure the recorded one and keeps every later caller out, so a
    // consequence never overwrites its cause.
    void NoteRenderFailure(const char* what, const char* detail)
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

    DWORD s_ulThreadId = 0;
    DWORD s_mainThreadId = 0;

    std::atomic<long long> s_lastPresentMs{ 0 };
    std::atomic<DWORD>     s_lastPresentTid{ 0 };
    std::atomic<bool>      s_stallWatchdog{ true };
    std::atomic<int>       s_stallThresholdMs{ 1500 };

    // 0.31.0 host settings. Declared in host/State.h; see the comment there for
    // what each one gates. Defined here because LoadHostSettings runs before the
    // renderer exists, and the freeze/load-stagger readers live in three other TUs.
    std::atomic<bool> s_freezeWorldCfg{ true };
    std::atomic<bool> s_freezeSkipCapture{ true };
    std::atomic<bool> s_loadStagger{ true };
    std::atomic<int>  s_loadBudgetMs{ 8 };
    // MSAA for page targets. 4 is upstream's default; the card may give less.
    std::atomic<int>  s_msaa{ 4 };
    std::atomic<int>  s_consoleLogCfg{ -1 };   // -1 = unset (see ConsoleLogLevel)

    std::filesystem::path s_runtimeDir;

    // Focus + UI mode. Defined here because the stall watchdog's sample line
    // reads them (it runs long after startup and must name what the user was
    // doing); written by the input sections.
    std::atomic<bool>          s_focused{ false };
    std::atomic<std::uint64_t> s_uiModeView{ 0 };   // UI mode's show/focus target (0 = off)

    // The game's output window (captured at hook install) and the devMode
    // flag. Neither is telemetry, but both are host-wide state that the
    // watchdog's TU already owns as the one that runs before the renderer,
    // and both have constant initialisers so relocation cannot change
    // static-initialisation order (see host/State.h rule 3).
    HWND                s_hwnd = nullptr;
    std::atomic<bool>   s_devMode{ false };   // "devMode": hot reload + JS error banner

    MgGpuCreateFn   s_gpuCreate = nullptr;
    MgGpuDestroyFn  s_gpuDestroy = nullptr;
    MgGpuGetDrvFn   s_gpuGetDriver = nullptr;
    MgGpuHasFn      s_gpuHas = nullptr;
    MgGpuDrawFn     s_gpuDraw = nullptr;
    MgGpuSrvFn      s_gpuSrv = nullptr;
    MgGpuSamplesFn  s_gpuSamples = nullptr;
    MgGpuRegExtFn   s_gpuRegExt = nullptr;
    MgGpuSetExtFn   s_gpuSetExtSrv = nullptr;
    MgGpuUnregExtFn s_gpuUnregExt = nullptr;
    void*           s_gpu = nullptr;
    bool            s_gpuActive = false;

    std::filesystem::path GameRoot()
    {
        wchar_t buf[MAX_PATH]{};
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        return std::filesystem::path(buf).parent_path();
    }

    // The driver's log lines arrive on ITS threads and have no prefix; route
    // them into ours so a backend message is attributable. Declared in
    // State.h — the D3D section that creates the backend installs this
    // callback, and Telemetry is where the backend is bound.
    void GpuLogBridge(const char* msg)
    {
        SKSE::log::error("Magelight: {}", msg ? msg : "?");
    }

    namespace {

        // ── Crash telemetry (0.3.2 diagnostics) ─────────────────────────
        // A first-chance vectored handler that LOGS severe exceptions from ANY
        // thread — code, address, owning module + offset — then continues the
        // search (log-only, never swallows). The field CTD produces no crashlog
        // and doesn't trip our SEH nets, so it lives on a thread/path we don't
        // guard; this names it. Rate-limited; known-handled families skipped.
        // module+offset for an address, module BASENAME: one resolver and one
        // format for the VEH logger and the stall watchdog (review 2026-09-09).
        // `path` is scratch the returned pointer points into.
        const char* ModuleOffsetOf(void* addr, char (&path)[MAX_PATH], std::uintptr_t& off)
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
        thread_local bool t_walkingStack = false;

        // One logged line per DISTINCT fault (code + owning module + offset), for the life of the process.
        // The previous scheme was a global cap of 8 EVENTS, which the CrashLogger symbolizer spent
        // 7 of on its own first-chance reads while walking frames to produce a crashlog (2026-10-02):
        // those fault per thread, on purpose, and they pass every filter above. The one real fault
        // survived only because it arrived 24 ms earlier — and any second genuine fault was then
        // invisible for the rest of the session. A count cap also cannot express "the same bad call
        // every frame", which is exactly the shape a broken hook has.
        //
        // Rules this table must keep:
        //   - No allocation and no mutex. This runs inside a vectored handler; new may throw
        //     bad_alloc and an exception must never unwind out of here.
        //   - The racy counter is acceptable: worst case two threads log one identical fault each.
        //   - Never swallow. Every path still returns EXCEPTION_CONTINUE_SEARCH.
        struct SeenFault {
            std::uint32_t code;
            std::uintptr_t module;   // module base, so equal bases mean the same module
            std::uintptr_t offset;
        };
        constexpr int kMaxSeenFaults = 64;
        SeenFault s_seen[kMaxSeenFaults]{};
        std::atomic<int>  s_seenCount{ 0 };
        std::atomic<bool> s_seenFull{ false };

        // Defined below, next to the unwinder it reuses.
        void LogVehStack(EXCEPTION_POINTERS* ep);

        LONG CALLBACK VectoredLogger(EXCEPTION_POINTERS* ep)
        {
            if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
            const auto code = ep->ExceptionRecord->ExceptionCode;
            if (code < 0xC0000000u) return EXCEPTION_CONTINUE_SEARCH;                       // not severe
            if (code == 0xC06D007Eu || code == 0xC06D007Fu) return EXCEPTION_CONTINUE_SEARCH;  // delay-load internals
            if (code == 0xE06D7363u) return EXCEPTION_CONTINUE_SEARCH;                      // C++ EH in flight
            if (t_walkingStack) return EXCEPTION_CONTINUE_SEARCH;                           // the watchdog's own, caught

            void* addr = ep->ExceptionRecord->ExceptionAddress;
            char modPath[MAX_PATH]; std::uintptr_t offset = 0;
            const char* modName = ModuleOffsetOf(addr, modPath, offset);

            // CrashLogger's symbolizer is not a fault of ours. Filter on the module OWNING THE
            // FAULTING ADDRESS, not on whether CrashLogger is loaded or on the stack: a real
            // fault that merely happens while it is running still resolves to the game's or a
            // mod's module and is still logged below.
            if (std::strcmp(modName, "CrashLogger.dll") == 0) return EXCEPTION_CONTINUE_SEARCH;

            const auto mod = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(modPath));
            // Clamp the scan: s_seenCount must never be trusted as a loop bound on its own.
            const int seen = s_seenCount.load(std::memory_order_relaxed);
            const int limit = seen < kMaxSeenFaults ? seen : kMaxSeenFaults;
            for (int i = 0; i < limit; ++i) {
                if (s_seen[i].code == code && s_seen[i].module == mod && s_seen[i].offset == offset)
                    return EXCEPTION_CONTINUE_SEARCH;   // already reported; do not spend a line on frame N of it
            }

            if (seen < kMaxSeenFaults) {
                s_seen[seen] = SeenFault{ code, mod, offset };
                s_seenCount.store(seen + 1, std::memory_order_relaxed);
            } else {
                // Stay quiet once the table is full, but say so ONCE — going silent silently
                // is what made the old 8-event cap unreadable.
                if (!s_seenFull.exchange(true))
                    SKSE::log::error("Magelight VEH: distinct-fault table full ({} entries); "
                                     "further faults are NOT logged", kMaxSeenFaults);
                return EXCEPTION_CONTINUE_SEARCH;
            }

            SKSE::log::error(
                "Magelight VEH: exception 0x{:08X} at {} (module {} +0x{:X}) on thread {}",
                code, addr, modName, offset, GetCurrentThreadId());
            // Then the callers. This is the whole point of the line above: a fault
            // address in SkyrimSE.exe names a game function but not the path that
            // reached it, and "our hook did it" versus "another mod did it" versus
            // "the game's own load path did it" are three different bugs. Always
            // attempted, never allowed to swallow the exception below it — LogVehStack
            // catches its own faults and returns.
            LogVehStack(ep);
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // ── Stall watchdog (0.28.3) ──────────────────────────────────────
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

        bool ThreadStackRange(HANDLE thread, std::uintptr_t& low, std::uintptr_t& high)
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
        int UnwindFromContext(CONTEXT* ctx, void** frames, int cap, std::uintptr_t stackLow, std::uintptr_t stackHigh)
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

        // ── VEH stack dump (2026-10-03) ─────────────────────────────────────
        // The VEH named the faulting module and offset but not the CALLERS, which
        // left "Magelight VEH: 0xC0000005 at SkyrimSE.exe+0x1AD5B40" unattributable:
        // an address in the game can be reached through our hook, through another
        // mod, or through the game's own load path. Only a stack can say which.
        //
        // Unwinding is safe HERE and only here, because we already hold the exact
        // CONTEXT of the faulting thread — no suspension, no cross-thread read, no
        // window in which the thread runs on and leaves a stale stack. That is the
        // same guarantee RtlLookupFunctionEntry/RtlVirtualUnwind need, and it is
        // why this goes in the vectored handler rather than the watchdog (which
        // has to suspend, and cannot do that from inside an exception).
        //
        // Rules this must keep:
        //   - Never swallow. Still returns EXCEPTION_CONTINUE_SEARCH on every path.
        //   - No allocation. Logging below is the same log-only call the handler
        //     already makes; the frames go in a fixed array on this frame.
        //   - No mutex, and never re-enter: t_walkingStack guards the unwinder's
        //     own faults, which are first-chance and would otherwise recurse.
        //   - Cap the depth. A deep or corrupt unwind must cost a bounded number
        //     of frames, not the thread's remaining time.
        constexpr int kVehMaxFrames = 24;

        void LogVehStack(EXCEPTION_POINTERS* ep)
        {
            if (!ep || !ep->ContextRecord) return;
            void* frames[kVehMaxFrames]{};

            // The faulting thread's own stack bounds, from its TEB — no handle to
            // leak and nothing to suspend.
            //
            // Pseudo-handles are NOT symmetric, and getting this wrong is what made
            // the first field dump worthless: -1 is the current PROCESS, -2 is the
            // current THREAD. Passing -1 made the kernel resolve the process
            // object, the ThreadBasicInformation query then failed, the range came
            // back empty and EVERY dump silently degraded to the single frame we
            // already had ("stack (1 frames)", 2026-10-03 field log) — a diagnostic
            // that looks like it worked. NtQueryCurrentThread is preferred where it
            // exists; both spellings want the calling THREAD.
            std::uintptr_t stackLow = 0, stackHigh = 0;
            bool haveRange = false;
            {
                using QueryFn = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
                auto* ntdll = GetModuleHandleW(L"ntdll.dll");
                const auto querySelf = reinterpret_cast<QueryFn>(GetProcAddress(ntdll, "NtQueryCurrentThread"));
                const auto queryInfo = reinterpret_cast<QueryFn>(GetProcAddress(ntdll, "NtQueryInformationThread"));
                const auto query = querySelf ? querySelf : queryInfo;
                MlThreadBasicInfo info{};
                if (query && query(reinterpret_cast<HANDLE>(-2), 0, &info, sizeof(info), nullptr) >= 0
                    && info.TebBaseAddress) {
                    const auto* tib = static_cast<const NT_TIB*>(info.TebBaseAddress);
                    stackLow  = reinterpret_cast<std::uintptr_t>(tib->StackLimit);
                    stackHigh = reinterpret_cast<std::uintptr_t>(tib->StackBase);
                    haveRange = stackHigh > stackLow;
                }
            }
            // Never degrade silently again: a one-frame dump is indistinguishable
            // from a genuinely shallow stack unless we say which it was.
            if (!haveRange)
                SKSE::log::error("Magelight VEH: stack range unavailable for thread {} — "
                                 "dumping the faulting frame only", GetCurrentThreadId());

            // Work on a COPY: UnwindFromContext advances Rip/Rsp, and the caller
            // still needs the original record intact for its own logging.
            CONTEXT ctx = *ep->ContextRecord;
            t_walkingStack = true;
            const int n = UnwindFromContext(&ctx, frames, kVehMaxFrames, stackLow, stackHigh);
            t_walkingStack = false;

            if (n <= 0) {
                SKSE::log::error("Magelight VEH: no stack for the faulting thread (code {})", n);
                return;
            }
            SKSE::log::error("Magelight VEH: faulting thread {} stack ({} frames):",
                             GetCurrentThreadId(), n);
            for (int i = 0; i < n; ++i) {
                char path[MAX_PATH]; std::uintptr_t off = 0;
                const char* base = ModuleOffsetOf(frames[i], path, off);
                SKSE::log::error("Magelight VEH:   #{:02} {}+0x{:X}", i, base, off);
            }
        }

        constexpr int kMaxStackFrames = 40;

        void LogThreadStack(DWORD tid, const char* label, long long gapMs, int maxFrames = kMaxStackFrames)
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
        void LogOtherThreadStacks(DWORD presentTid, DWORD mainTid, long long gapMs)
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

        void StallWatchdogLoop()
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
    }  // namespace

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
        // PE edit — cmake/stage/Runtime.cmake), so both Ultralight runtimes
        // coexist under different module names.
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
    s_gpuSamples  = reinterpret_cast<MgGpuSamplesFn>(GetProcAddress(gpu, "MgGpu_SetSampleCount"));
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

}  // namespace Magelight