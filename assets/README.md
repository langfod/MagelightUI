# assets/

Loose files staged into the runtime dir root (`Data/SKSE/Plugins/Magelight/`)
by `build.ps1`.

The flat cursor is drawn in code (`src/MagelightCursorArt.h`), so no cursor
art ships here. `Magelight.json` `"cursorFile"` still loads a custom image
(WIC-decodable PNG with alpha, up to 512x512) placed by `cursorHotspotX/Y`.
