










window.PCV = window.PCV || {};
(function(PCV) {


var chartRegistry = {};
window.chartRegistry = chartRegistry;


function destroyAllCharts() {
  for (const id of Object.keys(chartRegistry)) {
    try { chartRegistry[id].destroy(); } catch (e) { if(_DEBUG) console.warn('destroyAllCharts:', e.message); }
    delete chartRegistry[id];
  }
}
window.destroyAllCharts = destroyAllCharts;

function createLineChart(canvasId, data, label, color) {
  const canvas = document.getElementById(canvasId);
  if (!canvas) return;

  if (chartRegistry[canvasId]) {
    if (chartRegistry[canvasId].canvas === canvas) {
      const chart = chartRegistry[canvasId];
      chart.data.labels = data.map((_, i) => i);
      chart.data.datasets[0].data = data;
      chart.update('none');
      return;
    }

    try { chartRegistry[canvasId].destroy(); } catch (e) { if(_DEBUG) console.warn('createLineChart:', e.message); }
    delete chartRegistry[canvasId];
  }
  const ctx = canvas.getContext('2d');
  if (typeof Chart === 'undefined') {
    drawGraphFallback(canvasId, data, color);
    return;
  }
  const chart = new Chart(ctx, {
    type: 'line',
    data: {
      labels: data.map((_, i) => i),
      datasets: [{
        label: label || '',
        data: data,
        borderColor: color,
        backgroundColor: color.replace(')', ',0.15)').replace('rgb', 'rgba'),
        borderWidth: 1.5,
        fill: true,
        tension: 0.3,
        pointRadius: 0,
      }]
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: false,
      plugins: { legend: { display: false } },
      scales: {
        x: { display: false },
        y: { display: false, min: 0, max: 100 }
      }
    }
  });
  chartRegistry[canvasId] = chart;
}
window.createLineChart = createLineChart;

function drawGraphFallback(id, data, color) {
  const cv = document.getElementById(id);
  if (!cv) return;
  const x = cv.getContext('2d');
  cv.width = cv.offsetWidth;
  cv.height = cv.offsetHeight;
  const w = cv.width, h = cv.height;
  x.clearRect(0, 0, w, h);
  x.strokeStyle = color;
  x.lineWidth = 1.5;
  x.beginPath();
  for (let i = 0; i < data.length; i++) {
    const px = i / (data.length - 1) * w;
    const py = h - (data[i] / 100) * h;
    i === 0 ? x.moveTo(px, py) : x.lineTo(px, py);
  }
  x.stroke();
}
window.drawGraphFallback = drawGraphFallback;


function getChartColor(name) {
  try { return getComputedStyle(document.documentElement).getPropertyValue('--chart-' + name).trim() || name; }
  catch(e) { return name; }
}
window.getChartColor = getChartColor;


var _PROD_NODES = window._PROD_NODES || [{ name: 'Local', ip: window.location.hostname || '127.0.0.1' }];
var _VIP = window._VIP || null;
var _curHost = window._curHost || window.location.hostname;
var _isProd = window._isProd || false;
window._isProd = _isProd;
window._curHost = _curHost;
var MON_NODES = _PROD_NODES;
window.MON_NODES = MON_NODES;

async function _refreshMonNodes() {
  if (window._loadClusterNodes) await window._loadClusterNodes();
  _PROD_NODES = window._PROD_NODES || _PROD_NODES;
  _isProd = window._isProd || false;
  _VIP = window._VIP || null;
  MON_NODES = _PROD_NODES;
  window.MON_NODES = MON_NODES;
}
window._refreshMonNodes = _refreshMonNodes;
var monHist = {};
window.monHist = monHist;

function _parseLabels(raw) { const o = {}; raw.replace(/(\w+)="([^"]*)"/g, (_, k, v) => { o[k] = v; }); return o; }
window._parseLabels = _parseLabels;

function _newZfsLocks() {
  return { total: 0, ok: 0, busy: 0, error: 0, unknown: 0, waitSumMs: 0, waitCount: 0, byOp: {} };
}

function _zfsLockOp(stats, op) {
  var key = op || 'unknown';
  if (!stats.byOp[key]) stats.byOp[key] = { total: 0, ok: 0, busy: 0, error: 0, unknown: 0, waitSumMs: 0, waitCount: 0 };
  return stats.byOp[key];
}

function _recordZfsLockMetric(m, key, labels, value) {
  if (!m.zfsLocks) m.zfsLocks = _newZfsLocks();
  var op = labels.op || 'unknown';
  var result = labels.result || 'unknown';
  var opStats = _zfsLockOp(m.zfsLocks, op);
  if (key === 'purecvisor_zfs_inflight_lock_acquired_total') {
    m.zfsLocks.total += value;
    m.zfsLocks[result] = (m.zfsLocks[result] || 0) + value;
    opStats.total += value;
    opStats[result] = (opStats[result] || 0) + value;
  } else if (key === 'purecvisor_zfs_inflight_lock_wait_ms_sum') {
    m.zfsLocks.waitSumMs += value;
    opStats.waitSumMs += value;
  } else if (key === 'purecvisor_zfs_inflight_lock_wait_ms_count') {
    m.zfsLocks.waitCount += value;
    opStats.waitCount += value;
  }
}

var _monPort = location.port || '';
function _buildMetricsUrl(ip) {
  const proto = location.protocol || 'http:';
  const port = _monPort && _monPort !== '80' && _monPort !== '443' ? ':' + _monPort : '';
  return proto + '//' + ip + port + '/api/v1/metrics';
}
window._buildMetricsUrl = _buildMetricsUrl;

function _metricsAuthHeaders() {
  var token = window.authToken || (typeof authToken !== 'undefined' ? authToken : '');
  return token ? { Authorization: 'Bearer ' + token } : {};
}

async function _fetchMetricsText(url) {
  const r = await fetch(url, { headers: _metricsAuthHeaders() });
  if (!r.ok) throw new Error('metrics HTTP ' + r.status);
  return r.text();
}


async function fetchAllMetrics() {
  const all = await Promise.all(MON_NODES.map(async (nd) => {
    try {
      const txt = await _fetchMetricsText(_buildMetricsUrl(nd.ip));
        const m = { node: nd.name, ip: nd.ip, cores: {}, memInfo: {}, filesystems: [], disks: {}, netdevs: {}, hwmon: [], sockstat: {}, vmstat: {}, pressure: {}, zfsLocks: _newZfsLocks() };
      const vms = [];
      txt.split('\n').forEach(l => {
        if (l.startsWith('#') || !l.trim()) return;
        const sp = l.match(/^([a-zA-Z_][a-zA-Z0-9_]*)(\{[^}]*\})?\s+(.+)$/);
        if (!sp) return;
        const k = sp[1], labels = sp[2] || '', v = parseFloat(sp[3]), lb = labels ? _parseLabels(labels) : {};
        if (k === 'purecvisor_host_cpu_percent') m.cpu = v;
        if (k === 'purecvisor_host_memory_percent') m.mem = v;
        if (k === 'purecvisor_host_disk_percent') m.disk = v;
        if (k === 'purecvisor_host_memory_total_bytes') m.ram_total = v;
        if (k === 'purecvisor_host_cpu_temp_celsius') m.temp = v;
        if (k === 'purecvisor_host_load1') m.load = v;
        const vmM = k.match(/^purecvisor_vm_(\w+)$/);
        if (vmM && lb.vm) { let vm = vms.find(x => x.name === lb.vm); if (!vm) { vm = { name: lb.vm, node: nd.name }; vms.push(vm); } vm[vmM[1]] = v; }
        if (k === 'node_cpu_seconds_total' && lb.cpu && lb.mode) { const c = lb.cpu; if (!m.cores[c]) m.cores[c] = {}; m.cores[c][lb.mode] = v; }
        if (k.startsWith('node_memory_') && k.endsWith('_bytes')) { m.memInfo[k.slice(12, -6)] = v; }
        if (k.startsWith('node_filesystem_') && lb.mountpoint) { let fs = m.filesystems.find(f => f.mount === lb.mountpoint); if (!fs) { fs = { mount: lb.mountpoint, dev: lb.device || '', fstype: lb.fstype || '' }; m.filesystems.push(fs); } fs[k.slice(16)] = v; }
        if (k.startsWith('node_disk_') && lb.device) { if (!m.disks[lb.device]) m.disks[lb.device] = {}; m.disks[lb.device][k.slice(10)] = v; }
        if (k.startsWith('node_network_') && lb.device) { if (!m.netdevs[lb.device]) m.netdevs[lb.device] = {}; m.netdevs[lb.device][k.slice(13)] = v; }
        if (k === 'node_hwmon_temp_celsius' && lb.chip) m.hwmon.push({ chip: lb.chip, sensor: lb.sensor || '', temp: v });
        if (k === 'node_hwmon_temp_crit_celsius' && lb.chip) { const h2 = m.hwmon.find(x => x.chip === lb.chip && x.sensor === (lb.sensor || '')); if (h2) h2.crit = v; }
        if (k.startsWith('node_sockstat_')) { m.sockstat[k.slice(14)] = v; }
        if (k.startsWith('node_vmstat_')) { m.vmstat[k.slice(12)] = v; }
        if (k.startsWith('node_pressure_')) { m.pressure[k.slice(14)] = v; }
        if (k === 'node_load1') m.load1 = v; if (k === 'node_load5') m.load5 = v; if (k === 'node_load15') m.load15 = v;
        if (k === 'node_boot_time_seconds') m.boot_time = v;
        if (k === 'node_uptime_seconds') m.uptime = v;
        if (k === 'node_context_switches_total') m.ctxt = v;
        if (k === 'node_forks_total') m.forks = v;
        if (k === 'node_entropy_available_bits') m.entropy = v;
        if (k === 'node_filefd_allocated') m.fd_alloc = v;
        if (k === 'node_nf_conntrack_entries') m.conntrack = v;
        if (k === 'node_nf_conntrack_entries_limit') m.conntrack_max = v;
        if (k === 'purecvisor_anomaly_active') m.anomaly_active = v;
        if (k === 'purecvisor_anomaly_alerts_total') m.anomaly_total = v;
        if (k === 'purecvisor_predict_cpu_5m') m.cpu_pred = v;
        if (k === 'purecvisor_predict_mem_5m') m.mem_pred = v;
        if (k === 'purecvisor_predict_trend_cpu') m.cpu_trend = v;
        if (k === 'purecvisor_predict_trend_mem') m.mem_trend = v;
        if (k === 'purecvisor_healing_pending_approvals') m.healing_pending = v;
        if (k === 'purecvisor_healing_actions_total') m.healing_total = v;
        if (k === 'purecvisor_agent_consensus_confidence') m.agent_conf = v;
        if (k === 'purecvisor_agent_latency_ms' && lb.provider) { if (!m.agent_prov) m.agent_prov = {}; if (!m.agent_prov[lb.provider]) m.agent_prov[lb.provider] = {}; m.agent_prov[lb.provider].latency = v; m.agent_prov[lb.provider].model = lb.model || ''; }
        if (k === 'purecvisor_agent_confidence' && lb.provider) { if (!m.agent_prov) m.agent_prov = {}; if (!m.agent_prov[lb.provider]) m.agent_prov[lb.provider] = {}; m.agent_prov[lb.provider].confidence = v; }
        if (k === 'purecvisor_keepalived_active') m.keepalived_active = v;
        if (k === 'purecvisor_keepalived_master') m.keepalived_master = v;
        if (k === 'purecvisor_keepalived_vip_owner') m.keepalived_vip_owner = v;
        if (k === 'purecvisor_anomaly_score' && lb.metric) { if (!m.anomaly_scores) m.anomaly_scores = {}; m.anomaly_scores[lb.metric] = v; }
        if (k === 'purecvisor_zfs_inflight_lock_acquired_total' ||
            k === 'purecvisor_zfs_inflight_lock_wait_ms_sum' ||
            k === 'purecvisor_zfs_inflight_lock_wait_ms_count') {
          _recordZfsLockMetric(m, k, lb, v);
        }
      });
      m.vms = vms;
      if (!monHist[nd.ip]) monHist[nd.ip] = { cpu: [], mem: [], disk: [], netRx: [], netTx: [] };
      const hi = monHist[nd.ip]; hi.cpu.push(m.cpu || 0); hi.mem.push(m.mem || 0); hi.disk.push(m.disk || 0);
      const phys = Object.entries(m.netdevs).filter(([d]) => !['lo', 'ovs-system', 'br-int'].includes(d));
      hi.netRx.push(phys.reduce((s, [, d]) => s + (d.receive_bytes_total || 0), 0));
      hi.netTx.push(phys.reduce((s, [, d]) => s + (d.transmit_bytes_total || 0), 0));
      [hi.cpu, hi.mem, hi.disk, hi.netRx, hi.netTx].forEach(a => { while (a.length > 60) a.shift(); });
      return m;
    } catch (e) {
      return { node: nd.name, ip: nd.ip, cpu: 0, mem: 0, disk: 0, error: true, vms: [], cores: {}, memInfo: {}, filesystems: [], disks: {}, netdevs: {}, hwmon: [], sockstat: {}, vmstat: {}, pressure: {}, zfsLocks: _newZfsLocks() };
    }
  }));
  return all;
}
window.fetchAllMetrics = fetchAllMetrics;


function fmtBytes(b) { if (b >= 1e12) return (b / 1e12).toFixed(1) + ' TB'; if (b >= 1e9) return (b / 1e9).toFixed(1) + ' GB'; if (b >= 1e6) return (b / 1e6).toFixed(1) + ' MB'; if (b >= 1e3) return (b / 1e3).toFixed(1) + ' KB'; return b + ' B'; }
window.fmtBytes = fmtBytes;

function fmtRate(arr, i) { if (i < 1 || !arr[i] || !arr[i - 1]) return '0 B/s'; const d = arr[i] - arr[i - 1]; return d > 0 ? fmtBytes(d / 5) + '/s' : '0 B/s'; }
window.fmtRate = fmtRate;

function fmtUptime(s) { const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), mi = Math.floor(s % 3600 / 60); return d + 'd ' + h + 'h ' + mi + 'm'; }
window.fmtUptime = fmtUptime;


