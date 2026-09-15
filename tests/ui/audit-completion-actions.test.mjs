




import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const MODS = ['endpoints', 'api', 'ui', 'filter-state', 'uxlib', 'modal-core',
  'monitor', 'vm-console', 'vm-lifecycle', 'mobile', 'selfhealing'].map(x => 'ui/modules/' + x + '.js');

async function boot(page) {
  await page.evaluate(() => {
    Object.assign(window, {
      _L: (ko, en) => en, t: key => key, authToken: 'fixture', _DEBUG: false,
      currentUser: { role: 'ADMIN' }, currentTab: 'performance', selectedVmIndex: 0,
      checkedVms: new Set(), vmList: [{ name: 'vm-a', uuid: 'uuid-a', state: 'shutoff' }],
      cpuHistory: Array(60).fill(0), memHistory: Array(60).fill(0),
      auditRequests: [], loadAll: () => {}, render: () => {}, renderContent: () => {},
      addEvt: () => {}, toast: () => {}, alert: () => {}, createLineChart: () => {}
    });
    window.auditReply = (body, status = 200) => new Response(JSON.stringify(body), {
      status, headers: { 'content-type': 'application/json' }
    });
    window.auditHandler = () => auditReply({ error: { message: 'Controlled HTTP failure' } }, 503);
    window.fetch = async (url, options = {}) => {
      auditRequests.push({ url: String(url), method: options.method || 'GET',
        body: options.body ? JSON.parse(options.body) : null });
      return auditHandler(String(url), options);
    };
  });
}

test('자가치유 거부 입력창 취소는 요청하지 않고 빈 사유 확정과 구분한다', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    const rows = await page.evaluate(async () => {
      const rows = [];
      auditHandler = () => auditReply({ result: [] });
      for (const reason of [null, '', 'defer']) {
        window.prompt = () => reason;
        auditRequests.length = 0;
        await PCV.selfhealing.reject(123);
        rows.push(auditRequests.filter(r => r.body?.method === 'ai.healing.reject'));
      }
      return rows;
    });
    assert.equal(rows[0].length, 0);
    assert.equal(rows[1].length, 1);
    assert.equal(rows[1][0].body.params.reason, 'manual');
    assert.equal(rows[2][0].body.params.reason, 'defer');
  });
});

test('성능 Stack과 Auto 버튼은 실제 레이아웃을 양방향으로 전환한다', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    await page.evaluate(() => renderPerformance(document.getElementById('cb'), vmList[0]));
    for (const [label, height, grid] of [['Stack', '120px', false], ['Auto', '80px', true]]) {
      await page.evaluate(label => [...document.querySelectorAll('#cb button')]
        .find(button => button.textContent.includes(label)).click(), label);
      await page.waitForFunction(height => document.querySelector('#cg').parentElement.style.height === height, {}, height);
      assert.equal(await page.$eval('#cb', b => b.children[1].classList.contains('grid-2')), grid);
    }
  });
});

test('스냅샷 일괄 삭제 HTTP·전송 오류는 버튼을 복구해 재시도를 허용한다', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    const results = await page.evaluate(async () => {
      const results = [];
      for (const kind of ['http', 'transport']) {
        document.getElementById('cb').replaceChildren(
          PCV.uxlib.el('input', { id: 'sda-prefix', value: 'auto-' }),
          PCV.uxlib.el('input', { id: 'sda-keep', value: '2' }),
          PCV.uxlib.el('button', { id: 'sda-exec-btn' }, 'Delete'));
        auditHandler = () => kind === 'http'
          ? auditReply({ error: { message: 'Unavailable' } }, 503)
          : Promise.reject(Error('Disconnected'));
        await sdaExec('vm-a');
        results.push(document.getElementById('sda-exec-btn').disabled);
      }
      return results;
    });
    assert.deepEqual(results, [false, false]);
  });
});

