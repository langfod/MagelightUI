import type { HostInfo, HostTheme } from './types.js';

const absent: HostInfo = {
  version: '',
  versionNumber: 0,
  viewId: 0,
  modId: '',
  viewName: '',
  capabilities: {},
  dev: false,
  runtimeUrl: '',
};

function read(): HostInfo | undefined {
  return typeof window !== 'undefined' ? window.__MAGELIGHT__ : undefined;
}

/**
 * Who is hosting this page. Read lazily: the host installs its globals
 * before any page script runs, but a module evaluated in a plain browser
 * (or another Ultralight-based UI host) sees `present === false`.
 */
export const host = {
  get present(): boolean {
    return read() !== undefined;
  },
  get info(): HostInfo {
    return read() ?? absent;
  },
  get version(): string {
    return this.info.version;
  },
  get versionNumber(): number {
    return this.info.versionNumber;
  },
  get modId(): string {
    return this.info.modId;
  },
  get viewName(): string {
    return this.info.viewName;
  },
  get viewId(): number {
    return this.info.viewId;
  },
  get dev(): boolean {
    return this.info.dev;
  },
  /** `host.can('textureImage')` — 0/absent = no */
  can(capability: string): boolean {
    return (this.info.capabilities[capability] ?? 0) > 0;
  },
  /** at least `major.minor.patch` */
  atLeast(major: number, minor: number, patch = 0): boolean {
    return this.versionNumber >= major * 10000 + minor * 100 + patch;
  },
  /**
   * Play a UI sound through the game's audio (host 0.29.0; a page cannot on its
   * own — Ultralight has no media stack). Built-ins: 'ok'/'click', 'cancel',
   * 'prevnext', 'focus'/'hover', 'open', 'close', 'inactive' (the vanilla
   * UIMenu* sounds); any vanilla UI* / ITM* descriptor by EditorID
   * ('UIJournalOpen'); 'none'; or 'Plugin.esp|0xFormID' of any SNDR the mod
   * ships. Silent with no host and on hosts before 0.29.0 (the browser mock
   * logs it); `host.can('sound')` to feature-detect. Plain markup works too:
   * `data-ml-sound="click"`.
   */
  sound(name: string): void {
    const core = typeof window !== 'undefined' ? window.magelight : undefined;
    // No '__sound' send fallback: hosts before 0.29.0 have no such channel and log a warning per call.
    core?.sound?.(name);
  },
  /**
   * Give the host this view's colours (host 0.31.9): `keyboard` themes Magelight's own VR keyboard while
   * this view holds UI mode, `cursor` tints the host's drawn cursor over it. Send it again when your theme
   * changes; `null` gives the host's look back. Kept across a reload of the page, dropped with the view.
   * Does nothing with no host or on an older one; `host.can('keyboardTheme')` to feature-detect.
   */
  hostTheme(theme: HostTheme | null): void {
    const core = typeof window !== 'undefined' ? window.magelight : undefined;
    // No '__hosttheme' send fallback: hosts before 0.31.9 have no such channel and log a warning per call.
    core?.hostTheme?.(theme);
  },
  /** URL of a host-registered texture image (`RegisterTextureImage` name) */
  imageUrl(name: string): string {
    return this.info.runtimeUrl ? `${this.info.runtimeUrl}images/${name}.imgsrc` : '';
  },
};

/** Convenience: `if (isMagelight()) document.documentElement.classList.add('host-magelight')` */
export function isMagelight(): boolean {
  return host.present;
}
