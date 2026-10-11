





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
  await page.waitForFunction(() => typeof window.doLogout === 'function' && window.PCV.modalCore);
  await page.evaluate(() => {
    window.__fetchLog = [];
    const previousFetch = window.fetch;
    window.fetch = function (url, opts) {
      const headers = (opts && opts.headers) || {};
      window.__fetchLog.push({
        url: String(url),
        method: (opts && opts.method) || 'GET',
        auth: headers.Authorization || headers.authorization || null,
        body: opts && typeof opts.body === 'string' ? opts.body : null,
      });
      return previousFetch.apply(this, arguments);
    };
    window.__sockets = [];
    class FakeWS {
      static OPEN = 1;
      constructor(url) {
        this.url = url;
        this.readyState = 0;
        window.__sockets.push(this);
      }
      send() {}
      close() {
        if (this.readyState === 3) return;
        this.readyState = 3;
        setTimeout(() => { if (typeof this.onclose === 'function') this.onclose({}); }, 0);
      }
    }
    window.WebSocket = FakeWS;
    window.loadAll = function () {};
    window.__openSockets = () => window.__sockets.filter(s => s.readyState !== 3).length;
  });
}


async function loginState(page, access, refresh) {
  await page.evaluate((accessToken, refreshToken) => {
    window.authToken = accessToken;
    sessionStorage.setItem('pcv_token', accessToken);
    sessionStorage.setItem('pcv_refresh_token', refreshToken);
    sessionStorage.setItem('pcv_user', 'alice');
    window.currentUser = { username: 'alice', role: 'ADMIN' };
    window.pcvSetLoginVisible(false);
    PCV.api.connectWS();
    const ws = window.wsConnection;
    ws.readyState = 1;
    ws.onopen();
    ws.onmessage({ data: JSON.stringify({ type: 'auth_ok' }) });
  }, access, refresh);
}

test('로그아웃은 Bearer access token 으로 서버 /auth/logout 을 호출하고 로컬 정리는 즉시 끝난다', async () => {
  await withPage([], async (page, { port, requests }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await boot(page, port);
    await loginState(page, 'acc.tok.one', 'refresh-one');

    const result = await page.evaluate(async () => {
      const pending = window.doLogout();
      const syncState = {
        token: window.authToken,
        stored: ['pcv_token', 'pcv_refresh_token', 'pcv_user'].map(k => sessionStorage.getItem(k)),
        loginShown: document.getElementById('login-page').style.display,
      };
      const revoked = await pending;
      return { syncState, revoked, log: window.__fetchLog };
    });

    assert.deepEqual(pageErrors, []);
    assert.equal(result.syncState.token, '', '토큰은 서버 응답을 기다리지 않고 즉시 비워져야 한다');
    assert.deepEqual(result.syncState.stored, [null, null, null]);
    assert.equal(result.syncState.loginShown, 'flex');
    assert.equal(result.revoked, true, '서버 폐기 성공을 resolve 해야 한다');
    const logout = result.log.filter(r => r.url.endsWith('/api/v1/auth/logout'));
    assert.equal(logout.length, 1, '서버 로그아웃은 정확히 1회');
    assert.equal(logout[0].method, 'POST');
    assert.equal(logout[0].auth, 'Bearer acc.tok.one', '지우기 전 access token 사본으로 인증해야 한다');
    assert.ok(requests.some(r => r.method === 'POST' && r.path === '/api/v1/auth/logout'));
  }, { routes: { '/api/v1/auth/logout': { body: { status: 'ok', message: 'All sessions revoked' } } } });
});

test('access token 이 만료(401)면 refresh token 으로 재발급받아 서버 로그아웃을 완료한다', async () => {
  let logoutCalls = 0;
  await withPage([], async (page, { port, requests }) => {
    await boot(page, port);
    await loginState(page, 'acc.expired.x', 'refresh-two');

    const result = await page.evaluate(async () => {
      const revoked = await window.doLogout();
      return { revoked, log: window.__fetchLog };
    });

    assert.equal(result.revoked, true);
    const authCalls = result.log.filter(r => /\/api\/v1\/auth\/(logout|refresh)$/.test(r.url));
    assert.deepEqual(authCalls.map(r => r.url.replace(/^.*\/api\/v1/, '') + ' ' + (r.auth || '-')), [
      '/auth/logout Bearer acc.expired.x',
      '/auth/refresh -',
      '/auth/logout Bearer acc.fresh.y',
    ]);
    const refresh = requests.find(r => r.path === '/api/v1/auth/refresh');
    assert.equal(refresh.json.refresh_token, 'refresh-two');
  }, {
    routes: {
      '/api/v1/auth/logout': () => (++logoutCalls === 1
        ? { status: 401, body: { error: { code: 'UNAUTHORIZED', message: 'Valid token required for logout' } } }
        : { body: { status: 'ok' } }),
      '/api/v1/auth/refresh': { body: { access_token: 'acc.fresh.y', refresh_token: 'refresh-rotated' } },
    },
  });
});

test('서버 로그아웃이 네트워크 오류여도 클라이언트 정리는 완료되고 예외를 던지지 않는다', async () => {
  await withPage([], async (page, { port }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await boot(page, port);
    await loginState(page, 'acc.tok.net', 'refresh-net');

    const result = await page.evaluate(async () => {
      const previousFetch = window.fetch;
      window.fetch = function (url) {
        if (String(url).includes('/auth/')) return Promise.reject(new TypeError('Failed to fetch'));
        return previousFetch.apply(this, arguments);
      };
      let threw = false;
      let revoked;
      try { revoked = await window.doLogout(); } catch (e) { threw = true; }
      return {
        threw,
        revoked,
        token: window.authToken,
        refresh: sessionStorage.getItem('pcv_refresh_token'),
        loginShown: document.getElementById('login-page').style.display,
      };
    });

    assert.deepEqual(pageErrors, []);
    assert.equal(result.threw, false);
    assert.equal(result.revoked, false);
    assert.equal(result.token, '');
    assert.equal(result.refresh, null);
    assert.equal(result.loginShown, 'flex');
  });
});

