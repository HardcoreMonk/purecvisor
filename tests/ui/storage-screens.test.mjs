                                                                                                 
                                                                              
                                                                        
  
                                    
  
                                                                             
                                   
  
                                                                     
                                                          
  
                                                               
                                                  
                                                           
                                                           
                                                                               
                                                             
                                 
  
                                                    
   
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const MODS = [
  'ui/modules/ui.js',
  'ui/modules/uxlib.js',
  'ui/modules/endpoints.js',
  'ui/modules/filter-state.js',
  'ui/modules/storage.js'
];

                                                                         
                                                         
const POOLS = [
  { name: 'tank', health: 'ONLINE', size: '100G', alloc: '50G' },
  { name: 'backup', health: 'DEGRADED', size: '100G', alloc: '92G' }
];
                                                                     
                                              
const ZVOLS = [];
                                                                         
const ISCSI = [
  { iqn: 'iqn.2026-01.local:target1', lun: 0, size: '50G', state: 'active' },
  { iqn: 'iqn.2026-01.local:target2', lun: 1, size: '20G', state: 'offline' }
];
                                                                              
const BACKUP_POLICIES = [
  { vm_name: 'vm-a', interval_hours: 24, retention_count: 7, enabled: true },
  { vm_name: 'vm-b', interval_hours: 12, retention_count: 3, enabled: false }
];
                                                                                
                                                                           
const FORECAST_POOLS = [
  { name: 'tank', used_percent: 50, days_to_full: 90 },
  { name: 'backup', used_percent: 92, days_to_full: 15 }
];

const ROUTES = {
  '/api/v1/storage/pools': { status: 200, body: { data: POOLS } },
  '/api/v1/storage/zvols': { status: 200, body: { data: ZVOLS } },
  '/api/v1/iscsi/targets': { status: 200, body: { data: ISCSI } },
  '/api/v1/rpc': record => {
    const m = record.json && record.json.method;
    const result =
      m === 'storage.pool.forecast' ? FORECAST_POOLS :
      m === 'backup.policy.list' ? BACKUP_POLICIES :
      [];
    return { status: 200, body: { result } };
  }
};

                                 
async function bootCommon(page) {
  await page.setViewport({ width: 1280, height: 800 });
  await page.evaluate(() => {
    window._DEBUG = true;
    window.authToken = 'test-token';
    window.currentTab = 'storage';
    window._L = (ko, en) => en;
    window.t = key => key;
    window.fmtBytes = n => String(n);
    window.unwrapData = r => r == null ? r
      : (r.data !== undefined ? r.data : (r.result !== undefined ? r.result : r));
    window.unwrapList = r => Array.isArray(r) ? r
      : (Array.isArray(window.unwrapData(r)) ? window.unwrapData(r) : []);
    window.fetchGet = url => fetch(url).then(async response => {
      const body = await response.json();
      if (!response.ok) throw new Error(body.error?.message || `HTTP ${response.status}`);
      return body;
    });
    window.fetchPost = (url, body) => fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body)
    }).then(r => r.json());
  });
}

test('storage tab: summary gauge inline (no legacy pb) + pool health pill (ok/crit, no legacy badge)', async () => {
  await withPage(MODS, async page => {
    await bootCommon(page);
    await page.evaluate(() => window.renderStorage(document.getElementById('cb')));
    assert.ok(await page.$('#cb .sg.grid-3'), 'summary group must render (smoke)');
                                                         
    assert.equal(await page.$$eval('#cb .sg.grid-3 .gauge-inline', els => els.length), 1);
    assert.equal(await page.$$eval('#cb .sg.grid-3 .pb', els => els.length), 0);
                                                              
                                                      
    const pills = await page.$$eval('#cb .hc h4 .pill', els => els.map(e => ({ cls: e.className, text: e.textContent })));
    assert.equal(pills.length, 2);
    assert.match(pills[0].cls, /pill-ok/);
    assert.equal(pills[0].text, 'ONLINE');
    assert.match(pills[1].cls, /pill-crit/);
    assert.equal(pills[1].text, 'DEGRADED');
    assert.equal(await page.$$eval('#cb .badge', els => els.length), 0);
  }, { routes: ROUTES });
});

test('forecast: gauge-inline (used 92 row = crit) + severity pill (ok/crit, no legacy badge)', async () => {
  await withPage(MODS, async page => {
    await bootCommon(page);
                                                                     
                                                               
    await page.evaluate(() => {
      const div = document.createElement('div');
      div.id = 'storage-forecast';
      document.getElementById('cb').appendChild(div);
      return window.loadStorageForecast();
    });
    const gauges = await page.$$eval('#storage-forecast .gauge-inline', els => els.map(e => e.className));
    assert.equal(gauges.length, 2);
    assert.match(gauges[1], /g-crit/);
    const pills = await page.$$eval('#storage-forecast .pill', els => els.map(e => ({ cls: e.className, text: e.textContent })));
    assert.equal(pills.length, 2);
    assert.match(pills[0].cls, /pill-ok/);
    assert.equal(pills[0].text, 'Healthy');
    assert.match(pills[1].cls, /pill-crit/);
    assert.equal(pills[1].text, 'Critical');
    assert.equal(await page.$$eval('#storage-forecast .badge', els => els.length), 0);
  }, { routes: ROUTES });
});

test('iscsi tab: state pill open set (active=ok, offline=warn), no legacy hardcoded-green badge', async () => {
  await withPage(MODS, async page => {
    await bootCommon(page);
    await page.evaluate(() => window.renderIscsi(document.getElementById('cb')));
    const pills = await page.$$eval('#cb table tbody tr', trs => trs.map(tr => {
      const p = tr.querySelector('.pill');
      return { cls: p && p.className, text: p && p.textContent };
    }));
    assert.equal(pills.length, 2);
    assert.match(pills[0].cls, /pill-ok/);
    assert.equal(pills[0].text, 'ACTIVE');
    assert.match(pills[1].cls, /pill-warn/);
    assert.equal(pills[1].text, 'OFFLINE');
    assert.equal(await page.$$eval('#cb .badge', els => els.length), 0);
  }, { routes: ROUTES });
});

test('backup tab: policy ON/OFF pill (enabled=ok, disabled=idle)', async () => {
  await withPage(MODS, async page => {
    await bootCommon(page);
    await page.evaluate(() => window.renderBackup(document.getElementById('cb')));
    const pills = await page.$$eval('#backup-policies .pill', els => els.map(e => ({ cls: e.className, text: e.textContent })));
    assert.equal(pills.length, 2);
    assert.match(pills[0].cls, /pill-ok/);
    assert.equal(pills[0].text, 'ON');
    assert.match(pills[1].cls, /pill-idle/);
    assert.equal(pills[1].text, 'OFF');
  }, { routes: ROUTES });
});
