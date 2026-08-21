                                                                                                 
                                                                   
                                                                     
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage, CORE } from './harness.mjs';

test('gaugeLevel thresholds', async () => {
  await withPage(CORE, async (page) => {
    const r = await page.evaluate(() => ({
      ok: window.HN.gaugeLevel(34, 80, 95),
      warnBoundary: window.HN.gaugeLevel(80, 80, 95),
      critBoundary: window.HN.gaugeLevel(95, 80, 95),
      over: window.HN.gaugeLevel(120, 80, 95),
    }));
    assert.equal(r.ok, 'ok');
    assert.equal(r.warnBoundary, 'warn');
    assert.equal(r.critBoundary, 'crit');
    assert.equal(r.over, 'crit');
  });
});

test('gauge DOM structure', async () => {
  await withPage(CORE, async (page) => {
    const r = await page.evaluate(() => {
      const g = window.HN.gauge({ value: 72, warn: 70, crit: 90, limit: '46 / 64 GiB', unit: '%', label: '메모리' });
      const fill = g.querySelector('.fill');
      const ticks = g.querySelectorAll('.tick');
      return {
        rootClass: g.className,
        fillWidth: fill.style.width,
        level: g.classList.contains('g-warn'),
        tickCount: ticks.length,
        text: g.textContent,
      };
    });
    assert.ok(r.rootClass.includes('gauge'));
    assert.equal(r.fillWidth, '72%');                            
    assert.ok(r.level, 'value 72 >= warn 70 → g-warn');
    assert.equal(r.tickCount, 2);                          
    assert.ok(r.text.includes('메모리'));
    assert.ok(r.text.includes('46 / 64 GiB'));
  });
});

test('gauge clamps tick position to [0,100] when warn/crit exceed range', async () => {
  await withPage(['ui/modules/ui.js', 'ui/modules/uxlib.js'], async (page) => {
    const r = await page.evaluate(() => {
      const g = window.HN.gauge({ value: 50, warn: 120, crit: 150, unit: '%', label: 'X' });
      const ticks = g.querySelectorAll('.tick');
      return { warn: ticks[0].style.left, crit: ticks[1].style.left };
    });
    assert.equal(r.warn, '100%');                   
    assert.equal(r.crit, '100%');                   
  });
});

test('gauge inline variant renders track-only with level-colored fill', async () => {
  await withPage(CORE, async (page) => {
    const r = await page.evaluate(() => {
      const ok = window.HN.gauge({ value: 40, warn: 70, crit: 90, inline: true });
      const crit = window.HN.gauge({ value: 95, warn: 70, crit: 90, inline: true });
      return {
        cls: ok.className,
        hasTop: !!ok.querySelector('.gauge-top'),                        
        hasTick: !!ok.querySelector('.tick'),                             
        fill: ok.querySelector('.fill').style.width,
        critLevel: crit.className,
      };
    });
    assert.ok(r.cls.includes('gauge-inline'));
    assert.ok(r.cls.includes('g-ok'));
    assert.equal(r.hasTop, false);
    assert.equal(r.hasTick, false);
    assert.equal(r.fill, '40%');
    assert.ok(r.critLevel.includes('g-crit'));
  });
});
