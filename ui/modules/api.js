
if (!window.API_BASE) window.API_BASE = '/api/v1';
if (!window.authToken) window.authToken = sessionStorage.getItem('pcv_token') || '';
if (!window.eventLog) window.eventLog = [];

window.PCV = window.PCV || {};
(function(PCV) {

var _wsReconnectAttempt = 0;
var _wsMaxReconnect = 5;

function pcvSetLoginVisible(visible) {
  var loginPage = document.getElementById('login-page');
  if (loginPage) loginPage.style.display = visible ? 'flex' : 'none';
  if (document.body) document.body.classList.toggle('login-active', !!visible);
  var mobileNav = document.getElementById('mobile-nav');
  if (mobileNav) mobileNav.setAttribute('aria-hidden', visible ? 'true' : 'false');
}
window.pcvSetLoginVisible = pcvSetLoginVisible;

var _refreshInProgress = null;

async function _tryRefreshToken() {
  var refreshToken = sessionStorage.getItem('pcv_refresh_token');
  if (!refreshToken) return false;
  if (_refreshInProgress) return _refreshInProgress;
  _refreshInProgress = (async function() {
    try {
      var r = await fetch(window.API_BASE + '/auth/refresh', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ refresh_token: refreshToken })
      });
      if (!r.ok) return false;
      var d = await r.json();
      if (d.access_token) {
        window.authToken = d.access_token;
        sessionStorage.setItem('pcv_token', d.access_token);
        if (d.refresh_token) sessionStorage.setItem('pcv_refresh_token', d.refresh_token);
        return true;
      }
      return false;
    } catch (e) { return false; }
    finally { _refreshInProgress = null; }
  })();
  return _refreshInProgress;
}

function _redirectToLogin() {
  window.authToken = '';
  sessionStorage.removeItem('pcv_token');
  sessionStorage.removeItem('pcv_refresh_token');
  sessionStorage.removeItem('pcv_user');

  if (window.wsConnection) window.wsConnection.close();
  pcvSetLoginVisible(true);
  var la = document.getElementById('la');
  if (la) la.classList.remove('hidden');
  var us = document.getElementById('us');
  if (us) { us.classList.add('hidden'); us.style.display = 'none'; }
}

var PCV_FETCH_TIMEOUT_MS = 12000;

function _fetchWithTimeout(url, opts) {
  var controller = new AbortController();
  var tid = setTimeout(function() { controller.abort(); }, PCV_FETCH_TIMEOUT_MS);
  var merged = Object.assign({}, opts, { signal: controller.signal });
  return fetch(url, merged).finally(function() { clearTimeout(tid); });
}

async function _fetchWithRefresh(url, opts) {
  var r = await _fetchWithTimeout(url, opts);
  if (r.status === 401 && sessionStorage.getItem('pcv_refresh_token')) {
    var refreshed = await _tryRefreshToken();
    if (refreshed) {

      if (opts.headers) {
        if (typeof opts.headers.set === 'function') opts.headers.set('Authorization', 'Bearer ' + window.authToken);
        else opts.headers['Authorization'] = 'Bearer ' + window.authToken;
      }
      return _fetchWithTimeout(url, opts);
    } else {
      _redirectToLogin();
      throw new Error('Session expired');
    }
  }
  return r;
}

function fetchGet(u) {
  return _fetchWithRefresh(u, { headers: { Authorization: 'Bearer ' + window.authToken } }).then(r => {
    if (!r.ok) return r.json().catch(() => ({ error: { code: r.status, message: 'HTTP ' + r.status } }));
    return r.json();
  });
}

function fetchPost(u, b) {
  return _fetchWithRefresh(u, {
    method: 'POST',
    headers: { Authorization: 'Bearer ' + window.authToken, 'Content-Type': 'application/json' },
    body: JSON.stringify(b)
  }).then(r => {
    if (!r.ok) return r.json().catch(() => ({ error: { code: r.status, message: 'HTTP ' + r.status } }));
    return r.json();
  });
}

