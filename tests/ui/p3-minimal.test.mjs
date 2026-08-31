                                                                                        
                                                 
                                                                 
  
                               
  
                                                                   
                                                     
                                                           
  
                                                
                                                                           
                                    
                                                                 
                                                                               
                                                              
                                                           
                                                               
                                                             
                                                        
                                                            
                                                                             
                                                                  
                                      
                                                             
  
                                               
   
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const MODS_NETWORK = ['ui/modules/ui.js', 'ui/modules/uxlib.js', 'ui/modules/endpoints.js', 'ui/modules/network.js'];
const MODS_CLOUD = ['ui/modules/ui.js', 'ui/modules/uxlib.js', 'ui/modules/endpoints.js', 'ui/modules/cloud.js'];
const MODS_ACCOUNTS = ['ui/modules/ui.js', 'ui/modules/uxlib.js', 'ui/modules/endpoints.js', 'ui/modules/accounts.js'];
const MODS_ADVANCED = ['ui/modules/ui.js', 'ui/modules/uxlib.js', 'ui/modules/endpoints.js', 'ui/modules/modal-core.js', 'ui/modules/advanced.js'];
              
const MODS_ACCOUNTS_MODAL = ['ui/modules/ui.js', 'ui/modules/uxlib.js', 'ui/modules/endpoints.js', 'ui/modules/modal-core.js', 'ui/modules/accounts.js'];
const MODS_MOBILE = ['ui/modules/ui.js', 'ui/modules/uxlib.js', 'ui/modules/filter-state.js', 'ui/modules/mobile.js'];

