                                                                                               
                                                                                   
                                                     
  
                                                       
   
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { withPage } from './harness.mjs';

const MODS_VM = [
  'ui/modules/ui.js', 'ui/modules/uxlib.js', 'ui/modules/endpoints.js',
  'ui/modules/filter-state.js', 'ui/modules/vm.js'
];

function seedGlobals() {
  window._DEBUG = false;
  window._L = (ko) => ko;
  window.t = key => key;
  window.vmList = [];
  window.selectedVmIndex = 0;
  window.sortField = 'name';
  window.sortDirection = 1;
  window.checkedVms = new Set();
  window.navigateTo = () => {};
  window.renderSummary = () => {};
  window.showCreate = () => {};
  window.showSnap = () => {};
}

const ADMIN_TITLE = 'VM이 없습니다';
const SCOPED_TITLE = '볼 수 있는 VM이 없습니다';

test('VM 목록 빈 상태: ADMIN·비ADMIN 테넌트 있음·VIEWER 테넌트 없음·OPERATOR 테넌트 없음 네 갈래', async () => {
  await withPage(MODS_VM, async page => {
    await page.evaluate(seedGlobals);
    const r = await page.evaluate(() => {
      const vl = document.createElement('div'); vl.id = 'vl'; document.body.appendChild(vl);
      const vc = document.createElement('span'); vc.id = 'vc'; document.body.appendChild(vc);
      const cases = {
        unknown: null,
        admin: { role: 'ADMIN', tenant: '---' },
        tenant: { role: 'VIEWER', tenant: 'ta' },
        viewerNoTenant: { role: 'VIEWER', tenant: '---' },
        operatorNoTenant: { role: 'operator', tenant: '---' },
      };
      const out = {};
      for (const [k, user] of Object.entries(cases)) {
        window.currentUser = user;
        window.render(true);                                                        
        out[k] = vl.textContent;
      }
      return out;
    });
    assert.ok(r.unknown.includes(ADMIN_TITLE), r.unknown);
    assert.ok(r.admin.includes(ADMIN_TITLE) && r.admin.includes('첫 VM을 만들어'), r.admin);
    assert.ok(r.tenant.includes(SCOPED_TITLE) && r.tenant.includes('같은 테넌트의 VM과 직접 만든 VM만 보입니다.'), r.tenant);
    assert.ok(r.viewerNoTenant.includes(SCOPED_TITLE) &&
      r.viewerNoTenant.includes('이 계정에 tenant가 없어 볼 수 있는 VM이 없습니다'), r.viewerNoTenant);
    assert.ok(r.operatorNoTenant.includes(SCOPED_TITLE) &&
      r.operatorNoTenant.includes('이 계정에 tenant가 없어 직접 만든 VM만 보입니다'), r.operatorNoTenant);
    for (const k of ['tenant', 'viewerNoTenant', 'operatorNoTenant'])
      assert.ok(!r[k].includes('첫 VM을 만들어'), k + ': ADMIN 설명이 남으면 안 된다(제목은 "볼 수 있는 VM이 없습니다"가 ADMIN 제목을 품는다)');
  }, { routes: {} });
});

test('조작 거절·자가치유 대기·대시보드 빈 표 문구 도우미', async () => {
  await withPage(MODS_VM, async page => {
    await page.evaluate(seedGlobals);
    const r = await page.evaluate(() => {
      const s = window.PCV.vmScope;
      window.currentUser = { role: 'OPERATOR', tenant: 'ta' };
      const op = {
        denied: s.actionErrorText({ code: -32006, message: 'Permission denied: operators can access only VMs they created' }),
        roleDenied: s.actionErrorText({ code: -32006, message: 'Permission denied: insufficient role for this method' }),
        other: s.actionErrorText({ code: -32001, message: 'VM not found' }),
        none: s.actionErrorText(null),
        healing: s.healingPendingEmptyText('대기 없음'),
        dash: s.dashEmptyText(),
      };
      window.currentUser = { role: 'ADMIN', tenant: '---' };
      const admin = { healing: s.healingPendingEmptyText('대기 없음'), dash: s.dashEmptyText() };
      return { op, admin };
    });
    assert.equal(r.op.denied, '이 VM은 만든 사용자만 조작할 수 있습니다');
    assert.equal(r.op.roleDenied, 'Permission denied: insufficient role for this method', '역할 부족 −32006 은 원문');
    assert.equal(r.op.other, 'VM not found');
    assert.equal(r.op.none, 'Failed');
    assert.equal(r.op.healing, '대기 항목은 관리자만 볼 수 있습니다');
    assert.equal(r.op.dash, '볼 수 있는 VM 없음');
    assert.equal(r.admin.healing, '대기 없음');
    assert.equal(r.admin.dash, '');
  }, { routes: {} });
});

                                                           
const MODS_DASH = [
  'ui/i18n.js', 'ui/modules/endpoints.js', 'ui/modules/api.js', 'ui/modules/ui.js', 'ui/modules/filter-state.js',
  'ui/modules/uxlib.js', 'ui/modules/modal-core.js', 'ui/modules/modal.js', 'ui/modules/charts.js', 'ui/modules/metrics.js',
  'ui/modules/security.js', 'ui/modules/monitor.js', 'ui/modules/vm.js', 'ui/modules/vm-console.js',
  'ui/modules/vm-lifecycle.js', 'ui/modules/vm-guest.js', 'ui/modules/container.js', 'ui/modules/network.js',
  'ui/modules/storage.js', 'ui/modules/help.js', 'ui/modules/nav.js', 'ui/modules/theme.js', 'ui/modules/accounts.js',
  'ui/modules/advanced.js', 'ui/modules/selfhealing.js', 'ui/modules/mobile.js', 'ui/app.js',
];

