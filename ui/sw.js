                                                               
                                     
                                                
                                           
                                                                  

                                                         
                                                                  
                                                              
  
                                                             
                                                 
                                                  
                       
  
                                                             
                                             
                                                                        
                                                           
  
                             
                                                                    
                                                            
                                                           
                                                                     
                                              
  
                      
                                                             
                                                
                                                             
                                                      
                                                    
                                          
  
                       
                                                                   
                                                        
                                                    
                                                        
   

const CACHE_NAME="pcv-ui-v02d35ab3";
const OFFLINE_URL = '/ui/offline.html';

                              
                                                                 
                                                    
                                                  
const PENDING_CACHE = 'pcv-push-pending';
const PENDING_URL = '/ui/__pcv_push_pending';

                                                        
                                                        
                                                 
                                                             
const STATIC_ASSETS = [
  '/ui/',
  '/ui/index.html',
  '/ui/docs.html',
  '/ui/guide.html',
  '/ui/guide-content.md',
  '/ui/offline.html',
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
      Promise.all(keys.filter(k => k !== CACHE_NAME && k !== PENDING_CACHE)
        .map(k => caches.delete(k)))
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
      }).catch(async () => {
                                                          
                                                                            
                                                                       
                                                           
                                                                         
                                                        
                                                                 
                                                                  
                                                          
                                                          
                                                               
                                  
        return (await caches.match(req))
          || (await caches.match(OFFLINE_URL))
          || (await caches.match('/ui/index.html'));
      })
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

                         
                                      
                                                                 
                                               
                                          
                                                    
                                               
                   
self.addEventListener('push', event => {
                                                                    
  let d;
  try { d = (event.data ? event.data.json() : null) || {}; } catch (e) { d = { title: 'PureCVisor', body: '' }; }
  event.waitUntil(self.registration.showNotification(d.title || 'PureCVisor 알림', {
    body: d.body || '',
                                                         
                                                            
                                                         
    tag: 'pcv-alert',
    renotify: true,
    data: { url: d.url || '#mon-alerts' },
    icon: '/ui/icon-192.png'                                                
  }));
});

                                          
                                                  
                                                 
                                               
                                        
  
                                                
                                                 
                                             
                                                
                  
  
                                                                              
   
self.addEventListener('pushsubscriptionchange', event => {
  event.waitUntil((async () => {
    const oldSub = event.oldSubscription || null;
    let next = event.newSubscription || null;

                                         
                                                        
                                                 
    if (!next) {
      try {
        const key = oldSub && oldSub.options && oldSub.options.applicationServerKey;
        if (key) {
          next = await self.registration.pushManager.subscribe({
            userVisibleOnly: true, applicationServerKey: key
          });
        }
      } catch (e) { next = null; }
    }

    const rec = {
      old: oldSub && oldSub.endpoint ? oldSub.endpoint : null,
      sub: (next && typeof next.toJSON === 'function') ? next.toJSON() : null,
      ts: Date.now()
    };
                                       
    if (!rec.old && !rec.sub) return;

    try {
      const cache = await caches.open(PENDING_CACHE);
      await cache.put(new Request(PENDING_URL), new Response(JSON.stringify(rec), {
        headers: { 'content-type': 'application/json' }
      }));
    } catch (e) {                           }

                                                 
    const cs = await self.clients.matchAll({ type: 'window', includeUncontrolled: true });
    cs.forEach(c => c.postMessage({ type: 'pcv-push-changed' }));
  })());
});

                                                          
                                
self.addEventListener('notificationclick', event => {
  event.notification.close();
  const url = (event.notification.data && event.notification.data.url) || '#mon-alerts';
  event.waitUntil(self.clients.matchAll({ type: 'window', includeUncontrolled: true }).then(cs => {
    if (cs.length) {
      cs[0].focus();
      cs[0].postMessage({ type: 'pcv-nav', tab: url.replace('#', '') });
    } else {
      return self.clients.openWindow('/ui/' + url);
    }
  }));
});
