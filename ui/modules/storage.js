/* ═══════════════════════════════════════════════════════════════
   PureCVisor — modules/storage.js
   Storage (ZFS Pools + Zvols)
   ADR-0013: IIFE module scope — PCV.storage namespace
   ═══════════════════════════════════════════════════════════════ */
/*
 * Storage rendering treats pool state as operational data, not decoration.
 * Destructive actions use typed confirmation helpers, capacity widgets tolerate
 * absent metrics, and zvol selection is kept in window._zvolSel so rerenders do
 * not expose bulk deletion before the user has selected rows again.
 */
window.PCV = window.PCV || {};
(function(PCV) {

function storagePct(totalBytes, usedBytes) {
  if (!totalBytes || totalBytes <= 0) return 0;
  return Math.max(0, (usedBytes / totalBytes) * 100);
}

function storagePctText(totalBytes, usedBytes) {
  return storagePct(totalBytes, usedBytes).toFixed(1) + '%';
}

async function renderStorage(b) {
  b.innerHTML = showSkeleton();
  try {
    const p = await fetchGet(EP.STORAGE_POOLS());
    const pl = unwrapList(p);
    let h = '<div class="ops-section-heading"><div><h3>' + _L('스토리지 운영 개요', 'Storage operations overview') + '</h3><p>' + _L('풀 상태, 용량 계획, Zvol 작업을 한 화면에서 정리합니다.', 'Review pool health, capacity planning, and zvol operations in one place.') + '</p></div><button class="btn btn-g" onclick="showPoolCreate()">+ ' + _L('풀 생성', 'Create pool') + '</button></div>';
    if (pl.length === 0) { h += '<div class="empty-state" style="text-align:center;padding:40px 20px"><div style="font-size:48px;margin-bottom:12px;opacity:.5">&#128190;</div><div style="font-size:14px;color:var(--fg2);margin-bottom:16px">' + _L('구성된 ZFS 풀이 없습니다', 'No configured ZFS pools') + '</div><button class="btn btn-g" onclick="showPoolCreate()" class="text-12">+ ' + _L('풀 생성', 'Create pool') + '</button></div>'; }
    if (pl.length > 0) {
      const totalBytes = pl.reduce(function(sum, v) { return sum + parseSize(v.size); }, 0);
      const usedBytes = pl.reduce(function(sum, v) { return sum + parseSize(v.alloc || v.used); }, 0);
      const totalPct = storagePct(totalBytes, usedBytes);
      const warningPools = pl.filter(function(v) {
        const sz = parseSize(v.size);
        const us = parseSize(v.alloc || v.used);
        const pct = storagePct(sz, us);
        return v.health !== 'ONLINE' || pct >= 80;
      }).length;
      h += '<div class="sg grid-3">';
      h += H.card(_L('풀 상태', 'Pool health'), '<div class="stat-lg color-accent">' + pl.length + '</div>' + H.row(_L('정상', 'Healthy'), '<span class="color-green">' + (pl.length - warningPools) + '</span>') + H.row(_L('주의 필요', 'Needs attention'), '<span class="color-yellow">' + warningPools + '</span>'));
      h += H.card(_L('사용 중 용량', 'Used capacity'), '<div class="stat-lg color-green">' + fmtBytes(usedBytes) + '</div>' + renderProgressBar(Math.min(totalPct, 100)) + H.row(_L('전체', 'Total'), fmtBytes(totalBytes)) + H.row(_L('사용률', 'Usage'), storagePctText(totalBytes, usedBytes)));
      h += H.card(_L('운영 원칙', 'Operating rule'), '<div class="stat-label" style="line-height:1.7">' + _L('스크럽과 삭제는 풀 상태를 먼저 확인한 뒤 실행합니다.', 'Run scrub and destroy only after checking pool health.') + '</div>');
      h += '</div>';
      h += '<div class="ops-section-heading"><div><h3>' + _L('풀 상태', 'Pool status') + '</h3><p>' + _L('각 풀의 용량과 건강 상태를 확인한 뒤 유지보수 작업을 선택합니다.', 'Review each pool before choosing maintenance actions.') + '</p></div></div>';
    }
    pl.forEach(v => {
      const sz = parseSize(v.size), us = parseSize(v.alloc || v.used), pct = storagePct(sz, us);
      h += H.card('&#128190; ' + escapeHtml(v.name) + ' ' + H.badge(v.health, v.health === 'ONLINE' ? 'g' : 'r'), H.row(_L('총 용량', 'Total size'), fmtBytes(sz)) + H.row(_L('사용량', 'Used'), fmtBytes(us)) + H.row(_L('건강 상태', 'Health'), escapeHtml(v.health || '-')) + renderProgressBar(Math.min(pct, 100)) + H.row(_L('사용률', 'Usage'), storagePctText(sz, us)) + '<div class="flex gap-4 ops-action-row" style="margin-top:10px"><button class="btn btn-soft" style="font-size:10px;padding:3px 8px" onclick="poolScrub(\'' + escapeAttr(v.name) + '\')">&#128260; ' + _L('스크럽', 'Scrub') + '</button><button class="btn btn-r" style="font-size:10px;padding:3px 8px" onclick="poolDestroy(\'' + escapeAttr(v.name) + '\')">&#128465; ' + _L('영구 삭제', 'Destroy') + '</button></div>', 'mb-8');
    });
    /* Storage usage donut */
    if (pl.length > 0) {
      h += '<div class="sg grid-2">';
      pl.forEach(function(v, pi) {
        var sz = parseSize(v.size), us = parseSize(v.alloc || v.used), pct = storagePct(sz, us);
        h += H.card(esc(v.name) + ' ' + _L('사용량', 'Usage'), '<div style="position:relative;width:120px;height:120px;margin:0 auto">'
          + '<canvas id="pool-donut-' + pi + '" width="120" height="120"></canvas>'
          + '<div style="position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);font-size:16px;font-weight:700;color:var(--accent)">' + pct.toFixed(0) + '%</div>'
          + '</div>' + H.row(_L('전체', 'Total'), fmtBytes(sz)) + H.row(_L('사용', 'Used'), fmtBytes(us)));
      });
      h += '</div>';
    }

    /* Storage forecast panel */
    h += '<div class="hc mb-14"><h4>&#128200; ' + _L('용량 예측', 'Capacity planning') + '</h4>';
    h += '<p class="color-muted text-11 mb-8">' + _L('일별 증가량 기준으로 풀 소진 시점을 예측합니다. 확장이나 정리 시점을 먼저 판단하는 용도입니다.', 'Forecast pool exhaustion based on daily growth so you can plan expansion or cleanup ahead of time.') + '</p>';
    h += '<div id="storage-forecast"><span class="spinner"></span> ' + (t('loading') || 'Loading...') + '</div></div>';
    setTimeout(loadStorageForecast, 100);

    const z = await fetchGet(EP.STORAGE_ZVOLS());
    const zl = unwrapList(z);
    h += '<div class="ops-section-heading"><div><h3>' + _L('Zvol 볼륨', 'Zvol volumes') + '</h3><p>' + _L('디스크 볼륨은 표로 관리하고, 대량 삭제는 선택 상태에서만 노출합니다.', 'Manage disk volumes in a table and expose bulk delete only after selection.') + '</p></div><div class="flex gap-8 ops-action-row"><button class="btn btn-primary" onclick="showZvol()">+ ' + t('btn.create') + '</button> <button class="btn btn-r" id="zvol-bulk-del" style="display:none" onclick="zvolBulkDelete()">&#128465; ' + _L('선택 삭제', 'Delete Selected') + ' (<span id="zvol-sel-count">0</span>)</button></div></div>';
    if (zl.length === 0) { h += '<div class="empty-state"><div class="empty-state-icon">&#128190;</div><div class="empty-state-text">' + _L('아직 생성된 Zvol이 없습니다', 'No zvols created yet') + '</div><div class="color-muted text-12">' + _L('추가 디스크가 필요할 때 생성 버튼으로 바로 만들 수 있습니다.', 'Use the create button when a workload needs an additional disk.') + '</div></div>'; b.innerHTML = h;
      setTimeout(function() { pl.forEach(function(v, pi) { var canvas = document.getElementById('pool-donut-' + pi); if (!canvas) return; var ctx = canvas.getContext('2d'); var sz = parseSize(v.size), us = parseSize(v.alloc || v.used); var pct = sz > 0 ? us / sz : 0; var r = 50, cx = 60, cy = 60, lw = 14; ctx.lineWidth = lw; ctx.beginPath(); ctx.arc(cx, cy, r, 0, Math.PI * 2); ctx.strokeStyle = 'rgba(255,255,255,0.1)'; ctx.stroke(); ctx.beginPath(); ctx.arc(cx, cy, r, -Math.PI/2, -Math.PI/2 + Math.PI * 2 * pct); try { ctx.strokeStyle = getComputedStyle(document.documentElement).getPropertyValue(pct > 0.85 ? '--red' : pct > 0.6 ? '--yellow' : '--green').trim(); } catch(e) {} ctx.stroke(); }); }, 100);
      return; }
    if (!window._zvolSel) window._zvolSel = new Set();
    h += '<table class="table-sticky"><thead><tr><th><input type="checkbox" id="zvol-all" onclick="zvolToggleAll(this.checked)"></th><th>Path</th><th>Size</th><th>Used</th><th>' + _L('압축비', 'Compress') + '</th><th>Dedup</th><th>Written</th><th>' + t('vm.settings') + '</th></tr></thead><tbody>';
    zl.forEach(v => {
      const vn = v.name || v.path;
      const checked = window._zvolSel.has(vn) ? 'checked' : '';
      const rowCls = window._zvolSel.has(vn) ? ' class="multi-selected"' : '';
      h += `<tr${rowCls} oncontextmenu="event.preventDefault();zvolCtxMenu(event,'${escapeAttr(vn)}')"><td><input type="checkbox" ${checked} onclick="zvolToggleSel('${escapeAttr(vn)}',this.checked)"></td><td><span class="text-ellipsis" title="${escapeHtml(vn)}">${escapeHtml(vn)}</span></td><td>${escapeHtml(v.volsize || v.size || '-')}</td><td>${escapeHtml(v.used || '-')}</td><td>${escapeHtml(v.compression_ratio || '-')}</td><td>${escapeHtml(v.dedup || '-')}</td><td>${escapeHtml(v.written || '-')}</td><td><button class="btn btn-r" style="font-size:10px;padding:3px 8px" onclick="zvolDel('${escapeAttr(vn)}')">${t('btn.delete')}</button></td></tr>`;
    });
    b.innerHTML = h + '</tbody></table>';
    if (window._zvolSel.size > 0) {
      var bd = document.getElementById('zvol-bulk-del');
      var sc = document.getElementById('zvol-sel-count');
      if (bd) bd.style.display = '';
      if (sc) sc.textContent = window._zvolSel.size;
    }

    /* Draw donut charts */
    setTimeout(function() {
      pl.forEach(function(v, pi) {
        var canvas = document.getElementById('pool-donut-' + pi);
        if (!canvas) return;
        var ctx = canvas.getContext('2d');
        var sz = parseSize(v.size), us = parseSize(v.alloc || v.used);
        var pct = sz > 0 ? us / sz : 0;
        var r = 50, cx = 60, cy = 60, lw = 14;
        ctx.lineWidth = lw;
        ctx.beginPath(); ctx.arc(cx, cy, r, 0, Math.PI * 2);
        ctx.strokeStyle = 'rgba(255,255,255,0.1)'; ctx.stroke();
        ctx.beginPath(); ctx.arc(cx, cy, r, -Math.PI/2, -Math.PI/2 + Math.PI * 2 * pct);
        ctx.strokeStyle = pct > 0.85 ? 'var(--red)' : pct > 0.6 ? 'var(--yellow)' : 'var(--green)';
        try { ctx.strokeStyle = getComputedStyle(document.documentElement).getPropertyValue(pct > 0.85 ? '--red' : pct > 0.6 ? '--yellow' : '--green').trim(); } catch(e) {}
        ctx.stroke();
      });
    }, 100);
  } catch (e) { if(_DEBUG) console.warn('z:', e.message); }
}

function showPoolCreate() {
  showModal('<h2>Create ZFS Pool</h2><div class="fr"><label>Pool Name</label><input id="pc-name" placeholder="newpool"></div><div class="fr"><label>Disks (space sep)</label><input id="pc-disks" placeholder="/dev/sdb /dev/sdc"></div><div class="fr"><label>RAID Level</label><select id="pc-raid" style="width:100%;padding:6px;background:var(--bg);border:1px solid var(--border);color:var(--fg);border-radius:4px"><option value="">Stripe (default)</option><option value="mirror">Mirror</option><option value="raidz">RAIDZ</option><option value="raidz2">RAIDZ2</option></select></div><div class="text-right mt-12"><button class="btn btn-g" onclick="doPoolCreate()">' + t('btn.create') + '</button> <button class="btn btn-r" onclick="closeModal()">' + t('btn.cancel') + '</button></div>');
}

async function doPoolCreate() {
  var _btn = document.activeElement;
  if (_btn && _btn.tagName === 'BUTTON') { if (_btn.disabled) return; _btn.disabled = true; _btn.setAttribute('aria-busy', 'true'); }
  const name = document.getElementById('pc-name')?.value;
  const disks = document.getElementById('pc-disks')?.value;
  const raid = document.getElementById('pc-raid')?.value;
  if (!name) { toast(t('msg.pool_name_required'), false); if (_btn) { _btn.disabled = false; _btn.removeAttribute('aria-busy'); } return; }
  try {
    const r = await fetchPost(EP.STORAGE_POOLS(), { name, disks: disks || '', raid_level: raid || '' });
    if (r.error) { toast('Pool create failed: ' + (r.error.message || ''), false); return; }
    toast('Pool created: ' + name); addEvt('Pool created: ' + name);
    closeModal(); renderStorage(document.getElementById('cb'));
  } catch (e) { toast(e.message, false); }
  finally { if (_btn && _btn.tagName === 'BUTTON') { _btn.disabled = false; _btn.removeAttribute('aria-busy'); } }
}

async function poolScrub(name) {
  toast('Scrub started: ' + name);
  try {
    const r = await fetchPost(EP.STORAGE_SCRUB(), { name });
    if (r.error) { toast('Scrub failed: ' + (r.error.message || ''), false); return; }
    toast('Scrub initiated: ' + name); addEvt('Pool scrub: ' + name);
  } catch (e) { toast(e.message, false); }
}

async function poolDestroy(name) {
  /* #5 destroyConfirm — 풀 이름 타이핑 요구 (영구 데이터 손실 방지) */
  destroyConfirm({
    title: 'ZFS Pool 영구 삭제',
    name: name,
    warning: 'ZFS 풀 ' + name + '의 모든 데이터가 영구 삭제됩니다. 복구 불가.',
    onConfirm: async function () {
      try {
        const r = await fetchDelete(EP.STORAGE_POOLS(), { name });
        if (r.error) { showToastQueued('Pool destroy failed: ' + (r.error.message || ''), false); return; }
        showToastQueued('Pool destroyed: ' + name);
        addEvt('Pool destroyed: ' + name);
        invalidateCache('storage');
        renderStorage(document.getElementById('cb'));
      } catch (e) { reportError('poolDestroy', e); }
    }
  });
}

async function showZvol() {
  var pool = 'pcvpool/vms';
  try { var cfg = await fetchGet(EP.CONFIG_DAEMON()); var d = cfg.result || cfg.data || cfg; if (d.storage && d.storage.zvol_pool) pool = d.storage.zvol_pool; } catch(e) {}
  showModal(`<h2>${t('btn.create')} Zvol</h2>`
    + `<div class="fr"><label>Name</label><input id="zn" placeholder="data-disk" oninput="document.getElementById('zvol-preview').textContent='${escapeHtml(pool)}/' + (this.value||'name')"></div>`
    + `<div class="fr"><label>Size GB</label><input id="zs" type="number" value="20" min="1" max="2048"></div>`
    + `<div style="margin:8px 0 4px;padding:8px 10px;background:var(--bg3);border:1px solid var(--border);border-radius:6px;font-size:11px;font-family:var(--font-mono)">`
    + `<span class="color-muted">${_L('생성 경로', 'Path')}:</span> <span id="zvol-preview" class="color-accent">${escapeHtml(pool)}/name</span></div>`
    + `<div class="text-right mt-12"><button class="btn btn-g" onclick="doZvol()">${t('btn.create')}</button> <button class="btn btn-r" onclick="closeModal()">${t('btn.cancel')}</button></div>`);
}
async function doZvol() {
  var _btn = document.activeElement;
  if (_btn && _btn.tagName === 'BUTTON') { if (_btn.disabled) return; _btn.disabled = true; _btn.setAttribute('aria-busy', 'true'); }
  var name = (document.getElementById('zn')?.value || '').trim();
  var size = +(document.getElementById('zs')?.value || 0);
  if (!name) { toast(_L('Zvol 이름을 입력하세요', 'Zvol name is required'), false); if (_btn) { _btn.disabled = false; _btn.removeAttribute('aria-busy'); } return; }
  if (!/^[a-zA-Z0-9][a-zA-Z0-9_.-]{0,63}$/.test(name)) { toast(_L('이름: 영문/숫자/_.- 만 허용', 'Name: [a-zA-Z0-9_.-] only'), false); if (_btn) { _btn.disabled = false; _btn.removeAttribute('aria-busy'); } return; }
  if (size < 1 || size > 2048) { toast(_L('크기: 1~2048 GB', 'Size: 1-2048 GB'), false); if (_btn) { _btn.disabled = false; _btn.removeAttribute('aria-busy'); } return; }
  try { await fetchPost(EP.STORAGE_ZVOLS(), { name: name, size_gb: size }); toast(t('stg.zvol_created')); addEvt(t('stg.zvol_created')); closeModal(); renderStorage(document.getElementById('cb')); } catch (e) { toast(e.message, false); }
  finally { if (_btn && _btn.tagName === 'BUTTON') { _btn.disabled = false; _btn.removeAttribute('aria-busy'); } }
}

function zvolDel(name) {
  showModal(`<h2 class="color-red">&#9888; ${t('btn.delete')} Zvol</h2>`
    + `<p class="mb-12">${_L('Zvol을 영구 삭제합니다. 이 작업은 되돌릴 수 없습니다.', 'Permanently destroy this zvol. This action cannot be undone.')}</p>`
    + `<div style="margin-bottom:12px;padding:8px 10px;background:var(--bg3);border:1px solid var(--border);border-radius:6px;font-family:var(--font-mono);font-size:12px"><span class="color-muted">${_L('대상', 'Target')}:</span> <b class="color-accent">${escapeHtml(name)}</b></div>`
    + `<p class="mb-8 text-11">${_L('확인을 위해 아래에 전체 zvol 경로를 입력하세요:', 'Type the full zvol path below to confirm:')}</p>`
    + `<div class="fr"><label>${_L('경로', 'Path')}</label><input id="del-zvol-confirm" placeholder="${escapeHtml(name)}"></div>`
    + `<div class="text-right mt-14"><button class="btn btn-r" onclick="doZvolDel('${escapeAttr(name)}')">${t('btn.delete')}</button> <button class="btn" onclick="closeModal()">${t('btn.cancel')}</button></div>`);
}

async function doZvolDel(name) { const c = document.getElementById('del-zvol-confirm')?.value; if (c !== name) { toast(t('vm.name_mismatch'), false); return; }
  const mc = document.getElementById('mc'); mc.innerHTML = '<h2 class="color-red">&#9888; ' + _L('Zvol 삭제 중', 'Destroying Zvol') + '</h2><p><b class="color-accent">' + escapeHtml(name) + '</b></p><div class="prog-bar"><div class="prog-fill" id="dz-p" class="w-pct-15"></div></div><div class="prog-status" id="dz-s"><span class="spinner"></span>' + _L('삭제 중...', 'Destroying...') + '</div>';
  const pf = document.getElementById('dz-p'), ps = document.getElementById('dz-s');
  try { pf.style.width = '50%';
    const d = await fetchDelete(EP.STORAGE_ZVOLS(), { name: name });
    if (d.error) { pf.style.background = 'var(--red)'; pf.style.width = '100%'; ps.innerHTML = '&#10060; ' + escapeHtml(d.error.message); toast(t('btn.delete') + ' failed', false); return; }
    pf.style.width = '100%'; ps.innerHTML = '&#9989; ' + t('stg.zvol_destroyed'); toast(t('stg.zvol_destroyed')); addEvt(t('stg.zvol_destroyed') + ': ' + name); setTimeout(() => { closeModal(); renderStorage(document.getElementById('cb')); }, 1500);
  } catch (e) { pf.style.width = '100%'; ps.innerHTML = '&#10060; ' + escapeHtml(e.message); toast(e.message, false); } }

/* ═══ STORAGE CAPACITY FORECAST ═══ */
async function loadStorageForecast() {
  var el = document.getElementById('storage-forecast'); if (!el) return;
  el.innerHTML = '<span class="spinner"></span> ' + (t('loading') || 'Loading...');
  try {
    var r = await fetchPost(EP.RPC(), { method: 'storage.pool.forecast', params: {} });
    var d = unwrapData(r);
    var pools = Array.isArray(d) ? d : (d.pools || [d]);
    if (pools.length === 0) { el.innerHTML = '<span class="color-muted">' + (t('storage.no_forecast') || 'No forecast data available') + '</span>'; return; }
    var h = '<table class="text-12"><thead><tr>'
      + '<th>' + (t('storage.pool') || 'Pool') + '</th>'
      + '<th>' + (t('storage.used_pct') || 'Used %') + '</th>'
      + '<th>' + (t('storage.daily_growth') || 'Daily Growth') + '</th>'
      + '<th>' + (t('storage.days_to_full') || 'Days to Full') + '</th>'
      + '<th>' + (t('storage.predicted_date') || 'Predicted Date') + '</th>'
      + '<th>' + (t('storage.status') || 'Status') + '</th>'
      + '</tr></thead><tbody>';
    pools.forEach(function(p) {
      var usedPct = (p.used_percent || p.used_pct || 0).toFixed(1);
      var dailyGrowth = p.daily_growth_gb || p.daily_growth || 0;
      var daysToFull = p.days_to_full || 0;
      var predDate = p.predicted_full_date || p.full_date || '-';
      var severity = _forecastSeverity(daysToFull);
      h += '<tr>';
      h += '<td><b>' + esc(p.name || p.pool || '-') + '</b></td>';
      h += '<td>' + renderProgressBar(parseFloat(usedPct)) + '<span class="text-xs">' + usedPct + '%</span></td>';
      h += '<td>' + (dailyGrowth > 0 ? dailyGrowth.toFixed(2) + ' GB/day' : '<span class="color-muted">stable</span>') + '</td>';
      h += '<td><span style="color:' + severity.color + ';font-weight:700">' + (daysToFull > 0 ? daysToFull + ' ' + (t('storage.days') || 'days') : '&#8734;') + '</span></td>';
      h += '<td><span class="text-11">' + esc(String(predDate)) + '</span></td>';
      h += '<td>' + H.badge(severity.label, severity.badge) + '</td>';
      h += '</tr>';
    });
    h += '</tbody></table>';
    el.innerHTML = h;
  } catch (e) {
    el.innerHTML = '<span class="color-muted">' + (t('storage.forecast_unavailable') || 'Forecast unavailable') + ': ' + esc(e.message) + '</span>';
  }
}

function _forecastSeverity(daysToFull) {
  if (daysToFull <= 0) return { color: 'var(--green)', label: 'Stable', badge: 'g' };
  if (daysToFull < 30) return { color: 'var(--red)', label: 'Critical', badge: 'r' };
  if (daysToFull < 60) return { color: 'var(--yellow)', label: 'Warning', badge: 'y' };
  return { color: 'var(--green)', label: 'Healthy', badge: 'g' };
}

/* ═══ iSCSI TARGETS ═══ */
async function renderIscsi(b) {
  b.innerHTML = showSkeleton();
  try {
    const r = await fetchGet(EP.ISCSI_TARGETS());
    const l = unwrapList(r);
    let h = H.section('iSCSI Targets');
    if (!Array.isArray(l) || l.length === 0) {
      h += '<div class="empty-state"><div class="empty-state-icon">&#128190;</div><div class="empty-state-text">No iSCSI targets configured</div></div>';
    } else {
      h += '<table><thead><tr><th>IQN</th><th>LUN</th><th>Size</th><th>State</th></tr></thead><tbody>';
      l.forEach(function(tgt) {
        h += '<tr><td><b>' + esc(tgt.iqn || tgt.name || '?') + '</b></td><td>' + (tgt.lun || '-') + '</td><td>' + (tgt.size || '-') + '</td><td>' + H.badge(tgt.state || '?', 'g') + '</td></tr>';
      });
      h += '</tbody></table>';
    }
    b.innerHTML = h;
  } catch (e) { b.innerHTML = H.section('iSCSI Targets') + '<p class="color-muted">Failed to load</p>'; }
}
window.renderIscsi = renderIscsi;

/* ═══ BACKUP MANAGEMENT ═══ */
async function renderBackup(b) {
  var h = '<div class="flex items-center gap-10 mb-16"><span class="neon-blink color-cyan">&gt;&gt;</span><h2 style="font-family:var(--font-display);font-size:16px">' + _L('백업 관리', 'Backup Management') + '</h2></div>';

  /* Policy List */
  h += '<div class="hc mb-14"><h4>' + _L('백업 정책', 'Backup Policies') + '</h4>';
  h += '<div class="flex gap-8 mb-8"><button class="btn btn-g" onclick="backupAddPolicy()">' + _L('정책 추가', 'Add Policy') + '</button></div>';
  h += '<div id="backup-policies" class="skeleton-box" style="min-height:100px"></div></div>';

  /* History */
  h += '<div class="hc mb-14"><h4>' + _L('스냅샷 히스토리', 'Snapshot History') + '</h4>';
  h += '<div class="flex gap-8 mb-8"><input id="backup-hist-vm" class="input" placeholder="VM name (empty=all)" class="w-200"><button class="btn" onclick="backupLoadHistory()">' + _L('조회', 'Search') + '</button></div>';
  h += '<div id="backup-history" class="skeleton-box" style="min-height:100px"></div></div>';

  /* Restore */
  h += '<div class="hc mb-14"><h4>' + _L('복원', 'Restore') + '</h4>';
  h += '<p class="stat-label">' + _L('VM의 스냅샷을 선택하여 롤백합니다.', 'Select a VM snapshot to rollback.') + '</p>';
  h += '<div class="flex gap-8 mt-8"><input id="backup-restore-vm" class="input" placeholder="VM name" class="w-160"><input id="backup-restore-snap" class="input" placeholder="Snapshot name" class="w-200"><button class="btn btn-r" onclick="backupRestore()">' + _L('롤백', 'Rollback') + '</button></div></div>';

  b.innerHTML = h;

  /* Load policies */
  try {
    var r = await fetchPost(EP.RPC(), { jsonrpc: '2.0', method: 'backup.policy.list', params: {}, id: 'bp1' });
    var d = unwrapData(r);
    var policies = Array.isArray(d) ? d : (d && d.result ? unwrapList(d) : []);
    var pe = document.getElementById('backup-policies');
    if (pe) {
      if (policies.length === 0) {
        pe.innerHTML = '<div class="stat-label">' + _L('정책 없음', 'No policies') + '</div>';
      } else {
        var tbl = '<table class="tbl"><tr><th>VM</th><th>' + _L('주기(h)', 'Interval(h)') + '</th><th>' + _L('보존', 'Retention') + '</th><th>' + _L('활성', 'Enabled') + '</th><th></th></tr>';
        policies.forEach(function(p) {
          tbl += '<tr><td>' + esc(p.vm_name || '*') + '</td><td>' + (p.interval_hours || '-') + '</td><td>' + (p.retention_count || '-') + '</td><td>' + (p.enabled ? '<span class="color-green">ON</span>' : '<span class="color-red">OFF</span>') + '</td><td><button class="btn btn-sm btn-r" onclick="backupDeletePolicy(\'' + esc(p.vm_name) + '\')">' + _L('삭제', 'Del') + '</button></td></tr>';
        });
        tbl += '</table>';
        pe.innerHTML = tbl;
      }
    }
  } catch (e) {
    var pe2 = document.getElementById('backup-policies');
    if (pe2) pe2.innerHTML = '<div class="color-red">' + esc(e.message) + '</div>';
  }
}

function backupAddPolicy() {
  showModal(
    '<h2>' + _L('백업 정책 추가', 'Add Backup Policy') + '</h2>'
    + '<div class="fr"><label>VM</label><input id="bp-vm" class="input" placeholder="VM name (* = all)" value="*"></div>'
    + '<div class="fr"><label>' + _L('주기(시간)', 'Interval (hours)') + '</label><input id="bp-interval" class="input" type="number" value="24" min="1"></div>'
    + '<div class="fr"><label>' + _L('보존 수', 'Retention count') + '</label><input id="bp-retention" class="input" type="number" value="7" min="1"></div>'
    + '<div class="text-right mt-12"><button class="btn btn-g" onclick="doBackupAddPolicy()">' + _L('추가', 'Add') + '</button> <button class="btn btn-r" onclick="closeModal()">' + _L('취소', 'Cancel') + '</button></div>'
  );
}

async function doBackupAddPolicy() {
  var vm = (document.getElementById('bp-vm').value || '').trim() || '*';
  var interval = parseInt(document.getElementById('bp-interval').value) || 24;
  var retention = parseInt(document.getElementById('bp-retention').value) || 7;
  try {
    var r = await fetchPost(EP.RPC(), { jsonrpc:'2.0', method:'backup.policy.set', params:{ vm_name:vm, interval_hours:interval, retention_count:retention, enabled:true }, id:'bps1' });
    if (r.error) { toast(r.error.message || 'Failed', false); return; }
    toast(_L('정책 추가됨', 'Policy added'));
    closeModal();
    renderContent();
  } catch (e) { toast(e.message, false); }
}

async function backupLoadHistory() {
  var vm = (document.getElementById('backup-hist-vm').value || '').trim();
  var params = vm ? { vm_name: vm } : {};
  try {
    var r = await fetchPost(EP.RPC(), { jsonrpc:'2.0', method:'backup.history', params:params, id:'bh1' });
    var d = unwrapData(r);
    var snaps = Array.isArray(d) ? d : unwrapList(d);
    var el = document.getElementById('backup-history');
    if (!el) return;
    if (snaps.length === 0) { el.innerHTML = '<div class="stat-label">' + _L('스냅샷 없음', 'No snapshots') + '</div>'; return; }
    var tbl = '<table class="tbl"><tr><th>VM</th><th>Snapshot</th><th>Date</th></tr>';
    snaps.forEach(function(s) {
      tbl += '<tr><td>' + esc(s.vm_name || s.vm || '-') + '</td><td>' + esc(s.snapshot || s.name || '-') + '</td><td>' + esc(s.created_at || s.timestamp || '-') + '</td></tr>';
    });
    el.innerHTML = tbl + '</table>';
  } catch (e) {
    var el2 = document.getElementById('backup-history');
    if (el2) el2.innerHTML = '<div class="color-red">' + esc(e.message) + '</div>';
  }
}

async function backupRestore() {
  var vm = (document.getElementById('backup-restore-vm').value || '').trim();
  var snap = (document.getElementById('backup-restore-snap').value || '').trim();
  if (!vm || !snap) { toast(_L('VM과 스냅샷 이름을 입력하세요', 'Enter VM and snapshot name'), false); return; }
  if (!confirm(_L('정말 롤백하시겠습니까? 현재 상태를 잃을 수 있습니다.', 'Are you sure? This may lose current state.'))) return;
  try {
    var r = await fetchPost(EP.RPC(), { jsonrpc:'2.0', method:'backup.restore', params:{ vm_name:vm, snapshot:snap }, id:'br1' });
    if (r.error) { toast(r.error.message || 'Failed', false); return; }
    toast(_L('롤백 시작됨', 'Rollback started'));
  } catch (e) { toast(e.message, false); }
}

async function backupDeletePolicy(vm) {
  try {
    var r = await fetchPost(EP.RPC(), { jsonrpc:'2.0', method:'backup.policy.delete', params:{ vm_name:vm }, id:'bd1' });
    if (r.error) { toast(r.error.message, false); return; }
    toast(_L('정책 삭제됨', 'Policy removed'));
    renderContent();
  } catch (e) { toast(e.message, false); }
}

window.renderBackup = renderBackup;
window.backupAddPolicy = backupAddPolicy;
window.doBackupAddPolicy = doBackupAddPolicy;
window.backupLoadHistory = backupLoadHistory;
window.backupRestore = backupRestore;
window.backupDeletePolicy = backupDeletePolicy;

/* ═══ REGISTER ALL ON window ═══ */
window.renderStorage = renderStorage;
window.showPoolCreate = showPoolCreate;
window.doPoolCreate = doPoolCreate;
window.poolScrub = poolScrub;
window.poolDestroy = poolDestroy;
window.showZvol = showZvol;
window.doZvol = doZvol;
window.zvolDel = zvolDel;
window.doZvolDel = doZvolDel;
window.loadStorageForecast = loadStorageForecast;

/* ─── Multi-select bulk + 컨텍스트 메뉴 (#7/#8) ─── */
function zvolToggleSel(name, on) {
  if (!window._zvolSel) window._zvolSel = new Set();
  if (on) window._zvolSel.add(name); else window._zvolSel.delete(name);
  var bd = document.getElementById('zvol-bulk-del');
  var sc = document.getElementById('zvol-sel-count');
  if (bd) bd.style.display = window._zvolSel.size > 0 ? '' : 'none';
  if (sc) sc.textContent = window._zvolSel.size;
}
function zvolToggleAll(on) {
  if (!window._zvolSel) window._zvolSel = new Set();
  document.querySelectorAll('input[type=checkbox][onclick^="zvolToggleSel"]').forEach(function(cb) {
    cb.checked = on;
    var m = cb.getAttribute('onclick').match(/zvolToggleSel\('([^']+)'/);
    if (m) { if (on) window._zvolSel.add(m[1]); else window._zvolSel.delete(m[1]); }
  });
  var bd = document.getElementById('zvol-bulk-del');
  var sc = document.getElementById('zvol-sel-count');
  if (bd) bd.style.display = window._zvolSel.size > 0 ? '' : 'none';
  if (sc) sc.textContent = window._zvolSel.size;
}
async function zvolBulkDelete() {
  if (!window._zvolSel || window._zvolSel.size === 0) return;
  var names = Array.from(window._zvolSel);
  var ok = await customConfirm(_L('일괄 Zvol 삭제', 'Bulk Zvol Delete'),
    _L('선택한 ', 'Delete ') + names.length + _L(' 개 Zvol을 영구 삭제합니다. 되돌릴 수 없습니다.', ' zvols permanently? Cannot be undone.'));
  if (!ok) return;
  var failed = [];
  for (var i = 0; i < names.length; i++) {
    try {
      var r = await fetchDelete(EP.STORAGE_ZVOLS(), { name: names[i] });
      if (r && r.error) failed.push(names[i] + ': ' + (r.error.message || ''));
    }
    catch (e) { failed.push(names[i] + ': ' + e.message); }
  }
  window._zvolSel.clear();
  if (failed.length === 0) {
    if (typeof showToastQueued === 'function') showToastQueued(names.length + ' zvols deleted');
    else toast(names.length + ' zvols deleted');
  } else {
    if (typeof toastRetry === 'function') {
      toastRetry(failed.length + ' / ' + names.length + ' failed (R to retry)', { fn: zvolBulkDelete });
    } else {
      toast(failed.length + ' / ' + names.length + ' failed', false);
    }
  }
  renderStorage(document.getElementById('cb'));
}
function zvolCtxMenu(ev, name) {
  if (typeof showCtxMenu === 'function') {
    showCtxMenu(ev, [
      { label: '&#128465; ' + t('btn.delete'), fn: function(){ zvolDel(name); } },
      { label: '&#9881; ' + _L('선택', 'Select'), fn: function(){ zvolToggleSel(name, !window._zvolSel.has(name)); renderStorage(document.getElementById('cb')); } }
    ]);
  } else {
    /* fallback: 단순 confirm */
    if (window.confirm(_L('삭제하시겠습니까? ', 'Delete? ') + name)) zvolDel(name);
  }
}
window.zvolToggleSel = zvolToggleSel;
window.zvolToggleAll = zvolToggleAll;
window.zvolBulkDelete = zvolBulkDelete;
window.zvolCtxMenu = zvolCtxMenu;

/* ═══ PCV.storage namespace export ═══ */
PCV.storage = {
  renderStorage: renderStorage,
  showPoolCreate: showPoolCreate,
  doPoolCreate: doPoolCreate,
  poolScrub: poolScrub,
  poolDestroy: poolDestroy,
  showZvol: showZvol,
  doZvol: doZvol,
  zvolDel: zvolDel,
  doZvolDel: doZvolDel,
  loadStorageForecast: loadStorageForecast,
  renderIscsi: renderIscsi,
  renderBackup: renderBackup,
  backupAddPolicy: backupAddPolicy,
  doBackupAddPolicy: doBackupAddPolicy,
  backupLoadHistory: backupLoadHistory,
  backupRestore: backupRestore,
  backupDeletePolicy: backupDeletePolicy,
  zvolToggleSel: zvolToggleSel,
  zvolToggleAll: zvolToggleAll,
  zvolBulkDelete: zvolBulkDelete,
  zvolCtxMenu: zvolCtxMenu
};

})(window.PCV);
