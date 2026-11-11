import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const MODS = [
  'ui/i18n.js',
  'ui/modules/endpoints.js', 'ui/modules/api.js', 'ui/modules/ui.js',
  'ui/modules/filter-state.js', 'ui/modules/uxlib.js',
  'ui/modules/modal-core.js', 'ui/modules/modal.js', 'ui/modules/help.js'
];
const OK = { status: 200, body: { result: { ok: true } } };
const ROUTES = {
  '/api/v1/vms': OK,
  '/api/v1/vms/test/start': OK,
  '/api/v1/vms/test': OK,
  '/api/v1/containers': OK,
  '/api/v1/storage/pools': OK,
  '/api/v1/push/vapid/rotate': OK
};

async function boot(page) {
  const apiRequests = [];
  page.on('request', request => {
    const url = new URL(request.url());
    if (url.pathname.startsWith('/api/v1/')) {
      apiRequests.push({ method: request.method(), path: url.pathname, body: request.postData() || null });
    }
  });
  await page.evaluate(() => {
    I18N.setLang('ko');
    window.authToken = 'test-token';
    window.currentUser = { role: 'admin' };
    renderSwaggerApi(document.getElementById('cb'));
  });
  return apiRequests;
}

async function clickTry(page, method, path) {
  const found = await page.evaluate((m, p) => {
    const key = (m + ' ' + p).toLowerCase();
    const ep = [...document.querySelectorAll('.sw-ep')].find(node => node.dataset.sw.startsWith(key + ' '));
    if (!ep) return false;
    ep.querySelector('button.btn').click();
    return true;
  }, method, path);
  assert.equal(found, true, `카탈로그에 ${method} ${path} 가 없다`);
}

const settle = page => page.evaluate(() => new Promise(resolve => setTimeout(resolve, 150)));
const openDialogText = page => page.evaluate(() => {
  const dialogs = [...document.querySelectorAll('dialog[open]')];
  return dialogs.length ? dialogs[dialogs.length - 1].textContent : null;
});

test('GET은 확인 없이 바로 실행한다', async () => {
  await withPage(MODS, async page => {
    const apiRequests = await boot(page);
    await clickTry(page, 'GET', '/vms');
    await settle(page);
    assert.deepEqual(apiRequests.map(r => r.method + ' ' + r.path), ['GET /api/v1/vms']);
    assert.match(await openDialogText(page), /응답/);
  }, { routes: ROUTES });
});

test('POST 는 메서드·경로·자리표시자 치환을 보여 주고, 취소하면 요청이 나가지 않는다', async () => {
  await withPage(MODS, async page => {
    const apiRequests = await boot(page);
    await clickTry(page, 'POST', '/vms/{name}/start');
    await settle(page);
    const text = await openDialogText(page);
    assert.ok(text, '확인 창이 떠야 한다');
    assert.match(text, /POST/);
    assert.match(text, /\/vms\/\{name\}\/start/);
    assert.match(text, /\/api\/v1\/vms\/test\/start/, '실제로 보낼 경로(test 치환)가 보여야 한다');
    assert.match(text, /test/);
    assert.equal(await page.$('#dc-input'), null, '되돌릴 수 있는 요청은 이름 재입력을 요구하지 않는다');
    assert.deepEqual(apiRequests, [], '확인 전에는 요청이 나가면 안 된다');
    await page.click('[data-confirm-cancel]');
    await settle(page);
    assert.deepEqual(apiRequests, [], '취소 뒤에도 요청이 나가면 안 된다');
    assert.equal(await openDialogText(page), null);
  }, { routes: ROUTES });
});

test('POST 확인 창은 초기 focus가 취소이고 Esc로 닫으면 요청이 나가지 않는다', async () => {
  await withPage(MODS, async page => {
    const apiRequests = await boot(page);
    await clickTry(page, 'POST', '/containers');
    await settle(page);
    const text = await openDialogText(page);
    assert.match(text, /pcv-example-ctr/, '본문이 보여야 한다');
    assert.equal(await page.evaluate(() => document.activeElement.hasAttribute('data-confirm-cancel')), true);
    await page.keyboard.press('Escape');
    await settle(page);
    assert.deepEqual(apiRequests, []);
    assert.equal(await openDialogText(page), null);
  }, { routes: ROUTES });
});

test('POST 는 확인하면 보여 준 본문 그대로 한 번만 나간다', async () => {
  await withPage(MODS, async page => {
    const apiRequests = await boot(page);
    await clickTry(page, 'POST', '/containers');
    await settle(page);
    const shown = await openDialogText(page);
    await page.click('[data-confirm-accept]');
    await settle(page);
    assert.equal(apiRequests.length, 1);
    assert.equal(apiRequests[0].method, 'POST');
    assert.equal(apiRequests[0].path, '/api/v1/containers');
    assert.ok(shown.includes(apiRequests[0].body), `보낸 본문이 확인 창에 보여야 한다: ${apiRequests[0].body}`);
    assert.match(await openDialogText(page), /응답/);
  }, { routes: ROUTES });
});

