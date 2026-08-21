                                                                                            
                                                                                             
                                                                            
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

                                                                           
                                                                            
                                         
const MODS = [
  'ui/modules/ui.js',
  'ui/modules/uxlib.js',
  'ui/modules/endpoints.js',
  'ui/modules/filter-state.js',
  'ui/modules/modal-core.js',
  'ui/modules/network.js'
];

                                                  
                                                                         
const NETWORKS = [
  { name: 'pcvnat0', mode: 'nat', state: 'up', ip_cidr: '10.78.0.1/24', dhcp: true, subnet: '10.78.0.0/24' },
  { name: 'br-lan', mode: 'bridge', state: 'up', phys: 'enp5s0', uplink_mode: 'dedicated', ip_cidr: '', dhcp: false, subnet: '' },
  { name: 'iso-lab', mode: 'isolated', state: 'down', ip_cidr: '', dhcp: false, subnet: '' },
  { name: 'rt-edge', mode: 'routed', state: 'up', ip_cidr: '10.9.0.1/24', dhcp: false, subnet: '10.9.0.0/24' }
];

const ROUTES = { '/api/v1/networks': { status: 200, body: { data: NETWORKS } } };

                                                                      
                                                                                         
                                                            
async function boot(page, opts) {
  await page.setViewport({ width: 1280, height: 800 });
  await page.evaluate(o => {
    window._DEBUG = true;
    window.authToken = 'test-token';
    window.currentTab = 'networks';
    window._L = (ko, en) => en;
    window.t = key => key;
    window.unwrapData = value => value && value.data !== undefined ? value.data : value;
    window.unwrapList = value => {
      const data = window.unwrapData(value);
      return Array.isArray(data) ? data : (data?.items || data?.list || []);
    };
    window.fetchGet = url => fetch(url).then(async response => {
      const body = await response.json();
      if (!response.ok) throw new Error(body.error?.message || `HTTP ${response.status}`);
      return body;
    });
                                                             
                                                                   
    if (o && o.search) {
      history.replaceState(null, '', o.search);
      PCV.ui.filterState.readFromUrl();
    }
    return window.renderNetworks(document.getElementById('cb'));
  }, opts || null);
}

async function openNetEdit(page, current) {
  await boot(page);
  await page.evaluate(value => {
    window.__toasts = [];
    window.toast = (message, ok) => window.__toasts.push({ message, ok });
    window.addEvt = () => {};
    window.fetchPost = (url, body) => fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body)
    }).then(response => response.json());
    window.showNetEdit(value.name, value.mode, value.cidr, value.dhcp, value.phys);
  }, current);
  await page.waitForSelector('dialog[open] #ne-mode');
}

async function openNetCreate(page) {
  await boot(page);
  await page.evaluate(() => {
    window.__toasts = [];
    window.toast = (message, ok) => window.__toasts.push({ message, ok });
    window.addEvt = () => {};
    window.fetchPost = (url, body) => fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body)
    }).then(response => response.json());
    window.showNetCreate();
  });
  await page.waitForSelector('dialog[open] [data-net-create]');
}

function networkCreateRoute(postReply = { status: 200, body: { result: { status: 'created' } } }) {
  return request => request.method === 'POST'
    ? postReply
    : { status: 200, body: { data: NETWORKS } };
}

async function setInput(page, selector, value) {
  await page.$eval(selector, (input, nextValue) => {
    input.value = nextValue;
    input.dispatchEvent(new Event('input', { bubbles: true }));
  }, value);
}

