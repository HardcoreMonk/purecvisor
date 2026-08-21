                                                                                     
                                            
                                                                  
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const MODS_HN = ['ui/modules/endpoints.js', 'ui/modules/api.js', 'ui/modules/ui.js',
  'ui/modules/filter-state.js', 'ui/modules/uxlib.js'];

                                                                    
                                                            
const MODS_VM = ['ui/modules/endpoints.js', 'ui/modules/api.js', 'ui/modules/ui.js',
  'ui/modules/filter-state.js', 'ui/modules/uxlib.js',
  'ui/modules/modal-core.js', 'ui/modules/modal.js',
  'ui/modules/charts.js',
  'ui/modules/shell.js',
  'ui/modules/metrics.js',
  'ui/modules/vm.js', 'ui/modules/vm-console.js', 'ui/modules/vm-lifecycle.js', 'ui/modules/vm-guest.js',
  'ui/modules/help.js',
  'ui/modules/nav.js'];

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

test('HN.spark: polyline 좌표·플랫라인·tone', async () => {
  await withPage(MODS_HN, async (page) => {
    const r = await page.evaluate(() => {
      const now = Date.now();
      const s = HN.spark([{ t: now - 60e3, v: 0 }, { t: now, v: 100 }], { tone: 'warn' });
      const flat = HN.spark([], {});
      return {
        tag: s.tagName.toLowerCase(),
        line: s.querySelector('polyline') && s.querySelector('polyline').getAttribute('points'),
        stroke: s.querySelector('polyline').getAttribute('stroke'),
        flatDash: flat.querySelector('line') && flat.querySelector('line').getAttribute('stroke-dasharray')
      };
    });
    assert.equal(r.tag, 'svg');
    assert.ok(r.line.startsWith('0,'));                       
    assert.ok(r.stroke.includes('--st-warn'));
    assert.ok(r.flatDash);                                       
  });
});

test('HN.gaugeTile: 톤·peak·null → idle —', async () => {
  await withPage(MODS_HN, async (page) => {
    const r = await page.evaluate(() => {
      const t = HN.gaugeTile({ value: 86, unit: '%', peak: 'peak 91%', label: 'pcvpool', scale: 'warn 80 · crit 90', warn: 80, crit: 90 });
      const n = HN.gaugeTile({ value: null, label: 'net', warn: 80, crit: 90 });
      return {
        cls: t.className, metric: t.querySelector('.gauge-metric').textContent,
        peak: t.querySelector('.gauge-peak').textContent,
        ticks: t.querySelectorAll('.tick').length,
        nullCls: n.className, nullMetric: n.querySelector('.gauge-metric').textContent
      };
    });
    assert.ok(r.cls.includes('g-warn'));
    assert.equal(r.metric, '86%');
    assert.equal(r.peak, 'peak 91%');
    assert.equal(r.ticks, 2);
    assert.ok(r.nullCls.includes('g-idle'));
    assert.equal(r.nullMetric, '—');
  });
});

                                                                
                                                    
                                          
test('HN.gaugeTile: display 옵션 → 대수치 교체(fill 은 value 기준) + ticks:false', async () => {
  await withPage(MODS_HN, async (page) => {
    const r = await page.evaluate(() => {
      const t = HN.gaugeTile({ value: 42, display: '1.2 MB/s', peak: 'peak 3.4 MB/s',
        label: 'net', warn: 101, crit: 102, ticks: false });
      return {
        metric: t.querySelector('.gauge-metric').textContent,
        fillWidth: t.querySelector('.fill').style.width,
        ticks: t.querySelectorAll('.tick').length,
        cls: t.className,
      };
    });
    assert.equal(r.metric, '1.2 MB/s');
    assert.equal(r.fillWidth, '42%');
    assert.equal(r.ticks, 0);
    assert.ok(r.cls.includes('g-ok'));                                         
  });
});

test('vm 리스트 스파크: PCV.metrics 시계열 → 캔버스 드로잉(콘솔 에러 0)', async () => {
  await withPage(MODS_VM, async (page) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await bootVm(page);
    const r = await page.evaluate(async () => {
      PCV.metrics.push('vm.a.cpu', 10);
      PCV.metrics.push('vm.a.cpu', 40);
      PCV.metrics.push('vm.a.cpu', 70);
      const b = document.getElementById('cb');
      window.renderVmScreen(b, vmList[0], 'summary');
      await new Promise(resolve => setTimeout(resolve, 80));                                  
      const canvas = document.getElementById('spark-a');
      return {
        hasCanvas: !!canvas,
        hist: PCV.metrics.window('vm.a.cpu', '15m').map(s => s.v)
      };
    });
    assert.ok(r.hasCanvas);
    assert.deepEqual(r.hist, [10, 40, 70]);
    assert.equal(pageErrors.length, 0, pageErrors.join('; '));
  });
});

                                                                     
                                                                   
                                                                    
                                                               
                                                   
const MODS_DASH = [
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
  'ui/modules/cloud.js',
  'ui/modules/help.js',
  'ui/modules/nav.js',
  'ui/modules/theme.js',
  'ui/modules/accounts.js',
  'ui/modules/advanced.js',
  'ui/modules/selfhealing.js',
  'ui/modules/mobile.js',
  'ui/app.js',
];

const DASH_VMS = [
  { name: 'vm-run', state: 'running', vcpu: 4, live_cpu_pct: 31, memory_mb: 4096, memory_used_mb: 2048 },
  { name: 'vm-stop', state: 'shutoff', vcpu: 2, memory_mb: 2048 },
  { name: 'vm-bad', state: 'crashed', vcpu: 2, memory_mb: 2048 },
];

                                                                
                                                                 
                                                            
                                                                     
                                             
const DASH_ROUTES = {
  '/api/v1/vms': { body: DASH_VMS },
  '/api/v1/metrics': { status: 404, body: null },
};

