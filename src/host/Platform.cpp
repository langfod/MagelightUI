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
#include <cctype>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <mutex>
#include <string>
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

    // MG: 2.0 replaced the ReadPlainText/WritePlainText pair with a payload model —
    // Read() returns and Write() takes a ClipboardData (an ordered set of typed
    // entries). The library only uses text/plain, text/html, and text/uri-list; we
    // map text/plain to CF_UNICODETEXT and skip the rest, which is what the docs
    // bless ("a backend that only supports text is fine").
    class MlClipboard final : public ultralight::Clipboard {
    public:
        void Clear() override
        {
            if (!OpenClipboard(s_hwnd)) return;
            EmptyClipboard();
            CloseClipboard();
        }

        // Paste source. Returns a text/plain payload, or null for an empty clipboard.
        ultralight::RefPtr<ultralight::ClipboardData> Read() override
        {
            auto data = ultralight::ClipboardData::Create();
            if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(s_hwnd)) return data;
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                if (auto* w = static_cast<const wchar_t*>(GlobalLock(h))) {
                    const std::size_t len = wcsnlen(w, GlobalSize(h) / sizeof(wchar_t));
                    ultralight::String16 s16(reinterpret_cast<const ultralight::Char16*>(w), len);
                    data->Set("text/plain", ultralight::ClipboardValue(ultralight::String(s16)));
                    GlobalUnlock(h);
                }
            }
            CloseClipboard();
            return data;
        }

        // Copy source. Writes every text entry in one atomic clipboard transaction,
        // mapping text/plain to CF_UNICODETEXT and skipping types we don't support.
        void Write(ultralight::RefPtr<ultralight::ClipboardData> data) override
        {
            if (!data || !OpenClipboard(s_hwnd)) return;
            EmptyClipboard();
            for (std::size_t i = 0, n = data->size(); i < n; ++i) {
                const ultralight::String type = data->type_at(i);
                if (type != "text/plain") continue;  // text/html, text/uri-list: not mapped.
                const auto value = data->Get(type);
                if (!value.is_text()) continue;
                const ultralight::String16 s16 = value.text().utf16();
                const std::size_t len = s16.length();
                HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (len + 1) * sizeof(wchar_t));
                if (!h) continue;
                auto* dst = static_cast<wchar_t*>(GlobalLock(h));
                std::memcpy(dst, s16.data(), len * sizeof(wchar_t));
                dst[len] = L'\0';
                GlobalUnlock(h);
                if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
            }
            CloseClipboard();
        }
    };

    // ── Loopback parsing (the 127.0.0.0/8 rule) ────────────────────────────
    //
    // Network sandbox: a page's reach is its view's NetLevel, the owning mod's
    // NetworkPolicy (File unless the mod opted in). Tightening a default later
    // breaks mods, which is why the default is the tightest level.
    // The loopback hosts are the two the page's CSP (CspFor) admits: an exact
    // host match, never a prefix test (127.attacker.example is a legal name).
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
        return host == "localhost" || host == "127.0.0.1";
    }

    // ── MlNetworkListener: the network gate behind the page's CSP (CspFor) ───
    //
    // Ultralight calls OnNetworkRequest for http(s) loads only, never for file:,
    // data:, synchronous XHR or WebSocket. It runs on a WebKit thread and reads
    // the view registry only under s_viewsMutex.

    class MlNetworkListener final : public ultralight::NetworkListener {
    public:
        bool OnNetworkRequest(ultralight::View* caller, ultralight::NetworkRequest& req) override
        {
            const std::string proto = req.urlProtocol().utf8().data();
            // The CSP is the companion to this gate for http(s): Ultralight calls it for
            // http(s) loads only, never for file:, data:, synchronous XHR or WebSocket — those
            // are MlFileSystem's (file:) and the policy's (the rest). So file: passes here.
            if (proto == "data" || proto == "about" || proto == "blob" || proto == "file") return true;
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
    static NetLevel RootNetLevel(const std::filesystem::path& p);   // namespace-scope; defined after the class

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
        static std::string LowerExt(const std::filesystem::path& p)
        {
            std::string ext = p.extension().string();
            for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return ext;
        }
        static bool IsHtmlExt(const std::string& ext) { return ext == ".html" || ext == ".htm"; }

        ultralight::RefPtr<ultralight::Buffer> OpenFile(const ultralight::String& file_path) override
        {
            const auto path = Resolve(file_path);
            if (path.empty()) return nullptr;   // refused by the file gate (already logged)
            std::ifstream f(path, std::ios::binary | std::ios::ate);
            if (!f) {
                SKSE::log::warn("Magelight: OpenFile failed: {}", path.string());
                return nullptr;
            }
            const std::streamsize size = f.tellg();
            f.seekg(0, std::ios::beg);
            std::string data(static_cast<size_t>(size), '\0');
            if (size > 0 && !f.read(data.data(), size)) {
                SKSE::log::warn("Magelight: OpenFile read failed: {}", path.string());
                return nullptr;
            }
            // Every page gets its mod's CSP (CspFor). The hosted Web Inspector is left as shipped:
            // it is a dev surface served from the runtime dir, not written against a policy.
            if (IsHtmlExt(LowerExt(path)) && !PathIsUnder(path, s_runtimeDir / L"inspector"))
                InjectCsp(data, RootNetLevel(path));
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

    // Each root carries the network level of the views that load from it: the
    // page's CSP is chosen by the FILE's root, since the file system cannot
    // tell which view asked. The last view created or policy set for a root
    // wins (a mod's views all share its level). The runtime dir is never
    // noted: host pages stay file-only whatever a relative-path view asks.
    struct PageRoot { std::filesystem::path path; NetLevel net = NetLevel::File; };
    static std::mutex s_pageRootsMutex;
    static std::vector<PageRoot> s_pageRoots;

    void NotePageRoot(const std::filesystem::path& root, NetLevel net)
    {
        if (root.empty() || root == s_runtimeDir) return;
        std::lock_guard<std::mutex> lk(s_pageRootsMutex);
        for (auto& r : s_pageRoots) if (r.path == root) { r.net = net; return; }
        s_pageRoots.push_back({ root, net });
    }

    bool IsUnderRegisteredPageRoot(const std::filesystem::path& p)
    {
        std::lock_guard<std::mutex> lk(s_pageRootsMutex);
        for (const auto& r : s_pageRoots) if (PathIsUnder(p, r.path)) return true;
        return false;
    }

    // The level a page at `p` gets: the deepest registered root containing it
    // (a mod folder inside the mods root beats the mods root itself); file-only
    // when no root contains it.
    static NetLevel RootNetLevel(const std::filesystem::path& p)
    {
        std::lock_guard<std::mutex> lk(s_pageRootsMutex);
        NetLevel level = NetLevel::File;
        std::size_t best = 0;
        for (const auto& r : s_pageRoots) {
            const std::size_t len = r.path.native().size();
            if (len >= best && PathIsUnder(p, r.path)) { best = len; level = r.net; }
        }
        return level;
    }

    // The Content-Security-Policy MlFileSystem writes into every page, by its mod's NetworkPolicy.
    // Ultralight routes file:, data:, synchronous XHR and WebSocket requests past NetworkListener,
    // so the listener alone let a File page open a WebSocket, and a document's synchronous XHR read
    // any file on disk; the policy is what closes them. Rules the probe matrix settled:
    //  - connect-src never lists file: — a page cannot fetch its own files (Vite's modulepreload
    //    polyfill logs one refusal; @magelight/vite-plugin turns the polyfill off), and the sync-XHR
    //    disk read is gone.
    //  - frame-src never lists file: or http(s): every file:/// document shares one origin, so a
    //    frame of a looser mod's page would lend that page its policy; object-src 'none' closes the
    //    <object> route.
    //  - Loopback is http(s)/ws(s) to localhost and 127.0.0.1 only: CSP cannot express 127.0.0.0/8
    //    or [::1].
    //  - 'unsafe-inline' and 'unsafe-eval' stay: mod pages carry inline scripts.
    static std::string CspFor(NetLevel level)
    {
        switch (level) {
        case NetLevel::Loopback:
            return "default-src file: data: blob: 'unsafe-inline' 'unsafe-eval'"
                   " http://localhost:* http://127.0.0.1:* https://localhost:* https://127.0.0.1:*;"
                   " connect-src data: blob: http://localhost:* http://127.0.0.1:* https://localhost:* https://127.0.0.1:*"
                   " ws://localhost:* ws://127.0.0.1:* wss://localhost:* wss://127.0.0.1:*;"
                   " frame-src data:; form-action file: http://localhost:* http://127.0.0.1:* https://localhost:* https://127.0.0.1:*;"
                   " object-src 'none'";
        case NetLevel::Any:
            return "default-src * file: data: blob: 'unsafe-inline' 'unsafe-eval';"
                   " connect-src http: https: ws: wss: data: blob:; frame-src * data:; form-action *; object-src 'none'";
        default:
            return "default-src file: data: blob: 'unsafe-inline' 'unsafe-eval'; connect-src data: blob:;"
                   " frame-src data:; form-action file:; object-src 'none'";
        }
    }
    static std::string CspMetaFor(NetLevel level)
    {
        return "<meta http-equiv=\"Content-Security-Policy\" content=\"" + CspFor(level) + ">";
    }
    // Writes the policy into an HTML document right after its BOM, leading whitespace, comments,
    // XML prolog and doctype: no page byte that can run or load anything comes before it, and the
    // doctype still decides the mode (a tag ahead of the doctype would put the page in quirks mode).
    // Without a doctype it goes after the <html> start tag, or at the top of a document with none.
    void InjectCsp(std::string& html, NetLevel level)
    {
        const auto startsWith = [&](std::size_t at, const char* tok) {
            const std::size_t n = std::strlen(tok);
            return html.size() - at >= n && _strnicmp(html.data() + at, tok, n) == 0;
        };
        std::size_t at = 0;
        if (startsWith(0, "\xEF\xBB\xBF")) at = 3;
        for (;;) {
            while (at < html.size() && std::isspace(static_cast<unsigned char>(html[at]))) ++at;
            if (startsWith(at, "<!--")) {
                const std::size_t end = html.find("-->", at + 4);
                if (end == std::string::npos) break;
                at = end + 3;
            } else if (startsWith(at, "<?")) {
                const std::size_t end = html.find("?>", at + 2);
                if (end == std::string::npos) break;
                at = end + 2;
            } else {
                break;
            }
        }
        if (startsWith(at, "<!doctype")) {
            const std::size_t gt = html.find('>', at);
            if (gt != std::string::npos) at = gt + 1;
        } else if (startsWith(at, "<html")) {
            const std::size_t gt = html.find('>', at);
            if (gt != std::string::npos) at = gt + 1;
        }
        html.insert(at, CspMetaFor(level));
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

    // ── Text conversion (UTF-8 <-> UTF-16) ─────────────────────────────────
    // Declared in host/Platform.h, which states why these are non-throwing.

    bool TryUtf8ToWide(std::string_view sv, std::wstring& out)
    {
        if (sv.empty()) {
            out.clear();
            return true;
        }
        const int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            sv.data(), static_cast<int>(sv.size()), nullptr, 0);
        if (len == 0) return false;
        std::wstring ws(static_cast<std::size_t>(len), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                sv.data(), static_cast<int>(sv.size()), ws.data(), len) == 0)
            return false;
        out = std::move(ws);
        return true;
    }

    bool TryWideToUtf8(std::wstring_view wv, std::string& out)
    {
        if (wv.empty()) {
            out.clear();
            return true;
        }
        const int len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            wv.data(), static_cast<int>(wv.size()), nullptr, 0, nullptr, nullptr);
        if (len == 0) return false;
        std::string utf8(static_cast<std::size_t>(len), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                wv.data(), static_cast<int>(wv.size()), utf8.data(), len, nullptr, nullptr) == 0)
            return false;
        out = std::move(utf8);
        return true;
    }

    std::string WideToUtf8Lossy(std::wstring_view wv)
    {
        std::string out;
        if (TryWideToUtf8(wv, out)) return out;
        // Same conversion without WC_ERR_INVALID_CHARS: an unpaired surrogate
        // becomes U+FFFD, and the result is still well-formed UTF-8. This is
        // the IME's case — composition text legitimately carries a half-formed
        // surrogate while the user is mid-word, and raising there would turn a
        // cosmetic glitch into a crash on the window proc.
        if (wv.empty()) return {};
        const int len = WideCharToMultiByte(CP_UTF8, 0,
            wv.data(), static_cast<int>(wv.size()), nullptr, 0, nullptr, nullptr);
        if (len <= 0) return {};
        std::string utf8(static_cast<std::size_t>(len), '\0');
        if (WideCharToMultiByte(CP_UTF8, 0, wv.data(), static_cast<int>(wv.size()),
                utf8.data(), len, nullptr, nullptr) == 0)
            return {};
        return utf8;
    }

    std::string WideToUtf8(std::wstring_view wv) { return WideToUtf8Lossy(wv); }

    std::wstring Utf8ToWide(std::string_view sv)
    {
        std::wstring out;
        // Lossy via the replacement character: a page's UTF-8 is already
        // well-formed by the time WebKit hands it over, so the strict path is
        // the normal one and this only covers a hand-built path or a
        // mis-decoded registry value.
        if (TryUtf8ToWide(sv, out)) return out;
        if (sv.empty()) return {};
        const int len = MultiByteToWideChar(CP_UTF8, 0,
            sv.data(), static_cast<int>(sv.size()), nullptr, 0);
        if (len <= 0) return {};
        std::wstring ws(static_cast<std::size_t>(len), L'\0');
        if (MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()),
                ws.data(), len) == 0)
            return {};
        return ws;
    }

    // ── Path -> file:/// URL ───────────────────────────────────────────────
    // Forward slashes + the minimal percent set (space/%/#/?) so game-root
    // paths like "Program Files (x86)" survive WebKit's URL parser.
    // INVERSE of MlFileSystem::UrlDecode above, and it lives beside it for
    // exactly that reason: the encode half in the registry and the decode half
    // in the platform layer is how the pair got out of step once already.

    std::string PathToFileUrl(const std::filesystem::path& p)
    {
        std::string s = p.string();
        for (auto& c : s)
            if (c == '\\') c = '/';
        std::string out = "file:///";
        for (const char c : s) {
            switch (c) {
            case ' ': out += "%20"; break;
            case '%': out += "%25"; break;
            case '#': out += "%23"; break;
            case '?': out += "%3F"; break;
            default:  out += c;
            }
        }
        return out;
    }

}  // namespace Magelight