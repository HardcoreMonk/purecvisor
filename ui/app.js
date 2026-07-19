
var API_BASE = '/api/v1';
var authToken = sessionStorage.getItem('pcv_token') || '';
var wsConnection = null;
var vmList = [];
var selectedVmIndex = 0;
var currentTab = 'dashboard';
var sortField = 'name';
var sortDirection = 1;
var cpuHistory = Array(60).fill(0);
var memHistory = Array(60).fill(0);
var hostCpuHistory = Array(60).fill(0);
var hostMemHistory = Array(60).fill(0);
var checkedVms = new Set();
var eventLog = [];
var lastLoadTime = Date.now();
var _dashWidgets = JSON.parse(localStorage.getItem('pcv-dash-widgets') || '{"stats":true,"actions":true,"charts":true,"alerts":true,"vms":true}');

window.PCV = window.PCV || {};
Object.defineProperty(window.PCV, 'state', {
  get: function() {
    return {
      vmList: vmList,
      selectedVmIndex: selectedVmIndex,
      currentTab: currentTab,
      eventLog: eventLog,
      lastLoadTime: lastLoadTime
    };
  },
  configurable: true
});
window.PCV.config = {
  API_BASE: API_BASE,
  VERSION: '1.0',
  RPC_COUNT: 195,
  REST_COUNT: 130,
  METRICS_COUNT: 155
};
Object.defineProperty(window.PCV, 'auth', {
  get: function() {
    return {
      token: authToken,
      user: sessionStorage.getItem('pcv_user') || ''
    };
  },
  configurable: true
});

window.addEventListener('beforeunload', (e) => {

  if (authToken) {
    e.preventDefault();
    e.returnValue = '';
  }
  authToken = '';
  if (wsConnection) wsConnection.close();
});

var PCV_CLUSTER_ONLY_NAV = ['cluster', 'mon-cluster', 'federation'];

function applyEditionCapabilities() {
  fetch(API_BASE + '/health').then(function(r) { return r.json(); }).then(function(h) {
    var hasCluster = h.capabilities && h.capabilities.cluster;
    var edition = hasCluster ? 'multi' : 'single';
    window.pcvClusterEnabled = hasCluster;
    window.PCV_UI_EDITION = edition;
    if (window.PCV && typeof PCV.applyEditionEndpointSurface === 'function') {
      PCV.applyEditionEndpointSurface(edition);
    }
    if (!hasCluster) {
      document.querySelectorAll('.cluster-only').forEach(function(el) {
        el.style.display = 'none';
      });

      PCV_CLUSTER_ONLY_NAV.forEach(function(nav) {
        var sel = '[data-nav="' + nav + '"],[onclick*="navigateTo(\'' + nav + '\')"]';
        document.querySelectorAll(sel).forEach(function(el) {
          el.style.display = 'none';
        });
      });

      var clusterTab = document.querySelector('[data-sb="cluster"]');
      if (clusterTab && clusterTab.classList.contains('active')) {
        switchSbTab('vms');
      }
    }
  }).catch(function() {});
}

applyEditionCapabilities();

var esc = escapeHtml;

function ciIcon(name) {
  return '<svg class="ci-icon" aria-hidden="true"><use href="/ui/vendor/coolicons/coolicons.svg#ci-' + name + '"></use></svg>';
}

function _svgEl(tag, attrs, children) {
  var node = document.createElementNS('http://www.w3.org/2000/svg', tag);
  if (attrs) Object.keys(attrs).forEach(function (k) {
    var v = attrs[k];
    if (v === null || v === undefined || v === false) return;
    node.setAttribute(k, v);
  });
  (children || []).forEach(function (c) {
    if (c === null || c === undefined || c === false) return;
    node.appendChild(c instanceof Node ? c : document.createTextNode(String(c)));
  });
  return node;
}
function ciIconNode(name) {
  return _svgEl('svg', { class: 'ci-icon', 'aria-hidden': 'true' }, [
    _svgEl('use', { href: '/ui/vendor/coolicons/coolicons.svg#ci-' + name })
  ]);
}

function _progressBar(p, c) {
  var el = PCV.uxlib.el;
  var cl = p > 85 ? 'var(--red)' : p > 60 ? 'var(--yellow)' : 'var(--green)';
  var anim = p > 85 ? ' pulse-anim' : '';
  return el('div', { class: 'pb' + anim },
    el('div', { class: 'pb-f scan-anim', style: 'width:' + p + '%;background:' + (c || cl) }),
    el('div', { class: 'pb-t' }, p.toFixed(1) + '%'));
}

var EVT_ICONS = {
  auth: ciIcon('lock'), ws: ciIcon('globe'), vm: ciIcon('desktop-tower'), ctr: ciIcon('layers'), snap: ciIcon('camera'),
  net: ciIcon('globe'), storage: ciIcon('data'), cluster: ciIcon('layers'), ovn: ciIcon('layers'),
  alert: ciIcon('bell'), gpu: ciIcon('monitor'), docker: ciIcon('layers'), terraform: ciIcon('file-document'),
  federation: ciIcon('cloud'), config: ciIcon('settings'), template: ciIcon('file-document'), backup: ciIcon('save'),
  error: ciIcon('close-circle'), ok: ciIcon('circle-check'), info: ciIcon('info')
};

function popoutEventLog() {
  const w = window.open('', 'pcv-event-log', 'width=700,height=500,menubar=no,toolbar=no,location=no,status=no');
  if (!w) { toast('팝업이 차단되었습니다', false); return; }
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
    + '<div style="display:flex;gap:6px"><button class="btn" style="font-size:10px;padding:3px 8px" onclick="parent.clearEvts()">초기화</button>'
    + '<span id="ev-count" class="color-muted" style="font-size:10px"></span></div></div>'
    + '<div class="ev-body" id="ev-body"></div>'
    + '</body></html>');
  w.document.close();
  _syncPopoutLog();
}
window.popoutEventLog = popoutEventLog;

(function() {
  var mk = PCV.uxlib.el;
  const tls = document.getElementById('login-tls');
  if (!tls) return;
  PCV.uxlib.clearEl(tls);
  if (location.protocol === 'https:') {
    tls.appendChild(mk('span', { class: 'login-tls-compact color-green' },
      ciIconNode('lock'),
      mk('span', { class: 'login-tls-label' }, t('login.tls.secure'))));
  } else {
    tls.appendChild(mk('span', { class: 'login-tls-compact color-yellow' },
      ciIconNode('warning'),
      mk('span', { class: 'login-tls-label' }, t('login.tls.insecure')),
      mk('span', { 'aria-hidden': 'true' }, '—'),
      mk('a', { class: 'login-tls-action', href: 'https://' + encodeURIComponent(location.hostname) + ':443' + encodeURI(location.pathname) }, t('login.tls.switch'))));
  }
})();

window.addEventListener('DOMContentLoaded', () => {

  const ALLOWED = ['supanova', 'supanova-cyan', 'supanova-hicontrast'];
  const urlTheme = new URLSearchParams(window.location.search).get('theme');
  let t = urlTheme || localStorage.getItem('pcv-theme') || 'supanova';
  if (ALLOWED.indexOf(t) < 0) t = 'supanova';
  document.documentElement.setAttribute('data-theme', t);
  localStorage.setItem('pcv-theme', t);
  const s = document.getElementById('theme-select');
  if (s) s.value = t;
});

var ctrSortKey = 'name', ctrSortDir = 1;

window.setCtrSort = setCtrSort;

window.toggleInfraSort = toggleInfraSort;

(function() {
  let dragEl = null;
  function initDrag(container) {
    const items = container.querySelectorAll('.vi[draggable]');
    items.forEach(el => {
      el.addEventListener('dragstart', e => { dragEl = el; el.style.opacity = '.4'; e.dataTransfer.effectAllowed = 'move'; });
      el.addEventListener('dragend', () => { dragEl.style.opacity = ''; dragEl = null; container.querySelectorAll('.vi').forEach(v => v.style.borderTop = ''); });
      el.addEventListener('dragover', e => { e.preventDefault(); e.dataTransfer.dropEffect = 'move'; el.style.borderTop = '2px solid var(--accent)'; });
      el.addEventListener('dragleave', () => { el.style.borderTop = ''; });
      el.addEventListener('drop', e => { e.preventDefault(); el.style.borderTop = ''; if (dragEl && dragEl !== el) { container.insertBefore(dragEl, el); saveNavOrder(container); } });
    });
    restoreNavOrder(container);
  }
  function saveNavOrder(c) {
    const order = [...c.querySelectorAll('.vi[data-nav]')].map(v => v.dataset.nav);
    localStorage.setItem('pcv-nav-' + c.id, JSON.stringify(order));
  }
  function restoreNavOrder(c) {
    const saved = localStorage.getItem('pcv-nav-' + c.id);
    if (!saved) return;
    try {
      const order = JSON.parse(saved);
      const map = {}; c.querySelectorAll('.vi[data-nav]').forEach(v => { map[v.dataset.nav] = v; });
      order.forEach(key => { if (map[key]) { c.appendChild(map[key]); delete map[key]; } });
      Object.values(map).forEach(v => c.appendChild(v));
    } catch (e) { if(_DEBUG) console.warn('restoreNavOrder:', e.message); }
  }
  window.addEventListener('DOMContentLoaded', () => {
    const infra = document.getElementById('nav-infra');
    const mon = document.getElementById('nav-mon');
    if (infra) initDrag(infra);
    if (mon) initDrag(mon);
  });
})();

