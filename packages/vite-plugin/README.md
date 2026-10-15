# @magelight/vite-plugin

```ts
// vite.config.ts
import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import { magelight } from '@magelight/vite-plugin';

export default defineConfig({
  plugins: [react(), magelight({ entries: { config: 'index.html', hud: 'hud.html' } })],
});
```

`vite build` then writes `views/config/index.html` and `views/hud/index.html`, each with only the
assets that page reaches, ready to be named from `manifest.json` (`"path": "views/config/index.html"`).

What it sets for you: `base: './'` (file:// loading), es2022 output (the bundled Ultralight
2.0 engine parses through `es2022`; `es2019` was the older-SDK rule), no `crossorigin` on module tags (a null origin refuses CORS
loads → blank page), no modulepreload polyfill, per-entry pruning (the rule SeverActions'
`build-ui.ps1` enforced by hand).

Options: `entries`, `viewsDir` (`views`), `single` (`index`), `outDir` (`dist`), `keepDist`.