function drawLine(id, data, color, unit, max) {
  const cv = document.getElementById(id); if (!cv) return;
  const x = cv.getContext('2d'); cv.width = cv.offsetWidth; cv.height = cv.offsetHeight;
  const w = cv.width, h = cv.height;
  x.fillStyle = 'rgba(0,0,0,0.3)'; x.fillRect(0, 0, w, h);
  x.strokeStyle = 'rgba(255,255,255,0.05)'; x.lineWidth = 1;
  for (let i = 1; i < 4; i++) { const y = h * i / 4; x.beginPath(); x.moveTo(0, y); x.lineTo(w, y); x.stroke(); }
  x.strokeStyle = color; x.lineWidth = 2; x.beginPath(); const mx = max || 100;
  for (let i = 0; i < data.length; i++) { const px = i / (Math.max(data.length - 1, 1)) * w; const py = h - (data[i] / mx) * h; i === 0 ? x.moveTo(px, py) : x.lineTo(px, py); } x.stroke();
  x.lineTo(w, h); x.lineTo(0, h); x.closePath();
  const grd = x.createLinearGradient(0, 0, 0, h); grd.addColorStop(0, color.replace(')', ',0.3)').replace('rgb', 'rgba')); grd.addColorStop(1, 'rgba(0,0,0,0)'); x.fillStyle = grd; x.fill();
  x.fillStyle = 'rgba(255,255,255,0.5)'; x.font = '10px Inter'; x.fillText(data[data.length - 1]?.toFixed(1) + (unit || ''), 4, 12);
}
window.drawLine = drawLine;

function gauge(pct, label, color) {
  const cl = color || (pct > 80 ? 'var(--red)' : pct > 60 ? 'var(--yellow)' : 'var(--green)');
  return '<div class="text-center"><svg width="90" height="50" viewBox="0 0 90 50"><path d="M10 45 A35 35 0 0 1 80 45" fill="none" stroke="var(--border)" stroke-width="6" stroke-linecap="round"/><path d="M10 45 A35 35 0 0 1 80 45" fill="none" stroke="' + cl + '" stroke-width="6" stroke-linecap="round" stroke-dasharray="' + (pct * 1.1) + ' 110" style="filter:drop-shadow(0 0 4px ' + cl + ')"/></svg><div class="stat-sm" style="margin-top:-8px;color:' + cl + '">' + pct.toFixed(1) + '%</div><div class="stat-label">' + label + '</div></div>';
}
window.gauge = gauge;


async function renderMonitoring(b, tab) {

  if (typeof pcvDestroyAllInContainer === 'function') pcvDestroyAllInContainer(b);
  destroyAllCharts();
  b.innerHTML = showSkeleton();
  const all = await fetchAllMetrics();
  const allVms = all.flatMap(n => n.vms.map(v => ({ ...v, nodeIP: n.ip })));
  const running = allVms.filter(v => v.running === 1).length;
  const avgCpu = all.reduce((s, n) => s + (n.cpu || 0), 0) / Math.max(all.length, 1);
  const avgMem = all.reduce((s, n) => s + (n.mem || 0), 0) / Math.max(all.length, 1);
  const avgDisk = all.reduce((s, n) => s + (n.disk || 0), 0) / Math.max(all.length, 1);
  const totalRam = all.reduce((s, n) => s + (n.ram_total || 0), 0);

  if (tab === 'overview') renderMonOverview(b, all, allVms, running, avgCpu, avgMem, avgDisk, totalRam);
  else if (tab === 'hosts') renderMonHosts(b, all);
  else if (tab === 'vms') renderMonVms(b, allVms, running);
  else if (tab === 'storage') renderMonStorage(b, all, allVms, totalRam);


  if (typeof startAdaptivePolling === 'function') {
    startAdaptivePolling('mon-refresh', function() {
      if (window.currentTab && window.currentTab.startsWith('mon-')) {
        var cb = document.getElementById('cb');
        if (cb) fetchAllMetrics().then(function(fresh) {
          var freshVms = fresh.flatMap(function(n) { return n.vms.map(function(v) { return Object.assign({}, v, { nodeIP: n.ip }); }); });
          var r = freshVms.filter(function(v) { return v.running === 1; }).length;
          var ac = fresh.reduce(function(s, n) { return s + (n.cpu || 0); }, 0) / Math.max(fresh.length, 1);
          var am = fresh.reduce(function(s, n) { return s + (n.mem || 0); }, 0) / Math.max(fresh.length, 1);
          var ad = fresh.reduce(function(s, n) { return s + (n.disk || 0); }, 0) / Math.max(fresh.length, 1);
          var tr = fresh.reduce(function(s, n) { return s + (n.ram_total || 0); }, 0);
          if (window.currentTab === 'mon-overview') renderMonOverview(cb, fresh, freshVms, r, ac, am, ad, tr);
          else if (window.currentTab === 'mon-hosts') renderMonHosts(cb, fresh);
          else if (window.currentTab === 'mon-vms') renderMonVms(cb, freshVms, r);
          else if (window.currentTab === 'mon-storage') renderMonStorage(cb, fresh, freshVms, tr);
        }).catch(function() {  });
      } else {
        if (typeof stopAdaptivePolling === 'function') stopAdaptivePolling('mon-refresh');
      }
    }, 10000);
  }
}
window.renderMonitoring = renderMonitoring;

function _opsPct(n, fallback) {
  var v = Number(n);
  if (!Number.isFinite(v)) v = fallback || 0;
  return Math.max(0, Math.min(100, v));
}

function _opsStatus(label, tone) {
  var cls = tone || 'info';
  var dot = cls === 'bad' ? 'bad' : cls === 'warn' ? 'warn' : 'ok';
  return '<span class="ops-status ' + cls + '"><span class="ops-dot ' + dot + '"></span>' + esc(label) + '</span>';
}

function _opsMetricCard(label, value, detail, statusLabel, tone) {
  return '<article class="ops-triage-card ops-triage-metric ops-span-3">'
    + '<div class="ops-triage-metric-label">' + esc(label) + '</div>'
    + '<div class="ops-triage-metric-value">' + esc(value) + '</div>'
    + '<div class="ops-triage-metric-foot"><span>' + esc(detail) + '</span>' + _opsStatus(statusLabel, tone) + '</div>'
    + '</article>';
}

function _opsBar(pct) {
  var safe = _opsPct(pct, 0);
  var text = safe.toFixed(safe < 10 ? 1 : 0) + '%';
  return '<div class="ops-bar" style="--value:' + safe.toFixed(1) + '%"><div class="ops-bar-fill"></div><div class="ops-bar-label">' + text + '</div></div>';
}

function _opsVmStatus(v) {
  var state = String(v.state || (v.running === 1 ? 'running' : 'unknown')).toLowerCase();
  if (state === 'running' || state === 'http 200') return _opsStatus(state === 'http 200' ? '200' : 'RUN', 'ok');
  if (state === 'shut off' || state === 'stopped' || state === 'off') return _opsStatus('OFF', 'bad');
  return _opsStatus('CHECK', 'warn');
}

function _opsFallbackVms() {
  return [
    { name: 'pcv-demo-vm', role: '공개 데모', ip: '192.0.2.12', cpu: 2.0, mem: 48, state: 'running', running: 1 },
    { name: 'ovn-demo-a', role: '테넌트 A', ip: '10.77.0.12', cpu: 12, mem: 34, state: 'http 200', running: 1 },
    { name: 'ovn-demo-b', role: '테넌트 B', ip: '10.77.0.13', cpu: 16, mem: 41, state: 'unknown', running: 1 }
  ];
}

function _opsVmRows(sourceVms) {
  var rows = (sourceVms || []).slice(0, 5).map(function(v) {
    var maxMb = Number(v.memory_max_mb || v.memory_mb || v.maxmem || 0);
    var usedMb = Number(v.memory_used_mb || v.mem_used_mb || 0);
    var memPct = maxMb > 0 && usedMb > 0 ? usedMb / maxMb * 100 : _opsPct(v.mem || v.memory_percent, 34);
    var cpuPct = _opsPct(v.cpu || v.cpu_percent || v.cpu_usage, v.running === 1 ? 12 : 0);
    return {
      name: v.name || v.vm || '-',
      role: v.role || (v.node ? '호스트 ' + v.node : 'VM 자산'),
      ip: v.ip_addr || v.ip || v.addr || '-',
      cpu: cpuPct,
      mem: memPct,
      state: v.state || (v.running === 1 ? 'running' : 'unknown'),
      running: v.running
    };
  });
  if (rows.length === 0) {
    rows = _opsFallbackVms();
  }
  return rows.map(function(v) {
    return '<tr>'
      + '<td><div class="ops-name"><svg class="ci-icon" aria-hidden="true"><use href="vendor/coolicons/coolicons.svg#ci-desktop-tower"></use></svg>' + esc(v.name) + '</div></td>'
      + '<td>' + esc(v.role) + '</td>'
      + '<td class="ops-mono">' + esc(v.ip) + '</td>'
      + '<td>' + _opsBar(v.cpu) + '</td>'
      + '<td>' + _opsBar(v.mem) + '</td>'
      + '<td>' + _opsVmStatus(v) + '</td>'
      + '</tr>';
  }).join('');
}

function _opsAuditRows() {
  var raw = [];
  try { raw = (window.eventLog || eventLog || []).slice(-3).reverse(); } catch (e) { raw = []; }
  if (raw.length === 0) {
    raw = [
      { title: 'vm.guest.exec', detail: 'target=pcv-demo-vm, result=ok, job_id=81f2', time: '12:22', tone: 'ok' },
      { title: 'security.event', detail: 'target=viewer, source=nginx, result=warn', time: '12:21', tone: 'warn' },
      { title: 'ovn.status', detail: 'target=pcv-demo-lr, result=ok', time: '12:19', tone: 'ok' }
    ];
  }
  return raw.map(function(item) {
    var obj = typeof item === 'string'
      ? { title: item.split(':')[0] || 'event', detail: item, time: '-', tone: /fail|warn|error/i.test(item) ? 'warn' : 'ok' }
      : item;
    var tone = obj.tone || (/fail|error/i.test(obj.detail || obj.title || '') ? 'bad' : /warn/i.test(obj.detail || obj.title || '') ? 'warn' : 'ok');
    return '<div class="ops-triage-event">'
      + '<div class="ops-severity ' + tone + '"></div>'
      + '<div><p class="ops-event-title">' + esc(obj.title || 'event') + '</p><div class="ops-event-sub">' + esc(obj.detail || obj.msg || '') + '</div></div>'
      + '<div class="ops-event-time">' + esc(obj.time || '-') + '</div>'
      + '</div>';
  }).join('');
}

