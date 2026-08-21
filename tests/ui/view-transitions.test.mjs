                                                                                         
                                                                                 
                                                                     
  
                                                        
  
                                                                           
                                                                              
                                                                               
  
                                                      
   
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import { withPage, CORE } from './harness.mjs';

const VM_MODULES = [...CORE, 'ui/modules/vm.js'];
const NETWORK_MODULES = [...CORE, 'ui/modules/network.js'];
const NAV_SOURCE = fs.readFileSync(new URL('../../ui/modules/nav.js', import.meta.url), 'utf8');
const VM_SOURCE = fs.readFileSync(new URL('../../ui/modules/vm.js', import.meta.url), 'utf8');
const VM_FIXTURE = [{
  name: 'vm-alpha', state: 'running', running: 1, vcpu: 2, memory_mb: 2048,
  live_cpu_pct: 12, mem_percent: 25
}];

function replaceSourceOnce(source, needle, replacement, label) {
  const occurrences = source.split(needle).length - 1;
  assert.equal(occurrences, 1, `${label} source contract must occur exactly once`);
  return source.replace(needle, replacement);
}

async function loadNavigation(page) {
  await page.evaluate(() => {
    window._DEBUG = false;
    window._L = (ko) => ko;
    window.t = key => key;
    window.currentTab = 'dashboard';
    window.vmList = [];
    window.selectedVmIndex = 0;
    window.destroyAllCharts = () => {};
    window.PCV.filterEditionItems = items => items;
  });
  await page.addScriptTag({ url: '/ui/modules/nav.js' });
}

test('viewTransition updates exactly once in native, reduced, unsupported, and sync-throw paths', async () => {
  await withPage(CORE, async page => {
    const result = await page.evaluate(() => {
      const run = (mode) => {
        let nativeCalls = 0;
        let updates = 0;
        window.matchMedia = () => ({
          matches: mode === 'reduced',
          addEventListener() {},
          removeEventListener() {}
        });

        if (mode === 'unsupported') {
          Object.defineProperty(document, 'startViewTransition', {
            configurable: true, writable: true, value: undefined
          });
        } else {
          Object.defineProperty(document, 'startViewTransition', {
            configurable: true,
            writable: true,
            value: (update) => {
              nativeCalls++;
              if (mode === 'throw') throw new Error('sync VT failure');
              update();
              return { finished: Promise.resolve() };
            }
          });
        }

        PCV.uxlib.viewTransition(() => { updates++; });
        return { nativeCalls, updates };
      };

      return {
        native: run('native'),
        reduced: run('reduced'),
        unsupported: run('unsupported'),
        thrown: run('throw')
      };
    });

    assert.deepEqual(result, {
      native: { nativeCalls: 1, updates: 1 },
      reduced: { nativeCalls: 0, updates: 1 },
      unsupported: { nativeCalls: 0, updates: 1 },
      thrown: { nativeCalls: 1, updates: 1 }
    });
  });
});

test('delayed native callbacks coalesce rapid dashboard to networks to dashboard navigation', async () => {
  await withPage(CORE, async page => {
    await loadNavigation(page);
    const result = await page.evaluate(() => {
      const queued = [];
      const paints = [];
      window.matchMedia = () => ({ matches: false, addEventListener() {}, removeEventListener() {} });
      Object.defineProperty(document, 'startViewTransition', {
        configurable: true,
        writable: true,
        value: (update) => {
          queued.push(update);
          return { finished: Promise.resolve() };
        }
      });
      window.renderDashboard = target => {
        paints.push('dashboard');
        target.textContent = 'DASHBOARD FINAL';
      };
      window.renderNetworks = target => {
        paints.push('networks');
        target.textContent = 'NETWORKS STALE';
      };

      PCV.nav.navigateTo('dashboard');
      PCV.nav.navigateTo('networks');
      PCV.nav.navigateTo('dashboard');
      const queuedCount = queued.length;
      queued.forEach(update => update());

      return {
        queuedCount,
        paints,
        tab: window.currentTab,
        generation: window._navGeneration,
        text: document.getElementById('cb').textContent,
        faded: document.getElementById('cb').classList.contains('fade-out')
      };
    });

    assert.deepEqual(result, {
      queuedCount: 3,
      paints: ['dashboard'],
      tab: 'dashboard',
      generation: 3,
      text: 'DASHBOARD FINAL',
      faded: false
    });
  });
});