function fetchDelete(u, body) {
  var opts = {
    method: 'DELETE',
    headers: { Authorization: 'Bearer ' + window.authToken }
  };
  if (body) { opts.headers['Content-Type'] = 'application/json'; opts.body = JSON.stringify(body); }
  return _fetchWithRefresh(u, opts).then(r => {
    if (!r.ok) return r.json().catch(() => ({ error: { code: r.status, message: 'HTTP ' + r.status } }));
    return r.json();
  });
}

function fetchPut(u, b) {
  return _fetchWithRefresh(u, {
    method: 'PUT',
    headers: { Authorization: 'Bearer ' + window.authToken, 'Content-Type': 'application/json' },
    body: JSON.stringify(b)
  }).then(r => {
    if (!r.ok) return r.json().catch(() => ({ error: { code: r.status, message: 'HTTP ' + r.status } }));
    return r.json();
  });
}

function _onJobFailureEvent(m) {
  var detailEl = document.getElementById('pwr-err-detail');
  if (detailEl) {
    var row = document.createElement('div');
    row.appendChild(document.createTextNode('\u2022 '));
    var methodEl = document.createElement('strong');
    methodEl.textContent = m.method || '?';
    row.appendChild(methodEl);
    row.appendChild(document.createTextNode(': ' + (m.error || 'unknown')));
    detailEl.replaceChildren(row);
  }
  var pfEl = document.getElementById('pwr-p');
  if (pfEl) { pfEl.style.width = '100%'; pfEl.style.background = 'var(--red)'; }

  var rawMethod = m.method || '?';
  var rawError = m.error || 'unknown';
  var target = '';
  if (m.job_id) {

    var colonIdx = m.job_id.indexOf(':');
    if (colonIdx > 0) target = m.job_id.substring(colonIdx + 1);
  }
  var title = rawMethod + (target ? ' failed: ' + target : ' failed');

  if (typeof addNotification === 'function') {
    addNotification('error', title, rawError);
  }
  if (typeof addEvt === 'function') {
    addEvt('FAIL ' + title + ' — ' + rawError);
  }
}

