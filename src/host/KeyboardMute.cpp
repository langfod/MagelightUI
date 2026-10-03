// The keyboard mute — stage 4 of the split. See KeyboardMute.h.
//
// ── Why this exists, and why it lives alone ─────────────────────────────────
//
// CLAUDE.md invariant 17: "A key typed into a page must not exist anywhere else
// in the engine." The sink unlink in the input section only shields the sinks
// registered AFTER ours. Mods with a "priority input hook" — Immersive
// Equipment Displays hooks the dispatcher itself and opens its UI on Backspace,
// EnableInMenus or not (field 2026-09-03) — see the keys before ANY sink. Two
// things the sink layer alone cannot do put the mute at the device instead:
// a sink cannot precede a dispatcher hook, and unlinking events only shields
// sinks registered later. So we skip the engine's Poll.

#include "host/KeyboardMute.h"
#include "host/State.h"

#include <RE/Skyrim.h>
#include <REL/Relocation.h>
#include <SKSE/SKSE.h>
#include <REX/W32.h>   // REX::W32::DIDEVICEOBJECTDATA / IDirectInputDevice8A

#include <cstdint>

#include "MagelightVR.h"

namespace Magelight {

    // ── Why it is here and not in the input sink ────────────────────────────
    //
    // The engine's keyboard device turns DirectInput state into button events
    // in its Poll (BSIInputDevice vfunc 02); while a page is focused that poll
    // is skipped, so NO keyboard button event exists engine-wide — nothing for
    // a hook, a sink, or a Papyrus OnKeyDown to act on. The page keeps typing
    // through the window proc (WM_KEYDOWN / WM_CHAR are the OS, not
    // DirectInput), and Escape / bound hotkeys keep working through the same
    // path. Mouse and gamepad devices are untouched. While muted the hook still
    // DRAINS DirectInput's buffered queue into the engine's state arrays (see
    // the body) — skipping the read outright let the typed keys replay into the
    // game on unfocus.

    namespace {
        using KeyboardPollFn = void(*)(RE::BSWin32KeyboardDevice*, float);
        KeyboardPollFn s_origKeyboardPoll = nullptr;

        void Hook_KeyboardPoll(RE::BSWin32KeyboardDevice* self, float dt)
        {
            // VR (0.17.3): the mute is NOT applied. Typing, Escape and the
            // close hotkey ride the window proc, which only delivers while
            // the game window owns OS focus — and the SkyrimVR desktop mirror
            // rarely does (field 2026-09-03: F10 opened the panel through the
            // sink, the mute then silenced the only path that could close
            // it). On VR the sink is the keyboard path: it handles Escape
            // and the view's own hotkey below and still unlinks every key
            // for the sinks after ours.
            if (s_focused.load() && !s_renderDead.load() && !VR::IsVRRuntime()) {
                // MUTED — but never idle. The engine reads DirectInput's BUFFERED
                // event queue (GetDeviceData, ten at a time) and applies it to
                // curState; a skipped Poll leaves every key typed into a page
                // queued in DirectInput, and the first real Poll after unfocus
                // replayed the lot into the game (field 2026-09-06: search-box
                // keystrokes opened journal/map/etc. the moment the menu closed).
                // Drain the queue ourselves and apply each event to BOTH state
                // arrays: the engine's held-key picture stays truthful, its
                // unmute diff has nothing to fire, and no button event is ever
                // generated for a key typed into a page. Both arrays carry
                // static offset asserts in CommonLib. (0.27.0)
                if (self) {
                    auto& rd = self->GetRuntimeData();
                    if (auto* dev = reinterpret_cast<REX::W32::IDirectInputDevice8A*>(rd.dInputDevice)) {
                        REX::W32::DIDEVICEOBJECTDATA buf[32];
                        for (int guard = 0; guard < 16; ++guard) {
                            std::uint32_t n = 32;
                            const auto hr = dev->GetDeviceData(sizeof(REX::W32::DIDEVICEOBJECTDATA), buf, &n, 0);
                            if (hr < 0 || n == 0) break;   // not acquired / input lost: nothing to drain
                            for (std::uint32_t i = 0; i < n; ++i) {
                                const std::uint32_t k = buf[i].ofs;
                                if (k < 0x100) rd.curState[k] = rd.prevState[k] = (buf[i].data & 0x80) ? 0x80 : 0;
                            }
                            if (n < 32) break;
                        }
                    }
                }
                return;
            }
            if (s_origKeyboardPoll) s_origKeyboardPoll(self, dt);
        }
    }  // namespace

    void InstallKeyboardMute()
    {
        static bool s_done = false;
        if (s_done) return;
        s_done = true;
        // NOT ON VR (0.19.2). The mute exists to stop a key typed into a
        // page reaching other mods, and on VR it is already skipped in
        // Hook_KeyboardPoll (0.17.3: the window gets no key messages there,
        // so the sink IS the typing path). Installing it anyway left a
        // permanent vtable patch on a VR runtime for no behaviour at all —
        // and a vtable write is the one thing in our always-on path that
        // could corrupt memory if the address-library id resolved wrongly.
        // The sink's unlink still contains keys while a page is focused.
        if (REL::Module::IsVR()) {
            SKSE::log::info("Magelight: keyboard device poll NOT hooked (VR — the sink is the typing path)");
            return;
        }
        REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_BSWin32KeyboardDevice[0] };
        s_origKeyboardPoll = reinterpret_cast<KeyboardPollFn>(vtbl.write_vfunc(0x2, &Hook_KeyboardPoll));
        SKSE::log::info("Magelight: keyboard device poll hooked (muted while UI mode is on)");
    }

}  // namespace Magelight