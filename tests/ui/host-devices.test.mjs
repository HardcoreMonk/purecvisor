                                                                                              
                                                                                         
                                                                                                
                                                                                        
                                                                                      
                       
                                                                          
                                                               
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { withPage } from './harness.mjs';

                                                                                         
const MODS = [
  'ui/i18n.js', 'ui/modules/endpoints.js', 'ui/modules/api.js', 'ui/modules/ui.js', 'ui/modules/filter-state.js',
  'ui/modules/uxlib.js', 'ui/modules/modal-core.js', 'ui/modules/modal.js', 'ui/modules/charts.js', 'ui/modules/metrics.js',
  'ui/modules/security.js', 'ui/modules/monitor.js', 'ui/modules/vm.js', 'ui/modules/vm-console.js',
  'ui/modules/vm-lifecycle.js', 'ui/modules/vm-guest.js', 'ui/modules/container.js', 'ui/modules/network.js',
  'ui/modules/storage.js', 'ui/modules/host-devices.js', 'ui/modules/help.js', 'ui/modules/nav.js', 'ui/modules/theme.js',
  'ui/modules/accounts.js', 'ui/modules/advanced.js', 'ui/modules/selfhealing.js', 'ui/modules/mobile.js',
  'ui/modules/shell.js', 'ui/app.js',
];

const WIN = 'win-guest1';
const clone = value => JSON.parse(JSON.stringify(value));

                             
function asg(vm, extra = {}) {
  return { vm, vm_visible: true, vm_active: true, live: true, persistent: true, ...extra };
}
const usbAsg = (vm, extra = {}) => asg(vm, {
  missing: false, startup_policy: 'optional', stale_live_address: false, ambiguous: false, ...extra,
});
const gpuAsg = (vm, extra = {}) => asg(vm, { managed: 'no', ...extra });
const ifc = (cls, sub, proto, driver) => ({ class: cls, subclass: sub, protocol: proto, driver });

function usb(id, port, device, name, vendorName, deviceClass, interfaces, traits, assignments = [], extra = {}) {
  const [vendor, product] = id.split(':');
  const hidden = traits.includes('hub') || traits.includes('storage');
  return {
    kind: 'usb', group: traits.includes('bluetooth') ? 'bluetooth' : 'usb', id, present: true, name,
    vendor_name: vendorName, vendor_id: '0x' + vendor, product_id: '0x' + product,
    bus: Number(port.split('-')[0]), device, port, device_class: deviceClass, interfaces, traits,
    host_driver_bound: interfaces.some(i => i.driver && i.driver !== 'usbfs'), duplicate_on_host: false,
    hidden_by_default: hidden, assignable: !traits.includes('hub'), blocked_reason: traits.includes('hub') ? 'hub' : null,
    assignments, ...extra,
  };
}
const hub = (id, port, device, name, vendorName) =>
  usb(id, port, device, name, vendorName, '09', [ifc('09', '00', '00', 'hub')], ['hub']);

const IRIS = {
  kind: 'pci', group: 'gpu', id: '0000:00:02.0', present: true,
  name: 'Intel Corporation Alder Lake-P GT2 [Iris Xe Graphics]', vendor_name: 'Intel Corporation',
  pci_addr: '0000:00:02.0', vendor_id: '8086', device_id: '46a6', driver: 'i915', iommu_group: '0',
  boot_vga: true, display_connected: true, host_display: true, group_safe: true, group_vfio_ready: false,
  partial_assignment: false, hidden_by_default: false, assignable: false, blocked_reason: 'host_display',
  companions: [], assignments: [],
};
const RTX = {
  kind: 'pci', group: 'gpu', id: '0000:01:00.0', present: true,
  name: 'NVIDIA Corporation GA104 [GeForce RTX 3070 Ti]', vendor_name: 'NVIDIA Corporation',
  pci_addr: '0000:01:00.0', vendor_id: '10de', device_id: '2482', driver: 'vfio-pci', iommu_group: '17',
  boot_vga: false, display_connected: false, host_display: false, group_safe: true, group_vfio_ready: true,
  partial_assignment: false, hidden_by_default: false, assignable: true, blocked_reason: null,
  companions: [{
    pci_addr: '0000:01:00.1', class_code: '040300', name: 'NVIDIA Corporation GA104 High Definition Audio Controller',
    driver: 'vfio-pci', assignments: [gpuAsg(WIN)],
  }],
  assignments: [gpuAsg(WIN)],
};
const GONE_22AC = {
  kind: 'usb', group: 'usb', id: '1038:22ac', present: false, name: null, vendor_name: 'SteelSeries ApS',
  vendor_id: '0x1038', product_id: '0x22ac', hidden_by_default: false, assignable: false, blocked_reason: 'not_present',
  assignments: [usbAsg(WIN, { stale_live_address: true })],
};
const AX200 = usb('8087:0029', '3-10', 6, 'Intel Corp. AX200 Bluetooth', 'Intel Corp.', 'e0',
  [ifc('e0', '01', '01', 'btusb'), ifc('e0', '01', '01', 'btusb')], ['bluetooth']);

                                                           
const REAL = {
  devices: [
    IRIS, RTX,
    hub('174c:3274', '2-3', 2, 'ASMT2307', 'ASMT'),
    hub('2109:0817', '2-3.2', 3, 'VIA Labs, Inc. USB3.0 Hub', 'VIA Labs, Inc.'),
    hub('174c:2174', '3-3', 2, 'ASMT2307', 'ASMT'),
    usb('1038:22ad', '3-3.1', 5, 'SteelSeries Arctis Nova 7X', 'SteelSeries', '00',
      [ifc('01', '01', '00', 'usbfs'), ifc('01', '02', '00', 'usbfs'), ifc('03', '00', '00', 'usbfs')], ['audio'], [usbAsg(WIN)]),
    hub('2109:2817', '3-3.2', 4, 'VIA Labs, Inc. USB2.0 Hub', 'VIA Labs, Inc.'),
    usb('248a:8327', '3-3.2.2', 7, 'Telink Wireless Receiver', 'Telink', '00',
      [ifc('03', '01', '02', 'usbfs'), ifc('03', '01', '01', 'usbfs')], ['keyboard', 'mouse'], [usbAsg(WIN)]),
    usb('046d:c52b', '3-3.2.3', 8, 'Logitech USB Receiver', 'Logitech', '00',
      [ifc('03', '01', '01', 'usbhid'), ifc('03', '01', '02', 'usbhid'), ifc('03', '00', '00', 'usbhid')], ['keyboard', 'mouse']),
    usb('3554:fa09', '3-3.2.4', 9, 'Compx 2.4G Wireless Receiver', 'Compx', '00',
      [ifc('03', '01', '01', 'usbfs'), ifc('03', '01', '02', 'usbfs')], ['keyboard', 'mouse'], [usbAsg(WIN)]),
    usb('1c7a:0577', '3-3.4', 10, 'EgisTec EH577', 'EgisTec', 'ff', [ifc('ff', 'ff', '00', null)], []),
    hub('05e3:0608', '3-6', 3, 'USB2.0 Hub', 'Genesys Logic, Inc.'),
    usb('1d6b:a4a7', '3-7', 4, 'POROSVOC', 'POROSVOC', 'ef',
      [ifc('01', '01', '00', 'snd-usb-audio'), ifc('01', '02', '00', 'snd-usb-audio'), ifc('03', '00', '00', 'usbhid')], ['audio']),
    GONE_22AC,
    AX200,
  ],
  vms: [{ name: WIN, active: true, persistent: true }],
  sources: { usb: 'available', pci: 'available', names: 'available' },
  truncated: false,
};

                                                                                               
function full() {
  const data = clone(REAL);
  const at = id => data.devices.findIndex(d => d.id === id);
  data.devices.splice(at('174c:3274'), 0, {
    kind: 'pci', group: 'gpu', id: '0000:09:00.0', present: false, name: null, vendor_name: null, pci_addr: '0000:09:00.0',
    hidden_by_default: false, assignable: false, blocked_reason: 'not_present',
    assignments: [gpuAsg('gpu-lab', { vm_active: false, live: false })],
  });
  data.devices.splice(at('248a:8327'), 0, usb('054c:0ce6', '3-3.2.1', 11, 'Sony Interactive Entertainment DualSense Wireless Controller',
    'Sony Interactive Entertainment', '00', [ifc('03', '00', '00', 'usbfs')], [], [usbAsg(WIN, { live: false, startup_policy: 'mandatory' })]));
  data.devices.splice(at('1038:22ac'), 0,
    usb('1050:0407', '3-9', 12, 'Yubico YubiKey OTP+FIDO+CCID', 'Yubico', '00', [ifc('03', '01', '01', 'usbfs')], ['keyboard'],
      [usbAsg('dev-box', { vm_active: false, live: false }), usbAsg(WIN)]),
    { kind: 'usb', group: 'usb', id: '0951:1666', present: false, name: 'Kingston DataTraveler 100 G3', vendor_name: 'Kingston Technology',
      vendor_id: '0x0951', product_id: '0x1666', hidden_by_default: false, assignable: false, blocked_reason: 'not_present',
      assignments: [usbAsg('scratch-vm', { persistent: false, stale_live_address: true, startup_policy: null })] });
  data.vms = [
    { name: 'dev-box', active: false, persistent: true },
    { name: 'gpu-lab', active: false, persistent: true },
    { name: 'scratch-vm', active: true, persistent: false },
    { name: WIN, active: true, persistent: true },
  ];
  return data;
}

                                                                           
function maskFor(data, own) {
  const out = clone(data);
  const mask = list => (list || []).forEach(a => {
    if (!own.includes(a.vm)) { a.vm = null; a.vm_visible = false; }
  });
  out.devices.forEach(d => { mask(d.assignments); (d.companions || []).forEach(c => mask(c.assignments)); });
  out.vms = out.vms.filter(v => own.includes(v.name));
  return out;
}
const dev = (data, id) => data.devices.find(d => d.id === id);

                                                                
                                                                                                            
          
                                                                                                          
                                                                                                        
                                                                   
                                                                                           
                                                                        
                                                               
                                                                                      
                                                  
const RPC_HTTP_STATUS = new Map([
  [-32602, 400],
  [-32601, 404], [-32001, 404], [-32005, 404],
  [-32002, 409], [-32004, 409], [-32012, 409], [-32013, 409],
  [-32003, 504],
  [-32006, 403],
  [-32009, 401],
  [-32011, 429],
]);
                                                                                                                      
