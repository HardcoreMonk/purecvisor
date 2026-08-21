                                                                                                    
                                                                                            
                                                                     
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const MODS = [
  'ui/modules/endpoints.js', 'ui/modules/api.js', 'ui/modules/ui.js',
  'ui/modules/filter-state.js', 'ui/modules/uxlib.js',
  'ui/modules/modal-core.js', 'ui/modules/modal.js',
  'ui/modules/shell.js'
];

                              
                                       
async function boot(page) {
  await page.evaluate(() => {
    document.body.appendChild(Object.assign(document.createElement('aside'), { id: 'shell-sidebar' }));
    const main = document.createElement('div');
    ['shell-topbar', 'shell-statusbar'].forEach(id => {
      const d = document.createElement('div'); d.id = id; main.appendChild(d);
    });
    document.body.appendChild(main);
    window._L = (ko, en) => ko;
    window.currentTab = 'dashboard';
    window._navCalls = [];
    window.navigateTo = (n) => { window._navCalls.push(n); return true; };
    window.currentUser = { role: 'admin' };
  });
}

const SNAP = {
  vms: { run: 5, total: 6 }, ctrs: { run: 4, total: 4 },
                                                                   
                                                                
  alerts: { crit: 2, warn: 5, total: 7, unack: 4 }, sec: { count1h: 7, worst: 'warn' },
  storage: { worstPool: { name: 'pcvpool', pct: 86, state: 'DEGRADED' } },
  healing: { pending: 2 }
};

