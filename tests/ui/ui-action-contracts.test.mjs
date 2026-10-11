







import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const CORE = [
  'ui/i18n.js',
  'ui/modules/endpoints.js', 'ui/modules/api.js', 'ui/modules/ui.js',
  'ui/modules/filter-state.js', 'ui/modules/uxlib.js',
  'ui/modules/modal-core.js', 'ui/modules/modal.js',
];
const VM_MODS = [...CORE, 'ui/modules/vm-lifecycle.js'];


async function recordFeedback(page) {
  await page.evaluate(() => {
    I18N.setLang('ko');
    window._L = (ko, _en) => ko;
    window.authToken = 'test-token';
    window.__toasts = [];
    window.__events = [];
    window.addNotification = (level, title) => window.__toasts.push(level + ':' + title);
    window.addEvt = message => window.__events.push(String(message));
    if (!document.getElementById('toasts')) {
      const toasts = document.createElement('div');
      toasts.id = 'toasts';
      document.body.appendChild(toasts);
    }
  });
}

test("CD/DVD Browse 에서 ISO 를 누르면 경로가 입력칸에 들어가고 브라우저 모달만 닫힌다", async () => {
  const isoPath = "/pcvpool/iso/ubuntu's 22.04 \"lts\".iso";
  await withPage(VM_MODS, async page => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await recordFeedback(page);
    await page.evaluate(async () => {
      window.vmList = [{ name: 'vm-iso', state: 'running', cdrom_path: '' }];
      window.selectedVmIndex = 0;
      window.showModal(window.hwCd());
      await window.browseISOForMount();
    });
    await page.evaluate(() => {
      const rows = Array.from(document.querySelectorAll('dialog[open] .modal-body div'))
        .filter(node => node.style.cursor === 'pointer' && node.textContent.includes('ubuntu'));
      rows[0].click();
    });
    await page.waitForFunction(() => document.querySelectorAll('dialog[open]').length === 1, { timeout: 2000 });
    await page.waitForFunction(expected => document.getElementById('si')?.value === expected,
      { timeout: 2000 }, isoPath);
    const state = await page.evaluate(() => ({
      value: document.getElementById('si').value,
      current: document.getElementById('cdrom-current').textContent,
    }));

    assert.deepEqual(pageErrors, []);
    assert.equal(state.value, isoPath);
    assert.equal(state.current, isoPath);
  }, { routes: { '/api/v1/iso': { body: { data: [
    { path: isoPath, name: "ubuntu's 22.04 \"lts\".iso", dir: '/pcvpool/iso', size_mb: 1200 },
  ] } } } });
});

const CREATE_BASE_ROUTES = {
  '/api/v1/config/daemon': {
    body: { data: { storage: { zvol_pool: 'pcvpool/vms', image_dir: '/var/lib/libvirt/images' } } },
  },
  '/api/v1/storage/pools': { body: { data: [] } },
  '/api/v1/networks': {
    body: { data: [{ name: 'pcvnat0', mode: 'nat', ip_cidr: '10.78.0.1/24', state: 'up' }] },
  },
};

function createRoutes(acceptedBody, jobReplies) {
  let jobCall = 0;
  return {
    ...CREATE_BASE_ROUTES,
    '/api/v1/vms': record => (record.method === 'POST'
      ? { body: acceptedBody }
      : { body: { data: [{ name: 'job-vm', state: 'shutoff' }] } }),
    '/api/v1/jobs/job-create-1': () => jobReplies[Math.min(jobCall++, jobReplies.length - 1)],
  };
}

async function submitCreate(page) {
  await page.evaluate(() => {
    window.vmList = [];
    window.currentUser = { username: 'operator', role: 'operator' };
    window.loadAll = () => {};
    window.render = () => {};
    window.showCreate();
    document.getElementById('wn').value = 'job-vm';
    window.wizGo(2);
    window.wizGo(3);
  });
  await page.waitForFunction(() => document.getElementById('wb')?.value === 'pcvnat0');
  return page.evaluate(async () => {
    await window.doCreate();
    return { toasts: window.__toasts.slice(), events: window.__events.slice() };
  });
}

test('VM 생성 접수 응답은 "접수"만 표시하고 job 이 completed 된 뒤에만 "VM 생성 완료"를 남긴다', async () => {
  await withPage(VM_MODS, async page => {
    await recordFeedback(page);
    const atAccept = await submitCreate(page);

    assert.ok(atAccept.toasts.some(t => t.includes('VM 생성 접수됨') && t.includes('job-create-1')),
      '접수 토스트에 job_id 가 보여야 한다: ' + JSON.stringify(atAccept.toasts));
    assert.ok(!atAccept.toasts.some(t => t.includes('VM 생성 완료')), '접수 시점에 완료를 표시하면 안 된다');
    assert.ok(!atAccept.events.some(e => e.includes('VM 생성 완료')), '접수 시점에 완료를 기록하면 안 된다');

    await page.waitForFunction(() => window.__events.some(e => e.includes('VM 생성 완료')), { timeout: 6000 });
    const done = await page.evaluate(() => ({ toasts: window.__toasts.slice(), events: window.__events.slice() }));
    assert.ok(done.toasts.some(t => t.startsWith('info:') && t.includes('VM 생성 완료')));
    assert.ok(done.events.some(e => e.includes('VM 생성 완료') && e.includes('job-create-1')));
  }, { routes: createRoutes(
    { accepted: true, name: 'job-vm', job_id: 'job-create-1', status: 'accepted' },
    [
      { body: { data: { job_id: 'job-create-1', status: 'running' } } },
      { body: { data: { job_id: 'job-create-1', status: 'completed' } } },
    ]) });
});

