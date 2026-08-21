                                                                                           
                                                                         
                                                                       
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const MODS = ['ui/modules/ui.js', 'ui/modules/uxlib.js', 'ui/modules/filter-state.js'];
const FACETS = [{ key: 'status', label: '상태', options: [
  { value: 'running', label: '실행', count: 9, sw: 'ok' },
  { value: 'error', label: '오류', count: 1, sw: 'crit' } ] }];
const SINGLE = [{
  key: 'severity',
  label: '알림 심각도 필터',
  showAll: true,
  allLabel: '전체',
  singleSelect: true,
  options: [
    { value: 'crit', label: '심각', count: 2, sw: 'crit' },
    { value: 'warn', label: '경고', count: 3, sw: 'warn' },
  ],
}];

test('filterBar renders chips with counts', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate((facets) => {
      const bar = window.HN.filterBar(facets);
      const chips = bar.querySelectorAll('.chip');
      return { count: chips.length, first: chips[0].textContent, cls: bar.className };
    }, FACETS);
    assert.ok(r.cls.includes('filterbar'));
    assert.equal(r.count, 2);
    assert.ok(r.first.includes('실행'));
    assert.ok(r.first.includes('9'));
  });
});

test('filterBar chips are keyboard-accessible (role/tabindex/aria-pressed)', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate((facets) => {
      const bar = window.HN.filterBar(facets);
      const chip = bar.querySelector('.chip');
      return { role: chip.getAttribute('role'), tabindex: chip.getAttribute('tabindex'), pressed: chip.getAttribute('aria-pressed') };
    }, FACETS);
    assert.equal(r.role, 'button');                                                         
    assert.equal(r.tabindex, '0');             
    assert.equal(r.pressed, 'false');                
  });
});

test('clicking a chip applies filter and toggles on-class', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate((facets) => {
      const fs = window.PCV.ui.filterState;
      const bar = window.HN.filterBar(facets);
      document.getElementById('cb').appendChild(bar);
      const errorChip = Array.from(bar.querySelectorAll('.chip'))
        .find(c => c.textContent.includes('오류'));
      errorChip.click();
      return { search: location.search, on: errorChip.classList.contains('on'), current: fs.current() };
    }, FACETS);
    assert.equal(r.search, '?status=error');
    assert.equal(r.on, true);
    assert.deepEqual(r.current.status, ['error']);
  });
});

test('chip activates via keyboard (Enter through global [role=button] delegate)', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate((facets) => {
      const fs = window.PCV.ui.filterState;
      fs.apply({ status: [] });                       
      const bar = window.HN.filterBar(facets);
      document.getElementById('cb').appendChild(bar);
      const errorChip = Array.from(bar.querySelectorAll('.chip'))
        .find(c => c.textContent.includes('오류'));
      errorChip.focus();
                                                               
      errorChip.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }));
      return { on: errorChip.classList.contains('on'), current: fs.current().status };
    }, FACETS);
    assert.equal(r.on, true);                               
    assert.deepEqual(r.current, ['error']);               
  });
});

test('filterBar chips expose data-facet/data-val for focus restore', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate((facets) => {
      const bar = window.HN.filterBar(facets);
      const chip = bar.querySelector('.chip[data-val="error"]');
      return { facet: chip.getAttribute('data-facet'), val: chip.getAttribute('data-val') };
    }, FACETS);
    assert.equal(r.facet, 'status');
    assert.equal(r.val, 'error');
  });
});

test('single-select filterBar prepends All and exposes a labelled group', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate((facets) => {
      const bar = window.HN.filterBar(facets);
      const chips = Array.from(bar.querySelectorAll('.chip'));
      const multi = window.HN.filterBar([
        { key: 'status', label: '상태', options: [] },
        { key: 'type', options: [] },
      ]);
      return {
        rootCount: bar.matches('.filterbar') ? 1 : 0,
        role: bar.getAttribute('role'),
        label: bar.getAttribute('aria-label'),
        values: chips.map((chip) => chip.getAttribute('data-val')),
        labels: chips.map((chip) => chip.textContent.trim()),
        pressed: chips.map((chip) => chip.getAttribute('aria-pressed')),
        multiLabel: multi.getAttribute('aria-label'),
      };
    }, SINGLE);
    assert.equal(r.rootCount, 1);
    assert.equal(r.role, 'group');
    assert.equal(r.label, '알림 심각도 필터');
    assert.deepEqual(r.values, ['__all__', 'crit', 'warn']);
    assert.equal(r.labels[0], '전체');
    assert.deepEqual(r.pressed, ['true', 'false', 'false']);
    assert.equal(r.multiLabel, '상태, type');
  });
});

