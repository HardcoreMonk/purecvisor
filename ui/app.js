/* ═══════════════════════════════════════════════════════════════
   PureCVisor — app.js (Entry Point)
   Modular Web UI Dashboard
   Modules: api.js, ui.js (Phase 1 분리 완료)
   ═══════════════════════════════════════════════════════════════ */

/*
 * ===== app.js 모듈 개요 (주니어 개발자 필독) =====
 *
 * [역할]
 *   Web UI의 진입점(entry point). index.html의 <script> 순서에서
 *   api.js, ui.js 다음에 로드된다. 다른 모듈이 IIFE 안에서 PCV.*에
 *   등록한 함수를 이 파일이 **글로벌(window) 변수**로 묶어 최종 결합한다.
 *
 * [PCV 네임스페이스 전략 (ADR-0013)]
 *   - 각 모듈은 (function(PCV){ ... })(window.PCV) 안에서 PCV.api, PCV.ui,
 *     PCV.vm 등에 함수를 등록한다.
 *   - app.js는 IIFE를 쓰지 않는다. var 선언은 자동으로 window.*가 된다.
 *     이것이 의도적이다 — 다른 모듈과 HTML onclick에서 직접 참조해야 하므로.
 *   - PCV.state는 Object.defineProperty getter로 정의되어, 호출할 때마다
 *     최신 vmList/selectedVmIndex를 반환한다 (복사본이 아닌 라이브 참조).
 *   - PCV.config는 빌드 시점의 정적 값이다 (REST_COUNT 등). /health에서
 *     동적으로 갱신되지 않는다.
 *
 * [글로벌 상태 변수]
 *   - vmList: VM 목록 배열. loadAll()이 10초마다 갱신한다.
 *   - selectedVmIndex: 현재 선택된 VM의 vmList 인덱스.
 *   - currentTab: 현재 표시 중인 탭 ('dashboard', 'summary', 'console' 등).
 *   - cpuHistory/memHistory: 60초 링 버퍼. renderPerformance()가 그래프용으로 사용.
 *   - window.authToken: JWT 토큰. sessionStorage에도 동기 저장된다.
 *     **왜 window에?** — api.js, vm.js 등 모든 모듈이 fetch 헤더에 사용해야
 *     하므로, 네임스페이스가 아닌 window에 둔다.
 *
 * [주요 함수]
 *   - loadAll(skipContent): VM 목록 fetch + render. skipContent=true면 폴링.
 *   - renderDashboard(b): 대시보드 홈 화면 렌더링.
 *   - applyEditionCapabilities(): /health에서 cluster 지원 여부 판별.
 *   - pcvPostLoginInit(): 로그인 후 RBAC role 가시성 + hash 라우팅 적용.
 *
 * [흔한 실수]
 *   - var 대신 let/const를 쓰면 window에 등록되지 않아 다른 모듈에서 참조 불가.
 *   - selectedVmIndex를 변경한 뒤 render()를 호출하지 않으면 사이드바가 갱신 안 됨.
 *   - loadAll 내부의 cachedFetch는 skipContent=true(폴링)일 때만 작동한다.
 *     명시 호출 시 항상 fresh fetch를 한다.
 *   - HTML onclick="..." 안의 문자열에 사용자 입력을 넣을 때 반드시
 *     escapeAttr()를 사용하라 (escapeHtml 아님). 차이는 ui.js 주석 참조.
 */

/* ═══ MODULE: api.js, ui.js 는 index.html에서 먼저 로드됨 ═══ */

/* ═══ STATE VARIABLES (var = window 글로벌 스코프) ═══
 *  var로 선언하는 이유: 번들러 없이 <script>로 로드하므로 var = window 속성이 됨.
 *  let/const로 바꾸면 다른 파일에서 참조 불가 — 절대 변경 금지. */
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

