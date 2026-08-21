                                                                                          
                                                                                 
                                                                  
  
                            
  
                                                 
                                                       
                                  
   
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

const MODS = [
  'ui/modules/ui.js',
  'ui/modules/filter-state.js',
  'ui/modules/uxlib.js',
  'ui/modules/help.js',
  'ui/modules/nav.js'
];

async function openFixture(page) {
  await page.evaluate(() => {
    const badge = document.createElement('span');
    badge.id = 'notif-toolbar-badge';
    document.body.appendChild(badge);
    let nextId = 1000;
    Date.now = () => ++nextId;
    window.sendBrowserNotif = () => {};
    window.playNotifSound = () => {};
    addNotification('info', '배포 완료', '노드가 최신 상태입니다');
    addNotification('warning', '디스크 경고', '사용량이 80%를 넘었습니다');
    addNotification('error', 'Webhook 실패', '전달 대기열을 확인하세요');
    toggleNotifCenter();
  });
  await page.waitForSelector('#notif-center:popover-open');
}

test('notification center uses localized semantic controls and local icons', async () => {
  await withPage(MODS, async page => {
    await openFixture(page);
    const result = await page.evaluate(() => ({
      role: document.getElementById('notif-center').getAttribute('role'),
      labelledBy: document.getElementById('notif-center').getAttribute('aria-labelledby'),
      title: document.getElementById('notif-center-title').textContent,
      summary: document.getElementById('notif-unread-summary').textContent,
      actions: Array.from(document.querySelectorAll('.notif-header-action'))
        .map(node => node.textContent),
      closeLabel: document.querySelector('.notif-close').getAttribute('aria-label'),
      groupRole: document.querySelector('.notif-filter-bar').getAttribute('role'),
      groupLabel: document.querySelector('.notif-filter-bar').getAttribute('aria-label'),
      filters: Array.from(document.querySelectorAll('.notif-filter-btn'))
        .map(node => ({ text: node.textContent, pressed: node.getAttribute('aria-pressed') })),
      itemTags: Array.from(document.querySelectorAll('.notif-item')).map(node => node.tagName),
      types: Array.from(document.querySelectorAll('.notif-type')).map(node => node.textContent),
      hrefs: Array.from(document.querySelectorAll('#notif-center svg use'))
        .map(node => node.getAttribute('href')),
      text: document.getElementById('notif-center').textContent
    }));

    assert.equal(result.role, 'region');
    assert.equal(result.labelledBy, 'notif-center-title');
    assert.equal(result.title, '알림');
    assert.equal(result.summary, '3개 미확인');
    assert.deepEqual(result.actions, ['모두 확인', '지우기']);
    assert.equal(result.closeLabel, '알림 센터 닫기');
    assert.equal(result.groupRole, 'group');
    assert.equal(result.groupLabel, '알림 유형 필터');
    assert.deepEqual(result.filters, [
      { text: '전체 (3)', pressed: 'true' },
      { text: '오류 (1)', pressed: 'false' },
      { text: '경고 (1)', pressed: 'false' },
      { text: '정보 (1)', pressed: 'false' }
    ]);
    assert.deepEqual(result.itemTags, ['BUTTON', 'BUTTON', 'BUTTON']);
    assert.deepEqual(result.types, ['오류', '경고', '정보']);
    assert.equal(result.hrefs.length, 4);
    result.hrefs.forEach(href => assert.match(
      href,
      /^vendor\/coolicons\/coolicons\.svg#ci-/
    ));
    assert.doesNotMatch(result.text, /[❌⚠✅🔔✕]/u);
    assert.doesNotMatch(result.text, /Notifications|unread|Mark all read|Clear/);
  });
});

test('filter and keyboard read action update state without losing focus', async () => {
  await withPage(MODS, async page => {
    await openFixture(page);
    await page.click('.notif-filter-btn[data-filter="warning"]');

    let state = await page.evaluate(() => ({
      pressed: Array.from(document.querySelectorAll('.notif-filter-btn'))
        .filter(node => node.getAttribute('aria-pressed') === 'true')
        .map(node => node.dataset.filter),
      items: Array.from(document.querySelectorAll('.notif-item'))
        .map(node => node.querySelector('.notif-type').textContent),
      unread: document.querySelector('.notif-item').classList.contains('unread')
    }));
    assert.deepEqual(state, { pressed: ['warning'], items: ['경고'], unread: true });

    await page.focus('.notif-item');
    await page.keyboard.press('Enter');
    state = await page.evaluate(() => ({
      focused: document.activeElement.classList.contains('notif-item'),
      unread: document.querySelector('.notif-item').classList.contains('unread'),
      aria: document.querySelector('.notif-item').getAttribute('aria-label'),
      summary: document.getElementById('notif-unread-summary').textContent,
      toolbarBadge: document.getElementById('notif-toolbar-badge').textContent
    }));
    assert.equal(state.focused, true);
    assert.equal(state.unread, false);
    assert.match(state.aria, /^확인한 알림\./);
    assert.equal(state.summary, '2개 미확인');
    assert.equal(state.toolbarBadge, '2');
  });
});

test('empty state and close action preserve Popover lifecycle', async () => {
  await withPage(MODS, async page => {
    await openFixture(page);
    await page.click('.notif-header-action.btn-r');
    const empty = await page.evaluate(() => ({
      text: document.querySelector('.notif-center-empty').textContent.trim(),
      icon: document.querySelector('.notif-center-empty use').getAttribute('href'),
      itemCount: document.querySelectorAll('.notif-item').length,
      summaryHidden: document.getElementById('notif-unread-summary').hidden
    }));
    assert.equal(empty.text, '알림이 없습니다');
    assert.equal(empty.icon, 'vendor/coolicons/coolicons.svg#ci-bell');
    assert.equal(empty.itemCount, 0);
    assert.equal(empty.summaryHidden, true);

    await page.click('.notif-close');
    await page.waitForFunction(() => !document.getElementById('notif-center'));
    const closed = await page.evaluate(() => ({
      open: window.notifCenterOpen,
      nodeExists: !!document.getElementById('notif-center')
    }));
    assert.equal(closed.open, false);
    assert.equal(closed.nodeExists, false);
  });
});

test('notification filters and close affordance meet the 768px touch target', async () => {
  await withPage(MODS, async page => {
    await page.setViewport({ width: 768, height: 900 });
    await openFixture(page);
    const result = await page.evaluate(() => ({
      controls: Array.from(document.querySelectorAll('.notif-filter-btn,.notif-close'))
        .map(node => {
          const rect = node.getBoundingClientRect();
          return { width: rect.width, height: rect.height };
        }),
      overflow: document.documentElement.scrollWidth -
        document.documentElement.clientWidth
    }));
    result.controls.forEach(control => {
      assert.ok(control.width >= 40, `touch width ${control.width}`);
      assert.ok(control.height >= 40, `touch height ${control.height}`);
    });
    assert.equal(result.overflow, 0);
  });
});