test('bridge create hides CIDR, does not suggest the management NIC, and requires acknowledgement', async () => {
  await withPage(MODS, async (page, { requests }) => {
    await openNetCreate(page);
    await page.select('#nm', 'bridge');
    const initial = await page.evaluate(() => ({
      cidrHidden: document.getElementById('net-cidr-row').classList.contains('hidden'),
      cidrDisabled: document.getElementById('nc').disabled,
      nicTag: document.getElementById('np').tagName,
      warning: document.getElementById('net-bridge-help').textContent,
      buttonDisabled: document.querySelector('[data-net-create]').disabled,
      ackText: document.querySelector('#net-bridge-ack-row label').textContent
    }));
    assert.equal(initial.cidrHidden, true);
    assert.equal(initial.cidrDisabled, true);
    assert.equal(initial.nicTag, 'INPUT');
    assert.match(initial.warning, /management NIC|관리 NIC/i);
    assert.match(initial.warning, /no host IP|호스트 IP/i);
    assert.match(initial.ackText, /default route|기본 경로/i);
    assert.equal(initial.buttonDisabled, true);
    assert.equal(requests.some(request => request.path.includes('monitor')), false);

    await setInput(page, '#nn', 'pcvbr0');
    await setInput(page, '#np', 'enp42s0');
    assert.equal(await page.$eval('[data-net-create]', button => button.disabled), true);
    await page.evaluate(() => window.doNetCreate());
    assert.equal(requests.filter(request => request.method === 'POST').length, 0);

    await page.click('#net-bridge-ack');
    assert.equal(await page.$eval('[data-net-create]', button => button.disabled), false);
  }, { routes: { '/api/v1/networks': networkCreateRoute() } });
});

test('bridge create sends only the dedicated-uplink contract and closes on success', async () => {
  await withPage(MODS, async (page, { requests }) => {
    await openNetCreate(page);
    await page.select('#nm', 'bridge');
    await setInput(page, '#nn', 'pcvbr0');
    await setInput(page, '#np', 'enp42s0');
    await page.click('#net-bridge-ack');
    await page.click('[data-net-create]');
    await page.waitForFunction(() => !document.querySelector('dialog[open]'));
    const calls = requests.filter(request => request.method === 'POST');
    assert.equal(calls.length, 1);
    assert.deepEqual(calls[0].json, {
      bridge_name: 'pcvbr0',
      mode: 'bridge',
      physical_if: 'enp42s0',
      uplink_mode: 'dedicated',
      safety_ack: 'dedicated-uplink'
    });
  }, { routes: { '/api/v1/networks': networkCreateRoute() } });
});

test('shared bridge explains host preservation and sends the shared contract', async () => {
  await withPage(MODS, async (page, { requests }) => {
    await openNetCreate(page);
    await page.select('#nm', 'bridge');
    await page.click('#net-uplink-shared');
    const copy = await page.evaluate(() => ({
      help: document.getElementById('net-bridge-help').textContent,
      ack: document.getElementById('net-bridge-ack-text').textContent,
      hint: document.getElementById('net-mode-hint').textContent
    }));
    assert.match(copy.help, /Host IP|호스트 IP/i);
    assert.match(copy.help, /multiple MAC|여러 MAC/i);
    assert.match(copy.ack, /upstream switch|상위 스위치/i);
    assert.match(copy.hint, /TC-BPF/i);

    await setInput(page, '#nn', 'pcvbr0');
    await setInput(page, '#np', 'enp4s0');
    await page.click('#net-bridge-ack');
    await page.click('[data-net-create]');
    await page.waitForFunction(() => !document.querySelector('dialog[open]'));
    const call = requests.find(request => request.method === 'POST');
    assert.deepEqual(call.json, {
      bridge_name: 'pcvbr0',
      mode: 'bridge',
      physical_if: 'enp4s0',
      uplink_mode: 'shared',
      safety_ack: 'shared-uplink'
    });
  }, { routes: { '/api/v1/networks': networkCreateRoute() } });
});

test('bridge create preserves the confirmed form after a server safety rejection', async () => {
  await withPage(MODS, async page => {
    await openNetCreate(page);
    await page.select('#nm', 'bridge');
    await setInput(page, '#nn', 'pcvbr0');
    await setInput(page, '#np', 'eno1');
    await page.click('#net-bridge-ack');
    await page.click('[data-net-create]');
    await page.waitForFunction(() => !document.getElementById('net-create-error').classList.contains('hidden'));
    const state = await page.evaluate(() => ({
      dialogOpen: !!document.querySelector('dialog[open]'),
      nic: document.getElementById('np').value,
      acknowledged: document.getElementById('net-bridge-ack').checked,
      disabled: document.querySelector('[data-net-create]').disabled,
      busy: document.querySelector('[data-net-create]').getAttribute('aria-busy'),
      inlineError: document.getElementById('net-create-error').textContent,
      errorRole: document.getElementById('net-create-error').getAttribute('role')
    }));
    assert.equal(state.dialogOpen, true);
    assert.equal(state.nic, 'eno1');
    assert.equal(state.acknowledged, true);
    assert.equal(state.disabled, false);
    assert.equal(state.busy, null);
    assert.match(state.inlineError, /default route/i);
    assert.equal(state.errorRole, 'alert');
  }, { routes: { '/api/v1/networks': networkCreateRoute({
    status: 400,
    body: { error: { message: 'interface eno1 owns the IPv4 default route' } }
  }) } });
});

