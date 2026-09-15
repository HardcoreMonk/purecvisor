






import { test } from 'node:test';
import assert from 'node:assert/strict';
import axe from 'axe-core';
import { withPage } from './harness.mjs';


async function mountIndex(page) {
  await page.evaluate(async () => {
    const parsed = new DOMParser().parseFromString(
      await fetch('/ui/index.html').then(response => response.text()),
      'text/html'
    );
    document.body.className = parsed.body.className;
    document.body.innerHTML = parsed.body.innerHTML;
    document.getElementById('splash')?.remove();
  });
}


test('로그인과 인증 앱은 상호 inert이며 shell topbar/main은 native landmark다', async () => {
  await withPage(['ui/modules/endpoints.js', 'ui/modules/api.js'], async page => {
    await mountIndex(page);
    const states = await page.evaluate(() => {
      const snapshot = () => {
        const login = document.getElementById('login-page');
        const app = document.getElementById('app');
        const mains = [...document.querySelectorAll('main')];
        const exposedMains = mains.filter(main =>
          !main.closest('[inert]') && !main.closest('[aria-hidden="true"]'));
        return {
          loginHidden: login.getAttribute('aria-hidden'),
          loginInert: login.hasAttribute('inert'),
          appHidden: app.getAttribute('aria-hidden'),
          appInert: app.hasAttribute('inert'),
          exposedMainCount: exposedMains.length,
        };
      };
      const initial = snapshot();
      window.pcvSetLoginVisible(false);
      const authenticated = snapshot();
      window.pcvSetLoginVisible(true);
      const returned = snapshot();
      return {
        initial,
        authenticated,
        returned,
        topbarTag: document.getElementById('shell-topbar-static').tagName,
        contentTag: document.querySelector('.shell-content').tagName,
        userInsideBanner: !!document.getElementById('us-name').closest('header'),
        crumbHostInsideBanner: !!document.getElementById('shell-topbar').closest('header'),
      };
    });

    assert.deepEqual(states.initial, {
      loginHidden: null, loginInert: false,
      appHidden: 'true', appInert: true, exposedMainCount: 1,
    });
    assert.deepEqual(states.authenticated, {
      loginHidden: 'true', loginInert: true,
      appHidden: null, appInert: false, exposedMainCount: 1,
    });
    assert.deepEqual(states.returned, states.initial);
    assert.equal(states.topbarTag, 'HEADER');
    assert.equal(states.contentTag, 'MAIN');
    assert.equal(states.userInsideBanner, true);
    assert.equal(states.crumbHostInsideBanner, true);
  });
});


test('모바일 화면 제목 wrapper는 main 안에 중첩 banner landmark를 만들지 않는다', async () => {
  await withPage([
    'ui/modules/endpoints.js', 'ui/modules/ui.js', 'ui/modules/uxlib.js',
    'ui/modules/mobile.js'
  ], async page => {
    const shape = await page.evaluate(() => {
      window._L = ko => ko;
      const node = window.PCV.mobile.buildHome();
      document.getElementById('cb').appendChild(node);
      const head = node.querySelector('.m-pagehead');
      return {
        wrapperTag: head.tagName,
        titleTag: head.querySelector('.m-page-title').tagName,
        titleCount: node.querySelectorAll('h1').length,
      };
    });
    assert.deepEqual(shape, { wrapperTag: 'DIV', titleTag: 'H1', titleCount: 1 });
  });
});


