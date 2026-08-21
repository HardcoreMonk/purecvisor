                                                                                       
                                                                                      
                                                              
  
                            
  
                                                              
                                                  
           
  
                                                                                
                                                   
  
                                                                                  
                                                                              
                                                                         
                                                               
                                                        
                                  
  
                                            
   
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

const ALL_VMS = [
  { name: 'vm-web01', node: 'edge-a', running: 1, vcpu: 4, memory_max_mb: 4096, memory_used_mb: 3072 },
  { name: 'vm-db01', node: 'edge-a', running: 1, vcpu: 8, memory_max_mb: 8192, memory_used_mb: 7000 },
  { name: 'vm-idle01', node: 'edge-b', running: 0, vcpu: 2, memory_max_mb: 2048, memory_used_mb: 0 },
  { name: 'vm-idle02', node: 'edge-b', running: 0, vcpu: 2, memory_max_mb: 2048, memory_used_mb: 0 }
];
const RUNNING = ALL_VMS.filter(v => v.running === 1).length;

async function boot(page) {
  await page.setViewport({ width: 1280, height: 800 });
  await page.evaluate(({ allVms, running }) => {
    window._DEBUG = true;
    window.authToken = 'test-token';
    window.currentTab = 'mon-vms';
    window._L = (ko, en) => en;
    window.t = key => key;
    return window.renderMonVms(document.getElementById('cb'), allVms, running);
  }, { allVms: ALL_VMS, running: RUNNING });
}

test('smoke: stat grid totals and per-VM cards render with RUNNING/OFF status pill + memory gauge', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    assert.ok(await page.$('#cb .filterbar'), 'vmstate filterbar must render (smoke)');
    const stats = await page.$$eval('#cb .sg.grid-4 .stat-xl', els => els.map(el => el.textContent.trim()));
    assert.deepEqual(stats, [
      String(ALL_VMS.length),
      String(RUNNING),
      String(ALL_VMS.reduce((s, v) => s + v.vcpu, 0)),
      (ALL_VMS.reduce((s, v) => s + v.memory_max_mb, 0) / 1024).toFixed(1) + ' GB'
    ]);

    const cards = await page.$$eval('#cb .sg.grid-2 .hc', els => els.map(el => {
      const pill = el.querySelector('.pill');
      return { name: el.querySelector('h4').textContent, pillClass: pill.className, pillText: pill.textContent };
    }));
    assert.equal(cards.length, ALL_VMS.length);
    assert.deepEqual(cards, [
      { name: 'vm-web01', pillClass: 'pill pill-ok', pillText: 'RUNNING' },
      { name: 'vm-db01', pillClass: 'pill pill-ok', pillText: 'RUNNING' },
      { name: 'vm-idle01', pillClass: 'pill pill-idle', pillText: 'OFF' },
      { name: 'vm-idle02', pillClass: 'pill pill-idle', pillText: 'OFF' }
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

test('clicking a vmstate chip filters the VM grid client-side and issues no HTTP requests', async () => {
  await withPage(MODS, async (page, { requests }) => {
    await boot(page);
    await page.click('#cb .chip[data-facet="vmstate"][data-val="running"]');
    let names = await page.$$eval('#cb .sg.grid-2 .hc h4', els => els.map(el => el.textContent));
    assert.deepEqual(names, ['vm-web01', 'vm-db01']);

                                                                               
                                        
    const gaugeLevels = await page.$$eval('#cb .sg.grid-2 .gauge-inline', els =>
      els.map(el => [...el.classList].find(c => c.startsWith('g-')).replace('g-', '')));
    assert.deepEqual(gaugeLevels, ['ok', 'warn']);

                       
    await page.click('#cb .chip[data-facet="vmstate"][data-val="running"]');
    names = await page.$$eval('#cb .sg.grid-2 .hc h4', els => els.map(el => el.textContent));
    assert.equal(names.length, ALL_VMS.length);

    await page.click('#cb .chip[data-facet="vmstate"][data-val="stopped"]');
    names = await page.$$eval('#cb .sg.grid-2 .hc h4', els => els.map(el => el.textContent));
    assert.deepEqual(names, ['vm-idle01', 'vm-idle02']);
                                                                     
    const idleGauges = await page.$$eval('#cb .sg.grid-2 .stat-sm', els => els.map(el => el.textContent.trim()));
    assert.deepEqual(idleGauges, ['0.0%', '0.0%']);

    assert.equal(requests.length, 0, 'client-side filtering must not trigger any HTTP request');
  }, {});
});
