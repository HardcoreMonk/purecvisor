                                                                                                  
                                                                                       
                                                                      
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const MODS = [
  'ui/modules/ui.js',
  'ui/modules/uxlib.js',
  'ui/modules/endpoints.js',
  'ui/modules/modal-core.js',
  'ui/modules/vpc.js'
];

const VPC = {
  id: '11111111-1111-4111-8111-111111111111',
  tenant: 'acme',
  name: 'prod',
  egress_mode: 'nat',
  backend: 'linux',
  state: 'ACTIVE',
  revision: 3
};
const SUBNET = {
  id: '22222222-2222-4222-8222-222222222222',
  name: 'web',
  cidr: '10.60.10.0/24',
  gateway: '10.60.10.1',
  mtu: 1500,
  backend: 'linux',
  backend_ref: 'pcvs22222222222',
  bridge_name: 'pcvs22222222222',
  state: 'ACTIVE'
};
const ATTACHMENT = {
  id: '33333333-3333-4333-8333-333333333333',
  subnet_id: SUBNET.id,
  vm_name: 'web-prod',
  ip_address: '10.60.10.10',
  mac_address: '02:90:5b:01:02:03',
  state: 'ACTIVE'
};
const SERVICE = {
  id: '44444444-4444-4444-8444-444444444444',
  protocol: 'tcp',
  listen_address: '0.0.0.0',
  listen_port: 8443,
  target_ip: ATTACHMENT.ip_address,
  target_port: 443,
  allowed_sources_json: '["192.0.2.0/24"]',
  state: 'ACTIVE'
};

function routes(jobStatus = 'completed', createError = null, backends = [
  { id: 'linux', label: 'Linux bridge', ready: true, current_vpcs: 1, allocatable_vpcs: null },
  { id: 'ovn', label: 'OVN (Open vSwitch)', ready: false, current_vpcs: 0,
    allocatable_vpcs: 0, reason: 'br-int unavailable' }
], backendError = null) {
  return {
    '/api/v1/vpcs': record => record.method === 'POST'
      ? (createError
          ? { status: 409, body: { error: { code: -32004, message: createError } } }
          : { status: 200, body: { data: { status: 'accepted', job_id: 'job-vpc-create' } } })
      : { status: 200, body: { data: [VPC] } },
    '/api/v1/vpcs/status': {
      status: 200,
      body: { data: { healthy: true, reconcile_required: false, vpc_count: 1, subnet_count: 1, attachment_count: 1, service_publish_count: 1 } }
    },
    '/api/v1/vpcs/backends': backendError
      ? { status: 503, body: { error: { code: -32000, message: backendError } } }
      : { status: 200, body: { data: backends } },
    [`/api/v1/vpcs/${VPC.id}`]: record => record.method === 'DELETE'
      ? { status: 200, body: { data: { status: 'accepted', job_id: 'job-vpc-delete' } } }
      : { status: 200, body: { data: { ...VPC, subnets: [SUBNET], attachments: [ATTACHMENT], service_publishes: [SERVICE] } } },
    [`/api/v1/vpcs/${VPC.id}/egress`]: {
      status: 200,
      body: { data: { status: 'accepted', job_id: 'job-vpc-egress' } }
    },
    [`/api/v1/vpcs/${VPC.id}/subnets`]: {
      status: 200,
      body: { data: { status: 'accepted', job_id: 'job-vpc-subnet' } }
    },
    '/api/v1/vms': { status: 200, body: { data: [{ name: 'worker-stopped', state: 'shutoff' }] } },
    '/api/v1/vpc-attachments': {
      status: 200,
      body: { data: { status: 'accepted', job_id: 'job-vpc-attachment' } }
    },
    '/api/v1/vpc-services': {
      status: 200,
      body: { data: { status: 'accepted', job_id: 'job-vpc-service' } }
    },
    [`/api/v1/vpc-subnets/${SUBNET.id}`]: {
      status: 200,
      body: { data: { status: 'accepted', job_id: 'job-vpc-subnet-delete' } }
    },
    [`/api/v1/vpc-attachments/${ATTACHMENT.id}`]: {
      status: 200,
      body: { data: { status: 'accepted', job_id: 'job-vpc-attachment-delete' } }
    },
    [`/api/v1/vpc-services/${SERVICE.id}`]: {
      status: 200,
      body: { data: { status: 'accepted', job_id: 'job-vpc-service-delete' } }
    },
    '/api/v1/vpcs/reconcile': {
      status: 200,
      body: { data: { status: 'accepted', job_id: 'job-vpc-reconcile' } }
    },
    '/api/v1/jobs/job-vpc-create': {
      status: 200,
      body: { data: { id: 'job-vpc-create', status: jobStatus,
        detail: jobStatus === 'failed' ? 'Local VPC mutation running' : '{}',
        result: jobStatus === 'failed' ? '{"error":"fixture worker failure"}' : '{}' } }
    },
    '/api/v1/jobs/job-vpc-egress': {
      status: 200,
      body: { data: { id: 'job-vpc-egress', status: jobStatus,
        detail: jobStatus === 'failed' ? 'Local VPC mutation running' : '{}',
        result: jobStatus === 'failed' ? '{"error":"fixture worker failure"}' : '{}' } }
    },
    ...Object.fromEntries([
      'subnet', 'attachment', 'service', 'subnet-delete', 'attachment-delete',
      'service-delete', 'reconcile', 'delete'
    ].map(name => [`/api/v1/jobs/job-vpc-${name}`, {
      status: 200,
      body: { data: { id: `job-vpc-${name}`, status: 'completed', detail: '{}' } }
    }]))
  };
}