document.addEventListener('click', () => { document.getElementById('ctx').style.display = 'none'; });

document.getElementById('ct').addEventListener('click', e => {
  if (e.target.tagName === 'BUTTON') {
    document.querySelectorAll('#ct button').forEach(b => b.classList.remove('active'));
    e.target.classList.add('active');
    currentTab = e.target.dataset.t;
    renderContent();
  }
});

window.snapNameValidate = snapNameValidate;
window.snapCreateExec = snapCreateExec;

window.rbValidate = rbValidate;
window.rbExec = rbExec;

window.snapDeleteAll = snapDeleteAll;
window.sdaPreview = sdaPreview;
window.sdaExec = sdaExec;

var selCtr = null, ctrTab = 'summary', ctrHist = [];

window.ctrDistChanged = ctrDistChanged;

window.ctrIpModeChanged = ctrIpModeChanged;

window.ctrLoadBridges = ctrLoadBridges;

window.pw = vmPower;

window.showM = showModal;
window.closeM = closeModal;

function showConnect() {
  var el = PCV.uxlib.el;
  var cards = MON_NODES.map((nd, i) => HN.card(nd.name + (i === 0 ? ' (Current)' : ''), [
    HN.row('IP', nd.ip),
    HN.row('Port', '8080'),
    HN.row('Status', el('span', { class: 'color-green' }, t('connected')))
  ]));
  showModal([
    el('h2', null, 'Connect to Server'),
    el('div', { class: 'sg' }, cards),
    el('div', { style: 'text-align:right;margin-top:12px' },
      el('button', { class: 'btn btn-r', onclick: 'closeModal()' }, t('btn.close')))
  ]);
}

function showPrefs() {
  var el = PCV.uxlib.el;
  const curTheme = document.documentElement.getAttribute('data-theme') || '';
  var previews = THEME_PREVIEWS.map(tp => {
    const sel = tp.id === curTheme;
    return el('div', {
      onclick: 'changeTheme(\'' + tp.id + '\');document.getElementById(\'theme-select\').value=\'' + tp.id + '\';showPrefs()',
      style: 'cursor:pointer;padding:8px;border-radius:8px;border:2px solid ' + (sel ? 'var(--accent)' : 'var(--border)') + ';background:var(--bg2);text-align:center' + (sel ? ';box-shadow:0 0 8px var(--accent)' : '')
    },
      el('div', { style: 'display:flex;gap:3px;justify-content:center;margin-bottom:6px' },
        tp.colors.map(c => el('div', { style: 'width:20px;height:20px;border-radius:4px;background:' + c + ';border:1px solid rgba(255,255,255,0.1)' }))),
      el('div', { style: 'font-size:9px;color:var(--fg2);white-space:nowrap' }, tp.name));
  });
  showModal([
    el('h2', null, 'Preferences'),
    el('div', { class: 'fr' },
      el('label', { for: 'app-default-pool' }, 'Default Pool'),
      el('input', { id: 'app-default-pool', value: 'pcvpool/vms', disabled: '' })),
    el('div', { class: 'fr' },
      el('label', { for: 'app-api-port' }, 'API Port'),
      el('input', { id: 'app-api-port', value: '8080', disabled: '' })),
    el('div', { class: 'fr' },
      el('label', { for: 'app-theme' }, 'Theme'),
      el('select', { id: 'app-theme', onchange: 'changeTheme(this.value);document.getElementById(\'theme-select\').value=this.value' },
        el('option', { value: 'supanova' }, 'SUPANOVA (Teal)'),
        el('option', { value: 'supanova-cyan' }, 'SUPANOVA CYAN'),
        el('option', { value: 'supanova-hicontrast' }, 'SUPANOVA HI-CONTRAST'))),
    el('div', { style: 'margin:12px 0' },
      el('label', { style: 'font-size:12px;color:var(--fg2)' }, 'Theme Preview'),
      el('div', { style: 'display:grid;grid-template-columns:repeat(auto-fill,minmax(120px,1fr));gap:8px;margin-top:8px' }, previews)),

    el('div', { style: 'margin:14px 0;border-top:1px solid var(--border);padding-top:12px' },
      el('h4', { style: 'margin-bottom:8px' }, 'Configuration Management'),
      el('div', { class: 'flex gap-6' },
        el('button', { class: 'btn btn-g', onclick: 'configBackup()' }, '💾 Backup Config'),
        el('button', { class: 'btn', onclick: 'configHistory()' }, '📋 Config History'))),
    el('div', { class: 'flex gap-6 mt-12' },
      el('button', { class: 'btn', onclick: 'exportUiSettings()' }, _L('설정 내보내기', 'Export Settings')),
      el('button', { class: 'btn', onclick: 'importUiSettings()' }, _L('설정 가져오기', 'Import Settings'))),
    el('div', { style: 'text-align:right;margin-top:12px' },
      el('button', { class: 'btn', onclick: 'openThemeEditor()', style: 'margin-right:8px' }, 'Theme Editor'),
      el('button', { class: 'btn btn-r', onclick: 'closeModal()' }, t('btn.close')))
  ]);
}

function showAbout() {
  var el = PCV.uxlib.el;
  showModal([
    el('h2', null, 'About PureCVisor'),
    HN.card('', [
      HN.row('Version', el('span', { id: 'about-ver' }, 'Loading...')),
      HN.row('LOC', el('span', { id: 'about-loc' }, 'Loading...')),
      HN.row('Files', el('span', { id: 'about-files' }, 'Loading...')),
      HN.row('RPC', el('span', { id: 'about-rpc' }, 'Loading...')),
      HN.row('REST Endpoints', el('span', { id: 'about-rest' }, 'Loading...')),
      HN.row('Prometheus Metrics', el('span', { id: 'about-prom' }, 'Loading...')),
      HN.row('Subsystems', 'io_uring, OVN, DPDK, SR-IOV, gRPC, WebSocket'),
      HN.row('Author', 'HardcoreMonk')
    ]),
    el('div', { style: 'text-align:right;margin-top:12px' },
      el('button', { class: 'btn btn-r', onclick: 'closeModal()' }, t('btn.close')))
  ]);

  fetchGet(API_BASE + '/health').then(r => {
    var d = unwrapData(r);
    var el = document.getElementById('about-ver');

    if (el) {
      PCV.uxlib.clearEl(el);
      el.appendChild(PCV.uxlib.frag(
        String(d.version || '1.0') + ' ',
        PCV.uxlib.el('span', { class: 'stat-label' }, '(' + (d.status || 'ok') + ')')
      ));
    }
    var rpc_el = document.getElementById('about-rpc');
    if (rpc_el) rpc_el.textContent = (d.rpc_methods || '265') + ' + plugins';
    var rest_el = document.getElementById('about-rest');
    if (rest_el) rest_el.textContent = (d.rest_endpoints || '190') + '+';
    var prom_el = document.getElementById('about-prom');
    if (prom_el) prom_el.textContent = d.metrics_count || '~170';
  }).catch(function() {});
  var loc_el = document.getElementById('about-loc');
  if (loc_el) loc_el.textContent = '~82,000 src / ~125,800 total';
  var files_el = document.getElementById('about-files');
  if (files_el) files_el.textContent = 'Single Edge public tree';
}

