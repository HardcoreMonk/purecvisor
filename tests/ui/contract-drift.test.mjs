import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';
import { bootApp, captureBodies, captureScreens } from './contract-drift-bodies.mjs';

const UNKNOWN = '알 수 없음';
const AGENT_CONFIG = {
  rate_limit_sec: 300, timeout_sec: 10, total_queries: 0,
  providers: ['Claude', 'OpenAI', 'Gemini', 'Ollama'].map(name => ({ name, model: name + '-m', endpoint: 'http://e/' + name, enabled: false, api_key: '' }))
};
const INPUT = {
  vm: 'drift-vm',
  ctr: { name: 'drift-ctr', bridge: 'drbr0' },
  pool: { mirror_name: 'drmirror', stripe_name: 'drstripe', disks: ' /dev/drift-a   /dev/drift-b ' },
  audit: { username: 'bob', method: 'vm.delete', from: '2026-11-01', to: '2026-11-02' },
  agent: { key: 'drift-fake-key-0001', config: AGENT_CONFIG },
  timezone: 'Asia/Seoul'
};

let bodies = null;
async function allBodies() {
  if (!bodies) bodies = await captureBodies(INPUT);
  return bodies;
}

const rpcOk = result => ({ jsonrpc: '2.0', id: 'x', result });
const HEALING_OK = {
  'healing.pending': rpcOk([{ id: 7, policy: 'cpu-hog', action: 'throttle', reason: 'load', ts: 1 }]),
  'healing.history': rpcOk([]),
  'agent.history': rpcOk([])
};

test('컨테이너 생성 폼: IP Mode·정적 IP·게이트웨이·DNS 입력이 없다', async () => {
  await withPage([], async (page, ctx) => {
    await bootApp(page, ctx.port);
    await page.evaluate(() => window.showCtrCreate());
    await page.waitForSelector('#cc-name');
    for (const id of ['cc-ipmode', 'cc-ip', 'cc-gw', 'cc-dns', 'cc-static-row', 'cc-gw-row']) {
      assert.equal(await page.$(`#${id}`), null, `#${id}가 남아 있다`);
    }
    assert.ok(await page.$('#cc-br'), '브리지 선택은 남는다');
  }, { routes: { '/api/v1/networks': { status: 200, body: { data: [] } } } });
});

test('컨테이너 생성 본문: 서버가 읽는 키만 나간다', async () => {
  const { container_create: body } = await allBodies();
  assert.deepEqual(Object.keys(body).sort(), ['bridge', 'image', 'memory_mb', 'name', 'vcpu_count']);
  assert.equal(body.bridge, INPUT.ctr.bridge);
});

test('컨테이너 요약·옵션 탭: 브리지·권한은 서버 값, 서버가 주지 않는 AppArmor는 알 수 없음', async () => {
  const s = await captureScreens({
    ctr: 'c1',
    containers: { data: [{ name: 'c1', state: 'STOPPED', privileged: false, image: 'x', ip_addr: '' }] },
    nics: { data: [{ name: 'eth0', type: 'veth', bridge: 'drbr0' }, { name: 'eth1', type: 'veth', bridge: 'drbr1' }] }
  });
  assert.equal(s.ctr_bridge, 'drbr0, drbr1');
  assert.equal(s.ctr_apparmor, UNKNOWN);
  assert.equal(s.ctr_opt_apparmor, UNKNOWN);
  assert.equal(s.ctr_opt_unprivileged, 'Yes');
  assert.equal(s.ctr_type, 'LXC (unprivileged)');
  assert.doesNotMatch(s.ctr_opt_text, /unconfined|Keyctl|Nesting|FUSE|Start on boot|Protection|SIGRTMIN/);
});

