import fs from 'node:fs';
import { fileURLToPath } from 'node:url';
import { withPage } from './harness.mjs';

export const APP_MODS = [
  'ui/i18n.js', 'ui/modules/endpoints.js', 'ui/modules/api.js', 'ui/modules/ui.js',
  'ui/modules/filter-state.js', 'ui/modules/uxlib.js', 'ui/modules/modal-core.js', 'ui/modules/modal.js',
  'ui/modules/charts.js', 'ui/modules/metrics.js', 'ui/modules/security.js', 'ui/modules/monitor.js',
  'ui/modules/vm.js', 'ui/modules/vm-console.js', 'ui/modules/vm-lifecycle.js', 'ui/modules/vm-guest.js',
  'ui/modules/container.js', 'ui/modules/network.js', 'ui/modules/storage.js', 'ui/modules/host-devices.js',
  'ui/modules/help.js',
  'ui/modules/nav.js', 'ui/modules/theme.js', 'ui/modules/accounts.js', 'ui/modules/advanced.js',
  'ui/modules/selfhealing.js', 'ui/modules/mobile.js', 'ui/modules/shell.js', 'ui/app.js'
];

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

export async function bootApp(page, port, role = 'ADMIN') {
  await page.setViewport({ width: 1280, height: 900 });
  await page.evaluate(async () => {
    const html = await fetch('/ui/index.html').then(response => response.text());
    document.body.innerHTML = new DOMParser().parseFromString(html, 'text/html').body.innerHTML;
    class FakeWS {
      constructor(url) {
        this.url = url; this.readyState = 0; this.sent = []; window.__fakeWs = this;
        setTimeout(() => { this.readyState = 1; if (this.onopen) this.onopen(); }, 0);
      }
      send(data) { this.sent.push(data); }
      close() { this.readyState = 3; if (this.onclose) this.onclose(); }
      emit(obj) { this.onmessage({ data: JSON.stringify(obj) }); }
    }
    FakeWS.CONNECTING = 0; FakeWS.OPEN = 1; FakeWS.CLOSING = 2; FakeWS.CLOSED = 3;
    window.WebSocket = FakeWS;
  });
  for (const moduleFile of APP_MODS) {
    await page.addScriptTag({ url: `http://127.0.0.1:${port}/${moduleFile}` });
  }
  await page.evaluate(r => {
    I18N.setLang('ko');
    window.authToken = 'test-token';
    window.currentUser = { role: r };
    window._DEBUG = false;
    window.__toasts = [];
    window.toast = (msg, ok) => { window.__toasts.push({ msg: String(msg), ok }); };
    window.confirm = () => true;
    window.alert = () => {};

    const hostNode = document.createElement('div');
    hostNode.id = 'drift-host';
    document.body.appendChild(hostNode);
  }, role);
}

async function waitFor(ctx, predicate, what) {
  const deadline = Date.now() + 5000;
  while (Date.now() < deadline) {
    const hit = ctx.requests.filter(predicate);
    if (hit.length) return hit[hit.length - 1];
    await sleep(20);
  }
  throw new Error(`${what}: 5초 안에 요청이 없다`);
}

const clickButton = (page, scope, onclick) => page.$eval(`${scope} button[onclick="${onclick}"]`, node => node.click());

async function setValue(page, selector, value) {
  await page.$eval(selector, (node, v) => {
    node.value = v;
    node.dispatchEvent(new Event('input', { bubbles: true }));
    node.dispatchEvent(new Event('change', { bubbles: true }));
  }, value);
}

