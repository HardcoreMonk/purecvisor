import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const MODS_FULL = [
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
  'ui/modules/shell.js',
  'ui/app.js',
];

const FAIL_503 = { status: 503, body: { error: { code: -32000, message: 'Circuit breaker open' } } };
const FAIL_403 = { status: 403, body: { error: { code: 'FORBIDDEN', message: 'Insufficient role' } } };

function switchable(state, okBody) {
  return () => (state.fail ? FAIL_503 : { body: okBody });
}

async function boot(page, port) {
  await page.evaluate(async () => {
    const html = await fetch('/ui/index.html').then(response => response.text());
    const parsed = new DOMParser().parseFromString(html, 'text/html');
    document.body.innerHTML = parsed.body.innerHTML;
  });
  for (const moduleFile of MODS_FULL) {
    await page.addScriptTag({ url: `http://127.0.0.1:${port}/${moduleFile}` });
  }
  await page.evaluate(() => { I18N.setLang('ko'); });
}

function shellRoutes(state) {
  return {
    '/api/v1/containers': switchable(state, [{ name: 'c1', state: 'RUNNING' }, { name: 'c2', state: 'STOPPED' }]),
    '/api/v1/alerts': switchable(state, [{ severity: 'crit', message: 'disk', acknowledged: false }]),
    '/api/v1/storage/pools': switchable(state, [{ name: 'tank', size: '1T', alloc: '100G', health: 'ONLINE' }]),
    '/api/v1/monitor/fleet': switchable(state, { fleet: [{ name: 'n1' }] }),
    '/api/v1/networks': switchable(state, [{ name: 'net0' }]),
    '/api/v1/rpc': () => (state.fail ? FAIL_503 : { body: { jsonrpc: '2.0', id: 'x', result: [] } }),
    '/api/v1/metrics': { status: 404, body: null },
  };
}

test('셸 수집기: API 오류면 요약은 null, 수집한 원본은 그대로, 셸은 0이 아니라 알 수 없음이다', async () => {
  const state = { fail: false };
  await withPage([], async (page, { port }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await boot(page, port);
    const read = () => page.evaluate(async () => {
      window.authToken = 'test-token';
      await window._collectShellSlow();
      const s = window._shellSlow;
      return {
        ctrs: s.ctrs, alerts: s.alerts, storage: s.storage, healing: s.healing,
        rawCtrs: s.raw.ctrs.length, rawAlerts: s.raw.alerts.length, rawPools: s.raw.pools.length,
        ctrCnt: document.querySelector('#shell-sidebar [data-cnt="ctrs"]')?.textContent,
      };
    });
    const ok = await read();
    assert.deepEqual(ok.ctrs, { run: 1, total: 2 });
    assert.equal(ok.alerts.crit, 1);
    assert.equal(ok.storage.worstPool.name, 'tank');
    assert.equal(ok.ctrCnt, '1/2');

    state.fail = true;
    const down = await read();
    assert.equal(down.ctrs, null, '컨테이너 요약은 알 수 없음');
    assert.equal(down.alerts, null, '알림 요약은 알 수 없음');
    assert.equal(down.storage, null);
    assert.equal(down.healing, null);
    assert.equal(down.ctrCnt, '—', '사이드바는 0/0 이 아니라 —');

    assert.deepEqual([down.rawCtrs, down.rawAlerts, down.rawPools], [2, 1, 1]);
    assert.deepEqual(pageErrors, []);
  }, { routes: shellRoutes(state) });
});

