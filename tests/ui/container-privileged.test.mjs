import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const MODS = ['ui/modules/ui.js', 'ui/modules/uxlib.js', 'ui/modules/endpoints.js', 'ui/modules/filter-state.js', 'ui/modules/container.js'];
const LIST = [
  { name: 'ctr-priv', state: 'STOPPED', privileged: true },
  { name: 'ctr-user', state: 'STOPPED', privileged: false },
  { name: 'ctr-old', state: 'STOPPED' }
];
const NOT_READY = "unprivileged containers are not available on this host: /etc/subuid and /etc/subgid must delegate 100000:65536 to root. Add 'root:100000:65536' to both files. Or ask an admin to create a privileged container (rootless:false).";
const LIST_ROUTE = { '/api/v1/containers': { status: 200, body: { data: LIST } } };

async function boot(page) {
  await page.setViewport({ width: 1280, height: 800 });
  await page.evaluate(() => {
    window.authToken = 'test-token';
    window._L = (ko, en) => en;
    window.t = key => key;
    window.unwrapData = r => r == null ? r : (r.data !== undefined ? r.data : (r.result !== undefined ? r.result : r));
    window.unwrapList = r => Array.isArray(r) ? r : (Array.isArray(window.unwrapData(r)) ? window.unwrapData(r) : []);
    window.fetchGet = url => fetch(url).then(r => r.json());
    window.fetchPost = (url, body) => fetch(url, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) }).then(r => r.json());
    window.__toasts = [];
    window.toast = (msg, ok) => window.__toasts.push({ msg: String(msg), ok });
    window.addEvt = () => {};
    window.closeModal = () => {};
  });
}

async function rowValue(page, key) {
  return page.$$eval('#ctr-tab-content .hr', (rows, k) => {
    const r = rows.find(x => x.querySelector('.k') && x.querySelector('.k').textContent === k);
    return r ? r.querySelector('.v').textContent : null;
  }, key);
}

test('container list: privileged=true 항목에만 경고 배지, 필드 없는 옛 데몬 항목은 배지 없음', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    await page.evaluate(() => window.renderContainers(document.getElementById('cb')));
    const warn = await page.$$eval('#cb .pill-warn', els => els.map(e => ({
      text: e.textContent, owner: e.closest('[onclick]') && e.closest('[onclick]').getAttribute('onclick') })));
    assert.equal(warn.length, 1);
    assert.equal(warn[0].text, 'privileged');
    assert.match(warn[0].owner, /ctr-priv/);
  }, { routes: LIST_ROUTE });
});

for (const [name, type, unpriv] of [['ctr-priv', 'LXC (privileged)', 'No'], ['ctr-user', 'LXC (unprivileged)', 'Yes'], ['ctr-old', 'LXC', 'Unknown']]) {
  test(`container detail ${name}: Type='${type}', Options Unprivileged='${unpriv}'`, async () => {
    await withPage(MODS, async page => {
      await boot(page);
      await page.evaluate(n => { window.selCtr = n; window.ctrTab = 'summary'; }, name);
      await page.evaluate(() => window.renderContainers(document.getElementById('cb')));
      assert.equal(await rowValue(page, 'Type'), type);
      await page.evaluate(() => { window.ctrTab = 'options'; });
      await page.evaluate(() => window.renderContainers(document.getElementById('cb')));
      assert.equal(await rowValue(page, 'Unprivileged'), unpriv);
    }, { routes: LIST_ROUTE });
  });
}

test('container create: 동기 -32002 거절 메시지가 실패 토스트로 보인다', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    await page.evaluate(() => {
      for (const [id, v] of [['cc-name', 'web1'], ['cc-dist', 'ubuntu'], ['cc-rel', 'noble'], ['cc-br', 'pcvbr0'],
                             ['cc-vcpu', '1'], ['cc-mem', '512']]) {
        const i = document.createElement('input'); i.id = id; i.value = v; document.body.appendChild(i);
      }
    });
    await page.evaluate(() => window.doCtrCreate());
    const toasts = await page.evaluate(() => window.__toasts);
    const fail = toasts.find(x => x.ok === false);
    assert.ok(fail, JSON.stringify(toasts));
    assert.ok(fail.msg.includes("root:100000:65536"), fail.msg);
  }, { routes: { '/api/v1/containers': rec => rec.method === 'POST'
    ? { status: 409, body: { error: { code: -32002, message: NOT_READY } } }
    : { status: 200, body: { data: [] } } } });
});

test('container create: 폼에서 고른 브리지가 POST 본문 bridge로 간다', async () => {
  let posted = null;
  await withPage(MODS, async page => {
    await boot(page);
    await page.evaluate(() => {
      for (const [id, v] of [['cc-name', 'web2'], ['cc-dist', 'ubuntu'], ['cc-rel', 'noble'], ['cc-br', 'lxcbr0'],
                             ['cc-vcpu', '1'], ['cc-mem', '512']]) {
        const i = document.createElement('input'); i.id = id; i.value = v; document.body.appendChild(i);
      }
    });
    await page.evaluate(() => window.doCtrCreate());
  }, { routes: { '/api/v1/containers': rec => {
    if (rec.method !== 'POST') return { status: 200, body: { data: [] } };
    posted = rec.json;
    return { status: 400, body: { error: { code: -32602, message: 'recorded' } } };
  } } });
  assert.ok(posted, 'POST /api/v1/containers 요청이 없다');
  assert.equal(posted.bridge, 'lxcbr0');
  assert.equal(posted.name, 'web2');
});

test('container memory stats denial shows the server reason instead of an error table', async () => {
  const message = 'Permission denied: operators can access only containers they created';
  await withPage(MODS, async page => {
    await boot(page);
    const result = await page.evaluate(async () => {
      await window.showCtrMemoryStats('sc-theirs');
      return { toasts: window.__toasts, dialog: !!document.querySelector('dialog[open]') };
    });
    assert.deepEqual(result.toasts, [{ msg: message, ok: false }]);
    assert.equal(result.dialog, false);
  }, { routes: { ...LIST_ROUTE, '/api/v1/containers/sc-theirs/memory-stats': { status: 403, body: { error: { code: -32006, message } } } } });
});
