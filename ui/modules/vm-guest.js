/* ═══════════════════════════════════════════════════════════════
   PureCVisor — modules/vm-guest.js
   CPU Pinning, QoS, Mem/CPU Stats, Disk Resize, Guest Usage/Agent, Migration, I/O Throttle
   ADR-0013: IIFE module scope — vm.js에서 분할 (pure-move)
   ═══════════════════════════════════════════════════════════════ */
window.PCV = window.PCV || {};
(function(PCV) {

/* ═══ CPU PINNING ═══ */
function hwCpuPin() {
  var el = PCV.uxlib.el;
  return [
    el('h4', null, '📌 CPU Pinning'),
    el('p', { class: 'stat-label mb-8' }, 'Pin vCPUs to physical cores for performance isolation.'),
    el('div', { class: 'fr' }, el('label', { for: 'scpin' }, 'vCPU Map'), el('input', { id: 'scpin', placeholder: '0:0,1:2,2:4', class: 'flex-1' })),
    el('p', { class: 'stat-label' }, 'Format: vCPU:pCPU pairs, comma separated (e.g., 0:0,1:2)'),
    el('button', { class: 'btn btn-g mt-8', onclick: 'doCpuPin()' }, t('btn.apply'))
  ];
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

/* ═══ BANDWIDTH QoS ═══ */
function hwBandwidth() {
  var el = PCV.uxlib.el;
  return [
    el('h4', null, '📶 Network Bandwidth (QoS)'),
    el('p', { class: 'stat-label mb-8' }, 'Set network bandwidth limits for VM interfaces.'),
    el('div', { class: 'fr' }, el('label', { for: 'sbw-in' }, 'Inbound (Mbps)'), el('input', { id: 'sbw-in', type: 'number', value: '1000', placeholder: '1000' })),
    el('div', { class: 'fr' }, el('label', { for: 'sbw-out' }, 'Outbound (Mbps)'), el('input', { id: 'sbw-out', type: 'number', value: '1000', placeholder: '1000' })),
    el('div', { class: 'fr' }, el('label', { for: 'sbw-burst' }, 'Burst (KB)'), el('input', { id: 'sbw-burst', type: 'number', value: '1024', placeholder: '1024' })),
    el('button', { class: 'btn btn-g mt-8', onclick: 'doBandwidth()' }, t('btn.apply'))
  ];
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

/* ═══ VM MEMORY STATS ═══ */
async function showMemStats() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var mkEl = PCV.uxlib.el;
  showModal([
    mkEl('h2', null, '📌 Memory Stats: ' + v.name),
    mkEl('div', { id: 'mem-stats-body' }, mkEl('span', { class: 'spinner' }), ' ' + t('loading')),
    mkEl('div', { class: 'text-right mt-12' }, mkEl('button', { class: 'btn', onclick: 'closeModal()' }, t('btn.close')))
  ]);
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
    var mk = PCV.uxlib.el;
    var grid = mk('div', { style: 'display:grid;grid-template-columns:1fr 1fr;gap:8px 16px;font-size:12px' },
      mk('div', null, HN.row('Actual Balloon', mk('span', { class: 'color-accent' }, fmtKb(d.actual_balloon_kb || d.actual)))),
      mk('div', null, HN.row('RSS', mk('span', { class: 'color-green' }, fmtKb(d.rss_kb || d.rss)))),
      mk('div', null, HN.row('Unused', mk('span', { class: 'color-muted' }, fmtKb(d.unused_kb || d.unused)))),
      mk('div', null, HN.row('Available', mk('span', { class: 'color-cyan' }, fmtKb(d.available_kb || d.available)))),
      mk('div', null, HN.row('Swap In', fmtKb(d.swap_in_kb || d.swap_in || 0))),
      mk('div', null, HN.row('Swap Out', fmtKb(d.swap_out_kb || d.swap_out || 0))),
      mk('div', null, HN.row('Major Fault', String(d.major_fault || d.majflt || 0))),
      mk('div', null, HN.row('Minor Fault', String(d.minor_fault || d.minflt || 0))));
    PCV.uxlib.clearEl(el);
    el.appendChild(mk('div', { style: 'border:1px solid var(--border);border-radius:6px;padding:12px' }, grid));
  } catch (e) {
    // var 아님(no-redeclare) — try 블록의 `var el`이 함수 스코프에 이미
    // hoisting 되어 있으므로 재선언 없이 재대입 (동작 동일).
    el = document.getElementById('mem-stats-body');
    if (el) PCV.uxlib.setMsg(el, 'err', { tag: 'p' }, 'Failed: ' + e.message);
  }
}

/* ═══ VM CPU STATS ═══ */
async function showCpuStats() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var mkEl = PCV.uxlib.el;
  showModal([
    mkEl('h2', null, '⚙ CPU Stats: ' + v.name),
    mkEl('div', { id: 'cpu-stats-body' }, mkEl('span', { class: 'spinner' }), ' ' + t('loading')),
    mkEl('div', { class: 'text-right mt-12' }, mkEl('button', { class: 'btn', onclick: 'closeModal()' }, t('btn.close')))
  ]);
  try {
    var r = await fetchPost(EP.VM_RPC(v.name), { method: 'vm.cpu.stats', params: { name: v.name } });
    var d = unwrapData(r);
    var el = document.getElementById('cpu-stats-body'); if (!el) return;
    var vcpuCount = d.vcpu_count || d.vcpu || v.vcpu || 0;
    var maxVcpu = d.max_vcpu || d.max || vcpuCount;
    var mk = PCV.uxlib.el;
    var parts = [];
    parts.push(mk('div', { class: 'mb-12' },
      HN.row('vCPU Count', mk('span', { class: 'color-accent' }, vcpuCount)),
      HN.row('Max vCPU', mk('span', { class: 'color-muted' }, maxVcpu)),
      HN.row('CPU Time (ns)', mk('span', { class: 'color-green' }, d.cpu_time || 0))));
    var vcpus = d.vcpus || d.vcpu_list || [];
    if (vcpus.length > 0) {
      var tbody = mk('tbody');
      vcpus.forEach(function(vc, i) {
        var state = (vc.state === 1 || vc.state === 'running') ? mk('span', { class: 'color-green' }, 'Running') : mk('span', { class: 'color-muted' }, 'Offline');
        tbody.appendChild(mk('tr', null,
          mk('td', null, mk('b', null, vc.number !== undefined ? vc.number : i)),
          mk('td', null, state),
          mk('td', null, vc.cpu_time || 0),
          mk('td', null, vc.cpu !== undefined ? vc.cpu : '-')));
      });
      parts.push(mk('table', null,
        mk('thead', null, mk('tr', null,
          mk('th', null, 'vCPU'), mk('th', null, 'State'), mk('th', null, 'CPU Time (ns)'), mk('th', null, 'Physical CPU'))),
        tbody));
    } else {
      parts.push(mk('p', { class: 'color-muted text-12' }, 'Detailed per-vCPU info not available (VM may be stopped)'));
    }
    PCV.uxlib.clearEl(el);
    el.appendChild(PCV.uxlib.frag(parts));
  } catch (e) {
    // var 아님(no-redeclare) — try 블록의 `var el`이 함수 스코프에 이미
    // hoisting 되어 있으므로 재선언 없이 재대입 (동작 동일).
    el = document.getElementById('cpu-stats-body');
    if (el) PCV.uxlib.setMsg(el, 'err', { tag: 'p' }, 'Failed: ' + e.message);
  }
}

/* ═══ VM DISK LIVE RESIZE (MODAL) ═══ */
function showDiskLiveResize() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var el = PCV.uxlib.el;
  showModal([
    el('h2', null, '💾 Disk Live Resize: ' + v.name),
    el('p', { class: 'stat-label mb-12' }, 'Resize a VM disk while the VM is running. The guest OS may need to rescan partitions.'),
    el('div', { class: 'fr' }, el('label', { for: 'dlr-target' }, 'Target Device'), el('input', { id: 'dlr-target', value: 'vda', placeholder: 'vda', class: 'w-120' })),
    el('div', { class: 'fr' }, el('label', { for: 'dlr-size' }, 'New Size (GB)'), el('input', { id: 'dlr-size', type: 'number', value: '40', min: '1', placeholder: '40', class: 'w-120' })),
    el('div', { class: 'text-right mt-14' },
      el('button', { class: 'btn btn-g', onclick: 'doDiskLiveResize()' }, '💾 Resize'),
      ' ',
      el('button', { class: 'btn', onclick: 'closeModal()' }, t('btn.cancel')))
  ]);
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

/* ═══ VM GUEST DISK USAGE ═══ */
function _vmDiskUsagePct(fs) {
  if (!fs) return null;
  if (fs.usage_percent !== undefined) return Number(fs.usage_percent);
  var total = Number(fs.total_bytes || 0);
  var used = Number(fs.used_bytes || 0);
  return total > 0 ? (used * 100 / total) : null;
}

/* renderProgressBar(ui.js 문자열 헬퍼, 수정 금지) 의 노드 등가물 — class/구조 동형. */
function _vmgProgressBar(p, c) {
  var el = PCV.uxlib.el;
  var cl = p > 85 ? 'var(--red)' : p > 60 ? 'var(--yellow)' : 'var(--green)';
  var anim = p > 85 ? ' pulse-anim' : '';
  return el('div', { class: 'pb' + anim },
    el('div', { class: 'pb-f scan-anim', style: 'width:' + p + '%;background:' + (c || cl) }),
    el('div', { class: 'pb-t' }, p.toFixed(1) + '%'));
}

function _vmDiskUsageBar(pct) {
  var el = PCV.uxlib.el;
  if (pct === null || isNaN(pct)) return el('span', { class: 'color-muted' }, '-');
  return el('div', { style: 'min-width:120px' },
    _vmgProgressBar(Math.max(0, Math.min(100, pct))),
    el('div', { class: 'text-xs color-muted mt-4' }, pct.toFixed(1) + '%'));
}

function _vmDiskUsageSeverity(pct) {
  if (pct === null || isNaN(pct)) return 'y';
  if (pct >= 90) return 'r';
  if (pct >= 80) return 'y';
  return 'g';
}

function _vmRenderDiskUsage(d) {
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag;
  var filesystems = Array.isArray(d.filesystems) ? d.filesystems : [];
  var total = Number(d.total_bytes || 0);
  var used = Number(d.used_bytes || 0);
  var pct = d.usage_percent !== undefined ? Number(d.usage_percent) : (total > 0 ? used * 100 / total : null);
  var summary = el('div', { class: 'mb-12' },
    el('div', { class: 'sg grid-3' },
      HN.card(_L('전체 사용량', 'Total Usage'), [
        HN.row(_L('사용', 'Used'), used ? formatBytes(used) : '-'),
        HN.row(_L('전체', 'Total'), total ? formatBytes(total) : '-'),
        HN.row(_L('상태', 'Status'), HN.badge(pct === null ? _L('알 수 없음', 'Unknown') : pct.toFixed(1) + '%', _vmDiskUsageSeverity(pct))),
        _vmDiskUsageBar(pct)
      ]),
      HN.card(_L('마운트', 'Mounts'), [
        HN.row(_L('파일시스템', 'Filesystems'), String(filesystems.length)),
        HN.row(_L('수집 방식', 'Source'), 'qemu-guest-agent'),
        HN.row(_L('대상', 'Target'), d.name || '-')
      ])));

  if (!filesystems.length) {
    return frag(summary, el('p', { class: 'color-muted text-12' }, _L('게스트 파일시스템 정보가 없습니다.', 'No guest filesystem data returned.')));
  }

  filesystems.sort(function(a, b) {
    var am = a.mountpoint || '';
    var bm = b.mountpoint || '';
    if (am === '/') return -1;
    if (bm === '/') return 1;
    return am.localeCompare(bm);
  });

  var tbody = el('tbody');
  filesystems.forEach(function(fs) {
    var fsPct = _vmDiskUsagePct(fs);
    var fsUsed = Number(fs.used_bytes || 0);
    var fsTotal = Number(fs.total_bytes || 0);
    tbody.appendChild(el('tr', null,
      el('td', null,
        el('b', null, fs.mountpoint || '-'),
        el('div', { class: 'text-xs color-muted' }, fs.name || fs.device || '')),
      el('td', null, fs.type || '-'),
      el('td', null, fsUsed ? formatBytes(fsUsed) : '-'),
      el('td', null, fsTotal ? formatBytes(fsTotal) : '-'),
      el('td', null, _vmDiskUsageBar(fsPct))));
  });
  var table = el('table', null,
    el('thead', null, el('tr', null,
      el('th', null, _L('마운트', 'Mount')),
      el('th', null, _L('타입', 'Type')),
      el('th', null, _L('사용', 'Used')),
      el('th', null, _L('전체', 'Total')),
      el('th', null, _L('사용률', 'Usage')))),
    tbody);
  return frag(summary, table);
}

async function showVmDiskUsage() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var running = v.state === 'running';
  var el = PCV.uxlib.el;
  showModal([
    el('h2', null, '📊 ' + _L('디스크 사용량', 'Disk Usage') + ': ' + v.name),
    el('div', { id: 'vm-disk-usage-body', style: 'min-height:90px' }, el('span', { class: 'spinner' }), ' ' + t('loading')),
    el('div', { class: 'text-right mt-14' }, el('button', { class: 'btn', onclick: 'closeModal()' }, t('btn.close')))
  ]);

  var body = document.getElementById('vm-disk-usage-body');
  if (!running) {
    if (body) PCV.uxlib.setMsg(body, 'muted', { tag: 'p' }, _L('게스트 파일시스템 사용량은 VM 실행 중에만 조회할 수 있습니다.', 'Guest filesystem usage is available only while the VM is running.'));
    return;
  }

  try {
    var r = await fetchGet(EP.VM_DISK_USAGE(v.name));
    if (r.error) throw new Error(r.error.message || 'disk usage failed');
    var d = unwrapData(r) || {};
    if (body) { PCV.uxlib.clearEl(body); body.appendChild(_vmRenderDiskUsage(d)); }
  } catch (e) {
    if (body) {
      var mk = PCV.uxlib.el;
      PCV.uxlib.clearEl(body);
      body.appendChild(PCV.uxlib.frag(
        mk('p', { class: 'color-red' }, e.message),
        mk('p', { class: 'color-muted text-xs mt-8' }, _L('qemu-guest-agent 채널과 게스트 내부 에이전트 상태를 확인하세요.', 'Check the qemu-guest-agent channel and guest agent status.')),
        mk('button', { class: 'btn btn-g mt-8', onclick: 'closeModal();showGuestAgent()' }, '💬 Guest Agent')));
    }
  }
}

