                                                                                             
                                                                            
                                                                    
  
                                         
  
                                                             
                                                                     
                                                        
  
                                                
                                                                             
                                        
                                                                             
                                                 
                                                                
                                                            
                                                                  
                                                  
  
                                                         
   
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

                                                                        
const MODS = [
  'ui/modules/ui.js',
  'ui/modules/uxlib.js',
  'ui/modules/endpoints.js',
  'ui/modules/filter-state.js',
  'ui/modules/vm.js',
  'ui/modules/container.js'
];

const CTR_LIST = [
  { name: 'ctr-a', state: 'RUNNING', ip_addr: '10.0.3.5' },
  { name: 'ctr-b', state: 'STOPPED' }
];
const CTR_METRICS = { data: { cpu_percent: 42.5, mem_used_mb: 256, mem_limit_mb: 512, init_pid: 1234, net_rx_mb: 1, net_tx_mb: 2 } };
const CTR_EXEC = { data: { output: 'stub' } };

const ROUTES = {
  '/api/v1/containers': { status: 200, body: { data: CTR_LIST } },
  '/api/v1/containers/ctr-a/metrics': { status: 200, body: CTR_METRICS },
  '/api/v1/containers/ctr-a/exec': { status: 200, body: CTR_EXEC }
};

async function bootCommon(page) {
  await page.setViewport({ width: 1280, height: 800 });
  await page.evaluate(() => {
    window._DEBUG = true;
    window.authToken = 'test-token';
    window.currentTab = 'vm';
    window._L = (ko, en) => en;
    window.t = key => key;
                                                                       
                                                          
                                                    
    window.unwrapData = r => r == null ? r
      : (r.data !== undefined ? r.data : (r.result !== undefined ? r.result : r));
    window.unwrapList = r => Array.isArray(r) ? r
      : (Array.isArray(window.unwrapData(r)) ? window.unwrapData(r) : []);
    window.fetchGet = url => fetch(url).then(async response => {
      const body = await response.json().catch(() => null);
      if (!response.ok) throw new Error((body && body.error && body.error.message) || `HTTP ${response.status}`);
      return body;
    });
    window.fetchPost = (url, body) => fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body)
    }).then(r => r.json());
                                                                   
                                                 
    window.selectedVmIndex = 0;
  });
}

test('vm summary: header/auto_start/storage pills (ok tone map) + CPU/MEM inline gauges, no legacy pb/badge', async () => {
  await withPage(MODS, async page => {
    await bootCommon(page);
    const v = { name: 'web-01', state: 'running', live_cpu_pct: 42.5, mem_percent: 61, vcpu: 4, memory_mb: 4096, auto_start: true, storage_type: 'zvol' };
    await page.evaluate(v => window.renderSummary(document.getElementById('cb'), v), v);

    assert.equal(await page.$$eval('#cb .gauge-inline', els => els.length), 2, 'CPU + Memory gauge-inline');
    assert.equal(await page.$$eval('#cb .pb', els => els.length), 0, 'legacy _vmProgressBar must be gone (real RED axis)');
    assert.equal(await page.$$eval('#cb .badge', els => els.length), 0, 'legacy HN.badge must be gone');

    const pills = await page.$$eval('#cb .pill', els => els.map(e => ({ cls: e.className, text: e.textContent })));
    assert.equal(pills.length, 3);
    assert.match(pills[0].cls, /pill-ok/);
    assert.equal(pills[0].text, 'RUNNING');
    assert.match(pills[1].cls, /pill-ok/);
    assert.equal(pills[1].text, 'ON');
    assert.match(pills[2].cls, /pill-idle/);
    assert.equal(pills[2].text, 'ZVOL');
  }, { routes: ROUTES });
});

test('vm summary: HIGH_LOAD cpu (>85) flips header pill to warn tone', async () => {
  await withPage(MODS, async page => {
    await bootCommon(page);
    const v = { name: 'web-01', state: 'running', live_cpu_pct: 92, mem_percent: 61, vcpu: 4, memory_mb: 4096, auto_start: true, storage_type: 'zvol' };
    await page.evaluate(v => window.renderSummary(document.getElementById('cb'), v), v);

    const header = await page.$eval('#cb .pill', e => ({ cls: e.className, text: e.textContent }));
    assert.match(header.cls, /pill-warn/);
    assert.match(header.text, /\[HIGH_LOAD\]/);
  }, { routes: ROUTES });
});

test('vm card view: statusDot(glow) + inline gauges (CPU % text preserved) + status pill, no legacy pb/badge', async () => {
  await withPage(MODS, async page => {
    await bootCommon(page);
    await page.evaluate(() => {
      const cb = document.getElementById('cb');
      ['vl', 'vc', 'sb2', 'bbtn', 'ct'].forEach(id => { const d = document.createElement('div'); d.id = id; cb.appendChild(d); });
      window.vmList = [{ name: 'web-01', state: 'running', live_cpu_pct: 42.5, mem_percent: 61, vcpu: 4, memory_mb: 4096 }];
      window.checkedVms = new Set();
      window.sortField = 'name'; window.sortDirection = 1;
      window.lastLoadTime = Date.now();                              
      window.renderContent = () => {};                             
      window.toggleVmView();                                                                            
    });

    assert.equal(await page.$$eval('#cb .badge', els => els.length), 0);
    assert.equal(await page.$$eval('#cb .pb', els => els.length), 0);
    assert.equal(await page.$$eval('#cb .gauge-inline', els => els.length), 2);
    assert.equal(await page.$$eval('#cb .sdot-ok.sdot-glow', els => els.length), 1, 'glow is a separate class from the ok status class');
    const pills = await page.$$eval('#cb .pill', els => els.map(e => e.className + '|' + e.textContent));
    assert.deepEqual(pills, ['pill pill-ok|RUNNING']);
    assert.ok((await page.$eval('#cb', b => b.textContent)).includes('42.5%'), 'V2 numeric label must survive next to the gauge');
  }, { routes: ROUTES });
});

test('container main panel: sidebar dots + count pill + detail header pill + Summary tab inline gauges (CPU/MEM), no legacy pb/badge', async () => {
  await withPage(MODS, async page => {
    await bootCommon(page);
    await page.evaluate(() => { window.selCtr = 'ctr-a'; });                                                   
    await page.evaluate(() => window.renderContainers(document.getElementById('cb')));

    assert.equal(await page.$$eval('#cb .sdot-ok.sdot-glow', els => els.length), 1);
    assert.equal(await page.$$eval('#cb .sdot-idle', els => els.length), 1);
    assert.equal(await page.$$eval('#cb span', spans => spans.filter(s => s.textContent === '●').length), 0);

    const pills = await page.$$eval('#cb .pill', els => els.map(e => ({ cls: e.className, text: e.textContent })));
    assert.equal(pills.length, 2);
    assert.match(pills[0].cls, /pill-idle/);
    assert.equal(pills[0].text, '2');
    assert.match(pills[1].cls, /pill-ok/);
    assert.equal(pills[1].text, 'RUNNING');

    assert.equal(await page.$$eval('#ctr-tab-content .gauge-inline', els => els.length), 2, 'CPU 42.5% + MEM 50%(256/512) gauges');
    assert.equal(await page.$$eval('#cb .pb', els => els.length), 0);
    assert.equal(await page.$$eval('#cb .badge', els => els.length), 0);
  }, { routes: ROUTES });
});
