




import { test } from 'node:test';
import assert from 'node:assert/strict';
import axe from 'axe-core';
import { withPage } from './harness.mjs';

const MODULES = [
  'ui/modules/ui.js',
  'ui/modules/uxlib.js',
  'ui/modules/endpoints.js',
  'ui/modules/network.js'
];

async function boot(page) {
  await page.setViewport({ width: 1280, height: 900 });
  await page.evaluate(() => {
    window._DEBUG = true;
    window.authToken = 'topology-map-test';
    window.currentTab = 'topology';
    window._L = (ko, en) => en;
    window.t = key => key;
    window.API_BASE = '/api/v1';
    window.MON_NODES = [{ name: 'edge-01', ip: '192.0.2.20' }];
    window.unwrapData = value => value && value.data !== undefined ? value.data : value;
    window.unwrapList = value => Array.isArray(value) ? value
      : (Array.isArray(window.unwrapData(value)) ? window.unwrapData(value) : []);
    window.fetchGet = url => fetch(url).then(async response => {
      const body = await response.json().catch(() => null);
      if (!response.ok)
        throw new Error((body && body.error && body.error.message) || `HTTP ${response.status}`);
      return body;
    });
  });
  await page.evaluate(() => window.renderTopology(document.getElementById('cb')));
  await page.waitForSelector('.topology-map');
}

function baseRoutes(vms) {
  return {
    '/api/v1/networks': { status: 200, body: { data: [
      { name: 'pcvbr0', mode: 'nat', state: 'up', ip_cidr: '10.78.0.1/24' },
      { name: 'edge-uplink', mode: 'bridge', state: 'down', ip_cidr: '192.0.2.20/24' }
    ] } },
    '/api/v1/vms': { status: 200, body: { data: vms } }
  };
}

test('topology map groups VMs by actual NIC source and keeps unmapped workloads visible', async () => {
  const vms = [
    { name: 'api-prod', state: 'running' },
    { name: 'db-core', state: 'shutoff' },
    { name: 'orphan', state: 'running' }
  ];
  const routes = {
    ...baseRoutes(vms),
    '/api/v1/vms/api-prod/nics': { status: 200, body: { data: [
      { bridge: 'pcvbr0', ip: '10.78.0.31', mac: '52:54:00:00:00:01' },
      { source: 'edge-uplink', mac: '52:54:00:00:00:02' }
    ] } },
    '/api/v1/vms/db-core/nics': { status: 200, body: { data: [] } },
    '/api/v1/vms/orphan/nics': { status: 200, body: { data: [
      { network: 'legacy-br', mac: '52:54:00:00:00:03' }
    ] } }
  };

  await withPage(MODULES, async page => {
    await boot(page);

    assert.match(await page.$eval('.pagehead-title', node => node.textContent), /Single Edge architecture map/);
    assert.equal(await page.$$('canvas').then(nodes => nodes.length), 0);
    const mapped = await page.$$eval('.topology-network-card', cards => cards.map(card => ({
      network: card.dataset.network,
      vms: Array.from(card.querySelectorAll('.topology-workload-name'), node => node.textContent)
    })));
    assert.deepEqual(mapped, [
      { network: 'pcvbr0', vms: ['api-prod'] },
      { network: 'edge-uplink', vms: ['api-prod'] }
    ]);

    const unmapped = await page.$$eval('.topology-unmapped-row', rows => rows.map(row => row.textContent));
    assert.equal(unmapped.length, 2);
    assert.match(unmapped.join('\n'), /db-core.*No NICs attached/s);
    assert.match(unmapped.join('\n'), /orphan.*legacy-br/s);

    const summary = await page.$$eval('.topology-summary-item', items => Object.fromEntries(
      items.map(item => [item.dataset.summary, item.querySelector('.topology-summary-value').textContent])
    ));
    assert.deepEqual(summary, { host: '1', networks: '2', running: '2 / 3', links: '3' });
    assert.doesNotMatch(await page.$eval('#cb', node => node.textContent), /🖥|🌐|💻/);
  }, { routes });
});

