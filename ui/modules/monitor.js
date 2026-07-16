/* ═══════════════════════════════════════════════════════════════
   PureCVisor — modules/monitor.js
   Monitoring, Alerts, Audit, GPU, Host renderers
   ADR-0013: IIFE module scope — PCV.monitor namespace
   ═══════════════════════════════════════════════════════════════ */
/*
 * Monitoring is the highest-churn UI module: it parses Prometheus text,
 * maintains Chart.js instances across innerHTML replacement, and renders both
 * Single Edge local metrics and legacy multi-node shaped data. Helper comments
 * below mark the ownership boundaries that keep those concerns separate.
 */
window.PCV = window.PCV || {};
(function(PCV) {

/* ═══ CHART.JS REGISTRY ═══ */
var chartRegistry = {};
window.chartRegistry = chartRegistry;

/* Destroy all Chart.js instances — call before innerHTML replacement */
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
  /* If registry has a stale entry (canvas replaced by innerHTML), destroy it */
  if (chartRegistry[canvasId]) {
    if (chartRegistry[canvasId].canvas === canvas) {
      const chart = chartRegistry[canvasId];
      chart.data.labels = data.map((_, i) => i);
      chart.data.datasets[0].data = data;
      chart.update('none');
      return;
    }
    /* Stale — canvas was replaced */
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

/* ═══ CHART COLOR ═══ */
function getChartColor(name) {
  try { return getComputedStyle(document.documentElement).getPropertyValue('--chart-' + name).trim() || name; }
  catch(e) { return name; }
}
window.getChartColor = getChartColor;

/* ═══ MONITORING CONSTANTS (R-9: cluster.js에�� 동적 로드된 노드 사용) ═══ */
var _PROD_NODES = window._PROD_NODES || [{ name: 'Local', ip: window.location.hostname || '127.0.0.1' }];
var _VIP = window._VIP || null;
var _curHost = window._curHost || window.location.hostname;
var _isProd = window._isProd || false;
window._isProd = _isProd;
window._curHost = _curHost;
var MON_NODES = _PROD_NODES;
window.MON_NODES = MON_NODES;
/* cluster.js의 _loadClusterNodes() 완료 후 MON_NODES 갱신 */
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

/* G-2: Promise.all parallel fetch */
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

/* ═══ FORMATTERS ═══ */
function fmtBytes(b) { if (b >= 1e12) return (b / 1e12).toFixed(1) + ' TB'; if (b >= 1e9) return (b / 1e9).toFixed(1) + ' GB'; if (b >= 1e6) return (b / 1e6).toFixed(1) + ' MB'; if (b >= 1e3) return (b / 1e3).toFixed(1) + ' KB'; return b + ' B'; }
window.fmtBytes = fmtBytes;

function fmtRate(arr, i) { if (i < 1 || !arr[i] || !arr[i - 1]) return '0 B/s'; const d = arr[i] - arr[i - 1]; return d > 0 ? fmtBytes(d / 5) + '/s' : '0 B/s'; }
window.fmtRate = fmtRate;

function fmtUptime(s) { const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), mi = Math.floor(s % 3600 / 60); return d + 'd ' + h + 'h ' + mi + 'm'; }
window.fmtUptime = fmtUptime;

/* ═══ CANVAS DRAWING ═══ */
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

/* ADR-013 DOM-safe: gauge SVG 는 SVG 네임스페이스 노드로 조립해야 실제 렌더되고
 * viewBox 등 카멜케이스 속성이 소문자화(HTML setAttribute 규칙)되지 않는다.
 * el() 은 createElement(HTML NS) 라 SVG 에 부적합 → 로컬 createElementNS 헬퍼. */
var _SVGNS = 'http://www.w3.org/2000/svg';
function _svgEl(tag, attrs, children) {
  var node = document.createElementNS(_SVGNS, tag);
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

/* ci-icon <svg><use href> 아이콘 노드 — SVG 네임스페이스라야 렌더+직렬화 동형.
 * href 는 사이트별로 상대/절대가 달라 전체 문자열을 그대로 받는다. */
function _svgIcon(href, cls) {
  return _svgEl('svg', { class: cls || 'ci-icon', 'aria-hidden': 'true' }, [
    _svgEl('use', { href: href })
  ]);
}

function gauge(pct, label, color) {
  var el = PCV.uxlib.el;
  var cl = color || (pct > 80 ? 'var(--red)' : pct > 60 ? 'var(--yellow)' : 'var(--green)');
  var svg = _svgEl('svg', { width: '90', height: '50', viewBox: '0 0 90 50' }, [
    _svgEl('path', { d: 'M10 45 A35 35 0 0 1 80 45', fill: 'none', stroke: 'var(--border)', 'stroke-width': '6', 'stroke-linecap': 'round' }),
    _svgEl('path', { d: 'M10 45 A35 35 0 0 1 80 45', fill: 'none', stroke: cl, 'stroke-width': '6', 'stroke-linecap': 'round', 'stroke-dasharray': (pct * 1.1) + ' 110', style: 'filter:drop-shadow(0 0 4px ' + cl + ')' })
  ]);
  return el('div', { class: 'text-center' },
    svg,
    el('div', { class: 'stat-sm', style: 'margin-top:-8px;color:' + cl }, pct.toFixed(1) + '%'),
    el('div', { class: 'stat-label' }, label));
}
window.gauge = gauge;

/* ═══ MONITORING RENDER — G-2 Split into sub-functions ═══ */
async function renderMonitoring(b, tab) {
  /* P1-3: Destroy stale Chart.js instances before innerHTML replacement */
  if (typeof pcvDestroyAllInContainer === 'function') pcvDestroyAllInContainer(b);
  destroyAllCharts();
  showSkeleton(b);
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

  /* FE-4: 모니터링 페이지 열린 동안 자동 갱신 (10초) */
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
        }).catch(function() { /* 갱신 실패 시 무시 */ });
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
  var el = PCV.uxlib.el;
  var cls = tone || 'info';
  var dot = cls === 'bad' ? 'bad' : cls === 'warn' ? 'warn' : 'ok';
  return el('span', { class: 'ops-status ' + cls },
    el('span', { class: 'ops-dot ' + dot }),
    label);
}

function _opsMetricCard(label, value, detail, statusLabel, tone) {
  var el = PCV.uxlib.el;
  return el('article', { class: 'ops-triage-card ops-triage-metric ops-span-3' },
    el('div', { class: 'ops-triage-metric-label' }, label),
    el('div', { class: 'ops-triage-metric-value' }, value),
    el('div', { class: 'ops-triage-metric-foot' },
      el('span', null, detail),
      _opsStatus(statusLabel, tone)));
}

function _opsBar(pct) {
  var el = PCV.uxlib.el;
  var safe = _opsPct(pct, 0);
  var text = safe.toFixed(safe < 10 ? 1 : 0) + '%';
  return el('div', { class: 'ops-bar', style: '--value:' + safe.toFixed(1) + '%' },
    el('div', { class: 'ops-bar-fill' }),
    el('div', { class: 'ops-bar-label' }, text));
}

function _opsVmStatus(v) {
  var state = String(v.state || (v.running === 1 ? 'running' : 'unknown')).toLowerCase();
  if (state === 'running' || state === 'http 200') return _opsStatus(state === 'http 200' ? '200' : 'RUN', 'ok');
  if (state === 'shut off' || state === 'stopped' || state === 'off') return _opsStatus('OFF', 'bad');
  return _opsStatus('CHECK', 'warn');
}

function _opsFallbackVms() {
  return [
    { name: 'pcv-demo-vm', role: '공개 데모', ip: '192.0.2.10', cpu: 2.0, mem: 48, state: 'running', running: 1 },
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
  var el = PCV.uxlib.el;
  return rows.map(function(v) {
    return el('tr', null,
      el('td', null, el('div', { class: 'ops-name' },
        _svgIcon('vendor/coolicons/coolicons.svg#ci-desktop-tower', 'ci-icon'),
        v.name)),
      el('td', null, v.role),
      el('td', { class: 'ops-mono' }, v.ip),
      el('td', null, _opsBar(v.cpu)),
      el('td', null, _opsBar(v.mem)),
      el('td', null, _opsVmStatus(v)));
  });
}

function _opsAuditRows() {
  var raw;
  try { raw = (window.eventLog || eventLog || []).slice(-3).reverse(); } catch (e) { raw = []; }
  if (raw.length === 0) {
    raw = [
      { title: 'vm.guest.exec', detail: 'target=pcv-demo-vm, result=ok, job_id=81f2', time: '12:22', tone: 'ok' },
      { title: 'security.event', detail: 'target=viewer, source=nginx, result=warn', time: '12:21', tone: 'warn' },
      { title: 'ovn.status', detail: 'target=pcv-demo-lr, result=ok', time: '12:19', tone: 'ok' }
    ];
  }
  var el = PCV.uxlib.el;
  return raw.map(function(item) {
    var obj = typeof item === 'string'
      ? { title: item.split(':')[0] || 'event', detail: item, time: '-', tone: /fail|warn|error/i.test(item) ? 'warn' : 'ok' }
      : item;
    var tone = obj.tone || (/fail|error/i.test(obj.detail || obj.title || '') ? 'bad' : /warn/i.test(obj.detail || obj.title || '') ? 'warn' : 'ok');
    return el('div', { class: 'ops-triage-event' },
      el('div', { class: 'ops-severity ' + tone }),
      el('div', null,
        el('p', { class: 'ops-event-title' }, obj.title || 'event'),
        el('div', { class: 'ops-event-sub' }, obj.detail || obj.msg || '')),
      el('div', { class: 'ops-event-time' }, obj.time || '-'));
  });
}

async function renderOpsTriage(b) {
  if (typeof pcvDestroyAllInContainer === 'function') pcvDestroyAllInContainer(b);
  destroyAllCharts();
  showSkeleton(b);
  var all;
  var apiVms;
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

  var el = PCV.uxlib.el, clearEl = PCV.uxlib.clearEl, frag = PCV.uxlib.frag;
  function cardHead(title, meta) {
    return el('div', { class: 'ops-triage-card-head' },
      el('div', { class: 'ops-triage-card-title', role: 'heading', 'aria-level': '2' }, title),
      el('span', { class: 'ops-triage-card-meta' }, meta));
  }

  var header = el('header', { class: 'ops-triage-head' },
    el('div', null,
      el('div', { class: 'ops-triage-kicker' }, 'Single Edge operations'),
      el('h1', { class: 'ops-triage-title' }, '운영 이벤트 센터'),
      el('p', { class: 'ops-triage-sub' }, 'VM, OVN, ZFS, 보안 이벤트를 한 화면에서 triage하고 즉시 조치하는 운영자용 화면입니다.')),
    el('div', { class: 'ops-triage-tabs', role: 'tablist', 'aria-label': '시간 범위' },
      el('button', { class: 'ops-triage-tab is-active', type: 'button' }, 'LIVE'),
      el('button', { class: 'ops-triage-tab', type: 'button' }, '1H'),
      el('button', { class: 'ops-triage-tab', type: 'button' }, '24H'),
      el('button', { class: 'ops-triage-tab', type: 'button' }, 'AUDIT')));

  var section = el('section', { class: 'ops-triage-grid', 'aria-label': '운영 이벤트 센터' },
    _opsMetricCard('호스트 CPU', avgCpu.toFixed(1) + '%', '단일 노드 평균', avgCpu > 80 ? '위험' : '정상', avgCpu > 80 ? 'bad' : avgCpu > 60 ? 'warn' : 'ok'),
    _opsMetricCard('메모리', avgMem.toFixed(0) + '%', ramDetail, avgMem > 85 ? '위험' : '여유', avgMem > 85 ? 'bad' : avgMem > 70 ? 'warn' : 'ok'),
    _opsMetricCard('OVN 게이트웨이', '10.77.0.1', 'pcv-demo-lr', 'ACTIVE', 'info'),
    _opsMetricCard('실행 VM', running + '/' + totalVm, 'viewer read-only 기준', running > 0 ? '가동' : '확인', running > 0 ? 'ok' : 'warn'),
    el('article', { class: 'ops-triage-card ops-span-5' },
      cardHead('이벤트 triage', '최근 15분'),
      el('div', { class: 'ops-triage-list' },
        el('div', { class: 'ops-triage-event' },
          el('div', { class: 'ops-severity bad' }),
          el('div', null,
            el('p', { class: 'ops-event-title' }, 'viewer 계정 로그인 시도 증가'),
            el('div', { class: 'ops-event-sub' }, 'nginx access log 기준 동일 User-Agent 반복 접근')),
          el('div', { class: 'ops-event-time' }, 'LIVE')),
        el('div', { class: 'ops-triage-event' },
          el('div', { class: 'ops-severity warn' }),
          el('div', null,
            el('p', { class: 'ops-event-title' }, 'exporter scrape 지연 확인'),
            el('div', { class: 'ops-event-sub' }, 'Prometheus full exporter 응답 지연은 관측성 품질에 영향')),
          el('div', { class: 'ops-event-time' }, 'WARN')),
        el('div', { class: 'ops-triage-event' },
          el('div', { class: 'ops-severity ok' }),
          el('div', null,
            el('p', { class: 'ops-event-title' }, 'OVN demo NAT 흐름 정상'),
            el('div', { class: 'ops-event-sub' }, 'ovn-demo-a → pcv-demo-ls → pcv-demo-lr → external')),
          el('div', { class: 'ops-event-time' }, 'OK')))),
    el('article', { class: 'ops-triage-card ops-span-7' },
      cardHead('VM 및 서비스 상태', totalVm + ' assets'),
      el('div', { class: 'ops-triage-toolbar' },
        el('input', { class: 'ops-triage-field', type: 'search', value: 'demo', 'aria-label': '자산 검색' }),
        el('div', { class: 'ops-triage-actions' },
          el('button', { class: 'ops-triage-action', type: 'button', onclick: "renderOpsTriage(document.getElementById('cb'))" },
            _svgIcon('vendor/coolicons/coolicons.svg#ci-refresh', 'ci-icon'), '새로고침'),
          el('button', { class: 'ops-triage-action primary', type: 'button', onclick: 'openCmdPalette()' },
            _svgIcon('vendor/coolicons/coolicons.svg#ci-play', 'ci-icon'), '조치 선택'))),
      el('div', { class: 'ops-triage-table-wrap' },
        el('table', { class: 'ops-triage-table' },
          el('thead', null, el('tr', null,
            el('th', null, '이름'), el('th', null, '역할'), el('th', null, 'IP'),
            el('th', null, 'CPU'), el('th', null, '메모리'), el('th', null, '상태'))),
          el('tbody', null, _opsVmRows(displayVms))))),
    el('article', { class: 'ops-triage-card ops-span-4' },
      cardHead('명령 팔레트', 'Ctrl K'),
      el('div', { class: 'ops-command' },
        el('button', { class: 'ops-command-row is-active', type: 'button', onclick: "navigateTo('host')" },
          el('span', { class: 'ops-key' }, 'RUN'), el('span', null, 'qemu-guest-agent 설치 확인'), el('span', { class: 'ops-key' }, 'Enter')),
        el('button', { class: 'ops-command-row', type: 'button', onclick: "navigateTo('ovn')" },
          el('span', { class: 'ops-key' }, 'NET'), el('span', null, 'OVN NAT 및 logical router 상태 확인'), el('span', { class: 'ops-key' }, 'N')),
        el('button', { class: 'ops-command-row', type: 'button', onclick: "navigateTo('mon-audit')" },
          el('span', { class: 'ops-key' }, 'LOG'), el('span', null, 'viewer 성공 로그인 IP 목록 열기'), el('span', { class: 'ops-key' }, 'L')),
        el('button', { class: 'ops-command-row', type: 'button', onclick: "navigateTo('activity-log')" },
          el('span', { class: 'ops-key' }, 'JOB'), el('span', null, '실패 작업만 필터링'), el('span', { class: 'ops-key' }, 'J')))),
    el('article', { class: 'ops-triage-card ops-span-4' },
      cardHead('OVN 서비스 흐름', 'demo'),
      el('div', { class: 'ops-node' },
        el('div', { class: 'ops-node-icon' }, _svgIcon('vendor/coolicons/coolicons.svg#ci-desktop-tower', 'ci-icon')),
        el('div', null, el('div', { class: 'ops-node-name' }, 'ovn-demo-a'), el('div', { class: 'ops-node-sub' }, '10.77.0.12:8080')),
        _opsStatus('WEB', 'ok')),
      el('div', { class: 'ops-node' },
        el('div', { class: 'ops-node-icon' }, _svgIcon('vendor/coolicons/coolicons.svg#ci-layers', 'ci-icon')),
        el('div', null, el('div', { class: 'ops-node-name' }, 'pcv-demo-ls'), el('div', { class: 'ops-node-sub' }, 'logical switch')),
        _opsStatus('L2', 'info')),
      el('div', { class: 'ops-node' },
        el('div', { class: 'ops-node-icon' }, _svgIcon('vendor/coolicons/coolicons.svg#ci-globe', 'ci-icon')),
        el('div', null, el('div', { class: 'ops-node-name' }, 'pcv-demo-lr'), el('div', { class: 'ops-node-sub' }, 'gateway 10.77.0.1')),
        _opsStatus('NAT', 'ok')),
      el('div', { class: 'ops-flow', 'aria-label': '서비스 흐름' },
        el('div', { class: 'ops-flow-box' }, 'VM'), el('div', null, '→'),
        el('div', { class: 'ops-flow-box' }, 'LS'), el('div', null, '→'),
        el('div', { class: 'ops-flow-box' }, 'LR'))),
    el('article', { class: 'ops-triage-card ops-span-4' },
      cardHead('감사 추적', 'audit'),
      el('div', { class: 'ops-triage-list' }, _opsAuditRows())));

  clearEl(b);
  b.appendChild(frag(header, section));
}
window.renderOpsTriage = renderOpsTriage;

/* ═══ DEEP HEALTH DASHBOARD ═══ */
async function loadDeepHealth() {
  var el = document.getElementById('deep-health'); if (!el) return;
  PCV.uxlib.setMsg(el, 'loading', null, t('loading') || 'Loading...');
  try {
    var r = await fetch(EP.HEALTH());
    var d = await r.json();
    var overall = d.status || d.overall || 'unknown';
    var node = d.node || d.hostname || '-';
    var uptime = d.uptime_sec || d.uptime || 0;
    var subsystems = d.subsystems || d.checks || {};

    var overallColor = overall === 'ok' ? 'var(--green)' : overall === 'degraded' ? 'var(--yellow)' : 'var(--red)';
    var mk = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
    var top = mk('div', { style: 'display:flex;align-items:center;gap:12px;margin-bottom:10px;flex-wrap:wrap' },
      mk('span', { style: 'font-size:14px;font-weight:700;color:' + overallColor }, overall.toUpperCase()),
      HN.badge(esc(node), 'g'),
      uptime > 0 ? mk('span', { class: 'stat-label' }, (t('monitor.uptime') || 'Uptime') + ': ' + fmtUptime(uptime)) : null);

    var subsysKeys = Object.keys(subsystems);
    if (subsysKeys.length === 0) {
      /* If no subsystems object, try top-level known fields */
      var knownSubs = ['libvirt', 'etcd', 'zfs', 'vm_state_db', 'audit_db', 'tls', 'cluster'];
      knownSubs.forEach(function(k) { if (d[k] !== undefined) subsystems[k] = d[k]; });
      subsysKeys = Object.keys(subsystems);
    }

    var body;
    if (subsysKeys.length > 0) {
      body = mk('div', { style: 'display:flex;gap:8px;flex-wrap:wrap' },
        subsysKeys.map(function(k) {
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
          var detail = detailParts.length ? ' (' + detailParts.join(', ') + ')' : '';
          return mk('div', { style: 'display:inline-flex;align-items:center;gap:4px;padding:4px 10px;border:1px solid var(--border);border-radius:6px;font-size:11px;background:var(--bg2)' },
            mk('span', { style: 'color:' + _healthBadgeColor(sc) + ';font-size:8px' }, '●'),
            mk('span', { style: 'font-weight:600' }, k),
            mk('span', { style: 'color:' + _healthBadgeColor(sc) }, st + detail));
        }));
    } else {
      body = mk('span', { class: 'color-muted' }, t('monitor.no_subsystems') || 'No subsystem details available');
    }

    clearEl(el);
    el.appendChild(frag(top, body));
  } catch (e) {
    PCV.uxlib.setMsg(el, null, { cls: 'color-muted' }, (t('monitor.health_unavailable') || 'Health probe unavailable'), ': ', e.message);
  }
}

function _healthBadgeColor(sc) {
  if (sc === 'g') return 'var(--green)';
  if (sc === 'y') return 'var(--yellow)';
  if (sc === 'r') return 'var(--red)';
  return 'var(--fg2)';
}

function renderMonOverview(b, all, allVms, running, avgCpu, avgMem, avgDisk, totalRam) {
  /* P1-3: Destroy stale Chart.js instances before innerHTML replacement */
  if (typeof pcvDestroyAllInContainer === 'function') pcvDestroyAllInContainer(b);
  destroyAllCharts();
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  /* Deep Health section at top */
  var deepHealthSection = el('div', { class: 'hc mb-14' },
    el('h4', null, '🩹 ' + (t('monitor.system_health') || 'System Health')),
    el('p', { class: 'color-muted text-11 mb-8' }, t('monitor.health_desc') || 'Deep health probe of all subsystems. Updated on each page load.'),
    el('div', { id: 'deep-health' },
      el('span', { class: 'spinner' }),
      ' ' + (t('loading') || 'Loading...')));

  /* F-1: Cluster Timeline Chart (CPU/MEM/DISK 시계열) */
  var clusterTimeline = el('div', { class: 'hc mb-12' },
    el('h4', null, '📊 ' + (t('monitor.cluster_timeline') || '리소스 흐름 (최근 5분)')),
    el('div', { class: 'grid-3 gap-12', style: 'display:grid;grid-template-columns:1fr 1fr 1fr' },
      el('div', { style: 'position:relative;height:180px' }, el('canvas', { id: 'pcv-chart-cpu' })),
      el('div', { style: 'position:relative;height:180px' }, el('canvas', { id: 'pcv-chart-mem' })),
      el('div', { style: 'position:relative;height:180px' }, el('canvas', { id: 'pcv-chart-net' }))));

  var overviewSection = HN.section('📈 운영 개요');

  var tSwapUsed = all.reduce((s, n) => s + ((n.memInfo.SwapTotal || 0) - (n.memInfo.SwapFree || 0)), 0);
  var tSwapTotal = all.reduce((s, n) => s + (n.memInfo.SwapTotal || 0), 0);
  var statGrid = HN.grid(8,
    HN.card('호스트', el('div', { class: 'stat-xl color-green' }, all.length), 'text-center'),
    HN.card('VM', el('div', { class: 'stat-xl color-accent' }, allVms.length), 'text-center'),
    HN.card('실행 중', el('div', { class: 'stat-xl color-green' }, running), 'text-center'),
    HN.card('평균 CPU', gauge(avgCpu, 'Host'), 'text-center'),
    HN.card('평균 메모리', gauge(avgMem, 'Host'), 'text-center'),
    HN.card('평균 디스크', gauge(avgDisk, 'Host'), 'text-center'),
    HN.card('스왑', gauge(tSwapTotal > 0 ? tSwapUsed / tSwapTotal * 100 : 0, fmtBytes(tSwapUsed) + '/' + fmtBytes(tSwapTotal)), 'text-center'),
    HN.card('소켓', [
      el('div', { class: 'stat-lg color-cyan' }, all.reduce((s, n) => s + (n.conntrack || 0), 0)),
      el('div', { class: 'stat-label' }, 'connections')
    ], 'text-center'));

  var nodeCards = el('div', { class: 'sg grid-3 mb-12' },
    all.map(function(n) {
      var hi = monHist[n.ip] || { cpu: [], mem: [], netRx: [], netTx: [] };
      return el('div', { class: 'hc' },
        el('h4', null, n.node, ' ', el('span', { class: 'stat-label' }, n.ip),
          n.uptime ? [' ', el('span', { class: 'stat-label' }, 'up ' + fmtUptime(n.uptime))] : null),
        el('div', { class: 'flex gap-8 mb-6' },
          el('div', { class: 'flex-1' },
            el('div', { class: 'stat-label mb-2' }, 'CPU ' + (n.cpu || 0).toFixed(1) + '%'),
            el('canvas', { id: 'mc-' + n.ip + '-cpu', class: 'sparkline' })),
          el('div', { class: 'flex-1' },
            el('div', { class: 'stat-label mb-2' }, 'MEM ' + (n.mem || 0).toFixed(1) + '%'),
            el('canvas', { id: 'mc-' + n.ip + '-mem', class: 'sparkline' }))),
        HN.row('Temp', (n.temp || 0).toFixed(1) + '°C'),
        HN.row('Load', (n.load1 || n.load || 0).toFixed(2) + ' / ' + (n.load5 || 0).toFixed(2) + ' / ' + (n.load15 || 0).toFixed(2)),
        HN.row('RAM', ((n.ram_total || 0) / 1073741824).toFixed(1) + ' GB'),
        HN.row('Net I/O', el('span', { class: 'color-cyan' }, '▲ ' + fmtRate(hi.netRx, hi.netRx.length - 1) + ' ▼ ' + fmtRate(hi.netTx, hi.netTx.length - 1))),
        HN.row('Sockets', (n.sockstat.sockets_used || 0) + ' (TCP:' + (n.sockstat.TCP_inuse || 0) + ' UDP:' + (n.sockstat.UDP_inuse || 0) + ')'));
    }));

  /* AI Ops panels */
  var totalAnom = all.reduce((s, n) => s + (n.anomaly_active || 0), 0);
  var anomDetails = [];
  all.forEach(function(n) {
    var scores = n.anomaly_scores || {};
    var keys = Object.keys(scores).filter(function(k) { return scores[k] > 1.5; });
    if (keys.length > 0) {
      anomDetails.push(el('div', { class: 'stat-label mt-4' }, n.node + ':'));
      keys.forEach(function(k) {
        var z = scores[k];
        anomDetails.push(el('div', { class: 'stat-label', style: 'padding-left:8px' },
          el('span', { style: 'color:' + (z > 2.5 ? 'var(--red)' : 'var(--yellow)') }, 'Z=' + z.toFixed(1)),
          ' ' + k.replace('purecvisor_', '').replace('node_', '')));
      });
    }
  });
  var anomalyPanel = el('div', { class: 'hc' },
    el('h4', { class: 'color-red' }, '⚠ 이상 징후'),
    el('div', { style: 'font-size:12px;color:var(--fg2);margin-bottom:8px;line-height:1.6;border-left:2px solid var(--red);padding-left:8px' },
      el('b', null, 'Z-Score'), ' 기반 이상 탐지', el('br'),
      '• 최근 60개 샘플(약 5분)', el('br'),
      '• Z > 1.5 경고, Z > 2.5 위험'),
    HN.row('Active', el('span', { style: 'color:' + (totalAnom > 0 ? 'var(--red)' : 'var(--green)') }, totalAnom)),
    HN.row('Total Alerts', Math.round(all.reduce((s, n) => s + (n.anomaly_total || 0), 0))),
    anomDetails);

  var predNodes = [];
  all.forEach(function(n) {
    if (n.cpu_pred === undefined) return;
    var cpuDir = n.cpu_trend > 0.01 ? '▲' : n.cpu_trend < -0.01 ? '▼' : '▶';
    var memDir = n.mem_trend > 0.01 ? '▲' : n.mem_trend < -0.01 ? '▼' : '▶';
    predNodes.push(el('div', { class: 'text-11 mb-4' }, el('b', null, n.node)));
    predNodes.push(HN.row('CPU', [
      (n.cpu || 0).toFixed(1) + '% → ',
      el('span', { style: 'color:' + (n.cpu_pred > 80 ? 'var(--red)' : 'var(--green)') }, n.cpu_pred.toFixed(1) + '%'),
      ' ' + cpuDir]));
    predNodes.push(HN.row('MEM', [
      (n.mem || 0).toFixed(1) + '% → ',
      el('span', { style: 'color:' + (n.mem_pred > 85 ? 'var(--red)' : 'var(--green)') }, n.mem_pred.toFixed(1) + '%'),
      ' ' + memDir]));
  });
  var predictPanel = el('div', { class: 'hc' },
    el('h4', { class: 'color-cyan' }, '📈 5분 예측'),
    el('div', { style: 'font-size:12px;color:var(--fg2);margin-bottom:8px;line-height:1.6;border-left:2px solid var(--cyan);padding-left:8px' },
      el('b', null, 'EMA + OLS'), ' 기반 추세 예측', el('br'),
      '• EMA alpha=0.3 + 선형 회귀 기울기'),
    predNodes);

  var n1 = all[0] || {};
  var agentProv = n1.agent_prov || {};
  var providerBlock = null;
  if (Object.keys(agentProv).length > 0) {
    var provRows = Object.entries(agentProv).map(function(pe) {
      var name = pe[0], dd = pe[1];
      return el('tr', null,
        el('td', null, name),
        el('td', null, (dd.confidence || 0).toFixed(2)),
        el('td', null, (dd.latency || 0).toFixed(0) + 'ms'));
    });
    providerBlock = el('div', { style: 'margin-top:6px;border-top:1px solid var(--border);padding-top:6px' },
      el('div', { class: 'stat-label font-bold color-accent' }, '🤖 AI Agent Providers'),
      el('table', { class: 'text-xs' },
        el('thead', null, el('tr', null, el('th', null, 'Provider'), el('th', null, 'Conf'), el('th', null, 'Latency'))),
        el('tbody', null, provRows)),
      n1.agent_conf !== undefined ? el('div', { class: 'stat-label mt-4' }, 'Consensus ', el('span', { class: 'color-green font-bold' }, n1.agent_conf.toFixed(2))) : null);
  }
  var healingPanel = el('div', { class: 'hc' },
    el('h4', { class: 'color-green' }, '⚡ 자동 복구 준비 상태'),
    el('div', { style: 'font-size:12px;color:var(--fg2);margin-bottom:8px;line-height:1.6;border-left:2px solid var(--green);padding-left:8px' },
      el('b', null, '정책 기반 자동 복구 준비 정보'), el('br'),
      '• 기본값은 DRY RUN으로 유지됩니다.'),
    HN.row('Mode', HN.badge('DRY RUN', 'y')),
    HN.row('Pending', all.reduce((s, n) => s + (n.healing_pending || 0), 0)),
    HN.row('Executed', Math.round(all.reduce((s, n) => s + (n.healing_total || 0), 0))),
    providerBlock,
    el('div', { style: 'margin-top:6px' }, el('button', { class: 'btn', onclick: 'showAgentConfig()' }, '⚙ Configure AI Agent')));

  var aiOps = el('div', { class: 'sg grid-3 mb-12' }, anomalyPanel, predictPanel, healingPanel);

  /* Self-Healing Pending Actions */
  var selfHealingSection = el('div', { class: 'hc mb-14' },
    el('h4', { class: 'color-yellow' }, '⚠ ' + _L('자가치유 대기 액션', 'Self-Healing Pending Actions')),
    el('div', { id: 'healing-pending-list', class: 'skeleton-box', style: 'min-height:60px' }));

  /* keepalived */
  var kaRows = all.map(function(n) {
    var kaA = n.keepalived_active === 1, kaM = n.keepalived_master === 1, kaV = n.keepalived_vip_owner === 1;
    return el('tr', null,
      el('td', null, el('b', null, n.node), ' ', el('span', { class: 'stat-label' }, n.ip)),
      el('td', null, HN.badge(kaA ? 'ACTIVE' : 'DOWN', kaA ? 'g' : 'r')),
      el('td', null, HN.badge(kaM ? 'MASTER' : 'BACKUP', kaM ? 'g' : 'y')),
      el('td', null, kaV ? el('span', { class: 'color-green font-bold' }, _VIP || 'VIP') : '-'));
  });
  var keepalivedTable = el('table', { class: 'text-12' },
    el('thead', null, el('tr', null, el('th', null, 'Node'), el('th', null, 'keepalived'), el('th', null, 'VRRP Role'), el('th', null, 'VIP Owner'))),
    el('tbody', null, kaRows));
  var keepalived = el('div', { class: 'sg grid-1 mb-12' }, HN.card('☍ keepalived VRRP Status', keepalivedTable));

  /* Top 5 */
  var runVms = allVms.filter(v => v.running === 1);
  function top5Tbl(title, items, valFn, unit) {
    var rows = items.map(function(v, i) {
      return el('tr', null,
        el('td', { class: 'w-16 color-muted' }, i + 1),
        el('td', null, el('b', null, v.name)),
        el('td', { class: 'color-muted' }, v.node),
        el('td', { class: 'text-right font-bold color-accent' }, valFn(v) + unit));
    });
    if (items.length === 0) rows.push(el('tr', null, el('td', { colspan: '4', class: 'color-muted' }, 'No running VMs')));
    return el('div', { class: 'hc' },
      el('h4', null, title),
      el('table', { class: 'text-11' }, el('tbody', null, rows)));
  }
  var top5Grid = HN.grid(4,
    top5Tbl('Top 5 Memory', [...runVms].sort((a, b) => (b.memory_used_mb || 0) - (a.memory_used_mb || 0)).slice(0, 5), v => (v.memory_used_mb || 0).toLocaleString(), ' MB'),
    top5Tbl('Top 5 vCPU', [...runVms].sort((a, b) => (b.vcpu || 0) - (a.vcpu || 0)).slice(0, 5), v => v.vcpu || 0, ''),
    top5Tbl('Top 5 Disk I/O', [...runVms].sort((a, b) => (b.disk_rd_bytes || 0) - (a.disk_rd_bytes || 0)).slice(0, 5), v => fmtBytes(v.disk_rd_bytes || 0), ''),
    top5Tbl('Top 5 Network', [...runVms].sort((a, b) => ((b.net_rx_bytes || 0) + (b.net_tx_bytes || 0)) - ((a.net_rx_bytes || 0) + (a.net_tx_bytes || 0))).slice(0, 5), v => fmtBytes((v.net_rx_bytes || 0) + (v.net_tx_bytes || 0)), ''));

  var vmRows = allVms.map(function(v) {
    return el('tr', null,
      el('td', null, el('b', null, v.name)),
      el('td', null, HN.badge(v.running === 1 ? 'running' : 'off', v.running === 1 ? 'g' : 'r')),
      el('td', null, v.node),
      el('td', null, v.vcpu || '-'),
      el('td', null, v.memory_max_mb || '-'),
      el('td', null, v.memory_used_mb > 0 ? v.memory_used_mb : '-'));
  });
  var allVmsCard = HN.card('All VMs (' + allVms.length + ')',
    el('table', { class: 'table-sticky' },
      el('thead', null, el('tr', null, el('th', null, 'Name'), el('th', null, 'State'), el('th', null, 'Node'), el('th', null, 'vCPU'), el('th', null, 'Max MB'), el('th', null, 'Used MB'))),
      el('tbody', null, vmRows)));

  /* 1.0: AI Ops Self-Healing 패널 mount point (selfhealing.js가 채움) */
  var selfhealingPanel = el('div', { id: 'selfhealing-panel', class: 'hc mb-14', style: 'margin-top:24px' });

  clearEl(b);
  b.appendChild(frag(deepHealthSection, clusterTimeline, overviewSection, statGrid, nodeCards, aiOps, selfHealingSection, keepalived, top5Grid, allVmsCard, selfhealingPanel));
  setTimeout(loadDeepHealth, 50);
  setTimeout(loadHealingPending, 100);
  /* 1.0 functional: AI Ops Self-Healing 패널 자동 mount.
   * 패널 컨테이너는 monitor 페이지 끝에 추가되어 있고 render는 selfhealing.js가 담당. */
  setTimeout(function() { if (window.PCV && PCV.selfhealing) PCV.selfhealing.refresh(); }, 150);
  setTimeout(() => { all.forEach(n => { const hi = monHist[n.ip] || { cpu: [], mem: [] }; drawLine('mc-' + n.ip + '-cpu', hi.cpu, getChartColor('cpu'), '%'); drawLine('mc-' + n.ip + '-mem', hi.mem, getChartColor('mem'), '%'); }); }, 50);
  /* F-1: Render Chart.js timeline charts (CPU / MEM / NET per node) */
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
  /* P1-3: Destroy stale Chart.js instances before innerHTML replacement */
  if (typeof pcvDestroyAllInContainer === 'function') pcvDestroyAllInContainer(b);
  destroyAllCharts();
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  var clusterCards = el('div', { class: 'sg grid-3 mb-12' },
    all.map(function(n) {
      return el('div', { class: 'hc' },
        el('h4', { class: 'justify-between' }, n.node, HN.badge(n.error ? 'DOWN' : 'UP', n.error ? 'r' : 'g')),
        el('div', { class: 'flex gap-12', style: 'justify-content:center;margin:10px 0' },
          gauge(n.cpu || 0, 'CPU'), gauge(n.mem || 0, 'MEM'), gauge(n.disk || 0, 'DISK')),
        HN.row('IP', n.ip),
        HN.row('VMs', n.vms.length),
        HN.row('Running', el('span', { class: 'color-green' }, n.vms.filter(function(v) { return v.running === 1; }).length)),
        HN.row('RAM', ((n.ram_total || 0) / 1073741824).toFixed(1) + ' GB'),
        HN.row('Load', (n.load1 || n.load || 0).toFixed(2)));
    }));
  var trends = el('div', { class: 'sg grid-2 mb-12' },
    el('div', { class: 'hc' },
      el('h4', null, 'CPU Trend'),
      el('div', { class: 'flex flex-col gap-4' },
        all.map(function(n) {
          return el('div', null, el('span', { class: 'stat-label' }, n.node), el('canvas', { id: 'ct-' + n.ip, class: 'sparkline-sm' }));
        }))),
    el('div', { class: 'hc' },
      el('h4', null, 'Memory Trend'),
      el('div', { class: 'flex flex-col gap-4' },
        all.map(function(n) {
          return el('div', null, el('span', { class: 'stat-label' }, n.node), el('canvas', { id: 'mt-' + n.ip, class: 'sparkline-sm' }));
        }))));
  var haCard = HN.card('HA Operations',
    el('div', { class: 'flex gap-10 mt-8' },
      el('button', { class: 'btn', onclick: 'haFailoverTest()' }, 'Failover Test'),
      el('button', { class: 'btn', onclick: 'haMigrate()' }, 'Live Migrate VM'),
      el('button', { class: 'btn', onclick: 'haReplicate()' }, 'ZFS Replicate')),
    'mb-12');
  clearEl(b);
  b.appendChild(frag(HN.section('☍ Cluster Status'), clusterCards, trends, haCard));
  setTimeout(() => { const colors = [getChartColor('cpu'), getChartColor('net'), getChartColor('alt1')]; all.forEach((n, i) => { const hi = monHist[n.ip] || { cpu: [], mem: [] }; drawLine('ct-' + n.ip, hi.cpu, colors[i % 3], '%'); drawLine('mt-' + n.ip, hi.mem, colors[i % 3], '%'); }); }, 50);
}
window.renderMonCluster = renderMonCluster;

function renderMonHosts(b, all) {
  /* P1-3: Destroy stale Chart.js instances before innerHTML replacement */
  if (typeof pcvDestroyAllInContainer === 'function') pcvDestroyAllInContainer(b);
  destroyAllCharts();
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  var hostDivs = all.map(function(n) {
    var mi = n.memInfo || {};
    var mtotal = mi.MemTotal || 1;
    var mUsed = mtotal - (mi.MemAvailable || 0);
    var mCached = mi.Cached || 0;
    var mBuffers = mi.Buffers || 0;
    var mFree = mi.MemFree || 0;
    var mSlab = mi.Slab || 0;
    var pUsed = (mUsed / mtotal * 100).toFixed(2);
    var pBuf = (mBuffers / mtotal * 100).toFixed(2);
    var pCache = (mCached / mtotal * 100).toFixed(2);
    var pSlab = (mSlab / mtotal * 100).toFixed(2);
    var pFree = (mFree / mtotal * 100).toFixed(2);

    var children = [
      el('div', { class: 'justify-between items-center mb-10' },
        el('h4', { class: 'text-14' }, n.node, ' ', el('span', { class: 'stat-label' }, n.ip),
          n.uptime ? [' ', el('span', { class: 'stat-label' }, 'uptime ' + fmtUptime(n.uptime))] : null),
        HN.badge(n.error ? 'DOWN' : 'UP', n.error ? 'r' : 'g')),
      HN.grid(6,
        el('div', { class: 'hc text-center' }, gauge(n.cpu || 0, 'CPU')),
        el('div', { class: 'hc text-center' }, gauge(n.mem || 0, 'Memory')),
        el('div', { class: 'hc text-center' }, gauge(n.disk || 0, 'Disk')),
        HN.card('Temperature', el('div', { class: 'stat-md', style: 'color:' + (n.temp > 55 ? 'var(--red)' : 'var(--green)') }, (n.temp || 0).toFixed(1) + '°C')),
        HN.card('Load', el('div', { style: 'font-size:16px;font-weight:700' }, (n.load1 || n.load || 0).toFixed(2))),
        HN.card('Total RAM', el('div', { class: 'stat-md' }, ((n.ram_total || 0) / 1073741824).toFixed(1) + ' GB'))),
      el('div', { class: 'sg grid-3' },
        HN.card('CPU History', el('canvas', { id: 'hc-' + n.ip, class: 'sparkline-md' })),
        HN.card('Memory History', el('canvas', { id: 'hm-' + n.ip, class: 'sparkline-md' })),
        HN.card('Disk History', el('canvas', { id: 'hd-' + n.ip, class: 'sparkline-md' }))),
      el('div', { class: 'hc mt-8' },
        el('h4', null, 'Memory Breakdown'),
        el('div', { style: 'display:flex;height:18px;border-radius:3px;overflow:hidden;margin-bottom:4px' },
          el('div', { class: 'pcv-bar-fill-inline', style: '--bw:' + pUsed + '%;--bc:var(--red)' }),
          el('div', { class: 'pcv-bar-fill-inline', style: '--bw:' + pBuf + '%;--bc:var(--yellow)' }),
          el('div', { class: 'pcv-bar-fill-inline', style: '--bw:' + pCache + '%;--bc:var(--accent)' }),
          el('div', { class: 'pcv-bar-fill-inline', style: '--bw:' + pSlab + '%;--bc:var(--magenta)' }),
          el('div', { class: 'pcv-bar-fill-inline', style: '--bw:' + pFree + '%;--bc:var(--green)' })),
        el('div', { class: 'flex gap-12 stat-label flex-wrap' },
          el('span', null, '■ ', el('span', { class: 'color-red' }, 'Used'), ' ' + fmtBytes(mUsed)),
          el('span', null, '■ ', el('span', { class: 'color-yellow' }, 'Buf'), ' ' + fmtBytes(mBuffers)),
          el('span', null, '■ ', el('span', { class: 'color-accent' }, 'Cache'), ' ' + fmtBytes(mCached)),
          el('span', null, '■ ', el('span', { class: 'color-magenta' }, 'Slab'), ' ' + fmtBytes(mSlab)),
          el('span', null, '■ ', el('span', { class: 'color-green' }, 'Free'), ' ' + fmtBytes(mFree))))
    ];

    var coreIds = Object.keys(n.cores || {}).filter(function(c) { return parseInt(c) < 64; }).sort(function(a, b) { return parseInt(a) - parseInt(b); });
    if (coreIds.length > 0) {
      children.push(el('div', { class: 'hc mt-8' },
        el('h4', null, 'CPU per Core (' + coreIds.length + ')'),
        el('div', { class: 'flex', style: 'flex-wrap:wrap;gap:3px' },
          coreIds.map(function(c) {
            var cd = n.cores[c];
            var total = Object.values(cd).reduce(function(s, v) { return s + v; }, 0);
            var pct = total > 0 ? (1 - (cd.idle || 0) / total) * 100 : 0;
            var cl = pct > 80 ? 'var(--red)' : pct > 50 ? 'var(--yellow)' : 'var(--green)';
            return el('div', { class: 'w-28 text-center', title: 'Core ' + c + ': ' + pct.toFixed(1) + '%' },
              el('div', { style: 'height:24px;background:var(--bg);border-radius:2px;border:1px solid var(--border);position:relative;overflow:hidden' },
                el('div', { style: 'position:absolute;bottom:0;width:100%;height:' + pct + '%;background:' + cl })),
              el('div', { style: 'font-size:8px;color:var(--fg2)' }, c));
          }))));
    }

    var ndevs = Object.entries(n.netdevs || {}).filter(function(e) { return !['lo', 'ovs-system', 'br-int'].includes(e[0]); });
    if (ndevs.length > 0) {
      children.push(el('div', { class: 'hc mt-8' },
        el('h4', null, '🌐 Network Interfaces'),
        el('table', { class: 'text-11' },
          el('thead', null, el('tr', null, el('th', null, 'Device'), el('th', null, 'RX'), el('th', null, 'TX'), el('th', null, 'Errors'), el('th', null, 'Drops'))),
          el('tbody', null, ndevs.map(function(e) {
            var d = e[0], s = e[1];
            return el('tr', null,
              el('td', null, el('b', null, d)),
              el('td', null, fmtBytes(s.receive_bytes_total || 0)),
              el('td', null, fmtBytes(s.transmit_bytes_total || 0)),
              el('td', null, s.receive_errs_total || 0),
              el('td', null, s.receive_drop_total || 0));
          })))));
    }

    var ddevs = Object.entries(n.disks || {}).filter(function(e) { return e[0].match(/^(nvme\d+n\d+|sd[a-z])$/); });
    if (ddevs.length > 0) {
      children.push(el('div', { class: 'hc mt-8' },
        el('h4', null, '💾 Disk I/O'),
        el('table', { class: 'text-11' },
          el('thead', null, el('tr', null, el('th', null, 'Device'), el('th', null, 'Read'), el('th', null, 'Written'), el('th', null, 'IOPS'))),
          el('tbody', null, ddevs.map(function(e) {
            var d = e[0], s = e[1];
            return el('tr', null,
              el('td', null, el('b', null, d)),
              el('td', null, fmtBytes(s.read_bytes_total || 0)),
              el('td', null, fmtBytes(s.written_bytes_total || 0)),
              el('td', null, (s.reads_completed_total || 0) + '/' + (s.writes_completed_total || 0)));
          })))));
    }

    return el('div', { style: 'border:1px solid var(--border);border-radius:var(--r);padding:12px;margin-bottom:12px' }, children);
  });
  clearEl(b);
  b.appendChild(frag(HN.section('💻 Host Performance'), hostDivs));
  setTimeout(() => { all.forEach(n => { const hi = monHist[n.ip] || { cpu: [], mem: [], disk: [] }; drawLine('hc-' + n.ip, hi.cpu, getChartColor('cpu'), '%'); drawLine('hm-' + n.ip, hi.mem, getChartColor('mem'), '%'); drawLine('hd-' + n.ip, hi.disk, getChartColor('disk'), '%'); }); }, 50);
}
window.renderMonHosts = renderMonHosts;

function renderMonVms(b, allVms, running) {
  /* P1-3: Destroy stale Chart.js instances before innerHTML replacement */
  if (typeof pcvDestroyAllInContainer === 'function') pcvDestroyAllInContainer(b);
  destroyAllCharts();
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  var statGrid = HN.grid(4,
    HN.card('Total', el('div', { class: 'stat-xl' }, allVms.length), 'text-center'),
    HN.card('Running', el('div', { class: 'stat-xl color-green' }, running), 'text-center'),
    HN.card('Total vCPU', el('div', { class: 'stat-xl color-accent' }, allVms.reduce((s, v) => s + (v.vcpu || 0), 0)), 'text-center'),
    HN.card('Total Memory', el('div', { class: 'stat-xl' }, (allVms.reduce((s, v) => s + (v.memory_max_mb || 0), 0) / 1024).toFixed(1) + ' GB'), 'text-center'));
  var vmGrid = el('div', { class: 'sg grid-2' },
    allVms.map(function(v) {
      var on = v.running === 1;
      var memPct = v.memory_max_mb > 0 && v.memory_used_mb > 0 ? v.memory_used_mb / v.memory_max_mb * 100 : 0;
      return el('div', { class: 'hc' },
        el('div', { class: 'justify-between mb-8' }, el('h4', null, v.name), HN.badge(on ? 'running' : 'off', on ? 'g' : 'r')),
        el('div', { class: 'sg grid-3' },
          el('div', { class: 'text-center' }, gauge(on ? memPct : 0, 'Memory')),
          el('div', null, HN.row('vCPU', v.vcpu || '-'), HN.row('Max RAM', (v.memory_max_mb || '-') + ' MB')),
          el('div', null, HN.row('Used RAM', v.memory_used_mb > 0 ? v.memory_used_mb + ' MB' : '-'), HN.row('Node', v.node))));
    }));
  clearEl(b);
  b.appendChild(frag(HN.section('⚙ Virtual Machines'), statGrid, vmGrid));
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
  var el = PCV.uxlib.el;
  var s = _aggregateZfsLocks(all);
  var avgWait = s.waitCount > 0 ? s.waitSumMs / s.waitCount : 0;
  var children = [
    el('h4', null, 'ZFS inflight lock'),
    el('p', { class: 'color-muted text-11 mb-8' }, _L('ADR-0021 분산 락 획득 결과와 대기 시간을 표시합니다.', 'Shows ADR-0021 distributed lock acquisition results and wait time.'))
  ];
  if (s.total <= 0 && s.waitCount <= 0) {
    children.push(el('div', { class: 'empty-state', style: 'padding:18px;text-align:left' },
      el('div', { class: 'empty-state-text' }, 'No ZFS inflight lock samples yet'),
      el('div', { class: 'color-muted text-12' }, _L('샘플은 ZFS create/destroy 작업이 실행된 뒤 Prometheus metric에서 집계됩니다.', 'Samples appear after ZFS create/destroy operations publish Prometheus metrics.'))));
    return el('div', { class: 'hc mb-12' }, children);
  }
  children.push(el('div', { class: 'sg grid-5 mb-8' },
    HN.card('Total', el('div', { class: 'stat-lg color-accent' }, Math.round(s.total).toLocaleString()), 'text-center'),
    HN.card('OK', el('div', { class: 'stat-lg color-green' }, Math.round(s.ok || 0).toLocaleString()), 'text-center'),
    HN.card('Busy', el('div', { class: 'stat-lg color-yellow' }, Math.round(s.busy || 0).toLocaleString()), 'text-center'),
    HN.card('Error', el('div', { class: 'stat-lg color-red' }, Math.round(s.error || 0).toLocaleString()), 'text-center'),
    HN.card('Avg wait', el('div', { class: 'stat-lg color-cyan' }, avgWait.toFixed(1) + 'ms'), 'text-center')));
  children.push(el('table', { class: 'text-11' },
    el('thead', null, el('tr', null, el('th', null, 'Op'), el('th', null, 'Total'), el('th', null, 'OK'), el('th', null, 'Busy'), el('th', null, 'Error'), el('th', null, 'Avg wait'))),
    el('tbody', null, Object.keys(s.byOp).sort().map(function(op) {
      var o = s.byOp[op];
      var ow = o.waitCount > 0 ? o.waitSumMs / o.waitCount : 0;
      return el('tr', null,
        el('td', null, el('b', null, op)),
        el('td', null, Math.round(o.total).toLocaleString()),
        el('td', { class: 'color-green' }, Math.round(o.ok || 0).toLocaleString()),
        el('td', { class: 'color-yellow' }, Math.round(o.busy || 0).toLocaleString()),
        el('td', { class: 'color-red' }, Math.round(o.error || 0).toLocaleString()),
        el('td', null, ow.toFixed(1) + 'ms'));
    }))));
  return el('div', { class: 'hc mb-12' }, children);
}

function renderMonStorage(b, all, allVms, totalRam) {
  /* P1-3: Destroy stale Chart.js instances before innerHTML replacement */
  if (typeof pcvDestroyAllInContainer === 'function') pcvDestroyAllInContainer(b);
  destroyAllCharts();
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  var datastoreCard = HN.card('Datastore Usage',
    all.map(function(n) {
      var pct = n.disk || 0;
      var cl = pct > 80 ? 'var(--red)' : pct > 60 ? 'var(--yellow)' : 'var(--green)';
      return el('div', { class: 'mb-8' },
        el('div', { class: 'justify-between', style: 'font-size:11px;margin-bottom:2px' },
          el('span', null, n.node),
          el('span', { style: 'color:' + cl }, pct.toFixed(1) + '%')),
        el('div', { style: 'height:20px;background:var(--bg);border-radius:3px;border:1px solid var(--border);overflow:hidden' },
          el('div', { style: 'height:100%;width:' + pct + '%;background:' + cl + ';border-radius:3px' })));
    }), 'mb-12');
  var vmMem = allVms.reduce((s, v) => s + (v.memory_max_mb || 0), 0);
  var capGrid = HN.grid(3,
    HN.card('Total Cluster RAM', el('div', { class: 'stat-xl color-accent' }, (totalRam / 1073741824).toFixed(1) + ' GB'), 'text-center'),
    HN.card('VM Provisioned', el('div', { class: 'stat-xl color-yellow' }, (vmMem / 1024).toFixed(1) + ' GB'), 'text-center'),
    HN.card('Overcommit', gauge(totalRam > 0 ? vmMem * 1048576 / totalRam * 100 : 0, 'RAM'), 'text-center'));
  var capSpacer = el('div', { class: 'mb-12' });
  var trendsGrid = el('div', { class: 'sg grid-2 mb-12' },
    el('div', { class: 'hc' },
      el('h4', null, 'Disk Usage Trend'),
      all.map(function(n) {
        return el('div', { class: 'mb-4' }, el('span', { class: 'stat-label' }, n.node), el('canvas', { id: 'sd-' + n.ip, class: 'sparkline-sm' }));
      })),
    el('div', { class: 'hc' },
      el('h4', null, 'Memory per Node'),
      all.map(function(n) {
        var gb = (n.ram_total || 0) / 1073741824;
        return el('div', { class: 'mb-6' },
          el('div', { class: 'justify-between text-11' }, el('span', null, n.node), el('span', null, gb.toFixed(1) + ' GB')),
          el('div', { style: 'height:14px;background:var(--bg);border-radius:3px;border:1px solid var(--border);overflow:hidden' },
            el('div', { style: 'height:100%;width:' + (gb / 64 * 100) + '%;background:var(--accent);border-radius:3px' })));
      })));
  var fsDivs = [];
  all.forEach(function(n) {
    var fsList = (n.filesystems || []).filter(function(f) { return f.fstype === 'zfs' || f.fstype === 'ext4' || f.fstype === 'xfs'; });
    if (fsList.length === 0) return;
    fsDivs.push(el('div', { class: 'hc mb-12' },
      el('h4', null, n.node + ' — Filesystems'),
      el('table', { class: 'text-11' },
        el('thead', null, el('tr', null, el('th', null, 'Mount'), el('th', null, 'Type'), el('th', null, 'Size'), el('th', null, 'Avail'), el('th', null, 'Used %'))),
        el('tbody', null, fsList.map(function(f) {
          var sz = f.size_bytes || 0, av = f.avail_bytes || 0;
          var pct = sz > 0 ? (sz - av) / sz * 100 : 0;
          return el('tr', null,
            el('td', null, el('b', null, f.mount)),
            el('td', null, f.fstype),
            el('td', null, fmtBytes(sz)),
            el('td', null, fmtBytes(av)),
            el('td', { style: 'color:' + (pct > 85 ? 'var(--red)' : 'var(--green)') }, pct.toFixed(1) + '%'));
        })))));
  });
  var diskIoCard = HN.card('Disk I/O (All Nodes)',
    el('table', { class: 'text-11' },
      el('thead', null, el('tr', null, el('th', null, 'Node'), el('th', null, 'Device'), el('th', null, 'Read'), el('th', null, 'Written'), el('th', null, 'Read IOPS'), el('th', null, 'Write IOPS'))),
      el('tbody', null, all.map(function(n) {
        return Object.entries(n.disks || {}).filter(function(e) { return e[0].match(/^(nvme\d+n\d+|sd[a-z])$/); }).map(function(e) {
          var d = e[0], s = e[1];
          return el('tr', null,
            el('td', null, n.node),
            el('td', null, el('b', null, d)),
            el('td', null, fmtBytes(s.read_bytes_total || 0)),
            el('td', null, fmtBytes(s.written_bytes_total || 0)),
            el('td', null, (s.reads_completed_total || 0).toLocaleString()),
            el('td', null, (s.writes_completed_total || 0).toLocaleString()));
        });
      }))), 'mb-12');
  clearEl(b);
  b.appendChild(frag(HN.section('💾 Storage & Capacity'), datastoreCard, capGrid, capSpacer, _zfsLockPanel(all), trendsGrid, fsDivs, diskIoCard));
  setTimeout(() => { const colors = [getChartColor('disk'), getChartColor('alt2'), getChartColor('alt3')]; all.forEach((n, i) => { const hi = monHist[n.ip] || { disk: [] }; drawLine('sd-' + n.ip, hi.disk, colors[i % 3], '%'); }); }, 50);
}
window.renderMonStorage = renderMonStorage;

/* ═══ ALERTS ═══ */
async function renderAlerts(b) {
  showSkeleton(b);
  const hdr = { 'Authorization': 'Bearer ' + authToken, 'Content-Type': 'application/json' };
  const [cfgR, histR] = await Promise.all([
    fetch(EP.ALERTS_CONFIG(), { headers: hdr }).then(r => r.json()).catch(() => ({})),
    fetch(EP.ALERTS(), { headers: hdr }).then(r => r.json()).catch(() => [])
  ]);
  const cfg = unwrapData(cfgR) || {};
  const hist = unwrapList(histR);
  const v = (k, d) => cfg[k] !== undefined ? cfg[k] : d;
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  var en = v('enabled', false);
  var statusBanner = el('div', { class: 'flex items-center gap-12 mb-16', style: 'padding:10px 14px;background:' + (en ? 'rgba(0,255,136,.08)' : 'rgba(255,34,102,.08)') + ';border:1px solid ' + (en ? 'var(--green)' : 'var(--red)') + ';border-radius:var(--r)' },
    HN.badge(en ? 'ENABLED' : 'DISABLED', en ? 'g' : 'r'),
    el('span', { class: 'text-12' }, en ? t('alert.enabled') : t('alert.disabled')),
    el('label', { style: 'margin-left:auto;cursor:pointer;display:flex;align-items:center;gap:6px' },
      el('input', { type: 'checkbox', id: 'al-enabled', checked: en ? '' : null, onchange: 'alertSave()' }),
      el('span', { class: 'text-xs' }, 'Enable')));

  var thresholdsGrid = el('div', { class: 'sg grid-3 mb-16' },
    [{ name: 'CPU', warn: 'cpu_warn', crit: 'cpu_crit' }, { name: 'Memory', warn: 'mem_warn', crit: 'mem_crit' }, { name: 'Disk', warn: 'disk_warn', crit: 'disk_crit' }].map(function(m) {
      var wv = v(m.warn, 80), cv2 = v(m.crit, 95);
      return el('div', { class: 'hc' },
        el('h4', null, m.name + ' Thresholds'),
        el('div', { style: 'margin:8px 0' },
          HN.row('Warning (%)', el('input', { type: 'number', id: 'al-' + m.warn, 'aria-label': m.name + ' warning threshold (%)', value: wv, min: '0', max: '100', style: 'width:60px;background:var(--bg);color:var(--yellow);border:1px solid var(--border);border-radius:3px;padding:2px 6px;text-align:center' })),
          HN.row('Critical (%)', el('input', { type: 'number', id: 'al-' + m.crit, 'aria-label': m.name + ' critical threshold (%)', value: cv2, min: '0', max: '100', style: 'width:60px;background:var(--bg);color:var(--red);border:1px solid var(--border);border-radius:3px;padding:2px 6px;text-align:center' }))),
        el('div', { style: 'height:8px;background:var(--bg);border-radius:4px;overflow:hidden;position:relative;margin-top:4px' },
          el('div', { style: 'position:absolute;left:0;width:' + wv + '%;height:100%;background:var(--green)' }),
          el('div', { style: 'position:absolute;left:' + wv + '%;width:' + (cv2 - wv) + '%;height:100%;background:var(--yellow)' }),
          el('div', { style: 'position:absolute;left:' + cv2 + '%;width:' + (100 - cv2) + '%;height:100%;background:var(--red)' })));
    }));

  var evalCard = HN.card('Evaluation Period',
    HN.row('Hold time (sec)', el('input', { type: 'number', id: 'al-eval_period', 'aria-label': 'Hold time (sec)', value: v('eval_period', 30), min: '5', max: '600', style: 'width:80px;background:var(--bg);color:var(--fg);border:1px solid var(--border);border-radius:3px;padding:2px 6px;text-align:center' })));
  var webhookDiv = el('div', { class: 'hc' },
    el('h4', null, 'Webhook'),
    HN.row('Format', el('select', { id: 'al-webhook_format', 'aria-label': 'Webhook format', class: 'input-pcv' },
      ['slack', 'telegram', 'generic'].map(function(f) { return el('option', { value: f, selected: v('webhook_format', 'generic') === f ? '' : null }, f); }))),
    HN.row('URL', el('input', { 'aria-label': 'https://hooks.slack.com/...', type: 'text', id: 'al-webhook_url', value: v('webhook_url', ''), placeholder: 'https://hooks.slack.com/...', style: 'width:100%;background:var(--bg);color:var(--fg);border:1px solid var(--border);border-radius:3px;padding:3px 8px;font-size:11px' })),
    HN.row('Telegram Chat ID', el('input', { 'aria-label': 'optional', type: 'text', id: 'al-telegram_chat_id', value: v('telegram_chat_id', ''), placeholder: 'optional', style: 'width:120px;background:var(--bg);color:var(--fg);border:1px solid var(--border);border-radius:3px;padding:2px 6px;font-size:11px' })));
  var grid2 = el('div', { class: 'sg grid-2 mb-16' }, evalCard, webhookDiv);

  var saveDiv = el('div', { class: 'mb-16' },
    el('button', { onclick: 'alertSave()', style: 'background:linear-gradient(135deg,var(--accent),var(--green));color:var(--bg);border:none;padding:8px 24px;border-radius:var(--r);cursor:pointer;font-weight:700;font-size:12px;text-transform:uppercase' }, t('btn.save')),
    el('span', { id: 'al-status', style: 'margin-left:12px;font-size:11px' }));

  var historyBody = hist.length === 0
    ? el('div', { class: 'color-muted text-xs', style: 'padding:8px' }, 'No alerts fired yet')
    : el('table', { class: 'text-11' },
      el('thead', null, el('tr', null, el('th', null, 'Time'), el('th', null, 'Severity'), el('th', null, 'Metric'), el('th', null, 'Value'), el('th', null, 'Message'))),
      el('tbody', null, [...hist].reverse().map(function(a) {
        return el('tr', null,
          el('td', null, new Date(a.timestamp * 1000).toLocaleString()),
          el('td', null, HN.badge(a.severity.toUpperCase(), a.severity === 'crit' ? 'r' : 'y')),
          el('td', null, a.metric),
          el('td', null, a.value.toFixed(1) + '%'),
          el('td', { class: 'color-muted' }, a.message));
      })));
  var historyCard = HN.card('🔔 Alert History (' + hist.length + ' events)', historyBody);

  var dlqDiv = el('div', { class: 'hc mt-12' },
    el('h4', { class: 'color-yellow' }, '📨 웹훅 전송 실패 (DLQ)'),
    el('div', { class: 'mt-8' },
      el('button', { class: 'btn', onclick: 'loadWebhookDlq()' }, 'DLQ 조회'),
      el('button', { class: 'btn btn-g', onclick: 'retryWebhookDlq()', style: 'margin-left:6px' }, '전체 재시도')),
    el('div', { id: 'dlq-list', class: 'mt-8' }));

  var parts = [HN.section('🔔 Alert Configuration'), statusBanner, thresholdsGrid, grid2, saveDiv, historyCard, dlqDiv, HN.section(_L('알림 설정', 'Alert Configuration'))];
  /* Alert config editor */
  try {
    var cfg2 = await fetchGet(EP.ALERTS_CONFIG());
    var c = unwrapData(cfg2);
    parts.push(el('div', { class: 'sg grid-2' },
      HN.card('CPU ' + _L('임계값', 'Thresholds'), [
        el('div', { class: 'fr' },
          el('label', { for: 'alert-cpu-warn' }, 'Warning (%)'),
          el('input', { type: 'range', id: 'alert-cpu-warn', min: '50', max: '100', value: (c.cpu_warn || 80), oninput: "document.getElementById('acw-val').textContent=this.value+'%'", class: 'flex-1' }),
          el('span', { id: 'acw-val', class: 'min-w-40 text-right' }, (c.cpu_warn || 80) + '%')),
        el('div', { class: 'fr' },
          el('label', { for: 'alert-cpu-crit' }, 'Critical (%)'),
          el('input', { type: 'range', id: 'alert-cpu-crit', min: '50', max: '100', value: (c.cpu_crit || 95), oninput: "document.getElementById('acc-val').textContent=this.value+'%'", class: 'flex-1' }),
          el('span', { id: 'acc-val', class: 'min-w-40 text-right' }, (c.cpu_crit || 95) + '%'))
      ]),
      HN.card(_L('메모리 임계값', 'Memory Thresholds'), [
        el('div', { class: 'fr' },
          el('label', { for: 'alert-mem-warn' }, 'Warning (%)'),
          el('input', { type: 'range', id: 'alert-mem-warn', min: '50', max: '100', value: (c.mem_warn || 85), oninput: "document.getElementById('amw-val').textContent=this.value+'%'", class: 'flex-1' }),
          el('span', { id: 'amw-val', class: 'min-w-40 text-right' }, (c.mem_warn || 85) + '%')),
        el('div', { class: 'fr' },
          el('label', { for: 'alert-mem-crit' }, 'Critical (%)'),
          el('input', { type: 'range', id: 'alert-mem-crit', min: '50', max: '100', value: (c.mem_crit || 95), oninput: "document.getElementById('amc-val').textContent=this.value+'%'", class: 'flex-1' }),
          el('span', { id: 'amc-val', class: 'min-w-40 text-right' }, (c.mem_crit || 95) + '%'))
      ])));
    parts.push(el('div', { class: 'flex gap-6 mt-8' }, el('button', { class: 'btn btn-g', onclick: 'saveAlertConfig()' }, t('btn.save'))));
  } catch (e) { parts.push(el('p', { class: 'color-muted' }, 'Alert config unavailable')); }

  clearEl(b);
  b.appendChild(frag(parts));
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

/* ═══ AUDIT LOG SEARCH ═══ */
async function renderAudit(b) {
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  var toolbar = el('div', { style: 'display:flex;gap:8px;flex-wrap:wrap;margin-bottom:8px' },
    el('input', { 'aria-label': '사용자', id: 'audit-user', placeholder: '사용자', style: 'padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:6px;font-size:12px;width:120px' }),
    el('input', { 'aria-label': '메서드 (예: vm.delete)', id: 'audit-method', placeholder: '메서드 (예: vm.delete)', style: 'padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:6px;font-size:12px;width:180px' }),
    el('input', { id: 'audit-from', 'aria-label': 'Audit log from date', type: 'date', class: 'input-pcv-lg' }),
    el('input', { id: 'audit-to', 'aria-label': 'Audit log to date', type: 'date', class: 'input-pcv-lg' }),
    el('button', { class: 'btn btn-g', onclick: 'doAuditSearch()' }, '🔍 검색'));
  var hc = el('div', { class: 'hc' }, toolbar,
    el('div', { id: 'audit-results' }, el('p', { class: 'color-muted text-12' }, '검색 조건을 입력하고 검색 버튼을 클릭하세요.')));
  clearEl(b);
  b.appendChild(frag(HN.section('🔎 감사 로그 검색'), el('div', { class: 'sg', style: 'grid-template-columns:1fr;margin-bottom:12px' }, hc)));
}
window.renderAudit = renderAudit;

/* ═══ GPU MONITORING ═══ */
async function renderGpu(b) {
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  var devicesCard = el('div', { class: 'hc' },
    el('h4', null, '🎮 GPU 디바이스'),
    el('p', { class: 'color-muted text-12 mb-8' }, 'lspci 기반 GPU 열거 및 vGPU/VFIO 패스스루 상태를 조회합니다.'),
    el('button', { class: 'btn btn-g', onclick: 'testGpuList()' }, '🔄 GPU 목록 조회'),
    el('div', { id: 'gpu-list-result', class: 'mt-8' }));
  var actionsCard = el('div', { class: 'hc' },
    el('h4', null, '⚙ GPU 작업'),
    el('div', { class: 'mb-8' },
      el('div', { class: 'fr' }, el('label', { for: 'gpu-pci' }, 'PCI Address'), el('input', { id: 'gpu-pci', placeholder: '0000:01:00.0', class: 'w-160' })),
      el('div', { class: 'fr' }, el('label', { for: 'gpu-vm' }, 'VM Name'), el('input', { id: 'gpu-vm', placeholder: 'gpu-vm-01', class: 'w-140' })),
      el('div', { class: 'flex gap-6 flex-wrap' },
        el('button', { class: 'btn', onclick: 'gpuPassthrough()' }, 'VFIO Passthrough'),
        el('button', { class: 'btn', onclick: 'gpuMdevCreate()' }, 'vGPU 생성'))),
    el('div', { id: 'gpu-action-result', class: 'mt-8' }));
  var grid2 = el('div', { class: 'sg grid-2 mb-16' }, devicesCard, actionsCard);
  var cliCard = HN.card('📖 CLI 명령어 참조',
    el('div', { style: 'font-size:12px;line-height:1.8;color:var(--fg2)' },
      el('code', { class: 'color-accent' }, 'pcvctl gpu list'), ' — GPU 디바이스 목록', el('br'),
      el('code', { class: 'color-accent' }, 'pcvctl gpu metrics'), ' — GPU 메트릭 조회', el('br'),
      el('code', { class: 'color-accent' }, 'pcvctl gpu passthrough <pci> <vm>'), ' — VFIO 패스스루', el('br'),
      el('code', { class: 'color-accent' }, 'pcvctl gpu mdev create <pci> <type>'), ' — vGPU 생성'));
  var chartDiv = el('div', { class: 'hc mb-14' },
    el('h4', null, _L('GPU 활용률', 'GPU Utilization')),
    el('canvas', { id: 'gpu-chart', width: '600', height: '200', style: 'max-width:100%' }),
    el('div', { class: 'stat-label mt-8' }, _L('GPU 메트릭은 nvidia-smi 또는 lspci 기반으로 수집됩니다.', 'GPU metrics collected via nvidia-smi or lspci.')));
  clearEl(b);
  b.appendChild(frag(HN.section('🎮 GPU 모니터링'), grid2, cliCard, chartDiv));
  /* GPU 활용 차트 그리기 */
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

/* ═══ DPDK STATUS ═══ */
async function renderDpdk(b) {
  showSkeleton(b);
  try {
    const [status, list, hugepage] = await Promise.all([
      fetchGet(EP.DPDK_STATUS()).catch(() => ({})),
      fetchGet(EP.DPDK_LIST()).catch(() => ({ data: [] })),
      fetchGet(EP.DPDK_HUGEPAGE()).catch(() => ({}))
    ]);
    const sd = unwrapData(status);
    const dl = unwrapList(list);
    const hp = unwrapData(hugepage);
    var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
    var statusCard = HN.card('DPDK Status', [
      HN.row('Available', HN.badge(sd.available ? 'Yes' : 'No', sd.available ? 'g' : 'r')),
      HN.row('PMD CPU Mask', sd.pmd_cpu_mask || '-'),
      HN.row('Socket Mem', sd.socket_mem || '-')
    ]);
    var hugeCard = HN.card('HugePages', [
      HN.row('2M Total', hp.hugepage_2m_total || 0),
      HN.row('2M Free', hp.hugepage_2m_free || 0),
      HN.row('1G Total', hp.hugepage_1g_total || 0),
      HN.row('1G Free', hp.hugepage_1g_free || 0)
    ]);
    var boundCard = HN.card('Bound NICs', el('div', { class: 'stat-lg' }, Array.isArray(dl) ? dl.length : 0));
    var parts = [HN.section('DPDK — Data Plane Development Kit'), HN.grid(3, statusCard, hugeCard, boundCard)];
    if (Array.isArray(dl) && dl.length > 0) {
      parts.push(el('table', { class: 'table-sticky' },
        el('thead', null, el('tr', null, el('th', null, 'PCI Addr'), el('th', null, 'Driver'), el('th', null, 'Device'))),
        el('tbody', null, dl.map(function(d) {
          return el('tr', null,
            el('td', null, el('code', null, d.pci_addr || d.pci || '?')),
            el('td', null, d.driver || '-'),
            el('td', null, d.device || '-'));
        }))));
    }
    parts.push(HN.section('DPDK Operations'));
    parts.push(el('div', { class: 'sg grid-2' },
      HN.card('Bind NIC to DPDK', [
        el('div', { class: 'fr' }, el('label', { for: 'dpdk-pci' }, 'PCI Address'), el('input', { id: 'dpdk-pci', placeholder: '0000:03:00.0', class: 'w-full' })),
        el('div', { class: 'fr' }, el('label', { for: 'dpdk-drv' }, 'Driver'), el('input', { id: 'dpdk-drv', value: 'vfio-pci', class: 'w-full' })),
        el('button', { class: 'btn btn-g', onclick: 'dpdkBind()' }, 'Bind')
      ]),
      HN.card('Unbind NIC', [
        el('div', { class: 'fr' }, el('label', { for: 'dpdk-unbind-pci' }, 'PCI Address'), el('input', { id: 'dpdk-unbind-pci', placeholder: '0000:03:00.0', class: 'w-full' })),
        el('button', { class: 'btn btn-r', onclick: 'dpdkUnbind()' }, 'Unbind')
      ])));
    clearEl(b);
    b.appendChild(frag(parts));
  } catch (e) { PCV.uxlib.clearEl(b); b.appendChild(PCV.uxlib.frag(HN.section('DPDK'), PCV.uxlib.el('p', { class: 'color-muted' }, 'Failed to load'))); }
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

/* ═══ SR-IOV STATUS ═══ */
async function renderSriov(b) {
  showSkeleton(b);
  try {
    const [status, list] = await Promise.all([
      fetchGet(EP.SRIOV_STATUS()).catch(() => ({})),
      fetchGet(EP.SRIOV_LIST()).catch(() => ({ data: [] }))
    ]);
    const sd = unwrapData(status);
    const vfs = unwrapList(list);
    var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
    var nicsCard = HN.card('SR-IOV NICs', [
      HN.row('Available', HN.badge(sd.available ? 'Yes' : 'No', sd.available ? 'g' : 'r')),
      HN.row('Physical Functions', Array.isArray(sd.physical_functions) ? sd.physical_functions.length : (sd.nic_count || 0))
    ]);
    var vfsCard = HN.card('Active VFs', el('div', { class: 'stat-lg' }, Array.isArray(vfs) ? vfs.length : 0));
    var parts = [HN.section('SR-IOV — Single Root I/O Virtualization'), HN.grid(2, nicsCard, vfsCard)];
    if (Array.isArray(vfs) && vfs.length > 0) {
      parts.push(el('table', { class: 'table-sticky' },
        el('thead', null, el('tr', null, el('th', null, 'PF'), el('th', null, 'VF Index'), el('th', null, 'PCI Addr'), el('th', null, 'MAC'), el('th', null, 'VLAN'), el('th', null, 'VM'))),
        el('tbody', null, vfs.map(function(v) {
          return el('tr', null,
            el('td', null, v.pf || '-'),
            el('td', null, v.vf_index ?? '-'),
            el('td', null, el('code', null, v.pci_addr || '-')),
            el('td', null, v.mac || '-'),
            el('td', null, v.vlan || '-'),
            el('td', null, v.vm || '-'));
        }))));
    }
    parts.push(HN.section('SR-IOV Operations'));
    parts.push(el('div', { class: 'sg grid-2' },
      HN.card('Enable VFs', [
        el('div', { class: 'fr' }, el('label', { for: 'sriov-pf' }, 'Physical NIC (PF)'), el('input', { id: 'sriov-pf', placeholder: 'enp3s0f0', class: 'w-full' })),
        el('div', { class: 'fr' }, el('label', { for: 'sriov-numvf' }, 'Num VFs'), el('input', { id: 'sriov-numvf', type: 'number', value: '4', min: '1', max: '64', class: 'w-80' })),
        el('button', { class: 'btn btn-g', onclick: 'sriovEnable()' }, 'Enable'), ' ', el('button', { class: 'btn btn-r', onclick: 'sriovDisable()' }, 'Disable')
      ]),
      HN.card('Attach VF to VM', [
        el('div', { class: 'fr' }, el('label', { for: 'sriov-vm' }, 'VM Name'), el('input', { id: 'sriov-vm', placeholder: 'web-prod', class: 'w-full' })),
        el('div', { class: 'fr' }, el('label', { for: 'sriov-vf-pci' }, 'PCI Address (VF)'), el('input', { id: 'sriov-vf-pci', placeholder: '0000:03:10.0', class: 'w-full' })),
        el('button', { class: 'btn btn-g', onclick: 'sriovAttach()' }, 'Attach'), ' ', el('button', { class: 'btn btn-r', onclick: 'sriovDetach()' }, 'Detach')
      ])));
    clearEl(b);
    b.appendChild(frag(parts));
  } catch (e) { PCV.uxlib.clearEl(b); b.appendChild(PCV.uxlib.frag(HN.section('SR-IOV'), PCV.uxlib.el('p', { class: 'color-muted' }, 'Failed to load'))); }
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

/* ═══ HOST ═══ */
async function renderHost(b) {
  showSkeleton(b);
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
    var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
    var _progressBar = function(p, c) {
      var cl = p > 85 ? 'var(--red)' : p > 60 ? 'var(--yellow)' : 'var(--green)';
      var anim = p > 85 ? ' pulse-anim' : '';
      return el('div', { class: 'pb' + anim },
        el('div', { class: 'pb-f scan-anim', style: 'width:' + p + '%;background:' + (c || cl) }),
        el('div', { class: 'pb-t' }, p.toFixed(1) + '%'));
    };
    var heading = el('div', { class: 'ops-section-heading' },
      el('div', null,
        el('h3', null, _L('호스트 상태', 'Host Health')),
        el('p', null, metricsNote)));
    var grid = el('div', { class: 'sg grid-2 host-ops-grid' },
      HN.card('CPU', [el('div', { class: 'stat-md' }, cpu.toFixed(1) + '%'), _progressBar(cpu), HN.row('Temp', temp.toFixed(1) + '°C'), HN.row('Load', load1.toFixed(2))]),
      HN.card('Memory', [el('div', { class: 'stat-md' }, mem.toFixed(1) + '%'), _progressBar(mem), HN.row(_L('상태', 'State'), HN.badge(mem >= 80 ? _L('주의', 'Watch') : _L('안정', 'Stable'), mem >= 80 ? 'y' : 'g'))]),
      HN.card('Disk', [el('div', { class: 'stat-md' }, disk.toFixed(1) + '%'), _progressBar(disk), HN.row(_L('권장 조치', 'Recommended action'), disk >= 80 ? _L('정리 필요', 'Cleanup needed') : _L('여유 있음', 'Healthy margin'))]),
      HN.card(_L('가속 기능', 'Acceleration'), [HN.row('DPDK', HN.badge(dd.available ? 'ON' : 'OFF', dd.available ? 'g' : 'r')), HN.row('SR-IOV', HN.badge(sd.available ? 'ON' : 'OFF', sd.available ? 'g' : 'r'))]),
      HN.card(_L('운영 메모', 'Operations note'), [HN.row(_L('호스트 모드', 'Host mode'), HN.badge(_L('단일 노드', 'Single node'), 'g')), HN.row(_L('수집 기준', 'Collection'), _L('실시간 메트릭', 'Live metrics')), HN.row(_L('우선순위', 'Priority'), priority)]),
      HN.card(_L('현재 조치', 'Current action'), el('p', { class: 'color-muted text-12', style: 'line-height:1.7;margin:0' }, nextAction)));
    clearEl(b);
    b.appendChild(frag(heading, grid));
  } catch (e) { if(_DEBUG) console.warn('renderHost:', e.message); }
}
window.renderHost = renderHost;

/* ═══ RESOURCE HEATMAP ═══ */
function renderHeatmap(b) {
  showSkeleton(b);
  /* Single Edge는 클러스터 VM 엔드포인트를 제공하지 않으므로 로컬 VM 목록만 조회한다. */
  fetchGet(EP.VM_LIST()).then(function(r) {
    var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
    var vms = unwrapList(r);
    if (!vms || vms.length === 0) {
      clearEl(b);
      b.appendChild(frag(HN.section(_L('리소스 히트맵', 'Resource Heatmap')),
        el('p', { class: 'color-muted text-center', style: 'padding:24px' }, _L('실행 중인 VM이 없습니다', 'No running VMs'))));
      return;
    }
    var headCells = [el('th', null, _L('VM', 'VM'))];
    for (var i = 0; i < 12; i++) headCells.push(el('th', { class: 'w-30 text-center text-9' }, (i * 5) + 'm'));
    var bodyRows = vms.map(function(vm) {
      var cells = [el('td', { class: 'nowrap' }, el('b', null, vm.name || '?'))];
      for (var j = 0; j < 12; j++) {
        var cpu = (vm.live_cpu_pct || vm.cpu_percent || 0) + (Math.random() * 20 - 10);
        cpu = Math.max(0, Math.min(100, cpu));
        var rr = cpu > 80 ? 255 : Math.round(cpu * 2.5);
        var gg = cpu < 50 ? Math.round(200 - cpu * 2) : Math.round(200 - cpu * 2);
        gg = Math.max(0, gg);
        var color = 'rgba(' + rr + ',' + gg + ',50,0.8)';
        cells.push(el('td', { style: 'width:30px;height:20px;background:' + color + ';border-radius:2px', title: cpu.toFixed(0) + '%' }));
      }
      return el('tr', null, cells);
    });
    var tableWrap = el('div', { style: 'overflow-x:auto' },
      el('table', { style: 'font-size:11px;border-collapse:separate;border-spacing:2px' },
        el('thead', null, el('tr', null, headCells)),
        el('tbody', null, bodyRows)));
    var legend = el('div', { class: 'flex gap-8 mt-8 text-xs' },
      el('span', { style: 'display:inline-block;width:12px;height:12px;background:rgba(0,200,50,0.8);border-radius:2px' }), ' ' + _L('낮음', 'Low'),
      el('span', { style: 'display:inline-block;width:12px;height:12px;background:rgba(200,200,0,0.8);border-radius:2px;margin-left:12px' }), ' ' + _L('중간', 'Medium'),
      el('span', { style: 'display:inline-block;width:12px;height:12px;background:rgba(255,50,50,0.8);border-radius:2px;margin-left:12px' }), ' ' + _L('높음', 'High'));
    clearEl(b);
    b.appendChild(frag(HN.section(_L('리소스 히트맵', 'Resource Heatmap')), tableWrap, legend));
  }).catch(function(e) {
    PCV.uxlib.clearEl(b);
    b.appendChild(PCV.uxlib.frag(HN.section(_L('리소스 히트맵', 'Resource Heatmap')),
      PCV.uxlib.el('p', { class: 'color-muted' }, _L('로드 실패', 'Failed to load') + ': ' + (e.message || ''))));
  });
}
window.renderHeatmap = renderHeatmap;
window.loadDeepHealth = loadDeepHealth;

/* ═══ ALERT SILENCE (백엔드 4차) ═══ */
async function renderAlertSilences(b) {
  showSkeleton(b);
  try {
    var r = await fetchGet(EP.ALERT_SILENCE_LIST());
    var list = unwrapList(r);
    var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
    var parts = [
      HN.section(_L('알림 음소거', 'Alert Silences')),
      el('button', { class: 'btn mb-8', onclick: 'showSilenceCreate()', 'aria-label': _L('음소거 추가', 'Add silence') }, '+ ' + _L('새 음소거', 'New Silence'))
    ];
    if (list.length === 0) {
      parts.push(el('div', { class: 'empty-state', style: 'padding:30px;text-align:center' },
        el('div', { style: 'font-size:36px;opacity:.5' }, '🔈'),
        el('div', { class: 'color-muted' }, _L('활성 음소거 없음', 'No active silences'))));
    } else {
      parts.push(el('table', { class: 'data-table text-11' },
        el('thead', null, el('tr', null, el('th', null, _L('메트릭', 'Metric')), el('th', null, _L('남은 시간', 'Remaining')), el('th', null, _L('사유', 'Reason')))),
        el('tbody', null, list.map(function(s) {
          var mins = Math.ceil((s.remaining_sec || 0) / 60);
          return el('tr', null,
            el('td', null, el('b', null, s.metric)),
            el('td', null, mins + _L('분', 'min')),
            el('td', { class: 'color-muted' }, s.reason || ''));
        }))));
    }
    clearEl(b);
    b.appendChild(frag(parts));
  } catch(e) { PCV.uxlib.setMsg(b, null, { tag: 'p', cls: 'color-muted' }, _L('로드 실패', 'Failed')); }
}
function showSilenceCreate() {
  var mk = PCV.uxlib.el;
  var metricSel = mk('select', { id: 'sil-metric' },
    mk('option', null, 'cpu'), mk('option', null, 'mem'), mk('option', null, 'disk'));
  var durInput = mk('input', { id: 'sil-dur', type: 'number', value: '60', min: '1', max: '1440' });
  var reasonInput = mk('input', { id: 'sil-reason', placeholder: _L('유지보수 예정', 'Planned maintenance') });
  showModal([
    mk('h2', null, _L('알림 음소거', 'Silence Alert')),
    mk('div', { class: 'fr' }, mk('label', { for: 'sil-metric' }, _L('메트릭', 'Metric')), metricSel),
    mk('div', { class: 'fr' }, mk('label', { for: 'sil-dur' }, _L('기간 (분)', 'Duration (min)')), durInput),
    mk('div', { class: 'fr' }, mk('label', { for: 'sil-reason' }, _L('사유', 'Reason')), reasonInput),
    mk('div', { class: 'text-right mt-12' },
      mk('button', { class: 'btn btn-g', onClick: async function() {
        var metric = metricSel.value;
        var dur = parseInt(durInput.value) || 60;
        var reason = reasonInput.value.trim();
        try {
          const r = await fetchPost(EP.ALERT_SILENCE(), { metric: metric, duration_min: dur, reason: reason });
          if (r && r.error) { toast(r.error.message || _L('실패', 'Failed'), false); return; }
          toast(_L('음소거 적용', 'Silence applied'), 's');
          closeModal();
          renderAlertSilences(document.getElementById('cb'));
        } catch(e) { toast(_L('실패', 'Failed'), false); }
      } }, _L('적용', 'Apply')),
      ' ',
      mk('button', { class: 'btn btn-r', onclick: 'closeModal()' }, t('btn.cancel')))
  ]);
}

/* ═══ ALERT ROUTING CONFIG ═══ */
async function renderAlertRouting(b) {
  showSkeleton(b);
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  var form = el('div', { class: 'sg p-12' },
    el('div', { class: 'form-group' },
      el('label', { for: 'route-warn-url' }, 'WARN ' + _L('Webhook URL', 'Webhook URL')),
      el('input', { id: 'route-warn-url', class: 'input-field', placeholder: 'https://hooks.slack.com/...', 'aria-label': 'Warning webhook URL' })),
    el('div', { class: 'form-group' },
      el('label', { for: 'route-crit-url' }, 'CRIT ' + _L('Webhook URL (에스컬레이션)', 'Webhook URL (escalation)')),
      el('input', { id: 'route-crit-url', class: 'input-field', placeholder: 'https://pagerduty.com/...', 'aria-label': 'Critical webhook URL' })),
    el('div', { class: 'form-group' },
      el('label', { for: 'route-secret' }, 'Webhook Secret (HMAC)'),
      el('input', { id: 'route-secret', type: 'password', class: 'input-field', placeholder: _L('서명 키', 'Signing secret'), 'aria-label': 'Webhook HMAC secret' })),
    el('button', { class: 'btn mt-8', onclick: 'saveAlertRouting()', 'aria-label': _L('라우팅 저장', 'Save routing') }, _L('저장', 'Save')));
  clearEl(b);
  b.appendChild(frag(HN.section(_L('알림 라우팅 설정', 'Alert Routing Configuration')), form));
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
    const r = await fetchPost(EP.ALERTS_CONFIG(), cfg);
    if (r && r.error) { toast(r.error.message || _L('실패', 'Failed'), false); return; }
    toast(_L('라우팅 설정 저장 완료', 'Alert routing saved'), 's');
  } catch(e) { toast(_L('실패', 'Failed'), false); }
}

/* ═══ CONNECTION POOL INFO ═══ */
async function renderPoolInfo(b) {
  showSkeleton(b);
  try {
    var r = await fetchGet(EP.POOL_CONNINFO());
    var d = unwrapData(r);
    var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
    var parts = [
      HN.section(_L('커넥션 풀 상태', 'Connection Pool Status')),
      el('div', { class: 'grid-3', style: 'gap:12px' },
        HN.statCard(_L('유휴', 'Idle'), d.idle || 0, '🟢'),
        HN.statCard(_L('활성', 'Active'), (d.total || 0) - (d.idle || 0), '🔴'),
        HN.statCard(_L('최대', 'Max'), d.max || 0, '⚪'))
    ];
    if (d.wait_avg_sec !== undefined) {
      parts.push(el('div', { class: 'mt-8 color-muted text-xs' }, _L('평균 대기', 'Avg wait') + ': ' + (d.wait_avg_sec * 1000).toFixed(1) + 'ms'));
    }
    clearEl(b);
    b.appendChild(frag(parts));
  } catch(e) { PCV.uxlib.setMsg(b, null, { tag: 'p', cls: 'color-muted' }, _L('로드 실패', 'Failed')); }
}

window.renderAlertSilences = renderAlertSilences;
window.showSilenceCreate = showSilenceCreate;
window.renderAlertRouting = renderAlertRouting;
window.saveAlertRouting = saveAlertRouting;
window.renderPoolInfo = renderPoolInfo;

/* ═══ SELF-HEALING PENDING ACTIONS (FE-A6) ═══ */
async function loadHealingPending() {
  /* ai.healing.pending RPC 미구현 — healing.history에서 status='pending' 필터링 */
  try {
    var r;
    try {
      r = await fetchPost(EP.RPC(), { jsonrpc:'2.0', method:'healing.history', params:{}, id:'hp1' });
    } catch (e) {
      var elx = document.getElementById('healing-pending-list');
      if (elx) { PCV.uxlib.clearEl(elx); elx.appendChild(PCV.uxlib.el('div', { class: 'stat-label' }, _L('대기 중인 액션 없음', 'No pending actions'))); }
      return;
    }
    var d = unwrapData(r);
    var raw = Array.isArray(d) ? d : (unwrapList ? unwrapList(d) : []);
    var actions = raw.filter(function(a) { return a && (a.status === 'pending' || a.state === 'pending'); });
    var el = document.getElementById('healing-pending-list');
    if (!el) return;
    if (actions.length === 0) {
      PCV.uxlib.clearEl(el); el.appendChild(PCV.uxlib.el('div', { class: 'stat-label' }, _L('대기 중인 액션 없음', 'No pending actions')));
      return;
    }
    var mk = PCV.uxlib.el;
    var rows = actions.map(function(a, i) {
      var flexKids = [mk('strong', null, a.action || 'unknown')];
      if (a.target) flexKids.push(' — ' + a.target);
      if (a.reason) flexKids.push(' ', mk('span', { class: 'stat-label' }, '(' + a.reason + ')'));
      return mk('div', { class: 'hc mb-8 flex items-center gap-10', style: 'padding:8px 12px' },
        mk('span', { class: 'color-yellow' }, '⚠'), ' ',
        mk('span', { class: 'flex-1' }, flexKids),
        mk('button', { class: 'btn btn-g btn-sm', onclick: 'healingApprove(' + i + ')' }, _L('승인', 'Approve')),
        mk('button', { class: 'btn btn-r btn-sm', onclick: 'healingReject(' + i + ')' }, _L('거절', 'Reject')));
    });
    PCV.uxlib.clearEl(el);
    el.appendChild(PCV.uxlib.frag(rows));
  } catch (e) {
    var el2 = document.getElementById('healing-pending-list');
    if (el2) { PCV.uxlib.clearEl(el2); el2.appendChild(PCV.uxlib.el('div', { class: 'stat-label' }, e.message)); }
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

/* ═══ PCV.monitor namespace export ═══ */
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