async function bootDash(page, port) {
  await page.evaluate(async () => {
    const html = await fetch('/ui/index.html').then(response => response.text());
    const parsed = new DOMParser().parseFromString(html, 'text/html');
    document.body.innerHTML = parsed.body.innerHTML;
  });
  for (const moduleFile of MODS_DASH) {
    await page.addScriptTag({ url: `http://127.0.0.1:${port}/${moduleFile}` });
  }
}

test('renderDashboard: 안정 컨테이너 4개 골격 + 오류 없음', async () => {
  await withPage([], async (page, { port }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await bootDash(page, port);

    const r = await page.evaluate(async () => {
      const host = document.createElement('div');
      document.body.appendChild(host);
      await window.renderDashboard(host);
      return {
        ids: ['dash-gauges', 'dash-workloads', 'dash-feed', 'dash-healing']
          .map(id => !!host.querySelector('#' + id)),
        segs: Array.from(host.querySelectorAll('.dash2-timeseg button'), b => b.dataset.span),
        wlRows: host.querySelectorAll('#dash-workloads tbody tr').length,
        errorText: host.querySelector(':scope > p.color-red')?.textContent || '',
      };
    });

    assert.deepEqual(pageErrors, []);
    assert.equal(r.errorText, '');
    assert.deepEqual(r.ids, [true, true, true, true]);
    assert.deepEqual(r.segs, ['live', '15m', '1h']);
    assert.equal(r.wlRows, 3);
  }, { routes: DASH_ROUTES });
});

test('대시보드 빠른 작업: keyboard focus 노출 + 768px touch target', async () => {
  await withPage([], async (page, { port }) => {
    await page.setViewport({ width: 1440, height: 900 });
    await bootDash(page, port);

    await page.evaluate(async () => {
      window.currentUser = { role: 'OPERATOR' };
      const host = document.createElement('div');
      document.body.appendChild(host);
      await window.renderDashboard(host);
      const qa = host.querySelector('#dash-workloads tbody tr .dash2-qa');
      const button = qa.querySelector('button');
      button.focus();
    });
                                                                      
                                                                 
    await page.waitForFunction(() => {
      const qa = document.querySelector('#dash-workloads tbody tr .dash2-qa');
      return qa && Number(getComputedStyle(qa).opacity) > 0.99;
    }, { timeout: 2_000 });
    const desktop = await page.evaluate(() => {
      const qa = document.querySelector('#dash-workloads tbody tr .dash2-qa');
      const button = qa.querySelector('button');
      return {
        opacity: getComputedStyle(qa).opacity,
        display: getComputedStyle(qa).display,
        active: document.activeElement === button,
        focusWithin: qa.matches(':focus-within'),
      };
    });
    assert.ok(Number(desktop.opacity) > 0.99, JSON.stringify(desktop));

    await page.setViewport({ width: 768, height: 900 });
    const tablet = await page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => {
      const host = document.body.lastElementChild;
      const qa = host.querySelector('#dash-workloads tbody tr .dash2-qa');
      qa.querySelector('button').blur();
      const rect = selector => {
        const box = host.querySelector(selector).getBoundingClientRect();
        return { width: box.width, height: box.height };
      };
      resolve({
        opacity: getComputedStyle(qa).opacity,
        timeseg: rect('.dash2-timeseg button'),
        chip: rect('.filterbar .chip'),
        viewportWidth: window.innerWidth,
        documentWidth: document.documentElement.scrollWidth,
      });
    })));
    for (const key of ['timeseg', 'chip']) {
      assert.ok(tablet[key].width >= 40, `${key} width=${tablet[key].width}`);
      assert.ok(tablet[key].height >= 40, `${key} height=${tablet[key].height}`);
    }
    assert.equal(tablet.opacity, '1');
    assert.ok(tablet.documentWidth <= tablet.viewportWidth,
      `document overflow ${tablet.documentWidth} > ${tablet.viewportWidth}`);
  }, { routes: DASH_ROUTES });
});

