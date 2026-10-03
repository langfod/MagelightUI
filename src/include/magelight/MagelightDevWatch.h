#pragma once
// Dev loop: hot reload. A watched folder (ReadDirectoryChangesW, recursive,
// one thread per folder) marks its views for reload; the render thread's
// Tick issues ReloadView once the folder has been quiet for a moment, so an
// editor's save burst is one reload. Enabled by Magelight.json "devMode"
// (every v4 view's page folder) or a manifest's "dev": true (that mod's
// folder). Off = zero threads, zero cost.

#include "Magelight.h"

#include <filesystem>

namespace Magelight::Dev {

    void WatchView(ViewId view, const std::filesystem::path& dir);   // any thread
    void Tick();                                                     // render thread, before ApplyPendingLifecycle

}  // namespace Magelight::Dev
