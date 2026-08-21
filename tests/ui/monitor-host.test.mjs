                                                                                        
                                                                                  
                                                                 
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const MODS = [
  'ui/modules/ui.js',
  'ui/modules/uxlib.js',
  'ui/modules/endpoints.js',
  'ui/modules/monitor.js'
];

const METRICS = [
  'purecvisor_host_cpu_percent 62.9',
  'purecvisor_host_memory_percent 39.8',
  'purecvisor_host_disk_percent 87.9',
  'purecvisor_host_cpu_temp_celsius 27.8',
  'purecvisor_host_load1 36.46'
].join('\n') + '\n';

function routes(metrics = METRICS, dpdk = false, sriov = false) {
  return {
    '/api/v1/metrics': { status: 200, body: metrics },
    '/api/v1/dpdk/status': { status: 200, body: { data: { available: dpdk } } },
    '/api/v1/sriov/status': { status: 200, body: { data: { available: sriov } } }
  };
}

async function boot(page, metrics = METRICS, metricsStatus = 200) {
  await page.evaluate(({ metrics: metricText, metricsStatus: status }) => {
    const nativeFetch = window.fetch.bind(window);
    window.fetch = (url, options) => {
      if (String(url).endsWith('/api/v1/metrics')) {
        return Promise.resolve(new Response(
          status === 200 ? metricText : JSON.stringify({ error: { message: 'metrics unavailable' } }),
          { status, headers: { 'content-type': status === 200 ? 'text/plain' : 'application/json' } }
        ));
      }
      return nativeFetch(url, options);
    };
    window._DEBUG = false;
    window.authToken = 'test-token';
    window._L = text => text;
    window.t = key => key;
    window.unwrapData = value => value && value.data !== undefined ? value.data : value;
    window.fetchGet = url => fetch(url).then(async response => {
      const body = await response.json();
      if (!response.ok) throw new Error(body.error?.message || `HTTP ${response.status}`);
      return body;
    });
  }, { metrics, metricsStatus });
  await page.evaluate(() => PCV.monitor.renderHost(document.getElementById('cb')));
}

function statusPillClasses(result) {
  return result.map(node => node.className).sort();
}

test('renderHost replaces progress bars with thresholded inline gauges and status pills', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    const result = await page.evaluate(() => ({
      gaugeLevels: [...document.querySelectorAll('.host-ops-grid .gauge-inline')]
        .map(node => [...node.classList].find(name => /^g-(ok|warn|crit)$/.test(name))),
      progressBars: document.querySelectorAll('.host-ops-grid .pb').length,
      percentCounts: [...document.querySelectorAll('.host-ops-grid .stat-md')]
        .map(node => node.textContent.trim()),
      pills: [...document.querySelectorAll('.host-ops-grid .pill')]
        .map(node => ({ className: node.className, text: node.textContent.trim() }))
    }));
    assert.deepEqual(result.gaugeLevels, ['g-ok', 'g-ok', 'g-warn']);
    assert.equal(result.progressBars, 0);
    assert.deepEqual(result.percentCounts, ['62.9%', '39.8%', '87.9%']);
    assert.deepEqual(statusPillClasses(result.pills), [
      'pill pill-idle',
      'pill pill-idle',
      'pill pill-ok',
      'pill pill-ok'
    ]);
    assert.deepEqual(result.pills.map(pill => pill.text).sort(), ['OFF', 'OFF', '안정', '단일 노드'].sort());
  }, { routes: routes() });
});

test('renderHost maps exact warn and crit boundaries and available accelerators', async () => {
  const boundaryMetrics = [
    'purecvisor_host_cpu_percent 80',
    'purecvisor_host_memory_percent 95',
    'purecvisor_host_disk_percent 90',
    'purecvisor_host_cpu_temp_celsius 70',
    'purecvisor_host_load1 1'
  ].join('\n') + '\n';
  await withPage(MODS, async page => {
    await boot(page, boundaryMetrics);
    const result = await page.evaluate(() => ({
      gaugeLevels: [...document.querySelectorAll('.host-ops-grid .gauge-inline')]
        .map(node => [...node.classList].find(name => /^g-(ok|warn|crit)$/.test(name))),
      pills: [...document.querySelectorAll('.host-ops-grid .pill')]
        .map(node => ({ className: node.className, text: node.textContent.trim() }))
    }));
    assert.deepEqual(result.gaugeLevels, ['g-warn', 'g-crit', 'g-crit']);
    assert.ok(result.pills.some(pill => pill.className === 'pill pill-warn' && pill.text === '주의'));
    assert.equal(result.pills.filter(pill => pill.className === 'pill pill-ok' && pill.text === 'ON').length, 2);
    assert.ok(result.pills.some(pill => pill.className === 'pill pill-ok' && pill.text === '단일 노드'));
  }, { routes: routes('', true, true) });
});

test('renderHost replaces a metrics failure skeleton with an explicit retryable error', async () => {
  await withPage(MODS, async page => {
    await page.evaluate(() => {
      window.__pageErrors = [];
      window.addEventListener('error', event => window.__pageErrors.push(String(event.message)));
    });
    await boot(page, '', 503);
    const result = await page.evaluate(() => ({
      skeletons: document.querySelectorAll('#cb .skeleton').length,
      title: document.querySelector('#cb .pagehead-title')?.textContent,
      alert: document.querySelector('#cb [role="alert"]')?.textContent,
      retry: document.querySelector('#cb [role="alert"] button')?.textContent,
      pageErrors: window.__pageErrors
    }));
    assert.equal(result.skeletons, 0);
    assert.equal(result.title, '호스트 상태');
    assert.match(result.alert, /상태 로드 실패/);
    assert.equal(result.retry, '다시 시도');
    assert.deepEqual(result.pageErrors, []);
  }, { routes: routes() });
});