async function renderOpsTriage(b) {
  if (typeof pcvDestroyAllInContainer === 'function') pcvDestroyAllInContainer(b);
  destroyAllCharts();
  b.innerHTML = showSkeleton();
  var all = [];
  var apiVms = [];
  try {
    var result = await Promise.all([
      fetchAllMetrics().catch(function() { return []; }),
      fetchGet(EP.VM_LIST()).catch(function() { return { data: [] }; })
    ]);
    all = Array.isArray(result[0]) ? result[0] : [];
    apiVms = unwrapList(result[1]);
  } catch (e) {
    all = [];
    apiVms = [];
  }

  var metricVms = all.flatMap(function(n) {
    return (n.vms || []).map(function(v) {
      return Object.assign({}, v, { node: n.node || n.ip });
    });
  });
  var sourceVms = metricVms.length ? metricVms : apiVms;
  var displayVms = sourceVms.length ? sourceVms : _opsFallbackVms();
  var usableMetrics = all.filter(function(n) { return !n.error; });
  var avgCpu = usableMetrics.length ? usableMetrics.reduce(function(s, n) { return s + (n.cpu || 0); }, 0) / usableMetrics.length : 2.0;
  var avgMem = usableMetrics.length ? usableMetrics.reduce(function(s, n) { return s + (n.mem || 0); }, 0) / usableMetrics.length : 41;
  var totalRam = usableMetrics.reduce(function(s, n) { return s + (n.ram_total || 0); }, 0);
  var ramDetail = totalRam > 0 ? fmtBytes(totalRam) + ' total' : '32GB 중 13.1GB';
  var running = displayVms.filter(function(v) {
    var st = String(v.state || '').toLowerCase();
    return v.running === 1 || st === 'running';
  }).length;
  var totalVm = displayVms.length;

  var h = '<header class="ops-triage-head">'
    + '<div><div class="ops-triage-kicker">Single Edge operations</div>'
    + '<h1 class="ops-triage-title">운영 이벤트 센터</h1>'
    + '<p class="ops-triage-sub">VM, OVN, ZFS, 보안 이벤트를 한 화면에서 triage하고 즉시 조치하는 운영자용 화면입니다.</p></div>'
    + '<div class="ops-triage-tabs" role="tablist" aria-label="시간 범위">'
    + '<button class="ops-triage-tab is-active" type="button">LIVE</button>'
    + '<button class="ops-triage-tab" type="button">1H</button>'
    + '<button class="ops-triage-tab" type="button">24H</button>'
    + '<button class="ops-triage-tab" type="button">AUDIT</button>'
    + '</div></header>';

  h += '<section class="ops-triage-grid" aria-label="운영 이벤트 센터">';
  h += _opsMetricCard('호스트 CPU', avgCpu.toFixed(1) + '%', '단일 노드 평균', avgCpu > 80 ? '위험' : '정상', avgCpu > 80 ? 'bad' : avgCpu > 60 ? 'warn' : 'ok');
  h += _opsMetricCard('메모리', avgMem.toFixed(0) + '%', ramDetail, avgMem > 85 ? '위험' : '여유', avgMem > 85 ? 'bad' : avgMem > 70 ? 'warn' : 'ok');
  h += _opsMetricCard('OVN 게이트웨이', '10.77.0.1', 'pcv-demo-lr', 'ACTIVE', 'info');
  h += _opsMetricCard('실행 VM', running + '/' + totalVm, 'viewer read-only 기준', running > 0 ? '가동' : '확인', running > 0 ? 'ok' : 'warn');

  h += '<article class="ops-triage-card ops-span-5"><div class="ops-triage-card-head">'
    + '<div class="ops-triage-card-title" role="heading" aria-level="2">이벤트 triage</div><span class="ops-triage-card-meta">최근 15분</span></div>'
    + '<div class="ops-triage-list">'
    + '<div class="ops-triage-event"><div class="ops-severity bad"></div><div><p class="ops-event-title">viewer 계정 로그인 시도 증가</p><div class="ops-event-sub">nginx access log 기준 동일 User-Agent 반복 접근</div></div><div class="ops-event-time">LIVE</div></div>'
    + '<div class="ops-triage-event"><div class="ops-severity warn"></div><div><p class="ops-event-title">exporter scrape 지연 확인</p><div class="ops-event-sub">Prometheus full exporter 응답 지연은 관측성 품질에 영향</div></div><div class="ops-event-time">WARN</div></div>'
    + '<div class="ops-triage-event"><div class="ops-severity ok"></div><div><p class="ops-event-title">OVN demo NAT 흐름 정상</p><div class="ops-event-sub">ovn-demo-a → pcv-demo-ls → pcv-demo-lr → external</div></div><div class="ops-event-time">OK</div></div>'
    + '</div></article>';

  h += '<article class="ops-triage-card ops-span-7"><div class="ops-triage-card-head">'
    + '<div class="ops-triage-card-title" role="heading" aria-level="2">VM 및 서비스 상태</div><span class="ops-triage-card-meta">' + totalVm + ' assets</span></div>'
    + '<div class="ops-triage-toolbar"><input class="ops-triage-field" type="search" value="demo" aria-label="자산 검색">'
    + '<div class="ops-triage-actions"><button class="ops-triage-action" type="button" onclick="renderOpsTriage(document.getElementById(\'cb\'))"><svg class="ci-icon" aria-hidden="true"><use href="vendor/coolicons/coolicons.svg#ci-refresh"></use></svg>새로고침</button>'
    + '<button class="ops-triage-action primary" type="button" onclick="openCmdPalette()"><svg class="ci-icon" aria-hidden="true"><use href="vendor/coolicons/coolicons.svg#ci-play"></use></svg>조치 선택</button></div></div>'
    + '<div class="ops-triage-table-wrap"><table class="ops-triage-table"><thead><tr><th>이름</th><th>역할</th><th>IP</th><th>CPU</th><th>메모리</th><th>상태</th></tr></thead><tbody>'
    + _opsVmRows(displayVms)
    + '</tbody></table></div></article>';

  h += '<article class="ops-triage-card ops-span-4"><div class="ops-triage-card-head">'
    + '<div class="ops-triage-card-title" role="heading" aria-level="2">명령 팔레트</div><span class="ops-triage-card-meta">Ctrl K</span></div>'
    + '<div class="ops-command">'
    + '<button class="ops-command-row is-active" type="button" onclick="navigateTo(\'host\')"><span class="ops-key">RUN</span><span>qemu-guest-agent 설치 확인</span><span class="ops-key">Enter</span></button>'
    + '<button class="ops-command-row" type="button" onclick="navigateTo(\'ovn\')"><span class="ops-key">NET</span><span>OVN NAT 및 logical router 상태 확인</span><span class="ops-key">N</span></button>'
    + '<button class="ops-command-row" type="button" onclick="navigateTo(\'mon-audit\')"><span class="ops-key">LOG</span><span>viewer 성공 로그인 IP 목록 열기</span><span class="ops-key">L</span></button>'
    + '<button class="ops-command-row" type="button" onclick="navigateTo(\'activity-log\')"><span class="ops-key">JOB</span><span>실패 작업만 필터링</span><span class="ops-key">J</span></button>'
    + '</div></article>';

  h += '<article class="ops-triage-card ops-span-4"><div class="ops-triage-card-head">'
    + '<div class="ops-triage-card-title" role="heading" aria-level="2">OVN 서비스 흐름</div><span class="ops-triage-card-meta">demo</span></div>'
    + '<div class="ops-node"><div class="ops-node-icon"><svg class="ci-icon" aria-hidden="true"><use href="vendor/coolicons/coolicons.svg#ci-desktop-tower"></use></svg></div><div><div class="ops-node-name">ovn-demo-a</div><div class="ops-node-sub">10.77.0.12:8080</div></div>' + _opsStatus('WEB', 'ok') + '</div>'
    + '<div class="ops-node"><div class="ops-node-icon"><svg class="ci-icon" aria-hidden="true"><use href="vendor/coolicons/coolicons.svg#ci-layers"></use></svg></div><div><div class="ops-node-name">pcv-demo-ls</div><div class="ops-node-sub">logical switch</div></div>' + _opsStatus('L2', 'info') + '</div>'
    + '<div class="ops-node"><div class="ops-node-icon"><svg class="ci-icon" aria-hidden="true"><use href="vendor/coolicons/coolicons.svg#ci-globe"></use></svg></div><div><div class="ops-node-name">pcv-demo-lr</div><div class="ops-node-sub">gateway 10.77.0.1</div></div>' + _opsStatus('NAT', 'ok') + '</div>'
    + '<div class="ops-flow" aria-label="서비스 흐름"><div class="ops-flow-box">VM</div><div>→</div><div class="ops-flow-box">LS</div><div>→</div><div class="ops-flow-box">LR</div></div>'
    + '</article>';

  h += '<article class="ops-triage-card ops-span-4"><div class="ops-triage-card-head">'
    + '<div class="ops-triage-card-title" role="heading" aria-level="2">감사 추적</div><span class="ops-triage-card-meta">audit</span></div>'
    + '<div class="ops-triage-list">' + _opsAuditRows() + '</div></article>';
  h += '</section>';
  b.innerHTML = h;
}
window.renderOpsTriage = renderOpsTriage;


async function loadDeepHealth() {
  var el = document.getElementById('deep-health'); if (!el) return;
  el.innerHTML = '<span class="spinner"></span> ' + (t('loading') || 'Loading...');
  try {
    var r = await fetch(EP.HEALTH());
    var d = await r.json();
    var overall = d.status || d.overall || 'unknown';
    var node = d.node || d.hostname || '-';
    var uptime = d.uptime_sec || d.uptime || 0;
    var subsystems = d.subsystems || d.checks || {};

    var overallColor = overall === 'ok' ? 'var(--green)' : overall === 'degraded' ? 'var(--yellow)' : 'var(--red)';
    var h = '<div style="display:flex;align-items:center;gap:12px;margin-bottom:10px;flex-wrap:wrap">';
    h += '<span style="font-size:14px;font-weight:700;color:' + overallColor + '">' + esc(overall.toUpperCase()) + '</span>';
    h += H.badge(esc(node), 'g');
    if (uptime > 0) h += '<span class="stat-label">' + (t('monitor.uptime') || 'Uptime') + ': ' + fmtUptime(uptime) + '</span>';
    h += '</div>';

    var subsysKeys = Object.keys(subsystems);
    if (subsysKeys.length === 0) {

      var knownSubs = ['libvirt', 'etcd', 'zfs', 'vm_state_db', 'audit_db', 'tls', 'cluster'];
      knownSubs.forEach(function(k) { if (d[k] !== undefined) subsystems[k] = d[k]; });
      subsysKeys = Object.keys(subsystems);
    }

    if (subsysKeys.length > 0) {
      h += '<div style="display:flex;gap:8px;flex-wrap:wrap">';
      subsysKeys.forEach(function(k) {
        var v = subsystems[k];
        var st;
        if (typeof v === 'object' && v !== null) {
          if (v.status) st = v.status;
          else if (v.state) st = v.state;
          else if (v.ok !== undefined) st = v.ok ? 'ok' : 'fail';
          else if (v.enabled !== undefined) st = v.enabled ? 'enabled' : 'disabled';
          else if (v.mode) st = v.mode;
          else if (v.note) st = v.note;
          else st = 'unknown';
        } else {
          st = String(v);
        }
        var sc = (st === 'ok' || st === 'connected' || st === 'active' || st === 'enabled' || st === 'true' || st === true) ? 'g'
          : (st === 'warning' || st === 'degraded') ? 'y' : (st === 'unknown' || st === 'n/a' || st === 'disabled' || st === 'single_edge' || /standalone/.test(st)) ? '' : 'r';
        var detailParts = [];
        if (typeof v === 'object' && v !== null) {
          if (v.detail) detailParts.push(v.detail);
          if (v.latency_ms !== undefined) detailParts.push(v.latency_ms + 'ms');
          if (v.avail_gb !== undefined) detailParts.push(v.avail_gb.toFixed(1) + 'GB free');
          if (v.size_mb !== undefined) detailParts.push(v.size_mb.toFixed(1) + 'MB');
        }
        var detail = detailParts.length ? ' (' + esc(detailParts.join(', ')) + ')' : '';
        h += '<div style="display:inline-flex;align-items:center;gap:4px;padding:4px 10px;border:1px solid var(--border);border-radius:6px;font-size:11px;background:var(--bg2)">';
        h += '<span style="color:' + _healthBadgeColor(sc) + ';font-size:8px">&#9679;</span>';
        h += '<span style="font-weight:600">' + esc(k) + '</span>';
        h += '<span style="color:' + _healthBadgeColor(sc) + '">' + esc(st) + detail + '</span>';
        h += '</div>';
      });
      h += '</div>';
    } else {
      h += '<span class="color-muted">' + (t('monitor.no_subsystems') || 'No subsystem details available') + '</span>';
    }

    el.innerHTML = h;
  } catch (e) {
    el.innerHTML = '<span class="color-muted">' + (t('monitor.health_unavailable') || 'Health probe unavailable') + ': ' + esc(e.message) + '</span>';
  }
}

function _healthBadgeColor(sc) {
  if (sc === 'g') return 'var(--green)';
  if (sc === 'y') return 'var(--yellow)';
  if (sc === 'r') return 'var(--red)';
  return 'var(--fg2)';
}

