                                                                                             
                                                                       
                                                                     
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

                                                             
const MODS = ['ui/modules/ui.js', 'ui/modules/uxlib.js', 'ui/modules/filter-state.js'];

test('serialize / parse round-trip', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate(() => {
      const fs = window.PCV.ui.filterState;
      return {
        ser: fs.serialize({ status: ['error', 'running'], type: ['vm'] }),
        serEmpty: fs.serialize({ status: [], type: ['vm'] }),
        parsed: fs.parse('?status=error,running&type=vm'),
      };
    });
    assert.equal(r.ser, 'status=error,running&type=vm');
    assert.equal(r.serEmpty, 'type=vm');
    assert.deepEqual(r.parsed, { status: ['error', 'running'], type: ['vm'] });
  });
});

test('parse is total — skips malformed percent-encoded pairs, never throws', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate(() => {
      const fs = window.PCV.ui.filterState;
      return {
        malformed: fs.parse('?status=%zz&type=vm'),                                                          
        allBad: fs.parse('?a=%zz&b=%'),                                                             
      };
    });
    assert.deepEqual(r.malformed, { type: ['vm'] });
    assert.deepEqual(r.allBad, {});
  });
});

test('apply updates URL and notifies subscribers', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate(() => {
      const fs = window.PCV.ui.filterState;
      let seen = null;
      fs.subscribe((s) => { seen = s; });
      fs.apply({ status: ['error'], type: ['vm'] });
      return { search: location.search, seen, current: fs.current() };
    });
    assert.equal(r.search, '?status=error&type=vm');
    assert.deepEqual(r.seen.status, ['error']);
    assert.deepEqual(r.current.type, ['vm']);
  });
});

test('serialize encodes special chars per-value, comma separator stays literal', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate(() => {
      const fs = window.PCV.ui.filterState;
      return {
        plain: fs.serialize({ status: ['error', 'running'] }),                  
        special: fs.serialize({ tag: ['a,b', 'c&d'] }),                                   
        round: fs.parse(fs.serialize({ tag: ['a,b', 'c&d'] })),          
      };
    });
    assert.equal(r.plain, 'status=error,running');              
    assert.equal(r.special, 'tag=a%2Cb,c%26d');                               
    assert.deepEqual(r.round, { tag: ['a,b', 'c&d'] });
  });
});

test('apply stores a copy — caller mutation does not corrupt internal state', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate(() => {
      const fs = window.PCV.ui.filterState;
      const arr = ['running'];
      fs.apply({ status: arr });
      arr.push('stopped');                            
      return fs.current().status;                       
    });
    assert.deepEqual(r, ['running']);
  });
});