test('rapid VM tabs paint once and restore focus with complete tab semantics', async () => {
  await withPage(VM_MODULES, async page => {
    await page.evaluate(vms => {
      window._DEBUG = false;
      window._L = (ko) => ko;
      window.t = key => key;
      window.currentTab = 'summary';
      window.vmList = vms;
      window.selectedVmIndex = 0;
      window.sortField = 'name';
      window.sortDirection = 1;
      window.checkedVms = new Set();
      window.destroyAllCharts = () => {};
      window.PCV.filterEditionItems = items => items;
      window.renderSummary = target => { target.textContent = 'summary'; };
      window.renderConsole = target => { target.textContent = 'console'; };
      window.renderSnapshots = target => { target.textContent = 'snapshots'; };
      window.renderPerformance = target => { target.textContent = 'performance'; };
      window.renderTimeline = target => { target.textContent = 'timeline'; };
    }, VM_FIXTURE);
    await page.addScriptTag({ url: '/ui/modules/nav.js' });

    const result = await page.evaluate(() => {
      const queued = [];
      const paints = [];
      window.renderSummary = target => { paints.push('summary'); target.textContent = 'summary'; };
      window.renderConsole = target => { paints.push('console'); target.textContent = 'console'; };
      window.renderSnapshots = target => { paints.push('snapshots'); target.textContent = 'snapshots'; };
      window.renderPerformance = target => { paints.push('performance'); target.textContent = 'performance'; };
      window.renderTimeline = target => { paints.push('timeline'); target.textContent = 'timeline'; };

                                                                                  
      window.renderVmScreen(document.getElementById('cb'), window.vmList[0], 'summary');
      paints.length = 0;
      window.matchMedia = () => ({ matches: false, addEventListener() {}, removeEventListener() {} });
      Object.defineProperty(document, 'startViewTransition', {
        configurable: true,
        writable: true,
        value: (update) => {
          queued.push(update);
          return { finished: Promise.resolve() };
        }
      });

      document.querySelector('[role="tab"][data-t="console"]').click();
      document.querySelector('[role="tab"][data-t="snapshots"]').click();
      document.querySelector('[role="tab"][data-t="performance"]').click();
      const queuedCount = queued.length;
      queued.forEach(update => update());

      const tabs = [...document.querySelectorAll('[role="tab"]')];
      const panel = document.querySelector('[role="tabpanel"]');
      return {
        queuedCount,
        paints,
        tab: window.currentTab,
        focused: document.activeElement?.dataset.t || null,
        tablistCount: document.querySelectorAll('[role="tablist"]').length,
        selected: tabs.filter(tab => tab.getAttribute('aria-selected') === 'true').map(tab => tab.dataset.t),
        roving: tabs.map(tab => [tab.dataset.t, tab.getAttribute('tabindex')]),
        controls: tabs.map(tab => tab.getAttribute('aria-controls')),
        panel: {
          id: panel?.id || null,
          labelledBy: panel?.getAttribute('aria-labelledby') || null,
          text: panel?.textContent || ''
        }
      };
    });

    assert.equal(result.queuedCount, 3);
    assert.deepEqual(result.paints, ['performance']);
    assert.equal(result.tab, 'performance');
    assert.equal(result.focused, 'performance');
    assert.equal(result.tablistCount, 1);
    assert.deepEqual(result.selected, ['performance']);
    assert.deepEqual(result.roving, [
      ['summary', '-1'], ['console', '-1'], ['snapshots', '-1'],
      ['performance', '0'], ['timeline', '-1']
    ]);
    assert.ok(result.controls.every(value => value === 'vm-detail'));
    assert.deepEqual(result.panel, {
      id: 'vm-detail',
      labelledBy: 'vm-tab-performance',
      text: 'performance'
    });
  });
});

