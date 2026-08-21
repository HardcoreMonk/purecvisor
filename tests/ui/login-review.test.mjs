                                                                                 
                                                                   
                                                                   
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const LOGIN_MODS = [
  'ui/i18n.js', 'ui/modules/endpoints.js', 'ui/modules/api.js',
  'ui/modules/ui.js', 'ui/modules/filter-state.js', 'ui/modules/uxlib.js',
];

async function mountLoginMarkup(page) {
  await page.evaluate(async () => {
    const base = document.createElement('base');
    base.href = '/ui/';
    document.head.appendChild(base);
    const html = await fetch('/ui/index.html').then(response => response.text());
    const parsed = new DOMParser().parseFromString(html, 'text/html');
    document.body.innerHTML = parsed.body.innerHTML;
    document.getElementById('splash')?.remove();
  });
}

test('login keeps desktop split and puts authentication first at 860/480px without overflow', async () => {
  await withPage([], async (page) => {
    await mountLoginMarkup(page);

    await page.setViewport({ width: 1280, height: 800 });
    const desktop = await page.evaluate(() => {
      const pitch = document.querySelector('.login-pitch').getBoundingClientRect();
      const aside = document.querySelector('.login-aside').getBoundingClientRect();
      return {
        pitchLeft: pitch.left,
        asideLeft: aside.left,
        overflow: document.getElementById('login-page').scrollWidth > innerWidth,
      };
    });
    assert.ok(desktop.pitchLeft < desktop.asideLeft);
    assert.equal(desktop.overflow, false);

    for (const viewport of [{ width: 860, height: 900 }, { width: 480, height: 900 }]) {
      await page.setViewport(viewport);
      const mobile = await page.evaluate(() => {
        const overlay = document.getElementById('login-page');
        overlay.scrollTop = 0;
        const pitch = document.querySelector('.login-pitch').getBoundingClientRect();
        const aside = document.querySelector('.login-aside').getBoundingClientRect();
        const submit = document.querySelector('#login-cred-actions .login-submit').getBoundingClientRect();
        const input = document.getElementById('login-pass').getBoundingClientRect();
        const toggle = document.querySelector('.login-pass-toggle').getBoundingClientRect();
        return {
          authFirst: aside.top < pitch.top,
          submitVisible: submit.top >= 0 && submit.bottom <= innerHeight,
          overflow: overlay.scrollWidth > innerWidth,
          targets: [submit.height, input.height, toggle.height, toggle.width],
        };
      });
      assert.equal(mobile.authFirst, true, `${viewport.width}px must put authentication first`);
      assert.equal(mobile.submitVisible, true, `${viewport.width}px submit must fit the first viewport`);
      assert.equal(mobile.overflow, false, `${viewport.width}px must not overflow horizontally`);
      assert.ok(mobile.targets.every(size => size >= 40), `${viewport.width}px controls need 40px targets`);
    }
  });
});

test('login submit is single-flight and exposes a recoverable busy state', async () => {
  await withPage([], async (page, { port, requests }) => {
    await mountLoginMarkup(page);
    for (const moduleFile of LOGIN_MODS) {
      await page.addScriptTag({ url: `http://127.0.0.1:${port}/${moduleFile}` });
    }

    const state = await page.evaluate(async () => {
      document.getElementById('login-user').value = 'edge-admin';
      document.getElementById('login-pass').value = 'wrong-password';
      const first = window.doLoginPage();
      const second = window.doLoginPage();
      const button = document.querySelector('#login-cred-actions .login-submit');
      const label = button.querySelector('.login-submit-label');
      const during = {
        disabled: button.disabled,
        busy: button.getAttribute('aria-busy'),
        label: label.textContent,
      };
      await Promise.all([first, second]);
      return {
        during,
        after: {
          disabled: button.disabled,
          busy: button.getAttribute('aria-busy'),
          label: label.textContent,
          error: document.getElementById('login-err').textContent,
        },
      };
    });

    assert.deepEqual(state.during, { disabled: true, busy: 'true', label: '로그인 중…' });
    assert.equal(requests.filter(request => request.path === '/api/v1/auth/token').length, 1);
    assert.equal(state.after.disabled, false);
    assert.equal(state.after.busy, 'false');
    assert.equal(state.after.label, '로그인');
    assert.ok(state.after.error.includes('인증 실패'));
  }, {
    routes: {
      '/api/v1/auth/token': async () => {
        await new Promise(resolve => setTimeout(resolve, 120));
        return { status: 401, body: { error: { message: 'raw server text must stay hidden' } } };
      },
    },
  });
});
