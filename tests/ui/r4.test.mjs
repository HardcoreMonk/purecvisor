                                                                                        
                                                            
                                                                     
  
                                           
                                                                          
  
        
                                                                          
                                                                            
                                                 
                                               
  
                                                               
                        
   
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const ALERT_MODS = [
  'ui/modules/ui.js',
  'ui/modules/uxlib.js',
  'ui/modules/filter-state.js',
  'ui/modules/endpoints.js',
  'ui/modules/monitor.js'
];

const SHELL_MODS = [
  'ui/modules/endpoints.js', 'ui/modules/api.js', 'ui/modules/ui.js',
  'ui/modules/filter-state.js', 'ui/modules/uxlib.js',
  'ui/modules/modal-core.js', 'ui/modules/modal.js',
  'ui/modules/shell.js'
];

const MOBILE_MODS = [
  'ui/modules/ui.js', 'ui/modules/uxlib.js', 'ui/modules/filter-state.js',
  'ui/modules/endpoints.js', 'ui/modules/mobile.js'
];

const CONFIG = Object.freeze({
  enabled: true,
  cpu_warn: 80, cpu_crit: 95, mem_warn: 85, mem_crit: 95,
  disk_warn: 80, disk_crit: 90, eval_period: 30,
  webhook_url: '', webhook_format: 'generic', telegram_chat_id: '',
  dedup_window: 300, alert_count: 0, composite_rules: [],
  config_revision: 1, daemon_config_valid: true, daemon_config_error: ''
});

function alertRoutes(history) {
  return {
    '/api/v1/alerts/config': { status: 200, body: { data: CONFIG } },
    '/api/v1/alerts': { status: 200, body: history }
  };
}

                                                           
                                 
function rpcOkRoute() {
  return record => ({
    status: 200,
    body: { jsonrpc: '2.0', id: record.json && record.json.id, result: true }
  });
}

async function bootAlerts(page, opts) {
  opts = opts || {};
  await page.evaluate((role) => {
    window.currentTab = 'mon-alerts';
    window.authToken = 'test-token';
    window._DEBUG = false;
    window.addEvt = () => {};
    PCV.api = PCV.api || {};
    PCV.api.RPC_ERROR = { CONFLICT: -32002 };
    window.t = key => key;
    window._L = ko => ko;
    window.unwrapData = value => (value && value.data !== undefined ? value.data : value);
    window.unwrapList = value => {
      const data = window.unwrapData(value);
      return Array.isArray(data) ? data : [];
    };
    window.fetchGet = url => fetch(url).then(async response => {
      const json = await response.json();
      if (!response.ok) {
        const error = new Error(json.error?.message || `HTTP ${response.status}`);
        error.code = json.error?.code;
        throw error;
      }
      if (json.error) {
        const error = new Error(json.error.message);
        error.code = json.error.code;
        throw error;
      }
      return json;
    });
    window.fetchPost = (url, body) => fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body)
    }).then(response => response.json());
    window.fetchPut = () => Promise.reject(new Error('unexpected PUT'));
    if (role) window.currentUser = { role };
  }, opts.role || null);
  await page.evaluate(() => PCV.monitor.renderAlerts(document.getElementById('cb')));
  await page.waitForSelector('#alert-config-region:not([aria-busy="true"])', { timeout: 3000 });
  await page.waitForSelector('#alert-history-region:not([aria-busy="true"])', { timeout: 3000 });
}

test('ACK 클릭 → RPC body {method:"alert.ack", params:{alert_id}}', async () => {
  const history = [{
    alert_id: 501, timestamp: 100, severity: 'warn', metric: 'cpu',
    value: 81, message: 'cpu high', acknowledged: false
  }];
  await withPage(ALERT_MODS, async (page, ctx) => {
    await bootAlerts(page, { role: 'ADMIN' });
    await page.click('[data-ack-id="501"]');
    await page.waitForFunction(() => !document.querySelector('[data-ack-id="501"]'));
    const rpcCalls = ctx.requests.filter(r => r.path === '/api/v1/rpc');
    assert.equal(rpcCalls.length, 1);
    assert.equal(rpcCalls[0].json.method, 'alert.ack');
    assert.deepEqual(rpcCalls[0].json.params, { alert_id: 501 });
    assert.equal(rpcCalls[0].json.id, 'ack-501');
  }, { routes: { ...alertRoutes(history), '/api/v1/rpc': rpcOkRoute() } });
});

