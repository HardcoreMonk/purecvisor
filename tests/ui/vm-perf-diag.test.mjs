                                                                                                  
                                                                                
                                                                                    
                                                                        
                                                                          
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const MODS = ['endpoints', 'api', 'ui', 'filter-state', 'uxlib', 'modal-core',
  'monitor', 'monitor-vmdiag', 'vm', 'vm-console'].map(x => 'ui/modules/' + x + '.js');

const OBS = 1794757394000;
function rec(name, over) {
  return Object.assign({
    name, uuid: 'u-' + name, state: 'running', quality: 'ok', reason: null, missing: [], sample_age_ms: 2000, interval_ms: 5000,
    last_ok_at_ms: OBS, vcpus: 4, mem_max_mb: 8192, mem_alloc_mb: 8192, mem_guest_used_mb: 2048, mem_guest_available_mb: 6144,
    mem_rss_mb: 4096, guest_stats_age_s: 1, cpu_pct: 37.5, cpu_host_pct: 2.1,
    disk: { rd_bps: 1.5e6, wr_bps: 2.5e5, rd_iops: 12.5, wr_iops: 3, rd_lat_ms: 1.25, wr_lat_ms: 4, fl_lat_ms: 0.5 },
    net: { rx_bps: 3.2e6, tx_bps: 1.1e6, rx_pps: 410, tx_pps: 220, drop_ps: 0, err_ps: 0.2 },
    contention: { vcpu_delay_pct: 6.5, vcpu_delay_max_pct: 9, throttled_pct: 0, throttled_max_pct: 0, throttled_periods_ratio: 0,
      psi: { cpu: { some_avg10: 3, full_avg10: 0 }, memory: { some_avg10: 0, full_avg10: 0 }, io: { some_avg10: 1.5, full_avg10: 0.5 } },
      swap_in_kbps: 0, swap_out_kbps: 0, major_faults_ps: 0.1 },
    hints: []
  }, over);
}
const NULLS = { cpu_pct: null, cpu_host_pct: null, mem_alloc_mb: null, mem_guest_used_mb: null, mem_guest_available_mb: null,
  mem_rss_mb: null, guest_stats_age_s: null, contention: null, hints: [],
  disk: { rd_bps: null, wr_bps: null, rd_iops: null, wr_iops: null, rd_lat_ms: null, wr_lat_ms: null, fl_lat_ms: null },
  net: { rx_bps: null, tx_bps: null, rx_pps: null, tx_pps: null, drop_ps: null, err_ps: null } };