async function bootCommon(page, tab) {
  await page.setViewport({ width: 1280, height: 800 });
  await page.evaluate(tab => {
    window._DEBUG = true;
    window.authToken = 'test-token';
    window.currentTab = tab;
    window._L = (ko, en) => en;
    window.t = key => key;
    window.API_BASE = '/api/v1';
    window.unwrapData = r => r == null ? r
      : (r.data !== undefined ? r.data : (r.result !== undefined ? r.result : r));
    window.unwrapList = r => Array.isArray(r) ? r
      : (Array.isArray(window.unwrapData(r)) ? window.unwrapData(r) : []);
    window.fetchGet = url => fetch(url).then(async response => {
      const body = await response.json().catch(() => null);
      if (!response.ok) throw new Error((body && body.error && body.error.message) || `HTTP ${response.status}`);
      return body;
    });
    window.fetchPost = (url, body) => fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body)
    }).then(r => r.json());
  }, tab);
}

                                                                   
test('overlay networks: state pill (up=ok / down=crit), no legacy badge', async () => {
  const OVERLAY = [
    { name: 'ovl-a', vni: 100, peer_count: 2, state: 'up' },
    { name: 'ovl-b', vni: 200, peer_count: 0, state: 'down' }
  ];
  await withPage(MODS_NETWORK, async page => {
    await bootCommon(page, 'network');
    await page.evaluate(() => window.renderOverlayNetworks(document.getElementById('cb')));

    assert.equal(await page.$$eval('#cb .badge', els => els.length), 0, 'legacy HN.badge must be gone');
    const pills = await page.$$eval('#cb tbody .pill', els => els.map(e => ({ cls: e.className, text: e.textContent })));
    assert.equal(pills.length, 2);
    assert.match(pills[0].cls, /pill-ok/);
    assert.equal(pills[0].text, 'UP');
    assert.match(pills[1].cls, /pill-crit/);
    assert.equal(pills[1].text, 'DOWN');
  }, { routes: { '/api/v1/overlay': { status: 200, body: { data: OVERLAY } } } });
});

                                                                                                  
test('cloud migration jobs: direction pill(idle, both import/export) + status pill(done=ok/failed=crit/running=warn), no legacy badge', async () => {
  const JOBS = [
    { name: 'j-import', direction: 'import', status: 'done', progress_percent: 100 },
    { name: 'j-export', direction: 'export', status: 'failed', progress_percent: 40 },
    { name: 'j-running', direction: 'import', status: 'running', progress_percent: 55 }
  ];
  await withPage(MODS_CLOUD, async page => {
    await bootCommon(page, 'cloud');
    await page.evaluate(() => {
      const d = document.createElement('div');
      d.id = 'cm-jobs';
      document.getElementById('cb').appendChild(d);
    });
    await page.evaluate(() => window.cmLoadJobs());

    assert.equal(await page.$$eval('#cm-jobs .badge', els => els.length), 0, 'legacy HN.badge must be gone');
    const rows = await page.$$eval('#cm-jobs tbody tr', trs => trs.map(tr => {
      const pills = tr.querySelectorAll('.pill');
      return [0, 1].map(i => ({ cls: pills[i].className, text: pills[i].textContent }));
    }));
    assert.equal(rows.length, 3);
    assert.match(rows[0][0].cls, /pill-idle/, 'direction pill is idle regardless of import/export');
    assert.equal(rows[0][0].text, 'IMPORT');
    assert.match(rows[0][1].cls, /pill-ok/);
    assert.equal(rows[0][1].text, 'DONE');
    assert.match(rows[1][0].cls, /pill-idle/);
    assert.equal(rows[1][0].text, 'EXPORT');
    assert.match(rows[1][1].cls, /pill-crit/);
    assert.equal(rows[1][1].text, 'FAILED');
    assert.match(rows[2][1].cls, /pill-warn/);
    assert.equal(rows[2][1].text, 'RUNNING');
  }, { routes: { '/api/v1/cloud/jobs': { status: 200, body: { data: JOBS } } } });
});

                                                                         
test('accounts: heading RBAC pill(idle), no legacy badge', async () => {
  const USERS = [{ username: 'admin', role: 'admin', tenant: '-' }];
  await withPage(MODS_ACCOUNTS, async page => {
    await bootCommon(page, 'accounts');
    await page.evaluate(() => { window.currentUser = { role: 'admin' }; });
    await page.evaluate(() => window.renderAccounts(document.getElementById('cb')));

                                                                              
    const heading = await page.$eval('#cb .pagehead .pill', e => ({ cls: e.className, text: e.textContent }));
    assert.match(heading.cls, /pill-idle/);
    assert.equal(heading.text, 'RBAC');
    assert.equal(await page.$$eval('#cb .pagehead .badge', els => els.length), 0);
  }, { routes: { '/api/v1/auth/users': { status: 200, body: { data: USERS } } } });
});

                                                                                                                                   
test('accounts: API management grpc Config-based pill(idle) + request tester status pill(ok/crit), no legacy badge', async () => {
  await withPage(MODS_ACCOUNTS, async page => {
    await bootCommon(page, 'accounts');
    await page.evaluate(() => { window.currentUser = { role: 'admin' }; });
    await page.evaluate(() => window.renderApiManagement(document.getElementById('cb')));
                                                                           
    await page.waitForSelector('#grpc-status .pill');

    const grpcPill = await page.$eval('#grpc-status .pill', e => ({ cls: e.className, text: e.textContent }));
    assert.match(grpcPill.cls, /pill-idle/);
    assert.equal(grpcPill.text, 'Config-based');

                                      
    await page.evaluate(() => window.apiMgmtSend());
    await page.waitForSelector('#apimgmt-result .pill');
    let sendPill = await page.$eval('#apimgmt-result .pill', e => ({ cls: e.className, text: e.textContent }));
    assert.match(sendPill.cls, /pill-ok/);
    assert.equal(sendPill.text, '200');

                                       
    await page.evaluate(() => { document.getElementById('apimgmt-path').value = '/api/v1/does-not-exist'; });
    await page.evaluate(() => window.apiMgmtSend());
    await page.waitForFunction(() => document.querySelector('#apimgmt-result .pill')?.textContent === '404');
    sendPill = await page.$eval('#apimgmt-result .pill', e => ({ cls: e.className, text: e.textContent }));
    assert.match(sendPill.cls, /pill-crit/);
    assert.equal(sendPill.text, '404');

    assert.equal(await page.$$eval('#cb .badge', els => els.length), 0);
  }, { routes: {
    '/api/v1/health': { status: 200, body: { data: {} } },
    '/api/v1/auth/apikeys': { status: 200, body: { data: [] } },
    '/api/v1/vms': { status: 200, body: { data: [] } }
  } });
});

                                                                   
test('accounts: agent history urgency pill(high=crit/medium=warn/low=ok), no legacy badge', async () => {
  const HISTORY = {
    data: {
      consensus: 'migrate', confidence: 0.9, avg_latency_ms: 120, timestamp: 0,
      providers: [
        { provider: 'Claude', model: 'sonnet', action: 'migrate', confidence: 0.9, latency_ms: 100, urgency: 'high', success: true },
        { provider: 'OpenAI', model: 'gpt', action: 'migrate', confidence: 0.8, latency_ms: 110, urgency: 'medium', success: true },
        { provider: 'Gemini', model: 'gem', action: 'migrate', confidence: 0.7, latency_ms: 90, urgency: 'low', success: true }
      ]
    }
  };
  await withPage(MODS_ACCOUNTS, async page => {
    await bootCommon(page, 'accounts');
    await page.evaluate(() => window.renderAgentHistory(document.getElementById('cb')));

    assert.equal(await page.$$eval('#cb .badge', els => els.length), 0);
    const pills = await page.$$eval('#cb table .pill', els => els.map(e => ({ cls: e.className, text: e.textContent })));
    assert.equal(pills.length, 3);
    assert.match(pills[0].cls, /pill-crit/);
    assert.equal(pills[0].text, 'HIGH');
    assert.match(pills[1].cls, /pill-warn/);
    assert.equal(pills[1].text, 'MEDIUM');
    assert.match(pills[2].cls, /pill-ok/);
    assert.equal(pills[2].text, 'LOW');
  }, { routes: { '/api/v1/agent/history': { status: 200, body: HISTORY } } });
});

                                                                                      
test('accounts: testProvider result pill(Connected=ok/Failed=crit/Error=crit), no legacy badge', async () => {
  await withPage(MODS_ACCOUNTS, async page => {
    await bootCommon(page, 'accounts');
                                                             
                                                           
                                       
    await page.evaluate(() => {
      window._fetchCall = 0;
      window.fetch = (url, opts) => {
        const n = window._fetchCall++;
        if (n === 0) return Promise.resolve({ ok: true, status: 200 });
        if (n === 1) return Promise.resolve({ ok: false, status: 500 });
        return Promise.reject(new Error('network down'));
      };
                                                                       
      ['0', '1', '2'].forEach(i => {
        ['agr', 'agk', 'agm', 'age'].forEach(prefix => {
          const el = document.createElement(prefix === 'agr' ? 'span' : 'input');
          el.id = prefix + i;
          if (prefix === 'agk') el.value = 'sk-test-key';
          document.getElementById('cb').appendChild(el);
        });
      });
    });

    await page.evaluate(() => window.testProvider(0, 'Claude'));
    await page.waitForSelector('#agr0 .pill');
    const okPill = await page.$eval('#agr0 .pill', e => ({ cls: e.className, text: e.textContent }));
    assert.match(okPill.cls, /pill-ok/);
    assert.equal(okPill.text, 'Connected');

    await page.evaluate(() => window.testProvider(1, 'Claude'));
    await page.waitForSelector('#agr1 .pill');
    const failPill = await page.$eval('#agr1 .pill', e => ({ cls: e.className, text: e.textContent }));
    assert.match(failPill.cls, /pill-crit/);
    assert.equal(failPill.text, 'Failed');

    await page.evaluate(() => window.testProvider(2, 'Claude'));
    await page.waitForSelector('#agr2 .pill');
    const errPill = await page.$eval('#agr2 .pill', e => ({ cls: e.className, text: e.textContent }));
    assert.match(errPill.cls, /pill-crit/);
    assert.equal(errPill.text, 'Error');

    assert.equal(await page.$$eval('#cb .badge', els => els.length), 0);
  }, { routes: {} });
});

                                                                                                
test('accounts: API perf status pill(ok/crit) + grade pill(ok/warn/crit), no legacy badge', async () => {
  await withPage(MODS_ACCOUNTS, async page => {
    await bootCommon(page, 'accounts');
                                                      
                                                         
                                                             
                                                                    
                                                                      
                                                                    
                                                  
    await page.evaluate(() => {
      const times = [0, 10, 10, 20, 20, 190, 190, 200, 200, 210, 210, 810, 810, 820];
      let i = 0;
      window.performance.now = () => times[i++ % times.length];
    });
    await page.evaluate(() => window.renderApiPerf(document.getElementById('cb')));
    await page.waitForFunction(() => document.querySelectorAll('#cb table tbody tr').length === 7);

    assert.equal(await page.$$eval('#cb .badge', els => els.length), 0);
    const rows = await page.$$eval('#cb table tbody tr', trs => trs.map(tr => {
      const ep = tr.querySelector('td code').textContent;
      const pills = tr.querySelectorAll('.pill');
      return { ep, status: { cls: pills[0].className, text: pills[0].textContent }, grade: { cls: pills[1].className, text: pills[1].textContent } };
    }));
    const procRow = rows.find(r => r.ep === '/processes');
    assert.match(procRow.status.cls, /pill-crit/);
    assert.equal(procRow.status.text, 'ERROR');
    assert.match(procRow.grade.cls, /pill-ok/, '/processes 응답은 빠르므로(10ms) 상태와 무관하게 grade는 ok');
    const netRow = rows.find(r => r.ep === '/networks');
    assert.match(netRow.status.cls, /pill-ok/);
    assert.match(netRow.grade.cls, /pill-warn/, '170ms 고정 → B등급/warn');
    const alertRow = rows.find(r => r.ep === '/alerts');
    assert.match(alertRow.status.cls, /pill-ok/);
    assert.match(alertRow.grade.cls, /pill-crit/, '600ms 고정 → D등급/crit');
    const fastRow = rows.find(r => r.ep === '/vms');
    assert.match(fastRow.status.cls, /pill-ok/);
    assert.match(fastRow.grade.cls, /pill-ok/, '10ms 고정 → A+등급/ok');
  }, { routes: {
    '/api/v1/vms': { status: 200, body: {} },
    '/api/v1/containers': { status: 200, body: {} },
    '/api/v1/networks': { status: 200, body: {} },
    '/api/v1/storage/pools': { status: 200, body: {} },
    '/api/v1/health': { status: 200, body: {} },
    '/api/v1/alerts': { status: 200, body: {} }
                                                                       
  } });
});

                                                                                                 
test('accounts: API keys status pill(active=ok/revoked=idle) + expiry pill(expired=warn), no legacy badge', async () => {
  const now = Math.floor(Date.now() / 1000);
  const KEYS = [
    { client_name: 'k-active', role: 1, created_at: '2026-01-01', revoked: false, expires_at: 0 },
    { client_name: 'k-revoked', role: 1, created_at: '2026-01-01', revoked: true, expires_at: 0 },
    { client_name: 'k-expired', role: 1, created_at: '2026-01-01', revoked: false, expires_at: now - 3600 }
  ];
  await withPage(MODS_ACCOUNTS, async page => {
    await bootCommon(page, 'accounts');
    await page.evaluate(() => window.renderApiKeys(document.getElementById('cb')));

    assert.equal(await page.$$eval('#cb .badge', els => els.length), 0);
    const rows = await page.$$eval('#cb tbody tr', trs => trs.map(tr => {
      const cells = tr.querySelectorAll('td');
      const statusPill = cells[5].querySelector('.pill');
      const expPill = cells[4].querySelector('.pill');
      return {
        name: cells[0].textContent,
        status: statusPill ? { cls: statusPill.className, text: statusPill.textContent } : null,
        exp: expPill ? { cls: expPill.className, text: expPill.textContent } : null
      };
    }));
    const active = rows.find(r => r.name === 'k-active');
    assert.match(active.status.cls, /pill-ok/);
    assert.equal(active.status.text, 'Active');
    const revoked = rows.find(r => r.name === 'k-revoked');
    assert.match(revoked.status.cls, /pill-idle/);
    assert.equal(revoked.status.text, 'Revoked');
    const expired = rows.find(r => r.name === 'k-expired');
    assert.match(expired.exp.cls, /pill-warn/);
    assert.equal(expired.exp.text, 'Expired');
  }, { routes: { '/api/v1/auth/apikeys': { status: 200, body: { data: KEYS } } } });
});

                                                                      
test('advanced: template history action pill(create=ok/delete=crit/update=warn), no legacy badge', async () => {
  const HISTORY = [
    { timestamp: 't1', action: 'create', template: 'tpl-a', user: 'admin' },
    { timestamp: 't2', action: 'delete', template: 'tpl-b', user: 'admin' },
    { timestamp: 't3', action: 'update', template: 'tpl-c', user: 'admin' }
  ];
  await withPage(MODS_ADVANCED, async page => {
    await bootCommon(page, 'advanced');
    await page.evaluate(() => window.loadTemplateHistory());
    await page.waitForFunction(() => window.PCV.modalCore.currentBody() !== null);

    const pills = await page.evaluate(() => Array.from(window.PCV.modalCore.currentBody().querySelectorAll('table .pill')).map(e => ({ cls: e.className, text: e.textContent })));
    const badgeCount = await page.evaluate(() => window.PCV.modalCore.currentBody().querySelectorAll('.badge').length);
    assert.equal(badgeCount, 0, 'legacy HN.badge must be gone');
    assert.equal(pills.length, 3);
    assert.match(pills[0].cls, /pill-ok/);
    assert.equal(pills[0].text, 'CREATE');
    assert.match(pills[1].cls, /pill-crit/);
    assert.equal(pills[1].text, 'DELETE');
    assert.match(pills[2].cls, /pill-warn/);
    assert.equal(pills[2].text, 'UPDATE');
  }, { routes: { '/api/v1/templates/history': { status: 200, body: { data: HISTORY } } } });
});

                                                                        
                                                        
                                                                            
  
                  
                                                                 
                                                                
                                                      
                                        
                                                             
                                
                                                                      
                                        
                                                                           

                                                                                
test('accounts: user role chip pill(idle, uppercased) in DataTable, no legacy badge', async () => {
  const USERS = [
    { username: 'admin', role: 'admin', tenant: '-' },
    { username: 'ops1', role: 'operator', tenant: 't1' },
    { username: 'ro1', role: 'viewer', tenant: 't1' }
  ];
  await withPage(MODS_ACCOUNTS, async page => {
    await bootCommon(page, 'accounts');
    await page.evaluate(() => { window.currentUser = { role: 'admin' }; });
    await page.evaluate(() => window.renderAccounts(document.getElementById('cb')));
                                                                         
    await page.waitForFunction(() => document.querySelectorAll('#acct-table tbody tr').length === 3);

    assert.equal(await page.$$eval('#cb .badge', els => els.length), 0, 'legacy .badge role chip must be gone');
    const chips = await page.$$eval('#acct-table tbody tr', trs => trs.map(tr => {
      const pill = tr.querySelectorAll('td')[1].querySelector('.pill');
      return { cls: pill.className, text: pill.textContent };
    }));
    assert.equal(chips.length, 3);
                                                       
    chips.forEach(c => assert.match(c.cls, /pill-idle/));
    assert.deepEqual(chips.map(c => c.text), ['ADMIN', 'OPERATOR', 'VIEWER']);
  }, { routes: { '/api/v1/auth/users': { status: 200, body: { data: USERS } } } });
});

                                                                     
test('mobile alerts: Suricata pills(engine ACTIVE=ok/OFF=crit, threats>0=warn/0=ok, IPS ON=ok/OFF=crit), no legacy badge', async () => {
  await withPage(MODS_MOBILE, async page => {
    const on = await page.evaluate(() => {
      const node = window.PCV.mobile.buildAlerts({
        alerts: [], filter: [],
        suricata: { engine: { state: 'active' }, eve_tail: { alerts_crit: 4, alerts_warn: 7 } },
        ips: { enabled: true }
      });
      return {
        badges: node.querySelectorAll('.badge').length,
        pills: Array.from(node.querySelectorAll('.m-suricata .pill'), e => ({ cls: e.className, text: e.textContent }))
      };
    });
    assert.equal(on.badges, 0, 'legacy HN.badge must be gone');
    assert.equal(on.pills.length, 3);
    assert.match(on.pills[0].cls, /pill-ok/);
    assert.equal(on.pills[0].text, 'ACTIVE');
    assert.match(on.pills[1].cls, /pill-warn/, '위협 11건 → warn');
    assert.equal(on.pills[1].text, '11');
    assert.match(on.pills[2].cls, /pill-ok/);
    assert.equal(on.pills[2].text, 'ON');

    const off = await page.evaluate(() => {
      const node = window.PCV.mobile.buildAlerts({
        alerts: [], filter: [],
        suricata: { engine: { state: 'inactive' }, eve_tail: { alerts_crit: 0, alerts_warn: 0 } },
        ips: { enabled: false }
      });
      return {
        badges: node.querySelectorAll('.badge').length,
        pills: Array.from(node.querySelectorAll('.m-suricata .pill'), e => ({ cls: e.className, text: e.textContent }))
      };
    });
    assert.equal(off.badges, 0);
    assert.match(off.pills[0].cls, /pill-crit/);
    assert.equal(off.pills[0].text, 'OFF');
    assert.match(off.pills[1].cls, /pill-ok/, '위협 0건 → ok');
    assert.equal(off.pills[1].text, '0');
    assert.match(off.pills[2].cls, /pill-crit/);
    assert.equal(off.pills[2].text, 'OFF');
  });
});

                                                 
test('network OVN: availability pill + local switch/router surface, no public demo request or legacy badge', async () => {
  const ovnRoutes = available => ({
    '/api/v1/ovn/status': { status: 200, body: { data: { available } } },
    '/api/v1/ovn/switches': { status: 200, body: { data: ['edge-ls'] } },
    '/api/v1/ovn/routers': { status: 200, body: { data: ['edge-lr'] } }
  });
  const collect = () => page => page.$$eval('#cb .pill', els => els.map(e => ({ cls: e.className, text: e.textContent })));

  await withPage(MODS_NETWORK, async page => {
    await bootCommon(page, 'network');
    await page.evaluate(() => window.renderOvn(document.getElementById('cb')));
    await page.waitForFunction(() => document.querySelectorAll('#cb .pill').length > 0);

    assert.equal(await page.$$eval('#cb .badge', els => els.length), 0, 'legacy HN.badge must be gone');
    const pills = await collect()(page);
    const ready = pills.find(p => p.text === 'Ready');
    assert.ok(ready, 'available=true 이면 Ready pill 이 있다');
    assert.match(ready.cls, /pill-ok/);
    const avail = pills.find(p => p.text === 'Available');
    assert.match(avail.cls, /pill-ok/);
    const text = await page.$eval('#cb', el => el.textContent);
    assert.match(text, /edge-ls/);
    assert.match(text, /edge-lr/);
    assert.match(text, /Logical topology/);
    assert.match(text, /ACL policy add/);
    assert.doesNotMatch(text, /LB setup|Create LB|Load balancer status note/,
      'incomplete NFV LB lifecycle must not be exposed from the OVN product screen');
    assert.equal(await page.$$eval('button[onclick^="nfvLbCreate"]', els => els.length), 0);
    assert.doesNotMatch(text, /demo\.purecvisor\.site|ovn-visual|Public OVN demo health/);
  }, { routes: ovnRoutes(true) });

  await withPage(MODS_NETWORK, async page => {
    await bootCommon(page, 'network');
    await page.evaluate(() => window.renderOvn(document.getElementById('cb')));
    await page.waitForFunction(() => document.querySelectorAll('#cb .pill').length > 0);

    assert.equal(await page.$$eval('#cb .badge', els => els.length), 0);
    const pills = await collect()(page);
    assert.equal(pills.filter(p => p.text === 'Ready').length, 0, 'available=false 이면 Ready pill 자체가 없다');
    const unavail = pills.find(p => p.text === 'Unavailable');
    assert.ok(unavail);
    assert.match(unavail.cls, /pill-crit/);
  }, { routes: ovnRoutes(false) });
});

                                                                   
test('accounts: setAgentTab actually re-renders the agent tab body (closure bug regression)', async () => {
  const CONFIG = { data: { providers: [{ name: 'Claude', enabled: true, model: 'sonnet' }] } };
  const HISTORY = {
    data: {
      consensus: 'migrate', confidence: 0.9, avg_latency_ms: 120, timestamp: 0,
      providers: [
        { provider: 'Claude', model: 'sonnet', action: 'migrate', confidence: 0.9, latency_ms: 100, urgency: 'high', success: true }
      ]
    }
  };
  await withPage(MODS_ACCOUNTS_MODAL, async page => {
    await bootCommon(page, 'accounts');
    await page.evaluate(() => window.showAgentConfig());
                                                                    
    await page.waitForFunction(() => {
      const b = document.getElementById('agent-tab-body');
      return !!b && b.textContent.includes('Claude');
    });
    const before = await page.evaluate(() => document.getElementById('agent-tab-body').textContent);

                                                               
    assert.equal(await page.evaluate(() => typeof window.agentTab), 'undefined');
    assert.equal(await page.evaluate(() => typeof window.setAgentTab), 'function');

    assert.equal(await page.evaluate(() => document.querySelectorAll('dialog[open]').length), 1);

                                                               
    await page.click('dialog[open] [onclick="setAgentTab(\'history\')"]');
    await page.waitForFunction(() => {
      const b = document.getElementById('agent-tab-body');
      return !!b && b.textContent.includes('Last Consensus Result');
    });
    const after = await page.evaluate(() => document.getElementById('agent-tab-body').textContent);

                                                        
                                       
    assert.notEqual(before, after);
    assert.ok(before.includes('Claude'));
    assert.ok(!before.includes('Last Consensus Result'));
    assert.ok(after.includes('Last Consensus Result'));

                                                                          
                                                              
    assert.equal(await page.evaluate(() => document.querySelectorAll('dialog[open]').length), 1);
  }, { routes: {
    '/api/v1/agent/config': { status: 200, body: CONFIG },
    '/api/v1/agent/history': { status: 200, body: HISTORY }
  } });
});