test('점검 페이지는 강제 meta refresh 없이 status JSON을 주기적으로 갱신한다', async () => {
  await withPage([], async page => {
    const contract = await page.evaluate(async () => {
      const html = await fetch('/ui/maintenance.html').then(response => response.text());
      const parsed = new DOMParser().parseFromString(html, 'text/html');
      return {
        metaRefresh: !!parsed.querySelector('meta[http-equiv="refresh" i]'),
        hasStatusLoader: /function\s+loadStatus\s*\(/.test(html),
        hasStatusPoll: /setInterval\s*\(\s*loadStatus\s*,\s*60000\s*\)/.test(html),
      };
    });
    assert.deepEqual(contract, { metaRefresh: false, hasStatusLoader: true, hasStatusPoll: true });
  });
});


test('REST method badge, danger sample, mobile quick action은 허용 테마에서 contrast 위반 0', async () => {
  await withPage([
    'ui/modules/ui.js', 'ui/modules/uxlib.js', 'ui/modules/help.js',
    'ui/modules/endpoints.js', 'ui/modules/mobile.js'
  ], async (page, { port }) => {
    await page.evaluate(axe.source);
    const productViolations = await page.evaluate(async () => {
      window._L = ko => ko;
      const cb = document.getElementById('cb');
      window.renderSwaggerApi(cb);
      cb.appendChild(window.PCV.mobile.buildHome());
      const themes = [null, 'supanova', 'supanova-cyan', 'supanova-hicontrast'];
      const methodCount = document.querySelectorAll('.sw-method-badge').length;
      const found = [];
      for (const theme of themes) {
        if (theme) document.documentElement.setAttribute('data-theme', theme);
        else document.documentElement.removeAttribute('data-theme');
        const result = await window.axe.run({
          include: [['.sw-method-badge'], ['.m-quick']],
        }, {
          runOnly: { type: 'rule', values: ['color-contrast'] },
        });
        found.push(...result.violations.flatMap(rule => rule.nodes.map(node => ({
          theme: theme || 'default', id: rule.id, target: node.target.join(' '),
        }))));
      }
      return { methodCount, found };
    });
    assert.ok(productViolations.methodCount > 0, 'REST method badge selector must stay wired');
    assert.deepEqual(productViolations.found, []);

    const transitionViolations = await page.evaluate(async () => {
      document.documentElement.setAttribute('data-theme', 'supanova');
      await new Promise(resolve => setTimeout(resolve, 550));
      document.documentElement.setAttribute('data-theme', 'supanova-cyan');
      await new Promise(resolve => setTimeout(resolve, 100));
      const result = await window.axe.run(document.querySelector('.m-quick'), {
        runOnly: { type: 'rule', values: ['color-contrast'] },
      });
      return result.violations.flatMap(rule => rule.nodes.map(node => node.target.join(' ')));
    });
    assert.deepEqual(transitionViolations, [], 'theme switch intermediate frames must stay readable');

    await page.goto(`http://127.0.0.1:${port}/ui/samples/design-system-preview.html`, {
      waitUntil: 'load',
    });
    await page.evaluate(axe.source);
    const sampleViolations = await page.evaluate(async () => {
      const result = await window.axe.run(document.querySelector('.btn.is-danger'), {
        runOnly: { type: 'rule', values: ['color-contrast'] },
      });
      return result.violations.flatMap(rule => rule.nodes.map(node => node.target.join(' ')));
    });
    assert.deepEqual(sampleViolations, []);
  });
});


test('recent-errors endpoint는 registry와 호출 시점 custom API_BASE를 사용한다', async () => {
  await withPage(['ui/modules/endpoints.js'], async page => {
    const result = await page.evaluate(async () => {
      window.API_BASE = '/tenant/edge/api/v1';
      const endpoint = window.EP.HEALTH_RECENT_ERRORS('vm /?#한글', 3);
      const fallbackLimit = window.EP.HEALTH_RECENT_ERRORS('vm-a', '3&admin=true');
      const source = await fetch('/ui/modules/vm-lifecycle.js').then(response => response.text());
      return {
        endpoint,
        fallbackLimit,
        usesRegistry: /fetchGet\(EP\.HEALTH_RECENT_ERRORS\(vmName,\s*3\)\)/.test(source),
        hasHardcodedFetch: /fetchGet\(['"]\/api\/v1\/health\/recent-errors/.test(source),
      };
    });
    assert.equal(
      result.endpoint,
      '/tenant/edge/api/v1/health/recent-errors?vm=vm%20%2F%3F%23%ED%95%9C%EA%B8%80&limit=3'
    );
    assert.equal(result.fallbackLimit, '/tenant/edge/api/v1/health/recent-errors?vm=vm-a&limit=3');
    assert.equal(result.usesRegistry, true);
    assert.equal(result.hasHardcodedFetch, false);
  });
});
