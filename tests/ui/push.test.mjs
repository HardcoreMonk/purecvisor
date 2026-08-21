                                                                                         
                                                        
                                                                         
  
                                       
  
                  
                                                                 
                                                    
                                                                     
                                                                
                                                                    
                                                                   
                                                                    
                                                      
  
               
                                                   
                                                 
                          
   
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { withPage } from './harness.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const SW_SRC = fs.readFileSync(path.join(ROOT, 'ui/sw.js'), 'utf8');

                                                               
function loadSw(clientList = [], { swSubscribe = null } = {}) {
  const listeners = new Map();
  const shown = [];
  const opened = [];
  const matchAllArgs = [];
  const swSubscribeArgs = [];
  const self = {
    addEventListener(type, fn) {
      if (!listeners.has(type)) listeners.set(type, []);
      listeners.get(type).push(fn);
    },
    skipWaiting() {},
    registration: {
      showNotification(title, opts) {
        shown.push({ title, opts });
        return Promise.resolve();
      },
                                                                 
      pushManager: {
        subscribe(opts) {
          swSubscribeArgs.push(opts);
          return swSubscribe
            ? Promise.resolve(swSubscribe)
            : Promise.reject(new Error('no subscribe'));
        },
      },
    },
    clients: {
      claim: () => Promise.resolve(),
      matchAll: (opts) => { matchAllArgs.push(opts); return Promise.resolve(clientList); },
      openWindow: (url) => { opened.push(url); return Promise.resolve(null); },
    },
  };
                                                    
                                                     
                 
  const stores = new Map();
  const deleted = [];
  const caches = {
    open: (name) => {
      if (!stores.has(name)) stores.set(name, new Map());
      const s = stores.get(name);
      return Promise.resolve({
        add: () => Promise.resolve(),
        put: (req, res) => { s.set(typeof req === 'string' ? req : req.url, res); return Promise.resolve(); },
        match: (k) => Promise.resolve(s.get(typeof k === 'string' ? k : k.url)),
        delete: (k) => Promise.resolve(s.delete(typeof k === 'string' ? k : k.url)),
      });
    },
    keys: () => Promise.resolve([...stores.keys()]),
    match: () => Promise.resolve(undefined),
    delete: (name) => { deleted.push(name); stores.delete(name); return Promise.resolve(true); },
  };
                                                          
                                                                
                                                         
                                                             
  class SwRequest {
    constructor(u) { this.url = new URL(String(u), 'https://sw.test').toString(); }
  }
  vm.runInNewContext(SW_SRC, {
    self, caches, console, URL, Request: SwRequest, Response, Date,
    fetch: () => Promise.reject(new Error('no net')),
  });

  async function dispatch(type, event) {
    const waits = [];
    const ev = Object.assign({}, event, { waitUntil: (p) => { waits.push(p); } });
    for (const fn of listeners.get(type) || []) fn(ev);
    await Promise.all(waits);
  }
  return { listeners, shown, opened, matchAllArgs, dispatch, stores, deleted, swSubscribeArgs };
}

test('sw push: 페이로드를 showNotification 으로 띄운다', async () => {
  const sw = loadSw();
  await sw.dispatch('push', {
    data: { json: () => ({ title: 't', body: 'b', severity: 'crit', url: '#mon-alerts' }) },
  });
  assert.equal(sw.shown.length, 1);
  assert.equal(sw.shown[0].title, 't');
  assert.equal(sw.shown[0].opts.body, 'b');
  assert.equal(sw.shown[0].opts.tag, 'pcv-alert');
                                                   
  assert.equal(sw.shown[0].opts.renotify, true);
  assert.equal(sw.shown[0].opts.data.url, '#mon-alerts');
  assert.equal(sw.shown[0].opts.icon, '/ui/icon-192.png');
});

test('sw push: 아이콘 경로는 실제 파일이며 STATIC_ASSETS 에 프리캐시된다', () => {
  assert.ok(fs.existsSync(path.join(ROOT, 'ui/icon-192.png')), 'ui/icon-192.png must exist');
                                                    
                                            
  const assets = SW_SRC.slice(SW_SRC.indexOf('const STATIC_ASSETS'), SW_SRC.indexOf('];', SW_SRC.indexOf('const STATIC_ASSETS')));
  assert.ok(assets.includes("'/ui/icon-192.png'"), 'icon must be precached in STATIC_ASSETS');
});

test('sw push: 깨진 페이로드는 기본 제목으로 폴백한다', async () => {
  const sw = loadSw();
  await sw.dispatch('push', { data: { json: () => { throw new Error('not json'); } } });
  assert.equal(sw.shown.length, 1);
  assert.equal(sw.shown[0].title, 'PureCVisor');
  assert.equal(sw.shown[0].opts.body, '');
  assert.equal(sw.shown[0].opts.data.url, '#mon-alerts');
});

test('sw push: data 없는 이벤트도 알림을 띄운다', async () => {
  const sw = loadSw();
  await sw.dispatch('push', { data: null });
  assert.equal(sw.shown.length, 1);
  assert.equal(sw.shown[0].title, 'PureCVisor 알림');
});

test('sw notificationclick: 열린 창이 있으면 focus + pcv-nav postMessage', async () => {
  const focused = [];
  const posted = [];
  const client = { focus: () => { focused.push(true); return Promise.resolve(); }, postMessage: (m) => posted.push(m) };
  const sw = loadSw([client]);
  const closed = [];
  await sw.dispatch('notificationclick', {
    notification: { close: () => closed.push(true), data: { url: '#mon-alerts' } },
  });
  assert.deepEqual(closed, [true]);
  assert.deepEqual(focused, [true]);
                                                       
                           
  assert.equal(posted.length, 1);
  assert.equal(posted[0].type, 'pcv-nav');
  assert.equal(posted[0].tab, 'mon-alerts');
  assert.equal(sw.opened.length, 0);
  assert.equal(sw.matchAllArgs[0].type, 'window');
  assert.equal(sw.matchAllArgs[0].includeUncontrolled, true);
});

