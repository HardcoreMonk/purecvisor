                                                                                            
                                                          
                                                                         
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const MODS = ['ui/modules/ui.js', 'ui/modules/uxlib.js', 'ui/modules/filter-state.js', 'ui/modules/mobile.js'];
const LOADER_MODS = [
  'ui/modules/endpoints.js', 'ui/modules/api.js', 'ui/modules/ui.js',
  'ui/modules/filter-state.js', 'ui/modules/uxlib.js', 'ui/modules/mobile.js',
];

const DATA = {
  alerts: [
    { timestamp: 100, severity: 'warn', metric: 'cpu', value: 82.5, message: 'cpu high' },
    { timestamp: 200, severity: 'crit', metric: 'disk', value: 96.0, message: 'disk full' },
    { timestamp: 300, level: 'critical', metric: 'memory', value: 91, message: 'compat critical' },
    { timestamp: 400, severity: 'other', metric: 'network', value: 1, message: 'unknown event' },
  ],
  suricata: { engine: { state: 'active' }, eve_tail: { alerts_crit: 4, alerts_warn: 7 } },
  ips: { enabled: true, mode: 'block' },
};

test('buildAlerts renders severity filterbar, alert rows, and Suricata card', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate((d) => {
      const node = window.PCV.mobile.buildAlerts(Object.assign({ filter: [] }, d));
      document.getElementById('cb').appendChild(node);
      return {
        root: node.className,
        chips: node.querySelectorAll('.filterbar .chip').length,
        rows: node.querySelectorAll('.m-alert-row').length,
        labels: Array.from(node.querySelectorAll('.m-alert-row .pill'), pill => pill.textContent),
        suri: !!node.querySelector('.m-suricata'),
        text: node.textContent,
      };
    }, DATA);
    assert.ok(r.root.includes('mscreen-body'));
    assert.ok(r.chips >= 2);                                
    assert.equal(r.rows, 4);                             
    assert.deepEqual(r.labels, ['UNKNOWN', 'CRIT', 'CRIT', 'WARN']);
    assert.equal(r.suri, true);
    assert.ok(r.text.includes('ACTIVE'));                           
    assert.ok(r.text.includes('11'));                                           
  });
});

test('buildAlerts isolates malformed records with unknown empty detail fallbacks', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate(() => {
      const node = window.PCV.mobile.buildAlerts({
        alerts: [null, 'primitive alert', []],
        filter: [],
      });
      return {
        labels: Array.from(node.querySelectorAll('.m-alert-row .pill'), pill => pill.textContent),
        details: Array.from(node.querySelectorAll('.m-alert-row .m-name'), detail => ({
          message: detail.firstElementChild.textContent,
          metric: detail.querySelector('.m-hint').textContent,
        })),
      };
    });

    assert.deepEqual(r.labels, ['UNKNOWN', 'UNKNOWN', 'UNKNOWN']);
    assert.deepEqual(r.details, [
      { message: '', metric: ' · ' },
      { message: '', metric: ' · ' },
      { message: '', metric: ' · ' },
    ]);
  });
});

test('buildAlerts filter narrows the list to the selected severity', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate((d) => {
      const critNode = window.PCV.mobile.buildAlerts(Object.assign({ filter: ['crit'] }, d));
      const warnNode = window.PCV.mobile.buildAlerts(Object.assign({ filter: ['warn'] }, d));
      const conflict = {
        alerts: [{ severity: 'warn', level: 'critical', message: 'canonical warning' }],
        filter: ['warn'],
      };
      const conflictNode = window.PCV.mobile.buildAlerts(conflict);
      return {
        critText: Array.from(critNode.querySelectorAll('.m-alert-row'), row => row.textContent),
        warnText: Array.from(warnNode.querySelectorAll('.m-alert-row'), row => row.textContent),
        conflictText: Array.from(conflictNode.querySelectorAll('.m-alert-row'), row => row.textContent),
      };
    }, DATA);
    assert.equal(r.critText.length, 2);
    assert.ok(r.critText.some(text => text.includes('disk full')));
    assert.ok(r.critText.some(text => text.includes('compat critical')));
    assert.equal(r.warnText.length, 1);
    assert.ok(r.warnText[0].includes('cpu high'));
    assert.equal(r.conflictText.length, 1);
    assert.ok(r.conflictText[0].includes('canonical warning'));
  });
});

                                                         
                                                              
test('mobile alert filter keeps one guarded subscription and repaints once per change', async () => {
  await withPage(LOADER_MODS, async (page) => {
    await page.evaluate(() => {
      window.authToken = 'test-token';
      const screen = document.createElement('div');
      screen.id = 'm-screen';
      document.body.appendChild(screen);
      window.PCV.mobile.showTab('alerts');
    });
    await page.waitForSelector('#m-screen .m-alert-row');

                                                  
    await page.evaluate(() => window.PCV.mobile.showTab('alerts'));
    await page.waitForSelector('#m-screen .m-alert-row');
    await page.evaluate(() => new Promise(resolve => setTimeout(resolve, 20)));

    const repaints = await page.evaluate(() => {
      const original = window.PCV.uxlib.clearEl;
      let count = 0;
      window.PCV.uxlib.clearEl = function (node) {
        if (node && node.id === 'm-screen') count++;
        return original(node);
      };
      window.PCV.ui.filterState.apply({ severity: ['crit'] });
      window.PCV.uxlib.clearEl = original;
      return count;
    });
    assert.equal(repaints, 1);
  }, {
    routes: {
      '/api/v1/alerts': { body: { data: DATA.alerts } },
      '/api/v1/rpc': (request) => {
        const method = request.json && request.json.method;
        if (method === 'suricata.status') return { body: { data: DATA.suricata } };
        if (method === 'suricata.ips.status') return { body: { data: DATA.ips } };
        return { body: { data: [] } };
      },
    },
  });
});