test('renderDashboard: 게이지 4타일(CPU/MEM/ZFS/네트워크) + peak 는 _dashSpan 창', async () => {
  await withPage([], async (page, { port }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await bootDash(page, port);

    const r = await page.evaluate(async () => {
      PCV.metrics.push('host.cpu', 12);
      PCV.metrics.push('host.cpu', 42);
      PCV.metrics.push('host.mem', 61);
      PCV.metrics.push('host.disk', 77);
      const host = document.createElement('div');
      document.body.appendChild(host);
      await window.renderDashboard(host);
      const tiles = Array.from(host.querySelectorAll('#dash-gauges > .dash2-gaugetile'));
      return {
        count: tiles.length,
        metrics: tiles.map(t => t.querySelector('.gauge-metric').textContent),
        cpuPeak: tiles[0].querySelector('.gauge-peak').textContent,
        zfsIdle: tiles[2].className.includes('g-idle'),
      };
    });

    assert.deepEqual(pageErrors, []);
    assert.equal(r.count, 4);
                                                                    
                                                              
                                                                  
    assert.deepEqual(r.metrics, ['42%', '61%', '—', '—']);
    assert.equal(r.cpuPeak, 'peak 42%');
    assert.ok(r.zfsIdle);
  }, { routes: DASH_ROUTES });
});

                                                                  
                                                             
                                                
test('renderDashboard: 4번째 게이지 = 네트워크 rate(peak 대비 % fill + 사람 단위 표시)', async () => {
  await withPage([], async (page, { port }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await bootDash(page, port);

    const r = await page.evaluate(async () => {
      PCV.metrics.push('host.net', 500000);                                 
      PCV.metrics.push('host.net', 1258291);                                
      const host = document.createElement('div');
      document.body.appendChild(host);
      await window.renderDashboard(host);
      const tiles = Array.from(host.querySelectorAll('#dash-gauges > .dash2-gaugetile'));
      const netTile = tiles[3];
      return {
        count: tiles.length,
        metric: netTile.querySelector('.gauge-metric').textContent,
        peak: netTile.querySelector('.gauge-peak').textContent,
        ticks: netTile.querySelectorAll('.tick').length,
        cls: netTile.className,
        fillWidth: netTile.querySelector('.fill').style.width,
      };
    });

    assert.deepEqual(pageErrors, []);
    assert.equal(r.count, 4);
    assert.equal(r.metric, '1.2 MB/s');
    assert.equal(r.peak, 'peak 1.2 MB/s');
    assert.equal(r.ticks, 0);                                                          
    assert.ok(r.cls.includes('g-ok'), r.cls);                          
    assert.equal(r.fillWidth, '100%');                                           
  }, { routes: DASH_ROUTES });
});

                                                               
                                                                           
                                                      
test('collectHostMetrics: node_network_* 존재 시 lo 제외 최대 디바이스 1개 선택 + 타일 라벨 반영', async () => {
  await withPage([], async (page, { port }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await bootDash(page, port);

    const metricsText = [
      'purecvisor_host_cpu_percent 10',
      'purecvisor_host_memory_percent 20',
      'node_network_receive_bytes_total{device="lo"} 500000000',
      'node_network_transmit_bytes_total{device="lo"} 500000000',
      'node_network_receive_bytes_total{device="eth0"} 1000000',
      'node_network_transmit_bytes_total{device="eth0"} 2000000',
      'node_network_receive_bytes_total{device="br0"} 5000000',
      'node_network_transmit_bytes_total{device="br0"} 6000000',
    ].join('\n') + '\n';

    const r = await page.evaluate(async (metricText) => {
      window.authToken = 'test-token';
      const nativeFetch = window.fetch.bind(window);
      window.fetch = (url, options) => {
        if (String(url).endsWith('/api/v1/metrics')) {
          return Promise.resolve(new Response(metricText,
            { status: 200, headers: { 'content-type': 'text/plain' } }));
        }
        return nativeFetch(url, options);
      };
      await window.collectHostMetrics();
      const host = document.createElement('div');
      document.body.appendChild(host);
      window._renderDashGauges(host);
      const netTile = host.querySelectorAll('.dash2-gaugetile')[3];
      return { netDevice: window._netDevice, label: netTile.querySelector('.gauge-lbl').textContent };
    }, metricsText);

    assert.deepEqual(pageErrors, []);
                                                               
    assert.equal(r.netDevice, 'br0');
    assert.equal(r.label, '네트워크 br0');
  }, { routes: DASH_ROUTES });
});

test('collectHostMetrics: 디바이스 선택은 sticky — 근접 트래픽 순위 역전에도 라벨 유지', async () => {
  await withPage([], async (page, { port }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await bootDash(page, port);

                                                                  
                                                                   
    const tick1 = [
      'purecvisor_host_cpu_percent 10', 'purecvisor_host_memory_percent 20',
      'node_network_receive_bytes_total{device="lo"} 100',
      'node_network_transmit_bytes_total{device="lo"} 100',
      'node_network_receive_bytes_total{device="eth0"} 4000000',
      'node_network_transmit_bytes_total{device="eth0"} 4000000',
      'node_network_receive_bytes_total{device="br0"} 4100000',
      'node_network_transmit_bytes_total{device="br0"} 4100000',
    ].join('\n') + '\n';
    const tick2 = [
      'purecvisor_host_cpu_percent 10', 'purecvisor_host_memory_percent 20',
      'node_network_receive_bytes_total{device="lo"} 100',
      'node_network_transmit_bytes_total{device="lo"} 100',
      'node_network_receive_bytes_total{device="eth0"} 9000000',
      'node_network_transmit_bytes_total{device="eth0"} 9000000',
      'node_network_receive_bytes_total{device="br0"} 4200000',
      'node_network_transmit_bytes_total{device="br0"} 4200000',
    ].join('\n') + '\n';

    const r = await page.evaluate(async (t1, t2) => {
      window.authToken = 'test-token';
      const nativeFetch = window.fetch.bind(window);
      let served = t1;
      window.fetch = (url, options) => {
        if (String(url).endsWith('/api/v1/metrics')) {
          return Promise.resolve(new Response(served,
            { status: 200, headers: { 'content-type': 'text/plain' } }));
        }
        return nativeFetch(url, options);
      };
      await window.collectHostMetrics();
      const firstDevice = window._netDevice;
      served = t2;
      await window.collectHostMetrics();
      return { firstDevice, secondDevice: window._netDevice };
    }, tick1, tick2);

    assert.deepEqual(pageErrors, []);
    assert.equal(r.firstDevice, 'br0');
    assert.equal(r.secondDevice, 'br0');                           
  }, { routes: DASH_ROUTES });
});

