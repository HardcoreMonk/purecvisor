import fs from 'node:fs';
import { fileURLToPath } from 'node:url';
import { withPage } from './harness.mjs';

const MODS = [
  'ui/modules/ui.js',
  'ui/modules/uxlib.js',
  'ui/modules/filter-state.js',
  'ui/modules/endpoints.js',
  'ui/modules/monitor.js',
  'ui/modules/shell.js'
];

async function bootGlobals(page) {
  await page.evaluate(() => {
    window.currentTab = 'mon-alerts';
    window.authToken = 'test-token';
    window.currentUser = { role: 'ADMIN' };
    window._DEBUG = false;
    window.addEvt = () => {};
    PCV.api = PCV.api || {};
    PCV.api.RPC_ERROR = { CONFLICT: -32002 };
    window.t = key => key;
    window._L = ko => ko;
    window.toast = () => {};
    window.unwrapData = value => value && value.data !== undefined ? value.data : value;
    window.unwrapList = value => {
      const data = window.unwrapData(value);
      return Array.isArray(data) ? data : [];
    };
    window.fetchGet = url => fetch(url).then(response => response.json());
    window.fetchPut = (url, body) => fetch(url, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body)
    }).then(response => response.json());
  });
}

async function waitForPut(ctx, count) {
  const deadline = Date.now() + 5000;
  while (Date.now() < deadline) {
    const puts = ctx.requests.filter(request => request.method === 'PUT');
    if (puts.length >= count) return puts[count - 1].json;
    await new Promise(resolve => setTimeout(resolve, 20));
  }
  throw new Error('PUT /alerts/config를 5초 안에 보내지 않았다');
}

export async function captureAlertConfigBodies(config, routing) {
  let bodies = null;
  await withPage(MODS, async (page, ctx) => {
    await bootGlobals(page);

    await page.evaluate(() => PCV.monitor.renderAlertRouting(document.getElementById('cb')));
    await page.waitForSelector('#route-warn-url');
    await page.type('#route-warn-url', routing.warn);
    await page.type('#route-crit-url', routing.crit);
    await page.type('#route-secret', routing.secret);
    await page.evaluate(() => window.saveAlertRouting());
    const routingBody = await waitForPut(ctx, 1);

    await page.evaluate(() => PCV.monitor.renderAlerts(document.getElementById('cb')));
    await page.waitForSelector('#alert-config-region:not([aria-busy="true"]) #al-disk_warn');

    await page.$eval('#al-disk_warn', node => {
      node.value = String(node.valueAsNumber - 1);
      node.dispatchEvent(new Event('input', { bubbles: true }));
      node.dispatchEvent(new Event('change', { bubbles: true }));
    });
    await page.waitForFunction(() => !document.querySelector('[data-alert-apply]').disabled);
    await page.click('[data-alert-apply]');
    const formBody = await waitForPut(ctx, 2);
    bodies = { routing: routingBody, form: formBody };
  }, {
    routes: {
      '/api/v1/alerts': { status: 200, body: [] },
      '/api/v1/alerts/config': request => request.method === 'PUT'
        ? { status: 200, body: { data: { ...config, ...request.json, config_revision: config.config_revision + 1 } } }
        : { status: 200, body: { data: config } }
    }
  });
  return bodies;
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const [configPath, warn, crit, secret] = process.argv.slice(2);
  const config = JSON.parse(fs.readFileSync(configPath, 'utf8'));
  process.stdout.write(JSON.stringify(await captureAlertConfigBodies(config, { warn, crit, secret })) + '\n');
}