function restEnvelope(reply) {
  if (reply.gateway) {
    const g = reply.gateway;
    return { status: g.status, body: { error: { code: g.code, message: g.message } } };
  }
  if (reply.error) {
    const code = typeof reply.error.code === 'number' ? reply.error.code : -32000;
    return { status: RPC_HTTP_STATUS.get(code) ?? 500, body: { error: reply.error } };
  }
  return { status: 200, body: { data: reply.result } };
}

                                                                                                            
                                                                                                                 
function rpc(state) {
  return {
    '/api/v1/rpc': async record => {
      const method = record.json && record.json.method;
      if (state.delays && state.delays[method]) await new Promise(resolve => setTimeout(resolve, state.delays[method]));
      if (method === 'host.device.list') return restEnvelope(state.listReply ? state.listReply(record) : { result: state.list });
      return restEnvelope(state.reply ? state.reply(method, record.json.params || {}) : { result: { ok: true } });
    },
  };
}
const calls = (requests, method) => requests.filter(r => r.json && r.json.method === method);

async function boot(page, port, role, lang = 'ko') {
  await page.evaluate(async () => {
    const html = await fetch('/ui/index.html').then(response => response.text());
    const parsed = new DOMParser().parseFromString(html, 'text/html');
    document.body.replaceChildren(...[...parsed.body.childNodes].map(node => document.importNode(node, true)));
  });
  for (const moduleFile of MODS) await page.addScriptTag({ url: `http://127.0.0.1:${port}/${moduleFile}` });
  await page.evaluate(async ({ role, lang }) => {
    I18N.setLang(lang);
    window.authToken = 'hd-test-token';
    window.currentUser = role ? { role } : null;
    const host = document.createElement('div');
    host.id = 'hd-host';
    document.body.appendChild(host);
    await window.renderHostDevices(host);
  }, { role, lang });
}

                                                 
function readTable(page) {
  return page.evaluate(() => {
    const host = document.getElementById('hd-host');
    const text = node => (node ? node.textContent.replace(/\s+/g, ' ').trim() : null);
    return {
      heads: [...host.querySelectorAll('thead th')].map(th => ({ text: text(th), scope: th.getAttribute('scope') })),
      groups: [...host.querySelectorAll('tbody[data-hd-group]')].map(tb => ({
        group: tb.getAttribute('data-hd-group'),
        label: text(tb.querySelector('tr.hd-grp')),
        rows: [...tb.querySelectorAll('tr[data-hd-row]')].map(tr => tr.getAttribute('data-hd-row')),
      })),
      rows: Object.fromEntries([...host.querySelectorAll('tr[data-hd-row]')].map(tr => {
        const tds = tr.querySelectorAll('td');
        return [tr.getAttribute('data-hd-row'), {
          group: tr.closest('tbody').getAttribute('data-hd-group'),
          device: text(tds[0]),
          id: text(tds[1]), idTitle: tds[1].getAttribute('title'), idMono: tds[1].classList.contains('font-mono'),
          assign: text(tds[2]),
          vmBadges: [...tds[2].querySelectorAll('.hd-vm')].map(text),
          hostBadge: text(tds[2].querySelector('.hd-host')),
          status: [...tds[3].querySelectorAll('.hd-line')].map(text),
          warn: [...tds[3].querySelectorAll('.hd-warn')].map(text),
          action: text(tds[4]),
          buttons: [...tds[4].querySelectorAll('button')].map(b => ({
            text: text(b), disabled: b.disabled, label: b.getAttribute('aria-label'),
            describedBy: b.getAttribute('aria-describedby'),
            reason: b.getAttribute('aria-describedby') ? text(document.getElementById(b.getAttribute('aria-describedby'))) : null,
            key: b.getAttribute('data-hd-key'), kind: b.getAttribute('data-hd-kind'),
          })),
        }];
      })),
      chips: [...host.querySelectorAll('.filterbar .chip')].map(c => ({
        label: text(c.querySelector('span:not(.chip-c)')), count: text(c.querySelector('.chip-c')),
        pressed: c.getAttribute('aria-pressed'), val: c.getAttribute('data-val'),
      })),
      hiddenNote: text(host.querySelector('.hd-hidden-note')),
      hiddenToggle: (b => b && { text: text(b), pressed: b.getAttribute('aria-pressed') })(host.querySelector('[data-hd-ctl="hidden"]')),
      notes: [...host.querySelectorAll('.hd-note')].map(text),
      result: text(host.querySelector('.hd-result')),
      all: text(host),
    };
  });
}

                                                      
function press(page, id, kind) {
  return page.evaluate(({ id, kind }) => {
    const tr = [...document.querySelectorAll('#hd-host tr[data-hd-row]')].find(row => row.getAttribute('data-hd-row') === id);
    const button = tr && [...tr.querySelectorAll('button')].find(b => b.getAttribute('data-hd-kind') === kind);
    if (!button) throw new Error('button not found: ' + id + ' ' + kind);
    button.click();
  }, { id, kind });
}
                                  
function readDialog(page) {
  return page.evaluate(() => {
    const dlg = window.PCV.modalCore.currentDialog();
    if (!dlg) return null;
    const text = node => (node ? node.textContent.replace(/\s+/g, ' ').trim() : null);
    const sel = dlg.querySelector('select');
    const boxes = [...dlg.querySelectorAll('input[type="checkbox"]')];
    const buttons = [...dlg.querySelectorAll('button')];
    return {
      open: dlg.open, title: text(dlg.querySelector('h2')), text: text(dlg),
      options: sel ? [...sel.options].map(o => ({ value: o.value, text: text(o), disabled: o.disabled })) : null,
      selected: sel ? sel.value : null,
      boxes: boxes.map(b => ({ id: b.id, checked: b.checked, disabled: b.disabled, label: text(b.closest('label')) })),
      buttons: buttons.map(b => ({ text: text(b), disabled: b.disabled })),
      focused: text(document.activeElement),
      focusedInDialog: dlg.contains(document.activeElement),
      alert: text(dlg.querySelector('[role="alert"]')),
      imgs: dlg.querySelectorAll('img').length,
    };
  });
}
function choose(page, value) {
  return page.evaluate(value => {
    const sel = window.PCV.modalCore.currentDialog().querySelector('select');
    sel.value = value;
    sel.dispatchEvent(new Event('change', { bubbles: true }));
  }, value);
}
function toggleBox(page, index) {
  return page.evaluate(index => {
    window.PCV.modalCore.currentDialog().querySelectorAll('input[type="checkbox"]')[index].click();
  }, index);
}
function clickDialog(page, label) {
  return page.evaluate(label => {
    const button = [...window.PCV.modalCore.currentDialog().querySelectorAll('button')]
      .find(b => b.textContent.trim() === label);
    if (!button) throw new Error('dialog button not found: ' + label);
    button.click();
  }, label);
}
const waitNoDialog = page => page.waitForFunction(() => !window.PCV.modalCore.currentDialog(), { timeout: 5000 });
                                                                                 
const markTable = page => page.evaluate(() => {
  const table = document.querySelector('#hd-host table');
  if (table) table.dataset.stale = '1';
});
const waitRepaint = page => page.waitForFunction(() => {
  const table = document.querySelector('#hd-host table');
  return table && !table.dataset.stale;
}, { timeout: 5000 });
                                   
const settle = page => page.evaluate(() => new Promise(resolve => setTimeout(resolve, 150)));

const RUN_GONE = '호스트에서 빠짐 · 다시 꽂아도 자동으로 안 붙음(해제 뒤 다시 할당)';
const RUN_OLD = '옛 장치 번호 · 다시 연결 필요(해제 뒤 다시 할당)';
const VIEW_ONLY = '보기 권한만';
const ADMIN_ONLY = '관리자 권한 필요';
const OTHER_VM = '다른 VM이 사용 중';
const DUP = '같은 ID 장치가 둘 이상 꽂혀 있습니다 — 하나를 뺀 뒤 할당';
const STOP_FIRST = 'VM을 끈 뒤 가능';
const NO_PCI = '해제 불가 — 호스트에 장치가 없음(정의는 운영자가 정리)';

                           
                                                                                               
                                                                  
                                                                 
                                                                                         
                                                                              
