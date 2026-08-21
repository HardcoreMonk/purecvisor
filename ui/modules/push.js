                                                                  
                                                        
                                 

                                                                         
                                                            
                                                 
                              

                            
                                                                         
                                                                       
                                                            
                                                                      
                                                                          
                                                                           
                                                                       
                                                              
                                                      
                                              

                                                     
                                                                 
                                     
                                                                     
window.PCV = window.PCV || {};
(function (PCV) {
  'use strict';

                                                             
  function _t(ko, en) { return (typeof _L === 'function') ? _L(ko, en) : ko; }
                                                        
  function _api() { return PCV.api || {}; }

  function _unsupportedText() {
    return _t('이 브라우저는 웹 푸시를 지원하지 않습니다.',
              'This browser does not support web push.');
  }
  function _failText() { return _t('요청에 실패했습니다.', 'Request failed.'); }

                                                   
  function _errText(r, fallback) {
    var e = r && r.error;
    if (!e) return fallback;
    var m = e.message || e.code;
    return m ? String(m) : fallback;
  }

                                                             
                                                          
                                               
  function _urlB64ToU8(b64) {
    var pad = new Array(((4 - (b64.length % 4)) % 4) + 1).join('=');
    var s = (b64 + pad).replace(/-/g, '+').replace(/_/g, '/');
    var raw = window.atob(s);
    var out = new Uint8Array(raw.length);
    for (var i = 0; i < raw.length; i++) out[i] = raw.charCodeAt(i);
    return out;
  }

                                    
                                                            
                                                   
                                    
  function _sameKey(a, b) {
    if (!a || !b) return false;
    var x = new Uint8Array(a.buffer ? a.buffer : a);
    if (x.length !== b.length) return false;
    for (var i = 0; i < b.length; i++) if (x[i] !== b[i]) return false;
    return true;
  }

                                                              
                                                
                                                                  
  function supported() {
    return !!(window.navigator && navigator.serviceWorker &&
              window.PushManager && window.Notification);
  }

                                                                  
                                                                         
  function _currentSub() {
    return navigator.serviceWorker.getRegistration().then(function (reg) {
      if (!reg || !reg.pushManager) return null;
      return reg.pushManager.getSubscription();
    });
  }

                                                                  
                                                                
                                                              
                                                                 
                                                      
                        
    
                                                                      
                                                                    
                                                         
                                                   
                                                    
                         
  function _swBlockedText() {
    var err = window._swRegError;
    if (!err) {
      return _t('서비스 워커가 등록되지 않았습니다 — 페이지를 새로고침한 뒤 다시 시도하세요.',
                'Service worker is not registered — reload the page and try again.');
    }
    var msg = err.message || '';
    var isCert = err.name === 'SecurityError' || /ssl|certificat|insecure/i.test(msg);
    if (isCert) {
      return _t(
        '이 사이트의 인증서가 브라우저에서 신뢰되지 않아 서비스 워커 등록이 차단되었습니다. ' +
        '인증서를 신뢰 목록에 추가한 뒤 다시 시도하세요. (로컬 노드라면 http://127.0.0.1 계열 ' +
        '주소는 인증서 없이 동작합니다)',
        'This site’s certificate is not trusted by the browser, so service worker ' +
        'registration was blocked. Add the certificate to your trust store and try again. ' +
        '(If this is a local node, http://127.0.0.1-style addresses work without a certificate.)'
      );
    }
    return _t('서비스 워커 등록이 실패했습니다: ' + msg,
              'Service worker registration failed: ' + msg);
  }

  function _readyReg() {
    return navigator.serviceWorker.getRegistration().then(function (reg) {
      if (!reg) {
        throw new Error(_swBlockedText());
      }
      if (reg.active) return reg;
      var tid;
      var timeout = new Promise(function (_resolve, reject) {
        tid = setTimeout(function () {
          reject(new Error(_t('서비스 워커가 준비되지 않았습니다 (시간 초과).',
                              'Service worker did not become ready (timed out).')));
        }, PCV.push.READY_TIMEOUT_MS);
      });
      return Promise.race([
        navigator.serviceWorker.ready.then(function (r) { clearTimeout(tid); return r; }),
        timeout
      ]);
    });
  }

                                                                             
                                                               
                                                                         
                                                                
             
  function status() {
    if (!supported()) return Promise.resolve({ perm: 'unsupported', subscribed: false, swBlocked: false });
    var perm = 'default';
    try { perm = Notification.permission || 'default'; } catch (e) { perm = 'default'; }
    var swBlocked = !!window._swRegError;
    return _currentSub().then(function (sub) {
      return { perm: perm, subscribed: !!sub, swBlocked: swBlocked };
    }, function () {
      return { perm: perm, subscribed: false, swBlocked: swBlocked };
    });
  }

                                                              
  async function enable() {
    if (!supported()) return { ok: false, error: _unsupportedText() };

    var perm;
    try { perm = await Notification.requestPermission(); } catch (e) { perm = 'denied'; }
    if (perm !== 'granted') {
      return { ok: false, error: _t('브라우저에서 알림 권한이 허용되지 않았습니다.',
                                    'Notification permission was not granted.') };
    }

    var vr = await _api().fetchGet(EP.PUSH_VAPID());
    if (vr && vr.error) {
      return { ok: false, error: _errText(vr, _t('VAPID 공개키를 받지 못했습니다.',
                                                 'Failed to fetch the VAPID public key.')) };
    }
    var key = (_api().unwrapData(vr) || {}).key;
    if (!key) {
      return { ok: false, error: _t('서버가 VAPID 공개키를 주지 않았습니다 (webpush 비활성?).',
                                    'Server returned no VAPID public key (webpush disabled?).') };
    }

    var bytes = _urlB64ToU8(key);
    var sub;
    try {
      var reg = await _readyReg();
      sub = await reg.pushManager.getSubscription();
      if (sub && !_sameKey(sub.options && sub.options.applicationServerKey, bytes)) {
        try { await sub.unsubscribe(); } catch (e) {                      }
        sub = null;
      }
      if (!sub) {
        sub = await reg.pushManager.subscribe({
          userVisibleOnly: true,                                              
          applicationServerKey: bytes
        });
      }
    } catch (e) {
      return { ok: false, error: (e && e.message) ? String(e.message)
        : _t('브라우저 구독 생성에 실패했습니다.', 'Browser subscription failed.') };
    }

    var j = (typeof sub.toJSON === 'function') ? sub.toJSON() : sub;
    var keys = j.keys || {};
    var res = await _api().fetchPost(EP.PUSH_SUBSCRIBE(), {
      endpoint: j.endpoint,
      keys: { p256dh: keys.p256dh, auth: keys.auth }
    });
    if (res && res.error) {
                                                     
                                                      
      try { await sub.unsubscribe(); } catch (e) {             }
      return { ok: false, error: _errText(res, _failText()) };
    }
    return { ok: true };
  }

                                                              
                                       
                                                                 
                                                    
                                               
  function _sha256Hex(s) {
    var c = window.crypto;
    if (!c || !c.subtle || typeof TextEncoder !== 'function') return Promise.resolve(null);
    return c.subtle.digest('SHA-256', new TextEncoder().encode(s)).then(function (buf) {
      var b = new Uint8Array(buf), out = '';
      for (var i = 0; i < b.length; i++) out += (b[i] < 16 ? '0' : '') + b[i].toString(16);
      return out;
    }, function () { return null; });
  }

                                                   
                                                          
    
                                                         
                                                          
                                                 
                              
  function _serverHas(endpoint) {
    return _sha256Hex(endpoint).then(function (digest) {
      if (!digest) return null;
      return _api().fetchGet(EP.PUSH_MINE()).then(function (r) {
        if (!r || r.error) return null;
        var d = _api().unwrapData(r) || {};
        var rows = d.subscriptions;
        if (!Array.isArray(rows)) return null;
        for (var i = 0; i < rows.length; i++) {
          if (rows[i] && rows[i].digest === digest) return true;
        }
        return false;
      }, function () { return null; });
    });
  }

                                                         
                                                               
                                                
                                                                 
  var PENDING_CACHE = 'pcv-push-pending';
  var PENDING_URL = '/ui/__pcv_push_pending';

  function _pendingRead() {
    if (!window.caches) return Promise.resolve(null);
    return caches.open(PENDING_CACHE).then(function (c) {
      return c.match(PENDING_URL).then(function (res) {
        return res ? res.json() : null;
      });
    }).catch(function () { return null; });
  }
  function _pendingClear() {
    if (!window.caches) return Promise.resolve();
    return caches.open(PENDING_CACHE)
      .then(function (c) { return c.delete(PENDING_URL); })
      .catch(function () {                          });
  }

                                                   
                                                    
                                                    
                                               
                                             
  async function _drainPending() {
    var rec = await _pendingRead();
    if (!rec) return;

    if (rec.sub && rec.sub.endpoint) {
      var k = rec.sub.keys || {};
      var res = await _api().fetchPost(EP.PUSH_SUBSCRIBE(), {
        endpoint: rec.sub.endpoint,
        keys: { p256dh: k.p256dh, auth: k.auth }
      });
      if (res && res.error) return;                            
    }
    if (rec.old && (!rec.sub || rec.old !== rec.sub.endpoint)) {
      await _api().fetchPost(EP.PUSH_UNSUBSCRIBE(), { endpoint: rec.old });
    }
    await _pendingClear();
  }

                                                
    
                                                                   
                                                                   
                                                    
                                                   
         
    
                                                         
                                                     
                                                       
                                            
    
                                                    
                                     
  async function resync() {
    if (!supported()) return { ok: false, error: _unsupportedText() };

                                                          
                                                          
                                             
    try { await _drainPending(); } catch (e) {                            }

    var sub;
    try { sub = await _currentSub(); } catch (e) { sub = null; }
    if (!sub) return { ok: true };                            

    var vr = await _api().fetchGet(EP.PUSH_VAPID());
    if (vr && vr.error) {
      return { ok: false, error: _errText(vr, _t('VAPID 공개키를 받지 못했습니다.',
                                                 'Failed to fetch the VAPID public key.')) };
    }
    var key = (_api().unwrapData(vr) || {}).key;
    if (!key) {
      return { ok: false, error: _t('서버가 VAPID 공개키를 주지 않았습니다 (webpush 비활성?).',
                                    'Server returned no VAPID public key (webpush disabled?).') };
    }
                                                      
                                                 
                                                  
                                                
    if (!_sameKey(sub.options && sub.options.applicationServerKey, _urlB64ToU8(key))) {
      return enable();
    }

    var j = (typeof sub.toJSON === 'function') ? sub.toJSON() : sub;
    var have = await _serverHas(j.endpoint);
    if (have === true) return { ok: true };                       

    var keys = j.keys || {};
    var res = await _api().fetchPost(EP.PUSH_SUBSCRIBE(), {
      endpoint: j.endpoint,
      keys: { p256dh: keys.p256dh, auth: keys.auth }
    });
    if (res && res.error) return { ok: false, error: _errText(res, _failText()) };
    return { ok: true };
  }

                                                                  
  async function disable() {
    if (!supported()) return { ok: false, error: _unsupportedText() };
    var sub;
    try { sub = await _currentSub(); } catch (e) { sub = null; }
    if (!sub) return { ok: true };

    var endpoint = sub.endpoint;
    try { await sub.unsubscribe(); } catch (e) {                    }
    var res = await _api().fetchPost(EP.PUSH_UNSUBSCRIBE(), { endpoint: endpoint });
    if (res && res.error) return { ok: false, error: _errText(res, _failText()) };
    return { ok: true };
  }

                                                    
                                              
    
                          
                                                                
                                                        
                                                                    
                                                                    
                                                   
                                         
    
                                    
                                                            
                                                    
                                                        
                                             
                                  
    
                                                
                                                            
  var _resyncAt = 0;                                                     
  var _resyncInFlight = null;

                                                  
                                                
                                                 
  function _healOnce() {
    if (_resyncAt && (Date.now() - _resyncAt) < PCV.push.RESYNC_INTERVAL_MS) {
      return Promise.resolve(null);
    }
    if (!_resyncInFlight) {
      _resyncInFlight = resync().then(function (r) {
        _resyncInFlight = null;
        if (r && r.ok === false) return r;                              
        _resyncAt = Date.now();
        return r;
      }, function (e) {
        _resyncInFlight = null;
        throw e;
      });
    }
    return _resyncInFlight;
  }

                                                               
                                                           
                                                            
                                                              
  function _refresh(btn, line) {
    return status().then(function (s) {
      btn.textContent = s.subscribed ? _t('구독 해제', 'Unsubscribe') : _t('알림 구독', 'Subscribe');
      btn.dataset.subscribed = s.subscribed ? '1' : '0';
      btn.disabled = false;
      btn.removeAttribute('aria-disabled');
                                                        
                                                  
      if (s.swBlocked && !s.subscribed) {
        btn.disabled = true;
        btn.setAttribute('aria-disabled', 'true');
        PCV.uxlib.setMsg(line, 'err', { size: '12px' }, _swBlockedText());
      } else if (s.subscribed) {
        PCV.uxlib.setMsg(line, 'ok', { size: '12px' },
          _t('이 브라우저로 알림을 받습니다.', 'This browser receives alerts.'));
      } else if (s.perm === 'denied') {
        PCV.uxlib.setMsg(line, 'err', { size: '12px' },
          _t('브라우저에서 알림이 차단돼 있습니다 — 사이트 권한을 허용으로 바꾸세요.',
             'Notifications are blocked — allow them in site permissions.'));
      } else {
        PCV.uxlib.setMsg(line, 'muted', { size: '12px' },
          _t('구독하면 심각·경고 알림을 이 브라우저로 받습니다.',
             'Subscribe to receive critical/warning alerts in this browser.'));
      }
      return s;
    });
  }

     
                                                                
    
                                                                 
                                               
                                                          
                                            
    
                    
                                                       
                                                      
                                                     
                                                      
                                  
     
  function _onToggle(btn, line) {
    if (btn.disabled) return Promise.resolve();
    var wasSubscribed = btn.dataset.subscribed === '1';
    btn.disabled = true;
    PCV.uxlib.setMsg(line, 'loading', { size: '12px' }, wasSubscribed
      ? _t('해제 중…', 'Unsubscribing…') : _t('구독 중…', 'Subscribing…'));
    return (wasSubscribed ? disable() : enable()).then(function (r) {
      btn.disabled = false;
                                                    
                                                     
                                               
      if (r && r.ok) _resyncAt = wasSubscribed ? 0 : Date.now();
      return _refresh(btn, line).then(function () {
                                                    
        if (!r || !r.ok) {
          PCV.uxlib.setMsg(line, 'err', { size: '12px' }, (r && r.error) || _failText());
        }
      });
    }).catch(function (e) {
      btn.disabled = false;
      PCV.uxlib.setMsg(line, 'err', { size: '12px' },
        (e && e.message) ? String(e.message) : _failText());
    });
  }

                                            
                                                                    
  function toggleNode(opts) {
    opts = opts || {};
    var el = PCV.uxlib.el;
    var id = opts.id || 'push-toggle';
    var line = el('div', { class: 'push-status', id: id + '-status' });
    var btn = el('button', { class: opts.btnClass || 'btn', type: 'button', id: id },
      _t('알림 구독', 'Subscribe'));
    var wrap = el('div', { class: 'push-toggle' + (opts.cls ? ' ' + opts.cls : '') }, btn, line);

    if (!supported()) {
      btn.disabled = true;
      btn.setAttribute('aria-disabled', 'true');
      PCV.uxlib.setMsg(line, 'muted', { size: '12px' }, _unsupportedText());
      return wrap;
    }
    btn.dataset.subscribed = '0';
    btn.addEventListener('click', function () { _onToggle(btn, line); });
    PCV.uxlib.setMsg(line, 'loading', { size: '12px' },
      _t('구독 상태 확인 중…', 'Checking subscription…'));
                                                 
                                                            
                                                  
    _refresh(btn, line).then(function (s) {
      if (!s || !s.subscribed || s.perm !== 'granted') return null;
      return _healOnce().then(function (r) {
        if (r && r.ok === false) {
          PCV.uxlib.setMsg(line, 'err', { size: '12px' }, r.error || _failText());
        }
        return r;
      });
    }).catch(function () {
      PCV.uxlib.setMsg(line, 'err', { size: '12px' },
        _t('구독 상태를 확인하지 못했습니다.', 'Could not read the subscription state.'));
    });
    return wrap;
  }

                                                       
                                                                      
                                        
  function _routeTab(tab) {
    var m = PCV.mobile;
    if (m && typeof m.isActive === 'function' && m.isActive() && typeof m.showTab === 'function') {
      m.showTab(/alert/.test(tab) ? 'alerts' : 'home');
      return;
    }
    if (typeof window.navigateTo === 'function') window.navigateTo(tab);
  }

                              
                                                            
                                                                           
                                                         
  function _wireNavMessages() {
    if (!(window.navigator && navigator.serviceWorker)) return;
    navigator.serviceWorker.addEventListener('message', function (ev) {
      var d = ev && ev.data;
      if (!d) return;
      if (d.type === 'pcv-nav' && d.tab) { _routeTab(d.tab); return; }
      if (d.type === 'pcv-push-changed') {
                                                   
                                               
        _resyncAt = 0;
        resync().catch(function () {                           });
      }
    });
  }
  _wireNavMessages();

  PCV.push = {
    supported: supported,
    status: status,
    enable: enable,
    disable: disable,
    resync: resync,
    toggleNode: toggleNode,
                                                
                                                 
    READY_TIMEOUT_MS: 10000,
                                                      
                                                          
    RESYNC_INTERVAL_MS: 60000
  };
})(window.PCV);
