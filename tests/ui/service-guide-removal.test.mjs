                                                                           
                                                        
                                                    
                                             
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { withPage, CORE } from './harness.mjs';

const readUi = path => readFileSync(new URL(`../../${path}`, import.meta.url), 'utf8');

test('service guide is absent from canonical navigation and runtime content', () => {
  const canonicalSources = [
    'ui/modules/help.js',
    'ui/modules/nav.js',
    'ui/modules/shell.js',
    'ui/app.js',
    'ui/i18n.js',
  ];
  for (const path of canonicalSources) {
    const source = readUi(path);
    assert.equal(source.includes('serviceguide'), false, `${path} retains serviceguide`);
    assert.equal(source.includes('renderServiceGuide'), false, `${path} retains its renderer`);
    assert.equal(source.includes('filterGuide'), false, `${path} retains its filter`);
  }
  const help = readUi('ui/modules/help.js');
  assert.equal(help.includes('PureCVisor 서비스 가이드'), false);
  assert.equal(help.includes('SERVICE GUIDE'), false);
});

test('retired service guide bookmark normalizes to help without preserving a stale resource id', async () => {
  await withPage(CORE, async page => {
    const result = await page.evaluate(() => {
      history.replaceState(null, '', `${location.pathname}?source=bookmark#/serviceguide/old-section`);
      const calls = [];
      window.openResourceById = (...args) => calls.push(['resource', ...args]);
      window.navigateTo = (pageName, hooks) => {
        calls.push(['navigate', pageName]);
        hooks.after();
        return true;
      };
      window.navigateToHash();
      return {
        calls,
        hash: location.hash,
        search: location.search,
        parsed: window.parseHashRoute(),
      };
    });

    assert.deepEqual(result.calls, [['navigate', 'helppage']]);
    assert.equal(result.hash, '#/helppage');
    assert.equal(result.search, '?source=bookmark');
    assert.deepEqual(result.parsed, { page: 'helppage', id: null });
  });
});
