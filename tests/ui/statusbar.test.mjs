                                                                                    
                                                                                  
                                                                    
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const MODS = ['ui/modules/ui.js', 'ui/modules/uxlib.js', 'ui/modules/filter-state.js'];
const SEGS = [
  { key: 'vm', count: 5, total: 6, label: 'VM 실행', status: 'ok', sub: '/ 6 · 1 중지', filter: { status: ['running'], type: ['vm'] } },
  { key: 'alert', count: 2, label: 'Critical 알림', status: 'crit', sub: '3 unack', filter: { severity: ['crit'] } },
];

test('statusBar renders segments with status stripe', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate((segs) => {
      const bar = window.HN.statusBar(segs);
      const segEls = bar.querySelectorAll('.seg');
      return {
        cls: bar.className, n: segEls.length,
        firstCls: segEls[0].className, firstText: segEls[0].textContent,
      };
    }, SEGS);
    assert.ok(r.cls.includes('statusbar'));
    assert.equal(r.n, 2);
    assert.ok(r.firstCls.includes('seg-ok'));
    assert.ok(r.firstText.includes('5'));
    assert.ok(r.firstText.includes('VM 실행'));
  });
});

test('clicking a segment applies its filter and calls onNavigate', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate((segs) => {
      const fs = window.PCV.ui.filterState;
      let navKey = null;
      const bar = window.HN.statusBar(segs, { onNavigate: (s) => { navKey = s.key; } });
      document.getElementById('cb').appendChild(bar);
      bar.querySelector('.seg').click();
      return { search: location.search, navKey, current: fs.current() };
    }, SEGS);
    assert.equal(r.navKey, 'vm');
    assert.ok(r.search.includes('status=running'));
    assert.ok(r.search.includes('type=vm'));
    assert.deepEqual(r.current.type, ['vm']);
  });
});