for (const [label, input] of [
  ['빈 NIC 목록', { nics: { data: [] } }],
  ['NIC 조회 실패', { nics: { error: { code: -32603, message: 'boom' } }, status: { nics: 500 } }],
  ['bridge가 빈 NIC', { nics: { data: [{ name: 'eth0', bridge: '' }] } }]
]) {
  test(`컨테이너 요약 탭: ${label}이면 고정 pcvbr0가 아니라 알 수 없음`, async () => {
    const s = await captureScreens({
      ctr: 'c1', containers: { data: [{ name: 'c1', state: 'STOPPED', image: 'x' }] }, ...input
    });
    assert.equal(s.ctr_bridge, UNKNOWN);
    assert.equal(s.ctr_opt_unprivileged, UNKNOWN, '응답에 privileged가 없으면 권한도 알 수 없음');
  });
}

for (const [mode, label, cls] of [['active', 'ACTIVE', 'sh-active'], ['dry_run', 'DRY RUN', 'sh-dry']]) {
  test(`자가치유 ${mode}: 개요·자가치유·모바일이 healing.status 값을 그린다`, async () => {
    const s = await captureScreens({ rpc: { ...HEALING_OK, 'healing.status': rpcOk({ mode, pending: 1 }) } });
    assert.equal(s.selfhealing_badge, label);
    assert.equal(s.mobile_mode, label);
    assert.equal(s.overview_mode, label);
    assert.equal(s.overview_badge, label);
    assert.ok(cls);
  });
}

test('자가치유 개요 패널: 응답이 느리고 도중에 화면을 비웠다 다시 그려도 모드가 그려진다', async () => {
  await withPage([], async (page, ctx) => {
    await bootApp(page, ctx.port);
    await page.evaluate(async () => {
      const realFetch = window.fetch;
      window.fetch = async (...args) => { await new Promise(resolve => setTimeout(resolve, 300)); return realFetch(...args); };
      const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
      const b = document.getElementById('drift-host');
      await window.renderMonOverview(b, [], [], 0, 0, 0, 0, 0);
      await wait(250);
      b.textContent = '';
      await wait(100);
      await window.renderMonOverview(b, [], [], 0, 0, 0, 0, 0);
    });
    await page.waitForFunction(() => document.querySelector('#selfhealing-panel .sh-badge')?.textContent === 'ACTIVE');
    const mode = await page.$$eval('#drift-host .hr', rows => {
      const row = rows.find(r => r.querySelector('.k')?.textContent === 'Mode');
      return row ? row.querySelector('.v').textContent : null;
    });
    assert.equal(mode, 'ACTIVE');
  }, { routes: { '/api/v1/rpc': rec => {
    const body = { ...HEALING_OK, 'healing.status': rpcOk({ mode: 'active', pending: 1 }) }[rec.json && rec.json.method];
    return body ? { status: 200, body } : { status: 404, body: { error: { code: -32601, message: 'no fixture' } } };
  } } });
});

test('자가치유 조회 실패: 세 화면 모두 DRY RUN이 아니라 알 수 없음', async () => {
  const s = await captureScreens({
    rpc: { ...HEALING_OK, 'healing.status': { error: { code: -32603, message: 'boom' } } },
    status: { 'healing.status': 500 }
  });
  assert.equal(s.selfhealing_badge, UNKNOWN);
  assert.equal(s.mobile_mode, UNKNOWN);
  assert.equal(s.overview_mode, UNKNOWN);
});

test('자가치유 모드 값이 낯설면 추측하지 않고 알 수 없음', async () => {
  const s = await captureScreens({ rpc: { ...HEALING_OK, 'healing.status': rpcOk({ mode: 'weird', pending: 0 }) } });
  assert.equal(s.selfhealing_badge, UNKNOWN);
  assert.equal(s.overview_mode, UNKNOWN);
});

test('자가치유 조회 실패 뒤 새로고침: 직전 모드를 그대로 두지 않는다', async () => {
  let fail = false;
  await withPage([], async (page, ctx) => {
    await bootApp(page, ctx.port);
    await page.evaluate(async () => { await window.renderSelfHealing(document.getElementById('drift-host')); await PCV.selfhealing.refresh(); });
    assert.equal(await page.$eval('#selfhealing-panel .sh-badge', n => n.textContent), 'ACTIVE');
    fail = true;
    await page.evaluate(() => PCV.selfhealing.refresh());
    assert.equal(await page.$eval('#selfhealing-panel .sh-badge', n => n.textContent), UNKNOWN);
  }, { routes: { '/api/v1/rpc': rec => {
    if (rec.json.method === 'healing.status') {
      return fail ? { status: 500, body: { error: { code: -32603, message: 'boom' } } } : { status: 200, body: rpcOk({ mode: 'active', pending: 0 }) };
    }
    return { status: 200, body: rpcOk([]) };
  } } });
});

