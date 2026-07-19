
if (!window.eventLog) window.eventLog = [];

window.PCV = window.PCV || {};
(function(PCV) {

function escapeHtml(s) {
  if (!s) return '';
  return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;').replace(/'/g, '&#39;');
}
var esc = escapeHtml;

function escapeAttr(s) {
  if (!s) return '';
  return String(s).replace(/[^a-zA-Z0-9_.-]/g, function(c) {
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

  statCard: (label, value, icon) => `<div class="hc" style="text-align:center;padding:12px">${icon ? '<div style="font-size:20px">' + icon + '</div>' : ''}<div style="font-size:18px;font-weight:600">${escapeHtml(String(value))}</div><div class="stat-label">${escapeHtml(String(label))}</div></div>`,
};

var HN = {
  card: function (title, body, cls) {
    var node = PCV.uxlib.el('div', { class: 'hc ' + (cls || '') });
    if (title !== undefined && title !== null && title !== '') node.appendChild(PCV.uxlib.el('h4', null, title));
    if (body !== undefined && body !== null) node.appendChild(PCV.uxlib.frag(body));
    return node;
  },
  row: function (key, val, cls) {
    return PCV.uxlib.el('div', { class: 'hr' },
      PCV.uxlib.el('span', { class: 'k' }, key),
      PCV.uxlib.el('span', { class: 'v ' + (cls || '') }, val));
  },
  badge: function (text, type) { return PCV.uxlib.el('span', { class: 'badge b-' + type }, text); },
  grid: function (cols) {
    var kids = Array.prototype.slice.call(arguments, 1);
    return PCV.uxlib.el.apply(null, ['div', { class: 'sg grid-' + cols }].concat(kids));
  },
  section: function (title) { return PCV.uxlib.el('h3', { class: 'section-title' }, title); },
  sectionLg: function (title) { return PCV.uxlib.el('h3', { class: 'section-title-lg' }, title); },
  statCard: function (label, value, icon) {
    return PCV.uxlib.el('div', { class: 'hc', style: 'text-align:center;padding:12px' },
      icon ? PCV.uxlib.el('div', { style: 'font-size:20px' }, icon) : null,
      PCV.uxlib.el('div', { style: 'font-size:18px;font-weight:600' }, String(value)),
      PCV.uxlib.el('div', { class: 'stat-label' }, label));
  },
};

function toast(m, lvl = true) {

  var isErr = lvl === false || lvl === 'e' || lvl === 'error';
  var isWarn = !isErr && (lvl === 'w' || lvl === 'warn' || lvl === 'warning');
  var cls = isErr ? 't-er' : (isWarn ? 't-warn' : 't-ok');
  var icon = isErr ? '&#10060; ' : (isWarn ? '&#9888; ' : '&#9989; ');
  var barColor = isErr ? 'var(--red)' : (isWarn ? 'var(--yellow)' : 'var(--green)');
  const d = document.createElement('div');
  d.className = 'toast ' + cls;

  d.textContent = String(icon + m).replace(/&#(\d+);/g, function (_, n) { return String.fromCodePoint(+n); });
  d.style.cssText = 'transform:translateX(100%);transition:transform 0.3s ease-out';
  requestAnimationFrame(function() { d.style.transform = 'translateX(0)'; });
  d.onclick = function() { d.style.transform = 'translateX(100%)'; setTimeout(function() { d.remove(); }, 300); };
  const prog = document.createElement('div');
  prog.className = 'toast-progress';
  prog.style.cssText = 'height:3px;background:' + barColor + ';border-radius:0 0 6px 6px;width:100%;transition:width 2.8s linear';
  d.appendChild(prog);
  const container = document.getElementById('toasts');
  container.appendChild(d);
  while (container.children.length > 3) container.removeChild(container.firstChild);
  requestAnimationFrame(() => { prog.style.width = '0%'; });
  setTimeout(() => d.remove(), 3e3);

  if (typeof addNotification === 'function') {
    addNotification(isErr ? 'error' : (isWarn ? 'warning' : 'info'), m, '');
  }
}

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

var _EVT_SVGNS = 'http://www.w3.org/2000/svg';
function _evtIconNode(iconHtml) {
  var m = /#ci-([\w-]+)/.exec(iconHtml || '');
  var svg = document.createElementNS(_EVT_SVGNS, 'svg');
  svg.setAttribute('class', 'ci-icon');
  svg.setAttribute('aria-hidden', 'true');
  var use = document.createElementNS(_EVT_SVGNS, 'use');
  use.setAttribute('href', '/ui/vendor/coolicons/coolicons.svg#ci-' + (m ? m[1] : 'info'));
  svg.appendChild(use);
  return svg;
}

function addEvt(m) {
  const ts = new Date().toLocaleTimeString('ko-KR', { hour12: false, hour: '2-digit', minute: '2-digit', second: '2-digit' });
  const ms = String(Date.now() % 1000).padStart(3, '0');
  const entry = { time: ts + '.' + ms, msg: m, raw: ts + '.' + ms + ' ' + m };
  window.eventLog.push(entry);
  if (window.eventLog.length > 200) window.eventLog.shift();
  const ep = document.getElementById('evp');
  if (ep) {
    var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
    clearEl(ep);
    ep.appendChild(frag(window.eventLog.map(e => {
      const icon = _evtIcon(e.msg);
      const cls = _evtClass(e.msg);
      return el('div', { style: 'padding:2px 0;border-bottom:1px solid var(--border);display:flex;gap:6px;align-items:baseline;font-size:11px' },
        el('span', { class: 'color-muted', style: 'font-size:9px;white-space:nowrap;min-width:72px' }, e.time),
        el('span', { style: 'font-size:12px' }, _evtIconNode(icon)),
        el('span', { class: cls, style: 'flex:1;word-break:break-word' }, e.msg));
    })));
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
  if (ep) { PCV.uxlib.clearEl(ep); ep.appendChild(PCV.uxlib.frag(typeof t === 'function' ? t('events.waiting') : 'Waiting...')); }
  if (window._evPopout && !window._evPopout.closed) _syncPopoutLog();
}

function _syncPopoutLog() {
  const w = window._evPopout;
  if (!w || w.closed) return;
  const body = w.document.getElementById('ev-body');
  const count = w.document.getElementById('ev-count');
  if (!body) return;
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  clearEl(body);
  body.appendChild(frag(window.eventLog.map(e => {
    const icon = _evtIcon(e.msg);
    const cls = _evtClass(e.msg);
    return el('div', { class: 'ev-row' },
      el('span', { class: 'color-muted', style: 'font-size:9px;white-space:nowrap;min-width:72px' }, e.time),
      el('span', { style: 'font-size:12px' }, _evtIconNode(icon)),
      el('span', { class: cls, style: 'flex:1;word-break:break-word' }, e.msg));
  })));
  body.scrollTop = body.scrollHeight;
  if (count) count.textContent = window.eventLog.length + ' events';
}

var _modalStack = [];

function _setModalBody(mc, body) {
  if (typeof body === 'string') { mc.innerHTML = body; return; }
  PCV.uxlib.clearEl(mc);
  mc.appendChild(PCV.uxlib.frag(body));
}

function showModal(h, opts) {
  opts = opts || {};
  var mc = document.getElementById('mc');
  var bg = document.getElementById('mbg');
  if (!opts.replace && mc && mc.childNodes.length && bg && !bg.classList.contains('hidden')) {
    _modalStack.push(Array.prototype.slice.call(mc.childNodes));
  }
  if (mc) _setModalBody(mc, h);
  bg?.classList.remove('hidden');
  mc?.focus();
}

function closeModal(force) {
  if (force) {
    _modalStack.length = 0;
    var forcedMc = document.getElementById('mc');
    if (forcedMc) PCV.uxlib.clearEl(forcedMc);
    document.getElementById('mbg')?.classList.add('hidden');
    return;
  }
  if (_modalStack.length > 0) {
    var prev = _modalStack.pop();
    var mc = document.getElementById('mc');
    if (mc) { PCV.uxlib.clearEl(mc); mc.appendChild(PCV.uxlib.frag(prev)); }
  } else {
    document.getElementById('mbg')?.classList.add('hidden');
  }
}

function showInputModal(title, label, defaultVal, callback) {
  return new Promise(function(resolve) {
    var id = 'modal-input-' + Date.now();
    var mkEl = PCV.uxlib.el;
    showModal([
      mkEl('h2', null, title),
      mkEl('div', { class: 'fr' },
        mkEl('label', { for: id }, label),
        mkEl('input', { id: id, class: 'input', value: defaultVal || '', autofocus: '', style: 'flex:1;padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:6px' })),
      mkEl('div', { style: 'text-align:right;margin-top:12px' },
        mkEl('button', { class: 'btn btn-g', id: id + '-ok' }, typeof t === 'function' ? t('btn.confirm') : 'OK'),
        ' ',
        mkEl('button', { class: 'btn btn-r', id: id + '-cancel' }, typeof t === 'function' ? t('btn.cancel') : 'Cancel'))
    ]);
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

function customConfirm(title, message) {
  return new Promise(resolve => {
    const tFn = typeof t === 'function' ? t : k => k;
    const mkEl = PCV.uxlib.el;
    const msgParts = String(message || '').split('\n').flatMap((line, i) => i === 0 ? [line] : [mkEl('br'), line]);
    window._confirmResolve = resolve;
    showModal([
      mkEl('h2', null, title),
      mkEl('p', { class: 'mb-12' }, msgParts),
      mkEl('div', { style: 'text-align:right' },
        mkEl('button', { class: 'btn btn-r', onclick: "document.getElementById('mbg').classList.add('hidden');window._confirmResolve(true)" }, tFn('btn.confirm')),
        ' ',
        mkEl('button', { class: 'btn', onclick: "document.getElementById('mbg').classList.add('hidden');window._confirmResolve(false)" }, tFn('btn.cancel')))
    ]);
  });
}

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
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  var fragment = frag();
  for (var i = 0; i < (count || 3); i++) {
    fragment.appendChild(el('div', {
      class: 'hc skeleton',
      style: 'height:80px;margin-bottom:8px;background:var(--bg3);border-radius:var(--r);animation:skeleton-pulse 1.5s ease infinite'
    }));
  }
  var target = typeof container === 'string' ? document.getElementById(container) : container;
  if (target) { clearEl(target); target.appendChild(fragment); return; }
  return fragment;
}

function renderSortableTable(containerId, headers, rows, options) {
  const opts = options || {};
  const el = typeof containerId === 'string' ? document.getElementById(containerId) : containerId;
  if (!el) return;

  var mk = PCV.uxlib.el, clearEl = PCV.uxlib.clearEl;
  var headRow = mk('tr', null, headers.map(function(hdr) {
    return mk('th', null, typeof hdr === 'string' ? hdr : hdr.label);
  }));
  var bodyRows;
  if (rows.length === 0) {
    bodyRows = [mk('tr', null, mk('td', { colspan: headers.length, class: 'text-center color-muted' }, opts.emptyText || 'No data'))];
  } else {
    bodyRows = rows.map(function(row) {
      return mk('tr', null, row.map(function(cell) { return mk('td', null, cell); }));
    });
  }
  clearEl(el);
  el.appendChild(mk('table', null, mk('thead', null, headRow), mk('tbody', null, bodyRows)));
}

function createDataTable(containerId, config) {
  var mk = PCV.uxlib.el, mkFrag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  var el = typeof containerId === 'string' ? document.getElementById(containerId) : containerId;

  if (!el) {
    el = document.createElement('div');
    if (typeof containerId === 'string') el.id = containerId;
  }
  var cfg = config || {};
  var headers = cfg.headers || [];
  var rows = cfg.rows || [];
  var tableId = 'dt-' + Math.random().toString(36).substr(2, 6);
  var sortCol = -1, sortDir = 1;
  var tFn = typeof t === 'function' ? t : function(k) { return k; };

  function renderTable(filteredRows) {

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
    var children = [];

    if (cfg.searchable) {
      var bar = [mk('input', {
        'aria-label': tFn('search'),
        id: tableId + '-search',
        class: 'sb-search',
        placeholder: tFn('search'),
        oninput: 'window._dtFilter(\'' + tableId + '\')',
        style: 'max-width:300px;font-size:12px;padding:6px 10px;border-radius:4px'
      })];
      if (cfg.exportable) {
        bar.push(mk('button', {
          class: 'btn',
          style: 'font-size:10px;padding:3px 8px',
          onclick: 'window._dtExport(\'' + tableId + '\')'
        }, 'CSV'));
      }
      bar.push(mk('span', { class: 'color-muted text-xs' }, filteredRows.length + ' rows'));
      children.push(mk('div', { class: 'flex gap-8 items-center mb-8' }, bar));
    }

    var ths = headers.map(function(hdr, ci) {
      var dtx = window['_dt_' + tableId];
      var sc = dtx ? dtx.sortCol : sortCol;
      var sd = dtx ? dtx.sortDir : sortDir;

      var arrow = sc === ci ? (sd > 0 ? ' ▲' : ' ▼') : null;
      var attrs = hdr.sortable !== false
        ? { style: 'cursor:pointer', onclick: 'window._dtSort(\'' + tableId + '\',' + ci + ')' }
        : null;
      return mk('th', attrs, hdr.label || hdr.key || '', arrow);
    });

    var bodyRows;
    if (displayRows.length === 0) {
      bodyRows = [mk('tr', null, mk('td', { colspan: headers.length, class: 'text-center color-muted' }, cfg.emptyText || 'No data'))];
    } else {
      bodyRows = displayRows.map(function(row) {
        return mk('tr', null, row.map(function(cell) { return mk('td', null, cell); }));
      });
    }
    children.push(mk('table', { id: tableId + '-table' },
      mk('thead', null, mk('tr', null, ths)),
      mk('tbody', null, bodyRows)));

    if (pageSize > 0 && totalPages > 1) {
      children.push(mk('div', { class: 'flex items-center gap-8 mt-8' },
        mk('button', {
          class: 'btn btn-sm',
          disabled: currentPage <= 1 ? '' : null,
          onclick: 'window._dtPage(\'' + tableId + '\',' + (currentPage - 1) + ')'
        }, 'Prev'),
        mk('span', { class: 'stat-label' }, 'Page ' + currentPage + '/' + totalPages),
        mk('button', {
          class: 'btn btn-sm',
          disabled: currentPage >= totalPages ? '' : null,
          onclick: 'window._dtPage(\'' + tableId + '\',' + (currentPage + 1) + ')'
        }, 'Next')));
    }
    clearEl(el);
    el.appendChild(mkFrag(children));

    if (dt) dt.currentPage = currentPage;
  }

  window['_dt_' + tableId] = { headers: headers, rows: rows, sortCol: sortCol, sortDir: sortDir, currentPage: 1, el: el, cfg: cfg, renderTable: renderTable };
  renderTable(rows);
  return el;
}

function _dtCellText(cell) {
  if (cell === null || cell === undefined) return '';
  if (cell instanceof Node) return cell.textContent || '';
  if (Array.isArray(cell)) return cell.map(_dtCellText).join('');
  return String(cell).replace(/<[^>]+>/g, '');
}

function _dtSort(id, col) {
  var dt = window['_dt_' + id]; if (!dt) return;
  if (dt.sortCol === col) dt.sortDir *= -1; else { dt.sortCol = col; dt.sortDir = 1; }
  var sorted = dt.rows.slice().sort(function(a, b) {
    var va = _dtCellText(a[col]).toLowerCase();
    var vb = _dtCellText(b[col]).toLowerCase();
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
  var filtered = q ? dt.rows.filter(function(row) { return row.some(function(cell) { return _dtCellText(cell).toLowerCase().indexOf(q) !== -1; }); }) : dt.rows;
  dt.renderTable(filtered);
}

function _dtExport(id) {
  var dt = window['_dt_' + id]; if (!dt) return;
  var csv = dt.headers.map(function(h) { return h.label || h.key || ''; }).join(',') + '\n';
  dt.rows.forEach(function(row) { csv += row.map(function(cell) { return '"' + _dtCellText(cell).replace(/"/g, '""') + '"'; }).join(',') + '\n'; });
  var blob = new Blob([csv], { type: 'text/csv' });
  var a = document.createElement('a'); a.href = URL.createObjectURL(blob); a.download = 'purecvisor-export.csv'; a.click();
}

function _dtPage(id, page) {
  var dt = window['_dt_' + id]; if (!dt) return;
  dt.currentPage = page;

  var searchEl = document.getElementById(id + '-search');
  var q = (searchEl ? searchEl.value : '').toLowerCase();
  var filtered = q ? dt.rows.filter(function(row) { return row.some(function(cell) { return _dtCellText(cell).toLowerCase().indexOf(q) !== -1; }); }) : dt.rows;

  if (dt.sortCol >= 0) {
    filtered = filtered.slice().sort(function(a, b) {
      var va = _dtCellText(a[dt.sortCol]).toLowerCase();
      var vb = _dtCellText(b[dt.sortCol]).toLowerCase();
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

var _DEBUG = localStorage.getItem('pcv-debug') === 'true';

function safeAsync(fn, fallbackMsg) {
  return async function() {
    try { return await fn.apply(this, arguments); }
    catch (e) {
      if(_DEBUG) console.warn('safeAsync error:', e.message);
      toast((fallbackMsg || _L('작업 오류', 'Operation error')) + ': ' + (e.message || ''), false);
    }
  };
}

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

function emptyState(icon, msg) {
  return '<div class="empty-state" style="padding:40px;text-align:center" role="status">'
    + '<div style="font-size:42px;opacity:.4">' + icon + '</div>'
    + '<div class="color-muted mt-8">' + msg + '</div></div>';
}

async function withSpinner(btn, asyncFn) {
  if (!btn) { await asyncFn(); return; }

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

function decodeNumericEntities(s) {
  return String(s)
    .replace(/&#x([0-9a-fA-F]+);/g, function (_, hex) { return String.fromCodePoint(parseInt(hex, 16)); })
    .replace(/&#(\d+);/g, function (_, dec) { return String.fromCodePoint(parseInt(dec, 10)); });
}

function emptyStatePro(opts) {

  var el = PCV.uxlib.el;
  var children = [
    el('div', { class: 'empty-icon' }, decodeNumericEntities(opts.icon || '&#128230;')),
    el('div', { class: 'empty-title' }, opts.title || 'No items'),
    el('div', { class: 'empty-desc' }, opts.desc || '')
  ];
  if (opts.ctaLabel && opts.ctaAction) {

    children.push(el('button', { class: 'btn', onclick: opts.ctaAction }, opts.ctaLabel));
  }
  return el('div', { class: 'empty-state' }, children);
}

function statusBadge(text, kind) {

  return '<span class="status-badge s-' + (kind || 'off') + '">' + escapeHtml(text) + '</span>';
}

document.addEventListener('keydown', function(e) {
  if (e.key === 'Enter' || e.key === ' ') {
    var tgt = e.target;
    if (tgt && (tgt.getAttribute('role') === 'link' || tgt.getAttribute('role') === 'button') && tgt.tabIndex >= 0) {
      e.preventDefault();
      tgt.click();
    }
  }
});

PCV.ui = {
  escapeHtml: escapeHtml,
  escapeAttr: escapeAttr,
  esc: escapeHtml,
  H: H,
  HN: HN,
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

window.H = H;
window.HN = HN;
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
