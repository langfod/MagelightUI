// The host's Ultralight platform bindings — stage 4 of the split. See
// Platform.h for what these are and the four rules that keep them correct.
//
// THREADING: every callback below runs on a WEBKIT THREAD, not the render
// thread. They touch the view registry only under s_viewsMutex and never call
// into game state (CLAUDE.md invariant 2 — game state is GameTask::Post).

#include "host/Platform.h"
#include "host/Views.h"

#include <SKSE/SKSE.h>
#include <windows.h>

#include <atomic>
#include <cwctype>
#include <fstream>
#include <mutex>
#include <vector>

#pragma warning(push)
#pragma warning(disable : 4100)
#include <Ultralight/platform/FileSystem.h>
#include <Ultralight/platform/Logger.h>
#include <Ultralight/platform/Platform.h>
// NetworkListener is declared in Ultralight.h itself, not under platform/.
#pragma warning(pop)

namespace Magelight {

    // ── SpdLogger: UL's log lines, prefixed so they are findable ───────────

    class SpdLogger final : public ultralight::Logger {
    public:
        void LogMessage(ultralight::LogLevel, const ultralight::String& message) override
        {
            SKSE::log::info("[UL] {}", message.utf8().data());
        }
    };

    // ── MlClipboard: WebKit's editing commands need a real clipboard ───────
    //
    // Ultralight never sets one itself — without this every text field
    // silently ignores paste. Called on the render thread; OpenClipboard is
    // cheap and synchronous. Pages can only READ it through a user-initiated
    // paste (this WebKit build has no JS clipboard API).

