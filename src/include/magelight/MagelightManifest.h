#pragma once
// Manifest mods: Data/Magelight/<ModId>/manifest.json + the pages it names.
// A folder IS a mod — its views exist from kDataLoaded with no DLL and no
// script; a DLL (RegisterMod with the same modId) or, later, Papyrus can
// adopt and drive them. Until a DLL adopts it the mod is script-owned, so
// the Papyrus tier may act on it. See docs/MANIFEST.md for the schema.

namespace Magelight::Manifest {

    // kDataLoaded, game thread, after settings. Scans every
    // Data/Magelight/*/manifest.json, registers each as a v4 mod and creates
    // its views (they materialize on the render thread once the world is up).
    // Every failure names the file; nothing here can fail the plugin.
    void LoadAll();

}  // namespace Magelight::Manifest