window.alertSave = async function() {
  const cfg = { enabled: document.getElementById('al-enabled')?.checked || false, cpu_warn: parseInt(document.getElementById('al-cpu_warn')?.value || 80), cpu_crit: parseInt(document.getElementById('al-cpu_crit')?.value || 95), mem_warn: parseInt(document.getElementById('al-mem_warn')?.value || 85), mem_crit: parseInt(document.getElementById('al-mem_crit')?.value || 95), disk_warn: parseInt(document.getElementById('al-disk_warn')?.value || 80), disk_crit: parseInt(document.getElementById('al-disk_crit')?.value || 90), eval_period: parseInt(document.getElementById('al-eval_period')?.value || 30), webhook_url: document.getElementById('al-webhook_url')?.value || '', webhook_format: document.getElementById('al-webhook_format')?.value || 'generic', telegram_chat_id: document.getElementById('al-telegram_chat_id')?.value || '' };
  try { await fetchPut(API_BASE + '/alerts/config', cfg);
    const st = document.getElementById('al-status'); if (st) { st.textContent = t('alert.saved'); st.style.color = 'var(--green)'; setTimeout(() => { st.textContent = ''; }, 2000); }
    setTimeout(() => renderContent(), 500);
  } catch (e) { const st = document.getElementById('al-status'); if (st) { st.textContent = t('error') + ': ' + e.message; st.style.color = 'var(--red)'; } }
};

window.sgAddRule = async function() {
  const el = document.getElementById('sg-result');
  const sw = document.getElementById('sg-switch')?.value;
  const dir = document.getElementById('sg-dir')?.value;
  const pri = document.getElementById('sg-pri')?.value;
  const match = document.getElementById('sg-match')?.value;
  const act = document.getElementById('sg-act')?.value;
  if (!sw || !match) { if (el) PCV.uxlib.setMsg(el, 'err', null, 'Switch와 Match는 필수입니다'); return; }
  if (el) PCV.uxlib.setMsg(el, 'loading', null, '추가 중...');
  try {
    await fetchPost(API_BASE + '/ovn/acl', { switch_name: sw, direction: dir, priority: parseInt(pri), match: match, action: act });
    if (el) PCV.uxlib.setMsg(el, 'ok', null, 'ACL 규칙 추가 완료');
    toast('ACL 규칙 추가: ' + escapeHtml(sw));
  } catch (e) { if (el) PCV.uxlib.setMsg(el, 'err', null, '오류: ' + e.message); }
};

window.sgListRules = async function() {
  const el = document.getElementById('sg-rules');
  const sw = document.getElementById('sg-list-switch')?.value;
  if (!sw) { if (el) PCV.uxlib.setMsg(el, 'err', null, 'Switch 이름을 입력하세요'); return; }
  if (el) PCV.uxlib.setMsg(el, 'loading', null, '조회 중...');
  try {
    const r = await fetchGet(API_BASE + '/ovn/acl?switch=' + encodeURIComponent(sw));
    const list = Array.isArray(r) ? r : (r.data || r.result || []);
    if (list.length === 0) { if (el) PCV.uxlib.setMsg(el, 'muted', { tag: 'p', size: '12px' }, 'ACL 규칙 없음'); return; }
    var mk = PCV.uxlib.el;
    var rows = list.map(function(a) {
      var entry = typeof a === 'string' ? a : '';
      if (entry) return mk('tr', null, mk('td', { colspan: '4' }, entry));
      return mk('tr', null,
        mk('td', null, a.direction || ''),
        mk('td', null, String(a.priority || '')),
        mk('td', null, a.match || ''),
        mk('td', null, a.action || ''));
    });
    var table = mk('table', { style: 'font-size:11px' },
      mk('thead', null, mk('tr', null,
        mk('th', null, 'Direction'), mk('th', null, 'Priority'), mk('th', null, 'Match'), mk('th', null, 'Action'))),
      mk('tbody', null, rows));
    if (el) { PCV.uxlib.clearEl(el); el.appendChild(table); }
  } catch (e) { if (el) PCV.uxlib.setMsg(el, 'err', null, '오류: ' + e.message); }
};

window.testGpuList = async function() {
  const el = document.getElementById('gpu-list-result');
  if (!el) return;
  PCV.uxlib.setMsg(el, 'loading', null, 'GPU 목록 조회 중...');
  try {
    const r = await fetchGet(API_BASE + '/gpu/list');
    const list = Array.isArray(r) ? r : (r.data || r.result || []);
    if (list.length === 0) { PCV.uxlib.setMsg(el, 'muted', { tag: 'p', size: '12px' }, 'GPU 디바이스 없음'); return; }
    var mk = PCV.uxlib.el;
    var rows = list.map(function(g) {
      return mk('tr', null,
        mk('td', null, g.pci || g.address || ''),
        mk('td', null, g.name || g.device || ''),
        mk('td', null, g.driver || ''),
        mk('td', null, g.type || ''));
    });
    var table = mk('table', { style: 'font-size:11px' },
      mk('thead', null, mk('tr', null,
        mk('th', null, 'PCI'), mk('th', null, 'Name'), mk('th', null, 'Driver'), mk('th', null, 'Type'))),
      mk('tbody', null, rows));
    PCV.uxlib.clearEl(el); el.appendChild(table);
  } catch (e) { PCV.uxlib.setMsg(el, 'warn', { size: '12px' }, 'GPU REST 엔드포인트 미구현. CLI 사용: ', PCV.uxlib.el('code', null, 'pcvctl gpu list')); }
};

window.gpuPassthrough = async function() {
  const el = document.getElementById('gpu-action-result');
  const pci = document.getElementById('gpu-pci')?.value;
  const vm = document.getElementById('gpu-vm')?.value;
  if (!pci || !vm) { if (el) PCV.uxlib.setMsg(el, 'err', null, 'PCI 주소와 VM 이름을 입력하세요'); return; }
  if (el) PCV.uxlib.setMsg(el, 'loading', null, 'VFIO 바인딩 중...');
  try {
    await fetchPost(API_BASE + '/gpu/passthrough', { pci_address: pci, vm_name: vm });
    if (el) PCV.uxlib.setMsg(el, 'ok', null, 'VFIO 패스스루 완료: ' + pci + ' → ' + vm);
    toast('GPU Passthrough: ' + escapeHtml(pci));
  } catch (e) { if (el) PCV.uxlib.setMsg(el, 'warn', { size: '12px' }, 'GPU REST 엔드포인트 미구현. CLI 사용: ', PCV.uxlib.el('code', null, 'pcvctl gpu passthrough ' + pci + ' ' + vm)); }
};

window.gpuMdevCreate = async function() {
  const el = document.getElementById('gpu-action-result');
  const pci = document.getElementById('gpu-pci')?.value;
  if (!pci) { if (el) PCV.uxlib.setMsg(el, 'err', null, 'PCI 주소를 입력하세요'); return; }
  if (el) PCV.uxlib.setMsg(el, 'loading', null, 'vGPU 생성 중...');
  try {
    await fetchPost(API_BASE + '/gpu/mdev', { pci_address: pci });
    if (el) PCV.uxlib.setMsg(el, 'ok', null, 'vGPU 생성 완료: ' + pci);
    toast('vGPU created: ' + escapeHtml(pci));
  } catch (e) { if (el) PCV.uxlib.setMsg(el, 'warn', { size: '12px' }, 'GPU REST 엔드포인트 미구현. CLI 사용: ', PCV.uxlib.el('code', null, 'pcvctl gpu mdev create ' + pci)); }
};

window.doAuditSearch = async function() {
  const el = document.getElementById('audit-results');
  if (!el) return;
  PCV.uxlib.setMsg(el, 'loading', null, '검색 중...');
  try {
    const u = document.getElementById('audit-user')?.value;
    const m = document.getElementById('audit-method')?.value;
    const f = document.getElementById('audit-from')?.value;
    const t2 = document.getElementById('audit-to')?.value;
    let qs = 'limit=100';
    if (u) qs += '&user=' + encodeURIComponent(u);
    if (m) qs += '&action=' + encodeURIComponent(m);
    if (f) qs += '&from=' + encodeURIComponent(f);
    if (t2) qs += '&to=' + encodeURIComponent(t2);
    const url = API_BASE + '/audit/search?' + qs;
    const r = await fetchGet(url);
    const list = Array.isArray(r) ? r : (r.data || r.result || []);
    if (list.length === 0) { PCV.uxlib.setMsg(el, 'muted', { tag: 'p' }, '검색 결과 없음'); return; }
    var mk = PCV.uxlib.el;
    var rows = list.map(function(e) {
      return mk('tr', null,
        mk('td', null, e.ts || e.timestamp || ''),
        mk('td', null, e.username || e.user || ''),
        mk('td', null, e.method || e.action || ''),
        mk('td', null, e.target || ''),
        mk('td', null, e.result || e.status || ''),
        mk('td', null, e.src_ip || e.ip || ''));
    });
    var table = mk('table', { style: 'font-size:11px' },
      mk('thead', null, mk('tr', null,
        mk('th', null, '시각'), mk('th', null, '사용자'), mk('th', null, '메서드'),
        mk('th', null, '대상'), mk('th', null, '결과'), mk('th', null, 'IP'))),
      mk('tbody', null, rows));
    PCV.uxlib.clearEl(el); el.appendChild(table);
  } catch (e) { PCV.uxlib.setMsg(el, 'err', null, '오류: ' + e.message); }
};