test('sw notificationclick: 열린 창이 없으면 openWindow 로 딥링크', async () => {
  const sw = loadSw([]);
  await sw.dispatch('notificationclick', {
    notification: { close: () => {}, data: { url: '#mon-alerts' } },
  });
  assert.deepEqual(sw.opened, ['/ui/#mon-alerts']);
});

test('sw notificationclick: data 없는 알림도 기본 라우트로 연다', async () => {
  const sw = loadSw([]);
  await sw.dispatch('notificationclick', { notification: { close: () => {} } });
  assert.deepEqual(sw.opened, ['/ui/#mon-alerts']);
});

                                                                

const PENDING_CACHE = 'pcv-push-pending';
const PENDING_URL = '/ui/__pcv_push_pending';

async function readPending(sw) {
  const store = sw.stores.get(PENDING_CACHE);
  if (!store) return null;
  const res = [...store.values()][0];
  return res ? JSON.parse(await res.text()) : null;
}

                                             
                                                                   
test('sw pushsubscriptionchange: 새 구독을 인수인계함에 적고 열린 창을 깨운다', async () => {
  const msgs = [];
  const sw = loadSw([{ focus: () => {}, postMessage: (m) => msgs.push(m) }]);
  await sw.dispatch('pushsubscriptionchange', {
    oldSubscription: { endpoint: 'https://push.example.com/old' },
    newSubscription: {
      endpoint: 'https://push.example.com/new',
      toJSON: () => ({ endpoint: 'https://push.example.com/new', keys: { p256dh: 'p', auth: 'a' } }),
    },
  });
  const rec = await readPending(sw);
  assert.equal(rec.old, 'https://push.example.com/old');
  assert.equal(rec.sub.endpoint, 'https://push.example.com/new');
  assert.deepEqual(rec.sub.keys, { p256dh: 'p', auth: 'a' });
  assert.equal(sw.swSubscribeArgs.length, 0);                     
                                                            
  assert.equal(msgs.length, 1);
  assert.equal(msgs[0].type, 'pcv-push-changed');
});

                                                             
                                                  
                                             
test('sw pushsubscriptionchange: newSubscription 이 없으면 옛 키로 재구독한다', async () => {
  const sw = loadSw([], {
    swSubscribe: {
      endpoint: 'https://push.example.com/re',
      toJSON: () => ({ endpoint: 'https://push.example.com/re', keys: { p256dh: 'p2', auth: 'a2' } }),
    },
  });
  await sw.dispatch('pushsubscriptionchange', {
    oldSubscription: {
      endpoint: 'https://push.example.com/old',
      options: { applicationServerKey: new Uint8Array([1, 2, 3]) },
    },
  });
  assert.equal(sw.swSubscribeArgs.length, 1);
  assert.equal(sw.swSubscribeArgs[0].userVisibleOnly, true);
  const rec = await readPending(sw);
  assert.equal(rec.sub.endpoint, 'https://push.example.com/re');
  assert.equal(rec.old, 'https://push.example.com/old');
});

                                                           
test('sw pushsubscriptionchange: 적을 것이 없으면 인수인계함을 만들지 않는다', async () => {
  const sw = loadSw([]);
  await sw.dispatch('pushsubscriptionchange', {});
  assert.equal(sw.stores.has(PENDING_CACHE), false);
});

                                                             
                                                         
                                                                    
test('sw activate: 인수인계함은 배포 캐시 청소에서 살아남는다', async () => {
  const sw = loadSw([]);
  await sw.dispatch('pushsubscriptionchange', {
    oldSubscription: { endpoint: 'https://push.example.com/old' },
    newSubscription: {
      endpoint: 'https://push.example.com/new',
      toJSON: () => ({ endpoint: 'https://push.example.com/new', keys: {} }),
    },
  });
  await sw.dispatch('activate', {});
  assert.equal(sw.deleted.includes(PENDING_CACHE), false);
  assert.notEqual(await readPending(sw), null);
});

                                                              
const MODS = [
  'ui/modules/endpoints.js',
  'ui/modules/api.js',
  'ui/modules/ui.js',
  'ui/modules/uxlib.js',
  'ui/modules/push.js',
];

                                                     
const VAPID_B64 = 'AAECAwQFBgcICQoLDA0ODw';
const VAPID_BYTES = Array.from({ length: 16 }, (_, i) => i);

