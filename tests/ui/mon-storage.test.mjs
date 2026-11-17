                                                                                                    
                                                                                  
                                                                      
  
                                
  
                                                                  
                                                      
           
  
                                                                                    
                                                          
                                                     
                                                                               
              
  
                                                                             
                                                                                   
                                                                
  
                                                
   
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

const NODE_OK = { node: 'edge-ok', ip: '192.0.2.21', disk: 50, ram_total: 34359738368, filesystems: [] };
const NODE_WARN = { node: 'edge-warn', ip: '192.0.2.22', disk: 85, ram_total: 34359738368, filesystems: [] };
const NODE_CRIT = {
  node: 'edge-crit', ip: '192.0.2.23', disk: 95, ram_total: 34359738368,
                                                                                      
                                                                          
                                                              
  filesystems: [
    { mount: '/data', fstype: 'zfs', size_bytes: 1000, avail_bytes: 500 },
    { mount: '/var', fstype: 'ext4', size_bytes: 1000, avail_bytes: 200 },
    { mount: '/boot', fstype: 'xfs', size_bytes: 1000, avail_bytes: 50 },
    { mount: '/tmp', fstype: 'tmpfs', size_bytes: 1000, avail_bytes: 999 }
  ]
};
const ALL = [NODE_OK, NODE_WARN, NODE_CRIT];
const TOTAL_RAM = ALL.reduce((s, n) => s + n.ram_total, 0);
const DIAG = diagOf([diagVm('vm-a', 'running', 4, 16384), diagVm('vm-b', 'shutoff', 4, 16384)], 98304);

async function boot(page) {
  await page.setViewport({ width: 1280, height: 800 });
  await page.evaluate(({ all, totalRam, diag }) => {
    window._DEBUG = true;
    window.authToken = 'test-token';
    window.currentTab = 'mon-storage';
    window._L = (ko, en) => en;
    window.t = key => key;
    return window.renderMonStorage(document.getElementById('cb'), all, PCV.vmdiag.legacyVms(diag), totalRam, diag);
  }, { all: ALL, totalRam: TOTAL_RAM, diag: DIAG });
}

test('smoke: Datastore Usage card renders one row per node, ZFS lock panel shows empty-state', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    assert.ok(await page.$('#cb .hc'), 'storage cards must render (smoke)');
    const rows = await page.evaluate(() => {
      const heading = [...document.querySelectorAll('#cb h4')].find(el => el.textContent.trim() === 'Datastore Usage');
      return [...heading.closest('.hc').querySelectorAll(':scope > div.mb-8')].map(el => {
        const spans = el.querySelectorAll(':scope > div span');
        return { node: spans[0].textContent, pct: spans[1].textContent };
      });
    });
    assert.deepEqual(rows, [
      { node: 'edge-ok', pct: '50.0%' },
      { node: 'edge-warn', pct: '85.0%' },
      { node: 'edge-crit', pct: '95.0%' }
    ]);
    assert.equal(
      await page.$eval('#cb .empty-state-text', el => el.textContent.trim()),
      'No ZFS inflight lock samples yet'
    );
  }, {});
});

test('Datastore Usage gauges map warn/crit thresholds and VM memory card uses the allocation definition', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    const datastoreLevels = await page.$$eval('#cb .gauge.gauge-inline', els =>
      els.map(el => [...el.classList].find(c => c.startsWith('g-')).replace('g-', '')));
    assert.deepEqual(datastoreLevels, ['ok', 'warn', 'crit']);

    const card = await page.evaluate(() => {
      const h = [...document.querySelectorAll('#cb h4')].map(el => el.textContent);
      return { titles: h, text: document.getElementById('cb').textContent };
    });
    assert.ok(card.titles.includes('등록 VM 최대 메모리 합'));
    assert.ok(!card.titles.includes('VM Provisioned') && !card.titles.includes('Overcommit'));
    assert.match(card.text, /32\.0 GiB/);
    assert.match(card.text, /0\.33/);
    assert.match(card.text, /메모리는 96\.0 GiB 대비입니다/);
  }, {});
});

test('Filesystems table maps Used % status pill per mount and excludes non-zfs/ext4/xfs fstypes', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    const heading = await page.$$eval('#cb h4', els =>
      els.map(el => el.textContent.trim()).filter(t => t.includes('Filesystems')));
    assert.deepEqual(heading, ['edge-crit — Filesystems'], 'only the node with matching fstypes gets a Filesystems card');

    const rows = await page.evaluate(() => {
      const h = [...document.querySelectorAll('#cb h4')].find(el => el.textContent.trim() === 'edge-crit — Filesystems');
      return [...h.closest('.hc').querySelectorAll('table tbody tr')].map(tr => {
        const tds = tr.querySelectorAll('td');
        const pill = tds[4].querySelector('.pill');
        return { mount: tds[0].textContent.trim(), fstype: tds[1].textContent, pillClass: pill.className, pillText: pill.textContent };
      });
    });
    assert.deepEqual(rows, [
      { mount: '/data', fstype: 'zfs', pillClass: 'pill pill-ok', pillText: '50.0%' },
      { mount: '/var', fstype: 'ext4', pillClass: 'pill pill-warn', pillText: '80.0%' },
      { mount: '/boot', fstype: 'xfs', pillClass: 'pill pill-crit', pillText: '95.0%' }
    ]);
  }, {});
});