/* ═══ GUEST AGENT ═══ */
var _gaInstallCommands = {};

function showGuestAgent() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var el = PCV.uxlib.el;
  showModal([
    el('h2', null, '💬 Guest Agent: ' + v.name),
    el('div', { class: 'mb-14 p-10 border-muted rounded-md' },
      el('div', { style: 'display:flex;gap:8px;flex-wrap:wrap;margin-bottom:10px' },
        el('button', { class: 'btn', onclick: 'gaRefreshStatus()' }, '↻ Status'),
        el('button', { class: 'btn btn-g', onclick: 'gaEnsureChannel()' }, 'Channel'),
        el('button', { class: 'btn btn-g', onclick: 'gaPing()' }, '🟢 Ping'),
        el('button', { class: 'btn btn-r', onclick: 'gaShutdown()' }, '⚠ Graceful Shutdown')),
      el('div', { id: 'ga-status-body', style: 'font-size:12px;min-height:48px;margin-bottom:10px' }, el('span', { class: 'spinner' }), ' Checking...'),
      el('div', { id: 'ga-ping-result', style: 'font-size:12px;min-height:20px;margin-bottom:8px' })),
    el('div', { class: 'mb-14 p-10 border-muted rounded-md' },
      el('h4', { class: 'mb-8' }, 'Install qemu-guest-agent'),
      el('div', { id: 'ga-install-body', class: 'text-12 color-muted' })),
    el('div', { class: 'mb-14 p-10 border-muted rounded-md' },
      el('h4', { class: 'mb-8' }, '💻 Execute Command'),
      el('div', { class: 'fr' }, el('label', { for: 'ga-cmd' }, 'Command'), el('input', { id: 'ga-cmd', placeholder: 'cat /etc/hostname', class: 'flex-1' })),
      el('div', { class: 'fr' }, el('label', { for: 'ga-args' }, 'Args'), el('input', { id: 'ga-args', placeholder: '(optional, space separated)', class: 'flex-1' })),
      el('button', { class: 'btn btn-g', onclick: 'gaExec()', style: 'margin-top:6px' }, '▶ Execute'),
      el('div', { id: 'ga-exec-result', style: 'margin-top:10px;background:var(--bg);border:1px solid var(--border);border-radius:6px;padding:10px;max-height:250px;overflow:auto;font-size:11px;font-family:var(--font-mono);white-space:pre-wrap;display:none' })),
    el('div', { class: 'text-right' }, el('button', { class: 'btn', onclick: 'closeModal()' }, t('btn.close')))
  ]);
  setTimeout(gaRefreshStatus, 20);
}

