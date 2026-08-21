                                                                                                  
                                                                                               
                                                                
  
                                       
  
                  
                                                                                
                                                                               
                                                                           
  
               
                                                                             
                                                                               
   
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage, CORE } from './harness.mjs';

const ACTUAL_MODAL = [...CORE, 'ui/modules/modal-core.js'];

test('Esc settles a custom confirmation and releases the pending navigation slot', async () => {
  await withPage(ACTUAL_MODAL, async (page) => {
    const result = await page.evaluate(async () => {
      const commits = [];
      PCV.ui.setNavigationBlocker(() => (
        PCV.ui.customConfirm('이동', '계속하시겠습니까?')
      ));
      const first = PCV.ui.requestNavigation(
        'storage',
        () => commits.push('storage')
      );
      const firstDialog = PCV.modalCore.currentDialog();
      firstDialog.dispatchEvent(new Event('cancel', { cancelable: true }));
      await Promise.resolve();
      await Promise.resolve();

      const second = PCV.ui.requestNavigation(
        'host',
        () => commits.push('host')
      );
      const secondDialog = PCV.modalCore.currentDialog();
      if (secondDialog) {
        secondDialog.querySelector('[data-confirm-accept]')?.click();
        await Promise.resolve();
        await Promise.resolve();
      }
      return {
        first,
        second,
        firstClosed: !firstDialog.isConnected,
        secondOpened: Boolean(secondDialog),
        commits
      };
    });
    assert.deepEqual(result, {
      first: false,
      second: false,
      firstClosed: true,
      secondOpened: true,
      commits: ['host']
    });
  });
});

test('backdrop close settles a custom confirmation and permits a new confirmation', async () => {
  await withPage(ACTUAL_MODAL, async (page) => {
    const result = await page.evaluate(async () => {
      PCV.ui.setNavigationBlocker(() => (
        PCV.ui.customConfirm('이동', '계속하시겠습니까?')
      ));
      const commits = [];
      PCV.ui.requestNavigation('storage', () => commits.push('storage'));
      const firstDialog = PCV.modalCore.currentDialog();
      firstDialog.dispatchEvent(new MouseEvent('click', {
        bubbles: true,
        clientX: 0,
        clientY: 0
      }));
      await Promise.resolve();
      await Promise.resolve();

      PCV.ui.requestNavigation('host', () => commits.push('host'));
      const secondDialog = PCV.modalCore.currentDialog();
      if (secondDialog) {
        secondDialog.querySelector('[data-confirm-cancel]')?.click();
        await Promise.resolve();
        await Promise.resolve();
      }
      return {
        firstClosed: !firstDialog.isConnected,
        secondOpened: Boolean(secondDialog),
        commits
      };
    });
    assert.deepEqual(result, {
      firstClosed: true,
      secondOpened: true,
      commits: []
    });
  });
});

test('custom confirmation buttons retain true and false result contracts', async () => {
  await withPage(ACTUAL_MODAL, async (page) => {
    const result = await page.evaluate(async () => {
      const accepted = PCV.ui.customConfirm('확인', '승인');
      PCV.modalCore.currentDialog()
        .querySelector('.btn-r')
        .click();
      const acceptedValue = await accepted;

      const cancelled = PCV.ui.customConfirm('확인', '취소');
      PCV.modalCore.currentDialog()
        .querySelector('.btn:not(.btn-r)')
        .click();
      const cancelledValue = await cancelled;
      return { acceptedValue, cancelledValue };
    });
    assert.deepEqual(result, {
      acceptedValue: true,
      cancelledValue: false
    });
  });
});

test('navigation stays synchronous without a blocker', async () => {
  await withPage(CORE, async (page) => {
    const result = await page.evaluate(() => {
      const events = [];
      const accepted = PCV.ui.requestNavigation('dashboard', () => events.push('commit'));
      events.push('return');
      return { accepted, events };
    });
    assert.deepEqual(result, { accepted: true, events: ['commit', 'return'] });
  });
});

