                                                                                      
                                                    
                                                                    
  
                                     
  
                                                        
                                                                  
                                                               
                                                               
                                                 
   
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { withPage, CORE } from './harness.mjs';

const DISPATCHER_SOURCE = readFileSync(
  new URL('../../src/api/dispatcher.c', import.meta.url), 'utf8');
const REST_RBAC_SOURCE = readFileSync(
  new URL('../../src/modules/auth/pcv_rbac.c', import.meta.url), 'utf8');
const APP_SOURCE = readFileSync(new URL('../../ui/app.js', import.meta.url), 'utf8');
const NAV_SOURCE = readFileSync(new URL('../../ui/modules/nav.js', import.meta.url), 'utf8');

const BASE_MODULES = [...CORE, 'ui/modules/endpoints.js'];
const NETWORK_MODULES = [...BASE_MODULES, 'ui/modules/filter-state.js', 'ui/modules/network.js'];
const ADVANCED_MODULES = [...BASE_MODULES, 'ui/modules/advanced.js'];
const CLOUD_MODULES = [...BASE_MODULES, 'ui/modules/cloud.js'];
const VM_MODULES = [...BASE_MODULES, 'ui/modules/vm.js'];

const ROLE_RANK = { VIEWER: 0, OPERATOR: 1, ADMIN: 2 };
const EXPECTED_ROLE_ATTR = {
  1: 'OPERATOR,ADMIN',
  2: 'ADMIN'
};