test('topology map isolates one NIC failure and exposes partial coverage', async () => {
  const vms = [
    { name: 'healthy-vm', state: 'running' },
    { name: 'unreadable-vm', state: 'running' }
  ];
  const routes = {
    ...baseRoutes(vms),
    '/api/v1/vms/healthy-vm/nics': { status: 200, body: { data: [{ bridge: 'pcvbr0' }] } },
    '/api/v1/vms/unreadable-vm/nics': { status: 503, body: { error: { message: 'NIC inventory unavailable' } } }
  };

  await withPage(MODULES, async page => {
    await boot(page);
    assert.match(await page.$eval('.topology-partial', node => node.textContent), /PARTIAL/);
    assert.match(await page.$eval('.topology-partial', node => node.textContent), /1 \/ 2/);
    assert.equal(await page.$eval('[data-network="pcvbr0"] .topology-workload-name', node => node.textContent), 'healthy-vm');
    assert.match(await page.$eval('.topology-unmapped', node => node.textContent), /unreadable-vm/);
    assert.match(await page.$eval('.topology-unmapped', node => node.textContent), /NIC inventory unavailable/);
  }, { routes });
});

test('topology map renders explicit empty and core error states', async () => {
  await withPage(MODULES, async page => {
    await boot(page);
    const text = await page.$eval('.topology-map', node => node.textContent);
    assert.match(text, /No managed networks/);
    assert.match(text, /No workloads discovered/);
  }, { routes: {
    '/api/v1/networks': { status: 200, body: { data: [] } },
    '/api/v1/vms': { status: 200, body: { data: [] } }
  } });

  await withPage(MODULES, async page => {
    await boot(page);
    const error = await page.$eval('.topology-error', node => ({
      role: node.getAttribute('role'),
      text: node.textContent
    }));
    assert.equal(error.role, 'alert');
    assert.match(error.text, /Unable to load architecture data/);
  }, { routes: {
    '/api/v1/networks': { status: 503, body: { error: { message: 'network list unavailable' } } },
    '/api/v1/vms': { status: 503, body: { error: { message: 'VM list unavailable' } } }
  } });
});

test('topology map caps expansion at 24 workloads and bounds NIC concurrency at six', async () => {
  const vms = Array.from({ length: 25 }, (_, index) => ({
    name: `vm-${String(index + 1).padStart(2, '0')}`,
    state: index % 2 ? 'shutoff' : 'running'
  }));
  let active = 0;
  let maxActive = 0;
  const routes = {
    '/api/v1/networks': { status: 200, body: { data: [] } },
    '/api/v1/vms': { status: 200, body: { data: vms } }
  };
  for (const vm of vms.slice(0, 24)) {
    routes[`/api/v1/vms/${vm.name}/nics`] = async () => {
      active += 1;
      maxActive = Math.max(maxActive, active);
      await new Promise(resolve => setTimeout(resolve, 8));
      active -= 1;
      return { status: 200, body: { data: [] } };
    };
  }

  await withPage(MODULES, async (page, { requests }) => {
    await boot(page);
    assert.match(await page.$eval('.topology-overflow', node => node.textContent), /\+1 more workload/);
    assert.equal(requests.filter(request => request.path.endsWith('/nics')).length, 24);
    assert.ok(maxActive <= 6, `expected <= 6 concurrent NIC requests, observed ${maxActive}`);
    assert.equal(await page.$eval('[data-summary="running"] .topology-summary-value', node => node.textContent), '13 / 25');
  }, { routes });
});

test('topology map keeps 3/2/1-column hierarchy accessible without viewport overflow', async () => {
  const vms = [
    { name: 'api-prod', state: 'running' },
    { name: 'db-core', state: 'shutoff' }
  ];
  const routes = {
    ...baseRoutes(vms),
    '/api/v1/vms/api-prod/nics': { status: 200, body: { data: [{ bridge: 'pcvbr0' }] } },
    '/api/v1/vms/db-core/nics': { status: 200, body: { data: [{ bridge: 'edge-uplink' }] } }
  };

  await withPage(MODULES, async page => {
    await boot(page);
    const layouts = [];
    for (const width of [1440, 1024, 768, 480]) {
      await page.setViewport({ width, height: 900 });
      layouts.push(await page.evaluate(() => ({
        width: innerWidth,
        overflow: document.documentElement.scrollWidth - document.documentElement.clientWidth,
        columns: getComputedStyle(document.querySelector('.topology-network-grid')).gridTemplateColumns.split(' ').length
      })));
    }
    assert.deepEqual(layouts.map(item => item.columns), [3, 2, 1, 1]);
    assert.ok(layouts.every(item => item.overflow <= 1), JSON.stringify(layouts));

    await page.evaluate(axe.source);
    const violations = await page.evaluate(async () => {
      const result = await window.axe.run(document.querySelector('.topology-map'));
      return result.violations.map(item => item.id);
    });
    assert.deepEqual(violations, []);
  }, { routes });
});
