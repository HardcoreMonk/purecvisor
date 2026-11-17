                                                                                                                      
                                                                                                            
                                                                                                              
                                                                                                
                                                                                                 
                                                                                         
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const MODS = [
  'ui/modules/ui.js',
  'ui/modules/uxlib.js',
  'ui/modules/filter-state.js',
  'ui/modules/endpoints.js',
  'ui/modules/monitor.js',
  'ui/modules/monitor-vmdiag.js'
];
const MODS_POLL = [
  'ui/modules/endpoints.js',
  'ui/modules/api.js',
  'ui/modules/ui.js',
  'ui/modules/filter-state.js',
  'ui/modules/uxlib.js',
  'ui/modules/monitor.js',
  'ui/modules/monitor-vmdiag.js'
];

const METRICS = [
  '# TYPE purecvisor_healing_actions_total counter',
  'purecvisor_healing_actions_total{policy="cpu-overload",action="restart"} 3',
  'purecvisor_healing_actions_total{policy="mem-pressure",action="balloon"} 4',
  'purecvisor_host_cpu_percent 12.5'
].join('\n') + '\n';

const OBS = 1794757394000;
const NULL_DISK = { rd_bps: null, wr_bps: null, rd_iops: null, wr_iops: null, rd_lat_ms: null, wr_lat_ms: null, fl_lat_ms: null };
const NULL_NET = { rx_bps: null, tx_bps: null, rx_pps: null, tx_pps: null, drop_ps: null, err_ps: null };
function vm(name, over) {
  return Object.assign({
    name, uuid: 'u-' + name, state: 'running', quality: 'ok', reason: null, missing: [], sample_age_ms: 3000,
    interval_ms: 5000, last_ok_at_ms: OBS, vcpus: 4, mem_max_mb: 8192, mem_alloc_mb: 8192, mem_guest_used_mb: 4096,
    mem_guest_available_mb: 4096, mem_rss_mb: 7000, guest_stats_age_s: 2, cpu_pct: 40, cpu_host_pct: 2.2,
    disk: { rd_bps: 1e6, wr_bps: 5e5, rd_iops: 10, wr_iops: 20, rd_lat_ms: 1.2, wr_lat_ms: 2.5, fl_lat_ms: 0 },
    net: { rx_bps: 2e6, tx_bps: 1e6, rx_pps: 100, tx_pps: 50, drop_ps: 0, err_ps: 0 },
    contention: { vcpu_delay_pct: 1, vcpu_delay_max_pct: 2, throttled_pct: 0, throttled_max_pct: 0, throttled_periods_ratio: 0,
                  psi: null, swap_in_kbps: 0, swap_out_kbps: 0, major_faults_ps: 0 },
    hints: []
  }, over);
}
const STOPPED = { quality: 'stopped', sample_age_ms: null, interval_ms: null, last_ok_at_ms: null, mem_alloc_mb: null,
  mem_guest_used_mb: null, mem_guest_available_mb: null, mem_rss_mb: null, guest_stats_age_s: null, cpu_pct: null,
  cpu_host_pct: null, disk: NULL_DISK, net: NULL_NET, contention: null, hints: [] };
const WARM = { quality: 'warming_up', cpu_pct: null, cpu_host_pct: null, disk: NULL_DISK, net: NULL_NET, mem_guest_used_mb: null,
  mem_rss_mb: null, hints: [] };