const SUB_JSON = {
  endpoint: 'https://push.example.com/ep/abc',
  keys: { p256dh: 'p256dh-value', auth: 'auth-value' },
};

                                 
                              
                                                                 
                                                                     
                                                      
function installMocks(page, { hasSub = false, permission = 'granted', subKey = null,
                              registration = 'active' } = {}) {
  return page.evaluate((sub, hasSubscription, perm, subKeyBytes, regMode) => {
    window.__pushCalls = { subscribe: [], unsubscribe: [], permission: 0 };
    const subscription = {
      endpoint: sub.endpoint,
      options: { applicationServerKey: subKeyBytes ? new Uint8Array(subKeyBytes) : null },
      toJSON: () => JSON.parse(JSON.stringify(sub)),
      unsubscribe: () => { window.__pushCalls.unsubscribe.push(sub.endpoint); return Promise.resolve(true); },
    };
    let current = hasSubscription ? subscription : null;
    const reg = {
      active: regMode === 'active' ? {} : null,
      pushManager: {
        getSubscription: () => Promise.resolve(current),
        subscribe: (opts) => {
          window.__pushCalls.subscribe.push({
            userVisibleOnly: opts.userVisibleOnly,
            key: Array.from(opts.applicationServerKey),
          });
          subscription.options.applicationServerKey = opts.applicationServerKey;
          current = subscription;
          return Promise.resolve(subscription);
        },
      },
    };
    Object.defineProperty(navigator.serviceWorker, 'ready', {
                                                                    
      value: regMode === 'installing' ? new Promise(() => {}) : Promise.resolve(reg),
      configurable: true, writable: true,
    });
    navigator.serviceWorker.getRegistration = () =>
      Promise.resolve(regMode === 'none' ? undefined : reg);
    window.Notification = function () {};
    window.Notification.permission = perm;
    window.Notification.requestPermission = () => {
      window.__pushCalls.permission++;
      return Promise.resolve(perm);
    };
  }, SUB_JSON, hasSub, permission, subKey, registration);
}

                                                                     
                                                           
                                                  
const SUB_DIGEST = createHash('sha256').update(SUB_JSON.endpoint).digest('hex');

                                                          
const ROUTES_OK = {
  '/api/v1/push/vapid': { body: { data: { key: VAPID_B64 } } },
  '/api/v1/push/subscribe': { body: { data: { status: 'subscribed' } } },
  '/api/v1/push/unsubscribe': { body: { data: { status: 'unsubscribed' } } },
  '/api/v1/push/mine': { body: { data: { subscriptions: [], count: 0 } } },
};

                                 
const ROUTES_REGISTERED = {
  ...ROUTES_OK,
  '/api/v1/push/mine': {
    body: {
      data: {
        subscriptions: [{
          endpoint: SUB_JSON.endpoint.slice(0, 80), digest: SUB_DIGEST,
          created_at: 1, last_ok_at: 0, fail_count: 0,
        }],
        count: 1,
      },
    },
  },
};

test('PCV.push.supported(): secure context 기본값은 true', async () => {
  await withPage(MODS, async (page) => {
    const r = await page.evaluate(() => ({
      supported: window.PCV.push.supported(),
      api: ['supported', 'status', 'enable', 'disable'].map(k => typeof window.PCV.push[k]),
      ep: [window.EP.PUSH_VAPID(), window.EP.PUSH_SUBSCRIBE(), window.EP.PUSH_UNSUBSCRIBE(),
           window.EP.PUSH_MINE()],
    }));
    assert.equal(r.supported, true);
    assert.deepEqual(r.api, ['function', 'function', 'function', 'function']);
    assert.deepEqual(r.ep, ['/api/v1/push/vapid', '/api/v1/push/subscribe',
                            '/api/v1/push/unsubscribe', '/api/v1/push/mine']);
  });
});

test('PCV.push.supported(): PushManager 부재 브라우저는 false', async () => {
  await withPage(MODS, async (page) => {
    await page.evaluateOnNewDocument(() => { delete window.PushManager; });
    await page.reload({ waitUntil: 'load' });
    const r = await page.evaluate(async () => ({
      supported: window.PCV.push.supported(),
      status: await window.PCV.push.status(),
      enable: await window.PCV.push.enable(),
    }));
    assert.equal(r.supported, false);
    assert.equal(r.status.subscribed, false);
    assert.equal(r.status.perm, 'unsupported');
    assert.equal(r.enable.ok, false);
    assert.ok(r.enable.error.includes('웹 푸시'), r.enable.error);
  });
});

test('PCV.push.enable(): VAPID 키 조회 → subscribe → 서버 등록', async () => {
  await withPage(MODS, async (page, { requests }) => {
    await installMocks(page);
    const r = await page.evaluate(async () => {
      const res = await window.PCV.push.enable();
      return { res, calls: window.__pushCalls, status: await window.PCV.push.status() };
    });
    assert.deepEqual(r.res, { ok: true });
    assert.equal(r.calls.permission, 1);
    assert.equal(r.calls.subscribe.length, 1);
    assert.equal(r.calls.subscribe[0].userVisibleOnly, true);
    assert.deepEqual(r.calls.subscribe[0].key, VAPID_BYTES);
    assert.deepEqual(r.status, { perm: 'granted', subscribed: true, swBlocked: false });

    const post = requests.find(q => q.path === '/api/v1/push/subscribe');
    assert.ok(post, 'subscribe must be POSTed');
    assert.equal(post.method, 'POST');
    assert.deepEqual(post.json, SUB_JSON);
    assert.ok(requests.some(q => q.path === '/api/v1/push/vapid' && q.method === 'GET'));
  }, { routes: ROUTES_OK });
});

test('PCV.push.enable(): 서버 거부(-32602) 사유를 그대로 돌려주고 로컬 구독을 되돌린다', async () => {
  const routes = {
    ...ROUTES_OK,
    '/api/v1/push/subscribe': {
      status: 400,
      body: { error: { code: -32602, message: '허용되지 않는 push endpoint 입니다 (사설 대역)' } },
    },
  };
  await withPage(MODS, async (page) => {
    await installMocks(page);
    const r = await page.evaluate(async () => {
      const res = await window.PCV.push.enable();
      return { res, calls: window.__pushCalls };
    });
    assert.equal(r.res.ok, false);
    assert.equal(r.res.error, '허용되지 않는 push endpoint 입니다 (사설 대역)');
                                     
    assert.deepEqual(r.calls.unsubscribe, [SUB_JSON.endpoint]);
  }, { routes });
});