test('collectHostMetrics: vm-sum → NIC 소스 전환 틱은 push 생략(peak 오염 차단, 리뷰 HIGH-1)', async () => {
  await withPage([], async (page, { port }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await bootDash(page, port);

    const vmsumText = [
      'purecvisor_host_cpu_percent 10', 'purecvisor_host_memory_percent 20',
      'purecvisor_vm_net_rx_bytes_total{vm_name="a"} 1000000',
      'purecvisor_vm_net_tx_bytes_total{vm_name="a"} 1000000',
    ].join('\n') + '\n';
                                                                         
    const nicText = [
      'purecvisor_host_cpu_percent 10', 'purecvisor_host_memory_percent 20',
      'node_network_receive_bytes_total{device="lo"} 100',
      'node_network_transmit_bytes_total{device="lo"} 100',
      'node_network_receive_bytes_total{device="eth0"} 500000000000',
      'node_network_transmit_bytes_total{device="eth0"} 500000000000',
    ].join('\n') + '\n';

    const r = await page.evaluate(async (vmsum, nic) => {
      window.authToken = 'test-token';
      const nativeFetch = window.fetch.bind(window);
      let served = vmsum;
      window.fetch = (url, options) => {
        if (String(url).endsWith('/api/v1/metrics')) {
          return Promise.resolve(new Response(served,
            { status: 200, headers: { 'content-type': 'text/plain' } }));
        }
        return nativeFetch(url, options);
      };
      await window.collectHostMetrics();                                          
      window._prevNetSample.t -= 5000;                                              
      served = nic;
      await window.collectHostMetrics();                                                    
      const afterSwitch = PCV.metrics._buf('host.net').length;
      window._prevNetSample.t -= 5000;
      await window.collectHostMetrics();                                             
      const afterSameSource = PCV.metrics._buf('host.net').length;
      return {
        afterSwitch, afterSameSource,
        prevSrc: window._prevNetSample.src, prevDev: window._prevNetSample.dev,
      };
    }, vmsumText, nicText);

    assert.deepEqual(pageErrors, []);
    assert.equal(r.afterSwitch, 0, '소스 전환 틱은 dt>=2000 이어도 push 생략');
    assert.equal(r.afterSameSource, 1, '동일 소스 재수집은 정상 push(포지티브 컨트롤)');
    assert.equal(r.prevSrc, 'nic');
    assert.equal(r.prevDev, 'eth0');
  }, { routes: DASH_ROUTES });
});

test('collectHostMetrics: localStorage override 가 최대 트래픽보다 우선 적용된다', async () => {
  await withPage([], async (page, { port }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await bootDash(page, port);

    const metricsText = [
      'purecvisor_host_cpu_percent 10', 'purecvisor_host_memory_percent 20',
      'node_network_receive_bytes_total{device="lo"} 100',
      'node_network_transmit_bytes_total{device="lo"} 100',
      'node_network_receive_bytes_total{device="eth0"} 9000000',                         
      'node_network_transmit_bytes_total{device="eth0"} 9000000',
      'node_network_receive_bytes_total{device="br0"} 1000000',                             
      'node_network_transmit_bytes_total{device="br0"} 1000000',
    ].join('\n') + '\n';

    const r = await page.evaluate(async (metricText) => {
      window.authToken = 'test-token';
      localStorage.setItem('pcv-net-device', 'br0');
      const nativeFetch = window.fetch.bind(window);
      window.fetch = (url, options) => {
        if (String(url).endsWith('/api/v1/metrics')) {
          return Promise.resolve(new Response(metricText, { status: 200, headers: { 'content-type': 'text/plain' } }));
        }
        return nativeFetch(url, options);
      };
      await window.collectHostMetrics();
      const host = document.createElement('div');
      document.body.appendChild(host);
      window._renderDashGauges(host);
      const netTile = host.querySelectorAll('.dash2-gaugetile')[3];
      return { netDevice: window._netDevice, label: netTile.querySelector('.gauge-lbl').textContent };
    }, metricsText);

    assert.deepEqual(pageErrors, []);
    assert.equal(r.netDevice, 'br0', 'override 가 최대 트래픽(eth0)보다 우선해야 한다');
    assert.equal(r.label, '네트워크 br0');
  }, { routes: DASH_ROUTES });
});

test('collectHostMetrics: override 디바이스 소실 시 자동 폴백(저장값은 유지) + 전환 틱 push 생략', async () => {
  await withPage([], async (page, { port }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await bootDash(page, port);

                                                                           
    const withOvr = [
      'purecvisor_host_cpu_percent 10', 'purecvisor_host_memory_percent 20',
      'node_network_receive_bytes_total{device="lo"} 100',
      'node_network_transmit_bytes_total{device="lo"} 100',
      'node_network_receive_bytes_total{device="eth0"} 9000000',
      'node_network_transmit_bytes_total{device="eth0"} 9000000',
      'node_network_receive_bytes_total{device="ovr0"} 1000000',
      'node_network_transmit_bytes_total{device="ovr0"} 1000000',
    ].join('\n') + '\n';
                                                  
    const withoutOvr = [
      'purecvisor_host_cpu_percent 10', 'purecvisor_host_memory_percent 20',
      'node_network_receive_bytes_total{device="lo"} 100',
      'node_network_transmit_bytes_total{device="lo"} 100',
      'node_network_receive_bytes_total{device="eth0"} 9000000',
      'node_network_transmit_bytes_total{device="eth0"} 9000000',
    ].join('\n') + '\n';

    const r = await page.evaluate(async (t1, t2) => {
      window.authToken = 'test-token';
      localStorage.setItem('pcv-net-device', 'ovr0');
      const nativeFetch = window.fetch.bind(window);
      let served = t1;
      window.fetch = (url, options) => {
        if (String(url).endsWith('/api/v1/metrics')) {
          return Promise.resolve(new Response(served, { status: 200, headers: { 'content-type': 'text/plain' } }));
        }
        return nativeFetch(url, options);
      };
      await window.collectHostMetrics();                                                            
      const firstDevice = window._netDevice;
      window._prevNetSample.t -= 5000;
      served = t2;
      await window.collectHostMetrics();                                                                   
      const afterFallback = { device: window._netDevice, bufLen: PCV.metrics._buf('host.net').length };
      window._prevNetSample.t -= 5000;
      await window.collectHostMetrics();                                                            
      const afterSameSource = PCV.metrics._buf('host.net').length;
      return {
        firstDevice, afterFallback, afterSameSource,
        storedOverride: localStorage.getItem('pcv-net-device'),
      };
    }, withOvr, withoutOvr);

    assert.deepEqual(pageErrors, []);
    assert.equal(r.firstDevice, 'ovr0', '1틱은 override 적용');
    assert.equal(r.afterFallback.device, 'eth0', 'override 라인 소실 시 최대 트래픽으로 자동 폴백');
    assert.equal(r.afterFallback.bufLen, 0, '소스(dev) 전환 틱은 push 생략(기존 HIGH-1 가드 재사용 확인)');
    assert.equal(r.afterSameSource, 1, '전환 다음 틱(동일 소스 재수집)은 정상 push');
    assert.equal(r.storedOverride, 'ovr0', 'localStorage 저장값은 지우지 않는다 — 복귀 틱에 재적용 위함');
  }, { routes: DASH_ROUTES });
});

