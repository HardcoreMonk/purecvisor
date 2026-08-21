                                                                                                   
                                                         
                                                                    
  
                                   
                                                             
                                                           
   
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const CORE_MODAL = [
  'ui/i18n.js',
  'ui/modules/endpoints.js', 'ui/modules/api.js', 'ui/modules/ui.js',
  'ui/modules/filter-state.js', 'ui/modules/uxlib.js',
  'ui/modules/modal-core.js', 'ui/modules/modal.js', 'ui/modules/totp.js'
];
const COMMAND_MODS = [...CORE_MODAL, 'ui/modules/help.js', 'ui/modules/nav.js'];
                                                                   
                                
const ISO_MODS = [...COMMAND_MODS, 'ui/modules/vm-lifecycle.js'];

async function bootCommand(page) {
  await page.evaluate(() => {
    I18N.setLang('ko');
    window.authToken = 'test-token';
    window.currentUser = { role: 'admin' };
    window.pcvClusterEnabled = false;
    window.navigateTo = () => true;
    window.vmList = [
      { name: 'web-01', state: 'running' },
      { name: 'db-01', state: 'shutoff' }
    ];
    window._shellSlow = { raw: {
      ctrs: [{ name: 'proxy-lxc', state: 'RUNNING', ip_addr: '10.20.30.41' }],
      pools: [{ name: 'edgepool', health: 'ONLINE' }],
      fleet: [
        { name: 'web-01', ip: '10.20.30.55' },
        { name: 'db-01', ip: '10.20.30.56' }
      ],
      networks: [{ name: 'pcvbr0', mode: 'bridge' }]
    } };
  });
}

