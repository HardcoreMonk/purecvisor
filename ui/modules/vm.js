
window.PCV = window.PCV || {};
(function(PCV) {

var _lastVmListHash = '';
var vmViewMode = localStorage.getItem('pcv-vm-view') || 'list';

function toggleVmView() {
  vmViewMode = vmViewMode === 'list' ? 'card' : 'list';
  localStorage.setItem('pcv-vm-view', vmViewMode);
  render();
}
function setSort(k) {
  if (sortField === k) sortDirection *= -1;
  else { sortField = k; sortDirection = 1; }
  render();
}

function getFiltered() {
  const f = (document.getElementById('vf') || {}).value || '';
  let l = [...vmList];
  if (f) l = l.filter(v => typeof window.fuzzyMatch === 'function' ? window.fuzzyMatch(v.name, f) : v.name.toLowerCase().includes(f.toLowerCase()));
  l.sort((a, b) => {
    let va, vb;
    if (sortField === 'cpu') { va = a.live_cpu_pct || 0; vb = b.live_cpu_pct || 0; }
    else if (sortField === 'mem') { va = a.mem_percent || 0; vb = b.mem_percent || 0; }
    else if (sortField === 'state') { va = a.state; vb = b.state; }
    else { va = a.name; vb = b.name; }
    return va < vb ? -sortDirection : va > vb ? sortDirection : 0;
  });

  const favs = getFavorites();
  l.sort((a, b) => {
    const af = favs.includes(a.name) ? 0 : 1;
    const bf = favs.includes(b.name) ? 0 : 1;
    return af - bf;
  });
  return l;
}

var _renderInFlight = false;
function render(skipContent) {
  if (_renderInFlight) return;
  _renderInFlight = true;
  try { _renderCore(skipContent); } finally { _renderInFlight = false; }
}
function _renderCore(skipContent) {
  var newHash = vmList.map(function(v){return v.name+v.state+(v.live_cpu_pct||0);}).join('|');
  if (skipContent && newHash === _lastVmListHash) return;
  _lastVmListHash = newHash;
  const l = getFiltered();
  const favs = getFavorites();

  vmList.forEach(function(v) {
    if (!_vmCpuHist[v.name]) _vmCpuHist[v.name] = [];
    _vmCpuHist[v.name].push(v.live_cpu_pct || 0);
    if (_vmCpuHist[v.name].length > 30) _vmCpuHist[v.name].shift();
  });

  if (l.length === 0 && typeof emptyStatePro === 'function') {
    var _vl = document.getElementById('vl');
    PCV.uxlib.clearEl(_vl);
    _vl.appendChild(emptyStatePro({
      icon: '&#128187;',
      title: _L('VM이 없습니다', 'No virtual machines'),
      desc: _L('첫 VM을 만들어 시작하세요. 몇 초 안에 부팅 가능합니다.', 'Create your first VM. Boots in seconds.'),
      ctaLabel: _L('+ VM 만들기', '+ Create VM'),
      ctaAction: 'showCreate()'
    }));
    return;
  }
  var el = PCV.uxlib.el;
  var parts = [];
  parts.push(el('div', { class: 'flex gap-4 mb-8 justify-end' },
    el('button', { class: 'btn ' + (vmViewMode === 'list' ? 'btn-g' : '') + ' btn-xs', onclick: 'toggleVmView()' }, '☰ ' + _L('목록', 'List')),
    el('button', { class: 'btn ' + (vmViewMode === 'card' ? 'btn-g' : '') + ' btn-xs', onclick: 'toggleVmView()' }, '▦ ' + _L('카드', 'Card')),
    el('button', { class: 'btn', onclick: 'showVmCompare()' }, _L('비교', 'Compare')),
    el('button', { class: 'btn', onclick: 'showBulkActions()', 'data-role': 'OPERATOR,ADMIN' }, _L('일괄 작업', 'Bulk'))));
  if (vmViewMode === 'card') {
    var cardGrid = el('div', { class: 'sg grid-3' });
    l.forEach(function(v, ri) {
      var on = v.state === 'running';
      var cp = v.live_cpu_pct || 0;
      var mp = v.mem_percent || 0;
      cardGrid.appendChild(el('div', { class: 'hc', draggable: 'true', ondragstart: "event.dataTransfer.setData('text/plain','" + v.name + "')", style: 'cursor:grab;border-left:3px solid ' + (on ? 'var(--green)' : 'var(--red)'), onclick: 'selectedVmIndex=' + vmList.indexOf(v) + ";currentTab='summary';switchSbTab('vms');render()" },
        el('div', { class: 'flex items-center gap-6 mb-6' },
          el('span', { style: 'font-size:8px;color:' + (on ? 'var(--green)' : 'var(--red)') }, '●'),
          el('b', null, v.name)),
        el('div', { class: 'flex gap-8 text-11' },
          el('div', { class: 'flex-1' }, el('div', { class: 'color-muted' }, 'CPU'), _vmProgressBar(cp)),
          el('div', { class: 'flex-1' }, el('div', { class: 'color-muted' }, 'MEM'), _vmProgressBar(mp))),
        el('div', { class: 'flex gap-8 mt-6 text-xs color-muted' },
          el('span', null, (v.vcpu || '?') + ' vCPU'),
          el('span', null, (v.memory_mb || '?') + ' MB'),
          el('span', null, HN.badge(v.state || '?', on ? 'g' : 'r')))));
    });
    parts.push(cardGrid);

    parts.push(el('h3', { style: 'margin:16px 0 8px' }, _L('마이그레이션 대상 노드', 'Migration Target Nodes')));
    var nodeGrid = el('div', { class: 'sg grid-3' });
    var nodes = (typeof MON_NODES !== 'undefined' && MON_NODES) ? MON_NODES : [{name:'Node1',ip:'localhost'}];
    nodes.forEach(function(nd) {
      nodeGrid.appendChild(el('div', { class: 'hc', style: 'text-align:center;padding:20px;border:2px dashed var(--border);transition:border-color 0.2s', ondragover: "event.preventDefault();this.style.borderColor='var(--accent)'", ondragleave: "this.style.borderColor='var(--border)'", ondrop: "event.preventDefault();this.style.borderColor='var(--border)';vmMigrateDrop(event.dataTransfer.getData('text/plain'),'" + nd.ip + "','" + nd.name + "')" },
        el('div', { style: 'font-size:24px;margin-bottom:6px' }, '🖥'),
        el('div', { class: 'text-13 font-600' }, nd.name),
        el('div', { class: 'color-muted text-xs' }, nd.ip)));
    });
    parts.push(nodeGrid);
  } else {
    l.forEach((v, i) => {
      const ri = vmList.indexOf(v);
      const on = v.state === 'running';
      const cp = v.live_cpu_pct || 0;
      const c = cp > 85 ? 'var(--red)' : cp > 60 ? 'var(--yellow)' : 'var(--green)';
      const star = favs.includes(v.name) ? '★' : '☆';
      parts.push(el('div', { class: 'vi ' + (ri === selectedVmIndex ? 'active' : ''), onclick: 'selectedVmIndex=' + ri + ";currentTab=localStorage.getItem('pcv-last-vm-tab')||'summary';switchSbTab('vms');document.querySelectorAll('#ct button').forEach(b=>b.classList.toggle('active',b.dataset.t==='summary'));render()", oncontextmenu: 'showCtx(event,' + ri + ')' },
        el('input', { type: 'checkbox', checked: checkedVms.has(ri) ? '' : null, 'aria-label': 'Select ' + v.name, onclick: 'event.stopPropagation();toggleChk(' + ri + ')' }),
        el('span', { class: 'fav-star', onclick: "event.stopPropagation();toggleFavorite('" + escapeAttr(v.name) + "')", title: 'Favorite' }, star),
        el('span', { class: 'dot ' + (on ? 'on' : 'off') }),
        el('span', { class: 'nm' }, v.name),
        el('span', { class: 'mini-bar' }, el('span', { class: 'mini-fill pcv-bar-fill-inline', style: '--bw:' + cp + '%;--bc:' + c })),
        el('span', { class: 'st' }, cp.toFixed(0) + '%'),
        el('canvas', { class: 'vm-spark', id: 'spark-' + v.name, width: '40', height: '14', style: 'vertical-align:middle;margin-left:4px' })));
    });
  }
  var vl = document.getElementById('vl');
  PCV.uxlib.clearEl(vl);
  vl.appendChild(PCV.uxlib.frag(parts));

  setTimeout(function() {
    vmList.forEach(function(v) {
      var canvas = document.getElementById('spark-' + v.name);
      if (!canvas) return;
      var hist = _vmCpuHist[v.name] || [];
      if (hist.length < 2) return;
      var ctx = canvas.getContext('2d');
      var w = canvas.width, ht = canvas.height;
      ctx.clearRect(0, 0, w, ht);
      ctx.strokeStyle = 'rgba(0,240,255,0.6)';
      ctx.lineWidth = 1;
      ctx.beginPath();
      for (var si = 0; si < hist.length; si++) {
        var x = (si / (hist.length - 1)) * w;
        var y = ht - (hist[si] / 100) * ht;
        if (si === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
      }
      ctx.stroke();
    });
  }, 50);
  document.getElementById('vc').textContent = vmList.length;
  document.getElementById('sb2').textContent = vmList[selectedVmIndex] ? vmList[selectedVmIndex].name : t('no_vm');
  document.getElementById('bbtn').style.display = checkedVms.size > 0 ? 'inline' : 'none';

  const elapsed = Math.round((Date.now() - lastLoadTime) / 1000);
  const sb3 = document.getElementById('sb3');
  if (sb3) sb3.textContent = 'Updated ' + elapsed + 's ago';

  const ctBar = document.getElementById('ct');
  if (ctBar && ['summary','console','snapshots','performance','timeline'].includes(currentTab)) {
    ctBar.style.display = 'flex';
  }
  if (!skipContent) renderContent();
}

function toggleChk(i) {
  checkedVms.has(i) ? checkedVms.delete(i) : checkedVms.add(i);
  render();
}

async function bulkStop() {
  if (!await customConfirm(t('btn.stop_selected'), 'Stop ' + checkedVms.size + ' VMs?')) return;
  var total = checkedVms.size;
  var failed = [];
  for (const i of checkedVms) {
    const r = await fetchPost(EP.VM_STOP(vmList[i].name), {});
    if (r && r.error) { failed.push(vmList[i].name + ': ' + (r.error.message || '')); continue; }
    addEvt('VM Bulk stop — ' + vmList[i].name);
  }
  checkedVms.clear();
  if (failed.length) toast(failed.length + ' / ' + total + ' stop failed', false);
  setTimeout(loadAll, 1500);
}

document.addEventListener('keydown', function(e) {
  if (e.defaultPrevented) return;
  if (e.target.closest && e.target.closest('.menubar, [role="button"], [role="link"], [role="menuitem"]')) return;
  if (e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA' || e.target.tagName === 'SELECT') return;
  if (currentTab !== 'summary' && currentTab !== 'dashboard' && currentTab !== 'console' && currentTab !== 'snapshots' && currentTab !== 'performance') return;
  if (e.key === 'ArrowDown' || e.key === 'j') {
    e.preventDefault();
    if (selectedVmIndex < vmList.length - 1) { selectedVmIndex++; render(); }
  } else if (e.key === 'ArrowUp' || e.key === 'k') {
    e.preventDefault();
    if (selectedVmIndex > 0) { selectedVmIndex--; render(); }
  } else if (e.key === 'Enter') {
    e.preventDefault();
    currentTab = 'summary'; switchSbTab('vms'); render();
  }
});

var _vmCpuHist = {};

function showCtx(e, i) {
  e.preventDefault();
  selectedVmIndex = i;
  const m = document.getElementById('ctx');
  const ri = i;

  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  var sep = function() { return el('div', { class: 'sep' }); };
  var ci = function(label, onClick) { return el('div', { class: 'ci', onClick: onClick }, label); };
  clearEl(m);
  m.appendChild(frag(
    ci('▶ ' + t('power.start'), function() { window.vmPower('start'); }),
    ci('■ ' + t('power.stop'), function() { window.vmPower('stop'); }),
    sep(),
    ci('📷 ' + t('vm.snapshot'), function() { window.showSnap(); }),
    ci('⚙ ' + t('vm.settings'), function() { window.showSettings(); }),
    ci('✎ ' + _L('이름 변경', 'Rename'), function() { window.showRenameVm(); }),
    ci('🖨 VNC', function() { window.showVnc(); }),
    sep(),
    ci('📌 Memory Stats', function() { window.showMemStats(); }),
    ci('⚙ CPU Stats', function() { window.showCpuStats(); }),
    ci('💾 Disk Resize', function() { window.showDiskLiveResize(); }),
    ci('💬 Guest Agent', function() { window.showGuestAgent(); }),
    sep(),
    ci('🌐 NIC', function() { window.showNicMgr(); }),
    ci('📋 Clone', function() { window.vmClone(ri); }),
    ci('📦 Export OVA', function() { window.vmExportOva(ri); }),
    sep(),
    ci('❌ ' + t('btn.delete'), function() { window.vmDel(); })
  ));
  m.style.display = 'block';
  m.style.left = e.pageX + 'px';
  m.style.top = e.pageY + 'px';
  render();
}

function _vmStripCidr(v) {
  return String(v || '').split('/')[0];
}

function _vmNetSource(nic) {
  return nic.bridge || nic.source || nic.network || nic.type || '-';
}

function _vmNetworkMap(networks) {
  var map = {};
  (networks || []).forEach(function(n) {
    if (n && n.name) map[n.name] = n;
  });
  return map;
}

function _vmNicDns(nic, netMap) {
  var raw = String((nic && nic.dns) || '').trim();
  if (raw && raw !== 'off') return raw;
  if (raw === 'off') return PCV.uxlib.el('span', { class: 'color-muted' }, 'OFF');

  var source = _vmNetSource(nic);
  var meta = netMap[source];
  if (meta && meta.dhcp && meta.ip_cidr)
    return _vmStripCidr(meta.ip_cidr);
  return '-';
}

function _vmRenderNicDetails(nics, networks, v) {
  var el = PCV.uxlib.el;
  var netMap = _vmNetworkMap(networks);
  if (!Array.isArray(nics) || nics.length === 0) {
    var count = v && v.network_count ? String(v.network_count) : '0';
    return el('div', { class: 'color-muted text-xs', style: 'margin-top:8px' },
      count === '0' ? _L('할당된 NIC 없음', 'No assigned NICs')
                    : _L('NIC 상세 조회 불가', 'NIC details unavailable'));
  }

  var wrap = el('div', { style: 'margin-top:8px;border-top:1px solid var(--border);padding-top:6px' });
  nics.forEach(function(nic, idx) {
    var source = _vmNetSource(nic);
    var ip = nic.ip || '';
    var model = nic.model || 'virtio';
    var mac = nic.mac || '-';
    var target = nic.target ? ' / ' + nic.target : '';
    wrap.appendChild(el('div', { style: 'padding:5px 0;border-bottom:1px solid rgba(255,255,255,.06)' },
      HN.row('NIC ' + (idx + 1), [el('span', { class: 'color-accent' }, source), ' ', el('span', { class: 'color-muted text-xs' }, model + target)]),
      HN.row('MAC', el('span', { class: 'text-xs' }, mac)),
      HN.row('IP', ip ? el('span', { class: 'color-green' }, ip) : el('span', { class: 'color-muted' }, '-')),
      HN.row('DNS', _vmNicDns(nic, netMap))));
  });
  return wrap;
}

function _vmPrimaryNicValue(nics, field) {
  if (!Array.isArray(nics)) return '';
  for (var i = 0; i < nics.length; i++) {
    if (nics[i] && nics[i][field]) return nics[i][field];
  }
  return '';
}

function _vmProgressBar(p, c) {
  var el = PCV.uxlib.el;
  var cl = p > 85 ? 'var(--red)' : p > 60 ? 'var(--yellow)' : 'var(--green)';
  var anim = p > 85 ? ' pulse-anim' : '';
  return el('div', { class: 'pb' + anim },
    el('div', { class: 'pb-f scan-anim', style: 'width:' + p + '%;background:' + (c || cl) }),
    el('div', { class: 'pb-t' }, p.toFixed(1) + '%'));
}

async function renderSummary(b, v) {
  if (!v) { PCV.uxlib.clearEl(b); b.appendChild(PCV.uxlib.el('p', { class: 'color-muted' }, t('vm.select'))); return; }
  const on = v.state === 'running';

  var metrics = {};
  var nics = [];
  var networks = [];
  var summaryReqs = [
    on ? fetchGet(EP.VM_DETAIL(v.name)) : Promise.resolve({}),
    fetchGet(EP.VM_NICS(v.name)),
    fetchGet(EP.NET_LIST())
  ];
  var summaryResults = await Promise.allSettled(summaryReqs);
  if (summaryResults[0].status === 'fulfilled')
    metrics = unwrapData(summaryResults[0].value) || summaryResults[0].value || {};
  if (summaryResults[1].status === 'fulfilled')
    nics = unwrapList(summaryResults[1].value);
  if (summaryResults[2].status === 'fulfilled')
    networks = unwrapList(summaryResults[2].value);

  var cpuPct = metrics.cpu || v.live_cpu_pct || 0;
  var memPct = metrics.mem || v.mem_percent || 0;
  var vcpu = metrics.vcpu || v.vcpu || '-';
  var memMb = metrics.memory_mb || v.memory_mb || '-';
  var diskRd = metrics.disk_rd || v.disk_rd || 0;
  var diskWr = metrics.disk_wr || v.disk_wr || 0;
  var netRx = metrics.net_rx || v.net_rx || 0;
  var netTx = metrics.net_tx || v.net_tx || 0;
  var primaryIp = _vmPrimaryNicValue(nics, 'ip') || v.ip || '-';
  var primaryDns = nics.length ? _vmNicDns(nics[0], _vmNetworkMap(networks)) : '-';
  var diskUsageAction = on
    ? PCV.uxlib.el('button', { class: 'btn btn-xs', onclick: 'showVmDiskUsage()' }, '📊 ' + _L('디스크 사용량', 'Disk Usage'))
    : PCV.uxlib.el('span', { class: 'color-muted text-xs' }, _L('실행 중인 VM에서 확인 가능', 'Available while running'));

  v.live_cpu_pct = cpuPct;
  v.mem_percent = memPct;
  v.vcpu = vcpu;
  v.memory_mb = memMb;
  v.ip = primaryIp;

  const cpuHi = cpuPct > 85;
  var el = PCV.uxlib.el;
  var header = el('div', { class: 'flex gap-10 items-center mb-14' },
    el('span', { class: 'neon-blink color-accent' }, '>>'),
    el('h2', { style: 'font-family:var(--font-display);font-size:16px;letter-spacing:.05em' }, v.name),
    HN.badge(v.state + (cpuHi ? ' [HIGH_LOAD]' : ''), on ? 'g' : 'r'));
  var sg = el('div', { class: 'sg' },
    HN.card('💻 System', [
      HN.row('Guest OS', 'Linux (KVM)'),
      HN.row('UUID', el('span', { class: 'text-xs' }, v.uuid || '-')),
      HN.row(_L('부트', 'Boot'), (v.boot_mode || 'bios').toUpperCase()),
      HN.row(_L('자동시작', 'Auto Start'), v.auto_start ? el('span', { class: 'color-green' }, 'ON') : el('span', { class: 'color-muted' }, 'OFF'))
    ]),
    HN.card('⚙ CPU', [
      HN.row('vCPU', String(vcpu)),
      HN.row(_L('사용률', 'Usage'), el('span', { class: cpuHi ? 'color-red' : 'color-green' }, cpuPct.toFixed(1) + '%')),
      _vmProgressBar(cpuPct)
    ]),
    HN.card('📌 ' + _L('메모리', 'Memory'), [
      HN.row(_L('할당', 'Allocated'), String(memMb) + ' MB'),
      HN.row(_L('사용률', 'Usage'), memPct.toFixed(1) + '%'),
      _vmProgressBar(memPct)
    ]),
    HN.card('💾 ' + _L('스토리지', 'Storage'), [
      HN.row(_L('타입', 'Type'), HN.badge(escapeHtml(v.storage_type || '-'), v.storage_type === 'zvol' ? 'g' : 'y')),
      HN.row(_L('포맷', 'Format'), v.disk_format || '-'),
      HN.row(_L('경로', 'Path'), el('span', { class: 'text-xs' }, v.disk_path || '-')),
      HN.row(_L('게스트 사용량', 'Guest Usage'), diskUsageAction),
      HN.row(_L('스냅샷', 'Snapshots'), String(v.snapshot_count || 0)),
      HN.row('NIC', String(v.network_count || 0))
    ]),
    HN.card('💾 Disk I/O', [
      HN.row(_L('읽기', 'Read'), el('span', { class: 'color-cyan' }, formatBytes(diskRd))),
      HN.row(_L('쓰기', 'Write'), el('span', { class: 'color-peach' }, formatBytes(diskWr))),
      HN.row('IOPS R', el('span', { class: 'color-cyan' }, (metrics.disk_rd_req || 0).toLocaleString())),
      HN.row('IOPS W', el('span', { class: 'color-peach' }, (metrics.disk_wr_req || 0).toLocaleString()))
    ]),
    el('div', { class: 'hc glitch-panel' },
      el('h4', null, '🌐 ' + _L('네트워크', 'Network')),
      HN.row('RX', el('span', { class: 'color-yellow' }, formatBytes(netRx))),
      HN.row('TX', el('span', { class: 'color-yellow' }, formatBytes(netTx))),
      HN.row('IP', primaryIp && primaryIp !== '-' ? el('span', { class: 'color-green' }, primaryIp) : el('span', { class: 'color-muted' }, '-')),
      HN.row('DNS', primaryDns),
      HN.row('RX pps', el('span', { class: 'color-muted' }, (metrics.net_rx_pkts || 0).toLocaleString())),
      HN.row('TX pps', el('span', { class: 'color-muted' }, (metrics.net_tx_pkts || 0).toLocaleString())),
      _vmRenderNicDetails(nics, networks, v)),
    el('div', { class: 'hc', style: 'grid-column:1/-1' },
      el('h4', null, _L('작업', 'Actions')),
      el('div', { class: 'flex gap-4 flex-wrap mb-8' },
        el('button', { class: 'btn btn-g', onclick: "vmPower('start')" }, '▶ ' + t('power.start')),
        el('button', { class: 'btn', onclick: "vmPower('suspend')" }, '❚❚ ' + t('power.pause')),
        el('button', { class: 'btn', onclick: "vmPower('resume')" }, '▶▶ ' + t('power.resume')),
        el('button', { class: 'btn btn-r', onclick: "vmPower('stop')" }, '■ ' + t('power.stop'))),
      el('div', { style: 'display:grid;grid-template-columns:repeat(auto-fill,minmax(100px,1fr));gap:4px' },
        el('button', { class: 'btn', onclick: 'showSnap()' }, t('vm.snapshot')),
        el('button', { class: 'btn', onclick: 'showSettings()' }, t('vm.settings')),
        el('button', { class: 'btn', onclick: 'showRenameVm()' }, '✎ ' + _L('이름', 'Rename')),
        el('button', { class: 'btn', onclick: 'showNicMgr()' }, 'NIC'),
        el('button', { class: 'btn', onclick: 'vmClone(' + selectedVmIndex + ')' }, '📋 Clone'),
        el('button', { class: 'btn', onclick: 'vmExportOva(' + selectedVmIndex + ')' }, '📦 Export'),
        el('button', { class: 'btn', onclick: 'showImportOva()' }, '📥 Import'),
        el('button', { class: 'btn', onclick: 'showMemStats()' }, '📌 Mem'),
        el('button', { class: 'btn', onclick: 'showCpuStats()' }, '⚙ CPU'),
        el('button', { class: 'btn', onclick: 'showVmDiskUsage()' }, '📊 ' + _L('디스크 사용량', 'Disk Usage')),
        el('button', { class: 'btn', onclick: 'showDiskLiveResize()' }, '💾 Disk'),
        el('button', { class: 'btn', onclick: 'showBlkioEditor()' }, '⚙ I/O'),
        el('button', { class: 'btn', onclick: 'showGuestAgent()' }, '💬 Agent'))));
  PCV.uxlib.clearEl(b);
  b.appendChild(header);
  b.appendChild(sg);
}

PCV.vm = Object.assign(PCV.vm || {}, {
  render: render,
  setSort: setSort,
  getFiltered: getFiltered,
  toggleVmView: toggleVmView,
  renderSummary: renderSummary,
});

window.render = render;
window.setSort = setSort;
window.getFiltered = getFiltered;
window.toggleChk = toggleChk;
window.bulkStop = bulkStop;
window.showCtx = showCtx;
window.renderSummary = renderSummary;
window.toggleVmView = toggleVmView;

})(window.PCV);