test('bridge create keeps its loading label readable while blocking duplicate submits', async () => {
  await withPage(MODS, async page => {
    await openNetCreate(page);
    await page.evaluate(() => {
      window.__createCalls = 0;
      window.fetchPost = () => {
        window.__createCalls += 1;
        return new Promise(() => {});
      };
    });
    await page.select('#nm', 'bridge');
    await setInput(page, '#nn', 'pcvbr0');
    await setInput(page, '#np', 'enp42s0');
    await page.click('#net-bridge-ack');
    await page.click('[data-net-create]');
    await page.waitForSelector('[data-net-create][aria-busy="true"]');
    const state = await page.$eval('[data-net-create]', button => ({
      disabled: button.disabled,
      ariaDisabled: button.getAttribute('aria-disabled'),
      label: button.textContent,
      opacity: getComputedStyle(button).opacity
    }));
    assert.equal(state.disabled, false);
    assert.equal(state.ariaDisabled, 'true');
    assert.match(state.label, /Creating|생성 중/i);
    assert.equal(state.opacity, '1');
    await page.evaluate(() => window.doNetCreate());
    assert.equal(await page.evaluate(() => window.__createCalls), 1);
  }, { routes: ROUTES });
});

test('NAT create retains the CIDR payload without a bridge acknowledgement', async () => {
  await withPage(MODS, async (page, { requests }) => {
    await openNetCreate(page);
    await setInput(page, '#nn', 'pcvnat1');
    await setInput(page, '#nc', '10.79.0.1/24');
    await page.click('[data-net-create]');
    await page.waitForFunction(() => !document.querySelector('dialog[open]'));
    const call = requests.find(request => request.method === 'POST');
    assert.deepEqual(call.json, {
      bridge_name: 'pcvnat1',
      mode: 'nat',
      cidr: '10.79.0.1/24'
    });
  }, { routes: { '/api/v1/networks': networkCreateRoute() } });
});

test('bridge create remains readable with touch-sized actions at 480px', async () => {
  await withPage(MODS, async page => {
    await openNetCreate(page);
    await page.setViewport({ width: 480, height: 900 });
    await page.select('#nm', 'bridge');
    await setInput(page, '#nn', 'pcvbr0');
    await setInput(page, '#np', 'enp42s0');
    const layout = await page.evaluate(() => {
      const dialog = document.querySelector('dialog[open]');
      const actions = [...dialog.querySelectorAll('.net-mode-actions button')]
        .map(button => button.getBoundingClientRect().height);
      const ack = document.getElementById('net-bridge-ack-row').getBoundingClientRect();
      const box = dialog.getBoundingClientRect();
      return {
        overflow: dialog.scrollWidth - dialog.clientWidth,
        actions,
        ackInside: ack.left >= box.left && ack.right <= box.right
      };
    });
    assert.ok(layout.overflow <= 1);
    assert.ok(layout.actions.every(height => height >= 40));
    assert.equal(layout.ackInside, true);
  }, { routes: { '/api/v1/networks': networkCreateRoute() } });
});

