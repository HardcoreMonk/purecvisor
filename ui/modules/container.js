/* ═══════════════════════════════════════════════════════════════
   PureCVisor — modules/container.js
   Container List, Rendering, Actions, Create Wizard, NIC Ops
   ADR-0013: IIFE module scope — PCV.container namespace
   ═══════════════════════════════════════════════════════════════ */
window.PCV = window.PCV || {};
(function(PCV) {

/* ═══ CONTAINER STATE VARIABLES ═══ */
/* These must be on window.* because inline onclick handlers (e.g. selCtr='name')
   set window-scoped globals. Functions here must read/write window.* to stay in sync. */
window.selCtr = window.selCtr || null;
window.ctrTab = window.ctrTab || 'summary';
window.ctrHist = window.ctrHist || [];

/* ═══ CONTAINER SORT ═══ */
window.ctrSortKey = window.ctrSortKey || 'name';
window.ctrSortDir = window.ctrSortDir || 1;
function setCtrSort(k) {
  if (window.ctrSortKey === k) window.ctrSortDir *= -1; else { window.ctrSortKey = k; window.ctrSortDir = 1; }
  renderContainerList();
}

/* ═══ SIDEBAR CONTAINER LIST ═══ */
async function renderContainerList() {
  const el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  const listEl = document.getElementById('ctr-list');
  if (!listEl) return;
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
      clearEl(listEl);
      listEl.appendChild(el('div', { class: 'empty-state', style: 'padding:24px;text-align:center' },
        el('div', { style: 'font-size:32px;margin-bottom:8px' }, '☷'),
        el('div', { class: 'text-xs color-muted' }, t('msg.no_containers')),
        el('button', { class: 'btn btn-g mt-12 text-11', onclick: 'showCtrCreate()' }, '+ ' + t('ctr.new'))));
      return;
    }
    const items = fl.map(v => {
      const on = v.state === 'RUNNING';
      const s = selCtr === v.name;
      return el('div', {
        onclick: "selCtr='" + v.name + "';currentTab='containers';renderContent();renderContainerList()",
        class: 'vm-item' + (s ? ' sel' : ''),
        style: 'padding:6px 8px;cursor:pointer;border-left:3px solid ' + (s ? 'var(--accent)' : 'transparent') + ';background:' + (s ? 'rgba(0,240,255,.06)' : 'transparent')
      },
        el('div', { class: 'flex items-center gap-6' },
          el('span', { style: 'font-size:8px;color:' + (on ? 'var(--green)' : 'var(--red)') }, '●'),
          el('span', { style: 'font-size:12px;font-weight:' + (s ? '700' : '400') }, v.name)),
        el('div', { style: 'display:flex;justify-content:space-between;margin-left:14px;margin-top:1px' },
          el('span', { class: 'stat-label text-xs' }, on ? t('status.running') : t('status.stopped')),
          (on && v.ip_addr) ? el('span', { class: 'stat-label text-xs color-green' }, v.ip_addr) : null));
    });
    clearEl(listEl);
    listEl.appendChild(frag(items));
  } catch (e) { PCV.uxlib.setMsg(listEl, null, { tag: 'div', cls: 'text-xs color-muted', style: 'padding:8px' }, 'Failed to load containers.'); }
}

/* ═══ MAIN CONTAINER PANEL ═══ */
async function renderContainers(b) {
  const el = PCV.uxlib.el, clearEl = PCV.uxlib.clearEl;
  showSkeleton(b);
  try {
    const c = await fetchGet(EP.CTR_LIST());
    const l = unwrapList(c);
    if (l.length === 0 && typeof emptyStatePro === 'function') {
      PCV.uxlib.clearEl(b);
      b.appendChild(emptyStatePro({
        icon: '&#9783;',
        title: _L('컨테이너가 없습니다', 'No containers'),
        desc: _L('첫 LXC 컨테이너를 만들어보세요. ZFS 백엔드 + cloud-init 자동.', 'Create your first LXC container with ZFS backend.'),
        ctaLabel: _L('+ 컨테이너 만들기', '+ Create Container'),
        ctaAction: 'showCtrCreate()'
      }));
      return;
    }
    const listItems = l.map(v => {
      const on = v.state === 'RUNNING';
      const s = selCtr === v.name;
      return el('div', {
        onclick: "selCtr='" + escapeAttr(v.name) + "';ctrTab='summary';renderContainers(document.getElementById('cb'))",
        style: 'padding:6px 8px;cursor:pointer;border-radius:4px;margin-bottom:2px;border-left:3px solid ' + (s ? 'var(--accent)' : 'transparent') + ';background:' + (s ? 'var(--bg3)' : 'transparent')
      },
        el('div', { class: 'flex items-center gap-6' },
          el('span', { style: 'font-size:9px;color:' + (on ? 'var(--green)' : 'var(--fg2)') }, '●'),
          el('span', { style: 'font-size:13px;font-weight:' + (s ? '600' : '400') }, v.name)),
        el('div', { class: 'stat-label', style: 'margin-left:15px' }, v.state + (on ? ' • ' + (v.ip_addr || '') : '')));
    });
    const sidebar = el('div', { style: 'min-width:220px;max-width:220px;border-right:1px solid var(--border);overflow-y:auto;padding:8px' },
      el('div', { class: 'justify-between items-center mb-8' },
        el('span', { class: 'text-xs font-bold', style: 'text-transform:uppercase;letter-spacing:.06em;color:var(--fg2)' }, t('nav.containers')),
        el('div', { class: 'flex gap-6 items-center' },
          HN.badge(String(l.length), 'y'),
          el('button', { class: 'btn btn-g', onclick: 'showCtrCreate()', style: 'font-size:11px;padding:4px 10px' }, '+ ' + t('ctr.new')))),
      listItems);
    const cv = l.find(x => x.name === selCtr);
    const detailKids = [];
    if (cv) {
      const on = cv.state === 'RUNNING';
      const actions = [];
      if (!on) actions.push(el('button', { class: 'btn btn-g text-12 px-12 py-4', onclick: "ctrA('" + escapeAttr(cv.name) + "','start')" }, '▶ ' + t('power.start')));
      if (on) {
        actions.push(el('button', { class: 'btn btn-r text-12 px-12 py-4', onclick: "ctrA('" + escapeAttr(cv.name) + "','stop')" }, '■ ' + t('power.stop')));
        actions.push(el('button', { class: 'btn text-12 px-12 py-4', onclick: "ctrReboot('" + escapeAttr(cv.name) + "')" }, '↻ Reboot'));
      }
      actions.push(el('button', { class: 'btn btn-r text-12 px-12 py-4', onclick: "ctrDel('" + escapeAttr(cv.name) + "')" }, '🗑 ' + t('btn.delete')));
      detailKids.push(el('div', { class: 'justify-between items-center', style: 'padding:10px 14px;border-bottom:1px solid var(--border)' },
        el('div', null,
          el('span', { style: 'font-size:15px;font-weight:700' }, cv.name),
          ' ',
          HN.badge(cv.state, on ? 'g' : 'r')),
        el('div', { class: 'flex gap-4' }, actions)));
      const tabs = ['summary', 'console', 'resources', 'network', 'dns', 'options', 'snapshots', 'notes', 'tasks'];
      detailKids.push(el('div', { class: 'flex', style: 'border-bottom:1px solid var(--border);padding:0 10px;gap:2px;overflow-x:auto' },
        tabs.map(t2 => el('div', {
          onclick: "ctrTab='" + t2 + "';renderContainers(document.getElementById('cb'))",
          style: 'padding:9px 14px;font-size:13px;cursor:pointer;white-space:nowrap;border-bottom:2px solid ' + (ctrTab === t2 ? 'var(--accent)' : 'transparent') + ';color:' + (ctrTab === t2 ? 'var(--accent)' : 'var(--fg2)') + ';font-weight:' + (ctrTab === t2 ? '600' : '400') + ';transition:color .15s'
        }, t2.charAt(0).toUpperCase() + t2.slice(1)))));
      detailKids.push(el('div', { id: 'ctr-tab-content', style: 'padding:14px;flex:1' }));
    } else {
      detailKids.push(el('div', { style: 'flex:1;display:flex;align-items:center;justify-content:center;color:var(--fg2)' },
        el('div', { class: 'text-center' },
          el('div', { style: 'font-size:32px;margin-bottom:8px' }, '☷'),
          el('p', null, t('ctr.select')))));
    }
    const detail = el('div', { style: 'flex:1;overflow-y:auto;display:flex;flex-direction:column' }, detailKids);
    clearEl(b);
    b.appendChild(el('div', { style: 'display:flex;gap:0;height:calc(100vh - 280px);min-height:400px' }, sidebar, detail));
    if (cv) { const tb = document.getElementById('ctr-tab-content'); if (tb) await ctrRenderTab(tb, cv); }
  } catch (e) { PCV.uxlib.setMsg(b, null, { tag: 'p', cls: 'color-red' }, e.message); }
}

