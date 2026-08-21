                                                                                        
                                                                   
                                                                     
  
                                         
  
                                                                             
                                                                               
                                                                               
                             
  
                                                         
                                                                            
                                                  
                                                            
   
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { withPage, CORE } from './harness.mjs';

const NAV_SOURCE = readFileSync(new URL('../../ui/modules/nav.js', import.meta.url), 'utf8');
const VM_ROUTES = ['summary', 'console', 'snapshots', 'performance', 'timeline'];

const BASE_MODULES = [...CORE, 'ui/modules/endpoints.js'];
const SURFACE_MODULES = {
  vm: [...BASE_MODULES, 'ui/modules/vm.js'],
  container: [...BASE_MODULES, 'ui/modules/container.js'],
  network: [...BASE_MODULES, 'ui/modules/filter-state.js', 'ui/modules/network.js'],
  storage: [...BASE_MODULES, 'ui/modules/filter-state.js', 'ui/modules/storage.js']
};

const ROLE_ROUTES = {
  vm: {
    '/api/v1/vms/vm-d1': { status: 200, body: { data: { cpu: 18, mem: 32, vcpu: 2, memory_mb: 2048 } } },
    '/api/v1/vms/vm-d1/nics': { status: 200, body: { data: [] } },
    '/api/v1/networks': { status: 200, body: { data: [] } }
  },
  container: {
    '/api/v1/containers': {
      status: 200,
      body: { data: [{ name: 'ctr-d1', state: 'STOPPED', ip_addr: '' }] }
    }
  },
  network: {
    '/api/v1/networks': {
      status: 200,
      body: { data: [{
        name: 'pcvd1', mode: 'nat', state: 'up', ip_cidr: '10.78.0.1/24',
        subnet: '10.78.0.0/24', dhcp: true, vm_count: 1
      }] }
    }
  },
  storage: {
    '/api/v1/storage/pools': {
      status: 200,
      body: { data: [{ name: 'tank', health: 'ONLINE', size: '100G', alloc: '30G' }] }
    },
    '/api/v1/storage/zvols': {
      status: 200,
      body: { data: [{ name: 'tank/vm-d1', volsize: '20G', used: '4G' }] }
    },
    '/api/v1/rpc': { status: 200, body: { result: [] } }
  }
};

const ROLE_EXPECTED_MINIMUM = {
  vm: 10,
  container: 4,
  network: 4,
  storage: 5
};

async function seedProductGlobals(page, tab) {
  await page.setViewport({ width: 1280, height: 900 });
  await page.evaluate(activeTab => {
    window._DEBUG = false;
    window.API_BASE = '/api/v1';
    window.authToken = 'd1-test-token';
    window.currentTab = activeTab;
    window._L = (ko, en) => en || ko;
    window.t = key => key;
    window.unwrapData = value => value == null ? value
      : (value.data !== undefined ? value.data
        : (value.result !== undefined ? value.result : value));
    window.unwrapList = value => {
      const data = window.unwrapData(value);
      return Array.isArray(data) ? data
        : (Array.isArray(data?.items) ? data.items
          : (Array.isArray(data?.list) ? data.list : []));
    };
    window.fetchGet = async url => {
      const response = await fetch(url);
      const body = await response.json().catch(() => null);
      if (!response.ok) {
        return body || {
          error: { code: response.status, message: `HTTP ${response.status}` }
        };
      }
      return body;
    };
    window.fetchPost = (url, body) => fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body)
    }).then(response => response.json());
    window.fmtBytes = value => String(value);
    window.selectedVmIndex = 0;
  }, tab);
}

                                                       
                                                    