function gaCommand(key) {
  if (_gaInstallCommands && _gaInstallCommands[key]) return _gaInstallCommands[key];
  if (key === 'rhel_rocky_fedora') return 'sudo dnf install -y qemu-guest-agent && sudo systemctl enable --now qemu-guest-agent';
  if (key === 'suse') return 'sudo zypper install -y qemu-guest-agent && sudo systemctl enable --now qemu-guest-agent';
  return 'sudo apt update && sudo apt install -y qemu-guest-agent && sudo systemctl enable --now qemu-guest-agent';
}

function gaStatusBadge(status) {
  if (status === 'ok') return HN.badge('OK', 'g');
  if (status === 'vm_stopped') return HN.badge('Stopped', 'y');
  if (status === 'reboot_required') return HN.badge('Reboot needed', 'y');
  if (status === 'agent_unavailable') return HN.badge('Install needed', 'y');
  return HN.badge('Channel missing', 'r');
}

function gaRenderInstallCommands(cmds) {
  _gaInstallCommands = cmds || {};
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag;
  var rows = [
    ['debian_ubuntu', 'Debian / Ubuntu'],
    ['rhel_rocky_fedora', 'RHEL / Rocky / Fedora'],
    ['suse', 'SUSE']
  ];
  var out = frag();
  rows.forEach(function(row) {
    var key = row[0], label = row[1], cmd = gaCommand(key);
    out.appendChild(el('div', { class: 'mb-8' },
      el('div', { class: 'justify-between mb-4' },
        el('b', null, label),
        el('button', { class: 'btn', style: 'font-size:11px;padding:3px 8px', onclick: "gaCopyInstall('" + escapeAttr(key) + "')" }, 'Copy')),
      el('pre', { style: 'margin:0;background:var(--bg);border:1px solid var(--border);border-radius:6px;padding:8px;white-space:pre-wrap;overflow:auto' }, cmd)));
  });
  return out;
}

