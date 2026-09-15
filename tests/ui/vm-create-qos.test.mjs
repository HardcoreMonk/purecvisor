




import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

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

test('VM 생성 3단계는 명시적인 QoS 기본값·단위·label 계약을 노출한다', async () => {
  await withPage(VM_CREATE_MODS, async page => {
    await openCreateStep3(page);
    const contract = await page.evaluate(() => {
      const min = document.getElementById('wqmin');
      const max = document.getElementById('wqmax');
      const help = document.getElementById('wq-help');
      return {
        min: {
          label: document.querySelector('label[for="wqmin"]')?.textContent.trim(),
          value: min.value,
          min: min.min,
          max: min.max,
          step: min.step,
          required: min.required,
          describedBy: min.getAttribute('aria-describedby'),
        },
        max: {
          label: document.querySelector('label[for="wqmax"]')?.textContent.trim(),
          value: max.value,
          min: max.min,
          max: max.max,
          step: max.step,
          required: max.required,
          describedBy: max.getAttribute('aria-describedby'),
        },
        help: help.textContent.trim(),
      };
    });

    assert.deepEqual(contract.min, {
      label: 'QoS 최소', value: '0', min: '0', max: '4294967295', step: '1',
      required: true, describedBy: 'wq-help',
    });
    assert.deepEqual(contract.max, {
      label: 'QoS 최대', value: '1000', min: '1', max: '4294967295', step: '1',
      required: true, describedBy: 'wq-help',
    });
    assert.match(contract.help, /Mbit\/s/);
    assert.match(contract.help, /브리지 VM 필수/);
    assert.match(contract.help, /최소 ≤ 최대/);
  }, { routes: ROUTES });
});

test('VM 생성 POST는 사용자가 확인한 QoS 최소·최대 정수를 그대로 보낸다', async () => {
  await withPage(VM_CREATE_MODS, async (page, { requests }) => {
    await openCreateStep3(page);
    await page.evaluate(async () => {
      document.getElementById('wqmin').value = '25';
      document.getElementById('wqmax').value = '800';
      await window.doCreate();
    });
    const create = requests.find(record => record.method === 'POST' && record.path === '/api/v1/vms');
    assert.ok(create, 'VM create POST must be sent');
    assert.equal(create.json.name, 'qos-contract-vm');
    assert.equal(create.json.network_bridge, 'pcvnat0');
    assert.equal(create.json.qos_min_mbps, 25);
    assert.equal(create.json.qos_max_mbps, 800);
  }, { routes: ROUTES });
});

test('잘못된 QoS 값은 modal과 입력을 보존하며 요청을 보내지 않는다', async () => {
  await withPage(VM_CREATE_MODS, async (page, { requests }) => {
    const cases = [
      { min: '-1', max: '1000', message: 'QoS 최소값은 0~4294967295 사이의 정수여야 합니다' },
      { min: '0', max: '0', message: 'QoS 최대값은 1~4294967295 사이의 정수여야 합니다' },
      { min: '1001', max: '1000', message: 'QoS 최소값은 최대값보다 클 수 없습니다' },
      { min: '1.5', max: '1000', message: 'QoS 최소값은 0~4294967295 사이의 정수여야 합니다' },
      { min: '0', max: '4294967296', message: 'QoS 최대값은 1~4294967295 사이의 정수여야 합니다' },
    ];

    for (const entry of cases) {
      await openCreateStep3(page);
      const state = await page.evaluate(async ({ min, max }) => {
        document.getElementById('wqmin').value = min;
        document.getElementById('wqmax').value = max;
        await window.doCreate();
        const errors = [...document.querySelectorAll('.toast.t-er')];
        return {
          dialogOpen: !!document.querySelector('dialog.modal[open]'),
          min: document.getElementById('wqmin')?.value,
          max: document.getElementById('wqmax')?.value,
          error: errors.at(-1)?.textContent || '',
        };
      }, entry);
      assert.equal(state.dialogOpen, true);
      assert.equal(state.min, entry.min);
      assert.equal(state.max, entry.max);
      assert.match(state.error, new RegExp(entry.message));
    }

    assert.equal(
      requests.filter(record => record.method === 'POST' && record.path === '/api/v1/vms').length,
      0,
    );
  }, { routes: ROUTES });
});

test('QoS 행을 추가한 생성 modal은 480px에서 가로 overflow 없이 action에 도달한다', async () => {
  await withPage(VM_CREATE_MODS, async page => {
    await page.setViewport({ width: 480, height: 900 });
    await openCreateStep3(page);
    const layout = await page.evaluate(() => {
      const dialog = document.querySelector('dialog.modal[open]');
      const min = document.getElementById('wqmin').getBoundingClientRect();
      const max = document.getElementById('wqmax').getBoundingClientRect();
      const action = [...dialog.querySelectorAll('button')].find(button => button.textContent.includes('VM 생성'));
      dialog.scrollTop = dialog.scrollHeight;
      const dialogRect = dialog.getBoundingClientRect();
      const actionRect = action.getBoundingClientRect();
      return {
        documentOverflow: document.documentElement.scrollWidth > innerWidth,
        inputsInside: min.left >= dialogRect.left && min.right <= dialogRect.right
          && max.left >= dialogRect.left && max.right <= dialogRect.right,
        actionVisibleAfterScroll: actionRect.top >= dialogRect.top && actionRect.bottom <= dialogRect.bottom,
      };
    });
    assert.deepEqual(layout, {
      documentOverflow: false,
      inputsInside: true,
      actionVisibleAfterScroll: true,
    });
  }, { routes: ROUTES });
});