test('자가치유 개요 대기 패널: healing.pending을 쓰고 승인 버튼이 서버 id를 지목한다', async () => {
  const methods = [];
  await withPage([], async (page, ctx) => {
    await bootApp(page, ctx.port);
    await page.evaluate(() => { const el = document.createElement('div'); el.id = 'healing-pending-list'; document.body.appendChild(el); });
    await page.evaluate(() => window.loadHealingPending());
    const list = await page.$eval('#healing-pending-list', n => ({ text: n.textContent, approve: n.querySelector('button[onclick^="healingApprove"]')?.getAttribute('onclick') }));
    assert.match(list.text, /throttle/);
    assert.match(list.text, /cpu-hog/);
    assert.equal(list.approve, 'healingApprove(7)');
  }, { routes: { '/api/v1/rpc': rec => {
    methods.push(rec.json.method);
    return { status: 200, body: HEALING_OK[rec.json.method] || rpcOk([]) };
  } } });
  assert.ok(methods.includes('healing.pending'), methods.join());
  assert.ok(!methods.includes('healing.history'), '대기 패널이 이력을 읽으면 늘 비어 보인다');
});

test('자가치유 개요 대기 패널: 조회 실패가 "대기 없음"으로 보이지 않는다', async () => {
  const s = await captureScreens({
    rpc: { 'healing.status': rpcOk({ mode: 'dry_run', pending: 0 }), 'healing.history': rpcOk([]), 'agent.history': rpcOk([]),
           'healing.pending': { error: { code: -32603, message: 'boom' } } },
    status: { 'healing.pending': 500 }
  });
  assert.doesNotMatch(s.overview_pending, /대기 중인 액션 없음|No pending/);
  assert.match(s.overview_pending, new RegExp(UNKNOWN));
});

test('감사 검색 쿼리: 핸들러 키로 보내고 날짜를 감사 ts(UTC 초) 형식으로 바꾼다', async () => {
  const { audit_search: search } = await allBodies();
  const q = new URLSearchParams(search);
  assert.equal(q.get('username'), 'bob');
  assert.equal(q.get('method'), 'vm.delete');

  assert.equal(q.get('from_ts'), '2026-10-31 15:00:00');
  assert.equal(q.get('to_ts'), '2026-11-02 14:59:59');
  assert.equal(q.get('limit'), '100');
  for (const old of ['user', 'action', 'from', 'to']) assert.equal(q.get(old), null, `옛 키 ${old}`);
});

test('감사 결과: 서버의 {items} 봉투를 행으로 그린다', async () => {
  const s = await captureScreens({
    audit: { data: { items: [{ ts: '2026-11-01 00:00:01', username: 'bob', method: 'vm.delete', target: 'v1', result: 'ok', src_ip: '192.0.2.1' }], total: 1, offset: 0, limit: 100 } }
  });
  assert.deepEqual(s.audit_rows, [['2026-11-01 00:00:01', 'bob', 'vm.delete', 'v1', 'ok', '192.0.2.1']]);
  assert.equal(s.audit_error, false);
});

test('감사 빈 결과는 결과 없음, 오류는 오류로 구분된다', async () => {
  const empty = await captureScreens({ audit: { data: { items: [], total: 0, offset: 0, limit: 100 } } });
  assert.match(empty.audit_text, /검색 결과 없음/);
  assert.equal(empty.audit_error, false);
  const denied = await captureScreens({ audit: { error: { code: -32006, message: 'permission denied' } }, audit_status: 403 });
  assert.doesNotMatch(denied.audit_text, /검색 결과 없음/);
  assert.match(denied.audit_text, /permission denied/);
  assert.equal(denied.audit_error, true);
});

