                                                                                             
                                                                  
                                                                            
  
                                               
  
                  
                                                       
                                                          
                             
                                                              
                                                       
                                                      
  
               
                                               
                                         
   
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
  'ui/modules/totp.js',
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
  'ui/modules/cloud.js',
  'ui/modules/help.js',
  'ui/modules/nav.js',
  'ui/modules/theme.js',
  'ui/modules/accounts.js',
  'ui/modules/advanced.js',
  'ui/modules/selfhealing.js',
  'ui/modules/mobile.js',
  'ui/app.js',
];

const HEALTH_OK = {
  status: 'ok',
  service: 'purecvisorsd',
  version: '2.0.0',
  node_name: 'boot-node',
  uptime_sec: 93784,               
  checks: {
    libvirt: { ok: true, latency_ms: 4, sla: 'ok' },
    disk: { ok: true, avail_gb: 41.25 },
    tls: { mode: 'plain', enabled: false, degraded: false, status: 'disabled_by_config' },
  },
};

const ROUTES = { '/api/v1/health': { body: HEALTH_OK } };

                                                     
                                                          
async function bootLogin(page, port) {
  await page.evaluate(async () => {
    const html = await fetch('/ui/index.html').then(response => response.text());
    const parsed = new DOMParser().parseFromString(html, 'text/html');
    document.body.innerHTML = parsed.body.innerHTML;
  });
  for (const moduleFile of MODS) {
    await page.addScriptTag({ url: `http://127.0.0.1:${port}/${moduleFile}` });
  }
  await page.waitForFunction(
    () => document.getElementById('lh-node').textContent === 'boot-node',
    { timeout: 3000 }
  );
}

function readPanel() {
  const text = id => document.getElementById(id).textContent;
  const cls = id => document.getElementById(id).className;
  return {
    state: text('lh-state'),
    dot: cls('lh-dot'),
    node: text('lh-node'),
    version: text('lh-version'),
    uptime: text('lh-uptime'),
    kvm: text('lh-kvm'),
    kvmDot: cls('lh-kvm-dot'),
    disk: text('lh-disk'),
    diskDot: cls('lh-disk-dot'),
  };
}

test('_loginHealthTick: 부트 1회 틱이 /health 응답으로 노드 패널을 채운다', async () => {
  await withPage([], async (page, { port, requests }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await bootLogin(page, port);

    const panel = await page.evaluate(readPanel);

    assert.deepEqual(pageErrors, []);
    assert.ok(
      requests.some(r => r.path === '/api/v1/health'),
      'boot tick must call the unauthenticated health endpoint'
    );
    assert.equal(panel.state, '정상 운영');
    assert.equal(panel.dot, 'login-dot login-dot-ok');
    assert.equal(panel.node, 'boot-node');
    assert.equal(panel.version, 'v2.0.0');
    assert.equal(panel.uptime, '1d 2h');
    assert.equal(panel.kvm, '연결됨');
    assert.equal(panel.kvmDot, 'login-dot login-dot-ok');
    assert.equal(panel.disk, '41.3 GB 여유');
    assert.equal(panel.diskDot, 'login-dot login-dot-ok');
  }, { routes: ROUTES });
});

test('_loginHealthTick: critical 바디(503 동반)는 crit dot 으로 읽힌다', async () => {
  await withPage([], async (page, { port }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await bootLogin(page, port);

    const panel = await page.evaluate(async () => {
                                                   
      window.fetchGet = () => Promise.resolve({
        status: 'critical',
        version: '2.0.0',
        node_name: 'sick-node',
        uptime_sec: 120,
        checks: {
          libvirt: { ok: false, error: 'connection failed' },
          disk: { ok: false, avail_gb: 0.05 },
        },
      });
      await window._loginHealthTick();
      const text = id => document.getElementById(id).textContent;
      const cls = id => document.getElementById(id).className;
      return {
        state: text('lh-state'),
        dot: cls('lh-dot'),
        node: text('lh-node'),
        uptime: text('lh-uptime'),
        kvm: text('lh-kvm'),
        kvmDot: cls('lh-kvm-dot'),
        diskDot: cls('lh-disk-dot'),
      };
    });

    assert.deepEqual(pageErrors, []);
    assert.equal(panel.dot, 'login-dot login-dot-crit');
    assert.equal(panel.state, '점검 필요');
    assert.equal(panel.node, 'sick-node');
    assert.equal(panel.uptime, '2m');
    assert.equal(panel.kvm, '연결 실패');
    assert.equal(panel.kvmDot, 'login-dot login-dot-crit');
    assert.equal(panel.diskDot, 'login-dot login-dot-crit');
  }, { routes: ROUTES });
});

test('_loginHealthTick: 네트워크 실패는 idle("노드 응답 대기")로 되돌린다', async () => {
  await withPage([], async (page, { port }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await bootLogin(page, port);

    const panel = await page.evaluate(async () => {
      window.fetchGet = () => Promise.reject(new Error('network down'));
      await window._loginHealthTick();
      const text = id => document.getElementById(id).textContent;
      const cls = id => document.getElementById(id).className;
      return {
        state: text('lh-state'),
        dot: cls('lh-dot'),
        node: text('lh-node'),
        version: text('lh-version'),
        uptime: text('lh-uptime'),
        kvmDot: cls('lh-kvm-dot'),
        diskDot: cls('lh-disk-dot'),
      };
    });

    assert.deepEqual(pageErrors, []);
    assert.equal(panel.state, '노드 응답 대기');
    assert.equal(panel.dot, 'login-dot login-dot-idle');
    assert.equal(panel.node, '—');
    assert.equal(panel.version, '—');
    assert.equal(panel.uptime, '—');
    assert.equal(panel.kvmDot, 'login-dot login-dot-idle');
    assert.equal(panel.diskDot, 'login-dot login-dot-idle');
  }, { routes: ROUTES });
});

test('_loginHealthTick: 오버레이가 숨겨지면 요청도 DOM 갱신도 없다', async () => {
  await withPage([], async (page, { port }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await bootLogin(page, port);

    const result = await page.evaluate(async () => {
      let calls = 0;
      window.fetchGet = () => {
        calls += 1;
        return Promise.resolve({ status: 'critical', node_name: 'must-not-paint' });
      };
                                          
      window.pcvSetLoginVisible(false);
      await window._loginHealthTick();
      const hidden = { calls, node: document.getElementById('lh-node').textContent };
                                             
      window.pcvSetLoginVisible(true);
      await window._loginHealthTick();
      return { hidden, visibleCalls: calls, visibleNode: document.getElementById('lh-node').textContent };
    });

    assert.deepEqual(pageErrors, []);
    assert.equal(result.hidden.calls, 0);
    assert.equal(result.hidden.node, 'boot-node');
    assert.equal(result.visibleCalls, 1);
    assert.equal(result.visibleNode, 'must-not-paint');
  }, { routes: ROUTES });
});
