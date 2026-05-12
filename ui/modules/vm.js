




































































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
    document.getElementById('vl').innerHTML = emptyStatePro({
      icon: '&#128187;',
      title: _L('VM이 없습니다', 'No virtual machines'),
      desc: _L('첫 VM을 만들어 시작하세요. 몇 초 안에 부팅 가능합니다.', 'Create your first VM. Boots in seconds.'),
      ctaLabel: _L('+ VM 만들기', '+ Create VM'),
      ctaAction: 'showCreate()'
    });
    return;
  }
  let h = '';
  h += '<div class="flex gap-4 mb-8 justify-end">';
  h += '<button class="btn ' + (vmViewMode === 'list' ? 'btn-g' : '') + ' btn-xs" onclick="toggleVmView()">&#9776; ' + _L('목록', 'List') + '</button>';
  h += '<button class="btn ' + (vmViewMode === 'card' ? 'btn-g' : '') + ' btn-xs" onclick="toggleVmView()">&#9638; ' + _L('카드', 'Card') + '</button>';
  h += '<button class="btn" onclick="showVmCompare()" class="btn-xs">' + _L('비교', 'Compare') + '</button>';
  h += '<button class="btn" onclick="showBulkActions()" class="btn-xs" data-role="OPERATOR,ADMIN">' + _L('일괄 작업', 'Bulk') + '</button>';
  h += '</div>';
  if (vmViewMode === 'card') {
    h += '<div class="sg grid-3">';
    l.forEach(function(v, ri) {
      var on = v.state === 'running';
      var cp = v.live_cpu_pct || 0;
      var mp = v.mem_percent || 0;
      h += '<div class="hc" draggable="true" ondragstart="event.dataTransfer.setData(\'text/plain\',\'' + esc(v.name) + '\')" style="cursor:grab;border-left:3px solid ' + (on ? 'var(--green)' : 'var(--red)') + '" onclick="selectedVmIndex=' + vmList.indexOf(v) + ';currentTab=\'summary\';switchSbTab(\'vms\');render()">';
      h += '<div class="flex items-center gap-6 mb-6"><span style="font-size:8px;color:' + (on ? 'var(--green)' : 'var(--red)') + '">&#9679;</span><b>' + esc(v.name) + '</b></div>';
      h += '<div class="flex gap-8 text-11">';
      h += '<div class="flex-1"><div class="color-muted">CPU</div>' + renderProgressBar(cp) + '</div>';
      h += '<div class="flex-1"><div class="color-muted">MEM</div>' + renderProgressBar(mp) + '</div>';
      h += '</div>';
      h += '<div class="flex gap-8 mt-6 text-xs color-muted">';
      h += '<span>' + (v.vcpu || '?') + ' vCPU</span>';
      h += '<span>' + (v.memory_mb || '?') + ' MB</span>';
      h += '<span>' + H.badge(v.state || '?', on ? 'g' : 'r') + '</span>';
      h += '</div></div>';
    });
    h += '</div>';

    h += '<h3 style="margin:16px 0 8px">' + _L('마이그레이션 대상 노드', 'Migration Target Nodes') + '</h3>';
    h += '<div class="sg grid-3">';
    var nodes = (typeof MON_NODES !== 'undefined' && MON_NODES) ? MON_NODES : [{name:'Node1',ip:'localhost'}];
    nodes.forEach(function(nd) {
      h += '<div class="hc" style="text-align:center;padding:20px;border:2px dashed var(--border);transition:border-color 0.2s" '
        + 'ondragover="event.preventDefault();this.style.borderColor=\'var(--accent)\'" '
        + 'ondragleave="this.style.borderColor=\'var(--border)\'" '
        + 'ondrop="event.preventDefault();this.style.borderColor=\'var(--border)\';vmMigrateDrop(event.dataTransfer.getData(\'text/plain\'),\'' + esc(nd.ip) + '\',\'' + esc(nd.name) + '\')">'
        + '<div style="font-size:24px;margin-bottom:6px">&#128421;</div>'
        + '<div class="text-13 font-600">' + esc(nd.name) + '</div>'
        + '<div class="color-muted text-xs">' + esc(nd.ip) + '</div>'
        + '</div>';
    });
    h += '</div>';
  } else {
    l.forEach((v, i) => {
      const ri = vmList.indexOf(v);
      const on = v.state === 'running';
      const cp = v.live_cpu_pct || 0;
      const c = cp > 85 ? 'var(--red)' : cp > 60 ? 'var(--yellow)' : 'var(--green)';
      const star = favs.includes(v.name) ? '&#9733;' : '&#9734;';
      h += `<div class="vi ${ri === selectedVmIndex ? 'active' : ''}" onclick="selectedVmIndex=${ri};currentTab=localStorage.getItem('pcv-last-vm-tab')||'summary';switchSbTab('vms');document.querySelectorAll('#ct button').forEach(b=>b.classList.toggle('active',b.dataset.t==='summary'));render()" oncontextmenu="showCtx(event,${ri})"><input type="checkbox" ${checkedVms.has(ri) ? 'checked' : ''} onclick="event.stopPropagation();toggleChk(${ri})"><span class="fav-star" onclick="event.stopPropagation();toggleFavorite('${escapeAttr(v.name)}')" title="Favorite">${star}</span><span class="dot ${on ? 'on' : 'off'}"></span><span class="nm">${escapeHtml(v.name)}</span><span class="mini-bar"><span class="mini-fill pcv-bar-fill-inline" style="--bw:${cp}%;--bc:${c}"></span></span><span class="st">${cp.toFixed(0)}%</span><canvas class="vm-spark" id="spark-${esc(v.name)}" width="40" height="14" style="vertical-align:middle;margin-left:4px"></canvas></div>`;
    });
  }
  document.getElementById('vl').innerHTML = h;

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
  for (const i of checkedVms) {
    await fetchPost(EP.VM_STOP(vmList[i].name), {});
    addEvt('VM Bulk stop — ' + vmList[i].name);
  }
  checkedVms.clear();
  setTimeout(loadAll, 1500);
}


