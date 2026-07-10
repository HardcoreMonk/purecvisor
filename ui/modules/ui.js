/**
 * @module ui
 * @description PureCVisor UI 유틸리티 — HTML 빌더, 토스트, 모달, 이벤트 로그, 테마
 * ADR-0013: IIFE 모듈 스코프 전환 — window.PCV.ui 네임스페이스
 */

/*
 * ===== ui.js 모듈 개요 (주니어 개발자 필독) =====
 *
 * [역할]
 *   UI 공용 유틸리티 모듈. 다른 모든 페이지 모듈(vm.js, container.js 등)이
 *   이 모듈의 함수를 사용하여 HTML을 생성하고, 토스트를 표시하고,
 *   모달을 열고 닫는다. DOM 조작의 "표준 라이브러리" 역할.
 *
 * [PCV 네임스페이스 (ADR-0013)]
 *   IIFE 안에서 정의 후 PCV.ui = { ... }로 노출.
 *   하위 호환 심(window.toast = toast 등)은 전환기 코드.
 *
 * [XSS 방지: escapeHtml vs escapeAttr — 반드시 구분]
 *
 *   escapeHtml(s): HTML 태그/엔티티 이스케이프 (&, <, >, ", ').
 *     용도: innerHTML에 사용자 문자열을 삽입할 때.
 *     예: '<b>' + escapeHtml(v.name) + '</b>'
 *
 *   escapeAttr(s): 모든 비-알파뉴메릭을 \xHH로 변환.
 *     용도: onclick="func('${escapeAttr(val)}')" 같은 인라인 JS 문자열 안.
 *     왜 escapeHtml로는 부족한가:
 *       HTML 파서가 onclick 속성값의 &quot;를 먼저 "로 디코딩한 뒤
 *       JS 엔진에 넘긴다. 따라서 사용자 입력에 ' 또는 \가 있으면
 *       JS 문자열 리터럴을 탈출할 수 있다 (XSS).
 *       escapeAttr은 모든 특수문자를 \x27 같은 JS 이스케이프로 바꿔
 *       HTML + JS 양쪽에서 안전하다.
 *
 *   실전 예:
 *     잘못: onclick="doSomething('${escapeHtml(userInput)}')"  // XSS 가능!
 *     올바름: onclick="doSomething('${escapeAttr(userInput)}')"
 *
 * [H 빌더 객체]
 *   H.card(), H.row(), H.badge() 등은 반복되는 HTML 패턴을 함수화한 것.
 *   템플릿 리터럴로 HTML 문자열을 반환한다 (DOM 요소가 아님).
 *   innerHTML += H.card('Title', H.row('key', 'val')) 형태로 합성한다.
 *   왜 DOM createElement를 안 쓰나: 성능보다 가독성을 우선한 설계 결정.
 *   수십 개의 카드를 한 번에 그릴 때 innerHTML 할당이 더 빠르다.
 *
 * [토스트 큐]
 *   동시에 최대 3개까지 표시. 초과 시 가장 오래된 것을 즉시 제거.
 *   3초 후 자동 사라짐 + 프로그레스 바 애니메이션.
 *   클릭 시 즉시 제거 (슬라이드 아웃).
 *
 * [모달 스택 (F3)]
 *   showModal()을 중첩 호출하면 이전 모달 HTML이 _modalStack에 push된다.
 *   closeModal() 시 스택에서 pop하여 이전 모달을 복원한다.
 *   스택이 비어있으면 모달 배경(#mbg)을 숨긴다.
 *   모달 내 Tab 키는 포커스 트랩이 적용되어 모달 밖으로 나가지 않는다 (접근성).
 *
 * [DataTable 내부 구조]
 *   createDataTable()은 window['_dt_'+tableId] 전역 객체에
 *   행/헤더/정렬/페이지 상태를 저장한다.
 *   HTML onclick에서 window._dtSort(id, col) 형태로 호출되므로
 *   반드시 window에 등록해야 한다.
 *   검색은 HTML 태그를 strip한 뒤 텍스트에서 필터링한다.
 *   CSV 내보내기도 태그를 제거하여 순수 텍스트만 출력한다.
 *
 * [흔한 실수]
 *   - toast()의 두 번째 인자를 생략하면 ok=true (녹색). false를 넣어야 빨간색.
 *   - showModal(html) 호출 후 50ms setTimeout 없이 getElementById 하면
 *     innerHTML이 아직 파싱되지 않아 null이 반환될 수 있다.
 *   - customConfirm은 Promise를 반환한다. 반드시 await로 받아야 한다.
 *   - withSpinner은 버튼의 dataset.pcvBusy로 더블클릭을 차단한다.
 *     수동으로 disabled 관리하지 말고 이 래퍼를 사용하라.
 */