function connectWS() {
  if (!window.authToken) { setTimeout(connectWS, 2e3); return; }
  if (_wsReconnectAttempt >= _wsMaxReconnect) {
    if (!document.getElementById('ws-reconnect-banner')) {
      var banner = document.createElement('div');
      banner.id = 'ws-reconnect-banner';
      banner.style.cssText = 'position:fixed;bottom:40px;right:16px;background:var(--bg2);border:1px solid var(--red);border-radius:6px;padding:10px 16px;z-index:9999;font-size:12px;color:var(--fg)';

      var wsFailMsg = (typeof _L === 'function' ? _L('WebSocket 연결 실패', 'WebSocket connection failed') : 'WS failed');
      var wsRetryLabel = (typeof _L === 'function' ? _L('재시도', 'Retry') : 'Retry');
      banner.appendChild(PCV.uxlib.frag(
        PCV.uxlib.el('span', { class: 'color-red' }, '⚠'),
        ' ' + wsFailMsg + ' ',
        PCV.uxlib.el('button', { class: 'btn', style: 'font-size:10px;margin-left:8px', onClick: function() {
          banner.remove();
          _wsReconnectAttempt = 0;
          connectWS();
        } }, wsRetryLabel)
      ));
      document.body.appendChild(banner);
    }
    return;
  }
  const p = location.protocol === 'https:' ? 'wss:' : 'ws:';

  const ws = new WebSocket(p + '//' + location.host + window.API_BASE + '/ws/events');
  window.wsConnection = ws;
  ws.onopen = () => {

    if (ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ type: 'auth', token: window.authToken }));
    }
  };
  ws._pcvAuthDone = false;
  ws.onclose = () => {
    if (window.wsConnection === ws) window.wsConnection = null;
    var wsStatus = document.getElementById('ws-s');
    if (wsStatus) { PCV.uxlib.clearEl(wsStatus); wsStatus.appendChild(PCV.uxlib.el('span', { class: 'color-red' }, '●')); }
    _wsReconnectAttempt++;
    var delay = Math.min(30000, 1000 * Math.pow(2, _wsReconnectAttempt));
    setTimeout(connectWS, delay);
  };
  ws.onmessage = function(e) {

    if (!ws._pcvAuthDone) {
      try {
        var d = JSON.parse(e.data);
        if (d.type === 'auth_ok') {
          ws._pcvAuthDone = true;
          _wsReconnectAttempt = 0;
          var wsBanner = document.getElementById('ws-reconnect-banner');
          if (wsBanner) wsBanner.remove();
          var wsStatus = document.getElementById('ws-s');
          if (wsStatus) {
            PCV.uxlib.clearEl(wsStatus);
            wsStatus.appendChild(PCV.uxlib.frag(
              PCV.uxlib.el('span', { class: 'neon-blink color-green', style: 'font-size:14px' }, '●'),
              ' ' + (typeof t === 'function' ? t('ws.live') : 'Live')
            ));
          }
          window.addEvt('WS Connected — ' + p + '//' + location.host + ' (real-time events active)');
          return;
        }
        if (d.type === 'auth_fail') {
          if (typeof toast === 'function') toast('WebSocket auth failed', false);
          return;
        }
      } catch (_) {  }
    }
    try {
      const m = JSON.parse(e.data);
      window.addEvt('WS Event — type: ' + m.type + (m.name ? ', target: ' + m.name : '') + (m.node ? ', node: ' + m.node : ''));

      if (m.name) {
        var items = document.querySelectorAll('.vi .nm');
        items.forEach(function(el) {
          if (el.textContent === m.name) {
            el.parentElement.style.transition = 'background 0.3s';
            el.parentElement.style.background = 'rgba(0,240,255,0.15)';
            setTimeout(function() { el.parentElement.style.background = ''; }, 2000);
          }
        });
      }

      if (m.type === 'job.complete') {
        var jobPayload = m.payload || m;
        if (jobPayload && jobPayload.status === 'fail') {
          try { _onJobFailureEvent(jobPayload); } catch (_) {}
        }
      }
      if (m.type && (m.type.startsWith('vm-') || m.type.startsWith('vm.'))) {

        var doFullRefresh = function(retry) {
          fetchGet(window.API_BASE + '/vms').then(function(r) {
            var list = unwrapList(r);
            window.vmList = list;
            if (window.selectedVmIndex >= list.length) window.selectedVmIndex = 0;
            window.lastLoadTime = Date.now();
            if (typeof render === 'function') render(true);
          }).catch(function() {

            if (retry < 2) {
              setTimeout(function() { doFullRefresh(retry + 1); }, 3000 * (retry + 1));
            }
          });
        };
        var eventType = m.type;
        var isStateOnly = (eventType === 'vm.state_changed' || eventType === 'vm-state');
        if (isStateOnly && m.name && window.vmList) {

          var idx = window.vmList.findIndex(function(v){ return v && v.name === m.name; });
          if (idx >= 0 && m.state) {
            window.vmList[idx].state = m.state;
            if (typeof render === 'function') render(true);
            return;
          }
        }
        doFullRefresh(0);
      } else if (m.type === 'network-change') {
        if (window.currentTab === 'networks') window.renderContent();
      } else if (m.type === 'container-change') {
        if (window.currentTab === 'containers') window.renderContent();
      } else if (m.type === 'alert') {
        if (window.currentTab === 'mon-alerts') window.renderContent();
      } else {
        window.loadAll(true);
      }
    } catch (x) {  }
  };
}

function _loginI18n(key, fallback) {
  var value = (typeof t === 'function') ? t(key) : key;
  return value && value !== key ? value : fallback;
}

async function _readLoginResponse(r) {
  var body = await r.text();
  if (!body) return {};
  try {
    return JSON.parse(body);
  } catch (e) {

    return {
      error: {
        code: r.status || 0,
        message: _loginI18n('login.bad_response', 'Login server returned an invalid response.')
      }
    };
  }
}

function _loginHttpDetail(status) {
  if (status === 401 || status === 403) {
    return _loginI18n('login.invalid_credentials', 'Invalid username or password.');
  }
  if (status === 429) {
    return _loginI18n('login.too_many', 'Too many login attempts. Try again later.');
  }
  if (status === 502 || status === 503 || status === 504) {
    return _loginI18n('login.service_unavailable', 'Login service is temporarily unavailable.');
  }
  if (status >= 500) {
    return _loginI18n('login.server_error', 'Login server could not process the request.');
  }
  return _loginI18n('login.bad_response', 'Login server returned an invalid response.');
}

