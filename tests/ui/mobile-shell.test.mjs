                                                                                        
                                                                                        
                                                                  
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const MODS = ['ui/modules/ui.js', 'ui/modules/uxlib.js', 'ui/modules/mobile.js'];

test('mount() adds body.mshell + .mnav(4 tabs) + #m-screen', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate(() => {
      window.PCV.mobile.mount();
      return {
        shell: document.body.classList.contains('mshell'),
        navItems: document.querySelectorAll('.mnav .mnav-item').length,
        screen: !!document.getElementById('m-screen'),
        active: window.PCV.mobile.activeTab,
      };
    });
    assert.equal(r.shell, true);
    assert.equal(r.navItems, 4);
    assert.equal(r.screen, true);
    assert.equal(r.active, 'home');
  });
});

test('showTab() switches active tab, marks nav .on, routes into #m-screen', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate(() => {
      window.PCV.mobile.mount();
      window.PCV.mobile.showTab('alerts');
      const on = document.querySelector('.mnav .mnav-item.on');
      return {
        active: window.PCV.mobile.activeTab,
        onTab: on ? on.getAttribute('data-tab') : null,
        screenHasContent: document.getElementById('m-screen').childElementCount > 0,
      };
    });
    assert.equal(r.active, 'alerts');
    assert.equal(r.onTab, 'alerts');
    assert.equal(r.screenHasContent, true);
  });
});

test('setBadges() sets badge text and toggles hidden', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate(() => {
      window.PCV.mobile.mount();
      window.PCV.mobile.setBadges({ alerts: 3, healing: 0 });
      const a = document.querySelector('.mnav .mnav-badge[data-badge="alerts"]');
      const h = document.querySelector('.mnav .mnav-badge[data-badge="healing"]');
      return { aText: a.textContent, aHidden: a.hasAttribute('hidden'), hHidden: h.hasAttribute('hidden') };
    });
    assert.equal(r.aText, '3');
    assert.equal(r.aHidden, false);
    assert.equal(r.hHidden, true);
  });
});

                          
                                                                   
                                       
                                                                           
test('mount(): 콜드스타트 #mon-alerts 딥링크는 알림 탭에 착지한다', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate(() => {
      location.hash = '#mon-alerts';
      window.PCV.mobile.mount();
      const on = document.querySelector('.mnav .mnav-item.on');
      return { active: window.PCV.mobile.activeTab, onTab: on ? on.getAttribute('data-tab') : null };
    });
    assert.equal(r.active, 'alerts');
    assert.equal(r.onTab, 'alerts');
  });
});

                                                             
                                        
test('mount(): 정규화된 #/heal 딥링크는 치유 탭에 착지한다', async () => {
  await withPage(MODS, async (page) => {
    const active = await page.evaluate(() => {
      location.hash = '#/healing';
      window.PCV.mobile.mount();
      return window.PCV.mobile.activeTab;
    });
    assert.equal(active, 'healing');
  });
});

                                                    
                                                              
test('mount(): 해시는 1회만 소비 — 재마운트는 사용자가 고른 탭을 지킨다', async () => {
  await withPage(MODS, async (page) => {
    const active = await page.evaluate(() => {
      location.hash = '#mon-alerts';
      window.PCV.mobile.mount();
      window.PCV.mobile.showTab('power');
      window.PCV.mobile.unmount();
      window.PCV.mobile.mount();                          
      return window.PCV.mobile.activeTab;
    });
    assert.equal(active, 'power');
  });
});

                                        
test('mount(): 모바일 4탭과 무관한 해시는 홈을 유지한다', async () => {
  await withPage(MODS, async (page) => {
    const active = await page.evaluate(() => {
      location.hash = '#dashboard';
      window.PCV.mobile.mount();
      return window.PCV.mobile.activeTab;
    });
    assert.equal(active, 'home');
  });
});

test('unmount() removes shell chrome', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate(() => {
      window.PCV.mobile.mount();
      window.PCV.mobile.unmount();
      return {
        shell: document.body.classList.contains('mshell'),
        nav: !!document.querySelector('.mnav'),
        screen: !!document.getElementById('m-screen'),
      };
    });
    assert.equal(r.shell, false);
    assert.equal(r.nav, false);
    assert.equal(r.screen, false);
  });
});

test('sync() mounts at ≤600px and unmounts above', async () => {
  await withPage(MODS, async (page) => {
    await page.setViewport({ width: 390, height: 844 });
    const mounted = await page.evaluate(() => {
      window.authToken = 't';
      window.PCV.mobile.sync();
      return window.PCV.mobile.isActive();
    });
    assert.equal(mounted, true);
    await page.setViewport({ width: 1200, height: 800 });
    const unmounted = await page.evaluate(() => { window.PCV.mobile.sync(); return window.PCV.mobile.isActive(); });
    assert.equal(unmounted, false);
  });
});

test('sync() auth-gates at phone width: login-active/no-token blocks mount, authed clears it', async () => {
  await withPage(MODS, async (page) => {
    await page.setViewport({ width: 390, height: 844 });
    const loggedOut = await page.evaluate(() => {
      window.authToken = '';
      document.body.classList.add('login-active');
      window.PCV.mobile.sync();
      return window.PCV.mobile.isActive();
    });
    assert.equal(loggedOut, false);
    const loggedIn = await page.evaluate(() => {
      window.authToken = 't';
      document.body.classList.remove('login-active');
      window.PCV.mobile.sync();
      return window.PCV.mobile.isActive();
    });
    assert.equal(loggedIn, true);
  });
});

test('status mappers reduce raw states to ok/warn/crit/idle', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate(() => ({
      vmRun: window.PCV.mobile.vmStatus('running'),
      vmPause: window.PCV.mobile.vmStatus('paused'),
      vmOff: window.PCV.mobile.vmStatus('shutoff'),
      ctrRun: window.PCV.mobile.ctrStatus('RUNNING'),
      ctrOff: window.PCV.mobile.ctrStatus('STOPPED'),
      sevCrit: window.PCV.mobile.sevStatus('crit'),
      sevWarn: window.PCV.mobile.sevStatus('warning'),
      sevInfo: window.PCV.mobile.sevStatus('info'),
      sevCritical: window.PCV.mobile.sevStatus('critical'),
    }));
    assert.equal(r.vmRun, 'ok');
    assert.equal(r.vmPause, 'warn');
    assert.equal(r.vmOff, 'idle');
    assert.equal(r.ctrRun, 'ok');
    assert.equal(r.ctrOff, 'idle');
    assert.equal(r.sevCrit, 'crit');
    assert.equal(r.sevWarn, 'warn');
    assert.equal(r.sevInfo, 'idle');
                                                                
                                                                     
    assert.equal(r.sevCritical, 'crit');
  });
});
