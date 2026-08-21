                                                                                            
                                                                                       
                                                                     
  
                                            
  
                  
                                                                    
                                                     
                                                          
                                                              
   
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

                                                                               
const MODS = [
  'ui/i18n.js',
  'ui/modules/endpoints.js',
  'ui/modules/api.js',
  'ui/modules/ui.js',
  'ui/modules/filter-state.js',
  'ui/modules/uxlib.js',
  'ui/modules/modal-core.js',
  'ui/modules/modal.js',
  'ui/modules/totp.js',
];

                                                                        
                                                                     
const MODS_ACCOUNTS = [...MODS, 'ui/modules/accounts.js'];

async function paintCard(page) {
  await page.evaluate(() => {
    window.authToken = 'test';
    const host = document.createElement('div');
    host.id = 'totp-card-host';
    document.body.appendChild(host);
    window.PCV.totp.renderSettingsCard(host);
  });
  await page.waitForFunction(
    () => { const h = document.getElementById('totp-card-host'); return h && h.querySelector('.pill'); },
    { timeout: 3000 }
  );
  return page.evaluate(() => {
    const h = document.getElementById('totp-card-host');
    const pill = h.querySelector('.pill');
    return {
      pillText: pill.textContent,
      pillClass: pill.className,
      text: h.textContent,
      buttons: [...h.querySelectorAll('button')].map(b => b.textContent),
    };
  });
}

test('설정 카드: confirmed 는 활성 배지 + 잔여 복구코드 + 해제/재발급', async () => {
  await withPage(MODS, async (page) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));

    const out = await paintCard(page);

    assert.deepEqual(pageErrors, []);
    assert.equal(out.pillText, '활성', '활성 배지');
    assert.ok(out.pillClass.includes('pill-ok'), 'ok 톤 배지');
    assert.ok(out.text.includes('7'), '잔여 복구코드 7 표시');
    assert.ok(out.buttons.length >= 2, '해제·재발급 버튼');
  }, { routes: { '/api/v1/auth/totp/status': { body: { data: { enrolled: true, confirmed: true, recovery_remaining: 7 } } } } });
});

test('설정 카드: 미확정은 비활성 배지 + 활성화 버튼', async () => {
  await withPage(MODS, async (page) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));

    const out = await paintCard(page);

    assert.deepEqual(pageErrors, []);
    assert.equal(out.pillText, '비활성', '비활성 배지');
    assert.ok(out.pillClass.includes('pill-idle'), 'idle 톤 배지');
    assert.ok(out.buttons.some(b => b === '활성화'), '활성화 버튼');
  }, { routes: { '/api/v1/auth/totp/status': { body: { data: { enrolled: false, confirmed: false, recovery_remaining: 0 } } } } });
});

