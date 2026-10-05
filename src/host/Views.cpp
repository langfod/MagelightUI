// The host's view and image registries — stage 4 of the split. See Views.h for
// what these are and the three rules that keep them correct.

#include "host/Views.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>

#include <SKSE/SKSE.h>      // SKSE::log

#include "host/State.h"     // s_runtimeDir, the FileSystem root ResolveViewFile falls back to

namespace Magelight {

    // ── s_views ─────────────────────────────────────────────────────────────

    std::mutex s_viewsMutex;
    std::vector<std::unique_ptr<MlView>> s_views;  // z-order = (layer, order), see SortViewsLocked
    std::atomic<std::uint64_t> s_nextViewId{ 1 };
    std::atomic<std::uint64_t> s_toggleView{ 0 };
    std::atomic<std::uint64_t> s_nextViewOrder{ 1 };
    std::atomic<std::uint64_t> s_inspectorRequest{ 0 };   // inspector toggle request: page (or inspector) view id; render thread consumes (no key bound yet)
    std::map<ViewId, ViewId> s_inspectorOf;               // page -> hosted inspector view (render thread only)
    std::atomic<std::uint64_t> s_inspectorVisibleFor{ 0 };   // page whose inspector is showing (0 = none); any thread reads
    std::atomic<std::uint64_t> s_inspectorHideRequest{ 0 }; // 0.28.0: page whose inspector should hide; render thread
    std::atomic<HostEventFn> s_hostEventSink{ nullptr };

    void Emit(HostEvent type, ViewId view, int x, int y, const char* detail)
    {
        if (HostEventFn fn = s_hostEventSink.load()) fn(type, view, x, y, detail ? detail : "");
    }

    // s_viewsMutex must be held. Keeps the registry in z-order.
    void SortViewsLocked()
    {
        std::stable_sort(s_views.begin(), s_views.end(),
            [](const std::unique_ptr<MlView>& a, const std::unique_ptr<MlView>& b) {
                return a->layer != b->layer ? a->layer < b->layer : a->order < b->order;
            });
    }

    // s_viewsMutex must be held.
    MlView* FindViewLocked(ViewId id)
    {
        for (auto& v : s_views)
            if (v->id == id) return v.get();
        return nullptr;
    }

    MlView* FindViewByUlLocked(ultralight::View* ul)
    {
        for (auto& v : s_views)
            if (v->ul.get() == ul) return v.get();
        return nullptr;
    }