/* ═══ CONTAINER TAB RENDERING ═══ */
/* renderProgressBar(p,c) 의 노드 등가물 (app.js _progressBar 선례). 문자열 반환
 * 헬퍼를 카드 body 노드 조립에 끼워넣기 위한 로컬 헬퍼. */
function _ctrProgressBar(p, c) {
  const el = PCV.uxlib.el;
  const cl = p > 85 ? 'var(--red)' : p > 60 ? 'var(--yellow)' : 'var(--green)';
  const anim = p > 85 ? ' pulse-anim' : '';
  return el('div', { class: 'pb' + anim },
    el('div', { class: 'pb-f scan-anim', style: 'width:' + p + '%;background:' + (c || cl) }),
    el('div', { class: 'pb-t' }, p.toFixed(1) + '%'));
}

async function ctrRenderTab(tb, cv) {
  const el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  const n = cv.name;
  const on = cv.state === 'RUNNING';
  if (ctrTab === 'summary') {
    let m = {}; if (on) { try { const r = await fetchGet(EP.CTR_METRICS(n)); m = unwrapData(r); } catch (e) { if(_DEBUG) console.warn('ctrRenderTab:', e.message); } }
    let info = { hostname: n, os: '', uptime: '', procs: '', kernel: '' };
    if (on) { const cmds = [['hostname', 'hostname'], ['uptime', 'uptime -p'], ['nproc', 'nproc'], ['kernel', 'uname -r']];
      for (const [k, c] of cmds) { try { const r = await fetchPost(EP.CTR_EXEC(n), { command: c }); info[k] = unwrapData(r).output?.trim() || ''; } catch (e) { if(_DEBUG) console.warn('ctrRenderTab:', e.message); } } }
    const cpu = m.cpu_percent || 0, mem_u = m.mem_used_mb || 0, mem_l = m.mem_limit_mb || 0, mem_p = mem_l > 0 ? mem_u / mem_l * 100 : 0;
    const nrx = m.net_rx_mb || 0, ntx = m.net_tx_mb || 0;
    clearEl(tb);
    tb.appendChild(frag(
      HN.grid(4,
        HN.card('Status', [
          el('div', { class: 'stat-lg', style: 'color:' + (on ? 'var(--green)' : 'var(--fg2)') }, on ? t('status.running') : t('status.stopped')),
          el('div', { class: 'stat-label mt-4' }, on ? 'PID: ' + (m.init_pid || '-') : '')
        ]),
        HN.card('CPU', [
          el('div', { class: 'stat-md' }, cpu.toFixed(1) + '%'),
          _ctrProgressBar(cpu),
          el('div', { class: 'stat-label' }, (info.nproc || '?') + ' cores')
        ]),
        HN.card('Memory', [
          el('div', { class: 'stat-md' }, mem_l > 0 ? mem_p.toFixed(1) + '%' : mem_u.toFixed(0) + ' MB'),
          mem_l > 0 ? _ctrProgressBar(mem_p) : null
        ]),
        HN.card('Network', [
          HN.row('RX', nrx.toFixed(1) + ' MB'),
          HN.row('TX', ntx.toFixed(1) + ' MB'),
          HN.row('IP', el('span', { class: 'color-accent' }, cv.ip_addr || '-'))
        ])),
      el('div', { class: 'sg grid-2 mt-12' },
        HN.card('System Info', [
          HN.row('Hostname', info.hostname || '-'),
          HN.row('Uptime', info.uptime || '-'),
          HN.row('Kernel', info.kernel || '-'),
          HN.row('Image', cv.image || '-')
        ]),
        HN.card('Configuration', [
          HN.row('Bridge', 'pcvbr0'),
          HN.row('AppArmor', 'unconfined'),
          HN.row('Type', 'LXC (unprivileged: no)'),
          HN.row('Node', location.hostname)
        ]))
    ));
  } else if (ctrTab === 'console') {
    if (!on) { clearEl(tb); tb.appendChild(HN.card('⌨ ' + t('tab.console'), el('p', { class: 'color-muted' }, t('ctr.console.stopped')))); return; }
    clearEl(tb);
    tb.appendChild(
      el('div', { style: 'background:var(--bg);border:1px solid var(--border);border-radius:var(--r);padding:0;font-family:monospace;display:flex;flex-direction:column;height:100%' },
        el('div', { class: 'justify-between stat-label', style: 'padding:6px 10px;border-bottom:1px solid var(--border)' },
          el('span', null, '⌨ ' + n + ' — Shell'),
          el('span', { class: 'color-green' }, t('connected'))),
        el('pre', { id: 'ctr-output', style: 'flex:1;padding:8px 10px;margin:0;overflow-y:auto;font-size:11px;color:var(--green);white-space:pre-wrap;min-height:250px;max-height:400px' }, 'root@' + n + ':~# \n'),
        el('div', { class: 'flex', style: 'border-top:1px solid var(--border)' },
          el('span', { style: 'padding:6px 8px;color:var(--green);font-size:12px' }, '$'),
          el('input', { 'aria-label': 'Type command...', id: 'ctr-cmd', placeholder: 'Type command...', onkeydown: "if(event.key==='Enter')ctrRunCmd('" + n + "')", style: 'flex:1;background:transparent;border:none;color:var(--fg);font-family:monospace;font-size:12px;padding:6px 0;outline:none' }))));
    setTimeout(() => { document.getElementById('ctr-cmd')?.focus(); }, 100);
  } else if (ctrTab === 'resources') {
    let info = { cpu: '', mem: '', disk: '', procs: '' };
    if (on) { for (const [k, c] of [['cpu', 'nproc'], ['mem', 'free -h | head -2'], ['disk', 'df -h / 2>/dev/null | tail -1'], ['procs', 'ps aux --no-headers 2>/dev/null | wc -l']]) { try { const r = await fetchPost(EP.CTR_EXEC(n), { command: c }); info[k] = unwrapData(r).output?.trim() || '-'; } catch (e) { info[k] = '-'; } } }
    const memLines = (info.mem || '').split('\n'); const memHeader = memLines[0] || ''; const memData = memLines[1] || '';
    clearEl(tb);
    tb.appendChild(frag(
      el('h3', { class: 'section-title-md' }, 'Resources'),
      el('div', { class: 'sg' },
        HN.card('CPU', [HN.row('Cores', info.cpu || '-'), HN.row('Type', 'host (KVM passthrough)')]),
        HN.card('Memory', el('pre', { class: 'stat-label', style: 'margin:0;white-space:pre;overflow-x:auto' }, memHeader + '\n' + memData)),
        HN.card('Disk (rootfs)', el('pre', { class: 'stat-label', style: 'margin:0;white-space:pre;overflow-x:auto' }, info.disk || 'N/A')),
        HN.card('Processes', HN.row('Running', info.procs || '-'))),
      el('h3', { class: 'section-title-md mt-14' }, 'Resource Limits (cgroup v2)'),
      el('div', { class: 'sg grid-2' },
        HN.card('Set CPU Limit', [
          el('div', { class: 'fr' }, el('label', { for: 'ctr-cpu-shares' }, 'CPU Shares'), el('input', { id: 'ctr-cpu-shares', type: 'number', value: '1024' })),
          el('div', { class: 'fr' }, el('label', { for: 'ctr-cpu-quota' }, 'CPU Quota (µs)'), el('input', { id: 'ctr-cpu-quota', type: 'number', value: '100000' })),
          el('button', { class: 'btn btn-g mt-8', onclick: "ctrSetLimits('" + n + "','cpu')" }, 'Apply CPU Limit')
        ]),
        HN.card('Set Memory Limit', [
          el('div', { class: 'fr' }, el('label', { for: 'ctr-mem-limit' }, 'Memory Limit (MB)'), el('input', { id: 'ctr-mem-limit', type: 'number', value: '512' })),
          el('div', { class: 'fr' }, el('label', { for: 'ctr-swap-limit' }, 'Swap Limit (MB)'), el('input', { id: 'ctr-swap-limit', type: 'number', value: '0' })),
          el('button', { class: 'btn btn-g mt-8', onclick: "ctrSetLimits('" + n + "','mem')" }, 'Apply Memory Limit')
        ]))
    ));
  } else if (ctrTab === 'network') {
    clearEl(tb);
    tb.appendChild(frag(
      el('h3', { class: 'section-title-md' }, 'Network Interfaces ',
        el('button', { class: 'btn btn-g', onclick: "ctrNicAdd('" + n + "')", style: 'font-size:10px;margin-left:8px' }, '+ Add NIC')),
      el('div', { id: 'ctr-nic-list' }, el('span', { class: 'spinner' }), ' Loading NICs...'),
      el('h3', { class: 'section-title-md mt-14' }, 'Bandwidth QoS'),
      el('div', { class: 'sg grid-2' },
        HN.card('Set Bandwidth Limit', [
          el('div', { class: 'fr' }, el('label', { for: 'ctr-bw-nic' }, 'Interface'), el('input', { id: 'ctr-bw-nic', value: 'eth0', class: 'w-80' })),
          el('div', { class: 'fr' }, el('label', { for: 'ctr-bw-in' }, 'Inbound (Kbps)'), el('input', { id: 'ctr-bw-in', type: 'number', value: '0', placeholder: '0 = unlimited' })),
          el('div', { class: 'fr' }, el('label', { for: 'ctr-bw-out' }, 'Outbound (Kbps)'), el('input', { id: 'ctr-bw-out', type: 'number', value: '0', placeholder: '0 = unlimited' })),
          el('button', { class: 'btn btn-g mt-8', onclick: "ctrSetBandwidth('" + n + "')" }, 'Apply QoS')
        ]),
        HN.card('Routing & Addresses', el('div', { id: 'ctr-net-info' }, el('span', { class: 'spinner' }))))
    ));
    /* Load NIC list */
    try {
      const r = await fetchGet(EP.CTR_NICS(n));
      const nics = unwrapList(r);
      const nicEl = document.getElementById('ctr-nic-list');
      if (nicEl) {
        if (nics.length === 0) { PCV.uxlib.setMsg(nicEl, null, { tag: 'p', cls: 'color-muted' }, 'No NICs configured'); }
        else {
          const rows = nics.map(nc => el('tr', null,
            el('td', null, el('b', null, nc.name || '-')),
            el('td', null, nc.type || 'veth'),
            el('td', null, HN.badge(escapeHtml(nc.bridge || '-'), 'y')),
            el('td', { class: 'text-xs' }, nc.hwaddr || 'auto'),
            el('td', { class: 'color-accent' }, nc.ipv4 || '-'),
            el('td', null,
              nc.name !== 'eth0'
                ? el('button', { class: 'btn btn-r', onclick: "ctrNicDel('" + n + "','" + nc.name + "')", style: 'font-size:9px;padding:2px 6px' }, 'Remove')
                : el('span', { class: 'color-muted text-xs' }, 'primary'))));
          clearEl(nicEl);
          nicEl.appendChild(el('table', { class: 'table-sticky' },
            el('thead', null, el('tr', null,
              el('th', null, 'Name'), el('th', null, 'Type'), el('th', null, 'Bridge'),
              el('th', null, 'MAC'), el('th', null, 'IPv4'), el('th', null, 'Actions'))),
            el('tbody', null, rows)));
        }
      }
    } catch (e) {
      const nicEl = document.getElementById('ctr-nic-list');
      if (nicEl) { clearEl(nicEl); nicEl.appendChild(HN.card('Interface eth0', [
        HN.row('IP Address', el('span', { class: 'color-accent' }, cv.ip_addr || '-')),
        HN.row('Bridge', 'pcvbr0'),
        HN.row('Type', 'veth')])); }
    }
    /* Load routing info */
    if (on) {
      try { const r = await fetchPost(EP.CTR_EXEC(n), { command: 'ip -4 addr show 2>/dev/null; echo "---"; ip route 2>/dev/null | head -5' }); const ni = document.getElementById('ctr-net-info'); if (ni) { clearEl(ni); ni.appendChild(el('pre', { class: 'stat-label', style: 'margin:0;white-space:pre-wrap;overflow-x:auto' }, unwrapData(r).output || '')); } } catch (e) { const ni = document.getElementById('ctr-net-info'); if (ni) PCV.uxlib.setMsg(ni, null, { cls: 'color-muted' }, 'Unable to fetch'); }
    } else {
      const ni = document.getElementById('ctr-net-info'); if (ni) PCV.uxlib.setMsg(ni, null, { cls: 'color-muted' }, 'Container is stopped');
    }
  } else if (ctrTab === 'dns') {
    let dns = ''; if (on) { try { const r = await fetchPost(EP.CTR_EXEC(n), { command: 'cat /etc/resolv.conf 2>/dev/null' }); dns = unwrapData(r).output || ''; } catch (e) { if(_DEBUG) console.warn('dns:', e.message); } }
    clearEl(tb);
    tb.appendChild(frag(
      el('h3', { class: 'section-title-md' }, 'DNS'),
      HN.card('Resolver Configuration',
        on
          ? [
              el('pre', { style: 'font-size:11px;color:var(--fg);margin:8px 0;white-space:pre-wrap;background:var(--bg);padding:10px;border-radius:4px;border:1px solid var(--border)' }, dns),
              el('div', { class: 'mt-8' },
                el('div', { class: 'fr' },
                  el('label', { for: 'dns-ns' }, 'Add Nameserver'),
                  el('input', { id: 'dns-ns', placeholder: '8.8.8.8', class: 'flex-1' }),
                  el('button', { class: 'btn btn-g', onclick: "ctrDnsAdd('" + n + "')", style: 'margin-left:6px' }, 'Add')))
            ]
          : el('p', { class: 'color-muted' }, t('ctr.console.stopped')))
    ));
  } else if (ctrTab === 'options') {
    clearEl(tb);
    tb.appendChild(frag(
      el('h3', { class: 'section-title-md' }, 'Options'),
      el('div', { class: 'sg' },
        HN.card('General', [HN.row('Start on boot', 'No'), HN.row('Start order', '---'), HN.row('Protection', 'No'), HN.row('Unprivileged', 'No')]),
        HN.card('Security', [HN.row('AppArmor', 'unconfined'), HN.row('Keyctl', 'No'), HN.row('Nesting', 'No'), HN.row('FUSE', 'No')]),
        HN.card('Signals', [HN.row('Halt', 'SIGRTMIN+3'), HN.row('Reboot', 'SIGTERM')]))
    ));
  } else if (ctrTab === 'snapshots') {
    clearEl(tb);
    tb.appendChild(frag(
      el('h3', { class: 'section-title-md' }, t('vm.snapshot') + ' ',
        el('button', { class: 'btn btn-g', onclick: "ctrSnapCreate('" + n + "')", style: 'font-size:10px;margin-left:8px' }, '+ ' + t('btn.create'))),
      el('div', { id: 'ctr-snap-list' }, el('span', { class: 'spinner' }), ' ' + t('loading'))));
    try {
      const r = await fetchGet(EP.CTR_SNAPSHOTS(n)).catch(() => ({ data: [] }));
      const sl = unwrapList(r);
      const tbodyKids = [];
      if (sl.length === 0) tbodyKids.push(el('tr', null, el('td', { colspan: '2', class: 'color-muted' }, t('snap.none'))));
      sl.forEach(s => { const sn = typeof s === 'string' ? s : (s.name || s); tbodyKids.push(el('tr', null,
        el('td', null, sn),
        el('td', null,
          el('button', { class: 'btn text-9', onclick: "ctrSnapRb('" + n + "','" + sn + "')" }, 'Rollback'),
          ' ',
          el('button', { class: 'btn btn-r text-9', onclick: "ctrSnapDel('" + n + "','" + sn + "')" }, t('btn.delete'))))); });
      const tableNode = el('table', { class: 'table-sticky' },
        el('thead', null, el('tr', null, el('th', null, 'Name'), el('th', null, t('vm.settings')))),
        el('tbody', null, tbodyKids));
      const snapEl = document.getElementById('ctr-snap-list'); clearEl(snapEl); snapEl.appendChild(tableNode);
    } catch (e) { PCV.uxlib.setMsg('ctr-snap-list', null, { tag: 'p', cls: 'color-muted' }, t('snap.none')); }
  } else if (ctrTab === 'notes') {
    clearEl(tb);
    tb.appendChild(frag(
      el('h3', { class: 'section-title-md' }, 'Notes'),
      HN.card('Container Notes', [
        el('textarea', { 'aria-label': 'Add notes...', id: 'ctr-notes', placeholder: 'Add notes...', style: 'width:100%;min-height:150px;background:var(--bg);border:1px solid var(--border);border-radius:4px;color:var(--fg);padding:10px;font-family:monospace;font-size:12px;resize:vertical' }, localStorage.getItem('ctr-note-' + n) || ''),
        el('button', { class: 'btn mt-8', onclick: "localStorage.setItem('ctr-note-" + n + "',document.getElementById('ctr-notes').value);toast('" + t('btn.save') + "')" }, t('btn.save'))
      ])));
  } else if (ctrTab === 'tasks') {
    clearEl(tb);
    tb.appendChild(frag(
      el('h3', { class: 'section-title-md' }, 'Task History'),
      HN.card('Recent Events',
        el('div', { style: 'max-height:300px;overflow-y:auto;font-size:11px;font-family:monospace;color:var(--accent)' },
          eventLog.filter(e => { var s = (e.msg || e.raw || String(e)).toLowerCase(); return s.includes('ctr') || s.includes(n.toLowerCase()); })
            .map(e => el('div', { style: 'padding:2px 0;border-bottom:1px solid var(--border)' }, e.msg || e.raw || String(e))),
          el('div', { class: 'color-muted', style: 'padding:4px 0' }, eventLog.length + ' total events')))));
  }
}

