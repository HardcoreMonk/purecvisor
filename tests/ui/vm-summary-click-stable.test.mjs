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

const vm = (name, extra) => ({ name, uuid: 'u-' + name.toLowerCase(), state: 'running', vcpu: 2, memory_mb: 2048, ...extra });
const sleep = ms => new Promise(r => setTimeout(r, ms));

function makeRoutes(state) {
  const ok = { body: { data: {} } };
  const routes = {
    '/api/v1/vms': () => ({ body: { data: state.list } }),
    '/api/v1/networks': { body: { data: [] } },
  };
  for (const n of ['A', 'B', 'C']) {
    routes['/api/v1/vms/' + n] = async () => {
      const d = state.detail && state.detail[n];
      const body = typeof d === 'function' ? await d() : (d || { cpu: 5, mem: 10, vcpu: 2, memory_mb: 2048 });
      return { body: { data: body } };
    };
    routes['/api/v1/vms/' + n + '/nics'] = { body: { data: [] } };
    for (const a of ['start', 'stop', 'suspend', 'resume']) routes['/api/v1/vms/' + n + '/' + a] = ok;
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
    window.__fakeWs.emit && window.__fakeWs.emit({ type: 'auth_ok' });
  });
}

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
  await sleep(1300);
}

const DISK_BTN = `(() => { const bs = [...document.querySelectorAll('#vm-detail button')].filter(b => /디스크 사용량|Disk Usage/.test(b.textContent)); return bs[bs.length - 1] || null; })()`;
const trackDiskBtn = page => page.evaluate(`(() => { const b = ${DISK_BTN}; window.__tracked = b; if (!b) return null; const r = b.getBoundingClientRect(); return { x: r.x + r.width / 2, y: r.y + r.height / 2 }; })()`);

async function run(state, fn) {
  await withPage([], async (page, { port, requests }) => {
    await page.setViewport({ width: 1400, height: 2400 });
    await boot(page, port);
    await fn(page, requests);
  }, { routes: makeRoutes(state) });
}

test('1. 같은 VM을 다시 그려도 작업 버튼 노드가 그대로다', async () => {
  const state = { list: [vm('A'), vm('B'), vm('C')] };
  await run(state, async page => {
    await openSummary(page, 'B');
    await trackDiskBtn(page);
    await page.evaluate(() => { refreshVmDetail(); });
    await sleep(600);
    const r = await page.evaluate(`(() => ({ same: ${DISK_BTN} === window.__tracked, connected: !!window.__tracked && window.__tracked.isConnected }))()`);
    assert.deepEqual(r, { same: true, connected: true });
  });
});

test('2. 누르는 사이(mousedown→재그리기→mouseup)에 같은 VM이 다시 그려져도 클릭이 살아 있다 (후보 a)', async () => {
  const state = { list: [vm('A'), vm('B'), vm('C')] };
  await run(state, async page => {
    await openSummary(page, 'B');
    await page.evaluate(() => { window.__calls = 0; window.showVmDiskUsage = function () { window.__calls++; }; });
    const p = await trackDiskBtn(page);
    await page.mouse.move(p.x, p.y);
    await page.mouse.down();
    await page.evaluate(() => { refreshVmDetail(); });
    await sleep(600);
    await page.mouse.up();
    await sleep(300);
    assert.equal(await page.evaluate(() => window.__calls), 1);
  });
});

test('3. 선택 인덱스가 다른 VM으로 옮겨져도 [■ 중지]는 화면에 그려진 B에 간다', async () => {
  const state = { list: [vm('A'), vm('B'), vm('C')] };
  await run(state, async (page, requests) => {
    await openSummary(page, 'B');
    await page.evaluate(() => { window.selectedVmIndex = 0; });
    await page.evaluate(() => {
      const b = [...document.querySelectorAll('#vm-detail button')].find(x => x.textContent.includes('■'));
      b.click();
    });
    await sleep(400);
    assert.deepEqual(requests.filter(r => r.method === 'POST' && r.path.endsWith('/stop')).map(r => r.path), ['/api/v1/vms/B/stop']);
  });
});

test('4. 같은 VM을 다시 그리면 상태 배지는 바뀌고 버튼 노드는 그대로다', async () => {
  const state = { list: [vm('A'), vm('B', { state: 'shutoff' }), vm('C')] };
  await run(state, async page => {
    await openSummary(page, 'B');
    await trackDiskBtn(page);
    assert.match(await page.evaluate(() => document.querySelector('#vm-detail h2').parentNode.textContent), /SHUTOFF/);
    state.list = [vm('A'), vm('B'), vm('C')];
    await page.evaluate(async () => { invalidateCache('vm.list'); await loadAll(true); refreshVmDetail(); });
    await page.waitForFunction(() => { const h = document.querySelector('#vm-detail h2'); return !!h && /RUNNING/.test(h.parentNode.textContent); }, { timeout: 3000 });
    assert.equal(await page.evaluate(`${DISK_BTN} === window.__tracked`), true);
  });
});

