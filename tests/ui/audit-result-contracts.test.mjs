


import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import { withPage } from './harness.mjs';

const MODS = ['endpoints', 'api', 'ui', 'filter-state', 'uxlib', 'modal-core',
  'network', 'storage', 'cloud', 'container', 'vm-guest', 'vm', 'vm-lifecycle']
  .map(name => 'ui/modules/' + name + '.js');

async function boot(page) {
  await page.evaluate(() => {
    Object.assign(window, {
      _L: (ko, en) => en, t: key => key, authToken: 'fixture', _DEBUG: false,
      currentUser: { role: 'ADMIN' }, currentTab: 'networks', eventLog: [],
      vmList: [{ name: 'vm-a', uuid: 'uuid-a', state: 'shutoff', vcpu: 2,
        memory_mb: 2048, storage_type: 'qcow2' }],
      selectedVmIndex: 0, checkedVms: new Set([0]), selCtr: 'ctr-a',
      fmtBytes: n => String(n), drawDonut: () => {}, render: () => {},
      loadAll: () => {}, renderContent: () => {}, customConfirm: async () => true,
      _events: [], _toasts: [], _requests: [],
    });
    window.addEvt = message => _events.push(message);
    window.toast = (...args) => _toasts.push(args);
    window._reply = (data, status = 200) => new Response(JSON.stringify(data), {
      status, headers: { 'content-type': 'application/json' },
    });
    window.fetch = async (url, opts = {}) => {
      _requests.push({ url: String(url), method: opts.method || 'GET',
        body: opts.body ? JSON.parse(opts.body) : null });
      return window._handler(String(url), opts);
    };
    window._handler = () => _reply({ error: { message: 'CONTROLLED_SERVICE_ERROR' } }, 503);
    const originalTimeout = window.setTimeout.bind(window);

    const timers = new Set();
    window._clearTimers = () => { timers.forEach(clearTimeout); timers.clear(); };
    window.setTimeout = (fn, ms, ...args) => {
      const id = originalTimeout(fn, ms >= 1000 ? 0 : ms, ...args);
      timers.add(id); return id;
    };
  });
}

function regression(name, fn) {
  test(name, async () => withPage(MODS, async page => { await boot(page); await fn(page); }));
}

regression('delete/stop transport and HTTP errors never record completion', async page => {
  const rows = await page.evaluate(async () => {
    const out = [];
    for (const failure of ['transport', 'http']) {
      window._handler = () => failure === 'transport'
        ? Promise.reject(new TypeError('CONTROLLED_TRANSPORT_REJECT'))
        : _reply({ error: { message: 'CONTROLLED_SERVICE_ERROR' } }, 403);
      for (const kind of ['network', 'container', 'stop', 'vm']) {
        _clearTimers(); closeModal(true); _events.length = 0; _toasts.length = 0;
        if (kind === 'network') {
          netDel('net-a'); document.querySelector('#del-net-confirm').value = 'net-a';
          await doNetDel('net-a');
        } else if (kind === 'container') {
          ctrDel('ctr-a'); document.querySelector('#del-ctr-confirm').value = 'ctr-a';
          await doCtrDel('ctr-a');
        } else if (kind === 'stop') await ctrA('ctr-a', 'stop');
        else await doVmDel('vm-a');
        out.push({ failure, kind, text: PCV.modalCore.currentBody().textContent,
          events: [..._events], toasts: [..._toasts] });
      }
    }
    return out;
  });
  for (const row of rows) {
    assert.equal(row.events.length, 0, JSON.stringify(row));
    assert.doesNotMatch(row.text, /✅/, JSON.stringify(row));
    assert.match(row.text, /CONTROLLED_|unconfirmed|failed|verify|check/i);
  }
});

regression('container state confirmation distinguishes pending and completed actions', async page => {
  const rows = await page.evaluate(async () => {
    const out = [];
    for (const action of ['start', 'stop']) for (const reached of [false, true]) {
      _clearTimers(); closeModal(true); _events.length = 0; _requests.length = 0;
      const target = action === 'start' ? 'RUNNING' : 'STOPPED';
      window._handler = (url, opts) => _reply(opts.method === 'POST'
        ? { accepted: true } : { data: [{ name: 'ctr-a', state: reached ? target : 'STARTING' }] });
      await ctrA('ctr-a', action);
      out.push({ action, reached, text: document.querySelector('#ctr-st').textContent,
        events: [..._events], polls: _requests.filter(r => r.method === 'GET').length });
    }
    return out;
  });
  for (const row of rows) {
    assert.equal(row.events.length, row.reached ? 1 : 0);
    assert.equal(row.polls, row.reached ? 1 : 8);
    assert.match(row.text, row.reached ? /✅/ : /unconfirmed/);
  }
});