test('async blocker keeps the page on cancel and commits once on approval', async () => {
  await withPage(CORE, async (page) => {
    const result = await page.evaluate(async () => {
      const commits = [];
      let resolveDecision;
      PCV.ui.setNavigationBlocker(() => new Promise(resolve => {
        resolveDecision = resolve;
      }));
      const first = PCV.ui.requestNavigation('dashboard', () => commits.push('dashboard'));
      const duplicate = PCV.ui.requestNavigation('storage', () => commits.push('storage'));
      resolveDecision(false);
      await Promise.resolve();
      await Promise.resolve();

      PCV.ui.setNavigationBlocker(() => Promise.resolve(true));
      const approved = PCV.ui.requestNavigation('dashboard', () => commits.push('dashboard'));
      await Promise.resolve();
      await Promise.resolve();
      return { first, duplicate, approved, commits };
    });
    assert.deepEqual(result, {
      first: false,
      duplicate: false,
      approved: false,
      commits: ['dashboard']
    });
  });
});

test('blocker exceptions and rejections preserve the blocker and current page', async () => {
  await withPage(CORE, async (page) => {
    const result = await page.evaluate(async () => {
      const commits = [];
      let calls = 0;
      PCV.ui.setNavigationBlocker(() => {
        calls++;
        if (calls === 1) throw new Error('prompt failed');
        return Promise.reject(new Error('prompt rejected'));
      });
      const thrown = PCV.ui.requestNavigation('dashboard', () => commits.push('thrown'));
      const rejected = PCV.ui.requestNavigation('storage', () => commits.push('rejected'));
      await Promise.resolve();
      await Promise.resolve();
      const retried = PCV.ui.requestNavigation('host', () => commits.push('retried'));
      await Promise.resolve();
      await Promise.resolve();
      return { thrown, rejected, retried, calls, commits };
    });
    assert.deepEqual(result, {
      thrown: false,
      rejected: false,
      retried: false,
      calls: 3,
      commits: []
    });
  });
});

test('hash navigation restores the last approved route when navigation is blocked', async () => {
  await withPage(CORE, async (page) => {
    const result = await page.evaluate(async () => {
      PCV.uxlib.setHashRoute('mon-alerts');
      const before = location.hash;
      window.navigateTo = () => false;
      history.replaceState(null, '', '#/dashboard');
      window.dispatchEvent(new HashChangeEvent('hashchange'));
      await Promise.resolve();
      return { before, after: location.hash };
    });
    assert.deepEqual(result, {
      before: '#/mon-alerts',
      after: '#/mon-alerts'
    });
  });
});

test('approved async hash navigation applies the requested route once', async () => {
  await withPage(CORE, async (page) => {
    const result = await page.evaluate(async () => {
      const commits = [];
      let resolveDecision;
      PCV.uxlib.setHashRoute('mon-alerts');
      PCV.ui.setNavigationBlocker(() => new Promise(resolve => {
        resolveDecision = resolve;
      }));
      window.navigateTo = target => PCV.ui.requestNavigation(target, () => {
        commits.push(target);
        PCV.uxlib.setHashRoute(target);
      });
      history.replaceState(null, '', '#/dashboard');
      window.dispatchEvent(new HashChangeEvent('hashchange'));
      const pendingHash = location.hash;
      resolveDecision(true);
      await Promise.resolve();
      await Promise.resolve();
      return { pendingHash, finalHash: location.hash, commits };
    });
    assert.deepEqual(result, {
      pendingHash: '#/mon-alerts',
      finalHash: '#/dashboard',
      commits: ['dashboard']
    });
  });
});