const DIAG = {
  schema: 1, scope: 'all',
  sampler: { state: 'ok', observed_at_ms: OBS, age_ms: 3000, interval_ms: 5000, tracked: 8, truncated: 3, error: null },
  host: {
    logical_cpus: 72, physical_cores: 36, mem_total_mb: 124416,
    allocation: { vms_running: 6, vms_defined: 8, vcpu_running: 36, vcpu_defined: 80, vcpu_running_per_logical: 0.5,
      vcpu_defined_per_logical: 1.111, mem_running_max_mb: 61440, mem_defined_max_mb: 131072, mem_running_alloc_mb: 61440,
      mem_running_max_ratio: 0.494, mem_defined_max_ratio: 1.054 },
    psi: { cpu: { some_avg10: 1, full_avg10: 0 }, memory: { some_avg10: 0, full_avg10: 0 }, io: { some_avg10: 2, full_avg10: 1 } }
  },
  vms: [
    vm('a-ok'),
    vm('b-busy', { quality: 'busy', missing: ['disk'], last_ok_at_ms: OBS + 3000 - 12000, disk: NULL_DISK, cpu_host_pct: 1.0 }),
    vm('c-warm', Object.assign({ reason: 'first_sample' }, WARM)),
    vm('d-reset', Object.assign({ reason: 'reset' }, WARM)),
    vm('e-partial', { quality: 'partial', missing: ['guest_memory'], vcpus: 8, cpu_pct: 60, cpu_host_pct: 10.5, mem_guest_used_mb: null,
      mem_guest_available_mb: null, guest_stats_age_s: null, mem_rss_mb: 9000,
      disk: { rd_bps: 5e6, wr_bps: 5e6, rd_iops: 0, wr_iops: 0, rd_lat_ms: null, wr_lat_ms: null, fl_lat_ms: null },
      net: { rx_bps: 0, tx_bps: 0, rx_pps: 0, tx_pps: 0, drop_ps: 0, err_ps: 0 },
      hints: [{ code: 'cpu_wait', level: 'warn', value: 12.34, threshold: 10 },
              { code: 'io_pressure', level: 'notice', value: 22, threshold: 20 },
              { code: 'disk_latency', level: 'notice', value: 31, threshold: 20 }] }),
    vm('f-net', { cpu_host_pct: 5, vcpus: 16, net: { rx_bps: 1e7, tx_bps: 2.5e6, rx_pps: 1000, tx_pps: 500, drop_ps: 0, err_ps: 0 },
      hints: [{ code: 'mem_pressure', level: 'notice', value: 15, threshold: 10 }] }),
    vm('g-off', Object.assign({ state: 'shutoff', vcpus: 2, mem_max_mb: 2048 }, STOPPED)),
    vm('h-paused', Object.assign({ state: 'paused', vcpus: 2, mem_max_mb: 4096 }, STOPPED))
  ]
};
const NODE = {
  node: 'edge-a', ip: '10.168.3.11', cpu: 30, mem: 40, disk: 50, temp: 40, load1: 1, load5: 1, load15: 1, ram_total: 130459631616,
  memInfo: { MemTotal: 1, MemAvailable: 1, SwapTotal: 0, SwapFree: 0 }, sockstat: {}, conntrack: 0, cores: {}, netdevs: {}, disks: {},
  filesystems: [], hwmon: [], vmstat: {}, pressure: {}, anomaly_active: 1, healing_pending: 0, error: false, vms: []
};
const FORBIDDEN = ['원인', '때문', '장애', '문제입니다', 'CPU Ready', 'Ready', 'Co-Stop', '%RDY'];

async function setup(page, opts = {}) {
  await page.setViewport({ width: opts.width || 1440, height: 900 });
  await page.evaluate(() => {
    document.getElementById('cb').classList.add('cb');                                                           
    window._DEBUG = false;
    window.authToken = 'test-token';
    window._L = text => text;
    window.t = key => key;
    window.__nav = [];
    window.navigateTo = name => { window.__nav.push(name); };
    window.fetchGet = url => fetch(url).then(r => r.json().catch(() => ({ error: { code: r.status } }))
      .then(body => (r.ok ? body : (body && body.error ? body : { error: { code: r.status } }))));
  });
}
                                                                                   
async function render(page, tab, diag = DIAG) {
  await page.evaluate(({ tab, diag, node }) => {
    const d = diag.fail ? { ok: false, data: null, err: diag.fail } : { ok: true, data: diag, err: null };
    const legacy = PCV.vmdiag.legacyVms(d);
    const running = legacy.filter(v => v.running === 1).length;
    const cb = document.getElementById('cb');
    window.currentTab = 'mon-' + tab;
    if (tab === 'overview') window.renderMonOverview(cb, [node], legacy, running, 30, 40, 50, node.ram_total, d);
    else if (tab === 'vms') window.renderMonVms(cb, legacy, running, d);
    else window.renderMonStorage(cb, [node], legacy, node.ram_total, d);
  }, { tab, diag, node: NODE });
}

test('healing total sums labelled series', async () => {
  await withPage(MODS, async page => {
    const total = await page.evaluate(async metricText => {
      const nativeFetch = window.fetch.bind(window);
      window.fetch = (url, options) => String(url).endsWith('/api/v1/metrics')
        ? Promise.resolve(new Response(metricText, { status: 200, headers: { 'content-type': 'text/plain' } }))
        : nativeFetch(url, options);
      window.authToken = 'test-token';
      window._L = text => text;
      const nodes = await window.fetchAllMetrics();
      return nodes[0].healing_total;
    }, METRICS);
    assert.equal(total, 7);                              
  });
});

test('metrics parser unescapes label values and keeps braces inside quotes', async () => {
  await withPage(MODS, async page => {
    const devs = await page.evaluate(async () => {
      const text = 'node_network_receive_bytes_total{device="e\\"n\\\\o}1,x=y\\nz"} 5\n' +
                   'node_network_receive_bytes_total{device="plain"} 7\n';
      window.fetch = () => Promise.resolve(new Response(text, { status: 200 }));
      window.authToken = 'test-token';
      const nodes = await window.fetchAllMetrics();
      return Object.keys(nodes[0].netdevs).sort();
    });
    assert.deepEqual(devs, ['e"n\\o}1,x=y\nz', 'plain']);
  });
});

