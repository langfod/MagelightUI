// Page bridge - the JS <-> C++ side of a view (stage 4.5).
//
// One native dispatcher (__mlNative) is exposed to each page; per-listener
// window shims forward (viewId, name, arg) to it, installed at
// OnWindowObjectReady BEFORE the page's own scripts run - the contract
// SeverActions' frontend assumes. C++ -> JS is the other direction:
// InteropCall queues {fn, arg} on the view and the render thread drains it
// per frame, calling window.fn(arg) through JavaScriptCore with the payload
// as a REAL string argument (no JS-source inlining).
//
// The bodies below were moved verbatim from src/Magelight.cpp. Two rules
// make this file safe to have split out, and must not be undone:
//
//   - s_viewsMutex must NOT be held across any of it. The dispatcher runs
//     page JS and the drain calls window.fn, so a lock across either would
//     re-enter through a listener and throw system_error on a non-recursive
//     mutex (invariant 16).
//   - Every handler here fires on the RENDER thread. Game-state work must
//     marshal with GameTask::Post (MagelightGameTask.h), never a raw
//     SKSE AddTask - on VR that can hang the game.

#include "Magelight.h"

#include <JavaScriptCore/JavaScript.h>
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "MagelightApi4.h"
#include "MagelightGameTask.h"
#include "MagelightSound.h"
#include "MagelightVR.h"
#include "host/Platform.h"
#include "host/Render.h"
#include "host/State.h"
#include "host/Views.h"

namespace Magelight {

// Page bridge, moved verbatim from src/Magelight.cpp. The banner comments are
// kept deliberately - they carry the contract each piece has to honour.

    // ── Our own virtual keyboard (0.20.0) ───────────────────────────────
    // A host page (views/keyboard) the VR laser clicks, raised when a text
    // field in ANOTHER view takes focus. It never takes UI mode, so the page
    // being typed into keeps key focus and the keys we inject land there —
    // DrainInputQueue routes keys to the UI-mode view, and the laser's clicks
    // hit-test to whichever view is under the ray. Runtime-independent: no
    // ShowKeyboardForOverlay (it steals input focus), no export lookup.
    // External linkage (declared in host/Views.h): the render-thread rebuild
    // pass reads it to know which view owns the keyboard when a page is released.
    std::atomic<std::uint64_t> s_kbViewId{ 0 };
    // CreateView / SetViewLayer / ShowView / GetDisplaySize are the public
    // namespace functions declared in Magelight.h — do NOT redeclare them
    // static here, that gives them internal linkage and breaks every other
    // translation unit that calls them. Only the file-local queue needs a fwd.

    static void EnsureKeyboardView()
    {
        if (s_kbViewId.load()) return;
        int bw = 0, bh = 0;
        GetDisplaySize(bw, bh);
        constexpr int kKbW = 980, kKbH = 330;
        const int x = (bw > kKbW + 40) ? (bw - kKbW) / 2 : 20;
        const int y = (bh > kKbH + 100) ? (bh - kKbH - 70) : 20;
        const ViewId id = CreateView("views/keyboard/index.html", x, y, kKbW, kKbH, nullptr, false, false);
        if (!id) return;
        SetViewLayer(id, 3);   // System: above the panel it serves
        s_kbViewId.store(id);
        VR::Placement p;
        // Placed, not following — same reasoning as the page layer. Left at
        // the host default so the vr.panelFollow setting covers it too.
        p.mode = VR::DefaultPanelMode();
        // In FRONT of the page, not level with it. The panel layer sits at
        // panelDistanceM (1.2 m by default) — at the keyboard's old 1.1 m the
        // two quads were 10 cm apart with overlapping vertical extents, which
        // sort order hides but which reads as one surface clipping another.
        // 0.9 m keeps it plainly nearer than the page it serves and inside
        // easy pointing reach.
        p.distanceMeters = 0.9f;
        p.widthMeters = 1.0f;
        p.heightOffset = -0.42f;  // below the panel, angled into reach
        VR::SetPlacement(id, p);
        SKSE::log::info("Magelight: own keyboard view {} created ({}x{} at {},{})", id, kKbW, kKbH, x, y);
    }