window.loadWebhookDlq = async function() {
  var el = document.getElementById('dlq-list');
  if (!el) return;
  PCV.uxlib.setMsg(el, 'loading', null, 'DLQ 조회 중...');
  try {

    var r;
    try { r = await fetchGet(API_BASE + '/alerts/dlq'); } catch(e1) {
      r = await fetchPost(API_BASE + '/rpc', {jsonrpc:'2.0', method:'alert.dlq.list', params:{}, id:'dlq1'});
    }
    var items = Array.isArray(r) ? r : (r.data || r.result || []);
    if (items.length === 0) { PCV.uxlib.setMsg(el, 'ok', { tag: 'div', cls: 'stat-label' }, _L('DLQ 비어있음', 'DLQ empty')); return; }
    var mk = PCV.uxlib.el;
    var rows = items.map(function(d, i) {
      return mk('tr', null,
        mk('td', null, (d.url || d.webhook_url || '').substring(0, 40)),
        mk('td', null, mk('code', null, (d.payload || d.metric || d.error || '').substring(0, 60))),
        mk('td', null, d.timestamp || d.ts || '-'),
        mk('td', null, mk('button', { class: 'btn btn-sm', onclick: 'retryDlqItem(' + i + ')' }, _L('재시도', 'Retry'))));
    });
    var table = mk('table', { class: 'tbl', style: 'font-size:11px' },
      mk('thead', null, mk('tr', null,
        mk('th', null, 'URL'), mk('th', null, 'Payload'), mk('th', null, _L('시각', 'Time')), mk('th'))),
      mk('tbody', null, rows));
    PCV.uxlib.clearEl(el); el.appendChild(table);

    window._dlqItems = items;
  } catch (e) {
    PCV.uxlib.setMsg(el, 'warn', { tag: 'div', cls: 'stat-label' }, _L('DLQ 조회 불가', 'DLQ unavailable'));
  }
};

window.retryWebhookDlq = async function() {
  const el = document.getElementById('dlq-list');
  if (el) PCV.uxlib.setMsg(el, 'loading', null, '재시도 중...');
  try {
    var r = await fetchPost(API_BASE + '/webhook/dlq/retry', {});
    if (r && r.error) {
      toast('DLQ 재시도 실패: ' + r.error.message, false);
      if (el) PCV.uxlib.setMsg(el, 'warn', { size: '12px' }, 'DLQ 재시도 실패: ' + r.error.message);
      return;
    }
    toast('DLQ 전체 재시도 요청 완료');
    if (el) PCV.uxlib.setMsg(el, 'ok', { tag: 'p', size: '12px' }, '재시도 요청 전송 완료');
  } catch (e) {
    toast('DLQ 재시도 실패: ' + e.message, false);
    if (el) PCV.uxlib.setMsg(el, 'warn', { size: '12px' }, 'DLQ 재시도 요청 실패 (네트워크 오류)');
  }
};

window.retryDlqItem = async function(index) {
  var items = window._dlqItems || [];
  if (index < 0 || index >= items.length) return;
  var item = items[index];
  try {
    var r = await fetchPost(API_BASE + '/rpc', {jsonrpc:'2.0', method:'alert.dlq.retry', params:{index: index, url: item.url || item.webhook_url || ''}, id:'dlqr1'});
    if (r && r.error) { toast(_L('재시도 실패', 'Retry failed') + ': ' + r.error.message, false); return; }
    toast(_L('재시도 요청 전송됨', 'Retry requested'));
    window.loadWebhookDlq();
  } catch (e) {
    toast(_L('재시도 실패', 'Retry failed') + ': ' + e.message, false);
  }
};

async function apiKeyCreate() {
  var name = (document.getElementById('apikey-name')?.value || '').trim();
  var desc = (document.getElementById('apikey-desc')?.value || '').trim();
  var expiryDays = parseInt(document.getElementById('apikey-expiry')?.value, 10);
  if (!name) { toast('Name required', false); return; }

  var payload = { name: name, description: desc };
  var roleEl = document.getElementById('apikey-role');
  if (roleEl) {
    var role = parseInt(roleEl.value, 10);
    if (Number.isFinite(role)) payload.role = role;
  }
  if (Number.isFinite(expiryDays) && expiryDays > 0) {
    payload.expires_at = Math.floor(Date.now() / 1000) + expiryDays * 86400;
  }
  try {
    var r = await fetchPost(API_BASE + '/auth/apikeys', payload);
    if (r.error) { toast('Create failed: ' + (r.error.message || ''), false); return; }
    var d = r.data || r.result || r;
    var newEl = document.getElementById('apikey-new-result');
    if (newEl && d.api_key) {
      newEl.style.display = 'block';
      var mk = PCV.uxlib.el;
      PCV.uxlib.clearEl(newEl);
      newEl.appendChild(PCV.uxlib.frag(
        mk('span', { class: 'color-green' }, '✅ New API Key created. Copy it now (it won\'t be shown again):'),
        mk('br'),
        mk('code', { style: 'color:var(--accent);font-size:13px;word-break:break-all;user-select:all' }, d.api_key),
        mk('br'),
        mk('button', { class: 'btn', style: 'margin-top:6px;font-size:10px', onclick: function() { navigator.clipboard.writeText(d.api_key); toast('Copied!'); } }, '📋 Copy')
      ));
    }
    toast('API key created: ' + name);
    addEvt('API Key created: ' + name);
    var nameEl = document.getElementById('apikey-name'); if (nameEl) nameEl.value = '';
    var descEl = document.getElementById('apikey-desc'); if (descEl) descEl.value = '';

    var keysArea = document.getElementById('apikey-keys-area');
    if (keysArea && typeof window.renderApiKeys === 'function') window.renderApiKeys(keysArea);
  } catch (e) { toast('Error: ' + e.message, false); }
}
window.apiKeyCreate = apiKeyCreate;

