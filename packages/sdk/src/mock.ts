import type { HostInfo, PageCore } from './types.js';

/**
 * A stand-in `window.magelight` for a plain browser: lets you develop a
 * page with `vite dev`, inject payloads on any channel from a floating
 * panel, and see what the page sent. Installed by `ensureCore()` when no
 * host is present; never installed in the game.
 */
export interface MockOptions {
  /** show the floating panel (default: true when a document exists) */
  panel?: boolean;
  /** pretend to be this host (default: a dev host with every capability) */
  info?: Partial<HostInfo>;
  /** called for every page → host send */
  onSend?: (channel: string, payload: string) => void;
}

export function installMock(opts: MockOptions = {}): PageCore {
  const subs: Record<string, Array<(arg: string) => void>> = {};
  const buf: Record<string, string[]> = {};
  const sent: Array<{ channel: string; payload: string }> = [];

  const info: HostInfo = {
    // Pretend to be a current host: version gates (host.atLeast, HostGate
    // minVersion) must pass so version-gated pages can be iterated in the
    // browser. Override with opts.info to test the gate UI itself.
    version: '99.0.0-mock',
    versionNumber: 990000,
    viewId: 1,
    modId: 'Mock',
    viewName: 'dev',
    // The canonical capability set: keep in step with the host's QueryCapability names.
    // Values are the browser-mock truth: no GPU/VR/native powers, the
    // always-on page/logic capabilities on, the browser's own network open
    // (loopback / networkPolicy). `mock` flags the mock itself.
    capabilities: { gpu: 0, textureImage: 0, clipPathHole: 0, pause: 1, events: 1, clipboard: 1,
                    networkDeny: 0, sessions: 1, manifest: 1, http: 0, vr: 0, hotkeys: 1, evaljs: 1,
                    pagebridge: 1, cutout: 0, hibernate: 0, inspector: 0, ime: 0, loopback: 1,
                    escapeCapture: 0, viewOrder: 0, scrollStep: 0, networkPolicy: 1, sound: 0, freezeWorld: 0, consoleLog: 0, mock: 1 },
    dev: true,
    runtimeUrl: '',
    ...opts.info,
  };

  const core: PageCore = {
    send(channel, payload) {
      const a = payload == null ? '' : typeof payload === 'string' ? payload : JSON.stringify(payload);
      sent.push({ channel, payload: a });
      opts.onSend?.(channel, a);
      log(`→ ${channel}(${a.length > 120 ? a.slice(0, 120) + '…' : a})`);
    },
    on(channel, fn) {
      (subs[channel] ??= []).push(fn);
      const b = buf[channel];
      if (b) {
        delete buf[channel];
        // Host parity: one throwing handler does not kill the rest of the
        // buffer or propagate into the subscribe call site.
        for (const arg of b) {
          try {
            fn(arg);
          } catch (e) {
            console.error(`[magelight mock] replay for ${channel} threw`, e);
          }
        }
      }
      return () => core.off(channel, fn);
    },
    off(channel, fn) {
      const s = subs[channel];
      if (!s) return;
      const i = s.indexOf(fn);
      if (i >= 0) s.splice(i, 1);
    },
    _dispatch(channel, arg) {
      const s = subs[channel];
      if (!s || !s.length) {
        (buf[channel] ??= []).push(arg);
        return;
      }
      for (const fn of s) {
        try {
          fn(arg);
        } catch (e) {
          console.error(`[magelight mock] handler for ${channel} threw`, e);
        }
      }
    },
    pending(channel) {
      return (buf[channel] ?? []).length;
    },
    // No audio in a browser mock — just say what would have played.
    sound(name) {
      log(`\u266a sound(${name})`);
    },
  };

  window.__MAGELIGHT__ = info;
  window.magelight = core;
  (window as unknown as { __MAGELIGHT_MOCK__: { sent: typeof sent; core: PageCore } }).__MAGELIGHT_MOCK__ = { sent, core };

  let logEl: HTMLElement | null = null;
  function log(line: string) {
    if (logEl) {
      const d = document.createElement('div');
      d.textContent = line;
      logEl.prepend(d);
      while (logEl.childElementCount > 40) logEl.lastElementChild?.remove();
    }
  }

  const wantPanel = opts.panel ?? typeof document !== 'undefined';
  if (wantPanel) {
    const mount = () => {
      if (!document.body || document.getElementById('__ml_mock')) return;
      const root = document.createElement('div');
      root.id = '__ml_mock';
      root.innerHTML =
        `<style>#__ml_mock{position:fixed;right:8px;bottom:8px;z-index:2147483000;width:340px;font:12px/1.4 system-ui,sans-serif;` +
        `background:#1e1e1e;color:#ddd;border:1px solid #555;border-radius:6px;box-shadow:0 4px 18px #0008}` +
        `#__ml_mock header{padding:6px 10px;background:#2c2c2c;border-radius:6px 6px 0 0;cursor:pointer;display:flex;justify-content:space-between}` +
        `#__ml_mock .b{padding:8px 10px;display:none}#__ml_mock.open .b{display:block}` +
        `#__ml_mock input,#__ml_mock textarea{width:100%;box-sizing:border-box;background:#111;color:#eee;border:1px solid #444;border-radius:3px;padding:4px;font:12px monospace;margin:2px 0}` +
        `#__ml_mock textarea{height:70px}#__ml_mock button{margin-top:4px;padding:4px 10px;background:#3a5;color:#fff;border:0;border-radius:3px;cursor:pointer}` +
        `#__ml_mock .log{margin-top:8px;max-height:140px;overflow:auto;font:11px monospace;color:#9c9;border-top:1px solid #333;padding-top:4px}</style>` +
        `<header><span>Magelight mock host</span><span>▴</span></header><div class="b">` +
        `<input placeholder="channel (e.g. pageData)"><textarea placeholder='payload (JSON or text)'></textarea>` +
        `<button>Dispatch to page</button><div class="log"></div></div>`;
      document.body.appendChild(root);
      const header = root.querySelector('header')!;
      header.addEventListener('click', () => root.classList.toggle('open'));
      const [chan] = Array.from(root.querySelectorAll('input'));
      const ta = root.querySelector('textarea')!;
      root.querySelector('button')!.addEventListener('click', () => {
        if (!chan.value) return;
        core._dispatch(chan.value, ta.value);
        log(`← ${chan.value}(${ta.value.length} chars)`);
      });
      logEl = root.querySelector('.log');
    };
    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', mount);
    else mount();
  }
  return core;
}
