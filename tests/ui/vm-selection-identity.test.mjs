import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const MODS = [
  'ui/i18n.js', 'ui/modules/endpoints.js', 'ui/modules/api.js', 'ui/modules/ui.js',
  'ui/modules/filter-state.js', 'ui/modules/uxlib.js', 'ui/modules/modal-core.js', 'ui/modules/modal.js',
  'ui/modules/charts.js', 'ui/modules/metrics.js', 'ui/modules/security.js', 'ui/modules/monitor.js',
  'ui/modules/vm.js', 'ui/modules/vm-console.js', 'ui/modules/vm-lifecycle.js', 'ui/modules/vm-guest.js',
  'ui/modules/container.js', 'ui/modules/network.js', 'ui/modules/storage.js', 'ui/modules/help.js',
  'ui/modules/nav.js', 'ui/modules/theme.js', 'ui/modules/accounts.js', 'ui/modules/advanced.js',
  'ui/modules/selfhealing.js', 'ui/modules/mobile.js', 'ui/modules/shell.js', 'ui/app.js',
];

const MAC = '52:54:00:aa:bb:cc';
const NAMES = ['A', 'B', 'C', 'N', 'B2'];
const vm = (name, extra) => ({ name, uuid: 'u-' + name.toLowerCase(), state: 'running', vcpu: 2, memory_mb: 2048, ...extra });

function makeRoutes(state) {
  const ok = { body: { data: {} } };
  const routes = {
    '/api/v1/vms': () => ({ body: { data: state.list } }),
    '/api/v1/networks': { body: { data: [] } },
  };
  for (const n of NAMES) {
    routes['/api/v1/vms/' + n] = { body: { data: { cpu: 5, mem: 10, vcpu: 2, memory_mb: 2048 } } };
    routes['/api/v1/vms/' + n + '/nics'] = () => ({ body: { data: [{ mac: MAC, bridge: 'br0' }] } });
    routes['/api/v1/vms/' + n + '/nics/' + MAC] = ok;
    for (const a of ['start', 'suspend', 'resume', 'vcpu', 'memory', 'iso', 'disk']) routes['/api/v1/vms/' + n + '/' + a] = ok;
    routes['/api/v1/vms/' + n + '/stop'] = async () => {
      if (state.stopDelay) await new Promise(r => setTimeout(r, state.stopDelay));
      return ok;
    };
    routes['/api/v1/vms/' + n + '/rename'] = () => { if (state.onRename) state.onRename(n); return ok; };
  }
  return routes;
}

async function boot(page, port) {
  await page.evaluate(async () => {
    const html = await fetch('/ui/index.html').then(r => r.text());
    document.body.innerHTML = new DOMParser().parseFromString(html, 'text/html').body.innerHTML;
    class FakeWS {
      constructor(url) { this.url = url; this.readyState = 0; window.__fakeWs = this; setTimeout(() => { this.readyState = 1; if (this.onopen) this.onopen(); }, 0); }
      send() {}
      close() { this.readyState = 3; }
      emit(obj) { this.onmessage({ data: JSON.stringify(obj) }); }
    }
    FakeWS.CONNECTING = 0; FakeWS.OPEN = 1; FakeWS.CLOSING = 2; FakeWS.CLOSED = 3;
    window.WebSocket = FakeWS;
  });
  for (const m of MODS) await page.addScriptTag({ url: `http://127.0.0.1:${port}/${m}` });
  await page.evaluate(async () => {
    I18N.setLang('ko');
    const sp = document.getElementById('splash'); if (sp) sp.remove();
    if (window.pcvSetLoginVisible) window.pcvSetLoginVisible(false);
    window.authToken = 't';
    window.currentUser = { role: 'ADMIN' };
    window.customConfirm = async () => true;
    window.connectWS();
    await new Promise(r => setTimeout(r, 20));
    window.__fakeWs.emit({ type: 'auth_ok' });
  });
}

const sleep = ms => new Promise(r => setTimeout(r, ms));

async function openSummary(page, name) {
  await page.evaluate(async n => {
    await loadAll();
    window.selectedVmIndex = vmList.findIndex(v => v.name === n);
    navigateTo('summary');
  }, name);
  await page.waitForFunction(n => {
    const h = document.querySelector('#vm-detail h2');
    return h && h.textContent === n;
  }, { timeout: 5000 }, name);
  await sleep(1200);
}