test('ACK 클릭 → 행 상태 전환(버튼→idle pill), 확인된 행은 애초에 pill', async () => {
  const history = [
    { alert_id: 601, timestamp: 200, severity: 'crit', metric: 'disk', value: 97, message: 'unacked', acknowledged: false },
    { alert_id: 602, timestamp: 100, severity: 'warn', metric: 'mem', value: 88, message: 'already acked', acknowledged: true }
  ];
  await withPage(ALERT_MODS, async (page) => {
    await bootAlerts(page, { role: 'ADMIN' });
    const before = await page.evaluate(() => ({
      hasButton601: !!document.querySelector('[data-ack-id="601"]'),
      row602Pill: document.querySelector('.alert-row-warn .pill-idle')?.textContent || null,
      row602Button: !!document.querySelector('.alert-row-warn button[data-ack-id]')
    }));
    assert.equal(before.hasButton601, true);
    assert.equal(before.row602Pill, 'ACK');
    assert.equal(before.row602Button, false);

    await page.click('[data-ack-id="601"]');
    await page.waitForFunction(() => !document.querySelector('[data-ack-id="601"]'));
    const after = await page.evaluate(() => ({
      hasButton601: !!document.querySelector('[data-ack-id="601"]'),
      row601Pill: document.querySelector('.alert-row-crit .pill-idle')?.textContent || null
    }));
    assert.equal(after.hasButton601, false);
    assert.equal(after.row601Pill, 'ACK');
  }, { routes: { ...alertRoutes(history), '/api/v1/rpc': rpcOkRoute() } });
});

test('VIEWER: ACK 버튼 role-hidden(computed) — OPERATOR/ADMIN 은 그대로 노출', async () => {
  const history = [{
    alert_id: 701, timestamp: 100, severity: 'warn', metric: 'cpu',
    value: 81, message: 'm', acknowledged: false
  }];
  await withPage(ALERT_MODS, async (page) => {
    await bootAlerts(page, { role: 'VIEWER' });
    const viewerDisplay = await page.$eval(
      '[data-ack-id="701"]', node => getComputedStyle(node).display
    );
    assert.equal(viewerDisplay, 'none');

    await page.evaluate(() => {
      window.currentUser = { role: 'ADMIN' };
      return PCV.monitor.renderAlerts(document.getElementById('cb'));
    });
    await page.waitForSelector('#alert-history-region:not([aria-busy="true"])', { timeout: 3000 });
    const adminDisplay = await page.$eval(
      '[data-ack-id="701"]', node => getComputedStyle(node).display
    );
    assert.notEqual(adminDisplay, 'none');
  }, { routes: { ...alertRoutes(history), '/api/v1/rpc': rpcOkRoute() } });
});

test('ACK 전체: customConfirm 확인 후 미확인 전체를 배치 확인 처리한다', async () => {
  const history = [
    { alert_id: 801, timestamp: 300, severity: 'crit', metric: 'cpu', value: 95, message: 'a', acknowledged: false },
    { alert_id: 802, timestamp: 200, severity: 'warn', metric: 'mem', value: 86, message: 'b', acknowledged: false },
    { alert_id: 803, timestamp: 100, severity: 'warn', metric: 'disk', value: 82, message: 'c', acknowledged: true }
  ];
  await withPage(ALERT_MODS, async (page, ctx) => {
    await bootAlerts(page, { role: 'ADMIN' });
    await page.evaluate(() => {
      window.__confirmArgs = null;
      PCV.ui.customConfirm = (title, message) => {
        window.__confirmArgs = [title, message];
        return Promise.resolve(true);
      };
    });
    await page.click('[data-alert-ack-all]');
    await page.waitForFunction(() => !document.querySelector('[data-ack-id]'));
    const rpcCalls = ctx.requests.filter(r => r.path === '/api/v1/rpc');
    assert.equal(rpcCalls.length, 2, 'only the 2 unacknowledged rows call alert.ack');
    assert.deepEqual(
      rpcCalls.map(r => r.json.params.alert_id).sort(),
      [801, 802]
    );
    assert.ok(rpcCalls.every(r => r.json.method === 'alert.ack'));
    const confirmed = await page.evaluate(() => window.__confirmArgs);
    assert.ok(confirmed[1].includes('2'), confirmed[1]);
    const finalLabel = await page.$eval('[data-alert-ack-all]', node => node.textContent);
    assert.equal(finalLabel, '전체 확인');
  }, { routes: { ...alertRoutes(history), '/api/v1/rpc': rpcOkRoute() } });
});