test('top5 disk and network rank by bps', async () => {
  await withPage(MODS, async page => {
    await setup(page);
    await render(page, 'overview');
    const r = await page.evaluate(() => {
      const card = t => [...document.querySelectorAll('#cb .hc')].find(c => c.querySelector('h4')?.textContent === t);
      const first = t => card(t)?.querySelector('tbody tr td:nth-child(2)')?.textContent;
      return { disk: first('Top 5 디스크 I/O'), net: first('Top 5 네트워크'),
               netVal: card('Top 5 네트워크')?.querySelector('tbody tr td:nth-child(3)')?.textContent };
    });
    assert.equal(r.disk, 'e-partial');                        
    assert.equal(r.net, 'f-net');                           
    assert.match(r.netVal, /^12\.5 MB\/s/);
                                                  
    const vms = await page.evaluate(async () => {
      window.fetch = () => Promise.resolve(new Response(
        'purecvisor_vm_vcpu{vm="ghost"} 4\npurecvisor_vm_running{vm="ghost"} 1\n', { status: 200 }));
      return (await window.fetchAllMetrics())[0].vms;
    });
    assert.deepEqual(vms, []);
  });
});

test('top5 cpu by host pct with vcpu pct secondary and vcpu allocation title', async () => {
  await withPage(MODS, async page => {
    await setup(page);
    await render(page, 'overview');
    const r = await page.evaluate(() => {
      const titles = [...document.querySelectorAll('#cb .vmdiag-overview .hc h4')].map(h => h.textContent);
      const cpu = [...document.querySelectorAll('#cb .hc')].find(c => c.querySelector('h4')?.textContent === 'Top 5 CPU');
      return { titles, first: cpu.querySelector('tbody tr td:nth-child(2)').textContent,
               cell: cpu.querySelector('tbody tr td:nth-child(3)').textContent };
    });
    assert.ok(r.titles.includes('Top 5 CPU'));
    assert.ok(r.titles.includes('Top 5 vCPU 할당'));
    assert.ok(!r.titles.includes('Top 5 vCPU'));
    assert.equal(r.first, 'e-partial');                                                   
    assert.match(r.cell, /^10\.5%vCPU 8 · 60\.0%\(vCPU 대비\)$/);
  });
});

test('null cells are em dash with reason and no -1 anywhere', async () => {
  await withPage(MODS, async page => {
    await setup(page);
    await render(page, 'vms');
    const r = await page.evaluate(() => {
      const row = n => [...document.querySelectorAll('#cb table.vmdiag-table tbody tr')].find(tr => tr.querySelector('td b')?.textContent === n);
      const cells = n => [...row(n).querySelectorAll('td')];
      const warm = cells('c-warm'), part = cells('e-partial');
      return {
        warmCpuVisible: warm[3].firstChild.firstChild.textContent,
        warmCpuTitle: warm[3].querySelector('span[title]').getAttribute('title'),
        warmCpuSr: warm[3].querySelector('.sr-only').textContent.trim(),
        warmData: warm[2].textContent,
        partNet: part[9].textContent, partGuestTitle: part[5].querySelector('span[title]').getAttribute('title'),
        partLat: part[8].querySelector('span[title]').getAttribute('title'),
        body: document.getElementById('cb').textContent
      };
    });
    assert.equal(r.warmCpuVisible, '—');
    assert.match(r.warmCpuTitle, /^첫 표본/);
    assert.match(r.warmCpuSr, /^첫 표본/);
    assert.match(r.warmData, /준비 중.*첫 표본/);
    assert.equal(r.partNet, '0 B/s0 B/s');                         
    assert.match(r.partGuestTitle, /게스트 메모리 통계 없음/);
    assert.equal(r.partLat, '이 간격에 요청 없음');
    assert.ok(!/(^|[^0-9])-1([^0-9]|$)/.test(r.body), '-1 이 숫자로 보이면 안 된다');
    await render(page, 'overview');
    assert.ok(!/(^|[^0-9])-1([^0-9]|$)/.test(await page.evaluate(() => document.getElementById('cb').textContent)));
  });
});

test('quality badges text', async () => {
  await withPage(MODS, async page => {
    await setup(page);
    await render(page, 'vms');
    const r = await page.evaluate(() => Object.fromEntries([...document.querySelectorAll('#cb table.vmdiag-table tbody tr')]
      .map(tr => { const td = tr.querySelectorAll('td'); return [td[0].textContent, { state: td[1].textContent, data: td[2].textContent,
        pill: td[2].querySelector('.pill')?.className || null }]; })));
    assert.deepEqual(r['a-ok'], { state: '실행', data: '정상', pill: null });
    assert.match(r['c-warm'].data, /^준비 중/); assert.equal(r['c-warm'].pill, 'pill pill-idle vmdiag-pill');
    assert.match(r['d-reset'].data, /^준비 중 재시작·장치 변경/);
    assert.match(r['e-partial'].data, /^일부 게스트 메모리 없음/); assert.equal(r['e-partial'].pill, 'pill pill-idle vmdiag-pill');
    assert.equal(r['b-busy'].data, '읽기 보류 마지막 정상 12초 전'); assert.equal(r['b-busy'].pill, 'pill pill-warn vmdiag-pill');
    assert.deepEqual(r['g-off'], { state: '정지', data: '—', pill: null });
    assert.deepEqual(r['h-paused'], { state: '일시정지', data: '—', pill: null });
  });
});