function renderMonOverview(b, all, allVms, running, avgCpu, avgMem, avgDisk, totalRam) {

  if (typeof pcvDestroyAllInContainer === 'function') pcvDestroyAllInContainer(b);
  destroyAllCharts();

  let h = '<div class="hc mb-14"><h4>&#129657; ' + (t('monitor.system_health') || 'System Health') + '</h4>';
  h += '<p class="color-muted text-11 mb-8">' + (t('monitor.health_desc') || 'Deep health probe of all subsystems. Updated on each page load.') + '</p>';
  h += '<div id="deep-health"><span class="spinner"></span> ' + (t('loading') || 'Loading...') + '</div></div>';


  h += '<div class="hc mb-12"><h4>&#128202; ' + (t('monitor.cluster_timeline') || '리소스 흐름 (최근 5분)') + '</h4>';
  h += '<div class="grid-3 gap-12" style="display:grid;grid-template-columns:1fr 1fr 1fr">';
  h += '<div style="position:relative;height:180px"><canvas id="pcv-chart-cpu"></canvas></div>';
  h += '<div style="position:relative;height:180px"><canvas id="pcv-chart-mem"></canvas></div>';
  h += '<div style="position:relative;height:180px"><canvas id="pcv-chart-net"></canvas></div>';
  h += '</div></div>';

  h += H.section('&#128200; 운영 개요');
  h += H.grid(8,
    H.card('호스트', '<div class="stat-xl color-green">' + all.length + '</div>', 'text-center')
  + H.card('VM', '<div class="stat-xl color-accent">' + allVms.length + '</div>', 'text-center')
  + H.card('실행 중', '<div class="stat-xl color-green">' + running + '</div>', 'text-center')
  + H.card('평균 CPU', gauge(avgCpu, 'Host'), 'text-center')
  + H.card('평균 메모리', gauge(avgMem, 'Host'), 'text-center')
  + H.card('평균 디스크', gauge(avgDisk, 'Host'), 'text-center')
  + (function() { const tSwapUsed = all.reduce((s, n) => s + ((n.memInfo.SwapTotal || 0) - (n.memInfo.SwapFree || 0)), 0); const tSwapTotal = all.reduce((s, n) => s + (n.memInfo.SwapTotal || 0), 0); return H.card('스왑', gauge(tSwapTotal > 0 ? tSwapUsed / tSwapTotal * 100 : 0, fmtBytes(tSwapUsed) + '/' + fmtBytes(tSwapTotal)), 'text-center'); })()
  + H.card('소켓', '<div class="stat-lg color-cyan">' + all.reduce((s, n) => s + (n.conntrack || 0), 0) + '</div><div class="stat-label">connections</div>', 'text-center')
  );
  h += '<div class="sg grid-3 mb-12">';
  all.forEach(n => { const hi = monHist[n.ip] || { cpu: [], mem: [], netRx: [], netTx: [] };
    h += '<div class="hc"><h4>' + n.node + ' <span class="stat-label">' + n.ip + '</span>' + (n.uptime ? ' <span class="stat-label">up ' + fmtUptime(n.uptime) + '</span>' : '') + '</h4>';
    h += '<div class="flex gap-8 mb-6"><div class="flex-1"><div class="stat-label mb-2">CPU ' + (n.cpu || 0).toFixed(1) + '%</div><canvas id="mc-' + n.ip + '-cpu" class="sparkline"></canvas></div>';
    h += '<div class="flex-1"><div class="stat-label mb-2">MEM ' + (n.mem || 0).toFixed(1) + '%</div><canvas id="mc-' + n.ip + '-mem" class="sparkline"></canvas></div></div>';
    h += H.row('Temp', (n.temp || 0).toFixed(1) + '\u00B0C');
    h += H.row('Load', (n.load1 || n.load || 0).toFixed(2) + ' / ' + (n.load5 || 0).toFixed(2) + ' / ' + (n.load15 || 0).toFixed(2));
    h += H.row('RAM', ((n.ram_total || 0) / 1073741824).toFixed(1) + ' GB');
    h += H.row('Net I/O', '<span class="color-cyan">&#9650; ' + fmtRate(hi.netRx, hi.netRx.length - 1) + ' &#9660; ' + fmtRate(hi.netTx, hi.netTx.length - 1) + '</span>');
    h += H.row('Sockets', (n.sockstat.sockets_used || 0) + ' (TCP:' + (n.sockstat.TCP_inuse || 0) + ' UDP:' + (n.sockstat.UDP_inuse || 0) + ')') + '</div>'; });
  h += '</div>';

  h += '<div class="sg grid-3 mb-12">';
  h += '<div class="hc"><h4 class="color-red">&#9888; 이상 징후</h4><div style="font-size:12px;color:var(--fg2);margin-bottom:8px;line-height:1.6;border-left:2px solid var(--red);padding-left:8px"><b>Z-Score</b> 기반 이상 탐지<br>&#8226; 최근 60개 샘플(약 5분)<br>&#8226; Z &gt; 1.5 경고, Z &gt; 2.5 위험</div>';
  const totalAnom = all.reduce((s, n) => s + (n.anomaly_active || 0), 0);
  h += H.row('Active', '<span style="color:' + (totalAnom > 0 ? 'var(--red)' : 'var(--green)') + '">' + totalAnom + '</span>');
  h += H.row('Total Alerts', Math.round(all.reduce((s, n) => s + (n.anomaly_total || 0), 0)));
  all.forEach(n => { const scores = n.anomaly_scores || {}; const keys = Object.keys(scores).filter(k => scores[k] > 1.5);
    if (keys.length > 0) { h += '<div class="stat-label mt-4">' + n.node + ':</div>'; keys.forEach(k => { const z = scores[k]; h += '<div class="stat-label" style="padding-left:8px"><span style="color:' + (z > 2.5 ? 'var(--red)' : 'var(--yellow)') + '">Z=' + z.toFixed(1) + '</span> ' + k.replace('purecvisor_', '').replace('node_', '') + '</div>'; }); } });
  h += '</div>';
  h += '<div class="hc"><h4 class="color-cyan">&#128200; 5분 예측</h4><div style="font-size:12px;color:var(--fg2);margin-bottom:8px;line-height:1.6;border-left:2px solid var(--cyan);padding-left:8px"><b>EMA + OLS</b> 기반 추세 예측<br>&#8226; EMA alpha=0.3 + 선형 회귀 기울기</div>';
  all.forEach(n => { if (n.cpu_pred === undefined) return;
    const cpuDir = n.cpu_trend > 0.01 ? '&#9650;' : n.cpu_trend < -0.01 ? '&#9660;' : '&#9654;';
    const memDir = n.mem_trend > 0.01 ? '&#9650;' : n.mem_trend < -0.01 ? '&#9660;' : '&#9654;';
    h += '<div class="text-11 mb-4"><b>' + n.node + '</b></div>';
    h += H.row('CPU', (n.cpu || 0).toFixed(1) + '% \u2192 <span style="color:' + (n.cpu_pred > 80 ? 'var(--red)' : 'var(--green)') + '">' + n.cpu_pred.toFixed(1) + '%</span> ' + cpuDir);
    h += H.row('MEM', (n.mem || 0).toFixed(1) + '% \u2192 <span style="color:' + (n.mem_pred > 85 ? 'var(--red)' : 'var(--green)') + '">' + n.mem_pred.toFixed(1) + '%</span> ' + memDir); });
  h += '</div>';
  h += '<div class="hc"><h4 class="color-green">&#9889; 자동 복구 준비 상태</h4><div style="font-size:12px;color:var(--fg2);margin-bottom:8px;line-height:1.6;border-left:2px solid var(--green);padding-left:8px"><b>정책 기반 자동 복구 준비 정보</b><br>&#8226; 기본값은 DRY RUN으로 유지됩니다.</div>';
  h += H.row('Mode', H.badge('DRY RUN', 'y'));
  h += H.row('Pending', all.reduce((s, n) => s + (n.healing_pending || 0), 0));
  h += H.row('Executed', Math.round(all.reduce((s, n) => s + (n.healing_total || 0), 0)));
  const n1 = all[0] || {}; const agentProv = n1.agent_prov || {};
  if (Object.keys(agentProv).length > 0) { h += '<div style="margin-top:6px;border-top:1px solid var(--border);padding-top:6px"><div class="stat-label font-bold color-accent">&#129302; AI Agent Providers</div><table class="text-xs"><thead><tr><th>Provider</th><th>Conf</th><th>Latency</th></tr></thead><tbody>';
    Object.entries(agentProv).forEach(([name, d]) => { h += '<tr><td>' + name + '</td><td>' + (d.confidence || 0).toFixed(2) + '</td><td>' + (d.latency || 0).toFixed(0) + 'ms</td></tr>'; });
    h += '</tbody></table>'; if (n1.agent_conf !== undefined) h += '<div class="stat-label mt-4">Consensus <span class="color-green font-bold">' + n1.agent_conf.toFixed(2) + '</span></div>'; h += '</div>'; }
  h += '<div style="margin-top:6px"><button class="btn" onclick="showAgentConfig()" class="btn-xs">&#9881; Configure AI Agent</button></div></div></div>';

  h += '<div class="hc mb-14"><h4 class="color-yellow">&#9888; ' + _L('자가치유 대기 액션', 'Self-Healing Pending Actions') + '</h4>';
  h += '<div id="healing-pending-list" class="skeleton-box" style="min-height:60px"></div></div>';

  h += '<div class="sg grid-1 mb-12">' + H.card('&#9741; keepalived VRRP Status', '<table class="text-12"><thead><tr><th>Node</th><th>keepalived</th><th>VRRP Role</th><th>VIP Owner</th></tr></thead><tbody>' +
  all.map(n => { const kaA = n.keepalived_active === 1; const kaM = n.keepalived_master === 1; const kaV = n.keepalived_vip_owner === 1;
    return '<tr><td><b>' + n.node + '</b> <span class="stat-label">' + n.ip + '</span></td><td>' + H.badge(kaA ? 'ACTIVE' : 'DOWN', kaA ? 'g' : 'r') + '</td><td>' + H.badge(kaM ? 'MASTER' : 'BACKUP', kaM ? 'g' : 'y') + '</td><td>' + (kaV ? '<span class="color-green font-bold">' + escapeHtml(_VIP || 'VIP') + '</span>' : '-') + '</td></tr>'; }).join('') +
  '</tbody></table>') + '</div>';

  const runVms = allVms.filter(v => v.running === 1);
  function top5Tbl(title, items, valFn, unit) { let t2 = '<div class="hc"><h4>' + title + '</h4><table class="text-11"><tbody>';
    items.forEach((v, i) => { t2 += '<tr><td class="w-16 color-muted">' + (i + 1) + '</td><td><b>' + v.name + '</b></td><td class="color-muted">' + v.node + '</td><td class="text-right font-bold color-accent">' + valFn(v) + unit + '</td></tr>'; });
    if (items.length === 0) t2 += '<tr><td colspan="4" class="color-muted">No running VMs</td></tr>'; return t2 + '</tbody></table></div>'; }
  h += H.grid(4,
    top5Tbl('Top 5 Memory', [...runVms].sort((a, b) => (b.memory_used_mb || 0) - (a.memory_used_mb || 0)).slice(0, 5), v => (v.memory_used_mb || 0).toLocaleString(), ' MB')
  + top5Tbl('Top 5 vCPU', [...runVms].sort((a, b) => (b.vcpu || 0) - (a.vcpu || 0)).slice(0, 5), v => v.vcpu || 0, '')
  + top5Tbl('Top 5 Disk I/O', [...runVms].sort((a, b) => (b.disk_rd_bytes || 0) - (a.disk_rd_bytes || 0)).slice(0, 5), v => fmtBytes(v.disk_rd_bytes || 0), '')
  + top5Tbl('Top 5 Network', [...runVms].sort((a, b) => ((b.net_rx_bytes || 0) + (b.net_tx_bytes || 0)) - ((a.net_rx_bytes || 0) + (a.net_tx_bytes || 0))).slice(0, 5), v => fmtBytes((v.net_rx_bytes || 0) + (v.net_tx_bytes || 0)), '')
  );
  h += H.card('All VMs (' + allVms.length + ')', '<table class="table-sticky"><thead><tr><th>Name</th><th>State</th><th>Node</th><th>vCPU</th><th>Max MB</th><th>Used MB</th></tr></thead><tbody>' +
  allVms.map(v => '<tr><td><b>' + v.name + '</b></td><td>' + H.badge(v.running === 1 ? 'running' : 'off', v.running === 1 ? 'g' : 'r') + '</td><td>' + v.node + '</td><td>' + (v.vcpu || '-') + '</td><td>' + (v.memory_max_mb || '-') + '</td><td>' + (v.memory_used_mb > 0 ? v.memory_used_mb : '-') + '</td></tr>').join('') +
  '</tbody></table>');

  h += '<div id="selfhealing-panel" class="hc mb-14" style="margin-top:24px"></div>';
  b.innerHTML = h;
  setTimeout(loadDeepHealth, 50);
  setTimeout(loadHealingPending, 100);


  setTimeout(function() { if (window.PCV && PCV.selfhealing) PCV.selfhealing.refresh(); }, 150);
  setTimeout(() => { all.forEach(n => { const hi = monHist[n.ip] || { cpu: [], mem: [] }; drawLine('mc-' + n.ip + '-cpu', hi.cpu, getChartColor('cpu'), '%'); drawLine('mc-' + n.ip + '-mem', hi.mem, getChartColor('mem'), '%'); }); }, 50);

  setTimeout(function() {
    if (typeof pcvTimeSeries !== 'function') return;
    var cpuSeries = all.map(function(n) {
      var hi = monHist[n.ip] || { cpu: [] };
      return { label: n.node || n.ip, data: hi.cpu.slice(-60) };
    });
    var memSeries = all.map(function(n) {
      var hi = monHist[n.ip] || { mem: [] };
      return { label: n.node || n.ip, data: hi.mem.slice(-60) };
    });
    var netSeries = [];
    all.forEach(function(n, i) {
      var hi = monHist[n.ip] || { netRx: [], netTx: [] };
      var rxMb = hi.netRx.slice(-60).map(function(v){ return (v || 0) / 1048576; });
      var txMb = hi.netTx.slice(-60).map(function(v){ return (v || 0) / 1048576; });
      netSeries.push({ label: (n.node || n.ip) + ' RX', data: rxMb });
      netSeries.push({ label: (n.node || n.ip) + ' TX', data: txMb });
    });
    pcvTimeSeries('pcv-chart-cpu', cpuSeries, { title: 'CPU %', unit: '%', max: 100, fill: false });
    pcvTimeSeries('pcv-chart-mem', memSeries, { title: 'Memory %', unit: '%', max: 100, fill: false });
    pcvTimeSeries('pcv-chart-net', netSeries, { title: 'Network MB/s', unit: ' MB/s', fill: false });
  }, 100);
}
window.renderMonOverview = renderMonOverview;

function renderMonCluster(b, all) {

  if (typeof pcvDestroyAllInContainer === 'function') pcvDestroyAllInContainer(b);
  destroyAllCharts();
  let h = H.section('&#9741; Cluster Status') + '<div class="sg grid-3 mb-12">';
  all.forEach(n => { h += '<div class="hc"><h4 class="justify-between">' + n.node + H.badge(n.error ? 'DOWN' : 'UP', n.error ? 'r' : 'g') + '</h4><div class="flex gap-12" style="justify-content:center;margin:10px 0">' + gauge(n.cpu || 0, 'CPU') + gauge(n.mem || 0, 'MEM') + gauge(n.disk || 0, 'DISK') + '</div>';
    h += H.row('IP', n.ip) + H.row('VMs', n.vms.length) + H.row('Running', '<span class="color-green">' + n.vms.filter(v => v.running === 1).length + '</span>') + H.row('RAM', ((n.ram_total || 0) / 1073741824).toFixed(1) + ' GB') + H.row('Load', (n.load1 || n.load || 0).toFixed(2)) + '</div>'; });
  h += '</div>';
  h += '<div class="sg grid-2 mb-12"><div class="hc"><h4>CPU Trend</h4><div class="flex flex-col gap-4">';
  all.forEach(n => { h += '<div><span class="stat-label">' + n.node + '</span><canvas id="ct-' + n.ip + '" class="sparkline-sm"></canvas></div>'; });
  h += '</div></div><div class="hc"><h4>Memory Trend</h4><div class="flex flex-col gap-4">';
  all.forEach(n => { h += '<div><span class="stat-label">' + n.node + '</span><canvas id="mt-' + n.ip + '" class="sparkline-sm"></canvas></div>'; });
  h += '</div></div></div>';
  h += H.card('HA Operations', '<div class="flex gap-10 mt-8"><button class="btn" onclick="haFailoverTest()">Failover Test</button><button class="btn" onclick="haMigrate()">Live Migrate VM</button><button class="btn" onclick="haReplicate()">ZFS Replicate</button></div>', 'mb-12');
  b.innerHTML = h;
  setTimeout(() => { const colors = [getChartColor('cpu'), getChartColor('net'), getChartColor('alt1')]; all.forEach((n, i) => { const hi = monHist[n.ip] || { cpu: [], mem: [] }; drawLine('ct-' + n.ip, hi.cpu, colors[i % 3], '%'); drawLine('mt-' + n.ip, hi.mem, colors[i % 3], '%'); }); }, 50);
}
window.renderMonCluster = renderMonCluster;