test('ACK 전체: customConfirm 취소 시 RPC 를 보내지 않는다', async () => {
  const history = [{
    alert_id: 811, timestamp: 100, severity: 'warn', metric: 'cpu', value: 81, message: 'a', acknowledged: false
  }];
  await withPage(ALERT_MODS, async (page, ctx) => {
    await bootAlerts(page, { role: 'ADMIN' });
    await page.evaluate(() => { PCV.ui.customConfirm = () => Promise.resolve(false); });
    await page.click('[data-alert-ack-all]');
    await new Promise(resolve => setTimeout(resolve, 30));
    assert.equal(ctx.requests.filter(r => r.path === '/api/v1/rpc').length, 0);
  }, { routes: { ...alertRoutes(history), '/api/v1/rpc': rpcOkRoute() } });
});

  
                                                                               
                                                          
                                                                  
                                              
  
                                                        
                                                               
                                                          
   
test('ACK 후 늦게 도착하는 재조회(WS 이벤트발)가 확인된 행을 되돌리지 않는다 (R4-F3)', async () => {
  const initialHistory = [{
    alert_id: 901, timestamp: 100, severity: 'warn', metric: 'cpu',
    value: 81, message: 'race target', acknowledged: false
  }];
  let releaseStaleAlerts;
  const staleGate = new Promise(resolve => { releaseStaleAlerts = resolve; });
  const routes = {
    '/api/v1/alerts/config': { status: 200, body: { data: CONFIG } },
    '/api/v1/alerts': async (record, requestsSoFar) => {
      const gets = requestsSoFar.filter(r => r.method === 'GET' && r.path === '/api/v1/alerts');
      if (gets.length <= 1) return { status: 200, body: initialHistory };
      await staleGate;                                                   
      return { status: 200, body: initialHistory };                                       
    },
    '/api/v1/rpc': rpcOkRoute()
  };
  await withPage(ALERT_MODS, async (page) => {
                                                                     
                                                                 
                                                                        
                                                                        
    try {
      await bootAlerts(page, { role: 'ADMIN' });

      await page.click('[data-alert-refresh]');
      await page.waitForSelector('[data-alert-refresh][aria-busy="true"]');

                                      
      await page.click('[data-ack-id="901"]');
      await page.waitForFunction(() => !document.querySelector('[data-ack-id="901"]'));
      const afterAck = await page.evaluate(() => ({
        refreshBusy: document.querySelector('[data-alert-refresh]').getAttribute('aria-busy'),
        refreshDisabled: document.querySelector('[data-alert-refresh]').disabled,
        refreshLabel: document.querySelector('[data-alert-refresh]').textContent,
        pill: document.querySelector('.alert-row-warn .pill-idle')?.textContent || null
      }));
      assert.equal(afterAck.pill, 'ACK');
      assert.equal(afterAck.refreshBusy, 'false');
      assert.equal(afterAck.refreshDisabled, false);
      assert.equal(afterAck.refreshLabel, '새로고침');

                                                            
      releaseStaleAlerts();
      await new Promise(resolve => setTimeout(resolve, 80));
      const after = await page.evaluate(() => ({
        hasAckButton: !!document.querySelector('[data-ack-id="901"]'),
        refreshBusy: document.querySelector('[data-alert-refresh]').getAttribute('aria-busy'),
        refreshDisabled: document.querySelector('[data-alert-refresh]').disabled,
        refreshLabel: document.querySelector('[data-alert-refresh]').textContent
      }));
      assert.equal(after.hasAckButton, false, 'stale response must not resurrect the ACK button');
      assert.equal(after.refreshBusy, 'false');
      assert.equal(after.refreshDisabled, false);
      assert.equal(after.refreshLabel, '새로고침');
    } finally {
      releaseStaleAlerts();
    }
  }, { routes });
});