test('목 봉투: 실제 REST 봉투와 같고, 목 응답 본문은 restEnvelope로만 만든다', () => {
  const ok = restEnvelope({ result: { devices: [] } });
  assert.equal(ok.status, 200);
  assert.deepEqual(ok.body, { data: { devices: [] } }, '성공은 {data} — result·jsonrpc·id가 없다');
  for (const [code, status] of [[-32602, 400], [-32001, 404], [-32002, 409], [-32004, 409], [-32006, 403], [-32000, 500], [-32603, 500]]) {
    const err = restEnvelope({ error: { code, message: 'm' } });
    assert.equal(err.status, status, String(code));
    assert.deepEqual(err.body, { error: { code, message: 'm' } }, String(code));
  }
  const gw = restEnvelope({ gateway: { status: 500, code: 'NO_RESPONSE', message: 'No response from daemon' } });
  assert.equal(gw.status, 500);
  assert.deepEqual(gw.body, { error: { code: 'NO_RESPONSE', message: 'No response from daemon' } });

  const src = rel => readFileSync(new URL('../../' + rel, import.meta.url), 'utf8');
  const codes = Object.fromEntries([...src('src/modules/dispatcher/rpc_utils.h').matchAll(/(PURE_RPC_ERR_\w+)\s*=\s*(-\d+)/g)]
    .map(m => [m[1], Number(m[2])]));
  const policy = src('src/api/rest_policy.c');
  const fn = policy.slice(policy.indexOf('\npcv_rest_http_status_for_rpc_error('));
  const fnText = fn.slice(0, fn.indexOf('\n}'));
  const fromC = new Map();
  for (const m of fnText.matchAll(/if \(([\s\S]*?)\)\s*return (\d+);/g)) {
    for (const name of m[1].match(/PURE_RPC_ERR_\w+/g)) fromC.set(codes[name], Number(m[2]));
  }
  assert.ok(fromC.size >= 10 && /return 500;\s*$/.test(fnText), 'rest_policy.c 상태 표를 읽었다(나머지는 500)');
  const byCode = map => [...map].sort((a, b) => a[0] - b[0]);
  assert.deepEqual(byCode(RPC_HTTP_STATUS), byCode(fromC), 'RPC_HTTP_STATUS = pcv_rest_http_status_for_rpc_error');
  const resp = src('src/api/rest_response.c');
  assert.match(resp, /json_object_has_member\(obj, "result"\)[\s\S]{0,300}\{\\"data\\":%s\}[\s\S]{0,60}status = 200;/,
    'rest_response.c: result → {"data":…} 200');
  assert.match(resp, /\{\\"error\\":%s\}[\s\S]{0,40}status = pcv_rest_http_status_for_rpc_error\(code\);/,
    'rest_response.c: error → {"error":…} + 매핑 상태');

  const self = readFileSync(new URL(import.meta.url), 'utf8');
  const bodyKey = new RegExp('\\bbody' + '\\s*:', 'g');                              
  const inEnvelope = (restEnvelope.toString().match(bodyKey) || []).length;
  assert.ok(inEnvelope >= 1, 'restEnvelope가 응답 본문을 만든다');
  assert.equal((self.match(bodyKey) || []).length, inEnvelope, '응답 본문 키는 restEnvelope 안에만 있다');
});

                              

test('describe: 1038:22ac는 호스트에서 빠짐이고 실행 중이 아니다', async () => {
  await withPage([], async (page, { port }) => {
    await boot(page, port, 'ADMIN');
    const r = await page.evaluate(gone => {
      const d = window.PCV.hostDevices.describe;
      const replug = JSON.parse(JSON.stringify(gone));
      replug.present = true;                                                                            
      return { gone: d(gone, gone.assignments[0]), replug: d(replug, replug.assignments[0]) };
    }, GONE_22AC);
    assert.deepEqual([r.gone.run, r.gone.persist, r.gone.warn], [RUN_GONE, '영구', true]);
    assert.deepEqual([r.replug.run, r.replug.persist, r.replug.warn], [RUN_OLD, '영구', true]);
    const t = await readTable(page);
    assert.deepEqual(t.rows['1038:22ac'].status, [RUN_GONE + ' · 영구']);
    assert.ok(t.rows['1038:22ac'].warn.includes(RUN_GONE), '빠짐은 경고 색 + 글');
    assert.ok(!t.rows['1038:22ac'].status.join(' ').includes('실행 중'), '"실행 중"이 아니다(R1)');
    assert.deepEqual(t.rows['1038:22ad'].status, ['실행 중 · 영구'], '붙어 있는 장치는 실행 중 · 영구');
  }, { routes: rpc({ list: REAL }) });
});

test('describe: 빠진 live 전용 항목도 경고', async () => {
  await withPage([], async (page, { port }) => {
    await boot(page, port, 'ADMIN');
    const t = await readTable(page);
    const row = t.rows['0951:1666'];
    assert.deepEqual(row.status, [RUN_GONE + ' · 지금만(VM을 끄면 빠짐)']);
    assert.ok(row.warn.includes(RUN_GONE));
    const r = await page.evaluate(() => {
      const d = window.PCV.hostDevices.describe;
      const row = { kind: 'usb', present: false };
      return d(row, { vm: 'v', vm_visible: true, vm_active: true, live: true, persistent: false });                      
    });
    assert.deepEqual([r.run, r.persist, r.warn], [RUN_GONE, '지금만(VM을 끄면 빠짐)', true]);
  }, { routes: rpc({ list: full() }) });
});

test('describe: 영구만·지금은 안 붙음', async () => {
  await withPage([], async (page, { port }) => {
    await boot(page, port, 'ADMIN');
    const t = await readTable(page);
    assert.deepEqual(t.rows['054c:0ce6'].status, ['영구만 · 지금은 안 붙음(다음 시작 때 붙음)']);
    const r = await page.evaluate(() => window.PCV.hostDevices.describe({ kind: 'usb', present: false },
      { vm: 'v', vm_visible: true, vm_active: true, live: false, persistent: true }));
    assert.deepEqual([r.run, r.persist, r.warn], [null, '영구만 · 호스트에 안 꽂힘', true]);
  }, { routes: rpc({ list: full() }) });
});

test('describe: 꺼진 VM 영구', async () => {
  await withPage([], async (page, { port }) => {
    await boot(page, port, 'ADMIN');
    const r = await page.evaluate(() => {
      const d = window.PCV.hostDevices.describe;
      return {
        off: d({ kind: 'usb', present: true }, { vm: 'v', vm_visible: true, vm_active: false, live: false, persistent: true }),
        placeholder: d({ kind: 'usb', present: true }, { vm: 'v', vm_visible: true, vm_active: true, live: true, persistent: true, missing: true }),
        ambiguous: d({ kind: 'usb', present: true }, { vm: 'v', vm_visible: true, vm_active: true, live: true, persistent: false, ambiguous: true }),
      };
    });
    assert.deepEqual([r.off.run, r.off.persist, r.off.warn], [null, '영구(VM 꺼짐 — 다음 시작 때 붙음)', false]);
    assert.deepEqual([r.placeholder.run, r.placeholder.warn], ['자리만 있음 · 장치 없이 시작됨', true]);
    assert.deepEqual([r.ambiguous.run, r.ambiguous.warn], ['같은 ID 장치 여럿 · 어느 것인지 모름', true]);
    const t = await readTable(page);
    assert.deepEqual(t.rows['1050:0407'].status, ['영구(VM 꺼짐 — 다음 시작 때 붙음)', '실행 중 · 영구'],
      '할당이 둘이면 할당마다 한 줄');
    assert.deepEqual(t.rows['1050:0407'].vmBadges, ['dev-box · 꺼짐', WIN], '꺼진 VM 배지 안에 "꺼짐"');
  }, { routes: rpc({ list: full() }) });
});

                                 

function states(page, cases) {
  return page.evaluate(cases => cases.map(c => window.PCV.hostDevices.actionState(c.row, c.ctx)), cases);
}

test('actionState: 우선순위 VIEWER > 비ADMIN GPU > 가려짐 > 상태', async () => {
  await withPage([], async (page, { port }) => {
    await boot(page, port, 'ADMIN');
    const data = full();
    const dupRow = { ...clone(dev(data, '046d:c52b')), duplicate_on_host: true, assignable: false, blocked_reason: 'duplicate_on_host' };
    const hiddenRow = maskFor(data, []).devices.find(d => d.id === '1038:22ad');
    const rtx = dev(data, '0000:01:00.0');
    const unbound = { ...clone(RTX), assignments: [], companions: [], group_vfio_ready: false, assignable: false, blocked_reason: 'not_vfio_bound' };
    const vms = data.vms;
    const r = await states(page, [
      { row: dupRow, ctx: { role: 'VIEWER', vms: [] } },
      { row: hiddenRow, ctx: { role: 'VIEWER', vms: [] } },
      { row: rtx, ctx: { role: 'VIEWER', vms: [] } },
      { row: dupRow, ctx: { role: null, vms } },
      { row: rtx, ctx: { role: 'OPERATOR', vms } },
      { row: unbound, ctx: { role: 'OPERATOR', vms } },
      { row: hiddenRow, ctx: { role: 'OPERATOR', vms } },
      { row: dupRow, ctx: { role: 'OPERATOR', vms } },
    ]);
    assert.deepEqual(r[0], { buttons: [{ kind: 'assign', disabled: true }], text: null, reason: VIEW_ONLY }, '① 상태 이유보다 먼저');
    assert.deepEqual(r[1], { buttons: [], text: null, reason: VIEW_ONLY }, '① 가려진 할당이어도 보기 권한만');
    assert.equal(r[2].reason, VIEW_ONLY, '① 비ADMIN GPU보다 먼저');
    assert.ok(r[2].buttons.every(b => b.disabled));
    assert.equal(r[3].reason, VIEW_ONLY, '알 수 없는 역할은 닫힘 우선');
    assert.equal(r[4].reason, ADMIN_ONLY, '② 실행 중 VM 이유보다 먼저');
    assert.deepEqual(r[4].buttons.map(b => [b.kind, b.disabled]), [['release', true]]);
    assert.equal(r[5].reason, ADMIN_ONLY, '② vfio 이유보다 먼저');
    assert.deepEqual(r[6], { buttons: [], text: OTHER_VM, reason: null }, '③ 동작 없음');
    assert.deepEqual(r[7], { buttons: [{ kind: 'assign', disabled: true }], text: null, reason: DUP }, '④ 상태');
  }, { routes: rpc({ list: full() }) });
});

