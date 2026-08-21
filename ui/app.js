                                                                  
                                    
                           
                                         

                       
                                                  
                                                  
                                                                     

  
                                                    
  
       
                                                     
                                                  
  
                                                           
                                                                     
                                                                   
                                                        
                                         
  
                             
                                                                    
                          
                                                       
                                                     
                                                             
                                                        
                                                          
                     
  
              
                                               
                                              
                                                                    
                                                                       
                                                          
                                                             
                                  
  
          
                                                                        
                                         
                                                              
                                                             
  
          
                                                          
                                                              
                                                              
                                  
                                                  
                                                           
  
                       
                                                   
                                                   
   

                                                                              

                                                 
  
                                                                    
                                                                     
                                                                    
                                                           
                                                            
                                         
  
                    
                                                              
                                                               
                                                               
                                                                     
                                                             
                                                              
                                                              
                                         
   

                                                 
                                                            
                                               
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
var checkedVms = new Set();
var eventLog = [];
var lastLoadTime = Date.now();

                                                     
                                                                                    
  
                           
                                                        
                                                                      
                                              
                                            
  
                         
                                                
                                                     
   
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
  VERSION: '2.0.0',
                                                           
                                                                          
                                                                     
                                                          
                                           
  RPC_COUNT: 304,
  REST_COUNT: 229,
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
    }
  }).catch(function() {});
}
                                   
applyEditionCapabilities();

                                           
                                                          
                                                   
                                                 

                       
                                                    
                                                     

                                                                 
                                             
                                                   
                                                      
                                                        
                                                                     
                                                           
                                   
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

                                             
  
                                                      
                                         
  
                  
                                                                  
                                                       
                                                
                          
  
                                                                   
                                                                         
                                                              
                                           
  
                                                               
                                                    
   
function _loginVisible() {
  var page = document.getElementById('login-page');
  return !!page && page.style.display !== 'none';
}

function _loginFmtUptime(sec) {
  var s = Math.floor(Number(sec));
  if (!isFinite(s) || s < 0) return '—';
  var d = Math.floor(s / 86400);
  var h = Math.floor((s % 86400) / 3600);
  var m = Math.floor((s % 3600) / 60);
  if (d > 0) return d + 'd ' + h + 'h';
  if (h > 0) return h + 'h ' + m + 'm';
  return m + 'm';
}

                                                              
                                                         
                                                   
                                                          
                                   
function _loginSetText(id, text) {
  var node = document.getElementById(id);
  if (node) node.textContent = text;
}

function _loginSetDot(id, state) {
  var node = document.getElementById(id);
  if (node) node.className = 'login-dot login-dot-' + state;
}

                                               
function _loginHealthIdle() {
  _loginSetDot('lh-dot', 'idle');
  _loginSetText('lh-state', '노드 응답 대기');
  _loginSetText('lh-node', '—');
  _loginSetText('lh-version', '—');
  _loginSetText('lh-uptime', '—');
  _loginSetDot('lh-kvm-dot', 'idle');
  _loginSetText('lh-kvm', '—');
  _loginSetDot('lh-disk-dot', 'idle');
  _loginSetText('lh-disk', '—');
}

function _loginHealthTick() {
  if (!_loginVisible() || document.hidden) return Promise.resolve(false);
  if (typeof fetchGet !== 'function') return Promise.resolve(false);
  return Promise.resolve().then(function() {
    return fetchGet(window.API_BASE + '/health');
  }).then(function(h) {
    if (!h || typeof h.status !== 'string') { _loginHealthIdle(); return false; }
    var crit = h.status === 'critical';
    _loginSetDot('lh-dot', crit ? 'crit' : 'ok');
    _loginSetText('lh-state', crit ? '점검 필요' : '정상 운영');
    _loginSetText('lh-node', h.node_name || '—');
    _loginSetText('lh-version', h.version ? 'v' + h.version : '—');
    _loginSetText('lh-uptime', _loginFmtUptime(h.uptime_sec));
    var checks = h.checks || {};
    var kvm = checks.libvirt;
    _loginSetDot('lh-kvm-dot', kvm ? (kvm.ok ? 'ok' : 'crit') : 'idle');
    _loginSetText('lh-kvm', kvm ? (kvm.ok ? '연결됨' : '연결 실패') : '—');
    var disk = checks.disk;
    _loginSetDot('lh-disk-dot', disk ? (disk.ok ? 'ok' : 'crit') : 'idle');
    _loginSetText('lh-disk', disk && typeof disk.avail_gb === 'number'
      ? disk.avail_gb.toFixed(1) + ' GB 여유'
      : (disk ? (disk.ok ? '정상' : '부족') : '—'));
    return true;
  }, function() { _loginHealthIdle(); return false; });
}
window._loginHealthTick = _loginHealthTick;

_loginHealthTick();
setInterval(_loginHealthTick, 30000);

                         

                       

                                   