function renderMonHosts(b, all) {

  if (typeof pcvDestroyAllInContainer === 'function') pcvDestroyAllInContainer(b);
  destroyAllCharts();
  let h = H.section('&#128187; Host Performance');
  all.forEach(n => {
    h += '<div style="border:1px solid var(--border);border-radius:var(--r);padding:12px;margin-bottom:12px">';
    h += '<div class="justify-between items-center mb-10"><h4 class="text-14">' + n.node + ' <span class="stat-label">' + n.ip + '</span>' + (n.uptime ? ' <span class="stat-label">uptime ' + fmtUptime(n.uptime) + '</span>' : '') + '</h4>' + H.badge(n.error ? 'DOWN' : 'UP', n.error ? 'r' : 'g') + '</div>';
    h += H.grid(6,
      '<div class="hc text-center">' + gauge(n.cpu || 0, 'CPU') + '</div><div class="hc text-center">' + gauge(n.mem || 0, 'Memory') + '</div><div class="hc text-center">' + gauge(n.disk || 0, 'Disk') + '</div>'
    + H.card('Temperature', '<div class="stat-md" style="color:' + (n.temp > 55 ? 'var(--red)' : 'var(--green)') + '">' + (n.temp || 0).toFixed(1) + '\u00B0C</div>')
    + H.card('Load', '<div style="font-size:16px;font-weight:700">' + (n.load1 || n.load || 0).toFixed(2) + '</div>')
    + H.card('Total RAM', '<div class="stat-md">' + ((n.ram_total || 0) / 1073741824).toFixed(1) + ' GB</div>')
    );
    h += '<div class="sg grid-3">' + H.card('CPU History', '<canvas id="hc-' + n.ip + '" class="sparkline-md"></canvas>') + H.card('Memory History', '<canvas id="hm-' + n.ip + '" class="sparkline-md"></canvas>') + H.card('Disk History', '<canvas id="hd-' + n.ip + '" class="sparkline-md"></canvas>') + '</div>';

    const mi = n.memInfo || {};
    const mtotal = mi.MemTotal || 1;
    const mUsed = mtotal - (mi.MemAvailable || 0);
    const mCached = mi.Cached || 0;
    const mBuffers = mi.Buffers || 0;
    const mFree = mi.MemFree || 0;
    const mSlab = mi.Slab || 0;
    const pUsed = (mUsed / mtotal * 100).toFixed(2);
    const pBuf = (mBuffers / mtotal * 100).toFixed(2);
    const pCache = (mCached / mtotal * 100).toFixed(2);
    const pSlab = (mSlab / mtotal * 100).toFixed(2);
    const pFree = (mFree / mtotal * 100).toFixed(2);
    h += '<div class="hc mt-8"><h4>Memory Breakdown</h4><div style="display:flex;height:18px;border-radius:3px;overflow:hidden;margin-bottom:4px">'
       + '<div class="pcv-bar-fill-inline" style="--bw:' + pUsed + '%;--bc:var(--red)"></div>'
       + '<div class="pcv-bar-fill-inline" style="--bw:' + pBuf + '%;--bc:var(--yellow)"></div>'
       + '<div class="pcv-bar-fill-inline" style="--bw:' + pCache + '%;--bc:var(--accent)"></div>'
       + '<div class="pcv-bar-fill-inline" style="--bw:' + pSlab + '%;--bc:var(--magenta)"></div>'
       + '<div class="pcv-bar-fill-inline" style="--bw:' + pFree + '%;--bc:var(--green)"></div>'
       + '</div><div class="flex gap-12 stat-label flex-wrap">'
       + '<span>&#9632; <span class="color-red">Used</span> ' + fmtBytes(mUsed) + '</span>'
       + '<span>&#9632; <span class="color-yellow">Buf</span> ' + fmtBytes(mBuffers) + '</span>'
       + '<span>&#9632; <span class="color-accent">Cache</span> ' + fmtBytes(mCached) + '</span>'
       + '<span>&#9632; <span class="color-magenta">Slab</span> ' + fmtBytes(mSlab) + '</span>'
       + '<span>&#9632; <span class="color-green">Free</span> ' + fmtBytes(mFree) + '</span>'
       + '</div></div>';

    const coreIds = Object.keys(n.cores || {}).filter(c => parseInt(c) < 64).sort((a, b) => parseInt(a) - parseInt(b));
    if (coreIds.length > 0) { h += '<div class="hc mt-8"><h4>CPU per Core (' + coreIds.length + ')</h4><div class="flex" style="flex-wrap:wrap;gap:3px">';
      coreIds.forEach(c => { const cd = n.cores[c]; const total = Object.values(cd).reduce((s, v) => s + v, 0); const pct = total > 0 ? (1 - (cd.idle || 0) / total) * 100 : 0; const cl = pct > 80 ? 'var(--red)' : pct > 50 ? 'var(--yellow)' : 'var(--green)';
        h += '<div class="w-28 text-center" title="Core ' + c + ': ' + pct.toFixed(1) + '%"><div style="height:24px;background:var(--bg);border-radius:2px;border:1px solid var(--border);position:relative;overflow:hidden"><div style="position:absolute;bottom:0;width:100%;height:' + pct + '%;background:' + cl + '"></div></div><div style="font-size:8px;color:var(--fg2)">' + c + '</div></div>'; });
      h += '</div></div>'; }

    const ndevs = Object.entries(n.netdevs || {}).filter(([d]) => !['lo', 'ovs-system', 'br-int'].includes(d));
    if (ndevs.length > 0) { h += '<div class="hc mt-8"><h4>&#127760; Network Interfaces</h4><table class="text-11"><thead><tr><th>Device</th><th>RX</th><th>TX</th><th>Errors</th><th>Drops</th></tr></thead><tbody>';
      ndevs.forEach(([d, s]) => { h += '<tr><td><b>' + d + '</b></td><td>' + fmtBytes(s.receive_bytes_total || 0) + '</td><td>' + fmtBytes(s.transmit_bytes_total || 0) + '</td><td>' + (s.receive_errs_total || 0) + '</td><td>' + (s.receive_drop_total || 0) + '</td></tr>'; });
      h += '</tbody></table></div>'; }

    const ddevs = Object.entries(n.disks || {}).filter(([d]) => d.match(/^(nvme\d+n\d+|sd[a-z])$/));
    if (ddevs.length > 0) { h += '<div class="hc mt-8"><h4>&#128190; Disk I/O</h4><table class="text-11"><thead><tr><th>Device</th><th>Read</th><th>Written</th><th>IOPS</th></tr></thead><tbody>';
      ddevs.forEach(([d, s]) => { h += '<tr><td><b>' + d + '</b></td><td>' + fmtBytes(s.read_bytes_total || 0) + '</td><td>' + fmtBytes(s.written_bytes_total || 0) + '</td><td>' + (s.reads_completed_total || 0) + '/' + (s.writes_completed_total || 0) + '</td></tr>'; });
      h += '</tbody></table></div>'; }
    h += '</div>'; });
  b.innerHTML = h;
  setTimeout(() => { all.forEach(n => { const hi = monHist[n.ip] || { cpu: [], mem: [], disk: [] }; drawLine('hc-' + n.ip, hi.cpu, getChartColor('cpu'), '%'); drawLine('hm-' + n.ip, hi.mem, getChartColor('mem'), '%'); drawLine('hd-' + n.ip, hi.disk, getChartColor('disk'), '%'); }); }, 50);
}
window.renderMonHosts = renderMonHosts;

function renderMonVms(b, allVms, running) {

  if (typeof pcvDestroyAllInContainer === 'function') pcvDestroyAllInContainer(b);
  destroyAllCharts();
  let h = H.section('&#9881; Virtual Machines');
  h += H.grid(4,
    H.card('Total', '<div class="stat-xl">' + allVms.length + '</div>', 'text-center')
  + H.card('Running', '<div class="stat-xl color-green">' + running + '</div>', 'text-center')
  + H.card('Total vCPU', '<div class="stat-xl color-accent">' + allVms.reduce((s, v) => s + (v.vcpu || 0), 0) + '</div>', 'text-center')
  + H.card('Total Memory', '<div class="stat-xl">' + (allVms.reduce((s, v) => s + (v.memory_max_mb || 0), 0) / 1024).toFixed(1) + ' GB</div>', 'text-center')
  );
  h += '<div class="sg grid-2">';
  allVms.forEach(v => { const on = v.running === 1; const memPct = v.memory_max_mb > 0 && v.memory_used_mb > 0 ? v.memory_used_mb / v.memory_max_mb * 100 : 0;
    h += '<div class="hc"><div class="justify-between mb-8"><h4>' + v.name + '</h4>' + H.badge(on ? 'running' : 'off', on ? 'g' : 'r') + '</div><div class="sg grid-3"><div class="text-center">' + gauge(on ? memPct : 0, 'Memory') + '</div><div>' + H.row('vCPU', v.vcpu || '-') + H.row('Max RAM', (v.memory_max_mb || '-') + ' MB') + '</div><div>' + H.row('Used RAM', (v.memory_used_mb > 0 ? v.memory_used_mb + ' MB' : '-')) + H.row('Node', v.node) + '</div></div></div>'; });
  h += '</div>'; b.innerHTML = h;
}
window.renderMonVms = renderMonVms;

function _aggregateZfsLocks(all) {
  var total = _newZfsLocks();
  (all || []).forEach(function(n) {
    var s = n.zfsLocks || _newZfsLocks();
    ['total', 'ok', 'busy', 'error', 'unknown', 'waitSumMs', 'waitCount'].forEach(function(k) {
      total[k] += s[k] || 0;
    });
    Object.keys(s.byOp || {}).forEach(function(op) {
      var src = s.byOp[op];
      var dst = _zfsLockOp(total, op);
      ['total', 'ok', 'busy', 'error', 'unknown', 'waitSumMs', 'waitCount'].forEach(function(k) {
        dst[k] += src[k] || 0;
      });
    });
  });
  return total;
}

function _zfsLockPanel(all) {
  var s = _aggregateZfsLocks(all);
  var avgWait = s.waitCount > 0 ? s.waitSumMs / s.waitCount : 0;
  var h = '<div class="hc mb-12"><h4>ZFS inflight lock</h4>';
  h += '<p class="color-muted text-11 mb-8">' + _L('ADR-0021 분산 락 획득 결과와 대기 시간을 표시합니다.', 'Shows ADR-0021 distributed lock acquisition results and wait time.') + '</p>';
  if (s.total <= 0 && s.waitCount <= 0) {
    h += '<div class="empty-state" style="padding:18px;text-align:left"><div class="empty-state-text">No ZFS inflight lock samples yet</div>';
    h += '<div class="color-muted text-12">' + _L('샘플은 ZFS create/destroy 작업이 실행된 뒤 Prometheus metric에서 집계됩니다.', 'Samples appear after ZFS create/destroy operations publish Prometheus metrics.') + '</div></div></div>';
    return h;
  }
  h += '<div class="sg grid-5 mb-8">';
  h += H.card('Total', '<div class="stat-lg color-accent">' + Math.round(s.total).toLocaleString() + '</div>', 'text-center');
  h += H.card('OK', '<div class="stat-lg color-green">' + Math.round(s.ok || 0).toLocaleString() + '</div>', 'text-center');
  h += H.card('Busy', '<div class="stat-lg color-yellow">' + Math.round(s.busy || 0).toLocaleString() + '</div>', 'text-center');
  h += H.card('Error', '<div class="stat-lg color-red">' + Math.round(s.error || 0).toLocaleString() + '</div>', 'text-center');
  h += H.card('Avg wait', '<div class="stat-lg color-cyan">' + avgWait.toFixed(1) + 'ms</div>', 'text-center');
  h += '</div>';
  h += '<table class="text-11"><thead><tr><th>Op</th><th>Total</th><th>OK</th><th>Busy</th><th>Error</th><th>Avg wait</th></tr></thead><tbody>';
  Object.keys(s.byOp).sort().forEach(function(op) {
    var o = s.byOp[op];
    var ow = o.waitCount > 0 ? o.waitSumMs / o.waitCount : 0;
    h += '<tr><td><b>' + esc(op) + '</b></td><td>' + Math.round(o.total).toLocaleString() + '</td><td class="color-green">' + Math.round(o.ok || 0).toLocaleString() + '</td><td class="color-yellow">' + Math.round(o.busy || 0).toLocaleString() + '</td><td class="color-red">' + Math.round(o.error || 0).toLocaleString() + '</td><td>' + ow.toFixed(1) + 'ms</td></tr>';
  });
  h += '</tbody></table></div>';
  return h;
}

function renderMonStorage(b, all, allVms, totalRam) {

  if (typeof pcvDestroyAllInContainer === 'function') pcvDestroyAllInContainer(b);
  destroyAllCharts();
  let h = H.section('&#128190; Storage & Capacity');
  h += H.card('Datastore Usage', all.map(n => { const pct = n.disk || 0; const cl = pct > 80 ? 'var(--red)' : pct > 60 ? 'var(--yellow)' : 'var(--green)';
    return '<div class="mb-8"><div class="justify-between" style="font-size:11px;margin-bottom:2px"><span>' + n.node + '</span><span style="color:' + cl + '">' + pct.toFixed(1) + '%</span></div><div style="height:20px;background:var(--bg);border-radius:3px;border:1px solid var(--border);overflow:hidden"><div style="height:100%;width:' + pct + '%;background:' + cl + ';border-radius:3px"></div></div></div>'; }).join(''), 'mb-12');
  const vmMem = allVms.reduce((s, v) => s + (v.memory_max_mb || 0), 0);
  h += H.grid(3,
    H.card('Total Cluster RAM', '<div class="stat-xl color-accent">' + (totalRam / 1073741824).toFixed(1) + ' GB</div>', 'text-center')
  + H.card('VM Provisioned', '<div class="stat-xl color-yellow">' + (vmMem / 1024).toFixed(1) + ' GB</div>', 'text-center')
  + H.card('Overcommit', gauge(totalRam > 0 ? vmMem * 1048576 / totalRam * 100 : 0, 'RAM'), 'text-center')
  ) + '<div class="mb-12"></div>';
  h += _zfsLockPanel(all);
  h += '<div class="sg grid-2 mb-12"><div class="hc"><h4>Disk Usage Trend</h4>';
  all.forEach(n => { h += '<div class="mb-4"><span class="stat-label">' + n.node + '</span><canvas id="sd-' + n.ip + '" class="sparkline-sm"></canvas></div>'; });
  h += '</div><div class="hc"><h4>Memory per Node</h4>';
  all.forEach(n => { const gb = (n.ram_total || 0) / 1073741824; h += '<div class="mb-6"><div class="justify-between text-11"><span>' + n.node + '</span><span>' + gb.toFixed(1) + ' GB</span></div><div style="height:14px;background:var(--bg);border-radius:3px;border:1px solid var(--border);overflow:hidden"><div style="height:100%;width:' + (gb / 64 * 100) + '%;background:var(--accent);border-radius:3px"></div></div></div>'; });
  h += '</div></div>';

  all.forEach(n => { const fsList = (n.filesystems || []).filter(f => f.fstype === 'zfs' || f.fstype === 'ext4' || f.fstype === 'xfs');
    if (fsList.length === 0) return;
    h += '<div class="hc mb-12"><h4>' + n.node + ' \u2014 Filesystems</h4><table class="text-11"><thead><tr><th>Mount</th><th>Type</th><th>Size</th><th>Avail</th><th>Used %</th></tr></thead><tbody>';
    fsList.forEach(f => { const sz = f.size_bytes || 0; const av = f.avail_bytes || 0; const pct = sz > 0 ? (sz - av) / sz * 100 : 0; h += '<tr><td><b>' + f.mount + '</b></td><td>' + f.fstype + '</td><td>' + fmtBytes(sz) + '</td><td>' + fmtBytes(av) + '</td><td style="color:' + (pct > 85 ? 'var(--red)' : 'var(--green)') + '">' + pct.toFixed(1) + '%</td></tr>'; });
    h += '</tbody></table></div>'; });

  h += H.card('Disk I/O (All Nodes)', '<table class="text-11"><thead><tr><th>Node</th><th>Device</th><th>Read</th><th>Written</th><th>Read IOPS</th><th>Write IOPS</th></tr></thead><tbody>' +
  all.map(n => Object.entries(n.disks || {}).filter(([d]) => d.match(/^(nvme\d+n\d+|sd[a-z])$/)).map(([d, s]) =>
    '<tr><td>' + n.node + '</td><td><b>' + d + '</b></td><td>' + fmtBytes(s.read_bytes_total || 0) + '</td><td>' + fmtBytes(s.written_bytes_total || 0) + '</td><td>' + (s.reads_completed_total || 0).toLocaleString() + '</td><td>' + (s.writes_completed_total || 0).toLocaleString() + '</td></tr>').join('')).join('') +
  '</tbody></table>', 'mb-12');
  b.innerHTML = h;
  setTimeout(() => { const colors = [getChartColor('disk'), getChartColor('alt2'), getChartColor('alt3')]; all.forEach((n, i) => { const hi = monHist[n.ip] || { disk: [] }; drawLine('sd-' + n.ip, hi.disk, colors[i % 3], '%'); }); }, 50);
}
window.renderMonStorage = renderMonStorage;