test('actionState: GPU 해제는 present·group_safe·정지 VM', async () => {
  await withPage([], async (page, { port }) => {
    await boot(page, port, 'ADMIN');
    const data = full();
    const rtx = dev(data, '0000:01:00.0');
    const stopped = clone(rtx);
    stopped.assignments[0].vm_active = false; stopped.assignments[0].live = false;
    const unsafe = { ...clone(stopped), group_safe: false, group_vfio_ready: false };
    const absent = dev(data, '0000:09:00.0');
    const free = { ...clone(RTX), assignments: [], companions: [] };
    const ctx = { role: 'ADMIN', vms: data.vms };
    const r = await states(page, [
      { row: rtx, ctx }, { row: stopped, ctx }, { row: unsafe, ctx }, { row: absent, ctx },
      { row: IRIS, ctx },
      { row: { ...free, group_safe: false, assignable: false, blocked_reason: 'iommu_group_unsafe' }, ctx },
      { row: { ...free, group_vfio_ready: false, assignable: false, blocked_reason: 'not_vfio_bound' }, ctx },
      { row: free, ctx: { role: 'ADMIN', vms: [{ name: WIN, active: true, persistent: true }] } },
      { row: free, ctx },
    ]);
    assert.deepEqual(r[0], { buttons: [{ kind: 'release', disabled: true, vm: WIN }], text: null, reason: STOP_FIRST });
    assert.deepEqual(r[1], { buttons: [{ kind: 'release', disabled: false, vm: WIN }], text: null, reason: null });
    assert.equal(r[2].reason, 'IOMMU 그룹에 다른 장치가 섞여 있어 화면에서 해제할 수 없습니다(정의는 운영자가 정리)');
    assert.deepEqual(r[3], { buttons: [], text: NO_PCI, reason: null });
    assert.deepEqual(r[4], { buttons: [], text: '할당 불가', reason: null });
    assert.equal(r[5].reason, 'IOMMU 그룹에 다른 장치가 섞여 있어 할당할 수 없습니다');
    assert.equal(r[6].reason, '호스트에서 GPU 그룹 전체를 vfio-pci에 먼저 묶어야 합니다(ADR-0047)');
    assert.equal(r[7].reason, STOP_FIRST, '정지 VM이 없으면 GPU 할당 불가');
    assert.deepEqual(r[8], { buttons: [{ kind: 'assign', disabled: false }], text: null, reason: null });
    const t = await readTable(page);
    assert.equal(t.rows['0000:00:02.0'].action, '할당 불가');
    assert.ok(t.rows['0000:00:02.0'].status[0].includes('호스트 화면 사용 중'));
    assert.equal(t.rows['0000:00:02.0'].buttons.length, 0, '호스트 화면 GPU에서 할당 창을 열 방법이 없다');
    assert.deepEqual(t.rows['0000:01:00.0'].buttons.map(b => [b.text, b.disabled, b.reason]), [['해제', true, STOP_FIRST]]);
  }, { routes: rpc({ list: full() }) });
});

test('actionState: 섞인 행 해제…는 보이는 VM만', async () => {
  const data = maskFor(full(), ['dev-box']);
  await withPage([], async (page, { port, requests }) => {
    await boot(page, port, 'OPERATOR');
    const r = await states(page, [{ row: dev(data, '1050:0407'), ctx: { role: 'OPERATOR', vms: data.vms } }]);
    assert.deepEqual(r[0], { buttons: [{ kind: 'release-multi', disabled: false }], text: null, reason: null });
    const t = await readTable(page);
    assert.deepEqual(t.rows['1050:0407'].vmBadges, ['dev-box · 꺼짐', '다른 VM'], '가려진 할당은 "다른 VM"');
    await press(page, '1050:0407', 'release-multi');
    const d = await readDialog(page);
    assert.deepEqual(d.options.map(o => o.value), ['', 'dev-box'], '보이는 VM만 고른다');
    assert.ok(d.buttons.find(b => b.text === '해제').disabled, 'VM을 고르기 전에는 해제 잠금');
    await choose(page, 'dev-box');
    await clickDialog(page, '해제');
    await waitNoDialog(page);
    assert.deepEqual(calls(requests, 'vm.usb.detach').map(c => c.json.params),
      [{ vm_id: 'dev-box', vendor_id: '0x1050', product_id: '0x0407' }]);
  }, { routes: rpc({ list: data }) });
});

test('actionState: ADMIN 빈 vms', async () => {
  await withPage([], async (page, { port }) => {
    await boot(page, port, 'ADMIN');
    const row = dev(REAL, '1c7a:0577');
    const r = await states(page, [
      { row, ctx: { role: 'ADMIN', vms: [] } },
      { row, ctx: { role: 'OPERATOR', vms: [] } },
    ]);
    assert.deepEqual(r[0], { buttons: [{ kind: 'assign', disabled: true }], text: null, reason: '할당할 VM이 없습니다' });
    assert.deepEqual(r[1], { buttons: [{ kind: 'assign', disabled: true }], text: null, reason: '할당할 수 있는 내 VM이 없습니다' });
    const t = await readTable(page);
    assert.deepEqual(t.rows['1c7a:0577'].buttons.map(b => [b.text, b.disabled, b.reason]), [['할당…', true, '할당할 VM이 없습니다']]);
  }, { routes: rpc({ list: { ...clone(REAL), vms: [] } }) });
});

                                

test('화면: 묶음·열·개수·숨김 보기', async () => {
  await withPage([], async (page, { port }) => {
    await boot(page, port, 'ADMIN');
    let t = await readTable(page);
    assert.deepEqual(t.heads, ['장치', 'ID', '할당', '상태', '동작'].map(text => ({ text, scope: 'col' })));
    assert.deepEqual(t.groups.map(g => [g.group, g.label]), [['gpu', 'GPU'], ['usb', 'USB'], ['bluetooth', '블루투스']]);
    for (const g of t.groups) for (const id of g.rows) assert.equal(t.rows[id].group, g.group, `${id}는 ${g.group} 묶음`);
    assert.deepEqual(t.groups[0].rows, ['0000:00:02.0', '0000:01:00.0'], '서버 순서 그대로');
    assert.deepEqual(t.chips.map(c => [c.label, c.count, c.pressed]), [['전체', '10', 'true'], ['할당됨', '5', 'false'], ['미할당', '5', 'false']]);
    assert.equal(t.hiddenNote, '허브·저장 장치 5개 숨김 보기');
    assert.deepEqual(t.hiddenToggle, { text: '보기', pressed: 'false' });
    assert.equal(Object.keys(t.rows).length, 10);
    assert.ok(!('174c:3274' in t.rows), '허브는 처음에 그리지 않는다');
    const rtx = t.rows['0000:01:00.0'];
    assert.deepEqual([rtx.id, rtx.idTitle, rtx.idMono], ['01:00.0', '0000:01:00.0', true]);
    assert.deepEqual(rtx.vmBadges, [WIN]);
    assert.ok(rtx.device.includes('+ NVIDIA Corporation GA104 High Definition Audio Controller(01:00.1) 함께 이동'), rtx.device);
    assert.deepEqual(rtx.status, ['실행 중 · 영구 · vfio-pci']);
    assert.equal(t.rows['0000:00:02.0'].hostBadge, '호스트', '미할당은 점선 "호스트"');
    assert.equal(t.rows['248a:8327'].device, 'Telink Wireless Receiver (키보드·마우스)');
    assert.equal(t.rows['1038:22ac'].device, 'SteelSeries ApS USB 장치', '이름이 없으면 제조사 + "USB 장치"');
    assert.deepEqual(t.rows['8087:0029'].status, ['미할당 · USB 장치']);
    assert.ok(t.rows['1c7a:0577'].idMono);
    const hostBadgeStyle = await page.evaluate(() => getComputedStyle(document.querySelector('#hd-host .hd-host')).borderTopStyle);
    assert.equal(hostBadgeStyle, 'dashed', '"호스트"는 점선 — 색 없이 VM 배지와 구분');
    await page.evaluate(() => document.querySelector('#hd-host [data-hd-ctl="hidden"]').click());
    t = await readTable(page);
    assert.deepEqual(t.hiddenToggle, { text: '숨기기', pressed: 'true' });
    assert.equal(Object.keys(t.rows).length, 15);
    assert.equal(t.rows['174c:3274'].action, '할당 불가 · 허브');
    assert.deepEqual(t.chips.map(c => c.count), ['15', '5', '10'], '보기를 켜면 숨김 행도 센다');
  }, { routes: rpc({ list: REAL }) });
});

                                                                                             
                                                                                     
                                                                         
test('화면: 동반 function 보조 줄은 할당 가능한 GPU만 "함께 이동", 그 밖은 "같은 IOMMU 그룹"', async () => {
  const BMC = {
    kind: 'pci', group: 'gpu', id: '0000:04:00.0', present: true,
    name: 'ASPEED Technology, Inc. ASPEED Graphics Family', vendor_name: 'ASPEED Technology, Inc.',
    pci_addr: '0000:04:00.0', vendor_id: '1a03', device_id: '2000', driver: 'ast', iommu_group: '23',
    boot_vga: true, display_connected: true, host_display: true, group_safe: true, group_vfio_ready: false,
    partial_assignment: false, hidden_by_default: false, assignable: false, blocked_reason: 'host_display',
    companions: [{
      pci_addr: '0000:03:00.0', class_code: '060400', name: 'ASPEED Technology, Inc. AST1150 PCI-to-PCI Bridge',
      driver: null, assignments: [],
    }],
    assignments: [],
  };
                                                                            
  const UNBOUND = {
    ...clone(RTX), id: '0000:05:00.0', pci_addr: '0000:05:00.0', driver: 'nouveau', group_vfio_ready: false,
    assignable: false, blocked_reason: 'not_vfio_bound', assignments: [],
    companions: [{ ...clone(RTX.companions[0]), pci_addr: '0000:05:00.1', driver: 'snd_hda_intel', assignments: [] }],
  };
  const data = clone(REAL);
  data.devices.splice(2, 0, BMC, UNBOUND);
  const cases = [
    ['ko', '함께 이동', '같은 IOMMU 그룹'],
    ['en', 'moves with it', 'same IOMMU group'],
  ];
  for (const [lang, moves, sameGroup] of cases) {
    await withPage([], async (page, { port }) => {
      await boot(page, port, 'ADMIN', lang);
      const subs = await page.evaluate(() => Object.fromEntries([...document.querySelectorAll('#hd-host tr[data-hd-row]')]
        .map(tr => [tr.getAttribute('data-hd-row'),
          [...tr.querySelectorAll('td')[0].querySelectorAll('.hd-sub')].map(n => n.textContent.replace(/\s+/g, ' ').trim())])));
      assert.deepEqual(subs['0000:01:00.0'], ['+ NVIDIA Corporation GA104 High Definition Audio Controller(01:00.1) ' + moves],
        `${lang}: 할당 가능한 GPU(RTX)의 동반 function은 함께 이동`);
      assert.deepEqual(subs['0000:04:00.0'], ['+ ASPEED Technology, Inc. AST1150 PCI-to-PCI Bridge(03:00.0) ' + sameGroup],
        `${lang}: 호스트 화면 BMC VGA의 브리지는 같은 IOMMU 그룹`);
      assert.deepEqual(subs['0000:05:00.0'], ['+ NVIDIA Corporation GA104 High Definition Audio Controller(05:00.1) ' + sameGroup],
        `${lang}: vfio-pci에 묶이지 않은 GPU의 동반 function도 같은 IOMMU 그룹`);
    }, { routes: rpc({ list: data }) });
  }
});