test('DELETE 는 이름 재입력을 거치고, 다른 이름·취소로는 요청이 나가지 않는다', async () => {
  await withPage(MODS, async page => {
    const apiRequests = await boot(page);
    await clickTry(page, 'DELETE', '/storage/pools');
    await settle(page);
    const text = await openDialogText(page);
    assert.match(text, /DELETE/);
    assert.match(text, /\/api\/v1\/storage\/pools/);
    assert.match(text, /pcv-example-pool/, '본문이 보여야 한다');
    assert.notEqual(await page.$('#dc-input'), null, '이름 재입력 입력칸이 있어야 한다');

    await page.type('#dc-input', 'wrong');
    await page.click('#dc-go');
    await settle(page);
    assert.deepEqual(apiRequests, [], '이름이 틀리면 요청이 나가면 안 된다');
    assert.match(await page.$eval('#dc-msg', node => node.textContent), /일치/);

    await page.keyboard.press('Escape');
    await settle(page);
    assert.deepEqual(apiRequests, [], '취소하면 요청이 나가면 안 된다');
    assert.equal(await openDialogText(page), null);
  }, { routes: ROUTES });
});

test('DELETE 는 이름을 맞게 입력하면 본문과 함께 한 번만 나간다', async () => {
  await withPage(MODS, async page => {
    const apiRequests = await boot(page);
    await clickTry(page, 'DELETE', '/storage/pools');
    await settle(page);
    await page.type('#dc-input', '/storage/pools');
    await page.click('#dc-go');
    await settle(page);
    assert.equal(apiRequests.length, 1);
    assert.deepEqual(JSON.parse(apiRequests[0].body), { name: 'pcv-example-pool' });
    assert.equal(apiRequests[0].method, 'DELETE');
  }, { routes: ROUTES });
});

test('자리표시자 경로의 DELETE 는 치환된 test 경로를 재입력한다', async () => {
  await withPage(MODS, async page => {
    const apiRequests = await boot(page);
    await clickTry(page, 'DELETE', '/vms/{name}');
    await settle(page);
    const text = await openDialogText(page);
    assert.match(text, /\/vms\/test/);
    await page.type('#dc-input', '/vms/{name}');
    await page.click('#dc-go');
    await settle(page);
    assert.deepEqual(apiRequests, [], '자리표시자 원문은 맞는 이름이 아니다');
    await page.$eval('#dc-input', node => { node.value = ''; });
    await page.type('#dc-input', '/vms/test');
    await page.click('#dc-go');
    await settle(page);
    assert.deepEqual(apiRequests.map(r => r.method + ' ' + r.path), ['DELETE /api/v1/vms/test']);
  }, { routes: ROUTES });
});

test('/push/vapid/rotate 는 본문이 없어도 이름 재입력을 거친다', async () => {
  await withPage(MODS, async page => {
    const apiRequests = await boot(page);
    await clickTry(page, 'POST', '/push/vapid/rotate');
    await settle(page);
    assert.notEqual(await page.$('#dc-input'), null);
    assert.deepEqual(apiRequests, []);
    await page.keyboard.press('Escape');
    await settle(page);
    assert.deepEqual(apiRequests, []);
  }, { routes: ROUTES });
});

test('카탈로그의 GET이 아닌 "실행"은 전부 확인 단계에서 멈춘다', async () => {
  await withPage(MODS, async page => {
    const apiRequests = await boot(page);
    const eps = await page.evaluate(() => [...document.querySelectorAll('.sw-ep')].map((node, index) => ({
      index,
      method: node.querySelector('.sw-method-badge').textContent,
      text: node.dataset.sw
    })));
    const writes = eps.filter(ep => ep.method !== 'GET');
    assert.ok(writes.length > 90, `쓰기 엔드포인트가 너무 적다(파싱 회귀): ${writes.length}`);

    const result = await page.evaluate(async indexes => {
      const out = [];
      const nodes = [...document.querySelectorAll('.sw-ep')];
      for (const index of indexes) {
        nodes[index].querySelector('button.btn').click();
        await new Promise(resolve => setTimeout(resolve, 0));
        const dialogs = [...document.querySelectorAll('dialog[open]')];
        const dialog = dialogs[dialogs.length - 1];
        out.push({
          index,
          opened: !!dialog,
          irreversible: !!(dialog && dialog.querySelector('#dc-input')),
          text: dialog ? dialog.textContent : ''
        });
        PCV.modalCore.close(true);
        await new Promise(resolve => setTimeout(resolve, 0));
      }
      return out;
    }, writes.map(ep => ep.index));

    assert.deepEqual(apiRequests, [], '확인 전에 요청이 나간 엔드포인트가 있다');
    for (const row of result) {
      const ep = eps[row.index];
      assert.equal(row.opened, true, `${ep.text}: 확인 창이 없다`);
      assert.ok(row.text.includes(ep.method), `${ep.text}: 메서드가 안 보인다`);
      if (ep.method === 'DELETE') assert.equal(row.irreversible, true, `${ep.text}: DELETE 는 이름 재입력이어야 한다`);
    }

    const irreversible = result.filter(r => r.irreversible).map(r => eps[r.index].text);
    assert.ok(irreversible.some(text => text.startsWith('post /push/vapid/rotate')));
    assert.ok(irreversible.some(text => text.startsWith('post /vms/{name}/snapshot/rollback')));
    assert.ok(result.some(r => !r.irreversible));
    assert.equal(result.find(r => eps[r.index].text.startsWith('post /vms/{name}/start')).irreversible, false);
  }, { routes: ROUTES });
});