test('히트맵은 현재 CPU·실제 0·미수집만 표시하며 가상 시간 이력을 만들지 않는다', async () => {
  await withPage(MODS, async page => {
    await boot(page);
    await page.evaluate(() => {
      auditHandler = () => auditReply({ data: [
        { name: 'vm-a', cpu_percent: 50 }, { name: 'vm-zero', live_cpu_pct: 0, cpu_percent: 90 },
        { name: 'vm-unknown' }
      ] });
      renderHeatmap(document.getElementById('cb'));
    });
    await page.waitForSelector('#cb tbody tr');
    assert.deepEqual(await page.$$eval('#cb tbody tr', rows => rows.map(r => r.cells[1].textContent)),
      ['50.0%', '0.0%', 'Unavailable']);
    assert.equal(await page.$$eval('#cb th', nodes => nodes.length), 2);
    assert.match(await page.$eval('#cb', n => n.textContent), /Historical readings are not shown/);
  });
});

for (const operation of ['mobile-stop', 'snapshot-create']) {
  test(`${operation}: 목록 재정렬은 대상 유지, 삭제·동명 교체·취소는 요청 없음`, async () => {
    await withPage(MODS, async page => {
      await boot(page);
      for (const change of ['unchanged', 'reordered', 'replaced', 'deleted', 'cancelled']) {
        const result = await page.evaluate(async ({ operation, change }) => {
          closeModal(true);
          auditRequests.length = 0;
          vmList = [{ name: 'vm-a', uuid: 'uuid-a', state: 'running', storage_type: 'zvol' },
            { name: 'vm-b', uuid: 'uuid-b', state: 'running', storage_type: 'zvol' }];
          selectedVmIndex = 0;
          let resolveConfirm;
          if (operation === 'mobile-stop') {
            document.getElementById('cb').replaceChildren(PCV.mobile.buildPower({ vms: vmList, containers: [] }));
            window.customConfirm = () => new Promise(resolve => { resolveConfirm = resolve; });
            [...document.querySelectorAll('#cb button')].find(n => /Stop|정지/.test(n.textContent)).click();
          } else {
            await takeSnap();
            document.getElementById('snap-name-input').value = 'audit-fixture';
          }
          if (change === 'reordered') vmList = [vmList[1], vmList[0]];
          if (change === 'replaced') vmList = [{ ...vmList[0], uuid: 'replacement' }, vmList[1]];
          if (change === 'deleted') vmList = [vmList[1]];
          let preview = '';
          if (operation === 'mobile-stop') resolveConfirm(change !== 'cancelled');
          else if (change === 'cancelled') closeModal(true);
          else {
            snapNameValidate();
            preview = document.getElementById('snap-preview').textContent;
            await snapCreateExec();
          }
          await new Promise(resolve => setTimeout(resolve, 25));
          return { requests: auditRequests.filter(r => r.method === 'POST'), preview };
        }, { operation, change });
        const sends = change === 'unchanged' || change === 'reordered';
        assert.equal(result.requests.length, sends ? 1 : 0, change);
        if (sends) assert.match(result.requests[0].url, /\/vms\/vm-a\/(stop|snapshot\/create)$/);
        if (operation === 'snapshot-create' && change !== 'cancelled') assert.match(result.preview, /vm-a@audit-fixture/);
      }
    });
  });
}



const GUEST_MODS = ['ui/i18n.js', ...['endpoints', 'ui', 'uxlib', 'modal-core', 'vm-guest']
  .map(x => 'ui/modules/' + x + '.js')];

async function bootGuest(page) {
  await page.evaluate(() => {
    Object.assign(window, {
      _L: (ko, en) => en, t: key => key, selectedVmIndex: 0,
      vmList: [{ name: 'vm-a', uuid: 'uuid-a' }, { name: 'vm-b', uuid: 'uuid-b' }],
      guestRequests: [], guestEvents: [], guestToasts: [],
      unwrapData: response => response.data ?? response,
      addEvt: message => guestEvents.push(message), toast: message => guestToasts.push(message),
      guestHandler: () => ({ data: {} })
    });
    window.fetchPost = async (url, body) => {
      guestRequests.push({ url, body });
      return guestHandler(url, body);
    };
  });
}

