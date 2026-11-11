                                                                                                     
                                                                                    
                                                                                           
                                                                                                      
                                                                                          
                               
                                                                         
                                           
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const MODS = [
  'ui/i18n.js',
  'ui/modules/endpoints.js',
  'ui/modules/api.js',
  'ui/modules/ui.js',
  'ui/modules/filter-state.js',
  'ui/modules/uxlib.js',
  'ui/modules/modal-core.js',
  'ui/modules/modal.js',
  'ui/modules/charts.js',
  'ui/modules/metrics.js',
  'ui/modules/security.js',
  'ui/modules/monitor.js',
  'ui/modules/vm.js',
  'ui/modules/vm-console.js',
  'ui/modules/vm-lifecycle.js',
  'ui/modules/vm-guest.js',
  'ui/modules/container.js',
  'ui/modules/network.js',
  'ui/modules/storage.js',
  'ui/modules/help.js',
  'ui/modules/nav.js',
  'ui/modules/theme.js',
  'ui/modules/accounts.js',
  'ui/modules/advanced.js',
  'ui/modules/selfhealing.js',
  'ui/modules/mobile.js',
  'ui/modules/shell.js',
  'ui/app.js',
];

                                                         
const ROUTES = {
  '/api/v1/rpc': { body: { jsonrpc: '2.0', id: 'gl1', result: [] } },
  '/api/v1/gpu/list': { body: [] },
  '/api/v1/gpu/attach': { body: { result: { ok: true } } },
  '/api/v1/gpu/detach': { body: { result: { ok: true } } },
  '/api/v1/gpu/passthrough': { body: { result: { ok: true } } },
};

async function bootGpu(page, port) {
  await page.evaluate(async () => {
    const html = await fetch('/ui/index.html').then(response => response.text());
    const parsed = new DOMParser().parseFromString(html, 'text/html');
    document.body.innerHTML = parsed.body.innerHTML;
  });
  for (const moduleFile of MODS) {
    await page.addScriptTag({ url: `http://127.0.0.1:${port}/${moduleFile}` });
  }
  await page.evaluate(async () => {
    I18N.setLang('ko');
    const host = document.createElement('div');
    host.id = 'gpu-test-host';
    document.body.appendChild(host);
    await window.renderGpu(host);
  });
}

                                                           
function press(page, fn, pci) {
  return page.evaluate(async ({ fn, pci }) => {
    document.getElementById('gpu-pci').value = pci;
    await window[fn]();
    const out = document.getElementById('gpu-action-result');
    return {
      text: out.textContent,
      codes: Array.from(out.querySelectorAll('code')).map(c => c.textContent),
      bold: out.querySelectorAll('b').length,
    };
  }, { fn, pci });
}

const gpuWrites = requests => requests.filter(r => r.path !== '/api/v1/rpc' && r.path !== '/api/v1/gpu/list');
const rpcMethods = requests => requests.filter(r => r.path === '/api/v1/rpc').map(r => r.json && r.json.method);

test('GPU 할당 자리 버튼: 호스트 장치 화면으로 옮기고 요청을 보내지 않는다', async () => {
  await withPage([], async (page, { port, requests }) => {
    await bootGpu(page, port);
    const r = await page.evaluate(() => {
      const moved = [];
      window.navigateTo = name => { moved.push(name); };
      const host = document.getElementById('gpu-test-host');
      const button = Array.from(host.querySelectorAll('button')).find(b => b.textContent.trim() === '호스트 장치에서 할당');
      if (button) button.click();
      return {
        found: !!button, inline: button ? button.getAttribute('onclick') : 'missing', moved,
        vfio: Array.from(host.querySelectorAll('button')).some(b => b.textContent.includes('VFIO Passthrough')),
        vmInput: !!document.getElementById('gpu-vm'), pciInput: !!document.getElementById('gpu-pci'),
      };
    });
    assert.equal(r.found, true, "'VFIO Passthrough' 자리에 '호스트 장치에서 할당' 버튼");
    assert.equal(r.inline, null, '새 인라인 onclick 문자열 없이 함수 핸들러');
    assert.deepEqual(r.moved, ['host-devices']);
    assert.equal(r.vfio, false, "옛 'VFIO Passthrough' 버튼은 없다");
    assert.equal(r.vmInput, false, "'VM Name' 입력은 그 버튼만 썼으므로 함께 뺐다");
    assert.equal(r.pciInput, true, 'PCI 입력은 vGPU 안내가 쓴다');
    assert.deepEqual(gpuWrites(requests), [], 'GPU 할당 요청을 보내지 않는다');
    assert.ok(!rpcMethods(requests).some(m => /^device\.gpu\./.test(m)), 'device.gpu.* RPC도 없다');
  }, { routes: ROUTES });
});

test('gpuPassthrough(옛 이름): 요청 없이 호스트 장치 화면으로 옮긴다', async () => {
  await withPage([], async (page, { port, requests }) => {
    await bootGpu(page, port);
    const moved = await page.evaluate(async () => {
      const list = [];
      window.navigateTo = name => { list.push(name); };
      await window.gpuPassthrough();
      return list;
    });
    assert.deepEqual(moved, ['host-devices']);
    assert.deepEqual(gpuWrites(requests), []);
    assert.ok(!rpcMethods(requests).some(m => /^device\.gpu\./.test(m)));
  }, { routes: ROUTES });
});

test('GPU CLI 카드: pcvctl gpu attach·detach 두 줄이 있다', async () => {
  await withPage([], async (page, { port }) => {
    await bootGpu(page, port);
    const card = await page.evaluate(() => {
      const codes = Array.from(document.querySelectorAll('#gpu-test-host code')).map(c => c.textContent);
      const text = document.getElementById('gpu-test-host').textContent;
      return { codes, text };
    });
    for (const cmd of ['pcvctl gpu list', 'pcvctl gpu metrics', 'pcvctl gpu attach <vm> <pci>', 'pcvctl gpu detach <vm> <pci>']) {
      assert.ok(card.codes.includes(cmd), `CLI 카드에 ${cmd}: ${JSON.stringify(card.codes)}`);
    }
    assert.match(card.text, /정지 VM에 GPU 그룹 할당\(ADMIN\)/);
    assert.match(card.text, /정지 VM에서 GPU 그룹 해제\(ADMIN\)/);
  }, { routes: ROUTES });
});

test('vGPU 생성 버튼: 미지원 안내는 그대로다', async () => {
  await withPage([], async (page, { port, requests }) => {
    await bootGpu(page, port);
    const r = await press(page, 'gpuMdevCreate', '0000:01:00.0');
    assert.match(r.text, /vGPU\(mdev\) 생성은 이 빌드에서 지원하지 않습니다/);
    assert.deepEqual(gpuWrites(requests), []);
  }, { routes: ROUTES });
});

test('vGPU 생성 버튼: 빈 PCI면 입력 안내가 먼저다', async () => {
  await withPage([], async (page, { port, requests }) => {
    await bootGpu(page, port);
    const r = await press(page, 'gpuMdevCreate', '');
    assert.equal(r.text, 'PCI 주소를 입력하세요');
    assert.equal(r.bold, 0);
    assert.deepEqual(gpuWrites(requests), []);
  }, { routes: ROUTES });
});
