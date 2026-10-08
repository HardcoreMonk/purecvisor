


import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdirSync, readFileSync } from 'node:fs';
import path from 'node:path';
import { withPage, CORE } from './harness.mjs';

test('retired Cloud bookmark redirects to help without dispatching its resource or API', async () => {
  await withPage([...CORE, 'ui/modules/endpoints.js'], async page => {
    const requests = [];
    page.on('request', request => {
      if (new URL(request.url()).pathname.startsWith('/api/')) requests.push(request.url());
    });
    const result = await page.evaluate(() => {
      history.replaceState(null, '', `${location.pathname}?source=bookmark#/cloud-migration/old-vm`);
      const calls = [];
      window.openResourceById = (...args) => calls.push(['resource', ...args]);
      window.navigateTo = (name, hooks) => {
        calls.push(['navigate', name]); hooks.after(); return true;
      };
      window.navigateToHash();
      return { calls, hash: location.hash, search: location.search,
        route: window.parseHashRoute(), cloudEndpoints: Object.keys(EP).filter(key => key.startsWith('CLOUD_')) };
    });
    assert.deepEqual(result.calls, [['navigate', 'helppage']]);
    assert.equal(result.hash, '#/helppage');
    assert.equal(result.search, '?source=bookmark');
    assert.deepEqual(result.route, { page: 'helppage', id: null });
    assert.deepEqual(result.cloudEndpoints, []);
    assert.deepEqual(requests, []);
  });
});



const INDEX_SOURCE = readFileSync(new URL('../../ui/index.html', import.meta.url), 'utf8');
for (const width of [1280, 1024, 768, 480]) {
  test(`retired AWS entries are absent and Help preserves OVA/local backup at ${width}px`, async () => {
    await withPage(['ui/i18n.js', ...CORE, 'ui/modules/help.js', 'ui/modules/shell.js'], async page => {
      await page.setViewport({ width, height: 900 });
      const requests = [];
      const errors = [];
      page.on('request', request => {
        if (new URL(request.url()).pathname.startsWith('/api/')) requests.push(request.url());
      });
      page.on('pageerror', error => errors.push(String(error)));
      const state = await page.evaluate(async source => {
        document.documentElement.setAttribute('data-theme', 'supanova');
        const base = document.createElement('base');
        base.href = '/ui/';
        document.head.appendChild(base);
        const parsed = new DOMParser().parseFromString(source, 'text/html');
        const app = document.importNode(parsed.getElementById('app'), true);
        document.getElementById('cb').remove();
        document.body.appendChild(app);
        app.removeAttribute('inert');
        app.removeAttribute('aria-hidden');
        const font = document.createElement('link');
        font.rel = 'stylesheet';
        font.href = '/ui/vendor/pretendard/pretendard.css';
        document.head.appendChild(font);
        window.currentTab = 'helppage';
        PCV.shell.mount();
        renderHelp(document.getElementById('cb'));
        await document.fonts.ready;
        await Promise.all(document.getAnimations()
          .filter(animation => animation instanceof CSSTransition)
          .map(animation => animation.finished.catch(() => {})));
        const content = document.getElementById('cb');
        const help = content.textContent;
        const ovaRows = [...content.querySelectorAll('tr')].filter(row =>
          /vm\.(?:import|export)\.ova/.test(row.textContent));
        return {
          nav: [...document.querySelectorAll('[data-nav]')].map(node => node.dataset.nav),
          help,
          ovaRows: ovaRows.map(row => row.cells[3].textContent),
          active: document.querySelector('[aria-current="page"]')?.dataset.nav,
          contentOverflow: content.scrollWidth > content.clientWidth + 1,
          pageOverflow: document.documentElement.scrollWidth > innerWidth + 1
        };
      }, INDEX_SOURCE);
      assert.ok(state.nav.includes('vm') && state.nav.includes('backup'));
      assert.equal(state.nav.includes('cloud-migration'), false);
      assert.equal(state.active, 'helppage');
      assert.doesNotMatch(state.help, /vm\.(import|export)\.ec2|cloud\.jobs|Cloud Migration/);
      assert.doesNotMatch(state.help, /backup\.export_s3|export-s3|S3/);
      assert.match(state.help, /backup\.restore/);
      assert.match(state.help, /backup\.incremental/);
      assert.equal(state.ovaRows.length, 2);
      assert.deepEqual(state.ovaRows, ['vmlist', 'vmlist']);
      assert.equal(state.contentOverflow, false);
      assert.equal(state.pageOverflow, false);
      assert.deepEqual(requests, []);
      assert.deepEqual(errors, []);
      const evidence = process.env.PCV_AWS_REMOVAL_UI_EVIDENCE_DIR;
      if (evidence) {
        mkdirSync(evidence, { recursive: true });
        await page.screenshot({ path: path.join(evidence, `help-${width}.png`) });
      }
    });
  });
}

test('retired Cloud bookmark respects an unsaved-screen navigation veto', async () => {
  await withPage(CORE, async page => {
    const result = await page.evaluate(() => {
      window.setHashRoute('summary', 'kept-vm');
      history.replaceState(null, '', `${location.pathname}#/cloud-migration/old-vm`);
      const calls = [];
      window.navigateTo = name => { calls.push(name); return false; };
      window.navigateToHash();
      return { calls, hash: location.hash };
    });
    assert.deepEqual(result.calls, ['helppage']);
    assert.equal(result.hash, '#/summary/kept-vm');
  });
});