/* ═══ PCV NAMESPACE (structured state access) ═══ */
/* ADR-0013: merge into existing PCV namespace (modules add PCV.api, PCV.ui, PCV.vm)
 *
 * Object.assign을 쓰지 않는 이유:
 *   api.js 등이 이미 window.PCV = window.PCV || {} 로 초기화한 뒤
 *   PCV.api = {...}를 등록해둔 상태. 여기서 Object.assign(window, {PCV: ...})하면
 *   기존 PCV.api가 덮어써진다. 따라서 PCV 객체를 새로 만들지 않고,
 *   기존 객체에 .state, .config, .auth 속성만 추가한다.
 *
 * PCV.state가 getter인 이유:
 *   vmList 등은 var 선언이라 값이 계속 바뀐다. 일반 프로퍼티로 복사하면
 *   스냅샷이 되어 최신값을 반영하지 못한다. getter는 호출 시점의 라이브 값을 반환.
 */
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

/* Functions removed — provided by modules/*.js (api.js, ui.js, vm.js, container.js, network.js, storage.js, cluster.js, monitor.js, cloud.js, nav.js, help.js) */

/* ═══ CLEANUP ON TAB CLOSE ═══ */
window.addEventListener('beforeunload', (e) => {
  /* 로그인 상태에서 대시보드 이탈 시 경고 팝업 */
  if (authToken) {
    e.preventDefault();
    e.returnValue = '';
  }
  authToken = '';
  if (wsConnection) wsConnection.close();
});

/* ═══ EDITION CAPABILITY DETECTION ═══ */
/**
 * /health 응답의 capabilities.cluster를 확인하여
 * Single Edge에서 클러스터 UI 요소를 숨김.
 * 로그인 성공 후 1회 호출.
 */
/* Single Edge(capabilities.cluster=false)에서 숨길 페이지 네비게이션 키.
   마크업 수정 없이 중앙 리스트로 관리하여 신규 cluster 의존 페이지 추가 시
   이 배열만 갱신하면 된다. data-nav 속성과 onclick="navigateTo('...')" 양쪽 커버. */
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
      /* data-nav/onclick 기반 cluster 전용 메뉴 자동 hide (사이드바·팔레트 공통) */
      PCV_CLUSTER_ONLY_NAV.forEach(function(nav) {
        var sel = '[data-nav="' + nav + '"],[onclick*="navigateTo(\'' + nav + '\')"]';
        document.querySelectorAll(sel).forEach(function(el) {
          el.style.display = 'none';
        });
      });
      /* 싱글 엣지: 기본 사이드바를 VM 탭으로 */
      var clusterTab = document.querySelector('[data-sb="cluster"]');
      if (clusterTab && clusterTab.classList.contains('active')) {
        switchSbTab('vms');
      }
    }
  }).catch(function() {});
}
/* 페이지 로드 시 즉시 실행 (인증 불필요 엔드포인트) */
applyEditionCapabilities();

/* ═══ HTML BUILDER UTILITY (G-2, FE-6) ═══
 * 문자열 HTML 빌더 H 는 app.js 내 마지막 소비부(showConnect/showAbout/
 * showPrefs 등)가 8차 배치에서 HN 노드 빌더로 전환되며 미사용이 되어 제거.
 * 노드 컴포넌트가 필요하면 ui.js 의 전역 HN.* 사용 (ADR-013). */

/* ═══ UTILITIES ═══ */
var esc = escapeHtml;

function ciIcon(name) {
  return '<svg class="ci-icon" aria-hidden="true"><use href="/ui/vendor/coolicons/coolicons.svg#ci-' + name + '"></use></svg>';
}

/* ADR-013 DOM-safe: ciIcon()의 SVG 노드 등가물. el()은 HTML NS createElement라
 * SVG가 실제 렌더되지 않고 href/viewBox 등이 소문자화된다 → createElementNS 로컬
 * 헬퍼 (monitor.js _svgEl/_svgIcon 선례). 함수 선언이라 로그인 IIFE(로드시)에 hoist. */
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
/* renderProgressBar(ui.js 문자열 헬퍼, 수정 금지)의 노드 등가물 — class/구조 동형. */
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

/* ═══ EVENT LOG POPOUT WINDOW ═══ */
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

/* ═══ VM FAVORITES (G-4) ═══ */

/* ═══ CUSTOM CONFIRM DIALOG (G-4) ═══ */

/* ═══ SKELETON LOADING ═══ */

/* ═══ SORTABLE TABLE UTILITY ═══ */

/* ═══ ERROR HANDLING WRAPPER ═══ */

/* ═══ LOGIN ═══ */
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

/* ═══ API HELPERS ═══ */

