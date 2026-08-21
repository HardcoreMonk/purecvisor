                                                                                               
                                                                
                                                                
  
                                 
  
                                                                  
                                                       
           
  
                                                                 
                                                         
                                                           
                                                 
                                                   
                                                                       
                        
  
                                                                     
                                                                                     
                                                                               
                                                                      
                        
  
                                                 
   
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

                                                                   
                                                                                 
const NODE_A = {
  node: 'edge-a', ip: '192.0.2.11', cpu: 95, mem: 10, disk: 99, temp: 55.2,
  load1: 1.2, load5: 1.0, load15: 0.8, ram_total: 34359738368,
  memInfo: { MemTotal: 33554432, MemAvailable: 10000000, Cached: 5000000, Buffers: 1000000, MemFree: 2000000, Slab: 500000, SwapTotal: 1000, SwapFree: 500 },
  sockstat: { sockets_used: 512, TCP_inuse: 300, UDP_inuse: 20 },
  conntrack: 4096, cores: {}, netdevs: {}, disks: {}, filesystems: [],
  keepalived_active: 1, keepalived_master: 1, keepalived_vip_owner: 1,
  anomaly_active: 1, anomaly_total: 42, healing_pending: 2, healing_total: 15,
  error: false
};
const NODE_B = {
  node: 'edge-b', ip: '192.0.2.12', cpu: 80, mem: 10, disk: 91, temp: 30,
  load1: 0.1, load5: 0.1, load15: 0.1, ram_total: 34359738368,
  memInfo: { MemTotal: 33554432, MemAvailable: 20000000, Cached: 1000000, Buffers: 500000, MemFree: 10000000, Slab: 200000, SwapTotal: 1000, SwapFree: 700 },
  sockstat: { sockets_used: 100, TCP_inuse: 60, UDP_inuse: 5 },
  conntrack: 512, cores: {}, netdevs: {}, disks: {}, filesystems: [],
  keepalived_active: 0, keepalived_master: 0, keepalived_vip_owner: 0,
  anomaly_active: 0, anomaly_total: 0, healing_pending: 0, healing_total: 0,
  error: false
};
const ALL = [NODE_A, NODE_B];
const ALL_VMS = [
  { name: 'vm-web01', node: 'edge-a', nodeIP: '192.0.2.11', running: 1, vcpu: 4, memory_max_mb: 4096, memory_used_mb: 2048, disk_rd_bytes: 500000, net_rx_bytes: 100000, net_tx_bytes: 50000 },
  { name: 'vm-db01', node: 'edge-a', nodeIP: '192.0.2.11', running: 1, vcpu: 8, memory_max_mb: 8192, memory_used_mb: 6000, disk_rd_bytes: 900000, net_rx_bytes: 200000, net_tx_bytes: 150000 },
  { name: 'vm-idle01', node: 'edge-b', nodeIP: '192.0.2.12', running: 0, vcpu: 2, memory_max_mb: 2048, memory_used_mb: 0, disk_rd_bytes: 0, net_rx_bytes: 0, net_tx_bytes: 0 }
];
const RUNNING = ALL_VMS.filter(v => v.running === 1).length;
const AVG_CPU = ALL.reduce((s, n) => s + n.cpu, 0) / ALL.length;
const AVG_MEM = ALL.reduce((s, n) => s + n.mem, 0) / ALL.length;
const AVG_DISK = ALL.reduce((s, n) => s + n.disk, 0) / ALL.length;
const TOTAL_RAM = ALL.reduce((s, n) => s + n.ram_total, 0);

                                                                   
async function boot(page) {
  await page.setViewport({ width: 1280, height: 800 });
  await page.evaluate(({ all, allVms, running, avgCpu, avgMem, avgDisk, totalRam }) => {
    window._DEBUG = true;
    window.authToken = 'test-token';
    window.currentTab = 'mon-overview';
    window._L = (ko, en) => en;
    window.t = key => key;
    return window.renderMonOverview(document.getElementById('cb'), all, allVms, running, avgCpu, avgMem, avgDisk, totalRam);
  }, { all: ALL, allVms: ALL_VMS, running: RUNNING, avgCpu: AVG_CPU, avgMem: AVG_MEM, avgDisk: AVG_DISK, totalRam: TOTAL_RAM });
}