test('화면: 지금 붙이기와 해제가 함께', async () => {
  await withPage([], async (page, { port }) => {
    await boot(page, port, 'ADMIN');
    const t = await readTable(page);
    assert.deepEqual(t.rows['054c:0ce6'].buttons.map(b => [b.text, b.disabled, b.kind]),
      [['지금 붙이기', false, 'attach-now'], ['해제', false, 'release']]);
    assert.deepEqual(t.rows['1038:22ad'].buttons.map(b => [b.text, b.disabled]), [['해제', false]], '실행 중 VM의 USB 해제도 활성');
    assert.deepEqual(t.rows['1038:22ac'].buttons.map(b => [b.text, b.disabled]), [['해제', false]], '호스트에서 빠진 행도 해제할 수 있다');
  }, { routes: rpc({ list: full() }) });
});

test('화면: 호스트에 없는 PCI 행은 해제 불가 텍스트', async () => {
  await withPage([], async (page, { port }) => {
    await boot(page, port, 'ADMIN');
    const t = await readTable(page);
    const row = t.rows['0000:09:00.0'];
    assert.equal(row.group, 'gpu');
    assert.equal(row.device, 'PCI 장치');
    assert.equal(row.buttons.length, 0);
    assert.equal(row.action, NO_PCI);
    assert.deepEqual(row.status, ['영구만 · 호스트에 안 꽂힘 · PCI 장치 · 호스트에 없음(정의만 있음)']);
    assert.ok(row.warn.includes('PCI 장치 · 호스트에 없음(정의만 있음)'));
    assert.deepEqual(t.groups[0].rows, ['0000:00:02.0', '0000:01:00.0', '0000:09:00.0'], '호스트에 없는 행은 묶음 끝');
  }, { routes: rpc({ list: full() }) });
});

test('화면: 목록 오류는 장치 없음이 아니다', async () => {
  for (const reply of [
    { error: { code: -32000, message: 'cannot list VMs' } },                                                         
    { gateway: { status: 500, code: 'NO_RESPONSE', message: 'cannot list VMs' } },                       
  ]) {
    const state = { list: REAL, listReply: () => reply };
    await withPage([], async (page, { port, requests }) => {
      await boot(page, port, 'ADMIN');
      const r = await page.evaluate(() => {
        const host = document.getElementById('hd-host');
        const err = host.querySelector('.load-error-state');
        return {
          err: err && err.textContent, role: err && err.getAttribute('role'), table: host.querySelectorAll('table').length,
          retry: !!(err && err.querySelector('button')), all: host.textContent,
        };
      });
      assert.match(r.err, /cannot list VMs/);
      assert.equal(r.role, 'alert');
      assert.equal(r.table, 0);
      assert.ok(r.retry, '다시 시도 버튼');
      assert.ok(!r.all.includes('표시할 호스트 장치가 없습니다'), '오류를 빈 목록으로 보이지 않는다(A7)');
      state.listReply = null;
      await page.evaluate(() => document.querySelector('#hd-host .load-error-state button').click());
      await page.waitForFunction(() => document.querySelectorAll('#hd-host tr[data-hd-row]').length === 10, { timeout: 5000 });
      assert.equal(calls(requests, 'host.device.list').length, 2, '다시 시도는 목록 재요청 1건');
    }, { routes: rpc(state) });
  }
});

test('화면: truncated 안내', async () => {
  const data = { ...clone(REAL), truncated: true };
  await withPage([], async (page, { port }) => {
    await boot(page, port, 'ADMIN');
    const t = await readTable(page);
    const n = data.devices.length;
    assert.ok(t.notes.includes(`장치가 많아 일부만 보입니다(상한 ${n}개). pcvctl --format=json host devices --all로 전체를 보세요`),
      JSON.stringify(t.notes));
  }, { routes: rpc({ list: data }) });
  const plain = clone(REAL);
  await withPage([], async (page, { port }) => {
    await boot(page, port, 'ADMIN');
    assert.deepEqual((await readTable(page)).notes, [], '잘리지 않으면 안내 없음');
  }, { routes: rpc({ list: plain }) });
});

test('화면: VM 이름은 텍스트 노드', async () => {
  const evil = '<img src=x onerror="window.__hdXss=1">';
  const data = clone(REAL);
  data.devices.forEach(d => (d.assignments || []).forEach(a => { a.vm = evil; }));
  data.vms = [{ name: evil, active: false, persistent: true }];
  await withPage([], async (page, { port }) => {
    await boot(page, port, 'ADMIN');
    const t = await readTable(page);
    assert.deepEqual(t.rows['1038:22ad'].vmBadges, [evil]);
    await press(page, '1038:22ad', 'release');
    const d = await readDialog(page);
    assert.ok(d.text.includes(evil + '에서 SteelSeries Arctis Nova 7X(1038:22ad)를 뗍니다.'), d.text);
    await clickDialog(page, '취소');
    await press(page, '1c7a:0577', 'assign');
    const a = await readDialog(page);
    assert.equal(a.options[1].value, evil);
    const r = await page.evaluate(() => ({
      imgs: document.querySelectorAll('img[src="x"]').length, xss: window.__hdXss === 1,
    }));
    assert.deepEqual(r, { imgs: 0, xss: false });
  }, { routes: rpc({ list: data }) });
});

                          

