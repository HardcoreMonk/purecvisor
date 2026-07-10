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
    let h = '<div class="ops-section-heading"><div><h3>' + _L('네트워크 인벤토리', 'Network inventory') + '</h3><p>' + _L('브리지, DHCP, 외부 연결 상태를 먼저 확인한 뒤 정책 편집으로 이어갑니다.', 'Review bridges, DHCP, and external connectivity first, then move into policy editing.') + '</p></div><button class="btn btn-primary" onclick="showNetCreate()">+ ' + t('net.new') + '</button></div>';
    if (l.length === 0) { b.innerHTML = h + '<div class="empty-state" style="text-align:center;padding:40px 20px"><div style="font-size:48px;margin-bottom:12px;opacity:.5">&#127760;</div><div style="font-size:14px;color:var(--fg2);margin-bottom:16px">' + _L('구성된 네트워크가 없습니다', 'No configured networks') + '</div><button class="btn btn-primary" onclick="showNetCreate()" class="text-12">+ ' + _L('네트워크 생성', 'Create network') + '</button></div>'; return; }
    if (compactMode) {
      h += '<div class="sg grid-2">';
      l.forEach(v => {
        const ext = v.mode === 'bridge' ? (escapeHtml(v.phys) || '-') : v.mode === 'nat' ? 'NAT' : '-';
        h += H.card('<b>' + escapeHtml(v.name) + '</b>', H.row(_L('모드', 'Mode'), H.badge(escapeHtml(v.mode) || '?', v.mode === 'nat' ? 'y' : v.mode === 'bridge' ? 'g' : 'r')) + H.row(_L('상태', 'State'), H.badge(escapeHtml(v.state) || '?', v.state === 'up' ? 'g' : 'r')) + H.row(_L('외부 연결', 'External'), ext) + H.row(_L('호스트 IP', 'Host IP'), escapeHtml(v.ip_cidr) || '-') + H.row('DHCP', v.dhcp ? 'ON' : '-') + H.row('VM', String(v.vm_count || 0)) + H.row(_L('서브넷', 'Subnet'), escapeHtml(v.subnet) || '-') + '<div class="flex gap-4 ops-action-row" style="margin-top:10px"><button class="btn btn-soft" style="font-size:10px;padding:3px 8px" onclick="showNetEdit(\'' + escapeAttr(v.name) + '\',\'' + escapeAttr(v.mode) + '\',\'' + escapeAttr(v.ip_cidr) + '\',' + (v.dhcp || false) + ',\'' + escapeAttr(v.phys || '') + '\')">' + t('btn.edit') + '</button><button class="btn btn-r" style="font-size:10px;padding:3px 8px" onclick="netDel(\'' + escapeAttr(v.name) + '\')">' + t('btn.delete') + '</button></div>', 'ops-mobile-card');
      });
      h += '</div>';
    } else {
      h += '<table class="table-sticky"><thead><tr><th>' + _L('네트워크', 'Network') + '</th><th>' + _L('모드', 'Mode') + '</th><th>' + _L('상태', 'State') + '</th><th>' + _L('외부 연결', 'External') + '</th><th>' + _L('호스트 IP', 'Host IP') + '</th><th>DHCP</th><th>VM</th><th>' + _L('서브넷', 'Subnet') + '</th><th>' + t('vm.settings') + '</th></tr></thead><tbody>';
      l.forEach(v => {
        const ext = v.mode === 'bridge' ? (escapeHtml(v.phys) || '-') : v.mode === 'nat' ? 'NAT' : '-';
        h += `<tr><td><b>${escapeHtml(v.name)}</b></td><td>${H.badge(escapeHtml(v.mode) || '?', v.mode === 'nat' ? 'y' : v.mode === 'bridge' ? 'g' : 'r')}</td><td>${H.badge(escapeHtml(v.state) || '?', v.state === 'up' ? 'g' : 'r')}</td><td>${ext}</td><td>${escapeHtml(v.ip_cidr) || '-'}</td><td>${v.dhcp ? 'ON' : '-'}</td><td>${v.vm_count || 0}</td><td>${escapeHtml(v.subnet) || '-'}</td><td class="nowrap"><button class="btn btn-soft" style="font-size:10px;padding:3px 8px" onclick="showNetEdit('${escapeAttr(v.name)}','${escapeAttr(v.mode) || ''}','${escapeAttr(v.ip_cidr) || ''}',${v.dhcp || false},'${escapeAttr(v.phys) || ''}')">${t('btn.edit')}</button> <button class="btn btn-r" style="font-size:10px;padding:3px 8px" onclick="netDel('${escapeAttr(v.name)}')">${t('btn.delete')}</button></td></tr>`;
      });
      h += '</tbody></table>';
    }
    h += '<div class="sg grid-2" style="margin-top:16px">';
    h += '<div class="hc"><h4>' + _L('방화벽 정책 편집', 'Firewall policy editor') + '</h4>';
    h += '<p class="color-muted text-11 mb-8">' + _L('브리지나 세그먼트를 확인한 뒤 인바운드/아웃바운드 규칙을 추가합니다.', 'Add ingress or egress rules after checking the bridge or segment you are editing.') + '</p>';
    h += '<div class="flex gap-8 mb-8 ops-form-strip" style="flex-wrap:wrap">';
    h += '<select id="fw-direction" aria-label="Direction" class="input" style="width:110px"><option value="ingress">' + _L('인바운드', 'Ingress') + '</option><option value="egress">' + _L('아웃바운드', 'Egress') + '</option></select>';
    h += '<select id="fw-protocol" aria-label="Protocol" class="input w-80"><option>tcp</option><option>udp</option><option>icmp</option></select>';
    h += '<input aria-label="' + _L('포트 예: 80 또는 8080-8090', 'Port e.g. 80 or 8080-8090') + '" id="fw-port" class="input" placeholder="' + _L('포트 예: 80 또는 8080-8090', 'Port e.g. 80 or 8080-8090') + '" class="w-140">';
    h += '<input aria-label="' + _L('소스 CIDR', 'Source CIDR') + '" id="fw-source" class="input" placeholder="' + _L('소스 CIDR', 'Source CIDR') + '" value="0.0.0.0/0" class="w-160">';
    h += '<button class="btn btn-primary" onclick="fwAddRule()">' + _L('규칙 추가', 'Add rule') + '</button>';
    h += '</div><div id="fw-rules-list"></div></div>';
    h += '<div class="hc"><h4>' + _L('OVN ACL 운영 메모', 'OVN ACL operations note') + '</h4>';
    h += '<p class="color-muted text-11 mb-8">' + _L('싱글 엣지에서는 상태를 먼저 확인하고, 필요한 경우 수동 ACL 명령으로 보강합니다.', 'In Single Edge, check state first and use manual ACL commands only when you need to refine policy.') + '</p>';
    h += '<pre style="background:var(--bg3);padding:8px;border-radius:6px;font-size:11px;overflow-x:auto">pcvctl ovn acl list &lt;switch&gt;\npcvctl ovn acl add &lt;switch&gt; --direction to-lport --match "ip4.src==10.0.0.0/24" --action allow\npcvctl ovn acl del &lt;switch&gt; --uuid &lt;uuid&gt;</pre>';
    h += '</div></div>';
    b.innerHTML = h;
  } catch (e) { if(_DEBUG) console.warn('n:', e.message); }
}
function toggleFwPanel() { var p = document.getElementById('fw-panel'); if (p) p.classList.toggle('hidden'); }
function toggleAclPanel() { var p = document.getElementById('acl-panel'); if (p) p.classList.toggle('hidden'); }
window.toggleFwPanel = toggleFwPanel;
window.toggleAclPanel = toggleAclPanel;

