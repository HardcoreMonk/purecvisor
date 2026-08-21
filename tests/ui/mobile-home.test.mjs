                                                                                              
                                                                      
                                                                   
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const MODS = ['ui/modules/ui.js', 'ui/modules/uxlib.js', 'ui/modules/mobile.js'];

const DATA = {
  vms: [{ name: 'web', state: 'running' }, { name: 'db', state: 'shutoff' }],
  containers: [{ name: 'c1', state: 'RUNNING' }],
  alerts: [
    { severity: 'crit', message: 'disk full', timestamp: 200 },
    { level: 'warning', message: 'cpu high', timestamp: 100 },
  ],
  hostCpu: 72, hostMem: 40,
};

test('buildHome renders status bar, two gauges, quick actions, recent alerts', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate((d) => {
      const node = window.PCV.mobile.buildHome(d);
      document.getElementById('cb').appendChild(node);
      return {
        root: node.className,
        bars: node.querySelectorAll('.statusbar').length,
        gauges: node.querySelectorAll('.gauge').length,
        quick: node.querySelectorAll('.m-quick .btn').length,
        recent: node.querySelectorAll('.m-recent .pill').length,
        unresolved: node.querySelector('.statusbar .seg:last-child .seg-big').textContent,
        text: node.textContent,
      };
    }, DATA);
    assert.ok(r.root.includes('mscreen-body'));
    assert.equal(r.bars, 1);
    assert.equal(r.gauges, 2);                        
    assert.equal(r.quick, 3);                           
    assert.equal(r.recent, 2);                    
    assert.equal(r.unresolved, '2');                                            
    assert.ok(r.text.includes('72'));                    
  });
});

test('buildHome isolates malformed recent alerts with unknown empty fallbacks', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate(() => {
      const node = window.PCV.mobile.buildHome({
        vms: [],
        containers: [],
        alerts: [null, 'primitive alert', []],
        hostCpu: 0,
        hostMem: 0,
      });
      return {
        labels: Array.from(node.querySelectorAll('.m-recent .pill'), pill => pill.textContent),
        messages: Array.from(node.querySelectorAll('.m-recent .m-name'), message => message.textContent),
        unresolved: node.querySelector('.statusbar .seg:last-child .seg-big').textContent,
      };
    });

    assert.deepEqual(r.labels, ['UNKNOWN', 'UNKNOWN', 'UNKNOWN']);
    assert.deepEqual(r.messages, ['', '', '']);
    assert.equal(r.unresolved, '0');
  });
});

test('buildHome status bar segment onNavigate routes to the mobile tab', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate((d) => {
      window.PCV.mobile.mount();
      const node = window.PCV.mobile.buildHome(d);
      document.getElementById('m-screen').appendChild(node);
                                      
      const segs = node.querySelectorAll('.statusbar .seg');
      segs[segs.length - 1].click();
      return { active: window.PCV.mobile.activeTab };
    }, DATA);
    assert.equal(r.active, 'alerts');
  });
});