/* ═══ WEBSOCKET ═══ */

/* THEME — modules/theme.js로 이관됨 */

window.addEventListener('DOMContentLoaded', () => {
  /* 테마는 index.html의 inline head script에서 이미 sanitize + 적용됨.
     여기서는 URL 파라미터 override와 select UI 동기화만 담당.
     Supanova 변형만 허용. */
  const ALLOWED = ['supanova', 'supanova-cyan', 'supanova-hicontrast'];
  const urlTheme = new URLSearchParams(window.location.search).get('theme');
  let t = urlTheme || localStorage.getItem('pcv-theme') || 'supanova';
  if (ALLOWED.indexOf(t) < 0) t = 'supanova';
  document.documentElement.setAttribute('data-theme', t);
  localStorage.setItem('pcv-theme', t);
  const s = document.getElementById('theme-select');
  if (s) s.value = t;
});

/* Session restore — api.js restoreSession()으로 이관됨 (파일 끝에서 호출) */

/* ═══ SIDEBAR ═══ */

var ctrSortKey = 'name', ctrSortDir = 1;

window.setCtrSort = setCtrSort;

window.toggleInfraSort = toggleInfraSort;

/* ═══ DRAG AND DROP NAV REORDER ═══ */
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

/* ═══ SORT / FILTER / RENDER ═══ */

/* ═══ CONTEXT MENU ═══ */
document.addEventListener('click', () => { document.getElementById('ctx').style.display = 'none'; });

/* ═══ CONTENT TABS ═══ */
document.getElementById('ct').addEventListener('click', e => {
  if (e.target.tagName === 'BUTTON') {
    document.querySelectorAll('#ct button').forEach(b => b.classList.remove('active'));
    e.target.classList.add('active');
    currentTab = e.target.dataset.t;
    renderContent();
  }
});

/* navigateTo + renderContent — modules/nav.js로 이관됨 */

/* ═══ VM SUMMARY ═══ */

/* ═══ CONSOLE / VNC ═══ */

/* ═══ SNAPSHOTS ═══ */

window.snapNameValidate = snapNameValidate;
window.snapCreateExec = snapCreateExec;

window.rbValidate = rbValidate;
window.rbExec = rbExec;

window.snapDeleteAll = snapDeleteAll;
window.sdaPreview = sdaPreview;
window.sdaExec = sdaExec;

/* ═══ PERFORMANCE ═══ */

/* ═══ NETWORKS ═══ */

/* ═══ STORAGE ═══ */

/* ═══ CONTAINERS ═══ */
var selCtr = null, ctrTab = 'summary', ctrHist = [];

/* ═══ CONTAINER TAB RENDERING ═══ */

/* ═══ CONTAINER ACTIONS ═══ */

window.ctrDistChanged = ctrDistChanged;

window.ctrIpModeChanged = ctrIpModeChanged;

window.ctrLoadBridges = ctrLoadBridges;

/* ═══ HOST ═══ */

/* ═══ CLUSTER ═══ */

/* FE-6: addAffinityRule — cluster.js에서 정의, 중복 제거 */
/* window.addAffinityRule은 cluster.js에서 등록됨 */

/* ═══ OVN ═══ */

/* ═══ POWER / VM DELETE ═══ */
/* Keep global alias */
window.pw = vmPower;

/* ═══ MODALS ═══ */
/* Keep global aliases */
window.showM = showModal;
window.closeM = closeModal;

/* ═══ VM CREATE WIZARD ═══ */

/* ═══ SETTINGS ═══ */

/* ═══ SNAPSHOT SHORTCUT ═══ */

/* ═══ NIC MANAGER ═══ */

/* ═══ VNC MODAL ═══ */

/* ═══ NETWORK CREATE / EDIT ═══ */

/* ═══ ZVOL ═══ */