test('대시보드 피드·자가치유: 조회 실패와 정말 없는 상태를 구분한다', async () => {
  const state = { fail: false };
  const routes = {
    ...shellRoutes(state),
    '/api/v1/containers': switchable(state, []),
    '/api/v1/alerts': switchable(state, []),
    '/api/v1/storage/pools': switchable(state, []),
  };
  await withPage([], async (page, { port }) => {
    await boot(page, port);
    const read = () => page.evaluate(async () => {
      window.authToken = 'test-token';
      await window._collectShellSlow();
      const feed = document.createElement('div');
      const heal = document.createElement('div');
      window._renderDashFeed(feed);
      window._renderDashHealing(heal);
      return { feed: feed.textContent, heal: heal.textContent };
    });
    const empty = await read();
    assert.match(empty.feed, /최근 이벤트 없음/);
    assert.match(empty.heal, /대기 없음/);
    assert.doesNotMatch(empty.feed, /불러오지 못했습니다/);

    state.fail = true;
    const down = await read();
    assert.match(down.feed, /불러오지 못했습니다/);
    assert.doesNotMatch(down.feed, /최근 이벤트 없음/);
    assert.match(down.heal, /불러오지 못했습니다/);
    assert.doesNotMatch(down.heal, /대기 없음/);
  }, { routes });
});

test('대시보드 첫 렌더: /vms 오류면 VM 0개로 그리지 않고 안내하며 셸 VM 요약도 알 수 없음이다', async () => {
  const state = { fail: true };
  const routes = { ...shellRoutes(state), '/api/v1/vms': switchable(state, []) };
  await withPage([], async (page, { port }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await boot(page, port);
    const read = () => page.evaluate(async () => {
      const host = document.createElement('div');
      document.body.appendChild(host);
      await window.renderDashboard(host);
      const out = {
        wl: host.querySelector('#dash-workloads').textContent,
        rows: host.querySelectorAll('#dash-workloads tbody tr').length,
        listError: !!window.vmListError,
      };
      window._pushShellSnapshot();
      out.vmCnt = document.querySelector('#shell-sidebar [data-cnt="vms"]')?.textContent;
      host.remove();
      return out;
    });
    const down = await read();
    assert.deepEqual(pageErrors, []);
    assert.equal(down.listError, true);
    assert.match(down.wl, /VM 목록을 불러오지 못했습니다/);
    assert.equal(down.rows, 0);
    assert.equal(down.vmCnt, '—', '사이드바는 0/0 이 아니라 —');

    state.fail = false;
    const ok = await read();
    assert.equal(ok.listError, false);
    assert.doesNotMatch(ok.wl, /불러오지 못했습니다/);
    assert.equal(ok.vmCnt, '0/0', '정말 없으면 0/0');
  }, { routes });
});

test('10초 폴러: 대시보드에 머무는 동안 /vms가 실패하면 표와 셸에 바로 반영된다', async () => {
  const state = { fail: false };
  const vms = [{ name: 'web-01', uuid: 'u-1', state: 'running', live_cpu_pct: 3 }];
  const routes = { ...shellRoutes(state), '/api/v1/vms': switchable(state, vms) };
  await withPage([], async (page, { port }) => {
    await boot(page, port);
    await page.evaluate(async () => {
      window.authToken = 'test-token';
      const host = document.createElement('div');
      document.body.appendChild(host);
      await window.renderDashboard(host);
      window.currentTab = 'dashboard';
      PCV.state && (PCV.state.currentTab = 'dashboard');
    });
    state.fail = true;
    const down = await page.evaluate(async () => {
      await window.loadAll();
      return {
        wl: document.getElementById('dash-workloads').textContent,
        vmCnt: document.querySelector('#shell-sidebar [data-cnt="vms"]')?.textContent,
        listError: !!window.vmListError,
      };
    });
    assert.equal(down.listError, true);
    assert.match(down.wl, /VM 목록을 불러오지 못했습니다/);
    assert.match(down.wl, /web-01/, '직전 목록은 유지한다');
    assert.equal(down.vmCnt, '—');
  }, { routes });
});

for (const failure of [FAIL_503, FAIL_403]) {
  test(`GPU 목록: ${failure.status}이면 "GPU 디바이스 없음"이 아니라 오류다`, async () => {
    const state = { body: failure };
    await withPage([], async (page, { port }) => {
      await boot(page, port);
      const read = () => page.evaluate(async () => {
        let host = document.getElementById('gpu-list-result');
        if (!host) {
          host = document.createElement('div');
          host.id = 'gpu-list-result';
          document.body.appendChild(host);
        }
        await window.testGpuList();
        return host.textContent;
      });
      const down = await read();
      assert.match(down, /GPU 목록을 불러오지 못했습니다/);
      assert.ok(down.includes(failure.body.error.message), '서버 사유를 함께 보인다');
      assert.doesNotMatch(down, /GPU 디바이스 없음/);
      state.body = { body: [] };
      assert.match(await read(), /GPU 디바이스 없음/);
    }, { routes: { '/api/v1/gpu/list': () => state.body } });
  });
}