    class MlClipboard final : public ultralight::Clipboard {
    public:
        void Clear() override
        {
            if (!OpenClipboard(s_hwnd)) return;
            EmptyClipboard();
            CloseClipboard();
        }
        ultralight::String ReadPlainText() override
        {
            ultralight::String out;
            if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(s_hwnd)) return out;
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                if (auto* w = static_cast<const wchar_t*>(GlobalLock(h))) {
                    const std::size_t len = wcsnlen(w, GlobalSize(h) / sizeof(wchar_t));
                    ultralight::String16 s16(reinterpret_cast<const ultralight::Char16*>(w), len);
                    out = ultralight::String(s16);
                    GlobalUnlock(h);
                }
            }
            CloseClipboard();
            return out;
        }
        void WritePlainText(const ultralight::String& text) override
        {
            const ultralight::String16 s16 = text.utf16();
            const std::size_t len = s16.length();
            if (!OpenClipboard(s_hwnd)) return;
            EmptyClipboard();
            if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (len + 1) * sizeof(wchar_t))) {
                if (auto* dst = static_cast<wchar_t*>(GlobalLock(h))) {
                    std::memcpy(dst, s16.data(), len * sizeof(wchar_t));
                    dst[len] = L'\0';
                    GlobalUnlock(h);
                    if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
                } else {
                    GlobalFree(h);
                }
            }
            CloseClipboard();
        }
    };

    // ── Loopback parsing (the 127.0.0.0/8 rule) ────────────────────────────
    //
    // Network sandbox: a page's reach is its view's NetLevel, the owning mod's
    // NetworkPolicy (File unless the mod opted in). Tightening a default later
    // breaks mods, which is why the default is the tightest level.
    // "http://127.0.0.1:8080/x", "http://localhost/x", "http://[::1]:1/x" -> true.
    // 127.0.0.0/8 as a DOTTED QUAD only: a "127." prefix test let the legal
    // hostname 127.attacker.example through the sandbox (review 2026-09-09).

    static bool IsLoopbackV4(const std::string& host)
    {
        int octets = 0, value = 0, digits = 0;
        for (std::size_t i = 0; i <= host.size(); ++i) {
            const char c = i < host.size() ? host[i] : '.';
            if (c >= '0' && c <= '9') { if (++digits > 3) return false; value = value * 10 + (c - '0'); continue; }
            if (c != '.' || digits == 0 || value > 255) return false;
            if (octets == 0 && value != 127) return false;
            ++octets; value = 0; digits = 0;
        }
        return octets == 4;
    }

    static bool IsLoopbackUrl(const std::string& url)
    {
        const auto p = url.find("://");
        if (p == std::string::npos) return false;
        std::string host = url.substr(p + 3);
        const auto end = host.find_first_of("/?#");
        if (end != std::string::npos) host.resize(end);
        if (const auto at = host.rfind('@'); at != std::string::npos) host.erase(0, at + 1);
        if (!host.empty() && host[0] == '[') {
            const auto rb = host.find(']');
            host = (rb == std::string::npos) ? host : host.substr(0, rb + 1);
        } else if (const auto c = host.rfind(':'); c != std::string::npos) {
            host.resize(c);
        }
        for (auto& ch : host) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return host == "localhost" || host == "[::1]" || IsLoopbackV4(host);
    }

    // ── MlNetworkListener: the network sandbox ─────────────────────────────
    //
    // OnNetworkRequest runs on a WebKit thread: it reads the view registry
    // only under s_viewsMutex.

    class MlNetworkListener final : public ultralight::NetworkListener {
    public:
        bool OnNetworkRequest(ultralight::View* caller, ultralight::NetworkRequest& req) override
        {
            const std::string proto = req.urlProtocol().utf8().data();
            if (proto == "data" || proto == "about" || proto == "blob") return true;
            if (proto == "file") {
                // Host pinning: a page reads its OWN mod folder and the host
                // runtime dir (resources, cursor, host pages), nothing else —
                // no other mod's folder, no traversal out of Data, no
                // arbitrary drive path. The idea is MeridianUI's mod:// scheme;
                // Ultralight has no custom schemes, so the pin lives on the
                // request instead of the URL (0.26.4).
                std::filesystem::path root;
                ViewId id = 0;
                {
                    std::lock_guard<std::mutex> lk(s_viewsMutex);
                    if (MlView* v = FindViewByUlLocked(caller)) { root = v->root; id = v->id; }
                }
                const std::filesystem::path target = ResolveFileUrl(req.url().utf8().data());
                const bool ok = !target.empty() &&
                    (PathIsUnder(target, s_runtimeDir) || (!root.empty() && PathIsUnder(target, root)));
                if (ok) return true;
                static std::atomic<int> s_pinLogged{ 0 };
                if (s_pinLogged.fetch_add(1) < 32) {
                    SKSE::log::warn("Magelight: view {} refused file read outside its folder — {} (root '{}')",
                        id, target.string(), root.string());
                }
                return false;
            }
            // Beyond files, the owning mod decides (SetNetworkPolicy or the
            // manifest's "network"): Loopback lets a page talk to a server on
            // this machine, which can itself relay anywhere, so it is an
            // opt-in too; Any is for pages that talk to the internet, where a
            // host allowlist would break with every new host.
            NetLevel level = NetLevel::File;
            ViewId id = 0;
            {
                std::lock_guard<std::mutex> lk(s_viewsMutex);
                if (const MlView* v = FindViewByUlLocked(caller)) { level = v->netLevel; id = v->id; }
            }
            if (level == NetLevel::Any) return true;
            if (level == NetLevel::Loopback && (proto == "http" || proto == "https") &&
                IsLoopbackUrl(req.url().utf8().data()))
                return true;
            static std::atomic<int> s_logged{ 0 };
            if (s_logged.fetch_add(1) < 32) {
                SKSE::log::warn("Magelight: view {} blocked network request {} {} (protocol '{}') — its mod's network policy is {}",
                    id, req.httpMethod().utf8().data(), req.url().utf8().data(), proto,
                    level == NetLevel::Loopback ? "loopback (http(s) to this machine only)" : "file only");
            }
            return false;
        }
    };

    // ── MlFileSystem: our own, never AppCore's (invariant 5) ───────────────

    class MlFileSystem final : public ultralight::FileSystem {
    public:
        // Minimal percent-decode: WebKit encodes spaces (and friends) in
        // file:/// URLs, and game installs live under paths like
        // "Program Files (x86)".
        static std::string UrlDecode(const std::string& in)
        {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            std::string out;
            out.reserve(in.size());
            for (std::size_t i = 0; i < in.size(); ++i) {
                if (in[i] == '%' && i + 2 < in.size()) {
                    const int hi = hex(in[i + 1]), lo = hex(in[i + 2]);
                    if (hi >= 0 && lo >= 0) {
                        out += static_cast<char>((hi << 4) | lo);
                        i += 2;
                        continue;
                    }
                }
                out += in[i];
            }
            return out;
        }

        // file:///-URL → filesystem path. Beyond the demo's needs, the SA
        // port adds three shapes: QUERY STRINGS (?v=N cache-busters on
        // images the native side rewrites in place — stat/open must ignore
        // them), percent-encoding, and ABSOLUTE paths (SA's HTML ships in
        // SA's own mod folder; a drive-letter path resolves as-is instead
        // of against our runtime dir).
        std::filesystem::path Resolve(const ultralight::String& file_path)
        {
            std::string p = file_path.utf8().data();
            if (const auto q = p.find('?'); q != std::string::npos) p.resize(q);
            if (const auto h = p.find('#'); h != std::string::npos) p.resize(h);
            if (p.rfind("file:///", 0) == 0) p = p.substr(8);
            p = UrlDecode(p);
            while (!p.empty() && (p.front() == '/' || p.front() == '\\')) p.erase(p.begin());
            std::filesystem::path out = (p.size() > 1 && p[1] == ':')
                ? std::filesystem::path(p).make_preferred()
                : (s_runtimeDir / std::filesystem::path(p).make_preferred());
            // Defence in depth under the per-view pin: nothing outside the
            // host runtime dir, Data\Magelight or a registered page's own
            // root is ever served, and ".." is normalised away before the
            // test so it cannot climb out. This layer is view-blind (WebKit
            // asks for files from a worker thread with no View in hand), so
            // it accepts the UNION of page roots — the per-view pin in
            // MlNetworkListener is what keeps one page out of another's
            // folder. 0.28.1: the union used to be just Data\Magelight,
            // which refused every absolute page living elsewhere even though
            // the pin above had already accepted it — SkyrimNet's dashboard
            // under a folder outside Data\Magelight loaded as an empty view.
            out = out.lexically_normal();
            static const std::filesystem::path s_modsRoot = (GameRoot() / L"Data" / L"Magelight").lexically_normal();
            if (PathIsUnder(out, s_runtimeDir) || PathIsUnder(out, s_modsRoot) || IsUnderRegisteredPageRoot(out)) return out;
            static std::atomic<int> s_logged{ 0 };
            if (s_logged.fetch_add(1) < 32)
                SKSE::log::warn("Magelight: file read outside Data\\Magelight or any page root refused — {}", out.string());
            return {};
        }
        bool FileExists(const ultralight::String& file_path) override
        {
            const auto p = Resolve(file_path);
            if (p.empty()) return false;
            std::error_code ec;
            return std::filesystem::is_regular_file(p, ec);
        }
        ultralight::String GetFileMimeType(const ultralight::String& file_path) override
        {
            std::string ext = Resolve(file_path).extension().string();
            for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext == ".html" || ext == ".htm") return "text/html";
            if (ext == ".js" || ext == ".mjs") return "application/javascript";
            if (ext == ".css") return "text/css";
            if (ext == ".json" || ext == ".map") return "application/json";
            if (ext == ".svg") return "image/svg+xml";
            if (ext == ".png") return "image/png";
            if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
            if (ext == ".gif") return "image/gif";
            if (ext == ".webp") return "image/webp";
            if (ext == ".ico") return "image/x-icon";
            if (ext == ".woff") return "font/woff";
            if (ext == ".woff2") return "font/woff2";
            if (ext == ".ttf") return "font/ttf";
            if (ext == ".otf") return "font/otf";
            if (ext == ".txt") return "text/plain";
            if (ext == ".wasm") return "application/wasm";
            return "application/octet-stream";
        }
        ultralight::String GetFileCharset(const ultralight::String&) override
        {
            return "utf-8";
        }
        ultralight::RefPtr<ultralight::Buffer> OpenFile(const ultralight::String& file_path) override
        {
            const auto path = Resolve(file_path);
            if (path.empty()) return nullptr;   // refused by the pin (already logged)
            std::ifstream f(path, std::ios::binary | std::ios::ate);
            if (!f) {
                SKSE::log::warn("Magelight: OpenFile failed: {}", path.string());
                return nullptr;
            }
            const std::streamsize size = f.tellg();
            f.seekg(0, std::ios::beg);
            std::vector<char> data(static_cast<size_t>(size));
            if (size > 0 && !f.read(data.data(), size)) {
                SKSE::log::warn("Magelight: OpenFile read failed: {}", path.string());
                return nullptr;
            }
            return ultralight::Buffer::CreateFromCopy(data.data(), data.size());
        }
    };

    // ── The singletons (stable addresses — Platform::instance() holds them) ─

    static SpdLogger         s_logger;
    static MlClipboard       s_clipboard;
    static MlNetworkListener s_networkListener;
    static MlFileSystem      s_fileSystem;

    ultralight::Logger&          UlLogger()          { return s_logger; }
    ultralight::Clipboard&       UlClipboard()       { return s_clipboard; }
    ultralight::FileSystem&      UlFileSystem()      { return s_fileSystem; }
    ultralight::NetworkListener& UlNetworkListener() { return s_networkListener; }

    // ── Page roots outside Data\Magelight ──────────────────────────────────
    //
    // A page's absolute path pins to its own directory (PageRootFor).
    // Append-only and never pruned: a root is a directory, and keeping a
    // destroyed view's folder readable costs nothing, whereas pruning would
    // race a worker-thread read against the destroy. Own mutex, not
    // s_viewsMutex: MlFileSystem::Resolve runs on WebKit's worker threads
    // while the game thread may hold s_viewsMutex.

    static std::mutex s_pageRootsMutex;
    static std::vector<std::filesystem::path> s_pageRoots;

    void NotePageRoot(const std::filesystem::path& root)
    {
        if (root.empty()) return;
        std::lock_guard<std::mutex> lk(s_pageRootsMutex);
        for (const auto& r : s_pageRoots) if (r == root) return;
        s_pageRoots.push_back(root);
    }

    bool IsUnderRegisteredPageRoot(const std::filesystem::path& p)
    {
        std::lock_guard<std::mutex> lk(s_pageRootsMutex);
        for (const auto& r : s_pageRoots) if (PathIsUnder(p, r)) return true;
        return false;
    }

    // ── Path primitives ─────────────────────────────────────────────────────

    std::filesystem::path ResolveFileUrl(const std::string& url)
    {
        return s_fileSystem.Resolve(ultralight::String(url.c_str()));
    }

    // Case-insensitive prefix test on normalised native paths (Windows). Pure
    // string work — safe on WebKit's thread, no filesystem calls.
    bool PathIsUnder(const std::filesystem::path& p, const std::filesystem::path& base)
    {
        if (p.empty() || base.empty()) return false;
        std::wstring a = p.lexically_normal().native();
        std::wstring b = base.lexically_normal().native();
        while (!b.empty() && (b.back() == L'\\' || b.back() == L'/')) b.pop_back();
        if (a.size() < b.size()) return false;
        for (std::size_t i = 0; i < b.size(); ++i)
            if (std::towlower(a[i]) != std::towlower(b[i])) return false;
        return a.size() == b.size() || a[b.size()] == L'\\' || a[b.size()] == L'/';
    }

    std::filesystem::path PageRootFor(const std::string& htmlPath)
    {
        const bool absolute = htmlPath.size() > 1 && htmlPath[1] == ':';
        if (!absolute) return s_runtimeDir;
        const std::filesystem::path page = std::filesystem::path(htmlPath).lexically_normal();
        const std::filesystem::path modsRoot = (GameRoot() / L"Data" / L"Magelight").lexically_normal();
        if (PathIsUnder(page, modsRoot)) {
            // modsRoot / <Mod> — the first component below Data\Magelight
            const std::filesystem::path rel = page.lexically_relative(modsRoot);
            auto it = rel.begin();
            if (it != rel.end()) return modsRoot / *it;
        }
        return page.parent_path();
    }

}  // namespace Magelight