function gaRenderStatus(d) {
  var el = PCV.uxlib.el;
  var statusEl = document.getElementById('ga-status-body');
  var installEl = document.getElementById('ga-install-body');
  if (installEl) { PCV.uxlib.clearEl(installEl); installEl.appendChild(gaRenderInstallCommands(d.install_commands || {})); }
  if (!statusEl) return;
  var grid = el('div', { style: 'display:grid;grid-template-columns:1fr 1fr;gap:8px 16px' },
    HN.row('Status', gaStatusBadge(d.status)),
    HN.row('Running', d.running ? HN.badge('Yes', 'g') : HN.badge('No', 'y')),
    HN.row('Config channel', d.channel_configured ? HN.badge('Yes', 'g') : HN.badge('No', 'r')),
    HN.row('Live channel', d.channel_live ? HN.badge('Yes', 'g') : HN.badge('No', d.running ? 'y' : 'r')),
    HN.row('Agent ping', d.agent_ping ? HN.badge('OK', 'g') : HN.badge('No response', 'y')),
    HN.row('Next action', d.message || '-'),
    d.agent_error ? HN.row('Agent error', el('span', { class: 'color-muted text-11' }, d.agent_error)) : null);
  PCV.uxlib.clearEl(statusEl);
  statusEl.appendChild(grid);
}