const MODS_MOBILE = [
  'ui/modules/endpoints.js',
  'ui/modules/api.js',
  'ui/modules/ui.js',
  'ui/modules/filter-state.js',
  'ui/modules/uxlib.js',
  'ui/modules/mobile.js',
];

function mobileRoutes(state) {
  return {
    '/api/v1/vms': switchable(state, []),
    '/api/v1/containers': switchable(state, []),
    '/api/v1/alerts': switchable(state, []),
    '/api/v1/rpc': () => (state.fail ? FAIL_503 : { body: { jsonrpc: '2.0', id: 'x', result: [] } }),
  };
}

async function mobileTab(page, tab, marker) {
  await page.setViewport({ width: 400, height: 800 });
  await page.evaluate(() => {
    window.authToken = 'test-token';
    window.t = key => key;
    window.toast = () => {};
    window.PCV.mobile.mount();
    window.PCV.mobile.setBadges({ alerts: 5 });
  });
  await page.evaluate(name => window.PCV.mobile.showTab(name), tab);
  await page.waitForFunction(text => document.getElementById('m-screen')?.textContent.includes(text), { timeout: 5000 }, marker);
  return page.evaluate(() => ({
    text: document.getElementById('m-screen').textContent,
    bigs: Array.from(document.querySelectorAll('#m-screen .statusbar .seg-big'), node => node.textContent),
    badge: document.querySelector('.mnav .mnav-badge[data-badge="alerts"]')?.textContent,
  }));
}

test('모바일 홈: 오류면 VM·컨테이너·알림 수가 0이 아니라 알 수 없음이고 배지를 지우지 않는다', async () => {
  await withPage(MODS_MOBILE, async (page) => {
    const r = await mobileTab(page, 'home', '알림을 불러오지 못했습니다');
    assert.deepEqual(r.bigs, ['—', '—', '—']);
    assert.doesNotMatch(r.text, /최근 알림 없음/);
    assert.equal(r.badge, '5', '모르는 동안 직전 배지를 0으로 덮지 않는다');
  }, { routes: mobileRoutes({ fail: true }) });
});

test('모바일 홈: 정상 빈 응답은 0과 "최근 알림 없음"이다', async () => {
  await withPage(MODS_MOBILE, async (page) => {
    const r = await mobileTab(page, 'home', '최근 알림 없음');
    assert.deepEqual(r.bigs, ['0', '0', '0']);
    assert.equal(r.badge, '0');
  }, { routes: mobileRoutes({ fail: false }) });
});

test('모바일 알림: 오류면 "알림 없음"이 아니라 오류 안내다', async () => {
  await withPage(MODS_MOBILE, async (page) => {
    const r = await mobileTab(page, 'alerts', '알림을 불러오지 못했습니다');
    assert.doesNotMatch(r.text, /알림 없음/);
    assert.doesNotMatch(r.text, /심각 0/);
    assert.equal(r.badge, '5');
  }, { routes: mobileRoutes({ fail: true }) });
});

test('모바일 알림: 정상 빈 응답은 "알림 없음"이다', async () => {
  await withPage(MODS_MOBILE, async (page) => {
    const r = await mobileTab(page, 'alerts', '알림 없음');
    assert.doesNotMatch(r.text, /불러오지 못했습니다/);
  }, { routes: mobileRoutes({ fail: false }) });
});

test('모바일 전원: 오류면 "VM 없음"이 아니라 오류 안내다', async () => {
  await withPage(MODS_MOBILE, async (page) => {
    const r = await mobileTab(page, 'power', 'VM 목록을 불러오지 못했습니다');
    assert.match(r.text, /컨테이너 목록을 불러오지 못했습니다/);
    assert.doesNotMatch(r.text, /VM 없음/);
    assert.doesNotMatch(r.text, /컨테이너 없음/);
    assert.doesNotMatch(r.text, /VM 0/);
  }, { routes: mobileRoutes({ fail: true }) });
});