function showNetCreate() { showModal(`<h2>${t('net.new')}</h2><div class="fr"><label for="nn">Bridge</label><input id="nn" placeholder="pcvbr0"></div><div class="fr"><label for="nm">Mode</label><select id="nm" onchange="netModeChanged()"><option value="nat">${t('net.mode.nat')}</option><option value="isolated">${t('net.mode.isolated')}</option><option value="routed">${t('net.mode.routed')}</option><option value="bridge">${t('net.mode.bridge')}</option></select></div><div class="fr"><label for="nc">CIDR</label><input id="nc" placeholder="10.0.0.1/24"></div><div id="net-phys-row" class="fr hidden"><label for="np">Physical NIC</label><div class="flex gap-6 flex-1"><select id="np" style="flex:1;padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:6px;font-size:12px"><option value="">${t('loading')}</option></select></div></div><div class="stat-label" style="margin:4px 0 8px 98px" id="net-mode-hint">NAT: MASQUERADE + DHCP</div><div class="text-right mt-12"><button class="btn btn-g" onclick="doNetCreate()">${t('btn.create')}</button> <button class="btn btn-r" onclick="closeModal()">${t('btn.cancel')}</button></div>`); }

function netModeChanged() { const m = document.getElementById('nm').value; const pr = document.getElementById('net-phys-row'); const hint = document.getElementById('net-mode-hint'); const mr = document.getElementById('np-manual-row');
  if (m === 'bridge') { pr.classList.remove('hidden'); loadPhysNics(); hint.textContent = 'Bridge: Physical NIC binding'; } else { pr.classList.add('hidden'); if (mr) mr.remove(); const hints = { nat: 'NAT: MASQUERADE + DHCP', isolated: 'Isolated: VM-to-VM only', routed: 'Routed: ip_forward only' }; hint.textContent = hints[m] || ''; } }

