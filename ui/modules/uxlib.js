/* ============================================================
   PureCVisor — modules/uxlib.js
   3차 UX 점검 — 공통 헬퍼 모음
   - toast queue + dedup (#10)
   - request dedup / Promise coalescing (#3)
   - fetch cache with TTL (#9)
   - debounce / throttle (#17)
   - ESC handler attach (#4)
   - role-based UI hide (#15)
   - dirty form tracking (#11)
   - error toast classifier (#2)
   ============================================================ */

window.PCV = window.PCV || {};
(function (PCV) {
  'use strict';

  /* ── #10 Toast queue + dedup ─────────────────────── */
  var _toastQueue = [];
  var _toastShown = false;
  var _toastDedup = {};   /* msg → count */
  function showToastQueued(msg, ok) {
    if (!msg) return;
    var key = (ok === false ? 'E:' : 'I:') + msg;
    /* 1.5초 내 동일 메시지 → 카운트 누적 */
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

  /* ── #2 Error toast classifier ──────────────────── */
  /* 같은 에러 3회 연속 → toast, 1-2회는 status bar에만 */
  var _errCounters = {};
  function reportError(scope, err) {
    var key = scope + ':' + (err && err.message || String(err));
    _errCounters[key] = (_errCounters[key] || 0) + 1;
    setTimeout(function () { _errCounters[key] = Math.max(0, (_errCounters[key] || 0) - 1); }, 30000);
    if (_errCounters[key] >= 3) {
      showToastQueued(scope + ' 반복 오류: ' + (err && err.message || err), false);
      _errCounters[key] = 0;
    }
    /* 항상 콘솔/이벤트 로그 */
    if (typeof addEvt === 'function') {
      try { addEvt('[' + scope + '] ' + (err && err.message || err)); } catch (_) {}
    }
  }
  window.reportError = reportError;

  /* ── #3 Request dedup / coalescing ──────────────── */
  var _pending = {};
  function dedupRequest(key, promiseFactory) {
    if (_pending[key]) return _pending[key];
    var p = Promise.resolve().then(promiseFactory);
    _pending[key] = p;
    p.finally(function () { delete _pending[key]; });
    return p;
  }
  window.dedupRequest = dedupRequest;

  /* ── #9 fetch cache with TTL ────────────────────── */
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

  /* ── #17 debounce / throttle ────────────────────── */
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

  /* ── #4 ESC handler attach for inline showModal ── */
  var _modalEscBound = false;
  function bindGlobalModalEsc() {
    if (_modalEscBound) return;
    _modalEscBound = true;
    document.addEventListener('keydown', function (e) {
      if (e.key !== 'Escape') return;
      /* Modal.close가 우선, 없으면 closeModal */
      if (window.Modal && Modal._isOpen && Modal._isOpen()) { Modal.close(); return; }
      var m = document.getElementById('modal-bg');
      if (m && m.style.display !== 'none' && typeof closeModal === 'function') closeModal();
    });
  }
  window.bindGlobalModalEsc = bindGlobalModalEsc;

  /* ── #5 destroyConfirm: 이름 타이핑 요구 ────────── */
  function destroyConfirm(opts) {
    /* opts: { title, name, warning, onConfirm } */
    var title = opts.title || '삭제 확인';
    var name = opts.name || '';
    var warn = opts.warning || '이 작업은 되돌릴 수 없습니다.';
    var html = '<h2 style="color:var(--red)">&#9888; ' + escapeHtml(title) + '</h2>' +
      '<p class="color-yellow">' + escapeHtml(warn) + '</p>' +
      '<p class="text-13">계속하려면 <code class="color-red">' + escapeHtml(name) + '</code> 을(를) 정확히 입력하세요:</p>' +
      '<input id="dc-input" aria-label="' + escapeHtml(name) + ' 확인 입력" class="login-input" style="width:100%;margin:8px 0" autocomplete="off">' +
      '<div id="dc-msg" class="text-xs color-red" style="min-height:18px"></div>' +
      '<div style="text-align:right;margin-top:12px">' +
      '<button class="btn" onclick="closeModal()">취소</button> ' +
      '<button class="btn btn-r" id="dc-go">삭제</button></div>';
    if (typeof showModal === 'function') showModal(html);
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

  /* ── #15 Role-based UI hide ─────────────────────── */
  /* data-role="ADMIN" 또는 data-role="OPERATOR,ADMIN" 속성 검사 */
  function applyRoleVisibility(role) {
    var elements = document.querySelectorAll('[data-role]');
    for (var i = 0; i < elements.length; i++) {
      var el = elements[i];
      var allowed = (el.getAttribute('data-role') || '').split(',').map(function (s) { return s.trim().toUpperCase(); });
      if (allowed.indexOf(String(role || '').toUpperCase()) === -1) {
        el.style.display = 'none';
      } else {
        el.style.display = '';
      }
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

  /* ── #11 Form dirty tracking + beforeunload ────── */
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

  /* ── #12 Theme change → chart rebuild trigger ──── */
  window.addEventListener('pcv-theme-change', function () {
    if (window.pcvCharts && typeof pcvCharts === 'object') {
      Object.keys(pcvCharts).forEach(function (id) {
        try { pcvCharts[id].destroy(); } catch (_) {}
        delete pcvCharts[id];
      });
    }
  });

  /* 자동 초기화 */
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', bindGlobalModalEsc);
  } else {
    bindGlobalModalEsc();
  }

  /* ── #10 숫자/바이트/시간 포맷 헬퍼 ──────────────── */
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
  function formatRelativeTime(ts) {
    if (!ts) return '-';
    var t = (typeof ts === 'number') ? ts : Date.parse(ts);
    if (isNaN(t)) return String(ts);
    var diff = Math.floor((Date.now() - t) / 1000);
    if (diff < 0) diff = 0;
    if (diff < 60)    return diff + '초 전';
    if (diff < 3600)  return Math.floor(diff / 60) + '분 전';
    if (diff < 86400) return Math.floor(diff / 3600) + '시간 전';
    if (diff < 604800) return Math.floor(diff / 86400) + '일 전';
    return new Date(t).toISOString().slice(0, 10);
  }
  window.formatNumber = formatNumber;
  window.formatBytes = formatBytes;
  window.formatRelativeTime = formatRelativeTime;

  /* ── #13 재시도 가능한 toast — 마지막 액션 저장 ──── */
  var _lastFailedAction = null;
  function toastRetry(msg, action) {
    /* action: { label, fn } */
    _lastFailedAction = action;
    if (typeof toast === 'function') toast(msg + '  [R]', false);
    /* 키보드 R 키로 재시도 — 5초 윈도우 */
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

  /* ── #6 단축키 레지스트리 ───────────────────────── */
  var _shortcuts = {};
  function registerShortcut(combo, fn, label) {
    /* combo 예: 'n', '/', '?', 'ctrl+k' */
    _shortcuts[combo.toLowerCase()] = { fn: fn, label: label || '' };
  }
  function listShortcuts() { return _shortcuts; }
  window.registerShortcut = registerShortcut;
  window.listShortcuts = listShortcuts;
  document.addEventListener('keydown', function (e) {
    /* input/textarea 안에서는 무시 */
    var t = e.target;
    if (t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || t.isContentEditable)) return;
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

  /* ── #7 컨텍스트 메뉴 — 범용 ─────────────────── */
  function showCtxMenu(ev, items) {
    /* items: [{label, fn, icon?}, ...] */
    if (ev && ev.preventDefault) ev.preventDefault();
    var existing = document.getElementById('pcv-ctx-menu');
    if (existing) existing.remove();
    var m = document.createElement('div');
    m.id = 'pcv-ctx-menu';
    m.style.cssText = 'position:fixed;z-index:99999;background:var(--bg2,#1a1a2e);border:1px solid var(--border,#333);border-radius:4px;box-shadow:0 4px 12px rgba(0,0,0,0.6);padding:4px 0;min-width:160px;font-size:12px';
    items.forEach(function (it) {
      var row = document.createElement('div');
      row.style.cssText = 'padding:8px 14px;cursor:pointer;color:var(--fg,#ccc);white-space:nowrap';
      row.innerHTML = it.label;
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
    /* 화면 경계 보정 */
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

  /* ── #14 (보강) 동적 <title> 변경 ───────────────── */
  function setPageTitle(name) {
    document.title = name ? ('PureCVisor — ' + name) : 'PureCVisor';
  }
  window.setPageTitle = setPageTitle;

  /* ── #15 (보강) breadcrumb 렌더링 ───────────────── */
  function renderBreadcrumbs(items) {
    /* items: [{label, page}, ...] */
    var el = document.getElementById('breadcrumbs');
    if (!el) return;
    if (!items || items.length === 0) { el.innerHTML = ''; return; }
    var h = items.map(function (it, i) {
      var last = i === items.length - 1;
      var name = (typeof escapeHtml === 'function') ? escapeHtml(it.label) : it.label;
      if (last) return '<span class="bc-item bc-active">' + name + '</span>';
      if (it.page) return '<a class="bc-item" href="#/' + it.page + '" onclick="navigateTo(\'' + it.page + '\');return false">' + name + '</a><span class="bc-sep">›</span>';
      return '<span class="bc-item">' + name + '</span><span class="bc-sep">›</span>';
    }).join('');
    el.innerHTML = h;
  }
  window.renderBreadcrumbs = renderBreadcrumbs;

  /* ── #14 Hash routing — 페이지/리소스 deep link ── */
  /* 형식: #/page  또는  #/page/resource-name */
  function parseHash() {
    var h = (location.hash || '').replace(/^#\/?/, '');
    if (!h) return { page: null, id: null };
    var parts = h.split('/');
    return { page: parts[0] || null, id: parts[1] || null };
  }
  function navigateToHash() {
    var p = parseHash();
    if (!p.page) return;
    if (typeof navigateTo === 'function') {
      try { navigateTo(p.page); } catch (e) {}
    }
    if (p.id && typeof window.openResourceById === 'function') {
      setTimeout(function () { window.openResourceById(p.page, p.id); }, 200);
    }
  }
  function setHashRoute(page, id) {
    var h = '#/' + page + (id ? '/' + encodeURIComponent(id) : '');
    if (location.hash !== h) {
      try { history.replaceState(null, '', h); } catch (_) { location.hash = h; }
    }
  }
  window.parseHashRoute = parseHash;
  window.navigateToHash = navigateToHash;
  window.setHashRoute = setHashRoute;
  window.addEventListener('hashchange', navigateToHash);
  /* 초기 로드 시 hash 적용은 인증 완료 후 호출 (app.js에서) */

  /* ── #5 DOM-safe 빌더 (ADR-013) ─────────────────── */
  /* innerHTML/outerHTML/insertAdjacentHTML/document.write 금지(ADR-013)를
   * 코드로 강제하는 최소 빌더. el()/frag()에 넘긴 문자열은 어떤 경로로도
   * HTML로 파싱되지 않고 항상 텍스트 노드로만 삽입된다. */

  /* el/frag 공용 children 평탄화. 문자열/숫자 → TextNode, Node → 그대로,
   * 배열 → 재귀 평탄화, null/undefined/false → skip(조건부 렌더링 관용구). */
  function _appendChildren(node, children) {
    (children || []).forEach(function (child) {
      if (child === null || child === undefined || child === false) return;
      if (Array.isArray(child)) { _appendChildren(node, child); return; }
      if (child instanceof Node) { node.appendChild(child); return; }
      /* 문자열/숫자는 반드시 createTextNode만 — 마크업 해석 경로 없음 */
      node.appendChild(document.createTextNode(String(child)));
    });
  }

  /**
   * el(tag, attrs, ...children) — createElement + 속성 + 자식 합성.
   * attrs(객체|null) 처리 규칙:
   *   class/className → node.className
   *   dataset(객체)    → Object.assign(node.dataset, value) (병합)
   *   style(문자열)    → node.style.cssText = value
   *   on*(함수)        → addEventListener(이벤트명 소문자, fn) — 예: onClick → 'click'
   *   그 외            → setAttribute(key, value) (aria-*, role, id, href, ...)
   *   value가 null/undefined/false면 해당 속성은 건너뜀.
   * 사용례: el('span', { class: 'badge' }, 'Running')
   */
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

  /** frag(...children) — DocumentFragment 생성. children 규칙은 el()과 동일.
   *  여러 형제 노드를 한 번의 appendChild로 삽입할 때 사용. */
  function frag(...children) {
    var fragment = document.createDocumentFragment();
    _appendChildren(fragment, children);
    return fragment;
  }

  /** clearEl(node) — 자식 전량 제거 (innerHTML = '' 대체). */
  function clearEl(node) {
    if (!node) return;
    if (typeof node.replaceChildren === 'function') node.replaceChildren();
    else while (node.firstChild) node.removeChild(node.firstChild);
  }

  /* ── PCV.uxlib namespace export ─────────────────── */
  PCV.uxlib = {
    showToastQueued: showToastQueued,
    reportError: reportError,
    dedupRequest: dedupRequest,
    cachedFetch: cachedFetch,
    invalidateCache: invalidateCache,
    debounce: debounce,
    throttle: throttle,
    bindGlobalModalEsc: bindGlobalModalEsc,
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
    renderBreadcrumbs: renderBreadcrumbs,
    parseHashRoute: parseHash,
    navigateToHash: navigateToHash,
    setHashRoute: setHashRoute,
    el: el,
    frag: frag,
    clearEl: clearEl
  };
})(window.PCV);
