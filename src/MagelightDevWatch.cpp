// Dev loop hot reload — see MagelightDevWatch.h.

#include "MagelightDevWatch.h"

#include <SKSE/SKSE.h>
#include <windows.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>


namespace Magelight::Dev {

    namespace {

        struct Watcher {
            std::filesystem::path dir;
            HANDLE hDir = INVALID_HANDLE_VALUE;
            std::atomic<std::uint64_t> lastChange{ 0 };   // GetTickCount64 of the latest change; 0 = quiet
            std::vector<ViewId> views;                    // guarded by s_mutex
        };

        std::mutex s_mutex;
        std::vector<std::unique_ptr<Watcher>> s_watchers;
        constexpr std::uint64_t kQuietMs = 400;

        void WatchThread(Watcher* w)
        {
            alignas(DWORD) char buf[16 * 1024];
            for (;;) {
                DWORD bytes = 0;
                const BOOL ok = ReadDirectoryChangesW(w->hDir, buf, sizeof(buf), TRUE,
                    FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE |
                    FILE_NOTIFY_CHANGE_DIR_NAME,
                    &bytes, nullptr, nullptr);
                if (!ok) {
                    SKSE::log::warn("Magelight[dev]: watcher on {} stopped (error {})", w->dir.string(), GetLastError());
                    return;
                }
                w->lastChange.store(GetTickCount64());
            }
        }

    }  // namespace

    void WatchView(ViewId view, const std::filesystem::path& dirIn)
    {
        std::error_code ec;
        const auto dir = std::filesystem::weakly_canonical(dirIn, ec);
        if (ec || !std::filesystem::is_directory(dir, ec)) {
            SKSE::log::warn("Magelight[dev]: cannot watch {} — not a folder", dirIn.string());
            return;
        }
        std::lock_guard<std::mutex> lk(s_mutex);
        for (auto& w : s_watchers) {
            if (w->dir == dir) {
                for (ViewId v : w->views) if (v == view) return;
                w->views.push_back(view);
                SKSE::log::info("Magelight[dev]: view {} joins the watch on {}", view, dir.string());
                return;
            }
        }
        HANDLE h = CreateFileW(dir.c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            SKSE::log::warn("Magelight[dev]: cannot watch {} (error {})", dir.string(), GetLastError());
            return;
        }
        auto w = std::make_unique<Watcher>();
        w->dir = dir;
        w->hDir = h;
        w->views.push_back(view);
        Watcher* raw = w.get();
        s_watchers.push_back(std::move(w));
        std::thread(WatchThread, raw).detach();   // lives for the process; the registry never shrinks
        SKSE::log::info("Magelight[dev]: watching {} for view {} (hot reload)", dir.string(), view);
    }

    void Tick()
    {
        const std::uint64_t now = GetTickCount64();
        std::vector<ViewId> reload;
        {
            std::lock_guard<std::mutex> lk(s_mutex);
            for (auto& w : s_watchers) {
                const std::uint64_t t = w->lastChange.load();
                if (!t || now - t < kQuietMs) continue;
                w->lastChange.store(0);
                for (ViewId v : w->views) reload.push_back(v);
            }
        }
        for (ViewId v : reload) {
            if (!Magelight::IsViewValid(v)) continue;
            SKSE::log::info("Magelight[dev]: files changed — reloading view {}", v);
            Magelight::ReloadView(v);
        }
    }

}  // namespace Magelight::Dev