/* ═══ CONNECT / PREFS / ABOUT ═══ */
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
    /* Auto Theme 토글 제거 — pure-light/pure-dark 테마 삭제와 함께 무의미해짐 */
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
  /* /health에서 동적 데이터 로드 */
  fetchGet(API_BASE + '/health').then(r => {
    var d = unwrapData(r);
    var el = document.getElementById('about-ver');
    /* ADR-013 DOM-safe 전환(#5): 서버 응답(d.version/d.status)을 innerHTML
     * 문자열 결합 대신 PCV.uxlib.el/frag로 조립 — createTextNode만 쓰이므로
     * esc() 없이도 HTML 파싱 경로 자체가 없다. */
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

/* ACCOUNTS/AGENT — modules/accounts.js로 이관됨 */

/* ═══ ACCOUNTS — modules/accounts.js로 이관됨 ═══ */

/* ═══ MONITORING ═══ */

/* G-2: Promise.all parallel fetch */

/* fmtBytes — modules/monitor.js로 이관됨 */
/* fmtRate — modules/monitor.js로 이관됨 */
/* fmtUptime — modules/monitor.js로 이관됨 */

/* ═══ MONITORING RENDER — G-2 Split into sub-functions ═══ */

/* ═══ ALERTS ═══ */

window.alertSave = async function() {
  const cfg = { enabled: document.getElementById('al-enabled')?.checked || false, cpu_warn: parseInt(document.getElementById('al-cpu_warn')?.value || 80), cpu_crit: parseInt(document.getElementById('al-cpu_crit')?.value || 95), mem_warn: parseInt(document.getElementById('al-mem_warn')?.value || 85), mem_crit: parseInt(document.getElementById('al-mem_crit')?.value || 95), disk_warn: parseInt(document.getElementById('al-disk_warn')?.value || 80), disk_crit: parseInt(document.getElementById('al-disk_crit')?.value || 90), eval_period: parseInt(document.getElementById('al-eval_period')?.value || 30), webhook_url: document.getElementById('al-webhook_url')?.value || '', webhook_format: document.getElementById('al-webhook_format')?.value || 'generic', telegram_chat_id: document.getElementById('al-telegram_chat_id')?.value || '' };
  try { await fetchPut(API_BASE + '/alerts/config', cfg);
    const st = document.getElementById('al-status'); if (st) { st.textContent = t('alert.saved'); st.style.color = 'var(--green)'; setTimeout(() => { st.textContent = ''; }, 2000); }
    setTimeout(() => renderContent(), 500);
  } catch (e) { const st = document.getElementById('al-status'); if (st) { st.textContent = t('error') + ': ' + e.message; st.style.color = 'var(--red)'; } }
};

/* ═══ HA OPERATIONS ═══ */

/* ═══ SECURITY GROUPS ═══ */

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

/* ═══ GPU MONITORING ═══ */

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

/* ═══ AUDIT LOG SEARCH ═══ */

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

/* ═══ WEBHOOK DLQ ═══ */
window.loadWebhookDlq = async function() {
  var el = document.getElementById('dlq-list');
  if (!el) return;
  PCV.uxlib.setMsg(el, 'loading', null, 'DLQ 조회 중...');
  try {
    /* REST 우선 시도, 실패 시 RPC 폴백 */
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
    /* DLQ 항목 저장 (개별 재시도용) */
    window._dlqItems = items;
  } catch (e) {
    PCV.uxlib.setMsg(el, 'warn', { tag: 'div', cls: 'stat-label' }, _L('DLQ 조회 불가', 'DLQ unavailable'));
  }
};

window.retryWebhookDlq = async function() {
  const el = document.getElementById('dlq-list');
  if (el) PCV.uxlib.setMsg(el, 'loading', null, '재시도 중...');
  try {
    await fetchPost(API_BASE + '/alerts/dlq/retry', {});
    toast('DLQ 전체 재시도 요청 완료');
    if (el) PCV.uxlib.setMsg(el, 'ok', { tag: 'p', size: '12px' }, '재시도 요청 전송 완료');
  } catch (e) {
    toast('DLQ 재시도 실패: ' + e.message, false);
    if (el) PCV.uxlib.setMsg(el, 'warn', { size: '12px' }, 'DLQ 재시도 엔드포인트 미구현');
  }
};

window.retryDlqItem = async function(index) {
  var items = window._dlqItems || [];
  if (index < 0 || index >= items.length) return;
  var item = items[index];
  try {
    await fetchPost(API_BASE + '/rpc', {jsonrpc:'2.0', method:'alert.dlq.retry', params:{index: index, url: item.url || item.webhook_url || ''}, id:'dlqr1'});
    toast(_L('재시도 요청 전송됨', 'Retry requested'));
    window.loadWebhookDlq();
  } catch (e) {
    toast(_L('재시도 실패', 'Retry failed') + ': ' + e.message, false);
  }
};

/* ═══ HELP/GUIDE/SWAGGER — modules/help.js로 이관됨 ═══ */

/* ═══ API MANAGEMENT — modules/accounts.js로 이관됨 ═══ */

/* ═══ API KEY MANAGEMENT ═══ */
async function apiKeyCreate() {
  var desc = (document.getElementById('apikey-desc')?.value || '').trim();
  var expiry = parseInt(document.getElementById('apikey-expiry')?.value) || 90;
  if (!desc) { toast('Description required', false); return; }
  try {
    var r = await fetchPost(API_BASE + '/auth/apikeys', { description: desc, expiry_days: expiry });
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
        mk('button', { class: 'btn', style: 'margin-top:6px;font-size:10px', onclick: "navigator.clipboard.writeText('" + escapeHtml(d.api_key).replace(/'/g, "\\'") + "');toast('Copied!')" }, '📋 Copy')
      ));
    }
    toast('API key created: ' + desc);
    addEvt('API Key created: ' + desc);
    document.getElementById('apikey-desc').value = '';
    apiKeyList();
  } catch (e) { toast('Error: ' + e.message, false); }
}
window.apiKeyCreate = apiKeyCreate;

async function apiKeyList() {
  var el = document.getElementById('apikey-list'); if (!el) return;
  try {
    var r = await fetchGet(API_BASE + '/auth/apikeys');
    var keys = Array.isArray(r) ? r : (r.data || r.result || []);
    if (!Array.isArray(keys) || keys.length === 0) {
      PCV.uxlib.setMsg(el, null, { tag: 'p', cls: 'color-muted', size: '12px' }, 'No API keys. Create one above.');
      return;
    }
    var mk = PCV.uxlib.el;
    var rows = keys.map(function(k) {
      var keyMasked = k.key_prefix ? k.key_prefix + '...' : (k.api_key ? k.api_key.substring(0, 8) + '...' : '***...');
      var expired = k.expired || (k.expires_at && new Date(k.expires_at) < new Date());
      var statusBadge = expired ? HN.badge('Expired', 'r') : (k.revoked ? HN.badge('Revoked', 'r') : HN.badge('Active', 'g'));
      var actionCell = (!k.revoked && !expired)
        ? mk('button', { class: 'btn btn-r', style: 'font-size:9px;padding:2px 8px', onclick: "apiKeyRevoke('" + escapeHtml(k.id || k.key_id || '') + "','" + escapeHtml(k.description || '') + "')" }, 'Revoke')
        : null;
      return mk('tr', null,
        mk('td', null, mk('b', null, k.description || '-')),
        mk('td', null, mk('code', { class: 'color-muted' }, keyMasked)),
        mk('td', { class: 'text-xs' }, k.created_at || k.created || '-'),
        mk('td', { class: 'text-xs' }, k.expires_at || k.expires || '-'),
        mk('td', null, statusBadge),
        mk('td', null, actionCell));
    });
    var table = mk('table', { style: 'font-size:11px' },
      mk('thead', null, mk('tr', null,
        mk('th', null, 'Description'), mk('th', null, 'Key (masked)'), mk('th', null, 'Created'),
        mk('th', null, 'Expires'), mk('th', null, 'Status'), mk('th'))),
      mk('tbody', null, rows));
    PCV.uxlib.clearEl(el); el.appendChild(table);
  } catch (e) { PCV.uxlib.setMsg(el, null, { tag: 'p', cls: 'color-muted', size: '12px' }, 'API Keys not available: ' + e.message); }
}
window.apiKeyList = apiKeyList;

async function apiKeyRevoke(keyId, desc) {
  if (!await customConfirm('Revoke API Key', 'Revoke key "' + desc + '"? This cannot be undone.')) return;
  try {
    var r = await fetchDelete(API_BASE + '/auth/apikeys/' + encodeURIComponent(keyId));
    if (r.error) { toast('Revoke failed: ' + (r.error.message || ''), false); return; }
    toast('API key revoked: ' + desc);
    addEvt('API Key revoked: ' + desc);
    apiKeyList();
  } catch (e) { toast('Error: ' + e.message, false); }
}
window.apiKeyRevoke = apiKeyRevoke;

/* ═══ DASHBOARD HOME ═══ */
async function renderDashboard(b) {
  showSkeleton(b);
  /* 호스트 메트릭 즉시 수집 */
  await collectHostMetrics();
  try {
    var vms = [], ctrs = [], clusterData = {}, alertData = [];
    /* 빠른 API(VM/컨테이너)를 먼저 로드하여 즉시 렌더링,
       느린 API(클러스터/알림)는 비동기로 나중에 갱신 */
    var fastResults = await Promise.all([
      fetchGet(API_BASE + '/vms').catch(function() { return { data: [] }; }),
      fetchGet(API_BASE + '/containers').catch(function() { return { data: [] }; })
    ]);
    vms = Array.isArray(fastResults[0]) ? fastResults[0] : (fastResults[0].data || []);
    ctrs = Array.isArray(fastResults[1]) ? fastResults[1] : (fastResults[1].data || []);
    /* 클러스터: /health에서 cluster 정보 추출 (인증 불필요, 즉시 응답)
     * /cluster/status는 REST 스레드 블로킹 시 3초 타임아웃 → standalone 표시 방지 */
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

    /* F6: Widget toggle bar */
    parts.push(mk('div', { class: 'ops-section-heading' },
      mk('div', null,
        mk('h3', null, _L('표시 항목', 'Visible sections')),
        mk('p', null, _L('대시보드에서 바로 보고 싶은 카드만 켜 두십시오.', 'Keep only the sections you want to see on the dashboard.')))));
    /* 아이콘은 coolicons SVG(ci-*)로 통일 — 이모지 혼용(컬러/모노크롬 글리프,
     * OS 폰트 의존)이 '제각각' 인상의 원인이라 사이드바와 같은 시스템 사용.
     * 크기는 컨테이너 font-size 위계(칩 12px/카드 11px/타일 30px) 상속. */
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

    /* 상태 카드 */
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

    /* 바로가기 그리드 */
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

    /* 호스트 메트릭 차트 */
    if (_dashWidgets.charts !== false) {
      /* 호스트 메트릭 최신값 표시 */
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

    /* 최근 알림 */
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

    /* VM 목록 요약 */
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

    /* 컨테이너 목록 */
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

    /* Initialize dashboard charts */
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

/* F6: Dashboard widget toggle */
function toggleDashWidget(key) {
  _dashWidgets[key] = !(_dashWidgets[key] !== false);
  localStorage.setItem('pcv-dash-widgets', JSON.stringify(_dashWidgets));
  renderDashboard(document.getElementById('cb'));
}
window.toggleDashWidget = toggleDashWidget;

/* ═══ LOAD ALL ═══
 * skipContent=true: 10초 폴링 (캐시 OK, dedup 효과)
 * skipContent=false (또는 미지정): 명시 호출 — 항상 fresh, 캐시 무효화
 *
 * [왜 이 분기가 중요한가]
 *   10초 setInterval에서 호출할 때는 skipContent=true를 넘긴다.
 *   cachedFetch(uxlib.js)가 500ms TTL 동안 동일 요청을 병합(coalescing)하여
 *   WS 이벤트와 폴링이 동시에 발생해도 중복 fetch를 방지한다.
 *   사용자가 명시적으로 loadAll()을 호출하면 캐시를 무효화하여
 *   항상 최신 데이터를 가져온다.
 *
 *   render(skipContent)에서도 skipContent=true면 vmList 해시가
 *   변하지 않았을 때 DOM 업데이트를 건너뛴다 — 깜박임 방지. */
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

/* #14 hash routing 초기 적용 + #15 role 가시성 + 사용자 정보 캐시 */
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
/* 프론트 #4-A: 비가시 탭 폴링 중단 — document.hidden이면 콜백 진입부에서 스킵 */
setInterval(() => { if (document.hidden) return; if (authToken) loadAll(true); }, 10e3);

/* ═══ HOST METRICS COLLECTION ═══ */
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
  } catch (e) { /* metrics endpoint may be unavailable */ }
}
setInterval(() => { if (document.hidden) return; collectHostMetrics(); }, 5000);
/* G-4: auto-refresh indicator update — 네트워크 호출 없이 로컬 텍스트만 갱신하지만
   비가시 시 불필요한 DOM 작업이라 동일하게 스킵 */
setInterval(() => { if (document.hidden) return; const sb3 = document.getElementById('sb3'); if (sb3 && authToken) { const elapsed = Math.round((Date.now() - lastLoadTime) / 1000); sb3.textContent = 'Updated ' + elapsed + 's ago'; } }, 1000);

/* 탭 가시 복귀 시 loadAll·collectHostMetrics·updateStatusBar를 즉시 1회 실행해
   백그라운드 동안 밀린 화면을 따라잡는다 (clock은 다음 tick으로 충분 — 별도 처리 불필요) */
document.addEventListener('visibilitychange', () => {
  if (document.hidden) return;
  if (authToken) loadAll(true);
  collectHostMetrics();
  if (typeof updateStatusBar === 'function') updateStatusBar();
});

/* ═══ KEYBOARD SHORTCUTS ═══ */
document.addEventListener('keydown', e => {
  if (e.key === 'F11') { e.preventDefault(); toggleFS(); }
  if (e.ctrlKey && e.key === 'n') { e.preventDefault(); showCreate(); }
  if (e.ctrlKey && e.key === 'd') { e.preventDefault(); showSettings(); }
  if (e.ctrlKey && e.key === 'p') { e.preventDefault(); showPrefs(); }
  /* G-4: Command palette */
  if (e.ctrlKey && e.key === 'k') { e.preventDefault(); if (cmdPaletteOpen) closeCmdPalette(); else openCmdPalette(); }
  /* F-2: Escape to close modals */
  if (e.key === 'Escape') {
    if (cmdPaletteOpen) { closeCmdPalette(); e.preventDefault(); return; }
    const mbg = document.getElementById('mbg');
    if (mbg && !mbg.classList.contains('hidden')) { closeModal(); e.preventDefault(); }
    const iso = document.getElementById('iso-overlay');
    if (iso) { closeISOBrowser(); e.preventDefault(); }
  }
  /* F-2: Tab focus trapping in modal */
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

/* ═══ MOBILE ═══ */

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

/* ═══ COMMAND PALETTE (Ctrl+K) — G-4 ═══ */

/* ═══ KEYBOARD HELP OVERLAY (? key) — I-1/E-6 ═══ */

/* ═══ NOTIFICATION SOUND (Web Audio API) — I-1/E-3 ═══ */
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
  } catch (e) { /* Audio not supported */ }
}

/* ═══ BROWSER NOTIFICATIONS — I-1/E-4 ═══ */
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
  catch (e) { /* SW required on some browsers */ }
}
/* Request permission on first login */
var _origDoLoginPage = typeof doLoginPage === 'function' ? doLoginPage : null;

/* ═══ SERVICE WORKER (Network-First caching) ═══ */
if ('serviceWorker' in navigator) {
  window.addEventListener('load', () => {
    navigator.serviceWorker.register('/ui/sw.js', { updateViaCache: 'none' }).then(reg => {
      /* 새 SW가 waiting 상태이면 즉시 활성화 */
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
    /* SW 컨트롤러가 교체되면 페이지 리로드 */
    navigator.serviceWorker.addEventListener('controllerchange', () => {
      window.location.reload();
    });
  });
}

/* ═══ KEYBOARD SHORTCUTS EXTENSION — I-1 ═══ */
/* Add ? key handler to existing keydown listener */
document.addEventListener('keydown', e => {
  if (e.key === '?' && !e.ctrlKey && !e.altKey && !e.metaKey) {
    const tag = document.activeElement?.tagName;
    if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return;
    e.preventDefault();
    toggleKbdHelp();
  }
  if (e.key === 'Escape' && kbdHelpOpen) { e.preventDefault(); closeKbdHelp(); }
});

/* Request browser notification permission after login */
requestBrowserNotif();

/* ═══════════════════════════════════════════════════════════════
   A+B: 26개 미반영 RPC Web UI 반영
   Priority A: 실용적 (15개) + Priority B: 고급 기능 (11개)
   ═══════════════════════════════════════════════════════════════ */

/* A-1~B-EXTRA, LXC NIC, CLOUD MIGRATION 핸들러 — 전부 modules/*.js로 이관됨
   (vm/storage/container/cluster/network/cloud/advanced 모듈에서 window 등록) */

/* ═══ COMMAND PALETTE: Add new actions ═══ */
CMD_ACTIONS.push(
  { icon: '&#128195;', label: 'Templates', action: () => navigateTo('templates') },
  { icon: '&#9881;', label: 'Config Management', action: () => navigateTo('config-mgmt') },
  /* Docker/OCI 제거됨 */
  /* Terraform 제거됨 */
  /* Federation — 멀티 에디션 전용, cluster-only로 런타임 제어 */
  { icon: '&#128230;', label: 'Import OVA', action: () => showImportOva() },
  { icon: '&#9729;', label: 'Cloud Migration', action: () => navigateTo('cloud-migration') },
  { icon: '&#128269;', label: 'Global Search', hint: 'Ctrl+Shift+F', action: () => toggleGlobalSearch() },
  { icon: '&#9647;', label: 'Toggle Split View', hint: 'Ctrl+\\', action: () => toggleSplitView() },
  { icon: '&#128276;', label: 'Notifications', action: () => toggleNotifCenter() },
  { icon: '&#127748;', label: 'Zen Mode', hint: 'Ctrl+Shift+Z', action: () => toggleZenMode() },
  { icon: '&#9881;', label: 'Toggle Bottom Panel', action: () => toggleBottomPanel() }
);

/* ═══ V-1: ACTIVITY BAR ═══ */

/* ═══ V-2: EDITOR TABS ═══ */

/* Hook into navigateTo to auto-open tabs — BUG-9 fix: 재귀 가드 + 명시적 원본 참조 */
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
        'security-groups': '&#128737;', 'gpu': '&#127918;', 'docker': '&#128051;',
        'terraform': '&#127981;', 'federation': '&#127758;', 'apihelp': '&#128214;',
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

/* ═══ V-3: BOTTOM PANEL ═══ */

/* Panel resize drag */
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

/* Redirect event log to bottom panel — BUG-5 fix: 이중 래핑 방어 */
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

/* ═══ V-4: BREADCRUMBS ═══ */

/* ═══ V-5: GLOBAL SEARCH (Ctrl+Shift+F) ═══ */

/* ═══ V-6: SPLIT VIEW ═══ */
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
    /* Render current page in left, monitoring in right */
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

/* ═══ V-7: HOVER INFO ═══ */
/* hoverCard already created and appended in modules/nav.js (lines 697-700) */

/* ═══ V-8: NOTIFICATIONS CENTER ═══ */

/* updateNotifBadge — modules/nav.js로 이관됨 */

/* Hook toast to also create notifications
 * NOTE: original toast() in ui.js already calls addNotification(),
 * so we must NOT call it again here — just delegate to _origToast. */

/* ═══ V-9: ZEN MODE ═══ */

/* Keyboard: Ctrl+Shift+F for search, Ctrl+B toggle sidebar, Ctrl+\ split, Ctrl+Shift+Z zen */
document.addEventListener('keydown', e => {
  if (e.ctrlKey && e.shiftKey && e.key === 'F') { e.preventDefault(); toggleGlobalSearch(); }
  if (e.ctrlKey && e.key === 'b') { e.preventDefault(); document.getElementById('sidebar-panel')?.classList.toggle('collapsed'); }
  if (e.ctrlKey && e.key === '\\') { e.preventDefault(); toggleSplitView(); }
  if (e.ctrlKey && e.shiftKey && e.key === 'Z') { e.preventDefault(); toggleZenMode(); }
  if (e.key === 'Escape' && window.zenMode) { toggleZenMode(); }
  if (e.key === 'Escape' && window.globalSearchOpen) { closeGlobalSearch(); }
  if (e.key === 'Escape' && window.notifCenterOpen) { closeNotifCenter(); }
});

/* ═══ GLOBAL ALIASES (for onclick in HTML) ═══ */
window.render = render;
window.renderContent = renderContent;

/* ═══ OFFLINE DETECTION ═══ */
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

/* Session restore (api.js에서 제공) */
restoreSession();

/* #6 기본 단축키 등록 */
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
