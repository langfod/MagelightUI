#pragma once
// Papyrus tier: the `Magelight` script (papyrus/Magelight.psc). A thin
// consumer of the v4 registry living inside the host — scripts register a
// mod by slug (or adopt a manifest mod), create/drive views, and hear
// host events as ModEvents (`Magelight_<Event>`, strArg = modId[|detail],
// numArg = view). JS -> Papyrus rides `Magelight_JS_<listener>`.
// Papyrus has no caller identity, so a script acts only on script-owned
// mods with a storage jar of their own, and their views
// (Api4::ScriptAccessOfMod); a plugin's slug or view is refused. See
// docs/PAPYRUS.md.

namespace Magelight::Papyrus {

    // Registers the natives with SKSE's Papyrus interface. Call from
    // SKSEPluginLoad, before the runtime check — a script must always find
    // its natives, even on a session where the overlay is inert.
    void Register();

}  // namespace Magelight::Papyrus