async function loadPhysNics() { const sel = document.getElementById('np'); if (!sel) return;
  try { const r = await fetchGet(EP.NET_LIST()); const nl = unwrapList(r); let h = '<option value="">-- Select NIC --</option>'; const seen = new Set();
    nl.forEach(n => { (n.slaves || []).forEach(s => { if (!seen.has(s) && !s.startsWith('vnet')) { seen.add(s); h += '<option value="' + escapeHtml(s) + '">' + escapeHtml(s) + ' (slave of ' + escapeHtml(n.name) + ')</option>'; } }); }); h += '<option value="" disabled>-- Manual --</option>'; sel.innerHTML = h;
    const row = document.getElementById('net-phys-row');
    if (row && !document.getElementById('np-manual')) { const mi = document.createElement('div'); mi.className = 'fr'; mi.id = 'np-manual-row'; mi.style.marginTop = '4px'; mi.innerHTML = '<label for="np-manual">NIC Name</label><input id="np-manual" placeholder="e.g. wlo1, enp42s0, eno1..." style="flex:1;padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:6px;font-size:12px">'; row.parentNode.insertBefore(mi, row.nextSibling); }
  } catch (e) { sel.innerHTML = '<option value="">-- Enter NIC name below --</option>'; } }

async function doNetCreate() { var _btn = document.activeElement; if (_btn && _btn.tagName === 'BUTTON') { if (_btn.disabled) return; _btn.disabled = true; _btn.setAttribute('aria-busy', 'true'); } try { const body = { bridge_name: document.getElementById('nn').value, mode: document.getElementById('nm').value, cidr: document.getElementById('nc').value }; const m = body.mode; if (m === 'bridge') { const p = document.getElementById('np')?.value || document.getElementById('np-manual')?.value; if (p) body.physical_if = p; else { toast(t('net.phys_required'), false); return; } } const r = await fetchPost(EP.NET_LIST(), body); if (r.error) { toast(r.error.message || 'Failed', false); } else { toast(t('net.created')); addEvt(t('net.created')); } closeModal(); renderNetworks(document.getElementById('cb')); } catch (e) { toast(e.message, false); } finally { if (_btn && _btn.tagName === 'BUTTON') { _btn.disabled = false; _btn.removeAttribute('aria-busy'); } } }

async function netDel(name) { showModal(`<h2 class="color-red">&#9888; ${t('btn.delete')} Network</h2><p class="mb-12">${t('vm.delete.confirm').replace('VM', 'Network')} <b class="color-accent">${escapeHtml(name)}</b></p><p class="mb-12">${t('vm.delete.type_name').replace('VM', 'Network')}</p><div class="fr"><label for="del-net-confirm">Name</label><input id="del-net-confirm" placeholder="${escapeHtml(name)}"></div><div class="text-right mt-14"><button class="btn btn-r" onclick="doNetDel('${escapeAttr(name)}')">${t('btn.delete')}</button> <button class="btn" onclick="closeModal()">${t('btn.cancel')}</button></div>`); }