regression('successful network/container/VM deletion has positive confirmation', async page => {
  const rows = await page.evaluate(async () => {
    const out = [];
    for (const kind of ['network', 'container', 'vm']) {
      _clearTimers(); closeModal(true); _events.length = 0;
      window._handler = (url, opts) => {
        if (kind !== 'vm') return _reply(opts.method === 'DELETE' ? { data: {} } : { data: [] });
        if (opts.method === 'DELETE') return _reply({ data: { job_id: 'delete-fixture' } });
        if (url.includes('/jobs/')) return _reply({ data: { status: 'completed' } });
        return _reply({ data: window.vmList });
      };
      if (kind === 'network') {
        netDel('net-a'); document.querySelector('#del-net-confirm').value = 'net-a'; await doNetDel('net-a');
      } else if (kind === 'container') {
        ctrDel('ctr-a'); document.querySelector('#del-ctr-confirm').value = 'ctr-a'; await doCtrDel('ctr-a');
      } else await doVmDel('vm-a');
      out.push({ kind, events: [..._events], text: PCV.modalCore.currentBody().textContent });
    }
    return out;
  });
  for (const row of rows) { assert.equal(row.events.length, 1); assert.match(row.text, /✅/); }
});

regression('HTTP errors stay visible in inventory, backup, forecast and guest panels', async page => {
  const panels = await page.evaluate(async () => {
    const el = PCV.uxlib.el, cb = document.querySelector('#cb'), out = {};
    cb.appendChild(el('div', { id: 'cm-jobs' })); await cmLoadJobs(); out.cloud = cb.textContent;
    await renderIscsi(cb); out.iscsi = cb.textContent;
    await renderBackup(cb); out.backup = cb.querySelector('#backup-policies').textContent;
    await backupLoadHistory(); out.history = cb.querySelector('#backup-history').textContent;
    cb.appendChild(el('div', { id: 'storage-forecast' })); await loadStorageForecast();
    out.forecast = cb.querySelector('#storage-forecast').textContent;
    showGuestAgent(); await gaRefreshStatus(); out.guest = PCV.modalCore.currentBody().textContent;
    closeModal(true); await showCpuStats(); out.cpu = PCV.modalCore.currentBody().textContent;
    await renderOvn(cb); out.ovn = cb.textContent;
    cb.appendChild(el('input', { id: 'sg-list-switch', value: 'sw-a' }));
    cb.appendChild(el('div', { id: 'sg-rules' })); await sgListRules();
    out.acl = cb.querySelector('#sg-rules').textContent;
    cb.appendChild(el('div', { id: 'fw-rules-list' })); await fwLoadRules();
    out.firewall = cb.querySelector('#fw-rules-list').textContent;
    return out;
  });
  for (const [key, text] of Object.entries(panels)) {
    assert.match(text, /CONTROLLED_SERVICE_ERROR|Failed to load|Unable to load OVN data/, key + ': ' + text);
    assert.doesNotMatch(text, /No migration jobs|No iSCSI targets|No policies|No snapshots|Channel missing|VM may be stopped/);
  }

  const app = fs.readFileSync(new URL('../../ui/app.js', import.meta.url), 'utf8');
  const start = app.indexOf('window.sgListRules = async function');
  assert.ok(start >= 0);
  const end = app.indexOf('\n};', start) + 3;
  const text = await page.evaluate(async source => {
    window.eval(source); await sgListRules(); return document.querySelector('#sg-rules').textContent;
  }, app.slice(start, end));
  assert.match(text, /CONTROLLED_SERVICE_ERROR/);
});

