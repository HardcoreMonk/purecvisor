                                                                                       
                                                                       
                                                              
  
                                   
  
                                                                                        
  
                                                                  
                                                       
                                                                      
                                                                       
                                                       
                          
  
                                                   
   
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const MODS_VM = [
  'ui/modules/ui.js', 'ui/modules/uxlib.js', 'ui/modules/endpoints.js',
  'ui/modules/filter-state.js', 'ui/modules/vm.js'
];
                                            
                                            
const NAV_SCRIPT = '/ui/modules/nav.js';

const VMS = [
  { name: 'vm-alpha', state: 'running', running: 1, vcpu: 2, mem: 2048 },
  { name: 'vm-beta', state: 'shutoff', running: 0, vcpu: 1, mem: 1024 }
];

                                                 
                                               
function seedGlobals(vms) {
  window._DEBUG = false;
  window._L = (ko) => ko;
  window.t = key => key;
  window.vmList = vms;
  window.selectedVmIndex = 0;
  window.sortField = 'name';
  window.sortDirection = 1;
  window.checkedVms = new Set();
  window.navigateTo = () => {};
  window.renderSummary = () => {};
  window.showCreate = () => {};
  window.showSnap = () => {};
}

test('R1-F8: #vf 필터 값이 탭 전환 재렌더 후에도 보존된다', async () => {
  await withPage(MODS_VM, async page => {
    await page.evaluate(seedGlobals, VMS);
    const r = await page.evaluate(() => {
      const cb = document.getElementById('cb');
      window.currentTab = 'summary';
      window.renderVmScreen(cb, window.vmList[0], 'summary');

      const vf = document.getElementById('vf');
      vf.value = 'beta';
      vf.dispatchEvent(new Event('input'));
      const afterTypeRows = document.querySelectorAll('#vl .vi').length;

                                                 
      window.currentTab = 'console';
      window.renderVmScreen(cb, window.vmList[0], 'console');
      const vf2 = document.getElementById('vf');
      return {
        sameNode: vf2 === vf,
        value: vf2.value,
        afterTypeRows,
        afterRerenderRows: document.querySelectorAll('#vl .vi').length
      };
    });
    assert.equal(r.sameNode, false, '재렌더는 #vf 를 새 노드로 만든다(보존 대상이 맞는지 확인)');
    assert.equal(r.value, 'beta', '재렌더 후에도 필터 입력값이 유지된다');
    assert.equal(r.afterTypeRows, 1, '입력 직후 목록이 걸러진다');
    assert.equal(r.afterRerenderRows, 1, '재렌더 후에도 같은 필터가 적용된 목록이다');
  }, { routes: {} });
});

test('R2-F4: renderContent 말미 role 재적용 — VIEWER 는 vm pagehead 액션을 못 본다', async () => {
  await withPage(MODS_VM, async page => {
    await page.evaluate(seedGlobals, VMS);
    await page.evaluate(() => {
      window.destroyAllCharts = () => {};
      PCV.filterEditionItems = items => items;
    });
    await page.addScriptTag({ url: NAV_SCRIPT });
    const r = await page.evaluate(async () => {
                                                                
      window.matchMedia = () => ({ matches: true, addEventListener() {}, removeEventListener() {} });
                                                                
                                                     
                                                                               
      window.currentTab = 'summary';
      const read = () => Array.from(document.querySelectorAll('#cb .pagehead-actions [data-role]'))
        .map(n => getComputedStyle(n).display);

      window.currentUser = { role: 'VIEWER' };
      renderContent();
      const viewer = read();

      window.currentUser = { role: 'ADMIN' };
      renderContent();
      const admin = read();

      return { viewer, admin };
    });
    assert.deepEqual(r.viewer, ['none', 'none'], 'VIEWER 는 +새 VM / 스냅샷 액션이 숨는다');
    assert.ok(r.admin.every(d => d !== 'none'), 'ADMIN 은 그대로 보인다: ' + r.admin.join(','));
  }, { routes: {} });
});

                                                                   
                                                           
                                                         
test('applyRoleVisibility: 인라인 display 센티넬 보존(#bbtn 계열) + 비허용 role-hidden', async () => {
  await withPage(MODS_VM, async (page) => {
    const r = await page.evaluate(() => {
      const b = document.createElement('button');
      b.id = 'bbtn-probe';
      b.setAttribute('data-role', 'OPERATOR,ADMIN');
      b.style.display = 'none';                             
      document.body.appendChild(b);
      window.applyRoleVisibility('ADMIN');
      const allowedDisplay = getComputedStyle(b).display;
      const allowedClass = b.classList.contains('role-hidden');
      window.applyRoleVisibility('VIEWER');
      const deniedClass = b.classList.contains('role-hidden');
      b.style.display = 'inline';                                        
      const deniedDisplayWithInline = b.className;
      return { allowedDisplay, allowedClass, deniedClass, deniedDisplayWithInline };
    });
    assert.equal(r.allowedDisplay, 'none');                                     
    assert.equal(r.allowedClass, false);
    assert.equal(r.deniedClass, true);
    assert.ok(r.deniedDisplayWithInline.includes('role-hidden'));
  });
});