test('single-select chips replace selection and only All clears it', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate((facets) => {
      const fs = window.PCV.ui.filterState;
      const bar = window.HN.filterBar(facets);
      document.getElementById('cb').appendChild(bar);
      const chips = Array.from(bar.querySelectorAll('.chip'));
      function snapshot() {
        return {
          current: fs.current().severity || [],
          on: chips.map((chip) => chip.classList.contains('on')),
          pressed: chips.map((chip) => chip.getAttribute('aria-pressed')),
        };
      }

      const states = { initial: snapshot() };
      chips[1].click();
      states.crit = snapshot();
      chips[1].click();
      states.critAgain = snapshot();
      chips[2].click();
      states.warn = snapshot();
      chips[2].click();
      states.warnAgain = snapshot();
      chips[0].click();
      states.all = snapshot();
      return states;
    }, SINGLE);

    assert.deepEqual(r.initial, {
      current: [],
      on: [true, false, false],
      pressed: ['true', 'false', 'false'],
    });
    assert.deepEqual(r.crit, {
      current: ['crit'],
      on: [false, true, false],
      pressed: ['false', 'true', 'false'],
    });
    assert.deepEqual(r.critAgain, r.crit);
    assert.deepEqual(r.warn, {
      current: ['warn'],
      on: [false, false, true],
      pressed: ['false', 'false', 'true'],
    });
    assert.deepEqual(r.warnAgain, r.warn);
    assert.deepEqual(r.all, {
      current: [],
      on: [true, false, false],
      pressed: ['true', 'false', 'false'],
    });
  });
});

test('single-select render treats invalid URL selection as All without normalizing state', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate((facets) => {
      history.replaceState(null, '', '/?severity=crit,warn');
      const fs = window.PCV.ui.filterState;
      fs.readFromUrl();
      const before = { current: fs.current(), search: location.search };
      let applyCalls = 0;
      const apply = fs.apply;
      fs.apply = function (patch) {
        applyCalls++;
        return apply.call(fs, patch);
      };

      const bar = window.HN.filterBar(facets);
      const chips = Array.from(bar.querySelectorAll('.chip'));
      return {
        before,
        after: { current: fs.current(), search: location.search },
        applyCalls,
        pressed: chips.map((chip) => chip.getAttribute('aria-pressed')),
      };
    }, SINGLE);

    assert.deepEqual(r.before.current.severity, ['crit', 'warn']);
    assert.equal(r.before.search, '?severity=crit,warn');
    assert.deepEqual(r.after, r.before);
    assert.equal(r.applyCalls, 0);
    assert.deepEqual(r.pressed, ['true', 'false', 'false']);
  });
});

test('disabled filterBar chips ignore mouse and keyboard activation', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate((facets) => {
      history.replaceState(null, '', '/?severity=crit');
      const fs = window.PCV.ui.filterState;
      fs.readFromUrl();
      let applyCalls = 0;
      const apply = fs.apply;
      fs.apply = function (patch) {
        applyCalls++;
        return apply.call(fs, patch);
      };

      facets[0].disabled = true;
      const bar = window.HN.filterBar(facets);
      document.getElementById('cb').appendChild(bar);
      const chips = Array.from(bar.querySelectorAll('.chip'));
      chips[0].click();
      chips[2].click();
      chips[2].dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }));
      chips[2].dispatchEvent(new KeyboardEvent('keydown', { key: ' ', bubbles: true }));
      return {
        disabled: chips.map((chip) => chip.getAttribute('aria-disabled')),
        tabindex: chips.map((chip) => chip.getAttribute('tabindex')),
        applyCalls,
        current: fs.current().severity,
        search: location.search,
        pressed: chips.map((chip) => chip.getAttribute('aria-pressed')),
      };
    }, SINGLE);

    assert.deepEqual(r.disabled, ['true', 'true', 'true']);
    assert.deepEqual(r.tabindex, ['-1', '-1', '-1']);
    assert.equal(r.applyCalls, 0);
    assert.deepEqual(r.current, ['crit']);
    assert.equal(r.search, '?severity=crit');
    assert.deepEqual(r.pressed, ['false', 'true', 'false']);
  });
});