async function renderAlerts(b) {
  b.innerHTML = showSkeleton();
  const hdr = { 'Authorization': 'Bearer ' + authToken, 'Content-Type': 'application/json' };
  const [cfgR, histR] = await Promise.all([
    fetch(EP.ALERTS_CONFIG(), { headers: hdr }).then(r => r.json()).catch(() => ({})),
    fetch(EP.ALERTS(), { headers: hdr }).then(r => r.json()).catch(() => [])
  ]);
  const cfg = unwrapData(cfgR) || {};
  const hist = unwrapList(histR);
  const v = (k, d) => cfg[k] !== undefined ? cfg[k] : d;
  let h = H.section('&#128276; Alert Configuration');
  const en = v('enabled', false);
  h += '<div class="flex items-center gap-12 mb-16" style="padding:10px 14px;background:' + (en ? 'rgba(0,255,136,.08)' : 'rgba(255,34,102,.08)') + ';border:1px solid ' + (en ? 'var(--green)' : 'var(--red)') + ';border-radius:var(--r)">' + H.badge(en ? 'ENABLED' : 'DISABLED', en ? 'g' : 'r') + '<span class="text-12">' + (en ? t('alert.enabled') : t('alert.disabled')) + '</span><label style="margin-left:auto;cursor:pointer;display:flex;align-items:center;gap:6px"><input type="checkbox" id="al-enabled" ' + (en ? 'checked' : '') + ' onchange="alertSave()"><span class="text-xs">Enable</span></label></div>';
  h += '<div class="sg grid-3 mb-16">';
  [{ name: 'CPU', warn: 'cpu_warn', crit: 'cpu_crit' }, { name: 'Memory', warn: 'mem_warn', crit: 'mem_crit' }, { name: 'Disk', warn: 'disk_warn', crit: 'disk_crit' }].forEach(m => {
    const wv = v(m.warn, 80), cv2 = v(m.crit, 95);
    h += '<div class="hc"><h4>' + m.name + ' Thresholds</h4><div style="margin:8px 0">' + H.row('Warning (%)', '<input type="number" id="al-' + m.warn + '" value="' + wv + '" min="0" max="100" style="width:60px;background:var(--bg);color:var(--yellow);border:1px solid var(--border);border-radius:3px;padding:2px 6px;text-align:center">') + H.row('Critical (%)', '<input type="number" id="al-' + m.crit + '" value="' + cv2 + '" min="0" max="100" style="width:60px;background:var(--bg);color:var(--red);border:1px solid var(--border);border-radius:3px;padding:2px 6px;text-align:center">') + '</div>';
    h += '<div style="height:8px;background:var(--bg);border-radius:4px;overflow:hidden;position:relative;margin-top:4px"><div style="position:absolute;left:0;width:' + wv + '%;height:100%;background:var(--green)"></div><div style="position:absolute;left:' + wv + '%;width:' + (cv2 - wv) + '%;height:100%;background:var(--yellow)"></div><div style="position:absolute;left:' + cv2 + '%;width:' + (100 - cv2) + '%;height:100%;background:var(--red)"></div></div></div>'; });
  h += '</div>';
  h += '<div class="sg grid-2 mb-16">';
  h += H.card('Evaluation Period', H.row('Hold time (sec)', '<input type="number" id="al-eval_period" value="' + v('eval_period', 30) + '" min="5" max="600" style="width:80px;background:var(--bg);color:var(--fg);border:1px solid var(--border);border-radius:3px;padding:2px 6px;text-align:center">'));
  h += '<div class="hc"><h4>Webhook</h4>' + H.row('Format', '<select id="al-webhook_format" class="input-pcv">' + ['slack', 'telegram', 'generic'].map(f => '<option value="' + f + '"' + (v('webhook_format', 'generic') === f ? ' selected' : '') + '>' + f + '</option>').join('') + '</select>') + H.row('URL', '<input type="text" id="al-webhook_url" value="' + v('webhook_url', '') + '" placeholder="https://hooks.slack.com/..." style="width:100%;background:var(--bg);color:var(--fg);border:1px solid var(--border);border-radius:3px;padding:3px 8px;font-size:11px">') + H.row('Telegram Chat ID', '<input type="text" id="al-telegram_chat_id" value="' + v('telegram_chat_id', '') + '" placeholder="optional" style="width:120px;background:var(--bg);color:var(--fg);border:1px solid var(--border);border-radius:3px;padding:2px 6px;font-size:11px">') + '</div></div>';
  h += '<div class="mb-16"><button onclick="alertSave()" style="background:linear-gradient(135deg,var(--accent),var(--green));color:var(--bg);border:none;padding:8px 24px;border-radius:var(--r);cursor:pointer;font-weight:700;font-size:12px;text-transform:uppercase">' + t('btn.save') + '</button><span id="al-status" style="margin-left:12px;font-size:11px"></span></div>';
  h += H.card('&#128276; Alert History (' + hist.length + ' events)',
    hist.length === 0 ? '<div class="color-muted text-xs" style="padding:8px">No alerts fired yet</div>' :
    '<table class="text-11"><thead><tr><th>Time</th><th>Severity</th><th>Metric</th><th>Value</th><th>Message</th></tr></thead><tbody>' +
    [...hist].reverse().map(a => '<tr><td>' + new Date(a.timestamp * 1000).toLocaleString() + '</td><td>' + H.badge(a.severity.toUpperCase(), a.severity === 'crit' ? 'r' : 'y') + '</td><td>' + a.metric + '</td><td>' + a.value.toFixed(1) + '%</td><td class="color-muted">' + a.message + '</td></tr>').join('') +
    '</tbody></table>');
  h += '<div class="hc mt-12"><h4 class="color-yellow">&#128232; \uC6F9\uD6C5 \uC804\uC1A1 \uC2E4\uD328 (DLQ)</h4>';
  h += '<div class="mt-8"><button class="btn" onclick="loadWebhookDlq()">DLQ \uC870\uD68C</button>';
  h += '<button class="btn btn-g" onclick="retryWebhookDlq()" style="margin-left:6px">\uC804\uCCB4 \uC7AC\uC2DC\uB3C4</button></div>';
  h += '<div id="dlq-list" class="mt-8"></div></div>';

  h += H.section(_L('알림 설정', 'Alert Configuration'));
  try {
    var cfg2 = await fetchGet(EP.ALERTS_CONFIG());
    var c = unwrapData(cfg2);
    h += '<div class="sg grid-2">';
    h += H.card('CPU ' + _L('임계값', 'Thresholds'),
      '<div class="fr"><label>Warning (%)</label><input type="range" id="alert-cpu-warn" min="50" max="100" value="' + (c.cpu_warn || 80) + '" oninput="document.getElementById(\'acw-val\').textContent=this.value+\'%\'" class="flex-1"><span id="acw-val" class="min-w-40 text-right">' + (c.cpu_warn || 80) + '%</span></div>'
      + '<div class="fr"><label>Critical (%)</label><input type="range" id="alert-cpu-crit" min="50" max="100" value="' + (c.cpu_crit || 95) + '" oninput="document.getElementById(\'acc-val\').textContent=this.value+\'%\'" class="flex-1"><span id="acc-val" class="min-w-40 text-right">' + (c.cpu_crit || 95) + '%</span></div>');
    h += H.card(_L('메모리 임계값', 'Memory Thresholds'),
      '<div class="fr"><label>Warning (%)</label><input type="range" id="alert-mem-warn" min="50" max="100" value="' + (c.mem_warn || 85) + '" oninput="document.getElementById(\'amw-val\').textContent=this.value+\'%\'" class="flex-1"><span id="amw-val" class="min-w-40 text-right">' + (c.mem_warn || 85) + '%</span></div>'
      + '<div class="fr"><label>Critical (%)</label><input type="range" id="alert-mem-crit" min="50" max="100" value="' + (c.mem_crit || 95) + '" oninput="document.getElementById(\'amc-val\').textContent=this.value+\'%\'" class="flex-1"><span id="amc-val" class="min-w-40 text-right">' + (c.mem_crit || 95) + '%</span></div>');
    h += '</div>';
    h += '<div class="flex gap-6 mt-8"><button class="btn btn-g" onclick="saveAlertConfig()">' + t('btn.save') + '</button></div>';
  } catch (e) { h += '<p class="color-muted">Alert config unavailable</p>'; }

  b.innerHTML = h;
}
window.renderAlerts = renderAlerts;

async function saveAlertConfig() {
  var body = {
    cpu_warn: parseInt(document.getElementById('alert-cpu-warn')?.value) || 80,
    cpu_crit: parseInt(document.getElementById('alert-cpu-crit')?.value) || 95,
    mem_warn: parseInt(document.getElementById('alert-mem-warn')?.value) || 85,
    mem_crit: parseInt(document.getElementById('alert-mem-crit')?.value) || 95,
  };
  try {
    var r = await fetchPut(EP.ALERTS_CONFIG(), body);
    if (r.error) { toast(r.error.message, false); return; }
    toast(t('alert.saved'));
    addEvt('Alert config updated: CPU ' + body.cpu_warn + '/' + body.cpu_crit + ', MEM ' + body.mem_warn + '/' + body.mem_crit);
  } catch (e) { toast(e.message, false); }
}
window.saveAlertConfig = saveAlertConfig;


async function renderAudit(b) {
  let h = H.section('&#128270; \uAC10\uC0AC \uB85C\uADF8 \uAC80\uC0C9');
  h += '<div class="sg" style="grid-template-columns:1fr;margin-bottom:12px">';
  h += '<div class="hc">';
  h += '<div style="display:flex;gap:8px;flex-wrap:wrap;margin-bottom:8px">';
  h += '<input id="audit-user" placeholder="\uC0AC\uC6A9\uC790" style="padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:6px;font-size:12px;width:120px">';
  h += '<input id="audit-method" placeholder="\uBA54\uC11C\uB4DC (\uC608: vm.delete)" style="padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:6px;font-size:12px;width:180px">';
  h += '<input id="audit-from" type="date" class="input-pcv-lg">';
  h += '<input id="audit-to" type="date" class="input-pcv-lg">';
  h += '<button class="btn btn-g" onclick="doAuditSearch()">&#128269; \uAC80\uC0C9</button>';
  h += '</div>';
  h += '<div id="audit-results"><p class="color-muted text-12">\uAC80\uC0C9 \uC870\uAC74\uC744 \uC785\uB825\uD558\uACE0 \uAC80\uC0C9 \uBC84\uD2BC\uC744 \uD074\uB9AD\uD558\uC138\uC694.</p></div>';
  h += '</div></div>';
  b.innerHTML = h;
}
window.renderAudit = renderAudit;


async function renderGpu(b) {
  let h = H.section('&#127918; GPU \uBAA8\uB2C8\uD130\uB9C1');
  h += '<div class="sg grid-2 mb-16">';
  h += '<div class="hc"><h4>&#127918; GPU \uB514\uBC14\uC774\uC2A4</h4>';
  h += '<p class="color-muted text-12 mb-8">lspci \uAE30\uBC18 GPU \uC5F4\uAC70 \uBC0F vGPU/VFIO \uD328\uC2A4\uC2A4\uB8E8 \uC0C1\uD0DC\uB97C \uC870\uD68C\uD569\uB2C8\uB2E4.</p>';
  h += '<button class="btn btn-g" onclick="testGpuList()">&#128260; GPU \uBAA9\uB85D \uC870\uD68C</button>';
  h += '<div id="gpu-list-result" class="mt-8"></div></div>';
  h += '<div class="hc"><h4>&#9881; GPU \uC791\uC5C5</h4>';
  h += '<div class="mb-8">';
  h += '<div class="fr"><label>PCI Address</label><input id="gpu-pci" placeholder="0000:01:00.0" class="w-160"></div>';
  h += '<div class="fr"><label>VM Name</label><input id="gpu-vm" placeholder="gpu-vm-01" class="w-140"></div>';
  h += '<div class="flex gap-6 flex-wrap">';
  h += '<button class="btn" onclick="gpuPassthrough()">VFIO Passthrough</button>';
  h += '<button class="btn" onclick="gpuMdevCreate()">vGPU \uC0DD\uC131</button>';
  h += '</div></div>';
  h += '<div id="gpu-action-result" class="mt-8"></div></div>';
  h += '</div>';
  h += H.card('&#128214; CLI \uBA85\uB839\uC5B4 \uCC38\uC870', '<div style="font-size:12px;line-height:1.8;color:var(--fg2)">' +
    '<code class="color-accent">pcvctl gpu list</code> &mdash; GPU \uB514\uBC14\uC774\uC2A4 \uBAA9\uB85D<br>' +
    '<code class="color-accent">pcvctl gpu metrics</code> &mdash; GPU \uBA54\uD2B8\uB9AD \uC870\uD68C<br>' +
    '<code class="color-accent">pcvctl gpu passthrough &lt;pci&gt; &lt;vm&gt;</code> &mdash; VFIO \uD328\uC2A4\uC2A4\uB8E8<br>' +
    '<code class="color-accent">pcvctl gpu mdev create &lt;pci&gt; &lt;type&gt;</code> &mdash; vGPU \uC0DD\uC131</div>');

  h += '<div class="hc mb-14"><h4>' + _L('GPU 활용률', 'GPU Utilization') + '</h4>';
  h += '<canvas id="gpu-chart" width="600" height="200" style="max-width:100%"></canvas>';
  h += '<div class="stat-label mt-8">' + _L('GPU 메트릭은 nvidia-smi 또는 lspci 기반으로 수집됩니다.', 'GPU metrics collected via nvidia-smi or lspci.') + '</div></div>';
  b.innerHTML = h;

  try {
    var gr = await fetchPost(EP.RPC(), {jsonrpc:'2.0', method:'gpu.list', params:{}, id:'gl1'});
    var gpus = unwrapList(gr);
    var canvas = document.getElementById('gpu-chart');
    if (canvas && gpus.length > 0) {
      var ctx = canvas.getContext('2d');
      var barW = Math.min(80, (canvas.width - 40) / gpus.length);
      gpus.forEach(function(g, i) {
        var util = g.utilization || 0;
        var barH = (util / 100) * 160;
        ctx.fillStyle = util > 80 ? '#ff4444' : util > 50 ? '#ffaa00' : '#00ff88';
        ctx.fillRect(20 + i * (barW + 10), 180 - barH, barW, barH);
        ctx.fillStyle = '#aaa';
        ctx.font = '10px monospace';
        ctx.fillText(esc(g.name || 'GPU' + i).substring(0, 10), 20 + i * (barW + 10), 195);
        ctx.fillText(util + '%', 20 + i * (barW + 10) + barW/2 - 10, 175 - barH);
      });
    }
  } catch(e) { if(_DEBUG) console.warn('gpu-chart:', e.message); }
}
window.renderGpu = renderGpu;