test('ADMIN 리셋 버튼: 확인 후 대상 username 으로 reset 을 POST 한다', async () => {
  await withPage(MODS, async (page, { requests }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));

    const done = await page.evaluate(async () => {
      window.authToken = 'test';
      window.customConfirm = () => Promise.resolve(true);                 
      window.toast = () => {};                                      
      window.addEvt = () => {};
      let flag = false;
      const btn = window.PCV.totp.adminResetButton('bob', () => { flag = true; });
      document.body.appendChild(btn);
      btn.click();
      await new Promise(resolve => setTimeout(resolve, 250));
      return flag;
    });

    assert.deepEqual(pageErrors, []);
    const resetReq = requests.find(r => r.path === '/api/v1/auth/totp/reset');
    assert.ok(resetReq, 'reset 이 호출돼야 한다');
    assert.equal(resetReq.json.username, 'bob', '대상 username 전달');
    assert.equal(done, true, 'onDone 콜백 실행');
  }, { routes: { '/api/v1/auth/totp/reset': { body: { data: { ok: true } } } } });
});

                                                           
                                                        
                                                                  
test('복구코드 모달: ESC·백드롭으로 안 닫히고 "저장했습니다" 버튼으로만 닫힌다', async () => {
  await withPage(MODS, async (page) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));

    const out = await page.evaluate(async () => {
      window.PCV.totp.showRecoveryCodes(['aaaa-1111', 'bbbb-2222', 'cccc-3333']);
      const dlg = window.PCV.modalCore.currentDialog();
      const opened = !!dlg && dlg.isConnected && dlg.open;
      const codesShown = !!dlg && dlg.textContent.includes('aaaa-1111');

                                                                           
      dlg.dispatchEvent(new Event('cancel', { cancelable: true }));
      await Promise.resolve(); await Promise.resolve();
      const afterEsc = dlg.isConnected && window.PCV.modalCore.currentDialog() === dlg;

                                                            
      dlg.dispatchEvent(new MouseEvent('click', { bubbles: true, clientX: 0, clientY: 0 }));
      await Promise.resolve(); await Promise.resolve();
      const afterBackdrop = dlg.isConnected && window.PCV.modalCore.currentDialog() === dlg;

                                  
      dlg.querySelector('.btn-g').click();
      await Promise.resolve(); await Promise.resolve();
      const afterSave = dlg.isConnected;

      return { opened, codesShown, afterEsc, afterBackdrop, afterSave,
        dialogsLeft: document.querySelectorAll('dialog[open]').length };
    });

    assert.deepEqual(pageErrors, []);
    assert.equal(out.opened, true, '복구코드 모달이 열려야 한다');
    assert.equal(out.codesShown, true, '복구코드가 표시돼야 한다');
    assert.equal(out.afterEsc, true, 'ESC 로는 닫히지 않아야 한다');
    assert.equal(out.afterBackdrop, true, '백드롭 클릭으로는 닫히지 않아야 한다');
    assert.equal(out.afterSave, false, '"저장했습니다" 버튼으로는 닫혀야 한다');
    assert.equal(out.dialogsLeft, 0, '저장 후 열린 dialog 가 없어야 한다');
  });
});

                                                 
                                             
                                         
test('복구코드 모달: 지연 closeModal()은 noDismiss를 우회하지 못한다', async () => {
  await withPage(MODS, async (page) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));

    const out = await page.evaluate(async () => {
      window.PCV.totp.showRecoveryCodes(['aaaa-1111']);
      const dlg = window.PCV.modalCore.currentDialog();

      await new Promise(resolve => {
        setTimeout(() => { window.closeModal(); resolve(); }, 0);
      });
      await Promise.resolve();
      const afterDelayedGenericClose =
        dlg.isConnected && dlg.open && window.PCV.modalCore.currentDialog() === dlg;

      dlg.querySelector('.btn-g').click();
      await Promise.resolve();

      return {
        afterDelayedGenericClose,
        afterOwnerClose: dlg.isConnected,
        dialogsLeft: document.querySelectorAll('dialog[open]').length
      };
    });

    assert.deepEqual(pageErrors, []);
    assert.equal(out.afterDelayedGenericClose, true,
      '지연 범용 closeModal은 noDismiss 복구코드 모달을 닫으면 안 된다');
    assert.equal(out.afterOwnerClose, false,
      '모달 소유자의 저장 버튼은 closeDialog(dlg)로 닫아야 한다');
    assert.equal(out.dialogsLeft, 0, '명시 종료 후 열린 dialog가 없어야 한다');
  });
});

                                                            
test('기본 모달(noDismiss 미지정)은 ESC·백드롭으로 그대로 닫힌다', async () => {
  await withPage(MODS, async (page) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));

    const out = await page.evaluate(async () => {
      const el = window.PCV.uxlib.el;
      window.showModal([el('h2', null, 'Plain'), el('p', null, 'body')]);
      const d1 = window.PCV.modalCore.currentDialog();
      d1.dispatchEvent(new Event('cancel', { cancelable: true }));
      await Promise.resolve(); await Promise.resolve();
      const escClosed = !d1.isConnected;

      window.showModal([el('h2', null, 'Plain2')]);
      const d2 = window.PCV.modalCore.currentDialog();
      d2.dispatchEvent(new MouseEvent('click', { bubbles: true, clientX: 0, clientY: 0 }));
      await Promise.resolve(); await Promise.resolve();
      const backdropClosed = !d2.isConnected;

      return { escClosed, backdropClosed };
    });

    assert.deepEqual(pageErrors, []);
    assert.equal(out.escClosed, true, '기본 모달은 ESC 로 닫혀야 한다');
    assert.equal(out.backdropClosed, true, '기본 모달은 백드롭으로 닫혀야 한다');
  });
});

                                                                          
                                                                          
test('accounts TOTP 컬럼: confirmed=true 는 활성(ok), 미확정/부재는 비활성(idle)', async () => {
  const USERS = [
    { username: 'bob',   role: 'operator', tenant: null, totp: { enrolled: true, confirmed: true } },
    { username: 'carol', role: 'viewer',   tenant: null, totp: { enrolled: true, confirmed: false } },
    { username: 'dave',  role: 'viewer',   tenant: null }                         
  ];
  await withPage(MODS_ACCOUNTS, async (page) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));

    await page.evaluate(() => {
      window.authToken = 'test';
      window.currentUser = { role: 'admin' };                                      
      window.renderAccounts(document.getElementById('cb'));
    });
    await page.waitForFunction(
      () => document.querySelectorAll('#acct-table tbody tr').length === 3,
      { timeout: 3000 }
    );

    const rows = await page.$$eval('#acct-table tbody tr', trs => trs.map(tr => {
      const cells = tr.querySelectorAll('td');
      const pill = cells[3].querySelector('.pill');                                                 
      return { name: cells[0].textContent,
        cls: pill ? pill.className : null, text: pill ? pill.textContent : null };
    }));

    assert.deepEqual(pageErrors, []);
    const bob = rows.find(r => r.name === 'bob');
    assert.ok(bob.cls.includes('pill-ok'), 'confirmed=true → ok 톤');
    assert.equal(bob.text, '활성', 'confirmed=true → 활성 배지');
    const carol = rows.find(r => r.name === 'carol');
    assert.ok(carol.cls.includes('pill-idle'), 'confirmed=false → idle 톤');
    assert.equal(carol.text, '비활성', 'confirmed=false → 비활성 배지');
    const dave = rows.find(r => r.name === 'dave');
    assert.ok(dave.cls.includes('pill-idle'), 'totp 부재 → 폴백 idle');
    assert.equal(dave.text, '비활성', 'totp 부재 → 비활성 배지');
  }, { routes: {
    '/api/v1/auth/users': { body: { data: USERS } },
    '/api/v1/auth/totp/status': { body: { data: { enrolled: false, confirmed: false, recovery_remaining: 0 } } }
  } });
});
