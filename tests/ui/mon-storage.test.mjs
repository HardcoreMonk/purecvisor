                                                                                                    
                                                                                  
                                                                      
  
                                
  
                                                                  
                                                      
           
  
                                                                                    
                                                          
                                                     
                                                                               
              
  
                                                                             
                                                                                   
                                                                
  
                                                
   
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const MODS = [
  'ui/modules/ui.js',
  'ui/modules/uxlib.js',
  'ui/modules/filter-state.js',
  'ui/modules/endpoints.js',
  'ui/modules/monitor.js'
];

                                                                  
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
const ALL_VMS = [
  { name: 'vm-a', memory_max_mb: 16384 },
  { name: 'vm-b', memory_max_mb: 16384 }
];
const TOTAL_RAM = ALL.reduce((s, n) => s + n.ram_total, 0);

async function boot(page) {
  await page.setViewport({ width: 1280, height: 800 });
  await page.evaluate(({ all, allVms, totalRam }) => {
    window._DEBUG = true;
    window.authToken = 'test-token';
    window.currentTab = 'mon-storage';
    window._L = (ko, en) => en;
    window.t = key => key;
    return window.renderMonStorage(document.getElementById('cb'), all, allVms, totalRam);
  }, { all: ALL, allVms: ALL_VMS, totalRam: TOTAL_RAM });
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

test('Datastore Usage and Overcommit gauges map warn/crit thresholds (monitor.js:1199-1212)', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    const datastoreLevels = await page.$$eval('#cb .gauge.gauge-inline', els =>
      els.map(el => [...el.classList].find(c => c.startsWith('g-')).replace('g-', '')));
    assert.deepEqual(datastoreLevels, ['ok', 'warn', 'crit']);

    const vmMem = ALL_VMS.reduce((s, v) => s + v.memory_max_mb, 0);
    const overcommitPct = +(TOTAL_RAM > 0 ? vmMem * 1048576 / TOTAL_RAM * 100 : 0).toFixed(1);
    const overcommit = await page.$eval('#cb .gauge:not(.gauge-inline)', el => ({
      level: [...el.classList].find(c => c.startsWith('g-')).replace('g-', ''),
      metric: el.querySelector('.gauge-metric').textContent.replace(el.querySelector('.gauge-unit')?.textContent || '', '')
    }));
    assert.equal(overcommit.level, overcommitPct >= 100 ? 'crit' : overcommitPct >= 80 ? 'warn' : 'ok');
    assert.equal(overcommit.metric, String(overcommitPct));
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
