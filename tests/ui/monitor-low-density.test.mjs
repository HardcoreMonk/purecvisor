                                                                                         
                                                                                              
                                                                    
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { withPage } from './harness.mjs';

const MODS = [
  'ui/modules/ui.js',
  'ui/modules/uxlib.js',
  'ui/modules/modal-core.js',
  'ui/modules/endpoints.js',
  'ui/modules/monitor.js'
];

const REAL_API_MODS = [
  'ui/modules/ui.js',
  'ui/modules/uxlib.js',
  'ui/modules/modal-core.js',
  'ui/modules/endpoints.js',
  'ui/modules/api.js',
  'ui/modules/monitor.js'
];

function routes({ pool, dpdkStatus, dpdkList, hugepage, sriovStatus, sriovList }) {
  return {
    '/api/v1/pool/conninfo': { status: 200, body: { data: pool } },
    '/api/v1/dpdk/status': { status: 200, body: { data: dpdkStatus } },
    '/api/v1/dpdk/list': { status: 200, body: { data: dpdkList } },
    '/api/v1/dpdk/hugepage': { status: 200, body: { data: hugepage } },
    '/api/v1/sriov/status': { status: 200, body: { data: sriovStatus } },
    '/api/v1/sriov/list': { status: 200, body: { data: sriovList } }
  };
}

async function boot(page, renderer, { realApi = false } = {}) {
  await page.evaluate(({ name, realApi }) => {
    window._DEBUG = false;
    window.authToken = 'test-token';
    window.currentUser = { role: 'ADMIN' };
    window._L = text => text;
    window.t = text => text;
    window.__toasts = [];
    window.toast = (message, ok) => window.__toasts.push({ message, ok });
    window.__events = [];
    window.addEvt = message => window.__events.push(message);
    if (!realApi) {
      window.unwrapData = value => value && value.data !== undefined ? value.data : value;
      window.unwrapList = value => {
        const data = window.unwrapData(value);
        return Array.isArray(data) ? data : (data?.items || data?.list || []);
      };
                                                                   
      window.fetchGet = url => fetch(url).then(response => response.json());
      window.fetchPost = (url, body) => fetch(url, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body)
      }).then(response => response.json());
    }
    PCV.ui.navGen = () => 1;
    PCV.ui.renderTarget = () => document.getElementById('cb');
    return window[name](document.getElementById('cb'));
  }, { name: renderer, realApi });
}

function fixtureSet(overrides = {}) {
  return {
    ...routes({
      pool: { total: 10, idle: 5, max: 10, wait_avg_sec: 0.0042 },
      dpdkStatus: { available: true, vdev_count: 2, pmd_cpu_mask: '0x3', socket_mem: '1024,1024' },
      dpdkList: [
        { pci_addr: '0000:03:00.0', driver: 'vfio-pci', status: 'dpdk-bound' },
        { pci_addr: '0000:04:00.0', driver: 'vfio-pci', status: 'dpdk-bound' }
      ],
      hugepage: {
        hugepage_2m_free: 768, hugepage_2m_total: 1024, hugepage_2m_size_mb: 2,
        hugepage_1g_free: 2, hugepage_1g_total: 4, hugepage_1g_size_mb: 1024,
        total_mb: 6144, free_mb: 3584
      },
      sriovStatus: {
        available: true,
        physical_functions: [
          { name: 'enp3s0f0', max_vfs: 8, current_vfs: 2, pci_addr: '0000:03:00.0', driver: 'ixgbe', iommu_enabled: true }
        ]
      },
      sriovList: [
        { pf: 'enp3s0f0', vf_index: 0, pci_addr: '0000:03:10.0', driver: 'ixgbevf', mac: '52:54:00:00:00:01' },
        { pf: 'enp3s0f0', vf_index: 1, pci_addr: '0000:03:10.1', driver: 'ixgbevf', mac: '52:54:00:00:00:02' }
      ]
    }),
    ...overrides
  };
}

async function setOperationConfirm(page, decision) {
  await page.evaluate(confirmed => {
    window.__confirmCalls = [];
    const asyncConfirm = (...args) => {
      window.__confirmCalls.push(args);
      return Promise.resolve(confirmed);
    };
    PCV.ui.customConfirm = asyncConfirm;
    window.customConfirm = asyncConfirm;
    window.confirm = (...args) => {
      window.__confirmCalls.push(args);
      return confirmed;
    };
  }, decision);
}

function poolFixture({ total, idle, max }) {
  return routes({
    pool: { total, idle, max, wait_avg_sec: 0.0042 },
    dpdkStatus: { available: false },
    dpdkList: [],
    hugepage: {},
    sriovStatus: { available: false },
    sriovList: []
  });
}

for (const { name, total, idle, max, state } of [
  { name: 'original 50% fixture', total: 10, idle: 5, max: 10, state: 'ok' },
  { name: 'original 70% fixture', total: 10, idle: 3, max: 10, state: 'warn' },
  { name: 'original 90% fixture', total: 10, idle: 1, max: 10, state: 'crit' },
  { name: 'non-equal below-boundary fixture', total: 7, idle: 2, max: 10, state: 'ok' },
  { name: 'non-equal exact warning boundary', total: 8, idle: 2, max: 10, state: 'warn' },
  { name: 'non-equal exact critical boundary', total: 9, idle: 1, max: 10, state: 'crit' }
]) {
  test(`renderPoolInfo maps ${name} at ${(total - idle) / max * 100}% active/max to ${state}`, async () => {
    await withPage(MODS, async page => {
      await boot(page, 'renderPoolInfo');
      const result = await page.evaluate(() => {
        const token = (node, prefix) => node
          ? [...node.classList].find(name => name.startsWith(prefix))
          : undefined;
        const active = document.querySelector('.pool-kpi-active');
        const hasClasses = (node, names) => names.every(name => node?.classList.contains(name));
        const activeHeader = active?.querySelector(':scope > .justify-between');
        return {
          cards: document.querySelectorAll('.pool-kpi-card').length,
          values: [...document.querySelectorAll('.pool-kpi-card .stat-md')]
            .map(node => node.textContent.trim()),
          activePill: token(active?.querySelector('.pill'), 'pill-'),
          activePillText: active?.querySelector('.pill')?.textContent.trim(),
          dots: document.querySelectorAll('.pool-kpi-card .sdot').length,
          idleDot: token(document.querySelector('.pool-kpi-idle .sdot'), 'sdot-'),
          activeDot: token(active?.querySelector('.sdot'), 'sdot-'),
          maxDot: token(document.querySelector('.pool-kpi-max .sdot'), 'sdot-'),
          activeGauge: token(active?.querySelector('.gauge-inline'), 'g-'),
          wait: document.querySelector('.pool-kpi-wait')?.textContent.trim(),
          idleLabelAligned: hasClasses(
            document.querySelector('.pool-kpi-idle > .flex'),
            ['flex', 'items-center', 'gap-8']
          ),
          activeHeaderAligned: hasClasses(activeHeader, ['justify-between']),
          activeLabelAligned: hasClasses(
            activeHeader?.querySelector('.flex'),
            ['flex', 'items-center', 'gap-8']
          ),
          maxLabelAligned: hasClasses(
            document.querySelector('.pool-kpi-max > .flex'),
            ['flex', 'items-center', 'gap-8']
          )
        };
      });

      assert.equal(result.cards, 1);
      assert.deepEqual(result.values, [String(idle), String(total - idle), String(max)]);
      assert.equal(result.activePill, `pill-${state}`);
      assert.match(result.activePillText, new RegExp(`\\b${state.toUpperCase()}\\b`));
      assert.match(result.activePillText, new RegExp(`${Math.round((total - idle) / max * 100)}%`));
      assert.equal(result.dots, 3);
      assert.equal(result.idleDot, 'sdot-ok');
      assert.equal(result.activeDot, `sdot-${state}`);
      assert.equal(result.maxDot, 'sdot-idle');
      assert.equal(result.activeGauge, `g-${state}`);
      assert.equal(result.wait, '평균 대기 4.2ms');
      assert.equal(result.idleLabelAligned, true);
      assert.equal(result.activeHeaderAligned, true);
      assert.equal(result.activeLabelAligned, true);
      assert.equal(result.maxLabelAligned, true);
    }, { routes: poolFixture({ total, idle, max }) });
  });
}