async function doNetDel(name) { const cv = document.getElementById('del-net-confirm')?.value; if (cv !== name) { toast(t('vm.name_mismatch'), false); return; }
  const mc = document.getElementById('mc'); mc.innerHTML = '<h2 class="color-red">&#9888; Deleting Network</h2><p><b class="color-accent">' + escapeHtml(name) + '</b></p><div class="prog-bar"><div class="prog-fill" id="dn-p" class="w-pct-20"></div></div><div class="prog-status" id="dn-s"><span class="spinner"></span>Removing firewall rules & DHCP...</div>';
  const pf = document.getElementById('dn-p'), ps = document.getElementById('dn-s');
  try { pf.style.width = '50%'; PCV.uxlib.setMsg(ps, 'loading', null, 'Sending DELETE...');
    const d = await fetchDelete(EP.NET_DETAIL(name)).catch(() => ({}));
    if (d.error) { pf.style.background = 'var(--red)'; pf.style.width = '100%'; ps.innerHTML = '&#10060; ' + escapeHtml(d.error.message); toast(t('btn.delete') + ' failed', false); return; }
    pf.style.width = '100%'; ps.innerHTML = '&#9989; ' + t('net.deleted'); toast(t('net.deleted')); addEvt(t('net.deleted') + ': ' + name); setTimeout(() => { closeModal(); renderNetworks(document.getElementById('cb')); }, 1500);
  } catch (e) { pf.style.width = '100%'; ps.innerHTML = '&#10060; ' + escapeHtml(e.message); toast(e.message, false); } }

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
    return note ? '<br><span class="color-muted text-11">' + escapeHtml(note) + '</span>' : '';
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
    const stepHtml = steps.map(function(step) { return H.badge(String(step), 'g'); }).join('<span class="color-muted text-11">→</span>');
    const body = '<p class="color-muted text-11" style="line-height:1.5;margin:0 0 10px">' + escapeHtml(flow.summary || '') + '</p><div class="flex gap-4" style="flex-wrap:wrap;align-items:center">' + stepHtml + '</div>';
    return H.card(escapeHtml(flow.title || _L('서비스 흐름', 'Service flow')), body);
  };
  const visualPath = Array.isArray(visual.path) && visual.path.length
    ? visual.path.map(function(part) { return escapeHtml(part); }).join(' &rarr; ')
    : 'ovn-demo-a &rarr; pcv-demo-ls &rarr; pcv-demo-lr &rarr; ovn-demo-b';
  const live = dd.status === 'ok' && dd.stale !== true;
  const liveBadge = H.badge(live ? _L('라이브', 'Live') : _L('점검 필요', 'Check'), live ? 'g' : 'r');
  const switchBadge = H.badge(switchPresent ? _L('존재', 'Present') : _L('누락', 'Missing'), switchPresent ? 'g' : 'r');
  const routerBadge = H.badge(routerPresent ? _L('존재', 'Present') : _L('누락', 'Missing'), routerPresent ? 'g' : 'r');
  const vmPath = escapeHtml(vmA.name || 'ovn-demo-a') + ' (' + escapeHtml(vmA.ip || '10.77.0.11') + ') &rarr; ' + escapeHtml(vmB.name || 'ovn-demo-b') + ' (' + escapeHtml(vmB.ip || '10.77.0.12') + ')';
  let h = '<div class="ops-section-heading"><div><h3>' + _L('OVN 데모 서비스 구성', 'OVN demo service composition') + '</h3><p>' + _L('공개 데모 도메인부터 VM A → VM B 검증까지 현재 구성값을 API 응답으로 조합해 표시합니다.', 'The current API response composes the public demo domain through the VM A to VM B validation path.') + '</p></div></div>';
  h += '<div class="sg grid-3">';
  h += H.card(_L('공개 진입점', 'Public entrypoint'), H.row(_L('도메인', 'Domain'), publicDomain) + H.row('HTTPS', H.badge('443', 'g')) + H.row(_L('노드', 'Node'), escapeHtml(nodeName)));
  h += H.card('PureCVisor API', H.row(_L('서비스', 'Service'), escapeHtml(hd.service || 'purecvisorsd')) + H.row(_L('상태', 'State'), H.badge(hd.status || 'ok', hd.status === 'critical' ? 'r' : 'g')) + H.row('OVN', H.badge(sd.available ? _L('활성', 'Enabled') : _L('비활성', 'Disabled'), sd.available ? 'g' : 'r')));
  h += H.card(_L('라이브 증거', 'Live evidence'), liveBadge + H.row(_L('마지막 점검', 'Last check'), escapeHtml(dd.checked_at || '-')) + H.row(_L('stale 기준', 'Stale after'), String(dd.stale_after_sec || 300) + 's'));
  h += H.card(_L('OVN 내부 시각 서비스', 'OVN internal visual service'), H.badge(visualOk ? _L('정상', 'OK') : _L('확인 필요', 'Check'), visualOk ? 'g' : 'r') + H.row(_L('서비스', 'Service'), escapeHtml(visual.service || 'ovn-demo-visual')) + H.row(_L('공개 URL', 'Public URL'), escapeHtml(visualPublicUrl)) + H.row(_L('내부 URL', 'Internal URL'), escapeHtml(visualUrl)) + H.row(_L('외부 inbound', 'External inbound'), visual.external_inbound === false ? _L('없음', 'None') : _L('확인 필요', 'Check')));
  h += '</div>';
  h += '<div class="ops-section-heading"><div><h3>' + _L('서비스 흐름', 'Service flow') + '</h3><p>' + _L('각 VM과 OVN 구성요소가 요청, 점검, 응답 흐름에서 맡는 역할을 단계별로 표시합니다.', 'Each VM and OVN component is shown as a step in the request, validation, and response flow.') + '</p></div></div>';
  h += '<div class="sg grid-3">' + serviceFlows.map(renderServiceFlow).join('') + '</div>';
  h += '<div class="sg grid-2">';
  h += '<div class="hc"><h4>' + _L('동적 토폴로지', 'Dynamic topology') + '</h4><table class="table-sticky"><thead><tr><th>' + _L('계층', 'Layer') + '</th><th>' + _L('현재 값', 'Current value') + '</th><th>' + _L('상태', 'State') + '</th></tr></thead><tbody>';
  h += '<tr><td>' + _L('브라우저', 'Browser') + '</td><td>https://' + publicDomain + '/ui/</td><td>' + H.badge(_L('공개', 'Public'), 'g') + '</td></tr>';
  h += '<tr><td>PureCVisor API</td><td>/api/v1/demo/ovn-ovs/health</td><td>' + H.badge(_L('읽기', 'Read'), 'g') + '</td></tr>';
  h += '<tr><td>OVN Logical Switch</td><td>' + escapeHtml(switchName) + nodeNote(switchName) + '</td><td>' + switchBadge + '</td></tr>';
  h += '<tr><td>OVN Logical Router</td><td>' + escapeHtml(routerName) + ' / 10.77.0.1' + nodeNote(routerName) + '</td><td>' + routerBadge + '</td></tr>';
  h += '<tr><td>VM A → VM B</td><td>' + vmPath + nodeNote(vmA.name || 'ovn-demo-a') + nodeNote(vmB.name || 'ovn-demo-b') + '</td><td>' + H.badge(pingOk && httpOk ? _L('성공', 'OK') : _L('확인 필요', 'Check'), pingOk && httpOk ? 'g' : 'r') + '</td></tr>';
  h += '<tr><td>' + _L('시각 서비스', 'Visual service') + '</td><td>' + visualPath + '<br><span class="color-muted text-11">' + escapeHtml(visualPublicUrl) + ' · ' + escapeHtml(visualUrl) + ' · ' + _L('외부 inbound 없음', 'No external inbound') + '</span>' + nodeNote('ovn-demo-b') + '</td><td>' + H.badge(visualOk ? _L('성공', 'OK') : _L('확인 필요', 'Check'), visualOk ? 'g' : 'r') + '</td></tr>';
  h += '</tbody></table></div>';
  h += '<div class="hc"><h4>' + _L('viewer 읽기 전용 경계', 'Viewer read-only boundary') + '</h4><table class="table-sticky"><thead><tr><th>' + _L('요청', 'Request') + '</th><th>' + _L('기대 결과', 'Expected') + '</th></tr></thead><tbody>';
  h += '<tr><td>GET /api/v1/ovn/status</td><td>' + H.badge('200', 'g') + '</td></tr>';
  h += '<tr><td>GET /api/v1/ovn/switches</td><td>' + H.badge('200', 'g') + '</td></tr>';
  h += '<tr><td>GET /api/v1/demo/ovn-ovs/health</td><td>' + H.badge('200', 'g') + '</td></tr>';
  h += '<tr><td>GET /api/v1/vnc/' + escapeHtml(vmA.name || 'ovn-demo-a') + '</td><td>' + H.badge('403', 'r') + '</td></tr>';
  h += '<tr><td>POST /api/v1/vms/' + escapeHtml(vmA.name || 'ovn-demo-a') + '/stop</td><td>' + H.badge('403', 'r') + '</td></tr>';
  h += '</tbody></table></div>';
  h += '</div>';
  return h;
}