async function gaRefreshStatus() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var statusEl = document.getElementById('ga-status-body');
  if (statusEl) PCV.uxlib.setMsg(statusEl, 'loading', null, 'Checking...');
  try {
    var r = await fetchGet(EP.VM_GUEST_AGENT(v.name));
    var d = unwrapData(r);
    gaRenderStatus(d || {});
  } catch (e) {
    if (statusEl) PCV.uxlib.setMsg(statusEl, 'err', null, e.message);
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
  if (el) PCV.uxlib.setMsg(el, 'loading', null, 'Pinging...');
  try {
    var r = await fetchPost(EP.VM_GUEST_PING(v.name), {});
    if (r.error) { if (el) PCV.uxlib.setMsg(el, 'err', null, '❌ ' + _L('에이전트 응답 없음', 'Agent not responding') + ': ' + (r.error.message || '')); return; }
    if (el) PCV.uxlib.setMsg(el, 'ok', null, '✅ ' + _L('게스트 에이전트 정상 응답', 'Guest agent is responding'));
  } catch (e) { if (el) PCV.uxlib.setMsg(el, 'err', null, '❌ ' + e.message); }
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
  if (el) { el.style.display = 'block'; PCV.uxlib.setMsg(el, 'loading', null, 'Executing...'); }
  try {
    var params = { name: v.name, command: cmd };
    if (args) params.args = args.split(/\s+/);
    var r = await fetchPost(EP.VM_GUEST_EXEC(v.name), params);
    if (r.error) { if (el) PCV.uxlib.setMsg(el, 'err', null, 'Error: ' + (r.error.message || '')); return; }
    var d = unwrapData(r);
    var mk = PCV.uxlib.el;
    var parts = [];
    if (d.stdout) parts.push(mk('div', { class: 'mb-6' }, mk('span', { class: 'color-green' }, 'stdout:')), mk('div', null, d.stdout));
    if (d.stderr) parts.push(mk('div', { class: 'mt-8' }, mk('span', { class: 'color-red' }, 'stderr:')), mk('div', null, d.stderr));
    var exitCode = d.exitcode !== undefined ? d.exitcode : d.exit_code;
    if (exitCode !== undefined) parts.push(mk('div', { style: 'margin-top:6px;color:var(--fg2)' }, 'Exit code: ' + exitCode));
    if (!parts.length) parts.push(mk('span', { class: 'color-muted' }, 'Command executed (no output)'));
    if (el) { PCV.uxlib.clearEl(el); el.appendChild(PCV.uxlib.frag(parts)); }
  } catch (e) { if (el) PCV.uxlib.setMsg(el, 'err', null, e.message); }
}