async function boot(page, width = 1280, role = 'ADMIN') {
  await page.setViewport({ width, height: 900 });
  await page.evaluate(async effectiveRole => {
    window.authToken = 'test-token';
    window.currentUser = { role: effectiveRole };
    window._L = (ko, en) => en;
    window.__toasts = [];
    window.toast = (message, ok) => window.__toasts.push({ message, ok });
    window.addEvt = () => {};
    window.unwrapData = value => value?.data !== undefined ? value.data : (value?.result !== undefined ? value.result : value);
    window.unwrapList = value => {
      const data = window.unwrapData(value);
      return Array.isArray(data) ? data : [];
    };
    const call = (url, options = {}) => fetch(url, options).then(response => response.json());
    window.fetchGet = url => call(url, { headers: { Authorization: 'Bearer test-token' } });
    window.fetchPost = (url, body) => call(url, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
    window.fetchDelete = (url, body) => call(url, { method: 'DELETE', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
    const root = document.getElementById('cb');
    root.classList.add('cb');
    await window.renderVpcs(root);
  }, role);
  await page.waitForSelector('.vpc-detail-summary');
}

async function fillInitialSubnet(page, name = 'web', cidr = '10.60.20.0/24') {
  await page.type('#vpc-create-subnet-name', name);
  await page.type('#vpc-create-subnet-cidr', cidr);
}

test('VPC inventory keeps aggregate and child resources in one selected context', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    const view = await page.evaluate(() => ({
      title: document.querySelector('.pagehead-title')?.textContent,
      summaries: [...document.querySelectorAll('.vpc-summary-value')].map(node => node.textContent),
      selected: document.querySelector('.vpc-row-selected .vpc-select')?.textContent,
      facts: document.querySelector('.vpc-detail-facts')?.textContent,
      childHeadings: [...document.querySelectorAll('.vpc-child-section h3')].map(node => node.textContent),
      vm: document.querySelectorAll('.vpc-child-section')[1]?.textContent,
      service: document.querySelectorAll('.vpc-child-section')[2]?.textContent,
      unsafeHtml: document.querySelectorAll('script[src=x]').length
    }));
    assert.equal(view.title, 'Local VPC');
    assert.deepEqual(view.summaries, ['Healthy', '1', '1 / 1', '1']);
    assert.equal(view.selected, 'prod');
    assert.match(view.facts, new RegExp(VPC.id));
    assert.deepEqual(view.childHeadings, ['Subnets', 'VM attachments', 'Service Publish']);
    assert.match(view.vm, /web-prod/);
    assert.match(view.service, /8443/);
    assert.equal(view.unsafeHtml, 0);
  }, { routes: routes() });
});

