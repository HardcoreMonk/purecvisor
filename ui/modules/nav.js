                                                                  
                              
                                                                      
                                                  
                                                                  
                                                                     
                                                        
                               
  
                           
                                                                          
  
                                                                
                                                   
                                          
  
                                                                 
                                                                          
                                                 
  
                                                               
                                                                    
                         
  
                                                          
                                                       
  
                                                                  
                                                                
                                                                 
                                                   
                                                 
   

window.PCV = window.PCV || {};
(function(PCV) {

                        
window._navGeneration = 0;
   
                                                  
  
                                                                     
                                                  
                                                        
  
                                        
                                                                
                                                             
                                                  
                                                               
                                                                            
                                                          
  
                                                                       
                                                         
                                                    
  
                  
                                                      
                                                   
                                                    
  
                                                                      
   
function _commitNavigation(n) {
  if (window.pcvClusterEnabled === false && window.PCV_CLUSTER_ONLY_NAV && window.PCV_CLUSTER_ONLY_NAV.includes(n)) {
    if (typeof toast === 'function') toast(_L('Single Edge 공개 리포에는 포함되지 않는 화면입니다', 'This screen is not included in Single Edge'), false);
    n = 'dashboard';
  }
                                                           
  window._navGeneration = (window._navGeneration || 0) + 1;
                                                          
  if (typeof _cloudCleanupTimer === 'function' && currentTab === 'cloud-migration' && n !== 'cloud-migration') {
    _cloudCleanupTimer();
  }
                           
  if (typeof stopAdaptivePolling === 'function' && currentTab && currentTab.startsWith('mon-') && !(n && n.startsWith('mon-'))) {
    stopAdaptivePolling('mon-refresh');
  }
                                                                                   
  if (typeof clearAllFormDirty === 'function') clearAllFormDirty();
                                       
  var vmTabs = ['summary', 'console', 'snapshots', 'performance', 'timeline'];
  if (vmTabs.includes(n)) localStorage.setItem('pcv-last-vm-tab', n);
  currentTab = n;
  if (window.PCV && PCV.shell && PCV.shell.setActive) PCV.shell.setActive(n);
  renderContent();
}

  
                  
                                                                             
                                                                               
                                         
  
               
                                                                               
                                                             
   
function navigateTo(n, hooks) {
  return PCV.ui.requestNavigation(n, function() {
    if (hooks && typeof hooks.before === 'function') hooks.before();
    _commitNavigation(n);
    if (hooks && typeof hooks.after === 'function') hooks.after();
  });
}
window.navigateTo = navigateTo;
                       
window.go = navigateTo;

   
                                                              
  
                                                            
                                                        
                                           
                                            
  
                                                         
                                
   
function pcvRoleAllows(minRole) {
  var roleRank = { viewer: 0, operator: 1, admin: 2 };
  var current = String((window.currentUser && window.currentUser.role) || '').toLowerCase();
  var required = String(minRole || 'viewer').toLowerCase();
  return (roleRank[current] ?? -1) >= (roleRank[required] ?? 0);
}
window.pcvRoleAllows = pcvRoleAllows;

                              
var _renderContentTicket = 0;
function renderContent() {
                                                                                 
                                                                      
                                                                        
  var ticket = ++_renderContentTicket;
  var generation = window._navGeneration || 0;
  var tab = currentTab;
  PCV.uxlib.viewTransition(function() {
                                                                 
                                                              
                                                                 
    if (ticket !== _renderContentTicket || generation !== (window._navGeneration || 0) || tab !== currentTab) return;
    _renderContentPaint(tab, generation);
  });
}
   
                                                                            
  
                                                                      
                                                        
  
                                                              
                                                               
                                     
  
                                                                 
                              
   
function _renderContentPaint(tab, generation) {
                                                                  
                                                                            
  tab = tab || currentTab;
  generation = generation ?? (window._navGeneration || 0);
  destroyAllCharts();
  var cb = document.getElementById('cb');
                                                                         
  if (cb && !document.startViewTransition &&
      !(window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches)) {
    cb.classList.add('fade-out');
  }
                                                                           
                                                                        
                                                                
  if (cb) {
    PCV.uxlib.clearEl(cb);
    var _rt = PCV.uxlib.el('div', { class: 'cb-render' });
    cb.appendChild(_rt);
    PCV.ui._renderTarget = _rt;
  }
  const b = (cb ? PCV.ui._renderTarget : null), v = vmList[selectedVmIndex];
                                                                  
  var gen = generation;
  var mk = PCV.uxlib.el;
  try {
    var fn = (function() {
      var routes = {
      dashboard: () => renderDashboard(b),
      summary: () => renderVmScreen(b, v, 'summary'),
      console: () => renderVmScreen(b, v, 'console'),
      snapshots: () => renderVmScreen(b, v, 'snapshots'),
      performance: () => renderVmScreen(b, v, 'performance'),
      timeline: () => renderVmScreen(b, v, 'timeline'),
      networks: () => renderNetworks(b),
      vpcs: () => renderVpcs(b),
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
      'cloud-migration': () => renderCloudMigration(b),
      'overlay': () => renderOverlayNetworks(b),
      'iscsi': () => renderIscsi(b),
      'dpdk': () => renderDpdk(b),
      'sriov': () => renderSriov(b),
      'pool-info': () => renderPoolInfo(b),
      'heatmap': () => renderHeatmap(b),
      'api-perf': () => renderApiPerf(b),
      'activity-log': () => renderActivityLog(b),
      'topology': () => renderTopology(b),
      'backup': () => renderBackup(b),
      apihelp: () => renderSwaggerApi(b),
      helppage: () => renderHelp(b),
                                                                         
      'selfhealing': () => renderSelfHealing(b)
      };
      return routes;
    })()[tab];
    if (fn) {
      var result = fn();
      if (result && typeof result.then === 'function') {
        Promise.resolve(result).then(function() {
                                                          
                                                               
                                                        
                                                    
          if ((window._navGeneration || 0) !== gen) return;
          if (typeof applyRoleVisibility === 'function') {
            applyRoleVisibility(window.currentUser && window.currentUser.role);
          }
        }).catch(function(err) {
          if ((window._navGeneration || 0) !== gen) return;
          if (b) {
            PCV.uxlib.clearEl(b);
            b.appendChild(mk('div', { style: 'padding:40px;text-align:center' },
              mk('div', { style: 'font-size:48px;margin-bottom:12px' }, '⚠'),
              mk('h3', { style: 'color:var(--red)' }, _L('렌더링 오류', 'Rendering Error')),
              mk('p', { class: 'color-muted', style: 'margin:12px 0' }, err.message || ''),
              mk('button', { class: 'btn', onclick: "navigateTo('dashboard')", style: 'margin-top:12px' }, _L('대시보드로', 'Go to Dashboard'))));
          }
          if (_DEBUG) console.error('renderContent async error:', err);
        });
      }
    }
    else if (_DEBUG) console.warn('Unknown tab:', tab);
  } catch (renderErr) {
    if ((window._navGeneration || 0) !== gen) { return; }
    else {
      PCV.uxlib.clearEl(b);
      b.appendChild(mk('div', { style: 'padding:40px;text-align:center' },
        mk('div', { style: 'font-size:48px;margin-bottom:12px' }, '⚠'),
        mk('h3', { style: 'color:var(--red)' }, _L('렌더링 오류', 'Rendering Error')),
        mk('p', { class: 'color-muted', style: 'margin:12px 0' }, renderErr.message || ''),
        mk('pre', { style: 'background:var(--bg);padding:12px;border-radius:6px;font-size:11px;color:var(--red);text-align:left;max-height:200px;overflow:auto' }, renderErr.stack || ''),
        mk('button', { class: 'btn', onclick: "navigateTo('dashboard')", style: 'margin-top:12px' }, _L('대시보드로', 'Go to Dashboard'))));
      if (_DEBUG) console.error('renderContent error:', renderErr);
    }
  }
  setTimeout(function() { if (cb) cb.classList.remove('fade-out'); }, 50);
  if (typeof updateStatusBar === 'function') updateStatusBar();
                                                                       
  if (window.PCV && PCV.shell && PCV.shell.setActive) PCV.shell.setActive(tab);
                                                                
                                                                   
                                                
  if (typeof applyRoleVisibility === 'function') {
    applyRoleVisibility(window.currentUser && window.currentUser.role);
  }
}
window.renderContent = renderContent;

                                              
                                                               
                                                                      
                                                                        
                                  
function updateStatusBar() {
  var el0 = document.getElementById('shell-sync');
  if (!el0) return;
  var ws = (typeof wsConnection !== 'undefined' && wsConnection && wsConnection.readyState === 1);
  var elapsed = Math.round((Date.now() - (lastLoadTime || Date.now())) / 1000);
  var mk = PCV.uxlib.el;
  PCV.uxlib.clearEl(el0);
  el0.appendChild(mk('span', { style: 'color:' + (ws ? 'var(--green)' : 'var(--red)') }, '●'));
  el0.appendChild(document.createTextNode(' ' + elapsed + 's'));
  if (typeof updateFavicon === 'function') updateFavicon();
}
window.updateStatusBar = updateStatusBar;
                                                           
setInterval(() => { if (document.hidden) return; updateStatusBar(); }, 2000);

                                              
                                                                
                                                           
                                        
                                                         
                                              
function updateFavicon() {
  var running = 0;
  if (vmList) running = vmList.filter(function(v) { return v.state === 'running'; }).length;

                        
  document.title = 'PureCVisor' + (running > 0 ? ' (' + running + ' VMs)' : '');

                                
  var canvas = document.createElement('canvas');
  canvas.width = 32; canvas.height = 32;
  var ctx = canvas.getContext('2d');
                         
  var color = running > 0 ? '#00ff88' : (vmList && vmList.length > 0 ? '#ffee00' : '#ff2266');
  ctx.beginPath(); ctx.arc(16, 16, 14, 0, Math.PI * 2);
  ctx.fillStyle = color; ctx.fill();
            
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

                                     
function toggleSB() {
  document.getElementById('shell-sidebar')?.classList.toggle('collapsed');
}
window.toggleSB = toggleSB;

                               
function toggleFS() {
  document.fullscreenElement ? document.exitFullscreen() : document.documentElement.requestFullscreen();
}
window.toggleFS = toggleFS;

                                            
                         
                                                                   
                                                                 
                                                    
                                                
                                                                    
var CMD_ACTIONS = PCV.filterEditionItems([
  { icon: 'ci-plus-circle', label: _L('새 VM', 'New VM'), hint: 'Ctrl+N', role: 'operator', action: () => showCreate() },
  { icon: 'ci-chart-line', label: _L('운영 개요', 'Operations Overview'), action: () => window.navigateTo('mon-overview') },
  { icon: 'ci-warning', label: _L('이벤트 센터', 'Event Center'), action: () => window.navigateTo('ops-triage') },
  { icon: 'ci-desktop', label: _L('VM 자산', 'VM Inventory'), action: () => window.navigateTo(localStorage.getItem('pcv-last-vm-tab') || 'summary') },
  { icon: 'ci-globe', label: _L('네트워크', 'Networks'), action: () => window.navigateTo('networks') },
  { icon: 'ci-layers', label: 'Local VPC', action: () => window.navigateTo('vpcs') },
  { icon: 'ci-data', label: _L('스토리지', 'Storage'), action: () => window.navigateTo('storage') },
  { icon: 'ci-layers', label: _L('컨테이너', 'Containers'), action: () => window.navigateTo('containers') },
  { icon: 'ci-bell', label: _L('알림', 'Alerts'), action: () => window.navigateTo('mon-alerts') },
  { icon: 'ci-shield-check', label: _L('보안 이벤트', 'Security Events'), action: () => window.navigateTo('mon-security') },
  { icon: 'ci-users', label: _L('계정과 권한', 'Accounts'), role: 'admin', action: () => window.navigateTo('accounts') },
  { icon: 'ci-terminal', label: _L('API 관리', 'API Management'), role: 'admin', action: () => window.navigateTo('apimgmt') },
  { icon: 'ci-terminal', label: 'Swagger API', action: () => window.navigateTo('apihelp') },
  { icon: 'ci-info', label: _L('도움말', 'Help'), action: () => window.navigateTo('helppage') },
  { icon: 'ci-settings', label: _L('AI Agent 설정', 'AI Agent Config'), role: 'admin', action: () => showAgentConfig() },
  { icon: 'ci-settings', label: _L('환경설정', 'Preferences'), action: () => showPrefs() },
  { icon: 'ci-monitor', label: _L('테마 전환', 'Toggle Theme'), action: () => toggleTheme() },
  { icon: 'ci-settings', label: _L('테마 편집기', 'Theme Editor'), action: () => openThemeEditor() },
  { icon: 'ci-globe', label: _L('언어 전환', 'Toggle Language'), action: () => { I18N.toggle(); location.reload(); } },
  { icon: 'ci-desktop', label: _L('전체 화면', 'Fullscreen'), hint: 'F11', action: () => toggleFS() },
]);
window.CMD_ACTIONS = CMD_ACTIONS;

                                    
                                                           
                                                 
                                                        
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
var cmdDialog = null;                                    

function openCmdPalette() {
                                                            
                                                                            
  if (cmdPaletteOpen && cmdDialog) {
    var i = document.getElementById('cmd-input');
    if (i) i.focus();
    return;
  }
  cmdPaletteOpen = true; cmdSelectedIndex = 0;
  window.cmdPaletteOpen = cmdPaletteOpen;
  window.cmdSelectedIndex = cmdSelectedIndex;
  var mk = PCV.uxlib.el;
  const box = mk('div', { id: 'cmd-palette', class: 'cmd-palette-box' },
    mk('input', {
      'aria-label': _L('명령·객체·IP 검색', 'Search commands, objects, IPs'),
      'aria-autocomplete': 'list',
      'aria-controls': 'cmd-list',
      'aria-describedby': 'cmd-help',
      'aria-expanded': 'true',
      role: 'combobox',
      id: 'cmd-input',
      placeholder: _L('명령·객체·IP 검색', 'Search commands, objects, IPs'),
      class: 'cmd-input',
      autocomplete: 'off'
    }),
    mk('div', {
      id: 'cmd-list',
      class: 'cmd-list',
      role: 'listbox',
      'aria-label': _L('검색 결과', 'Search results')
    }),
    mk('div', { id: 'cmd-help', class: 'cmd-help' },
      mk('span', null, mk('kbd', null, '↑↓'), ' ', _L('이동', 'Move')),
      mk('span', null, mk('kbd', null, 'Enter'), ' ', _L('선택', 'Select')),
      mk('span', null, mk('kbd', null, 'Esc'), ' ', _L('닫기', 'Close'))));
  var mine = PCV.modalCore.openBare(box, {
    dialogClass: 'cmd-palette',
    ariaLabel: _L('명령·객체·IP 검색', 'Search commands, objects, IPs'),
                                                                       
                                            
    onClose: function () { if (cmdDialog !== mine) return; cmdPaletteOpen = false; window.cmdPaletteOpen = false; cmdDialog = null; }
  });
  cmdDialog = mine;
  const inp = document.getElementById('cmd-input'); inp.focus();
  renderCmdPalette('');
  inp.addEventListener('input', () => { cmdSelectedIndex = 0; renderCmdPalette(inp.value); });
  inp.addEventListener('keydown', e => {
    const items = document.querySelectorAll('.cmd-item');
    if (e.key === 'ArrowDown') { e.preventDefault(); cmdSelectedIndex = Math.min(cmdSelectedIndex + 1, items.length - 1); window.cmdSelectedIndex = cmdSelectedIndex; renderCmdPalette(inp.value); }
    else if (e.key === 'ArrowUp') { e.preventDefault(); cmdSelectedIndex = Math.max(cmdSelectedIndex - 1, 0); window.cmdSelectedIndex = cmdSelectedIndex; renderCmdPalette(inp.value); }
    else if (e.key === 'Enter') { e.preventDefault(); if (items[cmdSelectedIndex]) items[cmdSelectedIndex].click(); }
  });
}
window.openCmdPalette = openCmdPalette;

                                                                   
                                                                
                                                             
                                                                 
function closeCmdPalette() {
  if (cmdDialog) { var d = cmdDialog; cmdDialog = null; try { d.close(); } catch {} }
  cmdPaletteOpen = false;
  window.cmdPaletteOpen = false;
}
window.closeCmdPalette = closeCmdPalette;

                                                                  
                                                         
                    
function _cmdIcon(symbol) {
  var ns = 'http://www.w3.org/2000/svg';
  var svg = document.createElementNS(ns, 'svg');
  svg.setAttribute('class', 'ci-icon cmd-item-icon');
  svg.setAttribute('aria-hidden', 'true');
  svg.setAttribute('focusable', 'false');
  var use = document.createElementNS(ns, 'use');
  use.setAttribute('href', 'vendor/coolicons/coolicons.svg#' + symbol);
  svg.appendChild(use);
  return svg;
}

                                                           
                              
                                                                         
function _cmdPalettePages() {
  return PCV.filterEditionItems([
    { id: 'networks', label: _L('네트워크', 'Networks'), icon: 'ci-globe' },
    { id: 'vpcs', label: 'Local VPC', icon: 'ci-layers' },
    { id: 'storage', label: _L('스토리지', 'Storage'), icon: 'ci-data' },
    { id: 'containers', label: _L('컨테이너', 'Containers'), icon: 'ci-layers' },
    { id: 'mon-overview', label: _L('운영 개요', 'Monitoring Overview'), icon: 'ci-chart-line' },
    { id: 'ops-triage', label: _L('이벤트 센터', 'Event Center'), icon: 'ci-warning' },
    { id: 'mon-alerts', label: _L('알림', 'Alerts'), icon: 'ci-bell' },
    { id: 'mon-security', label: _L('보안 이벤트', 'Security Events'), icon: 'ci-shield-check' },
    { id: 'accounts', label: _L('계정과 권한', 'Accounts'), icon: 'ci-users', role: 'admin' },
    { id: 'security-groups', label: _L('보안 그룹', 'Security Groups'), icon: 'ci-shield' },
    { id: 'gpu', label: _L('GPU 장치', 'GPU'), icon: 'ci-desktop-tower' },
    { id: 'apihelp', label: _L('Swagger API', 'Swagger API'), icon: 'ci-terminal' },
    { id: 'overlay', label: _L('오버레이 네트워크', 'Overlay Networks'), icon: 'ci-cloud' },
    { id: 'iscsi', label: _L('iSCSI 타깃', 'iSCSI Targets'), icon: 'ci-data' },
    { id: 'dpdk', label: 'DPDK', icon: 'ci-terminal' },
    { id: 'sriov', label: 'SR-IOV', icon: 'ci-layers' },
    { id: 'pool-info', label: _L('커넥션 풀', 'Connection Pool'), icon: 'ci-chart-line' },
  ]);
}

                                                          
                                      
                                                                
                                                
function renderCmdPalette(filter) {
  const list = document.getElementById('cmd-list'); if (!list) return;
  const q = (filter || '').toLowerCase();
  var mk = PCV.uxlib.el;
  PCV.uxlib.clearEl(list);
  var idx = 0;

  function _group(title) {
    list.appendChild(mk('div', {
      class: 'cmd-group',
      style: 'padding:8px 16px 3px;font-size:10px;color:var(--fg2);text-transform:uppercase;letter-spacing:.06em;font-weight:600'
    }, title));
  }
                                                                 
  function _item(icon, label, hint, handler) {
    var i = idx++;
    var attrs = {
      class: 'cmd-item' + (i === cmdSelectedIndex ? ' active' : ''),
      id: 'cmd-option-' + i,
      role: 'option',
      tabindex: '-1',
      'aria-selected': i === cmdSelectedIndex ? 'true' : 'false'
    };
    if (typeof handler === 'string') attrs.onclick = handler; else attrs.onClick = handler;
    list.appendChild(mk('div', attrs,
      _cmdIcon(icon),
      mk('span', { class: 'cmd-item-label' }, label),
      hint ? mk('span', { class: 'cmd-item-hint' }, hint) : null));
  }

  function _syncActiveDescendant() {
    var input = document.getElementById('cmd-input');
    var selected = list.querySelector('[role="option"][aria-selected="true"]');
    if (!input) return;
    if (selected) {
      input.setAttribute('aria-activedescendant', selected.id);
                                                              
                                                           
                                                            
                          
      selected.scrollIntoView({ block: 'nearest', inline: 'nearest' });
    }
    else input.removeAttribute('aria-activedescendant');
  }

                
  const cmds = CMD_ACTIONS.filter(a => {
    if (a.cluster && !window.pcvClusterEnabled) return false;
    if (a.role && !pcvRoleAllows(a.role)) return false;
    return !q || fuzzyMatch(a.label, q);
  });
  if (q && cmds.length) _group(_L('명령', 'Commands'));
  cmds.forEach(function (a) {
    _item(a.icon, a.label, a.hint,
      "CMD_ACTIONS.find(x=>x.label==='" + a.label.replace(/'/g, "\\'") + "').action();closeCmdPalette()");
  });

  if (!q) { _syncActiveDescendant(); return; }

                                                                                 
                                                                  
                                                            
                                                  
  var fleet = (window._shellSlow && _shellSlow.raw && _shellSlow.raw.fleet) || [];
  var fleetIpByName = {};
  fleet.forEach(function (f) {
    if (f && f.name && f.ip && f.ip !== 'N/A') fleetIpByName[f.name] = f.ip;
  });
  var vms = (window.vmList || []).filter(function (v) {
    if (!v || !v.name) return false;
    if (fuzzyMatch(v.name, q)) return true;
    var ip = fleetIpByName[v.name];
    return !!ip && ip.toLowerCase().indexOf(q.toLowerCase()) !== -1;
  });
  if (vms.length) {
    _group(_L('가상 머신', 'Virtual Machines'));
    vms.forEach(function (v) {
      _item('ci-desktop', v.name, fleetIpByName[v.name] || v.state || null, function () {
        closeCmdPalette();
                                                           
                                          
        var i = (window.vmList || []).findIndex(function (x) { return x && x.name === v.name; });
        if (i >= 0) window.selectedVmIndex = i;
        window.navigateTo(localStorage.getItem('pcv-last-vm-tab') || 'summary');
      });
    });
  }

                                                              
  var ctrs = (window._shellSlow && _shellSlow.raw && _shellSlow.raw.ctrs) || [];
  var mctrs = ctrs.filter(function (c) {
    if (!c || !c.name) return false;
    var ip = String(c.ip_addr || c.ip || '').toLowerCase();
    return fuzzyMatch(c.name, q) || (!!ip && ip.indexOf(q) !== -1);
  });
  if (mctrs.length) {
    _group(_L('컨테이너', 'Containers'));
    mctrs.forEach(function (c) {
      _item('ci-layers', c.name, String(c.ip_addr || c.ip || '') || (c.state || null), function () {
        closeCmdPalette();
        window.selCtr = c.name;
        window.ctrTab = 'summary';
        window.navigateTo('containers');
      });
    });
  }

                                                      
                                                                          
                                                                            
  var pools = (window._shellSlow && _shellSlow.raw && _shellSlow.raw.pools) || [];
  var mpools = pools.filter(function (p) { return p && p.name && fuzzyMatch(p.name, q); });
  if (mpools.length) {
    _group(_L('풀', 'Pools'));
    mpools.forEach(function (p) {
      _item('ci-data', p.name, p.health || p.state || null, function () {
        closeCmdPalette();
        window.navigateTo('storage');
      });
    });
  }

                                                                              
                                                 
  var networks = (window._shellSlow && _shellSlow.raw && _shellSlow.raw.networks) || [];
  var mnets = networks.filter(function (n) { return n && n.name && fuzzyMatch(n.name, q); });
  if (mnets.length) {
    _group(_L('네트워크', 'Networks'));
    mnets.forEach(function (n) {
      _item('ci-globe', n.name, n.mode || n.state || null, function () {
        closeCmdPalette();
        window.navigateTo('networks');
      });
    });
  }

                 
  var pages = _cmdPalettePages().filter(function (p) {
    if (p.cluster && !window.pcvClusterEnabled) return false;
    if (p.role && !pcvRoleAllows(p.role)) return false;
    return fuzzyMatch(p.label, q);
  });
  if (pages.length) {
    _group(_L('페이지', 'Pages'));
    pages.forEach(function (p) {
      _item(p.icon, p.label, null, function () { closeCmdPalette(); window.navigateTo(p.id); });
    });
  }

  if (idx === 0) {
    list.appendChild(PCV.uxlib.msg('muted', { tag: 'div', style: 'padding:20px;text-align:center;font-size:12px' },
      _L('결과 없음', 'No results')));
  }
  _syncActiveDescendant();
}
window.renderCmdPalette = renderCmdPalette;

                    
function toggleMobileSB() {
  var sb = document.getElementById('shell-sidebar'), ov = document.getElementById('mobile-overlay');
  if (!sb) return;
  var open = sb.classList.toggle('mobile-open');
  if (ov) ov.style.display = open ? 'block' : 'none';
}
window.toggleMobileSB = toggleMobileSB;

function closeMobileSB() {
  var sb = document.getElementById('shell-sidebar'), ov = document.getElementById('mobile-overlay');
  if (sb) sb.classList.remove('mobile-open');
  if (ov) ov.style.display = 'none';
}
window.closeMobileSB = closeMobileSB;

                                           
                                                          
                                                                 
                                            
function toggleGlobalSearch() {
  if (cmdPaletteOpen && cmdDialog) { closeCmdPalette(); return; }
  openCmdPalette();
}
window.toggleGlobalSearch = toggleGlobalSearch;

                                       
var notifications = [];
window.notifCenterOpen = false;
var notifFilter = 'all';

                                                  
                                                         
var NOTIF_SVG_NS = 'http://www.w3.org/2000/svg';
function _notifIcon(symbol, cls) {
  var svg = document.createElementNS(NOTIF_SVG_NS, 'svg');
  svg.setAttribute('class', 'ci-icon' + (cls ? ' ' + cls : ''));
  svg.setAttribute('aria-hidden', 'true');
  svg.setAttribute('focusable', 'false');
  var use = document.createElementNS(NOTIF_SVG_NS, 'use');
  use.setAttribute('href', 'vendor/coolicons/coolicons.svg#' + symbol);
  svg.appendChild(use);
  return svg;
}

function _notifTypeMeta(type) {
  if (type === 'error') {
    return { icon: 'ci-close-circle', label: _L('오류', 'Error') };
  }
  if (type === 'warning') {
    return { icon: 'ci-warning', label: _L('경고', 'Warning') };
  }
  return { icon: 'ci-info', label: _L('정보', 'Info') };
}

function _notifItemAria(n, typeLabel) {
  var state = n.read
    ? _L('확인한 알림.', 'Read notification.')
    : _L('미확인 알림. 선택하면 확인 처리됩니다.', 'Unread notification. Activate to mark read.');
  return [state, typeLabel + '.', n.title, n.msg || '', _notifTimeAgo(n.time)]
    .filter(Boolean).join(' ');
}

   
                                                      
  
                                                  
                                                         
                                                                  
                                       
                                                            
                       
   
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
                         
  var toolbarBadge = document.getElementById('notif-toolbar-badge');
  if (toolbarBadge) { toolbarBadge.textContent = unread; toolbarBadge.style.display = unread > 0 ? '' : 'none'; }
                                                      
                                           
  var summary = document.getElementById('notif-unread-summary');
  if (summary) {
    summary.textContent = unread + _L('개 미확인', ' unread');
    summary.hidden = unread === 0;
  }
}
window.updateNotifBadge = updateNotifBadge;

                                                       
                                                         
                                                                       
function _renderNotifList(container, filter) {
  var filtered = notifications;
  if (filter && filter !== 'all') {
    filtered = notifications.filter(function(n) { return n.type === filter; });
  }
  var mk = PCV.uxlib.el;
  if (filtered.length === 0) {
    var emptyMeta = filter === 'all' ? null : _notifTypeMeta(filter);
    var emptyText = emptyMeta
      ? _L(
          emptyMeta.label + ' 알림이 없습니다',
          'No ' + emptyMeta.label.toLowerCase() + ' notifications'
        )
      : _L('알림이 없습니다', 'No notifications');
    PCV.uxlib.clearEl(container);
    container.appendChild(mk('div', { class: 'notif-center-empty' },
      _notifIcon('ci-bell', 'notif-empty-icon'),
      mk('span', null, emptyText)));
    return;
  }
  var items = filtered.map(function(n) {
    var meta = _notifTypeMeta(n.type);
    var timeAgo = _notifTimeAgo(n.time);
    var item = mk('button', {
      type: 'button',
      class: 'notif-item notif-item-' + (n.type || 'info') + (n.read ? '' : ' unread'),
      'aria-label': _notifItemAria(n, meta.label)
    },
      mk('span', { class: 'notif-icon' }, _notifIcon(meta.icon)),
      mk('div', { class: 'notif-body' },
        mk('div', { class: 'notif-title-row' },
          mk('span', { class: 'notif-title' }, n.title),
          mk('span', { class: 'notif-type' }, meta.label)),
        n.msg ? mk('div', { class: 'notif-msg' }, n.msg) : null,
        mk('div', { class: 'notif-time' }, timeAgo)));
    item.addEventListener('click', function() {
      markRead(n.id);
      item.classList.remove('unread');
      item.setAttribute('aria-label', _notifItemAria(n, meta.label));
    });
    return item;
  });
  PCV.uxlib.clearEl(container);
  items.forEach(function(it) { container.appendChild(it); });
}

function _notifTimeAgo(date) { return PCV.uxlib.formatRelativeTime(date); }

                                                                 
                                                  
function _renderNotifCenterContent(el0) {
  var mk = PCV.uxlib.el;
  var unread = notifications.filter(function(n) { return !n.read; }).length;
  PCV.uxlib.clearEl(el0);

                                     
  var header = mk('div', { class: 'notif-center-header' },
    mk('div', { class: 'notif-center-heading' },
      mk('h2', { id: 'notif-center-title', class: 'notif-center-title' },
        _L('알림', 'Notifications')),
      mk('span', {
        id: 'notif-unread-summary',
        class: 'notif-header-badge',
        hidden: unread > 0 ? null : ''
      }, unread + _L('개 미확인', ' unread'))),
    mk('div', { class: 'notif-center-actions' },
      mk('button', {
        class: 'btn notif-header-action',
        type: 'button',
        onClick: markAllRead
      }, _L('모두 확인', 'Mark all read')),
      mk('button', {
        class: 'btn btn-r notif-header-action',
        type: 'button',
        onClick: clearNotifications
      }, _L('지우기', 'Clear')),
      mk('button', {
        class: 'panel-action-btn notif-close',
        type: 'button',
        'aria-label': _L('알림 센터 닫기', 'Close notification center'),
        onClick: closeNotifCenter
      }, _notifIcon('ci-close-circle'))));

                                            
  function _fbtn(f, label) {
    return mk('button', {
      class: 'notif-filter-btn' + (notifFilter === f ? ' active' : ''),
      type: 'button',
      'data-filter': f,
      'aria-pressed': notifFilter === f ? 'true' : 'false',
      onClick: function() { setNotifFilter(f); }
    }, label);
  }
  var filterBar = mk('div', {
    class: 'notif-filter-bar',
    role: 'group',
    'aria-label': _L('알림 유형 필터', 'Notification type filter')
  },
    _fbtn('all', _L('전체', 'All') + ' (' + notifications.length + ')'),
    _fbtn('error', _L('오류', 'Error') + ' (' + notifications.filter(function(n){ return n.type==='error'; }).length + ')'),
    _fbtn('warning', _L('경고', 'Warning') + ' (' + notifications.filter(function(n){ return n.type==='warning'; }).length + ')'),
    _fbtn('info', _L('정보', 'Info') + ' (' + notifications.filter(function(n){ return n.type==='info'; }).length + ')'));

  el0.appendChild(header);
  el0.appendChild(filterBar);
  var listContainer = mk('div', { class: 'notif-center-list', id: 'notif-list-container' });
  el0.appendChild(listContainer);
  _renderNotifList(listContainer, notifFilter);
}

   
                                      
  
                                                               
                                                                        
                                                                       
                                                    
                                   
                                                                        
                                            
  
                                                           
                                                                     
                                                                      
   
function toggleNotifCenter() {
  if (window.notifCenterOpen) { closeNotifCenter(); return; }
  window.notifCenterOpen = true;
  notifFilter = 'all';
  var el = PCV.uxlib.el('div', {
    id: 'notif-center',
    class: 'notif-center',
    popover: 'auto',
    role: 'region',
    'aria-labelledby': 'notif-center-title'
  });
                                                                            
                                  
  el.addEventListener('toggle', function (e) {
    if (e.newState === 'closed') { window.notifCenterOpen = false; if (el.parentNode) el.remove(); }
  });
  document.body.appendChild(el);
  _renderNotifCenterContent(el);
  el.showPopover();
}
window.toggleNotifCenter = toggleNotifCenter;

function setNotifFilter(filter) {
  notifFilter = filter;
                                   
  document.querySelectorAll('.notif-filter-btn').forEach(function(btn) {
    var active = btn.dataset.filter === filter;
    btn.classList.toggle('active', active);
    btn.setAttribute('aria-pressed', active ? 'true' : 'false');
  });
  var listContainer = document.getElementById('notif-list-container');
  if (listContainer) _renderNotifList(listContainer, filter);
}
window.setNotifFilter = setNotifFilter;

function closeNotifCenter() {
  var el = document.getElementById('notif-center');
  if (el) { try { el.hidePopover(); } catch {} }                                           
  window.notifCenterOpen = false;                                   
}
window.closeNotifCenter = closeNotifCenter;

function markAllRead() {
  notifications.forEach(function(n) { n.read = true; });
  updateNotifBadge();
  var el = document.getElementById('notif-center');
  if (el) _renderNotifCenterContent(el);                                                
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
  var el = document.getElementById('notif-center');
  if (el) _renderNotifCenterContent(el);                     
}
window.clearNotifications = clearNotifications;

                             
var hoverCardTimeout = null;
var hoverCard = document.createElement('div');
hoverCard.className = 'hover-card';
hoverCard.id = 'hover-card';
document.body.appendChild(hoverCard);

function showHoverCard(e, vm) {
  clearTimeout(hoverCardTimeout);
  hoverCardTimeout = setTimeout(() => {
    const on = vm.state === 'running';
    var mk = PCV.uxlib.el;
    PCV.uxlib.clearEl(hoverCard);
    hoverCard.appendChild(mk('div', { class: 'hover-card-title' },
      mk('span', { class: 'dot ' + (on ? 'on' : 'off') }),
      vm.name));
    hoverCard.appendChild(mk('div', { class: 'hover-card-row' },
      mk('span', { class: 'hc-k' }, 'State'),
      mk('span', { class: 'hc-v' }, vm.state || '-')));
    hoverCard.appendChild(mk('div', { class: 'hover-card-row' },
      mk('span', { class: 'hc-k' }, 'vCPU'),
      mk('span', { class: 'hc-v' }, vm.vcpu || '-')));
    hoverCard.appendChild(mk('div', { class: 'hover-card-row' },
      mk('span', { class: 'hc-k' }, 'Memory'),
      mk('span', { class: 'hc-v' }, (vm.memory_mb || '-') + ' MB')));
    hoverCard.appendChild(mk('div', { class: 'hover-card-row' },
      mk('span', { class: 'hc-k' }, 'CPU'),
      mk('span', { class: 'hc-v' }, (vm.live_cpu_pct || 0).toFixed(1) + '%')));
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

                                         
                                                           
                                                            
                                                        
                                                   
                                       
function highlightText(text, query) {
  if (!query || !text) return esc(text);
  var escaped = esc(text);
  var re = new RegExp('(' + query.replace(/[.*+?^${}()|[\]\\]/g, '\\$&') + ')', 'gi');
  return escaped.replace(re, '<mark style="background:var(--yellow);color:#000;padding:0 2px;border-radius:2px">$1</mark>');
}
window.highlightText = highlightText;

                                
                                                                
                               
                                                          
                                                                
                                                                       
                                      
                                                           
           
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
      navigateTo(tabs[idx + 1]);
    } else if (dx > 50 && idx > 0) {
      navigateTo(tabs[idx - 1]);
    }
  }, { passive: true });

                                   
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

                                                    
                                                                 
                                                   
                
  
                                                              
                                                                  
                                                               
                             
function _renderVersionBadge(d) {
  var badge = document.getElementById('version-badge');
  if (!badge) return;
  var el = PCV.uxlib.el, clearEl = PCV.uxlib.clearEl;
  clearEl(badge);
  badge.classList.remove('is-update');
  badge.removeAttribute('hidden');
  badge.onclick = null; badge.title = ''; badge.style.cursor = '';
  if (!d || !d.current) { badge.setAttribute('hidden', ''); return; }
  var ok = d.state === 'ok';
  if (ok && d.update_available) {
    badge.classList.add('is-update');
    badge.appendChild(el('span', null, '↑ v' + d.latest));
    badge.title = _L('업데이트 가능', 'Update available') + ': v' + d.latest;
    if (d.url && d.url.indexOf('https://github.com/') === 0) {
      badge.style.cursor = 'pointer';
      badge.onclick = function () { window.open(d.url, '_blank', 'noopener'); };
    }
  } else if (ok) {
    badge.appendChild(el('span', null, '✓ v' + d.current));
    badge.title = _L('최신 버전', 'Up to date');
  } else {
    badge.appendChild(el('span', null, 'v' + d.current));                       
  }
}
function updateVersionBadge() {
  fetchGet(EP.UPDATE_CHECK()).then(function (r) {
    _renderVersionBadge(r && r.payload ? r.payload : r);
  }).catch(function () {
    var b = document.getElementById('version-badge'); if (b) b.setAttribute('hidden', '');
  });
}
window.updateVersionBadge = updateVersionBadge;

function checkForUpdates() {
  fetchGet(EP.UPDATE_CHECK()).then(function (r) {
    var d = r && r.payload ? r.payload : r;
    if (!d || d.state === 'disabled') { toast(_L('버전 확인이 비활성화됨', 'Version check disabled')); return; }
    if (d.state !== 'ok') { toast(_L('최신 버전 정보를 가져올 수 없습니다', 'Could not fetch latest version')); return; }
    if (d.update_available) {
      toast(_L('업데이트 가능', 'Update available') + ': v' + d.latest + ' (현재 v' + d.current + ')');
    } else {
      toast(_L('최신 버전입니다', 'You are up to date') + ' (v' + d.current + ')');
    }
    _renderVersionBadge(d);
  }).catch(function () { toast(_L('버전 확인 실패', 'Version check failed'), false); });
}
window.checkForUpdates = checkForUpdates;

                                                         
PCV.nav = {
  updateVersionBadge: updateVersionBadge,
  checkForUpdates: checkForUpdates,
  navigateTo: navigateTo,
  renderContent: renderContent,
  updateStatusBar: updateStatusBar,
  updateFavicon: updateFavicon,
  toggleSB: toggleSB,
  toggleFS: toggleFS,
  CMD_ACTIONS: CMD_ACTIONS,
  fuzzyMatch: fuzzyMatch,
  openCmdPalette: openCmdPalette,
  closeCmdPalette: closeCmdPalette,
  renderCmdPalette: renderCmdPalette,
  toggleMobileSB: toggleMobileSB,
  closeMobileSB: closeMobileSB,
  toggleGlobalSearch: toggleGlobalSearch,
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