document.addEventListener('keydown', function(e) {
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
  m.innerHTML = `<div class="ci" onclick="vmPower('start')">&#9654; ${t('power.start')}</div><div class="ci" onclick="vmPower('stop')">&#9632; ${t('power.stop')}</div><div class="sep"></div><div class="ci" onclick="showSnap()">&#128247; ${t('vm.snapshot')}</div><div class="ci" onclick="showSettings()">&#9881; ${t('vm.settings')}</div><div class="ci" onclick="showRenameVm()">&#9998; ${_L('이름 변경', 'Rename')}</div><div class="ci" onclick="showVnc()">&#128424; VNC</div><div class="sep"></div><div class="ci" onclick="showMemStats()">&#128204; Memory Stats</div><div class="ci" onclick="showCpuStats()">&#9881; CPU Stats</div><div class="ci" onclick="showDiskLiveResize()">&#128190; Disk Resize</div><div class="ci" onclick="showGuestAgent()">&#128172; Guest Agent</div><div class="sep"></div><div class="ci" onclick="showNicMgr()">&#127760; NIC</div><div class="ci" onclick="vmClone(${ri})">&#128203; Clone</div><div class="ci" onclick="vmExportOva(${ri})">&#128230; Export OVA</div><div class="sep"></div><div class="ci" onclick="vmDel()">&#10060; ${t('btn.delete')}</div>`;
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
  if (raw && raw !== 'off') return escapeHtml(raw);
  if (raw === 'off') return '<span class="color-muted">OFF</span>';

  var source = _vmNetSource(nic);
  var meta = netMap[source];
  if (meta && meta.dhcp && meta.ip_cidr)
    return escapeHtml(_vmStripCidr(meta.ip_cidr));
  return '-';
}

function _vmRenderNicDetails(nics, networks, v) {
  var netMap = _vmNetworkMap(networks);
  if (!Array.isArray(nics) || nics.length === 0) {
    var count = v && v.network_count ? String(v.network_count) : '0';
    return '<div class="color-muted text-xs" style="margin-top:8px">' +
      (count === '0' ? _L('할당된 NIC 없음', 'No assigned NICs')
                     : _L('NIC 상세 조회 불가', 'NIC details unavailable')) +
      '</div>';
  }

  var h = '<div style="margin-top:8px;border-top:1px solid var(--border);padding-top:6px">';
  nics.forEach(function(nic, idx) {
    var source = _vmNetSource(nic);
    var ip = nic.ip || '';
    var model = nic.model || 'virtio';
    var mac = nic.mac || '-';
    var target = nic.target ? ' / ' + nic.target : '';
    h += '<div style="padding:5px 0;border-bottom:1px solid rgba(255,255,255,.06)">';
    h += H.row('NIC ' + (idx + 1), '<span class="color-accent">' + escapeHtml(source) + '</span> <span class="color-muted text-xs">' + escapeHtml(model + target) + '</span>');
    h += H.row('MAC', '<span class="text-xs">' + escapeHtml(mac) + '</span>');
    h += H.row('IP', ip ? '<span class="color-green">' + escapeHtml(ip) + '</span>' : '<span class="color-muted">-</span>');
    h += H.row('DNS', _vmNicDns(nic, netMap));
    h += '</div>';
  });
  return h + '</div>';
}

function _vmPrimaryNicValue(nics, field) {
  if (!Array.isArray(nics)) return '';
  for (var i = 0; i < nics.length; i++) {
    if (nics[i] && nics[i][field]) return nics[i][field];
  }
  return '';
}


async function renderSummary(b, v) {
  if (!v) { b.innerHTML = '<p class="color-muted">' + t('vm.select') + '</p>'; return; }
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
    ? '<button class="btn btn-xs" onclick="showVmDiskUsage()">&#128202; ' + _L('디스크 사용량', 'Disk Usage') + '</button>'
    : '<span class="color-muted text-xs">' + _L('실행 중인 VM에서 확인 가능', 'Available while running') + '</span>';


  v.live_cpu_pct = cpuPct;
  v.mem_percent = memPct;
  v.vcpu = vcpu;
  v.memory_mb = memMb;
  v.ip = primaryIp;

  const cpuHi = cpuPct > 85;
  b.innerHTML = '<div class="flex gap-10 items-center mb-14"><span class="neon-blink color-accent">&gt;&gt;</span><h2 style="font-family:var(--font-display);font-size:16px;letter-spacing:.05em">' + escapeHtml(v.name) + '</h2>' + H.badge(v.state + (cpuHi ? ' [HIGH_LOAD]' : ''), on ? 'g' : 'r') + '</div>'
+ '<div class="sg">'
+ H.card('&#128187; System', H.row('Guest OS', 'Linux (KVM)') + H.row('UUID', '<span class="text-xs">' + escapeHtml(v.uuid || '-') + '</span>') + H.row(_L('부트', 'Boot'), escapeHtml((v.boot_mode || 'bios').toUpperCase())) + H.row(_L('자동시작', 'Auto Start'), v.auto_start ? '<span class="color-green">ON</span>' : '<span class="color-muted">OFF</span>'))
+ H.card('&#9881; CPU', H.row('vCPU', escapeHtml(String(vcpu))) + H.row(_L('사용률', 'Usage'), '<span class="' + (cpuHi ? 'color-red' : 'color-green') + '">' + cpuPct.toFixed(1) + '%</span>') + renderProgressBar(cpuPct))
+ H.card('&#128204; ' + _L('메모리', 'Memory'), H.row(_L('할당', 'Allocated'), escapeHtml(String(memMb)) + ' MB') + H.row(_L('사용률', 'Usage'), memPct.toFixed(1) + '%') + renderProgressBar(memPct))
+ H.card('&#128190; ' + _L('스토리지', 'Storage'), H.row(_L('타입', 'Type'), H.badge(escapeHtml(v.storage_type || '-'), v.storage_type === 'zvol' ? 'g' : 'y')) + H.row(_L('포맷', 'Format'), escapeHtml(v.disk_format || '-')) + H.row(_L('경로', 'Path'), '<span class="text-xs">' + escapeHtml(v.disk_path || '-') + '</span>') + H.row(_L('게스트 사용량', 'Guest Usage'), diskUsageAction) + H.row(_L('스냅샷', 'Snapshots'), escapeHtml(String(v.snapshot_count || 0))) + H.row('NIC', escapeHtml(String(v.network_count || 0))))
+ H.card('&#128190; Disk I/O', H.row(_L('읽기', 'Read'), '<span class="color-cyan">' + formatBytes(diskRd) + '</span>') + H.row(_L('쓰기', 'Write'), '<span class="color-peach">' + formatBytes(diskWr) + '</span>') + H.row('IOPS R', '<span class="color-cyan">' + (metrics.disk_rd_req || 0).toLocaleString() + '</span>') + H.row('IOPS W', '<span class="color-peach">' + (metrics.disk_wr_req || 0).toLocaleString() + '</span>'))
+ '<div class="hc glitch-panel"><h4>&#127760; ' + _L('네트워크', 'Network') + '</h4>' + H.row('RX', '<span class="color-yellow">' + formatBytes(netRx) + '</span>') + H.row('TX', '<span class="color-yellow">' + formatBytes(netTx) + '</span>') + H.row('IP', primaryIp && primaryIp !== '-' ? '<span class="color-green">' + escapeHtml(primaryIp) + '</span>' : '<span class="color-muted">-</span>') + H.row('DNS', primaryDns) + H.row('RX pps', '<span class="color-muted">' + (metrics.net_rx_pkts || 0).toLocaleString() + '</span>') + H.row('TX pps', '<span class="color-muted">' + (metrics.net_tx_pkts || 0).toLocaleString() + '</span>') + _vmRenderNicDetails(nics, networks, v) + '</div>'
+ '<div class="hc" style="grid-column:1/-1"><h4>' + _L('작업', 'Actions') + '</h4>'
+ '<div class="flex gap-4 flex-wrap mb-8"><button class="btn btn-g" onclick="vmPower(\'start\')">&#9654; ' + t('power.start') + '</button><button class="btn" onclick="vmPower(\'suspend\')">&#10074;&#10074; ' + t('power.pause') + '</button><button class="btn" onclick="vmPower(\'resume\')">&#9654;&#9654; ' + t('power.resume') + '</button><button class="btn btn-r" onclick="vmPower(\'stop\')">&#9632; ' + t('power.stop') + '</button></div>'
+ '<div style="display:grid;grid-template-columns:repeat(auto-fill,minmax(100px,1fr));gap:4px"><button class="btn" onclick="showSnap()">' + t('vm.snapshot') + '</button><button class="btn" onclick="showSettings()">' + t('vm.settings') + '</button><button class="btn" onclick="showRenameVm()">&#9998; ' + _L('이름', 'Rename') + '</button><button class="btn" onclick="showNicMgr()">NIC</button><button class="btn" onclick="vmClone(' + selectedVmIndex + ')">&#128203; Clone</button><button class="btn" onclick="vmExportOva(' + selectedVmIndex + ')">&#128230; Export</button><button class="btn" onclick="showImportOva()">&#128229; Import</button><button class="btn" onclick="showMemStats()">&#128204; Mem</button><button class="btn" onclick="showCpuStats()">&#9881; CPU</button><button class="btn" onclick="showVmDiskUsage()">&#128202; ' + _L('디스크 사용량', 'Disk Usage') + '</button><button class="btn" onclick="showDiskLiveResize()">&#128190; Disk</button><button class="btn" onclick="showBlkioEditor()">&#9881; I/O</button><button class="btn" onclick="showGuestAgent()">&#128172; Agent</button></div>'
+ '</div>'
+ '</div>';
}









async function renderConsole(b, v) {
  if (!v) return;
  let vncHtml = '<div class="text-center p-20"><p class="text-14">&#128424; ' + escapeHtml(v.name) + '</p><p class="stat-label mt-8">' + t('loading') + '</p></div>';
  b.innerHTML = '<div style="background:#000;border:1px solid var(--border);border-radius:var(--r);min-height:500px;height:calc(100vh - 200px);position:relative" id="vnc-frame">' + vncHtml + '</div>';
  try {
    const r = await fetchGet(EP.VNC(v.name));
    const d = unwrapData(r);
    const addr = d.vnc_address || d.address || 'localhost';
    const port = d.vnc_port || d.port || '';
    if (port && v.state === 'running') {
      document.getElementById('vnc-frame').innerHTML = `<div class="p-12"><div class="flex gap-12 items-center mb-12 flex-wrap">${H.badge('VNC ' + t('vnc.connected'), 'g')}<span class="text-13 font-600">${escapeHtml(addr)}:${escapeHtml(String(port))}</span><button class="btn btn-g" onclick="openNoVNCPopup('${escapeAttr(addr)}','${escapeAttr(String(port))}','${escapeAttr(v.name)}')">&#128424; ${t('vnc.open_popup')}</button><button class="btn" onclick="openNoVNC('${escapeAttr(addr)}','${escapeAttr(String(port))}')">${t('vnc.embedded')}</button><button class="btn" onclick="copyVncAddr('${escapeAttr(String(port))}')">&#128203; ${t('vnc.copy_addr')}</button></div><div id="vnc-placeholder" style="background:#111;height:calc(100vh - 280px);min-height:400px;border-radius:var(--r);display:flex;align-items:center;justify-content:center;color:var(--fg2)"><div class="text-center"><p class="text-lg">&#128424; ${escapeHtml(v.name)}</p><p class="mt-8">"${t('vnc.open_popup')}"</p><p class="stat-label mt-4">VNC: ${location.hostname}:${escapeHtml(String(port))}</p></div></div></div>`;
    } else {
      document.getElementById('vnc-frame').innerHTML = `<div class="text-center color-muted p-20"><p class="text-14">&#128424; ${escapeHtml(v.name)}</p><p class="mt-8">${v.state === 'running' ? _L('VNC 포트를 사용할 수 없습니다', 'VNC port not available') : _L('VM이 중지 상태입니다', 'VM is stopped')}</p><button class="btn mt-12" onclick="showVnc()">${_L('VNC 확인', 'Check VNC')}</button></div>`;
    }
  } catch (e) {
    document.getElementById('vnc-frame').innerHTML = '<div class="text-center color-muted p-20"><p>' + escapeHtml(t('vnc.unavailable')) + '</p><button class="btn mt-8" onclick="showVnc()">' + escapeHtml(t('vnc.manual_check')) + '</button></div>';
  }
}

function openNoVNC(addr, port) {
  const frame = document.getElementById('vnc-frame');
  if (!frame) return;
  const wsProto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const wsUrl = wsProto + '//' + location.host + '/api/v1/ws/vnc?port=' + port;
  const fullscreenText = t('vnc.fullscreen');
  const fitText = t('vnc.fit');
  const loadingConnectingText = t('vnc.loading_connecting');
  const connectedText = t('vnc.connected');
  const disconnectedText = t('vnc.disconnected');
  const remoteText = t('vnc.remote');
  const errorSuffixText = t('vnc.error_suffix');
  const securityFailureText = t('vnc.security_failure');
  const statusEndpoint = escapeHtml(addr) + ':' + escapeHtml(String(port));
  frame.innerHTML = '<div class="flex gap-6 mb-6 items-center"><button class="btn" onclick="vncFullscreen()" title="' + escapeAttr(fullscreenText) + '">&#9974; ' + escapeHtml(fullscreenText) + '</button><button class="btn" onclick="vncFitWindow()" title="' + escapeAttr(fitText) + '">&#128300; ' + escapeHtml(fitText) + '</button><span id="vnc-res" class="stat-label"></span></div><div id="vnc-container" style="width:100%;height:calc(100vh - 220px);min-height:500px;background:#000;border-radius:var(--r);position:relative"><div id="vnc-status" style="position:absolute;top:8px;left:8px;z-index:10;font-size:11px;color:var(--green);background:rgba(0,0,0,.7);padding:4px 10px;border-radius:4px"><span class="spinner"></span> ' + escapeHtml(loadingConnectingText) + ' ' + statusEndpoint + '...</div>' + vncIsoEjectTipHtml() + '</div>';
  const existing = document.getElementById('novnc-loader');
  if (existing) existing.remove();
  const m = document.createElement('script');
  m.id = 'novnc-loader'; m.type = 'module';
  m.textContent = 'import _mod from "/ui/vendor/novnc/novnc.esm.js";\n'
  + 'const RFB=_mod.default||_mod;\n'
  + 'const wsUrl=' + JSON.stringify(wsUrl) + ';\n'
  + 'const statusEndpoint=' + JSON.stringify(statusEndpoint) + ';\n'
  + 'const connectedText=' + JSON.stringify(escapeHtml(connectedText)) + ';\n'
  + 'const disconnectedText=' + JSON.stringify(escapeHtml(disconnectedText)) + ';\n'
  + 'const errorSuffixText=' + JSON.stringify(escapeHtml(errorSuffixText)) + ';\n'
  + 'const securityFailureText=' + JSON.stringify(escapeHtml(securityFailureText)) + ';\n'
  + 'const remoteText=' + JSON.stringify(remoteText) + ';\n'
  + 'try{\n'
  + 'const container=document.getElementById("vnc-container");\n'
  + 'const st=document.getElementById("vnc-status");\n'
  + 'function setStatusMark(color,text){if(!st)return;const mark=document.createElement("span");mark.style.color=color;mark.textContent="\\u25cf";st.replaceChildren(mark," ",text);}\n'
  + 'if(!container){console.error("no vnc-container");}\n'
  + 'if(typeof RFB!=="function"){throw new Error("RFB loaded as "+typeof RFB+", keys: "+Object.keys(_mod).join(","));}\n'
  + 'const rfb=new RFB(container,wsUrl);\n'
  + 'rfb.scaleViewport=true;rfb.resizeSession=true;rfb.clipViewport=false;rfb.qualityLevel=6;rfb.compressionLevel=2;\n'
  + 'rfb.addEventListener("connect",()=>{setStatusMark("lime",connectedText+": "+statusEndpoint);const ri=document.getElementById("vnc-res");if(ri)ri.textContent=remoteText+": "+rfb._fbWidth+"x"+rfb._fbHeight;});\n'
  + 'rfb.addEventListener("disconnect",(e)=>{setStatusMark("red",disconnectedText+(e.detail.clean?"":" "+errorSuffixText));});\n'
  + 'rfb.addEventListener("securityfailure",()=>{setStatusMark("red",securityFailureText);});\n'
  + 'window._vncRfb=rfb;\n'
  + '}catch(e){const st=document.getElementById("vnc-status");if(st)st.textContent="\\u25cf "+e.message;console.error("noVNC:",e)}\n';
  document.head.appendChild(m);
}

function vncIsoEjectTipHtml() {
  return '<div id="vnc-iso-tip" style="position:absolute;top:10px;right:10px;z-index:11;max-width:340px;background:rgba(8,12,18,.82);border:1px solid rgba(255,255,255,.18);border-radius:6px;padding:8px 10px;color:#e8f6ff;box-shadow:0 8px 28px rgba(0,0,0,.32);font-family:var(--font-ui,system-ui,sans-serif);pointer-events:none">'
    + '<div style="font-size:11px;font-weight:700;color:#80eaff;margin-bottom:3px">' + escapeHtml(t('vnc.iso_eject_title')) + '</div>'
    + '<div style="font-size:11px;line-height:1.45;color:#d9e7ef">' + escapeHtml(t('vnc.iso_eject_body')) + '</div>'
    + '</div>';
}

function vncFullscreen() {
  const el = document.getElementById('vnc-container');
  if (!el) return;
  if (document.fullscreenElement) { document.exitFullscreen(); }
  else { el.requestFullscreen().catch(() => {}); el.style.height = '100vh'; }
}

function vncFitWindow() {
  const rfb = window._vncRfb;
  if (!rfb) return;
  rfb.scaleViewport = true;
  rfb.resizeSession = true;
  const c = document.getElementById('vnc-container');
  if (c) { c.style.height = 'calc(100vh - 220px)'; }
}

function openNoVNCPopup(addr, port, vmName) {
  const wsProto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const wsUrl = wsProto + '//' + location.host + '/api/v1/ws/vnc?port=' + port;
  const w = window.open('', 'vnc_' + port, 'width=1060,height=820,menubar=no,toolbar=no,location=no,status=no,resizable=yes');
  if (!w) { toast(t('vnc.popup_blocked'), false); return; }
  const safeVmName = escapeHtml(vmName);
  const safeAddr = escapeHtml(addr);
  const safePort = escapeHtml(String(port));
  const loadingText = t('loading');
  const reconnectText = t('vnc.reconnect');
  const connectedText = t('vnc.connected');
  const disconnectedText = t('vnc.disconnected');
  const connectingText = t('vnc.connecting');
  const errorSuffixText = t('vnc.error_suffix');
  const securityFailureText = t('vnc.security_failure');
  const isoTipTitle = t('vnc.iso_eject_title');
  const isoTipBody = t('vnc.iso_eject_body');
  w.document.write(`<!DOCTYPE html><html><head><meta charset="UTF-8"><title>VNC: ${escapeHtml(vmName)} (${escapeHtml(addr)}:${escapeHtml(String(port))})</title>
<style>*{margin:0;padding:0;box-sizing:border-box}body{background:#000;overflow:hidden;font-family:monospace}
#bar{background:#0a0a14;color:#00f0ff;padding:6px 12px;font-size:12px;display:flex;align-items:center;gap:12px;border-bottom:1px solid #1a1a3a}
#bar button{background:none;border:1px solid #00f0ff;color:#00f0ff;padding:4px 10px;border-radius:4px;cursor:pointer;font-size:11px}
#bar button:hover{background:#00f0ff;color:#000}
#st{font-size:11px;color:#5a6a8a}
#vc{width:100%;height:calc(100vh - 36px);background:#000}
#install-tip{position:fixed;top:46px;right:12px;z-index:20;max-width:340px;background:rgba(8,12,18,.86);border:1px solid rgba(0,240,255,.35);border-radius:6px;padding:8px 10px;color:#e8f6ff;box-shadow:0 8px 28px rgba(0,0,0,.36);pointer-events:none}
#install-tip .tip-title{font-size:11px;font-weight:700;color:#80eaff;margin-bottom:3px}
#install-tip .tip-body{font-size:11px;line-height:1.45;color:#d9e7ef}</style></head>
<body><div id="bar"><span style="font-weight:700">${safeVmName}</span><span>${safeAddr}:${safePort}</span>
<button id="vnc-fullscreen" type="button">${t('vnc.fullscreen')}</button>
<button id="vnc-reconnect" type="button">${reconnectText}</button>
<span id="st">${loadingText}</span></div><div id="vc"></div>
<div id="install-tip"><div class="tip-title">${escapeHtml(isoTipTitle)}</div><div class="tip-body">${escapeHtml(isoTipBody)}</div></div>
<script type="module">
	import _mod from "/ui/vendor/novnc/novnc.esm.js";
const RFB=_mod.default||_mod;
const wsUrl=${JSON.stringify(wsUrl)};
const vmTitle=${JSON.stringify('VNC: ' + vmName)};
const connectedText=${JSON.stringify(connectedText)};
const disconnectedText=${JSON.stringify(disconnectedText)};
const connectingText=${JSON.stringify(connectingText)};
const errorSuffixText=${JSON.stringify(errorSuffixText)};
const securityFailureText=${JSON.stringify(securityFailureText)};
const st=document.getElementById("st");
const vc=document.getElementById("vc");
let rfb=null;
let connectSeq=0;
function setStatusText(text){if(st)st.textContent=text}
function setStatusMark(color,text){
  if(!st)return;
  const mark=document.createElement("span");
  mark.style.color=color;
  mark.textContent="\\u25cf";
  st.replaceChildren(mark," ",text);
}
function connectVNC(){
  const seq=++connectSeq;
  if(rfb){try{rfb.disconnect()}catch(e){} rfb=null}
  if(vc)vc.replaceChildren();
  setStatusText(connectingText+"...");
  try{
    const next=new RFB(vc,wsUrl);
    rfb=next;
    next.scaleViewport=true;next.resizeSession=true;next.clipViewport=false;next.qualityLevel=6;next.compressionLevel=2;
    next.addEventListener("connect",()=>{if(seq!==connectSeq)return;setStatusMark("lime",connectedText);document.title=vmTitle});
    next.addEventListener("disconnect",(e)=>{if(seq!==connectSeq)return;setStatusMark("red",disconnectedText+(e.detail.clean?"":" "+errorSuffixText))});
    next.addEventListener("securityfailure",()=>{if(seq!==connectSeq)return;setStatusMark("red",securityFailureText)});
    window._popupRfb=next;
  }catch(e){setStatusText(e.message)}
}
document.getElementById("vnc-fullscreen").addEventListener("click",()=>{vc.requestFullscreen().catch(()=>{})});
document.getElementById("vnc-reconnect").addEventListener("click",connectVNC);
window.addEventListener("beforeunload",()=>{if(rfb){try{rfb.disconnect()}catch(e){}}});
window.addEventListener("resize",()=>{if(rfb)rfb.scaleViewport=true});
connectVNC();
<\/script></body></html>`);
  w.document.close();
  toast(vmName + ' VNC ' + t('vnc.open_popup'));
}

function copyVncAddr(port) {
  const addr = location.hostname + ':' + port;
  navigator.clipboard.writeText(addr).then(() => toast(t('vnc.addr_copied') + ': ' + addr)).catch(() => toast(addr, true));
}









async function renderSnapshots(b, v) {
  if (!v) return;
  b.innerHTML = '<div><div class="justify-between items-center mb-12"><h3>' + t('vm.snapshot') + ': ' + esc(v.name) + '</h3><div class="flex gap-6"><button class="btn btn-g" onclick="takeSnap()" class="text-12">+ ' + t('btn.create') + '</button><button class="btn btn-r" onclick="snapDeleteAll(\'' + escapeAttr(v.name) + '\')" class="text-12">&#128465; Delete All</button></div></div><div id="stree"><span class="spinner"></span> ' + t('loading') + '</div></div>';
  try {
    const r = await fetchGet(EP.VM_SNAPSHOT_LIST(v.name));
    const raw = unwrapList(r);

    const snaps = raw.map(s => {
      if (typeof s === 'string') {
        const [full, time] = s.split('\t');
        const atIdx = full.lastIndexOf('@');
        return { name: atIdx >= 0 ? full.substring(atIdx + 1) : full, full_path: full, time: time || '' };
      }
      return { name: s.name || s, full_path: s.name || s, time: s.creation_time || '' };
    });
    if (!snaps.length) { document.getElementById('stree').innerHTML = '<p class="color-muted">' + t('snap.none') + '</p>'; return; }
    let h = '<table><thead><tr><th>Snapshot</th><th>Created</th><th class="w-140">Actions</th></tr></thead><tbody>';
    snaps.forEach(s => {
      h += '<tr><td><b>' + esc(s.name) + '</b></td>';
      h += '<td class="text-xs color-muted">' + esc(s.time) + '</td>';
      h += '<td class="nowrap">';
      const safeName = v.name.replace(/'/g, "\\'");
      const safeSnap = s.name.replace(/'/g, "\\'");
      h += '<button class="btn" style="font-size:10px;padding:3px 8px;margin-right:4px" onclick="snapRb(\'' + safeName + '\',\'' + safeSnap + '\')">' + t('snap.revert_confirm') + '</button>';
      h += '<button class="btn btn-r" style="font-size:10px;padding:3px 8px" onclick="snapDl(\'' + safeName + '\',\'' + safeSnap + '\')">' + t('btn.delete') + '</button>';
      h += '</td></tr>';
    });
    h += '</tbody></table>';
    document.getElementById('stree').innerHTML = h;
  } catch (e) { document.getElementById('stree').innerHTML = '<p class="color-red">' + esc(e.message) + '</p>'; }
}

async function takeSnap() {
  const v = vmList[selectedVmIndex];
  if (!v) return;
  const on = v.state === 'running';
  const ts = new Date().toISOString().replace(/[-:T]/g, '').substring(0, 14);
  const defaultName = 'snap-' + ts;

  let h = '<h2 class="mb-14">&#128247; Create Snapshot</h2>';
  h += '<div class="mb-14 p-10 border-muted rounded-md">';
  h += '<div class="flex items-center gap-8 mb-8"><span style="font-size:18px">&#128187;</span><div><b>' + esc(v.name) + '</b><div class="text-xs">' + (on ? '<span class="color-green">Running</span>' : '<span class="color-muted">Stopped</span>') + ' &bull; ' + (v.vcpu || '?') + ' vCPU &bull; ' + (v.memory_mb || '?') + ' MB</div></div></div>';
  if (on) h += '<div class="text-xs" style="color:var(--yellow);padding:4px 8px;background:rgba(255,200,0,.08);border-radius:4px">&#9888; VM is running. Snapshot will capture live state (crash-consistent).</div>';
  h += '</div>';

  h += '<div class="mb-12"><label class="text-12 font-600">Snapshot Name</label>';
  h += '<input id="snap-name-input" value="' + defaultName + '" class="w-full mt-4" oninput="snapNameValidate()" placeholder="alphanumeric, dash, underscore"></div>';
  h += '<div id="snap-name-err" style="font-size:11px;min-height:16px;margin-bottom:8px"></div>';

  h += '<div class="mb-14"><label class="text-12 font-600">Description <span class="color-muted">(optional)</span></label>';
  h += '<input id="snap-desc-input" placeholder="e.g. Before upgrade, pre-migration backup" class="w-full mt-4"></div>';

  h += '<div id="snap-preview" style="margin-bottom:14px;padding:10px;border:1px solid var(--border);border-radius:6px;font-size:11px">';
  h += '<div class="color-muted mb-4 font-600">Preview</div>';
  h += '<div>ZFS: <code>' + esc(v.name) + '@' + defaultName + '</code></div>';
  h += '</div>';

  h += '<div class="flex gap-6 justify-end">';
  h += '<button class="btn" onclick="closeModal()">Cancel</button>';
  h += '<button class="btn btn-g" id="snap-create-btn" onclick="snapCreateExec()">&#128247; Create Snapshot</button>';
  h += '</div>';

  showModal(h);
  setTimeout(() => { const el = document.getElementById('snap-name-input'); if (el) { el.focus(); el.select(); } }, 100);
}

function snapNameValidate() {
  const el = document.getElementById('snap-name-input');
  const err = document.getElementById('snap-name-err');
  const preview = document.getElementById('snap-preview');
  const btn = document.getElementById('snap-create-btn');
  if (!el) return;
  const n = el.value.trim();
  const valid = /^[a-zA-Z0-9_-]{1,128}$/.test(n);
  if (err) {
    if (!n) err.innerHTML = '<span class="color-red">Name is required</span>';
    else if (!valid) err.innerHTML = '<span class="color-red">Invalid characters (use a-z, 0-9, dash, underscore)</span>';
    else err.innerHTML = '<span class="color-green">&#9989; Valid</span>';
  }
  if (btn) btn.disabled = !valid || !n;
  const v = vmList[selectedVmIndex];
  if (preview && v) preview.innerHTML = '<div class="color-muted mb-4 font-600">Preview</div><div>ZFS: <code>' + esc(v.name) + '@' + esc(n) + '</code></div>';
}

async function snapCreateExec() {
  const v = vmList[selectedVmIndex];
  if (!v) return;
  const n = (document.getElementById('snap-name-input')?.value || '').trim();
  if (!n || !/^[a-zA-Z0-9_-]{1,128}$/.test(n)) { toast('Invalid snapshot name', false); return; }
  const btn = document.getElementById('snap-create-btn');
  if (btn) { btn.disabled = true; btn.innerHTML = '<span class="spinner"></span> Creating...'; }
  try {
    const r = await fetchPost(EP.VM_SNAPSHOT_CREATE(v.name), { snap_name: n });
    if (r && r.error) { toast('Create failed: ' + (r.error.message || ''), false); if (btn) { btn.disabled = false; btn.innerHTML = '&#128247; Retry'; } return; }
    toast(t('snap.created') + ': ' + n);
    addEvt('VM Snapshot created — ' + v.name + '@' + n);
    closeModal();
    renderSnapshots(document.getElementById('cb'), v);
  } catch (e) { toast('Error: ' + e.message, false); if (btn) { btn.disabled = false; btn.innerHTML = '&#128247; Retry'; } }
}

async function snapRb(vm, s) {
  const v = vmList.find(x => x.name === vm);
  const on = v && v.state === 'running';

  let h = '<h2 class="mb-14">&#9194; Rollback Snapshot</h2>';


  h += '<div style="margin-bottom:14px;padding:12px;border:1px solid var(--red);border-radius:6px;background:rgba(255,60,60,.06)">';
  h += '<div style="font-weight:700;color:var(--red);margin-bottom:6px">&#9888; Destructive Operation</div>';
  h += '<div class="text-xs color-muted">This will revert the VM disk to the snapshot point-in-time. <b>All data written after this snapshot will be permanently lost.</b></div>';
  if (on) h += '<div class="text-xs" style="color:var(--yellow);margin-top:6px">&#9889; VM is currently <b>running</b> — it will be <b>force-stopped</b> before rollback, then automatically restarted.</div>';
  h += '</div>';


  h += '<div class="mb-14 p-10 border-muted rounded-md">';
  h += '<div style="display:grid;grid-template-columns:100px 1fr;gap:4px 12px;font-size:12px">';
  h += '<span class="color-muted">VM</span><span><b>' + esc(vm) + '</b> ' + (on ? '<span class="color-green">Running</span>' : '<span class="color-muted">Stopped</span>') + '</span>';
  h += '<span class="color-muted">Snapshot</span><span><code>' + esc(s) + '</code></span>';
  h += '<span class="color-muted">ZFS Path</span><span class="text-xs"><code>' + esc(vm) + '@' + esc(s) + '</code></span>';
  h += '</div></div>';


  h += '<div class="mb-14"><label class="text-12 font-600">Type VM name to confirm: <code>' + esc(vm) + '</code></label>';
  h += '<input id="rb-confirm-input" placeholder="' + esc(vm) + '" class="w-full mt-4" oninput="rbValidate(\'' + vm.replace(/'/g, "\\'") + '\')"></div>';

  h += '<div class="flex gap-6 justify-end">';
  h += '<button class="btn" onclick="closeModal()">Cancel</button>';
  h += '<button class="btn btn-r" id="rb-exec-btn" disabled onclick="rbExec(\'' + vm.replace(/'/g, "\\'") + '\',\'' + s.replace(/'/g, "\\'") + '\')">&#9194; Rollback</button>';
  h += '</div>';

  showModal(h);
  setTimeout(() => { const el = document.getElementById('rb-confirm-input'); if (el) el.focus(); }, 100);
}

function rbValidate(vm) {
  const input = (document.getElementById('rb-confirm-input')?.value || '').trim();
  const btn = document.getElementById('rb-exec-btn');
  if (btn) btn.disabled = input !== vm;
}

async function rbExec(vm, s) {
  const btn = document.getElementById('rb-exec-btn');
  if (btn) { btn.disabled = true; btn.innerHTML = '<span class="spinner"></span> Rolling back...'; }
  try {
    const r = await fetchPost(EP.VM_SNAPSHOT_ROLLBACK(vm), { snap_name: s });
    if (r && r.error) { toast('Rollback failed: ' + (r.error.message || ''), false); if (btn) { btn.disabled = false; btn.innerHTML = '&#9194; Retry'; } return; }
    toast('Rollback accepted: ' + vm + '@' + s);
    addEvt('VM Snapshot rollback — ' + vm + '@' + s);
    closeModal();
    renderSnapshots(document.getElementById('cb'), vmList[selectedVmIndex]);
  } catch (e) { toast('Error: ' + e.message, false); if (btn) { btn.disabled = false; btn.innerHTML = '&#9194; Retry'; } }
}

async function snapDl(vm, s) {
  if (!await customConfirm('Delete snapshot "' + s + '"?')) return;
  try {
    const r = await fetchDelete(EP.VM_SNAPSHOT_DELETE(vm, s));
    if (r && r.error) { toast('Delete failed: ' + (r.error.message || JSON.stringify(r.error)), false); return; }
    toast(t('snap.deleted') + ': ' + s);
    addEvt('VM Snapshot deleted — ' + vm + '@' + s);
    renderSnapshots(document.getElementById('cb'), vmList[selectedVmIndex]);
  } catch (e) { toast('Delete error: ' + e.message, false); }
}

async function snapDeleteAll(vm) {

  let snaps = [];
  try {
    const r = await fetchGet(EP.VM_SNAPSHOT_LIST(vm));
    const raw = unwrapList(r);
    snaps = raw.map(s => {
      if (typeof s === 'string') {
        const [full, time] = s.split('\t');
        const at = full.lastIndexOf('@');
        return { name: at >= 0 ? full.substring(at + 1) : full, time: time || '' };
      }
      return { name: s.name || s, time: s.creation_time || '' };
    });
  } catch (e) { toast('Failed to load snapshots', false); return; }

  if (snaps.length === 0) { toast('No snapshots to delete'); return; }


  let h = '<h2 class="mb-12">&#128465; Bulk Delete Snapshots</h2>';
  h += '<div class="mb-12"><span class="color-muted">VM:</span> <b>' + esc(vm) + '</b> &mdash; <span class="color-accent">' + snaps.length + '</span> snapshots</div>';

  h += '<div class="mb-14 p-10 border-muted rounded-md">';
  h += '<div style="margin-bottom:8px;font-weight:600;font-size:12px">Options</div>';
  h += '<div class="mb-8"><label class="text-12">Prefix filter <span class="color-muted">(empty = all)</span></label>';
  h += '<input id="sda-prefix" placeholder="e.g. pcv-repl-" class="w-full mt-4" oninput="sdaPreview()"></div>';
  h += '<div><label class="text-12">Keep recent</label>';
  h += '<input id="sda-keep" type="number" min="0" value="0" style="width:80px;margin-left:8px" oninput="sdaPreview()"> <span class="color-muted text-xs">snapshots</span></div>';
  h += '</div>';

  h += '<div id="sda-preview" style="margin-bottom:14px;padding:10px;border:1px solid var(--border);border-radius:6px;max-height:200px;overflow-y:auto;font-size:11px"></div>';

  h += '<div class="flex gap-6 justify-end">';
  h += '<button class="btn" onclick="closeModal()">Cancel</button>';
  h += '<button class="btn btn-r" id="sda-exec-btn" onclick="sdaExec(\'' + vm.replace(/'/g, "\\'") + '\')">&#128465; Delete <span id="sda-count">0</span> Snapshots</button>';
  h += '</div>';

  showModal(h);


  window._sdaSnaps = snaps;
  window._sdaVm = vm;
  sdaPreview();
}

function sdaPreview() {
  const snaps = window._sdaSnaps || [];
  const prefix = (document.getElementById('sda-prefix')?.value || '').trim();
  const keep = parseInt(document.getElementById('sda-keep')?.value) || 0;
  const el = document.getElementById('sda-preview');
  const countEl = document.getElementById('sda-count');
  if (!el) return;


  let filtered = prefix ? snaps.filter(s => s.name.startsWith(prefix)) : [...snaps];
  const total = filtered.length;
  const toDelete = keep > 0 && keep < total ? total - keep : (keep >= total ? 0 : total);
  const delList = filtered.slice(0, toDelete);
  const keepList = filtered.slice(toDelete);

  let h = '<div style="font-weight:600;margin-bottom:6px;color:var(--red)">Will DELETE: ' + delList.length + '</div>';
  if (delList.length > 0) {
    delList.forEach(s => {
      h += '<div style="color:var(--red);padding:1px 0">&#10060; ' + esc(s.name) + ' <span class="color-muted">' + esc(s.time) + '</span></div>';
    });
  } else {
    h += '<div class="color-muted">No snapshots match the criteria</div>';
  }
  if (keepList.length > 0) {
    h += '<div style="font-weight:600;margin-top:8px;margin-bottom:4px;color:var(--green)">Will KEEP: ' + keepList.length + '</div>';
    keepList.forEach(s => {
      h += '<div style="color:var(--green);padding:1px 0">&#9989; ' + esc(s.name) + ' <span class="color-muted">' + esc(s.time) + '</span></div>';
    });
  }
  el.innerHTML = h;
  if (countEl) countEl.textContent = delList.length;

  const btn = document.getElementById('sda-exec-btn');
  if (btn) btn.disabled = delList.length === 0;
}

async function sdaExec(vm) {
  const prefix = (document.getElementById('sda-prefix')?.value || '').trim();
  const keep = parseInt(document.getElementById('sda-keep')?.value) || 0;
  const btn = document.getElementById('sda-exec-btn');
  if (btn) { btn.disabled = true; btn.innerHTML = '<span class="spinner"></span> Deleting...'; }
  try {
    const body = { keep_recent: keep };
    if (prefix) body.prefix = prefix;
    const r = await fetchPost(EP.VM_SNAPSHOT_DELETE_ALL(vm), body);
    if (r && r.error) { toast('Delete failed: ' + (r.error.message || ''), false); return; }
    const d = unwrapData(r);
    toast('Deleted ' + (d.deleted || 0) + ' snapshots (remaining: ' + (d.remaining || 0) + ')');
    addEvt('Snapshot bulk delete — ' + vm + ': ' + (d.deleted || 0) + ' deleted');
    closeModal();
    renderSnapshots(document.getElementById('cb'), vmList[selectedVmIndex]);
  } catch (e) { toast('Error: ' + e.message, false); }
  if (btn) { btn.disabled = false; btn.innerHTML = '&#128465; Done'; }
}


var perfLayout = 'auto';

async function renderPerformance(b, v) {
  if (!v) return;

  var metrics = {};
  if (v.state === 'running') {
    try { var mr = await fetchGet(EP.VM_DETAIL(v.name)); metrics = unwrapData(mr) || mr || {}; } catch(e) {}
  }
  var cpuPct = metrics.cpu || v.live_cpu_pct || 0;
  var memPct = metrics.mem || v.mem_percent || 0;
  v.live_cpu_pct = cpuPct;
  v.mem_percent = memPct;

  cpuHistory.push(cpuPct); cpuHistory.shift();
  memHistory.push(memPct); memHistory.shift();
  var chartH = perfLayout === 'manual' ? '120px' : '80px';
  var gridCls = perfLayout === 'auto' ? 'sg grid-2' : '';
  var gridStyle = perfLayout === 'auto' ? '' : 'display:flex;flex-direction:column;gap:12px';
  b.innerHTML = '<div class="justify-between items-center mb-12"><h3>' + t('tab.performance') + ': ' + escapeHtml(v.name) + '</h3><div class="flex gap-6"><button class="tb ' + (perfLayout === 'auto' ? '' : 'btn') + '" onclick="perfLayout=\'auto\';renderPerformance(document.getElementById(\'cb\'),vmList[selectedVmIndex])" class="text-11">&#9638; Auto</button><button class="tb ' + (perfLayout === 'manual' ? '' : 'btn') + '" onclick="perfLayout=\'manual\';renderPerformance(document.getElementById(\'cb\'),vmList[selectedVmIndex])" class="text-11">&#9776; Stack</button></div></div>'
+ '<div class="' + gridCls + '" style="' + gridStyle + '">'
+ H.card('CPU Usage (60s) — ' + cpuPct.toFixed(1) + '%', renderProgressBar(cpuPct) + '<div style="position:relative;height:' + chartH + ';width:100%;margin-top:6px"><canvas id="cg"></canvas></div>')
+ H.card('Memory Usage (60s) — ' + memPct.toFixed(1) + '%', renderProgressBar(memPct) + '<div style="position:relative;height:' + chartH + ';width:100%;margin-top:6px"><canvas id="mg"></canvas></div>')
+ H.card('Disk IOPS', H.row(_L('읽기', 'Read'), '<span class="color-cyan">' + (metrics.disk_rd_req || 0).toLocaleString() + ' ops</span>') + H.row(_L('쓰기', 'Write'), '<span class="color-peach">' + (metrics.disk_wr_req || 0).toLocaleString() + ' ops</span>') + H.row('I/O Read', '<span class="color-cyan">' + formatBytes(metrics.disk_rd || 0) + '</span>') + H.row('I/O Write', '<span class="color-peach">' + formatBytes(metrics.disk_wr || 0) + '</span>'))
+ H.card('Network Packets', H.row('RX', '<span class="color-yellow">' + formatBytes(metrics.net_rx || 0) + '</span> (' + (metrics.net_rx_pkts || 0).toLocaleString() + ' pps)') + H.row('TX', '<span class="color-yellow">' + formatBytes(metrics.net_tx || 0) + '</span> (' + (metrics.net_tx_pkts || 0).toLocaleString() + ' pps)'))
+ '</div>';
  setTimeout(function() {
    createLineChart('cg', cpuHistory, 'CPU %', getChartColor('cpu'));
    createLineChart('mg', memHistory, 'MEM %', getChartColor('mem'));
  }, 30);
}


function renderTimeline(b, v) {
  if (!v) { b.innerHTML = '<p class="color-muted">' + t('vm.select') + '</p>'; return; }
  var events = (eventLog || []).filter(function(e) {
    var msg = (e.msg || e.raw || '').toLowerCase();
    return msg.includes(v.name.toLowerCase());
  }).slice(-20);

  var h = '<h3 class="mb-14">' + _L('타임라인', 'Timeline') + ': ' + esc(v.name) + '</h3>';
  if (events.length === 0) {
    h += '<div class="empty-state" style="text-align:center;padding:30px"><div style="font-size:36px;opacity:.5">&#128337;</div><div class="color-muted mt-8">' + _L('이벤트 없음', 'No events yet') + '</div></div>';
  } else {
    h += '<div style="position:relative;padding-left:24px;border-left:2px solid var(--border)">';
    events.forEach(function(e) {
      var msg = e.msg || e.raw || '';
      var isErr = msg.toLowerCase().includes('error') || msg.toLowerCase().includes('fail');
      var isOk = msg.toLowerCase().includes('start') || msg.toLowerCase().includes('created') || msg.toLowerCase().includes('completed');
      var color = isErr ? 'var(--red)' : isOk ? 'var(--green)' : 'var(--accent)';
      var icon = isErr ? '&#10060;' : isOk ? '&#9989;' : '&#128312;';
      h += '<div style="position:relative;margin-bottom:14px">';
      h += '<div style="position:absolute;left:-30px;top:2px;width:12px;height:12px;border-radius:50%;background:' + color + ';border:2px solid var(--bg)"></div>';
      h += '<div style="font-size:10px;color:var(--fg2);margin-bottom:2px">' + esc(e.time || '') + '</div>';
      h += '<div style="font-size:12px;color:var(--fg);padding:6px 10px;background:var(--bg2);border-radius:4px;border-left:3px solid ' + color + '">' + icon + ' ' + esc(msg) + '</div>';
      h += '</div>';
    });
    h += '</div>';
  }
  b.innerHTML = h;
}

async function showVmCompare() {
  if (checkedVms.size < 2) { toast(_L('비교할 VM을 2개 이상 선택하세요', 'Select 2+ VMs to compare'), false); return; }
  var selected = vmList.filter(function(v, idx) { return checkedVms.has(idx); }).slice(0, 4);

  await Promise.all(selected.map(async function(v) {
    if (v.state === 'running') {
      try {
        var mr = await fetchGet(EP.VM_DETAIL(v.name));
        var m = unwrapData(mr) || mr || {};
        v.live_cpu_pct = m.cpu || v.live_cpu_pct || 0;
        v.mem_percent = m.mem || v.mem_percent || 0;
        v.disk_rd = m.disk_rd || v.disk_rd || 0;
        v.disk_wr = m.disk_wr || v.disk_wr || 0;
        v.net_rx = m.net_rx || v.net_rx || 0;
        v.net_tx = m.net_tx || v.net_tx || 0;
      } catch(e) {}
    }
  }));
  var h = '<h2>' + _L('VM 비교', 'VM Comparison') + '</h2>';
  h += '<table class="text-12 w-full"><thead><tr><th>' + _L('항목', 'Property') + '</th>';
  selected.forEach(function(v) { h += '<th>' + esc(v.name) + '</th>'; });
  h += '</tr></thead><tbody>';
  var props = [
    { key: 'state', label: _L('상태', 'State') },
    { key: 'vcpu', label: 'vCPU' },
    { key: 'memory_mb', label: _L('메모리', 'Memory') + ' (MB)' },
    { key: 'live_cpu_pct', label: 'CPU %' },
    { key: 'mem_percent', label: _L('메모리', 'Memory') + ' %' },
    { key: 'disk_rd', label: _L('디스크 읽기', 'Disk Read') },
    { key: 'disk_wr', label: _L('디스크 쓰기', 'Disk Write') },
    { key: 'net_rx', label: 'Net RX' },
    { key: 'net_tx', label: 'Net TX' },
    { key: 'uuid', label: 'UUID' },
  ];
  props.forEach(function(p) {
    h += '<tr><td class="color-muted"><b>' + p.label + '</b></td>';
    selected.forEach(function(v) {
      var val = v[p.key];
      if (p.key === 'state') val = H.badge(val || '?', val === 'running' ? 'g' : 'r');
      else if (p.key === 'live_cpu_pct' || p.key === 'mem_percent') val = (val || 0).toFixed(1) + '%';
      else if (p.key === 'disk_rd' || p.key === 'disk_wr' || p.key === 'net_rx' || p.key === 'net_tx') val = formatBytes(val || 0);
      else val = esc(String(val || '-'));
      h += '<td>' + val + '</td>';
    });
    h += '</tr>';
  });
  h += '</tbody></table>';
  h += '<div class="text-right mt-14"><button class="btn" onclick="closeModal()">' + t('btn.close') + '</button></div>';
  showModal(h);
}

function showBulkActions() {
  if (checkedVms.size === 0) { toast(_L('VM을 선택하세요', 'Select VMs first'), false); return; }
  var count = checkedVms.size;
  var names = Array.from(checkedVms).map(function(idx) { return vmList[idx] ? vmList[idx].name : '?'; }).join(', ');
  var h = '<h2>' + _L('일괄 작업', 'Bulk Actions') + ' (' + count + ' VMs)</h2>';
  h += '<p class="mb-12 color-muted text-xs">' + esc(names) + '</p>';
  h += '<div class="sg grid-2">';
  h += H.card('&#9654; ' + _L('일괄 시작', 'Start All'), '<button class="btn btn-g w-full" onclick="bulkAction(\'start\')">' + t('power.start') + ' ' + count + ' VMs</button>');
  h += H.card('&#9632; ' + _L('일괄 중지', 'Stop All'), '<button class="btn btn-r w-full" onclick="bulkAction(\'stop\')">' + t('power.stop') + ' ' + count + ' VMs</button>');
  h += H.card('&#128247; ' + _L('일괄 스냅샷', 'Snapshot All'), '<input id="bulk-snap-name" placeholder="snap-' + Date.now() + '" class="w-full mb-6"><button class="btn w-full" onclick="bulkSnapshot()">' + t('snap.created') + '</button>');
  h += H.card('&#10074;&#10074; ' + _L('일괄 일시정지', 'Suspend All'), '<button class="btn w-full" onclick="bulkAction(\'suspend\')">' + t('power.pause') + ' ' + count + ' VMs</button>');
  h += '</div>';
  h += '<div class="text-right mt-14"><button class="btn" onclick="closeModal()">' + t('btn.close') + '</button></div>';
  showModal(h);
}

async function bulkAction(action) {
  closeModal();
  var names = Array.from(checkedVms).map(function(idx) { return vmList[idx] ? vmList[idx].name : null; }).filter(Boolean);
  showModal('<h2>' + _L('일괄 작업 진행 중', 'Bulk Action in Progress') + '</h2><div class="prog-bar"><div class="prog-fill" id="bulk-prog" class="w-pct-0"></div></div><div id="bulk-status" class="prog-status"><span class="spinner"></span> 0/' + names.length + '</div>');
  var pf = document.getElementById('bulk-prog');
  var ps = document.getElementById('bulk-status');
  var failed = [];
  for (var i = 0; i < names.length; i++) {
    if (pf) pf.style.width = ((i + 1) / names.length * 100) + '%';
    if (ps) ps.innerHTML = '<span class="spinner"></span> ' + (i + 1) + '/' + names.length + ' — ' + esc(names[i]);
    try { await fetchPost(EP.VM_ACTION(names[i], action), {}); } catch (e) { failed.push(names[i] + ': ' + e.message); }
  }
  var okCount = names.length - failed.length;
  if (ps) {
    ps.innerHTML = '&#9989; ' + okCount + '/' + names.length + ' OK' +
      (failed.length ? '<br><span class="color-red">&#10060; ' + failed.length + ' failed</span><div class="text-xs color-muted" style="max-height:120px;overflow:auto">' + failed.map(esc).join('<br>') + '</div>' : '');
  }
  if (failed.length) toast(failed.length + ' / ' + names.length + ' ' + action + ' failed', false);
  addEvt('Bulk ' + action + ': ' + okCount + '/' + names.length + ' OK');
  setTimeout(function() { closeModal(); loadAll(); }, 2000);
}

async function bulkSnapshot() {
  closeModal();
  var snapName = document.getElementById('bulk-snap-name')?.value || 'snap-' + Date.now();
  var names = Array.from(checkedVms).map(function(idx) { return vmList[idx] ? vmList[idx].name : null; }).filter(Boolean);
  showModal('<h2>' + _L('일괄 스냅샷', 'Bulk Snapshot') + '</h2><div class="prog-bar"><div class="prog-fill" id="bulk-prog" class="w-pct-0"></div></div><div id="bulk-status" class="prog-status"><span class="spinner"></span> 0/' + names.length + '</div>');
  var pf = document.getElementById('bulk-prog');
  var ps = document.getElementById('bulk-status');
  for (var i = 0; i < names.length; i++) {
    if (pf) pf.style.width = ((i + 1) / names.length * 100) + '%';
    if (ps) ps.innerHTML = '<span class="spinner"></span> ' + (i + 1) + '/' + names.length + ' — ' + esc(names[i]);
    try { await fetchPost(EP.VM_SNAPSHOT_CREATE(names[i]), { snap_name: snapName }); } catch (e) {  }
  }
  if (ps) ps.innerHTML = '&#9989; ' + _L('완료', 'Done') + ': ' + names.length + ' snapshots';
  addEvt('Bulk snapshot: ' + snapName + ' → ' + names.join(', '));
  setTimeout(function() { closeModal(); loadAll(); }, 2000);
}




async function showVmFailureDetail(statusEl, progEl, vmName, actionLabel) {
  if (progEl) { progEl.style.width = '100%'; progEl.style.background = 'var(--red)'; }

  if (typeof addNotification === 'function') {
    addNotification('error',
      (actionLabel || 'VM action') + ' failed: ' + vmName,
      _L('30초 내 상태 전이 미확인 — audit DB 확인 필요', 'State change not confirmed within 30s — check audit DB'));
  }
  if (typeof addEvt === 'function') {
    addEvt('FAIL ' + (actionLabel || 'action') + ' ' + vmName + ' — state change timeout');
  }

  var safeName = escapeHtml(vmName);
  var titleHtml = '&#10060; ' + _L('상태 변경 실패', 'State change failed') + ': ' + safeName;
  var subHtml = _L('백엔드 워커가 30초 내 상태 전이를 완료하지 못했습니다.', 'Backend worker did not complete the state transition within 30s.');
  var loadingHtml = '<span class="spinner"></span> ' + _L('실패 사유 조회 중...', 'Loading failure reason...');
  var closeBtnHtml = '<button class="btn" onclick="closeModal();loadAll()">' + t('btn.close') + '</button>';
  var html = titleHtml
    + '<div class="text-xs color-muted mt-8">' + subHtml + '</div>'
    + '<div id="pwr-err-detail" class="text-xs mt-8" style="max-height:160px;overflow:auto;text-align:left;background:rgba(0,0,0,0.2);padding:8px;border-radius:4px">' + loadingHtml + '</div>'
    + '<div class="mt-12">' + closeBtnHtml + '</div>';
  if (statusEl) statusEl.innerHTML = html;

  try {
    var resp = await fetchGet('/api/v1/health/recent-errors?vm=' + encodeURIComponent(vmName) + '&limit=3');
    var errs = (resp && (resp.data || resp.errors)) || [];
    var detailEl = document.getElementById('pwr-err-detail');
    if (!detailEl) return;
    if (errs.length) {
      var rows = errs.map(function(e) {
        return '<div>&bull; <strong>' + escapeHtml(e.method || '?') + '</strong>: ' + escapeHtml(e.message || e.error || 'unknown') + '</div>';
      }).join('');
      detailEl.innerHTML = rows;
    } else {
      detailEl.innerHTML = '<em>' + _L('워커 실패 로그 없음 — journalctl -u purecvisorsd -f 로 실시간 확인', 'No worker failure log — try journalctl -u purecvisorsd -f') + '</em>';
    }
  } catch (lookupErr) {
    var detailEl2 = document.getElementById('pwr-err-detail');
    if (detailEl2) detailEl2.innerHTML = '<em>' + _L('상세 조회 실패', 'Detail lookup failed') + ': ' + escapeHtml(lookupErr.message || 'unknown') + '</em>';
  }
}






async function vmPower(a) {
  const v = vmList[selectedVmIndex]; if (!v) return;
  var actionLabels = {
    start: { icon: '&#9654;', label: _L('시작', 'Start'), past: _L('시작됨', 'Started'), color: 'var(--green)' },
    stop: { icon: '&#9632;', label: _L('중지', 'Stop'), past: _L('중지됨', 'Stopped'), color: 'var(--red)' },
    suspend: { icon: '&#10074;&#10074;', label: _L('일시정지', 'Pause'), past: _L('일시정지됨', 'Paused'), color: 'var(--yellow)' },
    resume: { icon: '&#9654;&#9654;', label: _L('재개', 'Resume'), past: _L('재개됨', 'Resumed'), color: 'var(--green)' }
  };
  var al = actionLabels[a] || { icon: '&#9881;', label: a, past: a, color: 'var(--accent)' };

  showModal('<div class="text-center p-20">'
    + '<div style="font-size:48px;margin-bottom:12px">' + al.icon + '</div>'
    + '<h2 class="mb-8">' + escapeHtml(v.name) + '</h2>'
    + '<div class="prog-bar"><div class="prog-fill" id="pwr-p" style="width:30%;background:' + al.color + '"></div></div>'
    + '<div class="prog-status" id="pwr-s" class="mt-10"><span class="spinner"></span> ' + al.label + ' ' + _L('진행 중...', 'in progress...') + '</div>'
    + '</div>');
  try {
    var pf = document.getElementById('pwr-p'), ps = document.getElementById('pwr-s');
    var r = await fetchPost(EP.VM_ACTION(v.name, a), {});

    if (r && r.error) {
      if (pf) { pf.style.width = '100%'; pf.style.background = 'var(--red)'; }
      if (ps) ps.innerHTML = '&#10060; ' + escapeHtml(r.error.message || 'Failed');
      setTimeout(closeModal, 3000);
      return;
    }
    if (pf) pf.style.width = '60%';
    if (ps) ps.innerHTML = '<span class="spinner"></span> ' + _L('상태 확인 중...', 'Verifying state...');

    var expectedState = (a === 'start' || a === 'resume') ? 'running' : (a === 'suspend') ? 'paused' : 'shutoff';
    var verified = false;
    var maxPolls = 15;
    for (var pi = 0; pi < maxPolls; pi++) {
      await new Promise(function(res) { setTimeout(res, 2000); });
      if (pf) pf.style.width = Math.min(95, 65 + pi * 2) + '%';
      try {
        var vl = await fetchGet(EP.VM_LIST());
        var list = unwrapList(vl);
        var found = list.find(function(x) { return x.name === v.name; });
        if (found && found.state === expectedState) { verified = true; break; }
      } catch(e2) {}
    }
    if (verified) {
      if (pf) { pf.style.width = '100%'; pf.style.background = 'var(--green)'; }
      if (ps) ps.innerHTML = '&#9989; ' + al.past;
      addEvt(v.name + ' ' + al.past);
      setTimeout(function() { closeModal(); loadAll(); }, 2000);
    } else {


      await showVmFailureDetail(ps, pf, v.name, al.label);
      loadAll();
    }
  } catch (e) {
    var pf2 = document.getElementById('pwr-p'), ps2 = document.getElementById('pwr-s');
    if (pf2) { pf2.style.width = '100%'; pf2.style.background = 'var(--red)'; }
    var errMsg = e.name === 'AbortError' ? _L('타임아웃 (10초)', 'timeout (10s)') : escapeHtml(e.message);

    var btnHtml = '<div class="mt-12"><button class="btn" onclick="closeModal()">' + t('btn.close') + '</button></div>';
    if (ps2) ps2.innerHTML = '&#10060; ' + _L('실패', 'Failed') + ': ' + errMsg + btnHtml;
  }
}


async function vmDel() {
  const v = vmList[selectedVmIndex]; if (!v) return;
  if (typeof destroyConfirm !== 'function') {

    if (!confirm(_L('VM 삭제: ', 'Delete VM: ') + v.name + '?')) return;
    return doVmDel(v.name);
  }
  destroyConfirm({
    title: t('vm.delete'),
    name: v.name,
    warning: t('vm.delete.confirm') + ' — ' +
             _L('ZFS 볼륨과 디스크 이미지까지 영구 삭제됩니다. 이 작업은 되돌릴 수 없습니다.',
                'ZFS volume and disk image will be permanently destroyed. This cannot be undone.'),
    onConfirm: function() { doVmDel(v.name); }
  });
}



async function doVmDel(n) {
  showModal('<h2 class="color-red">&#9888; Deleting VM</h2><p><b class="color-accent">' + escapeHtml(n) + '</b></p><div class="prog-bar"><div class="prog-fill" id="dv-p" class="w-pct-10"></div></div><div class="prog-status" id="dv-s"><span class="spinner"></span>Sending delete request...</div>');
  const pf = document.getElementById('dv-p'), ps = document.getElementById('dv-s');
  var deleteError = null;
  try {
    if (pf) pf.style.width = '30%';
    const d = await fetchDelete(EP.VM_DETAIL(n)).catch(function(e) { return { error: { message: e && e.message || 'Network error' } }; });
    if (d && d.error) {
      deleteError = d.error.message || 'Failed';

      if (ps) ps.innerHTML = '<span class="spinner"></span>&#9888; ' + escapeHtml(deleteError) + _L(' — 서버 상태 확인 중...', ' — polling server state...');
    } else {
      if (ps) ps.innerHTML = '<span class="spinner"></span>Waiting for zvol cleanup...';
    }
    if (pf) pf.style.width = '50%';
    for (let i = 0; i < 10; i++) {
      await new Promise(r => setTimeout(r, 2000));
      if (pf) pf.style.width = Math.min(95, 55 + i * 4) + '%';
      if (ps && !deleteError) ps.innerHTML = '<span class="spinner"></span>Cleaning up (' + (i + 1) + '/10)...';
      try {
        const vl = await fetchGet(EP.VM_LIST());
        const vms = unwrapList(vl);
        if (!vms.find(x => x.name === n)) {

          if (pf) { pf.style.width = '100%'; pf.style.background = 'var(--green)'; }
          if (ps) ps.innerHTML = '&#9989; ' + t('vm.deleted');
          toast(t('vm.deleted'));
          addEvt(t('vm.deleted') + ': ' + n);
          setTimeout(function() { closeModal(); loadAll(); }, 1500);
          return;
        }
      } catch (e) { if(typeof _DEBUG !== 'undefined' && _DEBUG) console.warn('vl:', e.message); }
    }

    if (pf) { pf.style.width = '100%'; pf.style.background = 'var(--yellow)'; }
    if (deleteError) {
      if (ps) ps.innerHTML = '&#10060; ' + escapeHtml(deleteError);
      toast('&#10060; ' + escapeHtml(deleteError), false);
    } else {
      if (ps) ps.innerHTML = '&#9888; ' + _L('삭제가 오래 걸리고 있습니다', 'Delete taking longer than expected');
      toast(_L('삭제가 오래 걸리고 있습니다. 잠시 후 새로고침하세요.', 'Delete taking longer than expected — refresh shortly.'), false);
    }
    setTimeout(function() { closeModal(); loadAll(); }, 2000);
  } catch (e) {
    if (pf) { pf.style.width = '100%'; pf.style.background = 'var(--red)'; }
    if (ps) ps.innerHTML = '&#10060; ' + escapeHtml(e.message || 'Unknown error');
    toast(e.message || 'Unknown error', false);
    setTimeout(closeModal, 3000);
  }
}









function wizDefaults() {
  return {
    name: '',
    vcpu: 2,
    mem: 2048,
    disk: 20,
    iso: '',
    bridge: '',
    storage_type: 'auto',
    storage_pool: '',
    image_dir: '',
    storage_pools: [],
    storage_loaded: false
  };
}

var wizStep = 1, wizData = wizDefaults();

function showCreate() {
  wizStep = 1;
  wizData = wizDefaults();
  if (typeof markFormDirty === 'function') markFormDirty('vm-create');
  renderWiz();
}
function wizSave() {
  if (wizStep === 1) { wizData.name = document.getElementById('wn')?.value || wizData.name; }
  else if (wizStep === 2) { wizData.vcpu = +(document.getElementById('wc')?.value || wizData.vcpu); wizData.mem = +(document.getElementById('wm')?.value || wizData.mem); }
  else if (wizStep === 3) {
    wizData.disk = +(document.getElementById('wd')?.value || wizData.disk);
    wizData.iso = document.getElementById('wi')?.value || wizData.iso;
    wizData.bridge = document.getElementById('wb')?.value || wizData.bridge;
    wizData.storage_type = document.getElementById('wst')?.value || wizData.storage_type;
    wizData.storage_pool = (document.getElementById('wspool')?.value || wizData.storage_pool || '').trim();
    wizData.image_dir = (document.getElementById('widir')?.value || wizData.image_dir || '').trim();
  }
}
function wizGo(s) {
  wizSave();

  if (wizStep === 1 && s > 1) {
    const name = wizData.name.trim();
    if (!name) { toast(_L('VM 이름을 입력하세요', 'VM name is required'), false); return; }
    if (!/^[a-zA-Z0-9][a-zA-Z0-9_-]{0,63}$/.test(name)) {
      toast(_L('VM 이름: 1-64자, 영문/숫자/_- 만 허용 (첫 글자는 영문/숫자)', 'VM name: 1-64 chars, [a-zA-Z0-9_-], must start with alphanumeric'), false);
      return;
    }
  }
  wizStep = s; renderWiz();
}

function renderWiz() {
  let h = `<h2>${t('vm.new')}</h2><div class="wizard-steps"><div class="step ${wizStep >= 1 ? 'active' : ''}${wizStep > 1 ? ' done' : ''}">1. Basic</div><div class="step ${wizStep >= 2 ? 'active' : ''}${wizStep > 2 ? ' done' : ''}">2. Resources</div><div class="step ${wizStep >= 3 ? 'active' : ''}">3. Storage &amp; Network</div></div>`;
  if (wizStep === 1) h += `<div class="fr"><label>VM Name</label><input id="wn" value="${escapeHtml(wizData.name)}" placeholder="my-vm"></div><div class="text-right mt-12"><button class="btn" onclick="wizGo(2)">${t('btn.next')} &rarr;</button></div>`;
  else if (wizStep === 2) h += `<div class="fr"><label>vCPU</label><input id="wc" type="number" value="${escapeHtml(String(wizData.vcpu))}"></div><div class="fr"><label>Memory MB</label><input id="wm" type="number" value="${escapeHtml(String(wizData.mem))}"></div><div class="text-right mt-12"><button class="tb" onclick="wizGo(1)">&larr; ${t('btn.prev')}</button> <button class="btn" onclick="wizGo(3)">${t('btn.next')} &rarr;</button></div>`;
  else {
    const stSel = wizData.storage_type || 'auto';
    const poolVal = wizData.storage_pool || 'pcvpool/vms';
    const imageDirVal = wizData.image_dir || '/var/lib/libvirt/images';
    const isFileStorage = stSel === 'qcow2' || stSel === 'raw';
    const pools = Array.isArray(wizData.storage_pools) ? wizData.storage_pools : [];
    const poolOptions = pools.map(function(p) {
      return '<option value="' + escapeHtml(p) + '"' + (p === poolVal ? ' selected' : '') + '>' + escapeHtml(p) + '</option>';
    }).join('');
    h += `<div class="fr"><label>Disk GB</label><input id="wd" type="number" value="${escapeHtml(String(wizData.disk))}"></div>`
      + `<div class="fr"><label>${_L('스토리지 타입', 'Storage Type')}</label>`
      + `<select id="wst" onchange="wizStorageChanged(true)">`
      + `<option value="auto"${stSel === 'auto' ? ' selected' : ''}>${_L('자동 (서버 감지)', 'Auto (server detected)')}</option>`
      + `<option value="zvol"${stSel === 'zvol' ? ' selected' : ''}>ZFS zvol — ${_L('블록 디바이스, 고성능', 'Block device, high performance')}</option>`
      + `<option value="qcow2"${stSel === 'qcow2' ? ' selected' : ''}>qcow2 — ${_L('파일 기반, 스냅샷/씬 프로비저닝', 'File based, snapshot/thin provisioning')}</option>`
      + `<option value="raw"${stSel === 'raw' ? ' selected' : ''}>raw — ${_L('파일 기반, 최대 I/O 성능', 'File based, maximum I/O performance')}</option>`
      + `</select></div>`
      + (isFileStorage
        ? `<div class="fr"><label>${_L('저장 위치', 'Storage Location')}</label><input id="widir" value="${escapeHtml(imageDirVal)}" placeholder="/var/lib/libvirt/images" oninput="wizStorageChanged(false)"></div>`
        : `<div class="fr"><label>${_L('저장 위치', 'Storage Location')}</label><div class="flex gap-6 flex-1"><input id="wspool" value="${escapeHtml(poolVal)}" placeholder="pcvpool/vms" class="flex-1" oninput="wizStorageChanged(false)">`
          + `<select id="wspick" onchange="wizPickStoragePool()" title="${escapeHtml(_L('사용 가능한 ZFS 풀', 'Available ZFS pools'))}"><option value="">${escapeHtml(_L('풀 선택', 'Pool'))}</option>${poolOptions}</select>`
          + `<button class="btn text-xs" onclick="wizLoadStorageTargets(true)">${_L('새로고침', 'Refresh')}</button></div></div>`)
      + (isFileStorage ? `<input id="wspool" type="hidden" value="${escapeHtml(poolVal)}">` : `<input id="widir" type="hidden" value="${escapeHtml(imageDirVal)}">`)
      + `<div class="stat-label mb-8" id="wstorage-preview">${escapeHtml(wizStoragePreview())}</div>`
      + `<div class="fr"><label>ISO Image</label><div class="flex gap-6"><input id="wi" value="${escapeHtml(wizData.iso)}" placeholder="ISO path..." class="flex-1"><button class="btn" onclick="browseISO()">Browse</button></div></div>`
      + `<div class="fr"><label>Network</label><div class="flex gap-6 flex-1"><select id="wb"><option value="${escapeHtml(wizData.bridge)}">${t('loading')}</option></select><button class="btn text-xs" onclick="wizLoadNets()">Refresh</button></div></div>`
      + `<div class="text-right mt-12"><button class="tb" onclick="wizGo(2)">&larr; ${t('btn.prev')}</button> <button class="btn btn-g" onclick="doCreate()">${t('vm.create')}</button></div>`;
  }
  showModal(h, { replace: true });
  if (wizStep === 3) {
    setTimeout(wizLoadNets, 80);
    setTimeout(function() { wizLoadStorageTargets(false); }, 80);
  }
}

function wizStoragePreview() {
  const st = wizData.storage_type || 'auto';
  const name = (wizData.name || '<vm-name>').trim() || '<vm-name>';
  const pool = wizData.storage_pool || 'pcvpool/vms';
  const imageDir = wizData.image_dir || '/var/lib/libvirt/images';
  if (st === 'qcow2') return imageDir + '/' + name + '.qcow2';
  if (st === 'raw') return imageDir + '/' + name + '.img';
  if (st === 'zvol') return '/dev/zvol/' + pool + '/' + name;
  return _L('자동: ZFS 가능 시 ', 'Auto: ZFS when available ') + '/dev/zvol/' + pool + '/' + name
    + _L(', 아니면 ', ', otherwise ') + imageDir + '/' + name + '.qcow2';
}

function wizStorageChanged(rerender) {
  wizSave();
  if (rerender) { renderWiz(); return; }
  const el = document.getElementById('wstorage-preview');
  if (el) el.textContent = wizStoragePreview();
}

function wizPickStoragePool() {
  const sel = document.getElementById('wspick');
  const inp = document.getElementById('wspool');
  if (sel && inp && sel.value) {
    inp.value = sel.value;
    wizData.storage_pool = sel.value;
  }
  wizStorageChanged(false);
}

async function wizLoadStorageTargets(force) {
  if (wizStep === 3) wizSave();
  if (wizData.storage_loaded && !force) return;
  try {
    const cfg = await fetchGet(EP.CONFIG_DAEMON());
    const data = unwrapData(cfg) || {};
    if (cfg && !cfg.error && data.storage) {
      if (!wizData.storage_pool && data.storage.zvol_pool) wizData.storage_pool = data.storage.zvol_pool;
      if (!wizData.image_dir && data.storage.image_dir) wizData.image_dir = data.storage.image_dir;
    }
  } catch (e) {}
  try {
    const poolsResp = await fetchGet(EP.STORAGE_POOLS());
    const pools = unwrapList(poolsResp).map(function(p) { return p && p.name; }).filter(Boolean);
    if (poolsResp && !poolsResp.error) wizData.storage_pools = pools;
  } catch (e) {}
  wizData.storage_loaded = true;
  const poolInput = document.getElementById('wspool');
  const imageInput = document.getElementById('widir');
  if (poolInput && wizData.storage_pool) poolInput.value = wizData.storage_pool;
  if (imageInput && wizData.image_dir) imageInput.value = wizData.image_dir;
  const poolSel = document.getElementById('wspick');
  if (poolSel) {
    const cur = wizData.storage_pool || poolInput?.value || '';
    poolSel.innerHTML = '<option value="">' + escapeHtml(_L('풀 선택', 'Pool')) + '</option>'
      + wizData.storage_pools.map(function(p) {
        return '<option value="' + escapeHtml(p) + '"' + (p === cur ? ' selected' : '') + '>' + escapeHtml(p) + '</option>';
      }).join('');
  }
  wizStorageChanged(false);
}



async function wizLoadNets() {
  const sel = document.getElementById('wb'); if (!sel) return;
  try {
    const r = await fetchGet(EP.NET_LIST());
    const nl = unwrapList(r);
    const cur = wizData.bridge || '';
    let h = '';
    nl.forEach(n => {
      const name = n.name || ''; if (!name) return;
      const mode = n.mode || ''; const state = n.state || ''; const ip = n.ip_cidr || '';
      const info = [mode, ip, state].filter(Boolean).join(' | ');
      h += '<option value="' + escapeHtml(name) + '"' + (name === cur ? ' selected' : '') + '>' +
           escapeHtml(name) + (info ? ' (' + escapeHtml(info) + ')' : '') + '</option>';
    });
    if (h === '') {
      h = '<option value="" disabled selected>' +
          escapeHtml(_L('네트워크 없음 — 먼저 브릿지를 생성하세요', 'No networks — create a bridge first')) +
          '</option>';
      toast(_L('네트워크가 없습니다. Network 탭에서 브릿지를 먼저 생성하세요.',
               'No networks found. Create a bridge in the Network tab first.'), false);
    }
    sel.innerHTML = h;
  } catch (e) {
    sel.innerHTML = '<option value="" disabled selected>' +
                    escapeHtml(_L('네트워크 조회 실패', 'Network list failed')) + '</option>';
    toast(_L('네트워크 목록 조회 실패: ', 'Failed to load network list: ') + (e.message || ''), false);
  }
}

async function browseISO() {
  closeISOBrowser();
  const ov = document.createElement('div'); ov.id = 'iso-overlay';
  ov.style.cssText = 'position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,.6);display:flex;align-items:center;justify-content:center;z-index:250';
  let h = '<div style="background:var(--bg-panel);backdrop-filter:blur(16px);border:1px solid var(--accent);border-radius:8px;padding:20px;min-width:500px;max-width:90vw;max-height:85vh;overflow-y:auto;box-shadow:0 0 30px var(--neon-glow)">';
  h += '<h2 style="font-family:var(--font-display);font-size:16px;color:var(--accent)">&#128191; ' + t('iso.browser_title') + '</h2>';
  h += '<div class="stat-label mb-10">' + t('iso.browser_desc') + '</div>';
  h += '<div id="iso-modal-list" style="max-height:380px;overflow-y:auto;border:1px solid var(--border);border-radius:6px;background:var(--bg)"><div class="p-12 color-muted text-xs"><span class="spinner"></span> ' + t('loading') + '</div></div>';
  h += '<div class="flex gap-8 items-center mt-10">';
  h += '<input id="iso-manual-path" placeholder="Direct path..." style="flex:1;padding:8px 10px;background:var(--bg3);border:1px solid var(--border);border-radius:6px;color:var(--fg);font-size:12px" onkeydown="if(event.key===\'Enter\')isoSelectManual()">';
  h += '<button class="btn btn-g" onclick="isoSelectManual()">' + t('btn.confirm') + '</button>';
  h += '<button class="btn btn-r" onclick="closeISOBrowser()">' + t('btn.cancel') + '</button>';
  h += '</div></div>';
  ov.innerHTML = h;
  ov.addEventListener('click', e => { if (e.target === ov) closeISOBrowser(); });
  document.body.appendChild(ov);
  try {
    const r = await fetchGet(EP.ISO_LIST()); const il = unwrapList(r);
    const el = document.getElementById('iso-modal-list'); if (!el) return;
    let lh = '';
    const dirs = {}; il.forEach(iso => { const d = iso.dir || '/pcvpool/iso'; if (!dirs[d]) dirs[d] = []; dirs[d].push(iso); });
    if (il.length === 0) { lh = '<div class="text-center" style="padding:16px;color:var(--fg2)"><div style="font-size:24px;margin-bottom:8px">&#128194;</div><div>' + t('iso.not_found') + '</div></div>'; }
    else { Object.entries(dirs).forEach(([dir, files]) => {
      lh += '<div style="padding:6px 10px;font-size:10px;color:var(--accent);border-bottom:1px solid var(--border);background:rgba(0,240,255,.03);font-weight:600">&#128194; ' + dir + ' (' + files.length + ' files)</div>';
      files.forEach(iso => { const p = iso.path || iso.name; const fn = (iso.name || p).replace(/^.*\//, '');
        const sz = iso.size_mb ? (iso.size_mb >= 1024 ? (iso.size_mb / 1024).toFixed(1) + 'GB' : iso.size_mb + 'MB') : '';
        const ext = fn.split('.').pop().toUpperCase(); const icon = ext === 'IMG' ? '&#128190;' : '&#128191;';
        lh += '<div onclick="isoSelect(\'' + p.replace(/'/g, "\\'") + '\')" style="padding:8px 12px;cursor:pointer;font-size:12px;display:flex;align-items:center;gap:8px;border-bottom:1px solid var(--border);transition:background .1s" onmouseover="this.style.background=\'var(--bg3)\'" onmouseout="this.style.background=\'\'">';
        lh += '<span class="text-lg">' + icon + '</span><span style="flex:1;font-weight:500">' + fn + '</span>' + H.badge(ext, 'y') + '<span class="color-muted" style="font-size:11px;min-width:60px;text-align:right">' + sz + '</span></div>'; }); }); }
    el.innerHTML = lh;
  } catch (e) { const el = document.getElementById('iso-modal-list'); if (el) el.innerHTML = '<div class="p-12 color-red">&#10060; Error: ' + escapeHtml(e.message) + '</div>'; }
}

function isoSelect(path) { wizData.iso = path; closeISOBrowser(); renderWiz(); }
function isoSelectManual() { const v = document.getElementById('iso-manual-path')?.value; if (v) { wizData.iso = v; closeISOBrowser(); renderWiz(); } }
function closeISOBrowser() { const el = document.getElementById('iso-overlay'); if (el) el.remove(); }

async function doCreate() {
  wizSave(); const d = wizData;
  const name = (d.name || '').trim();
  if (!name) { toast(_L('VM 이름을 입력하세요', 'VM name is required'), false); return; }
  if (!/^[a-zA-Z0-9][a-zA-Z0-9_-]{0,63}$/.test(name)) {
    toast(_L('VM 이름: 1-64자, 영문/숫자/_- 만 허용', 'VM name: 1-64 chars, [a-zA-Z0-9_-]'), false); return;
  }

  if (!d.bridge) {
    toast(_L('네트워크 브릿지를 선택하세요', 'Select a network bridge'), false); return;
  }

  if (!(d.vcpu >= 1 && d.vcpu <= 256)) {
    toast(_L('vCPU 는 1~256 사이', 'vCPU must be between 1 and 256'), false); return;
  }
  if (!(d.mem >= 256 && d.mem <= 1048576)) {
    toast(_L('메모리는 256~1048576 MB 사이', 'Memory must be 256~1048576 MB'), false); return;
  }
  if (!(d.disk >= 1 && d.disk <= 65536)) {
    toast(_L('디스크는 1~65536 GB 사이', 'Disk must be 1~65536 GB'), false); return;
  }


  if (typeof clearFormDirty === 'function') clearFormDirty('vm-create');
  closeModal(true);
  toast('&#128187; ' + escapeHtml(name) + ' ' + _L('생성 시작...', 'Creating...'), 's');

  try {

    const body = { name: name, vcpu: d.vcpu, memory_mb: d.mem, disk_size_gb: d.disk, network_bridge: d.bridge };
    if (d.storage_type && d.storage_type !== 'auto') body.storage_type = d.storage_type;
    if (d.storage_pool) body.storage_pool = d.storage_pool;
    if (d.image_dir) body.image_dir = d.image_dir;
    if (d.iso) body.iso_path = d.iso;
    const r = await fetchPost(EP.VM_CREATE(), body);

    if (r && r.error) {
      toast('&#10060; ' + _L('VM 생성 실패', 'VM creation failed') + ': ' + (r.error.message || JSON.stringify(r.error)), false);
      return;
    }

    toast('&#9989; ' + t('vm.created') + ': ' + escapeHtml(name), 's');
    const storageLabel = (d.storage_type && d.storage_type !== 'auto') ? d.storage_type : 'auto';
    addEvt(t('vm.created') + ': ' + name + ' (' + d.vcpu + 'vCPU, ' + d.mem + 'MB, ' + d.disk + 'GB, ' + storageLabel + ')');


    if (typeof invalidateCache === 'function') invalidateCache('vm.list');
    var attempts = 0;
    var errStreak = 0;
    var poll = null;
    var hardTimer = null;
    var stopPoll = function() {
      if (poll) { clearInterval(poll); poll = null; }
      if (hardTimer) { clearTimeout(hardTimer); hardTimer = null; }
    };
    hardTimer = setTimeout(function() {
      stopPoll();
      loadAll();
    }, 25000);
    poll = setInterval(async function() {
      attempts++;
      try {
        var fresh = await fetchGet(EP.VM_LIST());
        errStreak = 0;
        var list = Array.isArray(fresh) ? fresh : (fresh && fresh.data) || [];
        if (list.find(function(v){ return v.name === name; })) {
          stopPoll();
          window.vmList = list; vmList = list;
          render();
        } else if (attempts >= 20) {
          stopPoll();
          loadAll();
          toast(_L('VM 생성에 시간이 걸리고 있습니다. 잠시 후 새로고침하세요.', 'VM creation taking longer than expected'), false);
        }
      } catch (e) {
        errStreak++;
        if (errStreak >= 5 || attempts >= 20) {
          stopPoll();
          loadAll();
        }
      }
    }, 1000);
  } catch (e) {
    toast('&#10060; ' + _L('VM 생성 실패', 'VM creation failed') + ': ' + (e.message || ''), false);
  }
}


function showSettings() {
  const v = vmList[selectedVmIndex]; if (!v) return;
  showModal(`<h2>${t('vm.settings')}: ${escapeHtml(v.name)}</h2><div class="split"><div class="left"><div class="hw-list"><div class="hw-item active" onclick="setHw(this,'identity')"><span class="hw-icon">&#9998;</span><span class="hw-label">Identity</span><span class="hw-val">${escapeHtml(v.name)}</span></div><div class="hw-item" onclick="setHw(this,'cpu')"><span class="hw-icon">&#9881;</span><span class="hw-label">CPU</span><span class="hw-val">${escapeHtml(String(v.vcpu || '-'))} vCPU</span></div><div class="hw-item" onclick="setHw(this,'mem')"><span class="hw-icon">&#128204;</span><span class="hw-label">Memory</span><span class="hw-val">${escapeHtml(String(v.memory_mb || '-'))} MB</span></div><div class="hw-item" onclick="setHw(this,'disk')"><span class="hw-icon">&#128190;</span><span class="hw-label">Disk Resize</span><span class="hw-val">Storage</span></div><div class="hw-item" onclick="setHw(this,'cpupin')"><span class="hw-icon">&#128204;</span><span class="hw-label">CPU Pinning</span><span class="hw-val">vCPU Pin</span></div><div class="hw-item" onclick="setHw(this,'bw')"><span class="hw-icon">&#128246;</span><span class="hw-label">Bandwidth</span><span class="hw-val">QoS</span></div><div class="hw-item" onclick="setHw(this,'cdrom')"><span class="hw-icon">&#128191;</span><span class="hw-label">CD/DVD (SATA)</span><span class="hw-val">ISO</span></div><div class="hw-item" onclick="setHw(this,'nic')"><span class="hw-icon">&#127760;</span><span class="hw-label">Network</span><span class="hw-val">Bridge</span></div><div class="hw-item" onclick="setHw(this,'autoprotect')"><span class="hw-icon">&#128737;</span><span class="hw-label">AutoProtect</span><span class="hw-val">Backup</span></div></div></div><div class="right"><div id="hw-edit">${hwIdentity()}</div></div></div><div class="text-right mt-12"><button class="btn btn-r" onclick="closeModal()">${t('btn.close')}</button></div>`);
}

function setHw(el, t2) { document.querySelectorAll('.hw-item').forEach(e => e.classList.remove('active')); el.classList.add('active'); document.getElementById('hw-edit').innerHTML = ({ identity: hwIdentity, cpu: hwCpu, mem: hwMem, disk: hwDisk, cpupin: hwCpuPin, bw: hwBandwidth, cdrom: hwCd, nic: hwNic, autoprotect: hwAP })[t2](); }

function _vmRenameBlocked(v) {
  const st = String((v && (v.state || v.status)) || '').toLowerCase();
  return st === 'running' || st === 'paused';
}
function hwIdentity() {
  const v = vmList[selectedVmIndex];
  const blocked = _vmRenameBlocked(v);
  return `<h4>Identity</h4><div class="fr"><label>${_L('현재 이름', 'Current')}</label><input value="${escapeAttr(v?.name || '')}" disabled style="opacity:.65"></div><div class="fr"><label>${_L('새 이름', 'New')}</label><input id="rn-new" value="${escapeAttr(v?.name || '')}" maxlength="64" ${blocked ? 'disabled' : ''}></div>${blocked ? '<p class="color-yellow text-xs">' + _L('정지 상태에서만 변경할 수 있습니다.', 'Rename requires the VM to be shut off.') + '</p>' : ''}<button class="btn btn-g" onclick="doVmRename()" ${blocked ? 'disabled' : ''}>${_L('이름 변경', 'Rename')}</button>`;
}
function hwCpu() { return `<h4>CPU</h4><div class="fr"><label>vCPU</label><input id="sc" type="number" value="${escapeHtml(String(vmList[selectedVmIndex]?.vcpu || 2))}"></div><button class="btn" onclick="doSet('vcpu')">${t('btn.apply')}</button>`; }
function hwMem() { return `<h4>Memory</h4><div class="fr"><label>MB</label><input id="sm" type="number" value="${escapeHtml(String(vmList[selectedVmIndex]?.memory_mb || 2048))}"></div><button class="btn" onclick="doSet('memory')">${t('btn.apply')}</button>`; }
function _currentCdromPath(v) {
  const p = String((v && (v.cdrom_path || v.iso_path)) || '').trim();
  return (!p || p === '(empty)' || p === 'N/A' || p === '-') ? '' : p;
}

function _setCdromInput(path) {
  const clean = String(path || '').trim();
  const input = document.getElementById('si');
  const current = document.getElementById('cdrom-current');
  if (input) input.value = clean;
  if (current) {
    current.innerHTML = clean
      ? '<span class="color-green">' + escapeHtml(clean) + '</span>'
      : '<span class="color-muted">' + _L('비어 있음', 'Empty') + '</span>';
  }
}

function hwCd() {
  const current = _currentCdromPath(vmList[selectedVmIndex]);
  return `<h4>CD/DVD</h4>`
    + `<div class="fr"><label>${_L('현재 ISO', 'Current ISO')}</label><div id="cdrom-current" class="flex-1">${current ? '<span class="color-green">' + escapeHtml(current) + '</span>' : '<span class="color-muted">' + _L('비어 있음', 'Empty') + '</span>'}</div></div>`
    + `<div class="fr"><label>ISO</label><div class="flex gap-6 flex-1"><input id="si" placeholder="ISO path..." class="flex-1" value="${escapeAttr(current)}"><button class="btn" onclick="browseISOForMount()">&#128194; Browse</button></div></div>`
    + `<button class="btn" onclick="doMnt()">Mount</button> <button class="btn btn-r" onclick="doEjt()">Eject</button>`;
}

function selectISOForMount(path) {
  closeModal();
  setTimeout(function() { _setCdromInput(path); }, 0);
}

async function browseISOForMount() {
  try { const r = await fetchGet(EP.ISO_LIST()); const il = unwrapList(r);
    let h = '<h2>&#128191; ' + t('iso.browser_title') + '</h2><div style="max-height:350px;overflow-y:auto;border:1px solid var(--border);border-radius:6px;background:var(--bg)">';
    if (il.length === 0) { h += '<div class="text-center color-muted" style="padding:16px">' + t('iso.not_found') + '</div>'; }
    else { const dirs = {}; il.forEach(iso => { const d = iso.dir || '/pcvpool/iso'; if (!dirs[d]) dirs[d] = []; dirs[d].push(iso); });
      Object.entries(dirs).forEach(([dir, files]) => {
        h += '<div style="padding:6px 10px;font-size:10px;color:var(--accent);border-bottom:1px solid var(--border);font-weight:600">&#128194; ' + escapeHtml(dir) + '</div>';
        files.forEach(iso => { const p = String(iso.path || iso.name || ''); const fn = (iso.name || p).replace(/^.*\//, ''); const sz = iso.size_human || '';
          h += '<div onclick="selectISOForMount(' + escapeAttr(JSON.stringify(p)) + ')" style="padding:7px 12px;cursor:pointer;font-size:12px;display:flex;align-items:center;gap:6px;border-bottom:1px solid var(--border)" onmouseover="this.style.background=\'var(--bg3)\'" onmouseout="this.style.background=\'\'">';
          h += '<span class="color-accent">&#128191;</span><span class="flex-1">' + escapeHtml(fn) + '</span><span class="color-muted text-xs">' + escapeHtml(sz) + '</span></div>'; }); }); }
    h += '</div><div class="text-right mt-10"><button class="btn btn-r" onclick="closeModal()">' + t('btn.cancel') + '</button></div>';
    showModal(h); } catch (e) { toast('ISO list error: ' + e.message, false); }
}

function hwNic() { const v = vmList[selectedVmIndex]; const html = `<h4>Network</h4><div id="nic-list">${t('loading')}</div><div class="mt-8"><div class="fr"><label>Bridge</label><input id="nic-br" value="pcvbr0"><button class="btn btn-g" onclick="nicAdd()">+ Add NIC</button></div></div>`; setTimeout(() => loadNics(v?.name), 50); return html; }
async function loadNics(n) { if (!n) return; try { const r = await fetchGet(EP.VM_NICS(n)); const l = unwrapList(r); let h = '<table><thead><tr><th>MAC</th><th>Bridge</th><th>Model</th><th>IP</th><th>DNS</th><th></th></tr></thead><tbody>'; l.forEach(c => { const dns = c.dns === 'off' ? 'OFF' : (c.dns || '-'); h += `<tr><td>${escapeHtml(c.mac || '-')}</td><td>${escapeHtml(c.bridge || c.source || '-')}</td><td>${escapeHtml(c.model || 'virtio')}</td><td>${escapeHtml(c.ip || '-')}</td><td>${escapeHtml(dns)}</td><td><button class="btn btn-r text-9" onclick="nicDel('${escapeAttr(n)}','${escapeAttr(c.mac)}')">${t('btn.delete')}</button></td></tr>`; }); h += '</tbody></table>'; const el = document.getElementById('nic-list'); if (el) el.innerHTML = l.length ? h : '<p class="color-muted">No NICs</p>'; } catch (e) { if(_DEBUG) console.warn('loadNics:', e.message); } }
async function nicAdd() { const v = vmList[selectedVmIndex]; if (!v) return; try { await fetchPost(EP.VM_NICS(v.name), { bridge: document.getElementById('nic-br')?.value || 'pcvbr0' }); toast(t('nic.added')); loadNics(v.name); } catch (e) { toast(e.message, false); } }
async function nicDel(n, mac) { if (!await customConfirm(t('btn.delete'), 'NIC ' + mac + '?')) return; try { await fetchDelete(EP.VM_NIC_DETACH(n, mac)); toast(t('nic.removed')); loadNics(n); } catch (e) { toast(e.message, false); } }

function hwAP() { const html = `<h4>AutoProtect (Backup Policy)</h4><div id="bp-list">${t('loading')}</div><div class="mt-8"><div class="fr"><label>VM</label><input id="bp-vm" value="${escapeHtml(vmList[selectedVmIndex]?.name || '*')}"></div><div class="fr"><label>Interval (h)</label><input id="bp-int" type="number" value="24"></div><div class="fr"><label>Retention</label><input id="bp-ret" type="number" value="7"></div><button class="btn btn-g" onclick="bpSet()">Set Policy</button></div>`; setTimeout(loadBP, 50); return html; }
async function loadBP() { try { const r = await fetchGet(EP.BACKUP_POLICIES()); const l = unwrapList(r); let h = '<table><thead><tr><th>VM</th><th>Interval</th><th>Retention</th></tr></thead><tbody>'; if (Array.isArray(l)) l.forEach(p => { h += `<tr><td>${escapeHtml(p.vm_name || p.name || '-')}</td><td>${escapeHtml(String(p.interval_hours || p.interval || '-'))}h</td><td>${escapeHtml(String(p.retention || '-'))}</td></tr>`; }); h += '</tbody></table>'; const el = document.getElementById('bp-list'); if (el) el.innerHTML = l.length ? h : '<p class="color-muted">No policies</p>'; } catch (e) { if(_DEBUG) console.warn('loadBP:', e.message); } }
async function bpSet() { try { await fetchPost(EP.BACKUP_POLICIES(), { vm_name: document.getElementById('bp-vm')?.value || '*', interval: +(document.getElementById('bp-int')?.value || 24), retention: +(document.getElementById('bp-ret')?.value || 7) }); toast(t('backup.policy_set')); loadBP(); } catch (e) { toast(e.message, false); } }

function showRenameVm() {
  const v = vmList[selectedVmIndex]; if (!v) return;
  const blocked = _vmRenameBlocked(v);
  showModal('<h2>' + _L('VM 이름 변경', 'Rename VM') + ': ' + escapeHtml(v.name) + '</h2>'
    + '<div class="fr"><label>' + _L('현재 이름', 'Current') + '</label><input value="' + escapeAttr(v.name) + '" disabled style="opacity:.65"></div>'
    + '<div class="fr"><label>' + _L('새 이름', 'New') + '</label><input id="rn-new" value="' + escapeAttr(v.name) + '" maxlength="64" ' + (blocked ? 'disabled' : '') + '></div>'
    + (blocked ? '<p class="color-yellow text-xs">' + _L('정지 상태에서만 변경할 수 있습니다.', 'Rename requires the VM to be shut off.') + '</p>' : '')
    + '<div id="rn-status" class="mt-8 text-xs color-muted"></div>'
    + '<div class="text-right mt-12"><button class="btn btn-r" onclick="closeModal()">' + t('btn.cancel') + '</button> <button class="btn btn-g" onclick="doVmRename()" ' + (blocked ? 'disabled' : '') + '>' + _L('이름 변경', 'Rename') + '</button></div>');
}

async function doVmRename() {
  const v = vmList[selectedVmIndex]; if (!v) return;
  const statusEl = document.getElementById('rn-status');
  const next = String(document.getElementById('rn-new')?.value || '').trim();
  if (!/^[A-Za-z0-9_-]{1,64}$/.test(next)) {
    toast(_L('VM 이름은 영문/숫자/하이픈/언더스코어만 사용할 수 있습니다.', 'VM name allows only letters, numbers, dash, underscore.'), false);
    return;
  }
  if (next === v.name) {
    toast(_L('새 이름이 현재 이름과 같습니다.', 'New name is the same as the current name.'), false);
    return;
  }
  if (_vmRenameBlocked(v)) {
    toast(_L('VM을 종료한 뒤 다시 시도하세요.', 'Shut off the VM and retry.'), false);
    return;
  }
  try {
    if (statusEl) statusEl.innerHTML = '<span class="spinner"></span> ' + _L('변경 중...', 'Renaming...');
    const r = await fetchPut(EP.VM_RENAME(v.name), { new_name: next });
    if (r && r.error) {
      const msg = r.error.message || t('error');
      if (statusEl) statusEl.innerHTML = '&#10060; ' + escapeHtml(msg);
      toast(msg, false);
      return;
    }
    const d = unwrapData(r);
    toast(_L('VM 이름이 변경되었습니다.', 'VM renamed.'));
    addEvt('VM renamed: ' + v.name + ' -> ' + next);
    if (typeof invalidateCache === 'function') invalidateCache('vm.list');
    selectedVmIndex = -1;
    closeModal();
    await loadAll();
    if (d && d.new_name && typeof render === 'function') render();
  } catch (e) {
    if (statusEl) statusEl.innerHTML = '&#10060; ' + escapeHtml(e.message || 'Failed');
    toast(e.message, false);
  }
}

async function doSet(t2) {
  const v = vmList[selectedVmIndex];
  if (!v) return;
  const nextValue = t2 === 'vcpu'
    ? +document.getElementById('sc').value
    : +document.getElementById('sm').value;
  const b = t2 === 'vcpu' ? { vcpu_count: nextValue } : { memory_mb: nextValue };
  try {
    await fetchPut(EP.VM_ACTION(v.name, t2), b);
    if (t2 === 'vcpu') v.vcpu = nextValue;
    if (t2 === 'memory') v.memory_mb = nextValue;
    toast(t2 + ' updated');
    addEvt('VM Hotplug — ' + v.name + ' ' + t2 + ' updated');
    if (typeof invalidateCache === 'function') invalidateCache('vm.list');
    await loadAll();
  } catch (e) {
    toast(e.message, false);
  }
}
async function doMnt() {
  const v = vmList[selectedVmIndex]; if (!v) return;
  const isoPath = String(document.getElementById('si')?.value || '').trim();
  if (!isoPath) { toast(t('iso.path_required'), false); return; }
  try {
    const r = await fetchPost(EP.VM_ISO(v.name), { iso_path: isoPath });
    if (r && r.error) { toast(r.error.message || t('error'), false); return; }
    const d = unwrapData(r);
    const mountedPath = d && d.iso_path ? d.iso_path : isoPath;
    v.cdrom_path = mountedPath;
    _setCdromInput(mountedPath);
    if (typeof invalidateCache === 'function') invalidateCache('vm.list');
    toast(t('iso.mounted'));
  } catch (e) { toast(e.message, false); }
}
async function doEjt() {
  const v = vmList[selectedVmIndex]; if (!v) return;
  try {
    const r = await fetchDelete(EP.VM_ISO(v.name));
    if (r && r.error) { toast(r.error.message || t('error'), false); return; }
    v.cdrom_path = '(empty)';
    _setCdromInput('');
    if (typeof invalidateCache === 'function') invalidateCache('vm.list');
    toast(t('iso.ejected'));
  } catch (e) { toast(e.message, false); }
}


function showSnap() { currentTab = 'snapshots'; document.querySelectorAll('#ct button').forEach(b => { b.classList.remove('active'); if (b.dataset.t === 'snapshots') b.classList.add('active'); }); renderContent(); }


async function showNicMgr() { const v = vmList[selectedVmIndex]; if (!v) return; showModal(`<h2>NIC: ${escapeHtml(v.name)}</h2><div id="nic-mgr">${t('loading')}</div><div class="mt-10"><div class="fr"><label>Bridge</label><input id="nm-br" value="pcvbr0"><button class="btn btn-g" onclick="nmAdd()">+ Add</button></div></div><div class="text-right mt-12"><button class="btn btn-r" onclick="closeModal()">${t('btn.close')}</button></div>`);
  try { const r = await fetchGet(EP.VM_NICS(v.name)); const l = unwrapList(r); let h = '<table><thead><tr><th>MAC</th><th>Bridge</th><th>Model</th><th>IP</th><th>DNS</th><th></th></tr></thead><tbody>'; l.forEach(c => { const dns = c.dns === 'off' ? 'OFF' : (c.dns || '-'); h += `<tr><td>${escapeHtml(c.mac || '-')}</td><td>${escapeHtml(c.bridge || c.source || '-')}</td><td>${escapeHtml(c.model || 'virtio')}</td><td>${escapeHtml(c.ip || '-')}</td><td>${escapeHtml(dns)}</td><td><button class="btn btn-r text-9" onclick="nmDel('${escapeAttr(c.mac)}')">${t('btn.delete')}</button></td></tr>`; }); document.getElementById('nic-mgr').innerHTML = l.length ? h + '</tbody></table>' : '<p class="color-muted">No NICs</p>'; } catch (e) { document.getElementById('nic-mgr').innerHTML = t('error'); } }
async function nmAdd() { const v = vmList[selectedVmIndex]; if (!v) return; try { await fetchPost(EP.VM_NICS(v.name), { bridge: document.getElementById('nm-br')?.value || 'pcvbr0' }); toast(t('nic.added')); showNicMgr(); } catch (e) { toast(e.message, false); } }
async function nmDel(mac) { const v = vmList[selectedVmIndex]; if (!v || !await customConfirm(t('btn.delete'), mac + '?')) return; try { await fetchDelete(EP.VM_NIC_DETACH(v.name, mac)); toast(t('nic.removed')); showNicMgr(); } catch (e) { toast(e.message, false); } }


async function showVnc() { const v = vmList[selectedVmIndex]; if (!v) return; showModal(`<h2>VNC: ${escapeHtml(v.name)}</h2><div id="vnc-info">${t('loading')}</div><div class="text-right mt-12"><button class="btn btn-r" onclick="closeModal()">${t('btn.close')}</button></div>`);
  try { const r = await fetchGet(EP.VNC(v.name)); const d = unwrapData(r); const stBadge = v.state === 'running' ? H.badge('Available', 'g') : H.badge('VM stopped', 'r'); document.getElementById('vnc-info').innerHTML = H.card('', H.row('Address', escapeHtml(d.vnc_address || d.address || 'localhost')) + H.row('Port', escapeHtml(String(d.vnc_port || d.port || '-'))) + H.row('Status', stBadge)); } catch (e) { document.getElementById('vnc-info').innerHTML = H.card('', '<p class="color-muted">VNC info unavailable</p>'); } }


function _vmCloneStorageKind(v) {
  const st = String(v?.storage_type || '').toLowerCase();
  const fmt = String(v?.disk_format || '').toLowerCase();
  const path = String(v?.disk_path || '');
  const lowerPath = path.toLowerCase();

  if (st === 'zvol' || path.indexOf('/dev/zvol/') === 0) return 'zvol';
  if (st === 'qcow2' || st === 'raw' || fmt === 'qcow2' || fmt === 'raw' ||
      lowerPath.endsWith('.qcow2') || lowerPath.endsWith('.raw') ||
      lowerPath.endsWith('.img')) return 'file';
  return 'unknown';
}

function _vmCloneIsPoweredOn(v) {
  const state = String(v?.state || '').toLowerCase();
  return state === 'running' || state === 'paused' || state === 'blocked' ||
    state === 'pmsuspended' || state === 'shutdown';
}

function _vmCloneGuard(v, mode) {
  const kind = _vmCloneStorageKind(v);
  if (_vmCloneIsPoweredOn(v)) {
    return {
      ok: false,
      message: _L('Power on 상태에서는 clone을 사용할 수 없습니다. 원본 VM을 종료한 뒤 재시도하세요.', 'Clone is unavailable while the source VM is powered on. Shut it off and retry.')
    };
  }
  if (kind === 'file' && mode !== 'full') {
    return {
      ok: false,
      message: _L('qcow2/raw 파일 디스크는 Full clone만 지원합니다.', 'qcow2/raw file disks only support Full clone.')
    };
  }
  if (kind === 'file') {
    return {
      ok: true,
      message: _L('파일 디스크는 Full clone으로 실행됩니다.', 'File disk clone will run in Full mode.')
    };
  }
  if (kind === 'zvol') {
    return {
      ok: true,
      message: _L('ZFS zvol은 CoW 또는 Full clone을 사용할 수 있습니다.', 'ZFS zvol supports CoW or Full clone.')
    };
  }
  return {
    ok: true,
    message: _L('디스크 타입은 요청 시 백엔드에서 검증됩니다.', 'Disk type will be validated by the backend.')
  };
}

function _vmCloneModeHelp(kind) {
  const cowSuffix = kind === 'file'
    ? _L('파일 디스크에서는 사용할 수 없음', 'Unavailable for file disks')
    : _L('ZFS zvol 전용', 'ZFS zvol only');
  return '<div class="vm-clone-choice-grid">'
    + '<label class="vm-clone-choice" data-vm-clone-mode-choice="full">'
    + '<input type="radio" name="vm-clone-mode-choice" value="full">'
    + '<span class="vm-clone-choice-head"><span>Full</span><span class="badge b-g">' + _L('독립', 'Independent') + '</span></span>'
    + '<span class="vm-clone-choice-title">' + _L('디스크 전체 복제', 'Full disk copy') + '</span>'
    + '<span class="vm-clone-choice-copy">' + _L('원본 snapshot/origin에 의존하지 않습니다. 시간이 더 걸리고 용량을 더 사용합니다.', 'No source snapshot/origin dependency. Takes longer and uses more storage.') + '</span>'
    + '</label>'
    + '<label class="vm-clone-choice" data-vm-clone-mode-choice="cow">'
    + '<input type="radio" name="vm-clone-mode-choice" value="cow">'
    + '<span class="vm-clone-choice-head"><span>CoW</span><span class="badge b-y">' + escapeHtml(cowSuffix) + '</span></span>'
    + '<span class="vm-clone-choice-title">' + _L('빠른 snapshot 복제', 'Fast snapshot clone') + '</span>'
    + '<span class="vm-clone-choice-copy">' + _L('생성이 빠르고 공간 사용이 적습니다. 원본 snapshot/origin 의존성이 남습니다.', 'Fast and space-efficient. Keeps a source snapshot/origin dependency.') + '</span>'
    + '</label>'
    + '</div>';
}

function _vmCloneSafetyHelp() {
  return '<div class="vm-clone-choice-grid">'
    + '<label class="vm-clone-choice" data-vm-clone-safety-choice="guest-reset">'
    + '<input name="vm-clone-safety" value="guest-reset" type="radio" checked>'
    + '<span class="vm-clone-choice-head"><span>Guest reset</span><span class="badge b-y">libguestfs-tools</span></span>'
    + '<span class="vm-clone-choice-title">' + _L('일반 VM 복제', 'Normal VM clone') + '</span>'
    + '<span class="vm-clone-choice-copy">' + _L('운영 서버에 virt-sysprep, virt-customize, virt-filesystems, guestfish가 있어야 합니다. target guest identity를 새 VM 기준으로 재설정합니다.', 'Requires virt-sysprep, virt-customize, virt-filesystems, and guestfish on the host. Resets target guest identity for the new VM.') + '</span>'
    + '</label>'
    + '<label class="vm-clone-choice" data-vm-clone-safety-choice="template">'
    + '<input name="vm-clone-safety" value="template" type="radio">'
    + '<span class="vm-clone-choice-head"><span>Prepared template</span><span class="badge b-g">' + _L('도구 불필요', 'No tools') + '</span></span>'
    + '<span class="vm-clone-choice-title">' + _L('정리된 템플릿 전용', 'Prepared templates only') + '</span>'
    + '<span class="vm-clone-choice-copy">' + _L('이미 게스트 식별자를 정리한 VM에만 사용합니다. guest reset을 건너뛰므로 중복 식별자가 없다는 책임은 운영자에게 있습니다.', 'Use only when guest identities are already cleaned. Skips guest reset, so the operator is responsible for avoiding duplicate identities.') + '</span>'
    + '</label>'
    + '</div>';
}

function _vmCloneRefreshChoiceCards(groupName, dataAttr) {
  document.querySelectorAll('[' + dataAttr + ']').forEach(card => {
    const input = card.querySelector('input[name="' + groupName + '"]');
    if (!input) return;
    card.classList.toggle('active', input.checked);
    card.classList.toggle('disabled', input.disabled);
    card.setAttribute('aria-checked', input.checked ? 'true' : 'false');
    card.setAttribute('aria-disabled', input.disabled ? 'true' : 'false');
  });
}

function _vmCloneRefreshGuard(v) {
  const modeEl = document.getElementById('vm-clone-mode');
  const guardEl = document.getElementById('vm-clone-guard');
  const submitEl = document.getElementById('vm-clone-submit');

  const kind = _vmCloneStorageKind(v);
  const fullInput = document.querySelector('input[name="vm-clone-mode-choice"][value="full"]');
  const cowInput = document.querySelector('input[name="vm-clone-mode-choice"][value="cow"]');
  if (cowInput) cowInput.disabled = kind === 'file';
  if (kind === 'file' && cowInput && cowInput.checked && fullInput) fullInput.checked = true;
  const selectedMode = document.querySelector('input[name="vm-clone-mode-choice"]:checked')?.value ||
    (kind === 'zvol' ? 'cow' : 'full');
  if (modeEl) modeEl.value = selectedMode;
  _vmCloneRefreshChoiceCards('vm-clone-mode-choice', 'data-vm-clone-mode-choice');
  _vmCloneRefreshChoiceCards('vm-clone-safety', 'data-vm-clone-safety-choice');

  const guard = _vmCloneGuard(v, selectedMode);
  if (guardEl) {
    guardEl.className = 'vm-clone-guard ' + (guard.ok ? 'ok' : 'blocked');
    guardEl.textContent = guard.message;
  }
  if (submitEl) submitEl.disabled = !guard.ok;
}

function _vmCloneFriendlyError(message) {
  const raw = String(message || '');
  const lower = raw.toLowerCase();
  if (lower.includes('guest reset requires libguestfs-tools')) {
    return _L(
      'Guest reset에는 운영 서버의 libguestfs-tools가 필요합니다. 현재 서버에서 virt-sysprep, virt-customize, virt-filesystems, guestfish를 사용할 수 없습니다. 준비된 템플릿 VM이면 Prepared template을 선택하고, 일반 VM이면 서버에 libguestfs-tools를 설치한 뒤 다시 시도하세요.',
      'Guest reset requires libguestfs-tools on the host. virt-sysprep, virt-customize, virt-filesystems, and guestfish are unavailable. Select Prepared template only for an already prepared template VM, or install libguestfs-tools for a normal VM clone.'
    );
  }
  if (lower.includes('requires the source vm to be shut off')) {
    return _L('원본 VM이 실행 중입니다. VM을 종료한 뒤 clone을 다시 시도하세요.', 'The source VM is running. Shut it off and retry clone.');
  }
  return raw || t('error');
}

function _vmCloneShortError(message) {
  const lower = String(message || '').toLowerCase();
  if (lower.includes('guest reset requires libguestfs-tools')) {
    return _L('Guest reset 도구가 없습니다. Prepared template 선택 또는 libguestfs-tools 설치가 필요합니다.', 'Guest reset tools are missing. Select Prepared template or install libguestfs-tools.');
  }
  if (lower.includes('requires the source vm to be shut off')) {
    return _L('원본 VM을 종료한 뒤 다시 시도하세요.', 'Shut off the source VM and retry.');
  }
  return _vmCloneFriendlyError(message);
}

function _vmCloneShowError(message) {
  const friendly = _vmCloneFriendlyError(message);
  const guardEl = document.getElementById('vm-clone-guard');
  if (guardEl) {
    guardEl.className = 'vm-clone-guard blocked';
    guardEl.textContent = friendly;
  }
  toast(_vmCloneShortError(message), false);
}

async function vmClone(idx) {
  const actualIdx = (idx === 0 || idx) ? idx : selectedVmIndex;
  const v = vmList[actualIdx]; if (!v) return;
  selectedVmIndex = actualIdx;
  const suggested = (v.name || 'vm') + '-clone';
  const kind = _vmCloneStorageKind(v);
  const defaultMode = kind === 'zvol' ? 'cow' : 'full';
  showModal('<h2>' + _L('VM 복제', 'Clone VM') + ': ' + escapeHtml(v.name) + '</h2>'
    + '<div class="fr"><label>' + _L('새 VM 이름', 'New VM name') + '</label><input id="vm-clone-name" class="input-field" value="' + escapeAttr(suggested) + '"></div>'
    + '<input id="vm-clone-mode" type="hidden" value="' + escapeAttr(defaultMode) + '">'
    + '<div class="fr"><label>' + _L('복제 방식', 'Clone mode') + '</label><div class="flex-1">'
    + _vmCloneModeHelp(kind) + '</div></div>'
    + '<div class="fr"><label>' + _L('안전 처리', 'Safety') + '</label><div class="flex-1">'
    + _vmCloneSafetyHelp() + '</div></div>'
    + '<div id="vm-clone-guard" class="vm-clone-guard" role="status"></div>'
    + '<div class="text-right mt-12"><button class="btn" onclick="closeModal()">' + t('btn.cancel') + '</button> '
    + '<button id="vm-clone-submit" class="btn btn-g" onclick="doVmClone()">' + _L('복제', 'Clone') + '</button></div>');
  const defaultModeInput = document.querySelector('input[name="vm-clone-mode-choice"][value="' + defaultMode + '"]');
  if (defaultModeInput) defaultModeInput.checked = true;
  document.querySelectorAll('input[name="vm-clone-mode-choice"], input[name="vm-clone-safety"]').forEach(el => {
    el.addEventListener('change', () => _vmCloneRefreshGuard(v));
  });
  _vmCloneRefreshGuard(v);
}
async function doVmClone() {
  const v = vmList[selectedVmIndex]; if (!v) return;
  const name = (document.getElementById('vm-clone-name')?.value || '').trim();
  const mode = document.querySelector('input[name="vm-clone-mode-choice"]:checked')?.value ||
    document.getElementById('vm-clone-mode')?.value ||
    (_vmCloneStorageKind(v) === 'zvol' ? 'cow' : 'full');
  const safety = document.querySelector('input[name="vm-clone-safety"]:checked')?.value || 'guest-reset';
  const prepared = safety === 'template';
  if (!name || !/^[a-zA-Z0-9_-]{1,63}$/.test(name)) {
    toast(_L('VM 이름: 1-63자, 영문/숫자/_- 만 허용', 'VM name: 1-63 chars, [a-zA-Z0-9_-]'), false);
    return;
  }
  if (vmList.some(vm => vm && vm.name === name)) {
    toast(_L('대상 VM 이름이 이미 존재합니다.', 'Target VM name already exists.'), false);
    return;
  }
  const guard = _vmCloneGuard(v, mode);
  if (!guard.ok) {
    toast(guard.message, false);
    return;
  }
  const body = { new_name: name, mode: mode, guest_reset: !prepared };
  if (prepared) body.template_prepared = true;
  try {
    const r = await fetchPost(EP.VM_CLONE(v.name), body);
    if (r && r.error) { _vmCloneShowError(r.error.message || t('error')); return; }
    const d = unwrapData(r);
    toast(_L('복제 시작됨', 'Clone accepted') + ': ' + escapeHtml(name));
    addEvt('VM clone — ' + v.name + ' → ' + name + (d.job_id ? ' (' + d.job_id + ')' : ''));
    closeModal();
  } catch (e) { toast(e.message, false); }
}


function hwDisk() {
  return '<h4>&#128190; Disk Resize</h4><div class="fr"><label>New Size (GB)</label><input id="sd-size" type="number" value="40" placeholder="40"></div><div class="fr"><label>Disk Path</label><input id="sd-path" placeholder="vda (optional)"></div><button class="btn btn-g" onclick="doDiskResize()">' + t('btn.apply') + '</button><p class="stat-label mt-8">Live disk resize (qemu-img resize). VM can be running.</p>';
}
async function doDiskResize() {
  const v = vmList[selectedVmIndex]; if (!v) return;
  const size = document.getElementById('sd-size')?.value;
  const path = document.getElementById('sd-path')?.value;
  const body = { size_gb: parseInt(size) || 40 };
  if (path) body.disk_path = path;
  try {
    const r = await fetchPut(EP.VM_DISK(v.name), body);
    if (r.error) { toast('Resize failed: ' + (r.error.message || ''), false); return; }
    toast('Disk resized: ' + v.name); addEvt('Disk resize: ' + v.name + ' → ' + size + 'GB');
  } catch (e) { toast('Resize error: ' + e.message, false); }
}


async function vmDeleteStatus(name) {
  try {
    const r = await fetchGet(EP.VM_DELETE_STATUS(name));
    const d = unwrapData(r);
    return d.status || 'unknown';
  } catch (e) { return 'unknown'; }
}


async function vmExportOva(idx) {
  const v = vmList[idx ?? selectedVmIndex]; if (!v) return;
  if (!await customConfirm('Export OVA', _L('VM을 OVA 파일로 내보내시겠습니까?', 'Export ' + v.name + ' as OVA file?') + '\n' + v.name)) return;
  showModal('<h2>&#128230; Export OVA</h2><p class="mb-8"><b class="color-accent">' + escapeHtml(v.name) + '</b></p><div class="prog-bar"><div class="prog-fill" id="ova-p" class="w-pct-10"></div></div><div class="prog-status" id="ova-s"><span class="spinner"></span> ' + _L('내보내기 시작 중...', 'Starting export...') + '</div>');
  try {
    var pf = document.getElementById('ova-p'), ps = document.getElementById('ova-s');
    pf.style.width = '30%'; ps.innerHTML = '<span class="spinner"></span> ' + _L('OVA 변환 요청 중...', 'Requesting OVA conversion...');
    var r = await fetchPost(EP.VM_EXPORT(v.name), {});
    if (r.error) { pf.style.background = 'var(--red)'; pf.style.width = '100%'; ps.innerHTML = '&#10060; ' + escapeHtml(r.error.message || 'Export failed'); return; }
    pf.style.width = '70%'; ps.innerHTML = '<span class="spinner"></span> ' + _L('변환 진행 중...', 'Converting...');
    var d = unwrapData(r) || r;
    var path = d.path || d.ova_path || '';

    for (var pi = 0; pi < 5; pi++) {
      await new Promise(function(res) { setTimeout(res, 2000); });
      pf.style.width = (75 + pi * 5) + '%';
      try { var st = await fetchGet(EP.VM_DETAIL(v.name) + '/export-status'); var sd = unwrapData(st) || st; if (sd.status === 'done' || sd.status === 'completed') break; } catch(e) { break; }
    }
    pf.style.width = '100%'; pf.style.background = 'var(--green)';
    ps.innerHTML = '&#9989; ' + _L('내보내기 완료', 'Export completed') + (path ? '<br><span class="text-xs color-muted">' + escapeHtml(path) + '</span>' : '');
    toast('&#128230; ' + v.name + ' OVA ' + _L('내보내기 완료', 'export completed'));
    addEvt('OVA export: ' + v.name + (path ? ' → ' + path : ''));
  } catch (e) {
    var pf2 = document.getElementById('ova-p'), ps2 = document.getElementById('ova-s');
    if (pf2) { pf2.style.background = 'var(--red)'; pf2.style.width = '100%'; }
    if (ps2) ps2.innerHTML = '&#10060; ' + escapeHtml(e.message);
    toast(e.message, false);
  }
}


function hwCpuPin() {
  return '<h4>&#128204; CPU Pinning</h4><p class="stat-label mb-8">Pin vCPUs to physical cores for performance isolation.</p><div class="fr"><label>vCPU Map</label><input id="scpin" placeholder="0:0,1:2,2:4" class="flex-1"></div><p class="stat-label">Format: vCPU:pCPU pairs, comma separated (e.g., 0:0,1:2)</p><button class="btn btn-g mt-8" onclick="doCpuPin()">' + t('btn.apply') + '</button>';
}

async function doCpuPin() {
  const v = vmList[selectedVmIndex]; if (!v) return;
  const map = document.getElementById('scpin')?.value;
  if (!map) { toast('vCPU pin map required', false); return; }
  try {
    const r = await fetchPut(EP.VM_CPU_PIN(v.name), { vcpu_pin: map });
    if (r.error) { toast('CPU pin failed: ' + (r.error.message || ''), false); return; }
    toast('CPU pinning applied: ' + v.name); addEvt('CPU pin: ' + v.name);
  } catch (e) { toast(e.message, false); }
}


function hwBandwidth() {
  return '<h4>&#128246; Network Bandwidth (QoS)</h4><p class="stat-label mb-8">Set network bandwidth limits for VM interfaces.</p><div class="fr"><label>Inbound (Mbps)</label><input id="sbw-in" type="number" value="1000" placeholder="1000"></div><div class="fr"><label>Outbound (Mbps)</label><input id="sbw-out" type="number" value="1000" placeholder="1000"></div><div class="fr"><label>Burst (KB)</label><input id="sbw-burst" type="number" value="1024" placeholder="1024"></div><button class="btn btn-g mt-8" onclick="doBandwidth()">' + t('btn.apply') + '</button>';
}

async function doBandwidth() {
  const v = vmList[selectedVmIndex]; if (!v) return;
  try {
    const r = await fetchPut(EP.VM_BANDWIDTH(v.name), {
      inbound_avg: parseInt(document.getElementById('sbw-in')?.value) || 1000,
      outbound_avg: parseInt(document.getElementById('sbw-out')?.value) || 1000,
      burst: parseInt(document.getElementById('sbw-burst')?.value) || 1024
    });
    if (r.error) { toast('Bandwidth set failed: ' + (r.error.message || ''), false); return; }
    toast('Bandwidth QoS applied: ' + v.name); addEvt('Bandwidth: ' + v.name);
  } catch (e) { toast(e.message, false); }
}


async function showMemStats() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var h = '<h2>&#128204; Memory Stats: ' + esc(v.name) + '</h2>';
  h += '<div id="mem-stats-body"><span class="spinner"></span> ' + t('loading') + '</div>';
  h += '<div class="text-right mt-12"><button class="btn" onclick="closeModal()">' + t('btn.close') + '</button></div>';
  showModal(h);
  try {
    var r = await fetchPost(EP.VM_RPC(v.name), { method: 'vm.memory.stats', params: { name: v.name } });
    var d = unwrapData(r);
    var el = document.getElementById('mem-stats-body'); if (!el) return;
    var fmtKb = function(kb) {
      if (!kb && kb !== 0) return '-';
      if (kb >= 1048576) return (kb / 1048576).toFixed(2) + ' GB';
      if (kb >= 1024) return (kb / 1024).toFixed(1) + ' MB';
      return kb + ' KB';
    };
    var sh = '<div style="border:1px solid var(--border);border-radius:6px;padding:12px">';
    sh += '<div style="display:grid;grid-template-columns:1fr 1fr;gap:8px 16px;font-size:12px">';
    sh += '<div>' + H.row('Actual Balloon', '<span class="color-accent">' + fmtKb(d.actual_balloon_kb || d.actual) + '</span>') + '</div>';
    sh += '<div>' + H.row('RSS', '<span class="color-green">' + fmtKb(d.rss_kb || d.rss) + '</span>') + '</div>';
    sh += '<div>' + H.row('Unused', '<span class="color-muted">' + fmtKb(d.unused_kb || d.unused) + '</span>') + '</div>';
    sh += '<div>' + H.row('Available', '<span class="color-cyan">' + fmtKb(d.available_kb || d.available) + '</span>') + '</div>';
    sh += '<div>' + H.row('Swap In', fmtKb(d.swap_in_kb || d.swap_in || 0)) + '</div>';
    sh += '<div>' + H.row('Swap Out', fmtKb(d.swap_out_kb || d.swap_out || 0)) + '</div>';
    sh += '<div>' + H.row('Major Fault', String(d.major_fault || d.majflt || 0)) + '</div>';
    sh += '<div>' + H.row('Minor Fault', String(d.minor_fault || d.minflt || 0)) + '</div>';
    sh += '</div></div>';
    el.innerHTML = sh;
  } catch (e) {
    var el = document.getElementById('mem-stats-body');
    if (el) el.innerHTML = '<p class="color-red">Failed: ' + esc(e.message) + '</p>';
  }
}


async function showCpuStats() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var h = '<h2>&#9881; CPU Stats: ' + esc(v.name) + '</h2>';
  h += '<div id="cpu-stats-body"><span class="spinner"></span> ' + t('loading') + '</div>';
  h += '<div class="text-right mt-12"><button class="btn" onclick="closeModal()">' + t('btn.close') + '</button></div>';
  showModal(h);
  try {
    var r = await fetchPost(EP.VM_RPC(v.name), { method: 'vm.cpu.stats', params: { name: v.name } });
    var d = unwrapData(r);
    var el = document.getElementById('cpu-stats-body'); if (!el) return;
    var vcpuCount = d.vcpu_count || d.vcpu || v.vcpu || 0;
    var maxVcpu = d.max_vcpu || d.max || vcpuCount;
    var sh = '<div class="mb-12">';
    sh += H.row('vCPU Count', '<span class="color-accent">' + vcpuCount + '</span>');
    sh += H.row('Max vCPU', '<span class="color-muted">' + maxVcpu + '</span>');
    sh += H.row('CPU Time (ns)', '<span class="color-green">' + (d.cpu_time || 0) + '</span>');
    sh += '</div>';
    var vcpus = d.vcpus || d.vcpu_list || [];
    if (vcpus.length > 0) {
      sh += '<table><thead><tr><th>vCPU</th><th>State</th><th>CPU Time (ns)</th><th>Physical CPU</th></tr></thead><tbody>';
      vcpus.forEach(function(vc, i) {
        var state = vc.state === 1 || vc.state === 'running' ? '<span class="color-green">Running</span>' : '<span class="color-muted">Offline</span>';
        sh += '<tr><td><b>' + (vc.number !== undefined ? vc.number : i) + '</b></td>';
        sh += '<td>' + state + '</td>';
        sh += '<td>' + (vc.cpu_time || 0) + '</td>';
        sh += '<td>' + (vc.cpu !== undefined ? vc.cpu : '-') + '</td></tr>';
      });
      sh += '</tbody></table>';
    } else {
      sh += '<p class="color-muted text-12">Detailed per-vCPU info not available (VM may be stopped)</p>';
    }
    el.innerHTML = sh;
  } catch (e) {
    var el = document.getElementById('cpu-stats-body');
    if (el) el.innerHTML = '<p class="color-red">Failed: ' + esc(e.message) + '</p>';
  }
}


function showDiskLiveResize() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var h = '<h2>&#128190; Disk Live Resize: ' + esc(v.name) + '</h2>';
  h += '<p class="stat-label mb-12">Resize a VM disk while the VM is running. The guest OS may need to rescan partitions.</p>';
  h += '<div class="fr"><label>Target Device</label><input id="dlr-target" value="vda" placeholder="vda" class="w-120"></div>';
  h += '<div class="fr"><label>New Size (GB)</label><input id="dlr-size" type="number" value="40" min="1" placeholder="40" class="w-120"></div>';
  h += '<div class="text-right mt-14">';
  h += '<button class="btn btn-g" onclick="doDiskLiveResize()">&#128190; Resize</button> ';
  h += '<button class="btn" onclick="closeModal()">' + t('btn.cancel') + '</button>';
  h += '</div>';
  showModal(h);
}

async function doDiskLiveResize() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var target = (document.getElementById('dlr-target')?.value || 'vda').trim();
  var size = parseInt(document.getElementById('dlr-size')?.value) || 0;
  if (size < 1) { toast('Size must be at least 1 GB', false); return; }
  try {
    var r = await fetchPost(EP.VM_DISK_RESIZE(v.name), { target: target, new_size_gb: size });
    if (r.error) { toast('Resize failed: ' + (r.error.message || ''), false); return; }
    toast('Disk resized: ' + v.name + ' ' + target + ' -> ' + size + ' GB');
    addEvt('Disk live resize: ' + v.name + ' ' + target + ' -> ' + size + 'GB');
    closeModal();
  } catch (e) { toast('Resize error: ' + e.message, false); }
}


function _vmDiskUsagePct(fs) {
  if (!fs) return null;
  if (fs.usage_percent !== undefined) return Number(fs.usage_percent);
  var total = Number(fs.total_bytes || 0);
  var used = Number(fs.used_bytes || 0);
  return total > 0 ? (used * 100 / total) : null;
}

function _vmDiskUsageBar(pct) {
  if (pct === null || isNaN(pct)) return '<span class="color-muted">-</span>';
  return '<div style="min-width:120px">' + renderProgressBar(Math.max(0, Math.min(100, pct))) + '<div class="text-xs color-muted mt-4">' + pct.toFixed(1) + '%</div></div>';
}

function _vmDiskUsageSeverity(pct) {
  if (pct === null || isNaN(pct)) return 'y';
  if (pct >= 90) return 'r';
  if (pct >= 80) return 'y';
  return 'g';
}

function _vmRenderDiskUsage(d) {
  var filesystems = Array.isArray(d.filesystems) ? d.filesystems : [];
  var total = Number(d.total_bytes || 0);
  var used = Number(d.used_bytes || 0);
  var pct = d.usage_percent !== undefined ? Number(d.usage_percent) : (total > 0 ? used * 100 / total : null);
  var h = '<div class="mb-12">';
  h += '<div class="sg grid-3">';
  h += H.card(_L('전체 사용량', 'Total Usage'), H.row(_L('사용', 'Used'), used ? formatBytes(used) : '-') + H.row(_L('전체', 'Total'), total ? formatBytes(total) : '-') + H.row(_L('상태', 'Status'), H.badge(pct === null ? _L('알 수 없음', 'Unknown') : pct.toFixed(1) + '%', _vmDiskUsageSeverity(pct))) + _vmDiskUsageBar(pct));
  h += H.card(_L('마운트', 'Mounts'), H.row(_L('파일시스템', 'Filesystems'), String(filesystems.length)) + H.row(_L('수집 방식', 'Source'), 'qemu-guest-agent') + H.row(_L('대상', 'Target'), escapeHtml(d.name || '-')));
  h += '</div></div>';

  if (!filesystems.length) {
    return h + '<p class="color-muted text-12">' + _L('게스트 파일시스템 정보가 없습니다.', 'No guest filesystem data returned.') + '</p>';
  }

  filesystems.sort(function(a, b) {
    var am = a.mountpoint || '';
    var bm = b.mountpoint || '';
    if (am === '/') return -1;
    if (bm === '/') return 1;
    return am.localeCompare(bm);
  });

  h += '<table><thead><tr>'
    + '<th>' + _L('마운트', 'Mount') + '</th>'
    + '<th>' + _L('타입', 'Type') + '</th>'
    + '<th>' + _L('사용', 'Used') + '</th>'
    + '<th>' + _L('전체', 'Total') + '</th>'
    + '<th>' + _L('사용률', 'Usage') + '</th>'
    + '</tr></thead><tbody>';
  filesystems.forEach(function(fs) {
    var fsPct = _vmDiskUsagePct(fs);
    var fsUsed = Number(fs.used_bytes || 0);
    var fsTotal = Number(fs.total_bytes || 0);
    h += '<tr>';
    h += '<td><b>' + escapeHtml(fs.mountpoint || '-') + '</b><div class="text-xs color-muted">' + escapeHtml(fs.name || fs.device || '') + '</div></td>';
    h += '<td>' + escapeHtml(fs.type || '-') + '</td>';
    h += '<td>' + (fsUsed ? formatBytes(fsUsed) : '-') + '</td>';
    h += '<td>' + (fsTotal ? formatBytes(fsTotal) : '-') + '</td>';
    h += '<td>' + _vmDiskUsageBar(fsPct) + '</td>';
    h += '</tr>';
  });
  h += '</tbody></table>';
  return h;
}

async function showVmDiskUsage() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var running = v.state === 'running';
  var h = '<h2>&#128202; ' + _L('디스크 사용량', 'Disk Usage') + ': ' + esc(v.name) + '</h2>';
  h += '<div id="vm-disk-usage-body" style="min-height:90px"><span class="spinner"></span> ' + t('loading') + '</div>';
  h += '<div class="text-right mt-14"><button class="btn" onclick="closeModal()">' + t('btn.close') + '</button></div>';
  showModal(h);

  var body = document.getElementById('vm-disk-usage-body');
  if (!running) {
    if (body) body.innerHTML = '<p class="color-muted">' + _L('게스트 파일시스템 사용량은 VM 실행 중에만 조회할 수 있습니다.', 'Guest filesystem usage is available only while the VM is running.') + '</p>';
    return;
  }

  try {
    var r = await fetchGet(EP.VM_DISK_USAGE(v.name));
    if (r.error) throw new Error(r.error.message || 'disk usage failed');
    var d = unwrapData(r) || {};
    if (body) body.innerHTML = _vmRenderDiskUsage(d);
  } catch (e) {
    if (body) body.innerHTML = '<p class="color-red">' + esc(e.message) + '</p>'
      + '<p class="color-muted text-xs mt-8">' + _L('qemu-guest-agent 채널과 게스트 내부 에이전트 상태를 확인하세요.', 'Check the qemu-guest-agent channel and guest agent status.') + '</p>'
      + '<button class="btn btn-g mt-8" onclick="closeModal();showGuestAgent()">&#128172; Guest Agent</button>';
  }
}


var _gaInstallCommands = {};

function showGuestAgent() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var h = '<h2>&#128172; Guest Agent: ' + esc(v.name) + '</h2>';
  h += '<div class="mb-14 p-10 border-muted rounded-md">';
  h += '<div style="display:flex;gap:8px;flex-wrap:wrap;margin-bottom:10px">';
  h += '<button class="btn" onclick="gaRefreshStatus()">&#8635; Status</button>';
  h += '<button class="btn btn-g" onclick="gaEnsureChannel()">Channel</button>';
  h += '<button class="btn btn-g" onclick="gaPing()">&#128994; Ping</button>';
  h += '<button class="btn btn-r" onclick="gaShutdown()">&#9888; Graceful Shutdown</button>';
  h += '</div>';
  h += '<div id="ga-status-body" style="font-size:12px;min-height:48px;margin-bottom:10px"><span class="spinner"></span> Checking...</div>';
  h += '<div id="ga-ping-result" style="font-size:12px;min-height:20px;margin-bottom:8px"></div>';
  h += '</div>';

  h += '<div class="mb-14 p-10 border-muted rounded-md">';
  h += '<h4 class="mb-8">Install qemu-guest-agent</h4>';
  h += '<div id="ga-install-body" class="text-12 color-muted"></div>';
  h += '</div>';

  h += '<div class="mb-14 p-10 border-muted rounded-md">';
  h += '<h4 class="mb-8">&#128187; Execute Command</h4>';
  h += '<div class="fr"><label>Command</label><input id="ga-cmd" placeholder="cat /etc/hostname" class="flex-1"></div>';
  h += '<div class="fr"><label>Args</label><input id="ga-args" placeholder="(optional, space separated)" class="flex-1"></div>';
  h += '<button class="btn btn-g" onclick="gaExec()" style="margin-top:6px">&#9654; Execute</button>';
  h += '<div id="ga-exec-result" style="margin-top:10px;background:var(--bg);border:1px solid var(--border);border-radius:6px;padding:10px;max-height:250px;overflow:auto;font-size:11px;font-family:var(--font-mono);white-space:pre-wrap;display:none"></div>';
  h += '</div>';

  h += '<div class="text-right"><button class="btn" onclick="closeModal()">' + t('btn.close') + '</button></div>';
  showModal(h);
  setTimeout(gaRefreshStatus, 20);
}

function gaCommand(key) {
  if (_gaInstallCommands && _gaInstallCommands[key]) return _gaInstallCommands[key];
  if (key === 'rhel_rocky_fedora') return 'sudo dnf install -y qemu-guest-agent && sudo systemctl enable --now qemu-guest-agent';
  if (key === 'suse') return 'sudo zypper install -y qemu-guest-agent && sudo systemctl enable --now qemu-guest-agent';
  return 'sudo apt update && sudo apt install -y qemu-guest-agent && sudo systemctl enable --now qemu-guest-agent';
}

function gaStatusBadge(status) {
  if (status === 'ok') return H.badge('OK', 'g');
  if (status === 'vm_stopped') return H.badge('Stopped', 'y');
  if (status === 'reboot_required') return H.badge('Reboot needed', 'y');
  if (status === 'agent_unavailable') return H.badge('Install needed', 'y');
  return H.badge('Channel missing', 'r');
}

function gaRenderInstallCommands(cmds) {
  _gaInstallCommands = cmds || {};
  var rows = [
    ['debian_ubuntu', 'Debian / Ubuntu'],
    ['rhel_rocky_fedora', 'RHEL / Rocky / Fedora'],
    ['suse', 'SUSE']
  ];
  var h = '';
  rows.forEach(function(row) {
    var key = row[0], label = row[1], cmd = gaCommand(key);
    h += '<div class="mb-8">';
    h += '<div class="justify-between mb-4"><b>' + esc(label) + '</b><button class="btn" style="font-size:11px;padding:3px 8px" onclick="gaCopyInstall(\'' + escapeAttr(key) + '\')">Copy</button></div>';
    h += '<pre style="margin:0;background:var(--bg);border:1px solid var(--border);border-radius:6px;padding:8px;white-space:pre-wrap;overflow:auto">' + esc(cmd) + '</pre>';
    h += '</div>';
  });
  return h;
}

function gaRenderStatus(d) {
  var statusEl = document.getElementById('ga-status-body');
  var installEl = document.getElementById('ga-install-body');
  if (installEl) installEl.innerHTML = gaRenderInstallCommands(d.install_commands || {});
  if (!statusEl) return;
  var h = '<div style="display:grid;grid-template-columns:1fr 1fr;gap:8px 16px">';
  h += H.row('Status', gaStatusBadge(d.status));
  h += H.row('Running', d.running ? H.badge('Yes', 'g') : H.badge('No', 'y'));
  h += H.row('Config channel', d.channel_configured ? H.badge('Yes', 'g') : H.badge('No', 'r'));
  h += H.row('Live channel', d.channel_live ? H.badge('Yes', 'g') : H.badge('No', d.running ? 'y' : 'r'));
  h += H.row('Agent ping', d.agent_ping ? H.badge('OK', 'g') : H.badge('No response', 'y'));
  h += H.row('Next action', esc(d.message || '-'));
  if (d.agent_error) h += H.row('Agent error', '<span class="color-muted text-11">' + esc(d.agent_error) + '</span>');
  h += '</div>';
  statusEl.innerHTML = h;
}

async function gaRefreshStatus() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var statusEl = document.getElementById('ga-status-body');
  if (statusEl) statusEl.innerHTML = '<span class="spinner"></span> Checking...';
  try {
    var r = await fetchGet(EP.VM_GUEST_AGENT(v.name));
    var d = unwrapData(r);
    gaRenderStatus(d || {});
  } catch (e) {
    if (statusEl) statusEl.innerHTML = '<span class="color-red">' + esc(e.message) + '</span>';
  }
}

async function gaEnsureChannel() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  try {
    var r = await fetchPost(EP.VM_GUEST_AGENT_CHANNEL(v.name), {});
    if (r.error) { toast(r.error.message || 'Channel update failed', false); return; }
    var d = unwrapData(r);
    toast(d.reboot_required ? 'Channel configured. Restart VM to activate it.' : 'Guest agent channel ready.');
    addEvt('Guest agent channel: ' + v.name + ' ' + (d.status || 'updated'));
    gaRenderStatus(d || {});
    setTimeout(gaRefreshStatus, 800);
  } catch (e) { toast(e.message, false); }
}

function gaCopyInstall(key) {
  var cmd = gaCommand(key);
  navigator.clipboard.writeText(cmd)
    .then(function(){ toast('Install command copied'); })
    .catch(function(){ toast(cmd, true); });
}

async function gaPing() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var el = document.getElementById('ga-ping-result');
  if (el) el.innerHTML = '<span class="spinner"></span> Pinging...';
  try {
    var r = await fetchPost(EP.VM_GUEST_PING(v.name), {});
    if (r.error) { if (el) el.innerHTML = '<span class="color-red">&#10060; ' + _L('에이전트 응답 없음', 'Agent not responding') + ': ' + esc(r.error.message || '') + '</span>'; return; }
    if (el) el.innerHTML = '<span class="color-green">&#9989; ' + _L('게스트 에이전트 정상 응답', 'Guest agent is responding') + '</span>';
  } catch (e) { if (el) el.innerHTML = '<span class="color-red">&#10060; ' + esc(e.message) + '</span>'; }
}