test('VIEWER keeps read-only VPC context while mutation controls stay hidden', async () => {
  await withPage(MODS, async page => {
    await boot(page, 1280, 'VIEWER');
    const visibility = await page.evaluate(() => ({
      inventoryVisible: !!document.querySelector('.vpc-row-selected'),
      mutationCount: document.querySelectorAll('[data-role]').length,
      visibleMutations: [...document.querySelectorAll('[data-role]')]
        .filter(node => getComputedStyle(node).display !== 'none').length
    }));
    assert.equal(visibility.inventoryVisible, true);
    assert.ok(visibility.mutationCount > 0);
    assert.equal(visibility.visibleMutations, 0);
  }, { routes: routes() });
});

test('VPC create sends the dedicated REST body and waits for terminal completion', async () => {
  await withPage(MODS, async (page, { requests }) => {
    await boot(page);
    await page.click('.pagehead-actions .btn-primary');
    await page.type('#vpc-create-name', 'edge-prod');
    await page.type('#vpc-create-tenant', 'tenant-a');
    await page.select('#vpc-create-egress', 'isolated');
    await fillInitialSubnet(page);
    await page.click('[data-vpc-submit]');
    await page.waitForFunction(() => !document.querySelector('dialog[open]'));
    await page.waitForFunction(() => window.__toasts.some(item => item.ok));

    const create = requests.find(request => request.method === 'POST' && request.path === '/api/v1/vpcs');
    assert.deepEqual(create?.json, {
      name: 'edge-prod', tenant: 'tenant-a', egress_mode: 'isolated', backend: 'linux',
      subnet_name: 'web', subnet_cidr: '10.60.20.0/24', subnet_mtu: 1500
    });
    assert.ok(requests.some(request => request.path === '/api/v1/jobs/job-vpc-create'));
    assert.ok(await page.evaluate(() => window.__toasts.some(item => item.ok && /completed/.test(item.message))));
  }, { routes: routes() });
});

test('backend selector exposes readiness and sends an enabled OVN choice', async () => {
  const available = [
    { id: 'linux', label: 'Linux bridge', ready: true, current_vpcs: 1, allocatable_vpcs: null },
    { id: 'ovn', label: 'OVN (Open vSwitch)', ready: true, current_vpcs: 2, allocatable_vpcs: 14 }
  ];
  await withPage(MODS, async (page, { requests }) => {
    await boot(page);
    await page.click('.pagehead-actions .btn-primary');
    await page.select('#vpc-create-backend', 'ovn');
    const note = await page.$eval('#vpc-create-backend-status', node => node.textContent);
    assert.match(note, /current 2/);
    assert.match(note, /allocatable 14/);
    await page.type('#vpc-create-name', 'ovn-prod');
    await page.type('#vpc-create-tenant', 'tenant-a');
    await fillInitialSubnet(page, 'ovn-web', '10.61.20.0/24');
    await page.click('[data-vpc-submit]');
    await page.waitForFunction(() => !document.querySelector('dialog[open]'));
    const create = requests.find(request => request.method === 'POST' && request.path === '/api/v1/vpcs');
    assert.equal(create?.json.backend, 'ovn');
  }, { routes: routes('completed', null, available) });
});