function _loginFailureText(status) {
  var titleKey = (status === 401 || status === 403) ? 'login.failed' : 'login.error';
  var titleFallback = (status === 401 || status === 403) ? 'Login failed' : 'Connection error';
  var prefix = _loginI18n(titleKey, titleFallback);
  return prefix + ': ' + _loginHttpDetail(status || 0);
}

function _loginNetworkFailureText(e) {
  var prefix = _loginI18n('login.error', 'Connection error');
  var detail = e && e.name === 'AbortError'
    ? _loginI18n('login.timeout', 'Login request timed out.')
    : _loginI18n('login.network', 'Could not connect to the login server.');
  return prefix + ': ' + detail;
}

async function doLoginPage() {
  const user = document.getElementById('login-user')?.value.trim();
  const pass = document.getElementById('login-pass')?.value;
  const errEl = document.getElementById('login-err');
  if (errEl) errEl.textContent = '';
  if (!user || !pass) { if (errEl) errEl.textContent = typeof t === 'function' ? t('login.required') : 'Required'; return; }
  try {
    const r = await _fetchWithTimeout(window.API_BASE + '/auth/token', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ username: user, password: pass })
    });
    const d = await _readLoginResponse(r);
    if (r.ok && d.access_token) {
      window.authToken = d.access_token;
      sessionStorage.setItem('pcv_token', window.authToken);
      if (d.refresh_token) sessionStorage.setItem('pcv_refresh_token', d.refresh_token);
      sessionStorage.setItem('pcv_user', user);

      pcvSetLoginVisible(false);
      document.getElementById('la').classList.add('hidden');
      document.getElementById('us').classList.remove('hidden');
      document.getElementById('us').style.display = 'flex';
      document.getElementById('us-name').textContent = user;
      document.getElementById('sb1').textContent = typeof t === 'function' ? t('connected') : 'Connected';
      if (typeof window.__pcvDismissSplash === 'function') window.__pcvDismissSplash();
      window.toast(typeof t === 'function' ? t('logged.in') + ': ' + user : 'Logged in: ' + user);
      window.addEvt('AUTH Login successful — user: ' + user + ', JWT issued');
      window.loadAll();
      connectWS();
      startSessionWatch();
      if (typeof renderPinnedBar === 'function') renderPinnedBar();

      if (typeof pcvPostLoginInit === 'function') pcvPostLoginInit();
    } else {
      if (errEl) errEl.textContent = _loginFailureText(r.status);
    }
  } catch (e) {
    if (errEl) errEl.textContent = _loginNetworkFailureText(e);
  }
}

async function doLogin() {
  const user = document.getElementById('iu')?.value.trim();
  const pass = document.getElementById('ip')?.value;
  try {
    const r = await _fetchWithTimeout(window.API_BASE + '/auth/token', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ username: user, password: pass })
    });
    const d = await _readLoginResponse(r);
    if (r.ok && d.access_token) {
      window.authToken = d.access_token;
      sessionStorage.setItem('pcv_token', window.authToken);
      if (d.refresh_token) sessionStorage.setItem('pcv_refresh_token', d.refresh_token);
      sessionStorage.setItem('pcv_user', user);

      document.getElementById('la').classList.add('hidden');
      document.getElementById('us').classList.remove('hidden');
      document.getElementById('us').style.display = 'flex';
      document.getElementById('us-name').textContent = user;
      document.getElementById('sb1').textContent = typeof t === 'function' ? t('connected') : 'Connected';
      if (typeof window.__pcvDismissSplash === 'function') window.__pcvDismissSplash();
      window.toast(typeof t === 'function' ? t('logged.in') + ': ' + user : 'Logged in: ' + user);
      window.addEvt('AUTH Login successful — user: ' + user);
      window.loadAll();
      connectWS();
      startSessionWatch();
    } else {
      window.toast(_loginFailureText(r.status), false);
    }
  } catch (e) {
    window.toast(_loginNetworkFailureText(e), false);
  }
}

