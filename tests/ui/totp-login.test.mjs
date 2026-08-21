                                                                              
                                                           
                                                                      
  
                                
  
                  
                                                         
                                                                     
                                                      
                                                    
                                                                           
                                                            
                                                                  
   
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
];

function routes(tokenBody, verifyReply) {
  return {
    '/api/v1/auth/token': { body: tokenBody },
    '/api/v1/auth/totp/verify': verifyReply,
    '/api/v1/vms': { body: { data: [] } },
  };
}

async function bootLogin(page, port) {
  await page.evaluate(async () => {
    const html = await fetch('/ui/index.html').then(response => response.text());
    const parsed = new DOMParser().parseFromString(html, 'text/html');
    document.body.innerHTML = parsed.body.innerHTML;
  });
  for (const moduleFile of MODS) {
    await page.addScriptTag({ url: `http://127.0.0.1:${port}/${moduleFile}` });
  }
  await page.waitForFunction(
    () => typeof window.doLoginPage === 'function' && document.getElementById('login-totp-step'),
    { timeout: 3000 }
  );
                                                                       
  await page.evaluate(() => {
    function FakeWS() { this.readyState = 3; }
    FakeWS.OPEN = 1;
    FakeWS.prototype.send = function () {};
    FakeWS.prototype.close = function () {};
    window.WebSocket = FakeWS;
    window.loadAll = function () {};
  });
}

async function submitCredentials(page) {
  return page.evaluate(async () => {
    document.getElementById('login-user').value = 'alice';
    document.getElementById('login-pass').value = 'password1';
    await window.doLoginPage();
    return {
      stepHidden: document.getElementById('login-totp-step').hidden,
      credHidden: document.getElementById('login-cred-fields').hidden,
      actionsHidden: document.getElementById('login-cred-actions').hidden,
    };
  });
}

test('로그인 2단계: totp_required 는 코드 입력 스텝을 노출한다', async () => {
  await withPage([], async (page, { port }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await bootLogin(page, port);

    const shown = await submitCredentials(page);

    assert.deepEqual(pageErrors, []);
    assert.equal(shown.stepHidden, false, '#login-totp-step 가 노출돼야 한다');
    assert.equal(shown.credHidden, true, '자격증명 필드는 숨어야 한다');
    assert.equal(shown.actionsHidden, true, '1단계 제출/보조 링크는 숨어야 한다');
  }, { routes: routes({ totp_required: true, pending_token: 'p.p.p' }, { body: { access_token: 'a.a.a', token_type: 'Bearer', expires_in: 900 } }) });
});

test('로그인 2단계: 코드 검증 성공이 로그인을 완성한다', async () => {
  await withPage([], async (page, { port, requests }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await bootLogin(page, port);

    await submitCredentials(page);
    const result = await page.evaluate(async () => {
      document.getElementById('login-totp-code').value = '123456';
      await window.doLoginTotpSubmit();
      return {
        display: document.getElementById('login-page').style.display,
        token: window.authToken,
      };
    });

    assert.deepEqual(pageErrors, []);
    assert.equal(result.display, 'none', '로그인 오버레이가 닫혀야 한다');
    assert.equal(result.token, 'a.a.a', '정식 토큰이 세팅돼야 한다');
    const verifyReq = requests.find(r => r.path === '/api/v1/auth/totp/verify');
    assert.ok(verifyReq, 'verify 가 호출돼야 한다');
    assert.equal(verifyReq.json.pending_token, 'p.p.p');
    assert.equal(verifyReq.json.code, '123456');
  }, { routes: routes({ totp_required: true, pending_token: 'p.p.p' }, { body: { access_token: 'a.a.a', token_type: 'Bearer', expires_in: 900 } }) });
});

test('로그인 2단계: 코드 오류는 스텝을 유지하고 오류 문구를 남긴다', async () => {
  await withPage([], async (page, { port }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await bootLogin(page, port);

    await submitCredentials(page);
    const result = await page.evaluate(async () => {
      document.getElementById('login-totp-code').value = '000000';
      await window.doLoginTotpSubmit();
      return {
        stepHidden: document.getElementById('login-totp-step').hidden,
        err: document.getElementById('login-err').textContent,
        display: document.getElementById('login-page').style.display || '',
      };
    });

    assert.deepEqual(pageErrors, []);
    assert.equal(result.stepHidden, false, '코드 스텝은 그대로 유지돼야 한다');
    assert.ok(result.err && result.err.length > 0, '오류 문구가 있어야 한다');
    assert.notEqual(result.display, 'none', '로그인 오버레이는 닫히지 않아야 한다');
  }, { routes: routes({ totp_required: true, pending_token: 'p.p.p' }, { status: 401, body: { error: { code: 'TOTP_INVALID_CODE', message: 'Invalid TOTP code' } } }) });
});