test('unavailable OVN backend is disabled with its exact reason visible and associated', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    await page.click('.pagehead-actions .btn-primary');
    const state = await page.evaluate(() => ({
      disabled: document.querySelector('#vpc-create-backend option[value="ovn"]')?.disabled,
      option: document.querySelector('#vpc-create-backend option[value="ovn"]')?.textContent,
      currentNote: document.querySelector('#vpc-create-backend-status')?.textContent,
      unavailableNote: document.querySelector('#vpc-create-backend-unavailable')?.textContent,
      backendDescriptions: document.querySelector('#vpc-create-backend')?.getAttribute('aria-describedby'),
      mtuDescription: document.querySelector('#vpc-create-subnet-mtu')?.getAttribute('aria-describedby'),
      mtuHelp: document.querySelector('#vpc-create-subnet-mtu-help')?.textContent
    }));
    assert.equal(state.disabled, true);
    assert.match(state.option, /unavailable/);
    assert.match(state.currentNote, /Linux bridge/);
    assert.match(state.unavailableNote, /OVN \(Open vSwitch\).*br-int unavailable/);
    assert.match(state.backendDescriptions, /vpc-create-backend-unavailable/);
    assert.equal(state.mtuDescription, 'vpc-create-subnet-mtu-help');
    assert.match(state.mtuHelp, /allowed range 68–9216/);
  }, { routes: routes() });
});

test('backend capability failure stays visible while Linux fallback remains usable', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    await page.click('.pagehead-actions .btn-primary');
    const result = await page.evaluate(() => ({
      warning: document.querySelector('.vpc-backend-warning')?.textContent,
      role: document.querySelector('.vpc-backend-warning')?.getAttribute('role'),
      options: [...document.querySelectorAll('#vpc-create-backend option')].map(option => option.value),
      selected: document.querySelector('#vpc-create-backend')?.value,
      submitDisabled: document.querySelector('[data-vpc-submit]')?.disabled
    }));
    assert.match(result.warning, /capability service unavailable/);
    assert.equal(result.role, 'alert');
    assert.deepEqual(result.options, ['linux']);
    assert.equal(result.selected, 'linux');
    assert.equal(result.submitDisabled, false);
  }, { routes: routes('completed', null, undefined, 'capability service unavailable') });
});

test('VPC and subnet forms expose truthful count and address capacity before submit', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    await page.click('.pagehead-actions .btn-primary');
    const vpcContract = await page.$$eval('.vpc-form > .vpc-contract-note',
      nodes => nodes[nodes.length - 1].textContent);
    assert.match(vpcContract, /Current VPCs 1 · no fixed product limit/);
    assert.match(vpcContract, /addressless/);
    assert.match(vpcContract, /253 addresses/);
    await page.type('#vpc-create-subnet-cidr', '10.60.20.0/24');
    const initialCapacity = await page.$eval('#vpc-create-subnet-capacity', node => node.textContent);
    assert.match(initialCapacity, /Gateway 10\.60\.20\.1/);
    assert.match(initialCapacity, /VM 10\.60\.20\.2–10\.60\.20\.254/);
    assert.match(initialCapacity, /assignable 253 addresses/);
    await page.click('.vpc-form-actions .btn-soft');

    await page.evaluate(() => document.querySelectorAll('.vpc-child-section')[0].querySelector('.btn-primary').click());
    await page.type('#vpc-subnet-cidr', '10.60.20.0/24');
    const capacity = await page.$eval('#vpc-subnet-capacity', node => node.textContent);
    assert.match(capacity, /Gateway 10\.60\.20\.1/);
    assert.match(capacity, /VM 10\.60\.20\.2–10\.60\.20\.254/);
    assert.match(capacity, /assignable 253 addresses/);
  }, { routes: routes() });
});

test('egress mutation carries tenant and optimistic revision to its REST route', async () => {
  await withPage(MODS, async (page, { requests }) => {
    await boot(page);
    await page.click('.vpc-detail-actions .btn-soft');
    await page.select('#vpc-egress-mode', 'isolated');
    await page.click('[data-vpc-submit]');
    await page.waitForFunction(() => !document.querySelector('dialog[open]'));

    const change = requests.find(request => request.method === 'POST' && request.path.endsWith('/egress'));
    assert.deepEqual(change?.json, { tenant: 'acme', egress_mode: 'isolated', expected_revision: 3 });
    assert.ok(requests.some(request => request.path === '/api/v1/jobs/job-vpc-egress'));
  }, { routes: routes() });
});