async function renderDashboard(b) {
  showSkeleton(b);

  await collectHostMetrics();
  try {
    var vms = [], ctrs = [], clusterData = {}, alertData = [];

    var fastResults = await Promise.all([
      fetchGet(API_BASE + '/vms').catch(function() { return { data: [] }; }),
      fetchGet(API_BASE + '/containers').catch(function() { return { data: [] }; })
    ]);
    vms = Array.isArray(fastResults[0]) ? fastResults[0] : (fastResults[0].data || []);
    ctrs = Array.isArray(fastResults[1]) ? fastResults[1] : (fastResults[1].data || []);

    var clusterPromise = fetchGet(API_BASE + '/health').then(function(h) {
      var c = h.checks || {};
      var isStandalone = !h.capabilities || !h.capabilities.cluster;
      return {
        role: isStandalone ? 'standalone' : ((c.cluster && c.cluster.role) || 'unknown'),
        etcd_connected: isStandalone ? false : (c.etcd && c.etcd.ok && !c.etcd.note),
        leader: (c.cluster && c.cluster.role === 'leader') ? (h.node_name || '') : '',
        etcd_endpoints_healthy: c.etcd ? (c.etcd.healthy || 0) : 0,
        etcd_endpoints_total: c.etcd ? (c.etcd.total || 0) : 0,
        node_name: h.node_name || '',
        node_count: isStandalone ? 1 : undefined
      };
    }).catch(function() { return {}; });
    var alertPromise = Promise.race([
      fetchGet(API_BASE + '/alerts').catch(function() { return []; }),
      new Promise(function(r) { setTimeout(function() { r([]); }, 3000); })
    ]);
    var slowResults = await Promise.all([clusterPromise, alertPromise]);
    clusterData = slowResults[0].data || slowResults[0].result || slowResults[0] || {};
    alertData = Array.isArray(slowResults[1]) ? slowResults[1] : (slowResults[1].data || []);

    var runVms = vms.filter(function(v) { return v.state === 'running'; }).length;
    var runCtrs = ctrs.filter(function(c) { return c.state === 'RUNNING'; }).length;
    var nodeCount = clusterData.nodes ? clusterData.nodes.length : (clusterData.node_count || 1);
    var role = clusterData.role || 'standalone';
    var recentAlerts = alertData.slice(-5);
    var totalWorkloads = vms.length + ctrs.length;
    var connectedWorkloads = runVms + runCtrs;

    var mk = PCV.uxlib.el;
    var parts = [];

    parts.push(mk('section', { class: 'ops-hero' },
      mk('div', { class: 'ops-hero-copy' },
        mk('span', { class: 'ops-kicker' }, _L('Single Edge', 'Single Edge')),
        mk('h2', null, _L('싱글 엣지 운영 대시보드', 'Single Edge Operations Dashboard')),
        mk('p', null, _L('호스트 상태, 워크로드, 최근 경고를 한 화면에서 확인합니다.', 'See host health, workloads, and recent alerts in one place.')),
        mk('div', { class: 'ops-pill-row' },
          mk('span', { class: 'ops-pill' }, _L('활성 워크로드', 'Active workloads') + ' ', mk('b', null, connectedWorkloads + '/' + totalWorkloads)),
          mk('span', { class: 'ops-pill' }, _L('호스트 모드', 'Host mode') + ' ', mk('b', null, _L('단일 노드', 'Single node'))),
          mk('span', { class: 'ops-pill' }, _L('최근 경고', 'Recent alerts') + ' ', mk('b', null, alertData.length)))),
      mk('div', { class: 'ops-hero-aside hc' },
        mk('h4', null, _L('운영 메모', 'Operations note')),
        HN.row(_L('현재 역할', 'Current role'), HN.badge(role === 'standalone' ? _L('단독 운영', 'Standalone') : role, role === 'standalone' ? 'g' : 'y')),
        HN.row(_L('웹소켓', 'WebSocket'), document.getElementById('ws-s') && document.getElementById('ws-s').textContent ? _L('연결됨', 'Connected') : _L('연결 대기', 'Pending')),
        HN.row(_L('운영 우선순위', 'Priority'), alertData.length > 0 ? _L('경고 확인', 'Review alerts') : _L('자원 추이 점검', 'Review resource trend')))));

    parts.push(mk('div', { class: 'ops-section-heading' },
      mk('div', null,
        mk('h3', null, _L('표시 항목', 'Visible sections')),
        mk('p', null, _L('대시보드에서 바로 보고 싶은 카드만 켜 두십시오.', 'Keep only the sections you want to see on the dashboard.')))));

    var _dwList = [
      {key:'stats', label: _L('운영 요약','Operations summary'), icon:'house-01'},
      {key:'actions', label: _L('빠른 작업','Quick actions'), icon:'plus-circle'},
      {key:'charts', label: _L('자원 추이','Resource charts'), icon:'chart-line'},
      {key:'alerts', label: _L('최근 경고','Recent alerts'), icon:'bell'},
      {key:'vms', label: _L('워크로드 표','Workload tables'), icon:'menu-alt-01'}
    ];
    parts.push(mk('div', { class: 'flex gap-4 mb-12', style: 'flex-wrap:wrap' },
      _dwList.map(function(w) {
        var on = _dashWidgets[w.key] !== false;
        return mk('button', { class: 'btn dash-widget-toggle ' + (on ? 'is-active' : ''), onclick: "toggleDashWidget('" + w.key + "')" }, ciIconNode(w.icon), ' ' + w.label);
      })));

    if (_dashWidgets.stats !== false) {
      parts.push(mk('div', { class: 'ops-section-heading' },
        mk('div', null,
          mk('h3', null, _L('운영 요약', 'Operations summary')),
          mk('p', null, _L('가상 머신, 컨테이너, 호스트 상태를 한 번에 확인합니다.', 'Review virtual machines, containers, and host status at a glance.')))));
      var statCards = [
        HN.card([ciIconNode('monitor'), ' ' + _L('가상 머신', 'Virtual Machines')], [
          mk('div', { class: 'stat-lg color-accent' }, vms.length),
          HN.row(_L('실행 중', 'Running'), mk('span', { class: 'color-green' }, runVms)),
          HN.row(_L('정지', 'Stopped'), mk('span', { class: 'color-muted' }, vms.length - runVms))
        ]),
        HN.card([ciIconNode('layers'), ' ' + _L('컨테이너', 'Containers')], [
          mk('div', { class: 'stat-lg color-green' }, ctrs.length),
          HN.row(_L('실행 중', 'Running'), mk('span', { class: 'color-green' }, runCtrs)),
          HN.row(_L('정지', 'Stopped'), mk('span', { class: 'color-muted' }, ctrs.length - runCtrs))
        ]),
        window.pcvClusterEnabled
          ? HN.card([ciIconNode('copy'), ' ' + _L('클러스터', 'Cluster')], [
              mk('div', { class: 'stat-lg', style: 'color:var(--yellow)' }, nodeCount + ' ' + _L('노드', 'Nodes')),
              HN.row(_L('역할', 'Role'), HN.badge(role, role === 'leader' ? 'g' : 'y')),
              HN.row('etcd', HN.badge(clusterData.etcd_connected ? 'Connected' : 'N/A', clusterData.etcd_connected ? 'g' : 'r'))
            ])
          : HN.card([ciIconNode('desktop-tower'), ' ' + _L('호스트', 'Host')], [
              mk('div', { class: 'stat-lg', style: 'color:var(--yellow)' }, _L('정상', 'Healthy')),
              HN.row(_L('모드', 'Mode'), HN.badge('Single Edge', 'g')),
              HN.row(_L('상태', 'Status'), HN.badge(_L('운영 중', 'Active'), 'g'))
            ]),
        HN.card([ciIconNode('bell'), ' ' + _L('경고', 'Alerts')], [
          mk('div', { class: 'stat-lg color-red' }, alertData.length),
          HN.row(_L('최근', 'Recent'), recentAlerts.length + _L('건', ' items'))
        ])
      ];
      parts.push(mk('div', { class: 'sg grid-4' }, statCards));
    }

    if (_dashWidgets.actions !== false) {
      parts.push(mk('div', { class: 'ops-section-heading' },
        mk('div', null,
          mk('h3', null, _L('빠른 작업', 'Quick actions')),
          mk('p', null, _L('생성, 네트워크, 스토리지, 모니터링처럼 자주 쓰는 작업만 앞으로 배치했습니다.', 'The most common actions are kept in front: create, networking, storage, and monitoring.')))));
      var shortcuts = [
        { icon: 'monitor', label: _L('새 VM', 'New VM'), action: 'showCreate()', color: 'var(--green)' },
        { icon: 'layers', label: _L('새 컨테이너', 'New Container'), action: 'showCtrCreate()', color: 'var(--cyan)' },
        { icon: 'globe', label: _L('네트워크', 'Networks'), action: "navigateTo('networks')", color: 'var(--accent)' },
        { icon: 'save', label: _L('스토리지', 'Storage'), action: "navigateTo('storage')", color: 'var(--peach)' },
        { icon: 'chart-line', label: _L('운영 개요', 'Operations Overview'), action: "navigateTo('mon-overview')", color: 'var(--yellow)' },
        { icon: 'desktop-tower', label: _L('호스트 상태', 'Host Health'), action: "navigateTo('host')", color: 'var(--cyan)' },
        { icon: 'book', label: _L('서비스 가이드', 'Service Guide'), action: "navigateTo('serviceguide')", color: 'var(--green)' },
      ];
      if (window.pcvClusterEnabled) {
        shortcuts.splice(5, 0, { icon: 'copy', label: _L('클러스터', 'Cluster'), action: "navigateTo('cluster')", color: 'var(--magenta)' });
      }
      parts.push(mk('div', { class: 'sg grid-4' },
        shortcuts.map(function(s) {
          return mk('div', { class: 'hc ops-shortcut-card', onclick: s.action },
            mk('div', { class: 'ops-shortcut-icon', style: 'color:' + s.color }, ciIconNode(s.icon)),
            mk('div', { class: 'ops-shortcut-label', style: 'color:' + s.color }, s.label));
        })));
    }

    if (_dashWidgets.charts !== false) {

      var hostCpu = hostCpuHistory[hostCpuHistory.length - 1] || 0;
      var hostMem = hostMemHistory[hostMemHistory.length - 1] || 0;
      parts.push(mk('div', { class: 'ops-section-heading' },
        mk('div', null,
          mk('h3', null, _L('실시간 자원 추이', 'Live resource trend')),
          mk('p', null, _L('CPU와 메모리 사용률이 최근 수집값 기준으로 즉시 갱신됩니다.', 'CPU and memory usage update from the latest collected samples.')))));
      parts.push(mk('div', { class: 'sg grid-2' },
        HN.card('CPU ' + _L('사용률', 'Usage') + ' — ' + hostCpu.toFixed(1) + '%', [
          _progressBar(hostCpu),
          mk('div', { style: 'position:relative;height:120px;width:100%;margin-top:8px' }, mk('canvas', { id: 'dash-cpu-chart' }))
        ]),
        HN.card(_L('메모리 사용률', 'Memory Usage') + ' — ' + hostMem.toFixed(1) + '%', [
          _progressBar(hostMem),
          mk('div', { style: 'position:relative;height:120px;width:100%;margin-top:8px' }, mk('canvas', { id: 'dash-mem-chart' }))
        ])));
    }

    if (_dashWidgets.alerts !== false && recentAlerts.length > 0) {
      parts.push(mk('div', { class: 'ops-section-heading' },
        mk('div', null,
          mk('h3', null, _L('최근 경고', 'Recent alerts')),
          mk('p', null, _L('실시간 이벤트 중 운영에 바로 영향을 주는 항목만 먼저 확인합니다.', 'Review only the alerts that need immediate operational attention.')))));
      var alertRows = recentAlerts.map(function(a) {
        return mk('tr', null,
          mk('td', { class: 'color-muted' }, a.timestamp || a.time || '-'),
          mk('td', null, HN.badge(a.level || a.type || '?', a.level === 'critical' ? 'r' : 'y')),
          mk('td', null, a.message || a.detail || '-'));
      });
      parts.push(mk('table', { style: 'font-size:12px' },
        mk('thead', null, mk('tr', null,
          mk('th', null, _L('시각', 'Time')), mk('th', null, _L('유형', 'Type')), mk('th', null, _L('내용', 'Message')))),
        mk('tbody', null, alertRows)));
    }

    if (_dashWidgets.vms !== false && vms.length > 0) {
      parts.push(mk('div', { class: 'ops-section-heading' },
        mk('div', null,
          mk('h3', null, _L('워크로드 현황', 'Workload overview')),
          mk('p', null, _L('대시보드에서는 최근 상태만 보고, 세부 조작은 각 화면에서 이어갑니다.', 'Use the dashboard for status checks, then continue detailed actions in each screen.')))));
      parts.push(mk('h3', { style: 'margin:8px 0 12px' }, ciIconNode('monitor'), ' ' + _L('VM 현황', 'VM Status') + ' (' + vms.length + ')'));
      var vmRows = vms.slice(0, 10).map(function(v) {
        var on = v.state === 'running';
        return mk('tr', { style: 'cursor:pointer', onclick: 'selectedVmIndex=' + vms.indexOf(v) + ";currentTab='summary';switchSbTab('vms');render()" },
          mk('td', null, mk('b', null, v.name)),
          mk('td', null, HN.badge(v.state || '?', on ? 'g' : 'r')),
          mk('td', null, v.vcpu || '-'),
          mk('td', null, (v.memory_mb || '-') + ' MB'));
      });
      if (vms.length > 10) vmRows.push(mk('tr', null, mk('td', { colspan: '4', class: 'color-muted text-center' }, '... ' + _L('외', 'and') + ' ' + (vms.length - 10) + _L('개', ' more'))));
      parts.push(mk('table', { style: 'font-size:12px' },
        mk('thead', null, mk('tr', null,
          mk('th', null, _L('이름', 'Name')), mk('th', null, _L('상태', 'State')), mk('th', null, 'vCPU'), mk('th', null, _L('메모리', 'Memory')))),
        mk('tbody', null, vmRows)));
    }

    if (ctrs.length > 0) {
      parts.push(mk('h3', { style: 'margin:20px 0 12px' }, ciIconNode('layers'), ' ' + _L('컨테이너 현황', 'Container Status') + ' (' + ctrs.length + ')'));
      var ctrRows = ctrs.slice(0, 10).map(function(c) {
        var on = c.state === 'RUNNING';
        return mk('tr', { style: 'cursor:pointer', onclick: "selCtr='" + esc(c.name) + "';currentTab='containers';renderContent();renderContainerList()" },
          mk('td', null, mk('b', null, c.name)),
          mk('td', null, HN.badge(c.state || '?', on ? 'g' : 'r')),
          mk('td', null, c.ip_addr || c.ip || '-'),
          mk('td', { class: 'color-muted' }, c.image || '-'));
      });
      if (ctrs.length > 10) ctrRows.push(mk('tr', null, mk('td', { colspan: '4', class: 'color-muted text-center' }, '... ' + _L('외', 'and') + ' ' + (ctrs.length - 10) + _L('개', ' more'))));
      parts.push(mk('table', { style: 'font-size:12px' },
        mk('thead', null, mk('tr', null,
          mk('th', null, _L('이름', 'Name')), mk('th', null, _L('상태', 'State')), mk('th', null, 'IP'), mk('th', null, _L('이미지', 'Image')))),
        mk('tbody', null, ctrRows)));
    }

    PCV.uxlib.clearEl(b);
    b.appendChild(PCV.uxlib.frag(parts));

    setTimeout(function() {
      if (typeof createLineChart === 'function') {
        createLineChart('dash-cpu-chart', hostCpuHistory, 'CPU %', getChartColor('cpu'));
        createLineChart('dash-mem-chart', hostMemHistory, 'MEM %', getChartColor('mem'));
      }
    }, 100);
  } catch (e) {
    var mkErr = PCV.uxlib.el;
    PCV.uxlib.clearEl(b);
    b.appendChild(PCV.uxlib.frag(
      mkErr('h2', null, _L('대시보드', 'Dashboard')),
      mkErr('p', { class: 'color-red' }, _L('오류', 'Error') + ': ' + e.message)
    ));
  }
}
window.renderDashboard = renderDashboard;