for (const operation of ['blkioGet', 'blkioSet']) {
  test(`${operation}: dialog 대상 유지, 재배열·삭제·동명 교체·취소 대조`, async () => {
    await withPage(GUEST_MODS, async page => {
      for (const change of ['unchanged', 'reordered', 'replaced', 'deleted', 'cancelled']) {
        await bootGuest(page);
        const result = await page.evaluate(async ({ operation, change }) => {
          closeModal(true);
          showBlkioEditor();
          const title = PCV.modalCore.currentBody().querySelector('h2').textContent;
          document.getElementById('blkio-rd-bytes').value = '7';
          document.getElementById('blkio-wr-bytes').value = '3';
          document.getElementById('blkio-rd-iops').value = '110';
          document.getElementById('blkio-wr-iops').value = '220';
          if (change === 'reordered') vmList.reverse();
          if (change === 'deleted') vmList.shift();
          if (change === 'replaced') vmList[0] = { name: 'vm-a', uuid: 'replacement' };
          if (change === 'cancelled') closeModal(true);
          await window[operation]();
          closeModal(true);
          return { title, requests: guestRequests };
        }, { operation, change });
        assert.match(result.title, /vm-a$/);
        const sends = change === 'unchanged' || change === 'reordered';
        assert.equal(result.requests.length, sends ? 1 : 0, change);
        if (sends) {
          assert.match(result.requests[0].url, /\/vms\/vm-a\/rpc$/);
          assert.equal(result.requests[0].body.params.name, 'vm-a', change);
          if (operation === 'blkioSet') assert.deepEqual(result.requests[0].body.params, {
            name: 'vm-a', read_bytes_sec: 7 * 1048576, write_bytes_sec: 3 * 1048576,
            read_iops_sec: 110, write_iops_sec: 220
          });
        }
      }
    });
  });
}

for (const late of ['success', 'failure']) {
  test(`memory stats: 이전 창의 늦은 ${late}는 새 VM 결과를 덮지 않는다`, async () => {
    await withPage(GUEST_MODS, async page => {
      await bootGuest(page);
      const result = await page.evaluate(async late => {
        const pending = {};
        guestHandler = (url, body) => new Promise((resolve, reject) => {
          pending[body.params.name] = { resolve, reject };
        });
        const first = showMemStats();
        closeModal(true);
        selectedVmIndex = 1;
        const second = showMemStats();
        pending['vm-b'].resolve({ data: { actual_balloon_kb: 2048 } });
        await second;
        const before = document.getElementById('mem-stats-body').textContent;
        if (late === 'failure') pending['vm-a'].reject(Error('Old VM failure'));
        else pending['vm-a'].resolve({ data: { actual_balloon_kb: 1048576 } });
        await first;
        return { before, after: document.getElementById('mem-stats-body').textContent,
          title: PCV.modalCore.currentBody().querySelector('h2').textContent };
      }, late);
      assert.match(result.title, /vm-b$/);
      assert.match(result.before, /2\.0 MB/);
      assert.equal(result.after, result.before);
    });
  });
}

test('memory stats: 현재 창의 전송 오류와 RPC 오류를 표시한다', async () => {
  await withPage(GUEST_MODS, async page => {
    await bootGuest(page);
    const rows = await page.evaluate(async () => {
      const rows = [];
      for (const kind of ['transport', 'rpc']) {
        guestHandler = () => kind === 'transport' ? Promise.reject(Error('Current failure'))
          : { error: { message: 'Current failure' } };
        await showMemStats();
        rows.push(document.getElementById('mem-stats-body').textContent);
        closeModal(true);
      }
      return rows;
    });
    for (const row of rows) assert.match(row, /Failed: Current failure/);
  });
});