async function renderMutationSurface(page, surface, role) {
  return page.evaluate(async ({ surfaceName, effectiveRole }) => {
    window.currentUser = effectiveRole ? { role: effectiveRole } : null;
    const target = document.getElementById('cb');
    if (surfaceName === 'vm') {
      await window.renderSummary(target, {
        name: 'vm-d1', state: 'running', live_cpu_pct: 12, mem_percent: 25,
        vcpu: 2, memory_mb: 2048, storage_type: 'zvol'
      });
    } else if (surfaceName === 'container') {
      window.selCtr = 'ctr-d1';
      window.ctrTab = 'summary';
      await window.renderContainers(target);
    } else if (surfaceName === 'network') {
      await window.renderNetworks(target);
    } else if (surfaceName === 'storage') {
      window._zvolSel = new Set();
      await window.renderStorage(target);
    } else {
      throw new Error('unknown D1 surface: ' + surfaceName);
    }

    const commandPattern = {
      vm: /^(?:vmPower|showSnap|showSettings|showRenameVm|showNicMgr|vmClone|vmExportOva|showImportOva|showDiskLiveResize|showBlkioEditor)\b/,
      container: /^(?:showCtrCreate|ctrA|ctrReboot|ctrDel)\b/,
      network: /^(?:showNetCreate|showNetEdit|netDel|fwAddRule)\b/,
      storage: /^(?:showPoolCreate|poolScrub|poolDestroy|showZvol|zvolDel)\b/
    }[surfaceName];
    const buttons = [...target.querySelectorAll('button')].filter(button =>
      commandPattern.test(button.getAttribute('onclick') || ''));
    return buttons.map(button => {
      const gate = button.closest('[data-role]');
      return {
        command: button.getAttribute('onclick'),
        dataRole: gate?.getAttribute('data-role') || null,
        roleHidden: gate?.classList.contains('role-hidden') || false,
        display: gate ? getComputedStyle(gate).display : null
      };
    });
  }, { surfaceName: surface, effectiveRole: role });
}

function assertMutationVisibility(surface, role, nodes, visible) {
  assert.ok(
    nodes.length >= ROLE_EXPECTED_MINIMUM[surface],
    `${surface}/${role || 'unknown'} representative mutation count=${nodes.length}`
  );
  for (const node of nodes) {
    assert.ok(node.dataRole, `${surface}/${role || 'unknown'} lacks closest [data-role]: ${node.command}`);
    if (visible) {
      assert.equal(node.roleHidden, false, `${surface}/ADMIN role-hidden: ${node.command}`);
      assert.notEqual(node.display, 'none', `${surface}/ADMIN hidden: ${node.command}`);
    } else {
      assert.equal(node.roleHidden, true, `${surface}/${role || 'unknown'} not role-hidden: ${node.command}`);
      assert.equal(node.display, 'none', `${surface}/${role || 'unknown'} visible: ${node.command}`);
    }
  }
}