export async function captureBodies(input) {
  const out = {};
  const routes = {
    '/api/v1/networks': { status: 200, body: { data: [{ name: input.ctr.bridge, mode: 'bridge' }] } },

    '/api/v1/containers': rec => rec.method === 'POST'
      ? { status: 400, body: { error: { code: -32602, message: 'recorded' } } }
      : { status: 200, body: { data: [{ name: input.ctr.name, state: 'STOPPED', image: 'x', privileged: false }] } },
    [`/api/v1/vms/${input.vm}/bandwidth`]: { status: 200, body: { data: {} } },
    [`/api/v1/vms/${input.vm}/cpu-pin`]: { status: 200, body: { data: {} } },
    [`/api/v1/vms/${input.vm}/guest-exec`]: { status: 200, body: { data: {} } },
    '/api/v1/agent/config': rec => rec.method === 'PUT'
      ? { status: 200, body: { data: input.agent.config } } : { status: 200, body: { data: input.agent.config } },
    [`/api/v1/containers/${input.ctr.name}/limits`]: { status: 200, body: { data: { applied: 'config' } } },
    '/api/v1/storage/pools': rec => rec.method === 'POST'
      ? { status: 200, body: { data: {} } } : { status: 200, body: { data: [] } },
    '/api/v1/storage/zvols': { status: 200, body: { data: [] } },
    '/api/v1/audit/search': { status: 200, body: { data: { items: [], total: 0, offset: 0, limit: 100 } } },
    '/api/v1/vms': { status: 200, body: { data: [{ name: input.vm, uuid: 'u-1', state: 'running' }] } }
  };
  await withPage([], async (page, ctx) => {
    if (input.timezone) await page.emulateTimezone(input.timezone);
    await bootApp(page, ctx.port);
    await page.evaluate(vm => {
      window.vmList = [{ name: vm, uuid: 'u-1', state: 'running' }];
      window.selectedVmIndex = 0;
    }, input.vm);

    await page.evaluate(() => window.showCtrCreate());
    await page.waitForSelector('#cc-name');
    await page.type('#cc-name', input.ctr.name);

    await page.waitForFunction(br => [...document.getElementById('cc-br').options].some(o => o.value === br), {}, input.ctr.bridge);
    await setValue(page, '#cc-br', input.ctr.bridge);
    await clickButton(page, 'dialog', 'doCtrCreate()');
    out.container_create = (await waitFor(ctx, r => r.method === 'POST' && r.path === '/api/v1/containers', 'POST /containers')).json;
    await page.waitForFunction(() => window.__toasts.length > 0);

    const cb = '#drift-host';
    await page.evaluate(() => {
      window.showSettings();
      const host = document.getElementById('drift-host');
      host.textContent = '';
      window.hwBandwidth().forEach(node => host.appendChild(node));
    });
    await setValue(page, '#sbw-in', '2048');
    await setValue(page, '#sbw-out', '1024');
    await clickButton(page, cb, 'doBandwidth()');
    const bwPath = `/api/v1/vms/${input.vm}/bandwidth`;
    out.bandwidth = (await waitFor(ctx, r => r.path === bwPath, 'PUT bandwidth')).json;
    const before = ctx.requests.filter(r => r.path === bwPath).length;
    await setValue(page, '#sbw-in', '');
    await setValue(page, '#sbw-out', '512');
    await clickButton(page, cb, 'doBandwidth()');
    const deadline = Date.now() + 5000;
    while (ctx.requests.filter(r => r.path === bwPath).length <= before && Date.now() < deadline) await sleep(20);
    out.bandwidth_outbound_only = ctx.requests.filter(r => r.path === bwPath).at(-1).json;

    await page.evaluate(() => {
      const host = document.getElementById('drift-host');
      host.textContent = '';
      window.hwCpuPin().forEach(node => host.appendChild(node));
    });
    await setValue(page, '#scpin-vcpu', '0');
    await setValue(page, '#scpin-cpuset', '0');
    await clickButton(page, cb, 'doCpuPin()');
    out.cpu_pin = (await waitFor(ctx, r => r.path === `/api/v1/vms/${input.vm}/cpu-pin`, 'PUT cpu-pin')).json;
    await page.evaluate(() => window.closeModal(true));

    const limitsPath = `/api/v1/containers/${input.ctr.name}/limits`;
    await page.evaluate(n => {
      window.selCtr = n;
      window.ctrTab = 'resources';
      return window.renderContainers(document.getElementById('drift-host'));
    }, input.ctr.name);
    await setValue(page, '#ctr-cpu-percent', '50');
    await setValue(page, '#ctr-cpu-weight', '200');
    await clickButton(page, cb, `ctrSetLimits('${input.ctr.name}','cpu')`);
    out.limits_cpu = (await waitFor(ctx, r => r.path === limitsPath, 'PUT limits(cpu)')).json;
    await setValue(page, '#ctr-mem-limit', '256');
    await clickButton(page, cb, `ctrSetLimits('${input.ctr.name}','mem')`);
    const limitsEnd = Date.now() + 5000;
    while (ctx.requests.filter(r => r.path === limitsPath).length < 2 && Date.now() < limitsEnd) await sleep(20);
    out.limits_mem = ctx.requests.filter(r => r.path === limitsPath).at(-1).json;

    const agentPuts = () => ctx.requests.filter(r => r.method === 'PUT' && r.path === '/api/v1/agent/config');
    for (const [key, value] of [['agent_enable', input.agent.key], ['agent_disable', '']]) {
      const n = agentPuts().length;
      await page.evaluate(() => window.showAgentConfig());
      await page.waitForSelector('#agk0');
      await setValue(page, '#agk0', value);
      await page.$eval('dialog button[onclick="saveAgentConfig()"]', node => node.click());
      const end = Date.now() + 5000;
      while (agentPuts().length <= n && Date.now() < end) await sleep(20);
      out[key] = agentPuts().at(-1).json;
      await page.waitForFunction(() => !document.getElementById('agk0'));
    }

    await page.evaluate(() => window.showGuestAgent());
    await page.waitForSelector('#ga-cmd');
    await page.type('#ga-cmd', 'cat /etc/hostname');
    await clickButton(page, 'dialog', 'gaExec()');
    out.guest_exec = (await waitFor(ctx, r => r.path === `/api/v1/vms/${input.vm}/guest-exec`, 'POST guest-exec')).json;
    await page.evaluate(() => { if (document.querySelector('dialog[open]') && window.closeModal) window.closeModal(); });
    await page.waitForFunction(() => !document.getElementById('ga-cmd'));

    const poolPosts = () => ctx.requests.filter(r => r.method === 'POST' && r.path === '/api/v1/storage/pools');
    for (const [key, name, vdev] of [['pool_mirror', input.pool.mirror_name, 'mirror'], ['pool_stripe', input.pool.stripe_name, 'stripe']]) {
      const n = poolPosts().length;
      await page.evaluate(() => window.showPoolCreate());
      await page.waitForSelector('#pc-name');
      await page.type('#pc-name', name);
      await page.type('#pc-disks', input.pool.disks);
      await setValue(page, '#pc-raid', vdev);
      await clickButton(page, 'dialog', 'doPoolCreate()');
      const end = Date.now() + 5000;
      while (poolPosts().length <= n && Date.now() < end) await sleep(20);
      out[key] = poolPosts().at(-1).json;
      await page.waitForFunction(() => !document.getElementById('pc-name'));
    }

    await page.evaluate(() => window.renderAudit(document.getElementById('drift-host')));
    await page.waitForSelector('#audit-user');
    await page.type('#audit-user', input.audit.username);
    await page.type('#audit-method', input.audit.method);
    await setValue(page, '#audit-from', input.audit.from);
    await setValue(page, '#audit-to', input.audit.to);
    await clickButton(page, cb, 'doAuditSearch()');
    out.audit_search = (await waitFor(ctx, r => r.path === '/api/v1/audit/search', 'GET /audit/search')).search;

    out.pong = await page.evaluate(async () => {
      window.connectWS();
      await new Promise(resolve => setTimeout(resolve, 20));
      window.__fakeWs.emit({ type: 'auth_ok' });
      const n = window.__fakeWs.sent.length;
      window.__fakeWs.emit({ type: 'ping', ts: 1 });
      return window.__fakeWs.sent.slice(n);
    });
  }, { routes });
  return out;
}

