import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const MODS = ['endpoints', 'api', 'ui', 'filter-state', 'uxlib', 'modal-core', 'vm', 'vm-console', 'vm-config-history']
  .map(x => 'ui/modules/' + x + '.js');
const NOW = 1795110000000;
const ID = c => c.repeat(32);
function entry(id, over) {
  return Object.assign({ id, vm_uuid: 'u1', vm_name: 'web', observed_at: NOW - 60000, window_start: null, kind: 'changed',
    cause: 'rpc', scope: 'config', attribution: 'rpc', actor: 'alice', actor_hidden: false, actor_expired: false,
    actor_method: 'vm.set_vcpu', actor_result: 'ok', change_count: 1, changes: [{ path: 'vcpu.max', before: '2', after: '4' }] }, over);
}
const LIST = {
  entries: [
    entry(ID('a'), { changes: [{ path: 'vcpu.max', before: '<b>x</b>', after: '4' }] }),
    entry(ID('b'), { observed_at: NOW - 3 * 3600000, window_start: NOW - 4 * 3600000, cause: 'reconcile', scope: 'live',
      attribution: 'none', actor: null, actor_method: null, actor_result: null }),
    entry(ID('c'), { observed_at: NOW - 5 * 3600000, kind: 'resized', actor: null, actor_hidden: true, actor_method: 'vm.resize_disk',
      changes: [{ path: 'disk[vda].requested_size_bytes', before: null, after: 21474836480 }] }),
    entry(ID('d'), { observed_at: NOW - 40 * 86400000, actor: null, actor_expired: true, actor_method: 'vm.limit', actor_result: 'fail' })
  ],
  gaps: [{ start: NOW - 2 * 3600000, end: NOW - 3600000 * 1.5, reason: 'shutdown' }],
  store: { state: 'ok', oldest_at: NOW - 40 * 86400000, pruned_before: null, retention_days: 365 },
  scope: 'all', next_cursor: 'Y3Vyc29y'
};
const GET = { id: ID('a'), kind: 'changed', cause: 'rpc', prev_id: null,
  changes: [{ path: 'vcpu.max', before: '<b>x</b>', after: '4' }, { path: 'opaque.qemu_commandline', before: null, after: 'abcd' },
            { path: 'disk[vdb].requested_size_bytes', before: 1, after: 2 }] };

async function boot(page, list, opts) {
  await page.evaluate((list, get, now, opts) => {
    Object.assign(window, {
      _L: (ko, en) => ko, t: k => k, authToken: 'fixture', _DEBUG: false, currentUser: { role: 'ADMIN' }, currentTab: 'timeline',
      selectedVmIndex: 0, checkedVms: new Set(), vmList: [{ name: 'web', uuid: 'u1', state: 'running' }],
      eventLog: [{ time: '09:01:02', msg: 'VM web started' }],
      loadAll: () => {}, render: () => {}, renderContent: () => {}, addEvt: () => {}, toast: () => {}, __urls: [], __timers: []
    });
    Date.now = () => now;
    window.setInterval = (fn, ms) => { window.__timers.push({ fn, ms, kind: 'interval' }); return 1; };
    window.setTimeout = (fn, ms) => { window.__timers.push({ fn, ms, kind: 'timeout' }); return 1; };
    window.fetch = async (url) => {
      const u = String(url);
      window.__urls.push(u);
      const body = opts && opts.error ? { error: { code: -32603, message: 'boom' } }
        : /\/vm-config-history\/[0-9a-f]{32}$/.test(u) ? { data: get }
        : /\/vm-config-history/.test(u) ? { data: u.includes('cursor=') ? Object.assign({}, list, { entries: [], next_cursor: null }) : list }
        : { data: [] };
      return new Response(JSON.stringify(body), { status: 200, headers: { 'content-type': 'application/json' } });
    };
  }, list, GET, NOW, opts || null);
  await page.evaluate(async () => {
    const cb = document.getElementById('cb') || document.body.appendChild(Object.assign(document.createElement('div'), { id: 'cb' }));
    await window.renderTimeline(cb, window.vmList[0]);
  });
}
const histUrls = page => page.evaluate(() => window.__urls.filter(u => u.includes('/vm-config-history')));
const text = page => page.evaluate(() => document.querySelector('.vmch-card').textContent);

test('one list call on tab entry and no polling', async () => {
  await withPage(MODS, async page => {
    await boot(page, LIST);
    let urls = await histUrls(page);
    assert.equal(urls.length, 1, urls.join(' '));
    assert.match(urls[0], /\/vm-config-history\?vm=web&since=\d+&until=\d+&limit=50$/);
    assert.equal(await page.evaluate(() => window.__timers.filter(t => t.kind === 'interval').length), 0, '주기 타이머 없음');
    await page.evaluate(async () => { for (const t of window.__timers.splice(0)) t.fn(); await new Promise(r => queueMicrotask(r)); });
    urls = await histUrls(page);
    assert.equal(urls.length, 1, '10분 진행(등록 콜백 전부 실행) 뒤에도 추가 호출 0');
  });
});