const RECS = {
  'vm-a': rec('vm-a', { hints: [{ code: 'cpu_wait', level: 'warn', value: 6.5, threshold: 5 }] }),
  'vm-b': rec('vm-b', { quality: 'partial', missing: ['guest_memory'], mem_guest_used_mb: null, mem_guest_available_mb: null }),
  'vm-c': rec('vm-c', Object.assign({ state: 'shutoff', quality: 'stopped', vcpus: 2, mem_max_mb: 2048 }, NULLS)),
  'vm-d': rec('vm-d', Object.assign({ state: 'paused', quality: 'stopped', vcpus: 2, mem_max_mb: 4096 }, NULLS))
};
async function boot(page) {
  await page.evaluate((recs, obs) => {
    Object.assign(window, {
      _L: (ko, en) => ko, t: key => key, authToken: 'fixture', _DEBUG: false,
      currentUser: { role: 'ADMIN' }, currentTab: 'performance', selectedVmIndex: 0, checkedVms: new Set(),
      vmList: [{ name: 'vm-a', uuid: 'u-vm-a', state: 'running' }, { name: 'vm-b', uuid: 'u-vm-b', state: 'running' },
               { name: 'vm-c', uuid: 'u-vm-c', state: 'shutoff' }, { name: 'vm-d', uuid: 'u-vm-d', state: 'paused' }],
      cpuHistory: Array(60).fill(0), memHistory: Array(60).fill(0),
      loadAll: () => {}, render: () => {}, renderContent: () => {}, addEvt: () => {}, toast: () => {}, createLineChart: () => {},
      __urls: []
    });
    window.__diag = name => ({ schema: 1, scope: 'all',
      sampler: { state: 'ok', observed_at_ms: obs, age_ms: 2000, interval_ms: 5000, tracked: 4, truncated: 0, error: null },
      host: { logical_cpus: 16, physical_cores: 8, mem_total_mb: 65536, allocation: {}, psi: null }, vms: recs[name] ? [recs[name]] : [] });
    window.__summaryMetrics = { cpu: 12.5, mem: -1, vcpu: 4, memory_mb: 8192, disk_rd: 1000, disk_wr: 2000, net_rx: 3000, net_tx: 4000,
      disk_rd_req: 55, disk_wr_req: 66, net_rx_pkts: 77, net_tx_pkts: 88 };
    window.fetch = async (url) => {
      const u = String(url);
      window.__urls.push(u);
      const m = u.match(/\/monitor\/vm-diag\?vm=([^&]+)/);
      const body = m ? { data: window.__diag(decodeURIComponent(m[1])) }                            
        : /\/vms\/[^/]+$/.test(u) ? { data: window.__summaryMetrics } : { data: [] };
      return new Response(JSON.stringify(body), { status: 200, headers: { 'content-type': 'application/json' } });
    };
  }, RECS, OBS);
}
async function perf(page, i) {
  await page.evaluate(async i => { window.selectedVmIndex = i; await renderPerformance(document.getElementById('cb'), vmList[i]); }, i);
  return page.evaluate(() => ({ text: document.getElementById('cb').textContent, urls: window.__urls.slice(),
    titles: [...document.querySelectorAll('#cb h4')].map(h => h.textContent) }));
}

test('perf tab calls diag only', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    const r = await perf(page, 0);
    assert.ok(r.urls.some(u => u.endsWith('/monitor/vm-diag?vm=vm-a')));
    assert.equal(r.urls.filter(u => /\/vms\/vm-a$/.test(u)).length, 0, 'GET /vms/{n}(vm.metrics) 를 부르지 않는다');
    for (const s of ['1.5 MB/s', '250.0 KB/s', '12.5 IOPS', '3.0 IOPS', '3.2 MB/s', '1.1 MB/s', '410.0 pps', '220.0 pps', '1.3 / 4.0 / 0.5 ms'])
      assert.ok(r.text.includes(s), '응답 값 그대로: ' + s);
    assert.ok(!/\bops\b/.test(r.text), '누적 요청 수(ops)를 그리지 않는다');
    const b = await perf(page, 1);
    assert.ok(b.text.includes('게스트 메모리 통계 없음'));
    assert.ok(!b.text.includes('20.0%'));
  });
});

test('perf tab titles and stopped vm', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    const a = await perf(page, 0);
    assert.ok(a.titles.every(t => !t.includes('(60s)')), a.titles.join(' | '));
    assert.ok(a.text.includes('최근 표본 60개 · 10초 간격'));
    const c = await perf(page, 2);
    assert.ok(c.text.includes('정지 VM — 설정값만'));
    assert.match(c.text, /vCPU2/);
    assert.ok(c.text.includes('2.0 GiB'));
    const d = await perf(page, 3);
    assert.ok(d.text.includes('일시정지 VM — 설정값만'));
    assert.ok(!/(^|[^0-9])-1([^0-9]|$)/.test(c.text + d.text));
  });
});

test('perf tab hint sentences visible', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    await perf(page, 0);
    const li = await page.evaluate(() => [...document.querySelectorAll('#cb .vmdiag-hint-list li')].map(n => {
      const clone = n.cloneNode(true);
      clone.querySelectorAll('.sr-only').forEach(x => x.remove());
      return clone.textContent;
    }));
    assert.equal(li.length, 1);
    assert.match(li[0], /CPU 대기 · 주의/);
    assert.match(li[0], /기다린 시간이 6\.5%입니다\(잠정 기준 5% 이상\)/);
    assert.match(li[0], /CPU 상한\(vm\.limit\)에 걸려 멈춘 시간은 빼고 셉니다/);                                   
  });
});