regression('Job terminal status controls OVA and live resize completion', async page => {
  const rows = await page.evaluate(async () => {
    const out = [];
    for (const operation of ['ova', 'resize']) for (const status of ['failed', 'cancelled', 'running', 'completed']) {
      _clearTimers(); closeModal(true); _events.length = 0; _toasts.length = 0; _requests.length = 0;
      window._handler = (url, opts) => _reply(opts.method === 'POST'
        ? { accepted: true, job_id: 'job-' + operation, status: 'accepted' }
        : { data: { job_id: 'job-' + operation, status, detail: 'CONTROLLED_JOB_FAILURE' } });
      if (operation === 'ova') await vmExportOva(0);
      else { showDiskLiveResize(); await doDiskLiveResize(); }
      out.push({ operation, status, text: PCV.modalCore.currentBody()?.textContent || '',
        events: [..._events], toasts: [..._toasts], requests: [..._requests] });
    }
    return out;
  });
  for (const row of rows) {
    const completed = row.events.some(s => /OVA Export|resize completed/i.test(s));
    assert.equal(completed, row.status === 'completed', JSON.stringify(row));
    assert.ok(row.requests.some(r => r.url.endsWith('/jobs/job-' + row.operation)), JSON.stringify(row));
    if (row.status !== 'completed') assert.doesNotMatch(row.text, /Export completed/);
    if (['failed', 'cancelled'].includes(row.status)) assert.match(JSON.stringify(row.toasts), /CONTROLLED_JOB_FAILURE/);
  }
});

regression('forecast starts after slow zvol response mounts its own panel', async page => {
  await page.evaluate(() => {
    window._handler = async url => {
      if (url.endsWith('/storage/pools')) return _reply({ data: [{ name: 'tank', size: '100G', alloc: '50G', health: 'ONLINE' }] });
      if (url.endsWith('/storage/zvols')) { await new Promise(resolve => { window._releaseZvol = resolve; }); return _reply({ data: [] }); }
      return _reply({ result: [{ name: 'tank', used_percent: 50, days_to_full: 90 }] });
    };
    window._renderPromise = renderStorage(document.querySelector('#cb'));
  });
  await page.waitForFunction(() => typeof window._releaseZvol === 'function');
  await new Promise(resolve => setTimeout(resolve, 220));
  assert.equal(await page.evaluate(() => _requests.filter(r => r.body?.method === 'storage.pool.forecast').length), 0);
  await page.evaluate(async () => { _releaseZvol(); await _renderPromise; });
  await page.waitForFunction(() => document.querySelector('#storage-forecast')?.textContent.includes('90'));
  assert.equal(await page.evaluate(() => _requests.filter(r => r.body?.method === 'storage.pool.forecast').length), 1);
});

regression('clone pins name/UUID across reordering and rejects replacement or disappearance', async page => {
  const rows = await page.evaluate(async () => {
    const out = [];
    for (const change of ['unchanged', 'reordered', 'removed', 'replaced']) {
      _clearTimers(); closeModal(true); _requests.length = 0; _toasts.length = 0;
      const original = { name: 'vm-a', uuid: 'uuid-a', state: 'shutoff', storage_type: 'qcow2' };
      vmList = [original]; selectedVmIndex = 0;
      window._handler = () => _reply({ accepted: true, job_id: 'job-clone' });
      await vmClone(0); document.querySelector('#vm-clone-name').value = 'new-vm';
      if (change === 'reordered') vmList = [{ ...original, name: 'vm-b', uuid: 'uuid-b' }, original];
      if (change === 'removed') vmList = [{ ...original, name: 'vm-b', uuid: 'uuid-b' }];
      if (change === 'replaced') vmList = [{ ...original, uuid: 'uuid-replacement' }];
      await doVmClone(); out.push({ change, requests: [..._requests], toasts: [..._toasts] });
    }
    return out;
  });
  for (const row of rows) {
    const posts = row.requests.filter(r => r.method === 'POST');
    assert.equal(posts.length, ['removed', 'replaced'].includes(row.change) ? 0 : 1, JSON.stringify(row));
    if (posts.length) assert.match(posts[0].url, /\/vms\/vm-a\/clone$/);
    else assert.match(JSON.stringify(row.toasts), /Source VM changed/);
  }
});