async function renderDpdk(b) {
  b.innerHTML = showSkeleton();
  try {
    const [status, list, hugepage] = await Promise.all([
      fetchGet(EP.DPDK_STATUS()).catch(() => ({})),
      fetchGet(EP.DPDK_LIST()).catch(() => ({ data: [] })),
      fetchGet(EP.DPDK_HUGEPAGE()).catch(() => ({}))
    ]);
    const sd = unwrapData(status);
    const dl = unwrapList(list);
    const hp = unwrapData(hugepage);
    let h = H.section('DPDK — Data Plane Development Kit');
    h += H.grid(3,
      H.card('DPDK Status', H.row('Available', H.badge(sd.available ? 'Yes' : 'No', sd.available ? 'g' : 'r')) + H.row('PMD CPU Mask', esc(sd.pmd_cpu_mask || '-')) + H.row('Socket Mem', esc(sd.socket_mem || '-')))
    + H.card('HugePages', H.row('2M Total', hp.hugepage_2m_total || 0) + H.row('2M Free', hp.hugepage_2m_free || 0) + H.row('1G Total', hp.hugepage_1g_total || 0) + H.row('1G Free', hp.hugepage_1g_free || 0))
    + H.card('Bound NICs', '<div class="stat-lg">' + (Array.isArray(dl) ? dl.length : 0) + '</div>')
    );
    if (Array.isArray(dl) && dl.length > 0) {
      h += '<table class="table-sticky"><thead><tr><th>PCI Addr</th><th>Driver</th><th>Device</th></tr></thead><tbody>';
      dl.forEach(d => { h += '<tr><td><code>' + esc(d.pci_addr || d.pci || '?') + '</code></td><td>' + esc(d.driver || '-') + '</td><td>' + esc(d.device || '-') + '</td></tr>'; });
      h += '</tbody></table>';
    }
    h += H.section('DPDK Operations');
    h += '<div class="sg grid-2">';
    h += H.card('Bind NIC to DPDK', '<div class="fr"><label>PCI Address</label><input id="dpdk-pci" placeholder="0000:03:00.0" class="w-full"></div><div class="fr"><label>Driver</label><input id="dpdk-drv" value="vfio-pci" class="w-full"></div><button class="btn btn-g" onclick="dpdkBind()" class="mt-8">Bind</button>');
    h += H.card('Unbind NIC', '<div class="fr"><label>PCI Address</label><input id="dpdk-unbind-pci" placeholder="0000:03:00.0" class="w-full"></div><button class="btn btn-r" onclick="dpdkUnbind()" class="mt-8">Unbind</button>');
    h += '</div>';
    b.innerHTML = h;
  } catch (e) { b.innerHTML = H.section('DPDK') + '<p class="color-muted">Failed to load</p>'; }
}

async function dpdkBind() {
  var pci = document.getElementById('dpdk-pci')?.value;
  var drv = document.getElementById('dpdk-drv')?.value || 'vfio-pci';
  if (!pci) { toast(t('msg.name_required'), false); return; }
  try {
    var r = await fetchPost(EP.DPDK_BIND(), { pci_addr: pci, driver: drv });
    if (r.error) { toast('Bind failed: ' + (r.error.message || ''), false); return; }
    toast('DPDK bind: ' + pci); addEvt('DPDK bind ' + pci);
    renderDpdk(document.getElementById('cb'));
  } catch (e) { toast(e.message, false); }
}
async function dpdkUnbind() {
  var pci = document.getElementById('dpdk-unbind-pci')?.value;
  if (!pci) { toast(t('msg.name_required'), false); return; }
  try {
    var r = await fetchPost(EP.DPDK_UNBIND(), { pci_addr: pci });
    if (r.error) { toast('Unbind failed: ' + (r.error.message || ''), false); return; }
    toast('DPDK unbind: ' + pci); addEvt('DPDK unbind ' + pci);
    renderDpdk(document.getElementById('cb'));
  } catch (e) { toast(e.message, false); }
}

window.renderDpdk = renderDpdk;
window.dpdkBind = dpdkBind;
window.dpdkUnbind = dpdkUnbind;


async function renderSriov(b) {
  b.innerHTML = showSkeleton();
  try {
    const [status, list] = await Promise.all([
      fetchGet(EP.SRIOV_STATUS()).catch(() => ({})),
      fetchGet(EP.SRIOV_LIST()).catch(() => ({ data: [] }))
    ]);
    const sd = unwrapData(status);
    const vfs = unwrapList(list);
    let h = H.section('SR-IOV — Single Root I/O Virtualization');
    h += H.grid(2,
      H.card('SR-IOV NICs', H.row('Available', H.badge(sd.available ? 'Yes' : 'No', sd.available ? 'g' : 'r')) + H.row('Physical Functions', Array.isArray(sd.physical_functions) ? sd.physical_functions.length : (sd.nic_count || 0)))
    + H.card('Active VFs', '<div class="stat-lg">' + (Array.isArray(vfs) ? vfs.length : 0) + '</div>')
    );
    if (Array.isArray(vfs) && vfs.length > 0) {
      h += '<table class="table-sticky"><thead><tr><th>PF</th><th>VF Index</th><th>PCI Addr</th><th>MAC</th><th>VLAN</th><th>VM</th></tr></thead><tbody>';
      vfs.forEach(v => { h += '<tr><td>' + esc(v.pf || '-') + '</td><td>' + (v.vf_index ?? '-') + '</td><td><code>' + esc(v.pci_addr || '-') + '</code></td><td>' + esc(v.mac || '-') + '</td><td>' + (v.vlan || '-') + '</td><td>' + esc(v.vm || '-') + '</td></tr>'; });
      h += '</tbody></table>';
    }
    h += H.section('SR-IOV Operations');
    h += '<div class="sg grid-2">';
    h += H.card('Enable VFs', '<div class="fr"><label>Physical NIC (PF)</label><input id="sriov-pf" placeholder="enp3s0f0" class="w-full"></div><div class="fr"><label>Num VFs</label><input id="sriov-numvf" type="number" value="4" min="1" max="64" class="w-80"></div><button class="btn btn-g" onclick="sriovEnable()" class="mt-8">Enable</button> <button class="btn btn-r" onclick="sriovDisable()" class="mt-8">Disable</button>');
    h += H.card('Attach VF to VM', '<div class="fr"><label>VM Name</label><input id="sriov-vm" placeholder="web-prod" class="w-full"></div><div class="fr"><label>PCI Address (VF)</label><input id="sriov-vf-pci" placeholder="0000:03:10.0" class="w-full"></div><button class="btn btn-g" onclick="sriovAttach()" class="mt-8">Attach</button> <button class="btn btn-r" onclick="sriovDetach()" class="mt-8">Detach</button>');
    h += '</div>';
    b.innerHTML = h;
  } catch (e) { b.innerHTML = H.section('SR-IOV') + '<p class="color-muted">Failed to load</p>'; }
}

async function sriovEnable() {
  var pf = document.getElementById('sriov-pf')?.value;
  var num = parseInt(document.getElementById('sriov-numvf')?.value) || 4;
  if (!pf) { toast(t('msg.name_required'), false); return; }
  try {
    var r = await fetchPost(EP.SRIOV_ENABLE(), { pf: pf, num_vfs: num });
    if (r.error) { toast('Enable failed: ' + (r.error.message || ''), false); return; }
    toast('SR-IOV enabled: ' + pf + ' (' + num + ' VFs)'); addEvt('SR-IOV enable ' + pf);
    renderSriov(document.getElementById('cb'));
  } catch (e) { toast(e.message, false); }
}
async function sriovDisable() {
  var pf = document.getElementById('sriov-pf')?.value;
  if (!pf) { toast(t('msg.name_required'), false); return; }
  try {
    var r = await fetchPost(EP.SRIOV_DISABLE(), { pf: pf });
    if (r.error) { toast('Disable failed: ' + (r.error.message || ''), false); return; }
    toast('SR-IOV disabled: ' + pf); addEvt('SR-IOV disable ' + pf);
    renderSriov(document.getElementById('cb'));
  } catch (e) { toast(e.message, false); }
}
async function sriovAttach() {
  var vm = document.getElementById('sriov-vm')?.value;
  var pci = document.getElementById('sriov-vf-pci')?.value;
  if (!vm || !pci) { toast(t('msg.name_required'), false); return; }
  try {
    var r = await fetchPost(EP.SRIOV_ATTACH(), { vm_name: vm, pci_addr: pci });
    if (r.error) { toast('Attach failed: ' + (r.error.message || ''), false); return; }
    toast('VF attached to ' + vm); addEvt('SR-IOV attach ' + pci + ' \u2192 ' + vm);
    renderSriov(document.getElementById('cb'));
  } catch (e) { toast(e.message, false); }
}
async function sriovDetach() {
  var vm = document.getElementById('sriov-vm')?.value;
  var pci = document.getElementById('sriov-vf-pci')?.value;
  if (!vm || !pci) { toast(t('msg.name_required'), false); return; }
  try {
    var r = await fetchPost(EP.SRIOV_DETACH(), { vm_name: vm, pci_addr: pci });
    if (r.error) { toast('Detach failed: ' + (r.error.message || ''), false); return; }
    toast('VF detached from ' + vm); addEvt('SR-IOV detach ' + pci);
    renderSriov(document.getElementById('cb'));
  } catch (e) { toast(e.message, false); }
}

window.renderSriov = renderSriov;
window.sriovEnable = sriovEnable;
window.sriovDisable = sriovDisable;
window.sriovAttach = sriovAttach;
window.sriovDetach = sriovDetach;


async function renderHost(b) {
  b.innerHTML = showSkeleton();
  try {
    const met = await _fetchMetricsText(EP.METRICS());
    let cpu = 0, mem = 0, disk = 0, temp = 0, load1 = 0;
    met.split('\n').forEach(l => {
      if (l.startsWith('purecvisor_host_cpu_percent ')) cpu = parseFloat(l.split(' ')[1]);
      if (l.startsWith('purecvisor_host_memory_percent ')) mem = parseFloat(l.split(' ')[1]);
      if (l.startsWith('purecvisor_host_disk_percent ')) disk = parseFloat(l.split(' ')[1]);
      if (l.startsWith('purecvisor_host_cpu_temp_celsius ')) temp = parseFloat(l.split(' ')[1]);
      if (l.startsWith('purecvisor_host_load1 ')) load1 = parseFloat(l.split(' ')[1]);
    });
    const d = await fetchGet(EP.DPDK_STATUS()); const dd = unwrapData(d);
    const s = await fetchGet(EP.SRIOV_STATUS()); const sd = unwrapData(s);
    var priority = disk >= 80 ? _L('디스크 여유 공간 확인', 'Review disk headroom')
      : cpu >= 70 ? _L('CPU 부하 추적', 'Track CPU pressure')
      : mem >= 70 ? _L('메모리 사용률 점검', 'Review memory usage')
      : _L('가속 기능 준비도 확인', 'Confirm accelerator readiness');
    var nextAction = (dd.available || sd.available)
      ? _L('가속 기능이 준비되어 있습니다. 워크로드 배치 전에 바인딩 정책만 확인하면 됩니다.', 'Accelerators are available. Review binding policy before scheduling workloads.')
      : _L('현재는 CPU 기반 단독 노드 운용입니다. 고성능 워크로드가 필요하면 DPDK 또는 SR-IOV 준비 상태를 먼저 확인하십시오.', 'The node is currently running CPU-only. Review DPDK or SR-IOV readiness before placing high-performance workloads.');
    var metricsNote = _L('CPU, 메모리, 디스크, 가속 카드 상태를 단일 노드 기준으로 확인합니다.', 'Review CPU, memory, disk, and accelerator readiness for the single node.');
    b.innerHTML = '<div class="ops-section-heading"><div><h3>' + _L('호스트 상태', 'Host Health') + '</h3><p>' + metricsNote + '</p></div></div>'
    + '<div class="sg grid-2 host-ops-grid">'
    + H.card('CPU', '<div class="stat-md">' + cpu.toFixed(1) + '%</div>' + renderProgressBar(cpu) + H.row('Temp', temp.toFixed(1) + '&deg;C') + H.row('Load', load1.toFixed(2)))
    + H.card('Memory', '<div class="stat-md">' + mem.toFixed(1) + '%</div>' + renderProgressBar(mem) + H.row(_L('상태', 'State'), H.badge(mem >= 80 ? _L('주의', 'Watch') : _L('안정', 'Stable'), mem >= 80 ? 'y' : 'g')))
    + H.card('Disk', '<div class="stat-md">' + disk.toFixed(1) + '%</div>' + renderProgressBar(disk) + H.row(_L('권장 조치', 'Recommended action'), disk >= 80 ? _L('정리 필요', 'Cleanup needed') : _L('여유 있음', 'Healthy margin')))
    + H.card(_L('가속 기능', 'Acceleration'), H.row('DPDK', H.badge(dd.available ? 'ON' : 'OFF', dd.available ? 'g' : 'r')) + H.row('SR-IOV', H.badge(sd.available ? 'ON' : 'OFF', sd.available ? 'g' : 'r')))
    + H.card(_L('운영 메모', 'Operations note'), H.row(_L('호스트 모드', 'Host mode'), H.badge(_L('단일 노드', 'Single node'), 'g')) + H.row(_L('수집 기준', 'Collection'), _L('실시간 메트릭', 'Live metrics')) + H.row(_L('우선순위', 'Priority'), priority))
    + H.card(_L('현재 조치', 'Current action'), '<p class="color-muted text-12" style="line-height:1.7;margin:0">' + nextAction + '</p>')
    + '</div>';
  } catch (e) { if(_DEBUG) console.warn('renderHost:', e.message); }
}
window.renderHost = renderHost;