function toggleDashWidget(key) {
  _dashWidgets[key] = !(_dashWidgets[key] !== false);
  localStorage.setItem('pcv-dash-widgets', JSON.stringify(_dashWidgets));
  renderDashboard(document.getElementById('cb'));
}
window.toggleDashWidget = toggleDashWidget;

window._loadAllInFlight = false;
async function loadAll(skipContent) {
  if (window._loadAllInFlight) return;
  window._loadAllInFlight = true;
  try {
    if (!skipContent && typeof invalidateCache === 'function') invalidateCache('vm.list');
    const r = (typeof cachedFetch === 'function' && skipContent)
      ? await cachedFetch('vm.list', 500, function(){ return fetchGet(API_BASE + '/vms'); })
      : await fetchGet(API_BASE + '/vms');
    vmList = window.vmList = Array.isArray(r) ? r : (r.data || []);
    if (selectedVmIndex >= vmList.length) selectedVmIndex = window.selectedVmIndex = 0;
    lastLoadTime = Date.now();
    render(skipContent);
  } catch (e) {
    if(_DEBUG) console.warn('r:', e.message);
    if (typeof reportError === 'function') reportError('vm.list', e);
  } finally {
    window._loadAllInFlight = false;
  }
}
window.loadAll = loadAll;

async function pcvPostLoginInit() {
  try {
    const u = await fetchGet(API_BASE + '/auth/whoami').catch(function(){ return null; });
    if (u && u.data) {
      window.currentUser = u.data;
      if (typeof applyRoleVisibility === 'function') applyRoleVisibility(u.data.role);
    }
  } catch (_) {}
  if (typeof navigateToHash === 'function') navigateToHash();
}
window.pcvPostLoginInit = pcvPostLoginInit;

setInterval(() => { if (document.hidden) return; if (authToken) loadAll(true); }, 10e3);

async function collectHostMetrics() {
  var token = window.authToken || authToken;
  if (!token) return;
  try {
    var res = await fetch(EP.METRICS(), { headers: { Authorization: 'Bearer ' + token } });
    if (!res.ok) return;
    var met = await res.text();
    var cpu = 0, mem = 0;
    met.split('\n').forEach(function(l) {
      if (l.startsWith('purecvisor_host_cpu_percent ')) cpu = parseFloat(l.split(' ')[1]);
      if (l.startsWith('purecvisor_host_memory_percent ')) mem = parseFloat(l.split(' ')[1]);
    });
    hostCpuHistory.push(cpu); hostCpuHistory.shift();
    hostMemHistory.push(mem); hostMemHistory.shift();
  } catch (e) {  }
}
setInterval(() => { if (document.hidden) return; collectHostMetrics(); }, 5000);

setInterval(() => { if (document.hidden) return; const sb3 = document.getElementById('sb3'); if (sb3 && authToken) { const elapsed = Math.round((Date.now() - lastLoadTime) / 1000); sb3.textContent = 'Updated ' + elapsed + 's ago'; } }, 1000);

document.addEventListener('visibilitychange', () => {
  if (document.hidden) return;
  if (authToken) loadAll(true);
  collectHostMetrics();
  if (typeof updateStatusBar === 'function') updateStatusBar();
});

