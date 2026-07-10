/* ═══════════════════════════════════════════════════════════════
   PureCVisor — modules/nav.js
   Navigation, Sidebar, Activity Bar, Command Palette, Mobile,
   Keyboard Shortcuts, Editor Tabs, Breadcrumbs, Global Search,
   Zen Mode, Notification Center, Hover Cards, Bottom Panel
   Extracted from app.js — plain script, all functions on window.*
   ═══════════════════════════════════════════════════════════════ */

window.PCV = window.PCV || {};
(function(PCV) {

/* ═══ PINNED PAGES (J3) ═══ */
var _pinnedPages = JSON.parse(localStorage.getItem('pcv-pinned') || '[]');

function togglePin(pageId) {
  var idx = _pinnedPages.indexOf(pageId);
  if (idx >= 0) _pinnedPages.splice(idx, 1);
  else _pinnedPages.push(pageId);
  localStorage.setItem('pcv-pinned', JSON.stringify(_pinnedPages));
  renderPinnedBar();
}

function renderPinnedBar() {
  var el = document.getElementById('pinned-bar');
  if (!el) return;
  if (_pinnedPages.length === 0) { el.innerHTML = ''; el.style.display = 'none'; return; }
  el.style.display = 'block';
  var h = '<div style="padding:4px 8px;font-size:10px;color:var(--fg2);border-bottom:1px solid var(--border)">' + _L('고정됨', 'Pinned') + '</div>';
  _pinnedPages.forEach(function(p) {
    h += '<div class="vi" style="padding:4px 8px;font-size:11px" onclick="navigateTo(\'' + p + '\')"><span class="color-yellow" style="cursor:pointer;margin-right:4px" onclick="event.stopPropagation();togglePin(\'' + p + '\')">&#128204;</span>' + p + '</div>';
  });
  el.innerHTML = h;
}
window.togglePin = togglePin;
window.renderPinnedBar = renderPinnedBar;

/* ═══ NAVIGATION ═══ */
window._navGeneration = 0;
function navigateTo(n) {
  if (window.pcvClusterEnabled === false && window.PCV_CLUSTER_ONLY_NAV && window.PCV_CLUSTER_ONLY_NAV.includes(n)) {
    if (typeof toast === 'function') toast(_L('Single Edge 공개 리포에는 포함되지 않는 화면입니다', 'This screen is not included in Single Edge'), false);
    n = 'dashboard';
  }
  /* BUG-6 fix: 페이지 전환 시 generation 증가 → stale 비동기 콜백 차단 */
  window._navGeneration = (window._navGeneration || 0) + 1;
  /* FE-4: Cloud 폴 타이머 정리 (이전 페이지가 cloud-migration인 경우) */
  if (typeof _cloudCleanupTimer === 'function' && currentTab === 'cloud-migration' && n !== 'cloud-migration') {
    _cloudCleanupTimer();
  }
  /* FE-4: 모니터링 자동 갱신 정리 */
  if (typeof stopAdaptivePolling === 'function' && currentTab && currentTab.startsWith('mon-') && !(n && n.startsWith('mon-'))) {
    stopAdaptivePolling('mon-refresh');
  }
  /* P2-5: clear dirty form tracking on navigation (prevents false beforeunload) */
  if (typeof clearAllFormDirty === 'function') clearAllFormDirty();
  /* J1: Remember last VM detail tab */
  var vmTabs = ['summary', 'console', 'snapshots', 'performance', 'timeline'];
  if (vmTabs.includes(n)) localStorage.setItem('pcv-last-vm-tab', n);
  currentTab = n;
  document.querySelectorAll('#ct button').forEach(b => b.classList.remove('active'));
  const containerPages = ['containers', 'docker'];
  /* sb-cluster 패널은 존재하지 않음 — cluster 관련 페이지도 INFRA 사이드바에 노출 */
  const infraPages = PCV.filterEditionItems([
    { id: 'networks' }, { id: 'storage' }, { id: 'host' }, { id: 'ovn' },
    { id: 'accounts' }, { id: 'security-groups' }, { id: 'gpu' }, { id: 'templates' },
    { id: 'config-mgmt' }, { id: 'apimgmt' }, { id: 'apihelp' }, { id: 'helppage' },
    { id: 'serviceguide' }, { id: 'restguide' }, { id: 'mon-hosts' }, { id: 'mon-vms' },
    { id: 'mon-alerts' }, { id: 'mon-security' }, { id: 'mon-audit' }, { id: 'overlay' },
    { id: 'iscsi' }, { id: 'dpdk' }, { id: 'sriov' }, { id: 'heatmap' }, { id: 'api-perf' },
    { id: 'activity-log' }, { id: 'topology' }, { id: 'backup' },
    { id: 'ops-triage' },
    { id: 'mon-overview' }, { id: 'mon-storage' },
    { id: 'cloud-migration' }, { id: 'selfhealing' }
  ]).map(function(item) { return item.id; });
  if (containerPages.includes(n)) switchSbTab('containers');
  else if (infraPages.includes(n)) switchSbTab('infra');
  else { switchSbTab('vms'); }
  renderContent();
}
window.navigateTo = navigateTo;
/* Keep global alias */
window.go = navigateTo;

function pcvRoleAllows(minRole) {
  var roleRank = { viewer: 0, operator: 1, admin: 2 };
  var current = String((window.currentUser && window.currentUser.role) || '').toLowerCase();
  var required = String(minRole || 'viewer').toLowerCase();
  return (roleRank[current] ?? -1) >= (roleRank[required] ?? 0);
}
window.pcvRoleAllows = pcvRoleAllows;

/* ═══ SIDEBAR ═══ */
function switchSbTab(tab) {
  ['vms', 'containers', 'infra'].forEach(t => {
    const panel = document.getElementById('sb-' + t);
    if (panel) panel.classList.toggle('hidden', t !== tab);
  });
  document.querySelectorAll('#sb-tabs button').forEach(b => {
    b.classList.toggle('active', b.dataset.sb === tab);
  });
  const ct = document.getElementById('ct');
  if (ct) ct.style.display = (tab === 'vms') ? 'flex' : 'none';
  if (tab === 'containers') renderContainerList();
}
window.switchSbTab = switchSbTab;

/* ═══ CONTENT DISPATCH ═══ */
function renderContent() {
  destroyAllCharts();
  var cb = document.getElementById('cb');
  if (cb) cb.classList.add('fade-out');
  const b = cb, v = vmList[selectedVmIndex];
  /* BUG-6 fix: 비동기 렌더러의 stale 콜백이 DOM을 오염시키지 않도록 generation 캡처 */
  var gen = window._navGeneration || 0;
  try {
    var fn = (function() {
      var routes = {
      dashboard: () => renderDashboard(b),
      summary: () => renderSummary(b, v),
      console: () => renderConsole(b, v),
      snapshots: () => renderSnapshots(b, v),
      performance: () => renderPerformance(b, v),
      timeline: () => renderTimeline(b, v),
      networks: () => renderNetworks(b),
      storage: () => renderStorage(b),
      containers: () => renderContainers(b),
      host: () => renderHost(b),
      ovn: () => renderOvn(b),
      accounts: () => renderAccounts(b),
      apimgmt: () => renderApiManagement(b),
      'mon-overview': () => renderMonitoring(b, 'overview'),
      'ops-triage': () => renderOpsTriage(b),
      'mon-hosts': () => renderMonitoring(b, 'hosts'),
      'mon-vms': () => renderMonitoring(b, 'vms'),
      'mon-storage': () => renderMonitoring(b, 'storage'),
      'mon-alerts': () => renderAlerts(b),
      'mon-security': () => PCV.security.render(b),
      'mon-audit': () => renderAudit(b),
      'security-groups': () => renderSecGroups(b),
      'gpu': () => renderGpu(b),
      'templates': () => renderTemplates(b),
      'config-mgmt': () => renderConfigMgmt(b),
      'docker': () => renderDocker(b),
      'terraform': () => renderTerraform(b),
      'cloud-migration': () => renderCloudMigration(b),
      'overlay': () => renderOverlayNetworks(b),
      'iscsi': () => renderIscsi(b),
      'dpdk': () => renderDpdk(b),
      'sriov': () => renderSriov(b),
      'heatmap': () => renderHeatmap(b),
      'api-perf': () => renderApiPerf(b),
      'activity-log': () => renderActivityLog(b),
      'topology': () => renderTopology(b),
      'backup': () => renderBackup(b),
      apihelp: () => renderSwaggerApi(b),
      helppage: () => renderHelp(b),
      serviceguide: () => renderServiceGuide(b),
      restguide: () => renderRestGuide(b),
      /* 1.0: AI Self-Healing 별도 페이지 (Monitor Overview의 mount는 그대로 유지) */
      'selfhealing': () => renderSelfHealing(b)
      };
      return routes;
    })()[currentTab];
    if (fn) {
      var result = fn();
      if (result && typeof result.catch === 'function') {
        result.catch(function(err) {
          if ((window._navGeneration || 0) !== gen) return;
          if (b) b.innerHTML = '<div style="padding:40px;text-align:center"><div style="font-size:48px;margin-bottom:12px">&#9888;</div><h3 style="color:var(--red)">' + _L('렌더링 오류', 'Rendering Error') + '</h3><p class="color-muted" style="margin:12px 0">' + esc(err.message || '') + '</p><button class="btn" onclick="navigateTo(\'dashboard\')" style="margin-top:12px">' + _L('대시보드로', 'Go to Dashboard') + '</button></div>';
          if (_DEBUG) console.error('renderContent async error:', err);
        });
      }
    }
    else if (_DEBUG) console.warn('Unknown tab:', currentTab);
  } catch (renderErr) {
    if ((window._navGeneration || 0) !== gen) { /* stale page — ignore */ }
    else {
      b.innerHTML = '<div style="padding:40px;text-align:center"><div style="font-size:48px;margin-bottom:12px">&#9888;</div><h3 style="color:var(--red)">' + _L('렌더링 오류', 'Rendering Error') + '</h3><p class="color-muted" style="margin:12px 0">' + esc(renderErr.message || '') + '</p><pre style="background:var(--bg);padding:12px;border-radius:6px;font-size:11px;color:var(--red);text-align:left;max-height:200px;overflow:auto">' + esc(renderErr.stack || '') + '</pre><button class="btn" onclick="navigateTo(\'dashboard\')" style="margin-top:12px">' + _L('대시보드로', 'Go to Dashboard') + '</button></div>';
      if (_DEBUG) console.error('renderContent error:', renderErr);
    }
  }
  setTimeout(function() { if (cb) cb.classList.remove('fade-out'); }, 50);
  if (typeof updateStatusBar === 'function') updateStatusBar();
  if (typeof updateBreadcrumbs === 'function') updateBreadcrumbs();
}
window.renderContent = renderContent;

/* ═══ STATUS BAR CONTEXT ═══ */
function updateStatusBar() {
  var el = document.getElementById('sb-ctx');
  if (!el) return;
  var parts = [];
  if (vmList && vmList.length) {
    var running = vmList.filter(function(v) { return v.state === 'running'; }).length;
    parts.push('VM: ' + running + '/' + vmList.length);
  }
  parts.push(currentTab || 'dashboard');
  if (wsConnection && wsConnection.readyState === 1) parts.push('WS: Live');
  var elapsed = Math.round((Date.now() - (lastLoadTime || Date.now())) / 1000);
  parts.push('Sync: ' + elapsed + 's ago');
  if (window._perfMetrics) parts.push('API: ' + (_perfMetrics.avgApiTime || 0) + 'ms avg');
  /* Connection quality dot */
  var wsStat;
  if (typeof wsConnection !== 'undefined' && wsConnection && wsConnection.readyState === 1) {
    wsStat = '<span style="color:var(--green)">&#9679;</span>';
  } else if (typeof wsConnection !== 'undefined' && wsConnection && wsConnection.readyState === 0) {
    wsStat = '<span style="color:var(--yellow)">&#9679;</span>';
  } else {
    wsStat = '<span style="color:var(--red)">&#9679;</span>';
  }
  parts.unshift(wsStat);
  if (window._perfMetrics && _perfMetrics.avgApiTime > 0) {
    var latColor = _perfMetrics.avgApiTime < 100 ? 'var(--green)' : _perfMetrics.avgApiTime < 500 ? 'var(--yellow)' : 'var(--red)';
    parts.push('<span style="color:' + latColor + '">' + _perfMetrics.avgApiTime + 'ms</span>');
  }
  el.innerHTML = parts.join(' | ');
  if (typeof updateFavicon === 'function') updateFavicon();
}
window.updateStatusBar = updateStatusBar;
/* 프론트 #4-A: 비가시 탭 폴링 중단 — document.hidden이면 콜백 진입부에서 스킵 */
setInterval(() => { if (document.hidden) return; updateStatusBar(); }, 2000);

/* ═══ DYNAMIC FAVICON + TAB TITLE (N5) ═══ */
function updateFavicon() {
  var running = 0;
  if (vmList) running = vmList.filter(function(v) { return v.state === 'running'; }).length;

  /* Update tab title */
  document.title = 'PureCVisor' + (running > 0 ? ' (' + running + ' VMs)' : '');

  /* Generate dynamic favicon */
  var canvas = document.createElement('canvas');
  canvas.width = 32; canvas.height = 32;
  var ctx = canvas.getContext('2d');
  /* Background circle */
  var color = running > 0 ? '#00ff88' : (vmList && vmList.length > 0 ? '#ffee00' : '#ff2266');
  ctx.beginPath(); ctx.arc(16, 16, 14, 0, Math.PI * 2);
  ctx.fillStyle = color; ctx.fill();
  /* Text */
  ctx.fillStyle = '#000'; ctx.font = 'bold 16px monospace'; ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
  ctx.fillText(running > 0 ? running.toString() : '!', 16, 17);

  var link = document.getElementById('dynamic-favicon');
  if (!link) {
    link = document.createElement('link');
    link.id = 'dynamic-favicon';
    link.rel = 'icon';
    link.type = 'image/png';
    document.head.appendChild(link);
  }
  link.href = canvas.toDataURL('image/png');
}
window.updateFavicon = updateFavicon;

/* ═══ TOGGLE SIDEBAR ═══ */
function toggleSB() {
  const sb = document.getElementById('sidebar-panel') || document.getElementById('sidebar');
  if (sb) sb.classList.toggle('collapsed');
}
window.toggleSB = toggleSB;

/* ═══ TOGGLE FULLSCREEN ═══ */
function toggleFS() {
  document.fullscreenElement ? document.exitFullscreen() : document.documentElement.requestFullscreen();
}
window.toggleFS = toggleFS;

/* ═══ INFRA SORT ═══ */
var infraSortAsc = true;
window.infraSortAsc = infraSortAsc;

function toggleInfraSort() {
  infraSortAsc = !infraSortAsc;
  window.infraSortAsc = infraSortAsc;
  const container = document.getElementById('nav-infra');
  if (!container) return;
  const items = Array.from(container.querySelectorAll('.vi'));
  items.sort((a, b) => {
    const ta = (a.querySelector('.nm')?.textContent || '').toLowerCase();
    const tb = (b.querySelector('.nm')?.textContent || '').toLowerCase();
    return infraSortAsc ? ta.localeCompare(tb) : tb.localeCompare(ta);
  });
  items.forEach(el => container.appendChild(el));
  const btn = document.getElementById('infra-sort-btn');
  if (btn) btn.textContent = infraSortAsc ? 'A-Z' : 'Z-A';
}
window.toggleInfraSort = toggleInfraSort;

/* ═══ BOTTOM PANEL ═══ */
var bottomPanelTab = 'terminal';
var bottomPanelExpanded = false;

function switchPanelTab(tab) {
  bottomPanelTab = tab;
  document.querySelectorAll('.panel-tab').forEach(el => el.classList.toggle('active', el.dataset.panel === tab));
  ['terminal', 'events', 'alerts', 'output'].forEach(t => {
    const el = document.getElementById('panel-' + t);
    if (el) el.classList.toggle('hidden', t !== tab);
  });
}
window.switchPanelTab = switchPanelTab;

function toggleBottomPanel() {
  const panel = document.getElementById('bottom-panel');
  if (panel) panel.classList.toggle('collapsed');
}
window.toggleBottomPanel = toggleBottomPanel;

function togglePanelSize() {
  const panel = document.getElementById('bottom-panel');
  if (!panel) return;
  bottomPanelExpanded = !bottomPanelExpanded;
  panel.style.height = bottomPanelExpanded ? '50vh' : '200px';
}
window.togglePanelSize = togglePanelSize;

/* ═══ COMMAND PALETTE (Ctrl+K) — G-4 ═══ */
var CMD_ACTIONS = PCV.filterEditionItems([
  { icon: '+', label: _L('새 VM', 'New VM'), hint: 'Ctrl+N', role: 'operator', action: () => showCreate() },
  { icon: '#', label: _L('운영 개요', 'Operations Overview'), action: () => navigateTo('mon-overview') },
  { icon: '!', label: _L('이벤트 센터', 'Event Center'), action: () => navigateTo('ops-triage') },
  { icon: '>', label: _L('VM 자산', 'VM Inventory'), action: () => switchSbTab('vms') },
  { icon: '~', label: _L('네트워크', 'Networks'), action: () => navigateTo('networks') },
  { icon: '=', label: _L('스토리지', 'Storage'), action: () => navigateTo('storage') },
  { icon: '@', label: _L('컨테이너', 'Containers'), action: () => navigateTo('containers') },
  { icon: '!', label: _L('알림', 'Alerts'), action: () => navigateTo('mon-alerts') },
  { icon: 'S', label: _L('보안 이벤트', 'Security Events'), action: () => navigateTo('mon-security') },
  { icon: '&', label: _L('계정과 권한', 'Accounts'), role: 'admin', action: () => navigateTo('accounts') },
  { icon: '%', label: _L('API 관리', 'API Management'), role: 'admin', action: () => navigateTo('apimgmt') },
  { icon: '?', label: 'Swagger API', action: () => navigateTo('apihelp') },
  { icon: 'i', label: _L('서비스 가이드', 'Service Guide'), action: () => navigateTo('serviceguide') },
  { icon: 'h', label: _L('도움말', 'Help'), action: () => navigateTo('helppage') },
  { icon: 'A', label: _L('AI Agent 설정', 'AI Agent Config'), action: () => showAgentConfig() },
  { icon: 'P', label: _L('환경설정', 'Preferences'), action: () => showPrefs() },
  { icon: 'T', label: _L('테마 전환', 'Toggle Theme'), action: () => toggleTheme() },
  { icon: '\u{1F3A8}', label: _L('테마 편집기', 'Theme Editor'), action: () => openThemeEditor() },
  { icon: 'L', label: _L('언어 전환', 'Toggle Language'), action: () => { I18N.toggle(); location.reload(); } },
  { icon: 'F', label: _L('전체 화면', 'Fullscreen'), hint: 'F11', action: () => toggleFS() },
]);
window.CMD_ACTIONS = CMD_ACTIONS;

/* ═══ J2: FUZZY MATCH HELPER ═══ */
function fuzzyMatch(text, query) {
  if (!query) return true;
  var ti = 0, qi = 0;
  var tLow = text.toLowerCase(), qLow = query.toLowerCase();
  while (ti < tLow.length && qi < qLow.length) {
    if (tLow[ti] === qLow[qi]) qi++;
    ti++;
  }
  return qi === qLow.length;
}
window.fuzzyMatch = fuzzyMatch;

var cmdPaletteOpen = false;
window.cmdPaletteOpen = cmdPaletteOpen;
var cmdSelectedIndex = 0;
window.cmdSelectedIndex = cmdSelectedIndex;

function openCmdPalette() {
  cmdPaletteOpen = true; cmdSelectedIndex = 0;
  window.cmdPaletteOpen = cmdPaletteOpen;
  window.cmdSelectedIndex = cmdSelectedIndex;
  const ov = document.createElement('div'); ov.id = 'cmd-palette-overlay';
  ov.style.cssText = 'position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,.5);display:flex;justify-content:center;padding-top:15vh;z-index:300';
  ov.addEventListener('click', e => { if (e.target === ov) closeCmdPalette(); });
  const box = document.createElement('div'); box.id = 'cmd-palette';
  box.style.cssText = 'background:var(--bg-panel);backdrop-filter:blur(20px);border:1px solid var(--accent);border-radius:12px;width:500px;max-width:90vw;max-height:60vh;display:flex;flex-direction:column;box-shadow:0 0 40px var(--neon-glow);overflow:hidden';
  box.innerHTML = '<input aria-label="' + _L('명령이나 화면 이름을 입력하세요', 'Type a command or page name') + '" id="cmd-input" placeholder="' + _L('명령이나 화면 이름을 입력하세요', 'Type a command or page name') + '" style="padding:14px 16px;background:transparent;border:none;border-bottom:1px solid var(--border);color:var(--fg);font-size:15px;outline:none;font-family:var(--font-mono)" autocomplete="off"><div id="cmd-list" style="overflow-y:auto;flex:1;padding:4px 0"></div>';
  ov.appendChild(box); document.body.appendChild(ov);
  const inp = document.getElementById('cmd-input'); inp.focus();
  renderCmdPalette('');
  inp.addEventListener('input', () => { cmdSelectedIndex = 0; renderCmdPalette(inp.value); });
  inp.addEventListener('keydown', e => {
    const items = document.querySelectorAll('.cmd-item');
    if (e.key === 'ArrowDown') { e.preventDefault(); cmdSelectedIndex = Math.min(cmdSelectedIndex + 1, items.length - 1); renderCmdPalette(inp.value); }
    else if (e.key === 'ArrowUp') { e.preventDefault(); cmdSelectedIndex = Math.max(cmdSelectedIndex - 1, 0); renderCmdPalette(inp.value); }
    else if (e.key === 'Enter') { e.preventDefault(); if (items[cmdSelectedIndex]) items[cmdSelectedIndex].click(); }
  });
}
window.openCmdPalette = openCmdPalette;

function closeCmdPalette() {
  cmdPaletteOpen = false;
  window.cmdPaletteOpen = cmdPaletteOpen;
  const el = document.getElementById('cmd-palette-overlay'); if (el) el.remove();
}
window.closeCmdPalette = closeCmdPalette;

function renderCmdPalette(filter) {
  const list = document.getElementById('cmd-list'); if (!list) return;
  const q = (filter || '').toLowerCase();
  const filtered = CMD_ACTIONS.filter(a => {
    if (a.cluster && !window.pcvClusterEnabled) return false;
    if (a.role && !pcvRoleAllows(a.role)) return false;
    return !q || fuzzyMatch(a.label, q);
  });
  list.innerHTML = filtered.map((a, i) => '<div class="cmd-item" style="padding:10px 16px;cursor:pointer;display:flex;align-items:center;gap:12px;font-size:13px;background:' + (i === cmdSelectedIndex ? 'var(--bg3)' : 'transparent') + ';border-left:3px solid ' + (i === cmdSelectedIndex ? 'var(--accent)' : 'transparent') + '" onclick="CMD_ACTIONS.find(x=>x.label===\'' + a.label.replace(/'/g, "\\'") + '\').action();closeCmdPalette()"><span style="width:22px;text-align:center;font-size:14px">' + a.icon + '</span><span style="flex:1">' + a.label + '</span>' + (a.hint ? '<span class="stat-label">' + a.hint + '</span>' : '') + '</div>').join('');
}
window.renderCmdPalette = renderCmdPalette;

/* ═══ J4: MULTI-WINDOW POPUP ═══ */
function openInPopup(pageId) {
  var page = pageId || currentTab;
  var w = window.open('', 'pcv-' + page, 'width=1200,height=800,menubar=no,toolbar=no');
  if (!w) { toast(_L('팝업 차단됨', 'Popup blocked'), false); return; }
  var theme = document.documentElement.getAttribute('data-theme') || '';
  w.document.write('<!DOCTYPE html><html lang="ko"' + (theme ? ' data-theme="' + theme + '"' : '') + '>'
    + '<head><meta charset="UTF-8"><title>PureCVisor \u2014 ' + page + '</title>'
    + '<link rel="stylesheet" href="/ui/style.css">'
    + '<style>body{margin:0;padding:16px;background:var(--bg);color:var(--fg);font-family:var(--font-mono)}</style>'
    + '</head><body><div id="popup-content"><span class="spinner"></span> Loading...</div>'
    + '<script src="/ui/i18n.js"></script>'
    + '<script src="/ui/modules/api.js"></script>'
    + '<script src="/ui/modules/ui.js"></script>'
    + '<script>authToken="' + (authToken || '') + '";</script>'
    + '</body></html>');
  w.document.close();
  /* Render content into popup after scripts load */
  setTimeout(function() {
    var cb = w.document.getElementById('popup-content');
    if (!cb) return;
    var content = document.getElementById('cb');
    if (content) cb.innerHTML = content.innerHTML;
  }, 500);
}
window.openInPopup = openInPopup;

/* ═══ MOBILE ═══ */
function toggleMobileSB() {
  const sb = document.getElementById('sidebar-panel') || document.getElementById('sidebar'), ov = document.getElementById('mobile-overlay');
  if (sb.classList.contains('mobile-open')) { closeMobileSB(); }
  else { sb.classList.add('mobile-open'); sb.classList.remove('collapsed'); ov.style.display = 'block'; }
}
window.toggleMobileSB = toggleMobileSB;

function closeMobileSB() {
  const sb = document.getElementById('sidebar-panel') || document.getElementById('sidebar'), ov = document.getElementById('mobile-overlay');
  sb.classList.remove('mobile-open'); ov.style.display = 'none';
}
window.closeMobileSB = closeMobileSB;

/* ═══ V-1: ACTIVITY BAR ═══ */
var currentActivity = 'vms';
window.currentActivity = currentActivity;

function switchActivity(panel) {
  if (currentActivity === panel) {
    document.getElementById('sidebar-panel')?.classList.toggle('collapsed');
    return;
  }
  currentActivity = panel;
  window.currentActivity = currentActivity;
  document.getElementById('sidebar-panel')?.classList.remove('collapsed');
  document.querySelectorAll('.activity-icon[data-panel]').forEach(el => {
    el.classList.toggle('active', el.dataset.panel === panel);
  });
  ['vms', 'containers', 'infra'].forEach(t => {
    const el = document.getElementById('sb-' + t);
    if (el) el.classList.toggle('hidden', t !== panel);
  });
  /* sync tab buttons */
  document.querySelectorAll('#sb-tabs button').forEach(b => {
    b.classList.toggle('active', b.dataset.sb === panel);
  });
  const ct = document.getElementById('ct');
  if (ct) ct.style.display = (panel === 'vms') ? 'flex' : 'none';
  if (panel === 'containers') { navigateTo('containers'); }
  if (panel === 'infra') { navigateTo('mon-overview'); }
}
window.switchActivity = switchActivity;

/* ═══ V-2: EDITOR TABS ═══ */
var editorTabs = [];
var activeEditorTab = null;

function openEditorTab(id, label, icon) {
  if (!editorTabs.find(t => t.id === id)) {
    editorTabs.push({ id, label, icon: icon || '' });
  }
  activeEditorTab = id;
  renderEditorTabs();
}
window.openEditorTab = openEditorTab;

function closeEditorTab(id) {
  editorTabs = editorTabs.filter(t => t.id !== id);
  if (activeEditorTab === id) {
    activeEditorTab = editorTabs.length > 0 ? editorTabs[editorTabs.length - 1].id : null;
    if (activeEditorTab) navigateTo(activeEditorTab);
  }
  renderEditorTabs();
}
window.closeEditorTab = closeEditorTab;

function renderEditorTabs() {
  const container = document.getElementById('editor-tabs');
  if (!container) return;
  container.innerHTML = editorTabs.map(t =>
    '<div class="editor-tab ' + (t.id === activeEditorTab ? 'active' : '') + '" onclick="navigateTo(\'' + t.id + '\')">' +
    '<span class="tab-icon">' + t.icon + '</span>' +
    '<span class="tab-label">' + escapeHtml(t.label) + '</span>' +
    '<span class="tab-close" onclick="event.stopPropagation();closeEditorTab(\'' + t.id + '\')">&times;</span>' +
    '</div>'
  ).join('');
}
window.renderEditorTabs = renderEditorTabs;

/* ═══ V-4: BREADCRUMBS (G8: Interactive) ═══ */
function updateBreadcrumbs(page) {
  var el = document.getElementById('breadcrumbs');
  if (!el) return;
  var pg = page || currentTab || 'dashboard';
  var parts = [{ label: _L('대시보드', 'Dashboard'), tab: 'dashboard' }];
  var tabLabels = {
    'summary': _L('요약', 'Summary'), 'console': _L('콘솔', 'Console'),
    'snapshots': _L('스냅샷', 'Snapshots'), 'performance': _L('성능', 'Performance'),
    'timeline': _L('타임라인', 'Timeline'), 'networks': _L('네트워크', 'Networks'),
    'storage': _L('스토리지', 'Storage'), 'cluster': _L('클러스터', 'Cluster'),
    'containers': _L('컨테이너', 'Containers'), 'mon-overview': _L('운영 개요', 'Operations Overview'),
    'ops-triage': _L('이벤트 센터', 'Event Center'),
    'dashboard': _L('대시보드', 'Dashboard'),
    'host': _L('호스트 상태', 'Host Health'),
  };
  if (pg && pg !== 'dashboard') {
    var label = tabLabels[pg] || pg.replace(/-/g, ' ').replace(/\b\w/g, function(c) { return c.toUpperCase(); });
    parts.push({ label: label, tab: pg });
    if (['summary','console','snapshots','performance','timeline'].indexOf(pg) !== -1 && vmList[selectedVmIndex]) {
      parts.push({ label: vmList[selectedVmIndex].name, tab: null });
    }
  }
  var h = parts.map(function(p, i) {
    var isLast = i === parts.length - 1;
    if (isLast) return '<span class="color-accent" style="font-size:11px">' + esc(p.label) + '</span>';
    return '<span style="cursor:pointer;font-size:11px;color:var(--fg2)" onclick="navigateTo(\'' + (p.tab || 'dashboard') + '\')">' + esc(p.label) + '</span>';
  }).join(' <span class="color-muted" style="font-size:10px">&#9656;</span> ');
  h += ' <span style="cursor:pointer;font-size:10px;color:' + (_pinnedPages.includes(currentTab) ? 'var(--yellow)' : 'var(--fg2)') + '" onclick="togglePin(\'' + currentTab + '\');updateBreadcrumbs()">&#128204;</span>';
  h += ' <span style="cursor:pointer;font-size:10px;color:var(--fg2)" onclick="openInPopup()" title="' + _L('새 창에서 열기', 'Open in new window') + '">&#128194;</span>';
  el.innerHTML = h;
}
window.updateBreadcrumbs = updateBreadcrumbs;

/* ═══ V-5: GLOBAL SEARCH (Ctrl+Shift+F) ═══ */
window.globalSearchOpen = false;

function toggleGlobalSearch() {
  if (window.globalSearchOpen) { closeGlobalSearch(); return; }
  window.globalSearchOpen = true;
  const ov = document.createElement('div');
  ov.id = 'global-search-overlay';
  ov.className = 'global-search-overlay';
  ov.onclick = e => { if (e.target === ov) closeGlobalSearch(); };
  ov.innerHTML = '<div class="global-search-box">' +
    '<input aria-label="Search VMs, containers, networks, settings..." class="global-search-input" id="global-search-input" placeholder="Search VMs, containers, networks, settings..." oninput="(window._gs || (window._gs = pcvDebounce(doGlobalSearch, 200)))(this.value)" autofocus>' +
    '<div class="global-search-results" id="global-search-results"></div></div>';
  document.body.appendChild(ov);
  setTimeout(() => document.getElementById('global-search-input')?.focus(), 50);
}
window.toggleGlobalSearch = toggleGlobalSearch;

function closeGlobalSearch() {
  window.globalSearchOpen = false;
  document.getElementById('global-search-overlay')?.remove();
}
window.closeGlobalSearch = closeGlobalSearch;

function doGlobalSearch(query) {
  const results = document.getElementById('global-search-results');
  if (!results || !query) { if (results) results.innerHTML = ''; return; }
  const q = query.toLowerCase();
  let html = '';

  /* Search VMs */
  const matchedVMs = vmList.filter(v => fuzzyMatch(v.name, q));
  if (matchedVMs.length > 0) {
    html += '<div class="global-search-group"><div class="global-search-group-title">Virtual Machines</div>';
    matchedVMs.forEach(v => {
      html += '<div class="global-search-item" onclick="closeGlobalSearch();selectedVmIndex=' + vmList.indexOf(v) + ';switchActivity(\'vms\');render()"><span class="search-icon">&#128187;</span><span class="search-label">' + escapeHtml(v.name) + '</span><span class="search-type">' + (v.state || '') + '</span></div>';
    });
    html += '</div>';
  }

  /* Search pages */
  const pages = PCV.filterEditionItems([
    { id: 'networks', label: _L('네트워크', 'Networks'), icon: '&#127760;' },
    { id: 'storage', label: _L('스토리지', 'Storage'), icon: '&#128190;' },
    { id: 'containers', label: _L('컨테이너', 'Containers'), icon: '&#9783;' },
    { id: 'mon-overview', label: _L('운영 개요', 'Monitoring Overview'), icon: '&#128200;' },
    { id: 'ops-triage', label: _L('이벤트 센터', 'Event Center'), icon: '&#9889;' },
    { id: 'mon-alerts', label: _L('알림', 'Alerts'), icon: '&#128276;' },
    { id: 'mon-security', label: _L('보안 이벤트', 'Security Events'), icon: '&#128737;' },
    { id: 'accounts', label: _L('계정과 권한', 'Accounts'), icon: '&#128100;', role: 'admin' },
    { id: 'security-groups', label: _L('보안 그룹', 'Security Groups'), icon: '&#128737;' },
    { id: 'gpu', label: _L('GPU 장치', 'GPU'), icon: '&#127918;' },
    /* Docker/OCI 제거됨 */
    /* Terraform 제거됨 */
    { id: 'apihelp', label: _L('Swagger API', 'Swagger API'), icon: '&#128214;' },
    { id: 'serviceguide', label: _L('서비스 가이드', 'Service Guide'), icon: '&#128218;' },
    { id: 'overlay', label: _L('오버레이 네트워크', 'Overlay Networks'), icon: '&#127760;' },
    { id: 'iscsi', label: _L('iSCSI 타깃', 'iSCSI Targets'), icon: '&#128190;' },
    { id: 'dpdk', label: 'DPDK', icon: '&#9889;' },
    { id: 'sriov', label: 'SR-IOV', icon: '&#128268;' },
  ]);
  const matchedPages = pages.filter(p => {
    if (p.cluster && !window.pcvClusterEnabled) return false;
    if (p.role && !pcvRoleAllows(p.role)) return false;
    return fuzzyMatch(p.label, q);
  });
  if (matchedPages.length > 0) {
    html += '<div class="global-search-group"><div class="global-search-group-title">Pages</div>';
    matchedPages.forEach(p => {
      html += '<div class="global-search-item" onclick="closeGlobalSearch();navigateTo(\'' + p.id + '\')"><span class="search-icon">' + p.icon + '</span><span class="search-label">' + p.label + '</span><span class="search-type">Page</span></div>';
    });
    html += '</div>';
  }

  if (!html) html = '<div style="padding:20px;text-align:center;color:var(--fg2)">No results for "' + escapeHtml(query) + '"</div>';
  results.innerHTML = html;
}
window.doGlobalSearch = doGlobalSearch;

/* ═══ V-9: ZEN MODE ═══ */
window.zenMode = false;
function toggleZenMode() {
  window.zenMode = !window.zenMode;
  document.body.classList.toggle('zen-mode', window.zenMode);
  if (window.zenMode) toast('Zen Mode \u2014 press Escape to exit');
}
window.toggleZenMode = toggleZenMode;

/* ═══ V-8: NOTIFICATIONS CENTER ═══ */
var notifications = [];
window.notifCenterOpen = false;
var notifFilter = 'all';

function addNotification(type, title, msg) {
  notifications.unshift({ type: type, title: title, msg: msg || '', time: new Date(), read: false, id: Date.now() });
  if (notifications.length > 50) notifications.pop();
  updateNotifBadge();
  if (typeof sendBrowserNotif === 'function') sendBrowserNotif(title, msg);
  if (typeof playNotifSound === 'function') playNotifSound(type === 'error' ? 'error' : type === 'warning' ? 'warning' : 'success');
}
window.addNotification = addNotification;

function updateNotifBadge() {
  var unread = notifications.filter(function(n) { return !n.read; }).length;
  var badge = document.getElementById('alert-badge');
  if (badge) { badge.textContent = unread; badge.style.display = unread > 0 ? '' : 'none'; }
  var actBadge = document.querySelector('.activity-icon[data-panel="notifications"] .activity-badge');
  if (actBadge) { actBadge.textContent = unread; actBadge.style.display = unread > 0 ? '' : 'none'; }
  /* Update toolbar notif icon badge if present */
  var toolbarBadge = document.getElementById('notif-toolbar-badge');
  if (toolbarBadge) { toolbarBadge.textContent = unread; toolbarBadge.style.display = unread > 0 ? '' : 'none'; }
}
window.updateNotifBadge = updateNotifBadge;

function _renderNotifList(container, filter) {
  var filtered = notifications;
  if (filter && filter !== 'all') {
    filtered = notifications.filter(function(n) { return n.type === filter; });
  }
  if (filtered.length === 0) {
    container.innerHTML = '<div class="notif-center-empty">&#128276; No ' + (filter !== 'all' ? filter + ' ' : '') + 'notifications</div>';
    return;
  }
  var list = '';
  filtered.forEach(function(n) {
    var icon = n.type === 'error' ? '&#10060;' : n.type === 'warning' ? '&#9888;' : '&#9989;';
    var timeAgo = _notifTimeAgo(n.time);
    list += '<div class="notif-item ' + (n.read ? '' : 'unread') + '" onclick="markRead(' + n.id + ');this.classList.remove(\'unread\')">'
      + '<span class="notif-icon">' + icon + '</span>'
      + '<div class="notif-body">'
      + '<div class="notif-title">' + escapeHtml(n.title) + '</div>'
      + (n.msg ? '<div class="notif-msg">' + escapeHtml(n.msg) + '</div>' : '')
      + '<div class="notif-time">' + timeAgo + '</div>'
      + '</div></div>';
  });
  container.innerHTML = list;
}

function _notifTimeAgo(date) {
  var diff = Math.floor((Date.now() - date.getTime()) / 1000);
  if (diff < 60) return diff + 's ago';
  if (diff < 3600) return Math.floor(diff / 60) + 'm ago';
  if (diff < 86400) return Math.floor(diff / 3600) + 'h ago';
  return date.toLocaleDateString();
}

function toggleNotifCenter() {
  if (window.notifCenterOpen) { closeNotifCenter(); return; }
  window.notifCenterOpen = true;
  notifFilter = 'all';
  var unread = notifications.filter(function(n) { return !n.read; }).length;
  var el = document.createElement('div');
  el.id = 'notif-center';
  el.className = 'notif-center';

  /* Header with title and actions */
  var header = '<div class="notif-center-header">'
    + '<span>Notifications' + (unread > 0 ? ' <span class="notif-header-badge">' + unread + ' unread</span>' : '') + '</span>'
    + '<div style="display:flex;gap:6px">'
    + '<button class="btn" style="font-size:9px;padding:2px 8px" onclick="markAllRead()">Mark all read</button>'
    + '<button class="btn btn-r" style="font-size:9px;padding:2px 8px" onclick="clearNotifications()">Clear</button>'
    + '<button class="panel-action-btn" onclick="closeNotifCenter()">&#10005;</button>'
    + '</div></div>';

  /* Filter bar */
  var filterBar = '<div class="notif-filter-bar">'
    + '<button class="notif-filter-btn active" data-filter="all" onclick="setNotifFilter(\'all\')">All (' + notifications.length + ')</button>'
    + '<button class="notif-filter-btn" data-filter="error" onclick="setNotifFilter(\'error\')">Error (' + notifications.filter(function(n){ return n.type==='error'; }).length + ')</button>'
    + '<button class="notif-filter-btn" data-filter="warning" onclick="setNotifFilter(\'warning\')">Warning (' + notifications.filter(function(n){ return n.type==='warning'; }).length + ')</button>'
    + '<button class="notif-filter-btn" data-filter="info" onclick="setNotifFilter(\'info\')">Info (' + notifications.filter(function(n){ return n.type==='info'; }).length + ')</button>'
    + '</div>';

  el.innerHTML = header + filterBar + '<div class="notif-center-list" id="notif-list-container"></div>';
  document.body.appendChild(el);

  /* Render list */
  var listContainer = document.getElementById('notif-list-container');
  _renderNotifList(listContainer, 'all');
}
window.toggleNotifCenter = toggleNotifCenter;

function setNotifFilter(filter) {
  notifFilter = filter;
  /* Update filter button states */
  document.querySelectorAll('.notif-filter-btn').forEach(function(btn) {
    btn.classList.toggle('active', btn.dataset.filter === filter);
  });
  var listContainer = document.getElementById('notif-list-container');
  if (listContainer) _renderNotifList(listContainer, filter);
}
window.setNotifFilter = setNotifFilter;

function closeNotifCenter() {
  window.notifCenterOpen = false;
  var el = document.getElementById('notif-center');
  if (el) el.remove();
}
window.closeNotifCenter = closeNotifCenter;

function markAllRead() {
  notifications.forEach(function(n) { n.read = true; });
  updateNotifBadge();
  closeNotifCenter(); toggleNotifCenter();
}
window.markAllRead = markAllRead;

function markRead(id) {
  var n = notifications.find(function(x) { return x.id === id; });
  if (n) n.read = true;
  updateNotifBadge();
}
window.markRead = markRead;

function clearNotifications() {
  notifications = [];
  updateNotifBadge();
  closeNotifCenter(); toggleNotifCenter();
}
window.clearNotifications = clearNotifications;

/* ═══ V-7: HOVER INFO ═══ */
var hoverCardTimeout = null;
var hoverCard = document.createElement('div');
hoverCard.className = 'hover-card';
hoverCard.id = 'hover-card';
document.body.appendChild(hoverCard);

function showHoverCard(e, vm) {
  clearTimeout(hoverCardTimeout);
  hoverCardTimeout = setTimeout(() => {
    const on = vm.state === 'running';
    hoverCard.innerHTML = '<div class="hover-card-title"><span class="dot ' + (on ? 'on' : 'off') + '"></span>' + escapeHtml(vm.name) + '</div>' +
      '<div class="hover-card-row"><span class="hc-k">State</span><span class="hc-v">' + (vm.state || '-') + '</span></div>' +
      '<div class="hover-card-row"><span class="hc-k">vCPU</span><span class="hc-v">' + (vm.vcpu || '-') + '</span></div>' +
      '<div class="hover-card-row"><span class="hc-k">Memory</span><span class="hc-v">' + (vm.memory_mb || '-') + ' MB</span></div>' +
      '<div class="hover-card-row"><span class="hc-k">CPU</span><span class="hc-v">' + ((vm.live_cpu_pct || 0).toFixed(1)) + '%</span></div>';
    hoverCard.style.left = (e.clientX + 16) + 'px';
    hoverCard.style.top = (e.clientY - 10) + 'px';
    hoverCard.classList.add('visible');
  }, 300);
}
window.showHoverCard = showHoverCard;

function hideHoverCard() {
  clearTimeout(hoverCardTimeout);
  hoverCard.classList.remove('visible');
}
window.hideHoverCard = hideHoverCard;

/* ═══ G7: SEARCH RESULT HIGHLIGHT ═══ */
function highlightText(text, query) {
  if (!query || !text) return esc(text);
  var escaped = esc(text);
  var re = new RegExp('(' + query.replace(/[.*+?^${}()|[\]\\]/g, '\\$&') + ')', 'gi');
  return escaped.replace(re, '<mark style="background:var(--yellow);color:#000;padding:0 2px;border-radius:2px">$1</mark>');
}
window.highlightText = highlightText;

/* ═══ G6: TOUCH GESTURES ═══ */
(function() {
  var touchStartX = 0, touchStartY = 0, touchStartTime = 0;
  document.addEventListener('touchstart', function(e) {
    touchStartX = e.touches[0].clientX;
    touchStartY = e.touches[0].clientY;
    touchStartTime = Date.now();
  }, { passive: true });

  document.addEventListener('touchend', function(e) {
    var dx = e.changedTouches[0].clientX - touchStartX;
    var dy = e.changedTouches[0].clientY - touchStartY;
    var dt = Date.now() - touchStartTime;
    if (dt > 500 || Math.abs(dx) < 50) return;
    if (Math.abs(dy) > Math.abs(dx)) return;

    var tabs = ['summary', 'console', 'snapshots', 'performance', 'timeline'];
    var idx = tabs.indexOf(currentTab);
    if (idx === -1) return;

    if (dx < -50 && idx < tabs.length - 1) {
      currentTab = tabs[idx + 1];
      renderContent();
    } else if (dx > 50 && idx > 0) {
      currentTab = tabs[idx - 1];
      renderContent();
    }
  }, { passive: true });

  /* Long press for context menu */
  var longPressTimer = null;
  document.addEventListener('touchstart', function(e) {
    var vi = e.target.closest('.vi');
    if (!vi) return;
    longPressTimer = setTimeout(function() {
      var idx = Array.from(vi.parentElement.children).filter(function(c) { return c.classList.contains('vi'); }).indexOf(vi);
      if (idx >= 0 && typeof showCtx === 'function') {
        showCtx({ preventDefault: function(){}, pageX: e.touches[0].clientX, pageY: e.touches[0].clientY }, idx);
      }
    }, 600);
  }, { passive: true });
  document.addEventListener('touchend', function() { clearTimeout(longPressTimer); }, { passive: true });
  document.addEventListener('touchmove', function() { clearTimeout(longPressTimer); }, { passive: true });
})();

/* ═══ SIDEBAR RESIZE (D5) ═══ */
(function() {
  var handle = document.getElementById('sb-resize');
  var sidebar = document.querySelector('.sidebar-panel') || document.querySelector('.sidebar');
  if (!handle || !sidebar) return;
  var startX, startW;
  handle.addEventListener('mousedown', function(e) {
    e.preventDefault();
    startX = e.clientX;
    startW = sidebar.offsetWidth;
    handle.classList.add('active');
    function onMove(e2) {
      var newW = Math.max(180, Math.min(500, startW + e2.clientX - startX));
      sidebar.style.width = newW + 'px';
      document.documentElement.style.setProperty('--sw', newW + 'px');
    }
    function onUp() {
      handle.classList.remove('active');
      document.removeEventListener('mousemove', onMove);
      document.removeEventListener('mouseup', onUp);
      localStorage.setItem('pcv-sb-width', sidebar.style.width);
    }
    document.addEventListener('mousemove', onMove);
    document.addEventListener('mouseup', onUp);
  });
  var saved = localStorage.getItem('pcv-sb-width');
  if (saved) { sidebar.style.width = saved; document.documentElement.style.setProperty('--sw', saved); }
})();

/* ═══ 메뉴바 키보드 접근 (프론트 #3, 3-1) + roving tabindex (프론트 #4-C1) ═══
 * 기존에는 style.css의 .menu-item:hover .menu-drop { display:block } 만이
 * 드롭다운을 여는 유일한 트리거였다 — 키보드/터치로는 File/Edit/View 등
 * 메뉴를 열 수 없었다. click/keydown 위임으로 .open 토글을 추가해
 * 열기/닫기 키보드 수단을 확보한다. (:focus-within 을 열기 트리거로
 * 쓰지 않는 이유: Esc 후에도 포커스가 트리거에 남아 드롭다운이
 * 시각적으로 계속 열려 보이는 문제 — 포커스만으로는 열지 않는 게
 * disclosure 표준 동작이기도 하다. 83e4e7f에서 이미 제거됨.)
 *
 * #4-C1: WAI-ARIA menubar 패턴의 roving tabindex를 완성한다.
 * - top-level .menu-item 8개: 탭 스톱 1개만 유지(활성 항목만 tabindex=0,
 *   나머지 -1) — roving. 드롭다운 .mi 45개: 전부 tabindex=-1로 JS가
 *   일괄 초기화(45개 정적 수정보다 유지보수 안전 — 컨트롤러 결정).
 *   tabindex=-1이어도 el.focus()로 프로그램적 포커스는 가능하다.
 * - Enter/Space/ArrowDown으로 열면 첫 .mi로 포커스 진입, 드롭다운
 *   안에서는 ↑/↓로 .mi 간 순환 이동, ←/→는 현재 메뉴를 닫고 인접
 *   top-level 메뉴를 열어 그 첫 .mi로 이동한다. top-level 자체에서
 *   ←/→는 인접 top-level로 포커스만 이동(끝에서 순환) — 이미 메뉴가
 *   열려 있었을 때만 이동한 메뉴도 함께 열어 hover와 동일한 UX를 낸다.
 * - Enter/Space는 이 IIFE에서 의도적으로 가로채지 않는다: 3-3 전역
 *   위임(document keydown, 아래)이 버블링으로 el.click()을 발화한다
 *   (vm.js F1 핸들러의 위젯 가드 — 별도 수정 — 덕분에 더 이상 가로채지
 *   않음을 확인했다). */
(function() {
  var menubar = document.querySelector('.menubar');
  if (!menubar) return;
  var items = menubar.querySelectorAll('.menu-item');

  /* roving tabindex 초기 상태: 첫 top-level만 탭 스톱, 나머지 -1.
   * 드롭다운 .mi는 전부 -1(포커스는 오직 JS가 명시적으로 옮긴다). */
  items.forEach(function(mi, idx) { mi.setAttribute('tabindex', idx === 0 ? '0' : '-1'); });
  menubar.querySelectorAll('.mi').forEach(function(mi) { mi.setAttribute('tabindex', '-1'); });

  function closeAll(except) {
    items.forEach(function(mi) {
      if (mi !== except) { mi.classList.remove('open'); mi.setAttribute('aria-expanded', 'false'); }
    });
  }

  function openItem(item) {
    closeAll(item);
    item.classList.add('open');
    item.setAttribute('aria-expanded', 'true');
  }

  /* 탭 스톱(roving tabindex=0)을 el로 옮긴다 — 나머지 top-level은 -1. */
  function setActiveTopLevel(el) {
    items.forEach(function(mi) { mi.setAttribute('tabindex', mi === el ? '0' : '-1'); });
  }

  function topIndex(item) { return Array.prototype.indexOf.call(items, item); }
  function topLevelAt(idx) {
    var n = items.length;
    return items[((idx % n) + n) % n];
  }

  /* item의 드롭다운 안 .mi 목록 — separator(.sep) 제외, role 가시성으로
   * display:none 처리된 항목(#15 데코 가드)도 이동/포커스 대상에서 제외. */
  function menuItemsOf(item) {
    var drop = item.querySelector('.menu-drop');
    if (!drop) return [];
    return Array.prototype.filter.call(drop.querySelectorAll('.mi'), function(el) {
      return el.style.display !== 'none';
    });
  }

  menubar.addEventListener('click', function(e) {
    var item = e.target.closest('.menu-item');
    if (!item) return;
    if (e.target.closest('.mi')) {
      /* 드롭다운 안 menuitem 클릭 실행(기존 onclick 그대로 발화) 후 메뉴 닫기 */
      closeAll(null);
      return;
    }
    var willOpen = !item.classList.contains('open');
    closeAll(item);
    item.classList.toggle('open', willOpen);
    item.setAttribute('aria-expanded', String(willOpen));
  });

  /* 마우스 클릭도 tabindex=-1인 top-level item을 포커스시킬 수 있어
   * (클릭 포커스는 tabindex 값과 무관하게 동작) focusin 한 곳에서 roving
   * 상태를 일관되게 갱신한다 — 클릭 자체의 열기/닫기 UX는 위 click
   * 핸들러 그대로, 변경 없음. */
  menubar.addEventListener('focusin', function(e) {
    if (e.target.classList && e.target.classList.contains('menu-item')) {
      setActiveTopLevel(e.target);
    }
  });

  menubar.addEventListener('keydown', function(e) {
    var mi = e.target.closest('.mi');
    if (mi) {
      var miItem = mi.closest('.menu-item');
      if (!miItem) return;
      if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
        /* 드롭다운 안 .mi 간 순환 이동 */
        e.preventDefault();
        e.stopPropagation();
        var mis = menuItemsOf(miItem);
        var idx = mis.indexOf(mi);
        if (idx === -1 || mis.length === 0) return;
        var delta = e.key === 'ArrowDown' ? 1 : -1;
        var n = mis.length;
        mis[((idx + delta) % n + n) % n].focus();
      } else if (e.key === 'ArrowLeft' || e.key === 'ArrowRight') {
        /* 현재 메뉴 닫고 인접 top-level 메뉴를 열어 그 첫 .mi로 이동 */
        e.preventDefault();
        e.stopPropagation();
        var idx2 = topIndex(miItem);
        var delta2 = e.key === 'ArrowRight' ? 1 : -1;
        var next = topLevelAt(idx2 + delta2);
        setActiveTopLevel(next);
        openItem(next);
        var fmi = menuItemsOf(next)[0];
        if (fmi) fmi.focus(); else next.focus();
      } else if (e.key === 'Escape') {
        e.preventDefault();
        e.stopPropagation();
        miItem.classList.remove('open');
        miItem.setAttribute('aria-expanded', 'false');
        setActiveTopLevel(miItem);
        miItem.focus();
      }
      /* Enter/Space는 여기서 처리하지 않고 그대로 버블링시켜 3-3 전역
       * 위임(document keydown)이 el.click()으로 실행하게 둔다. */
      return;
    }

    var item = e.target.closest('.menu-item');
    if (!item) return;
    if (e.key === 'Enter' || e.key === ' ' || e.key === 'Spacebar' || e.key === 'ArrowDown') {
      /* stopPropagation 필수: document 레벨의 다른 전역 keydown(예: vm.js의
       * VM 목록 j/k/Enter 탐색)이 같은 Enter를 가로채 화면을 전환하는
       * 충돌이 실측에서 확인됨(currentTab이 dashboard/summary 등일 때
       * 메뉴 Enter가 VM Summary로 넘어가버림) — 위젯이 키를 완전히
       * 소비했음을 명시해 document까지 버블링되지 않게 막는다. */
      e.preventDefault();
      e.stopPropagation();
      setActiveTopLevel(item);
      openItem(item);
      var firstMi = menuItemsOf(item)[0];
      if (firstMi) firstMi.focus();
    } else if (e.key === 'Escape' && item.classList.contains('open')) {
      e.preventDefault();
      e.stopPropagation();
      item.classList.remove('open');
      item.setAttribute('aria-expanded', 'false');
      item.focus();
    } else if (e.key === 'ArrowLeft' || e.key === 'ArrowRight') {
      /* top-level 사이 순환 이동. 이미 열려 있던 메뉴가 있었을 때만
       * 이동한 메뉴도 함께 열어 hover와 동일한 UX를 낸다. */
      e.preventDefault();
      e.stopPropagation();
      var idx3 = topIndex(item);
      var delta3 = e.key === 'ArrowRight' ? 1 : -1;
      var wasOpen = item.classList.contains('open');
      var next2 = topLevelAt(idx3 + delta3);
      setActiveTopLevel(next2);
      next2.focus();
      if (wasOpen) openItem(next2);
    }
  });

  /* 메뉴바 바깥 클릭 시 전부 닫기 */
  document.addEventListener('click', function(e) {
    if (!menubar.contains(e.target)) closeAll(null);
  });

  /* 포커스가 메뉴바 밖으로 나가면 전부 닫기 — Tab 이탈 시 .open 이
   * 남아 드롭다운이 열린 채 방치되는 것을 방지 (relatedTarget 이 null
   * 인 경우(창 포커스 이탈 등)도 닫는 쪽이 안전) */
  menubar.addEventListener('focusout', function(e) {
    if (!menubar.contains(e.relatedTarget)) closeAll(null);
  });
})();

/* ═══ 인터랙티브 div/span 키보드 활성화 (프론트 #3, 3-3) ═══
 * <div role="button|menuitem|link"> 100+ 개가 onclick 만으로 구현되어
 * Enter/Space에 무반응이었다(네이티브 button/a 가 아니므로 브라우저가
 * 자동으로 keydown→click 을 발생시켜주지 않음). 전역 keydown 위임 1개로
 * 보정 — 기존 단축키(app.js: Ctrl+K/N/D/P·Esc·F11)와는 대상 태그가
 * 겹치지 않아 충돌 없음. defaultPrevented 체크로 상위 메뉴바(3-1, top-level
 * .menu-item 도 role="menuitem")의 자체 Enter/Space 처리와 이중 발화 방지. */
document.addEventListener('keydown', function(e) {
  if (e.defaultPrevented) return;
  var el = e.target.closest && e.target.closest('[role="button"], [role="menuitem"], [role="link"]');
  if (!el) return;
  var tag = el.tagName;
  if (tag === 'BUTTON' || tag === 'A' || tag === 'INPUT' || tag === 'SELECT' || tag === 'TEXTAREA') return;
  var role = el.getAttribute('role');
  var activate = (role === 'link') ? (e.key === 'Enter') : (e.key === 'Enter' || e.key === ' ' || e.key === 'Spacebar');
  if (!activate) return;
  e.preventDefault();
  el.click();
});

/* ── PCV.nav namespace export ─────────────────────── */
PCV.nav = {
  togglePin: togglePin,
  renderPinnedBar: renderPinnedBar,
  navigateTo: navigateTo,
  switchSbTab: switchSbTab,
  renderContent: renderContent,
  updateStatusBar: updateStatusBar,
  updateFavicon: updateFavicon,
  toggleSB: toggleSB,
  toggleFS: toggleFS,
  toggleInfraSort: toggleInfraSort,
  switchPanelTab: switchPanelTab,
  toggleBottomPanel: toggleBottomPanel,
  togglePanelSize: togglePanelSize,
  CMD_ACTIONS: CMD_ACTIONS,
  fuzzyMatch: fuzzyMatch,
  openCmdPalette: openCmdPalette,
  closeCmdPalette: closeCmdPalette,
  renderCmdPalette: renderCmdPalette,
  openInPopup: openInPopup,
  toggleMobileSB: toggleMobileSB,
  closeMobileSB: closeMobileSB,
  switchActivity: switchActivity,
  openEditorTab: openEditorTab,
  closeEditorTab: closeEditorTab,
  renderEditorTabs: renderEditorTabs,
  updateBreadcrumbs: updateBreadcrumbs,
  toggleGlobalSearch: toggleGlobalSearch,
  closeGlobalSearch: closeGlobalSearch,
  doGlobalSearch: doGlobalSearch,
  toggleZenMode: toggleZenMode,
  addNotification: addNotification,
  updateNotifBadge: updateNotifBadge,
  toggleNotifCenter: toggleNotifCenter,
  setNotifFilter: setNotifFilter,
  closeNotifCenter: closeNotifCenter,
  markAllRead: markAllRead,
  markRead: markRead,
  clearNotifications: clearNotifications,
  showHoverCard: showHoverCard,
  hideHoverCard: hideHoverCard,
  highlightText: highlightText
};
})(window.PCV);