regression('bulk snapshot reads requested name before modal teardown; blank keeps default', async page => {
  const rows = await page.evaluate(async () => {
    const out = [];
    for (const input of ['user-required-name', '']) {
      _clearTimers(); closeModal(true); _requests.length = 0;
      window._handler = () => _reply({ data: { status: 'done' } });
      showBulkActions(); document.querySelector('#bulk-snap-name').value = input;
      await bulkSnapshot(); out.push({ input, requests: [..._requests] });
    }
    return out;
  });
  for (const row of rows) {
    assert.equal(row.requests.length, 1);
    assert.match(row.requests[0].url, /\/vms\/vm-a\//);
    if (row.input) assert.equal(row.requests[0].body.snap_name, row.input);
    else assert.match(row.requests[0].body.snap_name, /^snap-/);
  }
});


for (const panel of ['cpu', 'agent']) for (const failure of ['success', 'http', 'transport']) {
  regression(`${panel} late ${failure} response cannot overwrite another VM dialog`, async page => {
    const result = await page.evaluate(async ({ panel, failure }) => {
      const original = vmList[0]; let release;
      window._handler = async url => {
        if (url.includes('/vm-a/')) {
          await new Promise(resolve => { release = resolve; });
          if (failure === 'transport') throw new TypeError('OLD_VM_FAILURE');
          if (failure === 'http') return _reply({ error: { message: 'OLD_VM_FAILURE' } }, 503);
          return _reply({ data: { vcpu_count: 111, cpu_time_ns: 111111, status: 'vm_stopped', message: 'OLD_VM_STATUS' } });
        }
        return _reply({ data: { vcpu_count: 222, cpu_time_ns: 222222, status: 'ok', message: 'NEW_VM_STATUS' } });
      };
      if (panel === 'agent') { showGuestAgent(); _clearTimers(); }
      const old = panel === 'cpu' ? showCpuStats() : gaRefreshStatus();
      await Promise.resolve();
      closeModal(true); _clearTimers();
      vmList = [{ ...original, name: 'vm-b', uuid: 'uuid-b' }];
      if (panel === 'agent') { showGuestAgent(); _clearTimers(); }
      await (panel === 'cpu' ? showCpuStats() : gaRefreshStatus());
      const body = PCV.modalCore.currentBody(), before = body.textContent;
      release(); await old;
      return { before, after: body.textContent, current: body === PCV.modalCore.currentBody() };
    }, { panel, failure });
    assert.equal(result.current, true);
    assert.equal(result.after, result.before);
    assert.match(result.after, panel === 'cpu' ? /222222/ : /NEW_VM_STATUS/);
    assert.doesNotMatch(result.after, /OLD_VM_|111111/);
  });
}

regression('resize and guest controls retain the named VM and reject replacement', async page => {
  const rows = await page.evaluate(async () => {
    const out = [];
    for (const action of ['resize', 'refresh', 'ensure', 'ping', 'exec', 'shutdown']) {
      for (const change of ['unchanged', 'reordered', 'removed', 'replaced']) {
        _clearTimers(); closeModal(true); _requests.length = 0; _toasts.length = 0;

        const original = { name: 'vm-' + action + '-' + change, uuid: 'uuid-a', state: 'running' };
        vmList = [original]; selectedVmIndex = 0;
        window._handler = () => _reply({ data: { status: 'CONTROL_OK' } });
        if (action === 'resize') showDiskLiveResize();
        else { showGuestAgent(); document.querySelector('#ga-cmd').value = 'fixture-command'; }
        _clearTimers();
        if (change === 'reordered') vmList = [{ ...original, name: 'vm-other', uuid: 'uuid-b' }, original];
        if (change === 'removed') vmList = [];
        if (change === 'replaced') vmList = [{ ...original, uuid: 'uuid-replacement' }];
        const run = { resize: doDiskLiveResize, refresh: gaRefreshStatus, ensure: gaEnsureChannel,
          ping: gaPing, exec: gaExec, shutdown: gaShutdown }[action];
        await run();
        out.push({ action, change, name: original.name, requests: [..._requests], toasts: [..._toasts] });
      }
    }
    _clearTimers();
    return out;
  });
  for (const row of rows) {
    const rejected = ['removed', 'replaced'].includes(row.change);
    assert.equal(row.requests.length, rejected ? 0 : 1, JSON.stringify(row));
    if (rejected) assert.match(JSON.stringify(row.toasts), /VM changed or disappeared/);
    else assert.ok(row.requests[0].url.includes('/vms/' + row.name + '/'), JSON.stringify(row));
  }
});

regression('accepted resize response cannot close a subsequently opened dialog', async page => {
  const result = await page.evaluate(async () => {
    let release;
    window._handler = async () => {
      await new Promise(resolve => { release = resolve; });
      return _reply({ accepted: true });
    };
    showDiskLiveResize(); const pending = doDiskLiveResize();
    await Promise.resolve(); closeModal(true); _clearTimers();
    showGuestAgent(); _clearTimers();
    const body = PCV.modalCore.currentBody(), before = body.textContent;
    release(); await pending;
    return { current: PCV.modalCore.currentBody() === body, connected: body.isConnected,
      before, after: body.textContent, events: _events };
  });
  assert.equal(result.current, true); assert.equal(result.connected, true);
  assert.equal(result.after, result.before);
  assert.match(result.events.join('\n'), /Disk resize accepted: vm-a/);
});

regression('guest confirmation revalidates the captured VM after awaiting the user', async page => {
  const result = await page.evaluate(async () => {
    window.customConfirm = async () => { vmList = [{ ...vmList[0], uuid: 'replacement' }]; return true; };
    showGuestAgent(); _clearTimers();
    await gaShutdown();
    return { requests: _requests, toasts: _toasts };
  });
  assert.equal(result.requests.length, 0);
  assert.match(JSON.stringify(result.toasts), /VM changed or disappeared/);
});

regression('network reentry and reverse responses cannot replace the current filter cache', async page => {
  const result = await page.evaluate(async () => {
    window._navGeneration = 1;
    const cb = document.querySelector('#cb');
    const network = name => ({ name, mode: 'nat', state: 'up', ip_cidr: '', slaves: [] });
    let releaseOld, count = 0;
    window._handler = async url => {
      if (url === EP.NET_LIST()) {
        if (++count === 1) { await new Promise(resolve => { releaseOld = resolve; }); return _reply({ data: [network('old-network')] }); }
        return _reply({ data: [network('current-network')] });
      }
      return _reply({ data: {} });
    };
    const oldTarget = document.createElement('div'); cb.appendChild(oldTarget);
    const pending = renderNetworks(oldTarget);
    window._navGeneration += 2;
    oldTarget.remove();
    await renderNetworks(cb);
    releaseOld(); await pending;
    const before = cb.textContent;
    PCV.ui.filterState.apply({ netstate: ['up'] });
    return { before, after: cb.textContent, count };
  });
  assert.match(result.before, /current-network/);
  assert.match(result.after, /current-network/);
  assert.doesNotMatch(result.after, /old-network/);
  assert.equal(result.count, 2);
});

regression('host baseline retries only publish the newest response', async page => {
  const result = await page.evaluate(async () => {
    window._handler = () => _reply({ data: [] });
    await renderNetworks(document.querySelector('#cb'));
    let releaseOld, count = 0;
    window._handler = async url => {
      if (url === EP.NET_HOST_BASELINE()) {
        const sequence = ++count;
        if (sequence === 1) await new Promise(resolve => { releaseOld = resolve; });
        return _reply({ data: { management: { available: true,
          interface: sequence === 1 ? 'old-management' : 'new-management' }, interfaces: [], routes: [] } });
      }
      return _reply({ data: {} });
    };
    const old = retryHostNetworkBaseline();
    await retryHostNetworkBaseline(); releaseOld(); await old;
    return { text: document.querySelector('#cb').textContent, count };
  });
  assert.equal(result.count, 2);
  assert.match(result.text, /new-management/);
  assert.doesNotMatch(result.text, /old-management/);
});

regression('late network transport error cannot erase a newer refresh in the same generation', async page => {
  const text = await page.evaluate(async () => {
    let rejectOld, count = 0;
    window._handler = url => {
      if (url === EP.NET_LIST() && ++count === 1)
        return new Promise((resolve, reject) => { rejectOld = reject; });
      return _reply({ data: url === EP.NET_LIST()
        ? [{ name: 'latest-network', mode: 'nat', state: 'up', slaves: [] }] : {} });
    };
    const cb = document.querySelector('#cb'), old = renderNetworks(cb);
    await renderNetworks(cb);
    rejectOld(new TypeError('LATE_TRANSPORT_ERROR')); await old;
    return cb.textContent;
  });
  assert.match(text, /latest-network/);
  assert.doesNotMatch(text, /LATE_TRANSPORT_ERROR/);
});