    void ShowOwnKeyboard(bool on)
    {
        if (on) {
            if (!VR::OwnKeyboardEnabled()) return;   // flat types on the real keyboard
            if (!IsUIModeActive()) return;            // nothing is open to type into
            EnsureKeyboardView();
        }
        // The visibility and the message that says so change together (QueueKeyboardThemeLocked).
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        MlView* kb = FindViewLocked(static_cast<ViewId>(s_kbViewId.load()));
        if (!kb) return;
        ShowViewLocked(*kb, on);
        QueueKeyboardThemeLocked();
    }

    // A key from that page: {"c":"a"} a character, {"vk":8} a virtual key,
    // {"close":true} dismiss. Queued exactly like a typed key, so it reaches
    // the focused page through the ordinary path.
    static void OnOwnKeyboardKey(const std::string& json)
    {
        try {
            const auto j = nlohmann::json::parse(json);
            if (j.value("close", false)) { ShowOwnKeyboard(false); return; }
            if (auto d = j.find("drag"); d != j.end() && d->is_string()) {
                const std::string what = d->get<std::string>();
                const ViewId kb = static_cast<ViewId>(s_kbViewId.load());
                if (what == "start") VR::BeginViewDrag(kb);
                else VR::EndViewDrag();
                return;
            }
            if (auto it = j.find("vk"); it != j.end() && it->is_number_integer()) {
                const int vk = it->get<int>();
                if (vk <= 0 || vk > 0xFF) return;
                const LPARAM scan = static_cast<LPARAM>(
                    MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC) & 0xFF) << 16;
                QueueInput(WM_KEYDOWN, static_cast<WPARAM>(vk), scan);
                if (vk == VK_RETURN || vk == VK_TAB)   // these also produce text
                    QueueInput(WM_CHAR, static_cast<WPARAM>(vk == VK_RETURN ? L'\r' : L'\t'), 0);
                QueueInput(WM_KEYUP, static_cast<WPARAM>(vk), scan | (1 << 30) | (1 << 31));
                return;
            }
            if (auto it = j.find("c"); it != j.end() && it->is_string()) {
                // The page sends ONE character (views/keyboard/index.html sends
                // `{c: <one char>}`), so this is not a bulk text path. The old
                // fixed wchar_t[8] with a cap of 7 happened to fit that; Utf8ToWide
                // removes the arithmetic from the call site entirely.
                for (const wchar_t ch : Utf8ToWide(it->get<std::string>()))
                    QueueInput(WM_CHAR, static_cast<WPARAM>(ch), 0);
            }
        } catch (...) {
            SKSE::log::warn("Magelight: unreadable key payload from the keyboard page");
        }
    }

    // IME (0.27.0) — bodies live after the window statics they touch.
    // The IME's halves of this pair are declared in host/Views.h (stage 4.5):
    // the IME section has not been split into its own TU yet, so its
    // definitions still live in Magelight.cpp and this file cannot see them as
    // file-static.

    static void PlayPageSound(ViewId id, const std::string& arg);   // fwd (defined after the dispatcher)
    static void OnPageHostTheme(ViewId id, const std::string& arg); // fwd (defined after the dispatcher)

    // `function` is the page's own __mlNative, which carries its view id as
    // private data (InstallBridgeShims). The id a page passes is only checked
    // against it: trusting it would let any page fire another view's
    // listeners, sounds or keys as that view.
    static JSValueRef MlNativeDispatch(JSContextRef ctx, JSObjectRef function, JSObjectRef,
        size_t argc, const JSValueRef argv[], JSValueRef*)
    {
        auto argToStr = [&](size_t i) -> std::string {
            if (argc <= i) return {};
            JSStringRef js = JSValueToStringCopy(ctx, argv[i], nullptr);
            if (!js) return {};
            std::string out = JSStringToStd(js);
            JSStringRelease(js);
            return out;
        };
        const std::string idStr = argToStr(0);
        const std::string name = argToStr(1);
        const std::string arg = argToStr(2);
        const ViewId id = static_cast<ViewId>(reinterpret_cast<std::uintptr_t>(JSObjectGetPrivate(function)));
        if (!id || std::strtoull(idStr.c_str(), nullptr, 10) != id) {
            static std::atomic<int> s_forgedLogged{ 0 };
            if (s_forgedLogged.fetch_add(1) < 32) {
                SKSE::log::warn("Magelight: view {} called '{}' as view '{}' — refused (a page acts only as itself)",
                    id, name, idStr);
            }
            return JSValueMakeUndefined(ctx);
        }

        // Reserved channels: the '__' prefix is the host's, so none of these
        // ever reaches a mod's listeners.
        if (name == "__escapecapture") {   // 0.28.0: the page owns Escape ('1') or hands it back ('0')
            SetViewEscapeCapture(id, arg == "1");
            return JSValueMakeUndefined(ctx);
        }
        if (name == "__prepaint") {   // 0.31.11: SetViewPrepaint from the page itself ('1' / '0')
            Magelight::SetViewPrepaint(id, arg == "1");
            return JSValueMakeUndefined(ctx);
        }
        // The IME, the caret and the host keyboard serve the page the user is
        // in: a background page's focus change, blur or caret must not
        // attach, cancel or move them under it, nor hide the keyboard it raised.
        const bool fromUiView = IsUIModeActive() && id == static_cast<ViewId>(s_uiModeView.load()) &&
                                id != static_cast<ViewId>(s_kbViewId.load());
        if (name == "__imecaret") {   // IME (0.27.0): the page's caret -> candidate placement
            if (fromUiView) ImeOnCaret(id, arg);
            return JSValueMakeUndefined(ctx);
        }
        if (name == "__textfocus") {
            const bool focused = (arg == "1");
            // Only the UI-mode view claims the runtime keyboard (typed keys go to it); a
            // blur from any view clears only that view's own claim.
            if (!focused || fromUiView) VR::NoteTextFocus(id, focused);
            if (fromUiView) {
                ImeOnTextFocus(focused);   // IME (0.27.0)
                ShowOwnKeyboard(focused);
            }
            return JSValueMakeUndefined(ctx);
        }
        if (name == "__key") {   // the host's own keyboard page only: the keys go to the UI-mode view
            if (id == static_cast<ViewId>(s_kbViewId.load())) OnOwnKeyboardKey(arg);
            return JSValueMakeUndefined(ctx);
        }
        if (name == "__sound") {   // 0.29.0: page-driven UI sound; "hover:<name>" is throttled
            PlayPageSound(id, arg);
            return JSValueMakeUndefined(ctx);
        }
        if (name == "__hosttheme") {   // 0.31.9: the page's VR keyboard and cursor colours (magelight.hostTheme)
            OnPageHostTheme(id, arg);
            return JSValueMakeUndefined(ctx);
        }

        MlView::Listener l;
        {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            if (MlView* v = FindViewLocked(id)) {
                auto it = v->listeners.find(name);
                if (it != v->listeners.end()) l = it->second;
            }
        }
        if (l.fnEx) l.fnEx(id, arg.c_str(), l.user);
        else if (l.fn) l.fn(arg.c_str());
        else SKSE::log::warn("Magelight: JS called unregistered listener '{}' (view {})", name, id);
        return JSValueMakeUndefined(ctx);
    }

    // 0.29.0, JS thread: validate here, play on the game thread.
    // Only a VISIBLE view may play — a hidden or hibernated page that fires a
    // sound on load must not be heard (the hazard the '__textfocus' gate
    // closed for the keyboard). 'hover:<name>' is throttled to one per ~60 ms
    // per view so a mouse sweep over a list cannot spam the engine.
    static void PlayPageSound(ViewId id, const std::string& arg)
    {
        std::string name = arg;
        bool hover = false;
        if (name.rfind("hover:", 0) == 0) { hover = true; name.erase(0, 6); }
        if (name.empty() || name.size() > 128) return;
        {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            MlView* v = FindViewLocked(id);
            if (!v || !v->visible || v->dormant) return;
        }
        if (hover && !Sound::HoverAllowed(id)) return;
        GameTask::Post([id, name]() { Sound::Play(name, id); });
    }

    // "lit FFD8CCB0 shade ..." or "none (the host's colours)", for the log.
    static std::string DescribeCursorTint(const CursorTintSet& t)
    {
        if (t.Empty()) return "none (the host's colours)";
        char buf[96];
        std::snprintf(buf, sizeof(buf), "lit %08X shade %08X ink %08X glow %08X ibeam %08X", static_cast<unsigned>(t.lit),
                      static_cast<unsigned>(t.shade), static_cast<unsigned>(t.ink), static_cast<unsigned>(t.glow),
                      static_cast<unsigned>(t.ibeam));
        return buf;
    }

    // 0.31.9, JS thread: magelight.hostTheme(obj | null). The keyboard page types into the UI-mode view, so a
    // theme is checked here in full before anything is stored: one bad value refuses the whole message (no partial
    // theme), and only the caller's own slots change. Log lines are rate-limited per view (ThemeLogRate).
    static constexpr std::size_t kHostThemeMaxBytes = 2048;

    static void RefuseHostTheme(ViewId id, const std::string& why)
    {
        std::uint32_t n = 0;
        {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            if (MlView* v = FindViewLocked(id)) n = ++v->themeLog.refusals;
        }
        if (n && n <= kThemeRefusalsLogged)
            SKSE::log::warn("Magelight: view {} hostTheme refused - {} (refusal {}{})", id, why, n,
                            n == kThemeRefusalsLogged ? "; this view's later refusals are counted, not logged" : "");
    }

    static void OnPageHostTheme(ViewId id, const std::string& arg)
    {
        if (id == static_cast<ViewId>(s_kbViewId.load())) return RefuseHostTheme(id, "the keyboard page cannot set a theme");
        if (arg.size() > kHostThemeMaxBytes) return RefuseHostTheme(id, "the message is over 2 KB");
        enum class Part { Keep, Clear, Set };   // the part absent, null, or an object
        Part kbPart = Part::Keep, cursorPart = Part::Keep;
        HostTheme::KbTheme kb{};
        std::string notes;   // optional colours ResolveKeyboard discarded, a low-contrast accent
        CursorTintSet tint;
        if (arg.empty()) {   // hostTheme(null)
            kbPart = cursorPart = Part::Clear;
        } else {
            nlohmann::json j;
            try {
                j = nlohmann::json::parse(arg);
            } catch (...) {
                return RefuseHostTheme(id, "the message is not JSON");
            }
            if (!j.is_object()) return RefuseHostTheme(id, "the message is not a JSON object");
            const auto ver = j.find("v");
            if (ver == j.end() || !ver->is_number_integer() || ver->get<std::int64_t>() != 1)
                return RefuseHostTheme(id, "\"v\" must be 1");
            // A colour of a part: false when absent or bad; `bad` names the first bad one. Unknown keys are ignored.
            std::string bad;
            const auto colour = [&bad](const nlohmann::json& part, const char* partName, const char* key, std::uint32_t& rgb) {
                const auto it = part.find(key);
                if (it == part.end()) return false;
                if (it->is_string() && HostTheme::ParseHex(it->get_ref<const std::string&>(), rgb)) return true;
                if (bad.empty()) bad = std::string(partName) + "." + key + " is not a #rrggbb colour";
                return false;
            };
            if (const auto k = j.find("keyboard"); k != j.end()) {
                if (k->is_null()) {
                    kbPart = Part::Clear;
                } else if (!k->is_object()) {
                    return RefuseHostTheme(id, "\"keyboard\" must be an object or null");
                } else {
                    HostTheme::KbInput in;
                    for (int i = 0; i < HostTheme::kKbTokens; ++i)
                        in.given[i] = colour(*k, "keyboard", HostTheme::kKbNames[i], in.rgb[i]);
                    if (!bad.empty()) return RefuseHostTheme(id, bad);
                    std::string why;
                    if (!HostTheme::ResolveKeyboard(in, kb, why, &notes)) return RefuseHostTheme(id, "keyboard: " + why);
                    kbPart = Part::Set;
                }
            }
            if (const auto c = j.find("cursor"); c != j.end()) {
                if (c->is_null()) {
                    cursorPart = Part::Clear;
                } else if (!c->is_object()) {
                    return RefuseHostTheme(id, "\"cursor\" must be an object or null");
                } else {
                    std::uint32_t* const slots[] = { &tint.lit, &tint.shade, &tint.ink, &tint.glow, &tint.ibeam };
                    static_assert(std::size(slots) == HostTheme::kCursorNames.size());
                    for (std::size_t i = 0; i < std::size(slots); ++i) {
                        std::uint32_t rgb = 0;
                        if (colour(*c, "cursor", HostTheme::kCursorNames[i], rgb)) *slots[i] = 0xFF000000u | rgb;
                    }
                    if (!bad.empty()) return RefuseHostTheme(id, bad);
                    cursorPart = Part::Set;   // a colour left out keeps the host's, as in CursorTint
                }
            }
        }
        // Both parts in one critical section. The cursor part is SetViewCursorTint's store without its per-call log
        // line; the one line here is built from what is stored and written after unlocking.
        std::string line;
        {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            MlView* v = FindViewLocked(id);
            if (!v || v->destroyPending) return;
            bool kbChanged = false, cursorChanged = false;
            if (kbPart != Part::Keep) {
                const std::optional<HostTheme::KbTheme> next = kbPart == Part::Set ? std::optional(kb) : std::nullopt;
                kbChanged = v->kbTheme != next;
                v->kbTheme = next;
                if (kbChanged && id == static_cast<ViewId>(s_uiModeView.load())) QueueKeyboardThemeLocked();
            }
            if (cursorPart != Part::Keep) {
                cursorChanged = !(v->cursorTint == tint);
                v->cursorTint = tint;
            }
            std::uint32_t skipped = 0;
            if ((!kbChanged && !cursorChanged) || !v->themeLog.Admit(GetTickCount64(), skipped)) return;
            if (kbChanged)
                line = "keyboard " + (v->kbTheme ? HostTheme::KeyboardJson(*v->kbTheme) : std::string("none (the host's look)"));
            if (cursorChanged) line += std::string(line.empty() ? "" : "; ") + "cursor tint " + DescribeCursorTint(v->cursorTint);
            if (kbChanged && !notes.empty()) line += "; " + notes;
            else notes.clear();
            if (skipped) line += " (" + std::to_string(skipped) + " earlier changes not logged)";
        }
        if (!notes.empty()) SKSE::log::warn("Magelight: view {} hostTheme (page): {}", id, line);
        else SKSE::log::info("Magelight: view {} hostTheme (page): {}", id, line);
    }

    // Render thread. full (OnWindowObjectReady, a fresh window object): the
    // native dispatcher, the page core (__MAGELIGHT__, window.magelight and
    // the document listeners) and one shim per listener. !full (shimsDirty, a
    // listener registered after load): a fresh __mlNative and every listener
    // shim, early ones included; the page keeps its window.magelight and every
    // subscription on it. The core also guards itself with window.__mlCore:
    // it runs once per window, never twice.
    void InstallBridgeShims(MlView& v, bool full)
    {
        if (!v.ul) return;
        auto ctxScope = v.ul->LockJSContext();
        JSContextRef ctx = (*ctxScope);
        JSObjectRef global = JSContextGetGlobalObject(ctx);

        // A callable object of a class, not a plain function: only a class
        // instance can carry private data, and that is how the dispatcher
        // knows which page called it.
        static const JSClassRef s_nativeClass = [] {
            JSClassDefinition def{};   // zeroed, the same as kJSClassDefinitionEmpty
            def.className = "MagelightNative";
            def.callAsFunction = &MlNativeDispatch;
            return JSClassCreate(&def);
        }();
        JSStringRef dispName = JSStringCreateWithUTF8CString("__mlNative");
        JSObjectRef fnObj = JSObjectMake(ctx, s_nativeClass,
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(v.id)));
        // A class instance inherits Object.prototype: without Function.prototype
        // the SDK's typed-as-a-function __mlNative has no call/apply/bind.
        {
            JSStringRef fnName = JSStringCreateWithUTF8CString("Function");
            JSStringRef protoName = JSStringCreateWithUTF8CString("prototype");
            const JSValueRef ctor = JSObjectGetProperty(ctx, global, fnName, nullptr);
            if (ctor && JSValueIsObject(ctx, ctor)) {
                const JSValueRef proto = JSObjectGetProperty(ctx, JSValueToObject(ctx, ctor, nullptr), protoName, nullptr);
                if (proto && JSValueIsObject(ctx, proto)) JSObjectSetPrototype(ctx, fnObj, proto);
            }
            JSStringRelease(protoName);
            JSStringRelease(fnName);
        }
        JSObjectSetProperty(ctx, global, dispName, fnObj, kJSPropertyAttributeNone, nullptr);
        JSStringRelease(dispName);

        // Host marker FIRST — pages detect which host they run on at their
        // first line (runtime host detection for the SA frontend; the prior
        // host installed no page global, so presence == Magelight).
        // __MAGELIGHT__ = who am I / what can the host do; magelight = the two
        // verbs every page gets (send/on/off) with pre-subscribe buffering,
        // so a payload that lands before the app mounts is replayed, not
        // lost. InteropCall to a channel with no window shim routes into
        // _dispatch (see DrainBridgeQueues) — plain HTML and the SDK share it.
        const std::string idStr0 = std::to_string(v.id);
        std::string shims;
        if (full) shims =
            "if(!window.__mlCore){Object.defineProperty(window,'__mlCore',{value:true});"
            "window.__MAGELIGHT__={version:'" PLUGIN_VERSION "',versionNumber:" +
            std::to_string(PLUGIN_VERSION_MAJOR * 10000 + PLUGIN_VERSION_MINOR * 100 + PLUGIN_VERSION_PATCH) +
            ",viewId:" + idStr0 + "," + Api4::ViewIdentityJson(v.id) +
            ",capabilities:" + Api4::CapabilitiesJson() +
            ",dev:" + (s_devMode.load() ? "true" : "false") +
            ",runtimeUrl:" + MlJsonQuote(PathToFileUrl(s_runtimeDir) + "/") + "};"
            "window.magelight=(function(){var subs={},buf={},id='" + idStr0 + "';"
            "function dispatch(ch,arg){var s=subs[ch];if(!s||!s.length){(buf[ch]=buf[ch]||[]).push(arg);return;}"
            "for(var i=0;i<s.length;i++){try{s[i](arg);}catch(e){console.error('[magelight] handler for '+ch+' threw',e);}}}"
            "return{send:function(ch,p){var a=p==null?'':(typeof p==='string'?p:JSON.stringify(p));__mlNative(id,ch,a);},"
            // 0.29.0: magelight.sound('click') -> the reserved '__sound' channel (host plays it through the game's audio)
            "sound:function(n){try{__mlNative(id,'__sound',n==null?'':String(n));}catch(e){}},"
            // 0.31.9: magelight.hostTheme({v:1,keyboard:{..},cursor:{..}}) or (null) -> the reserved '__hosttheme' channel
            "hostTheme:function(o){try{__mlNative(id,'__hosttheme',o==null?'':JSON.stringify(o));}catch(e){}},"
            "on:function(ch,fn){(subs[ch]=subs[ch]||[]).push(fn);var b=buf[ch];if(b){delete buf[ch];for(var i=0;i<b.length;i++){try{fn(b[i]);}catch(e){console.error('[magelight] replay for '+ch+' threw',e);}}}"
            "return function(){window.magelight.off(ch,fn);};},"
            "off:function(ch,fn){var s=subs[ch];if(!s)return;var i=s.indexOf(fn);if(i>=0)s.splice(i,1);},"
            "_dispatch:dispatch,pending:function(ch){return (buf[ch]||[]).length;}};})();"
            // Text-field focus -> the reserved '__textfocus' channel. The VR
            // presenter raises the runtime's virtual keyboard on it (there is
            // no keyboard in a headset), and it costs a flat host nothing.
            "(function(){function isText(e){if(!e)return false;if(e.isContentEditable)return true;"
            "var t=(e.tagName||'').toUpperCase();if(t==='TEXTAREA')return true;if(t!=='INPUT')return false;"
            "var y=(e.type||'text').toLowerCase();"
            "return ['text','search','url','tel','email','password','number'].indexOf(y)>=0;}"
            "window.__mlIsText=isText;"   // shared with the IME block below (one predicate)
            "function send(f){try{__mlNative('" + idStr0 + "','__textfocus',f?'1':'0');}catch(e){}}"
            "document.addEventListener('focusin',function(ev){if(isText(ev.target))send(true);},true);"
            "document.addEventListener('focusout',function(){setTimeout(function(){send(isText(document.activeElement));},0);},true);"
            "if(isText(document.activeElement))send(true);})();"
            // UI sounds (0.29.0): the markup convention. data-ml-sound="click"
            // plays on click (keyboard activation raises click too), data-ml-sound-hover
            // ="focus" on entering the element. Delegated on document so a manifest-only
            // mod gets button sounds from one attribute; 'mouseover' (bubbles) with a
            // last-element latch, since 'mouseenter' does not bubble. The host throttles
            // hover per view.
            "(function(){var id='" + idStr0 + "',last=null;"
            "function up(e,a){while(e&&e.getAttribute){if(e.hasAttribute(a))return e;e=e.parentNode;}return null;}"
            "document.addEventListener('click',function(ev){var t=up(ev.target,'data-ml-sound');if(!t)return;"
            // A <label> wrapping its control re-dispatches the click on the control: play that one only.
            "if(t.control&&!t.control.disabled&&!t.control.contains(ev.target)&&t.contains(t.control))return;"
            "var n=t.getAttribute('data-ml-sound');if(n)try{__mlNative(id,'__sound',n);}catch(e){}},true);"
            "document.addEventListener('mouseover',function(ev){var t=up(ev.target,'data-ml-sound-hover');if(t===last)return;last=t;if(!t)return;"
            "var n=t.getAttribute('data-ml-sound-hover');if(n)try{__mlNative(id,'__sound','hover:'+n);}catch(e){}},true);"
            "})();"
            // IME (0.27.0): caret reporting ('__imecaret', CSS px), the inline pre-edit
            // (the composition rides as the field's SELECTION so the commit — ordinary
            // characters — replaces it and a cancel deletes it) and a 'magelight:ime'
            // window event for pages that want an indicator.
            "(function(){var id='" + idStr0 + "',ime={el:null,base:null,pre:'',start:0,end:0};"
            "var isText=window.__mlIsText;"
            "function caret(){var e=document.activeElement;if(!isText(e))return;var r=e.getBoundingClientRect(),x=r.left,y=r.top,h=r.height;"
            "try{if(e.isContentEditable){var s=window.getSelection();if(s&&s.rangeCount){var rr=s.getRangeAt(0).getBoundingClientRect();"
            "if(rr.left||rr.top){x=rr.left;y=rr.top;h=rr.height||h;}}}"
            "else{var cs=getComputedStyle(e),cv=document.createElement('canvas').getContext('2d');cv.font=cs.font;"
            "var v=e.value||'',p=e.selectionStart||0,line=v.slice(0,p),nl=line.lastIndexOf('\\n');if(nl>=0)line=line.slice(nl+1);"
            "var lh=parseFloat(cs.lineHeight);if(!(lh>0))lh=(parseFloat(cs.fontSize)||14)*1.3;h=lh;"
            "x=r.left+(parseFloat(cs.paddingLeft)||0)+(parseFloat(cs.borderLeftWidth)||0)+cv.measureText(line).width-(e.scrollLeft||0);"
            "if(e.tagName.toUpperCase()==='TEXTAREA'){var ln=v.slice(0,p).split('\\n').length;y=r.top+(parseFloat(cs.paddingTop)||0)+(ln-1)*lh-(e.scrollTop||0);}"
            "else{y=r.top+(r.height-lh)/2;}}}catch(err){}"
            "try{__mlNative(id,'__imecaret',Math.round(x)+','+Math.round(y)+','+Math.round(h));}catch(err){}}"
            "document.addEventListener('focusin',function(ev){if(isText(ev.target))setTimeout(caret,0);},true);"
            "document.addEventListener('input',function(){caret();},true);"
            "document.addEventListener('selectionchange',function(){caret();});"
            "document.addEventListener('click',function(){setTimeout(caret,0);},true);"
            "function sel(e,a,b){try{e.setSelectionRange(a,b);}catch(x){}}"
            "function preeditStanding(e){return ime.el===e&&ime.base!=null&&e.value===ime.base.slice(0,ime.start)+ime.pre+ime.base.slice(ime.end)"
            "&&e.selectionStart===ime.start&&e.selectionEnd===ime.start+ime.pre.length;}"
            "function clearPreedit(e){if(preeditStanding(e)){e.value=ime.base.slice(0,ime.start)+ime.base.slice(ime.end);sel(e,ime.start,ime.start);}}"
            "if(window.magelight){window.magelight.on('__ime',function(a){var m;try{m=JSON.parse(a);}catch(e){return;}"
            "var e=document.activeElement,t=(e&&e.tagName||'').toUpperCase(),plain=!!e&&(t==='INPUT'||t==='TEXTAREA')&&!e.isContentEditable;"
            "if(m.state==='start'){ime.pre='';if(plain){ime.el=e;ime.base=e.value;ime.start=e.selectionStart||0;ime.end=e.selectionEnd||ime.start;}else{ime.el=null;ime.base=null;}}"
            "else if(m.state==='compose'){if(plain&&ime.el===e&&ime.base!=null){ime.pre=m.text||'';e.value=ime.base.slice(0,ime.start)+ime.pre+ime.base.slice(ime.end);sel(e,ime.start,ime.start+ime.pre.length);}caret();}"
            "else if(m.state==='commit'){if(plain)clearPreedit(e);ime.base=null;ime.pre='';}"
            "else if(m.state==='end'){if(plain)clearPreedit(e);ime.el=null;ime.base=null;ime.pre='';}"
            "try{window.dispatchEvent(new CustomEvent('magelight:ime',{detail:m}));}catch(x){}});}"
            "})();"
            "}";
        std::size_t count = 0;
        {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            const std::string idStr = idStr0;
            for (const auto& [n, l] : v.listeners) {
                (void)l;
                // JsonQuote: the listener name is mod-supplied — a quote in it
                // must not break (or break out of) the generated script.
                shims += "window[" + JsonQuote(n) + "]=function(a){__mlNative('" + idStr +
                         "'," + JsonQuote(n) + ",a==null?'':String(a));};";
            }
            count = v.listeners.size();
            v.shimsDirty = false;  // this install covers everything registered so far
        }
        v.ul->EvaluateScript(shims.c_str());
        if (full) SKSE::log::info("Magelight: bridge installed on view {} ({} JS listeners)", v.id, count);
        else SKSE::log::info("Magelight: {} JS listener shim(s) refreshed on view {}", count, v.id);
    }

    // Render thread, once per frame: deliver queued C++ -> JS calls per view.
    void DrainBridgeQueues()
    {
        // Snapshot (view, calls) pairs under the lock; deliver outside it.
        std::vector<std::pair<ultralight::RefPtr<ultralight::View>, std::vector<OutboundCall>>> work;
        struct EvalWork { ultralight::RefPtr<ultralight::View> ul; ViewId id; std::vector<PendingEval> evals; };
        std::vector<EvalWork> evalWork;
        {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            for (auto& v : s_views) {
                if (!v->ul || !v->domReady) continue;   // held until the page can take them
                if (!v->outbound.empty()) {
                    work.emplace_back(v->ul, std::move(v->outbound));
                    v->outbound.clear();
                }
                if (!v->evals.empty()) {
                    evalWork.push_back(EvalWork{ v->ul, v->id, std::move(v->evals) });
                    v->evals.clear();
                }
            }
        }
        for (auto& ew : evalWork) {
            for (const auto& e : ew.evals) {
                ultralight::String exc;
                const ultralight::String result = ew.ul->EvaluateScript(e.script.c_str(), &exc);
                if (e.fn) e.fn(ew.id, result.utf8().data(), exc.utf8().data(), e.user);
            }
        }
        for (auto& [ul, calls] : work) {
            auto ctxScope = ul->LockJSContext();
            JSContextRef ctx = (*ctxScope);
            JSObjectRef global = JSContextGetGlobalObject(ctx);
            for (const auto& c : calls) {
                if (c.raw) {
                    ul->EvaluateScript(c.fn.c_str());  // fn carries the script
                    continue;
                }
                JSStringRef fnName = JSStringCreateWithUTF8CString(c.fn.c_str());
                JSValueRef fnVal = JSObjectGetProperty(ctx, global, fnName, nullptr);
                JSStringRelease(fnName);
                JSObjectRef fnObj = (fnVal && JSValueIsObject(ctx, fnVal))
                    ? JSValueToObject(ctx, fnVal, nullptr) : nullptr;
                if (!fnObj || !JSObjectIsFunction(ctx, fnObj)) {
                    // No window shim for this channel: hand it to the page
                    // core, which buffers until someone subscribes.
                    ul->EvaluateScript(("window.magelight&&window.magelight._dispatch(" + MlJsonQuote(c.fn) + "," +
                                        MlJsonQuote(c.arg) + ");").c_str());
                    continue;
                }
                JSStringRef argStr = JSStringCreateWithUTF8CString(c.arg.c_str());
                JSValueRef argVal = JSValueMakeString(ctx, argStr);
                JSStringRelease(argStr);
                JSValueRef exc = nullptr;
                JSObjectCallAsFunction(ctx, fnObj, nullptr, 1, &argVal, &exc);
                if (exc) SKSE::log::warn("Magelight: InteropCall('{}') threw in JS", c.fn);
            }
        }
    }

}  // namespace Magelight