test('sampler stale error truncated lines', async () => {
  await withPage(MODS, async page => {
    await setup(page);
    await render(page, 'overview');
    let line = await page.evaluate(() => document.querySelector('#cb .vmdiag-overview').textContent);
    assert.match(line, /VM 3대는 상한\(256대\) 밖이라 진단하지 않았습니다/);
    assert.match(line, /VM 표본 3초 전 · 5초 간격 · 8대/);
    await render(page, 'overview', Object.assign({}, DIAG, { sampler: Object.assign({}, DIAG.sampler, { state: 'stale', age_ms: 45000 }) }));
    line = await page.evaluate(() => { const s = document.querySelector('#cb .vmdiag-status'); return { t: s.textContent, p: s.querySelector('.pill').className }; });
    assert.match(line.t, /^표본 오래됨 VM 진단 표본이 45초 동안 갱신되지 않았습니다\. 값은 마지막 표본 기준입니다\./);
    assert.equal(line.p, 'pill pill-warn vmdiag-pill');
    await render(page, 'overview', Object.assign({}, DIAG, { sampler: Object.assign({}, DIAG.sampler, { state: 'error', error: 'connect_failed' }) }));
    line = await page.evaluate(() => { const s = document.querySelector('#cb .vmdiag-status'); return { t: s.textContent, p: s.querySelector('.pill').className }; });
    assert.match(line.t, /^표본 실패 libvirt 연결에 실패했습니다/);
    assert.equal(line.p, 'pill pill-crit vmdiag-pill');
                                          
    const writes = await page.evaluate(async diag => {
      const seq = ['ok', 'ok', 'stale', 'stale', 'stale', 'ok'];
      let i = 0;
      window.fetchGet = () => Promise.resolve(Object.assign({}, diag, { sampler: Object.assign({}, diag.sampler, { state: seq[i++] }) }));
      let n = 0;
      await PCV.vmdiag.refresh('overview');
      const live = () => document.getElementById('vmdiag-live');
      for (let k = 1; k < seq.length; k++) {
        const before = live() ? live().textContent : null;
        await PCV.vmdiag.refresh('overview');
        if (live() && live().textContent !== before) n++;
      }
      return { n, role: live().getAttribute('role'), text: live().textContent };
    }, DIAG);
    assert.equal(writes.n, 2);                                                    
    assert.equal(writes.role, 'status');
  });
});

test('hint badges levels and forbidden words', async () => {
  await withPage(MODS, async page => {
    await setup(page);
    await render(page, 'vms');
    const r = await page.evaluate(() => {
      const tr = [...document.querySelectorAll('#cb table.vmdiag-table tbody tr')].find(t => t.querySelector('td b').textContent === 'e-partial');
      const pills = [...tr.querySelectorAll('td:last-child .pill')];
      const card = document.querySelector('#cb .vmdiag-tablecard');
      const clone = card.cloneNode(true);
      clone.querySelectorAll('.vmdiag-details').forEach(n => n.remove());
      const foot = [...clone.querySelectorAll('.stat-label')].find(n => n.textContent.startsWith('신호는 잠정 기준'));
      if (foot) foot.remove();
      const titles = [...card.querySelectorAll('[title]')].map(n => n.getAttribute('title')).join('\n');
      return { pills: pills.map(p => ({ cls: p.className, text: p.firstChild.textContent })), text: clone.textContent + '\n' + titles };
    });
    assert.deepEqual(r.pills, [
      { cls: 'pill pill-warn vmdiag-pill', text: 'CPU 대기 · 주의' },
      { cls: 'pill pill-idle vmdiag-pill', text: 'I/O 대기 · 참고' },
      { cls: 'pill pill-idle vmdiag-pill', text: '+1' }
    ]);
    for (const w of FORBIDDEN) assert.ok(!r.text.includes(w), '금지어: ' + w);
    assert.match(r.text, /기다린 시간이 12\.3%입니다\(잠정 기준 10% 이상\)/);
  });
});

test('host net rate uses elapsed time', async () => {
  await withPage(MODS, async page => {
    await setup(page);
    const rates = await page.evaluate(async node => {
      let now = 1_000_000;
      Date.now = () => now;
      const vals = [0, 125e6, 50e6];                                              
      let k = 0;
      window.fetch = () => Promise.resolve(new Response(
        'node_network_receive_bytes_total{device="eno1"} ' + vals[k] + '\nnode_network_transmit_bytes_total{device="eno1"} ' + (vals[k++] * 1.2) + '\n', { status: 200 }));
      const out = [];
      for (let i = 0; i < 3; i++) {
        const nodes = await window.fetchAllMetrics();
        const n = Object.assign({}, node, nodes[0], { node: 'edge-a' });
        window.renderMonOverview(document.getElementById('cb'), [n], [], 0, 0, 0, 0, 0, { ok: false, data: null, err: 'http' });
        out.push([...document.querySelectorAll('#cb .hr')].find(r => r.querySelector('.k').textContent === 'Net I/O').querySelector('.v').textContent);
        now += 10_000;
      }
      return out;
    }, NODE);
    assert.equal(rates[0], '▲ — ▼ —');
    assert.match(rates[1], /^▲ 12\.5 MB\/s ▼ 15\.0 MB\/s$/);
    assert.equal(rates[2], '▲ — ▼ —');
  });
});

