#pragma once
// The host's Ultralight platform bindings — stage 4 of the split.
//
// ── What this is ───────────────────────────────────────────────────────────
//
// Everything the host hands to `ultralight::Platform::instance()` plus the
// file-URL plumbing they depend on. Ultralight calls these from WEBKIT
// THREADS (not the render thread), which is the constraint that shapes this
// file:
//
//   s_ulLogger          UL's log lines into Magelight.log with a [UL] prefix
//   s_clipboard         Win32 clipboard for WebKit's editing commands
//   s_networkListener   the network SANDBOX (invariant 5's other half)
//   s_fileSystem        our own FileSystem: fixed MIME table, query-string
//                       strip, percent-decode, absolute drive-letter paths
//
// Plus the path primitives the sandbox and the view loader share, and the
// page-root pin that lets a page outside Data\Magelight be served.
//
// ── Rules that keep this correct ───────────────────────────────────────────
//
//  1. CLAUDE.md invariant 5: we use OUR OWN FileSystem, never AppCore's.
//     AppCore's resolves MIME types from the Windows REGISTRY, where .js is
//     frequently text/plain or absent. A module script then hard-requires a
//     JavaScript MIME, refuses, and the Vite bundle loads as a fully
//     transparent silently-empty page — the 0.5.1 field report. The fixed
//     extension table in GetFileMimeType removes the machine-dependence
//     entirely. Do not "simplify" this back to AppCore's.
//  2. WebKit threads reach the view registry ONLY under s_viewsMutex (never
//     the page-root mutex while holding it — they are separate, and taking
//     them in the other order than the code below would be a lock-order bug).
//     s_pageRoots has its OWN mutex because Resolve() runs on worker threads
//     while the game thread may hold s_viewsMutex; pruning those roots was
//     considered and rejected — it races a worker read against the destroy,
//     and keeping a destroyed view's folder readable costs nothing.
//  3. `ResolveFileUrl` and `PathIsUnder` are PURE. PathIsUnder does string
//     work only (no filesystem calls) precisely so it is safe to call from a
//     WebKit thread; keep it that way.
//  4. Loopback parsing accepts 127.0.0.0/8 only as a DOTTED QUAD. A "127."
//     prefix test let the legal hostname 127.attacker.example through the
//     sandbox (review 2026-09-09) — do not reintroduce it.

#include "Magelight.h"
#include "host/State.h"

#include <filesystem>
#include <string>

#pragma warning(push)
#pragma warning(disable : 4100)
#include <Ultralight/Ultralight.h>
#pragma warning(pop)

namespace Magelight {

    // ── Platform singletons (installed by the renderer-init section) ─────────
    // These are the exact objects handed to Platform::instance(); the renderer
    // section that owns the install must keep its addresses stable, so they
    // live here rather than in that section's anonymous namespace.

    extern ultralight::Logger&          UlLogger();
    extern ultralight::Clipboard&       UlClipboard();
    extern ultralight::FileSystem&      UlFileSystem();
    extern ultralight::NetworkListener& UlNetworkListener();

    // ── Path primitives ─────────────────────────────────────────────────────

    // Case-insensitive prefix test on normalised native paths (Windows).
    bool PathIsUnder(const std::filesystem::path& p, const std::filesystem::path& base);

    // file:///-URL → filesystem path, through the same resolve (and therefore
    // the same refusals) as a page's own file requests.
    std::filesystem::path ResolveFileUrl(const std::string& url);

    // The folder a page may read from (MlView::root). A drive-letter page
    // under Data\Magelight\<Mod>\... pins to that mod folder; any other
    // absolute page pins to its own directory; a relative (host) page pins
    // to the runtime dir. Host resources are always readable on top.
    std::filesystem::path PageRootFor(const std::string& htmlPath);

    // ── Page roots outside Data\Magelight ──────────────────────────────────
    // Append-only, never pruned — see rule 2.

    void NotePageRoot(const std::filesystem::path& root);
    bool IsUnderRegisteredPageRoot(const std::filesystem::path& p);

}  // namespace Magelight