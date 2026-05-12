




window.PCV = window.PCV || {};
(function(PCV) {




window.selCtr = window.selCtr || null;
window.ctrTab = window.ctrTab || 'summary';
window.ctrHist = window.ctrHist || [];


window.ctrSortKey = window.ctrSortKey || 'name';
window.ctrSortDir = window.ctrSortDir || 1;
function setCtrSort(k) {
  if (window.ctrSortKey === k) window.ctrSortDir *= -1; else { window.ctrSortKey = k; window.ctrSortDir = 1; }
  renderContainerList();
}


async function renderContainerList() {
  const el = document.getElementById('ctr-list');
  if (!el) return;
  try {
    const c = await fetchGet(EP.CTR_LIST());
    const l = unwrapList(c);
    const countEl = document.getElementById('ctr-count');
    if (countEl) countEl.textContent = l.length;

    const filter = (document.getElementById('ctr-filter')?.value || '').toLowerCase();
    let fl = filter ? l.filter(v => v.name.toLowerCase().includes(filter)) : [...l];

    fl.sort((a, b) => {
      let va, vb;
      if (window.ctrSortKey === 'name') { va = a.name || ''; vb = b.name || ''; }
      else if (window.ctrSortKey === 'state') { va = a.state || ''; vb = b.state || ''; }
      else if (window.ctrSortKey === 'ip') { va = a.ip_addr || ''; vb = b.ip_addr || ''; }
      else { va = a.name || ''; vb = b.name || ''; }
      return (va < vb ? -1 : va > vb ? 1 : 0) * window.ctrSortDir;
    });

    if (fl.length === 0) {
      el.innerHTML = '<div class="empty-state" style="padding:24px;text-align:center"><div style="font-size:32px;margin-bottom:8px">&#9783;</div><div class="text-xs color-muted">' + t('msg.no_containers') + '</div><button class="btn btn-g mt-12 text-11" onclick="showCtrCreate()">+ ' + t('ctr.new') + '</button></div>';
      return;
    }
    let h = '';
    fl.forEach(v => {
      const on = v.state === 'RUNNING';
      const s = selCtr === v.name;
      h += '<div onclick="selCtr=\'' + esc(v.name) + '\';currentTab=\'containers\';renderContent();renderContainerList()" class="vm-item' + (s ? ' sel' : '') + '" style="padding:6px 8px;cursor:pointer;border-left:3px solid ' + (s ? 'var(--accent)' : 'transparent') + ';background:' + (s ? 'rgba(0,240,255,.06)' : 'transparent') + '">';
      h += '<div class="flex items-center gap-6">';
      h += '<span style="font-size:8px;color:' + (on ? 'var(--green)' : 'var(--red)') + '">&#9679;</span>';
      h += '<span style="font-size:12px;font-weight:' + (s ? '700' : '400') + '">' + esc(v.name) + '</span>';
      h += '</div>';
      h += '<div style="display:flex;justify-content:space-between;margin-left:14px;margin-top:1px">';
      h += '<span class="stat-label text-xs">' + (on ? t('status.running') : t('status.stopped')) + '</span>';
      if (on && v.ip_addr) h += '<span class="stat-label text-xs color-green">' + esc(v.ip_addr) + '</span>';
      h += '</div></div>';
    });
    el.innerHTML = h;
  } catch (e) { el.innerHTML = '<div class="text-xs color-muted" style="padding:8px">Failed to load containers.</div>'; }
}


async function renderContainers(b) {
  b.innerHTML = showSkeleton();
  try {
    const c = await fetchGet(EP.CTR_LIST());
    const l = unwrapList(c);
    if (l.length === 0) {
      b.innerHTML = (typeof emptyStatePro === 'function')
        ? emptyStatePro({
            icon: '&#9783;',
            title: _L('컨테이너가 없습니다', 'No containers'),
            desc: _L('첫 LXC 컨테이너를 만들어보세요. ZFS 백엔드 + cloud-init 자동.', 'Create your first LXC container with ZFS backend.'),
            ctaLabel: _L('+ 컨테이너 만들기', '+ Create Container'),
            ctaAction: 'showCtrCreate()'
          })
        : '<div class="empty-state"><div class="empty-state-icon">&#9783;</div><div class="empty-state-text">' + t('msg.no_containers') + '</div><button class="btn btn-g" onclick="showCtrCreate()" class="mt-12">+ ' + t('ctr.new') + '</button></div>';
      return;
    }
    let h = '<div style="display:flex;gap:0;height:calc(100vh - 280px);min-height:400px">';
    h += '<div style="min-width:220px;max-width:220px;border-right:1px solid var(--border);overflow-y:auto;padding:8px">';
    h += '<div class="justify-between items-center mb-8"><span class="text-xs font-bold" style="text-transform:uppercase;letter-spacing:.06em;color:var(--fg2)">' + t('nav.containers') + '</span><div class="flex gap-6 items-center">' + H.badge(String(l.length), 'y') + '<button class="btn btn-g" style="font-size:11px;padding:4px 10px" onclick="showCtrCreate()">+ ' + t('ctr.new') + '</button></div></div>';
    l.forEach(v => {
      const on = v.state === 'RUNNING';
      const s = selCtr === v.name;
      h += '<div onclick="selCtr=\'' + v.name + '\';ctrTab=\'summary\';renderContainers(document.getElementById(\'cb\'))" style="padding:6px 8px;cursor:pointer;border-radius:4px;margin-bottom:2px;border-left:3px solid ' + (s ? 'var(--accent)' : 'transparent') + ';background:' + (s ? 'var(--bg3)' : 'transparent') + '">';
      h += '<div class="flex items-center gap-6"><span style="font-size:9px;color:' + (on ? 'var(--green)' : 'var(--fg2)') + '">&#9679;</span><span style="font-size:13px;font-weight:' + (s ? '600' : '400') + '">' + escapeHtml(v.name) + '</span></div>';
      h += '<div class="stat-label" style="margin-left:15px">' + v.state + (on ? ' &bull; ' + (v.ip_addr || '') : '') + '</div></div>';
    });
    h += '</div>';
    h += '<div style="flex:1;overflow-y:auto;display:flex;flex-direction:column">';
    const cv = l.find(x => x.name === selCtr);
    if (cv) {
      const on = cv.state === 'RUNNING';
      h += '<div style="padding:10px 14px;border-bottom:1px solid var(--border)" class="justify-between items-center">';
      h += '<div><span style="font-size:15px;font-weight:700">' + escapeHtml(cv.name) + '</span> ' + H.badge(cv.state, on ? 'g' : 'r') + '</div>';
      h += '<div class="flex gap-4">';
      if (!on) h += '<button class="btn btn-g text-12 px-12 py-4" onclick="ctrA(\'' + cv.name + '\',\'start\')">&#9654; ' + t('power.start') + '</button>';
      if (on) {
        h += '<button class="btn btn-r text-12 px-12 py-4" onclick="ctrA(\'' + cv.name + '\',\'stop\')">&#9632; ' + t('power.stop') + '</button>';
        h += '<button class="btn text-12 px-12 py-4" onclick="ctrReboot(\'' + cv.name + '\')">&#8635; Reboot</button>';
      }
      h += '<button class="btn btn-r text-12 px-12 py-4" onclick="ctrDel(\'' + cv.name + '\')">&#128465; ' + t('btn.delete') + '</button>';
      h += '</div></div>';
      const tabs = ['summary', 'console', 'resources', 'network', 'dns', 'options', 'snapshots', 'notes', 'tasks'];
      h += '<div class="flex" style="border-bottom:1px solid var(--border);padding:0 10px;gap:2px;overflow-x:auto">';
      tabs.forEach(t2 => {
        h += '<div onclick="ctrTab=\'' + t2 + '\';renderContainers(document.getElementById(\'cb\'))" style="padding:9px 14px;font-size:13px;cursor:pointer;white-space:nowrap;border-bottom:2px solid ' + (ctrTab === t2 ? 'var(--accent)' : 'transparent') + ';color:' + (ctrTab === t2 ? 'var(--accent)' : 'var(--fg2)') + ';font-weight:' + (ctrTab === t2 ? '600' : '400') + ';transition:color .15s">' + t2.charAt(0).toUpperCase() + t2.slice(1) + '</div>';
      });
      h += '</div>';
      h += '<div style="padding:14px;flex:1" id="ctr-tab-content"></div>';
    } else {
      h += '<div style="flex:1;display:flex;align-items:center;justify-content:center;color:var(--fg2)"><div class="text-center"><div style="font-size:32px;margin-bottom:8px">&#9783;</div><p>' + t('ctr.select') + '</p></div></div>';
    }
    h += '</div></div>';
    b.innerHTML = h;
    if (cv) { const tb = document.getElementById('ctr-tab-content'); if (tb) await ctrRenderTab(tb, cv); }
  } catch (e) { b.innerHTML = '<p class="color-red">' + escapeHtml(e.message) + '</p>'; }
}


async function ctrRenderTab(tb, cv) {
  const n = cv.name;
  const on = cv.state === 'RUNNING';
  if (ctrTab === 'summary') {
    let m = {}; if (on) { try { const r = await fetchGet(EP.CTR_METRICS(n)); m = unwrapData(r); } catch (e) { if(_DEBUG) console.warn('ctrRenderTab:', e.message); } }
    let info = { hostname: n, os: '', uptime: '', procs: '', kernel: '' };
    if (on) { const cmds = [['hostname', 'hostname'], ['uptime', 'uptime -p'], ['nproc', 'nproc'], ['kernel', 'uname -r']];
      for (const [k, c] of cmds) { try { const r = await fetchPost(EP.CTR_EXEC(n), { command: c }); info[k] = unwrapData(r).output?.trim() || ''; } catch (e) { if(_DEBUG) console.warn('ctrRenderTab:', e.message); } } }
    const cpu = m.cpu_percent || 0, mem_u = m.mem_used_mb || 0, mem_l = m.mem_limit_mb || 0, mem_p = mem_l > 0 ? mem_u / mem_l * 100 : 0;
    const nrx = m.net_rx_mb || 0, ntx = m.net_tx_mb || 0;
    tb.innerHTML = H.grid(4,
      H.card('Status', '<div class="stat-lg" style="color:' + (on ? 'var(--green)' : 'var(--fg2)') + '">' + (on ? t('status.running') : t('status.stopped')) + '</div><div class="stat-label mt-4">' + (on ? 'PID: ' + (m.init_pid || '-') : '') + '</div>')
    + H.card('CPU', '<div class="stat-md">' + cpu.toFixed(1) + '%</div>' + renderProgressBar(cpu) + '<div class="stat-label">' + (info.nproc || '?') + ' cores</div>')
    + H.card('Memory', '<div class="stat-md">' + (mem_l > 0 ? mem_p.toFixed(1) + '%' : mem_u.toFixed(0) + ' MB') + '</div>' + (mem_l > 0 ? renderProgressBar(mem_p) : ''))
    + H.card('Network', H.row('RX', nrx.toFixed(1) + ' MB') + H.row('TX', ntx.toFixed(1) + ' MB') + H.row('IP', '<span class="color-accent">' + escapeHtml(cv.ip_addr || '-') + '</span>'))
    ) + '<div class="sg grid-2 mt-12">'
    + H.card('System Info', H.row('Hostname', escapeHtml(info.hostname || '-')) + H.row('Uptime', escapeHtml(info.uptime || '-')) + H.row('Kernel', escapeHtml(info.kernel || '-')) + H.row('Image', escapeHtml(cv.image || '-')))
    + H.card('Configuration', H.row('Bridge', 'pcvbr0') + H.row('AppArmor', 'unconfined') + H.row('Type', 'LXC (unprivileged: no)') + H.row('Node', location.hostname))
    + '</div>';
  } else if (ctrTab === 'console') {
    if (!on) { tb.innerHTML = H.card('&#9000; ' + t('tab.console'), '<p class="color-muted">' + t('ctr.console.stopped') + '</p>'); return; }
    tb.innerHTML = '<div style="background:var(--bg);border:1px solid var(--border);border-radius:var(--r);padding:0;font-family:monospace;display:flex;flex-direction:column;height:100%">'
    + '<div class="justify-between stat-label" style="padding:6px 10px;border-bottom:1px solid var(--border)"><span>&#9000; ' + n + ' — Shell</span><span class="color-green">' + t('connected') + '</span></div>'
    + '<pre id="ctr-output" style="flex:1;padding:8px 10px;margin:0;overflow-y:auto;font-size:11px;color:var(--green);white-space:pre-wrap;min-height:250px;max-height:400px">root@' + n + ':~# \n</pre>'
    + '<div class="flex" style="border-top:1px solid var(--border)"><span style="padding:6px 8px;color:var(--green);font-size:12px">$</span><input id="ctr-cmd" style="flex:1;background:transparent;border:none;color:var(--fg);font-family:monospace;font-size:12px;padding:6px 0;outline:none" placeholder="Type command..." onkeydown="if(event.key===\'Enter\')ctrRunCmd(\'' + n + '\')"></div></div>';
    setTimeout(() => { document.getElementById('ctr-cmd')?.focus(); }, 100);
  } else if (ctrTab === 'resources') {
    let info = { cpu: '', mem: '', disk: '', procs: '' };
    if (on) { for (const [k, c] of [['cpu', 'nproc'], ['mem', 'free -h | head -2'], ['disk', 'df -h / 2>/dev/null | tail -1'], ['procs', 'ps aux --no-headers 2>/dev/null | wc -l']]) { try { const r = await fetchPost(EP.CTR_EXEC(n), { command: c }); info[k] = unwrapData(r).output?.trim() || '-'; } catch (e) { info[k] = '-'; } } }
    const memLines = (info.mem || '').split('\n'); const memHeader = memLines[0] || ''; const memData = memLines[1] || '';
    tb.innerHTML = '<h3 class="section-title-md">Resources</h3><div class="sg">'
    + H.card('CPU', H.row('Cores', info.cpu || '-') + H.row('Type', 'host (KVM passthrough)'))
    + H.card('Memory', '<pre class="stat-label" style="margin:0;white-space:pre;overflow-x:auto">' + memHeader + '\n' + memData + '</pre>')
    + H.card('Disk (rootfs)', '<pre class="stat-label" style="margin:0;white-space:pre;overflow-x:auto">' + (info.disk || 'N/A') + '</pre>')
    + H.card('Processes', H.row('Running', info.procs || '-'))
    + '</div>'
    + '<h3 class="section-title-md mt-14">Resource Limits (cgroup v2)</h3>'
    + '<div class="sg grid-2">'
    + H.card('Set CPU Limit', '<div class="fr"><label>CPU Shares</label><input id="ctr-cpu-shares" type="number" value="1024"></div><div class="fr"><label>CPU Quota (µs)</label><input id="ctr-cpu-quota" type="number" value="100000"></div><button class="btn btn-g mt-8" onclick="ctrSetLimits(\'' + escapeHtml(n) + '\',\'cpu\')">Apply CPU Limit</button>')
    + H.card('Set Memory Limit', '<div class="fr"><label>Memory Limit (MB)</label><input id="ctr-mem-limit" type="number" value="512"></div><div class="fr"><label>Swap Limit (MB)</label><input id="ctr-swap-limit" type="number" value="0"></div><button class="btn btn-g mt-8" onclick="ctrSetLimits(\'' + escapeHtml(n) + '\',\'mem\')">Apply Memory Limit</button>')
    + '</div>';
  } else if (ctrTab === 'network') {
    tb.innerHTML = '<h3 class="section-title-md">Network Interfaces <button class="btn btn-g" style="font-size:10px;margin-left:8px" onclick="ctrNicAdd(\'' + escapeHtml(n) + '\')">+ Add NIC</button></h3><div id="ctr-nic-list"><span class="spinner"></span> Loading NICs...</div>';
    tb.innerHTML += '<h3 class="section-title-md mt-14">Bandwidth QoS</h3><div class="sg grid-2">'
    + H.card('Set Bandwidth Limit', '<div class="fr"><label>Interface</label><input id="ctr-bw-nic" value="eth0" class="w-80"></div><div class="fr"><label>Inbound (Kbps)</label><input id="ctr-bw-in" type="number" value="0" placeholder="0 = unlimited"></div><div class="fr"><label>Outbound (Kbps)</label><input id="ctr-bw-out" type="number" value="0" placeholder="0 = unlimited"></div><button class="btn btn-g mt-8" onclick="ctrSetBandwidth(\'' + escapeHtml(n) + '\')">Apply QoS</button>')
    + H.card('Routing &amp; Addresses', '<div id="ctr-net-info"><span class="spinner"></span></div>')
    + '</div>';

    try {
      const r = await fetchGet(EP.CTR_NICS(n));
      const nics = unwrapList(r);
      const el = document.getElementById('ctr-nic-list');
      if (el) {
        if (nics.length === 0) { el.innerHTML = '<p class="color-muted">No NICs configured</p>'; }
        else {
          let h = '<table class="table-sticky"><thead><tr><th>Name</th><th>Type</th><th>Bridge</th><th>MAC</th><th>IPv4</th><th>Actions</th></tr></thead><tbody>';
          nics.forEach(nc => {
            h += '<tr><td><b>' + escapeHtml(nc.name || '-') + '</b></td><td>' + escapeHtml(nc.type || 'veth') + '</td><td>' + H.badge(escapeHtml(nc.bridge || '-'), 'y') + '</td><td class="text-xs">' + escapeHtml(nc.hwaddr || 'auto') + '</td><td class="color-accent">' + escapeHtml(nc.ipv4 || '-') + '</td><td>';
            if (nc.name !== 'eth0') h += '<button class="btn btn-r" style="font-size:9px;padding:2px 6px" onclick="ctrNicDel(\'' + escapeHtml(n) + '\',\'' + escapeHtml(nc.name) + '\')">Remove</button>';
            else h += '<span class="color-muted text-xs">primary</span>';
            h += '</td></tr>';
          });
          h += '</tbody></table>';
          el.innerHTML = h;
        }
      }
    } catch (e) {
      const el = document.getElementById('ctr-nic-list');
      if (el) el.innerHTML = H.card('Interface eth0', H.row('IP Address', '<span class="color-accent">' + escapeHtml(cv.ip_addr || '-') + '</span>') + H.row('Bridge', 'pcvbr0') + H.row('Type', 'veth'));
    }

    if (on) {
      try { const r = await fetchPost(EP.CTR_EXEC(n), { command: 'ip -4 addr show 2>/dev/null; echo "---"; ip route 2>/dev/null | head -5' }); const ni = document.getElementById('ctr-net-info'); if (ni) ni.innerHTML = '<pre class="stat-label" style="margin:0;white-space:pre-wrap;overflow-x:auto">' + escapeHtml(unwrapData(r).output || '') + '</pre>'; } catch (e) { const ni = document.getElementById('ctr-net-info'); if (ni) ni.innerHTML = '<span class="color-muted">Unable to fetch</span>'; }
    } else {
      const ni = document.getElementById('ctr-net-info'); if (ni) ni.innerHTML = '<span class="color-muted">Container is stopped</span>';
    }
  } else if (ctrTab === 'dns') {
    let dns = ''; if (on) { try { const r = await fetchPost(EP.CTR_EXEC(n), { command: 'cat /etc/resolv.conf 2>/dev/null' }); dns = unwrapData(r).output || ''; } catch (e) { if(_DEBUG) console.warn('dns:', e.message); } }
    tb.innerHTML = '<h3 class="section-title-md">DNS</h3>' + H.card('Resolver Configuration',
    (on ? '<pre style="font-size:11px;color:var(--fg);margin:8px 0;white-space:pre-wrap;background:var(--bg);padding:10px;border-radius:4px;border:1px solid var(--border)">' + dns.replace(/</g, '&lt;') + '</pre>'
    + '<div class="mt-8"><div class="fr"><label>Add Nameserver</label><input id="dns-ns" placeholder="8.8.8.8" class="flex-1"><button class="btn btn-g" style="margin-left:6px" onclick="ctrDnsAdd(\'' + n + '\')">Add</button></div></div>'
    : '<p class="color-muted">' + t('ctr.console.stopped') + '</p>'));
  } else if (ctrTab === 'options') {
    tb.innerHTML = '<h3 class="section-title-md">Options</h3><div class="sg">'
    + H.card('General', H.row('Start on boot', 'No') + H.row('Start order', '---') + H.row('Protection', 'No') + H.row('Unprivileged', 'No'))
    + H.card('Security', H.row('AppArmor', 'unconfined') + H.row('Keyctl', 'No') + H.row('Nesting', 'No') + H.row('FUSE', 'No'))
    + H.card('Signals', H.row('Halt', 'SIGRTMIN+3') + H.row('Reboot', 'SIGTERM'))
    + '</div>';
  } else if (ctrTab === 'snapshots') {
    tb.innerHTML = '<h3 class="section-title-md">' + t('vm.snapshot') + ' <button class="btn btn-g" style="font-size:10px;margin-left:8px" onclick="ctrSnapCreate(\'' + escapeHtml(n) + '\')">+ ' + t('btn.create') + '</button></h3><div id="ctr-snap-list"><span class="spinner"></span> ' + t('loading') + '</div>';
    try {
      const r = await fetchGet(EP.CTR_SNAPSHOTS(n)).catch(() => ({ data: [] }));
      const sl = unwrapList(r);
      let sh = '<table class="table-sticky"><thead><tr><th>Name</th><th>' + t('vm.settings') + '</th></tr></thead><tbody>';
      if (sl.length === 0) sh += '<tr><td colspan="2" class="color-muted">' + t('snap.none') + '</td></tr>';
      sl.forEach(s => { const sn = typeof s === 'string' ? s : (s.name || s); sh += '<tr><td>' + escapeHtml(sn) + '</td><td><button class="btn text-9" onclick="ctrSnapRb(\'' + escapeHtml(n) + '\',\'' + escapeHtml(sn) + '\')">Rollback</button> <button class="btn btn-r text-9" onclick="ctrSnapDel(\'' + escapeHtml(n) + '\',\'' + escapeHtml(sn) + '\')">' + t('btn.delete') + '</button></td></tr>'; });
      sh += '</tbody></table>'; document.getElementById('ctr-snap-list').innerHTML = sh;
    } catch (e) { document.getElementById('ctr-snap-list').innerHTML = '<p class="color-muted">' + t('snap.none') + '</p>'; }
  } else if (ctrTab === 'notes') {
    tb.innerHTML = '<h3 class="section-title-md">Notes</h3>' + H.card('Container Notes', '<textarea id="ctr-notes" style="width:100%;min-height:150px;background:var(--bg);border:1px solid var(--border);border-radius:4px;color:var(--fg);padding:10px;font-family:monospace;font-size:12px;resize:vertical" placeholder="Add notes...">' + escapeHtml(localStorage.getItem('ctr-note-' + n) || '') + '</textarea><button class="btn mt-8" onclick="localStorage.setItem(\'ctr-note-' + escapeHtml(n) + '\',document.getElementById(\'ctr-notes\').value);toast(\'' + t('btn.save') + '\')">' + t('btn.save') + '</button>');
  } else if (ctrTab === 'tasks') {
    tb.innerHTML = '<h3 class="section-title-md">Task History</h3>' + H.card('Recent Events', '<div style="max-height:300px;overflow-y:auto;font-size:11px;font-family:monospace;color:var(--accent)">' + eventLog.filter(e => { var s = (e.msg || e.raw || String(e)).toLowerCase(); return s.includes('ctr') || s.includes(n.toLowerCase()); }).map(e => '<div style="padding:2px 0;border-bottom:1px solid var(--border)">' + escapeHtml(e.msg || e.raw || String(e)) + '</div>').join('') + '<div class="color-muted" style="padding:4px 0">' + eventLog.length + ' total events</div></div>');
  }
}


async function ctrA(n, a) {
  showModal('<h2>' + (a === 'start' ? '&#9654; ' + t('ctr.starting') : '&#9632; ' + t('ctr.stopping')) + '</h2><p class="mb-8"><b class="color-accent">' + n + '</b></p><div class="prog-bar"><div class="prog-fill" id="ctr-prog" class="w-pct-10"></div></div><div class="prog-status" id="ctr-st"><span class="spinner"></span>Sending ' + a + ' command...</div>');
  const pf = document.getElementById('ctr-prog'), ps = document.getElementById('ctr-st');
  try {
    pf.style.width = '30%'; ps.innerHTML = '<span class="spinner"></span>Waiting for container ' + a + '...';
    const d = await fetchPost(a === 'start' ? EP.CTR_START(n) : EP.CTR_STOP(n), {});
    pf.style.width = '60%';
    if (d.error) { pf.style.background = 'var(--red)'; pf.style.width = '100%'; ps.innerHTML = '&#10060; ' + escapeHtml(a) + ' failed: ' + escapeHtml(d.error.message || 'Unknown error'); toast(a + ' failed', false); return; }
    ps.innerHTML = '<span class="spinner"></span>' + escapeHtml(a) + ' completed, refreshing...';
    if (a === 'start') {
      for (let i = 0; i < 8; i++) { pf.style.width = (65 + i * 4) + '%'; await new Promise(r => setTimeout(r, 1500));
        try { const c = await fetchGet(EP.CTR_LIST()); const l = unwrapList(c); const ct = l.find(x => x.name === n);
          if (ct && ct.state === 'RUNNING') { const ip = ct.ip_addr || ct.ip || '';
            if (ip && ip !== 'N/A') { pf.style.width = '100%'; ps.innerHTML = '&#9989; Running — IP: <b class="color-green">' + escapeHtml(ip) + '</b>'; toast(n + ' started (' + ip + ')'); addEvt('LXC Started — ' + n + ', IP: ' + ip); setTimeout(closeModal, 2500); renderContainers(document.getElementById('cb')); return; }
            ps.innerHTML = '<span class="spinner"></span>Running, waiting for DHCP IP... (' + (i + 1) + '/8)'; } } catch (e) { if(_DEBUG) console.warn('c:', e.message); } }
      pf.style.width = '100%'; ps.innerHTML = '&#9989; Running (IP pending)'; toast(n + ' started'); addEvt('LXC Started — ' + n + ' (IP pending)');
    } else { pf.style.width = '100%'; ps.innerHTML = '&#9989; Container stopped'; toast(n + ' stopped'); addEvt('LXC Stopped — ' + n); }
    setTimeout(() => { closeModal(); renderContainers(document.getElementById('cb')); }, 2e3);
  } catch (e) { if (pf) pf.style.width = '100%'; if (ps) ps.innerHTML = '&#9989; ' + a + ' requested'; toast(n + ' ' + a); addEvt('LXC ' + a + ' — ' + n); setTimeout(() => { closeModal(); renderContainers(document.getElementById('cb')); }, 2500); }
}

async function ctrRunCmd(n) {
  const inp = document.getElementById('ctr-cmd'); const out = document.getElementById('ctr-output');
  if (!inp || !out) return; const cmd = inp.value.trim(); if (!cmd) return;
  ctrHist.push(cmd); out.textContent += '$ ' + cmd + '\n'; out.scrollTop = out.scrollHeight; inp.value = '';
  try { const r = await fetchPost(EP.CTR_EXEC(n), { command: cmd }); const d = unwrapData(r); out.textContent += (d.output || d.stdout || '(no output)') + '\n'; out.scrollTop = out.scrollHeight; } catch (e) { out.textContent += 'Error: ' + e.message + '\n'; out.scrollTop = out.scrollHeight; }
}

async function ctrDnsAdd(n) { const ns = document.getElementById('dns-ns')?.value; if (!ns) return;
  try { await fetchPost(EP.CTR_EXEC(n), { command: 'echo "nameserver ' + ns + '" >> /etc/resolv.conf' }); toast('Nameserver added'); ctrTab = 'dns'; renderContainers(document.getElementById('cb')); } catch (e) { toast(e.message, false); } }

async function ctrReboot(n) {
  showModal('<h2>&#128260; Rebooting</h2><p><b class="color-accent">' + esc(n) + '</b></p><div class="prog-bar"><div class="prog-fill" id="cr-p" class="w-pct-10"></div></div><div class="prog-status" id="cr-s"><span class="spinner"></span> Stopping...</div>');
  var pf = document.getElementById('cr-p'), ps = document.getElementById('cr-s');
  try {
    if (pf) pf.style.width = '30%';
    await fetchPost(EP.CTR_STOP(n), {});
    if (pf) pf.style.width = '50%'; if (ps) ps.innerHTML = '<span class="spinner"></span> Waiting...';
    await new Promise(function(r) { setTimeout(r, 2000); });
    if (pf) pf.style.width = '70%'; if (ps) ps.innerHTML = '<span class="spinner"></span> Starting...';
    await fetchPost(EP.CTR_START(n), {});
    if (pf) pf.style.width = '100%'; if (ps) ps.innerHTML = '&#9989; Reboot complete';
    toast(n + ' rebooted'); addEvt('LXC Reboot — ' + n);
    setTimeout(function() { closeModal(); renderContainers(document.getElementById('cb')); }, 2000);
  } catch (e) {
    if (pf) { pf.style.width = '100%'; pf.style.background = 'var(--red)'; }
    if (ps) ps.innerHTML = '&#10060; Reboot error: ' + esc(e.message);
    toast(t('msg.reboot_error'), false);
  }
}

async function ctrSnapCreate(n) { var s = await showInputModal(t('snap.name_prompt') || 'Snapshot name', t('snap.name_prompt') || 'Name', 'snap-' + Date.now()); if (!s) return;
  showModal('<h2>&#128247; ' + t('snap.created') + '</h2><p><b class="color-accent">' + esc(n) + '@' + esc(s) + '</b></p><div class="prog-bar"><div class="prog-fill" id="cs-p" class="w-pct-20"></div></div><div class="prog-status" id="cs-s"><span class="spinner"></span> Creating snapshot...</div>');
  var pf = document.getElementById('cs-p'), ps = document.getElementById('cs-s');
  try {
    if (pf) pf.style.width = '60%';
    await fetchPost(EP.CTR_SNAPSHOTS(n), { snap_name: s });
    if (pf) pf.style.width = '100%'; if (ps) ps.innerHTML = '&#9989; ' + t('snap.created') + ': ' + esc(s);
    toast(t('snap.created') + ': ' + s); addEvt('LXC Snapshot created — ' + n + '@' + s);
    setTimeout(function() { closeModal(); ctrTab = 'snapshots'; renderContainers(document.getElementById('cb')); }, 1500);
  } catch (e) {
    if (pf) { pf.style.width = '100%'; pf.style.background = 'var(--red)'; }
    if (ps) ps.innerHTML = '&#10060; ' + esc(e.message); toast(e.message, false);
  }
}
async function ctrSnapRb(n, s) { if (!await customConfirm('Rollback', n + ' → ' + s + '?')) return;
  showModal('<h2>&#9194; Rollback</h2><p><b class="color-accent">' + esc(n) + '@' + esc(s) + '</b></p><div class="prog-bar"><div class="prog-fill" id="crb-p" class="w-pct-20"></div></div><div class="prog-status" id="crb-s"><span class="spinner"></span> Rolling back...</div>');
  var pf = document.getElementById('crb-p'), ps = document.getElementById('crb-s');
  try {
    if (pf) pf.style.width = '60%';
    await fetchPost(EP.CTR_SNAP_ROLLBACK(n), { snap_name: s });
    if (pf) pf.style.width = '100%'; if (ps) ps.innerHTML = '&#9989; ' + t('snap.reverted');
    toast(t('snap.reverted')); addEvt('LXC Snapshot rollback — ' + n + '@' + s);
    setTimeout(function() { closeModal(); renderContainers(document.getElementById('cb')); }, 1500);
  } catch (e) {
    if (pf) { pf.style.width = '100%'; pf.style.background = 'var(--red)'; }
    if (ps) ps.innerHTML = '&#10060; ' + esc(e.message); toast(e.message, false);
  }
}
async function ctrSnapDel(n, s) { if (!await customConfirm(t('btn.delete'), s + '?')) return;
  showModal('<h2>&#128465; ' + t('snap.deleted') + '</h2><p><b class="color-accent">' + esc(n) + '@' + esc(s) + '</b></p><div class="prog-bar"><div class="prog-fill" id="csd-p" class="w-pct-20"></div></div><div class="prog-status" id="csd-s"><span class="spinner"></span> Deleting...</div>');
  var pf = document.getElementById('csd-p'), ps = document.getElementById('csd-s');
  try {
    if (pf) pf.style.width = '60%';
    await fetchDelete(EP.CTR_SNAP_DELETE(n, s));
    if (pf) pf.style.width = '100%'; if (ps) ps.innerHTML = '&#9989; ' + t('snap.deleted');
    toast(t('snap.deleted')); addEvt('LXC Snapshot deleted — ' + n + '@' + s);
    setTimeout(function() { closeModal(); ctrTab = 'snapshots'; renderContainers(document.getElementById('cb')); }, 1500);
  } catch (e) {
    if (pf) { pf.style.width = '100%'; pf.style.background = 'var(--red)'; }
    if (ps) ps.innerHTML = '&#10060; ' + esc(e.message); toast(e.message, false);
  }
}

async function ctrExec(n) {
  try { const c = await fetchGet(EP.CTR_LIST()); const l = unwrapList(c); const ct = l.find(x => x.name === n);
    if (!ct || ct.state !== 'RUNNING') { toast(n + ' is not running', false); return; } } catch (e) { if(_DEBUG) console.warn('c:', e.message); }
  selCtr = n; ctrTab = 'console'; renderContainers(document.getElementById('cb'));
}

function ctrDel(n) { showModal('<h2 class="color-red">&#9888; ' + t('ctr.destroying') + '</h2><p class="mb-12">' + t('ctr.delete.confirm') + ' <b class="color-accent">' + n + '</b></p><p class="mb-12">' + t('ctr.delete.type_name') + '</p><div class="fr"><label>Name</label><input id="del-ctr-confirm" placeholder="' + n + '"></div><div class="text-right mt-14"><button class="btn btn-r" onclick="doCtrDel(\'' + n + '\')">' + t('btn.delete') + '</button> <button class="btn" onclick="closeModal()">' + t('btn.cancel') + '</button></div>'); }

async function doCtrDel(n) {
  const c = document.getElementById('del-ctr-confirm')?.value; if (c !== n) { toast(t('vm.name_mismatch'), false); return; }
  const mc = document.getElementById('mc'); mc.innerHTML = '<h2 class="color-red">&#9888; ' + t('ctr.destroying') + '</h2><p><b class="color-accent">' + escapeHtml(n) + '</b></p><div class="prog-bar"><div class="prog-fill" id="dc-p" class="w-pct-10"></div></div><div class="prog-status" id="dc-s"><span class="spinner"></span>Stopping...</div>';
  const pf = document.getElementById('dc-p'), ps = document.getElementById('dc-s');
  try { pf.style.width = '30%'; ps.innerHTML = '<span class="spinner"></span>Destroying...';
    const d = await fetchDelete(EP.CTR_DETAIL(n)).catch(() => ({}));
    pf.style.width = '80%';
    if (d.error) { pf.style.background = 'var(--red)'; pf.style.width = '100%'; ps.innerHTML = '&#10060; ' + escapeHtml(d.error.message); toast(t('btn.delete') + ' failed', false); return; }
    pf.style.width = '100%'; ps.innerHTML = '&#9989; ' + t('ctr.destroyed'); toast(t('ctr.destroyed')); addEvt('LXC Destroyed — ' + n); selCtr = null; setTimeout(() => { closeModal(); renderContainers(document.getElementById('cb')); }, 1500);
  } catch (e) { pf.style.width = '100%'; ps.innerHTML = '&#10060; ' + escapeHtml(e.message); toast(e.message, false); }
}


function showCtrCreate() {
  if (typeof markFormDirty === 'function') markFormDirty('ctr-create');
  let h = '<h2>' + t('ctr.new') + '</h2>';

  h += '<div style="display:grid;grid-template-columns:1fr 1fr;gap:16px">';

  h += '<div>';
  h += '<h4 class="mb-8">&#9783; Basic</h4>';
  h += '<div class="fr"><label class="min-w-80">Name</label><input id="cc-name" placeholder="my-container" class="flex-1"></div>';
  h += '<div class="fr"><label class="min-w-80">Distribution</label><select id="cc-dist" onchange="ctrDistChanged()" style="flex:1;padding:6px;background:var(--bg);border:1px solid var(--border);color:var(--fg);border-radius:4px"><option value="ubuntu">Ubuntu</option><option value="debian">Debian</option><option value="alpine">Alpine</option><option value="centos">CentOS</option><option value="fedora">Fedora</option><option value="archlinux">Arch Linux</option></select></div>';
  h += '<div class="fr"><label class="min-w-80">Release</label><input id="cc-rel" value="jammy" placeholder="jammy / bookworm / 3.19" class="flex-1"></div>';
  h += '</div>';

  h += '<div>';
  h += '<h4 class="mb-8">&#127760; Network</h4>';
  h += '<div class="fr"><label class="min-w-70">Bridge</label><div class="flex gap-6 flex-1"><select id="cc-br" style="flex:1;padding:6px;background:var(--bg);border:1px solid var(--border);color:var(--fg);border-radius:4px"><option value="pcvbr0">pcvbr0 (default)</option></select><button class="btn text-xs" onclick="ctrLoadBridges()">&#128260;</button></div></div>';
  h += '<div class="fr"><label class="min-w-70">IP Mode</label><select id="cc-ipmode" onchange="ctrIpModeChanged()" style="flex:1;padding:6px;background:var(--bg);border:1px solid var(--border);color:var(--fg);border-radius:4px"><option value="dhcp">DHCP (auto)</option><option value="static">Static IP</option></select></div>';
  h += '<div class="fr hidden" id="cc-static-row"><label class="min-w-70">Static IP</label><input id="cc-ip" placeholder="10.0.3.100/24" class="flex-1"></div>';
  h += '<div class="fr hidden" id="cc-gw-row"><label class="min-w-70">Gateway</label><input id="cc-gw" placeholder="10.0.3.1" class="flex-1"></div>';
  h += '<div class="fr"><label class="min-w-70">DNS</label><input id="cc-dns" placeholder="8.8.8.8 (optional)" class="flex-1"></div>';
  h += '<h4 style="margin:12px 0 8px">&#9881; Resources</h4>';
  h += '<div class="fr"><label class="min-w-70">vCPU</label><input id="cc-vcpu" type="number" value="1" min="1" max="64" class="flex-1"></div>';
  h += '<div class="fr"><label class="min-w-70">Memory (MB)</label><input id="cc-mem" type="number" value="512" min="64" class="flex-1"></div>';
  h += '</div></div>';
  h += '<div class="text-right mt-14"><button class="btn btn-g" onclick="doCtrCreate()">' + t('btn.create') + '</button> <button class="btn" onclick="closeModal()">' + t('btn.cancel') + '</button></div>';

  showModal(h);
  var mc = document.getElementById('mc');
  if (mc) mc.classList.add('modal-wide');
  setTimeout(ctrLoadBridges, 80);
}

function ctrDistChanged() {
  const dist = document.getElementById('cc-dist')?.value;
  const rel = document.getElementById('cc-rel');
  if (!rel) return;
  const defaults = { ubuntu: 'jammy', debian: 'bookworm', alpine: '3.19', centos: '9-Stream', fedora: '39', archlinux: 'current' };
  rel.value = defaults[dist] || '';
}

function ctrIpModeChanged() {
  const mode = document.getElementById('cc-ipmode')?.value;
  document.getElementById('cc-static-row')?.classList.toggle('hidden', mode !== 'static');
  document.getElementById('cc-gw-row')?.classList.toggle('hidden', mode !== 'static');
}

async function ctrLoadBridges() {
  const sel = document.getElementById('cc-br'); if (!sel) return;
  try {
    const r = await fetchGet(EP.NET_LIST());
    const nl = unwrapList(r);
    let h = '';
    nl.forEach(n => { const name = n.name || ''; if (!name) return; const mode = n.mode || ''; const ip = n.ip_cidr || '';
      h += '<option value="' + escapeHtml(name) + '"' + (name === 'pcvbr0' ? ' selected' : '') + '>' + escapeHtml(name) + (mode ? ' (' + mode + ')' : '') + (ip ? ' ' + ip : '') + '</option>'; });
    if (!h) h = '<option value="pcvbr0">pcvbr0</option>';
    sel.innerHTML = h;
  } catch (e) { sel.innerHTML = '<option value="pcvbr0">pcvbr0</option>'; }
}

async function doCtrCreate() {
  const n = document.getElementById('cc-name')?.value?.trim();
  const dist = document.getElementById('cc-dist')?.value;
  const rel = document.getElementById('cc-rel')?.value;
  const br = document.getElementById('cc-br')?.value;
  const vcpu = parseInt(document.getElementById('cc-vcpu')?.value) || 1;
  const mem = parseInt(document.getElementById('cc-mem')?.value) || 512;
  const ipmode = document.getElementById('cc-ipmode')?.value;
  const ip = document.getElementById('cc-ip')?.value;
  const gw = document.getElementById('cc-gw')?.value;
  const dns = document.getElementById('cc-dns')?.value;
  if (!n) { toast(t('msg.name_required'), false); return; }


  if (typeof clearFormDirty === 'function') clearFormDirty('ctr-create');
  closeModal();
  toast('&#9783; ' + escapeHtml(n) + ' ' + _L('생성 시작...', 'Creating...'), 's');

  const body = { name: n, image: dist + ':' + rel, bridge: br || 'pcvbr0', vcpu_count: vcpu, memory_mb: mem };
  if (ipmode === 'static' && ip) { body.static_ip = ip; if (gw) body.gateway = gw; }
  if (dns) body.dns = dns;

  try {
    const r = await fetchPost(EP.CTR_LIST(), body);


    if (r && r.error) {
      var errMsg = r.error.message || r.error.data || JSON.stringify(r.error);
      toast('&#10060; ' + _L('컨테이너 생성 실패', 'Container creation failed') + ': ' + errMsg, false);
      return;
    }


    addEvt('LXC Creating — ' + n + ' (' + dist + ':' + rel + ', ' + br + ', ' + vcpu + 'vCPU, ' + mem + 'MB)');
    var created = false;
    for (var pi = 0; pi < 18; pi++) {
      await new Promise(function(res) { setTimeout(res, 5000); });
      try {
        var cl = await fetchGet(EP.CTR_LIST()); var list = unwrapList(cl);
        if (list.find(function(x) { return x.name === n; })) { created = true; break; }
      } catch(e2) {}
    }

    if (created) {
      toast('&#9989; ' + escapeHtml(n) + ' ' + _L('생성 완료', 'created'), 's');
      addEvt('LXC Created — ' + n);
      selCtr = n; ctrTab = 'summary';
      renderContainerList();
      if (document.getElementById('cb')) renderContainers(document.getElementById('cb'));
    } else {
      toast('&#9888; ' + escapeHtml(n) + ' ' + _L('생성 진행 중 — 잠시 후 확인하세요', 'Still creating — check later'), 'w');
    }
  } catch (e) {
    var errDetail = e && e.message ? e.message : '';
    toast('&#10060; ' + _L('컨테이너 생성 실패', 'Container creation failed') + (errDetail ? ': ' + errDetail : ''), false);
  }
}


async function ctrSetLimits(name, type) {
  const body = { name };
  if (type === 'cpu') {
    body.cpu_shares = parseInt(document.getElementById('ctr-cpu-shares')?.value) || 1024;
    body.cpu_quota = parseInt(document.getElementById('ctr-cpu-quota')?.value) || 100000;
  } else {
    body.memory_limit_mb = parseInt(document.getElementById('ctr-mem-limit')?.value) || 512;
    body.swap_limit_mb = parseInt(document.getElementById('ctr-swap-limit')?.value) || 0;
  }
  try {
    const r = await fetchPut(EP.CTR_LIMITS(name), body);
    if (r.error) { toast('Set limits failed: ' + (r.error.message || ''), false); return; }
    toast('Resource limits applied: ' + name); addEvt('CTR limits: ' + name);
  } catch (e) { toast(e.message, false); }
}


function ctrNicAdd(name) {
  showModal('<h2>Add NIC to ' + escapeHtml(name) + '</h2><div class="fr"><label>Bridge</label><input id="ctr-nic-br" value="pcvbr0" placeholder="pcvbr0"></div><div class="fr"><label>MAC (optional)</label><input id="ctr-nic-mac" placeholder="auto"></div><div class="text-right mt-12"><button class="btn btn-g" onclick="doCtrNicAdd(\'' + escapeHtml(name) + '\')">Add NIC</button> <button class="btn btn-r" onclick="closeModal()">' + t('btn.cancel') + '</button></div>');
}

async function doCtrNicAdd(name) {
  const bridge = document.getElementById('ctr-nic-br')?.value || 'pcvbr0';
  const mac = document.getElementById('ctr-nic-mac')?.value || '';
  try {
    const r = await fetchPost(EP.CTR_NICS(name), { bridge, hwaddr: mac || undefined });
    if (r.error) { toast('NIC add failed: ' + (r.error.message || ''), false); return; }
    toast('NIC added to ' + name + ' (bridge: ' + bridge + ')');
    addEvt('LXC NIC attached — ' + name + ' → ' + bridge);
    closeModal(); ctrTab = 'network'; renderContainers(document.getElementById('cb'));
  } catch (e) { toast(e.message, false); }
}

async function ctrNicDel(name, nicName) {
  if (!await customConfirm('Remove NIC', 'Remove ' + nicName + ' from ' + name + '?')) return;
  try {
    const r = await fetchDelete(EP.CTR_NICS(name) + '/' + encodeURIComponent(nicName));
    if (r.error) { toast('NIC remove failed: ' + (r.error.message || ''), false); return; }
    toast('NIC removed: ' + nicName);
    addEvt('LXC NIC detached — ' + name + '/' + nicName);
    ctrTab = 'network'; renderContainers(document.getElementById('cb'));
  } catch (e) { toast(e.message, false); }
}

async function ctrSetBandwidth(name) {
  const nic = document.getElementById('ctr-bw-nic')?.value || 'eth0';
  const inKbps = parseInt(document.getElementById('ctr-bw-in')?.value) || 0;
  const outKbps = parseInt(document.getElementById('ctr-bw-out')?.value) || 0;
  if (inKbps <= 0 && outKbps <= 0) { toast('Set at least one bandwidth limit', false); return; }
  try {
    const r = await fetchPut(EP.CTR_BANDWIDTH(name), { nic_name: nic, inbound_kbps: inKbps, outbound_kbps: outKbps });
    if (r.error) { toast('Bandwidth set failed: ' + (r.error.message || ''), false); return; }
    toast('Bandwidth QoS applied: ' + name + '/' + nic + ' (in: ' + inKbps + ' out: ' + outKbps + ' Kbps)');
    addEvt('LXC Bandwidth QoS — ' + name + '/' + nic + ' in:' + inKbps + ' out:' + outKbps + ' Kbps');
  } catch (e) { toast(e.message, false); }
}




window.setCtrSort = setCtrSort;
window.renderContainerList = renderContainerList;
window.renderContainers = renderContainers;
window.ctrRenderTab = ctrRenderTab;
window.ctrA = ctrA;
window.ctrRunCmd = ctrRunCmd;
window.ctrDnsAdd = ctrDnsAdd;
window.ctrReboot = ctrReboot;
window.ctrSnapCreate = ctrSnapCreate;
window.ctrSnapRb = ctrSnapRb;
window.ctrSnapDel = ctrSnapDel;
window.ctrExec = ctrExec;
window.ctrDel = ctrDel;
window.doCtrDel = doCtrDel;
window.showCtrCreate = showCtrCreate;
window.ctrDistChanged = ctrDistChanged;
window.ctrIpModeChanged = ctrIpModeChanged;
window.ctrLoadBridges = ctrLoadBridges;
window.doCtrCreate = doCtrCreate;
window.ctrSetLimits = ctrSetLimits;
window.ctrNicAdd = ctrNicAdd;
window.doCtrNicAdd = doCtrNicAdd;
window.ctrNicDel = ctrNicDel;
window.ctrSetBandwidth = ctrSetBandwidth;


async function showCtrClone(name) {
  var html = '<div class="form-group"><label>' + _L('원본', 'Source') + '</label>';
  html += '<input class="input-field" value="' + esc(name) + '" disabled></div>';
  html += '<div class="form-group"><label>' + _L('클론 이름', 'Clone Name') + '</label>';
  html += '<input id="ctr-clone-name" class="input-field" placeholder="' + esc(name) + '-clone"></div>';
  showModal(_L('컨테이너 클론', 'Clone Container'), html, async function() {
    var dst = document.getElementById('ctr-clone-name').value.trim();
    if (!dst) { toast(_L('이름 필수', 'Name required'), 'w'); return; }
    try {
      await fetchPost(EP.CTR_CLONE(name), { source: name, dest: dst });
      toast(_L('클론 요청 완료', 'Clone requested'), 's');
    } catch(e) { toast(_L('실패', 'Failed'), 'e'); }
  });
}


async function showCtrMemoryStats(name) {
  try {
    var r = await fetchGet(EP.CTR_MEMORY_STATS(name));
    var d = unwrapData(r);
    var h = '<h4>' + esc(name) + ' — ' + _L('메모리 상세', 'Memory Details') + '</h4>';
    h += '<table class="data-table text-11"><thead><tr>';
    h += '<th>' + _L('항목', 'Field') + '</th><th>' + _L('값', 'Value') + '</th></tr></thead><tbody>';
    var fields = ['anon', 'file', 'slab', 'sock', 'shmem', 'pgfault', 'pgmajfault'];
    fields.forEach(function(f) {
      if (d[f] !== undefined) {
        var val = d[f] > 1048576 ? (d[f] / 1048576).toFixed(1) + ' MB' : d[f];
        h += '<tr><td><b>' + esc(f) + '</b></td><td>' + val + '</td></tr>';
      }
    });
    Object.keys(d).forEach(function(k) {
      if (fields.indexOf(k) === -1 && k !== 'container') {
        h += '<tr><td class="color-muted">' + esc(k) + '</td><td>' + d[k] + '</td></tr>';
      }
    });
    h += '</tbody></table>';
    showModal(_L('메모리 상세', 'Memory Stats'), h);
  } catch(e) { toast(_L('로드 실패', 'Failed'), 'e'); }
}


async function checkCtrHealth(name) {
  try {
    var r = await fetchGet(EP.CTR_HEALTH(name));
    var d = unwrapData(r);
    var icon = d.running ? '🟢' : '🔴';
    toast(icon + ' ' + esc(name) + ': ' + (d.state || 'unknown'), d.running ? 's' : 'w');
  } catch(e) { toast(_L('헬스 체크 실패', 'Health check failed'), 'e'); }
}


window.showCtrClone = showCtrClone;
window.showCtrMemoryStats = showCtrMemoryStats;
window.checkCtrHealth = checkCtrHealth;


PCV.container = {
  setCtrSort: setCtrSort,
  renderContainerList: renderContainerList,
  renderContainers: renderContainers,
  ctrRenderTab: ctrRenderTab,
  ctrA: ctrA,
  ctrRunCmd: ctrRunCmd,
  ctrDnsAdd: ctrDnsAdd,
  ctrReboot: ctrReboot,
  ctrSnapCreate: ctrSnapCreate,
  ctrSnapRb: ctrSnapRb,
  ctrSnapDel: ctrSnapDel,
  ctrExec: ctrExec,
  ctrDel: ctrDel,
  doCtrDel: doCtrDel,
  showCtrCreate: showCtrCreate,
  ctrDistChanged: ctrDistChanged,
  ctrIpModeChanged: ctrIpModeChanged,
  ctrLoadBridges: ctrLoadBridges,
  doCtrCreate: doCtrCreate,
  ctrSetLimits: ctrSetLimits,
  ctrNicAdd: ctrNicAdd,
  doCtrNicAdd: doCtrNicAdd,
  ctrNicDel: ctrNicDel,
  ctrSetBandwidth: ctrSetBandwidth,
  showCtrClone: showCtrClone,
  showCtrMemoryStats: showCtrMemoryStats,
  checkCtrHealth: checkCtrHealth
};

})(window.PCV);

window.showCtrClone = showCtrClone;
window.showCtrMemoryStats = showCtrMemoryStats;
window.checkCtrHealth = checkCtrHealth;