function regexEscape(value) {
  return value.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

function dispatcherMinRole(method) {
  const expression = new RegExp(
    `\\{\\s*"${regexEscape(method)}"\\s*,\\s*([012])\\s*\\}`, 'g');
  const values = [...DISPATCHER_SOURCE.matchAll(expression)].map(match => Number(match[1]));
  assert.ok(values.length > 0, `${method} must be explicit in g_method_policies[]`);
  assert.equal(new Set(values).size, 1, `${method} has conflicting dispatcher role entries`);
  return values[0];
}

function sourceSection(source, startNeedle, endNeedle) {
  const start = source.indexOf(startNeedle);
  const end = source.indexOf(endNeedle, start + startNeedle.length);
  assert.ok(start >= 0 && end > start, `source markers missing: ${startNeedle}`);
  return source.slice(start, end);
}

                                                                           
                                                                   
const REST_ADMIN_SECTION = sourceSection(
  REST_RBAC_SOURCE, '/* ── ADMIN-only methods', '/* ── OPERATOR: known operational methods');

function assertRestAdmin(method) {
  assert.match(
    REST_ADMIN_SECTION,
    new RegExp(`g_strcmp0\\(method,\\s*"${regexEscape(method)}"\\)\\s*==\\s*0`),
    `${method} must remain ADMIN at the REST RBAC gate`
  );
}

function extractSource(source, startNeedle, endNeedle) {
  const start = source.indexOf(startNeedle);
  const end = endNeedle ? source.indexOf(endNeedle, start + startNeedle.length) : source.length;
  assert.ok(start >= 0 && end > start, `source slice missing: ${startNeedle}`);
  return source.slice(start, end);
}

const ROLE_GUARD_SOURCE = extractSource(
  NAV_SOURCE, 'function pcvRoleAllows(minRole)', '/* ═══ CONTENT DISPATCH');
const LEGACY_N_SHORTCUT_SOURCE = extractSource(
  APP_SOURCE, '/* ═══ KEYBOARD SHORTCUTS ═══ */', '/* ═══ MOBILE ═══ */');
const REGISTRY_N_SHORTCUT_SOURCE = extractSource(
  APP_SOURCE, '/* #6 기본 단축키 등록 */', null);

async function seedProductGlobals(page, tab) {
  await page.setViewport({ width: 1280, height: 900 });
  await page.evaluate(activeTab => {
    window._DEBUG = false;
    window.API_BASE = '/api/v1';
    window.authToken = 'd1-rbac-token';
    window.currentTab = activeTab;
    window._L = (ko, en) => en || ko;
    window.t = key => key;
    window.unwrapData = value => value == null ? value
      : (value.data !== undefined ? value.data
        : (value.result !== undefined ? value.result : value));
    window.unwrapList = value => {
      const data = window.unwrapData(value);
      return Array.isArray(data) ? data
        : (Array.isArray(data?.items) ? data.items
          : (Array.isArray(data?.list) ? data.list : []));
    };
    window.fetchGet = async url => {
      const response = await fetch(url);
      const body = await response.json().catch(() => null);
      if (!response.ok) throw new Error(body?.error?.message || `HTTP ${response.status}`);
      if (body && body.error) throw new Error(body.error.message || 'request failed');
      return body;
    };
    const send = (method, url, body) => fetch(url, {
      method,
      headers: { 'Content-Type': 'application/json' },
      body: body === undefined ? undefined : JSON.stringify(body)
    }).then(response => response.json());
    window.fetchPost = (url, body) => send('POST', url, body);
    window.fetchPut = (url, body) => send('PUT', url, body);
    window.fetchDelete = (url, body) => send('DELETE', url, body);
    window.fmtBytes = value => String(value);
    window.vmList = [{ name: 'vm-rbac', state: 'running' }];
    window.selectedVmIndex = 0;
    window.checkedVms = new Set();
    window.addEvt = () => {};
    window.toast = () => {};
    window.customConfirm = async () => true;
    window.closeModal = () => {};
    window.showModal = () => {};
    window.renderContent = () => {};
    window.loadAll = () => {};
  }, tab);
}

const SURFACE_CASES = [
  {
    name: 'templates',
    modules: ADVANCED_MODULES,
    tab: 'templates',
    renderer: 'renderTemplates',
    routes: {
      '/api/v1/templates': {
        body: { data: [{ name: 'tpl-rbac', vcpu: 2, memory_mb: 2048, disk_gb: 20, os_variant: 'linux' }] }
      }
    },
    probes: [
      { selector: 'button[onclick^="showTemplateCreate"]', method: 'template.create', minRole: 2, count: 1 },
                                                                    
      { selector: 'button[onclick^="templateUse"]', method: 'vm.create', minRole: 1, count: 1 },
      { selector: 'button[onclick^="templateDel"]', method: 'template.delete', minRole: 2, count: 1 }
    ],
    readSelector: 'button[onclick="loadTemplateHistory()"]'
  },
  {
    name: 'ovn',
    modules: NETWORK_MODULES,
    tab: 'ovn',
    renderer: 'renderOvn',
    routes: {
      '/api/v1/ovn/status': { body: { data: { available: true } } },
      '/api/v1/ovn/switches': { body: { data: [{ name: 'sw-rbac' }] } },
      '/api/v1/ovn/routers': { body: { data: [{ name: 'rt-rbac' }] } }
    },
    probes: [
      { selector: 'button[onclick^="nfvFwAdd"]', method: 'ovn.acl.add', minRole: 2, count: 1 }
    ],
    readSelector: '#cb .pagehead'
  },
  {
    name: 'security-groups',
    modules: NETWORK_MODULES,
    tab: 'security-groups',
    renderer: 'renderSecGroups',
    routes: {},
    probes: [
      { selector: 'button[onclick^="sgAddRule"]', method: 'ovn.acl.add', minRole: 2, count: 1 }
    ],
    readSelector: 'button[onclick="sgListRules()"]'
  },
  {
    name: 'cloud-migration',
    modules: CLOUD_MODULES,
    tab: 'cloud-migration',
    renderer: 'renderCloudMigration',
    routes: {
      '/api/v1/cloud/jobs': {
        body: { data: [
          { name: 'running-rbac', direction: 'import', status: 'running', progress_percent: 20 },
          { name: 'cutover-rbac', direction: 'import', status: 'awaiting_cutover', progress_percent: 90 }
        ] }
      }
    },
    probes: [
                                                                     
      { selector: 'button[onclick^="cmDoImport"]', method: 'vm.import.ec2', minRole: 2, count: 1 },
      { selector: 'button[onclick^="cmDoExport"]', method: 'vm.export.ec2', minRole: 2, count: 1 },
      { selector: 'button[onclick^="cmCancelJob"]', method: 'cloud.job.cancel', minRole: 2, count: 1 },
      { selector: 'button[onclick^="cmFinalize"]', method: 'cloud.import.finalize', minRole: 2, count: 1 }
    ],
    readSelector: '#cm-jobs table'
  },
  {
    name: 'config-mgmt',
    modules: ADVANCED_MODULES,
    tab: 'config-mgmt',
    renderer: 'renderConfigMgmt',
    routes: {
      '/api/v1/config/daemon': {
        body: { data: {
          storage: { zvol_pool: 'tank/vms', image_dir: '/images', iso_dirs: '/iso', container_pool: 'tank/ctrs' },
          container: { lxc_path: '/var/lib/purecvisor/lxc' }
        } }
      },
      '/api/v1/config/history': { body: { data: [] } }
    },
    probes: [
      { selector: 'button[onclick^="saveStorageCfg"]', method: 'daemon.config.set', minRole: 2, count: 2 },
      { selector: 'button[onclick^="configBackup"]', method: 'config.backup', minRole: 2, count: 1 }
    ],
    readSelector: '#cfg-history'
  }
];

test('representative UI mutations remain anchored to explicit backend policy', () => {
  const expected = {
    'template.create': 2,
    'template.delete': 2,
    'vm.create': 1,
    'ovn.acl.add': 2,
    'vm.import.ec2': 1,
    'vm.export.ec2': 1,
    'cloud.job.cancel': 2,
    'cloud.import.finalize': 2,
    'daemon.config.set': 2,
    'config.backup': 2,
    'storage.zvol.delete': 2,
    'vm.start': 1,
    'vm.stop': 1,
    'vm.snapshot.create': 1,
    'vm.rename': 1,
    'vm.vnc': 1,
    'vm.disk.live_resize': 1,
    'vm.clone': 1,
    'vm.delete': 1,
    'vm.export.ova': 2
  };
  for (const [method, minRole] of Object.entries(expected)) {
    assert.equal(dispatcherMinRole(method), minRole, method);
  }
  assertRestAdmin('vm.import.ec2');
  assertRestAdmin('vm.export.ec2');
});

                           
                                                           
                                                                       
                                                                
  
                       
                                               
                                 
for (const surface of SURFACE_CASES) {
  test(`${surface.name} mutations follow backend VIEWER/OPERATOR/ADMIN visibility`, async () => {
    await withPage(surface.modules, async page => {
      await seedProductGlobals(page, surface.tab);

      for (const role of [null, 'VIEWER', 'OPERATOR', 'ADMIN']) {
        const result = await page.evaluate(async ({ renderer, probes, readSelector, effectiveRole }) => {
          window.currentUser = effectiveRole ? { role: effectiveRole } : null;
          const target = document.getElementById('cb');
          await window[renderer](target);
                                                                      
                                                                          
          if (renderer === 'renderCloudMigration') await window.cmLoadJobs();

          const mutations = probes.map(probe => ({
            probe,
            nodes: [...target.querySelectorAll(probe.selector)].map(node => {
              const gate = node.closest('[data-role]');
              return {
                roleAttr: gate?.getAttribute('data-role') || null,
                roleHidden: gate?.classList.contains('role-hidden') || false,
                display: gate ? getComputedStyle(gate).display : null
              };
            })
          }));
          const readNode = target.querySelector(readSelector);
          return {
            mutations,
            readVisible: Boolean(readNode) && getComputedStyle(readNode).display !== 'none'
          };
        }, {
          renderer: surface.renderer,
          probes: surface.probes,
          readSelector: surface.readSelector,
          effectiveRole: role
        });

        assert.equal(result.readVisible, true, `${surface.name}/${role || 'unknown'} read surface`);
        for (const { probe, nodes } of result.mutations) {
          assert.equal(nodes.length, probe.count,
            `${surface.name}/${role || 'unknown'} ${probe.method} control count`);
          for (const node of nodes) {
            assert.equal(node.roleAttr, EXPECTED_ROLE_ATTR[probe.minRole],
              `${surface.name}/${probe.method} closest gate`);
            const visible = role !== null && ROLE_RANK[role] >= probe.minRole;
            assert.equal(node.roleHidden, !visible,
              `${surface.name}/${role || 'unknown'} ${probe.method} role-hidden`);
            if (visible) assert.notEqual(node.display, 'none', `${probe.method} should be visible`);
            else assert.equal(node.display, 'none', `${probe.method} should be hidden`);
          }
        }
      }

      await page.evaluate(() => {
        if (typeof window._cloudCleanupTimer === 'function') window._cloudCleanupTimer();
      });
    }, { routes: surface.routes });
  });
}

test('VM context menu keeps reads visible and gates OPERATOR versus ADMIN actions', async () => {
  await withPage(VM_MODULES, async page => {
    await seedProductGlobals(page, 'summary');
    const matrix = await page.evaluate(() => {
      window.vmList = [{
        name: 'vm-context-rbac', state: 'running', live_cpu_pct: 10,
        mem_percent: 20, vcpu: 2, memory_mb: 2048
      }];
      const ctx = document.createElement('div');
      ctx.id = 'ctx';
      document.body.appendChild(ctx);
      const results = {};
      for (const role of [null, 'VIEWER', 'OPERATOR', 'ADMIN']) {
        window.currentUser = role ? { role } : null;
        window.showCtx({ preventDefault() {}, pageX: 20, pageY: 20 }, 0);
        results[role || 'unknown'] = {
          mutation: [...ctx.querySelectorAll('.ci[data-role]')].map(node => ({
            label: node.textContent.trim(),
            roleAttr: node.getAttribute('data-role'),
            roleHidden: node.classList.contains('role-hidden'),
            display: getComputedStyle(node).display
          })),
          read: [...ctx.querySelectorAll('.ci:not([data-role])')].map(node => ({
            label: node.textContent.trim(),
            display: getComputedStyle(node).display
          }))
        };
      }
      return results;
    });

    for (const [role, result] of Object.entries(matrix)) {
      assert.equal(result.mutation.length, 12, `${role} VM mutation count`);
      assert.equal(result.read.length, 2, `${role} VM read count`);
      assert.deepEqual(result.read.map(item => item.label), ['📌 Memory Stats', '⚙ CPU Stats']);
      assert.ok(result.read.every(item => item.display !== 'none'), `${role} read items stay visible`);

      for (const item of result.mutation) {
        const minRole = item.label.includes('Export OVA') ? 2 : 1;
        assert.equal(item.roleAttr, EXPECTED_ROLE_ATTR[minRole], `${item.label} backend role`);
        const visible = role !== 'unknown' && ROLE_RANK[role] >= minRole;
        assert.equal(item.roleHidden, !visible, `${role}/${item.label} role-hidden`);
        if (visible) assert.notEqual(item.display, 'none', `${role}/${item.label} visible`);
        else assert.equal(item.display, 'none', `${role}/${item.label} hidden`);
      }
    }
  });
});

test('zvol context menu exposes delete only to dispatcher ADMIN', async () => {
  await withPage([...BASE_MODULES, 'ui/modules/filter-state.js', 'ui/modules/storage.js'], async page => {
    await seedProductGlobals(page, 'storage');
    const matrix = await page.evaluate(() => {
      window._zvolSel = new Set();
      const results = {};
      for (const role of [null, 'VIEWER', 'OPERATOR', 'ADMIN']) {
        window.currentUser = role ? { role } : null;
        window.zvolCtxMenu({ preventDefault() {}, clientX: 10, clientY: 10 }, 'tank/zvol-rbac');
        results[role || 'unknown'] = [...document.querySelectorAll('#pcv-ctx-menu > div')]
          .map(node => node.textContent.trim());
      }
      return results;
    });

    assert.deepEqual(matrix.unknown, ['⚙ Select']);
    assert.deepEqual(matrix.VIEWER, ['⚙ Select']);
    assert.deepEqual(matrix.OPERATOR, ['⚙ Select']);
    assert.deepEqual(matrix.ADMIN, ['🗑 btn.delete', '⚙ Select']);
  });
});

test('global N shortcuts fail closed below vm.create OPERATOR and ignore editable single-N', async () => {
  await withPage(CORE, async page => {
                                                               
                                                                    
    await page.addScriptTag({ content: ROLE_GUARD_SOURCE });
    await page.evaluate(() => {
      window.__createCalls = 0;
      window.showCreate = () => { window.__createCalls += 1; };
      window.toggleFS = () => {};
      window.showSettings = () => {};
      window.showPrefs = () => {};
      window.openCmdPalette = () => {};
      window.closeCmdPalette = () => {};
      window.cmdPaletteOpen = false;
      window.navigateTo = () => {};
    });
    await page.addScriptTag({ content: LEGACY_N_SHORTCUT_SOURCE });
    await page.addScriptTag({ content: REGISTRY_N_SHORTCUT_SOURCE });

    const matrix = await page.evaluate(() => {
      const fire = (role, ctrlKey, target) => {
        window.currentUser = role ? { role } : null;
        window.__createCalls = 0;
        target.dispatchEvent(new KeyboardEvent('keydown', {
          key: 'n', ctrlKey, bubbles: true, cancelable: true
        }));
        return window.__createCalls;
      };
      const input = document.createElement('input');
      document.body.appendChild(input);
      const result = {};
      for (const role of [null, 'VIEWER', 'OPERATOR', 'ADMIN']) {
        const key = role || 'unknown';
        result[key] = {
          n: fire(role, false, document.body),
          ctrlN: fire(role, true, document.body)
        };
      }
      result.operatorInputN = fire('OPERATOR', false, input);
      return result;
    });

    assert.deepEqual(matrix.unknown, { n: 0, ctrlN: 0 });
    assert.deepEqual(matrix.VIEWER, { n: 0, ctrlN: 0 });
    assert.deepEqual(matrix.OPERATOR, { n: 1, ctrlN: 1 });
    assert.deepEqual(matrix.ADMIN, { n: 1, ctrlN: 1 });
    assert.equal(matrix.operatorInputN, 0);
  });
});
