                                                                                          
                                                              
                                                               
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

                   
                                                                   
                                                            
                                                                 
                                                                  
                                                            
                                                                   
                                                                 
                                               
                                                          
                                                                     
                                                              
                                              
                                                                  
                            

const MODULES = [
  'ui/i18n.js',
  'ui/modules/endpoints.js',
  'ui/modules/api.js',
  'ui/modules/ui.js',
  'ui/modules/filter-state.js',
  'ui/modules/uxlib.js',
  'ui/modules/modal-core.js',
  'ui/modules/modal.js',
  'ui/modules/charts.js',
  'ui/modules/metrics.js',
  'ui/modules/security.js',
  'ui/modules/monitor.js',
  'ui/modules/vm.js',
  'ui/modules/vm-console.js',
  'ui/modules/vm-lifecycle.js',
  'ui/modules/vm-guest.js',
  'ui/modules/container.js',
  'ui/modules/network.js',
  'ui/modules/storage.js',
  'ui/modules/cloud.js',
  'ui/modules/help.js',
  'ui/modules/nav.js',
  'ui/modules/theme.js',
  'ui/modules/accounts.js',
  'ui/modules/advanced.js',
  'ui/modules/selfhealing.js',
  'ui/modules/mobile.js',
  'ui/app.js',
];

const ALERTS = [
  { timestamp: 'canonical-time', severity: 'crit', message: 'canonical critical' },
  { time: 'compat-time', level: 'warning', detail: 'compat warning' },
  null,
  'malformed',
  [],
];

test('dashboard feed normalizes alert severity and isolates malformed records', async () => {
  await withPage([], async (page, { port }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));

    await page.evaluate(async () => {
      const html = await fetch('/ui/index.html').then(response => response.text());
      const parsed = new DOMParser().parseFromString(html, 'text/html');
      document.body.innerHTML = parsed.body.innerHTML;
    });
    for (const moduleFile of MODULES) {
      await page.addScriptTag({ url: `http://127.0.0.1:${port}/${moduleFile}` });
    }

    const result = await page.evaluate(async (alerts) => {
                                                     
      window._shellSlow.raw = { alerts, sec: [] };
      const host = document.createElement('div');
      document.body.appendChild(host);
      await window.renderDashboard(host);
      const rows = Array.from(host.querySelectorAll('#dash-feed .dash2-feed-item'));
      return {
        count: rows.length,
        classes: rows.map(row => row.className),
        severities: rows.map(row => row.querySelector('.dash2-feed-sev').textContent),
        texts: rows.map(row => row.children[1].textContent),
        header: host.querySelector('#dash-feed h4').textContent,
        errorText: host.querySelector(':scope > p.color-red')?.textContent || '',
      };
    }, ALERTS);

    assert.deepEqual(pageErrors, []);
    assert.equal(result.errorText, '');
                                                     
    assert.ok(result.count > 0, 'feed rendered no rows');
                                                            
    assert.equal(result.count, 2);
    assert.deepEqual(result.classes, [
      'dash2-feed-item f-crit',
      'dash2-feed-item f-warn',
    ]);
    assert.deepEqual(result.severities, ['CRIT', 'WARN']);
    assert.deepEqual(result.texts, ['canonical critical', 'compat warning']);
                                                  
                                                                    
                       
    assert.match(result.header, /\(총 2 · 2 unack\)/, result.header);
  }, {
    routes: {
      '/api/v1/vms': { body: [] },
      '/api/v1/containers': { body: [] },
      '/api/v1/health': {
        body: {
          node_name: 'test-node',
          capabilities: { cluster: false },
          checks: {},
        },
      },
    },
  });
});