window.addEventListener('DOMContentLoaded', () => {
                                                            
                                               
                        
  const ALLOWED = ['supanova', 'supanova-cyan', 'supanova-hicontrast', 'supanova-mockup'];
  const urlTheme = new URLSearchParams(window.location.search).get('theme');
                                                       
                                                             
                                                            
                                                                          
                                                                     
                                                  
                                                                     
                                  
  if (!urlTheme && document.documentElement.getAttribute('data-theme') === 'custom'
      && localStorage.getItem('pcv-theme') === 'custom') return;
  let t = urlTheme || localStorage.getItem('pcv-theme') || 'supanova';
  if (ALLOWED.indexOf(t) < 0) t = 'supanova';
  document.documentElement.setAttribute('data-theme', t);
  localStorage.setItem('pcv-theme', t);
  const s = document.getElementById('theme-select');
  if (s) s.value = t;
});

                                                                 

                     

                                       
                                                                     
                                                    
  
                                                                         
                                                                   
                                                                          
                                
                                                  
                                                          
                                      
  
                                                                  
                                                          
                                                      
                                                 
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
  const curTheme = document.documentElement.dataset.theme || '';
  var previews = THEME_PREVIEWS.map(tp => {
    const sel = tp.id === curTheme;
    return el('div', {
                                                           
                                     
      onclick: 'changeTheme(\'' + tp.id + '\');showPrefs()',
      style: 'cursor:pointer;padding:8px;border-radius:8px;border:2px solid ' + (sel ? 'var(--accent)' : 'var(--border)') + ';background:var(--bg2);text-align:center' + (sel ? ';box-shadow:0 0 8px var(--accent)' : '')
    },
      el('div', { style: 'display:flex;gap:3px;justify-content:center;margin-bottom:6px' },
        tp.colors.map(c => el('div', { style: 'width:20px;height:20px;border-radius:4px;background:' + c + ';border:1px solid rgba(255,255,255,0.1)' }))),
      el('div', { style: 'font-size:9px;color:var(--fg2);white-space:nowrap' }, tp.name));
  });
                                                                        
                                                                  
                                                             
  var themeOptions = THEME_PREVIEWS.map(tp => el('option', { value: tp.id }, tp.name));
  if (curTheme === 'custom') {
                                                              
                                                                        
                                                            
    themeOptions.push(el('option', { value: 'custom', disabled: '' }, 'CUSTOM'));
  }
  var themeSel = el('select', { id: 'theme-select', class: 'theme-select', onchange: 'changeTheme(this.value)', 'aria-label': 'Theme selector' },
    themeOptions);
  themeSel.value = curTheme;
                                                  
                                                             
                                                 
  var langSel = el('select', { id: 'lang-select', class: 'theme-select', 'aria-label': 'Language selector',
    onchange: "I18N.setLang(this.value);if(typeof applyI18n==='function'){applyI18n();if(window.PCV&&PCV.shell)PCV.shell.mount();if(typeof renderContent==='function'){try{renderContent();}catch(e){}}showPrefs();}else location.reload()" },
    el('option', { value: 'ko' }, '한국어'),
    el('option', { value: 'en' }, 'English'));
  if (typeof I18N !== 'undefined' && I18N.getLang) langSel.value = I18N.getLang();
  var netDevOptions = [el('option', { value: '' }, _L('자동(최대 트래픽)', 'Auto (highest traffic)'))]
    .concat((_netDevices || []).map(function (d) { return el('option', { value: d }, d); }));
  var netDevSel = el('select', { id: 'net-device-select', class: 'theme-select', 'aria-label': 'Network device selector',
    onchange: "localStorage.setItem('pcv-net-device', this.value)" },
    netDevOptions);
  netDevSel.value = localStorage.getItem('pcv-net-device') || '';
  showModal([
    el('h2', null, 'Preferences'),
    el('div', { class: 'fr' },
      el('label', { for: 'app-default-pool' }, 'Default Pool'),
      el('input', { id: 'app-default-pool', value: 'pcvpool/vms', disabled: '' })),
    el('div', { class: 'fr' },
      el('label', { for: 'app-api-port' }, 'API Port'),
      el('input', { id: 'app-api-port', value: '8080', disabled: '' })),
    el('div', { class: 'fr' },
      el('label', { for: 'theme-select' }, 'Theme'),
      themeSel),
    el('div', { class: 'fr' },
      el('label', { for: 'lang-select' }, 'Language'),
      langSel),
    el('div', { class: 'fr' },
      el('label', { for: 'net-device-select' }, _L('네트워크 타일 디바이스', 'Network Tile Device')),
      netDevSel),
                                                        
                                                   
                                                      
                                           
    el('div', { style: 'margin:14px 0;border-top:1px solid var(--border);padding-top:12px' },
      el('h4', { style: 'margin-bottom:8px', role: 'heading', 'aria-level': '3' }, _L('알림', 'Notifications')),
      (PCV.push && PCV.push.toggleNode)
        ? PCV.push.toggleNode({ id: 'push-toggle' })
        : PCV.uxlib.msg('muted', { size: '12px' },
            _L('푸시 모듈을 불러오지 못했습니다.', 'Push module is unavailable.'))),
    el('div', { style: 'margin:12px 0' },
      el('label', { style: 'font-size:12px;color:var(--fg2)' }, 'Theme Preview'),
      el('div', { style: 'display:grid;grid-template-columns:repeat(auto-fill,minmax(120px,1fr));gap:8px;margin-top:8px' }, previews)),
                                                                 
    el('div', { style: 'margin:14px 0;border-top:1px solid var(--border);padding-top:12px' },
      el('h4', { style: 'margin-bottom:8px', role: 'heading', 'aria-level': '3' }, 'Configuration Management'),
      el('div', { class: 'flex gap-6' },
        el('button', { class: 'btn btn-g', onclick: 'configBackup()', 'data-role': 'ADMIN' }, '💾 Backup Config'),
        el('button', { class: 'btn', onclick: 'configHistory()' }, '📋 Config History'))),
    el('div', { class: 'flex gap-6 mt-12' },
      el('button', { class: 'btn', onclick: 'exportUiSettings()' }, _L('설정 내보내기', 'Export Settings')),
      el('button', { class: 'btn', onclick: 'importUiSettings()' }, _L('설정 가져오기', 'Import Settings'))),
    el('div', { style: 'text-align:right;margin-top:12px' },
      el('button', { class: 'btn', onclick: 'openThemeEditor()', style: 'margin-right:8px' }, 'Theme Editor'),
      el('button', { class: 'btn btn-r', onclick: 'closeModal()' }, t('btn.close')))
  ], { replace: true });                                                           
  if (typeof applyRoleVisibility === 'function') applyRoleVisibility(window.currentUser && window.currentUser.role);
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
                                                            
                                                                          
                                              
    const r = await fetchPost(EP.OVN_ACL(), { 'switch': sw, direction: dir, priority: parseInt(pri), match: match, action: act });
                                                           
                                                 
    if (r && r.error) {
      const emsg = r.error.message || 'ACL 규칙 추가 실패';
      if (el) PCV.uxlib.setMsg(el, 'err', null, 'ACL 규칙 추가 실패: ' + emsg);
      toast('ACL 규칙 추가 실패: ' + emsg, false);
      return;
    }
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
    const r = await fetchGet(EP.OVN_ACL() + '?switch=' + encodeURIComponent(sw));
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
                                                      
                                                             
                                                        
  if (el) PCV.uxlib.setMsg(el, 'warn', { size: '12px' },
    'VFIO 패스스루는 이 빌드에서 지원하지 않습니다 (REST·RPC·CLI 모두 없음). 조회만 가능: ',
    PCV.uxlib.el('code', null, 'pcvctl gpu list'));
};