test('감사 메뉴와 바로가기: ADMIN에게만 보인다', async () => {
  await withPage([], async (page, ctx) => {
    await bootApp(page, ctx.port);
    await page.evaluate(() => {
      for (const id of ['shell-sidebar', 'shell-topbar', 'shell-statusbar']) {
        if (!document.getElementById(id)) { const d = document.createElement('div'); d.id = id; document.body.appendChild(d); }
      }
      PCV.shell.mount();
    });
    for (const [role, hidden] of [['VIEWER', true], ['OPERATOR', true], ['ADMIN', false]]) {
      const got = await page.evaluate(r => {
        window.currentUser = { role: r };
        window.applyRoleVisibility(r);
        const node = document.querySelector('[data-nav="mon-audit"]');
        return { attr: node.getAttribute('data-role'), hidden: node.classList.contains('role-hidden') };
      }, role);
      assert.equal(got.attr, 'ADMIN');
      assert.equal(got.hidden, hidden, role);
    }
    const shortcuts = await page.evaluate(async () => {
      window.currentUser = { role: 'VIEWER' };
      await window.renderOpsTriage(document.getElementById('drift-host'));
      window.applyRoleVisibility('VIEWER');
      return [...document.querySelectorAll('#drift-host button')]
        .filter(n => /감사 로그/.test(n.textContent))
        .map(n => ({ role: n.getAttribute('data-role'), hidden: n.classList.contains('role-hidden') }));
    });
    assert.ok(shortcuts.length >= 1);
    for (const s of shortcuts) assert.deepEqual(s, { role: 'ADMIN', hidden: true });
  }, { routes: { '/api/v1/vms': { status: 200, body: { data: [] } } } });
});

test('대역폭 화면: burst 입력이 없고 단위가 KB/s로 적힌다', async () => {
  await withPage([], async (page, ctx) => {
    await bootApp(page, ctx.port);
    const r = await page.evaluate(() => {
      const host = document.getElementById('drift-host');
      host.textContent = '';
      window.hwBandwidth().forEach(node => host.appendChild(node));
      return { burst: !!document.getElementById('sbw-burst'), text: host.textContent, in: document.getElementById('sbw-in').value };
    });
    assert.equal(r.burst, false);
    assert.match(r.text, /KB\/s/);
    assert.doesNotMatch(r.text, /Mbps\)/);
    assert.equal(r.in, '', '기본값이 없어 누르기만 해서 상한이 걸리지 않는다');
  });
});

test('대역폭 본문: inbound_kbps·outbound_kbps 정수, 빈 칸은 보내지 않는다', async () => {
  const { bandwidth, bandwidth_outbound_only: one } = await allBodies();
  assert.deepEqual(bandwidth, { inbound_kbps: 2048, outbound_kbps: 1024 });
  assert.deepEqual(one, { outbound_kbps: 512 });
});

test('대역폭 거부: 둘 다 비었거나 0 이하이면 보내지 않는다', async () => {
  await withPage([], async (page, ctx) => {
    await bootApp(page, ctx.port);
    for (const [i, o] of [['', ''], ['0', ''], ['-5', '0'], ['1.5', '']]) {
      const n = await page.evaluate(async (a, b) => {
        window.vmList = [{ name: 'v1' }]; window.selectedVmIndex = 0; window.__toasts = []; window.showSettings();
        const host = document.getElementById('drift-host');
        host.textContent = '';
        window.hwBandwidth().forEach(node => host.appendChild(node));
        document.getElementById('sbw-in').value = a;
        document.getElementById('sbw-out').value = b;
        await window.doBandwidth();
        return window.__toasts.filter(t => t.ok === false).length;
      }, i, o);
      assert.equal(n, 1, `in=${i} out=${o}`);
    }
    assert.equal(ctx.requests.filter(r => r.path === '/api/v1/vms/v1/bandwidth').length, 0);
  }, { routes: { '/api/v1/vms/v1/bandwidth': { status: 200, body: { data: {} } } } });
});