test('collectHostMetrics: node_network_* 부재(구 데몬) 시 경로 A(VM 합산) 폴백', async () => {
  await withPage([], async (page, { port }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await bootDash(page, port);

    const metricsText = [
      'purecvisor_host_cpu_percent 10',
      'purecvisor_host_memory_percent 20',
      'purecvisor_vm_net_rx_bytes_total{vm_name="a"} 1000000',
      'purecvisor_vm_net_tx_bytes_total{vm_name="a"} 2000000',
    ].join('\n') + '\n';

    const r = await page.evaluate(async (metricText) => {
      window.authToken = 'test-token';
      const nativeFetch = window.fetch.bind(window);
      window.fetch = (url, options) => {
        if (String(url).endsWith('/api/v1/metrics')) {
          return Promise.resolve(new Response(metricText,
            { status: 200, headers: { 'content-type': 'text/plain' } }));
        }
        return nativeFetch(url, options);
      };
      await window.collectHostMetrics();
      const host = document.createElement('div');
      document.body.appendChild(host);
      window._renderDashGauges(host);
      const netTile = host.querySelectorAll('.dash2-gaugetile')[3];
      return { netDevice: window._netDevice, label: netTile.querySelector('.gauge-lbl').textContent };
    }, metricsText);

    assert.deepEqual(pageErrors, []);
    assert.equal(r.netDevice, null);
    assert.equal(r.label, '네트워크 VM 합산');
  }, { routes: DASH_ROUTES });
});

                                                       
                                                         
                                                      
                                                        
test('collectHostMetrics: NIC → 구 데몬 다운그레이드 전이는 sticky 디바이스도 해제(라벨 정합)', async () => {
  await withPage([], async (page, { port }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await bootDash(page, port);

    const nicText = [
      'purecvisor_host_cpu_percent 10', 'purecvisor_host_memory_percent 20',
      'node_network_receive_bytes_total{device="lo"} 100',
      'node_network_transmit_bytes_total{device="lo"} 100',
      'node_network_receive_bytes_total{device="br0"} 5000000',
      'node_network_transmit_bytes_total{device="br0"} 5000000',
      'purecvisor_vm_net_rx_bytes_total{vm_name="a"} 1000000',
      'purecvisor_vm_net_tx_bytes_total{vm_name="a"} 2000000',
    ].join('\n') + '\n';
    const oldDaemonText = [
      'purecvisor_host_cpu_percent 10', 'purecvisor_host_memory_percent 20',
      'purecvisor_vm_net_rx_bytes_total{vm_name="a"} 1100000',
      'purecvisor_vm_net_tx_bytes_total{vm_name="a"} 2100000',
    ].join('\n') + '\n';

    const r = await page.evaluate(async (nic, old) => {
      window.authToken = 'test-token';
      const nativeFetch = window.fetch.bind(window);
      let served = nic;
      window.fetch = (url, options) => {
        if (String(url).endsWith('/api/v1/metrics')) {
          return Promise.resolve(new Response(served,
            { status: 200, headers: { 'content-type': 'text/plain' } }));
        }
        return nativeFetch(url, options);
      };
      const host = document.createElement('div');
      document.body.appendChild(host);
      const readLabel = () => {
        window._renderDashGauges(host);
        return host.querySelectorAll('.dash2-gaugetile')[3].querySelector('.gauge-lbl').textContent;
      };
      await window.collectHostMetrics();
      const nicDevice = window._netDevice, nicLabel = readLabel();
      served = old;
      window._prevNetSample.t -= 5000;                        
      await window.collectHostMetrics();
      return {
        nicDevice, nicLabel,
        afterDevice: window._netDevice, afterLabel: readLabel(),
        prevSrc: window._prevNetSample.src, prevDev: window._prevNetSample.dev,
        buf: PCV.metrics._buf('host.net').length,
      };
    }, nicText, oldDaemonText);

    assert.deepEqual(pageErrors, []);
    assert.equal(r.nicDevice, 'br0');
    assert.equal(r.nicLabel, '네트워크 br0');
    assert.equal(r.afterDevice, null, 'node_network_* 부재 틱은 sticky 선택 해제');
    assert.equal(r.afterLabel, '네트워크 VM 합산');
    assert.equal(r.prevSrc, 'vmsum');
    assert.equal(r.prevDev, null);
    assert.equal(r.buf, 0, '소스 전환 틱은 dt>=2000 이어도 push 생략(HIGH-1 유지)');
  }, { routes: DASH_ROUTES });
});