test('로그아웃 후 즉시 재로그인해도 소켓은 정확히 1개만 열려 있고 재연결 루프가 새 소켓을 만들지 않는다', async () => {
  await withPage([], async (page, { port }) => {
    await boot(page, port);
    await loginState(page, 'acc.first', 'refresh-a');

    await page.evaluate(() => {
      window.__firstSocket = window.wsConnection;
      window.doLogout();

      window.authToken = 'acc.second';
      PCV.api.connectWS();
      window.__secondSocket = window.wsConnection;
    });

    await new Promise(resolve => setTimeout(resolve, 2600));
    const state = await page.evaluate(() => ({
      created: window.__sockets.length,
      open: window.__openSockets(),
      firstClosed: window.__firstSocket.readyState === 3,
      currentIsSecond: window.wsConnection === window.__secondSocket,
    }));

    assert.equal(state.firstClosed, true, '로그아웃이 첫 소켓을 닫아야 한다');
    assert.equal(state.created, 2, '재로그인 소켓 외에 재연결 루프가 소켓을 더 만들면 안 된다');
    assert.equal(state.open, 1, '열린 소켓은 하나뿐이어야 한다');
    assert.equal(state.currentIsSecond, true);
  });
});

test('connectWS 재호출은 기존 소켓을 닫고 교체하며, 닫힌 옛 소켓이 재연결을 예약하지 않는다', async () => {
  await withPage([], async (page, { port }) => {
    await boot(page, port);
    await loginState(page, 'acc.dup', 'refresh-dup');

    await page.evaluate(() => { PCV.api.connectWS(); });
    await new Promise(resolve => setTimeout(resolve, 2600));
    const state = await page.evaluate(() => ({
      created: window.__sockets.length,
      open: window.__openSockets(),
      current: window.__sockets.indexOf(window.wsConnection),
    }));

    assert.equal(state.created, 2, '교체 소켓 1개만 추가되고 옛 소켓 onclose 가 재연결하면 안 된다');
    assert.equal(state.open, 1);
    assert.equal(state.current, 1, '현재 소켓은 마지막에 만든 소켓이어야 한다');
  });
});

test('비의도적 끊김은 지금처럼 백오프 재연결한다(회귀 방지)', async () => {
  await withPage([], async (page, { port }) => {
    await boot(page, port);
    await loginState(page, 'acc.drop', 'refresh-drop');

    await page.evaluate(() => {

      const ws = window.wsConnection;
      ws.readyState = 3;
      ws.onclose({});
    });
    await new Promise(resolve => setTimeout(resolve, 2600));
    const state = await page.evaluate(() => ({
      created: window.__sockets.length,
      open: window.__openSockets(),
    }));
    assert.equal(state.created, 2, '장애 끊김 뒤에는 재연결 소켓이 생겨야 한다');
    assert.equal(state.open, 1);
  });
});

test('로그아웃·세션 만료는 열린 모달(noDismiss 포함)과 currentUser 를 정리한다', async () => {
  await withPage([], async (page, { port }) => {
    await boot(page, port);

    for (const exit of ['logout', 'expire']) {
      await loginState(page, 'acc.modal.' + exit, 'refresh-modal');
      const state = await page.evaluate(exitPath => {
        const el = PCV.uxlib.el;
        window.showModal([el('h2', null, 'VM settings'), el('input', { id: 'modal-field' })]);
        window.showModal([el('h2', null, 'Recovery codes')], { noDismiss: true });
        const before = document.querySelectorAll('dialog[open]').length;
        if (exitPath === 'logout') window.doLogout();
        else PCV.api._redirectToLogin();
        return {
          before,
          after: document.querySelectorAll('dialog[open]').length,
          stack: PCV.modalCore.currentDialog(),
          user: window.currentUser,
          loginInert: document.getElementById('login-page').inert,
        };
      }, exit);

      assert.equal(state.before, 2, exit);
      assert.equal(state.after, 0, exit + ': 열린 dialog 가 남으면 로그인 폼이 inert 로 막힌다');
      assert.equal(state.stack, null, exit);
      assert.equal(state.user, null, exit + ': 이전 사용자 role 이 남으면 안 된다');
      assert.equal(state.loginInert, false, exit);
    }
  }, { routes: { '/api/v1/auth/logout': { body: { status: 'ok' } } } });
});

test('세션 만료(_redirectToLogin)도 소켓을 닫고 재연결을 예약하지 않는다', async () => {
  await withPage([], async (page, { port }) => {
    await boot(page, port);
    await loginState(page, 'acc.exp', 'refresh-exp');

    await page.evaluate(() => {
      PCV.api._redirectToLogin();

      window.authToken = 'acc.late';
    });
    await new Promise(resolve => setTimeout(resolve, 2600));
    const state = await page.evaluate(() => ({
      created: window.__sockets.length,
      open: window.__openSockets(),
      current: window.wsConnection,
    }));
    assert.equal(state.created, 1);
    assert.equal(state.open, 0);
    assert.equal(state.current, null);
  });
});