test('VM tablist supports Left/Right/Home/End roving activation without extra tab stops', async () => {
  await withPage(VM_MODULES, async page => {
    await page.evaluate(vms => {
      window._DEBUG = false;
      window._L = ko => ko;
      window.t = key => key;
      window.currentTab = 'summary';
      window.vmList = vms;
      window.selectedVmIndex = 0;
      window.sortField = 'name';
      window.sortDirection = 1;
      window.checkedVms = new Set();
      window.destroyAllCharts = () => {};
      window.PCV.filterEditionItems = items => items;
      for (const tab of ['Summary', 'Console', 'Snapshots', 'Performance', 'Timeline']) {
        window['render' + tab] = target => { target.textContent = tab.toLowerCase(); };
      }
      Object.defineProperty(document, 'startViewTransition', {
        configurable: true, writable: true, value: undefined
      });
    }, VM_FIXTURE);
    await page.addScriptTag({ url: '/ui/modules/nav.js' });

    const result = await page.evaluate(() => {
      window.renderVmScreen(document.getElementById('cb'), window.vmList[0], 'summary');
      const press = key => {
        const active = document.querySelector('[role="tab"][aria-selected="true"]');
        active.focus();
        active.dispatchEvent(new KeyboardEvent('keydown', { key, bubbles: true, cancelable: true }));
        return {
          selected: document.querySelector('[role="tab"][aria-selected="true"]')?.dataset.t,
          focused: document.activeElement?.dataset.t,
          tabStops: document.querySelectorAll('[role="tab"][tabindex="0"]').length
        };
      };
      return {
        right: press('ArrowRight'),
        end: press('End'),
        home: press('Home'),
        wrapLeft: press('ArrowLeft')
      };
    });

    assert.deepEqual(result, {
      right: { selected: 'console', focused: 'console', tabStops: 1 },
      end: { selected: 'timeline', focused: 'timeline', tabStops: 1 },
      home: { selected: 'summary', focused: 'summary', tabStops: 1 },
      wrapLeft: { selected: 'timeline', focused: 'timeline', tabStops: 1 }
    });
  });
});

test('started async network renderer settles only inside its detached generation on resolve and reject', async () => {
  await withPage(NETWORK_MODULES, async page => {
    await loadNavigation(page);
    const result = await page.evaluate(async () => {
      window.EP = { NET_LIST: () => '/test/networks' };
      window.unwrapList = response => Array.isArray(response?.data) ? response.data : [];
      window.matchMedia = () => ({ matches: false, addEventListener() {}, removeEventListener() {} });
      Object.defineProperty(document, 'startViewTransition', {
        configurable: true,
        writable: true,
        value: update => {
          update();
          return { finished: Promise.resolve() };
        }
      });

      const run = async outcome => {
        let settleFetch;
        let fetches = 0;
        let cleanups = 0;
        let roleApplications = 0;
        let dashboardPaints = 0;
        window.currentUser = { role: 'admin' };
        window.currentTab = 'dashboard';
        window._navGeneration = 0;
        window.PCV.ui._renderTarget = null;
        window.PCV.uxlib.clearEl(document.getElementById('cb'));
        window.destroyAllCharts = () => { cleanups++; };
        window.applyRoleVisibility = () => { roleApplications++; };
        window.fetchGet = () => {
          fetches++;
          return new Promise((resolve, reject) => {
            settleFetch = () => outcome === 'resolve'
              ? resolve({ data: [] })
              : reject(new Error('stale network failure'));
          });
        };
        window.renderDashboard = target => {
          dashboardPaints++;
          target.textContent = 'DASHBOARD FINAL';
        };

        PCV.nav.navigateTo('networks');
        await Promise.resolve();
        const oldTarget = PCV.ui._renderTarget;
        const startGeneration = window._navGeneration;
        const loadingSkeletons = oldTarget.querySelectorAll('.skeleton').length;
        const fetchesAfterStart = fetches;

        PCV.nav.navigateTo('dashboard');
        const newTarget = PCV.ui._renderTarget;
        const finalGeneration = window._navGeneration;
        const fetchesBeforeSettle = fetches;
        const cleanupsBeforeSettle = cleanups;
        const rolesBeforeSettle = roleApplications;

        settleFetch();
        await new Promise(resolve => setTimeout(resolve, 0));

        return {
          outcome,
          startGeneration,
          finalGeneration,
          loadingSkeletons,
          fetchesAfterStart,
          fetchesAfterSettle: fetches,
          staleFetchDelta: fetches - fetchesBeforeSettle,
          cleanupsBeforeSettle,
          cleanupsAfterSettle: cleanups,
          staleCleanupDelta: cleanups - cleanupsBeforeSettle,
          staleRoleDelta: roleApplications - rolesBeforeSettle,
          dashboardPaints,
          distinctTargets: oldTarget !== newTarget,
          oldConnected: oldTarget.isConnected,
          newConnected: newTarget.isConnected,
          wrapperCount: document.querySelectorAll('#cb > .cb-render').length,
          finalTab: window.currentTab,
          finalText: newTarget.textContent,
          newHasError: !!newTarget.querySelector('.load-error-state'),
          oldState: oldTarget.querySelector('.empty-state') ? 'empty'
            : oldTarget.querySelector('.load-error-state') ? 'error' : 'loading'
        };
      };

      return {
        resolved: await run('resolve'),
        rejected: await run('reject')
      };
    });

    const common = {
      startGeneration: 1,
      finalGeneration: 2,
      loadingSkeletons: 3,
      fetchesAfterStart: 1,
      fetchesAfterSettle: 1,
      staleFetchDelta: 0,
      cleanupsBeforeSettle: 2,
      cleanupsAfterSettle: 2,
      staleCleanupDelta: 0,
      dashboardPaints: 1,
      distinctTargets: true,
      oldConnected: false,
      newConnected: true,
      wrapperCount: 1,
      finalTab: 'dashboard',
      finalText: 'DASHBOARD FINAL',
      newHasError: false
    };
    assert.deepEqual(result.resolved, {
      outcome: 'resolve',
      ...common,
      staleRoleDelta: 1,
      oldState: 'empty'
    });
    assert.deepEqual(result.rejected, {
      outcome: 'reject',
      ...common,
      staleRoleDelta: 0,
      oldState: 'error'
    });
  });
});

