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

/* ADR-013 DOM-safe: renderProgressBar(ui.js 문자열 헬퍼, 수정 금지)의 노드 등가물 —
 * class/구조 동형 (app.js _progressBar 선례). */
function _progressBar(p, c) {
  var mk = PCV.uxlib.el;
  var cl = p > 85 ? 'var(--red)' : p > 60 ? 'var(--yellow)' : 'var(--green)';
  var anim = p > 85 ? ' pulse-anim' : '';
  return mk('div', { class: 'pb' + anim },
    mk('div', { class: 'pb-f scan-anim', style: 'width:' + p + '%;background:' + (c || cl) }),
    mk('div', { class: 'pb-t' }, p.toFixed(1) + '%'));
}

async function renderStorage(b) {
  showSkeleton(b);
  /* ADR-013 DOM-safe: `h +=` 문자열 누적 대신 최상위 형제 노드 배열 parts 에
   * el/HN 노드를 push, 마지막에 clearEl+frag 로 일괄 삽입 (network.js 선례). */
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  try {
    const p = await fetchGet(EP.STORAGE_POOLS());
    const pl = unwrapList(p);
    var parts = [el('div', { class: 'ops-section-heading' },
      el('div', null,
        el('h3', null, _L('스토리지 운영 개요', 'Storage operations overview')),
        el('p', null, _L('풀 상태, 용량 계획, Zvol 작업을 한 화면에서 정리합니다.', 'Review pool health, capacity planning, and zvol operations in one place.'))),
      el('button', { class: 'btn btn-g', onclick: 'showPoolCreate()' }, '+ ' + _L('풀 생성', 'Create pool')))];
    if (pl.length === 0) {
      parts.push(el('div', { class: 'empty-state', style: 'text-align:center;padding:40px 20px' },
        el('div', { style: 'font-size:48px;margin-bottom:12px;opacity:.5' }, '💾'),
        el('div', { style: 'font-size:14px;color:var(--fg2);margin-bottom:16px' }, _L('구성된 ZFS 풀이 없습니다', 'No configured ZFS pools')),
        el('button', { class: 'btn btn-g', onclick: 'showPoolCreate()' }, '+ ' + _L('풀 생성', 'Create pool'))));
    }
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
      parts.push(el('div', { class: 'sg grid-3' },
        HN.card(_L('풀 상태', 'Pool health'), [
          el('div', { class: 'stat-lg color-accent' }, pl.length),
          HN.row(_L('정상', 'Healthy'), el('span', { class: 'color-green' }, pl.length - warningPools)),
          HN.row(_L('주의 필요', 'Needs attention'), el('span', { class: 'color-yellow' }, warningPools))]),
        HN.card(_L('사용 중 용량', 'Used capacity'), [
          el('div', { class: 'stat-lg color-green' }, fmtBytes(usedBytes)),
          _progressBar(Math.min(totalPct, 100)),
          HN.row(_L('전체', 'Total'), fmtBytes(totalBytes)),
          HN.row(_L('사용률', 'Usage'), storagePctText(totalBytes, usedBytes))]),
        HN.card(_L('운영 원칙', 'Operating rule'),
          el('div', { class: 'stat-label', style: 'line-height:1.7' }, _L('스크럽과 삭제는 풀 상태를 먼저 확인한 뒤 실행합니다.', 'Run scrub and destroy only after checking pool health.')))));
      parts.push(el('div', { class: 'ops-section-heading' },
        el('div', null,
          el('h3', null, _L('풀 상태', 'Pool status')),
          el('p', null, _L('각 풀의 용량과 건강 상태를 확인한 뒤 유지보수 작업을 선택합니다.', 'Review each pool before choosing maintenance actions.')))));
    }
    pl.forEach(v => {
      const sz = parseSize(v.size), us = parseSize(v.alloc || v.used), pct = storagePct(sz, us);
      parts.push(HN.card(
        ['💾 ' + v.name + ' ', HN.badge(v.health, v.health === 'ONLINE' ? 'g' : 'r')],
        [
          HN.row(_L('총 용량', 'Total size'), fmtBytes(sz)),
          HN.row(_L('사용량', 'Used'), fmtBytes(us)),
          HN.row(_L('건강 상태', 'Health'), v.health || '-'),
          _progressBar(Math.min(pct, 100)),
          HN.row(_L('사용률', 'Usage'), storagePctText(sz, us)),
          el('div', { class: 'flex gap-4 ops-action-row', style: 'margin-top:10px' },
            el('button', { class: 'btn btn-soft', style: 'font-size:10px;padding:3px 8px', onclick: "poolScrub('" + escapeAttr(v.name) + "')" }, '🔄 ' + _L('스크럽', 'Scrub')),
            el('button', { class: 'btn btn-r', style: 'font-size:10px;padding:3px 8px', onclick: "poolDestroy('" + escapeAttr(v.name) + "')" }, '🗑 ' + _L('영구 삭제', 'Destroy')))
        ],
        'mb-8'));
    });
    /* Storage usage donut */
    if (pl.length > 0) {
      parts.push(el('div', { class: 'sg grid-2' }, pl.map(function(v, pi) {
        var sz = parseSize(v.size), us = parseSize(v.alloc || v.used), pct = storagePct(sz, us);
        return HN.card(v.name + ' ' + _L('사용량', 'Usage'), [
          el('div', { style: 'position:relative;width:120px;height:120px;margin:0 auto' },
            el('canvas', { id: 'pool-donut-' + pi, width: 120, height: 120 }),
            el('div', { style: 'position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);font-size:16px;font-weight:700;color:var(--accent)' }, pct.toFixed(0) + '%')),
          HN.row(_L('전체', 'Total'), fmtBytes(sz)),
          HN.row(_L('사용', 'Used'), fmtBytes(us))
        ]);
      })));
    }

    /* Storage forecast panel */
    parts.push(el('div', { class: 'hc mb-14' },
      el('h4', null, '📈 ' + _L('용량 예측', 'Capacity planning')),
      el('p', { class: 'color-muted text-11 mb-8' }, _L('일별 증가량 기준으로 풀 소진 시점을 예측합니다. 확장이나 정리 시점을 먼저 판단하는 용도입니다.', 'Forecast pool exhaustion based on daily growth so you can plan expansion or cleanup ahead of time.')),
      el('div', { id: 'storage-forecast' }, el('span', { class: 'spinner' }), ' ' + (t('loading') || 'Loading...'))));
    setTimeout(loadStorageForecast, 100);

    const z = await fetchGet(EP.STORAGE_ZVOLS());
    const zl = unwrapList(z);
    parts.push(el('div', { class: 'ops-section-heading' },
      el('div', null,
        el('h3', null, _L('Zvol 볼륨', 'Zvol volumes')),
        el('p', null, _L('디스크 볼륨은 표로 관리하고, 대량 삭제는 선택 상태에서만 노출합니다.', 'Manage disk volumes in a table and expose bulk delete only after selection.'))),
      el('div', { class: 'flex gap-8 ops-action-row' },
        el('button', { class: 'btn btn-primary', onclick: 'showZvol()' }, '+ ' + t('btn.create')),
        ' ',
        el('button', { class: 'btn btn-r', id: 'zvol-bulk-del', style: 'display:none', onclick: 'zvolBulkDelete()' },
          '🗑 ' + _L('선택 삭제', 'Delete Selected') + ' (', el('span', { id: 'zvol-sel-count' }, '0'), ')'))));
    if (zl.length === 0) {
      parts.push(el('div', { class: 'empty-state' },
        el('div', { class: 'empty-state-icon' }, '💾'),
        el('div', { class: 'empty-state-text' }, _L('아직 생성된 Zvol이 없습니다', 'No zvols created yet')),
        el('div', { class: 'color-muted text-12' }, _L('추가 디스크가 필요할 때 생성 버튼으로 바로 만들 수 있습니다.', 'Use the create button when a workload needs an additional disk.'))));
      clearEl(b);
      b.appendChild(frag(parts));
      setTimeout(function() { pl.forEach(function(v, pi) { var canvas = document.getElementById('pool-donut-' + pi); if (!canvas) return; var ctx = canvas.getContext('2d'); var sz = parseSize(v.size), us = parseSize(v.alloc || v.used); var pct = sz > 0 ? us / sz : 0; var r = 50, cx = 60, cy = 60, lw = 14; ctx.lineWidth = lw; ctx.beginPath(); ctx.arc(cx, cy, r, 0, Math.PI * 2); ctx.strokeStyle = 'rgba(255,255,255,0.1)'; ctx.stroke(); ctx.beginPath(); ctx.arc(cx, cy, r, -Math.PI/2, -Math.PI/2 + Math.PI * 2 * pct); try { ctx.strokeStyle = getComputedStyle(document.documentElement).getPropertyValue(pct > 0.85 ? '--red' : pct > 0.6 ? '--yellow' : '--green').trim(); } catch(e) {} ctx.stroke(); }); }, 100);
      return;
    }
    if (!window._zvolSel) window._zvolSel = new Set();
    var thead = el('thead', null, el('tr', null,
      el('th', null, el('input', { type: 'checkbox', id: 'zvol-all', 'aria-label': _L('전체 선택', 'Select all zvols'), onclick: 'zvolToggleAll(this.checked)' })),
      el('th', null, 'Path'), el('th', null, 'Size'), el('th', null, 'Used'),
      el('th', null, _L('압축비', 'Compress')), el('th', null, 'Dedup'), el('th', null, 'Written'),
      el('th', null, t('vm.settings'))));
    var tbody = el('tbody', null, zl.map(function(v) {
      const vn = v.name || v.path;
      const selected = window._zvolSel.has(vn);
      return el('tr', { class: selected ? 'multi-selected' : null, oncontextmenu: "event.preventDefault();zvolCtxMenu(event,'" + escapeAttr(vn) + "')" },
        el('td', null, el('input', { type: 'checkbox', checked: selected ? '' : null, 'aria-label': vn, onclick: "zvolToggleSel('" + escapeAttr(vn) + "',this.checked)" })),
        el('td', null, el('span', { class: 'text-ellipsis', title: vn }, vn)),
        el('td', null, v.volsize || v.size || '-'),
        el('td', null, v.used || '-'),
        el('td', null, v.compression_ratio || '-'),
        el('td', null, v.dedup || '-'),
        el('td', null, v.written || '-'),
        el('td', null, el('button', { class: 'btn btn-r', style: 'font-size:10px;padding:3px 8px', onclick: "zvolDel('" + escapeAttr(vn) + "')" }, t('btn.delete'))));
    }));
    parts.push(el('table', { class: 'table-sticky' }, thead, tbody));
    clearEl(b);
    b.appendChild(frag(parts));
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
  var el = PCV.uxlib.el;
  showModal([
    el('h2', null, 'Create ZFS Pool'),
    el('div', { class: 'fr' },
      el('label', { for: 'pc-name' }, 'Pool Name'),
      el('input', { id: 'pc-name', placeholder: 'newpool' })),
    el('div', { class: 'fr' },
      el('label', { for: 'pc-disks' }, 'Disks (space sep)'),
      el('input', { id: 'pc-disks', placeholder: '/dev/sdb /dev/sdc' })),
    el('div', { class: 'fr' },
      el('label', { for: 'pc-raid' }, 'RAID Level'),
      el('select', { id: 'pc-raid', style: 'width:100%;padding:6px;background:var(--bg);border:1px solid var(--border);color:var(--fg);border-radius:4px' },
        el('option', { value: '' }, 'Stripe (default)'),
        el('option', { value: 'mirror' }, 'Mirror'),
        el('option', { value: 'raidz' }, 'RAIDZ'),
        el('option', { value: 'raidz2' }, 'RAIDZ2'))),
    el('div', { class: 'text-right mt-12' },
      el('button', { class: 'btn btn-g', onclick: 'doPoolCreate()' }, t('btn.create')),
      ' ',
      el('button', { class: 'btn btn-r', onclick: 'closeModal()' }, t('btn.cancel')))
  ]);
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
  var el = PCV.uxlib.el;
  showModal([
    el('h2', null, t('btn.create') + ' Zvol'),
    el('div', { class: 'fr' },
      el('label', { for: 'zn' }, 'Name'),
      el('input', { id: 'zn', placeholder: 'data-disk', oninput: "document.getElementById('zvol-preview').textContent='" + escapeHtml(pool) + "/' + (this.value||'name')" })),
    el('div', { class: 'fr' },
      el('label', { for: 'zs' }, 'Size GB'),
      el('input', { id: 'zs', type: 'number', value: '20', min: '1', max: '2048' })),
    el('div', { style: 'margin:8px 0 4px;padding:8px 10px;background:var(--bg3);border:1px solid var(--border);border-radius:6px;font-size:11px;font-family:var(--font-mono)' },
      el('span', { class: 'color-muted' }, _L('생성 경로', 'Path') + ':'),
      ' ',
      el('span', { id: 'zvol-preview', class: 'color-accent' }, pool + '/name')),
    el('div', { class: 'text-right mt-12' },
      el('button', { class: 'btn btn-g', onclick: 'doZvol()' }, t('btn.create')),
      ' ',
      el('button', { class: 'btn btn-r', onclick: 'closeModal()' }, t('btn.cancel')))
  ]);
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
  var el = PCV.uxlib.el;
  showModal([
    el('h2', { class: 'color-red' }, '⚠ ' + t('btn.delete') + ' Zvol'),
    el('p', { class: 'mb-12' }, _L('Zvol을 영구 삭제합니다. 이 작업은 되돌릴 수 없습니다.', 'Permanently destroy this zvol. This action cannot be undone.')),
    el('div', { style: 'margin-bottom:12px;padding:8px 10px;background:var(--bg3);border:1px solid var(--border);border-radius:6px;font-family:var(--font-mono);font-size:12px' },
      el('span', { class: 'color-muted' }, _L('대상', 'Target') + ':'),
      ' ',
      el('b', { class: 'color-accent' }, name)),
    el('p', { class: 'mb-8 text-11' }, _L('확인을 위해 아래에 전체 zvol 경로를 입력하세요:', 'Type the full zvol path below to confirm:')),
    el('div', { class: 'fr' },
      el('label', { for: 'del-zvol-confirm' }, _L('경로', 'Path')),
      el('input', { id: 'del-zvol-confirm', placeholder: escapeHtml(name) })),
    el('div', { class: 'text-right mt-14' },
      el('button', { class: 'btn btn-r', onclick: "doZvolDel('" + escapeAttr(name) + "')" }, t('btn.delete')),
      ' ',
      el('button', { class: 'btn', onclick: 'closeModal()' }, t('btn.cancel')))
  ]);
}

