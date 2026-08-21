                                                                                              
                                                                                  
                                                              
  
                                           
                                                                 
                                                      
                                                                
                                         
  
                                                                  
                                                                          
                                                                
   
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const MODS = ['ui/modules/ui.js', 'ui/modules/uxlib.js', 'ui/modules/filter-state.js', 'ui/modules/mobile.js'];

const POWER_DATA = {
  vms: [{ name: 'web', state: 'running' }, { name: 'db', state: 'shutoff' }],
  containers: [{ name: 'c1', state: 'RUNNING' }],
};

test('VIEWER: 모바일 전원 버튼(VM 4종+컨테이너) role-hidden(computed) — OPERATOR/ADMIN 은 노출', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate((d) => {
      const node = window.PCV.mobile.buildPower(d);
      document.getElementById('cb').appendChild(node);

      window.currentUser = { role: 'VIEWER' };
      window.applyRoleVisibility('VIEWER');
      const buttons = Array.from(node.querySelectorAll('.m-actions button'));
      const viewerDisplays = buttons.map((b) => getComputedStyle(b).display);

      window.applyRoleVisibility('OPERATOR');
      const operatorDisplays = buttons.map((b) => getComputedStyle(b).display);

      window.applyRoleVisibility('ADMIN');
      const adminDisplays = buttons.map((b) => getComputedStyle(b).display);

      return { count: buttons.length, viewerDisplays, operatorDisplays, adminDisplays };
    }, POWER_DATA);

                                                                  
    assert.equal(r.count, 4);
    assert.ok(r.viewerDisplays.every((d) => d === 'none'), r.viewerDisplays.join(','));
    assert.ok(r.operatorDisplays.every((d) => d !== 'none'), r.operatorDisplays.join(','));
    assert.ok(r.adminDisplays.every((d) => d !== 'none'), r.adminDisplays.join(','));
  });
});

test('모바일 RBAC: ACK·새 VM은 OPERATOR, 자가치유 승인/거절은 ADMIN부터 노출', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate(() => {
      const home = window.PCV.mobile.buildHome({ vms: [], containers: [], alerts: [], hostCpu: 0, hostMem: 0 });
      const alerts = window.PCV.mobile.buildAlerts({
        alerts: [{ alert_id: 1, severity: 'warn', message: 'm', acknowledged: false }],
        filter: [],
      });
      const healing = window.PCV.mobile.buildHealing({
        mode: 'dry_run',
        pending: [{ id: 9, policy: 'p', action: 'a', reason: 'r', ts: 1 }],
        history: [],
      });
      const cb = document.getElementById('cb');
      cb.appendChild(home);
      cb.appendChild(alerts);
      cb.appendChild(healing);

      const newVmBtn = Array.from(home.querySelectorAll('button')).find((b) => b.textContent.includes('New VM') || b.textContent.includes('새 VM'));
      const ackBtn = alerts.querySelector('[data-ack-id="1"]');
      const healBtns = Array.from(healing.querySelectorAll('.m-heal-pending button'));

      function displays() {
        return {
          newVm: getComputedStyle(newVmBtn).display,
          ack: getComputedStyle(ackBtn).display,
          heal: healBtns.map((b) => getComputedStyle(b).display),
        };
      }

      window.currentUser = { role: 'VIEWER' };
      window.applyRoleVisibility('VIEWER');
      const viewer = displays();

      window.applyRoleVisibility('OPERATOR');
      const operator = displays();

      window.applyRoleVisibility('ADMIN');
      const admin = displays();

      return { healCount: healBtns.length, viewer, operator, admin };
    });

    assert.equal(r.healCount, 2);           

    assert.equal(r.viewer.newVm, 'none');
    assert.equal(r.viewer.ack, 'none');
    assert.ok(r.viewer.heal.every((d) => d === 'none'), r.viewer.heal.join(','));

    assert.notEqual(r.operator.newVm, 'none');
    assert.notEqual(r.operator.ack, 'none');
    assert.ok(r.operator.heal.every((d) => d === 'none'), r.operator.heal.join(','));

    assert.notEqual(r.admin.newVm, 'none');
    assert.notEqual(r.admin.ack, 'none');
    assert.ok(r.admin.heal.every((d) => d !== 'none'), r.admin.heal.join(','));
  });
});

test('_paint() 말미 재적용 — currentUser 부재(로그아웃) 시 TypeError 없이 통과', async () => {
  await withPage(MODS, async (page) => {
                                                                    
                                                               
                                                                 
    const result = await page.evaluate(async () => {
      window.__mobileErrors = [];
      window.addEventListener('error', (e) => window.__mobileErrors.push(String((e && e.error) || (e && e.message))));
      window.addEventListener('unhandledrejection', (e) => window.__mobileErrors.push(String(e.reason)));
      window.authToken = null;
      window.currentUser = null;
                                                                       
                                                                  
                                                                 
                                                                    
                              
      var scr = document.createElement('div');
      scr.id = 'm-screen';
      document.body.appendChild(scr);
      window.PCV.mobile.showTab('power');                                             
      await new Promise((resolve) => setTimeout(resolve, 50));
      return {
        errors: window.__mobileErrors,
        emptyShown: !!document.querySelector('#m-screen .mscreen-empty'),
      };
    });
    assert.deepEqual(result.errors, []);
    assert.equal(result.emptyShown, true);
  });
});
