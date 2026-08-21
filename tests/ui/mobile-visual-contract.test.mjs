                                                                                               
                                                    
                                                                 
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const MODS = ['ui/modules/ui.js', 'ui/modules/uxlib.js', 'ui/modules/filter-state.js', 'ui/modules/mobile.js'];

const LONG_ALERT = '스토리지 풀의 사용률이 임계값을 초과했습니다. 데이터 보호를 위해 즉시 여유 공간과 스냅샷 보존 정책을 확인하세요.';
const LONG_REASON = '연속 상태 확인 실패가 감지되어 워크로드 재시작 승인을 기다리고 있습니다.';

function fixtures() {
  return {
    home: {
      vms: [{ name: 'web', state: 'running' }],
      containers: [{ name: 'edge-container', state: 'RUNNING' }],
      alerts: [{ severity: 'crit', message: LONG_ALERT }],
      hostCpu: 43,
      hostMem: 58,
    },
    alerts: {
      alerts: [{ alert_id: 7, severity: 'crit', metric: 'storage', value: 96.2, message: LONG_ALERT }],
      suricata: { engine: { state: 'active' }, eve_tail: { alerts_crit: 0, alerts_warn: 0 } },
      ips: { enabled: true },
      filter: [],
    },
    power: {
      vms: [{ name: 'very-long-production-workload-name-that-must-ellipsize', state: 'running' }],
      containers: [{ name: 'edge-container', state: 'RUNNING' }],
    },
    healing: {
      mode: 'dry_run',
      pending: [{ id: 9, policy: 'production-workload-recovery-policy', action: 'restart', reason: LONG_REASON }],
      history: [],
    },
  };
}

test('mobile tabs use one local Coolicons family with labels and touch targets', async () => {
  await withPage(MODS, async (page) => {
    await page.setViewport({ width: 390, height: 844 });
    const result = await page.evaluate(() => {
      window.PCV.mobile.mount();
      return {
        hrefs: Array.from(document.querySelectorAll('.mnav .mnav-icon use'), use => use.getAttribute('href')),
        labels: Array.from(document.querySelectorAll('.mnav-label'), node => node.textContent),
        heights: Array.from(document.querySelectorAll('.mnav-item'), node => node.getBoundingClientRect().height),
        rawText: document.querySelector('.mnav').textContent,
        active: document.querySelector('.mnav-item.on').getAttribute('aria-current'),
      };
    });

    assert.deepEqual(result.hrefs, [
      'vendor/coolicons/coolicons.svg#ci-house-01',
      'vendor/coolicons/coolicons.svg#ci-bell',
      'vendor/coolicons/coolicons.svg#ci-play',
      'vendor/coolicons/coolicons.svg#ci-shield-check',
    ]);
    assert.deepEqual(result.labels, ['홈', '알림', '전원', '치유']);
    assert.ok(result.heights.every(height => height >= 48));
    assert.ok(!/[⌂🔔⏻🩹]/u.test(result.rawText));
    assert.equal(result.active, 'page');
  });
});

test('mobile builders expose page headings and preserve long action context without horizontal overflow', async () => {
  await withPage(MODS, async (page) => {
    await page.setViewport({ width: 390, height: 844 });
    const at390 = await page.evaluate((data) => {
      const builders = {
        home: window.PCV.mobile.buildHome,
        alerts: window.PCV.mobile.buildAlerts,
        power: window.PCV.mobile.buildPower,
        healing: window.PCV.mobile.buildHealing,
      };
      const expected = {
        home: '운영 개요', alerts: '알림·보안', power: '전원 관리', healing: '자가치유 승인',
      };
      const results = {};
      Object.keys(builders).forEach((key) => {
        const node = builders[key](data[key]);
        document.getElementById('cb').replaceChildren(node);
        const copy = node.querySelector('.m-copy');
        const name = node.querySelector('.m-vm .m-name');
        results[key] = {
          heading: node.querySelector('h1').textContent,
          overflow: node.scrollWidth > node.clientWidth,
          copyWhiteSpace: copy ? getComputedStyle(copy).whiteSpace : null,
          copyFits: copy ? copy.scrollWidth <= copy.clientWidth : null,
          vmWhiteSpace: name ? getComputedStyle(name).whiteSpace : null,
        };
        if (results[key].heading !== expected[key]) throw new Error('unexpected heading for ' + key);
      });
      return results;
    }, fixtures());

    assert.deepEqual(Object.values(at390).map(item => item.heading), [
      '운영 개요', '알림·보안', '전원 관리', '자가치유 승인',
    ]);
    assert.ok(Object.values(at390).every(item => item.overflow === false));
    assert.equal(at390.alerts.copyWhiteSpace, 'normal');
    assert.equal(at390.alerts.copyFits, true);
    assert.equal(at390.healing.copyWhiteSpace, 'normal');
    assert.equal(at390.healing.copyFits, true);
    assert.equal(at390.power.vmWhiteSpace, 'nowrap');

    for (const width of [480, 600]) {
      await page.setViewport({ width, height: 900 });
      const fits = await page.evaluate((data) => {
        const node = window.PCV.mobile.buildHome(data.home);
        document.getElementById('cb').replaceChildren(node);
        return node.scrollWidth <= node.clientWidth && document.documentElement.scrollWidth <= innerWidth;
      }, fixtures());
      assert.equal(fits, true, `${width}px home must not overflow horizontally`);
    }
  });
});