test('renderPoolInfo marks a zero-max pool as not configured without a utilization gauge', async () => {
  await withPage(MODS, async page => {
    await boot(page, 'renderPoolInfo');
    const result = await page.evaluate(() => {
      const active = document.querySelector('.pool-kpi-active');
      const pill = active?.querySelector('.pill');
      return {
        pillText: pill?.textContent.trim(),
        pillState: pill && [...pill.classList].find(name => name.startsWith('pill-')),
        gaugeCount: active?.querySelectorAll('.gauge-inline').length
      };
    });

    assert.equal(result.pillState, 'pill-idle');
    assert.equal(result.pillText, 'NOT CONFIGURED');
    assert.equal(result.gaugeCount, 0);
  }, { routes: poolFixture({ total: 5, idle: 2, max: 0 }) });
});

test('renderDpdk renders availability, hugepage, bound-NIC, and form contracts', async () => {
  await withPage(MODS, async page => {
    await boot(page, 'renderDpdk');
    const result = await page.evaluate(() => {
      const card = title => [...document.querySelectorAll('#cb .hc')]
        .find(node => title.test(node.querySelector(':scope > h4')?.textContent.trim() || ''));
      const pill = node => {
        const target = node?.querySelector('.pill');
        return {
          text: target?.textContent.trim(),
          state: target && [...target.classList].find(name => name.startsWith('pill-'))
        };
      };
      const followingTable = node => {
        for (let cursor = node?.nextElementSibling; cursor; cursor = cursor.nextElementSibling) {
          if (cursor.matches('table')) return cursor;
          const nested = cursor.querySelector('table');
          if (nested) return nested;
        }
        return null;
      };
      const statusCard = card(/^DPDK (?:Status|상태)$/);
      const hugeCard = card(/^HugePages$/);
      const heading = document.querySelector('.dpdk-device-heading');
      const table = followingTable(heading);
      const hasClasses = (node, names) => names.every(name => node?.classList.contains(name));
      return {
        availability: pill(statusCard),
        health: pill(hugeCard),
        statusText: statusCard?.textContent,
        gauges: [...(hugeCard?.querySelectorAll('.gauge-inline') || [])]
          .map(node => [...node.classList].find(name => name.startsWith('g-'))),
        heading: heading?.querySelector('h3')?.textContent.trim(),
        count: heading?.querySelector('.pill')?.textContent.trim(),
        headingAligned: hasClasses(heading, ['justify-between', 'mt-12']),
        rows: table?.querySelectorAll('tbody tr').length,
        headers: [...(table?.querySelectorAll('thead th') || [])].map(node => node.textContent.trim()),
        cells: [...(table?.querySelectorAll('tbody td') || [])].map(node => node.textContent.trim()),
        formIds: ['dpdk-pci', 'dpdk-drv', 'dpdk-unbind-pci']
          .map(id => Boolean(document.getElementById(id)))
      };
    });

    assert.deepEqual(result.availability, { text: 'AVAILABLE', state: 'pill-ok' });
    assert.deepEqual(result.health, { text: 'OK', state: 'pill-ok' });
    assert.match(result.statusText, /0x3/);
    assert.match(result.statusText, /1024,1024/);
    assert.deepEqual(result.gauges, ['g-ok', 'g-ok']);
    assert.match(result.heading, /^(?:Bound NICs|바인딩된 NIC)$/);
    assert.equal(result.count, '2');
    assert.equal(result.headingAligned, true);
    assert.equal(result.rows, 2);
    assert.equal(result.headers.length, 3);
    assert.match(result.headers[0], /^PCI (?:Addr|Address|주소)$/);
    assert.match(result.headers[1], /^(?:Driver|드라이버)$/);
    assert.match(result.headers[2], /^(?:Status|상태)$/);
    assert.ok(result.cells.includes('0000:03:00.0'));
    assert.ok(result.cells.includes('vfio-pci'));
    assert.ok(result.cells.includes('dpdk-bound'));
    assert.deepEqual(result.formIds, [true, true, true]);
  }, {
    routes: routes({
      pool: {},
      dpdkStatus: { available: true, vdev_count: 2, pmd_cpu_mask: '0x3', socket_mem: '1024,1024' },
      dpdkList: [
        { pci_addr: '0000:03:00.0', driver: 'vfio-pci', status: 'dpdk-bound' },
        { pci_addr: '0000:04:00.0', driver: 'vfio-pci', status: 'dpdk-bound' }
      ],
      hugepage: {
        hugepage_2m_free: 768, hugepage_2m_total: 1024,
        hugepage_1g_free: 2, hugepage_1g_total: 4
      },
      sriovStatus: { available: false },
      sriovList: []
    })
  });
});

for (const capacity of [
  {
    name: 'healthy capacity',
    hugepage: {
      hugepage_2m_free: 768, hugepage_2m_total: 1024,
      hugepage_1g_free: 2, hugepage_1g_total: 4
    },
    state: 'ok',
    text: 'OK',
    gauges: ['g-ok', 'g-ok']
  },
  {
    name: 'exact 25%-free warning boundary',
    hugepage: {
      hugepage_2m_free: 256, hugepage_2m_total: 1024,
      hugepage_1g_free: 3, hugepage_1g_total: 4
    },
    state: 'warn',
    text: 'WARN',
    gauges: ['g-warn', 'g-ok']
  },
  {
    name: 'exact 10%-free critical boundary and worst-state reduction',
    hugepage: {
      hugepage_2m_free: 256, hugepage_2m_total: 1024,
      hugepage_1g_free: 1, hugepage_1g_total: 10
    },
    state: 'crit',
    text: 'CRIT',
    gauges: ['g-warn', 'g-crit']
  },
  {
    name: 'zero-total not-configured capacity',
    hugepage: {
      hugepage_2m_free: 0, hugepage_2m_total: 0,
      hugepage_1g_free: 0, hugepage_1g_total: 0
    },
    state: 'idle',
    text: 'NOT CONFIGURED',
    gauges: []
  }
]) {
  test(`renderDpdk maps HugePages ${capacity.name}`, async () => {
    await withPage(MODS, async page => {
      await boot(page, 'renderDpdk');
      const result = await page.evaluate(() => {
        const hugeCard = [...document.querySelectorAll('#cb .hc')]
          .find(node => node.querySelector(':scope > h4')?.textContent.trim() === 'HugePages');
        const status = hugeCard?.querySelector('.pill');
        return {
          text: status?.textContent.trim(),
          state: status && [...status.classList].find(name => name.startsWith('pill-')),
          gauges: [...(hugeCard?.querySelectorAll('.gauge-inline') || [])]
            .map(node => [...node.classList].find(name => name.startsWith('g-')))
        };
      });

      assert.equal(result.state, `pill-${capacity.state}`);
      assert.equal(result.text, capacity.text);
      assert.deepEqual(result.gauges, capacity.gauges);
    }, {
      routes: routes({
        pool: {},
        dpdkStatus: { available: true, pmd_cpu_mask: '0x3', socket_mem: '1024,1024' },
        dpdkList: [],
        hugepage: capacity.hugepage,
        sriovStatus: { available: false },
        sriovList: []
      })
    });
  });
}

const SRIOV_VFS = [
  { pf: 'enp3s0f0', vf_index: 0, pci_addr: '0000:03:10.0', driver: 'ixgbevf', mac: '52:54:00:00:00:01' },
  { pf: 'enp3s0f0', vf_index: 1, pci_addr: '0000:03:10.1', driver: 'ixgbevf', mac: '52:54:00:00:00:02' }
];

