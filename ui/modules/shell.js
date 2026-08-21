                                                                  
                                
                                                          
                                                                             
                                            
                                                                     
                                                               
  
                           
                                                                    
  
                                                                
                                                     
                                              
  
                                             
                                                                 
                                                                
                                                       
                                                              
  
                                                        
                                                     
  
                                                                    
                                                                              
                                                             
                 
  
                                                               
                                                     
   
window.PCV = window.PCV || {};
(function (PCV) {

var VM_TABS = ['summary', 'console', 'snapshots', 'performance', 'timeline'];

function L(ko, en) { return (typeof _L === 'function') ? _L(ko, en) : ko; }

                                                                   
                                                                  
                                                           
var SHELL_SVG_NS = 'http://www.w3.org/2000/svg';
function shellIcon(symbol, cls) {
  var svg = document.createElementNS(SHELL_SVG_NS, 'svg');
  svg.setAttribute('class', 'ci-icon' + (cls ? ' ' + cls : ''));
  svg.setAttribute('aria-hidden', 'true');
  svg.setAttribute('focusable', 'false');
  var use = document.createElementNS(SHELL_SVG_NS, 'use');
  use.setAttribute('href', 'vendor/coolicons/coolicons.svg#' + symbol);
  svg.appendChild(use);
  return svg;
}

            
                                                                           
                                                       
                                                             
                                                             
                                                              
                                                                     
                                                                          
                                                                       
                                                 
function NAV_SECTIONS() {
  return [
    { label: null, items: [ { id: 'dashboard', ko: '운영 대시보드', en: 'Dashboard', ico: 'ci-house-01' } ] },
    { label: L('워크로드', 'Workloads'), items: [
      { id: 'vm', ko: '가상 머신', en: 'Virtual Machines', ico: 'ci-desktop', cnt: 'vms' },
      { id: 'containers', ko: '컨테이너', en: 'Containers', ico: 'ci-layers', cnt: 'ctrs' },
      { id: 'templates', ko: '템플릿', en: 'Templates', ico: 'ci-file-document' } ] },
    { label: L('인프라', 'Infrastructure'), items: [
      { id: 'networks', ko: '네트워크', en: 'Networks', ico: 'ci-globe' },
      { id: 'vpcs', ko: 'Local VPC', en: 'Local VPC', ico: 'ci-layers' },
      { id: 'ovn', ko: 'OVN SDN', en: 'OVN SDN', ico: 'ci-data' },
      { id: 'overlay', ko: '오버레이 네트워크', en: 'Overlay Networks', ico: 'ci-cloud' },
      { id: 'security-groups', ko: '보안 그룹', en: 'Security Groups', ico: 'ci-shield' },
      { id: 'storage', ko: '스토리지', en: 'Storage', ico: 'ci-data', dot: 'storage' },
      { id: 'backup', ko: '백업', en: 'Backup', ico: 'ci-refresh' },
      { id: 'iscsi', ko: 'iSCSI 타깃', en: 'iSCSI Targets', ico: 'ci-data' },
      { id: 'dpdk', ko: 'DPDK', en: 'DPDK', ico: 'ci-terminal' },
      { id: 'sriov', ko: 'SR-IOV', en: 'SR-IOV', ico: 'ci-layers' },
      { id: 'pool-info', ko: '커넥션 풀', en: 'Connection Pool', ico: 'ci-chart-line' },
      { id: 'gpu', ko: 'GPU 장치', en: 'GPU Devices', ico: 'ci-desktop-tower' },
      { id: 'topology', ko: '토폴로지', en: 'Topology', ico: 'ci-chart-line' },
      { id: 'cloud-migration', ko: '클라우드 마이그레이션', en: 'Cloud Migration', ico: 'ci-cloud' },
      { id: 'host', ko: '호스트 상태', en: 'Host Health', ico: 'ci-monitor' } ] },
    { label: L('관제', 'Monitoring'), items: [
      { id: 'mon-overview', ko: '운영 개요', en: 'Overview', ico: 'ci-chart-line' },
      { id: 'ops-triage', ko: '이벤트 센터', en: 'Event Center', ico: 'ci-warning' },
      { id: 'mon-alerts', ko: '알림', en: 'Alerts', ico: 'ci-bell', dot: 'alerts' },
      { id: 'mon-security', ko: '보안 이벤트', en: 'Security Events', ico: 'ci-shield-check', dot: 'sec' },
      { id: 'mon-audit', ko: '감사 로그', en: 'Audit Log', ico: 'ci-search' },
      { id: 'activity-log', ko: '활동 로그', en: 'Activity Log', ico: 'ci-file-document' },
      { id: 'mon-vms', ko: 'VM 모니터', en: 'VM Monitor', ico: 'ci-desktop' },
      { id: 'mon-storage', ko: '스토리지 모니터', en: 'Storage Monitor', ico: 'ci-data' },
      { id: 'mon-hosts', ko: '호스트 모니터', en: 'Host Monitor', ico: 'ci-monitor' },
      { id: 'heatmap', ko: '히트맵', en: 'Heatmap', ico: 'ci-layers' },
      { id: 'api-perf', ko: 'API 성능', en: 'API Performance', ico: 'ci-chart-line' },
      { id: 'selfhealing', ko: '자가치유', en: 'Self-Healing', ico: 'ci-refresh', dot: 'healing' } ] },
    { label: L('시스템', 'System'), items: [
      { id: 'accounts', ko: '계정과 권한', en: 'Accounts', ico: 'ci-users', role: 'ADMIN' },
      { id: 'apimgmt', ko: 'API 관리', en: 'API Management', ico: 'ci-terminal', role: 'ADMIN' },
      { id: 'config-mgmt', ko: '설정 관리', en: 'Config Management', ico: 'ci-settings' } ] },
    { label: L('도움말', 'Help'), items: [
      { id: 'helppage', ko: '도움말', en: 'Help', ico: 'ci-info' },
      { id: 'apihelp', ko: 'Swagger API', en: 'Swagger API', ico: 'ci-terminal' } ] }
  ];
}

var _snapshot = null;

function itemLabel(it) { return L(it.ko, it.en); }

                                                         
                                                                   
function navTarget(id) {
  if (id === 'vm') return localStorage.getItem('pcv-last-vm-tab') || 'summary';
  return id;
}

                
                                                     
                                                            
                                   
                                                                               
                                                        
function buildSidebar() {
  var el = PCV.uxlib.el;
  var root = document.getElementById('shell-sidebar');
  if (!root) return;
  PCV.uxlib.clearEl(root);
  root.appendChild(el('div', { class: 'shell-brand' },
    el('div', { class: 'shell-brand-mark', 'aria-hidden': 'true' }, 'P'),
    el('div', null,
      el('div', { class: 'shell-brand-name' }, 'PureCVisor'),
      el('div', { class: 'shell-brand-edi' }, 'SINGLE EDGE'))));
  var wrap = el('nav', { class: 'shell-navwrap', role: 'navigation', 'aria-label': 'Main navigation' });
  NAV_SECTIONS().forEach(function (sec) {
    var box = el('div', { class: 'shell-navsec' });
    if (sec.label) box.appendChild(el('div', { class: 'shell-navlbl' }, sec.label));
    sec.items.forEach(function (it) {
      var attrs = {
        class: 'shell-navitem', 'data-nav': it.id, role: 'link', tabindex: '-1',
                                                       
                                                                 
        onClick: function () {
          if (typeof navigateTo !== 'function') return;
          navigateTo(navTarget(it.id), {
            after: function () { if (typeof closeMobileSB === 'function') closeMobileSB(); }
          });
        }
      };
      if (it.role) attrs['data-role'] = it.role;
      var node = el('div', attrs,
        shellIcon(it.ico || 'ci-info', 'shell-ico'),
        el('span', { class: 'shell-nm' }, itemLabel(it)),
        it.cnt ? el('span', { class: 'shell-cnt', 'data-cnt': it.cnt }, '—') : null,
        it.dot ? el('span', { class: 'shell-dot', 'data-dot': it.dot, hidden: '' }) : null);
      box.appendChild(node);
    });
    wrap.appendChild(box);
  });
                                                         
                                                       
  wrap.addEventListener('keydown', function (e) {
    if (e.key !== 'ArrowDown' && e.key !== 'ArrowUp') return;
    var items = Array.prototype.filter.call(
      wrap.querySelectorAll('.shell-navitem'),
      function (n) { return n.style.display !== 'none'; });
    var idx = items.indexOf(document.activeElement);
    if (idx === -1) return;
    e.preventDefault();
    var n = items.length, d = e.key === 'ArrowDown' ? 1 : -1;
    var next = items[((idx + d) % n + n) % n];
    items.forEach(function (m) { m.setAttribute('tabindex', m === next ? '0' : '-1'); });
    next.focus();
  });
  var first = wrap.querySelector('.shell-navitem');
  if (first) first.setAttribute('tabindex', '0');
  root.appendChild(wrap);
}

                  
function buildTopbar() {
  var el = PCV.uxlib.el;
  var root = document.getElementById('shell-topbar');
  if (!root) return;
  PCV.uxlib.clearEl(root);
  root.appendChild(el('div', { class: 'shell-crumb', id: 'shell-crumb' }, 'Single Edge'));
  root.appendChild(el('div', {
    class: 'shell-search', role: 'button', tabindex: '0',
    'aria-label': L('글로벌 검색 열기', 'Open global search'),
    onClick: function () { if (typeof toggleGlobalSearch === 'function') toggleGlobalSearch(); }
  },
    shellIcon('ci-search', 'shell-search-ico'),
    el('span', { class: 'shell-search-ph' }, L('검색', 'Search')),
    el('kbd', null, 'Ctrl+K')));
  var right = el('div', { class: 'shell-topbar-r' });
  right.appendChild(el('span', {
    id: 'shell-sync', class: 'shell-sync', role: 'status',
    'aria-live': 'polite', 'aria-atomic': 'true'
  }));
                                                            
                                                 
                                                                
  root.appendChild(right);
}

                                        
                                                            
                           
                                                                           
                                                                
                                     
function seg(status, count, label, sub, target, filter) {
  return { key: target, status: status, count: count, label: label, sub: sub,
           filter: filter || null, _target: target };
}
                                                               
                                              
                                                                 
                                                                          
function buildSegments(s) {
  var dash = '—';
  var vms = s && s.vms, ctrs = s && s.ctrs, al = s && s.alerts,
      sec = s && s.sec, st = s && s.storage && s.storage.worstPool, he = s && s.healing;
  return [
    vms ? seg(vms.run < vms.total ? 'warn' : 'ok', vms.run, L('VM 실행', 'VMs up'),
              '/ ' + vms.total + ' · ' + (vms.total - vms.run) + L(' 중지', ' down'), navTarget('vm'))
        : seg('idle', dash, L('VM 실행', 'VMs up'), '', navTarget('vm')),
    ctrs ? seg(ctrs.run < ctrs.total ? 'warn' : 'ok', ctrs.run, L('컨테이너', 'Containers'),
               ctrs.run + '/' + ctrs.total, 'containers')
         : seg('idle', dash, L('컨테이너', 'Containers'), '', 'containers'),
    al ? seg(al.crit > 0 ? 'crit' : (al.warn > 0 ? 'warn' : 'ok'), al.crit, 'Critical',
             '+' + al.warn + ' warn · ' + al.unack + ' unack', 'mon-alerts',
             al.crit > 0 ? { severity: ['crit'] } : null)
       : seg('idle', dash, 'Critical', '', 'mon-alerts'),
    sec ? seg(sec.worst === 'crit' ? 'crit' : (sec.count1h > 0 ? 'warn' : 'ok'), sec.count1h,
              L('보안 이벤트', 'Security'), 'Suricata · 1h', 'mon-security')
        : seg('idle', dash, L('보안 이벤트', 'Security'), '', 'mon-security'),
    st ? seg(st.state === 'ONLINE' ? (st.pct >= 80 ? 'warn' : 'ok') : 'crit', st.pct + '%',
             st.name, String(st.state || ''), 'storage')
       : seg('idle', dash, L('스토리지', 'Storage'), '', 'storage'),
    he ? seg(he.pending > 0 ? 'warn' : 'ok', he.pending, L('자가치유', 'Self-Healing'),
             he.pending > 0 ? L('승인 필요', 'approval needed') : L('대기 없음', 'idle'), 'selfhealing')
       : seg('idle', dash, L('자가치유', 'Self-Healing'), '', 'selfhealing')
  ];
}
function buildStatusbar() {
  var root = document.getElementById('shell-statusbar');
  if (!root) return;
                                                                
                                                   
                                                                                  
  var active = document.activeElement;
  var focusKey = active && root.contains(active) ? active.dataset.segKey : null;
  PCV.uxlib.clearEl(root);
  root.appendChild(HN.statusBar(buildSegments(_snapshot), {
    onNavigate: function (s) { if (typeof navigateTo === 'function') navigateTo(s._target); }
  }));
  if (focusKey) {
    var replacement = root.querySelector('[data-seg-key="' + focusKey + '"]');
    if (replacement) replacement.focus();
  }
}

                 
   
                                                          
  
                                                                           
                                                      
                   
                                                        
                                                     
   
function mount() {
  if (!document.getElementById('shell-sidebar')) {
    console.error('PCV.shell.mount: shell containers missing');
    return;
  }
  buildSidebar();
  buildTopbar();
                                                       
                                                 
                                                                     
  if (_snapshot) update(_snapshot); else buildStatusbar();
  if (window.currentUser && typeof applyRoleVisibility === 'function') {
    applyRoleVisibility(window.currentUser.role);
  }
  setActive(window.currentTab || 'dashboard');
}

   
                                                       
  
                                                        
                                                 
                                                        
  
                                                             
                                   
                                                                                        
   
function update(snapshot) {
  _snapshot = snapshot || _snapshot;
  var s = _snapshot || {};
                               
  var vmCnt = document.querySelector('#shell-sidebar [data-cnt="vms"]');
  if (vmCnt) vmCnt.textContent = s.vms ? s.vms.run + '/' + s.vms.total : '—';
  var ctrCnt = document.querySelector('#shell-sidebar [data-cnt="ctrs"]');
  if (ctrCnt) ctrCnt.textContent = s.ctrs ? s.ctrs.run + '/' + s.ctrs.total : '—';
  function dot(name, on, tone) {
    var d = document.querySelector('#shell-sidebar [data-dot="' + name + '"]');
    if (!d) return;
    if (on) { d.removeAttribute('hidden'); d.className = 'shell-dot shell-dot-' + tone; }
    else d.setAttribute('hidden', '');
  }
  dot('alerts', !!(s.alerts && s.alerts.crit > 0), 'crit');
  dot('sec', !!(s.sec && s.sec.count1h > 0), s.sec && s.sec.worst === 'crit' ? 'crit' : 'warn');
  dot('storage', !!(s.storage && s.storage.worstPool &&
      (s.storage.worstPool.state !== 'ONLINE' || s.storage.worstPool.pct >= 80)), 'warn');
  dot('healing', !!(s.healing && s.healing.pending > 0), 'warn');
  buildStatusbar();
}

                                                         
                                                               
                                                               
function crumbFor(tabId) {
  var effective = VM_TABS.indexOf(tabId) !== -1 ? 'vm' : tabId;
  var secLabel = null, itLabel = null;
  NAV_SECTIONS().forEach(function (sec) {
    sec.items.forEach(function (it) {
      if (it.id === effective) { secLabel = sec.label; itLabel = itemLabel(it); }
    });
  });
  return { section: secLabel, item: itLabel || tabId, effective: effective };
}

                                                                  
                                                                              
                                                    
function setActive(tabId) {
  var c = crumbFor(tabId);
  document.querySelectorAll('#shell-sidebar .shell-navitem').forEach(function (n) {
    var on = n.dataset.nav === c.effective;
    n.classList.toggle('active', on);
    if (on) n.setAttribute('aria-current', 'page'); else n.removeAttribute('aria-current');
  });
  var el = PCV.uxlib.el;
  var crumb = document.getElementById('shell-crumb');
  if (crumb) {
    PCV.uxlib.clearEl(crumb);
    crumb.appendChild(el('span', null, 'Single Edge'));
    if (c.section) {
      crumb.appendChild(el('span', { class: 'shell-crumb-sep' }, '/'));
      crumb.appendChild(el('span', null, c.section));
    }
    crumb.appendChild(el('span', { class: 'shell-crumb-sep' }, '/'));
    crumb.appendChild(el('b', null, c.item));
  }
}

PCV.shell = { mount: mount, update: update, setActive: setActive, NAV_SECTIONS: NAV_SECTIONS };
})(window.PCV);