test('subnet, stopped-VM attachment, and service publish forms keep their domain payloads', async () => {
  await withPage(MODS, async (page, { requests }) => {
    await boot(page);

    await page.evaluate(() => document.querySelectorAll('.vpc-child-section')[0].querySelector('.btn-primary').click());
    await page.type('#vpc-subnet-name', 'api');
    await page.type('#vpc-subnet-cidr', '10.60.20.0/24');
    await page.click('[data-vpc-submit]');
    await page.waitForFunction(() => !PCV.vpc.state.busy && !document.querySelector('dialog[open]'));

    await page.evaluate(() => document.querySelectorAll('.vpc-child-section')[1].querySelector('.btn-primary').click());
    await page.waitForSelector('#vpc-attach-vm');
    await page.type('#vpc-attach-vm', 'worker-stopped');
    await page.type('#vpc-attach-ip', '10.60.10.20');
    await page.click('[data-vpc-submit]');
    await page.waitForFunction(() => !PCV.vpc.state.busy && !document.querySelector('dialog[open]'));

    await page.evaluate(() => document.querySelectorAll('.vpc-child-section')[1].querySelector('.vpc-row-actions .btn-soft').click());
    await page.type('#vpc-publish-listen', '9443');
    await page.type('#vpc-publish-target', '443');
    await page.type('#vpc-publish-sources', '192.0.2.0/24\n198.51.100.0/24');
    await page.click('[data-vpc-submit]');
    await page.waitForFunction(() => !PCV.vpc.state.busy && !document.querySelector('dialog[open]'));

    const subnetCall = requests.find(request => request.method === 'POST' && request.path.endsWith('/subnets'));
    const attachmentCall = requests.find(request => request.method === 'POST' && request.path === '/api/v1/vpc-attachments');
    const serviceCall = requests.find(request => request.method === 'POST' && request.path === '/api/v1/vpc-services');
    assert.deepEqual(subnetCall?.json, {
      tenant: 'acme', name: 'api', cidr: '10.60.20.0/24', mtu: 1500, expected_revision: 3
    });
    assert.deepEqual(attachmentCall?.json, {
      tenant: 'acme', subnet_id: SUBNET.id, vm: 'worker-stopped', ip_address: '10.60.10.20'
    });
    assert.deepEqual(serviceCall?.json, {
      tenant: 'acme', attachment_id: ATTACHMENT.id, protocol: 'tcp', listen_address: '0.0.0.0',
      listen_port: 9443, target_port: 443, allowed_sources: ['192.0.2.0/24', '198.51.100.0/24']
    });
  }, { routes: routes() });
});

test('reconcile and reverse-order removals use dedicated REST routes with tenant scope', async () => {
  await withPage(MODS, async (page, { requests }) => {
    await boot(page);
    await page.evaluate(() => { window.customConfirm = () => Promise.resolve(true); });

    const run = async selectorScript => {
      const before = await page.evaluate(() => window.__toasts.length);
      await page.evaluate(selectorScript);
      await page.waitForFunction(count => window.__toasts.length > count, {}, before);
      await page.waitForFunction(() => PCV.vpc.state.busy === null);
    };
    await run(() => document.querySelector('.pagehead-actions .btn-soft').click());
    await run(() => document.querySelectorAll('.vpc-child-section')[2].querySelector('.btn-r').click());
    await run(() => document.querySelectorAll('.vpc-child-section')[1].querySelector('.btn-r').click());
    await run(() => document.querySelectorAll('.vpc-child-section')[0].querySelector('.btn-r').click());
    await run(() => document.querySelector('.vpc-list-card .btn-r').click());

    const writes = requests.filter(request => request.method === 'POST' || request.method === 'DELETE');
    assert.ok(writes.some(request => request.method === 'POST' && request.path === '/api/v1/vpcs/reconcile' && JSON.stringify(request.json) === '{}'));
    for (const path of [
      `/api/v1/vpc-services/${SERVICE.id}`,
      `/api/v1/vpc-attachments/${ATTACHMENT.id}`,
      `/api/v1/vpc-subnets/${SUBNET.id}`,
      `/api/v1/vpcs/${VPC.id}`
    ]) {
      const removal = writes.find(request => request.method === 'DELETE' && request.path === path);
      assert.deepEqual(removal?.json, { tenant: 'acme' }, `missing tenant-scoped DELETE ${path}`);
    }
  }, { routes: routes() });
});

