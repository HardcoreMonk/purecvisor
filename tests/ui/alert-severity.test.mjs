                                                                                          
                                                            
                                                               
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { CORE, withPage } from './harness.mjs';

test('alertSeverity normalizes the canonical field before compatibility fallbacks', async () => {
  await withPage(CORE, async (page) => {
    const actual = await page.evaluate(() => {
      const cases = [
        { severity: ' crit ' },
        { severity: 'CRITICAL' },
        { level: ' warning ' },
        { type: 'WARN' },
        { severity: 'warn', level: 'critical' },
        { severity: 'bogus', level: 'critical' },
        null,
        'critical',
      ];
      return cases.map(value => window.HN.alertSeverity(value));
    });

    assert.deepEqual(actual, [
      'crit',
      'crit',
      'warn',
      'warn',
      'warn',
      'idle',
      'idle',
      'idle',
    ]);
  });
});

test('alertSeverity rejects arrays even when they carry a severity property', async () => {
  await withPage(CORE, async (page) => {
    const actual = await page.evaluate(() => {
      const alert = [];
      alert.severity = 'critical';
      return window.HN.alertSeverity(alert);
    });

    assert.equal(actual, 'idle');
  });
});
