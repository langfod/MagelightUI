#pragma once
// Key names accepted wherever a hotkey is written by hand (a manifest's
// "hotkey", Magelight.json "hotkeys" overrides): a DirectInput scancode
// number, or a name from this table. Shared so both spell the same set.

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdint>
#include <string>
#include <utility>

namespace Magelight::HotkeyNames {

    inline std::string Lower(std::string s)
    {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    inline constexpr const char* kAccepted =
        "a DirectInput scancode number, F1-F12, Insert/Home/PageUp/Delete/End/PageDown, Numpad0-9, "
        "Backslash/Grave/Minus/Equals, or 0 to disable";

    // 0 = disabled / invalid (err says which).
    inline std::uint32_t Parse(const nlohmann::json& v, std::string& err)
    {
        if (v.is_number_unsigned()) {
            const auto n = v.get<std::uint32_t>();
            if (n > 0xFF) { err = "scancode must be 0..255"; return 0; }
            return n;
        }
        if (!v.is_string()) { err = std::string("must be ") + kAccepted; return 0; }
        static const std::pair<const char*, std::uint32_t> kNames[] = {
            { "f1", 0x3B }, { "f2", 0x3C }, { "f3", 0x3D }, { "f4", 0x3E }, { "f5", 0x3F }, { "f6", 0x40 },
            { "f7", 0x41 }, { "f8", 0x42 }, { "f9", 0x43 }, { "f10", 0x44 }, { "f11", 0x57 }, { "f12", 0x58 },
            { "insert", 210 }, { "home", 199 }, { "pageup", 201 }, { "delete", 211 }, { "end", 207 },
            { "pagedown", 209 }, { "numpad0", 82 }, { "numpad1", 79 }, { "numpad2", 80 }, { "numpad3", 81 },
            { "numpad4", 75 }, { "numpad5", 76 }, { "numpad6", 77 }, { "numpad7", 71 }, { "numpad8", 72 },
            { "numpad9", 73 }, { "backslash", 43 }, { "grave", 41 }, { "minus", 12 }, { "equals", 13 },
            { "none", 0 }, { "off", 0 },
        };
        const std::string l = Lower(v.get<std::string>());
        for (const auto& [n, code] : kNames) if (l == n) return code;
        err = "unknown key name '" + l + "' (use " + kAccepted + ")";
        return 0;
    }

}  // namespace Magelight::HotkeyNames