test('renderDashboard: 워크로드 err 매핑(crashed → crit dot/pill) + 상태 칩', async () => {
  await withPage([], async (page, { port }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await bootDash(page, port);

    const r = await page.evaluate(async () => {
      const host = document.createElement('div');
      document.body.appendChild(host);
      await window.renderDashboard(host);
      const rows = Array.from(host.querySelectorAll('#dash-workloads tbody tr'));
      const cell = row => ({
        name: row.querySelector('b').textContent,
        dot: row.querySelector('.sdot').className,
        pill: row.querySelector('.pill').className,
      });
      return {
        rows: rows.map(cell),
        chips: Array.from(host.querySelectorAll('#dash-workloads .chip'),
          c => c.dataset.facet + ':' + c.dataset.val),
      };
    });

    assert.deepEqual(pageErrors, []);
    assert.equal(r.rows.length, 3);
    assert.ok(r.rows[0].dot.includes('sdot-ok'));
    assert.ok(r.rows[1].dot.includes('sdot-idle'));
    assert.equal(r.rows[2].name, 'vm-bad');
    assert.ok(r.rows[2].dot.includes('sdot-crit'), r.rows[2].dot);
    assert.ok(r.rows[2].pill.includes('pill-crit'), r.rows[2].pill);
                                                          
    assert.deepEqual(r.chips, [
      'status:running', 'status:stopped', 'status:error', 'type:vm', 'type:lxc',
    ]);
  }, { routes: DASH_ROUTES });
});

                                                                  
                                                     
                                                          
                                           
const NOW_SEC = () => Math.floor(Date.now() / 1000);

test('_renderDashFeed: alerts+sec epoch 초 병합 정렬(최신 우선) + 상위 8건', async () => {
  await withPage([], async (page, { port }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await bootDash(page, port);

    const r = await page.evaluate(async (nowSec) => {
      const filler = [];
      for (let i = 0; i < 8; i++) {
        filler.push({ timestamp: nowSec - 1000 - i, severity: 'warn', message: 'filler-' + i });
      }
      window._shellSlow.raw = {
        alerts: filler.concat([
          { timestamp: nowSec - 600, severity: 'warn', message: 'a-old' },
          { timestamp: nowSec - 60, severity: 'crit', message: 'a-new' },
        ]),
        sec: [{ timestamp: nowSec - 300, severity: 'critical', summary: 's-mid' }],
      };
      const host = document.createElement('div');
      document.body.appendChild(host);
      window._renderDashFeed(host);
      const rows = Array.from(host.querySelectorAll('.dash2-feed-item'));
      return {
        count: rows.length,
        order: rows.slice(0, 3).map(x => x.children[1].textContent),
        header: host.querySelector('h4').textContent,
        times: rows.map(x => x.querySelector('.dash2-feed-time').textContent),
      };
    }, NOW_SEC());

    assert.deepEqual(pageErrors, []);
    assert.equal(r.count, 8);                                          
    assert.deepEqual(r.order, ['a-new', 's-mid', 'a-old']);
                                                                        
                                                      
    assert.match(r.header, /\(총 11 · 10 unack\)/, r.header);
                                                                        
    assert.ok(r.times.every(t => !t.includes('1970')), r.times.join(','));
  }, { routes: DASH_ROUTES });
});

test('_renderDashFeed: 보안 이벤트는 f-sec/SEC + 행 클릭 → mon-security', async () => {
  await withPage([], async (page, { port }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await bootDash(page, port);

    const r = await page.evaluate(async (nowSec) => {
      window._navCalls = [];
      window.navigateTo = (n) => { window._navCalls.push(n); return true; };
      window._shellSlow.raw = {
        alerts: [{ timestamp: nowSec - 30, severity: 'crit', message: 'alert-row' }],
        sec: [{ timestamp: nowSec - 10, severity: 'high', summary: 'ssh brute force' }],
      };
      const host = document.createElement('div');
      document.body.appendChild(host);
      window._renderDashFeed(host);
      const rows = Array.from(host.querySelectorAll('.dash2-feed-item'));
      rows[0].click();                
      rows[1].click();                   
      return {
        classes: rows.map(x => x.className),
        sevs: rows.map(x => x.querySelector('.dash2-feed-sev').textContent),
        texts: rows.map(x => x.children[1].textContent),
        nav: window._navCalls.slice(),
      };
    }, NOW_SEC());

    assert.deepEqual(pageErrors, []);
    assert.deepEqual(r.classes, ['dash2-feed-item f-sec', 'dash2-feed-item f-crit']);
    assert.deepEqual(r.sevs, ['SEC', 'CRIT']);
    assert.deepEqual(r.texts, ['ssh brute force', 'alert-row']);
    assert.deepEqual(r.nav, ['mon-security', 'mon-alerts']);
  }, { routes: DASH_ROUTES });
});

                                                                     
                                                             