async function gaShutdown() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  if (!await customConfirm('Graceful Shutdown', 'Send ACPI shutdown signal via guest agent to ' + v.name + '?')) return;
  try {
    var r = await fetchPost(EP.VM_GUEST_SHUTDOWN(v.name), {});
    if (r.error) { toast('Shutdown failed: ' + (r.error.message || ''), false); return; }
    toast('Graceful shutdown sent: ' + v.name);
    addEvt('Guest agent shutdown: ' + v.name);
    closeModal();
    setTimeout(loadAll, 3000);
  } catch (e) { toast('Error: ' + e.message, false); }
}

async function gaExec() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var cmd = (document.getElementById('ga-cmd')?.value || '').trim();
  if (!cmd) { toast('Command required', false); return; }
  var args = (document.getElementById('ga-args')?.value || '').trim();
  var el = document.getElementById('ga-exec-result');
  if (el) { el.style.display = 'block'; el.innerHTML = '<span class="spinner"></span> Executing...'; }
  try {
    var params = { name: v.name, command: cmd };
    if (args) params.args = args.split(/\s+/);
    var r = await fetchPost(EP.VM_GUEST_EXEC(v.name), params);
    if (r.error) { if (el) el.innerHTML = '<span class="color-red">Error: ' + esc(r.error.message || '') + '</span>'; return; }
    var d = unwrapData(r);
    var out = '';
    if (d.stdout) out += '<div class="mb-6"><span class="color-green">stdout:</span></div><div>' + esc(d.stdout) + '</div>';
    if (d.stderr) out += '<div class="mt-8"><span class="color-red">stderr:</span></div><div>' + esc(d.stderr) + '</div>';
    var exitCode = d.exitcode !== undefined ? d.exitcode : d.exit_code;
    if (exitCode !== undefined) out += '<div style="margin-top:6px;color:var(--fg2)">Exit code: ' + exitCode + '</div>';
    if (!out) out = '<span class="color-muted">Command executed (no output)</span>';
    if (el) el.innerHTML = out;
  } catch (e) { if (el) el.innerHTML = '<span class="color-red">' + esc(e.message) + '</span>'; }
}