/* ═══ D3: DRAG & DROP VM MIGRATION ═══ */
async function vmMigrateDrop(vmName, targetIp, targetName) {
  if (!PCV.isMultiEdgeUI()) {
    toast(_L('클러스터 빌드 전용 기능입니다', 'This action is available only on the cluster build'), false);
    return;
  }
  if (!await customConfirm(_L('라이브 마이그레이션', 'Live Migration'),
    vmName + ' → ' + targetName + ' (' + targetIp + ')?')) return;
  var el = PCV.uxlib.el;
  showModal([
    el('h2', null, '🚀 ' + _L('마이그레이션', 'Migrating')),
    el('p', null, vmName + ' → ' + targetName),
    el('div', { class: 'prog-bar' }, el('div', { class: 'prog-fill', id: 'mig-prog' })),
    el('div', { class: 'prog-status', id: 'mig-st' }, el('span', { class: 'spinner' }), ' ' + _L('전송 중...', 'Transferring...'))
  ]);
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
      if (ps) PCV.uxlib.setMsg(ps, null, null, '❌ ' + (r.error.message || 'Failed'));
    } else {
      if (pf) pf.style.width = '100%';
      if (ps) PCV.uxlib.setMsg(ps, null, null, '✅ ' + _L('마이그레이션 시작됨', 'Migration started'));
      addEvt('VM Migrate: ' + vmName + ' → ' + targetName);
      setTimeout(function() { closeModal(); loadAll(); }, 2000);
    }
  } catch (e) {
    // var 아님(no-redeclare) — try 블록의 `var ps`가 함수 스코프에 이미
    // hoisting 되어 있으므로 재선언 없이 재대입 (동작 동일).
    ps = document.getElementById('mig-st');
    if (ps) PCV.uxlib.setMsg(ps, null, null, '❌ ' + e.message);
  }
}
/* ═══ DISK I/O THROTTLE EDITOR ═══ */
function showBlkioEditor() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var el = PCV.uxlib.el;
  showModal([
    el('h2', null, '💾 ' + (t('vm.blkio_title') || 'Disk I/O Limits') + ': ' + v.name),
    el('p', { class: 'stat-label mb-12' }, t('vm.blkio_desc') || 'Set disk I/O throttle limits. Values in bytes/sec and IOPS. Set 0 for unlimited.'),
    el('div', { class: 'fr' },
      el('label', { for: 'blkio-rd-bytes' }, t('vm.read_bytes_sec') || 'Read (MB/s)'),
      el('input', { id: 'blkio-rd-bytes', type: 'number', value: '0', min: '0', placeholder: '0', class: 'w-140' }),
      el('span', { class: 'stat-label ml-4' }, 'MB/s')),
    el('div', { class: 'fr' },
      el('label', { for: 'blkio-wr-bytes' }, t('vm.write_bytes_sec') || 'Write (MB/s)'),
      el('input', { id: 'blkio-wr-bytes', type: 'number', value: '0', min: '0', placeholder: '0', class: 'w-140' }),
      el('span', { class: 'stat-label ml-4' }, 'MB/s')),
    el('div', { class: 'fr' },
      el('label', { for: 'blkio-rd-iops' }, t('vm.read_iops_sec') || 'Read IOPS'),
      el('input', { id: 'blkio-rd-iops', type: 'number', value: '0', min: '0', placeholder: '0', class: 'w-140' })),
    el('div', { class: 'fr' },
      el('label', { for: 'blkio-wr-iops' }, t('vm.write_iops_sec') || 'Write IOPS'),
      el('input', { id: 'blkio-wr-iops', type: 'number', value: '0', min: '0', placeholder: '0', class: 'w-140' })),
    el('div', { id: 'blkio-status', style: 'font-size:11px;min-height:20px;margin:8px 0' }),
    el('div', { class: 'text-right mt-14' },
      el('button', { class: 'btn', onclick: 'blkioGet()', style: 'margin-right:4px' }, '🔍 ' + (t('vm.blkio_get') || 'Get Current')),
      el('button', { class: 'btn btn-g', onclick: 'blkioSet()' }, '✅ ' + (t('vm.blkio_apply') || 'Apply')),
      ' ',
      el('button', { class: 'btn', onclick: 'closeModal()' }, t('btn.cancel')))
  ]);
}