test('polling scope and counts', async () => {
  await withPage(MODS_POLL, async (page, { requests }) => {
    await setup(page);
    await page.evaluate(() => {
      const nativeFetch = window.fetch.bind(window);
      window.__ticks = [];
      window.setInterval = (fn) => { window.__ticks.push(fn); return window.__ticks.length; };
      window.clearInterval = () => {};
      window.fetch = (url, o) => String(url).endsWith('/api/v1/metrics')
        ? Promise.resolve(new Response('purecvisor_host_cpu_percent 1\n', { status: 200 })) : nativeFetch(url, o);
    });
    const diagCount = () => requests.filter(r => r.path === '/api/v1/monitor/vm-diag').length;
    await page.evaluate(async () => { window.currentTab = 'mon-overview'; await window.renderMonitoring(document.getElementById('cb'), 'overview'); });
    await new Promise(r => setTimeout(r, 300));
    assert.equal(diagCount(), 1, '진입 직후 1초 안 진단 요청은 1건');
    for (let i = 0; i < 6; i++) {
      await page.evaluate(() => window.__ticks[window.__ticks.length - 1]());
      await new Promise(r => setTimeout(r, 150));
    }
    assert.equal(diagCount(), 7, '개요 60초 = 진입 1 + tick 6');
                   
    await page.evaluate(() => Object.defineProperty(document, 'hidden', { configurable: true, get: () => true }));
    await page.evaluate(() => window.__ticks[window.__ticks.length - 1]());
    await new Promise(r => setTimeout(r, 150));
    assert.equal(diagCount(), 7);
    await page.evaluate(() => Object.defineProperty(document, 'hidden', { configurable: true, get: () => false }));
                                       
    await page.evaluate(async () => { window.currentTab = 'mon-hosts'; await window.renderMonitoring(document.getElementById('cb'), 'hosts'); });
    await page.evaluate(() => window.__ticks[window.__ticks.length - 1]());
    await new Promise(r => setTimeout(r, 150));
    assert.equal(diagCount(), 7);
                                         
    await page.evaluate(() => { window.currentTab = 'dashboard'; window.__ticks[window.__ticks.length - 1](); });
    await new Promise(r => setTimeout(r, 150));
    assert.equal(diagCount(), 7);
  }, { routes: { '/api/v1/monitor/vm-diag': { status: 200, body: { data: DIAG } } } });                            
});

test('allocation card values denominators overcommit', async () => {
  await withPage(MODS, async page => {
    await setup(page);
    await render(page, 'overview');
    const read = () => page.evaluate(() => {
      const card = [...document.querySelectorAll('#cb .hc')].find(c => c.querySelector('h4')?.textContent === '할당률(물리 용량 대비 설정 합)');
      const cells = [...card.querySelectorAll('tbody td')].map(td => ({
        ratio: td.querySelector('.stat-lg').firstChild.textContent, over: !!td.querySelector('.pill-warn'),
        frac: td.querySelectorAll('.stat-label')[0].textContent, sub: td.querySelectorAll('.stat-label')[1]?.textContent || null }));
      return { cells, note: card.querySelector('.vmdiag-alloc-note').textContent };
    });
    let r = await read();
    assert.deepEqual(r.cells.map(c => [c.ratio, c.over, c.frac]), [
      ['0.50', false, '36 / 72'], ['0.49', false, '60.0 / 121.5 GiB'],
      ['1.11', true, '80 / 72'], ['1.05', true, '128.0 / 121.5 GiB']]);
    assert.equal(r.cells[0].sub, '물리 코어 36개 기준 1.00');
    assert.match(r.note, /논리 CPU 72개 대비/);
    assert.match(r.note, /메모리는 121\.5 GiB 대비/);
    const noPhys = JSON.parse(JSON.stringify(DIAG)); noPhys.host.physical_cores = null;
    await render(page, 'overview', noPhys);
    r = await read();
    assert.equal(r.cells[0].sub, null);
                                  
    await render(page, 'vms');
    const vmsTitles = await page.evaluate(() => [...document.querySelectorAll('#cb .hc h4')].map(h => h.textContent));
    assert.ok(vmsTitles.includes('할당률(물리 용량 대비 설정 합)'));
    assert.ok(!vmsTitles.includes('Total vCPU') && !vmsTitles.includes('Total Memory'));
  });
});