test('failed terminal job remains a failure and keeps the mutation form recoverable', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    await page.click('.pagehead-actions .btn-primary');
    await page.type('#vpc-create-name', 'edge-fail');
    await page.type('#vpc-create-tenant', 'tenant-a');
    await fillInitialSubnet(page);
    await page.click('[data-vpc-submit]');
    await page.waitForFunction(() => document.querySelector('.vpc-form-error')?.textContent === 'fixture worker failure');
    await page.waitForFunction(() => document.querySelector('[data-vpc-submit]')?.disabled === false);
    const result = await page.evaluate(() => ({
      open: !!document.querySelector('dialog[open]'),
      submitDisabled: document.querySelector('[data-vpc-submit]')?.disabled,
      error: document.querySelector('.vpc-form-error')?.textContent,
      name: document.querySelector('#vpc-create-name')?.value,
      tenant: document.querySelector('#vpc-create-tenant')?.value,
      subnetName: document.querySelector('#vpc-create-subnet-name')?.value,
      subnetCidr: document.querySelector('#vpc-create-subnet-cidr')?.value,
      backend: document.querySelector('#vpc-create-backend')?.value,
      errorFocused: document.activeElement === document.querySelector('.vpc-form-error'),
      toasts: window.__toasts
    }));
    assert.equal(result.open, true);
    assert.equal(result.submitDisabled, false);
    assert.equal(result.error, 'fixture worker failure');
    assert.equal(result.name, 'edge-fail');
    assert.equal(result.tenant, 'tenant-a');
    assert.equal(result.subnetName, 'web');
    assert.equal(result.subnetCidr, '10.60.20.0/24');
    assert.equal(result.backend, 'linux');
    assert.equal(result.errorFocused, true);
    assert.equal(result.toasts.some(item => !item.ok), false);
    assert.equal(result.toasts.some(item => item.ok), false);
  }, { routes: routes('failed') });
});

test('immediate API rejection stays readable and is rendered as text inside the open modal', async () => {
  const serverMessage = '<img src=x onerror=alert(1)> subnet overlaps host route';
  await withPage(MODS, async page => {
    await boot(page);
    await page.click('.pagehead-actions .btn-primary');
    await page.type('#vpc-create-name', 'edge-conflict');
    await page.type('#vpc-create-tenant', 'tenant-a');
    await fillInitialSubnet(page);
    await page.click('[data-vpc-submit]');
    await page.waitForFunction(message => document.querySelector('.vpc-form-error')?.textContent === message, {}, serverMessage);
    const result = await page.evaluate(() => ({
      open: !!document.querySelector('dialog[open]'),
      error: document.querySelector('.vpc-form-error')?.textContent,
      injectedImageCount: document.querySelectorAll('.vpc-form-error img').length,
      name: document.querySelector('#vpc-create-name')?.value,
      tenant: document.querySelector('#vpc-create-tenant')?.value,
      subnetName: document.querySelector('#vpc-create-subnet-name')?.value,
      subnetCidr: document.querySelector('#vpc-create-subnet-cidr')?.value,
      submitDisabled: document.querySelector('[data-vpc-submit]')?.disabled,
      failureToasts: window.__toasts.filter(item => !item.ok).length
    }));
    assert.deepEqual(result, {
      open: true,
      error: serverMessage,
      injectedImageCount: 0,
      name: 'edge-conflict',
      tenant: 'tenant-a',
      subnetName: 'web',
      subnetCidr: '10.60.20.0/24',
      submitDisabled: false,
      failureToasts: 0
    });
  }, { routes: routes('completed', serverMessage) });
});