async function vmMigrateDrop(vmName, targetIp, targetName) {
  if (!PCV.isMultiEdgeUI()) {
    toast(_L('클러스터 빌드 전용 기능입니다', 'This action is available only on the cluster build'), false);
    return;
  }
  if (!await customConfirm(_L('라이브 마이그레이션', 'Live Migration'),
    vmName + ' → ' + targetName + ' (' + targetIp + ')?')) return;
  showModal('<h2>&#128640; ' + _L('마이그레이션', 'Migrating') + '</h2><p>' + esc(vmName) + ' → ' + esc(targetName) + '</p><div class="prog-bar"><div class="prog-fill" id="mig-prog" class="w-pct-20"></div></div><div class="prog-status" id="mig-st"><span class="spinner"></span> ' + _L('전송 중...', 'Transferring...') + '</div>');
  try {
    var migrateEndpoint = PCV.getOptionalEndpoint('VM_MIGRATE', vmName);
    if (!migrateEndpoint) {
      toast(_L('Single Edge에서는 마이그레이션이 비활성화됩니다', 'Migration is disabled on Single Edge'), false);
      closeModal();
      return;
    }
    var r = await fetchPost(migrateEndpoint, { target: 'qemu+ssh://pcvdev@' + targetIp + '/system' });
    var pf = document.getElementById('mig-prog'), ps = document.getElementById('mig-st');
    if (r && r.error) {
      if (pf) { pf.style.width = '100%'; pf.style.background = 'var(--red)'; }
      if (ps) ps.innerHTML = '&#10060; ' + esc(r.error.message || 'Failed');
    } else {
      if (pf) pf.style.width = '100%';
      if (ps) ps.innerHTML = '&#9989; ' + _L('마이그레이션 시작됨', 'Migration started');
      addEvt('VM Migrate: ' + vmName + ' → ' + targetName);
      setTimeout(function() { closeModal(); loadAll(); }, 2000);
    }
  } catch (e) {
    var ps = document.getElementById('mig-st');
    if (ps) ps.innerHTML = '&#10060; ' + esc(e.message);
  }
}

