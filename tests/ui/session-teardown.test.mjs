                                                                        
                                                                   
                                                                         
                                                       
                                                        
                                        
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

test('(1): 로그아웃은 Bearer + 본문 refresh 로 /auth/logout 을 1회 보내고 /auth/refresh 는 0회, 로컬 정리는 즉시 끝난다', async () => {
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
    assert.equal(result.revoked, true);
    const logout = result.log.filter(r => r.url.endsWith('/api/v1/auth/logout'));
    assert.equal(logout.length, 1, '서버 로그아웃은 정확히 1회');
    assert.equal(logout[0].auth, 'Bearer acc.tok.one');
    assert.deepEqual(JSON.parse(logout[0].body), { refresh_token: 'refresh-one' }, '이 탭의 refresh 를 본문으로 보낸다');
    assert.equal(result.log.filter(r => r.url.endsWith('/auth/refresh')).length, 0, '로그아웃이 토큰 회전을 일으키면 안 된다');
    assert.deepEqual(requests.find(r => r.path === '/api/v1/auth/logout').json, { refresh_token: 'refresh-one' });
  }, { routes: { '/api/v1/auth/logout': { body: { status: 'ok', scope: 'session', session_revoked: true, message: 'Session revoked' } } } });
});

test('(2): access 가 만료돼 서버가 401 이어도 재시도·/auth/refresh 없이 요청 1회로 끝난다', async () => {
  await withPage([], async (page, { port }) => {
    await boot(page, port);
    await loginState(page, 'acc.expired.x', 'refresh-two');
    const result = await page.evaluate(async () => ({ revoked: await window.doLogout(), log: window.__fetchLog }));
    assert.equal(result.revoked, false, '401 은 서버 확인 실패로 resolve 한다(던지지 않는다)');
    const authCalls = result.log.filter(r => /\/api\/v1\/auth\/(logout|refresh)/.test(r.url));
    assert.deepEqual(authCalls.map(r => r.url.replace(/^.*\/api\/v1/, '')), ['/auth/logout']);
    assert.deepEqual(JSON.parse(authCalls[0].body), { refresh_token: 'refresh-two' });
  }, { routes: { '/api/v1/auth/logout': { status: 401, body: { error: { code: 'UNAUTHORIZED', message: 'Valid token required for logout' } } } } });
});

test('(3): 모든 기기 로그아웃은 확인 뒤 /auth/logout/all 만 1회 보내고, 취소하면 요청도 정리도 없다', async () => {
  await withPage([], async (page, { port }) => {
    await boot(page, port);
    await loginState(page, 'acc.all', 'refresh-all');
    const choose = async which => page.evaluate(async sel => {
      const pending = window.doLogoutAll();
      for (let i = 0; i < 40 && !document.querySelector('dialog[open] ' + sel); i++) {
        await new Promise(r => setTimeout(r, 25));
      }
      const focused = document.activeElement && document.activeElement.hasAttribute('data-confirm-cancel');
      document.querySelector('dialog[open] ' + sel).click();
      const revoked = await pending;
      return {
        revoked, focused,
        token: window.authToken,
        stored: sessionStorage.getItem('pcv_refresh_token'),
        log: window.__fetchLog.filter(r => /\/auth\/logout/.test(r.url)),
      };
    }, which);

    const cancelled = await choose('[data-confirm-cancel]');
    assert.equal(cancelled.focused, true, 'destructive 확인의 초기 focus 는 취소(DESIGN.md)');
    assert.equal(cancelled.revoked, false);
    assert.equal(cancelled.token, 'acc.all', '취소하면 로그인 상태가 그대로다');
    assert.equal(cancelled.stored, 'refresh-all');
    assert.equal(cancelled.log.length, 0, '취소하면 요청 0회');

    const accepted = await choose('[data-confirm-accept]');
    assert.equal(accepted.revoked, true);
    assert.equal(accepted.token, '');
    assert.equal(accepted.stored, null);
    assert.deepEqual(accepted.log.map(r => r.url.replace(/^.*\/api\/v1/, '')), ['/auth/logout/all']);
    assert.equal(accepted.log[0].auth, 'Bearer acc.all');
    assert.deepEqual(JSON.parse(accepted.log[0].body), { refresh_token: 'refresh-all' });
  }, { routes: { '/api/v1/auth/logout/all': { body: { status: 'ok', scope: 'all', message: 'All sessions revoked' } } } });
});