test('overview layout and table semantics', async () => {
  await withPage(MODS, async page => {
    await setup(page);
    await render(page, 'overview');
    const r = await page.evaluate(() => {
      const cb = document.getElementById('cb');
      const vmSec = [...cb.querySelectorAll('h3.section-title')].find(h => h.textContent === 'VM 자원');
      const anom = [...cb.querySelectorAll('h4')].find(h => /이상 징후|Anomal/.test(h.textContent));
      const tables = [...cb.querySelectorAll('table.vmdiag-top5')];
      return {
        order: vmSec && anom ? (vmSec.compareDocumentPosition(anom) & Node.DOCUMENT_POSITION_FOLLOWING) !== 0 : null,
        n: tables.length,
        sem: tables.map(t => !!t.querySelector('caption') && [...t.querySelectorAll('thead th')].every(th => th.getAttribute('scope') === 'col')
          && t.querySelectorAll('thead th').length === 3),
        excl: cb.querySelector('.vmdiag-overview').textContent.match(/값 없는 VM \d대 제외/g)
      };
    });
    assert.equal(r.order, true, '"VM 자원" 절이 이상 징후 패널보다 앞');
    assert.equal(r.n, 5);
    assert.deepEqual(r.sem, [true, true, true, true, true]);
    assert.ok(r.excl && r.excl.length >= 1);
  });
});

                                                                                            
                                                                          
                                                                                       
const LONG = 'vmdiag_long_name_with_underscores_for_scope_rule_check';
function longDiag() {
  const d = JSON.parse(JSON.stringify(DIAG));
  d.vms.push(vm(LONG, { vcpus: 32, cpu_host_pct: 50, mem_rss_mb: 99999,
    disk: { rd_bps: 1e8, wr_bps: 1e8, rd_iops: 10, wr_iops: 20, rd_lat_ms: 1.2, wr_lat_ms: 2.5, fl_lat_ms: 0 },
    net: { rx_bps: 1e8, tx_bps: 1e8, rx_pps: 100, tx_pps: 50, drop_ps: 0, err_ps: 0 } }));
  return d;
}
async function shellWidth(page, w) {
  await page.setViewport({ width: w, height: 900 });
  await page.evaluate(pad => { document.body.style.paddingLeft = pad + 'px'; }, w > 1024 ? 260 : (w > 768 ? 220 : 0));
}
test('top5 long vm name stays one line', async () => {
  await withPage(MODS, async page => {
    await setup(page);
    for (const w of [1440, 1024, 768]) {                                                         
      await shellWidth(page, w);
      await render(page, 'overview', longDiag());
      const r = await page.evaluate(long => {
        const tables = [...document.querySelectorAll('#cb table.vmdiag-top5')];
        const cb = document.getElementById('cb');
        const cbRight = cb.getBoundingClientRect().right;
        return {
          over: document.documentElement.scrollWidth - window.innerWidth,
          cbOver: cb.scrollWidth - cb.clientWidth,                                                  
          tables: tables.map(t => {
            const rows = [...t.querySelectorAll('tbody tr')];
            const lr = rows.find(tr => tr.cells[1] && tr.cells[1].textContent === long);
            const sr = rows.find(tr => tr !== lr && tr.cells.length === 3);
            const lines = el => {                                                                  
              const n = document.createTreeWalker(el, NodeFilter.SHOW_TEXT).nextNode();
              const rg = document.createRange(); rg.selectNodeContents(n);
              return new Set([...rg.getClientRects()].filter(q => q.height > 0).map(q => Math.round(q.top))).size;
            };
            return { title: t.querySelector('caption').textContent, has: !!lr, longH: lr ? lr.getBoundingClientRect().height : null,
                     shortH: sr ? sr.getBoundingClientRect().height : null, nameLines: lr ? lines(lr.cells[1]) : null,
                     nameTitle: lr ? lr.cells[1].title : null, valLines: lr ? lines(lr.cells[2]) : null,
                     wide: t.scrollWidth - t.parentElement.clientWidth,
                     cardOut: Math.round(t.closest('.hc').getBoundingClientRect().right - cbRight) };
          })
        };
      }, LONG);
      assert.equal(r.tables.length, 5);
      const fails = [];                               
      if (r.over > 0) fails.push(`${w}px 가로 넘침 ${r.over}`);
      if (r.cbOver > 0) fails.push(`${w}px 본문(#cb) 가로 넘침 ${r.cbOver}`);
      for (const t of r.tables) {
        if (!t.has) { fails.push(`${w}px ${t.title}: 긴 이름 VM 행 없음`); continue; }
        if (t.nameLines !== 1) fails.push(`${w}px ${t.title}: 이름 ${t.nameLines}줄`);
        if (t.valLines !== 1) fails.push(`${w}px ${t.title}: 값 ${t.valLines}줄`);
        if (t.nameTitle !== LONG) fails.push(`${w}px ${t.title}: title=${t.nameTitle}`);
        if (Math.abs(t.longH - t.shortH) > 1) fails.push(`${w}px ${t.title}: 행 높이 ${t.longH} ≠ ${t.shortH}`);
        if (t.wide > 0) fails.push(`${w}px ${t.title}: 표가 카드 밖으로 ${t.wide}px`);
        if (t.cardOut > 0) fails.push(`${w}px ${t.title}: 카드가 본문 밖으로 ${t.cardOut}px`);
      }
      assert.deepEqual(fails, []);
    }
  });
});

                                                                         
test('vm diag table numeric cells do not wrap', async () => {
  await withPage(MODS, async page => {
    await setup(page);
    for (const w of [1440, 1024]) {
      await shellWidth(page, w);
      await render(page, 'vms', longDiag());
      const bad = await page.evaluate(() => {
        const out = [];
        for (const td of document.querySelectorAll('#cb table.vmdiag-table tbody td.mono')) {
          const walker = document.createTreeWalker(td, NodeFilter.SHOW_TEXT);
          for (let n = walker.nextNode(); n; n = walker.nextNode()) {
            if (!n.textContent.trim() || n.parentElement.closest('.sr-only')) continue;
            const rg = document.createRange(); rg.selectNodeContents(n);
            if (new Set([...rg.getClientRects()].filter(q => q.height > 0).map(q => Math.round(q.top))).size > 1) out.push(td.dataset.label + ': ' + n.textContent);
          }
        }
        return out;
      });
      assert.deepEqual(bad, [], w + 'px 수치 칸 줄바꿈');
      const over = await page.evaluate(() => document.documentElement.scrollWidth - window.innerWidth);
      assert.ok(over <= 0, w + 'px 가로 넘침 ' + over);
    }
  });
});

