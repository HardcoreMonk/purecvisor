




import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const COMMON = ['ui/i18n.js', 'ui/modules/endpoints.js', 'ui/modules/api.js',
  'ui/modules/ui.js', 'ui/modules/uxlib.js'];

test('사이드바 방향키는 CSS로 숨긴 항목을 건너뛰고 가시 탭 스톱 하나를 유지한다', async () => {
  await withPage([...COMMON, 'ui/modules/shell.js'], async page => {
    await page.evaluate(() => {
      for (const id of ['shell-sidebar', 'shell-topbar', 'shell-statusbar']) {
        document.body.appendChild(Object.assign(document.createElement('div'), { id }));
      }
      PCV.shell.mount();
    });
    for (const hidden of [true, false]) {
      const expected = await page.evaluate(hidden => {
        const items = [...document.querySelectorAll('.shell-navitem')];
        const accounts = items.find(n => n.dataset.nav === 'accounts');
        const before = items[items.indexOf(accounts) - 1];
        accounts.classList.toggle('role-hidden', hidden);
        before.focus();
        return (hidden ? items[items.indexOf(accounts) + 1] : accounts).dataset.nav;
      }, hidden);
      await page.keyboard.press('ArrowDown');
      const result = await page.evaluate(() => ({
        active: document.activeElement.dataset.nav,
        stops: [...document.querySelectorAll('.shell-navitem[tabindex="0"]')]
          .map(n => ({ nav: n.dataset.nav, visible: n.getClientRects().length > 0 }))
      }));
      assert.equal(result.active, expected);
      assert.deepEqual(result.stops, [{ nav: expected, visible: true }]);
      await page.keyboard.press('ArrowUp');
      assert.notEqual(await page.evaluate(() => document.activeElement.dataset.nav), 'accounts');
    }
  });
});

test('검색 입력과 selection을 유지하고 정렬·페이지 이동에도 필터를 적용한다', async () => {
  await withPage(COMMON, async page => {
    await page.evaluate(() => {
      createDataTable('cb', { searchable: true, pageSize: 2,
        headers: [{ label: 'User' }, { label: 'Value' }],
        rows: [['bob', '1'], ['alice-z', '30'], ['alice-a', '10'], ['alice-m', '20']] });
      window.auditSearchInput = document.querySelector('#cb input');
    });
    await page.focus('#cb input');
    await page.keyboard.type('alice');
    const typed = await page.evaluate(() => ({
      same: document.querySelector('#cb input') === window.auditSearchInput,
      active: document.activeElement === window.auditSearchInput,
      value: window.auditSearchInput.value,
      selection: [window.auditSearchInput.selectionStart, window.auditSearchInput.selectionEnd]
    }));
    assert.deepEqual(typed, { same: true, active: true, value: 'alice', selection: [5, 5] });
    await page.click('#cb th:nth-child(2)');
    const rows = () => page.$$eval('#cb tbody tr', els => els.map(n => n.cells[0].textContent));
    assert.deepEqual(await rows(), ['alice-a', 'alice-m']);
    await page.evaluate(() => [...document.querySelectorAll('#cb button')].find(n => n.textContent === 'Next').click());
    assert.deepEqual(await rows(), ['alice-z']);
    await page.focus('#cb input');
    await page.keyboard.type('-a');
    assert.deepEqual(await rows(), ['alice-a']);
    assert.equal(await page.$eval('#cb input', n => n.value), 'alice-a');
    await page.evaluate(() => {
      const input = document.querySelector('#cb input');
      input.setSelectionRange(0, input.value.length);
    });
    await page.keyboard.press('Backspace');
    assert.deepEqual(await rows(), ['bob', 'alice-a']);
  });
});

test('TOTP 조회 HTTP 오류는 실패로 표시하며 비활성 배지·활성화 버튼을 만들지 않는다', async () => {
  await withPage([...COMMON, 'ui/modules/totp.js'], async page => {
    await page.evaluate(() => {
      window.authToken = 'fixture';
      PCV.totp.renderSettingsCard(document.getElementById('cb'));
    });
    await page.waitForFunction(() => document.getElementById('cb').textContent.includes('상태 조회 실패'));
    assert.equal(await page.$$eval('#cb .pill, #cb button', nodes => nodes.length), 0);
  }, { routes: { '/api/v1/auth/totp/status': { status: 503, body: { error: { message: 'unavailable' } } } } });
});