function showNetEdit(name, mode, cidr, dhcp, phys) {
  showModal(`<h2>&#9881; ${t('btn.edit')}: ${escapeHtml(name)}</h2><div class="fr"><label for="net-bridge">Bridge</label><input id="net-bridge" value="${escapeHtml(name)}" disabled style="opacity:.6"></div><div class="fr"><label for="ne-mode">Mode</label><select id="ne-mode" onchange="netEditModeChanged()"><option value="nat" ${mode === 'nat' ? 'selected' : ''}>nat</option><option value="isolated" ${mode === 'isolated' ? 'selected' : ''}>isolated</option><option value="routed" ${mode === 'routed' ? 'selected' : ''}>routed</option><option value="bridge" ${mode === 'bridge' ? 'selected' : ''}>bridge</option></select></div><div class="fr"><label for="ne-dhcp">DHCP</label><select id="ne-dhcp"><option value="on" ${dhcp ? 'selected' : ''}>ON</option><option value="off" ${!dhcp ? 'selected' : ''}>OFF</option></select></div><div class="fr" id="ne-phys-row" ${mode !== 'bridge' ? 'class="hidden"' : ''}><label for="ne-phys">Physical NIC</label><input id="ne-phys" value="${escapeHtml(phys)}" placeholder="e.g. wlo1, enp42s0"></div><div class="stat-label" style="margin:4px 0 8px 98px" id="ne-hint">Mode: ${escapeHtml(mode)}</div><div class="text-right mt-14"><button class="btn btn-g" onclick="doNetEdit('${escapeAttr(name)}')">${t('btn.apply')}</button> <button class="btn btn-r" onclick="closeModal()">${t('btn.cancel')}</button></div>`);
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
    let h = '<div class="ops-section-heading"><div><h3>OVN SDN</h3><p>' + _L('싱글 엣지에서 허용된 로컬 OVN 상태와 수동 구성을 확인합니다.', 'Review the local OVN state and the manually managed configuration allowed in Single Edge.') + '</p></div></div>';
    h += '<div class="sg grid-3">';
    h += H.card(_L('OVN 가용성', 'OVN availability'), sd.available ? H.badge(_L('준비됨', 'Ready'), 'g') + H.row('Encap', 'Geneve') : '<p class="color-muted text-xs">' + _L('OVN이 설치되지 않았습니다', 'OVN is not installed') + '</p>');
    h += H.card(_L('논리 스위치', 'Logical switches'), '<div class="stat-lg color-accent">' + sl.length + '</div>' + H.row(_L('상태', 'State'), H.badge(sd.available ? _L('조회 가능', 'Available') : _L('미설치', 'Unavailable'), sd.available ? 'g' : 'r')));
    h += H.card(_L('논리 라우터', 'Logical routers'), '<div class="stat-lg color-green">' + rl.length + '</div>' + H.row(_L('운영 방식', 'Mode'), _L('수동 구성', 'Manual configuration')));
    h += '</div>';
    h += '<div class="ops-section-heading"><div><h3>' + _L('공개 OVN 데모 헬스', 'Public OVN demo health') + '</h3><p>' + _L('viewer 권한으로 데모 VM 간 실제 통신 결과를 읽기 전용으로 확인합니다.', 'Viewer users can inspect the live VM-to-VM demo result without control permissions.') + '</p></div></div>';
    h += '<div class="sg grid-3">';
    h += H.card(_L('VM 간 통신', 'VM reachability'), H.badge(demoLive ? _L('정상', 'OK') : _L('확인 필요', 'Check'), demoBadge) + H.row('Ping', pingOk ? 'OK' : 'N/A') + H.row('HTTP', httpOk ? 'OK' : 'N/A') + H.row(_L('시각 서비스', 'Visual service'), visualOk ? 'OK' : 'N/A'));
    h += H.card(_L('데모 네트워크', 'Demo network'), H.row(_L('스위치', 'Switch'), escapeHtml(dd.switch || 'pcv-demo-ls')) + H.row(_L('라우터', 'Router'), escapeHtml(dd.router || 'pcv-demo-lr')));
    h += H.card(_L('최근 점검', 'Last check'), H.row(_L('시각', 'Time'), escapeHtml(dd.checked_at || '-')) + H.row(_L('상태', 'State'), dd.stale ? 'stale' : 'live'));
    h += '</div>';
    h += renderOvnDemoServiceComposition(dd, sd, sl, rl, hd);
    h += '<div class="ops-section-heading"><div><h3>' + _L('논리 토폴로지', 'Logical topology') + '</h3><p>' + _L('스위치와 라우터 목록을 먼저 확인한 뒤 수동 정책과 부가 구성을 적용합니다.', 'Review switches and routers first, then apply manual policy and optional configuration.') + '</p></div></div>';
    h += '<div class="sg grid-2">';
    h += '<div class="hc"><h4>' + _L('논리 스위치', 'Logical switches') + ' (' + sl.length + ')</h4>';
    if (sl.length === 0) h += '<p class="color-muted text-xs">' + _L('구성된 스위치가 없습니다', 'No switches configured') + '<br><span class="text-12">' + _L('로컬 정책을 넣기 전, 현재 토폴로지가 비어 있는지 먼저 확인하십시오.', 'Confirm the topology is intentionally empty before adding local policy.') + '</span></p>';
    else h += '<table class="table-sticky"><thead><tr><th>' + _L('이름', 'Name') + '</th></tr></thead><tbody>' + sl.map(function(s) { const n = typeof s === 'string' ? s : (s.name || s.entry || JSON.stringify(s)); return '<tr><td>' + esc(n) + '</td></tr>'; }).join('') + '</tbody></table>';
    h += '</div>';
    h += '<div class="hc"><h4>' + _L('논리 라우터', 'Logical routers') + ' (' + rl.length + ')</h4>';
    if (rl.length === 0) h += '<p class="color-muted text-xs">' + _L('구성된 라우터가 없습니다', 'No routers configured') + '<br><span class="text-12">' + _L('필요한 경우에만 수동 라우터를 추가하십시오.', 'Create a router only when the local design requires it.') + '</span></p>';
    else h += '<table class="table-sticky"><thead><tr><th>' + _L('이름', 'Name') + '</th></tr></thead><tbody>' + rl.map(function(r) { const n = typeof r === 'string' ? r : (r.name || r.entry || JSON.stringify(r)); return '<tr><td>' + esc(n) + '</td></tr>'; }).join('') + '</tbody></table>';
    h += '</div></div>';
    h += '<div class="sg grid-2">';
    h += '<div class="hc"><h4>&#9878; ' + _L('로드 밸런서 설정', 'LB setup') + '</h4><p class="color-muted text-11 mb-8">' + _L('VIP와 백엔드 목록을 수동으로 입력해 단일 노드 로컬 구성을 점검합니다.', 'Enter the VIP and backend list manually to validate the local single-node setup.') + '</p><div class="mb-8 ops-stack-form"><div class="fr"><label for="lb-n">' + _L('이름', 'Name') + '</label><input id="lb-n" placeholder="edge-lb"></div><div class="fr"><label for="lb-vip">VIP:Port</label><input id="lb-vip" placeholder="10.0.0.100"><input id="lb-port" aria-label="Port" type="number" value="80" class="w-60"></div><div class="fr"><label for="lb-bk">' + _L('백엔드', 'Backends') + '</label><input id="lb-bk" placeholder="10.0.0.1:80,10.0.0.2:80"></div><button class="btn btn-primary" onclick="nfvLbCreate()">' + _L('LB 생성', 'Create LB') + '</button></div><div id="lb-list"></div></div>';
    h += '<div class="hc"><h4>&#128737; ' + _L('ACL 정책 추가', 'ACL policy add') + '</h4><p class="color-muted text-11 mb-8">' + _L('스위치 단위로 방향, 우선순위, 매치 조건을 입력해 ACL을 추가합니다.', 'Add ACLs per switch with direction, priority, and match conditions.') + '</p><div class="mb-8 ops-stack-form"><div class="fr"><label for="fw-sw">' + _L('스위치', 'Switch') + '</label><input id="fw-sw" placeholder="web-tier"></div><div class="fr"><label for="fw-dir">' + _L('방향', 'Direction') + '</label><select id="fw-dir"><option>from-lport</option><option>to-lport</option></select></div><div class="fr"><label for="fw-pri">' + _L('우선순위', 'Priority') + '</label><input id="fw-pri" type="number" value="1000"></div><div class="fr"><label for="fw-match">Match</label><input id="fw-match" placeholder="ip4.src==10.0.0.0/24"></div><div class="fr"><label for="fw-act">' + _L('동작', 'Action') + '</label><select id="fw-act"><option>allow</option><option>drop</option><option>reject</option></select></div><button class="btn btn-primary" onclick="nfvFwAdd()">' + _L('ACL 규칙 추가', 'Add ACL rule') + '</button></div></div>';
    h += '</div>';
    b.innerHTML = h; loadLBList();
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
async function nfvFwAdd() { try { const sw = document.getElementById('fw-sw')?.value; const dir = document.getElementById('fw-dir')?.value; const pri = document.getElementById('fw-pri')?.value; const match = document.getElementById('fw-match')?.value; const act = document.getElementById('fw-act')?.value; if (!sw || !match) { toast('Switch and Match required', false); return; } await fetchPost(EP.OVN_ACL(), { switch: sw, direction: dir, priority: +pri, match: match, action: act }); toast('ACL rule added'); addEvt('ACL rule added to ' + sw); } catch (e) { toast(e.message, false); } }

/* ═══ SECURITY GROUPS ═══ */
async function renderSecGroups(b) {
  showSkeleton(b);
  let h = H.section('&#128737; Security Groups');
  h += '<div class="sg grid-2 mb-16">';
  h += '<div class="hc"><h4>&#128737; OVN ACL 보안 그룹</h4>';
  h += '<p class="color-muted text-12 mb-8">OVN ACL 기반 보안 그룹을 관리합니다. 논리 스위치에 인바운드/아웃바운드 규칙을 적용합니다.</p>';
  h += '<div class="mb-8">';
  h += '<div class="fr"><label for="sg-switch">Switch</label><input id="sg-switch" placeholder="web-tier" class="w-150"></div>';
  h += '<div class="fr"><label for="sg-dir">Direction</label><select id="sg-dir" class="input-pcv"><option value="from-lport">Inbound (from-lport)</option><option value="to-lport">Outbound (to-lport)</option></select></div>';
  h += '<div class="fr"><label for="sg-pri">Priority</label><input id="sg-pri" type="number" value="1000" class="w-80"></div>';
  h += '<div class="fr"><label for="sg-match">Match</label><input id="sg-match" placeholder="ip4.src==10.0.0.0/24" style="width:250px"></div>';
  h += '<div class="fr"><label for="sg-act">Action</label><select id="sg-act" class="input-pcv"><option>allow</option><option>drop</option><option>reject</option></select></div>';
  h += '<button class="btn btn-g" onclick="sgAddRule()">+ ACL 규칙 추가</button>';
  h += '</div><div id="sg-result" class="mt-8"></div></div>';
  h += '<div class="hc"><h4>&#128203; 현재 ACL 규칙</h4>';
  h += '<div class="fr"><label for="sg-list-switch">Switch</label><input id="sg-list-switch" placeholder="web-tier" class="w-150"> <button class="btn" onclick="sgListRules()">조회</button></div>';
  h += '<div id="sg-rules" class="mt-8"></div></div>';
  h += '</div>';
  h += H.card('&#128214; CLI 명령어 참조', '<div style="font-size:12px;line-height:1.8;color:var(--fg2)">' +
    '<code class="color-accent">pcvctl ovn acl list &lt;switch&gt;</code> &mdash; ACL 규칙 목록<br>' +
    '<code class="color-accent">pcvctl ovn acl add &lt;switch&gt; from-lport 1000 &quot;ip4.src==10.0.0.0/24&quot; allow</code> &mdash; 규칙 추가<br>' +
    '<code class="color-accent">pcvctl ovn tenant create &lt;name&gt; &lt;cidr&gt;</code> &mdash; 멀티테넌트 생성 (스위치+ACL+DHCP)</div>');
  b.innerHTML = h;
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
    await fetchPost(EP.OVN_ACL(), { switch_name: sw, direction: dir, priority: parseInt(pri), match: match, action: act });
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
    let h = '<table class="text-11"><thead><tr><th>Direction</th><th>Priority</th><th>Match</th><th>Action</th></tr></thead><tbody>';
    list.forEach(a => {
      const entry = typeof a === 'string' ? a : '';
      if (entry) { h += '<tr><td colspan="4">' + escapeHtml(entry) + '</td></tr>'; }
      else { h += '<tr><td>' + escapeHtml(a.direction || '') + '</td><td>' + escapeHtml(String(a.priority || '')) + '</td><td>' + escapeHtml(a.match || '') + '</td><td>' + escapeHtml(a.action || '') + '</td></tr>'; }
    });
    h += '</tbody></table>';
    if (el) el.innerHTML = h;
  } catch (e) { if (el) PCV.uxlib.setMsg(el, null, { cls: 'color-red' }, '오류: ', e.message); }
};

