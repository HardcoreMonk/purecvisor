
const CACHE_NAME = 'pcv-ui-v44ffc65f';
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

self.addEventListener('activate', event => {
  event.waitUntil(
    caches.keys().then(keys =>
      Promise.all(keys.filter(k => k !== CACHE_NAME).map(k => caches.delete(k)))
    ).then(() => self.clients.claim())
  );
});

self.addEventListener('message', event => {
  if (event.data && event.data.type === 'SKIP_WAITING') {
    self.skipWaiting();
  }
  if (event.data && event.data.type === 'CLEAR_CACHE') {
    caches.keys().then(keys => Promise.all(keys.map(k => caches.delete(k))))
      .then(() => event.ports[0] && event.ports[0].postMessage({ ok: true }));
  }
});

self.addEventListener('fetch', event => {
  const url = new URL(event.request.url);
  const req = event.request;

  if (url.pathname.startsWith('/api/')) return;
  if (req.url.startsWith('ws://') || req.url.startsWith('wss://')) return;

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