test('rollup vm signal segment', async () => {
  await withPage(MODS, async page => {
    await setup(page);
    await render(page, 'overview');
    const seg = await page.evaluate(() => {
      const s = document.querySelector('#cb .statusbar .seg[data-seg-key="vmsignal"]');
      return { cls: s.className, count: s.querySelector('.seg-big').textContent, label: s.querySelector('span.seg-sub').textContent,
               n: document.querySelectorAll('#cb .statusbar .seg').length };
    });
    assert.deepEqual(seg, { cls: 'seg seg-warn', count: '2', label: 'VM 신호', n: 5 });
    await page.focus('#cb .seg[data-seg-key="vmsignal"]');
    await page.keyboard.press('Enter');
    assert.deepEqual(await page.evaluate(() => window.__nav), ['mon-vms']);
    await render(page, 'overview', { fail: 'http' });
    const failed = await page.evaluate(() => document.querySelector('#cb .seg[data-seg-key="vmsignal"] .seg-big').textContent);
    assert.equal(failed, '—');
  });
});

test('vm diag table columns details and card mobile', async () => {
  await withPage(MODS, async page => {
    await setup(page);
    await render(page, 'vms');
    const r = await page.evaluate(() => {
      const t = document.querySelector('#cb table.vmdiag-table');
      return { heads: [...t.querySelectorAll('thead th')].map(th => th.textContent), mobile: t.classList.contains('card-mobile'),
               labels: [...t.querySelectorAll('tbody td')].every(td => td.hasAttribute('data-label')) };
    });
    assert.deepEqual(r.heads, ['VM', '상태', '데이터', 'CPU %(vCPU 대비)', '호스트 %', '게스트 메모리(사용/할당)', 'RSS',
      '디스크(읽기/쓰기 B/s)', '디스크 지연 ms', '네트워크(수신/송신 B/s)', '신호']);
    assert.ok(r.mobile && r.labels);
                             
    await page.focus('#cb .vmdiag-details summary');
    await page.keyboard.press('Enter');
    assert.equal(await page.evaluate(() => document.querySelector('#cb .vmdiag-details').open), true);
    for (const w of [1440, 1024, 768]) {
      await page.setViewport({ width: w, height: 900 });
      await render(page, 'vms');
      const over = await page.evaluate(() => document.documentElement.scrollWidth - window.innerWidth);
      assert.ok(over <= 0, w + 'px 가로 넘침 ' + over);
    }
  });
});

