                                                                                             
                                                                                   
                                                                          
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

                                                                             
const MODS = [
  'ui/modules/ui.js',
  'ui/modules/uxlib.js',
  'ui/modules/endpoints.js',
  'ui/modules/filter-state.js',
  'ui/modules/security.js'
];

                                                                                  
const EVENTS = [
  { event_id: 'ev-1', timestamp: 1753600000, severity: 'crit', source: 'suricata', status: 'open',
    target: '10.0.0.9', target_kind: 'ip', summary: 'port scan', recommended_action: 'block_ip',
    confidence: 92, occurrence_count: 3, evidence_json: '{}' },
  { event_id: 'ev-2', timestamp: 1753600100, severity: 'warn', source: 'file_integrity', status: 'action_pending',
    target: '/etc/passwd', target_kind: 'file', summary: 'integrity drift', recommended_action: 'manual_runbook',
    confidence: 60, occurrence_count: 1, evidence_json: '{}' },
  { event_id: 'ev-3', timestamp: 1753600200, severity: 'info', source: 'suricata', status: 'resolved',
    target: 'sshd', target_kind: 'service', summary: 'benign probe', recommended_action: '',
    confidence: 10, occurrence_count: 1, evidence_json: '{}' },
  { event_id: 'ev-4', timestamp: 1753600300, severity: 'warn', source: 'runtime', status: 'suppressed',
    target: 'gti12', target_kind: 'host', summary: 'rate spike', recommended_action: '',
    confidence: 40, occurrence_count: 2, evidence_json: '{}' }
];
const ACTIONS = [
  { event_id: 'ev-1', action: 'block_ip', target: '10.0.0.9', ttl_sec: 600 },
  { event_id: 'ev-2', action: 'manual_runbook', target: '/etc/passwd', ttl_sec: 0 }
];
const CFG = { enabled: true, baseline_status: 'trusted', open_risk: 2, pending_actions: 2, degraded: false };
const SURI_STATUS = {
  engine: { state: 'active', binary_present: true },
  eve_tail: { running: true, alerts_crit: 4, alerts_warn: 7, alerts_info: 11, dropped: 0 },
  rules: { count: 67856, update: { running: false, last_result: 'ok', last_url: 'https://rules.example/et.tar.gz' } },
  policy: { auto_isolate: 'dry_run', tenants: 1 }
};
const SURI_POLICY = { auto_isolate: 'dry_run', tenants: { global: { inspect: true, profile: 'default' } }, drop_sids: [2034647] };
const SURI_IPS = {
  engine: { state: 'active', binary_present: true }, enabled: true, queue_num: 0,
  fail_open: true, mode: 'nfqueue', degraded: false,
  last_toggle: { running: false, op: 'enable', result: 'ok', ts: 1753600400 }
};

                                                                           
                                                     
const ROUTES = {
  '/api/v1/rpc': record => {
    const m = record.json && record.json.method;
    const result =
      m === 'security.config.get' ? CFG :
      m === 'security.event.list' ? EVENTS :
      m === 'security.action.pending' ? ACTIONS :
      m === 'security.event.get'
        ? (EVENTS.find(e => e.event_id === (record.json.params || {}).event_id) || {})
        : {};
    return { status: 200, body: { jsonrpc: '2.0', id: record.json && record.json.id, result } };
  },
  '/api/v1/suricata/status': { status: 200, body: { data: SURI_STATUS } },
  '/api/v1/suricata/policy': record => record.method === 'GET'
    ? { status: 200, body: { data: SURI_POLICY } }
    : { status: 200, body: { data: { ...SURI_POLICY, ...(record.json || {}) } } },
  '/api/v1/suricata/ips/status': { status: 200, body: { data: SURI_IPS } },
  '/api/v1/suricata/ips/drop': record => record.method === 'GET'
    ? { status: 200, body: { data: { sids: [2034647] } } }
    : { status: 200, body: { data: { status: 'started', op: record.method === 'DELETE' ? 'drop.remove' : 'drop.add' } } },
  '/api/v1/suricata/rules/update': { status: 200, body: { data: { status: 'started' } } },
  '/api/v1/suricata/ips/enable': { status: 200, body: { data: { status: 'started', op: 'enable' } } },
  '/api/v1/suricata/ips/disable': { status: 200, body: { data: { status: 'started', op: 'disable' } } }
};