async function doZvolDel(name) { const c = document.getElementById('del-zvol-confirm')?.value; if (c !== name) { toast(t('vm.name_mismatch'), false); return; }
  /* ADR-013 DOM-safe: 진행 모달을 el/frag 로 조립. 아이콘 HTML 엔티티(&#9888; 등)는
   * 동일 코드포인트 리터럴 글리프로, escapeHtml(name) 은 TextNode 로 대체(이스케이프 불요).
   * (원본 prog-fill div 는 class 속성이 중복(class="prog-fill"..class="w-pct-15")이라
   *  HTML 파서상 첫 class="prog-fill" 만 적용되고 w-pct-15 는 무시됨 — 렌더 동등 보존.) */
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  const mc = document.getElementById('mc');
  clearEl(mc);
  mc.appendChild(frag(
    el('h2', { class: 'color-red' }, '⚠ ' + _L('Zvol 삭제 중', 'Destroying Zvol')),
    el('p', null, el('b', { class: 'color-accent' }, name)),
    el('div', { class: 'prog-bar' }, el('div', { class: 'prog-fill', id: 'dz-p' })),
    el('div', { class: 'prog-status', id: 'dz-s' }, el('span', { class: 'spinner' }), _L('삭제 중...', 'Destroying...'))
  ));
  const pf = document.getElementById('dz-p'), ps = document.getElementById('dz-s');
  try { pf.style.width = '50%';
    const d = await fetchDelete(EP.STORAGE_ZVOLS(), { name: name });
    if (d.error) { pf.style.background = 'var(--red)'; pf.style.width = '100%'; ps.textContent = '❌ ' + d.error.message; toast(t('btn.delete') + ' failed', false); return; }
    pf.style.width = '100%'; ps.textContent = '✅ ' + t('stg.zvol_destroyed'); toast(t('stg.zvol_destroyed')); addEvt(t('stg.zvol_destroyed') + ': ' + name); setTimeout(() => { closeModal(); renderStorage(document.getElementById('cb')); }, 1500);
  } catch (e) { pf.style.width = '100%'; ps.textContent = '❌ ' + e.message; toast(e.message, false); } }

