/* ============================================================
   PureCVisor — Service Worker (F-8c)
   전략: Network-First (항상 최신 파일 우선, 오프라인 시 캐시 폴백)
   v14: app.bundle.js 단일 번들 + 오프라인 fallback
   ============================================================ */

const CACHE_NAME = 'pcv-ui-v5ce36745';
const OFFLINE_URL = '/ui/offline.html';

const STATIC_ASSETS = [
  '/ui/',
  '/ui/index.html',
  '/ui/style.css',
  '/ui/app.bundle.js',
  '/ui/i18n.js',
  '/ui/vendor/chart.umd.min.js',
  '/ui/vendor/novnc/novnc.esm.js',
  '/ui/vendor/pretendard/pretendard.css',
  '/ui/vendor/coolicons/coolicons.svg',
  '/ui/manifest.json',
  '/ui/icon-192.png',
  '/ui/icon-512.png',
];

/* Install: pre-cache + 즉시 활성화 */
self.addEventListener('install', event => {
  event.waitUntil(
    caches.open(CACHE_NAME).then(cache =>
      Promise.all(STATIC_ASSETS.map(url =>
        cache.add(new Request(url, { cache: 'reload' })).catch(err => {
          console.warn('[SW] precache miss:', url, err.message);
        })
      ))
    )
  );
  self.skipWaiting();
});

/* Activate: 구버전 캐시 전부 삭제 + 즉시 제어 */
self.addEventListener('activate', event => {
  event.waitUntil(
    caches.keys().then(keys =>
      Promise.all(keys.filter(k => k !== CACHE_NAME).map(k => caches.delete(k)))
    ).then(() => self.clients.claim())
  );
});

/* Message: SKIP_WAITING / CLEAR_CACHE */
self.addEventListener('message', event => {
  if (event.data && event.data.type === 'SKIP_WAITING') {
    self.skipWaiting();
  }
  if (event.data && event.data.type === 'CLEAR_CACHE') {
    caches.keys().then(keys => Promise.all(keys.map(k => caches.delete(k))))
      .then(() => event.ports[0] && event.ports[0].postMessage({ ok: true }));
  }
});

/* Fetch: Network-First with offline fallback */
self.addEventListener('fetch', event => {
  const url = new URL(event.request.url);
  const req = event.request;

  /* API / WebSocket: SW 개입 안 함 (실시간 데이터) */
  if (url.pathname.startsWith('/api/')) return;
  if (req.url.startsWith('ws://') || req.url.startsWith('wss://')) return;

  /* HTML navigation: network-first + offline fallback */
  const acc = req.headers.get('accept') || '';
  if (req.mode === 'navigate' || (req.method === 'GET' && acc.indexOf('text/html') !== -1)) {
    event.respondWith(
      fetch(req).then(r => {
        if (r.ok) {
          const clone = r.clone();
          caches.open(CACHE_NAME).then(c => c.put(req, clone));
        }
        return r;
      }).catch(() =>
        caches.match(req).then(cached =>
          cached || caches.match(OFFLINE_URL) || caches.match('/ui/index.html')
        )
      )
    );
    return;
  }

  /* Static assets: network-first, cache fallback */
  if (url.pathname.startsWith('/ui/')) {
    event.respondWith(
      fetch(req).then(r => {
        if (r.ok) {
          const clone = r.clone();
          caches.open(CACHE_NAME).then(c => c.put(req, clone));
        }
        return r;
      }).catch(() => caches.match(req))
    );
  }
});
