#pragma once
// MagelightGameTask — how the host, and through the v4 API its mods, posts game-thread work.
//
// On Skyrim VR, SKSE also drains its task queue on game job threads and holds the queue's lock until the queue is
// empty. An AddTask made on the main thread while it is inside Present (every listener, DOM-ready, console, event
// and VR callback runs there) or handling input, window messages, engine-menu or SKSE messages or a Papyrus native
// waits for that lock, and if the job being drained waits for the main thread to finish its frame, the game hangs
// (seen on the first load of a large save, when every view loads while the save's post-load backlog drains). So
// those entry points hold a Scope, and a post made inside one goes to a FIFO that one pump thread hands to SKSE.
//
// Anywhere else a post goes straight to SKSE, as before. Inside an SKSE task that is the queue the task's own thread
// already holds, so the post never waits and runs in the same drain, in order with the caller's own AddTask calls
// (consumers sequence work after a UI-mode call that way). A post made outside a scope can run before an earlier
// scoped one that is still in the pump.

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

#include <SKSE/SKSE.h>

namespace Magelight::GameTask
{
    /// How many scopes the calling thread is inside.
    inline int& Depth() noexcept
    {
        thread_local int depth = 0;
        return depth;
    }

    /// Held by the host's main-thread entry points: HookPresent, the window proc, the input sink, the focus menu's
    /// message handler, the SKSE message handler and the Papyrus natives that post.
    struct Scope
    {
        Scope() noexcept { ++Depth(); }
        ~Scope() { --Depth(); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };

    inline bool Inside() noexcept { return Depth() > 0; }

    namespace detail
    {
        /// Push takes only the queue's lock, so only the pump thread ever waits on SKSE. Whatever queued while the
        /// pump was waiting goes out as one SKSE task, in order.
        class Pump
        {
        public:
            void Start()
            {
                std::lock_guard<std::mutex> lk(m_mx);
                StartLocked();
            }

            void Push(std::function<void()> fn)
            {
                {
                    std::lock_guard<std::mutex> lk(m_mx);
                    m_queue.push_back(std::move(fn));
                    StartLocked();
                }
                m_cv.notify_one();
            }

        private:
            void StartLocked() noexcept
            {
                if (m_started) return;
                try {
                    std::thread([this]() { Run(); }).detach();
                    m_started = true;
                } catch (...) {
                    // Left queued; the next Push tries again.
                }
            }

            void Run()
            {
                for (;;) {
                    std::deque<std::function<void()>> batch;
                    {
                        std::unique_lock<std::mutex> lk(m_mx);
                        m_cv.wait(lk, [this]() { return !m_queue.empty(); });
                        batch.swap(m_queue);
                    }
                    const std::size_t count = batch.size();
                    auto* ti = SKSE::GetTaskInterface();
                    if (!ti) {
                        SKSE::log::error("Magelight: no SKSE task interface - {} game-thread post(s) dropped", count);
                        continue;
                    }
                    try {
                        if (count == 1) ti->AddTask(std::move(batch.front()));
                        else ti->AddTask([jobs = std::move(batch)]() { for (const auto& job : jobs) job(); });
                    } catch (...) {
                        SKSE::log::error("Magelight: a game-thread post failed - {} job(s) dropped", count);
                    }
                }
            }

            std::mutex m_mx;
            std::condition_variable m_cv;
            std::deque<std::function<void()>> m_queue;
            bool m_started = false;
        };

        inline Pump& ThePump()
        {
            // Never destroyed: the pump thread is detached and uses it until the process ends.
            static Pump& pump = *new Pump();
            return pump;
        }
    }

    /// Start the pump thread before the first frame (InstallHook), so no thread is created inside Present.
    inline void Start() { detail::ThePump().Start(); }

    /// Run fn on the game thread as an SKSE task: through the pump inside a Scope, straight to SKSE otherwise.
    inline void Post(std::function<void()> fn)
    {
        if (!fn) return;
        if (Inside()) {
            detail::ThePump().Push(std::move(fn));
            return;
        }
        auto* ti = SKSE::GetTaskInterface();
        if (!ti) {
            SKSE::log::error("Magelight: no SKSE task interface - a game-thread post was dropped");
            return;
        }
        ti->AddTask(std::move(fn));
    }
}
