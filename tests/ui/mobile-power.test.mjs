                                                                                        
                                                                                  
                                                                   
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const MODS = ['ui/modules/ui.js', 'ui/modules/uxlib.js', 'ui/modules/mobile.js'];

const DATA = {
  vms: [{ name: 'web', state: 'running' }, { name: 'db', state: 'shutoff' }, { name: 'cache', state: 'paused' }],
  containers: [{ name: 'c1', state: 'RUNNING', ip_addr: '10.0.0.2' }, { name: 'c2', state: 'STOPPED' }],
};

test('buildPower renders VM + container rows with status dot/pill and power buttons', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate((d) => {
      const node = window.PCV.mobile.buildPower(d);
      document.getElementById('cb').appendChild(node);
      return {
        root: node.className,
        dots: node.querySelectorAll('.sdot').length,
        rows: node.querySelectorAll('.m-listcard').length,
                                                       
        consoleHints: node.querySelectorAll('.m-hint').length,
        text: node.textContent,
      };
    }, DATA);
    assert.ok(r.root.includes('mscreen-body'));
    assert.equal(r.rows, 5);                                 
    assert.equal(r.dots, 5);
    assert.ok(r.text.includes('web'));
    assert.ok(r.consoleHints >= 1);                             
  });
});

test('buildPower running VM exposes stop+suspend; stopped VM exposes start', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate((d) => {
      const node = window.PCV.mobile.buildPower(d);
      const cards = node.querySelectorAll('.m-vm');
      function labels(card) { return Array.from(card.querySelectorAll('button')).map((b) => b.textContent); }
      return { web: labels(cards[0]), db: labels(cards[1]), cache: labels(cards[2]) };
    }, DATA);
                              
    assert.equal(r.web.length, 2);
                      
    assert.equal(r.db.length, 1);
                             
    assert.equal(r.cache.length, 2);
  });
});