function showBlkioEditor() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var h = '<h2>&#128190; ' + (t('vm.blkio_title') || 'Disk I/O Limits') + ': ' + esc(v.name) + '</h2>';
  h += '<p class="stat-label mb-12">'
    + (t('vm.blkio_desc') || 'Set disk I/O throttle limits. Values in bytes/sec and IOPS. Set 0 for unlimited.')
    + '</p>';
  h += '<div class="fr"><label>' + (t('vm.read_bytes_sec') || 'Read (MB/s)') + '</label><input id="blkio-rd-bytes" type="number" value="0" min="0" placeholder="0" class="w-140"><span class="stat-label ml-4">MB/s</span></div>';
  h += '<div class="fr"><label>' + (t('vm.write_bytes_sec') || 'Write (MB/s)') + '</label><input id="blkio-wr-bytes" type="number" value="0" min="0" placeholder="0" class="w-140"><span class="stat-label ml-4">MB/s</span></div>';
  h += '<div class="fr"><label>' + (t('vm.read_iops_sec') || 'Read IOPS') + '</label><input id="blkio-rd-iops" type="number" value="0" min="0" placeholder="0" class="w-140"></div>';
  h += '<div class="fr"><label>' + (t('vm.write_iops_sec') || 'Write IOPS') + '</label><input id="blkio-wr-iops" type="number" value="0" min="0" placeholder="0" class="w-140"></div>';
  h += '<div id="blkio-status" style="font-size:11px;min-height:20px;margin:8px 0"></div>';
  h += '<div class="text-right mt-14">';
  h += '<button class="btn" onclick="blkioGet()" style="margin-right:4px">&#128269; ' + (t('vm.blkio_get') || 'Get Current') + '</button>';
  h += '<button class="btn btn-g" onclick="blkioSet()">&#9989; ' + (t('vm.blkio_apply') || 'Apply') + '</button> ';
  h += '<button class="btn" onclick="closeModal()">' + t('btn.cancel') + '</button>';
  h += '</div>';
  showModal(h);
}