test('real VM tabs activate by mouse, Enter, and Space with one paint and destination focus', async () => {
  await withPage(VM_MODULES, async page => {
    await page.evaluate(vms => {
      window._DEBUG = false;
      window._L = ko => ko;
      window.t = key => key;
      window.currentTab = 'summary';
      window.vmList = vms;
      window.selectedVmIndex = 0;
      window.sortField = 'name';
      window.sortDirection = 1;
      window.checkedVms = new Set();
      window.destroyAllCharts = () => {};
      window.PCV.filterEditionItems = items => items;
      window.__vmPaints = [];
      for (const tab of ['Summary', 'Console', 'Snapshots', 'Performance', 'Timeline']) {
        window['render' + tab] = target => {
          window.__vmPaints.push(tab.toLowerCase());
          target.textContent = tab.toLowerCase();
        };
      }
      Object.defineProperty(document, 'startViewTransition', {
        configurable: true, writable: true, value: undefined
      });
    }, VM_FIXTURE);
    await page.addScriptTag({ url: '/ui/modules/nav.js' });

    const scenarios = [
      { input: 'mouse', target: 'console' },
      { input: 'Enter', target: 'snapshots' },
      { input: 'Space', target: 'performance' }
    ];
    const results = [];
    for (const scenario of scenarios) {
      await page.evaluate(() => {
        window.currentTab = 'summary';
        window._navGeneration = 0;
        window.renderVmScreen(document.getElementById('cb'), window.vmList[0], 'summary');
        window.__vmPaints.length = 0;
      });
      const selector = `[role="tab"][data-t="${scenario.target}"]`;
      if (scenario.input === 'mouse') {
        await page.click(selector);
      } else {
        await page.focus(selector);
        await page.keyboard.press(scenario.input);
      }
      await page.waitForFunction(target => (
        window.currentTab === target &&
        document.activeElement?.getAttribute('data-t') === target
      ), {}, scenario.target);
      results.push(await page.evaluate(input => {
        const selected = [...document.querySelectorAll('[role="tab"][aria-selected="true"]')];
        const panel = document.querySelector('[role="tabpanel"]');
        return {
          input,
          paints: window.__vmPaints.slice(),
          tab: window.currentTab,
          generation: window._navGeneration,
          focused: document.activeElement?.getAttribute('data-t') || null,
          selected: selected.map(tab => tab.getAttribute('data-t')),
          tabStops: document.querySelectorAll('[role="tab"][tabindex="0"]').length,
          controls: selected[0]?.getAttribute('aria-controls') || null,
          panel: {
            id: panel?.id || null,
            labelledBy: panel?.getAttribute('aria-labelledby') || null,
            text: panel?.textContent || ''
          },
          wrapperCount: document.querySelectorAll('#cb > .cb-render').length
        };
      }, scenario.input));
    }

    assert.deepEqual(results, [
      {
        input: 'mouse', paints: ['console'], tab: 'console', generation: 1,
        focused: 'console', selected: ['console'], tabStops: 1, controls: 'vm-detail',
        panel: { id: 'vm-detail', labelledBy: 'vm-tab-console', text: 'console' },
        wrapperCount: 1
      },
      {
        input: 'Enter', paints: ['snapshots'], tab: 'snapshots', generation: 1,
        focused: 'snapshots', selected: ['snapshots'], tabStops: 1, controls: 'vm-detail',
        panel: { id: 'vm-detail', labelledBy: 'vm-tab-snapshots', text: 'snapshots' },
        wrapperCount: 1
      },
      {
        input: 'Space', paints: ['performance'], tab: 'performance', generation: 1,
        focused: 'performance', selected: ['performance'], tabStops: 1, controls: 'vm-detail',
        panel: { id: 'vm-detail', labelledBy: 'vm-tab-performance', text: 'performance' },
        wrapperCount: 1
      }
    ]);
  });
});

