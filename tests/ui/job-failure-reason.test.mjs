import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const BASE_MSG = "Base image '/x.raw' is missing, not a regular file, or changed after admission";
const CASES = [
  [{ status: 'failed', result: BASE_MSG, detail: 'VM creation started' }, BASE_MSG],
  [{ status: 'failed', result: '{"error":"export disk read failed"}', detail: 'Converting 40%' }, 'export disk read failed'],
  [{ status: 'failed', result: '{"message":"vpc conflict"}', detail: 'running' }, 'vpc conflict'],
  [{ status: 'failed', result: '"import refused"', detail: 'running' }, 'import refused'],
  [{ status: 'failed', result: '{}', detail: 'Local VPC mutation running' }, 'Local VPC mutation running'],
  [{ status: 'failed', result: '   ', detail: 'detail after blank' }, 'detail after blank'],
  [{ status: 'failed', result: '"   "', detail: 'detail after blank literal' }, 'detail after blank literal'],
  [{ status: 'failed', result: '{"error":{"message":"x"}}', detail: 'nested falls through' }, 'nested falls through'],
  [{ status: 'failed', result: '[]', detail: 'array falls through' }, 'array falls through'],
  [{ status: 'failed', result: 'null', detail: 'null literal falls through' }, 'null literal falls through'],
  [{ status: 'failed', result: 'true', detail: 'bool falls through' }, 'bool falls through'],
  [{ status: 'failed', result: '{"error":"  ","message":"second key"}' }, 'second key'],
  [{ status: 'failed', result: null, detail: null }, 'failed'],
  [{ status: 'interrupted', detail: 'Execution interrupted; actual outcome unknown' }, 'Execution interrupted; actual outcome unknown'],
];

test('jobFailureReason 표', async () => {
  await withPage(['ui/modules/endpoints.js', 'ui/modules/api.js'], async page => {
    const got = await page.evaluate(cases => cases.map(([job]) => PCV.api.jobFailureReason(job)), CASES);
    assert.deepEqual(got, CASES.map(c => c[1]));
  });
});

test('waitForJob은 옵션 없이 result 사유를 던진다', async () => {
  await withPage(['ui/modules/endpoints.js', 'ui/modules/api.js'], async page => {
    const message = await page.evaluate(async job => {
      window.fetch = async () => ({ ok: true, status: 200, json: async () => ({ data: job }) });
      try { await PCV.api.waitForJob('job-1', { attempts: 1, interval: 0 }); return 'no throw'; }
      catch (e) { return e.message; }
    }, CASES[0][0]);
    assert.equal(message, BASE_MSG);
    assert.ok(!message.includes('VM creation started'));
  });
});

const VM_CREATE_MODS = [
  'ui/i18n.js',
  'ui/modules/endpoints.js', 'ui/modules/api.js', 'ui/modules/ui.js',
  'ui/modules/filter-state.js', 'ui/modules/uxlib.js',
  'ui/modules/modal-core.js', 'ui/modules/modal.js',
  'ui/modules/vm-lifecycle.js',
];

const ROUTES = {
  '/api/v1/config/daemon': {
    status: 200,
    body: { data: { storage: { zvol_pool: 'pcvpool/vms', image_dir: '/var/lib/libvirt/images' } } },
  },
  '/api/v1/storage/pools': { status: 200, body: { data: [] } },
  '/api/v1/networks': {
    status: 200,
    body: { data: [{ name: 'pcvnat0', mode: 'nat', ip_cidr: '10.78.0.1/24', state: 'up' }] },
  },
  '/api/v1/vms': record => ({
    status: 200,
    body: record.method === 'POST'
      ? { accepted: true, job_id: 'job-vm-qos', status: 'accepted' }
      : { data: [{ name: 'qos-contract-vm', state: 'shutoff' }] },
  }),
  '/api/v1/jobs/job-vm-qos': { status: 200, body: { data: CASES[0][0] } },
};

async function openCreateStep3(page, name = 'qos-contract-vm') {
  await page.evaluate(vmName => {
    I18N.setLang('ko');
    window._L = (ko, _en) => ko;
    window.authToken = 'test-token';
    window.currentUser = { username: 'operator', role: 'operator' };
    window.vmList = [];
    window.loadAll = () => {};
    window.render = () => {};
    window.addEvt = () => {};
    if (!document.getElementById('toasts')) {
      const toasts = document.createElement('div');
      toasts.id = 'toasts';
      document.body.appendChild(toasts);
    }
    window.showCreate();
    document.getElementById('wn').value = vmName;
    window.wizGo(2);
    window.wizGo(3);
  }, name);
  await page.waitForSelector('#wqmax', { visible: true });
  await page.waitForFunction(() => document.getElementById('wb')?.value === 'pcvnat0');
}

test('VM 생성 실패 토스트에 서버 사유가 보인다', async () => {
  await withPage(VM_CREATE_MODS, async page => {
    await openCreateStep3(page);
    await page.evaluate(async () => { await window.doCreate(); });
    await page.waitForFunction(
      () => (document.getElementById('toasts')?.textContent || '').includes("Base image '/x.raw' is missing"),
      { timeout: 5000 });
    const text = await page.evaluate(() => document.getElementById('toasts').textContent);
    assert.ok(!text.includes('VM creation started'));
  }, { routes: ROUTES });
});
