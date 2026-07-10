/* ═══════════════════════════════════════════════════════════════
   PureCVisor — modules/vm-guest.js
   CPU Pinning, QoS, Mem/CPU Stats, Disk Resize, Guest Usage/Agent, Migration, I/O Throttle
   ADR-0013: IIFE module scope — vm.js에서 분할 (pure-move)
   ═══════════════════════════════════════════════════════════════ */
window.PCV = window.PCV || {};
(function(PCV) {

/* ═══ CPU PINNING ═══ */
function hwCpuPin() {
  return '<h4>&#128204; CPU Pinning</h4><p class="stat-label mb-8">Pin vCPUs to physical cores for performance isolation.</p><div class="fr"><label for="scpin">vCPU Map</label><input id="scpin" placeholder="0:0,1:2,2:4" class="flex-1"></div><p class="stat-label">Format: vCPU:pCPU pairs, comma separated (e.g., 0:0,1:2)</p><button class="btn btn-g mt-8" onclick="doCpuPin()">' + t('btn.apply') + '</button>';
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
  return '<h4>&#128246; Network Bandwidth (QoS)</h4><p class="stat-label mb-8">Set network bandwidth limits for VM interfaces.</p><div class="fr"><label for="sbw-in">Inbound (Mbps)</label><input id="sbw-in" type="number" value="1000" placeholder="1000"></div><div class="fr"><label for="sbw-out">Outbound (Mbps)</label><input id="sbw-out" type="number" value="1000" placeholder="1000"></div><div class="fr"><label for="sbw-burst">Burst (KB)</label><input id="sbw-burst" type="number" value="1024" placeholder="1024"></div><button class="btn btn-g mt-8" onclick="doBandwidth()">' + t('btn.apply') + '</button>';
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
    // var 아님(no-redeclare) — try 블록의 `var el`이 함수 스코프에 이미
    // hoisting 되어 있으므로 재선언 없이 재대입 (동작 동일).
    el = document.getElementById('mem-stats-body');
    if (el) el.innerHTML = '<p class="color-red">Failed: ' + esc(e.message) + '</p>';
  }
}

/* ═══ VM CPU STATS ═══ */
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
    // var 아님(no-redeclare) — try 블록의 `var el`이 함수 스코프에 이미
    // hoisting 되어 있으므로 재선언 없이 재대입 (동작 동일).
    el = document.getElementById('cpu-stats-body');
    if (el) el.innerHTML = '<p class="color-red">Failed: ' + esc(e.message) + '</p>';
  }
}

/* ═══ VM DISK LIVE RESIZE (MODAL) ═══ */
function showDiskLiveResize() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var h = '<h2>&#128190; Disk Live Resize: ' + esc(v.name) + '</h2>';
  h += '<p class="stat-label mb-12">Resize a VM disk while the VM is running. The guest OS may need to rescan partitions.</p>';
  h += '<div class="fr"><label for="dlr-target">Target Device</label><input id="dlr-target" value="vda" placeholder="vda" class="w-120"></div>';
  h += '<div class="fr"><label for="dlr-size">New Size (GB)</label><input id="dlr-size" type="number" value="40" min="1" placeholder="40" class="w-120"></div>';
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

/* ═══ VM GUEST DISK USAGE ═══ */
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

/* ═══ GUEST AGENT ═══ */
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
  h += '<div class="fr"><label for="ga-cmd">Command</label><input id="ga-cmd" placeholder="cat /etc/hostname" class="flex-1"></div>';
  h += '<div class="fr"><label for="ga-args">Args</label><input id="ga-args" placeholder="(optional, space separated)" class="flex-1"></div>';
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

/* ═══ D3: DRAG & DROP VM MIGRATION ═══ */
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
    // var 아님(no-redeclare) — try 블록의 `var ps`가 함수 스코프에 이미
    // hoisting 되어 있으므로 재선언 없이 재대입 (동작 동일).
    ps = document.getElementById('mig-st');
    if (ps) ps.innerHTML = '&#10060; ' + esc(e.message);
  }
}
/* ═══ DISK I/O THROTTLE EDITOR ═══ */
function showBlkioEditor() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var h = '<h2>&#128190; ' + (t('vm.blkio_title') || 'Disk I/O Limits') + ': ' + esc(v.name) + '</h2>';
  h += '<p class="stat-label mb-12">'
    + (t('vm.blkio_desc') || 'Set disk I/O throttle limits. Values in bytes/sec and IOPS. Set 0 for unlimited.')
    + '</p>';
  h += '<div class="fr"><label for="blkio-rd-bytes">' + (t('vm.read_bytes_sec') || 'Read (MB/s)') + '</label><input id="blkio-rd-bytes" type="number" value="0" min="0" placeholder="0" class="w-140"><span class="stat-label ml-4">MB/s</span></div>';
  h += '<div class="fr"><label for="blkio-wr-bytes">' + (t('vm.write_bytes_sec') || 'Write (MB/s)') + '</label><input id="blkio-wr-bytes" type="number" value="0" min="0" placeholder="0" class="w-140"><span class="stat-label ml-4">MB/s</span></div>';
  h += '<div class="fr"><label for="blkio-rd-iops">' + (t('vm.read_iops_sec') || 'Read IOPS') + '</label><input id="blkio-rd-iops" type="number" value="0" min="0" placeholder="0" class="w-140"></div>';
  h += '<div class="fr"><label for="blkio-wr-iops">' + (t('vm.write_iops_sec') || 'Write IOPS') + '</label><input id="blkio-wr-iops" type="number" value="0" min="0" placeholder="0" class="w-140"></div>';
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