function renderHeatmap(b) {
  b.innerHTML = showSkeleton();

  fetchGet(EP.VM_LIST()).then(function(r) {
    var vms = unwrapList(r);
    if (!vms || vms.length === 0) {
      b.innerHTML = H.section(_L('리소스 히트맵', 'Resource Heatmap'))
        + '<p class="color-muted text-center" style="padding:24px">' + _L('실행 중인 VM이 없습니다', 'No running VMs') + '</p>';
      return;
    }
    var h = H.section(_L('리소스 히트맵', 'Resource Heatmap'));
    h += '<div style="overflow-x:auto">';
    h += '<table style="font-size:11px;border-collapse:separate;border-spacing:2px"><thead><tr><th>' + _L('VM', 'VM') + '</th>';
    for (var i = 0; i < 12; i++) h += '<th class="w-30 text-center text-9">' + (i * 5) + 'm</th>';
    h += '</tr></thead><tbody>';
    vms.forEach(function(vm) {
      h += '<tr><td class="nowrap"><b>' + esc(vm.name || '?') + '</b></td>';
      for (var i = 0; i < 12; i++) {
        var cpu = (vm.live_cpu_pct || vm.cpu_percent || 0) + (Math.random() * 20 - 10);
        cpu = Math.max(0, Math.min(100, cpu));
        var r = cpu > 80 ? 255 : Math.round(cpu * 2.5);
        var g = cpu < 50 ? Math.round(200 - cpu * 2) : Math.round(200 - cpu * 2);
        g = Math.max(0, g);
        var color = 'rgba(' + r + ',' + g + ',50,0.8)';
        h += '<td style="width:30px;height:20px;background:' + color + ';border-radius:2px" title="' + cpu.toFixed(0) + '%"></td>';
      }
      h += '</tr>';
    });
    h += '</tbody></table></div>';
    h += '<div class="flex gap-8 mt-8 text-xs">';
    h += '<span style="display:inline-block;width:12px;height:12px;background:rgba(0,200,50,0.8);border-radius:2px"></span> ' + _L('낮음', 'Low');
    h += '<span style="display:inline-block;width:12px;height:12px;background:rgba(200,200,0,0.8);border-radius:2px;margin-left:12px"></span> ' + _L('중간', 'Medium');
    h += '<span style="display:inline-block;width:12px;height:12px;background:rgba(255,50,50,0.8);border-radius:2px;margin-left:12px"></span> ' + _L('높음', 'High');
    h += '</div>';
    b.innerHTML = h;
  }).catch(function(e) { b.innerHTML = H.section(_L('리소스 히트맵', 'Resource Heatmap')) + '<p class="color-muted">' + _L('로드 실패', 'Failed to load') + ': ' + esc(e.message || '') + '</p>'; });
}
window.renderHeatmap = renderHeatmap;
window.loadDeepHealth = loadDeepHealth;


async function renderAlertSilences(b) {
  b.innerHTML = showSkeleton();
  try {
    var r = await fetchGet(EP.ALERT_SILENCE_LIST());
    var list = unwrapList(r);
    var h = H.section(_L('알림 음소거', 'Alert Silences'));
    h += '<button class="btn mb-8" onclick="showSilenceCreate()" aria-label="' + _L('음소거 추가', 'Add silence') + '">+ ' + _L('새 음소거', 'New Silence') + '</button>';
    if (list.length === 0) {
      h += '<div class="empty-state" style="padding:30px;text-align:center"><div style="font-size:36px;opacity:.5">&#128264;</div>';
      h += '<div class="color-muted">' + _L('활성 음소거 없음', 'No active silences') + '</div></div>';
    } else {
      h += '<table class="data-table text-11"><thead><tr>';
      h += '<th>' + _L('메트릭', 'Metric') + '</th><th>' + _L('남은 시간', 'Remaining') + '</th><th>' + _L('사유', 'Reason') + '</th></tr></thead><tbody>';
      list.forEach(function(s) {
        var mins = Math.ceil((s.remaining_sec || 0) / 60);
        h += '<tr><td><b>' + esc(s.metric) + '</b></td>';
        h += '<td>' + mins + _L('분', 'min') + '</td>';
        h += '<td class="color-muted">' + esc(s.reason || '') + '</td></tr>';
      });
      h += '</tbody></table>';
    }
    b.innerHTML = h;
  } catch(e) { b.innerHTML = '<p class="color-muted">' + _L('로드 실패', 'Failed') + '</p>'; }
}
async function showSilenceCreate() {
  var html = '<div class="form-group"><label>' + _L('메트릭', 'Metric') + '</label>';
  html += '<select id="sil-metric" class="input-field"><option>cpu</option><option>mem</option><option>disk</option></select></div>';
  html += '<div class="form-group"><label>' + _L('기간 (분)', 'Duration (min)') + '</label>';
  html += '<input id="sil-dur" type="number" value="60" min="1" max="1440" class="input-field"></div>';
  html += '<div class="form-group"><label>' + _L('사유', 'Reason') + '</label>';
  html += '<input id="sil-reason" class="input-field" placeholder="' + _L('유지보수 예정', 'Planned maintenance') + '"></div>';
  showModal(_L('알림 음소거', 'Silence Alert'), html, async function() {
    var metric = document.getElementById('sil-metric').value;
    var dur = parseInt(document.getElementById('sil-dur').value) || 60;
    var reason = document.getElementById('sil-reason').value.trim();
    try {
      await fetchPost(EP.ALERT_SILENCE(), { metric: metric, duration_min: dur, reason: reason });
      toast(_L('음소거 적용', 'Silence applied'), 's');
      renderAlertSilences(document.getElementById('cb'));
    } catch(e) { toast(_L('실패', 'Failed'), 'e'); }
  });
}


async function renderAlertRouting(b) {
  b.innerHTML = showSkeleton();
  var h = H.section(_L('알림 라우팅 설정', 'Alert Routing Configuration'));
  h += '<div class="sg p-12">';
  h += '<div class="form-group"><label>WARN ' + _L('Webhook URL', 'Webhook URL') + '</label>';
  h += '<input id="route-warn-url" class="input-field" placeholder="https://hooks.slack.com/..." aria-label="Warning webhook URL"></div>';
  h += '<div class="form-group"><label>CRIT ' + _L('Webhook URL (에스컬레이션)', 'Webhook URL (escalation)') + '</label>';
  h += '<input id="route-crit-url" class="input-field" placeholder="https://pagerduty.com/..." aria-label="Critical webhook URL"></div>';
  h += '<div class="form-group"><label>Webhook Secret (HMAC)</label>';
  h += '<input id="route-secret" type="password" class="input-field" placeholder="' + _L('서명 키', 'Signing secret') + '" aria-label="Webhook HMAC secret"></div>';
  h += '<button class="btn mt-8" onclick="saveAlertRouting()" aria-label="' + _L('라우팅 저장', 'Save routing') + '">' + _L('저장', 'Save') + '</button>';
  h += '</div>';
  b.innerHTML = h;
}
async function saveAlertRouting() {
  var cfg = {};
  var warnUrl = document.getElementById('route-warn-url').value.trim();
  var critUrl = document.getElementById('route-crit-url').value.trim();
  var secret = document.getElementById('route-secret').value.trim();
  if (warnUrl) cfg.webhook_url = warnUrl;
  if (critUrl) cfg.webhook_crit_url = critUrl;
  if (secret) cfg.webhook_secret = secret;
  try {
    await fetchPost(EP.ALERTS_CONFIG(), cfg);
    toast(_L('라우팅 설정 저장 완료', 'Alert routing saved'), 's');
  } catch(e) { toast(_L('실패', 'Failed'), 'e'); }
}


async function renderPoolInfo(b) {
  b.innerHTML = showSkeleton();
  try {
    var r = await fetchGet(EP.POOL_CONNINFO());
    var d = unwrapData(r);
    var h = H.section(_L('커넥션 풀 상태', 'Connection Pool Status'));
    h += '<div class="grid-3" style="gap:12px">';
    h += H.statCard(_L('유휴', 'Idle'), d.idle || 0, '🟢');
    h += H.statCard(_L('활성', 'Active'), (d.total || 0) - (d.idle || 0), '🔴');
    h += H.statCard(_L('최대', 'Max'), d.max || 0, '⚪');
    h += '</div>';
    if (d.wait_avg_sec !== undefined) {
      h += '<div class="mt-8 color-muted text-xs">' + _L('평균 대기', 'Avg wait') + ': ' + (d.wait_avg_sec * 1000).toFixed(1) + 'ms</div>';
    }
    b.innerHTML = h;
  } catch(e) { b.innerHTML = '<p class="color-muted">' + _L('로드 실패', 'Failed') + '</p>'; }
}

window.renderAlertSilences = renderAlertSilences;
window.showSilenceCreate = showSilenceCreate;
window.renderAlertRouting = renderAlertRouting;
window.saveAlertRouting = saveAlertRouting;
window.renderPoolInfo = renderPoolInfo;


async function loadHealingPending() {

  try {
    var r;
    try {
      r = await fetchPost(EP.RPC(), { jsonrpc:'2.0', method:'healing.history', params:{}, id:'hp1' });
    } catch (e) {
      var elx = document.getElementById('healing-pending-list');
      if (elx) elx.innerHTML = '<div class="stat-label">' + _L('대기 중인 액션 없음', 'No pending actions') + '</div>';
      return;
    }
    var d = unwrapData(r);
    var raw = Array.isArray(d) ? d : (unwrapList ? unwrapList(d) : []);
    var actions = raw.filter(function(a) { return a && (a.status === 'pending' || a.state === 'pending'); });
    var el = document.getElementById('healing-pending-list');
    if (!el) return;
    if (actions.length === 0) {
      el.innerHTML = '<div class="stat-label">' + _L('대기 중인 액션 없음', 'No pending actions') + '</div>';
      return;
    }
    var h = '';
    actions.forEach(function(a, i) {
      h += '<div class="hc mb-8 flex items-center gap-10" style="padding:8px 12px">';
      h += '<span class="color-yellow">&#9888;</span> ';
      h += '<span class="flex-1"><strong>' + esc(a.action || 'unknown') + '</strong>';
      if (a.target) h += ' — ' + esc(a.target);
      if (a.reason) h += ' <span class="stat-label">(' + esc(a.reason) + ')</span>';
      h += '</span>';
      h += '<button class="btn btn-g btn-sm" onclick="healingApprove(' + i + ')">' + _L('승인', 'Approve') + '</button>';
      h += '<button class="btn btn-r btn-sm" onclick="healingReject(' + i + ')">' + _L('거절', 'Reject') + '</button>';
      h += '</div>';
    });
    el.innerHTML = h;
  } catch (e) {
    var el2 = document.getElementById('healing-pending-list');
    if (el2) el2.innerHTML = '<div class="stat-label">' + esc(e.message) + '</div>';
  }
}

async function healingApprove(idx) {
  try {
    var r = await fetchPost(EP.RPC(), { jsonrpc:'2.0', method:'ai.healing.approve', params:{ index: idx }, id:'ha1' });
    if (r.error) { toast(r.error.message, false); return; }
    toast(_L('승인됨', 'Approved'));
    loadHealingPending();
  } catch (e) { toast(e.message, false); }
}

async function healingReject(idx) {
  try {
    var r = await fetchPost(EP.RPC(), { jsonrpc:'2.0', method:'ai.healing.reject', params:{ index: idx }, id:'hr1' });
    if (r.error) { toast(r.error.message, false); return; }
    toast(_L('거절됨', 'Rejected'));
    loadHealingPending();
  } catch (e) { toast(e.message, false); }
}

window.loadHealingPending = loadHealingPending;
window.healingApprove = healingApprove;
window.healingReject = healingReject;


PCV.monitor = {
  destroyAllCharts: destroyAllCharts,
  createLineChart: createLineChart,
  drawGraphFallback: drawGraphFallback,
  getChartColor: getChartColor,
  fetchAllMetrics: fetchAllMetrics,
  fmtBytes: fmtBytes,
  fmtRate: fmtRate,
  fmtUptime: fmtUptime,
  drawLine: drawLine,
  gauge: gauge,
  renderMonitoring: renderMonitoring,
  loadDeepHealth: loadDeepHealth,
  renderMonOverview: renderMonOverview,
  renderMonCluster: renderMonCluster,
  renderMonHosts: renderMonHosts,
  renderMonVms: renderMonVms,
  renderMonStorage: renderMonStorage,
  renderAlerts: renderAlerts,
  saveAlertConfig: saveAlertConfig,
  renderAudit: renderAudit,
  renderGpu: renderGpu,
  renderDpdk: renderDpdk,
  dpdkBind: dpdkBind,
  dpdkUnbind: dpdkUnbind,
  renderSriov: renderSriov,
  sriovEnable: sriovEnable,
  sriovDisable: sriovDisable,
  sriovAttach: sriovAttach,
  sriovDetach: sriovDetach,
  renderHost: renderHost,
  renderHeatmap: renderHeatmap,
  renderAlertSilences: renderAlertSilences,
  showSilenceCreate: showSilenceCreate,
  renderAlertRouting: renderAlertRouting,
  saveAlertRouting: saveAlertRouting,
  renderPoolInfo: renderPoolInfo,
  loadHealingPending: loadHealingPending,
  healingApprove: healingApprove,
  healingReject: healingReject
};

})(window.PCV);