document.addEventListener('keydown', e => {
  if (e.key === 'F11') { e.preventDefault(); toggleFS(); }
  if (e.ctrlKey && e.key === 'n') { e.preventDefault(); showCreate(); }
  if (e.ctrlKey && e.key === 'd') { e.preventDefault(); showSettings(); }
  if (e.ctrlKey && e.key === 'p') { e.preventDefault(); showPrefs(); }

  if (e.ctrlKey && e.key === 'k') { e.preventDefault(); if (cmdPaletteOpen) closeCmdPalette(); else openCmdPalette(); }

  if (e.key === 'Escape') {
    if (cmdPaletteOpen) { closeCmdPalette(); e.preventDefault(); return; }
    const mbg = document.getElementById('mbg');
    if (mbg && !mbg.classList.contains('hidden')) { closeModal(); e.preventDefault(); }
    const iso = document.getElementById('iso-overlay');
    if (iso) { closeISOBrowser(); e.preventDefault(); }
  }

  if (e.key === 'Tab') {
    const mbg = document.getElementById('mbg');
    if (mbg && !mbg.classList.contains('hidden')) {
      const modal = document.getElementById('mc');
      const focusable = modal.querySelectorAll('input,select,textarea,button,[tabindex]');
      if (focusable.length > 0) {
        const first = focusable[0], last = focusable[focusable.length - 1];
        if (e.shiftKey && document.activeElement === first) { e.preventDefault(); last.focus(); }
        else if (!e.shiftKey && document.activeElement === last) { e.preventDefault(); first.focus(); }
      }
    }
  }
});

(document.getElementById('sidebar-panel') || document.getElementById('sidebar')).addEventListener('click', e => {
  if (window.innerWidth <= 768 && e.target.closest('.vi')) { setTimeout(closeMobileSB, 150); }
});

function handleResize() {
  const btn = document.getElementById('mobile-menu-btn'); const sb = document.getElementById('sidebar-panel') || document.getElementById('sidebar');
  if (window.innerWidth <= 768) { btn.style.display = 'block'; if (!sb.classList.contains('mobile-open')) { sb.classList.remove('collapsed'); } }
  else { btn.style.display = 'none'; sb.classList.remove('mobile-open'); document.getElementById('mobile-overlay').style.display = 'none'; }
}
window.addEventListener('resize', handleResize);
handleResize();

var touchStartX = 0, touchStartY = 0;
document.addEventListener('touchstart', e => { touchStartX = e.touches[0].clientX; touchStartY = e.touches[0].clientY; }, { passive: true });
document.addEventListener('touchend', e => {
  if (window.innerWidth > 768) return;
  const dx = e.changedTouches[0].clientX - touchStartX;
  const dy = Math.abs(e.changedTouches[0].clientY - touchStartY);
  if (dy > 80) return;
  if (dx > 60 && touchStartX < 40) { toggleMobileSB(); }
  else if (dx < -60 && (document.getElementById('sidebar-panel') || document.getElementById('sidebar')).classList.contains('mobile-open')) { closeMobileSB(); }
}, { passive: true });

var audioCtx = null;
function playNotifSound(type) {
  try {
    if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    const osc = audioCtx.createOscillator();
    const gain = audioCtx.createGain();
    osc.connect(gain); gain.connect(audioCtx.destination);
    gain.gain.value = 0.08;
    if (type === 'error') { osc.frequency.value = 300; osc.type = 'square'; }
    else if (type === 'warning') { osc.frequency.value = 500; osc.type = 'triangle'; }
    else { osc.frequency.value = 800; osc.type = 'sine'; }
    osc.start(); osc.stop(audioCtx.currentTime + 0.12);
  } catch (e) {  }
}

var browserNotifEnabled = false;
function requestBrowserNotif() {
  if (!('Notification' in window)) return;
  if (Notification.permission === 'granted') { browserNotifEnabled = true; return; }
  if (Notification.permission !== 'denied') {
    Notification.requestPermission().then(p => { browserNotifEnabled = (p === 'granted'); });
  }
}
function sendBrowserNotif(title, body, icon) {
  if (!browserNotifEnabled) return;
  try { new Notification(title, { body: body, icon: icon || '', tag: 'pcv-' + Date.now() }); }
  catch (e) {  }
}

var _origDoLoginPage = typeof doLoginPage === 'function' ? doLoginPage : null;

if ('serviceWorker' in navigator) {
  window.addEventListener('load', () => {
    navigator.serviceWorker.register('/ui/sw.js', { updateViaCache: 'none' }).then(reg => {

      if (reg.waiting) reg.waiting.postMessage({ type: 'SKIP_WAITING' });
      reg.addEventListener('updatefound', () => {
        const nw = reg.installing;
        if (!nw) return;
        nw.addEventListener('statechange', () => {
          if (nw.state === 'installed' && navigator.serviceWorker.controller) {
            nw.postMessage({ type: 'SKIP_WAITING' });
          }
        });
      });
      reg.update().catch(() => {});
    }).catch(() => {});

    navigator.serviceWorker.addEventListener('controllerchange', () => {
      window.location.reload();
    });
  });
}

document.addEventListener('keydown', e => {
  if (e.key === '?' && !e.ctrlKey && !e.altKey && !e.metaKey) {
    const tag = document.activeElement?.tagName;
    if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return;
    e.preventDefault();
    toggleKbdHelp();
  }
  if (e.key === 'Escape' && kbdHelpOpen) { e.preventDefault(); closeKbdHelp(); }
});

requestBrowserNotif();

CMD_ACTIONS.push(
  { icon: '&#128195;', label: 'Templates', action: () => navigateTo('templates') },
  { icon: '&#9881;', label: 'Config Management', action: () => navigateTo('config-mgmt') },

  { icon: '&#128230;', label: 'Import OVA', action: () => showImportOva() },
  { icon: '&#9729;', label: 'Cloud Migration', action: () => navigateTo('cloud-migration') },
  { icon: '&#128269;', label: 'Global Search', hint: 'Ctrl+Shift+F', action: () => toggleGlobalSearch() },
  { icon: '&#9647;', label: 'Toggle Split View', hint: 'Ctrl+\\', action: () => toggleSplitView() },
  { icon: '&#128276;', label: 'Notifications', action: () => toggleNotifCenter() },
  { icon: '&#127748;', label: 'Zen Mode', hint: 'Ctrl+Shift+Z', action: () => toggleZenMode() },
  { icon: '&#9881;', label: 'Toggle Bottom Panel', action: () => toggleBottomPanel() }
);

if (!window._navTabsWrapped) {
  window._pcvOrigNavigateTo = navigateTo;
  window.navigateTo = function navigateToWithTabs(n) {
    if (navigateToWithTabs._busy) { window._pcvOrigNavigateTo(n); return; }
    navigateToWithTabs._busy = true;
    try {
      var tabIcons = {
        'networks': '&#127760;', 'storage': '&#128190;', 'containers': '&#9783;',
        'host': '&#128187;', 'cluster': '&#9741;', 'ovn': '&#9707;',
        'accounts': '&#128100;', 'mon-overview': '&#128200;', 'ops-triage': '&#9889;', 'mon-alerts': '&#128276;',
        'mon-cluster': '&#9741;', 'mon-hosts': '&#128187;', 'mon-vms': '&#9881;',
        'mon-storage': '&#128190;', 'mon-audit': '&#128270;',
        'security-groups': '&#128737;', 'gpu': '&#127918;',
        'federation': '&#127758;', 'apihelp': '&#128214;',
        'helppage': '&#10068;', 'serviceguide': '&#128218;', 'restguide': '&#128220;',
        'apimgmt': '&#128268;', 'templates': '&#128195;', 'config-mgmt': '&#9881;',
        'cloud-migration': '&#9729;',
      };
      var tabLabels = {
        'dashboard': _L('대시보드', 'Dashboard'),
        'summary': _L('요약', 'Summary'),
        'console': _L('콘솔', 'Console'),
        'snapshots': _L('스냅샷', 'Snapshots'),
        'performance': _L('성능', 'Performance'),
        'timeline': _L('타임라인', 'Timeline'),
        'networks': _L('네트워크', 'Networks'),
        'storage': _L('스토리지', 'Storage'),
        'containers': _L('컨테이너', 'Containers'),
        'host': _L('호스트 상태', 'Host Health'),
        'cluster': _L('클러스터', 'Cluster'),
        'ovn': 'OVN SDN',
        'accounts': _L('계정과 권한', 'Accounts & Permissions'),
        'mon-overview': _L('운영 개요', 'Operations Overview'),
        'ops-triage': _L('이벤트 센터', 'Event Center'),
        'mon-alerts': _L('알림', 'Alerts'),
        'mon-cluster': _L('클러스터 모니터', 'Cluster Monitor'),
        'mon-hosts': _L('호스트 상태', 'Host Health'),
        'mon-vms': _L('VM 모니터', 'VM Monitor'),
        'mon-storage': _L('스토리지 모니터', 'Storage Monitor'),
        'mon-audit': _L('감사 로그', 'Audit Log'),
        'security-groups': _L('보안 그룹', 'Security Groups'),
        'gpu': _L('GPU 장치', 'GPU'),
        'apihelp': _L('Swagger API', 'Swagger API'),
        'helppage': _L('도움말', 'Help'),
        'serviceguide': _L('서비스 가이드', 'Service Guide'),
        'restguide': _L('REST API 가이드', 'REST API Guide'),
        'apimgmt': _L('API 관리', 'API Management'),
        'templates': _L('템플릿', 'Templates'),
        'config-mgmt': _L('설정 관리', 'Configuration Management'),
        'cloud-migration': _L('클라우드 마이그레이션', 'Cloud Migration')
      };
      var label = tabLabels[n] || n.replace(/-/g, ' ').replace(/\b\w/g, function(c) { return c.toUpperCase(); });
      openEditorTab(n, label, tabIcons[n] || '&#128196;');
      window._pcvOrigNavigateTo(n);
      updateBreadcrumbs(n);
      if (typeof setHashRoute === 'function') setHashRoute(n);
      if (typeof setPageTitle === 'function') setPageTitle(label);
      if (typeof renderBreadcrumbs === 'function') {
        var group = n.startsWith('mon-') ? _L('모니터링', 'Monitoring')
                  : (['networks','storage','containers','host','cluster','ovn'].indexOf(n) >= 0 ? _L('운영', 'Operations')
                  : (['accounts','apimgmt'].indexOf(n) >= 0 ? _L('인증', 'Auth')
                  : (['apihelp','helppage','serviceguide','restguide'].indexOf(n) >= 0 ? _L('도움말', 'Help') : null)));
        var items = [{ label: _L('대시보드', 'Dashboard'), page: 'dashboard' }];
        if (group) items.push({ label: group });
        items.push({ label: label });
        renderBreadcrumbs(items);
      }
    } finally { navigateToWithTabs._busy = false; }
  };
  window.go = window.navigateTo;
  window._navTabsWrapped = true;
}