async function boot(page, opts) {
  await page.setViewport({ width: 1280, height: 800 });
  await page.evaluate(o => {
    window._DEBUG = true;
    window.authToken = 'test-token';
    window.currentTab = 'mon-security';
    window._L = (ko, en) => en;
    window.t = key => key;
                                                                         
                                                                 
    window.pcvRoleAllows = role => o && o.role === 'viewer' ? false : true;
    window.__confirmCalls = [];
    window.customConfirm = (...args) => {
      window.__confirmCalls.push(args);
      return Promise.resolve(true);
    };
                                                                  
                                                             
                                                            
    window.fetchPost = (url, body) => fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body)
    }).then(r => r.json());
    window.fetchGet = url => fetch(url).then(r => r.json());
    window.fetchPut = (url, body) => fetch(url, {
      method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body)
    }).then(r => r.json());
    window.fetchDelete = (url, body) => fetch(url, {
      method: 'DELETE', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body)
    }).then(r => r.json());
    if (o && o.search) {
      history.replaceState(null, '', o.search);
      PCV.ui.filterState.readFromUrl();                                         
    }
    return PCV.security.render(document.getElementById('cb'));
  }, opts || null);
}

test('severity/status pills follow approved tone map (INFO neutral), no legacy badge in event table', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    assert.ok(await page.$('#cb .sec-event-row'), 'event rows must render (smoke)');
    const cells = await page.$$eval('#cb .sec-event-row', rows => rows.map(r => {
      const tds = r.querySelectorAll('td');
      return {
        sev: tds[1].querySelector('.pill') && tds[1].querySelector('.pill').className,
        sevText: tds[1].textContent.trim(),
        st: tds[5].querySelector('.pill') && tds[5].querySelector('.pill').className
      };
    }));
    assert.match(String(cells[0].sev), /pill-crit/);
    assert.equal(cells[0].sevText, 'CRIT');
    assert.match(String(cells[1].sev), /pill-warn/);
    assert.match(String(cells[2].sev), /pill-idle/);                            
    assert.match(String(cells[0].st), /pill-warn/);              
    assert.match(String(cells[2].st), /pill-ok/);                    
    assert.match(String(cells[3].st), /pill-idle/);                    
    assert.equal(await page.$$eval('#cb .sec-event-row .badge', els => els.length), 0);
                                                                          
    assert.ok(await page.$('#cb .chip[data-facet="secsev"][data-val="info"] .sdot-idle'));
  }, { routes: ROUTES });
});

test('KPI dots, pending-action pills, legend pills render; boot issues exactly 3 rpc calls', async () => {
  await withPage(MODS, async (page, { requests }) => {
    await boot(page);
    const dots = await page.$$eval('#cb .sg .sdot-ok, #cb .sg .sdot-crit, #cb .sg .sdot-warn', els => els.length);
    assert.ok(dots >= 2, 'Guard/Baseline statusDot expected, got ' + dots);
    const pendingPills = await page.$$eval('#cb [data-sec-action-event-id] .pill', els => els.map(e => e.className));
    assert.equal(pendingPills.length, 2);
    assert.match(pendingPills[0], /pill-crit/);                              
    assert.match(pendingPills[1], /pill-idle/);                       
                                                            
                                         
    assert.equal(await page.$$eval('#cb .badge', els => els.length), 0);
    assert.equal(requests.filter(r => r.path === '/api/v1/rpc').length, 3);
  }, { routes: ROUTES });
});

test('Suricata runtime uses four dedicated REST reads and renders admin controls', async () => {
  await withPage(MODS, async (page, { requests }) => {
    await boot(page);
    assert.equal(await page.$eval('#suricata-runtime', el => el.textContent.includes('Suricata Runtime')), true);
    assert.match(await page.$eval('#suricata-runtime', el => el.textContent), /ACTIVE/);
    assert.match(await page.$eval('#suricata-runtime', el => el.textContent), /67,?856|67856/);
    assert.ok(await page.$('#suri-policy-mode'));
    assert.ok(await page.$('[data-suri-ips="disable"]'));
    assert.ok(await page.$('[data-suri-drop-remove="2034647"]'));
    const reads = requests.filter(r => r.method === 'GET' && r.path.startsWith('/api/v1/suricata/'));
    assert.deepEqual(reads.map(r => r.path).sort(), [
      '/api/v1/suricata/ips/drop', '/api/v1/suricata/ips/status',
      '/api/v1/suricata/policy', '/api/v1/suricata/status'
    ]);
  }, { routes: ROUTES });
});