test('PCV.push.enable(): 권한 거부 시 네트워크 호출 없이 실패한다', async () => {
  await withPage(MODS, async (page, { requests }) => {
    await installMocks(page, { permission: 'denied' });
    const r = await page.evaluate(async () => {
      const res = await window.PCV.push.enable();
      return { res, calls: window.__pushCalls };
    });
    assert.equal(r.res.ok, false);
    assert.ok(r.res.error.length > 0);
    assert.equal(r.calls.subscribe.length, 0);
    assert.equal(requests.filter(q => q.path.startsWith('/api/v1/push')).length, 0);
  }, { routes: ROUTES_OK });
});

test('PCV.push.disable(): 로컬 구독 해지 + 서버 해지 요청', async () => {
  await withPage(MODS, async (page, { requests }) => {
    await installMocks(page, { hasSub: true });
    const r = await page.evaluate(async () => {
      const before = await window.PCV.push.status();
      const res = await window.PCV.push.disable();
      return { before, res, calls: window.__pushCalls };
    });
    assert.deepEqual(r.before, { perm: 'granted', subscribed: true, swBlocked: false });
    assert.deepEqual(r.res, { ok: true });
    assert.deepEqual(r.calls.unsubscribe, [SUB_JSON.endpoint]);
    const post = requests.find(q => q.path === '/api/v1/push/unsubscribe');
    assert.ok(post, 'unsubscribe must be POSTed');
    assert.deepEqual(post.json, { endpoint: SUB_JSON.endpoint });
  }, { routes: ROUTES_OK });
});

test('PCV.push.disable(): 구독이 없으면 서버를 호출하지 않는다', async () => {
  await withPage(MODS, async (page, { requests }) => {
    await installMocks(page, { hasSub: false });
    const res = await page.evaluate(() => window.PCV.push.disable());
    assert.deepEqual(res, { ok: true });
    assert.equal(requests.filter(q => q.path === '/api/v1/push/unsubscribe').length, 0);
  }, { routes: ROUTES_OK });
});

                                                   
                                              
function waitStatus(page, id, needle) {
  return page.waitForFunction(
    (elId, text) => {
      const el = document.getElementById(elId);
      return !!el && el.textContent.includes(text);
    },
    { timeout: 5000 }, id + '-status', needle
  );
}

                                                          
                                                               
async function waitRequests(requests, path, n, timeout = 5000) {
  const deadline = Date.now() + timeout;
  const count = () => requests.filter(q => q.path === path).length;
  while (count() < n && Date.now() < deadline) {
    await new Promise(res => setTimeout(res, 20));
  }
  await new Promise(res => setTimeout(res, 150));                         
  return count();
}

test('PCV.push.enable(): SW 미등록이면 즉시 실패하고 토글이 원상 복구된다', async () => {
  await withPage(MODS, async (page) => {
    await installMocks(page, { registration: 'none' });
    await page.evaluate(async () => {
      document.getElementById('cb').appendChild(window.PCV.push.toggleNode({ id: 'push-toggle' }));
      await new Promise(res => setTimeout(res, 30));
      document.getElementById('push-toggle').click();
    });
    await waitStatus(page, 'push-toggle', '서비스 워커');
    const r = await page.evaluate(() => {
      const btn = document.getElementById('push-toggle');
      return {
        disabled: btn.disabled,
        label: btn.textContent,
        status: document.getElementById('push-toggle-status').textContent,
        calls: window.__pushCalls,
      };
    });
                                                          
    assert.equal(r.disabled, false);
    assert.ok(r.status.includes('서비스 워커'), r.status);
    assert.equal(r.calls.subscribe.length, 0);
    assert.ok(r.label.includes('구독'), r.label);
  }, { routes: ROUTES_OK });
});

test('PCV.push.enable(): SW 활성화가 끝나지 않으면 시간 초과로 실패한다', async () => {
  await withPage(MODS, async (page) => {
    await installMocks(page, { registration: 'installing' });
    await page.evaluate(async () => {
      window.PCV.push.READY_TIMEOUT_MS = 60;                
      document.getElementById('cb').appendChild(window.PCV.push.toggleNode({ id: 'push-toggle' }));
      await new Promise(res => setTimeout(res, 30));
      document.getElementById('push-toggle').click();
    });
                                          
    await waitStatus(page, 'push-toggle', '시간 초과');
    const r = await page.evaluate(() => ({
      disabled: document.getElementById('push-toggle').disabled,
      status: document.getElementById('push-toggle-status').textContent,
      calls: window.__pushCalls,
    }));
    assert.equal(r.disabled, false);
    assert.ok(r.status.includes('시간 초과'), r.status);
    assert.equal(r.calls.subscribe.length, 0);
  }, { routes: ROUTES_OK });
});

                                                                   
                                               
                                                  
                                                       
                                          
                                                      
                               

test('PCV.push.enable(): 인증서 신뢰 문제로 SW 등록이 막히면 사유를 안내한다(새로고침 문구 없음)', async () => {
  await withPage(MODS, async (page) => {
    await installMocks(page, { registration: 'none' });
    await page.evaluate(() => {
      window._swRegError = {
        name: 'SecurityError',
        message: 'Failed to register a ServiceWorker: An SSL certificate error occurred',
      };
    });
    const r = await page.evaluate(() => window.PCV.push.enable());
    assert.equal(r.ok, false);
    assert.ok(r.error.includes('인증서'), r.error);
    assert.ok(!r.error.includes('새로고침'), r.error);
  }, { routes: ROUTES_OK });
});

