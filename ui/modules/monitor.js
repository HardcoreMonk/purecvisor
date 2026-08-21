                                                                  
                                  
                                                 
                                                      
                                                                     
  
                           
                                                                        
                                                                              
                                                                                
                                                                          
                                                
   
  
                                                                 
  
                                                                   
                                               
                                                     
                                               
  
     
                                                               
                                                                       
                                
                                                 
  
            
                                                                     
                                                                        
                                                                  
                                         
                                                                                
                                                                            
                                                                             
  
                                
                                                        
                                                                     
                                                                
  
              
                                                       
                                                      
                                                  
                                                                
  
                
                                                            
                                                            
                                                                           
                                                    
                                                                  
  
                         
                                                                        
                                                                  
                                                              
                                   
  
                    
                                                               
                                                 
                                                          
                                                 
  
                           
                                                                 
                                                  
                                              
   
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
  x.fillStyle = 'rgba(255,255,255,0.5)'; x.font = '10px Inter'; x.fillText(data.at(-1)?.toFixed(1) + (unit || ''), 4, 12);
}
window.drawLine = drawLine;

                                                            
                                                        
                                                                        
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

                                                              
   
                                                                                
  
                                                    
                                              
  
                                                              
                                                                 
                                                    
                                                    
  
                                            
                                                                      
                                                             
                                             
                                                                      
                                                             
                                                            
                                      
                                                                
                                                      
                                                           
                          
                                                 
                                                
  
                                                         
                                                                       
                                                    
   
async function renderMonitoring(b, tab) {
                                                        
                                                                      
                               
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

                                        
  if (typeof startAdaptivePolling === 'function') {
    startAdaptivePolling('mon-refresh', function() {
      if (window.currentTab && window.currentTab.startsWith('mon-')) {
        var cb = PCV.ui.renderTarget();                                                  
        var _navGen = PCV.ui.navGen();
        if (cb) fetchAllMetrics().then(function(fresh) {
          var freshVms = fresh.flatMap(function(n) { return n.vms.map(function(v) { return Object.assign({}, v, { nodeIP: n.ip }); }); });
          var r = freshVms.filter(function(v) { return v.running === 1; }).length;
          var ac = fresh.reduce(function(s, n) { return s + (n.cpu || 0); }, 0) / Math.max(fresh.length, 1);
          var am = fresh.reduce(function(s, n) { return s + (n.mem || 0); }, 0) / Math.max(fresh.length, 1);
          var ad = fresh.reduce(function(s, n) { return s + (n.disk || 0); }, 0) / Math.max(fresh.length, 1);
          var tr = fresh.reduce(function(s, n) { return s + (n.ram_total || 0); }, 0);
          if (PCV.ui.navGen() === _navGen) {
            if (window.currentTab === 'mon-overview') renderMonOverview(cb, fresh, freshVms, r, ac, am, ad, tr);
            else if (window.currentTab === 'mon-hosts') renderMonHosts(cb, fresh);
            else if (window.currentTab === 'mon-vms') renderMonVms(cb, freshVms, r);
            else if (window.currentTab === 'mon-storage') renderMonStorage(cb, fresh, freshVms, tr);
          }
        }).catch(function() {                  });
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

                                                       
                                                                  
                                                          
                                                         
function _opsVmRows(sourceVms) {
  var el = PCV.uxlib.el;
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
    return [el('tr', null,
      el('td', { colspan: '6', class: 'color-muted text-12' },
        '수집된 VM 데이터가 없습니다. VM 모니터링 화면에서 연결 상태를 확인하십시오.'))];
  }
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
  var el = PCV.uxlib.el;
  if (raw.length === 0) {
    return [el('div', { class: 'color-muted text-12' },
      '최근 브라우저 이벤트가 없습니다. 서버 감사 기록은 감사 로그에서 확인하십시오.')];
  }
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
  var displayVms = sourceVms;
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

                                                       
                                                                  
                                               
                                                      
  var header = HN.pagehead({
    title: '운영 이벤트 센터',
    desc: 'VM, OVN, ZFS, 보안 이벤트를 한 화면에서 triage하고 즉시 조치하는 운영자용 화면입니다.',
    actions: [el('div', { class: 'ops-triage-tabs', role: 'tablist', 'aria-label': '시간 범위' },
      el('button', { class: 'ops-triage-tab is-active', type: 'button' }, 'LIVE'),
      el('button', { class: 'ops-triage-tab', type: 'button' }, '1H'),
      el('button', { class: 'ops-triage-tab', type: 'button' }, '24H'),
      el('button', { class: 'ops-triage-tab', type: 'button' }, 'AUDIT'))]
  });

  var section = el('section', { class: 'ops-triage-grid', 'aria-label': '운영 이벤트 센터' },
    _opsMetricCard('호스트 CPU', avgCpu.toFixed(1) + '%', '단일 노드 평균', avgCpu > 80 ? '위험' : '정상', avgCpu > 80 ? 'bad' : avgCpu > 60 ? 'warn' : 'ok'),
    _opsMetricCard('메모리', avgMem.toFixed(0) + '%', ramDetail, avgMem > 85 ? '위험' : '여유', avgMem > 85 ? 'bad' : avgMem > 70 ? 'warn' : 'ok'),
    _opsMetricCard('OVN 관리', '로컬', 'OVN 상태 화면에서 확인', '확인', 'info'),
    _opsMetricCard('실행 VM', running + '/' + totalVm, '현재 조회 결과', running > 0 ? '가동' : '확인', running > 0 ? 'ok' : 'warn'),
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
            el('p', { class: 'ops-event-title' }, 'OVN 구성 상태 확인'),
            el('div', { class: 'ops-event-sub' }, '논리 스위치, 라우터와 ACL 상태는 OVN 화면에서 확인')),
          el('div', { class: 'ops-event-time' }, '이동')))),
    el('article', { class: 'ops-triage-card ops-span-7' },
      cardHead('VM 및 서비스 상태', totalVm + ' assets'),
      el('div', { class: 'ops-triage-toolbar' },
        el('input', { class: 'ops-triage-field', type: 'search', placeholder: '이름 또는 IP', 'aria-label': '자산 검색' }),
        el('div', { class: 'ops-triage-actions' },
          el('button', { class: 'ops-triage-action', type: 'button', onclick: "renderOpsTriage(PCV.ui.renderTarget())" },
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
      cardHead('OVN 관리', 'local'),
      el('p', { class: 'color-muted text-12' },
        '현재 논리 스위치, 라우터와 ACL 상태는 OVN 관리 화면에서 조회합니다.'),
      el('button', { class: 'ops-triage-action', type: 'button', onclick: "navigateTo('ovn')" },
        _svgIcon('vendor/coolicons/coolicons.svg#ci-layers', 'ci-icon'), 'OVN 화면 열기')),
    el('article', { class: 'ops-triage-card ops-span-4' },
      cardHead('감사 추적', 'audit'),
      el('div', { class: 'ops-triage-list' }, _opsAuditRows())));

  clearEl(b);
  b.appendChild(frag(header, section));
}
window.renderOpsTriage = renderOpsTriage;

                                   
   
                                                       
  
                                                                    
                                                    
                                                
  
                                                                     
                                                   
                                                    
                                                 
                                                               
                                 
  
                                               
                                            
   
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
      HN.statusPill('idle', node),
      uptime > 0 ? mk('span', { class: 'stat-label' }, (t('monitor.uptime') || 'Uptime') + ': ' + fmtUptime(uptime)) : null);

    var subsysKeys = Object.keys(subsystems);
    if (subsysKeys.length === 0) {
                                                               
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
                                                        
                                                                      
                               
  if (typeof pcvDestroyAllInContainer === 'function') pcvDestroyAllInContainer(b);
  destroyAllCharts();
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
                                  
  var deepHealthSection = el('div', { class: 'hc mb-14' },
    el('h4', { role: 'heading', 'aria-level': '3' }, '🩹 ' + (t('monitor.system_health') || 'System Health')),
    el('p', { class: 'color-muted text-11 mb-8' }, t('monitor.health_desc') || 'Deep health probe of all subsystems. Updated on each page load.'),
    el('div', { id: 'deep-health' },
      el('span', { class: 'spinner' }),
      ' ' + (t('loading') || 'Loading...')));

                                                      
  var clusterTimeline = el('div', { class: 'hc mb-12' },
    el('h4', { role: 'heading', 'aria-level': '3' }, '📊 ' + (t('monitor.cluster_timeline') || '리소스 흐름 (최근 5분)')),
    el('div', { class: 'sg grid-3 gap-12 mon-overview-timeline' },
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
    HN.card('평균 CPU', [el('div', { class: 'stat-sm' }, avgCpu.toFixed(1) + '%'), HN.gauge({ value: +avgCpu.toFixed(1), warn: 80, crit: 95, inline: true })], 'text-center'),
    HN.card('평균 메모리', [el('div', { class: 'stat-sm' }, avgMem.toFixed(1) + '%'), HN.gauge({ value: +avgMem.toFixed(1), warn: 80, crit: 95, inline: true })], 'text-center'),
    HN.card('평균 디스크', [el('div', { class: 'stat-sm' }, avgDisk.toFixed(1) + '%'), HN.gauge({ value: +avgDisk.toFixed(1), warn: 80, crit: 90, inline: true })], 'text-center'),
    HN.card('스왑', [el('div', { class: 'stat-sm' }, (tSwapTotal > 0 ? tSwapUsed / tSwapTotal * 100 : 0).toFixed(1) + '%'), HN.gauge({ value: +((tSwapTotal > 0 ? tSwapUsed / tSwapTotal * 100 : 0).toFixed(1)), warn: 50, crit: 80, inline: true }), el('div', { class: 'stat-label' }, fmtBytes(tSwapUsed) + '/' + fmtBytes(tSwapTotal))], 'text-center'),
    HN.card('소켓', [
      el('div', { class: 'stat-lg color-cyan' }, all.reduce((s, n) => s + (n.conntrack || 0), 0)),
      el('div', { class: 'stat-label' }, 'connections')
    ], 'text-center'));
  statGrid.classList.add('mon-overview-stat-grid');

  var nodeCards = el('div', { class: 'sg grid-3 mb-12' },
    all.map(function(n) {
      var hi = monHist[n.ip] || { cpu: [], mem: [], netRx: [], netTx: [] };
      return el('div', { class: 'hc' },
        el('h4', { role: 'heading', 'aria-level': '3' }, n.node, ' ', el('span', { class: 'stat-label' }, n.ip),
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
    el('h4', { class: 'color-red', role: 'heading', 'aria-level': '3' }, '⚠ 이상 징후'),
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
    el('h4', { class: 'color-cyan', role: 'heading', 'aria-level': '3' }, '📈 5분 예측'),
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
    el('h4', { class: 'color-green', role: 'heading', 'aria-level': '3' }, '⚡ 자동 복구 준비 상태'),
    el('div', { style: 'font-size:12px;color:var(--fg2);margin-bottom:8px;line-height:1.6;border-left:2px solid var(--green);padding-left:8px' },
      el('b', null, '정책 기반 자동 복구 준비 정보'), el('br'),
      '• 기본값은 DRY RUN으로 유지됩니다.'),
    HN.row('Mode', HN.statusPill('warn', 'DRY RUN')),
    HN.row('Pending', all.reduce((s, n) => s + (n.healing_pending || 0), 0)),
    HN.row('Executed', Math.round(all.reduce((s, n) => s + (n.healing_total || 0), 0))),
    providerBlock,
    el('div', { style: 'margin-top:6px' }, el('button', { class: 'btn', onclick: 'showAgentConfig()', 'data-role': 'ADMIN' }, '⚙ Configure AI Agent')));

  var aiOps = el('div', { class: 'sg grid-3 mb-12' }, anomalyPanel, predictPanel, healingPanel);

                                    
  var selfHealingSection = el('div', { class: 'hc mb-14' },
    el('h4', { class: 'color-yellow', role: 'heading', 'aria-level': '3' }, '⚠ ' + _L('자가치유 대기 액션', 'Self-Healing Pending Actions')),
    el('div', { id: 'healing-pending-list', class: 'skeleton-box', style: 'min-height:60px' }));

                  
  var kaRows = all.map(function(n) {
    var kaA = n.keepalived_active === 1, kaM = n.keepalived_master === 1, kaV = n.keepalived_vip_owner === 1;
    return el('tr', null,
      el('td', null, el('b', null, n.node), ' ', el('span', { class: 'stat-label' }, n.ip)),
      el('td', null, HN.statusPill(kaA ? 'ok' : 'crit', kaA ? 'ACTIVE' : 'DOWN')),
      el('td', null, HN.statusPill(kaM ? 'ok' : 'warn', kaM ? 'MASTER' : 'BACKUP')),
      el('td', null, kaV ? el('span', { class: 'color-green font-bold' }, _VIP || 'VIP') : '-'));
  });
  var keepalivedTable = el('table', { class: 'text-12' },
    el('thead', null, el('tr', null, el('th', null, 'Node'), el('th', null, 'keepalived'), el('th', null, 'VRRP Role'), el('th', null, 'VIP Owner'))),
    el('tbody', null, kaRows));
  var keepalived = el('div', { class: 'sg grid-1 mb-12' }, HN.card('☍ keepalived VRRP Status', keepalivedTable));

             
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
      el('h4', { role: 'heading', 'aria-level': '3' }, title),
      el('table', { class: 'text-11' }, el('tbody', null, rows)));
  }
                                                       
                                                   
                                              
                                                          
  var top5Grid = HN.grid(4,
    top5Tbl('Top 5 Memory', runVms.toSorted((a, b) => (b.memory_used_mb || 0) - (a.memory_used_mb || 0)).slice(0, 5), v => (v.memory_used_mb || 0).toLocaleString(), ' MB'),
    top5Tbl('Top 5 vCPU', runVms.toSorted((a, b) => (b.vcpu || 0) - (a.vcpu || 0)).slice(0, 5), v => v.vcpu || 0, ''),
    top5Tbl('Top 5 Disk I/O', runVms.toSorted((a, b) => (b.disk_rd_bytes || 0) - (a.disk_rd_bytes || 0)).slice(0, 5), v => fmtBytes(v.disk_rd_bytes || 0), ''),
    top5Tbl('Top 5 Network', runVms.toSorted((a, b) => ((b.net_rx_bytes || 0) + (b.net_tx_bytes || 0)) - ((a.net_rx_bytes || 0) + (a.net_tx_bytes || 0))).slice(0, 5), v => fmtBytes((v.net_rx_bytes || 0) + (v.net_tx_bytes || 0)), ''));
  top5Grid.classList.add('mon-overview-top5');

                                                                    
                                                  
                                                        
                 
  var vmRows = allVms.map(function(v) {
    return el('tr', null,
      el('td', null, el('b', null, v.name)),
      el('td', null, HN.statusDot(v.running === 1 ? 'ok' : 'idle', v.running === 1 ? { glow: true } : null), ' ' + (v.running === 1 ? 'running' : 'off')),
      el('td', null, v.node),
      el('td', null, v.vcpu || '-'),
      el('td', null, v.memory_max_mb || '-'),
      el('td', null, v.memory_used_mb > 0 ? v.memory_used_mb : '-'));
  });
  var allVmsCard = HN.card('All VMs (' + allVms.length + ')',
    el('table', { class: 'table-sticky' },
      el('thead', null, el('tr', null, el('th', null, 'Name'), el('th', null, 'State'), el('th', null, 'Node'), el('th', null, 'vCPU'), el('th', null, 'Max MB'), el('th', null, 'Used MB'))),
      el('tbody', null, vmRows)));

                                                                    
  var selfhealingPanel = el('div', { id: 'selfhealing-panel', class: 'hc mb-14', style: 'margin-top:24px' });

                                                   
  var hostUp = all.filter(function (n) { return !n.error; }).length;
  var healingPending = all.reduce(function (s, n) { return s + (n.healing_pending || 0); }, 0);
  var rollupBar = HN.statusBar([
    { count: hostUp, label: '호스트 UP', status: hostUp === all.length ? 'ok' : 'crit', sub: '/ ' + all.length, key: 'hosts' },
    { count: running, label: 'VM 실행', status: allVms.length === 0 ? 'idle' : (running === allVms.length ? 'ok' : 'warn'), sub: '/ ' + allVms.length, key: 'vms' },
    { count: totalAnom, label: '이상 징후', status: totalAnom > 0 ? 'crit' : 'ok', sub: '활성', key: 'anomaly' },
    { count: healingPending, label: '복구 대기', status: healingPending > 0 ? 'warn' : 'ok', sub: '대기', key: 'healing' }
  ], { onNavigate: function (s) {
    if (s.key === 'hosts' || s.key === 'anomaly') navigateTo('mon-hosts');
    else if (s.key === 'vms') navigateTo('mon-vms');
    else if (s.key === 'healing') navigateTo('selfhealing');
  } });
  clearEl(b);
  b.appendChild(frag(HN.pagehead({ title: _L('모니터링 개요', 'Monitoring overview') }),
    rollupBar, deepHealthSection, clusterTimeline, overviewSection, statGrid, nodeCards, aiOps, selfHealingSection, keepalived, top5Grid, allVmsCard, selfhealingPanel));
                                                                 
                                                               
                          
  if (typeof applyRoleVisibility === 'function') {
    applyRoleVisibility(window.currentUser && window.currentUser.role);
  }
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
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  var clusterCards = el('div', { class: 'sg grid-3 mb-12' },
    all.map(function(n) {
      return el('div', { class: 'hc' },
        el('h4', { class: 'justify-between' }, n.node, HN.statusPill(n.error ? 'crit' : 'ok', n.error ? 'DOWN' : 'UP')),
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
        HN.statusPill(n.error ? 'crit' : 'ok', n.error ? 'DOWN' : 'UP')),
      HN.grid(6,
        el('div', { class: 'hc text-center' }, el('div', { class: 'stat-sm' }, (n.cpu || 0).toFixed(1) + '%'), HN.gauge({ value: +(n.cpu || 0).toFixed(1), warn: 80, crit: 95, inline: true }), el('div', { class: 'stat-label' }, 'CPU')),
        el('div', { class: 'hc text-center' }, el('div', { class: 'stat-sm' }, (n.mem || 0).toFixed(1) + '%'), HN.gauge({ value: +(n.mem || 0).toFixed(1), warn: 80, crit: 95, inline: true }), el('div', { class: 'stat-label' }, 'Memory')),
        el('div', { class: 'hc text-center' }, el('div', { class: 'stat-sm' }, (n.disk || 0).toFixed(1) + '%'), HN.gauge({ value: +(n.disk || 0).toFixed(1), warn: 80, crit: 90, inline: true }), el('div', { class: 'stat-label' }, 'Disk')),
        HN.card('Temperature', [el('div', { class: 'stat-md' }, (n.temp || 0).toFixed(1) + '°C'), HN.statusPill(n.temp >= 70 ? 'crit' : n.temp >= 55 ? 'warn' : 'ok', n.temp >= 70 ? 'HOT' : n.temp >= 55 ? 'WARM' : 'OK')]),
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
  b.appendChild(frag(HN.pagehead({ title: 'Host Performance' }), hostDivs));
  setTimeout(() => { all.forEach(n => { const hi = monHist[n.ip] || { cpu: [], mem: [], disk: [] }; drawLine('hc-' + n.ip, hi.cpu, getChartColor('cpu'), '%'); drawLine('hm-' + n.ip, hi.mem, getChartColor('mem'), '%'); drawLine('hd-' + n.ip, hi.disk, getChartColor('disk'), '%'); }); }, 50);
}
window.renderMonHosts = renderMonHosts;

                                                       
                                                                               
                                                               
var _monVmsData = null;
var _monVmsUnsub = null;
function _monVmsShown(allVms) {
  var fs = PCV.ui && PCV.ui.filterState;
  var sel = (fs && fs.current().vmstate) || [];
  if (!sel.length) return allVms;
  return allVms.filter(function (v) {
    return sel.indexOf(v.running === 1 ? 'running' : 'stopped') !== -1;
  });
}
function _onMonVmsFilterChange() {
  if (window.currentTab !== 'mon-vms') {
    if (_monVmsUnsub) { _monVmsUnsub(); _monVmsUnsub = null; }
    return;
  }
  if (_monVmsData) renderMonVms(PCV.ui.renderTarget(), _monVmsData.allVms, _monVmsData.running);
}

   
                                                 
  
                                                            
                                                                 
                                                 
                                          
  
                                                      
                                                             
                                                 
                                          
   
function renderMonVms(b, allVms, running) {
                                                        
                                                                      
                               
  if (typeof pcvDestroyAllInContainer === 'function') pcvDestroyAllInContainer(b);
  destroyAllCharts();
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  _monVmsData = { allVms: allVms, running: running };
                                                       
  if (!_monVmsUnsub && PCV.ui && PCV.ui.filterState && PCV.ui.filterState.subscribe) {
    _monVmsUnsub = PCV.ui.filterState.subscribe(_onMonVmsFilterChange);
  }
  var statGrid = HN.grid(4,
    HN.card('Total', el('div', { class: 'stat-xl' }, allVms.length), 'text-center'),
    HN.card('Running', el('div', { class: 'stat-xl color-green' }, running), 'text-center'),
    HN.card('Total vCPU', el('div', { class: 'stat-xl color-accent' }, allVms.reduce((s, v) => s + (v.vcpu || 0), 0)), 'text-center'),
    HN.card('Total Memory', el('div', { class: 'stat-xl' }, (allVms.reduce((s, v) => s + (v.memory_max_mb || 0), 0) / 1024).toFixed(1) + ' GB'), 'text-center'));
                                                                       
  var nStopped = allVms.length - running;
  var vmFilterBar = HN.filterBar([{ key: 'vmstate', options: [
    { value: 'running', label: 'Running', count: running, sw: 'ok' },
    { value: 'stopped', label: 'Stopped', count: nStopped, sw: 'idle' }
  ] }]);
  var shownVms = _monVmsShown(allVms);
  var vmGrid = el('div', { class: 'sg grid-2' },
    shownVms.map(function(v) {
      var on = v.running === 1;
      var memPct = v.memory_max_mb > 0 && v.memory_used_mb > 0 ? v.memory_used_mb / v.memory_max_mb * 100 : 0;
      var mp = on ? memPct : 0;
      return el('div', { class: 'hc' },
        el('div', { class: 'justify-between mb-8' }, el('h4', null, v.name), HN.statusPill(on ? 'ok' : 'idle', on ? 'RUNNING' : 'OFF')),
        el('div', { class: 'sg grid-3' },
          el('div', { class: 'text-center' },
            el('div', { class: 'stat-sm' }, mp.toFixed(1) + '%'),
            HN.gauge({ value: +mp.toFixed(1), warn: 80, crit: 95, inline: true }),
            el('div', { class: 'stat-label' }, 'Memory')),
          el('div', null, HN.row('vCPU', v.vcpu || '-'), HN.row('Max RAM', (v.memory_max_mb || '-') + ' MB')),
          el('div', null, HN.row('Used RAM', v.memory_used_mb > 0 ? v.memory_used_mb + ' MB' : '-'), HN.row('Node', v.node))));
    }));
  clearEl(b);
  b.appendChild(frag(HN.pagehead({ title: 'Virtual Machines' }), statGrid, vmFilterBar, vmGrid));
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
                                                        
                                                                      
                               
  if (typeof pcvDestroyAllInContainer === 'function') pcvDestroyAllInContainer(b);
  destroyAllCharts();
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  var datastoreCard = HN.card('Datastore Usage',
    all.map(function(n) {
      var pct = +(n.disk || 0).toFixed(1);
      return el('div', { class: 'mb-8' },
        el('div', { class: 'justify-between', style: 'font-size:11px;margin-bottom:2px' },
          el('span', null, n.node),
          el('span', null, pct.toFixed(1) + '%')),
        HN.gauge({ value: pct, warn: 80, crit: 90, inline: true }));
    }), 'mb-12');
  var vmMem = allVms.reduce((s, v) => s + (v.memory_max_mb || 0), 0);
  var capGrid = HN.grid(3,
    HN.card('Total Cluster RAM', el('div', { class: 'stat-xl color-accent' }, (totalRam / 1073741824).toFixed(1) + ' GB'), 'text-center'),
    HN.card('VM Provisioned', el('div', { class: 'stat-xl color-yellow' }, (vmMem / 1024).toFixed(1) + ' GB'), 'text-center'),
    HN.card('Overcommit', HN.gauge({ value: +(totalRam > 0 ? vmMem * 1048576 / totalRam * 100 : 0).toFixed(1), warn: 80, crit: 100, unit: '%', label: 'RAM' }), 'text-center'));
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
            el('td', null, HN.statusPill(pct >= 90 ? 'crit' : pct >= 80 ? 'warn' : 'ok', pct.toFixed(1) + '%')));
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
  b.appendChild(frag(HN.pagehead({ title: 'Storage & Capacity' }), datastoreCard, capGrid, capSpacer, _zfsLockPanel(all), trendsGrid, fsDivs, diskIoCard));
  setTimeout(() => { const colors = [getChartColor('disk'), getChartColor('alt2'), getChartColor('alt3')]; all.forEach((n, i) => { const hi = monHist[n.ip] || { disk: [] }; drawLine('sd-' + n.ip, hi.disk, colors[i % 3], '%'); }); }, 50);
}
window.renderMonStorage = renderMonStorage;

                 
  
                                      
                                                        
                                         
                                               
                                                 
  
                                                      
                                                            
                                                 
                                             
  
                                         
                                                      
                                                          
                                                       
                                                   
  
                                       
   
var _alertsState = {
  navGen: 0,
  root: null,
  configRoot: null,
  historyRoot: null,
  config: null,
  baseline: null,
  history: [],
  configError: null,
  historyError: null,
  historyRefreshError: null,
  configLoading: false,
  applying: false,
  historyLoading: false,
  refreshing: false,
  historyPage: 0,
  lastSuccessAt: null,
  historyFilterUnsub: null,
  historyRequestGen: 0,
  applyMessage: null,
  applyConflict: false,
                                                           
  ackAllBusy: false,
  ackAllProgress: null
};

var ALERT_EDITABLE_CONFIG_KEYS = [
  'enabled',
  'cpu_warn', 'cpu_crit',
  'mem_warn', 'mem_crit',
  'disk_warn', 'disk_crit',
  'eval_period',
  'webhook_url', 'webhook_format', 'telegram_chat_id'
];

function _cloneAlertConfig(config) {
  return JSON.parse(JSON.stringify(config || {}));
}

function _editableAlertConfig(config) {
  return ALERT_EDITABLE_CONFIG_KEYS.reduce(function(result, key) {
    result[key] = config[key];
    return result;
  }, {});
}

                                                        
                                              
                                                          
                               
function _isAlertsActive() {
  return window.currentTab === 'mon-alerts' &&
    _alertsState.root &&
    document.documentElement.contains(_alertsState.root);
}

   
                                                               
  
                                                
                                
                                                           
                   
  
                                                                          
                                                            
                                                           
   
function _beginAlertsView(root) {
  if (_alertsState.historyFilterUnsub) {
    _alertsState.historyFilterUnsub();
    _alertsState.historyFilterUnsub = null;
  }
  _alertsState.navGen = PCV.ui.navGen();
  _alertsState.root = root;
  _alertsState.config = null;
  _alertsState.baseline = null;
  _alertsState.history = [];
  _alertsState.configError = null;
  _alertsState.historyError = null;
  _alertsState.historyRefreshError = null;
  _alertsState.configLoading = false;
  _alertsState.historyLoading = false;
  _alertsState.refreshing = false;
  _alertsState.historyPage = 0;
  _alertsState.lastSuccessAt = null;
  _alertsState.historyRequestGen++;
  _alertsState.applyMessage = null;
  _alertsState.applyConflict = false;
  _alertsState.ackAllBusy = false;
  _alertsState.ackAllProgress = null;
  if (typeof clearFormDirty === 'function') clearFormDirty('alert-config');
  return _alertsState.navGen;
}

                                           
                                                                 
                                                       
                                             
                                               
                                 
function _alertViewIsCurrent(navGen) {
  return navGen === _alertsState.navGen &&
    navGen === PCV.ui.navGen() &&
    _isAlertsActive();
}

function _renderAlertConfigLoading() {
  _alertsState.configRoot.setAttribute('aria-busy', 'true');
  PCV.uxlib.clearEl(_alertsState.configRoot);
  _alertsState.configRoot.appendChild(
    PCV.uxlib.el('div', { class: 'skeleton skeleton-card' },
      _L('알림 설정을 불러오는 중', 'Loading alert settings'))
  );
}

function _renderAlertConfigError() {
  var el = PCV.uxlib.el;
  _alertsState.configRoot.setAttribute('aria-busy', 'false');
  PCV.uxlib.clearEl(_alertsState.configRoot);
  _alertsState.configRoot.appendChild(el('div', { class: 'hc' },
    el('h2', { id: 'alert-config-title' },
      _L('알림 설정', 'Alert Configuration')),
    PCV.uxlib.msg('err', { tag: 'p' },
      _L('설정을 불러오지 못했습니다', 'Could not load settings')),
    el('button', {
      class: 'btn',
      type: 'button',
      'data-alert-config-retry': '',
      onClick: function() {
        _loadAlertConfig(_alertsState.navGen);
      }
    }, _L('다시 시도', 'Retry'))
  ));
}

function _utf8Length(value) {
  return new TextEncoder().encode(value).length;
}

   
                                                          
  
                                                   
                                                             
                                                  
  
                                                        
                                                          
                                                            
                                                        
                     
  
                                                          
   
function _validAlertConfig(config) {
  if (!config || typeof config !== 'object' || Array.isArray(config)) return false;
  var thresholds = [
    'cpu_warn', 'cpu_crit',
    'mem_warn', 'mem_crit',
    'disk_warn', 'disk_crit'
  ];
  var thresholdsValid = thresholds.every(function(key) {
    return Number.isInteger(config[key]) &&
      config[key] >= 0 && config[key] <= 100;
  });
  return typeof config.enabled === 'boolean' &&
    Number.isInteger(config.config_revision) &&
    config.config_revision >= 1 &&
    thresholdsValid &&
    config.cpu_warn < config.cpu_crit &&
    config.mem_warn < config.mem_crit &&
    config.disk_warn < config.disk_crit &&
    Number.isInteger(config.eval_period) &&
    config.eval_period >= 5 && config.eval_period <= 600 &&
    typeof config.webhook_url === 'string' &&
    _utf8Length(config.webhook_url) < 512 &&
    ['slack', 'telegram', 'generic'].indexOf(config.webhook_format) !== -1 &&
    typeof config.telegram_chat_id === 'string' &&
    _utf8Length(config.telegram_chat_id) < 64 &&
    (config.daemon_config_valid === undefined ||
      typeof config.daemon_config_valid === 'boolean') &&
    (config.daemon_config_error === undefined ||
      typeof config.daemon_config_error === 'string');
}

   
                                                         
  
                                                     
                                                           
                                                  
                                                 
                     
  
                                                      
                                                    
                                                                    
   
async function _loadAlertConfig(navGen) {
  _alertsState.configError = null;
  _alertsState.configLoading = true;
  _renderAlertConfigLoading();
  try {
    var response = await fetchGet(EP.ALERTS_CONFIG());
    if (!_alertViewIsCurrent(navGen)) return;
    if (response && response.error) {
      throw new Error(response.error.message || 'config unavailable');
    }
    var config = unwrapData(response);
    if (!_validAlertConfig(config)) throw new Error('invalid alert config');
    _alertsState.config = _cloneAlertConfig(config);
    _alertsState.baseline = _editableAlertConfig(config);
    _alertsState.applyConflict = false;
    _renderAlertConfigRegion();
  } catch (error) {
    if (!_alertViewIsCurrent(navGen)) return;
    _alertsState.configError = error;
    _renderAlertConfigError();
  } finally {
    if (_alertViewIsCurrent(navGen)) {
      _alertsState.configLoading = false;
      _renderAlertHistoryRegion();
    }
  }
}

function _alertNumberInput(id, label, value, min, max) {
  return PCV.uxlib.el('input', {
    id: id,
    type: 'number',
    value: String(value),
    min: String(min),
    max: String(max),
    'aria-label': label,
    onInput: _onAlertFormChange
  });
}

function _alertThresholdCard(name, warnKey, critKey) {
  var el = PCV.uxlib.el;
  return el('div', { class: 'hc' },
    el('h3', null, name + ' ' + _L('임계값', 'Thresholds')),
    HN.row('Warning (%)',
      _alertNumberInput(
        'al-' + warnKey,
        name + ' warning threshold (%)',
        _alertsState.config[warnKey],
        0,
        100
      )),
    HN.row('Critical (%)',
      _alertNumberInput(
        'al-' + critKey,
        name + ' critical threshold (%)',
        _alertsState.config[critKey],
        0,
        100
      )));
}

function _readAlertForm() {
  var root = _alertsState.configRoot;
  return {
    enabled: root.querySelector('#al-enabled').checked,
    cpu_warn: root.querySelector('#al-cpu_warn').valueAsNumber,
    cpu_crit: root.querySelector('#al-cpu_crit').valueAsNumber,
    mem_warn: root.querySelector('#al-mem_warn').valueAsNumber,
    mem_crit: root.querySelector('#al-mem_crit').valueAsNumber,
    disk_warn: root.querySelector('#al-disk_warn').valueAsNumber,
    disk_crit: root.querySelector('#al-disk_crit').valueAsNumber,
    eval_period: root.querySelector('#al-eval_period').valueAsNumber,
    webhook_url: root.querySelector('#al-webhook_url').value,
    webhook_format: root.querySelector('#al-webhook_format').value,
    telegram_chat_id: root.querySelector('#al-telegram_chat_id').value
  };
}

   
                                                                
  
                                                   
                                                      
                               
                                                         
  
                                                    
                                                          
                                        
   
function _validateAlertDraft(config) {
  var invalid = [];
  var integerRange = function(key, min, max) {
    if (!Number.isInteger(config[key]) ||
        config[key] < min || config[key] > max) {
      invalid.push('al-' + key);
    }
  };
  [
    'cpu_warn', 'cpu_crit',
    'mem_warn', 'mem_crit',
    'disk_warn', 'disk_crit'
  ].forEach(function(key) {
    integerRange(key, 0, 100);
  });
  integerRange('eval_period', 5, 600);
  [
    ['cpu_warn', 'cpu_crit'],
    ['mem_warn', 'mem_crit'],
    ['disk_warn', 'disk_crit']
  ].forEach(function(pair) {
    if (config[pair[0]] >= config[pair[1]]) {
      invalid.push('al-' + pair[0], 'al-' + pair[1]);
    }
  });

  var validWebhookUrl = true;
  if (config.webhook_url) {
    try {
      var parsed = new URL(config.webhook_url);
      validWebhookUrl =
        (parsed.protocol === 'http:' || parsed.protocol === 'https:') &&
        !!parsed.hostname;
    } catch {
      validWebhookUrl = false;
    }
  }
  if (_utf8Length(config.webhook_url) >= 512 || !validWebhookUrl) {
    invalid.push('al-webhook_url');
  }
  if (_utf8Length(config.telegram_chat_id) >= 64) {
    invalid.push('al-telegram_chat_id');
  }
  return {
    valid: invalid.length === 0,
    fields: Array.from(new Set(invalid)),
    message: invalid.length
      ? _L(
          '입력 범위·길이와 Warning < Critical 조건을 확인하세요',
          'Check input ranges, lengths, and ensure Warning < Critical'
        )
      : ''
  };
}

function _alertConfigDirty() {
  if (!_alertsState.baseline ||
      !_alertsState.configRoot ||
      !_alertsState.configRoot.querySelector('#al-enabled')) return false;
  return JSON.stringify(_readAlertForm()) !==
    JSON.stringify(_alertsState.baseline);
}

   
                                                                
                                             
  
                                                 
                                                  
                                                 
  
                                                               
                                                
                                              
   
function _syncAlertDirtyState() {
  if (!_alertsState.configRoot ||
      !_alertsState.configRoot.querySelector('#al-enabled')) return;
  var apply = _alertsState.configRoot.querySelector('[data-alert-apply]');
  var conflictReload =
    _alertsState.configRoot.querySelector('[data-alert-conflict-reload]');
  var status = _alertsState.configRoot.querySelector('#al-status');
  var validation = _validateAlertDraft(_readAlertForm());
  var dirty = _alertConfigDirty();
  if (apply) {
    apply.disabled = !dirty || !validation.valid ||
      _alertsState.applying || _alertsState.applyConflict;
  }
  if (conflictReload) {
    conflictReload.hidden = !_alertsState.applyConflict;
    conflictReload.disabled = _alertsState.applying;
  }
  _alertsState.configRoot
    .querySelectorAll('[aria-invalid="true"]')
    .forEach(function(node) {
      node.removeAttribute('aria-invalid');
    });
  validation.fields.forEach(function(id) {
    var node = _alertsState.configRoot.querySelector('#' + id);
    if (node) node.setAttribute('aria-invalid', 'true');
  });
  if (dirty) {
    if (typeof markFormDirty === 'function') markFormDirty('alert-config');
  } else if (typeof clearFormDirty === 'function') {
    clearFormDirty('alert-config');
  }

  if (_alertsState.applying) {
    if (status) {
      PCV.uxlib.setMsg(
        status,
        'loading',
        null,
        _L('적용 중…', 'Applying…')
      );
    }
  } else if (!validation.valid) {
    if (status) PCV.uxlib.setMsg(status, 'err', null, validation.message);
  } else if (_alertsState.applyMessage) {
    if (status) {
      PCV.uxlib.setMsg(
        status,
        _alertsState.applyMessage.kind,
        null,
        _alertsState.applyMessage.text
      );
    }
  } else if (status) {
    PCV.uxlib.setMsg(
      status,
      dirty ? 'warn' : 'muted',
      null,
      dirty ? _L('적용되지 않은 변경', 'Unapplied changes') : ''
    );
  }
}

function _onAlertFormChange() {
  if (!_alertsState.applyConflict) _alertsState.applyMessage = null;
  _syncAlertDirtyState();
}

async function _reloadAlertConfigAfterConflict() {
  var allowed = await PCV.ui.customConfirm(
    _L('서버 설정 다시 불러오기', 'Reload server settings'),
    _L(
      '현재 입력을 폐기하고 다른 작업자가 적용한 최신 설정을 불러오시겠습니까?',
      'Discard the current draft and load the latest settings applied by another operator?'
    )
  );
  if (!allowed || !_isAlertsActive()) return;
  _alertsState.applyConflict = false;
  await _loadAlertConfig(_alertsState.navGen);
}

  
               
                                                         
                                                
                                                      
                                              
                                                     
   
function _buildAlertDaemonWarning() {
  var warning = PCV.uxlib.msg('warn', {
    tag: 'p',
    cls: 'alert-daemon-config-warning'
  }, _L(
    'daemon.conf의 알림 설정이 잘못되어 임계값 평가를 끈 안전 기본값으로 실행 중입니다. 보안·운영 이벤트는 계속 기록되며, 현재 적용은 재시작 전까지만 유지됩니다.',
    'Invalid daemon.conf alert settings were replaced with safe defaults and threshold evaluation was disabled. Security and operational events continue; runtime changes last only until restart.'
  ));
  warning.setAttribute('aria-live', 'polite');
  return warning;
}

   
                                                
  
                                                   
                                                      
                                                                       
                                                      
             
   
function _renderAlertConfigRegion() {
  var el = PCV.uxlib.el;
  var config = _alertsState.config;
  _alertsState.configRoot.setAttribute('aria-busy', 'false');
  PCV.uxlib.clearEl(_alertsState.configRoot);
  _alertsState.configRoot.appendChild(PCV.uxlib.frag(
    el('h2', {
      id: 'alert-config-title',
      class: 'ops-section-title',
      tabindex: '-1'
    }, _L('알림 설정', 'Alert Configuration')),
    config.daemon_config_valid === false
      ? _buildAlertDaemonWarning()
      : null,
    el('div', { class: 'alert-engine-row' },
      HN.statusPill(
        config.enabled ? 'ok' : 'idle',
        config.enabled ? 'ENABLED' : 'DISABLED'
      ),
      el('label', { for: 'al-enabled' },
        _L('임계값 알림 사용', 'Enable threshold alerts')),
      el('input', {
        id: 'al-enabled',
        type: 'checkbox',
        checked: config.enabled ? '' : null,
        onChange: _onAlertFormChange
      })),
    el('div', { class: 'sg grid-3 alert-threshold-grid' },
      _alertThresholdCard('CPU', 'cpu_warn', 'cpu_crit'),
      _alertThresholdCard(
        _L('메모리', 'Memory'),
        'mem_warn',
        'mem_crit'
      ),
      _alertThresholdCard(
        _L('디스크', 'Disk'),
        'disk_warn',
        'disk_crit'
      )),
    el('div', { class: 'sg grid-2 alert-secondary-grid' },
      el('div', { class: 'hc' },
        el('h3', null, _L('평가 주기', 'Evaluation Period')),
        HN.row(_L('유지 시간(초)', 'Hold time (sec)'),
          _alertNumberInput(
            'al-eval_period',
            'Hold time (sec)',
            config.eval_period,
            5,
            600
          ))),
      el('div', { class: 'hc' },
        el('h3', null, 'Webhook'),
                                                                 
                                                                  
        el('div', { class: 'alert-webhook-grid' },
          el('label', { for: 'al-webhook_format' },
            _L('형식', 'Format')),
          el('select', {
            id: 'al-webhook_format',
            onChange: _onAlertFormChange
          }, ['slack', 'telegram', 'generic'].map(function(format) {
            return el('option', {
              value: format,
              selected: config.webhook_format === format ? '' : null
            }, format);
          })),
          el('label', { for: 'al-webhook_url' }, 'URL'),
          el('input', {
            id: 'al-webhook_url',
            type: 'url',
            value: config.webhook_url,
            onInput: _onAlertFormChange
          }),
          el('label', { for: 'al-telegram_chat_id' }, 'Telegram Chat ID'),
          el('input', {
            id: 'al-telegram_chat_id',
            value: config.telegram_chat_id,
            onInput: _onAlertFormChange
          })))),
    el('div', { class: 'alert-config-actions' },
      el('button', {
        class: 'btn btn-primary',
        type: 'button',
        'data-alert-apply': '',
        'data-role': 'ADMIN',
        disabled: '',
        onClick: applyAlertConfig
      }, _alertsState.applying
        ? _L('적용 중…', 'Applying…')
        : _L('적용', 'Apply')),
      el('button', {
        class: 'btn',
        type: 'button',
        'data-alert-conflict-reload': '',
        'data-role': 'ADMIN',
        hidden: _alertsState.applyConflict ? null : '',
        onClick: _reloadAlertConfigAfterConflict
      }, _L('서버 설정 다시 불러오기', 'Reload server settings')),
      el('p', { class: 'color-muted alert-runtime-note' },
        _L(
          '현재 실행 환경에만 적용됩니다. 데몬을 재시작하면 daemon.conf 설정으로 돌아갑니다.',
          'Applies to the current runtime only. A daemon restart restores daemon.conf values.'
        )),
      el('span', {
        id: 'al-status',
        class: 'alert-form-status',
        'aria-live': 'polite',
        'aria-atomic': 'true'
      }))
  ));
  if (_alertsState.applying) {
    _alertsState.configRoot
      .querySelectorAll('input,select,button')
      .forEach(function(node) {
        node.disabled = true;
      });
  }
  _syncAlertDirtyState();
                                                      
                                                            
                                                 
  var alertRole = String((window.currentUser && window.currentUser.role) || '').toUpperCase();
  var canEditAlerts = typeof pcvRoleAllows === 'function'
    ? pcvRoleAllows('admin')
    : alertRole === 'ADMIN';
  if (!canEditAlerts) {
    _alertsState.configRoot.querySelectorAll('input,select').forEach(function(node) {
      node.disabled = true;
    });
  }
  if (typeof applyRoleVisibility === 'function') applyRoleVisibility(window.currentUser && window.currentUser.role);
}

  
                  
                                                                            
                                                                              
                                                                              
                               
  
               
                                                                           
                                                     
   
async function applyAlertConfig() {
  if (_alertsState.applying ||
      _alertsState.applyConflict ||
      !_alertConfigDirty()) return;
  var navGen = _alertsState.navGen;
  var body = _readAlertForm();
  var validation = _validateAlertDraft(body);
  if (!validation.valid) {
    _syncAlertDirtyState();
    var firstInvalid =
      _alertsState.configRoot.querySelector('#' + validation.fields[0]);
    if (firstInvalid) firstInvalid.focus();
    return;
  }
  body.expected_revision = _alertsState.config.config_revision;
  var controls = Array.from(
    _alertsState.configRoot.querySelectorAll('input,select,button')
  );
  var apply = _alertsState.configRoot.querySelector('[data-alert-apply]');
  var applied = false;
  _alertsState.applying = true;
  controls.forEach(function(node) {
    node.disabled = true;
  });
  if (apply) apply.textContent = _L('적용 중…', 'Applying…');
  _syncAlertDirtyState();
  try {
    var response = await fetchPut(EP.ALERTS_CONFIG(), body);
    if (!_alertViewIsCurrent(navGen)) return;
    if (response && response.error) {
      var applyError =
        new Error(response.error.message || 'apply failed');
      applyError.code = response.error.code;
      throw applyError;
    }
    var appliedConfig = unwrapData(response);
    if (!_validAlertConfig(appliedConfig)) {
      throw new Error('invalid applied alert config');
    }
    _alertsState.config = _cloneAlertConfig(appliedConfig);
    _alertsState.baseline = _editableAlertConfig(appliedConfig);
    _alertsState.applyConflict = false;
    _alertsState.applyMessage = {
      kind: 'ok',
      text: _L(
        '현재 실행 환경에 설정을 적용했습니다',
        'Settings applied to the current runtime'
      )
    };
    applied = true;
  } catch (error) {
    if (!_alertViewIsCurrent(navGen)) return;
    _alertsState.applyConflict =
      error.code === PCV.api.RPC_ERROR.CONFLICT;
    _alertsState.applyMessage = _alertsState.applyConflict
      ? {
          kind: 'warn',
          text: _L(
            '다른 작업자가 설정을 변경했습니다. 현재 입력은 보존되었습니다.',
            'Another operator changed these settings. Your draft has been preserved.'
          )
        }
      : {
          kind: 'err',
          text: _L(
            '설정을 적용하지 못했습니다',
            'Could not apply settings'
          ) + ': ' + error.message
        };
  } finally {
    _alertsState.applying = false;
    if (_alertViewIsCurrent(navGen)) {
      if (applied) {
        _renderAlertConfigRegion();
        _renderAlertHistoryRegion();
      } else {
        controls.forEach(function(node) {
          node.disabled = false;
        });
        if (apply) apply.textContent = _L('적용', 'Apply');
        _syncAlertDirtyState();
      }
    } else if (_isAlertsActive()) {
      _loadAlertConfig(_alertsState.navGen);
    }
  }
}

                                  
                                                              
                                            

                                                      
                                                  
                                               
var ALERT_HISTORY_PAGE_SIZE = 100;
var ALERT_HISTORY_CACHE_LIMIT = 1000;
                                                          
                                                  
var ALERT_ACK_ALL_BATCH = 20;

function _alertTimestampValue(timestamp) {
  return typeof timestamp === 'number' && Number.isFinite(timestamp)
    ? timestamp
    : null;
}

function _alertTimeValue(alert) {
  var value = _alertTimestampValue(alert && alert.timestamp);
  return value === null ? -Infinity : value;
}

function _formatAlertTime(timestamp) {
  var value = _alertTimestampValue(timestamp);
  if (value === null) return '—';
  var date = new Date(value * 1000);
  if (Number.isNaN(date.getTime())) return '—';
  var pad = function(number) {
    return String(number).padStart(2, '0');
  };
  return date.getFullYear() + '-' + pad(date.getMonth() + 1) + '-' +
    pad(date.getDate()) + ' ' + pad(date.getHours()) + ':' +
    pad(date.getMinutes()) + ':' + pad(date.getSeconds());
}

function _formatAlertHistoryValue(value) {
  return typeof value === 'number' && Number.isFinite(value)
    ? value.toFixed(1) + '%'
    : '—';
}

function _alertLabel(alert) {
  var severity = HN.alertSeverity(alert);
  return severity === 'crit'
    ? 'CRIT'
    : (severity === 'warn' ? 'WARN' : 'UNKNOWN');
}

function _selectedAlertSeverity() {
  var state = PCV.ui.filterState
    ? PCV.ui.filterState.current()
    : {};
  var selected = state.severity || [];
  return selected.length === 1 &&
    (selected[0] === 'crit' || selected[0] === 'warn')
    ? selected[0]
    : null;
}

function _shownAlerts() {
  var selected = _selectedAlertSeverity();
  return selected
    ? _alertsState.history.filter(function(alert) {
        return HN.alertSeverity(alert) === selected;
      })
    : _alertsState.history;
}

function _alertHistoryPage(alerts) {
  var maxPage = Math.max(
    0,
    Math.ceil(alerts.length / ALERT_HISTORY_PAGE_SIZE) - 1
  );
  _alertsState.historyPage = Math.max(
    0,
    Math.min(_alertsState.historyPage, maxPage)
  );
  var start = _alertsState.historyPage * ALERT_HISTORY_PAGE_SIZE;
  return {
    rows: alerts.slice(start, start + ALERT_HISTORY_PAGE_SIZE),
    start: alerts.length ? start + 1 : 0,
    end: Math.min(start + ALERT_HISTORY_PAGE_SIZE, alerts.length),
    total: alerts.length,
    maxPage: maxPage
  };
}

function _changeAlertHistoryPage(delta) {
  _alertsState.historyPage += delta;
  _renderAlertHistoryRegion();
}

function _clearAlertFilter() {
  PCV.ui.filterState.apply({ severity: [] });
}

function _focusAlertConfig() {
  var heading = document.getElementById('alert-config-title');
  if (heading) heading.focus();
}

function _alertEmptyNode(message, actionLabel, action) {
  var el = PCV.uxlib.el;
  return el('div', { class: 'alert-history-empty' },
    el('p', { class: 'color-muted' }, message),
    actionLabel
      ? el('button', {
          class: 'btn',
          type: 'button',
          onClick: action
        }, actionLabel)
      : null);
}

                                                             
                                                    
                                  
function _ackCell(alert) {
  var el = PCV.uxlib.el;
  if (alert.acknowledged) return HN.statusPill('idle', 'ACK');
  if (typeof alert.alert_id !== 'number') return el('span', { class: 'color-muted' }, '—');
  return el('button', {
    class: 'btn btn-sm btn-g',
    type: 'button',
    'data-ack-id': alert.alert_id,
    'data-role': 'OPERATOR,ADMIN',
    onClick: function() { _ackAlert(alert.alert_id); }
  }, _L('확인', 'ACK'));
}

function _renderAlertHistoryTable(alerts) {
  var el = PCV.uxlib.el;
  var rows = alerts.map(function(record) {
    var alert = record && typeof record === 'object' &&
      !Array.isArray(record) ? record : {};
    var severity = HN.alertSeverity(alert);
    return el('tr', { class: 'alert-row-' + severity },
      el('td', {
        class: 'alert-history-time font-mono'
      }, _formatAlertTime(alert.timestamp)),
      el('td', null, HN.statusPill(severity, _alertLabel(alert))),
      el('td', {
        class: 'alert-history-metric font-mono'
      }, typeof alert.metric === 'string' && alert.metric
        ? alert.metric
        : '—'),
      el('td', {
        class: 'alert-history-value font-mono text-right'
      }, _formatAlertHistoryValue(alert.value)),
      el('td', { class: 'color-muted' },
        typeof alert.message === 'string' && alert.message
          ? alert.message
          : '—'),
      el('td', null, _ackCell(alert)));
  });
  return el('table', { class: 'text-11' },
    el('caption', null, _L('최근 알림 이력', 'Recent alert history')),
    el('thead', null, el('tr', null,
      el('th', { scope: 'col' }, _L('시각', 'Time')),
      el('th', { scope: 'col' }, _L('심각도', 'Severity')),
      el('th', { scope: 'col' }, _L('지표', 'Metric')),
      el('th', { scope: 'col' }, _L('값', 'Value')),
      el('th', { scope: 'col' }, _L('메시지', 'Message')),
      el('th', { scope: 'col' }, _L('확인', 'Ack')))),
    el.apply(null, ['tbody', null].concat(rows)));
}

function _formatAlertFetchTime(date) {
  if (!(date instanceof Date) || Number.isNaN(date.getTime())) return '—';
  var pad = function(number) {
    return String(number).padStart(2, '0');
  };
  return pad(date.getHours()) + ':' + pad(date.getMinutes()) + ':' +
    pad(date.getSeconds());
}

function _alertHistoryRefreshMessage() {
  var message = PCV.uxlib.msg('warn', {
    tag: 'p',
    cls: 'alert-history-refresh-error'
  }, _L(
    '새로고침에 실패했습니다. 새 알림 이력을 갱신하지 못했습니다. 기존 이력은 유지됩니다.',
    'Refresh failed. Existing alert history is preserved.'
  ));
  message.setAttribute('aria-live', 'polite');
  return message;
}

  
                  
                                                                 
                                                          
                                   
  
               
                                           
   
async function _ackAlert(alertId) {
  try {
    var r = await fetchPost(EP.RPC(), {
      jsonrpc: '2.0', method: 'alert.ack',
      params: { alert_id: alertId }, id: 'ack-' + alertId
    });
    if (r && r.error) {
      toast(r.error.message || _L('확인 실패', 'Acknowledge failed'), false);
      return;
    }
    var record = _alertsState.history.find(function(a) {
      return a && typeof a === 'object' && !Array.isArray(a) &&
        a.alert_id === alertId;
    });
    if (record) record.acknowledged = true;
                                                                         
                                                      
                                                         
                                                                 
                                                    
                              
    _alertsState.historyLoading = false;
    _alertsState.refreshing = false;
    _alertsState.historyRequestGen++;
    if (_isAlertsActive()) _renderAlertHistoryRegion();
  } catch (e) {
    toast((e && e.message) || _L('확인 실패', 'Acknowledge failed'), false);
  }
}

  
                  
                                                                
                                                        
                                                               
  
               
                                                  
                                                   
   
async function _ackAllAlerts() {
  if (_alertsState.ackAllBusy) return;
                                                            
                                                        
                                                      
  var targets = _shownAlerts().filter(function(a) {
    return a && typeof a === 'object' && !Array.isArray(a) &&
      !a.acknowledged && typeof a.alert_id === 'number';
  });
  if (!targets.length) return;
  var allowed = await PCV.ui.customConfirm(
    _L('알림 전체 확인', 'Acknowledge all alerts'),
    _L(
      '현재 필터에 표시된 미확인 알림 ' + targets.length + '건을 확인 처리하시겠습니까?',
      'Acknowledge ' + targets.length + ' unacknowledged alerts shown by the current filter?'
    )
  );
  if (!allowed || !_isAlertsActive()) return;
                                                       
                                                        
                                                            
  _alertsState.historyLoading = false;
  _alertsState.refreshing = false;
  _alertsState.historyRequestGen++;
  _alertsState.ackAllBusy = true;
  _alertsState.ackAllProgress = { done: 0, total: targets.length };
  _renderAlertHistoryRegion();
  var failed = 0;
  for (var i = 0; i < targets.length; i += ALERT_ACK_ALL_BATCH) {
    var batch = targets.slice(i, i + ALERT_ACK_ALL_BATCH);
    var results = await Promise.allSettled(batch.map(function(alert) {
      return fetchPost(EP.RPC(), {
        jsonrpc: '2.0', method: 'alert.ack',
        params: { alert_id: alert.alert_id }, id: 'ack-' + alert.alert_id
      }).then(function(r) {
        if (r && r.error) throw new Error(r.error.message || 'ack failed');
        alert.acknowledged = true;
      });
    }));
    results.forEach(function(res) { if (res.status !== 'fulfilled') failed++; });
    _alertsState.ackAllProgress = {
      done: Math.min(i + batch.length, targets.length),
      total: targets.length
    };
    if (!_isAlertsActive()) { _alertsState.ackAllBusy = false; _alertsState.ackAllProgress = null; return; }
    _renderAlertHistoryRegion();
  }
                                                        
                                                       
                                 
  _alertsState.historyLoading = false;
  _alertsState.refreshing = false;
  _alertsState.historyRequestGen++;
  _alertsState.ackAllBusy = false;
  _alertsState.ackAllProgress = null;
  if (_isAlertsActive()) _renderAlertHistoryRegion();
  if (failed) toast(_L(failed + '건 확인 실패', failed + ' failed to acknowledge'), false);
  else toast(_L('전체 확인 완료', 'All alerts acknowledged'));
}

   
                                                            
  
                                                  
                                                     
                                                          
                                                   
                                 
  
                                                   
                                                     
                                                       
                                   
  
                                                                  
                              
   
function _renderAlertHistoryRegion() {
  if (!_alertsState.historyRoot) return;
  var el = PCV.uxlib.el;
  var active = document.activeElement;
  var focusKey = active && _alertsState.historyRoot.contains(active)
    ? {
        facet: active.dataset.facet,
        value: active.dataset.val,
        page: active.dataset.alertPage,
        ackId: active.dataset.ackId
      }
    : null;
  var counts = { crit: 0, warn: 0 };
  _alertsState.history.forEach(function(alert) {
    var severity = HN.alertSeverity(alert);
    if (severity === 'crit') counts.crit++;
    if (severity === 'warn') counts.warn++;
  });
  var filter = HN.filterBar([{
    key: 'severity',
    label: _L('알림 심각도 필터', 'Alert severity filter'),
    showAll: true,
    allLabel: _L('전체', 'All'),
    allCount: _alertsState.history.length,
    singleSelect: true,
    disabled: _alertsState.historyLoading || _alertsState.refreshing,
    options: [
      { value: 'crit', label: 'Critical', count: counts.crit, sw: 'crit' },
      { value: 'warn', label: 'Warning', count: counts.warn, sw: 'warn' }
    ]
  }]);
  var shown = _shownAlerts();
  var page = _alertHistoryPage(shown);
  var selected = _selectedAlertSeverity();
  var body;
  if (_alertsState.historyLoading) {
    body = el('div', { class: 'skeleton skeleton-card' },
      _L('알림 이력을 불러오는 중', 'Loading alert history'));
  } else if (_alertsState.historyError && !_alertsState.history.length) {
    body = _alertEmptyNode(
      _L('알림 이력을 불러오지 못했습니다', 'Could not load alert history'),
      _L('다시 시도', 'Retry'),
      function() {
        _loadAlertHistory(_alertsState.navGen);
      }
    );
    body.querySelector('button').setAttribute(
      'data-alert-history-retry',
      ''
    );
  } else if (!_alertsState.history.length) {
    if (_alertsState.configLoading) {
      body = _alertEmptyNode(_L(
        '알림 이력이 없습니다. 설정 상태를 확인하는 중입니다.',
        'No alert history. Checking alert settings.'
      ));
    } else if (_alertsState.configError) {
      body = _alertEmptyNode(_L(
        '알림 이력이 없습니다. 임계값 평가 상태는 확인할 수 없습니다.',
        'No alert history. Threshold evaluation status is unavailable.'
      ));
    } else if (_alertsState.config && _alertsState.config.enabled) {
      body = _alertEmptyNode(_L(
        '임계값을 초과한 알림이 아직 없습니다',
        'No threshold alerts yet'
      ));
    } else {
      body = _alertEmptyNode(
        _L(
          '임계값 알림 평가가 꺼져 있습니다. 보안·운영 이벤트는 계속 기록됩니다.',
          'Threshold alert evaluation is off. Security and operational events continue to be recorded.'
        ),
        _L('설정으로 이동', 'Go to settings'),
        _focusAlertConfig
      );
    }
  } else if (!shown.length) {
    body = _alertEmptyNode(
      (selected === 'crit' ? 'Critical' : 'Warning') + ' ' +
        _L('알림이 없습니다', 'alerts are empty'),
      _L('전체 보기', 'Show all'),
      _clearAlertFilter
    );
  } else {
    body = _renderAlertHistoryTable(page.rows);
  }

  var pagination = shown.length > ALERT_HISTORY_PAGE_SIZE
    ? el('nav', {
        class: 'alert-history-pagination',
        'aria-label': _L('알림 이력 페이지', 'Alert history pages')
      },
        el('button', {
          class: 'btn',
          type: 'button',
          'data-alert-page': 'prev',
          disabled: _alertsState.historyPage === 0 ? '' : null,
          onClick: function() {
            _changeAlertHistoryPage(-1);
          }
        }, _L('이전', 'Previous')),
        el('span', { class: 'color-muted' },
          page.start + '–' + page.end + ' / ' + page.total),
        el('button', {
          class: 'btn',
          type: 'button',
          'data-alert-page': 'next',
          disabled: _alertsState.historyPage === page.maxPage ? '' : null,
          onClick: function() {
            _changeAlertHistoryPage(1);
          }
        }, _L('다음', 'Next')))
    : null;

  PCV.uxlib.clearEl(_alertsState.historyRoot);
  _alertsState.historyRoot.setAttribute(
    'aria-busy',
    _alertsState.historyLoading || _alertsState.refreshing
      ? 'true'
      : 'false'
  );
  _alertsState.historyRoot.appendChild(PCV.uxlib.frag(
    el('div', { class: 'alert-history-head' },
      el('h2', {
        id: 'alert-history-title',
        class: 'ops-section-title'
      }, _L('알림 이력', 'Alert History')),
      el('div', { class: 'alert-history-actions' },
        el('span', {
          id: 'alert-last-updated',
          class: 'color-muted'
        }, _L('마지막 조회 ', 'Last fetched ') +
          _formatAlertFetchTime(_alertsState.lastSuccessAt)),
        el('button', {
          class: 'btn',
          type: 'button',
          'data-alert-refresh': '',
          'aria-busy': _alertsState.historyLoading ||
            _alertsState.refreshing ? 'true' : 'false',
          disabled: _alertsState.historyLoading ||
            _alertsState.refreshing ? '' : null,
          onClick: refreshAlertHistory
        }, _alertsState.refreshing
          ? _L('새로고침 중…', 'Refreshing…')
          : _L('새로고침', 'Refresh')),
        el('button', {
          class: 'btn',
          type: 'button',
          'data-alert-ack-all': '',
          'data-role': 'OPERATOR,ADMIN',
          disabled: (_alertsState.ackAllBusy ||
            !_alertsState.history.some(function(a) {
              return a && typeof a === 'object' && !Array.isArray(a) &&
                !a.acknowledged && typeof a.alert_id === 'number';
            })) ? '' : null,
          onClick: function() { _ackAllAlerts(); }
        }, _alertsState.ackAllBusy && _alertsState.ackAllProgress
          ? _alertsState.ackAllProgress.done + '/' +
            _alertsState.ackAllProgress.total + '…'
          : _L('전체 확인', 'ACK All')))),
    filter,
    el('span', {
      id: 'alert-result-count',
      class: 'sr-only',
      'aria-live': 'polite'
    }, page.start + '–' + page.end + ' / ' + page.total +
      _L('건 표시', ' results')),
    el('div', {
      id: 'alert-history-message',
      tabindex: '-1',
      'aria-live': 'polite',
      'aria-atomic': 'true'
    }, _alertsState.historyRefreshError
      ? _alertHistoryRefreshMessage()
      : null),
    body,
    pagination
  ));
  if (focusKey && focusKey.facet && focusKey.value) {
    var replacement = _alertsState.historyRoot.querySelector(
      '[data-facet="' + focusKey.facet + '"]' +
      '[data-val="' + focusKey.value + '"]'
    );
    if (replacement) replacement.focus();
  } else if (focusKey && focusKey.page) {
    var pageButton = _alertsState.historyRoot.querySelector(
      '[data-alert-page="' + focusKey.page + '"]'
    );
    if (pageButton && !pageButton.disabled) {
      pageButton.focus();
    } else {
      var fallback = _alertsState.historyRoot.querySelector(
        '[data-alert-page]:not([disabled])'
      );
      if (fallback) fallback.focus();
    }
  } else if (focusKey && focusKey.ackId) {
                                                    
                                                  
                                                   
    var ackRows = page.rows;
    var ackIdx = ackRows.findIndex(function(a) {
      return a && typeof a === 'object' && !Array.isArray(a) &&
        String(a.alert_id) === focusKey.ackId;
    });
    var nextAckButton = null;
    for (var ackI = ackIdx + 1; ackIdx !== -1 && ackI < ackRows.length; ackI++) {
      var candidate = ackRows[ackI];
      if (candidate && typeof candidate === 'object' && !Array.isArray(candidate) &&
          !candidate.acknowledged && typeof candidate.alert_id === 'number') {
        nextAckButton = _alertsState.historyRoot.querySelector(
          '[data-ack-id="' + candidate.alert_id + '"]'
        );
        if (nextAckButton) break;
      }
    }
    if (nextAckButton) nextAckButton.focus();
    else {
      var ackFallback = document.getElementById('alert-history-message');
      if (ackFallback) ackFallback.focus();
    }
  }
                                                                 
                                                          
                                                           
  if (window.currentUser && typeof applyRoleVisibility === 'function') {
    applyRoleVisibility(window.currentUser.role);
  }
}

  
                  
                                                                           
                                                                           
                                                                              
                                                                               
                                                                    
  
               
                                                                           
                                                                            
                                                                              
   
async function _loadAlertHistory(navGen, options) {
  options = options || {};
  var preserveHistory = options.preserveHistory === true;
  var manual = options.manual === true;
  if (manual && (_alertsState.refreshing ||
      _alertsState.historyLoading)) return false;
  var requestGen = ++_alertsState.historyRequestGen;
  if (preserveHistory) {
    _alertsState.refreshing = true;
    _alertsState.historyRefreshError = null;
    _renderAlertHistoryRegion();
  } else {
    _alertsState.historyLoading = true;
    _alertsState.historyError = null;
    _alertsState.historyRefreshError = null;
    _renderAlertHistoryRegion();
  }
  try {
    var response = await fetchGet(EP.ALERTS());
    if (!_alertViewIsCurrent(navGen) ||
        requestGen !== _alertsState.historyRequestGen) return false;
    if (response && response.error) {
      throw new Error(response.error.message || 'history unavailable');
    }
    var history = unwrapData(response);
    if (!Array.isArray(history)) throw new Error('invalid alert history');
    _alertsState.history = history.slice().sort(function(a, b) {
      var aTime = _alertTimeValue(a);
      var bTime = _alertTimeValue(b);
      return aTime === bTime ? 0 : bTime - aTime;
    }).slice(0, ALERT_HISTORY_CACHE_LIMIT);
    _alertsState.historyPage = 0;
    _alertsState.lastSuccessAt = new Date();
    _alertsState.historyError = null;
    _alertsState.historyRefreshError = null;
    return true;
  } catch (error) {
    if (!_alertViewIsCurrent(navGen) ||
        requestGen !== _alertsState.historyRequestGen) return false;
    if (preserveHistory &&
        (_alertsState.lastSuccessAt || _alertsState.history.length)) {
      _alertsState.historyRefreshError = error;
    } else {
      _alertsState.historyError = error;
      _alertsState.historyRefreshError = null;
    }
    return false;
  } finally {
    if (_alertViewIsCurrent(navGen) &&
        requestGen === _alertsState.historyRequestGen) {
      _alertsState.historyLoading = false;
      _alertsState.refreshing = false;
      _renderAlertHistoryRegion();
    }
  }
}

function refreshAlertHistory() {
  return _loadAlertHistory(_alertsState.navGen, {
    preserveHistory: true,
    manual: true
  });
}

  
                  
                                                             
                                                                   
                                                    
  
               
                                              
                       
   
function refreshAlertHistoryFromEvent() {
  if (!_isAlertsActive()) return Promise.resolve(false);
  return _loadAlertHistory(_alertsState.navGen, {
    preserveHistory: true
  });
}

function _validAlertSeveritySelection(state) {
  var selected = state && state.severity;
  return selected === undefined ||
    (Array.isArray(selected) && (
      selected.length === 0 ||
      (selected.length === 1 &&
        (selected[0] === 'crit' || selected[0] === 'warn'))
    ));
}

  
                  
                                                                               
                                                                              
                                                                               
  
               
                                                                             
                                                       
   
function _onAlertHistoryFilterChange(state) {
  if (!_isAlertsActive()) {
    if (_alertsState.historyFilterUnsub) {
      _alertsState.historyFilterUnsub();
      _alertsState.historyFilterUnsub = null;
    }
    return;
  }
  if (!_validAlertSeveritySelection(state)) {
    PCV.ui.filterState.apply({ severity: [] });
    return;
  }
  _alertsState.historyPage = 0;
  _renderAlertHistoryRegion();
}

function _subscribeAlertHistoryFilter() {
  if (_alertsState.historyFilterUnsub ||
      !PCV.ui.filterState ||
      !PCV.ui.filterState.subscribe) return;
    
                                                                          
                                                                           
                                                                         
     
  if (PCV.ui.filterState.readFromUrl) {
    PCV.ui.filterState.readFromUrl();
  }
  var state = PCV.ui.filterState.current();
  if (!_validAlertSeveritySelection(state)) {
    PCV.ui.filterState.apply({ severity: [] });
  }
  _alertsState.historyFilterUnsub =
    PCV.ui.filterState.subscribe(_onAlertHistoryFilterChange);
}

                                                                  
                                                     
                        
  
               
                                                
                                                        
                                        
function _buildAlertDlq() {
  var el = PCV.uxlib.el;
  return el('section', { 'aria-labelledby': 'alert-dlq-title' },
    el('h2', { id: 'alert-dlq-title' },
      _L('웹훅 전송 실패 (DLQ)', 'Webhook Delivery Failures (DLQ)')),
    el('div', { class: 'hc' },
      el('div', { class: 'flex gap-6' },
        el('button', {
          class: 'btn',
          type: 'button',
          onClick: function() {
            if (typeof window.loadWebhookDlq === 'function') {
              window.loadWebhookDlq();
            }
          }
        }, _L('DLQ 조회', 'Load DLQ')),
        el('button', {
          class: 'btn btn-g',
          type: 'button',
          onClick: function() {
            if (typeof window.retryWebhookDlq === 'function') {
              window.retryWebhookDlq();
            }
          }
        }, _L('전체 재시도', 'Retry all'))),
      el('div', { id: 'dlq-list', class: 'mt-8' })));
}

   
                                                                     
  
                                                        
                                                       
                                                                 
                                             
  
               
                                                
                                              
                   
   
function _registerAlertNavigationBlocker() {
  PCV.ui.setNavigationBlocker(function() {
    if (!_isAlertsActive() || !_alertConfigDirty()) return true;
    return PCV.ui.customConfirm(
      _L('적용하지 않은 변경', 'Unapplied changes'),
      _L(
        '이 화면을 떠나면 적용하지 않은 설정 변경이 사라집니다. 이동하시겠습니까?',
        'Leaving this page discards unapplied setting changes. Continue?'
      )
    );
  });
}

  
                  
                                                                                 
                                                                             
                                                                                
  
               
                                                                                 
                                                              
   
async function renderAlerts(b) {
  var el = PCV.uxlib.el;
  var navGen = _beginAlertsView(b);
  PCV.uxlib.clearEl(b);
  _alertsState.configRoot = el('section', {
    id: 'alert-config-region',
    'aria-labelledby': 'alert-config-title',
    'aria-busy': 'true'
  });
  _alertsState.historyRoot = el('section', {
    id: 'alert-history-region',
    'aria-labelledby': 'alert-history-title',
    'aria-busy': 'true'
  });
                                                                   
                                                                      
                           
  b.appendChild(PCV.uxlib.frag(
    HN.pagehead({ title: _L('알림', 'Alerts') }),
    _alertsState.configRoot,
    _alertsState.historyRoot,
    _buildAlertDlq()
  ));
  _registerAlertNavigationBlocker();
  _subscribeAlertHistoryFilter();
  await Promise.allSettled([
    _loadAlertConfig(navGen),
    _loadAlertHistory(navGen)
  ]);
}
window.renderAlerts = renderAlerts;

                              
                                                                  
                                                       
                                                            
                                                  
                          
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
  b.appendChild(frag(HN.pagehead({ title: '감사 로그 검색' }), el('div', { class: 'sg', style: 'grid-template-columns:1fr;margin-bottom:12px' }, hc)));
}
window.renderAudit = renderAudit;

                            
                                                                      
                                                  
                                                         
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
      el('code', { class: 'color-accent' }, 'pcvctl gpu metrics'), ' — GPU 메트릭 조회'));
  var chartDiv = el('div', { class: 'hc mb-14' },
    el('h4', null, _L('GPU 활용률', 'GPU Utilization')),
    el('canvas', { id: 'gpu-chart', width: '600', height: '200', style: 'max-width:100%' }),
    el('div', { class: 'stat-label mt-8' }, _L('GPU 메트릭은 nvidia-smi 또는 lspci 기반으로 수집됩니다.', 'GPU metrics collected via nvidia-smi or lspci.')));
  clearEl(b);
  b.appendChild(frag(HN.pagehead({ title: 'GPU 모니터링' }), grid2, cliCard, chartDiv));
                     
                                                                  
                                                          
                                                      
                                                              
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

                                                 
  
                                                                
                                                           
                                                  
                                                        
   
function _d2RequireResponse(response, label) {
  if (response && response.error) {
    var message = response.error.message || response.error.code || _L('알 수 없는 오류', 'Unknown error');
    throw new Error(label + ': ' + message);
  }
  if (response === null || response === undefined) {
    throw new Error(label + ': ' + _L('빈 응답', 'Empty response'));
  }
  return response;
}

function _d2MalformedResponse(label, detail) {
  throw new Error(label + ': ' + _L('잘못된 성공 응답', 'Malformed success response') +
    (detail ? ' (' + detail + ')' : ''));
}

function _d2RequireObjectData(response, label) {
  var data = unwrapData(_d2RequireResponse(response, label));
  if (!data || typeof data !== 'object' || Array.isArray(data)) {
    _d2MalformedResponse(label, _L('객체 필요', 'object required'));
  }
  return data;
}

function _d2RequireListData(response, label) {
  var data = unwrapData(_d2RequireResponse(response, label));
  if (!Array.isArray(data)) {
    _d2MalformedResponse(label, _L('배열 필요', 'array required'));
  }
  return data;
}

function _d2RequireBooleanField(data, field, label) {
  if (typeof data[field] !== 'boolean') {
    _d2MalformedResponse(label, field + ' ' + _L('불리언 필요', 'boolean required'));
  }
}

function _d2RequireFiniteFields(data, fields, label) {
  fields.forEach(function(field) {
    if (typeof data[field] !== 'number' || !Number.isFinite(data[field])) {
      _d2MalformedResponse(label, field + ' ' + _L('숫자 필요', 'number required'));
    }
  });
}

function _d2RequireRecord(value, label) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) {
    _d2MalformedResponse(label, _L('객체 필요', 'object required'));
  }
  return value;
}

function _d2RequireStringField(data, field, label) {
  if (typeof data[field] !== 'string' || data[field].trim() === '') {
    _d2MalformedResponse(label, field + ' ' + _L('문자열 필요', 'string required'));
  }
}

function _d2RequireOptionalStringField(data, field, label) {
  if (data[field] !== undefined && typeof data[field] !== 'string') {
    _d2MalformedResponse(label, field + ' ' + _L('문자열 필요', 'string required'));
  }
}

function _d2RequireNonnegativeIntegerField(data, field, label) {
  if (!Number.isInteger(data[field]) || data[field] < 0) {
    _d2MalformedResponse(label, field + ' ' + _L('0 이상의 정수 필요', 'non-negative integer required'));
  }
}

                                                    
                                                       
                                                               
                                                    
function _d2RequireDpdkDeviceItems(devices) {
  devices.forEach(function(device, index) {
    var label = 'DPDK devices[' + index + ']';
    device = _d2RequireRecord(device, label);
    _d2RequireStringField(device, 'pci_addr', label);
    _d2RequireStringField(device, 'status', label);
    _d2RequireOptionalStringField(device, 'driver', label);
  });
}

function _d2RequireSriovPfItems(pfs) {
  pfs.forEach(function(pf, index) {
    var label = 'SR-IOV physical_functions[' + index + ']';
    pf = _d2RequireRecord(pf, label);
    _d2RequireStringField(pf, 'name', label);
    _d2RequireNonnegativeIntegerField(pf, 'current_vfs', label);
    _d2RequireNonnegativeIntegerField(pf, 'max_vfs', label);
    _d2RequireBooleanField(pf, 'iommu_enabled', label);
    _d2RequireOptionalStringField(pf, 'pci_addr', label);
    _d2RequireOptionalStringField(pf, 'driver', label);
  });
}

function _d2RequireSriovVfItems(vfs) {
  vfs.forEach(function(vf, index) {
    var label = 'SR-IOV virtual functions[' + index + ']';
    vf = _d2RequireRecord(vf, label);
    _d2RequireStringField(vf, 'pf', label);
    _d2RequireNonnegativeIntegerField(vf, 'vf_index', label);
    _d2RequireStringField(vf, 'pci_addr', label);
    _d2RequireOptionalStringField(vf, 'driver', label);
    _d2RequireOptionalStringField(vf, 'mac', label);
  });
}

function _d2RenderLoadError(b, title, error, retry) {
  var el = PCV.uxlib.el;
  PCV.uxlib.clearEl(b);
  b.appendChild(PCV.uxlib.frag(
    HN.pagehead({
      title: title,
      desc: _L('현재 상태를 확인할 수 없어 작업을 잠갔습니다.', 'Actions are locked because the current state is unavailable.')
    }),
    el('div', { class: 'hc d2-load-error', role: 'alert', 'aria-live': 'assertive' },
      el('h3', { class: 'color-red' }, _L('상태 로드 실패', 'Failed to load state')),
      el('p', { class: 'color-muted text-12' }, error && error.message ? error.message : _L('요청이 실패했습니다.', 'The request failed.')),
      el('button', { class: 'btn', type: 'button', onClick: retry }, _L('다시 시도', 'Retry')))
  ));
}

function _d2ApplyRoleVisibility() {
                                                   
                                                       
                                         
  if (typeof applyRoleVisibility === 'function') {
    applyRoleVisibility(window.currentUser && window.currentUser.role);
  }
}

var _d2MutationLock = null;

function _d2FindActionButton(root, action) {
  if (!root || !action || !root.querySelectorAll) return null;
  return Array.from(root.querySelectorAll('[data-d2-action]')).find(function(button) {
    return button.getAttribute('data-d2-action') === action;
  }) || null;
}

function _d2MutationContextCurrent(token) {
  return Boolean(token && token.target && token.target.isConnected &&
    PCV.ui.navGen() === token.navGen && PCV.ui.renderTarget() === token.target);
}

                  
                                                                            
                                                             
                                                           
  
               
                                                 
                                                  
                           
function _d2BeginMutation(action) {
  if (_d2MutationLock) return null;
  var target = PCV.ui.renderTarget();
  var active = document.activeElement;
  var token = {
    action: action,
    navGen: PCV.ui.navGen(),
    target: target,
    opener: active && active.getAttribute && active.getAttribute('data-d2-action') === action
      ? active : _d2FindActionButton(target, action),
                                                             
                                                               
    buttonStates: target && target.querySelectorAll
      ? Array.from(target.querySelectorAll('.d2-operations button')).map(function(button) {
        return {
          node: button,
          disabled: button.disabled,
          ariaBusy: button.getAttribute('aria-busy')
        };
      }) : [],
    activated: false
  };
  _d2MutationLock = token;
  return token;
}

function _d2ActivateMutation(token) {
  if (!token || _d2MutationLock !== token || token.activated) return false;
  token.activated = true;
                                                             
                                                              
  token.buttonStates.forEach(function(state) {
    state.node.disabled = true;
    state.node.setAttribute('aria-busy', 'true');
  });
  return true;
}

function _d2EndMutation(token) {
  if (!token || _d2MutationLock !== token) return;
  token.buttonStates.forEach(function(state) {
    state.node.disabled = state.disabled;
    if (state.ariaBusy === null) state.node.removeAttribute('aria-busy');
    else state.node.setAttribute('aria-busy', state.ariaBusy);
  });
  var focusTarget = null;
  if (_d2MutationContextCurrent(token)) {
    focusTarget = token.opener && token.opener.isConnected
      ? token.opener : _d2FindActionButton(token.target, token.action);
  }
  _d2MutationLock = null;
  if (focusTarget && !focusTarget.disabled && typeof focusTarget.focus === 'function') {
    try { focusTarget.focus({ preventScroll: true }); } catch (e) { focusTarget.focus(); }
  }
}

async function _d2RunMutation(options) {
  var token = _d2BeginMutation(options.action);
  if (!token) return;
  try {
    if (!await PCV.ui.customConfirm(options.confirmTitle, options.confirmMessage)) return;
    if (!_d2ActivateMutation(token)) return;
    var response = await fetchPost(options.endpoint, options.body);
    if (response && response.error) {
      toast(options.failurePrefix + (response.error.message || ''), false);
      return;
    }
                               
                                                                  
                                                                        
                                                            
      
                   
                                                   
                                          
    var result = unwrapData(response);
    if (!result || typeof result !== 'object' || Array.isArray(result) ||
        result.status !== options.expectedStatus) {
      toast(options.failurePrefix + _L(
        '서버 성공 응답을 확인할 수 없습니다.',
        'Server success response could not be verified.'
      ), false);
      return;
    }
    toast(options.successToast);
    addEvt(options.eventText);
                                                                      
                                                               
    if (_d2MutationContextCurrent(token)) {
      await options.render(token.target);
      if (_d2MutationContextCurrent(token) && typeof options.afterRender === 'function') {
        options.afterRender(token.target);
      }
    }
  } catch (e) {
    toast(e.message, false);
  } finally {
    _d2EndMutation(token);
  }
}

function _d2RestoreSriovSelection(target, vm, pci) {
  if (!target) return;
  var vmInput = target.querySelector('#sriov-vm');
  var select = target.querySelector('#sriov-vf-pci');
  if (vmInput) vmInput.value = vm;
  if (!select) return;
  var option = Array.from(select.options || []).find(function(candidate) { return candidate.value === pci; });
  if (!option) return;
  select.value = pci;
  var attach = _d2FindActionButton(target, 'sriov-attach');
  if (attach) attach.disabled = option.getAttribute('data-iommu-ready') !== 'true';
}

                         
   
                                                                  
  
                                                     
                                                                        
                                               
                                                                       
                                                               
                                              
   
async function renderDpdk(b) {
  showSkeleton(b);
  try {
    const [status, list, hugepage] = await Promise.all([
      fetchGet(EP.DPDK_STATUS()),
      fetchGet(EP.DPDK_LIST()),
      fetchGet(EP.DPDK_HUGEPAGE())
    ]);
    const sd = _d2RequireObjectData(status, 'DPDK status');
    const dl = _d2RequireListData(list, 'DPDK devices');
    const hp = _d2RequireObjectData(hugepage, 'HugePages');
    _d2RequireBooleanField(sd, 'available', 'DPDK status');
    _d2RequireDpdkDeviceItems(dl);
    _d2RequireFiniteFields(hp, [
      'hugepage_2m_free', 'hugepage_2m_total',
      'hugepage_1g_free', 'hugepage_1g_total'
    ], 'HugePages');
    var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
    var freeRatio = function(free, total) {
      if (total <= 0) return null;
      return Math.max(0, Math.min(100, free / total * 100));
    };
    var freeState = function(freePct) {
      if (freePct === null) return 'idle';
      if (freePct <= 10) return 'crit';
      if (freePct <= 25) return 'warn';
      return 'ok';
    };
    var metric = function(size, free, total) {
      var freePct = freeRatio(free, total);
      var state = freeState(freePct);
      return {
        state: state,
        row: HN.row(size, el('div', null,
          el('div', { class: 'stat-label' }, 'Free ' + free + ' / Total ' + total),
          freePct === null
            ? HN.statusPill('idle', 'NOT CONFIGURED')
            : HN.gauge({ value: 100 - freePct, warn: 75, crit: 90, inline: true })))
      };
    };
    var num = function(value) {
      value = Number(value);
      return Number.isFinite(value) ? value : 0;
    };
    var twoM = metric('2M', num(hp.hugepage_2m_free), num(hp.hugepage_2m_total));
    var oneG = metric('1G', num(hp.hugepage_1g_free), num(hp.hugepage_1g_total));
    var capacityState = HN.statusReduce([twoM.state, oneG.state]);
    var capacityLabel = capacityState === 'ok' ? 'OK'
      : capacityState === 'warn' ? 'WARN'
      : capacityState === 'crit' ? 'CRIT' : 'NOT CONFIGURED';
    var statusCard = HN.card(_L('DPDK 상태', 'DPDK Status'), [
      HN.row(_L('가용성', 'Available'), HN.statusPill(sd.available ? 'ok' : 'idle', sd.available ? 'AVAILABLE' : 'UNAVAILABLE')),
      HN.row('PMD CPU Mask', sd.pmd_cpu_mask || '-'),
      HN.row('Socket Mem', sd.socket_mem || '-')
    ]);
    var hugeCard = HN.card('HugePages', [
      HN.row(_L('용량', 'Capacity'), HN.statusPill(capacityState, capacityLabel)),
      twoM.row,
      oneG.row
    ]);
    var boundCard = HN.card(_L('바인딩된 NIC', 'Bound NICs'), el('div', { class: 'stat-lg' }, dl.length));
    var parts = [
      HN.pagehead({
        title: 'DPDK — Data Plane Development Kit',
        desc: _L('가속 데이터 경로의 준비 상태와 NIC 바인딩을 관리합니다.', 'Review accelerator readiness and manage NIC bindings.')
      }),
      HN.section(_L('운영 상태', 'Operating status')),
      HN.grid(3, statusCard, hugeCard, boundCard)
    ];
    parts.push(el('div', { class: 'dpdk-device-heading justify-between items-center mt-12' },
      el('h3', null, _L('바인딩된 NIC', 'Bound NICs')),
      HN.statusPill(dl.length ? 'ok' : 'idle', String(dl.length))));
    if (Array.isArray(dl) && dl.length > 0) {
      parts.push(el('table', { class: 'table-sticky d2-data-table' },
        el('thead', null, el('tr', null,
          el('th', null, _L('PCI 주소', 'PCI Address')),
          el('th', null, _L('드라이버', 'Driver')),
          el('th', null, _L('상태', 'Status')))),
        el('tbody', null, dl.map(function(d) {
          return el('tr', null,
            el('td', null, el('code', null, d.pci_addr)),
            el('td', null, d.driver || '-'),
            el('td', null, d.status || '-'));
        }))));
    } else {
      parts.push(emptyStatePro({
        icon: '—',
        title: _L('바인딩된 NIC가 없습니다', 'No bound NICs'),
        desc: sd.available
          ? _L('관리 네트워크가 아닌 데이터 NIC만 검증 후 바인딩하세요.', 'Bind only a verified data NIC that is not used for management traffic.')
          : _L('DPDK가 비활성 상태입니다. 호스트 설정을 확인하세요.', 'DPDK is unavailable. Review the host configuration.')
      }));
    }
    var unavailable = !sd.available;
    parts.push(el('div', { class: 'hc d2-readonly-note', 'data-role': 'VIEWER,OPERATOR' },
      HN.statusPill('idle', 'ADMIN ONLY'),
      el('span', { class: 'color-muted text-12 ml-8' },
        _L('DPDK 상태는 읽을 수 있지만 NIC 바인딩 변경은 관리자만 실행할 수 있습니다.', 'You can review DPDK state, but only an administrator can change NIC bindings.'))));
    parts.push(el('section', { class: 'd2-operations', 'data-role': 'ADMIN' },
      HN.section(_L('DPDK 작업', 'DPDK Operations')),
      unavailable ? el('p', { class: 'color-muted text-12' }, _L('DPDK가 비활성 상태이므로 새 바인딩은 잠겼습니다. 기존 NIC 언바인딩은 복구를 위해 계속 사용할 수 있습니다.', 'Binding is locked while DPDK is unavailable. Unbind remains available for recovery.')) : null,
      el('div', { class: 'sg grid-2' },
        HN.card(_L('NIC를 DPDK에 바인딩', 'Bind NIC to DPDK'), [
          el('div', { class: 'fr' }, el('label', { for: 'dpdk-pci' }, _L('PCI 주소', 'PCI Address')), el('input', { id: 'dpdk-pci', placeholder: '0000:03:00.0', class: 'w-full', disabled: unavailable ? 'disabled' : null })),
          el('div', { class: 'fr' }, el('label', { for: 'dpdk-drv' }, _L('드라이버', 'Driver')), el('input', { id: 'dpdk-drv', value: 'vfio-pci', class: 'w-full', disabled: unavailable ? 'disabled' : null })),
          el('button', { class: 'btn btn-r', type: 'button', onclick: 'dpdkBind()', 'data-d2-action': 'dpdk-bind', disabled: unavailable ? 'disabled' : null }, _L('바인딩', 'Bind'))
        ]),
        HN.card(_L('NIC 언바인딩', 'Unbind NIC'), [
          el('div', { class: 'fr' }, el('label', { for: 'dpdk-unbind-pci' }, _L('PCI 주소', 'PCI Address')), el('input', { id: 'dpdk-unbind-pci', placeholder: '0000:03:00.0', class: 'w-full' })),
          el('button', { class: 'btn btn-r', type: 'button', onclick: 'dpdkUnbind()', 'data-d2-action': 'dpdk-unbind' }, _L('언바인딩', 'Unbind'))
        ]))));
    clearEl(b);
    b.appendChild(frag(parts));
    _d2ApplyRoleVisibility();
  } catch (e) {
    _d2RenderLoadError(b, 'DPDK — Data Plane Development Kit', e, function() { renderDpdk(b); });
  }
}

   
                                                         
                                        
  
                  
                                                               
                                                   
                                                  
                                            
                                                      
  
               
                                                         
                                                       
                                                        
                                                       
                                          
   
async function dpdkBind() {
  var pci = document.getElementById('dpdk-pci')?.value;
  var drv = document.getElementById('dpdk-drv')?.value || 'vfio-pci';
  if (!pci) { toast(t('msg.name_required'), false); return; }
  return _d2RunMutation({
    action: 'dpdk-bind',
    confirmTitle: _L('DPDK NIC 바인딩 확인', 'Confirm DPDK NIC binding'),
    confirmMessage: _L('이 NIC가 커널 네트워크에서 분리됩니다. 관리 트래픽에 사용 중이 아닌지 확인하세요.\n', 'This NIC will leave the kernel network stack. Confirm it is not carrying management traffic.\n') + pci,
    endpoint: EP.DPDK_BIND(),
    expectedStatus: 'bound',
    body: { pci_addr: pci, driver: drv },
    failurePrefix: 'Bind failed: ',
    successToast: 'DPDK bind: ' + pci,
    eventText: 'DPDK bind ' + pci,
    render: renderDpdk
  });
}
                                                            
  
               
                                                      
                                                
                        
async function dpdkUnbind() {
  var pci = document.getElementById('dpdk-unbind-pci')?.value;
  if (!pci) { toast(t('msg.name_required'), false); return; }
  return _d2RunMutation({
    action: 'dpdk-unbind',
    confirmTitle: _L('DPDK NIC 언바인딩 확인', 'Confirm DPDK NIC unbinding'),
    confirmMessage: _L('이 NIC를 사용하는 DPDK 워크로드의 네트워크가 즉시 끊길 수 있습니다.\n', 'DPDK workloads using this NIC may immediately lose network connectivity.\n') + pci,
    endpoint: EP.DPDK_UNBIND(),
    expectedStatus: 'unbound',
    body: { pci_addr: pci },
    failurePrefix: 'Unbind failed: ',
    successToast: 'DPDK unbind: ' + pci,
    eventText: 'DPDK unbind ' + pci,
    render: renderDpdk
  });
}

window.renderDpdk = renderDpdk;
window.dpdkBind = dpdkBind;
window.dpdkUnbind = dpdkUnbind;

                           
                                                   
                                                                
                                        
async function renderSriov(b) {
  showSkeleton(b);
  try {
    const [status, list] = await Promise.all([
      fetchGet(EP.SRIOV_STATUS()),
      fetchGet(EP.SRIOV_LIST())
    ]);
    const sd = _d2RequireObjectData(status, 'SR-IOV status');
    const vfs = _d2RequireListData(list, 'SR-IOV virtual functions');
    _d2RequireBooleanField(sd, 'available', 'SR-IOV status');
    if (!Array.isArray(sd.physical_functions)) {
      _d2MalformedResponse('SR-IOV status', 'physical_functions ' + _L('배열 필요', 'array required'));
    }
    _d2RequireSriovPfItems(sd.physical_functions);
    _d2RequireSriovVfItems(vfs);
    var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
    var nicsCard = HN.card(_L('SR-IOV NIC', 'SR-IOV NICs'), [
      HN.row(_L('가용성', 'Available'), HN.statusPill(sd.available ? 'ok' : 'idle', sd.available ? 'AVAILABLE' : 'UNAVAILABLE')),
      HN.row(_L('물리 기능', 'Physical Functions'), Array.isArray(sd.physical_functions) ? sd.physical_functions.length : (sd.nic_count || 0))
    ], 'sriov-status-card');
    var pfs = Array.isArray(sd.physical_functions) ? sd.physical_functions : [];
    var iommuReadyByPf = Object.create(null);
    pfs.forEach(function(pf) {
      if (pf && pf.name) iommuReadyByPf[pf.name] = pf.iommu_enabled === true;
    });
    var vfsCard = HN.card(_L('활성 VF', 'Active VFs'), el('div', { class: 'stat-lg' }, vfs.length));
    var parts = [
      HN.pagehead({
        title: 'SR-IOV — Single Root I/O Virtualization',
        desc: _L('PF/VF 준비 상태를 확인하고 VM 패스스루 연결을 관리합니다.', 'Review PF/VF readiness and manage VM passthrough attachments.')
      }),
      HN.section(_L('운영 상태', 'Operating status')),
      HN.grid(2, nicsCard, vfsCard)
    ];
    parts.push(el('div', { class: 'sriov-pf-heading justify-between items-center mt-12' },
      el('h3', null, _L('물리 기능', 'Physical Functions')),
      HN.statusPill(pfs.length ? 'ok' : 'idle', String(pfs.length))));
    if (pfs.length) {
      parts.push(el('table', { class: 'table-sticky card-mobile sriov-pf-table' },
        el('thead', null, el('tr', null,
          el('th', null, _L('이름', 'Name')),
          el('th', null, _L('PCI 주소', 'PCI Address')),
          el('th', null, _L('활성 / 최대 VF', 'Active / Max VFs')),
          el('th', null, _L('드라이버', 'Driver')),
          el('th', null, 'IOMMU'))),
        el('tbody', null, pfs.map(function(pf) {
          return el('tr', null,
            el('td', { 'data-label': _L('이름', 'Name') }, pf.name || '-'),
            el('td', { 'data-label': _L('PCI 주소', 'PCI Address') }, el('code', null, pf.pci_addr || '-')),
            el('td', { 'data-label': _L('활성 / 최대 VF', 'Active / Max VFs') },
              String(Number.isFinite(Number(pf.current_vfs)) ? Number(pf.current_vfs) : 0) + ' / ' +
              String(Number.isFinite(Number(pf.max_vfs)) ? Number(pf.max_vfs) : 0)),
            el('td', { 'data-label': _L('드라이버', 'Driver') }, pf.driver || '-'),
            el('td', { 'data-label': 'IOMMU' }, HN.statusPill(pf.iommu_enabled ? 'ok' : 'warn', pf.iommu_enabled ? 'ENABLED' : 'DISABLED')));
        }))));
    } else {
      parts.push(el('p', { class: 'color-muted text-12' },
        _L('SR-IOV를 지원하는 물리 NIC가 없습니다.', 'No physical NIC supports SR-IOV.')));
    }
    parts.push(el('div', { class: 'sriov-device-heading justify-between items-center mt-12' },
      el('h3', null, _L('활성 VF', 'Active VFs')),
      HN.statusPill(vfs.length ? 'ok' : 'idle', String(vfs.length))));
    if (Array.isArray(vfs) && vfs.length > 0) {
      parts.push(el('table', { class: 'table-sticky d2-data-table' },
        el('thead', null, el('tr', null,
          el('th', null, 'PF'),
          el('th', null, 'VF Index'),
          el('th', null, _L('PCI 주소', 'PCI Address')),
          el('th', null, _L('드라이버', 'Driver')),
          el('th', null, 'MAC'))),
        el('tbody', null, vfs.map(function(v) {
          return el('tr', null,
            el('td', null, v.pf || '-'),
            el('td', null, v.vf_index ?? '-'),
            el('td', null, el('code', null, v.pci_addr || '-')),
            el('td', null, v.driver || '-'),
            el('td', null, v.mac || '-'));
        }))));
    } else {
      parts.push(emptyStatePro({
        icon: '—',
        title: _L('활성 VF가 없습니다', 'No active VFs'),
        desc: sd.available
          ? _L('관리자가 PF에 VF를 활성화하면 이 목록에서 연결 대상을 선택할 수 있습니다.', 'After an administrator enables VFs on a PF, attachment targets appear here.')
          : _L('SR-IOV 지원 NIC 또는 IOMMU 설정을 확인하세요.', 'Review SR-IOV NIC support and IOMMU configuration.')
      }));
    }
    var unavailable = !sd.available;
    var selectableVfs = vfs.filter(function(v) {
      return v && v.pf && Number.isInteger(Number(v.vf_index)) && v.pci_addr;
    });
    var attachableVfs = selectableVfs.filter(function(v) { return iommuReadyByPf[v.pf] === true; });
    var vfOptions = [el('option', { value: '' }, selectableVfs.length
      ? _L('VF 선택', 'Select a VF')
      : _L('활성 VF 없음', 'No active VFs'))].concat(selectableVfs.map(function(v) {
      return el('option', {
        value: v.pci_addr,
        'data-pf': v.pf,
        'data-vf-index': String(v.vf_index),
        'data-iommu-ready': iommuReadyByPf[v.pf] === true ? 'true' : 'false'
      }, v.pf + ' · VF ' + v.vf_index + ' · ' + v.pci_addr +
        (iommuReadyByPf[v.pf] === true ? '' : ' · IOMMU ' + _L('미준비', 'NOT READY')));
    }));
    var operationsDisabled = unavailable || selectableVfs.length === 0;
    parts.push(el('div', { class: 'hc d2-readonly-note', 'data-role': 'VIEWER,OPERATOR' },
      HN.statusPill('idle', 'ADMIN ONLY'),
      el('span', { class: 'color-muted text-12 ml-8' },
        _L('SR-IOV 상태는 읽을 수 있지만 네트워크 변경은 관리자만 실행할 수 있습니다.', 'You can review SR-IOV state, but only an administrator can change networking.'))));
    parts.push(el('section', { class: 'd2-operations', 'data-role': 'ADMIN' },
      HN.section(_L('SR-IOV 작업', 'SR-IOV Operations')),
      unavailable ? el('p', { class: 'color-muted text-12' }, _L('SR-IOV가 비활성 상태이므로 작업을 사용할 수 없습니다.', 'Actions are unavailable while SR-IOV is disabled.')) : null,
      !unavailable && selectableVfs.length && !attachableVfs.length
        ? el('p', { class: 'color-yellow text-12', role: 'status' },
          _L('IOMMU가 활성화된 PF의 VF가 없어 연결을 잠갔습니다. 기존 VF 분리는 계속 가능합니다.', 'Attach is locked because no VF belongs to an IOMMU-enabled PF. Existing VFs can still be detached.'))
        : null,
      el('div', { class: 'sg grid-2' },
        HN.card(_L('VF 활성화', 'Enable VFs'), [
          el('div', { class: 'fr' }, el('label', { for: 'sriov-pf' }, _L('물리 NIC (PF)', 'Physical NIC (PF)')), el('input', { id: 'sriov-pf', placeholder: 'enp3s0f0', class: 'w-full', disabled: unavailable ? 'disabled' : null })),
          el('div', { class: 'fr' }, el('label', { for: 'sriov-numvf' }, _L('VF 개수', 'Number of VFs')), el('input', { id: 'sriov-numvf', type: 'number', value: '4', min: '1', max: '64', class: 'w-80', disabled: unavailable ? 'disabled' : null })),
          el('button', { class: 'btn btn-r', type: 'button', onclick: 'sriovEnable()', 'data-d2-action': 'sriov-enable', disabled: unavailable ? 'disabled' : null }, _L('활성화', 'Enable')), ' ',
          el('button', { class: 'btn btn-r', type: 'button', onclick: 'sriovDisable()', 'data-d2-action': 'sriov-disable', disabled: unavailable ? 'disabled' : null }, _L('비활성화', 'Disable'))
        ]),
        HN.card(_L('VF를 VM에 연결', 'Attach VF to VM'), [
          el('div', { class: 'fr' }, el('label', { for: 'sriov-vm' }, _L('VM 이름', 'VM Name')), el('input', { id: 'sriov-vm', placeholder: 'web-prod', class: 'w-full', disabled: operationsDisabled ? 'disabled' : null })),
          el('div', { class: 'fr' }, el('label', { for: 'sriov-vf-pci' }, _L('가상 기능 (PF / VF / PCI)', 'Virtual Function (PF / VF / PCI)')), el('select', {
            id: 'sriov-vf-pci', class: 'w-full', disabled: operationsDisabled ? 'disabled' : null,
            onChange: function(event) {
              var option = event.target.selectedOptions && event.target.selectedOptions[0];
              var button = document.querySelector('[data-d2-action="sriov-attach"]');
              if (button) button.disabled = !option || option.getAttribute('data-iommu-ready') !== 'true';
            }
          }, vfOptions)),
          el('button', { class: 'btn btn-r', type: 'button', onclick: 'sriovAttach()', 'data-d2-action': 'sriov-attach', disabled: 'disabled' }, _L('연결', 'Attach')), ' ',
          el('button', { class: 'btn btn-r', type: 'button', onclick: 'sriovDetach()', 'data-d2-action': 'sriov-detach', disabled: operationsDisabled ? 'disabled' : null }, _L('분리', 'Detach'))
        ]))));
    clearEl(b);
    b.appendChild(frag(parts));
    _d2ApplyRoleVisibility();
  } catch (e) {
    _d2RenderLoadError(b, 'SR-IOV — Single Root I/O Virtualization', e, function() { renderSriov(b); });
  }
}

  
                                                                          
                                              
                                                          
  
                  
                                                      
                                                     
                                                   
                                             
  
                                     
                                                           
                                                           
                  
                                                                
                                                            
                   
                                                         
                                                        
                                                      
                                                   
                                                           
   
async function sriovEnable() {
  var pf = document.getElementById('sriov-pf')?.value;
  var num = parseInt(document.getElementById('sriov-numvf')?.value) || 4;
  if (!pf) { toast(t('msg.name_required'), false); return; }
  return _d2RunMutation({
    action: 'sriov-enable',
    confirmTitle: _L('SR-IOV VF 활성화 확인', 'Confirm SR-IOV VF enablement'),
    confirmMessage: _L('VF 구성이 다시 생성되며 기존 VM 네트워크가 끊길 수 있습니다.\n', 'The VF configuration will be recreated and existing VM networking may be interrupted.\n') + pf + ' · ' + num + ' VFs',
    endpoint: EP.SRIOV_ENABLE(),
    expectedStatus: 'enabled',
    body: { pf: pf, num_vfs: num },
    failurePrefix: 'Enable failed: ',
    successToast: 'SR-IOV enabled: ' + pf + ' (' + num + ' VFs)',
    eventText: 'SR-IOV enable ' + pf,
    render: renderSriov
  });
}
                                                        
async function sriovDisable() {
  var pf = document.getElementById('sriov-pf')?.value;
  if (!pf) { toast(t('msg.name_required'), false); return; }
  return _d2RunMutation({
    action: 'sriov-disable',
    confirmTitle: _L('SR-IOV VF 비활성화 확인', 'Confirm SR-IOV VF disablement'),
    confirmMessage: _L('이 PF의 모든 VF가 제거되어 연결된 VM의 네트워크가 끊길 수 있습니다.\n', 'Every VF on this PF will be removed and attached VMs may lose network connectivity.\n') + pf,
    endpoint: EP.SRIOV_DISABLE(),
    expectedStatus: 'disabled',
    body: { pf: pf },
    failurePrefix: 'Disable failed: ',
    successToast: 'SR-IOV disabled: ' + pf,
    eventText: 'SR-IOV disable ' + pf,
    render: renderSriov
  });
}
                                                       
async function sriovAttach() {
  var vm = document.getElementById('sriov-vm')?.value;
  var vfSelect = document.getElementById('sriov-vf-pci');
  var option = vfSelect?.selectedOptions?.[0];
  var pci = vfSelect?.value;
  var pf = option?.getAttribute('data-pf');
  var vfIndex = Number(option?.getAttribute('data-vf-index'));
  if (!vm || !pci || !pf || !Number.isInteger(vfIndex)) { toast(t('msg.name_required'), false); return; }
  if (option.getAttribute('data-iommu-ready') !== 'true') {
    toast(_L('IOMMU가 활성화된 PF의 VF만 연결할 수 있습니다.', 'Only a VF on an IOMMU-enabled PF can be attached.'), false);
    return;
  }
  return _d2RunMutation({
    action: 'sriov-attach',
    confirmTitle: _L('SR-IOV VF 연결 확인', 'Confirm SR-IOV VF attachment'),
    confirmMessage: _L('선택한 VF가 VM에 직접 연결됩니다. 다른 워크로드가 사용 중이 아닌지 확인하세요.\n', 'The selected VF will be attached directly to the VM. Confirm no other workload is using it.\n') + pf + ' · VF ' + vfIndex + ' → ' + vm,
    endpoint: EP.SRIOV_ATTACH(),
    expectedStatus: 'attached',
    body: { vm_name: vm, pf: pf, vf_index: vfIndex },
    failurePrefix: 'Attach failed: ',
    successToast: 'VF attached to ' + vm,
    eventText: 'SR-IOV attach ' + pci + ' \u2192 ' + vm,
    render: renderSriov,
    afterRender: function(target) { _d2RestoreSriovSelection(target, vm, pci); }
  });
}
                                                       
async function sriovDetach() {
  var vm = document.getElementById('sriov-vm')?.value;
  var pci = document.getElementById('sriov-vf-pci')?.value;
  if (!vm || !pci) { toast(t('msg.name_required'), false); return; }
  return _d2RunMutation({
    action: 'sriov-detach',
    confirmTitle: _L('SR-IOV VF 분리 확인', 'Confirm SR-IOV VF detachment'),
    confirmMessage: _L('VM에서 해당 네트워크 인터페이스가 즉시 사라집니다.\n', 'The network interface will immediately disappear from the VM.\n') + pci + ' · ' + vm,
    endpoint: EP.SRIOV_DETACH(),
    expectedStatus: 'detached',
    body: { vm_name: vm, pci_addr: pci },
    failurePrefix: 'Detach failed: ',
    successToast: 'VF detached from ' + vm,
    eventText: 'SR-IOV detach ' + pci,
    render: renderSriov,
    afterRender: function(target) { _d2RestoreSriovSelection(target, vm, pci); }
  });
}

window.renderSriov = renderSriov;
window.sriovEnable = sriovEnable;
window.sriovDisable = sriovDisable;
window.sriovAttach = sriovAttach;
window.sriovDetach = sriovDetach;

                  
   
                                                          
  
                                                                 
                                                     
                                                                
                                                 
       
  
                                                    
                                     
                                                        
                                                             
                                                          
   
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
    const d = await fetchGet(EP.DPDK_STATUS());
    const dd = _d2RequireObjectData(d, 'DPDK status');
    _d2RequireBooleanField(dd, 'available', 'DPDK status');
    const s = await fetchGet(EP.SRIOV_STATUS());
    const sd = _d2RequireObjectData(s, 'SR-IOV status');
    _d2RequireBooleanField(sd, 'available', 'SR-IOV status');
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
    var heading = HN.pagehead({
      title: _L('호스트 상태', 'Host Health'),
      desc: metricsNote
    });
    var grid = el('div', { class: 'sg grid-2 host-ops-grid' },
      HN.card('CPU', [el('div', { class: 'stat-md' }, cpu.toFixed(1) + '%'), HN.gauge({ value: +cpu.toFixed(1), warn: 80, crit: 95, inline: true }), HN.row('Temp', temp.toFixed(1) + '°C'), HN.row('Load', load1.toFixed(2))]),
      HN.card('Memory', [el('div', { class: 'stat-md' }, mem.toFixed(1) + '%'), HN.gauge({ value: +mem.toFixed(1), warn: 80, crit: 95, inline: true }), HN.row(_L('상태', 'State'), HN.statusPill(mem >= 80 ? 'warn' : 'ok', mem >= 80 ? _L('주의', 'Watch') : _L('안정', 'Stable')))]),
      HN.card('Disk', [el('div', { class: 'stat-md' }, disk.toFixed(1) + '%'), HN.gauge({ value: +disk.toFixed(1), warn: 80, crit: 90, inline: true }), HN.row(_L('권장 조치', 'Recommended action'), disk >= 80 ? _L('정리 필요', 'Cleanup needed') : _L('여유 있음', 'Healthy margin'))]),
      HN.card(_L('가속 기능', 'Acceleration'), [HN.row('DPDK', HN.statusPill(dd.available ? 'ok' : 'idle', dd.available ? 'ON' : 'OFF')), HN.row('SR-IOV', HN.statusPill(sd.available ? 'ok' : 'idle', sd.available ? 'ON' : 'OFF'))]),
      HN.card(_L('운영 메모', 'Operations note'), [HN.row(_L('호스트 모드', 'Host mode'), HN.statusPill('ok', _L('단일 노드', 'Single node'))), HN.row(_L('수집 기준', 'Collection'), _L('실시간 메트릭', 'Live metrics')), HN.row(_L('우선순위', 'Priority'), priority)]),
      HN.card(_L('현재 조치', 'Current action'), el('p', { class: 'color-muted text-12', style: 'line-height:1.7;margin:0' }, nextAction)));
    clearEl(b);
    b.appendChild(frag(heading, grid));
  } catch (e) {
    if(_DEBUG) console.warn('renderHost:', e.message);
    PCV.uxlib.renderLoadError(b, {
      title: _L('호스트 상태', 'Host Health'),
      message: e.message,
      retry: function() { renderHost(b); }
    });
  }
}
window.renderHost = renderHost;

                              
   
                                  
  
               
                                                    
                                                         
                                                 
                                                        
                                     
  
                  
                                                        
                                                   
                             
   
function renderHeatmap(b) {
  showSkeleton(b);
                                                             
  fetchGet(EP.VM_LIST()).then(function(r) {
    var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
    var vms = unwrapList(r);
    if (!vms || vms.length === 0) {
      clearEl(b);
      b.appendChild(frag(HN.pagehead({ title: _L('리소스 히트맵', 'Resource Heatmap') }),
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
    b.appendChild(frag(HN.pagehead({ title: _L('리소스 히트맵', 'Resource Heatmap') }), tableWrap, legend));
  }).catch(function(e) {
    PCV.uxlib.clearEl(b);
    b.appendChild(PCV.uxlib.frag(HN.pagehead({ title: _L('리소스 히트맵', 'Resource Heatmap') }),
      PCV.uxlib.el('p', { class: 'color-muted' }, _L('로드 실패', 'Failed to load') + ': ' + (e.message || ''))));
  });
}
window.renderHeatmap = renderHeatmap;
window.loadDeepHealth = loadDeepHealth;

                                    
                                                       
                                                     
                        
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
          var _navGen = PCV.ui.navGen();
          const r = await fetchPost(EP.ALERT_SILENCE(), { metric: metric, duration_min: dur, reason: reason });
          if (r && r.error) { toast(r.error.message || _L('실패', 'Failed'), false); return; }
          toast(_L('음소거 적용', 'Silence applied'), 's');
          closeModal();
          if (PCV.ui.navGen() === _navGen) renderAlertSilences(PCV.ui.renderTarget());
        } catch(e) { toast(_L('실패', 'Failed'), false); }
      } }, _L('적용', 'Apply')),
      ' ',
      mk('button', { class: 'btn btn-r', onclick: 'closeModal()' }, t('btn.cancel')))
  ]);
}

                                  
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
                                                        
                                                           
                                                       
                                                      
                                              
    var cur = await fetchGet(EP.ALERTS_CONFIG());
    if (cur && cur.error) { toast(cur.error.message || _L('설정 로드 실패', 'Failed to load config'), false); return; }
    var loaded = unwrapData(cur);
    if (!_validAlertConfig(loaded)) { toast(_L('설정 형식이 올바르지 않습니다', 'Invalid alert config received'), false); return; }
    cfg.expected_revision = loaded.config_revision;
    const r = await fetchPut(EP.ALERTS_CONFIG(), cfg);
    if (r && r.error) { toast(r.error.message || _L('실패', 'Failed'), false); return; }
    toast(_L('라우팅 설정 저장 완료', 'Alert routing saved'), 's');
  } catch(e) { toast(_L('실패', 'Failed'), false); }
}

                                  
                                                                
                                               
                                                             
                                                      
                                                          
                                                         
async function renderPoolInfo(b) {
  showSkeleton(b);
  try {
    var d = _d2RequireObjectData(await fetchGet(EP.POOL_CONNINFO()), 'Connection pool');
    _d2RequireFiniteFields(d, ['idle', 'total', 'max', 'wait_avg_sec'], 'Connection pool');
    var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
    var num = function(value) {
      value = Number(value);
      return Number.isFinite(value) ? value : 0;
    };
    var idle = num(d.idle);
    var total = num(d.total);
    var max = num(d.max);
    var active = Math.max(0, total - idle);
    var activePct = max > 0 ? Math.min(100, active / max * 100) : 0;
    var activeState = max <= 0 ? 'idle' : activePct >= 80 ? 'crit' : activePct >= 60 ? 'warn' : 'ok';
    var activeLabel = activeState === 'ok' ? 'OK'
      : activeState === 'warn' ? 'WARN'
      : activeState === 'crit' ? 'CRIT' : 'NOT CONFIGURED';
    var activeKpi = el('div', { class: 'pool-kpi pool-kpi-active' },
      el('div', { class: 'justify-between' },
        el('div', { class: 'flex items-center gap-8' },
          HN.statusDot(activeState),
          el('div', { class: 'stat-label' }, _L('활성', 'Active'))),
        HN.statusPill(activeState, max <= 0 ? activeLabel : activeLabel + ' · ' + Math.round(activePct) + '%')),
      el('div', { class: 'stat-md' }, active),
      max > 0 ? HN.gauge({ value: activePct, warn: 60, crit: 80, inline: true }) : null);
    var cardContents = [
      el('div', { class: 'sg grid-3' },
        el('div', { class: 'pool-kpi pool-kpi-idle' },
          el('div', { class: 'flex items-center gap-8' },
            HN.statusDot('ok', { glow: true }),
            el('div', { class: 'stat-label' }, _L('유휴', 'Idle'))),
          el('div', { class: 'stat-md' }, idle)),
        activeKpi,
        el('div', { class: 'pool-kpi pool-kpi-max' },
          el('div', { class: 'flex items-center gap-8' },
            HN.statusDot('idle'),
            el('div', { class: 'stat-label' }, _L('최대', 'Max'))),
          el('div', { class: 'stat-md' }, max)))
    ];
    var waitMs = d.wait_avg_sec * 1000;
    if (Number.isFinite(d.wait_avg_sec)) {
      cardContents.push(el('div', { class: 'pool-kpi-wait' }, _L('평균 대기', 'Avg wait') + ' ' + waitMs.toFixed(1) + 'ms'));
    }
    var parts = [
      HN.pagehead({
        title: _L('커넥션 풀', 'Connection Pool'),
        desc: _L('libvirt 연결의 유휴·활성·최대 용량과 평균 대기 시간을 확인합니다.', 'Review idle, active, and maximum libvirt connections with average wait time.')
      }),
      HN.card('', cardContents, 'pool-kpi-card')
    ];
    clearEl(b);
    b.appendChild(frag(parts));
  } catch(e) {
    _d2RenderLoadError(b, _L('커넥션 풀', 'Connection Pool'), e, function() { renderPoolInfo(b); });
  }
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
    var rows = actions.map(function(a) {
      var flexKids = [mk('strong', null, a.action || 'unknown')];
      if (a.target) flexKids.push(' — ' + a.target);
      if (a.reason) flexKids.push(' ', mk('span', { class: 'stat-label' }, '(' + a.reason + ')'));
                                              
                                                                         
                                            
                                                           
                                                             
      var hasId = (a.id !== undefined && a.id !== null);
      var mkBtn = function(cls, fn, label) {
        var attrs = { class: cls, 'data-role': 'ADMIN' };
        if (hasId) attrs.onclick = fn + '(' + Number(a.id) + ')';
        else { attrs.disabled = 'disabled'; attrs.title = _L('서버가 액션 id 를 주지 않았습니다', 'Server did not provide an action id'); }
        return mk('button', attrs, label);
      };
      return mk('div', { class: 'hc mb-8 flex items-center gap-10', style: 'padding:8px 12px' },
        mk('span', { class: 'color-yellow' }, '⚠'), ' ',
        mk('span', { class: 'flex-1' }, flexKids),
        mkBtn('btn btn-g btn-sm', 'healingApprove', _L('승인', 'Approve')),
        mkBtn('btn btn-r btn-sm', 'healingReject', _L('거절', 'Reject')));
    });
    PCV.uxlib.clearEl(el);
    el.appendChild(PCV.uxlib.frag(rows));
    if (typeof applyRoleVisibility === 'function') applyRoleVisibility(window.currentUser && window.currentUser.role);
  } catch (e) {
    var el2 = document.getElementById('healing-pending-list');
    if (el2) { PCV.uxlib.clearEl(el2); el2.appendChild(PCV.uxlib.el('div', { class: 'stat-label' }, e.message)); }
  }
}

async function healingApprove(actionId) {
  try {
    var r = await fetchPost(EP.RPC(), { jsonrpc:'2.0', method:'ai.healing.approve', params:{ action_id: actionId }, id:'ha1' });
    if (r.error) { toast(r.error.message, false); return; }
    toast(_L('승인됨', 'Approved'));
    loadHealingPending();
  } catch (e) { toast(e.message, false); }
}

async function healingReject(actionId) {
  try {
    var r = await fetchPost(EP.RPC(), { jsonrpc:'2.0', method:'ai.healing.reject', params:{ action_id: actionId }, id:'hr1' });
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
  refreshAlertHistoryFromEvent: refreshAlertHistoryFromEvent,
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
