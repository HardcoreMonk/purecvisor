                                                                     
                                                                                          
                                                                       
                                                                 
                                                      
                                                           
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

test('version badge: hidden 속성이면 실제 style.css에서도 보이지 않는다', async () => {
  await withPage([], async page => {
    const shown = await page.evaluate(() => {
      const badge = document.createElement('span');
      badge.id = 'version-badge';
      badge.className = 'version-badge';
      badge.setAttribute('hidden', '');
      document.getElementById('cb').appendChild(badge);
      const hiddenDisplay = getComputedStyle(badge).display;
      const hiddenWidth = badge.getBoundingClientRect().width;
      badge.removeAttribute('hidden');
      badge.textContent = 'v2.2.0';
      return { hiddenDisplay, hiddenWidth, visibleDisplay: getComputedStyle(badge).display };
    });
    assert.equal(shown.hiddenDisplay, 'none');
    assert.equal(shown.hiddenWidth, 0);
    assert.equal(shown.visibleDisplay, 'inline-flex');
  });
});