test('PCV.push.enable(): 기타 SW 등록 실패는 기록된 사유를 그대로 보여준다', async () => {
  await withPage(MODS, async (page) => {
    await installMocks(page, { registration: 'none' });
    await page.evaluate(() => {
      window._swRegError = { name: 'TypeError', message: 'Failed to fetch' };
    });
    const r = await page.evaluate(() => window.PCV.push.enable());
    assert.equal(r.ok, false);
    assert.ok(r.error.includes('Failed to fetch'), r.error);
  }, { routes: ROUTES_OK });
});

test('PCV.push.status(): swBlocked 는 SW 등록 실패 기록 유무를 반영한다', async () => {
  await withPage(MODS, async (page) => {
    await installMocks(page);
    const blocked = await page.evaluate(() => {
      window._swRegError = { name: 'SecurityError', message: 'x' };
      return window.PCV.push.status();
    });
    assert.equal(blocked.swBlocked, true);
    const cleared = await page.evaluate(() => {
      window._swRegError = null;
      return window.PCV.push.status();
    });
    assert.equal(cleared.swBlocked, false);
  }, { routes: ROUTES_OK });
});

test('toggleNode: SW 등록이 차단된 상태면 누르기 전부터 disabled + 사유가 보인다', async () => {
  await withPage(MODS, async (page) => {
    await installMocks(page, { hasSub: false });
    await page.evaluate(() => {
      window._swRegError = { name: 'SecurityError', message: 'x' };
      document.getElementById('cb').appendChild(window.PCV.push.toggleNode({ id: 'push-toggle' }));
    });
    await waitStatus(page, 'push-toggle', '인증서');
    const r = await page.evaluate(() => ({
      disabled: document.getElementById('push-toggle').disabled,
      status: document.getElementById('push-toggle-status').textContent,
    }));
    assert.equal(r.disabled, true);
    assert.ok(r.status.includes('인증서'), r.status);
  }, { routes: ROUTES_OK });
});

test('PCV.push.resync(): 키가 같고 서버 장부에 없으면 조회 후 등록한다', async () => {
  await withPage(MODS, async (page, { requests }) => {
    await installMocks(page, { hasSub: true, subKey: VAPID_BYTES });
    const r = await page.evaluate(async () => {
      const res = await window.PCV.push.resync();
      return { res, calls: window.__pushCalls };
    });
    assert.deepEqual(r.res, { ok: true });
    assert.equal(r.calls.subscribe.length, 0);                      
    assert.equal(r.calls.unsubscribe.length, 0);
                                 
    assert.equal(requests.filter(q => q.path === '/api/v1/push/mine').length, 1);
    const posts = requests.filter(q => q.path === '/api/v1/push/subscribe');
    assert.equal(posts.length, 1);
    assert.deepEqual(posts[0].json, SUB_JSON);
  }, { routes: ROUTES_OK });
});

                                                          
                                                                        
test('PCV.push.resync(): 서버 장부에 이미 있으면 쓰기 없이 끝난다', async () => {
  await withPage(MODS, async (page, { requests }) => {
    await installMocks(page, { hasSub: true, subKey: VAPID_BYTES });
    const res = await page.evaluate(() => window.PCV.push.resync());
    assert.deepEqual(res, { ok: true });
    assert.equal(requests.filter(q => q.path === '/api/v1/push/mine').length, 1);
    assert.equal(requests.filter(q => q.path === '/api/v1/push/subscribe').length, 0);
  }, { routes: ROUTES_REGISTERED });
});

                                                  
                                              
                                                           
                                          
test('PCV.push.resync(): push.mine 이 없는 서버에서는 upsert 로 폴백한다', async () => {
  const routes = {
    ...ROUTES_OK,
    '/api/v1/push/mine': { status: 404, body: { error: { code: -32601, message: 'not found' } } },
  };
  await withPage(MODS, async (page, { requests }) => {
    await installMocks(page, { hasSub: true, subKey: VAPID_BYTES });
    const res = await page.evaluate(() => window.PCV.push.resync());
    assert.deepEqual(res, { ok: true });
    assert.equal(requests.filter(q => q.path === '/api/v1/push/subscribe').length, 1);
  }, { routes });
});

                                                   
                                               
                          
                                                                    