test('command palette는 이름 있는 combobox/listbox이며 선택과 로컬 icon을 노출한다', async () => {
  await withPage(COMMAND_MODS, async page => {
    await bootCommand(page);
    const before = await page.evaluate(() => {
      openCmdPalette();
      const input = document.getElementById('cmd-input');
      const list = document.getElementById('cmd-list');
      const selected = list.querySelectorAll('[role="option"][aria-selected="true"]');
      return {
        dialogLabel: document.querySelector('dialog[open]').getAttribute('aria-label'),
        inputRole: input.getAttribute('role'),
        listRole: list.getAttribute('role'),
        active: input.getAttribute('aria-activedescendant'),
        selected: selected.length,
        optionCount: list.querySelectorAll('[role="option"]').length,
        iconHrefs: [...list.querySelectorAll('.cmd-item-icon use')].map(use => use.getAttribute('href')),
        help: document.getElementById('cmd-help').textContent,
        rawEmoji: /[\u{1F300}-\u{1FAFF}]/u.test(document.getElementById('cmd-palette').textContent)
      };
    });

    assert.equal(before.dialogLabel, '명령·객체·IP 검색');
    assert.equal(before.inputRole, 'combobox');
    assert.equal(before.listRole, 'listbox');
    assert.ok(before.optionCount > 2);
    assert.equal(before.selected, 1);
    assert.equal(before.active, 'cmd-option-0');
    assert.ok(before.iconHrefs.every(href => /^vendor\/coolicons\/coolicons\.svg#ci-/.test(href)));
    assert.match(before.help, /Enter/);
    assert.match(before.help, /Esc/);
    assert.equal(before.rawEmoji, false);

    await page.keyboard.press('ArrowDown');
    const after = await page.evaluate(() => {
      const input = document.getElementById('cmd-input');
      const selected = document.querySelectorAll('#cmd-list [aria-selected="true"]');
      return {
        active: input.getAttribute('aria-activedescendant'),
        selected: selected.length,
        selectedId: selected[0]?.id,
        inputFocused: document.activeElement === input
      };
    });
    assert.deepEqual(after, {
      active: 'cmd-option-1',
      selected: 1,
      selectedId: 'cmd-option-1',
      inputFocused: true
    });

                                                         
                                                              
    for (let index = 0; index < 16; index += 1) await page.keyboard.press('ArrowDown');
    const scrolled = await page.evaluate(() => {
      const list = document.getElementById('cmd-list');
      const selected = list.querySelector('[aria-selected="true"]');
      const listRect = list.getBoundingClientRect();
      const selectedRect = selected.getBoundingClientRect();
      return {
        active: document.getElementById('cmd-input').getAttribute('aria-activedescendant'),
        scrollTop: list.scrollTop,
        visible: selectedRect.top >= listRect.top && selectedRect.bottom <= listRect.bottom
      };
    });
    assert.equal(scrolled.active, 'cmd-option-17');
    assert.ok(scrolled.scrollTop > 0, `scrollTop=${scrolled.scrollTop}`);
    assert.equal(scrolled.visible, true);
  });
});

test('키보드 도움말은 고유 이름을 갖고 480px에서 한 열로 배치된다', async () => {
  await withPage(COMMAND_MODS, async page => {
    await page.setViewport({ width: 480, height: 900 });
    await bootCommand(page);
    const result = await page.evaluate(() => {
      toggleKbdHelp();
      const dialog = document.querySelector('dialog.kbd-help[open]');
      const labelledBy = dialog.getAttribute('aria-labelledby');
      const grid = dialog.querySelector('.kbd-grid');
      const box = dialog.querySelector('.kbd-box').getBoundingClientRect();
      return {
        name: document.getElementById(labelledBy)?.textContent,
        columns: getComputedStyle(grid).gridTemplateColumns.split(' ').filter(Boolean).length,
        left: Math.round(box.left),
        right: Math.round(box.right),
        documentOverflow: document.documentElement.scrollWidth > innerWidth
      };
    });
    assert.equal(result.name, '키보드 단축키');
    assert.equal(result.columns, 1);
    assert.ok(result.left >= 16, `left inset=${result.left}`);
    assert.ok(result.right <= 464, `right inset=${480 - result.right}`);
    assert.equal(result.documentOverflow, false);
  });
});

test('ISO browser는 키보드 선택 가능한 button·로컬 icon·좁은 폭 inset을 사용한다', async () => {
  await withPage(ISO_MODS, async page => {
    await page.setViewport({ width: 480, height: 900 });
    await page.evaluate(() => {
      I18N.setLang('ko');
      window.authToken = 'test-token';
      browseISO();
    });
    await page.waitForFunction(() => document.querySelectorAll('.iso-browser-file').length === 2);

    const before = await page.evaluate(() => {
      const dialog = document.querySelector('dialog.iso-browser[open]');
      const box = dialog.querySelector('.iso-browser-box').getBoundingClientRect();
      const files = [...dialog.querySelectorAll('.iso-browser-file')];
      files[0].focus();
      const labelledBy = dialog.getAttribute('aria-labelledby');
      return {
        name: document.getElementById(labelledBy)?.textContent.trim(),
        tags: files.map(file => file.tagName),
        minHeights: files.map(file => file.getBoundingClientRect().height),
        iconHrefs: [...dialog.querySelectorAll('.ci-icon use')].map(use => use.getAttribute('href')),
        rawEmoji: /[\u{1F300}-\u{1FAFF}]/u.test(dialog.textContent),
        pathLabel: document.getElementById('iso-manual-path').getAttribute('aria-label'),
        left: Math.round(box.left),
        right: Math.round(box.right),
        focused: document.activeElement === files[0],
        documentOverflow: document.documentElement.scrollWidth > innerWidth
      };
    });

    assert.equal(before.name, 'ISO 이미지 탐색기');
    assert.deepEqual(before.tags, ['BUTTON', 'BUTTON']);
    assert.ok(before.minHeights.every(height => height >= 40));
    assert.ok(before.iconHrefs.every(href => /^vendor\/coolicons\/coolicons\.svg#ci-/.test(href)));
    assert.equal(before.rawEmoji, false);
    assert.equal(before.pathLabel, 'ISO 직접 경로');
    assert.ok(before.left >= 12, `left inset=${before.left}`);
    assert.ok(before.right <= 468, `right inset=${480 - before.right}`);
    assert.equal(before.focused, true);
    assert.equal(before.documentOverflow, false);

    await page.keyboard.press('Enter');
    await page.waitForFunction(() => !document.querySelector('dialog.iso-browser[open]'));
    assert.equal(await page.evaluate(() => document.querySelectorAll('dialog.iso-browser[open]').length), 0);
  }, {
    routes: {
      '/api/v1/iso': {
        status: 200,
        body: { data: [
          { dir: '/pcvpool/iso', path: '/pcvpool/iso/ubuntu.iso', name: 'ubuntu.iso', size_mb: 2048 },
          { dir: '/pcvpool/images', path: '/pcvpool/images/rescue.img', name: 'rescue.img', size_mb: 768 }
        ] }
      }
    }
  });
});

test('공용 파괴 확인은 취소에 초기 focus를 두고 결과 계약을 유지한다', async () => {
  await withPage(CORE_MODAL, async page => {
    const cancel = await page.evaluate(async () => {
      const pending = customConfirm('Local VPC 삭제', '이 작업은 되돌릴 수 없습니다.');
      const dialog = document.querySelector('dialog[open]');
      const focussed = document.activeElement?.hasAttribute('data-confirm-cancel');
      const labelledBy = dialog.getAttribute('aria-labelledby');
      const name = document.getElementById(labelledBy)?.textContent;
      dialog.querySelector('[data-confirm-cancel]').click();
      return { focussed, name, result: await pending };
    });
    assert.deepEqual(cancel, { focussed: true, name: 'Local VPC 삭제', result: false });

    const confirm = await page.evaluate(async () => {
      const pending = customConfirm('Local VPC 삭제', '이 작업은 되돌릴 수 없습니다.');
      document.querySelector('[data-confirm-accept]').click();
      return pending;
    });
    assert.equal(confirm, true);
  });
});

test('표준 Modal.show와 HN.card noDismiss dialog는 모두 고유 이름을 갖는다', async () => {
  await withPage(CORE_MODAL, async page => {
    const result = await page.evaluate(() => {
      I18N.setLang('ko');
      PCV.totp.showRecoveryCodes(['aaaa-1111', 'bbbb-2222']);
      const recoveryDialog = PCV.modalCore.currentDialog();
      const recoveryLabelledBy = recoveryDialog.getAttribute('aria-labelledby');
      const recovery = {
        name: document.getElementById(recoveryLabelledBy)?.textContent,
        generic: recoveryDialog.getAttribute('aria-label'),
        noDismiss: recoveryDialog._pcvNoDismiss
      };
      PCV.modalCore.closeDialog(recoveryDialog);

      Modal.showRegister();
      const standardDialog = PCV.modalCore.currentDialog();
      const standardLabelledBy = standardDialog.getAttribute('aria-labelledby');
      return {
        recovery,
        standard: {
          name: document.getElementById(standardLabelledBy)?.textContent,
          generic: standardDialog.getAttribute('aria-label'),
          closeLabel: standardDialog.querySelector('#pcv-modal-close-x')?.getAttribute('aria-label')
        }
      };
    });
    assert.deepEqual(result, {
      recovery: { name: '복구코드', generic: null, noDismiss: true },
      standard: { name: '회원가입', generic: null, closeLabel: '닫기' }
    });
  });
});

test('중첩 dialog를 닫으면 부모 입력과 focus가 복원되고 마지막 종료는 trigger로 돌아간다', async () => {
  await withPage(CORE_MODAL, async page => {
    const result = await page.evaluate(async () => {
      const el = PCV.uxlib.el;
      const trigger = el('button', { id: 'modal-trigger' }, '편집');
      document.body.appendChild(trigger);
      trigger.focus();

      const input = el('input', { id: 'parent-value', value: '보존할 값' });
      showModal([el('h2', null, '부모 설정'), input]);
      input.focus();
      const confirmPending = customConfirm('변경 확인', '부모 입력은 유지되어야 합니다.');
      const stacked = document.querySelectorAll('dialog[open]').length;
      document.querySelector('[data-confirm-cancel]').click();
      const confirmResult = await confirmPending;
      await Promise.resolve();

      const parent = PCV.modalCore.currentDialog();
      const parentValue = document.getElementById('parent-value')?.value;
      const parentFocused = document.activeElement === input;
      const parentIsCurrent = parent?.getAttribute('aria-labelledby') === 'pcv-dlg-title-1';
      closeModal();
      await Promise.resolve();

      return {
        stacked,
        confirmResult,
        parentValue,
        parentFocused,
        parentIsCurrent,
        dialogsLeft: document.querySelectorAll('dialog[open]').length,
        triggerFocused: document.activeElement === trigger
      };
    });
    assert.deepEqual(result, {
      stacked: 2,
      confirmResult: false,
      parentValue: '보존할 값',
      parentFocused: true,
      parentIsCurrent: true,
      dialogsLeft: 0,
      triggerFocused: true
    });
  });
});