test('mode editor exposes only backend-supported new modes and separates current bridge state', async () => {
  await withPage(MODS, async page => {
    await openNetEdit(page, {
      name: 'br-lan', mode: 'bridge', cidr: '192.0.2.73/24',
      dhcp: false, phys: 'enp5s0'
    });
    const state = await page.evaluate(() => ({
      options: [...document.querySelectorAll('#ne-mode option')].map(option => ({
        value: option.value,
        disabled: option.disabled,
        selected: option.selected
      })),
      currentMode: document.querySelector('[data-net-current-mode]')?.textContent,
      applyDisabled: document.querySelector('[data-net-edit-apply]')?.disabled,
      modeSelectDisabled: document.getElementById('ne-mode')?.disabled,
      cidrDisabled: document.getElementById('ne-cidr')?.disabled,
      hasDhcpControl: !!document.getElementById('ne-dhcp'),
      hasPhysicalControl: !!document.getElementById('ne-phys'),
      note: document.querySelector('[data-net-mode-contract-note]')?.textContent || ''
    }));
    assert.deepEqual(state.options.map(option => option.value), ['', 'nat', 'isolated', 'routed']);
    assert.equal(state.options[0].disabled, true);
    assert.equal(state.options[0].selected, true);
    assert.equal(state.currentMode, 'BRIDGE');
    assert.equal(state.applyDisabled, true);
    assert.equal(state.modeSelectDisabled, true);
    assert.equal(state.cidrDisabled, true);
    assert.equal(state.hasDhcpControl, false);
    assert.equal(state.hasPhysicalControl, false);
    assert.match(state.note, /dedicated physical bridge|전용 물리 브리지/i);

    await page.select('#ne-mode', 'nat');
    assert.equal(await page.$eval('[data-net-edit-apply]', button => button.disabled), true);
  }, { routes: ROUTES });
});

test('mode editor disables and rejects no-op submit, then enables a valid dirty form', async () => {
  await withPage(MODS, async (page, { requests }) => {
    await openNetEdit(page, {
      name: 'pcvnat0', mode: 'nat', cidr: '10.78.0.1/24',
      dhcp: true, phys: ''
    });
    assert.equal(await page.$eval('[data-net-edit-apply]', button => button.disabled), true);
    await page.evaluate(() => window.doNetEdit('pcvnat0'));
    assert.equal(requests.filter(request => request.method === 'POST').length, 0);
    assert.ok(await page.evaluate(() => window.__toasts.some(item => /No changes|변경된 값/.test(item.message))));

    await page.$eval('#ne-cidr', input => {
      input.value = '10.79.0.1/24';
      input.dispatchEvent(new Event('input', { bubbles: true }));
    });
    assert.equal(await page.$eval('[data-net-edit-apply]', button => button.disabled), false);

    await page.$eval('#ne-cidr', input => {
      input.value = '10.78.0.1/24';
      input.dispatchEvent(new Event('input', { bubbles: true }));
    });
    assert.equal(await page.$eval('[data-net-edit-apply]', button => button.disabled), true);
  }, { routes: ROUTES });
});

test('mode editor explains routed impact with text and keeps a single primary action', async () => {
  await withPage(MODS, async page => {
    await openNetEdit(page, {
      name: 'pcvnat0', mode: 'nat', cidr: '10.78.0.1/24',
      dhcp: true, phys: ''
    });
    await page.select('#ne-mode', 'routed');
    const state = await page.evaluate(() => ({
      impact: document.querySelector('[data-net-mode-impact]')?.textContent || '',
      impactClass: document.querySelector('[data-net-mode-impact]')?.className || '',
      live: document.querySelector('[data-net-mode-impact]')?.getAttribute('aria-live'),
      modeDescribedBy: document.getElementById('ne-mode')?.getAttribute('aria-describedby'),
      cidrDescribedBy: document.getElementById('ne-cidr')?.getAttribute('aria-describedby'),
      dialogLabelledBy: document.querySelector('dialog[open]')?.getAttribute('aria-labelledby'),
      titleId: document.querySelector('dialog[open] h2')?.id,
      focusOrder: [...document.querySelectorAll('dialog[open] select, dialog[open] input:not(:disabled), dialog[open] button:not(:disabled)')]
        .map(element => element.id || element.textContent),
      actions: [...document.querySelectorAll('.net-mode-actions button')].map(button => ({
        text: button.textContent,
        className: button.className
      }))
    }));
    assert.match(state.impact, /DHCP/i);
    assert.match(state.impact, /static routing|정적 라우팅/i);
    assert.match(state.impactClass, /net-mode-impact-warn/);
    assert.equal(state.live, 'polite');
    assert.equal(state.modeDescribedBy, 'ne-impact');
    assert.equal(state.cidrDescribedBy, 'ne-cidr-help');
    assert.equal(state.dialogLabelledBy, state.titleId);
    assert.deepEqual(state.focusOrder, ['ne-mode', 'ne-cidr', 'btn.cancel', 'btn.apply']);
    assert.deepEqual(state.actions.map(action => action.text), ['btn.cancel', 'btn.apply']);
    assert.match(state.actions[0].className, /btn-soft/);
    assert.match(state.actions[1].className, /btn-primary/);
  }, { routes: ROUTES });
});