test('CPU 핀 본문: vcpu 정수와 cpuset 문자열', async () => {
  const { cpu_pin: body } = await allBodies();
  assert.deepEqual(body, { vcpu: 0, cpuset: '0' });
});

test('CPU 핀 거부: vcpu·cpuset이 비었거나 형식이 틀리면 보내지 않는다', async () => {
  await withPage([], async (page, ctx) => {
    await bootApp(page, ctx.port);
    for (const [v, c] of [['', '0'], ['0', ''], ['-1', '0'], ['a', '0'], ['0', 'x;y']]) {
      const n = await page.evaluate(async (a, b) => {
        window.vmList = [{ name: 'v1' }]; window.selectedVmIndex = 0; window.__toasts = []; window.showSettings();
        const host = document.getElementById('drift-host');
        host.textContent = '';
        window.hwCpuPin().forEach(node => host.appendChild(node));
        document.getElementById('scpin-vcpu').value = a;
        document.getElementById('scpin-cpuset').value = b;
        await window.doCpuPin();
        return window.__toasts.filter(t => t.ok === false).length;
      }, v, c);
      assert.equal(n, 1, `vcpu=${v} cpuset=${c}`);
    }
    assert.equal(ctx.requests.filter(r => r.path === '/api/v1/vms/v1/cpu-pin').length, 0);
  }, { routes: { '/api/v1/vms/v1/cpu-pin': { status: 200, body: { data: {} } } } });
});

test('풀 생성 본문: disks 배열과 vdev_type, stripe는 명시 선택 뒤 빈 문자열', async () => {
  const { pool_mirror: mirror, pool_stripe: stripe } = await allBodies();
  assert.deepEqual(mirror, { name: 'drmirror', disks: ['/dev/drift-a', '/dev/drift-b'], vdev_type: 'mirror' });
  assert.deepEqual(stripe, { name: 'drstripe', disks: ['/dev/drift-a', '/dev/drift-b'], vdev_type: '' });
});

test('풀 생성 거부: 빈 디스크·구성 미선택은 보내지 않는다', async () => {
  await withPage([], async (page, ctx) => {
    await bootApp(page, ctx.port);
    await page.evaluate(() => window.showPoolCreate());
    await page.waitForSelector('#pc-name');
    assert.equal(await page.$eval('#pc-raid', n => n.value), '', '기본 선택이 stripe이면 안 된다');
    const attempt = async (name, disks, raid) => page.evaluate(async (n, d, r) => {
      window.__toasts = [];
      document.getElementById('pc-name').value = n;
      document.getElementById('pc-disks').value = d;
      document.getElementById('pc-raid').value = r;
      await window.doPoolCreate();
      return window.__toasts.filter(t => t.ok === false).length;
    }, name, disks, raid);
    assert.equal(await attempt('p1', '   ', 'mirror'), 1, '빈 디스크');
    assert.equal(await attempt('p1', '/dev/a', ''), 1, '구성 미선택');
    assert.equal(ctx.requests.filter(r => r.method === 'POST' && r.path === '/api/v1/storage/pools').length, 0);
  }, { routes: { '/api/v1/storage/pools': { status: 200, body: { data: [] } } } });
});

test('자원 제한 본문: 서버가 읽는 cpu_percent·cpu_weight·memory_mb만 나간다', async () => {
  const { limits_cpu: cpu, limits_mem: mem } = await allBodies();
  assert.deepEqual(cpu, { name: INPUT.ctr.name, cpu_percent: 50, cpu_weight: 200 });
  assert.deepEqual(mem, { name: INPUT.ctr.name, memory_mb: 256 });
});