test('counterfactual nav source without the generation guard exposes all stale route paints', async () => {
  const guard = '    if (ticket !== _renderContentTicket || generation !== (window._navGeneration || 0) || tab !== currentTab) return;';
  const mutatedNav = replaceSourceOnce(NAV_SOURCE, guard, '    /* mutation fixture: generation guard removed */', 'nav generation guard');

  await withPage(CORE, async page => {
    await page.evaluate(() => {
      window._DEBUG = false;
      window._L = ko => ko;
      window.t = key => key;
      window.currentTab = 'dashboard';
      window.vmList = [];
      window.selectedVmIndex = 0;
      window.PCV.filterEditionItems = items => items;
    });
    await page.addScriptTag({ content: mutatedNav });
    const result = await page.evaluate(() => {
      const queued = [];
      const paints = [];
      let cleanups = 0;
      window.destroyAllCharts = () => { cleanups++; };
      window.matchMedia = () => ({ matches: false, addEventListener() {}, removeEventListener() {} });
      Object.defineProperty(document, 'startViewTransition', {
        configurable: true,
        writable: true,
        value: update => {
          queued.push(update);
          return { finished: Promise.resolve() };
        }
      });
      window.renderDashboard = target => { paints.push('dashboard'); target.textContent = 'dashboard'; };
      window.renderNetworks = target => { paints.push('networks'); target.textContent = 'networks'; };

      PCV.nav.navigateTo('dashboard');
      PCV.nav.navigateTo('networks');
      PCV.nav.navigateTo('dashboard');
      queued.forEach(update => update());
      return {
        paints,
        cleanups,
        generation: window._navGeneration,
        tab: window.currentTab,
        text: document.getElementById('cb').textContent,
        wrapperCount: document.querySelectorAll('#cb > .cb-render').length
      };
    });

    assert.deepEqual(result, {
      paints: ['dashboard', 'networks', 'dashboard'],
      cleanups: 3,
      generation: 3,
      tab: 'dashboard',
      text: 'dashboard',
      wrapperCount: 1
    });
  });
});

test('counterfactual VM source without focus restoration leaves the committed tab at BODY', async () => {
  const focusRestore = `  if (focusIntent && focusIntent.tab === tab &&
      focusIntent.generation === (window._navGeneration || 0)) {
    var focusedTab = strip.querySelector('[data-t="' + tab + '"]');
    if (focusedTab) focusedTab.focus({ preventScroll: true });
  }`;
  const mutatedVm = replaceSourceOnce(VM_SOURCE, focusRestore,
    '  /* mutation fixture: destination focus restoration removed */\n  void focusIntent;',
    'VM destination focus restoration');

  await withPage(CORE, async page => {
    await page.evaluate(vms => {
      window._DEBUG = false;
      window._L = ko => ko;
      window.t = key => key;
      window.currentTab = 'summary';
      window.vmList = vms;
      window.selectedVmIndex = 0;
      window.sortField = 'name';
      window.sortDirection = 1;
      window.checkedVms = new Set();
      window.destroyAllCharts = () => {};
      window.PCV.filterEditionItems = items => items;
      window.__vmPaints = [];
      for (const tab of ['Summary', 'Console', 'Snapshots', 'Performance', 'Timeline']) {
        window['render' + tab] = target => {
          window.__vmPaints.push(tab.toLowerCase());
          target.textContent = tab.toLowerCase();
        };
      }
      Object.defineProperty(document, 'startViewTransition', {
        configurable: true, writable: true, value: undefined
      });
    }, VM_FIXTURE);
    await page.addScriptTag({ content: mutatedVm });
    await page.addScriptTag({ url: '/ui/modules/nav.js' });
    await page.evaluate(() => {
      window.renderVmScreen(document.getElementById('cb'), window.vmList[0], 'summary');
      window.__vmPaints.length = 0;
    });

    await page.click('[role="tab"][data-t="console"]');
    await page.waitForFunction(() => window.currentTab === 'console');
    const result = await page.evaluate(() => ({
      paints: window.__vmPaints.slice(),
      tab: window.currentTab,
      focused: document.activeElement?.tagName || null,
      selected: document.querySelector('[role="tab"][aria-selected="true"]')?.getAttribute('data-t') || null,
      panel: document.querySelector('[role="tabpanel"]')?.getAttribute('aria-labelledby') || null
    }));

    assert.deepEqual(result, {
      paints: ['console'],
      tab: 'console',
      focused: 'BODY',
      selected: 'console',
      panel: 'vm-tab-console'
    });
  });
});
