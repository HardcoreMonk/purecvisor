




import { test } from 'node:test';
import assert from 'node:assert/strict';
import axe from 'axe-core';
import { withPage } from './harness.mjs';

const MODULES = ['ui/i18n.js', 'ui/modules/endpoints.js', 'ui/modules/api.js',
  'ui/modules/ui.js', 'ui/modules/uxlib.js', 'ui/modules/monitor.js'];
const THEMES = ['supanova', 'supanova-cyan', 'supanova-hicontrast', 'supanova-mockup'];
const VMS = [
  { name: 'alpha', state: 'running', ip: '192.0.2.11', cpu: 2, memory_percent: 40 },
  { name: 'beta', state: 'stopped', ip: '192.0.2.22', cpu: 0, memory_percent: 0 },
  { name: 'gamma', state: 'unknown', ip: '192.0.2.33' },
];

for (const theme of THEMES) {
  test(`OPS ${theme}: 작은 글자·상태·빈 결과와 키보드 접근을 유지한다`, async () => {
    await withPage(MODULES, async page => {
      await page.evaluate(axe.source);
      await page.evaluate(async theme => {
        document.documentElement.dataset.theme = theme;
        window._L = ko => ko;
        window.authToken = 'fixture';
        window.opsContrastNavigations = [];
        window.navigateTo = route => { window.opsContrastNavigations.push(route); };
        window.eventLog = [
          { title: '조회 완료', detail: '브라우저 활동 표식', time: '10:00', tone: 'ok' },
          { title: '조회 지연', detail: '주의 상태 표식', time: '10:01', tone: 'warn' },
        ];
        const nativeFetch = window.fetch.bind(window);
        window.fetch = (url, options) => String(url).endsWith('/api/v1/metrics')
          ? Promise.resolve(new Response('purecvisor_host_cpu_percent 85\n'
            + 'purecvisor_host_memory_percent 75\n'
            + 'purecvisor_host_memory_total_bytes 8589934592\n'))
          : nativeFetch(url, options);
        await renderOpsTriage(document.getElementById('cb'));
        await document.fonts.ready;
      }, theme);

      await page.waitForFunction(() => document.querySelectorAll('.ops-status').length === 7);
      const findings = [];
      for (const width of [1440, 768, 480]) {
        await page.setViewport({ width, height: 1000 });
        await page.evaluate(() => new Promise(resolve => setTimeout(resolve, 550)));
        const scan = async state => {
          const result = await page.evaluate(async () => {
            const result = await axe.run(document.querySelector('.ops-triage-grid'), {
              runOnly: { type: 'rule', values: ['color-contrast'] },
            });
            return {
              violations: result.violations.flatMap(rule => rule.nodes.map(node => ({
                target: node.target, summary: node.failureSummary,
              }))),
              measured: result.passes.filter(rule => rule.id === 'color-contrast')
                .reduce((count, rule) => count + rule.nodes.length, 0),
              incomplete: result.incomplete.flatMap(rule => rule.nodes.map(node => ({
                target: node.target, summary: node.failureSummary,
              }))),
            };
          });
          assert.ok(result.measured + result.violations.length > 10,
            `${theme}/${width}/${state}: 실제 글자 검사가 필요하다`);
          findings.push(...result.violations.map(item => ({ theme, width, state, ...item })));
          findings.push(...result.incomplete.map(item => ({ theme, width, state,
            incomplete: true, ...item })));
        };
        await scan('normal');
        await page.hover('.ops-triage-action.primary');
        await scan('primary-hover');
        await page.focus('.ops-triage-field');
        await page.keyboard.type('no-such-fixture');
        assert.match(await page.$eval('tbody', node => node.textContent), /검색 결과가 없습니다/);
        const focus = await page.$eval('.ops-triage-field', node => {
          const css = getComputedStyle(node);
          return { active: node === document.activeElement, visible: node.matches(':focus-visible'),
            outline: parseFloat(css.outlineWidth), outlineStyle: css.outlineStyle };
        });
        assert.ok(focus.active && focus.visible && focus.outline >= 2 && focus.outlineStyle !== 'none',
          `${theme}/${width}: 검색 입력의 키보드 초점이 보여야 한다`);
        await scan('empty-search-focus');
        await page.keyboard.down('Control');
        await page.keyboard.press('KeyA');
        await page.keyboard.up('Control');
        await page.keyboard.press('Backspace');
        assert.equal(await page.$$eval('tbody tr', nodes => nodes.length), 3);
      }
      assert.deepEqual(findings, [], 'OPS의 합성된 텍스트 대비는 AA 기준을 충족해야 한다');
      await page.focus('.ops-triage-action');
      await page.keyboard.press('Enter');
      assert.deepEqual(await page.evaluate(() => window.opsContrastNavigations), ['mon-alerts']);
    }, { routes: { '/api/v1/vms': { body: { data: VMS } } } });
  });
}
