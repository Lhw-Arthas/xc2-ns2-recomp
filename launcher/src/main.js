import './theme.css';
import './layout.css';
import './components.css';
import { listen } from '@tauri-apps/api/event';
import { api, esc, icon, refreshIcons, errText, toast } from './util.js';
import { t, initLang, getLang, setLang } from './i18n.js';
import * as play from './pages/play.js';
import * as mods from './pages/mods.js';
import * as settings from './pages/settings.js';
import * as setup from './pages/setup.js';
import * as about from './pages/about.js';
import { upd, notify, checkNow, available } from './update.js';

const PAGES = {
  play: { mod: play, key: 'nav.play', icon: 'play' },
  mods: { mod: mods, key: 'nav.mods', icon: 'package' },
  setup: { mod: setup, key: 'nav.setup', icon: 'hammer' },
  settings: { mod: settings, key: 'nav.settings', icon: 'settings' },
  about: { mod: about, key: 'nav.about', icon: 'info' },
};

const nav = document.getElementById('nav');
// The launcher's own configuration, kept here so the corner language switch can save it too (boot() fills it in).
let cfg = { dump_dir: '' };
const view = document.getElementById('view');
let current = null;
let statusHandler = null;
let setupHandler = null;
let logHandler = null;
let modsNeedsRebuild = false;

const ctx = {
  go,
  onStatus(fn) { statusHandler = fn; },
  onSetup(fn) { setupHandler = fn; },
  onLog(fn) { logHandler = fn; },
  setModsRebuild(v) {
    const next = Boolean(v);
    if (next !== modsNeedsRebuild) {
      modsNeedsRebuild = next;
      drawNav();
    }
  },
  /** Repaints the surrounding chrome only: window title, nav labels, language attribute. The open page keeps its state. */
  shell() { drawShell(); },
  /** Repaints the chrome and renders the open page again from disk. */
  redraw() { redrawForLang(); },
};

function drawNav() {
  nav.innerHTML =
    `<div class="brand"><span class="name">${esc(t('brand.name'))}</span></div>` +
    Object.entries(PAGES)
      .map(([id, p]) => {
        const dot = id === 'mods' && modsNeedsRebuild ? `<span class="nav-dot" title="${esc(t('mods.rebuildPendingNav'))}"></span>` : '';
        return `<button class="nav-item ${id === current ? 'active' : ''}" data-page="${id}">${icon(p.icon)}<span class="label">${esc(t(p.key))}</span>${dot}</button>`;
      })
      .join('') +
    (available() || upd.status.state === 'done'
      ? `<button class="nav-item update-pill" data-page="about">${icon('circle-arrow-up')}<span class="label">${esc(
          upd.status.state === 'done' ? t('nav.restartUpdate') : t('nav.updateTo', { version: upd.check.latest.version }),
        )}</span></button>`
      : '');
  nav.querySelectorAll('[data-page]').forEach((b) => b.addEventListener('click', () => go(b.dataset.page)));
  refreshIcons();
}

/** Repaints the chrome. Translations come from a language pack, so they are escaped like any other text. */
function drawShell() {
  document.title = t('window.title');
  nav.setAttribute('aria-label', t('nav.main'));
  drawNav();
  drawLangSwitch();
}

/**
 * The language switch in the top-right corner. The label is the language it switches *to*, so one click always does
 * what the button says. It writes the same two places the Settings page writes: the launcher's own config and the
 * in-game F1 menu's [gfx] ui_language, so the two never disagree.
 */
function drawLangSwitch() {
  let b = document.getElementById('lang-switch');
  if (!b) {
    b = document.createElement('button');
    b.id = 'lang-switch';
    b.className = 'lang-switch';
    b.type = 'button';
    b.addEventListener('click', async () => {
      const next = getLang() === 'zh' ? 'en' : 'zh';
      cfg = { ...cfg, lang: next };
      try { await api('save_config', { config: cfg }); } catch (e) { /* applies for this session anyway */ }
      try {
        await api('write_settings', { edits: [{ file: 'game', section: 'gfx', key: 'ui_language', value: next === 'zh' ? 'zh' : 'auto' }] });
      } catch (e) { /* nxrt.ini may not exist yet; the menu keeps its language */ }
      await setLang(next, () => redrawForLang());
    });
    document.body.appendChild(b);
  }
  const next = getLang() === 'zh' ? 'en' : 'zh';
  b.textContent = next === 'zh' ? '中文' : 'EN';
  b.title = next === 'zh' ? '切换到中文' : 'Switch to English';
  b.setAttribute('aria-label', b.title);
}

/** Redraws the shell and the open page after the interface language changed. */
function redrawForLang() {
  // Switching the language re-renders the page, which would otherwise throw the reader back to the top. Keep both
  // scroll positions (the window and the page container) and put them back after the render, and again on the next
  // frame because some pages fill themselves in asynchronously.
  const y = window.scrollY || document.scrollingElement?.scrollTop || 0;
  const view = document.getElementById('view');
  const viewY = view ? view.scrollTop : 0;
  drawShell();
  if (current) go(current);
  const restore = () => {
    window.scrollTo(0, y);
    if (view) view.scrollTop = viewY;
  };
  restore();
  requestAnimationFrame(restore);
}

async function go(page, opts = {}) {
  current = page;
  statusHandler = null;
  setupHandler = null;
  logHandler = null;
  drawNav();
  view.scrollTop = 0;
  try {
    await PAGES[page].mod.render(view, { ...ctx, ...opts });
  } catch (e) {
    view.innerHTML = `<div class="page"><div class="banner danger">${icon('circle-x')}<div>${errText(e)}</div></div></div>`;
    toast(errText(e), 'error');
  }
  refreshIcons();
}

async function boot() {
  await listen('game-status', (ev) => statusHandler && statusHandler(ev.payload));
  await listen('setup-status', (ev) => {
    setupHandler && setupHandler(ev.payload);
    if (ev.payload?.state === 'done' || ev.payload?.state === 'idle') {
      api('list_mods').then((s) => ctx.setModsRebuild(s.needs_rebuild)).catch(() => {});
    }
  });
  await listen('setup-log', (ev) => logHandler && logHandler(ev.payload));
  await listen('update-status', (ev) => {
    upd.status = ev.payload;
    notify();
  });
  upd.listeners.add(drawNav);
  try {
    cfg = await api('get_config');
  } catch (e) {
    toast(errText(e), 'error');
  }
  await initLang(() => redrawForLang(), cfg.lang);
  drawShell();
  api('list_mods').then((s) => ctx.setModsRebuild(s.needs_rebuild)).catch(() => {});
  // first run: no game files chosen yet
  go('setup', { firstRun: !cfg.dump_dir });
  if (!cfg.skip_update_check) checkNow(); // quietly: a failure only shows on the About page
}

boot();

export { redrawForLang, getLang };