function doLogout() {
  window.authToken = '';
  sessionStorage.removeItem('pcv_token');
  sessionStorage.removeItem('pcv_refresh_token');
  sessionStorage.removeItem('pcv_user');

  if (window.wsConnection) window.wsConnection.close();
  window.wsConnection = null;
  window.vmList = [];
  pcvSetLoginVisible(true);
  document.getElementById('la').classList.remove('hidden');
  document.getElementById('us').classList.add('hidden');
  document.getElementById('us').style.display = 'none';
  document.getElementById('sb1').textContent = typeof t === 'function' ? t('not_connected') : 'Not connected';
  PCV.uxlib.clearEl(document.getElementById('ws-s'));
  PCV.uxlib.clearEl(document.getElementById('vl'));
  PCV.uxlib.clearEl(document.getElementById('cb'));
  document.getElementById('vc').textContent = '0';
  window.toast(typeof t === 'function' ? t('logged.out') : 'Logged out');
  window.addEvt(typeof t === 'function' ? t('logged.out') : 'Logged out');
}

var _sessionCheckInterval = null;
function startSessionWatch() {
  if (_sessionCheckInterval) clearInterval(_sessionCheckInterval);
  _sessionCheckInterval = setInterval(function() {
    var token = window.authToken;
    if (!token) return;
    try {
      var payload = JSON.parse(atob(token.split('.')[1]));
      var exp = payload.exp * 1000;
      var remaining = exp - Date.now();
      if (remaining < 0) {

        clearInterval(_sessionCheckInterval);
        _redirectToLogin();
        toast(typeof _L === 'function' ? _L('세션 만료 — 다시 로그인하세요', 'Session expired — please login again') : 'Session expired', false);
      } else if (remaining < 300000) {

        var mins = Math.ceil(remaining / 60000);
        var el = document.getElementById('session-warn');
        if (!el) {
          el = document.createElement('div');
          el.id = 'session-warn';
          el.style.cssText = 'position:fixed;bottom:40px;left:50%;transform:translateX(-50%);background:var(--bg2);border:1px solid var(--yellow);border-radius:6px;padding:8px 16px;z-index:9999;font-size:12px;color:var(--fg)';
          document.body.appendChild(el);
        }
        var lblExpires = typeof _L === 'function' ? _L('세션 만료까지', 'Session expires in') : 'Expires in';
        var lblMin = typeof _L === 'function' ? _L('분', 'min') : 'min';
        PCV.uxlib.clearEl(el);
        el.appendChild(PCV.uxlib.frag(
          PCV.uxlib.el('span', { class: 'color-yellow' }, '⚠'),
          ' ' + lblExpires + ' ' + mins + lblMin
        ));

        if (remaining < 120000) {
          _tryRefreshToken().then(function(ok) {
            if (ok) {
              var warn = document.getElementById('session-warn');
              if (warn) warn.remove();
              toast(typeof _L === 'function' ? _L('세션 자동 갱신됨', 'Session auto-renewed') : 'Session renewed');
            }
          }).catch(function(){});
        }
      } else {
        var warn = document.getElementById('session-warn');
        if (warn) warn.remove();
      }
    } catch (e) {  }
  }, 30000);
}

function restoreSession() {
  if (!window.authToken) {

    pcvSetLoginVisible(true);
    var la = document.getElementById('la');
    if (la) la.classList.remove('hidden');
    return;
  }
  const savedUser = sessionStorage.getItem('pcv_user') || 'admin';

  var refreshToken = sessionStorage.getItem('pcv_refresh_token');
  (refreshToken ? _tryRefreshToken().then(function(ok) {
    if (!ok) throw new Error('refresh failed');
    return true;
  }) : fetchGet(window.API_BASE + '/vms')).then(() => {
    pcvSetLoginVisible(false);
    document.getElementById('la').classList.add('hidden');
    const us = document.getElementById('us');
    if (us) { us.classList.remove('hidden'); us.style.display = 'flex'; }
    document.getElementById('us-name').textContent = savedUser;
    document.getElementById('sb1').textContent = typeof t === 'function' ? t('connected') : 'Connected';
    if (typeof window.__pcvDismissSplash === 'function') window.__pcvDismissSplash();
    window.loadAll();
    connectWS();
    startSessionWatch();
  }).catch(() => {
    sessionStorage.removeItem('pcv_token');
    sessionStorage.removeItem('pcv_user');

    window.authToken = '';
  });
}

