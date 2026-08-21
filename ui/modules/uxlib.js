                                                               
                                
                      
                              
                                            
                              
                              
                            
                             
                              
                                

                       
                                                   
                                                 
                                                                  

                                                                              
                                                                   
                                                  
  
                                                              
                                                        
                                                       
                                             
                                                                              
                                                  
                                                    
                            
                                                                 
                                                                   
                                                   
                                                                 
                                                               
             
  
                                                                      
                                                    
                                             
  
                                                                              
                                                 
                                                                  
                                                    
                                                                             
                                                                    
                                                                
                                                                
                                                                  
                                               
   

window.PCV = window.PCV || {};
(function (PCV) {
  'use strict';

                                                          
  var _toastQueue = [];
  var _toastShown = false;
  var _toastDedup = {};                    
                                                          
                                                           
                                                         
                                 
  function showToastQueued(msg, ok) {
    if (!msg) return;
    var key = (ok === false ? 'E:' : 'I:') + msg;
                                
    if (_toastDedup[key]) {
      _toastDedup[key].count++;
      return;
    }
    _toastDedup[key] = { count: 1, ts: Date.now() };
    _toastQueue.push({ msg: msg, ok: ok, key: key });
    setTimeout(function () { delete _toastDedup[key]; }, 1500);
    _drainToasts();
  }
  function _drainToasts() {
    if (_toastShown || !_toastQueue.length) return;
    _toastShown = true;
    var t = _toastQueue.shift();
    var entry = _toastDedup[t.key];
    var label = (entry && entry.count > 1) ? (t.msg + ' (×' + entry.count + ')') : t.msg;
    if (typeof toast === 'function') toast(label, t.ok);
    setTimeout(function () { _toastShown = false; _drainToasts(); }, 1800);
  }
  window.showToastQueued = showToastQueued;

                                                         
                                               
  var _errCounters = {};
                                                       
                                            
                                                         
                                          
  function reportError(scope, err) {
    var key = scope + ':' + (err && err.message || String(err));
    _errCounters[key] = (_errCounters[key] || 0) + 1;
    setTimeout(function () { _errCounters[key] = Math.max(0, (_errCounters[key] || 0) - 1); }, 30000);
    if (_errCounters[key] >= 3) {
      showToastQueued(scope + ' 반복 오류: ' + (err && err.message || err), false);
      _errCounters[key] = 0;
    }
                      
    if (typeof addEvt === 'function') {
      try { addEvt('[' + scope + '] ' + (err && err.message || err)); } catch (_) {}
    }
  }
  window.reportError = reportError;

                                                         
  var _pending = {};
                                                                
                                                         
                                                      
                                                        
  function dedupRequest(key, promiseFactory) {
    if (_pending[key]) return _pending[key];
    var p = Promise.resolve().then(promiseFactory);
    _pending[key] = p;
    p.finally(function () { delete _pending[key]; });
    return p;
  }
  window.dedupRequest = dedupRequest;

                                                         
  var _cache = {};
                                                                
                                                     
                                                            
                                                               
                     
  function cachedFetch(key, ttlMs, fetcher) {
    var now = Date.now();
    var hit = _cache[key];
    if (hit && (now - hit.ts) < ttlMs) return Promise.resolve(hit.data);
    return dedupRequest('cache:' + key, function () {
      return fetcher().then(function (data) {
        _cache[key] = { data: data, ts: Date.now() };
        return data;
      });
    });
  }
  function invalidateCache(prefix) {
    if (!prefix) { _cache = {}; return; }
    Object.keys(_cache).forEach(function (k) {
      if (k.indexOf(prefix) === 0) delete _cache[k];
    });
  }
  window.cachedFetch = cachedFetch;
  window.invalidateCache = invalidateCache;

                                                         
  function debounce(fn, ms) {
    var t = null;
    return function () {
      var args = arguments, ctx = this;
      clearTimeout(t);
      t = setTimeout(function () { fn.apply(ctx, args); }, ms);
    };
  }
  function throttle(fn, ms) {
    var last = 0, pending = null;
    return function () {
      var args = arguments, ctx = this, now = Date.now();
      if (now - last >= ms) { last = now; fn.apply(ctx, args); }
      else { clearTimeout(pending); pending = setTimeout(function () { last = Date.now(); fn.apply(ctx, args); }, ms - (now - last)); }
    };
  }
  window.pcvDebounce = debounce;
  window.pcvThrottle = throttle;

                                                  
     
                                                           
    
                    
                                                            
                                                       
                                                                        
                                                           
                                                                                
                                                        
                                                                
                                         
    
                 
                                                        
                                                   
                                            
                                                       
                                          
                                                     
                                             
     
  function destroyConfirm(opts) {
                                                   
    var title = opts.title || '삭제 확인';
    var name = opts.name || '';
    var warn = opts.warning || '이 작업은 되돌릴 수 없습니다.';
    var body = [
      el('h2', { style: 'color:var(--red)' }, '\u{26A0} ', title),
      el('p', { class: 'color-yellow' }, warn),
      el('p', { class: 'text-13' }, '계속하려면 ', el('code', { class: 'color-red' }, name), ' 을(를) 정확히 입력하세요:'),
                                                                    
                                                              
                                                                
                                                                
                                                        
      el('input', { id: 'dc-input', 'aria-label': escapeHtml(name) + ' 확인 입력', class: 'login-input', style: 'width:100%;margin:8px 0', autocomplete: 'off' }),
      el('div', { id: 'dc-msg', class: 'text-xs color-red', style: 'min-height:18px' }),
      el('div', { style: 'text-align:right;margin-top:12px' },
        el('button', { class: 'btn', onclick: 'closeModal()' }, '취소'),
        ' ',
        el('button', { class: 'btn btn-r', id: 'dc-go' }, '삭제'))
    ];
    if (typeof showModal === 'function') showModal(body);
    setTimeout(function () {
      var i = document.getElementById('dc-input');
      var b = document.getElementById('dc-go');
      var m = document.getElementById('dc-msg');
      if (!i || !b) return;
      i.focus();
      b.addEventListener('click', function () {
        if (i.value !== name) {
          if (m) m.textContent = '이름이 일치하지 않습니다';
          i.style.borderColor = 'var(--red)';
          return;
        }
        if (typeof closeModal === 'function') closeModal();
        try { opts.onConfirm && opts.onConfirm(); } catch (e) { reportError('destroyConfirm', e); }
      });
      i.addEventListener('keydown', function (e) {
        if (e.key === 'Enter') b.click();
      });
    }, 100);
  }
  window.destroyConfirm = destroyConfirm;

                                                         
                                                           
                                                        
                                                   
                                                              
     
                                                                    
    
                 
                                                      
                                                         
                                                          
                            
                                                     
                                                      
                      
     
  function applyRoleVisibility(role) {
    var elements = document.querySelectorAll('[data-role]');
    for (var i = 0; i < elements.length; i++) {
      var el = elements[i];
      var allowed = (el.getAttribute('data-role') || '').split(',').map(function (s) { return s.trim().toUpperCase(); });
      el.classList.toggle('role-hidden', allowed.indexOf(String(role || '').toUpperCase()) === -1);
    }
  }
  window.applyRoleVisibility = applyRoleVisibility;

                                                          
                                                         
                                                                
                                                    
                                                 
  function getUiEdition() {
    return 'single';
  }
  function isMultiEdgeUI() {
    return false;
  }
  function filterEditionItems(items) {
    return (items || []).filter(function(item) {
      return !item || !item.multiOnly || isMultiEdgeUI();
    });
  }
  PCV.getUiEdition = getUiEdition;
  PCV.isMultiEdgeUI = isMultiEdgeUI;
  PCV.filterEditionItems = filterEditionItems;
  window.getUiEdition = getUiEdition;

                                                        
                                                           
                                              
                                                          
                                                       
                                                               
  var _dirtyForms = new Set();
  function markFormDirty(id) { _dirtyForms.add(id); }
  function clearFormDirty(id) { _dirtyForms.delete(id); }
  function clearAllFormDirty() { _dirtyForms.clear(); }
  window.markFormDirty = markFormDirty;
  window.clearFormDirty = clearFormDirty;
  window.clearAllFormDirty = clearAllFormDirty;
  window.addEventListener('beforeunload', function (e) {
    if (_dirtyForms.size > 0) {
      e.preventDefault();
      e.returnValue = '';
      return '';
    }
  });

                                                        
                                                        
                                                      
                                                        
                                               
  window.addEventListener('pcv-theme-change', function () {
    if (window.pcvCharts && typeof pcvCharts === 'object') {
      Object.keys(pcvCharts).forEach(function (id) {
        try { pcvCharts[id].destroy(); } catch (_) {}
        delete pcvCharts[id];
      });
    }
  });

                                               
  function formatNumber(n) {
    if (n === null || n === undefined || isNaN(n)) return '-';
    try { return Number(n).toLocaleString('ko-KR'); }
    catch (_) { return String(n); }
  }
                                                                     
                                                                
                                                              
  function formatBytes(b, decimals) {
    if (b === null || b === undefined || isNaN(b)) return '-';
    var d = decimals === undefined ? 1 : decimals;
    var n = Number(b);
    if (n === 0) return '0 B';
    var k = 1024, units = ['B', 'KB', 'MB', 'GB', 'TB', 'PB'];
    var i = Math.min(units.length - 1, Math.floor(Math.log(Math.abs(n)) / Math.log(k)));
    return (n / Math.pow(k, i)).toFixed(d) + ' ' + units[i];
  }
                                                                          
                                                                             
                          
  function formatRelativeTime(input) {
    if (input === null || input === undefined || input === '') return '-';
    var t = (input instanceof Date) ? input.getTime()
          : (typeof input === 'number') ? input
          : Date.parse(input);
    if (isNaN(t)) return String(input);
    var diffSec = Math.round((Date.now() - t) / 1000);
    var lang = (window.I18N && typeof I18N.getLang === 'function') ? I18N.getLang() : 'ko';
    var abs = Math.abs(diffSec), v, unit;
    if (abs < 60)       { v = diffSec;                  unit = 'second'; }
    else if (abs < 3600)  { v = Math.round(diffSec / 60);    unit = 'minute'; }
    else if (abs < 86400) { v = Math.round(diffSec / 3600);  unit = 'hour'; }
    else if (abs < 604800){ v = Math.round(diffSec / 86400); unit = 'day'; }
    else { return new Date(t).toLocaleDateString(lang === 'en' ? 'en-US' : 'ko-KR'); }
    try { return new Intl.RelativeTimeFormat(lang, { numeric: 'auto' }).format(-v, unit); }
    catch { return (abs < 60 ? diffSec + 's' : Math.round(diffSec / 60) + 'm') + ' ago'; }
  }
  window.formatNumber = formatNumber;
  window.formatBytes = formatBytes;
  window.formatRelativeTime = formatRelativeTime;

                                             
  var _lastFailedAction = null;
                                                     
                                                             
                      
                                                             
                                                 
                                                
                                  
  function toastRetry(msg, action) {
                               
    _lastFailedAction = action;
    if (typeof toast === 'function') toast(msg + '  [R]', false);
                               
    var handler = function (e) {
      if (e.key === 'r' || e.key === 'R') {
        document.removeEventListener('keydown', handler);
        if (_lastFailedAction && typeof _lastFailedAction.fn === 'function') {
          _lastFailedAction.fn();
          _lastFailedAction = null;
        }
      }
    };
    document.addEventListener('keydown', handler);
    setTimeout(function () { document.removeEventListener('keydown', handler); }, 5000);
  }
  window.toastRetry = toastRetry;

                                                 
  var _shortcuts = {};
                                                               
                                                    
                                                       
                                                       
  function registerShortcut(combo, fn, label) {
                                          
    _shortcuts[combo.toLowerCase()] = { fn: fn, label: label || '' };
  }
  function listShortcuts() { return _shortcuts; }
  window.registerShortcut = registerShortcut;
  window.listShortcuts = listShortcuts;
  document.addEventListener('keydown', function (e) {
                                                  
    var t = e.target;
    if (t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' ||
              t.tagName === 'SELECT' || t.isContentEditable)) return;
    var key = '';
    if (e.ctrlKey)  key += 'ctrl+';
    if (e.altKey)   key += 'alt+';
    if (e.shiftKey && e.key.length > 1) key += 'shift+';
    key += e.key.toLowerCase();
    var sc = _shortcuts[key];
    if (sc && typeof sc.fn === 'function') {
      e.preventDefault();
      sc.fn();
    }
  });

                                              
     
                                                           
    
                                                         
                                                   
                                                
                                                          
                                                       
         
                                                                
                               
                                                                     
                                                          
                                                           
     
  function showCtxMenu(ev, items) {
                                                                                
    if (ev && ev.preventDefault) ev.preventDefault();
    var existing = document.getElementById('pcv-ctx-menu');
    if (existing) existing.remove();
    var m = document.createElement('div');
    m.id = 'pcv-ctx-menu';
    m.style.cssText = 'position:fixed;z-index:99999;background:var(--bg2,#1a1a2e);border:1px solid var(--border,#333);border-radius:4px;box-shadow:0 4px 12px rgba(0,0,0,0.6);padding:4px 0;min-width:160px;font-size:12px';
    items.forEach(function (it) {
      var row = document.createElement('div');
      row.style.cssText = 'padding:8px 14px;cursor:pointer;color:var(--fg,#ccc);white-space:nowrap';
                                                                  
                                                            
      row.textContent = it.label;
      row.addEventListener('mouseenter', function(){ row.style.background = 'rgba(0,240,255,0.08)'; });
      row.addEventListener('mouseleave', function(){ row.style.background = ''; });
      row.addEventListener('click', function () {
        m.remove();
        if (typeof it.fn === 'function') {
          try { it.fn(); } catch (e) { reportError('ctx-menu', e); }
        }
      });
      m.appendChild(row);
    });
    document.body.appendChild(m);
    var x = ev.clientX, y = ev.clientY;
                  
    var rect = m.getBoundingClientRect();
    if (x + rect.width > window.innerWidth) x = window.innerWidth - rect.width - 8;
    if (y + rect.height > window.innerHeight) y = window.innerHeight - rect.height - 8;
    m.style.left = x + 'px';
    m.style.top = y + 'px';
    var close = function (e) {
      if (!m.contains(e.target)) {
        m.remove();
        document.removeEventListener('click', close);
        document.removeEventListener('keydown', escClose);
      }
    };
    var escClose = function (e) {
      if (e.key === 'Escape') {
        m.remove();
        document.removeEventListener('click', close);
        document.removeEventListener('keydown', escClose);
      }
    };
    setTimeout(function () {
      document.addEventListener('click', close);
      document.addEventListener('keydown', escClose);
    }, 0);
  }
  window.showCtxMenu = showCtxMenu;

                                                   
  function setPageTitle(name) {
    document.title = name ? ('PureCVisor — ' + name) : 'PureCVisor';
  }
  window.setPageTitle = setPageTitle;

                                                  
                                            
                    
                                                                     
                                                              
                                      
    
                 
                                                     
     
  var _lastApprovedHash = location.hash || '';
                                                         
                                                               
  var RETIRED_HASH_ROUTES = Object.freeze({ serviceguide: 'helppage' });
                                                                    
                                                                      
  var RETIRED_DOCUMENT_ROUTES = Object.freeze({ restguide: '/ui/docs.html#14-rest-api' });

                    
                                                                           
                                                              
                                                    
    
                 
                                                              
                                                                       
                                                      
  function _hashUrl(h) { return location.pathname + location.search + (h || ''); }

  function parseHash() {
    var h = (location.hash || '').replace(/^#\/?/, '');
    if (!h) return { page: null, id: null };
    var parts = h.split('/');
    return { page: parts[0] || null, id: parts[1] || null };
  }
  function navigateToHash() {
    var p = parseHash();
    if (!p.page) return;
    if (Object.prototype.hasOwnProperty.call(RETIRED_DOCUMENT_ROUTES, p.page)) {
      window.location.replace(RETIRED_DOCUMENT_ROUTES[p.page]);
      return;
    }
    if (Object.prototype.hasOwnProperty.call(RETIRED_HASH_ROUTES, p.page)) {
      p = { page: RETIRED_HASH_ROUTES[p.page], id: null };
    }
    var committed = false;
    function commitRequestedRoute() {
      if (committed) return;
      committed = true;
      setHashRoute(p.page, p.id);
      if (p.id && typeof window.openResourceById === 'function') {
        setTimeout(function () { window.openResourceById(p.page, p.id); }, 200);
      }
    }
    if (typeof navigateTo === 'function') {
      try {
        var accepted = navigateTo(p.page, { after: commitRequestedRoute });
        if (accepted === false) {
          try { history.replaceState(null, '', _hashUrl(_lastApprovedHash)); } catch {}
          return;
        }
                                                                           
                                                                        
        if (!committed) commitRequestedRoute();
      } catch {
        try { history.replaceState(null, '', _hashUrl(_lastApprovedHash)); } catch {}
        return;
      }
    } else {
      commitRequestedRoute();
    }
  }
  function setHashRoute(page, id) {
    var h = '#/' + page + (id ? '/' + encodeURIComponent(id) : '');
    _lastApprovedHash = h;
    if (location.hash !== h) {
      try { history.replaceState(null, '', _hashUrl(h)); } catch (_) { location.hash = h; }
    }
  }
  window.parseHashRoute = parseHash;
  window.navigateToHash = navigateToHash;
  window.setHashRoute = setHashRoute;
  window.addEventListener('hashchange', navigateToHash);
                                              

                                                       
                                                                       
                                                 
                                       

                                                            
                                                              
  function _appendChildren(node, children) {
    (children || []).forEach(function (child) {
      if (child === null || child === undefined || child === false) return;
      if (Array.isArray(child)) { _appendChildren(node, child); return; }
      if (child instanceof Node) { node.appendChild(child); return; }
                                                      
      node.appendChild(document.createTextNode(String(child)));
    });
  }

     
                                                              
                          
                                       
                                                               
                                                 
                                                                             
                                                                              
                                               
                                                          
     
  function el(tag, attrs, ...children) {
    var node = document.createElement(tag);
    if (attrs) {
      Object.keys(attrs).forEach(function (key) {
        var value = attrs[key];
        if (value === null || value === undefined || value === false) return;
        if (key === 'class' || key === 'className') {
          node.className = value;
        } else if (key === 'dataset') {
          Object.assign(node.dataset, value);
        } else if (key === 'style' && typeof value === 'string') {
          node.style.cssText = value;
        } else if (key.slice(0, 2) === 'on' && typeof value === 'function') {
          node.addEventListener(key.slice(2).toLowerCase(), value);
        } else {
          node.setAttribute(key, value);
        }
      });
    }
    _appendChildren(node, children);
    return node;
  }

                                                                     
                                              
  function frag(...children) {
    var fragment = document.createDocumentFragment();
    _appendChildren(fragment, children);
    return fragment;
  }

                                                      
  function clearEl(node) {
    if (!node) return;
    if (typeof node.replaceChildren === 'function') node.replaceChildren();
    else while (node.firstChild) node.removeChild(node.firstChild);
  }

                                                                           
  function _prefersReducedMotion() {
    return !!(window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches);
  }

                                                                   
                                                                                        
                                                                           
  function viewTransition(updateFn) {
    if (typeof updateFn !== 'function') return;
    if (document.startViewTransition && !_prefersReducedMotion()) {
      try { document.startViewTransition(updateFn); return; } catch {}
    }
    updateFn();
  }

                                               
                                            
                                 
                                                                              
                                                                                    
                                                            
                                                                 
                                            
                                                                          
  var MSG_COLOR = { ok: 'var(--green)', err: 'var(--red)', warn: 'var(--yellow)', muted: 'var(--fg2)' };
  function msg(kind, opts, ...children) {
    opts = opts || {};
    var style = (MSG_COLOR[kind] ? 'color:' + MSG_COLOR[kind] + ';' : '') +
                (opts.size ? 'font-size:' + opts.size + ';' : '') + (opts.style || '');
    var node = el(opts.tag || 'span', { class: opts.cls || null, style: style || null });
    if (kind === 'loading') {
      node.appendChild(el('span', { class: 'spinner' }));
      node.appendChild(document.createTextNode(' '));
    }
    _appendChildren(node, children);
    return node;
  }
  function setMsg(target, kind, opts, ...children) {
    var node = typeof target === 'string' ? document.getElementById(target) : target;
    if (!node) return null;
    clearEl(node);
    var built = msg.apply(null, [kind, opts].concat(children));
    node.appendChild(built);
    return built;
  }

     
                                                              
    
                                                                 
                                                         
                                          
     
  function renderLoadError(target, options) {
    options = options || {};
    var node = typeof target === 'string' ? document.getElementById(target) : target;
    if (!node) return null;
    var title = options.title || (typeof _L === 'function' ? _L('상태 조회', 'Status') : 'Status');
    var message = options.message || (typeof _L === 'function'
      ? _L('데이터를 불러오지 못했습니다.', 'Unable to load data.')
      : 'Unable to load data.');
    var retryLabel = typeof _L === 'function' ? _L('다시 시도', 'Retry') : 'Retry';
    clearEl(node);
    node.appendChild(frag(
      window.HN && HN.pagehead ? HN.pagehead({ title: title }) : el('h2', null, title),
      el('div', { class: 'hc load-error-state', role: 'alert', 'aria-live': 'assertive' },
        el('h3', { class: 'color-red' },
          typeof _L === 'function' ? _L('상태 로드 실패', 'Failed to load state') : 'Failed to load state'),
        el('p', { class: 'color-muted text-12 mb-8' }, String(message)),
        typeof options.retry === 'function'
          ? el('button', { class: 'btn', type: 'button', onClick: options.retry }, retryLabel)
          : null)
    ));
    return node.querySelector('.load-error-state');
  }

                                                         
  PCV.uxlib = {
    showToastQueued: showToastQueued,
    reportError: reportError,
    dedupRequest: dedupRequest,
    cachedFetch: cachedFetch,
    invalidateCache: invalidateCache,
    debounce: debounce,
    throttle: throttle,
    destroyConfirm: destroyConfirm,
    applyRoleVisibility: applyRoleVisibility,
    markFormDirty: markFormDirty,
    clearFormDirty: clearFormDirty,
    clearAllFormDirty: clearAllFormDirty,
    formatNumber: formatNumber,
    formatBytes: formatBytes,
    formatRelativeTime: formatRelativeTime,
    toastRetry: toastRetry,
    registerShortcut: registerShortcut,
    listShortcuts: listShortcuts,
    showCtxMenu: showCtxMenu,
    setPageTitle: setPageTitle,
    parseHashRoute: parseHash,
    navigateToHash: navigateToHash,
    setHashRoute: setHashRoute,
    el: el,
    frag: frag,
    clearEl: clearEl,
    viewTransition: viewTransition,
    msg: msg,
    setMsg: setMsg,
    renderLoadError: renderLoadError
  };
})(window.PCV);