const poll = page => page.evaluate(async () => { invalidateCache('vm.list'); await loadAll(true); });
const posts = (requests, suffix) => requests.filter(r => r.method === 'POST' && r.path.endsWith(suffix)).map(r => r.path);
const clickText = (page, selector, text) => page.evaluate((sel, tx) => {
  const el = [...document.querySelectorAll(sel)].find(e => e.textContent.includes(tx));
  if (!el) return false;
  el.click();
  return true;
}, selector, text);
const ctxEvent = '({ preventDefault() {}, pageX: 5, pageY: 5 })';

async function run(state, fn) {
  await withPage([], async (page, { port, requests }) => {
    await page.setViewport({ width: 1400, height: 2400 });
    await boot(page, port);
    await fn(page, requests);
  }, { routes: makeRoutes(state) });
}

test('1. 목록 앞에 VM이 생겨도 B 요약의 [중지]는 B에만 간다', async () => {
  const state = { list: [vm('A'), vm('B'), vm('C')] };
  await run(state, async (page, requests) => {
    await openSummary(page, 'B');
    state.list = [vm('N'), vm('A'), vm('B'), vm('C')];
    await poll(page);
    assert.equal(await clickText(page, '#vm-detail button', '■'), true, '[■ 중지] 버튼이 있어야 한다');
    await sleep(400);
    assert.deepEqual(posts(requests, '/stop'), ['/api/v1/vms/B/stop']);
  });
});

test('2. WS 전체 재조회(vm.created) 뒤에도 B 요약의 [중지]는 B에만 간다', async () => {
  const state = { list: [vm('A'), vm('B'), vm('C')] };
  await run(state, async (page, requests) => {
    await openSummary(page, 'B');
    state.list = [vm('N'), vm('A'), vm('B'), vm('C')];
    await page.evaluate(() => window.__fakeWs.emit({ type: 'vm.created', name: 'N' }));
    await page.waitForFunction(() => vmList.length === 4);
    assert.equal(await clickText(page, '#vm-detail button', '■'), true);
    await sleep(400);
    assert.deepEqual(posts(requests, '/stop'), ['/api/v1/vms/B/stop']);
  });
});

test('3. B가 사라지면 선택은 −1로 남고 안내를 그리며 중지·Ctrl+D가 다른 VM을 집지 않는다', async () => {
  const state = { list: [vm('A'), vm('B'), vm('C')] };
  await run(state, async (page, requests) => {
    await openSummary(page, 'B');
    state.list = [vm('A'), vm('C')];
    await poll(page);
    assert.equal(await page.evaluate(() => selectedVmIndex), -1);
    await page.waitForFunction(() => /선택한 VM이 목록에 없습니다/.test(document.querySelector('#vm-detail').textContent));
    await page.evaluate(() => window.vmPower('stop'));
    await sleep(300);
    assert.deepEqual(posts(requests, '/stop'), []);
    await poll(page);
    assert.equal(await page.evaluate(() => selectedVmIndex), -1);
    await page.evaluate(() => document.dispatchEvent(new KeyboardEvent('keydown', { key: 'd', ctrlKey: true })));
    await sleep(300);
    assert.equal(await page.evaluate(() => !!PCV.modalCore.currentBody()), false, 'Ctrl+D가 설정 모달을 열면 안 된다');
  });
});

test('4. 같은 이름 다른 UUID로 다시 만들어지면 같은 VM으로 보지 않는다', async () => {
  const state = { list: [vm('A'), vm('B'), vm('C')] };
  await run(state, async page => {
    await openSummary(page, 'B');
    state.list = [vm('A'), vm('B', { uuid: 'u-b2' }), vm('C')];
    await poll(page);
    assert.equal(await page.evaluate(() => selectedVmIndex), -1);
  });
});

test('5. 설정 모달을 연 채 순서가 바뀌어도 vCPU 적용은 B로 간다', async () => {
  const state = { list: [vm('A'), vm('B'), vm('C')] };
  await run(state, async (page, requests) => {
    await openSummary(page, 'B');
    await page.evaluate(() => showSettings());
    state.list = [vm('N'), vm('A'), vm('B'), vm('C')];
    await poll(page);
    assert.equal(await clickText(page, '.hw-item', 'vCPU'), true);
    assert.equal(await clickText(page, '#hw-edit button', '적용') || await clickText(page, '#hw-edit button', 'Apply'), true);
    await sleep(400);
    assert.deepEqual(requests.filter(r => r.method === 'PUT').map(r => r.path), ['/api/v1/vms/B/vcpu']);
  });
});

