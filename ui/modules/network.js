/* ═══════════════════════════════════════════════════════════════
   PureCVisor — modules/network.js
   Network, OVN, NFV, Security Groups
   ADR-0013: IIFE module scope — PCV.network namespace
   ═══════════════════════════════════════════════════════════════ */
window.PCV = window.PCV || {};
(function(PCV) {

async function renderNetworks(b) {
  showSkeleton(b);
  try {
    const n = await fetchGet(EP.NET_LIST());
    const l = unwrapList(n);
    const compactMode = !!(window.matchMedia && window.matchMedia('(max-width: 768px)').matches);
    var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
    var heading = el('div', { class: 'ops-section-heading' },
      el('div', null,
        el('h3', null, _L('네트워크 인벤토리', 'Network inventory')),
        el('p', null, _L('브리지, DHCP, 외부 연결 상태를 먼저 확인한 뒤 정책 편집으로 이어갑니다.', 'Review bridges, DHCP, and external connectivity first, then move into policy editing.'))),
      el('button', { class: 'btn btn-primary', onclick: 'showNetCreate()' }, '+ ' + t('net.new')));
    if (l.length === 0) {
      clearEl(b);
      b.appendChild(frag(heading,
        el('div', { class: 'empty-state', style: 'text-align:center;padding:40px 20px' },
          el('div', { style: 'font-size:48px;margin-bottom:12px;opacity:.5' }, '🌐'),
          el('div', { style: 'font-size:14px;color:var(--fg2);margin-bottom:16px' }, _L('구성된 네트워크가 없습니다', 'No configured networks')),
          el('button', { class: 'btn btn-primary', onclick: 'showNetCreate()' }, '+ ' + _L('네트워크 생성', 'Create network')))));
      return;
    }
    var body;
    if (compactMode) {
      var cards = l.map(function(v) {
        var ext = v.mode === 'bridge' ? (v.phys || '-') : v.mode === 'nat' ? 'NAT' : '-';
        return HN.card(el('b', null, v.name), [
          HN.row(_L('모드', 'Mode'), HN.badge(escapeHtml(v.mode) || '?', v.mode === 'nat' ? 'y' : v.mode === 'bridge' ? 'g' : 'r')),
          HN.row(_L('상태', 'State'), HN.badge(escapeHtml(v.state) || '?', v.state === 'up' ? 'g' : 'r')),
          HN.row(_L('외부 연결', 'External'), ext),
          HN.row(_L('호스트 IP', 'Host IP'), v.ip_cidr || '-'),
          HN.row('DHCP', v.dhcp ? 'ON' : '-'),
          HN.row('VM', String(v.vm_count || 0)),
          HN.row(_L('서브넷', 'Subnet'), v.subnet || '-'),
          el('div', { class: 'flex gap-4 ops-action-row', style: 'margin-top:10px' },
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
      var rows = l.map(function(v) {
        var ext = v.mode === 'bridge' ? (v.phys || '-') : v.mode === 'nat' ? 'NAT' : '-';
        return el('tr', null,
          el('td', null, el('b', null, v.name)),
          el('td', null, HN.badge(escapeHtml(v.mode) || '?', v.mode === 'nat' ? 'y' : v.mode === 'bridge' ? 'g' : 'r')),
          el('td', null, HN.badge(escapeHtml(v.state) || '?', v.state === 'up' ? 'g' : 'r')),
          el('td', null, ext),
          el('td', null, v.ip_cidr || '-'),
          el('td', null, v.dhcp ? 'ON' : '-'),
          el('td', null, v.vm_count || 0),
          el('td', null, v.subnet || '-'),
          el('td', { class: 'nowrap' },
            el('button', { class: 'btn btn-soft', style: 'font-size:10px;padding:3px 8px', onclick: "showNetEdit('" + escapeAttr(v.name) + "','" + (escapeAttr(v.mode) || '') + "','" + (escapeAttr(v.ip_cidr) || '') + "'," + (v.dhcp || false) + ",'" + (escapeAttr(v.phys) || '') + "')" }, t('btn.edit')),
            ' ',
            el('button', { class: 'btn btn-r', style: 'font-size:10px;padding:3px 8px', onclick: "netDel('" + escapeAttr(v.name) + "')" }, t('btn.delete'))));
      });
      body = el('table', { class: 'table-sticky' }, thead, el('tbody', null, rows));
    }
    var fwPanel = el('div', { class: 'sg grid-2', style: 'margin-top:16px' },
      el('div', { class: 'hc' },
        el('h4', null, _L('방화벽 정책 편집', 'Firewall policy editor')),
        el('p', { class: 'color-muted text-11 mb-8' }, _L('브리지나 세그먼트를 확인한 뒤 인바운드/아웃바운드 규칙을 추가합니다.', 'Add ingress or egress rules after checking the bridge or segment you are editing.')),
        el('div', { class: 'flex gap-8 mb-8 ops-form-strip', style: 'flex-wrap:wrap' },
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
        el('h4', null, _L('OVN ACL 운영 메모', 'OVN ACL operations note')),
        el('p', { class: 'color-muted text-11 mb-8' }, _L('싱글 엣지에서는 상태를 먼저 확인하고, 필요한 경우 수동 ACL 명령으로 보강합니다.', 'In Single Edge, check state first and use manual ACL commands only when you need to refine policy.')),
        el('pre', { style: 'background:var(--bg3);padding:8px;border-radius:6px;font-size:11px;overflow-x:auto' },
          'pcvctl ovn acl list <switch>\npcvctl ovn acl add <switch> --direction to-lport --match "ip4.src==10.0.0.0/24" --action allow\npcvctl ovn acl del <switch> --uuid <uuid>')));
    clearEl(b);
    b.appendChild(frag(heading, body, fwPanel));
  } catch (e) { if(_DEBUG) console.warn('n:', e.message); }
}
function toggleFwPanel() { var p = document.getElementById('fw-panel'); if (p) p.classList.toggle('hidden'); }
function toggleAclPanel() { var p = document.getElementById('acl-panel'); if (p) p.classList.toggle('hidden'); }
window.toggleFwPanel = toggleFwPanel;
window.toggleAclPanel = toggleAclPanel;

function showNetCreate() {
  var el = PCV.uxlib.el;
  showModal([
    el('h2', null, t('net.new')),
    el('div', { class: 'fr' }, el('label', { for: 'nn' }, 'Bridge'), el('input', { id: 'nn', placeholder: 'pcvbr0' })),
    el('div', { class: 'fr' }, el('label', { for: 'nm' }, 'Mode'),
      el('select', { id: 'nm', onchange: 'netModeChanged()' },
        el('option', { value: 'nat' }, t('net.mode.nat')),
        el('option', { value: 'isolated' }, t('net.mode.isolated')),
        el('option', { value: 'routed' }, t('net.mode.routed')),
        el('option', { value: 'bridge' }, t('net.mode.bridge')))),
    el('div', { class: 'fr' }, el('label', { for: 'nc' }, 'CIDR'), el('input', { id: 'nc', placeholder: '10.0.0.1/24' })),
    el('div', { id: 'net-phys-row', class: 'fr hidden' },
      el('label', { for: 'np' }, 'Physical NIC'),
      el('div', { class: 'flex gap-6 flex-1' },
        el('select', { id: 'np', style: 'flex:1;padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:6px;font-size:12px' },
          el('option', { value: '' }, t('loading'))))),
    el('div', { class: 'stat-label', style: 'margin:4px 0 8px 98px', id: 'net-mode-hint' }, 'NAT: MASQUERADE + DHCP'),
    el('div', { class: 'text-right mt-12' },
      el('button', { class: 'btn btn-g', onclick: 'doNetCreate()' }, t('btn.create')),
      ' ',
      el('button', { class: 'btn btn-r', onclick: 'closeModal()' }, t('btn.cancel')))
  ]);
}

function netModeChanged() { const m = document.getElementById('nm').value; const pr = document.getElementById('net-phys-row'); const hint = document.getElementById('net-mode-hint'); const mr = document.getElementById('np-manual-row');
  if (m === 'bridge') { pr.classList.remove('hidden'); loadPhysNics(); hint.textContent = 'Bridge: Physical NIC binding'; } else { pr.classList.add('hidden'); if (mr) mr.remove(); const hints = { nat: 'NAT: MASQUERADE + DHCP', isolated: 'Isolated: VM-to-VM only', routed: 'Routed: ip_forward only' }; hint.textContent = hints[m] || ''; } }

async function loadPhysNics() { const sel = document.getElementById('np'); if (!sel) return;
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  try { const r = await fetchGet(EP.NET_LIST()); const nl = unwrapList(r); const seen = new Set();
    var opts = [el('option', { value: '' }, '-- Select NIC --')];
    nl.forEach(n => { (n.slaves || []).forEach(s => { if (!seen.has(s) && !s.startsWith('vnet')) { seen.add(s); opts.push(el('option', { value: s }, s + ' (slave of ' + n.name + ')')); } }); });
    opts.push(el('option', { value: '', disabled: '' }, '-- Manual --'));
    clearEl(sel); sel.appendChild(frag(opts));
    const row = document.getElementById('net-phys-row');
    if (row && !document.getElementById('np-manual')) { const mi = document.createElement('div'); mi.className = 'fr'; mi.id = 'np-manual-row'; mi.style.marginTop = '4px';
      mi.appendChild(frag(el('label', { for: 'np-manual' }, 'NIC Name'), el('input', { id: 'np-manual', placeholder: 'e.g. wlo1, enp42s0, eno1...', style: 'flex:1;padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:6px;font-size:12px' })));
      row.parentNode.insertBefore(mi, row.nextSibling); }
  } catch (e) { clearEl(sel); sel.appendChild(el('option', { value: '' }, '-- Enter NIC name below --')); } }

async function doNetCreate() { var _btn = document.activeElement; if (_btn && _btn.tagName === 'BUTTON') { if (_btn.disabled) return; _btn.disabled = true; _btn.setAttribute('aria-busy', 'true'); } try { const body = { bridge_name: document.getElementById('nn').value, mode: document.getElementById('nm').value, cidr: document.getElementById('nc').value }; const m = body.mode; if (m === 'bridge') { const p = document.getElementById('np')?.value || document.getElementById('np-manual')?.value; if (p) body.physical_if = p; else { toast(t('net.phys_required'), false); return; } } const r = await fetchPost(EP.NET_LIST(), body); if (r.error) { toast(r.error.message || 'Failed', false); } else { toast(t('net.created')); addEvt(t('net.created')); } closeModal(); renderNetworks(document.getElementById('cb')); } catch (e) { toast(e.message, false); } finally { if (_btn && _btn.tagName === 'BUTTON') { _btn.disabled = false; _btn.removeAttribute('aria-busy'); } } }

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
  /* ADR-013 DOM-safe: 진행 모달을 el/frag 로 조립. 아이콘 HTML 엔티티(&#9888; 등)는
   * 동일 코드포인트 리터럴 글리프로, escapeHtml(name) 은 TextNode 로 대체(이스케이프 불요).
   * (원본 prog-fill div 는 class 속성이 중복(class="prog-fill"..class="w-pct-20")이라
   *  HTML 파서상 첫 class="prog-fill" 만 적용되고 w-pct-20 은 무시됨 — 렌더 동등 보존.) */
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  const mc = document.getElementById('mc');
  clearEl(mc);
  mc.appendChild(frag(
    el('h2', { class: 'color-red' }, '⚠ Deleting Network'),
    el('p', null, el('b', { class: 'color-accent' }, name)),
    el('div', { class: 'prog-bar' }, el('div', { class: 'prog-fill', id: 'dn-p' })),
    el('div', { class: 'prog-status', id: 'dn-s' }, el('span', { class: 'spinner' }), 'Removing firewall rules & DHCP...')
  ));
  const pf = document.getElementById('dn-p'), ps = document.getElementById('dn-s');
  try { pf.style.width = '50%'; PCV.uxlib.setMsg(ps, 'loading', null, 'Sending DELETE...');
    const d = await fetchDelete(EP.NET_DETAIL(name)).catch(() => ({}));
    if (d.error) { pf.style.background = 'var(--red)'; pf.style.width = '100%'; ps.textContent = '❌ ' + d.error.message; toast(t('btn.delete') + ' failed', false); return; }
    pf.style.width = '100%'; ps.textContent = '✅ ' + t('net.deleted'); toast(t('net.deleted')); addEvt(t('net.deleted') + ': ' + name); setTimeout(() => { closeModal(); renderNetworks(document.getElementById('cb')); }, 1500);
  } catch (e) { pf.style.width = '100%'; ps.textContent = '❌ ' + e.message; toast(e.message, false); } }

function showNetEditor() { navigateTo('networks'); }

function ovnDemoEntryName(entry) {
  if (typeof entry === 'string') return entry;
  if (!entry) return '';
  return entry.name || entry.entry || JSON.stringify(entry);
}

function ovnDemoHasEntry(list, name) {
  return Array.isArray(list) && list.some(function(entry) {
    return ovnDemoEntryName(entry) === name;
  });
}

function renderOvnDemoServiceComposition(dd, sd, sl, rl, hd) {
  var el = PCV.uxlib.el;
  const publicDomain = 'demo.purecvisor.example.com';
  const nodeName = hd.node_name || hd.node || 'PureCVisor-Prod-2';
  const switchName = dd.switch || 'pcv-demo-ls';
  const routerName = dd.router || 'pcv-demo-lr';
  const vmA = dd.vm_a || { name: 'ovn-demo-a', ip: '10.77.0.11' };
  const vmB = dd.vm_b || { name: 'ovn-demo-b', ip: '10.77.0.12' };
  const switchPresent = (dd.ovn && dd.ovn.switch_present === true) || ovnDemoHasEntry(sl, switchName);
  const routerPresent = (dd.ovn && dd.ovn.router_present === true) || ovnDemoHasEntry(rl, routerName);
  const pingOk = !!(dd.ping && dd.ping.ok === true);
  const httpOk = !!(dd.http && dd.http.ok === true);
  const visual = dd.visual_service || {};
  const visualOk = visual.ok === true;
  const visualUrl = visual.url || 'http://10.77.0.12:8080';
  const visualPublicUrl = 'https://' + publicDomain + '/ovn-visual/';
  const nodeDescriptions = Object.assign({
    'ovn-demo-a': _L('요청을 시작하는 클라이언트 VM입니다. OVN 내부망에서 ovn-demo-b 서비스까지 실제 통신을 발생시킵니다.', 'Client VM that initiates the demo request through the OVN internal network.'),
    'pcv-demo-ls': _L('OVN Logical Switch입니다. 데모 VM들이 같은 10.77.0.0/24 논리 L2 네트워크에 붙는 지점입니다.', 'OVN logical switch that places the demo VMs on the same 10.77.0.0/24 logical L2 network.'),
    'pcv-demo-lr': _L('OVN Logical Router입니다. 10.77.0.1 게이트웨이 역할을 하며 논리 네트워크의 라우팅 경계를 보여 줍니다.', 'OVN logical router that represents the 10.77.0.1 gateway and routing boundary.'),
    'ovn-demo-b': _L('시각 서비스가 실행되는 서버 VM입니다. 10.77.0.12:8080에서 HTML, health, visual-state를 제공합니다.', 'Server VM running the visual service on 10.77.0.12:8080.')
  }, visual.node_descriptions || {});
  const nodeNote = function(name) {
    const note = nodeDescriptions[name] || '';
    return note ? [el('br'), el('span', { class: 'color-muted text-11' }, note)] : null;
  };
  const fallbackServiceFlows = [
    {
      title: _L('외부 공개 시각 서비스 흐름', 'External visual service flow'),
      summary: _L('브라우저 요청이 nginx reverse proxy와 pcv-demo-host OVN access port를 거쳐 ovn-demo-b:8080으로 전달됩니다.', 'Browser requests pass through nginx reverse proxy and the pcv-demo-host OVN access port to ovn-demo-b:8080.'),
      steps: ['browser', 'nginx /ovn-visual/', 'pcv-demo-host 10.77.0.254', 'pcv-demo-ls', 'ovn-demo-b:8080', 'HTML / JSON']
    },
    {
      title: _L('내부 VM 점검 흐름', 'Internal VM check flow'),
      summary: _L('PureCVisor host collector가 qemu-guest-agent로 ovn-demo-a 내부 스크립트를 실행하고, ovn-demo-a가 OVN 내부망으로 ovn-demo-b의 health와 visual-state를 조회합니다.', 'The host collector runs the ovn-demo-a script through qemu-guest-agent, then ovn-demo-a reads ovn-demo-b health and visual-state through the OVN internal network.'),
      steps: ['host collector', 'qemu-guest-agent', 'ovn-demo-a', 'pcv-demo-ls', 'ovn-demo-b:8080', 'health JSON', 'PureCVisor UI']
    },
    {
      title: _L('pcv-demo-lr 라우팅 경계', 'pcv-demo-lr routing boundary'),
      summary: _L('pcv-demo-lr은 10.77.0.1 게이트웨이와 logical routing boundary를 보여 줍니다. 현재 health/visual service 왕복은 같은 10.77.0.0/24 내부 서비스 흐름이라 pcv-demo-ls를 중심으로 동작합니다.', 'pcv-demo-lr represents the 10.77.0.1 gateway and logical routing boundary. The current health and visual-service flows stay within 10.77.0.0/24 and center on pcv-demo-ls.'),
      steps: ['pcv-demo-ls', 'pcv-demo-lr 10.77.0.1', 'routing boundary']
    }
  ];
  const serviceFlows = Array.isArray(visual.service_flows) && visual.service_flows.length ? visual.service_flows : fallbackServiceFlows;
  const renderServiceFlow = function(flow) {
    const steps = Array.isArray(flow.steps) ? flow.steps : [];
    const stepNodes = [];
    steps.forEach(function(step, i) {
      if (i > 0) stepNodes.push(el('span', { class: 'color-muted text-11' }, '→'));
      stepNodes.push(HN.badge(String(step), 'g'));
    });
    const body = [
      el('p', { class: 'color-muted text-11', style: 'line-height:1.5;margin:0 0 10px' }, flow.summary || ''),
      el('div', { class: 'flex gap-4', style: 'flex-wrap:wrap;align-items:center' }, stepNodes)
    ];
    return HN.card(flow.title || _L('서비스 흐름', 'Service flow'), body);
  };
  const visualPath = Array.isArray(visual.path) && visual.path.length
    ? visual.path.join(' → ')
    : 'ovn-demo-a → pcv-demo-ls → pcv-demo-lr → ovn-demo-b';
  const live = dd.status === 'ok' && dd.stale !== true;
  const liveBadge = HN.badge(live ? _L('라이브', 'Live') : _L('점검 필요', 'Check'), live ? 'g' : 'r');
  const switchBadge = HN.badge(switchPresent ? _L('존재', 'Present') : _L('누락', 'Missing'), switchPresent ? 'g' : 'r');
  const routerBadge = HN.badge(routerPresent ? _L('존재', 'Present') : _L('누락', 'Missing'), routerPresent ? 'g' : 'r');
  const vmPath = (vmA.name || 'ovn-demo-a') + ' (' + (vmA.ip || '10.77.0.11') + ') → ' + (vmB.name || 'ovn-demo-b') + ' (' + (vmB.ip || '10.77.0.12') + ')';
  const heading1 = el('div', { class: 'ops-section-heading' },
    el('div', null,
      el('h3', null, _L('OVN 데모 서비스 구성', 'OVN demo service composition')),
      el('p', null, _L('공개 데모 도메인부터 VM A → VM B 검증까지 현재 구성값을 API 응답으로 조합해 표시합니다.', 'The current API response composes the public demo domain through the VM A to VM B validation path.'))));
  const grid1 = el('div', { class: 'sg grid-3' },
    HN.card(_L('공개 진입점', 'Public entrypoint'), [
      HN.row(_L('도메인', 'Domain'), publicDomain),
      HN.row('HTTPS', HN.badge('443', 'g')),
      HN.row(_L('노드', 'Node'), nodeName)]),
    HN.card('PureCVisor API', [
      HN.row(_L('서비스', 'Service'), hd.service || 'purecvisorsd'),
      HN.row(_L('상태', 'State'), HN.badge(hd.status || 'ok', hd.status === 'critical' ? 'r' : 'g')),
      HN.row('OVN', HN.badge(sd.available ? _L('활성', 'Enabled') : _L('비활성', 'Disabled'), sd.available ? 'g' : 'r'))]),
    HN.card(_L('라이브 증거', 'Live evidence'), [
      liveBadge,
      HN.row(_L('마지막 점검', 'Last check'), dd.checked_at || '-'),
      HN.row(_L('stale 기준', 'Stale after'), String(dd.stale_after_sec || 300) + 's')]),
    HN.card(_L('OVN 내부 시각 서비스', 'OVN internal visual service'), [
      HN.badge(visualOk ? _L('정상', 'OK') : _L('확인 필요', 'Check'), visualOk ? 'g' : 'r'),
      HN.row(_L('서비스', 'Service'), visual.service || 'ovn-demo-visual'),
      HN.row(_L('공개 URL', 'Public URL'), visualPublicUrl),
      HN.row(_L('내부 URL', 'Internal URL'), visualUrl),
      HN.row(_L('외부 inbound', 'External inbound'), visual.external_inbound === false ? _L('없음', 'None') : _L('확인 필요', 'Check'))]));
  const heading2 = el('div', { class: 'ops-section-heading' },
    el('div', null,
      el('h3', null, _L('서비스 흐름', 'Service flow')),
      el('p', null, _L('각 VM과 OVN 구성요소가 요청, 점검, 응답 흐름에서 맡는 역할을 단계별로 표시합니다.', 'Each VM and OVN component is shown as a step in the request, validation, and response flow.'))));
  const flowGrid = el('div', { class: 'sg grid-3' }, serviceFlows.map(renderServiceFlow));
  const table1 = el('div', { class: 'hc' },
    el('h4', null, _L('동적 토폴로지', 'Dynamic topology')),
    el('table', { class: 'table-sticky' },
      el('thead', null, el('tr', null, el('th', null, _L('계층', 'Layer')), el('th', null, _L('현재 값', 'Current value')), el('th', null, _L('상태', 'State')))),
      el('tbody', null,
        el('tr', null, el('td', null, _L('브라우저', 'Browser')), el('td', null, 'https://' + publicDomain + '/ui/'), el('td', null, HN.badge(_L('공개', 'Public'), 'g'))),
        el('tr', null, el('td', null, 'PureCVisor API'), el('td', null, '/api/v1/demo/ovn-ovs/health'), el('td', null, HN.badge(_L('읽기', 'Read'), 'g'))),
        el('tr', null, el('td', null, 'OVN Logical Switch'), el('td', null, switchName, nodeNote(switchName)), el('td', null, switchBadge)),
        el('tr', null, el('td', null, 'OVN Logical Router'), el('td', null, routerName, ' / 10.77.0.1', nodeNote(routerName)), el('td', null, routerBadge)),
        el('tr', null, el('td', null, 'VM A → VM B'), el('td', null, vmPath, nodeNote(vmA.name || 'ovn-demo-a'), nodeNote(vmB.name || 'ovn-demo-b')), el('td', null, HN.badge(pingOk && httpOk ? _L('성공', 'OK') : _L('확인 필요', 'Check'), pingOk && httpOk ? 'g' : 'r'))),
        el('tr', null,
          el('td', null, _L('시각 서비스', 'Visual service')),
          el('td', null, visualPath, el('br'), el('span', { class: 'color-muted text-11' }, visualPublicUrl + ' · ' + visualUrl + ' · ' + _L('외부 inbound 없음', 'No external inbound')), nodeNote('ovn-demo-b')),
          el('td', null, HN.badge(visualOk ? _L('성공', 'OK') : _L('확인 필요', 'Check'), visualOk ? 'g' : 'r'))))));
  const table2 = el('div', { class: 'hc' },
    el('h4', null, _L('viewer 읽기 전용 경계', 'Viewer read-only boundary')),
    el('table', { class: 'table-sticky' },
      el('thead', null, el('tr', null, el('th', null, _L('요청', 'Request')), el('th', null, _L('기대 결과', 'Expected')))),
      el('tbody', null,
        el('tr', null, el('td', null, 'GET /api/v1/ovn/status'), el('td', null, HN.badge('200', 'g'))),
        el('tr', null, el('td', null, 'GET /api/v1/ovn/switches'), el('td', null, HN.badge('200', 'g'))),
        el('tr', null, el('td', null, 'GET /api/v1/demo/ovn-ovs/health'), el('td', null, HN.badge('200', 'g'))),
        el('tr', null, el('td', null, 'GET /api/v1/vnc/' + (vmA.name || 'ovn-demo-a')), el('td', null, HN.badge('403', 'r'))),
        el('tr', null, el('td', null, 'POST /api/v1/vms/' + (vmA.name || 'ovn-demo-a') + '/stop'), el('td', null, HN.badge('403', 'r'))))));
  const grid2 = el('div', { class: 'sg grid-2' }, table1, table2);
  return [heading1, grid1, heading2, flowGrid, grid2];
}

function showNetEdit(name, mode, cidr, dhcp, phys) {
  var el = PCV.uxlib.el;
  showModal([
    el('h2', null, '\u{2699} ', t('btn.edit'), ': ', name),
    el('div', { class: 'fr' }, el('label', { for: 'net-bridge' }, 'Bridge'), el('input', { id: 'net-bridge', value: escapeHtml(name), disabled: '', style: 'opacity:.6' })),
    el('div', { class: 'fr' }, el('label', { for: 'ne-mode' }, 'Mode'),
      el('select', { id: 'ne-mode', onchange: 'netEditModeChanged()' },
        el('option', { value: 'nat', selected: mode === 'nat' ? '' : null }, 'nat'),
        el('option', { value: 'isolated', selected: mode === 'isolated' ? '' : null }, 'isolated'),
        el('option', { value: 'routed', selected: mode === 'routed' ? '' : null }, 'routed'),
        el('option', { value: 'bridge', selected: mode === 'bridge' ? '' : null }, 'bridge'))),
    el('div', { class: 'fr' }, el('label', { for: 'ne-dhcp' }, 'DHCP'),
      el('select', { id: 'ne-dhcp' },
        el('option', { value: 'on', selected: dhcp ? '' : null }, 'ON'),
        el('option', { value: 'off', selected: !dhcp ? '' : null }, 'OFF'))),
    /* ADR-013 DOM-safe: 원본 div 는 class 속성이 중복(class="fr" .. 조건부 class="hidden")
     * 이라 HTML 파서상 첫 class="fr" 만 적용되고 조건부 hidden 은 무시됨 — 렌더 동등
     * 보존(초기 표시는 mode 무관 항상 노출; netEditModeChanged 가 이후 토글). */
    el('div', { class: 'fr', id: 'ne-phys-row' },
      el('label', { for: 'ne-phys' }, 'Physical NIC'),
      el('input', { id: 'ne-phys', value: escapeHtml(phys), placeholder: 'e.g. wlo1, enp42s0' })),
    el('div', { class: 'stat-label', style: 'margin:4px 0 8px 98px', id: 'ne-hint' }, 'Mode: ', mode),
    el('div', { class: 'text-right mt-14' },
      el('button', { class: 'btn btn-g', onclick: "doNetEdit('" + escapeAttr(name) + "')" }, t('btn.apply')),
      ' ',
      el('button', { class: 'btn btn-r', onclick: 'closeModal()' }, t('btn.cancel')))
  ]);
}

function netEditModeChanged() { const m = document.getElementById('ne-mode').value; const pr = document.getElementById('ne-phys-row'); const hint = document.getElementById('ne-hint');
  if (m === 'bridge') { pr.classList.remove('hidden'); hint.textContent = 'Bridge: Physical NIC required'; } else { pr.classList.add('hidden'); hint.textContent = 'Mode: ' + m; } }

async function doNetEdit(name) { const mode = document.getElementById('ne-mode').value; try { const mr = await fetchPost(EP.NET_MODE(name), { mode: mode }); if (mr.error) { toast('Failed: ' + (mr.error.message || ''), false); return; } toast(name + ' updated'); addEvt('Network edit: ' + name); closeModal(); renderNetworks(document.getElementById('cb')); } catch (e) { toast('Edit failed: ' + e.message, false); } }

/* ═══ OVN ═══ */
async function renderOvn(b) {
  showSkeleton(b);
  try {
    const st = await fetchGet(EP.OVN_STATUS()); const sd = unwrapData(st);
    const sw = await fetchGet(EP.OVN_SWITCHES()); const sl = unwrapList(sw);
    const rt = await fetchGet(EP.OVN_ROUTERS()); const rl = unwrapList(rt);
    const health = await fetchGet(EP.HEALTH()).catch(function() {
      return { status: 'unknown', node_name: 'PureCVisor-Prod-2', service: 'purecvisorsd' };
    });
    const hd = unwrapData(health);
    const demo = await fetchGet(EP.DEMO_OVN_HEALTH()).catch(function(e) {
      return { data: { status: 'stale', stale: true, reason: e.message } };
    });
    const dd = unwrapData(demo);
    const demoLive = dd.status === 'ok' && dd.stale !== true;
    const demoBadge = demoLive ? 'g' : (dd.status === 'degraded' ? 'y' : 'r');
    const pingOk = !!(dd.ping && dd.ping.ok === true);
    const httpOk = !!(dd.http && dd.http.ok === true);
    const visualOk = !!(dd.visual_service && dd.visual_service.ok === true);
    var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
    var heading1 = el('div', { class: 'ops-section-heading' },
      el('div', null, el('h3', null, 'OVN SDN'), el('p', null, _L('싱글 엣지에서 허용된 로컬 OVN 상태와 수동 구성을 확인합니다.', 'Review the local OVN state and the manually managed configuration allowed in Single Edge.'))));
    var grid1 = el('div', { class: 'sg grid-3' },
      HN.card(_L('OVN 가용성', 'OVN availability'), sd.available ? [HN.badge(_L('준비됨', 'Ready'), 'g'), HN.row('Encap', 'Geneve')] : el('p', { class: 'color-muted text-xs' }, _L('OVN이 설치되지 않았습니다', 'OVN is not installed'))),
      HN.card(_L('논리 스위치', 'Logical switches'), [el('div', { class: 'stat-lg color-accent' }, sl.length), HN.row(_L('상태', 'State'), HN.badge(sd.available ? _L('조회 가능', 'Available') : _L('미설치', 'Unavailable'), sd.available ? 'g' : 'r'))]),
      HN.card(_L('논리 라우터', 'Logical routers'), [el('div', { class: 'stat-lg color-green' }, rl.length), HN.row(_L('운영 방식', 'Mode'), _L('수동 구성', 'Manual configuration'))]));
    var heading2 = el('div', { class: 'ops-section-heading' },
      el('div', null, el('h3', null, _L('공개 OVN 데모 헬스', 'Public OVN demo health')), el('p', null, _L('viewer 권한으로 데모 VM 간 실제 통신 결과를 읽기 전용으로 확인합니다.', 'Viewer users can inspect the live VM-to-VM demo result without control permissions.'))));
    var grid2 = el('div', { class: 'sg grid-3' },
      HN.card(_L('VM 간 통신', 'VM reachability'), [HN.badge(demoLive ? _L('정상', 'OK') : _L('확인 필요', 'Check'), demoBadge), HN.row('Ping', pingOk ? 'OK' : 'N/A'), HN.row('HTTP', httpOk ? 'OK' : 'N/A'), HN.row(_L('시각 서비스', 'Visual service'), visualOk ? 'OK' : 'N/A')]),
      HN.card(_L('데모 네트워크', 'Demo network'), [HN.row(_L('스위치', 'Switch'), dd.switch || 'pcv-demo-ls'), HN.row(_L('라우터', 'Router'), dd.router || 'pcv-demo-lr')]),
      HN.card(_L('최근 점검', 'Last check'), [HN.row(_L('시각', 'Time'), dd.checked_at || '-'), HN.row(_L('상태', 'State'), dd.stale ? 'stale' : 'live')]));
    var demoComposition = renderOvnDemoServiceComposition(dd, sd, sl, rl, hd);
    var heading3 = el('div', { class: 'ops-section-heading' },
      el('div', null, el('h3', null, _L('논리 토폴로지', 'Logical topology')), el('p', null, _L('스위치와 라우터 목록을 먼저 확인한 뒤 수동 정책과 부가 구성을 적용합니다.', 'Review switches and routers first, then apply manual policy and optional configuration.'))));
    var switchPanel = el('div', { class: 'hc' },
      el('h4', null, _L('논리 스위치', 'Logical switches') + ' (' + sl.length + ')'),
      sl.length === 0
        ? el('p', { class: 'color-muted text-xs' }, _L('구성된 스위치가 없습니다', 'No switches configured'), el('br'), el('span', { class: 'text-12' }, _L('로컬 정책을 넣기 전, 현재 토폴로지가 비어 있는지 먼저 확인하십시오.', 'Confirm the topology is intentionally empty before adding local policy.')))
        : el('table', { class: 'table-sticky' },
            el('thead', null, el('tr', null, el('th', null, _L('이름', 'Name')))),
            el('tbody', null, sl.map(function(s) { const n = typeof s === 'string' ? s : (s.name || s.entry || JSON.stringify(s)); return el('tr', null, el('td', null, n)); }))));
    var routerPanel = el('div', { class: 'hc' },
      el('h4', null, _L('논리 라우터', 'Logical routers') + ' (' + rl.length + ')'),
      rl.length === 0
        ? el('p', { class: 'color-muted text-xs' }, _L('구성된 라우터가 없습니다', 'No routers configured'), el('br'), el('span', { class: 'text-12' }, _L('필요한 경우에만 수동 라우터를 추가하십시오.', 'Create a router only when the local design requires it.')))
        : el('table', { class: 'table-sticky' },
            el('thead', null, el('tr', null, el('th', null, _L('이름', 'Name')))),
            el('tbody', null, rl.map(function(r) { const n = typeof r === 'string' ? r : (r.name || r.entry || JSON.stringify(r)); return el('tr', null, el('td', null, n)); }))));
    var topoGrid = el('div', { class: 'sg grid-2' }, switchPanel, routerPanel);
    var lbPanel = el('div', { class: 'hc' },
      el('h4', null, '⚖ ' + _L('로드 밸런서 설정', 'LB setup')),
      el('p', { class: 'color-muted text-11 mb-8' }, _L('VIP와 백엔드 목록을 수동으로 입력해 단일 노드 로컬 구성을 점검합니다.', 'Enter the VIP and backend list manually to validate the local single-node setup.')),
      el('div', { class: 'mb-8 ops-stack-form' },
        el('div', { class: 'fr' }, el('label', { for: 'lb-n' }, _L('이름', 'Name')), el('input', { id: 'lb-n', placeholder: 'edge-lb' })),
        el('div', { class: 'fr' }, el('label', { for: 'lb-vip' }, 'VIP:Port'), el('input', { id: 'lb-vip', placeholder: '10.0.0.100' }), el('input', { id: 'lb-port', 'aria-label': 'Port', type: 'number', value: '80', class: 'w-60' })),
        el('div', { class: 'fr' }, el('label', { for: 'lb-bk' }, _L('백엔드', 'Backends')), el('input', { id: 'lb-bk', placeholder: '10.0.0.1:80,10.0.0.2:80' })),
        el('button', { class: 'btn btn-primary', onclick: 'nfvLbCreate()' }, _L('LB 생성', 'Create LB'))),
      el('div', { id: 'lb-list' }));
    var aclPanel = el('div', { class: 'hc' },
      el('h4', null, '🛡 ' + _L('ACL 정책 추가', 'ACL policy add')),
      el('p', { class: 'color-muted text-11 mb-8' }, _L('스위치 단위로 방향, 우선순위, 매치 조건을 입력해 ACL을 추가합니다.', 'Add ACLs per switch with direction, priority, and match conditions.')),
      el('div', { class: 'mb-8 ops-stack-form' },
        el('div', { class: 'fr' }, el('label', { for: 'fw-sw' }, _L('스위치', 'Switch')), el('input', { id: 'fw-sw', placeholder: 'web-tier' })),
        el('div', { class: 'fr' }, el('label', { for: 'fw-dir' }, _L('방향', 'Direction')), el('select', { id: 'fw-dir' }, el('option', null, 'from-lport'), el('option', null, 'to-lport'))),
        el('div', { class: 'fr' }, el('label', { for: 'fw-pri' }, _L('우선순위', 'Priority')), el('input', { id: 'fw-pri', type: 'number', value: '1000' })),
        el('div', { class: 'fr' }, el('label', { for: 'fw-match' }, 'Match'), el('input', { id: 'fw-match', placeholder: 'ip4.src==10.0.0.0/24' })),
        el('div', { class: 'fr' }, el('label', { for: 'fw-act' }, _L('동작', 'Action')), el('select', { id: 'fw-act' }, el('option', null, 'allow'), el('option', null, 'drop'), el('option', null, 'reject'))),
        el('button', { class: 'btn btn-primary', onclick: 'nfvFwAdd()' }, _L('ACL 규칙 추가', 'Add ACL rule'))));
    var lbAclGrid = el('div', { class: 'sg grid-2' }, lbPanel, aclPanel);
    clearEl(b);
    b.appendChild(frag(heading1, grid1, heading2, grid2, demoComposition, heading3, topoGrid, lbAclGrid));
    loadLBList();
  } catch (e) { PCV.uxlib.setMsg(b, null, { tag: 'p', cls: 'color-red' }, _L('OVN 정보를 불러오지 못했습니다', 'Unable to load OVN data')); }
}

async function loadLBList() { try { const el = document.getElementById('lb-list'); if (el) PCV.uxlib.setMsg(el, null, { tag: 'p', cls: 'color-muted text-xs' }, _L('로드 밸런서 상태 메모', 'Load balancer status note'), ': ', _L('위 가용성 카드에서 OVN 상태를 먼저 확인한 뒤 LB를 추가하십시오.', 'Confirm OVN availability above before adding a load balancer.')); } catch (e) { if(_DEBUG) console.warn('loadLBList:', e.message); } }
async function nfvLbCreate() {
  try {
    var name = document.getElementById('nfv-lb-name')?.value?.trim() || document.getElementById('lb-n')?.value?.trim();
    var vip = document.getElementById('nfv-lb-vip')?.value?.trim() || document.getElementById('lb-vip')?.value?.trim();
    var members = document.getElementById('nfv-lb-members')?.value?.trim() || document.getElementById('lb-bk')?.value?.trim();
    if (!name || !vip) { toast(_L('이름과 VIP를 입력하세요', 'Name and VIP required'), false); return; }
    var r = await fetchPost(EP.RPC(), {jsonrpc:'2.0', method:'nfv.lb.create', params:{
      name: name, vip: vip, members: members ? members.split(',').map(function(s){return s.trim();}) : []
    }, id:'nlb1'});
    if (r.error) { toast(r.error.message || 'Failed', false); return; }
    toast(_L('LB 생성됨', 'LB created'));
    addEvt('LB created: ' + name);
    renderContent();
  } catch (e) { toast(e.message, false); }
}
async function nfvFwAdd() { try { const sw = document.getElementById('fw-sw')?.value; const dir = document.getElementById('fw-dir')?.value; const pri = document.getElementById('fw-pri')?.value; const match = document.getElementById('fw-match')?.value; const act = document.getElementById('fw-act')?.value; if (!sw || !match) { toast('Switch and Match required', false); return; } const r = await fetchPost(EP.OVN_ACL(), { switch: sw, direction: dir, priority: +pri, match: match, action: act }); if (r && r.error) { toast(r.error.message || 'Failed', false); return; } toast('ACL rule added'); addEvt('ACL rule added to ' + sw); } catch (e) { toast(e.message, false); } }

/* ═══ SECURITY GROUPS ═══ */
async function renderSecGroups(b) {
  showSkeleton(b);
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  var grid = el('div', { class: 'sg grid-2 mb-16' },
    el('div', { class: 'hc' },
      el('h4', null, '🛡 OVN ACL 보안 그룹'),
      el('p', { class: 'color-muted text-12 mb-8' }, 'OVN ACL 기반 보안 그룹을 관리합니다. 논리 스위치에 인바운드/아웃바운드 규칙을 적용합니다.'),
      el('div', { class: 'mb-8' },
        el('div', { class: 'fr' }, el('label', { for: 'sg-switch' }, 'Switch'), el('input', { id: 'sg-switch', placeholder: 'web-tier', class: 'w-150' })),
        el('div', { class: 'fr' }, el('label', { for: 'sg-dir' }, 'Direction'), el('select', { id: 'sg-dir', class: 'input-pcv' }, el('option', { value: 'from-lport' }, 'Inbound (from-lport)'), el('option', { value: 'to-lport' }, 'Outbound (to-lport)'))),
        el('div', { class: 'fr' }, el('label', { for: 'sg-pri' }, 'Priority'), el('input', { id: 'sg-pri', type: 'number', value: '1000', class: 'w-80' })),
        el('div', { class: 'fr' }, el('label', { for: 'sg-match' }, 'Match'), el('input', { id: 'sg-match', placeholder: 'ip4.src==10.0.0.0/24', style: 'width:250px' })),
        el('div', { class: 'fr' }, el('label', { for: 'sg-act' }, 'Action'), el('select', { id: 'sg-act', class: 'input-pcv' }, el('option', null, 'allow'), el('option', null, 'drop'), el('option', null, 'reject'))),
        el('button', { class: 'btn btn-g', onclick: 'sgAddRule()' }, '+ ACL 규칙 추가')),
      el('div', { id: 'sg-result', class: 'mt-8' })),
    el('div', { class: 'hc' },
      el('h4', null, '📋 현재 ACL 규칙'),
      el('div', { class: 'fr' }, el('label', { for: 'sg-list-switch' }, 'Switch'), el('input', { id: 'sg-list-switch', placeholder: 'web-tier', class: 'w-150' }), ' ', el('button', { class: 'btn', onclick: 'sgListRules()' }, '조회')),
      el('div', { id: 'sg-rules', class: 'mt-8' })));
  var cliCard = HN.card('📖 CLI 명령어 참조',
    el('div', { style: 'font-size:12px;line-height:1.8;color:var(--fg2)' },
      el('code', { class: 'color-accent' }, 'pcvctl ovn acl list <switch>'), ' — ACL 규칙 목록', el('br'),
      el('code', { class: 'color-accent' }, 'pcvctl ovn acl add <switch> from-lport 1000 "ip4.src==10.0.0.0/24" allow'), ' — 규칙 추가', el('br'),
      el('code', { class: 'color-accent' }, 'pcvctl ovn tenant create <name> <cidr>'), ' — 멀티테넌트 생성 (스위치+ACL+DHCP)'));
  clearEl(b);
  b.appendChild(frag(HN.section('🛡 Security Groups'), grid, cliCard));
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
    const r = await fetchPost(EP.OVN_ACL(), { switch_name: sw, direction: dir, priority: parseInt(pri), match: match, action: act });
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
    /* 이름 충돌: 이 함수의 로컬 el 은 DOM 엘리먼트라 PCV.uxlib.el 별칭 금지 — 완전수식 호출 */
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

/* ═══ OVERLAY NETWORKS ═══ */
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
            el('td', null, HN.badge(v.state || '?', v.state === 'up' ? 'g' : 'r')));
        })));
    }
    clearEl(b);
    b.appendChild(frag(HN.section('Overlay Networks (VXLAN)'), body));
  } catch (e) {
    clearEl(b);
    b.appendChild(frag(HN.section('Overlay Networks'), el('p', { class: 'color-muted' }, 'Failed to load: ', e.message)));
  }
}
window.renderOverlayNetworks = renderOverlayNetworks;