test('(4): 갱신 중 로그아웃하면 늦게 온 회전 결과를 저장하지 않고 회전된 refresh 로 /auth/logout 을 1회 더 보낸다', async () => {
  let releaseRefresh;
  const refreshGate = new Promise(resolve => { releaseRefresh = resolve; });
  await withPage([], async (page, { port }) => {
    await boot(page, port);
                                                                  
    await page.exposeFunction('__releaseRefresh', () => releaseRefresh());
    await loginState(page, 'acc.race', 'refresh-race');

    const result = await page.evaluate(async () => {
      const refreshing = PCV.api._tryRefreshToken();
      for (let i = 0; i < 40 && !window.__fetchLog.some(r => r.url.endsWith('/auth/refresh')); i++) {
        await new Promise(r => setTimeout(r, 25));
      }
      window.doLogout();
      window.__releaseRefresh();
      const refreshed = await refreshing;
      for (let i = 0; i < 80 && window.__fetchLog.filter(r => r.url.endsWith('/api/v1/auth/logout')).length < 2; i++) {
        await new Promise(r => setTimeout(r, 25));
      }
                                                                
      window.authToken = 'acc.next';
      sessionStorage.setItem('pcv_refresh_token', 'refresh-next');
      const fresh = await PCV.api._tryRefreshToken();
      return {
        refreshed, fresh,
        token: window.authToken,
        stored: sessionStorage.getItem('pcv_refresh_token'),
        log: window.__fetchLog.filter(r => /\/auth\/(logout|refresh)$/.test(r.url))
          .map(r => ({ path: r.url.replace(/^.*\/api\/v1/, ''), auth: r.auth, body: r.body && JSON.parse(r.body) })),
      };
    });

    assert.equal(result.refreshed, false, '세대가 바뀐 갱신은 실패로 끝난다');
    assert.deepEqual(result.log.slice(0, 3), [
      { path: '/auth/refresh', auth: null, body: { refresh_token: 'refresh-race' } },
      { path: '/auth/logout', auth: 'Bearer acc.race', body: { refresh_token: 'refresh-race' } },
      { path: '/auth/logout', auth: null, body: { refresh_token: 'refresh-rotated' } },
    ]);
    assert.equal(result.fresh, true, '새 로그인의 갱신은 새로 나가야 한다');
    assert.deepEqual(result.log[3], { path: '/auth/refresh', auth: null, body: { refresh_token: 'refresh-next' } });
    assert.equal(result.stored, 'refresh-next-2');
  }, {
    routes: {
      '/api/v1/auth/refresh': async record => {
        if (record.json && record.json.refresh_token === 'refresh-race') {
          await refreshGate;
          return { body: { access_token: 'acc.rotated', refresh_token: 'refresh-rotated' } };
        }
        return { body: { access_token: 'acc.next-2', refresh_token: 'refresh-next-2' } };
      },
      '/api/v1/auth/logout': { body: { status: 'ok', scope: 'session', session_revoked: true, message: 'Session revoked' } },
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

test('(5): L7 — 옛 갱신 진행 중 재로그인하면 새 갱신은 공유하지 않고, 옛 finally 가 새 슬롯을 지우지 않는다', async () => {
  let releaseOld, releaseNew;
  const oldGate = new Promise(r => { releaseOld = r; });
  const newGate = new Promise(r => { releaseNew = r; });
  await withPage([], async (page, { port }) => {
    await boot(page, port);
    await page.exposeFunction('__releaseOld', () => releaseOld());
    await page.exposeFunction('__releaseNew', () => releaseNew());
    await loginState(page, 'acc.old', 'refresh-old');
    const result = await page.evaluate(async () => {
      const count = () => window.__fetchLog.filter(r => r.url.endsWith('/auth/refresh')).length;
      const stale = PCV.api._tryRefreshToken();
      for (let i = 0; i < 40 && count() < 1; i++) await new Promise(r => setTimeout(r, 25));
      window.doLogout();
      window.authToken = 'acc.next';
      sessionStorage.setItem('pcv_refresh_token', 'refresh-next');
      const fresh = PCV.api._tryRefreshToken();                             
      for (let i = 0; i < 40 && count() < 2; i++) await new Promise(r => setTimeout(r, 25));
      const afterFresh = count();
      window.__releaseOld();
      const staleResult = await stale;                                      
      const third = PCV.api._tryRefreshToken();                                        
      await new Promise(r => setTimeout(r, 100));
      const afterThird = count();
      window.__releaseNew();
      return { afterFresh, afterThird, staleResult, freshResult: await fresh, thirdResult: await third,
               stored: sessionStorage.getItem('pcv_refresh_token') };
    });
    assert.equal(result.afterFresh, 2, '재로그인 탭의 갱신은 새 요청을 내야 한다(옛 Promise 공유 금지)');
    assert.equal(result.staleResult, false);
    assert.equal(result.afterThird, 2, '옛 finally 가 새 슬롯을 비우면 세 번째 호출이 갱신을 또 보낸다');
    assert.equal(result.freshResult, true);
    assert.equal(result.thirdResult, true);
    assert.equal(result.stored, 'refresh-next-2');
  }, { routes: {
    '/api/v1/auth/refresh': async record => {
      if (record.json.refresh_token === 'refresh-old') { await oldGate; return { body: { access_token: 'acc.rot', refresh_token: 'refresh-rot' } }; }
      await newGate; return { body: { access_token: 'acc.next-2', refresh_token: 'refresh-next-2' } };
    },
    '/api/v1/auth/logout': { body: { status: 'ok', scope: 'session', session_revoked: true } },
  } });
});

                                                                                
test('(6): O1 — 로그아웃·모든 기기·만료 뒤 #login-pass 는 비어 있고 type=password 로 돌아온다', async () => {
  await withPage([], async (page, { port }) => {
    await boot(page, port);
    for (const exit of ['logout', 'all', 'expire']) {
      const state = await page.evaluate(async exitPath => {
        document.getElementById('login-user').value = 'alice';
        const pass = document.getElementById('login-pass');
        pass.value = 'Prev-Secret-1';
        document.querySelector('.login-pass-toggle').click();                            
        await window.doLoginPage();
        const afterLogin = { value: pass.value, type: pass.type, loggedIn: !!window.authToken };
        if (exitPath === 'logout') window.doLogout();
        else if (exitPath === 'expire') PCV.api._redirectToLogin();
        else {
          const pending = window.doLogoutAll();
          for (let i = 0; i < 40 && !document.querySelector('dialog[open] [data-confirm-accept]'); i++) await new Promise(r => setTimeout(r, 25));
          document.querySelector('dialog[open] [data-confirm-accept]').click();
          await pending;
        }
        return { afterLogin, value: pass.value, type: pass.type,
                 icon: document.querySelector('.login-pass-toggle use').getAttribute('href'),
                 shown: document.getElementById('login-page').style.display };
      }, exit);
      assert.equal(state.afterLogin.loggedIn, true, exit + ': 로그인 성공 전제');
      assert.equal(state.shown, 'flex', exit);
      assert.equal(state.value, '', exit + ': 직전 비밀번호가 로그인 칸에 남으면 안 된다');
      assert.equal(state.type, 'password', exit + ': 표시 전환 상태가 남으면 평문으로 보인다');
      assert.equal(state.icon, '#pcv-i-eye', exit);
    }
  }, { routes: {
    '/api/v1/auth/token': { body: { access_token: 'acc.o1', refresh_token: 'refresh-o1' } },
    '/api/v1/auth/logout': { body: { status: 'ok', scope: 'session', session_revoked: true } },
    '/api/v1/auth/logout/all': { body: { status: 'ok', scope: 'all' } },
  } });
});

                                                                             
                                                                
test('(7): N1 — 모든 기기 로그아웃 중 늦게 온 회전 refresh 는 /auth/logout/all 로 폐기한다', async () => {
  let release;
  const gate = new Promise(r => { release = r; });
  await withPage([], async (page, { port }) => {
    await boot(page, port);
    await page.exposeFunction('__release', () => release());
    await loginState(page, 'acc.expired', 'refresh-r0');
    const log = await page.evaluate(async () => {
      const refreshing = PCV.api._tryRefreshToken();
      for (let i = 0; i < 40 && !window.__fetchLog.some(r => r.url.endsWith('/auth/refresh')); i++) await new Promise(r => setTimeout(r, 25));
      const pending = window.doLogoutAll();
      for (let i = 0; i < 40 && !document.querySelector('dialog[open] [data-confirm-accept]'); i++) await new Promise(r => setTimeout(r, 25));
      document.querySelector('dialog[open] [data-confirm-accept]').click();
      await pending;
      window.__release();
      await refreshing;
      for (let i = 0; i < 80 && window.__fetchLog.filter(r => /\/auth\/logout/.test(r.url)).length < 2; i++) await new Promise(r => setTimeout(r, 25));
      return window.__fetchLog.filter(r => /\/auth\/logout/.test(r.url))
        .map(r => ({ path: r.url.replace(/^.*\/api\/v1/, ''), body: r.body && JSON.parse(r.body) }));
    });
    assert.deepEqual(log, [
      { path: '/auth/logout/all', body: { refresh_token: 'refresh-r0' } },
      { path: '/auth/logout/all', body: { refresh_token: 'refresh-r1' } },
    ]);
  }, { routes: {
    '/api/v1/auth/refresh': async () => { await gate; return { body: { access_token: 'acc.r1', refresh_token: 'refresh-r1' } }; },
    '/api/v1/auth/logout/all': { status: 401, body: { error: { code: 'UNAUTHORIZED' } } },
    '/api/v1/auth/logout': { body: { status: 'ok', scope: 'session', session_revoked: true } },
  } });
});