test('_renderDashHealing: 승인 버튼 → ai.healing.approve RPC body(action_id)', async () => {
  await withPage([], async (page, { port, requests }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await bootDash(page, port);

    const r = await page.evaluate(async (nowSec) => {
      window._shellSlow.raw = {
        healing: [{ id: 7, policy: 'vm-reboot-loop', action: 'restart',
          reason: 'crash x3 in 5m', ts: nowSec - 120 }],
      };
      const host = document.createElement('div');
      document.body.appendChild(host);
      window._renderDashHealing(host);
      const row = host.querySelector('.dash2-healrow');
      const btns = Array.from(row.querySelectorAll('button'));
      btns[0].click();
      await new Promise(resolve => setTimeout(resolve, 150));
      return {
        tag: row.querySelector('.dash2-heal-tag').textContent,
        name: row.querySelector('b').textContent,
        summary: row.querySelector('.dash2-wl-meta').textContent,
        time: row.querySelector('.dash2-feed-time').textContent,
        btnCount: btns.length,
      };
    }, NOW_SEC());

    assert.deepEqual(pageErrors, []);
                                                                       
    assert.equal(r.tag, 'restart');
    assert.equal(r.name, 'vm-reboot-loop');
    assert.equal(r.summary, 'crash x3 in 5m');
    assert.ok(!r.time.includes('1970'), r.time);
    assert.equal(r.btnCount, 2);
    const rpc = requests.filter(x => x.path === '/api/v1/rpc');
    assert.equal(rpc.length, 1);
    assert.equal(rpc[0].method, 'POST');
    assert.equal(rpc[0].json.method, 'ai.healing.approve');
    assert.deepEqual(rpc[0].json.params, { action_id: 7 });
  }, { routes: { ...DASH_ROUTES, '/api/v1/rpc': { body: { result: { ok: true } } } } });
});

                                                        
                                               
                               
test('_renderDashHealing: VIEWER·OPERATOR는 숨기고 ADMIN에게만 승인/거부를 표시', async () => {
  await withPage([], async (page, { port }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await bootDash(page, port);

    const r = await page.evaluate(async (nowSec) => {
      window._shellSlow.raw = {
        healing: [{ id: 3, policy: 'p', action: 'restart', reason: 'r', ts: nowSec - 10 }],
      };
      const host = document.createElement('div');
      document.body.appendChild(host);
      const disp = () => Array.from(host.querySelectorAll('.dash2-healrow button'),
        b => getComputedStyle(b).display);
      window.currentUser = { role: 'VIEWER' };
      window._renderDashHealing(host);
      const viewer = disp();
      window.currentUser = { role: 'OPERATOR' };
      window._renderDashHealing(host);
      const operator = disp();
      window.currentUser = { role: 'ADMIN' };
      window._renderDashHealing(host);
      const admin = disp();
      return { viewer, operator, admin };
    }, NOW_SEC());

    assert.deepEqual(pageErrors, []);
    assert.deepEqual(r.viewer, ['none', 'none']);
    assert.deepEqual(r.operator, ['none', 'none']);
    assert.ok(r.admin.every(d => d !== 'none'), r.admin.join(','));
  }, { routes: DASH_ROUTES });
});

                                                   
                                                      
                                                                
                                                    
test('_collectShellSlow: RPC 실패 분기는 raw 를 이전 값으로 유지', async () => {
  await withPage([], async (page, { port }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await bootDash(page, port);

    const r = await page.evaluate(async () => {
      window.authToken = 'test-token';
      const seedSec = [{ timestamp: 1, severity: 'high', summary: 'kept-sec' }];
      const seedHeal = [{ id: 1, policy: 'kept-policy', action: 'restart', reason: 'r', ts: 2 }];
      window._shellSlow.raw.sec = seedSec;
      window._shellSlow.raw.healing = seedHeal;
      await window._collectShellSlow();
      return {
        secCount: window._shellSlow.sec,
        healCount: window._shellSlow.healing,
        rawSec: window._shellSlow.raw.sec.map(x => x.summary),
        rawHeal: window._shellSlow.raw.healing.map(x => x.policy),
      };
    });

    assert.deepEqual(pageErrors, []);
    assert.equal(r.secCount, null);                             
    assert.equal(r.healCount, null);
    assert.deepEqual(r.rawSec, ['kept-sec']);                    
    assert.deepEqual(r.rawHeal, ['kept-policy']);
  }, { routes: { ...DASH_ROUTES, '/api/v1/rpc': { body: { error: { message: 'rpc down' } } } } });
});

                                                                  
                                                          
                                        
test('_collectShellSlow: raw.pools 는 unwrapList 직후 보존(풀 0개에도 stale 방지)', async () => {
  await withPage([], async (page, { port }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await bootDash(page, port);

    const r = await page.evaluate(async () => {
      window.authToken = 'test-token';
      window._shellSlow.raw.pools = [{ name: 'stale-pool' }];               
      await window._collectShellSlow();
      return { storage: window._shellSlow.storage, rawPools: window._shellSlow.raw.pools };
    });

    assert.deepEqual(pageErrors, []);
    assert.equal(r.storage, null);                                
    assert.deepEqual(r.rawPools, []);                                           
  }, { routes: { ...DASH_ROUTES, '/api/v1/storage/pools': { body: { data: [] } } } });
});

test('_collectShellSlow: fleet/networks HTTP 오류에도 raw 는 최신 빈 배열로 갱신(raw.pools 선례, M-1)', async () => {
  await withPage([], async (page, { port }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await bootDash(page, port);

    const r = await page.evaluate(async () => {
      window.authToken = 'test-token';
      window._shellSlow.raw.fleet = [{ name: 'stale-vm', ip: '10.0.0.1' }];               
      window._shellSlow.raw.networks = [{ name: 'stale-net' }];
      await window._collectShellSlow();
      return { rawFleet: window._shellSlow.raw.fleet, rawNetworks: window._shellSlow.raw.networks };
    });

    assert.deepEqual(pageErrors, []);
    assert.deepEqual(r.rawFleet, []);                                                      
    assert.deepEqual(r.rawNetworks, []);
  }, { routes: { ...DASH_ROUTES,
    '/api/v1/monitor/fleet': { status: 500, body: { error: { message: 'down' } } },
    '/api/v1/networks': { status: 500, body: { error: { message: 'down' } } },
  } });
});