test('5. 이전 VM의 늦은 응답은 새 VM 헤더를 덮어쓰지 않는다', async () => {
  const state = { list: [vm('A'), vm('B'), vm('C')], detail: { A: () => new Promise(r => setTimeout(() => r({ cpu: 5, vcpu: 2, memory_mb: 2048 }), 1000)) } };
  await run(state, async page => {
    await openSummary(page, 'C');
    await page.evaluate(() => { window.selectedVmIndex = 0; refreshVmDetail(); });
    await page.evaluate(() => { window.selectedVmIndex = 1; refreshVmDetail(); });
    await sleep(1400);
    assert.equal(await page.evaluate(() => document.querySelector('#vm-detail h2').textContent), 'B');
  });
});

test('5b. 같은 VM의 먼저 시작한 늦은 렌더가 나중 렌더(세대)를 덮어쓰지 않는다', async () => {

  const state = { list: [vm('A'), vm('B'), vm('C')] };
  state.detail = { B: () => (state.cpu === undefined ? { cpu: 5, vcpu: 2, memory_mb: 2048 } : { cpu: state.cpu, vcpu: 2, memory_mb: 2048 }) };
  await run(state, async page => {
    await openSummary(page, 'B');
    await page.evaluate(() => {
      const real = window.fetch.bind(window);
      let slow = true;
      window.fetch = function (u, o) {
        if (slow && /\/vms\/B$/.test(String(u))) {
          slow = false;
          return real(u, o).then(r => new Promise(res => setTimeout(() => res(r), 900)));
        }
        return real(u, o);
      };
    });
    state.cpu = 5;
    await page.evaluate(() => { refreshVmDetail(); });
    await sleep(100);
    state.cpu = 90;
    await page.evaluate(() => { refreshVmDetail(); });
    await sleep(1400);
    assert.match(await page.evaluate(() => document.querySelector('#vm-detail').textContent), /90\.0%/);
  });
});

test('6. VM 탭 사이를 옮긴 직후(View Transition 구간)의 클릭도 새 요약에 닿는다 (후보 b)', async () => {
  const state = { list: [vm('A'), vm('B'), vm('C')] };
  await run(state, async page => {
    await openSummary(page, 'B');
    const vt = await page.evaluate(() => typeof document.startViewTransition);
    assert.equal(vt, 'function', '하네스 Chromium이 View Transition을 지원해야 시험이 의미가 있다');
    const p = await trackDiskBtn(page);
    await page.evaluate(() => { window.showVmDiskUsage = function () { window.__calls++; }; navigateTo('console'); });
    await sleep(1000);
    await page.evaluate(() => { window.__calls = 0; navigateTo('summary'); });
    await sleep(100);
    await page.mouse.click(p.x, p.y);
    await sleep(300);
    assert.equal(await page.evaluate(() => window.__calls), 1);
  });
});

test('6b. 다른 화면(대시보드)에서 VM 요약으로 들어온 직후의 클릭도 닿는다', async () => {
  const state = { list: [vm('A'), vm('B'), vm('C')] };
  await run(state, async page => {
    await openSummary(page, 'B');
    const p = await trackDiskBtn(page);
    await page.evaluate(() => { window.showVmDiskUsage = function () { window.__calls++; }; navigateTo('dashboard'); });
    await sleep(1200);
    await page.evaluate(() => { window.__calls = 0; navigateTo('summary'); });
    await sleep(100);
    await page.mouse.click(p.x, p.y);
    await sleep(300);
    assert.equal(await page.evaluate(() => window.__calls), 1);
  });
});

test('2b. 저장소 행의 [디스크 사용량] xs 버튼도 누르는 사이 재그리기에 노드가 바뀌지 않는다', async () => {
  const state = { list: [vm('A'), vm('B'), vm('C')] };
  await run(state, async page => {
    await openSummary(page, 'B');
    await page.evaluate(() => { window.__calls = 0; window.showVmDiskUsage = function () { window.__calls++; }; });
    const p = await page.evaluate(() => {
      const b = document.querySelector('#vm-detail button.btn-xs');
      window.__tracked = b;
      const r = b.getBoundingClientRect();
      return { x: r.x + r.width / 2, y: r.y + r.height / 2 };
    });
    await page.mouse.move(p.x, p.y);
    await page.mouse.down();
    await page.evaluate(() => { refreshVmDetail(); });
    await sleep(600);
    await page.mouse.up();
    await sleep(300);
    const r = await page.evaluate(() => ({ calls: window.__calls, same: document.querySelector('#vm-detail button.btn-xs') === window.__tracked }));
    assert.deepEqual(r, { calls: 1, same: true });
  });
});