/* ═══ NETWORK TOPOLOGY ═══ */
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
      HN.section(_L('네트워크 토폴로지', 'Network Topology')),
      el('canvas', { id: 'topo-canvas', width: '800', height: '500', style: 'width:100%;max-width:800px;border:1px solid var(--border);border-radius:6px;background:var(--bg2)' }),
      el('div', { class: 'flex gap-8 mt-8 text-xs' },
        el('span', null, '🖥 ' + _L('노드', 'Node')),
        el('span', null, '🌐 ' + _L('브릿지', 'Bridge')),
        el('span', null, '💻 VM'))));

    /* Draw topology */
    setTimeout(function() {
      var canvas = document.getElementById('topo-canvas');
      if (!canvas) return;
      var ctx = canvas.getContext('2d');
      var W = canvas.width;

      /* Compute styles */
      var style = getComputedStyle(document.documentElement);
      var accentColor = style.getPropertyValue('--accent').trim() || '#00f0ff';
      var greenColor = style.getPropertyValue('--green').trim() || '#00ff88';
      var fgColor = style.getPropertyValue('--fg').trim() || '#e0f0ff';
      var dimColor = style.getPropertyValue('--fg2').trim() || '#5a6a8a';

      /* Layout: nodes across top, bridges in middle, VMs at bottom */
      var nodes = (typeof MON_NODES !== 'undefined' && MON_NODES) ? MON_NODES : [{name:'Node1',ip:'localhost'}];

      /* Draw nodes */
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

      /* Draw bridges */
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
        /* Connect to closest node */
        var closest = nodePositions[0] || {x:x, y:50};
        ctx.strokeStyle = dimColor; ctx.lineWidth = 1;
        ctx.setLineDash([4, 4]);
        ctx.beginPath(); ctx.moveTo(x, y - 12); ctx.lineTo(closest.x, closest.y + 20); ctx.stroke();
        ctx.setLineDash([]);
      });

      /* Draw VMs */
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
        /* Connect to a bridge */
        if (bridgePositions.length > 0) {
          var br = bridgePositions[i % bridgePositions.length];
          ctx.strokeStyle = on ? accentColor : 'rgba(90,106,138,0.3)';
          ctx.lineWidth = on ? 1 : 0.5;
          ctx.beginPath(); ctx.moveTo(x, y - 10); ctx.lineTo(br.x, br.y + 12); ctx.stroke();
        }
      });
    }, 100);
  } catch (e) { PCV.uxlib.clearEl(b); b.appendChild(PCV.uxlib.frag(HN.section('Topology'), PCV.uxlib.el('p', { class: 'color-red' }, e.message))); }
}
window.renderTopology = renderTopology;