/* ═══ OVERLAY NETWORKS ═══ */
async function renderOverlayNetworks(b) {
  showSkeleton(b);
  try {
    const r = await fetchGet(EP.OVERLAY_LIST());
    const l = unwrapList(r);
    let h = H.section('Overlay Networks (VXLAN)');
    if (!Array.isArray(l) || l.length === 0) {
      h += '<div class="empty-state"><div class="empty-state-icon">&#127760;</div><div class="empty-state-text">No overlay networks</div></div>';
    } else {
      h += '<table class="table-sticky"><thead><tr><th>Name</th><th>VNI</th><th>Peers</th><th>Status</th></tr></thead><tbody>';
      l.forEach(v => {
        h += '<tr><td><b>' + esc(v.name || '?') + '</b></td><td>' + (v.vni || '-') + '</td><td>' + (v.peer_count || 0) + '</td><td>' + H.badge(v.state || '?', v.state === 'up' ? 'g' : 'r') + '</td></tr>';
      });
      h += '</tbody></table>';
    }
    b.innerHTML = h;
  } catch (e) { b.innerHTML = H.section('Overlay Networks') + '<p class="color-muted">Failed to load: ' + esc(e.message) + '</p>'; }
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

    var h = H.section(_L('네트워크 토폴로지', 'Network Topology'));
    h += '<canvas id="topo-canvas" width="800" height="500" style="width:100%;max-width:800px;border:1px solid var(--border);border-radius:6px;background:var(--bg2)"></canvas>';
    h += '<div class="flex gap-8 mt-8 text-xs">';
    h += '<span>&#128421; ' + _L('노드', 'Node') + '</span>';
    h += '<span>&#127760; ' + _L('브릿지', 'Bridge') + '</span>';
    h += '<span>&#128187; VM</span>';
    h += '</div>';
    b.innerHTML = h;

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
  } catch (e) { b.innerHTML = H.section('Topology') + '<p class="color-red">' + esc(e.message) + '</p>'; }
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
  var h = '';
  groups.forEach(function(sg) {
    h += '<div class="mb-8"><strong>' + esc(sg.name) + '</strong> <span class="stat-label">(' + (sg.rule_count || 0) + ' rules)</span>';
    if (sg.rules && sg.rules.length) {
      h += '<table class="tbl mt-4"><tr><th>Dir</th><th>Proto</th><th>Port</th><th>Source</th><th></th></tr>';
      sg.rules.forEach(function(r) {
        var portStr = r.port_end > r.port_start ? r.port_start + '-' + r.port_end : (r.port_start || '*');
        h += '<tr><td>' + esc(r.direction) + '</td><td>' + esc(r.protocol) + '</td><td>' + portStr + '</td><td>' + esc(r.source) + '</td>';
        h += '<td><button class="btn btn-sm btn-r" onclick="fwDelRule(\'' + esc(sg.name) + '\',' + (r.db_id || 0) + ')">' + _L('삭제','Del') + '</button></td></tr>';
      });
      h += '</table>';
    }
    h += '</div>';
  });
  el.innerHTML = h || '<div class="stat-label">No security groups</div>';
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
