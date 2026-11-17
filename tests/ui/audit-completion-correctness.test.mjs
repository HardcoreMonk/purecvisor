                                                          
                                                           
                       
                                                      
                                     
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

                                                                                          
                                                                            
const OPS_VM_LIST = { data: [
  { uuid: 'u-alpha', name: 'alpha', state: 'running', vcpu: 2, memory_mb: 2048, storage_type: 'zfs', network_count: 1 },
  { uuid: 'u-beta', name: 'beta', state: 'running', vcpu: 2, memory_mb: 1024, storage_type: 'zfs', network_count: 1 },
  { uuid: 'u-gamma', name: 'gamma', state: 'shut off', vcpu: 1, memory_mb: 512, storage_type: 'zfs', network_count: 1 }
] };
function opsDiagVm(name, over) {
  return Object.assign({ name, uuid: 'u-' + name, state: 'running', quality: 'ok', reason: null, missing: [], vcpus: 2,
    mem_max_mb: 2048, mem_alloc_mb: 2048, mem_guest_used_mb: 1024, mem_guest_available_mb: 1024, mem_rss_mb: 1800,
    cpu_pct: 5, cpu_host_pct: 0.5, hints: [] }, over);
}
const OPS_DIAG = { schema: 1, scope: 'all',
  sampler: { state: 'ok', observed_at_ms: 1794757394000, age_ms: 2000, interval_ms: 5000, tracked: 2, truncated: 0, error: null },
  host: { logical_cpus: 8, physical_cores: 4, mem_total_mb: 16384, allocation: {}, psi: {} },
  vms: [
    opsDiagVm('alpha', { quality: 'partial', missing: ['guest_memory'], mem_guest_used_mb: null, mem_guest_available_mb: null, mem_rss_mb: 1536 }),
    opsDiagVm('beta', { mem_max_mb: 1024, mem_alloc_mb: 1024, mem_guest_used_mb: 0, mem_rss_mb: 300 }),
    opsDiagVm('gamma', { state: 'shutoff', quality: 'stopped', vcpus: 1, mem_max_mb: 512, mem_alloc_mb: null, mem_guest_used_mb: null, mem_rss_mb: null })
  ] };

test('운영 센터는 누락 수치를 만들지 않고 실제 0·검색·조회 이동을 보존한다', async () => {
  await withPage([...COMMON, 'ui/modules/monitor.js', 'ui/modules/monitor-vmdiag.js'], async page => {
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
      memTitles: [...host.querySelectorAll('tbody tr')].map(n => (n.cells[4].querySelector('[title]') || {}).title || ''),
      text: host.textContent, tabs: host.querySelectorAll('[role="tablist"]').length
    }));
    assert.deepEqual(initial.values.slice(0, 2), ['미수집', '미수집']);
    assert.equal(initial.rows.length, 3);
                                                                                
    assert.match(initial.rows[0][4], /^— RSS 1\.5 GiB/);
    assert.match(initial.memTitles[0], /게스트 메모리/);
                                            
    assert.equal(initial.rows[1][4], '0.0%');
                         
    assert.match(initial.rows[2][4], /^—/);
    assert.match(initial.memTitles[2], /정지 VM/);
    assert.ok(!initial.rows.some(r => r[4].includes('미수집')), 'VM 메모리 칸에 미수집이 남지 않는다');
    assert.ok(!/로그인 시도 증가|scrape 지연 확인|32GB 중 13.1GB/.test(initial.text));
    assert.equal(initial.tabs, 0);
    await page.type('.ops-triage-field', 'bet');
    assert.deepEqual(await page.$$eval('tbody tr', rows => rows.map(n => n.cells[0].textContent)), ['beta']);
    assert.equal(await page.$eval('.ops-triage-field', n => n === document.activeElement), true);
    await page.keyboard.type('x');
    assert.match(await page.$eval('tbody', n => n.textContent), /검색 결과가 없습니다/);
    await page.evaluate(() => [...document.querySelectorAll('#cb button')].find(n => n.textContent === '서버 감사 로그 조회').click());
    assert.deepEqual(await page.evaluate(() => window.auditNavCalls), ['mon-audit']);
  }, { routes: { '/api/v1/vms': { body: OPS_VM_LIST }, '/api/v1/monitor/vm-diag': { body: { data: OPS_DIAG } } } });                            
});