async function blkioGet() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var el = document.getElementById('blkio-status');
  if (el) el.innerHTML = '<span class="spinner"></span> ' + (t('loading') || 'Loading...');
  try {
    var r = await fetchPost(EP.VM_RPC(v.name), { method: 'vm.blkio.get', params: { name: v.name } });
    if (r.error) { if (el) el.innerHTML = '<span class="color-red">' + esc(r.error.message || 'Failed') + '</span>'; return; }
    var d = unwrapData(r);
    var rdB = document.getElementById('blkio-rd-bytes');
    var wrB = document.getElementById('blkio-wr-bytes');
    var rdI = document.getElementById('blkio-rd-iops');
    var wrI = document.getElementById('blkio-wr-iops');
    if (rdB) rdB.value = Math.round((d.read_bytes_sec || 0) / 1048576);
    if (wrB) wrB.value = Math.round((d.write_bytes_sec || 0) / 1048576);
    if (rdI) rdI.value = d.read_iops_sec || 0;
    if (wrI) wrI.value = d.write_iops_sec || 0;
    if (el) el.innerHTML = '<span class="color-green">&#9989; ' + (t('vm.blkio_loaded') || 'Current limits loaded') + '</span>';
  } catch (e) {
    if (el) el.innerHTML = '<span class="color-red">' + esc(e.message) + '</span>';
  }
}