test('PCV.push.resync(): SW 인수인계 기록을 신규 등록 → 옛 해지 순으로 반영한다', async () => {
  await withPage(MODS, async (page, { requests }) => {
    await installMocks(page, { hasSub: true, subKey: VAPID_BYTES });
    await page.evaluate(async (cacheName, url) => {
      const c = await caches.open(cacheName);
      await c.put(new Request(url), new Response(JSON.stringify({
        old: 'https://push.example.com/ep/OLD',
        sub: { endpoint: 'https://push.example.com/ep/NEW', keys: { p256dh: 'pN', auth: 'aN' } },
        ts: 1,
      }), { headers: { 'content-type': 'application/json' } }));
    }, PENDING_CACHE, PENDING_URL);

    await page.evaluate(() => window.PCV.push.resync());

    const paths = requests.filter(q => q.path.startsWith('/api/v1/push')).map(q => q.path);
    const subs = requests.filter(q => q.path === '/api/v1/push/subscribe');
    const unsub = requests.filter(q => q.path === '/api/v1/push/unsubscribe');
    assert.equal(subs[0].json.endpoint, 'https://push.example.com/ep/NEW');
    assert.deepEqual(subs[0].json.keys, { p256dh: 'pN', auth: 'aN' });
    assert.equal(unsub.length, 1);
    assert.equal(unsub[0].json.endpoint, 'https://push.example.com/ep/OLD');
                           
    assert.ok(paths.indexOf('/api/v1/push/subscribe') < paths.indexOf('/api/v1/push/unsubscribe'),
      paths.join(','));

                                                    
    const left = await page.evaluate(async (cacheName, url) => {
      const c = await caches.open(cacheName);
      return !!(await c.match(url));
    }, PENDING_CACHE, PENDING_URL);
    assert.equal(left, false);
  }, { routes: ROUTES_REGISTERED });
});

                                                    
test('PCV.push.resync(): 인수인계 신규 등록 실패 시 옛 해지를 하지 않고 기록을 남긴다', async () => {
  const routes = {
    ...ROUTES_REGISTERED,
    '/api/v1/push/subscribe': { status: 500, body: { error: { code: -32603, message: '일시 장애' } } },
  };
  await withPage(MODS, async (page, { requests }) => {
    await installMocks(page, { hasSub: true, subKey: VAPID_BYTES });
    await page.evaluate(async (cacheName, url) => {
      const c = await caches.open(cacheName);
      await c.put(new Request(url), new Response(JSON.stringify({
        old: 'https://push.example.com/ep/OLD',
        sub: { endpoint: 'https://push.example.com/ep/NEW', keys: { p256dh: 'pN', auth: 'aN' } },
        ts: 1,
      }), { headers: { 'content-type': 'application/json' } }));
    }, PENDING_CACHE, PENDING_URL);

    await page.evaluate(() => window.PCV.push.resync());

    assert.equal(requests.filter(q => q.path === '/api/v1/push/unsubscribe').length, 0);
    const left = await page.evaluate(async (cacheName, url) => {
      const c = await caches.open(cacheName);
      return !!(await c.match(url));
    }, PENDING_CACHE, PENDING_URL);
    assert.equal(left, true);
  }, { routes });
});

test('PCV.push.resync(): 구독이 없으면 아무 요청도 하지 않는다', async () => {
  await withPage(MODS, async (page, { requests }) => {
    await installMocks(page, { hasSub: false });
    const res = await page.evaluate(() => window.PCV.push.resync());
    assert.deepEqual(res, { ok: true });
    assert.equal(requests.filter(q => q.path.startsWith('/api/v1/push')).length, 0);
  }, { routes: ROUTES_OK });
});

test('toggleNode: 구독 중이면 생성 시 서버와 1회 자가치유(rotate 후 재구독)', async () => {
  await withPage(MODS, async (page, { requests }) => {
                                                      
    await installMocks(page, { hasSub: true, subKey: [9, 9, 9] });
    await page.evaluate(() => {
      document.getElementById('cb').appendChild(window.PCV.push.toggleNode({ id: 'push-toggle' }));
    });
                                                       
    await page.waitForFunction(() => window.__pushCalls.subscribe.length === 1, { timeout: 5000 });
    const r = await page.evaluate(() => ({
      calls: window.__pushCalls,
      sub: document.getElementById('push-toggle').dataset.subscribed,
      status: document.getElementById('push-toggle-status').textContent,
    }));
    assert.deepEqual(r.calls.unsubscribe, [SUB_JSON.endpoint]);                  
    assert.equal(r.calls.subscribe.length, 1);
    assert.deepEqual(r.calls.subscribe[0].key, VAPID_BYTES);                    
    assert.equal(r.sub, '1');
    assert.ok(r.status.includes('알림을 받습니다'), r.status);
    assert.equal(requests.filter(q => q.path === '/api/v1/push/subscribe').length, 1);
  }, { routes: ROUTES_OK });
});

test('toggleNode: 자가치유는 주기당 1회 — 노드를 여러 번 만들어도 요청이 늘지 않는다', async () => {
  await withPage(MODS, async (page, { requests }) => {
    await installMocks(page, { hasSub: true, subKey: VAPID_BYTES });
    await page.evaluate(async () => {
      const host = document.getElementById('cb');
      for (let i = 0; i < 3; i++) {
        host.replaceChildren(window.PCV.push.toggleNode({ id: 'push-toggle' }));
        await new Promise(res => setTimeout(res, 80));
      }
    });
    assert.equal(await waitRequests(requests, '/api/v1/push/subscribe', 1), 1);
    assert.equal(requests.filter(q => q.path === '/api/v1/push/vapid').length, 1);
  }, { routes: ROUTES_OK });
});

                                                
                                                        
                                        
                                                    
                
test('toggleNode: RESYNC_INTERVAL_MS 가 지나면 다시 수렴한다', async () => {
  await withPage(MODS, async (page, { requests }) => {
    await installMocks(page, { hasSub: true, subKey: VAPID_BYTES });
    await page.evaluate(async () => {
      window.PCV.push.RESYNC_INTERVAL_MS = 120;                
      const host = document.getElementById('cb');
      host.replaceChildren(window.PCV.push.toggleNode({ id: 'push-toggle' }));
      await new Promise(res => setTimeout(res, 300));              
      host.replaceChildren(window.PCV.push.toggleNode({ id: 'push-toggle' }));
      await new Promise(res => setTimeout(res, 200));
    });
    assert.equal(await waitRequests(requests, '/api/v1/push/mine', 2), 2);
  }, { routes: ROUTES_REGISTERED });
});