test('mode editor preserves key-value rows and touch targets at 480px', async () => {
  await withPage(MODS, async page => {
    await openNetEdit(page, {
      name: 'pcvnat0', mode: 'nat', cidr: '10.78.0.1/24',
      dhcp: true, phys: ''
    });
    await page.setViewport({ width: 480, height: 900 });
    const layout = await page.evaluate(() => {
      const rows = [...document.querySelectorAll('.net-mode-readonly')].map(row => {
        const key = row.querySelector('.net-mode-key').getBoundingClientRect();
        const value = row.querySelector('.net-mode-value').getBoundingClientRect();
        return { keyRight: key.right, valueLeft: value.left, keyTop: key.top, valueTop: value.top };
      });
      const buttons = [...document.querySelectorAll('.net-mode-actions button')]
        .map(button => button.getBoundingClientRect().height);
      const dialog = document.querySelector('dialog[open]');
      return {
        rows,
        buttons,
        overflow: dialog.scrollWidth - dialog.clientWidth
      };
    });
    assert.ok(layout.rows.every(row => row.valueLeft >= row.keyRight));
    assert.ok(layout.rows.every(row => Math.abs(row.keyTop - row.valueTop) < 2));
    assert.ok(layout.buttons.every(height => height >= 40));
    assert.ok(layout.overflow <= 1);
  }, { routes: ROUTES });
});

test('mode editor sends only mode_set payload for a supported transition', async () => {
  await withPage(MODS, async (page, { requests }) => {
    await openNetEdit(page, {
      name: 'pcvnat0', mode: 'nat', cidr: '10.78.0.1/24',
      dhcp: true, phys: ''
    });
    await page.select('#ne-mode', 'isolated');
    await page.click('[data-net-edit-apply]');
    await page.waitForFunction(() => !document.querySelector('dialog[open]'));
    const calls = requests.filter(request =>
      request.method === 'POST' && request.path === '/api/v1/networks/pcvnat0/mode');
    assert.equal(calls.length, 1);
    assert.deepEqual(calls[0].json, {
      mode: 'isolated',
      cidr: '10.78.0.1/24'
    });
  }, { routes: {
    ...ROUTES,
    '/api/v1/networks/pcvnat0/mode': { status: 200, body: { result: true } }
  } });
});

test('mode editor blocks an unsupported mode even after DOM injection', async () => {
  await withPage(MODS, async (page, { requests }) => {
    await openNetEdit(page, {
      name: 'pcvnat0', mode: 'nat', cidr: '10.78.0.1/24',
      dhcp: true, phys: ''
    });
    await page.evaluate(() => {
      const select = document.getElementById('ne-mode');
      const injected = document.createElement('option');
      injected.value = 'bridge';
      injected.textContent = 'bridge';
      select.appendChild(injected);
      select.value = 'bridge';
      return window.doNetEdit('pcvnat0');
    });
    const calls = requests.filter(request =>
      request.method === 'POST' && request.path === '/api/v1/networks/pcvnat0/mode');
    assert.equal(calls.length, 0);
    const toastMessages = await page.evaluate(() => window.__toasts.map(item => item.message));
    assert.ok(toastMessages.some(message => /unsupported|지원하지/i.test(message)));
    assert.ok(await page.$('dialog[open]'));
  }, { routes: {
    ...ROUTES,
    '/api/v1/networks/pcvnat0/mode': { status: 200, body: { result: true } }
  } });
});

test('mode editor restores the dirty form after a failed request', async () => {
  await withPage(MODS, async page => {
    await openNetEdit(page, {
      name: 'pcvnat0', mode: 'nat', cidr: '10.78.0.1/24',
      dhcp: true, phys: ''
    });
    await page.select('#ne-mode', 'isolated');
    await page.click('[data-net-edit-apply]');
    await page.waitForFunction(() => window.__toasts.length > 0);
    const state = await page.evaluate(() => {
      const button = document.querySelector('[data-net-edit-apply]');
      return {
        dialogOpen: !!document.querySelector('dialog[open]'),
        disabled: button.disabled,
        busy: button.getAttribute('aria-busy'),
        label: button.textContent,
        toasts: window.__toasts.map(item => item.message)
      };
    });
    assert.equal(state.dialogOpen, true);
    assert.equal(state.disabled, false);
    assert.equal(state.busy, null);
    assert.equal(state.label, 'btn.apply');
    assert.ok(state.toasts.some(message => /failed/i.test(message)));
  }, { routes: {
    ...ROUTES,
    '/api/v1/networks/pcvnat0/mode': {
      status: 400,
      body: { error: { message: 'fixture failure' } }
    }
  } });
});