test('shell.js statusbar: Critical 세그먼트 sub 의 unack 수치가 감소를 반영한다', async () => {
  await withPage(SHELL_MODS, async (page) => {
    await page.evaluate(() => {
      document.body.appendChild(Object.assign(document.createElement('aside'), { id: 'shell-sidebar' }));
      const main = document.createElement('div');
      ['shell-topbar', 'shell-statusbar'].forEach(id => {
        const d = document.createElement('div'); d.id = id; main.appendChild(d);
      });
      document.body.appendChild(main);
      window._L = (ko) => ko;
      window.currentTab = 'dashboard';
      window.navigateTo = () => {};
      window.currentUser = { role: 'admin' };
    });
    const r = await page.evaluate(() => {
      const snap = base => ({
        vms: { run: 5, total: 6 }, ctrs: { run: 4, total: 4 },
        alerts: { crit: 2, warn: 5, total: 7, unack: base },
        sec: { count1h: 7, worst: 'warn' },
        storage: { worstPool: { name: 'pcvpool', pct: 86, state: 'DEGRADED' } },
        healing: { pending: 2 }
      });
      PCV.shell.mount();
      PCV.shell.update(snap(4));
      const before = document.querySelector('#shell-statusbar .seg-crit').textContent;
      PCV.shell.update(snap(1));
      const after = document.querySelector('#shell-statusbar .seg-crit').textContent;
      return { before, after };
    });
    assert.match(r.before, /4 unack/);
    assert.match(r.after, /1 unack/);
  });
});

  
                                          
                                                                    
                                                     
   
const THEME_MODS = ['ui/modules/theme.js'];

test('theme.js: sanitizeTheme 이 supanova-mockup 을 허용 테마로 통과시킨다', async () => {
  await withPage(THEME_MODS, async (page) => {
    const r = await page.evaluate(() => ({
                                                                 
                                           
      sanitized: window.sanitizeTheme('supanova-mockup'),
      preview: window.THEME_PREVIEWS.find(p => p.id === 'supanova-mockup')
    }));
    assert.equal(r.sanitized, 'supanova-mockup');
    assert.ok(r.preview);
    assert.equal(r.preview.colors.length, 4);
  });
});

test('mobile.js: 알림 행 ACK 버튼이 데스크톱과 동일한 RPC body 를 보낸다', async () => {
  await withPage(MOBILE_MODS, async (page, ctx) => {
    await page.evaluate(() => {
      window._L = (ko) => ko;
      window.fetchPost = (url, body) => fetch(url, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body)
      }).then(response => response.json());
      const node = window.PCV.mobile.buildAlerts({
        alerts: [{
          alert_id: 901, timestamp: 100, severity: 'warn', metric: 'cpu',
          value: 80, message: 'm', acknowledged: false
        }],
        filter: []
      });
      document.getElementById('cb').appendChild(node);
    });
    await Promise.all([
      page.waitForResponse(res => res.url().includes('/api/v1/rpc')),
      page.click('[data-ack-id="901"]')
    ]);
    const rpcCalls = ctx.requests.filter(r => r.path === '/api/v1/rpc');
    assert.equal(rpcCalls.length, 1);
    assert.equal(rpcCalls[0].json.method, 'alert.ack');
    assert.deepEqual(rpcCalls[0].json.params, { alert_id: 901 });
  }, { routes: { '/api/v1/rpc': rpcOkRoute() } });
});

  
                           
                                                                          
                                                                          
                                                               
                                
                                                                     
                                    
   
const CONSOLE_MODS = [
  'ui/modules/uxlib.js', 'ui/modules/endpoints.js', 'ui/modules/api.js',
  'ui/modules/vm-console.js'
];