/* ═══ STORAGE CAPACITY FORECAST ═══ */
async function loadStorageForecast() {
  var el = document.getElementById('storage-forecast'); if (!el) return;
  /* ADR-013 DOM-safe: el 지역변수는 DOM 노드라 빌더는 PCV.uxlib.* 로 직접 호출.
   * 표 본문은 _progressBar(renderProgressBar 노드 등가물)/HN.badge 로 조립. */
  PCV.uxlib.clearEl(el);
  el.appendChild(PCV.uxlib.frag(PCV.uxlib.el('span', { class: 'spinner' }), ' ' + (t('loading') || 'Loading...')));
  try {
    var r = await fetchPost(EP.RPC(), { method: 'storage.pool.forecast', params: {} });
    var d = unwrapData(r);
    var pools = Array.isArray(d) ? d : (d.pools || [d]);
    if (pools.length === 0) { PCV.uxlib.clearEl(el); el.appendChild(PCV.uxlib.el('span', { class: 'color-muted' }, t('storage.no_forecast') || 'No forecast data available')); return; }
    var mk = PCV.uxlib.el;
    var thead = mk('thead', null, mk('tr', null,
      mk('th', null, t('storage.pool') || 'Pool'),
      mk('th', null, t('storage.used_pct') || 'Used %'),
      mk('th', null, t('storage.daily_growth') || 'Daily Growth'),
      mk('th', null, t('storage.days_to_full') || 'Days to Full'),
      mk('th', null, t('storage.predicted_date') || 'Predicted Date'),
      mk('th', null, t('storage.status') || 'Status')));
    var tbody = mk('tbody', null, pools.map(function(p) {
      var usedPct = (p.used_percent || p.used_pct || 0).toFixed(1);
      var dailyGrowth = p.daily_growth_gb || p.daily_growth || 0;
      var daysToFull = p.days_to_full || 0;
      var predDate = p.predicted_full_date || p.full_date || '-';
      var severity = _forecastSeverity(daysToFull);
      return mk('tr', null,
        mk('td', null, mk('b', null, p.name || p.pool || '-')),
        mk('td', null, _progressBar(parseFloat(usedPct)), mk('span', { class: 'text-xs' }, usedPct + '%')),
        mk('td', null, dailyGrowth > 0 ? (dailyGrowth.toFixed(2) + ' GB/day') : mk('span', { class: 'color-muted' }, 'stable')),
        mk('td', null, mk('span', { style: 'color:' + severity.color + ';font-weight:700' }, daysToFull > 0 ? (daysToFull + ' ' + (t('storage.days') || 'days')) : '∞')),
        mk('td', null, mk('span', { class: 'text-11' }, String(predDate))),
        mk('td', null, HN.badge(severity.label, severity.badge)));
    }));
    PCV.uxlib.clearEl(el);
    el.appendChild(mk('table', { class: 'text-12' }, thead, tbody));
  } catch (e) {
    PCV.uxlib.clearEl(el);
    el.appendChild(PCV.uxlib.el('span', { class: 'color-muted' }, (t('storage.forecast_unavailable') || 'Forecast unavailable') + ': ' + e.message));
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
  showSkeleton(b);
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  try {
    const r = await fetchGet(EP.ISCSI_TARGETS());
    const l = unwrapList(r);
    var body;
    if (!Array.isArray(l) || l.length === 0) {
      body = el('div', { class: 'empty-state' },
        el('div', { class: 'empty-state-icon' }, '💾'),
        el('div', { class: 'empty-state-text' }, 'No iSCSI targets configured'));
    } else {
      var tbody = el('tbody', null, l.map(function(tgt) {
        return el('tr', null,
          el('td', null, el('b', null, tgt.iqn || tgt.name || '?')),
          el('td', null, tgt.lun || '-'),
          el('td', null, tgt.size || '-'),
          el('td', null, HN.badge(tgt.state || '?', 'g')));
      }));
      body = el('table', null,
        el('thead', null, el('tr', null, el('th', null, 'IQN'), el('th', null, 'LUN'), el('th', null, 'Size'), el('th', null, 'State'))),
        tbody);
    }
    clearEl(b);
    b.appendChild(frag(HN.section('iSCSI Targets'), body));
  } catch (e) {
    clearEl(b);
    b.appendChild(frag(HN.section('iSCSI Targets'), el('p', { class: 'color-muted' }, 'Failed to load')));
  }
}
window.renderIscsi = renderIscsi;

/* ═══ BACKUP MANAGEMENT ═══ */
async function renderBackup(b) {
  /* ADR-013 DOM-safe: 정적 폼 템플릿을 el/frag 로 조립. 인라인 onclick 문자열은
   * 동일 의미 클로저(window.fn)로, 엔티티 &gt;&gt; 는 리터럴 '>>' TextNode 로.
   * (원본 input 은 class 속성 중복(class="input"..class="w-200")이라 HTML 파서상
   *  첫 class="input" 만 적용, w-200/w-160 은 무시됨 — 렌더 동등 보존.) */
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  clearEl(b);
  b.appendChild(frag(
    el('div', { class: 'flex items-center gap-10 mb-16' },
      el('span', { class: 'neon-blink color-cyan' }, '>>'),
      el('h2', { style: 'font-family:var(--font-display);font-size:16px' }, _L('백업 관리', 'Backup Management'))
    ),
    /* Policy List */
    el('div', { class: 'hc mb-14' },
      el('h4', null, _L('백업 정책', 'Backup Policies')),
      el('div', { class: 'flex gap-8 mb-8' },
        el('button', { class: 'btn btn-g', onClick: function(){ window.backupAddPolicy(); } }, _L('정책 추가', 'Add Policy'))
      ),
      el('div', { id: 'backup-policies', class: 'skeleton-box', style: 'min-height:100px' })
    ),
    /* History */
    el('div', { class: 'hc mb-14' },
      el('h4', null, _L('스냅샷 히스토리', 'Snapshot History')),
      el('div', { class: 'flex gap-8 mb-8' },
        el('input', { 'aria-label': 'VM name (empty=all)', id: 'backup-hist-vm', class: 'input', placeholder: 'VM name (empty=all)' }),
        el('button', { class: 'btn', onClick: function(){ window.backupLoadHistory(); } }, _L('조회', 'Search'))
      ),
      el('div', { id: 'backup-history', class: 'skeleton-box', style: 'min-height:100px' })
    ),
    /* Restore */
    el('div', { class: 'hc mb-14' },
      el('h4', null, _L('복원', 'Restore')),
      el('p', { class: 'stat-label' }, _L('VM의 스냅샷을 선택하여 롤백합니다.', 'Select a VM snapshot to rollback.')),
      el('div', { class: 'flex gap-8 mt-8' },
        el('input', { 'aria-label': 'VM name', id: 'backup-restore-vm', class: 'input', placeholder: 'VM name' }),
        el('input', { 'aria-label': 'Snapshot name', id: 'backup-restore-snap', class: 'input', placeholder: 'Snapshot name' }),
        el('button', { class: 'btn btn-r', onClick: function(){ window.backupRestore(); } }, _L('롤백', 'Rollback'))
      )
    )
  ));

  /* Load policies */
  try {
    var r = await fetchPost(EP.RPC(), { jsonrpc: '2.0', method: 'backup.policy.list', params: {}, id: 'bp1' });
    var d = unwrapData(r);
    var policies = Array.isArray(d) ? d : (d && d.result ? unwrapList(d) : []);
    var pe = document.getElementById('backup-policies');
    if (pe) {
      if (policies.length === 0) {
        clearEl(pe);
        pe.appendChild(el('div', { class: 'stat-label' }, _L('정책 없음', 'No policies')));
      } else {
        /* 원본 '<table class="tbl"><tr>...' 는 파서가 tbody 를 자동 삽입 →
         * CSS 'tbody tr' 규칙(hover/border-left) 적용. DOM 빌드는 자동 삽입이
         * 없으므로 명시적 tbody 로 감싸 렌더/hover 동등 보존. */
        var tbody = el('tbody', null,
          el('tr', null,
            el('th', null, 'VM'),
            el('th', null, _L('주기(h)', 'Interval(h)')),
            el('th', null, _L('보존', 'Retention')),
            el('th', null, _L('활성', 'Enabled')),
            el('th')
          )
        );
        policies.forEach(function(p) {
          tbody.appendChild(el('tr', null,
            el('td', null, p.vm_name || '*'),
            el('td', null, p.interval_hours || '-'),
            el('td', null, p.retention_count || '-'),
            el('td', null, p.enabled ? el('span', { class: 'color-green' }, 'ON') : el('span', { class: 'color-red' }, 'OFF')),
            el('td', null, el('button', { class: 'btn btn-sm btn-r', onClick: function(){ window.backupDeletePolicy(p.vm_name); } }, _L('삭제', 'Del')))
          ));
        });
        clearEl(pe);
        pe.appendChild(el('table', { class: 'tbl' }, tbody));
      }
    }
  } catch (e) {
    var pe2 = document.getElementById('backup-policies');
    if (pe2) { clearEl(pe2); pe2.appendChild(el('div', { class: 'color-red' }, e.message)); }
  }
}

function backupAddPolicy() {
  var el = PCV.uxlib.el;
  showModal([
    el('h2', null, _L('백업 정책 추가', 'Add Backup Policy')),
    el('div', { class: 'fr' },
      el('label', { for: 'bp-vm' }, 'VM'),
      el('input', { id: 'bp-vm', class: 'input', placeholder: 'VM name (* = all)', value: '*' })),
    el('div', { class: 'fr' },
      el('label', { for: 'bp-interval' }, _L('주기(시간)', 'Interval (hours)')),
      el('input', { id: 'bp-interval', class: 'input', type: 'number', value: '24', min: '1' })),
    el('div', { class: 'fr' },
      el('label', { for: 'bp-retention' }, _L('보존 수', 'Retention count')),
      el('input', { id: 'bp-retention', class: 'input', type: 'number', value: '7', min: '1' })),
    el('div', { class: 'text-right mt-12' },
      el('button', { class: 'btn btn-g', onclick: 'doBackupAddPolicy()' }, _L('추가', 'Add')),
      ' ',
      el('button', { class: 'btn btn-r', onclick: 'closeModal()' }, _L('취소', 'Cancel')))
  ]);
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
    /* ADR-013 DOM-safe: el/el2 는 DOM 노드 지역변수라 빌더는 PCV.uxlib.* 로 직접 호출. */
    if (snaps.length === 0) { PCV.uxlib.clearEl(el); el.appendChild(PCV.uxlib.el('div', { class: 'stat-label' }, _L('스냅샷 없음', 'No snapshots'))); return; }
    /* 명시적 tbody 로 감싸 파서 자동삽입 tbody 와 동등('tbody tr' CSS 적용). */
    var tbody = PCV.uxlib.el('tbody', null,
      PCV.uxlib.el('tr', null,
        PCV.uxlib.el('th', null, 'VM'),
        PCV.uxlib.el('th', null, 'Snapshot'),
        PCV.uxlib.el('th', null, 'Date')
      )
    );
    snaps.forEach(function(s) {
      tbody.appendChild(PCV.uxlib.el('tr', null,
        PCV.uxlib.el('td', null, s.vm_name || s.vm || '-'),
        PCV.uxlib.el('td', null, s.snapshot || s.name || '-'),
        PCV.uxlib.el('td', null, s.created_at || s.timestamp || '-')
      ));
    });
    PCV.uxlib.clearEl(el);
    el.appendChild(PCV.uxlib.el('table', { class: 'tbl' }, tbody));
  } catch (e) {
    var el2 = document.getElementById('backup-history');
    if (el2) { PCV.uxlib.clearEl(el2); el2.appendChild(PCV.uxlib.el('div', { class: 'color-red' }, e.message)); }
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
      { label: '🗑 ' + t('btn.delete'), fn: function(){ zvolDel(name); } },
      { label: '⚙ ' + _L('선택', 'Select'), fn: function(){ zvolToggleSel(name, !window._zvolSel.has(name)); renderStorage(document.getElementById('cb')); } }
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