test('network timeline is rate', async () => {
  await withPage(MODS, async page => {
    await setup(page);
    const last = await page.evaluate(async node => {
      let now = 2_000_000;
      Date.now = () => now;
      const vals = [0, 125e6];
      let k = 0;
      window.fetch = () => Promise.resolve(new Response('node_network_receive_bytes_total{device="eno1"} ' + vals[k++] + '\n', { status: 200 }));
      const series = {};
      window.pcvTimeSeries = (id, s) => { series[id] = s; };
      let nodes;
      for (let i = 0; i < 2; i++) { nodes = await window.fetchAllMetrics(); now += 10_000; }
      const n = Object.assign({}, node, nodes[0], { node: 'edge-a' });
      window.renderMonOverview(document.getElementById('cb'), [n], [], 0, 0, 0, 0, 0, { ok: false, data: null, err: 'http' });
      await new Promise(r => setTimeout(r, 250));
      const rx = series['pcv-chart-net'][0].data;
      return rx[rx.length - 1];
    }, NODE);
    assert.ok(Math.abs(last - 12.5) <= 0.625, '마지막 점 ' + last);
  });
});

                                                                                           
test('all vms last column is host rss', async () => {
  await withPage(MODS, async page => {
    await setup(page);
    await render(page, 'overview');
    const t = await page.evaluate(() => {
      const card = [...document.querySelectorAll('#cb .hc')].find(c => ((c.querySelector('h4') || {}).textContent || '').startsWith('All VMs'));
      const rows = {};
      card.querySelectorAll('tbody tr').forEach(tr => { rows[tr.cells[0].textContent] = tr.cells[4].textContent; });
      return { head: [...card.querySelectorAll('thead th')].map(th => th.textContent), rows };
    });
    assert.equal(t.head[4], '호스트 RSS');
    assert.equal(t.rows['e-partial'], '8.8 GiB');                                     
    assert.equal(t.rows['a-ok'], '6.8 GiB');                                          
    assert.equal(t.rows['g-off'], '—');                            
  });
});

                                                                                    
test('vm tab keeps stale guest value with badge', async () => {
  await withPage(MODS, async page => {
    await setup(page);
    const d = JSON.parse(JSON.stringify(DIAG));
    const a = d.vms.find(v => v.name === 'a-ok');
    a.guest_stats_age_s = 2954;
    a.hints = [{ code: 'guest_stats_stale', level: 'notice', value: 2954, threshold: 30 }];
    await render(page, 'vms', d);
    const row = await page.evaluate(() => [...document.querySelectorAll('#cb table.vmdiag-table tbody tr')]
      .map(tr => [...tr.cells].map(c => c.innerText.replace(/\s+/g, ' ').trim())).find(c => c[0] === 'a-ok'));
    assert.equal(row[5], '4.0 GiB / 8.0 GiB');
    assert.match(row[10], /게스트 값 오래됨 · 참고/);
  });
});

test('rpc failure kinds', async () => {
  await withPage(MODS, async page => {
    await setup(page);
    const get = async status => {
      await page.evaluate(async st => {
        window.fetch = (url, o) => String(url).endsWith('/api/v1/metrics')
          ? Promise.resolve(new Response('purecvisor_host_cpu_percent 1\n', { status: 200 }))
                                                                                                                 
          : Promise.resolve(new Response(JSON.stringify(st === 500 ? { error: { message: 'x' } }
            : { error: { code: 'NOT_FOUND', message: 'Endpoint not found or method not allowed' } }),
            { status: st, headers: { 'content-type': 'application/json' } }));
        window.startAdaptivePolling = () => {};
        window.currentTab = 'mon-overview';
        await window.renderMonitoring(document.getElementById('cb'), 'overview');
      }, status);
      return page.evaluate(() => ({ text: document.querySelector('#cb .vmdiag-overview').textContent,
        retry: [...document.querySelectorAll('#cb .vmdiag-overview button')].map(b => b.textContent),
        nodeCards: [...document.querySelectorAll('#cb .hr .k')].filter(k => k.textContent === 'Net I/O').length }));
    };
    let r = await get(500);
    assert.match(r.text, /VM 진단을 불러오지 못했습니다/);
    assert.deepEqual(r.retry, ['다시 시도']);
    assert.ok(r.nodeCards >= 1, '호스트 카드는 그대로');
                                                                                      
    const counts = await page.evaluate(() => {
      const seg = document.querySelector('#cb .statusbar .seg[data-seg-key="vms"]');
      const card = t => [...document.querySelectorAll('#cb .hc')].find(c => (c.querySelector('h4') || {}).textContent === t);
      return { seg: seg.querySelector('.seg-big').textContent, segCls: seg.className, segSub: seg.querySelector('div.seg-sub').textContent,
               vmCard: card('VM').querySelector('.stat-xl').textContent,
               allVms: [...document.querySelectorAll('#cb .hc h4')].map(h => h.textContent).filter(t => t.startsWith('All VMs')) };
    });
    assert.deepEqual(counts, { seg: '—', segCls: 'seg seg-idle', segSub: '진단 없음', vmCard: '—', allVms: ['All VMs (—)'] });
    await render(page, 'vms', { fail: 'http' });
    const vmTab = await page.evaluate(() => ({
      stats: [...document.querySelectorAll('#cb .hc')].filter(c => ['Total', 'Running'].includes((c.querySelector('h4') || {}).textContent))
        .map(c => c.querySelector('.stat-xl').textContent),
      chipCounts: [...document.querySelectorAll('#cb .filterbar .chip-c')].map(n => n.textContent)
    }));
    assert.deepEqual(vmTab, { stats: ['—', '—'], chipCounts: [] });
    r = await get(404);
    assert.match(r.text, /이 데몬은 VM 진단을 지원하지 않습니다/);
  });
});

test('storage card names match allocation', async () => {
  await withPage(MODS, async page => {
    await setup(page);
    await render(page, 'storage');
    const r = await page.evaluate(() => ({
      titles: [...document.querySelectorAll('#cb .hc h4')].map(h => h.textContent),
      text: document.getElementById('cb').textContent
    }));
    assert.ok(r.titles.includes('등록 VM 최대 메모리 합'));
    assert.ok(!r.titles.includes('VM Provisioned'));
    assert.match(r.text, /128\.0 GiB/);
    assert.match(r.text, /1\.05/);
    assert.match(r.text, /메모리는 121\.5 GiB 대비입니다/);
  });
});