test('할당 창: VM 미선택 잠금·꺼진 VM 체크 강제·GPU 체크 잠금·D2 확인 체크', async () => {
  const data = full();
  const telink = dev(data, '248a:8327');
  telink.assignments = [];                                                                          
  const gpuFree = clone(data);
  dev(gpuFree, '0000:01:00.0').assignments = [];
  dev(gpuFree, '0000:01:00.0').companions[0].assignments = [];
  const state = { list: data };
  await withPage([], async (page, { port }) => {
    await boot(page, port, 'ADMIN');
    await press(page, '046d:c52b', 'assign');
    let d = await readDialog(page);
    assert.equal(d.title, 'Logitech USB Receiver 046d:c52b를 어느 VM에?');
    assert.deepEqual(d.options[0], { value: '', text: 'VM 선택', disabled: false });
    assert.deepEqual(d.options.slice(1).map(o => o.text),
      ['dev-box (꺼짐)', 'gpu-lab (꺼짐)', 'scratch-vm (실행 중)', 'win-guest1 (실행 중)']);
    assert.ok(d.buttons.find(b => b.text === '할당').disabled, 'VM 고르기 전 잠금');
    assert.equal(d.focused, '취소', '초기 포커스는 취소');
    assert.ok(d.text.includes('이 장치는 지금 호스트의 키보드·마우스로 쓰이고 있습니다. VM에 넘기면 호스트에서 이 장치로 입력할 수 없습니다.'));
    assert.deepEqual(d.boxes.map(b => [b.checked, b.disabled]), [[true, false], [false, false]], '영구 기본 켜짐 + 확인 체크');
    await choose(page, 'dev-box');
    d = await readDialog(page);
    assert.deepEqual([d.boxes[0].checked, d.boxes[0].disabled], [true, true]);
    assert.ok(d.text.includes('꺼진 VM은 영구 정의에만 붙일 수 있습니다'));
    assert.ok(d.buttons.find(b => b.text === '할당').disabled, '확인 체크 전 잠금(D2)');
    await toggleBox(page, 1);
    assert.equal((await readDialog(page)).buttons.find(b => b.text === '할당').disabled, false);
    await choose(page, 'scratch-vm');
    d = await readDialog(page);
    assert.deepEqual([d.boxes[0].checked, d.boxes[0].disabled], [false, true]);
    assert.ok(d.text.includes('이 VM은 영구 정의가 없어 지금만 붙습니다'));
    await choose(page, WIN);
    d = await readDialog(page);
    assert.deepEqual([d.boxes[0].checked, d.boxes[0].disabled], [true, false]);
    assert.ok(d.text.includes('실행 중인 VM이면 지금 바로 붙고, 정지된 VM이면 다음 시작 때 붙습니다.'));
    await toggleBox(page, 0);
    assert.ok((await readDialog(page)).text.includes('지금만 붙고 VM을 끄면 빠집니다'));
    await clickDialog(page, '취소');
    await waitNoDialog(page);

    await press(page, '248a:8327', 'assign');
    d = await readDialog(page);
    assert.equal(d.boxes.length, 1, 'usbfs(VM이 잡은) 장치에는 확인 체크가 없다');
    assert.ok(!d.text.includes('키보드·마우스로 쓰이고'));
    await clickDialog(page, '취소');
    await waitNoDialog(page);

    await press(page, '8087:0029', 'assign');
    d = await readDialog(page);
    assert.ok(d.text.includes('호스트 블루투스가 끊깁니다.'));
    assert.equal(d.boxes.length, 1, '블루투스는 경고만, 확인 체크 없음');
    await choose(page, WIN);
    assert.equal((await readDialog(page)).buttons.find(b => b.text === '할당').disabled, false);
    await clickDialog(page, '취소');
    await waitNoDialog(page);

    state.list = gpuFree;
    await page.evaluate(() => window.renderHostDevices(document.getElementById('hd-host')));
    await press(page, '0000:01:00.0', 'assign');
    d = await readDialog(page);
    assert.deepEqual([d.boxes[0].checked, d.boxes[0].disabled], [true, true], 'GPU는 영구 체크 잠금');
    assert.ok(d.text.includes('GPU는 정지 VM의 영구 정의에만 할당되고 다음 시작 때 붙습니다'));
    assert.deepEqual(d.options.filter(o => o.disabled).map(o => o.text),
      ['scratch-vm (실행 중) (끈 뒤 가능)', 'win-guest1 (실행 중) (끈 뒤 가능)']);
  }, { routes: rpc(state) });
});

test('할당 창: 보내는 본문', async () => {
  const data = full();
  const gpuFree = clone(data);
  dev(gpuFree, '0000:01:00.0').assignments = [];
  dev(gpuFree, '0000:01:00.0').companions[0].assignments = [];
  const stopped = clone(data);
  dev(stopped, '0000:01:00.0').assignments[0].vm_active = false;
  dev(stopped, '0000:01:00.0').assignments[0].live = false;
  const state = { list: data };
  await withPage([], async (page, { port, requests }) => {
    await boot(page, port, 'ADMIN');
    for (const persist of [false, true]) {
      await press(page, '1c7a:0577', 'assign');
      await choose(page, WIN);
      if (!persist) await toggleBox(page, 0);
      await clickDialog(page, '할당');
      await waitNoDialog(page);
    }
    assert.deepEqual(calls(requests, 'vm.usb.attach').map(c => c.json.params), [
      { vm_id: WIN, vendor_id: '0x1c7a', product_id: '0x0577', persistent: false },
      { vm_id: WIN, vendor_id: '0x1c7a', product_id: '0x0577', persistent: true },
    ], 'persistent는 늘 명시(spec 6.2)');
    state.list = gpuFree;
    await page.evaluate(() => window.renderHostDevices(document.getElementById('hd-host')));
    await press(page, '0000:01:00.0', 'assign');
    await choose(page, 'gpu-lab');
    await clickDialog(page, '할당');
    await waitNoDialog(page);
    assert.deepEqual(calls(requests, 'device.gpu.attach').map(c => c.json.params), [{ vm_name: 'gpu-lab', pci_addr: '0000:01:00.0' }]);
    state.list = stopped;
    await page.evaluate(() => window.renderHostDevices(document.getElementById('hd-host')));
    await press(page, '0000:01:00.0', 'release');
    const d = await readDialog(page);
    assert.ok(d.text.includes(`정지된 ${WIN}의 영구 정의에서 GPU 그룹(01:00.0, 01:00.1)을 뺍니다. 호스트 드라이버 바인딩은 바꾸지 않습니다.`), d.text);
    await clickDialog(page, '해제');
    await waitNoDialog(page);
    assert.deepEqual(calls(requests, 'device.gpu.detach').map(c => c.json.params), [{ vm_name: WIN, pci_addr: '0000:01:00.0' }]);
  }, { routes: rpc(state) });
});

test('지금 붙이기: startup_policy 본문', async () => {
  const state = { list: full(), reply: () => ({ result: { attached: true, live: true, config: false, startup_policy: 'mandatory' } }) };
  await withPage([], async (page, { port, requests }) => {
    await boot(page, port, 'ADMIN');
    await markTable(page);
    await press(page, '054c:0ce6', 'attach-now');
    await waitRepaint(page);
    await settle(page);
    assert.deepEqual(calls(requests, 'vm.usb.attach').map(c => c.json.params),
      [{ vm_id: WIN, vendor_id: '0x054c', product_id: '0x0ce6', startup_policy: 'mandatory' }], '할당의 영구 정책을 보낸다(spec 6.4)');
    assert.equal(calls(requests, 'host.device.list').length, 2, '결과 뒤 목록 재요청 1건');
    assert.equal(await page.evaluate(() => window.PCV.modalCore.currentDialog()), null, '확인 창 없이');
    assert.equal((await readTable(page)).result, 'win-guest1에 지금 붙였습니다(실행 중)', '응답 data의 live·config를 읽는다');
  }, { routes: rpc(state) });
});

                                 

test('동작 창: 요청 중 확정 버튼 disabled·두 번 눌러도 1건', async () => {
  const state = { list: full(), delays: { 'vm.usb.attach': 400, 'vm.usb.detach': 400 } };
  await withPage([], async (page, { port, requests }) => {
    await boot(page, port, 'ADMIN');
    await markTable(page);
    await press(page, '1c7a:0577', 'assign');
    await choose(page, WIN);
    const during = await page.evaluate(() => {
      const ok = [...window.PCV.modalCore.currentDialog().querySelectorAll('button')].find(b => b.textContent.trim() === '할당');
      const before = ok.getBoundingClientRect().width;
      ok.click(); ok.click();
      return { disabled: ok.disabled, sameWidth: Math.abs(ok.getBoundingClientRect().width - before) < 0.5, text: ok.textContent.trim() };
    });
    assert.deepEqual(during, { disabled: true, sameWidth: true, text: '할당' });
    await waitNoDialog(page);
    await waitRepaint(page);
    assert.equal(calls(requests, 'vm.usb.attach').length, 1, '할당 1건');
    await markTable(page);
    await press(page, '1038:22ad', 'release');
    await page.evaluate(() => {
      const ok = [...window.PCV.modalCore.currentDialog().querySelectorAll('button')].find(b => b.textContent.trim() === '해제');
      ok.click(); ok.click();
    });
    await waitNoDialog(page);
    await waitRepaint(page);
    assert.equal(calls(requests, 'vm.usb.detach').length, 1, '해제 1건');
    await markTable(page);
    await page.evaluate(() => {
      const tr = [...document.querySelectorAll('#hd-host tr[data-hd-row]')].find(r => r.getAttribute('data-hd-row') === '054c:0ce6');
      const b = tr.querySelector('button[data-hd-kind="attach-now"]');
      b.click(); b.click();
      window.__hdNowDisabled = b.disabled;
    });
    assert.equal(await page.evaluate(() => window.__hdNowDisabled), true);
    await waitRepaint(page);
    await settle(page);
    assert.equal(calls(requests, 'vm.usb.attach').length, 2, '지금 붙이기 1건');
  }, { routes: rpc(state) });
});

test('동작 창: 성공 → 닫힘 → 목록 재요청 1건', async () => {
  const state = { list: full(), reply: () => ({ result: { attached: true, live: true, config: true, startup_policy: 'optional' } }) };
  await withPage([], async (page, { port, requests }) => {
    await boot(page, port, 'ADMIN');
    await markTable(page);
    await press(page, '1c7a:0577', 'assign');
    await choose(page, WIN);
    await clickDialog(page, '할당');
    await waitNoDialog(page);
    await waitRepaint(page);
    await settle(page);
    assert.equal(calls(requests, 'host.device.list').length, 2);
    assert.equal((await readTable(page)).result, 'win-guest1에 붙였습니다(실행 중 · 영구)');
  }, { routes: rpc(state) });
});