test('480px keeps document width fixed and confines event table overflow', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    await page.setViewport({ width: 480, height: 900 });
    const dimensions = await page.evaluate(() => {
      const table = document.querySelector('[data-sec-table-scroll] table');
      return {
        viewport: document.documentElement.clientWidth,
        document: document.documentElement.scrollWidth,
        tableClient: table && table.clientWidth,
        tableScroll: table && table.scrollWidth
      };
    });
    assert.equal(dimensions.document, dimensions.viewport);
    assert.ok(dimensions.tableScroll > dimensions.tableClient,
      `table must own horizontal overflow: ${JSON.stringify(dimensions)}`);
  }, { routes: ROUTES });
});

test('non-admin sees explicit restricted state and sends no Suricata request', async () => {
  await withPage(MODS, async (page, { requests }) => {
    await boot(page, { role: 'viewer' });
    assert.match(await page.$eval('#suricata-runtime', el => el.textContent), /ADMIN ONLY/);
    assert.equal(await page.$('#suricata-runtime [data-suri-action]'), null);
    assert.equal(requests.filter(r => r.path.startsWith('/api/v1/suricata/')).length, 0);
  }, { routes: ROUTES });
});

test('degraded IPS is text-visible and not conveyed by color alone', async () => {
  const routes = {
    ...ROUTES,
    '/api/v1/suricata/ips/status': { status: 200, body: { data: {
      ...SURI_IPS, degraded: true, enabled: false,
      engine: { state: 'failed', binary_present: true },
      last_toggle: { running: false, op: 'enable', result: 'fail', error: 'readiness timeout' }
    } } }
  };
  await withPage(MODS, async page => {
    await boot(page);
    const text = await page.$eval('#suricata-runtime', el => el.textContent);
    assert.match(text, /DEGRADED/);
    assert.match(text, /readiness timeout/);
  }, { routes });
});

test('policy mutation confirms, PUTs dedicated REST, then re-fetches server state', async () => {
  await withPage(MODS, async (page, { requests }) => {
    await boot(page);
    await page.select('#suri-policy-mode', 'enforce');
    const putDone = page.waitForResponse(r =>
      new URL(r.url()).pathname === '/api/v1/suricata/policy' && r.request().method() === 'PUT');
    await page.click('[data-suri-policy-save]');
    await putDone;
    await page.waitForFunction(() => {
      const control = document.querySelector('[data-suri-policy-save]');
      return control && !control.disabled;
    });
    const writes = requests.filter(r => r.path === '/api/v1/suricata/policy' && r.method === 'PUT');
    assert.equal(writes.length, 1);
    assert.deepEqual(writes[0].json, { auto_isolate: 'enforce' });
    const confirms = await page.evaluate(() => window.__confirmCalls);
    assert.equal(confirms.length, 1);
    assert.match(confirms[0][0], /Confirm Suricata change/);
    assert.match(confirms[0][1], /enforce/i);
    assert.ok(requests.filter(r => r.path === '/api/v1/suricata/policy' && r.method === 'GET').length >= 2);
  }, { routes: ROUTES });
});

test('event detail: confidence inline gauge + recommendation pill, no legacy badge', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    await page.evaluate(() => document.querySelector('#cb .sec-event-row').click());
    await page.waitForSelector('#security-detail .gauge-inline', { timeout: 5000 });
    const detail = await page.$eval('#security-detail', d => ({
      badges: d.querySelectorAll('.badge').length,
      pills: [...d.querySelectorAll('.pill')].map(p => p.className)
    }));
    assert.equal(detail.badges, 0);
    assert.ok(detail.pills.some(c => /pill-crit/.test(c)), 'recommendation pill (block_ip=crit) expected');
  }, { routes: ROUTES });
});

test('filterBar chips drive DOM-local filtering without rerender or refetch', async () => {
  await withPage(MODS, async (page, { requests }) => {
    await boot(page);
    const chips = await page.$$eval('#cb .filterbar .chip', els =>
      els.map(c => c.getAttribute('data-facet') + ':' + c.getAttribute('data-val')));
    assert.deepEqual(chips.filter(c => c.startsWith('secsev:')), ['secsev:crit', 'secsev:warn', 'secsev:info']);
    assert.ok(chips.includes('secsrc:suricata') && chips.includes('secstatus:open'));
    const bootRpc = requests.filter(r => r.path === '/api/v1/rpc').length;            

                                                  
    await page.evaluate(() => {
      document.querySelector('#cb .sec-event-row').setAttribute('data-sentinel', 'keep');
      document.querySelector('#cb .chip[data-facet="secsev"][data-val="crit"]').click();
    });
    const vis = await page.$$eval('#cb .sec-event-row', rows =>
      rows.map(r => ({ id: r.getAttribute('data-sec-event-id'), shown: r.style.display !== 'none' })));
    assert.deepEqual(vis.filter(v => v.shown).map(v => v.id), ['ev-1']);
    assert.equal(await page.$eval('#cb .sec-event-row', r => r.getAttribute('data-sentinel')), 'keep');
    assert.match(await page.evaluate(() => location.search), /secsev=crit/);

    await page.evaluate(() => document.querySelector('#cb .chip[data-facet="secsev"][data-val="crit"]').click());
    assert.equal(await page.$$eval('#cb .sec-event-row', rows =>
      rows.filter(r => r.style.display !== 'none').length), 4);
                                                                        
    assert.equal(requests.filter(r => r.path === '/api/v1/rpc').length, bootRpc);
  }, { routes: ROUTES });
});

