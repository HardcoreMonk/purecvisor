

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { withPage } from './harness.mjs';

async function deliver(page, payload) {
  return page.evaluate(value => {
    window.authToken = 'fixture';
    window.notices = [];
    window.events = [];
    window.addNotification = (...args) => notices.push(args);
    window.addEvt = message => events.push(message);
    window._L = (ko, en) => en;
    window.WebSocket = class {
      static OPEN = 1;
      constructor() { this.readyState = 1; }
      send() {}
    };
    PCV.api.connectWS();
    wsConnection._pcvAuthDone = true;
    wsConnection.onmessage({ data: JSON.stringify({ type: 'job.complete', payload: value }) });
    return { notices, events };
  }, payload);
}

test('성공 작업의 결과 저장 실패는 Job ID를 포함한 warning 한 번으로 전달한다', async () => {
  await withPage(['ui/modules/endpoints.js', 'ui/modules/api.js'], async page => {
    const result = await deliver(page, { job_id: 'job-12345678', method: 'vm.create',
      status: 'completed', result_persisted: false });
    assert.equal(result.notices.length, 1);
    assert.equal(result.notices[0][0], 'warn');
    assert.match(result.notices[0][2], /job-12345678/);
    assert.match(result.notices[0][2], /completed/);
    assert.ok(result.events.some(message => message.includes('job-12345678')));
  });
});

test('저장 완료·구 서버 필드 없음·문자열 false는 저장 오류 알림을 만들지 않는다', async () => {
  await withPage(['ui/modules/endpoints.js', 'ui/modules/api.js'], async page => {
    for (const field of [{ result_persisted: true }, {}, { result_persisted: 'false' }]) {
      const result = await deliver(page, { job_id: 'job-12345678', method: 'vm.create',
        status: 'completed', ...field });
      assert.equal(result.notices.length, 0);
      assert.equal(result.events.filter(message => message.startsWith('WARN Job result')).length, 0);
    }
  });
});

test('실제 작업 실패와 저장 실패는 각각의 사유를 보존한다', async () => {
  await withPage(['ui/modules/endpoints.js', 'ui/modules/api.js'], async page => {
    const result = await deliver(page, { job_id: 'job-12345678', method: 'backup.restore',
      status: 'failed', error: 'backend failed', result_persisted: false });
    assert.equal(result.notices.length, 2);
    assert.ok(result.notices.some(n => n[0] === 'error' && n[2] === 'backend failed'));
    assert.ok(result.notices.some(n => n[0] === 'warn' && n[2].includes('job-12345678')));
  });
});

test('서버 사유·Job ID의 마크업은 알림 인자로만 전달한다', async () => {
  await withPage(['ui/modules/endpoints.js', 'ui/modules/api.js'], async page => {
    const result = await deliver(page, { job_id: 'job-<img src=x onerror=alert(1)>',
      method: 'vm.create', status: 'completed', result_persisted: false });
    assert.equal(result.notices.length, 1);
    assert.match(result.notices[0][2], /<img/);
    assert.equal(await page.$('img'), null);
  });
});