test('5b. 설정 모달이 열린 동안 선택이 다른 VM으로 옮겨져도 제출은 B로 간다', async () => {
  const state = { list: [vm('A'), vm('B'), vm('C')] };
  await run(state, async (page, requests) => {
    await openSummary(page, 'B');
    await page.evaluate(() => showSettings());
    await page.evaluate(() => { window.selectedVmIndex = 0; });
    assert.equal(await clickText(page, '.hw-item', 'vCPU'), true);
    assert.equal(await clickText(page, '#hw-edit button', '적용'), true, '[적용] 클릭이 실제로 일어나야 한다');
    await sleep(400);
    assert.deepEqual(requests.filter(r => r.method === 'PUT').map(r => r.path), ['/api/v1/vms/B/vcpu']);
  });
});

test('5c. 설정 모달의 대상이 사라지면 제출은 요청하지 않는다', async () => {
  const state = { list: [vm('A'), vm('B'), vm('C')] };
  await run(state, async (page, requests) => {
    await openSummary(page, 'B');
    await page.evaluate(() => showSettings());
    assert.equal(await clickText(page, '.hw-item', 'vCPU'), true);
    state.list = [vm('A'), vm('C')];
    await poll(page);
    assert.equal(await clickText(page, '#hw-edit button', '적용'), true, '[적용] 클릭이 실제로 일어나야 한다');
    await sleep(400);
    assert.deepEqual(requests.filter(r => r.method === 'PUT'), []);
  });
});

test('6. NIC 관리 모달에서 순서가 바뀐 뒤 제거해도 B의 NIC만 지운다', async () => {
  const state = { list: [vm('A'), vm('B'), vm('C')] };
  await run(state, async (page, requests) => {
    await openSummary(page, 'B');
    await page.evaluate(() => showNicMgr());
    await page.waitForFunction(() => document.querySelector('#nic-mgr table'));
    state.list = [vm('N'), vm('A'), vm('B'), vm('C')];
    await poll(page);
    await page.evaluate(() => { window.selectedVmIndex = 0; });
    await page.evaluate(m => window.nmDel(m), MAC);
    await sleep(400);
    assert.deepEqual(requests.filter(r => r.method === 'DELETE').map(r => r.path), ['/api/v1/vms/B/nics/' + MAC]);
  });
});

test('7. 우클릭 Clone 뒤 순서가 바뀌어도 복제 창 제목은 B다', async () => {
  const state = { list: [vm('A'), vm('B'), vm('C')] };
  await run(state, async page => {
    await openSummary(page, 'B');
    await page.evaluate(ev => window.showCtx(eval(ev), 1), ctxEvent);
    state.list = [vm('N'), vm('A'), vm('B'), vm('C')];
    await poll(page);
    assert.equal(await clickText(page, '#ctx .ci', 'Clone'), true);
    await page.waitForFunction(() => PCV.modalCore.currentBody());
    assert.match(await page.evaluate(() => PCV.modalCore.currentBody().querySelector('h2').textContent), /: B$/);
  });
});

test('8. bulkStop은 첫 응답을 기다리는 중 순서가 바뀌어도 체크한 이름마다 한 번씩만 보낸다', async () => {
  const state = { list: [vm('A'), vm('B'), vm('C')], stopDelay: 400 };
  await run(state, async (page, requests) => {
    await page.evaluate(async () => { await loadAll(); toggleChk(0); toggleChk(2); });
    await page.evaluate(() => { window.__stop = window.bulkStop(); });
    await sleep(150);
    state.list = [vm('N'), vm('A'), vm('B'), vm('C')];
    await poll(page);
    await page.evaluate(() => window.__stop);
    assert.deepEqual(posts(requests, '/stop').sort(), ['/api/v1/vms/A/stop', '/api/v1/vms/C/stop']);
  });
});