test('blkioGet: 닫힌 창과 같은 창의 이전 조회는 최신 입력·오류를 덮지 않는다', async () => {
  await withPage(GUEST_MODS, async page => {
    await bootGuest(page);
    const rows = await page.evaluate(async () => {
      const rows = [];
      for (const scenario of ['new-dialog', 'newer-success', 'newer-error']) {
        const pending = [];
        guestHandler = () => new Promise(resolve => pending.push(resolve));
        showBlkioEditor();
        const first = blkioGet();
        if (scenario === 'new-dialog') {
          closeModal(true);
          selectedVmIndex = 1;
          showBlkioEditor();
        }
        const second = blkioGet();
        pending[1](scenario === 'newer-error' ? { error: { message: 'Latest failure' } }
          : { data: { read_bytes_sec: 2 * 1048576, write_bytes_sec: 3 * 1048576,
            read_iops_sec: 4, write_iops_sec: 5 } });
        await second;
        const snapshot = () => ({ inputs: [...PCV.modalCore.currentBody().querySelectorAll('input')]
          .map(input => input.value), status: document.getElementById('blkio-status').textContent });
        const before = snapshot();
        pending[0]({ data: { read_bytes_sec: 99 * 1048576 } });
        await first;
        rows.push({ scenario, before, after: snapshot() });
        closeModal(true);
      }
      return rows;
    });
    for (const row of rows) {
      assert.deepEqual(row.after, row.before, row.scenario);
      if (row.scenario === 'newer-error') assert.match(row.before.status, /Latest failure/);
      else assert.deepEqual(row.before.inputs, ['2', '3', '4', '5']);
    }
  });
});

test('blkioSet: 정상 자동 닫기와 수동 종료·겹친 dialog의 timer 소유권', async () => {
  await withPage(GUEST_MODS, async page => {
    await bootGuest(page);
    const rows = await page.evaluate(async () => {
      const rows = [];
      for (const scenario of ['normal', 'reopened', 'stacked']) {
        showBlkioEditor();
        const original = PCV.modalCore.currentDialog();
        await blkioSet();
        if (scenario === 'reopened') closeModal(true);
        if (scenario !== 'normal') showModal([
          PCV.uxlib.el('h2', null, 'Unrelated dialog')
        ], { replace: false });
        await new Promise(resolve => setTimeout(resolve, 1650));
        rows.push({ scenario, originalConnected: original.isConnected,
          title: PCV.modalCore.currentBody()?.querySelector('h2')?.textContent || null });
        closeModal(true);
      }
      return rows;
    });
    assert.deepEqual(rows, [
      { scenario: 'normal', originalConnected: false, title: null },
      { scenario: 'reopened', originalConnected: false, title: 'Unrelated dialog' },
      { scenario: 'stacked', originalConnected: false, title: 'Unrelated dialog' }
    ]);
  });
});

test('blkioSet: 닫힌 창의 적용 결과는 VM 기록을 남기고 새 창을 닫지 않는다', async () => {
  await withPage(GUEST_MODS, async page => {
    await bootGuest(page);
    const result = await page.evaluate(async () => {
      let resolveSet;
      guestHandler = () => new Promise(resolve => { resolveSet = resolve; });
      showBlkioEditor();
      const applying = blkioSet();
      closeModal(true);
      showModal([PCV.uxlib.el('h2', null, 'Unrelated dialog')]);
      resolveSet({ data: {} });
      await applying;
      await new Promise(resolve => setTimeout(resolve, 1650));
      return { title: PCV.modalCore.currentBody()?.querySelector('h2')?.textContent,
        events: guestEvents, toasts: guestToasts };
    });
    assert.equal(result.title, 'Unrelated dialog');
    assert.deepEqual(result.events, ['BlkIO set: vm-a R:0MB/s W:0MB/s']);
    assert.match(result.toasts[0], /vm-a$/);
  });
});

test('blkio: 새 조회는 이전 자동 닫기를 취소하고 늦은 적용 표시를 무효화한다', async () => {
  await withPage(GUEST_MODS, async page => {
    await bootGuest(page);
    const result = await page.evaluate(async () => {
      showBlkioEditor();
      await blkioSet();
      let resolveGet;
      guestHandler = () => new Promise(resolve => { resolveGet = resolve; });
      const getting = blkioGet();
      await new Promise(resolve => setTimeout(resolve, 1650));
      const survivedTimer = !!PCV.modalCore.currentBody();
      resolveGet({ data: { read_bytes_sec: 2 * 1048576 } });
      await getting;
      closeModal(true);
      showBlkioEditor();
      let resolveSet;
      guestHandler = () => new Promise(resolve => { resolveSet = resolve; });
      const applying = blkioSet();
      guestHandler = () => ({ error: { message: 'Latest request failed' } });
      await blkioGet();
      resolveSet({ data: {} });
      await applying;
      await new Promise(resolve => setTimeout(resolve, 1650));
      return { survivedTimer, status: document.getElementById('blkio-status')?.textContent };
    });
    assert.equal(result.survivedTimer, true);
    assert.match(result.status, /Latest request failed/);
  });
});



