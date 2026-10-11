





import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const MODS = [
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
  'ui/modules/help.js',
  'ui/modules/nav.js',
  'ui/modules/theme.js',
  'ui/modules/accounts.js',
  'ui/modules/advanced.js',
  'ui/modules/selfhealing.js',
  'ui/modules/mobile.js',
  'ui/app.js',
];

const VMS = [
  { name: 'web-01', uuid: 'u-1', state: 'running', live_cpu_pct: 12 },
  { name: 'db-01', uuid: 'u-2', state: 'shutoff' },
];



async function boot(page, port) {
  await page.evaluate(async () => {
    const html = await fetch('/ui/index.html').then(response => response.text());
    const parsed = new DOMParser().parseFromString(html, 'text/html');
    document.body.innerHTML = parsed.body.innerHTML;
  });
  for (const moduleFile of MODS) {
    await page.addScriptTag({ url: `http://127.0.0.1:${port}/${moduleFile}` });
  }
  await page.evaluate(() => {
    I18N.setLang('ko');
    window.authToken = 'test-token';
    const host = document.createElement('div');
    host.id = 'vm-left-fixture';
    const el = PCV.uxlib.el;
    host.appendChild(el('span', { id: 'vc' }, '0'));
    host.appendChild(el('div', { id: 'vl' }));
    host.appendChild(el('button', { id: 'bbtn', style: 'display:none' }));
    document.body.appendChild(host);
  });
}

function listState(page) {
  return page.evaluate(() => {
    const vl = document.getElementById('vl');
    const err = vl.querySelector('.vm-list-error');
    return {
      vmCount: window.vmList.length,
      rows: vl.querySelectorAll('.vi').length,
      errorText: err ? err.textContent : '',
      errorRole: err ? err.getAttribute('role') : null,
      emptyCta: vl.textContent.includes('VM이 없습니다'),
      count: document.getElementById('vc').textContent,
    };
  });
}

for (const failure of [
  { status: 503, body: { error: { code: -32000, message: 'Circuit breaker open' } }, reason: 'Circuit breaker open' },
  { status: 403, body: { error: { code: 'FORBIDDEN', message: 'Insufficient role' } }, reason: 'Insufficient role' },
]) {
  test(`첫 /vms 조회가 ${failure.status} 이면 빈 목록이 아니라 오류 상태를 그린다`, async () => {
    await withPage([], async (page, { port }) => {
      const pageErrors = [];
      page.on('pageerror', error => pageErrors.push(String(error)));
      await boot(page, port);
      await page.evaluate(async () => { await window.loadAll(); });
      const state = await listState(page);

      assert.deepEqual(pageErrors, []);
      assert.equal(state.emptyCta, false, '오류를 "VM이 없습니다" 빈 상태로 그리면 안 된다');
      assert.match(state.errorText, /VM 목록을 불러오지 못했습니다/);
      assert.ok(state.errorText.includes(failure.reason), '서버 사유를 함께 보여야 한다');
      assert.equal(state.errorRole, 'alert');
      assert.equal(state.count, '—', 'VM 개수를 0 으로 단정하면 안 된다');
    }, { routes: { '/api/v1/vms': { status: failure.status, body: failure.body } } });
  });
}

test('정상 조회 뒤 503 이 오면 직전 목록을 유지하고 오류를 함께 표시하며, 복구되면 오류가 사라진다', async () => {
  let call = 0;
  await withPage([], async (page, { port }) => {
    await boot(page, port);
    await page.evaluate(async () => { await window.loadAll(); });
    const ok = await listState(page);
    await page.evaluate(async () => { await window.loadAll(); });
    const failed = await listState(page);
    await page.evaluate(async () => { await window.loadAll(); });
    const recovered = await listState(page);

    assert.equal(ok.rows, 2);
    assert.equal(ok.errorText, '');
    assert.equal(failed.vmCount, 2, '503 이 직전 인벤토리를 지우면 안 된다');
    assert.equal(failed.rows, 2);
    assert.equal(failed.count, '2');
    assert.match(failed.errorText, /VM 목록을 불러오지 못했습니다/);
    assert.match(failed.errorText, /마지막으로 받은 목록/);
    assert.equal(recovered.rows, 2);
    assert.equal(recovered.errorText, '', '복구 후에는 오류 표시가 사라져야 한다');
  }, {
    routes: {
      '/api/v1/vms': () => (++call === 2
        ? { status: 503, body: { error: { code: -32000, message: 'Circuit breaker open' } } }
        : { body: { data: VMS } }),
    },
  });
});

test('WS vm.* 이벤트 재조회가 503 이면 인벤토리를 비우지 않고 오류를 표시한다', async () => {
  let call = 0;
  await withPage([], async (page, { port }) => {
    await boot(page, port);
    await page.evaluate(async () => {
      await window.loadAll();
      window.WebSocket = class {
        static OPEN = 1;
        constructor() { this.readyState = 1; }
        send() {}
        close() {}
      };
      PCV.api.connectWS();
      wsConnection._pcvAuthDone = true;
      wsConnection.onmessage({ data: JSON.stringify({ type: 'vm.created', name: 'new-vm' }) });
    });
    await page.waitForFunction(() => document.querySelector('#vl .vm-list-error'), { timeout: 3000 });
    const state = await listState(page);

    assert.equal(state.vmCount, 2);
    assert.equal(state.rows, 2);
    assert.match(state.errorText, /Circuit breaker open/);
  }, {
    routes: {
      '/api/v1/vms': () => (++call === 1
        ? { body: { data: VMS } }
        : { status: 503, body: { error: { code: -32000, message: 'Circuit breaker open' } } }),
    },
  });
});
