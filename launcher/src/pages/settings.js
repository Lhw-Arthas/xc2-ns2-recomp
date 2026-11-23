import { api, esc, icon, refreshIcons, toast, errText } from '../util.js';
import { t, getLang, listLangs, setLang } from '../i18n.js';
import { fileDetail } from '../rusttext.js';

/*
 * Settings schema. Every key is described in runtime/docs/CONFIG.md.
 *   file: "game" (fefw.ini), "controls" (controls.ini, written by the game on its first start) or "config" (the launcher's own options)
 * Types: select, toggle, size, autonum, range, path, text, number, language. A toggle with `on` / `off` writes those words
 * instead of 1 / 0 (`off`, 0, false or no read as off, anything else as on).
 * Text is a key into src/locales: `label` and `hint` are always translated, and an option name that contains a dot is a key
 * too (a plain name such as "720p" or "DLAA" is shown as it is).
 */
// The WebKitGTK launcher on Linux (the Windows one is WebView2)
const IS_LINUX = /Linux/.test(navigator.userAgent) && !/Android/.test(navigator.userAgent);

const SECTIONS = [
  {
    title: 'settings.sec.picture', icon: 'monitor', file: 'game',
    fields: [
      // [am] resolution is "WxH"; empty means the console's own default for the operation mode (1920x1080 docked,
      // 1280x720 handheld). The runtime caps the internal render at 1440p whatever is chosen here.
      { id: 'resolution', type: 'select', label: 'settings.resolution.label', sec: 'am', key: 'resolution', def: '',
        options: [['', 'settings.resolution.auto'], ['1280x720', '720p'], ['1920x1080', '1080p'],
          ['2560x1440', '1440p'], ['3840x2160', '2160p (4K)']],
        hint: 'settings.resolution.hint' },
      { id: 'operation_mode', type: 'select', label: 'settings.opMode.label', sec: 'am', key: 'operation_mode', def: 'docked',
        options: [['docked', 'settings.opMode.docked'], ['handheld', 'settings.opMode.handheld']],
        hint: 'settings.opMode.hint' },
      { id: 'upscaler', type: 'select', label: 'settings.upscaler.label', sec: 'dlss', key: 'upscaler', def: 'taau',
        options: [['taau', 'settings.upscaler.taau'], ['fsr3', 'settings.upscaler.fsr3'], ['dlss', 'settings.upscaler.dlss'],
          ['xess', 'settings.upscaler.xess'], ['auto', 'settings.upscaler.auto']],
        hint: 'settings.upscaler.hint',
        // the DLSS files (NVIDIA's, not in the package) and the button that fetches them, while DLSS is chosen for upscaling or frames
        status: 'dlss', showStatus: (m) => m.upscaler === 'dlss' || m.upscaler === 'auto' || m.frame_gen === 'dlss' },
      { id: 'lockRenderSize', type: 'toggle', label: 'settings.lockRenderSize.label', sec: 'upscale', key: 'lock_render_size', def: '0',
        hint: 'settings.lockRenderSize.hint' },
      { id: 'fsr_dll', type: 'path', label: 'settings.fsrDll.label', sec: 'upscale', key: 'fsr_dll', def: '', placeholder: 'settings.fsrDll.placeholder',
        hint: 'settings.fsrDll.hint',
        status: 'fsr', showStatus: (m) => m.upscaler === 'fsr3' || m.upscaler === 'auto' || m.frame_gen === 'fsr3' },
      { id: 'frame_gen', type: 'select', label: 'settings.frameGen.label', sec: 'dlss', key: 'frame_gen', def: '0',
        options: [['0', 'settings.frameGen.off'], ['dlss', 'settings.frameGen.dlss'], ['fsr3', 'settings.frameGen.fsr3']],
        hint: 'settings.frameGen.hint' },
      { id: 'hdr_mode', type: 'select', label: 'settings.hdr.label', sec: 'hdr', key: 'mode', def: 'auto',
        options: [['auto', 'settings.hdr.auto'], ['on', 'settings.hdr.on'], ['native', 'settings.hdr.native'], ['off', 'settings.hdr.off']],
        hint: 'settings.hdr.hint' },
      // FIFO is the default and is written explicitly: an empty value left the mode to [display] vsync, which the page
      // does not offer, so the entry labelled "FIFO" produced Immediate (issue 12).
      { id: 'present_mode', type: 'select', label: 'settings.presentMode.label', sec: 'gfx', key: 'present_mode', def: 'fifo',
        options: [['fifo', 'settings.presentMode.default'], ['fifo_relaxed', 'settings.presentMode.vrr'],
          ['mailbox', 'mailbox'], ['immediate', 'immediate']],
        hint: 'settings.presentMode.hint' },
    ],
  },
  {
    title: 'settings.sec.frameRate', icon: 'gauge', file: 'game',
    fields: [
      // [xc2] fps: 0 keeps the game's own 30 Hz step, 60/120 make the runtime drive its limiter faster. The game paces
      // itself by real time, so this changes the smoothness, not the speed (handover 132).
      { id: 'fps', type: 'select', label: 'settings.fps.label', sec: 'xc2', key: 'fps', def: '60',
        options: [['0', 'settings.fps.native'], ['60', '60'], ['120', '120']],
        hint: 'settings.fps.hint' },
      { id: 'vsync_cap', type: 'toggle', label: 'settings.vsyncCap.label', sec: 'latency', key: 'vsync_cap', def: '1',
        hint: 'settings.vsyncCap.hint' },
    ],
  },
  {
    title: 'settings.sec.sound', icon: 'volume-2', file: 'game',
    fields: [
      { id: 'audio_device', type: 'text', label: 'settings.audioDevice.label', sec: 'audio', key: 'device', def: 'default',
        placeholder: 'settings.audioDevice.placeholder', hint: 'settings.audioDevice.hint' },
      { id: 'out_latency', type: 'number', label: 'settings.audioLatency.label', sec: 'audio', key: 'out_latency_ms', def: '30',
        min: 5, max: 200, hint: 'settings.audioLatency.hint' },
      { id: 'jc2_mouse', type: 'select', label: 'settings.jc2Mouse.label', sec: 'input', key: 'jc2_mouse', def: 'auto',
        options: [['auto', 'settings.jc2Mouse.auto'], ['on', 'settings.jc2Mouse.on'], ['off', 'settings.jc2Mouse.off']], hint: 'settings.jc2Mouse.hint' },
      { id: 'jc2_clicks', type: 'select', label: 'settings.jc2Clicks.label', sec: 'input', key: 'jc2_clicks', def: 'both',
        options: [['both', 'settings.jc2Clicks.both'], ['device', 'settings.jc2Clicks.device']], hint: 'settings.jc2Clicks.hint' },
    ],
  },
  {
    title: 'settings.sec.input', icon: 'folder', file: 'game',
    fields: [
      { id: 'save_dir', type: 'path', label: 'settings.saveDir.label', sec: 'fs', key: 'save_dir', def: '', placeholder: 'settings.saveDir.placeholder' },
      { id: 'controls', type: 'path', label: 'settings.controls.label', sec: 'play', key: 'controls', def: 'controls.ini',
        placeholder: 'settings.controls.placeholder', hint: 'settings.controls.hint' },
      { id: 'log_path', type: 'path', label: 'settings.logPath.label', sec: 'log', key: 'path', def: 'nxrt.log',
        placeholder: 'settings.logPath.placeholder', hint: 'settings.logPath.hint' },
      // The game's own language: it follows the console's, so the value is a Switch code (zh-CN is a Windows locale
      // The game's own text language (AmericanEnglish runs as BritishEnglish). Voice is Japanese and English whatever is chosen.
      { id: 'game_lang', type: 'select', label: 'settings.gameLang.label', sec: 'system', key: 'language', def: 'auto',
        // Order is a statement, not alphabetical: Simplified first, Traditional next, then the two English entries,
        // then Japanese, then the rest. Automatic (follow the Windows language) sits last.
        options: [['SimplifiedChinese', 'settings.gameLang.simplifiedChinese'],
                  ['TraditionalChinese', 'settings.gameLang.traditionalChinese'],
                  ['AmericanEnglish', 'settings.gameLang.americanEnglish'],
                  ['BritishEnglish', 'settings.gameLang.britishEnglish'],
                  ['Japanese', 'settings.gameLang.japanese'],
                  ['French', 'settings.gameLang.french'],
                  ['German', 'settings.gameLang.german'],
                  ['Italian', 'settings.gameLang.italian'],
                  ['Spanish', 'settings.gameLang.spanish'],
                  ['LatinAmericanSpanish', 'settings.gameLang.latinSpanish'],
                  ['Korean', 'settings.gameLang.korean'],
                  ['auto', 'settings.gameLang.auto']],
        hint: 'settings.gameLang.hint' },
    ],
  },
  {
    title: 'settings.sec.launcher', icon: 'wrench', file: 'config',
    fields: [
      { id: 'lang', type: 'language', label: 'settings.lang.label', hint: 'settings.lang.hint' },
      { id: 'repo_dir', type: 'path', label: 'settings.repoDir.label', sec: '', key: 'repo_dir', def: '', placeholder: 'settings.repoDir.placeholder', hint: 'settings.repoDir.hint' },
      { id: 'game_dir', type: 'path', label: 'settings.gameDir.label', sec: '', key: 'game_dir', def: '', placeholder: 'settings.gameDir.placeholder', hint: 'settings.gameDir.hint' },
      { id: 'jobs', type: 'number', label: 'settings.jobs.label', sec: '', key: 'jobs', def: '0', min: 0, max: 256, hint: 'settings.jobs.hint' },
      { id: 'minimize_on_play', type: 'toggle', label: 'settings.minimizeOnPlay.label', sec: '', key: 'minimize_on_play', def: '0' },
    ],
  },
];