const STOP_POS = `(() => { const b = [...document.querySelectorAll('#vm-detail button')].find(x => x.textContent.includes('■')); if (!b) return null; const r = b.getBoundingClientRect(); return { x: r.x + r.width / 2, y: r.y + r.height / 2 }; })()`;
const slowC = ms => () => new Promise(r => setTimeout(() => r({ cpu: 5, vcpu: 2, memory_mb: 2048 }), ms));
const stopPosts = requests => requests.filter(r => r.method === 'POST' && r.path.endsWith('/stop')).map(r => r.path);

async function switchToC(page) {
  await page.evaluate(() => { window.selectedVmIndex = 2; refreshVmDetail(); });
  await page.waitForFunction(() => { const h = document.querySelector('#vm-detail h2'); return !!h && h.textContent === 'C'; }, { timeout: 3000 });
  await sleep(150);
}

test('7. VM을 바꾼 직후 데이터가 오기 전후로 [■ 중지] 좌표가 같다 (레이아웃 이동 0)', async () => {
  const state = { list: [vm('A'), vm('B'), vm('C')], detail: { C: slowC(800) } };
  await run(state, async page => {
    await openSummary(page, 'B');
    await switchToC(page);
    const before = await page.evaluate(STOP_POS);
    await sleep(1100);
    const after = await page.evaluate(STOP_POS);
    assert.deepEqual(after, before);
  });
});

test('7b. 데이터가 도착하는 순간 누르고 있던 [■ 중지]는 C에 POST 1건으로 끝난다', async () => {
  const state = { list: [vm('A'), vm('B'), vm('C')], detail: { C: slowC(800) } };
  await run(state, async (page, requests) => {
    await openSummary(page, 'B');
    await switchToC(page);
    const p = await page.evaluate(STOP_POS);
    await page.mouse.move(p.x, p.y);
    await page.mouse.down();
    await sleep(1100);
    await page.mouse.up();
    await sleep(400);
    assert.deepEqual(stopPosts(requests), ['/api/v1/vms/C/stop']);
  });
});

test('7c. 데이터가 오기 전에 이전 VM 데이터 카드가 있던 자리를 눌러도 전원 RPC가 나가지 않는다', async () => {
  const state = { list: [vm('A'), vm('B'), vm('C')], detail: { C: slowC(800) } };
  await run(state, async (page, requests) => {
    await openSummary(page, 'B');

    const rect = await page.evaluate(() => { const c = [...document.querySelectorAll('#vm-detail .hc')].find(x => /System/.test(x.textContent)); const r = c.getBoundingClientRect(); return { x: r.x, y: r.y, w: r.width, h: r.height }; });
    await switchToC(page);
    for (let i = 1; i <= 6; i++) for (let j = 1; j <= 5; j++) {
      await page.mouse.click(rect.x + rect.w * i / 7, rect.y + rect.h * j / 6);
    }
    await sleep(400);
    assert.deepEqual(requests.filter(r => r.method === 'POST'), []);
  });
});

test('8. VM이 바뀌면 전원 버튼이 새 VM으로 다시 고정된다 (옛 골격 노드는 문서에서 떨어진다)', async () => {
  const state = { list: [vm('A'), vm('B'), vm('C')] };
  await run(state, async (page, requests) => {
    await openSummary(page, 'B');
    await page.evaluate(() => { window.__oldStop = [...document.querySelectorAll('#vm-detail button')].find(x => x.textContent.includes('■')); });
    await page.evaluate(() => { window.selectedVmIndex = 2; refreshVmDetail(); });
    await page.waitForFunction(() => { const h = document.querySelector('#vm-detail h2'); return !!h && h.textContent === 'C'; }, { timeout: 3000 });
    assert.equal(await page.evaluate(() => window.__oldStop.isConnected), false);
    await page.evaluate(() => { [...document.querySelectorAll('#vm-detail button')].find(x => x.textContent.includes('■')).click(); });
    await sleep(400);
    assert.deepEqual(stopPosts(requests), ['/api/v1/vms/C/stop']);
  });
});
