import { getCurrentWebview } from '@tauri-apps/api/webview';
import { api, esc, icon, refreshIcons, errText, toast } from '../util.js';
import { t } from '../i18n.js';

export async function render(el, ctx) {
  let snapshot = null;
  let draftOrder = [];
  let draftDisabled = new Set();
  let draftChoices = {};
  let draftCheats = {};
  let importNotices = null; // { name, warnings }
  let logModal = null; // { file, lines, total_matched }
  const expanded = new Set();
  let dragSource = null;

  function syncDraft(snap) {
    snapshot = snap;
    draftOrder = snap.mods.map((m) => m.folder);
    draftDisabled = new Set(snap.mods.filter((m) => !m.enabled).map((m) => m.folder));
    draftChoices = {};
    draftCheats = {};
    for (const m of snap.mods) {
      if (m.options.length) {
        draftChoices[m.folder] = {};
        for (const o of m.options) {
          draftChoices[m.folder][o.key] = o.kind === 'toggle' ? o.value === 'true' : o.value;
        }
      }
      const on = m.cheats.filter((c) => c.enabled).map((c) => c.title);
      draftCheats[m.folder] = new Set(on);
    }
    ctx.setModsRebuild?.(Boolean(snap.needs_rebuild));
  }

  function sameSet(a, b) {
    if (a.size !== b.size) return false;
    for (const x of a) if (!b.has(x)) return false;
    return true;
  }

  function dirtyCount() {
    if (!snapshot) return 0;
    let count = 0;
    const origOrder = snapshot.mods.map((m) => m.folder);
    if (origOrder.length !== draftOrder.length || origOrder.some((v, i) => v !== draftOrder[i])) count++;
    for (const m of snapshot.mods) {
      const origEnabled = m.enabled;
      const nowEnabled = !draftDisabled.has(m.folder);
      if (origEnabled !== nowEnabled) count++;
      for (const o of m.options) {
        const origVal = o.kind === 'toggle' ? o.value === 'true' : o.value;
        const curVal = draftChoices[m.folder]?.[o.key];
        if (origVal !== curVal) count++;
      }
      const origCheats = new Set(m.cheats.filter((c) => c.enabled).map((c) => c.title));
      const curCheats = draftCheats[m.folder] ?? new Set();
      if (!sameSet(origCheats, curCheats)) count++;
    }
    return count;
  }

  function hasDrawer(m) {
    return Boolean(
      m.meta_error ||
      m.bdat_error ||
      m.meta?.description ||
      m.options.length ||
      m.cheats.length ||
      m.conflicts.length ||
      m.unmatched_paths_count > 0,
    );
  }

  function chipHtml(cls, label, titleAttr = '') {
    const tAttr = titleAttr ? ` title="${esc(titleAttr)}"` : '';
    return `<span class="mod-chip ${cls}"${tAttr}>${esc(label)}</span>`;
  }

  function renderOption(m, o) {
    const val = draftChoices[m.folder]?.[o.key];
    const targetPath = o.kind === 'select'
      ? (o.choices.find((c) => c.id === val)?.path ?? o.path)
      : (val ? o.path : '');
    const hintParts = [];
    if (o.hint) hintParts.push(o.hint);
    if (targetPath) hintParts.push(`→ ${targetPath}`);
    const pathWarn = o.unsafe_path ? `<span class="mod-chip danger">${esc(t('mods.unsafePath'))}</span>` : '';

    let control = '';
    if (o.kind === 'toggle') {
      control = `<label class="switch"><input type="checkbox" data-opt-toggle="${esc(m.folder)}" data-opt-key="${esc(o.key)}" ${val ? 'checked' : ''} /><span>${esc(t(val ? 'common.on' : 'common.off'))}</span></label>`;
    } else {
      control = `<select data-opt-select="${esc(m.folder)}" data-opt-key="${esc(o.key)}">${o.choices
        .map((c) => `<option value="${esc(c.id)}" ${c.id === val ? 'selected' : ''}>${esc(c.label)}</option>`)
        .join('')}</select>`;
    }
    return `<div class="field">
      <div>
        <label class="name">${esc(o.label)} ${pathWarn}</label>
        ${hintParts.length ? `<span class="hint">${esc(hintParts.join(' · '))}</span>` : ''}
      </div>
      <div class="control">${control}</div>
    </div>`;
  }

  function renderCheats(m) {
    if (!m.cheats.length) return '';
    const byFile = new Map();
    for (const c of m.cheats) {
      if (!byFile.has(c.file)) byFile.set(c.file, []);
      byFile.get(c.file).push(c);
    }
    const enabledSet = draftCheats[m.folder] ?? new Set();
    let html = '';
    for (const [file, list] of byFile.entries()) {
      html += `<div class="mod-cheats">
        <div class="mod-subhead">${esc(t('mods.cheatsHeader', { file }))}</div>
        ${list
          .map((c) => {
            const on = enabledSet.has(c.title);
            const badge = c.kind === 'code'
              ? chipHtml('warn', t('mods.cheatKindCode'))
              : chipHtml('', t('mods.cheatKindMem'));
            return `<label class="mod-cheat-row">
              <span class="switch"><input type="checkbox" data-cheat-mod="${esc(m.folder)}" data-cheat-title="${esc(c.title)}" ${on ? 'checked' : ''} /><span>${esc(c.title)}</span></span>
              ${badge}
            </label>`;
          })
          .join('')}
      </div>`;
    }
    return html;
  }

  function renderConflicts(m) {
    if (!m.conflicts.length) return '';
    return `<div>
      <div class="mod-subhead">${esc(t('mods.conflictsHeader', { count: m.conflict_count }))}</div>
      <ul class="mod-conflicts">
        ${m.conflicts
          .map((c) => {
            const msg = c.role === 'wins'
              ? t('mods.conflictWins', { peer: c.peer })
              : t('mods.conflictLoses', { peer: c.peer });
            return `<li><code>${esc(c.path)}</code> — ${esc(msg)}</li>`;
          })
          .join('')}
      </ul>
    </div>`;
  }

  function renderUnmatchedPaths(m) {
    if (!m.unmatched_paths_count) return '';
    return `<div>
      <div class="mod-subhead">${esc(t('mods.unmatchedPathsHeader'))}</div>
      <ul class="mod-conflicts">
        ${m.unmatched_paths_sample.map((p) => `<li><code>${esc(p)}</code></li>`).join('')}
        ${m.unmatched_paths_count > m.unmatched_paths_sample.length ? `<li class="dim">… +${m.unmatched_paths_count - m.unmatched_paths_sample.length}</li>` : ''}
      </ul>
    </div>`;
  }

  function healthLabel(m, enabled) {
    if (!enabled || m.health === 'off') {
      return { light: 'off', text: t('mods.healthDisabled'), tip: t('mods.healthDisabledTip') };
    }
    const rt = m.runtime_status || {};
    const statsParts = [];
    if (rt.files > 0) statsParts.push(t('mods.rtFiles', { count: rt.files }));
    if (rt.ard_overrides > 0) statsParts.push(t('mods.rtArd', { count: rt.ard_overrides }));
    if (rt.archive_recs > 0) statsParts.push(t('mods.rtRecs', { count: rt.archive_recs }));
    const stats = statsParts.join(' · ');

    if (m.health === 'green') {
      return {
        light: 'green',
        text: stats ? `${t('mods.healthActive')} · ${stats}` : t('mods.healthActive'),
        tip: stats ? `${t('mods.healthActiveTip')} (${stats})` : t('mods.healthActiveTip'),
      };
    }
    if (m.health === 'red') {
      const key = m.health_reason === 'empty' ? 'mods.healthEmpty' : 'mods.healthIncompatible';
      return { light: 'red', text: t(key), tip: m.bdat_error || t(`${key}Tip`) };
    }
    if (m.health_reason === 'partial') {
      return {
        light: 'yellow',
        text: stats ? `${t('mods.healthPartial')} · ${stats}` : t('mods.healthPartial'),
        tip: t('mods.healthPartialTip'),
      };
    }
    if (m.health_reason === 'pending_rebuild') {
      return { light: 'yellow', text: t('mods.capPendingRebuild'), tip: t('mods.healthPendingTip') };
    }
    return { light: 'yellow', text: t('mods.healthNotVerified'), tip: t('mods.healthNotVerifiedTip') };
  }

  function renderModCard(m, idx, total) {
    const enabled = !draftDisabled.has(m.folder);
    const isOpen = expanded.has(m.folder);
    const drawerAvail = hasDrawer(m);
    const hl = healthLabel(m, enabled);
    const cardState = !enabled
      ? 'disabled'
      : hl.light === 'red'
        ? 'danger'
        : hl.light === 'yellow' && (m.pending_rebuild || m.health_reason === 'partial')
          ? 'warn'
          : (idx === 0 ? 'accent' : '');

    const title = m.meta?.name ? m.meta.name : m.folder;
    const subParts = [];
    if (m.meta?.name && m.meta.name !== m.folder) subParts.push(m.folder);
    if (m.meta?.version) subParts.push(`v${m.meta.version}`);
    if (m.meta?.author) subParts.push(m.meta.author);

    const badges = [];
    if (enabled) {
      const hlCls = hl.light === 'green' ? 'ok' : hl.light === 'red' ? 'danger' : 'warn';
      badges.push(chipHtml(hlCls, hl.text, hl.tip));
    }
    // High-level modification categories (e.g. BDAT, UI, Models, Map, Audio/Video, Archive, Code, Cheats, Other)
    for (const cat of (m.categories || [])) {
      if (cat.key === 'cheats' && m.cheats.length > 0) {
        const activeCheats = draftCheats[m.folder]?.size ?? 0;
        badges.push(chipHtml('cat', t('mods.capCheats', { active: activeCheats, total: m.cheats.length })));
      } else {
        const catName = t(`mods.cat.${cat.key}`);
        badges.push(chipHtml('cat', `${catName} · ${cat.count}`));
      }
    }
    if (m.bdat_error) badges.push(chipHtml('danger', t('mods.capBdatError'), m.bdat_error));
    if (m.caps.unmatched_patches > 0) badges.push(chipHtml('danger', t('mods.capUnmatched', { count: m.caps.unmatched_patches })));
    if (m.unmatched_cheats > 0) badges.push(chipHtml('warn', t('mods.capUnmatchedCheats', { count: m.unmatched_cheats })));
    if (m.version_mismatch && m.meta?.game) badges.push(chipHtml('danger', t('mods.capWrongGame', { game: m.meta.game })));
    if (m.unmatched_paths_count > 0 && enabled) {
      badges.push(chipHtml('warn', t('mods.capUnmatchedPaths', { count: m.unmatched_paths_count }), m.unmatched_paths_sample.join('\n')));
    }
    if (m.conflict_count > 0 && enabled) {
      const hoverSummary = m.conflicts
        .slice(0, 8)
        .map((c) => `${c.path}: ${c.role === 'wins' ? t('mods.conflictWins', { peer: c.peer }) : t('mods.conflictLoses', { peer: c.peer })}`)
        .join('\n');
      badges.push(chipHtml('warn', t('mods.capConflicts', { count: m.conflict_count }), hoverSummary));
    }

    let drawerHtml = '';
    if (isOpen && drawerAvail) {
      drawerHtml = `<div class="mod-drawer">
        ${m.bdat_error ? `<div class="banner danger">${icon('circle-x')}<div>${esc(m.bdat_error)}</div></div>` : ''}
        ${m.meta_error ? `<div class="banner warn">${icon('triangle-alert')}<div>${esc(m.meta_error)}</div></div>` : ''}
        ${m.meta?.description ? `<p class="dim">${esc(m.meta.description)}</p>` : ''}
        ${m.options.map((o) => renderOption(m, o)).join('')}
        ${renderCheats(m)}
        ${renderConflicts(m)}
        ${renderUnmatchedPaths(m)}
      </div>`;
    }

    const rankLabel = idx === 0 ? t('mods.rankTop') : `#${idx + 1}`;

    return `<div class="mod-card ${cardState}" data-folder="${esc(m.folder)}" draggable="true">
      <div class="mod-head">
        <span class="mod-grip" title="${esc(t('mods.dragHint'))}">${icon('grip-vertical')}</span>
        <span class="mod-light ${hl.light}" title="${esc(hl.tip)}"></span>
        <input type="checkbox" data-toggle-mod="${esc(m.folder)}" ${enabled ? 'checked' : ''} aria-label="${esc(title)}" />
        <div class="mod-title-wrap">
          <span class="mod-name">${esc(title)}</span>
          ${subParts.length ? `<span class="mod-meta">${esc(subParts.join(' · '))}</span>` : ''}
        </div>
        <span class="mod-rank">${esc(rankLabel)}</span>
        <div class="mod-actions">
          <button class="btn icon-sm" data-move="${esc(m.folder)}" data-dir="-1" ${idx === 0 ? 'disabled' : ''} title="${esc(t('mods.moveUp'))}">${icon('chevron-up')}</button>
          <button class="btn icon-sm" data-move="${esc(m.folder)}" data-dir="1" ${idx === total - 1 ? 'disabled' : ''} title="${esc(t('mods.moveDown'))}">${icon('chevron-down')}</button>
          <button class="btn icon-sm" data-open-mod="${esc(m.folder)}" title="${esc(t('mods.openOneFolder'))}">${icon('folder-open')}</button>
          <button class="btn icon-sm danger-icon" data-delete-mod="${esc(m.folder)}" ${snapshot.game_running ? 'disabled' : ''} title="${esc(t('mods.deleteMod'))}">${icon('trash-2')}</button>
          ${drawerAvail ? `<button class="btn icon-sm" data-expand="${esc(m.folder)}" title="${esc(t('mods.toggleDetails'))}">${icon(isOpen ? 'chevron-down' : 'chevron-right')}</button>` : ''}
        </div>
      </div>
      ${badges.length ? `<div class="mod-badges">${badges.join('')}</div>` : ''}
      ${drawerHtml}
    </div>`;
  }

  function renderLogModal() {
    if (!logModal) return '';
    return `<div class="modal-backdrop" id="mod-log-backdrop">
      <div class="modal-card" style="padding: var(--space-4)">
        <div class="row between">
          <strong>${esc(t('mods.logTitle'))}</strong>
          <button class="btn icon-sm" id="close-mod-log">${icon('x')}</button>
        </div>
        <div class="row between wrap">
          <span class="faint" style="font-size: var(--font-size-xs)">${esc(logModal.file)}</span>
          <span class="chip">${esc(t('mods.logCount', { count: logModal.total_matched }))}</span>
        </div>
        ${logModal.lines.length
          ? `<pre class="mod-log-pre">${esc(logModal.lines.join('\n'))}</pre>`
          : `<p class="dim">${esc(t('mods.logEmpty'))}</p>`}
      </div>
    </div>`;
  }

  function draw() {
    if (!snapshot) {
      el.innerHTML = `<div class="page"><div class="hero"><h1>${esc(t('mods.title'))}</h1><p class="dim lead">${esc(t('mods.loading'))}</p></div></div>`;
      return;
    }

    const modByFolder = new Map(snapshot.mods.map((m) => [m.folder, m]));
    const orderedMods = draftOrder.map((f) => modByFolder.get(f)).filter(Boolean);
    const enabledCount = orderedMods.filter((m) => !draftDisabled.has(m.folder)).length;
    const disabledCount = orderedMods.length - enabledCount;
    const dirty = dirtyCount();

    const summary = orderedMods.length
      ? t('mods.summaryChip', { enabled: enabledCount, disabled: disabledCount })
      : t('mods.emptyChip');

    el.innerHTML = `
      <div class="page">
        <div class="mods-header">
          <div>
            <h1>${esc(t('mods.title'))}</h1>
            <p class="mods-subtitle">${esc(t('mods.priorityNote'))}</p>
          </div>
          <div class="mods-pills">
            <span class="chip ${enabledCount ? 'ok' : ''}">${icon('package')}<span>${esc(summary)}</span></span>
            <span class="mod-bid-pill" title="${esc(t('mods.versionStrip', { bid: snapshot.main_build_id || '8BEF68ED1EEBC24B' }))}">
              ${icon('info')}<span>3.0.1 · ${esc(snapshot.main_build_id || '8BEF68ED1EEBC24B')}</span>
            </span>
          </div>
        </div>

        <div class="mods-toolbar">
          <div class="mods-toolbar-group">
            <button class="btn sm" id="import-mod-file" title="${esc(t('mods.pickFileTitle'))}">${icon('upload')}${esc(t('mods.importFile'))}</button>
            <button class="btn sm" id="add-mod">${icon('plus')}${esc(t('mods.addMod'))}</button>
            <button class="btn sm" id="view-mod-log">${icon('scroll-text')}${esc(t('mods.viewLog'))}</button>
            <button class="btn sm" id="open-mods-root">${icon('folder-open')}${esc(t('mods.openDirShort'))}</button>
            <button class="btn sm icon-sm" id="refresh-mods" title="${esc(t('mods.refresh'))}">${icon('refresh-cw')}</button>
          </div>
          <button class="btn primary sm" id="safe-mode-play" ${snapshot.game_running ? 'disabled' : ''} title="${esc(t('mods.safeModeHint'))}">${icon('shield-off')}${esc(t('mods.safeMode'))}</button>
        </div>

        ${importNotices?.warnings?.length ? `<div class="banner warn">
          ${icon('triangle-alert')}
          <div style="flex:1">
            <strong>${esc(t('mods.importWarningsTitle', { name: importNotices.name }))}</strong>
            <ul class="mod-conflicts" style="margin-top: 6px">
              ${importNotices.warnings.map((w) => `<li>${esc(w)}</li>`).join('')}
            </ul>
          </div>
          <button class="btn icon-sm" id="dismiss-import-notices">${icon('x')}</button>
        </div>` : ''}

        ${snapshot.game_running ? `<div class="banner info">${icon('circle-alert')}<div>${esc(t('mods.runningBanner'))}</div></div>` : ''}
        ${snapshot.needs_rebuild ? `<div class="banner warn">
          ${icon('triangle-alert')}
          <div style="flex:1">
            <div>${esc(t('mods.rebuildBanner', { modules: snapshot.rebuild_modules.join(', ') }))}</div>
            <div style="margin-top: var(--space-2)"><button class="btn primary sm" id="go-rebuild">${icon('hammer')}${esc(t('mods.rebuildNow'))}</button></div>
          </div>
        </div>` : ''}

        ${orderedMods.length === 0
          ? `<div class="mod-dropzone" id="mod-dropzone">
              ${icon('upload')}
              <div>${esc(t('mods.dropHint'))}</div>
              <div class="mod-drop-path">${esc(snapshot.mods_dir)}</div>
            </div>`
          : `<div class="mod-list">${orderedMods.map((m, i) => renderModCard(m, i, orderedMods.length)).join('')}</div>
             <div class="mod-dropzone compact" id="mod-dropzone">
               ${icon('upload')}<span>${esc(t('mods.dropHintCompact'))}</span>
             </div>`}

        ${dirty > 0 ? `<div class="savebar">
          <span class="dim">${esc(dirty === 1 ? t('common.unsavedOne') : t('common.unsavedMany', { count: dirty }))}</span>
          <div class="row">
            <button class="btn quiet" id="discard-mods">${icon('undo-2')}${esc(t('common.revert'))}</button>
            <button class="btn primary" id="save-mods">${icon('save')}${esc(t('common.save'))}</button>
          </div>
        </div>` : ''}

        ${renderLogModal()}
      </div>`;
    refreshIcons();
    wireEvents();
  }

  async function runImport(sourcePath) {
    try {
      const res = await api('import_mod', { sourcePath });
      syncDraft(res.snapshot);
      importNotices = res.warnings?.length ? { name: res.folder, warnings: res.warnings } : null;
      draw();
      toast(t('mods.added', { name: res.folder }), 'ok');
    } catch (e) {
      toast(errText(e), 'error');
    }
  }

  function moveMod(folder, delta) {
    const i = draftOrder.indexOf(folder);
    const j = i + delta;
    if (i < 0 || j < 0 || j >= draftOrder.length) return;
    [draftOrder[i], draftOrder[j]] = [draftOrder[j], draftOrder[i]];
    draw();
  }

  function wireEvents() {
    el.querySelector('#refresh-mods')?.addEventListener('click', load);
    el.querySelector('#open-mods-root')?.addEventListener('click', () => api('open_mods_dir').catch((e) => toast(errText(e), 'error')));
    el.querySelector('#go-rebuild')?.addEventListener('click', () => ctx.go('setup'));
    el.querySelector('#dismiss-import-notices')?.addEventListener('click', () => {
      importNotices = null;
      draw();
    });

    el.querySelector('#import-mod-file')?.addEventListener('click', async () => {
      const file = await api('pick_mod_file', { title: t('mods.pickFileTitle') }).catch(() => null);
      if (!file) return;
      await runImport(file);
    });

    el.querySelector('#add-mod')?.addEventListener('click', async () => {
      const folder = await api('pick_folder', { title: t('mods.pickModTitle'), start: null }).catch(() => null);
      if (!folder) return;
      await runImport(folder);
    });

    el.querySelector('#view-mod-log')?.addEventListener('click', async () => {
      try {
        logModal = await api('read_mod_log');
        draw();
      } catch (e) {
        toast(errText(e), 'error');
      }
    });

    el.querySelector('#close-mod-log')?.addEventListener('click', () => {
      logModal = null;
      draw();
    });
    el.querySelector('#mod-log-backdrop')?.addEventListener('click', (ev) => {
      if (ev.target.id === 'mod-log-backdrop') {
        logModal = null;
        draw();
      }
    });

    el.querySelector('#safe-mode-play')?.addEventListener('click', async () => {
      try {
        await api('play', { safeMode: true });
        toast(t('mods.safeModeStarted'), 'ok');
      } catch (e) {
        toast(errText(e), 'error');
      }
    });

    el.querySelectorAll('[data-toggle-mod]').forEach((cb) =>
      cb.addEventListener('change', () => {
        const f = cb.dataset.toggleMod;
        if (cb.checked) draftDisabled.delete(f);
        else draftDisabled.add(f);
        draw();
      }),
    );

    el.querySelectorAll('[data-move]').forEach((btn) =>
      btn.addEventListener('click', () => moveMod(btn.dataset.move, Number(btn.dataset.dir))),
    );

    el.querySelectorAll('[data-open-mod]').forEach((btn) =>
      btn.addEventListener('click', () => api('open_mod_folder', { name: btn.dataset.openMod }).catch((e) => toast(errText(e), 'error'))),
    );

    el.querySelectorAll('[data-delete-mod]').forEach((btn) =>
      btn.addEventListener('click', async () => {
        const name = btn.dataset.deleteMod;
        if (!window.confirm(t('mods.deleteConfirm', { name }))) return;
        try {
          const res = await api('delete_mod', { name });
          expanded.delete(name);
          syncDraft(res.snapshot);
          draw();
          if (res.had_code_mods) {
            toast(t('mods.deletedRebuildHint', { name }), 'warn');
          } else {
            toast(t('mods.deleted', { name }), 'ok');
          }
        } catch (e) {
          toast(errText(e), 'error');
        }
      }),
    );

    el.querySelectorAll('[data-expand]').forEach((btn) =>
      btn.addEventListener('click', () => {
        const f = btn.dataset.expand;
        if (expanded.has(f)) expanded.delete(f);
        else expanded.add(f);
        draw();
      }),
    );

    el.querySelectorAll('[data-opt-select]').forEach((sel) =>
      sel.addEventListener('change', () => {
        const f = sel.dataset.optSelect;
        if (!draftChoices[f]) draftChoices[f] = {};
        draftChoices[f][sel.dataset.optKey] = sel.value;
        draw();
      }),
    );

    el.querySelectorAll('[data-opt-toggle]').forEach((cb) =>
      cb.addEventListener('change', () => {
        const f = cb.dataset.optToggle;
        if (!draftChoices[f]) draftChoices[f] = {};
        draftChoices[f][cb.dataset.optKey] = cb.checked;
        draw();
      }),
    );

    el.querySelectorAll('[data-cheat-mod]').forEach((cb) =>
      cb.addEventListener('change', () => {
        const f = cb.dataset.cheatMod;
        const title = cb.dataset.cheatTitle;
        if (!draftCheats[f]) draftCheats[f] = new Set();
        if (cb.checked) draftCheats[f].add(title);
        else draftCheats[f].delete(title);
        draw();
      }),
    );

    el.querySelector('#discard-mods')?.addEventListener('click', () => {
      syncDraft(snapshot);
      draw();
    });

    el.querySelector('#save-mods')?.addEventListener('click', async () => {
      const cheatsEnabled = {};
      for (const [m, set] of Object.entries(draftCheats)) {
        if (set.size) cheatsEnabled[m] = Array.from(set);
      }
      try {
        const snap = await api('save_mods_state', {
          input: {
            order: draftOrder,
            disabled: Array.from(draftDisabled),
            choices: draftChoices,
            cheats_enabled: cheatsEnabled,
          },
        });
        syncDraft(snap);
        draw();
        toast(t('mods.saved'), 'ok');
      } catch (e) {
        toast(errText(e), 'error');
      }
    });

    el.querySelectorAll('.mod-card').forEach((card) => {
      card.addEventListener('dragstart', (ev) => {
        dragSource = card.dataset.folder;
        ev.dataTransfer.effectAllowed = 'move';
        ev.dataTransfer.setData('text/plain', dragSource);
      });
      card.addEventListener('dragover', (ev) => {
        if (!dragSource || dragSource === card.dataset.folder) return;
        ev.preventDefault();
        const rect = card.getBoundingClientRect();
        const before = ev.clientY < rect.top + rect.height / 2;
        card.classList.toggle('drop-before', before);
        card.classList.toggle('drop-after', !before);
      });
      card.addEventListener('dragleave', () => {
        card.classList.remove('drop-before', 'drop-after');
      });
      card.addEventListener('drop', (ev) => {
        ev.preventDefault();
        const target = card.dataset.folder;
        const before = card.classList.contains('drop-before');
        card.classList.remove('drop-before', 'drop-after');
        if (!dragSource || dragSource === target) return;
        const from = draftOrder.indexOf(dragSource);
        if (from < 0) return;
        draftOrder.splice(from, 1);
        let to = draftOrder.indexOf(target);
        if (!before) to++;
        draftOrder.splice(to, 0, dragSource);
        dragSource = null;
        draw();
      });
      card.addEventListener('dragend', () => {
        dragSource = null;
        el.querySelectorAll('.mod-card').forEach((c) => c.classList.remove('drop-before', 'drop-after'));
      });
    });
  }

  // Subscribe to Tauri 2 native OS file drag-and-drop so players can drag folders/zips/files onto the Mods page.
  try {
    const unlisten = await getCurrentWebview().onDragDropEvent(async (event) => {
      if (!el.isConnected) {
        unlisten();
        return;
      }
      if (dragSource) return; // Ignore internal mod-card reorder drags.
      const dz = el.querySelector('#mod-dropzone');
      const kind = event.payload?.type;
      if (kind === 'over' || kind === 'enter') {
        dz?.classList.add('active');
      } else if (kind === 'leave' || kind === 'cancel') {
        dz?.classList.remove('active');
      } else if (kind === 'drop') {
        dz?.classList.remove('active');
        const paths = event.payload?.paths ?? [];
        for (const p of paths) {
          await runImport(p);
        }
      }
    });
  } catch {
    // Plain browser preview without Tauri runtime: ignore.
  }

  async function load() {
    try {
      const snap = await api('list_mods');
      syncDraft(snap);
      draw();
    } catch (e) {
      el.innerHTML = `<div class="page"><div class="banner danger">${icon('circle-x')}<div>${esc(errText(e))}</div></div></div>`;
      refreshIcons();
    }
  }

  ctx.onStatus?.((s) => {
    if (snapshot) {
      snapshot.game_running = s.state === 'running';
      draw();
    }
  });

  draw();
  await load();
}