window.gpuMdevCreate = async function() {
  const el = document.getElementById('gpu-action-result');
  const pci = document.getElementById('gpu-pci')?.value;
  if (!pci) { if (el) PCV.uxlib.setMsg(el, 'err', null, 'PCI 주소를 입력하세요'); return; }
                                         
                                                        
  if (el) PCV.uxlib.setMsg(el, 'warn', { size: '12px' },
    'vGPU(mdev) 생성은 이 빌드에서 지원하지 않습니다 (REST·RPC·CLI 모두 없음). 조회만 가능: ',
    PCV.uxlib.el('code', null, 'pcvctl gpu list'));
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
        mk('td', null, ''));                                            
    });
    var table = mk('table', { class: 'tbl', style: 'font-size:11px' },
      mk('thead', null, mk('tr', null,
        mk('th', null, 'URL'), mk('th', null, 'Payload'), mk('th', null, _L('시각', 'Time')), mk('th'))),
      mk('tbody', null, rows));
                                              
                                                            
                                              
    PCV.uxlib.clearEl(el);
    el.appendChild(table);
  } catch (e) {
    PCV.uxlib.setMsg(el, 'warn', { tag: 'div', cls: 'stat-label' }, _L('DLQ 조회 불가', 'DLQ unavailable'));
  }
};

