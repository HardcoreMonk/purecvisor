                                                                                          
                                                          
                                                               
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const MODS = ['ui/modules/metrics.js'];

test('push/window/latest/peak — 시간 창·링버퍼 상한·비유한값 무시', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate(() => {
      const M = PCV.metrics;
      const now = Date.now();
                                                          
                                  
      M.push('host.cpu', 10);
      M._buf('host.cpu')[0].t = now - 20 * 60e3;                             
      M.push('host.cpu', 50);
      M.push('host.cpu', NaN);                 
      M.push('host.cpu', Infinity);            
      for (let i = 0; i < 725; i++) M.push('cap.test', i);
      return {
        w15: M.window('host.cpu', '15m').map(s => s.v),
        w1h: M.window('host.cpu', '1h').map(s => s.v),
        latest: M.latest('host.cpu'),
        peak1h: M.peak('host.cpu', '1h'),
        capLen: M.window('cap.test', '1h').length,
        capFirst: M.window('cap.test', '1h')[0].v,
        emptyW: M.window('none', '15m'),
        emptyLatest: M.latest('none')
      };
    });
    assert.deepEqual(r.w15, [50]);
    assert.deepEqual(r.w1h, [10, 50]);
    assert.equal(r.latest, 50);
    assert.equal(r.peak1h, 50);
    assert.equal(r.capLen, 720);
    assert.equal(r.capFirst, 5);                                  
    assert.deepEqual(r.emptyW, []);
    assert.equal(r.emptyLatest, null);
  });
});

                                                        
                                                     
test('prune — prefix 후보 중 keepKeys 밖 키만 삭제 (점 포함 이름 안전)', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate(() => {
      const M = PCV.metrics;
      M.push('vm.web.cpu', 1);
      M.push('vm.db.prod.example.cpu', 2);                              
      M.push('vm.gone.cpu', 3);
      M.push('host.cpu', 4);
      const removed = M.prune('vm.', ['vm.web.cpu', 'vm.db.prod.example.cpu']);
      const emptyPrefix = M.prune('', ['vm.web.cpu']);
                                                                
                                               
      M.push('other.a', 1);
      M.push('other.b', 2);
      const removeAll = M.prune('other.', []);
      return {
        removed,
        emptyPrefix,
        removeAll,
        web: M.latest('vm.web.cpu'),
        dotted: M.latest('vm.db.prod.example.cpu'),
        gone: M.latest('vm.gone.cpu'),
        host: M.latest('host.cpu')
      };
    });
    assert.equal(r.removed, 1);
    assert.equal(r.web, 1);
    assert.equal(r.dotted, 2);
    assert.equal(r.gone, null, 'keepKeys 밖 키는 지워진다');
    assert.equal(r.host, 4, 'prefix 밖 키는 건드리지 않는다');
    assert.equal(r.emptyPrefix, 0, 'prefix 없으면 아무것도 지우지 않는다');
    assert.equal(r.removeAll, 2, 'keepKeys 가 비면 prefix 매치 전부 삭제');
  });
});