async function blkioSet() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var rdMB = parseInt((document.getElementById('blkio-rd-bytes') || {}).value) || 0;
  var wrMB = parseInt((document.getElementById('blkio-wr-bytes') || {}).value) || 0;
  var rdIops = parseInt((document.getElementById('blkio-rd-iops') || {}).value) || 0;
  var wrIops = parseInt((document.getElementById('blkio-wr-iops') || {}).value) || 0;
  var el = document.getElementById('blkio-status');
  if (el) el.innerHTML = '<span class="spinner"></span> ' + (t('vm.blkio_applying') || 'Applying...');
  try {
    var r = await fetchPost(EP.VM_RPC(v.name), {
      method: 'vm.blkio.set',
      params: {
        name: v.name,
        read_bytes_sec: rdMB * 1048576,
        write_bytes_sec: wrMB * 1048576,
        read_iops_sec: rdIops,
        write_iops_sec: wrIops
      }
    });
    if (r.error) {
      if (el) el.innerHTML = '<span class="color-red">' + esc(r.error.message || 'Failed') + '</span>';
      toast((t('vm.blkio_failed') || 'I/O limit failed') + ': ' + (r.error.message || ''), false);
      return;
    }
    if (el) el.innerHTML = '<span class="color-green">&#9989; ' + (t('vm.blkio_applied') || 'I/O limits applied') + '</span>';
    toast((t('vm.blkio_applied') || 'I/O limits applied') + ': ' + v.name);
    addEvt('BlkIO set: ' + v.name + ' R:' + rdMB + 'MB/s W:' + wrMB + 'MB/s');
    setTimeout(closeModal, 1500);
  } catch (e) {
    if (el) el.innerHTML = '<span class="color-red">' + esc(e.message) + '</span>';
    toast(e.message, false);
  }
}