function gaugeLevel(value, warn, crit) {
  if (value >= crit) return 'crit';
  if (value >= warn) return 'warn';
  return 'ok';
}

test('smoke: rollup bar reflects host/vm/anomaly/healing counts, All VMs table renders every VM', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    assert.ok(await page.$('#cb .statusbar'), 'rollup statusbar must render (smoke)');
    assert.ok(await page.$('#deep-health'), 'deep health mount point must exist');

    const segs = await page.$$eval('#cb .statusbar .seg', els => els.map(el => ({
      status: [...el.classList].find(c => c.startsWith('seg-')).replace('seg-', ''),
      count: el.querySelector('.seg-big').textContent.trim(),
      label: el.querySelector('span.seg-sub').textContent.trim(),
      sub: el.querySelector('div.seg-sub')?.textContent.trim()
    })));
    assert.deepEqual(segs, [
      { status: 'ok', count: '2', label: '호스트 UP', sub: '/ 2' },
      { status: 'warn', count: '2', label: 'VM 실행', sub: '/ 3' },
      { status: 'crit', count: '1', label: '이상 징후', sub: '활성' },
      { status: 'warn', count: '2', label: '복구 대기', sub: '대기' }
    ]);

    const vmRows = await page.$$eval('#cb table.table-sticky tbody tr', trs => trs.length);
    assert.equal(vmRows, ALL_VMS.length);
  }, {});
});

test('stat grid gauges map avg CPU/memory/disk/swap to warn|ok|crit thresholds (monitor.js:714-717)', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    const gauges = await page.$$eval('#cb .sg.grid-8 .gauge-inline', els =>
      els.map(el => [...el.classList].find(c => c.startsWith('g-')).replace('g-', '')));
    const tSwapUsed = ALL.reduce((s, n) => s + (n.memInfo.SwapTotal - n.memInfo.SwapFree), 0);
    const tSwapTotal = ALL.reduce((s, n) => s + n.memInfo.SwapTotal, 0);
    const swapPct = +((tSwapUsed / tSwapTotal * 100).toFixed(1));
    assert.deepEqual(gauges, [
      gaugeLevel(+AVG_CPU.toFixed(1), 80, 95),
      gaugeLevel(+AVG_MEM.toFixed(1), 80, 95),
      gaugeLevel(+AVG_DISK.toFixed(1), 80, 90),
      gaugeLevel(swapPct, 50, 80)
    ]);
    assert.equal(gauges[0], 'warn');
    assert.equal(gauges[1], 'ok');
    assert.equal(gauges[2], 'crit');

    const values = await page.$$eval('#cb .sg.grid-8 .stat-sm', els => els.map(el => el.textContent.trim()));
    assert.deepEqual(values, [
      AVG_CPU.toFixed(1) + '%',
      AVG_MEM.toFixed(1) + '%',
      AVG_DISK.toFixed(1) + '%',
      swapPct.toFixed(1) + '%'
    ]);
  }, {});
});

test('keepalived VRRP table maps active/master/VIP-owner state per node (monitor.js:828-838)', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    const rows = await page.$$eval('#cb table.text-12 tbody tr', trs => trs.map(tr => {
      const tds = tr.querySelectorAll('td');
      const kaPill = tds[1].querySelector('.pill');
      const rolePill = tds[2].querySelector('.pill');
      return {
        node: tds[0].querySelector('b').textContent,
        kaClass: kaPill.className, kaText: kaPill.textContent,
        roleClass: rolePill.className, roleText: rolePill.textContent,
        vip: tds[3].textContent.trim()
      };
    }));
    assert.deepEqual(rows, [
      { node: 'edge-a', kaClass: 'pill pill-ok', kaText: 'ACTIVE', roleClass: 'pill pill-ok', roleText: 'MASTER', vip: 'VIP' },
      { node: 'edge-b', kaClass: 'pill pill-crit', kaText: 'DOWN', roleClass: 'pill pill-warn', roleText: 'BACKUP', vip: '-' }
    ]);
  }, {});
});