test('vm-console.js: fullscreenchange 로 fullscreen 이탈 시 vncFitWindow 가 자동 호출된다', async () => {
  await withPage(CONSOLE_MODS, async (page) => {
    const r = await page.evaluate(() => {
      const c = document.createElement('div');
      c.id = 'vnc-container';
      c.style.height = '100vh';
      document.body.appendChild(c);
      window._vncRfb = { scaleViewport: false, resizeSession: false };
      document.dispatchEvent(new Event('fullscreenchange'));
      return {
        scaleViewport: window._vncRfb.scaleViewport,
        resizeSession: window._vncRfb.resizeSession,
        height: document.getElementById('vnc-container').style.height
      };
    });
    assert.equal(r.scaleViewport, true);
    assert.equal(r.resizeSession, true);
    assert.equal(r.height, '');
  });
});

test('shell.js statusbar: R1-F5 — 10s 재조립 후에도 포커스가 같은 세그먼트에 남는다', async () => {
  await withPage(SHELL_MODS, async (page) => {
    await page.evaluate(() => {
      document.body.appendChild(Object.assign(document.createElement('aside'), { id: 'shell-sidebar' }));
      const main = document.createElement('div');
      ['shell-topbar', 'shell-statusbar'].forEach(id => {
        const d = document.createElement('div'); d.id = id; main.appendChild(d);
      });
      document.body.appendChild(main);
      window._L = (ko) => ko;
      window.currentTab = 'dashboard';
      window.navigateTo = () => {};
      window.currentUser = { role: 'admin' };
    });
    const r = await page.evaluate(() => {
      const snap = base => ({
        vms: { run: 5, total: 6 }, ctrs: { run: 4, total: 4 },
        alerts: { crit: 2, warn: 5, total: 7, unack: base },
        sec: { count1h: 7, worst: 'warn' },
        storage: { worstPool: { name: 'pcvpool', pct: 86, state: 'DEGRADED' } },
        healing: { pending: 2 }
      });
      PCV.shell.mount();
      PCV.shell.update(snap(4));
      const critSeg = document.querySelector('#shell-statusbar .seg-crit');
      const segKey = critSeg.dataset.segKey;
      critSeg.focus();
      const before = document.activeElement === critSeg;
      PCV.shell.update(snap(1));
      const after = document.activeElement;
      return {
        before,
        segKey,
        afterInStatusbar: document.getElementById('shell-statusbar').contains(after),
        afterSegKey: after && after.dataset ? after.dataset.segKey : null
      };
    });
    assert.equal(r.before, true, 'precondition: crit segment actually holds focus');
    assert.equal(r.afterInStatusbar, true);
    assert.equal(r.afterSegKey, r.segKey);
  });
});

  
                           
                                                                          
                                                               
                                                             
                                                                    
  
                                                                            
                                                             
                                                                    
   
const PALETTE_MODS = [
  'ui/modules/endpoints.js', 'ui/modules/api.js', 'ui/modules/ui.js',
  'ui/modules/filter-state.js', 'ui/modules/uxlib.js',
  'ui/modules/modal-core.js', 'ui/modules/modal.js',
  'ui/modules/help.js', 'ui/modules/nav.js'
];

async function bootPalette(page) {
  await page.evaluate(() => {
    window._DEBUG = false;
    window.authToken = 'test-token';
    window.currentTab = 'dashboard';
    window.currentUser = { role: 'admin' };
    window.pcvClusterEnabled = false;
    window._navCalls = [];
    window.navigateTo = n => { window._navCalls.push(n); return true; };
    window.vmList = [
      { name: 'web-01', state: 'running' },
      { name: 'db-01', state: 'shutoff' }
    ];
                                             
                                                               
    window._shellSlow = { raw: {
      ctrs: [
        { name: 'nginx-a', state: 'RUNNING', ip_addr: '10.20.30.41' },
        { name: 'redis-b', state: 'RUNNING', ip_addr: '10.20.30.42' }
      ],
      pools: [
        { name: 'pcvpool', health: 'ONLINE' },
        { name: 'backup-pool', health: 'DEGRADED' }
      ],
      fleet: [
        { name: 'web-01', ip: '10.20.30.55' },
        { name: 'db-01', ip: 'N/A' }
      ],
      networks: [
        { name: 'pcvbr0', mode: 'bridge' },
        { name: 'nat-internal', mode: 'nat' }
      ]
    } };
  });
}

