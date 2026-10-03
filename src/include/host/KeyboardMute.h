#pragma once
// The keyboard mute — stage 4 of the split.
//
// One function, one file: InstallKeyboardMute(). It is the answer to a problem
// no other mechanism can solve, and the reasoning belongs with it. See the
// comment block at the top of KeyboardMute.cpp for the full story; the short
// version is that CLAUDE.md invariant 17 exists because of it.

namespace Magelight {

    // Patches the engine keyboard device's Poll vfunc so no keyboard button
    // event is generated while UI mode is on. Idempotent, and a no-op on a VR
    // runtime (there the input sink is the typing path). Called from
    // RegisterInputSink at kInputLoaded — as early as the device manager
    // exists, because sinks run in registration order and the mute only shields
    // the ones that come after.
    void InstallKeyboardMute();

}  // namespace Magelight