function dispatcherRoutes() {
  const start = NAV_SOURCE.indexOf('var routes = {');
  const end = NAV_SOURCE.indexOf('return routes;', start);
  assert.ok(start >= 0 && end > start, 'nav dispatcher route table must exist');
  return [...NAV_SOURCE.slice(start, end).matchAll(/^\s{6}['"]?([a-z0-9-]+)['"]?\s*:/gm)]
    .map(match => match[1]);
}

async function shellRoutes(page) {
  return page.evaluate(vmRoutes => {
    const items = PCV.shell.NAV_SECTIONS().flatMap(section => section.items.map(item => item.id));
    return items.flatMap(id => id === 'vm' ? vmRoutes : [id]);
  }, VM_ROUTES);
}

test('current shell and dispatcher expose the same 40 canonical desktop routes', async () => {
  await withPage([...CORE, 'ui/modules/shell.js'], async page => {
    await page.evaluate(() => { window._L = (ko, en) => en || ko; });
    const shell = await shellRoutes(page);
    const dispatcher = dispatcherRoutes();
    assert.equal(shell.length, 40);
    assert.equal(new Set(shell).size, 40);
    assert.deepEqual([...shell].sort(), [...dispatcher].sort());
    assert.equal(shell.includes('serviceguide'), false, 'retired service guide must not remain canonical');
    assert.equal(shell.includes('restguide'), false, 'retired REST guide must not remain canonical');
    assert.ok(shell.includes('vpcs'), 'Local VPC must remain in the inventory');
    assert.ok(shell.includes('pool-info'), 'Connection Pool must remain in the inventory');
  });
});

for (const surface of ['vm', 'container', 'network', 'storage']) {
  test(`${surface} async mutations are fail-closed for VIEWER/unknown and visible for ADMIN`, async () => {
    await withPage(SURFACE_MODULES[surface], async page => {
      await seedProductGlobals(page, surface);

                                                                        
                                               
      const unknown = await renderMutationSurface(page, surface, null);
      assertMutationVisibility(surface, null, unknown, false);

      const viewer = await renderMutationSurface(page, surface, 'VIEWER');
      assertMutationVisibility(surface, 'VIEWER', viewer, false);

      const admin = await renderMutationSurface(page, surface, 'ADMIN');
      assertMutationVisibility(surface, 'ADMIN', admin, true);
    }, { routes: ROLE_ROUTES[surface] });
  });
}

const LOAD_ERROR_CASES = [
  {
    name: 'container',
    modules: SURFACE_MODULES.container,
    tab: 'containers',
    renderer: 'renderContainers',
    path: '/api/v1/containers',
    success: { data: [] },
    successSelector: '#cb .empty-state'
  },
  {
    name: 'network',
    modules: SURFACE_MODULES.network,
    tab: 'networks',
    renderer: 'renderNetworks',
    path: '/api/v1/networks',
    success: { data: [] },
    successSelector: '#cb .empty-state'
  },
  {
    name: 'storage',
    modules: SURFACE_MODULES.storage,
    tab: 'storage',
    renderer: 'renderStorage',
    path: '/api/v1/storage/pools',
    success: { data: [] },
    successSelector: '#cb .empty-state',
    extraRoutes: {
      '/api/v1/storage/zvols': { status: 200, body: { data: [] } },
      '/api/v1/rpc': { status: 200, body: { result: [] } }
    }
  },
  {
    name: 'accounts',
    modules: [...BASE_MODULES, 'ui/modules/accounts.js'],
    tab: 'accounts',
    renderer: 'renderAccounts',
    path: '/api/v1/auth/users',
    success: { data: [] },
    successSelector: '#cb #acct-table table'
  }
];

for (const errorCase of LOAD_ERROR_CASES) {
  test(`${errorCase.name} GET failure preserves pagehead and Retry replaces alert with empty/normal state`, async () => {
    let attempts = 0;
    const outageMessage = `${errorCase.name} D1 fixture outage`;
    const routes = {
      ...(errorCase.extraRoutes || {}),
      [errorCase.path]: () => {
        attempts += 1;
        return attempts === 1
          ? { status: 503, body: { error: { code: 'TEMPORARY', message: outageMessage } } }
          : { status: 200, body: errorCase.success };
      }
    };

    await withPage(errorCase.modules, async (page, context) => {
      await seedProductGlobals(page, errorCase.tab);
      await page.addScriptTag({ url: '/ui/modules/api.js' });
      assert.equal(
        await page.evaluate(() => window.fetchGet === window.PCV.api.fetchGet),
        true,
        `${errorCase.name} load-error contract must exercise the production fetchGet`
      );
      await page.evaluate(() => { window.currentUser = { role: 'ADMIN' }; });
      await page.evaluate(renderer => window[renderer](document.getElementById('cb')), errorCase.renderer);

      const failed = await page.evaluate(() => {
        const alert = document.querySelector('#cb [role="alert"]');
        const retry = alert && [...alert.querySelectorAll('button')]
          .find(button => /retry|다시\s*시도|재시도/i.test(button.textContent));
        return {
          pageheads: document.querySelectorAll('#cb .pagehead').length,
          title: document.querySelector('#cb .pagehead-title')?.textContent.trim() || '',
          text: alert?.textContent || '',
          retry: Boolean(retry),
          skeletons: document.querySelectorAll('#cb .skeleton').length
        };
      });
      assert.equal(failed.pageheads, 1, 'error state must preserve one canonical pagehead');
      assert.ok(failed.title, 'error pagehead title must remain readable');
      assert.match(failed.text, new RegExp(outageMessage));
      assert.equal(failed.retry, true, 'error state must expose a native Retry button');
      assert.equal(failed.skeletons, 0, 'failed GET must not leave an indefinite skeleton');

      await page.click('#cb [role="alert"] button');
      await page.waitForSelector(errorCase.successSelector, { timeout: 3000 });

      const recovered = await page.evaluate(() => ({
        alert: Boolean(document.querySelector('#cb [role="alert"]')),
        pageheads: document.querySelectorAll('#cb .pagehead').length,
        skeletons: document.querySelectorAll('#cb .skeleton').length
      }));
      assert.deepEqual(recovered, { alert: false, pageheads: 1, skeletons: 0 });
      assert.equal(
        context.requests.filter(request => request.path === errorCase.path).length,
        2,
        'Retry must issue exactly one fresh GET and must not replay a mutation'
      );
    }, { routes });
  });
}

test('storage zvol GET failure preserves pagehead and Retry replaces alert with empty state', async () => {
  let zvolAttempts = 0;
  const outageMessage = 'storage zvol D1 fixture outage';
  const routes = {
    '/api/v1/storage/pools': { status: 200, body: { data: [] } },
    '/api/v1/storage/zvols': () => {
      zvolAttempts += 1;
      return zvolAttempts === 1
        ? { status: 503, body: { error: { code: 'TEMPORARY', message: outageMessage } } }
        : { status: 200, body: { data: [] } };
    },
    '/api/v1/rpc': { status: 200, body: { result: [] } }
  };

  await withPage(SURFACE_MODULES.storage, async (page, context) => {
    await seedProductGlobals(page, 'storage');
    await page.addScriptTag({ url: '/ui/modules/api.js' });
    assert.equal(
      await page.evaluate(() => window.fetchGet === window.PCV.api.fetchGet),
      true,
      'storage zvol load-error contract must exercise the production fetchGet'
    );
    await page.evaluate(() => { window.currentUser = { role: 'ADMIN' }; });
    await page.evaluate(() => window.renderStorage(document.getElementById('cb')));

    const failed = await page.evaluate(() => {
      const alert = document.querySelector('#cb [role="alert"]');
      const retry = alert && [...alert.querySelectorAll('button')]
        .find(button => /retry|다시\s*시도|재시도/i.test(button.textContent));
      return {
        pageheads: document.querySelectorAll('#cb .pagehead').length,
        title: document.querySelector('#cb .pagehead-title')?.textContent.trim() || '',
        text: alert?.textContent || '',
        retry: Boolean(retry),
        skeletons: document.querySelectorAll('#cb .skeleton').length
      };
    });
    assert.equal(failed.pageheads, 1, 'zvol error state must preserve one canonical pagehead');
    assert.ok(failed.title, 'zvol error pagehead title must remain readable');
    assert.match(failed.text, new RegExp(outageMessage));
    assert.equal(failed.retry, true, 'zvol error state must expose a native Retry button');
    assert.equal(failed.skeletons, 0, 'failed zvol GET must not leave an indefinite skeleton');

    await page.click('#cb [role="alert"] button');
    await page.waitForSelector('#cb .empty-state', { timeout: 3000 });

    assert.deepEqual(await page.evaluate(() => ({
      alert: Boolean(document.querySelector('#cb [role="alert"]')),
      pageheads: document.querySelectorAll('#cb .pagehead').length,
      skeletons: document.querySelectorAll('#cb .skeleton').length
    })), { alert: false, pageheads: 1, skeletons: 0 });
    assert.equal(
      context.requests.filter(request => request.path === '/api/v1/storage/zvols').length,
      2,
      'Retry must issue exactly one fresh zvol GET'
    );
  }, { routes });
});

test('host metrics failure renders pagehead + alert + Retry and recovers without a skeleton', async () => {
  const modules = [...BASE_MODULES, 'ui/modules/monitor.js'];
  const metrics = [
    'purecvisor_host_cpu_percent 31.5',
    'purecvisor_host_memory_percent 42.5',
    'purecvisor_host_disk_percent 53.5',
    'purecvisor_host_cpu_temp_celsius 44',
    'purecvisor_host_load1 0.75'
  ].join('\n') + '\n';
  const routes = {
    '/api/v1/dpdk/status': { status: 200, body: { data: { available: false } } },
    '/api/v1/sriov/status': { status: 200, body: { data: { available: false } } }
  };

  await withPage(modules, async page => {
    await seedProductGlobals(page, 'host');
    await page.evaluate(metricText => {
      const nativeFetch = window.fetch.bind(window);
      window.__d1HostMetricCalls = 0;
      window.fetch = (url, options) => {
        if (String(url).endsWith('/api/v1/metrics')) {
          window.__d1HostMetricCalls += 1;
          return Promise.resolve(window.__d1HostMetricCalls === 1
            ? new Response(JSON.stringify({ error: { message: 'host metrics D1 outage' } }), {
                status: 503, headers: { 'content-type': 'application/json' }
              })
            : new Response(metricText, {
                status: 200, headers: { 'content-type': 'text/plain' }
              }));
        }
        return nativeFetch(url, options);
      };
    }, metrics);

    await page.evaluate(() => window.renderHost(document.getElementById('cb')));
    const failed = await page.evaluate(() => ({
      title: document.querySelector('#cb .pagehead-title')?.textContent.trim() || '',
      alert: document.querySelector('#cb [role="alert"]')?.textContent || '',
      retry: Boolean(document.querySelector('#cb [role="alert"] button')),
      skeletons: document.querySelectorAll('#cb .skeleton').length
    }));
    assert.ok(failed.title);
    assert.match(failed.alert, /metrics HTTP 503/i);
    assert.equal(failed.retry, true);
    assert.equal(failed.skeletons, 0);

    await page.click('#cb [role="alert"] button');
    await page.waitForSelector('#cb .host-ops-grid', { timeout: 3000 });
    assert.deepEqual(await page.evaluate(() => ({
      calls: window.__d1HostMetricCalls,
      alert: Boolean(document.querySelector('#cb [role="alert"]')),
      skeletons: document.querySelectorAll('#cb .skeleton').length,
      pageheads: document.querySelectorAll('#cb .pagehead').length
    })), { calls: 2, alert: false, skeletons: 0, pageheads: 1 });
  }, { routes });
});

const HOST_ACCELERATOR_ERROR_CASES = [
  {
    name: 'DPDK status',
    path: '/api/v1/dpdk/status',
    expectedRequests: { dpdk: 2, sriov: 1 },
    expectedStates: { dpdk: 'ON', sriov: 'OFF' }
  },
  {
    name: 'SR-IOV status',
    path: '/api/v1/sriov/status',
    expectedRequests: { dpdk: 2, sriov: 2 },
    expectedStates: { dpdk: 'OFF', sriov: 'ON' }
  }
];

for (const errorCase of HOST_ACCELERATOR_ERROR_CASES) {
  test(`host ${errorCase.name} HTTP failure renders Retry and recovers with live accelerator state`, async () => {
    let attempts = 0;
    const outageMessage = `host ${errorCase.name} D1 fixture outage`;
    const statusRoute = path => path === errorCase.path
      ? () => {
          attempts += 1;
          return attempts === 1
            ? { status: 503, body: { error: { code: 'TEMPORARY', message: outageMessage } } }
            : { status: 200, body: { data: { available: true } } };
        }
      : { status: 200, body: { data: { available: false } } };
    const routes = {
      '/api/v1/dpdk/status': statusRoute('/api/v1/dpdk/status'),
      '/api/v1/sriov/status': statusRoute('/api/v1/sriov/status')
    };
    const metrics = [
      'purecvisor_host_cpu_percent 31.5',
      'purecvisor_host_memory_percent 42.5',
      'purecvisor_host_disk_percent 53.5',
      'purecvisor_host_cpu_temp_celsius 44',
      'purecvisor_host_load1 0.75'
    ].join('\n') + '\n';

    await withPage([...BASE_MODULES, 'ui/modules/monitor.js'], async (page, context) => {
      await seedProductGlobals(page, 'host');
      await page.evaluate(metricText => {
        const nativeFetch = window.fetch.bind(window);
        window.__d1HostMetricCalls = 0;
        window.fetch = (url, options) => {
          if (String(url).endsWith('/api/v1/metrics')) {
            window.__d1HostMetricCalls += 1;
            return Promise.resolve(new Response(metricText, {
              status: 200, headers: { 'content-type': 'text/plain' }
            }));
          }
          return nativeFetch(url, options);
        };
      }, metrics);
      await page.addScriptTag({ url: '/ui/modules/api.js' });
      assert.equal(
        await page.evaluate(() => window.fetchGet === window.PCV.api.fetchGet),
        true,
        `${errorCase.name} host contract must exercise the production fetchGet`
      );

      await page.evaluate(() => window.renderHost(document.getElementById('cb')));
      const failed = await page.evaluate(() => {
        const alert = document.querySelector('#cb [role="alert"]');
        const retry = alert && [...alert.querySelectorAll('button')]
          .find(button => /retry|다시\s*시도|재시도/i.test(button.textContent));
        return {
          pageheads: document.querySelectorAll('#cb .pagehead').length,
          title: document.querySelector('#cb .pagehead-title')?.textContent.trim() || '',
          text: alert?.textContent || '',
          retry: Boolean(retry),
          skeletons: document.querySelectorAll('#cb .skeleton').length,
          grid: Boolean(document.querySelector('#cb .host-ops-grid'))
        };
      });
      assert.equal(failed.pageheads, 1, 'accelerator error must preserve one host pagehead');
      assert.ok(failed.title, 'accelerator error must preserve the host title');
      assert.match(failed.text, new RegExp(outageMessage));
      assert.equal(failed.retry, true, 'accelerator error must expose a native Retry button');
      assert.equal(failed.skeletons, 0, 'accelerator error must replace the loading skeleton');
      assert.equal(failed.grid, false, 'failed accelerator state must not look healthy');

      await page.click('#cb [role="alert"] button');
      await page.waitForSelector('#cb .host-ops-grid', { timeout: 3000 });

      const recovered = await page.evaluate(() => ({
        alert: Boolean(document.querySelector('#cb [role="alert"]')),
        pageheads: document.querySelectorAll('#cb .pagehead').length,
        skeletons: document.querySelectorAll('#cb .skeleton').length,
        metricsCalls: window.__d1HostMetricCalls,
        gridText: document.querySelector('#cb .host-ops-grid')?.textContent || ''
      }));
      assert.equal(recovered.alert, false);
      assert.equal(recovered.pageheads, 1);
      assert.equal(recovered.skeletons, 0);
      assert.equal(recovered.metricsCalls, 2, 'Retry must re-read host metrics exactly once');
      assert.match(recovered.gridText, new RegExp(`DPDK\\s*${errorCase.expectedStates.dpdk}`));
      assert.match(recovered.gridText, new RegExp(`SR-IOV\\s*${errorCase.expectedStates.sriov}`));
      assert.equal(
        context.requests.filter(request =>
          request.method === 'GET' && request.path === '/api/v1/dpdk/status').length,
        errorCase.expectedRequests.dpdk,
        `${errorCase.name} flow must make the expected DPDK GET count`
      );
      assert.equal(
        context.requests.filter(request =>
          request.method === 'GET' && request.path === '/api/v1/sriov/status').length,
        errorCase.expectedRequests.sriov,
        `${errorCase.name} flow must make the expected SR-IOV GET count`
      );
    }, { routes });
  });
}

test('monitoring overview stays inside #cb at 1024px with the complete desktop shell', async () => {
  const modules = [
    ...BASE_MODULES,
    'ui/modules/filter-state.js',
    'ui/modules/shell.js',
    'ui/modules/monitor.js'
  ];
  const node = {
    node: 'edge-d1', ip: '192.0.2.10', cpu: 45, mem: 52, disk: 61, temp: 43,
    load1: 0.5, load5: 0.4, load15: 0.3, ram_total: 17179869184,
    memInfo: {
      MemTotal: 16777216, MemAvailable: 8388608, Cached: 2097152,
      Buffers: 1048576, MemFree: 4194304, Slab: 524288,
      SwapTotal: 1024, SwapFree: 768
    },
    sockstat: { sockets_used: 20, TCP_inuse: 12, UDP_inuse: 3 },
    conntrack: 40, cores: {}, netdevs: {}, disks: {}, filesystems: [],
    keepalived_active: 1, keepalived_master: 1, keepalived_vip_owner: 1,
    anomaly_active: 0, anomaly_total: 0, healing_pending: 0, healing_total: 0,
    error: false
  };
  const vm = {
    name: 'vm-d1', node: 'edge-d1', nodeIP: '192.0.2.10', running: 1,
    vcpu: 2, memory_max_mb: 2048, memory_used_mb: 1024,
    disk_rd_bytes: 1024, net_rx_bytes: 2048, net_tx_bytes: 1024
  };

  await withPage(modules, async page => {
    await page.setViewport({ width: 1024, height: 900 });
    await seedProductGlobals(page, 'mon-overview');
    await page.setViewport({ width: 1024, height: 900 });
    await page.evaluate(({ nodeFixture, vmFixture }) => {
      const cb = document.getElementById('cb');
      cb.className = 'cb';
      cb.tabIndex = 0;

      const app = document.createElement('div');
      app.id = 'app';
      app.className = 'app';
      const sidebar = document.createElement('aside');
      sidebar.id = 'shell-sidebar';
      sidebar.className = 'shell-sidebar';
      const main = document.createElement('div');
      main.className = 'shell-main';
      const topbar = document.createElement('div');
      topbar.className = 'shell-topbar';
      const topbarDynamic = document.createElement('div');
      topbarDynamic.id = 'shell-topbar';
      topbarDynamic.className = 'shell-topbar-dyn';
      topbar.appendChild(topbarDynamic);
      const statusbar = document.createElement('div');
      statusbar.id = 'shell-statusbar';
      statusbar.className = 'shell-statusbar';
      const content = document.createElement('div');
      content.className = 'content shell-content';
      content.setAttribute('role', 'main');
      content.appendChild(cb);
      main.append(topbar, statusbar, content);
      app.append(sidebar, main);
      document.body.replaceChildren(app);

      window.currentUser = { role: 'ADMIN' };
      window.navigateTo = () => true;
      window.openCommandPalette = () => {};
      window.monHist[nodeFixture.ip] = {
        cpu: [nodeFixture.cpu], mem: [nodeFixture.mem], disk: [nodeFixture.disk],
        netRx: [0, 2048], netTx: [0, 1024]
      };
      PCV.shell.mount();
      window.renderMonOverview(
        cb,
        [nodeFixture],
        [vmFixture],
        1,
        nodeFixture.cpu,
        nodeFixture.mem,
        nodeFixture.disk,
        nodeFixture.ram_total
      );
    }, { nodeFixture: node, vmFixture: vm });
    await page.evaluate(() => new Promise(resolve =>
      requestAnimationFrame(() => requestAnimationFrame(resolve))));

    const layout = await page.evaluate(() => {
      const delta = element => Math.max(0, element.scrollWidth - element.clientWidth);
      const timeline = document.querySelector('.mon-overview-timeline');
      const cb = document.getElementById('cb');
      const cbRect = cb.getBoundingClientRect();
      const childRects = [...timeline.children].map(child => {
        const rect = child.getBoundingClientRect();
        return { left: rect.left, right: rect.right, width: rect.width };
      });
      const offenders = [...cb.querySelectorAll('*')].map(element => {
        const rect = element.getBoundingClientRect();
        return {
          node: element.tagName.toLowerCase() + (element.id ? '#' + element.id : '') +
            (element.className && typeof element.className === 'string'
              ? '.' + element.className.trim().replace(/\s+/g, '.') : ''),
          ownOverflow: Math.max(0, element.scrollWidth - element.clientWidth),
          rightOverflow: Math.max(0, Math.round(rect.right - cbRect.right))
        };
      }).filter(item => item.ownOverflow > 1 || item.rightOverflow > 1)
        .sort((a, b) => Math.max(b.ownOverflow, b.rightOverflow) -
          Math.max(a.ownOverflow, a.rightOverflow))
        .slice(0, 12);
      return {
        documentOverflow: Math.max(0, document.documentElement.scrollWidth - innerWidth),
        appOverflow: delta(document.getElementById('app')),
        contentOverflow: delta(document.querySelector('.shell-content')),
        cbOverflow: delta(cb),
        timelineOverflow: delta(timeline),
        cbLeft: cbRect.left,
        cbRight: cbRect.right,
        childRects,
        offenders
      };
    });
    assert.equal(layout.documentOverflow, 0, JSON.stringify(layout));
    assert.equal(layout.appOverflow, 0, JSON.stringify(layout));
    assert.equal(layout.contentOverflow, 0, JSON.stringify(layout));
    assert.equal(layout.cbOverflow, 0, JSON.stringify(layout));
    assert.equal(layout.timelineOverflow, 0, JSON.stringify(layout));
    assert.ok(layout.childRects.every(rect =>
      rect.left >= layout.cbLeft - 1 && rect.right <= layout.cbRight + 1), JSON.stringify(layout));
  }, { routes: {
    '/api/v1/health': { status: 200, body: { status: 'ok', node: 'edge-d1', subsystems: {} } },
    '/api/v1/rpc': { status: 200, body: { result: [] } }
  } });
});