test('inventory renders statusPill for mode (idle) and state (ok/crit), no legacy badge', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    assert.ok(await page.$('#cb table.table-sticky'), 'inventory table must render (smoke)');
    const rows = await page.$$eval('#cb table.table-sticky tbody tr', trs => trs.map(tr => {
      const tds = tr.querySelectorAll('td');
      const modePill = tds[1].querySelector('.pill');
      const statePill = tds[2].querySelector('.pill');
      return {
        name: tds[0].textContent.trim(),
        modeClass: modePill && modePill.className,
        modeText: modePill && modePill.textContent,
        stateClass: statePill && statePill.className,
        stateText: statePill && statePill.textContent
      };
    }));
    assert.equal(rows.length, 4);
    for (const row of rows) assert.match(String(row.modeClass), /pill-idle/);
    assert.equal(rows[0].modeText, 'NAT');
    assert.equal(rows[2].modeText, 'ISOLATED');
    assert.match(String(rows[0].stateClass), /pill-ok/);
    assert.equal(rows[0].stateText, 'UP');
    assert.match(String(rows[2].stateClass), /pill-crit/);
    assert.equal(rows[2].stateText, 'DOWN');
    const badges = await page.$$eval('#cb table.table-sticky .badge', els => els.length);
    assert.equal(badges, 0);
  }, { routes: ROUTES });
});

test('destructive delete action is preserved per row', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    const delBtns = await page.$$eval('#cb table.table-sticky tbody .btn-r', els => els.length);
    assert.equal(delBtns, 4);
  }, { routes: ROUTES });
});

test('filterBar facets filter rows client-side without refetch', async () => {
  await withPage(MODS, async (page, { requests }) => {
    await boot(page);
    const chips = await page.$$eval('#cb .filterbar .chip', els =>
      els.map(el => `${el.getAttribute('data-facet')}:${el.getAttribute('data-val')}`));
    assert.deepEqual(chips, [
      'netmode:nat', 'netmode:bridge', 'netmode:isolated', 'netmode:routed',
      'netstate:up', 'netstate:down'
    ]);

    await page.click('#cb .chip[data-facet="netmode"][data-val="nat"]');
    let rows = await page.$$eval('#cb table.table-sticky tbody tr', trs => trs.map(tr => tr.querySelector('td b').textContent));
    assert.deepEqual(rows, ['pcvnat0']);
    assert.match(await page.evaluate(() => location.search), /netmode=nat/);

                                  
    await page.click('#cb .chip[data-facet="netmode"][data-val="nat"]');
    assert.equal(await page.$$eval('#cb table.table-sticky tbody tr', trs => trs.length), 4);

    await page.click('#cb .chip[data-facet="netstate"][data-val="down"]');
    rows = await page.$$eval('#cb table.table-sticky tbody tr', trs => trs.map(tr => tr.querySelector('td b').textContent));
    assert.deepEqual(rows, ['iso-lab']);

                                                                  
                                                                      
    await page.click('#cb .chip[data-facet="netmode"][data-val="nat"]');
    assert.equal(await page.$$eval('#cb table.table-sticky tbody tr', trs => trs.length), 0);
    assert.equal(await page.$$eval('#cb table.table-sticky thead', els => els.length), 1);

                                                          
    assert.equal(requests.filter(r => r.path === '/api/v1/networks').length, 1);
  }, { routes: ROUTES });
});

test('non-canonical backend mode gets a derived facet chip and stays reachable', async () => {
                                                                
  const NETS = NETWORKS.concat([{ name: 'docker0', mode: 'unknown', state: 'up', ip_cidr: '172.17.0.1/16', dhcp: false, subnet: '172.17.0.0/16' }]);
  await withPage(MODS, async page => {
    await boot(page);
    const modeChips = await page.$$eval('#cb .filterbar .chip[data-facet="netmode"]', els =>
      els.map(el => el.getAttribute('data-val')));
    assert.deepEqual(modeChips, ['nat', 'bridge', 'isolated', 'routed', 'unknown']);
    await page.click('#cb .chip[data-facet="netmode"][data-val="unknown"]');
    const rows = await page.$$eval('#cb table.table-sticky tbody tr', trs => trs.map(tr => tr.querySelector('td b').textContent));
    assert.deepEqual(rows, ['docker0']);
  }, { routes: { '/api/v1/networks': { status: 200, body: { data: NETS } } } });
});