var _apiActivityLog = [];

var _perfMetrics = {
  pageLoadTime: 0,
  firstContentfulPaint: 0,
  apiCallCount: 0,
  apiTotalTime: 0,
  avgApiTime: 0
};

window.addEventListener('load', function() {
  setTimeout(function() {
    if (window.performance && window.performance.timing) {
      var t = window.performance.timing;
      _perfMetrics.pageLoadTime = t.loadEventEnd - t.navigationStart;
    }
    if (window.performance && window.performance.getEntriesByType) {
      var paints = window.performance.getEntriesByType('paint');
      paints.forEach(function(p) {
        if (p.name === 'first-contentful-paint') _perfMetrics.firstContentfulPaint = Math.round(p.startTime);
      });
    }
  }, 100);
});

function unwrapData(r) {
  if (r == null) return r;
  if (r.data !== undefined) return r.data;
  if (r.result !== undefined) return r.result;
  return r;
}

function unwrapList(r) {
  if (Array.isArray(r)) return r;
  var d = unwrapData(r);
  return Array.isArray(d) ? d : [];
}

var _pollingTimers = {};
function startAdaptivePolling(id, fn, intervalMs) {
  stopAdaptivePolling(id);
  var run = function() {
    if (document.hidden) return;
    fn();
  };
  run();
  _pollingTimers[id] = setInterval(run, intervalMs);

  var _vc = function() {
    if (!_pollingTimers[id]) { document.removeEventListener('visibilitychange', _vc); return; }
    if (!document.hidden) run();
  };
  if (!window._pollingListeners) window._pollingListeners = {};
  window._pollingListeners[id] = _vc;
  document.addEventListener('visibilitychange', _vc);
}
function stopAdaptivePolling(id) {
  if (_pollingTimers[id]) { clearInterval(_pollingTimers[id]); delete _pollingTimers[id]; }
  if (window._pollingListeners && window._pollingListeners[id]) {
    document.removeEventListener('visibilitychange', window._pollingListeners[id]);
    delete window._pollingListeners[id];
  }
}

var _origFetch = window.fetch;
window.fetch = function() {
  var url = arguments[0] || '';
  if (typeof url === 'string' && url.includes('/api/')) {
    _perfMetrics.apiCallCount++;
    var start = performance.now();
    return _origFetch.apply(this, arguments).then(function(r) {
      _perfMetrics.apiTotalTime += (performance.now() - start);
      _perfMetrics.avgApiTime = Math.round(_perfMetrics.apiTotalTime / _perfMetrics.apiCallCount);
      return r;
    });
  }
  return _origFetch.apply(this, arguments);
};

PCV.api = {
  fetchGet: fetchGet,
  fetchPost: fetchPost,
  fetchDelete: fetchDelete,
  fetchPut: fetchPut,
  unwrapData: unwrapData,
  unwrapList: unwrapList,
  connectWS: connectWS,
  doLoginPage: doLoginPage,
  doLogin: doLogin,
  doLogout: doLogout,
  restoreSession: restoreSession,
  startSessionWatch: startSessionWatch,
  startAdaptivePolling: startAdaptivePolling,
  stopAdaptivePolling: stopAdaptivePolling,
  _tryRefreshToken: _tryRefreshToken,
  _redirectToLogin: _redirectToLogin,
  _apiActivityLog: _apiActivityLog,
  _perfMetrics: _perfMetrics
};

window.unwrapData = unwrapData;
window.unwrapList = unwrapList;
window.fetchGet = fetchGet;
window.fetchPost = fetchPost;
window.fetchDelete = fetchDelete;
window.fetchPut = fetchPut;
window._tryRefreshToken = _tryRefreshToken;
window._redirectToLogin = _redirectToLogin;
window.connectWS = connectWS;
window.doLoginPage = doLoginPage;
window.doLogin = doLogin;
window.doLogout = doLogout;
window.startSessionWatch = startSessionWatch;
window.restoreSession = restoreSession;
window.startAdaptivePolling = startAdaptivePolling;
window.stopAdaptivePolling = stopAdaptivePolling;
window._apiActivityLog = _apiActivityLog;
window._perfMetrics = _perfMetrics;

})(window.PCV);