for (const availability of [
  { available: false, text: 'UNAVAILABLE', state: 'idle' },
  { available: true, text: 'AVAILABLE', state: 'ok' }
]) {
  test(`renderSriov renders the ${availability.text} branch and preserves device/form contracts`, async () => {
    await withPage(MODS, async page => {
      await boot(page, 'renderSriov');
      const result = await page.evaluate(() => {
        const followingTable = node => {
          for (let cursor = node?.nextElementSibling; cursor; cursor = cursor.nextElementSibling) {
            if (cursor.matches('table')) return cursor;
            const nested = cursor.querySelector('table');
            if (nested) return nested;
          }
          return null;
        };
        const status = document.querySelector('.sriov-status-card .pill');
        const heading = document.querySelector('.sriov-device-heading');
        const table = followingTable(heading);
        const pfTable = document.querySelector('.sriov-pf-table');
        const hasClasses = (node, names) => names.every(name => node?.classList.contains(name));
        return {
          status: {
            text: status?.textContent.trim(),
            state: status && [...status.classList].find(name => name.startsWith('pill-'))
          },
          heading: heading?.querySelector('h3')?.textContent.trim(),
          count: heading?.querySelector('.pill')?.textContent.trim(),
          headingAligned: hasClasses(heading, ['justify-between', 'mt-12']),
          rows: table?.querySelectorAll('tbody tr').length,
          headers: [...(table?.querySelectorAll('thead th') || [])].map(node => node.textContent.trim()),
          cells: [...(table?.querySelectorAll('tbody td') || [])].map(node => node.textContent.trim()),
          pfHeaders: [...(pfTable?.querySelectorAll('thead th') || [])].map(node => node.textContent.trim()),
          pfCells: [...(pfTable?.querySelectorAll('tbody td') || [])].map(node => node.textContent.trim()),
          formIds: ['sriov-pf', 'sriov-numvf', 'sriov-vm', 'sriov-vf-pci']
            .map(id => Boolean(document.getElementById(id)))
        };
      });

      assert.deepEqual(result.status, {
        text: availability.text,
        state: `pill-${availability.state}`
      });
      assert.match(result.heading, /^(?:Active VFs|활성 VF)$/);
      assert.equal(result.count, '2');
      assert.equal(result.headingAligned, true);
      assert.equal(result.rows, 2);
      assert.equal(result.headers.length, 5);
      assert.equal(result.headers[0], 'PF');
      assert.equal(result.headers[1], 'VF Index');
      assert.match(result.headers[2], /^PCI (?:Addr|Address|주소)$/);
      assert.match(result.headers[3], /^(?:Driver|드라이버)$/);
      assert.equal(result.headers[4], 'MAC');
      assert.ok(result.cells.includes('enp3s0f0'));
      assert.ok(result.cells.includes('1'));
      assert.ok(result.cells.includes('0000:03:10.1'));
      assert.ok(result.cells.includes('ixgbevf'));
      assert.ok(result.cells.includes('52:54:00:00:00:02'));
      assert.equal(result.pfHeaders.length, 5);
      assert.ok(result.pfCells.includes('0000:03:00.0'));
      assert.ok(result.pfCells.includes('2 / 8'));
      assert.ok(result.pfCells.includes('ixgbe'));
      assert.ok(result.pfCells.includes('ENABLED'));
      assert.deepEqual(result.formIds, [true, true, true, true]);
    }, {
      routes: routes({
        pool: {},
        dpdkStatus: { available: false },
        dpdkList: [],
        hugepage: {},
        sriovStatus: {
          available: availability.available,
          physical_functions: [
            { name: 'enp3s0f0', max_vfs: 8, current_vfs: 2, pci_addr: '0000:03:00.0', driver: 'ixgbe', iommu_enabled: true }
          ]
        },
        sriovList: SRIOV_VFS
      })
    });
  });
}

