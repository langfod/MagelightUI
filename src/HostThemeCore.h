#pragma once
// Host themes (0.31.9): the colour rules for a keyboard theme a page (magelight.hostTheme) or a plugin
// (SetViewKeyboardTheme) gives a view. Engine-free, so tools/hosttheme-test runs them on the desktop.
// Colours here are 0xRRGGBB.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace Magelight::HostTheme {

    // The keyboard page's tokens, in KeyboardTheme's member order (api/MagelightUI_API.h): the page sets
    // --kb-<name> from each. The first kKbRequired are required, the rest are filled in (ResolveKeyboard).
    enum KbToken : int {
        kPanel, kPanelBorder, kKey, kKeyBorder, kKeyHover, kText, kMuted, kAccent,
        kAction, kDanger, kPressed, kPressedText, kBarHover, kKbTokens
    };
    inline constexpr int kKbRequired = kAction;
    inline constexpr std::array<const char*, kKbTokens> kKbNames = {
        "panel", "panelBorder", "key", "keyBorder", "keyHover", "text", "muted", "accent",
        "action", "danger", "pressed", "pressedText", "barHover"
    };
    // A page's cursor part, in CursorTint's member order.
    inline constexpr std::array<const char*, 5> kCursorNames = { "lit", "shade", "ink", "glow", "ibeam" };

    // Label colours need 3:1 (WCAG's floor for large text) on the keys they sit on, keyHover included: in VR the
    // laser hovers every key before it is pressed. pressedText needs 4.5:1 on pressed.
    inline constexpr double kKbLabelContrast = 3.0;
    inline constexpr double kKbPressedContrast = 4.5;

    // Exactly '#' and six hex digits: opaque, no shorthand, no alpha, nothing around it.
    constexpr bool ParseHex(std::string_view s, std::uint32_t& rgb)
    {
        if (s.size() != 7 || s[0] != '#') return false;
        std::uint32_t v = 0;
        for (std::size_t i = 1; i < s.size(); ++i) {
            const char c = s[i];
            std::uint32_t d = 0;
            if (c >= '0' && c <= '9') d = static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') d = static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = static_cast<std::uint32_t>(c - 'A' + 10);
            else return false;
            v = (v << 4) | d;
        }
        rgb = v;
        return true;
    }

    // `over` at pct percent over `under`, per sRGB channel, rounded half up (CSS color-mix in srgb).
    constexpr std::uint32_t Mix(std::uint32_t over, std::uint32_t under, int pct)
    {
        std::uint32_t out = 0;
        for (int shift = 16; shift >= 0; shift -= 8) {
            const int a = static_cast<int>((over >> shift) & 0xFF), b = static_cast<int>((under >> shift) & 0xFF);
            out |= static_cast<std::uint32_t>((b * (100 - pct) + a * pct + 50) / 100) << shift;
        }
        return out;
    }

    // WCAG 2 relative luminance, 0..1.
    inline double Luminance(std::uint32_t rgb)
    {
        const auto lin = [](std::uint32_t c8) {
            const double c = static_cast<double>(c8) / 255.0;
            return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * lin((rgb >> 16) & 0xFF) + 0.7152 * lin((rgb >> 8) & 0xFF) + 0.0722 * lin(rgb & 0xFF);
    }

    // WCAG 2 contrast ratio, 1..21.
    inline double Contrast(std::uint32_t a, std::uint32_t b)
    {
        const double la = Luminance(a), lb = Luminance(b);
        return la > lb ? (la + 0.05) / (lb + 0.05) : (lb + 0.05) / (la + 0.05);
    }

    inline std::string HexOf(std::uint32_t rgb)
    {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "#%06x", static_cast<unsigned>(rgb & 0xFFFFFFu));
        return buf;
    }

    // A keyboard theme as given: given[i] false = absent (a page left the key out, a plugin passed alpha 0).
    struct KbInput {
        std::array<std::uint32_t, kKbTokens> rgb{};
        std::array<bool, kKbTokens> given{};
    };
    using KbTheme = std::array<std::uint32_t, kKbTokens>;   // complete: every token set

    inline std::string RatioOf(double c)
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.2f:1", c);
        return buf;
    }

    // Checks a theme and fills in its optional tokens; out is complete only on true.
    //  - Refused (false, with why): a missing required token, or text under 3:1 on key, keyHover or panel.
    //  - Discarded and filled in (a line in notes): a given action or danger under 3:1 on key or keyHover, a given
    //    pressedText under 4.5:1 on pressed. An optional token never refuses the theme.
    //  - Warned (notes): accent under 3:1 on key. It is kept as given (it is also the pressed border and pressed's
    //    base); only the action fill-in stops copying it when it cannot be read.
    // notes is "; "-joined and empty when there is nothing to say.
    inline bool ResolveKeyboard(const KbInput& in, KbTheme& out, std::string& why, std::string* notes = nullptr)
    {
        for (int i = 0; i < kKbRequired; ++i) {
            if (!in.given[i]) {
                why = std::string("'") + kKbNames[i] + "' is required";
                return false;
            }
        }
        for (const int bg : { kKey, kKeyHover, kPanel }) {
            const double c = Contrast(in.rgb[kText], in.rgb[bg]);
            if (c < kKbLabelContrast) {
                why = "text " + HexOf(in.rgb[kText]) + " on " + kKbNames[bg] + " " + HexOf(in.rgb[bg]) + " has contrast " +
                      RatioOf(c) + ", under 3:1";
                return false;
            }
        }
        std::string said;
        const auto note = [&said](const std::string& line) { said += (said.empty() ? "" : "; ") + line; };
        KbTheme t = in.rgb;
        // The lower of a label's contrasts on key and on keyHover: the two backgrounds a label must read on.
        const auto onKeys = [&t](std::uint32_t label) {
            const double a = Contrast(label, t[kKey]), b = Contrast(label, t[kKeyHover]);
            return a < b ? a : b;
        };
        const double accentOnKey = Contrast(t[kAccent], t[kKey]);
        if (accentOnKey < kKbLabelContrast)
            note("accent " + HexOf(t[kAccent]) + " has contrast " + RatioOf(accentOnKey) + " on key, under 3:1 (Shift, "
                 "Caps and the title use it)");
        bool fillAction = !in.given[kAction], fillDanger = !in.given[kDanger], fillPressedText = !in.given[kPressedText];
        for (const int tok : { kAction, kDanger }) {
            if (!in.given[tok] || onKeys(t[tok]) >= kKbLabelContrast) continue;
            note(std::string(kKbNames[tok]) + " " + HexOf(t[tok]) + " has contrast " + RatioOf(onKeys(t[tok])) +
                 " on key or keyHover, under 3:1: filled in instead");
            (tok == kAction ? fillAction : fillDanger) = true;
        }
        if (fillAction) t[kAction] = onKeys(t[kAccent]) >= kKbLabelContrast ? t[kAccent] : t[kText];
        if (fillDanger) {
            constexpr std::uint32_t kReds[] = { 0xC0392Bu, 0xE74C3Cu };   // dark red for light keys, light red for dark
            const std::uint32_t red = onKeys(kReds[0]) >= onKeys(kReds[1]) ? kReds[0] : kReds[1];
            t[kDanger] = onKeys(red) >= kKbLabelContrast ? red : t[kText];
        }
        // Order matters: pressedText is judged against the final pressed.
        if (!in.given[kPressed]) t[kPressed] = Mix(t[kAccent], t[kKey], 20);
        if (!fillPressedText && Contrast(t[kPressedText], t[kPressed]) < kKbPressedContrast) {
            note("pressedText " + HexOf(t[kPressedText]) + " has contrast " + RatioOf(Contrast(t[kPressedText], t[kPressed])) +
                 " on pressed " + HexOf(t[kPressed]) + ", under 4.5:1: filled in instead");
            fillPressedText = true;
        }
        if (fillPressedText) {
            t[kPressedText] = t[kText];
            if (Contrast(t[kText], t[kPressed]) < kKbPressedContrast)
                t[kPressedText] = Contrast(0x000000u, t[kPressed]) >= Contrast(0xFFFFFFu, t[kPressed]) ? 0x000000u : 0xFFFFFFu;
        }
        if (!in.given[kBarHover]) t[kBarHover] = Mix(t[kPanel], t[kKey], 50);
        out = t;
        if (notes) *notes = said;
        return true;
    }

    // {"panel":"#1e1913",...}: every token, written from the numbers, never from text a page sent.
    inline std::string KeyboardJson(const KbTheme& t)
    {
        std::string out = "{";
        for (int i = 0; i < kKbTokens; ++i) {
            if (i) out += ',';
            out += '"';
            out += kKbNames[i];
            out += "\":\"";
            out += HexOf(t[i]);
            out += '"';
        }
        return out + "}";
    }

    namespace detail {
        constexpr std::uint32_t HexOr(std::string_view s, std::uint32_t bad)
        {
            std::uint32_t v = bad;
            return ParseHex(s, v) ? v : bad;
        }
    }
    static_assert(detail::HexOr("#1e1913", 1) == 0x1E1913u && detail::HexOr("#ABCdef", 1) == 0xABCDEFu);
    static_assert(detail::HexOr("#abc", 1) == 1 && detail::HexOr("1e1913a", 1) == 1 && detail::HexOr("#12345g", 1) == 1 &&
                  detail::HexOr(" #123456", 1) == 1 && detail::HexOr("#1234567", 1) == 1);
    static_assert(Mix(0xFFFFFFu, 0x000000u, 50) == 0x808080u && Mix(0xCBA560u, 0x292118u, 20) == 0x493B26u &&
                  Mix(0x123456u, 0xABCDEFu, 0) == 0xABCDEFu && Mix(0x123456u, 0xABCDEFu, 100) == 0x123456u);

}  // namespace Magelight::HostTheme