test('운영 센터 VM 메모리는 진단 실패를 미수집이 아니라 실패로 구분한다', async () => {
  for (const [reply, word] of [[{ status: 500, body: { error: { message: 'x' } } }, '진단 실패'],
    [{ status: 404, body: { error: { code: 'NOT_FOUND', message: 'Endpoint not found or method not allowed' } } }, '진단 미지원']]) {
    await withPage([...COMMON, 'ui/modules/monitor.js', 'ui/modules/monitor-vmdiag.js'], async page => {
      await page.evaluate(async () => {
        window._L = (ko, en) => I18N.getLang() === 'ko' ? ko : en;
        window.authToken = 'fixture';
        const nativeFetch = window.fetch.bind(window);
        window.fetch = (url, options) => String(url).endsWith('/api/v1/metrics')
          ? Promise.resolve(new Response('unavailable', { status: 503 })) : nativeFetch(url, options);
        await renderOpsTriage(document.getElementById('cb'));
      });
      const rows = await page.$$eval('#cb tbody tr', trs => trs.map(n => ({ mem: n.cells[4].textContent,
        title: (n.cells[4].querySelector('[title]') || {}).title || '' })));
      assert.equal(rows.length, 3);
      for (const r of rows) {
        assert.ok(r.mem.startsWith('—') && r.mem.includes(word), `메모리 칸 ${JSON.stringify(r.mem)} 에 "${word}"`);
        assert.ok(!r.mem.includes('미수집'));
      }
    }, { routes: { '/api/v1/vms': { body: OPS_VM_LIST }, '/api/v1/monitor/vm-diag': reply } });
  }
});

                                                                                               
                                                                              
test('운영 센터 VM 메모리는 게스트 값이 오래됐으면 지금 값처럼 보이지 않는다', async () => {
  const list = { data: [
    { uuid: 'u-stale', name: 'stale', state: 'running', vcpu: 1, memory_mb: 1024 },
    { uuid: 'u-fresh', name: 'fresh', state: 'running', vcpu: 1, memory_mb: 1024 }
  ] };
  const diag = Object.assign({}, OPS_DIAG, { vms: [
    opsDiagVm('stale', { mem_max_mb: 1024, mem_alloc_mb: 1024, mem_guest_used_mb: 257, mem_rss_mb: 546, guest_stats_age_s: 2954,
      hints: [{ code: 'guest_stats_stale', level: 'notice', value: 2954, threshold: 30 }] }),
    opsDiagVm('fresh', { mem_max_mb: 1024, mem_alloc_mb: 1024, mem_guest_used_mb: 256, mem_rss_mb: 500, guest_stats_age_s: 3 })
  ] });
  await withPage([...COMMON, 'ui/modules/monitor.js', 'ui/modules/monitor-vmdiag.js'], async page => {
    await page.evaluate(async () => {
      window._L = (ko, en) => I18N.getLang() === 'ko' ? ko : en;
      window.authToken = 'fixture';
      const nativeFetch = window.fetch.bind(window);
      window.fetch = (url, options) => String(url).endsWith('/api/v1/metrics')
        ? Promise.resolve(new Response('unavailable', { status: 503 })) : nativeFetch(url, options);
      await renderOpsTriage(document.getElementById('cb'));
    });
    const rows = await page.$$eval('#cb tbody tr', trs => trs.map(n => ({ name: n.cells[0].textContent, mem: n.cells[4].textContent,
      title: (n.cells[4].querySelector('[title]') || {}).title || '' })));
    const stale = rows.find(r => r.name.includes('stale'));
    const fresh = rows.find(r => r.name.includes('fresh'));
    assert.match(stale.mem, /^— RSS 546 MiB/);
    assert.ok(!stale.mem.includes('%'), `오래된 값이 막대로 보이면 안 된다: ${stale.mem}`);
    assert.match(stale.title, /게스트 값 오래됨\(2954초 전\)/);
    assert.equal(fresh.mem, '25%');                                    
  }, { routes: { '/api/v1/vms': { body: list }, '/api/v1/monitor/vm-diag': { body: { data: diag } } } });
});