test('9. 이름 변경(B → B2)에 성공하면 선택이 B2다', async () => {
  const state = { list: [vm('A'), vm('B', { state: 'shutoff' }), vm('C')] };
  state.onRename = () => { state.list = [vm('A'), vm('B2', { state: 'shutoff', uuid: 'u-b' }), vm('C')]; };
  await run(state, async page => {
    await openSummary(page, 'B');
    await page.evaluate(() => showRenameVm());
    await page.evaluate(() => { document.getElementById('rn-new').value = 'B2'; });
    await page.evaluate(() => window.doVmRename());
    await page.waitForFunction(() => vmList.some(v => v.name === 'B2'));
    await sleep(300);
    assert.equal(await page.evaluate(() => vmList[selectedVmIndex] && vmList[selectedVmIndex].name), 'B2');
  });
});

test('10. 첫 로드(이전 목록 없음)에는 0번 VM이 선택된다', async () => {
  const state = { list: [vm('A'), vm('B'), vm('C')] };
  await run(state, async page => {
    await page.evaluate(() => loadAll());
    assert.equal(await page.evaluate(() => selectedVmIndex), 0);
  });
});

test('11. 요약 X에서 Y를 우클릭하면 요약이 Y로 다시 그려지고 [설정] 창 제목도 Y다', async () => {
  const state = { list: [vm('A'), vm('B'), vm('C')] };
  await run(state, async page => {
    await openSummary(page, 'B');
    await page.evaluate(ev => window.showCtx(eval(ev), 2), ctxEvent);
    await page.evaluate(() => document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' })));
    await page.waitForFunction(() => { const h = document.querySelector('#vm-detail h2'); return h && h.textContent === 'C'; }, { timeout: 3000 });
    await page.evaluate(() => showSettings());
    assert.match(await page.evaluate(() => PCV.modalCore.currentBody().querySelector('h2').textContent), /: C$/);
  });
});

test('12. 콘솔 탭에서 Y를 우클릭해도 콘솔 화면은 다시 그려지지 않고 선택만 Y로 간다', async () => {
  const state = { list: [vm('A'), vm('B'), vm('C')] };
  await run(state, async page => {
    await page.evaluate(async () => {
      await loadAll();
      window.selectedVmIndex = 1;
      navigateTo('console');
    });
    await page.waitForFunction(() => window.currentTab === 'console' && document.querySelector('#vm-detail'));
    await sleep(1500);
    await page.evaluate(() => {
      window.__calls = 0;
      window._vncRfb = { disconnect() { window.__calls++; } };
      window.__frame = document.getElementById('vnc-frame');
    });
    await page.evaluate(ev => window.showCtx(eval(ev), 2), ctxEvent);
    await sleep(300);
    const r = await page.evaluate(() => ({
      same: document.getElementById('vnc-frame') === window.__frame,
      calls: window.__calls, sel: selectedVmIndex,
    }));
    assert.deepEqual(r, { same: true, calls: 0, sel: 2 });
  });
});

for (const tab of ['console', 'snapshots', 'performance', 'timeline']) {
  test(`13. ${tab} 탭에서 보던 VM이 사라지면 빈 화면이 아니라 안내를 그린다`, async () => {
    const state = { list: [vm('A'), vm('B'), vm('C')] };
    await run(state, async page => {
      await page.evaluate(async t => {
        await loadAll();
        window.selectedVmIndex = 1;
        navigateTo(t);
      }, tab);
      await page.waitForFunction(t => window.currentTab === t && document.querySelector('#vm-detail'), {}, tab);
      await sleep(1500);
      state.list = [vm('A'), vm('C')];
      await poll(page);
      await page.waitForFunction(() => /선택한 VM이 목록에 없습니다/.test(document.querySelector('#vm-detail').textContent), { timeout: 3000 });
    });
  });
}

async function clickHeadDelete(page) {
  await page.click('#vm-delete');
  await sleep(300);
  return page.evaluate(() => {
    const body = PCV.modalCore.currentBody();
    if (!body || !body.querySelector('#vm-delete-confirm')) return null;
    return [...body.querySelectorAll('ul li')].map(li => li.textContent);
  });
}
const headDelete = page => page.evaluate(() => {
  const b = document.getElementById('vm-delete');
  return b && { disabled: b.disabled, title: b.title };
});

test('14. 요약 B에서 C를 우클릭하고 ESC를 누른 뒤 머리 [VM 삭제]는 C의 확인창을 연다', async () => {
  const state = { list: [vm('A'), vm('B'), vm('C')] };
  await run(state, async page => {
    await openSummary(page, 'B');
    assert.deepEqual(await headDelete(page), { disabled: false, title: 'B' });
    await page.evaluate(ev => window.showCtx(eval(ev), 2), ctxEvent);
    await page.keyboard.press('Escape');
    await page.waitForFunction(() => { const h = document.querySelector('#vm-detail h2'); return h && h.textContent === 'C'; }, { timeout: 3000 });
    assert.deepEqual(await headDelete(page), { disabled: false, title: 'C' });
    assert.deepEqual(await clickHeadDelete(page), ['C']);
  });
});

test('15. j로 다음 VM으로 옮기면 머리 [VM 삭제]의 title과 확인창이 그 VM이다', async () => {
  const state = { list: [vm('A'), vm('B'), vm('C')] };
  await run(state, async page => {
    await openSummary(page, 'B');
    await page.evaluate(() => { if (document.activeElement) document.activeElement.blur(); });
    await page.keyboard.press('j');
    await page.waitForFunction(() => { const h = document.querySelector('#vm-detail h2'); return h && h.textContent === 'C'; }, { timeout: 3000 });
    assert.equal(await page.evaluate(() => selectedVmIndex), 2);
    assert.deepEqual(await headDelete(page), { disabled: false, title: 'C' });
    assert.deepEqual(await clickHeadDelete(page), ['C']);
  });
});

test('16. 목록 교체로 선택이 사라지면(−1) 머리 [VM 삭제]는 꺼지고 눌러도 확인창이 열리지 않는다', async () => {
  const state = { list: [vm('A'), vm('B'), vm('C')] };
  await run(state, async (page, requests) => {
    await openSummary(page, 'B');
    state.list = [vm('A'), vm('C')];
    await poll(page);
    assert.equal(await page.evaluate(() => selectedVmIndex), -1);
    await page.waitForFunction(() => /선택한 VM이 목록에 없습니다/.test(document.querySelector('#vm-detail').textContent));
    assert.deepEqual(await headDelete(page), { disabled: true, title: 'VM을 선택하세요' });
    assert.equal(await clickHeadDelete(page), null);
    await page.evaluate(() => document.getElementById('vm-delete').click());
    await sleep(200);
    assert.equal(await page.evaluate(() => !!PCV.modalCore.currentBody()), false, '삭제 확인창이 열리면 안 된다');
    assert.deepEqual(requests.filter(r => r.method === 'DELETE'), []);
  });
});

test('17. 콘솔 탭에서 C를 우클릭하면 VNC 화면은 그대로이고 머리 [VM 삭제]는 C를 따른다', async () => {
  const state = { list: [vm('A'), vm('B'), vm('C')] };
  await run(state, async page => {
    await page.evaluate(async () => {
      await loadAll();
      window.selectedVmIndex = 1;
      navigateTo('console');
    });
    await page.waitForFunction(() => window.currentTab === 'console' && document.querySelector('#vm-detail'));
    await sleep(1500);
    assert.deepEqual(await headDelete(page), { disabled: false, title: 'B' });
    await page.evaluate(() => {
      window.__calls = 0;
      window._vncRfb = { disconnect() { window.__calls++; } };
      window.__frame = document.getElementById('vnc-frame');
    });
    await page.evaluate(ev => window.showCtx(eval(ev), 2), ctxEvent);
    await sleep(300);
    assert.deepEqual(await page.evaluate(() => ({
      same: document.getElementById('vnc-frame') === window.__frame, calls: window.__calls,
    })), { same: true, calls: 0 });
    assert.deepEqual(await headDelete(page), { disabled: false, title: 'C' });
    assert.deepEqual(await clickHeadDelete(page), ['C']);
  });
});

test('18. 이 화면에서 이름을 바꾸면(B → B2) 머리 [VM 삭제]의 title과 확인창이 B2다', async () => {
  const state = { list: [vm('A'), vm('B', { state: 'shutoff' }), vm('C')] };
  state.onRename = () => { state.list = [vm('A'), vm('B2', { state: 'shutoff', uuid: 'u-b' }), vm('C')]; };
  await run(state, async page => {
    await openSummary(page, 'B');
    await page.evaluate(() => showRenameVm());
    await page.evaluate(() => { document.getElementById('rn-new').value = 'B2'; });
    await page.evaluate(() => window.doVmRename());
    await page.waitForFunction(() => vmList.some(v => v.name === 'B2'));
    await sleep(300);
    assert.deepEqual(await headDelete(page), { disabled: false, title: 'B2' });
    assert.deepEqual(await clickHeadDelete(page), ['B2']);
  });
});
