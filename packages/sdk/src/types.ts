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
