                                                                                       
                                                                                      
                                                              
  
                            
  
                                                              
                                                  
           
  
                                                                                
                                                   
  
                                                                                  
                                                                              
                                                                         
                                                               
                                                        
                                  
  
                                            
   
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

                                                                               
function diagVm(name, state, vcpus, maxMb) {
  const on = state === 'running';
  return { name, uuid: 'u-' + name, state, quality: on ? 'ok' : 'stopped', reason: null, missing: [], vcpus, mem_max_mb: maxMb,
    mem_alloc_mb: on ? maxMb : null, mem_guest_used_mb: on ? maxMb / 2 : null, mem_rss_mb: on ? maxMb / 2 : null,
    cpu_pct: on ? 10 : null, cpu_host_pct: on ? 1 : null, last_ok_at_ms: 0,
    disk: { rd_bps: on ? 0 : null, wr_bps: on ? 0 : null, rd_iops: on ? 0 : null, wr_iops: on ? 0 : null, rd_lat_ms: null, wr_lat_ms: null, fl_lat_ms: null },
    net: { rx_bps: on ? 0 : null, tx_bps: on ? 0 : null, rx_pps: on ? 0 : null, tx_pps: on ? 0 : null, drop_ps: null, err_ps: null },
    contention: null, hints: [] };
}
function diagOf(vms, memTotalMb) {
  const run = vms.filter(v => v.state === 'running');
  const sum = (a, k) => a.reduce((s, v) => s + v[k], 0);
  return { ok: true, err: null, data: { schema: 1, scope: 'all',
    sampler: { state: 'ok', observed_at_ms: 0, age_ms: 2000, interval_ms: 5000, tracked: vms.length, truncated: 0, error: null },
    host: { logical_cpus: 16, physical_cores: null, mem_total_mb: memTotalMb,
      allocation: { vms_running: run.length, vms_defined: vms.length, vcpu_running: sum(run, 'vcpus'), vcpu_defined: sum(vms, 'vcpus'),
        vcpu_running_per_logical: sum(run, 'vcpus') / 16, vcpu_defined_per_logical: sum(vms, 'vcpus') / 16,
        mem_running_max_mb: sum(run, 'mem_max_mb'), mem_defined_max_mb: sum(vms, 'mem_max_mb'), mem_running_alloc_mb: sum(run, 'mem_max_mb'),
        mem_running_max_ratio: sum(run, 'mem_max_mb') / memTotalMb, mem_defined_max_ratio: sum(vms, 'mem_max_mb') / memTotalMb },
      psi: null },
    vms } };
}

const ALL_VMS = [
  { name: 'vm-web01', node: 'edge-a', running: 1, vcpu: 4, memory_max_mb: 4096, memory_used_mb: 3072 },
  { name: 'vm-db01', node: 'edge-a', running: 1, vcpu: 8, memory_max_mb: 8192, memory_used_mb: 7000 },
  { name: 'vm-idle01', node: 'edge-b', running: 0, vcpu: 2, memory_max_mb: 2048, memory_used_mb: 0 },
  { name: 'vm-idle02', node: 'edge-b', running: 0, vcpu: 2, memory_max_mb: 2048, memory_used_mb: 0 }
];
const RUNNING = ALL_VMS.filter(v => v.running === 1).length;
const DIAG = diagOf(ALL_VMS.map(v => diagVm(v.name, v.running === 1 ? 'running' : 'shutoff', v.vcpu, v.memory_max_mb)), 32768);

async function boot(page) {
  await page.setViewport({ width: 1280, height: 800 });
  await page.evaluate(({ running, diag }) => {
    window._DEBUG = true;
    window.authToken = 'test-token';
    window.currentTab = 'mon-vms';
    window._L = (ko, en) => en;
    window.t = key => key;
    return window.renderMonVms(document.getElementById('cb'), PCV.vmdiag.legacyVms(diag), running, diag);
  }, { running: RUNNING, diag: DIAG });
}

test('smoke: totals, allocation card and VM diag table rows with state text', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    assert.ok(await page.$('#cb .filterbar'), 'vmstate filterbar must render (smoke)');
    const stats = await page.$$eval('#cb .sg.grid-2 .stat-xl', els => els.map(el => el.textContent.trim()));
    assert.deepEqual(stats, [String(ALL_VMS.length), String(RUNNING)]);
                                                                     
    const titles = await page.$$eval('#cb .hc h4', els => els.map(el => el.textContent));
    assert.ok(titles.includes('할당률(물리 용량 대비 설정 합)'));
    assert.ok(!titles.includes('Total vCPU') && !titles.includes('Total Memory'));

    const rows = await page.$$eval('#cb table.vmdiag-table tbody tr', trs => trs.map(tr => {
      const td = tr.querySelectorAll('td');
      return { name: td[0].textContent, state: td[1].textContent };
    }));
    assert.deepEqual(rows, [
      { name: 'vm-web01', state: '실행' },
      { name: 'vm-db01', state: '실행' },
      { name: 'vm-idle01', state: '정지' },
      { name: 'vm-idle02', state: '정지' }
    ]);
  }, {});
});

test('filterbar chips expose running/stopped counts (monitor.js:1113-1118)', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    const chips = await page.$$eval('#cb .filterbar .chip', els => els.map(el => ({
      facet: el.getAttribute('data-facet'), val: el.getAttribute('data-val'),
                                                                                      
                                                                               
      label: el.querySelector('span:not([class])').textContent, count: el.querySelector('.chip-c').textContent
    })));
    assert.deepEqual(chips, [
      { facet: 'vmstate', val: 'running', label: 'Running', count: String(RUNNING) },
      { facet: 'vmstate', val: 'stopped', label: 'Stopped', count: String(ALL_VMS.length - RUNNING) }
    ]);
  }, {});
});

test('clicking a vmstate chip filters the VM diag table client-side and issues no HTTP requests', async () => {
  await withPage(MODS, async (page, { requests }) => {
    await boot(page);
    const names = () => page.$$eval('#cb table.vmdiag-table tbody tr td:first-child', els => els.map(el => el.textContent));
    await page.click('#cb .chip[data-facet="vmstate"][data-val="running"]');
    assert.deepEqual(await names(), ['vm-web01', 'vm-db01']);

                       
    await page.click('#cb .chip[data-facet="vmstate"][data-val="running"]');
    assert.equal((await names()).length, ALL_VMS.length);

    await page.click('#cb .chip[data-facet="vmstate"][data-val="stopped"]');
    assert.deepEqual(await names(), ['vm-idle01', 'vm-idle02']);
                                                
    const dashes = await page.$$eval('#cb table.vmdiag-table tbody tr td:nth-child(4)', els => els.map(el => el.firstChild.firstChild.textContent));
    assert.deepEqual(dashes, ['\u2014', '\u2014']);

    assert.equal(requests.length, 0, 'client-side filtering must not trigger any HTTP request');
  }, {});
});