test('워커 단계 실패(job failed)는 완료로 기록하지 않고 사유를 오류로 알린다', async () => {
  await withPage(VM_MODS, async page => {
    await recordFeedback(page);
    await submitCreate(page);

    await page.waitForFunction(() => window.__toasts.some(t => t.startsWith('error:') && t.includes('zfs create failed')),
      { timeout: 6000 });
    const state = await page.evaluate(() => ({ toasts: window.__toasts.slice(), events: window.__events.slice() }));
    assert.ok(!state.toasts.some(t => t.includes('VM 생성 완료')), JSON.stringify(state.toasts));
    assert.ok(!state.events.some(e => e.includes('VM 생성 완료')), JSON.stringify(state.events));
  }, { routes: createRoutes(
    { accepted: true, name: 'job-vm', job_id: 'job-create-1', status: 'accepted' },
    [{ body: { data: { job_id: 'job-create-1', status: 'failed', detail: 'zfs create failed' } } }]) });
});

test('job_id 없는 구 서버 응답은 목록에 VM 이 나타난 뒤에만 완료를 표시한다', async () => {
  await withPage(VM_MODS, async page => {
    await recordFeedback(page);
    const atAccept = await submitCreate(page);

    assert.ok(atAccept.toasts.some(t => t.includes('VM 생성 접수됨')));
    assert.ok(!atAccept.events.some(e => e.includes('VM 생성 완료')));
    await page.waitForFunction(() => window.__events.some(e => e.includes('VM 생성 완료')), { timeout: 4000 });
  }, { routes: createRoutes({ accepted: true, name: 'job-vm', status: 'accepted' }, [{ body: {} }]) });
});

test('VNC 툴바 버튼 title 은 escapeAttr(\\xHH) 가 아니라 번역 원문이다', async () => {
  await withPage([...CORE, 'ui/modules/vm-console.js'], async page => {
    const titles = await page.evaluate(() => {
      I18N.setLang('ko');
      const frame = document.createElement('div');
      frame.id = 'vnc-frame';
      document.body.appendChild(frame);
      window.openNoVNC('127.0.0.1', 5901, 'uuid-1');
      const buttons = Array.from(frame.querySelectorAll('button'));
      if (window._vncView) window._vncView.abort.abort();
      return {
        got: buttons.map(b => b.getAttribute('title')),
        want: [t('vnc.fullscreen'), t('vnc.fit')],
      };
    });
    assert.deepEqual(titles.got, titles.want);
    assert.ok(!titles.got.some(title => title.includes('\\x')));
  });
});

test('데몬 설정 폼은 서버 값을 엔티티로 바꾸지 않고 저장도 원문 그대로 보낸다', async () => {
  const storage = {
    zvol_pool: 'tank/vms',
    image_dir: '/srv/img&co',
    iso_dirs: "/srv/iso's,/iso<2>",
    container_pool: 'tank/ctrs',
  };
  const container = { lxc_path: '/var/lib/"lxc"' };
  await withPage([...CORE, 'ui/modules/advanced.js'], async (page, { requests }) => {
    await recordFeedback(page);
    const shown = await page.evaluate(async () => {
      window.currentUser = { username: 'root', role: 'ADMIN' };
      const host = document.createElement('div');
      document.body.appendChild(host);
      await window.renderConfigMgmt(host);
      return ['cfg-zvol', 'cfg-imgdir', 'cfg-iso', 'cfg-ctrpool', 'cfg-lxcpath']
        .map(id => document.getElementById(id).value);
    });
    assert.deepEqual(shown, [storage.zvol_pool, storage.image_dir, storage.iso_dirs,
      storage.container_pool, container.lxc_path]);

    await page.evaluate(async () => {
      await window.saveStorageCfg('vm');
      await window.saveStorageCfg('ctr');
    });
    const puts = requests.filter(r => r.method === 'PUT' && r.path === '/api/v1/config/daemon')
      .map(r => r.json.key + '=' + r.json.value);
    assert.deepEqual(puts, [
      'zvol_pool=' + storage.zvol_pool,
      'image_dir=' + storage.image_dir,
      'iso_dirs=' + storage.iso_dirs,
      'container_pool=' + storage.container_pool,
      'lxc_path=' + container.lxc_path,
    ]);
  }, {
    routes: {
      '/api/v1/config/daemon': record => (record.method === 'PUT'
        ? { body: { data: { ok: true } } }
        : { body: { data: { storage, container } } }),
      '/api/v1/config/history': { body: { data: [] } },
    },
  });
});



test("디스크 크기 변경 PUT 은 new_size_gb·target 키를 보낸다", async () => {
  await withPage(VM_MODS, async (page, { requests }) => {
    await recordFeedback(page);
    await page.evaluate(async () => {
      window.vmList = [{ name: 'vm-disk', state: 'running' }];
      window.selectedVmIndex = 0;
      window.showModal(window.hwDisk());
      document.getElementById('sd-size').value = '80';
      document.getElementById('sd-path').value = ' vdb ';
      await window.doDiskResize();
    });
    const puts = requests.filter(r => r.method === 'PUT' && r.path === '/api/v1/vms/vm-disk/disk');
    assert.equal(puts.length, 1);
    assert.deepEqual(puts[0].json, { new_size_gb: 80, target: 'vdb' });
  }, { routes: { '/api/v1/vms/vm-disk/disk': { body: { data: 'resize accepted' } } } });
});