test('connection-pool, DPDK, and SR-IOV pages are reachable from the shell and nav dispatcher', () => {
  const shellSource = readFileSync(new URL('../../ui/modules/shell.js', import.meta.url), 'utf8');
  const navSource = readFileSync(new URL('../../ui/modules/nav.js', import.meta.url), 'utf8');
  const indexSource = readFileSync(new URL('../../ui/index.html', import.meta.url), 'utf8');
  const pages = [
    { id: 'pool-info', renderer: 'renderPoolInfo' },
    { id: 'dpdk', renderer: 'renderDpdk' },
    { id: 'sriov', renderer: 'renderSriov' }
  ];

  for (const page of pages) {
    assert.match(
      shellSource,
      new RegExp(`\\{\\s*id:\\s*['"]${page.id}['"]`),
      `${page.id} must have a shell navigation item`
    );
    assert.match(
      navSource,
      new RegExp(`(?:['"]${page.id}['"]|${page.id})\\s*:\\s*\\(\\)\\s*=>\\s*${page.renderer}\\(b\\)`),
      `${page.id} must dispatch to ${page.renderer}`
    );
  }
  assert.match(
    indexSource,
    /<div\b[^>]*\bid=["']cb["'][^>]*\btabindex=["']0["'][^>]*><\/div>/,
    'the persistent scrollable content landmark must be keyboard-focusable even when every action is disabled'
  );
});

for (const emptyCase of [
  {
    name: 'DPDK bound NIC list',
    renderer: 'renderDpdk',
    routes: fixtureSet({ '/api/v1/dpdk/list': { status: 200, body: { data: [] } } }),
    expected: /no bound nics?|바인딩[^\n]*없/i
  },
  {
    name: 'SR-IOV active VF list',
    renderer: 'renderSriov',
    routes: fixtureSet({ '/api/v1/sriov/list': { status: 200, body: { data: [] } } }),
    expected: /no active vfs?|활성[^\n]*vf[^\n]*없/i
  }
]) {
  test(`${emptyCase.name} renders an explicit empty state`, async () => {
    await withPage(MODS, async page => {
      await boot(page, emptyCase.renderer);
      const state = await page.evaluate(() => {
        const node = document.querySelector('#cb .empty-state, #cb [data-state="empty"]');
        return node && { text: node.textContent.trim(), role: node.getAttribute('role') };
      });

      assert.ok(state, 'an explicit empty-state component must remain visible');
      assert.match(state.text, emptyCase.expected);
    }, { routes: emptyCase.routes });
  });
}

for (const errorCase of [
  { name: 'connection pool', renderer: 'renderPoolInfo', path: '/api/v1/pool/conninfo' },
  { name: 'DPDK', renderer: 'renderDpdk', path: '/api/v1/dpdk/status' },
  { name: 'SR-IOV', renderer: 'renderSriov', path: '/api/v1/sriov/status' }
]) {
  test(`${errorCase.name} recognizes an HTTP {error} body and renders an actionable error state`, async () => {
    const message = `${errorCase.name} fixture outage`;
    const errorRoutes = fixtureSet({
      [errorCase.path]: {
        status: 503,
        body: { error: { code: 'BACKEND_UNAVAILABLE', message } }
      }
    });

    await withPage(MODS, async page => {
      await boot(page, errorCase.renderer);
      const state = await page.evaluate(() => {
        const node = document.querySelector('#cb [role="alert"], #cb [data-state="error"], #cb .error-state');
        const retry = node && [...node.querySelectorAll('button')]
          .find(button => /retry|재시도|다시\s*시도/i.test(button.textContent));
        return node && { text: node.textContent.trim(), hasRetry: Boolean(retry) };
      });

      assert.ok(state, 'HTTP {error} must not be rendered as zero, unavailable, or empty data');
      assert.match(state.text, new RegExp(message, 'i'));
      assert.equal(state.hasRetry, true, 'the error state must offer an explicit retry action');
    }, { routes: errorRoutes });
  });
}

test('malformed HTTP 200 read payloads fail closed before D2 operations render', async () => {
  const cases = [
    {
      name: 'connection pool null data', renderer: 'renderPoolInfo', path: '/api/v1/pool/conninfo',
      body: { data: null }
    },
    {
      name: 'connection pool identity object', renderer: 'renderPoolInfo', path: '/api/v1/pool/conninfo',
      body: {}
    },
    {
      name: 'connection pool nonnumeric field', renderer: 'renderPoolInfo', path: '/api/v1/pool/conninfo',
      body: { data: { total: 10, idle: 5, max: '10', wait_avg_sec: 0.0042 } }
    },
    {
      name: 'DPDK null status', renderer: 'renderDpdk', path: '/api/v1/dpdk/status',
      body: { data: null }
    },
    {
      name: 'DPDK identity object status', renderer: 'renderDpdk', path: '/api/v1/dpdk/status',
      body: {}
    },
    {
      name: 'DPDK nonboolean availability', renderer: 'renderDpdk', path: '/api/v1/dpdk/status',
      body: { data: { available: 'true' } }
    },
    {
      name: 'DPDK nonarray device list', renderer: 'renderDpdk', path: '/api/v1/dpdk/list',
      body: { data: {} }
    },
    {
      name: 'DPDK null device item', renderer: 'renderDpdk', path: '/api/v1/dpdk/list',
      body: { data: [null] }
    },
    {
      name: 'DPDK item missing canonical fields', renderer: 'renderDpdk', path: '/api/v1/dpdk/list',
      body: { data: [{}] }
    },
    {
      name: 'DPDK item with nonstring status', renderer: 'renderDpdk', path: '/api/v1/dpdk/list',
      body: { data: [{ pci_addr: '0000:03:00.0', status: false }] }
    },
    {
      name: 'DPDK incomplete HugePages object', renderer: 'renderDpdk', path: '/api/v1/dpdk/hugepage',
      body: { data: {} }
    },
    {
      name: 'SR-IOV nonboolean availability', renderer: 'renderSriov', path: '/api/v1/sriov/status',
      body: { data: { available: 'false', physical_functions: [] } }
    },
    {
      name: 'SR-IOV identity object status', renderer: 'renderSriov', path: '/api/v1/sriov/status',
      body: {}
    },
    {
      name: 'SR-IOV nonarray PF list', renderer: 'renderSriov', path: '/api/v1/sriov/status',
      body: { data: { available: false, physical_functions: {} } }
    },
    {
      name: 'SR-IOV PF item missing canonical fields', renderer: 'renderSriov', path: '/api/v1/sriov/status',
      body: { data: { available: true, physical_functions: [{}] } }
    },
    {
      name: 'SR-IOV PF item with nonboolean IOMMU state', renderer: 'renderSriov', path: '/api/v1/sriov/status',
      body: { data: { available: true, physical_functions: [{
        name: 'enp3s0f0', current_vfs: 1, max_vfs: 8, iommu_enabled: 'true'
      }] } }
    },
    {
      name: 'SR-IOV nonarray VF list', renderer: 'renderSriov', path: '/api/v1/sriov/list',
      body: { data: {} }
    },
    {
      name: 'SR-IOV VF item missing canonical fields', renderer: 'renderSriov', path: '/api/v1/sriov/list',
      body: { data: [{}] }
    },
    {
      name: 'SR-IOV VF item with string index', renderer: 'renderSriov', path: '/api/v1/sriov/list',
      body: { data: [{ pf: 'enp3s0f0', vf_index: '0', pci_addr: '0000:03:10.0' }] }
    },
    {
      name: 'SR-IOV VF item missing operation PCI address', renderer: 'renderSriov', path: '/api/v1/sriov/list',
      body: { data: [{ pf: 'enp3s0f0', vf_index: 0 }] }
    }
  ];

  for (const malformed of cases) {
    await withPage(REAL_API_MODS, async page => {
      await boot(page, malformed.renderer, { realApi: true });
      const state = await page.evaluate(() => {
        const alert = document.querySelector('#cb [role="alert"]');
        return {
          alert: alert?.textContent || '',
          retry: Boolean(alert?.querySelector('button')),
          operations: document.querySelectorAll('#cb .d2-operations').length,
          mutationButtons: document.querySelectorAll('#cb [data-d2-action]').length,
          poolCards: document.querySelectorAll('#cb .pool-kpi-card').length
        };
      });

      assert.match(state.alert, /malformed success response|잘못된 성공 응답/i, malformed.name);
      assert.equal(state.retry, true, `${malformed.name}: Retry must remain available`);
      assert.equal(state.operations, 0, `${malformed.name}: operation section must not render`);
      assert.equal(state.mutationButtons, 0, `${malformed.name}: mutation controls must not render`);
      assert.equal(state.poolCards, 0, `${malformed.name}: status cards must not render`);
    }, {
      routes: fixtureSet({
        [malformed.path]: { status: 200, body: malformed.body }
      })
    });
  }
});

test('Host Health rejects malformed accelerator HTTP 200 status before rendering ON or OFF', async () => {
  const hostMetrics = [
    'purecvisor_host_cpu_percent 10',
    'purecvisor_host_memory_percent 20',
    'purecvisor_host_disk_percent 30',
    'purecvisor_host_cpu_temp_celsius 40',
    'purecvisor_host_load1 0.5'
  ].join('\n') + '\n';
  const cases = [
    { name: 'DPDK missing availability', path: '/api/v1/dpdk/status', body: { data: {} }, label: 'DPDK status' },
    { name: 'DPDK string availability', path: '/api/v1/dpdk/status', body: { data: { available: 'false' } }, label: 'DPDK status' },
    { name: 'SR-IOV missing availability', path: '/api/v1/sriov/status', body: { data: {} }, label: 'SR-IOV status' },
    { name: 'SR-IOV string availability', path: '/api/v1/sriov/status', body: { data: { available: 'false' } }, label: 'SR-IOV status' }
  ];

  for (const malformed of cases) {
    await withPage(REAL_API_MODS, async page => {
      await boot(page, 'renderHost', { realApi: true });
      const state = await page.evaluate(() => {
        const alert = document.querySelector('#cb [role="alert"]');
        return {
          alert: alert?.textContent || '',
          retry: Boolean(alert?.querySelector('button')),
          hostGrid: document.querySelectorAll('#cb .host-ops-grid').length,
          acceleratorStates: [...document.querySelectorAll('#cb .pill')]
            .filter(node => /^(?:ON|OFF)$/.test(node.textContent.trim())).length
        };
      });

      assert.match(state.alert, new RegExp(malformed.label, 'i'), malformed.name);
      assert.match(state.alert, /malformed success response|잘못된 성공 응답/i, malformed.name);
      assert.equal(state.retry, true, `${malformed.name}: Retry must remain available`);
      assert.equal(state.hostGrid, 0, `${malformed.name}: Host Health cards must not render`);
      assert.equal(state.acceleratorStates, 0, `${malformed.name}: malformed state must not become ON or OFF`);
    }, {
      routes: fixtureSet({
        '/api/v1/metrics': { status: 200, body: hostMetrics },
        [malformed.path]: { status: 200, body: malformed.body }
      })
    });
  }
});

test('connection-pool Retry re-invokes the renderer and replaces error state with live data', async () => {
  let attempts = 0;
  const retryRoutes = fixtureSet({
    '/api/v1/pool/conninfo': () => {
      attempts += 1;
      return attempts === 1
        ? { status: 503, body: { error: { code: 'TEMPORARY', message: 'retry fixture outage' } } }
        : { status: 200, body: { data: { total: 10, idle: 5, max: 10, wait_avg_sec: 0.0042 } } };
    }
  });

  await withPage(MODS, async (page, context) => {
    await boot(page, 'renderPoolInfo');
    assert.ok(await page.$('#cb [role="alert"]'), 'first response must render the error state');

    await page.click('#cb [role="alert"] button');
    await page.waitForSelector('#cb .pool-kpi-card', { timeout: 3000 });

    const state = await page.evaluate(() => ({
      hasError: Boolean(document.querySelector('#cb [role="alert"]')),
      values: [...document.querySelectorAll('#cb .pool-kpi-card .stat-md')]
        .map(node => node.textContent.trim())
    }));
    const requests = context.requests.filter(request => request.path === '/api/v1/pool/conninfo');
    assert.equal(requests.length, 2, 'Retry must make exactly one fresh pool request');
    assert.deepEqual(state, { hasError: false, values: ['5', '5', '10'] });
  }, { routes: retryRoutes });
});

for (const operationSurface of [
  { name: 'DPDK', renderer: 'renderDpdk', count: 2 },
  { name: 'SR-IOV', renderer: 'renderSriov', count: 4 }
]) {
  test(`${operationSurface.name} mutation controls declare the ADMIN visibility contract`, async () => {
    await withPage(MODS, async page => {
      await boot(page, operationSurface.renderer);
      const roles = await page.evaluate(() => [...document.querySelectorAll('#cb button')].map(button => ({
        label: button.textContent.trim(),
        role: button.closest('[data-role]')?.getAttribute('data-role') || null
      })));

      assert.equal(roles.length, operationSurface.count);
      assert.deepEqual(
        roles.map(entry => entry.role),
        Array(operationSurface.count).fill('ADMIN'),
        `${operationSurface.name} mutations are backend ADMIN-only and must not be offered to lower roles`
      );
    }, { routes: fixtureSet() });
  });
}

test('low-density ADMIN operation surfaces are hidden for VIEWER/OPERATOR and visible for ADMIN', async () => {
  await withPage(MODS, async page => {
    for (const renderer of ['renderDpdk', 'renderSriov']) {
      await boot(page, renderer);
      for (const role of ['VIEWER', 'OPERATOR', 'ADMIN']) {
        const state = await page.evaluate(effectiveRole => {
          window.currentUser = { role: effectiveRole };
          applyRoleVisibility(effectiveRole);
          const operations = document.querySelector('#cb .d2-operations');
          return operations && {
            dataRole: operations.getAttribute('data-role'),
            roleHidden: operations.classList.contains('role-hidden'),
            display: getComputedStyle(operations).display
          };
        }, role);

        assert.ok(state, `${renderer}: .d2-operations must exist`);
        assert.equal(state.dataRole, 'ADMIN');
        if (role === 'ADMIN') {
          assert.equal(state.roleHidden, false, `${renderer}: ADMIN must not be role-hidden`);
          assert.notEqual(state.display, 'none', `${renderer}: ADMIN operations must be visible`);
        } else {
          assert.equal(state.roleHidden, true, `${renderer}: ${role} must be role-hidden`);
          assert.equal(state.display, 'none', `${renderer}: ${role} operations must be visually hidden`);
        }
      }
    }
  }, { routes: fixtureSet() });
});

test('low-density mutation surfaces stay fail-closed while the effective role is unknown', async () => {
  await withPage(MODS, async page => {
    for (const renderer of ['renderDpdk', 'renderSriov']) {
      await boot(page, renderer);
      const state = await page.evaluate(async name => {
        window.currentUser = null;
        await window[name](document.getElementById('cb'));
        const operations = document.querySelector('#cb .d2-operations');
        return {
          hidden: operations.classList.contains('role-hidden'),
          display: getComputedStyle(operations).display
        };
      }, renderer);
      assert.deepEqual(state, { hidden: true, display: 'none' }, `${renderer}: unknown role`);
    }
  }, { routes: fixtureSet() });
});

test('SR-IOV attach is fail-closed for a VF on an IOMMU-disabled PF while detach stays available', async () => {
  const unsafeRoutes = fixtureSet({
    '/api/v1/sriov/status': {
      status: 200,
      body: { data: { available: true, physical_functions: [
        { name: 'enp3s0f0', max_vfs: 8, current_vfs: 1, pci_addr: '0000:03:00.0', driver: 'ixgbe', iommu_enabled: false }
      ] } }
    },
    '/api/v1/sriov/list': {
      status: 200,
      body: { data: [
        { pf: 'enp3s0f0', vf_index: 0, pci_addr: '0000:03:10.0', driver: 'ixgbevf', mac: '52:54:00:00:00:01' }
      ] }
    },
    '/api/v1/sriov/attach': { status: 200, body: { data: { status: 'attached' } } }
  });

  await withPage(MODS, async (page, context) => {
    await boot(page, 'renderSriov');
    await setOperationConfirm(page, true);
    const state = await page.evaluate(async () => {
      document.getElementById('sriov-vm').value = 'edge-c';
      const select = document.getElementById('sriov-vf-pci');
      select.value = '0000:03:10.0';
      select.dispatchEvent(new Event('change', { bubbles: true }));
      const attach = document.querySelector('[data-d2-action="sriov-attach"]');
      const detach = document.querySelector('[data-d2-action="sriov-detach"]');
      await window.sriovAttach();
      return {
        selectDisabled: select.disabled,
        attachDisabled: attach.disabled,
        detachDisabled: detach.disabled,
        confirms: window.__confirmCalls.length,
        text: document.getElementById('cb').textContent
      };
    });
    assert.equal(state.selectDisabled, false, 'the VF remains selectable for recovery detach');
    assert.equal(state.attachDisabled, true, 'attach must stay locked without IOMMU');
    assert.equal(state.detachDisabled, false, 'detach must remain available as a recovery path');
    assert.equal(state.confirms, 0, 'unsafe attach must stop before confirmation');
    assert.match(state.text, /IOMMU/);
    assert.equal(context.requests.filter(request => request.path === '/api/v1/sriov/attach').length, 0);
  }, { routes: unsafeRoutes });
});

test('DPDK unavailable locks new binding but keeps canonical unbind recovery available', async () => {
  const unavailableRoutes = fixtureSet({
    '/api/v1/dpdk/status': {
      status: 200,
      body: { data: { available: false, vdev_count: 1 } }
    },
    '/api/v1/dpdk/list': {
      status: 200,
      body: { data: [
        { pci_addr: '0000:06:00.0', driver: 'vfio-pci', status: 'dpdk-bound' }
      ] }
    },
    '/api/v1/dpdk/unbind': {
      status: 200,
      body: { data: { status: 'unbound' } }
    }
  });

  await withPage(MODS, async (page, context) => {
    await boot(page, 'renderDpdk');
    const controls = await page.evaluate(() => ({
      bindPciDisabled: document.getElementById('dpdk-pci').disabled,
      bindDriverDisabled: document.getElementById('dpdk-drv').disabled,
      bindDisabled: document.querySelector('[data-d2-action="dpdk-bind"]').disabled,
      unbindPciDisabled: document.getElementById('dpdk-unbind-pci').disabled,
      unbindDisabled: document.querySelector('[data-d2-action="dpdk-unbind"]').disabled
    }));
    assert.deepEqual(controls, {
      bindPciDisabled: true,
      bindDriverDisabled: true,
      bindDisabled: true,
      unbindPciDisabled: false,
      unbindDisabled: false
    }, 'an unavailable host must preserve the unbind escape hatch without allowing new binding');

    await page.type('#dpdk-unbind-pci', '0000:06:00.0');
    await page.focus('[data-d2-action="dpdk-unbind"]');
    await page.keyboard.press('Enter');
    await page.waitForSelector('dialog.modal[open] [data-confirm-accept]', { timeout: 3000 });
    const confirmation = await page.evaluate(() => {
      const dialog = document.querySelector('dialog.modal[open]');
      return {
        usesModalCore: PCV.modalCore.currentDialog() === dialog,
        title: dialog.querySelector('h2')?.textContent || '',
        message: dialog.querySelector('p')?.textContent || '',
        bindDisabled: document.querySelector('[data-d2-action="dpdk-bind"]').disabled,
        unbindDisabled: document.querySelector('[data-d2-action="dpdk-unbind"]').disabled,
        unbindBusy: document.querySelector('[data-d2-action="dpdk-unbind"]').getAttribute('aria-busy')
      };
    });
    assert.equal(confirmation.usesModalCore, true);
    assert.match(confirmation.title, /DPDK NIC .*확인|Confirm DPDK NIC unbinding/i);
    assert.match(confirmation.message, /0000:06:00\.0/);
    assert.match(confirmation.message, /네트워크.*끊|network connectivity/i);
    assert.equal(confirmation.bindDisabled, true);
    assert.equal(confirmation.unbindDisabled, false);
    assert.equal(confirmation.unbindBusy, null, 'confirmation reserves logically before approval');

    await page.click('dialog.modal[open] [data-confirm-accept]');
    await page.waitForFunction(() =>
      !document.querySelector('dialog.modal[open]') &&
      document.querySelector('[data-d2-action="dpdk-unbind"]')?.disabled === false &&
      document.querySelector('[data-d2-action="dpdk-unbind"]')?.getAttribute('aria-busy') !== 'true');

    const posts = context.requests.filter(request =>
      request.method === 'POST' && request.path === '/api/v1/dpdk/unbind');
    assert.equal(posts.length, 1, 'approved recovery must send exactly one unbind request');
    assert.deepEqual(posts[0].json, { pci_addr: '0000:06:00.0' });
    assert.equal(
      await page.evaluate(() => document.activeElement?.getAttribute('data-d2-action')),
      'dpdk-unbind',
      'the recovery action must regain focus after its awaited refresh'
    );
  }, { routes: unavailableRoutes });
});

const MUTATION_CASES = [
  {
    name: 'DPDK bind', renderer: 'renderDpdk', fn: 'dpdkBind', path: '/api/v1/dpdk/bind',
    action: 'dpdk-bind',
    expectedStatus: 'bound',
    body: { pci_addr: '0000:05:00.0', driver: 'vfio-pci' }
  },
  {
    name: 'DPDK unbind', renderer: 'renderDpdk', fn: 'dpdkUnbind', path: '/api/v1/dpdk/unbind',
    action: 'dpdk-unbind',
    expectedStatus: 'unbound',
    body: { pci_addr: '0000:06:00.0' }
  },
  {
    name: 'SR-IOV enable', renderer: 'renderSriov', fn: 'sriovEnable', path: '/api/v1/sriov/enable',
    action: 'sriov-enable',
    expectedStatus: 'enabled',
    body: { pf: 'enp3s0f0', num_vfs: 6 }
  },
  {
    name: 'SR-IOV disable', renderer: 'renderSriov', fn: 'sriovDisable', path: '/api/v1/sriov/disable',
    action: 'sriov-disable',
    expectedStatus: 'disabled',
    body: { pf: 'enp3s0f0' }
  },
  {
    name: 'SR-IOV attach', renderer: 'renderSriov', fn: 'sriovAttach', path: '/api/v1/sriov/attach',
    action: 'sriov-attach',
    expectedStatus: 'attached',
    body: { vm_name: 'edge-c', pf: 'enp3s0f0', vf_index: 1 }
  },
  {
    name: 'SR-IOV detach', renderer: 'renderSriov', fn: 'sriovDetach', path: '/api/v1/sriov/detach',
    action: 'sriov-detach',
    expectedStatus: 'detached',
    body: { vm_name: 'edge-c', pci_addr: '0000:03:10.1' }
  }
];

async function fillMutationFields(page, mutation) {
  await page.evaluate(spec => {
    if (spec.fn === 'dpdkBind') {
      document.getElementById('dpdk-pci').value = spec.body.pci_addr;
      document.getElementById('dpdk-drv').value = spec.body.driver;
    } else if (spec.fn === 'dpdkUnbind') {
      document.getElementById('dpdk-unbind-pci').value = spec.body.pci_addr;
    } else if (spec.fn === 'sriovEnable') {
      document.getElementById('sriov-pf').value = spec.body.pf;
      document.getElementById('sriov-numvf').value = String(spec.body.num_vfs);
    } else if (spec.fn === 'sriovDisable') {
      document.getElementById('sriov-pf').value = spec.body.pf;
    } else {
      document.getElementById('sriov-vm').value = spec.body.vm_name;
      const vf = document.getElementById('sriov-vf-pci');
      if (!vf) throw new Error('SR-IOV attach/detach VF control is missing');
      const pci = spec.body.pci_addr || '0000:03:10.1';
      const option = [...vf.options].find(candidate =>
        candidate.value === pci || candidate.textContent.includes(pci));
      if (!option) throw new Error(`canonical VF fixture ${pci} is not selectable`);
      vf.value = option.value;
      vf.dispatchEvent(new Event('change', { bubbles: true }));
    }
  }, mutation);
}

async function fillAndInvokeMutation(page, mutation) {
  await fillMutationFields(page, mutation);
  await page.evaluate(async spec => {
    await window[spec.fn]();
  }, mutation);
}

function operationButtonStates(page) {
  return page.evaluate(() => Object.fromEntries(
    [...document.querySelectorAll('[data-d2-action]')].map(button => [
      button.getAttribute('data-d2-action'),
      { disabled: button.disabled, ariaBusy: button.getAttribute('aria-busy') }
    ])
  ));
}

for (const mutation of MUTATION_CASES) {
  test(`${mutation.name} keyboard confirmation Escape restores trigger focus and keeps one logical reservation`, async () => {
    await withPage(MODS, async (page, context) => {
      await boot(page, mutation.renderer);
      await fillMutationFields(page, mutation);
      const selector = `[data-d2-action="${mutation.action}"]`;
      const before = await operationButtonStates(page);
      assert.equal(before[mutation.action]?.disabled, false, `${mutation.name}: trigger must begin enabled`);
      assert.equal(
        await page.evaluate(() => window.customConfirm === window.PCV.ui.customConfirm),
        true,
        `${mutation.name}: test must exercise the product customConfirm`
      );

      await page.focus(selector);
      assert.equal(
        await page.evaluate(() => document.activeElement?.getAttribute('data-d2-action')),
        mutation.action,
        `${mutation.name}: keyboard flow must begin on the mutation trigger`
      );
      await page.keyboard.press('Enter');
      await page.waitForSelector('dialog.modal[open] [data-confirm-cancel]', { timeout: 3000 });
      assert.equal(
        await page.evaluate(() =>
          PCV.modalCore.currentDialog() === document.querySelector('dialog.modal[open]')),
        true,
        `${mutation.name}: confirmation must use the product modal-core dialog`
      );
      assert.deepEqual(
        await operationButtonStates(page),
        before,
        `${mutation.name}: confirmation must reserve only the logical lock before approval`
      );
      await page.evaluate(async spec => window[spec.fn](), mutation);
      assert.equal(
        await page.$$eval('dialog.modal[open]', dialogs => dialogs.length),
        1,
        `${mutation.name}: a second direct invocation must not open another confirmation`
      );
      assert.equal(
        context.requests.filter(request =>
          request.method === 'POST' && request.path === mutation.path).length,
        0,
        `${mutation.name}: logical reservation must stop the second invocation before POST`
      );

      await page.keyboard.press('Escape');
      await page.waitForFunction(() =>
        !document.querySelector('dialog.modal[open]') &&
        [...document.querySelectorAll('[data-d2-action]')]
          .every(button => button.getAttribute('aria-busy') !== 'true'));

      const after = await operationButtonStates(page);
      assert.deepEqual(after, before, `${mutation.name}: cancel must restore disabled/aria-busy state`);
      assert.equal(
        await page.evaluate(() => document.activeElement?.getAttribute('data-d2-action')),
        mutation.action,
        `${mutation.name}: cancel must restore focus to the action that opened confirmation`
      );
      assert.equal(
        context.requests.filter(request => request.method === 'POST' && request.path === mutation.path).length,
        0,
        `${mutation.name}: cancellation must send zero POST requests`
      );
    }, {
      routes: fixtureSet({
        [mutation.path]: { status: 200, body: { data: { status: mutation.expectedStatus } } }
      })
    });
  });
}

test('approved delayed mutation exposes busy state only after approval and focuses its replacement action', async () => {
  const delayedRoutes = fixtureSet({
    '/api/v1/dpdk/bind': async () => {
      await new Promise(resolve => setTimeout(resolve, 250));
      return { status: 200, body: { data: { status: 'bound' } } };
    }
  });

  await withPage(MODS, async (page, context) => {
    const mutation = MUTATION_CASES.find(candidate => candidate.fn === 'dpdkBind');
    await boot(page, mutation.renderer);
    await fillMutationFields(page, mutation);
    const selector = `[data-d2-action="${mutation.action}"]`;
    const before = await operationButtonStates(page);
    await page.focus(selector);
    await page.evaluate(selectorValue => {
      window.__d2OriginalTrigger = document.querySelector(selectorValue);
    }, selector);
    await page.keyboard.press('Enter');
    await page.waitForSelector('dialog.modal[open] [data-confirm-accept]', { timeout: 3000 });
    assert.deepEqual(
      await operationButtonStates(page),
      before,
      'opening the actual confirmation must not expose DOM busy state'
    );

    await page.click('dialog.modal[open] [data-confirm-accept]');
    await page.waitForFunction(() =>
      [...document.querySelectorAll('[data-d2-action]')].length > 0 &&
      [...document.querySelectorAll('[data-d2-action]')]
        .every(button => button.disabled && button.getAttribute('aria-busy') === 'true'));
    assert.ok(
      (await operationButtonStates(page))[mutation.action].ariaBusy === 'true',
      'approval must expose aria-busy while the delayed POST is in flight'
    );

    await page.waitForFunction(action =>
      !document.querySelector('dialog.modal[open]') &&
      document.activeElement?.getAttribute('data-d2-action') === action &&
      [...document.querySelectorAll('[data-d2-action]')]
        .every(button => button.getAttribute('aria-busy') !== 'true'),
    { timeout: 3000 }, mutation.action);

    const completion = await page.evaluate(action => ({
      oldConnected: window.__d2OriginalTrigger.isConnected,
      isReplacement: document.activeElement !== window.__d2OriginalTrigger,
      action: document.activeElement?.getAttribute('data-d2-action'),
      disabled: document.activeElement?.disabled,
      busy: document.activeElement?.getAttribute('aria-busy')
    }), mutation.action);
    assert.deepEqual(completion, {
      oldConnected: false,
      isReplacement: true,
      action: mutation.action,
      disabled: false,
      busy: null
    });
    assert.equal(
      context.requests.filter(request =>
        request.method === 'POST' && request.path === mutation.path).length,
      1,
      'approved delayed operation must send exactly one POST'
    );
  }, { routes: delayedRoutes });
});

for (const mutation of MUTATION_CASES.filter(candidate =>
  candidate.fn === 'dpdkBind' || candidate.fn === 'sriovAttach')) {
  test(`${mutation.name} approved after navigation cannot repaint the fresh destination`, async () => {
    const delayedRoutes = fixtureSet({
      [mutation.path]: async () => {
        await new Promise(resolve => setTimeout(resolve, 250));
        return { status: 200, body: { data: { status: mutation.expectedStatus } } };
      }
    });

    await withPage(MODS, async (page, context) => {
      await boot(page, mutation.renderer);
      await fillMutationFields(page, mutation);
      await page.evaluate(() => {
        window.__d2NavigationGeneration = 10;
        PCV.ui.navGen = () => window.__d2NavigationGeneration;
        PCV.ui.renderTarget = () => document.getElementById('cb');
      });
      const before = await operationButtonStates(page);
      const readPaths = mutation.renderer === 'renderDpdk'
        ? ['/api/v1/dpdk/status', '/api/v1/dpdk/list', '/api/v1/dpdk/hugepage']
        : ['/api/v1/sriov/status', '/api/v1/sriov/list'];
      const initialReadCount = context.requests.filter(request =>
        request.method === 'GET' && readPaths.includes(request.path)).length;

      await page.click(`[data-d2-action="${mutation.action}"]`);
      await page.waitForSelector('dialog.modal[open] [data-confirm-accept]', { timeout: 3000 });
      assert.deepEqual(
        await operationButtonStates(page),
        before,
        `${mutation.name}: navigation during confirmation must begin before DOM busy state`
      );

      await page.evaluate(action => {
        const oldTarget = document.getElementById('cb');
        window.__d2NavigationGeneration += 1;
        const freshTarget = document.createElement('div');
        freshTarget.id = 'cb';
        freshTarget.textContent = 'FRESH DESTINATION';
                                                            
                                                 
        const freshAction = document.createElement('button');
        freshAction.id = 'fresh-destination-action';
        freshAction.type = 'button';
        freshAction.setAttribute('data-d2-action', action);
        freshAction.setAttribute('aria-label', 'fresh destination action');
        freshTarget.appendChild(freshAction);
        oldTarget.replaceWith(freshTarget);
      }, mutation.action);
      await page.click('dialog.modal[open] [data-confirm-accept]');
      const successFragment = mutation.fn === 'dpdkBind' ? 'DPDK bind:' : 'VF attached to';
      await page.waitForFunction(fragment =>
        window.__toasts.some(entry => String(entry.message).includes(fragment)),
      { timeout: 3000 }, successFragment);
      assert.equal(
        context.requests.filter(request =>
          request.method === 'POST' && request.path === mutation.path).length,
        1,
        `${mutation.name}: approval must send exactly one POST`
      );
      assert.equal(
        context.requests.filter(request =>
          request.method === 'GET' && readPaths.includes(request.path)).length,
        initialReadCount,
        `${mutation.name}: stale completion must not invoke the old renderer`
      );
      assert.equal(
        await page.$eval('#cb', node => node.textContent),
        'FRESH DESTINATION',
        `${mutation.name}: stale completion must preserve the new navigation destination`
      );
      assert.equal(
        await page.evaluate(() => document.activeElement?.id === 'fresh-destination-action'),
        false,
        `${mutation.name}: stale completion must not steal focus into the new destination`
      );
    }, { routes: delayedRoutes });
  });
}

test('all six low-density mutations require confirmation and send one canonical request only after approval', async () => {
  const mutationRoutes = Object.fromEntries(MUTATION_CASES.map(mutation => [
    mutation.path,
    { status: 200, body: { data: { status: mutation.expectedStatus } } }
  ]));

  await withPage(MODS, async (page, context) => {
    let activeRenderer = null;
    for (const mutation of MUTATION_CASES) {
      if (mutation.renderer !== activeRenderer) {
        await boot(page, mutation.renderer);
                                                              
                                                         
        await page.evaluate(() => {
          window.__mutationNavGen = 0;
          PCV.ui.navGen = () => window.__mutationNavGen++;
        });
        activeRenderer = mutation.renderer;
      }

      await setOperationConfirm(page, false);
      await fillAndInvokeMutation(page, mutation);
      const cancelled = {
        confirmCalls: await page.evaluate(() => window.__confirmCalls.length),
        posts: context.requests.filter(request =>
          request.method === 'POST' && request.path === mutation.path)
      };
      assert.equal(cancelled.confirmCalls, 1, `${mutation.name}: cancellation must be confirmed once`);
      assert.equal(cancelled.posts.length, 0, `${mutation.name}: cancellation must send zero POST requests`);

      await setOperationConfirm(page, true);
      await fillAndInvokeMutation(page, mutation);
      const approved = {
        confirmCalls: await page.evaluate(() => window.__confirmCalls.length),
        posts: context.requests.filter(request =>
          request.method === 'POST' && request.path === mutation.path)
      };
      assert.equal(approved.confirmCalls, 1, `${mutation.name}: approval must be confirmed once`);
      assert.equal(approved.posts.length, 1, `${mutation.name}: approval must send exactly one POST request`);
      assert.deepEqual(approved.posts[0].json, mutation.body, `${mutation.name}: canonical request body`);
    }
  }, { routes: fixtureSet(mutationRoutes) });
});

test('all six mutations reject ambiguous HTTP 200 payloads without success toast or event', async () => {
  const invalidBodies = [
    { data: null },
    { data: {} },
    { data: { status: 'completed' } },
    {},
    { data: [] },
    { data: { status: 'bound' } }
  ];
  const mutationRoutes = Object.fromEntries(MUTATION_CASES.map((mutation, index) => [
    mutation.path,
    { status: 200, body: invalidBodies[index] }
  ]));

  await withPage(MODS, async (page, context) => {
    for (const mutation of MUTATION_CASES) {
      await boot(page, mutation.renderer);
      await setOperationConfirm(page, true);
      await fillAndInvokeMutation(page, mutation);

      const outcome = await page.evaluate(() => ({
        toasts: window.__toasts,
        events: window.__events
      }));
      assert.equal(outcome.toasts.length, 1, `${mutation.name}: ambiguous result must show one failure`);
      assert.equal(outcome.toasts[0].ok, false, `${mutation.name}: ambiguous result is not success`);
      assert.match(
        String(outcome.toasts[0].message),
        /성공 응답을 확인할 수 없습니다|success response could not be verified/i,
        `${mutation.name}: failure must explain that terminal success was not verified`
      );
      assert.deepEqual(outcome.events, [], `${mutation.name}: ambiguous result must add no success event`);
      assert.equal(
        context.requests.filter(request =>
          request.method === 'POST' && request.path === mutation.path).length,
        1,
        `${mutation.name}: validation happens on the single canonical POST result`
      );
    }
  }, { routes: fixtureSet(mutationRoutes) });
});

test('accelerator mutations use one shared in-flight lock and restore controls after completion', async () => {
  const delayedRoutes = fixtureSet({
    '/api/v1/dpdk/bind': async () => {
      await new Promise(resolve => setTimeout(resolve, 250));
      return { status: 200, body: { data: { status: 'bound' } } };
    },
    '/api/v1/dpdk/unbind': { status: 200, body: { data: { status: 'unbound' } } }
  });

  await withPage(MODS, async (page, context) => {
    await boot(page, 'renderDpdk');
    await setOperationConfirm(page, true);
    await page.evaluate(() => {
      document.getElementById('dpdk-pci').value = '0000:05:00.0';
      document.getElementById('dpdk-unbind-pci').value = '0000:06:00.0';
      window.__pendingD2Mutation = Promise.all([window.dpdkBind(), window.dpdkBind(), window.dpdkUnbind()]);
    });
    await page.waitForFunction(() => document.querySelector('[data-d2-action="dpdk-bind"]')?.getAttribute('aria-busy') === 'true');
    const busy = await page.evaluate(() => [...document.querySelectorAll('.d2-operations button')]
      .map(button => ({ disabled: button.disabled, busy: button.getAttribute('aria-busy') })));
    assert.ok(busy.length > 0);
    assert.ok(busy.every(button => button.disabled && button.busy === 'true'));

    await page.evaluate(() => window.__pendingD2Mutation);
    const posts = context.requests.filter(request => request.method === 'POST');
    assert.equal(posts.filter(request => request.path === '/api/v1/dpdk/bind').length, 1);
    assert.equal(posts.filter(request => request.path === '/api/v1/dpdk/unbind').length, 0);
    assert.equal(await page.evaluate(() => window.__confirmCalls.length), 1);
  }, { routes: delayedRoutes });
});

test('network rejection renders a retryable error and a failed mutation releases the one-shot lock', async () => {
  await withPage(MODS, async page => {
    await boot(page, 'renderDpdk');
    const readError = await page.evaluate(async () => {
      window.fetchGet = () => Promise.reject(new Error('network offline fixture'));
      await window.renderDpdk(document.getElementById('cb'));
      const alert = document.querySelector('#cb [role="alert"]');
      return { text: alert?.textContent || '', retry: Boolean(alert?.querySelector('button')) };
    });
    assert.match(readError.text, /network offline fixture/i);
    assert.equal(readError.retry, true);

    await boot(page, 'renderDpdk');
    await setOperationConfirm(page, true);
    const mutation = await page.evaluate(async () => {
      document.getElementById('dpdk-pci').value = '0000:05:00.0';
      window.fetchPost = () => Promise.reject(new Error('mutation network failure'));
      await window.dpdkBind();
      await window.dpdkBind();
      return {
        confirms: window.__confirmCalls.length,
        busy: [...document.querySelectorAll('.d2-operations button')]
          .some(button => button.getAttribute('aria-busy') === 'true'),
        toastText: window.__toasts.map(entry => entry.message).join(' ')
      };
    });
    assert.equal(mutation.confirms, 2, 'the second attempt must be allowed after the first failure');
    assert.equal(mutation.busy, false, 'finally must clear aria-busy');
    assert.match(mutation.toastText, /mutation network failure/i);
  }, { routes: fixtureSet() });
});

test('monitoring overview poll reapplies Configure AI Agent RBAC after direct rerender', async () => {
  const node = {
    node: 'edge-d2', ip: '192.0.2.20', cpu: 25, mem: 35, disk: 45, temp: 42,
    load1: 0.25, load5: 0.2, load15: 0.15, ram_total: 17179869184,
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

  await withPage(MODS, async page => {
    const states = await page.evaluate(nodeFixture => {
      window._DEBUG = false;
      window._L = (ko, en) => en || ko;
      window.t = key => key;
      window.fetchGet = () => Promise.resolve({ data: [] });
      window.fetchPost = () => Promise.resolve({ data: {} });
      window.currentTab = 'mon-overview';
      const roles = [null, 'VIEWER', 'OPERATOR', 'ADMIN'];
      const result = {};
      function paint() {
        window.renderMonOverview(
          document.getElementById('cb'), [nodeFixture], [], 0,
          nodeFixture.cpu, nodeFixture.mem, nodeFixture.disk, nodeFixture.ram_total
        );
      }
      function controlState() {
        const button = [...document.querySelectorAll('#cb [data-role="ADMIN"]')]
          .find(candidate => /Configure AI Agent/i.test(candidate.textContent));
        return {
          exists: Boolean(button),
          roleHidden: button?.classList.contains('role-hidden') || false,
          display: button ? getComputedStyle(button).display : null
        };
      }
      for (const role of roles) {
        window.currentUser = role ? { role } : null;
        paint();
        window.applyRoleVisibility(role);
        const initial = controlState();
                                                                
                                                                         
        paint();
        result[role || 'UNKNOWN'] = { initial, afterPoll: controlState() };
      }
      return result;
    }, node);

    for (const role of ['UNKNOWN', 'VIEWER', 'OPERATOR']) {
      assert.deepEqual(states[role].initial, {
        exists: true, roleHidden: true, display: 'none'
      }, `${role}: initial global role application must hide the ADMIN control`);
      assert.deepEqual(states[role].afterPoll, {
        exists: true, roleHidden: true, display: 'none'
      }, `${role}: poll rerender must remain fail-closed`);
    }
    assert.equal(states.ADMIN.initial.exists, true);
    assert.equal(states.ADMIN.initial.roleHidden, false);
    assert.notEqual(states.ADMIN.initial.display, 'none');
    assert.equal(states.ADMIN.afterPoll.exists, true);
    assert.equal(states.ADMIN.afterPoll.roleHidden, false);
    assert.notEqual(states.ADMIN.afterPoll.display, 'none');
  });
});

test('the canonical #/pool-info deep-link dispatches the product renderer', async () => {
  const navMods = ['ui/modules/ui.js', 'ui/modules/uxlib.js', 'ui/modules/help.js', 'ui/modules/nav.js'];
  await withPage(navMods, async page => {
    await page.evaluate(() => {
      window._DEBUG = false;
      window.currentTab = 'dashboard';
      window.vmList = [];
      window.selectedVmIndex = 0;
      window.destroyAllCharts = () => {};
      window.updateStatusBar = () => {};
      window.currentUser = { role: 'ADMIN' };
      window.renderPoolInfo = target => {
        window.__d2DeepLinkRender = (window.__d2DeepLinkRender || 0) + 1;
        target.textContent = 'POOL DEEP LINK';
      };
      history.replaceState(null, '', `${location.pathname}#/pool-info`);
      window.PCV.uxlib.navigateToHash();
    });
    await page.waitForFunction(() => window.__d2DeepLinkRender === 1);
    const state = await page.evaluate(() => ({
      tab: window.currentTab,
      hash: location.hash,
      text: document.getElementById('cb').textContent
    }));
    assert.deepEqual(state, { tab: 'pool-info', hash: '#/pool-info', text: 'POOL DEEP LINK' });
  });
});

test('low-density pages avoid viewport overflow at 1440/1024/768 and keep 768px touch targets', async () => {
  await withPage(MODS, async page => {
    for (const width of [1440, 1024, 768]) {
      await page.setViewport({ width, height: 900 });
      for (const renderer of ['renderPoolInfo', 'renderDpdk', 'renderSriov']) {
        await boot(page, renderer);
        const layout = await page.evaluate(() => {
          const root = document.documentElement;
          const controls = [...document.querySelectorAll('#cb input, #cb select, #cb button')]
            .filter(node => getComputedStyle(node).display !== 'none');
          const outside = controls.filter(node => {
            const rect = node.getBoundingClientRect();
            return rect.left < -0.5 || rect.right > window.innerWidth + 0.5;
          }).map(node => `${node.tagName.toLowerCase()}#${node.id}`);
          const undersized = window.innerWidth === 768
            ? [...document.querySelectorAll('#cb button')].filter(node => {
                const rect = node.getBoundingClientRect();
                return rect.width < 40 || rect.height < 40;
              }).map(node => node.textContent.trim())
            : [];
          return {
            documentWidth: Math.max(root.scrollWidth, document.body.scrollWidth),
            viewportWidth: root.clientWidth,
            outside,
            undersized
          };
        });

        assert.ok(
          layout.documentWidth <= layout.viewportWidth + 1,
          `${renderer} overflows at ${width}px (${layout.documentWidth} > ${layout.viewportWidth})`
        );
        assert.deepEqual(layout.outside, [], `${renderer} controls escape the ${width}px viewport`);
        assert.deepEqual(layout.undersized, [], `${renderer} has a touch target below 40px at 768px`);
      }
    }
  }, { routes: fixtureSet() });
});