/* ═══ CONTAINER ACTIONS ═══ */
async function ctrA(n, a) {
  const el = PCV.uxlib.el;
  /* prog-fill: 원본 class 속성 중복(class="prog-fill" ... class="w-pct-10") — HTML
   * 파서가 첫 class만 적용하고 w-pct-* 는 무시. 노드도 class:'prog-fill' 만
   * 유지(렌더 동등, 7차 doCtrDel 주석 선례). 이하 진행모달 전부 동일. */
  showModal([
    el('h2', null, (a === 'start' ? '▶ ' + t('ctr.starting') : '■ ' + t('ctr.stopping'))),
    el('p', { class: 'mb-8' }, el('b', { class: 'color-accent' }, n)),
    el('div', { class: 'prog-bar' }, el('div', { class: 'prog-fill', id: 'ctr-prog' })),
    el('div', { class: 'prog-status', id: 'ctr-st' }, el('span', { class: 'spinner' }), 'Sending ' + a + ' command...')
  ]);
  const pf = document.getElementById('ctr-prog'), ps = document.getElementById('ctr-st');
  try {
    pf.style.width = '30%'; PCV.uxlib.setMsg(ps, 'loading', null, 'Waiting for container ' + a + '...');
    const d = await fetchPost(a === 'start' ? EP.CTR_START(n) : EP.CTR_STOP(n), {});
    pf.style.width = '60%';
    if (d.error) { pf.style.background = 'var(--red)'; pf.style.width = '100%'; PCV.uxlib.setMsg(ps, null, null, '❌ ' + a + ' failed: ' + (d.error.message || 'Unknown error')); toast(a + ' failed', false); return; }
    PCV.uxlib.setMsg(ps, 'loading', null, a, ' completed, refreshing...');
    if (a === 'start') {
      for (let i = 0; i < 8; i++) { pf.style.width = (65 + i * 4) + '%'; await new Promise(r => setTimeout(r, 1500));
        try { const c = await fetchGet(EP.CTR_LIST()); const l = unwrapList(c); const ct = l.find(x => x.name === n);
          if (ct && ct.state === 'RUNNING') { const ip = ct.ip_addr || ct.ip || '';
            if (ip && ip !== 'N/A') { pf.style.width = '100%'; PCV.uxlib.setMsg(ps, null, null, '✅ Running — IP: ', PCV.uxlib.el('b', { class: 'color-green' }, ip)); toast(n + ' started (' + ip + ')'); addEvt('LXC Started — ' + n + ', IP: ' + ip); setTimeout(closeModal, 2500); renderContainers(document.getElementById('cb')); return; }
            PCV.uxlib.setMsg(ps, 'loading', null, 'Running, waiting for DHCP IP... (', (i + 1), '/8)'); } } catch (e) { if(_DEBUG) console.warn('c:', e.message); } }
      pf.style.width = '100%'; PCV.uxlib.setMsg(ps, null, null, '✅ Running (IP pending)'); toast(n + ' started'); addEvt('LXC Started — ' + n + ' (IP pending)');
    } else { pf.style.width = '100%'; PCV.uxlib.setMsg(ps, null, null, '✅ Container stopped'); toast(n + ' stopped'); addEvt('LXC Stopped — ' + n); }
    setTimeout(() => { closeModal(); renderContainers(document.getElementById('cb')); }, 2e3);
  } catch (e) { if (pf) pf.style.width = '100%'; if (ps) PCV.uxlib.setMsg(ps, null, null, '✅ ' + a + ' requested'); toast(n + ' ' + a); addEvt('LXC ' + a + ' — ' + n); setTimeout(() => { closeModal(); renderContainers(document.getElementById('cb')); }, 2500); }
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
  var el = PCV.uxlib.el;
  showModal([
    el('h2', null, '🔄 Rebooting'),
    el('p', null, el('b', { class: 'color-accent' }, n)),
    el('div', { class: 'prog-bar' }, el('div', { class: 'prog-fill', id: 'cr-p' })),
    el('div', { class: 'prog-status', id: 'cr-s' }, el('span', { class: 'spinner' }), ' Stopping...')
  ]);
  var pf = document.getElementById('cr-p'), ps = document.getElementById('cr-s');
  try {
    if (pf) pf.style.width = '30%';
    await fetchPost(EP.CTR_STOP(n), {});
    if (pf) pf.style.width = '50%'; if (ps) PCV.uxlib.setMsg(ps, 'loading', null, 'Waiting...');
    await new Promise(function(r) { setTimeout(r, 2000); });
    if (pf) pf.style.width = '70%'; if (ps) PCV.uxlib.setMsg(ps, 'loading', null, 'Starting...');
    await fetchPost(EP.CTR_START(n), {});
    if (pf) pf.style.width = '100%'; if (ps) PCV.uxlib.setMsg(ps, null, null, '✅ Reboot complete');
    toast(n + ' rebooted'); addEvt('LXC Reboot — ' + n);
    setTimeout(function() { closeModal(); renderContainers(document.getElementById('cb')); }, 2000);
  } catch (e) {
    if (pf) { pf.style.width = '100%'; pf.style.background = 'var(--red)'; }
    if (ps) PCV.uxlib.setMsg(ps, null, null, '❌ Reboot error: ' + e.message);
    toast(t('msg.reboot_error'), false);
  }
}

