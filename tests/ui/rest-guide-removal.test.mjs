                                                                          
                                                              
                                                    
                                                  
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { withPage, CORE } from './harness.mjs';

const readUi = path => readFileSync(new URL(`../../${path}`, import.meta.url), 'utf8');

test('retired REST guide is absent from canonical product surfaces', () => {
  for (const path of [
    'ui/modules/help.js',
    'ui/modules/nav.js',
    'ui/modules/shell.js',
    'ui/app.js',
    'ui/i18n.js',
  ]) {
    const source = readUi(path);
    assert.equal(source.includes('restguide'), false, `${path} retains the retired route`);
    assert.equal(source.includes('renderRestGuide'), false, `${path} retains the retired renderer`);
  }

  const style = readUi('ui/style.css');
  assert.equal(style.includes('.rest-docs'), false, 'retired REST reader CSS remains');

  const accounts = readUi('ui/modules/accounts.js');
  assert.equal(accounts.includes("navigateTo('restguide')"), false);
  assert.ok(accounts.includes("href: '/ui/docs.html#14-rest-api'"),
    'API management must point directly to the canonical REST chapter');

  const uxlib = readUi('ui/modules/uxlib.js');
  assert.ok(uxlib.includes("restguide: '/ui/docs.html#14-rest-api'"),
    'legacy bookmark compatibility must remain explicit');
});

test('retired REST guide bookmark replaces the URL with the canonical chapter', async () => {
  await withPage(CORE, async page => {
    await page.evaluate(() => {
      history.replaceState(null, '', `${location.pathname}?source=bookmark#/restguide/old-section`);
    });
    const navigation = page.waitForNavigation({ waitUntil: 'networkidle0' });
    await page.evaluate(() => window.navigateToHash());
    await navigation;
    await page.waitForFunction(() => location.pathname === '/ui/docs.html' &&
      decodeURIComponent(location.hash) === '#14-rest-api' &&
      document.getElementById('14-rest-api') && !document.getElementById('docs-reader').hidden);

    const state = await page.evaluate(() => {
      const start = document.getElementById('14-rest-api');
      const end = document.getElementById('15-cli-레퍼런스');
      let node = start.nextElementSibling;
      let codeBlocks = 0;
      let text = start.textContent;
      while (node && node !== end) {
        if (node.matches('.reader-code')) codeBlocks += 1;
        text += ` ${node.textContent}`;
        node = node.nextElementSibling;
      }
      return {
        pathname: location.pathname,
        search: location.search,
        hash: decodeURIComponent(location.hash),
        codeBlocks,
        hasPush: text.includes('PushSubscription.toJSON()'),
        hasRpc: text.includes('tenant_overlay.create') && text.includes('debug.trace.start'),
      };
    });

    assert.deepEqual(state, {
      pathname: '/ui/docs.html',
      search: '',
      hash: '#14-rest-api',
      codeBlocks: 25,
      hasPush: true,
      hasRpc: true,
    });
  });
});