async function blkioGet() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var el = document.getElementById('blkio-status');
  if (el) PCV.uxlib.setMsg(el, 'loading', null, t('loading') || 'Loading...');
  try {
    var r = await fetchPost(EP.VM_RPC(v.name), { method: 'vm.blkio.get', params: { name: v.name } });
    if (r.error) { if (el) PCV.uxlib.setMsg(el, 'err', null, r.error.message || 'Failed'); return; }
    var d = unwrapData(r);
    var rdB = document.getElementById('blkio-rd-bytes');
    var wrB = document.getElementById('blkio-wr-bytes');
    var rdI = document.getElementById('blkio-rd-iops');
    var wrI = document.getElementById('blkio-wr-iops');
    if (rdB) rdB.value = Math.round((d.read_bytes_sec || 0) / 1048576);
    if (wrB) wrB.value = Math.round((d.write_bytes_sec || 0) / 1048576);
    if (rdI) rdI.value = d.read_iops_sec || 0;
    if (wrI) wrI.value = d.write_iops_sec || 0;
    if (el) PCV.uxlib.setMsg(el, 'ok', null, '✅ ' + (t('vm.blkio_loaded') || 'Current limits loaded'));
  } catch (e) {
    if (el) PCV.uxlib.setMsg(el, 'err', null, e.message);
  }
}