test('search input combines with chips; zero-match shows the empty filter row', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    await page.evaluate(() => {
      document.querySelector('#cb .chip[data-facet="secsrc"][data-val="suricata"]').click();
      const q = document.getElementById('sec-filter-q');
      q.value = 'integrity';
      q.dispatchEvent(new Event('input', { bubbles: true }));
    });
    assert.equal(await page.$$eval('#cb .sec-event-row', rows =>
      rows.filter(r => r.style.display !== 'none').length), 0);
    assert.equal(await page.$eval('#sec-filter-empty', r => r.style.display), '');
  }, { routes: ROUTES });
});

test('cold deep-link: pre-read filterState filters first paint; tab exit self-unsubscribes', async () => {
  await withPage(MODS, async page => {
    await boot(page, { search: '?secstatus=open' });
    assert.deepEqual(await page.$$eval('#cb .sec-event-row', rows =>
      rows.filter(r => r.style.display !== 'none').map(r => r.getAttribute('data-sec-event-id'))), ['ev-1']);
                                           
    await page.evaluate(() => { window.currentTab = 'vm'; PCV.ui.filterState.apply({ secstatus: [] }); });
    assert.deepEqual(await page.$$eval('#cb .sec-event-row', rows =>
      rows.filter(r => r.style.display !== 'none').map(r => r.getAttribute('data-sec-event-id'))), ['ev-1']);
  }, { routes: ROUTES });
});

                                                           
                                                      
test('refresh preserves search query and re-applies it (asymmetric reset regression)', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    await page.evaluate(() => {
      var q = document.getElementById('sec-filter-q');
      q.value = 'passwd';
      q.dispatchEvent(new Event('input', { bubbles: true }));
    });
    assert.deepEqual(await page.$$eval('#cb .sec-event-row', rows =>
      rows.filter(r => r.style.display !== 'none').map(r => r.getAttribute('data-sec-event-id'))), ['ev-2']);
                                                                     
                                                                  
    await page.evaluate(() => PCV.security.render(document.getElementById('cb')));
    assert.equal(await page.$eval('#sec-filter-q', el => el.value), 'passwd');
    assert.deepEqual(await page.$$eval('#cb .sec-event-row', rows =>
      rows.filter(r => r.style.display !== 'none').map(r => r.getAttribute('data-sec-event-id'))), ['ev-2']);
  }, { routes: ROUTES });
});

                                                                              
                                                           
                                                  
                                                     
test('orphan non-canonical URL value vanishes from DOM on click (DOM-toggle screen, no rerender)', async () => {
  await withPage(MODS, async page => {
    await boot(page, { search: '?secsev=bogus' });
    assert.equal(await page.$eval('#cb .chip[data-facet="secsev"][data-val="bogus"]',
      el => el.getAttribute('aria-pressed')), 'true');
    assert.equal(await page.$eval('#cb .chip[data-facet="secsev"][data-val="bogus"] .chip-c',
      el => el.textContent), '0');
    assert.equal(await page.$$eval('#cb .sec-event-row', rows =>
      rows.filter(r => r.style.display !== 'none').length), 0);
    await page.evaluate(() =>
      document.querySelector('#cb .chip[data-facet="secsev"][data-val="bogus"]').click());
                                                         
    assert.equal(await page.$('#cb .chip[data-facet="secsev"][data-val="bogus"]'), null);
    assert.doesNotMatch(await page.evaluate(() => location.search), /secsev/);
    assert.equal(await page.$$eval('#cb .sec-event-row', rows =>
      rows.filter(r => r.style.display !== 'none').length), 4);
  }, { routes: ROUTES });
});