test('동작 창: 실패 → 유지·role=alert·값 유지·닫을 때 재요청', async () => {
  const message = "USB device 0x1c7a:0x0577 cannot be attached to VM 'win-guest1' without saving it: the VM is not running";
  const state = { list: full(), reply: () => ({ error: { code: -32002, message } }) };
  await withPage([], async (page, { port, requests }) => {
    await boot(page, port, 'ADMIN');
    await press(page, '1c7a:0577', 'assign');
    await choose(page, WIN);
    await toggleBox(page, 0);
    await clickDialog(page, '할당');
    await page.waitForFunction(() => {
      const dlg = window.PCV.modalCore.currentDialog();
      return dlg && dlg.querySelector('[role="alert"]').textContent.length > 0;
    }, { timeout: 5000 });
    const d = await readDialog(page);
    assert.equal(d.open, true, '실패면 창을 닫지 않는다');
    assert.ok(d.alert.includes("지금 상태와 맞지 않습니다(정책이 다름·자리표시자·같은 장치 여럿·꺼진 VM에 '지금만') — 먼저 해제하거나 '영구로 저장'을 켜세요"), d.alert);
    assert.ok(d.alert.includes(message), '서버 문구 원문');
    assert.equal(d.selected, WIN, '고른 VM 유지');
    assert.equal(d.boxes[0].checked, false, '체크 값 유지');
    assert.equal(d.buttons.find(b => b.text === '할당').disabled, false, '확정 버튼을 다시 연다');
    await settle(page);
    assert.equal(calls(requests, 'host.device.list').length, 1, '실패 직후에는 재요청하지 않는다');
    await markTable(page);
    await clickDialog(page, '취소');
    await waitNoDialog(page);
    await waitRepaint(page);
    await settle(page);
    assert.equal(calls(requests, 'host.device.list').length, 2, '창을 닫을 때 1건');
  }, { routes: rpc(state) });
});

test('동작 창: 포커스 복귀', async () => {
  const before = full();
  const afterAssign = clone(before);
  dev(afterAssign, '1c7a:0577').assignments = [{ vm: WIN, vm_visible: true, vm_active: true, live: true, persistent: true,
    missing: false, startup_policy: 'optional', stale_live_address: false, ambiguous: false }];
  const afterRelease = clone(afterAssign);
  afterRelease.devices = afterRelease.devices.filter(d => d.id !== '1038:22ac');
  const state = { list: before };
  state.reply = method => {
    if (method === 'vm.usb.attach') state.list = afterAssign;
    if (method === 'vm.usb.detach') state.list = afterRelease;
    return { result: { attached: true, live: true, config: true } };
  };
  const focused = page => page.evaluate(() => {
    const a = document.activeElement;
    return { key: a.getAttribute('data-hd-key'), kind: a.getAttribute('data-hd-kind'), title: a.classList.contains('pagehead-title') };
  });
  await withPage([], async (page, { port }) => {
    await boot(page, port, 'ADMIN');
    await press(page, '1c7a:0577', 'assign');
    await page.keyboard.press('Escape');
    await waitNoDialog(page);
    assert.deepEqual(await focused(page), { key: '1c7a:0577|', kind: 'assign', title: false }, 'ESC 뒤 누른 할당… 버튼');
    await markTable(page);
    await press(page, '1c7a:0577', 'assign');
    await choose(page, WIN);
    await clickDialog(page, '할당');
    await waitNoDialog(page);
    await waitRepaint(page);
    assert.deepEqual(await focused(page), { key: '1c7a:0577|win-guest1', kind: 'release', title: false }, '바뀐 버튼 → 같은 행의 새 동작');
    await markTable(page);
    await press(page, '1038:22ac', 'release');
    await clickDialog(page, '해제');
    await waitNoDialog(page);
    await waitRepaint(page);
    assert.deepEqual(await focused(page), { key: null, kind: null, title: true }, '행이 없어지면 머리줄');
  }, { routes: rpc(state) });
});

test('해제 확인: 취소·ESC·백드롭 요청 0건·초기 포커스 취소', async () => {
  await withPage([], async (page, { port, requests }) => {
    await page.setViewport({ width: 1280, height: 900 });
    await boot(page, port, 'ADMIN');
    for (const how of ['cancel', 'escape', 'backdrop']) {
      await press(page, '1038:22ad', 'release');
      const d = await readDialog(page);
      assert.equal(d.focused, '취소', `${how}: 초기 포커스는 취소`);
      assert.ok(d.text.includes('win-guest1에서 SteelSeries Arctis Nova 7X(1038:22ad)를 뗍니다. 실행 중이면 게스트에서 바로 사라지고, 영구 정의에서도 빠집니다.'), d.text);
      if (how === 'cancel') await clickDialog(page, '취소');
      if (how === 'escape') await page.keyboard.press('Escape');
      if (how === 'backdrop') await page.mouse.click(4, 4);
      await waitNoDialog(page);
      assert.equal(calls(requests, 'vm.usb.detach').length, 0, `${how}: 요청 0건`);
    }
    await settle(page);
    assert.equal(calls(requests, 'host.device.list').length, 1, '닫기만 하면 재조회도 없다');
    await press(page, '1038:22ad', 'release');
    await clickDialog(page, '해제');
    await waitNoDialog(page);
    assert.deepEqual(calls(requests, 'vm.usb.detach').map(c => c.json.params), [{ vm_id: WIN, vendor_id: '0x1038', product_id: '0x22ad' }]);
  }, { routes: rpc({ list: full() }) });
});

                                     

test('필터: URL 상태 유지, 보기는 유지 안 함', async () => {
  await withPage([], async (page, { port }) => {
    await boot(page, port, 'ADMIN');
    await page.evaluate(() => document.querySelector('#hd-host [data-hd-ctl="hidden"]').click());
    assert.equal(Object.keys((await readTable(page)).rows).length, 15);
    await page.evaluate(() => [...document.querySelectorAll('#hd-host .chip')].find(c => c.getAttribute('data-val') === 'assigned').click());
    let t = await readTable(page);
    assert.deepEqual(Object.keys(t.rows).sort(), ['0000:01:00.0', '1038:22ac', '1038:22ad', '248a:8327', '3554:fa09']);
    assert.match(await page.evaluate(() => location.search), /hdassign=assigned/, '필터는 URL에 남는다');
    await page.evaluate(() => window.renderHostDevices(document.getElementById('hd-host')));
    t = await readTable(page);
    assert.equal(t.chips.find(c => c.val === 'assigned').pressed, 'true', '다시 열어도 같은 필터');
    assert.deepEqual(t.hiddenToggle, { text: '보기', pressed: 'false' }, '보기는 다시 열면 꺼진다');
    await page.evaluate(() => [...document.querySelectorAll('#hd-host .chip')].find(c => c.getAttribute('data-val') === '__all__').click());
    t = await readTable(page);
    assert.equal(Object.keys(t.rows).length, 10, '숨김 행은 다시 숨는다');
    assert.doesNotMatch(await page.evaluate(() => location.search), /hdassign=/);
  }, { routes: rpc({ list: REAL }) });
});

test('새로고침: 진행 중 폭 유지·disabled·끝나도 포커스 유지', async () => {
  const state = { list: REAL, delays: {} };
  await withPage([], async (page, { port, requests }) => {
    await boot(page, port, 'ADMIN');
    await markTable(page);
    state.delays['host.device.list'] = 400;
    const during = await page.evaluate(() => {
      const b = document.querySelector('#hd-host [data-hd-ctl="refresh"]');
      b.focus();
      const before = b.getBoundingClientRect().width;
      b.click(); b.click();
      return { disabled: b.disabled, sameWidth: Math.abs(b.getBoundingClientRect().width - before) < 0.5, text: b.textContent.trim() };
    });
    assert.deepEqual(during, { disabled: true, sameWidth: true, text: '새로고침' });
    await waitRepaint(page);
    await settle(page);
    assert.equal(calls(requests, 'host.device.list').length, 2, '두 번 눌러도 1건');
    assert.equal(await page.evaluate(() => document.activeElement.getAttribute('data-hd-ctl')), 'refresh');
  }, { routes: rpc(state) });
});

                               

test('빈 상태: ADMIN 문구와 비ADMIN resource.grant 안내 문구', async () => {
  const empty = { devices: [], vms: [], sources: { usb: 'available', pci: 'available', names: 'available' }, truncated: false };
  for (const [role, text] of [
    ['ADMIN', '표시할 호스트 장치가 없습니다'],
    ['OPERATOR', '이 계정 테넌트에 허용된 호스트 장치가 없습니다(관리자가 resource.grant로 허용)'],
    ['VIEWER', '이 계정 테넌트에 허용된 호스트 장치가 없습니다(관리자가 resource.grant로 허용)'],
  ]) {
    await withPage([], async (page, { port }) => {
      await boot(page, port, role);
      const t = await readTable(page);
      assert.ok(t.all.includes(text), `${role}: ${t.all}`);
      assert.equal(await page.evaluate(() => document.querySelectorAll('#hd-host table').length), 0);
      assert.ok(await page.evaluate(() => !!document.querySelector('#hd-host [data-hd-ctl="refresh"]')), '빈 상태에도 새로고침');
    }, { routes: rpc({ list: empty }) });
  }
});