test('⌘K 팔레트: VM 이름 질의 → VM 그룹 + 클릭 시 이름 재조회 selectedVmIndex·라우팅', async () => {
  await withPage(PALETTE_MODS, async (page) => {
    await bootPalette(page);
    const r = await page.evaluate(() => {
      window.openCmdPalette();
      const inp = document.getElementById('cmd-input');
      const emptyItems = document.querySelectorAll('.cmd-item').length;
      const emptyGroups = document.querySelectorAll('.cmd-group').length;
      inp.value = 'web-01';
      inp.dispatchEvent(new Event('input'));
      const groups = [...document.querySelectorAll('.cmd-group')].map(x => x.textContent);
      const vmItem = [...document.querySelectorAll('.cmd-item')].find(x => x.textContent.includes('web-01'));
                                                         
                                       
      window.vmList = [{ name: 'db-01', state: 'shutoff' }, { name: 'web-01', state: 'running' }];
      vmItem.click();
      return {
        emptyItems, emptyGroups, groups,
        vmItemText: vmItem.textContent,
        selectedVmIndex: window.selectedVmIndex,
        nav: window._navCalls,
        placeholder: inp.getAttribute('placeholder')
      };
    });
    assert.equal(r.emptyGroups, 0, '빈 질의는 명령만 — 그룹 헤더 없음');
    assert.ok(r.emptyItems > 0);
    assert.ok(r.groups.includes('가상 머신'), `VM 그룹 없음: ${JSON.stringify(r.groups)}`);
    assert.ok(r.vmItemText.includes('web-01'));
    assert.equal(r.selectedVmIndex, 1);
    assert.deepEqual(r.nav, ['summary']);
    assert.equal(r.placeholder, '명령·객체·IP 검색');
  });
});

test('⌘K 팔레트: 컨테이너 IP 질의 → 컨테이너 그룹 + 클릭 시 selCtr·containers 라우팅', async () => {
  await withPage(PALETTE_MODS, async (page) => {
    await bootPalette(page);
    const r = await page.evaluate(() => {
      window.openCmdPalette();
      const inp = document.getElementById('cmd-input');
      inp.value = '10.20.30.42';
      inp.dispatchEvent(new Event('input'));
      const groups = [...document.querySelectorAll('.cmd-group')].map(x => x.textContent);
      const items = [...document.querySelectorAll('.cmd-item')].map(x => x.textContent);
      const hit = [...document.querySelectorAll('.cmd-item')].find(x => x.textContent.includes('redis-b'));
      hit.click();
      return { groups, items, selCtr: window.selCtr, nav: window._navCalls };
    });
    assert.ok(r.groups.includes('컨테이너'), `컨테이너 그룹 없음: ${JSON.stringify(r.groups)}`);
    assert.equal(r.items.filter(t => t.includes('redis-b')).length, 1);
    assert.equal(r.items.filter(t => t.includes('nginx-a')).length, 0, 'IP 질의는 매치 컨테이너만');
    assert.equal(r.selCtr, 'redis-b');
    assert.deepEqual(r.nav, ['containers']);

                                                              
    const guarded = await page.evaluate(() => {
      window._shellSlow = undefined;
      try { window.renderCmdPalette('10.20.30'); return 'ok'; } catch (e) { return String(e && e.message); }
    });
    assert.equal(guarded, 'ok');
  });
});

                                                                           
                                                                      
