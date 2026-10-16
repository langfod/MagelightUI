/** What the host installs as `window.__MAGELIGHT__` at window-object-ready. */
export interface HostInfo {
  /** "0.15.0" */
  version: string;
  /** MAJOR*10000 + MINOR*100 + PATCH — compare against this, never parse the string */
  versionNumber: number;
  viewId: number;
  /** "" for views created through the v1-v3 API */
  modId: string;
  viewName: string;
  /** 1 = supported, 0 = not (`consoleLog` is a level, 0-3); names: `QueryCapability` in the host, or the SDK mock */
  capabilities: Record<string, number>;
  /** Magelight.json devMode: hot reload of page files, JS error banner on the page */
  dev: boolean;
  /** file:///.../Data/SKSE/Plugins/Magelight/ — images live under images/<name>.imgsrc */
  runtimeUrl: string;
}

/** The two verbs the host installs as `window.magelight` (the page core). */
export interface PageCore {
  send(channel: string, payload?: unknown): void;
  on(channel: string, fn: (arg: string) => void): () => void;
  off(channel: string, fn: (arg: string) => void): void;
  _dispatch(channel: string, arg: string): void;
  pending(channel: string): number;
  /** 0.29.0: play a UI sound through the game's audio (absent on older hosts, where `host.sound` does nothing) */
  sound?(name: string): void;
  /** 0.31.9: this view's colours for the host's VR keyboard and drawn cursor; `null` clears both (see `HostTheme`) */
  hostTheme?(theme: HostTheme | null): void;
}

/** `#rrggbb`, exactly: opaque, no shorthand, no alpha. Anything else refuses the whole theme. */
export type HexColor = `#${string}`;

/**
 * Magelight's own VR keyboard while this view holds UI mode (host 0.31.9). The first eight are required;
 * the host fills in the rest: action = accent when it has 3:1 contrast on key and keyHover (else text),
 * danger = a red with 3:1 on key and keyHover (else text), pressed = accent 20% over key, pressedText =
 * text (or black or white under 4.5:1 on pressed), barHover = panel and key half and half. A given
 * action/danger under 3:1 on key or keyHover, or pressedText under 4.5:1 on pressed, is replaced by its
 * fill-in. Refused when text has under 3:1 contrast on key, keyHover or panel.
 */
export interface KeyboardThemeColors {
  panel: HexColor;
  panelBorder: HexColor;
  key: HexColor;
  keyBorder: HexColor;
  keyHover: HexColor;
  text: HexColor;
  muted: HexColor;
  accent: HexColor;
  action?: HexColor;
  danger?: HexColor;
  pressed?: HexColor;
  /** the label of any key held down, and of Shift or Caps while on */
  pressedText?: HexColor;
  barHover?: HexColor;
}

/** The host's drawn cursor over this view (the same slot as C++ `SetViewCursorTint`); a colour left out keeps the host's. */
export interface CursorThemeColors {
  lit?: HexColor;
  shade?: HexColor;
  ink?: HexColor;
  glow?: HexColor;
  ibeam?: HexColor;
}

/**
 * What `magelight.hostTheme` sends. Each part: an object sets it, `null` clears it, left out keeps it.
 * One bad value refuses the whole message (the reason is in Magelight.log); at most 2 KB as JSON.
 */
export interface HostTheme {
  v: 1;
  keyboard?: KeyboardThemeColors | null;
  cursor?: CursorThemeColors | null;
}

declare global {
  interface Window {
    __MAGELIGHT__?: HostInfo;
    magelight?: PageCore;
    __mlNative?: (viewId: string, channel: string, arg: string) => void;
  }
}

export type Handler<T> = (value: T) => void;
export type Unsubscribe = () => void;