async function ctrSnapCreate(n) { var s = await showInputModal(t('snap.name_prompt') || 'Snapshot name', t('snap.name_prompt') || 'Name', 'snap-' + Date.now()); if (!s) return;
  var el = PCV.uxlib.el;
  showModal([
    el('h2', null, '📷 ' + t('snap.created')),
    el('p', null, el('b', { class: 'color-accent' }, n + '@' + s)),
    el('div', { class: 'prog-bar' }, el('div', { class: 'prog-fill', id: 'cs-p' })),
    el('div', { class: 'prog-status', id: 'cs-s' }, el('span', { class: 'spinner' }), ' Creating snapshot...')
  ]);
  var pf = document.getElementById('cs-p'), ps = document.getElementById('cs-s');
  try {
    if (pf) pf.style.width = '60%';
    await fetchPost(EP.CTR_SNAPSHOTS(n), { snap_name: s });
    if (pf) pf.style.width = '100%'; if (ps) PCV.uxlib.setMsg(ps, null, null, '✅ ' + t('snap.created') + ': ' + s);
    toast(t('snap.created') + ': ' + s); addEvt('LXC Snapshot created — ' + n + '@' + s);
    setTimeout(function() { closeModal(); ctrTab = 'snapshots'; renderContainers(document.getElementById('cb')); }, 1500);
  } catch (e) {
    if (pf) { pf.style.width = '100%'; pf.style.background = 'var(--red)'; }
    if (ps) PCV.uxlib.setMsg(ps, null, null, '❌ ' + e.message); toast(e.message, false);
  }
}
async function ctrSnapRb(n, s) { if (!await customConfirm('Rollback', n + ' → ' + s + '?')) return;
  var el = PCV.uxlib.el;
  showModal([
    el('h2', null, '⏪ Rollback'),
    el('p', null, el('b', { class: 'color-accent' }, n + '@' + s)),
    el('div', { class: 'prog-bar' }, el('div', { class: 'prog-fill', id: 'crb-p' })),
    el('div', { class: 'prog-status', id: 'crb-s' }, el('span', { class: 'spinner' }), ' Rolling back...')
  ]);
  var pf = document.getElementById('crb-p'), ps = document.getElementById('crb-s');
  try {
    if (pf) pf.style.width = '60%';
    await fetchPost(EP.CTR_SNAP_ROLLBACK(n), { snap_name: s });
    if (pf) pf.style.width = '100%'; if (ps) PCV.uxlib.setMsg(ps, null, null, '✅ ' + t('snap.reverted'));
    toast(t('snap.reverted')); addEvt('LXC Snapshot rollback — ' + n + '@' + s);
    setTimeout(function() { closeModal(); renderContainers(document.getElementById('cb')); }, 1500);
  } catch (e) {
    if (pf) { pf.style.width = '100%'; pf.style.background = 'var(--red)'; }
    if (ps) PCV.uxlib.setMsg(ps, null, null, '❌ ' + e.message); toast(e.message, false);
  }
}
async function ctrSnapDel(n, s) { if (!await customConfirm(t('btn.delete'), s + '?')) return;
  var el = PCV.uxlib.el;
  showModal([
    el('h2', null, '🗑 ' + t('snap.deleted')),
    el('p', null, el('b', { class: 'color-accent' }, n + '@' + s)),
    el('div', { class: 'prog-bar' }, el('div', { class: 'prog-fill', id: 'csd-p' })),
    el('div', { class: 'prog-status', id: 'csd-s' }, el('span', { class: 'spinner' }), ' Deleting...')
  ]);
  var pf = document.getElementById('csd-p'), ps = document.getElementById('csd-s');
  try {
    if (pf) pf.style.width = '60%';
    await fetchDelete(EP.CTR_SNAP_DELETE(n, s));
    if (pf) pf.style.width = '100%'; if (ps) PCV.uxlib.setMsg(ps, null, null, '✅ ' + t('snap.deleted'));
    toast(t('snap.deleted')); addEvt('LXC Snapshot deleted — ' + n + '@' + s);
    setTimeout(function() { closeModal(); ctrTab = 'snapshots'; renderContainers(document.getElementById('cb')); }, 1500);
  } catch (e) {
    if (pf) { pf.style.width = '100%'; pf.style.background = 'var(--red)'; }
    if (ps) PCV.uxlib.setMsg(ps, null, null, '❌ ' + e.message); toast(e.message, false);
  }
}