test('⌘K 팔레트: 풀 이름 질의 → 풀 그룹 + 클릭 시 storage 라우팅', async () => {
  await withPage(PALETTE_MODS, async (page) => {
    await bootPalette(page);
    const r = await page.evaluate(() => {
      window.openCmdPalette();
      const inp = document.getElementById('cmd-input');
      inp.value = 'backup';
      inp.dispatchEvent(new Event('input'));
      const groups = [...document.querySelectorAll('.cmd-group')].map(x => x.textContent);
      const items = [...document.querySelectorAll('.cmd-item')].map(x => x.textContent);
      const hit = [...document.querySelectorAll('.cmd-item')].find(x => x.textContent.includes('backup-pool'));
      hit.click();
      return { groups, items, nav: window._navCalls };
    });
    assert.ok(r.groups.includes('풀'), `풀 그룹 없음: ${JSON.stringify(r.groups)}`);
    assert.equal(r.items.filter(t => t.includes('backup-pool')).length, 1);
    assert.equal(r.items.filter(t => t.includes('pcvpool')).length, 0, '질의는 매치 풀만');
    assert.deepEqual(r.nav, ['storage']);

                                                                        
    const guarded = await page.evaluate(() => {
      window._shellSlow = undefined;
      try { window.renderCmdPalette('pool'); return 'ok'; } catch (e) { return String(e && e.message); }
    });
    assert.equal(guarded, 'ok');
  });
});

test('⌘K 팔레트: VM IP 질의 → VM 그룹 히트(힌트 IP 표기)', async () => {
  await withPage(PALETTE_MODS, async (page) => {
    await bootPalette(page);
    const r = await page.evaluate(() => {
      window.openCmdPalette();
      const inp = document.getElementById('cmd-input');
      inp.value = '20.30';                                               
      inp.dispatchEvent(new Event('input'));
      const groups = [...document.querySelectorAll('.cmd-group')].map(x => x.textContent);
      const hit = [...document.querySelectorAll('.cmd-item')].find(x => x.textContent.includes('web-01'));
      const hitText = hit ? hit.textContent : null;
      hit && hit.click();
      return { groups, hitText, nav: window._navCalls, selectedVmIndex: window.selectedVmIndex };
    });
    assert.ok(r.groups.includes('가상 머신'), `VM 그룹 없음: ${JSON.stringify(r.groups)}`);
    assert.ok(r.hitText && r.hitText.includes('10.20.30.55'), `힌트에 IP 없음: ${r.hitText}`);
    assert.deepEqual(r.nav, ['summary']);
  });
});

test('⌘K 팔레트: fleet IP "N/A" 는 매치 대상에서 제외된다(리뷰 HIGH-1)', async () => {
  await withPage(PALETTE_MODS, async (page) => {
    await bootPalette(page);
    const r = await page.evaluate(() => {
      window.openCmdPalette();
      const inp = document.getElementById('cmd-input');
      inp.value = 'a';                                                            
      inp.dispatchEvent(new Event('input'));
      const items = [...document.querySelectorAll('.cmd-item')].map(x => x.textContent);
      return { items };
    });
    assert.equal(r.items.filter(t => t.includes('db-01')).length, 0,
      '"N/A" IP 를 걸러내지 않으면 한 글자 질의에도 db-01 이 오탐 매치된다');
  });
});

test('⌘K 팔레트: 네트워크 이름 질의 → 네트워크 그룹 + 클릭 시 networks 라우팅', async () => {
  await withPage(PALETTE_MODS, async (page) => {
    await bootPalette(page);
    const r = await page.evaluate(() => {
      window.openCmdPalette();
      const inp = document.getElementById('cmd-input');
      inp.value = 'pcvbr';
      inp.dispatchEvent(new Event('input'));
      const groups = [...document.querySelectorAll('.cmd-group')].map(x => x.textContent);
      const items = [...document.querySelectorAll('.cmd-item')].map(x => x.textContent);
      const hit = [...document.querySelectorAll('.cmd-item')].find(x => x.textContent.includes('pcvbr0'));
      hit.click();
      return { groups, items, nav: window._navCalls };
    });
    assert.ok(r.groups.includes('네트워크'), `네트워크 그룹 없음: ${JSON.stringify(r.groups)}`);
    assert.equal(r.items.filter(t => t.includes('pcvbr0')).length, 1);
    assert.equal(r.items.filter(t => t.includes('nat-internal')).length, 0, '질의는 매치 네트워크만');
    assert.deepEqual(r.nav, ['networks']);

                                                                              
    const guarded = await page.evaluate(() => {
      window._shellSlow = undefined;
      try { window.renderCmdPalette('pcvbr'); return 'ok'; } catch (e) { return String(e && e.message); }
    });
    assert.equal(guarded, 'ok');
  });
});