PCV.vm = {
  render: render,
  setSort: setSort,
  getFiltered: getFiltered,
  toggleVmView: toggleVmView,
  vmPower: vmPower,
  vmDel: vmDel,
  doVmDel: doVmDel,
  showCreate: showCreate,
  doCreate: doCreate,
  wizStorageChanged: wizStorageChanged,
  wizPickStoragePool: wizPickStoragePool,
  wizLoadStorageTargets: wizLoadStorageTargets,
  showSettings: showSettings,
  showRenameVm: showRenameVm,
  doVmRename: doVmRename,
  showSnap: showSnap,
  showVnc: showVnc,
  vmClone: vmClone,
  doVmClone: doVmClone,
  vmExportOva: vmExportOva,
  vmDeleteStatus: vmDeleteStatus,
  showNicMgr: showNicMgr,
  showMemStats: showMemStats,
  showCpuStats: showCpuStats,
  showDiskLiveResize: showDiskLiveResize,
  showVmDiskUsage: showVmDiskUsage,
  showGuestAgent: showGuestAgent,
  gaRefreshStatus: gaRefreshStatus,
  gaEnsureChannel: gaEnsureChannel,
  showBlkioEditor: showBlkioEditor,
  showVmCompare: showVmCompare,
  showBulkActions: showBulkActions,
  bulkAction: bulkAction,
  bulkSnapshot: bulkSnapshot,
  renderSummary: renderSummary,
  renderConsole: renderConsole,
  renderSnapshots: renderSnapshots,
  renderPerformance: renderPerformance,
  renderTimeline: renderTimeline,
  vmMigrateDrop: vmMigrateDrop,
  connectWS: connectWS
};


window.render = render;
window.setSort = setSort;
window.getFiltered = getFiltered;
window.toggleChk = toggleChk;
window.bulkStop = bulkStop;
window.showCtx = showCtx;
window.renderSummary = renderSummary;
window.renderConsole = renderConsole;
window.openNoVNC = openNoVNC;
window.vncFullscreen = vncFullscreen;
window.vncFitWindow = vncFitWindow;
window.openNoVNCPopup = openNoVNCPopup;
window.copyVncAddr = copyVncAddr;
window.renderSnapshots = renderSnapshots;
window.takeSnap = takeSnap;
window.snapNameValidate = snapNameValidate;
window.snapCreateExec = snapCreateExec;
window.snapRb = snapRb;
window.rbValidate = rbValidate;
window.rbExec = rbExec;
window.snapDl = snapDl;
window.snapDeleteAll = snapDeleteAll;
window.sdaPreview = sdaPreview;
window.sdaExec = sdaExec;
window.renderPerformance = renderPerformance;
window.renderTimeline = renderTimeline;
window.vmPower = vmPower;
window.pw = vmPower;
window.vmDel = vmDel;
window.doVmDel = doVmDel;
window.showCreate = showCreate;
window.wizSave = wizSave;
window.wizGo = wizGo;
window.renderWiz = renderWiz;
window.wizLoadNets = wizLoadNets;
window.wizStorageChanged = wizStorageChanged;
window.wizPickStoragePool = wizPickStoragePool;
window.wizLoadStorageTargets = wizLoadStorageTargets;
window.browseISO = browseISO;
window.isoSelect = isoSelect;
window.isoSelectManual = isoSelectManual;
window.closeISOBrowser = closeISOBrowser;
window.doCreate = doCreate;
window.showSettings = showSettings;
window.showRenameVm = showRenameVm;
window.doVmRename = doVmRename;
window.setHw = setHw;
window.hwIdentity = hwIdentity;
window.hwCpu = hwCpu;
window.hwMem = hwMem;
window.hwCd = hwCd;
window.browseISOForMount = browseISOForMount;
window.selectISOForMount = selectISOForMount;
window.hwNic = hwNic;
window.loadNics = loadNics;
window.nicAdd = nicAdd;
window.nicDel = nicDel;
window.hwAP = hwAP;
window.loadBP = loadBP;
window.bpSet = bpSet;
window.doSet = doSet;
window.doMnt = doMnt;
window.doEjt = doEjt;
window.showSnap = showSnap;
window.showNicMgr = showNicMgr;
window.nmAdd = nmAdd;
window.nmDel = nmDel;
window.showVnc = showVnc;
window.vmClone = vmClone;
window.doVmClone = doVmClone;
window.hwDisk = hwDisk;
window.doDiskResize = doDiskResize;
window.vmDeleteStatus = vmDeleteStatus;
window.vmExportOva = vmExportOva;
window.hwCpuPin = hwCpuPin;
window.doCpuPin = doCpuPin;
window.hwBandwidth = hwBandwidth;
window.doBandwidth = doBandwidth;
window.showMemStats = showMemStats;
window.showCpuStats = showCpuStats;
window.showDiskLiveResize = showDiskLiveResize;
window.showVmDiskUsage = showVmDiskUsage;
window.doDiskLiveResize = doDiskLiveResize;
window.showGuestAgent = showGuestAgent;
window.gaRefreshStatus = gaRefreshStatus;
window.gaEnsureChannel = gaEnsureChannel;
window.gaCopyInstall = gaCopyInstall;
window.gaPing = gaPing;
window.gaShutdown = gaShutdown;
window.gaExec = gaExec;
window.showBlkioEditor = showBlkioEditor;
window.blkioGet = blkioGet;
window.blkioSet = blkioSet;
window.toggleVmView = toggleVmView;
window.showVmCompare = showVmCompare;
window.showBulkActions = showBulkActions;
window.bulkAction = bulkAction;
window.bulkSnapshot = bulkSnapshot;
window.vmMigrateDrop = vmMigrateDrop;

})(window.PCV);