async function ctrExec(n) {
  try { const c = await fetchGet(EP.CTR_LIST()); const l = unwrapList(c); const ct = l.find(x => x.name === n);
    if (!ct || ct.state !== 'RUNNING') { toast(n + ' is not running', false); return; } } catch (e) { if(_DEBUG) console.warn('c:', e.message); }
  selCtr = n; ctrTab = 'console'; renderContainers(document.getElementById('cb'));
}

function ctrDel(n) {
  var el = PCV.uxlib.el;
  showModal([
    el('h2', { class: 'color-red' }, '⚠ ' + t('ctr.destroying')),
    el('p', { class: 'mb-12' }, t('ctr.delete.confirm') + ' ', el('b', { class: 'color-accent' }, n)),
    el('p', { class: 'mb-12' }, t('ctr.delete.type_name')),
    el('div', { class: 'fr' },
      el('label', { for: 'del-ctr-confirm' }, 'Name'),
      el('input', { id: 'del-ctr-confirm', placeholder: n })),
    el('div', { class: 'text-right mt-14' },
      el('button', { class: 'btn btn-r', onclick: "doCtrDel('" + n + "')" }, t('btn.delete')),
      ' ',
      el('button', { class: 'btn', onclick: 'closeModal()' }, t('btn.cancel')))
  ]);
}