test('자원 제한 화면: 서버가 쓰지 않는 Shares·Quota·Swap 입력과 고정 기본값이 없다', async () => {
  await withPage([], async (page, ctx) => {
    await bootApp(page, ctx.port);
    await page.evaluate(() => {
      window.selCtr = 'c1';
      window.ctrTab = 'resources';
      return window.renderContainers(document.getElementById('drift-host'));
    });
    for (const id of ['ctr-cpu-shares', 'ctr-cpu-quota', 'ctr-swap-limit']) {
      assert.equal(await page.$(`#${id}`), null, `#${id}가 남아 있다`);
    }
    for (const id of ['ctr-cpu-percent', 'ctr-cpu-weight', 'ctr-mem-limit']) {
      assert.equal(await page.$eval(`#${id}`, n => n.value), '', `#${id} 기본값`);
    }
  }, { routes: { '/api/v1/containers': { status: 200, body: { data: [{ name: 'c1', state: 'STOPPED', image: 'x' }] } } } });
});

test('자원 제한 거부: 비었거나 범위 밖이면 보내지 않고, 응답의 적용 시점을 토스트로 알린다', async () => {
  await withPage([], async (page, ctx) => {
    await bootApp(page, ctx.port);
    await page.evaluate(() => {
      window.selCtr = 'c1';
      window.ctrTab = 'resources';
      return window.renderContainers(document.getElementById('drift-host'));
    });
    const attempt = (pct, weight) => page.evaluate(async (a, b) => {
      window.__toasts = [];
      document.getElementById('ctr-cpu-percent').value = a;
      document.getElementById('ctr-cpu-weight').value = b;
      await window.ctrSetLimits('c1', 'cpu');
      return window.__toasts;
    }, pct, weight);
    for (const [pct, weight] of [['', ''], ['0', ''], ['-1', ''], ['1.5', ''], ['', '10001']]) {
      const toasts = await attempt(pct, weight);
      assert.equal(toasts.filter(t => t.ok === false).length, 1, `pct=${pct} weight=${weight}`);
    }
    assert.equal(ctx.requests.filter(r => r.path === '/api/v1/containers/c1/limits').length, 0);
    const ok = await attempt('50', '');
    assert.match(ok.at(-1).msg, /다음 시작/);
  }, { routes: {
    '/api/v1/containers': { status: 200, body: { data: [{ name: 'c1', state: 'STOPPED', image: 'x' }] } },
    '/api/v1/containers/c1/limits': { status: 200, body: { data: { applied: 'config' } } }
  } });
});

test('게스트 실행: Args 입력이 없고 본문은 name·command뿐이다(서버는 /bin/sh -c로 command 한 줄만 실행한다)', async () => {
  const { guest_exec: body } = await allBodies();
  assert.deepEqual(body, { name: INPUT.vm, command: 'cat /etc/hostname' });
  await withPage([], async (page, ctx) => {
    await bootApp(page, ctx.port);
    await page.evaluate(() => { window.vmList = [{ name: 'v1', uuid: 'u' }]; window.selectedVmIndex = 0; window.showGuestAgent(); });
    await page.waitForSelector('#ga-cmd');
    assert.equal(await page.$('#ga-args'), null);
  });
});

test('AI Agent 저장 본문: 공급자마다 name·model·api_key·endpoint뿐이고 enabled를 보내지 않는다', async () => {
  const { agent_enable: on, agent_disable: off } = await allBodies();
  for (const body of [on, off]) {
    assert.deepEqual(Object.keys(body), ['providers']);
    assert.equal(body.providers.length, 4);
    for (const p of body.providers) assert.deepEqual(Object.keys(p).sort(), ['api_key', 'endpoint', 'model', 'name']);
  }
  assert.equal(on.providers[0].api_key, INPUT.agent.key);
  assert.equal(off.providers[0].api_key, '');
});

