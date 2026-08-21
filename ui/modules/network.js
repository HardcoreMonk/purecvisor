                                                                  
                                  
                                     
                                                      
                                                                     
  
                           
                                                 
                                                        
                    
                                                               
                                                                        
                                                                     
                                                              
                                                                
                                                  
                                                   
                                                         
                                                            
           
                                                       
                                         
  
                                                      
                                                                     
   
window.PCV = window.PCV || {};
(function(PCV) {

                  
                                                                                     
                                                                                  
                                                                
var _netData = null;
var _netUnsub = null;
                                                                   
                                                  
const NETWORK_MODE_SET_MODES = Object.freeze(['nat', 'isolated', 'routed']);
function _netShown(list) {
  var fs = PCV.ui && PCV.ui.filterState;
  var cur = fs ? fs.current() : {};
  var modes = cur.netmode || [];
  var states = cur.netstate || [];
  return list.filter(function (v) {
    var st = v.state === 'up' ? 'up' : 'down';
    if (modes.length && modes.indexOf(String(v.mode || 'unknown')) === -1) return false;
    if (states.length && states.indexOf(st) === -1) return false;
    return true;
  });
}
                  
                                                                  
                                                                            
                                                                 
                        
function _onNetFilterChange() {
  if (window.currentTab !== 'networks') {
    if (_netUnsub) { _netUnsub(); _netUnsub = null; }
    return;
  }
  if (!_netData) return;
  var ae = document.activeElement;
  var fk = ae && ae.getAttribute ? ae.getAttribute('data-facet') : null;
  var fv = ae && ae.getAttribute ? ae.getAttribute('data-val') : null;
  renderNetworks(PCV.ui.renderTarget(), _netData);
  if (fk && fv) {
    var chips = document.querySelectorAll('#net-inv .chip');
    for (var i = 0; i < chips.length; i++) {
      if (chips[i].getAttribute('data-facet') === fk && chips[i].getAttribute('data-val') === fv) {
        chips[i].focus();
        break;
      }
    }
  }
}
async function renderNetworks(b, cachedList) {
  if (!cachedList) showSkeleton(b);
  try {
                                                                     
    const response = cachedList ? null : await fetchGet(EP.NET_LIST());
                                                               
                                                               
    if (response && response.error) throw new Error(response.error.message || _L('네트워크 목록 조회 실패', 'Unable to load networks'));
    const l = cachedList || unwrapList(response);
    _netData = l;
    if (!_netUnsub && PCV.ui && PCV.ui.filterState && PCV.ui.filterState.subscribe) {
      _netUnsub = PCV.ui.filterState.subscribe(_onNetFilterChange);
    }
    const compactMode = !!(window.matchMedia && window.matchMedia('(max-width: 768px)').matches);
    var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
    var heading = HN.pagehead({
      title: _L('네트워크 인벤토리', 'Network inventory'),
      desc: _L('브리지, DHCP, 외부 연결 상태를 먼저 확인한 뒤 정책 편집으로 이어갑니다.', 'Review bridges, DHCP, and external connectivity first, then move into policy editing.'),
      actions: [el('button', { class: 'btn btn-primary', onclick: 'showNetCreate()', 'data-role': 'ADMIN' }, '+ ' + t('net.new'))]
    });
    if (l.length === 0) {
      clearEl(b);
      b.appendChild(frag(heading,
        el('div', { class: 'empty-state', style: 'text-align:center;padding:40px 20px' },
          el('div', { style: 'font-size:48px;margin-bottom:12px;opacity:.5' }, '🌐'),
          el('div', { style: 'font-size:14px;color:var(--fg2);margin-bottom:16px' }, _L('구성된 네트워크가 없습니다', 'No configured networks')),
          el('button', { class: 'btn btn-primary', onclick: 'showNetCreate()', 'data-role': 'ADMIN' }, '+ ' + _L('네트워크 생성', 'Create network')))));
      if (typeof applyRoleVisibility === 'function') applyRoleVisibility(window.currentUser && window.currentUser.role);
      return;
    }
                                                           
                                                                  
                                                          
    var counts = { mode: {}, up: 0, down: 0 };
    l.forEach(function (v) {
      var m = String(v.mode || 'unknown');                                                    
      counts.mode[m] = (counts.mode[m] || 0) + 1;
      if (v.state === 'up') counts.up++; else counts.down++;
    });
    var CANON = ['nat', 'bridge', 'isolated', 'routed'];
    var extraModes = Object.keys(counts.mode).filter(function (m) { return CANON.indexOf(m) === -1; }).sort();
    var fbar = HN.filterBar([
      { key: 'netmode', label: _L('모드', 'Mode'), options: CANON.concat(extraModes).map(function (m) {
          return { value: m, label: String(m).toUpperCase(), count: counts.mode[m] || 0 };
        }) },
      { key: 'netstate', label: _L('상태', 'State'), options: [
          { value: 'up', label: 'Up', count: counts.up, sw: 'ok' },
          { value: 'down', label: 'Down', count: counts.down, sw: 'crit' }
        ] }
    ]);
    var shown = _netShown(l);
    var body;
    if (compactMode) {
      var cards = shown.map(function(v) {
        var ext = v.mode === 'bridge'
          ? ((v.uplink_mode && v.uplink_mode !== 'none' ? v.uplink_mode.toUpperCase() + ' · ' : '') + (v.phys || '-'))
          : v.mode === 'nat' ? 'NAT' : '-';
        return HN.card(el('b', null, v.name), [
          HN.row(_L('모드', 'Mode'), HN.statusPill('idle', String(v.mode || '?').toUpperCase())),
          HN.row(_L('상태', 'State'), HN.statusPill(v.state === 'up' ? 'ok' : 'crit', String(v.state || '?').toUpperCase())),
          HN.row(_L('외부 연결', 'External'), ext),
          HN.row(_L('호스트 IP', 'Host IP'), v.ip_cidr || '-'),
          HN.row('DHCP', v.dhcp ? 'ON' : '-'),
          HN.row('VM', String(v.vm_count || 0)),
          HN.row(_L('서브넷', 'Subnet'), v.subnet || '-'),
          el('div', { class: 'flex gap-4 ops-action-row', style: 'margin-top:10px', 'data-role': 'ADMIN' },
            el('button', { class: 'btn btn-soft', style: 'font-size:10px;padding:3px 8px', onclick: "showNetEdit('" + escapeAttr(v.name) + "','" + escapeAttr(v.mode) + "','" + escapeAttr(v.ip_cidr) + "'," + (v.dhcp || false) + ",'" + escapeAttr(v.phys || '') + "')" }, t('btn.edit')),
            el('button', { class: 'btn btn-r', style: 'font-size:10px;padding:3px 8px', onclick: "netDel('" + escapeAttr(v.name) + "')" }, t('btn.delete')))
        ], 'ops-mobile-card');
      });
      body = el('div', { class: 'sg grid-2' }, cards);
    } else {
      var thead = el('thead', null, el('tr', null,
        el('th', null, _L('네트워크', 'Network')), el('th', null, _L('모드', 'Mode')), el('th', null, _L('상태', 'State')),
        el('th', null, _L('외부 연결', 'External')), el('th', null, _L('호스트 IP', 'Host IP')), el('th', null, 'DHCP'),
        el('th', null, 'VM'), el('th', null, _L('서브넷', 'Subnet')), el('th', null, t('vm.settings'))));
      var rows = shown.map(function(v) {
        var ext = v.mode === 'bridge'
          ? ((v.uplink_mode && v.uplink_mode !== 'none' ? v.uplink_mode.toUpperCase() + ' · ' : '') + (v.phys || '-'))
          : v.mode === 'nat' ? 'NAT' : '-';
        return el('tr', null,
          el('td', null, el('b', null, v.name)),
          el('td', null, HN.statusPill('idle', String(v.mode || '?').toUpperCase())),
          el('td', null, HN.statusPill(v.state === 'up' ? 'ok' : 'crit', String(v.state || '?').toUpperCase())),
          el('td', null, ext),
          el('td', null, v.ip_cidr || '-'),
          el('td', null, v.dhcp ? 'ON' : '-'),
          el('td', null, v.vm_count || 0),
          el('td', null, v.subnet || '-'),
          el('td', { class: 'nowrap', 'data-role': 'ADMIN' },
            el('button', { class: 'btn btn-soft', style: 'font-size:10px;padding:3px 8px', onclick: "showNetEdit('" + escapeAttr(v.name) + "','" + (escapeAttr(v.mode) || '') + "','" + (escapeAttr(v.ip_cidr) || '') + "'," + (v.dhcp || false) + ",'" + (escapeAttr(v.phys) || '') + "')" }, t('btn.edit')),
            ' ',
            el('button', { class: 'btn btn-r', style: 'font-size:10px;padding:3px 8px', onclick: "netDel('" + escapeAttr(v.name) + "')" }, t('btn.delete'))));
      });
      body = el('table', { class: 'table-sticky' }, thead, el('tbody', null, rows));
    }
    var fwPanel = el('div', { class: 'sg grid-2', style: 'margin-top:16px' },
      el('div', { class: 'hc' },
        el('h4', { role: 'heading', 'aria-level': '3' }, _L('방화벽 정책 편집', 'Firewall policy editor')),
        el('p', { class: 'color-muted text-11 mb-8' }, _L('브리지나 세그먼트를 확인한 뒤 인바운드/아웃바운드 규칙을 추가합니다.', 'Add ingress or egress rules after checking the bridge or segment you are editing.')),
        el('div', { class: 'flex gap-8 mb-8 ops-form-strip', style: 'flex-wrap:wrap', 'data-role': 'ADMIN' },
          el('select', { id: 'fw-direction', 'aria-label': 'Direction', class: 'input', style: 'width:110px' },
            el('option', { value: 'ingress' }, _L('인바운드', 'Ingress')),
            el('option', { value: 'egress' }, _L('아웃바운드', 'Egress'))),
          el('select', { id: 'fw-protocol', 'aria-label': 'Protocol', class: 'input w-80' },
            el('option', null, 'tcp'), el('option', null, 'udp'), el('option', null, 'icmp')),
          el('input', { 'aria-label': _L('포트 예: 80 또는 8080-8090', 'Port e.g. 80 or 8080-8090'), id: 'fw-port', class: 'input', placeholder: _L('포트 예: 80 또는 8080-8090', 'Port e.g. 80 or 8080-8090') }),
          el('input', { 'aria-label': _L('소스 CIDR', 'Source CIDR'), id: 'fw-source', class: 'input', placeholder: _L('소스 CIDR', 'Source CIDR'), value: '0.0.0.0/0' }),
          el('button', { class: 'btn btn-primary', onclick: 'fwAddRule()' }, _L('규칙 추가', 'Add rule'))),
        el('div', { id: 'fw-rules-list' })),
      el('div', { class: 'hc' },
        el('h4', { role: 'heading', 'aria-level': '3' }, _L('OVN ACL 운영 메모', 'OVN ACL operations note')),
        el('p', { class: 'color-muted text-11 mb-8' }, _L('싱글 엣지에서는 상태를 먼저 확인하고, 필요한 경우 수동 ACL 명령으로 보강합니다.', 'In Single Edge, check state first and use manual ACL commands only when you need to refine policy.')),
        el('pre', { style: 'background:var(--bg3);padding:8px;border-radius:6px;font-size:11px;overflow-x:auto' },
          'pcvctl ovn acl list <switch>\npcvctl ovn acl add <switch> to-lport 1000 "ip4.src==10.0.0.0/24" allow')));
                                                       
                                                              
                                 
    var invContent = frag(heading, fbar, body);
    var invHost = cachedList ? document.getElementById('net-inv') : null;
    if (invHost) {
      clearEl(invHost);
      invHost.appendChild(invContent);
      if (typeof applyRoleVisibility === 'function') applyRoleVisibility(window.currentUser && window.currentUser.role);
      return;
    }
    clearEl(b);
    b.appendChild(frag(el('div', { id: 'net-inv' }, invContent), fwPanel));
    if (typeof applyRoleVisibility === 'function') applyRoleVisibility(window.currentUser && window.currentUser.role);
  } catch (e) {
    if(_DEBUG) console.warn('n:', e.message);
    PCV.uxlib.renderLoadError(b, {
      title: _L('네트워크 인벤토리', 'Network inventory'),
      message: e.message,
      retry: function() { renderNetworks(b); }
    });
  }
}
function toggleFwPanel() { var p = document.getElementById('fw-panel'); if (p) p.classList.toggle('hidden'); }
function toggleAclPanel() { var p = document.getElementById('acl-panel'); if (p) p.classList.toggle('hidden'); }
window.toggleFwPanel = toggleFwPanel;
window.toggleAclPanel = toggleAclPanel;

function showNetCreate() {
  var el = PCV.uxlib.el;
  showModal([
    el('h2', null, t('net.new')),
    el('div', { class: 'fr' }, el('label', { for: 'nn' }, 'Bridge'), el('input', { id: 'nn', placeholder: 'pcvbr0', oninput: 'updateNetCreateState()' })),
    el('div', { class: 'fr' }, el('label', { for: 'nm' }, 'Mode'),
      el('select', { id: 'nm', onchange: 'netModeChanged()' },
        el('option', { value: 'nat' }, t('net.mode.nat')),
        el('option', { value: 'isolated' }, t('net.mode.isolated')),
        el('option', { value: 'routed' }, t('net.mode.routed')),
        el('option', { value: 'bridge' }, t('net.mode.bridge')))),
    el('div', { id: 'net-cidr-row', class: 'fr' }, el('label', { for: 'nc' }, 'CIDR'), el('input', { id: 'nc', placeholder: '10.0.0.1/24', oninput: 'updateNetCreateState()' })),
    el('div', { id: 'net-phys-row', class: 'fr hidden' },
      el('label', { for: 'np' }, _L('물리 NIC', 'Physical NIC')),
      el('input', { id: 'np', placeholder: 'enp42s0', autocomplete: 'off',
        'aria-describedby': 'net-bridge-help', oninput: 'updateNetCreateState()' })),
    el('div', { id: 'net-uplink-row', class: 'fr hidden' },
      el('label', null, _L('업링크 방식', 'Uplink mode')),
      el('div', { style: 'display:grid;gap:8px;flex:1;min-width:0' },
        el('label', { style: 'display:flex;gap:8px;align-items:flex-start;cursor:pointer;width:auto' },
          el('input', { id: 'net-uplink-dedicated', type: 'radio', name: 'net-uplink-mode', value: 'dedicated', checked: '', style: 'flex:none;width:14px;height:14px;margin-top:2px;accent-color:var(--accent)', onchange: 'netUplinkChanged()' }),
          el('span', null, el('b', null, _L('전용 업링크', 'Dedicated uplink')), el('br'),
            el('span', { class: 'color-muted text-11' }, _L('미사용 NIC를 Linux bridge 포트로 전환', 'Convert an unused NIC into a Linux bridge port')))),
        el('label', { style: 'display:flex;gap:8px;align-items:flex-start;cursor:pointer;width:auto' },
          el('input', { id: 'net-uplink-shared', type: 'radio', name: 'net-uplink-mode', value: 'shared', style: 'flex:none;width:14px;height:14px;margin-top:2px;accent-color:var(--accent)', onchange: 'netUplinkChanged()' }),
          el('span', null, el('b', null, _L('공유 NIC (VMware식)', 'Shared NIC (VMware-style)')), el('br'),
            el('span', { class: 'color-muted text-11' }, _L('호스트 IP·경로를 NIC에 유지하고 VM L2 프레임만 중계', 'Keep host IP and routes on the NIC; relay only VM L2 frames')))))),
    el('div', { id: 'net-bridge-help', class: 'net-mode-impact net-mode-impact-warn hidden',
      'aria-live': 'polite' },
      _L('관리 NIC를 선택하면 호스트 연결이 끊길 수 있습니다. 호스트 IP와 기본 경로가 없는 미사용 Ethernet NIC만 입력하세요. 브리지에는 호스트 IP가 할당되지 않으며 서버가 적용 전에 다시 검사합니다.',
         'Selecting a management NIC can disconnect this host. Enter only an unused Ethernet NIC with no host IP or default route. The bridge remains unnumbered and the server rechecks it before applying.')),
    el('div', { id: 'net-bridge-ack-row', class: 'net-mode-contract-note hidden' },
      el('label', { for: 'net-bridge-ack', style: 'display:flex;gap:8px;align-items:flex-start;color:var(--fg);font-size:11px;line-height:1.5;cursor:pointer' },
        el('input', { id: 'net-bridge-ack', type: 'checkbox', style: 'flex:none;width:14px;height:14px;margin-top:1px;accent-color:var(--accent)', onchange: 'updateNetCreateState()' }),
        el('span', { id: 'net-bridge-ack-text' }, _L('이 NIC는 관리용이 아니며 호스트 IP·기본 경로가 없는 전용 L2 업링크입니다.',
                           'This NIC is not used for management and is a dedicated L2 uplink with no host IP or default route.')))),
    el('div', { id: 'net-create-error', class: 'vpc-error hidden', role: 'alert',
      'aria-live': 'assertive' }),
    el('div', { class: 'stat-label net-mode-contract-note', id: 'net-mode-hint', 'aria-live': 'polite' }, 'NAT: MASQUERADE + DHCP'),
    el('div', { class: 'net-mode-actions mt-12' },
      el('button', { class: 'btn btn-soft', onclick: 'closeModal()' }, t('btn.cancel')),
      el('button', { class: 'btn btn-primary', onclick: 'doNetCreate()', 'data-net-create': '', disabled: '' }, t('btn.create')))
  ]);
  updateNetCreateState();
}

function loadPhysNics() {
                                                                    
                                                             
  var input = document.getElementById('np');
  if (input) input.placeholder = 'enp42s0';
}

function netModeChanged() {
  const m = document.getElementById('nm').value;
  const bridge = m === 'bridge';
  const cidrRow = document.getElementById('net-cidr-row');
  const cidr = document.getElementById('nc');
  const physRow = document.getElementById('net-phys-row');
  const uplinkRow = document.getElementById('net-uplink-row');
  const help = document.getElementById('net-bridge-help');
  const ackRow = document.getElementById('net-bridge-ack-row');
  const ack = document.getElementById('net-bridge-ack');
  const hint = document.getElementById('net-mode-hint');
  cidrRow.classList.toggle('hidden', bridge);
  cidr.disabled = bridge;
  physRow.classList.toggle('hidden', !bridge);
  uplinkRow.classList.toggle('hidden', !bridge);
  help.classList.toggle('hidden', !bridge);
  ackRow.classList.toggle('hidden', !bridge);
  if (!bridge) ack.checked = false;
  if (bridge) {
    loadPhysNics();
    netUplinkChanged();
  } else {
    const hints = { nat: 'NAT: MASQUERADE + DHCP', isolated: 'Isolated: VM-to-VM only', routed: 'Routed: ip_forward only' };
    hint.textContent = hints[m] || '';
  }
  updateNetCreateState();
}

function netUplinkChanged() {
  const shared = !!document.getElementById('net-uplink-shared')?.checked;
  const help = document.getElementById('net-bridge-help');
  const ack = document.getElementById('net-bridge-ack');
  const ackText = document.getElementById('net-bridge-ack-text');
  const hint = document.getElementById('net-mode-hint');
  if (ack) ack.checked = false;
  if (help) help.textContent = shared
    ? _L('호스트 IP·기본 경로·DNS는 물리 NIC에 그대로 유지됩니다. 유선 Ethernet만 지원하며, 상위 스위치가 포트당 여러 MAC 주소를 허용해야 합니다. Wi-Fi·bond·VLAN·trunk에는 적용할 수 없습니다.',
         'Host IP, default routes, and DNS stay on the physical NIC. Wired Ethernet only; the upstream switch must allow multiple MAC addresses on the port. Wi-Fi, bonds, VLANs, and trunks are unsupported.')
    : _L('관리 NIC를 선택하면 호스트 연결이 끊길 수 있습니다. 호스트 IP와 기본 경로가 없는 미사용 Ethernet NIC만 입력하세요. 브리지에는 호스트 IP가 할당되지 않으며 서버가 적용 전에 다시 검사합니다.',
         'Selecting a management NIC can disconnect this host. Enter only an unused Ethernet NIC with no host IP or default route. The bridge remains unnumbered and the server rechecks it before applying.');
  if (ackText) ackText.textContent = shared
    ? _L('이 유선 NIC의 호스트 네트워크를 유지한 채 VM 트래픽 공유를 적용하며, 상위 스위치의 다중 MAC 정책을 확인했습니다.',
         'Apply VM traffic sharing while preserving this wired NIC host network; the upstream switch multiple-MAC policy has been checked.')
    : _L('이 NIC는 관리용이 아니며 호스트 IP·기본 경로가 없는 전용 L2 업링크입니다.',
         'This NIC is not used for management and is a dedicated L2 uplink with no host IP or default route.');
  if (hint) hint.textContent = shared
    ? _L('공유 NIC 브리지: 호스트 L3 유지 · TC-BPF 포털 · 외부 DHCP',
         'Shared NIC bridge: host L3 preserved · TC-BPF portal · external DHCP')
    : _L('전용 L2 브리지: 호스트 IP·DHCP 없음, 재부팅 후 안전 복구',
         'Dedicated L2 bridge: no host IP or DHCP, safely restored after reboot');
  updateNetCreateState();
}

function updateNetCreateState() {
  const button = document.querySelector('[data-net-create]');
  if (!button || button.getAttribute('aria-busy') === 'true') return;
  const name = (document.getElementById('nn')?.value || '').trim();
  const mode = document.getElementById('nm')?.value || 'nat';
  const cidr = (document.getElementById('nc')?.value || '').trim();
  const physical = (document.getElementById('np')?.value || '').trim();
  const acknowledged = !!document.getElementById('net-bridge-ack')?.checked;
  button.disabled = !name || (mode === 'bridge' ? (!physical || !acknowledged) : !cidr);
}

function setNetCreateError(message) {
  const panel = document.getElementById('net-create-error');
  if (!panel) return;
  panel.textContent = message || '';
  panel.classList.toggle('hidden', !message);
}

async function doNetCreate() {
  var button = document.querySelector('[data-net-create]');
  if (!button || button.disabled || button.getAttribute('aria-busy') === 'true') return;
  const mode = document.getElementById('nm').value;
  const body = { bridge_name: document.getElementById('nn').value.trim(), mode: mode };
  if (mode === 'bridge') {
    const physical = document.getElementById('np').value.trim();
    const uplinkMode = document.getElementById('net-uplink-shared')?.checked ? 'shared' : 'dedicated';
    if (!physical || !document.getElementById('net-bridge-ack').checked) {
      toast(_L('업링크 NIC와 선택한 방식의 안전 확인이 필요합니다.', 'The uplink NIC and matching safety acknowledgement are required.'), false);
      return;
    }
    body.physical_if = physical;
    body.uplink_mode = uplinkMode;
    body.safety_ack = uplinkMode + '-uplink';
  } else {
    body.cidr = document.getElementById('nc').value.trim();
  }
  setNetCreateError('');
                                                                     
                                                 
  button.setAttribute('aria-disabled', 'true');
  button.setAttribute('aria-busy', 'true');
  button.style.transition = 'none';
  button.style.opacity = '1';
  button.style.filter = 'none';
  button.style.cursor = 'progress';
  const originalLabel = button.textContent;
  button.textContent = _L('생성 중…', 'Creating…');
  try {
    var navGen = PCV.ui.navGen();
    const response = await fetchPost(EP.NET_LIST(), body);
    if (response.error) {
      setNetCreateError(response.error.message || _L('네트워크 생성에 실패했습니다.', 'Network creation failed.'));
      return;
    }
    toast(t('net.created'));
    addEvt(t('net.created'));
    closeModal();
    if (PCV.ui.navGen() === navGen) renderNetworks(PCV.ui.renderTarget());
  } catch (error) {
    setNetCreateError(error.message || _L('네트워크 생성에 실패했습니다.', 'Network creation failed.'));
  } finally {
    if (button.isConnected) {
      button.removeAttribute('aria-disabled');
      button.removeAttribute('aria-busy');
      button.style.removeProperty('transition');
      button.style.removeProperty('opacity');
      button.style.removeProperty('filter');
      button.style.removeProperty('cursor');
      button.textContent = originalLabel;
      updateNetCreateState();
    }
  }
}

               
                                               
                                                
                                                                       
                                                     
                                           
                 
async function netDel(name) {
  var el = PCV.uxlib.el;
  showModal([
    el('h2', { class: 'color-red' }, '\u{26A0} ', t('btn.delete'), ' Network'),
    el('p', { class: 'mb-12' }, t('vm.delete.confirm').replace('VM', 'Network'), ' ', el('b', { class: 'color-accent' }, name)),
    el('p', { class: 'mb-12' }, t('vm.delete.type_name').replace('VM', 'Network')),
                                                                  
                                                               
                                                      
                                    
    el('div', { class: 'fr' }, el('label', { for: 'del-net-confirm' }, 'Name'), el('input', { id: 'del-net-confirm', placeholder: escapeHtml(name) })),
    el('div', { class: 'text-right mt-14' },
      el('button', { class: 'btn btn-r', onclick: "doNetDel('" + escapeAttr(name) + "')" }, t('btn.delete')),
      ' ',
      el('button', { class: 'btn', onclick: 'closeModal()' }, t('btn.cancel')))
  ]);
}

async function doNetDel(name) { const cv = document.getElementById('del-net-confirm')?.value; if (cv !== name) { toast(t('vm.name_mismatch'), false); return; }
                                                                    
                                                                   
                                                                            
                                                                        
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  const mc = PCV.modalCore.currentBody();
  clearEl(mc);
  mc.appendChild(frag(
    el('h2', { class: 'color-red' }, '⚠ Deleting Network'),
    el('p', null, el('b', { class: 'color-accent' }, name)),
    el('div', { class: 'prog-bar' }, el('div', { class: 'prog-fill', id: 'dn-p' })),
    el('div', { class: 'prog-status', id: 'dn-s' }, el('span', { class: 'spinner' }), 'Removing firewall rules & DHCP...')
  ));
  const pf = document.getElementById('dn-p'), ps = document.getElementById('dn-s');
  try { pf.style.width = '50%'; PCV.uxlib.setMsg(ps, 'loading', null, 'Sending DELETE...');
    var _navGen = PCV.ui.navGen();
                                                                           
                                                       
                                                                 
                                                  
                                                            
    const d = await fetchDelete(EP.NET_DETAIL(name)).catch(() => ({}));
    if (d.error) { pf.style.background = 'var(--red)'; pf.style.width = '100%'; ps.textContent = '❌ ' + d.error.message; toast(t('btn.delete') + ' failed', false); return; }
    pf.style.width = '100%'; ps.textContent = '✅ ' + t('net.deleted'); toast(t('net.deleted')); addEvt(t('net.deleted') + ': ' + name); setTimeout(() => { closeModal(); if (PCV.ui.navGen() === _navGen) renderNetworks(PCV.ui.renderTarget()); }, 1500);
  } catch (e) { pf.style.width = '100%'; ps.textContent = '❌ ' + e.message; toast(e.message, false); } }

function showNetEditor() { navigateTo('networks'); }

function showNetEdit(name, mode, cidr, dhcp, phys) {
  var el = PCV.uxlib.el;
  var supportedCurrent = NETWORK_MODE_SET_MODES.indexOf(mode) !== -1;
  var immutablePhysical = mode === 'bridge' && !!phys && phys !== '-';
  var originalCidr = String(cidr || '').trim();
  var currentModePill = HN.statusPill('idle', String(mode || 'unknown').toUpperCase());
  currentModePill.setAttribute('data-net-current-mode', '');
  var modeOptions = [
    el('option', {
      value: '', disabled: '', selected: supportedCurrent ? null : ''
    }, _L('새 모드를 선택하세요', 'Select a supported mode'))
  ].concat(NETWORK_MODE_SET_MODES.map(function (allowedMode) {
    return el('option', {
      value: allowedMode,
      selected: mode === allowedMode ? '' : null
    }, allowedMode);
  }));

                                                            
                                                                 
  showModal([
    el('h2', null, _L('네트워크 모드 변경', 'Change network mode'), ': ',
      el('span', { class: 'net-mode-target' }, name)),
    el('div', { class: 'fr net-mode-form' },
      el('label', { for: 'net-bridge' }, _L('네트워크', 'Network')),
      el('input', { id: 'net-bridge', class: 'net-mode-network-input', value: name, disabled: '' })),
    el('div', { class: 'net-mode-readonly' },
      el('span', { class: 'net-mode-key' }, _L('현재 모드', 'Current mode')),
      el('span', { class: 'net-mode-value' }, currentModePill)),
    el('div', { class: 'fr net-mode-form' }, el('label', { for: 'ne-mode' }, _L('새 모드', 'New mode')),
      el('select', {
        id: 'ne-mode', onChange: netEditModeChanged,
        'data-net-original-mode': supportedCurrent ? mode : '',
        'data-net-immutable-physical': immutablePhysical ? 'true' : 'false',
        disabled: immutablePhysical ? '' : null,
        'aria-describedby': 'ne-impact'
      }, modeOptions)),
    el('div', {
      id: 'ne-impact', class: 'net-mode-impact',
      'data-net-mode-impact': '', 'aria-live': 'polite'
    }),
                                                                        
                                                    
                                                   
                                              
                                                     
    el('div', { class: 'fr net-mode-form' }, el('label', { for: 'ne-cidr' }, _L('대역(CIDR)', 'Subnet (CIDR)')),
      el('input', {
        id: 'ne-cidr', value: originalCidr, placeholder: '10.0.0.1/24',
        onInput: netEditModeChanged,
        disabled: immutablePhysical ? '' : null,
        'data-net-original-cidr': originalCidr,
        'aria-describedby': 'ne-cidr-help'
      })),
    el('div', { class: 'stat-label net-mode-helper', id: 'ne-cidr-help' },
      _L('모드 변경에도 필요합니다. 비워두면 변경이 거부됩니다.', 'Required even for a mode-only change. Leaving it blank is rejected by the server.')),
    el('div', { class: 'net-mode-readonly' },
      el('span', { class: 'net-mode-key' }, _L('현재 DHCP', 'Current DHCP')),
      el('span', { class: 'net-mode-value' }, dhcp ? 'ON' : 'OFF')),
    el('div', { class: 'net-mode-readonly' },
      el('span', { class: 'net-mode-key' }, 'Physical NIC'),
      el('span', { class: 'net-mode-value' }, phys || '-')),
    el('div', {
      class: 'stat-label net-mode-contract-note',
      id: 'ne-hint', 'data-net-mode-contract-note': ''
    }, immutablePhysical ? _L(
      '전용 물리 브리지는 live 모드 변경을 지원하지 않습니다. 안전하게 삭제한 뒤 원하는 모드로 다시 생성하세요.',
      'A dedicated physical bridge cannot be changed live. Delete it safely, then recreate it in the required mode.'
    ) : _L(
      'DHCP는 모드에 맞춰 서버가 자동 동기화합니다. Physical NIC 연결은 여기서 바뀌지 않습니다.',
      'DHCP is synchronized automatically for the selected mode. Physical NIC binding is not changed here.'
    )),
    el('div', { class: 'net-mode-actions mt-14' },
      el('button', { class: 'btn btn-soft', onClick: function () { closeModal(); } }, t('btn.cancel')),
      el('button', {
        class: 'btn btn-primary', disabled: '',
        'data-net-edit-apply': '', onClick: function () { doNetEdit(name); }
      }, t('btn.apply')))
  ]);
  netEditModeChanged();
}

function netEditImpactText(mode) {
  if (mode === 'nat') {
    return _L(
      'NAT는 MASQUERADE와 DHCP를 활성화하고 서버 상태를 즉시 재수렴합니다.',
      'NAT enables MASQUERADE and DHCP, then immediately reconverges the server state.'
    );
  }
  if (mode === 'isolated') {
    return _L(
      'Isolated는 외부 경로를 차단하고 내부 네트워크용 DHCP를 재수렴합니다.',
      'Isolated blocks external routes and reconverges DHCP for the internal network.'
    );
  }
  if (mode === 'routed') {
    return _L(
      'Routed는 이 네트워크의 DHCP를 즉시 중단합니다. VM에는 별도 주소 할당과 상위 정적 라우팅이 필요합니다.',
      'Routed immediately stops DHCP for this network. VMs require separate address assignment and upstream static routing.'
    );
  }
  return _L(
    '지원 모드를 선택하면 적용 영향이 여기에 표시됩니다.',
    'Select a supported mode to review its impact.'
  );
}

function netEditModeChanged() {
  const modeEl = document.getElementById('ne-mode');
  const cidrEl = document.getElementById('ne-cidr');
  const applyEl = document.querySelector('[data-net-edit-apply]');
  const impactEl = document.querySelector('[data-net-mode-impact]');
  const mode = modeEl ? modeEl.value : '';
  const cidr = cidrEl ? cidrEl.value.trim() : '';
  const originalMode = modeEl ? modeEl.getAttribute('data-net-original-mode') || '' : '';
  const originalCidr = cidrEl ? cidrEl.getAttribute('data-net-original-cidr') || '' : '';
  const immutablePhysical = modeEl?.getAttribute('data-net-immutable-physical') === 'true';
  const supported = NETWORK_MODE_SET_MODES.indexOf(mode) !== -1;
  const changed = mode !== originalMode || cidr !== originalCidr;
  if (applyEl && applyEl.getAttribute('aria-busy') !== 'true') {
    applyEl.disabled = immutablePhysical || !supported || !cidr || !changed;
  }
  if (impactEl) {
    impactEl.textContent = immutablePhysical
      ? _L('물리 업링크·호스트 L3를 부분 변경하지 않도록 서버도 이 전환을 거부합니다.',
           'The server also rejects this transition to prevent partial uplink or host L3 mutation.')
      : netEditImpactText(mode);
    impactEl.classList.toggle('net-mode-impact-warn', immutablePhysical || mode === 'routed');
  }
}

               
                                                         
                                                  
                                                                
                                                      
                                                  
                                                                     
                        
async function doNetEdit(name) {
  const modeEl = document.getElementById('ne-mode');
  const cidrEl = document.getElementById('ne-cidr');
  const applyEl = document.querySelector('[data-net-edit-apply]');
  const mode = modeEl ? modeEl.value : '';
  const cidr = cidrEl ? cidrEl.value.trim() : '';
  if (modeEl?.getAttribute('data-net-immutable-physical') === 'true') {
    toast(_L('전용 물리 브리지는 live 모드 변경을 지원하지 않습니다.',
             'Dedicated physical bridges cannot be changed live.'), false);
    return;
  }
  if (NETWORK_MODE_SET_MODES.indexOf(mode) === -1) {
    toast(_L('지원하지 않는 네트워크 모드입니다.', 'Unsupported network mode.'), false);
    return;
  }
  if (!cidr) {
    toast(_L('CIDR 대역을 입력하세요.', 'CIDR is required for a mode change.'), false);
    return;
  }
  const originalMode = modeEl ? modeEl.getAttribute('data-net-original-mode') || '' : '';
  const originalCidr = cidrEl ? cidrEl.getAttribute('data-net-original-cidr') || '' : '';
  if (mode === originalMode && cidr === originalCidr) {
    toast(_L('변경된 값이 없습니다.', 'No changes to apply.'));
    return;
  }
  if (applyEl && applyEl.getAttribute('aria-busy') === 'true') return;
  if (applyEl) {
    applyEl.disabled = true;
    applyEl.setAttribute('aria-busy', 'true');
    applyEl.setAttribute('data-net-apply-label', applyEl.textContent || '');
    applyEl.textContent = _L('적용 중…', 'Applying…');
  }
  try {
    var _navGen = PCV.ui.navGen();
    const mr = await fetchPost(EP.NET_MODE(name), { mode: mode, cidr: cidr });
    if (mr.error) { toast('Failed: ' + (mr.error.message || ''), false); return; }
    toast(name + ' updated');
    addEvt('Network edit: ' + name);
    closeModal();
    if (PCV.ui.navGen() === _navGen) renderNetworks(PCV.ui.renderTarget());
  } catch (e) {
    toast('Edit failed: ' + e.message, false);
  } finally {
    if (applyEl && document.contains(applyEl)) {
      applyEl.removeAttribute('aria-busy');
      applyEl.textContent = applyEl.getAttribute('data-net-apply-label') || t('btn.apply');
      applyEl.removeAttribute('data-net-apply-label');
      netEditModeChanged();
    }
  }
}

                 
async function renderOvn(b) {
  showSkeleton(b);
  try {
    const st = await fetchGet(EP.OVN_STATUS()); const sd = unwrapData(st);
    const sw = await fetchGet(EP.OVN_SWITCHES()); const sl = unwrapList(sw);
    const rt = await fetchGet(EP.OVN_ROUTERS()); const rl = unwrapList(rt);
    var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
    var heading1 = HN.pagehead({
      title: 'OVN SDN',
      desc: _L('싱글 엣지에서 허용된 로컬 OVN 상태와 수동 구성을 확인합니다.', 'Review the local OVN state and the manually managed configuration allowed in Single Edge.')
    });
    var grid1 = el('div', { class: 'sg grid-3' },
      HN.card(_L('OVN 가용성', 'OVN availability'), sd.available ? [HN.statusPill('ok', _L('준비됨', 'Ready')), HN.row('Encap', 'Geneve')] : el('p', { class: 'color-muted text-xs' }, _L('OVN이 설치되지 않았습니다', 'OVN is not installed'))),
      HN.card(_L('논리 스위치', 'Logical switches'), [el('div', { class: 'stat-lg color-accent' }, sl.length), HN.row(_L('상태', 'State'), HN.statusPill(sd.available ? 'ok' : 'crit', sd.available ? _L('조회 가능', 'Available') : _L('미설치', 'Unavailable')))]),
      HN.card(_L('논리 라우터', 'Logical routers'), [el('div', { class: 'stat-lg color-green' }, rl.length), HN.row(_L('운영 방식', 'Mode'), _L('수동 구성', 'Manual configuration'))]));
    var heading3 = el('div', { class: 'ops-section-heading' },
      el('div', null, el('h3', null, _L('논리 토폴로지', 'Logical topology')), el('p', null, _L('스위치와 라우터 목록을 먼저 확인한 뒤 수동 정책과 부가 구성을 적용합니다.', 'Review switches and routers first, then apply manual policy and optional configuration.'))));
    var switchPanel = el('div', { class: 'hc' },
      el('h4', { role: 'heading', 'aria-level': '3' }, _L('논리 스위치', 'Logical switches') + ' (' + sl.length + ')'),
      sl.length === 0
        ? el('p', { class: 'color-muted text-xs' }, _L('구성된 스위치가 없습니다', 'No switches configured'), el('br'), el('span', { class: 'text-12' }, _L('로컬 정책을 넣기 전, 현재 토폴로지가 비어 있는지 먼저 확인하십시오.', 'Confirm the topology is intentionally empty before adding local policy.')))
        : el('table', { class: 'table-sticky' },
            el('thead', null, el('tr', null, el('th', null, _L('이름', 'Name')))),
            el('tbody', null, sl.map(function(s) { const n = typeof s === 'string' ? s : (s.name || s.entry || JSON.stringify(s)); return el('tr', null, el('td', null, n)); }))));
    var routerPanel = el('div', { class: 'hc' },
      el('h4', { role: 'heading', 'aria-level': '3' }, _L('논리 라우터', 'Logical routers') + ' (' + rl.length + ')'),
      rl.length === 0
        ? el('p', { class: 'color-muted text-xs' }, _L('구성된 라우터가 없습니다', 'No routers configured'), el('br'), el('span', { class: 'text-12' }, _L('필요한 경우에만 수동 라우터를 추가하십시오.', 'Create a router only when the local design requires it.')))
        : el('table', { class: 'table-sticky' },
            el('thead', null, el('tr', null, el('th', null, _L('이름', 'Name')))),
            el('tbody', null, rl.map(function(r) { const n = typeof r === 'string' ? r : (r.name || r.entry || JSON.stringify(r)); return el('tr', null, el('td', null, n)); }))));
    var topoGrid = el('div', { class: 'sg grid-2' }, switchPanel, routerPanel);
    var lbPanel = el('div', { class: 'hc' },
      el('h4', { role: 'heading', 'aria-level': '3' }, '⚖ ' + _L('로드 밸런서 설정', 'LB setup')),
      el('p', { class: 'color-muted text-11 mb-8' }, _L('VIP와 백엔드 목록을 수동으로 입력해 단일 노드 로컬 구성을 점검합니다.', 'Enter the VIP and backend list manually to validate the local single-node setup.')),
      el('div', { class: 'mb-8 ops-stack-form', 'data-role': 'ADMIN' },
        el('div', { class: 'fr' }, el('label', { for: 'lb-n' }, _L('이름', 'Name')), el('input', { id: 'lb-n', placeholder: 'edge-lb' })),
        el('div', { class: 'fr' }, el('label', { for: 'lb-vip' }, 'VIP:Port'), el('input', { id: 'lb-vip', placeholder: '10.0.0.100' }), el('input', { id: 'lb-port', 'aria-label': 'Port', type: 'number', value: '80', class: 'w-60' })),
        el('div', { class: 'fr' }, el('label', { for: 'lb-bk' }, _L('백엔드', 'Backends')), el('input', { id: 'lb-bk', placeholder: '10.0.0.1:80,10.0.0.2:80' })),
        el('button', { class: 'btn btn-primary', onclick: 'nfvLbCreate()', 'data-role': 'ADMIN' }, _L('LB 생성', 'Create LB'))),
      el('div', { id: 'lb-list' }));
    var aclPanel = el('div', { class: 'hc' },
      el('h4', { role: 'heading', 'aria-level': '3' }, '🛡 ' + _L('ACL 정책 추가', 'ACL policy add')),
      el('p', { class: 'color-muted text-11 mb-8' }, _L('스위치 단위로 방향, 우선순위, 매치 조건을 입력해 ACL을 추가합니다.', 'Add ACLs per switch with direction, priority, and match conditions.')),
      el('div', { class: 'mb-8 ops-stack-form', 'data-role': 'ADMIN' },
        el('div', { class: 'fr' }, el('label', { for: 'fw-sw' }, _L('스위치', 'Switch')), el('input', { id: 'fw-sw', placeholder: 'web-tier' })),
        el('div', { class: 'fr' }, el('label', { for: 'fw-dir' }, _L('방향', 'Direction')), el('select', { id: 'fw-dir' }, el('option', null, 'from-lport'), el('option', null, 'to-lport'))),
        el('div', { class: 'fr' }, el('label', { for: 'fw-pri' }, _L('우선순위', 'Priority')), el('input', { id: 'fw-pri', type: 'number', value: '1000' })),
        el('div', { class: 'fr' }, el('label', { for: 'fw-match' }, 'Match'), el('input', { id: 'fw-match', placeholder: 'ip4.src==10.0.0.0/24' })),
        el('div', { class: 'fr' }, el('label', { for: 'fw-act' }, _L('동작', 'Action')), el('select', { id: 'fw-act' }, el('option', null, 'allow'), el('option', null, 'drop'), el('option', null, 'reject'))),
        el('button', { class: 'btn btn-primary', onclick: 'nfvFwAdd()', 'data-role': 'ADMIN' }, _L('ACL 규칙 추가', 'Add ACL rule'))));
    var lbAclGrid = el('div', { class: 'sg grid-2' }, lbPanel, aclPanel);
    clearEl(b);
    b.appendChild(frag(heading1, grid1, heading3, topoGrid, lbAclGrid));
    loadLBList();
    if (typeof applyRoleVisibility === 'function') applyRoleVisibility(window.currentUser && window.currentUser.role);
  } catch (e) { PCV.uxlib.setMsg(b, null, { tag: 'p', cls: 'color-red' }, _L('OVN 정보를 불러오지 못했습니다', 'Unable to load OVN data')); }
}

async function loadLBList() { try { const el = document.getElementById('lb-list'); if (el) PCV.uxlib.setMsg(el, null, { tag: 'p', cls: 'color-muted text-xs' }, _L('로드 밸런서 상태 메모', 'Load balancer status note'), ': ', _L('위 가용성 카드에서 OVN 상태를 먼저 확인한 뒤 LB를 추가하십시오.', 'Confirm OVN availability above before adding a load balancer.')); } catch (e) { if(_DEBUG) console.warn('loadLBList:', e.message); } }
                           
                                                                            
                                                                    
                                     
                                                                             
                                                          
                                                                        
                                                      
                                                         
                                                                
                                                       
  
                       
                                                            
                                                       
                                                 
   
async function nfvLbCreate() {
  try {
    var name = document.getElementById('nfv-lb-name')?.value?.trim() || document.getElementById('lb-n')?.value?.trim();
    var vip = document.getElementById('nfv-lb-vip')?.value?.trim() || document.getElementById('lb-vip')?.value?.trim();
    var members = document.getElementById('nfv-lb-members')?.value?.trim() || document.getElementById('lb-bk')?.value?.trim();
    var port = Number(document.getElementById('nfv-lb-port')?.value || document.getElementById('lb-port')?.value);
    var backends = members ? members.split(',').map(function(s){return s.trim();}).filter(Boolean) : [];
    if (!name || !vip || !Number.isInteger(port) || port < 1 || port > 65535 || backends.length === 0) {
      toast(_L('이름, VIP, 포트와 백엔드를 확인하세요', 'Name, VIP, port, and backends are required'), false);
      return;
    }
    var r = await fetchPost(EP.RPC(), {jsonrpc:'2.0', method:'nfv.lb.create', params:{
      name: name, vip: vip, port: port, backends: backends
    }, id:'nlb1'});
    if (r.error) { toast(r.error.message || 'Failed', false); return; }
    toast(_L('LB 생성됨', 'LB created'));
    addEvt('LB created: ' + name);
    renderContent();
  } catch (e) { toast(e.message, false); }
}
async function nfvFwAdd() { try { const sw = document.getElementById('fw-sw')?.value; const dir = document.getElementById('fw-dir')?.value; const pri = document.getElementById('fw-pri')?.value; const match = document.getElementById('fw-match')?.value; const act = document.getElementById('fw-act')?.value; if (!sw || !match) { toast('Switch and Match required', false); return; } const r = await fetchPost(EP.OVN_ACL(), { switch: sw, direction: dir, priority: +pri, match: match, action: act }); if (r && r.error) { toast(r.error.message || 'Failed', false); return; } toast('ACL rule added'); addEvt('ACL rule added to ' + sw); } catch (e) { toast(e.message, false); } }

                             
async function renderSecGroups(b) {
  showSkeleton(b);
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  var grid = el('div', { class: 'sg grid-2 mb-16' },
    el('div', { class: 'hc' },
      el('h4', { role: 'heading', 'aria-level': '3' }, '🛡 OVN ACL 보안 그룹'),
      el('p', { class: 'color-muted text-12 mb-8' }, 'OVN ACL 기반 보안 그룹을 관리합니다. 논리 스위치에 인바운드/아웃바운드 규칙을 적용합니다.'),
      el('div', { class: 'mb-8', 'data-role': 'ADMIN' },
        el('div', { class: 'fr' }, el('label', { for: 'sg-switch' }, 'Switch'), el('input', { id: 'sg-switch', placeholder: 'web-tier', class: 'w-150' })),
        el('div', { class: 'fr' }, el('label', { for: 'sg-dir' }, 'Direction'), el('select', { id: 'sg-dir', class: 'input-pcv' }, el('option', { value: 'from-lport' }, 'Inbound (from-lport)'), el('option', { value: 'to-lport' }, 'Outbound (to-lport)'))),
        el('div', { class: 'fr' }, el('label', { for: 'sg-pri' }, 'Priority'), el('input', { id: 'sg-pri', type: 'number', value: '1000', class: 'w-80' })),
        el('div', { class: 'fr' }, el('label', { for: 'sg-match' }, 'Match'), el('input', { id: 'sg-match', placeholder: 'ip4.src==10.0.0.0/24', style: 'width:250px' })),
        el('div', { class: 'fr' }, el('label', { for: 'sg-act' }, 'Action'), el('select', { id: 'sg-act', class: 'input-pcv' }, el('option', null, 'allow'), el('option', null, 'drop'), el('option', null, 'reject'))),
        el('button', { class: 'btn btn-g', onclick: 'sgAddRule()', 'data-role': 'ADMIN' }, '+ ACL 규칙 추가')),
      el('div', { id: 'sg-result', class: 'mt-8' })),
    el('div', { class: 'hc' },
      el('h4', { role: 'heading', 'aria-level': '3' }, '📋 현재 ACL 규칙'),
      el('div', { class: 'fr' }, el('label', { for: 'sg-list-switch' }, 'Switch'), el('input', { id: 'sg-list-switch', placeholder: 'web-tier', class: 'w-150' }), ' ', el('button', { class: 'btn', onclick: 'sgListRules()' }, '조회')),
      el('div', { id: 'sg-rules', class: 'mt-8' })));
  var cliCard = HN.card('📖 CLI 명령어 참조',
    el('div', { style: 'font-size:12px;line-height:1.8;color:var(--fg2)' },
      el('code', { class: 'color-accent' }, 'pcvctl ovn acl list <switch>'), ' — ACL 규칙 목록', el('br'),
      el('code', { class: 'color-accent' }, 'pcvctl ovn acl add <switch> from-lport 1000 "ip4.src==10.0.0.0/24" allow'), ' — 규칙 추가'));
  clearEl(b);
  b.appendChild(frag(HN.pagehead({ title: 'Security Groups' }), grid, cliCard));
  if (typeof applyRoleVisibility === 'function') applyRoleVisibility(window.currentUser && window.currentUser.role);
}

                           
                                                                        
                                                          
                                                           
                                                             
                                                                 
                                                   
  
                       
                                                       
                                                          
                                    
   
window.sgAddRule = async function() {
  const el = document.getElementById('sg-result');
  const sw = document.getElementById('sg-switch')?.value;
  const dir = document.getElementById('sg-dir')?.value;
  const pri = document.getElementById('sg-pri')?.value;
  const match = document.getElementById('sg-match')?.value;
  const act = document.getElementById('sg-act')?.value;
  if (!sw || !match) { if (el) PCV.uxlib.setMsg(el, null, { cls: 'color-red' }, 'Switch와 Match는 필수입니다'); return; }
  if (el) PCV.uxlib.setMsg(el, 'loading', null, '추가 중...');
  try {
    const r = await fetchPost(EP.OVN_ACL(), { 'switch': sw, direction: dir, priority: parseInt(pri), match: match, action: act });
    if (r && r.error) { if (el) PCV.uxlib.setMsg(el, null, { cls: 'color-red' }, '오류: ', r.error.message || 'Failed'); toast(r.error.message || 'Failed', false); return; }
    if (el) PCV.uxlib.setMsg(el, null, { cls: 'color-green' }, 'ACL 규칙 추가 완료');
    toast('ACL 규칙 추가: ' + escapeHtml(sw));
  } catch (e) { if (el) PCV.uxlib.setMsg(el, null, { cls: 'color-red' }, '오류: ', e.message); }
};

window.sgListRules = async function() {
  const el = document.getElementById('sg-rules');
  const sw = document.getElementById('sg-list-switch')?.value;
  if (!sw) { if (el) PCV.uxlib.setMsg(el, null, { cls: 'color-red' }, 'Switch 이름을 입력하세요'); return; }
  if (el) PCV.uxlib.setMsg(el, 'loading', null, '조회 중...');
  try {
    const r = await fetchGet(EP.OVN_ACL() + '?switch=' + encodeURIComponent(sw));
    const list = unwrapList(r);
    if (list.length === 0) { if (el) PCV.uxlib.setMsg(el, null, { tag: 'p', cls: 'color-muted text-12' }, 'ACL 규칙 없음'); return; }
                                                                     
    var rows = list.map(function(a) {
      const entry = typeof a === 'string' ? a : '';
      if (entry) return PCV.uxlib.el('tr', null, PCV.uxlib.el('td', { colspan: '4' }, entry));
      return PCV.uxlib.el('tr', null,
        PCV.uxlib.el('td', null, a.direction || ''),
        PCV.uxlib.el('td', null, String(a.priority || '')),
        PCV.uxlib.el('td', null, a.match || ''),
        PCV.uxlib.el('td', null, a.action || ''));
    });
    var table = PCV.uxlib.el('table', { class: 'text-11' },
      PCV.uxlib.el('thead', null, PCV.uxlib.el('tr', null, PCV.uxlib.el('th', null, 'Direction'), PCV.uxlib.el('th', null, 'Priority'), PCV.uxlib.el('th', null, 'Match'), PCV.uxlib.el('th', null, 'Action'))),
      PCV.uxlib.el('tbody', null, rows));
    if (el) { PCV.uxlib.clearEl(el); el.appendChild(table); }
  } catch (e) { if (el) PCV.uxlib.setMsg(el, null, { cls: 'color-red' }, '오류: ', e.message); }
};

                              
async function renderOverlayNetworks(b) {
  showSkeleton(b);
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  try {
    const r = await fetchGet(EP.OVERLAY_LIST());
    const l = unwrapList(r);
    var body;
    if (!Array.isArray(l) || l.length === 0) {
      body = el('div', { class: 'empty-state' }, el('div', { class: 'empty-state-icon' }, '🌐'), el('div', { class: 'empty-state-text' }, 'No overlay networks'));
    } else {
      body = el('table', { class: 'table-sticky' },
        el('thead', null, el('tr', null, el('th', null, 'Name'), el('th', null, 'VNI'), el('th', null, 'Peers'), el('th', null, 'Status'))),
        el('tbody', null, l.map(function(v) {
          return el('tr', null,
            el('td', null, el('b', null, v.name || '?')),
            el('td', null, v.vni || '-'),
            el('td', null, v.peer_count || 0),
            el('td', null, HN.statusPill(v.state === 'up' ? 'ok' : 'crit', String(v.state || '?').toUpperCase())));
        })));
    }
    clearEl(b);
    b.appendChild(frag(HN.pagehead({ title: 'Overlay Networks (VXLAN)' }), body));
  } catch (e) {
    clearEl(b);
    b.appendChild(frag(HN.pagehead({ title: 'Overlay Networks' }), el('p', { class: 'color-muted' }, 'Failed to load: ', e.message)));
  }
}
window.renderOverlayNetworks = renderOverlayNetworks;

                              
async function renderTopology(b) {
  showSkeleton(b);
  try {
    var results = await Promise.all([
      fetchGet(EP.NET_LIST()).catch(function() { return { data: [] }; }),
      fetchGet(EP.VM_LIST()).catch(function() { return { data: [] }; })
    ]);
    var nets = unwrapList(results[0]);
    var vms = unwrapList(results[1]);

    var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
    clearEl(b);
    b.appendChild(frag(
      HN.pagehead({ title: _L('네트워크 토폴로지', 'Network Topology') }),
      el('canvas', { id: 'topo-canvas', width: '800', height: '500', style: 'width:100%;max-width:800px;border:1px solid var(--border);border-radius:6px;background:var(--bg2)' }),
      el('div', { class: 'flex gap-8 mt-8 text-xs' },
        el('span', null, '🖥 ' + _L('노드', 'Node')),
        el('span', null, '🌐 ' + _L('브릿지', 'Bridge')),
        el('span', null, '💻 VM'))));

                       
    setTimeout(function() {
      var canvas = document.getElementById('topo-canvas');
      if (!canvas) return;
      var ctx = canvas.getContext('2d');
      var W = canvas.width;

                          
      var style = getComputedStyle(document.documentElement);
      var accentColor = style.getPropertyValue('--accent').trim() || '#00f0ff';
      var greenColor = style.getPropertyValue('--green').trim() || '#00ff88';
      var fgColor = style.getPropertyValue('--fg').trim() || '#e0f0ff';
      var dimColor = style.getPropertyValue('--fg2').trim() || '#5a6a8a';

                                                                      
      var nodes = (typeof MON_NODES !== 'undefined' && MON_NODES) ? MON_NODES : [{name:'Node1',ip:'localhost'}];

                      
      var nodePositions = [];
      nodes.forEach(function(nd, i) {
        var x = (W / (nodes.length + 1)) * (i + 1);
        var y = 50;
        nodePositions.push({x:x, y:y, name:nd.name});
        ctx.fillStyle = accentColor;
        ctx.beginPath(); ctx.arc(x, y, 20, 0, Math.PI * 2); ctx.fill();
        ctx.fillStyle = '#000';
        ctx.font = '10px monospace'; ctx.textAlign = 'center';
        ctx.fillText(nd.name, x, y + 4);
        ctx.fillStyle = dimColor;
        ctx.fillText(nd.ip, x, y + 34);
      });

                        
      var bridgePositions = [];
      nets.slice(0, 6).forEach(function(net, i) {
        var x = (W / (Math.min(nets.length, 6) + 1)) * (i + 1);
        var y = 200;
        bridgePositions.push({x:x, y:y, name:net.name});
        ctx.fillStyle = greenColor;
        ctx.fillRect(x - 30, y - 12, 60, 24);
        ctx.fillStyle = '#000';
        ctx.font = '9px monospace'; ctx.textAlign = 'center';
        ctx.fillText(net.name, x, y + 4);
                                     
        var closest = nodePositions[0] || {x:x, y:50};
        ctx.strokeStyle = dimColor; ctx.lineWidth = 1;
        ctx.setLineDash([4, 4]);
        ctx.beginPath(); ctx.moveTo(x, y - 12); ctx.lineTo(closest.x, closest.y + 20); ctx.stroke();
        ctx.setLineDash([]);
      });

                    
      vms.slice(0, 12).forEach(function(vm, i) {
        var cols = Math.min(vms.length, 6);
        var row = Math.floor(i / cols);
        var col = i % cols;
        var x = (W / (cols + 1)) * (col + 1);
        var y = 340 + row * 60;
        var on = vm.state === 'running';
        ctx.fillStyle = on ? greenColor : dimColor;
        ctx.fillRect(x - 25, y - 10, 50, 20);
        ctx.fillStyle = on ? '#000' : fgColor;
        ctx.font = '8px monospace'; ctx.textAlign = 'center';
        var label = vm.name.length > 8 ? vm.name.substring(0, 8) + '..' : vm.name;
        ctx.fillText(label, x, y + 3);
                                 
        if (bridgePositions.length > 0) {
          var br = bridgePositions[i % bridgePositions.length];
          ctx.strokeStyle = on ? accentColor : 'rgba(90,106,138,0.3)';
          ctx.lineWidth = on ? 1 : 0.5;
          ctx.beginPath(); ctx.moveTo(x, y - 10); ctx.lineTo(br.x, br.y + 12); ctx.stroke();
        }
      });
    }, 100);
  } catch (e) { PCV.uxlib.clearEl(b); b.appendChild(PCV.uxlib.frag(HN.pagehead({ title: 'Topology' }), PCV.uxlib.el('p', { class: 'color-red' }, e.message))); }
}
window.renderTopology = renderTopology;

                               
                           
                                                                                   
                                                          
                                                                          
                                                           
                                                                  
                                                            
                                                                  
                                                  
  
                       
                                                        
                                                            
                              
   
async function fwAddRule() {
  var dir = document.getElementById('fw-direction').value;
  var proto = document.getElementById('fw-protocol').value;
  var port = document.getElementById('fw-port').value.trim();
  var source = document.getElementById('fw-source').value.trim() || '0.0.0.0/0';
  var portStart = 0, portEnd = 0;
  if (port.includes('-')) { var ps = port.split('-'); portStart = parseInt(ps[0]); portEnd = parseInt(ps[1]); }
  else if (port) { portStart = parseInt(port); portEnd = portStart; }

  var sgName = 'default';
  var r = await fetchPost(EP.RPC(), {jsonrpc:'2.0', method:'security_group.rule.add', params:{
    name: sgName, direction: dir, protocol: proto, port_start: portStart, port_end: portEnd, source: source
  }, id:'fw1'});
  if (r.error) { toast(r.error.message || 'Failed', false); return; }
  toast(_L('규칙 추가됨', 'Rule added'));
  fwLoadRules();
}

async function fwLoadRules() {
  var r = await fetchPost(EP.RPC(), {jsonrpc:'2.0', method:'security_group.list', params:{}, id:'fwl1'});
  var groups = unwrapList(r);
  var el = document.getElementById('fw-rules-list');
  if (!el) return;
                                                                   
  PCV.uxlib.clearEl(el);
  if (groups.length === 0) { el.appendChild(PCV.uxlib.el('div', { class: 'stat-label' }, 'No security groups')); return; }
  var blocks = groups.map(function(sg) {
    var kids = [
      PCV.uxlib.el('strong', null, sg.name),
      ' ',
      PCV.uxlib.el('span', { class: 'stat-label' }, '(' + (sg.rule_count || 0) + ' rules)')
    ];
    if (sg.rules && sg.rules.length) {
      var rows = sg.rules.map(function(rule) {
        var portStr = rule.port_end > rule.port_start ? rule.port_start + '-' + rule.port_end : (rule.port_start || '*');
        return PCV.uxlib.el('tr', null,
          PCV.uxlib.el('td', null, rule.direction),
          PCV.uxlib.el('td', null, rule.protocol),
          PCV.uxlib.el('td', null, portStr),
          PCV.uxlib.el('td', null, rule.source),
          PCV.uxlib.el('td', null, PCV.uxlib.el('button', { class: 'btn btn-sm btn-r', onclick: "fwDelRule('" + sg.name + "'," + (rule.db_id || 0) + ")", 'data-role': 'ADMIN' }, _L('삭제', 'Del'))));
      });
      kids.push(PCV.uxlib.el('table', { class: 'tbl mt-4' },
        PCV.uxlib.el('tbody', null,
          PCV.uxlib.el('tr', null, PCV.uxlib.el('th', null, 'Dir'), PCV.uxlib.el('th', null, 'Proto'), PCV.uxlib.el('th', null, 'Port'), PCV.uxlib.el('th', null, 'Source'), PCV.uxlib.el('th', null)),
          rows)));
    }
    return PCV.uxlib.el('div', { class: 'mb-8' }, kids);
  });
  el.appendChild(PCV.uxlib.frag(blocks));
  if (typeof applyRoleVisibility === 'function') applyRoleVisibility(window.currentUser && window.currentUser.role);
}

async function fwDelRule(sg, ruleId) {
  var r = await fetchPost(EP.RPC(), {jsonrpc:'2.0', method:'security_group.rule.remove', params:{name:sg, rule_id:ruleId}, id:'fwd1'});
  if (r.error) { toast(r.error.message, false); return; }
  toast(_L('규칙 삭제됨', 'Rule removed'));
  fwLoadRules();
}

window.fwAddRule = fwAddRule;
window.fwLoadRules = fwLoadRules;
window.fwDelRule = fwDelRule;

                                    
window.renderNetworks = renderNetworks;
window.showNetCreate = showNetCreate;
window.netModeChanged = netModeChanged;
window.netUplinkChanged = netUplinkChanged;
window.loadPhysNics = loadPhysNics;
window.updateNetCreateState = updateNetCreateState;
window.doNetCreate = doNetCreate;
window.netDel = netDel;
window.doNetDel = doNetDel;
window.showNetEditor = showNetEditor;
window.showNetEdit = showNetEdit;
window.netEditModeChanged = netEditModeChanged;
window.doNetEdit = doNetEdit;
window.renderOvn = renderOvn;
window.loadLBList = loadLBList;
window.nfvLbCreate = nfvLbCreate;
window.nfvFwAdd = nfvFwAdd;
window.renderSecGroups = renderSecGroups;
                                                                

                                          
PCV.network = {
  renderNetworks: renderNetworks,
  toggleFwPanel: toggleFwPanel,
  toggleAclPanel: toggleAclPanel,
  showNetCreate: showNetCreate,
  netModeChanged: netModeChanged,
  netUplinkChanged: netUplinkChanged,
  loadPhysNics: loadPhysNics,
  updateNetCreateState: updateNetCreateState,
  doNetCreate: doNetCreate,
  netDel: netDel,
  doNetDel: doNetDel,
  showNetEditor: showNetEditor,
  showNetEdit: showNetEdit,
  netEditModeChanged: netEditModeChanged,
  doNetEdit: doNetEdit,
  renderOvn: renderOvn,
  loadLBList: loadLBList,
  nfvLbCreate: nfvLbCreate,
  nfvFwAdd: nfvFwAdd,
  renderSecGroups: renderSecGroups,
  renderOverlayNetworks: renderOverlayNetworks,
  renderTopology: renderTopology,
  fwAddRule: fwAddRule,
  fwLoadRules: fwLoadRules,
  fwDelRule: fwDelRule
};

})(window.PCV);