test('모니터링 상세 상태 제목·설명·가동 시간은 실제 ko/en 사전으로 표시한다', async () => {
  await withPage([...COMMON, 'ui/modules/filter-state.js', 'ui/modules/monitor.js'], async page => {
    for (const [lang, title, uptime] of [['ko', '시스템 상태', '가동 시간'], ['en', 'System Health', 'Uptime']]) {
      await page.evaluate(lang => {
        I18N.setLang(lang);
        window._L = (ko, en) => I18N.getLang() === 'ko' ? ko : en;
        window.authToken = 'fixture';
        renderMonOverview(document.getElementById('cb'), [], [], 0, 0, 0, 0, 0);
        loadDeepHealth();
      }, lang);
      await page.waitForFunction(text => document.getElementById('deep-health').textContent.includes(text), {}, uptime);
      const text = await page.$eval('#cb', n => n.textContent);
      assert.ok(text.includes(title));
      assert.ok(!/monitor\.(system_health|health_desc|uptime)/.test(text));
    }
  }, { routes: { '/api/v1/health': { body: { status: 'ok', uptime_sec: 60 } } } });
});

test('운영 센터는 누락 수치를 만들지 않고 실제 0·검색·조회 이동을 보존한다', async () => {
  await withPage([...COMMON, 'ui/modules/monitor.js'], async page => {
    await page.evaluate(async () => {
      window._L = (ko, en) => I18N.getLang() === 'ko' ? ko : en;
      window.authToken = 'fixture';
      window.auditNavCalls = [];
      window.navigateTo = route => { window.auditNavCalls.push(route); };
      const nativeFetch = window.fetch.bind(window);
      window.fetch = (url, options) => String(url).endsWith('/api/v1/metrics')
        ? Promise.resolve(new Response('unavailable', { status: 503 })) : nativeFetch(url, options);
      await renderOpsTriage(document.getElementById('cb'));
    });
    const initial = await page.$eval('#cb', host => ({
      values: [...host.querySelectorAll('.ops-triage-metric-value')].map(n => n.textContent),
      rows: [...host.querySelectorAll('tbody tr')].map(n => [...n.cells].map(c => c.textContent)),
      text: host.textContent, tabs: host.querySelectorAll('[role="tablist"]').length
    }));
    assert.deepEqual(initial.values.slice(0, 2), ['미수집', '미수집']);
    assert.deepEqual(initial.rows[0].slice(3, 5), ['미수집', '미수집']);
    assert.deepEqual(initial.rows[1].slice(3, 5), ['0.0%', '0.0%']);
    assert.ok(!/로그인 시도 증가|scrape 지연 확인|32GB 중 13.1GB/.test(initial.text));
    assert.equal(initial.tabs, 0);
    await page.type('.ops-triage-field', '192.0.2.22');
    assert.deepEqual(await page.$$eval('tbody tr', rows => rows.map(n => n.cells[0].textContent)), ['beta']);
    assert.equal(await page.$eval('.ops-triage-field', n => n === document.activeElement), true);
    await page.keyboard.type('x');
    assert.match(await page.$eval('tbody', n => n.textContent), /검색 결과가 없습니다/);
    await page.evaluate(() => [...document.querySelectorAll('#cb button')].find(n => n.textContent === '서버 감사 로그 조회').click());
    assert.deepEqual(await page.evaluate(() => window.auditNavCalls), ['mon-audit']);
  }, { routes: { '/api/v1/vms': { body: { data: [
    { name: 'alpha', running: 1, ip: '192.0.2.11' },
    { name: 'beta', running: 1, ip: '192.0.2.22', cpu: 0, memory_max_mb: 1024, memory_used_mb: 0 }
  ] } } } });
});