const truthy = (v) => ['1', 'true', 'yes', 'on'].includes(String(v).trim().toLowerCase());
const clamp = (n, lo, hi) => Math.min(hi, Math.max(lo, n));
const fwd = (s) => String(s).trim().replace(/\\/g, '/');

/** An option label: a dotted name is a key, anything else ("720p", "DLAA") is shown as written. */
const optName = (label) => (String(label).includes('.') ? t(label) : label);

export async function render(el, ctx) {
  const [cfg, status] = await Promise.all([api('get_config'), api('game_status')]);
  const data = await api('read_settings').catch(() => ({ game_dir: '', game: {}, controls: {}, controls_exists: false }));
  const files = { game: data.game, controls: data.controls, config: { repo_dir: cfg.repo_dir, game_dir: cfg.game_dir, jobs: String(cfg.jobs || 0), minimize_on_play: cfg.minimize_on_play ? '1' : '0', runner: cfg.runner || '' } };
  const exists = { game: true, controls: data.controls_exists, config: true };
  const keyOf = (f, k) => (f.file === 'config' ? k : `${f.sec}.${k}`);
  const get = (f, k, def) => files[f.file][keyOf(f, k)] ?? def;

  // ----- model: one UI value per field -----
  const READ = {
    select: (f) => get(f, f.key, f.def),
    toggle: (f) => (f.off !== undefined ? ![f.off, '0', 'false', 'no'].includes(String(get(f, f.key, f.def)).trim().toLowerCase()) : truthy(get(f, f.key, f.def))),
    size: (f) => ({ w: get(f, f.keys[0], f.def[0]), h: get(f, f.keys[1], f.def[1]) }),
    autonum: (f) => {
      const v = String(get(f, f.key, f.def));
      return v.toLowerCase() === 'auto' ? { auto: true, num: String(f.numDefault) } : { auto: false, num: v };
    },
    range: (f) => Math.round(clamp(parseFloat(get(f, f.key, f.def)) || 0, 0, 1) * 100),
    path: (f) => get(f, f.key, f.def),
    text: (f) => get(f, f.key, f.def),
    number: (f) => String(get(f, f.key, f.def)),
    language: () => getLang(),
  };
  const WRITE = {
    select: (f, x) => [[f.sec, f.key, x]],
    toggle: (f, x) => [[f.sec, f.key, x ? (f.on ?? '1') : (f.off ?? '0')]],
    size: (f, x) => [
      [f.sec, f.keys[0], String(clamp(parseInt(x.w, 10) || 1280, 320, 16384))],
      [f.sec, f.keys[1], String(clamp(parseInt(x.h, 10) || 720, 320, 16384))],
    ],
    autonum: (f, x) => [[f.sec, f.key, x.auto ? 'auto' : String(clamp(Math.round(parseFloat(x.num)) || f.numDefault, f.min, f.max))]],
    range: (f, x) => [[f.sec, f.key, (x / 100).toFixed(2)]],
    path: (f, x) => [[f.sec, f.key, fwd(x)]],
    text: (f, x) => [[f.sec, f.key, String(x).trim()]],
    number: (f, x) => [[f.sec, f.key, String(clamp(parseInt(x, 10) || 0, f.min, f.max))]],
    language: () => [],
  };

  const all = SECTIONS.flatMap((s) => s.fields.map((f) => ({ ...f, file: s.file })));
  const model = {};
  const orig = {};
  for (const f of all) {
    model[f.id] = READ[f.type](f);
    orig[f.id] = JSON.stringify(model[f.id]);
  }
  const dirtyFields = () => all.filter((f) => JSON.stringify(model[f.id]) !== orig[f.id]);

  // ----- markup -----
  const dis = (f) => (exists[f.file] ? '' : 'disabled');
  const CONTROL = {
    select: (f, x) => {
      const opts = f.options.some(([v]) => String(v) === String(x)) ? f.options : [...f.options, [x, x]];
      return `<select data-f="${f.id}" ${dis(f)}>${opts.map(([v, l]) => `<option value="${esc(v)}" ${String(v) === String(x) ? 'selected' : ''}>${esc(optName(l))}</option>`).join('')}</select>`;
    },
    toggle: (f, x) => `<label class="switch"><input type="checkbox" data-f="${f.id}" ${x ? 'checked' : ''} ${dis(f)}>${esc(t(x ? f.onLabel || 'common.on' : f.offLabel || 'common.off'))}</label>`,
    size: (f, x) => `<input type="number" data-f="${f.id}" data-part="w" min="320" max="16384" value="${esc(x.w)}" ${dis(f)}><span class="unit">x</span><input type="number" data-f="${f.id}" data-part="h" min="320" max="16384" value="${esc(x.h)}" ${dis(f)}>`,
    autonum: (f, x) => `<label class="switch"><input type="checkbox" data-f="${f.id}" data-part="auto" ${x.auto ? 'checked' : ''} ${dis(f)}>${esc(optName(f.autoLabel))}</label><input type="number" data-f="${f.id}" data-part="num" min="${f.min}" max="${f.max}" value="${esc(x.num)}" ${x.auto || !exists[f.file] ? 'disabled' : ''}><span class="unit">${esc(optName(f.unit))}</span>`,
    range: (f, x) => `<input type="range" data-f="${f.id}" min="0" max="100" step="1" value="${x}" ${dis(f)}><span class="unit" data-readout="${f.id}" style="min-width: 3.5ch; text-align: right">${x}${esc(optName(f.unit || '%'))}</span>`,
    text: (f, x) => `<input type="text" data-f="${f.id}" spellcheck="false" placeholder="${esc(f.placeholder ? optName(f.placeholder) : '')}" value="${esc(x)}" ${dis(f)}>`,
    number: (f, x) => `<input type="number" data-f="${f.id}" min="${f.min}" max="${f.max}" value="${esc(x)}" ${dis(f)}>`,
    path: (f, x) => `<input type="text" data-f="${f.id}" spellcheck="false" placeholder="${esc(f.placeholder ? optName(f.placeholder) : '')}" value="${esc(x)}" ${dis(f)}><button class="btn" data-browse="${f.id}" ${dis(f)}>${icon('folder-open')}${esc(t('common.browse'))}</button>`,
    language: (f, x) => `<select data-f="${f.id}">${listLangs().map((l) => `<option value="${esc(l.code)}" ${l.code === x ? 'selected' : ''}>${esc(l.name)}</option>`).join('')}</select>`,
  };

  const fieldHtml = (f) => `
    <div class="field">
      <label class="name">${esc(t(f.label))}${f.hint ? `<span class="hint">${esc(t(f.hint))}</span>` : ''}${f.status && (!f.showStatus || f.showStatus(model)) ? `<span class="hint" data-status="${f.status}">${esc(t('settings.files.checking'))}</span>` : ''}</label>
      <div class="control">${CONTROL[f.type](f, model[f.id])}</div>
    </div>`;

  const barText = (n) => (n ? (n > 1 ? t('common.unsavedMany', { count: n }) : t('common.unsavedOne')) : t('common.noChanges'));

  function draw() {
    const n = dirtyFields().length;
    el.innerHTML = `
      <div class="page">
        <div class="hero">
          <h1>${esc(t('settings.title'))}</h1>
          <p class="dim">${esc(t('settings.introBefore'))}<code>${esc(data.game_dir)}</code>${esc(t('settings.introAfter'))}</p>
        </div>
        ${status.state === 'running' ? `<div class="banner warn">${icon('triangle-alert')}<div>${esc(t('settings.gameRunning'))}</div></div>` : ''}
        ${!data.controls_exists ? `<div class="banner info">${icon('info')}<div>${esc(t('settings.noControlsBefore'))}<code>${esc(data.game_dir)}</code>${esc(t('settings.noControlsAfter'))}</div></div>` : ''}
        ${SECTIONS.map((s) => `<div class="card"><h2>${icon(s.icon)}${esc(t(s.title))}</h2>${s.fields.map((f) => fieldHtml({ ...f, file: s.file })).join('')}</div>`).join('')}
        <div class="savebar">
          <span class="dim">${esc(barText(n))}</span>
          <div class="row">
            <button class="btn quiet" id="revert" ${n ? '' : 'disabled'}>${icon('undo-2')}${esc(t('common.revert'))}</button>
            <button class="btn primary" id="save" ${n ? '' : 'disabled'}>${icon('save')}${esc(t('common.save'))}</button>
          </div>
        </div>
      </div>`;
    refreshIcons();
    bind();
    checkFiles();
  }

  /** Puts the (unsaved) model back into freshly drawn markup. */
  function applyModel() {
    for (const f of all) {
      const x = model[f.id];
      if (f.type === 'size') {
        const w = el.querySelector(`[data-f="${f.id}"][data-part="w"]`);
        const h = el.querySelector(`[data-f="${f.id}"][data-part="h"]`);
        if (w) w.value = x.w;
        if (h) h.value = x.h;
      } else if (f.type === 'autonum') {
        const a = el.querySelector(`[data-f="${f.id}"][data-part="auto"]`);
        const num = el.querySelector(`[data-f="${f.id}"][data-part="num"]`);
        if (a) a.checked = x.auto;
        if (num) {
          num.value = x.num;
          num.disabled = x.auto || !exists[f.file];
        }
      } else if (f.type === 'range') {
        const r = el.querySelector(`[data-f="${f.id}"]`);
        if (r) r.value = x;
        const out = el.querySelector(`[data-readout="${f.id}"]`);
        if (out) out.textContent = `${x}${optName(f.unit || '%')}`;
      } else if (f.type === 'toggle') {
        const c = el.querySelector(`[data-f="${f.id}"]`);
        if (c) c.checked = !!x;
      } else {
        const c = el.querySelector(`[data-f="${f.id}"]`);
        if (c) c.value = x;
      }
    }
    updateBar();
  }

  /**
   * Switches the interface language: saved at once, then the page is redrawn in the new language with the
   * unsaved edits kept. `draw()` rebuilds the markup from `model`, and `applyModel()` puts the edited values
   * back into it, so nothing has to be read from disk again. Only the shell (navigation, window title) is
   * repainted; the page itself is not re-rendered, or the edits kept here would be dropped with this closure.
   */
  async function liveLang(code) {
    if (code === getLang()) return;
    const keep = { ...model };
    try {
      await api('save_config', { config: { ...cfg, lang: code } });
    } catch (e) {
      toast(errText(e), 'error');
      return;
    }
    cfg.lang = code;
    // Keep the in-game F1 menu in step: it reads [gfx] ui_language from fefw.ini.
    // zh maps to the zh pack; any other launcher language falls back to English there.
    try {
      await api('write_settings', { edits: [{ file: 'game', section: 'gfx', key: 'ui_language', value: code === 'zh' ? 'zh' : 'auto' }] });
    } catch (e) { /* fefw.ini may not exist yet; the menu keeps its current language */ }
    await setLang(code, () => {
      for (const id of Object.keys(keep)) model[id] = keep[id];
      // The switch was saved above, so it is not a pending edit: the picker shows the new language and the
      // save bar does not count it. Everything else keeps the value it had before the switch.
      model.lang = code;
      orig.lang = JSON.stringify(code);
      draw();
      applyModel();
      ctx.shell(); // navigation labels and the window title; the page keeps the edits above
    });
  }

  // The upscaler files the game will find with the folders as typed (found only when every file a feature needs is in one folder).
  let checkSeq = 0;
  async function checkFiles() {
    const seq = ++checkSeq;
    const r = await api('upscaler_files', { fsrDir: fwd(model.fsr_dll || ''), dlssDir: fwd(model.dlss_dll || ''), xessDir: fwd(model.xess_dll || '') })
      .catch(() => null);
    if (seq !== checkSeq || !r) return;
    const line = (name, x) => `${t(name)}: ${x.found ? t('settings.files.found', { detail: x.detail }) : t('settings.files.notFound', { detail: fileDetail(x.detail) })}`;
    const put = (which, text) => el.querySelectorAll(`[data-status="${which}"]`).forEach((n) => { n.textContent = text; });
    put('fsr', `${line('settings.files.upscaling', r.fsr_upscaling)}. ${line('settings.files.frameGen', r.fsr_framegen)}.`);
    put('dlss', `${line('settings.files.dlss', r.dlss)}. ${line('settings.files.dlssFrameGen', r.dlss_framegen)}.`);
    if (r.xess) put('xess', `${line('settings.files.xess', r.xess)}.`);
    // Optional official-source downloaders for DLSS (NVIDIA Streamline) and FSR (AMD GPUOpen), shown while the default folder lacks them.
    const syncDl = async (kind, need) => {
      const st = await api(`${kind}_download_status`).catch(() => null);
      if (st && st.state === 'running') pollDownloader(kind);
      else renderDownloader(kind, need, st && st.state === 'error' ? st : null);
    };
    await syncDl('dlss', !r.dlss_framegen.found && !String(model.dlss_dll || '').trim());
    await syncDl('fsr', (!r.fsr_upscaling.found || !r.fsr_framegen.found) && !String(model.fsr_dll || '').trim());
  }

  const dlCfg = {
    dlss: {
      size: 276,
      getKey: 'settings.files.dlssGet',
      hintKey: 'settings.files.dlssGetHint',
      doneKey: 'settings.files.dlssGetDone',
      confirmKey: 'settings.files.dlssConfirm',
      params: { version: '2.14.16', size: 276 },
    },
    fsr: {
      size: 46,
      getKey: 'settings.files.fsrGet',
      hintKey: 'settings.files.fsrGetHint',
      doneKey: 'settings.files.fsrGetDone',
      confirmKey: 'settings.files.fsrConfirm',
      params: { version: '2.4.0 / 1.1.4', size: 46 },
    },
  };
  const dlPoll = { dlss: 0, fsr: 0 };

  function renderDownloader(kind, show, st) {
    const cfgDl = dlCfg[kind];
    el.querySelectorAll(`[data-status="${kind}"]`).forEach((n) => {
      let box = n.parentElement.querySelector(`[data-dl-get="${kind}"]`);
      if (!show) {
        if (box) box.remove();
        return;
      }
      if (!box) {
        box = document.createElement('span');
        box.className = 'hint';
        box.dataset.dlGet = kind;
        n.after(box);
      }
      if (st && st.state === 'running') {
        box.innerHTML = `${esc(st.message)} <button class="btn" data-dl-cancel="${kind}">${esc(t('settings.files.dlssGetCancel'))}</button>`;
      } else {
        box.innerHTML = `<button class="btn" data-dl-start="${kind}">${icon('download')}${esc(t(cfgDl.getKey, cfgDl.params))}</button> `
          + `${esc(t(cfgDl.hintKey, cfgDl.params))}${st ? ` ${esc(st.message)}` : ''}`;
      }
      box.querySelector(`[data-dl-start="${kind}"]`)?.addEventListener('click', () => startDownloader(kind));
      box.querySelector(`[data-dl-cancel="${kind}"]`)?.addEventListener('click', () => api(`cancel_${kind}_download`).catch(() => {}));
    });
    refreshIcons();
  }
  async function startDownloader(kind) {
    const cfgDl = dlCfg[kind];
    if (!window.confirm(t(cfgDl.confirmKey, cfgDl.params))) return;
    try {
      await api(`start_${kind}_download`);
    } catch (e) {
      toast(errText(e), 'error');
      return;
    }
    pollDownloader(kind);
  }
  async function pollDownloader(kind) {
    clearTimeout(dlPoll[kind]);
    const st = await api(`${kind}_download_status`).catch(() => null);
    if (!st) return;
    if (st.state === 'running') {
      renderDownloader(kind, true, st);
      dlPoll[kind] = setTimeout(() => pollDownloader(kind), 400);
    } else if (st.state === 'done') {
      toast(t(dlCfg[kind].doneKey), 'ok');
      checkFiles();
    } else if (st.state === 'error') {
      renderDownloader(kind, true, st);
    }
  }
  let checkTimer = 0;

  function bind() {
    const byId = Object.fromEntries(all.map((f) => [f.id, f]));
    el.querySelectorAll('[data-f]').forEach((input) => {
      const f = byId[input.dataset.f];
      const part = input.dataset.part;
      const onChange = () => {
        const v = input.type === 'checkbox' ? input.checked : input.value;
        if (part) model[f.id] = { ...model[f.id], [part]: v };
        else model[f.id] = f.type === 'range' ? parseInt(v, 10) : v;
        if (f.type === 'language') {
          liveLang(v);
          return;
        }
        if (f.type === 'range') {
          el.querySelector(`[data-readout="${f.id}"]`).textContent = `${model[f.id]}${optName(f.unit || '%')}`;
          updateBar();
        } else if (input.type === 'text' || input.type === 'number') {
          updateBar();
          if (f.status) {
            clearTimeout(checkTimer);
            checkTimer = setTimeout(checkFiles, 300);
          }
        } else {
          draw(); // toggles and selects change dependent markup (labels, disabled inputs)
        }
      };
      input.addEventListener(input.type === 'range' || input.type === 'text' || input.type === 'number' ? 'input' : 'change', onChange);
    });
    el.querySelectorAll('[data-browse]').forEach((b) =>
      b.addEventListener('click', async () => {
        const f = byId[b.dataset.browse];
        const picked = await api('pick_folder', { title: t('common.choose', { label: t(f.label) }), start: model[f.id] || null });
        if (picked) {
          model[f.id] = picked;
          draw();
        }
      }),
    );
    el.querySelector('#revert').addEventListener('click', () => {
      for (const f of all) model[f.id] = JSON.parse(orig[f.id]);
      draw();
    });
    el.querySelector('#save').addEventListener('click', save);
  }

  function updateBar() {
    const n = dirtyFields().length;
    const bar = el.querySelector('.savebar');
    if (!bar) return;
    bar.querySelector('span').textContent = barText(n);
    bar.querySelector('#save').disabled = !n;
    bar.querySelector('#revert').disabled = !n;
  }

  async function save() {
    const edits = [];
    const cfgNew = { ...cfg };
    for (const f of dirtyFields()) {
      for (const [sec, key, value] of WRITE[f.type](f, model[f.id])) {
        if (f.file === 'config') {
          cfgNew[key] = key === 'minimize_on_play' ? value === '1' : key === 'jobs' ? parseInt(value, 10) || 0 : key === 'runner' ? String(value).trim() : fwd(value);
        } else {
          edits.push({ file: f.file, section: sec, key, value });
        }
      }
    }
    try {
      if (edits.length) await api('write_settings', { edits });
      if (JSON.stringify(cfgNew) !== JSON.stringify(cfg)) await api('save_config', { config: cfgNew });
      toast(t('common.settingsSaved'), 'ok');
      ctx.go('settings');
    } catch (e) {
      toast(errText(e), 'error');
    }
  }

  draw();
}