    // True when any view wants pixels this frame.
    bool AnyViewVisible()
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        for (const auto& v : s_views)
            if (v->visible) return true;
        return false;
    }

    // A visible view the player can click: the UI-pass carrier opens only for
    // one of these (see MagelightOverlayMenu), so a HUD view that stays up
    // never holds it open over gameplay.
    bool AnyInteractiveViewVisible()
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        for (const auto& v : s_views)
            if (v->visible && !v->clickThrough) return true;
        return false;
    }

    // Resolve the anchored position against the current backbuffer size.
    void EffectivePos(const MlView& v, float bw, float bh, float& outX, float& outY)
    {
        outX = (v.x >= 0) ? static_cast<float>(v.x) : bw + static_cast<float>(v.x);
        outY = (v.y >= 0) ? static_cast<float>(v.y) : bh + static_cast<float>(v.y);
    }

    // ── s_images ────────────────────────────────────────────────────────────

    std::mutex s_imagesMutex;
    std::vector<std::unique_ptr<MlImage>> s_images;
    std::atomic<std::uint32_t> s_nextImageId{ 1 };

    MlImage* FindImageLocked(ImageId id)
    {
        for (auto& i : s_images)
            if (i->id == id) return i.get();
        return nullptr;
    }

    // ── String helpers ──────────────────────────────────────────────────────

    std::string JSStringToStd(JSStringRef s)
    {
        if (!s) return {};
        const size_t cap = JSStringGetMaximumUTF8CStringSize(s);
        std::string out(cap, '\0');
        const size_t n = JSStringGetUTF8CString(s, out.data(), cap);
        out.resize(n ? n - 1 : 0);
        return out;
    }

    std::string JsonEscape(const std::string& s)
    {
        std::string out;
        out.reserve(s.size() + 8);
        for (const char c : s) {
            switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04X", c);
                    out += buf;
                } else {
                    out += c;
                }
            }
        }
        return out;
    }

    // Quotes for embedding in JS source. Delegates to the inline JsonQuote in
    // Magelight.h, which is the ONE escaper: the v4 surface (MagelightApi4)
    // and the public App API include that header and get the same function, so
    // a string quoted here and a string quoted there cannot disagree about
    // what needs escaping.
    std::string MlJsonQuote(const std::string& in)
    {
        return JsonQuote(in);
    }

    // ── Log line capping (0.31.0) ───────────────────────────────────────────
    //
    // A page's console text is unbounded (a minified bundle logs a whole source
    // line, an object literal can be megabytes). Cutting on a UTF-8 CHARACTER
    // boundary, not a byte boundary: a cut mid-sequence would put a partial
    // code point in the log, and the log is read by humans and by tools.
    std::string CapLogText(const std::string& in, std::size_t maxBytes)
    {
        if (in.size() <= maxBytes) return in;
        std::size_t cut = maxBytes;
        while (cut > 0 && (static_cast<unsigned char>(in[cut]) & 0xC0) == 0x80) --cut;
        return in.substr(0, cut) + "\xE2\x80\xA6 [" + std::to_string(in.size()) + " bytes]";
    }

    double s_lastUlUpdateMs = 0.0;   // the previous frame's Renderer::Update, ms (written by FrameWork)

    void NoteLoadNow(ViewId view)
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) v->loadNow = true;
    }

    // ── Per-view cursors (0.31.0 / 0.31.1) ──────────────────────────────────
    //
    // Two layers per view: its own set over its mod's default. Images are shared
    // by (file, hotspot, height, press) through s_cursorImages, which holds WEAK
    // references: a set that goes away lets the image be collected. The host's
    // own cursor texture is NOT in here (that is s_cursorTex in Draw.cpp), so the
    // cache can never alias the one the engine drives.

    std::mutex s_cursorImagesMutex;
    std::map<std::string, std::weak_ptr<CursorImage>> s_cursorImages;

    // Human-readable form of a set, for the log lines below. Page-supplied paths,
    // so they only ever reach the log — never a JS-visible string.
    std::string DescribeCursorSet(const CursorSet& set)
    {
        if (set.none) return "none (the page draws its own)";
        std::string out;
        for (int i = 0; i < kCursorStates; ++i) {
            const auto& img = set.image[i];
            if (!img) continue;
            if (!out.empty()) out += ", ";
            out += (i == 0 ? "arrow=" : (i == 1 ? "pointer=" : "text="));
            out += img->file.filename().string();
        }
        return out.empty() ? std::string("empty") : out;
    }

    // Validate and cache; DO NOT decode. The render thread decodes the first time
    // an image is actually drawn (D3D11 and WIC are render-thread-only here), so a
    // page's SetCursor never touches the device.
    std::shared_ptr<CursorImage> MakeCursorImage(const std::filesystem::path& file, float hotX, float hotY,
                                                 float height, bool press, std::string* why)
    {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec)) {
            if (why) *why = "no file at " + file.string();
            return nullptr;
        }
        const auto finite = [](float v) { return v == v && v > -1e30f && v < 1e30f; };
        hotX = finite(hotX) ? std::clamp(hotX, 0.0f, static_cast<float>(kViewCursorMaxPx)) : 0.0f;
        hotY = finite(hotY) ? std::clamp(hotY, 0.0f, static_cast<float>(kViewCursorMaxPx)) : 0.0f;
        height = (finite(height) && height > 0.0f) ? std::clamp(height, 8.0f, 256.0f) : 0.0f;
        const std::filesystem::path norm = file.lexically_normal();
        std::string key = norm.string();
        for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        key += "|" + std::to_string(hotX) + "|" + std::to_string(hotY) + "|" + std::to_string(height) + (press ? "|p" : "|-");
        std::lock_guard<std::mutex> lk(s_cursorImagesMutex);
        if (auto it = s_cursorImages.find(key); it != s_cursorImages.end()) {
            if (auto live = it->second.lock()) return live;
        }
        for (auto it = s_cursorImages.begin(); it != s_cursorImages.end();)
            it = it->second.expired() ? s_cursorImages.erase(it) : std::next(it);
        auto img = std::make_shared<CursorImage>();
        img->file = norm;
        img->hotX = hotX;
        img->hotY = hotY;
        img->height = height;
        img->press = press;
        s_cursorImages[key] = img;
        return img;
    }

    // `path` against the view's mod folder. The view lookup is its own short
    // hold; the resolve happens after it is released, so s_viewsMutex is never
    // held across file IO.
    bool ResolveViewFile(ViewId view, const std::string& path, bool absoluteOk, std::filesystem::path& out,
                         std::string* why)
    {
        const auto fail = [why](const char* w) { if (why) *why = w; return false; };
        if (path.empty() || path.size() > 260) return fail("the path is empty or longer than 260 characters");
        const bool absolute = path.size() > 1 && path[1] == ':';
        if (absolute) {
            if (!absoluteOk) return fail("the path must be relative to the mod folder");
            out = std::filesystem::path(path).lexically_normal();
            return true;
        }
        if (path[0] == '/' || path[0] == '\\' || path.find("..") != std::string::npos)
            return fail("the path must stay inside the mod folder (relative, no '..')");
        std::filesystem::path root;
        {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            const MlView* v = FindViewLocked(view);
            if (!v) return fail("the view is gone");
            root = v->root;
        }
        if (root.empty()) root = s_runtimeDir;
        out = (root / std::filesystem::path(path).make_preferred()).lexically_normal();
        return true;
    }

    bool SetViewCursorSet(ViewId view, bool modDefault, const CursorSet& set)
    {
        {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            MlView* v = FindViewLocked(view);
            if (!v || v->destroyPending) return false;
            (modDefault ? v->cursorMod : v->cursorOwn) = set;
        }
        SKSE::log::info("Magelight: view {} {}cursor: {}", view, modDefault ? "mod default " : "", DescribeCursorSet(set));
        return true;
    }

    bool SetViewCursorState(ViewId view, int state, std::shared_ptr<CursorImage> image)
    {
        if (state < 0 || state >= kCursorStates) return false;
        std::string desc;
        {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            MlView* v = FindViewLocked(view);
            if (!v || v->destroyPending) return false;
            if (image || state == 0) v->cursorOwn.none = false;   // "none" is set through the arrow state
            v->cursorOwn.image[state] = std::move(image);
            desc = DescribeCursorSet(v->cursorOwn);
        }
        SKSE::log::info("Magelight: view {} cursor: {}", view, desc);
        return true;
    }

    bool SetViewCursorNone(ViewId view)
    {
        CursorSet none;
        none.none = true;
        return SetViewCursorSet(view, false, none);
    }

    bool SetViewCursorTint(ViewId view, const CursorTintSet& tint)
    {
        {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            MlView* v = FindViewLocked(view);
            if (!v || v->destroyPending) return false;
            v->cursorTint = tint;
        }
        if (tint.Empty())
            SKSE::log::info("Magelight: view {} cursor tint: none (the host's colours)", view);
        else
            SKSE::log::info("Magelight: view {} cursor tint: lit {:08X} shade {:08X} ink {:08X} glow {:08X} ibeam {:08X}",
                            view, tint.lit, tint.shade, tint.ink, tint.glow, tint.ibeam);
        return true;
    }

}  // namespace Magelight