window.retryWebhookDlq = async function() {
  const el = document.getElementById('dlq-list');
  if (el) PCV.uxlib.setMsg(el, 'loading', null, '재시도 중...');
  try {
    var r = await fetchPost(EP.WEBHOOK_DLQ_RETRY(), {});
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
    var r = await fetchPost(EP.AUTH_APIKEY_CREATE(), payload);
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

                            
                      
                                                                             
                                                          
                                                        
                    
                                                                   
                                                            
                                                     
var _dashSubscribed = false;
var _dashVms = [];                               
var _dashSpan = '15m';                                                  
window._dashSpan = _dashSpan;

                                 
                                                                      
                                                            
                                                    
                                                           
                                                                            
function _epochMs(x) {
  var raw = x && (x.timestamp != null ? x.timestamp : (x.ts != null ? x.ts : x.time));
  var n = Number(raw);
  if (isFinite(n) && n > 0) return n < 1e12 ? n * 1000 : n;                 
  return Date.parse(raw || '') || 0;
}
window._epochMs = _epochMs;

                                                                    
                                                          
                                                         
function _fmtRate(bytesPerSec) {
  var n = Math.max(0, Number(bytesPerSec) || 0);
  var units = ['B/s', 'KB/s', 'MB/s', 'GB/s'];
  var i = 0;
  while (n >= 1024 && i < units.length - 1) { n /= 1024; i++; }
  return { num: n === 0 ? 0 : Number(n.toPrecision(2)), unit: units[i] };
}

                                            
                                                                       
                                                 
                                                                 
                                                                 
                                                                
                                                                     
function _renderDashGauges(host) {
  PCV.uxlib.clearEl(host);
  var M = PCV.metrics;
  var cpu = M.latest('host.cpu'), mem = M.latest('host.mem');
  var pk = function (k) { var p = M.peak(k, _dashSpan); return p === null ? null : 'peak ' + Math.round(p) + '%'; };
  var pool = window._shellSlow && _shellSlow.storage && _shellSlow.storage.worstPool;
  host.appendChild(HN.gaugeTile({ value: cpu === null ? null : Math.round(cpu), unit: '%', peak: pk('host.cpu'),
    label: _L('호스트 CPU', 'Host CPU'), scale: 'warn 80 · crit 95', warn: 80, crit: 95 }));
  host.appendChild(HN.gaugeTile({ value: mem === null ? null : Math.round(mem), unit: '%', peak: pk('host.mem'),
    label: _L('메모리', 'Memory'), scale: 'warn 70 · crit 90', warn: 70, crit: 90 }));
  host.appendChild(HN.gaugeTile({ value: pool ? pool.pct : null, unit: '%', peak: pool ? String(pool.state || '') : null,
    label: pool ? pool.name + ' ZFS' : 'ZFS', scale: 'warn 80 · crit 90', warn: 80, crit: 90 }));
  var netRate = M.latest('host.net'), netPeak = M.peak('host.net', _dashSpan);
                                                                 
                              
  var pctOfPeak = !netPeak ? 0 : Math.max(0, Math.min(100, (netRate / netPeak) * 100));
  var netFmt = netRate === null ? null : _fmtRate(netRate);
  var netPeakFmt = netPeak === null ? null : _fmtRate(netPeak);
  host.appendChild(HN.gaugeTile({
    value: netRate === null ? null : pctOfPeak,
    display: netFmt ? (netFmt.num + ' ' + netFmt.unit) : null,
    peak: netPeakFmt ? ('peak ' + netPeakFmt.num + ' ' + netPeakFmt.unit) : null,
    label: _L('네트워크', 'Network') + ' ' + (_netDevice || _L('VM 합산', 'VM sum')),
    warn: 101, crit: 102, ticks: false }));
}
window._renderDashGauges = _renderDashGauges;

                                                         
function _mapWlState(raw) {
  var s = String(raw || '').toLowerCase();
  if (s === 'running') return 'run';
  if (s === 'shutoff' || s === 'stopped') return 'stop';
  if (s === 'crashed' || s === 'error') return 'err';
  return 'warn';
}

                                                                            
                                                                      
function _wlStatusKey(it) {
  if (it.state === 'run') return 'running';
  if (it.state === 'err') return 'error';
  return 'stopped';
}

var _WL_TONE = { run: 'ok', stop: 'idle', err: 'crit', warn: 'warn' };

                                                 
                                                      
function _dashQaBtn(label, fn) {
  return PCV.uxlib.el('button', { class: 'btn', style: 'font-size:10px;padding:3px 8px',
    onClick: function (e) { if (e && e.stopPropagation) e.stopPropagation(); fn(); } }, label);
}

                                                               
                                                       
                                                             
function _dashFocusVmByName(name) {
  var i = vmList.findIndex(function (v) { return v.name === name; });
  if (i < 0) { toast(_L('대상 VM을 찾을 수 없습니다', 'VM not found'), false); return false; }
  selectedVmIndex = window.selectedVmIndex = i;
  return true;
}

                                                                      
                                                 
                                                     
                                                              
function _renderDashWorkloads(host) {
  var mk = PCV.uxlib.el;
                                                 
  var _ae = document.activeElement;
  var _focusChip = (_ae && _ae.classList && _ae.classList.contains('chip'))
    ? { f: _ae.getAttribute('data-facet'), v: _ae.getAttribute('data-val') } : null;
  PCV.uxlib.clearEl(host);

                                                        
  var ctrs = (window._shellSlow && _shellSlow.raw) ? (_shellSlow.raw.ctrs || []) : [];
  var items = _dashVms.map(function (v) {
    var maxMb = Number(v.memory_max_mb || v.memory_mb || 0);
    var usedMb = Number(v.memory_used_mb || v.mem_used_mb || 0);
    return {
      name: v.name, kind: 'vm', state: _mapWlState(v.state), raw: v.state || '?',
      cpu: v.live_cpu_pct == null ? null : Number(v.live_cpu_pct),
      mem: (maxMb > 0 && usedMb > 0) ? Math.round(usedMb / maxMb * 100) : null,
      meta: 'vm · ' + (v.vcpu || '?') + ' vCPU',
      vm: v
    };
  }).concat(ctrs.map(function (c) {
    return {
      name: c.name, kind: 'lxc', state: _mapWlState(c.state), raw: c.state || '?',
      cpu: null, mem: null,
      meta: 'lxc · ' + (c.ip_addr || c.ip || '—')
    };
  }));

  var nRun = 0, nStop = 0, nErr = 0, nVm = 0, nLxc = 0;
  items.forEach(function (it) {
    var k = _wlStatusKey(it);
    if (k === 'running') nRun++; else if (k === 'error') nErr++; else nStop++;
    if (it.kind === 'vm') nVm++; else nLxc++;
  });
  var statusOpts = [
    { value: 'running', label: _L('실행', 'Running'), count: nRun, sw: 'ok' },
    { value: 'stopped', label: _L('중지', 'Stopped'), count: nStop, sw: 'idle' }
  ];
                                                  
  if (nErr) statusOpts.push({ value: 'error', label: _L('오류', 'Error'), count: nErr, sw: 'crit' });
  host.appendChild(HN.filterBar([
    { key: 'status', label: _L('상태', 'Status'), options: statusOpts },
                                                                  
    { key: 'type', label: _L('유형', 'Type'), options: [
      { value: 'vm', label: 'VM', count: nVm },
      { value: 'lxc', label: 'LXC', count: nLxc }
    ] }
  ]));

  var cur = PCV.ui.filterState.current();
  var statusFilter = cur.status || [], typeFilter = cur.type || [];
  var shown = items.filter(function (it) {
    if (typeFilter.length && typeFilter.indexOf(it.kind) === -1) return false;
    if (statusFilter.length && statusFilter.indexOf(_wlStatusKey(it)) === -1) return false;
    return true;
  });

                                                              
                                                  
                                                          
  function pctCell(v) {
    if (v === null || !isFinite(v)) return mk('td', { class: 'color-muted' }, '—');
    var n = Math.round(v);
    return mk('td', null, mk('div', { class: 'flex items-center gap-4' },
      mk('div', { class: 'dash2-wl-gauge' }, HN.gauge({ value: n, warn: 80, crit: 95, inline: true })),
      mk('span', { class: 'dash2-wl-meta' }, n + '%')));
  }

  var rows = shown.slice(0, 10).map(function (it) {
    var tone = _WL_TONE[it.state] || 'warn';
                                                           
    var series = (it.kind === 'vm' && it.state === 'run' && PCV.metrics)
      ? PCV.metrics.window('vm.' + it.name + '.cpu', _dashSpan) : [];
    var qa = mk('div', { class: 'dash2-qa', 'data-role': 'OPERATOR,ADMIN' });
    if (it.kind === 'vm') {
      var focusVm = function () { return _dashFocusVmByName(it.name); };
      if (it.state === 'run') {
        qa.appendChild(_dashQaBtn(_L('중지', 'Stop'), function () {
          if (focusVm() && typeof vmPower === 'function') vmPower('stop');
        }));
        qa.appendChild(_dashQaBtn(_L('콘솔', 'Console'), function () {
          if (focusVm()) navigateTo('console');
        }));
      } else {
        qa.appendChild(_dashQaBtn(_L('시작', 'Start'), function () {
          if (focusVm() && typeof vmPower === 'function') vmPower('start');
        }));
      }
    } else {
      var ctrAct = it.state === 'run' ? 'stop' : 'start';
      qa.appendChild(_dashQaBtn(ctrAct === 'stop' ? _L('중지', 'Stop') : _L('시작', 'Start'), function () {
        if (typeof ctrA === 'function') ctrA(it.name, ctrAct);
      }));
    }
    return mk('tr', { style: 'cursor:pointer', onClick: function () {
      if (it.kind === 'vm') {
        if (_dashFocusVmByName(it.name)) navigateTo('summary');
      } else {
        selCtr = window.selCtr = it.name; ctrTab = window.ctrTab = 'summary';
        navigateTo('containers');
      }
    } },
      mk('td', null, mk('div', { class: 'flex items-center gap-8' },
        HN.statusDot(tone, { glow: it.state === 'run' }),
        mk('div', null,
          mk('b', null, it.name),
          mk('div', { class: 'dash2-wl-meta' }, it.meta)))),
      mk('td', null, HN.statusPill(tone, it.raw)),
      pctCell(it.cpu),
      pctCell(it.mem),
      mk('td', null, HN.spark(series, { tone: tone })),
      mk('td', { style: 'text-align:right' }, qa));
  });
  if (shown.length > 10) {
    rows.push(mk('tr', null, mk('td', { colspan: '6', class: 'color-muted text-center' },
      '... ' + _L('외', 'and') + ' ' + (shown.length - 10) + _L('개', ' more'))));
  }
  host.appendChild(mk('table', { style: 'font-size:12px' },
    mk('thead', null, mk('tr', null,
      mk('th', null, _L('워크로드', 'Workload')),
      mk('th', null, _L('상태', 'State')),
      mk('th', null, 'CPU'),
      mk('th', null, 'MEM'),
      mk('th', null, _L('추이', 'Trend')),
      mk('th', { style: 'text-align:right' }, _L('작업', 'Actions')))),
    mk('tbody', null, rows)));
                               
  if (_focusChip) {
    var _sel = host.querySelector('.chip[data-facet="' + _focusChip.f + '"][data-val="' + _focusChip.v + '"]');
    if (_sel) _sel.focus();
  }
                                                               
                                                                     
  if (window.currentUser && typeof applyRoleVisibility === 'function') {
    applyRoleVisibility(window.currentUser.role);
  }
}
window._renderDashWorkloads = _renderDashWorkloads;

                                                                          
function _feedText(it) {
  return it.message || it.detail || it.summary || '—';
}
                                              
                                                       
function _isRec(x) { return !!x && typeof x === 'object' && !Array.isArray(x); }

                            
                                                                      
                                               
                              
                                                                       
                                                     
function _renderDashFeed(host) {
  var mk = PCV.uxlib.el;
  PCV.uxlib.clearEl(host);
  var raw = (window._shellSlow && _shellSlow.raw) || {};
  var alertItems = (Array.isArray(raw.alerts) ? raw.alerts : []).filter(_isRec);
                                                            
                                                           
                                            
  var unack = alertItems.filter(function (a) { return !a.acknowledged; }).length;
  var items = alertItems.map(function (a) {
    var sev = HN.alertSeverity(a);
    return { tone: sev, label: sev.toUpperCase(), text: _feedText(a), ms: _epochMs(a), nav: 'mon-alerts' };
  }).concat((Array.isArray(raw.sec) ? raw.sec : []).filter(_isRec).map(function (ev) {
    return { tone: 'sec', label: 'SEC', text: _feedText(ev), ms: _epochMs(ev), nav: 'mon-security' };
  }));
  items.sort(function (a, b) { return b.ms - a.ms; });

  host.appendChild(mk('h4', { style: 'margin:0;padding:12px 14px 10px;border-bottom:1px solid var(--bg3)', role: 'heading', 'aria-level': '3' },
    _L('알림 · 보안 이벤트', 'Alerts · Security') + ' (' + _L('총 ', 'Total ') + items.length + ' · ' + unack + ' unack)'));
  var top = items.slice(0, 8);
  if (!top.length) {
    host.appendChild(mk('div', { style: 'padding:14px' },
      PCV.uxlib.msg('muted', { size: '12px' }, _L('최근 이벤트 없음', 'No recent events'))));
    return;
  }
  top.forEach(function (it) {
    host.appendChild(mk('div', { class: 'dash2-feed-item f-' + it.tone, style: 'cursor:pointer',
      onClick: function () { navigateTo(it.nav); } },
      mk('span', { class: 'dash2-feed-sev' }, it.label),
      mk('span', { style: 'font-size:12px' }, it.text),
      mk('span', { class: 'dash2-feed-time' }, it.ms ? formatRelativeTime(it.ms) : '—')));
  });
}
window._renderDashFeed = _renderDashFeed;

                                                                             
                                                      
                                                                      
function _healAct(method, params, okKo, okEn) {
  return function () {
    _shellRpc(method, params).then(function () {
      toast(_L(okKo, okEn));
      _collectShellSlow();
    }).catch(function (e) {
      toast(_L('실패', 'Failed') + ': ' + ((e && e.message) || method), false);
    });
  };
}

                                    
                                                                            
function _renderDashHealing(host) {
  var mk = PCV.uxlib.el;
  PCV.uxlib.clearEl(host);
  var raw = (window._shellSlow && _shellSlow.raw) || {};
  var pend = (Array.isArray(raw.healing) ? raw.healing : []).filter(_isRec);
  host.appendChild(mk('h4', { style: 'margin:0;padding:12px 14px 10px;border-bottom:1px solid var(--bg3)', role: 'heading', 'aria-level': '3' },
    _L('자가치유 승인 대기', 'Self-healing approvals') + ' (' + pend.length + ')'));
  if (!pend.length) {
    host.appendChild(mk('div', { style: 'padding:14px' },
      PCV.uxlib.msg('muted', { size: '12px' }, _L('대기 없음', 'Nothing pending'))));
  } else {
    var qaStyle = 'font-size:10px;padding:3px 8px';
    pend.slice(0, 3).forEach(function (p) {
      var ms = _epochMs(p);
      host.appendChild(mk('div', { class: 'dash2-healrow' },
        mk('div', { class: 'flex items-center gap-8' },
          mk('span', { class: 'dash2-heal-tag' }, String(p.action || '—')),
          mk('b', { style: 'font-size:12px' }, String(p.policy || '—')),
          mk('span', { class: 'dash2-feed-time' }, ms ? formatRelativeTime(ms) : '—')),
        mk('div', { class: 'dash2-wl-meta', style: 'margin:5px 0 8px' }, String(p.reason || '—')),
        mk('div', { class: 'flex gap-8', style: 'justify-content:flex-end' },
          mk('button', { class: 'btn', style: qaStyle, 'data-role': 'ADMIN',
            onClick: _healAct('ai.healing.approve', { action_id: p.id }, '승인됨', 'Approved') },
            _L('승인', 'Approve')),
          mk('button', { class: 'btn', style: qaStyle, 'data-role': 'ADMIN',
            onClick: _healAct('ai.healing.reject', { action_id: p.id, reason: 'dashboard' }, '거부됨', 'Rejected') },
            _L('거부', 'Reject')))));
    });
  }
                                                          
                                                     
  if (typeof applyRoleVisibility === 'function') applyRoleVisibility(window.currentUser && window.currentUser.role);
}
window._renderDashHealing = _renderDashHealing;

async function renderDashboard(b) {
  showSkeleton(b);
                                                                  
                                                     
                                                            
                                                   
  PCV.ui.filterState.readFromUrl();
  if (!_dashSubscribed) {
    _dashSubscribed = true;
    PCV.ui.filterState.subscribe(function () {
      if (PCV.state.currentTab !== 'dashboard') return;
      var host = document.getElementById('dash-workloads');
      if (host) _renderDashWorkloads(host);
    });
  }
                     
  await collectHostMetrics();
  try {
    var r = await fetchGet(API_BASE + '/vms').catch(function () { return { data: [] }; });
    var vms = unwrapList(r);
    _dashVms = vms;
    if (PCV.metrics) vms.forEach(function (v) { PCV.metrics.push('vm.' + v.name + '.cpu', v.live_cpu_pct || 0); });
    var mk = PCV.uxlib.el;
    PCV.uxlib.clearEl(b);
                                                              
                                      
    b.appendChild(mk('div', { class: 'flex items-center gap-8', style: 'flex-wrap:wrap;margin-bottom:14px' },
      mk('div', null,
        mk('h2', { style: 'margin:0' }, _L('운영 대시보드', 'Operations Dashboard')),
        mk('p', { class: 'color-muted', style: 'margin:2px 0 0;font-size:12.5px' },
          _L('호스트 상태, 워크로드, 최근 경고를 한 화면에서.', 'Host health, workloads, and recent alerts in one view.'))),
      mk('div', { class: 'dash2-timeseg', role: 'group', 'aria-label': _L('표시 창', 'Time window') },
        ['live', '15m', '1h'].map(function (s) {
          return mk('button', { class: s === _dashSpan ? 'on' : '', 'data-span': s,
            onClick: function () {
              _dashSpan = s; window._dashSpan = s;
              document.querySelectorAll('.dash2-timeseg button').forEach(function (x) { x.classList.toggle('on', x.dataset.span === s); });
              var g = document.getElementById('dash-gauges'); if (g) _renderDashGauges(g);
              var w = document.getElementById('dash-workloads'); if (w) _renderDashWorkloads(w);
            } }, s === 'live' ? 'Live' : s);
        }))));
    var gauges = mk('div', { class: 'dash2-grid4', id: 'dash-gauges' });
    var wl = mk('div', { class: 'hc', id: 'dash-workloads', style: 'padding:0' });
    var feed = mk('div', { class: 'hc', id: 'dash-feed', style: 'padding:0' });
    var heal = mk('div', { class: 'hc', id: 'dash-healing', style: 'padding:0' });
    b.appendChild(gauges);
    b.appendChild(mk('div', { class: 'dash2-main', style: 'margin-top:14px' }, wl,
      mk('div', { class: 'flex', style: 'flex-direction:column;gap:14px' }, feed, heal)));
    _renderDashGauges(gauges);
    _renderDashWorkloads(wl);
    _renderDashFeed(feed);
    _renderDashHealing(heal);
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
    if (PCV.metrics) {
      vmList.forEach(function (v) { PCV.metrics.push('vm.' + v.name + '.cpu', v.live_cpu_pct || 0); });
                                                        
                                                     
      PCV.metrics.prune('vm.', vmList.map(function (v) { return 'vm.' + v.name + '.cpu'; }));
    }
    render(skipContent);
    if (!skipContent && typeof refreshVmDetail === 'function') refreshVmDetail();
                                                     
                                                     
                                                                    
    if (currentTab === 'dashboard') {
      var g = document.getElementById('dash-gauges'); if (g) _renderDashGauges(g);
      var w = document.getElementById('dash-workloads');
      if (w) { _dashVms = vmList; _renderDashWorkloads(w); }
    }
    _pushShellSnapshot();
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
                                                                  
                                                                
                                                      
                           
  if (PCV.ui && PCV.ui.filterState && PCV.ui.filterState.readFromUrl) PCV.ui.filterState.readFromUrl();
  if (typeof navigateToHash === 'function') navigateToHash();
                                                                        
  _collectShellSlow();
}
window.pcvPostLoginInit = pcvPostLoginInit;
                                                           
setInterval(() => { if (document.hidden) return; if (authToken) loadAll(true); }, 10e3);

                                     
                                                                           
                                                                       
                                                           
                                                         
                                                           
                                                                      
                                                       
                                                            
                                                                         
                                                         
                                                     
                                                    
var _prevNetSample = null;
                                                   
                                                 
                                                 
                                              
var _netDevice = null;
                                                      
                                                         
                          
var _netDevices = [];
function _pushNetRate(totalBytes, src, dev) {
  var now = Date.now();
  if (_prevNetSample && _prevNetSample.src === src && _prevNetSample.dev === dev) {
    var dt = now - _prevNetSample.t;
    if (dt >= 2000) {
      var deltaBytes = totalBytes - _prevNetSample.v;
      PCV.metrics.push('host.net', deltaBytes > 0 ? (deltaBytes / dt) * 1000 : 0);
    }
  }
  _prevNetSample = { src: src, dev: dev, t: now, v: totalBytes };
}

async function collectHostMetrics() {
  var token = window.authToken || authToken;
  if (!token) return;
  try {
    var res = await fetch(EP.METRICS(), { headers: { Authorization: 'Bearer ' + token } });
    if (!res.ok) return;
    var met = await res.text();
    var cpu = 0, mem = 0, vmNetTotal = 0, vmNetFound = false;
    var nicDevs = {}, nicFound = false;
    met.split('\n').forEach(function(l) {
      if (l.startsWith('purecvisor_host_cpu_percent ')) cpu = parseFloat(l.split(' ')[1]);
      if (l.startsWith('purecvisor_host_memory_percent ')) mem = parseFloat(l.split(' ')[1]);
                                                                 
                                                                
                                                                    
                                                 
      if (l.startsWith('purecvisor_vm_net_rx_bytes_total{') || l.startsWith('purecvisor_vm_net_tx_bytes_total{')) {
        var v = parseFloat(l.slice(l.lastIndexOf(' ') + 1));
        if (isFinite(v)) { vmNetTotal += v; vmNetFound = true; }
      }
                                                               
                                                          
                                  
      if (l.startsWith('node_network_receive_bytes_total{') || l.startsWith('node_network_transmit_bytes_total{')) {
        var dm = l.match(/device="([^"]+)"/);
        var nv = parseFloat(l.slice(l.lastIndexOf(' ') + 1));
        if (dm && isFinite(nv)) { nicDevs[dm[1]] = (nicDevs[dm[1]] || 0) + nv; nicFound = true; }
      }
    });
    _netDevices = Object.keys(nicDevs).filter(function (d) { return d !== 'lo'; });
    if (PCV.metrics) {
      PCV.metrics.push('host.cpu', cpu); PCV.metrics.push('host.mem', mem);
      var netTotal = 0, netFound = false, src = 'vmsum', dev = null;
      if (nicFound) {
                                                                     
                                                                    
                                                      
                                                                
                                                                
                                                           
                    
        var ovr = localStorage.getItem('pcv-net-device');
        if (ovr && nicDevs[ovr] !== undefined) {
          _netDevice = ovr;
        } else if (!(_netDevice && nicDevs[_netDevice] !== undefined)) {
          var best = null, bestVal = -1;
          for (var d in nicDevs) {
            if (d === 'lo') continue;
            if (nicDevs[d] > bestVal) { bestVal = nicDevs[d]; best = d; }
          }
          _netDevice = best;
        }
        if (_netDevice) { netTotal = nicDevs[_netDevice]; netFound = true; src = 'nic'; dev = _netDevice; }
      } else {
                                                                
                                                          
                                                         
        _netDevice = null;
      }
      if (!netFound && vmNetFound) { netTotal = vmNetTotal; netFound = true; src = 'vmsum'; dev = null; }
      if (netFound) _pushNetRate(netTotal, src, dev);
    }
  } catch (e) {                                           }
}
setInterval(() => { if (document.hidden) return; collectHostMetrics(); }, 5000);
                                                                                  

                                                                  
                                                             
document.addEventListener('visibilitychange', () => {
  if (document.hidden) return;
  if (authToken) loadAll(true);
  collectHostMetrics();
  if (typeof updateStatusBar === 'function') updateStatusBar();
});

                                
                                                           
                                                           
                                                  
                                                   
                                                  
                                                
                              
document.addEventListener('keydown', e => {
  if (e.key === 'F11') { e.preventDefault(); toggleFS(); }
  if (e.ctrlKey && e.key === 'n') {
    e.preventDefault();
    if (typeof pcvRoleAllows === 'function' && pcvRoleAllows('operator')) showCreate();
  }
  if (e.ctrlKey && e.key === 'd') {
    e.preventDefault();
    if (typeof pcvRoleAllows === 'function' && pcvRoleAllows('operator')) showSettings();
  }
  if (e.ctrlKey && e.key === 'p') { e.preventDefault(); showPrefs(); }
                            
  if (e.ctrlKey && e.key === 'k') { e.preventDefault(); if (cmdPaletteOpen) closeCmdPalette(); else openCmdPalette(); }
});

                    

                                                             
                                                                   

function handleResize() {
  if (window.innerWidth <= 860) return;
  const sb = document.getElementById('shell-sidebar');
  const ov = document.getElementById('mobile-overlay');
  if (sb) sb.classList.remove('mobile-open');
  if (ov) ov.style.display = 'none';
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
  else if (dx < -60 && document.getElementById('shell-sidebar')?.classList.contains('mobile-open')) { closeMobileSB(); }
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
  } catch (e) {                           }
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
  catch (e) {                                    }
}
                                       
var _origDoLoginPage = typeof doLoginPage === 'function' ? doLoginPage : null;

                                                    
                                                          
                   
  
                                                                        
                                                                   
                                                            
                                                                      
                                                           
  
                                                                 
                                                         
  
                                                                       
                                                          
                                                    
             
  
                                                
                                                              
                                                    
                    
if ('serviceWorker' in navigator) {
  window.addEventListener('load', () => {
    navigator.serviceWorker.register('/ui/sw.js', { updateViaCache: 'none' }).then(reg => {
      window._swRegError = null;
                                     
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
    }).catch(err => {
                                                     
                                                 
      window._swRegError = {
        name: err && err.name,
        message: err && err.message ? String(err.message) : String(err)
      };
    });
                               
    navigator.serviceWorker.addEventListener('controllerchange', () => {
      window.location.reload();
    });
  });
}

                                                         
requestBrowserNotif();

                                                                  
                             
                                                  
                                                                     

                                                                   
                                                                          

                                              
CMD_ACTIONS.push(
  { icon: 'ci-file-document', label: 'Templates', action: () => window.navigateTo('templates') },
  { icon: 'ci-settings', label: 'Config Management', action: () => window.navigateTo('config-mgmt') },
                                                    
  { icon: 'ci-file-document', label: 'Import OVA', role: 'operator', action: () => showImportOva() },
  { icon: 'ci-cloud', label: 'Cloud Migration', action: () => window.navigateTo('cloud-migration') },
  { icon: 'ci-bell', label: 'Notifications', action: () => toggleNotifCenter() }
);

                  
                                                                             
                                                                              
                                                         
  
               
                                             
   
                                                                        
if (!window._navTabsWrapped) {
  window._pcvOrigNavigateTo = navigateTo;
  window.navigateTo = function navigateToWithTabs(n, hooks) {
    if (navigateToWithTabs._busy) return window._pcvOrigNavigateTo(n, hooks);
    navigateToWithTabs._busy = true;
    try {
      var tabLabels = {
        'dashboard': _L('대시보드', 'Dashboard'),
        'summary': _L('요약', 'Summary'),
        'console': _L('콘솔', 'Console'),
        'snapshots': _L('스냅샷', 'Snapshots'),
        'performance': _L('성능', 'Performance'),
        'timeline': _L('타임라인', 'Timeline'),
        'networks': _L('네트워크', 'Networks'),
        'vpcs': 'Local VPC',
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
        'apimgmt': _L('API 관리', 'API Management'),
        'templates': _L('템플릿', 'Templates'),
        'config-mgmt': _L('설정 관리', 'Configuration Management'),
        'cloud-migration': _L('클라우드 마이그레이션', 'Cloud Migration')
      };
      var label = tabLabels[n] || n.replace(/-/g, ' ').replace(/\b\w/g, function(c) { return c.toUpperCase(); });
      return window._pcvOrigNavigateTo(n, {
        before: function() {
          if (hooks && typeof hooks.before === 'function') hooks.before();
        },
        after: function() {
          if (typeof setHashRoute === 'function') setHashRoute(n);
          if (typeof setPageTitle === 'function') setPageTitle(label);
          if (hooks && typeof hooks.after === 'function') hooks.after();
        }
      });
    } finally { navigateToWithTabs._busy = false; }
  };
  window.go = window.navigateTo;
  window._navTabsWrapped = true;
}

                               

                                                              
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

                              

                                               

                             
                                                                                           

                             
                                                                              

                                       

                                            

                                          
                                                                   
                                                                      

                                                              
document.addEventListener('keydown', e => {
  if (e.ctrlKey && e.shiftKey && e.key === 'F') { e.preventDefault(); toggleGlobalSearch(); }
  if (e.ctrlKey && e.key === 'b') { e.preventDefault(); toggleSB(); }
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

                                        
if (window.PCV && PCV.shell) PCV.shell.mount();

                                                  
                                                           
                                                        
window._shellSlow = { ctrs: null, alerts: null, sec: null, storage: null, healing: null, raw: {} };

                           
                                                                      
                                                              
                                      
                                                 
                                  
                                                                         
                           
async function _shellRpc(method, params) {
  var r = await fetchPost(EP.RPC(), { jsonrpc: '2.0', method: method, params: params || {}, id: 'shell-' + method });
  if (r && r.error) throw new Error(r.error.message || method);
  return (typeof unwrapData === 'function') ? unwrapData(r) : (r.result || r.data || r);
}

   
                                                           
                                           
  
                                                          
                                                   
  
                                                                  
                                                                     
                                                          
                                                                   
                                                                      
                                      
  
                                                 
                                                       
  
                                                                
                  
                                                                     
                               
   
async function _collectShellSlow() {
  if (!authToken) return;
  var s = window._shellSlow;
  await Promise.allSettled([
    fetchGet(EP.CTR_LIST()).then(function (r) {
      var l = unwrapList(r);
      s.raw.ctrs = l;
      s.ctrs = { run: l.filter(function (c) { return c.state === 'RUNNING'; }).length, total: l.length };
    }).catch(function () { s.ctrs = null; }),
    fetchGet(API_BASE + '/alerts').then(function (r) {
      var l = unwrapList(r);
      s.raw.alerts = l;
      var crit = l.filter(function (a) { return HN.alertSeverity(a) === 'crit'; }).length;
      var warn = l.filter(function (a) { return HN.alertSeverity(a) === 'warn'; }).length;
      var unack = l.filter(function (a) { return !a.acknowledged; }).length;
      s.alerts = { crit: crit, warn: warn, total: l.length, unack: unack };
    }).catch(function () { s.alerts = null; }),
    _shellRpc('security.event.list').then(function (l) {
      l = Array.isArray(l) ? l : (l && l.events) || [];
      var hourAgo = Date.now() - 3600e3;
                                                                         
                                            
      var recent = l.filter(function (ev) {
        return _epochMs(ev) >= hourAgo;
      });
                                                              
                                                         
                                                
      s.raw.sec = l;
      var worst = recent.some(function (ev) {
        return String(ev.severity || '').toLowerCase().indexOf('crit') === 0;
      }) ? 'crit' : (recent.length ? 'warn' : 'ok');
      s.sec = { count1h: recent.length, worst: worst };
    }).catch(function () { s.sec = null; }),
    fetchGet(EP.STORAGE_POOLS()).then(function (r) {
      var pools = unwrapList(r);
                                                                             
                                                     
                                                
      s.raw.pools = pools;
      if (!pools.length) { s.storage = null; return; }
                                                                                          
      function poolPct(x) {
        var size = parseSize(x.size) || 0, used = parseSize(x.alloc || x.used) || 0;
        return size > 0 ? Math.round(used / size * 100) : 0;
      }
      function bad(x) { return (x.health || x.state) !== 'ONLINE' ? 1 : 0; }
      var worst = pools.reduce(function (a, p) {
        return (bad(p) > bad(a) || (bad(p) === bad(a) && poolPct(p) > poolPct(a))) ? p : a;
      });
      s.storage = { worstPool: { name: worst.name || '?', pct: poolPct(worst),
        state: worst.health || worst.state || '?' } };
    }).catch(function () { s.storage = null; }),
    _shellRpc('healing.pending').then(function (l) {
      s.raw.healing = Array.isArray(l) ? l : [];
      s.healing = { pending: Array.isArray(l) ? l.length : 0 };
    }).catch(function () { s.healing = null; }),
    fetchGet(EP.MONITOR_FLEET()).then(function (r) {
      s.raw.fleet = unwrapData(r).fleet || [];
    }),
    fetchGet(EP.NET_LIST()).then(function (r) {
      s.raw.networks = unwrapList(r);
    })
  ]);
                                                        
                                                       
  if (currentTab === 'dashboard') {
    var f = document.getElementById('dash-feed'); if (f) _renderDashFeed(f);
    var h = document.getElementById('dash-healing'); if (h) _renderDashHealing(h);
    var g = document.getElementById('dash-gauges'); if (g) _renderDashGauges(g);
  }
  _pushShellSnapshot();
}
window._collectShellSlow = _collectShellSlow;

function _buildShellSnapshot() {
  var s = window._shellSlow;
  return {
    vms: vmList ? { run: vmList.filter(function (v) { return v.state === 'running'; }).length, total: vmList.length } : null,
    ctrs: s.ctrs, alerts: s.alerts, sec: s.sec, storage: s.storage, healing: s.healing
  };
}
window._buildShellSnapshot = _buildShellSnapshot;

function _pushShellSnapshot() {
  if (window.PCV && PCV.shell && PCV.shell.update) PCV.shell.update(_buildShellSnapshot());
}
window._pushShellSnapshot = _pushShellSnapshot;

setInterval(function () { if (document.hidden) return; _collectShellSlow(); }, 30e3);

                                   
restoreSession();

                  
if (typeof registerShortcut === 'function') {
  registerShortcut('/', function(){ if (typeof toggleGlobalSearch === 'function') toggleGlobalSearch(); }, '글로벌 검색');
  registerShortcut('n', function(){
    if (typeof pcvRoleAllows === 'function' && pcvRoleAllows('operator') && typeof showCreate === 'function') showCreate();
  }, '새 VM');
  registerShortcut('?', function(){ if (typeof toggleKbdHelp === 'function') toggleKbdHelp(); }, '단축키 도움말');
  registerShortcut('g', function(){ navigateTo('dashboard'); }, '대시보드 이동');
  registerShortcut('m', function(){ navigateTo('mon-overview'); }, '모니터링');
}