for (const operation of ['memory', 'get', 'set']) {
  test(`${operation}: 팔레트 아래 열린 원래 창에 성공·RPC/전송 실패를 표시한다`, async () => {
    await withPage(GUEST_MODS, async page => {
      await bootGuest(page);
      await page.evaluate(() => {
        PCV.filterEditionItems = items => items;
        window.pcvRoleAllows = () => false;
      });
      await page.addScriptTag({ url: new URL('ui/modules/nav.js', page.url()).href });
      for (const response of ['success', 'rpc', 'transport']) {
        const result = await page.evaluate(async ({ operation, response }) => {
          closeModal(true);
          let resolve, reject;
          guestHandler = () => new Promise((yes, no) => { resolve = yes; reject = no; });
          let task;
          if (operation === 'memory') task = showMemStats();
          else { showBlkioEditor(); task = operation === 'get' ? blkioGet() : blkioSet(); }
          const owner = PCV.modalCore.currentDialog();
          const body = owner.querySelector('.modal-body');
          openCmdPalette();
          const palette = PCV.modalCore.currentDialog();
          const paletteBefore = palette.textContent;
          if (response === 'transport') reject(Error('Covered transport failure'));
          else if (response === 'rpc') resolve({ error: { message: 'Covered RPC failure' } });
          else resolve({ data: { actual_balloon_kb: 2048, read_bytes_sec: 7 * 1048576 } });
          await task;
          const status = body.querySelector(operation === 'memory' ? '#mem-stats-body' : '#blkio-status').textContent;
          const readValue = body.querySelector('#blkio-rd-bytes')?.value;
          if (operation === 'set' && response === 'success')
            await new Promise(done => setTimeout(done, 1650));
          const result = { status, readValue, ownerConnected: owner.isConnected,
            paletteUnchanged: palette.open && palette.textContent === paletteBefore
              && PCV.modalCore.currentDialog() === palette };
          PCV.modalCore.closeDialog(palette);
          closeModal(true);
          return result;
        }, { operation, response });
        assert.equal(result.paletteUnchanged, true, response);
        assert.equal(result.ownerConnected, !(operation === 'set' && response === 'success'), response);
        if (response === 'transport') assert.match(result.status, /Covered transport failure/);
        else if (response === 'rpc') assert.match(result.status, /Covered RPC failure/);
        else if (operation === 'memory') assert.match(result.status, /2\.0 MB/);
        else if (operation === 'get') {
          assert.match(result.status, /vm.blkio_loaded/);
          assert.equal(result.readValue, '7');
        } else assert.match(result.status, /vm.blkio_applied/);
      }
    });
  });
}

test('guest 완료: native close 직후 DOM 정리 전에도 닫힌 창의 표시를 버린다', async () => {
  await withPage(GUEST_MODS, async page => {
    await bootGuest(page);
    const rows = await page.evaluate(async () => {
      const rows = [];
      for (const operation of ['memory', 'get', 'set']) {
        let resolve;
        guestHandler = () => new Promise(done => { resolve = done; });
        let task;
        if (operation === 'memory') task = showMemStats();
        else { showBlkioEditor(); task = operation === 'get' ? blkioGet() : blkioSet(); }
        const owner = PCV.modalCore.currentDialog();
        const body = owner.querySelector('.modal-body');
        const status = () => body.querySelector(operation === 'memory' ? '#mem-stats-body' : '#blkio-status').textContent;
        const before = status();
        owner.close();
        const closedBeforeDetach = !owner.open && owner.isConnected;
        resolve({ data: { actual_balloon_kb: 2048, read_bytes_sec: 7 * 1048576 } });
        await task;
        rows.push({ operation, before, after: status(), closedBeforeDetach });
        closeModal(true);
      }
      return rows;
    });
    for (const row of rows) {
      assert.equal(row.closedBeforeDetach, true, row.operation);
      assert.equal(row.after, row.before, row.operation);
    }
  });
});