test('perf polling follows vm and tab', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    await page.evaluate(() => {
      window.__ticks = {};
      window.setInterval = (fn, ms) => { window.__ticks[ms] = fn; return 1; };
      window.clearInterval = () => {};
    });
    await perf(page, 0);
    const diagUrls = () => page.evaluate(() => window.__urls.filter(u => u.includes('/monitor/vm-diag')));
    assert.equal((await diagUrls()).length, 1, '진입 조회 1건(즉시 tick 없음)');
    assert.ok(await page.evaluate(() => typeof window.__ticks[10000] === 'function'), '10초 tick');
    await page.evaluate(() => { window.selectedVmIndex = 1; });                             
    await page.evaluate(() => window.__ticks[10000]());
    await new Promise(r => setTimeout(r, 200));
    let urls = await diagUrls();
    assert.ok(urls[urls.length - 1].endsWith('?vm=vm-b'), urls.join(' '));
    await page.evaluate(() => { window.currentTab = 'summary'; window.__ticks[10000](); });                        
    await new Promise(r => setTimeout(r, 200));
    urls = await diagUrls();
    assert.equal(urls.length, 2);
  });
});

test('summary tab cumulative labels', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    await page.evaluate(async () => { window.currentTab = 'summary'; await renderSummary(document.getElementById('cb'), vmList[0]); });
    await page.waitForFunction(() => /읽기 요청\(누적\)/.test(document.getElementById('cb').textContent), { timeout: 3000 });
    const t = await page.evaluate(() => document.getElementById('cb').textContent);
    for (const s of ['읽기 요청(누적)', '쓰기 요청(누적)', '수신 패킷(누적)', '송신 패킷(누적)']) assert.ok(t.includes(s), s);
    assert.ok(!/IOPS/.test(t) && !/\bpps\b/.test(t), '누적값과 IOPS·pps 가 함께 나오지 않는다');
  });
});

test('negative mem renders dash everywhere', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    await page.evaluate(async () => { window.currentTab = 'summary'; await renderSummary(document.getElementById('cb'), vmList[0]); });
    await page.waitForFunction(() => /읽기 요청\(누적\)/.test(document.getElementById('cb').textContent), { timeout: 3000 });
    const summary = await page.evaluate(() => document.getElementById('cb').textContent);
    assert.ok(!/(^|[^0-9])-1(\.0)?%/.test(summary), '요약 메모리 사용률에 -1 이 없다');
    assert.equal(await page.evaluate(() => vmList[0].mem_percent), undefined, '-1 을 목록 객체에 되쓰지 않는다');
                                       
    const order = await page.evaluate(() => {
      vmList.forEach((v, i) => { v.mem_percent = [-1, 50, 10, -1][i]; });
      window.sortField = 'mem'; window.sortDirection = -1;
      if (typeof getFavorites !== 'function') window.getFavorites = () => [];
      return getFiltered().map(v => v.name);
    });
    assert.deepEqual(order.slice(0, 2), ['vm-b', 'vm-c']);
               
    await page.evaluate(async () => {
      window.__summaryMetrics = Object.assign({}, window.__summaryMetrics, { mem: -1 });
      vmList.forEach(v => { v.mem_percent = -1; v.state = 'running'; });
      checkedVms.add(0); checkedVms.add(1);
      await showVmCompare();
    });
    const modal = await page.evaluate(() => (PCV.modalCore && PCV.modalCore.currentBody() ? PCV.modalCore.currentBody().textContent : document.body.textContent));
    assert.ok(!/(^|[^0-9])-1(\.0)?%/.test(modal), '비교 모달에 -1% 가 없다');
    assert.ok(modal.includes('—'));
  });
});