test('mount: 사이드바 6섹션·topbar·statusbar 6세그먼트', async () => {
  await withPage(MODS, async (page) => {
    await boot(page);
    const r = await page.evaluate((snap) => {
      PCV.shell.mount();
      PCV.shell.update(snap);
      return {
        sections: document.querySelectorAll('#shell-sidebar .shell-navsec').length,
        items: document.querySelectorAll('#shell-sidebar .shell-navitem').length,
        navIcons: document.querySelectorAll('#shell-sidebar .shell-ico use').length,
        iconHrefs: [...document.querySelectorAll('#shell-sidebar .shell-ico use')]
          .map(use => use.getAttribute('href')),
        iconText: [...document.querySelectorAll('#shell-sidebar .shell-ico')]
          .map(icon => icon.textContent).join(''),
        searchIcon: document.querySelector('.shell-search .shell-search-ico use')
          ?.getAttribute('href') || '',
        syncRole: document.getElementById('shell-sync')?.getAttribute('role') || '',
        syncLive: document.getElementById('shell-sync')?.getAttribute('aria-live') || '',
        segs: document.querySelectorAll('#shell-statusbar .seg').length,
        crumb: !!document.getElementById('shell-crumb'),
        adminItem: !!document.querySelector('#shell-sidebar [data-nav="accounts"][data-role="ADMIN"]')
      };
    }, SNAP);
    assert.equal(r.sections, 6);
    assert.ok(r.items >= 36, `nav items ${r.items}`);
    assert.equal(r.navIcons, r.items);
    assert.ok(r.iconHrefs.every(href => /^vendor\/coolicons\/coolicons\.svg#ci-[a-z0-9-]+$/.test(href)),
      r.iconHrefs.join(','));
    assert.equal(r.iconText, '');
    assert.equal(r.searchIcon, 'vendor/coolicons/coolicons.svg#ci-search');
    assert.equal(r.syncRole, 'status');
    assert.equal(r.syncLive, 'polite');
    assert.equal(r.segs, 6);
    assert.ok(r.crumb);
    assert.ok(r.adminItem);
  });
});

test('update: 카운트·톤·실패 세그먼트 —', async () => {
  await withPage(MODS, async (page) => {
    await boot(page);
    const r = await page.evaluate((snap) => {
      PCV.shell.mount();
      PCV.shell.update(snap);
      const segTexts = [...document.querySelectorAll('#shell-statusbar .seg')].map(s => s.textContent);
      PCV.shell.update({ ...snap, storage: null });           
      const degraded = [...document.querySelectorAll('#shell-statusbar .seg')].map(s => s.textContent);
      return {
        vmCnt: document.querySelector('#shell-sidebar [data-nav="vm"] .shell-cnt').textContent,
        critSeg: !!document.querySelector('#shell-statusbar .seg-crit'),
        segTexts, degradedHasDash: degraded.some(t => t.includes('—'))
      };
    }, SNAP);
    assert.equal(r.vmCnt, '5/6');
    assert.ok(r.critSeg);                                                
    assert.ok(r.segTexts.some(t => t.includes('86')));        
    assert.ok(r.degradedHasDash);
  });
});

  
                  
                                                              
                                                 
                                             
   
test('remount: 마지막 스냅샷 카운트·dot 유지', async () => {
  await withPage(MODS, async (page) => {
    await boot(page);
    const r = await page.evaluate((snap) => {
      PCV.shell.mount();
      PCV.shell.update(snap);
      PCV.shell.mount();              
      return {
        vmCnt: document.querySelector('#shell-sidebar [data-cnt="vms"]').textContent,
        ctrCnt: document.querySelector('#shell-sidebar [data-cnt="ctrs"]').textContent,
        alertDot: !document.querySelector('#shell-sidebar [data-dot="alerts"]').hasAttribute('hidden'),
        segs: document.querySelectorAll('#shell-statusbar .seg').length
      };
    }, SNAP);
    assert.equal(r.vmCnt, '5/6');
    assert.equal(r.ctrCnt, '4/4');
    assert.ok(r.alertDot);
    assert.equal(r.segs, 6);
  });
});

test('setActive: active 표시·vm 5탭 매핑·크럼 갱신', async () => {
  await withPage(MODS, async (page) => {
    await boot(page);
    const r = await page.evaluate(() => {
      PCV.shell.mount();
      PCV.shell.setActive('networks');
      const a1 = document.querySelector('#shell-sidebar .shell-navitem[aria-current="page"]');
      PCV.shell.setActive('snapshots');                              
      const a2 = document.querySelector('#shell-sidebar .shell-navitem[aria-current="page"]');
      return {
        first: a1 && a1.dataset.nav,
        second: a2 && a2.dataset.nav,
        crumb: document.getElementById('shell-crumb').textContent
      };
    });
    assert.equal(r.first, 'networks');
    assert.equal(r.second, 'vm');
    assert.ok(r.crumb.includes('가상 머신'));
  });
});

test('statusbar 클릭: navigateTo + Critical 세그먼트 filterState', async () => {
  await withPage(MODS, async (page) => {
    await boot(page);
    const r = await page.evaluate((snap) => {
      PCV.shell.mount();
      PCV.shell.update(snap);
      const segs = document.querySelectorAll('#shell-statusbar .seg');
      segs[2].click();                 
      return { nav: window._navCalls.slice(), q: location.search };
    }, SNAP);
    assert.deepEqual(r.nav, ['mon-alerts']);
    assert.ok(r.q.includes('severity=crit'), r.q);
  });
});

  
                  
                                                  
                                                                  
               
   
test('사이드바 항목 클릭: 이동 확정 후 모바일 오버레이 닫힘', async () => {
  await withPage(MODS, async (page) => {
    await boot(page);
    const r = await page.evaluate(() => {
      window._closeCalls = 0;
      window.closeMobileSB = () => { window._closeCalls++; };
      window.navigateTo = (n, hooks) => {
        window._navCalls.push(n);
        if (hooks && typeof hooks.after === 'function') hooks.after();
        return true;
      };
      PCV.shell.mount();
      document.querySelector('#shell-sidebar .shell-navitem[data-nav="networks"]').click();
      const afterCommit = window._closeCalls;
      window.navigateTo = () => false;                            
      document.querySelector('#shell-sidebar .shell-navitem[data-nav="storage"]').click();
      return { nav: window._navCalls.slice(), afterCommit, afterCancel: window._closeCalls };
    });
    assert.deepEqual(r.nav, ['networks']);
    assert.equal(r.afterCommit, 1);
    assert.equal(r.afterCancel, 1);                          
  });
});

test('사이드바 키보드: ↑/↓ roving + Enter 활성', async () => {
  await withPage(MODS, async (page) => {
    await boot(page);
    const r = await page.evaluate(() => {
      PCV.shell.mount();
      const items = document.querySelectorAll('#shell-sidebar .shell-navitem');
      items[0].focus();
      items[0].dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true }));
      const focusedNav = document.activeElement.dataset.nav;
      document.activeElement.click();                                                       
      return { focusedNav, nav: window._navCalls.slice() };
    });
    assert.ok(r.focusedNav);
    assert.equal(r.nav.length, 1);
  });
});

test('768px 셸 control: 검색·아이콘·메뉴 target 40px 이상', async () => {
  await withPage(MODS, async (page) => {
    await page.setViewport({ width: 768, height: 900 });
    await boot(page);
    const r = await page.evaluate(() => {
      PCV.shell.mount();
      const iconButton = document.createElement('button');
      iconButton.className = 'shell-iconbtn';
      document.body.appendChild(iconButton);
      const menuButton = document.createElement('button');
      menuButton.className = 'mobile-menu-btn';
      document.body.appendChild(menuButton);
      const rect = selector => {
        const box = document.querySelector(selector).getBoundingClientRect();
        return { width: box.width, height: box.height };
      };
      return {
        search: rect('.shell-search'),
        iconButton: rect('.shell-iconbtn'),
        menuButton: rect('.mobile-menu-btn'),
        navItem: rect('.shell-navitem'),
      };
    });
    for (const key of ['search', 'iconButton', 'menuButton', 'navItem']) {
      assert.ok(r[key].width >= 40, `${key} width=${r[key].width}`);
      assert.ok(r[key].height >= 40, `${key} height=${r[key].height}`);
    }
  });
});

                                         
                                                                      
                                                                      
                                  
                                                               
                                                                            
                                                                        
                                                              
                                                               
                                     
