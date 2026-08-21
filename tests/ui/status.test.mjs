                                                                                     
                                                        
                                                              
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage, CORE } from './harness.mjs';

test('statusRank/statusReduce/statusPill/statusDot', async () => {
  await withPage(CORE, async (page) => {
    const r = await page.evaluate(() => {
      const HN = window.HN;
      const pill = HN.statusPill('crit', 'ERROR');
      const dot = HN.statusDot('ok');
      return {
        rankCrit: HN.statusRank('crit'),
        rankUnknown: HN.statusRank('nope'),
        reduceWarn: HN.statusReduce(['ok', 'warn', 'ok']),
        reduceCrit: HN.statusReduce(['ok', 'crit', 'warn']),
        reduceEmpty: HN.statusReduce([]),
        pillTag: pill.tagName,
        pillClass: pill.className,
        pillText: pill.textContent,
        dotClass: dot.className,
      };
    });
    assert.equal(r.rankCrit, 3);
    assert.equal(r.rankUnknown, 0);
    assert.equal(r.reduceWarn, 'warn');
    assert.equal(r.reduceCrit, 'crit');
    assert.equal(r.reduceEmpty, 'idle');
    assert.equal(r.pillTag, 'SPAN');
    assert.equal(r.pillClass, 'pill pill-crit');
    assert.equal(r.pillText, 'ERROR');
    assert.equal(r.dotClass, 'sdot sdot-ok');
  });
});

test('statusDot glow renders in status color (currentColor)', async () => {
  const r = await withPage(CORE, async (page) => {
    return await page.evaluate(() => {
      const dot = window.HN.statusDot('crit', { glow: true });
      document.getElementById('cb').appendChild(dot);
      const cs = getComputedStyle(dot);
      return { cls: dot.className, color: cs.color };
    });
  });
  assert.equal(r.cls, 'sdot sdot-crit sdot-glow');
  assert.equal(r.color, 'rgb(244, 63, 94)');                                
});