async function blkioSet() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var rdMB = parseInt((document.getElementById('blkio-rd-bytes') || {}).value) || 0;
  var wrMB = parseInt((document.getElementById('blkio-wr-bytes') || {}).value) || 0;
  var rdIops = parseInt((document.getElementById('blkio-rd-iops') || {}).value) || 0;
  var wrIops = parseInt((document.getElementById('blkio-wr-iops') || {}).value) || 0;
  var el = document.getElementById('blkio-status');
  if (el) PCV.uxlib.setMsg(el, 'loading', null, t('vm.blkio_applying') || 'Applying...');
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
      if (el) PCV.uxlib.setMsg(el, 'err', null, r.error.message || 'Failed');
      toast((t('vm.blkio_failed') || 'I/O limit failed') + ': ' + (r.error.message || ''), false);
      return;
    }
    if (el) PCV.uxlib.setMsg(el, 'ok', null, '✅ ' + (t('vm.blkio_applied') || 'I/O limits applied'));
    toast((t('vm.blkio_applied') || 'I/O limits applied') + ': ' + v.name);
    addEvt('BlkIO set: ' + v.name + ' R:' + rdMB + 'MB/s W:' + wrMB + 'MB/s');
    setTimeout(closeModal, 1500);
  } catch (e) {
    if (el) PCV.uxlib.setMsg(el, 'err', null, e.message);
    toast(e.message, false);
  }
}

/* ═══ EXPORT TO PCV NAMESPACE (ADR-0013) ═══
 *  PCV.vm에 등록되는 함수가 이 모듈의 공식 인터페이스.
 *  아래 BACKWARD COMPAT SHIMS는 HTML onclick과 다른 모듈의
 *  window.render() 등 직접 참조를 위한 전환기 코드.
 *  신규 코드에서는 PCV.vm.render() 사용을 권장. */
PCV.vm = Object.assign(PCV.vm || {}, {
  showMemStats: showMemStats,
  showCpuStats: showCpuStats,
  showDiskLiveResize: showDiskLiveResize,
  showVmDiskUsage: showVmDiskUsage,
  showGuestAgent: showGuestAgent,
  gaRefreshStatus: gaRefreshStatus,
  gaEnsureChannel: gaEnsureChannel,
  showBlkioEditor: showBlkioEditor,
  vmMigrateDrop: vmMigrateDrop,
});

/* ═══ BACKWARD COMPAT SHIMS (ADR-0013: remove after full transition) ═══ */
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
window.vmMigrateDrop = vmMigrateDrop;

})(window.PCV);