export async function captureScreens(input) {
  const out = {};
  const statusOf = key => (input.status && input.status[key]) || 200;
  const routes = {
    '/api/v1/rpc': rec => {
      const method = rec.json && rec.json.method;
      const body = (input.rpc || {})[method];
      return body === undefined
        ? { status: statusOf(method) === 200 ? 404 : statusOf(method), body: { error: { code: -32601, message: 'no fixture ' + method } } }
        : { status: statusOf(method), body };
    },
    '/api/v1/containers': { status: 200, body: input.containers || { data: [] } }
  };
  if (input.ctr) {
    routes[`/api/v1/containers/${input.ctr}/nics`] = { status: statusOf('nics'), body: input.nics || { data: [] } };
    routes[`/api/v1/containers/${input.ctr}/metrics`] = { status: 200, body: { data: {} } };
  }
  if (input.agent !== undefined) routes['/api/v1/agent/config'] = { status: 200, body: input.agent };
  if (input.audit !== undefined) routes['/api/v1/audit/search'] = { status: input.audit_status || 200, body: input.audit };
  await withPage([], async (page, ctx) => {
    await bootApp(page, ctx.port, input.role || 'ADMIN');
    const text = selector => page.$eval(selector, node => node.textContent.trim()).catch(() => null);

    if (input.rpc) {

      await page.evaluate(async () => { await window.renderSelfHealing(document.getElementById('drift-host')); await PCV.selfhealing.refresh(); });
      out.selfhealing_badge = await text('#selfhealing-panel .sh-badge');

      out.mobile_mode = await page.evaluate(() => {
        const st = PCV.selfhealing.state;
        return PCV.mobile.buildHealing({ mode: st.mode, pending: st.pending, history: st.history }).querySelector('.m-heal-mode').textContent;
      });

      await page.evaluate(async () => {
        const b = document.getElementById('drift-host');
        b.textContent = '';
        await window.renderMonOverview(b, [], [], 0, 0, 0, 0, 0);
      });

      await page.waitForFunction(() => document.querySelector('#selfhealing-panel .sh-badge')
        && document.getElementById('healing-pending-list')?.childElementCount > 0);
      out.overview_mode = await page.evaluate(() => {
        const row = [...document.querySelectorAll('#drift-host .hr')].find(r => r.querySelector('.k') && r.querySelector('.k').textContent === 'Mode');
        return row ? row.querySelector('.v').textContent : null;
      });
      out.overview_pending = await text('#healing-pending-list');
      out.overview_badge = await text('#selfhealing-panel .sh-badge');
    }

    if (input.ctr) {
      const rowValue = key => page.$$eval('#ctr-tab-content .hr', (rows, k) => {
        const r = rows.find(x => x.querySelector('.k') && x.querySelector('.k').textContent === k);
        return r ? r.querySelector('.v').textContent : null;
      }, key);
      const rowShown = key => page.waitForFunction(k => [...document.querySelectorAll('#ctr-tab-content .hr')]
        .some(r => r.querySelector('.k')?.textContent === k), {}, key);
      await page.evaluate(n => { window.selCtr = n; window.ctrTab = 'summary'; }, input.ctr);
      await page.evaluate(() => window.renderContainers(document.getElementById('drift-host')));
      await rowShown('Bridge');
      out.ctr_bridge = await rowValue('Bridge');
      out.ctr_apparmor = await rowValue('AppArmor');
      out.ctr_type = await rowValue('Type');
      await page.evaluate(() => { window.ctrTab = 'options'; });
      await page.evaluate(() => window.renderContainers(document.getElementById('drift-host')));
      await rowShown('Unprivileged');
      out.ctr_opt_apparmor = await rowValue('AppArmor');
      out.ctr_opt_unprivileged = await rowValue('Unprivileged');
      out.ctr_opt_text = await text('#ctr-tab-content');
    }

    if (input.agent !== undefined) {
      await page.evaluate(() => window.showAgentConfig());
      await page.waitForSelector('#agk0');
      out.agent_checkbox = !!(await page.$('#agen0'));
      out.agent_providers_text = await text('#agent-tab-body');
      await page.evaluate(() => window.setAgentTab('settings'));
      await page.waitForFunction(() => /Statistics/.test(document.getElementById('agent-tab-body')?.textContent || ''));
      out.agent_settings_text = await text('#agent-tab-body');
      out.agent_settings_inputs = await page.$$eval('#agent-tab-body input', nodes => nodes.length);
      out.agent_settings_save = !!(await page.$('#agent-tab-body button[onclick="saveAgentSettings()"]'));
      await page.evaluate(() => window.closeModal && window.closeModal());
    }

    if (input.audit !== undefined) {
      await page.evaluate(() => window.renderAudit(document.getElementById('drift-host')));
      await page.waitForSelector('#audit-user');
      await page.$eval('#drift-host button[onclick="doAuditSearch()"]', node => node.click());
      await page.waitForFunction(() => !document.querySelector('#audit-results .spinner')
        && !/검색 중/.test(document.getElementById('audit-results').textContent));
      out.audit_text = await text('#audit-results');
      out.audit_rows = await page.$$eval('#audit-results tbody tr', rows => rows.map(r => [...r.cells].map(c => c.textContent)));
      out.audit_error = await page.$eval('#audit-results', node => !!node.querySelector('.audit-search-error'));
    }
  }, { routes });
  return out;
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const [mode, inputPath] = process.argv.slice(2);
  const input = JSON.parse(fs.readFileSync(inputPath, 'utf8'));
  const result = mode === 'screens' ? await captureScreens(input) : await captureBodies(input);
  process.stdout.write(JSON.stringify(result) + '\n');
}