test('renderDashboard: 타임세그 전환은 컨테이너 identity 보존(부분 재렌더)', async () => {
  await withPage([], async (page, { port }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await bootDash(page, port);

    const r = await page.evaluate(async () => {
      const host = document.createElement('div');
      document.body.appendChild(host);
      await window.renderDashboard(host);
      const before = {
        gauges: host.querySelector('#dash-gauges'),
        workloads: host.querySelector('#dash-workloads'),
      };
      host.querySelector('.dash2-timeseg button[data-span="1h"]').click();
      const after = {
        gauges: host.querySelector('#dash-gauges'),
        workloads: host.querySelector('#dash-workloads'),
      };
      return {
        sameGauges: before.gauges === after.gauges,
        sameWorkloads: before.workloads === after.workloads,
        span: window._dashSpan,
        on: Array.from(host.querySelectorAll('.dash2-timeseg button.on'), b => b.dataset.span),
        tiles: host.querySelectorAll('#dash-gauges > .dash2-gaugetile').length,
      };
    });

    assert.deepEqual(pageErrors, []);
    assert.ok(r.sameGauges);
    assert.ok(r.sameWorkloads);
    assert.equal(r.span, '1h');
    assert.deepEqual(r.on, ['1h']);
    assert.equal(r.tiles, 4);
  }, { routes: DASH_ROUTES });
});

                                                           
                                                     
                                                        
                                                     
const NEXT_VMS = [
  { name: 'vm-run', state: 'shutoff', vcpu: 4, memory_mb: 4096 },                              
  { name: 'vm-new', state: 'running', vcpu: 1, live_cpu_pct: 5, memory_mb: 1024, memory_used_mb: 512 },
];                                               

test('loadAll: 대시보드 부분 재렌더가 _dashVms 를 갱신 → 표가 폴러 결과를 반영(H1-1)', async () => {
  await withPage([], async (page, { port }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await bootDash(page, port);

                                                                  
                                                              
                                                                 
    const r = await page.evaluate(async () => {
      const host = document.createElement('div');
      document.body.appendChild(host);
      await window.renderDashboard(host);
      const names = () => Array.from(host.querySelectorAll('#dash-workloads tbody tr b'), b => b.textContent);
      const before = names();
      await window.loadAll();                             
      const after = names();
      const runPill = host.querySelector('#dash-workloads tbody tr .pill')?.textContent;
      return { before, after, runPill };
    });

    assert.deepEqual(pageErrors, []);
    assert.deepEqual(r.before, ['vm-run', 'vm-stop', 'vm-bad']);
    assert.deepEqual(r.after, ['vm-run', 'vm-new']);                     
    assert.equal(r.runPill, 'shutoff');                                   
  }, { routes: { ...DASH_ROUTES, '/api/v1/vms': (record, reqs) => ({
    body: reqs.filter(x => x.path === '/api/v1/vms').length <= 1 ? DASH_VMS : NEXT_VMS,
  }) } });
});

                                                           
                                                        
                                                     
                                              
test('_renderDashWorkloads: VIEWER 퀵액션은 폴링 재렌더 후에도 display:none(M1)', async () => {
  await withPage([], async (page, { port }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await bootDash(page, port);

    const r = await page.evaluate(async () => {
      const host = document.createElement('div');
      document.body.appendChild(host);
      window.currentUser = { role: 'VIEWER' };
      await window.renderDashboard(host);
                                                            
                                                              
                             
      const disp = () => Array.from(host.querySelectorAll('.dash2-qa'),
        q => getComputedStyle(q).display);
      const initial = disp();
      await window.loadAll();                                        
      const afterPoll = disp();
      window.currentUser = { role: 'OPERATOR' };
      const w = document.getElementById('dash-workloads');
      window._renderDashWorkloads(w);
      const operator = disp();
      return { initial, afterPoll, operator };
    });

    assert.deepEqual(pageErrors, []);
    assert.ok(r.initial.length > 0);
    assert.ok(r.initial.every(d => d === 'none'), r.initial.join(','));
    assert.ok(r.afterPoll.every(d => d === 'none'), r.afterPoll.join(','));
    assert.ok(r.operator.some(d => d !== 'none'), r.operator.join(','));
  }, { routes: DASH_ROUTES });
});

test('_dashFocusVmByName: vmList 재정렬 후 퀵액션 클릭이 이름 기준으로 재조준(H1-2)', async () => {
  await withPage([], async (page, { port }) => {
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(String(error)));
    await bootDash(page, port);

    const r = await page.evaluate(async () => {
      window._vmPowerCalls = [];
      window.vmPower = (action) => { window._vmPowerCalls.push(action); };
      const host = document.createElement('div');
      document.body.appendChild(host);
      await window.renderDashboard(host);                                           
      await window.loadAll();                                         
                                                        
                                                  
                                      
      window.vmList = window.vmList.slice().reverse();
      window.selectedVmIndex = -1;
      const firstRowQa = host.querySelector('#dash-workloads tbody tr .dash2-qa');
      firstRowQa.querySelector('button').click();                        
      return {
        selectedVmIndex: window.selectedVmIndex,
        expected: window.vmList.findIndex(v => v.name === 'vm-run'),
        vmPowerCalls: window._vmPowerCalls,
      };
    });

    assert.deepEqual(pageErrors, []);
    assert.notEqual(r.expected, 0);                                           
    assert.equal(r.selectedVmIndex, r.expected);                                          
    assert.deepEqual(r.vmPowerCalls, ['stop']);
  }, { routes: DASH_ROUTES });
});