test('filter rerender preserves firewall panel runtime state', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    await page.evaluate(() => {
      document.getElementById('fw-rules-list').textContent = 'sentinel-rules';
      document.getElementById('fw-port').value = '8080';
    });
    await page.click('#cb .chip[data-facet="netstate"][data-val="up"]');
    assert.equal(await page.$eval('#fw-rules-list', el => el.textContent), 'sentinel-rules');
    assert.equal(await page.$eval('#fw-port', el => el.value), '8080');
  }, { routes: ROUTES });
});

test('chip toggle retains focus on the equivalent rerendered chip', async () => {
  await withPage(MODS, async page => {
    await boot(page);
                                                        
                                                       
    await page.click('#cb .chip[data-facet="netstate"][data-val="down"]');
    const active = await page.evaluate(() => {
      const el = document.activeElement;
      return el ? `${el.getAttribute('data-facet')}:${el.getAttribute('data-val')}` : null;
    });
    assert.equal(active, 'netstate:down');
    assert.equal(await page.$$eval('#cb table.table-sticky tbody tr', trs => trs.length), 1);
  }, { routes: ROUTES });
});

test('cold deep-link: pre-read URL query filters initial render and survives first chip apply', async () => {
  await withPage(MODS, async page => {
    await boot(page, { search: '?netmode=nat' });
    let rows = await page.$$eval('#net-inv tbody tr td b', bs => bs.map(b => b.textContent));
    assert.deepEqual(rows, ['pcvnat0']);
    assert.equal(await page.$eval('#net-inv .chip[data-facet="netmode"][data-val="nat"]',
      el => el.getAttribute('aria-pressed')), 'true');
                                                                     
    await page.evaluate(() => document.querySelector('#net-inv .chip[data-facet="netmode"][data-val="bridge"]').click());
    const search = await page.evaluate(() => location.search);
    assert.match(search, /nat/);
    assert.match(search, /bridge/);
  }, { routes: ROUTES });
});

test('tab exit self-unsubscribes: later filter applies never rerender detached networks view', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    await page.evaluate(() => {
      document.querySelector('#net-inv table.table-sticky').setAttribute('data-sentinel', 'keep');
      window.currentTab = 'vm';
      PCV.ui.filterState.apply({ netmode: ['nat'] });                                             
    });
    assert.equal(await page.$eval('#net-inv table.table-sticky',
      el => el.getAttribute('data-sentinel')), 'keep');
    await page.evaluate(() => { window.currentTab = 'networks'; PCV.ui.filterState.apply({ netmode: [] }); });
                                                                   
    assert.equal(await page.$eval('#net-inv table.table-sticky',
      el => el.getAttribute('data-sentinel')), 'keep');
  }, { routes: ROUTES });
});

test('orphan non-canonical URL value renders a removable chip and clears on click', async () => {
  await withPage(MODS, async page => {
    await boot(page, { search: '?netmode=bogus' });
                                                      
    const chip = await page.$('#net-inv .chip[data-facet="netmode"][data-val="bogus"]');
    assert.ok(chip, 'orphan chip must render');
    assert.equal(await page.$eval('#net-inv .chip[data-facet="netmode"][data-val="bogus"]',
      el => el.getAttribute('aria-pressed')), 'true');
    assert.equal(await page.$$eval('#net-inv tbody tr', trs => trs.length), 0);
                                      
    await page.evaluate(() =>
      document.querySelector('#net-inv .chip[data-facet="netmode"][data-val="bogus"]').click());
                                                          
                                                      
    assert.equal(await page.$('#net-inv .chip[data-facet="netmode"][data-val="bogus"]'), null);
    assert.doesNotMatch(await page.evaluate(() => location.search), /netmode/);
    assert.equal(await page.$$eval('#net-inv tbody tr', trs => trs.length), 4);
  }, { routes: ROUTES });
});