test('mobile buildAlerts: 10초 폴링 재렌더가 반복돼도 자가치유는 1회만', async () => {
                                                                              
                                                              
                                  
  const MODS_MOBILE = [
    'ui/modules/endpoints.js', 'ui/modules/api.js', 'ui/modules/ui.js',
    'ui/modules/uxlib.js', 'ui/modules/filter-state.js', 'ui/modules/push.js',
    'ui/modules/mobile.js',
  ];
  await withPage(MODS_MOBILE, async (page, { requests }) => {
    await installMocks(page, { hasSub: true, subKey: VAPID_BYTES });
    await page.evaluate(async () => {
      const host = document.getElementById('cb');
      for (let i = 0; i < 4; i++) {
        host.replaceChildren(window.PCV.mobile.buildAlerts({ alerts: [], filter: [] }));
        await new Promise(res => setTimeout(res, 80));
      }
    });
    await waitRequests(requests, '/api/v1/push/subscribe', 1);
    const push = requests.filter(q => q.path.startsWith('/api/v1/push'));
    assert.deepEqual(
      push.map(q => q.path),
      ['/api/v1/push/vapid', '/api/v1/push/mine', '/api/v1/push/subscribe'],
      '재렌더 4회에도 자가치유 왕복은 1세트여야 한다'
    );
  }, { routes: ROUTES_OK });
});

test('toggleNode: 자가치유가 실패하면 다음 렌더에서 재시도된다(영구 차단 없음)', async () => {
  const routes = {
    ...ROUTES_OK,
    '/api/v1/push/subscribe': (record, all) => {
      const nth = all.filter(q => q.path === '/api/v1/push/subscribe').length;
      return nth === 1
        ? { status: 500, body: { error: { code: -32603, message: '일시 장애' } } }
        : { body: { data: { status: 'subscribed' } } };
    },
  };
  await withPage(MODS, async (page, { requests }) => {
    await installMocks(page, { hasSub: true, subKey: VAPID_BYTES });
    await page.evaluate(() => {
      document.getElementById('cb').replaceChildren(window.PCV.push.toggleNode({ id: 'push-toggle' }));
    });
    await waitStatus(page, 'push-toggle', '일시 장애');

    await page.evaluate(async () => {
      const host = document.getElementById('cb');
      for (let i = 0; i < 2; i++) {
        host.replaceChildren(window.PCV.push.toggleNode({ id: 'push-toggle' }));
        await new Promise(res => setTimeout(res, 100));
      }
    });
                                                
    assert.equal(await waitRequests(requests, '/api/v1/push/subscribe', 2), 2);
  }, { routes });
});

test('toggleNode: 미구독이면 생성 시 서버를 호출하지 않는다', async () => {
  await withPage(MODS, async (page, { requests }) => {
    await installMocks(page, { hasSub: false });
    await page.evaluate(async () => {
      document.getElementById('cb').appendChild(window.PCV.push.toggleNode({ id: 'push-toggle' }));
      await new Promise(res => setTimeout(res, 100));
    });
    assert.equal(requests.filter(q => q.path.startsWith('/api/v1/push')).length, 0);
  }, { routes: ROUTES_OK });
});

test('toggleNode: 자가치유 실패 사유는 상태줄에 남는다', async () => {
  const routes = {
    ...ROUTES_OK,
    '/api/v1/push/subscribe': {
      status: 400,
      body: { error: { code: -32602, message: '허용되지 않는 push endpoint 입니다 (사설 대역)' } },
    },
  };
  await withPage(MODS, async (page) => {
    await installMocks(page, { hasSub: true, subKey: VAPID_BYTES });
    await page.evaluate(() => {
      document.getElementById('cb').appendChild(window.PCV.push.toggleNode({ id: 'push-toggle' }));
    });
    await waitStatus(page, 'push-toggle', '사설 대역');
  }, { routes });
});

test('PCV.push: SW 의 pcv-nav 메시지는 navigateTo 로 라우팅된다', async () => {
  await withPage(MODS, async (page) => {
    const tab = await page.evaluate(async () => {
      window.__nav = [];
      window.navigateTo = (t) => window.__nav.push(t);
      navigator.serviceWorker.dispatchEvent(new MessageEvent('message', {
        data: { type: 'pcv-nav', tab: 'mon-alerts' },
      }));
      await new Promise(r => setTimeout(r, 0));
      return window.__nav;
    });
    assert.deepEqual(tab, ['mon-alerts']);
  });
});

test('PCV.push: 모바일 셸이 활성이면 pcv-nav 는 모바일 알림 탭으로 분기한다', async () => {
  const MODS_M = [
    'ui/modules/endpoints.js', 'ui/modules/api.js', 'ui/modules/ui.js',
    'ui/modules/uxlib.js', 'ui/modules/filter-state.js', 'ui/modules/push.js',
    'ui/modules/mobile.js',
  ];
  await withPage(MODS_M, async (page) => {
    const r = await page.evaluate(async () => {
      window.__nav = [];
      window.navigateTo = (t) => window.__nav.push(t);
      document.body.classList.add('mshell');                 
      navigator.serviceWorker.dispatchEvent(new MessageEvent('message', {
        data: { type: 'pcv-nav', tab: 'mon-alerts' },
      }));
      await new Promise(res => setTimeout(res, 0));
      return { nav: window.__nav, mtab: window.PCV.mobile.activeTab };
    });
    assert.equal(r.mtab, 'alerts');
                                                                   
    assert.deepEqual(r.nav, []);
  });
});

                                                                
test('PCV.push.toggleNode(): 미지원 브라우저는 disabled + 사유', async () => {
  await withPage(MODS, async (page) => {
    await page.evaluateOnNewDocument(() => { delete window.PushManager; });
    await page.reload({ waitUntil: 'load' });
    const r = await page.evaluate(() => {
      const node = window.PCV.push.toggleNode({ id: 'push-toggle' });
      document.getElementById('cb').appendChild(node);
      const btn = document.getElementById('push-toggle');
      return { disabled: btn.disabled, text: node.textContent };
    });
    assert.equal(r.disabled, true);
    assert.ok(r.text.includes('웹 푸시'), r.text);
  });
});

