import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const MODS = [
  'ui/i18n.js',
  'ui/modules/endpoints.js',
  'ui/modules/api.js',
  'ui/modules/ui.js',
  'ui/modules/filter-state.js',
  'ui/modules/uxlib.js',
  'ui/modules/modal-core.js',
  'ui/modules/modal.js',
  'ui/modules/charts.js',
  'ui/modules/metrics.js',
  'ui/modules/security.js',
  'ui/modules/monitor.js',
  'ui/modules/vm.js',
  'ui/modules/vm-console.js',
  'ui/modules/vm-lifecycle.js',
  'ui/modules/vm-guest.js',
  'ui/modules/container.js',
  'ui/modules/network.js',
  'ui/modules/storage.js',
  'ui/modules/help.js',
  'ui/modules/nav.js',
  'ui/modules/theme.js',
  'ui/modules/accounts.js',
  'ui/modules/advanced.js',
  'ui/modules/selfhealing.js',
  'ui/modules/mobile.js',
  'ui/app.js',
];

const VMS = [{ name: 'web-01', uuid: 'u-1', state: 'running', live_cpu_pct: 3 }];

async function boot(page, port) {
  await page.evaluate(async () => {
    const html = await fetch('/ui/index.html').then(response => response.text());
    document.body.innerHTML = new DOMParser().parseFromString(html, 'text/html').body.innerHTML;
    class FakeWS {
      constructor(url) {
        this.url = url;
        this.readyState = 0;
        this.sent = [];
        window.__fakeWs = this;
        setTimeout(() => { this.readyState = 1; if (this.onopen) this.onopen(); }, 0);
      }
      send(data) { this.sent.push(data); }
      close() { this.readyState = 3; if (this.onclose) this.onclose(); }
      emit(obj) { this.onmessage({ data: JSON.stringify(obj) }); }
    }
    FakeWS.CONNECTING = 0; FakeWS.OPEN = 1; FakeWS.CLOSING = 2; FakeWS.CLOSED = 3;
    window.WebSocket = FakeWS;
  });
  for (const moduleFile of MODS) {
    await page.addScriptTag({ url: `http://127.0.0.1:${port}/${moduleFile}` });
  }
  await page.evaluate(async () => {
    I18N.setLang('ko');
    window.authToken = 'test-token';
    window.connectWS();
    await new Promise(resolve => setTimeout(resolve, 20));
    window.__fakeWs.emit({ type: 'auth_ok' });
  });
}

const vmsCount = requests => requests.filter(request => request.path === '/api/v1/vms').length;
const PERIODIC = /WS Event — type: (metric|forecast|metrics-update|ping)\b/;

test('주기 WS 메시지(metric·forecast·metrics-update·ping)는 /vms 재조회도 이벤트 패널 재조립도 일으키지 않는다', async () => {
  await withPage([], async (page, { port, requests }) => {
    await boot(page, port);
    const before = vmsCount(requests);
    const usesBefore = await page.evaluate(() => document.querySelectorAll('#evp use').length);

    await page.evaluate(async () => {
      const ts = Math.floor(Date.now() / 1000);
      for (let i = 0; i < 6; i++) {
        window.__fakeWs.emit({ type: 'metric', ts, payload: { vm_count: 1 } });
        if (i === 1) window.__fakeWs.emit({ type: 'forecast', ts, payload: { cpu: 1, mem: 2 } });
        if (i === 3) window.__fakeWs.emit({ type: 'metrics-update', ts, payload: { cpu_percent: 1 } });
        if (i === 5) window.__fakeWs.emit({ type: 'ping', ts });
        await new Promise(resolve => setTimeout(resolve, 600));
      }
    });
    const after = vmsCount(requests);
    const state = await page.evaluate(pattern => ({
      uses: document.querySelectorAll('#evp use').length,
      noisy: window.eventLog.filter(entry => new RegExp(pattern).test(entry.msg)).length,
    }), PERIODIC.source);

    assert.ok(after - before <= 1, `/vms 재조회 ${after - before}회 (기대 <= 1)`);
    assert.equal(state.noisy, 0, '주기 메시지가 이벤트 로그에 쌓이면 안 된다');
    assert.equal(state.uses, usesBefore, '#evp 의 <use>(coolicons 스프라이트 참조)가 늘면 안 된다');
  }, { routes: { '/api/v1/vms': { body: { data: VMS } } } });
});

test('계약 유지: job.complete 와 vm.* 이벤트는 여전히 /vms 를 다시 읽는다', async () => {
  await withPage([], async (page, { port, requests }) => {
    await boot(page, port);
    await new Promise(resolve => setTimeout(resolve, 700));
    const b0 = vmsCount(requests);
    await page.evaluate(() => window.__fakeWs.emit({ type: 'job.complete', ts: 1, payload: { job_id: 'j', method: 'vm.start', status: 'completed' } }));
    await new Promise(resolve => setTimeout(resolve, 700));
    const b1 = vmsCount(requests);
    await page.evaluate(() => window.__fakeWs.emit({ type: 'vm.created', name: 'new-01' }));
    await new Promise(resolve => setTimeout(resolve, 300));
    const b2 = vmsCount(requests);
    assert.ok(b1 - b0 >= 1, 'job.complete 뒤 /vms 재조회가 있어야 한다');
    assert.ok(b2 - b1 >= 1, 'vm.created 뒤 /vms 재조회가 있어야 한다');
  }, { routes: { '/api/v1/vms': { body: { data: VMS } } } });
});

test('ping에는 {"type":"pong"} 한 번으로 답하고 재조회·이벤트 로그는 일으키지 않는다', async () => {
  await withPage([], async (page, { port, requests }) => {
    await boot(page, port);
    await new Promise(resolve => setTimeout(resolve, 700));
    const before = vmsCount(requests);
    const r = await page.evaluate(() => {
      const ws = window.__fakeWs;
      const n = ws.sent.length;
      ws.emit({ type: 'ping', ts: 1 });
      const afterPing = ws.sent.slice(n);
      const m = ws.sent.length;
      for (const type of ['metric', 'forecast', 'metrics-update']) ws.emit({ type, ts: 1 });
      return { afterPing, afterOthers: ws.sent.slice(m), noisy: window.eventLog.filter(e => /type: ping/.test(e.msg)).length };
    });
    assert.deepEqual(r.afterPing, ['{"type":"pong"}']);
    assert.deepEqual(r.afterOthers, [], '다른 주기 메시지에는 답하지 않는다');
    assert.equal(r.noisy, 0);
    await new Promise(resolve => setTimeout(resolve, 700));
    assert.ok(vmsCount(requests) - before <= 1, 'ping이 /vms 재조회를 부르면 안 된다');
  }, { routes: { '/api/v1/vms': { body: { data: VMS } } } });
});

test('닫히는 중인 소켓에는 pong을 보내지 않는다', async () => {
  await withPage([], async (page, { port }) => {
    await boot(page, port);
    const r = await page.evaluate(() => {
      const ws = window.__fakeWs;
      ws.readyState = 2;
      const n = ws.sent.length;
      ws.emit({ type: 'ping', ts: 1 });
      return ws.sent.slice(n);
    });
    assert.deepEqual(r, []);
  }, { routes: { '/api/v1/vms': { body: { data: VMS } } } });
});