async function doCtrDel(n) {
  const c = document.getElementById('del-ctr-confirm')?.value; if (c !== n) { toast(t('vm.name_mismatch'), false); return; }
  /* prog-fill div 는 원본이 class 속성 중복(class="prog-fill" .. class="w-pct-10") — HTML
   * 파서상 첫 class="prog-fill" 만 적용되고 w-pct-10 은 무시됨 (렌더 동등 보존). */
  const el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  const mc = document.getElementById('mc');
  clearEl(mc);
  mc.appendChild(frag(
    el('h2', { class: 'color-red' }, '⚠ ' + t('ctr.destroying')),
    el('p', null, el('b', { class: 'color-accent' }, n)),
    el('div', { class: 'prog-bar' }, el('div', { class: 'prog-fill', id: 'dc-p' })),
    el('div', { class: 'prog-status', id: 'dc-s' }, el('span', { class: 'spinner' }), 'Stopping...')
  ));
  const pf = document.getElementById('dc-p'), ps = document.getElementById('dc-s');
  try { pf.style.width = '30%'; PCV.uxlib.setMsg(ps, 'loading', null, 'Destroying...');
    const d = await fetchDelete(EP.CTR_DETAIL(n)).catch(() => ({}));
    pf.style.width = '80%';
    if (d.error) { pf.style.background = 'var(--red)'; pf.style.width = '100%'; PCV.uxlib.setMsg(ps, null, null, '❌ ' + d.error.message); toast(t('btn.delete') + ' failed', false); return; }
    pf.style.width = '100%'; PCV.uxlib.setMsg(ps, null, null, '✅ ' + t('ctr.destroyed')); toast(t('ctr.destroyed')); addEvt('LXC Destroyed — ' + n); selCtr = null; setTimeout(() => { closeModal(); renderContainers(document.getElementById('cb')); }, 1500);
  } catch (e) { pf.style.width = '100%'; PCV.uxlib.setMsg(ps, null, null, '❌ ' + e.message); toast(e.message, false); }
}