test('toggleGlobalSearch: 구 global-search 소멸 → 팔레트 토글 위임(dialog 정확히 1)', async () => {
  await withPage(PALETTE_MODS, async (page) => {
    await bootPalette(page);
    const opened = await page.evaluate(() => {
      window.toggleGlobalSearch();
      return {
        dialogs: document.querySelectorAll('dialog.pcv-dialog').length,
        palettes: document.querySelectorAll('dialog.pcv-dialog.cmd-palette').length,
        legacy: document.querySelectorAll('dialog.pcv-dialog.global-search, .global-search-box').length,
        input: !!document.getElementById('cmd-input'),
        open: window.cmdPaletteOpen,
        removed: ['doGlobalSearch', 'closeGlobalSearch'].map(k => typeof window[k])
      };
    });
    assert.equal(opened.dialogs, 1);
    assert.equal(opened.palettes, 1);
    assert.equal(opened.legacy, 0);
    assert.equal(opened.input, true);
    assert.equal(opened.open, true);
    assert.deepEqual(opened.removed, ['undefined', 'undefined'], 'globalSearch 잔재 심볼 소멸');

    const closed = await page.evaluate(async () => {
      window.toggleGlobalSearch();
                                                                 
                                                              
                                                                 
                                                      
      const deadline = performance.now() + 2000;
      while (performance.now() < deadline) {
        if (document.querySelectorAll('dialog.pcv-dialog').length === 0) break;
        await new Promise(r => setTimeout(r, 5));
      }
      return { dialogs: document.querySelectorAll('dialog.pcv-dialog').length, open: window.cmdPaletteOpen };
    });
    assert.equal(closed.dialogs, 0);
    assert.equal(closed.open, false);
  });
});

test('openCmdPalette: 재진입 가드 — 열린 상태 재호출해도 dialog 1 유지(H-1)', async () => {
  await withPage(PALETTE_MODS, async (page) => {
    await bootPalette(page);
    const r = await page.evaluate(() => {
      window.openCmdPalette();
      document.getElementById('cmd-input').value = 'web';
      document.body.focus();
      window.openCmdPalette();                                         
      window.toggleGlobalSearch.length;                            
      const inp = document.getElementById('cmd-input');
      return {
        dialogs: document.querySelectorAll('dialog.pcv-dialog').length,
        value: inp.value,
        focused: document.activeElement === inp,
        open: window.cmdPaletteOpen
      };
    });
    assert.equal(r.dialogs, 1, '재진입 가드 부재 — 고아 dialog 발생');
    assert.equal(r.value, 'web', '같은 dialog 가 유지돼 입력이 보존돼야 한다');
    assert.equal(r.focused, true, '재진입은 기존 입력으로 포커스 복귀');
    assert.equal(r.open, true);
  });
});

                                                                    
                                                   
                                                                            
test('cmd-palette 항목 텍스트가 검정(UA CanvasText)으로 렌더되지 않는다', async () => {
  await withPage(PALETTE_MODS, async (page) => {
    await bootPalette(page);
    const r = await page.evaluate(() => {
      window.openCmdPalette();
      const item = document.querySelector('.cmd-item span');
      const dlg = document.querySelector('dialog[open]');
      return {
        item: item ? getComputedStyle(item).color : null,
        dlg: dlg ? getComputedStyle(dlg).color : null
      };
    });
    assert.ok(r.item && r.item !== 'rgb(0, 0, 0)', 'item color: ' + r.item);
    assert.ok(r.dlg && r.dlg !== 'rgb(0, 0, 0)', 'dialog color: ' + r.dlg);
  });
});
