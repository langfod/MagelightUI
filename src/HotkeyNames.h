#pragma once
// Key names accepted wherever a hotkey is written by hand (a manifest's
// "hotkey", Magelight.json "hotkeys" overrides and "toggleKey"): a DirectInput scancode
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

    // A typed number that is not a key DirectInput reports, or one in Windows' F1-F12 key-code range
    // (112-123, often copied by mistake): the warning to log, with the scancode the player meant. "" = fine.
    inline std::string ScancodeWarning(std::uint32_t n)
    {
        if (n == 0) return {};
        if (n >= 112 && n <= 123) {
            static constexpr std::uint32_t kFn[12] = { 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58 };
            return std::to_string(n) + " is the Windows key code of F" + std::to_string(n - 111) +
                   "; its DirectInput scancode is " + std::to_string(kFn[n - 112]) + " (or write \"F" +
                   std::to_string(n - 111) + "\")";
        }
        static constexpr std::uint8_t kValidHigh[] = { 0x64, 0x65, 0x66, 0x70, 0x73, 0x79, 0x7B, 0x7D, 0x7E, 0x8D,
            0x90, 0x91, 0x92, 0x93, 0x94, 0x96, 0x97, 0x99, 0x9C, 0x9D, 0xA0, 0xA1, 0xA2, 0xA4, 0xAE, 0xB0,
            0xB2, 0xB3, 0xB5, 0xB7, 0xB8, 0xC5, 0xC7, 0xC8, 0xC9, 0xCB, 0xCD, 0xCF, 0xD0, 0xD1, 0xD2, 0xD3,
            0xDB, 0xDC, 0xDD, 0xDE, 0xDF, 0xE3, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED };
        if (n <= 0x58) return {};
        for (const auto v : kValidHigh) if (n == v) return {};
        return std::to_string(n) + " is not a key DirectInput reports, so it never fires";
    }

}  // namespace Magelight::HotkeyNames