(function() {
  const handle = document.getElementById('panel-resize');
  const panel = document.getElementById('bottom-panel');
  if (!handle || !panel) return;
  let startY, startH;
  handle.addEventListener('mousedown', e => {
    startY = e.clientY; startH = panel.offsetHeight;
    handle.classList.add('dragging');
    const move = e2 => { panel.style.height = Math.max(100, Math.min(window.innerHeight * 0.7, startH - (e2.clientY - startY))) + 'px'; };
    const up = () => { handle.classList.remove('dragging'); document.removeEventListener('mousemove', move); document.removeEventListener('mouseup', up); };
    document.addEventListener('mousemove', move);
    document.addEventListener('mouseup', up);
  });
})();

if (!window._addEvtPanelWrapped) {
  var _origAddEvt = addEvt;
  function addEvtToPanel(m) {
    _origAddEvt(m);
    var panelEvents = document.getElementById('panel-events');
    if (panelEvents) {
      panelEvents.textContent = '';
      eventLog.forEach(function(e) {
        var div = document.createElement('div');
        div.style.cssText = 'padding:1px 0;border-bottom:1px solid var(--border)';
        div.textContent = typeof e === 'string' ? e : (e.raw || e.msg || '');
        panelEvents.appendChild(div);
      });
      panelEvents.scrollTop = panelEvents.scrollHeight;
    }
  }
  window.addEvt = addEvtToPanel;
  window._addEvtPanelWrapped = true;
}

var splitViewActive = false;
function toggleSplitView() {
  splitViewActive = !splitViewActive;
  const cb = document.getElementById('cb');
  if (!cb) return;
  if (splitViewActive) {
    cb.style.display = 'none';
    const split = document.createElement('div');
    split.id = 'split-container';
    split.className = 'split-container';
    split.appendChild(PCV.uxlib.frag(
      PCV.uxlib.el('div', { class: 'split-pane', id: 'split-left' }),
      PCV.uxlib.el('div', { class: 'split-divider', id: 'split-divider' }),
      PCV.uxlib.el('div', { class: 'split-pane', id: 'split-right' })
    ));
    cb.parentNode.insertBefore(split, cb.nextSibling);

    renderContent();
    const leftContent = cb.innerHTML;
    document.getElementById('split-left').innerHTML = leftContent;
    PCV.uxlib.setMsg('split-right', 'muted', { tag: 'p', style: 'padding:20px' }, 'Select content for right pane from the sidebar');
    initSplitDivider();
  } else {
    document.getElementById('split-container')?.remove();
    const cb2 = document.getElementById('cb');
    if (cb2) cb2.style.display = '';
  }
}

function initSplitDivider() {
  const divider = document.getElementById('split-divider');
  const left = document.getElementById('split-left');
  if (!divider || !left) return;
  let startX, startW;
  divider.addEventListener('mousedown', e => {
    startX = e.clientX; startW = left.offsetWidth;
    divider.classList.add('dragging');
    const move = e2 => { left.style.width = Math.max(200, startW + (e2.clientX - startX)) + 'px'; left.style.flex = 'none'; };
    const up = () => { divider.classList.remove('dragging'); document.removeEventListener('mousemove', move); document.removeEventListener('mouseup', up); };
    document.addEventListener('mousemove', move);
    document.addEventListener('mouseup', up);
  });
}

document.addEventListener('keydown', e => {
  if (e.ctrlKey && e.shiftKey && e.key === 'F') { e.preventDefault(); toggleGlobalSearch(); }
  if (e.ctrlKey && e.key === 'b') { e.preventDefault(); document.getElementById('sidebar-panel')?.classList.toggle('collapsed'); }
  if (e.ctrlKey && e.key === '\\') { e.preventDefault(); toggleSplitView(); }
  if (e.ctrlKey && e.shiftKey && e.key === 'Z') { e.preventDefault(); toggleZenMode(); }
  if (e.key === 'Escape' && window.zenMode) { toggleZenMode(); }
  if (e.key === 'Escape' && window.globalSearchOpen) { closeGlobalSearch(); }
  if (e.key === 'Escape' && window.notifCenterOpen) { closeNotifCenter(); }
});

window.render = render;
window.renderContent = renderContent;

window.addEventListener('online', function() {
  var banner = document.getElementById('offline-banner');
  if (banner) banner.remove();
  toast(_L ? _L('온라인 복구', 'Back online') : 'Back online');
  loadAll();
});
window.addEventListener('offline', function() {
  if (document.getElementById('offline-banner')) return;
  var banner = document.createElement('div');
  banner.id = 'offline-banner';
  banner.style.cssText = 'position:fixed;top:0;left:0;right:0;z-index:99999;background:var(--red);color:#fff;text-align:center;padding:6px;font-size:12px;font-weight:700';
  banner.textContent = (_L ? _L('오프라인 — 네트워크 연결을 확인하세요', 'Offline — Check network connection') : 'Offline');
  document.body.prepend(banner);
});

restoreSession();

if (typeof registerShortcut === 'function') {
  registerShortcut('/', function(){ if (typeof toggleGlobalSearch === 'function') toggleGlobalSearch(); }, '글로벌 검색');
  registerShortcut('n', function(){ if (typeof showCreate === 'function') showCreate(); }, '새 VM');
  registerShortcut('?', function(){ if (typeof showShortcutsHelp === 'function') showShortcutsHelp(); else showHelp && showHelp(); }, '단축키 도움말');
  registerShortcut('g', function(){ navigateTo('dashboard'); }, '대시보드 이동');
  registerShortcut('m', function(){ navigateTo('mon-overview'); }, '모니터링');
}
function showShortcutsHelp() {
  if (typeof listShortcuts !== 'function' || typeof showModal !== 'function') return;
  var el = PCV.uxlib.el;
  var sc = listShortcuts();
  var rows = Object.keys(sc).map(function(k){
    return el('tr', null,
      el('td', null, el('span', { class: 'kbd' }, k)),
      el('td', null, sc[k].label || ''));
  });
  showModal([
    el('h2', null, '⌨ 단축키 도움말'),
    el('table', null,
      el('thead', null, el('tr', null, el('th', null, '키'), el('th', null, '동작'))),
      el('tbody', null, rows)),
    el('div', { style: 'text-align:right;margin-top:12px' },
      el('button', { class: 'btn', onclick: 'closeModal()' }, '닫기'))
  ]);
}
window.showShortcutsHelp = showShortcutsHelp;
