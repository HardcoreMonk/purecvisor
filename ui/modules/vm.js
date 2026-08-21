                                                                  
                             
                                                   
                                                  
                                                                     

  
                                                        
                                                                   
                                                   
  
                                                                 
                                                    
                                                    
                                                    
  
     
                                                        
                              
                                                            
                                                   
                                                                         
                                                                          
                                                    
  
                                              
                                                                             
                                                                 
                                
                                                                         
                                                                              
                                                              
                                                                   
                                                      
  
            
                                                                  
                                                                 
                                                          
                                                    
                                                                              
                                                                                    
                                                                     
                                                                           
                          
  
                                
                                                              
                                                           
                                                                       
                                              
                                           
                                                           
  
              
                                                            
                                                  
                                                                   
                                                           
                                                                
                                       
                                                                  
  
             
                                                                  
                                                                  
                                                                  
                                     
  
                           
                                                                           
                                                             
                                                           
                                                                     
                                                    
                              
  
                         
                                                                             
                                                                          
                                                                           
                                                          
                                                           
  
                    
                                                                   
                                                   
                                                        
                                              
  
                                
                                                               
                                                             
                                                       
                                                        
                                                        
                             
  
               
                                                                  
                                                     
                                                                     
                                                                   
                                                   
                                                         
                                                   
                                                            
  
                                                  
                                                     
                  
                                                          
                                                              
                                             
                                                                               
                                                   
                                                        
                                                     
                                                        
  
        
                                                                              
                                                       
                                                        
                                                                    
                                                            
                                                           
                                                        
   

window.PCV = window.PCV || {};
(function(PCV) {

                                    
                                                                
                                                   
                                          
                                                       
                                       
var _lastVmListHash = '';
var vmViewMode = localStorage.getItem('pcv-vm-view') || 'list';
                                                                
                                                  
                                                    
var _vfValue = '';

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
  if (!document.getElementById('vl')) return;                               
  var newHash = vmList.map(function(v){return v.name+v.state+(v.live_cpu_pct||0);}).join('|');
  if (skipContent && newHash === _lastVmListHash) return;
  _lastVmListHash = newHash;
  const l = getFiltered();
  const favs = getFavorites();
                                   
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
      cardGrid.appendChild(el('div', { class: 'hc', draggable: 'true', ondragstart: "event.dataTransfer.setData('text/plain','" + v.name + "')", style: 'cursor:grab;border-left:3px solid ' + (on ? 'var(--st-ok)' : 'var(--st-idle)'), onclick: 'selectedVmIndex=' + vmList.indexOf(v) + ";window.navigateTo('summary')" },
        el('div', { class: 'flex items-center gap-6 mb-6' },
          HN.statusDot(on ? 'ok' : 'idle', { glow: on }),
          el('b', null, v.name)),
        el('div', { class: 'flex gap-8 text-11' },
          el('div', { class: 'flex-1' }, el('div', { class: 'color-muted' }, 'CPU ' + cp.toFixed(1) + '%'), HN.gauge({ value: cp, warn: 80, crit: 95, inline: true })),
          el('div', { class: 'flex-1' }, el('div', { class: 'color-muted' }, 'MEM ' + mp.toFixed(1) + '%'), HN.gauge({ value: mp, warn: 80, crit: 95, inline: true }))),
        el('div', { class: 'flex gap-8 mt-6 text-xs color-muted' },
          el('span', null, (v.vcpu || '?') + ' vCPU'),
          el('span', null, (v.memory_mb || '?') + ' MB'),
          el('span', null, HN.statusPill(on ? 'ok' : 'idle', String(v.state || '?').toUpperCase())))));
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
      parts.push(el('div', { class: 'vi ' + (ri === selectedVmIndex ? 'active' : ''), onclick: 'selectedVmIndex=' + ri + ";window.navigateTo(localStorage.getItem('pcv-last-vm-tab')||'summary')", oncontextmenu: 'showCtx(event,' + ri + ')' },
        el('input', { type: 'checkbox', checked: checkedVms.has(ri) ? '' : null, 'aria-label': 'Select ' + v.name, onclick: 'event.stopPropagation();toggleChk(' + ri + ')' }),
        el('span', { class: 'fav-star', onclick: "event.stopPropagation();toggleFavorite('" + escapeAttr(v.name) + "')", title: 'Favorite' }, star),
        HN.statusDot(on ? 'ok' : 'idle', { glow: on }),
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
      var hist = (PCV.metrics ? PCV.metrics.window('vm.' + v.name + '.cpu', '15m') : []).map(function (s) { return s.v; });
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
  document.getElementById('bbtn').style.display = checkedVms.size > 0 ? 'inline' : 'none';
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
  if (e.target.closest && e.target.closest('button, a, input, select, textarea, [role="button"], [role="link"], [role="menuitem"], [role="tab"]')) return;
  if (e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA' || e.target.tagName === 'SELECT') return;
  if (currentTab !== 'summary' && currentTab !== 'dashboard' && currentTab !== 'console' && currentTab !== 'snapshots' && currentTab !== 'performance') return;
  if (e.key === 'ArrowDown' || e.key === 'j') {
    e.preventDefault();
    if (selectedVmIndex < vmList.length - 1) { selectedVmIndex++; render(); refreshVmDetail(); }
  } else if (e.key === 'ArrowUp' || e.key === 'k') {
    e.preventDefault();
    if (selectedVmIndex > 0) { selectedVmIndex--; render(); refreshVmDetail(); }
  } else if (e.key === 'Enter') {
    e.preventDefault();
    navigateTo('summary');
  }
});

                                                                                

                          
   
                                    
  
                                
  
                                                                            
                                                                  
                                                    
                                  
  
               
                                                        
                                                        
                                                       
   
function showCtx(e, i) {
  e.preventDefault();
  selectedVmIndex = i;
  const m = document.getElementById('ctx');
  const ri = i;
                                                              
                                                       
                                                       
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  var sep = function() { return el('div', { class: 'sep' }); };
  var ci = function(label, onClick, role) {
    var attrs = { class: 'ci', onClick: onClick };
    if (role) attrs['data-role'] = role;
    return el('div', attrs, label);
  };
  clearEl(m);
  m.appendChild(frag(
    ci('▶ ' + t('power.start'), function() { window.vmPower('start'); }, 'OPERATOR,ADMIN'),
    ci('■ ' + t('power.stop'), function() { window.vmPower('stop'); }, 'OPERATOR,ADMIN'),
    sep(),
    ci('📷 ' + t('vm.snapshot'), function() { window.showSnap(); }, 'OPERATOR,ADMIN'),
    ci('⚙ ' + t('vm.settings'), function() { window.showSettings(); }, 'OPERATOR,ADMIN'),
    ci('✎ ' + _L('이름 변경', 'Rename'), function() { window.showRenameVm(); }, 'OPERATOR,ADMIN'),
    ci('🖨 VNC', function() { window.showVnc(); }, 'OPERATOR,ADMIN'),
    sep(),
    ci('📌 Memory Stats', function() { window.showMemStats(); }),
    ci('⚙ CPU Stats', function() { window.showCpuStats(); }),
    ci('💾 Disk Resize', function() { window.showDiskLiveResize(); }, 'OPERATOR,ADMIN'),
    ci('💬 Guest Agent', function() { window.showGuestAgent(); }, 'OPERATOR,ADMIN'),
    sep(),
    ci('🌐 NIC', function() { window.showNicMgr(); }, 'OPERATOR,ADMIN'),
    ci('📋 Clone', function() { window.vmClone(ri); }, 'OPERATOR,ADMIN'),
    ci('📦 Export OVA', function() { window.vmExportOva(ri); }, 'ADMIN'),
    sep(),
    ci('❌ ' + t('btn.delete'), function() { window.vmDel(); }, 'OPERATOR,ADMIN')
  ));
  if (typeof applyRoleVisibility === 'function') applyRoleVisibility(window.currentUser && window.currentUser.role);
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
    HN.statusPill(cpuHi ? 'warn' : (on ? 'ok' : 'idle'), String(v.state || '?').toUpperCase() + (cpuHi ? ' [HIGH_LOAD]' : '')));
  var sg = el('div', { class: 'sg' },
    HN.card('💻 System', [
      HN.row('Guest OS', 'Linux (KVM)'),
      HN.row('UUID', el('span', { class: 'text-xs' }, v.uuid || '-')),
      HN.row(_L('부트', 'Boot'), (v.boot_mode || 'bios').toUpperCase()),
      HN.row(_L('자동시작', 'Auto Start'), v.auto_start ? HN.statusPill('ok', 'ON') : HN.statusPill('idle', 'OFF'))
    ]),
    HN.card('⚙ CPU', [
      HN.row('vCPU', String(vcpu)),
      HN.row(_L('사용률', 'Usage'), el('span', { class: cpuHi ? 'color-red' : 'color-green' }, cpuPct.toFixed(1) + '%')),
      HN.gauge({ value: cpuPct, warn: 80, crit: 95, inline: true })
    ]),
    HN.card('📌 ' + _L('메모리', 'Memory'), [
      HN.row(_L('할당', 'Allocated'), String(memMb) + ' MB'),
      HN.row(_L('사용률', 'Usage'), memPct.toFixed(1) + '%'),
      HN.gauge({ value: memPct, warn: 80, crit: 95, inline: true })
    ]),
    HN.card('💾 ' + _L('스토리지', 'Storage'), [
      HN.row(_L('타입', 'Type'), HN.statusPill('idle', String(v.storage_type || '-').toUpperCase())),
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
      el('div', { class: 'flex gap-4 flex-wrap mb-8', 'data-role': 'OPERATOR,ADMIN' },
        el('button', { class: 'btn btn-g', onclick: "vmPower('start')" }, '▶ ' + t('power.start')),
        el('button', { class: 'btn', onclick: "vmPower('suspend')" }, '❚❚ ' + t('power.pause')),
        el('button', { class: 'btn', onclick: "vmPower('resume')" }, '▶▶ ' + t('power.resume')),
        el('button', { class: 'btn btn-r', onclick: "vmPower('stop')" }, '■ ' + t('power.stop'))),
      el('div', { style: 'display:grid;grid-template-columns:repeat(auto-fill,minmax(100px,1fr));gap:4px' },
        el('button', { class: 'btn', onclick: 'showSnap()', 'data-role': 'OPERATOR,ADMIN' }, t('vm.snapshot')),
        el('button', { class: 'btn', onclick: 'showSettings()', 'data-role': 'OPERATOR,ADMIN' }, t('vm.settings')),
        el('button', { class: 'btn', onclick: 'showRenameVm()', 'data-role': 'OPERATOR,ADMIN' }, '✎ ' + _L('이름', 'Rename')),
        el('button', { class: 'btn', onclick: 'showNicMgr()', 'data-role': 'OPERATOR,ADMIN' }, 'NIC'),
        el('button', { class: 'btn', onclick: 'vmClone(' + selectedVmIndex + ')', 'data-role': 'OPERATOR,ADMIN' }, '📋 Clone'),
        el('button', { class: 'btn', onclick: 'vmExportOva(' + selectedVmIndex + ')', 'data-role': 'ADMIN' }, '📦 Export'),
        el('button', { class: 'btn', onclick: 'showImportOva()', 'data-role': 'OPERATOR,ADMIN' }, '📥 Import'),
        el('button', { class: 'btn', onclick: 'showMemStats()' }, '📌 Mem'),
        el('button', { class: 'btn', onclick: 'showCpuStats()' }, '⚙ CPU'),
        el('button', { class: 'btn', onclick: 'showVmDiskUsage()' }, '📊 ' + _L('디스크 사용량', 'Disk Usage')),
        el('button', { class: 'btn', onclick: 'showDiskLiveResize()', 'data-role': 'OPERATOR,ADMIN' }, '💾 Disk'),
        el('button', { class: 'btn', onclick: 'showBlkioEditor()', 'data-role': 'OPERATOR,ADMIN' }, '⚙ I/O'),
        el('button', { class: 'btn', onclick: 'showGuestAgent()', 'data-role': 'OPERATOR,ADMIN' }, '💬 Agent'))));
  PCV.uxlib.clearEl(b);
  b.appendChild(header);
  b.appendChild(sg);
  if (typeof applyRoleVisibility === 'function') {
    applyRoleVisibility(window.currentUser && window.currentUser.role);
  }
}

                                     
                                                                   
                                                       
                                                           
var VM_DETAIL_TABS = [
  { t: 'summary', ko: '요약', en: 'Summary' },
  { t: 'console', ko: '콘솔', en: 'Console' },
  { t: 'snapshots', ko: '스냅샷', en: 'Snapshots' },
  { t: 'performance', ko: '성능', en: 'Performance' },
  { t: 'timeline', ko: '운영 타임라인', en: 'Timeline' }
];
var _vmTabFocusIntent = null;

function _navigateVmTab(tab) {
  navigateTo(tab, {
                                                                  
                                                            
                                                  
    before: function() {
      _vmTabFocusIntent = { tab: tab, generation: (window._navGeneration || 0) + 1 };
    }
  });
}

function _vmTabKeydown(event) {
  var tabs = Array.from(event.currentTarget.closest('[role="tablist"]').querySelectorAll('[role="tab"]'));
  var index = tabs.indexOf(event.currentTarget);
  var next = null;
  if (event.key === 'ArrowRight') next = tabs[(index + 1) % tabs.length];
  else if (event.key === 'ArrowLeft') next = tabs[(index - 1 + tabs.length) % tabs.length];
  else if (event.key === 'Home') next = tabs[0];
  else if (event.key === 'End') next = tabs[tabs.length - 1];
  if (!next) return;
  event.preventDefault();
  next.focus();
  next.click();
}
                                                           
                                                                
                                                              
function _vmDetailFns() {
  return { summary: window.renderSummary, console: window.renderConsole,
    snapshots: window.renderSnapshots, performance: window.renderPerformance,
    timeline: window.renderTimeline };
}
function refreshVmDetail() {
  var d = document.getElementById('vm-detail');
  if (!d) return;
  var fn = _vmDetailFns()[currentTab];
  if (!fn) return;
  PCV.uxlib.clearEl(d);
  var result = fn(d, vmList[selectedVmIndex]);
  if (result && typeof result.catch === 'function') result.catch(function () {});
}

function renderVmScreen(b, v, tab) {
  var el = PCV.uxlib.el;
  PCV.uxlib.clearEl(b);
  var left = el('div', { class: 'vm-screen-left' },
    el('div', { class: 'sb-head' },
      el('span', null, _L('VM 자산', 'VM Inventory')),
      el('span', { id: 'vc' }, String(vmList.length))),
    el('div', { style: 'padding:4px 8px' },
      el('input', { class: 'sb-search', id: 'vf', value: _vfValue, placeholder: _L('이름으로 찾기', 'Filter by name'),
        onInput: function (e) { _vfValue = e.target.value; render(); },
        'aria-label': _L('VM 이름으로 찾기', 'Filter VMs by name') }),
      el('div', { class: 'flex gap-6', style: 'justify-content:center' },
        el('span', { class: 'sb-sort', onclick: "setSort('name')", role: 'button', tabindex: '0' }, _L('이름', 'Name')),
        el('span', { class: 'sb-sort', onclick: "setSort('cpu')", role: 'button', tabindex: '0' }, 'CPU'),
        el('span', { class: 'sb-sort', onclick: "setSort('mem')", role: 'button', tabindex: '0' }, 'MEM'),
        el('span', { class: 'sb-sort', onclick: "setSort('state')", role: 'button', tabindex: '0' }, _L('상태', 'State')))),
    el('div', { class: 'vm-list', id: 'vl', role: 'region', 'aria-label': 'Virtual machines' }),
    el('button', { class: 'tb', id: 'bbtn', onclick: 'bulkStop()', style: 'display:none;margin:6px 8px', 'data-role': 'OPERATOR,ADMIN' }, _L('선택 항목 중지', 'Stop selected')));
  var strip = el('div', { class: 'vm-tabstrip flex', role: 'tablist', 'aria-label': _L('VM 상세', 'VM details'), style: 'border-bottom:1px solid var(--border);padding:0 10px;gap:2px' },
    VM_DETAIL_TABS.map(function (d) {
      var on = d.t === tab;
      return el('button', {
        id: 'vm-tab-' + d.t, 'data-t': d.t, type: 'button', role: 'tab',
        tabindex: on ? '0' : '-1', 'aria-selected': on ? 'true' : 'false', 'aria-controls': 'vm-detail',
        onClick: function () { _navigateVmTab(d.t); }, onKeyDown: _vmTabKeydown,
        style: 'padding:9px 14px;font-size:13px;cursor:pointer;background:none;border-top:0;border-right:0;border-left:0;border-bottom:2px solid ' + (on ? 'var(--accent)' : 'transparent') + ';color:' + (on ? 'var(--accent)' : 'var(--fg2)') + ';font-weight:' + (on ? '600' : '400')
      }, _L(d.ko, d.en));
    }));
  var detail = el('div', { id: 'vm-detail', role: 'tabpanel', 'aria-labelledby': 'vm-tab-' + tab, style: 'flex:1;min-width:0;overflow-y:auto' });
                                                                
                                                       
                                                 
  var pagehead = HN.pagehead({
    title: _L('가상 머신', 'Virtual Machines'),
    desc: _L('노드의 VM 자산과 전원 상태를 한 화면에서 관리합니다.', 'Manage this node’s VM inventory and power state in one place.'),
    actions: [
      el('button', { class: 'btn btn-primary', onclick: 'showCreate()', 'data-role': 'OPERATOR,ADMIN' }, '+ ' + _L('새 VM', 'New VM')),
      el('button', { class: 'btn', onclick: 'showSnap()', 'data-role': 'OPERATOR,ADMIN' }, _L('스냅샷', 'Snapshots'))
    ]
  });
  b.appendChild(el('div', { style: 'display:flex;flex-direction:column;height:100%;min-height:0' },
    pagehead,
    el('div', { class: 'vm-screen flex', style: 'flex:1;min-height:0' },
      left,
      el('div', { class: 'vm-screen-right', style: 'flex:1;display:flex;flex-direction:column;min-width:0' }, strip, detail))));
  render();               
  var fn = _vmDetailFns()[tab] || window.renderSummary;
  var result = fn(detail, v);
  var focusIntent = _vmTabFocusIntent;
  _vmTabFocusIntent = null;
  if (focusIntent && focusIntent.tab === tab &&
      focusIntent.generation === (window._navGeneration || 0)) {
    var focusedTab = strip.querySelector('[data-t="' + tab + '"]');
    if (focusedTab) focusedTab.focus({ preventScroll: true });
  }
  return result;
}
window.renderVmScreen = renderVmScreen;

                                             
                                    
                                                  
                                       
                                       
PCV.vm = Object.assign(PCV.vm || {}, {
  render: render,
  setSort: setSort,
  getFiltered: getFiltered,
  toggleVmView: toggleVmView,
  renderSummary: renderSummary,
  renderVmScreen: renderVmScreen,
  refreshVmDetail: refreshVmDetail,
});

                                                                            
window.render = render;
window.setSort = setSort;
window.getFiltered = getFiltered;
window.toggleChk = toggleChk;
window.bulkStop = bulkStop;
window.showCtx = showCtx;
window.renderSummary = renderSummary;
window.toggleVmView = toggleVmView;
window.refreshVmDetail = refreshVmDetail;

})(window.PCV);
