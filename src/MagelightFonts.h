#pragma once

// The font loader Magelight gives Ultralight: the platform's, with the last-resort family made safe.
// Engine-free (Ultralight + Win32 only) so tools/font-test can run it without the game.

#include <Ultralight/platform/FontLoader.h>

namespace MagelightFonts
{
    // level 0 = info, 1 = warning. Called on the thread that asked for the font.
    using LogFn = void (*)(int level, const char* message);

    // The family name the bundled DejaVu Sans (src/fonts, an RCDATA resource) answers to.
    inline constexpr const char* kBundledFamily = "Magelight Fallback";

    // Wraps `platform` so the family Ultralight falls back to last always loads: WebCore
    // dereferences that font without a null check, so a system where it cannot be loaded
    // crashes the page. Also reads a font file whose path is not ASCII itself, and logs an
    // installed family the system loader returns nothing for, or whose file this loader read
    // and could not use. The loader lives for the process.
    ultralight::FontLoader* CreateLoader(ultralight::FontLoader* platform, LogFn log);
}
