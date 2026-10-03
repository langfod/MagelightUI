// MagelightSound.h — host-played UI sounds (0.29.0).
//
// Ultralight has no media stack: <audio> and Web Audio do nothing, so a page
// cannot play a click on its own. The host plays
// through the game's own audio instead — BSAudioManager on the GAME THREAD —
// so volume and mute follow the player's UI audio settings for free.
//
// Names are built-ins (the vanilla UIMenu* descriptors, resolved by FormID —
// EditorID lookup is unreliable at runtime on VR without po3) or
// "Plugin.esp|0xFormID" for any SNDR a mod ships. No file playback on purpose:
// GetSoundHandleByFile would be a loose-path attack surface (SECURITY.md) and
// would bypass the audio settings.
#pragma once

#include <cstdint>
#include <string_view>

namespace Magelight::Sound
{
    // GAME THREAD ONLY. Resolves (cached per name for the session) and plays.
    // Unknown name: one warning per name per session, then silent.
    void Play(std::string_view name, std::uint64_t view);

    // Any thread. One hover sound per ~60 ms per view: a mouse sweep over a
    // list must not spam the engine (at most ~15 ticks/s).
    bool HoverAllowed(std::uint64_t view);
}