test('approved async resource hash preserves the id and opens the resource once', async () => {
  await withPage(CORE, async (page) => {
    const result = await page.evaluate(async () => {
      const commits = [];
      const opened = [];
      let resolveDecision;
      PCV.uxlib.setHashRoute('mon-alerts');
      PCV.ui.setNavigationBlocker(() => new Promise(resolve => {
        resolveDecision = resolve;
      }));
      window.openResourceById = (target, id) => opened.push([target, id]);
      window.navigateTo = (target, hooks) => PCV.ui.requestNavigation(
        target,
        () => {
          commits.push(target);
          if (hooks && typeof hooks.after === 'function') hooks.after();
        }
      );
      history.replaceState(null, '', '#/vms/vm-42');
      window.dispatchEvent(new HashChangeEvent('hashchange'));
      const pendingHash = location.hash;
      resolveDecision(true);
                                                                     
                                                            
                                                       
      await new Promise((resolve, reject) => {
        const deadline = performance.now() + 2000;
        const poll = () => {
          if (opened.length === 1) return resolve();
          if (performance.now() >= deadline) {
            return reject(new Error('resource after-hook did not run'));
          }
          setTimeout(poll, 10);
        };
        poll();
      });
      return {
        pendingHash,
        finalHash: location.hash,
        commits,
        opened
      };
    });
    assert.deepEqual(result, {
      pendingHash: '#/mon-alerts',
      finalHash: '#/vms/vm-42',
      commits: ['vms'],
      opened: [['vms', 'vm-42']]
    });
  });
});

test('route hooks and route state change only after navigation approval', async () => {
  await withPage(CORE, async (page) => {
    await page.evaluate(() => {
      window._L = ko => ko;
      window.t = key => key;
      PCV.filterEditionItems = items => items;
    });
    await page.addScriptTag({ url: '/ui/modules/nav.js' });
    const result = await page.evaluate(async () => {
      window._DEBUG = false;
      window.currentTab = 'mon-alerts';
      window.vmList = [];
      window.selectedVmIndex = 0;
      window.destroyAllCharts = () => {};
      const events = [];
      let resolveDecision;
      PCV.ui.setNavigationBlocker(() => new Promise(resolve => {
        resolveDecision = resolve;
      }));
      PCV.nav.navigateTo('test-route', {
        before: () => events.push('before'),
        after: () => events.push(`after:${window.currentTab}`)
      });
      const pending = {
        tab: window.currentTab,
        generation: window._navGeneration,
        events: events.slice()
      };
      resolveDecision(false);
      await Promise.resolve();
      await Promise.resolve();
      const cancelled = {
        tab: window.currentTab,
        generation: window._navGeneration,
        events: events.slice()
      };

      PCV.ui.setNavigationBlocker(() => Promise.resolve(true));
      PCV.nav.navigateTo('test-route', {
        before: () => events.push('before'),
        after: () => events.push(`after:${window.currentTab}`)
      });
      await Promise.resolve();
      await Promise.resolve();
      return {
        pending,
        cancelled,
        approved: {
          tab: window.currentTab,
          generation: window._navGeneration,
          events
        }
      };
    });
    assert.deepEqual(result, {
      pending: { tab: 'mon-alerts', generation: 0, events: [] },
      cancelled: { tab: 'mon-alerts', generation: 0, events: [] },
      approved: {
        tab: 'test-route',
        generation: 1,
        events: ['before', 'after:test-route']
      }
    });
  });
});

  
                  
                                                                                
                                                                              
                                                                          
  
               
                                                                  
                                               
   
test('setHashRoute keeps the current query string when writing the route hash', async () => {
  await withPage(CORE, async (page) => {
    const result = await page.evaluate(() => {
      const base = document.createElement('base');
      base.href = '/ui/';
      document.head.appendChild(base);
      history.replaceState(null, '', location.pathname + '?severity=crit');
      PCV.uxlib.setHashRoute('mon-alerts');
      return { search: location.search, hash: location.hash };
    });
    assert.deepEqual(result, { search: '?severity=crit', hash: '#/mon-alerts' });
  });
});
