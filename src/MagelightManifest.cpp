// Manifest mods — see MagelightManifest.h and docs/MANIFEST.md.

#include <SKSE/SKSE.h>

#include "Magelight.h"
#include "MagelightApi4.h"
#include "MagelightManifest.h"
#include "MagelightDevWatch.h"
#include "HotkeyNames.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>

namespace Magelight::Manifest {

    using namespace MAGELIGHT_API;
    namespace fs = std::filesystem;

    namespace {

        // "1.2.3" -> 10203 (missing parts = 0); anything unparseable = 0.
        std::uint32_t ParseVersion(const std::string& s)
        {
            unsigned parts[3] = { 0, 0, 0 };
            int i = 0;
            std::string cur;
            for (char c : s) {
                if (c == '.') { if (i < 2) parts[i++] = static_cast<unsigned>(std::strtoul(cur.c_str(), nullptr, 10)); cur.clear(); }
                else if (std::isdigit(static_cast<unsigned char>(c))) cur += c;
                else break;
            }
            if (!cur.empty() && i < 3) parts[i] = static_cast<unsigned>(std::strtoul(cur.c_str(), nullptr, 10));
            return PackVersion(parts[0], parts[1], parts[2]);
        }

        std::string Lower(std::string s)
        {
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        Layer ParseLayer(const std::string& s)
        {
            const std::string l = Lower(s);
            if (l == "hud") return Layer::Hud;
            if (l == "popup") return Layer::Popup;
            if (l == "system") return Layer::System;
            return Layer::Panel;
        }

        std::uint32_t ParseVRButton(const std::string& raw)
        {
            const std::string s = Lower(raw);
            if (s.empty() || s == "none") return MAGELIGHT_API::kVRButtonNone;
            if (s == "menu" || s == "b" || s == "y" || s == "by") return MAGELIGHT_API::kVRButtonMenu;
            if (s == "grip" || s == "squeeze") return MAGELIGHT_API::kVRButtonGrip;
            if (s == "a" || s == "x" || s == "ax") return MAGELIGHT_API::kVRButtonA;
            if (s == "stick" || s == "stickclick" || s == "thumbstick") return MAGELIGHT_API::kVRButtonStickClick;
            if (s == "trigger") return MAGELIGHT_API::kVRButtonTrigger;
            if (s == "touchpad" || s == "pad") return MAGELIGHT_API::kVRButtonTouchpad;
            return 0xFFFFFFFFu;   // named but unknown — the caller reports it
        }

        Anchor ParseAnchor(const std::string& s)
        {
            const std::string l = Lower(s);
            if (l == "top-right" || l == "topright") return Anchor::TopRight;
            if (l == "bottom-left" || l == "bottomleft") return Anchor::BottomLeft;
            if (l == "bottom-right" || l == "bottomright") return Anchor::BottomRight;
            return Anchor::TopLeft;
        }

        // A page path must stay inside the mod folder: relative, no "..".
        bool SafeRelative(const std::string& p)
        {
            if (p.empty() || p.size() > 260) return false;
            if (p[0] == '/' || p[0] == '\\' || (p.size() > 1 && p[1] == ':')) return false;
            if (p.find("..") != std::string::npos) return false;
            return true;
        }

        // Non-throwing scalar reads: nlohmann's `.value(key, default)` THROWS
        // when the key is present with a mismatched type — quoting a number
        // ("w": "320") or a bool ("clickThrough": "true") is among the most
        // common JSON mistakes, and an uncaught throw here aborted the whole
        // manifest scan (or crashed). These read the default AND log a clear
        // per-field warning on a type mismatch instead (review 2026-09-05).
        static int  MJInt (const nlohmann::json& o, const char* k, int  d, const std::string& ctx) {
            auto it = o.find(k); if (it == o.end()) return d;
            if (!it->is_number_integer()) { SKSE::log::warn("Magelight[manifest]: {}'{}' should be a whole number — ignored", ctx, k); return d; }
            return it->get<int>();
        }
        static bool MJBool(const nlohmann::json& o, const char* k, bool d, const std::string& ctx) {
            auto it = o.find(k); if (it == o.end()) return d;
            if (!it->is_boolean()) { SKSE::log::warn("Magelight[manifest]: {}'{}' should be true or false — ignored", ctx, k); return d; }
            return it->get<bool>();
        }
        static float MJFloat(const nlohmann::json& o, const char* k, float d, const std::string& ctx) {
            auto it = o.find(k); if (it == o.end()) return d;
            if (!it->is_number()) { SKSE::log::warn("Magelight[manifest]: {}'{}' should be a number — ignored", ctx, k); return d; }
            return it->get<float>();
        }
        static std::string MJStr(const nlohmann::json& o, const char* k, const std::string& d, const std::string& ctx) {
            auto it = o.find(k); if (it == o.end()) return d;
            if (!it->is_string()) { SKSE::log::warn("Magelight[manifest]: {}'{}' should be text — ignored", ctx, k); return d; }
            return it->get<std::string>();
        }
        // Warn on any key not in the known set (a case/spelling slip is
        // otherwise dropped silently, MANIFEST.md "unknown keys are ignored").
        static void WarnUnknownKeys(const nlohmann::json& o, std::initializer_list<const char*> known, const std::string& ctx) {
            for (auto it = o.begin(); it != o.end(); ++it) {
                bool ok = false, nearMiss = false; const char* suggest = nullptr;
                for (const char* k : known) {
                    if (it.key() == k) { ok = true; break; }
                    if (!nearMiss && Lower(it.key()) == Lower(k)) { nearMiss = true; suggest = k; }
                }
                if (!ok) {
                    if (suggest) SKSE::log::warn("Magelight[manifest]: {}unknown key '{}' — did you mean '{}'?", ctx, it.key(), suggest);
                    else         SKSE::log::warn("Magelight[manifest]: {}unknown key '{}' — ignored", ctx, it.key());
                }
            }
        }

        // 0.31.0 "cursor": "img.png" (the arrow image, hotspot 0,0) | "none" (the page draws its own pointer) |
        // { "arrow": state, "pointer": state, "text": state, "press": bool }, a state being "img.png" or
        // { "image": "img.png", "hotspot": [x, y], "height": px }. Paths are relative to the mod folder. False when
        // nothing usable was declared; each problem is logged.
        bool ParseCursor(const nlohmann::json& c, const fs::path& modDir, const std::string& ctx, Magelight::CursorSet& out)
        {
            out = Magelight::CursorSet{};
            const std::string cctx = ctx + "cursor: ";
            const auto image = [&](const std::string& path, float hx, float hy, float height, bool press,
                                   const char* state) -> std::shared_ptr<Magelight::CursorImage> {
                if (!SafeRelative(path)) {
                    SKSE::log::error("Magelight[manifest]: {}{} '{}' must be a relative path inside the mod folder", cctx, state, path);
                    return nullptr;
                }
                std::string why;
                auto img = Magelight::MakeCursorImage(modDir / fs::path(path).make_preferred(), hx, hy, height, press, &why);
                if (!img) SKSE::log::error("Magelight[manifest]: {}{} - {}", cctx, state, why);
                return img;
            };
            if (c.is_string()) {
                const std::string v = c.get<std::string>();
                if (Lower(v) == "none") {
                    out.none = true;
                    return true;
                }
                out.image[0] = image(v, 0.0f, 0.0f, 0.0f, false, "arrow");
                return !out.Empty();
            }
            if (!c.is_object()) {
                SKSE::log::warn("Magelight[manifest]: {}'cursor' must be an image path, \"none\" or an object - ignored", ctx);
                return false;
            }
            WarnUnknownKeys(c, { "arrow", "pointer", "text", "press" }, cctx);
            const bool press = MJBool(c, "press", false, cctx);
            static constexpr const char* kStates[Magelight::kCursorStates] = { "arrow", "pointer", "text" };
            for (int i = 0; i < Magelight::kCursorStates; ++i) {
                auto it = c.find(kStates[i]);
                if (it == c.end()) continue;
                if (it->is_string()) {
                    out.image[i] = image(it->get<std::string>(), 0.0f, 0.0f, 0.0f, press, kStates[i]);
                    continue;
                }
                if (!it->is_object()) {
                    SKSE::log::warn("Magelight[manifest]: {}'{}' must be an image path or {{ \"image\", \"hotspot\", \"height\" }} - ignored",
                                    cctx, kStates[i]);
                    continue;
                }
                const std::string sctx = cctx + kStates[i] + ": ";
                WarnUnknownKeys(*it, { "image", "hotspot", "height" }, sctx);
                const std::string path = MJStr(*it, "image", "", sctx);
                float hx = 0.0f, hy = 0.0f;
                if (auto hs = it->find("hotspot"); hs != it->end()) {
                    if (hs->is_array() && hs->size() == 2 && (*hs)[0].is_number() && (*hs)[1].is_number()) {
                        hx = (*hs)[0].get<float>();
                        hy = (*hs)[1].get<float>();
                    } else {
                        SKSE::log::warn("Magelight[manifest]: {}'hotspot' should be [x, y] in image pixels - using 0, 0", sctx);
                    }
                }
                const float height = MJFloat(*it, "height", 0.0f, sctx);
                if (path.empty()) {
                    SKSE::log::error("Magelight[manifest]: {}'image' is missing", sctx);
                    continue;
                }
                out.image[i] = image(path, hx, hy, height, press, kStates[i]);
            }
            return !out.Empty();
        }

        void LoadOne(const fs::path& modDir)
        try {
            const fs::path file = modDir / "manifest.json";
            const std::string folder = modDir.filename().string();
            nlohmann::json j;
            try {
                std::ifstream f(file);
                if (!f) { SKSE::log::error("Magelight[manifest]: {} — cannot open", file.string()); return; }
                f >> j;
            } catch (const std::exception& e) {
                SKSE::log::error("Magelight[manifest]: {} — invalid JSON: {}", file.string(), e.what());
                return;
            }
            if (!j.is_object()) {
                SKSE::log::error("Magelight[manifest]: {} — top level must be an object", file.string());
                return;
            }
            const std::string topCtx = file.string() + " — ";
            WarnUnknownKeys(j, { "modId", "name", "version", "minHost", "session", "network", "dev", "views", "cursor" }, topCtx);
            const std::string modId = MJStr(j, "modId", folder, topCtx);
            // The folder is the mod's identity: a manifest naming another
            // modId would take over that mod's slug, and with it the network
            // reach and storage jar of the plugin that registers it.
            if (Lower(modId) != Lower(folder)) {
                SKSE::log::error("Magelight[manifest]: {} — modId '{}' must equal its folder name '{}' "
                                 "(or be left out); not loaded", file.string(), modId, folder);
                return;
            }
            const std::string name = MJStr(j, "name", modId, topCtx);
            const std::string version = MJStr(j, "version", "0.0.0", topCtx);
            const std::string minHost = MJStr(j, "minHost", "0.0.0", topCtx);
            const std::string session = Lower(MJStr(j, "session", "isolated", topCtx));
            // "file" (the default) or "loopback". Internet reach ("any") is a
            // plugin's call: only a DLL can vouch for its pages that way.
            const std::string network = Lower(MJStr(j, "network", "file", topCtx));
            bool loopback = false;
            if (network == "loopback") {
                loopback = true;
            } else if (network == "any") {
                SKSE::log::error("Magelight[manifest]: {}'network': \"any\" refused — a manifest may ask for \"file\" or "
                                 "\"loopback\"; internet reach needs the mod's DLL (SetNetworkPolicy). Using \"file\"", topCtx);
            } else if (network != "file") {
                SKSE::log::warn("Magelight[manifest]: {}'network' should be \"file\" or \"loopback\" — using \"file\"", topCtx);
            }
            const bool dev = MJBool(j, "dev", false, topCtx);   // hot reload this mod's folder
            if (dev) SKSE::log::info("Magelight[manifest]: '{}' dev mode — PAGE files hot-reload; "
                                     "manifest.json edits (geometry, hotkeys, adding views) need a game restart", modId);

            ModId mod = 0;
            const std::string dirStr = modDir.string();
            const Result r = Api4::RegisterManifestMod(modId.c_str(), name.c_str(), ParseVersion(version),
                                                       ParseVersion(minHost),
                                                       session == "default" ? "default" : nullptr, dirStr.c_str(), &mod);
            if (r != Result::Ok) {
                const char* why = r == Result::HostTooOld ? "needs a newer Magelight (minHost)"
                                : r == Result::InvalidArgument ? "modId is not a valid slug ([A-Za-z0-9_.-], 1..32)"
                                : r == Result::InvalidMod ? "modId already registered by a DLL"
                                : r == Result::Denied ? "its storage jar (named after the modId) is a plugin's session"
                                : "refused";
                SKSE::log::error("Magelight[manifest]: {} — {}", file.string(), why);
                return;
            }
            // Before the views exist, so each one is created at the mod's level.
            if (loopback) Api4::SetNetworkPolicy(mod, NetworkPolicy::LoopbackOnly);
            // 0.31.0: the mod's default cursor, before the views exist so each one is created with it.
            if (auto c = j.find("cursor"); c != j.end()) {
                Magelight::CursorSet set;
                if (ParseCursor(*c, modDir, topCtx, set)) Api4::SetModCursor(mod, set);
            }

            int created = 0, failed = 0;
            if (auto views = j.find("views"); views != j.end() && views->is_object()) {
                for (auto it = views->begin(); it != views->end(); ++it) {
                    const std::string vname = it.key();
                    const nlohmann::json& v = it.value();
                    if (!v.is_object()) { SKSE::log::error("Magelight[manifest]: {} — view '{}' must be an object", file.string(), vname); ++failed; continue; }
                    const std::string vctx = file.string() + " — view '" + vname + "': ";
                    WarnUnknownKeys(v, { "path", "anchor", "x", "y", "w", "h", "fullscreen", "clickThrough",
                                         "startVisible", "layer", "hibernateMs", "vr", "vrHotkey", "hotkey",
                                         "hotkeyPause", "sounds", "loadOnShow", "cursor" }, vctx);
                    const std::string path = MJStr(v, "path", "", vctx);
                    if (!SafeRelative(path)) {
                        SKSE::log::error("Magelight[manifest]: {} — view '{}': 'path' must be a relative path inside the mod folder", file.string(), vname);
                        ++failed; continue;
                    }
                    const fs::path page = modDir / path;
                    if (!fs::exists(page)) {
                        SKSE::log::error("Magelight[manifest]: {} — view '{}': page not found: {}", file.string(), vname, page.string());
                        ++failed; continue;
                    }
                    const std::string abs = page.string();
                    ViewDesc vd{};
                    vd.size = sizeof(ViewDesc);
                    vd.name = vname.c_str();
                    vd.htmlPath = abs.c_str();
                    vd.anchor = ParseAnchor(MJStr(v, "anchor", "top-left", vctx));
                    vd.x = MJInt(v, "x", 0, vctx); vd.y = MJInt(v, "y", 0, vctx);
                    vd.w = MJInt(v, "w", 0, vctx); vd.h = MJInt(v, "h", 0, vctx);
                    vd.fullscreen = MJBool(v, "fullscreen", false, vctx);
                    vd.clickThrough = MJBool(v, "clickThrough", false, vctx);
                    vd.startVisible = MJBool(v, "startVisible", false, vctx);
                    vd.layer = ParseLayer(MJStr(v, "layer", "panel", vctx));
                    vd.uiScale = 0.0f;
                    vd.onDomReady = nullptr;
                    if (!vd.fullscreen && (vd.w <= 0 || vd.h <= 0)) {
                        SKSE::log::error("Magelight[manifest]: {} — view '{}': needs w/h > 0 or fullscreen: true", file.string(), vname);
                        ++failed; continue;
                    }
                    ViewId id = 0;
                    if (Api4::CreateViewEx(mod, &vd, &id) != Result::Ok) {
                        SKSE::log::error("Magelight[manifest]: {} — view '{}': {}", file.string(), vname, Api4::GetLastErrorMessage(mod));
                        ++failed; continue;
                    }
                    ++created;
                    if (dev) Dev::WatchView(id, modDir);
                    // 0.31.0: manifest views exist before the world loads, so this always lands before the first load.
                    if (MJBool(v, "loadOnShow", false, vctx)) Api4::SetViewLoadOnShow(id, true);
                    if (const int hib = MJInt(v, "hibernateMs", 0, vctx); hib > 0) Api4::SetViewHibernate(id, static_cast<std::uint32_t>(hib));
                    // 0.31.0: the view's own cursor, over the mod's default.
                    if (auto c = v.find("cursor"); c != v.end()) {
                        Magelight::CursorSet set;
                        if (ParseCursor(*c, modDir, vctx, set)) Magelight::SetViewCursorSet(id, false, set);
                    }
                    // UI sounds (0.29.0): host-played when this view
                    // enters / leaves UI mode. Off unless asked for — no surprise
                    // sounds for existing mods.
                    if (auto sn = v.find("sounds"); sn != v.end()) {
                        if (!sn->is_object()) {
                            SKSE::log::warn("Magelight[manifest]: {} — view '{}': 'sounds' must be an object {{ \"open\": name, \"close\": name }} — ignored",
                                            file.string(), vname);
                        } else {
                            WarnUnknownKeys(*sn, { "open", "close" }, vctx + "sounds: ");
                            const std::string so = MJStr(*sn, "open", "", vctx + "sounds: ");
                            const std::string sc = MJStr(*sn, "close", "", vctx + "sounds: ");
                            if (!so.empty() || !sc.empty()) Api4::SetViewSounds(id, so.c_str(), sc.c_str());
                        }
                    }
                    // VR placement (0.18.0). Ignored on a flat runtime.
                    if (auto vr = v.find("vr"); vr != v.end() && vr->is_object()) {
                        MAGELIGHT_API::VRPlacementDesc pd{};
                        pd.size = sizeof(pd);
                        // Same non-throwing reads as the top level (review 2026-09-09:
                        // a mistyped value here still aborted the WHOLE manifest).
                        WarnUnknownKeys(*vr, { "mode", "distance", "width", "heightOffset" }, vctx + "vr: ");
                        const std::string m = Lower(MJStr(*vr, "mode", "lazy", vctx + "vr: "));
                        pd.mode = (m == "head" || m == "headlocked") ? 0
                                : (m == "world" || m == "worldlocked") ? 1 : 3;
                        pd.distanceMeters = MJFloat(*vr, "distance", 0.0f, vctx);
                        pd.widthMeters    = MJFloat(*vr, "width", 0.0f, vctx);
                        pd.heightOffset   = MJFloat(*vr, "heightOffset", -0.15f, vctx);
                        Api4::SetViewVRPlacement(id, &pd);
                    }
                    // VR controller binding (0.18.0): the keyboard-free way in.
                    if (auto vh = v.find("vrHotkey"); vh != v.end() && vh->is_object()) {
                        WarnUnknownKeys(*vh, { "button", "modifier", "hand", "pause" }, vctx + "vrHotkey: ");
                        const std::string bname = MJStr(*vh, "button", "", vctx + "vrHotkey: ");
                        const std::string mname = MJStr(*vh, "modifier", "", vctx + "vrHotkey: ");
                        const std::uint32_t btn = ParseVRButton(bname);
                        const std::uint32_t mod2 = ParseVRButton(mname);
                        if (btn == 0xFFFFFFFFu || mod2 == 0xFFFFFFFFu) {
                            SKSE::log::error("Magelight[manifest]: {} — view '{}': vrHotkey button/modifier unknown "
                                "(menu|grip|a|stick|trigger|touchpad)", file.string(), vname);
                        } else if (btn) {
                            MAGELIGHT_API::VRHotkeyDesc hd{};
                            hd.size = sizeof(hd);
                            hd.button = btn;
                            hd.modifier = mod2;
                            const std::string h = Lower(MJStr(*vh, "hand", "either", vctx + "vrHotkey: "));
                            hd.hand = (h == "left") ? 1 : (h == "right") ? 2 : 0;
                            const bool hud2 = vd.clickThrough || vd.layer == Layer::Hud;
                            hd.action = hud2 ? kHotkeyActionToggleVisible
                                             : MJBool(*vh, "pause", false, vctx + "vrHotkey: ") ? kHotkeyActionToggleUIModePaused
                                                                         : kHotkeyActionToggleUIMode;
                            if (Api4::BindVRHotkey(id, &hd) != Result::Ok)
                                SKSE::log::error("Magelight[manifest]: {} — view '{}': {}", file.string(), vname, Api4::GetLastErrorMessage(mod));
                        }
                    }
                    if (auto hk = v.find("hotkey"); hk != v.end()) {
                        std::string err;
                        const std::uint32_t code = HotkeyNames::Parse(*hk, err);
                        if (!err.empty()) {
                            SKSE::log::error("Magelight[manifest]: {} — view '{}': hotkey {}", file.string(), vname, err);
                        } else if (!code) {
                            SKSE::log::info("Magelight[manifest]: {} — view '{}': hotkey off", file.string(), vname);
                        } else {
                            const bool hud = vd.clickThrough || vd.layer == Layer::Hud;
                            const std::uint32_t action = hud ? kHotkeyActionToggleVisible
                                                       : MJBool(v, "hotkeyPause", false, vctx) ? kHotkeyActionToggleUIModePaused
                                                                                              : kHotkeyActionToggleUIMode;
                            if (Api4::BindHotkey(id, code, action) != Result::Ok)
                                SKSE::log::error("Magelight[manifest]: {} — view '{}': {}", file.string(), vname, Api4::GetLastErrorMessage(mod));
                        }
                    }
                }
            }
            SKSE::log::info("Magelight[manifest]: '{}' ({}) v{} from {} — {} view(s) created{}",
                modId, name, version, folder, created, failed ? (", " + std::to_string(failed) + " failed") : "");
        }
        catch (const std::exception& e) {
            SKSE::log::error("Magelight[manifest]: {} — aborted ({}); other mods still load",
                             (modDir / "manifest.json").string(), e.what());
        }

    }  // namespace

    void LoadAll()
    {
        const fs::path root = GameRootPath() / L"Data" / L"Magelight";
        std::error_code ec;
        if (!fs::is_directory(root, ec)) {
            SKSE::log::info("Magelight[manifest]: no Data/Magelight folder — no manifest mods");
            return;
        }
        int found = 0;
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (!entry.is_directory(ec)) continue;
            if (!fs::exists(entry.path() / "manifest.json", ec)) continue;
            ++found;
            LoadOne(entry.path());
        }
        SKSE::log::info("Magelight[manifest]: {} manifest mod(s) under {}", found, root.string());
    }

}  // namespace Magelight::Manifest