test('PCV.push.toggleNode(): 구독 상태에 따라 라벨이 바뀐다', async () => {
  await withPage(MODS, async (page) => {
    await installMocks(page, { hasSub: true });
    await page.evaluate(() => {
      document.getElementById('cb').appendChild(window.PCV.push.toggleNode({ id: 'push-toggle' }));
    });
    await page.waitForFunction(
      () => document.getElementById('push-toggle').dataset.subscribed === '1',
      { timeout: 5000 }
    );
    const label = await page.evaluate(() => {
      const btn = document.getElementById('push-toggle');
      return { text: btn.textContent, sub: btn.dataset.subscribed, disabled: btn.disabled };
    });
    assert.equal(label.disabled, false);
    assert.equal(label.sub, '1');
    assert.ok(label.text.includes('해제'), label.text);
  }, { routes: ROUTES_OK });
});

                                                            
const MODS_FULL = [
  'ui/i18n.js',
  'ui/modules/endpoints.js',
  'ui/modules/api.js',
  'ui/modules/ui.js',
  'ui/modules/filter-state.js',
  'ui/modules/uxlib.js',
  'ui/modules/modal-core.js',
  'ui/modules/modal.js',
  'ui/modules/charts.js',
  'ui/modules/shell.js',
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
  'ui/modules/push.js',
  'ui/modules/mobile.js',
  'ui/app.js',
];

async function bootApp(page, port) {
  await page.evaluate(async () => {
    const html = await fetch('/ui/index.html').then(r => r.text());
    const parsed = new DOMParser().parseFromString(html, 'text/html');
    document.body.innerHTML = parsed.body.innerHTML;
  });
  for (const moduleFile of MODS_FULL) {
    await page.addScriptTag({ url: `http://127.0.0.1:${port}/${moduleFile}` });
  }
}

test('showPrefs: 알림 소섹션에 #push-toggle 이 붙는다', async () => {
  await withPage([], async (page, { port }) => {
    const pageErrors = [];
    page.on('pageerror', e => pageErrors.push(String(e)));
    await bootApp(page, port);
    await installMocks(page, { hasSub: false });
    await page.evaluate(() => window.showPrefs());
    await page.waitForFunction(
      () => document.getElementById('push-toggle')?.dataset.subscribed === '0',
      { timeout: 5000 }
    );
    const r = await page.evaluate(() => {
      const body = window.PCV.modalCore.currentBody();
      const btn = body.querySelector('#push-toggle');
      return {
        found: !!btn,
        disabled: btn ? btn.disabled : null,
        sub: btn ? btn.dataset.subscribed : null,
        text: body.textContent,
      };
    });
    assert.deepEqual(pageErrors, []);
    assert.equal(r.found, true);
    assert.equal(r.disabled, false);
    assert.equal(r.sub, '0');
    assert.ok(r.text.includes('알림'), r.text.slice(0, 200));
  }, { routes: { '/api/v1/health': { body: { data: { status: 'ok' } } }, ...ROUTES_OK } });
});

test('showPrefs: 미지원 브라우저에서는 토글이 disabled + 사유', async () => {
  await withPage([], async (page, { port }) => {
    await page.evaluateOnNewDocument(() => { delete window.PushManager; });
    await page.reload({ waitUntil: 'load' });
    await bootApp(page, port);
    const r = await page.evaluate(() => {
      window.showPrefs();
      const body = window.PCV.modalCore.currentBody();
      const btn = body.querySelector('#push-toggle');
      return { found: !!btn, disabled: btn ? btn.disabled : null, text: body.textContent };
    });
    assert.equal(r.found, true);
    assert.equal(r.disabled, true);
    assert.ok(r.text.includes('웹 푸시'), r.text.slice(0, 300));
  }, { routes: { '/api/v1/health': { body: { data: { status: 'ok' } } } } });
});

                                                         
test('mobile buildAlerts: filterbar 위에 푸시 토글 카드가 온다', async () => {
  const MODS_MOBILE = [
    'ui/modules/endpoints.js',
    'ui/modules/api.js',
    'ui/modules/ui.js',
    'ui/modules/uxlib.js',
    'ui/modules/filter-state.js',
    'ui/modules/push.js',
    'ui/modules/mobile.js',
  ];
  await withPage(MODS_MOBILE, async (page) => {
    await installMocks(page, { hasSub: false });
    const r = await page.evaluate(() => {
      const node = window.PCV.mobile.buildAlerts({ alerts: [], filter: [] });
      document.getElementById('cb').appendChild(node);
      const kids = Array.from(node.children);
      return {
        pushIdx: kids.findIndex(k => k.classList.contains('m-push')),
        barIdx: kids.findIndex(k => k.classList.contains('filterbar')),
        btn: !!node.querySelector('#m-push-toggle'),
        listcard: !!node.querySelector('.m-push.m-listcard'),
      };
    });
    assert.equal(r.btn, true);
    assert.equal(r.listcard, true);
    assert.ok(r.pushIdx >= 0 && r.barIdx >= 0);
    assert.ok(r.pushIdx < r.barIdx, `push card(${r.pushIdx}) must precede filterbar(${r.barIdx})`);
  }, { routes: ROUTES_OK });
});
