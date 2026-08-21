                                                                                                     
                                                             
                                                          
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const MODS = ['ui/modules/ui.js', 'ui/modules/uxlib.js', 'ui/modules/mobile.js'];

const STATE = {
  mode: 'dry_run',
  pending: [
    { id: 7, policy: 'vm-reboot-loop', action: 'restart', reason: 'crash loop', ts: 1000 },
  ],
  history: [
    { timestamp: 900, action: 'restart', target: 'web', result: 'ok', reason: 'auto' },
  ],
};

test('buildHealing renders mode badge, pending card with approve/reject, and history', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate((st) => {
      const node = window.PCV.mobile.buildHealing(st);
      document.getElementById('cb').appendChild(node);
      const pend = node.querySelector('.m-heal-pending');
      return {
        root: node.className,
        mode: node.querySelector('.m-heal-mode') ? node.querySelector('.m-heal-mode').textContent : '',
        pendCards: node.querySelectorAll('.m-heal-pending').length,
        pendBtns: pend ? pend.querySelectorAll('button').length : 0,
        history: node.querySelectorAll('.m-heal-hist').length,
        text: node.textContent,
      };
    }, STATE);
    assert.ok(r.root.includes('mscreen-body'));
    assert.ok(r.mode.includes('DRY'));
    assert.equal(r.pendCards, 1);
    assert.equal(r.pendBtns, 2);                  
    assert.equal(r.history, 1);
    assert.ok(r.text.includes('vm-reboot-loop'));
  });
});

test('buildHealing with empty pending shows empty hint', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate(() => {
      const node = window.PCV.mobile.buildHealing({ mode: 'active', pending: [], history: [] });
      return { mode: node.querySelector('.m-heal-mode').textContent, empty: !!node.querySelector('.m-heal-empty') };
    });
    assert.ok(r.mode.includes('ACTIVE'));
    assert.equal(r.empty, true);
  });
});