test('접근성: 동작 버튼 이름에 장치 이름, 비활성 이유는 aria-describedby', async () => {
  await withPage([], async (page, { port }) => {
    await boot(page, port, 'ADMIN');
    let t = await readTable(page);
    assert.equal(t.rows['8087:0029'].buttons[0].label, 'Intel Corp. AX200 Bluetooth 할당…');
    assert.equal(t.rows['1038:22ad'].buttons[0].label, 'SteelSeries Arctis Nova 7X 해제');
    const rtx = t.rows['0000:01:00.0'].buttons[0];
    assert.equal(rtx.disabled, true);
    assert.equal(rtx.reason, STOP_FIRST, '비활성 이유는 글 + aria-describedby');
    assert.ok(t.rows['0000:01:00.0'].action.includes(STOP_FIRST), '이유가 화면에 글로 보인다');
    assert.equal(t.rows['8087:0029'].buttons[0].describedBy, null, '활성 버튼에는 이유 연결이 없다');
  }, { routes: rpc({ list: REAL }) });
  await withPage([], async (page, { port }) => {
    await boot(page, port, 'VIEWER');
    const t = await readTable(page);
    for (const id of ['1038:22ac', '8087:0029', '0000:01:00.0']) {
      const row = t.rows[id];
      assert.ok(row.buttons.length > 0 ? row.buttons.every(b => b.disabled && b.reason === VIEW_ONLY) : row.action === VIEW_ONLY,
        `${id}: ${JSON.stringify(row)}`);
    }
  }, { routes: rpc({ list: maskFor(REAL, []) }) });
  await withPage([], async (page, { port }) => {
    await boot(page, port, 'OPERATOR');
    const t = await readTable(page);
    assert.equal(t.rows['1038:22ad'].action, OTHER_VM, '남의 VM 할당만 있는 행은 동작 없음');
    assert.deepEqual(t.rows['1038:22ad'].vmBadges, ['다른 VM']);
    assert.deepEqual(t.rows['0000:01:00.0'].buttons.length, 0);
    assert.equal(t.rows['0000:01:00.0'].action, OTHER_VM);
  }, { routes: rpc({ list: maskFor(REAL, []) }) });
});

                                    

test('메뉴: 인프라 묶음 gpu 앞·라우트·⌘K 팔레트·제목', async () => {
  await withPage([], async (page, { port }) => {
    await boot(page, port, 'ADMIN');
    const r = await page.evaluate(() => {
      const infra = window.PCV.shell.NAV_SECTIONS().find(s => s.items.some(i => i.id === 'gpu')).items;
      const at = infra.findIndex(i => i.id === 'host-devices');
      return {
        item: infra[at] && { ko: infra[at].ko, en: infra[at].en },
        beforeGpu: infra[at + 1] && infra[at + 1].id,
      };
    });
    assert.deepEqual(r, { item: { ko: '호스트 장치', en: 'Host Devices' }, beforeGpu: 'gpu' });
    const nav = readFileSync(new URL('../../ui/modules/nav.js', import.meta.url), 'utf8');
    const app = readFileSync(new URL('../../ui/app.js', import.meta.url), 'utf8');
    assert.match(nav, /^ {6}'host-devices': \(\) => renderHostDevices\(b\),$/m, '라우트 표');
    assert.ok(nav.includes("{ id: 'host-devices', label: _L('호스트 장치', 'Host Devices')"), '⌘K 팔레트');
    assert.ok(nav.indexOf("{ id: 'host-devices', label:") < nav.indexOf("{ id: 'gpu', label:"), '팔레트도 gpu 앞');
    assert.ok(app.includes("'host-devices': _L('호스트 장치', 'Host Devices'),"), '제목 표');
  }, { routes: rpc({ list: REAL }) });
});

                                                       
                                                                                   
                                                                        
const CB_WIDTH = { 1440: 1171, 1280: 1010, 1024: 755, 900: 631, 480: 456 };

async function placeInCb(page, viewport) {
  await page.setViewport({ width: viewport, height: 900 });
  await page.evaluate(width => {
    let cb = document.getElementById('hd-cb');
    if (!cb) {
      cb = document.createElement('div');
      cb.id = 'hd-cb';
      cb.className = 'cb';
      document.body.appendChild(cb);
      cb.appendChild(document.getElementById('hd-host'));
    }
    cb.style.cssText = `width:${width}px;padding:0 !important;overflow:visible;flex:none;animation:none`;
  }, CB_WIDTH[viewport]);
  await page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
}

                                                         
function readLayout(page) {
  return page.evaluate(() => {
    const host = document.getElementById('hd-host');
    const table = host.querySelector('table.hd-table');
    const lines = node => {
      const tops = [];
      const walker = document.createTreeWalker(node, NodeFilter.SHOW_TEXT);
      for (let t = walker.nextNode(); t; t = walker.nextNode()) {
        if (!t.textContent.trim()) continue;
        const range = document.createRange();
        range.selectNodeContents(t);
        for (const box of range.getClientRects()) {
          if (box.width > 0 && !tops.some(top => Math.abs(top - box.top) < 3)) tops.push(box.top);
        }
      }
      return tops.length;
    };
    const badges = [...host.querySelectorAll('.hd-vm, .hd-host')].filter(b => b.getClientRects().length);
    const cells = [...host.querySelectorAll('tr[data-hd-row] td')].filter(td => td.getClientRects().length);
    return {
      card: getComputedStyle(table).display === 'block',
      badges: badges.length,
      multiLine: badges.filter(b => lines(b) > 1).map(b => b.textContent),
      overflow: host.scrollWidth - host.clientWidth,
      cellOverflow: cells.filter(td => td.scrollWidth > td.clientWidth + 1)
        .map(td => td.closest('tr').getAttribute('data-hd-row') + ':' + td.getAttribute('data-label')),
    };
  });
}

test('배치: 900·1024·1280·1440 표에서 VM·호스트 배지는 한 줄이고 표가 가로로 넘치지 않는다(F1)', async () => {
  await withPage([], async (page, { port }) => {
    await boot(page, port, 'ADMIN');
    for (const viewport of [1440, 1280, 1024, 900]) {
      await placeInCb(page, viewport);
      const l = await readLayout(page);
      assert.equal(l.card, false, `${viewport}: 표 모양`);
      assert.ok(l.badges >= 14, `${viewport}: 배지 ${l.badges}`);
      assert.deepEqual(l.multiLine, [], `${viewport}: 단어 중간 줄바꿈 없음("window" / "s-11" 회귀)`);
      assert.ok(l.overflow <= 0, `${viewport}: 가로 넘침 ${l.overflow}px`);
    }
  }, { routes: rpc({ list: full() }) });
});

test('배치: 64자 VM 이름도 표를 가로로 밀지 않고 다른 배지는 한 줄(F1)', async () => {
  const LONG = ('build-runner-' + 'x'.repeat(64)).slice(0, 64);                           
  const data = full();
  data.devices.forEach(d => (d.assignments || []).forEach(a => { if (a.vm === 'scratch-vm') a.vm = LONG; }));
  data.vms.forEach(v => { if (v.name === 'scratch-vm') v.name = LONG; });
  await withPage([], async (page, { port }) => {
    await boot(page, port, 'ADMIN');
    for (const viewport of [1024, 900]) {
      await placeInCb(page, viewport);
      const l = await readLayout(page);
      assert.ok(l.overflow <= 0, `${viewport}: 가로 넘침 ${l.overflow}px`);
      assert.deepEqual(l.multiLine, [LONG], `${viewport}: 긴 이름만 배지 폭 안에서 줄을 바꾼다`);
    }
  }, { routes: rpc({ list: data }) });
});

test('배치: 480px 카드형에서 상태·보조 글이 칸 밖으로 넘치지 않는다(F3)', async () => {
  await withPage([], async (page, { port }) => {
    await boot(page, port, 'ADMIN');
    await placeInCb(page, 480);
    const l = await readLayout(page);
    assert.equal(l.card, true, '카드형');
    assert.deepEqual(l.cellOverflow, [], '칸 밖으로 나간 글 없음(전역 ≤480px table nowrap 회귀)');
    assert.ok(l.overflow <= 0, `가로 넘침 ${l.overflow}px`);
    assert.deepEqual(l.multiLine, [], '배지는 한 줄');
  }, { routes: rpc({ list: full() }) });
});

test('명암비: 가려진 VM 배지와 그 안 "· 꺼짐"은 테마 4종에서 4.5:1 이상(F2)', async () => {
  await withPage([], async (page, { port }) => {
    await boot(page, port, 'OPERATOR');
    const out = [];
    for (const theme of ['supanova', 'supanova-cyan', 'supanova-hicontrast', 'supanova-mockup']) {
      out.push(...await page.evaluate(theme => {
        document.documentElement.dataset.theme = theme;
        const rgb = c => c.match(/[\d.]+/g).map(Number);
        const over = (f, b) => [0, 1, 2].map(i => f[i] * (f[3] ?? 1) + b[i] * (1 - (f[3] ?? 1)));
        const lum = c => c.map(v => { v /= 255; return v <= 0.04045 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4; })
          .reduce((s, v, i) => s + v * [0.2126, 0.7152, 0.0722][i], 0);
        const background = node => {
          const chain = [];
          for (let n = node; n; n = n.parentElement) chain.unshift(n);
          return chain.reduce((bg, n) => over(rgb(getComputedStyle(n).backgroundColor), bg), [255, 255, 255]);
        };
        return [...document.querySelectorAll('#hd-host .hd-vm-other, #hd-host .hd-vm-other .hd-vm-off')].map(node => {
          const bg = background(node);
          const fg = over(rgb(getComputedStyle(node).color), bg);
          const [a, b] = [lum(fg), lum(bg)].sort((x, y) => y - x);
          return { theme, text: node.textContent, ratio: Math.round((a + 0.05) / (b + 0.05) * 100) / 100 };
        });
      }, theme));
    }
    assert.ok(out.length >= 8, `가려진 배지가 그려졌다: ${out.length}`);
    const low = out.filter(r => r.ratio < 4.5);
    assert.deepEqual(low, [], '작은 글자 4.5:1 미만 없음');
  }, { routes: rpc({ list: maskFor(full(), [WIN]) }) });
});