async function bootDash(page, port) {
  await page.evaluate(async () => {
    const html = await fetch('/ui/index.html').then(response => response.text());
    const parsed = new DOMParser().parseFromString(html, 'text/html');
    document.body.innerHTML = parsed.body.innerHTML;
  });
  for (const moduleFile of MODS_DASH)
    await page.addScriptTag({ url: `http://127.0.0.1:${port}/${moduleFile}` });
}

test('대시보드: 워크로드 표 통째 빈 표는 비ADMIN 한 행·ADMIN 행 없음·컨테이너만 있으면 행 없음, 자가치유 대기 비ADMIN 문구', async () => {
  await withPage([], async (page, { port }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await bootDash(page, port);
    const r = await page.evaluate(() => {
      const host = document.createElement('div');
      document.body.appendChild(host);
      const rowsText = () => Array.from(host.querySelectorAll('tbody tr'), tr => tr.textContent);
      window._dashVms = [];
      window._shellSlow = { raw: { ctrs: [], healing: [] } };
      window.currentUser = { role: 'VIEWER', tenant: 'ta' };
      window._renderDashWorkloads(host);
      const viewerEmpty = rowsText();
      window.currentUser = { role: 'ADMIN', tenant: '---' };
      window._renderDashWorkloads(host);
      const adminEmpty = rowsText();
      window.currentUser = { role: 'VIEWER', tenant: 'ta' };
      window._shellSlow = { raw: { ctrs: [{ name: 'c1', state: 'RUNNING' }], healing: [] } };
      window._renderDashWorkloads(host);
      const viewerCtrOnly = rowsText();
      const heal = document.createElement('div');
      document.body.appendChild(heal);
      window._renderDashHealing(heal);
      const viewerHealing = heal.textContent;
      window.currentUser = { role: 'ADMIN', tenant: '---' };
      window._renderDashHealing(heal);
      const adminHealing = heal.textContent;
      return { viewerEmpty, adminEmpty, viewerCtrOnly, viewerHealing, adminHealing };
    });
    assert.deepEqual(pageErrors, []);
    assert.deepEqual(r.viewerEmpty, ['볼 수 있는 VM 없음']);
    assert.deepEqual(r.adminEmpty, []);
    assert.equal(r.viewerCtrOnly.length, 1, '컨테이너 한 행만');
    assert.ok(!r.viewerCtrOnly[0].includes('볼 수 있는 VM 없음'), r.viewerCtrOnly[0]);
    assert.ok(r.viewerHealing.includes('대기 항목은 관리자만 볼 수 있습니다'), r.viewerHealing);
    assert.ok(r.adminHealing.includes('대기 없음') && !r.adminHealing.includes('관리자만'), r.adminHealing);
  }, { routes: { '/api/v1/metrics': { status: 404, body: null } } });
});

test('소스: 바뀐 다섯 파일에 innerHTML 계열 새 사용이 없다(L4 UI 불변식 1)', () => {
  const vm = readFileSync(new URL('../../ui/modules/vm.js', import.meta.url), 'utf8');
  const start = vm.indexOf('function _vmScopeRole() {');
  const end = vm.indexOf('PCV.vmScope = {');
  assert.ok(start > 0 && end > start);
  assert.doesNotMatch(vm.slice(start, end), /innerHTML|outerHTML|insertAdjacentHTML|document\.write/);
  const sites = [
    ['../../ui/app.js', 'PCV.vmScope.dashEmptyText()'],
    ['../../ui/app.js', "PCV.vmScope.healingPendingEmptyText('대기 없음')"],
    ['../../ui/modules/vm-lifecycle.js', 'PCV.vmScope.actionErrorText(r.error)'],
    ['../../ui/modules/monitor-vmdiag.js', "diag.data.scope === 'own'"],
    ['../../ui/modules/selfhealing.js', "PCV.vmScope.healingPendingEmptyText('대기 중인 액션 없음')"],
  ];
  for (const [f, needle] of sites) {
    const line = readFileSync(new URL(f, import.meta.url), 'utf8').split('\n').find(l => l.includes(needle));
    assert.ok(line, f + ' 에 ' + needle + ' 호출이 있어야 한다');
    assert.doesNotMatch(line, /innerHTML|outerHTML|insertAdjacentHTML|document\.write/, f);
  }
});

test('모니터 VM 진단 표: scope "own"·0행이면 "볼 수 있는 VM 지표가 없습니다"', async () => {
  await withPage(['ui/modules/ui.js', 'ui/modules/uxlib.js', 'ui/modules/filter-state.js', 'ui/modules/endpoints.js',
    'ui/modules/monitor.js', 'ui/modules/monitor-vmdiag.js'], async page => {
    const r = await page.evaluate(() => {
      window._L = (ko) => ko;
      const api = window.PCV.vmdiag || {};
      const fn = api.vmTable;
      if (typeof fn !== 'function') return { missing: Object.keys(api) };
      const mk = scope => fn({ ok: true, data: { scope, vms: [] } });
      return { own: mk('own').textContent, all: mk('all').textContent };
    });
    assert.ok(!r.missing, 'vmTable 노출 이름: ' + JSON.stringify(r.missing));
    assert.ok(r.own.includes('볼 수 있는 VM 지표가 없습니다'), r.own);
    assert.ok(r.all.includes('VM 없음') && !r.all.includes('볼 수 있는'), r.all);
  }, { routes: {} });
});
