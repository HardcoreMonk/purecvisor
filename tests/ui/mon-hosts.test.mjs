                                                                                             
                                                                                           
                                                            
  
                              
  
                                                               
                                                    
           
  
                                                                                  
                                                   
                                  
  
                                                                       
                                                                                          
                                                                                            
                                                                                   
                            
  
                                              
   
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

function node(overrides) {
  return Object.assign({
    node: 'edge-x', ip: '192.0.2.1', ram_total: 34359738368,
    memInfo: { MemTotal: 33554432, MemAvailable: 10000000, Cached: 1000000, Buffers: 500000, MemFree: 2000000, Slab: 200000 },
    load1: 0.5, load5: 0.4, load15: 0.3, cores: {}, netdevs: {}, disks: {}, error: false
  }, overrides);
}

const HOST_UP = node({ node: 'edge-a', ip: '192.0.2.11', cpu: 95, mem: 80, disk: 90, temp: 70, error: false });
const HOST_DOWN = node({ node: 'edge-b', ip: '192.0.2.12', cpu: 10, mem: 10, disk: 10, temp: 55, error: true });
const ALL = [HOST_UP, HOST_DOWN];

async function boot(page, all = ALL) {
  await page.setViewport({ width: 1280, height: 800 });
  await page.evaluate(nodes => {
    window._DEBUG = true;
    window.authToken = 'test-token';
    window.currentTab = 'mon-hosts';
    window._L = (ko, en) => en;
    window.t = key => key;
    return window.renderMonHosts(document.getElementById('cb'), nodes);
  }, all);
}

test('smoke: renders one block per host with correct UP/DOWN status pill and identity', async () => {
  await withPage(MODS, async page => {
    await boot(page);
                                                                  
                                             
    const blocks = await page.$$eval('#cb > div:not(.pagehead)', els => els.map(el => {
      const pill = el.querySelector('.justify-between.items-center .pill');
      return {
        heading: el.querySelector('h4.text-14').textContent.replace(/\s+/g, ' ').trim(),
        pillClass: pill.className, pillText: pill.textContent,
        gaugeCount: el.querySelectorAll('.gauge-inline').length
      };
    }));
    assert.equal(blocks.length, 2, 'one bordered block per host (smoke)');
    assert.match(blocks[0].heading, /^edge-a\s+192\.168\.3\.11/);
    assert.equal(blocks[0].pillClass, 'pill pill-ok');
    assert.equal(blocks[0].pillText, 'UP');
    assert.match(blocks[1].heading, /^edge-b\s+192\.168\.3\.12/);
    assert.equal(blocks[1].pillClass, 'pill pill-crit');
    assert.equal(blocks[1].pillText, 'DOWN');
    assert.deepEqual(blocks.map(b => b.gaugeCount), [3, 3]);
  }, {});
});

test('CPU/Mem/Disk gauges and Temperature pill map thresholds independently of host UP/DOWN (monitor.js:993-996)', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    const result = await page.$$eval('#cb > div:not(.pagehead)', els => els.map(el => {
      const grid = el.querySelector('.sg.grid-6');
      const gaugeLevel = i => [...grid.children[i].querySelector('.gauge-inline').classList]
        .find(c => c.startsWith('g-')).replace('g-', '');
                                                                                   
      const tempCard = [...grid.querySelectorAll(':scope > .hc')]
        .find(c => c.querySelector('h4')?.textContent.trim() === 'Temperature');
      const tempPill = tempCard.querySelector('.pill');
      return {
        cpu: gaugeLevel(0), mem: gaugeLevel(1), disk: gaugeLevel(2),
        tempClass: tempPill.className, tempText: tempPill.textContent.trim()
      };
    }));
    assert.deepEqual(result[0], { cpu: 'crit', mem: 'warn', disk: 'crit', tempClass: 'pill pill-crit', tempText: 'HOT' });
    assert.deepEqual(result[1], { cpu: 'ok', mem: 'ok', disk: 'ok', tempClass: 'pill pill-warn', tempText: 'WARM' });
  }, {});
});

test('Network Interfaces / Disk I/O tables exclude virtual devices per device filter regex (monitor.js:1036,1053)', async () => {
  const host = node({
    node: 'edge-c', ip: '192.0.2.13', cpu: 1, mem: 1, disk: 1, temp: 20, error: false,
    netdevs: {
      eth0: { receive_bytes_total: 1000, transmit_bytes_total: 500, receive_errs_total: 0, receive_drop_total: 0 },
      lo: { receive_bytes_total: 99999, transmit_bytes_total: 99999 },
      'ovs-system': { receive_bytes_total: 88888, transmit_bytes_total: 88888 }
    },
    disks: {
      'nvme0n1': { read_bytes_total: 2000, written_bytes_total: 1000, reads_completed_total: 5, writes_completed_total: 3 },
      sda: { read_bytes_total: 3000, written_bytes_total: 1500, reads_completed_total: 6, writes_completed_total: 4 },
      'dm-0': { read_bytes_total: 77777, written_bytes_total: 77777, reads_completed_total: 1, writes_completed_total: 1 }
    }
  });
  await withPage(MODS, async page => {
    await boot(page, [host]);
    const result = await page.evaluate(() => {
      const heading = title => [...document.querySelectorAll('#cb h4')].find(h => h.textContent.trim() === title);
      const tableAfter = h => h.closest('.hc').querySelector('table tbody');
      const netRows = [...tableAfter(heading('🌐 Network Interfaces')).querySelectorAll('tr')]
        .map(tr => tr.querySelector('td b').textContent);
      const diskRows = [...tableAfter(heading('💾 Disk I/O')).querySelectorAll('tr')]
        .map(tr => tr.querySelector('td b').textContent);
      return { netRows, diskRows };
    });
    assert.deepEqual(result.netRows, ['eth0']);
    assert.deepEqual(result.diskRows, ['nvme0n1', 'sda']);
  }, {});
});