test('AI Agent 저장 거부: 키 칸이 빈 공급자의 model·endpoint를 고치면 보내지 않고 어느 공급자인지 알린다', async () => {
  const agentRoute = { status: 200, body: { data: { ...AGENT_CONFIG, providers: [
    { ...AGENT_CONFIG.providers[0], api_key: 'cla...123', enabled: true }, ...AGENT_CONFIG.providers.slice(1)
  ] } } };
  await withPage([], async (page, ctx) => {
    await bootApp(page, ctx.port);
    const puts = () => ctx.requests.filter(r => r.method === 'PUT' && r.path === '/api/v1/agent/config');
    const open = async () => {

      await page.evaluate(() => { window.__toasts = []; window.closeModal(); });
      await page.waitForFunction(() => !document.getElementById('agk0'));
      await page.evaluate(() => window.showAgentConfig());
      await page.waitForSelector('#agk0');
    };
    const save = () => page.evaluate(async () => { await window.saveAgentConfig(); return window.__toasts; });
    const set = (id, value) => page.$eval(id, (node, v) => { node.value = v; }, value);

    for (const [id, name, value] of [['#agm1', 'OpenAI', 'edited-model'], ['#age3', 'Ollama', 'http://edited/']]) {
      await open();
      await set(id, value);
      const toasts = await save();
      assert.equal(puts().length, 0, `${id}: 서버는 끄기만 하고 값을 버리므로 보내지 않는다`);
      assert.equal(toasts.filter(t => t.ok === false).length, 1, id);
      assert.match(toasts.at(-1).msg, new RegExp(name));
      assert.ok(await page.$('dialog #agk0'), '고칠 수 있게 창을 닫지 않는다');
    }

    await open();
    await set('#agk0', '');
    await set('#agm0', 'edited-model');
    const cleared = await save();
    assert.equal(puts().length, 0);
    assert.match(cleared.at(-1).msg, /Claude/);
    assert.doesNotMatch(cleared.at(-1).msg, /OpenAI|Gemini|Ollama/);

    await open();
    await set('#agm1', 'edited-model');
    await set('#agm1', 'OpenAI-m');
    assert.equal((await save()).filter(t => t.ok === false).length, 0);
    assert.equal(puts().length, 1);
    assert.equal(puts()[0].json.providers[1].api_key, '');

    await open();
    await set('#agm1', 'edited-model');
    await set('#agk1', 'fixture-provider-key');
    assert.equal((await save()).filter(t => t.ok === false).length, 0);
    assert.equal(puts().length, 2);
    assert.deepEqual(puts()[1].json.providers[1], { name: 'OpenAI', model: 'edited-model', api_key: 'fixture-provider-key', endpoint: 'http://e/OpenAI' });
    assert.equal(puts()[1].json.providers[2].api_key, '');
    assert.equal(puts()[1].json.providers[0].api_key, 'cla...123', '가림값은 그대로 돌려보낸다');

    await open();
    await set('#agk0', '');
    assert.equal((await save()).filter(t => t.ok === false).length, 0);
    assert.equal(puts().length, 3);
    assert.equal(puts()[2].json.providers[0].api_key, '');
  }, { routes: { '/api/v1/agent/config': agentRoute } });
});

test('AI Agent 공급자 탭: 활성 체크박스가 없고 활성은 키로 정해진다고 적는다', async () => {
  const s = await captureScreens({ agent: { data: AGENT_CONFIG } });
  assert.equal(s.agent_checkbox, false, '눌러도 서버에 닿지 않는 체크박스는 잘못된 효과를 약속한다');
  assert.match(s.agent_providers_text, /API 키가 있으면 켜지고/);
  assert.match(s.agent_providers_text, /DISABLED/);
});

test('AI Agent 설정 탭: 호출 간격·시간 제한은 서버 값 읽기 전용이고 저장 버튼이 없다', async () => {
  const s = await captureScreens({ agent: { data: { ...AGENT_CONFIG, rate_limit_sec: 123, timeout_sec: 45 } } });
  assert.equal(s.agent_settings_inputs, 0);
  assert.equal(s.agent_settings_save, false);
  assert.match(s.agent_settings_text, /123/);
  assert.match(s.agent_settings_text, /45/);
  const missing = await captureScreens({ agent: { data: { providers: AGENT_CONFIG.providers } } });
  assert.match(missing.agent_settings_text, new RegExp(UNKNOWN), '응답에 없으면 고정 300·10을 그리지 않는다');
  assert.doesNotMatch(missing.agent_settings_text, /300/);
});
