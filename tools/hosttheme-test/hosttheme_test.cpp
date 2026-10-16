// The colour rules of src/HostThemeCore.h (0.31.9 keyboard themes), on the desktop: build.bat builds and
// runs this; exit 0 when every check passes.

#include "HostThemeCore.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace Magelight::HostTheme;

static int s_failed = 0;

static void Check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++s_failed;
}

static bool Near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

// The eight required tokens of the host's own look (views/keyboard/index.html).
static KbInput HostLookRequired()
{
    KbInput in;
    const std::uint32_t req[kKbRequired] = { 0x1E1913, 0x5A4A2E, 0x292118, 0x3B3221, 0x35291C, 0xE8DCC4, 0x8A7A5A, 0xCBA560 };
    for (int i = 0; i < kKbRequired; ++i) {
        in.rgb[i] = req[i];
        in.given[i] = true;
    }
    return in;
}

int main()
{
    // ParseHex: exactly '#' and six hex digits.
    {
        std::uint32_t v = 0;
        Check(ParseHex("#1e1913", v) && v == 0x1E1913, "ParseHex reads #1e1913");
        Check(ParseHex("#ABCDEF", v) && v == 0xABCDEF, "ParseHex reads upper case");
        const char* bad[] = { "", "#", "#abc", "#abcd", "#12345", "#1234567", "123456", " #123456", "#123456 ",
                              "#12345g", "#-12345", "rgb(1,2,3)", "#12 456" };
        bool all = true;
        for (const char* b : bad) all = all && !ParseHex(b, v);
        Check(all, "ParseHex refuses shorthand, alpha, spaces and non-hex");
    }

    // WCAG contrast.
    {
        Check(Near(Contrast(0x000000, 0xFFFFFF), 21.0, 1e-9), "black on white is 21:1");
        Check(Near(Contrast(0x123456, 0x123456), 1.0, 1e-12), "a colour on itself is 1:1");
        Check(Near(Contrast(0x777777, 0xFFFFFF), 4.478, 0.005), "#777777 on white is about 4.48:1");
        Check(Contrast(0x111111, 0x222222) == Contrast(0x222222, 0x111111), "contrast is symmetric");
    }

    // Mix: `over` at pct percent over `under`.
    {
        Check(Mix(0xCBA560, 0x292118, 20) == 0x493B26, "accent 20% over key (host look) is #493b26");
        Check(Mix(0x1E1913, 0x292118, 50) == 0x241D16, "panel and key half and half (host look) is #241d16");
    }

    // Every token given and readable: kept exactly, and nothing to say.
    {
        KbInput in = HostLookRequired();
        const std::uint32_t opt[] = { 0xE0925A, 0xE74C3C, 0x4A3A24, 0xFFF3D8, 0x251D14 };
        for (int i = kKbRequired; i < kKbTokens; ++i) {
            in.rgb[i] = opt[i - kKbRequired];
            in.given[i] = true;
        }
        KbTheme t{};
        std::string why, notes = "x";
        const bool ok = ResolveKeyboard(in, t, why, &notes);
        Check(ok && t == in.rgb && notes.empty(), "every token given and readable: kept exactly, no notes");
    }

    // The host look in full: its own Close colour (#a3472b) has only 2.6:1 on its own key, so a theme that gives it
    // gets the filled-in red instead, and a note.
    {
        KbInput in = HostLookRequired();
        const std::uint32_t opt[] = { 0xE0925A, 0xA3472B, 0x4A3A24, 0xFFF3D8, 0x251D14 };
        for (int i = kKbRequired; i < kKbTokens; ++i) {
            in.rgb[i] = opt[i - kKbRequired];
            in.given[i] = true;
        }
        KbTheme t{};
        std::string why, notes;
        const bool ok = ResolveKeyboard(in, t, why, &notes);
        Check(ok && t[kDanger] == 0xE74C3C && notes.find("danger #a3472b") != std::string::npos,
              "a given danger under 3:1 on key is discarded, filled in and noted");
        Check(KeyboardJson(t) ==
                  "{\"panel\":\"#1e1913\",\"panelBorder\":\"#5a4a2e\",\"key\":\"#292118\",\"keyBorder\":\"#3b3221\","
                  "\"keyHover\":\"#35291c\",\"text\":\"#e8dcc4\",\"muted\":\"#8a7a5a\",\"accent\":\"#cba560\","
                  "\"action\":\"#e0925a\",\"danger\":\"#e74c3c\",\"pressed\":\"#4a3a24\",\"pressedText\":\"#fff3d8\","
                  "\"barHover\":\"#251d14\"}",
              "KeyboardJson writes all thirteen as #rrggbb, in order");
    }

    // Dark theme, required tokens only: the fill-in.
    {
        KbTheme t{};
        std::string why, notes = "x";
        const bool ok = ResolveKeyboard(HostLookRequired(), t, why, &notes);
        Check(ok && notes.empty(), "the host look's eight required colours resolve, no notes");
        Check(t[kAction] == 0xCBA560, "action = accent when accent has 3:1 on key and keyHover");
        Check(t[kPressed] == 0x493B26, "pressed = accent 20% over key");
        Check(t[kPressedText] == 0xE8DCC4, "pressedText = text when it has 4.5:1 on pressed");
        Check(t[kBarHover] == 0x241D16, "barHover = panel and key half and half");
        // #c0392b has under 3:1 on that dark key, #e74c3c over: the lighter red.
        Check(Contrast(0xC0392B, 0x292118) < 3.0 && t[kDanger] == 0xE74C3C, "danger on a dark key = #e74c3c");
    }

    // Light theme, required tokens only.
    {
        KbInput in;
        const std::uint32_t req[kKbRequired] = { 0xF4EFE6, 0xC9BFAE, 0xFFFFFF, 0xD8D0C2, 0xF0EAE0, 0x2B2620, 0x7A7064, 0x3060C0 };
        for (int i = 0; i < kKbRequired; ++i) {
            in.rgb[i] = req[i];
            in.given[i] = true;
        }
        KbTheme t{};
        std::string why;
        const bool ok = ResolveKeyboard(in, t, why);
        Check(ok, "a light theme resolves");
        Check(t[kDanger] == 0xC0392B, "danger on a white key = #c0392b (more contrast than #e74c3c)");
        Check(t[kAction] == 0x3060C0, "a dark accent on light keys is action too");
        Check(t[kPressed] == Mix(0x3060C0, 0xFFFFFF, 20), "pressed = accent 20% over a white key");
        Check(t[kPressedText] == 0x2B2620, "dark text stays on a light pressed key");
        Check(Contrast(t[kPressedText], t[kPressed]) >= 4.5, "pressedText has 4.5:1 on pressed");
    }

    // A light theme that keeps the host's brass accent: accepted, accent noted, the action keys take text.
    {
        KbInput in;
        const std::uint32_t req[kKbRequired] = { 0xFAF5EA, 0xC9BFAE, 0xF5ECD8, 0xD8D0C2, 0xECE0C6, 0x2B2620, 0x7A7064, 0xCBA560 };
        for (int i = 0; i < kKbRequired; ++i) {
            in.rgb[i] = req[i];
            in.given[i] = true;
        }
        KbTheme t{};
        std::string why, notes;
        const bool ok = ResolveKeyboard(in, t, why, &notes);
        Check(ok && notes.find("accent #cba560") != std::string::npos, "brass accent on light keys: kept, with a note");
        Check(ok && t[kAccent] == 0xCBA560 && t[kAction] == 0x2B2620, "action = text when accent has under 3:1 on key");
        Check(ok && t[kDanger] == 0xC0392B, "the darker red on light keys");
    }

    // An accent the same as key (the review's case, with a readable keyHover): accepted with a note, and nothing
    // that the fill-in sets is drawn in it.
    {
        KbInput in = HostLookRequired();
        in.rgb[kAccent] = 0x292118;
        KbTheme t{};
        std::string why, notes;
        const bool ok = ResolveKeyboard(in, t, why, &notes);
        Check(ok && notes.find("accent") != std::string::npos && t[kAction] == 0xE8DCC4 && t[kDanger] == 0xE74C3C,
              "accent = key: noted; action = text, danger a red");
    }

    // A given action that reads on key but not on keyHover is discarded; accent fails keyHover too, so text.
    {
        KbInput in = HostLookRequired();
        in.rgb[kKeyHover] = 0x6A5A40;
        in.rgb[kAction] = 0xB07040;
        in.given[kAction] = true;
        KbTheme t{};
        std::string why, notes;
        const bool ok = ResolveKeyboard(in, t, why, &notes);
        const bool premise = Contrast(0xB07040, 0x292118) >= 3.0 && Contrast(0xB07040, 0x6A5A40) < 3.0 &&
                             Contrast(0xCBA560, 0x6A5A40) < 3.0;
        Check(ok && premise && t[kAction] == 0xE8DCC4 && notes.find("action #b07040") != std::string::npos,
              "a given action under 3:1 on keyHover is discarded and filled in with text");
    }

    // The danger fill-in is judged on keyHover as well: here both reds read on the white key, neither on the
    // grey hover, so Close takes text.
    {
        KbInput in;
        const std::uint32_t req[kKbRequired] = { 0xF0F0F0, 0x808080, 0xFFFFFF, 0xC0C0C0, 0xA0A0A0, 0x000000, 0x505050, 0x1A4A8A };
        for (int i = 0; i < kKbRequired; ++i) {
            in.rgb[i] = req[i];
            in.given[i] = true;
        }
        KbTheme t{};
        std::string why;
        const bool ok = ResolveKeyboard(in, t, why);
        const bool premise = Contrast(0xC0392B, 0xFFFFFF) >= 3.0 && Contrast(0xC0392B, 0xA0A0A0) < 3.0 &&
                             Contrast(0xE74C3C, 0xA0A0A0) < 3.0;
        Check(ok && premise && t[kDanger] == 0x000000, "danger = text when no red has 3:1 on keyHover");
    }

    // pressedText: a given one under 4.5:1 on pressed is discarded; the fill-in falls back to black or white.
    {
        KbInput in = HostLookRequired();
        in.rgb[kPressed] = 0x4A3A24;
        in.given[kPressed] = true;
        in.rgb[kPressedText] = 0x5A4A2E;   // given, dark on dark
        in.given[kPressedText] = true;
        KbTheme t{};
        std::string why, notes;
        const bool ok = ResolveKeyboard(in, t, why, &notes);
        Check(ok && t[kPressedText] == 0xE8DCC4 && notes.find("pressedText #5a4a2e") != std::string::npos,
              "a given pressedText under 4.5:1 on pressed is discarded and filled in");

        KbInput light = HostLookRequired();
        light.rgb[kKey] = 0xFFFFFF;
        light.rgb[kKeyHover] = 0xEEEEEE;
        light.rgb[kPanel] = 0xF0F0F0;
        light.rgb[kText] = 0x404040;
        light.rgb[kPressed] = 0x303030;   // given, dark: dark text cannot be read on it
        light.given[kPressed] = true;
        const bool ok2 = ResolveKeyboard(light, t, why);
        Check(ok2 && t[kPressed] == 0x303030, "a given pressed is kept");
        Check(ok2 && t[kPressedText] == 0xFFFFFF, "pressedText = white on a dark pressed with dark text");
        light.rgb[kPressed] = 0xE0E0E0;
        light.rgb[kText] = 0x505050;
        const bool ok3 = ResolveKeyboard(light, t, why);
        Check(ok3 && t[kPressedText] == 0x505050, "pressedText = text when it reads on pressed");
    }

    // danger falls back to text when neither red reaches 3:1 on key.
    {
        KbInput in = HostLookRequired();
        in.rgb[kKey] = 0x8A5050;   // a mid red-brown key: both reds sit near it
        in.rgb[kPanel] = 0x101010;
        in.rgb[kText] = 0xFFFFFF;
        KbTheme t{};
        std::string why;
        const bool ok = ResolveKeyboard(in, t, why);
        const bool noRed = Contrast(0xC0392B, 0x8A5050) < 3.0 && Contrast(0xE74C3C, 0x8A5050) < 3.0;
        Check(ok && noRed && t[kDanger] == 0xFFFFFF, "danger = text when no red has 3:1 on key");
    }

    // Refusals.
    {
        KbTheme t{};
        std::string why;
        for (int i = 0; i < kKbRequired; ++i) {
            KbInput in = HostLookRequired();
            in.given[i] = false;
            const bool ok = ResolveKeyboard(in, t, why);
            const std::string want = std::string("'") + kKbNames[i] + "' is required";
            if (ok || why != want) {
                Check(false, ("a missing " + std::string(kKbNames[i]) + " is refused by name").c_str());
                break;
            }
            if (i == kKbRequired - 1) Check(true, "each missing required token is refused by name");
        }
        KbInput low = HostLookRequired();
        low.rgb[kText] = 0x3A3020;   // dark text on a dark key
        Check(!ResolveKeyboard(low, t, why) && why.find("on key") != std::string::npos && why.find("under 3:1") != std::string::npos,
              "text under 3:1 on key is refused, and says so");
        KbInput lowPanel = HostLookRequired();
        lowPanel.rgb[kPanel] = 0xD0C8B8;   // the key stays dark, the panel is light under light text
        Check(!ResolveKeyboard(lowPanel, t, why) && why.find("on panel") != std::string::npos,
              "text under 3:1 on panel is refused, and says so");
        // The review's case: keyHover the colour of text, so the aimed key would be a blank block.
        KbInput hover = HostLookRequired();
        hover.rgb[kKeyHover] = 0xE8DCC4;
        hover.rgb[kAccent] = 0x292118;
        Check(!ResolveKeyboard(hover, t, why) && why.find("on keyHover") != std::string::npos,
              "text under 3:1 on keyHover is refused, and says so");
    }

    if (s_failed) {
        std::printf("\n%d check(s) FAILED\n", s_failed);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}