/* ═══ FIREWALL RULE EDITOR ═══ */
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
  /* 이름 충돌: 이 함수의 로컬 el 은 DOM 엘리먼트라 PCV.uxlib.el 별칭 금지 — 완전수식 호출 */
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
          PCV.uxlib.el('td', null, PCV.uxlib.el('button', { class: 'btn btn-sm btn-r', onclick: "fwDelRule('" + sg.name + "'," + (rule.db_id || 0) + ")" }, _L('삭제', 'Del'))));
      });
      kids.push(PCV.uxlib.el('table', { class: 'tbl mt-4' },
        PCV.uxlib.el('tbody', null,
          PCV.uxlib.el('tr', null, PCV.uxlib.el('th', null, 'Dir'), PCV.uxlib.el('th', null, 'Proto'), PCV.uxlib.el('th', null, 'Port'), PCV.uxlib.el('th', null, 'Source'), PCV.uxlib.el('th', null)),
          rows)));
    }
    return PCV.uxlib.el('div', { class: 'mb-8' }, kids);
  });
  el.appendChild(PCV.uxlib.frag(blocks));
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

/* ═══ REGISTER ALL ON window ═══ */
window.renderNetworks = renderNetworks;
window.showNetCreate = showNetCreate;
window.netModeChanged = netModeChanged;
window.loadPhysNics = loadPhysNics;
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
/* sgAddRule and sgListRules already assigned to window above */

/* ═══ PCV.network namespace export ═══ */
PCV.network = {
  renderNetworks: renderNetworks,
  toggleFwPanel: toggleFwPanel,
  toggleAclPanel: toggleAclPanel,
  showNetCreate: showNetCreate,
  netModeChanged: netModeChanged,
  loadPhysNics: loadPhysNics,
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
