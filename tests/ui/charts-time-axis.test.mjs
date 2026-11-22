import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage, CORE } from './harness.mjs';

const MODS = [...CORE, 'ui/modules/charts.js'];

function stubChart() {
  window.__charts = [];
  window.Chart = class {
    constructor(el, cfg) { this.canvas = el; this.data = cfg.data; this.options = cfg.options; window.__charts.push(this); }
    update() { this.updated = (this.updated || 0) + 1; }
    destroy() {}
  };
  const c = document.createElement('canvas'); c.id = 'cv'; document.body.appendChild(c);
}

function hms(t) {
  const d = new Date(t), p = n => String(n).padStart(2, '0');
  return p(d.getHours()) + ':' + p(d.getMinutes()) + ':' + p(d.getSeconds());
}

test('불규칙 10,006ms 간격 표본 라벨은 실제 수집 시각이고 -5s·now를 지어내지 않는다', async () => {
  await withPage(MODS, async page => {
    const T0 = 1794757394000;
    const ts = [T0, T0 + 10006, T0 + 10006 + 4321];
    const out = await page.evaluate((stub, ts) => {
      new Function(stub)();
      pcvTimeSeries('cv', [{ label: 'a', data: [1, 2, 3] }], { timestamps: ts });
      return window.__charts[0].data.labels;
    }, `(${stubChart})()`, ts);
    assert.deepEqual(out, ts.map(hms));
    assert.ok(!out.some(l => /^-\d+s$|^now$/.test(l)), out.join(','));
  });
});

test('짧은 series는 앞을 null로 채워 최신 표본끼리 같은 칸에 맞춘다', async () => {
  await withPage(MODS, async page => {
    const out = await page.evaluate(stub => {
      new Function(stub)();
      pcvTimeSeries('cv', [{ label: 'old', data: [1, 2, 3, 4] }, { label: 'new', data: [9, 8] }],
        { timestamps: [1000, 2000, 3000, 4000] });
      return window.__charts[0].data.datasets.map(d => d.data);
    }, `(${stubChart})()`);
    assert.deepEqual(out, [[1, 2, 3, 4], [null, null, 9, 8]]);
  });
});

test('시각이 없거나 모자란 칸은 빈 라벨이고, 갱신 경로도 같은 라벨을 쓴다', async () => {
  await withPage(MODS, async page => {
    const out = await page.evaluate(stub => {
      new Function(stub)();
      pcvTimeSeries('cv', [{ label: 'a', data: [1, 2] }], {});
      const first = window.__charts[0].data.labels.slice();
      pcvTimeSeries('cv', [{ label: 'a', data: [1, 2, 3] }], { timestamps: [5000, 6000] });
      const ch = window.__charts[0];
      return { first, n: window.__charts.length, updated: ch.updated, labels: ch.data.labels };
    }, `(${stubChart})()`);
    assert.deepEqual(out.first, ['', '']);
    assert.equal(out.n, 1);
    assert.equal(out.updated, 1);
    assert.deepEqual(out.labels, ['', hms(5000), hms(6000)]);
  });
});

test('수집이 빠졌던 노드의 옛 표본은 시각으로 맞춰져 최근 칸에 그려지지 않는다', async () => {
  await withPage(MODS, async page => {
    const out = await page.evaluate(() => {
      const axis = [0, 10000, 20000, 1800000, 1810000];
      const bTs = [0, 10000, 1810000];
      return {
        b: pcvAlignToAxis(axis, bTs, [1, 2, 3], 2000),
        jitter: pcvAlignToAxis([1000, 11000], [1400, 10200], [5, 6], 2000),
        shortData: pcvAlignToAxis([0, 10000, 20000], [0, 10000, 20000], [null, 7], 2000),
        axis: pcvNodeAxis([[0, 10000, 1800000], [400, 1800300, 1810000], null], 3)
      };
    });
    assert.deepEqual(out.b, [1, 2, null, null, 3]);
    assert.deepEqual(out.jitter, [5, 6]);
    assert.deepEqual(out.shortData, [null, null, 7]);
    assert.deepEqual(out.axis, [10000, 1800000, 1810000]);
  });
});