/* ═══ CONTAINER CREATE WIZARD ═══ */
function showCtrCreate() {
  if (typeof markFormDirty === 'function') markFormDirty('ctr-create');
  var el = PCV.uxlib.el;
  var selStyle = 'flex:1;padding:6px;background:var(--bg);border:1px solid var(--border);color:var(--fg);border-radius:4px';
  showModal([
    el('h2', null, t('ctr.new')),
    /* 단일 컬럼 레이아웃 — 모달 잘림 방지 */
    el('div', { style: 'display:grid;grid-template-columns:1fr 1fr;gap:16px' },
      /* 왼쪽: 기본 설정 */
      el('div', null,
        el('h4', { class: 'mb-8' }, '☷ Basic'),
        el('div', { class: 'fr' },
          el('label', { for: 'cc-name', class: 'min-w-80' }, 'Name'),
          el('input', { id: 'cc-name', placeholder: 'my-container', class: 'flex-1' })),
        el('div', { class: 'fr' },
          el('label', { for: 'cc-dist', class: 'min-w-80' }, 'Distribution'),
          el('select', { id: 'cc-dist', onchange: 'ctrDistChanged()', style: selStyle },
            el('option', { value: 'ubuntu' }, 'Ubuntu'),
            el('option', { value: 'debian' }, 'Debian'),
            el('option', { value: 'alpine' }, 'Alpine'),
            el('option', { value: 'centos' }, 'CentOS'),
            el('option', { value: 'fedora' }, 'Fedora'),
            el('option', { value: 'archlinux' }, 'Arch Linux'))),
        el('div', { class: 'fr' },
          el('label', { for: 'cc-rel', class: 'min-w-80' }, 'Release'),
          el('input', { id: 'cc-rel', value: 'jammy', placeholder: 'jammy / bookworm / 3.19', class: 'flex-1' }))),
      /* 오른쪽: 네트워크 + 리소스 */
      el('div', null,
        el('h4', { class: 'mb-8' }, '🌐 Network'),
        el('div', { class: 'fr' },
          el('label', { for: 'cc-br', class: 'min-w-70' }, 'Bridge'),
          el('div', { class: 'flex gap-6 flex-1' },
            el('select', { id: 'cc-br', style: selStyle },
              el('option', { value: 'pcvbr0' }, 'pcvbr0 (default)')),
            el('button', { class: 'btn text-xs', onclick: 'ctrLoadBridges()' }, '🔄'))),
        el('div', { class: 'fr' },
          el('label', { for: 'cc-ipmode', class: 'min-w-70' }, 'IP Mode'),
          el('select', { id: 'cc-ipmode', onchange: 'ctrIpModeChanged()', style: selStyle },
            el('option', { value: 'dhcp' }, 'DHCP (auto)'),
            el('option', { value: 'static' }, 'Static IP'))),
        el('div', { class: 'fr hidden', id: 'cc-static-row' },
          el('label', { for: 'cc-ip', class: 'min-w-70' }, 'Static IP'),
          el('input', { id: 'cc-ip', placeholder: '10.0.3.100/24', class: 'flex-1' })),
        el('div', { class: 'fr hidden', id: 'cc-gw-row' },
          el('label', { for: 'cc-gw', class: 'min-w-70' }, 'Gateway'),
          el('input', { id: 'cc-gw', placeholder: '10.0.3.1', class: 'flex-1' })),
        el('div', { class: 'fr' },
          el('label', { for: 'cc-dns', class: 'min-w-70' }, 'DNS'),
          el('input', { id: 'cc-dns', placeholder: '8.8.8.8 (optional)', class: 'flex-1' })),
        el('h4', { style: 'margin:12px 0 8px' }, '⚙ Resources'),
        el('div', { class: 'fr' },
          el('label', { for: 'cc-vcpu', class: 'min-w-70' }, 'vCPU'),
          el('input', { id: 'cc-vcpu', type: 'number', value: '1', min: '1', max: '64', class: 'flex-1' })),
        el('div', { class: 'fr' },
          el('label', { for: 'cc-mem', class: 'min-w-70' }, 'Memory (MB)'),
          el('input', { id: 'cc-mem', type: 'number', value: '512', min: '64', class: 'flex-1' })))),
    el('div', { class: 'text-right mt-14' },
      el('button', { class: 'btn btn-g', onclick: 'doCtrCreate()' }, t('btn.create')),
      ' ',
      el('button', { class: 'btn', onclick: 'closeModal()' }, t('btn.cancel')))
  ]);
  /* 넓은 모달 클래스 적용 */
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
  const el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  const sel = document.getElementById('cc-br'); if (!sel) return;
  try {
    const r = await fetchGet(EP.NET_LIST());
    const nl = unwrapList(r);
    const opts = [];
    nl.forEach(n => { const name = n.name || ''; if (!name) return; const mode = n.mode || ''; const ip = n.ip_cidr || '';
      opts.push(el('option', { value: name, selected: name === 'pcvbr0' ? '' : null }, name + (mode ? ' (' + mode + ')' : '') + (ip ? ' ' + ip : ''))); });
    clearEl(sel);
    if (opts.length === 0) sel.appendChild(el('option', { value: 'pcvbr0' }, 'pcvbr0'));
    else sel.appendChild(frag(opts));
  } catch (e) { clearEl(sel); sel.appendChild(el('option', { value: 'pcvbr0' }, 'pcvbr0')); }
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

  /* 모달 즉시 닫고 백그라운드 처리 시작 */
  if (typeof clearFormDirty === 'function') clearFormDirty('ctr-create');
  closeModal();
  toast('&#9783; ' + escapeHtml(n) + ' ' + _L('생성 시작...', 'Creating...'), 's');

  const body = { name: n, image: dist + ':' + rel, bridge: br || 'pcvbr0', vcpu_count: vcpu, memory_mb: mem };
  if (ipmode === 'static' && ip) { body.static_ip = ip; if (gw) body.gateway = gw; }
  if (dns) body.dns = dns;

  try {
    const r = await fetchPost(EP.CTR_LIST(), body);

    /* 에러 응답 처리 */
    if (r && r.error) {
      var errMsg = r.error.message || r.error.data || JSON.stringify(r.error);
      toast('&#10060; ' + _L('컨테이너 생성 실패', 'Container creation failed') + ': ' + errMsg, false);
      return;
    }

    /* 백그라운드 폴링 — 컨테이너 목록에 나타날 때까지 (최대 90초) */
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

/* ═══ CONTAINER SET LIMITS ═══ */
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

/* ═══ LXC NIC MANAGEMENT ═══ */
function ctrNicAdd(name) {
  var el = PCV.uxlib.el;
  showModal([
    el('h2', null, 'Add NIC to ' + name),
    el('div', { class: 'fr' },
      el('label', { for: 'ctr-nic-br' }, 'Bridge'),
      el('input', { id: 'ctr-nic-br', value: 'pcvbr0', placeholder: 'pcvbr0' })),
    el('div', { class: 'fr' },
      el('label', { for: 'ctr-nic-mac' }, 'MAC (optional)'),
      el('input', { id: 'ctr-nic-mac', placeholder: 'auto' })),
    el('div', { class: 'text-right mt-12' },
      el('button', { class: 'btn btn-g', onclick: "doCtrNicAdd('" + escapeHtml(name) + "')" }, 'Add NIC'),
      ' ',
      el('button', { class: 'btn btn-r', onclick: 'closeModal()' }, t('btn.cancel')))
  ]);
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

/* ═══ REGISTER ALL ON WINDOW ═══ */
/* State variables (selCtr, ctrTab, ctrHist, ctrSortKey, ctrSortDir)
   are already initialized on window.* at the top of this file. */
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

/* ═══ CONTAINER CLONE (백엔드 4차) ═══ */
async function showCtrClone(name) {
  var el = PCV.uxlib.el;
  showModal([
    el('h2', null, _L('컨테이너 클론', 'Clone Container')),
    el('div', { class: 'fr' },
      el('label', { for: 'ctr-source' }, _L('원본', 'Source')),
      el('input', { id: 'ctr-source', value: name, disabled: '' })),
    el('div', { class: 'fr' },
      el('label', { for: 'ctr-clone-name' }, _L('클론 이름', 'Clone Name')),
      el('input', { id: 'ctr-clone-name', placeholder: name + '-clone' })),
    el('div', { class: 'text-right mt-14' },
      el('button', { class: 'btn btn-g', id: 'ctr-clone-ok' }, _L('클론', 'Clone')),
      ' ',
      el('button', { class: 'btn btn-r', id: 'ctr-clone-cancel' }, t('btn.cancel')))
  ]);
  setTimeout(function() {
    var okBtn = document.getElementById('ctr-clone-ok');
    var cancelBtn = document.getElementById('ctr-clone-cancel');
    var inp = document.getElementById('ctr-clone-name');
    if (inp) inp.focus();
    if (cancelBtn) cancelBtn.addEventListener('click', function() { closeModal(); });
    if (okBtn) okBtn.addEventListener('click', async function() {
      var dst = document.getElementById('ctr-clone-name').value.trim();
      if (!dst) { toast(_L('이름 필수', 'Name required'), false); return; }
      try {
        await fetchPost(EP.CTR_CLONE(name), { source: name, dest: dst });
        toast(_L('클론 요청 완료', 'Clone requested'));
        addEvt('LXC Clone requested — ' + name + ' → ' + dst);
        closeModal();
      } catch (e) { toast(_L('실패', 'Failed'), false); }
    });
    if (inp) inp.addEventListener('keydown', function(e) {
      if (e.key === 'Enter') { e.preventDefault(); if (okBtn) okBtn.click(); }
    });
  }, 50);
}

/* ═══ CONTAINER MEMORY STATS (cgroup v2) ═══ */
async function showCtrMemoryStats(name) {
  try {
    var r = await fetchGet(EP.CTR_MEMORY_STATS(name));
    var d = unwrapData(r) || {};
    var el = PCV.uxlib.el;
    var fields = ['anon', 'file', 'slab', 'sock', 'shmem', 'pgfault', 'pgmajfault'];
    var rows = [];
    fields.forEach(function(f) {
      if (d[f] !== undefined) {
        var val = d[f] > 1048576 ? (d[f] / 1048576).toFixed(1) + ' MB' : d[f];
        rows.push(el('tr', null, el('td', null, el('b', null, f)), el('td', null, String(val))));
      }
    });
    Object.keys(d).forEach(function(k) {
      if (fields.indexOf(k) === -1 && k !== 'container') {
        rows.push(el('tr', null, el('td', { class: 'color-muted' }, k), el('td', null, String(d[k]))));
      }
    });
    var body = rows.length
      ? el('table', { class: 'data-table text-11' },
          el('thead', null, el('tr', null,
            el('th', null, _L('항목', 'Field')),
            el('th', null, _L('값', 'Value')))),
          el('tbody', null, rows))
      : PCV.uxlib.msg('muted', { tag: 'p' }, _L('메모리 데이터 없음', 'No memory data'));
    showModal([
      el('h2', null, name + ' — ' + _L('메모리 상세', 'Memory Details')),
      body,
      el('div', { class: 'text-right mt-12' },
        el('button', { class: 'btn', onclick: 'closeModal()' }, _L('닫기', 'Close')))
    ]);
  } catch (e) { toast(_L('로드 실패', 'Failed'), false); }
}

/* ═══ CONTAINER HEALTH CHECK ═══ */
async function checkCtrHealth(name) {
  try {
    var r = await fetchGet(EP.CTR_HEALTH(name));
    var d = unwrapData(r);
    var icon = d.running ? '🟢' : '🔴';
    toast(icon + ' ' + esc(name) + ': ' + (d.state || 'unknown'), d.running ? 's' : 'w');
  } catch(e) { toast(_L('헬스 체크 실패', 'Health check failed'), 'e'); }
}

/* ═══ BACKWARD COMPAT SHIMS ═══ */
window.showCtrClone = showCtrClone;
window.showCtrMemoryStats = showCtrMemoryStats;
window.checkCtrHealth = checkCtrHealth;

/* ═══ PCV.container namespace export ═══ */
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