/* ═══ DEFAULTS ═══ */
if (!window.eventLog) window.eventLog = [];

window.PCV = window.PCV || {};
(function(PCV) {

/* ═══ HTML BUILDER ═══
 *  escapeHtml: innerHTML 삽입용. HTML 5대 특수문자만 이스케이프.
 *  escapeAttr: onclick 등 인라인 JS 문자열용. 모든 비-알파뉴메릭을 \xHH로.
 *  두 함수의 차이를 모르면 XSS 취약점이 생긴다. 위 모듈 주석 참조. */
function escapeHtml(s) {
  if (!s) return '';
  return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;').replace(/'/g, '&#39;');
}
var esc = escapeHtml;

/**
 * escapeAttr — onclick 등 HTML 속성 내 JS 문자열 리터럴 안전 이스케이프.
 * HTML 파서가 엔티티를 디코드한 뒤 JS 엔진에 전달하므로,
 * escapeHtml만으로는 JS 문자열 탈출을 방지할 수 없다.
 * 이 함수는 모든 비-알파뉴메릭을 \\xHH로 변환하여 JS+HTML 양쪽 안전.
 */
function escapeAttr(s) {
  if (!s) return '';
  return String(s).replace(/[^a-zA-Z0-9_.\-]/g, function(c) {
    return '\\x' + c.charCodeAt(0).toString(16).padStart(2, '0');
  });
}

var H = {
  card: (title, body, cls) => `<div class="hc ${cls||''}">${title?'<h4>'+title+'</h4>':''}${body}</div>`,
  row: (key, val, cls) => `<div class="hr"><span class="k">${key}</span><span class="v ${cls||''}">${val}</span></div>`,
  badge: (text, type) => `<span class="badge b-${type}">${escapeHtml(text)}</span>`,
  grid: (cols, content) => `<div class="sg grid-${cols}">${content}</div>`,
  section: (title) => `<h3 class="section-title">${title}</h3>`,
  sectionLg: (title) => `<h3 class="section-title-lg">${title}</h3>`,
};

/* ═══ TOAST ═══ */
function toast(m, ok = true) {
  var icon = ok ? '&#9989; ' : '&#10060; ';
  const d = document.createElement('div');
  d.className = 'toast ' + (ok ? 't-ok' : 't-er');
  d.innerHTML = icon + m;
  d.style.cssText = 'transform:translateX(100%);transition:transform 0.3s ease-out';
  requestAnimationFrame(function() { d.style.transform = 'translateX(0)'; });
  d.onclick = function() { d.style.transform = 'translateX(100%)'; setTimeout(function() { d.remove(); }, 300); };
  const prog = document.createElement('div');
  prog.className = 'toast-progress';
  prog.style.cssText = 'height:3px;background:' + (ok ? 'var(--green)' : 'var(--red)') + ';border-radius:0 0 6px 6px;width:100%;transition:width 2.8s linear';
  d.appendChild(prog);
  const container = document.getElementById('toasts');
  container.appendChild(d);
  while (container.children.length > 3) container.removeChild(container.firstChild);
  requestAnimationFrame(() => { prog.style.width = '0%'; });
  setTimeout(() => d.remove(), 3e3);
  /* B2: Feed toast into notification center */
  if (typeof addNotification === 'function') {
    addNotification(ok ? 'info' : 'error', m, '');
  }
}

/* ═══ EVENT LOG ═══ */
function ciIcon(name) {
  return '<svg class="ci-icon" aria-hidden="true"><use href="/ui/vendor/coolicons/coolicons.svg#ci-' + name + '"></use></svg>';
}

var EVT_ICONS = {
  auth: ciIcon('lock'), ws: ciIcon('globe'), vm: ciIcon('desktop-tower'), ctr: ciIcon('layers'), snap: ciIcon('camera'),
  net: ciIcon('globe'), storage: ciIcon('data'), cluster: ciIcon('layers'), ovn: ciIcon('layers'),
  alert: ciIcon('bell'), gpu: ciIcon('monitor'), docker: ciIcon('layers'), terraform: ciIcon('file-document'),
  federation: ciIcon('cloud'), config: ciIcon('settings'), template: ciIcon('file-document'), backup: ciIcon('save'),
  error: ciIcon('close-circle'), ok: ciIcon('circle-check'), info: ciIcon('info')
};

function _evtIcon(m) {
  const ml = m.toLowerCase();
  if (ml.includes('error') || ml.includes('fail')) return EVT_ICONS.error;
  if (ml.includes('auth') || ml.includes('login') || ml.includes('logout')) return EVT_ICONS.auth;
  if (ml.startsWith('ws')) return EVT_ICONS.ws;
  if (ml.includes('ctr ') || ml.includes('container') || ml.includes('lxc')) return EVT_ICONS.ctr;
  if (ml.includes('snapshot') || ml.includes('rollback')) return EVT_ICONS.snap;
  if (ml.includes('cluster') || ml.includes('drain') || ml.includes('maintenance')) return EVT_ICONS.cluster;
  if (ml.includes('network') || ml.includes('ovn') || ml.includes('acl')) return EVT_ICONS.net;
  if (ml.includes('pool') || ml.includes('zvol') || ml.includes('storage')) return EVT_ICONS.storage;
  if (ml.includes('gpu')) return EVT_ICONS.gpu;
  if (ml.includes('docker') || ml.includes('oci')) return EVT_ICONS.docker;
  if (ml.includes('terraform')) return EVT_ICONS.terraform;
  if (ml.includes('federation')) return EVT_ICONS.federation;
  if (ml.includes('config')) return EVT_ICONS.config;
  if (ml.includes('template')) return EVT_ICONS.template;
  if (ml.includes('backup')) return EVT_ICONS.backup;
  if (ml.includes('vm') || ml.includes('start') || ml.includes('stop') || ml.includes('clone')) return EVT_ICONS.vm;
  return EVT_ICONS.info;
}

function _evtClass(m) {
  const ml = m.toLowerCase();
  if (ml.includes('error') || ml.includes('fail')) return 'color-red';
  if (ml.includes('created') || ml.includes('started') || ml.includes('ok') || ml.includes('completed')) return 'color-green';
  if (ml.includes('stopped') || ml.includes('deleted') || ml.includes('destroyed') || ml.includes('drain')) return 'color-yellow';
  return '';
}

function addEvt(m) {
  const ts = new Date().toLocaleTimeString('ko-KR', { hour12: false, hour: '2-digit', minute: '2-digit', second: '2-digit' });
  const ms = String(Date.now() % 1000).padStart(3, '0');
  const entry = { time: ts + '.' + ms, msg: m, raw: ts + '.' + ms + ' ' + m };
  window.eventLog.push(entry);
  if (window.eventLog.length > 200) window.eventLog.shift();
  const ep = document.getElementById('evp');
  if (ep) {
    ep.innerHTML = window.eventLog.map(e => {
      const icon = _evtIcon(e.msg);
      const cls = _evtClass(e.msg);
      return '<div style="padding:2px 0;border-bottom:1px solid var(--border);display:flex;gap:6px;align-items:baseline;font-size:11px">'
        + '<span class="color-muted" style="font-size:9px;white-space:nowrap;min-width:72px">' + escapeHtml(e.time) + '</span>'
        + '<span style="font-size:12px">' + icon + '</span>'
        + '<span class="' + cls + '" style="flex:1;word-break:break-word">' + escapeHtml(e.msg) + '</span></div>';
    }).join('');
    ep.scrollTop = ep.scrollHeight;
  }
  _updateEvFooter();
  if (window._evPopout && !window._evPopout.closed) _syncPopoutLog();
}

function toggleEvPanel() {
  const p = document.getElementById('ev-side');
  if (!p) return;
  const opening = !p.classList.contains('open');
  p.classList.toggle('open', opening);
  document.querySelectorAll('.main, .menubar, .toolbar, .sb-bar').forEach(el => {
    el.classList.toggle('ev-open', opening);
  });
  _updateEvFooter();
}

function _updateEvFooter() {
  const countEl = document.getElementById('ev-count');
  const statusEl = document.getElementById('ev-status');
  if (countEl) countEl.textContent = window.eventLog.length + ' events';
  if (statusEl) statusEl.textContent = window.wsConnection && window.wsConnection.readyState === 1 ? 'Live' : 'Offline';
}

function clearEvts() {
  window.eventLog = [];
  const ep = document.getElementById('evp');
  if (ep) ep.innerHTML = typeof t === 'function' ? t('events.waiting') : 'Waiting...';
  if (window._evPopout && !window._evPopout.closed) _syncPopoutLog();
}

function _syncPopoutLog() {
  const w = window._evPopout;
  if (!w || w.closed) return;
  const body = w.document.getElementById('ev-body');
  const count = w.document.getElementById('ev-count');
  if (!body) return;
  body.innerHTML = window.eventLog.map(e => {
    const icon = _evtIcon(e.msg);
    const cls = _evtClass(e.msg);
    return '<div class="ev-row">'
      + '<span class="color-muted" style="font-size:9px;white-space:nowrap;min-width:72px">' + escapeHtml(e.time) + '</span>'
      + '<span style="font-size:12px">' + icon + '</span>'
      + '<span class="' + cls + '" style="flex:1;word-break:break-word">' + escapeHtml(e.msg) + '</span></div>';
  }).join('');
  body.scrollTop = body.scrollHeight;
  if (count) count.textContent = window.eventLog.length + ' events';
}

/* ═══ MODAL (F3: Stack support) ═══
 *  모달 중첩이 필요한 이유: VM 설정 모달 안에서 ISO 브라우저 모달을 열 때,
 *  이전 모달 HTML을 보존해야 한다. _modalStack이 이를 관리한다.
 *  closeModal() 시 스택에 항목이 있으면 pop하여 복원, 없으면 #mbg를 숨긴다. */
var _modalStack = [];

function showModal(h, opts) {
  opts = opts || {};
  var mc = document.getElementById('mc');
  var bg = document.getElementById('mbg');
  if (!opts.replace && mc && mc.innerHTML && bg && !bg.classList.contains('hidden')) {
    _modalStack.push(mc.innerHTML);
  }
  if (mc) mc.innerHTML = h;
  bg?.classList.remove('hidden');
  mc?.focus();
}

function closeModal(force) {
  if (force) {
    _modalStack.length = 0;
    var forcedMc = document.getElementById('mc');
    if (forcedMc) forcedMc.innerHTML = '';
    document.getElementById('mbg')?.classList.add('hidden');
    return;
  }
  if (_modalStack.length > 0) {
    var prev = _modalStack.pop();
    var mc = document.getElementById('mc');
    if (mc) mc.innerHTML = prev;
  } else {
    document.getElementById('mbg')?.classList.add('hidden');
  }
}

/* ═══ CUSTOM INPUT MODAL (FE-4: prompt() 대체) ═══ */
function showInputModal(title, label, defaultVal, callback) {
  return new Promise(function(resolve) {
    var id = 'modal-input-' + Date.now();
    var html = '<h2>' + escapeHtml(title) + '</h2>'
      + '<div class="fr"><label>' + escapeHtml(label) + '</label>'
      + '<input id="' + id + '" class="input" value="' + escapeHtml(defaultVal || '') + '" autofocus '
      + 'style="flex:1;padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:6px"></div>'
      + '<div style="text-align:right;margin-top:12px">'
      + '<button class="btn btn-g" id="' + id + '-ok">' + (typeof t === 'function' ? t('btn.confirm') : 'OK') + '</button> '
      + '<button class="btn btn-r" id="' + id + '-cancel">' + (typeof t === 'function' ? t('btn.cancel') : 'Cancel') + '</button></div>';
    showModal(html);
    setTimeout(function() {
      var inp = document.getElementById(id);
      var okBtn = document.getElementById(id + '-ok');
      var cancelBtn = document.getElementById(id + '-cancel');
      if (inp) inp.focus();
      function doOk() {
        var val = inp ? inp.value.trim() : '';
        closeModal();
        if (callback) callback(val);
        resolve(val);
      }
      function doCancel() {
        closeModal();
        if (callback) callback(null);
        resolve(null);
      }
      if (okBtn) okBtn.addEventListener('click', doOk);
      if (cancelBtn) cancelBtn.addEventListener('click', doCancel);
      if (inp) inp.addEventListener('keydown', function(e) {
        if (e.key === 'Enter') { e.preventDefault(); doOk(); }
        if (e.key === 'Escape') { e.preventDefault(); doCancel(); }
      });
    }, 50);
  });
}

/* ═══ CUSTOM CONFIRM ═══ */
function customConfirm(title, message) {
  return new Promise(resolve => {
    const tFn = typeof t === 'function' ? t : k => k;
    const body = escapeHtml(message || '').replace(/\n/g, '<br>');
    const html = `<h2>${escapeHtml(title)}</h2><p class="mb-12">${body}</p><div style="text-align:right"><button class="btn btn-r" onclick="document.getElementById('mbg').classList.add('hidden');window._confirmResolve(true)">${tFn('btn.confirm')}</button> <button class="btn" onclick="document.getElementById('mbg').classList.add('hidden');window._confirmResolve(false)">${tFn('btn.cancel')}</button></div>`;
    window._confirmResolve = resolve;
    showModal(html);
  });
}

/* ═══ UTILITIES ═══ */
function renderProgressBar(p, c) {
  const cl = p > 85 ? 'var(--red)' : p > 60 ? 'var(--yellow)' : 'var(--green)';
  const anim = p > 85 ? ' pulse-anim' : '';
  return `<div class="pb${anim}"><div class="pb-f scan-anim" style="width:${p}%;background:${c || cl}"></div><div class="pb-t">${p.toFixed(1)}%</div></div>`;
}

function formatBytes(b) {
  if (!b || b < 1024) return (b || 0) + 'B';
  if (b < 1048576) return (b / 1024).toFixed(1) + 'K';
  if (b < 1073741824) return (b / 1048576).toFixed(1) + 'M';
  return (b / 1073741824).toFixed(2) + 'G';
}

function parseSize(s) {
  if (!s || typeof s === 'number') return s || 0;
  const m = String(s).trim().match(/^([\d.]+)\s*([TGMK]?)(?:I?B)?$/i);
  if (!m) return 0;
  const v = parseFloat(m[1]), u = (m[2] || '').toUpperCase();
  if (u === 'T') return v * 1099511627776;
  if (u === 'G') return v * 1073741824;
  if (u === 'M') return v * 1048576;
  if (u === 'K') return v * 1024;
  return v;
}

function showSkeleton(container, count) {
  let h = '';
  for (let i = 0; i < (count || 3); i++) {
    h += '<div class="hc skeleton" style="height:80px;margin-bottom:8px;background:var(--bg3);border-radius:var(--r);animation:skeleton-pulse 1.5s ease infinite"></div>';
  }
  if (typeof container === 'string') {
    const el = document.getElementById(container);
    if (el) el.innerHTML = h;
  } else if (container) {
    container.innerHTML = h;
  }
  return h;
}

function renderSortableTable(containerId, headers, rows, options) {
  const opts = options || {};
  const el = typeof containerId === 'string' ? document.getElementById(containerId) : containerId;
  if (!el) return;
  let h = '<table><thead><tr>';
  headers.forEach(hdr => { h += '<th>' + (typeof hdr === 'string' ? hdr : hdr.label) + '</th>'; });
  h += '</tr></thead><tbody>';
  if (rows.length === 0) {
    h += '<tr><td colspan="' + headers.length + '" class="text-center color-muted">' + (opts.emptyText || 'No data') + '</td></tr>';
  }
  rows.forEach(row => { h += '<tr>'; row.forEach(cell => { h += '<td>' + cell + '</td>'; }); h += '</tr>'; });
  h += '</tbody></table>';
  el.innerHTML = h;
}

/* ═══ DATA TABLE (B3: sortable, searchable, exportable) ═══
 *  createDataTable은 내부 상태(정렬/검색/페이지)를 window['_dt_'+tableId]에 저장한다.
 *  HTML onclick에서 window._dtSort(id, col)로 호출되므로 window 등록 필수.
 *  검색/정렬 시 HTML 태그를 strip(.replace(/<[^>]+>/g,''))하여 텍스트만 비교.
 *  pageSize를 config에 넘기면 페이지네이션이 자동 활성화된다 (pageSize=0이면 비활성). */
function createDataTable(containerId, config) {
  var el = typeof containerId === 'string' ? document.getElementById(containerId) : containerId;
  if (!el) return;
  var cfg = config || {};
  var headers = cfg.headers || [];
  var rows = cfg.rows || [];
  var tableId = 'dt-' + Math.random().toString(36).substr(2, 6);
  var sortCol = -1, sortDir = 1;
  var tFn = typeof t === 'function' ? t : function(k) { return k; };

  function renderTable(filteredRows) {
    var h = '';
    /* 페이지네이션 계산 */
    var pageSize = cfg.pageSize || 0;
    var dt = window['_dt_' + tableId];
    var currentPage = (dt && dt.currentPage) ? dt.currentPage : 1;
    var totalPages = pageSize > 0 ? Math.ceil(filteredRows.length / pageSize) : 1;
    if (currentPage > totalPages) currentPage = totalPages || 1;
    var displayRows = filteredRows;
    if (pageSize > 0) {
      var start = (currentPage - 1) * pageSize;
      displayRows = filteredRows.slice(start, start + pageSize);
    }
    if (cfg.searchable) {
      h += '<div class="flex gap-8 items-center mb-8"><input id="' + tableId + '-search" class="sb-search" placeholder="' + tFn('search') + '" oninput="window._dtFilter(\'' + tableId + '\')" style="max-width:300px;font-size:12px;padding:6px 10px;border-radius:4px">';
      if (cfg.exportable) h += '<button class="btn" style="font-size:10px;padding:3px 8px" onclick="window._dtExport(\'' + tableId + '\')">CSV</button>';
      h += '<span class="color-muted text-xs">' + filteredRows.length + ' rows</span></div>';
    }
    h += '<table id="' + tableId + '-table"><thead><tr>';
    headers.forEach(function(hdr, ci) {
      var dtx = window['_dt_' + tableId];
      var sc = dtx ? dtx.sortCol : sortCol;
      var sd = dtx ? dtx.sortDir : sortDir;
      var arrow = sc === ci ? (sd > 0 ? ' &#9650;' : ' &#9660;') : '';
      var sortAttr = hdr.sortable !== false ? ' style="cursor:pointer" onclick="window._dtSort(\'' + tableId + '\',' + ci + ')"' : '';
      h += '<th' + sortAttr + '>' + (hdr.label || hdr.key || '') + arrow + '</th>';
    });
    h += '</tr></thead><tbody>';
    if (displayRows.length === 0) {
      h += '<tr><td colspan="' + headers.length + '" class="text-center color-muted">' + (cfg.emptyText || 'No data') + '</td></tr>';
    }
    displayRows.forEach(function(row) {
      h += '<tr>'; row.forEach(function(cell) { h += '<td>' + cell + '</td>'; }); h += '</tr>';
    });
    h += '</tbody></table>';
    /* 페이지네이션 UI */
    if (pageSize > 0 && totalPages > 1) {
      h += '<div class="flex items-center gap-8 mt-8">';
      h += '<button class="btn btn-sm" ' + (currentPage <= 1 ? 'disabled' : '') + ' onclick="window._dtPage(\'' + tableId + '\',' + (currentPage - 1) + ')">Prev</button>';
      h += '<span class="stat-label">Page ' + currentPage + '/' + totalPages + '</span>';
      h += '<button class="btn btn-sm" ' + (currentPage >= totalPages ? 'disabled' : '') + ' onclick="window._dtPage(\'' + tableId + '\',' + (currentPage + 1) + ')">Next</button>';
      h += '</div>';
    }
    el.innerHTML = h;
    /* 현재 페이지 저장 */
    if (dt) dt.currentPage = currentPage;
  }

  window['_dt_' + tableId] = { headers: headers, rows: rows, sortCol: sortCol, sortDir: sortDir, currentPage: 1, el: el, cfg: cfg, renderTable: renderTable };
  renderTable(rows);
}

function _dtSort(id, col) {
  var dt = window['_dt_' + id]; if (!dt) return;
  if (dt.sortCol === col) dt.sortDir *= -1; else { dt.sortCol = col; dt.sortDir = 1; }
  var sorted = dt.rows.slice().sort(function(a, b) {
    var va = (a[col] || '').toString().replace(/<[^>]+>/g, '').toLowerCase();
    var vb = (b[col] || '').toString().replace(/<[^>]+>/g, '').toLowerCase();
    var na = parseFloat(va), nb = parseFloat(vb);
    if (!isNaN(na) && !isNaN(nb)) return (na - nb) * dt.sortDir;
    return (va < vb ? -1 : va > vb ? 1 : 0) * dt.sortDir;
  });
  dt.renderTable(sorted);
}

function _dtFilter(id) {
  var dt = window['_dt_' + id]; if (!dt) return;
  var searchEl = document.getElementById(id + '-search');
  var q = (searchEl ? searchEl.value : '').toLowerCase();
  var filtered = q ? dt.rows.filter(function(row) { return row.some(function(cell) { return (cell || '').toString().replace(/<[^>]+>/g, '').toLowerCase().indexOf(q) !== -1; }); }) : dt.rows;
  dt.renderTable(filtered);
}

function _dtExport(id) {
  var dt = window['_dt_' + id]; if (!dt) return;
  var csv = dt.headers.map(function(h) { return h.label || h.key || ''; }).join(',') + '\n';
  dt.rows.forEach(function(row) { csv += row.map(function(cell) { return '"' + (cell || '').toString().replace(/<[^>]+>/g, '').replace(/"/g, '""') + '"'; }).join(',') + '\n'; });
  var blob = new Blob([csv], { type: 'text/csv' });
  var a = document.createElement('a'); a.href = URL.createObjectURL(blob); a.download = 'purecvisor-export.csv'; a.click();
}

function _dtPage(id, page) {
  var dt = window['_dt_' + id]; if (!dt) return;
  dt.currentPage = page;
  /* 재검색 적용 */
  var searchEl = document.getElementById(id + '-search');
  var q = (searchEl ? searchEl.value : '').toLowerCase();
  var filtered = q ? dt.rows.filter(function(row) { return row.some(function(cell) { return (cell || '').toString().replace(/<[^>]+>/g, '').toLowerCase().indexOf(q) !== -1; }); }) : dt.rows;
  /* 정렬 적용 */
  if (dt.sortCol >= 0) {
    filtered = filtered.slice().sort(function(a, b) {
      var va = (a[dt.sortCol] || '').toString().replace(/<[^>]+>/g, '').toLowerCase();
      var vb = (b[dt.sortCol] || '').toString().replace(/<[^>]+>/g, '').toLowerCase();
      var na = parseFloat(va), nb = parseFloat(vb);
      if (!isNaN(na) && !isNaN(nb)) return (na - nb) * dt.sortDir;
      return (va < vb ? -1 : va > vb ? 1 : 0) * dt.sortDir;
    });
  }
  dt.renderTable(filtered);
}

async function fetchWithRetry(fn, retries) {
  const maxRetries = retries || 2;
  for (let attempt = 0; attempt <= maxRetries; attempt++) {
    try { return await fn(); }
    catch (e) { if (attempt === maxRetries) throw e; await new Promise(r => setTimeout(r, 1000 * (attempt + 1))); }
  }
}

/* ═══ DEBUG FLAG (I2) ═══ */
var _DEBUG = localStorage.getItem('pcv-debug') === 'true';

/* ═══ SAFE ASYNC WRAPPER (I1) ═══ */
function safeAsync(fn, fallbackMsg) {
  return async function() {
    try { return await fn.apply(this, arguments); }
    catch (e) {
      if(_DEBUG) console.warn('safeAsync error:', e.message);
      toast((fallbackMsg || _L('작업 오류', 'Operation error')) + ': ' + (e.message || ''), false);
    }
  };
}

/* ═══ FAVORITES ═══ */
function getFavorites() {
  try { return JSON.parse(localStorage.getItem('pcv-favorites') || '[]'); } catch(e) { return []; }
}
function toggleFavorite(name) {
  let favs = getFavorites();
  if (favs.includes(name)) favs = favs.filter(f => f !== name);
  else favs.push(name);
  localStorage.setItem('pcv-favorites', JSON.stringify(favs));
  window.render();
}
function isFavorite(name) { return getFavorites().includes(name); }

/* ═══ POPOUT EVENT LOG ═══ */
function popoutEventLog() {
  const w = window.open('', 'pcv-event-log', 'width=700,height=500,menubar=no,toolbar=no,location=no,status=no');
  if (!w) { toast(t('msg.popup_blocked'), false); return; }
  window._evPopout = w;
  const theme = document.documentElement.getAttribute('data-theme') || '';
  w.document.write('<!DOCTYPE html><html lang="ko"><head><meta charset="UTF-8"><title>PureCVisor — Event Log</title>'
    + '<link rel="stylesheet" href="/ui/style.css">'
    + '<style>body{margin:0;padding:0;background:var(--bg);color:var(--fg);font-family:var(--font-mono);font-size:12px;overflow:hidden;display:flex;flex-direction:column;height:100vh}'
    + '.ev-toolbar{padding:8px 12px;border-bottom:1px solid var(--border);display:flex;justify-content:space-between;align-items:center;background:var(--bg2)}'
    + '.ev-body{flex:1;overflow-y:auto;padding:6px 10px}'
    + '.ev-row{padding:2px 0;border-bottom:1px solid var(--border);display:flex;gap:6px;align-items:baseline;font-size:11px}'
    + '</style></head><body' + (theme ? ' data-theme="' + theme + '"' : '') + '>'
    + '<div class="ev-toolbar"><span style="font-weight:700">&#128220; PureCVisor Event Log</span>'
    + '<div style="display:flex;gap:6px"><button class="btn" style="font-size:10px;padding:3px 8px" onclick="parent.clearEvts()">Clear</button>'
    + '<span id="ev-count" class="color-muted" style="font-size:10px"></span></div></div>'
    + '<div class="ev-body" id="ev-body"></div>'
    + '</body></html>');
  w.document.close();
  _syncPopoutLog();
}

/* ═══ 모달 포커스 트랩 (접근성, FE-5: 셀렉터 수정) ═══ */
document.addEventListener('keydown', function(e) {
  var modal = document.querySelector('#mbg:not(.hidden)');
  if (!modal || e.key !== 'Tab') return;
  var focusable = modal.querySelectorAll('button, [href], input, select, textarea, [tabindex]:not([tabindex="-1"])');
  if (focusable.length === 0) return;
  var first = focusable[0], last = focusable[focusable.length - 1];
  if (e.shiftKey) {
    if (document.activeElement === first) { e.preventDefault(); last.focus(); }
  } else {
    if (document.activeElement === last) { e.preventDefault(); first.focus(); }
  }
});

/* ═══ 빈 상태 헬퍼 ═══ */
function emptyState(icon, msg) {
  return '<div class="empty-state" style="padding:40px;text-align:center" role="status">'
    + '<div style="font-size:42px;opacity:.4">' + icon + '</div>'
    + '<div class="color-muted mt-8">' + msg + '</div></div>';
}

/* ═══ 비동기 작업 로딩 래퍼 (#1/#3 더블클릭 dedup) ═══ */
async function withSpinner(btn, asyncFn) {
  if (!btn) { await asyncFn(); return; }
  /* #3 진행 중 재진입 차단 */
  if (btn.dataset.pcvBusy === '1') return;
  btn.dataset.pcvBusy = '1';
  btn.classList.add('is-loading');
  btn.disabled = true;
  btn.setAttribute('aria-busy', 'true');
  try { await asyncFn(); }
  catch (e) {
    if (typeof reportError === 'function') reportError('action', e);
    throw e;
  }
  finally {
    btn.disabled = false;
    btn.classList.remove('is-loading');
    btn.removeAttribute('aria-busy');
    delete btn.dataset.pcvBusy;
  }
}

/* ═══ 빈 상태 컴포넌트 강화 (#16) ═══ */
function emptyStatePro(opts) {
  /* opts: { icon, title, desc, ctaLabel, ctaAction } */
  var i = opts.icon || '&#128230;';
  var t = escapeHtml(opts.title || 'No items');
  var d = escapeHtml(opts.desc || '');
  var btn = '';
  if (opts.ctaLabel && opts.ctaAction) {
    btn = '<button class="btn" onclick="' + escapeHtml(opts.ctaAction) + '">' + escapeHtml(opts.ctaLabel) + '</button>';
  }
  return '<div class="empty-state"><div class="empty-icon">' + i + '</div>' +
    '<div class="empty-title">' + t + '</div>' +
    '<div class="empty-desc">' + d + '</div>' + btn + '</div>';
}

/* ═══ 색맹 보조 상태 배지 (#18) ═══ */
function statusBadge(text, kind) {
  /* kind: 'ok' | 'warn' | 'err' | 'off' */
  return '<span class="status-badge s-' + (kind || 'off') + '">' + escapeHtml(text) + '</span>';
}

/* ═══ 사이드바 키보드 활성화 (FE-5: Enter/Space로 클릭) ═══ */
document.addEventListener('keydown', function(e) {
  if (e.key === 'Enter' || e.key === ' ') {
    var tgt = e.target;
    if (tgt && (tgt.getAttribute('role') === 'link' || tgt.getAttribute('role') === 'button') && tgt.tabIndex >= 0) {
      e.preventDefault();
      tgt.click();
    }
  }
});

/* ═══ EXPORT TO PCV NAMESPACE (ADR-0013) ═══ */
PCV.ui = {
  escapeHtml: escapeHtml,
  escapeAttr: escapeAttr,
  esc: escapeHtml,
  H: H,
  toast: toast,
  addEvt: addEvt,
  toggleEvPanel: toggleEvPanel,
  clearEvts: clearEvts,
  showModal: showModal,
  closeModal: closeModal,
  customConfirm: customConfirm,
  showInputModal: showInputModal,
  renderProgressBar: renderProgressBar,
  formatBytes: formatBytes,
  parseSize: parseSize,
  showSkeleton: showSkeleton,
  renderSortableTable: renderSortableTable,
  createDataTable: createDataTable,
  fetchWithRetry: fetchWithRetry,
  getFavorites: getFavorites,
  toggleFavorite: toggleFavorite,
  isFavorite: isFavorite,
  popoutEventLog: popoutEventLog,
  safeAsync: safeAsync,
  emptyState: emptyState,
  emptyStatePro: emptyStatePro,
  statusBadge: statusBadge,
  withSpinner: withSpinner,
  _modalStack: _modalStack
};

/* ═══ BACKWARD COMPAT SHIMS (ADR-0013: remove after full transition) ═══ */
window.H = H;
window.escapeHtml = escapeHtml;
window.esc = esc;
window.escapeAttr = escapeAttr;
window.toast = toast;
window.addEvt = addEvt;
window.toggleEvPanel = toggleEvPanel;
window.clearEvts = clearEvts;
window.showModal = showModal;
window.closeModal = closeModal;
window.showM = showModal;
window.closeM = closeModal;
window.customConfirm = customConfirm;
window.showInputModal = showInputModal;
window.renderProgressBar = renderProgressBar;
window.formatBytes = formatBytes;
window.parseSize = parseSize;
window.showSkeleton = showSkeleton;
window.renderSortableTable = renderSortableTable;
window.createDataTable = createDataTable;
window._dtSort = _dtSort;
window._dtFilter = _dtFilter;
window._dtExport = _dtExport;
window._dtPage = _dtPage;
window.fetchWithRetry = fetchWithRetry;
window.getFavorites = getFavorites;
window.toggleFavorite = toggleFavorite;
window.isFavorite = isFavorite;
window.popoutEventLog = popoutEventLog;
window.safeAsync = safeAsync;
window._DEBUG = _DEBUG;
window.emptyState = emptyState;
window.emptyStatePro = emptyStatePro;
window.statusBadge = statusBadge;
window.withSpinner = withSpinner;

})(window.PCV);
