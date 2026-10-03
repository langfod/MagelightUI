// The host's view and image registries — stage 4 of the split. See Views.h for
// what these are and the three rules that keep them correct.

#include "host/Views.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

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

    // Quotes for embedding in JS source. Shared with the Api4 layer, which has
    // its own copy in Magelight.h — these two must agree, so both delegate to
    // the one inline there rather than carrying separate escapes.
    std::string MlJsonQuote(const std::string& in)
    {
        return JsonQuote(in);
    }

    // Absolute path → file:/// URL: forward slashes + the minimal percent set
    // (space/%/#/?) so game-root paths like Program Files (x86) survive
    // WebKit's URL parser. Inverse of MlFileSystem::UrlDecode.
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