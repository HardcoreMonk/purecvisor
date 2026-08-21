                                                                                          
                                                                            
                                                                   
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const MODS = ['ui/modules/ui.js', 'ui/modules/uxlib.js', 'ui/modules/mobile.js'];

test('resize round-trip preserves the active tab (unmount→remount restores)', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate(() => {
      window.PCV.mobile.mount();
      window.PCV.mobile.showTab('power');
      window.PCV.mobile.unmount();                       
      window.PCV.mobile.mount();                   
      const on = document.querySelector('.mnav .mnav-item.on');
      return { active: window.PCV.mobile.activeTab, onTab: on ? on.getAttribute('data-tab') : null };
    });
    assert.equal(r.active, 'power');
    assert.equal(r.onTab, 'power');
  });
});

test('mobile shell never mutates desktop chrome nodes (golden: no #m-screen at desktop width)', async () => {
  await withPage(MODS, async (page) => {
    await page.setViewport({ width: 1200, height: 800 });
    const r = await page.evaluate(() => {
      window.PCV.mobile.sync();
      return {
        active: window.PCV.mobile.isActive(),
        shell: document.body.classList.contains('mshell'),
        screen: !!document.getElementById('m-screen'),
                                              
        cbUntouched: !!document.getElementById('cb'),
      };
    });
    assert.equal(r.active, false);
    assert.equal(r.shell, false);
    assert.equal(r.screen, false);
    assert.equal(r.cbUntouched, true);
  });
});

test('live-refresh wrapper is idempotent and passes through when shell inactive', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate(() => {
                                                                   
      let calls = 0;
      window.authToken = 't';
      window.loadAll = function () { calls++; return 'orig'; };
                                            
      window.PCV.mobile._wireLiveRefresh();
      window.PCV.mobile._wireLiveRefresh();                
      const ret = window.loadAll();                               
      return { calls, ret, wrapped: window._mLoadAllWrapped === true };
    });
    assert.equal(r.wrapped, true);
    assert.equal(r.calls, 1);                 
    assert.equal(r.ret, 'orig');              
  });
});