const MODS_VM = [
  'ui/modules/endpoints.js', 'ui/modules/api.js', 'ui/modules/ui.js',
  'ui/modules/filter-state.js', 'ui/modules/uxlib.js',
  'ui/modules/modal-core.js', 'ui/modules/modal.js',
  'ui/modules/charts.js',
  'ui/modules/shell.js',
  'ui/modules/vm.js', 'ui/modules/vm-console.js', 'ui/modules/vm-lifecycle.js', 'ui/modules/vm-guest.js',
  'ui/modules/help.js',
  'ui/modules/nav.js'
];

                                                                        
                                                               
async function bootVm(page) {
  await page.evaluate(() => {
    window._DEBUG = true;
    window.authToken = 'test-token';
    window._L = (ko, en) => ko;
    window.t = key => key;
    window.currentTab = 'summary';
    window.unwrapData = r => r == null ? r
      : (r.data !== undefined ? r.data : (r.result !== undefined ? r.result : r));
    window.unwrapList = r => Array.isArray(r) ? r
      : (Array.isArray(window.unwrapData(r)) ? window.unwrapData(r) : []);
    window.fetchGet = url => fetch(url).then(async response => {
      const body = await response.json().catch(() => null);
      if (!response.ok) throw new Error((body && body.error && body.error.message) || `HTTP ${response.status}`);
      return body;
    });
    window.fetchPost = (url, body) => fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body)
    }).then(r => r.json());
    window.vmList = [{ name: 'a', state: 'running' }, { name: 'b', state: 'shutoff' }];
    window.selectedVmIndex = 0;
    window.checkedVms = new Set();
    window.sortField = 'name'; window.sortDirection = 1;
    window.lastLoadTime = Date.now();
    window._navCalls = [];
    window.navigateTo = (n) => { window._navCalls.push(n); return true; };
    window.renderContent = () => {};                                              
  });
}

test('vm 화면: 2열 접합 — #vl 좌열 + 상세 탭 우열', async () => {
  await withPage(MODS_VM, async (page) => {
    await bootVm(page);
    const r = await page.evaluate(() => {
      const b = document.getElementById('cb');
      window.renderVmScreen(b, vmList[0], 'summary');
      return {
        vl: !!document.querySelector('#cb #vl'),
        vc: document.getElementById('vc') && document.getElementById('vc').textContent,
        tabs: [...document.querySelectorAll('#cb .vm-tabstrip [data-t]')].map(x => x.dataset.t),
        detail: !!document.getElementById('vm-detail'),
        bbtn: !!document.getElementById('bbtn')
      };
    });
    assert.ok(r.vl); assert.ok(r.detail); assert.ok(r.bbtn);
    assert.equal(r.vc, '2');
    assert.deepEqual(r.tabs, ['summary', 'console', 'snapshots', 'performance', 'timeline']);
  });
});

test('vm 화면: #vl 부재 시 render() no-op (타 화면에서 10s 폴링 안전)', async () => {
  await withPage(MODS_VM, async (page) => {
    await bootVm(page);
    const ok = await page.evaluate(() => { try { window.render(); return true; } catch (e) { return e.message; } });
    assert.equal(ok, true);
  });
});

  
                                                               
                                                                 
                                           
   
test('refreshVmDetail: j 키 탐색 후 #vm-detail이 새 선택 VM을 반영', async () => {
  await withPage(MODS_VM, async (page) => {
    await bootVm(page);
    const r = await page.evaluate(() => {
                                                     
                                                     
      window.renderSummary = function (b, v) {
        PCV.uxlib.clearEl(b);
        b.appendChild(document.createTextNode('summary:' + (v && v.name)));
      };
      window.currentTab = 'summary';
      const b = document.getElementById('cb');
      window.renderVmScreen(b, vmList[0], 'summary');
      const before = { idx: window.selectedVmIndex, detail: document.getElementById('vm-detail').textContent };
      document.dispatchEvent(new KeyboardEvent('keydown', { key: 'j', bubbles: true }));
      const after = { idx: window.selectedVmIndex, detail: document.getElementById('vm-detail').textContent };
      return { before, after };
    });
    assert.equal(r.before.idx, 0);
    assert.equal(r.before.detail, 'summary:a');
    assert.equal(r.after.idx, 1);
    assert.equal(r.after.detail, 'summary:b');                                  
  });
});

test('refreshVmDetail: #vm-detail 부재 화면(대시보드 등)에서 no-op — loadAll()이 아무 화면에서나 호출해도 안전', async () => {
  await withPage(MODS_VM, async (page) => {
    await bootVm(page);
    const r = await page.evaluate(() => {
      window.currentTab = 'dashboard';
      var threw = null;
      try { window.refreshVmDetail(); } catch (e) { threw = e.message; }
      return { threw, hasDetail: !!document.getElementById('vm-detail') };
    });
    assert.equal(r.threw, null);
    assert.equal(r.hasDetail, false);
  });
});