test('1024/768/480px views avoid document overflow and preserve labelled mobile cards', async () => {
  await withPage(MODS, async page => {
    await boot(page, 1024);
    const layouts = [];
    for (const width of [1024, 768, 480]) {
      await page.setViewport({ width, height: 900 });
      layouts.push(await page.evaluate(async currentWidth => {
        await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
        const grid = document.querySelector('.vpc-summary-grid');
        const first = document.querySelector('.vpc-table.card-mobile tbody tr');
        const actionButtons = [...document.querySelectorAll('.vpc-row-actions .btn')];
        return {
          width: currentWidth,
          columns: getComputedStyle(grid).gridTemplateColumns.split(' ').length,
          rowDisplay: getComputedStyle(first).display,
          labels: [...first.querySelectorAll('td')].map(cell => cell.dataset.label),
          minActionHeight: Math.min(...actionButtons.map(button => button.getBoundingClientRect().height)),
          overflow: document.documentElement.scrollWidth - document.documentElement.clientWidth
        };
      }, width));
    }
    assert.deepEqual(layouts.map(layout => layout.columns), [2, 1, 1]);
    assert.deepEqual(layouts.map(layout => layout.rowDisplay), ['table-row', 'block', 'block']);
    assert.ok(layouts[2].labels.includes('Name') && layouts[2].labels.includes('Actions'));
    assert.ok(layouts.slice(1).every(layout => layout.minActionHeight >= 40));
    assert.ok(layouts.every(layout => layout.overflow <= 1), JSON.stringify(layouts));
  }, { routes: routes() });
});

test('compound VPC and subnet modal stays grouped and scrollable at 1024/768/480px', async () => {
  await withPage(MODS, async page => {
    await boot(page, 1024);
    await page.click('.pagehead-actions .btn-primary');
    const layouts = [];
    for (const width of [1024, 768, 480]) {
      await page.setViewport({ width, height: 900 });
      layouts.push(await page.evaluate(async currentWidth => {
        await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
        const dialog = document.querySelector('dialog[open]');
        const grid = document.querySelector('.vpc-form-grid');
        const actions = [...document.querySelectorAll('.vpc-form-actions .btn')];
        return {
          width: currentWidth,
          legends: [...document.querySelectorAll('.vpc-form-group legend')].map(node => node.textContent),
          columns: getComputedStyle(grid).gridTemplateColumns.split(' ').length,
          dialogOverflow: dialog.scrollWidth - dialog.clientWidth,
          documentOverflow: document.documentElement.scrollWidth - document.documentElement.clientWidth,
          dialogHeight: dialog.getBoundingClientRect().height,
          viewportHeight: innerHeight,
          minActionHeight: Math.min(...actions.map(button => button.getBoundingClientRect().height))
        };
      }, width));
    }
    assert.deepEqual(layouts.map(layout => layout.legends), [
      ['VPC', 'Initial subnet'], ['VPC', 'Initial subnet'], ['VPC', 'Initial subnet']
    ]);
    assert.deepEqual(layouts.map(layout => layout.columns), [2, 1, 1]);
    assert.ok(layouts.every(layout => layout.dialogOverflow <= 1 && layout.documentOverflow <= 1), JSON.stringify(layouts));
    assert.ok(layouts.every(layout => layout.dialogHeight <= layout.viewportHeight * 0.91), JSON.stringify(layouts));
    assert.ok(layouts[2].minActionHeight >= 40);
  }, { routes: routes() });
});