test('모바일 전원: 조회 실패 동안 삭제 선택을 지우지 않는다', async () => {
  await withPage(MODS_MOBILE, async (page) => {
    const r = await page.evaluate(() => {
      const build = window.PCV.mobile.buildPower;
      const first = build({ vms: [{ name: 'web-01', uuid: 'u-1', state: 'shutoff' }], containers: [] });
      const box = first.querySelector('input[type="checkbox"]');
      box.checked = true;
      box.dispatchEvent(new Event('change', { bubbles: true }));
      const during = build({ vms: null, containers: null });
      const after = build({ vms: [{ name: 'web-01', uuid: 'u-1', state: 'shutoff' }], containers: [] });
      return {
        during: during.querySelector('.m-vm-bulk-delete').textContent,
        after: after.querySelector('.m-vm-bulk-delete').textContent,
      };
    });
    assert.match(r.during, /\(1\)/);
    assert.match(r.after, /\(1\)/);
  });
});

test('모바일 전원: 정상 빈 응답은 "VM 없음"이다', async () => {
  await withPage(MODS_MOBILE, async (page) => {
    const r = await mobileTab(page, 'power', 'VM 없음');
    assert.match(r.text, /컨테이너 없음/);
    assert.doesNotMatch(r.text, /불러오지 못했습니다/);
  }, { routes: mobileRoutes({ fail: false }) });
});

test('모바일 배지 갱신: 오류면 직전 배지를 유지한다', async () => {
  const state = { fail: false };
  await withPage(MODS_MOBILE, async (page) => {

    await mobileTab(page, 'home', '최근 알림 없음');
    const refresh = () => page.evaluate(async () => {
      window.PCV.mobile.setBadges({ alerts: 7 });
      await window.PCV.mobile._refreshBadges();
      return document.querySelector('.mnav .mnav-badge[data-badge="alerts"]').textContent;
    });
    assert.equal(await refresh(), '0', '정상 빈 응답은 0으로 갱신');
    state.fail = true;
    assert.equal(await refresh(), '7');
  }, { routes: mobileRoutes(state) });
});

test('스냅샷 목록: 조회 실패는 "스냅샷 없음"이 아니라 오류 안내이고 빈 목록과 구분된다', async () => {
  const state = { fail: true };
  await withPage([], async (page, { port }) => {
    await boot(page, port);
    const read = () => page.evaluate(async () => {
      const host = document.createElement('div');
      document.body.appendChild(host);
      await window.renderSnapshots(host, { name: 'web-01', uuid: 'u-1' });
      const out = {
        tree: document.getElementById('stree').textContent,
        none: window.t('snap.none'),
        rows: host.querySelectorAll('#stree tbody tr').length,
      };
      host.remove();
      return out;
    });
    const down = await read();
    assert.match(down.tree, /스냅샷 목록을 불러오지 못했습니다/);
    assert.ok(down.tree.includes('Circuit breaker open'), '서버 사유를 함께 보인다');
    assert.ok(!down.tree.includes(down.none), '"스냅샷 없음"으로 보이면 안 된다');

    state.fail = false;
    const empty = await read();
    assert.ok(empty.tree.includes(empty.none));
    assert.doesNotMatch(empty.tree, /불러오지 못했습니다/);
  }, { routes: { '/api/v1/vms/web-01/snapshot': switchable(state, []) } });
});

test('스냅샷 일괄 삭제: 조회 실패는 "지울 스냅샷 없음"이 아니라 실패 안내다', async () => {
  await withPage([], async (page, { port }) => {
    await boot(page, port);
    const toasts = await page.evaluate(async () => {
      const seen = [];
      window.toast = (message, ok) => seen.push({ message, ok });
      await window.snapDeleteAll('web-01');
      return seen;
    });
    assert.equal(toasts.length, 1);
    assert.match(toasts[0].message, /Failed to load snapshots/);
    assert.equal(toasts[0].ok, false);
  }, { routes: { '/api/v1/vms/web-01/snapshot': FAIL_503 } });
});