test('row texts actor states badges and gap order', async () => {
  await withPage(MODS, async page => {
    await boot(page, LIST);
    const t = await text(page);
    for (const s of ['구성 변경(서버 기록)', 'alice · vm.set_vcpu', '요청 귀속 없음', '다른 주체 · vm.resize_disk',
      '행위자 기록 만료(30일) · vm.limit · 실패한 요청 중 바뀜', 'live — 재시작 때 사라짐', '디스크 크기 요청', '요청 크기 (vda)',
      '사이', '대조 스캔', '기록 공백 ', '데몬 중지', '보관 365일'])
      assert.ok(t.includes(s), '문구: ' + s + '\n' + t);
    const order = await page.evaluate(() => [...document.querySelectorAll('.vmch-list > li')].map(li => li.className));
    assert.deepEqual(order, ['vmch-item', 'vmch-gap', 'vmch-item', 'vmch-item', 'vmch-item'], '공백 띠는 시각 순서 자리');
    const hint = await page.evaluate(() => [...document.querySelectorAll('#cb h4')].map(h => h.textContent));
    assert.ok(hint.includes('이 브라우저의 최근 이벤트(힌트)'), hint.join('|'));
  });
});

test('expand calls get once and renders text only', async () => {
  await withPage(MODS, async page => {
    await boot(page, LIST);
    const btn = '.vmch-list > li:first-child > button';
    await page.click(btn);
    await new Promise(r => setTimeout(r, 150));
    assert.equal(await page.$eval(btn, b => b.getAttribute('aria-expanded')), 'true');
    let urls = await histUrls(page);
    assert.equal(urls.filter(u => /\/vm-config-history\/a{32}$/.test(u)).length, 1);
    const d = await page.evaluate(() => {
      const box = document.querySelector('.vmch-detail-box');
      return { text: box.textContent, bold: document.querySelectorAll('.vmch-card b').length };
    });
    assert.ok(d.text.includes('<b>x</b>'), '값은 글자 그대로');
    assert.equal(d.bold, 0, 'HTML 해석 0');
    assert.ok(d.text.includes('바뀜(내용 비공개)'));
    assert.ok(d.text.includes('요청 크기 (vdb)'));
    assert.ok(d.text.includes('직전 기록 없음'));
    await page.click(btn);
    await page.click(btn);
    await new Promise(r => setTimeout(r, 100));
    urls = await histUrls(page);
    assert.equal(urls.filter(u => /\/vm-config-history\/a{32}$/.test(u)).length, 1, '두 번째 펼침은 캐시');
  });
});

test('refresh and load more', async () => {
  await withPage(MODS, async page => {
    await boot(page, LIST);
    const btns = await page.$$('.vmch-card button.btn');
    const labels = await Promise.all(btns.map(b => b.evaluate(n => n.textContent + (n.hidden ? '(hidden)' : ''))));
    assert.deepEqual(labels, ['새로고침', '더 보기']);
    await btns[1].click();
    await new Promise(r => setTimeout(r, 150));
    let urls = await histUrls(page);
    assert.ok(urls[urls.length - 1].includes('cursor=Y3Vyc29y'), urls.join(' '));
    assert.equal(await page.$eval('.vmch-card button.btn:nth-of-type(1)', b => b.textContent), '새로고침');
    await btns[0].click();
    await new Promise(r => setTimeout(r, 150));
    urls = await histUrls(page);
    assert.equal(urls.filter(u => !u.includes('cursor=') && !/[0-9a-f]{32}$/.test(u)).length, 2, '새로고침 1회 = list 1회');
  });
});

test('keyboard: Tab order refresh then rows, Enter expands with aria', async () => {
  await withPage(MODS, async page => {
    await boot(page, LIST);
    await page.focus('.vmch-card button.btn');
    await page.keyboard.press('Tab');
    assert.equal(await page.evaluate(() => document.activeElement.className), 'vmch-row');
    await page.keyboard.press('Enter');
    await new Promise(r => setTimeout(r, 150));
    const st = await page.evaluate(() => {
      const b = document.activeElement;
      const region = document.getElementById(b.getAttribute('aria-controls'));
      return { exp: b.getAttribute('aria-expanded'), hidden: region.hidden, role: region.getAttribute('role'),
        label: region.getAttribute('aria-label') };
    });
    assert.deepEqual(st, { exp: 'true', hidden: false, role: 'region', label: '변경 상세' });
  });
});

test('empty disabled unavailable and error states', async () => {
  const cases = [
    [Object.assign({}, LIST, { entries: [], gaps: [], next_cursor: null }), '최근 7일'],
    [Object.assign({}, LIST, { entries: [], gaps: [], next_cursor: null, store: { state: 'ok', oldest_at: null, retention_days: 365 } }), '아직 기록이 없습니다'],
    [Object.assign({}, LIST, { entries: [], gaps: [], next_cursor: null, store: { state: 'disabled', retention_days: 365 } }), '꺼져 있습니다'],
    [Object.assign({}, LIST, { entries: [], gaps: [], next_cursor: null, store: { state: 'unavailable', retention_days: 365 } }), '[vm-cfg-hist]']
  ];
  for (const [list, needle] of cases) {
    await withPage(MODS, async page => {
      await boot(page, list);
      assert.ok((await text(page)).includes(needle), needle);
    });
  }
  await withPage(MODS, async page => {
    await boot(page, LIST, { error: true });
    assert.ok((await text(page)).includes('boom'));
    assert.ok((await page.evaluate(() => document.querySelector('#cb').textContent)).includes('VM web started'), '힌트 목록은 그대로');
  });
});
