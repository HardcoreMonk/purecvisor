





























































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
      banner.innerHTML = '<span class="color-red">&#9888;</span> ' + (typeof _L === 'function' ? _L('WebSocket 연결 실패', 'WebSocket connection failed') : 'WS failed') + ' <button class="btn" style="font-size:10px;margin-left:8px" onclick="this.parentElement.remove();window._wsReconnectAttempt=0;connectWS()">' + (typeof _L === 'function' ? _L('재시도', 'Retry') : 'Retry') + '</button>';
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
    if (wsStatus) wsStatus.innerHTML = '<span class="color-red">&#9679;</span>';
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
          if (wsStatus) wsStatus.innerHTML = '<span class="neon-blink color-green" style="font-size:14px">&#9679;</span> ' + (typeof t === 'function' ? t('ws.live') : 'Live');
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
  document.getElementById('ws-s').innerHTML = '';
  document.getElementById('vl').innerHTML = '';
  document.getElementById('cb').innerHTML = '';
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
        el.innerHTML = '<span class="color-yellow">&#9888;</span> ' + lblExpires + ' ' + mins + lblMin;

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

window._wsReconnectAttempt = 0;

})(window.PCV);















window.PCV = window.PCV || {};
(function(PCV) {

var EP = (function() {
  var B = function() { return window.API_BASE || '/api/v1'; };
  var enc = encodeURIComponent;

  var COMMON_ENDPOINTS = {

    VM_LIST:              function()     { return B() + '/vms'; },
    VM_CREATE:            function()     { return B() + '/vms'; },
    VM_DETAIL:            function(n)    { return B() + '/vms/' + enc(n); },
    VM_STOP:              function(n)    { return B() + '/vms/' + enc(n) + '/stop'; },
    VM_ACTION:            function(n,a)  { return B() + '/vms/' + enc(n) + '/' + enc(a); },
    VM_SNAPSHOT_LIST:     function(n)    { return B() + '/vms/' + enc(n) + '/snapshot'; },
    VM_SNAPSHOT_CREATE:   function(n)    { return B() + '/vms/' + enc(n) + '/snapshot/create'; },
    VM_SNAPSHOT_ROLLBACK: function(n)    { return B() + '/vms/' + enc(n) + '/snapshot/rollback'; },
    VM_SNAPSHOT_DELETE:   function(n,s)  { return B() + '/vms/' + enc(n) + '/snapshot/' + enc(s); },
    VM_SNAPSHOT_DELETE_ALL:function(n)   { return B() + '/vms/' + enc(n) + '/snapshot/delete_all'; },
    VM_NICS:              function(n)    { return B() + '/vms/' + enc(n) + '/nics'; },
    VM_NIC_DETACH:        function(n,m)  { return B() + '/vms/' + enc(n) + '/nics/' + enc(m); },
    VM_VCPU:              function(n)    { return B() + '/vms/' + enc(n) + '/vcpu'; },
    VM_MEMORY:            function(n)    { return B() + '/vms/' + enc(n) + '/memory_mb'; },
    VM_ISO:               function(n)    { return B() + '/vms/' + enc(n) + '/iso'; },
    VM_CLONE:             function(n)    { return B() + '/vms/' + enc(n) + '/clone'; },
    VM_DISK:              function(n)    { return B() + '/vms/' + enc(n) + '/disk'; },
    VM_DELETE_STATUS:     function(n)    { return B() + '/vms/' + enc(n) + '/delete-status'; },
    VM_RENAME:            function(n)    { return B() + '/vms/' + enc(n) + '/rename'; },
    VM_EXPORT:            function(n)    { return B() + '/vms/' + enc(n) + '/export'; },
    VM_CPU_PIN:           function(n)    { return B() + '/vms/' + enc(n) + '/cpu-pin'; },
    VM_BANDWIDTH:         function(n)    { return B() + '/vms/' + enc(n) + '/bandwidth'; },
    VM_RPC:               function(n)    { return B() + '/vms/' + enc(n) + '/rpc'; },
    VM_DISK_RESIZE:       function(n)    { return B() + '/vms/' + enc(n) + '/disk-resize'; },
    VM_GUEST_PING:        function(n)    { return B() + '/vms/' + enc(n) + '/guest-ping'; },
    VM_GUEST_AGENT:       function(n)    { return B() + '/vms/' + enc(n) + '/guest-agent'; },
    VM_GUEST_AGENT_CHANNEL:function(n)   { return B() + '/vms/' + enc(n) + '/guest-agent-channel'; },
    VM_GUEST_SHUTDOWN:    function(n)    { return B() + '/vms/' + enc(n) + '/guest-shutdown'; },
    VM_GUEST_EXEC:        function(n)    { return B() + '/vms/' + enc(n) + '/guest-exec'; },
    VM_DISK_USAGE:        function(n)    { return B() + '/vms/' + enc(n) + '/disk-usage'; },
    VNC:                  function(n)    { return B() + '/vnc/' + enc(n); },


    CTR_LIST:             function()     { return B() + '/containers'; },
    CTR_CREATE:           function()     { return B() + '/containers'; },
    CTR_DETAIL:           function(n)    { return B() + '/containers/' + enc(n); },
    CTR_METRICS:          function(n)    { return B() + '/containers/' + enc(n) + '/metrics'; },
    CTR_EXEC:             function(n)    { return B() + '/containers/' + enc(n) + '/exec'; },
    CTR_NICS:             function(n)    { return B() + '/containers/' + enc(n) + '/nics'; },
    CTR_SNAPSHOTS:        function(n)    { return B() + '/containers/' + enc(n) + '/snapshots'; },
    CTR_SNAP_DELETE:      function(n,s)  { return B() + '/containers/' + enc(n) + '/snapshots/' + enc(s); },
    CTR_SNAP_ROLLBACK:    function(n)    { return B() + '/containers/' + enc(n) + '/snapshots/rollback'; },
    CTR_STOP:             function(n)    { return B() + '/containers/' + enc(n) + '/stop'; },
    CTR_START:            function(n)    { return B() + '/containers/' + enc(n) + '/start'; },
    CTR_LIMITS:           function(n)    { return B() + '/containers/' + enc(n) + '/limits'; },
    CTR_BANDWIDTH:        function(n)    { return B() + '/containers/' + enc(n) + '/bandwidth'; },


    STORAGE_POOLS:        function()     { return B() + '/storage/pools'; },
    STORAGE_SCRUB:        function()     { return B() + '/storage/pools/scrub'; },
    STORAGE_ZVOLS:        function()     { return B() + '/storage/zvols'; },
    ISCSI_TARGETS:        function()     { return B() + '/iscsi/targets'; },


    NET_LIST:             function()     { return B() + '/networks'; },
    NET_DETAIL:           function(n)    { return B() + '/networks/' + enc(n); },
    NET_MODE:             function(n)    { return B() + '/networks/' + enc(n) + '/mode'; },
    OVN_STATUS:           function()     { return B() + '/ovn/status'; },
    OVN_SWITCHES:         function()     { return B() + '/ovn/switches'; },
    OVN_ROUTERS:          function()     { return B() + '/ovn/routers'; },
    OVN_ACL:              function()     { return B() + '/ovn/acl'; },
    DEMO_OVN_HEALTH:      function()     { return B() + '/demo/ovn-ovs/health'; },
    OVERLAY_LIST:         function()     { return B() + '/overlay'; },


    CLOUD_JOBS:           function()     { return B() + '/cloud/jobs'; },
    CLOUD_CANCEL:         function()     { return B() + '/cloud/cancel'; },
    CLOUD_IMPORT:         function(n)    { return B() + '/vms/' + enc(n) + '/import-ec2'; },
    CLOUD_EXPORT:         function(n)    { return B() + '/vms/' + enc(n) + '/export-ec2'; },


    AUTH_TOKEN:           function()     { return B() + '/auth/token'; },
    AUTH_REGISTER:        function()     { return B() + '/auth/register'; },
    AUTH_PASSWORD:        function()     { return B() + '/auth/password'; },
    AUTH_REFRESH:         function()     { return B() + '/auth/refresh'; },
    AUTH_USERS:           function()     { return B() + '/auth/users'; },
    AUTH_USER:            function(u)    { return B() + '/auth/users/' + enc(u); },
    AUTH_ROLE:            function()     { return B() + '/auth/role'; },


    HEALTH:               function()     { return B() + '/health'; },
    ALERTS:               function()     { return B() + '/alerts'; },
    ALERTS_CONFIG:        function()     { return B() + '/alerts/config'; },
    METRICS:              function()     { return B() + '/metrics'; },
    MONITOR_FLEET:        function()     { return B() + '/monitor/fleet'; },
    DPDK_STATUS:          function()     { return B() + '/dpdk/status'; },
    DPDK_LIST:            function()     { return B() + '/dpdk/list'; },
    DPDK_HUGEPAGE:        function()     { return B() + '/dpdk/hugepage'; },
    DPDK_BIND:            function()     { return B() + '/dpdk/bind'; },
    DPDK_UNBIND:          function()     { return B() + '/dpdk/unbind'; },
    SRIOV_STATUS:         function()     { return B() + '/sriov/status'; },
    SRIOV_LIST:           function()     { return B() + '/sriov/list'; },
    SRIOV_ENABLE:         function()     { return B() + '/sriov/enable'; },
    SRIOV_DISABLE:        function()     { return B() + '/sriov/disable'; },
    SRIOV_ATTACH:         function()     { return B() + '/sriov/attach'; },
    SRIOV_DETACH:         function()     { return B() + '/sriov/detach'; },


    TEMPLATES:            function()     { return B() + '/templates'; },
    TEMPLATE:             function(n)    { return B() + '/templates/' + enc(n); },
    TEMPLATE_HISTORY:     function()     { return B() + '/templates/history'; },
    BACKUP_POLICIES:      function()     { return B() + '/backup/policies'; },
    BACKUP_RESTORE:       function()     { return B() + '/backup/restore'; },
    DOCKER_LIST:          function()     { return B() + '/docker'; },
    DOCKER_PULL:          function()     { return B() + '/docker/pull'; },
    DOCKER_RUN:           function()     { return B() + '/docker/run'; },
    DOCKER_STOP:          function(n)    { return B() + '/docker/' + enc(n) + '/stop'; },
    TERRAFORM_PLAN:       function()     { return B() + '/terraform/plan'; },
    TERRAFORM_APPLY:      function()     { return B() + '/terraform/apply'; },
    TERRAFORM_STATE:      function()     { return B() + '/terraform/state'; },
    CONFIG_BACKUP:        function()     { return B() + '/config/backup'; },
    CONFIG_HISTORY:       function()     { return B() + '/config/history'; },
    CONFIG_DAEMON:        function()     { return B() + '/config/daemon'; },
    AGENT_CONFIG:         function()     { return B() + '/agent/config'; },
    AGENT_HISTORY:        function()     { return B() + '/agent/history'; },


    ISO_LIST:             function()     { return B() + '/iso'; },




    AUTH_APIKEY_CREATE:   function()     { return B() + '/auth/apikeys'; },
    AUTH_APIKEY_LIST:     function()     { return B() + '/auth/apikeys'; },
    AUTH_APIKEY_REVOKE:   function(n)    { return B() + '/auth/apikeys/' + enc(n) + '/revoke'; },
    AUTH_SESSION_REVOKE:  function()     { return B() + '/auth/sessions/revoke'; },


    VM_BATCH:             function()     { return B() + '/vms/batch'; },
    VM_LIST_FILTERED:     function()     { return B() + '/vms/filtered'; },


    CONFIG_RELOAD:        function()     { return B() + '/config/reload'; },
    HEALTH_DEEP:          function()     { return B() + '/health/deep'; },
    BACKUP_VERIFY:        function()     { return B() + '/backup/verify'; },
    JOBS_PERSIST:         function()     { return B() + '/jobs/persistent'; },
    POOL_CONNINFO:        function()     { return B() + '/pool/conninfo'; },
    DB_MIGRATION:         function()     { return B() + '/db/migration'; },


    ALERT_SILENCE:        function()     { return B() + '/alerts/silence'; },
    ALERT_SILENCE_LIST:   function()     { return B() + '/alerts/silences'; },
    ALERT_ROUTING:        function()     { return B() + '/alerts/routing'; },


    CTR_CLONE:            function(n)    { return B() + '/containers/' + enc(n) + '/clone'; },
    CTR_MEMORY_STATS:     function(n)    { return B() + '/containers/' + enc(n) + '/memory-stats'; },
    CTR_HEALTH:           function(n)    { return B() + '/containers/' + enc(n) + '/health'; },


    OVA_IMPORT:           function()     { return B() + '/vms/import/import'; },


    RPC:                  function()     { return B() + '/rpc'; }
  };

  function buildEndpointSurface(edition) {
    (void edition);
    return Object.assign({}, COMMON_ENDPOINTS);
  }

  function applyEditionEndpointSurface(edition) {
    (void edition);
    var surface = buildEndpointSurface('single');
    window.PCV_UI_EDITION = 'single';
    PCV.endpoints = surface;
    window.EP = surface;
    return surface;
  }

  PCV.buildEndpointSurface = buildEndpointSurface;
  PCV.applyEditionEndpointSurface = applyEditionEndpointSurface;
  PCV.getOptionalEndpoint = function(name) {
    var registry = window.EP || {};
    var fn = registry[name];
    if (typeof fn !== 'function') return null;
    return fn.apply(null, Array.prototype.slice.call(arguments, 1));
  };

  return applyEditionEndpointSurface('single');
})();


PCV.endpoints = EP;


window.EP = EP;
})(window.PCV);










































































if (!window.eventLog) window.eventLog = [];

window.PCV = window.PCV || {};
(function(PCV) {





function escapeHtml(s) {
  if (!s) return '';
  return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;').replace(/'/g, '&#39;');
}
var esc = escapeHtml;







function escapeAttr(s) {
  if (!s) return '';
  return String(s).replace(/[^a-zA-Z0-9_.\-]/g, function(c) {
    return '\\x' + c.charCodeAt(0).toString(16).padStart(2, '0');
  });
}

var H = {
  card: (title, body, cls) => `<div class="hc ${cls||''}">${title?'<h4>'+title+'</h4>':''}${body}</div>`,
  row: (key, val, cls) => `<div class="hr"><span class="k">${key}</span><span class="v ${cls||''}">${val}</span></div>`,
  badge: (text, type) => `<span class="badge b-${type}">${escapeHtml(text)}</span>`,
  grid: (cols, content) => `<div class="sg grid-${cols}">${content}</div>`,
  section: (title) => `<h3 class="section-title">${title}</h3>`,
  sectionLg: (title) => `<h3 class="section-title-lg">${title}</h3>`,
};


function toast(m, ok = true) {
  var icon = ok ? '&#9989; ' : '&#10060; ';
  const d = document.createElement('div');
  d.className = 'toast ' + (ok ? 't-ok' : 't-er');
  d.innerHTML = icon + m;
  d.style.cssText = 'transform:translateX(100%);transition:transform 0.3s ease-out';
  requestAnimationFrame(function() { d.style.transform = 'translateX(0)'; });
  d.onclick = function() { d.style.transform = 'translateX(100%)'; setTimeout(function() { d.remove(); }, 300); };
  const prog = document.createElement('div');
  prog.className = 'toast-progress';
  prog.style.cssText = 'height:3px;background:' + (ok ? 'var(--green)' : 'var(--red)') + ';border-radius:0 0 6px 6px;width:100%;transition:width 2.8s linear';
  d.appendChild(prog);
  const container = document.getElementById('toasts');
  container.appendChild(d);
  while (container.children.length > 3) container.removeChild(container.firstChild);
  requestAnimationFrame(() => { prog.style.width = '0%'; });
  setTimeout(() => d.remove(), 3e3);

  if (typeof addNotification === 'function') {
    addNotification(ok ? 'info' : 'error', m, '');
  }
}


function ciIcon(name) {
  return '<svg class="ci-icon" aria-hidden="true"><use href="/ui/vendor/coolicons/coolicons.svg#ci-' + name + '"></use></svg>';
}

var EVT_ICONS = {
  auth: ciIcon('lock'), ws: ciIcon('globe'), vm: ciIcon('desktop-tower'), ctr: ciIcon('layers'), snap: ciIcon('camera'),
  net: ciIcon('globe'), storage: ciIcon('data'), cluster: ciIcon('layers'), ovn: ciIcon('layers'),
  alert: ciIcon('bell'), gpu: ciIcon('monitor'), docker: ciIcon('layers'), terraform: ciIcon('file-document'),
  federation: ciIcon('cloud'), config: ciIcon('settings'), template: ciIcon('file-document'), backup: ciIcon('save'),
  error: ciIcon('close-circle'), ok: ciIcon('circle-check'), info: ciIcon('info')
};

function _evtIcon(m) {
  const ml = m.toLowerCase();
  if (ml.includes('error') || ml.includes('fail')) return EVT_ICONS.error;
  if (ml.includes('auth') || ml.includes('login') || ml.includes('logout')) return EVT_ICONS.auth;
  if (ml.startsWith('ws')) return EVT_ICONS.ws;
  if (ml.includes('ctr ') || ml.includes('container') || ml.includes('lxc')) return EVT_ICONS.ctr;
  if (ml.includes('snapshot') || ml.includes('rollback')) return EVT_ICONS.snap;
  if (ml.includes('cluster') || ml.includes('drain') || ml.includes('maintenance')) return EVT_ICONS.cluster;
  if (ml.includes('network') || ml.includes('ovn') || ml.includes('acl')) return EVT_ICONS.net;
  if (ml.includes('pool') || ml.includes('zvol') || ml.includes('storage')) return EVT_ICONS.storage;
  if (ml.includes('gpu')) return EVT_ICONS.gpu;
  if (ml.includes('docker') || ml.includes('oci')) return EVT_ICONS.docker;
  if (ml.includes('terraform')) return EVT_ICONS.terraform;
  if (ml.includes('federation')) return EVT_ICONS.federation;
  if (ml.includes('config')) return EVT_ICONS.config;
  if (ml.includes('template')) return EVT_ICONS.template;
  if (ml.includes('backup')) return EVT_ICONS.backup;
  if (ml.includes('vm') || ml.includes('start') || ml.includes('stop') || ml.includes('clone')) return EVT_ICONS.vm;
  return EVT_ICONS.info;
}

function _evtClass(m) {
  const ml = m.toLowerCase();
  if (ml.includes('error') || ml.includes('fail')) return 'color-red';
  if (ml.includes('created') || ml.includes('started') || ml.includes('ok') || ml.includes('completed')) return 'color-green';
  if (ml.includes('stopped') || ml.includes('deleted') || ml.includes('destroyed') || ml.includes('drain')) return 'color-yellow';
  return '';
}

function addEvt(m) {
  const ts = new Date().toLocaleTimeString('ko-KR', { hour12: false, hour: '2-digit', minute: '2-digit', second: '2-digit' });
  const ms = String(Date.now() % 1000).padStart(3, '0');
  const entry = { time: ts + '.' + ms, msg: m, raw: ts + '.' + ms + ' ' + m };
  window.eventLog.push(entry);
  if (window.eventLog.length > 200) window.eventLog.shift();
  const ep = document.getElementById('evp');
  if (ep) {
    ep.innerHTML = window.eventLog.map(e => {
      const icon = _evtIcon(e.msg);
      const cls = _evtClass(e.msg);
      return '<div style="padding:2px 0;border-bottom:1px solid var(--border);display:flex;gap:6px;align-items:baseline;font-size:11px">'
        + '<span class="color-muted" style="font-size:9px;white-space:nowrap;min-width:72px">' + escapeHtml(e.time) + '</span>'
        + '<span style="font-size:12px">' + icon + '</span>'
        + '<span class="' + cls + '" style="flex:1;word-break:break-word">' + escapeHtml(e.msg) + '</span></div>';
    }).join('');
    ep.scrollTop = ep.scrollHeight;
  }
  _updateEvFooter();
  if (window._evPopout && !window._evPopout.closed) _syncPopoutLog();
}

function toggleEvPanel() {
  const p = document.getElementById('ev-side');
  if (!p) return;
  const opening = !p.classList.contains('open');
  p.classList.toggle('open', opening);
  document.querySelectorAll('.main, .menubar, .toolbar, .sb-bar').forEach(el => {
    el.classList.toggle('ev-open', opening);
  });
  _updateEvFooter();
}

function _updateEvFooter() {
  const countEl = document.getElementById('ev-count');
  const statusEl = document.getElementById('ev-status');
  if (countEl) countEl.textContent = window.eventLog.length + ' events';
  if (statusEl) statusEl.textContent = window.wsConnection && window.wsConnection.readyState === 1 ? 'Live' : 'Offline';
}

function clearEvts() {
  window.eventLog = [];
  const ep = document.getElementById('evp');
  if (ep) ep.innerHTML = typeof t === 'function' ? t('events.waiting') : 'Waiting...';
  if (window._evPopout && !window._evPopout.closed) _syncPopoutLog();
}

function _syncPopoutLog() {
  const w = window._evPopout;
  if (!w || w.closed) return;
  const body = w.document.getElementById('ev-body');
  const count = w.document.getElementById('ev-count');
  if (!body) return;
  body.innerHTML = window.eventLog.map(e => {
    const icon = _evtIcon(e.msg);
    const cls = _evtClass(e.msg);
    return '<div class="ev-row">'
      + '<span class="color-muted" style="font-size:9px;white-space:nowrap;min-width:72px">' + escapeHtml(e.time) + '</span>'
      + '<span style="font-size:12px">' + icon + '</span>'
      + '<span class="' + cls + '" style="flex:1;word-break:break-word">' + escapeHtml(e.msg) + '</span></div>';
  }).join('');
  body.scrollTop = body.scrollHeight;
  if (count) count.textContent = window.eventLog.length + ' events';
}





var _modalStack = [];

function showModal(h, opts) {
  opts = opts || {};
  var mc = document.getElementById('mc');
  var bg = document.getElementById('mbg');
  if (!opts.replace && mc && mc.innerHTML && bg && !bg.classList.contains('hidden')) {
    _modalStack.push(mc.innerHTML);
  }
  if (mc) mc.innerHTML = h;
  bg?.classList.remove('hidden');
  mc?.focus();
}

function closeModal(force) {
  if (force) {
    _modalStack.length = 0;
    var forcedMc = document.getElementById('mc');
    if (forcedMc) forcedMc.innerHTML = '';
    document.getElementById('mbg')?.classList.add('hidden');
    return;
  }
  if (_modalStack.length > 0) {
    var prev = _modalStack.pop();
    var mc = document.getElementById('mc');
    if (mc) mc.innerHTML = prev;
  } else {
    document.getElementById('mbg')?.classList.add('hidden');
  }
}


function showInputModal(title, label, defaultVal, callback) {
  return new Promise(function(resolve) {
    var id = 'modal-input-' + Date.now();
    var html = '<h2>' + escapeHtml(title) + '</h2>'
      + '<div class="fr"><label>' + escapeHtml(label) + '</label>'
      + '<input id="' + id + '" class="input" value="' + escapeHtml(defaultVal || '') + '" autofocus '
      + 'style="flex:1;padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:6px"></div>'
      + '<div style="text-align:right;margin-top:12px">'
      + '<button class="btn btn-g" id="' + id + '-ok">' + (typeof t === 'function' ? t('btn.confirm') : 'OK') + '</button> '
      + '<button class="btn btn-r" id="' + id + '-cancel">' + (typeof t === 'function' ? t('btn.cancel') : 'Cancel') + '</button></div>';
    showModal(html);
    setTimeout(function() {
      var inp = document.getElementById(id);
      var okBtn = document.getElementById(id + '-ok');
      var cancelBtn = document.getElementById(id + '-cancel');
      if (inp) inp.focus();
      function doOk() {
        var val = inp ? inp.value.trim() : '';
        closeModal();
        if (callback) callback(val);
        resolve(val);
      }
      function doCancel() {
        closeModal();
        if (callback) callback(null);
        resolve(null);
      }
      if (okBtn) okBtn.addEventListener('click', doOk);
      if (cancelBtn) cancelBtn.addEventListener('click', doCancel);
      if (inp) inp.addEventListener('keydown', function(e) {
        if (e.key === 'Enter') { e.preventDefault(); doOk(); }
        if (e.key === 'Escape') { e.preventDefault(); doCancel(); }
      });
    }, 50);
  });
}


function customConfirm(title, message) {
  return new Promise(resolve => {
    const tFn = typeof t === 'function' ? t : k => k;
    const body = escapeHtml(message || '').replace(/\n/g, '<br>');
    const html = `<h2>${escapeHtml(title)}</h2><p class="mb-12">${body}</p><div style="text-align:right"><button class="btn btn-r" onclick="document.getElementById('mbg').classList.add('hidden');window._confirmResolve(true)">${tFn('btn.confirm')}</button> <button class="btn" onclick="document.getElementById('mbg').classList.add('hidden');window._confirmResolve(false)">${tFn('btn.cancel')}</button></div>`;
    window._confirmResolve = resolve;
    showModal(html);
  });
}


function renderProgressBar(p, c) {
  const cl = p > 85 ? 'var(--red)' : p > 60 ? 'var(--yellow)' : 'var(--green)';
  const anim = p > 85 ? ' pulse-anim' : '';
  return `<div class="pb${anim}"><div class="pb-f scan-anim" style="width:${p}%;background:${c || cl}"></div><div class="pb-t">${p.toFixed(1)}%</div></div>`;
}

function formatBytes(b) {
  if (!b || b < 1024) return (b || 0) + 'B';
  if (b < 1048576) return (b / 1024).toFixed(1) + 'K';
  if (b < 1073741824) return (b / 1048576).toFixed(1) + 'M';
  return (b / 1073741824).toFixed(2) + 'G';
}

function parseSize(s) {
  if (!s || typeof s === 'number') return s || 0;
  const m = String(s).trim().match(/^([\d.]+)\s*([TGMK]?)(?:I?B)?$/i);
  if (!m) return 0;
  const v = parseFloat(m[1]), u = (m[2] || '').toUpperCase();
  if (u === 'T') return v * 1099511627776;
  if (u === 'G') return v * 1073741824;
  if (u === 'M') return v * 1048576;
  if (u === 'K') return v * 1024;
  return v;
}

function showSkeleton(container, count) {
  let h = '';
  for (let i = 0; i < (count || 3); i++) {
    h += '<div class="hc skeleton" style="height:80px;margin-bottom:8px;background:var(--bg3);border-radius:var(--r);animation:skeleton-pulse 1.5s ease infinite"></div>';
  }
  if (typeof container === 'string') {
    const el = document.getElementById(container);
    if (el) el.innerHTML = h;
  } else if (container) {
    container.innerHTML = h;
  }
  return h;
}

function renderSortableTable(containerId, headers, rows, options) {
  const opts = options || {};
  const el = typeof containerId === 'string' ? document.getElementById(containerId) : containerId;
  if (!el) return;
  let h = '<table><thead><tr>';
  headers.forEach(hdr => { h += '<th>' + (typeof hdr === 'string' ? hdr : hdr.label) + '</th>'; });
  h += '</tr></thead><tbody>';
  if (rows.length === 0) {
    h += '<tr><td colspan="' + headers.length + '" class="text-center color-muted">' + (opts.emptyText || 'No data') + '</td></tr>';
  }
  rows.forEach(row => { h += '<tr>'; row.forEach(cell => { h += '<td>' + cell + '</td>'; }); h += '</tr>'; });
  h += '</tbody></table>';
  el.innerHTML = h;
}






function createDataTable(containerId, config) {
  var el = typeof containerId === 'string' ? document.getElementById(containerId) : containerId;
  if (!el) return;
  var cfg = config || {};
  var headers = cfg.headers || [];
  var rows = cfg.rows || [];
  var tableId = 'dt-' + Math.random().toString(36).substr(2, 6);
  var sortCol = -1, sortDir = 1;
  var tFn = typeof t === 'function' ? t : function(k) { return k; };

  function renderTable(filteredRows) {
    var h = '';

    var pageSize = cfg.pageSize || 0;
    var dt = window['_dt_' + tableId];
    var currentPage = (dt && dt.currentPage) ? dt.currentPage : 1;
    var totalPages = pageSize > 0 ? Math.ceil(filteredRows.length / pageSize) : 1;
    if (currentPage > totalPages) currentPage = totalPages || 1;
    var displayRows = filteredRows;
    if (pageSize > 0) {
      var start = (currentPage - 1) * pageSize;
      displayRows = filteredRows.slice(start, start + pageSize);
    }
    if (cfg.searchable) {
      h += '<div class="flex gap-8 items-center mb-8"><input id="' + tableId + '-search" class="sb-search" placeholder="' + tFn('search') + '" oninput="window._dtFilter(\'' + tableId + '\')" style="max-width:300px;font-size:12px;padding:6px 10px;border-radius:4px">';
      if (cfg.exportable) h += '<button class="btn" style="font-size:10px;padding:3px 8px" onclick="window._dtExport(\'' + tableId + '\')">CSV</button>';
      h += '<span class="color-muted text-xs">' + filteredRows.length + ' rows</span></div>';
    }
    h += '<table id="' + tableId + '-table"><thead><tr>';
    headers.forEach(function(hdr, ci) {
      var dtx = window['_dt_' + tableId];
      var sc = dtx ? dtx.sortCol : sortCol;
      var sd = dtx ? dtx.sortDir : sortDir;
      var arrow = sc === ci ? (sd > 0 ? ' &#9650;' : ' &#9660;') : '';
      var sortAttr = hdr.sortable !== false ? ' style="cursor:pointer" onclick="window._dtSort(\'' + tableId + '\',' + ci + ')"' : '';
      h += '<th' + sortAttr + '>' + (hdr.label || hdr.key || '') + arrow + '</th>';
    });
    h += '</tr></thead><tbody>';
    if (displayRows.length === 0) {
      h += '<tr><td colspan="' + headers.length + '" class="text-center color-muted">' + (cfg.emptyText || 'No data') + '</td></tr>';
    }
    displayRows.forEach(function(row) {
      h += '<tr>'; row.forEach(function(cell) { h += '<td>' + cell + '</td>'; }); h += '</tr>';
    });
    h += '</tbody></table>';

    if (pageSize > 0 && totalPages > 1) {
      h += '<div class="flex items-center gap-8 mt-8">';
      h += '<button class="btn btn-sm" ' + (currentPage <= 1 ? 'disabled' : '') + ' onclick="window._dtPage(\'' + tableId + '\',' + (currentPage - 1) + ')">Prev</button>';
      h += '<span class="stat-label">Page ' + currentPage + '/' + totalPages + '</span>';
      h += '<button class="btn btn-sm" ' + (currentPage >= totalPages ? 'disabled' : '') + ' onclick="window._dtPage(\'' + tableId + '\',' + (currentPage + 1) + ')">Next</button>';
      h += '</div>';
    }
    el.innerHTML = h;

    if (dt) dt.currentPage = currentPage;
  }

  window['_dt_' + tableId] = { headers: headers, rows: rows, sortCol: sortCol, sortDir: sortDir, currentPage: 1, el: el, cfg: cfg, renderTable: renderTable };
  renderTable(rows);
}

function _dtSort(id, col) {
  var dt = window['_dt_' + id]; if (!dt) return;
  if (dt.sortCol === col) dt.sortDir *= -1; else { dt.sortCol = col; dt.sortDir = 1; }
  var sorted = dt.rows.slice().sort(function(a, b) {
    var va = (a[col] || '').toString().replace(/<[^>]+>/g, '').toLowerCase();
    var vb = (b[col] || '').toString().replace(/<[^>]+>/g, '').toLowerCase();
    var na = parseFloat(va), nb = parseFloat(vb);
    if (!isNaN(na) && !isNaN(nb)) return (na - nb) * dt.sortDir;
    return (va < vb ? -1 : va > vb ? 1 : 0) * dt.sortDir;
  });
  dt.renderTable(sorted);
}

function _dtFilter(id) {
  var dt = window['_dt_' + id]; if (!dt) return;
  var searchEl = document.getElementById(id + '-search');
  var q = (searchEl ? searchEl.value : '').toLowerCase();
  var filtered = q ? dt.rows.filter(function(row) { return row.some(function(cell) { return (cell || '').toString().replace(/<[^>]+>/g, '').toLowerCase().indexOf(q) !== -1; }); }) : dt.rows;
  dt.renderTable(filtered);
}

function _dtExport(id) {
  var dt = window['_dt_' + id]; if (!dt) return;
  var csv = dt.headers.map(function(h) { return h.label || h.key || ''; }).join(',') + '\n';
  dt.rows.forEach(function(row) { csv += row.map(function(cell) { return '"' + (cell || '').toString().replace(/<[^>]+>/g, '').replace(/"/g, '""') + '"'; }).join(',') + '\n'; });
  var blob = new Blob([csv], { type: 'text/csv' });
  var a = document.createElement('a'); a.href = URL.createObjectURL(blob); a.download = 'purecvisor-export.csv'; a.click();
}

function _dtPage(id, page) {
  var dt = window['_dt_' + id]; if (!dt) return;
  dt.currentPage = page;

  var searchEl = document.getElementById(id + '-search');
  var q = (searchEl ? searchEl.value : '').toLowerCase();
  var filtered = q ? dt.rows.filter(function(row) { return row.some(function(cell) { return (cell || '').toString().replace(/<[^>]+>/g, '').toLowerCase().indexOf(q) !== -1; }); }) : dt.rows;

  if (dt.sortCol >= 0) {
    filtered = filtered.slice().sort(function(a, b) {
      var va = (a[dt.sortCol] || '').toString().replace(/<[^>]+>/g, '').toLowerCase();
      var vb = (b[dt.sortCol] || '').toString().replace(/<[^>]+>/g, '').toLowerCase();
      var na = parseFloat(va), nb = parseFloat(vb);
      if (!isNaN(na) && !isNaN(nb)) return (na - nb) * dt.sortDir;
      return (va < vb ? -1 : va > vb ? 1 : 0) * dt.sortDir;
    });
  }
  dt.renderTable(filtered);
}

async function fetchWithRetry(fn, retries) {
  const maxRetries = retries || 2;
  for (let attempt = 0; attempt <= maxRetries; attempt++) {
    try { return await fn(); }
    catch (e) { if (attempt === maxRetries) throw e; await new Promise(r => setTimeout(r, 1000 * (attempt + 1))); }
  }
}


var _DEBUG = localStorage.getItem('pcv-debug') === 'true';


function safeAsync(fn, fallbackMsg) {
  return async function() {
    try { return await fn.apply(this, arguments); }
    catch (e) {
      if(_DEBUG) console.warn('safeAsync error:', e.message);
      toast((fallbackMsg || _L('작업 오류', 'Operation error')) + ': ' + (e.message || ''), false);
    }
  };
}


function getFavorites() {
  try { return JSON.parse(localStorage.getItem('pcv-favorites') || '[]'); } catch(e) { return []; }
}
function toggleFavorite(name) {
  let favs = getFavorites();
  if (favs.includes(name)) favs = favs.filter(f => f !== name);
  else favs.push(name);
  localStorage.setItem('pcv-favorites', JSON.stringify(favs));
  window.render();
}
function isFavorite(name) { return getFavorites().includes(name); }


function popoutEventLog() {
  const w = window.open('', 'pcv-event-log', 'width=700,height=500,menubar=no,toolbar=no,location=no,status=no');
  if (!w) { toast(t('msg.popup_blocked'), false); return; }
  window._evPopout = w;
  const theme = document.documentElement.getAttribute('data-theme') || '';
  w.document.write('<!DOCTYPE html><html lang="ko"><head><meta charset="UTF-8"><title>PureCVisor — Event Log</title>'
    + '<link rel="stylesheet" href="/ui/style.css">'
    + '<style>body{margin:0;padding:0;background:var(--bg);color:var(--fg);font-family:var(--font-mono);font-size:12px;overflow:hidden;display:flex;flex-direction:column;height:100vh}'
    + '.ev-toolbar{padding:8px 12px;border-bottom:1px solid var(--border);display:flex;justify-content:space-between;align-items:center;background:var(--bg2)}'
    + '.ev-body{flex:1;overflow-y:auto;padding:6px 10px}'
    + '.ev-row{padding:2px 0;border-bottom:1px solid var(--border);display:flex;gap:6px;align-items:baseline;font-size:11px}'
    + '</style></head><body' + (theme ? ' data-theme="' + theme + '"' : '') + '>'
    + '<div class="ev-toolbar"><span style="font-weight:700">&#128220; PureCVisor Event Log</span>'
    + '<div style="display:flex;gap:6px"><button class="btn" style="font-size:10px;padding:3px 8px" onclick="parent.clearEvts()">Clear</button>'
    + '<span id="ev-count" class="color-muted" style="font-size:10px"></span></div></div>'
    + '<div class="ev-body" id="ev-body"></div>'
    + '</body></html>');
  w.document.close();
  _syncPopoutLog();
}


document.addEventListener('keydown', function(e) {
  var modal = document.querySelector('#mbg:not(.hidden)');
  if (!modal || e.key !== 'Tab') return;
  var focusable = modal.querySelectorAll('button, [href], input, select, textarea, [tabindex]:not([tabindex="-1"])');
  if (focusable.length === 0) return;
  var first = focusable[0], last = focusable[focusable.length - 1];
  if (e.shiftKey) {
    if (document.activeElement === first) { e.preventDefault(); last.focus(); }
  } else {
    if (document.activeElement === last) { e.preventDefault(); first.focus(); }
  }
});


function emptyState(icon, msg) {
  return '<div class="empty-state" style="padding:40px;text-align:center" role="status">'
    + '<div style="font-size:42px;opacity:.4">' + icon + '</div>'
    + '<div class="color-muted mt-8">' + msg + '</div></div>';
}


async function withSpinner(btn, asyncFn) {
  if (!btn) { await asyncFn(); return; }

  if (btn.dataset.pcvBusy === '1') return;
  btn.dataset.pcvBusy = '1';
  btn.classList.add('is-loading');
  btn.disabled = true;
  btn.setAttribute('aria-busy', 'true');
  try { await asyncFn(); }
  catch (e) {
    if (typeof reportError === 'function') reportError('action', e);
    throw e;
  }
  finally {
    btn.disabled = false;
    btn.classList.remove('is-loading');
    btn.removeAttribute('aria-busy');
    delete btn.dataset.pcvBusy;
  }
}


function emptyStatePro(opts) {

  var i = opts.icon || '&#128230;';
  var t = escapeHtml(opts.title || 'No items');
  var d = escapeHtml(opts.desc || '');
  var btn = '';
  if (opts.ctaLabel && opts.ctaAction) {
    btn = '<button class="btn" onclick="' + escapeHtml(opts.ctaAction) + '">' + escapeHtml(opts.ctaLabel) + '</button>';
  }
  return '<div class="empty-state"><div class="empty-icon">' + i + '</div>' +
    '<div class="empty-title">' + t + '</div>' +
    '<div class="empty-desc">' + d + '</div>' + btn + '</div>';
}


function statusBadge(text, kind) {

  return '<span class="status-badge s-' + (kind || 'off') + '">' + escapeHtml(text) + '</span>';
}


document.addEventListener('keydown', function(e) {
  if (e.key === 'Enter' || e.key === ' ') {
    var tgt = e.target;
    if (tgt && (tgt.getAttribute('role') === 'link' || tgt.getAttribute('role') === 'button') && tgt.tabIndex >= 0) {
      e.preventDefault();
      tgt.click();
    }
  }
});


PCV.ui = {
  escapeHtml: escapeHtml,
  escapeAttr: escapeAttr,
  esc: escapeHtml,
  H: H,
  toast: toast,
  addEvt: addEvt,
  toggleEvPanel: toggleEvPanel,
  clearEvts: clearEvts,
  showModal: showModal,
  closeModal: closeModal,
  customConfirm: customConfirm,
  showInputModal: showInputModal,
  renderProgressBar: renderProgressBar,
  formatBytes: formatBytes,
  parseSize: parseSize,
  showSkeleton: showSkeleton,
  renderSortableTable: renderSortableTable,
  createDataTable: createDataTable,
  fetchWithRetry: fetchWithRetry,
  getFavorites: getFavorites,
  toggleFavorite: toggleFavorite,
  isFavorite: isFavorite,
  popoutEventLog: popoutEventLog,
  safeAsync: safeAsync,
  emptyState: emptyState,
  emptyStatePro: emptyStatePro,
  statusBadge: statusBadge,
  withSpinner: withSpinner,
  _modalStack: _modalStack
};


window.H = H;
window.escapeHtml = escapeHtml;
window.esc = esc;
window.escapeAttr = escapeAttr;
window.toast = toast;
window.addEvt = addEvt;
window.toggleEvPanel = toggleEvPanel;
window.clearEvts = clearEvts;
window.showModal = showModal;
window.closeModal = closeModal;
window.showM = showModal;
window.closeM = closeModal;
window.customConfirm = customConfirm;
window.showInputModal = showInputModal;
window.renderProgressBar = renderProgressBar;
window.formatBytes = formatBytes;
window.parseSize = parseSize;
window.showSkeleton = showSkeleton;
window.renderSortableTable = renderSortableTable;
window.createDataTable = createDataTable;
window._dtSort = _dtSort;
window._dtFilter = _dtFilter;
window._dtExport = _dtExport;
window._dtPage = _dtPage;
window.fetchWithRetry = fetchWithRetry;
window.getFavorites = getFavorites;
window.toggleFavorite = toggleFavorite;
window.isFavorite = isFavorite;
window.popoutEventLog = popoutEventLog;
window.safeAsync = safeAsync;
window._DEBUG = _DEBUG;
window.emptyState = emptyState;
window.emptyStatePro = emptyStatePro;
window.statusBadge = statusBadge;
window.withSpinner = withSpinner;

})(window.PCV);














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


  var _modalEscBound = false;
  function bindGlobalModalEsc() {
    if (_modalEscBound) return;
    _modalEscBound = true;
    document.addEventListener('keydown', function (e) {
      if (e.key !== 'Escape') return;

      if (window.Modal && Modal._isOpen && Modal._isOpen()) { Modal.close(); return; }
      var m = document.getElementById('modal-bg');
      if (m && m.style.display !== 'none' && typeof closeModal === 'function') closeModal();
    });
  }
  window.bindGlobalModalEsc = bindGlobalModalEsc;


  function destroyConfirm(opts) {

    var title = opts.title || '삭제 확인';
    var name = opts.name || '';
    var warn = opts.warning || '이 작업은 되돌릴 수 없습니다.';
    var html = '<h2 style="color:var(--red)">&#9888; ' + escapeHtml(title) + '</h2>' +
      '<p class="color-yellow">' + escapeHtml(warn) + '</p>' +
      '<p class="text-13">계속하려면 <code class="color-red">' + escapeHtml(name) + '</code> 을(를) 정확히 입력하세요:</p>' +
      '<input id="dc-input" class="login-input" style="width:100%;margin:8px 0" autocomplete="off">' +
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


  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', bindGlobalModalEsc);
  } else {
    bindGlobalModalEsc();
  }


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


  function renderBreadcrumbs(items) {

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
    setHashRoute: setHashRoute
  };
})(window.PCV);






























window.PCV = window.PCV || {};
(function(PCV) {
  var overlay = null;
  var prevFocus = null;
  var currentOpts = null;

  function _t(key, fallback) {
    if (typeof t === 'function') {
      var val = t(key);
      if (val != null && val !== '') return val;
    }
    return fallback || key;
  }

  function _ensureOverlay() {
    if (overlay) return overlay;
    overlay = document.createElement('div');
    overlay.id = 'pcv-modal-overlay';
    overlay.setAttribute('role', 'dialog');
    overlay.setAttribute('aria-modal', 'true');
    overlay.style.cssText =
      'display:none;position:fixed;inset:0;background:rgba(0,0,0,0.7);' +
      'z-index:10000;align-items:center;justify-content:center;backdrop-filter:blur(4px)';
    overlay.addEventListener('click', function(e) {
      if (e.target === overlay) Modal.close();
    });
    document.body.appendChild(overlay);

    document.addEventListener('keydown', function(e) {
      if (overlay.style.display !== 'flex') return;
      if (e.key === 'Escape') { Modal.close(); }
      if (e.key === 'Tab') { _trapFocus(e); }
    });
    return overlay;
  }

  function _trapFocus(e) {
    var focusables = overlay.querySelectorAll(
      'input,button,select,textarea,a[href],[tabindex]:not([tabindex="-1"])'
    );
    if (focusables.length === 0) return;
    var first = focusables[0], last = focusables[focusables.length - 1];
    if (e.shiftKey && document.activeElement === first) {
      e.preventDefault(); last.focus();
    } else if (!e.shiftKey && document.activeElement === last) {
      e.preventDefault(); first.focus();
    }
  }

  function _esc(s) {
    return String(s == null ? '' : s)
      .replace(/&/g, '&amp;').replace(/</g, '&lt;')
      .replace(/>/g, '&gt;').replace(/"/g, '&quot;');
  }

  var Modal = {
    show: function(opts) {
      currentOpts = opts || {};
      _ensureOverlay();
      prevFocus = document.activeElement;

      var width = (opts.width || 360) + 'px';
      var actionsHtml = '';
      if (opts.actions && opts.actions.length) {
        actionsHtml = '<div style="display:flex;gap:8px;justify-content:flex-end;margin-top:16px">';
        opts.actions.forEach(function(a, i) {
          var bg = a.primary ? '' :
                   a.danger  ? 'background:var(--red);color:#fff' :
                              'background:var(--bg3)';
          var cls = a.primary ? 'btn login-btn' : a.danger ? 'btn btn-r' : 'btn';
          actionsHtml += '<button class="' + cls + '" data-pcv-action="' + i + '" ' +
                         'style="margin:0;' + bg + '">' + _esc(a.label) + '</button>';
        });
        actionsHtml += '</div>';
      }

      var bodyHtml = (typeof opts.body === 'string') ? opts.body : '';

      overlay.innerHTML =
        '<div role="document" style="background:var(--bg2);border:1px solid var(--accent);' +
        'border-radius:8px;padding:24px;min-width:' + width + ';max-width:90vw;' +
        'box-shadow:0 0 24px rgba(0,240,255,0.2);color:var(--fg)">' +
          '<div style="font-size:18px;font-weight:bold;color:var(--accent);' +
          'margin-bottom:16px;display:flex;justify-content:space-between;align-items:center">' +
            '<span>' + _esc(opts.title || '') + '</span>' +
            '<button id="pcv-modal-close-x" aria-label="Close" ' +
            'style="background:none;border:none;color:var(--fg2);font-size:20px;cursor:pointer;padding:0 4px">&times;</button>' +
          '</div>' +
          '<div id="pcv-modal-body">' + bodyHtml + '</div>' +
          actionsHtml +
        '</div>';


      if (opts.body && typeof opts.body !== 'string' && opts.body.nodeType) {
        var bodyEl = overlay.querySelector('#pcv-modal-body');
        bodyEl.innerHTML = '';
        bodyEl.appendChild(opts.body);
      }


      overlay.querySelector('#pcv-modal-close-x').addEventListener('click', Modal.close);


      if (opts.actions) {
        var btns = overlay.querySelectorAll('[data-pcv-action]');
        btns.forEach(function(btn) {
          btn.addEventListener('click', function() {
            var idx = parseInt(btn.getAttribute('data-pcv-action'), 10);
            var a = opts.actions[idx];
            if (a && typeof a.onClick === 'function') {
              var keep = a.onClick();
              if (keep !== false) Modal.close();
            } else {
              Modal.close();
            }
          });
        });
      }

      overlay.style.display = 'flex';

      setTimeout(function() {
        var first = overlay.querySelector('input,select,textarea,button[data-pcv-action]');
        if (first) first.focus();
      }, 50);
    },

    close: function() {
      if (!overlay) return;
      overlay.style.display = 'none';
      overlay.innerHTML = '';
      if (currentOpts && typeof currentOpts.onClose === 'function') {
        try { currentOpts.onClose(); } catch (e) {}
      }
      currentOpts = null;
      if (prevFocus && prevFocus.focus) { try { prevFocus.focus(); } catch (e) {} }
    },

    setMessage: function(text, type) {
      var el = overlay && overlay.querySelector('#pcv-modal-msg');
      if (!el) return;
      el.textContent = text || '';
      el.className = 'text-sm';
      if (type === 'error') el.style.color = 'var(--red)';
      else if (type === 'success') el.style.color = 'var(--accent)';
      else el.style.color = '';
    },


    showRegister: function() {
      var body =
        '<div style="margin-bottom:12px"><input id="pcv-reg-user" class="login-input" ' +
        'placeholder="' + _t('register.user_ph', 'Username (3-32, a-z 0-9 _)') + '" ' +
        'autocomplete="username" style="width:100%"></div>' +
        '<div style="margin-bottom:12px"><input id="pcv-reg-pass" type="password" class="login-input" ' +
        'placeholder="' + _t('register.pass_ph', 'Password (8자 이상)') + '" ' +
        'autocomplete="new-password" style="width:100%"></div>' +
        '<div style="margin-bottom:16px"><input id="pcv-reg-pass2" type="password" class="login-input" ' +
        'placeholder="' + _t('register.pass2_ph', 'Password 확인') + '" ' +
        'autocomplete="new-password" style="width:100%"></div>' +
        '<div id="pcv-modal-msg" class="text-sm" style="min-height:18px" role="alert" aria-live="assertive"></div>' +
        '<div class="text-xs color-muted mt-12" style="text-align:center">' +
        _t('register.note', '가입 후 기본 권한: VIEWER (조회 전용). 관리자가 권한 승격 가능.') + '</div>';

      Modal.show({
        title: _t('register.title', '회원가입'),
        body: body,
        width: 360,
        actions: [
          { label: _t('btn.cancel', '취소') },
          { label: _t('btn.register', '가입'), primary: true, onClick: function() {
            var u = document.getElementById('pcv-reg-user').value.trim();
            var p = document.getElementById('pcv-reg-pass').value;
            var p2 = document.getElementById('pcv-reg-pass2').value;
            if (!/^[a-z0-9_]{3,32}$/.test(u)) { Modal.setMessage(_t('register.err_user', '사용자명은 3-32자 (a-z, 0-9, _)'), 'error'); return false; }
            if (p.length < 8) { Modal.setMessage(_t('register.err_pass', '비밀번호는 8자 이상'), 'error'); return false; }
            if (p !== p2) { Modal.setMessage(_t('register.err_match', '비밀번호 확인이 일치하지 않습니다'), 'error'); return false; }
            if (u === p) { Modal.setMessage(_t('register.err_same', '비밀번호는 사용자명과 달라야 합니다'), 'error'); return false; }
            Modal.setMessage(_t('msg.processing', '처리 중...'));
            fetch(EP.AUTH_REGISTER(), {
              method: 'POST',
              headers: { 'Content-Type': 'application/json' },
              body: JSON.stringify({ username: u, password: p })
            }).then(function(r) { return r.json().then(function(d) { return { status: r.status, body: d }; }); })
              .then(function(res) {
                if (res.status === 201) {
                  Modal.setMessage(_t('register.ok', '가입 성공! 로그인 화면으로 돌아갑니다.'), 'success');
                  setTimeout(function() {
                    Modal.close();
                    var lu = document.getElementById('login-user');
                    var lp = document.getElementById('login-pass');
                    if (lu) lu.value = u;
                    if (lp) lp.focus();
                  }, 1200);
                } else {
                  var em = (res.body && res.body.error && res.body.error.message) || ('HTTP ' + res.status);
                  if (res.status === 403) em = _t('register.err_disabled', '회원가입이 비활성화되어 있습니다 (관리자에게 문의)');
                  Modal.setMessage(em, 'error');
                }
              })
              .catch(function(e) { Modal.setMessage((_t('msg.network_err', '네트워크 오류') + ': ' + e.message), 'error'); });
            return false;
          }}
        ]
      });
    },


    showChangePassword: function() {
      var body =
        '<div style="margin-bottom:12px"><input id="pcv-cp-old" type="password" class="login-input" ' +
        'placeholder="' + _t('changepw.old_ph', '현재 비밀번호') + '" ' +
        'autocomplete="current-password" style="width:100%"></div>' +
        '<div style="margin-bottom:12px"><input id="pcv-cp-new" type="password" class="login-input" ' +
        'placeholder="' + _t('changepw.new_ph', '새 비밀번호 (8자 이상)') + '" ' +
        'autocomplete="new-password" style="width:100%"></div>' +
        '<div style="margin-bottom:16px"><input id="pcv-cp-new2" type="password" class="login-input" ' +
        'placeholder="' + _t('changepw.new2_ph', '새 비밀번호 확인') + '" ' +
        'autocomplete="new-password" style="width:100%"></div>' +
        '<div id="pcv-modal-msg" class="text-sm" style="min-height:18px" role="alert" aria-live="assertive"></div>' +
        '<div class="text-xs color-muted mt-12" style="text-align:center">' +
        _t('changepw.note', '변경 후 다른 디바이스의 모든 세션이 자동 로그아웃됩니다.') + '</div>';

      Modal.show({
        title: '\uD83D\uDD12 ' + _t('changepw.title', '비밀번호 변경'),
        body: body,
        width: 360,
        actions: [
          { label: _t('btn.cancel', '취소') },
          { label: _t('btn.change', '변경'), primary: true, onClick: function() {
            var op = document.getElementById('pcv-cp-old').value;
            var np = document.getElementById('pcv-cp-new').value;
            var np2 = document.getElementById('pcv-cp-new2').value;
            if (!op) { Modal.setMessage(_t('changepw.err_old', '현재 비밀번호를 입력하세요'), 'error'); return false; }
            if (np.length < 8) { Modal.setMessage(_t('changepw.err_short', '새 비밀번호는 8자 이상'), 'error'); return false; }
            if (np !== np2) { Modal.setMessage(_t('changepw.err_match', '새 비밀번호 확인이 일치하지 않습니다'), 'error'); return false; }
            if (op === np) { Modal.setMessage(_t('changepw.err_same', '새 비밀번호는 현재와 달라야 합니다'), 'error'); return false; }
            Modal.setMessage(_t('msg.processing', '처리 중...'));
            var token = sessionStorage.getItem('pcv_token') || sessionStorage.getItem('access_token') || '';
            fetch(EP.AUTH_PASSWORD(), {
              method: 'POST',
              headers: { 'Content-Type': 'application/json', 'Authorization': 'Bearer ' + token },
              body: JSON.stringify({ old_password: op, new_password: np })
            }).then(function(r) { return r.json().then(function(d) { return { status: r.status, body: d }; }); })
              .then(function(res) {
                if (res.status === 200) {
                  Modal.setMessage(_t('changepw.ok', '변경 완료. 3초 후 다시 로그인합니다.'), 'success');
                  setTimeout(function() { sessionStorage.clear(); location.reload(); }, 3000);
                } else {
                  var em = (res.body && res.body.error && res.body.error.message) || ('HTTP ' + res.status);
                  Modal.setMessage(em, 'error');
                }
              })
              .catch(function(e) { Modal.setMessage(_t('msg.network_err', '네트워크 오류') + ': ' + e.message, 'error'); });
            return false;
          }}
        ]
      });
    }
  };


  PCV.modal = Modal;


  window.Modal = Modal;
  window.showRegisterModal = function() { Modal.showRegister(); };
  window.showChangePwModal = function() { Modal.showChangePassword(); };
})(window.PCV);







window.PCV = window.PCV || {};
(function(PCV) {


var pcvCharts = {};
window.pcvCharts = pcvCharts;


var PCV_CHART_COLORS = [
  '#00f0ff', '#00ff88', '#ff00aa', '#ffee00',
  '#ff6600', '#a78bfa', '#22d3ee', '#f472b6'
];

function pcvChartTheme() {

  var s = getComputedStyle(document.documentElement);
  return {
    fg: (s.getPropertyValue('--fg') || '#e0f0ff').trim(),
    fg2: (s.getPropertyValue('--fg2') || '#5a6a8a').trim(),
    border: (s.getPropertyValue('--border') || '#1a1a3a').trim(),
    bg2: (s.getPropertyValue('--bg2') || '#0a0a14').trim()
  };
}




function pcvDestroyChart(id) {
  if (pcvCharts[id]) {
    try { pcvCharts[id].destroy(); } catch (e) {}
    delete pcvCharts[id];
  }
}
window.pcvDestroyChart = pcvDestroyChart;









function pcvTimeSeries(canvasId, series, opts) {
  if (typeof Chart === 'undefined') return;
  var el = document.getElementById(canvasId);
  if (!el) return;
  opts = opts || {};
  var theme = pcvChartTheme();
  var maxLen = Math.max.apply(null, series.map(function(s){return s.data.length;}).concat([1]));
  var labels = [];
  for (var i = 0; i < maxLen; i++) {
    var ago = (maxLen - 1 - i) * 5;
    labels.push(ago === 0 ? 'now' : '-' + ago + 's');
  }

  var datasets = series.map(function(s, idx) {
    var c = s.color || PCV_CHART_COLORS[idx % PCV_CHART_COLORS.length];

    var pr = s.data.length <= 2 ? 3 : 0;
    return {
      label: s.label,
      data: s.data,
      borderColor: c,
      backgroundColor: opts.fill ? c + '33' : 'transparent',
      borderWidth: 2,
      pointRadius: pr,
      pointHoverRadius: 4,
      tension: 0.3,
      fill: !!opts.fill
    };
  });


  if (pcvCharts[canvasId]) {
    var ch = pcvCharts[canvasId];
    if (ch.canvas && document.body.contains(ch.canvas) && ch.canvas === el) {
      ch.data.labels = labels;

      ch.data.datasets = datasets;
      ch.update('none');
      return ch;
    }

    try { ch.destroy(); } catch (e) {}
    delete pcvCharts[canvasId];
  }

  if (opts.height) el.style.height = opts.height + 'px';

  var config = {
    type: 'line',
    data: { labels: labels, datasets: datasets },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: { duration: 0 },
      interaction: { mode: 'index', intersect: false },
      plugins: {
        legend: {
          display: series.length > 1,
          position: 'top',
          align: 'end',
          labels: { color: theme.fg, font: { size: 10 }, boxWidth: 10, padding: 8 }
        },
        title: opts.title ? {
          display: true, text: opts.title, color: theme.fg,
          font: { size: 12, weight: 'bold' }, padding: { bottom: 8 }
        } : { display: false },
        tooltip: {
          backgroundColor: theme.bg2,
          titleColor: theme.fg,
          bodyColor: theme.fg,
          borderColor: theme.border,
          borderWidth: 1,
          callbacks: {
            label: function(ctx) {
              return ctx.dataset.label + ': ' + ctx.parsed.y.toFixed(1) + (opts.unit || '');
            }
          }
        }
      },
      scales: {
        x: {
          grid: { color: theme.border, drawBorder: false },
          ticks: { color: theme.fg2, font: { size: 9 }, maxTicksLimit: 6 }
        },
        y: {
          grid: { color: theme.border, drawBorder: false },
          ticks: {
            color: theme.fg2,
            font: { size: 9 },
            callback: function(v) { return v + (opts.unit || ''); }
          },
          min: 0,
          max: opts.max || undefined
        }
      }
    }
  };

  pcvCharts[canvasId] = new Chart(el, config);
  return pcvCharts[canvasId];
}
window.pcvTimeSeries = pcvTimeSeries;





function pcvDoughnut(canvasId, value, max, opts) {
  if (typeof Chart === 'undefined') return;
  var el = document.getElementById(canvasId);
  if (!el) return;
  opts = opts || {};
  var theme = pcvChartTheme();
  var pct = Math.min(100, Math.max(0, (value / max) * 100));
  var color = opts.color ||
    (pct > 90 ? '#ff2266' : pct > 75 ? '#ffee00' : '#00ff88');

  if (pcvCharts[canvasId]) {
    var ch = pcvCharts[canvasId];
    ch.data.datasets[0].data = [value, Math.max(0, max - value)];
    ch.data.datasets[0].backgroundColor[0] = color;
    ch.update('none');
    return ch;
  }

  if (opts.height) el.style.height = opts.height + 'px';

  pcvCharts[canvasId] = new Chart(el, {
    type: 'doughnut',
    data: {
      datasets: [{
        data: [value, Math.max(0, max - value)],
        backgroundColor: [color, theme.border],
        borderWidth: 0,
        cutout: '75%'
      }]
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: { duration: 300 },
      plugins: { legend: { display: false }, tooltip: { enabled: false } }
    }
  });
  return pcvCharts[canvasId];
}
window.pcvDoughnut = pcvDoughnut;





function pcvDestroyAllInContainer(parentEl) {
  Object.keys(pcvCharts).forEach(function(id) {
    var el = document.getElementById(id);
    if (!el || (parentEl && !parentEl.contains(el))) {
      pcvDestroyChart(id);
    }
  });
}
window.pcvDestroyAllInContainer = pcvDestroyAllInContainer;


PCV.charts = {
  instances: pcvCharts,
  colors: PCV_CHART_COLORS,
  theme: pcvChartTheme,
  destroy: pcvDestroyChart,
  timeSeries: pcvTimeSeries,
  doughnut: pcvDoughnut,
  destroyAllInContainer: pcvDestroyAllInContainer
};
})(window.PCV);












window.PCV = window.PCV || {};
(function(PCV) {


var chartRegistry = {};
window.chartRegistry = chartRegistry;


function destroyAllCharts() {
  for (const id of Object.keys(chartRegistry)) {
    try { chartRegistry[id].destroy(); } catch (e) { if(_DEBUG) console.warn('destroyAllCharts:', e.message); }
    delete chartRegistry[id];
  }
}
window.destroyAllCharts = destroyAllCharts;

function createLineChart(canvasId, data, label, color) {
  const canvas = document.getElementById(canvasId);
  if (!canvas) return;

  if (chartRegistry[canvasId]) {
    if (chartRegistry[canvasId].canvas === canvas) {
      const chart = chartRegistry[canvasId];
      chart.data.labels = data.map((_, i) => i);
      chart.data.datasets[0].data = data;
      chart.update('none');
      return;
    }

    try { chartRegistry[canvasId].destroy(); } catch (e) { if(_DEBUG) console.warn('createLineChart:', e.message); }
    delete chartRegistry[canvasId];
  }
  const ctx = canvas.getContext('2d');
  if (typeof Chart === 'undefined') {
    drawGraphFallback(canvasId, data, color);
    return;
  }
  const chart = new Chart(ctx, {
    type: 'line',
    data: {
      labels: data.map((_, i) => i),
      datasets: [{
        label: label || '',
        data: data,
        borderColor: color,
        backgroundColor: color.replace(')', ',0.15)').replace('rgb', 'rgba'),
        borderWidth: 1.5,
        fill: true,
        tension: 0.3,
        pointRadius: 0,
      }]
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: false,
      plugins: { legend: { display: false } },
      scales: {
        x: { display: false },
        y: { display: false, min: 0, max: 100 }
      }
    }
  });
  chartRegistry[canvasId] = chart;
}
window.createLineChart = createLineChart;

function drawGraphFallback(id, data, color) {
  const cv = document.getElementById(id);
  if (!cv) return;
  const x = cv.getContext('2d');
  cv.width = cv.offsetWidth;
  cv.height = cv.offsetHeight;
  const w = cv.width, h = cv.height;
  x.clearRect(0, 0, w, h);
  x.strokeStyle = color;
  x.lineWidth = 1.5;
  x.beginPath();
  for (let i = 0; i < data.length; i++) {
    const px = i / (data.length - 1) * w;
    const py = h - (data[i] / 100) * h;
    i === 0 ? x.moveTo(px, py) : x.lineTo(px, py);
  }
  x.stroke();
}
window.drawGraphFallback = drawGraphFallback;


function getChartColor(name) {
  try { return getComputedStyle(document.documentElement).getPropertyValue('--chart-' + name).trim() || name; }
  catch(e) { return name; }
}
window.getChartColor = getChartColor;


var _PROD_NODES = window._PROD_NODES || [{ name: 'Local', ip: window.location.hostname || '127.0.0.1' }];
var _VIP = window._VIP || null;
var _curHost = window._curHost || window.location.hostname;
var _isProd = window._isProd || false;
window._isProd = _isProd;
window._curHost = _curHost;
var MON_NODES = _PROD_NODES;
window.MON_NODES = MON_NODES;

async function _refreshMonNodes() {
  if (window._loadClusterNodes) await window._loadClusterNodes();
  _PROD_NODES = window._PROD_NODES || _PROD_NODES;
  _isProd = window._isProd || false;
  _VIP = window._VIP || null;
  MON_NODES = _PROD_NODES;
  window.MON_NODES = MON_NODES;
}
window._refreshMonNodes = _refreshMonNodes;
var monHist = {};
window.monHist = monHist;

function _parseLabels(raw) { const o = {}; raw.replace(/(\w+)="([^"]*)"/g, (_, k, v) => { o[k] = v; }); return o; }
window._parseLabels = _parseLabels;

function _newZfsLocks() {
  return { total: 0, ok: 0, busy: 0, error: 0, unknown: 0, waitSumMs: 0, waitCount: 0, byOp: {} };
}

function _zfsLockOp(stats, op) {
  var key = op || 'unknown';
  if (!stats.byOp[key]) stats.byOp[key] = { total: 0, ok: 0, busy: 0, error: 0, unknown: 0, waitSumMs: 0, waitCount: 0 };
  return stats.byOp[key];
}

function _recordZfsLockMetric(m, key, labels, value) {
  if (!m.zfsLocks) m.zfsLocks = _newZfsLocks();
  var op = labels.op || 'unknown';
  var result = labels.result || 'unknown';
  var opStats = _zfsLockOp(m.zfsLocks, op);
  if (key === 'purecvisor_zfs_inflight_lock_acquired_total') {
    m.zfsLocks.total += value;
    m.zfsLocks[result] = (m.zfsLocks[result] || 0) + value;
    opStats.total += value;
    opStats[result] = (opStats[result] || 0) + value;
  } else if (key === 'purecvisor_zfs_inflight_lock_wait_ms_sum') {
    m.zfsLocks.waitSumMs += value;
    opStats.waitSumMs += value;
  } else if (key === 'purecvisor_zfs_inflight_lock_wait_ms_count') {
    m.zfsLocks.waitCount += value;
    opStats.waitCount += value;
  }
}

var _monPort = location.port || '';
function _buildMetricsUrl(ip) {
  const proto = location.protocol || 'http:';
  const port = _monPort && _monPort !== '80' && _monPort !== '443' ? ':' + _monPort : '';
  return proto + '//' + ip + port + '/api/v1/metrics';
}
window._buildMetricsUrl = _buildMetricsUrl;

function _metricsAuthHeaders() {
  var token = window.authToken || (typeof authToken !== 'undefined' ? authToken : '');
  return token ? { Authorization: 'Bearer ' + token } : {};
}

async function _fetchMetricsText(url) {
  const r = await fetch(url, { headers: _metricsAuthHeaders() });
  if (!r.ok) throw new Error('metrics HTTP ' + r.status);
  return r.text();
}


async function fetchAllMetrics() {
  const all = await Promise.all(MON_NODES.map(async (nd) => {
    try {
      const txt = await _fetchMetricsText(_buildMetricsUrl(nd.ip));
        const m = { node: nd.name, ip: nd.ip, cores: {}, memInfo: {}, filesystems: [], disks: {}, netdevs: {}, hwmon: [], sockstat: {}, vmstat: {}, pressure: {}, zfsLocks: _newZfsLocks() };
      const vms = [];
      txt.split('\n').forEach(l => {
        if (l.startsWith('#') || !l.trim()) return;
        const sp = l.match(/^([a-zA-Z_][a-zA-Z0-9_]*)(\{[^}]*\})?\s+(.+)$/);
        if (!sp) return;
        const k = sp[1], labels = sp[2] || '', v = parseFloat(sp[3]), lb = labels ? _parseLabels(labels) : {};
        if (k === 'purecvisor_host_cpu_percent') m.cpu = v;
        if (k === 'purecvisor_host_memory_percent') m.mem = v;
        if (k === 'purecvisor_host_disk_percent') m.disk = v;
        if (k === 'purecvisor_host_memory_total_bytes') m.ram_total = v;
        if (k === 'purecvisor_host_cpu_temp_celsius') m.temp = v;
        if (k === 'purecvisor_host_load1') m.load = v;
        const vmM = k.match(/^purecvisor_vm_(\w+)$/);
        if (vmM && lb.vm) { let vm = vms.find(x => x.name === lb.vm); if (!vm) { vm = { name: lb.vm, node: nd.name }; vms.push(vm); } vm[vmM[1]] = v; }
        if (k === 'node_cpu_seconds_total' && lb.cpu && lb.mode) { const c = lb.cpu; if (!m.cores[c]) m.cores[c] = {}; m.cores[c][lb.mode] = v; }
        if (k.startsWith('node_memory_') && k.endsWith('_bytes')) { m.memInfo[k.slice(12, -6)] = v; }
        if (k.startsWith('node_filesystem_') && lb.mountpoint) { let fs = m.filesystems.find(f => f.mount === lb.mountpoint); if (!fs) { fs = { mount: lb.mountpoint, dev: lb.device || '', fstype: lb.fstype || '' }; m.filesystems.push(fs); } fs[k.slice(16)] = v; }
        if (k.startsWith('node_disk_') && lb.device) { if (!m.disks[lb.device]) m.disks[lb.device] = {}; m.disks[lb.device][k.slice(10)] = v; }
        if (k.startsWith('node_network_') && lb.device) { if (!m.netdevs[lb.device]) m.netdevs[lb.device] = {}; m.netdevs[lb.device][k.slice(13)] = v; }
        if (k === 'node_hwmon_temp_celsius' && lb.chip) m.hwmon.push({ chip: lb.chip, sensor: lb.sensor || '', temp: v });
        if (k === 'node_hwmon_temp_crit_celsius' && lb.chip) { const h2 = m.hwmon.find(x => x.chip === lb.chip && x.sensor === (lb.sensor || '')); if (h2) h2.crit = v; }
        if (k.startsWith('node_sockstat_')) { m.sockstat[k.slice(14)] = v; }
        if (k.startsWith('node_vmstat_')) { m.vmstat[k.slice(12)] = v; }
        if (k.startsWith('node_pressure_')) { m.pressure[k.slice(14)] = v; }
        if (k === 'node_load1') m.load1 = v; if (k === 'node_load5') m.load5 = v; if (k === 'node_load15') m.load15 = v;
        if (k === 'node_boot_time_seconds') m.boot_time = v;
        if (k === 'node_uptime_seconds') m.uptime = v;
        if (k === 'node_context_switches_total') m.ctxt = v;
        if (k === 'node_forks_total') m.forks = v;
        if (k === 'node_entropy_available_bits') m.entropy = v;
        if (k === 'node_filefd_allocated') m.fd_alloc = v;
        if (k === 'node_nf_conntrack_entries') m.conntrack = v;
        if (k === 'node_nf_conntrack_entries_limit') m.conntrack_max = v;
        if (k === 'purecvisor_anomaly_active') m.anomaly_active = v;
        if (k === 'purecvisor_anomaly_alerts_total') m.anomaly_total = v;
        if (k === 'purecvisor_predict_cpu_5m') m.cpu_pred = v;
        if (k === 'purecvisor_predict_mem_5m') m.mem_pred = v;
        if (k === 'purecvisor_predict_trend_cpu') m.cpu_trend = v;
        if (k === 'purecvisor_predict_trend_mem') m.mem_trend = v;
        if (k === 'purecvisor_healing_pending_approvals') m.healing_pending = v;
        if (k === 'purecvisor_healing_actions_total') m.healing_total = v;
        if (k === 'purecvisor_agent_consensus_confidence') m.agent_conf = v;
        if (k === 'purecvisor_agent_latency_ms' && lb.provider) { if (!m.agent_prov) m.agent_prov = {}; if (!m.agent_prov[lb.provider]) m.agent_prov[lb.provider] = {}; m.agent_prov[lb.provider].latency = v; m.agent_prov[lb.provider].model = lb.model || ''; }
        if (k === 'purecvisor_agent_confidence' && lb.provider) { if (!m.agent_prov) m.agent_prov = {}; if (!m.agent_prov[lb.provider]) m.agent_prov[lb.provider] = {}; m.agent_prov[lb.provider].confidence = v; }
        if (k === 'purecvisor_keepalived_active') m.keepalived_active = v;
        if (k === 'purecvisor_keepalived_master') m.keepalived_master = v;
        if (k === 'purecvisor_keepalived_vip_owner') m.keepalived_vip_owner = v;
        if (k === 'purecvisor_anomaly_score' && lb.metric) { if (!m.anomaly_scores) m.anomaly_scores = {}; m.anomaly_scores[lb.metric] = v; }
        if (k === 'purecvisor_zfs_inflight_lock_acquired_total' ||
            k === 'purecvisor_zfs_inflight_lock_wait_ms_sum' ||
            k === 'purecvisor_zfs_inflight_lock_wait_ms_count') {
          _recordZfsLockMetric(m, k, lb, v);
        }
      });
      m.vms = vms;
      if (!monHist[nd.ip]) monHist[nd.ip] = { cpu: [], mem: [], disk: [], netRx: [], netTx: [] };
      const hi = monHist[nd.ip]; hi.cpu.push(m.cpu || 0); hi.mem.push(m.mem || 0); hi.disk.push(m.disk || 0);
      const phys = Object.entries(m.netdevs).filter(([d]) => !['lo', 'ovs-system', 'br-int'].includes(d));
      hi.netRx.push(phys.reduce((s, [, d]) => s + (d.receive_bytes_total || 0), 0));
      hi.netTx.push(phys.reduce((s, [, d]) => s + (d.transmit_bytes_total || 0), 0));
      [hi.cpu, hi.mem, hi.disk, hi.netRx, hi.netTx].forEach(a => { while (a.length > 60) a.shift(); });
      return m;
    } catch (e) {
      return { node: nd.name, ip: nd.ip, cpu: 0, mem: 0, disk: 0, error: true, vms: [], cores: {}, memInfo: {}, filesystems: [], disks: {}, netdevs: {}, hwmon: [], sockstat: {}, vmstat: {}, pressure: {}, zfsLocks: _newZfsLocks() };
    }
  }));
  return all;
}
window.fetchAllMetrics = fetchAllMetrics;


function fmtBytes(b) { if (b >= 1e12) return (b / 1e12).toFixed(1) + ' TB'; if (b >= 1e9) return (b / 1e9).toFixed(1) + ' GB'; if (b >= 1e6) return (b / 1e6).toFixed(1) + ' MB'; if (b >= 1e3) return (b / 1e3).toFixed(1) + ' KB'; return b + ' B'; }
window.fmtBytes = fmtBytes;

function fmtRate(arr, i) { if (i < 1 || !arr[i] || !arr[i - 1]) return '0 B/s'; const d = arr[i] - arr[i - 1]; return d > 0 ? fmtBytes(d / 5) + '/s' : '0 B/s'; }
window.fmtRate = fmtRate;

function fmtUptime(s) { const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), mi = Math.floor(s % 3600 / 60); return d + 'd ' + h + 'h ' + mi + 'm'; }
window.fmtUptime = fmtUptime;


function drawLine(id, data, color, unit, max) {
  const cv = document.getElementById(id); if (!cv) return;
  const x = cv.getContext('2d'); cv.width = cv.offsetWidth; cv.height = cv.offsetHeight;
  const w = cv.width, h = cv.height;
  x.fillStyle = 'rgba(0,0,0,0.3)'; x.fillRect(0, 0, w, h);
  x.strokeStyle = 'rgba(255,255,255,0.05)'; x.lineWidth = 1;
  for (let i = 1; i < 4; i++) { const y = h * i / 4; x.beginPath(); x.moveTo(0, y); x.lineTo(w, y); x.stroke(); }
  x.strokeStyle = color; x.lineWidth = 2; x.beginPath(); const mx = max || 100;
  for (let i = 0; i < data.length; i++) { const px = i / (Math.max(data.length - 1, 1)) * w; const py = h - (data[i] / mx) * h; i === 0 ? x.moveTo(px, py) : x.lineTo(px, py); } x.stroke();
  x.lineTo(w, h); x.lineTo(0, h); x.closePath();
  const grd = x.createLinearGradient(0, 0, 0, h); grd.addColorStop(0, color.replace(')', ',0.3)').replace('rgb', 'rgba')); grd.addColorStop(1, 'rgba(0,0,0,0)'); x.fillStyle = grd; x.fill();
  x.fillStyle = 'rgba(255,255,255,0.5)'; x.font = '10px Inter'; x.fillText(data[data.length - 1]?.toFixed(1) + (unit || ''), 4, 12);
}
window.drawLine = drawLine;

function gauge(pct, label, color) {
  const cl = color || (pct > 80 ? 'var(--red)' : pct > 60 ? 'var(--yellow)' : 'var(--green)');
  return '<div class="text-center"><svg width="90" height="50" viewBox="0 0 90 50"><path d="M10 45 A35 35 0 0 1 80 45" fill="none" stroke="var(--border)" stroke-width="6" stroke-linecap="round"/><path d="M10 45 A35 35 0 0 1 80 45" fill="none" stroke="' + cl + '" stroke-width="6" stroke-linecap="round" stroke-dasharray="' + (pct * 1.1) + ' 110" style="filter:drop-shadow(0 0 4px ' + cl + ')"/></svg><div class="stat-sm" style="margin-top:-8px;color:' + cl + '">' + pct.toFixed(1) + '%</div><div class="stat-label">' + label + '</div></div>';
}
window.gauge = gauge;


async function renderMonitoring(b, tab) {

  if (typeof pcvDestroyAllInContainer === 'function') pcvDestroyAllInContainer(b);
  destroyAllCharts();
  b.innerHTML = showSkeleton();
  const all = await fetchAllMetrics();
  const allVms = all.flatMap(n => n.vms.map(v => ({ ...v, nodeIP: n.ip })));
  const running = allVms.filter(v => v.running === 1).length;
  const avgCpu = all.reduce((s, n) => s + (n.cpu || 0), 0) / Math.max(all.length, 1);
  const avgMem = all.reduce((s, n) => s + (n.mem || 0), 0) / Math.max(all.length, 1);
  const avgDisk = all.reduce((s, n) => s + (n.disk || 0), 0) / Math.max(all.length, 1);
  const totalRam = all.reduce((s, n) => s + (n.ram_total || 0), 0);

  if (tab === 'overview') renderMonOverview(b, all, allVms, running, avgCpu, avgMem, avgDisk, totalRam);
  else if (tab === 'hosts') renderMonHosts(b, all);
  else if (tab === 'vms') renderMonVms(b, allVms, running);
  else if (tab === 'storage') renderMonStorage(b, all, allVms, totalRam);


  if (typeof startAdaptivePolling === 'function') {
    startAdaptivePolling('mon-refresh', function() {
      if (window.currentTab && window.currentTab.startsWith('mon-')) {
        var cb = document.getElementById('cb');
        if (cb) fetchAllMetrics().then(function(fresh) {
          var freshVms = fresh.flatMap(function(n) { return n.vms.map(function(v) { return Object.assign({}, v, { nodeIP: n.ip }); }); });
          var r = freshVms.filter(function(v) { return v.running === 1; }).length;
          var ac = fresh.reduce(function(s, n) { return s + (n.cpu || 0); }, 0) / Math.max(fresh.length, 1);
          var am = fresh.reduce(function(s, n) { return s + (n.mem || 0); }, 0) / Math.max(fresh.length, 1);
          var ad = fresh.reduce(function(s, n) { return s + (n.disk || 0); }, 0) / Math.max(fresh.length, 1);
          var tr = fresh.reduce(function(s, n) { return s + (n.ram_total || 0); }, 0);
          if (window.currentTab === 'mon-overview') renderMonOverview(cb, fresh, freshVms, r, ac, am, ad, tr);
          else if (window.currentTab === 'mon-hosts') renderMonHosts(cb, fresh);
          else if (window.currentTab === 'mon-vms') renderMonVms(cb, freshVms, r);
          else if (window.currentTab === 'mon-storage') renderMonStorage(cb, fresh, freshVms, tr);
        }).catch(function() {  });
      } else {
        if (typeof stopAdaptivePolling === 'function') stopAdaptivePolling('mon-refresh');
      }
    }, 10000);
  }
}
window.renderMonitoring = renderMonitoring;

function _opsPct(n, fallback) {
  var v = Number(n);
  if (!Number.isFinite(v)) v = fallback || 0;
  return Math.max(0, Math.min(100, v));
}

function _opsStatus(label, tone) {
  var cls = tone || 'info';
  var dot = cls === 'bad' ? 'bad' : cls === 'warn' ? 'warn' : 'ok';
  return '<span class="ops-status ' + cls + '"><span class="ops-dot ' + dot + '"></span>' + esc(label) + '</span>';
}

function _opsMetricCard(label, value, detail, statusLabel, tone) {
  return '<article class="ops-triage-card ops-triage-metric ops-span-3">'
    + '<div class="ops-triage-metric-label">' + esc(label) + '</div>'
    + '<div class="ops-triage-metric-value">' + esc(value) + '</div>'
    + '<div class="ops-triage-metric-foot"><span>' + esc(detail) + '</span>' + _opsStatus(statusLabel, tone) + '</div>'
    + '</article>';
}

function _opsBar(pct) {
  var safe = _opsPct(pct, 0);
  var text = safe.toFixed(safe < 10 ? 1 : 0) + '%';
  return '<div class="ops-bar" style="--value:' + safe.toFixed(1) + '%"><div class="ops-bar-fill"></div><div class="ops-bar-label">' + text + '</div></div>';
}

function _opsVmStatus(v) {
  var state = String(v.state || (v.running === 1 ? 'running' : 'unknown')).toLowerCase();
  if (state === 'running' || state === 'http 200') return _opsStatus(state === 'http 200' ? '200' : 'RUN', 'ok');
  if (state === 'shut off' || state === 'stopped' || state === 'off') return _opsStatus('OFF', 'bad');
  return _opsStatus('CHECK', 'warn');
}

function _opsFallbackVms() {
  return [
    { name: 'pcv-demo-vm', role: '공개 데모', ip: '192.0.2.12', cpu: 2.0, mem: 48, state: 'running', running: 1 },
    { name: 'ovn-demo-a', role: '테넌트 A', ip: '10.77.0.12', cpu: 12, mem: 34, state: 'http 200', running: 1 },
    { name: 'ovn-demo-b', role: '테넌트 B', ip: '10.77.0.13', cpu: 16, mem: 41, state: 'unknown', running: 1 }
  ];
}

function _opsVmRows(sourceVms) {
  var rows = (sourceVms || []).slice(0, 5).map(function(v) {
    var maxMb = Number(v.memory_max_mb || v.memory_mb || v.maxmem || 0);
    var usedMb = Number(v.memory_used_mb || v.mem_used_mb || 0);
    var memPct = maxMb > 0 && usedMb > 0 ? usedMb / maxMb * 100 : _opsPct(v.mem || v.memory_percent, 34);
    var cpuPct = _opsPct(v.cpu || v.cpu_percent || v.cpu_usage, v.running === 1 ? 12 : 0);
    return {
      name: v.name || v.vm || '-',
      role: v.role || (v.node ? '호스트 ' + v.node : 'VM 자산'),
      ip: v.ip_addr || v.ip || v.addr || '-',
      cpu: cpuPct,
      mem: memPct,
      state: v.state || (v.running === 1 ? 'running' : 'unknown'),
      running: v.running
    };
  });
  if (rows.length === 0) {
    rows = _opsFallbackVms();
  }
  return rows.map(function(v) {
    return '<tr>'
      + '<td><div class="ops-name"><svg class="ci-icon" aria-hidden="true"><use href="vendor/coolicons/coolicons.svg#ci-desktop-tower"></use></svg>' + esc(v.name) + '</div></td>'
      + '<td>' + esc(v.role) + '</td>'
      + '<td class="ops-mono">' + esc(v.ip) + '</td>'
      + '<td>' + _opsBar(v.cpu) + '</td>'
      + '<td>' + _opsBar(v.mem) + '</td>'
      + '<td>' + _opsVmStatus(v) + '</td>'
      + '</tr>';
  }).join('');
}

function _opsAuditRows() {
  var raw = [];
  try { raw = (window.eventLog || eventLog || []).slice(-3).reverse(); } catch (e) { raw = []; }
  if (raw.length === 0) {
    raw = [
      { title: 'vm.guest.exec', detail: 'target=pcv-demo-vm, result=ok, job_id=81f2', time: '12:22', tone: 'ok' },
      { title: 'security.event', detail: 'target=viewer, source=nginx, result=warn', time: '12:21', tone: 'warn' },
      { title: 'ovn.status', detail: 'target=pcv-demo-lr, result=ok', time: '12:19', tone: 'ok' }
    ];
  }
  return raw.map(function(item) {
    var obj = typeof item === 'string'
      ? { title: item.split(':')[0] || 'event', detail: item, time: '-', tone: /fail|warn|error/i.test(item) ? 'warn' : 'ok' }
      : item;
    var tone = obj.tone || (/fail|error/i.test(obj.detail || obj.title || '') ? 'bad' : /warn/i.test(obj.detail || obj.title || '') ? 'warn' : 'ok');
    return '<div class="ops-triage-event">'
      + '<div class="ops-severity ' + tone + '"></div>'
      + '<div><p class="ops-event-title">' + esc(obj.title || 'event') + '</p><div class="ops-event-sub">' + esc(obj.detail || obj.msg || '') + '</div></div>'
      + '<div class="ops-event-time">' + esc(obj.time || '-') + '</div>'
      + '</div>';
  }).join('');
}

async function renderOpsTriage(b) {
  if (typeof pcvDestroyAllInContainer === 'function') pcvDestroyAllInContainer(b);
  destroyAllCharts();
  b.innerHTML = showSkeleton();
  var all = [];
  var apiVms = [];
  try {
    var result = await Promise.all([
      fetchAllMetrics().catch(function() { return []; }),
      fetchGet(EP.VM_LIST()).catch(function() { return { data: [] }; })
    ]);
    all = Array.isArray(result[0]) ? result[0] : [];
    apiVms = unwrapList(result[1]);
  } catch (e) {
    all = [];
    apiVms = [];
  }

  var metricVms = all.flatMap(function(n) {
    return (n.vms || []).map(function(v) {
      return Object.assign({}, v, { node: n.node || n.ip });
    });
  });
  var sourceVms = metricVms.length ? metricVms : apiVms;
  var displayVms = sourceVms.length ? sourceVms : _opsFallbackVms();
  var usableMetrics = all.filter(function(n) { return !n.error; });
  var avgCpu = usableMetrics.length ? usableMetrics.reduce(function(s, n) { return s + (n.cpu || 0); }, 0) / usableMetrics.length : 2.0;
  var avgMem = usableMetrics.length ? usableMetrics.reduce(function(s, n) { return s + (n.mem || 0); }, 0) / usableMetrics.length : 41;
  var totalRam = usableMetrics.reduce(function(s, n) { return s + (n.ram_total || 0); }, 0);
  var ramDetail = totalRam > 0 ? fmtBytes(totalRam) + ' total' : '32GB 중 13.1GB';
  var running = displayVms.filter(function(v) {
    var st = String(v.state || '').toLowerCase();
    return v.running === 1 || st === 'running';
  }).length;
  var totalVm = displayVms.length;

  var h = '<header class="ops-triage-head">'
    + '<div><div class="ops-triage-kicker">Single Edge operations</div>'
    + '<h1 class="ops-triage-title">운영 이벤트 센터</h1>'
    + '<p class="ops-triage-sub">VM, OVN, ZFS, 보안 이벤트를 한 화면에서 triage하고 즉시 조치하는 운영자용 화면입니다.</p></div>'
    + '<div class="ops-triage-tabs" role="tablist" aria-label="시간 범위">'
    + '<button class="ops-triage-tab is-active" type="button">LIVE</button>'
    + '<button class="ops-triage-tab" type="button">1H</button>'
    + '<button class="ops-triage-tab" type="button">24H</button>'
    + '<button class="ops-triage-tab" type="button">AUDIT</button>'
    + '</div></header>';

  h += '<section class="ops-triage-grid" aria-label="운영 이벤트 센터">';
  h += _opsMetricCard('호스트 CPU', avgCpu.toFixed(1) + '%', '단일 노드 평균', avgCpu > 80 ? '위험' : '정상', avgCpu > 80 ? 'bad' : avgCpu > 60 ? 'warn' : 'ok');
  h += _opsMetricCard('메모리', avgMem.toFixed(0) + '%', ramDetail, avgMem > 85 ? '위험' : '여유', avgMem > 85 ? 'bad' : avgMem > 70 ? 'warn' : 'ok');
  h += _opsMetricCard('OVN 게이트웨이', '10.77.0.1', 'pcv-demo-lr', 'ACTIVE', 'info');
  h += _opsMetricCard('실행 VM', running + '/' + totalVm, 'viewer read-only 기준', running > 0 ? '가동' : '확인', running > 0 ? 'ok' : 'warn');

  h += '<article class="ops-triage-card ops-span-5"><div class="ops-triage-card-head">'
    + '<div class="ops-triage-card-title" role="heading" aria-level="2">이벤트 triage</div><span class="ops-triage-card-meta">최근 15분</span></div>'
    + '<div class="ops-triage-list">'
    + '<div class="ops-triage-event"><div class="ops-severity bad"></div><div><p class="ops-event-title">viewer 계정 로그인 시도 증가</p><div class="ops-event-sub">nginx access log 기준 동일 User-Agent 반복 접근</div></div><div class="ops-event-time">LIVE</div></div>'
    + '<div class="ops-triage-event"><div class="ops-severity warn"></div><div><p class="ops-event-title">exporter scrape 지연 확인</p><div class="ops-event-sub">Prometheus full exporter 응답 지연은 관측성 품질에 영향</div></div><div class="ops-event-time">WARN</div></div>'
    + '<div class="ops-triage-event"><div class="ops-severity ok"></div><div><p class="ops-event-title">OVN demo NAT 흐름 정상</p><div class="ops-event-sub">ovn-demo-a → pcv-demo-ls → pcv-demo-lr → external</div></div><div class="ops-event-time">OK</div></div>'
    + '</div></article>';

  h += '<article class="ops-triage-card ops-span-7"><div class="ops-triage-card-head">'
    + '<div class="ops-triage-card-title" role="heading" aria-level="2">VM 및 서비스 상태</div><span class="ops-triage-card-meta">' + totalVm + ' assets</span></div>'
    + '<div class="ops-triage-toolbar"><input class="ops-triage-field" type="search" value="demo" aria-label="자산 검색">'
    + '<div class="ops-triage-actions"><button class="ops-triage-action" type="button" onclick="renderOpsTriage(document.getElementById(\'cb\'))"><svg class="ci-icon" aria-hidden="true"><use href="vendor/coolicons/coolicons.svg#ci-refresh"></use></svg>새로고침</button>'
    + '<button class="ops-triage-action primary" type="button" onclick="openCmdPalette()"><svg class="ci-icon" aria-hidden="true"><use href="vendor/coolicons/coolicons.svg#ci-play"></use></svg>조치 선택</button></div></div>'
    + '<div class="ops-triage-table-wrap"><table class="ops-triage-table"><thead><tr><th>이름</th><th>역할</th><th>IP</th><th>CPU</th><th>메모리</th><th>상태</th></tr></thead><tbody>'
    + _opsVmRows(displayVms)
    + '</tbody></table></div></article>';

  h += '<article class="ops-triage-card ops-span-4"><div class="ops-triage-card-head">'
    + '<div class="ops-triage-card-title" role="heading" aria-level="2">명령 팔레트</div><span class="ops-triage-card-meta">Ctrl K</span></div>'
    + '<div class="ops-command">'
    + '<button class="ops-command-row is-active" type="button" onclick="navigateTo(\'host\')"><span class="ops-key">RUN</span><span>qemu-guest-agent 설치 확인</span><span class="ops-key">Enter</span></button>'
    + '<button class="ops-command-row" type="button" onclick="navigateTo(\'ovn\')"><span class="ops-key">NET</span><span>OVN NAT 및 logical router 상태 확인</span><span class="ops-key">N</span></button>'
    + '<button class="ops-command-row" type="button" onclick="navigateTo(\'mon-audit\')"><span class="ops-key">LOG</span><span>viewer 성공 로그인 IP 목록 열기</span><span class="ops-key">L</span></button>'
    + '<button class="ops-command-row" type="button" onclick="navigateTo(\'activity-log\')"><span class="ops-key">JOB</span><span>실패 작업만 필터링</span><span class="ops-key">J</span></button>'
    + '</div></article>';

  h += '<article class="ops-triage-card ops-span-4"><div class="ops-triage-card-head">'
    + '<div class="ops-triage-card-title" role="heading" aria-level="2">OVN 서비스 흐름</div><span class="ops-triage-card-meta">demo</span></div>'
    + '<div class="ops-node"><div class="ops-node-icon"><svg class="ci-icon" aria-hidden="true"><use href="vendor/coolicons/coolicons.svg#ci-desktop-tower"></use></svg></div><div><div class="ops-node-name">ovn-demo-a</div><div class="ops-node-sub">10.77.0.12:8080</div></div>' + _opsStatus('WEB', 'ok') + '</div>'
    + '<div class="ops-node"><div class="ops-node-icon"><svg class="ci-icon" aria-hidden="true"><use href="vendor/coolicons/coolicons.svg#ci-layers"></use></svg></div><div><div class="ops-node-name">pcv-demo-ls</div><div class="ops-node-sub">logical switch</div></div>' + _opsStatus('L2', 'info') + '</div>'
    + '<div class="ops-node"><div class="ops-node-icon"><svg class="ci-icon" aria-hidden="true"><use href="vendor/coolicons/coolicons.svg#ci-globe"></use></svg></div><div><div class="ops-node-name">pcv-demo-lr</div><div class="ops-node-sub">gateway 10.77.0.1</div></div>' + _opsStatus('NAT', 'ok') + '</div>'
    + '<div class="ops-flow" aria-label="서비스 흐름"><div class="ops-flow-box">VM</div><div>→</div><div class="ops-flow-box">LS</div><div>→</div><div class="ops-flow-box">LR</div></div>'
    + '</article>';

  h += '<article class="ops-triage-card ops-span-4"><div class="ops-triage-card-head">'
    + '<div class="ops-triage-card-title" role="heading" aria-level="2">감사 추적</div><span class="ops-triage-card-meta">audit</span></div>'
    + '<div class="ops-triage-list">' + _opsAuditRows() + '</div></article>';
  h += '</section>';
  b.innerHTML = h;
}
window.renderOpsTriage = renderOpsTriage;


async function loadDeepHealth() {
  var el = document.getElementById('deep-health'); if (!el) return;
  el.innerHTML = '<span class="spinner"></span> ' + (t('loading') || 'Loading...');
  try {
    var r = await fetch(EP.HEALTH());
    var d = await r.json();
    var overall = d.status || d.overall || 'unknown';
    var node = d.node || d.hostname || '-';
    var uptime = d.uptime_sec || d.uptime || 0;
    var subsystems = d.subsystems || d.checks || {};

    var overallColor = overall === 'ok' ? 'var(--green)' : overall === 'degraded' ? 'var(--yellow)' : 'var(--red)';
    var h = '<div style="display:flex;align-items:center;gap:12px;margin-bottom:10px;flex-wrap:wrap">';
    h += '<span style="font-size:14px;font-weight:700;color:' + overallColor + '">' + esc(overall.toUpperCase()) + '</span>';
    h += H.badge(esc(node), 'g');
    if (uptime > 0) h += '<span class="stat-label">' + (t('monitor.uptime') || 'Uptime') + ': ' + fmtUptime(uptime) + '</span>';
    h += '</div>';

    var subsysKeys = Object.keys(subsystems);
    if (subsysKeys.length === 0) {

      var knownSubs = ['libvirt', 'etcd', 'zfs', 'vm_state_db', 'audit_db', 'tls', 'cluster'];
      knownSubs.forEach(function(k) { if (d[k] !== undefined) subsystems[k] = d[k]; });
      subsysKeys = Object.keys(subsystems);
    }

    if (subsysKeys.length > 0) {
      h += '<div style="display:flex;gap:8px;flex-wrap:wrap">';
      subsysKeys.forEach(function(k) {
        var v = subsystems[k];
        var st;
        if (typeof v === 'object' && v !== null) {
          if (v.status) st = v.status;
          else if (v.state) st = v.state;
          else if (v.ok !== undefined) st = v.ok ? 'ok' : 'fail';
          else if (v.enabled !== undefined) st = v.enabled ? 'enabled' : 'disabled';
          else if (v.mode) st = v.mode;
          else if (v.note) st = v.note;
          else st = 'unknown';
        } else {
          st = String(v);
        }
        var sc = (st === 'ok' || st === 'connected' || st === 'active' || st === 'enabled' || st === 'true' || st === true) ? 'g'
          : (st === 'warning' || st === 'degraded') ? 'y' : (st === 'unknown' || st === 'n/a' || st === 'disabled' || st === 'single_edge' || /standalone/.test(st)) ? '' : 'r';
        var detailParts = [];
        if (typeof v === 'object' && v !== null) {
          if (v.detail) detailParts.push(v.detail);
          if (v.latency_ms !== undefined) detailParts.push(v.latency_ms + 'ms');
          if (v.avail_gb !== undefined) detailParts.push(v.avail_gb.toFixed(1) + 'GB free');
          if (v.size_mb !== undefined) detailParts.push(v.size_mb.toFixed(1) + 'MB');
        }
        var detail = detailParts.length ? ' (' + esc(detailParts.join(', ')) + ')' : '';
        h += '<div style="display:inline-flex;align-items:center;gap:4px;padding:4px 10px;border:1px solid var(--border);border-radius:6px;font-size:11px;background:var(--bg2)">';
        h += '<span style="color:' + _healthBadgeColor(sc) + ';font-size:8px">&#9679;</span>';
        h += '<span style="font-weight:600">' + esc(k) + '</span>';
        h += '<span style="color:' + _healthBadgeColor(sc) + '">' + esc(st) + detail + '</span>';
        h += '</div>';
      });
      h += '</div>';
    } else {
      h += '<span class="color-muted">' + (t('monitor.no_subsystems') || 'No subsystem details available') + '</span>';
    }

    el.innerHTML = h;
  } catch (e) {
    el.innerHTML = '<span class="color-muted">' + (t('monitor.health_unavailable') || 'Health probe unavailable') + ': ' + esc(e.message) + '</span>';
  }
}

function _healthBadgeColor(sc) {
  if (sc === 'g') return 'var(--green)';
  if (sc === 'y') return 'var(--yellow)';
  if (sc === 'r') return 'var(--red)';
  return 'var(--fg2)';
}

function renderMonOverview(b, all, allVms, running, avgCpu, avgMem, avgDisk, totalRam) {

  if (typeof pcvDestroyAllInContainer === 'function') pcvDestroyAllInContainer(b);
  destroyAllCharts();

  let h = '<div class="hc mb-14"><h4>&#129657; ' + (t('monitor.system_health') || 'System Health') + '</h4>';
  h += '<p class="color-muted text-11 mb-8">' + (t('monitor.health_desc') || 'Deep health probe of all subsystems. Updated on each page load.') + '</p>';
  h += '<div id="deep-health"><span class="spinner"></span> ' + (t('loading') || 'Loading...') + '</div></div>';


  h += '<div class="hc mb-12"><h4>&#128202; ' + (t('monitor.cluster_timeline') || '리소스 흐름 (최근 5분)') + '</h4>';
  h += '<div class="grid-3 gap-12" style="display:grid;grid-template-columns:1fr 1fr 1fr">';
  h += '<div style="position:relative;height:180px"><canvas id="pcv-chart-cpu"></canvas></div>';
  h += '<div style="position:relative;height:180px"><canvas id="pcv-chart-mem"></canvas></div>';
  h += '<div style="position:relative;height:180px"><canvas id="pcv-chart-net"></canvas></div>';
  h += '</div></div>';

  h += H.section('&#128200; 운영 개요');
  h += H.grid(8,
    H.card('호스트', '<div class="stat-xl color-green">' + all.length + '</div>', 'text-center')
  + H.card('VM', '<div class="stat-xl color-accent">' + allVms.length + '</div>', 'text-center')
  + H.card('실행 중', '<div class="stat-xl color-green">' + running + '</div>', 'text-center')
  + H.card('평균 CPU', gauge(avgCpu, 'Host'), 'text-center')
  + H.card('평균 메모리', gauge(avgMem, 'Host'), 'text-center')
  + H.card('평균 디스크', gauge(avgDisk, 'Host'), 'text-center')
  + (function() { const tSwapUsed = all.reduce((s, n) => s + ((n.memInfo.SwapTotal || 0) - (n.memInfo.SwapFree || 0)), 0); const tSwapTotal = all.reduce((s, n) => s + (n.memInfo.SwapTotal || 0), 0); return H.card('스왑', gauge(tSwapTotal > 0 ? tSwapUsed / tSwapTotal * 100 : 0, fmtBytes(tSwapUsed) + '/' + fmtBytes(tSwapTotal)), 'text-center'); })()
  + H.card('소켓', '<div class="stat-lg color-cyan">' + all.reduce((s, n) => s + (n.conntrack || 0), 0) + '</div><div class="stat-label">connections</div>', 'text-center')
  );
  h += '<div class="sg grid-3 mb-12">';
  all.forEach(n => { const hi = monHist[n.ip] || { cpu: [], mem: [], netRx: [], netTx: [] };
    h += '<div class="hc"><h4>' + n.node + ' <span class="stat-label">' + n.ip + '</span>' + (n.uptime ? ' <span class="stat-label">up ' + fmtUptime(n.uptime) + '</span>' : '') + '</h4>';
    h += '<div class="flex gap-8 mb-6"><div class="flex-1"><div class="stat-label mb-2">CPU ' + (n.cpu || 0).toFixed(1) + '%</div><canvas id="mc-' + n.ip + '-cpu" class="sparkline"></canvas></div>';
    h += '<div class="flex-1"><div class="stat-label mb-2">MEM ' + (n.mem || 0).toFixed(1) + '%</div><canvas id="mc-' + n.ip + '-mem" class="sparkline"></canvas></div></div>';
    h += H.row('Temp', (n.temp || 0).toFixed(1) + '\u00B0C');
    h += H.row('Load', (n.load1 || n.load || 0).toFixed(2) + ' / ' + (n.load5 || 0).toFixed(2) + ' / ' + (n.load15 || 0).toFixed(2));
    h += H.row('RAM', ((n.ram_total || 0) / 1073741824).toFixed(1) + ' GB');
    h += H.row('Net I/O', '<span class="color-cyan">&#9650; ' + fmtRate(hi.netRx, hi.netRx.length - 1) + ' &#9660; ' + fmtRate(hi.netTx, hi.netTx.length - 1) + '</span>');
    h += H.row('Sockets', (n.sockstat.sockets_used || 0) + ' (TCP:' + (n.sockstat.TCP_inuse || 0) + ' UDP:' + (n.sockstat.UDP_inuse || 0) + ')') + '</div>'; });
  h += '</div>';

  h += '<div class="sg grid-3 mb-12">';
  h += '<div class="hc"><h4 class="color-red">&#9888; 이상 징후</h4><div style="font-size:12px;color:var(--fg2);margin-bottom:8px;line-height:1.6;border-left:2px solid var(--red);padding-left:8px"><b>Z-Score</b> 기반 이상 탐지<br>&#8226; 최근 60개 샘플(약 5분)<br>&#8226; Z &gt; 1.5 경고, Z &gt; 2.5 위험</div>';
  const totalAnom = all.reduce((s, n) => s + (n.anomaly_active || 0), 0);
  h += H.row('Active', '<span style="color:' + (totalAnom > 0 ? 'var(--red)' : 'var(--green)') + '">' + totalAnom + '</span>');
  h += H.row('Total Alerts', Math.round(all.reduce((s, n) => s + (n.anomaly_total || 0), 0)));
  all.forEach(n => { const scores = n.anomaly_scores || {}; const keys = Object.keys(scores).filter(k => scores[k] > 1.5);
    if (keys.length > 0) { h += '<div class="stat-label mt-4">' + n.node + ':</div>'; keys.forEach(k => { const z = scores[k]; h += '<div class="stat-label" style="padding-left:8px"><span style="color:' + (z > 2.5 ? 'var(--red)' : 'var(--yellow)') + '">Z=' + z.toFixed(1) + '</span> ' + k.replace('purecvisor_', '').replace('node_', '') + '</div>'; }); } });
  h += '</div>';
  h += '<div class="hc"><h4 class="color-cyan">&#128200; 5분 예측</h4><div style="font-size:12px;color:var(--fg2);margin-bottom:8px;line-height:1.6;border-left:2px solid var(--cyan);padding-left:8px"><b>EMA + OLS</b> 기반 추세 예측<br>&#8226; EMA alpha=0.3 + 선형 회귀 기울기</div>';
  all.forEach(n => { if (n.cpu_pred === undefined) return;
    const cpuDir = n.cpu_trend > 0.01 ? '&#9650;' : n.cpu_trend < -0.01 ? '&#9660;' : '&#9654;';
    const memDir = n.mem_trend > 0.01 ? '&#9650;' : n.mem_trend < -0.01 ? '&#9660;' : '&#9654;';
    h += '<div class="text-11 mb-4"><b>' + n.node + '</b></div>';
    h += H.row('CPU', (n.cpu || 0).toFixed(1) + '% \u2192 <span style="color:' + (n.cpu_pred > 80 ? 'var(--red)' : 'var(--green)') + '">' + n.cpu_pred.toFixed(1) + '%</span> ' + cpuDir);
    h += H.row('MEM', (n.mem || 0).toFixed(1) + '% \u2192 <span style="color:' + (n.mem_pred > 85 ? 'var(--red)' : 'var(--green)') + '">' + n.mem_pred.toFixed(1) + '%</span> ' + memDir); });
  h += '</div>';
  h += '<div class="hc"><h4 class="color-green">&#9889; 자동 복구 준비 상태</h4><div style="font-size:12px;color:var(--fg2);margin-bottom:8px;line-height:1.6;border-left:2px solid var(--green);padding-left:8px"><b>정책 기반 자동 복구 준비 정보</b><br>&#8226; 기본값은 DRY RUN으로 유지됩니다.</div>';
  h += H.row('Mode', H.badge('DRY RUN', 'y'));
  h += H.row('Pending', all.reduce((s, n) => s + (n.healing_pending || 0), 0));
  h += H.row('Executed', Math.round(all.reduce((s, n) => s + (n.healing_total || 0), 0)));
  const n1 = all[0] || {}; const agentProv = n1.agent_prov || {};
  if (Object.keys(agentProv).length > 0) { h += '<div style="margin-top:6px;border-top:1px solid var(--border);padding-top:6px"><div class="stat-label font-bold color-accent">&#129302; AI Agent Providers</div><table class="text-xs"><thead><tr><th>Provider</th><th>Conf</th><th>Latency</th></tr></thead><tbody>';
    Object.entries(agentProv).forEach(([name, d]) => { h += '<tr><td>' + name + '</td><td>' + (d.confidence || 0).toFixed(2) + '</td><td>' + (d.latency || 0).toFixed(0) + 'ms</td></tr>'; });
    h += '</tbody></table>'; if (n1.agent_conf !== undefined) h += '<div class="stat-label mt-4">Consensus <span class="color-green font-bold">' + n1.agent_conf.toFixed(2) + '</span></div>'; h += '</div>'; }
  h += '<div style="margin-top:6px"><button class="btn" onclick="showAgentConfig()" class="btn-xs">&#9881; Configure AI Agent</button></div></div></div>';

  h += '<div class="hc mb-14"><h4 class="color-yellow">&#9888; ' + _L('자가치유 대기 액션', 'Self-Healing Pending Actions') + '</h4>';
  h += '<div id="healing-pending-list" class="skeleton-box" style="min-height:60px"></div></div>';

  h += '<div class="sg grid-1 mb-12">' + H.card('&#9741; keepalived VRRP Status', '<table class="text-12"><thead><tr><th>Node</th><th>keepalived</th><th>VRRP Role</th><th>VIP Owner</th></tr></thead><tbody>' +
  all.map(n => { const kaA = n.keepalived_active === 1; const kaM = n.keepalived_master === 1; const kaV = n.keepalived_vip_owner === 1;
    return '<tr><td><b>' + n.node + '</b> <span class="stat-label">' + n.ip + '</span></td><td>' + H.badge(kaA ? 'ACTIVE' : 'DOWN', kaA ? 'g' : 'r') + '</td><td>' + H.badge(kaM ? 'MASTER' : 'BACKUP', kaM ? 'g' : 'y') + '</td><td>' + (kaV ? '<span class="color-green font-bold">' + escapeHtml(_VIP || 'VIP') + '</span>' : '-') + '</td></tr>'; }).join('') +
  '</tbody></table>') + '</div>';

  const runVms = allVms.filter(v => v.running === 1);
  function top5Tbl(title, items, valFn, unit) { let t2 = '<div class="hc"><h4>' + title + '</h4><table class="text-11"><tbody>';
    items.forEach((v, i) => { t2 += '<tr><td class="w-16 color-muted">' + (i + 1) + '</td><td><b>' + v.name + '</b></td><td class="color-muted">' + v.node + '</td><td class="text-right font-bold color-accent">' + valFn(v) + unit + '</td></tr>'; });
    if (items.length === 0) t2 += '<tr><td colspan="4" class="color-muted">No running VMs</td></tr>'; return t2 + '</tbody></table></div>'; }
  h += H.grid(4,
    top5Tbl('Top 5 Memory', [...runVms].sort((a, b) => (b.memory_used_mb || 0) - (a.memory_used_mb || 0)).slice(0, 5), v => (v.memory_used_mb || 0).toLocaleString(), ' MB')
  + top5Tbl('Top 5 vCPU', [...runVms].sort((a, b) => (b.vcpu || 0) - (a.vcpu || 0)).slice(0, 5), v => v.vcpu || 0, '')
  + top5Tbl('Top 5 Disk I/O', [...runVms].sort((a, b) => (b.disk_rd_bytes || 0) - (a.disk_rd_bytes || 0)).slice(0, 5), v => fmtBytes(v.disk_rd_bytes || 0), '')
  + top5Tbl('Top 5 Network', [...runVms].sort((a, b) => ((b.net_rx_bytes || 0) + (b.net_tx_bytes || 0)) - ((a.net_rx_bytes || 0) + (a.net_tx_bytes || 0))).slice(0, 5), v => fmtBytes((v.net_rx_bytes || 0) + (v.net_tx_bytes || 0)), '')
  );
  h += H.card('All VMs (' + allVms.length + ')', '<table class="table-sticky"><thead><tr><th>Name</th><th>State</th><th>Node</th><th>vCPU</th><th>Max MB</th><th>Used MB</th></tr></thead><tbody>' +
  allVms.map(v => '<tr><td><b>' + v.name + '</b></td><td>' + H.badge(v.running === 1 ? 'running' : 'off', v.running === 1 ? 'g' : 'r') + '</td><td>' + v.node + '</td><td>' + (v.vcpu || '-') + '</td><td>' + (v.memory_max_mb || '-') + '</td><td>' + (v.memory_used_mb > 0 ? v.memory_used_mb : '-') + '</td></tr>').join('') +
  '</tbody></table>');

  h += '<div id="selfhealing-panel" class="hc mb-14" style="margin-top:24px"></div>';
  b.innerHTML = h;
  setTimeout(loadDeepHealth, 50);
  setTimeout(loadHealingPending, 100);


  setTimeout(function() { if (window.PCV && PCV.selfhealing) PCV.selfhealing.refresh(); }, 150);
  setTimeout(() => { all.forEach(n => { const hi = monHist[n.ip] || { cpu: [], mem: [] }; drawLine('mc-' + n.ip + '-cpu', hi.cpu, getChartColor('cpu'), '%'); drawLine('mc-' + n.ip + '-mem', hi.mem, getChartColor('mem'), '%'); }); }, 50);

  setTimeout(function() {
    if (typeof pcvTimeSeries !== 'function') return;
    var cpuSeries = all.map(function(n) {
      var hi = monHist[n.ip] || { cpu: [] };
      return { label: n.node || n.ip, data: hi.cpu.slice(-60) };
    });
    var memSeries = all.map(function(n) {
      var hi = monHist[n.ip] || { mem: [] };
      return { label: n.node || n.ip, data: hi.mem.slice(-60) };
    });
    var netSeries = [];
    all.forEach(function(n, i) {
      var hi = monHist[n.ip] || { netRx: [], netTx: [] };
      var rxMb = hi.netRx.slice(-60).map(function(v){ return (v || 0) / 1048576; });
      var txMb = hi.netTx.slice(-60).map(function(v){ return (v || 0) / 1048576; });
      netSeries.push({ label: (n.node || n.ip) + ' RX', data: rxMb });
      netSeries.push({ label: (n.node || n.ip) + ' TX', data: txMb });
    });
    pcvTimeSeries('pcv-chart-cpu', cpuSeries, { title: 'CPU %', unit: '%', max: 100, fill: false });
    pcvTimeSeries('pcv-chart-mem', memSeries, { title: 'Memory %', unit: '%', max: 100, fill: false });
    pcvTimeSeries('pcv-chart-net', netSeries, { title: 'Network MB/s', unit: ' MB/s', fill: false });
  }, 100);
}
window.renderMonOverview = renderMonOverview;

function renderMonCluster(b, all) {

  if (typeof pcvDestroyAllInContainer === 'function') pcvDestroyAllInContainer(b);
  destroyAllCharts();
  let h = H.section('&#9741; Cluster Status') + '<div class="sg grid-3 mb-12">';
  all.forEach(n => { h += '<div class="hc"><h4 class="justify-between">' + n.node + H.badge(n.error ? 'DOWN' : 'UP', n.error ? 'r' : 'g') + '</h4><div class="flex gap-12" style="justify-content:center;margin:10px 0">' + gauge(n.cpu || 0, 'CPU') + gauge(n.mem || 0, 'MEM') + gauge(n.disk || 0, 'DISK') + '</div>';
    h += H.row('IP', n.ip) + H.row('VMs', n.vms.length) + H.row('Running', '<span class="color-green">' + n.vms.filter(v => v.running === 1).length + '</span>') + H.row('RAM', ((n.ram_total || 0) / 1073741824).toFixed(1) + ' GB') + H.row('Load', (n.load1 || n.load || 0).toFixed(2)) + '</div>'; });
  h += '</div>';
  h += '<div class="sg grid-2 mb-12"><div class="hc"><h4>CPU Trend</h4><div class="flex flex-col gap-4">';
  all.forEach(n => { h += '<div><span class="stat-label">' + n.node + '</span><canvas id="ct-' + n.ip + '" class="sparkline-sm"></canvas></div>'; });
  h += '</div></div><div class="hc"><h4>Memory Trend</h4><div class="flex flex-col gap-4">';
  all.forEach(n => { h += '<div><span class="stat-label">' + n.node + '</span><canvas id="mt-' + n.ip + '" class="sparkline-sm"></canvas></div>'; });
  h += '</div></div></div>';
  h += H.card('HA Operations', '<div class="flex gap-10 mt-8"><button class="btn" onclick="haFailoverTest()">Failover Test</button><button class="btn" onclick="haMigrate()">Live Migrate VM</button><button class="btn" onclick="haReplicate()">ZFS Replicate</button></div>', 'mb-12');
  b.innerHTML = h;
  setTimeout(() => { const colors = [getChartColor('cpu'), getChartColor('net'), getChartColor('alt1')]; all.forEach((n, i) => { const hi = monHist[n.ip] || { cpu: [], mem: [] }; drawLine('ct-' + n.ip, hi.cpu, colors[i % 3], '%'); drawLine('mt-' + n.ip, hi.mem, colors[i % 3], '%'); }); }, 50);
}
window.renderMonCluster = renderMonCluster;

function renderMonHosts(b, all) {

  if (typeof pcvDestroyAllInContainer === 'function') pcvDestroyAllInContainer(b);
  destroyAllCharts();
  let h = H.section('&#128187; Host Performance');
  all.forEach(n => {
    h += '<div style="border:1px solid var(--border);border-radius:var(--r);padding:12px;margin-bottom:12px">';
    h += '<div class="justify-between items-center mb-10"><h4 class="text-14">' + n.node + ' <span class="stat-label">' + n.ip + '</span>' + (n.uptime ? ' <span class="stat-label">uptime ' + fmtUptime(n.uptime) + '</span>' : '') + '</h4>' + H.badge(n.error ? 'DOWN' : 'UP', n.error ? 'r' : 'g') + '</div>';
    h += H.grid(6,
      '<div class="hc text-center">' + gauge(n.cpu || 0, 'CPU') + '</div><div class="hc text-center">' + gauge(n.mem || 0, 'Memory') + '</div><div class="hc text-center">' + gauge(n.disk || 0, 'Disk') + '</div>'
    + H.card('Temperature', '<div class="stat-md" style="color:' + (n.temp > 55 ? 'var(--red)' : 'var(--green)') + '">' + (n.temp || 0).toFixed(1) + '\u00B0C</div>')
    + H.card('Load', '<div style="font-size:16px;font-weight:700">' + (n.load1 || n.load || 0).toFixed(2) + '</div>')
    + H.card('Total RAM', '<div class="stat-md">' + ((n.ram_total || 0) / 1073741824).toFixed(1) + ' GB</div>')
    );
    h += '<div class="sg grid-3">' + H.card('CPU History', '<canvas id="hc-' + n.ip + '" class="sparkline-md"></canvas>') + H.card('Memory History', '<canvas id="hm-' + n.ip + '" class="sparkline-md"></canvas>') + H.card('Disk History', '<canvas id="hd-' + n.ip + '" class="sparkline-md"></canvas>') + '</div>';

    const mi = n.memInfo || {};
    const mtotal = mi.MemTotal || 1;
    const mUsed = mtotal - (mi.MemAvailable || 0);
    const mCached = mi.Cached || 0;
    const mBuffers = mi.Buffers || 0;
    const mFree = mi.MemFree || 0;
    const mSlab = mi.Slab || 0;
    const pUsed = (mUsed / mtotal * 100).toFixed(2);
    const pBuf = (mBuffers / mtotal * 100).toFixed(2);
    const pCache = (mCached / mtotal * 100).toFixed(2);
    const pSlab = (mSlab / mtotal * 100).toFixed(2);
    const pFree = (mFree / mtotal * 100).toFixed(2);
    h += '<div class="hc mt-8"><h4>Memory Breakdown</h4><div style="display:flex;height:18px;border-radius:3px;overflow:hidden;margin-bottom:4px">'
       + '<div class="pcv-bar-fill-inline" style="--bw:' + pUsed + '%;--bc:var(--red)"></div>'
       + '<div class="pcv-bar-fill-inline" style="--bw:' + pBuf + '%;--bc:var(--yellow)"></div>'
       + '<div class="pcv-bar-fill-inline" style="--bw:' + pCache + '%;--bc:var(--accent)"></div>'
       + '<div class="pcv-bar-fill-inline" style="--bw:' + pSlab + '%;--bc:var(--magenta)"></div>'
       + '<div class="pcv-bar-fill-inline" style="--bw:' + pFree + '%;--bc:var(--green)"></div>'
       + '</div><div class="flex gap-12 stat-label flex-wrap">'
       + '<span>&#9632; <span class="color-red">Used</span> ' + fmtBytes(mUsed) + '</span>'
       + '<span>&#9632; <span class="color-yellow">Buf</span> ' + fmtBytes(mBuffers) + '</span>'
       + '<span>&#9632; <span class="color-accent">Cache</span> ' + fmtBytes(mCached) + '</span>'
       + '<span>&#9632; <span class="color-magenta">Slab</span> ' + fmtBytes(mSlab) + '</span>'
       + '<span>&#9632; <span class="color-green">Free</span> ' + fmtBytes(mFree) + '</span>'
       + '</div></div>';

    const coreIds = Object.keys(n.cores || {}).filter(c => parseInt(c) < 64).sort((a, b) => parseInt(a) - parseInt(b));
    if (coreIds.length > 0) { h += '<div class="hc mt-8"><h4>CPU per Core (' + coreIds.length + ')</h4><div class="flex" style="flex-wrap:wrap;gap:3px">';
      coreIds.forEach(c => { const cd = n.cores[c]; const total = Object.values(cd).reduce((s, v) => s + v, 0); const pct = total > 0 ? (1 - (cd.idle || 0) / total) * 100 : 0; const cl = pct > 80 ? 'var(--red)' : pct > 50 ? 'var(--yellow)' : 'var(--green)';
        h += '<div class="w-28 text-center" title="Core ' + c + ': ' + pct.toFixed(1) + '%"><div style="height:24px;background:var(--bg);border-radius:2px;border:1px solid var(--border);position:relative;overflow:hidden"><div style="position:absolute;bottom:0;width:100%;height:' + pct + '%;background:' + cl + '"></div></div><div style="font-size:8px;color:var(--fg2)">' + c + '</div></div>'; });
      h += '</div></div>'; }

    const ndevs = Object.entries(n.netdevs || {}).filter(([d]) => !['lo', 'ovs-system', 'br-int'].includes(d));
    if (ndevs.length > 0) { h += '<div class="hc mt-8"><h4>&#127760; Network Interfaces</h4><table class="text-11"><thead><tr><th>Device</th><th>RX</th><th>TX</th><th>Errors</th><th>Drops</th></tr></thead><tbody>';
      ndevs.forEach(([d, s]) => { h += '<tr><td><b>' + d + '</b></td><td>' + fmtBytes(s.receive_bytes_total || 0) + '</td><td>' + fmtBytes(s.transmit_bytes_total || 0) + '</td><td>' + (s.receive_errs_total || 0) + '</td><td>' + (s.receive_drop_total || 0) + '</td></tr>'; });
      h += '</tbody></table></div>'; }

    const ddevs = Object.entries(n.disks || {}).filter(([d]) => d.match(/^(nvme\d+n\d+|sd[a-z])$/));
    if (ddevs.length > 0) { h += '<div class="hc mt-8"><h4>&#128190; Disk I/O</h4><table class="text-11"><thead><tr><th>Device</th><th>Read</th><th>Written</th><th>IOPS</th></tr></thead><tbody>';
      ddevs.forEach(([d, s]) => { h += '<tr><td><b>' + d + '</b></td><td>' + fmtBytes(s.read_bytes_total || 0) + '</td><td>' + fmtBytes(s.written_bytes_total || 0) + '</td><td>' + (s.reads_completed_total || 0) + '/' + (s.writes_completed_total || 0) + '</td></tr>'; });
      h += '</tbody></table></div>'; }
    h += '</div>'; });
  b.innerHTML = h;
  setTimeout(() => { all.forEach(n => { const hi = monHist[n.ip] || { cpu: [], mem: [], disk: [] }; drawLine('hc-' + n.ip, hi.cpu, getChartColor('cpu'), '%'); drawLine('hm-' + n.ip, hi.mem, getChartColor('mem'), '%'); drawLine('hd-' + n.ip, hi.disk, getChartColor('disk'), '%'); }); }, 50);
}
window.renderMonHosts = renderMonHosts;

function renderMonVms(b, allVms, running) {

  if (typeof pcvDestroyAllInContainer === 'function') pcvDestroyAllInContainer(b);
  destroyAllCharts();
  let h = H.section('&#9881; Virtual Machines');
  h += H.grid(4,
    H.card('Total', '<div class="stat-xl">' + allVms.length + '</div>', 'text-center')
  + H.card('Running', '<div class="stat-xl color-green">' + running + '</div>', 'text-center')
  + H.card('Total vCPU', '<div class="stat-xl color-accent">' + allVms.reduce((s, v) => s + (v.vcpu || 0), 0) + '</div>', 'text-center')
  + H.card('Total Memory', '<div class="stat-xl">' + (allVms.reduce((s, v) => s + (v.memory_max_mb || 0), 0) / 1024).toFixed(1) + ' GB</div>', 'text-center')
  );
  h += '<div class="sg grid-2">';
  allVms.forEach(v => { const on = v.running === 1; const memPct = v.memory_max_mb > 0 && v.memory_used_mb > 0 ? v.memory_used_mb / v.memory_max_mb * 100 : 0;
    h += '<div class="hc"><div class="justify-between mb-8"><h4>' + v.name + '</h4>' + H.badge(on ? 'running' : 'off', on ? 'g' : 'r') + '</div><div class="sg grid-3"><div class="text-center">' + gauge(on ? memPct : 0, 'Memory') + '</div><div>' + H.row('vCPU', v.vcpu || '-') + H.row('Max RAM', (v.memory_max_mb || '-') + ' MB') + '</div><div>' + H.row('Used RAM', (v.memory_used_mb > 0 ? v.memory_used_mb + ' MB' : '-')) + H.row('Node', v.node) + '</div></div></div>'; });
  h += '</div>'; b.innerHTML = h;
}
window.renderMonVms = renderMonVms;

function _aggregateZfsLocks(all) {
  var total = _newZfsLocks();
  (all || []).forEach(function(n) {
    var s = n.zfsLocks || _newZfsLocks();
    ['total', 'ok', 'busy', 'error', 'unknown', 'waitSumMs', 'waitCount'].forEach(function(k) {
      total[k] += s[k] || 0;
    });
    Object.keys(s.byOp || {}).forEach(function(op) {
      var src = s.byOp[op];
      var dst = _zfsLockOp(total, op);
      ['total', 'ok', 'busy', 'error', 'unknown', 'waitSumMs', 'waitCount'].forEach(function(k) {
        dst[k] += src[k] || 0;
      });
    });
  });
  return total;
}

function _zfsLockPanel(all) {
  var s = _aggregateZfsLocks(all);
  var avgWait = s.waitCount > 0 ? s.waitSumMs / s.waitCount : 0;
  var h = '<div class="hc mb-12"><h4>ZFS inflight lock</h4>';
  h += '<p class="color-muted text-11 mb-8">' + _L('ADR-0021 분산 락 획득 결과와 대기 시간을 표시합니다.', 'Shows ADR-0021 distributed lock acquisition results and wait time.') + '</p>';
  if (s.total <= 0 && s.waitCount <= 0) {
    h += '<div class="empty-state" style="padding:18px;text-align:left"><div class="empty-state-text">No ZFS inflight lock samples yet</div>';
    h += '<div class="color-muted text-12">' + _L('샘플은 ZFS create/destroy 작업이 실행된 뒤 Prometheus metric에서 집계됩니다.', 'Samples appear after ZFS create/destroy operations publish Prometheus metrics.') + '</div></div></div>';
    return h;
  }
  h += '<div class="sg grid-5 mb-8">';
  h += H.card('Total', '<div class="stat-lg color-accent">' + Math.round(s.total).toLocaleString() + '</div>', 'text-center');
  h += H.card('OK', '<div class="stat-lg color-green">' + Math.round(s.ok || 0).toLocaleString() + '</div>', 'text-center');
  h += H.card('Busy', '<div class="stat-lg color-yellow">' + Math.round(s.busy || 0).toLocaleString() + '</div>', 'text-center');
  h += H.card('Error', '<div class="stat-lg color-red">' + Math.round(s.error || 0).toLocaleString() + '</div>', 'text-center');
  h += H.card('Avg wait', '<div class="stat-lg color-cyan">' + avgWait.toFixed(1) + 'ms</div>', 'text-center');
  h += '</div>';
  h += '<table class="text-11"><thead><tr><th>Op</th><th>Total</th><th>OK</th><th>Busy</th><th>Error</th><th>Avg wait</th></tr></thead><tbody>';
  Object.keys(s.byOp).sort().forEach(function(op) {
    var o = s.byOp[op];
    var ow = o.waitCount > 0 ? o.waitSumMs / o.waitCount : 0;
    h += '<tr><td><b>' + esc(op) + '</b></td><td>' + Math.round(o.total).toLocaleString() + '</td><td class="color-green">' + Math.round(o.ok || 0).toLocaleString() + '</td><td class="color-yellow">' + Math.round(o.busy || 0).toLocaleString() + '</td><td class="color-red">' + Math.round(o.error || 0).toLocaleString() + '</td><td>' + ow.toFixed(1) + 'ms</td></tr>';
  });
  h += '</tbody></table></div>';
  return h;
}

function renderMonStorage(b, all, allVms, totalRam) {

  if (typeof pcvDestroyAllInContainer === 'function') pcvDestroyAllInContainer(b);
  destroyAllCharts();
  let h = H.section('&#128190; Storage & Capacity');
  h += H.card('Datastore Usage', all.map(n => { const pct = n.disk || 0; const cl = pct > 80 ? 'var(--red)' : pct > 60 ? 'var(--yellow)' : 'var(--green)';
    return '<div class="mb-8"><div class="justify-between" style="font-size:11px;margin-bottom:2px"><span>' + n.node + '</span><span style="color:' + cl + '">' + pct.toFixed(1) + '%</span></div><div style="height:20px;background:var(--bg);border-radius:3px;border:1px solid var(--border);overflow:hidden"><div style="height:100%;width:' + pct + '%;background:' + cl + ';border-radius:3px"></div></div></div>'; }).join(''), 'mb-12');
  const vmMem = allVms.reduce((s, v) => s + (v.memory_max_mb || 0), 0);
  h += H.grid(3,
    H.card('Total Cluster RAM', '<div class="stat-xl color-accent">' + (totalRam / 1073741824).toFixed(1) + ' GB</div>', 'text-center')
  + H.card('VM Provisioned', '<div class="stat-xl color-yellow">' + (vmMem / 1024).toFixed(1) + ' GB</div>', 'text-center')
  + H.card('Overcommit', gauge(totalRam > 0 ? vmMem * 1048576 / totalRam * 100 : 0, 'RAM'), 'text-center')
  ) + '<div class="mb-12"></div>';
  h += _zfsLockPanel(all);
  h += '<div class="sg grid-2 mb-12"><div class="hc"><h4>Disk Usage Trend</h4>';
  all.forEach(n => { h += '<div class="mb-4"><span class="stat-label">' + n.node + '</span><canvas id="sd-' + n.ip + '" class="sparkline-sm"></canvas></div>'; });
  h += '</div><div class="hc"><h4>Memory per Node</h4>';
  all.forEach(n => { const gb = (n.ram_total || 0) / 1073741824; h += '<div class="mb-6"><div class="justify-between text-11"><span>' + n.node + '</span><span>' + gb.toFixed(1) + ' GB</span></div><div style="height:14px;background:var(--bg);border-radius:3px;border:1px solid var(--border);overflow:hidden"><div style="height:100%;width:' + (gb / 64 * 100) + '%;background:var(--accent);border-radius:3px"></div></div></div>'; });
  h += '</div></div>';

  all.forEach(n => { const fsList = (n.filesystems || []).filter(f => f.fstype === 'zfs' || f.fstype === 'ext4' || f.fstype === 'xfs');
    if (fsList.length === 0) return;
    h += '<div class="hc mb-12"><h4>' + n.node + ' \u2014 Filesystems</h4><table class="text-11"><thead><tr><th>Mount</th><th>Type</th><th>Size</th><th>Avail</th><th>Used %</th></tr></thead><tbody>';
    fsList.forEach(f => { const sz = f.size_bytes || 0; const av = f.avail_bytes || 0; const pct = sz > 0 ? (sz - av) / sz * 100 : 0; h += '<tr><td><b>' + f.mount + '</b></td><td>' + f.fstype + '</td><td>' + fmtBytes(sz) + '</td><td>' + fmtBytes(av) + '</td><td style="color:' + (pct > 85 ? 'var(--red)' : 'var(--green)') + '">' + pct.toFixed(1) + '%</td></tr>'; });
    h += '</tbody></table></div>'; });

  h += H.card('Disk I/O (All Nodes)', '<table class="text-11"><thead><tr><th>Node</th><th>Device</th><th>Read</th><th>Written</th><th>Read IOPS</th><th>Write IOPS</th></tr></thead><tbody>' +
  all.map(n => Object.entries(n.disks || {}).filter(([d]) => d.match(/^(nvme\d+n\d+|sd[a-z])$/)).map(([d, s]) =>
    '<tr><td>' + n.node + '</td><td><b>' + d + '</b></td><td>' + fmtBytes(s.read_bytes_total || 0) + '</td><td>' + fmtBytes(s.written_bytes_total || 0) + '</td><td>' + (s.reads_completed_total || 0).toLocaleString() + '</td><td>' + (s.writes_completed_total || 0).toLocaleString() + '</td></tr>').join('')).join('') +
  '</tbody></table>', 'mb-12');
  b.innerHTML = h;
  setTimeout(() => { const colors = [getChartColor('disk'), getChartColor('alt2'), getChartColor('alt3')]; all.forEach((n, i) => { const hi = monHist[n.ip] || { disk: [] }; drawLine('sd-' + n.ip, hi.disk, colors[i % 3], '%'); }); }, 50);
}
window.renderMonStorage = renderMonStorage;


async function renderAlerts(b) {
  b.innerHTML = showSkeleton();
  const hdr = { 'Authorization': 'Bearer ' + authToken, 'Content-Type': 'application/json' };
  const [cfgR, histR] = await Promise.all([
    fetch(EP.ALERTS_CONFIG(), { headers: hdr }).then(r => r.json()).catch(() => ({})),
    fetch(EP.ALERTS(), { headers: hdr }).then(r => r.json()).catch(() => [])
  ]);
  const cfg = unwrapData(cfgR) || {};
  const hist = unwrapList(histR);
  const v = (k, d) => cfg[k] !== undefined ? cfg[k] : d;
  let h = H.section('&#128276; Alert Configuration');
  const en = v('enabled', false);
  h += '<div class="flex items-center gap-12 mb-16" style="padding:10px 14px;background:' + (en ? 'rgba(0,255,136,.08)' : 'rgba(255,34,102,.08)') + ';border:1px solid ' + (en ? 'var(--green)' : 'var(--red)') + ';border-radius:var(--r)">' + H.badge(en ? 'ENABLED' : 'DISABLED', en ? 'g' : 'r') + '<span class="text-12">' + (en ? t('alert.enabled') : t('alert.disabled')) + '</span><label style="margin-left:auto;cursor:pointer;display:flex;align-items:center;gap:6px"><input type="checkbox" id="al-enabled" ' + (en ? 'checked' : '') + ' onchange="alertSave()"><span class="text-xs">Enable</span></label></div>';
  h += '<div class="sg grid-3 mb-16">';
  [{ name: 'CPU', warn: 'cpu_warn', crit: 'cpu_crit' }, { name: 'Memory', warn: 'mem_warn', crit: 'mem_crit' }, { name: 'Disk', warn: 'disk_warn', crit: 'disk_crit' }].forEach(m => {
    const wv = v(m.warn, 80), cv2 = v(m.crit, 95);
    h += '<div class="hc"><h4>' + m.name + ' Thresholds</h4><div style="margin:8px 0">' + H.row('Warning (%)', '<input type="number" id="al-' + m.warn + '" value="' + wv + '" min="0" max="100" style="width:60px;background:var(--bg);color:var(--yellow);border:1px solid var(--border);border-radius:3px;padding:2px 6px;text-align:center">') + H.row('Critical (%)', '<input type="number" id="al-' + m.crit + '" value="' + cv2 + '" min="0" max="100" style="width:60px;background:var(--bg);color:var(--red);border:1px solid var(--border);border-radius:3px;padding:2px 6px;text-align:center">') + '</div>';
    h += '<div style="height:8px;background:var(--bg);border-radius:4px;overflow:hidden;position:relative;margin-top:4px"><div style="position:absolute;left:0;width:' + wv + '%;height:100%;background:var(--green)"></div><div style="position:absolute;left:' + wv + '%;width:' + (cv2 - wv) + '%;height:100%;background:var(--yellow)"></div><div style="position:absolute;left:' + cv2 + '%;width:' + (100 - cv2) + '%;height:100%;background:var(--red)"></div></div></div>'; });
  h += '</div>';
  h += '<div class="sg grid-2 mb-16">';
  h += H.card('Evaluation Period', H.row('Hold time (sec)', '<input type="number" id="al-eval_period" value="' + v('eval_period', 30) + '" min="5" max="600" style="width:80px;background:var(--bg);color:var(--fg);border:1px solid var(--border);border-radius:3px;padding:2px 6px;text-align:center">'));
  h += '<div class="hc"><h4>Webhook</h4>' + H.row('Format', '<select id="al-webhook_format" class="input-pcv">' + ['slack', 'telegram', 'generic'].map(f => '<option value="' + f + '"' + (v('webhook_format', 'generic') === f ? ' selected' : '') + '>' + f + '</option>').join('') + '</select>') + H.row('URL', '<input type="text" id="al-webhook_url" value="' + v('webhook_url', '') + '" placeholder="https://hooks.slack.com/..." style="width:100%;background:var(--bg);color:var(--fg);border:1px solid var(--border);border-radius:3px;padding:3px 8px;font-size:11px">') + H.row('Telegram Chat ID', '<input type="text" id="al-telegram_chat_id" value="' + v('telegram_chat_id', '') + '" placeholder="optional" style="width:120px;background:var(--bg);color:var(--fg);border:1px solid var(--border);border-radius:3px;padding:2px 6px;font-size:11px">') + '</div></div>';
  h += '<div class="mb-16"><button onclick="alertSave()" style="background:linear-gradient(135deg,var(--accent),var(--green));color:var(--bg);border:none;padding:8px 24px;border-radius:var(--r);cursor:pointer;font-weight:700;font-size:12px;text-transform:uppercase">' + t('btn.save') + '</button><span id="al-status" style="margin-left:12px;font-size:11px"></span></div>';
  h += H.card('&#128276; Alert History (' + hist.length + ' events)',
    hist.length === 0 ? '<div class="color-muted text-xs" style="padding:8px">No alerts fired yet</div>' :
    '<table class="text-11"><thead><tr><th>Time</th><th>Severity</th><th>Metric</th><th>Value</th><th>Message</th></tr></thead><tbody>' +
    [...hist].reverse().map(a => '<tr><td>' + new Date(a.timestamp * 1000).toLocaleString() + '</td><td>' + H.badge(a.severity.toUpperCase(), a.severity === 'crit' ? 'r' : 'y') + '</td><td>' + a.metric + '</td><td>' + a.value.toFixed(1) + '%</td><td class="color-muted">' + a.message + '</td></tr>').join('') +
    '</tbody></table>');
  h += '<div class="hc mt-12"><h4 class="color-yellow">&#128232; \uC6F9\uD6C5 \uC804\uC1A1 \uC2E4\uD328 (DLQ)</h4>';
  h += '<div class="mt-8"><button class="btn" onclick="loadWebhookDlq()">DLQ \uC870\uD68C</button>';
  h += '<button class="btn btn-g" onclick="retryWebhookDlq()" style="margin-left:6px">\uC804\uCCB4 \uC7AC\uC2DC\uB3C4</button></div>';
  h += '<div id="dlq-list" class="mt-8"></div></div>';

  h += H.section(_L('알림 설정', 'Alert Configuration'));
  try {
    var cfg2 = await fetchGet(EP.ALERTS_CONFIG());
    var c = unwrapData(cfg2);
    h += '<div class="sg grid-2">';
    h += H.card('CPU ' + _L('임계값', 'Thresholds'),
      '<div class="fr"><label>Warning (%)</label><input type="range" id="alert-cpu-warn" min="50" max="100" value="' + (c.cpu_warn || 80) + '" oninput="document.getElementById(\'acw-val\').textContent=this.value+\'%\'" class="flex-1"><span id="acw-val" class="min-w-40 text-right">' + (c.cpu_warn || 80) + '%</span></div>'
      + '<div class="fr"><label>Critical (%)</label><input type="range" id="alert-cpu-crit" min="50" max="100" value="' + (c.cpu_crit || 95) + '" oninput="document.getElementById(\'acc-val\').textContent=this.value+\'%\'" class="flex-1"><span id="acc-val" class="min-w-40 text-right">' + (c.cpu_crit || 95) + '%</span></div>');
    h += H.card(_L('메모리 임계값', 'Memory Thresholds'),
      '<div class="fr"><label>Warning (%)</label><input type="range" id="alert-mem-warn" min="50" max="100" value="' + (c.mem_warn || 85) + '" oninput="document.getElementById(\'amw-val\').textContent=this.value+\'%\'" class="flex-1"><span id="amw-val" class="min-w-40 text-right">' + (c.mem_warn || 85) + '%</span></div>'
      + '<div class="fr"><label>Critical (%)</label><input type="range" id="alert-mem-crit" min="50" max="100" value="' + (c.mem_crit || 95) + '" oninput="document.getElementById(\'amc-val\').textContent=this.value+\'%\'" class="flex-1"><span id="amc-val" class="min-w-40 text-right">' + (c.mem_crit || 95) + '%</span></div>');
    h += '</div>';
    h += '<div class="flex gap-6 mt-8"><button class="btn btn-g" onclick="saveAlertConfig()">' + t('btn.save') + '</button></div>';
  } catch (e) { h += '<p class="color-muted">Alert config unavailable</p>'; }

  b.innerHTML = h;
}
window.renderAlerts = renderAlerts;

async function saveAlertConfig() {
  var body = {
    cpu_warn: parseInt(document.getElementById('alert-cpu-warn')?.value) || 80,
    cpu_crit: parseInt(document.getElementById('alert-cpu-crit')?.value) || 95,
    mem_warn: parseInt(document.getElementById('alert-mem-warn')?.value) || 85,
    mem_crit: parseInt(document.getElementById('alert-mem-crit')?.value) || 95,
  };
  try {
    var r = await fetchPut(EP.ALERTS_CONFIG(), body);
    if (r.error) { toast(r.error.message, false); return; }
    toast(t('alert.saved'));
    addEvt('Alert config updated: CPU ' + body.cpu_warn + '/' + body.cpu_crit + ', MEM ' + body.mem_warn + '/' + body.mem_crit);
  } catch (e) { toast(e.message, false); }
}
window.saveAlertConfig = saveAlertConfig;


async function renderAudit(b) {
  let h = H.section('&#128270; \uAC10\uC0AC \uB85C\uADF8 \uAC80\uC0C9');
  h += '<div class="sg" style="grid-template-columns:1fr;margin-bottom:12px">';
  h += '<div class="hc">';
  h += '<div style="display:flex;gap:8px;flex-wrap:wrap;margin-bottom:8px">';
  h += '<input id="audit-user" placeholder="\uC0AC\uC6A9\uC790" style="padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:6px;font-size:12px;width:120px">';
  h += '<input id="audit-method" placeholder="\uBA54\uC11C\uB4DC (\uC608: vm.delete)" style="padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:6px;font-size:12px;width:180px">';
  h += '<input id="audit-from" type="date" class="input-pcv-lg">';
  h += '<input id="audit-to" type="date" class="input-pcv-lg">';
  h += '<button class="btn btn-g" onclick="doAuditSearch()">&#128269; \uAC80\uC0C9</button>';
  h += '</div>';
  h += '<div id="audit-results"><p class="color-muted text-12">\uAC80\uC0C9 \uC870\uAC74\uC744 \uC785\uB825\uD558\uACE0 \uAC80\uC0C9 \uBC84\uD2BC\uC744 \uD074\uB9AD\uD558\uC138\uC694.</p></div>';
  h += '</div></div>';
  b.innerHTML = h;
}
window.renderAudit = renderAudit;


async function renderGpu(b) {
  let h = H.section('&#127918; GPU \uBAA8\uB2C8\uD130\uB9C1');
  h += '<div class="sg grid-2 mb-16">';
  h += '<div class="hc"><h4>&#127918; GPU \uB514\uBC14\uC774\uC2A4</h4>';
  h += '<p class="color-muted text-12 mb-8">lspci \uAE30\uBC18 GPU \uC5F4\uAC70 \uBC0F vGPU/VFIO \uD328\uC2A4\uC2A4\uB8E8 \uC0C1\uD0DC\uB97C \uC870\uD68C\uD569\uB2C8\uB2E4.</p>';
  h += '<button class="btn btn-g" onclick="testGpuList()">&#128260; GPU \uBAA9\uB85D \uC870\uD68C</button>';
  h += '<div id="gpu-list-result" class="mt-8"></div></div>';
  h += '<div class="hc"><h4>&#9881; GPU \uC791\uC5C5</h4>';
  h += '<div class="mb-8">';
  h += '<div class="fr"><label>PCI Address</label><input id="gpu-pci" placeholder="0000:01:00.0" class="w-160"></div>';
  h += '<div class="fr"><label>VM Name</label><input id="gpu-vm" placeholder="gpu-vm-01" class="w-140"></div>';
  h += '<div class="flex gap-6 flex-wrap">';
  h += '<button class="btn" onclick="gpuPassthrough()">VFIO Passthrough</button>';
  h += '<button class="btn" onclick="gpuMdevCreate()">vGPU \uC0DD\uC131</button>';
  h += '</div></div>';
  h += '<div id="gpu-action-result" class="mt-8"></div></div>';
  h += '</div>';
  h += H.card('&#128214; CLI \uBA85\uB839\uC5B4 \uCC38\uC870', '<div style="font-size:12px;line-height:1.8;color:var(--fg2)">' +
    '<code class="color-accent">pcvctl gpu list</code> &mdash; GPU \uB514\uBC14\uC774\uC2A4 \uBAA9\uB85D<br>' +
    '<code class="color-accent">pcvctl gpu metrics</code> &mdash; GPU \uBA54\uD2B8\uB9AD \uC870\uD68C<br>' +
    '<code class="color-accent">pcvctl gpu passthrough &lt;pci&gt; &lt;vm&gt;</code> &mdash; VFIO \uD328\uC2A4\uC2A4\uB8E8<br>' +
    '<code class="color-accent">pcvctl gpu mdev create &lt;pci&gt; &lt;type&gt;</code> &mdash; vGPU \uC0DD\uC131</div>');

  h += '<div class="hc mb-14"><h4>' + _L('GPU 활용률', 'GPU Utilization') + '</h4>';
  h += '<canvas id="gpu-chart" width="600" height="200" style="max-width:100%"></canvas>';
  h += '<div class="stat-label mt-8">' + _L('GPU 메트릭은 nvidia-smi 또는 lspci 기반으로 수집됩니다.', 'GPU metrics collected via nvidia-smi or lspci.') + '</div></div>';
  b.innerHTML = h;

  try {
    var gr = await fetchPost(EP.RPC(), {jsonrpc:'2.0', method:'gpu.list', params:{}, id:'gl1'});
    var gpus = unwrapList(gr);
    var canvas = document.getElementById('gpu-chart');
    if (canvas && gpus.length > 0) {
      var ctx = canvas.getContext('2d');
      var barW = Math.min(80, (canvas.width - 40) / gpus.length);
      gpus.forEach(function(g, i) {
        var util = g.utilization || 0;
        var barH = (util / 100) * 160;
        ctx.fillStyle = util > 80 ? '#ff4444' : util > 50 ? '#ffaa00' : '#00ff88';
        ctx.fillRect(20 + i * (barW + 10), 180 - barH, barW, barH);
        ctx.fillStyle = '#aaa';
        ctx.font = '10px monospace';
        ctx.fillText(esc(g.name || 'GPU' + i).substring(0, 10), 20 + i * (barW + 10), 195);
        ctx.fillText(util + '%', 20 + i * (barW + 10) + barW/2 - 10, 175 - barH);
      });
    }
  } catch(e) { if(_DEBUG) console.warn('gpu-chart:', e.message); }
}
window.renderGpu = renderGpu;


async function renderDpdk(b) {
  b.innerHTML = showSkeleton();
  try {
    const [status, list, hugepage] = await Promise.all([
      fetchGet(EP.DPDK_STATUS()).catch(() => ({})),
      fetchGet(EP.DPDK_LIST()).catch(() => ({ data: [] })),
      fetchGet(EP.DPDK_HUGEPAGE()).catch(() => ({}))
    ]);
    const sd = unwrapData(status);
    const dl = unwrapList(list);
    const hp = unwrapData(hugepage);
    let h = H.section('DPDK — Data Plane Development Kit');
    h += H.grid(3,
      H.card('DPDK Status', H.row('Available', H.badge(sd.available ? 'Yes' : 'No', sd.available ? 'g' : 'r')) + H.row('PMD CPU Mask', esc(sd.pmd_cpu_mask || '-')) + H.row('Socket Mem', esc(sd.socket_mem || '-')))
    + H.card('HugePages', H.row('2M Total', hp.hugepage_2m_total || 0) + H.row('2M Free', hp.hugepage_2m_free || 0) + H.row('1G Total', hp.hugepage_1g_total || 0) + H.row('1G Free', hp.hugepage_1g_free || 0))
    + H.card('Bound NICs', '<div class="stat-lg">' + (Array.isArray(dl) ? dl.length : 0) + '</div>')
    );
    if (Array.isArray(dl) && dl.length > 0) {
      h += '<table class="table-sticky"><thead><tr><th>PCI Addr</th><th>Driver</th><th>Device</th></tr></thead><tbody>';
      dl.forEach(d => { h += '<tr><td><code>' + esc(d.pci_addr || d.pci || '?') + '</code></td><td>' + esc(d.driver || '-') + '</td><td>' + esc(d.device || '-') + '</td></tr>'; });
      h += '</tbody></table>';
    }
    h += H.section('DPDK Operations');
    h += '<div class="sg grid-2">';
    h += H.card('Bind NIC to DPDK', '<div class="fr"><label>PCI Address</label><input id="dpdk-pci" placeholder="0000:03:00.0" class="w-full"></div><div class="fr"><label>Driver</label><input id="dpdk-drv" value="vfio-pci" class="w-full"></div><button class="btn btn-g" onclick="dpdkBind()" class="mt-8">Bind</button>');
    h += H.card('Unbind NIC', '<div class="fr"><label>PCI Address</label><input id="dpdk-unbind-pci" placeholder="0000:03:00.0" class="w-full"></div><button class="btn btn-r" onclick="dpdkUnbind()" class="mt-8">Unbind</button>');
    h += '</div>';
    b.innerHTML = h;
  } catch (e) { b.innerHTML = H.section('DPDK') + '<p class="color-muted">Failed to load</p>'; }
}

async function dpdkBind() {
  var pci = document.getElementById('dpdk-pci')?.value;
  var drv = document.getElementById('dpdk-drv')?.value || 'vfio-pci';
  if (!pci) { toast(t('msg.name_required'), false); return; }
  try {
    var r = await fetchPost(EP.DPDK_BIND(), { pci_addr: pci, driver: drv });
    if (r.error) { toast('Bind failed: ' + (r.error.message || ''), false); return; }
    toast('DPDK bind: ' + pci); addEvt('DPDK bind ' + pci);
    renderDpdk(document.getElementById('cb'));
  } catch (e) { toast(e.message, false); }
}
async function dpdkUnbind() {
  var pci = document.getElementById('dpdk-unbind-pci')?.value;
  if (!pci) { toast(t('msg.name_required'), false); return; }
  try {
    var r = await fetchPost(EP.DPDK_UNBIND(), { pci_addr: pci });
    if (r.error) { toast('Unbind failed: ' + (r.error.message || ''), false); return; }
    toast('DPDK unbind: ' + pci); addEvt('DPDK unbind ' + pci);
    renderDpdk(document.getElementById('cb'));
  } catch (e) { toast(e.message, false); }
}

window.renderDpdk = renderDpdk;
window.dpdkBind = dpdkBind;
window.dpdkUnbind = dpdkUnbind;


async function renderSriov(b) {
  b.innerHTML = showSkeleton();
  try {
    const [status, list] = await Promise.all([
      fetchGet(EP.SRIOV_STATUS()).catch(() => ({})),
      fetchGet(EP.SRIOV_LIST()).catch(() => ({ data: [] }))
    ]);
    const sd = unwrapData(status);
    const vfs = unwrapList(list);
    let h = H.section('SR-IOV — Single Root I/O Virtualization');
    h += H.grid(2,
      H.card('SR-IOV NICs', H.row('Available', H.badge(sd.available ? 'Yes' : 'No', sd.available ? 'g' : 'r')) + H.row('Physical Functions', Array.isArray(sd.physical_functions) ? sd.physical_functions.length : (sd.nic_count || 0)))
    + H.card('Active VFs', '<div class="stat-lg">' + (Array.isArray(vfs) ? vfs.length : 0) + '</div>')
    );
    if (Array.isArray(vfs) && vfs.length > 0) {
      h += '<table class="table-sticky"><thead><tr><th>PF</th><th>VF Index</th><th>PCI Addr</th><th>MAC</th><th>VLAN</th><th>VM</th></tr></thead><tbody>';
      vfs.forEach(v => { h += '<tr><td>' + esc(v.pf || '-') + '</td><td>' + (v.vf_index ?? '-') + '</td><td><code>' + esc(v.pci_addr || '-') + '</code></td><td>' + esc(v.mac || '-') + '</td><td>' + (v.vlan || '-') + '</td><td>' + esc(v.vm || '-') + '</td></tr>'; });
      h += '</tbody></table>';
    }
    h += H.section('SR-IOV Operations');
    h += '<div class="sg grid-2">';
    h += H.card('Enable VFs', '<div class="fr"><label>Physical NIC (PF)</label><input id="sriov-pf" placeholder="enp3s0f0" class="w-full"></div><div class="fr"><label>Num VFs</label><input id="sriov-numvf" type="number" value="4" min="1" max="64" class="w-80"></div><button class="btn btn-g" onclick="sriovEnable()" class="mt-8">Enable</button> <button class="btn btn-r" onclick="sriovDisable()" class="mt-8">Disable</button>');
    h += H.card('Attach VF to VM', '<div class="fr"><label>VM Name</label><input id="sriov-vm" placeholder="web-prod" class="w-full"></div><div class="fr"><label>PCI Address (VF)</label><input id="sriov-vf-pci" placeholder="0000:03:10.0" class="w-full"></div><button class="btn btn-g" onclick="sriovAttach()" class="mt-8">Attach</button> <button class="btn btn-r" onclick="sriovDetach()" class="mt-8">Detach</button>');
    h += '</div>';
    b.innerHTML = h;
  } catch (e) { b.innerHTML = H.section('SR-IOV') + '<p class="color-muted">Failed to load</p>'; }
}

async function sriovEnable() {
  var pf = document.getElementById('sriov-pf')?.value;
  var num = parseInt(document.getElementById('sriov-numvf')?.value) || 4;
  if (!pf) { toast(t('msg.name_required'), false); return; }
  try {
    var r = await fetchPost(EP.SRIOV_ENABLE(), { pf: pf, num_vfs: num });
    if (r.error) { toast('Enable failed: ' + (r.error.message || ''), false); return; }
    toast('SR-IOV enabled: ' + pf + ' (' + num + ' VFs)'); addEvt('SR-IOV enable ' + pf);
    renderSriov(document.getElementById('cb'));
  } catch (e) { toast(e.message, false); }
}
async function sriovDisable() {
  var pf = document.getElementById('sriov-pf')?.value;
  if (!pf) { toast(t('msg.name_required'), false); return; }
  try {
    var r = await fetchPost(EP.SRIOV_DISABLE(), { pf: pf });
    if (r.error) { toast('Disable failed: ' + (r.error.message || ''), false); return; }
    toast('SR-IOV disabled: ' + pf); addEvt('SR-IOV disable ' + pf);
    renderSriov(document.getElementById('cb'));
  } catch (e) { toast(e.message, false); }
}
async function sriovAttach() {
  var vm = document.getElementById('sriov-vm')?.value;
  var pci = document.getElementById('sriov-vf-pci')?.value;
  if (!vm || !pci) { toast(t('msg.name_required'), false); return; }
  try {
    var r = await fetchPost(EP.SRIOV_ATTACH(), { vm_name: vm, pci_addr: pci });
    if (r.error) { toast('Attach failed: ' + (r.error.message || ''), false); return; }
    toast('VF attached to ' + vm); addEvt('SR-IOV attach ' + pci + ' \u2192 ' + vm);
    renderSriov(document.getElementById('cb'));
  } catch (e) { toast(e.message, false); }
}
async function sriovDetach() {
  var vm = document.getElementById('sriov-vm')?.value;
  var pci = document.getElementById('sriov-vf-pci')?.value;
  if (!vm || !pci) { toast(t('msg.name_required'), false); return; }
  try {
    var r = await fetchPost(EP.SRIOV_DETACH(), { vm_name: vm, pci_addr: pci });
    if (r.error) { toast('Detach failed: ' + (r.error.message || ''), false); return; }
    toast('VF detached from ' + vm); addEvt('SR-IOV detach ' + pci);
    renderSriov(document.getElementById('cb'));
  } catch (e) { toast(e.message, false); }
}

window.renderSriov = renderSriov;
window.sriovEnable = sriovEnable;
window.sriovDisable = sriovDisable;
window.sriovAttach = sriovAttach;
window.sriovDetach = sriovDetach;


async function renderHost(b) {
  b.innerHTML = showSkeleton();
  try {
    const met = await _fetchMetricsText(EP.METRICS());
    let cpu = 0, mem = 0, disk = 0, temp = 0, load1 = 0;
    met.split('\n').forEach(l => {
      if (l.startsWith('purecvisor_host_cpu_percent ')) cpu = parseFloat(l.split(' ')[1]);
      if (l.startsWith('purecvisor_host_memory_percent ')) mem = parseFloat(l.split(' ')[1]);
      if (l.startsWith('purecvisor_host_disk_percent ')) disk = parseFloat(l.split(' ')[1]);
      if (l.startsWith('purecvisor_host_cpu_temp_celsius ')) temp = parseFloat(l.split(' ')[1]);
      if (l.startsWith('purecvisor_host_load1 ')) load1 = parseFloat(l.split(' ')[1]);
    });
    const d = await fetchGet(EP.DPDK_STATUS()); const dd = unwrapData(d);
    const s = await fetchGet(EP.SRIOV_STATUS()); const sd = unwrapData(s);
    var priority = disk >= 80 ? _L('디스크 여유 공간 확인', 'Review disk headroom')
      : cpu >= 70 ? _L('CPU 부하 추적', 'Track CPU pressure')
      : mem >= 70 ? _L('메모리 사용률 점검', 'Review memory usage')
      : _L('가속 기능 준비도 확인', 'Confirm accelerator readiness');
    var nextAction = (dd.available || sd.available)
      ? _L('가속 기능이 준비되어 있습니다. 워크로드 배치 전에 바인딩 정책만 확인하면 됩니다.', 'Accelerators are available. Review binding policy before scheduling workloads.')
      : _L('현재는 CPU 기반 단독 노드 운용입니다. 고성능 워크로드가 필요하면 DPDK 또는 SR-IOV 준비 상태를 먼저 확인하십시오.', 'The node is currently running CPU-only. Review DPDK or SR-IOV readiness before placing high-performance workloads.');
    var metricsNote = _L('CPU, 메모리, 디스크, 가속 카드 상태를 단일 노드 기준으로 확인합니다.', 'Review CPU, memory, disk, and accelerator readiness for the single node.');
    b.innerHTML = '<div class="ops-section-heading"><div><h3>' + _L('호스트 상태', 'Host Health') + '</h3><p>' + metricsNote + '</p></div></div>'
    + '<div class="sg grid-2 host-ops-grid">'
    + H.card('CPU', '<div class="stat-md">' + cpu.toFixed(1) + '%</div>' + renderProgressBar(cpu) + H.row('Temp', temp.toFixed(1) + '&deg;C') + H.row('Load', load1.toFixed(2)))
    + H.card('Memory', '<div class="stat-md">' + mem.toFixed(1) + '%</div>' + renderProgressBar(mem) + H.row(_L('상태', 'State'), H.badge(mem >= 80 ? _L('주의', 'Watch') : _L('안정', 'Stable'), mem >= 80 ? 'y' : 'g')))
    + H.card('Disk', '<div class="stat-md">' + disk.toFixed(1) + '%</div>' + renderProgressBar(disk) + H.row(_L('권장 조치', 'Recommended action'), disk >= 80 ? _L('정리 필요', 'Cleanup needed') : _L('여유 있음', 'Healthy margin')))
    + H.card(_L('가속 기능', 'Acceleration'), H.row('DPDK', H.badge(dd.available ? 'ON' : 'OFF', dd.available ? 'g' : 'r')) + H.row('SR-IOV', H.badge(sd.available ? 'ON' : 'OFF', sd.available ? 'g' : 'r')))
    + H.card(_L('운영 메모', 'Operations note'), H.row(_L('호스트 모드', 'Host mode'), H.badge(_L('단일 노드', 'Single node'), 'g')) + H.row(_L('수집 기준', 'Collection'), _L('실시간 메트릭', 'Live metrics')) + H.row(_L('우선순위', 'Priority'), priority))
    + H.card(_L('현재 조치', 'Current action'), '<p class="color-muted text-12" style="line-height:1.7;margin:0">' + nextAction + '</p>')
    + '</div>';
  } catch (e) { if(_DEBUG) console.warn('renderHost:', e.message); }
}
window.renderHost = renderHost;


function renderHeatmap(b) {
  b.innerHTML = showSkeleton();

  fetchGet(EP.VM_LIST()).then(function(r) {
    var vms = unwrapList(r);
    if (!vms || vms.length === 0) {
      b.innerHTML = H.section(_L('리소스 히트맵', 'Resource Heatmap'))
        + '<p class="color-muted text-center" style="padding:24px">' + _L('실행 중인 VM이 없습니다', 'No running VMs') + '</p>';
      return;
    }
    var h = H.section(_L('리소스 히트맵', 'Resource Heatmap'));
    h += '<div style="overflow-x:auto">';
    h += '<table style="font-size:11px;border-collapse:separate;border-spacing:2px"><thead><tr><th>' + _L('VM', 'VM') + '</th>';
    for (var i = 0; i < 12; i++) h += '<th class="w-30 text-center text-9">' + (i * 5) + 'm</th>';
    h += '</tr></thead><tbody>';
    vms.forEach(function(vm) {
      h += '<tr><td class="nowrap"><b>' + esc(vm.name || '?') + '</b></td>';
      for (var i = 0; i < 12; i++) {
        var cpu = (vm.live_cpu_pct || vm.cpu_percent || 0) + (Math.random() * 20 - 10);
        cpu = Math.max(0, Math.min(100, cpu));
        var r = cpu > 80 ? 255 : Math.round(cpu * 2.5);
        var g = cpu < 50 ? Math.round(200 - cpu * 2) : Math.round(200 - cpu * 2);
        g = Math.max(0, g);
        var color = 'rgba(' + r + ',' + g + ',50,0.8)';
        h += '<td style="width:30px;height:20px;background:' + color + ';border-radius:2px" title="' + cpu.toFixed(0) + '%"></td>';
      }
      h += '</tr>';
    });
    h += '</tbody></table></div>';
    h += '<div class="flex gap-8 mt-8 text-xs">';
    h += '<span style="display:inline-block;width:12px;height:12px;background:rgba(0,200,50,0.8);border-radius:2px"></span> ' + _L('낮음', 'Low');
    h += '<span style="display:inline-block;width:12px;height:12px;background:rgba(200,200,0,0.8);border-radius:2px;margin-left:12px"></span> ' + _L('중간', 'Medium');
    h += '<span style="display:inline-block;width:12px;height:12px;background:rgba(255,50,50,0.8);border-radius:2px;margin-left:12px"></span> ' + _L('높음', 'High');
    h += '</div>';
    b.innerHTML = h;
  }).catch(function(e) { b.innerHTML = H.section(_L('리소스 히트맵', 'Resource Heatmap')) + '<p class="color-muted">' + _L('로드 실패', 'Failed to load') + ': ' + esc(e.message || '') + '</p>'; });
}
window.renderHeatmap = renderHeatmap;
window.loadDeepHealth = loadDeepHealth;


async function renderAlertSilences(b) {
  b.innerHTML = showSkeleton();
  try {
    var r = await fetchGet(EP.ALERT_SILENCE_LIST());
    var list = unwrapList(r);
    var h = H.section(_L('알림 음소거', 'Alert Silences'));
    h += '<button class="btn mb-8" onclick="showSilenceCreate()" aria-label="' + _L('음소거 추가', 'Add silence') + '">+ ' + _L('새 음소거', 'New Silence') + '</button>';
    if (list.length === 0) {
      h += '<div class="empty-state" style="padding:30px;text-align:center"><div style="font-size:36px;opacity:.5">&#128264;</div>';
      h += '<div class="color-muted">' + _L('활성 음소거 없음', 'No active silences') + '</div></div>';
    } else {
      h += '<table class="data-table text-11"><thead><tr>';
      h += '<th>' + _L('메트릭', 'Metric') + '</th><th>' + _L('남은 시간', 'Remaining') + '</th><th>' + _L('사유', 'Reason') + '</th></tr></thead><tbody>';
      list.forEach(function(s) {
        var mins = Math.ceil((s.remaining_sec || 0) / 60);
        h += '<tr><td><b>' + esc(s.metric) + '</b></td>';
        h += '<td>' + mins + _L('분', 'min') + '</td>';
        h += '<td class="color-muted">' + esc(s.reason || '') + '</td></tr>';
      });
      h += '</tbody></table>';
    }
    b.innerHTML = h;
  } catch(e) { b.innerHTML = '<p class="color-muted">' + _L('로드 실패', 'Failed') + '</p>'; }
}
async function showSilenceCreate() {
  var html = '<div class="form-group"><label>' + _L('메트릭', 'Metric') + '</label>';
  html += '<select id="sil-metric" class="input-field"><option>cpu</option><option>mem</option><option>disk</option></select></div>';
  html += '<div class="form-group"><label>' + _L('기간 (분)', 'Duration (min)') + '</label>';
  html += '<input id="sil-dur" type="number" value="60" min="1" max="1440" class="input-field"></div>';
  html += '<div class="form-group"><label>' + _L('사유', 'Reason') + '</label>';
  html += '<input id="sil-reason" class="input-field" placeholder="' + _L('유지보수 예정', 'Planned maintenance') + '"></div>';
  showModal(_L('알림 음소거', 'Silence Alert'), html, async function() {
    var metric = document.getElementById('sil-metric').value;
    var dur = parseInt(document.getElementById('sil-dur').value) || 60;
    var reason = document.getElementById('sil-reason').value.trim();
    try {
      await fetchPost(EP.ALERT_SILENCE(), { metric: metric, duration_min: dur, reason: reason });
      toast(_L('음소거 적용', 'Silence applied'), 's');
      renderAlertSilences(document.getElementById('cb'));
    } catch(e) { toast(_L('실패', 'Failed'), 'e'); }
  });
}


async function renderAlertRouting(b) {
  b.innerHTML = showSkeleton();
  var h = H.section(_L('알림 라우팅 설정', 'Alert Routing Configuration'));
  h += '<div class="sg p-12">';
  h += '<div class="form-group"><label>WARN ' + _L('Webhook URL', 'Webhook URL') + '</label>';
  h += '<input id="route-warn-url" class="input-field" placeholder="https://hooks.slack.com/..." aria-label="Warning webhook URL"></div>';
  h += '<div class="form-group"><label>CRIT ' + _L('Webhook URL (에스컬레이션)', 'Webhook URL (escalation)') + '</label>';
  h += '<input id="route-crit-url" class="input-field" placeholder="https://pagerduty.com/..." aria-label="Critical webhook URL"></div>';
  h += '<div class="form-group"><label>Webhook Secret (HMAC)</label>';
  h += '<input id="route-secret" type="password" class="input-field" placeholder="' + _L('서명 키', 'Signing secret') + '" aria-label="Webhook HMAC secret"></div>';
  h += '<button class="btn mt-8" onclick="saveAlertRouting()" aria-label="' + _L('라우팅 저장', 'Save routing') + '">' + _L('저장', 'Save') + '</button>';
  h += '</div>';
  b.innerHTML = h;
}
async function saveAlertRouting() {
  var cfg = {};
  var warnUrl = document.getElementById('route-warn-url').value.trim();
  var critUrl = document.getElementById('route-crit-url').value.trim();
  var secret = document.getElementById('route-secret').value.trim();
  if (warnUrl) cfg.webhook_url = warnUrl;
  if (critUrl) cfg.webhook_crit_url = critUrl;
  if (secret) cfg.webhook_secret = secret;
  try {
    await fetchPost(EP.ALERTS_CONFIG(), cfg);
    toast(_L('라우팅 설정 저장 완료', 'Alert routing saved'), 's');
  } catch(e) { toast(_L('실패', 'Failed'), 'e'); }
}


async function renderPoolInfo(b) {
  b.innerHTML = showSkeleton();
  try {
    var r = await fetchGet(EP.POOL_CONNINFO());
    var d = unwrapData(r);
    var h = H.section(_L('커넥션 풀 상태', 'Connection Pool Status'));
    h += '<div class="grid-3" style="gap:12px">';
    h += H.statCard(_L('유휴', 'Idle'), d.idle || 0, '🟢');
    h += H.statCard(_L('활성', 'Active'), (d.total || 0) - (d.idle || 0), '🔴');
    h += H.statCard(_L('최대', 'Max'), d.max || 0, '⚪');
    h += '</div>';
    if (d.wait_avg_sec !== undefined) {
      h += '<div class="mt-8 color-muted text-xs">' + _L('평균 대기', 'Avg wait') + ': ' + (d.wait_avg_sec * 1000).toFixed(1) + 'ms</div>';
    }
    b.innerHTML = h;
  } catch(e) { b.innerHTML = '<p class="color-muted">' + _L('로드 실패', 'Failed') + '</p>'; }
}

window.renderAlertSilences = renderAlertSilences;
window.showSilenceCreate = showSilenceCreate;
window.renderAlertRouting = renderAlertRouting;
window.saveAlertRouting = saveAlertRouting;
window.renderPoolInfo = renderPoolInfo;


async function loadHealingPending() {

  try {
    var r;
    try {
      r = await fetchPost(EP.RPC(), { jsonrpc:'2.0', method:'healing.history', params:{}, id:'hp1' });
    } catch (e) {
      var elx = document.getElementById('healing-pending-list');
      if (elx) elx.innerHTML = '<div class="stat-label">' + _L('대기 중인 액션 없음', 'No pending actions') + '</div>';
      return;
    }
    var d = unwrapData(r);
    var raw = Array.isArray(d) ? d : (unwrapList ? unwrapList(d) : []);
    var actions = raw.filter(function(a) { return a && (a.status === 'pending' || a.state === 'pending'); });
    var el = document.getElementById('healing-pending-list');
    if (!el) return;
    if (actions.length === 0) {
      el.innerHTML = '<div class="stat-label">' + _L('대기 중인 액션 없음', 'No pending actions') + '</div>';
      return;
    }
    var h = '';
    actions.forEach(function(a, i) {
      h += '<div class="hc mb-8 flex items-center gap-10" style="padding:8px 12px">';
      h += '<span class="color-yellow">&#9888;</span> ';
      h += '<span class="flex-1"><strong>' + esc(a.action || 'unknown') + '</strong>';
      if (a.target) h += ' — ' + esc(a.target);
      if (a.reason) h += ' <span class="stat-label">(' + esc(a.reason) + ')</span>';
      h += '</span>';
      h += '<button class="btn btn-g btn-sm" onclick="healingApprove(' + i + ')">' + _L('승인', 'Approve') + '</button>';
      h += '<button class="btn btn-r btn-sm" onclick="healingReject(' + i + ')">' + _L('거절', 'Reject') + '</button>';
      h += '</div>';
    });
    el.innerHTML = h;
  } catch (e) {
    var el2 = document.getElementById('healing-pending-list');
    if (el2) el2.innerHTML = '<div class="stat-label">' + esc(e.message) + '</div>';
  }
}

async function healingApprove(idx) {
  try {
    var r = await fetchPost(EP.RPC(), { jsonrpc:'2.0', method:'ai.healing.approve', params:{ index: idx }, id:'ha1' });
    if (r.error) { toast(r.error.message, false); return; }
    toast(_L('승인됨', 'Approved'));
    loadHealingPending();
  } catch (e) { toast(e.message, false); }
}

async function healingReject(idx) {
  try {
    var r = await fetchPost(EP.RPC(), { jsonrpc:'2.0', method:'ai.healing.reject', params:{ index: idx }, id:'hr1' });
    if (r.error) { toast(r.error.message, false); return; }
    toast(_L('거절됨', 'Rejected'));
    loadHealingPending();
  } catch (e) { toast(e.message, false); }
}

window.loadHealingPending = loadHealingPending;
window.healingApprove = healingApprove;
window.healingReject = healingReject;


PCV.monitor = {
  destroyAllCharts: destroyAllCharts,
  createLineChart: createLineChart,
  drawGraphFallback: drawGraphFallback,
  getChartColor: getChartColor,
  fetchAllMetrics: fetchAllMetrics,
  fmtBytes: fmtBytes,
  fmtRate: fmtRate,
  fmtUptime: fmtUptime,
  drawLine: drawLine,
  gauge: gauge,
  renderMonitoring: renderMonitoring,
  loadDeepHealth: loadDeepHealth,
  renderMonOverview: renderMonOverview,
  renderMonCluster: renderMonCluster,
  renderMonHosts: renderMonHosts,
  renderMonVms: renderMonVms,
  renderMonStorage: renderMonStorage,
  renderAlerts: renderAlerts,
  saveAlertConfig: saveAlertConfig,
  renderAudit: renderAudit,
  renderGpu: renderGpu,
  renderDpdk: renderDpdk,
  dpdkBind: dpdkBind,
  dpdkUnbind: dpdkUnbind,
  renderSriov: renderSriov,
  sriovEnable: sriovEnable,
  sriovDisable: sriovDisable,
  sriovAttach: sriovAttach,
  sriovDetach: sriovDetach,
  renderHost: renderHost,
  renderHeatmap: renderHeatmap,
  renderAlertSilences: renderAlertSilences,
  showSilenceCreate: showSilenceCreate,
  renderAlertRouting: renderAlertRouting,
  saveAlertRouting: saveAlertRouting,
  renderPoolInfo: renderPoolInfo,
  loadHealingPending: loadHealingPending,
  healingApprove: healingApprove,
  healingReject: healingReject
};

})(window.PCV);






window.PCV = window.PCV || {};
(function(PCV) {
  'use strict';

  var API = PCV.api || {};
  var currentEventId = '';
  var lastEvents = [];






  function _L(ko, en) {
    return (window.PCV_LANG === 'en') ? (en || ko) : ko;
  }

  async function rpc(method, params) {
    var body = {
      jsonrpc: '2.0',
      method: method,
      params: params || {},
      id: 'sec-' + Date.now()
    };
    var json = API.fetchPost
      ? await API.fetchPost(EP.RPC(), body)
      : await fetchPost(EP.RPC(), body);
    if (json && json.error) {
      throw new Error(json.error.message || 'RPC failed');
    }
    return API.unwrapData ? API.unwrapData(json) : (json.result || json.data || json);
  }

  function canRole(role) {
    return typeof pcvRoleAllows === 'function' ? pcvRoleAllows(role) : false;
  }

  function notify(msg, ok) {
    if (typeof toast === 'function') toast(msg, ok !== false);
  }

  async function askConfirm(msg) {
    if (typeof customConfirm === 'function') {
      return customConfirm(msg);
    }
    return window.confirm(msg);
  }

  function asArray(value, key) {
    if (Array.isArray(value)) return value;
    if (value && Array.isArray(value[key])) return value[key];
    return [];
  }

  function formatTs(ts) {
    var n = Number(ts || 0);
    if (!n) return '-';
    return new Date(n * 1000).toLocaleString();
  }

  function upper(v) {
    return String(v || '').toUpperCase();
  }

  function badgeSeverity(sev) {
    if (sev === 'crit') return H.badge('CRIT', 'r');
    if (sev === 'warn') return H.badge('WARN', 'y');
    return H.badge('INFO', 'g');
  }

  function badgeStatus(status) {
    if (status === 'resolved' || status === 'suppressed') return H.badge(upper(status), 'g');
    if (status === 'action_pending') return H.badge('PENDING', 'y');
    if (status === 'open') return H.badge('OPEN', 'y');
    return H.badge(status || '-', 'y');
  }

  function statusCard(title, value, hint, type) {
    var cls = type === 'r' ? 'color-red' : type === 'y' ? 'color-yellow' : 'color-green';
    return H.card(esc(title),
      '<div class="stat-md ' + cls + '">' + esc(value) + '</div>'
      + '<div class="stat-label mt-4">' + esc(hint || '') + '</div>',
      'text-center');
  }

  function renderStatusBar(status, pendingCount) {




    var guard = status && status.enabled ? 'enabled' : 'disabled';
    var baseline = (status && status.baseline_status) || 'unknown';
    var degraded = status && status.degraded;
    var guardHint = guard === 'enabled'
      ? _L('탐지 경로 활성', 'Detection path active')
      : _L('admin opt-in 필요', 'admin opt-in required');
    var baselineHint = baseline === 'trusted'
      ? _L('관리자 refresh 완료', 'admin refresh completed')
      : _L('자동 trusted 전환 없음', 'never auto-trusted');
    var risk = String((status && status.open_risk) || 0);
    var pending = String((status && status.pending_actions) || pendingCount || 0);

    return H.grid(4,
      statusCard('Guard', upper(guard), guardHint, guard === 'enabled' ? 'g' : 'r')
      + statusCard('Baseline', upper(baseline), baselineHint, baseline === 'trusted' ? 'g' : 'y')
      + statusCard(_L('Open Risk', 'Open Risk'), risk, degraded ? _L('store degraded', 'store degraded') : _L('CRIT/WARN 가중치', 'CRIT/WARN weighted'), Number(risk) > 0 ? 'r' : 'g')
      + statusCard(_L('Pending Actions', 'Pending Actions'), pending, _L('수동 승인 전용', 'manual approval only'), Number(pending) > 0 ? 'y' : 'g')
    ) + '<div class="hc mb-12"><div class="flex gap-8 flex-wrap items-center">'
      + '<button class="btn" type="button" data-sec-refresh="1">&#10227; ' + _L('새로고침', 'Refresh') + '</button>'
      + (canRole('admin')
        ? '<label class="text-xs" style="display:flex;align-items:center;gap:6px"><input type="checkbox" data-sec-enabled="1" ' + (guard === 'enabled' ? 'checked' : '') + '> ' + _L('Security Guard 활성', 'Enable Security Guard') + '</label>'
        : '<span class="stat-label">' + _L('Guard 변경은 admin 권한이 필요합니다.', 'Changing Guard state requires admin.') + '</span>')
      + '</div></div>';
  }

  function uniqueValues(items, key) {
    var seen = {};
    var out = [];
    items.forEach(function(item) {
      var v = String((item && item[key]) || '');
      if (v && !seen[v]) {
        seen[v] = true;
        out.push(v);
      }
    });
    return out.sort();
  }

  function optionHtml(value, label) {
    return '<option value="' + esc(value) + '">' + esc(label || value) + '</option>';
  }

  function renderFilters(events) {
    var sources = uniqueValues(events, 'source');
    var statuses = uniqueValues(events, 'status');
    return '<div class="hc mb-12"><div class="flex gap-8 flex-wrap items-end">'
      + '<label class="text-xs">' + _L('심각도', 'Severity')
      + '<select id="sec-filter-sev" class="input-pcv">'
      + optionHtml('', _L('전체', 'All'))
      + optionHtml('crit', 'CRIT') + optionHtml('warn', 'WARN') + optionHtml('info', 'INFO')
      + '</select></label>'
      + '<label class="text-xs">' + _L('소스', 'Source')
      + '<select id="sec-filter-source" class="input-pcv">' + optionHtml('', _L('전체', 'All'))
      + sources.map(function(v) { return optionHtml(v, v); }).join('') + '</select></label>'
      + '<label class="text-xs">' + _L('상태', 'Status')
      + '<select id="sec-filter-status" class="input-pcv">' + optionHtml('', _L('전체', 'All'))
      + statuses.map(function(v) { return optionHtml(v, v); }).join('') + '</select></label>'
      + '<label class="text-xs" style="flex:1;min-width:220px">' + _L('검색', 'Search')
      + '<input id="sec-filter-q" class="input-pcv" type="search" placeholder="target, summary, event_id" style="width:100%"></label>'
      + '</div></div>';
  }

  function renderEventTable(events) {
    if (!events.length) {
      return '<div class="empty-state p-20 text-center">'
        + '<div class="empty-title">' + _L('보안 이벤트 없음', 'No security events') + '</div>'
        + '<div class="empty-desc">' + _L('WARN/CRIT 이벤트는 audit target과 event_id를 공유합니다.', 'WARN/CRIT events share audit target with event_id.') + '</div>'
        + '</div>';
    }

    var rows = events.map(function(ev) {
      var text = [
        ev.event_id, ev.source, ev.status, ev.target, ev.summary, ev.recommended_action
      ].join(' ').toLowerCase();
      return '<tr class="sec-event-row" data-sec-event-id="' + esc(ev.event_id || '') + '"'
        + ' data-sec-severity="' + esc(ev.severity || '') + '"'
        + ' data-sec-source="' + esc(ev.source || '') + '"'
        + ' data-sec-status="' + esc(ev.status || '') + '"'
        + ' data-sec-text="' + esc(text) + '" tabindex="0" style="cursor:pointer">'
        + '<td class="color-muted nowrap">' + esc(formatTs(ev.timestamp)) + '</td>'
        + '<td>' + badgeSeverity(ev.severity || 'info') + '</td>'
        + '<td>' + esc(ev.source || '-') + '</td>'
        + '<td><code class="text-xs">' + esc(ev.target || '-') + '</code></td>'
        + '<td>' + esc(ev.summary || '-') + '</td>'
        + '<td>' + badgeStatus(ev.status || '-') + '</td>'
        + '<td>' + esc(ev.recommended_action || '-') + '</td></tr>';
    }).join('');

    return '<div style="overflow:auto;max-height:560px">'
      + '<table class="data-table text-11"><thead><tr>'
      + '<th>' + _L('시간', 'Time') + '</th><th>' + _L('심각도', 'Severity') + '</th>'
      + '<th>' + _L('소스', 'Source') + '</th><th>' + _L('대상', 'Target') + '</th>'
      + '<th>' + _L('요약', 'Summary') + '</th><th>' + _L('상태', 'Status') + '</th>'
      + '<th>' + _L('권고', 'Action') + '</th></tr></thead><tbody>'
      + rows
      + '<tr id="sec-filter-empty" style="display:none"><td colspan="7" class="color-muted text-center p-12">'
      + _L('필터 조건과 일치하는 이벤트가 없습니다.', 'No events match the current filters.')
      + '</td></tr></tbody></table></div>';
  }

  function renderPendingActions(actions) {
    if (!actions.length) {
      return H.card(_L('Pending Approval Queue', 'Pending Approval Queue'),
        '<div class="empty-state p-20 text-center"><div class="empty-title">'
        + _L('승인 대기 없음', 'No pending approvals') + '</div></div>');
    }

    var body = '<div style="overflow:auto;max-height:260px"><table class="data-table text-11"><thead><tr>'
      + '<th>event_id</th><th>' + _L('대응', 'Action') + '</th><th>' + _L('대상', 'Target') + '</th>'
      + '<th>TTL</th><th>' + _L('처리', 'Controls') + '</th></tr></thead><tbody>';
    actions.forEach(function(action) {
      var name = action.action || '';
      var executable = name === 'block_ip' || name === 'revoke_api_key';




      var controls = executable
        ? (canRole('admin')
          ? '<button class="btn btn-r" type="button" data-sec-approve="' + esc(action.event_id || '') + '">' + _L('승인', 'Approve') + '</button>'
          : '<span class="stat-label">' + _L('admin 승인 필요', 'admin approval required') + '</span>')
        : '<span class="stat-label">' + _L('수동 runbook', 'manual runbook') + '</span>';
      if (canRole('operator')) {
        controls += ' <button class="btn" type="button" data-sec-dismiss="' + esc(action.event_id || '') + '">' + _L('거부', 'Dismiss') + '</button>';
      }
      body += '<tr data-sec-action-event-id="' + esc(action.event_id || '') + '" style="cursor:pointer">'
        + '<td><code class="text-xs">' + esc(action.event_id || '-') + '</code></td>'
        + '<td>' + H.badge(name || '-', executable ? 'r' : 'y') + '</td>'
        + '<td><code class="text-xs">' + esc(action.target || '-') + '</code></td>'
        + '<td>' + esc(String(action.ttl_sec || '-')) + 's</td>'
        + '<td class="nowrap">' + controls + '</td></tr>';
    });
    body += '</tbody></table></div>';
    return H.card(_L('Pending Approval Queue', 'Pending Approval Queue'), body);
  }

  function renderVerificationCard() {
    return H.card(_L('Verification Gates', 'Verification Gates'),
      H.row('Viewer', H.badge('READ', 'g'))
      + H.row('Operator', H.badge('DISMISS', 'y'))
      + H.row('Admin', H.badge('APPROVE', 'r'))
      + H.row('ADR-0018', H.badge('ASYNC', 'g'))
      + H.row('Audit', '<code>target = event_id</code>'));
  }

  function renderShell(cfg, events, actions) {
    var h = H.section('&#128737; ' + _L('보안 이벤트', 'Security Events'));
    h += renderStatusBar(cfg || {}, actions.length);
    h += renderFilters(events);
    h += '<div class="sg grid-2 mb-12">';
    h += H.card(_L('Event Queue', 'Event Queue'), renderEventTable(events));
    h += '<div id="security-detail" class="hc"><h4>' + _L('Selected Event', 'Selected Event') + '</h4>'
      + '<p class="color-muted text-12">' + _L('이벤트를 선택하세요.', 'Select an event.') + '</p></div>';
    h += '</div>';
    h += '<div class="sg grid-2">' + renderPendingActions(actions) + renderVerificationCard() + '</div>';
    return h;
  }

  function applyFilters(root) {




    root = root || document;
    var sev = (document.getElementById('sec-filter-sev') || {}).value || '';
    var source = (document.getElementById('sec-filter-source') || {}).value || '';
    var status = (document.getElementById('sec-filter-status') || {}).value || '';
    var q = ((document.getElementById('sec-filter-q') || {}).value || '').trim().toLowerCase();
    var visible = 0;
    root.querySelectorAll('.sec-event-row').forEach(function(row) {
      var ok = (!sev || row.getAttribute('data-sec-severity') === sev)
        && (!source || row.getAttribute('data-sec-source') === source)
        && (!status || row.getAttribute('data-sec-status') === status)
        && (!q || (row.getAttribute('data-sec-text') || '').indexOf(q) >= 0);
      row.style.display = ok ? '' : 'none';
      if (ok) visible++;
    });
    var empty = document.getElementById('sec-filter-empty');
    if (empty) empty.style.display = visible ? 'none' : '';
  }

  function bindSecurityHandlers(root) {
    root.querySelectorAll('[data-sec-refresh]').forEach(function(btn) {
      btn.addEventListener('click', refresh);
    });
    root.querySelectorAll('[data-sec-event-id]').forEach(function(row) {
      row.addEventListener('click', function() {
        selectEvent(row.getAttribute('data-sec-event-id') || '');
      });
      row.addEventListener('keydown', function(e) {
        if (e.key === 'Enter' || e.key === ' ') {
          e.preventDefault();
          selectEvent(row.getAttribute('data-sec-event-id') || '');
        }
      });
    });
    root.querySelectorAll('[data-sec-action-event-id]').forEach(function(row) {
      row.addEventListener('click', function(e) {
        if (e.target && e.target.closest && e.target.closest('button')) return;
        selectEvent(row.getAttribute('data-sec-action-event-id') || '');
      });
    });
    root.querySelectorAll('[data-sec-approve]').forEach(function(btn) {
      btn.addEventListener('click', function() {
        approveAction(btn.getAttribute('data-sec-approve') || '');
      });
    });
    root.querySelectorAll('[data-sec-dismiss]').forEach(function(btn) {
      btn.addEventListener('click', function() {
        dismissAction(btn.getAttribute('data-sec-dismiss') || '');
      });
    });
    root.querySelectorAll('#sec-filter-sev,#sec-filter-source,#sec-filter-status').forEach(function(el) {
      el.addEventListener('change', function() { applyFilters(root); });
    });
    var q = root.querySelector('#sec-filter-q');
    if (q) q.addEventListener('input', function() { applyFilters(root); });
    var enabled = root.querySelector('[data-sec-enabled]');
    if (enabled) {
      enabled.addEventListener('change', function() {
        setGuardEnabled(Boolean(enabled.checked));
      });
    }
  }

  async function renderSecurityEvents(b) {




    b.innerHTML = showSkeleton();
    try {
      var loaded = await Promise.all([
        rpc('security.config.get', {}),
        rpc('security.event.list', { limit: 100 }),
        rpc('security.action.pending', {})
      ]);
      var cfg = loaded[0] || {};
      var events = asArray(loaded[1], 'events');
      var actions = asArray(loaded[2], 'actions');
      lastEvents = events;
      b.innerHTML = renderShell(cfg, events, actions);
      bindSecurityHandlers(b);
      applyFilters(b);
      if (currentEventId && events.some(function(ev) { return ev.event_id === currentEventId; })) {
        selectEvent(currentEventId);
      }
    } catch (e) {
      b.innerHTML = '<div class="hc"><h4 class="color-red">'
        + _L('보안 이벤트 로드 실패', 'Failed to load security events')
        + '</h4><p class="color-muted">' + esc(e.message || '') + '</p>'
        + '<button class="btn" type="button" data-sec-refresh="1">Retry</button></div>';
      bindSecurityHandlers(b);
    }
  }

  function renderEventDetail(ev) {
    var action = ev.recommended_action || '';
    var executable = action === 'block_ip' || action === 'revoke_api_key';
    var controls = '';
    if (executable) {
      controls += canRole('admin')
        ? '<button class="btn btn-r" type="button" data-sec-approve="' + esc(ev.event_id || '') + '">' + _L('승인', 'Approve') + '</button>'
        : '<span class="stat-label">' + _L('admin 승인 필요', 'admin approval required') + '</span>';
    } else if (action === 'manual_runbook') {
      controls += '<span class="stat-label">' + _L('수동 runbook 후보입니다. 자동 실행하지 않습니다.', 'Manual runbook candidate. It will not execute automatically.') + '</span>';
    }
    if (canRole('operator')) {
      controls += ' <button class="btn" type="button" data-sec-dismiss="' + esc(ev.event_id || '') + '">' + _L('거부', 'Dismiss') + '</button>';
    }

    return '<h4>' + _L('Selected Event', 'Selected Event') + ' ' + badgeSeverity(ev.severity || 'info') + '</h4>'
      + '<div class="mb-8"><b>' + esc(ev.summary || ev.event_id || '-') + '</b></div>'
      + H.row('event_id', '<code>' + esc(ev.event_id || '') + '</code>')
      + H.row(_L('소스', 'Source'), esc(ev.source || '-'))
      + H.row(_L('대상 유형', 'Target Kind'), esc(ev.target_kind || '-'))
      + H.row(_L('대상', 'Target'), '<code class="text-xs">' + esc(ev.target || '-') + '</code>')
      + H.row(_L('상태', 'Status'), badgeStatus(ev.status || '-'))
      + H.row(_L('신뢰도', 'Confidence'), esc(String(ev.confidence || 0)) + '%')
      + H.row(_L('권고 대응', 'Recommended Response'), H.badge(action || '-', executable ? 'r' : 'y'))
      + H.row(_L('감사 상관키', 'Audit Correlation'), '<code>security.event target=' + esc(ev.event_id || '') + '</code>')
      + H.row(_L('발생 횟수', 'Occurrences'), esc(String(ev.occurrence_count || 1)))
      + '<div class="mt-10 mb-8"><b class="text-12">' + _L('Evidence', 'Evidence') + '</b></div>'
      + '<pre class="stat-label" style="white-space:pre-wrap;overflow:auto;max-height:260px;margin:0">'
      + esc(ev.evidence_json || '{}') + '</pre>'
      + '<div class="flex gap-6 mt-10 flex-wrap">' + controls + '</div>';
  }

  async function selectEvent(eventId) {
    if (!eventId) return;
    currentEventId = eventId;
    var detail = document.getElementById('security-detail');
    if (!detail) return;
    detail.innerHTML = showSkeleton();
    try {
      var ev = await rpc('security.event.get', { event_id: eventId });
      detail.innerHTML = renderEventDetail(ev || {});
      bindSecurityHandlers(detail);
      document.querySelectorAll('.sec-event-row').forEach(function(row) {
        row.classList.toggle('selected', row.getAttribute('data-sec-event-id') === eventId);
      });
    } catch (e) {
      detail.innerHTML = '<p class="color-red">' + esc(e.message || '') + '</p>';
    }
  }

  function refresh() {
    var b = document.getElementById('cb');
    if (b) renderSecurityEvents(b);
  }

  async function setGuardEnabled(enabled) {
    if (!canRole('admin')) {
      notify(_L('admin 권한이 필요합니다.', 'admin role required'), false);
      refresh();
      return;
    }
    try {
      await rpc('security.config.set', { enabled: enabled });
      notify(enabled ? _L('Security Guard 활성화', 'Security Guard enabled') : _L('Security Guard 비활성화', 'Security Guard disabled'), true);
      refresh();
    } catch (e) {
      notify(e.message || 'failed', false);
      refresh();
    }
  }

  async function approveAction(eventId) {
    if (!eventId) return;
    if (!canRole('admin')) {
      notify(_L('admin 권한이 필요합니다.', 'admin role required'), false);
      return;
    }




    if (!await askConfirm(_L('이 대응을 승인하시겠습니까?', 'Approve this response?') + '\n' + eventId)) return;
    try {
      await rpc('security.action.approve', { event_id: eventId });
      notify(_L('승인 요청됨', 'Approval accepted'), true);
      refresh();
    } catch (e) {
      notify(e.message || 'failed', false);
    }
  }

  async function dismissAction(eventId) {
    if (!eventId) return;
    if (!canRole('operator')) {
      notify(_L('operator 권한이 필요합니다.', 'operator role required'), false);
      return;
    }
    try {
      await rpc('security.action.dismiss', {
        event_id: eventId,
        reason: 'dismissed from UI'
      });
      notify(_L('거부 완료', 'Dismissed'), true);
      refresh();
    } catch (e) {
      notify(e.message || 'failed', false);
    }
  }

  PCV.security = {
    render: renderSecurityEvents,
    refresh: refresh,
    selectEvent: selectEvent,
    approveAction: approveAction,
    dismissAction: dismissAction,
    setGuardEnabled: setGuardEnabled,
    _lastEvents: function() { return lastEvents.slice(); }
  };
})(window.PCV);






































































window.PCV = window.PCV || {};
(function(PCV) {







var _lastVmListHash = '';
var vmViewMode = localStorage.getItem('pcv-vm-view') || 'list';

function toggleVmView() {
  vmViewMode = vmViewMode === 'list' ? 'card' : 'list';
  localStorage.setItem('pcv-vm-view', vmViewMode);
  render();
}
function setSort(k) {
  if (sortField === k) sortDirection *= -1;
  else { sortField = k; sortDirection = 1; }
  render();
}

function getFiltered() {
  const f = (document.getElementById('vf') || {}).value || '';
  let l = [...vmList];
  if (f) l = l.filter(v => typeof window.fuzzyMatch === 'function' ? window.fuzzyMatch(v.name, f) : v.name.toLowerCase().includes(f.toLowerCase()));
  l.sort((a, b) => {
    let va, vb;
    if (sortField === 'cpu') { va = a.live_cpu_pct || 0; vb = b.live_cpu_pct || 0; }
    else if (sortField === 'mem') { va = a.mem_percent || 0; vb = b.mem_percent || 0; }
    else if (sortField === 'state') { va = a.state; vb = b.state; }
    else { va = a.name; vb = b.name; }
    return va < vb ? -sortDirection : va > vb ? sortDirection : 0;
  });

  const favs = getFavorites();
  l.sort((a, b) => {
    const af = favs.includes(a.name) ? 0 : 1;
    const bf = favs.includes(b.name) ? 0 : 1;
    return af - bf;
  });
  return l;
}

var _renderInFlight = false;
function render(skipContent) {
  if (_renderInFlight) return;
  _renderInFlight = true;
  try { _renderCore(skipContent); } finally { _renderInFlight = false; }
}
function _renderCore(skipContent) {
  var newHash = vmList.map(function(v){return v.name+v.state+(v.live_cpu_pct||0);}).join('|');
  if (skipContent && newHash === _lastVmListHash) return;
  _lastVmListHash = newHash;
  const l = getFiltered();
  const favs = getFavorites();

  vmList.forEach(function(v) {
    if (!_vmCpuHist[v.name]) _vmCpuHist[v.name] = [];
    _vmCpuHist[v.name].push(v.live_cpu_pct || 0);
    if (_vmCpuHist[v.name].length > 30) _vmCpuHist[v.name].shift();
  });

  if (l.length === 0 && typeof emptyStatePro === 'function') {
    document.getElementById('vl').innerHTML = emptyStatePro({
      icon: '&#128187;',
      title: _L('VM이 없습니다', 'No virtual machines'),
      desc: _L('첫 VM을 만들어 시작하세요. 몇 초 안에 부팅 가능합니다.', 'Create your first VM. Boots in seconds.'),
      ctaLabel: _L('+ VM 만들기', '+ Create VM'),
      ctaAction: 'showCreate()'
    });
    return;
  }
  let h = '';
  h += '<div class="flex gap-4 mb-8 justify-end">';
  h += '<button class="btn ' + (vmViewMode === 'list' ? 'btn-g' : '') + ' btn-xs" onclick="toggleVmView()">&#9776; ' + _L('목록', 'List') + '</button>';
  h += '<button class="btn ' + (vmViewMode === 'card' ? 'btn-g' : '') + ' btn-xs" onclick="toggleVmView()">&#9638; ' + _L('카드', 'Card') + '</button>';
  h += '<button class="btn" onclick="showVmCompare()" class="btn-xs">' + _L('비교', 'Compare') + '</button>';
  h += '<button class="btn" onclick="showBulkActions()" class="btn-xs" data-role="OPERATOR,ADMIN">' + _L('일괄 작업', 'Bulk') + '</button>';
  h += '</div>';
  if (vmViewMode === 'card') {
    h += '<div class="sg grid-3">';
    l.forEach(function(v, ri) {
      var on = v.state === 'running';
      var cp = v.live_cpu_pct || 0;
      var mp = v.mem_percent || 0;
      h += '<div class="hc" draggable="true" ondragstart="event.dataTransfer.setData(\'text/plain\',\'' + esc(v.name) + '\')" style="cursor:grab;border-left:3px solid ' + (on ? 'var(--green)' : 'var(--red)') + '" onclick="selectedVmIndex=' + vmList.indexOf(v) + ';currentTab=\'summary\';switchSbTab(\'vms\');render()">';
      h += '<div class="flex items-center gap-6 mb-6"><span style="font-size:8px;color:' + (on ? 'var(--green)' : 'var(--red)') + '">&#9679;</span><b>' + esc(v.name) + '</b></div>';
      h += '<div class="flex gap-8 text-11">';
      h += '<div class="flex-1"><div class="color-muted">CPU</div>' + renderProgressBar(cp) + '</div>';
      h += '<div class="flex-1"><div class="color-muted">MEM</div>' + renderProgressBar(mp) + '</div>';
      h += '</div>';
      h += '<div class="flex gap-8 mt-6 text-xs color-muted">';
      h += '<span>' + (v.vcpu || '?') + ' vCPU</span>';
      h += '<span>' + (v.memory_mb || '?') + ' MB</span>';
      h += '<span>' + H.badge(v.state || '?', on ? 'g' : 'r') + '</span>';
      h += '</div></div>';
    });
    h += '</div>';

    h += '<h3 style="margin:16px 0 8px">' + _L('마이그레이션 대상 노드', 'Migration Target Nodes') + '</h3>';
    h += '<div class="sg grid-3">';
    var nodes = (typeof MON_NODES !== 'undefined' && MON_NODES) ? MON_NODES : [{name:'Node1',ip:'localhost'}];
    nodes.forEach(function(nd) {
      h += '<div class="hc" style="text-align:center;padding:20px;border:2px dashed var(--border);transition:border-color 0.2s" '
        + 'ondragover="event.preventDefault();this.style.borderColor=\'var(--accent)\'" '
        + 'ondragleave="this.style.borderColor=\'var(--border)\'" '
        + 'ondrop="event.preventDefault();this.style.borderColor=\'var(--border)\';vmMigrateDrop(event.dataTransfer.getData(\'text/plain\'),\'' + esc(nd.ip) + '\',\'' + esc(nd.name) + '\')">'
        + '<div style="font-size:24px;margin-bottom:6px">&#128421;</div>'
        + '<div class="text-13 font-600">' + esc(nd.name) + '</div>'
        + '<div class="color-muted text-xs">' + esc(nd.ip) + '</div>'
        + '</div>';
    });
    h += '</div>';
  } else {
    l.forEach((v, i) => {
      const ri = vmList.indexOf(v);
      const on = v.state === 'running';
      const cp = v.live_cpu_pct || 0;
      const c = cp > 85 ? 'var(--red)' : cp > 60 ? 'var(--yellow)' : 'var(--green)';
      const star = favs.includes(v.name) ? '&#9733;' : '&#9734;';
      h += `<div class="vi ${ri === selectedVmIndex ? 'active' : ''}" onclick="selectedVmIndex=${ri};currentTab=localStorage.getItem('pcv-last-vm-tab')||'summary';switchSbTab('vms');document.querySelectorAll('#ct button').forEach(b=>b.classList.toggle('active',b.dataset.t==='summary'));render()" oncontextmenu="showCtx(event,${ri})"><input type="checkbox" ${checkedVms.has(ri) ? 'checked' : ''} onclick="event.stopPropagation();toggleChk(${ri})"><span class="fav-star" onclick="event.stopPropagation();toggleFavorite('${escapeAttr(v.name)}')" title="Favorite">${star}</span><span class="dot ${on ? 'on' : 'off'}"></span><span class="nm">${escapeHtml(v.name)}</span><span class="mini-bar"><span class="mini-fill pcv-bar-fill-inline" style="--bw:${cp}%;--bc:${c}"></span></span><span class="st">${cp.toFixed(0)}%</span><canvas class="vm-spark" id="spark-${esc(v.name)}" width="40" height="14" style="vertical-align:middle;margin-left:4px"></canvas></div>`;
    });
  }
  document.getElementById('vl').innerHTML = h;

  setTimeout(function() {
    vmList.forEach(function(v) {
      var canvas = document.getElementById('spark-' + v.name);
      if (!canvas) return;
      var hist = _vmCpuHist[v.name] || [];
      if (hist.length < 2) return;
      var ctx = canvas.getContext('2d');
      var w = canvas.width, ht = canvas.height;
      ctx.clearRect(0, 0, w, ht);
      ctx.strokeStyle = 'rgba(0,240,255,0.6)';
      ctx.lineWidth = 1;
      ctx.beginPath();
      for (var si = 0; si < hist.length; si++) {
        var x = (si / (hist.length - 1)) * w;
        var y = ht - (hist[si] / 100) * ht;
        if (si === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
      }
      ctx.stroke();
    });
  }, 50);
  document.getElementById('vc').textContent = vmList.length;
  document.getElementById('sb2').textContent = vmList[selectedVmIndex] ? vmList[selectedVmIndex].name : t('no_vm');
  document.getElementById('bbtn').style.display = checkedVms.size > 0 ? 'inline' : 'none';

  const elapsed = Math.round((Date.now() - lastLoadTime) / 1000);
  const sb3 = document.getElementById('sb3');
  if (sb3) sb3.textContent = 'Updated ' + elapsed + 's ago';

  const ctBar = document.getElementById('ct');
  if (ctBar && ['summary','console','snapshots','performance','timeline'].includes(currentTab)) {
    ctBar.style.display = 'flex';
  }
  if (!skipContent) renderContent();
}

function toggleChk(i) {
  checkedVms.has(i) ? checkedVms.delete(i) : checkedVms.add(i);
  render();
}

async function bulkStop() {
  if (!await customConfirm(t('btn.stop_selected'), 'Stop ' + checkedVms.size + ' VMs?')) return;
  for (const i of checkedVms) {
    await fetchPost(EP.VM_STOP(vmList[i].name), {});
    addEvt('VM Bulk stop — ' + vmList[i].name);
  }
  checkedVms.clear();
  setTimeout(loadAll, 1500);
}


document.addEventListener('keydown', function(e) {
  if (e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA' || e.target.tagName === 'SELECT') return;
  if (currentTab !== 'summary' && currentTab !== 'dashboard' && currentTab !== 'console' && currentTab !== 'snapshots' && currentTab !== 'performance') return;
  if (e.key === 'ArrowDown' || e.key === 'j') {
    e.preventDefault();
    if (selectedVmIndex < vmList.length - 1) { selectedVmIndex++; render(); }
  } else if (e.key === 'ArrowUp' || e.key === 'k') {
    e.preventDefault();
    if (selectedVmIndex > 0) { selectedVmIndex--; render(); }
  } else if (e.key === 'Enter') {
    e.preventDefault();
    currentTab = 'summary'; switchSbTab('vms'); render();
  }
});






var _vmCpuHist = {};


function showCtx(e, i) {
  e.preventDefault();
  selectedVmIndex = i;
  const m = document.getElementById('ctx');
  const ri = i;
  m.innerHTML = `<div class="ci" onclick="vmPower('start')">&#9654; ${t('power.start')}</div><div class="ci" onclick="vmPower('stop')">&#9632; ${t('power.stop')}</div><div class="sep"></div><div class="ci" onclick="showSnap()">&#128247; ${t('vm.snapshot')}</div><div class="ci" onclick="showSettings()">&#9881; ${t('vm.settings')}</div><div class="ci" onclick="showRenameVm()">&#9998; ${_L('이름 변경', 'Rename')}</div><div class="ci" onclick="showVnc()">&#128424; VNC</div><div class="sep"></div><div class="ci" onclick="showMemStats()">&#128204; Memory Stats</div><div class="ci" onclick="showCpuStats()">&#9881; CPU Stats</div><div class="ci" onclick="showDiskLiveResize()">&#128190; Disk Resize</div><div class="ci" onclick="showGuestAgent()">&#128172; Guest Agent</div><div class="sep"></div><div class="ci" onclick="showNicMgr()">&#127760; NIC</div><div class="ci" onclick="vmClone(${ri})">&#128203; Clone</div><div class="ci" onclick="vmExportOva(${ri})">&#128230; Export OVA</div><div class="sep"></div><div class="ci" onclick="vmDel()">&#10060; ${t('btn.delete')}</div>`;
  m.style.display = 'block';
  m.style.left = e.pageX + 'px';
  m.style.top = e.pageY + 'px';
  render();
}

function _vmStripCidr(v) {
  return String(v || '').split('/')[0];
}

function _vmNetSource(nic) {
  return nic.bridge || nic.source || nic.network || nic.type || '-';
}

function _vmNetworkMap(networks) {
  var map = {};
  (networks || []).forEach(function(n) {
    if (n && n.name) map[n.name] = n;
  });
  return map;
}

function _vmNicDns(nic, netMap) {
  var raw = String((nic && nic.dns) || '').trim();
  if (raw && raw !== 'off') return escapeHtml(raw);
  if (raw === 'off') return '<span class="color-muted">OFF</span>';

  var source = _vmNetSource(nic);
  var meta = netMap[source];
  if (meta && meta.dhcp && meta.ip_cidr)
    return escapeHtml(_vmStripCidr(meta.ip_cidr));
  return '-';
}

function _vmRenderNicDetails(nics, networks, v) {
  var netMap = _vmNetworkMap(networks);
  if (!Array.isArray(nics) || nics.length === 0) {
    var count = v && v.network_count ? String(v.network_count) : '0';
    return '<div class="color-muted text-xs" style="margin-top:8px">' +
      (count === '0' ? _L('할당된 NIC 없음', 'No assigned NICs')
                     : _L('NIC 상세 조회 불가', 'NIC details unavailable')) +
      '</div>';
  }

  var h = '<div style="margin-top:8px;border-top:1px solid var(--border);padding-top:6px">';
  nics.forEach(function(nic, idx) {
    var source = _vmNetSource(nic);
    var ip = nic.ip || '';
    var model = nic.model || 'virtio';
    var mac = nic.mac || '-';
    var target = nic.target ? ' / ' + nic.target : '';
    h += '<div style="padding:5px 0;border-bottom:1px solid rgba(255,255,255,.06)">';
    h += H.row('NIC ' + (idx + 1), '<span class="color-accent">' + escapeHtml(source) + '</span> <span class="color-muted text-xs">' + escapeHtml(model + target) + '</span>');
    h += H.row('MAC', '<span class="text-xs">' + escapeHtml(mac) + '</span>');
    h += H.row('IP', ip ? '<span class="color-green">' + escapeHtml(ip) + '</span>' : '<span class="color-muted">-</span>');
    h += H.row('DNS', _vmNicDns(nic, netMap));
    h += '</div>';
  });
  return h + '</div>';
}

function _vmPrimaryNicValue(nics, field) {
  if (!Array.isArray(nics)) return '';
  for (var i = 0; i < nics.length; i++) {
    if (nics[i] && nics[i][field]) return nics[i][field];
  }
  return '';
}


async function renderSummary(b, v) {
  if (!v) { b.innerHTML = '<p class="color-muted">' + t('vm.select') + '</p>'; return; }
  const on = v.state === 'running';


  var metrics = {};
  var nics = [];
  var networks = [];
  var summaryReqs = [
    on ? fetchGet(EP.VM_DETAIL(v.name)) : Promise.resolve({}),
    fetchGet(EP.VM_NICS(v.name)),
    fetchGet(EP.NET_LIST())
  ];
  var summaryResults = await Promise.allSettled(summaryReqs);
  if (summaryResults[0].status === 'fulfilled')
    metrics = unwrapData(summaryResults[0].value) || summaryResults[0].value || {};
  if (summaryResults[1].status === 'fulfilled')
    nics = unwrapList(summaryResults[1].value);
  if (summaryResults[2].status === 'fulfilled')
    networks = unwrapList(summaryResults[2].value);

  var cpuPct = metrics.cpu || v.live_cpu_pct || 0;
  var memPct = metrics.mem || v.mem_percent || 0;
  var vcpu = metrics.vcpu || v.vcpu || '-';
  var memMb = metrics.memory_mb || v.memory_mb || '-';
  var diskRd = metrics.disk_rd || v.disk_rd || 0;
  var diskWr = metrics.disk_wr || v.disk_wr || 0;
  var netRx = metrics.net_rx || v.net_rx || 0;
  var netTx = metrics.net_tx || v.net_tx || 0;
  var primaryIp = _vmPrimaryNicValue(nics, 'ip') || v.ip || '-';
  var primaryDns = nics.length ? _vmNicDns(nics[0], _vmNetworkMap(networks)) : '-';
  var diskUsageAction = on
    ? '<button class="btn btn-xs" onclick="showVmDiskUsage()">&#128202; ' + _L('디스크 사용량', 'Disk Usage') + '</button>'
    : '<span class="color-muted text-xs">' + _L('실행 중인 VM에서 확인 가능', 'Available while running') + '</span>';


  v.live_cpu_pct = cpuPct;
  v.mem_percent = memPct;
  v.vcpu = vcpu;
  v.memory_mb = memMb;
  v.ip = primaryIp;

  const cpuHi = cpuPct > 85;
  b.innerHTML = '<div class="flex gap-10 items-center mb-14"><span class="neon-blink color-accent">&gt;&gt;</span><h2 style="font-family:var(--font-display);font-size:16px;letter-spacing:.05em">' + escapeHtml(v.name) + '</h2>' + H.badge(v.state + (cpuHi ? ' [HIGH_LOAD]' : ''), on ? 'g' : 'r') + '</div>'
+ '<div class="sg">'
+ H.card('&#128187; System', H.row('Guest OS', 'Linux (KVM)') + H.row('UUID', '<span class="text-xs">' + escapeHtml(v.uuid || '-') + '</span>') + H.row(_L('부트', 'Boot'), escapeHtml((v.boot_mode || 'bios').toUpperCase())) + H.row(_L('자동시작', 'Auto Start'), v.auto_start ? '<span class="color-green">ON</span>' : '<span class="color-muted">OFF</span>'))
+ H.card('&#9881; CPU', H.row('vCPU', escapeHtml(String(vcpu))) + H.row(_L('사용률', 'Usage'), '<span class="' + (cpuHi ? 'color-red' : 'color-green') + '">' + cpuPct.toFixed(1) + '%</span>') + renderProgressBar(cpuPct))
+ H.card('&#128204; ' + _L('메모리', 'Memory'), H.row(_L('할당', 'Allocated'), escapeHtml(String(memMb)) + ' MB') + H.row(_L('사용률', 'Usage'), memPct.toFixed(1) + '%') + renderProgressBar(memPct))
+ H.card('&#128190; ' + _L('스토리지', 'Storage'), H.row(_L('타입', 'Type'), H.badge(escapeHtml(v.storage_type || '-'), v.storage_type === 'zvol' ? 'g' : 'y')) + H.row(_L('포맷', 'Format'), escapeHtml(v.disk_format || '-')) + H.row(_L('경로', 'Path'), '<span class="text-xs">' + escapeHtml(v.disk_path || '-') + '</span>') + H.row(_L('게스트 사용량', 'Guest Usage'), diskUsageAction) + H.row(_L('스냅샷', 'Snapshots'), escapeHtml(String(v.snapshot_count || 0))) + H.row('NIC', escapeHtml(String(v.network_count || 0))))
+ H.card('&#128190; Disk I/O', H.row(_L('읽기', 'Read'), '<span class="color-cyan">' + formatBytes(diskRd) + '</span>') + H.row(_L('쓰기', 'Write'), '<span class="color-peach">' + formatBytes(diskWr) + '</span>') + H.row('IOPS R', '<span class="color-cyan">' + (metrics.disk_rd_req || 0).toLocaleString() + '</span>') + H.row('IOPS W', '<span class="color-peach">' + (metrics.disk_wr_req || 0).toLocaleString() + '</span>'))
+ '<div class="hc glitch-panel"><h4>&#127760; ' + _L('네트워크', 'Network') + '</h4>' + H.row('RX', '<span class="color-yellow">' + formatBytes(netRx) + '</span>') + H.row('TX', '<span class="color-yellow">' + formatBytes(netTx) + '</span>') + H.row('IP', primaryIp && primaryIp !== '-' ? '<span class="color-green">' + escapeHtml(primaryIp) + '</span>' : '<span class="color-muted">-</span>') + H.row('DNS', primaryDns) + H.row('RX pps', '<span class="color-muted">' + (metrics.net_rx_pkts || 0).toLocaleString() + '</span>') + H.row('TX pps', '<span class="color-muted">' + (metrics.net_tx_pkts || 0).toLocaleString() + '</span>') + _vmRenderNicDetails(nics, networks, v) + '</div>'
+ '<div class="hc" style="grid-column:1/-1"><h4>' + _L('작업', 'Actions') + '</h4>'
+ '<div class="flex gap-4 flex-wrap mb-8"><button class="btn btn-g" onclick="vmPower(\'start\')">&#9654; ' + t('power.start') + '</button><button class="btn" onclick="vmPower(\'suspend\')">&#10074;&#10074; ' + t('power.pause') + '</button><button class="btn" onclick="vmPower(\'resume\')">&#9654;&#9654; ' + t('power.resume') + '</button><button class="btn btn-r" onclick="vmPower(\'stop\')">&#9632; ' + t('power.stop') + '</button></div>'
+ '<div style="display:grid;grid-template-columns:repeat(auto-fill,minmax(100px,1fr));gap:4px"><button class="btn" onclick="showSnap()">' + t('vm.snapshot') + '</button><button class="btn" onclick="showSettings()">' + t('vm.settings') + '</button><button class="btn" onclick="showRenameVm()">&#9998; ' + _L('이름', 'Rename') + '</button><button class="btn" onclick="showNicMgr()">NIC</button><button class="btn" onclick="vmClone(' + selectedVmIndex + ')">&#128203; Clone</button><button class="btn" onclick="vmExportOva(' + selectedVmIndex + ')">&#128230; Export</button><button class="btn" onclick="showImportOva()">&#128229; Import</button><button class="btn" onclick="showMemStats()">&#128204; Mem</button><button class="btn" onclick="showCpuStats()">&#9881; CPU</button><button class="btn" onclick="showVmDiskUsage()">&#128202; ' + _L('디스크 사용량', 'Disk Usage') + '</button><button class="btn" onclick="showDiskLiveResize()">&#128190; Disk</button><button class="btn" onclick="showBlkioEditor()">&#9881; I/O</button><button class="btn" onclick="showGuestAgent()">&#128172; Agent</button></div>'
+ '</div>'
+ '</div>';
}









async function renderConsole(b, v) {
  if (!v) return;
  let vncHtml = '<div class="text-center p-20"><p class="text-14">&#128424; ' + escapeHtml(v.name) + '</p><p class="stat-label mt-8">' + t('loading') + '</p></div>';
  b.innerHTML = '<div style="background:#000;border:1px solid var(--border);border-radius:var(--r);min-height:500px;height:calc(100vh - 200px);position:relative" id="vnc-frame">' + vncHtml + '</div>';
  try {
    const r = await fetchGet(EP.VNC(v.name));
    const d = unwrapData(r);
    const addr = d.vnc_address || d.address || 'localhost';
    const port = d.vnc_port || d.port || '';
    if (port && v.state === 'running') {
      document.getElementById('vnc-frame').innerHTML = `<div class="p-12"><div class="flex gap-12 items-center mb-12 flex-wrap">${H.badge('VNC ' + t('vnc.connected'), 'g')}<span class="text-13 font-600">${escapeHtml(addr)}:${escapeHtml(String(port))}</span><button class="btn btn-g" onclick="openNoVNCPopup('${escapeAttr(addr)}','${escapeAttr(String(port))}','${escapeAttr(v.name)}')">&#128424; ${t('vnc.open_popup')}</button><button class="btn" onclick="openNoVNC('${escapeAttr(addr)}','${escapeAttr(String(port))}')">${t('vnc.embedded')}</button><button class="btn" onclick="copyVncAddr('${escapeAttr(String(port))}')">&#128203; ${t('vnc.copy_addr')}</button></div><div id="vnc-placeholder" style="background:#111;height:calc(100vh - 280px);min-height:400px;border-radius:var(--r);display:flex;align-items:center;justify-content:center;color:var(--fg2)"><div class="text-center"><p class="text-lg">&#128424; ${escapeHtml(v.name)}</p><p class="mt-8">"${t('vnc.open_popup')}"</p><p class="stat-label mt-4">VNC: ${location.hostname}:${escapeHtml(String(port))}</p></div></div></div>`;
    } else {
      document.getElementById('vnc-frame').innerHTML = `<div class="text-center color-muted p-20"><p class="text-14">&#128424; ${escapeHtml(v.name)}</p><p class="mt-8">${v.state === 'running' ? _L('VNC 포트를 사용할 수 없습니다', 'VNC port not available') : _L('VM이 중지 상태입니다', 'VM is stopped')}</p><button class="btn mt-12" onclick="showVnc()">${_L('VNC 확인', 'Check VNC')}</button></div>`;
    }
  } catch (e) {
    document.getElementById('vnc-frame').innerHTML = '<div class="text-center color-muted p-20"><p>' + escapeHtml(t('vnc.unavailable')) + '</p><button class="btn mt-8" onclick="showVnc()">' + escapeHtml(t('vnc.manual_check')) + '</button></div>';
  }
}

function openNoVNC(addr, port) {
  const frame = document.getElementById('vnc-frame');
  if (!frame) return;
  const wsProto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const wsUrl = wsProto + '//' + location.host + '/api/v1/ws/vnc?port=' + port;
  const fullscreenText = t('vnc.fullscreen');
  const fitText = t('vnc.fit');
  const loadingConnectingText = t('vnc.loading_connecting');
  const connectedText = t('vnc.connected');
  const disconnectedText = t('vnc.disconnected');
  const remoteText = t('vnc.remote');
  const errorSuffixText = t('vnc.error_suffix');
  const securityFailureText = t('vnc.security_failure');
  const statusEndpoint = escapeHtml(addr) + ':' + escapeHtml(String(port));
  frame.innerHTML = '<div class="flex gap-6 mb-6 items-center"><button class="btn" onclick="vncFullscreen()" title="' + escapeAttr(fullscreenText) + '">&#9974; ' + escapeHtml(fullscreenText) + '</button><button class="btn" onclick="vncFitWindow()" title="' + escapeAttr(fitText) + '">&#128300; ' + escapeHtml(fitText) + '</button><span id="vnc-res" class="stat-label"></span></div><div id="vnc-container" style="width:100%;height:calc(100vh - 220px);min-height:500px;background:#000;border-radius:var(--r);position:relative"><div id="vnc-status" style="position:absolute;top:8px;left:8px;z-index:10;font-size:11px;color:var(--green);background:rgba(0,0,0,.7);padding:4px 10px;border-radius:4px"><span class="spinner"></span> ' + escapeHtml(loadingConnectingText) + ' ' + statusEndpoint + '...</div>' + vncIsoEjectTipHtml() + '</div>';
  const existing = document.getElementById('novnc-loader');
  if (existing) existing.remove();
  const m = document.createElement('script');
  m.id = 'novnc-loader'; m.type = 'module';
  m.textContent = 'import _mod from "/ui/vendor/novnc/novnc.esm.js";\n'
  + 'const RFB=_mod.default||_mod;\n'
  + 'const wsUrl=' + JSON.stringify(wsUrl) + ';\n'
  + 'const statusEndpoint=' + JSON.stringify(statusEndpoint) + ';\n'
  + 'const connectedText=' + JSON.stringify(escapeHtml(connectedText)) + ';\n'
  + 'const disconnectedText=' + JSON.stringify(escapeHtml(disconnectedText)) + ';\n'
  + 'const errorSuffixText=' + JSON.stringify(escapeHtml(errorSuffixText)) + ';\n'
  + 'const securityFailureText=' + JSON.stringify(escapeHtml(securityFailureText)) + ';\n'
  + 'const remoteText=' + JSON.stringify(remoteText) + ';\n'
  + 'try{\n'
  + 'const container=document.getElementById("vnc-container");\n'
  + 'const st=document.getElementById("vnc-status");\n'
  + 'function setStatusMark(color,text){if(!st)return;const mark=document.createElement("span");mark.style.color=color;mark.textContent="\\u25cf";st.replaceChildren(mark," ",text);}\n'
  + 'if(!container){console.error("no vnc-container");}\n'
  + 'if(typeof RFB!=="function"){throw new Error("RFB loaded as "+typeof RFB+", keys: "+Object.keys(_mod).join(","));}\n'
  + 'const rfb=new RFB(container,wsUrl);\n'
  + 'rfb.scaleViewport=true;rfb.resizeSession=true;rfb.clipViewport=false;rfb.qualityLevel=6;rfb.compressionLevel=2;\n'
  + 'rfb.addEventListener("connect",()=>{setStatusMark("lime",connectedText+": "+statusEndpoint);const ri=document.getElementById("vnc-res");if(ri)ri.textContent=remoteText+": "+rfb._fbWidth+"x"+rfb._fbHeight;});\n'
  + 'rfb.addEventListener("disconnect",(e)=>{setStatusMark("red",disconnectedText+(e.detail.clean?"":" "+errorSuffixText));});\n'
  + 'rfb.addEventListener("securityfailure",()=>{setStatusMark("red",securityFailureText);});\n'
  + 'window._vncRfb=rfb;\n'
  + '}catch(e){const st=document.getElementById("vnc-status");if(st)st.textContent="\\u25cf "+e.message;console.error("noVNC:",e)}\n';
  document.head.appendChild(m);
}

function vncIsoEjectTipHtml() {
  return '<div id="vnc-iso-tip" style="position:absolute;top:10px;right:10px;z-index:11;max-width:340px;background:rgba(8,12,18,.82);border:1px solid rgba(255,255,255,.18);border-radius:6px;padding:8px 10px;color:#e8f6ff;box-shadow:0 8px 28px rgba(0,0,0,.32);font-family:var(--font-ui,system-ui,sans-serif);pointer-events:none">'
    + '<div style="font-size:11px;font-weight:700;color:#80eaff;margin-bottom:3px">' + escapeHtml(t('vnc.iso_eject_title')) + '</div>'
    + '<div style="font-size:11px;line-height:1.45;color:#d9e7ef">' + escapeHtml(t('vnc.iso_eject_body')) + '</div>'
    + '</div>';
}

function vncFullscreen() {
  const el = document.getElementById('vnc-container');
  if (!el) return;
  if (document.fullscreenElement) { document.exitFullscreen(); }
  else { el.requestFullscreen().catch(() => {}); el.style.height = '100vh'; }
}

function vncFitWindow() {
  const rfb = window._vncRfb;
  if (!rfb) return;
  rfb.scaleViewport = true;
  rfb.resizeSession = true;
  const c = document.getElementById('vnc-container');
  if (c) { c.style.height = 'calc(100vh - 220px)'; }
}

function openNoVNCPopup(addr, port, vmName) {
  const wsProto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const wsUrl = wsProto + '//' + location.host + '/api/v1/ws/vnc?port=' + port;
  const w = window.open('', 'vnc_' + port, 'width=1060,height=820,menubar=no,toolbar=no,location=no,status=no,resizable=yes');
  if (!w) { toast(t('vnc.popup_blocked'), false); return; }
  const safeVmName = escapeHtml(vmName);
  const safeAddr = escapeHtml(addr);
  const safePort = escapeHtml(String(port));
  const loadingText = t('loading');
  const reconnectText = t('vnc.reconnect');
  const connectedText = t('vnc.connected');
  const disconnectedText = t('vnc.disconnected');
  const connectingText = t('vnc.connecting');
  const errorSuffixText = t('vnc.error_suffix');
  const securityFailureText = t('vnc.security_failure');
  const isoTipTitle = t('vnc.iso_eject_title');
  const isoTipBody = t('vnc.iso_eject_body');
  w.document.write(`<!DOCTYPE html><html><head><meta charset="UTF-8"><title>VNC: ${escapeHtml(vmName)} (${escapeHtml(addr)}:${escapeHtml(String(port))})</title>
<style>*{margin:0;padding:0;box-sizing:border-box}body{background:#000;overflow:hidden;font-family:monospace}
#bar{background:#0a0a14;color:#00f0ff;padding:6px 12px;font-size:12px;display:flex;align-items:center;gap:12px;border-bottom:1px solid #1a1a3a}
#bar button{background:none;border:1px solid #00f0ff;color:#00f0ff;padding:4px 10px;border-radius:4px;cursor:pointer;font-size:11px}
#bar button:hover{background:#00f0ff;color:#000}
#st{font-size:11px;color:#5a6a8a}
#vc{width:100%;height:calc(100vh - 36px);background:#000}
#install-tip{position:fixed;top:46px;right:12px;z-index:20;max-width:340px;background:rgba(8,12,18,.86);border:1px solid rgba(0,240,255,.35);border-radius:6px;padding:8px 10px;color:#e8f6ff;box-shadow:0 8px 28px rgba(0,0,0,.36);pointer-events:none}
#install-tip .tip-title{font-size:11px;font-weight:700;color:#80eaff;margin-bottom:3px}
#install-tip .tip-body{font-size:11px;line-height:1.45;color:#d9e7ef}</style></head>
<body><div id="bar"><span style="font-weight:700">${safeVmName}</span><span>${safeAddr}:${safePort}</span>
<button id="vnc-fullscreen" type="button">${t('vnc.fullscreen')}</button>
<button id="vnc-reconnect" type="button">${reconnectText}</button>
<span id="st">${loadingText}</span></div><div id="vc"></div>
<div id="install-tip"><div class="tip-title">${escapeHtml(isoTipTitle)}</div><div class="tip-body">${escapeHtml(isoTipBody)}</div></div>
<script type="module">
	import _mod from "/ui/vendor/novnc/novnc.esm.js";
const RFB=_mod.default||_mod;
const wsUrl=${JSON.stringify(wsUrl)};
const vmTitle=${JSON.stringify('VNC: ' + vmName)};
const connectedText=${JSON.stringify(connectedText)};
const disconnectedText=${JSON.stringify(disconnectedText)};
const connectingText=${JSON.stringify(connectingText)};
const errorSuffixText=${JSON.stringify(errorSuffixText)};
const securityFailureText=${JSON.stringify(securityFailureText)};
const st=document.getElementById("st");
const vc=document.getElementById("vc");
let rfb=null;
let connectSeq=0;
function setStatusText(text){if(st)st.textContent=text}
function setStatusMark(color,text){
  if(!st)return;
  const mark=document.createElement("span");
  mark.style.color=color;
  mark.textContent="\\u25cf";
  st.replaceChildren(mark," ",text);
}
function connectVNC(){
  const seq=++connectSeq;
  if(rfb){try{rfb.disconnect()}catch(e){} rfb=null}
  if(vc)vc.replaceChildren();
  setStatusText(connectingText+"...");
  try{
    const next=new RFB(vc,wsUrl);
    rfb=next;
    next.scaleViewport=true;next.resizeSession=true;next.clipViewport=false;next.qualityLevel=6;next.compressionLevel=2;
    next.addEventListener("connect",()=>{if(seq!==connectSeq)return;setStatusMark("lime",connectedText);document.title=vmTitle});
    next.addEventListener("disconnect",(e)=>{if(seq!==connectSeq)return;setStatusMark("red",disconnectedText+(e.detail.clean?"":" "+errorSuffixText))});
    next.addEventListener("securityfailure",()=>{if(seq!==connectSeq)return;setStatusMark("red",securityFailureText)});
    window._popupRfb=next;
  }catch(e){setStatusText(e.message)}
}
document.getElementById("vnc-fullscreen").addEventListener("click",()=>{vc.requestFullscreen().catch(()=>{})});
document.getElementById("vnc-reconnect").addEventListener("click",connectVNC);
window.addEventListener("beforeunload",()=>{if(rfb){try{rfb.disconnect()}catch(e){}}});
window.addEventListener("resize",()=>{if(rfb)rfb.scaleViewport=true});
connectVNC();
<\/script></body></html>`);
  w.document.close();
  toast(vmName + ' VNC ' + t('vnc.open_popup'));
}

function copyVncAddr(port) {
  const addr = location.hostname + ':' + port;
  navigator.clipboard.writeText(addr).then(() => toast(t('vnc.addr_copied') + ': ' + addr)).catch(() => toast(addr, true));
}









async function renderSnapshots(b, v) {
  if (!v) return;
  b.innerHTML = '<div><div class="justify-between items-center mb-12"><h3>' + t('vm.snapshot') + ': ' + esc(v.name) + '</h3><div class="flex gap-6"><button class="btn btn-g" onclick="takeSnap()" class="text-12">+ ' + t('btn.create') + '</button><button class="btn btn-r" onclick="snapDeleteAll(\'' + escapeAttr(v.name) + '\')" class="text-12">&#128465; Delete All</button></div></div><div id="stree"><span class="spinner"></span> ' + t('loading') + '</div></div>';
  try {
    const r = await fetchGet(EP.VM_SNAPSHOT_LIST(v.name));
    const raw = unwrapList(r);

    const snaps = raw.map(s => {
      if (typeof s === 'string') {
        const [full, time] = s.split('\t');
        const atIdx = full.lastIndexOf('@');
        return { name: atIdx >= 0 ? full.substring(atIdx + 1) : full, full_path: full, time: time || '' };
      }
      return { name: s.name || s, full_path: s.name || s, time: s.creation_time || '' };
    });
    if (!snaps.length) { document.getElementById('stree').innerHTML = '<p class="color-muted">' + t('snap.none') + '</p>'; return; }
    let h = '<table><thead><tr><th>Snapshot</th><th>Created</th><th class="w-140">Actions</th></tr></thead><tbody>';
    snaps.forEach(s => {
      h += '<tr><td><b>' + esc(s.name) + '</b></td>';
      h += '<td class="text-xs color-muted">' + esc(s.time) + '</td>';
      h += '<td class="nowrap">';
      const safeName = v.name.replace(/'/g, "\\'");
      const safeSnap = s.name.replace(/'/g, "\\'");
      h += '<button class="btn" style="font-size:10px;padding:3px 8px;margin-right:4px" onclick="snapRb(\'' + safeName + '\',\'' + safeSnap + '\')">' + t('snap.revert_confirm') + '</button>';
      h += '<button class="btn btn-r" style="font-size:10px;padding:3px 8px" onclick="snapDl(\'' + safeName + '\',\'' + safeSnap + '\')">' + t('btn.delete') + '</button>';
      h += '</td></tr>';
    });
    h += '</tbody></table>';
    document.getElementById('stree').innerHTML = h;
  } catch (e) { document.getElementById('stree').innerHTML = '<p class="color-red">' + esc(e.message) + '</p>'; }
}

async function takeSnap() {
  const v = vmList[selectedVmIndex];
  if (!v) return;
  const on = v.state === 'running';
  const ts = new Date().toISOString().replace(/[-:T]/g, '').substring(0, 14);
  const defaultName = 'snap-' + ts;

  let h = '<h2 class="mb-14">&#128247; Create Snapshot</h2>';
  h += '<div class="mb-14 p-10 border-muted rounded-md">';
  h += '<div class="flex items-center gap-8 mb-8"><span style="font-size:18px">&#128187;</span><div><b>' + esc(v.name) + '</b><div class="text-xs">' + (on ? '<span class="color-green">Running</span>' : '<span class="color-muted">Stopped</span>') + ' &bull; ' + (v.vcpu || '?') + ' vCPU &bull; ' + (v.memory_mb || '?') + ' MB</div></div></div>';
  if (on) h += '<div class="text-xs" style="color:var(--yellow);padding:4px 8px;background:rgba(255,200,0,.08);border-radius:4px">&#9888; VM is running. Snapshot will capture live state (crash-consistent).</div>';
  h += '</div>';

  h += '<div class="mb-12"><label class="text-12 font-600">Snapshot Name</label>';
  h += '<input id="snap-name-input" value="' + defaultName + '" class="w-full mt-4" oninput="snapNameValidate()" placeholder="alphanumeric, dash, underscore"></div>';
  h += '<div id="snap-name-err" style="font-size:11px;min-height:16px;margin-bottom:8px"></div>';

  h += '<div class="mb-14"><label class="text-12 font-600">Description <span class="color-muted">(optional)</span></label>';
  h += '<input id="snap-desc-input" placeholder="e.g. Before upgrade, pre-migration backup" class="w-full mt-4"></div>';

  h += '<div id="snap-preview" style="margin-bottom:14px;padding:10px;border:1px solid var(--border);border-radius:6px;font-size:11px">';
  h += '<div class="color-muted mb-4 font-600">Preview</div>';
  h += '<div>ZFS: <code>' + esc(v.name) + '@' + defaultName + '</code></div>';
  h += '</div>';

  h += '<div class="flex gap-6 justify-end">';
  h += '<button class="btn" onclick="closeModal()">Cancel</button>';
  h += '<button class="btn btn-g" id="snap-create-btn" onclick="snapCreateExec()">&#128247; Create Snapshot</button>';
  h += '</div>';

  showModal(h);
  setTimeout(() => { const el = document.getElementById('snap-name-input'); if (el) { el.focus(); el.select(); } }, 100);
}

function snapNameValidate() {
  const el = document.getElementById('snap-name-input');
  const err = document.getElementById('snap-name-err');
  const preview = document.getElementById('snap-preview');
  const btn = document.getElementById('snap-create-btn');
  if (!el) return;
  const n = el.value.trim();
  const valid = /^[a-zA-Z0-9_-]{1,128}$/.test(n);
  if (err) {
    if (!n) err.innerHTML = '<span class="color-red">Name is required</span>';
    else if (!valid) err.innerHTML = '<span class="color-red">Invalid characters (use a-z, 0-9, dash, underscore)</span>';
    else err.innerHTML = '<span class="color-green">&#9989; Valid</span>';
  }
  if (btn) btn.disabled = !valid || !n;
  const v = vmList[selectedVmIndex];
  if (preview && v) preview.innerHTML = '<div class="color-muted mb-4 font-600">Preview</div><div>ZFS: <code>' + esc(v.name) + '@' + esc(n) + '</code></div>';
}

async function snapCreateExec() {
  const v = vmList[selectedVmIndex];
  if (!v) return;
  const n = (document.getElementById('snap-name-input')?.value || '').trim();
  if (!n || !/^[a-zA-Z0-9_-]{1,128}$/.test(n)) { toast('Invalid snapshot name', false); return; }
  const btn = document.getElementById('snap-create-btn');
  if (btn) { btn.disabled = true; btn.innerHTML = '<span class="spinner"></span> Creating...'; }
  try {
    const r = await fetchPost(EP.VM_SNAPSHOT_CREATE(v.name), { snap_name: n });
    if (r && r.error) { toast('Create failed: ' + (r.error.message || ''), false); if (btn) { btn.disabled = false; btn.innerHTML = '&#128247; Retry'; } return; }
    toast(t('snap.created') + ': ' + n);
    addEvt('VM Snapshot created — ' + v.name + '@' + n);
    closeModal();
    renderSnapshots(document.getElementById('cb'), v);
  } catch (e) { toast('Error: ' + e.message, false); if (btn) { btn.disabled = false; btn.innerHTML = '&#128247; Retry'; } }
}

async function snapRb(vm, s) {
  const v = vmList.find(x => x.name === vm);
  const on = v && v.state === 'running';

  let h = '<h2 class="mb-14">&#9194; Rollback Snapshot</h2>';


  h += '<div style="margin-bottom:14px;padding:12px;border:1px solid var(--red);border-radius:6px;background:rgba(255,60,60,.06)">';
  h += '<div style="font-weight:700;color:var(--red);margin-bottom:6px">&#9888; Destructive Operation</div>';
  h += '<div class="text-xs color-muted">This will revert the VM disk to the snapshot point-in-time. <b>All data written after this snapshot will be permanently lost.</b></div>';
  if (on) h += '<div class="text-xs" style="color:var(--yellow);margin-top:6px">&#9889; VM is currently <b>running</b> — it will be <b>force-stopped</b> before rollback, then automatically restarted.</div>';
  h += '</div>';


  h += '<div class="mb-14 p-10 border-muted rounded-md">';
  h += '<div style="display:grid;grid-template-columns:100px 1fr;gap:4px 12px;font-size:12px">';
  h += '<span class="color-muted">VM</span><span><b>' + esc(vm) + '</b> ' + (on ? '<span class="color-green">Running</span>' : '<span class="color-muted">Stopped</span>') + '</span>';
  h += '<span class="color-muted">Snapshot</span><span><code>' + esc(s) + '</code></span>';
  h += '<span class="color-muted">ZFS Path</span><span class="text-xs"><code>' + esc(vm) + '@' + esc(s) + '</code></span>';
  h += '</div></div>';


  h += '<div class="mb-14"><label class="text-12 font-600">Type VM name to confirm: <code>' + esc(vm) + '</code></label>';
  h += '<input id="rb-confirm-input" placeholder="' + esc(vm) + '" class="w-full mt-4" oninput="rbValidate(\'' + vm.replace(/'/g, "\\'") + '\')"></div>';

  h += '<div class="flex gap-6 justify-end">';
  h += '<button class="btn" onclick="closeModal()">Cancel</button>';
  h += '<button class="btn btn-r" id="rb-exec-btn" disabled onclick="rbExec(\'' + vm.replace(/'/g, "\\'") + '\',\'' + s.replace(/'/g, "\\'") + '\')">&#9194; Rollback</button>';
  h += '</div>';

  showModal(h);
  setTimeout(() => { const el = document.getElementById('rb-confirm-input'); if (el) el.focus(); }, 100);
}

function rbValidate(vm) {
  const input = (document.getElementById('rb-confirm-input')?.value || '').trim();
  const btn = document.getElementById('rb-exec-btn');
  if (btn) btn.disabled = input !== vm;
}

async function rbExec(vm, s) {
  const btn = document.getElementById('rb-exec-btn');
  if (btn) { btn.disabled = true; btn.innerHTML = '<span class="spinner"></span> Rolling back...'; }
  try {
    const r = await fetchPost(EP.VM_SNAPSHOT_ROLLBACK(vm), { snap_name: s });
    if (r && r.error) { toast('Rollback failed: ' + (r.error.message || ''), false); if (btn) { btn.disabled = false; btn.innerHTML = '&#9194; Retry'; } return; }
    toast('Rollback accepted: ' + vm + '@' + s);
    addEvt('VM Snapshot rollback — ' + vm + '@' + s);
    closeModal();
    renderSnapshots(document.getElementById('cb'), vmList[selectedVmIndex]);
  } catch (e) { toast('Error: ' + e.message, false); if (btn) { btn.disabled = false; btn.innerHTML = '&#9194; Retry'; } }
}

async function snapDl(vm, s) {
  if (!await customConfirm('Delete snapshot "' + s + '"?')) return;
  try {
    const r = await fetchDelete(EP.VM_SNAPSHOT_DELETE(vm, s));
    if (r && r.error) { toast('Delete failed: ' + (r.error.message || JSON.stringify(r.error)), false); return; }
    toast(t('snap.deleted') + ': ' + s);
    addEvt('VM Snapshot deleted — ' + vm + '@' + s);
    renderSnapshots(document.getElementById('cb'), vmList[selectedVmIndex]);
  } catch (e) { toast('Delete error: ' + e.message, false); }
}

async function snapDeleteAll(vm) {

  let snaps = [];
  try {
    const r = await fetchGet(EP.VM_SNAPSHOT_LIST(vm));
    const raw = unwrapList(r);
    snaps = raw.map(s => {
      if (typeof s === 'string') {
        const [full, time] = s.split('\t');
        const at = full.lastIndexOf('@');
        return { name: at >= 0 ? full.substring(at + 1) : full, time: time || '' };
      }
      return { name: s.name || s, time: s.creation_time || '' };
    });
  } catch (e) { toast('Failed to load snapshots', false); return; }

  if (snaps.length === 0) { toast('No snapshots to delete'); return; }


  let h = '<h2 class="mb-12">&#128465; Bulk Delete Snapshots</h2>';
  h += '<div class="mb-12"><span class="color-muted">VM:</span> <b>' + esc(vm) + '</b> &mdash; <span class="color-accent">' + snaps.length + '</span> snapshots</div>';

  h += '<div class="mb-14 p-10 border-muted rounded-md">';
  h += '<div style="margin-bottom:8px;font-weight:600;font-size:12px">Options</div>';
  h += '<div class="mb-8"><label class="text-12">Prefix filter <span class="color-muted">(empty = all)</span></label>';
  h += '<input id="sda-prefix" placeholder="e.g. pcv-repl-" class="w-full mt-4" oninput="sdaPreview()"></div>';
  h += '<div><label class="text-12">Keep recent</label>';
  h += '<input id="sda-keep" type="number" min="0" value="0" style="width:80px;margin-left:8px" oninput="sdaPreview()"> <span class="color-muted text-xs">snapshots</span></div>';
  h += '</div>';

  h += '<div id="sda-preview" style="margin-bottom:14px;padding:10px;border:1px solid var(--border);border-radius:6px;max-height:200px;overflow-y:auto;font-size:11px"></div>';

  h += '<div class="flex gap-6 justify-end">';
  h += '<button class="btn" onclick="closeModal()">Cancel</button>';
  h += '<button class="btn btn-r" id="sda-exec-btn" onclick="sdaExec(\'' + vm.replace(/'/g, "\\'") + '\')">&#128465; Delete <span id="sda-count">0</span> Snapshots</button>';
  h += '</div>';

  showModal(h);


  window._sdaSnaps = snaps;
  window._sdaVm = vm;
  sdaPreview();
}

function sdaPreview() {
  const snaps = window._sdaSnaps || [];
  const prefix = (document.getElementById('sda-prefix')?.value || '').trim();
  const keep = parseInt(document.getElementById('sda-keep')?.value) || 0;
  const el = document.getElementById('sda-preview');
  const countEl = document.getElementById('sda-count');
  if (!el) return;


  let filtered = prefix ? snaps.filter(s => s.name.startsWith(prefix)) : [...snaps];
  const total = filtered.length;
  const toDelete = keep > 0 && keep < total ? total - keep : (keep >= total ? 0 : total);
  const delList = filtered.slice(0, toDelete);
  const keepList = filtered.slice(toDelete);

  let h = '<div style="font-weight:600;margin-bottom:6px;color:var(--red)">Will DELETE: ' + delList.length + '</div>';
  if (delList.length > 0) {
    delList.forEach(s => {
      h += '<div style="color:var(--red);padding:1px 0">&#10060; ' + esc(s.name) + ' <span class="color-muted">' + esc(s.time) + '</span></div>';
    });
  } else {
    h += '<div class="color-muted">No snapshots match the criteria</div>';
  }
  if (keepList.length > 0) {
    h += '<div style="font-weight:600;margin-top:8px;margin-bottom:4px;color:var(--green)">Will KEEP: ' + keepList.length + '</div>';
    keepList.forEach(s => {
      h += '<div style="color:var(--green);padding:1px 0">&#9989; ' + esc(s.name) + ' <span class="color-muted">' + esc(s.time) + '</span></div>';
    });
  }
  el.innerHTML = h;
  if (countEl) countEl.textContent = delList.length;

  const btn = document.getElementById('sda-exec-btn');
  if (btn) btn.disabled = delList.length === 0;
}

async function sdaExec(vm) {
  const prefix = (document.getElementById('sda-prefix')?.value || '').trim();
  const keep = parseInt(document.getElementById('sda-keep')?.value) || 0;
  const btn = document.getElementById('sda-exec-btn');
  if (btn) { btn.disabled = true; btn.innerHTML = '<span class="spinner"></span> Deleting...'; }
  try {
    const body = { keep_recent: keep };
    if (prefix) body.prefix = prefix;
    const r = await fetchPost(EP.VM_SNAPSHOT_DELETE_ALL(vm), body);
    if (r && r.error) { toast('Delete failed: ' + (r.error.message || ''), false); return; }
    const d = unwrapData(r);
    toast('Deleted ' + (d.deleted || 0) + ' snapshots (remaining: ' + (d.remaining || 0) + ')');
    addEvt('Snapshot bulk delete — ' + vm + ': ' + (d.deleted || 0) + ' deleted');
    closeModal();
    renderSnapshots(document.getElementById('cb'), vmList[selectedVmIndex]);
  } catch (e) { toast('Error: ' + e.message, false); }
  if (btn) { btn.disabled = false; btn.innerHTML = '&#128465; Done'; }
}


var perfLayout = 'auto';

async function renderPerformance(b, v) {
  if (!v) return;

  var metrics = {};
  if (v.state === 'running') {
    try { var mr = await fetchGet(EP.VM_DETAIL(v.name)); metrics = unwrapData(mr) || mr || {}; } catch(e) {}
  }
  var cpuPct = metrics.cpu || v.live_cpu_pct || 0;
  var memPct = metrics.mem || v.mem_percent || 0;
  v.live_cpu_pct = cpuPct;
  v.mem_percent = memPct;

  cpuHistory.push(cpuPct); cpuHistory.shift();
  memHistory.push(memPct); memHistory.shift();
  var chartH = perfLayout === 'manual' ? '120px' : '80px';
  var gridCls = perfLayout === 'auto' ? 'sg grid-2' : '';
  var gridStyle = perfLayout === 'auto' ? '' : 'display:flex;flex-direction:column;gap:12px';
  b.innerHTML = '<div class="justify-between items-center mb-12"><h3>' + t('tab.performance') + ': ' + escapeHtml(v.name) + '</h3><div class="flex gap-6"><button class="tb ' + (perfLayout === 'auto' ? '' : 'btn') + '" onclick="perfLayout=\'auto\';renderPerformance(document.getElementById(\'cb\'),vmList[selectedVmIndex])" class="text-11">&#9638; Auto</button><button class="tb ' + (perfLayout === 'manual' ? '' : 'btn') + '" onclick="perfLayout=\'manual\';renderPerformance(document.getElementById(\'cb\'),vmList[selectedVmIndex])" class="text-11">&#9776; Stack</button></div></div>'
+ '<div class="' + gridCls + '" style="' + gridStyle + '">'
+ H.card('CPU Usage (60s) — ' + cpuPct.toFixed(1) + '%', renderProgressBar(cpuPct) + '<div style="position:relative;height:' + chartH + ';width:100%;margin-top:6px"><canvas id="cg"></canvas></div>')
+ H.card('Memory Usage (60s) — ' + memPct.toFixed(1) + '%', renderProgressBar(memPct) + '<div style="position:relative;height:' + chartH + ';width:100%;margin-top:6px"><canvas id="mg"></canvas></div>')
+ H.card('Disk IOPS', H.row(_L('읽기', 'Read'), '<span class="color-cyan">' + (metrics.disk_rd_req || 0).toLocaleString() + ' ops</span>') + H.row(_L('쓰기', 'Write'), '<span class="color-peach">' + (metrics.disk_wr_req || 0).toLocaleString() + ' ops</span>') + H.row('I/O Read', '<span class="color-cyan">' + formatBytes(metrics.disk_rd || 0) + '</span>') + H.row('I/O Write', '<span class="color-peach">' + formatBytes(metrics.disk_wr || 0) + '</span>'))
+ H.card('Network Packets', H.row('RX', '<span class="color-yellow">' + formatBytes(metrics.net_rx || 0) + '</span> (' + (metrics.net_rx_pkts || 0).toLocaleString() + ' pps)') + H.row('TX', '<span class="color-yellow">' + formatBytes(metrics.net_tx || 0) + '</span> (' + (metrics.net_tx_pkts || 0).toLocaleString() + ' pps)'))
+ '</div>';
  setTimeout(function() {
    createLineChart('cg', cpuHistory, 'CPU %', getChartColor('cpu'));
    createLineChart('mg', memHistory, 'MEM %', getChartColor('mem'));
  }, 30);
}


function renderTimeline(b, v) {
  if (!v) { b.innerHTML = '<p class="color-muted">' + t('vm.select') + '</p>'; return; }
  var events = (eventLog || []).filter(function(e) {
    var msg = (e.msg || e.raw || '').toLowerCase();
    return msg.includes(v.name.toLowerCase());
  }).slice(-20);

  var h = '<h3 class="mb-14">' + _L('타임라인', 'Timeline') + ': ' + esc(v.name) + '</h3>';
  if (events.length === 0) {
    h += '<div class="empty-state" style="text-align:center;padding:30px"><div style="font-size:36px;opacity:.5">&#128337;</div><div class="color-muted mt-8">' + _L('이벤트 없음', 'No events yet') + '</div></div>';
  } else {
    h += '<div style="position:relative;padding-left:24px;border-left:2px solid var(--border)">';
    events.forEach(function(e) {
      var msg = e.msg || e.raw || '';
      var isErr = msg.toLowerCase().includes('error') || msg.toLowerCase().includes('fail');
      var isOk = msg.toLowerCase().includes('start') || msg.toLowerCase().includes('created') || msg.toLowerCase().includes('completed');
      var color = isErr ? 'var(--red)' : isOk ? 'var(--green)' : 'var(--accent)';
      var icon = isErr ? '&#10060;' : isOk ? '&#9989;' : '&#128312;';
      h += '<div style="position:relative;margin-bottom:14px">';
      h += '<div style="position:absolute;left:-30px;top:2px;width:12px;height:12px;border-radius:50%;background:' + color + ';border:2px solid var(--bg)"></div>';
      h += '<div style="font-size:10px;color:var(--fg2);margin-bottom:2px">' + esc(e.time || '') + '</div>';
      h += '<div style="font-size:12px;color:var(--fg);padding:6px 10px;background:var(--bg2);border-radius:4px;border-left:3px solid ' + color + '">' + icon + ' ' + esc(msg) + '</div>';
      h += '</div>';
    });
    h += '</div>';
  }
  b.innerHTML = h;
}

async function showVmCompare() {
  if (checkedVms.size < 2) { toast(_L('비교할 VM을 2개 이상 선택하세요', 'Select 2+ VMs to compare'), false); return; }
  var selected = vmList.filter(function(v, idx) { return checkedVms.has(idx); }).slice(0, 4);

  await Promise.all(selected.map(async function(v) {
    if (v.state === 'running') {
      try {
        var mr = await fetchGet(EP.VM_DETAIL(v.name));
        var m = unwrapData(mr) || mr || {};
        v.live_cpu_pct = m.cpu || v.live_cpu_pct || 0;
        v.mem_percent = m.mem || v.mem_percent || 0;
        v.disk_rd = m.disk_rd || v.disk_rd || 0;
        v.disk_wr = m.disk_wr || v.disk_wr || 0;
        v.net_rx = m.net_rx || v.net_rx || 0;
        v.net_tx = m.net_tx || v.net_tx || 0;
      } catch(e) {}
    }
  }));
  var h = '<h2>' + _L('VM 비교', 'VM Comparison') + '</h2>';
  h += '<table class="text-12 w-full"><thead><tr><th>' + _L('항목', 'Property') + '</th>';
  selected.forEach(function(v) { h += '<th>' + esc(v.name) + '</th>'; });
  h += '</tr></thead><tbody>';
  var props = [
    { key: 'state', label: _L('상태', 'State') },
    { key: 'vcpu', label: 'vCPU' },
    { key: 'memory_mb', label: _L('메모리', 'Memory') + ' (MB)' },
    { key: 'live_cpu_pct', label: 'CPU %' },
    { key: 'mem_percent', label: _L('메모리', 'Memory') + ' %' },
    { key: 'disk_rd', label: _L('디스크 읽기', 'Disk Read') },
    { key: 'disk_wr', label: _L('디스크 쓰기', 'Disk Write') },
    { key: 'net_rx', label: 'Net RX' },
    { key: 'net_tx', label: 'Net TX' },
    { key: 'uuid', label: 'UUID' },
  ];
  props.forEach(function(p) {
    h += '<tr><td class="color-muted"><b>' + p.label + '</b></td>';
    selected.forEach(function(v) {
      var val = v[p.key];
      if (p.key === 'state') val = H.badge(val || '?', val === 'running' ? 'g' : 'r');
      else if (p.key === 'live_cpu_pct' || p.key === 'mem_percent') val = (val || 0).toFixed(1) + '%';
      else if (p.key === 'disk_rd' || p.key === 'disk_wr' || p.key === 'net_rx' || p.key === 'net_tx') val = formatBytes(val || 0);
      else val = esc(String(val || '-'));
      h += '<td>' + val + '</td>';
    });
    h += '</tr>';
  });
  h += '</tbody></table>';
  h += '<div class="text-right mt-14"><button class="btn" onclick="closeModal()">' + t('btn.close') + '</button></div>';
  showModal(h);
}

function showBulkActions() {
  if (checkedVms.size === 0) { toast(_L('VM을 선택하세요', 'Select VMs first'), false); return; }
  var count = checkedVms.size;
  var names = Array.from(checkedVms).map(function(idx) { return vmList[idx] ? vmList[idx].name : '?'; }).join(', ');
  var h = '<h2>' + _L('일괄 작업', 'Bulk Actions') + ' (' + count + ' VMs)</h2>';
  h += '<p class="mb-12 color-muted text-xs">' + esc(names) + '</p>';
  h += '<div class="sg grid-2">';
  h += H.card('&#9654; ' + _L('일괄 시작', 'Start All'), '<button class="btn btn-g w-full" onclick="bulkAction(\'start\')">' + t('power.start') + ' ' + count + ' VMs</button>');
  h += H.card('&#9632; ' + _L('일괄 중지', 'Stop All'), '<button class="btn btn-r w-full" onclick="bulkAction(\'stop\')">' + t('power.stop') + ' ' + count + ' VMs</button>');
  h += H.card('&#128247; ' + _L('일괄 스냅샷', 'Snapshot All'), '<input id="bulk-snap-name" placeholder="snap-' + Date.now() + '" class="w-full mb-6"><button class="btn w-full" onclick="bulkSnapshot()">' + t('snap.created') + '</button>');
  h += H.card('&#10074;&#10074; ' + _L('일괄 일시정지', 'Suspend All'), '<button class="btn w-full" onclick="bulkAction(\'suspend\')">' + t('power.pause') + ' ' + count + ' VMs</button>');
  h += '</div>';
  h += '<div class="text-right mt-14"><button class="btn" onclick="closeModal()">' + t('btn.close') + '</button></div>';
  showModal(h);
}

async function bulkAction(action) {
  closeModal();
  var names = Array.from(checkedVms).map(function(idx) { return vmList[idx] ? vmList[idx].name : null; }).filter(Boolean);
  showModal('<h2>' + _L('일괄 작업 진행 중', 'Bulk Action in Progress') + '</h2><div class="prog-bar"><div class="prog-fill" id="bulk-prog" class="w-pct-0"></div></div><div id="bulk-status" class="prog-status"><span class="spinner"></span> 0/' + names.length + '</div>');
  var pf = document.getElementById('bulk-prog');
  var ps = document.getElementById('bulk-status');
  var failed = [];
  for (var i = 0; i < names.length; i++) {
    if (pf) pf.style.width = ((i + 1) / names.length * 100) + '%';
    if (ps) ps.innerHTML = '<span class="spinner"></span> ' + (i + 1) + '/' + names.length + ' — ' + esc(names[i]);
    try { await fetchPost(EP.VM_ACTION(names[i], action), {}); } catch (e) { failed.push(names[i] + ': ' + e.message); }
  }
  var okCount = names.length - failed.length;
  if (ps) {
    ps.innerHTML = '&#9989; ' + okCount + '/' + names.length + ' OK' +
      (failed.length ? '<br><span class="color-red">&#10060; ' + failed.length + ' failed</span><div class="text-xs color-muted" style="max-height:120px;overflow:auto">' + failed.map(esc).join('<br>') + '</div>' : '');
  }
  if (failed.length) toast(failed.length + ' / ' + names.length + ' ' + action + ' failed', false);
  addEvt('Bulk ' + action + ': ' + okCount + '/' + names.length + ' OK');
  setTimeout(function() { closeModal(); loadAll(); }, 2000);
}

async function bulkSnapshot() {
  closeModal();
  var snapName = document.getElementById('bulk-snap-name')?.value || 'snap-' + Date.now();
  var names = Array.from(checkedVms).map(function(idx) { return vmList[idx] ? vmList[idx].name : null; }).filter(Boolean);
  showModal('<h2>' + _L('일괄 스냅샷', 'Bulk Snapshot') + '</h2><div class="prog-bar"><div class="prog-fill" id="bulk-prog" class="w-pct-0"></div></div><div id="bulk-status" class="prog-status"><span class="spinner"></span> 0/' + names.length + '</div>');
  var pf = document.getElementById('bulk-prog');
  var ps = document.getElementById('bulk-status');
  for (var i = 0; i < names.length; i++) {
    if (pf) pf.style.width = ((i + 1) / names.length * 100) + '%';
    if (ps) ps.innerHTML = '<span class="spinner"></span> ' + (i + 1) + '/' + names.length + ' — ' + esc(names[i]);
    try { await fetchPost(EP.VM_SNAPSHOT_CREATE(names[i]), { snap_name: snapName }); } catch (e) {  }
  }
  if (ps) ps.innerHTML = '&#9989; ' + _L('완료', 'Done') + ': ' + names.length + ' snapshots';
  addEvt('Bulk snapshot: ' + snapName + ' → ' + names.join(', '));
  setTimeout(function() { closeModal(); loadAll(); }, 2000);
}




async function showVmFailureDetail(statusEl, progEl, vmName, actionLabel) {
  if (progEl) { progEl.style.width = '100%'; progEl.style.background = 'var(--red)'; }

  if (typeof addNotification === 'function') {
    addNotification('error',
      (actionLabel || 'VM action') + ' failed: ' + vmName,
      _L('30초 내 상태 전이 미확인 — audit DB 확인 필요', 'State change not confirmed within 30s — check audit DB'));
  }
  if (typeof addEvt === 'function') {
    addEvt('FAIL ' + (actionLabel || 'action') + ' ' + vmName + ' — state change timeout');
  }

  var safeName = escapeHtml(vmName);
  var titleHtml = '&#10060; ' + _L('상태 변경 실패', 'State change failed') + ': ' + safeName;
  var subHtml = _L('백엔드 워커가 30초 내 상태 전이를 완료하지 못했습니다.', 'Backend worker did not complete the state transition within 30s.');
  var loadingHtml = '<span class="spinner"></span> ' + _L('실패 사유 조회 중...', 'Loading failure reason...');
  var closeBtnHtml = '<button class="btn" onclick="closeModal();loadAll()">' + t('btn.close') + '</button>';
  var html = titleHtml
    + '<div class="text-xs color-muted mt-8">' + subHtml + '</div>'
    + '<div id="pwr-err-detail" class="text-xs mt-8" style="max-height:160px;overflow:auto;text-align:left;background:rgba(0,0,0,0.2);padding:8px;border-radius:4px">' + loadingHtml + '</div>'
    + '<div class="mt-12">' + closeBtnHtml + '</div>';
  if (statusEl) statusEl.innerHTML = html;

  try {
    var resp = await fetchGet('/api/v1/health/recent-errors?vm=' + encodeURIComponent(vmName) + '&limit=3');
    var errs = (resp && (resp.data || resp.errors)) || [];
    var detailEl = document.getElementById('pwr-err-detail');
    if (!detailEl) return;
    if (errs.length) {
      var rows = errs.map(function(e) {
        return '<div>&bull; <strong>' + escapeHtml(e.method || '?') + '</strong>: ' + escapeHtml(e.message || e.error || 'unknown') + '</div>';
      }).join('');
      detailEl.innerHTML = rows;
    } else {
      detailEl.innerHTML = '<em>' + _L('워커 실패 로그 없음 — journalctl -u purecvisorsd -f 로 실시간 확인', 'No worker failure log — try journalctl -u purecvisorsd -f') + '</em>';
    }
  } catch (lookupErr) {
    var detailEl2 = document.getElementById('pwr-err-detail');
    if (detailEl2) detailEl2.innerHTML = '<em>' + _L('상세 조회 실패', 'Detail lookup failed') + ': ' + escapeHtml(lookupErr.message || 'unknown') + '</em>';
  }
}






async function vmPower(a) {
  const v = vmList[selectedVmIndex]; if (!v) return;
  var actionLabels = {
    start: { icon: '&#9654;', label: _L('시작', 'Start'), past: _L('시작됨', 'Started'), color: 'var(--green)' },
    stop: { icon: '&#9632;', label: _L('중지', 'Stop'), past: _L('중지됨', 'Stopped'), color: 'var(--red)' },
    suspend: { icon: '&#10074;&#10074;', label: _L('일시정지', 'Pause'), past: _L('일시정지됨', 'Paused'), color: 'var(--yellow)' },
    resume: { icon: '&#9654;&#9654;', label: _L('재개', 'Resume'), past: _L('재개됨', 'Resumed'), color: 'var(--green)' }
  };
  var al = actionLabels[a] || { icon: '&#9881;', label: a, past: a, color: 'var(--accent)' };

  showModal('<div class="text-center p-20">'
    + '<div style="font-size:48px;margin-bottom:12px">' + al.icon + '</div>'
    + '<h2 class="mb-8">' + escapeHtml(v.name) + '</h2>'
    + '<div class="prog-bar"><div class="prog-fill" id="pwr-p" style="width:30%;background:' + al.color + '"></div></div>'
    + '<div class="prog-status" id="pwr-s" class="mt-10"><span class="spinner"></span> ' + al.label + ' ' + _L('진행 중...', 'in progress...') + '</div>'
    + '</div>');
  try {
    var pf = document.getElementById('pwr-p'), ps = document.getElementById('pwr-s');
    var r = await fetchPost(EP.VM_ACTION(v.name, a), {});

    if (r && r.error) {
      if (pf) { pf.style.width = '100%'; pf.style.background = 'var(--red)'; }
      if (ps) ps.innerHTML = '&#10060; ' + escapeHtml(r.error.message || 'Failed');
      setTimeout(closeModal, 3000);
      return;
    }
    if (pf) pf.style.width = '60%';
    if (ps) ps.innerHTML = '<span class="spinner"></span> ' + _L('상태 확인 중...', 'Verifying state...');

    var expectedState = (a === 'start' || a === 'resume') ? 'running' : (a === 'suspend') ? 'paused' : 'shutoff';
    var verified = false;
    var maxPolls = 15;
    for (var pi = 0; pi < maxPolls; pi++) {
      await new Promise(function(res) { setTimeout(res, 2000); });
      if (pf) pf.style.width = Math.min(95, 65 + pi * 2) + '%';
      try {
        var vl = await fetchGet(EP.VM_LIST());
        var list = unwrapList(vl);
        var found = list.find(function(x) { return x.name === v.name; });
        if (found && found.state === expectedState) { verified = true; break; }
      } catch(e2) {}
    }
    if (verified) {
      if (pf) { pf.style.width = '100%'; pf.style.background = 'var(--green)'; }
      if (ps) ps.innerHTML = '&#9989; ' + al.past;
      addEvt(v.name + ' ' + al.past);
      setTimeout(function() { closeModal(); loadAll(); }, 2000);
    } else {


      await showVmFailureDetail(ps, pf, v.name, al.label);
      loadAll();
    }
  } catch (e) {
    var pf2 = document.getElementById('pwr-p'), ps2 = document.getElementById('pwr-s');
    if (pf2) { pf2.style.width = '100%'; pf2.style.background = 'var(--red)'; }
    var errMsg = e.name === 'AbortError' ? _L('타임아웃 (10초)', 'timeout (10s)') : escapeHtml(e.message);

    var btnHtml = '<div class="mt-12"><button class="btn" onclick="closeModal()">' + t('btn.close') + '</button></div>';
    if (ps2) ps2.innerHTML = '&#10060; ' + _L('실패', 'Failed') + ': ' + errMsg + btnHtml;
  }
}


async function vmDel() {
  const v = vmList[selectedVmIndex]; if (!v) return;
  if (typeof destroyConfirm !== 'function') {

    if (!confirm(_L('VM 삭제: ', 'Delete VM: ') + v.name + '?')) return;
    return doVmDel(v.name);
  }
  destroyConfirm({
    title: t('vm.delete'),
    name: v.name,
    warning: t('vm.delete.confirm') + ' — ' +
             _L('ZFS 볼륨과 디스크 이미지까지 영구 삭제됩니다. 이 작업은 되돌릴 수 없습니다.',
                'ZFS volume and disk image will be permanently destroyed. This cannot be undone.'),
    onConfirm: function() { doVmDel(v.name); }
  });
}



async function doVmDel(n) {
  showModal('<h2 class="color-red">&#9888; Deleting VM</h2><p><b class="color-accent">' + escapeHtml(n) + '</b></p><div class="prog-bar"><div class="prog-fill" id="dv-p" class="w-pct-10"></div></div><div class="prog-status" id="dv-s"><span class="spinner"></span>Sending delete request...</div>');
  const pf = document.getElementById('dv-p'), ps = document.getElementById('dv-s');
  var deleteError = null;
  try {
    if (pf) pf.style.width = '30%';
    const d = await fetchDelete(EP.VM_DETAIL(n)).catch(function(e) { return { error: { message: e && e.message || 'Network error' } }; });
    if (d && d.error) {
      deleteError = d.error.message || 'Failed';

      if (ps) ps.innerHTML = '<span class="spinner"></span>&#9888; ' + escapeHtml(deleteError) + _L(' — 서버 상태 확인 중...', ' — polling server state...');
    } else {
      if (ps) ps.innerHTML = '<span class="spinner"></span>Waiting for zvol cleanup...';
    }
    if (pf) pf.style.width = '50%';
    for (let i = 0; i < 10; i++) {
      await new Promise(r => setTimeout(r, 2000));
      if (pf) pf.style.width = Math.min(95, 55 + i * 4) + '%';
      if (ps && !deleteError) ps.innerHTML = '<span class="spinner"></span>Cleaning up (' + (i + 1) + '/10)...';
      try {
        const vl = await fetchGet(EP.VM_LIST());
        const vms = unwrapList(vl);
        if (!vms.find(x => x.name === n)) {

          if (pf) { pf.style.width = '100%'; pf.style.background = 'var(--green)'; }
          if (ps) ps.innerHTML = '&#9989; ' + t('vm.deleted');
          toast(t('vm.deleted'));
          addEvt(t('vm.deleted') + ': ' + n);
          setTimeout(function() { closeModal(); loadAll(); }, 1500);
          return;
        }
      } catch (e) { if(typeof _DEBUG !== 'undefined' && _DEBUG) console.warn('vl:', e.message); }
    }

    if (pf) { pf.style.width = '100%'; pf.style.background = 'var(--yellow)'; }
    if (deleteError) {
      if (ps) ps.innerHTML = '&#10060; ' + escapeHtml(deleteError);
      toast('&#10060; ' + escapeHtml(deleteError), false);
    } else {
      if (ps) ps.innerHTML = '&#9888; ' + _L('삭제가 오래 걸리고 있습니다', 'Delete taking longer than expected');
      toast(_L('삭제가 오래 걸리고 있습니다. 잠시 후 새로고침하세요.', 'Delete taking longer than expected — refresh shortly.'), false);
    }
    setTimeout(function() { closeModal(); loadAll(); }, 2000);
  } catch (e) {
    if (pf) { pf.style.width = '100%'; pf.style.background = 'var(--red)'; }
    if (ps) ps.innerHTML = '&#10060; ' + escapeHtml(e.message || 'Unknown error');
    toast(e.message || 'Unknown error', false);
    setTimeout(closeModal, 3000);
  }
}









function wizDefaults() {
  return {
    name: '',
    vcpu: 2,
    mem: 2048,
    disk: 20,
    iso: '',
    bridge: '',
    storage_type: 'auto',
    storage_pool: '',
    image_dir: '',
    storage_pools: [],
    storage_loaded: false
  };
}

var wizStep = 1, wizData = wizDefaults();

function showCreate() {
  wizStep = 1;
  wizData = wizDefaults();
  if (typeof markFormDirty === 'function') markFormDirty('vm-create');
  renderWiz();
}
function wizSave() {
  if (wizStep === 1) { wizData.name = document.getElementById('wn')?.value || wizData.name; }
  else if (wizStep === 2) { wizData.vcpu = +(document.getElementById('wc')?.value || wizData.vcpu); wizData.mem = +(document.getElementById('wm')?.value || wizData.mem); }
  else if (wizStep === 3) {
    wizData.disk = +(document.getElementById('wd')?.value || wizData.disk);
    wizData.iso = document.getElementById('wi')?.value || wizData.iso;
    wizData.bridge = document.getElementById('wb')?.value || wizData.bridge;
    wizData.storage_type = document.getElementById('wst')?.value || wizData.storage_type;
    wizData.storage_pool = (document.getElementById('wspool')?.value || wizData.storage_pool || '').trim();
    wizData.image_dir = (document.getElementById('widir')?.value || wizData.image_dir || '').trim();
  }
}
function wizGo(s) {
  wizSave();

  if (wizStep === 1 && s > 1) {
    const name = wizData.name.trim();
    if (!name) { toast(_L('VM 이름을 입력하세요', 'VM name is required'), false); return; }
    if (!/^[a-zA-Z0-9][a-zA-Z0-9_-]{0,63}$/.test(name)) {
      toast(_L('VM 이름: 1-64자, 영문/숫자/_- 만 허용 (첫 글자는 영문/숫자)', 'VM name: 1-64 chars, [a-zA-Z0-9_-], must start with alphanumeric'), false);
      return;
    }
  }
  wizStep = s; renderWiz();
}

function renderWiz() {
  let h = `<h2>${t('vm.new')}</h2><div class="wizard-steps"><div class="step ${wizStep >= 1 ? 'active' : ''}${wizStep > 1 ? ' done' : ''}">1. Basic</div><div class="step ${wizStep >= 2 ? 'active' : ''}${wizStep > 2 ? ' done' : ''}">2. Resources</div><div class="step ${wizStep >= 3 ? 'active' : ''}">3. Storage &amp; Network</div></div>`;
  if (wizStep === 1) h += `<div class="fr"><label>VM Name</label><input id="wn" value="${escapeHtml(wizData.name)}" placeholder="my-vm"></div><div class="text-right mt-12"><button class="btn" onclick="wizGo(2)">${t('btn.next')} &rarr;</button></div>`;
  else if (wizStep === 2) h += `<div class="fr"><label>vCPU</label><input id="wc" type="number" value="${escapeHtml(String(wizData.vcpu))}"></div><div class="fr"><label>Memory MB</label><input id="wm" type="number" value="${escapeHtml(String(wizData.mem))}"></div><div class="text-right mt-12"><button class="tb" onclick="wizGo(1)">&larr; ${t('btn.prev')}</button> <button class="btn" onclick="wizGo(3)">${t('btn.next')} &rarr;</button></div>`;
  else {
    const stSel = wizData.storage_type || 'auto';
    const poolVal = wizData.storage_pool || 'pcvpool/vms';
    const imageDirVal = wizData.image_dir || '/var/lib/libvirt/images';
    const isFileStorage = stSel === 'qcow2' || stSel === 'raw';
    const pools = Array.isArray(wizData.storage_pools) ? wizData.storage_pools : [];
    const poolOptions = pools.map(function(p) {
      return '<option value="' + escapeHtml(p) + '"' + (p === poolVal ? ' selected' : '') + '>' + escapeHtml(p) + '</option>';
    }).join('');
    h += `<div class="fr"><label>Disk GB</label><input id="wd" type="number" value="${escapeHtml(String(wizData.disk))}"></div>`
      + `<div class="fr"><label>${_L('스토리지 타입', 'Storage Type')}</label>`
      + `<select id="wst" onchange="wizStorageChanged(true)">`
      + `<option value="auto"${stSel === 'auto' ? ' selected' : ''}>${_L('자동 (서버 감지)', 'Auto (server detected)')}</option>`
      + `<option value="zvol"${stSel === 'zvol' ? ' selected' : ''}>ZFS zvol — ${_L('블록 디바이스, 고성능', 'Block device, high performance')}</option>`
      + `<option value="qcow2"${stSel === 'qcow2' ? ' selected' : ''}>qcow2 — ${_L('파일 기반, 스냅샷/씬 프로비저닝', 'File based, snapshot/thin provisioning')}</option>`
      + `<option value="raw"${stSel === 'raw' ? ' selected' : ''}>raw — ${_L('파일 기반, 최대 I/O 성능', 'File based, maximum I/O performance')}</option>`
      + `</select></div>`
      + (isFileStorage
        ? `<div class="fr"><label>${_L('저장 위치', 'Storage Location')}</label><input id="widir" value="${escapeHtml(imageDirVal)}" placeholder="/var/lib/libvirt/images" oninput="wizStorageChanged(false)"></div>`
        : `<div class="fr"><label>${_L('저장 위치', 'Storage Location')}</label><div class="flex gap-6 flex-1"><input id="wspool" value="${escapeHtml(poolVal)}" placeholder="pcvpool/vms" class="flex-1" oninput="wizStorageChanged(false)">`
          + `<select id="wspick" onchange="wizPickStoragePool()" title="${escapeHtml(_L('사용 가능한 ZFS 풀', 'Available ZFS pools'))}"><option value="">${escapeHtml(_L('풀 선택', 'Pool'))}</option>${poolOptions}</select>`
          + `<button class="btn text-xs" onclick="wizLoadStorageTargets(true)">${_L('새로고침', 'Refresh')}</button></div></div>`)
      + (isFileStorage ? `<input id="wspool" type="hidden" value="${escapeHtml(poolVal)}">` : `<input id="widir" type="hidden" value="${escapeHtml(imageDirVal)}">`)
      + `<div class="stat-label mb-8" id="wstorage-preview">${escapeHtml(wizStoragePreview())}</div>`
      + `<div class="fr"><label>ISO Image</label><div class="flex gap-6"><input id="wi" value="${escapeHtml(wizData.iso)}" placeholder="ISO path..." class="flex-1"><button class="btn" onclick="browseISO()">Browse</button></div></div>`
      + `<div class="fr"><label>Network</label><div class="flex gap-6 flex-1"><select id="wb"><option value="${escapeHtml(wizData.bridge)}">${t('loading')}</option></select><button class="btn text-xs" onclick="wizLoadNets()">Refresh</button></div></div>`
      + `<div class="text-right mt-12"><button class="tb" onclick="wizGo(2)">&larr; ${t('btn.prev')}</button> <button class="btn btn-g" onclick="doCreate()">${t('vm.create')}</button></div>`;
  }
  showModal(h, { replace: true });
  if (wizStep === 3) {
    setTimeout(wizLoadNets, 80);
    setTimeout(function() { wizLoadStorageTargets(false); }, 80);
  }
}

function wizStoragePreview() {
  const st = wizData.storage_type || 'auto';
  const name = (wizData.name || '<vm-name>').trim() || '<vm-name>';
  const pool = wizData.storage_pool || 'pcvpool/vms';
  const imageDir = wizData.image_dir || '/var/lib/libvirt/images';
  if (st === 'qcow2') return imageDir + '/' + name + '.qcow2';
  if (st === 'raw') return imageDir + '/' + name + '.img';
  if (st === 'zvol') return '/dev/zvol/' + pool + '/' + name;
  return _L('자동: ZFS 가능 시 ', 'Auto: ZFS when available ') + '/dev/zvol/' + pool + '/' + name
    + _L(', 아니면 ', ', otherwise ') + imageDir + '/' + name + '.qcow2';
}

function wizStorageChanged(rerender) {
  wizSave();
  if (rerender) { renderWiz(); return; }
  const el = document.getElementById('wstorage-preview');
  if (el) el.textContent = wizStoragePreview();
}

function wizPickStoragePool() {
  const sel = document.getElementById('wspick');
  const inp = document.getElementById('wspool');
  if (sel && inp && sel.value) {
    inp.value = sel.value;
    wizData.storage_pool = sel.value;
  }
  wizStorageChanged(false);
}

async function wizLoadStorageTargets(force) {
  if (wizStep === 3) wizSave();
  if (wizData.storage_loaded && !force) return;
  try {
    const cfg = await fetchGet(EP.CONFIG_DAEMON());
    const data = unwrapData(cfg) || {};
    if (cfg && !cfg.error && data.storage) {
      if (!wizData.storage_pool && data.storage.zvol_pool) wizData.storage_pool = data.storage.zvol_pool;
      if (!wizData.image_dir && data.storage.image_dir) wizData.image_dir = data.storage.image_dir;
    }
  } catch (e) {}
  try {
    const poolsResp = await fetchGet(EP.STORAGE_POOLS());
    const pools = unwrapList(poolsResp).map(function(p) { return p && p.name; }).filter(Boolean);
    if (poolsResp && !poolsResp.error) wizData.storage_pools = pools;
  } catch (e) {}
  wizData.storage_loaded = true;
  const poolInput = document.getElementById('wspool');
  const imageInput = document.getElementById('widir');
  if (poolInput && wizData.storage_pool) poolInput.value = wizData.storage_pool;
  if (imageInput && wizData.image_dir) imageInput.value = wizData.image_dir;
  const poolSel = document.getElementById('wspick');
  if (poolSel) {
    const cur = wizData.storage_pool || poolInput?.value || '';
    poolSel.innerHTML = '<option value="">' + escapeHtml(_L('풀 선택', 'Pool')) + '</option>'
      + wizData.storage_pools.map(function(p) {
        return '<option value="' + escapeHtml(p) + '"' + (p === cur ? ' selected' : '') + '>' + escapeHtml(p) + '</option>';
      }).join('');
  }
  wizStorageChanged(false);
}



async function wizLoadNets() {
  const sel = document.getElementById('wb'); if (!sel) return;
  try {
    const r = await fetchGet(EP.NET_LIST());
    const nl = unwrapList(r);
    const cur = wizData.bridge || '';
    let h = '';
    nl.forEach(n => {
      const name = n.name || ''; if (!name) return;
      const mode = n.mode || ''; const state = n.state || ''; const ip = n.ip_cidr || '';
      const info = [mode, ip, state].filter(Boolean).join(' | ');
      h += '<option value="' + escapeHtml(name) + '"' + (name === cur ? ' selected' : '') + '>' +
           escapeHtml(name) + (info ? ' (' + escapeHtml(info) + ')' : '') + '</option>';
    });
    if (h === '') {
      h = '<option value="" disabled selected>' +
          escapeHtml(_L('네트워크 없음 — 먼저 브릿지를 생성하세요', 'No networks — create a bridge first')) +
          '</option>';
      toast(_L('네트워크가 없습니다. Network 탭에서 브릿지를 먼저 생성하세요.',
               'No networks found. Create a bridge in the Network tab first.'), false);
    }
    sel.innerHTML = h;
  } catch (e) {
    sel.innerHTML = '<option value="" disabled selected>' +
                    escapeHtml(_L('네트워크 조회 실패', 'Network list failed')) + '</option>';
    toast(_L('네트워크 목록 조회 실패: ', 'Failed to load network list: ') + (e.message || ''), false);
  }
}

async function browseISO() {
  closeISOBrowser();
  const ov = document.createElement('div'); ov.id = 'iso-overlay';
  ov.style.cssText = 'position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,.6);display:flex;align-items:center;justify-content:center;z-index:250';
  let h = '<div style="background:var(--bg-panel);backdrop-filter:blur(16px);border:1px solid var(--accent);border-radius:8px;padding:20px;min-width:500px;max-width:90vw;max-height:85vh;overflow-y:auto;box-shadow:0 0 30px var(--neon-glow)">';
  h += '<h2 style="font-family:var(--font-display);font-size:16px;color:var(--accent)">&#128191; ' + t('iso.browser_title') + '</h2>';
  h += '<div class="stat-label mb-10">' + t('iso.browser_desc') + '</div>';
  h += '<div id="iso-modal-list" style="max-height:380px;overflow-y:auto;border:1px solid var(--border);border-radius:6px;background:var(--bg)"><div class="p-12 color-muted text-xs"><span class="spinner"></span> ' + t('loading') + '</div></div>';
  h += '<div class="flex gap-8 items-center mt-10">';
  h += '<input id="iso-manual-path" placeholder="Direct path..." style="flex:1;padding:8px 10px;background:var(--bg3);border:1px solid var(--border);border-radius:6px;color:var(--fg);font-size:12px" onkeydown="if(event.key===\'Enter\')isoSelectManual()">';
  h += '<button class="btn btn-g" onclick="isoSelectManual()">' + t('btn.confirm') + '</button>';
  h += '<button class="btn btn-r" onclick="closeISOBrowser()">' + t('btn.cancel') + '</button>';
  h += '</div></div>';
  ov.innerHTML = h;
  ov.addEventListener('click', e => { if (e.target === ov) closeISOBrowser(); });
  document.body.appendChild(ov);
  try {
    const r = await fetchGet(EP.ISO_LIST()); const il = unwrapList(r);
    const el = document.getElementById('iso-modal-list'); if (!el) return;
    let lh = '';
    const dirs = {}; il.forEach(iso => { const d = iso.dir || '/pcvpool/iso'; if (!dirs[d]) dirs[d] = []; dirs[d].push(iso); });
    if (il.length === 0) { lh = '<div class="text-center" style="padding:16px;color:var(--fg2)"><div style="font-size:24px;margin-bottom:8px">&#128194;</div><div>' + t('iso.not_found') + '</div></div>'; }
    else { Object.entries(dirs).forEach(([dir, files]) => {
      lh += '<div style="padding:6px 10px;font-size:10px;color:var(--accent);border-bottom:1px solid var(--border);background:rgba(0,240,255,.03);font-weight:600">&#128194; ' + dir + ' (' + files.length + ' files)</div>';
      files.forEach(iso => { const p = iso.path || iso.name; const fn = (iso.name || p).replace(/^.*\//, '');
        const sz = iso.size_mb ? (iso.size_mb >= 1024 ? (iso.size_mb / 1024).toFixed(1) + 'GB' : iso.size_mb + 'MB') : '';
        const ext = fn.split('.').pop().toUpperCase(); const icon = ext === 'IMG' ? '&#128190;' : '&#128191;';
        lh += '<div onclick="isoSelect(\'' + p.replace(/'/g, "\\'") + '\')" style="padding:8px 12px;cursor:pointer;font-size:12px;display:flex;align-items:center;gap:8px;border-bottom:1px solid var(--border);transition:background .1s" onmouseover="this.style.background=\'var(--bg3)\'" onmouseout="this.style.background=\'\'">';
        lh += '<span class="text-lg">' + icon + '</span><span style="flex:1;font-weight:500">' + fn + '</span>' + H.badge(ext, 'y') + '<span class="color-muted" style="font-size:11px;min-width:60px;text-align:right">' + sz + '</span></div>'; }); }); }
    el.innerHTML = lh;
  } catch (e) { const el = document.getElementById('iso-modal-list'); if (el) el.innerHTML = '<div class="p-12 color-red">&#10060; Error: ' + escapeHtml(e.message) + '</div>'; }
}

function isoSelect(path) { wizData.iso = path; closeISOBrowser(); renderWiz(); }
function isoSelectManual() { const v = document.getElementById('iso-manual-path')?.value; if (v) { wizData.iso = v; closeISOBrowser(); renderWiz(); } }
function closeISOBrowser() { const el = document.getElementById('iso-overlay'); if (el) el.remove(); }

async function doCreate() {
  wizSave(); const d = wizData;
  const name = (d.name || '').trim();
  if (!name) { toast(_L('VM 이름을 입력하세요', 'VM name is required'), false); return; }
  if (!/^[a-zA-Z0-9][a-zA-Z0-9_-]{0,63}$/.test(name)) {
    toast(_L('VM 이름: 1-64자, 영문/숫자/_- 만 허용', 'VM name: 1-64 chars, [a-zA-Z0-9_-]'), false); return;
  }

  if (!d.bridge) {
    toast(_L('네트워크 브릿지를 선택하세요', 'Select a network bridge'), false); return;
  }

  if (!(d.vcpu >= 1 && d.vcpu <= 256)) {
    toast(_L('vCPU 는 1~256 사이', 'vCPU must be between 1 and 256'), false); return;
  }
  if (!(d.mem >= 256 && d.mem <= 1048576)) {
    toast(_L('메모리는 256~1048576 MB 사이', 'Memory must be 256~1048576 MB'), false); return;
  }
  if (!(d.disk >= 1 && d.disk <= 65536)) {
    toast(_L('디스크는 1~65536 GB 사이', 'Disk must be 1~65536 GB'), false); return;
  }


  if (typeof clearFormDirty === 'function') clearFormDirty('vm-create');
  closeModal(true);
  toast('&#128187; ' + escapeHtml(name) + ' ' + _L('생성 시작...', 'Creating...'), 's');

  try {

    const body = { name: name, vcpu: d.vcpu, memory_mb: d.mem, disk_size_gb: d.disk, network_bridge: d.bridge };
    if (d.storage_type && d.storage_type !== 'auto') body.storage_type = d.storage_type;
    if (d.storage_pool) body.storage_pool = d.storage_pool;
    if (d.image_dir) body.image_dir = d.image_dir;
    if (d.iso) body.iso_path = d.iso;
    const r = await fetchPost(EP.VM_CREATE(), body);

    if (r && r.error) {
      toast('&#10060; ' + _L('VM 생성 실패', 'VM creation failed') + ': ' + (r.error.message || JSON.stringify(r.error)), false);
      return;
    }

    toast('&#9989; ' + t('vm.created') + ': ' + escapeHtml(name), 's');
    const storageLabel = (d.storage_type && d.storage_type !== 'auto') ? d.storage_type : 'auto';
    addEvt(t('vm.created') + ': ' + name + ' (' + d.vcpu + 'vCPU, ' + d.mem + 'MB, ' + d.disk + 'GB, ' + storageLabel + ')');


    if (typeof invalidateCache === 'function') invalidateCache('vm.list');
    var attempts = 0;
    var errStreak = 0;
    var poll = null;
    var hardTimer = null;
    var stopPoll = function() {
      if (poll) { clearInterval(poll); poll = null; }
      if (hardTimer) { clearTimeout(hardTimer); hardTimer = null; }
    };
    hardTimer = setTimeout(function() {
      stopPoll();
      loadAll();
    }, 25000);
    poll = setInterval(async function() {
      attempts++;
      try {
        var fresh = await fetchGet(EP.VM_LIST());
        errStreak = 0;
        var list = Array.isArray(fresh) ? fresh : (fresh && fresh.data) || [];
        if (list.find(function(v){ return v.name === name; })) {
          stopPoll();
          window.vmList = list; vmList = list;
          render();
        } else if (attempts >= 20) {
          stopPoll();
          loadAll();
          toast(_L('VM 생성에 시간이 걸리고 있습니다. 잠시 후 새로고침하세요.', 'VM creation taking longer than expected'), false);
        }
      } catch (e) {
        errStreak++;
        if (errStreak >= 5 || attempts >= 20) {
          stopPoll();
          loadAll();
        }
      }
    }, 1000);
  } catch (e) {
    toast('&#10060; ' + _L('VM 생성 실패', 'VM creation failed') + ': ' + (e.message || ''), false);
  }
}


function showSettings() {
  const v = vmList[selectedVmIndex]; if (!v) return;
  showModal(`<h2>${t('vm.settings')}: ${escapeHtml(v.name)}</h2><div class="split"><div class="left"><div class="hw-list"><div class="hw-item active" onclick="setHw(this,'identity')"><span class="hw-icon">&#9998;</span><span class="hw-label">Identity</span><span class="hw-val">${escapeHtml(v.name)}</span></div><div class="hw-item" onclick="setHw(this,'cpu')"><span class="hw-icon">&#9881;</span><span class="hw-label">CPU</span><span class="hw-val">${escapeHtml(String(v.vcpu || '-'))} vCPU</span></div><div class="hw-item" onclick="setHw(this,'mem')"><span class="hw-icon">&#128204;</span><span class="hw-label">Memory</span><span class="hw-val">${escapeHtml(String(v.memory_mb || '-'))} MB</span></div><div class="hw-item" onclick="setHw(this,'disk')"><span class="hw-icon">&#128190;</span><span class="hw-label">Disk Resize</span><span class="hw-val">Storage</span></div><div class="hw-item" onclick="setHw(this,'cpupin')"><span class="hw-icon">&#128204;</span><span class="hw-label">CPU Pinning</span><span class="hw-val">vCPU Pin</span></div><div class="hw-item" onclick="setHw(this,'bw')"><span class="hw-icon">&#128246;</span><span class="hw-label">Bandwidth</span><span class="hw-val">QoS</span></div><div class="hw-item" onclick="setHw(this,'cdrom')"><span class="hw-icon">&#128191;</span><span class="hw-label">CD/DVD (SATA)</span><span class="hw-val">ISO</span></div><div class="hw-item" onclick="setHw(this,'nic')"><span class="hw-icon">&#127760;</span><span class="hw-label">Network</span><span class="hw-val">Bridge</span></div><div class="hw-item" onclick="setHw(this,'autoprotect')"><span class="hw-icon">&#128737;</span><span class="hw-label">AutoProtect</span><span class="hw-val">Backup</span></div></div></div><div class="right"><div id="hw-edit">${hwIdentity()}</div></div></div><div class="text-right mt-12"><button class="btn btn-r" onclick="closeModal()">${t('btn.close')}</button></div>`);
}

function setHw(el, t2) { document.querySelectorAll('.hw-item').forEach(e => e.classList.remove('active')); el.classList.add('active'); document.getElementById('hw-edit').innerHTML = ({ identity: hwIdentity, cpu: hwCpu, mem: hwMem, disk: hwDisk, cpupin: hwCpuPin, bw: hwBandwidth, cdrom: hwCd, nic: hwNic, autoprotect: hwAP })[t2](); }

function _vmRenameBlocked(v) {
  const st = String((v && (v.state || v.status)) || '').toLowerCase();
  return st === 'running' || st === 'paused';
}
function hwIdentity() {
  const v = vmList[selectedVmIndex];
  const blocked = _vmRenameBlocked(v);
  return `<h4>Identity</h4><div class="fr"><label>${_L('현재 이름', 'Current')}</label><input value="${escapeAttr(v?.name || '')}" disabled style="opacity:.65"></div><div class="fr"><label>${_L('새 이름', 'New')}</label><input id="rn-new" value="${escapeAttr(v?.name || '')}" maxlength="64" ${blocked ? 'disabled' : ''}></div>${blocked ? '<p class="color-yellow text-xs">' + _L('정지 상태에서만 변경할 수 있습니다.', 'Rename requires the VM to be shut off.') + '</p>' : ''}<button class="btn btn-g" onclick="doVmRename()" ${blocked ? 'disabled' : ''}>${_L('이름 변경', 'Rename')}</button>`;
}
function hwCpu() { return `<h4>CPU</h4><div class="fr"><label>vCPU</label><input id="sc" type="number" value="${escapeHtml(String(vmList[selectedVmIndex]?.vcpu || 2))}"></div><button class="btn" onclick="doSet('vcpu')">${t('btn.apply')}</button>`; }
function hwMem() { return `<h4>Memory</h4><div class="fr"><label>MB</label><input id="sm" type="number" value="${escapeHtml(String(vmList[selectedVmIndex]?.memory_mb || 2048))}"></div><button class="btn" onclick="doSet('memory')">${t('btn.apply')}</button>`; }
function _currentCdromPath(v) {
  const p = String((v && (v.cdrom_path || v.iso_path)) || '').trim();
  return (!p || p === '(empty)' || p === 'N/A' || p === '-') ? '' : p;
}

function _setCdromInput(path) {
  const clean = String(path || '').trim();
  const input = document.getElementById('si');
  const current = document.getElementById('cdrom-current');
  if (input) input.value = clean;
  if (current) {
    current.innerHTML = clean
      ? '<span class="color-green">' + escapeHtml(clean) + '</span>'
      : '<span class="color-muted">' + _L('비어 있음', 'Empty') + '</span>';
  }
}

function hwCd() {
  const current = _currentCdromPath(vmList[selectedVmIndex]);
  return `<h4>CD/DVD</h4>`
    + `<div class="fr"><label>${_L('현재 ISO', 'Current ISO')}</label><div id="cdrom-current" class="flex-1">${current ? '<span class="color-green">' + escapeHtml(current) + '</span>' : '<span class="color-muted">' + _L('비어 있음', 'Empty') + '</span>'}</div></div>`
    + `<div class="fr"><label>ISO</label><div class="flex gap-6 flex-1"><input id="si" placeholder="ISO path..." class="flex-1" value="${escapeAttr(current)}"><button class="btn" onclick="browseISOForMount()">&#128194; Browse</button></div></div>`
    + `<button class="btn" onclick="doMnt()">Mount</button> <button class="btn btn-r" onclick="doEjt()">Eject</button>`;
}

function selectISOForMount(path) {
  closeModal();
  setTimeout(function() { _setCdromInput(path); }, 0);
}

async function browseISOForMount() {
  try { const r = await fetchGet(EP.ISO_LIST()); const il = unwrapList(r);
    let h = '<h2>&#128191; ' + t('iso.browser_title') + '</h2><div style="max-height:350px;overflow-y:auto;border:1px solid var(--border);border-radius:6px;background:var(--bg)">';
    if (il.length === 0) { h += '<div class="text-center color-muted" style="padding:16px">' + t('iso.not_found') + '</div>'; }
    else { const dirs = {}; il.forEach(iso => { const d = iso.dir || '/pcvpool/iso'; if (!dirs[d]) dirs[d] = []; dirs[d].push(iso); });
      Object.entries(dirs).forEach(([dir, files]) => {
        h += '<div style="padding:6px 10px;font-size:10px;color:var(--accent);border-bottom:1px solid var(--border);font-weight:600">&#128194; ' + escapeHtml(dir) + '</div>';
        files.forEach(iso => { const p = String(iso.path || iso.name || ''); const fn = (iso.name || p).replace(/^.*\//, ''); const sz = iso.size_human || '';
          h += '<div onclick="selectISOForMount(' + escapeAttr(JSON.stringify(p)) + ')" style="padding:7px 12px;cursor:pointer;font-size:12px;display:flex;align-items:center;gap:6px;border-bottom:1px solid var(--border)" onmouseover="this.style.background=\'var(--bg3)\'" onmouseout="this.style.background=\'\'">';
          h += '<span class="color-accent">&#128191;</span><span class="flex-1">' + escapeHtml(fn) + '</span><span class="color-muted text-xs">' + escapeHtml(sz) + '</span></div>'; }); }); }
    h += '</div><div class="text-right mt-10"><button class="btn btn-r" onclick="closeModal()">' + t('btn.cancel') + '</button></div>';
    showModal(h); } catch (e) { toast('ISO list error: ' + e.message, false); }
}

function hwNic() { const v = vmList[selectedVmIndex]; const html = `<h4>Network</h4><div id="nic-list">${t('loading')}</div><div class="mt-8"><div class="fr"><label>Bridge</label><input id="nic-br" value="pcvbr0"><button class="btn btn-g" onclick="nicAdd()">+ Add NIC</button></div></div>`; setTimeout(() => loadNics(v?.name), 50); return html; }
async function loadNics(n) { if (!n) return; try { const r = await fetchGet(EP.VM_NICS(n)); const l = unwrapList(r); let h = '<table><thead><tr><th>MAC</th><th>Bridge</th><th>Model</th><th>IP</th><th>DNS</th><th></th></tr></thead><tbody>'; l.forEach(c => { const dns = c.dns === 'off' ? 'OFF' : (c.dns || '-'); h += `<tr><td>${escapeHtml(c.mac || '-')}</td><td>${escapeHtml(c.bridge || c.source || '-')}</td><td>${escapeHtml(c.model || 'virtio')}</td><td>${escapeHtml(c.ip || '-')}</td><td>${escapeHtml(dns)}</td><td><button class="btn btn-r text-9" onclick="nicDel('${escapeAttr(n)}','${escapeAttr(c.mac)}')">${t('btn.delete')}</button></td></tr>`; }); h += '</tbody></table>'; const el = document.getElementById('nic-list'); if (el) el.innerHTML = l.length ? h : '<p class="color-muted">No NICs</p>'; } catch (e) { if(_DEBUG) console.warn('loadNics:', e.message); } }
async function nicAdd() { const v = vmList[selectedVmIndex]; if (!v) return; try { await fetchPost(EP.VM_NICS(v.name), { bridge: document.getElementById('nic-br')?.value || 'pcvbr0' }); toast(t('nic.added')); loadNics(v.name); } catch (e) { toast(e.message, false); } }
async function nicDel(n, mac) { if (!await customConfirm(t('btn.delete'), 'NIC ' + mac + '?')) return; try { await fetchDelete(EP.VM_NIC_DETACH(n, mac)); toast(t('nic.removed')); loadNics(n); } catch (e) { toast(e.message, false); } }

function hwAP() { const html = `<h4>AutoProtect (Backup Policy)</h4><div id="bp-list">${t('loading')}</div><div class="mt-8"><div class="fr"><label>VM</label><input id="bp-vm" value="${escapeHtml(vmList[selectedVmIndex]?.name || '*')}"></div><div class="fr"><label>Interval (h)</label><input id="bp-int" type="number" value="24"></div><div class="fr"><label>Retention</label><input id="bp-ret" type="number" value="7"></div><button class="btn btn-g" onclick="bpSet()">Set Policy</button></div>`; setTimeout(loadBP, 50); return html; }
async function loadBP() { try { const r = await fetchGet(EP.BACKUP_POLICIES()); const l = unwrapList(r); let h = '<table><thead><tr><th>VM</th><th>Interval</th><th>Retention</th></tr></thead><tbody>'; if (Array.isArray(l)) l.forEach(p => { h += `<tr><td>${escapeHtml(p.vm_name || p.name || '-')}</td><td>${escapeHtml(String(p.interval_hours || p.interval || '-'))}h</td><td>${escapeHtml(String(p.retention || '-'))}</td></tr>`; }); h += '</tbody></table>'; const el = document.getElementById('bp-list'); if (el) el.innerHTML = l.length ? h : '<p class="color-muted">No policies</p>'; } catch (e) { if(_DEBUG) console.warn('loadBP:', e.message); } }
async function bpSet() { try { await fetchPost(EP.BACKUP_POLICIES(), { vm_name: document.getElementById('bp-vm')?.value || '*', interval: +(document.getElementById('bp-int')?.value || 24), retention: +(document.getElementById('bp-ret')?.value || 7) }); toast(t('backup.policy_set')); loadBP(); } catch (e) { toast(e.message, false); } }

function showRenameVm() {
  const v = vmList[selectedVmIndex]; if (!v) return;
  const blocked = _vmRenameBlocked(v);
  showModal('<h2>' + _L('VM 이름 변경', 'Rename VM') + ': ' + escapeHtml(v.name) + '</h2>'
    + '<div class="fr"><label>' + _L('현재 이름', 'Current') + '</label><input value="' + escapeAttr(v.name) + '" disabled style="opacity:.65"></div>'
    + '<div class="fr"><label>' + _L('새 이름', 'New') + '</label><input id="rn-new" value="' + escapeAttr(v.name) + '" maxlength="64" ' + (blocked ? 'disabled' : '') + '></div>'
    + (blocked ? '<p class="color-yellow text-xs">' + _L('정지 상태에서만 변경할 수 있습니다.', 'Rename requires the VM to be shut off.') + '</p>' : '')
    + '<div id="rn-status" class="mt-8 text-xs color-muted"></div>'
    + '<div class="text-right mt-12"><button class="btn btn-r" onclick="closeModal()">' + t('btn.cancel') + '</button> <button class="btn btn-g" onclick="doVmRename()" ' + (blocked ? 'disabled' : '') + '>' + _L('이름 변경', 'Rename') + '</button></div>');
}

async function doVmRename() {
  const v = vmList[selectedVmIndex]; if (!v) return;
  const statusEl = document.getElementById('rn-status');
  const next = String(document.getElementById('rn-new')?.value || '').trim();
  if (!/^[A-Za-z0-9_-]{1,64}$/.test(next)) {
    toast(_L('VM 이름은 영문/숫자/하이픈/언더스코어만 사용할 수 있습니다.', 'VM name allows only letters, numbers, dash, underscore.'), false);
    return;
  }
  if (next === v.name) {
    toast(_L('새 이름이 현재 이름과 같습니다.', 'New name is the same as the current name.'), false);
    return;
  }
  if (_vmRenameBlocked(v)) {
    toast(_L('VM을 종료한 뒤 다시 시도하세요.', 'Shut off the VM and retry.'), false);
    return;
  }
  try {
    if (statusEl) statusEl.innerHTML = '<span class="spinner"></span> ' + _L('변경 중...', 'Renaming...');
    const r = await fetchPut(EP.VM_RENAME(v.name), { new_name: next });
    if (r && r.error) {
      const msg = r.error.message || t('error');
      if (statusEl) statusEl.innerHTML = '&#10060; ' + escapeHtml(msg);
      toast(msg, false);
      return;
    }
    const d = unwrapData(r);
    toast(_L('VM 이름이 변경되었습니다.', 'VM renamed.'));
    addEvt('VM renamed: ' + v.name + ' -> ' + next);
    if (typeof invalidateCache === 'function') invalidateCache('vm.list');
    selectedVmIndex = -1;
    closeModal();
    await loadAll();
    if (d && d.new_name && typeof render === 'function') render();
  } catch (e) {
    if (statusEl) statusEl.innerHTML = '&#10060; ' + escapeHtml(e.message || 'Failed');
    toast(e.message, false);
  }
}

async function doSet(t2) {
  const v = vmList[selectedVmIndex];
  if (!v) return;
  const nextValue = t2 === 'vcpu'
    ? +document.getElementById('sc').value
    : +document.getElementById('sm').value;
  const b = t2 === 'vcpu' ? { vcpu_count: nextValue } : { memory_mb: nextValue };
  try {
    await fetchPut(EP.VM_ACTION(v.name, t2), b);
    if (t2 === 'vcpu') v.vcpu = nextValue;
    if (t2 === 'memory') v.memory_mb = nextValue;
    toast(t2 + ' updated');
    addEvt('VM Hotplug — ' + v.name + ' ' + t2 + ' updated');
    if (typeof invalidateCache === 'function') invalidateCache('vm.list');
    await loadAll();
  } catch (e) {
    toast(e.message, false);
  }
}
async function doMnt() {
  const v = vmList[selectedVmIndex]; if (!v) return;
  const isoPath = String(document.getElementById('si')?.value || '').trim();
  if (!isoPath) { toast(t('iso.path_required'), false); return; }
  try {
    const r = await fetchPost(EP.VM_ISO(v.name), { iso_path: isoPath });
    if (r && r.error) { toast(r.error.message || t('error'), false); return; }
    const d = unwrapData(r);
    const mountedPath = d && d.iso_path ? d.iso_path : isoPath;
    v.cdrom_path = mountedPath;
    _setCdromInput(mountedPath);
    if (typeof invalidateCache === 'function') invalidateCache('vm.list');
    toast(t('iso.mounted'));
  } catch (e) { toast(e.message, false); }
}
async function doEjt() {
  const v = vmList[selectedVmIndex]; if (!v) return;
  try {
    const r = await fetchDelete(EP.VM_ISO(v.name));
    if (r && r.error) { toast(r.error.message || t('error'), false); return; }
    v.cdrom_path = '(empty)';
    _setCdromInput('');
    if (typeof invalidateCache === 'function') invalidateCache('vm.list');
    toast(t('iso.ejected'));
  } catch (e) { toast(e.message, false); }
}


function showSnap() { currentTab = 'snapshots'; document.querySelectorAll('#ct button').forEach(b => { b.classList.remove('active'); if (b.dataset.t === 'snapshots') b.classList.add('active'); }); renderContent(); }


async function showNicMgr() { const v = vmList[selectedVmIndex]; if (!v) return; showModal(`<h2>NIC: ${escapeHtml(v.name)}</h2><div id="nic-mgr">${t('loading')}</div><div class="mt-10"><div class="fr"><label>Bridge</label><input id="nm-br" value="pcvbr0"><button class="btn btn-g" onclick="nmAdd()">+ Add</button></div></div><div class="text-right mt-12"><button class="btn btn-r" onclick="closeModal()">${t('btn.close')}</button></div>`);
  try { const r = await fetchGet(EP.VM_NICS(v.name)); const l = unwrapList(r); let h = '<table><thead><tr><th>MAC</th><th>Bridge</th><th>Model</th><th>IP</th><th>DNS</th><th></th></tr></thead><tbody>'; l.forEach(c => { const dns = c.dns === 'off' ? 'OFF' : (c.dns || '-'); h += `<tr><td>${escapeHtml(c.mac || '-')}</td><td>${escapeHtml(c.bridge || c.source || '-')}</td><td>${escapeHtml(c.model || 'virtio')}</td><td>${escapeHtml(c.ip || '-')}</td><td>${escapeHtml(dns)}</td><td><button class="btn btn-r text-9" onclick="nmDel('${escapeAttr(c.mac)}')">${t('btn.delete')}</button></td></tr>`; }); document.getElementById('nic-mgr').innerHTML = l.length ? h + '</tbody></table>' : '<p class="color-muted">No NICs</p>'; } catch (e) { document.getElementById('nic-mgr').innerHTML = t('error'); } }
async function nmAdd() { const v = vmList[selectedVmIndex]; if (!v) return; try { await fetchPost(EP.VM_NICS(v.name), { bridge: document.getElementById('nm-br')?.value || 'pcvbr0' }); toast(t('nic.added')); showNicMgr(); } catch (e) { toast(e.message, false); } }
async function nmDel(mac) { const v = vmList[selectedVmIndex]; if (!v || !await customConfirm(t('btn.delete'), mac + '?')) return; try { await fetchDelete(EP.VM_NIC_DETACH(v.name, mac)); toast(t('nic.removed')); showNicMgr(); } catch (e) { toast(e.message, false); } }


async function showVnc() { const v = vmList[selectedVmIndex]; if (!v) return; showModal(`<h2>VNC: ${escapeHtml(v.name)}</h2><div id="vnc-info">${t('loading')}</div><div class="text-right mt-12"><button class="btn btn-r" onclick="closeModal()">${t('btn.close')}</button></div>`);
  try { const r = await fetchGet(EP.VNC(v.name)); const d = unwrapData(r); const stBadge = v.state === 'running' ? H.badge('Available', 'g') : H.badge('VM stopped', 'r'); document.getElementById('vnc-info').innerHTML = H.card('', H.row('Address', escapeHtml(d.vnc_address || d.address || 'localhost')) + H.row('Port', escapeHtml(String(d.vnc_port || d.port || '-'))) + H.row('Status', stBadge)); } catch (e) { document.getElementById('vnc-info').innerHTML = H.card('', '<p class="color-muted">VNC info unavailable</p>'); } }


function _vmCloneStorageKind(v) {
  const st = String(v?.storage_type || '').toLowerCase();
  const fmt = String(v?.disk_format || '').toLowerCase();
  const path = String(v?.disk_path || '');
  const lowerPath = path.toLowerCase();

  if (st === 'zvol' || path.indexOf('/dev/zvol/') === 0) return 'zvol';
  if (st === 'qcow2' || st === 'raw' || fmt === 'qcow2' || fmt === 'raw' ||
      lowerPath.endsWith('.qcow2') || lowerPath.endsWith('.raw') ||
      lowerPath.endsWith('.img')) return 'file';
  return 'unknown';
}

function _vmCloneIsPoweredOn(v) {
  const state = String(v?.state || '').toLowerCase();
  return state === 'running' || state === 'paused' || state === 'blocked' ||
    state === 'pmsuspended' || state === 'shutdown';
}

function _vmCloneGuard(v, mode) {
  const kind = _vmCloneStorageKind(v);
  if (_vmCloneIsPoweredOn(v)) {
    return {
      ok: false,
      message: _L('Power on 상태에서는 clone을 사용할 수 없습니다. 원본 VM을 종료한 뒤 재시도하세요.', 'Clone is unavailable while the source VM is powered on. Shut it off and retry.')
    };
  }
  if (kind === 'file' && mode !== 'full') {
    return {
      ok: false,
      message: _L('qcow2/raw 파일 디스크는 Full clone만 지원합니다.', 'qcow2/raw file disks only support Full clone.')
    };
  }
  if (kind === 'file') {
    return {
      ok: true,
      message: _L('파일 디스크는 Full clone으로 실행됩니다.', 'File disk clone will run in Full mode.')
    };
  }
  if (kind === 'zvol') {
    return {
      ok: true,
      message: _L('ZFS zvol은 CoW 또는 Full clone을 사용할 수 있습니다.', 'ZFS zvol supports CoW or Full clone.')
    };
  }
  return {
    ok: true,
    message: _L('디스크 타입은 요청 시 백엔드에서 검증됩니다.', 'Disk type will be validated by the backend.')
  };
}

function _vmCloneModeHelp(kind) {
  const cowSuffix = kind === 'file'
    ? _L('파일 디스크에서는 사용할 수 없음', 'Unavailable for file disks')
    : _L('ZFS zvol 전용', 'ZFS zvol only');
  return '<div class="vm-clone-choice-grid">'
    + '<label class="vm-clone-choice" data-vm-clone-mode-choice="full">'
    + '<input type="radio" name="vm-clone-mode-choice" value="full">'
    + '<span class="vm-clone-choice-head"><span>Full</span><span class="badge b-g">' + _L('독립', 'Independent') + '</span></span>'
    + '<span class="vm-clone-choice-title">' + _L('디스크 전체 복제', 'Full disk copy') + '</span>'
    + '<span class="vm-clone-choice-copy">' + _L('원본 snapshot/origin에 의존하지 않습니다. 시간이 더 걸리고 용량을 더 사용합니다.', 'No source snapshot/origin dependency. Takes longer and uses more storage.') + '</span>'
    + '</label>'
    + '<label class="vm-clone-choice" data-vm-clone-mode-choice="cow">'
    + '<input type="radio" name="vm-clone-mode-choice" value="cow">'
    + '<span class="vm-clone-choice-head"><span>CoW</span><span class="badge b-y">' + escapeHtml(cowSuffix) + '</span></span>'
    + '<span class="vm-clone-choice-title">' + _L('빠른 snapshot 복제', 'Fast snapshot clone') + '</span>'
    + '<span class="vm-clone-choice-copy">' + _L('생성이 빠르고 공간 사용이 적습니다. 원본 snapshot/origin 의존성이 남습니다.', 'Fast and space-efficient. Keeps a source snapshot/origin dependency.') + '</span>'
    + '</label>'
    + '</div>';
}

function _vmCloneSafetyHelp() {
  return '<div class="vm-clone-choice-grid">'
    + '<label class="vm-clone-choice" data-vm-clone-safety-choice="guest-reset">'
    + '<input name="vm-clone-safety" value="guest-reset" type="radio" checked>'
    + '<span class="vm-clone-choice-head"><span>Guest reset</span><span class="badge b-y">libguestfs-tools</span></span>'
    + '<span class="vm-clone-choice-title">' + _L('일반 VM 복제', 'Normal VM clone') + '</span>'
    + '<span class="vm-clone-choice-copy">' + _L('운영 서버에 virt-sysprep, virt-customize, virt-filesystems, guestfish가 있어야 합니다. target guest identity를 새 VM 기준으로 재설정합니다.', 'Requires virt-sysprep, virt-customize, virt-filesystems, and guestfish on the host. Resets target guest identity for the new VM.') + '</span>'
    + '</label>'
    + '<label class="vm-clone-choice" data-vm-clone-safety-choice="template">'
    + '<input name="vm-clone-safety" value="template" type="radio">'
    + '<span class="vm-clone-choice-head"><span>Prepared template</span><span class="badge b-g">' + _L('도구 불필요', 'No tools') + '</span></span>'
    + '<span class="vm-clone-choice-title">' + _L('정리된 템플릿 전용', 'Prepared templates only') + '</span>'
    + '<span class="vm-clone-choice-copy">' + _L('이미 게스트 식별자를 정리한 VM에만 사용합니다. guest reset을 건너뛰므로 중복 식별자가 없다는 책임은 운영자에게 있습니다.', 'Use only when guest identities are already cleaned. Skips guest reset, so the operator is responsible for avoiding duplicate identities.') + '</span>'
    + '</label>'
    + '</div>';
}

function _vmCloneRefreshChoiceCards(groupName, dataAttr) {
  document.querySelectorAll('[' + dataAttr + ']').forEach(card => {
    const input = card.querySelector('input[name="' + groupName + '"]');
    if (!input) return;
    card.classList.toggle('active', input.checked);
    card.classList.toggle('disabled', input.disabled);
    card.setAttribute('aria-checked', input.checked ? 'true' : 'false');
    card.setAttribute('aria-disabled', input.disabled ? 'true' : 'false');
  });
}

function _vmCloneRefreshGuard(v) {
  const modeEl = document.getElementById('vm-clone-mode');
  const guardEl = document.getElementById('vm-clone-guard');
  const submitEl = document.getElementById('vm-clone-submit');

  const kind = _vmCloneStorageKind(v);
  const fullInput = document.querySelector('input[name="vm-clone-mode-choice"][value="full"]');
  const cowInput = document.querySelector('input[name="vm-clone-mode-choice"][value="cow"]');
  if (cowInput) cowInput.disabled = kind === 'file';
  if (kind === 'file' && cowInput && cowInput.checked && fullInput) fullInput.checked = true;
  const selectedMode = document.querySelector('input[name="vm-clone-mode-choice"]:checked')?.value ||
    (kind === 'zvol' ? 'cow' : 'full');
  if (modeEl) modeEl.value = selectedMode;
  _vmCloneRefreshChoiceCards('vm-clone-mode-choice', 'data-vm-clone-mode-choice');
  _vmCloneRefreshChoiceCards('vm-clone-safety', 'data-vm-clone-safety-choice');

  const guard = _vmCloneGuard(v, selectedMode);
  if (guardEl) {
    guardEl.className = 'vm-clone-guard ' + (guard.ok ? 'ok' : 'blocked');
    guardEl.textContent = guard.message;
  }
  if (submitEl) submitEl.disabled = !guard.ok;
}

function _vmCloneFriendlyError(message) {
  const raw = String(message || '');
  const lower = raw.toLowerCase();
  if (lower.includes('guest reset requires libguestfs-tools')) {
    return _L(
      'Guest reset에는 운영 서버의 libguestfs-tools가 필요합니다. 현재 서버에서 virt-sysprep, virt-customize, virt-filesystems, guestfish를 사용할 수 없습니다. 준비된 템플릿 VM이면 Prepared template을 선택하고, 일반 VM이면 서버에 libguestfs-tools를 설치한 뒤 다시 시도하세요.',
      'Guest reset requires libguestfs-tools on the host. virt-sysprep, virt-customize, virt-filesystems, and guestfish are unavailable. Select Prepared template only for an already prepared template VM, or install libguestfs-tools for a normal VM clone.'
    );
  }
  if (lower.includes('requires the source vm to be shut off')) {
    return _L('원본 VM이 실행 중입니다. VM을 종료한 뒤 clone을 다시 시도하세요.', 'The source VM is running. Shut it off and retry clone.');
  }
  return raw || t('error');
}

function _vmCloneShortError(message) {
  const lower = String(message || '').toLowerCase();
  if (lower.includes('guest reset requires libguestfs-tools')) {
    return _L('Guest reset 도구가 없습니다. Prepared template 선택 또는 libguestfs-tools 설치가 필요합니다.', 'Guest reset tools are missing. Select Prepared template or install libguestfs-tools.');
  }
  if (lower.includes('requires the source vm to be shut off')) {
    return _L('원본 VM을 종료한 뒤 다시 시도하세요.', 'Shut off the source VM and retry.');
  }
  return _vmCloneFriendlyError(message);
}

function _vmCloneShowError(message) {
  const friendly = _vmCloneFriendlyError(message);
  const guardEl = document.getElementById('vm-clone-guard');
  if (guardEl) {
    guardEl.className = 'vm-clone-guard blocked';
    guardEl.textContent = friendly;
  }
  toast(_vmCloneShortError(message), false);
}

async function vmClone(idx) {
  const actualIdx = (idx === 0 || idx) ? idx : selectedVmIndex;
  const v = vmList[actualIdx]; if (!v) return;
  selectedVmIndex = actualIdx;
  const suggested = (v.name || 'vm') + '-clone';
  const kind = _vmCloneStorageKind(v);
  const defaultMode = kind === 'zvol' ? 'cow' : 'full';
  showModal('<h2>' + _L('VM 복제', 'Clone VM') + ': ' + escapeHtml(v.name) + '</h2>'
    + '<div class="fr"><label>' + _L('새 VM 이름', 'New VM name') + '</label><input id="vm-clone-name" class="input-field" value="' + escapeAttr(suggested) + '"></div>'
    + '<input id="vm-clone-mode" type="hidden" value="' + escapeAttr(defaultMode) + '">'
    + '<div class="fr"><label>' + _L('복제 방식', 'Clone mode') + '</label><div class="flex-1">'
    + _vmCloneModeHelp(kind) + '</div></div>'
    + '<div class="fr"><label>' + _L('안전 처리', 'Safety') + '</label><div class="flex-1">'
    + _vmCloneSafetyHelp() + '</div></div>'
    + '<div id="vm-clone-guard" class="vm-clone-guard" role="status"></div>'
    + '<div class="text-right mt-12"><button class="btn" onclick="closeModal()">' + t('btn.cancel') + '</button> '
    + '<button id="vm-clone-submit" class="btn btn-g" onclick="doVmClone()">' + _L('복제', 'Clone') + '</button></div>');
  const defaultModeInput = document.querySelector('input[name="vm-clone-mode-choice"][value="' + defaultMode + '"]');
  if (defaultModeInput) defaultModeInput.checked = true;
  document.querySelectorAll('input[name="vm-clone-mode-choice"], input[name="vm-clone-safety"]').forEach(el => {
    el.addEventListener('change', () => _vmCloneRefreshGuard(v));
  });
  _vmCloneRefreshGuard(v);
}
async function doVmClone() {
  const v = vmList[selectedVmIndex]; if (!v) return;
  const name = (document.getElementById('vm-clone-name')?.value || '').trim();
  const mode = document.querySelector('input[name="vm-clone-mode-choice"]:checked')?.value ||
    document.getElementById('vm-clone-mode')?.value ||
    (_vmCloneStorageKind(v) === 'zvol' ? 'cow' : 'full');
  const safety = document.querySelector('input[name="vm-clone-safety"]:checked')?.value || 'guest-reset';
  const prepared = safety === 'template';
  if (!name || !/^[a-zA-Z0-9_-]{1,63}$/.test(name)) {
    toast(_L('VM 이름: 1-63자, 영문/숫자/_- 만 허용', 'VM name: 1-63 chars, [a-zA-Z0-9_-]'), false);
    return;
  }
  if (vmList.some(vm => vm && vm.name === name)) {
    toast(_L('대상 VM 이름이 이미 존재합니다.', 'Target VM name already exists.'), false);
    return;
  }
  const guard = _vmCloneGuard(v, mode);
  if (!guard.ok) {
    toast(guard.message, false);
    return;
  }
  const body = { new_name: name, mode: mode, guest_reset: !prepared };
  if (prepared) body.template_prepared = true;
  try {
    const r = await fetchPost(EP.VM_CLONE(v.name), body);
    if (r && r.error) { _vmCloneShowError(r.error.message || t('error')); return; }
    const d = unwrapData(r);
    toast(_L('복제 시작됨', 'Clone accepted') + ': ' + escapeHtml(name));
    addEvt('VM clone — ' + v.name + ' → ' + name + (d.job_id ? ' (' + d.job_id + ')' : ''));
    closeModal();
  } catch (e) { toast(e.message, false); }
}


function hwDisk() {
  return '<h4>&#128190; Disk Resize</h4><div class="fr"><label>New Size (GB)</label><input id="sd-size" type="number" value="40" placeholder="40"></div><div class="fr"><label>Disk Path</label><input id="sd-path" placeholder="vda (optional)"></div><button class="btn btn-g" onclick="doDiskResize()">' + t('btn.apply') + '</button><p class="stat-label mt-8">Live disk resize (qemu-img resize). VM can be running.</p>';
}
async function doDiskResize() {
  const v = vmList[selectedVmIndex]; if (!v) return;
  const size = document.getElementById('sd-size')?.value;
  const path = document.getElementById('sd-path')?.value;
  const body = { size_gb: parseInt(size) || 40 };
  if (path) body.disk_path = path;
  try {
    const r = await fetchPut(EP.VM_DISK(v.name), body);
    if (r.error) { toast('Resize failed: ' + (r.error.message || ''), false); return; }
    toast('Disk resized: ' + v.name); addEvt('Disk resize: ' + v.name + ' → ' + size + 'GB');
  } catch (e) { toast('Resize error: ' + e.message, false); }
}


async function vmDeleteStatus(name) {
  try {
    const r = await fetchGet(EP.VM_DELETE_STATUS(name));
    const d = unwrapData(r);
    return d.status || 'unknown';
  } catch (e) { return 'unknown'; }
}


async function vmExportOva(idx) {
  const v = vmList[idx ?? selectedVmIndex]; if (!v) return;
  if (!await customConfirm('Export OVA', _L('VM을 OVA 파일로 내보내시겠습니까?', 'Export ' + v.name + ' as OVA file?') + '\n' + v.name)) return;
  showModal('<h2>&#128230; Export OVA</h2><p class="mb-8"><b class="color-accent">' + escapeHtml(v.name) + '</b></p><div class="prog-bar"><div class="prog-fill" id="ova-p" class="w-pct-10"></div></div><div class="prog-status" id="ova-s"><span class="spinner"></span> ' + _L('내보내기 시작 중...', 'Starting export...') + '</div>');
  try {
    var pf = document.getElementById('ova-p'), ps = document.getElementById('ova-s');
    pf.style.width = '30%'; ps.innerHTML = '<span class="spinner"></span> ' + _L('OVA 변환 요청 중...', 'Requesting OVA conversion...');
    var r = await fetchPost(EP.VM_EXPORT(v.name), {});
    if (r.error) { pf.style.background = 'var(--red)'; pf.style.width = '100%'; ps.innerHTML = '&#10060; ' + escapeHtml(r.error.message || 'Export failed'); return; }
    pf.style.width = '70%'; ps.innerHTML = '<span class="spinner"></span> ' + _L('변환 진행 중...', 'Converting...');
    var d = unwrapData(r) || r;
    var path = d.path || d.ova_path || '';

    for (var pi = 0; pi < 5; pi++) {
      await new Promise(function(res) { setTimeout(res, 2000); });
      pf.style.width = (75 + pi * 5) + '%';
      try { var st = await fetchGet(EP.VM_DETAIL(v.name) + '/export-status'); var sd = unwrapData(st) || st; if (sd.status === 'done' || sd.status === 'completed') break; } catch(e) { break; }
    }
    pf.style.width = '100%'; pf.style.background = 'var(--green)';
    ps.innerHTML = '&#9989; ' + _L('내보내기 완료', 'Export completed') + (path ? '<br><span class="text-xs color-muted">' + escapeHtml(path) + '</span>' : '');
    toast('&#128230; ' + v.name + ' OVA ' + _L('내보내기 완료', 'export completed'));
    addEvt('OVA export: ' + v.name + (path ? ' → ' + path : ''));
  } catch (e) {
    var pf2 = document.getElementById('ova-p'), ps2 = document.getElementById('ova-s');
    if (pf2) { pf2.style.background = 'var(--red)'; pf2.style.width = '100%'; }
    if (ps2) ps2.innerHTML = '&#10060; ' + escapeHtml(e.message);
    toast(e.message, false);
  }
}


function hwCpuPin() {
  return '<h4>&#128204; CPU Pinning</h4><p class="stat-label mb-8">Pin vCPUs to physical cores for performance isolation.</p><div class="fr"><label>vCPU Map</label><input id="scpin" placeholder="0:0,1:2,2:4" class="flex-1"></div><p class="stat-label">Format: vCPU:pCPU pairs, comma separated (e.g., 0:0,1:2)</p><button class="btn btn-g mt-8" onclick="doCpuPin()">' + t('btn.apply') + '</button>';
}

async function doCpuPin() {
  const v = vmList[selectedVmIndex]; if (!v) return;
  const map = document.getElementById('scpin')?.value;
  if (!map) { toast('vCPU pin map required', false); return; }
  try {
    const r = await fetchPut(EP.VM_CPU_PIN(v.name), { vcpu_pin: map });
    if (r.error) { toast('CPU pin failed: ' + (r.error.message || ''), false); return; }
    toast('CPU pinning applied: ' + v.name); addEvt('CPU pin: ' + v.name);
  } catch (e) { toast(e.message, false); }
}


function hwBandwidth() {
  return '<h4>&#128246; Network Bandwidth (QoS)</h4><p class="stat-label mb-8">Set network bandwidth limits for VM interfaces.</p><div class="fr"><label>Inbound (Mbps)</label><input id="sbw-in" type="number" value="1000" placeholder="1000"></div><div class="fr"><label>Outbound (Mbps)</label><input id="sbw-out" type="number" value="1000" placeholder="1000"></div><div class="fr"><label>Burst (KB)</label><input id="sbw-burst" type="number" value="1024" placeholder="1024"></div><button class="btn btn-g mt-8" onclick="doBandwidth()">' + t('btn.apply') + '</button>';
}

async function doBandwidth() {
  const v = vmList[selectedVmIndex]; if (!v) return;
  try {
    const r = await fetchPut(EP.VM_BANDWIDTH(v.name), {
      inbound_avg: parseInt(document.getElementById('sbw-in')?.value) || 1000,
      outbound_avg: parseInt(document.getElementById('sbw-out')?.value) || 1000,
      burst: parseInt(document.getElementById('sbw-burst')?.value) || 1024
    });
    if (r.error) { toast('Bandwidth set failed: ' + (r.error.message || ''), false); return; }
    toast('Bandwidth QoS applied: ' + v.name); addEvt('Bandwidth: ' + v.name);
  } catch (e) { toast(e.message, false); }
}


async function showMemStats() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var h = '<h2>&#128204; Memory Stats: ' + esc(v.name) + '</h2>';
  h += '<div id="mem-stats-body"><span class="spinner"></span> ' + t('loading') + '</div>';
  h += '<div class="text-right mt-12"><button class="btn" onclick="closeModal()">' + t('btn.close') + '</button></div>';
  showModal(h);
  try {
    var r = await fetchPost(EP.VM_RPC(v.name), { method: 'vm.memory.stats', params: { name: v.name } });
    var d = unwrapData(r);
    var el = document.getElementById('mem-stats-body'); if (!el) return;
    var fmtKb = function(kb) {
      if (!kb && kb !== 0) return '-';
      if (kb >= 1048576) return (kb / 1048576).toFixed(2) + ' GB';
      if (kb >= 1024) return (kb / 1024).toFixed(1) + ' MB';
      return kb + ' KB';
    };
    var sh = '<div style="border:1px solid var(--border);border-radius:6px;padding:12px">';
    sh += '<div style="display:grid;grid-template-columns:1fr 1fr;gap:8px 16px;font-size:12px">';
    sh += '<div>' + H.row('Actual Balloon', '<span class="color-accent">' + fmtKb(d.actual_balloon_kb || d.actual) + '</span>') + '</div>';
    sh += '<div>' + H.row('RSS', '<span class="color-green">' + fmtKb(d.rss_kb || d.rss) + '</span>') + '</div>';
    sh += '<div>' + H.row('Unused', '<span class="color-muted">' + fmtKb(d.unused_kb || d.unused) + '</span>') + '</div>';
    sh += '<div>' + H.row('Available', '<span class="color-cyan">' + fmtKb(d.available_kb || d.available) + '</span>') + '</div>';
    sh += '<div>' + H.row('Swap In', fmtKb(d.swap_in_kb || d.swap_in || 0)) + '</div>';
    sh += '<div>' + H.row('Swap Out', fmtKb(d.swap_out_kb || d.swap_out || 0)) + '</div>';
    sh += '<div>' + H.row('Major Fault', String(d.major_fault || d.majflt || 0)) + '</div>';
    sh += '<div>' + H.row('Minor Fault', String(d.minor_fault || d.minflt || 0)) + '</div>';
    sh += '</div></div>';
    el.innerHTML = sh;
  } catch (e) {
    var el = document.getElementById('mem-stats-body');
    if (el) el.innerHTML = '<p class="color-red">Failed: ' + esc(e.message) + '</p>';
  }
}


async function showCpuStats() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var h = '<h2>&#9881; CPU Stats: ' + esc(v.name) + '</h2>';
  h += '<div id="cpu-stats-body"><span class="spinner"></span> ' + t('loading') + '</div>';
  h += '<div class="text-right mt-12"><button class="btn" onclick="closeModal()">' + t('btn.close') + '</button></div>';
  showModal(h);
  try {
    var r = await fetchPost(EP.VM_RPC(v.name), { method: 'vm.cpu.stats', params: { name: v.name } });
    var d = unwrapData(r);
    var el = document.getElementById('cpu-stats-body'); if (!el) return;
    var vcpuCount = d.vcpu_count || d.vcpu || v.vcpu || 0;
    var maxVcpu = d.max_vcpu || d.max || vcpuCount;
    var sh = '<div class="mb-12">';
    sh += H.row('vCPU Count', '<span class="color-accent">' + vcpuCount + '</span>');
    sh += H.row('Max vCPU', '<span class="color-muted">' + maxVcpu + '</span>');
    sh += H.row('CPU Time (ns)', '<span class="color-green">' + (d.cpu_time || 0) + '</span>');
    sh += '</div>';
    var vcpus = d.vcpus || d.vcpu_list || [];
    if (vcpus.length > 0) {
      sh += '<table><thead><tr><th>vCPU</th><th>State</th><th>CPU Time (ns)</th><th>Physical CPU</th></tr></thead><tbody>';
      vcpus.forEach(function(vc, i) {
        var state = vc.state === 1 || vc.state === 'running' ? '<span class="color-green">Running</span>' : '<span class="color-muted">Offline</span>';
        sh += '<tr><td><b>' + (vc.number !== undefined ? vc.number : i) + '</b></td>';
        sh += '<td>' + state + '</td>';
        sh += '<td>' + (vc.cpu_time || 0) + '</td>';
        sh += '<td>' + (vc.cpu !== undefined ? vc.cpu : '-') + '</td></tr>';
      });
      sh += '</tbody></table>';
    } else {
      sh += '<p class="color-muted text-12">Detailed per-vCPU info not available (VM may be stopped)</p>';
    }
    el.innerHTML = sh;
  } catch (e) {
    var el = document.getElementById('cpu-stats-body');
    if (el) el.innerHTML = '<p class="color-red">Failed: ' + esc(e.message) + '</p>';
  }
}


function showDiskLiveResize() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var h = '<h2>&#128190; Disk Live Resize: ' + esc(v.name) + '</h2>';
  h += '<p class="stat-label mb-12">Resize a VM disk while the VM is running. The guest OS may need to rescan partitions.</p>';
  h += '<div class="fr"><label>Target Device</label><input id="dlr-target" value="vda" placeholder="vda" class="w-120"></div>';
  h += '<div class="fr"><label>New Size (GB)</label><input id="dlr-size" type="number" value="40" min="1" placeholder="40" class="w-120"></div>';
  h += '<div class="text-right mt-14">';
  h += '<button class="btn btn-g" onclick="doDiskLiveResize()">&#128190; Resize</button> ';
  h += '<button class="btn" onclick="closeModal()">' + t('btn.cancel') + '</button>';
  h += '</div>';
  showModal(h);
}

async function doDiskLiveResize() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var target = (document.getElementById('dlr-target')?.value || 'vda').trim();
  var size = parseInt(document.getElementById('dlr-size')?.value) || 0;
  if (size < 1) { toast('Size must be at least 1 GB', false); return; }
  try {
    var r = await fetchPost(EP.VM_DISK_RESIZE(v.name), { target: target, new_size_gb: size });
    if (r.error) { toast('Resize failed: ' + (r.error.message || ''), false); return; }
    toast('Disk resized: ' + v.name + ' ' + target + ' -> ' + size + ' GB');
    addEvt('Disk live resize: ' + v.name + ' ' + target + ' -> ' + size + 'GB');
    closeModal();
  } catch (e) { toast('Resize error: ' + e.message, false); }
}


function _vmDiskUsagePct(fs) {
  if (!fs) return null;
  if (fs.usage_percent !== undefined) return Number(fs.usage_percent);
  var total = Number(fs.total_bytes || 0);
  var used = Number(fs.used_bytes || 0);
  return total > 0 ? (used * 100 / total) : null;
}

function _vmDiskUsageBar(pct) {
  if (pct === null || isNaN(pct)) return '<span class="color-muted">-</span>';
  return '<div style="min-width:120px">' + renderProgressBar(Math.max(0, Math.min(100, pct))) + '<div class="text-xs color-muted mt-4">' + pct.toFixed(1) + '%</div></div>';
}

function _vmDiskUsageSeverity(pct) {
  if (pct === null || isNaN(pct)) return 'y';
  if (pct >= 90) return 'r';
  if (pct >= 80) return 'y';
  return 'g';
}

function _vmRenderDiskUsage(d) {
  var filesystems = Array.isArray(d.filesystems) ? d.filesystems : [];
  var total = Number(d.total_bytes || 0);
  var used = Number(d.used_bytes || 0);
  var pct = d.usage_percent !== undefined ? Number(d.usage_percent) : (total > 0 ? used * 100 / total : null);
  var h = '<div class="mb-12">';
  h += '<div class="sg grid-3">';
  h += H.card(_L('전체 사용량', 'Total Usage'), H.row(_L('사용', 'Used'), used ? formatBytes(used) : '-') + H.row(_L('전체', 'Total'), total ? formatBytes(total) : '-') + H.row(_L('상태', 'Status'), H.badge(pct === null ? _L('알 수 없음', 'Unknown') : pct.toFixed(1) + '%', _vmDiskUsageSeverity(pct))) + _vmDiskUsageBar(pct));
  h += H.card(_L('마운트', 'Mounts'), H.row(_L('파일시스템', 'Filesystems'), String(filesystems.length)) + H.row(_L('수집 방식', 'Source'), 'qemu-guest-agent') + H.row(_L('대상', 'Target'), escapeHtml(d.name || '-')));
  h += '</div></div>';

  if (!filesystems.length) {
    return h + '<p class="color-muted text-12">' + _L('게스트 파일시스템 정보가 없습니다.', 'No guest filesystem data returned.') + '</p>';
  }

  filesystems.sort(function(a, b) {
    var am = a.mountpoint || '';
    var bm = b.mountpoint || '';
    if (am === '/') return -1;
    if (bm === '/') return 1;
    return am.localeCompare(bm);
  });

  h += '<table><thead><tr>'
    + '<th>' + _L('마운트', 'Mount') + '</th>'
    + '<th>' + _L('타입', 'Type') + '</th>'
    + '<th>' + _L('사용', 'Used') + '</th>'
    + '<th>' + _L('전체', 'Total') + '</th>'
    + '<th>' + _L('사용률', 'Usage') + '</th>'
    + '</tr></thead><tbody>';
  filesystems.forEach(function(fs) {
    var fsPct = _vmDiskUsagePct(fs);
    var fsUsed = Number(fs.used_bytes || 0);
    var fsTotal = Number(fs.total_bytes || 0);
    h += '<tr>';
    h += '<td><b>' + escapeHtml(fs.mountpoint || '-') + '</b><div class="text-xs color-muted">' + escapeHtml(fs.name || fs.device || '') + '</div></td>';
    h += '<td>' + escapeHtml(fs.type || '-') + '</td>';
    h += '<td>' + (fsUsed ? formatBytes(fsUsed) : '-') + '</td>';
    h += '<td>' + (fsTotal ? formatBytes(fsTotal) : '-') + '</td>';
    h += '<td>' + _vmDiskUsageBar(fsPct) + '</td>';
    h += '</tr>';
  });
  h += '</tbody></table>';
  return h;
}

async function showVmDiskUsage() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var running = v.state === 'running';
  var h = '<h2>&#128202; ' + _L('디스크 사용량', 'Disk Usage') + ': ' + esc(v.name) + '</h2>';
  h += '<div id="vm-disk-usage-body" style="min-height:90px"><span class="spinner"></span> ' + t('loading') + '</div>';
  h += '<div class="text-right mt-14"><button class="btn" onclick="closeModal()">' + t('btn.close') + '</button></div>';
  showModal(h);

  var body = document.getElementById('vm-disk-usage-body');
  if (!running) {
    if (body) body.innerHTML = '<p class="color-muted">' + _L('게스트 파일시스템 사용량은 VM 실행 중에만 조회할 수 있습니다.', 'Guest filesystem usage is available only while the VM is running.') + '</p>';
    return;
  }

  try {
    var r = await fetchGet(EP.VM_DISK_USAGE(v.name));
    if (r.error) throw new Error(r.error.message || 'disk usage failed');
    var d = unwrapData(r) || {};
    if (body) body.innerHTML = _vmRenderDiskUsage(d);
  } catch (e) {
    if (body) body.innerHTML = '<p class="color-red">' + esc(e.message) + '</p>'
      + '<p class="color-muted text-xs mt-8">' + _L('qemu-guest-agent 채널과 게스트 내부 에이전트 상태를 확인하세요.', 'Check the qemu-guest-agent channel and guest agent status.') + '</p>'
      + '<button class="btn btn-g mt-8" onclick="closeModal();showGuestAgent()">&#128172; Guest Agent</button>';
  }
}


var _gaInstallCommands = {};

function showGuestAgent() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var h = '<h2>&#128172; Guest Agent: ' + esc(v.name) + '</h2>';
  h += '<div class="mb-14 p-10 border-muted rounded-md">';
  h += '<div style="display:flex;gap:8px;flex-wrap:wrap;margin-bottom:10px">';
  h += '<button class="btn" onclick="gaRefreshStatus()">&#8635; Status</button>';
  h += '<button class="btn btn-g" onclick="gaEnsureChannel()">Channel</button>';
  h += '<button class="btn btn-g" onclick="gaPing()">&#128994; Ping</button>';
  h += '<button class="btn btn-r" onclick="gaShutdown()">&#9888; Graceful Shutdown</button>';
  h += '</div>';
  h += '<div id="ga-status-body" style="font-size:12px;min-height:48px;margin-bottom:10px"><span class="spinner"></span> Checking...</div>';
  h += '<div id="ga-ping-result" style="font-size:12px;min-height:20px;margin-bottom:8px"></div>';
  h += '</div>';

  h += '<div class="mb-14 p-10 border-muted rounded-md">';
  h += '<h4 class="mb-8">Install qemu-guest-agent</h4>';
  h += '<div id="ga-install-body" class="text-12 color-muted"></div>';
  h += '</div>';

  h += '<div class="mb-14 p-10 border-muted rounded-md">';
  h += '<h4 class="mb-8">&#128187; Execute Command</h4>';
  h += '<div class="fr"><label>Command</label><input id="ga-cmd" placeholder="cat /etc/hostname" class="flex-1"></div>';
  h += '<div class="fr"><label>Args</label><input id="ga-args" placeholder="(optional, space separated)" class="flex-1"></div>';
  h += '<button class="btn btn-g" onclick="gaExec()" style="margin-top:6px">&#9654; Execute</button>';
  h += '<div id="ga-exec-result" style="margin-top:10px;background:var(--bg);border:1px solid var(--border);border-radius:6px;padding:10px;max-height:250px;overflow:auto;font-size:11px;font-family:var(--font-mono);white-space:pre-wrap;display:none"></div>';
  h += '</div>';

  h += '<div class="text-right"><button class="btn" onclick="closeModal()">' + t('btn.close') + '</button></div>';
  showModal(h);
  setTimeout(gaRefreshStatus, 20);
}

function gaCommand(key) {
  if (_gaInstallCommands && _gaInstallCommands[key]) return _gaInstallCommands[key];
  if (key === 'rhel_rocky_fedora') return 'sudo dnf install -y qemu-guest-agent && sudo systemctl enable --now qemu-guest-agent';
  if (key === 'suse') return 'sudo zypper install -y qemu-guest-agent && sudo systemctl enable --now qemu-guest-agent';
  return 'sudo apt update && sudo apt install -y qemu-guest-agent && sudo systemctl enable --now qemu-guest-agent';
}

function gaStatusBadge(status) {
  if (status === 'ok') return H.badge('OK', 'g');
  if (status === 'vm_stopped') return H.badge('Stopped', 'y');
  if (status === 'reboot_required') return H.badge('Reboot needed', 'y');
  if (status === 'agent_unavailable') return H.badge('Install needed', 'y');
  return H.badge('Channel missing', 'r');
}

function gaRenderInstallCommands(cmds) {
  _gaInstallCommands = cmds || {};
  var rows = [
    ['debian_ubuntu', 'Debian / Ubuntu'],
    ['rhel_rocky_fedora', 'RHEL / Rocky / Fedora'],
    ['suse', 'SUSE']
  ];
  var h = '';
  rows.forEach(function(row) {
    var key = row[0], label = row[1], cmd = gaCommand(key);
    h += '<div class="mb-8">';
    h += '<div class="justify-between mb-4"><b>' + esc(label) + '</b><button class="btn" style="font-size:11px;padding:3px 8px" onclick="gaCopyInstall(\'' + escapeAttr(key) + '\')">Copy</button></div>';
    h += '<pre style="margin:0;background:var(--bg);border:1px solid var(--border);border-radius:6px;padding:8px;white-space:pre-wrap;overflow:auto">' + esc(cmd) + '</pre>';
    h += '</div>';
  });
  return h;
}

function gaRenderStatus(d) {
  var statusEl = document.getElementById('ga-status-body');
  var installEl = document.getElementById('ga-install-body');
  if (installEl) installEl.innerHTML = gaRenderInstallCommands(d.install_commands || {});
  if (!statusEl) return;
  var h = '<div style="display:grid;grid-template-columns:1fr 1fr;gap:8px 16px">';
  h += H.row('Status', gaStatusBadge(d.status));
  h += H.row('Running', d.running ? H.badge('Yes', 'g') : H.badge('No', 'y'));
  h += H.row('Config channel', d.channel_configured ? H.badge('Yes', 'g') : H.badge('No', 'r'));
  h += H.row('Live channel', d.channel_live ? H.badge('Yes', 'g') : H.badge('No', d.running ? 'y' : 'r'));
  h += H.row('Agent ping', d.agent_ping ? H.badge('OK', 'g') : H.badge('No response', 'y'));
  h += H.row('Next action', esc(d.message || '-'));
  if (d.agent_error) h += H.row('Agent error', '<span class="color-muted text-11">' + esc(d.agent_error) + '</span>');
  h += '</div>';
  statusEl.innerHTML = h;
}

async function gaRefreshStatus() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var statusEl = document.getElementById('ga-status-body');
  if (statusEl) statusEl.innerHTML = '<span class="spinner"></span> Checking...';
  try {
    var r = await fetchGet(EP.VM_GUEST_AGENT(v.name));
    var d = unwrapData(r);
    gaRenderStatus(d || {});
  } catch (e) {
    if (statusEl) statusEl.innerHTML = '<span class="color-red">' + esc(e.message) + '</span>';
  }
}

async function gaEnsureChannel() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  try {
    var r = await fetchPost(EP.VM_GUEST_AGENT_CHANNEL(v.name), {});
    if (r.error) { toast(r.error.message || 'Channel update failed', false); return; }
    var d = unwrapData(r);
    toast(d.reboot_required ? 'Channel configured. Restart VM to activate it.' : 'Guest agent channel ready.');
    addEvt('Guest agent channel: ' + v.name + ' ' + (d.status || 'updated'));
    gaRenderStatus(d || {});
    setTimeout(gaRefreshStatus, 800);
  } catch (e) { toast(e.message, false); }
}

function gaCopyInstall(key) {
  var cmd = gaCommand(key);
  navigator.clipboard.writeText(cmd)
    .then(function(){ toast('Install command copied'); })
    .catch(function(){ toast(cmd, true); });
}

async function gaPing() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var el = document.getElementById('ga-ping-result');
  if (el) el.innerHTML = '<span class="spinner"></span> Pinging...';
  try {
    var r = await fetchPost(EP.VM_GUEST_PING(v.name), {});
    if (r.error) { if (el) el.innerHTML = '<span class="color-red">&#10060; ' + _L('에이전트 응답 없음', 'Agent not responding') + ': ' + esc(r.error.message || '') + '</span>'; return; }
    if (el) el.innerHTML = '<span class="color-green">&#9989; ' + _L('게스트 에이전트 정상 응답', 'Guest agent is responding') + '</span>';
  } catch (e) { if (el) el.innerHTML = '<span class="color-red">&#10060; ' + esc(e.message) + '</span>'; }
}

async function gaShutdown() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  if (!await customConfirm('Graceful Shutdown', 'Send ACPI shutdown signal via guest agent to ' + v.name + '?')) return;
  try {
    var r = await fetchPost(EP.VM_GUEST_SHUTDOWN(v.name), {});
    if (r.error) { toast('Shutdown failed: ' + (r.error.message || ''), false); return; }
    toast('Graceful shutdown sent: ' + v.name);
    addEvt('Guest agent shutdown: ' + v.name);
    closeModal();
    setTimeout(loadAll, 3000);
  } catch (e) { toast('Error: ' + e.message, false); }
}

async function gaExec() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var cmd = (document.getElementById('ga-cmd')?.value || '').trim();
  if (!cmd) { toast('Command required', false); return; }
  var args = (document.getElementById('ga-args')?.value || '').trim();
  var el = document.getElementById('ga-exec-result');
  if (el) { el.style.display = 'block'; el.innerHTML = '<span class="spinner"></span> Executing...'; }
  try {
    var params = { name: v.name, command: cmd };
    if (args) params.args = args.split(/\s+/);
    var r = await fetchPost(EP.VM_GUEST_EXEC(v.name), params);
    if (r.error) { if (el) el.innerHTML = '<span class="color-red">Error: ' + esc(r.error.message || '') + '</span>'; return; }
    var d = unwrapData(r);
    var out = '';
    if (d.stdout) out += '<div class="mb-6"><span class="color-green">stdout:</span></div><div>' + esc(d.stdout) + '</div>';
    if (d.stderr) out += '<div class="mt-8"><span class="color-red">stderr:</span></div><div>' + esc(d.stderr) + '</div>';
    var exitCode = d.exitcode !== undefined ? d.exitcode : d.exit_code;
    if (exitCode !== undefined) out += '<div style="margin-top:6px;color:var(--fg2)">Exit code: ' + exitCode + '</div>';
    if (!out) out = '<span class="color-muted">Command executed (no output)</span>';
    if (el) el.innerHTML = out;
  } catch (e) { if (el) el.innerHTML = '<span class="color-red">' + esc(e.message) + '</span>'; }
}


async function vmMigrateDrop(vmName, targetIp, targetName) {
  if (!PCV.isMultiEdgeUI()) {
    toast(_L('클러스터 빌드 전용 기능입니다', 'This action is available only on the cluster build'), false);
    return;
  }
  if (!await customConfirm(_L('라이브 마이그레이션', 'Live Migration'),
    vmName + ' → ' + targetName + ' (' + targetIp + ')?')) return;
  showModal('<h2>&#128640; ' + _L('마이그레이션', 'Migrating') + '</h2><p>' + esc(vmName) + ' → ' + esc(targetName) + '</p><div class="prog-bar"><div class="prog-fill" id="mig-prog" class="w-pct-20"></div></div><div class="prog-status" id="mig-st"><span class="spinner"></span> ' + _L('전송 중...', 'Transferring...') + '</div>');
  try {
    var migrateEndpoint = PCV.getOptionalEndpoint('VM_MIGRATE', vmName);
    if (!migrateEndpoint) {
      toast(_L('Single Edge에서는 마이그레이션이 비활성화됩니다', 'Migration is disabled on Single Edge'), false);
      closeModal();
      return;
    }
    var r = await fetchPost(migrateEndpoint, { target: 'qemu+ssh://pcvdev@' + targetIp + '/system' });
    var pf = document.getElementById('mig-prog'), ps = document.getElementById('mig-st');
    if (r && r.error) {
      if (pf) { pf.style.width = '100%'; pf.style.background = 'var(--red)'; }
      if (ps) ps.innerHTML = '&#10060; ' + esc(r.error.message || 'Failed');
    } else {
      if (pf) pf.style.width = '100%';
      if (ps) ps.innerHTML = '&#9989; ' + _L('마이그레이션 시작됨', 'Migration started');
      addEvt('VM Migrate: ' + vmName + ' → ' + targetName);
      setTimeout(function() { closeModal(); loadAll(); }, 2000);
    }
  } catch (e) {
    var ps = document.getElementById('mig-st');
    if (ps) ps.innerHTML = '&#10060; ' + esc(e.message);
  }
}

function showBlkioEditor() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var h = '<h2>&#128190; ' + (t('vm.blkio_title') || 'Disk I/O Limits') + ': ' + esc(v.name) + '</h2>';
  h += '<p class="stat-label mb-12">'
    + (t('vm.blkio_desc') || 'Set disk I/O throttle limits. Values in bytes/sec and IOPS. Set 0 for unlimited.')
    + '</p>';
  h += '<div class="fr"><label>' + (t('vm.read_bytes_sec') || 'Read (MB/s)') + '</label><input id="blkio-rd-bytes" type="number" value="0" min="0" placeholder="0" class="w-140"><span class="stat-label ml-4">MB/s</span></div>';
  h += '<div class="fr"><label>' + (t('vm.write_bytes_sec') || 'Write (MB/s)') + '</label><input id="blkio-wr-bytes" type="number" value="0" min="0" placeholder="0" class="w-140"><span class="stat-label ml-4">MB/s</span></div>';
  h += '<div class="fr"><label>' + (t('vm.read_iops_sec') || 'Read IOPS') + '</label><input id="blkio-rd-iops" type="number" value="0" min="0" placeholder="0" class="w-140"></div>';
  h += '<div class="fr"><label>' + (t('vm.write_iops_sec') || 'Write IOPS') + '</label><input id="blkio-wr-iops" type="number" value="0" min="0" placeholder="0" class="w-140"></div>';
  h += '<div id="blkio-status" style="font-size:11px;min-height:20px;margin:8px 0"></div>';
  h += '<div class="text-right mt-14">';
  h += '<button class="btn" onclick="blkioGet()" style="margin-right:4px">&#128269; ' + (t('vm.blkio_get') || 'Get Current') + '</button>';
  h += '<button class="btn btn-g" onclick="blkioSet()">&#9989; ' + (t('vm.blkio_apply') || 'Apply') + '</button> ';
  h += '<button class="btn" onclick="closeModal()">' + t('btn.cancel') + '</button>';
  h += '</div>';
  showModal(h);
}

async function blkioGet() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var el = document.getElementById('blkio-status');
  if (el) el.innerHTML = '<span class="spinner"></span> ' + (t('loading') || 'Loading...');
  try {
    var r = await fetchPost(EP.VM_RPC(v.name), { method: 'vm.blkio.get', params: { name: v.name } });
    if (r.error) { if (el) el.innerHTML = '<span class="color-red">' + esc(r.error.message || 'Failed') + '</span>'; return; }
    var d = unwrapData(r);
    var rdB = document.getElementById('blkio-rd-bytes');
    var wrB = document.getElementById('blkio-wr-bytes');
    var rdI = document.getElementById('blkio-rd-iops');
    var wrI = document.getElementById('blkio-wr-iops');
    if (rdB) rdB.value = Math.round((d.read_bytes_sec || 0) / 1048576);
    if (wrB) wrB.value = Math.round((d.write_bytes_sec || 0) / 1048576);
    if (rdI) rdI.value = d.read_iops_sec || 0;
    if (wrI) wrI.value = d.write_iops_sec || 0;
    if (el) el.innerHTML = '<span class="color-green">&#9989; ' + (t('vm.blkio_loaded') || 'Current limits loaded') + '</span>';
  } catch (e) {
    if (el) el.innerHTML = '<span class="color-red">' + esc(e.message) + '</span>';
  }
}

async function blkioSet() {
  var v = vmList[selectedVmIndex]; if (!v) return;
  var rdMB = parseInt((document.getElementById('blkio-rd-bytes') || {}).value) || 0;
  var wrMB = parseInt((document.getElementById('blkio-wr-bytes') || {}).value) || 0;
  var rdIops = parseInt((document.getElementById('blkio-rd-iops') || {}).value) || 0;
  var wrIops = parseInt((document.getElementById('blkio-wr-iops') || {}).value) || 0;
  var el = document.getElementById('blkio-status');
  if (el) el.innerHTML = '<span class="spinner"></span> ' + (t('vm.blkio_applying') || 'Applying...');
  try {
    var r = await fetchPost(EP.VM_RPC(v.name), {
      method: 'vm.blkio.set',
      params: {
        name: v.name,
        read_bytes_sec: rdMB * 1048576,
        write_bytes_sec: wrMB * 1048576,
        read_iops_sec: rdIops,
        write_iops_sec: wrIops
      }
    });
    if (r.error) {
      if (el) el.innerHTML = '<span class="color-red">' + esc(r.error.message || 'Failed') + '</span>';
      toast((t('vm.blkio_failed') || 'I/O limit failed') + ': ' + (r.error.message || ''), false);
      return;
    }
    if (el) el.innerHTML = '<span class="color-green">&#9989; ' + (t('vm.blkio_applied') || 'I/O limits applied') + '</span>';
    toast((t('vm.blkio_applied') || 'I/O limits applied') + ': ' + v.name);
    addEvt('BlkIO set: ' + v.name + ' R:' + rdMB + 'MB/s W:' + wrMB + 'MB/s');
    setTimeout(closeModal, 1500);
  } catch (e) {
    if (el) el.innerHTML = '<span class="color-red">' + esc(e.message) + '</span>';
    toast(e.message, false);
  }
}






PCV.vm = {
  render: render,
  setSort: setSort,
  getFiltered: getFiltered,
  toggleVmView: toggleVmView,
  vmPower: vmPower,
  vmDel: vmDel,
  doVmDel: doVmDel,
  showCreate: showCreate,
  doCreate: doCreate,
  wizStorageChanged: wizStorageChanged,
  wizPickStoragePool: wizPickStoragePool,
  wizLoadStorageTargets: wizLoadStorageTargets,
  showSettings: showSettings,
  showRenameVm: showRenameVm,
  doVmRename: doVmRename,
  showSnap: showSnap,
  showVnc: showVnc,
  vmClone: vmClone,
  doVmClone: doVmClone,
  vmExportOva: vmExportOva,
  vmDeleteStatus: vmDeleteStatus,
  showNicMgr: showNicMgr,
  showMemStats: showMemStats,
  showCpuStats: showCpuStats,
  showDiskLiveResize: showDiskLiveResize,
  showVmDiskUsage: showVmDiskUsage,
  showGuestAgent: showGuestAgent,
  gaRefreshStatus: gaRefreshStatus,
  gaEnsureChannel: gaEnsureChannel,
  showBlkioEditor: showBlkioEditor,
  showVmCompare: showVmCompare,
  showBulkActions: showBulkActions,
  bulkAction: bulkAction,
  bulkSnapshot: bulkSnapshot,
  renderSummary: renderSummary,
  renderConsole: renderConsole,
  renderSnapshots: renderSnapshots,
  renderPerformance: renderPerformance,
  renderTimeline: renderTimeline,
  vmMigrateDrop: vmMigrateDrop,
  connectWS: connectWS
};


window.render = render;
window.setSort = setSort;
window.getFiltered = getFiltered;
window.toggleChk = toggleChk;
window.bulkStop = bulkStop;
window.showCtx = showCtx;
window.renderSummary = renderSummary;
window.renderConsole = renderConsole;
window.openNoVNC = openNoVNC;
window.vncFullscreen = vncFullscreen;
window.vncFitWindow = vncFitWindow;
window.openNoVNCPopup = openNoVNCPopup;
window.copyVncAddr = copyVncAddr;
window.renderSnapshots = renderSnapshots;
window.takeSnap = takeSnap;
window.snapNameValidate = snapNameValidate;
window.snapCreateExec = snapCreateExec;
window.snapRb = snapRb;
window.rbValidate = rbValidate;
window.rbExec = rbExec;
window.snapDl = snapDl;
window.snapDeleteAll = snapDeleteAll;
window.sdaPreview = sdaPreview;
window.sdaExec = sdaExec;
window.renderPerformance = renderPerformance;
window.renderTimeline = renderTimeline;
window.vmPower = vmPower;
window.pw = vmPower;
window.vmDel = vmDel;
window.doVmDel = doVmDel;
window.showCreate = showCreate;
window.wizSave = wizSave;
window.wizGo = wizGo;
window.renderWiz = renderWiz;
window.wizLoadNets = wizLoadNets;
window.wizStorageChanged = wizStorageChanged;
window.wizPickStoragePool = wizPickStoragePool;
window.wizLoadStorageTargets = wizLoadStorageTargets;
window.browseISO = browseISO;
window.isoSelect = isoSelect;
window.isoSelectManual = isoSelectManual;
window.closeISOBrowser = closeISOBrowser;
window.doCreate = doCreate;
window.showSettings = showSettings;
window.showRenameVm = showRenameVm;
window.doVmRename = doVmRename;
window.setHw = setHw;
window.hwIdentity = hwIdentity;
window.hwCpu = hwCpu;
window.hwMem = hwMem;
window.hwCd = hwCd;
window.browseISOForMount = browseISOForMount;
window.selectISOForMount = selectISOForMount;
window.hwNic = hwNic;
window.loadNics = loadNics;
window.nicAdd = nicAdd;
window.nicDel = nicDel;
window.hwAP = hwAP;
window.loadBP = loadBP;
window.bpSet = bpSet;
window.doSet = doSet;
window.doMnt = doMnt;
window.doEjt = doEjt;
window.showSnap = showSnap;
window.showNicMgr = showNicMgr;
window.nmAdd = nmAdd;
window.nmDel = nmDel;
window.showVnc = showVnc;
window.vmClone = vmClone;
window.doVmClone = doVmClone;
window.hwDisk = hwDisk;
window.doDiskResize = doDiskResize;
window.vmDeleteStatus = vmDeleteStatus;
window.vmExportOva = vmExportOva;
window.hwCpuPin = hwCpuPin;
window.doCpuPin = doCpuPin;
window.hwBandwidth = hwBandwidth;
window.doBandwidth = doBandwidth;
window.showMemStats = showMemStats;
window.showCpuStats = showCpuStats;
window.showDiskLiveResize = showDiskLiveResize;
window.showVmDiskUsage = showVmDiskUsage;
window.doDiskLiveResize = doDiskLiveResize;
window.showGuestAgent = showGuestAgent;
window.gaRefreshStatus = gaRefreshStatus;
window.gaEnsureChannel = gaEnsureChannel;
window.gaCopyInstall = gaCopyInstall;
window.gaPing = gaPing;
window.gaShutdown = gaShutdown;
window.gaExec = gaExec;
window.showBlkioEditor = showBlkioEditor;
window.blkioGet = blkioGet;
window.blkioSet = blkioSet;
window.toggleVmView = toggleVmView;
window.showVmCompare = showVmCompare;
window.showBulkActions = showBulkActions;
window.bulkAction = bulkAction;
window.bulkSnapshot = bulkSnapshot;
window.vmMigrateDrop = vmMigrateDrop;

})(window.PCV);






window.PCV = window.PCV || {};
(function(PCV) {




window.selCtr = window.selCtr || null;
window.ctrTab = window.ctrTab || 'summary';
window.ctrHist = window.ctrHist || [];


window.ctrSortKey = window.ctrSortKey || 'name';
window.ctrSortDir = window.ctrSortDir || 1;
function setCtrSort(k) {
  if (window.ctrSortKey === k) window.ctrSortDir *= -1; else { window.ctrSortKey = k; window.ctrSortDir = 1; }
  renderContainerList();
}


async function renderContainerList() {
  const el = document.getElementById('ctr-list');
  if (!el) return;
  try {
    const c = await fetchGet(EP.CTR_LIST());
    const l = unwrapList(c);
    const countEl = document.getElementById('ctr-count');
    if (countEl) countEl.textContent = l.length;

    const filter = (document.getElementById('ctr-filter')?.value || '').toLowerCase();
    let fl = filter ? l.filter(v => v.name.toLowerCase().includes(filter)) : [...l];

    fl.sort((a, b) => {
      let va, vb;
      if (window.ctrSortKey === 'name') { va = a.name || ''; vb = b.name || ''; }
      else if (window.ctrSortKey === 'state') { va = a.state || ''; vb = b.state || ''; }
      else if (window.ctrSortKey === 'ip') { va = a.ip_addr || ''; vb = b.ip_addr || ''; }
      else { va = a.name || ''; vb = b.name || ''; }
      return (va < vb ? -1 : va > vb ? 1 : 0) * window.ctrSortDir;
    });

    if (fl.length === 0) {
      el.innerHTML = '<div class="empty-state" style="padding:24px;text-align:center"><div style="font-size:32px;margin-bottom:8px">&#9783;</div><div class="text-xs color-muted">' + t('msg.no_containers') + '</div><button class="btn btn-g mt-12 text-11" onclick="showCtrCreate()">+ ' + t('ctr.new') + '</button></div>';
      return;
    }
    let h = '';
    fl.forEach(v => {
      const on = v.state === 'RUNNING';
      const s = selCtr === v.name;
      h += '<div onclick="selCtr=\'' + esc(v.name) + '\';currentTab=\'containers\';renderContent();renderContainerList()" class="vm-item' + (s ? ' sel' : '') + '" style="padding:6px 8px;cursor:pointer;border-left:3px solid ' + (s ? 'var(--accent)' : 'transparent') + ';background:' + (s ? 'rgba(0,240,255,.06)' : 'transparent') + '">';
      h += '<div class="flex items-center gap-6">';
      h += '<span style="font-size:8px;color:' + (on ? 'var(--green)' : 'var(--red)') + '">&#9679;</span>';
      h += '<span style="font-size:12px;font-weight:' + (s ? '700' : '400') + '">' + esc(v.name) + '</span>';
      h += '</div>';
      h += '<div style="display:flex;justify-content:space-between;margin-left:14px;margin-top:1px">';
      h += '<span class="stat-label text-xs">' + (on ? t('status.running') : t('status.stopped')) + '</span>';
      if (on && v.ip_addr) h += '<span class="stat-label text-xs color-green">' + esc(v.ip_addr) + '</span>';
      h += '</div></div>';
    });
    el.innerHTML = h;
  } catch (e) { el.innerHTML = '<div class="text-xs color-muted" style="padding:8px">Failed to load containers.</div>'; }
}


async function renderContainers(b) {
  b.innerHTML = showSkeleton();
  try {
    const c = await fetchGet(EP.CTR_LIST());
    const l = unwrapList(c);
    if (l.length === 0) {
      b.innerHTML = (typeof emptyStatePro === 'function')
        ? emptyStatePro({
            icon: '&#9783;',
            title: _L('컨테이너가 없습니다', 'No containers'),
            desc: _L('첫 LXC 컨테이너를 만들어보세요. ZFS 백엔드 + cloud-init 자동.', 'Create your first LXC container with ZFS backend.'),
            ctaLabel: _L('+ 컨테이너 만들기', '+ Create Container'),
            ctaAction: 'showCtrCreate()'
          })
        : '<div class="empty-state"><div class="empty-state-icon">&#9783;</div><div class="empty-state-text">' + t('msg.no_containers') + '</div><button class="btn btn-g" onclick="showCtrCreate()" class="mt-12">+ ' + t('ctr.new') + '</button></div>';
      return;
    }
    let h = '<div style="display:flex;gap:0;height:calc(100vh - 280px);min-height:400px">';
    h += '<div style="min-width:220px;max-width:220px;border-right:1px solid var(--border);overflow-y:auto;padding:8px">';
    h += '<div class="justify-between items-center mb-8"><span class="text-xs font-bold" style="text-transform:uppercase;letter-spacing:.06em;color:var(--fg2)">' + t('nav.containers') + '</span><div class="flex gap-6 items-center">' + H.badge(String(l.length), 'y') + '<button class="btn btn-g" style="font-size:11px;padding:4px 10px" onclick="showCtrCreate()">+ ' + t('ctr.new') + '</button></div></div>';
    l.forEach(v => {
      const on = v.state === 'RUNNING';
      const s = selCtr === v.name;
      h += '<div onclick="selCtr=\'' + v.name + '\';ctrTab=\'summary\';renderContainers(document.getElementById(\'cb\'))" style="padding:6px 8px;cursor:pointer;border-radius:4px;margin-bottom:2px;border-left:3px solid ' + (s ? 'var(--accent)' : 'transparent') + ';background:' + (s ? 'var(--bg3)' : 'transparent') + '">';
      h += '<div class="flex items-center gap-6"><span style="font-size:9px;color:' + (on ? 'var(--green)' : 'var(--fg2)') + '">&#9679;</span><span style="font-size:13px;font-weight:' + (s ? '600' : '400') + '">' + escapeHtml(v.name) + '</span></div>';
      h += '<div class="stat-label" style="margin-left:15px">' + v.state + (on ? ' &bull; ' + (v.ip_addr || '') : '') + '</div></div>';
    });
    h += '</div>';
    h += '<div style="flex:1;overflow-y:auto;display:flex;flex-direction:column">';
    const cv = l.find(x => x.name === selCtr);
    if (cv) {
      const on = cv.state === 'RUNNING';
      h += '<div style="padding:10px 14px;border-bottom:1px solid var(--border)" class="justify-between items-center">';
      h += '<div><span style="font-size:15px;font-weight:700">' + escapeHtml(cv.name) + '</span> ' + H.badge(cv.state, on ? 'g' : 'r') + '</div>';
      h += '<div class="flex gap-4">';
      if (!on) h += '<button class="btn btn-g text-12 px-12 py-4" onclick="ctrA(\'' + cv.name + '\',\'start\')">&#9654; ' + t('power.start') + '</button>';
      if (on) {
        h += '<button class="btn btn-r text-12 px-12 py-4" onclick="ctrA(\'' + cv.name + '\',\'stop\')">&#9632; ' + t('power.stop') + '</button>';
        h += '<button class="btn text-12 px-12 py-4" onclick="ctrReboot(\'' + cv.name + '\')">&#8635; Reboot</button>';
      }
      h += '<button class="btn btn-r text-12 px-12 py-4" onclick="ctrDel(\'' + cv.name + '\')">&#128465; ' + t('btn.delete') + '</button>';
      h += '</div></div>';
      const tabs = ['summary', 'console', 'resources', 'network', 'dns', 'options', 'snapshots', 'notes', 'tasks'];
      h += '<div class="flex" style="border-bottom:1px solid var(--border);padding:0 10px;gap:2px;overflow-x:auto">';
      tabs.forEach(t2 => {
        h += '<div onclick="ctrTab=\'' + t2 + '\';renderContainers(document.getElementById(\'cb\'))" style="padding:9px 14px;font-size:13px;cursor:pointer;white-space:nowrap;border-bottom:2px solid ' + (ctrTab === t2 ? 'var(--accent)' : 'transparent') + ';color:' + (ctrTab === t2 ? 'var(--accent)' : 'var(--fg2)') + ';font-weight:' + (ctrTab === t2 ? '600' : '400') + ';transition:color .15s">' + t2.charAt(0).toUpperCase() + t2.slice(1) + '</div>';
      });
      h += '</div>';
      h += '<div style="padding:14px;flex:1" id="ctr-tab-content"></div>';
    } else {
      h += '<div style="flex:1;display:flex;align-items:center;justify-content:center;color:var(--fg2)"><div class="text-center"><div style="font-size:32px;margin-bottom:8px">&#9783;</div><p>' + t('ctr.select') + '</p></div></div>';
    }
    h += '</div></div>';
    b.innerHTML = h;
    if (cv) { const tb = document.getElementById('ctr-tab-content'); if (tb) await ctrRenderTab(tb, cv); }
  } catch (e) { b.innerHTML = '<p class="color-red">' + escapeHtml(e.message) + '</p>'; }
}


async function ctrRenderTab(tb, cv) {
  const n = cv.name;
  const on = cv.state === 'RUNNING';
  if (ctrTab === 'summary') {
    let m = {}; if (on) { try { const r = await fetchGet(EP.CTR_METRICS(n)); m = unwrapData(r); } catch (e) { if(_DEBUG) console.warn('ctrRenderTab:', e.message); } }
    let info = { hostname: n, os: '', uptime: '', procs: '', kernel: '' };
    if (on) { const cmds = [['hostname', 'hostname'], ['uptime', 'uptime -p'], ['nproc', 'nproc'], ['kernel', 'uname -r']];
      for (const [k, c] of cmds) { try { const r = await fetchPost(EP.CTR_EXEC(n), { command: c }); info[k] = unwrapData(r).output?.trim() || ''; } catch (e) { if(_DEBUG) console.warn('ctrRenderTab:', e.message); } } }
    const cpu = m.cpu_percent || 0, mem_u = m.mem_used_mb || 0, mem_l = m.mem_limit_mb || 0, mem_p = mem_l > 0 ? mem_u / mem_l * 100 : 0;
    const nrx = m.net_rx_mb || 0, ntx = m.net_tx_mb || 0;
    tb.innerHTML = H.grid(4,
      H.card('Status', '<div class="stat-lg" style="color:' + (on ? 'var(--green)' : 'var(--fg2)') + '">' + (on ? t('status.running') : t('status.stopped')) + '</div><div class="stat-label mt-4">' + (on ? 'PID: ' + (m.init_pid || '-') : '') + '</div>')
    + H.card('CPU', '<div class="stat-md">' + cpu.toFixed(1) + '%</div>' + renderProgressBar(cpu) + '<div class="stat-label">' + (info.nproc || '?') + ' cores</div>')
    + H.card('Memory', '<div class="stat-md">' + (mem_l > 0 ? mem_p.toFixed(1) + '%' : mem_u.toFixed(0) + ' MB') + '</div>' + (mem_l > 0 ? renderProgressBar(mem_p) : ''))
    + H.card('Network', H.row('RX', nrx.toFixed(1) + ' MB') + H.row('TX', ntx.toFixed(1) + ' MB') + H.row('IP', '<span class="color-accent">' + escapeHtml(cv.ip_addr || '-') + '</span>'))
    ) + '<div class="sg grid-2 mt-12">'
    + H.card('System Info', H.row('Hostname', escapeHtml(info.hostname || '-')) + H.row('Uptime', escapeHtml(info.uptime || '-')) + H.row('Kernel', escapeHtml(info.kernel || '-')) + H.row('Image', escapeHtml(cv.image || '-')))
    + H.card('Configuration', H.row('Bridge', 'pcvbr0') + H.row('AppArmor', 'unconfined') + H.row('Type', 'LXC (unprivileged: no)') + H.row('Node', location.hostname))
    + '</div>';
  } else if (ctrTab === 'console') {
    if (!on) { tb.innerHTML = H.card('&#9000; ' + t('tab.console'), '<p class="color-muted">' + t('ctr.console.stopped') + '</p>'); return; }
    tb.innerHTML = '<div style="background:var(--bg);border:1px solid var(--border);border-radius:var(--r);padding:0;font-family:monospace;display:flex;flex-direction:column;height:100%">'
    + '<div class="justify-between stat-label" style="padding:6px 10px;border-bottom:1px solid var(--border)"><span>&#9000; ' + n + ' — Shell</span><span class="color-green">' + t('connected') + '</span></div>'
    + '<pre id="ctr-output" style="flex:1;padding:8px 10px;margin:0;overflow-y:auto;font-size:11px;color:var(--green);white-space:pre-wrap;min-height:250px;max-height:400px">root@' + n + ':~# \n</pre>'
    + '<div class="flex" style="border-top:1px solid var(--border)"><span style="padding:6px 8px;color:var(--green);font-size:12px">$</span><input id="ctr-cmd" style="flex:1;background:transparent;border:none;color:var(--fg);font-family:monospace;font-size:12px;padding:6px 0;outline:none" placeholder="Type command..." onkeydown="if(event.key===\'Enter\')ctrRunCmd(\'' + n + '\')"></div></div>';
    setTimeout(() => { document.getElementById('ctr-cmd')?.focus(); }, 100);
  } else if (ctrTab === 'resources') {
    let info = { cpu: '', mem: '', disk: '', procs: '' };
    if (on) { for (const [k, c] of [['cpu', 'nproc'], ['mem', 'free -h | head -2'], ['disk', 'df -h / 2>/dev/null | tail -1'], ['procs', 'ps aux --no-headers 2>/dev/null | wc -l']]) { try { const r = await fetchPost(EP.CTR_EXEC(n), { command: c }); info[k] = unwrapData(r).output?.trim() || '-'; } catch (e) { info[k] = '-'; } } }
    const memLines = (info.mem || '').split('\n'); const memHeader = memLines[0] || ''; const memData = memLines[1] || '';
    tb.innerHTML = '<h3 class="section-title-md">Resources</h3><div class="sg">'
    + H.card('CPU', H.row('Cores', info.cpu || '-') + H.row('Type', 'host (KVM passthrough)'))
    + H.card('Memory', '<pre class="stat-label" style="margin:0;white-space:pre;overflow-x:auto">' + memHeader + '\n' + memData + '</pre>')
    + H.card('Disk (rootfs)', '<pre class="stat-label" style="margin:0;white-space:pre;overflow-x:auto">' + (info.disk || 'N/A') + '</pre>')
    + H.card('Processes', H.row('Running', info.procs || '-'))
    + '</div>'
    + '<h3 class="section-title-md mt-14">Resource Limits (cgroup v2)</h3>'
    + '<div class="sg grid-2">'
    + H.card('Set CPU Limit', '<div class="fr"><label>CPU Shares</label><input id="ctr-cpu-shares" type="number" value="1024"></div><div class="fr"><label>CPU Quota (µs)</label><input id="ctr-cpu-quota" type="number" value="100000"></div><button class="btn btn-g mt-8" onclick="ctrSetLimits(\'' + escapeHtml(n) + '\',\'cpu\')">Apply CPU Limit</button>')
    + H.card('Set Memory Limit', '<div class="fr"><label>Memory Limit (MB)</label><input id="ctr-mem-limit" type="number" value="512"></div><div class="fr"><label>Swap Limit (MB)</label><input id="ctr-swap-limit" type="number" value="0"></div><button class="btn btn-g mt-8" onclick="ctrSetLimits(\'' + escapeHtml(n) + '\',\'mem\')">Apply Memory Limit</button>')
    + '</div>';
  } else if (ctrTab === 'network') {
    tb.innerHTML = '<h3 class="section-title-md">Network Interfaces <button class="btn btn-g" style="font-size:10px;margin-left:8px" onclick="ctrNicAdd(\'' + escapeHtml(n) + '\')">+ Add NIC</button></h3><div id="ctr-nic-list"><span class="spinner"></span> Loading NICs...</div>';
    tb.innerHTML += '<h3 class="section-title-md mt-14">Bandwidth QoS</h3><div class="sg grid-2">'
    + H.card('Set Bandwidth Limit', '<div class="fr"><label>Interface</label><input id="ctr-bw-nic" value="eth0" class="w-80"></div><div class="fr"><label>Inbound (Kbps)</label><input id="ctr-bw-in" type="number" value="0" placeholder="0 = unlimited"></div><div class="fr"><label>Outbound (Kbps)</label><input id="ctr-bw-out" type="number" value="0" placeholder="0 = unlimited"></div><button class="btn btn-g mt-8" onclick="ctrSetBandwidth(\'' + escapeHtml(n) + '\')">Apply QoS</button>')
    + H.card('Routing &amp; Addresses', '<div id="ctr-net-info"><span class="spinner"></span></div>')
    + '</div>';

    try {
      const r = await fetchGet(EP.CTR_NICS(n));
      const nics = unwrapList(r);
      const el = document.getElementById('ctr-nic-list');
      if (el) {
        if (nics.length === 0) { el.innerHTML = '<p class="color-muted">No NICs configured</p>'; }
        else {
          let h = '<table class="table-sticky"><thead><tr><th>Name</th><th>Type</th><th>Bridge</th><th>MAC</th><th>IPv4</th><th>Actions</th></tr></thead><tbody>';
          nics.forEach(nc => {
            h += '<tr><td><b>' + escapeHtml(nc.name || '-') + '</b></td><td>' + escapeHtml(nc.type || 'veth') + '</td><td>' + H.badge(escapeHtml(nc.bridge || '-'), 'y') + '</td><td class="text-xs">' + escapeHtml(nc.hwaddr || 'auto') + '</td><td class="color-accent">' + escapeHtml(nc.ipv4 || '-') + '</td><td>';
            if (nc.name !== 'eth0') h += '<button class="btn btn-r" style="font-size:9px;padding:2px 6px" onclick="ctrNicDel(\'' + escapeHtml(n) + '\',\'' + escapeHtml(nc.name) + '\')">Remove</button>';
            else h += '<span class="color-muted text-xs">primary</span>';
            h += '</td></tr>';
          });
          h += '</tbody></table>';
          el.innerHTML = h;
        }
      }
    } catch (e) {
      const el = document.getElementById('ctr-nic-list');
      if (el) el.innerHTML = H.card('Interface eth0', H.row('IP Address', '<span class="color-accent">' + escapeHtml(cv.ip_addr || '-') + '</span>') + H.row('Bridge', 'pcvbr0') + H.row('Type', 'veth'));
    }

    if (on) {
      try { const r = await fetchPost(EP.CTR_EXEC(n), { command: 'ip -4 addr show 2>/dev/null; echo "---"; ip route 2>/dev/null | head -5' }); const ni = document.getElementById('ctr-net-info'); if (ni) ni.innerHTML = '<pre class="stat-label" style="margin:0;white-space:pre-wrap;overflow-x:auto">' + escapeHtml(unwrapData(r).output || '') + '</pre>'; } catch (e) { const ni = document.getElementById('ctr-net-info'); if (ni) ni.innerHTML = '<span class="color-muted">Unable to fetch</span>'; }
    } else {
      const ni = document.getElementById('ctr-net-info'); if (ni) ni.innerHTML = '<span class="color-muted">Container is stopped</span>';
    }
  } else if (ctrTab === 'dns') {
    let dns = ''; if (on) { try { const r = await fetchPost(EP.CTR_EXEC(n), { command: 'cat /etc/resolv.conf 2>/dev/null' }); dns = unwrapData(r).output || ''; } catch (e) { if(_DEBUG) console.warn('dns:', e.message); } }
    tb.innerHTML = '<h3 class="section-title-md">DNS</h3>' + H.card('Resolver Configuration',
    (on ? '<pre style="font-size:11px;color:var(--fg);margin:8px 0;white-space:pre-wrap;background:var(--bg);padding:10px;border-radius:4px;border:1px solid var(--border)">' + dns.replace(/</g, '&lt;') + '</pre>'
    + '<div class="mt-8"><div class="fr"><label>Add Nameserver</label><input id="dns-ns" placeholder="8.8.8.8" class="flex-1"><button class="btn btn-g" style="margin-left:6px" onclick="ctrDnsAdd(\'' + n + '\')">Add</button></div></div>'
    : '<p class="color-muted">' + t('ctr.console.stopped') + '</p>'));
  } else if (ctrTab === 'options') {
    tb.innerHTML = '<h3 class="section-title-md">Options</h3><div class="sg">'
    + H.card('General', H.row('Start on boot', 'No') + H.row('Start order', '---') + H.row('Protection', 'No') + H.row('Unprivileged', 'No'))
    + H.card('Security', H.row('AppArmor', 'unconfined') + H.row('Keyctl', 'No') + H.row('Nesting', 'No') + H.row('FUSE', 'No'))
    + H.card('Signals', H.row('Halt', 'SIGRTMIN+3') + H.row('Reboot', 'SIGTERM'))
    + '</div>';
  } else if (ctrTab === 'snapshots') {
    tb.innerHTML = '<h3 class="section-title-md">' + t('vm.snapshot') + ' <button class="btn btn-g" style="font-size:10px;margin-left:8px" onclick="ctrSnapCreate(\'' + escapeHtml(n) + '\')">+ ' + t('btn.create') + '</button></h3><div id="ctr-snap-list"><span class="spinner"></span> ' + t('loading') + '</div>';
    try {
      const r = await fetchGet(EP.CTR_SNAPSHOTS(n)).catch(() => ({ data: [] }));
      const sl = unwrapList(r);
      let sh = '<table class="table-sticky"><thead><tr><th>Name</th><th>' + t('vm.settings') + '</th></tr></thead><tbody>';
      if (sl.length === 0) sh += '<tr><td colspan="2" class="color-muted">' + t('snap.none') + '</td></tr>';
      sl.forEach(s => { const sn = typeof s === 'string' ? s : (s.name || s); sh += '<tr><td>' + escapeHtml(sn) + '</td><td><button class="btn text-9" onclick="ctrSnapRb(\'' + escapeHtml(n) + '\',\'' + escapeHtml(sn) + '\')">Rollback</button> <button class="btn btn-r text-9" onclick="ctrSnapDel(\'' + escapeHtml(n) + '\',\'' + escapeHtml(sn) + '\')">' + t('btn.delete') + '</button></td></tr>'; });
      sh += '</tbody></table>'; document.getElementById('ctr-snap-list').innerHTML = sh;
    } catch (e) { document.getElementById('ctr-snap-list').innerHTML = '<p class="color-muted">' + t('snap.none') + '</p>'; }
  } else if (ctrTab === 'notes') {
    tb.innerHTML = '<h3 class="section-title-md">Notes</h3>' + H.card('Container Notes', '<textarea id="ctr-notes" style="width:100%;min-height:150px;background:var(--bg);border:1px solid var(--border);border-radius:4px;color:var(--fg);padding:10px;font-family:monospace;font-size:12px;resize:vertical" placeholder="Add notes...">' + escapeHtml(localStorage.getItem('ctr-note-' + n) || '') + '</textarea><button class="btn mt-8" onclick="localStorage.setItem(\'ctr-note-' + escapeHtml(n) + '\',document.getElementById(\'ctr-notes\').value);toast(\'' + t('btn.save') + '\')">' + t('btn.save') + '</button>');
  } else if (ctrTab === 'tasks') {
    tb.innerHTML = '<h3 class="section-title-md">Task History</h3>' + H.card('Recent Events', '<div style="max-height:300px;overflow-y:auto;font-size:11px;font-family:monospace;color:var(--accent)">' + eventLog.filter(e => { var s = (e.msg || e.raw || String(e)).toLowerCase(); return s.includes('ctr') || s.includes(n.toLowerCase()); }).map(e => '<div style="padding:2px 0;border-bottom:1px solid var(--border)">' + escapeHtml(e.msg || e.raw || String(e)) + '</div>').join('') + '<div class="color-muted" style="padding:4px 0">' + eventLog.length + ' total events</div></div>');
  }
}


async function ctrA(n, a) {
  showModal('<h2>' + (a === 'start' ? '&#9654; ' + t('ctr.starting') : '&#9632; ' + t('ctr.stopping')) + '</h2><p class="mb-8"><b class="color-accent">' + n + '</b></p><div class="prog-bar"><div class="prog-fill" id="ctr-prog" class="w-pct-10"></div></div><div class="prog-status" id="ctr-st"><span class="spinner"></span>Sending ' + a + ' command...</div>');
  const pf = document.getElementById('ctr-prog'), ps = document.getElementById('ctr-st');
  try {
    pf.style.width = '30%'; ps.innerHTML = '<span class="spinner"></span>Waiting for container ' + a + '...';
    const d = await fetchPost(a === 'start' ? EP.CTR_START(n) : EP.CTR_STOP(n), {});
    pf.style.width = '60%';
    if (d.error) { pf.style.background = 'var(--red)'; pf.style.width = '100%'; ps.innerHTML = '&#10060; ' + escapeHtml(a) + ' failed: ' + escapeHtml(d.error.message || 'Unknown error'); toast(a + ' failed', false); return; }
    ps.innerHTML = '<span class="spinner"></span>' + escapeHtml(a) + ' completed, refreshing...';
    if (a === 'start') {
      for (let i = 0; i < 8; i++) { pf.style.width = (65 + i * 4) + '%'; await new Promise(r => setTimeout(r, 1500));
        try { const c = await fetchGet(EP.CTR_LIST()); const l = unwrapList(c); const ct = l.find(x => x.name === n);
          if (ct && ct.state === 'RUNNING') { const ip = ct.ip_addr || ct.ip || '';
            if (ip && ip !== 'N/A') { pf.style.width = '100%'; ps.innerHTML = '&#9989; Running — IP: <b class="color-green">' + escapeHtml(ip) + '</b>'; toast(n + ' started (' + ip + ')'); addEvt('LXC Started — ' + n + ', IP: ' + ip); setTimeout(closeModal, 2500); renderContainers(document.getElementById('cb')); return; }
            ps.innerHTML = '<span class="spinner"></span>Running, waiting for DHCP IP... (' + (i + 1) + '/8)'; } } catch (e) { if(_DEBUG) console.warn('c:', e.message); } }
      pf.style.width = '100%'; ps.innerHTML = '&#9989; Running (IP pending)'; toast(n + ' started'); addEvt('LXC Started — ' + n + ' (IP pending)');
    } else { pf.style.width = '100%'; ps.innerHTML = '&#9989; Container stopped'; toast(n + ' stopped'); addEvt('LXC Stopped — ' + n); }
    setTimeout(() => { closeModal(); renderContainers(document.getElementById('cb')); }, 2e3);
  } catch (e) { if (pf) pf.style.width = '100%'; if (ps) ps.innerHTML = '&#9989; ' + a + ' requested'; toast(n + ' ' + a); addEvt('LXC ' + a + ' — ' + n); setTimeout(() => { closeModal(); renderContainers(document.getElementById('cb')); }, 2500); }
}

async function ctrRunCmd(n) {
  const inp = document.getElementById('ctr-cmd'); const out = document.getElementById('ctr-output');
  if (!inp || !out) return; const cmd = inp.value.trim(); if (!cmd) return;
  ctrHist.push(cmd); out.textContent += '$ ' + cmd + '\n'; out.scrollTop = out.scrollHeight; inp.value = '';
  try { const r = await fetchPost(EP.CTR_EXEC(n), { command: cmd }); const d = unwrapData(r); out.textContent += (d.output || d.stdout || '(no output)') + '\n'; out.scrollTop = out.scrollHeight; } catch (e) { out.textContent += 'Error: ' + e.message + '\n'; out.scrollTop = out.scrollHeight; }
}

async function ctrDnsAdd(n) { const ns = document.getElementById('dns-ns')?.value; if (!ns) return;
  try { await fetchPost(EP.CTR_EXEC(n), { command: 'echo "nameserver ' + ns + '" >> /etc/resolv.conf' }); toast('Nameserver added'); ctrTab = 'dns'; renderContainers(document.getElementById('cb')); } catch (e) { toast(e.message, false); } }

async function ctrReboot(n) {
  showModal('<h2>&#128260; Rebooting</h2><p><b class="color-accent">' + esc(n) + '</b></p><div class="prog-bar"><div class="prog-fill" id="cr-p" class="w-pct-10"></div></div><div class="prog-status" id="cr-s"><span class="spinner"></span> Stopping...</div>');
  var pf = document.getElementById('cr-p'), ps = document.getElementById('cr-s');
  try {
    if (pf) pf.style.width = '30%';
    await fetchPost(EP.CTR_STOP(n), {});
    if (pf) pf.style.width = '50%'; if (ps) ps.innerHTML = '<span class="spinner"></span> Waiting...';
    await new Promise(function(r) { setTimeout(r, 2000); });
    if (pf) pf.style.width = '70%'; if (ps) ps.innerHTML = '<span class="spinner"></span> Starting...';
    await fetchPost(EP.CTR_START(n), {});
    if (pf) pf.style.width = '100%'; if (ps) ps.innerHTML = '&#9989; Reboot complete';
    toast(n + ' rebooted'); addEvt('LXC Reboot — ' + n);
    setTimeout(function() { closeModal(); renderContainers(document.getElementById('cb')); }, 2000);
  } catch (e) {
    if (pf) { pf.style.width = '100%'; pf.style.background = 'var(--red)'; }
    if (ps) ps.innerHTML = '&#10060; Reboot error: ' + esc(e.message);
    toast(t('msg.reboot_error'), false);
  }
}

async function ctrSnapCreate(n) { var s = await showInputModal(t('snap.name_prompt') || 'Snapshot name', t('snap.name_prompt') || 'Name', 'snap-' + Date.now()); if (!s) return;
  showModal('<h2>&#128247; ' + t('snap.created') + '</h2><p><b class="color-accent">' + esc(n) + '@' + esc(s) + '</b></p><div class="prog-bar"><div class="prog-fill" id="cs-p" class="w-pct-20"></div></div><div class="prog-status" id="cs-s"><span class="spinner"></span> Creating snapshot...</div>');
  var pf = document.getElementById('cs-p'), ps = document.getElementById('cs-s');
  try {
    if (pf) pf.style.width = '60%';
    await fetchPost(EP.CTR_SNAPSHOTS(n), { snap_name: s });
    if (pf) pf.style.width = '100%'; if (ps) ps.innerHTML = '&#9989; ' + t('snap.created') + ': ' + esc(s);
    toast(t('snap.created') + ': ' + s); addEvt('LXC Snapshot created — ' + n + '@' + s);
    setTimeout(function() { closeModal(); ctrTab = 'snapshots'; renderContainers(document.getElementById('cb')); }, 1500);
  } catch (e) {
    if (pf) { pf.style.width = '100%'; pf.style.background = 'var(--red)'; }
    if (ps) ps.innerHTML = '&#10060; ' + esc(e.message); toast(e.message, false);
  }
}
async function ctrSnapRb(n, s) { if (!await customConfirm('Rollback', n + ' → ' + s + '?')) return;
  showModal('<h2>&#9194; Rollback</h2><p><b class="color-accent">' + esc(n) + '@' + esc(s) + '</b></p><div class="prog-bar"><div class="prog-fill" id="crb-p" class="w-pct-20"></div></div><div class="prog-status" id="crb-s"><span class="spinner"></span> Rolling back...</div>');
  var pf = document.getElementById('crb-p'), ps = document.getElementById('crb-s');
  try {
    if (pf) pf.style.width = '60%';
    await fetchPost(EP.CTR_SNAP_ROLLBACK(n), { snap_name: s });
    if (pf) pf.style.width = '100%'; if (ps) ps.innerHTML = '&#9989; ' + t('snap.reverted');
    toast(t('snap.reverted')); addEvt('LXC Snapshot rollback — ' + n + '@' + s);
    setTimeout(function() { closeModal(); renderContainers(document.getElementById('cb')); }, 1500);
  } catch (e) {
    if (pf) { pf.style.width = '100%'; pf.style.background = 'var(--red)'; }
    if (ps) ps.innerHTML = '&#10060; ' + esc(e.message); toast(e.message, false);
  }
}
async function ctrSnapDel(n, s) { if (!await customConfirm(t('btn.delete'), s + '?')) return;
  showModal('<h2>&#128465; ' + t('snap.deleted') + '</h2><p><b class="color-accent">' + esc(n) + '@' + esc(s) + '</b></p><div class="prog-bar"><div class="prog-fill" id="csd-p" class="w-pct-20"></div></div><div class="prog-status" id="csd-s"><span class="spinner"></span> Deleting...</div>');
  var pf = document.getElementById('csd-p'), ps = document.getElementById('csd-s');
  try {
    if (pf) pf.style.width = '60%';
    await fetchDelete(EP.CTR_SNAP_DELETE(n, s));
    if (pf) pf.style.width = '100%'; if (ps) ps.innerHTML = '&#9989; ' + t('snap.deleted');
    toast(t('snap.deleted')); addEvt('LXC Snapshot deleted — ' + n + '@' + s);
    setTimeout(function() { closeModal(); ctrTab = 'snapshots'; renderContainers(document.getElementById('cb')); }, 1500);
  } catch (e) {
    if (pf) { pf.style.width = '100%'; pf.style.background = 'var(--red)'; }
    if (ps) ps.innerHTML = '&#10060; ' + esc(e.message); toast(e.message, false);
  }
}

async function ctrExec(n) {
  try { const c = await fetchGet(EP.CTR_LIST()); const l = unwrapList(c); const ct = l.find(x => x.name === n);
    if (!ct || ct.state !== 'RUNNING') { toast(n + ' is not running', false); return; } } catch (e) { if(_DEBUG) console.warn('c:', e.message); }
  selCtr = n; ctrTab = 'console'; renderContainers(document.getElementById('cb'));
}

function ctrDel(n) { showModal('<h2 class="color-red">&#9888; ' + t('ctr.destroying') + '</h2><p class="mb-12">' + t('ctr.delete.confirm') + ' <b class="color-accent">' + n + '</b></p><p class="mb-12">' + t('ctr.delete.type_name') + '</p><div class="fr"><label>Name</label><input id="del-ctr-confirm" placeholder="' + n + '"></div><div class="text-right mt-14"><button class="btn btn-r" onclick="doCtrDel(\'' + n + '\')">' + t('btn.delete') + '</button> <button class="btn" onclick="closeModal()">' + t('btn.cancel') + '</button></div>'); }

async function doCtrDel(n) {
  const c = document.getElementById('del-ctr-confirm')?.value; if (c !== n) { toast(t('vm.name_mismatch'), false); return; }
  const mc = document.getElementById('mc'); mc.innerHTML = '<h2 class="color-red">&#9888; ' + t('ctr.destroying') + '</h2><p><b class="color-accent">' + escapeHtml(n) + '</b></p><div class="prog-bar"><div class="prog-fill" id="dc-p" class="w-pct-10"></div></div><div class="prog-status" id="dc-s"><span class="spinner"></span>Stopping...</div>';
  const pf = document.getElementById('dc-p'), ps = document.getElementById('dc-s');
  try { pf.style.width = '30%'; ps.innerHTML = '<span class="spinner"></span>Destroying...';
    const d = await fetchDelete(EP.CTR_DETAIL(n)).catch(() => ({}));
    pf.style.width = '80%';
    if (d.error) { pf.style.background = 'var(--red)'; pf.style.width = '100%'; ps.innerHTML = '&#10060; ' + escapeHtml(d.error.message); toast(t('btn.delete') + ' failed', false); return; }
    pf.style.width = '100%'; ps.innerHTML = '&#9989; ' + t('ctr.destroyed'); toast(t('ctr.destroyed')); addEvt('LXC Destroyed — ' + n); selCtr = null; setTimeout(() => { closeModal(); renderContainers(document.getElementById('cb')); }, 1500);
  } catch (e) { pf.style.width = '100%'; ps.innerHTML = '&#10060; ' + escapeHtml(e.message); toast(e.message, false); }
}


function showCtrCreate() {
  if (typeof markFormDirty === 'function') markFormDirty('ctr-create');
  let h = '<h2>' + t('ctr.new') + '</h2>';

  h += '<div style="display:grid;grid-template-columns:1fr 1fr;gap:16px">';

  h += '<div>';
  h += '<h4 class="mb-8">&#9783; Basic</h4>';
  h += '<div class="fr"><label class="min-w-80">Name</label><input id="cc-name" placeholder="my-container" class="flex-1"></div>';
  h += '<div class="fr"><label class="min-w-80">Distribution</label><select id="cc-dist" onchange="ctrDistChanged()" style="flex:1;padding:6px;background:var(--bg);border:1px solid var(--border);color:var(--fg);border-radius:4px"><option value="ubuntu">Ubuntu</option><option value="debian">Debian</option><option value="alpine">Alpine</option><option value="centos">CentOS</option><option value="fedora">Fedora</option><option value="archlinux">Arch Linux</option></select></div>';
  h += '<div class="fr"><label class="min-w-80">Release</label><input id="cc-rel" value="jammy" placeholder="jammy / bookworm / 3.19" class="flex-1"></div>';
  h += '</div>';

  h += '<div>';
  h += '<h4 class="mb-8">&#127760; Network</h4>';
  h += '<div class="fr"><label class="min-w-70">Bridge</label><div class="flex gap-6 flex-1"><select id="cc-br" style="flex:1;padding:6px;background:var(--bg);border:1px solid var(--border);color:var(--fg);border-radius:4px"><option value="pcvbr0">pcvbr0 (default)</option></select><button class="btn text-xs" onclick="ctrLoadBridges()">&#128260;</button></div></div>';
  h += '<div class="fr"><label class="min-w-70">IP Mode</label><select id="cc-ipmode" onchange="ctrIpModeChanged()" style="flex:1;padding:6px;background:var(--bg);border:1px solid var(--border);color:var(--fg);border-radius:4px"><option value="dhcp">DHCP (auto)</option><option value="static">Static IP</option></select></div>';
  h += '<div class="fr hidden" id="cc-static-row"><label class="min-w-70">Static IP</label><input id="cc-ip" placeholder="10.0.3.100/24" class="flex-1"></div>';
  h += '<div class="fr hidden" id="cc-gw-row"><label class="min-w-70">Gateway</label><input id="cc-gw" placeholder="10.0.3.1" class="flex-1"></div>';
  h += '<div class="fr"><label class="min-w-70">DNS</label><input id="cc-dns" placeholder="8.8.8.8 (optional)" class="flex-1"></div>';
  h += '<h4 style="margin:12px 0 8px">&#9881; Resources</h4>';
  h += '<div class="fr"><label class="min-w-70">vCPU</label><input id="cc-vcpu" type="number" value="1" min="1" max="64" class="flex-1"></div>';
  h += '<div class="fr"><label class="min-w-70">Memory (MB)</label><input id="cc-mem" type="number" value="512" min="64" class="flex-1"></div>';
  h += '</div></div>';
  h += '<div class="text-right mt-14"><button class="btn btn-g" onclick="doCtrCreate()">' + t('btn.create') + '</button> <button class="btn" onclick="closeModal()">' + t('btn.cancel') + '</button></div>';

  showModal(h);
  var mc = document.getElementById('mc');
  if (mc) mc.classList.add('modal-wide');
  setTimeout(ctrLoadBridges, 80);
}

function ctrDistChanged() {
  const dist = document.getElementById('cc-dist')?.value;
  const rel = document.getElementById('cc-rel');
  if (!rel) return;
  const defaults = { ubuntu: 'jammy', debian: 'bookworm', alpine: '3.19', centos: '9-Stream', fedora: '39', archlinux: 'current' };
  rel.value = defaults[dist] || '';
}

function ctrIpModeChanged() {
  const mode = document.getElementById('cc-ipmode')?.value;
  document.getElementById('cc-static-row')?.classList.toggle('hidden', mode !== 'static');
  document.getElementById('cc-gw-row')?.classList.toggle('hidden', mode !== 'static');
}

async function ctrLoadBridges() {
  const sel = document.getElementById('cc-br'); if (!sel) return;
  try {
    const r = await fetchGet(EP.NET_LIST());
    const nl = unwrapList(r);
    let h = '';
    nl.forEach(n => { const name = n.name || ''; if (!name) return; const mode = n.mode || ''; const ip = n.ip_cidr || '';
      h += '<option value="' + escapeHtml(name) + '"' + (name === 'pcvbr0' ? ' selected' : '') + '>' + escapeHtml(name) + (mode ? ' (' + mode + ')' : '') + (ip ? ' ' + ip : '') + '</option>'; });
    if (!h) h = '<option value="pcvbr0">pcvbr0</option>';
    sel.innerHTML = h;
  } catch (e) { sel.innerHTML = '<option value="pcvbr0">pcvbr0</option>'; }
}

async function doCtrCreate() {
  const n = document.getElementById('cc-name')?.value?.trim();
  const dist = document.getElementById('cc-dist')?.value;
  const rel = document.getElementById('cc-rel')?.value;
  const br = document.getElementById('cc-br')?.value;
  const vcpu = parseInt(document.getElementById('cc-vcpu')?.value) || 1;
  const mem = parseInt(document.getElementById('cc-mem')?.value) || 512;
  const ipmode = document.getElementById('cc-ipmode')?.value;
  const ip = document.getElementById('cc-ip')?.value;
  const gw = document.getElementById('cc-gw')?.value;
  const dns = document.getElementById('cc-dns')?.value;
  if (!n) { toast(t('msg.name_required'), false); return; }


  if (typeof clearFormDirty === 'function') clearFormDirty('ctr-create');
  closeModal();
  toast('&#9783; ' + escapeHtml(n) + ' ' + _L('생성 시작...', 'Creating...'), 's');

  const body = { name: n, image: dist + ':' + rel, bridge: br || 'pcvbr0', vcpu_count: vcpu, memory_mb: mem };
  if (ipmode === 'static' && ip) { body.static_ip = ip; if (gw) body.gateway = gw; }
  if (dns) body.dns = dns;

  try {
    const r = await fetchPost(EP.CTR_LIST(), body);


    if (r && r.error) {
      var errMsg = r.error.message || r.error.data || JSON.stringify(r.error);
      toast('&#10060; ' + _L('컨테이너 생성 실패', 'Container creation failed') + ': ' + errMsg, false);
      return;
    }


    addEvt('LXC Creating — ' + n + ' (' + dist + ':' + rel + ', ' + br + ', ' + vcpu + 'vCPU, ' + mem + 'MB)');
    var created = false;
    for (var pi = 0; pi < 18; pi++) {
      await new Promise(function(res) { setTimeout(res, 5000); });
      try {
        var cl = await fetchGet(EP.CTR_LIST()); var list = unwrapList(cl);
        if (list.find(function(x) { return x.name === n; })) { created = true; break; }
      } catch(e2) {}
    }

    if (created) {
      toast('&#9989; ' + escapeHtml(n) + ' ' + _L('생성 완료', 'created'), 's');
      addEvt('LXC Created — ' + n);
      selCtr = n; ctrTab = 'summary';
      renderContainerList();
      if (document.getElementById('cb')) renderContainers(document.getElementById('cb'));
    } else {
      toast('&#9888; ' + escapeHtml(n) + ' ' + _L('생성 진행 중 — 잠시 후 확인하세요', 'Still creating — check later'), 'w');
    }
  } catch (e) {
    var errDetail = e && e.message ? e.message : '';
    toast('&#10060; ' + _L('컨테이너 생성 실패', 'Container creation failed') + (errDetail ? ': ' + errDetail : ''), false);
  }
}


async function ctrSetLimits(name, type) {
  const body = { name };
  if (type === 'cpu') {
    body.cpu_shares = parseInt(document.getElementById('ctr-cpu-shares')?.value) || 1024;
    body.cpu_quota = parseInt(document.getElementById('ctr-cpu-quota')?.value) || 100000;
  } else {
    body.memory_limit_mb = parseInt(document.getElementById('ctr-mem-limit')?.value) || 512;
    body.swap_limit_mb = parseInt(document.getElementById('ctr-swap-limit')?.value) || 0;
  }
  try {
    const r = await fetchPut(EP.CTR_LIMITS(name), body);
    if (r.error) { toast('Set limits failed: ' + (r.error.message || ''), false); return; }
    toast('Resource limits applied: ' + name); addEvt('CTR limits: ' + name);
  } catch (e) { toast(e.message, false); }
}


function ctrNicAdd(name) {
  showModal('<h2>Add NIC to ' + escapeHtml(name) + '</h2><div class="fr"><label>Bridge</label><input id="ctr-nic-br" value="pcvbr0" placeholder="pcvbr0"></div><div class="fr"><label>MAC (optional)</label><input id="ctr-nic-mac" placeholder="auto"></div><div class="text-right mt-12"><button class="btn btn-g" onclick="doCtrNicAdd(\'' + escapeHtml(name) + '\')">Add NIC</button> <button class="btn btn-r" onclick="closeModal()">' + t('btn.cancel') + '</button></div>');
}

async function doCtrNicAdd(name) {
  const bridge = document.getElementById('ctr-nic-br')?.value || 'pcvbr0';
  const mac = document.getElementById('ctr-nic-mac')?.value || '';
  try {
    const r = await fetchPost(EP.CTR_NICS(name), { bridge, hwaddr: mac || undefined });
    if (r.error) { toast('NIC add failed: ' + (r.error.message || ''), false); return; }
    toast('NIC added to ' + name + ' (bridge: ' + bridge + ')');
    addEvt('LXC NIC attached — ' + name + ' → ' + bridge);
    closeModal(); ctrTab = 'network'; renderContainers(document.getElementById('cb'));
  } catch (e) { toast(e.message, false); }
}

async function ctrNicDel(name, nicName) {
  if (!await customConfirm('Remove NIC', 'Remove ' + nicName + ' from ' + name + '?')) return;
  try {
    const r = await fetchDelete(EP.CTR_NICS(name) + '/' + encodeURIComponent(nicName));
    if (r.error) { toast('NIC remove failed: ' + (r.error.message || ''), false); return; }
    toast('NIC removed: ' + nicName);
    addEvt('LXC NIC detached — ' + name + '/' + nicName);
    ctrTab = 'network'; renderContainers(document.getElementById('cb'));
  } catch (e) { toast(e.message, false); }
}

async function ctrSetBandwidth(name) {
  const nic = document.getElementById('ctr-bw-nic')?.value || 'eth0';
  const inKbps = parseInt(document.getElementById('ctr-bw-in')?.value) || 0;
  const outKbps = parseInt(document.getElementById('ctr-bw-out')?.value) || 0;
  if (inKbps <= 0 && outKbps <= 0) { toast('Set at least one bandwidth limit', false); return; }
  try {
    const r = await fetchPut(EP.CTR_BANDWIDTH(name), { nic_name: nic, inbound_kbps: inKbps, outbound_kbps: outKbps });
    if (r.error) { toast('Bandwidth set failed: ' + (r.error.message || ''), false); return; }
    toast('Bandwidth QoS applied: ' + name + '/' + nic + ' (in: ' + inKbps + ' out: ' + outKbps + ' Kbps)');
    addEvt('LXC Bandwidth QoS — ' + name + '/' + nic + ' in:' + inKbps + ' out:' + outKbps + ' Kbps');
  } catch (e) { toast(e.message, false); }
}




window.setCtrSort = setCtrSort;
window.renderContainerList = renderContainerList;
window.renderContainers = renderContainers;
window.ctrRenderTab = ctrRenderTab;
window.ctrA = ctrA;
window.ctrRunCmd = ctrRunCmd;
window.ctrDnsAdd = ctrDnsAdd;
window.ctrReboot = ctrReboot;
window.ctrSnapCreate = ctrSnapCreate;
window.ctrSnapRb = ctrSnapRb;
window.ctrSnapDel = ctrSnapDel;
window.ctrExec = ctrExec;
window.ctrDel = ctrDel;
window.doCtrDel = doCtrDel;
window.showCtrCreate = showCtrCreate;
window.ctrDistChanged = ctrDistChanged;
window.ctrIpModeChanged = ctrIpModeChanged;
window.ctrLoadBridges = ctrLoadBridges;
window.doCtrCreate = doCtrCreate;
window.ctrSetLimits = ctrSetLimits;
window.ctrNicAdd = ctrNicAdd;
window.doCtrNicAdd = doCtrNicAdd;
window.ctrNicDel = ctrNicDel;
window.ctrSetBandwidth = ctrSetBandwidth;


async function showCtrClone(name) {
  var html = '<div class="form-group"><label>' + _L('원본', 'Source') + '</label>';
  html += '<input class="input-field" value="' + esc(name) + '" disabled></div>';
  html += '<div class="form-group"><label>' + _L('클론 이름', 'Clone Name') + '</label>';
  html += '<input id="ctr-clone-name" class="input-field" placeholder="' + esc(name) + '-clone"></div>';
  showModal(_L('컨테이너 클론', 'Clone Container'), html, async function() {
    var dst = document.getElementById('ctr-clone-name').value.trim();
    if (!dst) { toast(_L('이름 필수', 'Name required'), 'w'); return; }
    try {
      await fetchPost(EP.CTR_CLONE(name), { source: name, dest: dst });
      toast(_L('클론 요청 완료', 'Clone requested'), 's');
    } catch(e) { toast(_L('실패', 'Failed'), 'e'); }
  });
}


async function showCtrMemoryStats(name) {
  try {
    var r = await fetchGet(EP.CTR_MEMORY_STATS(name));
    var d = unwrapData(r);
    var h = '<h4>' + esc(name) + ' — ' + _L('메모리 상세', 'Memory Details') + '</h4>';
    h += '<table class="data-table text-11"><thead><tr>';
    h += '<th>' + _L('항목', 'Field') + '</th><th>' + _L('값', 'Value') + '</th></tr></thead><tbody>';
    var fields = ['anon', 'file', 'slab', 'sock', 'shmem', 'pgfault', 'pgmajfault'];
    fields.forEach(function(f) {
      if (d[f] !== undefined) {
        var val = d[f] > 1048576 ? (d[f] / 1048576).toFixed(1) + ' MB' : d[f];
        h += '<tr><td><b>' + esc(f) + '</b></td><td>' + val + '</td></tr>';
      }
    });
    Object.keys(d).forEach(function(k) {
      if (fields.indexOf(k) === -1 && k !== 'container') {
        h += '<tr><td class="color-muted">' + esc(k) + '</td><td>' + d[k] + '</td></tr>';
      }
    });
    h += '</tbody></table>';
    showModal(_L('메모리 상세', 'Memory Stats'), h);
  } catch(e) { toast(_L('로드 실패', 'Failed'), 'e'); }
}


async function checkCtrHealth(name) {
  try {
    var r = await fetchGet(EP.CTR_HEALTH(name));
    var d = unwrapData(r);
    var icon = d.running ? '🟢' : '🔴';
    toast(icon + ' ' + esc(name) + ': ' + (d.state || 'unknown'), d.running ? 's' : 'w');
  } catch(e) { toast(_L('헬스 체크 실패', 'Health check failed'), 'e'); }
}


window.showCtrClone = showCtrClone;
window.showCtrMemoryStats = showCtrMemoryStats;
window.checkCtrHealth = checkCtrHealth;


PCV.container = {
  setCtrSort: setCtrSort,
  renderContainerList: renderContainerList,
  renderContainers: renderContainers,
  ctrRenderTab: ctrRenderTab,
  ctrA: ctrA,
  ctrRunCmd: ctrRunCmd,
  ctrDnsAdd: ctrDnsAdd,
  ctrReboot: ctrReboot,
  ctrSnapCreate: ctrSnapCreate,
  ctrSnapRb: ctrSnapRb,
  ctrSnapDel: ctrSnapDel,
  ctrExec: ctrExec,
  ctrDel: ctrDel,
  doCtrDel: doCtrDel,
  showCtrCreate: showCtrCreate,
  ctrDistChanged: ctrDistChanged,
  ctrIpModeChanged: ctrIpModeChanged,
  ctrLoadBridges: ctrLoadBridges,
  doCtrCreate: doCtrCreate,
  ctrSetLimits: ctrSetLimits,
  ctrNicAdd: ctrNicAdd,
  doCtrNicAdd: doCtrNicAdd,
  ctrNicDel: ctrNicDel,
  ctrSetBandwidth: ctrSetBandwidth,
  showCtrClone: showCtrClone,
  showCtrMemoryStats: showCtrMemoryStats,
  checkCtrHealth: checkCtrHealth
};

})(window.PCV);

window.showCtrClone = showCtrClone;
window.showCtrMemoryStats = showCtrMemoryStats;
window.checkCtrHealth = checkCtrHealth;






window.PCV = window.PCV || {};
(function(PCV) {

async function renderNetworks(b) {
  b.innerHTML = showSkeleton();
  try {
    const n = await fetchGet(EP.NET_LIST());
    const l = unwrapList(n);
    const compactMode = !!(window.matchMedia && window.matchMedia('(max-width: 768px)').matches);
    let h = '<div class="ops-section-heading"><div><h3>' + _L('네트워크 인벤토리', 'Network inventory') + '</h3><p>' + _L('브리지, DHCP, 외부 연결 상태를 먼저 확인한 뒤 정책 편집으로 이어갑니다.', 'Review bridges, DHCP, and external connectivity first, then move into policy editing.') + '</p></div><button class="btn btn-primary" onclick="showNetCreate()">+ ' + t('net.new') + '</button></div>';
    if (l.length === 0) { b.innerHTML = h + '<div class="empty-state" style="text-align:center;padding:40px 20px"><div style="font-size:48px;margin-bottom:12px;opacity:.5">&#127760;</div><div style="font-size:14px;color:var(--fg2);margin-bottom:16px">' + _L('구성된 네트워크가 없습니다', 'No configured networks') + '</div><button class="btn btn-primary" onclick="showNetCreate()" class="text-12">+ ' + _L('네트워크 생성', 'Create network') + '</button></div>'; return; }
    if (compactMode) {
      h += '<div class="sg grid-2">';
      l.forEach(v => {
        const ext = v.mode === 'bridge' ? (escapeHtml(v.phys) || '-') : v.mode === 'nat' ? 'NAT' : '-';
        h += H.card('<b>' + escapeHtml(v.name) + '</b>', H.row(_L('모드', 'Mode'), H.badge(escapeHtml(v.mode) || '?', v.mode === 'nat' ? 'y' : v.mode === 'bridge' ? 'g' : 'r')) + H.row(_L('상태', 'State'), H.badge(escapeHtml(v.state) || '?', v.state === 'up' ? 'g' : 'r')) + H.row(_L('외부 연결', 'External'), ext) + H.row(_L('호스트 IP', 'Host IP'), escapeHtml(v.ip_cidr) || '-') + H.row('DHCP', v.dhcp ? 'ON' : '-') + H.row('VM', String(v.vm_count || 0)) + H.row(_L('서브넷', 'Subnet'), escapeHtml(v.subnet) || '-') + '<div class="flex gap-4 ops-action-row" style="margin-top:10px"><button class="btn btn-soft" style="font-size:10px;padding:3px 8px" onclick="showNetEdit(\'' + escapeAttr(v.name) + '\',\'' + escapeAttr(v.mode) + '\',\'' + escapeAttr(v.ip_cidr) + '\',' + (v.dhcp || false) + ',\'' + escapeAttr(v.phys || '') + '\')">' + t('btn.edit') + '</button><button class="btn btn-r" style="font-size:10px;padding:3px 8px" onclick="netDel(\'' + escapeAttr(v.name) + '\')">' + t('btn.delete') + '</button></div>', 'ops-mobile-card');
      });
      h += '</div>';
    } else {
      h += '<table class="table-sticky"><thead><tr><th>' + _L('네트워크', 'Network') + '</th><th>' + _L('모드', 'Mode') + '</th><th>' + _L('상태', 'State') + '</th><th>' + _L('외부 연결', 'External') + '</th><th>' + _L('호스트 IP', 'Host IP') + '</th><th>DHCP</th><th>VM</th><th>' + _L('서브넷', 'Subnet') + '</th><th>' + t('vm.settings') + '</th></tr></thead><tbody>';
      l.forEach(v => {
        const ext = v.mode === 'bridge' ? (escapeHtml(v.phys) || '-') : v.mode === 'nat' ? 'NAT' : '-';
        h += `<tr><td><b>${escapeHtml(v.name)}</b></td><td>${H.badge(escapeHtml(v.mode) || '?', v.mode === 'nat' ? 'y' : v.mode === 'bridge' ? 'g' : 'r')}</td><td>${H.badge(escapeHtml(v.state) || '?', v.state === 'up' ? 'g' : 'r')}</td><td>${ext}</td><td>${escapeHtml(v.ip_cidr) || '-'}</td><td>${v.dhcp ? 'ON' : '-'}</td><td>${v.vm_count || 0}</td><td>${escapeHtml(v.subnet) || '-'}</td><td class="nowrap"><button class="btn btn-soft" style="font-size:10px;padding:3px 8px" onclick="showNetEdit('${escapeAttr(v.name)}','${escapeAttr(v.mode) || ''}','${escapeAttr(v.ip_cidr) || ''}',${v.dhcp || false},'${escapeAttr(v.phys) || ''}')">${t('btn.edit')}</button> <button class="btn btn-r" style="font-size:10px;padding:3px 8px" onclick="netDel('${escapeAttr(v.name)}')">${t('btn.delete')}</button></td></tr>`;
      });
      h += '</tbody></table>';
    }
    h += '<div class="sg grid-2" style="margin-top:16px">';
    h += '<div class="hc"><h4>' + _L('방화벽 정책 편집', 'Firewall policy editor') + '</h4>';
    h += '<p class="color-muted text-11 mb-8">' + _L('브리지나 세그먼트를 확인한 뒤 인바운드/아웃바운드 규칙을 추가합니다.', 'Add ingress or egress rules after checking the bridge or segment you are editing.') + '</p>';
    h += '<div class="flex gap-8 mb-8 ops-form-strip" style="flex-wrap:wrap">';
    h += '<select id="fw-direction" class="input" style="width:110px"><option value="ingress">' + _L('인바운드', 'Ingress') + '</option><option value="egress">' + _L('아웃바운드', 'Egress') + '</option></select>';
    h += '<select id="fw-protocol" class="input w-80"><option>tcp</option><option>udp</option><option>icmp</option></select>';
    h += '<input id="fw-port" class="input" placeholder="' + _L('포트 예: 80 또는 8080-8090', 'Port e.g. 80 or 8080-8090') + '" class="w-140">';
    h += '<input id="fw-source" class="input" placeholder="' + _L('소스 CIDR', 'Source CIDR') + '" value="0.0.0.0/0" class="w-160">';
    h += '<button class="btn btn-primary" onclick="fwAddRule()">' + _L('규칙 추가', 'Add rule') + '</button>';
    h += '</div><div id="fw-rules-list"></div></div>';
    h += '<div class="hc"><h4>' + _L('OVN ACL 운영 메모', 'OVN ACL operations note') + '</h4>';
    h += '<p class="color-muted text-11 mb-8">' + _L('싱글 엣지에서는 상태를 먼저 확인하고, 필요한 경우 수동 ACL 명령으로 보강합니다.', 'In Single Edge, check state first and use manual ACL commands only when you need to refine policy.') + '</p>';
    h += '<pre style="background:var(--bg3);padding:8px;border-radius:6px;font-size:11px;overflow-x:auto">pcvctl ovn acl list &lt;switch&gt;\npcvctl ovn acl add &lt;switch&gt; --direction to-lport --match "ip4.src==10.0.0.0/24" --action allow\npcvctl ovn acl del &lt;switch&gt; --uuid &lt;uuid&gt;</pre>';
    h += '</div></div>';
    b.innerHTML = h;
  } catch (e) { if(_DEBUG) console.warn('n:', e.message); }
}
function toggleFwPanel() { var p = document.getElementById('fw-panel'); if (p) p.classList.toggle('hidden'); }
function toggleAclPanel() { var p = document.getElementById('acl-panel'); if (p) p.classList.toggle('hidden'); }
window.toggleFwPanel = toggleFwPanel;
window.toggleAclPanel = toggleAclPanel;

function showNetCreate() { showModal(`<h2>${t('net.new')}</h2><div class="fr"><label>Bridge</label><input id="nn" placeholder="pcvbr0"></div><div class="fr"><label>Mode</label><select id="nm" onchange="netModeChanged()"><option value="nat">${t('net.mode.nat')}</option><option value="isolated">${t('net.mode.isolated')}</option><option value="routed">${t('net.mode.routed')}</option><option value="bridge">${t('net.mode.bridge')}</option></select></div><div class="fr"><label>CIDR</label><input id="nc" placeholder="10.0.0.1/24"></div><div id="net-phys-row" class="fr hidden"><label>Physical NIC</label><div class="flex gap-6 flex-1"><select id="np" style="flex:1;padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:6px;font-size:12px"><option value="">${t('loading')}</option></select></div></div><div class="stat-label" style="margin:4px 0 8px 98px" id="net-mode-hint">NAT: MASQUERADE + DHCP</div><div class="text-right mt-12"><button class="btn btn-g" onclick="doNetCreate()">${t('btn.create')}</button> <button class="btn btn-r" onclick="closeModal()">${t('btn.cancel')}</button></div>`); }

function netModeChanged() { const m = document.getElementById('nm').value; const pr = document.getElementById('net-phys-row'); const hint = document.getElementById('net-mode-hint'); const mr = document.getElementById('np-manual-row');
  if (m === 'bridge') { pr.classList.remove('hidden'); loadPhysNics(); hint.textContent = 'Bridge: Physical NIC binding'; } else { pr.classList.add('hidden'); if (mr) mr.remove(); const hints = { nat: 'NAT: MASQUERADE + DHCP', isolated: 'Isolated: VM-to-VM only', routed: 'Routed: ip_forward only' }; hint.textContent = hints[m] || ''; } }

async function loadPhysNics() { const sel = document.getElementById('np'); if (!sel) return;
  try { const r = await fetchGet(EP.NET_LIST()); const nl = unwrapList(r); let h = '<option value="">-- Select NIC --</option>'; const seen = new Set();
    nl.forEach(n => { (n.slaves || []).forEach(s => { if (!seen.has(s) && !s.startsWith('vnet')) { seen.add(s); h += '<option value="' + escapeHtml(s) + '">' + escapeHtml(s) + ' (slave of ' + escapeHtml(n.name) + ')</option>'; } }); }); h += '<option value="" disabled>-- Manual --</option>'; sel.innerHTML = h;
    const row = document.getElementById('net-phys-row');
    if (row && !document.getElementById('np-manual')) { const mi = document.createElement('div'); mi.className = 'fr'; mi.id = 'np-manual-row'; mi.style.marginTop = '4px'; mi.innerHTML = '<label>NIC Name</label><input id="np-manual" placeholder="e.g. wlo1, enp42s0, eno1..." style="flex:1;padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:6px;font-size:12px">'; row.parentNode.insertBefore(mi, row.nextSibling); }
  } catch (e) { sel.innerHTML = '<option value="">-- Enter NIC name below --</option>'; } }

async function doNetCreate() { var _btn = document.activeElement; if (_btn && _btn.tagName === 'BUTTON') { if (_btn.disabled) return; _btn.disabled = true; _btn.setAttribute('aria-busy', 'true'); } try { const body = { bridge_name: document.getElementById('nn').value, mode: document.getElementById('nm').value, cidr: document.getElementById('nc').value }; const m = body.mode; if (m === 'bridge') { const p = document.getElementById('np')?.value || document.getElementById('np-manual')?.value; if (p) body.physical_if = p; else { toast(t('net.phys_required'), false); return; } } const r = await fetchPost(EP.NET_LIST(), body); if (r.error) { toast(r.error.message || 'Failed', false); } else { toast(t('net.created')); addEvt(t('net.created')); } closeModal(); renderNetworks(document.getElementById('cb')); } catch (e) { toast(e.message, false); } finally { if (_btn && _btn.tagName === 'BUTTON') { _btn.disabled = false; _btn.removeAttribute('aria-busy'); } } }

async function netDel(name) { showModal(`<h2 class="color-red">&#9888; ${t('btn.delete')} Network</h2><p class="mb-12">${t('vm.delete.confirm').replace('VM', 'Network')} <b class="color-accent">${escapeHtml(name)}</b></p><p class="mb-12">${t('vm.delete.type_name').replace('VM', 'Network')}</p><div class="fr"><label>Name</label><input id="del-net-confirm" placeholder="${escapeHtml(name)}"></div><div class="text-right mt-14"><button class="btn btn-r" onclick="doNetDel('${escapeAttr(name)}')">${t('btn.delete')}</button> <button class="btn" onclick="closeModal()">${t('btn.cancel')}</button></div>`); }

async function doNetDel(name) { const cv = document.getElementById('del-net-confirm')?.value; if (cv !== name) { toast(t('vm.name_mismatch'), false); return; }
  const mc = document.getElementById('mc'); mc.innerHTML = '<h2 class="color-red">&#9888; Deleting Network</h2><p><b class="color-accent">' + escapeHtml(name) + '</b></p><div class="prog-bar"><div class="prog-fill" id="dn-p" class="w-pct-20"></div></div><div class="prog-status" id="dn-s"><span class="spinner"></span>Removing firewall rules & DHCP...</div>';
  const pf = document.getElementById('dn-p'), ps = document.getElementById('dn-s');
  try { pf.style.width = '50%'; ps.innerHTML = '<span class="spinner"></span>Sending DELETE...';
    const d = await fetchDelete(EP.NET_DETAIL(name)).catch(() => ({}));
    if (d.error) { pf.style.background = 'var(--red)'; pf.style.width = '100%'; ps.innerHTML = '&#10060; ' + escapeHtml(d.error.message); toast(t('btn.delete') + ' failed', false); return; }
    pf.style.width = '100%'; ps.innerHTML = '&#9989; ' + t('net.deleted'); toast(t('net.deleted')); addEvt(t('net.deleted') + ': ' + name); setTimeout(() => { closeModal(); renderNetworks(document.getElementById('cb')); }, 1500);
  } catch (e) { pf.style.width = '100%'; ps.innerHTML = '&#10060; ' + escapeHtml(e.message); toast(e.message, false); } }

function showNetEditor() { navigateTo('networks'); }

function ovnDemoEntryName(entry) {
  if (typeof entry === 'string') return entry;
  if (!entry) return '';
  return entry.name || entry.entry || JSON.stringify(entry);
}

function ovnDemoHasEntry(list, name) {
  return Array.isArray(list) && list.some(function(entry) {
    return ovnDemoEntryName(entry) === name;
  });
}

function renderOvnDemoServiceComposition(dd, sd, sl, rl, hd) {
  const publicDomain = 'demo.purecvisor.site';
  const nodeName = hd.node_name || hd.node || 'PureCVisor-Prod-2';
  const switchName = dd.switch || 'pcv-demo-ls';
  const routerName = dd.router || 'pcv-demo-lr';
  const vmA = dd.vm_a || { name: 'ovn-demo-a', ip: '10.77.0.11' };
  const vmB = dd.vm_b || { name: 'ovn-demo-b', ip: '10.77.0.12' };
  const switchPresent = (dd.ovn && dd.ovn.switch_present === true) || ovnDemoHasEntry(sl, switchName);
  const routerPresent = (dd.ovn && dd.ovn.router_present === true) || ovnDemoHasEntry(rl, routerName);
  const pingOk = !!(dd.ping && dd.ping.ok === true);
  const httpOk = !!(dd.http && dd.http.ok === true);
  const visual = dd.visual_service || {};
  const visualOk = visual.ok === true;
  const visualUrl = visual.url || 'http://10.77.0.12:8080';
  const visualPublicUrl = 'https://' + publicDomain + '/ovn-visual/';
  const nodeDescriptions = Object.assign({
    'ovn-demo-a': _L('요청을 시작하는 클라이언트 VM입니다. OVN 내부망에서 ovn-demo-b 서비스까지 실제 통신을 발생시킵니다.', 'Client VM that initiates the demo request through the OVN internal network.'),
    'pcv-demo-ls': _L('OVN Logical Switch입니다. 데모 VM들이 같은 10.77.0.0/24 논리 L2 네트워크에 붙는 지점입니다.', 'OVN logical switch that places the demo VMs on the same 10.77.0.0/24 logical L2 network.'),
    'pcv-demo-lr': _L('OVN Logical Router입니다. 10.77.0.1 게이트웨이 역할을 하며 논리 네트워크의 라우팅 경계를 보여 줍니다.', 'OVN logical router that represents the 10.77.0.1 gateway and routing boundary.'),
    'ovn-demo-b': _L('시각 서비스가 실행되는 서버 VM입니다. 10.77.0.12:8080에서 HTML, health, visual-state를 제공합니다.', 'Server VM running the visual service on 10.77.0.12:8080.')
  }, visual.node_descriptions || {});
  const nodeNote = function(name) {
    const note = nodeDescriptions[name] || '';
    return note ? '<br><span class="color-muted text-11">' + escapeHtml(note) + '</span>' : '';
  };
  const fallbackServiceFlows = [
    {
      title: _L('외부 공개 시각 서비스 흐름', 'External visual service flow'),
      summary: _L('브라우저 요청이 nginx reverse proxy와 pcv-demo-host OVN access port를 거쳐 ovn-demo-b:8080으로 전달됩니다.', 'Browser requests pass through nginx reverse proxy and the pcv-demo-host OVN access port to ovn-demo-b:8080.'),
      steps: ['browser', 'nginx /ovn-visual/', 'pcv-demo-host 10.77.0.254', 'pcv-demo-ls', 'ovn-demo-b:8080', 'HTML / JSON']
    },
    {
      title: _L('내부 VM 점검 흐름', 'Internal VM check flow'),
      summary: _L('PureCVisor host collector가 qemu-guest-agent로 ovn-demo-a 내부 스크립트를 실행하고, ovn-demo-a가 OVN 내부망으로 ovn-demo-b의 health와 visual-state를 조회합니다.', 'The host collector runs the ovn-demo-a script through qemu-guest-agent, then ovn-demo-a reads ovn-demo-b health and visual-state through the OVN internal network.'),
      steps: ['host collector', 'qemu-guest-agent', 'ovn-demo-a', 'pcv-demo-ls', 'ovn-demo-b:8080', 'health JSON', 'PureCVisor UI']
    },
    {
      title: _L('pcv-demo-lr 라우팅 경계', 'pcv-demo-lr routing boundary'),
      summary: _L('pcv-demo-lr은 10.77.0.1 게이트웨이와 logical routing boundary를 보여 줍니다. 현재 health/visual service 왕복은 같은 10.77.0.0/24 내부 서비스 흐름이라 pcv-demo-ls를 중심으로 동작합니다.', 'pcv-demo-lr represents the 10.77.0.1 gateway and logical routing boundary. The current health and visual-service flows stay within 10.77.0.0/24 and center on pcv-demo-ls.'),
      steps: ['pcv-demo-ls', 'pcv-demo-lr 10.77.0.1', 'routing boundary']
    }
  ];
  const serviceFlows = Array.isArray(visual.service_flows) && visual.service_flows.length ? visual.service_flows : fallbackServiceFlows;
  const renderServiceFlow = function(flow) {
    const steps = Array.isArray(flow.steps) ? flow.steps : [];
    const stepHtml = steps.map(function(step) { return H.badge(String(step), 'g'); }).join('<span class="color-muted text-11">→</span>');
    const body = '<p class="color-muted text-11" style="line-height:1.5;margin:0 0 10px">' + escapeHtml(flow.summary || '') + '</p><div class="flex gap-4" style="flex-wrap:wrap;align-items:center">' + stepHtml + '</div>';
    return H.card(escapeHtml(flow.title || _L('서비스 흐름', 'Service flow')), body);
  };
  const visualPath = Array.isArray(visual.path) && visual.path.length
    ? visual.path.map(function(part) { return escapeHtml(part); }).join(' &rarr; ')
    : 'ovn-demo-a &rarr; pcv-demo-ls &rarr; pcv-demo-lr &rarr; ovn-demo-b';
  const live = dd.status === 'ok' && dd.stale !== true;
  const liveBadge = H.badge(live ? _L('라이브', 'Live') : _L('점검 필요', 'Check'), live ? 'g' : 'r');
  const switchBadge = H.badge(switchPresent ? _L('존재', 'Present') : _L('누락', 'Missing'), switchPresent ? 'g' : 'r');
  const routerBadge = H.badge(routerPresent ? _L('존재', 'Present') : _L('누락', 'Missing'), routerPresent ? 'g' : 'r');
  const vmPath = escapeHtml(vmA.name || 'ovn-demo-a') + ' (' + escapeHtml(vmA.ip || '10.77.0.11') + ') &rarr; ' + escapeHtml(vmB.name || 'ovn-demo-b') + ' (' + escapeHtml(vmB.ip || '10.77.0.12') + ')';
  let h = '<div class="ops-section-heading"><div><h3>' + _L('OVN 데모 서비스 구성', 'OVN demo service composition') + '</h3><p>' + _L('공개 데모 도메인부터 VM A → VM B 검증까지 현재 구성값을 API 응답으로 조합해 표시합니다.', 'The current API response composes the public demo domain through the VM A to VM B validation path.') + '</p></div></div>';
  h += '<div class="sg grid-3">';
  h += H.card(_L('공개 진입점', 'Public entrypoint'), H.row(_L('도메인', 'Domain'), publicDomain) + H.row('HTTPS', H.badge('443', 'g')) + H.row(_L('노드', 'Node'), escapeHtml(nodeName)));
  h += H.card('PureCVisor API', H.row(_L('서비스', 'Service'), escapeHtml(hd.service || 'purecvisorsd')) + H.row(_L('상태', 'State'), H.badge(hd.status || 'ok', hd.status === 'critical' ? 'r' : 'g')) + H.row('OVN', H.badge(sd.available ? _L('활성', 'Enabled') : _L('비활성', 'Disabled'), sd.available ? 'g' : 'r')));
  h += H.card(_L('라이브 증거', 'Live evidence'), liveBadge + H.row(_L('마지막 점검', 'Last check'), escapeHtml(dd.checked_at || '-')) + H.row(_L('stale 기준', 'Stale after'), String(dd.stale_after_sec || 300) + 's'));
  h += H.card(_L('OVN 내부 시각 서비스', 'OVN internal visual service'), H.badge(visualOk ? _L('정상', 'OK') : _L('확인 필요', 'Check'), visualOk ? 'g' : 'r') + H.row(_L('서비스', 'Service'), escapeHtml(visual.service || 'ovn-demo-visual')) + H.row(_L('공개 URL', 'Public URL'), escapeHtml(visualPublicUrl)) + H.row(_L('내부 URL', 'Internal URL'), escapeHtml(visualUrl)) + H.row(_L('외부 inbound', 'External inbound'), visual.external_inbound === false ? _L('없음', 'None') : _L('확인 필요', 'Check')));
  h += '</div>';
  h += '<div class="ops-section-heading"><div><h3>' + _L('서비스 흐름', 'Service flow') + '</h3><p>' + _L('각 VM과 OVN 구성요소가 요청, 점검, 응답 흐름에서 맡는 역할을 단계별로 표시합니다.', 'Each VM and OVN component is shown as a step in the request, validation, and response flow.') + '</p></div></div>';
  h += '<div class="sg grid-3">' + serviceFlows.map(renderServiceFlow).join('') + '</div>';
  h += '<div class="sg grid-2">';
  h += '<div class="hc"><h4>' + _L('동적 토폴로지', 'Dynamic topology') + '</h4><table class="table-sticky"><thead><tr><th>' + _L('계층', 'Layer') + '</th><th>' + _L('현재 값', 'Current value') + '</th><th>' + _L('상태', 'State') + '</th></tr></thead><tbody>';
  h += '<tr><td>' + _L('브라우저', 'Browser') + '</td><td>https://' + publicDomain + '/ui/</td><td>' + H.badge(_L('공개', 'Public'), 'g') + '</td></tr>';
  h += '<tr><td>PureCVisor API</td><td>/api/v1/demo/ovn-ovs/health</td><td>' + H.badge(_L('읽기', 'Read'), 'g') + '</td></tr>';
  h += '<tr><td>OVN Logical Switch</td><td>' + escapeHtml(switchName) + nodeNote(switchName) + '</td><td>' + switchBadge + '</td></tr>';
  h += '<tr><td>OVN Logical Router</td><td>' + escapeHtml(routerName) + ' / 10.77.0.1' + nodeNote(routerName) + '</td><td>' + routerBadge + '</td></tr>';
  h += '<tr><td>VM A → VM B</td><td>' + vmPath + nodeNote(vmA.name || 'ovn-demo-a') + nodeNote(vmB.name || 'ovn-demo-b') + '</td><td>' + H.badge(pingOk && httpOk ? _L('성공', 'OK') : _L('확인 필요', 'Check'), pingOk && httpOk ? 'g' : 'r') + '</td></tr>';
  h += '<tr><td>' + _L('시각 서비스', 'Visual service') + '</td><td>' + visualPath + '<br><span class="color-muted text-11">' + escapeHtml(visualPublicUrl) + ' · ' + escapeHtml(visualUrl) + ' · ' + _L('외부 inbound 없음', 'No external inbound') + '</span>' + nodeNote('ovn-demo-b') + '</td><td>' + H.badge(visualOk ? _L('성공', 'OK') : _L('확인 필요', 'Check'), visualOk ? 'g' : 'r') + '</td></tr>';
  h += '</tbody></table></div>';
  h += '<div class="hc"><h4>' + _L('viewer 읽기 전용 경계', 'Viewer read-only boundary') + '</h4><table class="table-sticky"><thead><tr><th>' + _L('요청', 'Request') + '</th><th>' + _L('기대 결과', 'Expected') + '</th></tr></thead><tbody>';
  h += '<tr><td>GET /api/v1/ovn/status</td><td>' + H.badge('200', 'g') + '</td></tr>';
  h += '<tr><td>GET /api/v1/ovn/switches</td><td>' + H.badge('200', 'g') + '</td></tr>';
  h += '<tr><td>GET /api/v1/demo/ovn-ovs/health</td><td>' + H.badge('200', 'g') + '</td></tr>';
  h += '<tr><td>GET /api/v1/vnc/' + escapeHtml(vmA.name || 'ovn-demo-a') + '</td><td>' + H.badge('403', 'r') + '</td></tr>';
  h += '<tr><td>POST /api/v1/vms/' + escapeHtml(vmA.name || 'ovn-demo-a') + '/stop</td><td>' + H.badge('403', 'r') + '</td></tr>';
  h += '</tbody></table></div>';
  h += '</div>';
  return h;
}

function showNetEdit(name, mode, cidr, dhcp, phys) {
  showModal(`<h2>&#9881; ${t('btn.edit')}: ${escapeHtml(name)}</h2><div class="fr"><label>Bridge</label><input value="${escapeHtml(name)}" disabled style="opacity:.6"></div><div class="fr"><label>Mode</label><select id="ne-mode" onchange="netEditModeChanged()"><option value="nat" ${mode === 'nat' ? 'selected' : ''}>nat</option><option value="isolated" ${mode === 'isolated' ? 'selected' : ''}>isolated</option><option value="routed" ${mode === 'routed' ? 'selected' : ''}>routed</option><option value="bridge" ${mode === 'bridge' ? 'selected' : ''}>bridge</option></select></div><div class="fr"><label>DHCP</label><select id="ne-dhcp"><option value="on" ${dhcp ? 'selected' : ''}>ON</option><option value="off" ${!dhcp ? 'selected' : ''}>OFF</option></select></div><div class="fr" id="ne-phys-row" ${mode !== 'bridge' ? 'class="hidden"' : ''}><label>Physical NIC</label><input id="ne-phys" value="${escapeHtml(phys)}" placeholder="e.g. wlo1, enp42s0"></div><div class="stat-label" style="margin:4px 0 8px 98px" id="ne-hint">Mode: ${escapeHtml(mode)}</div><div class="text-right mt-14"><button class="btn btn-g" onclick="doNetEdit('${escapeAttr(name)}')">${t('btn.apply')}</button> <button class="btn btn-r" onclick="closeModal()">${t('btn.cancel')}</button></div>`);
}

function netEditModeChanged() { const m = document.getElementById('ne-mode').value; const pr = document.getElementById('ne-phys-row'); const hint = document.getElementById('ne-hint');
  if (m === 'bridge') { pr.classList.remove('hidden'); hint.textContent = 'Bridge: Physical NIC required'; } else { pr.classList.add('hidden'); hint.textContent = 'Mode: ' + m; } }

async function doNetEdit(name) { const mode = document.getElementById('ne-mode').value; try { const mr = await fetchPost(EP.NET_MODE(name), { mode: mode }); if (mr.error) { toast('Failed: ' + (mr.error.message || ''), false); return; } toast(name + ' updated'); addEvt('Network edit: ' + name); closeModal(); renderNetworks(document.getElementById('cb')); } catch (e) { toast('Edit failed: ' + e.message, false); } }


async function renderOvn(b) {
  b.innerHTML = showSkeleton();
  try {
    const st = await fetchGet(EP.OVN_STATUS()); const sd = unwrapData(st);
    const sw = await fetchGet(EP.OVN_SWITCHES()); const sl = unwrapList(sw);
    const rt = await fetchGet(EP.OVN_ROUTERS()); const rl = unwrapList(rt);
    const health = await fetchGet(EP.HEALTH()).catch(function() {
      return { status: 'unknown', node_name: 'PureCVisor-Prod-2', service: 'purecvisorsd' };
    });
    const hd = unwrapData(health);
    const demo = await fetchGet(EP.DEMO_OVN_HEALTH()).catch(function(e) {
      return { data: { status: 'stale', stale: true, reason: e.message } };
    });
    const dd = unwrapData(demo);
    const demoLive = dd.status === 'ok' && dd.stale !== true;
    const demoBadge = demoLive ? 'g' : (dd.status === 'degraded' ? 'y' : 'r');
    const pingOk = !!(dd.ping && dd.ping.ok === true);
    const httpOk = !!(dd.http && dd.http.ok === true);
    const visualOk = !!(dd.visual_service && dd.visual_service.ok === true);
    let h = '<div class="ops-section-heading"><div><h3>OVN SDN</h3><p>' + _L('싱글 엣지에서 허용된 로컬 OVN 상태와 수동 구성을 확인합니다.', 'Review the local OVN state and the manually managed configuration allowed in Single Edge.') + '</p></div></div>';
    h += '<div class="sg grid-3">';
    h += H.card(_L('OVN 가용성', 'OVN availability'), sd.available ? H.badge(_L('준비됨', 'Ready'), 'g') + H.row('Encap', 'Geneve') : '<p class="color-muted text-xs">' + _L('OVN이 설치되지 않았습니다', 'OVN is not installed') + '</p>');
    h += H.card(_L('논리 스위치', 'Logical switches'), '<div class="stat-lg color-accent">' + sl.length + '</div>' + H.row(_L('상태', 'State'), H.badge(sd.available ? _L('조회 가능', 'Available') : _L('미설치', 'Unavailable'), sd.available ? 'g' : 'r')));
    h += H.card(_L('논리 라우터', 'Logical routers'), '<div class="stat-lg color-green">' + rl.length + '</div>' + H.row(_L('운영 방식', 'Mode'), _L('수동 구성', 'Manual configuration')));
    h += '</div>';
    h += '<div class="ops-section-heading"><div><h3>' + _L('공개 OVN 데모 헬스', 'Public OVN demo health') + '</h3><p>' + _L('viewer 권한으로 데모 VM 간 실제 통신 결과를 읽기 전용으로 확인합니다.', 'Viewer users can inspect the live VM-to-VM demo result without control permissions.') + '</p></div></div>';
    h += '<div class="sg grid-3">';
    h += H.card(_L('VM 간 통신', 'VM reachability'), H.badge(demoLive ? _L('정상', 'OK') : _L('확인 필요', 'Check'), demoBadge) + H.row('Ping', pingOk ? 'OK' : 'N/A') + H.row('HTTP', httpOk ? 'OK' : 'N/A') + H.row(_L('시각 서비스', 'Visual service'), visualOk ? 'OK' : 'N/A'));
    h += H.card(_L('데모 네트워크', 'Demo network'), H.row(_L('스위치', 'Switch'), escapeHtml(dd.switch || 'pcv-demo-ls')) + H.row(_L('라우터', 'Router'), escapeHtml(dd.router || 'pcv-demo-lr')));
    h += H.card(_L('최근 점검', 'Last check'), H.row(_L('시각', 'Time'), escapeHtml(dd.checked_at || '-')) + H.row(_L('상태', 'State'), dd.stale ? 'stale' : 'live'));
    h += '</div>';
    h += renderOvnDemoServiceComposition(dd, sd, sl, rl, hd);
    h += '<div class="ops-section-heading"><div><h3>' + _L('논리 토폴로지', 'Logical topology') + '</h3><p>' + _L('스위치와 라우터 목록을 먼저 확인한 뒤 수동 정책과 부가 구성을 적용합니다.', 'Review switches and routers first, then apply manual policy and optional configuration.') + '</p></div></div>';
    h += '<div class="sg grid-2">';
    h += '<div class="hc"><h4>' + _L('논리 스위치', 'Logical switches') + ' (' + sl.length + ')</h4>';
    if (sl.length === 0) h += '<p class="color-muted text-xs">' + _L('구성된 스위치가 없습니다', 'No switches configured') + '<br><span class="text-12">' + _L('로컬 정책을 넣기 전, 현재 토폴로지가 비어 있는지 먼저 확인하십시오.', 'Confirm the topology is intentionally empty before adding local policy.') + '</span></p>';
    else h += '<table class="table-sticky"><thead><tr><th>' + _L('이름', 'Name') + '</th></tr></thead><tbody>' + sl.map(function(s) { const n = typeof s === 'string' ? s : (s.name || s.entry || JSON.stringify(s)); return '<tr><td>' + esc(n) + '</td></tr>'; }).join('') + '</tbody></table>';
    h += '</div>';
    h += '<div class="hc"><h4>' + _L('논리 라우터', 'Logical routers') + ' (' + rl.length + ')</h4>';
    if (rl.length === 0) h += '<p class="color-muted text-xs">' + _L('구성된 라우터가 없습니다', 'No routers configured') + '<br><span class="text-12">' + _L('필요한 경우에만 수동 라우터를 추가하십시오.', 'Create a router only when the local design requires it.') + '</span></p>';
    else h += '<table class="table-sticky"><thead><tr><th>' + _L('이름', 'Name') + '</th></tr></thead><tbody>' + rl.map(function(r) { const n = typeof r === 'string' ? r : (r.name || r.entry || JSON.stringify(r)); return '<tr><td>' + esc(n) + '</td></tr>'; }).join('') + '</tbody></table>';
    h += '</div></div>';
    h += '<div class="sg grid-2">';
    h += '<div class="hc"><h4>&#9878; ' + _L('로드 밸런서 설정', 'LB setup') + '</h4><p class="color-muted text-11 mb-8">' + _L('VIP와 백엔드 목록을 수동으로 입력해 단일 노드 로컬 구성을 점검합니다.', 'Enter the VIP and backend list manually to validate the local single-node setup.') + '</p><div class="mb-8 ops-stack-form"><div class="fr"><label>' + _L('이름', 'Name') + '</label><input id="lb-n" placeholder="edge-lb"></div><div class="fr"><label>VIP:Port</label><input id="lb-vip" placeholder="10.0.0.100"><input id="lb-port" type="number" value="80" class="w-60"></div><div class="fr"><label>' + _L('백엔드', 'Backends') + '</label><input id="lb-bk" placeholder="10.0.0.1:80,10.0.0.2:80"></div><button class="btn btn-primary" onclick="nfvLbCreate()">' + _L('LB 생성', 'Create LB') + '</button></div><div id="lb-list"></div></div>';
    h += '<div class="hc"><h4>&#128737; ' + _L('ACL 정책 추가', 'ACL policy add') + '</h4><p class="color-muted text-11 mb-8">' + _L('스위치 단위로 방향, 우선순위, 매치 조건을 입력해 ACL을 추가합니다.', 'Add ACLs per switch with direction, priority, and match conditions.') + '</p><div class="mb-8 ops-stack-form"><div class="fr"><label>' + _L('스위치', 'Switch') + '</label><input id="fw-sw" placeholder="web-tier"></div><div class="fr"><label>' + _L('방향', 'Direction') + '</label><select id="fw-dir"><option>from-lport</option><option>to-lport</option></select></div><div class="fr"><label>' + _L('우선순위', 'Priority') + '</label><input id="fw-pri" type="number" value="1000"></div><div class="fr"><label>Match</label><input id="fw-match" placeholder="ip4.src==10.0.0.0/24"></div><div class="fr"><label>' + _L('동작', 'Action') + '</label><select id="fw-act"><option>allow</option><option>drop</option><option>reject</option></select></div><button class="btn btn-primary" onclick="nfvFwAdd()">' + _L('ACL 규칙 추가', 'Add ACL rule') + '</button></div></div>';
    h += '</div>';
    b.innerHTML = h; loadLBList();
  } catch (e) { b.innerHTML = '<p class="color-red">' + _L('OVN 정보를 불러오지 못했습니다', 'Unable to load OVN data') + '</p>'; }
}

async function loadLBList() { try { const el = document.getElementById('lb-list'); if (el) el.innerHTML = '<p class="color-muted text-xs">' + _L('로드 밸런서 상태 메모', 'Load balancer status note') + ': ' + _L('위 가용성 카드에서 OVN 상태를 먼저 확인한 뒤 LB를 추가하십시오.', 'Confirm OVN availability above before adding a load balancer.') + '</p>'; } catch (e) { if(_DEBUG) console.warn('loadLBList:', e.message); } }
async function nfvLbCreate() {
  try {
    var name = document.getElementById('nfv-lb-name')?.value?.trim() || document.getElementById('lb-n')?.value?.trim();
    var vip = document.getElementById('nfv-lb-vip')?.value?.trim() || document.getElementById('lb-vip')?.value?.trim();
    var members = document.getElementById('nfv-lb-members')?.value?.trim() || document.getElementById('lb-bk')?.value?.trim();
    if (!name || !vip) { toast(_L('이름과 VIP를 입력하세요', 'Name and VIP required'), false); return; }
    var r = await fetchPost(EP.RPC(), {jsonrpc:'2.0', method:'nfv.lb.create', params:{
      name: name, vip: vip, members: members ? members.split(',').map(function(s){return s.trim();}) : []
    }, id:'nlb1'});
    if (r.error) { toast(r.error.message || 'Failed', false); return; }
    toast(_L('LB 생성됨', 'LB created'));
    addEvt('LB created: ' + name);
    renderContent();
  } catch (e) { toast(e.message, false); }
}
async function nfvFwAdd() { try { const sw = document.getElementById('fw-sw')?.value; const dir = document.getElementById('fw-dir')?.value; const pri = document.getElementById('fw-pri')?.value; const match = document.getElementById('fw-match')?.value; const act = document.getElementById('fw-act')?.value; if (!sw || !match) { toast('Switch and Match required', false); return; } await fetchPost(EP.OVN_ACL(), { switch: sw, direction: dir, priority: +pri, match: match, action: act }); toast('ACL rule added'); addEvt('ACL rule added to ' + sw); } catch (e) { toast(e.message, false); } }


async function renderSecGroups(b) {
  b.innerHTML = showSkeleton();
  let h = H.section('&#128737; Security Groups');
  h += '<div class="sg grid-2 mb-16">';
  h += '<div class="hc"><h4>&#128737; OVN ACL 보안 그룹</h4>';
  h += '<p class="color-muted text-12 mb-8">OVN ACL 기반 보안 그룹을 관리합니다. 논리 스위치에 인바운드/아웃바운드 규칙을 적용합니다.</p>';
  h += '<div class="mb-8">';
  h += '<div class="fr"><label>Switch</label><input id="sg-switch" placeholder="web-tier" class="w-150"></div>';
  h += '<div class="fr"><label>Direction</label><select id="sg-dir" class="input-pcv"><option value="from-lport">Inbound (from-lport)</option><option value="to-lport">Outbound (to-lport)</option></select></div>';
  h += '<div class="fr"><label>Priority</label><input id="sg-pri" type="number" value="1000" class="w-80"></div>';
  h += '<div class="fr"><label>Match</label><input id="sg-match" placeholder="ip4.src==10.0.0.0/24" style="width:250px"></div>';
  h += '<div class="fr"><label>Action</label><select id="sg-act" class="input-pcv"><option>allow</option><option>drop</option><option>reject</option></select></div>';
  h += '<button class="btn btn-g" onclick="sgAddRule()">+ ACL 규칙 추가</button>';
  h += '</div><div id="sg-result" class="mt-8"></div></div>';
  h += '<div class="hc"><h4>&#128203; 현재 ACL 규칙</h4>';
  h += '<div class="fr"><label>Switch</label><input id="sg-list-switch" placeholder="web-tier" class="w-150"> <button class="btn" onclick="sgListRules()">조회</button></div>';
  h += '<div id="sg-rules" class="mt-8"></div></div>';
  h += '</div>';
  h += H.card('&#128214; CLI 명령어 참조', '<div style="font-size:12px;line-height:1.8;color:var(--fg2)">' +
    '<code class="color-accent">pcvctl ovn acl list &lt;switch&gt;</code> &mdash; ACL 규칙 목록<br>' +
    '<code class="color-accent">pcvctl ovn acl add &lt;switch&gt; from-lport 1000 &quot;ip4.src==10.0.0.0/24&quot; allow</code> &mdash; 규칙 추가<br>' +
    '<code class="color-accent">pcvctl ovn tenant create &lt;name&gt; &lt;cidr&gt;</code> &mdash; 멀티테넌트 생성 (스위치+ACL+DHCP)</div>');
  b.innerHTML = h;
}

window.sgAddRule = async function() {
  const el = document.getElementById('sg-result');
  const sw = document.getElementById('sg-switch')?.value;
  const dir = document.getElementById('sg-dir')?.value;
  const pri = document.getElementById('sg-pri')?.value;
  const match = document.getElementById('sg-match')?.value;
  const act = document.getElementById('sg-act')?.value;
  if (!sw || !match) { if (el) el.innerHTML = '<span class="color-red">Switch와 Match는 필수입니다</span>'; return; }
  if (el) el.innerHTML = '<span class="spinner"></span> 추가 중...';
  try {
    await fetchPost(EP.OVN_ACL(), { switch_name: sw, direction: dir, priority: parseInt(pri), match: match, action: act });
    if (el) el.innerHTML = '<span class="color-green">ACL 규칙 추가 완료</span>';
    toast('ACL 규칙 추가: ' + escapeHtml(sw));
  } catch (e) { if (el) el.innerHTML = '<span class="color-red">오류: ' + escapeHtml(e.message) + '</span>'; }
};

window.sgListRules = async function() {
  const el = document.getElementById('sg-rules');
  const sw = document.getElementById('sg-list-switch')?.value;
  if (!sw) { if (el) el.innerHTML = '<span class="color-red">Switch 이름을 입력하세요</span>'; return; }
  if (el) el.innerHTML = '<span class="spinner"></span> 조회 중...';
  try {
    const r = await fetchGet(EP.OVN_ACL() + '?switch=' + encodeURIComponent(sw));
    const list = unwrapList(r);
    if (list.length === 0) { if (el) el.innerHTML = '<p class="color-muted text-12">ACL 규칙 없음</p>'; return; }
    let h = '<table class="text-11"><thead><tr><th>Direction</th><th>Priority</th><th>Match</th><th>Action</th></tr></thead><tbody>';
    list.forEach(a => {
      const entry = typeof a === 'string' ? a : '';
      if (entry) { h += '<tr><td colspan="4">' + escapeHtml(entry) + '</td></tr>'; }
      else { h += '<tr><td>' + escapeHtml(a.direction || '') + '</td><td>' + escapeHtml(String(a.priority || '')) + '</td><td>' + escapeHtml(a.match || '') + '</td><td>' + escapeHtml(a.action || '') + '</td></tr>'; }
    });
    h += '</tbody></table>';
    if (el) el.innerHTML = h;
  } catch (e) { if (el) el.innerHTML = '<span class="color-red">오류: ' + escapeHtml(e.message) + '</span>'; }
};


async function renderOverlayNetworks(b) {
  b.innerHTML = showSkeleton();
  try {
    const r = await fetchGet(EP.OVERLAY_LIST());
    const l = unwrapList(r);
    let h = H.section('Overlay Networks (VXLAN)');
    if (!Array.isArray(l) || l.length === 0) {
      h += '<div class="empty-state"><div class="empty-state-icon">&#127760;</div><div class="empty-state-text">No overlay networks</div></div>';
    } else {
      h += '<table class="table-sticky"><thead><tr><th>Name</th><th>VNI</th><th>Peers</th><th>Status</th></tr></thead><tbody>';
      l.forEach(v => {
        h += '<tr><td><b>' + esc(v.name || '?') + '</b></td><td>' + (v.vni || '-') + '</td><td>' + (v.peer_count || 0) + '</td><td>' + H.badge(v.state || '?', v.state === 'up' ? 'g' : 'r') + '</td></tr>';
      });
      h += '</tbody></table>';
    }
    b.innerHTML = h;
  } catch (e) { b.innerHTML = H.section('Overlay Networks') + '<p class="color-muted">Failed to load: ' + esc(e.message) + '</p>'; }
}
window.renderOverlayNetworks = renderOverlayNetworks;


async function renderTopology(b) {
  b.innerHTML = showSkeleton();
  try {
    var results = await Promise.all([
      fetchGet(EP.NET_LIST()).catch(function() { return { data: [] }; }),
      fetchGet(EP.VM_LIST()).catch(function() { return { data: [] }; })
    ]);
    var nets = unwrapList(results[0]);
    var vms = unwrapList(results[1]);

    var h = H.section(_L('네트워크 토폴로지', 'Network Topology'));
    h += '<canvas id="topo-canvas" width="800" height="500" style="width:100%;max-width:800px;border:1px solid var(--border);border-radius:6px;background:var(--bg2)"></canvas>';
    h += '<div class="flex gap-8 mt-8 text-xs">';
    h += '<span>&#128421; ' + _L('노드', 'Node') + '</span>';
    h += '<span>&#127760; ' + _L('브릿지', 'Bridge') + '</span>';
    h += '<span>&#128187; VM</span>';
    h += '</div>';
    b.innerHTML = h;


    setTimeout(function() {
      var canvas = document.getElementById('topo-canvas');
      if (!canvas) return;
      var ctx = canvas.getContext('2d');
      var W = canvas.width;


      var style = getComputedStyle(document.documentElement);
      var accentColor = style.getPropertyValue('--accent').trim() || '#00f0ff';
      var greenColor = style.getPropertyValue('--green').trim() || '#00ff88';
      var fgColor = style.getPropertyValue('--fg').trim() || '#e0f0ff';
      var dimColor = style.getPropertyValue('--fg2').trim() || '#5a6a8a';


      var nodes = (typeof MON_NODES !== 'undefined' && MON_NODES) ? MON_NODES : [{name:'Node1',ip:'localhost'}];


      var nodePositions = [];
      nodes.forEach(function(nd, i) {
        var x = (W / (nodes.length + 1)) * (i + 1);
        var y = 50;
        nodePositions.push({x:x, y:y, name:nd.name});
        ctx.fillStyle = accentColor;
        ctx.beginPath(); ctx.arc(x, y, 20, 0, Math.PI * 2); ctx.fill();
        ctx.fillStyle = '#000';
        ctx.font = '10px monospace'; ctx.textAlign = 'center';
        ctx.fillText(nd.name, x, y + 4);
        ctx.fillStyle = dimColor;
        ctx.fillText(nd.ip, x, y + 34);
      });


      var bridgePositions = [];
      nets.slice(0, 6).forEach(function(net, i) {
        var x = (W / (Math.min(nets.length, 6) + 1)) * (i + 1);
        var y = 200;
        bridgePositions.push({x:x, y:y, name:net.name});
        ctx.fillStyle = greenColor;
        ctx.fillRect(x - 30, y - 12, 60, 24);
        ctx.fillStyle = '#000';
        ctx.font = '9px monospace'; ctx.textAlign = 'center';
        ctx.fillText(net.name, x, y + 4);

        var closest = nodePositions[0] || {x:x, y:50};
        ctx.strokeStyle = dimColor; ctx.lineWidth = 1;
        ctx.setLineDash([4, 4]);
        ctx.beginPath(); ctx.moveTo(x, y - 12); ctx.lineTo(closest.x, closest.y + 20); ctx.stroke();
        ctx.setLineDash([]);
      });


      vms.slice(0, 12).forEach(function(vm, i) {
        var cols = Math.min(vms.length, 6);
        var row = Math.floor(i / cols);
        var col = i % cols;
        var x = (W / (cols + 1)) * (col + 1);
        var y = 340 + row * 60;
        var on = vm.state === 'running';
        ctx.fillStyle = on ? greenColor : dimColor;
        ctx.fillRect(x - 25, y - 10, 50, 20);
        ctx.fillStyle = on ? '#000' : fgColor;
        ctx.font = '8px monospace'; ctx.textAlign = 'center';
        var label = vm.name.length > 8 ? vm.name.substring(0, 8) + '..' : vm.name;
        ctx.fillText(label, x, y + 3);

        if (bridgePositions.length > 0) {
          var br = bridgePositions[i % bridgePositions.length];
          ctx.strokeStyle = on ? accentColor : 'rgba(90,106,138,0.3)';
          ctx.lineWidth = on ? 1 : 0.5;
          ctx.beginPath(); ctx.moveTo(x, y - 10); ctx.lineTo(br.x, br.y + 12); ctx.stroke();
        }
      });
    }, 100);
  } catch (e) { b.innerHTML = H.section('Topology') + '<p class="color-red">' + esc(e.message) + '</p>'; }
}
window.renderTopology = renderTopology;


async function fwAddRule() {
  var dir = document.getElementById('fw-direction').value;
  var proto = document.getElementById('fw-protocol').value;
  var port = document.getElementById('fw-port').value.trim();
  var source = document.getElementById('fw-source').value.trim() || '0.0.0.0/0';
  var portStart = 0, portEnd = 0;
  if (port.includes('-')) { var ps = port.split('-'); portStart = parseInt(ps[0]); portEnd = parseInt(ps[1]); }
  else if (port) { portStart = parseInt(port); portEnd = portStart; }

  var sgName = 'default';
  var r = await fetchPost(EP.RPC(), {jsonrpc:'2.0', method:'security_group.rule.add', params:{
    name: sgName, direction: dir, protocol: proto, port_start: portStart, port_end: portEnd, source: source
  }, id:'fw1'});
  if (r.error) { toast(r.error.message || 'Failed', false); return; }
  toast(_L('규칙 추가됨', 'Rule added'));
  fwLoadRules();
}

async function fwLoadRules() {
  var r = await fetchPost(EP.RPC(), {jsonrpc:'2.0', method:'security_group.list', params:{}, id:'fwl1'});
  var groups = unwrapList(r);
  var el = document.getElementById('fw-rules-list');
  if (!el) return;
  var h = '';
  groups.forEach(function(sg) {
    h += '<div class="mb-8"><strong>' + esc(sg.name) + '</strong> <span class="stat-label">(' + (sg.rule_count || 0) + ' rules)</span>';
    if (sg.rules && sg.rules.length) {
      h += '<table class="tbl mt-4"><tr><th>Dir</th><th>Proto</th><th>Port</th><th>Source</th><th></th></tr>';
      sg.rules.forEach(function(r) {
        var portStr = r.port_end > r.port_start ? r.port_start + '-' + r.port_end : (r.port_start || '*');
        h += '<tr><td>' + esc(r.direction) + '</td><td>' + esc(r.protocol) + '</td><td>' + portStr + '</td><td>' + esc(r.source) + '</td>';
        h += '<td><button class="btn btn-sm btn-r" onclick="fwDelRule(\'' + esc(sg.name) + '\',' + (r.db_id || 0) + ')">' + _L('삭제','Del') + '</button></td></tr>';
      });
      h += '</table>';
    }
    h += '</div>';
  });
  el.innerHTML = h || '<div class="stat-label">No security groups</div>';
}

async function fwDelRule(sg, ruleId) {
  var r = await fetchPost(EP.RPC(), {jsonrpc:'2.0', method:'security_group.rule.remove', params:{name:sg, rule_id:ruleId}, id:'fwd1'});
  if (r.error) { toast(r.error.message, false); return; }
  toast(_L('규칙 삭제됨', 'Rule removed'));
  fwLoadRules();
}

window.fwAddRule = fwAddRule;
window.fwLoadRules = fwLoadRules;
window.fwDelRule = fwDelRule;


window.renderNetworks = renderNetworks;
window.showNetCreate = showNetCreate;
window.netModeChanged = netModeChanged;
window.loadPhysNics = loadPhysNics;
window.doNetCreate = doNetCreate;
window.netDel = netDel;
window.doNetDel = doNetDel;
window.showNetEditor = showNetEditor;
window.showNetEdit = showNetEdit;
window.netEditModeChanged = netEditModeChanged;
window.doNetEdit = doNetEdit;
window.renderOvn = renderOvn;
window.loadLBList = loadLBList;
window.nfvLbCreate = nfvLbCreate;
window.nfvFwAdd = nfvFwAdd;
window.renderSecGroups = renderSecGroups;



PCV.network = {
  renderNetworks: renderNetworks,
  toggleFwPanel: toggleFwPanel,
  toggleAclPanel: toggleAclPanel,
  showNetCreate: showNetCreate,
  netModeChanged: netModeChanged,
  loadPhysNics: loadPhysNics,
  doNetCreate: doNetCreate,
  netDel: netDel,
  doNetDel: doNetDel,
  showNetEditor: showNetEditor,
  showNetEdit: showNetEdit,
  netEditModeChanged: netEditModeChanged,
  doNetEdit: doNetEdit,
  renderOvn: renderOvn,
  loadLBList: loadLBList,
  nfvLbCreate: nfvLbCreate,
  nfvFwAdd: nfvFwAdd,
  renderSecGroups: renderSecGroups,
  renderOverlayNetworks: renderOverlayNetworks,
  renderTopology: renderTopology,
  fwAddRule: fwAddRule,
  fwLoadRules: fwLoadRules,
  fwDelRule: fwDelRule
};

})(window.PCV);












window.PCV = window.PCV || {};
(function(PCV) {

function storagePct(totalBytes, usedBytes) {
  if (!totalBytes || totalBytes <= 0) return 0;
  return Math.max(0, (usedBytes / totalBytes) * 100);
}

function storagePctText(totalBytes, usedBytes) {
  return storagePct(totalBytes, usedBytes).toFixed(1) + '%';
}

async function renderStorage(b) {
  b.innerHTML = showSkeleton();
  try {
    const p = await fetchGet(EP.STORAGE_POOLS());
    const pl = unwrapList(p);
    let h = '<div class="ops-section-heading"><div><h3>' + _L('스토리지 운영 개요', 'Storage operations overview') + '</h3><p>' + _L('풀 상태, 용량 계획, Zvol 작업을 한 화면에서 정리합니다.', 'Review pool health, capacity planning, and zvol operations in one place.') + '</p></div><button class="btn btn-g" onclick="showPoolCreate()">+ ' + _L('풀 생성', 'Create pool') + '</button></div>';
    if (pl.length === 0) { h += '<div class="empty-state" style="text-align:center;padding:40px 20px"><div style="font-size:48px;margin-bottom:12px;opacity:.5">&#128190;</div><div style="font-size:14px;color:var(--fg2);margin-bottom:16px">' + _L('구성된 ZFS 풀이 없습니다', 'No configured ZFS pools') + '</div><button class="btn btn-g" onclick="showPoolCreate()" class="text-12">+ ' + _L('풀 생성', 'Create pool') + '</button></div>'; }
    if (pl.length > 0) {
      const totalBytes = pl.reduce(function(sum, v) { return sum + parseSize(v.size); }, 0);
      const usedBytes = pl.reduce(function(sum, v) { return sum + parseSize(v.alloc || v.used); }, 0);
      const totalPct = storagePct(totalBytes, usedBytes);
      const warningPools = pl.filter(function(v) {
        const sz = parseSize(v.size);
        const us = parseSize(v.alloc || v.used);
        const pct = storagePct(sz, us);
        return v.health !== 'ONLINE' || pct >= 80;
      }).length;
      h += '<div class="sg grid-3">';
      h += H.card(_L('풀 상태', 'Pool health'), '<div class="stat-lg color-accent">' + pl.length + '</div>' + H.row(_L('정상', 'Healthy'), '<span class="color-green">' + (pl.length - warningPools) + '</span>') + H.row(_L('주의 필요', 'Needs attention'), '<span class="color-yellow">' + warningPools + '</span>'));
      h += H.card(_L('사용 중 용량', 'Used capacity'), '<div class="stat-lg color-green">' + fmtBytes(usedBytes) + '</div>' + renderProgressBar(Math.min(totalPct, 100)) + H.row(_L('전체', 'Total'), fmtBytes(totalBytes)) + H.row(_L('사용률', 'Usage'), storagePctText(totalBytes, usedBytes)));
      h += H.card(_L('운영 원칙', 'Operating rule'), '<div class="stat-label" style="line-height:1.7">' + _L('스크럽과 삭제는 풀 상태를 먼저 확인한 뒤 실행합니다.', 'Run scrub and destroy only after checking pool health.') + '</div>');
      h += '</div>';
      h += '<div class="ops-section-heading"><div><h3>' + _L('풀 상태', 'Pool status') + '</h3><p>' + _L('각 풀의 용량과 건강 상태를 확인한 뒤 유지보수 작업을 선택합니다.', 'Review each pool before choosing maintenance actions.') + '</p></div></div>';
    }
    pl.forEach(v => {
      const sz = parseSize(v.size), us = parseSize(v.alloc || v.used), pct = storagePct(sz, us);
      h += H.card('&#128190; ' + escapeHtml(v.name) + ' ' + H.badge(v.health, v.health === 'ONLINE' ? 'g' : 'r'), H.row(_L('총 용량', 'Total size'), fmtBytes(sz)) + H.row(_L('사용량', 'Used'), fmtBytes(us)) + H.row(_L('건강 상태', 'Health'), escapeHtml(v.health || '-')) + renderProgressBar(Math.min(pct, 100)) + H.row(_L('사용률', 'Usage'), storagePctText(sz, us)) + '<div class="flex gap-4 ops-action-row" style="margin-top:10px"><button class="btn btn-soft" style="font-size:10px;padding:3px 8px" onclick="poolScrub(\'' + escapeAttr(v.name) + '\')">&#128260; ' + _L('스크럽', 'Scrub') + '</button><button class="btn btn-r" style="font-size:10px;padding:3px 8px" onclick="poolDestroy(\'' + escapeAttr(v.name) + '\')">&#128465; ' + _L('영구 삭제', 'Destroy') + '</button></div>', 'mb-8');
    });

    if (pl.length > 0) {
      h += '<div class="sg grid-2">';
      pl.forEach(function(v, pi) {
        var sz = parseSize(v.size), us = parseSize(v.alloc || v.used), pct = storagePct(sz, us);
        h += H.card(esc(v.name) + ' ' + _L('사용량', 'Usage'), '<div style="position:relative;width:120px;height:120px;margin:0 auto">'
          + '<canvas id="pool-donut-' + pi + '" width="120" height="120"></canvas>'
          + '<div style="position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);font-size:16px;font-weight:700;color:var(--accent)">' + pct.toFixed(0) + '%</div>'
          + '</div>' + H.row(_L('전체', 'Total'), fmtBytes(sz)) + H.row(_L('사용', 'Used'), fmtBytes(us)));
      });
      h += '</div>';
    }


    h += '<div class="hc mb-14"><h4>&#128200; ' + _L('용량 예측', 'Capacity planning') + '</h4>';
    h += '<p class="color-muted text-11 mb-8">' + _L('일별 증가량 기준으로 풀 소진 시점을 예측합니다. 확장이나 정리 시점을 먼저 판단하는 용도입니다.', 'Forecast pool exhaustion based on daily growth so you can plan expansion or cleanup ahead of time.') + '</p>';
    h += '<div id="storage-forecast"><span class="spinner"></span> ' + (t('loading') || 'Loading...') + '</div></div>';
    setTimeout(loadStorageForecast, 100);

    const z = await fetchGet(EP.STORAGE_ZVOLS());
    const zl = unwrapList(z);
    h += '<div class="ops-section-heading"><div><h3>' + _L('Zvol 볼륨', 'Zvol volumes') + '</h3><p>' + _L('디스크 볼륨은 표로 관리하고, 대량 삭제는 선택 상태에서만 노출합니다.', 'Manage disk volumes in a table and expose bulk delete only after selection.') + '</p></div><div class="flex gap-8 ops-action-row"><button class="btn btn-primary" onclick="showZvol()">+ ' + t('btn.create') + '</button> <button class="btn btn-r" id="zvol-bulk-del" style="display:none" onclick="zvolBulkDelete()">&#128465; ' + _L('선택 삭제', 'Delete Selected') + ' (<span id="zvol-sel-count">0</span>)</button></div></div>';
    if (zl.length === 0) { h += '<div class="empty-state"><div class="empty-state-icon">&#128190;</div><div class="empty-state-text">' + _L('아직 생성된 Zvol이 없습니다', 'No zvols created yet') + '</div><div class="color-muted text-12">' + _L('추가 디스크가 필요할 때 생성 버튼으로 바로 만들 수 있습니다.', 'Use the create button when a workload needs an additional disk.') + '</div></div>'; b.innerHTML = h;
      setTimeout(function() { pl.forEach(function(v, pi) { var canvas = document.getElementById('pool-donut-' + pi); if (!canvas) return; var ctx = canvas.getContext('2d'); var sz = parseSize(v.size), us = parseSize(v.alloc || v.used); var pct = sz > 0 ? us / sz : 0; var r = 50, cx = 60, cy = 60, lw = 14; ctx.lineWidth = lw; ctx.beginPath(); ctx.arc(cx, cy, r, 0, Math.PI * 2); ctx.strokeStyle = 'rgba(255,255,255,0.1)'; ctx.stroke(); ctx.beginPath(); ctx.arc(cx, cy, r, -Math.PI/2, -Math.PI/2 + Math.PI * 2 * pct); try { ctx.strokeStyle = getComputedStyle(document.documentElement).getPropertyValue(pct > 0.85 ? '--red' : pct > 0.6 ? '--yellow' : '--green').trim(); } catch(e) {} ctx.stroke(); }); }, 100);
      return; }
    if (!window._zvolSel) window._zvolSel = new Set();
    h += '<table class="table-sticky"><thead><tr><th><input type="checkbox" id="zvol-all" onclick="zvolToggleAll(this.checked)"></th><th>Path</th><th>Size</th><th>Used</th><th>' + _L('압축비', 'Compress') + '</th><th>Dedup</th><th>Written</th><th>' + t('vm.settings') + '</th></tr></thead><tbody>';
    zl.forEach(v => {
      const vn = v.name || v.path;
      const checked = window._zvolSel.has(vn) ? 'checked' : '';
      const rowCls = window._zvolSel.has(vn) ? ' class="multi-selected"' : '';
      h += `<tr${rowCls} oncontextmenu="event.preventDefault();zvolCtxMenu(event,'${escapeAttr(vn)}')"><td><input type="checkbox" ${checked} onclick="zvolToggleSel('${escapeAttr(vn)}',this.checked)"></td><td><span class="text-ellipsis" title="${escapeHtml(vn)}">${escapeHtml(vn)}</span></td><td>${escapeHtml(v.volsize || v.size || '-')}</td><td>${escapeHtml(v.used || '-')}</td><td>${escapeHtml(v.compression_ratio || '-')}</td><td>${escapeHtml(v.dedup || '-')}</td><td>${escapeHtml(v.written || '-')}</td><td><button class="btn btn-r" style="font-size:10px;padding:3px 8px" onclick="zvolDel('${escapeAttr(vn)}')">${t('btn.delete')}</button></td></tr>`;
    });
    b.innerHTML = h + '</tbody></table>';
    if (window._zvolSel.size > 0) {
      var bd = document.getElementById('zvol-bulk-del');
      var sc = document.getElementById('zvol-sel-count');
      if (bd) bd.style.display = '';
      if (sc) sc.textContent = window._zvolSel.size;
    }


    setTimeout(function() {
      pl.forEach(function(v, pi) {
        var canvas = document.getElementById('pool-donut-' + pi);
        if (!canvas) return;
        var ctx = canvas.getContext('2d');
        var sz = parseSize(v.size), us = parseSize(v.alloc || v.used);
        var pct = sz > 0 ? us / sz : 0;
        var r = 50, cx = 60, cy = 60, lw = 14;
        ctx.lineWidth = lw;
        ctx.beginPath(); ctx.arc(cx, cy, r, 0, Math.PI * 2);
        ctx.strokeStyle = 'rgba(255,255,255,0.1)'; ctx.stroke();
        ctx.beginPath(); ctx.arc(cx, cy, r, -Math.PI/2, -Math.PI/2 + Math.PI * 2 * pct);
        ctx.strokeStyle = pct > 0.85 ? 'var(--red)' : pct > 0.6 ? 'var(--yellow)' : 'var(--green)';
        try { ctx.strokeStyle = getComputedStyle(document.documentElement).getPropertyValue(pct > 0.85 ? '--red' : pct > 0.6 ? '--yellow' : '--green').trim(); } catch(e) {}
        ctx.stroke();
      });
    }, 100);
  } catch (e) { if(_DEBUG) console.warn('z:', e.message); }
}

function showPoolCreate() {
  showModal('<h2>Create ZFS Pool</h2><div class="fr"><label>Pool Name</label><input id="pc-name" placeholder="newpool"></div><div class="fr"><label>Disks (space sep)</label><input id="pc-disks" placeholder="/dev/sdb /dev/sdc"></div><div class="fr"><label>RAID Level</label><select id="pc-raid" style="width:100%;padding:6px;background:var(--bg);border:1px solid var(--border);color:var(--fg);border-radius:4px"><option value="">Stripe (default)</option><option value="mirror">Mirror</option><option value="raidz">RAIDZ</option><option value="raidz2">RAIDZ2</option></select></div><div class="text-right mt-12"><button class="btn btn-g" onclick="doPoolCreate()">' + t('btn.create') + '</button> <button class="btn btn-r" onclick="closeModal()">' + t('btn.cancel') + '</button></div>');
}

async function doPoolCreate() {
  var _btn = document.activeElement;
  if (_btn && _btn.tagName === 'BUTTON') { if (_btn.disabled) return; _btn.disabled = true; _btn.setAttribute('aria-busy', 'true'); }
  const name = document.getElementById('pc-name')?.value;
  const disks = document.getElementById('pc-disks')?.value;
  const raid = document.getElementById('pc-raid')?.value;
  if (!name) { toast(t('msg.pool_name_required'), false); if (_btn) { _btn.disabled = false; _btn.removeAttribute('aria-busy'); } return; }
  try {
    const r = await fetchPost(EP.STORAGE_POOLS(), { name, disks: disks || '', raid_level: raid || '' });
    if (r.error) { toast('Pool create failed: ' + (r.error.message || ''), false); return; }
    toast('Pool created: ' + name); addEvt('Pool created: ' + name);
    closeModal(); renderStorage(document.getElementById('cb'));
  } catch (e) { toast(e.message, false); }
  finally { if (_btn && _btn.tagName === 'BUTTON') { _btn.disabled = false; _btn.removeAttribute('aria-busy'); } }
}

async function poolScrub(name) {
  toast('Scrub started: ' + name);
  try {
    const r = await fetchPost(EP.STORAGE_SCRUB(), { name });
    if (r.error) { toast('Scrub failed: ' + (r.error.message || ''), false); return; }
    toast('Scrub initiated: ' + name); addEvt('Pool scrub: ' + name);
  } catch (e) { toast(e.message, false); }
}

async function poolDestroy(name) {

  destroyConfirm({
    title: 'ZFS Pool 영구 삭제',
    name: name,
    warning: 'ZFS 풀 ' + name + '의 모든 데이터가 영구 삭제됩니다. 복구 불가.',
    onConfirm: async function () {
      try {
        const r = await fetchDelete(EP.STORAGE_POOLS(), { name });
        if (r.error) { showToastQueued('Pool destroy failed: ' + (r.error.message || ''), false); return; }
        showToastQueued('Pool destroyed: ' + name);
        addEvt('Pool destroyed: ' + name);
        invalidateCache('storage');
        renderStorage(document.getElementById('cb'));
      } catch (e) { reportError('poolDestroy', e); }
    }
  });
}

async function showZvol() {
  var pool = 'pcvpool/vms';
  try { var cfg = await fetchGet(EP.CONFIG_DAEMON()); var d = cfg.result || cfg.data || cfg; if (d.storage && d.storage.zvol_pool) pool = d.storage.zvol_pool; } catch(e) {}
  showModal(`<h2>${t('btn.create')} Zvol</h2>`
    + `<div class="fr"><label>Name</label><input id="zn" placeholder="data-disk" oninput="document.getElementById('zvol-preview').textContent='${escapeHtml(pool)}/' + (this.value||'name')"></div>`
    + `<div class="fr"><label>Size GB</label><input id="zs" type="number" value="20" min="1" max="2048"></div>`
    + `<div style="margin:8px 0 4px;padding:8px 10px;background:var(--bg3);border:1px solid var(--border);border-radius:6px;font-size:11px;font-family:var(--font-mono)">`
    + `<span class="color-muted">${_L('생성 경로', 'Path')}:</span> <span id="zvol-preview" class="color-accent">${escapeHtml(pool)}/name</span></div>`
    + `<div class="text-right mt-12"><button class="btn btn-g" onclick="doZvol()">${t('btn.create')}</button> <button class="btn btn-r" onclick="closeModal()">${t('btn.cancel')}</button></div>`);
}
async function doZvol() {
  var _btn = document.activeElement;
  if (_btn && _btn.tagName === 'BUTTON') { if (_btn.disabled) return; _btn.disabled = true; _btn.setAttribute('aria-busy', 'true'); }
  var name = (document.getElementById('zn')?.value || '').trim();
  var size = +(document.getElementById('zs')?.value || 0);
  if (!name) { toast(_L('Zvol 이름을 입력하세요', 'Zvol name is required'), false); if (_btn) { _btn.disabled = false; _btn.removeAttribute('aria-busy'); } return; }
  if (!/^[a-zA-Z0-9][a-zA-Z0-9_.-]{0,63}$/.test(name)) { toast(_L('이름: 영문/숫자/_.- 만 허용', 'Name: [a-zA-Z0-9_.-] only'), false); if (_btn) { _btn.disabled = false; _btn.removeAttribute('aria-busy'); } return; }
  if (size < 1 || size > 2048) { toast(_L('크기: 1~2048 GB', 'Size: 1-2048 GB'), false); if (_btn) { _btn.disabled = false; _btn.removeAttribute('aria-busy'); } return; }
  try { await fetchPost(EP.STORAGE_ZVOLS(), { name: name, size_gb: size }); toast(t('stg.zvol_created')); addEvt(t('stg.zvol_created')); closeModal(); renderStorage(document.getElementById('cb')); } catch (e) { toast(e.message, false); }
  finally { if (_btn && _btn.tagName === 'BUTTON') { _btn.disabled = false; _btn.removeAttribute('aria-busy'); } }
}

function zvolDel(name) {
  showModal(`<h2 class="color-red">&#9888; ${t('btn.delete')} Zvol</h2>`
    + `<p class="mb-12">${_L('Zvol을 영구 삭제합니다. 이 작업은 되돌릴 수 없습니다.', 'Permanently destroy this zvol. This action cannot be undone.')}</p>`
    + `<div style="margin-bottom:12px;padding:8px 10px;background:var(--bg3);border:1px solid var(--border);border-radius:6px;font-family:var(--font-mono);font-size:12px"><span class="color-muted">${_L('대상', 'Target')}:</span> <b class="color-accent">${escapeHtml(name)}</b></div>`
    + `<p class="mb-8 text-11">${_L('확인을 위해 아래에 전체 zvol 경로를 입력하세요:', 'Type the full zvol path below to confirm:')}</p>`
    + `<div class="fr"><label>${_L('경로', 'Path')}</label><input id="del-zvol-confirm" placeholder="${escapeHtml(name)}"></div>`
    + `<div class="text-right mt-14"><button class="btn btn-r" onclick="doZvolDel('${escapeAttr(name)}')">${t('btn.delete')}</button> <button class="btn" onclick="closeModal()">${t('btn.cancel')}</button></div>`);
}

async function doZvolDel(name) { const c = document.getElementById('del-zvol-confirm')?.value; if (c !== name) { toast(t('vm.name_mismatch'), false); return; }
  const mc = document.getElementById('mc'); mc.innerHTML = '<h2 class="color-red">&#9888; ' + _L('Zvol 삭제 중', 'Destroying Zvol') + '</h2><p><b class="color-accent">' + escapeHtml(name) + '</b></p><div class="prog-bar"><div class="prog-fill" id="dz-p" class="w-pct-15"></div></div><div class="prog-status" id="dz-s"><span class="spinner"></span>' + _L('삭제 중...', 'Destroying...') + '</div>';
  const pf = document.getElementById('dz-p'), ps = document.getElementById('dz-s');
  try { pf.style.width = '50%';
    const d = await fetchDelete(EP.STORAGE_ZVOLS(), { name: name });
    if (d.error) { pf.style.background = 'var(--red)'; pf.style.width = '100%'; ps.innerHTML = '&#10060; ' + escapeHtml(d.error.message); toast(t('btn.delete') + ' failed', false); return; }
    pf.style.width = '100%'; ps.innerHTML = '&#9989; ' + t('stg.zvol_destroyed'); toast(t('stg.zvol_destroyed')); addEvt(t('stg.zvol_destroyed') + ': ' + name); setTimeout(() => { closeModal(); renderStorage(document.getElementById('cb')); }, 1500);
  } catch (e) { pf.style.width = '100%'; ps.innerHTML = '&#10060; ' + escapeHtml(e.message); toast(e.message, false); } }


async function loadStorageForecast() {
  var el = document.getElementById('storage-forecast'); if (!el) return;
  el.innerHTML = '<span class="spinner"></span> ' + (t('loading') || 'Loading...');
  try {
    var r = await fetchPost(EP.RPC(), { method: 'storage.pool.forecast', params: {} });
    var d = unwrapData(r);
    var pools = Array.isArray(d) ? d : (d.pools || [d]);
    if (pools.length === 0) { el.innerHTML = '<span class="color-muted">' + (t('storage.no_forecast') || 'No forecast data available') + '</span>'; return; }
    var h = '<table class="text-12"><thead><tr>'
      + '<th>' + (t('storage.pool') || 'Pool') + '</th>'
      + '<th>' + (t('storage.used_pct') || 'Used %') + '</th>'
      + '<th>' + (t('storage.daily_growth') || 'Daily Growth') + '</th>'
      + '<th>' + (t('storage.days_to_full') || 'Days to Full') + '</th>'
      + '<th>' + (t('storage.predicted_date') || 'Predicted Date') + '</th>'
      + '<th>' + (t('storage.status') || 'Status') + '</th>'
      + '</tr></thead><tbody>';
    pools.forEach(function(p) {
      var usedPct = (p.used_percent || p.used_pct || 0).toFixed(1);
      var dailyGrowth = p.daily_growth_gb || p.daily_growth || 0;
      var daysToFull = p.days_to_full || 0;
      var predDate = p.predicted_full_date || p.full_date || '-';
      var severity = _forecastSeverity(daysToFull);
      h += '<tr>';
      h += '<td><b>' + esc(p.name || p.pool || '-') + '</b></td>';
      h += '<td>' + renderProgressBar(parseFloat(usedPct)) + '<span class="text-xs">' + usedPct + '%</span></td>';
      h += '<td>' + (dailyGrowth > 0 ? dailyGrowth.toFixed(2) + ' GB/day' : '<span class="color-muted">stable</span>') + '</td>';
      h += '<td><span style="color:' + severity.color + ';font-weight:700">' + (daysToFull > 0 ? daysToFull + ' ' + (t('storage.days') || 'days') : '&#8734;') + '</span></td>';
      h += '<td><span class="text-11">' + esc(String(predDate)) + '</span></td>';
      h += '<td>' + H.badge(severity.label, severity.badge) + '</td>';
      h += '</tr>';
    });
    h += '</tbody></table>';
    el.innerHTML = h;
  } catch (e) {
    el.innerHTML = '<span class="color-muted">' + (t('storage.forecast_unavailable') || 'Forecast unavailable') + ': ' + esc(e.message) + '</span>';
  }
}

function _forecastSeverity(daysToFull) {
  if (daysToFull <= 0) return { color: 'var(--green)', label: 'Stable', badge: 'g' };
  if (daysToFull < 30) return { color: 'var(--red)', label: 'Critical', badge: 'r' };
  if (daysToFull < 60) return { color: 'var(--yellow)', label: 'Warning', badge: 'y' };
  return { color: 'var(--green)', label: 'Healthy', badge: 'g' };
}


async function renderIscsi(b) {
  b.innerHTML = showSkeleton();
  try {
    const r = await fetchGet(EP.ISCSI_TARGETS());
    const l = unwrapList(r);
    let h = H.section('iSCSI Targets');
    if (!Array.isArray(l) || l.length === 0) {
      h += '<div class="empty-state"><div class="empty-state-icon">&#128190;</div><div class="empty-state-text">No iSCSI targets configured</div></div>';
    } else {
      h += '<table><thead><tr><th>IQN</th><th>LUN</th><th>Size</th><th>State</th></tr></thead><tbody>';
      l.forEach(function(tgt) {
        h += '<tr><td><b>' + esc(tgt.iqn || tgt.name || '?') + '</b></td><td>' + (tgt.lun || '-') + '</td><td>' + (tgt.size || '-') + '</td><td>' + H.badge(tgt.state || '?', 'g') + '</td></tr>';
      });
      h += '</tbody></table>';
    }
    b.innerHTML = h;
  } catch (e) { b.innerHTML = H.section('iSCSI Targets') + '<p class="color-muted">Failed to load</p>'; }
}
window.renderIscsi = renderIscsi;


async function renderBackup(b) {
  var h = '<div class="flex items-center gap-10 mb-16"><span class="neon-blink color-cyan">&gt;&gt;</span><h2 style="font-family:var(--font-display);font-size:16px">' + _L('백업 관리', 'Backup Management') + '</h2></div>';


  h += '<div class="hc mb-14"><h4>' + _L('백업 정책', 'Backup Policies') + '</h4>';
  h += '<div class="flex gap-8 mb-8"><button class="btn btn-g" onclick="backupAddPolicy()">' + _L('정책 추가', 'Add Policy') + '</button></div>';
  h += '<div id="backup-policies" class="skeleton-box" style="min-height:100px"></div></div>';


  h += '<div class="hc mb-14"><h4>' + _L('스냅샷 히스토리', 'Snapshot History') + '</h4>';
  h += '<div class="flex gap-8 mb-8"><input id="backup-hist-vm" class="input" placeholder="VM name (empty=all)" class="w-200"><button class="btn" onclick="backupLoadHistory()">' + _L('조회', 'Search') + '</button></div>';
  h += '<div id="backup-history" class="skeleton-box" style="min-height:100px"></div></div>';


  h += '<div class="hc mb-14"><h4>' + _L('복원', 'Restore') + '</h4>';
  h += '<p class="stat-label">' + _L('VM의 스냅샷을 선택하여 롤백합니다.', 'Select a VM snapshot to rollback.') + '</p>';
  h += '<div class="flex gap-8 mt-8"><input id="backup-restore-vm" class="input" placeholder="VM name" class="w-160"><input id="backup-restore-snap" class="input" placeholder="Snapshot name" class="w-200"><button class="btn btn-r" onclick="backupRestore()">' + _L('롤백', 'Rollback') + '</button></div></div>';

  b.innerHTML = h;


  try {
    var r = await fetchPost(EP.RPC(), { jsonrpc: '2.0', method: 'backup.policy.list', params: {}, id: 'bp1' });
    var d = unwrapData(r);
    var policies = Array.isArray(d) ? d : (d && d.result ? unwrapList(d) : []);
    var pe = document.getElementById('backup-policies');
    if (pe) {
      if (policies.length === 0) {
        pe.innerHTML = '<div class="stat-label">' + _L('정책 없음', 'No policies') + '</div>';
      } else {
        var tbl = '<table class="tbl"><tr><th>VM</th><th>' + _L('주기(h)', 'Interval(h)') + '</th><th>' + _L('보존', 'Retention') + '</th><th>' + _L('활성', 'Enabled') + '</th><th></th></tr>';
        policies.forEach(function(p) {
          tbl += '<tr><td>' + esc(p.vm_name || '*') + '</td><td>' + (p.interval_hours || '-') + '</td><td>' + (p.retention_count || '-') + '</td><td>' + (p.enabled ? '<span class="color-green">ON</span>' : '<span class="color-red">OFF</span>') + '</td><td><button class="btn btn-sm btn-r" onclick="backupDeletePolicy(\'' + esc(p.vm_name) + '\')">' + _L('삭제', 'Del') + '</button></td></tr>';
        });
        tbl += '</table>';
        pe.innerHTML = tbl;
      }
    }
  } catch (e) {
    var pe2 = document.getElementById('backup-policies');
    if (pe2) pe2.innerHTML = '<div class="color-red">' + esc(e.message) + '</div>';
  }
}

function backupAddPolicy() {
  showModal(
    '<h2>' + _L('백업 정책 추가', 'Add Backup Policy') + '</h2>'
    + '<div class="fr"><label>VM</label><input id="bp-vm" class="input" placeholder="VM name (* = all)" value="*"></div>'
    + '<div class="fr"><label>' + _L('주기(시간)', 'Interval (hours)') + '</label><input id="bp-interval" class="input" type="number" value="24" min="1"></div>'
    + '<div class="fr"><label>' + _L('보존 수', 'Retention count') + '</label><input id="bp-retention" class="input" type="number" value="7" min="1"></div>'
    + '<div class="text-right mt-12"><button class="btn btn-g" onclick="doBackupAddPolicy()">' + _L('추가', 'Add') + '</button> <button class="btn btn-r" onclick="closeModal()">' + _L('취소', 'Cancel') + '</button></div>'
  );
}

async function doBackupAddPolicy() {
  var vm = (document.getElementById('bp-vm').value || '').trim() || '*';
  var interval = parseInt(document.getElementById('bp-interval').value) || 24;
  var retention = parseInt(document.getElementById('bp-retention').value) || 7;
  try {
    var r = await fetchPost(EP.RPC(), { jsonrpc:'2.0', method:'backup.policy.set', params:{ vm_name:vm, interval_hours:interval, retention_count:retention, enabled:true }, id:'bps1' });
    if (r.error) { toast(r.error.message || 'Failed', false); return; }
    toast(_L('정책 추가됨', 'Policy added'));
    closeModal();
    renderContent();
  } catch (e) { toast(e.message, false); }
}

async function backupLoadHistory() {
  var vm = (document.getElementById('backup-hist-vm').value || '').trim();
  var params = vm ? { vm_name: vm } : {};
  try {
    var r = await fetchPost(EP.RPC(), { jsonrpc:'2.0', method:'backup.history', params:params, id:'bh1' });
    var d = unwrapData(r);
    var snaps = Array.isArray(d) ? d : unwrapList(d);
    var el = document.getElementById('backup-history');
    if (!el) return;
    if (snaps.length === 0) { el.innerHTML = '<div class="stat-label">' + _L('스냅샷 없음', 'No snapshots') + '</div>'; return; }
    var tbl = '<table class="tbl"><tr><th>VM</th><th>Snapshot</th><th>Date</th></tr>';
    snaps.forEach(function(s) {
      tbl += '<tr><td>' + esc(s.vm_name || s.vm || '-') + '</td><td>' + esc(s.snapshot || s.name || '-') + '</td><td>' + esc(s.created_at || s.timestamp || '-') + '</td></tr>';
    });
    el.innerHTML = tbl + '</table>';
  } catch (e) {
    var el2 = document.getElementById('backup-history');
    if (el2) el2.innerHTML = '<div class="color-red">' + esc(e.message) + '</div>';
  }
}

async function backupRestore() {
  var vm = (document.getElementById('backup-restore-vm').value || '').trim();
  var snap = (document.getElementById('backup-restore-snap').value || '').trim();
  if (!vm || !snap) { toast(_L('VM과 스냅샷 이름을 입력하세요', 'Enter VM and snapshot name'), false); return; }
  if (!confirm(_L('정말 롤백하시겠습니까? 현재 상태를 잃을 수 있습니다.', 'Are you sure? This may lose current state.'))) return;
  try {
    var r = await fetchPost(EP.RPC(), { jsonrpc:'2.0', method:'backup.restore', params:{ vm_name:vm, snapshot:snap }, id:'br1' });
    if (r.error) { toast(r.error.message || 'Failed', false); return; }
    toast(_L('롤백 시작됨', 'Rollback started'));
  } catch (e) { toast(e.message, false); }
}

async function backupDeletePolicy(vm) {
  try {
    var r = await fetchPost(EP.RPC(), { jsonrpc:'2.0', method:'backup.policy.delete', params:{ vm_name:vm }, id:'bd1' });
    if (r.error) { toast(r.error.message, false); return; }
    toast(_L('정책 삭제됨', 'Policy removed'));
    renderContent();
  } catch (e) { toast(e.message, false); }
}

window.renderBackup = renderBackup;
window.backupAddPolicy = backupAddPolicy;
window.doBackupAddPolicy = doBackupAddPolicy;
window.backupLoadHistory = backupLoadHistory;
window.backupRestore = backupRestore;
window.backupDeletePolicy = backupDeletePolicy;


window.renderStorage = renderStorage;
window.showPoolCreate = showPoolCreate;
window.doPoolCreate = doPoolCreate;
window.poolScrub = poolScrub;
window.poolDestroy = poolDestroy;
window.showZvol = showZvol;
window.doZvol = doZvol;
window.zvolDel = zvolDel;
window.doZvolDel = doZvolDel;
window.loadStorageForecast = loadStorageForecast;


function zvolToggleSel(name, on) {
  if (!window._zvolSel) window._zvolSel = new Set();
  if (on) window._zvolSel.add(name); else window._zvolSel.delete(name);
  var bd = document.getElementById('zvol-bulk-del');
  var sc = document.getElementById('zvol-sel-count');
  if (bd) bd.style.display = window._zvolSel.size > 0 ? '' : 'none';
  if (sc) sc.textContent = window._zvolSel.size;
}
function zvolToggleAll(on) {
  if (!window._zvolSel) window._zvolSel = new Set();
  document.querySelectorAll('input[type=checkbox][onclick^="zvolToggleSel"]').forEach(function(cb) {
    cb.checked = on;
    var m = cb.getAttribute('onclick').match(/zvolToggleSel\('([^']+)'/);
    if (m) { if (on) window._zvolSel.add(m[1]); else window._zvolSel.delete(m[1]); }
  });
  var bd = document.getElementById('zvol-bulk-del');
  var sc = document.getElementById('zvol-sel-count');
  if (bd) bd.style.display = window._zvolSel.size > 0 ? '' : 'none';
  if (sc) sc.textContent = window._zvolSel.size;
}
async function zvolBulkDelete() {
  if (!window._zvolSel || window._zvolSel.size === 0) return;
  var names = Array.from(window._zvolSel);
  var ok = await customConfirm(_L('일괄 Zvol 삭제', 'Bulk Zvol Delete'),
    _L('선택한 ', 'Delete ') + names.length + _L(' 개 Zvol을 영구 삭제합니다. 되돌릴 수 없습니다.', ' zvols permanently? Cannot be undone.'));
  if (!ok) return;
  var failed = [];
  for (var i = 0; i < names.length; i++) {
    try {
      var r = await fetchDelete(EP.STORAGE_ZVOLS(), { name: names[i] });
      if (r && r.error) failed.push(names[i] + ': ' + (r.error.message || ''));
    }
    catch (e) { failed.push(names[i] + ': ' + e.message); }
  }
  window._zvolSel.clear();
  if (failed.length === 0) {
    if (typeof showToastQueued === 'function') showToastQueued(names.length + ' zvols deleted');
    else toast(names.length + ' zvols deleted');
  } else {
    if (typeof toastRetry === 'function') {
      toastRetry(failed.length + ' / ' + names.length + ' failed (R to retry)', { fn: zvolBulkDelete });
    } else {
      toast(failed.length + ' / ' + names.length + ' failed', false);
    }
  }
  renderStorage(document.getElementById('cb'));
}
function zvolCtxMenu(ev, name) {
  if (typeof showCtxMenu === 'function') {
    showCtxMenu(ev, [
      { label: '&#128465; ' + t('btn.delete'), fn: function(){ zvolDel(name); } },
      { label: '&#9881; ' + _L('선택', 'Select'), fn: function(){ zvolToggleSel(name, !window._zvolSel.has(name)); renderStorage(document.getElementById('cb')); } }
    ]);
  } else {

    if (window.confirm(_L('삭제하시겠습니까? ', 'Delete? ') + name)) zvolDel(name);
  }
}
window.zvolToggleSel = zvolToggleSel;
window.zvolToggleAll = zvolToggleAll;
window.zvolBulkDelete = zvolBulkDelete;
window.zvolCtxMenu = zvolCtxMenu;


PCV.storage = {
  renderStorage: renderStorage,
  showPoolCreate: showPoolCreate,
  doPoolCreate: doPoolCreate,
  poolScrub: poolScrub,
  poolDestroy: poolDestroy,
  showZvol: showZvol,
  doZvol: doZvol,
  zvolDel: zvolDel,
  doZvolDel: doZvolDel,
  loadStorageForecast: loadStorageForecast,
  renderIscsi: renderIscsi,
  renderBackup: renderBackup,
  backupAddPolicy: backupAddPolicy,
  doBackupAddPolicy: doBackupAddPolicy,
  backupLoadHistory: backupLoadHistory,
  backupRestore: backupRestore,
  backupDeletePolicy: backupDeletePolicy,
  zvolToggleSel: zvolToggleSel,
  zvolToggleAll: zvolToggleAll,
  zvolBulkDelete: zvolBulkDelete,
  zvolCtxMenu: zvolCtxMenu
};

})(window.PCV);






window.PCV = window.PCV || {};
(function(PCV) {

var _cloudPollTimer = null;


window.addEventListener('beforeunload', function() {
  if (_cloudPollTimer) { clearInterval(_cloudPollTimer); _cloudPollTimer = null; }
});


function _cloudCleanupTimer() {
  if (_cloudPollTimer) { clearInterval(_cloudPollTimer); _cloudPollTimer = null; }
}
window._cloudCleanupTimer = _cloudCleanupTimer;

async function renderCloudMigration(b) {
  b.innerHTML = showSkeleton();
  let h = H.section('&#9729; Cloud Migration — AWS EC2 &#8596; PureCVisor');
  h += '<div class="sg grid-2 mb-14">';


  h += '<div class="hc"><h4 style="color:var(--accent)">&#128229; Import (EC2 &#8594; PureCVisor)</h4>';
  h += '<p class="stat-label" style="margin-bottom:10px">AWS EC2 AMI를 PureCVisor VM으로 가져옵니다. EBS→S3→다운로드→qcow2 변환→VM 생성</p>';
  h += '<div class="fr"><label>VM Name</label><input id="cm-imp-name" placeholder="web-prod"></div>';
  h += '<div class="fr"><label>AMI ID</label><input id="cm-imp-ami" placeholder="ami-0abcdef1234"></div>';
  h += '<div class="fr"><label>Region</label><select id="cm-imp-region" style="width:100%;padding:6px;background:var(--bg);border:1px solid var(--border);color:var(--fg);border-radius:4px"><option value="ap-northeast-2">ap-northeast-2 (Seoul)</option><option value="us-east-1">us-east-1 (Virginia)</option><option value="us-west-2">us-west-2 (Oregon)</option><option value="eu-west-1">eu-west-1 (Ireland)</option><option value="ap-southeast-1">ap-southeast-1 (Singapore)</option></select></div>';
  h += '<div class="fr"><label>S3 Bucket</label><input id="cm-imp-bucket" placeholder="pcv-migration"></div>';
  h += '<div class="fr"><label>vCPU</label><input id="cm-imp-vcpu" type="number" value="2" min="1" max="64" style="width:80px"></div>';
  h += '<div class="fr"><label>Memory (MB)</label><input id="cm-imp-mem" type="number" value="2048" style="width:100px"></div>';
  h += '<div class="fr"><label>Bridge</label><input id="cm-imp-br" value="pcvbr0" style="width:120px"></div>';
  h += '<div class="fr"><label>Mode</label><select id="cm-imp-mode" style="width:100%;padding:6px;background:var(--bg);border:1px solid var(--border);color:var(--fg);border-radius:4px"><option value="standard">Standard (full download)</option><option value="near-live">Near-Live (2-phase, minimal downtime)</option></select></div>';
  h += '<button class="btn btn-g" onclick="cmDoImport()" style="margin-top:8px;width:100%">&#128229; Start Import</button>';
  h += '</div>';


  h += '<div class="hc"><h4 style="color:var(--green)">&#128230; Export (PureCVisor &#8594; EC2)</h4>';
  h += '<p class="stat-label" style="margin-bottom:10px">PureCVisor VM을 AWS EC2 AMI로 내보냅니다. qcow2→RAW→S3→AMI 등록</p>';
  h += '<div class="fr"><label>VM Name</label><select id="cm-exp-name" style="width:100%;padding:6px;background:var(--bg);border:1px solid var(--border);color:var(--fg);border-radius:4px"><option value="">' + t('loading') + '</option></select></div>';
  h += '<div class="fr"><label>Region</label><select id="cm-exp-region" style="width:100%;padding:6px;background:var(--bg);border:1px solid var(--border);color:var(--fg);border-radius:4px"><option value="ap-northeast-2">ap-northeast-2 (Seoul)</option><option value="us-east-1">us-east-1 (Virginia)</option><option value="us-west-2">us-west-2 (Oregon)</option><option value="eu-west-1">eu-west-1 (Ireland)</option></select></div>';
  h += '<div class="fr"><label>S3 Bucket</label><input id="cm-exp-bucket" placeholder="pcv-migration"></div>';
  h += '<div class="fr"><label>AMI Name</label><input id="cm-exp-ami-name" placeholder="web-prod-exported"></div>';
  h += '<div class="fr"><label>Description</label><input id="cm-exp-desc" placeholder="Exported from PureCVisor"></div>';
  h += '<button class="btn btn-g" onclick="cmDoExport()" style="margin-top:8px;width:100%">&#128230; Start Export</button>';
  h += '</div></div>';


  h += '<div class="hc" style="margin-bottom:14px"><h4>&#128202; Migration Jobs</h4>';
  h += '<div id="cm-jobs"><span class="spinner"></span> Loading...</div></div>';


  h += H.card('&#128736; Pipeline Reference', '<div style="font-size:11px;line-height:1.8;color:var(--fg2)">'
    + '<b style="color:var(--accent)">Import:</b> aws sts verify &#8594; ec2 export-image &#8594; S3 download &#8594; qemu-img convert &#8594; virt-customize &#8594; VM define<br>'
    + '<b style="color:var(--green)">Export:</b> qemu-img convert &#8594; S3 upload &#8594; ec2 import-image &#8594; AMI ready<br>'
    + '<b style="color:var(--yellow)">Near-Live:</b> Phase1 사전동기화(실행 중) &#8594; Phase2 델타전송(2~5분 중단)</div>');

  b.innerHTML = h;


  try {
    const vl = vmList.length ? vmList : [];
    const sel = document.getElementById('cm-exp-name');
    if (sel && vl.length) {
      sel.innerHTML = vl.map(v => '<option value="' + escapeHtml(v.name) + '">' + escapeHtml(v.name) + ' (' + v.state + ')</option>').join('');
    } else if (sel) { sel.innerHTML = '<option value="">No VMs</option>'; }
  } catch (e) {  }


  cmLoadJobs();
  if (_cloudPollTimer) clearInterval(_cloudPollTimer);
  _cloudPollTimer = setInterval(cmLoadJobs, 5000);
}
window.renderCloudMigration = renderCloudMigration;

async function cmLoadJobs() {
  const el = document.getElementById('cm-jobs');
  if (!el) { if (_cloudPollTimer) { clearInterval(_cloudPollTimer); _cloudPollTimer = null; } return; }

  try {
    const r = await fetchGet(EP.CLOUD_JOBS());
    const jobs = unwrapList(r);
    if (!Array.isArray(jobs) || jobs.length === 0) {
      el.innerHTML = '<p class="color-muted" style="font-size:12px">No migration jobs. Start an Import or Export above.</p>';
      return;
    }

    let html = '<table><thead><tr><th>VM</th><th>Dir</th><th>Status</th><th>Progress</th><th>Detail</th><th>Elapsed</th><th></th></tr></thead><tbody>';
    for (const j of jobs) {
      const pct = j.progress_percent || 0;
      const st = j.status || '?';
      const color = st === 'done' ? 'var(--green)' : st === 'failed' ? 'var(--red)' : 'var(--accent)';
      const active = st !== 'done' && st !== 'failed';
      const awaitingCutover = st === 'awaiting_cutover';
      const cancelBtn = active && !awaitingCutover
        ? '<button class="btn btn-r" style="font-size:10px;padding:2px 8px" onclick="cmCancelJob(\'' + esc(j.name) + '\')">Cancel</button>'
        : '';
      const finalizeBtn = awaitingCutover
        ? '<button class="btn btn-g" style="font-size:10px;padding:2px 8px" onclick="cmFinalize(\'' + esc(j.name) + '\')">Finalize</button>'
        : '';
      html += '<tr>'
        + '<td><b>' + esc(j.name || '') + '</b></td>'
        + '<td>' + H.badge(j.direction || '?', j.direction === 'import' ? 'y' : 'g') + '</td>'
        + '<td>' + H.badge(st, st === 'done' ? 'g' : st === 'failed' ? 'r' : awaitingCutover ? 'y' : 'y') + '</td>'
        + '<td><div class="pb" style="min-width:120px"><div class="pb-f" style="width:' + pct + '%;background:' + color + '"></div><div class="pb-t">' + pct + '%</div></div></td>'
        + '<td class="text-xs">' + esc(j.detail || '-') + '</td>'
        + '<td class="text-xs">' + (j.elapsed_sec || 0) + 's</td>'
        + '<td>' + cancelBtn + finalizeBtn + '</td>'
        + '</tr>';
    }
    html += '</tbody></table>';
    el.innerHTML = html;
  } catch (e) {  }
}
window.cmLoadJobs = cmLoadJobs;

async function cmCancelJob(name) {
  if (!await customConfirm('Cancel migration job for ' + name + '?')) return;
  try {
    const r = await fetchPost(EP.CLOUD_CANCEL(), { name });
    if (r.error) { toast('Cancel failed: ' + (r.error.message || ''), false); return; }
    toast('Cancel requested: ' + name);
    cmLoadJobs();
  } catch (e) { toast('Cancel error: ' + e.message, false); }
}
window.cmCancelJob = cmCancelJob;

async function cmDoImport() {
  const name = document.getElementById('cm-imp-name')?.value;
  const ami = document.getElementById('cm-imp-ami')?.value;
  if (!name || !ami) { toast(t('msg.name_required'), false); return; }
  if (!/^ami-[a-f0-9]{8,17}$/.test(ami)) { toast(t('msg.invalid_ami'), false); return; }
  const body = {
    ami_id: ami,
    aws_region: document.getElementById('cm-imp-region')?.value || 'ap-northeast-2',
    s3_bucket: document.getElementById('cm-imp-bucket')?.value || '',
    vcpu: parseInt(document.getElementById('cm-imp-vcpu')?.value) || 2,
    memory_mb: parseInt(document.getElementById('cm-imp-mem')?.value) || 2048,
    network_bridge: document.getElementById('cm-imp-br')?.value || 'pcvbr0',
    mode: document.getElementById('cm-imp-mode')?.value || 'standard'
  };
  try {
    const r = await fetchPost(EP.CLOUD_IMPORT(name), body);
    if (r.error) { toast('Import failed: ' + (r.error.message || ''), false); return; }
    const d = unwrapData(r);
    toast('Import started: ' + name + ' (job: ' + (d.job_id || '') + ')');
    addEvt('Cloud Import started — ' + name + ' \u2190 AMI ' + ami);
    cmLoadJobs();
  } catch (e) { toast('Import error: ' + e.message, false); }
}
window.cmDoImport = cmDoImport;

async function cmDoExport() {
  const name = document.getElementById('cm-exp-name')?.value;
  if (!name) { toast('VM Name required', false); return; }
  const body = {
    aws_region: document.getElementById('cm-exp-region')?.value || 'ap-northeast-2',
    s3_bucket: document.getElementById('cm-exp-bucket')?.value || '',
    ami_name: document.getElementById('cm-exp-ami-name')?.value || '',
    ami_description: document.getElementById('cm-exp-desc')?.value || ''
  };
  try {
    const r = await fetchPost(EP.CLOUD_EXPORT(name), body);
    if (r.error) { toast('Export failed: ' + (r.error.message || ''), false); return; }
    const d = unwrapData(r);
    toast('Export started: ' + name + ' (job: ' + (d.job_id || '') + ')');
    addEvt('Cloud Export started — ' + name + ' \u2192 EC2 AMI');
    cmLoadJobs();
  } catch (e) { toast('Export error: ' + e.message, false); }
}
window.cmDoExport = cmDoExport;

async function cmFinalize(name) {
  if (!await customConfirm('Finalize Near-Live Import for ' + name + '?',
    'This will stop the EC2 instance, download delta changes, and start the VM in PureCVisor. Downtime: ~2-5 minutes.')) return;
  try {
    const r = await fetchPost(EP.CLOUD_IMPORT(name), { finalize: true });
    if (r.error) { toast('Finalize failed: ' + (r.error.message || ''), false); return; }
    toast('Finalize started: ' + name);
    cmLoadJobs();
  } catch (e) { toast('Finalize error: ' + e.message, false); }
}
window.cmFinalize = cmFinalize;


PCV.cloud = {
  renderCloudMigration: renderCloudMigration,
  cmLoadJobs: cmLoadJobs,
  cmCancelJob: cmCancelJob,
  cmDoImport: cmDoImport,
  cmDoExport: cmDoExport,
  cmFinalize: cmFinalize,
  _cloudCleanupTimer: _cloudCleanupTimer
};

})(window.PCV);




















var _L = window._L = function(ko, en) {
  return (typeof I18N !== 'undefined' && I18N.getLang() === 'en') ? en : ko;
};

window.PCV = window.PCV || {};
(function(PCV) {


function renderHelp(b) {
  var h = H.sectionLg(_L('도움말 & 참조', 'Help & Reference'));
  h += '<div style="margin-bottom:20px;padding:16px 20px;background:linear-gradient(135deg,rgba(0,240,255,0.08),rgba(0,255,136,0.05));border:1px solid var(--accent);border-radius:8px;display:flex;align-items:center;gap:16px;flex-wrap:wrap">'
    + '<div style="flex:1;min-width:200px"><div style="font-size:15px;font-weight:600;color:var(--accent);margin-bottom:4px">' + _L('PureCVisor 완벽 가이드', 'PureCVisor Complete Guide') + '</div>'
    + '<div style="font-size:12px;color:var(--fg2)">' + _L('18개 챕터, 설치부터 트러블슈팅까지 전체 문서를 ReadTheDocs 스타일로 탐색하세요.', 'Browse all 18 chapters from installation to troubleshooting in a ReadTheDocs-style viewer.') + '</div></div>'
    + '<a href="/ui/guide.html" target="_blank" style="display:inline-flex;align-items:center;gap:6px;padding:8px 20px;background:var(--accent);color:#000;border-radius:6px;font-size:13px;font-weight:600;text-decoration:none;white-space:nowrap">&#128214; ' + _L('가이드 열기', 'Open Guide') + '</a></div>';
  h += '<div class="mb-16"><input id="help-search" class="sb-search" placeholder="' + t('search') + '" oninput="filterHelp()" style="max-width:600px;font-size:15px;padding:10px 14px;border-radius:8px"></div>';
  h += '<div id="help-content">';
  var helpData = buildHelpData();
  function buildHelpData() {
    var data = [
    { cat: _L('VM 관리', 'VM Management'), items: [
      { cmd: 'vm.list / vm.create / vm.delete', cli: 'pcvctl vm list/create/delete', tui: 'F1', web: _L('VM 라이브러리', 'VM Library'), desc: _L('VM 라이프사이클 — 생성, 시작, 중지, 삭제, 복제, 내보내기 (operator는 소유 VM 한정)', 'VM lifecycle — create, start, stop, delete, clone, export (operators are limited to owned VMs)') },
      { cmd: 'vm.start / vm.stop / vm.pause / vm.resume', cli: 'pcvctl vm start/stop', tui: 'F1: s/x/p', web: _L('전원 버튼 + 컨텍스트 메뉴', 'Power buttons + Context menu'), desc: _L('전원 제어 (실시간 프로그레스 모달)', 'Power control with real-time progress modal') },
      { cmd: 'vm.snapshot.create/list/rollback/delete/delete_all', cli: 'pcvctl vm snapshot ...', tui: 'F1: S', web: _L('스냅샷 탭', 'Snapshots tab'), desc: _L('ZFS 스냅샷 CRUD + 일괄 삭제 + 니어라이브', 'ZFS snapshot CRUD + bulk delete + near-live') },
      { cmd: 'vm.set_vcpu / vm.set_memory / vm.resize_disk', cli: 'pcvctl vm set-vcpu/set-mem', tui: '-', web: _L('설정 모달', 'Settings modal'), desc: _L('핫 리소스 조정', 'Hot resource adjustment') },
      { cmd: 'device.nic.list/attach/detach', cli: 'pcvctl nic list/add/remove', tui: 'F1: N/+/-', web: _L('NIC 관리자', 'NIC Manager'), desc: _L('NIC 핫플러그', 'NIC hotplug') },
      { cmd: 'vm.mount_iso / vm.eject', cli: 'pcvctl iso mount/eject', tui: '-', web: _L('설정 > CD', 'Settings > CD'), desc: _L('ISO 마운트/꺼내기', 'ISO mount/eject') },
      { cmd: 'vm.create (nic_type/pci_addr)', cli: 'pcvctl vm create --nic-type sriov', tui: '-', web: _L('VM 생성 모달', 'VM Create modal'), desc: _L('NIC 타입 선택 (bridge/dpdk/sriov) + PCI 주소', 'NIC type selection (bridge/dpdk/sriov) + PCI address') },
    ]},
    { cat: _L('컨테이너 (LXC)', 'Containers (LXC)'), items: [
      { cmd: 'container.list/create/start/stop/destroy', cli: 'pcvctl container ...', tui: 'F4', web: _L('컨테이너 라이브러리', 'Container Library'), desc: _L('LXC 라이프사이클 (프로그레스 모달)', 'LXC lifecycle with progress modal') },
      { cmd: 'container.exec', cli: 'pcvctl container exec', tui: 'F4: E', web: _L('콘솔 탭', 'Console tab'), desc: _L('컨테이너 명령 실행', 'Execute command in container') },
      { cmd: 'container.snapshot.create/rollback/delete', cli: 'pcvctl container snap ...', tui: 'F4: 3', web: _L('스냅샷 탭', 'Snapshots tab'), desc: _L('컨테이너 ZFS 스냅샷', 'Container ZFS snapshots') },
      { cmd: 'container.nic.list/attach/detach', cli: 'pcvctl container nic ...', tui: 'F4: N', web: _L('네트워크 탭', 'Network tab'), desc: _L('컨테이너 NIC 관리', 'Container NIC management') },
      { cmd: 'container.set_limits / set_bandwidth', cli: 'pcvctl container limits', tui: 'F4: L', web: _L('리소스 탭', 'Resources tab'), desc: _L('CPU/메모리 제한, 대역폭 QoS', 'CPU/memory limits, bandwidth QoS') },
      { cmd: 'container.clone', cli: 'pcvctl container clone', tui: '-', web: _L('컨테이너 탭', 'Container tab'), desc: _L('LXC 컨테이너 클론 (lxc-copy, ZFS 기반)', 'LXC container clone (lxc-copy, ZFS-backed)') },
      { cmd: 'container.checkpoint/restore', cli: 'pcvctl container checkpoint/restore', tui: '-', web: '-', desc: _L('CRIU 체크포인트/복원', 'CRIU checkpoint/restore') },
    ]},
    { cat: _L('네트워크', 'Network'), items: [
      { cmd: 'network.create/delete/list/info', cli: 'pcvctl network ...', tui: 'F2', web: _L('네트워크 페이지', 'Networks page'), desc: _L('NAT/격리/라우팅/브릿지 네트워크 CRUD', 'NAT/Isolated/Routed/Bridge network CRUD') },
      { cmd: 'ovn.status/switch.*/router.*/acl.*/nat.*', cli: 'pcvctl ovn ...', tui: 'F7', web: 'OVN SDN', desc: _L('OVN 논리 스위치/라우터, ACL, NAT', 'OVN logical switches, routers, ACL, NAT') },
      { cmd: 'overlay.list', cli: 'pcvctl overlay list', tui: 'F6: o', web: _L('오버레이 네트워크', 'Overlay Networks'), desc: _L('VXLAN 오버레이 메시', 'VXLAN overlay mesh') },
      { cmd: 'security_group.*', cli: 'pcvctl sg ...', tui: 'F7: G', web: _L('보안 그룹', 'Security Groups'), desc: _L('NFV 보안 그룹 규칙', 'NFV security group rules') },
    ]},
    { cat: _L('스토리지', 'Storage'), items: [
      { cmd: 'storage.pool.list/create/destroy/scrub', cli: 'pcvctl storage pool ...', tui: 'F3', web: _L('스토리지 페이지', 'Storage page'), desc: _L('ZFS 풀 관리', 'ZFS pool management') },
      { cmd: 'storage.zvol.list/create/delete', cli: 'pcvctl storage zvol ...', tui: 'F3: z/Z', web: _L('스토리지 페이지', 'Storage page'), desc: _L('ZFS zvol 관리', 'ZFS zvol management') },
      { cmd: 'iscsi.target.list', cli: 'pcvctl iscsi list', tui: 'F3: I', web: _L('iSCSI 타겟', 'iSCSI Targets'), desc: _L('iSCSI 타겟 관리', 'iSCSI target management') },
      { cmd: 'storage.pool.health', cli: 'pcvctl storage pool health', tui: 'F3: h', web: _L('스토리지 페이지', 'Storage page'), desc: _L('ZFS 풀 상태/scrub/용량 모니터링', 'ZFS pool health/scrub/capacity monitoring') },
      { cmd: 'zfs.promote', cli: 'pcvctl storage promote', tui: '-', web: '-', desc: _L('ZFS 클론 독립화 (promote)', 'ZFS clone promote to independent dataset') },
    ]},
    ];
    return data;
  }
  window.buildHelpData = buildHelpData;
  var helpDataDetail = [
    { cat: _L('모니터링', 'Monitoring'), items: [
      { cmd: 'telemetry.host / monitor.fleet', cli: 'pcvctl monitor ...', tui: 'F5', web: _L('모니터링 6탭', 'Monitoring 6 tabs'), desc: _L('CPU/메모리/디스크/네트워크 실시간 메트릭', 'CPU/Mem/Disk/Net real-time metrics') },
      { cmd: 'alert.config.get/set / alert.history', cli: 'pcvctl alert ...', tui: 'F5: a', web: _L('알림 페이지', 'Alerts page'), desc: _L('알림 엔진 설정 + 이력', 'Alert engine config + history') },
      { cmd: 'audit.search', cli: 'pcvctl audit ...', tui: 'F5: d', web: _L('감사 로그', 'Audit Log'), desc: _L('감사 로그 검색', 'Audit log search') },
      { cmd: 'alert.acknowledge / alert.sla', cli: 'pcvctl alert ack/sla', tui: '-', web: _L('알림 페이지', 'Alerts page'), desc: _L('알림 확인(ACK) + VM SLA 추적', 'Alert acknowledge + VM SLA tracking') },
      { cmd: 'ai.healing.pending/approve/reject', cli: 'pcvctl agent approve/reject', tui: '-', web: _L('모니터링 Overview', 'Monitoring Overview'), desc: _L('자가치유 대기 액션 승인/거절', 'Self-healing pending action approve/reject') },
    ]},
    { cat: _L('클라우드 마이그레이션', 'Cloud Migration'), items: [
      { cmd: 'vm.import.ec2 / vm.export.ec2', cli: 'pcvctl cloud import/export', tui: 'F6: w', web: _L('클라우드 마이그레이션', 'Cloud Migration'), desc: _L('AWS EC2 ↔ PureCVisor VM 이전', 'AWS EC2 ↔ PureCVisor VM migration') },
      { cmd: 'vm.import.ec2 (mode=near-live)', cli: 'pcvctl cloud import --mode near-live', tui: '-', web: _L('Import 모드 선택', 'Import Mode selector'), desc: _L('니어라이브 2단계 (Phase 1 + Finalize)', 'Near-Live 2-Phase (Phase 1 + Finalize)') },
      { cmd: 'cloud.jobs.list / cloud.job.cancel', cli: 'pcvctl cloud jobs/cancel', tui: 'F6: w', web: _L('클라우드 마이그레이션', 'Cloud Migration'), desc: _L('마이그레이션 작업 관리', 'Migration job management') },
    ]},
    { cat: _L('고급 기능', 'Advanced'), items: [
      { cmd: 'dpdk.status/bind/unbind/list', cli: 'pcvctl dpdk ...', tui: '-', web: 'DPDK', desc: _L('OVS-DPDK NIC 바인딩', 'OVS-DPDK NIC binding') },
      { cmd: 'sriov.status/enable/disable/attach/detach', cli: 'pcvctl sriov ...', tui: '-', web: 'SR-IOV', desc: _L('SR-IOV VF 관리', 'SR-IOV VF management') },
      { cmd: 'gpu.list / gpu.metrics', cli: 'pcvctl gpu ...', tui: 'F6: g', web: 'GPU', desc: _L('GPU 인벤토리 + 메트릭', 'GPU inventory + metrics') },
      { cmd: 'template.list/create/get/delete', cli: 'pcvctl template ...', tui: 'F6: t', web: _L('템플릿', 'Templates'), desc: _L('VM 템플릿 관리', 'VM template management') },
      { cmd: 'backup.policy.list/set/delete', cli: 'pcvctl backup ...', tui: 'F3: b', web: _L('백업', 'Backup'), desc: _L('백업 정책 + 이력', 'Backup policy + history') },
      { cmd: 'agent.config.get/set / agent.history', cli: 'pcvctl agent ...', tui: '-', web: _L('AI 에이전트', 'AI Agent'), desc: _L('AI 에이전트 멀티 프로바이더 합의', 'AI Agent multi-provider consensus') },
    ]},
    { cat: _L('백업 & 복원', 'Backup & Restore'), items: [
      { cmd: 'backup.list/set/remove', cli: 'pcvctl backup list/set', tui: 'F3: b', web: _L('백업 페이지', 'Backup page'), desc: _L('백업 정책 CRUD (주기/보존/활성화)', 'Backup policy CRUD (interval/retention/enable)') },
      { cmd: 'backup.history', cli: 'pcvctl backup history', tui: '-', web: _L('백업 페이지', 'Backup page'), desc: _L('스냅샷 히스토리 조회', 'Snapshot history query') },
      { cmd: 'backup.restore', cli: 'pcvctl backup restore', tui: '-', web: _L('백업 페이지', 'Backup page'), desc: _L('ZFS 스냅샷 롤백 복원', 'ZFS snapshot rollback restore') },
      { cmd: 'backup.replicate', cli: 'pcvctl backup replicate', tui: '-', web: '-', desc: _L('ZFS 원격 복제 (SSH, 원격 보존 정책)', 'ZFS remote replication (SSH, remote retention)') },
    ]},
  ];
  helpData = helpData.concat(helpDataDetail);
  helpData.forEach(function(cat) {
    h += '<div class="hc mb-14"><h4 class="color-accent">' + cat.cat + '</h4><table style="font-size:12px"><thead><tr><th>RPC</th><th>CLI</th><th>TUI</th><th>Web UI</th><th>' + _L('설명', 'Description') + '</th></tr></thead><tbody>';
    cat.items.forEach(function(i) { h += '<tr data-search="' + (i.cmd + ' ' + i.cli + ' ' + i.desc).toLowerCase() + '"><td style="color:var(--accent);font-family:var(--font-mono);font-size:11px">' + i.cmd + '</td><td style="font-size:11px">' + i.cli + '</td><td>' + i.tui + '</td><td>' + i.web + '</td><td class="color-muted">' + i.desc + '</td></tr>'; });
    h += '</tbody></table></div>';
  });
  h += '</div>';
  h += '<div class="sg grid-3">';
  h += H.card(_L('&#9881; 시스템', '&#9881; System'), H.row(_L('RPC 메서드', 'RPC Methods'), (typeof PCV !== 'undefined' ? PCV.config.RPC_COUNT : 264) + '+') + H.row(_L('REST 엔드포인트', 'REST Endpoints'), (typeof PCV !== 'undefined' ? PCV.config.REST_COUNT : 195) + '+') + H.row(_L('CLI 커맨드', 'CLI Commands'), '168'));
  h += H.card(_L('&#128200; 모니터링', '&#128200; Monitoring'), H.row('node_*', '126') + H.row('purecvisor_*', '44') + H.row(_L('Prometheus 합계', 'Total Prometheus'), '' + (typeof PCV !== 'undefined' ? PCV.config.METRICS_COUNT : 170)));
  h += H.card(_L('&#128187; 인프라', '&#128187; Infrastructure'), H.row(_L('3노드 클러스터', '3-Node Cluster'), 'HA Active') + H.row(_L('Web UI 모듈', 'Web UI Modules'), '20 (12K LOC)') + H.row(_L('i18n 키', 'i18n Keys'), '280+'));
  h += '</div>';
  b.innerHTML = h;
}
window.renderHelp = renderHelp;

function filterHelp() { var q = document.getElementById('help-search').value.toLowerCase(); document.querySelectorAll('#help-content tr[data-search]').forEach(function(r) { r.style.display = !q || r.dataset.search.includes(q) ? '' : 'none'; }); }
window.filterHelp = filterHelp;


function renderRestGuide(b) {
  var h = H.sectionLg(_L('REST API 가이드', 'REST API Guide'));
  h += '<div class="sg grid-2">';
  h += H.card(_L('&#128274; 인증', '&#128274; Authentication'), '<div style="font-size:13px;line-height:1.8">'
    + '<div style="border-left:3px solid var(--accent);padding-left:12px;margin-bottom:10px"><b>1. ' + _L('로그인', 'Login') + '</b><pre style="background:var(--bg);padding:8px;border-radius:4px;font-size:11px;color:var(--green);margin-top:4px">curl -X POST /api/v1/auth/token \\\n  -d \'{"username":"admin","password":"&lt;configured-admin-password&gt;"}\'</pre></div>'
    + '<div style="border-left:3px solid var(--green);padding-left:12px;margin-bottom:10px"><b>2. ' + _L('토큰 사용', 'Use Token') + '</b><pre style="background:var(--bg);padding:8px;border-radius:4px;font-size:11px;color:var(--green);margin-top:4px">curl -H "Authorization: Bearer eyJ..." \\\n  /api/v1/vms</pre></div>'
    + '<div style="border-left:3px solid var(--yellow);padding-left:12px"><b>3. ' + _L('쓰기 작업', 'Write Operations') + '</b><pre style="background:var(--bg);padding:8px;border-radius:4px;font-size:11px;color:var(--green);margin-top:4px">curl -X POST \\\n  -H "Authorization: Bearer eyJ..." \\\n  /api/v1/vms/web-prod/start</pre></div></div>');
  h += H.card(_L('&#128100; RBAC 역할', '&#128100; RBAC Roles'),
    '<table style="font-size:13px;width:100%"><thead><tr><th>' + _L('역할', 'Role') + '</th><th>' + _L('레벨', 'Level') + '</th><th>' + _L('권한', 'Permissions') + '</th></tr></thead><tbody>'
    + '<tr><td>' + H.badge('VIEWER', 'g') + '</td><td>0</td><td>' + _L('읽기 전용 (GET)', 'Read-only (GET endpoints)') + '</td></tr>'
    + '<tr><td>' + H.badge('OPERATOR', 'y') + '</td><td>1</td><td>' + _L('VM/컨테이너 운영, VM action은 생성자 범위', 'VM/Container operations, VM actions scoped to creator') + '</td></tr>'
    + '<tr><td>' + H.badge('ADMIN', 'r') + '</td><td>2</td><td>' + _L('전체 관리자', 'Full access') + '</td></tr></tbody></table>'
    + '<div style="margin-top:10px;font-size:12px;color:var(--fg2)">' + _L('내장 기본 비밀번호 없음. 첫 로그인 전 daemon.conf 또는 PURECVISOR_ADMIN_PASSWORD로 bootstrap 비밀번호를 설정합니다.', 'No built-in default password. Set the bootstrap password in daemon.conf or PURECVISOR_ADMIN_PASSWORD before first login.') + '</div>'
    + '<div style="margin-top:6px;font-size:12px;color:var(--fg2)">' + _L('OPERATOR는 libvirt domain metadata의 owner가 본인인 VM에만 시작, 중지, 삭제, 스냅샷, VNC, 일괄 작업을 수행할 수 있습니다.', 'OPERATOR can start, stop, delete, snapshot, access VNC, and batch-operate only VMs whose libvirt domain metadata owner matches the caller.') + '</div>');
  h += '</div>';

  h += '<div class="sg grid-3">';
  h += H.card(_L('&#128737; 보안', '&#128737; Security'), H.row(_L('속도 제한', 'Rate Limit'), _L('600 IP / 1200 유저 / 60 인증', '600 IP / 1200 user / 60 auth')) + H.row(_L('JWT 알고리즘', 'JWT Algorithm'), 'HS256') + H.row(_L('JWT 만료', 'JWT Expiry'), '900s (15min) + refresh 7d') + H.row('CORS', _L('화이트리스트', 'Whitelist')) + H.row('ETag', _L('GET 응답 조건부 캐싱 (304)', 'GET conditional caching (304)')) + H.row(_L('JWT IP 바인딩', 'JWT IP Binding'), _L('선택적 클라이언트 IP 검증', 'Optional client IP verification')));
  h += H.card(_L('&#127760; 엔드포인트', '&#127760; Endpoints'), H.row(_L('기본 URL', 'Base URL'), '<code>/api/v1</code>') + H.row(_L('포트', 'Port'), '80 / 443') + H.row(_L('합계', 'Total'), (typeof PCV !== 'undefined' ? PCV.config.REST_COUNT : 195) + '+') + H.row('WebSocket', '<code>/ws/events</code>'));
  h += H.card(_L('&#128196; 응답 형식', '&#128196; Response Format'), H.row(_L('성공', 'Success'), '<code>{"data": ...}</code>') + H.row(_L('에러', 'Error'), '<code>{"error": {code, message}}</code>') + H.row('Content-Type', 'application/json') + H.row(_L('캐시', 'Cache'), _L('ETag + max-age=5 (GET) / no-store (POST)', 'ETag + max-age=5 (GET) / no-store (POST)')) + H.row(_L('페이지네이션', 'Pagination'), 'X-Total-Count + Link rel="next/prev"'));
  h += '</div>';

  h += H.section('curl ' + _L('예제', 'Examples'));
  var examples = [
    { title: _L('VM 목록', 'List VMs'), cmd: 'curl -s -H "Authorization: Bearer $TOKEN" \\\n  http://HOST/api/v1/vms | jq' },
    { title: _L('VM 생성', 'Create VM'), cmd: 'curl -X POST -H "Authorization: Bearer $TOKEN" \\\n  -H "Content-Type: application/json" \\\n  -d \'{"name":"web","vcpu":2,"memory_mb":2048,"disk_size_gb":20}\' \\\n  http://HOST/api/v1/vms' },
    { title: _L('스냅샷 생성', 'Snapshot Create'), cmd: 'curl -X POST -H "Authorization: Bearer $TOKEN" \\\n  -d \'{"snap_name":"backup-1"}\' \\\n  http://HOST/api/v1/vms/web/snapshot/create' },
    { title: _L('컨테이너 실행', 'Container Exec'), cmd: 'curl -X POST -H "Authorization: Bearer $TOKEN" \\\n  -d \'{"command":"hostname -I"}\' \\\n  http://HOST/api/v1/containers/app-ctr/exec' },
    { title: _L('클라우드 임포트 (니어라이브)', 'Cloud Import (Near-Live)'), cmd: 'curl -X POST -H "Authorization: Bearer $TOKEN" \\\n  -d \'{"name":"web","ami_id":"ami-0abc","mode":"near-live"}\' \\\n  http://HOST/api/v1/vms/web/import-ec2' },
    { title: _L('WebSocket 이벤트', 'WebSocket Events'), cmd: 'wscat -c "ws://HOST/api/v1/ws/events?token=$TOKEN"' },
  ];
  h += '<div class="sg grid-2">';
  examples.forEach(function(ex) {
    h += H.card(ex.title, '<pre style="background:var(--bg);padding:10px;border-radius:4px;font-size:11px;color:var(--green);overflow-x:auto;white-space:pre-wrap">' + esc(ex.cmd) + '</pre>');
  });
  h += '</div>';
  b.innerHTML = h;
}
window.renderRestGuide = renderRestGuide;


function renderServiceGuide(b) {
  var h = H.sectionLg(_L('PureCVisor 서비스 가이드', 'PureCVisor Service Guide'));
  h += '<div class="mb-16"><input id="guide-search" class="sb-search" placeholder="' + t('search') + '" oninput="filterGuide()" style="max-width:600px;font-size:15px;padding:10px 14px;border-radius:8px"></div>';
  h += '<div id="guide-content">';

  var services = [
    { title: _L('빠른 시작', 'Quick Start'), icon: '&#128640;', sections: [
      { sub: _L('5분 설정', '5-Minute Setup'), content:
        '<ol style="font-size:13px;line-height:2;padding-left:18px">'
        + '<li>' + _L('로그인', 'Login') + ': <code>http://NODE_IP/ui/</code> (admin / configured password)</li>'
        + '<li>' + _L('VM 생성: Ctrl+N → 이름, vCPU, 메모리, 디스크 → 생성', 'Create VM: Ctrl+N → Name, vCPU, Memory, Disk → Create') + '</li>'
        + '<li>' + _L('VM 시작: VM 선택 → 시작 버튼 또는 우클릭 → 시작', 'Start VM: Select VM → Start button or right-click → Start') + '</li>'
        + '<li>' + _L('VNC 콘솔: VM 선택 → 콘솔 탭 → noVNC', 'VNC Console: Select VM → Console tab → noVNC') + '</li>'
        + '<li>' + _L('모니터링: INFRA 사이드바 → 모니터링 Overview', 'Monitor: INFRA sidebar → Monitoring Overview') + '</li></ol>' },
      { sub: _L('CLI 빠른 시작', 'CLI Quick Start'), content:
        '<pre style="background:var(--bg);padding:12px;border-radius:6px;font-size:11px;color:var(--green);overflow-x:auto">'
        + '# ' + _L('로그인', 'Login') + '\n'
        + 'TOKEN=$(curl -s -X POST http://localhost/api/v1/auth/token \\\n'
        + '  -d \'{"username":"admin","password":"configured-admin-password"}\' | jq -r .access_token)\n\n'
        + '# VM\n'
        + 'pcvctl vm list\n'
        + 'pcvctl vm create web --vcpu 2 --memory_mb 2048 --disk_size_gb 20\n'
        + 'pcvctl vm start web\n'
        + 'pcvctl vm stop web\n\n'
        + '# ' + _L('컨테이너', 'Container') + '\n'
        + 'pcvctl container list\n'
        + 'pcvctl container exec app-ctr "hostname -I"\n\n'
        + '# ' + _L('모니터링', 'Monitoring') + '\n'
        + 'pcvctl monitor fleet\n'
        + 'pcvctl alert list</pre>' },
    ]},
    { title: _L('아키텍처', 'Architecture'), icon: '&#127959;', sections: [
      { sub: _L('시스템 개요', 'System Overview'), content:
        '<pre style="background:var(--bg);padding:12px;border-radius:6px;font-size:11px;color:var(--accent);overflow-x:auto">'
        + _L('클라이언트', 'Client') + ' (pcvctl / pcvtui / Web UI / REST API)\n'
        + '         |\n'
        + '   UDS ' + _L('서버', 'Server') + ' (JSON-RPC 2.0) | REST ' + _L('서버', 'Server') + ' (HTTP+JWT)\n'
        + '         |\n'
        + '   ' + _L('디스패처', 'Dispatcher') + ' (' + (typeof PCV !== 'undefined' ? PCV.config.RPC_COUNT : 264) + '+ RPC ' + _L('메서드', 'methods') + ')\n'
        + '     method policy / RBAC / VM owner-scope\n'
        + '         |\n'
        + '   ' + _L('핸들러 계층', 'Handler Layer') + ' (dispatcher/*.c)\n'
        + '         |\n'
        + '   ' + _L('코어 모듈', 'Core Modules') + ' (vm_manager, network, zfs, lxc)\n'
        + '         |\n'
        + '   ' + _L('시스템', 'System') + ' (libvirt, nftables, dnsmasq, ZFS, LXC)</pre>' },
      { sub: _L('기술 스택', 'Tech Stack'), content:
        '<table style="font-size:12px;width:100%"><tbody>'
        + '<tr><td class="color-muted">' + _L('언어', 'Language') + '</td><td>C23 (gnu23)</td></tr>'
        + '<tr><td class="color-muted">' + _L('이벤트 루프', 'Event Loop') + '</td><td>GMainLoop (GLib)</td></tr>'
        + '<tr><td class="color-muted">' + _L('하이퍼바이저', 'Hypervisor') + '</td><td>KVM/QEMU via libvirt</td></tr>'
        + '<tr><td class="color-muted">' + _L('컨테이너', 'Container') + '</td><td>LXC (liblxc)</td></tr>'
        + '<tr><td class="color-muted">' + _L('스토리지', 'Storage') + '</td><td>ZFS (zvol + snapshots)</td></tr>'
        + '<tr><td class="color-muted">' + _L('네트워크', 'Network') + '</td><td>nftables + OVS + OVN</td></tr>'
        + '<tr><td class="color-muted">REST</td><td>libsoup3 (HTTP/HTTPS)</td></tr>'
        + '<tr><td class="color-muted">' + _L('비동기 I/O', 'Async I/O') + '</td><td>io_uring</td></tr>'
        + '<tr><td class="color-muted">' + _L('인증', 'Auth') + '</td><td>JWT HS256 + RBAC + VM owner-scope</td></tr>'
        + '<tr><td class="color-muted">' + _L('모니터링', 'Monitoring') + '</td><td>' + _L('자체 node_exporter (' + (typeof PCV !== 'undefined' ? PCV.config.METRICS_COUNT : 170) + ' 메트릭)', 'Self node_exporter (' + (typeof PCV !== 'undefined' ? PCV.config.METRICS_COUNT : 170) + ' metrics)') + '</td></tr>'
        + '<tr><td class="color-muted">Web UI</td><td>Vanilla JS (Single Edge modules)</td></tr>'
        + '</tbody></table>' },
    ]},
    { title: _L('가상 머신', 'Virtual Machines'), icon: '&#128187;', sections: [
      { sub: _L('라이프사이클', 'Lifecycle'), content:
        H.row(_L('생성', 'Create'), _L('virt-install + XML 폴백, cloud-init 지원', 'virt-install + XML fallback, cloud-init support'))
        + H.row(_L('시작/중지', 'Start/Stop'), _L('virDomainCreate / virDomainShutdown (graceful 30초 → 강제)', 'virDomainCreate / virDomainShutdown (graceful 30s → force)'))
        + H.row(_L('일시정지/재개', 'Pause/Resume'), 'virDomainSuspend / virDomainResume')
        + H.row(_L('삭제', 'Delete'), _L('virDomainUndefine + ZFS zvol 삭제 (비동기)', 'virDomainUndefine + ZFS zvol destroy (async)'))
        + H.row(_L('복제', 'Clone'), _L('ZFS clone + 새 도메인 정의', 'ZFS clone + new domain define'))
        + H.row(_L('가져오기/내보내기', 'Import/Export'), _L('qcow2 기반 VM 이미지 가져오기와 내보내기', 'qcow2-based VM image import and export')) },
      { sub: _L('스냅샷', 'Snapshots'), content:
        H.row(_L('생성', 'Create'), _L('ZFS 스냅샷 (크래시 일관성, 실행/중지 상태)', 'ZFS snapshot (crash-consistent, live or stopped)'))
        + H.row(_L('롤백', 'Rollback'), _L('VM 중지 → zfs rollback -r → 재시작 (fire-and-forget)', 'VM stop → zfs rollback -r → restart (fire-and-forget)'))
        + H.row(_L('일괄 삭제', 'Bulk Delete'), _L('vm.snapshot.delete_all — prefix 필터 + keep_recent', 'vm.snapshot.delete_all — prefix filter + keep_recent'))
        + H.row('UI', _L('생성 모달 (검증+미리보기) + 롤백 (이름 타이핑 확인) + 일괄 삭제 (미리보기)', 'Create modal (validation+preview) + Rollback (name typing confirm) + Bulk delete (preview)')) },
      { sub: _L('핫플러그', 'Hotplug'), content:
        H.row('NIC', _L('device.nic.attach/detach — 브릿지 + 모델 (virtio)', 'device.nic.attach/detach — bridge + model (virtio)'))
        + H.row('ISO', _L('vm.mount_iso / vm.eject — 핫 마운트/꺼내기', 'vm.mount_iso / vm.eject — hot mount/eject'))
        + H.row('vCPU', _L('vm.set_vcpu — 라이브 조정', 'vm.set_vcpu — live adjust'))
        + H.row(_L('메모리', 'Memory'), _L('vm.set_memory — 라이브 조정 (balloon)', 'vm.set_memory — live adjust (balloon)'))
        + H.row(_L('디스크', 'Disk'), _L('vm.resize_disk — qemu-img resize + virDomainBlockResize', 'vm.resize_disk — qemu-img resize + virDomainBlockResize')) },
    ]},
    { title: _L('클라우드 마이그레이션', 'Cloud Migration'), icon: '&#9729;', sections: [
      { sub: _L('AWS EC2 임포트', 'AWS EC2 Import'), content:
        '<div style="font-size:13px;line-height:1.8">'
        + '<b>' + _L('표준 임포트 (6단계):', 'Standard Import (6 stages):') + '</b>'
        + '<ol style="padding-left:18px;margin:4px 0"><li>' + _L('AWS 자격증명 검증', 'AWS credential validation') + '</li><li>' + _L('AMI → S3 내보내기', 'AMI → S3 export') + '</li><li>' + _L('진행률 폴링', 'Progress polling') + '</li><li>' + _L('S3 다운로드', 'S3 download') + '</li><li>' + _L('RAW → qcow2 변환', 'RAW → qcow2 conversion') + '</li><li>' + _L('VM 정의 + 시작', 'VM define + start') + '</li></ol>'
        + '<b>' + _L('니어라이브 임포트 (2단계):', 'Near-Live Import (2 phases):') + '</b>'
        + '<ol style="padding-left:18px;margin:4px 0"><li><span class="color-green">' + _L('Phase 1', 'Phase 1') + '</span>: ' + _L('사전동기화 (기본 이미지 다운로드, 다운타임 0)', 'Pre-sync (base image download, no downtime)') + '</li>'
        + '<li><span class="color-yellow">' + _L('Phase 2', 'Phase 2') + '</span>: ' + _L('최종전환 (EC2 중지 → 델타 스냅샷 → 리베이스 → VM 시작, ~2-5분)', 'Finalize (EC2 stop → delta snapshot → rebase → VM start, ~2-5min)') + '</li></ol></div>' },
    ]},
    { title: _L('보안', 'Security'), icon: '&#128274;', sections: [
      { sub: _L('보안 강화', 'Hardening'), content:
        H.row('XSS', _L('escapeHtml() — 모든 사용자 입력', 'escapeHtml() — all user input'))
        + H.row('CORS', _L('화이트리스트 모드', 'Whitelist mode'))
        + H.row('RBAC', _L('디스패처 메서드 정책 + operator VM owner metadata 검사', 'Dispatcher method policy + operator VM owner metadata check'))
        + H.row(_L('속도 제한', 'Rate Limit'), _L('600 IP / 1200 유저 / 60 인증', '600 IP / 1200 user / 60 auth'))
        + H.row('SQL', _L('Prepared statements', 'Prepared statements'))
        + H.row(_L('경로 순회', 'Path Traversal'), _L('realpath() 검증', 'realpath() validation'))
        + H.row(_L('명령 주입', 'CMD Injection'), _L('pcv_spawn_sync() argv 배열 (쉘 없음)', 'pcv_spawn_sync() argv array (no shell)'))
        + H.row(_L('패스워드', 'Password'), 'PBKDF2 (HMAC-SHA256) + ' + _L('자동 마이그레이션', 'auto-migration'))
        + H.row('Seccomp', _L('시스콜 필터링', 'Syscall filtering'))
        + H.row('WebSocket', _L('JWT 인증 + 300초 유휴 타임아웃', 'JWT auth + 300s idle timeout'))
        + H.row(_L('보안 그룹', 'Security Groups'), _L('SQLite 영속화 + default-deny + 포트 범위', 'SQLite persistence + default-deny + port ranges'))
        + H.row(_L('시크릿', 'Secrets'), _L('PCV_SECRET_* 환경변수 우선 로드', 'PCV_SECRET_* env var priority')) },
    ]},
    { title: _L('설정', 'Configuration'), icon: '&#9881;', sections: [
      { sub: 'daemon.conf', content:
        '<pre style="background:var(--bg);padding:10px;border-radius:6px;font-size:11px;color:var(--green);overflow-x:auto">'
        + '[server]\nport = 80\ndrain_timeout = 30\n\n[tls]\nenabled = false\n\n[storage]\nzvol_pool = pcvpool/vms\nimage_dir = /var/lib/libvirt/images\niso_dirs = /pcvpool/iso,/var/lib/libvirt/images\n\n[alert]\nenabled = true\ncpu_warn = 80\ncpu_crit = 95\ndata_pool_warn = 80\ndata_pool_crit = 90\nwebhook_url = https://hooks.slack.com/...\nwebhook_format = slack\n\n[cpu]\nallow_overcommit = false</pre>' },
      { sub: _L('서비스 관리', 'Service Management'), content:
        '<pre style="background:var(--bg);padding:10px;border-radius:6px;font-size:11px;color:var(--green);overflow-x:auto">'
        + 'sudo systemctl start purecvisorsd  # Single Edge\nsudo systemctl status purecvisorsd\njournalctl -u purecvisorsd -f\n\n# ' + _L('수동 RPC 테스트', 'Manual RPC test') + '\n'
        + 'echo \'{"jsonrpc":"2.0","method":"vm.list","params":{},"id":"1"}\' \\\n  | nc -U /var/run/purecvisor/daemon.sock | jq</pre>' },
    ]},
    { title: _L('트러블슈팅', 'Troubleshooting'), icon: '&#128295;', sections: [
      { sub: _L('자주 발생하는 문제', 'Common Issues'), content:
        '<table style="font-size:12px;width:100%"><thead><tr><th>' + _L('증상', 'Symptom') + '</th><th>' + _L('원인', 'Cause') + '</th><th>' + _L('해결', 'Fix') + '</th></tr></thead><tbody>'
        + '<tr><td>' + _L('VM 상태 "unknown"', 'VM state "unknown"') + '</td><td>' + _L('libvirt 연결 끊김', 'libvirt connection lost') + '</td><td><code>systemctl restart purecvisorsd</code></td></tr>'
        + '<tr><td>/health ' + _L('느림 (30초)', 'slow (30s)') + '</td><td>' + _L('libvirt 프로브 타임아웃', 'libvirt probe timeout') + '</td><td><code>systemctl status libvirtd</code></td></tr>'
        + '<tr><td>' + _L('스냅샷 500 에러', 'Snapshot 500 error') + '</td><td>' + _L('스냅샷 과다 (>1000개)', 'Too many snapshots (>1000)') + '</td><td><code>pcvctl vm snapshot delete-all --keep 10</code></td></tr>'
        + '<tr><td>' + _L('컨테이너 IP 대기중', 'Container IP pending') + '</td><td>' + _L('DHCP 지연', 'DHCP delay') + '</td><td>' + _L('10-15초 대기 또는 브릿지 설정 확인', 'Wait 10-15s or check bridge config') + '</td></tr>'
        + '<tr><td>REST 403 Forbidden</td><td>' + _L('RBAC 역할 부족 또는 VM owner 불일치', 'RBAC role insufficient or VM owner mismatch') + '</td><td><code>pcvctl auth list</code></td></tr>'
        + '</tbody></table>' },
      { sub: _L('디버그 명령어', 'Debug Commands'), content:
        '<pre style="background:var(--bg);padding:10px;border-radius:6px;font-size:11px;color:var(--green);overflow-x:auto">'
        + '# ' + _L('데몬 상태', 'Daemon status') + '\n'
        + 'sudo systemctl status purecvisorsd\njournalctl -u purecvisorsd --since "5 min ago"\n\n'
        + '# libvirt\nsudo virsh list --all\njournalctl -u libvirtd -n 50\n\n'
        + '# ' + _L('네트워크', 'Network') + '\n'
        + 'ip link show type bridge && brctl show\nsudo nft list table inet purecvisor\nsudo ovs-vsctl show\n\n'
        + '# ZFS\nzpool status pcvpool\nzfs list -t snapshot -r pcvpool/vms</pre>' },
    ]},
  ];

  services.forEach(function(s) {
    var searchText = (s.title + ' ' + s.sections.map(function(x){return x.sub;}).join(' ')).toLowerCase().replace(/[^a-z0-9가-힣\s]/g, '');
    h += '<div class="hc mb-14" data-guide="' + searchText + '">';
    h += '<h4 style="font-size:16px;color:var(--accent);margin-bottom:12px;cursor:pointer" onclick="this.parentElement.classList.toggle(\'guide-collapsed\')">' + s.icon + ' ' + s.title + ' <span class="color-muted" style="font-size:11px">(' + s.sections.length + ' ' + _L('섹션', 'sections') + ')</span></h4>';
    s.sections.forEach(function(sec) {
      h += '<div style="margin-bottom:14px;padding-left:12px;border-left:2px solid var(--border)">';
      h += '<div style="font-weight:600;font-size:13px;margin-bottom:6px;color:var(--fg)">' + sec.sub + '</div>';
      h += '<div style="font-size:12px;color:var(--fg2)">' + sec.content + '</div>';
      h += '</div>';
    });
    h += '</div>';
  });
  h += '</div>';
  b.innerHTML = h;
}
window.renderServiceGuide = renderServiceGuide;

function filterGuide() { var q = document.getElementById('guide-search').value.toLowerCase(); document.querySelectorAll('#guide-content .hc[data-guide]').forEach(function(c) { c.style.display = !q || c.dataset.guide.includes(q) ? '' : 'none'; }); }
window.filterGuide = filterGuide;


function renderSwaggerApi(b) {
  var mc = function(m) { return m === 'GET' ? '#61affe' : m === 'POST' ? '#49cc90' : m === 'DELETE' ? '#f93e3e' : m === 'PUT' ? '#fca130' : '#00f0ff'; };
  var endpoints = [
    { tag: 'Health & Auth (4)', endpoints: [
      { m: 'GET', p: '/health', d: _L('심층 헬스체크 (6개 서브시스템)', 'Deep health probe (6 subsystems)'), auth: false },
      { m: 'GET', p: '/metrics', d: _L('Prometheus 메트릭 (' + (typeof PCV !== 'undefined' ? PCV.config.METRICS_COUNT : 170) + '개)', 'Prometheus metrics (' + (typeof PCV !== 'undefined' ? PCV.config.METRICS_COUNT : 170) + ')'), auth: false },
      { m: 'POST', p: '/auth/token', d: _L('JWT 로그인', 'JWT login'), auth: false, body: '{"username":"admin","password":"configured-admin-password"}' },
      { m: 'GET', p: '/auth/users', d: _L('사용자 목록 (RBAC)', 'User list (RBAC)') },
    ]},
    { tag: 'VMs (22)', endpoints: [
      { m: 'GET', p: '/vms', d: _L('VM 목록', 'VM list') },
      { m: 'POST', p: '/vms', d: _L('VM 생성', 'Create VM'), body: '{"name":"web","vcpu":2,"memory_mb":2048,"disk_size_gb":20}' },
      { m: 'DELETE', p: '/vms/{name}', d: _L('VM 삭제', 'Delete VM') },
      { m: 'POST', p: '/vms/{name}/start', d: _L('VM 시작', 'Start VM') },
      { m: 'POST', p: '/vms/{name}/stop', d: _L('VM 중지', 'Stop VM') },
      { m: 'POST', p: '/vms/{name}/suspend', d: _L('VM 일시정지', 'Pause VM') },
      { m: 'POST', p: '/vms/{name}/resume', d: _L('VM 재개', 'Resume VM') },
      { m: 'GET', p: '/vms/{name}/snapshot', d: _L('스냅샷 목록', 'Snapshot list') },
      { m: 'POST', p: '/vms/{name}/snapshot/create', d: _L('스냅샷 생성', 'Create snapshot'), body: '{"snap_name":"backup-1"}' },
      { m: 'POST', p: '/vms/{name}/snapshot/rollback', d: _L('스냅샷 롤백', 'Rollback snapshot'), body: '{"snap_name":"backup-1"}' },
      { m: 'DELETE', p: '/vms/{name}/snapshot/{snap}', d: _L('스냅샷 삭제', 'Delete snapshot') },
      { m: 'POST', p: '/vms/{name}/snapshot/delete_all', d: _L('일괄 삭제', 'Bulk delete'), body: '{"prefix":"pcv-repl-","keep_recent":5}' },
      { m: 'GET', p: '/vms/{name}/nics', d: _L('NIC 목록', 'NIC list') },
      { m: 'POST', p: '/vms/{name}/nics', d: _L('NIC 추가', 'NIC attach'), body: '{"bridge":"pcvbr0","model":"virtio"}' },
      { m: 'DELETE', p: '/vms/{name}/nics/{mac}', d: _L('NIC 제거', 'NIC detach') },
      { m: 'PUT', p: '/vms/{name}/vcpu', d: _L('vCPU 설정', 'Set vCPU'), body: '{"vcpu_count":4}' },
      { m: 'PUT', p: '/vms/{name}/memory', d: _L('메모리 설정', 'Set memory'), body: '{"memory_mb":4096}' },
      { m: 'POST', p: '/vms/{name}/clone', d: _L('VM 복제', 'Clone VM'), body: '{"new_name":"web-clone"}' },
      { m: 'POST', p: '/vms/{name}/iso', d: _L('ISO 마운트', 'Mount ISO'), body: '{"iso_path":"/iso/ubuntu.iso"}' },
      { m: 'DELETE', p: '/vms/{name}/iso', d: _L('ISO 꺼내기', 'Eject ISO') },
      { m: 'GET', p: '/vms/{name}/delete-status', d: _L('삭제 진행률', 'Delete progress') },
      { m: 'PUT', p: '/vms/{name}/bandwidth', d: _L('대역폭 QoS', 'Bandwidth QoS'), body: '{"rate":"100mbit"}' },
    ]},
    { tag: _L('컨테이너 (17)', 'Containers (17)'), endpoints: [
      { m: 'GET', p: '/containers', d: _L('컨테이너 목록', 'Container list') },
      { m: 'POST', p: '/containers', d: _L('컨테이너 생성', 'Create container'), body: '{"name":"app","dist":"ubuntu","release":"24.04"}' },
      { m: 'DELETE', p: '/containers/{name}', d: _L('컨테이너 삭제', 'Destroy container') },
      { m: 'POST', p: '/containers/{name}/start', d: _L('시작', 'Start') },
      { m: 'POST', p: '/containers/{name}/stop', d: _L('중지', 'Stop') },
      { m: 'POST', p: '/containers/{name}/exec', d: _L('명령 실행', 'Exec command'), body: '{"command":"hostname"}' },
      { m: 'GET', p: '/containers/{name}/snapshots', d: _L('스냅샷 목록', 'Snapshot list') },
      { m: 'POST', p: '/containers/{name}/snapshots', d: _L('스냅샷 생성', 'Create snapshot'), body: '{"snap_name":"snap-1"}' },
      { m: 'GET', p: '/containers/{name}/nics', d: _L('NIC 목록', 'NIC list') },
      { m: 'PUT', p: '/containers/{name}/limits', d: _L('리소스 제한', 'Set limits'), body: '{"cpu_limit":"2","memory_limit":"512M"}' },
      { m: 'POST', p: '/rpc', d: _L('컨테이너 클론 (container.clone)', 'Container clone (container.clone)'), body: '{"jsonrpc":"2.0","method":"container.clone","params":{"name":"app","new_name":"app-clone"},"id":"1"}' },
      { m: 'POST', p: '/rpc', d: _L('컨테이너 체크포인트 (container.checkpoint)', 'Container checkpoint (container.checkpoint)'), body: '{"jsonrpc":"2.0","method":"container.checkpoint","params":{"name":"app"},"id":"1"}' },
      { m: 'POST', p: '/rpc', d: _L('컨테이너 복원 (container.restore)', 'Container restore (container.restore)'), body: '{"jsonrpc":"2.0","method":"container.restore","params":{"name":"app"},"id":"1"}' },
    ]},
    { tag: _L('네트워크 (5)', 'Networks (5)'), endpoints: [
      { m: 'GET', p: '/networks', d: _L('네트워크 목록', 'Network list') },
      { m: 'POST', p: '/networks', d: _L('네트워크 생성', 'Create network'), body: '{"name":"br1","mode":"nat","cidr":"10.10.0.1/24","dhcp":true}' },
      { m: 'DELETE', p: '/networks/{br}', d: _L('네트워크 삭제', 'Delete network') },
    ]},
    { tag: _L('스토리지 (7)', 'Storage (7)'), endpoints: [
      { m: 'GET', p: '/storage/pools', d: _L('ZFS 풀 목록', 'ZFS pools') },
      { m: 'POST', p: '/storage/pools', d: _L('풀 생성', 'Create pool') },
      { m: 'GET', p: '/storage/zvols', d: _L('Zvol 목록', 'Zvol list') },
      { m: 'POST', p: '/storage/zvols', d: _L('Zvol 생성', 'Create zvol'), body: '{"name":"data","size":"20G"}' },
    ]},
    { tag: _L('클라우드 마이그레이션 (5)', 'Cloud Migration (5)'), endpoints: [
      { m: 'POST', p: '/vms/{name}/import-ec2', d: _L('EC2에서 임포트', 'Import from EC2'), body: '{"ami_id":"ami-0abc","mode":"near-live"}' },
      { m: 'POST', p: '/vms/{name}/export-ec2', d: _L('EC2로 내보내기', 'Export to EC2'), body: '{"s3_bucket":"my-bucket"}' },
      { m: 'GET', p: '/vms/{name}/import-status', d: _L('임포트 진행률', 'Import progress') },
      { m: 'GET', p: '/cloud/jobs', d: _L('마이그레이션 작업 목록', 'Migration job list') },
      { m: 'POST', p: '/cloud/cancel', d: _L('마이그레이션 취소', 'Cancel migration'), body: '{"name":"web"}' },
    ]},
    { tag: _L('모니터링 (11)', 'Monitoring (11)'), endpoints: [
      { m: 'GET', p: '/processes', d: _L('프로세스 목록', 'Process list') },
      { m: 'GET', p: '/alerts', d: _L('알림 이력', 'Alert history') },
      { m: 'GET', p: '/alerts/config', d: _L('알림 설정', 'Alert config') },
      { m: 'PUT', p: '/alerts/config', d: _L('알림 설정 변경', 'Update alert config'), body: '{"cpu_warn":80,"cpu_crit":95}' },
      { m: 'GET', p: '/alerts/sla/{vm}', d: _L('VM SLA 추적', 'VM SLA tracking') },
      { m: 'POST', p: '/rpc', d: _L('알림 확인 (alert.acknowledge)', 'Alert acknowledge (alert.acknowledge)'), body: '{"jsonrpc":"2.0","method":"alert.acknowledge","params":{"alert_id":"..."},"id":"1"}' },
      { m: 'POST', p: '/rpc', d: _L('자가치유 대기/승인/거절 (ai.healing.*)', 'Self-healing pending/approve/reject (ai.healing.*)'), body: '{"jsonrpc":"2.0","method":"ai.healing.pending","params":{},"id":"1"}' },
      { m: 'GET', p: '/audit/search', d: _L('감사 로그 검색', 'Audit log search') },
      { m: 'GET', p: '/iso', d: _L('ISO 파일 목록', 'ISO file list') },
    ]},
    { tag: 'DPDK & SR-IOV (12)', endpoints: [
      { m: 'GET', p: '/dpdk/status', d: _L('DPDK 상태', 'DPDK status') },
      { m: 'POST', p: '/dpdk/bind', d: _L('NIC 바인드', 'Bind NIC'), body: '{"pci_addr":"0000:03:00.0","driver":"vfio-pci"}' },
      { m: 'POST', p: '/dpdk/unbind', d: _L('NIC 언바인드', 'Unbind NIC') },
      { m: 'GET', p: '/sriov/status', d: _L('SR-IOV 상태', 'SR-IOV status') },
      { m: 'POST', p: '/sriov/enable', d: _L('VF 활성화', 'Enable VFs'), body: '{"pf":"enp3s0f0","num_vfs":4}' },
      { m: 'POST', p: '/sriov/attach', d: _L('VF→VM 연결', 'Attach VF to VM'), body: '{"vm_name":"web","pci_addr":"0000:03:10.0"}' },
    ]},
    { tag: _L('백업 & 보안 그룹 (5)', 'Backup & Security Groups (5)'), endpoints: [
      { m: 'POST', p: '/rpc', d: _L('백업 정책 목록 (backup.list)', 'Backup policy list (backup.list)'), body: '{"jsonrpc":"2.0","method":"backup.list","params":{},"id":"1"}' },
      { m: 'POST', p: '/rpc', d: _L('백업 정책 설정 (backup.set)', 'Backup policy set (backup.set)'), body: '{"jsonrpc":"2.0","method":"backup.set","params":{"vm_name":"web","interval_hours":24,"retention":7},"id":"1"}' },
      { m: 'POST', p: '/rpc', d: _L('백업 복원 (backup.restore)', 'Backup restore (backup.restore)'), body: '{"jsonrpc":"2.0","method":"backup.restore","params":{"vm_name":"web","snap_name":"auto-2026-04-01"},"id":"1"}' },
      { m: 'POST', p: '/rpc', d: _L('보안 그룹 규칙 삭제 (security_group.rule.remove)', 'Security group rule remove (security_group.rule.remove)'), body: '{"jsonrpc":"2.0","method":"security_group.rule.remove","params":{"group":"default","rule_id":"r-001"},"id":"1"}' },
    ]},
    { tag: 'GPU (3)', endpoints: [
      { m: 'GET', p: '/gpu/list', d: _L('GPU 장치 목록', 'GPU device list') },
      { m: 'GET', p: '/gpu/metrics', d: _L('GPU 메트릭', 'GPU metrics') },
    ]},
  ];
  var total = 0; endpoints.forEach(function(g) { total += g.endpoints.length; });
  var h = '<h3 style="font-family:var(--font-display);margin-bottom:8px">&#128214; PureCVisor REST API</h3>';
  h += '<div class="flex gap-10 mb-8">' + H.badge('OpenAPI 3.0', 'g') + H.badge(total + ' ' + _L('엔드포인트', 'Endpoints'), 'y') + H.badge('JWT + RBAC', 'r') + '</div>';
  h += '<div class="stat-label mb-12">' + _L('기본', 'Base') + ': <code>/api/v1</code> | ' + _L('인증', 'Auth') + ': <code>Bearer JWT</code></div>';
  h += '<div class="mb-12"><input id="sw-search" class="sb-search" placeholder="' + _L('엔드포인트 검색...', 'Search endpoints...') + '" oninput="filterSwagger()" style="max-width:500px;font-size:13px;padding:8px 12px;border-radius:6px"></div>';
  endpoints.forEach(function(g) {
    h += '<div class="mb-16 sw-group"><div style="font-family:var(--font-display);font-size:11px;font-weight:700;color:var(--accent);text-transform:uppercase;letter-spacing:.08em;border-bottom:1px solid rgba(0,240,255,.15);padding:6px 0;margin-bottom:4px">' + g.tag + '</div>';
    g.endpoints.forEach(function(e, i) { var id = 'sw-' + g.tag.replace(/\W/g, '') + i;
      h += '<div class="mb-2 sw-ep" data-sw="' + (e.m + ' ' + e.p + ' ' + e.d).toLowerCase() + '" style="border:1px solid var(--border);border-radius:4px;overflow:hidden"><div onclick="document.getElementById(\'' + id + '\').classList.toggle(\'hidden\')" style="display:flex;align-items:center;padding:8px 12px;cursor:pointer;gap:10px;background:var(--bg2)"><span style="background:' + mc(e.m) + ';color:#fff;font-size:10px;font-weight:700;padding:2px 8px;border-radius:3px;min-width:52px;text-align:center">' + e.m + '</span><span style="font-family:monospace;font-size:12px">' + e.p + '</span><span class="stat-label" style="margin-left:auto">' + e.d + '</span>' + (e.auth === false ? '' : '<span style="font-size:9px;color:var(--yellow)">&#128274;</span>') + '</div>';
      h += '<div id="' + id + '" class="hidden" style="padding:10px 12px;background:var(--bg3);font-size:11px">';
      if (e.body) h += '<div class="mb-6"><b>' + _L('요청 본문:', 'Request Body:') + '</b><pre style="background:var(--bg);padding:8px;border-radius:4px;color:var(--green);overflow-x:auto">' + e.body + '</pre></div>';
      h += '<button class="btn" style="font-size:10px;padding:3px 10px" onclick="swTry(\'' + e.m + '\',\'' + e.p + '\',' + (e.body ? '\'' + e.body.replace(/'/g, "\\'") + '\'' : 'null') + ')">&#9654; ' + _L('실행', 'Try it') + '</button></div></div>'; });
    h += '</div>';
  });
  b.innerHTML = h;
}
window.renderSwaggerApi = renderSwaggerApi;

function filterSwagger() {
  var q = (document.getElementById('sw-search')?.value || '').toLowerCase();
  document.querySelectorAll('.sw-ep').forEach(function(el) { el.style.display = !q || el.dataset.sw.includes(q) ? '' : 'none'; });
}
window.filterSwagger = filterSwagger;

async function swTry(m, p, body) {
  var url = API_BASE + p.replace(/\{[^}]+\}/g, 'test');
  try { var opts = { headers: { Authorization: 'Bearer ' + authToken } };
    if (m === 'POST' || m === 'PUT' || m === 'DELETE') { opts.method = m; opts.headers['Content-Type'] = 'application/json'; if (body) opts.body = body; }
    var r = await fetch(url, opts); var txt = await r.text(); var pretty = txt; try { pretty = JSON.stringify(JSON.parse(txt), null, 2); } catch (e) {  }
    showModal('<h2>' + _L('응답', 'Response') + ': ' + m + ' ' + p + '</h2>' + H.row(_L('상태', 'Status'), '<span style="color:' + (r.ok ? 'var(--green)' : 'var(--red)') + '">' + r.status + '</span>') + '<pre style="background:var(--bg);padding:12px;border-radius:6px;max-height:400px;overflow:auto;font-size:11px;color:var(--cyan);white-space:pre-wrap">' + pretty.replace(/</g, '&lt;') + '</pre><div style="text-align:right;margin-top:12px"><button class="btn" onclick="closeModal()">' + t('btn.close') + '</button></div>');
  } catch (e) { toast(_L('요청 실패', 'Request failed') + ': ' + e.message, false); }
}
window.swTry = swTry;


var kbdHelpOpen = false;
window.kbdHelpOpen = kbdHelpOpen;

function toggleKbdHelp() {
  if (kbdHelpOpen) { closeKbdHelp(); return; }
  kbdHelpOpen = true; window.kbdHelpOpen = kbdHelpOpen;
  var shortcuts = [
    ['Ctrl+K', _L('커맨드 팔레트', 'Command Palette')],
    ['Ctrl+N', _L('새 VM', 'New VM')],
    ['Ctrl+D', _L('VM 설정', 'VM Settings')],
    ['Ctrl+P', _L('환경설정', 'Preferences')],
    ['F11', _L('전체 화면', 'Fullscreen')],
    ['Escape', _L('대화상자 닫기', 'Close Dialog')],
    ['?', _L('이 도움말', 'This Help')],
    ['Ctrl+Shift+F', _L('전역 검색', 'Global Search')],
    ['Ctrl+B', _L('사이드바 전환', 'Toggle Sidebar')],
  ];
  var ov = document.createElement('div');
  ov.id = 'kbd-help-overlay'; ov.className = 'kbd-overlay';
  ov.onclick = function(e) { if (e.target === ov) closeKbdHelp(); };
  var h = '<div class="kbd-box"><div class="kbd-title">' + _L('키보드 단축키', 'Keyboard Shortcuts') + '</div><div class="kbd-grid">';
  shortcuts.forEach(function(s) { h += '<div class="kbd-row"><span class="kbd-key">' + s[0] + '</span><span class="kbd-desc">' + s[1] + '</span></div>'; });
  h += '</div><div class="kbd-close">' + _L('? 또는 Esc 키로 닫기', 'Press ? or Esc to close') + '</div></div>';
  ov.innerHTML = h; document.body.appendChild(ov);
}
window.toggleKbdHelp = toggleKbdHelp;

function closeKbdHelp() { kbdHelpOpen = false; window.kbdHelpOpen = kbdHelpOpen; var el = document.getElementById('kbd-help-overlay'); if (el) el.remove(); }
window.closeKbdHelp = closeKbdHelp;


PCV.help = {
  renderHelp: renderHelp,
  filterHelp: filterHelp,
  renderRestGuide: renderRestGuide,
  renderServiceGuide: renderServiceGuide,
  filterGuide: filterGuide,
  renderSwaggerApi: renderSwaggerApi,
  filterSwagger: filterSwagger,
  swTry: swTry,
  toggleKbdHelp: toggleKbdHelp,
  closeKbdHelp: closeKbdHelp
};
})(window.PCV);









window.PCV = window.PCV || {};
(function(PCV) {


var _pinnedPages = JSON.parse(localStorage.getItem('pcv-pinned') || '[]');

function togglePin(pageId) {
  var idx = _pinnedPages.indexOf(pageId);
  if (idx >= 0) _pinnedPages.splice(idx, 1);
  else _pinnedPages.push(pageId);
  localStorage.setItem('pcv-pinned', JSON.stringify(_pinnedPages));
  renderPinnedBar();
}

function renderPinnedBar() {
  var el = document.getElementById('pinned-bar');
  if (!el) return;
  if (_pinnedPages.length === 0) { el.innerHTML = ''; el.style.display = 'none'; return; }
  el.style.display = 'block';
  var h = '<div style="padding:4px 8px;font-size:10px;color:var(--fg2);border-bottom:1px solid var(--border)">' + _L('고정됨', 'Pinned') + '</div>';
  _pinnedPages.forEach(function(p) {
    h += '<div class="vi" style="padding:4px 8px;font-size:11px" onclick="navigateTo(\'' + p + '\')"><span class="color-yellow" style="cursor:pointer;margin-right:4px" onclick="event.stopPropagation();togglePin(\'' + p + '\')">&#128204;</span>' + p + '</div>';
  });
  el.innerHTML = h;
}
window.togglePin = togglePin;
window.renderPinnedBar = renderPinnedBar;


window._navGeneration = 0;
function navigateTo(n) {
  if (window.pcvClusterEnabled === false && window.PCV_CLUSTER_ONLY_NAV && window.PCV_CLUSTER_ONLY_NAV.includes(n)) {
    if (typeof toast === 'function') toast(_L('Single Edge 공개 리포에는 포함되지 않는 화면입니다', 'This screen is not included in Single Edge'), false);
    n = 'dashboard';
  }

  window._navGeneration = (window._navGeneration || 0) + 1;

  if (typeof _cloudCleanupTimer === 'function' && currentTab === 'cloud-migration' && n !== 'cloud-migration') {
    _cloudCleanupTimer();
  }

  if (typeof stopAdaptivePolling === 'function' && currentTab && currentTab.startsWith('mon-') && !(n && n.startsWith('mon-'))) {
    stopAdaptivePolling('mon-refresh');
  }

  if (typeof clearAllFormDirty === 'function') clearAllFormDirty();

  var vmTabs = ['summary', 'console', 'snapshots', 'performance', 'timeline'];
  if (vmTabs.includes(n)) localStorage.setItem('pcv-last-vm-tab', n);
  currentTab = n;
  document.querySelectorAll('#ct button').forEach(b => b.classList.remove('active'));
  const containerPages = ['containers', 'docker'];

  const infraPages = PCV.filterEditionItems([
    { id: 'networks' }, { id: 'storage' }, { id: 'host' }, { id: 'ovn' },
    { id: 'accounts' }, { id: 'security-groups' }, { id: 'gpu' }, { id: 'templates' },
    { id: 'config-mgmt' }, { id: 'apimgmt' }, { id: 'apihelp' }, { id: 'helppage' },
    { id: 'serviceguide' }, { id: 'restguide' }, { id: 'mon-hosts' }, { id: 'mon-vms' },
    { id: 'mon-alerts' }, { id: 'mon-security' }, { id: 'mon-audit' }, { id: 'overlay' },
    { id: 'iscsi' }, { id: 'dpdk' }, { id: 'sriov' }, { id: 'heatmap' }, { id: 'api-perf' },
    { id: 'activity-log' }, { id: 'topology' }, { id: 'backup' },
    { id: 'ops-triage' },
    { id: 'mon-overview' }, { id: 'mon-storage' },
    { id: 'cloud-migration' }, { id: 'selfhealing' }
  ]).map(function(item) { return item.id; });
  if (containerPages.includes(n)) switchSbTab('containers');
  else if (infraPages.includes(n)) switchSbTab('infra');
  else { switchSbTab('vms'); }
  renderContent();
}
window.navigateTo = navigateTo;

window.go = navigateTo;

function pcvRoleAllows(minRole) {
  var roleRank = { viewer: 0, operator: 1, admin: 2 };
  var current = String((window.currentUser && window.currentUser.role) || '').toLowerCase();
  var required = String(minRole || 'viewer').toLowerCase();
  return (roleRank[current] ?? -1) >= (roleRank[required] ?? 0);
}
window.pcvRoleAllows = pcvRoleAllows;


function switchSbTab(tab) {
  ['vms', 'containers', 'infra'].forEach(t => {
    const panel = document.getElementById('sb-' + t);
    if (panel) panel.classList.toggle('hidden', t !== tab);
  });
  document.querySelectorAll('#sb-tabs button').forEach(b => {
    b.classList.toggle('active', b.dataset.sb === tab);
  });
  const ct = document.getElementById('ct');
  if (ct) ct.style.display = (tab === 'vms') ? 'flex' : 'none';
  if (tab === 'containers') renderContainerList();
}
window.switchSbTab = switchSbTab;


function renderContent() {
  destroyAllCharts();
  var cb = document.getElementById('cb');
  if (cb) cb.classList.add('fade-out');
  const b = cb, v = vmList[selectedVmIndex];

  var gen = window._navGeneration || 0;
  try {
    var fn = (function() {
      var routes = {
      dashboard: () => renderDashboard(b),
      summary: () => renderSummary(b, v),
      console: () => renderConsole(b, v),
      snapshots: () => renderSnapshots(b, v),
      performance: () => renderPerformance(b, v),
      timeline: () => renderTimeline(b, v),
      networks: () => renderNetworks(b),
      storage: () => renderStorage(b),
      containers: () => renderContainers(b),
      host: () => renderHost(b),
      ovn: () => renderOvn(b),
      accounts: () => renderAccounts(b),
      apimgmt: () => renderApiManagement(b),
      'mon-overview': () => renderMonitoring(b, 'overview'),
      'ops-triage': () => renderOpsTriage(b),
      'mon-hosts': () => renderMonitoring(b, 'hosts'),
      'mon-vms': () => renderMonitoring(b, 'vms'),
      'mon-storage': () => renderMonitoring(b, 'storage'),
      'mon-alerts': () => renderAlerts(b),
      'mon-security': () => PCV.security.render(b),
      'mon-audit': () => renderAudit(b),
      'security-groups': () => renderSecGroups(b),
      'gpu': () => renderGpu(b),
      'templates': () => renderTemplates(b),
      'config-mgmt': () => renderConfigMgmt(b),
      'docker': () => renderDocker(b),
      'terraform': () => renderTerraform(b),
      'cloud-migration': () => renderCloudMigration(b),
      'overlay': () => renderOverlayNetworks(b),
      'iscsi': () => renderIscsi(b),
      'dpdk': () => renderDpdk(b),
      'sriov': () => renderSriov(b),
      'heatmap': () => renderHeatmap(b),
      'api-perf': () => renderApiPerf(b),
      'activity-log': () => renderActivityLog(b),
      'topology': () => renderTopology(b),
      'backup': () => renderBackup(b),
      apihelp: () => renderSwaggerApi(b),
      helppage: () => renderHelp(b),
      serviceguide: () => renderServiceGuide(b),
      restguide: () => renderRestGuide(b),

      'selfhealing': () => renderSelfHealing(b)
      };
      return routes;
    })()[currentTab];
    if (fn) {
      var result = fn();
      if (result && typeof result.catch === 'function') {
        result.catch(function(err) {
          if ((window._navGeneration || 0) !== gen) return;
          if (b) b.innerHTML = '<div style="padding:40px;text-align:center"><div style="font-size:48px;margin-bottom:12px">&#9888;</div><h3 style="color:var(--red)">' + _L('렌더링 오류', 'Rendering Error') + '</h3><p class="color-muted" style="margin:12px 0">' + esc(err.message || '') + '</p><button class="btn" onclick="navigateTo(\'dashboard\')" style="margin-top:12px">' + _L('대시보드로', 'Go to Dashboard') + '</button></div>';
          if (_DEBUG) console.error('renderContent async error:', err);
        });
      }
    }
    else if (_DEBUG) console.warn('Unknown tab:', currentTab);
  } catch (renderErr) {
    if ((window._navGeneration || 0) !== gen) {  }
    else {
      b.innerHTML = '<div style="padding:40px;text-align:center"><div style="font-size:48px;margin-bottom:12px">&#9888;</div><h3 style="color:var(--red)">' + _L('렌더링 오류', 'Rendering Error') + '</h3><p class="color-muted" style="margin:12px 0">' + esc(renderErr.message || '') + '</p><pre style="background:var(--bg);padding:12px;border-radius:6px;font-size:11px;color:var(--red);text-align:left;max-height:200px;overflow:auto">' + esc(renderErr.stack || '') + '</pre><button class="btn" onclick="navigateTo(\'dashboard\')" style="margin-top:12px">' + _L('대시보드로', 'Go to Dashboard') + '</button></div>';
      if (_DEBUG) console.error('renderContent error:', renderErr);
    }
  }
  setTimeout(function() { if (cb) cb.classList.remove('fade-out'); }, 50);
  if (typeof updateStatusBar === 'function') updateStatusBar();
  if (typeof updateBreadcrumbs === 'function') updateBreadcrumbs();
}
window.renderContent = renderContent;


function updateStatusBar() {
  var el = document.getElementById('sb-ctx');
  if (!el) return;
  var parts = [];
  if (vmList && vmList.length) {
    var running = vmList.filter(function(v) { return v.state === 'running'; }).length;
    parts.push('VM: ' + running + '/' + vmList.length);
  }
  parts.push(currentTab || 'dashboard');
  if (wsConnection && wsConnection.readyState === 1) parts.push('WS: Live');
  var elapsed = Math.round((Date.now() - (lastLoadTime || Date.now())) / 1000);
  parts.push('Sync: ' + elapsed + 's ago');
  if (window._perfMetrics) parts.push('API: ' + (_perfMetrics.avgApiTime || 0) + 'ms avg');

  var wsStat = '&#9679;';
  if (typeof wsConnection !== 'undefined' && wsConnection && wsConnection.readyState === 1) {
    wsStat = '<span style="color:var(--green)">&#9679;</span>';
  } else if (typeof wsConnection !== 'undefined' && wsConnection && wsConnection.readyState === 0) {
    wsStat = '<span style="color:var(--yellow)">&#9679;</span>';
  } else {
    wsStat = '<span style="color:var(--red)">&#9679;</span>';
  }
  parts.unshift(wsStat);
  if (window._perfMetrics && _perfMetrics.avgApiTime > 0) {
    var latColor = _perfMetrics.avgApiTime < 100 ? 'var(--green)' : _perfMetrics.avgApiTime < 500 ? 'var(--yellow)' : 'var(--red)';
    parts.push('<span style="color:' + latColor + '">' + _perfMetrics.avgApiTime + 'ms</span>');
  }
  el.innerHTML = parts.join(' | ');
  if (typeof updateFavicon === 'function') updateFavicon();
}
window.updateStatusBar = updateStatusBar;
setInterval(updateStatusBar, 2000);


function updateFavicon() {
  var running = 0;
  if (vmList) running = vmList.filter(function(v) { return v.state === 'running'; }).length;


  document.title = 'PureCVisor' + (running > 0 ? ' (' + running + ' VMs)' : '');


  var canvas = document.createElement('canvas');
  canvas.width = 32; canvas.height = 32;
  var ctx = canvas.getContext('2d');

  var color = running > 0 ? '#00ff88' : (vmList && vmList.length > 0 ? '#ffee00' : '#ff2266');
  ctx.beginPath(); ctx.arc(16, 16, 14, 0, Math.PI * 2);
  ctx.fillStyle = color; ctx.fill();

  ctx.fillStyle = '#000'; ctx.font = 'bold 16px monospace'; ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
  ctx.fillText(running > 0 ? running.toString() : '!', 16, 17);

  var link = document.getElementById('dynamic-favicon');
  if (!link) {
    link = document.createElement('link');
    link.id = 'dynamic-favicon';
    link.rel = 'icon';
    link.type = 'image/png';
    document.head.appendChild(link);
  }
  link.href = canvas.toDataURL('image/png');
}
window.updateFavicon = updateFavicon;


function toggleSB() {
  const sb = document.getElementById('sidebar-panel') || document.getElementById('sidebar');
  if (sb) sb.classList.toggle('collapsed');
}
window.toggleSB = toggleSB;


function toggleFS() {
  document.fullscreenElement ? document.exitFullscreen() : document.documentElement.requestFullscreen();
}
window.toggleFS = toggleFS;


var infraSortAsc = true;
window.infraSortAsc = infraSortAsc;

function toggleInfraSort() {
  infraSortAsc = !infraSortAsc;
  window.infraSortAsc = infraSortAsc;
  const container = document.getElementById('nav-infra');
  if (!container) return;
  const items = Array.from(container.querySelectorAll('.vi'));
  items.sort((a, b) => {
    const ta = (a.querySelector('.nm')?.textContent || '').toLowerCase();
    const tb = (b.querySelector('.nm')?.textContent || '').toLowerCase();
    return infraSortAsc ? ta.localeCompare(tb) : tb.localeCompare(ta);
  });
  items.forEach(el => container.appendChild(el));
  const btn = document.getElementById('infra-sort-btn');
  if (btn) btn.textContent = infraSortAsc ? 'A-Z' : 'Z-A';
}
window.toggleInfraSort = toggleInfraSort;


var bottomPanelTab = 'terminal';
var bottomPanelExpanded = false;

function switchPanelTab(tab) {
  bottomPanelTab = tab;
  document.querySelectorAll('.panel-tab').forEach(el => el.classList.toggle('active', el.dataset.panel === tab));
  ['terminal', 'events', 'alerts', 'output'].forEach(t => {
    const el = document.getElementById('panel-' + t);
    if (el) el.classList.toggle('hidden', t !== tab);
  });
}
window.switchPanelTab = switchPanelTab;

function toggleBottomPanel() {
  const panel = document.getElementById('bottom-panel');
  if (panel) panel.classList.toggle('collapsed');
}
window.toggleBottomPanel = toggleBottomPanel;

function togglePanelSize() {
  const panel = document.getElementById('bottom-panel');
  if (!panel) return;
  bottomPanelExpanded = !bottomPanelExpanded;
  panel.style.height = bottomPanelExpanded ? '50vh' : '200px';
}
window.togglePanelSize = togglePanelSize;


var CMD_ACTIONS = PCV.filterEditionItems([
  { icon: '+', label: _L('새 VM', 'New VM'), hint: 'Ctrl+N', role: 'operator', action: () => showCreate() },
  { icon: '#', label: _L('운영 개요', 'Operations Overview'), action: () => navigateTo('mon-overview') },
  { icon: '!', label: _L('이벤트 센터', 'Event Center'), action: () => navigateTo('ops-triage') },
  { icon: '>', label: _L('VM 자산', 'VM Inventory'), action: () => switchSbTab('vms') },
  { icon: '~', label: _L('네트워크', 'Networks'), action: () => navigateTo('networks') },
  { icon: '=', label: _L('스토리지', 'Storage'), action: () => navigateTo('storage') },
  { icon: '@', label: _L('컨테이너', 'Containers'), action: () => navigateTo('containers') },
  { icon: '!', label: _L('알림', 'Alerts'), action: () => navigateTo('mon-alerts') },
  { icon: 'S', label: _L('보안 이벤트', 'Security Events'), action: () => navigateTo('mon-security') },
  { icon: '&', label: _L('계정과 권한', 'Accounts'), role: 'admin', action: () => navigateTo('accounts') },
  { icon: '%', label: _L('API 관리', 'API Management'), role: 'admin', action: () => navigateTo('apimgmt') },
  { icon: '?', label: 'Swagger API', action: () => navigateTo('apihelp') },
  { icon: 'i', label: _L('서비스 가이드', 'Service Guide'), action: () => navigateTo('serviceguide') },
  { icon: 'h', label: _L('도움말', 'Help'), action: () => navigateTo('helppage') },
  { icon: 'A', label: _L('AI Agent 설정', 'AI Agent Config'), action: () => showAgentConfig() },
  { icon: 'P', label: _L('환경설정', 'Preferences'), action: () => showPrefs() },
  { icon: 'T', label: _L('테마 전환', 'Toggle Theme'), action: () => toggleTheme() },
  { icon: '\u{1F3A8}', label: _L('테마 편집기', 'Theme Editor'), action: () => openThemeEditor() },
  { icon: 'L', label: _L('언어 전환', 'Toggle Language'), action: () => { I18N.toggle(); location.reload(); } },
  { icon: 'F', label: _L('전체 화면', 'Fullscreen'), hint: 'F11', action: () => toggleFS() },
]);
window.CMD_ACTIONS = CMD_ACTIONS;


function fuzzyMatch(text, query) {
  if (!query) return true;
  var ti = 0, qi = 0;
  var tLow = text.toLowerCase(), qLow = query.toLowerCase();
  while (ti < tLow.length && qi < qLow.length) {
    if (tLow[ti] === qLow[qi]) qi++;
    ti++;
  }
  return qi === qLow.length;
}
window.fuzzyMatch = fuzzyMatch;

var cmdPaletteOpen = false;
window.cmdPaletteOpen = cmdPaletteOpen;
var cmdSelectedIndex = 0;
window.cmdSelectedIndex = cmdSelectedIndex;

function openCmdPalette() {
  cmdPaletteOpen = true; cmdSelectedIndex = 0;
  window.cmdPaletteOpen = cmdPaletteOpen;
  window.cmdSelectedIndex = cmdSelectedIndex;
  const ov = document.createElement('div'); ov.id = 'cmd-palette-overlay';
  ov.style.cssText = 'position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,.5);display:flex;justify-content:center;padding-top:15vh;z-index:300';
  ov.addEventListener('click', e => { if (e.target === ov) closeCmdPalette(); });
  const box = document.createElement('div'); box.id = 'cmd-palette';
  box.style.cssText = 'background:var(--bg-panel);backdrop-filter:blur(20px);border:1px solid var(--accent);border-radius:12px;width:500px;max-width:90vw;max-height:60vh;display:flex;flex-direction:column;box-shadow:0 0 40px var(--neon-glow);overflow:hidden';
  box.innerHTML = '<input id="cmd-input" placeholder="' + _L('명령이나 화면 이름을 입력하세요', 'Type a command or page name') + '" style="padding:14px 16px;background:transparent;border:none;border-bottom:1px solid var(--border);color:var(--fg);font-size:15px;outline:none;font-family:var(--font-mono)" autocomplete="off"><div id="cmd-list" style="overflow-y:auto;flex:1;padding:4px 0"></div>';
  ov.appendChild(box); document.body.appendChild(ov);
  const inp = document.getElementById('cmd-input'); inp.focus();
  renderCmdPalette('');
  inp.addEventListener('input', () => { cmdSelectedIndex = 0; renderCmdPalette(inp.value); });
  inp.addEventListener('keydown', e => {
    const items = document.querySelectorAll('.cmd-item');
    if (e.key === 'ArrowDown') { e.preventDefault(); cmdSelectedIndex = Math.min(cmdSelectedIndex + 1, items.length - 1); renderCmdPalette(inp.value); }
    else if (e.key === 'ArrowUp') { e.preventDefault(); cmdSelectedIndex = Math.max(cmdSelectedIndex - 1, 0); renderCmdPalette(inp.value); }
    else if (e.key === 'Enter') { e.preventDefault(); if (items[cmdSelectedIndex]) items[cmdSelectedIndex].click(); }
  });
}
window.openCmdPalette = openCmdPalette;

function closeCmdPalette() {
  cmdPaletteOpen = false;
  window.cmdPaletteOpen = cmdPaletteOpen;
  const el = document.getElementById('cmd-palette-overlay'); if (el) el.remove();
}
window.closeCmdPalette = closeCmdPalette;

function renderCmdPalette(filter) {
  const list = document.getElementById('cmd-list'); if (!list) return;
  const q = (filter || '').toLowerCase();
  const filtered = CMD_ACTIONS.filter(a => {
    if (a.cluster && !window.pcvClusterEnabled) return false;
    if (a.role && !pcvRoleAllows(a.role)) return false;
    return !q || fuzzyMatch(a.label, q);
  });
  list.innerHTML = filtered.map((a, i) => '<div class="cmd-item" style="padding:10px 16px;cursor:pointer;display:flex;align-items:center;gap:12px;font-size:13px;background:' + (i === cmdSelectedIndex ? 'var(--bg3)' : 'transparent') + ';border-left:3px solid ' + (i === cmdSelectedIndex ? 'var(--accent)' : 'transparent') + '" onclick="CMD_ACTIONS.find(x=>x.label===\'' + a.label.replace(/'/g, "\\'") + '\').action();closeCmdPalette()"><span style="width:22px;text-align:center;font-size:14px">' + a.icon + '</span><span style="flex:1">' + a.label + '</span>' + (a.hint ? '<span class="stat-label">' + a.hint + '</span>' : '') + '</div>').join('');
}
window.renderCmdPalette = renderCmdPalette;


function openInPopup(pageId) {
  var page = pageId || currentTab;
  var w = window.open('', 'pcv-' + page, 'width=1200,height=800,menubar=no,toolbar=no');
  if (!w) { toast(_L('팝업 차단됨', 'Popup blocked'), false); return; }
  var theme = document.documentElement.getAttribute('data-theme') || '';
  w.document.write('<!DOCTYPE html><html lang="ko"' + (theme ? ' data-theme="' + theme + '"' : '') + '>'
    + '<head><meta charset="UTF-8"><title>PureCVisor \u2014 ' + page + '</title>'
    + '<link rel="stylesheet" href="/ui/style.css">'
    + '<style>body{margin:0;padding:16px;background:var(--bg);color:var(--fg);font-family:var(--font-mono)}</style>'
    + '</head><body><div id="popup-content"><span class="spinner"></span> Loading...</div>'
    + '<script src="/ui/i18n.js"><\/script>'
    + '<script src="/ui/modules/api.js"><\/script>'
    + '<script src="/ui/modules/ui.js"><\/script>'
    + '<script>authToken="' + (authToken || '') + '";<\/script>'
    + '</body></html>');
  w.document.close();

  setTimeout(function() {
    var cb = w.document.getElementById('popup-content');
    if (!cb) return;
    var content = document.getElementById('cb');
    if (content) cb.innerHTML = content.innerHTML;
  }, 500);
}
window.openInPopup = openInPopup;


function toggleMobileSB() {
  const sb = document.getElementById('sidebar-panel') || document.getElementById('sidebar'), ov = document.getElementById('mobile-overlay');
  if (sb.classList.contains('mobile-open')) { closeMobileSB(); }
  else { sb.classList.add('mobile-open'); sb.classList.remove('collapsed'); ov.style.display = 'block'; }
}
window.toggleMobileSB = toggleMobileSB;

function closeMobileSB() {
  const sb = document.getElementById('sidebar-panel') || document.getElementById('sidebar'), ov = document.getElementById('mobile-overlay');
  sb.classList.remove('mobile-open'); ov.style.display = 'none';
}
window.closeMobileSB = closeMobileSB;


var currentActivity = 'vms';
window.currentActivity = currentActivity;

function switchActivity(panel) {
  if (currentActivity === panel) {
    document.getElementById('sidebar-panel')?.classList.toggle('collapsed');
    return;
  }
  currentActivity = panel;
  window.currentActivity = currentActivity;
  document.getElementById('sidebar-panel')?.classList.remove('collapsed');
  document.querySelectorAll('.activity-icon[data-panel]').forEach(el => {
    el.classList.toggle('active', el.dataset.panel === panel);
  });
  ['vms', 'containers', 'infra'].forEach(t => {
    const el = document.getElementById('sb-' + t);
    if (el) el.classList.toggle('hidden', t !== panel);
  });

  document.querySelectorAll('#sb-tabs button').forEach(b => {
    b.classList.toggle('active', b.dataset.sb === panel);
  });
  const ct = document.getElementById('ct');
  if (ct) ct.style.display = (panel === 'vms') ? 'flex' : 'none';
  if (panel === 'containers') { navigateTo('containers'); }
  if (panel === 'infra') { navigateTo('mon-overview'); }
}
window.switchActivity = switchActivity;


var editorTabs = [];
var activeEditorTab = null;

function openEditorTab(id, label, icon) {
  if (!editorTabs.find(t => t.id === id)) {
    editorTabs.push({ id, label, icon: icon || '' });
  }
  activeEditorTab = id;
  renderEditorTabs();
}
window.openEditorTab = openEditorTab;

function closeEditorTab(id) {
  editorTabs = editorTabs.filter(t => t.id !== id);
  if (activeEditorTab === id) {
    activeEditorTab = editorTabs.length > 0 ? editorTabs[editorTabs.length - 1].id : null;
    if (activeEditorTab) navigateTo(activeEditorTab);
  }
  renderEditorTabs();
}
window.closeEditorTab = closeEditorTab;

function renderEditorTabs() {
  const container = document.getElementById('editor-tabs');
  if (!container) return;
  container.innerHTML = editorTabs.map(t =>
    '<div class="editor-tab ' + (t.id === activeEditorTab ? 'active' : '') + '" onclick="navigateTo(\'' + t.id + '\')">' +
    '<span class="tab-icon">' + t.icon + '</span>' +
    '<span class="tab-label">' + escapeHtml(t.label) + '</span>' +
    '<span class="tab-close" onclick="event.stopPropagation();closeEditorTab(\'' + t.id + '\')">&times;</span>' +
    '</div>'
  ).join('');
}
window.renderEditorTabs = renderEditorTabs;


function updateBreadcrumbs(page) {
  var el = document.getElementById('breadcrumbs');
  if (!el) return;
  var pg = page || currentTab || 'dashboard';
  var parts = [{ label: _L('대시보드', 'Dashboard'), tab: 'dashboard' }];
  var tabLabels = {
    'summary': _L('요약', 'Summary'), 'console': _L('콘솔', 'Console'),
    'snapshots': _L('스냅샷', 'Snapshots'), 'performance': _L('성능', 'Performance'),
    'timeline': _L('타임라인', 'Timeline'), 'networks': _L('네트워크', 'Networks'),
    'storage': _L('스토리지', 'Storage'), 'cluster': _L('클러스터', 'Cluster'),
    'containers': _L('컨테이너', 'Containers'), 'mon-overview': _L('운영 개요', 'Operations Overview'),
    'ops-triage': _L('이벤트 센터', 'Event Center'),
    'dashboard': _L('대시보드', 'Dashboard'),
    'host': _L('호스트 상태', 'Host Health'),
  };
  if (pg && pg !== 'dashboard') {
    var label = tabLabels[pg] || pg.replace(/-/g, ' ').replace(/\b\w/g, function(c) { return c.toUpperCase(); });
    parts.push({ label: label, tab: pg });
    if (['summary','console','snapshots','performance','timeline'].indexOf(pg) !== -1 && vmList[selectedVmIndex]) {
      parts.push({ label: vmList[selectedVmIndex].name, tab: null });
    }
  }
  var h = parts.map(function(p, i) {
    var isLast = i === parts.length - 1;
    if (isLast) return '<span class="color-accent" style="font-size:11px">' + esc(p.label) + '</span>';
    return '<span style="cursor:pointer;font-size:11px;color:var(--fg2)" onclick="navigateTo(\'' + (p.tab || 'dashboard') + '\')">' + esc(p.label) + '</span>';
  }).join(' <span class="color-muted" style="font-size:10px">&#9656;</span> ');
  h += ' <span style="cursor:pointer;font-size:10px;color:' + (_pinnedPages.includes(currentTab) ? 'var(--yellow)' : 'var(--fg2)') + '" onclick="togglePin(\'' + currentTab + '\');updateBreadcrumbs()">&#128204;</span>';
  h += ' <span style="cursor:pointer;font-size:10px;color:var(--fg2)" onclick="openInPopup()" title="' + _L('새 창에서 열기', 'Open in new window') + '">&#128194;</span>';
  el.innerHTML = h;
}
window.updateBreadcrumbs = updateBreadcrumbs;


window.globalSearchOpen = false;

function toggleGlobalSearch() {
  if (window.globalSearchOpen) { closeGlobalSearch(); return; }
  window.globalSearchOpen = true;
  const ov = document.createElement('div');
  ov.id = 'global-search-overlay';
  ov.className = 'global-search-overlay';
  ov.onclick = e => { if (e.target === ov) closeGlobalSearch(); };
  ov.innerHTML = '<div class="global-search-box">' +
    '<input class="global-search-input" id="global-search-input" placeholder="Search VMs, containers, networks, settings..." oninput="(window._gs || (window._gs = pcvDebounce(doGlobalSearch, 200)))(this.value)" autofocus>' +
    '<div class="global-search-results" id="global-search-results"></div></div>';
  document.body.appendChild(ov);
  setTimeout(() => document.getElementById('global-search-input')?.focus(), 50);
}
window.toggleGlobalSearch = toggleGlobalSearch;

function closeGlobalSearch() {
  window.globalSearchOpen = false;
  document.getElementById('global-search-overlay')?.remove();
}
window.closeGlobalSearch = closeGlobalSearch;

function doGlobalSearch(query) {
  const results = document.getElementById('global-search-results');
  if (!results || !query) { if (results) results.innerHTML = ''; return; }
  const q = query.toLowerCase();
  let html = '';


  const matchedVMs = vmList.filter(v => fuzzyMatch(v.name, q));
  if (matchedVMs.length > 0) {
    html += '<div class="global-search-group"><div class="global-search-group-title">Virtual Machines</div>';
    matchedVMs.forEach(v => {
      html += '<div class="global-search-item" onclick="closeGlobalSearch();selectedVmIndex=' + vmList.indexOf(v) + ';switchActivity(\'vms\');render()"><span class="search-icon">&#128187;</span><span class="search-label">' + escapeHtml(v.name) + '</span><span class="search-type">' + (v.state || '') + '</span></div>';
    });
    html += '</div>';
  }


  const pages = PCV.filterEditionItems([
    { id: 'networks', label: _L('네트워크', 'Networks'), icon: '&#127760;' },
    { id: 'storage', label: _L('스토리지', 'Storage'), icon: '&#128190;' },
    { id: 'containers', label: _L('컨테이너', 'Containers'), icon: '&#9783;' },
    { id: 'mon-overview', label: _L('운영 개요', 'Monitoring Overview'), icon: '&#128200;' },
    { id: 'ops-triage', label: _L('이벤트 센터', 'Event Center'), icon: '&#9889;' },
    { id: 'mon-alerts', label: _L('알림', 'Alerts'), icon: '&#128276;' },
    { id: 'mon-security', label: _L('보안 이벤트', 'Security Events'), icon: '&#128737;' },
    { id: 'accounts', label: _L('계정과 권한', 'Accounts'), icon: '&#128100;', role: 'admin' },
    { id: 'security-groups', label: _L('보안 그룹', 'Security Groups'), icon: '&#128737;' },
    { id: 'gpu', label: _L('GPU 장치', 'GPU'), icon: '&#127918;' },


    { id: 'apihelp', label: _L('Swagger API', 'Swagger API'), icon: '&#128214;' },
    { id: 'serviceguide', label: _L('서비스 가이드', 'Service Guide'), icon: '&#128218;' },
    { id: 'overlay', label: _L('오버레이 네트워크', 'Overlay Networks'), icon: '&#127760;' },
    { id: 'iscsi', label: _L('iSCSI 타깃', 'iSCSI Targets'), icon: '&#128190;' },
    { id: 'dpdk', label: 'DPDK', icon: '&#9889;' },
    { id: 'sriov', label: 'SR-IOV', icon: '&#128268;' },
  ]);
  const matchedPages = pages.filter(p => {
    if (p.cluster && !window.pcvClusterEnabled) return false;
    if (p.role && !pcvRoleAllows(p.role)) return false;
    return fuzzyMatch(p.label, q);
  });
  if (matchedPages.length > 0) {
    html += '<div class="global-search-group"><div class="global-search-group-title">Pages</div>';
    matchedPages.forEach(p => {
      html += '<div class="global-search-item" onclick="closeGlobalSearch();navigateTo(\'' + p.id + '\')"><span class="search-icon">' + p.icon + '</span><span class="search-label">' + p.label + '</span><span class="search-type">Page</span></div>';
    });
    html += '</div>';
  }

  if (!html) html = '<div style="padding:20px;text-align:center;color:var(--fg2)">No results for "' + escapeHtml(query) + '"</div>';
  results.innerHTML = html;
}
window.doGlobalSearch = doGlobalSearch;


window.zenMode = false;
function toggleZenMode() {
  window.zenMode = !window.zenMode;
  document.body.classList.toggle('zen-mode', window.zenMode);
  if (window.zenMode) toast('Zen Mode \u2014 press Escape to exit');
}
window.toggleZenMode = toggleZenMode;


var notifications = [];
window.notifCenterOpen = false;
var notifFilter = 'all';

function addNotification(type, title, msg) {
  notifications.unshift({ type: type, title: title, msg: msg || '', time: new Date(), read: false, id: Date.now() });
  if (notifications.length > 50) notifications.pop();
  updateNotifBadge();
  if (typeof sendBrowserNotif === 'function') sendBrowserNotif(title, msg);
  if (typeof playNotifSound === 'function') playNotifSound(type === 'error' ? 'error' : type === 'warning' ? 'warning' : 'success');
}
window.addNotification = addNotification;

function updateNotifBadge() {
  var unread = notifications.filter(function(n) { return !n.read; }).length;
  var badge = document.getElementById('alert-badge');
  if (badge) { badge.textContent = unread; badge.style.display = unread > 0 ? '' : 'none'; }
  var actBadge = document.querySelector('.activity-icon[data-panel="notifications"] .activity-badge');
  if (actBadge) { actBadge.textContent = unread; actBadge.style.display = unread > 0 ? '' : 'none'; }

  var toolbarBadge = document.getElementById('notif-toolbar-badge');
  if (toolbarBadge) { toolbarBadge.textContent = unread; toolbarBadge.style.display = unread > 0 ? '' : 'none'; }
}
window.updateNotifBadge = updateNotifBadge;

function _renderNotifList(container, filter) {
  var filtered = notifications;
  if (filter && filter !== 'all') {
    filtered = notifications.filter(function(n) { return n.type === filter; });
  }
  if (filtered.length === 0) {
    container.innerHTML = '<div class="notif-center-empty">&#128276; No ' + (filter !== 'all' ? filter + ' ' : '') + 'notifications</div>';
    return;
  }
  var list = '';
  filtered.forEach(function(n) {
    var icon = n.type === 'error' ? '&#10060;' : n.type === 'warning' ? '&#9888;' : '&#9989;';
    var timeAgo = _notifTimeAgo(n.time);
    list += '<div class="notif-item ' + (n.read ? '' : 'unread') + '" onclick="markRead(' + n.id + ');this.classList.remove(\'unread\')">'
      + '<span class="notif-icon">' + icon + '</span>'
      + '<div class="notif-body">'
      + '<div class="notif-title">' + escapeHtml(n.title) + '</div>'
      + (n.msg ? '<div class="notif-msg">' + escapeHtml(n.msg) + '</div>' : '')
      + '<div class="notif-time">' + timeAgo + '</div>'
      + '</div></div>';
  });
  container.innerHTML = list;
}

function _notifTimeAgo(date) {
  var diff = Math.floor((Date.now() - date.getTime()) / 1000);
  if (diff < 60) return diff + 's ago';
  if (diff < 3600) return Math.floor(diff / 60) + 'm ago';
  if (diff < 86400) return Math.floor(diff / 3600) + 'h ago';
  return date.toLocaleDateString();
}

function toggleNotifCenter() {
  if (window.notifCenterOpen) { closeNotifCenter(); return; }
  window.notifCenterOpen = true;
  notifFilter = 'all';
  var unread = notifications.filter(function(n) { return !n.read; }).length;
  var el = document.createElement('div');
  el.id = 'notif-center';
  el.className = 'notif-center';


  var header = '<div class="notif-center-header">'
    + '<span>Notifications' + (unread > 0 ? ' <span class="notif-header-badge">' + unread + ' unread</span>' : '') + '</span>'
    + '<div style="display:flex;gap:6px">'
    + '<button class="btn" style="font-size:9px;padding:2px 8px" onclick="markAllRead()">Mark all read</button>'
    + '<button class="btn btn-r" style="font-size:9px;padding:2px 8px" onclick="clearNotifications()">Clear</button>'
    + '<button class="panel-action-btn" onclick="closeNotifCenter()">&#10005;</button>'
    + '</div></div>';


  var filterBar = '<div class="notif-filter-bar">'
    + '<button class="notif-filter-btn active" data-filter="all" onclick="setNotifFilter(\'all\')">All (' + notifications.length + ')</button>'
    + '<button class="notif-filter-btn" data-filter="error" onclick="setNotifFilter(\'error\')">Error (' + notifications.filter(function(n){ return n.type==='error'; }).length + ')</button>'
    + '<button class="notif-filter-btn" data-filter="warning" onclick="setNotifFilter(\'warning\')">Warning (' + notifications.filter(function(n){ return n.type==='warning'; }).length + ')</button>'
    + '<button class="notif-filter-btn" data-filter="info" onclick="setNotifFilter(\'info\')">Info (' + notifications.filter(function(n){ return n.type==='info'; }).length + ')</button>'
    + '</div>';

  el.innerHTML = header + filterBar + '<div class="notif-center-list" id="notif-list-container"></div>';
  document.body.appendChild(el);


  var listContainer = document.getElementById('notif-list-container');
  _renderNotifList(listContainer, 'all');
}
window.toggleNotifCenter = toggleNotifCenter;

function setNotifFilter(filter) {
  notifFilter = filter;

  document.querySelectorAll('.notif-filter-btn').forEach(function(btn) {
    btn.classList.toggle('active', btn.dataset.filter === filter);
  });
  var listContainer = document.getElementById('notif-list-container');
  if (listContainer) _renderNotifList(listContainer, filter);
}
window.setNotifFilter = setNotifFilter;

function closeNotifCenter() {
  window.notifCenterOpen = false;
  var el = document.getElementById('notif-center');
  if (el) el.remove();
}
window.closeNotifCenter = closeNotifCenter;

function markAllRead() {
  notifications.forEach(function(n) { n.read = true; });
  updateNotifBadge();
  closeNotifCenter(); toggleNotifCenter();
}
window.markAllRead = markAllRead;

function markRead(id) {
  var n = notifications.find(function(x) { return x.id === id; });
  if (n) n.read = true;
  updateNotifBadge();
}
window.markRead = markRead;

function clearNotifications() {
  notifications = [];
  updateNotifBadge();
  closeNotifCenter(); toggleNotifCenter();
}
window.clearNotifications = clearNotifications;


var hoverCardTimeout = null;
var hoverCard = document.createElement('div');
hoverCard.className = 'hover-card';
hoverCard.id = 'hover-card';
document.body.appendChild(hoverCard);

function showHoverCard(e, vm) {
  clearTimeout(hoverCardTimeout);
  hoverCardTimeout = setTimeout(() => {
    const on = vm.state === 'running';
    hoverCard.innerHTML = '<div class="hover-card-title"><span class="dot ' + (on ? 'on' : 'off') + '"></span>' + escapeHtml(vm.name) + '</div>' +
      '<div class="hover-card-row"><span class="hc-k">State</span><span class="hc-v">' + (vm.state || '-') + '</span></div>' +
      '<div class="hover-card-row"><span class="hc-k">vCPU</span><span class="hc-v">' + (vm.vcpu || '-') + '</span></div>' +
      '<div class="hover-card-row"><span class="hc-k">Memory</span><span class="hc-v">' + (vm.memory_mb || '-') + ' MB</span></div>' +
      '<div class="hover-card-row"><span class="hc-k">CPU</span><span class="hc-v">' + ((vm.live_cpu_pct || 0).toFixed(1)) + '%</span></div>';
    hoverCard.style.left = (e.clientX + 16) + 'px';
    hoverCard.style.top = (e.clientY - 10) + 'px';
    hoverCard.classList.add('visible');
  }, 300);
}
window.showHoverCard = showHoverCard;

function hideHoverCard() {
  clearTimeout(hoverCardTimeout);
  hoverCard.classList.remove('visible');
}
window.hideHoverCard = hideHoverCard;


function highlightText(text, query) {
  if (!query || !text) return esc(text);
  var escaped = esc(text);
  var re = new RegExp('(' + query.replace(/[.*+?^${}()|[\]\\]/g, '\\$&') + ')', 'gi');
  return escaped.replace(re, '<mark style="background:var(--yellow);color:#000;padding:0 2px;border-radius:2px">$1</mark>');
}
window.highlightText = highlightText;


(function() {
  var touchStartX = 0, touchStartY = 0, touchStartTime = 0;
  document.addEventListener('touchstart', function(e) {
    touchStartX = e.touches[0].clientX;
    touchStartY = e.touches[0].clientY;
    touchStartTime = Date.now();
  }, { passive: true });

  document.addEventListener('touchend', function(e) {
    var dx = e.changedTouches[0].clientX - touchStartX;
    var dy = e.changedTouches[0].clientY - touchStartY;
    var dt = Date.now() - touchStartTime;
    if (dt > 500 || Math.abs(dx) < 50) return;
    if (Math.abs(dy) > Math.abs(dx)) return;

    var tabs = ['summary', 'console', 'snapshots', 'performance', 'timeline'];
    var idx = tabs.indexOf(currentTab);
    if (idx === -1) return;

    if (dx < -50 && idx < tabs.length - 1) {
      currentTab = tabs[idx + 1];
      renderContent();
    } else if (dx > 50 && idx > 0) {
      currentTab = tabs[idx - 1];
      renderContent();
    }
  }, { passive: true });


  var longPressTimer = null;
  document.addEventListener('touchstart', function(e) {
    var vi = e.target.closest('.vi');
    if (!vi) return;
    longPressTimer = setTimeout(function() {
      var idx = Array.from(vi.parentElement.children).filter(function(c) { return c.classList.contains('vi'); }).indexOf(vi);
      if (idx >= 0 && typeof showCtx === 'function') {
        showCtx({ preventDefault: function(){}, pageX: e.touches[0].clientX, pageY: e.touches[0].clientY }, idx);
      }
    }, 600);
  }, { passive: true });
  document.addEventListener('touchend', function() { clearTimeout(longPressTimer); }, { passive: true });
  document.addEventListener('touchmove', function() { clearTimeout(longPressTimer); }, { passive: true });
})();


(function() {
  var handle = document.getElementById('sb-resize');
  var sidebar = document.querySelector('.sidebar-panel') || document.querySelector('.sidebar');
  if (!handle || !sidebar) return;
  var startX, startW;
  handle.addEventListener('mousedown', function(e) {
    e.preventDefault();
    startX = e.clientX;
    startW = sidebar.offsetWidth;
    handle.classList.add('active');
    function onMove(e2) {
      var newW = Math.max(180, Math.min(500, startW + e2.clientX - startX));
      sidebar.style.width = newW + 'px';
      document.documentElement.style.setProperty('--sw', newW + 'px');
    }
    function onUp() {
      handle.classList.remove('active');
      document.removeEventListener('mousemove', onMove);
      document.removeEventListener('mouseup', onUp);
      localStorage.setItem('pcv-sb-width', sidebar.style.width);
    }
    document.addEventListener('mousemove', onMove);
    document.addEventListener('mouseup', onUp);
  });
  var saved = localStorage.getItem('pcv-sb-width');
  if (saved) { sidebar.style.width = saved; document.documentElement.style.setProperty('--sw', saved); }
})();


PCV.nav = {
  togglePin: togglePin,
  renderPinnedBar: renderPinnedBar,
  navigateTo: navigateTo,
  switchSbTab: switchSbTab,
  renderContent: renderContent,
  updateStatusBar: updateStatusBar,
  updateFavicon: updateFavicon,
  toggleSB: toggleSB,
  toggleFS: toggleFS,
  toggleInfraSort: toggleInfraSort,
  switchPanelTab: switchPanelTab,
  toggleBottomPanel: toggleBottomPanel,
  togglePanelSize: togglePanelSize,
  CMD_ACTIONS: CMD_ACTIONS,
  fuzzyMatch: fuzzyMatch,
  openCmdPalette: openCmdPalette,
  closeCmdPalette: closeCmdPalette,
  renderCmdPalette: renderCmdPalette,
  openInPopup: openInPopup,
  toggleMobileSB: toggleMobileSB,
  closeMobileSB: closeMobileSB,
  switchActivity: switchActivity,
  openEditorTab: openEditorTab,
  closeEditorTab: closeEditorTab,
  renderEditorTabs: renderEditorTabs,
  updateBreadcrumbs: updateBreadcrumbs,
  toggleGlobalSearch: toggleGlobalSearch,
  closeGlobalSearch: closeGlobalSearch,
  doGlobalSearch: doGlobalSearch,
  toggleZenMode: toggleZenMode,
  addNotification: addNotification,
  updateNotifBadge: updateNotifBadge,
  toggleNotifCenter: toggleNotifCenter,
  setNotifFilter: setNotifFilter,
  closeNotifCenter: closeNotifCenter,
  markAllRead: markAllRead,
  markRead: markRead,
  clearNotifications: clearNotifications,
  showHoverCard: showHoverCard,
  hideHoverCard: hideHoverCard,
  highlightText: highlightText
};
})(window.PCV);






window.PCV = window.PCV || {};
(function(PCV) {


var THEME_PREVIEWS = [
  { id: 'supanova',         name: 'SUPANOVA (Teal)',  colors: ['#07090c','#14b8a6','#34d399','#f43f5e'] },
  { id: 'supanova-cyan',    name: 'SUPANOVA CYAN',    colors: ['#07090c','#0891b2','#34d399','#f43f5e'] },
  { id: 'supanova-hicontrast', name: 'SUPANOVA HI-CONTRAST', colors: ['#030405','#facc15','#ffffff','#ff4d6d'] },
];


var SUPANOVA_THEMES = ['supanova', 'supanova-cyan', 'supanova-hicontrast'];
function sanitizeTheme(t) {
  return SUPANOVA_THEMES.indexOf(t) >= 0 ? t : 'supanova';
}

function changeTheme(t) {
  t = sanitizeTheme(t);
  document.documentElement.setAttribute('data-theme', t);
  localStorage.setItem('pcv-theme', t);

  destroyAllCharts();

  try { window.dispatchEvent(new Event('pcv-theme-change')); } catch (_) {}
  if (typeof renderContent === 'function') {
    try { renderContent(); } catch(e) {}
  }
}

function toggleTheme() {
  const themes = SUPANOVA_THEMES;
  const cur = document.documentElement.getAttribute('data-theme') || 'supanova';
  const idx = (themes.indexOf(cur) + 1) % themes.length;
  changeTheme(themes[idx]);
  const s = document.getElementById('theme-select');
  if (s) s.value = themes[idx];
}





var THEME_VARS = ['bg','bg2','bg3','fg','fg2','accent','green','red','yellow','cyan','peach','magenta','border'];

function openThemeEditor() {
  var originalTheme = document.documentElement.getAttribute('data-theme') || '';
  const style = getComputedStyle(document.documentElement);
  let h = '<h2>Theme Editor</h2>';
  h += '<div class="theme-editor"><div class="theme-editor-grid">';
  THEME_VARS.forEach(v => {
    const cur = style.getPropertyValue('--' + v).trim();
    const hex = cssColorToHex(cur);
    h += '<div class="theme-editor-item"><label>--' + v + '</label><input type="color" value="' + hex + '" data-var="' + v + '" onchange="previewThemeVar(this)"><span style="font-size:9px;color:var(--fg2)" id="te-val-' + v + '">' + hex + '</span></div>';
  });
  h += '</div></div>';
  h += '<div class="theme-editor-actions">';
  h += '<button class="btn btn-g" onclick="saveCustomTheme()">Save as Custom</button>';
  h += '<button class="btn" onclick="exportTheme()">Export JSON</button>';
  h += '<button class="btn" onclick="importTheme()">Import</button>';
  h += '<button class="btn btn-r" onclick="changeTheme(\'' + originalTheme + '\');closeModal()" style="margin-left:8px">' + (_L ? _L('원래 테마로', 'Reset to Original') : 'Reset to Original') + '</button>';
  h += '<button class="btn btn-r" onclick="closeModal()">Cancel</button>';
  h += '</div>';
  showModal(h);
}

function cssColorToHex(c) {
  if (!c) return '#000000';
  c = c.trim();
  if (c.startsWith('#')) return c.length === 4 ? '#' + c[1]+c[1]+c[2]+c[2]+c[3]+c[3] : c;
  const m = c.match(/rgba?\((\d+),\s*(\d+),\s*(\d+)/);
  if (m) return '#' + [m[1],m[2],m[3]].map(x => parseInt(x).toString(16).padStart(2,'0')).join('');
  return '#000000';
}

function previewThemeVar(el) {
  const v = el.dataset.var;
  document.documentElement.style.setProperty('--' + v, el.value);
  const span = document.getElementById('te-val-' + v);
  if (span) span.textContent = el.value;
}

function saveCustomTheme() {
  const custom = {};
  THEME_VARS.forEach(v => {
    const el = document.querySelector('.theme-editor-item input[data-var="' + v + '"]');
    if (el) custom[v] = el.value;
  });
  localStorage.setItem('pcv-custom-theme', JSON.stringify(custom));
  applyCustomTheme(custom);
  toast('Custom theme saved');
  closeModal();
}

function applyCustomTheme(vars) {
  Object.entries(vars).forEach(([k, v]) => {
    document.documentElement.style.setProperty('--' + k, v);
  });
  document.documentElement.setAttribute('data-theme', 'custom');
  localStorage.setItem('pcv-theme', 'custom');
  const s = document.getElementById('theme-select');
  if (s) s.value = 'custom';
}

function exportTheme() {
  const custom = {};
  THEME_VARS.forEach(v => {
    const el = document.querySelector('.theme-editor-item input[data-var="' + v + '"]');
    if (el) custom[v] = el.value;
  });
  const blob = new Blob([JSON.stringify(custom, null, 2)], { type: 'application/json' });
  const a = document.createElement('a'); a.href = URL.createObjectURL(blob);
  a.download = 'pcv-theme.json'; a.click();
  toast(t('msg.theme_exported'));
}

function importTheme() {
  const input = document.createElement('input'); input.type = 'file'; input.accept = '.json';
  input.onchange = e => {
    const file = e.target.files[0]; if (!file) return;
    const reader = new FileReader();
    reader.onload = ev => {
      try {
        const vars = JSON.parse(ev.target.result);
        localStorage.setItem('pcv-custom-theme', JSON.stringify(vars));
        applyCustomTheme(vars);
        toast(t('msg.theme_imported'));
        closeModal();
      } catch (err) { toast(t('msg.invalid_theme'), false); }
    };
    reader.readAsText(file);
  };
  input.click();
}


function exportUiSettings() {
  var settings = {
    theme: document.documentElement.getAttribute('data-theme') || '',
    sidebarWidth: localStorage.getItem('pcv-sb-width') || '',
    vmViewMode: localStorage.getItem('pcv-vm-view') || 'list',
    language: typeof I18N !== 'undefined' ? I18N.getLang() : 'ko',
    autoTheme: localStorage.getItem('pcv-auto-theme') || 'false',
    favorites: localStorage.getItem('pcv-favorites') || '[]',
    customTheme: localStorage.getItem('pcv-custom-theme') || '',
  };
  var blob = new Blob([JSON.stringify(settings, null, 2)], { type: 'application/json' });
  var a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = 'purecvisor-settings.json';
  a.click();
  toast(_L ? _L('설정 내보내기 완료', 'Settings exported') : 'Settings exported');
}

function importUiSettings() {
  var input = document.createElement('input');
  input.type = 'file'; input.accept = '.json';
  input.onchange = function() {
    var file = input.files[0]; if (!file) return;
    var reader = new FileReader();
    reader.onload = function(e) {
      try {
        var s = JSON.parse(e.target.result);
        if (s.theme !== undefined) { changeTheme(s.theme); }
        if (s.sidebarWidth) localStorage.setItem('pcv-sb-width', s.sidebarWidth);
        if (s.vmViewMode) localStorage.setItem('pcv-vm-view', s.vmViewMode);
        if (s.language && typeof I18N !== 'undefined') { I18N.setLang(s.language); if (typeof applyI18n === 'function') applyI18n(); }
        if (s.autoTheme) localStorage.setItem('pcv-auto-theme', s.autoTheme);
        if (s.favorites) localStorage.setItem('pcv-favorites', s.favorites);
        if (s.customTheme) localStorage.setItem('pcv-custom-theme', s.customTheme);
        toast(_L ? _L('설정 가져오기 완료 — 새로고침 권장', 'Settings imported — refresh recommended') : 'Settings imported');
      } catch (err) {
        toast(_L ? _L('잘못된 설정 파일', 'Invalid settings file') : 'Invalid', false);
      }
    };
    reader.readAsText(file);
  };
  input.click();
}
window.exportUiSettings = exportUiSettings;
window.importUiSettings = importUiSettings;


PCV.theme = {
  PREVIEWS: THEME_PREVIEWS,
  VARS: THEME_VARS,
  change: changeTheme,
  toggle: toggleTheme,
  sanitize: sanitizeTheme,
  openEditor: openThemeEditor,
  cssColorToHex: cssColorToHex,
  previewVar: previewThemeVar,
  saveCustom: saveCustomTheme,
  applyCustom: applyCustomTheme,
  exportTheme: exportTheme,
  importTheme: importTheme,
  exportUiSettings: exportUiSettings,
  importUiSettings: importUiSettings
};


window.THEME_PREVIEWS = THEME_PREVIEWS;
window.changeTheme = changeTheme;
window.toggleTheme = toggleTheme;
window.sanitizeTheme = sanitizeTheme;
window.THEME_VARS = THEME_VARS;
window.openThemeEditor = openThemeEditor;
window.cssColorToHex = cssColorToHex;
window.previewThemeVar = previewThemeVar;
window.saveCustomTheme = saveCustomTheme;
window.applyCustomTheme = applyCustomTheme;
window.exportTheme = exportTheme;
window.importTheme = importTheme;
})(window.PCV);
















window.PCV = window.PCV || {};
(function(PCV) {

var agentTab = 'providers';


async function getCurrentAccountRole() {
  if (window.currentUser && window.currentUser.role) {
    return String(window.currentUser.role).toLowerCase();
  }
  if (!window.authToken) return '';
  try {
    var res = await fetchGet(window.API_BASE + '/auth/whoami');
    if (res && res.data) {
      window.currentUser = res.data;
      if (typeof applyRoleVisibility === 'function') applyRoleVisibility(res.data.role);
      return String(res.data.role || '').toLowerCase();
    }
  } catch (_) {}
  return '';
}

async function isAdminAccountView() {
  return (await getCurrentAccountRole()) === 'admin';
}

function renderAdminOnlyNotice(b) {
  b.innerHTML = '<div style="padding:40px;text-align:center">'
    + '<div style="font-size:44px;margin-bottom:12px">&#128274;</div>'
    + '<h3 style="color:var(--yellow);margin-bottom:10px">' + _L('관리자 전용 화면', 'Admin-only page') + '</h3>'
    + '<p class="color-muted" style="margin-bottom:14px">' + _L('계정과 API 키 관리는 admin 역할에서만 사용할 수 있습니다.', 'Account and API key management are available only to the admin role.') + '</p>'
    + '<button class="btn" onclick="navigateTo(\'dashboard\')">' + _L('대시보드로 이동', 'Go to Dashboard') + '</button>'
    + '</div>';
}

async function renderAccounts(b) {
  if (!await isAdminAccountView()) {
    renderAdminOnlyNotice(b);
    return;
  }
  b.innerHTML = showSkeleton();
  try { const r = await fetchGet(EP.AUTH_USERS()); const l = unwrapList(r);
    let h = '<h3 class="mb-14">&#128100; Account Management ' + H.badge('RBAC', 'y') + '</h3>';
    h += '<div class="sg grid-2" style="gap:14px"><div><div id="acct-table"></div></div>';
    setTimeout(function() {
      createDataTable('acct-table', {
        headers: [
          {key:'username', label:'Username', sortable: true},
          {key:'role', label:'Role', sortable: true},
          {key:'tenant', label:'Tenant', sortable: true},
          {key:'actions', label: t('vm.settings'), sortable: false}
        ],
        rows: l.map(function(u) {
          var rc = u.role === 'admin' ? 'var(--red)' : u.role === 'operator' ? 'var(--yellow)' : 'var(--fg2)';
          var actions = '';
          if (u.username !== 'admin') {
            actions = '<select id="role-' + escapeHtml(u.username) + '" style="background:var(--bg);border:1px solid var(--border);color:var(--fg);border-radius:4px;padding:2px 6px;font-size:10px"><option ' + (u.role === 'viewer' ? 'selected' : '') + '>viewer</option><option ' + (u.role === 'operator' ? 'selected' : '') + '>operator</option><option ' + (u.role === 'admin' ? 'selected' : '') + '>admin</option></select> <button class="btn btn-xxs" onclick="acctRole(\'' + escapeHtml(u.username) + '\')">Set</button> <button class="btn btn-r btn-xxs" onclick="acctDel(\'' + escapeHtml(u.username) + '\')">' + t('btn.delete') + '</button>';
          } else {
            actions = '<span class="stat-label">System admin</span>';
          }
          return [
            '<b>' + escapeHtml(u.username) + '</b>',
            '<span class="badge" style="border:1px solid ' + rc + ';color:' + rc + '">' + escapeHtml(u.role) + '</span>',
            escapeHtml(u.tenant || '---'),
            actions
          ];
        }),
        searchable: true,
        exportable: true,
        emptyText: 'No users'
      });
    }, 0);
    h += H.card(t('btn.create') + ' User', '<div class="fr"><label>Username</label><input id="acct-user" placeholder="newuser"></div><div class="fr"><label>Password</label><input id="acct-pass" type="password" placeholder="password"></div><div class="fr"><label>Role</label><select id="acct-role" style="width:100%;padding:6px;background:var(--bg);border:1px solid var(--border);color:var(--fg);border-radius:4px"><option>viewer</option><option selected>operator</option><option>admin</option></select></div><button class="btn btn-g mt-8 w-full" onclick="acctCreate()">' + t('btn.create') + ' User</button>');
    h += '</div>';
    b.innerHTML = h;
  } catch (e) { b.innerHTML = '<p class="color-red">Error loading accounts</p>'; }
}

async function acctCreate() { const u = document.getElementById('acct-user')?.value; const p = document.getElementById('acct-pass')?.value; const r = document.getElementById('acct-role')?.value;
  if (!u || !p) { toast(t('auth.required'), false); return; }
  try { const res = await fetchPost(EP.AUTH_USERS(), { username: u, password: p, role: r || 'operator' });
    if (res.error) { toast(res.error.message || 'Failed', false); } else { toast(t('auth.user_created') + ': ' + u); addEvt('IAM User created — ' + u + ' (role: ' + (r || 'operator') + ')'); renderAccounts(document.getElementById('cb')); } } catch (e) { toast(e.message, false); } }

async function acctRole(u) { const r = document.getElementById('role-' + u)?.value; if (!r) return;
  try { const res = await fetchPost(EP.AUTH_ROLE(), { username: u, role: r });
    if (res.error) { toast(res.error.message || 'Failed', false); } else { toast(t('auth.role_changed') + ': ' + u + ' → ' + r); addEvt('IAM Role changed — ' + u + ' → ' + r); renderAccounts(document.getElementById('cb')); } } catch (e) { toast(e.message, false); } }

async function acctDel(u) { if (!await customConfirm(t('btn.delete'), 'User ' + u + '?')) return;
  try { const res = await fetchDelete(EP.AUTH_USER(u));
    if (res.error) { toast(res.error.message || 'Failed', false); } else { toast(t('auth.user_deleted') + ': ' + u); addEvt('IAM User deleted — ' + u); renderAccounts(document.getElementById('cb')); } } catch (e) { toast(e.message, false); } }


async function renderApiManagement(b) {
  if (!await isAdminAccountView()) {
    renderAdminOnlyNotice(b);
    return;
  }
  let h = '<div class="flex items-center gap-10 mb-16"><span class="neon-blink color-yellow">&gt;&gt;</span><h2 style="font-family:var(--font-display);font-size:16px">API Management</h2></div>';
  h += H.grid(4,
    H.card('&#128268; Total Endpoints', '<div class="stat-xl color-accent" id="api-ep-count">...</div>')
  + H.card('&#128274; Auth', '<div class="stat-md color-green">JWT HS256</div>')
  + H.card('&#128101; RBAC', '<div class="stat-md" style="color:var(--magenta)">3 Levels</div>')
  + H.card('&#9889; Rate Limit', '<div class="stat-md color-yellow" id="api-rl-count">...</div>')
  ) + '<div class="mb-16"></div>';

  fetchGet(EP.HEALTH()).then(function(r) {
    var d = unwrapData(r);
    var ep = document.getElementById('api-ep-count');
    if (ep) ep.textContent = (d.rest_endpoints || '190') + '+';
    var rl = document.getElementById('api-rl-count');
    if (rl) rl.textContent = (d.rate_limit || '600') + ' req/min';
  }).catch(function() {
    var ep = document.getElementById('api-ep-count');
    if (ep) ep.textContent = '190+';
    var rl = document.getElementById('api-rl-count');
    if (rl) rl.textContent = '600 req/min';
  });
  h += H.card('<span class="color-accent">&#128272; JWT Token — Quick Test</span>', '<div class="flex gap-8 items-center mb-8 flex-wrap"><input id="apimgmt-user" value="admin" placeholder="Username" style="padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:6px;font-size:12px;width:140px"><input id="apimgmt-pass" type="password" value="admin" placeholder="Password" style="padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:6px;font-size:12px;width:140px"><button class="btn btn-g" onclick="apiMgmtGetToken()">&#9654; Get Token</button><button class="btn" onclick="apiMgmtTestHealth()">&#128994; Health Check</button></div><div id="apimgmt-token-result" class="stat-label" style="word-break:break-all;max-height:60px;overflow:auto"></div>', 'mb-14');
  h += H.card('<span class="color-green">&#128640; API Request Tester</span>', '<div class="flex gap-8 items-center mb-8 flex-wrap"><select id="apimgmt-method" style="padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--accent);border-radius:6px;font-size:12px;font-weight:700"><option>GET</option><option>POST</option><option>PUT</option><option>DELETE</option></select><input id="apimgmt-path" value="/api/v1/vms" style="flex:1;min-width:200px;padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:6px;font-size:12px"><button class="btn btn-g" onclick="apiMgmtSend()">&#9654; Send</button></div><textarea id="apimgmt-body" placeholder="Request body (JSON)" rows="2" style="width:100%;padding:6px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:6px;font-size:11px;resize:vertical"></textarea><div id="apimgmt-result" style="background:var(--bg);border:1px solid var(--border);border-radius:6px;padding:10px;max-height:300px;overflow:auto;font-size:11px;color:var(--cyan);white-space:pre-wrap;display:none"></div>', 'mb-14');
  h += H.card('<span class="color-yellow">&#128268; gRPC Server</span>', '<div id="grpc-status" class="text-12 color-muted">Checking...</div><div style="margin-top:6px;font-size:11px;color:var(--fg2)">Port: 50051 | Protocol: protobuf-c binary framing<br>Transport: TCP (HTTP/2 planned)<br>Config: daemon.conf <code>[grpc] enabled=true</code></div>', 'mb-14');


  h += '<div class="hc mb-14"><h4>&#128273; API Keys</h4>';
  h += '<p class="stat-label" style="margin-bottom:10px">Create and manage API keys for programmatic access. Keys use the same RBAC as user tokens.</p>';
  h += '<div style="display:flex;gap:8px;flex-wrap:wrap;margin-bottom:12px">';
  h += '<input id="apikey-desc" placeholder="Key description (e.g. CI pipeline)" style="flex:1;min-width:180px;padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:6px;font-size:12px">';
  h += '<input id="apikey-expiry" type="number" value="90" min="1" max="365" style="width:80px;padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:6px;font-size:12px" title="Expiry (days)">';
  h += '<span class="color-muted" style="font-size:11px;align-self:center">days</span>';
  h += '<button class="btn btn-g" onclick="apiKeyCreate()">+ Create Key</button>';
  h += '</div>';
  h += '<div id="apikey-new-result" style="display:none;margin-bottom:12px;padding:10px;border:1px solid var(--green);border-radius:6px;background:rgba(0,255,0,.04);font-size:11px"></div>';
  h += '<div id="apikey-list"><span class="spinner"></span> Loading keys...</div>';
  h += '</div>';

  h += '<div class="flex gap-8 flex-wrap"><button class="btn" onclick="navigateTo(\'apihelp\')">&#128214; Swagger API</button><button class="btn" onclick="navigateTo(\'restguide\')">&#128220; REST API Guide</button><button class="btn" onclick="navigateTo(\'accounts\')">&#128100; Accounts</button></div>';
  b.innerHTML = h;
  setTimeout(() => {
    const el = document.getElementById('grpc-status');
    if (!el) return;
    el.innerHTML = H.badge('Config-based', 'y') + ' daemon.conf [grpc] enabled check required<br><code class="text-xs color-cyan">pcvctl grpc status</code> to verify from CLI';
  }, 100);
  setTimeout(() => {

    if (document.getElementById('apikey-list-area')) apiKeyList();
  }, 120);
}

async function apiMgmtGetToken() { const u = document.getElementById('apimgmt-user').value, p = document.getElementById('apimgmt-pass').value; const el = document.getElementById('apimgmt-token-result');
  try { const r = await fetch(EP.AUTH_TOKEN(), { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ username: u, password: p }) }); const d = await r.json();
    if (d.access_token) { el.innerHTML = '<span class="color-green">&#9989; Token:</span> <code class="color-accent">' + escapeHtml(d.access_token.substring(0, 50)) + '...</code>'; }
    else { el.innerHTML = '<span class="color-red">&#10060; ' + escapeHtml(JSON.stringify(d)) + '</span>'; }
  } catch (e) { el.innerHTML = '<span class="color-red">&#10060; ' + escapeHtml(e.message) + '</span>'; } }

async function apiMgmtTestHealth() { const el = document.getElementById('apimgmt-token-result');
  try { const r = await fetchGet(EP.HEALTH()); const d = unwrapData(r); el.innerHTML = '<span class="color-green">&#9989; Status: ' + esc(d.status || 'ok') + '</span> | edition: ' + esc(window.PCV_UI_EDITION || 'single'); } catch (e) { el.innerHTML = '<span class="color-red">&#10060; ' + esc(e.message) + '</span>'; } }

async function apiMgmtSend() { const m = document.getElementById('apimgmt-method').value, path = document.getElementById('apimgmt-path').value; const body = document.getElementById('apimgmt-body').value, el = document.getElementById('apimgmt-result');
  el.style.display = 'block'; el.textContent = t('loading');
  try { const opts = { method: m, headers: {} }; if (authToken) opts.headers['Authorization'] = 'Bearer ' + authToken;
    if ((m === 'POST' || m === 'PUT') && body) { opts.headers['Content-Type'] = 'application/json'; opts.body = body; }
    const r = await fetch(location.origin + path, opts); const txt = await r.text(); let pretty = txt; try { pretty = JSON.stringify(JSON.parse(txt), null, 2); } catch (e) { if(_DEBUG) console.warn('apiMgmtSend:', e.message); }
    el.innerHTML = '<div class="mb-6">' + H.badge(String(r.status), r.ok ? 'g' : 'r') + ' <span class="color-muted">' + escapeHtml(m) + ' ' + escapeHtml(path) + '</span></div><pre style="white-space:pre-wrap">' + escapeHtml(pretty) + '</pre>';
  } catch (e) { el.innerHTML = '<span class="color-red">Error: ' + escapeHtml(e.message) + '</span>'; } }


async function showAgentConfig() {
  try { const r = await fetchGet(EP.AGENT_CONFIG()); const d = unwrapData(r); window._agentCfg = d;
    let h = '<h2>&#129302; AI Agent Configuration</h2>';
    h += '<div class="flex" style="border-bottom:1px solid var(--border);margin-bottom:12px;gap:2px">';
    ['providers', 'settings', 'history', 'status'].forEach(t2 => { h += '<div onclick="agentTab=\'' + t2 + '\';showAgentConfig()" style="padding:8px 16px;font-size:12px;cursor:pointer;border-bottom:2px solid ' + (agentTab === t2 ? 'var(--accent)' : 'transparent') + ';color:' + (agentTab === t2 ? 'var(--accent)' : 'var(--fg2)') + ';font-weight:' + (agentTab === t2 ? '600' : '400') + '">' + { providers: 'Providers', settings: t('vm.settings'), history: 'History', status: 'Status' }[t2] + '</div>'; });
    h += '</div><div id="agent-tab-body"></div>'; showModal(h); setTimeout(() => renderAgentTab(d), 50);
  } catch (e) { toast('Failed to load agent config: ' + e.message, false); }
}
function renderAgentTab(d) { const b = document.getElementById('agent-tab-body'); if (!b) return; if (agentTab === 'providers') renderAgentProviders(b, d); else if (agentTab === 'settings') renderAgentSettings(b, d); else if (agentTab === 'history') renderAgentHistory(b); else if (agentTab === 'status') renderAgentStatus(b, d); }

function renderAgentProviders(b, d) {
  const provs = d.providers || []; let h = '';
  provs.forEach((p, i) => { const ico = { Claude: '&#129302;', OpenAI: '&#9889;', Gemini: '&#128142;', Ollama: '&#128026;' }[p.name] || '&#9881;';
    h += '<div class="hc mb-10"><h4 class="justify-between items-center"><span>' + ico + ' ' + p.name + '</span><label style="display:flex;align-items:center;gap:6px;cursor:pointer;font-size:10px"><input type="checkbox" id="agen' + i + '" ' + (p.enabled ? 'checked' : '') + ' style="accent-color:var(--accent)">' + (p.enabled ? '<span class="color-green">ENABLED</span>' : '<span class="color-muted">DISABLED</span>') + '</label></h4>';
    h += '<div class="fr"><label>Model</label><input id="agm' + i + '" value="' + (p.model || '') + '" class="text-11"></div>';
    h += '<div class="fr"><label>API Key</label><div class="flex gap-4 flex-1"><input id="agk' + i + '" type="password" value="' + (p.api_key || '') + '" class="text-11 flex-1"><button class="btn" onclick="toggleKeyVis(' + i + ')" style="font-size:10px;padding:4px 8px" id="agt' + i + '">Show</button></div></div>';
    h += '<div class="fr"><label>Endpoint</label><input id="age' + i + '" value="' + (p.endpoint || '') + '" class="text-11"></div>';
    h += '<div class="flex gap-6 mt-8"><button class="btn" onclick="testProvider(' + i + ',\'' + p.name + '\')" style="font-size:10px;padding:4px 10px">&#9889; Test</button>';
    h += (i === 0 ? '<button class="btn" onclick="testAllProviders()" style="font-size:10px;padding:4px 10px">&#9889; Test All</button>' : '');
    h += '<span id="agr' + i + '" class="text-11"></span></div></div>'; });
  h += '<div class="flex gap-8 justify-end mt-14"><button class="btn btn-g" onclick="saveAgentConfig()">' + t('btn.save') + ' All</button><button class="btn btn-r" onclick="closeModal()">' + t('btn.cancel') + '</button></div>';
  b.innerHTML = h;
}

function renderAgentSettings(b, d) {
  let h = '<div class="hc mb-10"><h4>&#9881; General ' + t('vm.settings') + '</h4>';
  h += '<div class="fr"><label>Rate Limit</label><div class="flex gap-6 items-center flex-1"><input id="ag-rate" type="number" value="' + (d.rate_limit_sec || 300) + '" class="text-11 w-80"><span class="stat-label">seconds between queries</span></div></div>';
  h += '<div class="fr"><label>Timeout</label><div class="flex gap-6 items-center flex-1"><input id="ag-timeout" type="number" value="' + (d.timeout_sec || 10) + '" class="text-11 w-80"><span class="stat-label">seconds per request</span></div></div></div>';
  h += '<div class="hc mb-10"><h4>&#128202; Statistics</h4>' + H.row('Total Queries', '<span class="color-accent">' + (d.total_queries || 0) + '</span>');
  const en = ((d.providers || []).filter(p => p.enabled).length);
  h += H.row('Active Providers', '<span class="color-green">' + en + ' / ' + (d.providers || []).length + '</span>') + '</div>';
  h += '<div class="flex gap-8 justify-end mt-14"><button class="btn btn-g" onclick="saveAgentSettings()">' + t('btn.save') + '</button><button class="btn btn-r" onclick="closeModal()">' + t('btn.cancel') + '</button></div>';
  b.innerHTML = h;
}

async function renderAgentHistory(b) {
  b.innerHTML = '<div class="text-center p-20"><span class="spinner"></span> ' + t('loading') + '</div>';
  try { const r = await fetchGet(EP.AGENT_HISTORY()); const d = unwrapData(r);
    if (!d || !d.consensus) { b.innerHTML = H.card('&#128202; History', '<p class="color-muted text-12 mt-8">No data yet.</p>'); return; }
    let h = '<div class="hc mb-10"><h4>&#128202; Last Consensus Result</h4>';
    h += H.row('Action', '<span class="color-accent">' + d.consensus + '</span>');
    h += H.row('Confidence', '<span class="color-green">' + (d.confidence || 0).toFixed(2) + '</span>');
    h += H.row('Avg Latency', (d.avg_latency_ms || 0).toFixed(0) + ' ms');
    if (d.timestamp) { h += H.row('Timestamp', '<span class="text-xs">' + new Date(d.timestamp * 1000).toLocaleString() + '</span>'); }
    h += '</div>';
    if (d.providers && d.providers.length) { h += H.card('&#129302; Per-Provider Results', '<table><thead><tr><th>Provider</th><th>Action</th><th>Target</th><th>Confidence</th><th>Latency</th><th>Urgency</th><th>Status</th></tr></thead><tbody>' +
      d.providers.map(p => '<tr><td><b>' + p.provider + '</b><br><span class="stat-label">' + p.model + '</span></td><td>' + p.action + '</td><td>' + (p.target_vm || '-') + (p.from_node ? ' <span class="color-muted">' + p.from_node + '→' + p.to_node + '</span>' : '') + '</td><td class="color-green">' + (p.confidence || 0).toFixed(2) + '</td><td>' + (p.latency_ms || 0).toFixed(0) + 'ms</td><td>' + H.badge(p.urgency || '-', p.urgency === 'high' ? 'r' : p.urgency === 'medium' ? 'y' : 'g') + '</td><td>' + (p.success ? '<span class="color-green">OK</span>' : '<span class="color-red">' + (p.error || 'FAIL') + '</span>') + '</td></tr>').join('') + '</tbody></table>');
      if (d.providers[0] && d.providers[0].reason) { h += '<div class="hc mt-10"><h4>&#128172; Reasoning</h4>'; d.providers.forEach(p => { if (p.reason) h += '<div class="mb-6"><b class="color-accent">' + p.provider + ':</b> <span class="text-11">' + p.reason + '</span></div>'; }); h += '</div>'; } }
    b.innerHTML = h;
  } catch (e) { b.innerHTML = H.card('', '<p class="color-red">Failed: ' + escapeHtml(e.message) + '</p>'); }
}

function renderAgentStatus(b, d) {
  const provs = d.providers || []; const en = provs.filter(p => p.enabled);
  let h = '<div class="sg">';
  h += H.card('&#128994; Agent Status', '<div class="stat-xl" style="color:' + (en.length > 0 ? 'var(--green)' : 'var(--fg2)') + ';margin:8px 0">' + (en.length > 0 ? 'ACTIVE' : 'INACTIVE') + '</div>' + H.row('Enabled Providers', en.length + ' / ' + provs.length) + H.row('Total Queries', d.total_queries || 0));
  provs.forEach(p => { const ico = { Claude: '&#129302;', OpenAI: '&#9889;', Gemini: '&#128142;', Ollama: '&#128026;' }[p.name] || '&#9881;';
    h += H.card(ico + ' ' + p.name, '<div style="font-size:14px;font-weight:700;color:' + (p.enabled ? 'var(--green)' : 'var(--fg2)') + ';margin:4px 0">' + (p.enabled ? 'ONLINE' : 'OFFLINE') + '</div>' + H.row('Model', '<span class="text-xs">' + (p.model || '-') + '</span>') + H.row('API Key', '<span class="text-xs">' + (p.api_key && p.api_key !== '' ? 'Configured' : 'Not set') + '</span>')); });
  h += '</div>'; b.innerHTML = h;
}

function toggleKeyVis(i) { const el = document.getElementById('agk' + i); const bt = document.getElementById('agt' + i); if (el.type === 'password') { el.type = 'text'; bt.textContent = 'Hide'; } else { el.type = 'password'; bt.textContent = 'Show'; } }

async function testProvider(i, name) {
  const rEl = document.getElementById('agr' + i); const key = document.getElementById('agk' + i).value; const model = document.getElementById('agm' + i).value; const endpoint = document.getElementById('age' + i).value;
  if (!key || key.startsWith('***')) { rEl.innerHTML = '<span class="color-red">No API key set</span>'; return; }
  rEl.innerHTML = '<span class="spinner"></span> Testing...';
  try { let ok = false, detail = ''; const t0 = performance.now();
    if (name === 'Claude') { const r = await fetch(endpoint || 'https://api.anthropic.com/v1/messages', { method: 'POST', headers: { 'Content-Type': 'application/json', 'x-api-key': key, 'anthropic-version': '2023-06-01' }, body: JSON.stringify({ model: model || 'claude-sonnet-4-20250514', max_tokens: 1, messages: [{ role: 'user', content: 'ping' }] }) }); ok = r.ok || r.status === 400; detail = r.ok ? 'OK' : 'HTTP ' + r.status; }
    else if (name === 'OpenAI') { const r = await fetch((endpoint || 'https://api.openai.com/v1') + '/models', { headers: { Authorization: 'Bearer ' + key } }); ok = r.ok; detail = r.ok ? 'OK' : 'HTTP ' + r.status; }
    else if (name === 'Gemini') { const r = await fetch((endpoint || 'https://generativelanguage.googleapis.com/v1beta') + '/models?key=' + key); ok = r.ok; detail = r.ok ? 'OK' : 'HTTP ' + r.status; }
    else if (name === 'Ollama') { const r = await fetch((endpoint || 'http://localhost:11434') + '/api/tags'); ok = r.ok; detail = r.ok ? 'OK' : 'HTTP ' + r.status; }
    const ms = Math.round(performance.now() - t0);
    if (ok) { rEl.innerHTML = H.badge('Connected', 'g') + ' <span class="stat-label">' + ms + 'ms</span>'; }
    else { rEl.innerHTML = H.badge('Failed', 'r') + ' <span class="stat-label">' + detail + '</span>'; }
  } catch (e) { rEl.innerHTML = H.badge('Error', 'r') + ' <span class="text-xs color-red">' + escapeHtml(e.message) + '</span>'; }
}

async function testAllProviders() { const provNames = ['Claude', 'OpenAI', 'Gemini', 'Ollama']; for (let i = 0; i < provNames.length; i++) { const k = document.getElementById('agk' + i); if (k && k.value && !k.value.startsWith('***')) await testProvider(i, provNames[i]); } }

async function saveAgentConfig() {
  try { const provNames = ['Claude', 'OpenAI', 'Gemini', 'Ollama'];
    const providers = provNames.map((name, i) => { const enEl = document.getElementById('agen' + i); return { name, model: document.getElementById('agm' + i).value, api_key: document.getElementById('agk' + i).value, endpoint: document.getElementById('age' + i).value, enabled: enEl ? enEl.checked : undefined }; });
    const res = await fetch(EP.AGENT_CONFIG(), { method: 'PUT', headers: { Authorization: 'Bearer ' + authToken, 'Content-Type': 'application/json' }, body: JSON.stringify({ providers }) });
    if (!res.ok) throw new Error('HTTP ' + res.status);
    toast(t('agent.config_saved')); closeModal();
  } catch (e) { toast('Save failed: ' + e.message, false); }
}

async function saveAgentSettings() {
  try { const rate = parseInt(document.getElementById('ag-rate').value) || 300; const timeout = parseInt(document.getElementById('ag-timeout').value) || 10;
    const res = await fetch(EP.AGENT_CONFIG(), { method: 'PUT', headers: { Authorization: 'Bearer ' + authToken, 'Content-Type': 'application/json' }, body: JSON.stringify({ rate_limit_sec: rate, timeout_sec: timeout }) });
    if (!res.ok) throw new Error('HTTP ' + res.status);
    toast(t('agent.config_saved')); closeModal();
  } catch (e) { toast('Save failed: ' + e.message, false); }
}


async function renderApiPerf(b) {
  b.innerHTML = showSkeleton();
  var endpoints = ['/vms', '/containers', '/networks', '/storage/pools', '/health', '/alerts', '/processes'];
  var results = [];
  var h = H.section(_L('API 응답 시간', 'API Response Times'));

  for (var i = 0; i < endpoints.length; i++) {
    var ep = endpoints[i];
    var start = performance.now();
    try {
      await fetchGet(API_BASE + ep);
      var elapsed = Math.round(performance.now() - start);
      results.push({ endpoint: ep, time: elapsed, status: 'ok' });
    } catch (e) {
      var elapsed2 = Math.round(performance.now() - start);
      results.push({ endpoint: ep, time: elapsed2, status: 'error' });
    }
  }

  var avg = results.reduce(function(s, r) { return s + r.time; }, 0) / results.length;
  h += '<div class="sg grid-3">';
  h += H.card(_L('평균 응답', 'Average'), '<div class="stat-lg ' + (avg < 100 ? 'color-green' : avg < 500 ? 'color-yellow' : 'color-red') + '">' + avg.toFixed(0) + 'ms</div>');
  h += H.card(_L('최고', 'Fastest'), '<div class="stat-lg color-green">' + Math.min.apply(null, results.map(function(r){return r.time;})) + 'ms</div>');
  h += H.card(_L('최저', 'Slowest'), '<div class="stat-lg color-red">' + Math.max.apply(null, results.map(function(r){return r.time;})) + 'ms</div>');
  h += '</div>';

  h += '<table class="text-12 mt-12"><thead><tr><th>Endpoint</th><th>' + _L('응답 시간', 'Response') + '</th><th>' + _L('상태', 'Status') + '</th><th>' + _L('등급', 'Grade') + '</th></tr></thead><tbody>';
  results.sort(function(a, b) { return a.time - b.time; });
  results.forEach(function(r) {
    var grade = r.time < 50 ? 'A+' : r.time < 100 ? 'A' : r.time < 300 ? 'B' : r.time < 500 ? 'C' : 'D';
    var gradeColor = r.time < 100 ? 'g' : r.time < 300 ? 'y' : 'r';
    h += '<tr><td><code>' + esc(r.endpoint) + '</code></td><td><b>' + r.time + 'ms</b></td><td>' + H.badge(r.status, r.status === 'ok' ? 'g' : 'r') + '</td><td>' + H.badge(grade, gradeColor) + '</td></tr>';
  });
  h += '</tbody></table>';
  h += '<button class="btn mt-12" onclick="renderApiPerf(document.getElementById(\'cb\'))">&#128260; ' + _L('재측정', 'Re-run') + '</button>';
  h += '<button class="btn" onclick="runApiBenchmark()" style="margin-left:8px">&#9889; ' + _L('벤치마크', 'Benchmark') + ' (5x)</button>';
  b.innerHTML = h;
}
window.renderApiPerf = renderApiPerf;

async function runApiBenchmark() {
  var endpoints = ['/vms', '/containers', '/networks', '/storage/pools', '/health', '/alerts'];
  var iterations = 5;
  showModal('<h2>&#9889; ' + _L('벤치마크 실행 중', 'Running Benchmark') + '</h2><div class="prog-bar"><div class="prog-fill" id="bench-prog" class="w-pct-0"></div></div><div id="bench-st" class="prog-status"><span class="spinner"></span> 0/' + (endpoints.length * iterations) + '</div>');
  var results = {};
  var total = endpoints.length * iterations;
  var done = 0;
  for (var i = 0; i < endpoints.length; i++) {
    results[endpoints[i]] = [];
    for (var j = 0; j < iterations; j++) {
      var start = performance.now();
      try { await fetchGet(API_BASE + endpoints[i]); } catch(e) {}
      results[endpoints[i]].push(Math.round(performance.now() - start));
      done++;
      var pf = document.getElementById('bench-prog');
      var ps = document.getElementById('bench-st');
      if (pf) pf.style.width = (done / total * 100) + '%';
      if (ps) ps.innerHTML = '<span class="spinner"></span> ' + done + '/' + total + ' — ' + endpoints[i];
    }
  }
  var h = '<h2>&#9889; ' + _L('벤치마크 결과', 'Benchmark Results') + '</h2>';
  h += '<table class="text-12"><thead><tr><th>Endpoint</th><th>Avg</th><th>Min</th><th>Max</th><th>P95</th></tr></thead><tbody>';
  Object.keys(results).forEach(function(ep) {
    var times = results[ep].sort(function(a,b){return a-b;});
    var avg = Math.round(times.reduce(function(s,t2){return s+t2;},0) / times.length);
    var p95 = times[Math.floor(times.length * 0.95)] || times[times.length-1];
    h += '<tr><td><code>' + esc(ep) + '</code></td><td><b>' + avg + 'ms</b></td><td class="color-green">' + times[0] + 'ms</td><td class="color-red">' + times[times.length-1] + 'ms</td><td>' + p95 + 'ms</td></tr>';
  });
  h += '</tbody></table><div class="text-right mt-12"><button class="btn" onclick="closeModal()">' + t('btn.close') + '</button></div>';
  var mc = document.getElementById('mc');
  if (mc) mc.innerHTML = h;
}
window.runApiBenchmark = runApiBenchmark;


function renderActivityLog(b) {
  var log = (eventLog || []).filter(function(e) { return e && e.msg; });
  var h = H.section(_L('API 활동 로그', 'API Activity Log'));
  h += '<div class="flex gap-6 mb-8"><span class="color-muted text-xs">' + log.length + ' ' + _L('건', 'requests') + '</span><button class="btn btn-xs" onclick="window.eventLog=[];renderActivityLog(document.getElementById(\'cb\'))">Clear</button></div>';
  if (log.length === 0) {
    h += '<div class="empty-state p-20 text-center"><div style="font-size:36px;opacity:.5">&#128196;</div><div class="color-muted">' + _L('기록 없음', 'No activity yet') + '</div></div>';
  } else {
    h += '<table class="text-11"><thead><tr><th>' + _L('시각', 'Time') + '</th><th>' + _L('이벤트', 'Event') + '</th></tr></thead><tbody>';
    log.slice().reverse().forEach(function(l) {
      var timeStr = l.ts ? new Date(l.ts).toLocaleTimeString() : '';
      var msg = l.msg || String(l);
      var isApi = msg.includes('API') || msg.includes('fetch') || msg.includes('GET') || msg.includes('POST') || msg.includes('Auth') || msg.includes('WS');
      h += '<tr><td class="color-muted">' + esc(timeStr) + '</td><td>' + (isApi ? '<span class="color-accent">' : '<span>') + esc(msg) + '</span></td></tr>';
    });
    h += '</tbody></table>';
  }
  b.innerHTML = h;
}
window.renderActivityLog = renderActivityLog;


async function renderSessions(b) {
  b.innerHTML = showSkeleton();
  var h = H.section(_L('세션 관리', 'Session Management'));
  h += '<div class="mb-8">' + _L('활성 JWT 세션을 관리합니다.', 'Manage active JWT sessions.') + '</div>';
  h += '<div class="flex gap-8 mb-12">';
  h += '<input id="revoke-jti" placeholder="' + _L('세션 JTI 입력', 'Enter session JTI') + '" class="input-field flex-1">';
  h += '<button class="btn btn-r" onclick="revokeSession()" aria-label="' + _L('세션 강제 해제', 'Revoke session') + '">' + _L('강제 해제', 'Force Logout') + '</button>';
  h += '</div>';
  h += '<div class="empty-state p-20 text-center">';
  h += '<div style="font-size:36px;opacity:.5">&#128274;</div>';
  h += '<div class="color-muted">' + _L('JTI를 입력하여 특정 세션을 무효화합니다', 'Enter JTI to revoke a specific session') + '</div></div>';
  b.innerHTML = h;
}
async function revokeSession() {
  var jti = document.getElementById('revoke-jti');
  if (!jti || !jti.value.trim()) { toast(_L('JTI를 입력하세요', 'Enter JTI'), 'w'); return; }
  if (!await customConfirm(_L('이 세션을 강제 해제하시겠습니까?', 'Force logout this session?'))) return;
  try {
    await fetchPost(EP.AUTH_SESSION_REVOKE(), { jti: jti.value.trim() });
    toast(_L('세션이 해제되었습니다', 'Session revoked'), 's');
    jti.value = '';
  } catch(e) { toast(_L('실패', 'Failed') + ': ' + (e.message || ''), 'e'); }
}


async function renderApiKeys(b) {
  b.innerHTML = showSkeleton();
  try {
    var r = await fetchGet(EP.AUTH_APIKEY_LIST());
    var list = unwrapList(r);
    var h = H.section(_L('API 키 관리', 'API Key Management'));
    h += '<button class="btn mb-8" onclick="showApiKeyCreate()" aria-label="' + _L('새 API 키 생성', 'Create new API key') + '">+ ' + _L('새 키 생성', 'New Key') + '</button>';
    if (list.length === 0) {
      h += '<div class="empty-state p-20 text-center"><div style="font-size:36px;opacity:.5">&#128273;</div>';
      h += '<div class="color-muted">' + _L('등록된 API 키가 없습니다', 'No API keys registered') + '</div></div>';
    } else {
      h += '<table class="data-table text-11"><thead><tr>';
      h += '<th>' + _L('클라이언트', 'Client') + '</th><th>' + _L('역할', 'Role') + '</th>';
      h += '<th>' + _L('생성일', 'Created') + '</th><th>' + _L('최종 사용', 'Last Used') + '</th>';
      h += '<th>' + _L('상태', 'Status') + '</th><th></th></tr></thead><tbody>';
      list.forEach(function(k) {
        var st = k.revoked ? '<span class="badge badge-r">' + _L('폐기', 'Revoked') + '</span>'
                           : '<span class="badge badge-g">' + _L('활성', 'Active') + '</span>';
        h += '<tr><td><b>' + esc(k.client_name) + '</b></td>';
        h += '<td>' + esc(['viewer','operator','admin'][k.role] || '?') + '</td>';
        h += '<td class="color-muted">' + esc(k.created_at || '') + '</td>';
        h += '<td class="color-muted">' + esc(k.last_used_at || _L('미사용', 'Never')) + '</td>';
        h += '<td>' + st + '</td>';
        h += '<td>' + (k.revoked ? '' : '<button class="btn btn-r btn-xxs" onclick="revokeApiKey(\'' + esc(k.client_name) + '\')" aria-label="' + _L('키 폐기', 'Revoke key') + '">' + _L('폐기', 'Revoke') + '</button>') + '</td></tr>';
      });
      h += '</tbody></table>';
    }
    b.innerHTML = h;
  } catch(e) { b.innerHTML = '<p class="color-muted">' + _L('로드 실패', 'Failed') + '</p>'; }
}
async function showApiKeyCreate() {
  var html = '<div class="form-group"><label>' + _L('클라이언트 이름', 'Client Name') + '</label>';
  html += '<input id="ak-name" class="input-field" placeholder="grafana-scraper"></div>';
  html += '<div class="form-group"><label>' + _L('역할', 'Role') + '</label>';
  html += '<select id="ak-role" class="input-field"><option value="0">viewer</option><option value="1" selected>operator</option><option value="2">admin</option></select></div>';
  showModal(_L('API 키 생성', 'Create API Key'), html, async function() {
    var name = document.getElementById('ak-name').value.trim();
    var role = parseInt(document.getElementById('ak-role').value);
    if (!name) { toast(_L('이름 필수', 'Name required'), 'w'); return; }
    try {
      var r = await fetchPost(EP.AUTH_APIKEY_CREATE(), { client_name: name, role: role });
      var data = unwrapData(r);
      toast(_L('키 생성 완료', 'Key created') + ' — ' + _L('복사하세요', 'Copy it now'), 's');
      showModal(_L('API 키', 'API Key'), '<div class="code-block break-all text-12">' + esc(data.api_key) + '</div><p class="color-muted text-xs">' + _L('이 키는 다시 표시되지 않습니다', 'This key will not be shown again') + '</p>');
    } catch(e) { toast(_L('실패', 'Failed') + ': ' + (e.message || ''), 'e'); }
  });
}
async function revokeApiKey(name) {
  if (!await customConfirm(_L('이 API 키를 폐기하시겠습니까?', 'Revoke this API key?') + '\n' + name)) return;
  try {
    await fetchPost(EP.AUTH_APIKEY_REVOKE(name), {});
    toast(_L('키 폐기 완료', 'Key revoked'), 's');
    renderApiKeys(document.getElementById('cb'));
  } catch(e) { toast(_L('실패', 'Failed'), 'e'); }
}


window.agentTab = agentTab;
window.renderAccounts = renderAccounts;
window.renderSessions = renderSessions;
window.revokeSession = revokeSession;
window.renderApiKeys = renderApiKeys;
window.showApiKeyCreate = showApiKeyCreate;
window.revokeApiKey = revokeApiKey;
window.acctCreate = acctCreate;
window.acctRole = acctRole;
window.acctDel = acctDel;
window.renderApiManagement = renderApiManagement;
window.apiMgmtGetToken = apiMgmtGetToken;
window.apiMgmtTestHealth = apiMgmtTestHealth;
window.apiMgmtSend = apiMgmtSend;
window.showAgentConfig = showAgentConfig;
window.renderAgentTab = renderAgentTab;
window.renderAgentProviders = renderAgentProviders;
window.renderAgentSettings = renderAgentSettings;
window.renderAgentHistory = renderAgentHistory;
window.renderAgentStatus = renderAgentStatus;
window.toggleKeyVis = toggleKeyVis;
window.testProvider = testProvider;
window.testAllProviders = testAllProviders;
window.saveAgentConfig = saveAgentConfig;
window.saveAgentSettings = saveAgentSettings;


PCV.accounts = {
  renderAccounts: renderAccounts,
  acctCreate: acctCreate,
  acctRole: acctRole,
  acctDel: acctDel,
  renderApiManagement: renderApiManagement,
  apiMgmtGetToken: apiMgmtGetToken,
  apiMgmtTestHealth: apiMgmtTestHealth,
  apiMgmtSend: apiMgmtSend,
  showAgentConfig: showAgentConfig,
  renderAgentTab: renderAgentTab,
  renderAgentProviders: renderAgentProviders,
  renderAgentSettings: renderAgentSettings,
  renderAgentHistory: renderAgentHistory,
  renderAgentStatus: renderAgentStatus,
  toggleKeyVis: toggleKeyVis,
  testProvider: testProvider,
  testAllProviders: testAllProviders,
  saveAgentConfig: saveAgentConfig,
  saveAgentSettings: saveAgentSettings,
  renderApiPerf: renderApiPerf,
  runApiBenchmark: runApiBenchmark,
  renderActivityLog: renderActivityLog,
  renderSessions: renderSessions,
  revokeSession: revokeSession,
  renderApiKeys: renderApiKeys,
  showApiKeyCreate: showApiKeyCreate,
  revokeApiKey: revokeApiKey
};

})(window.PCV);












window.PCV = window.PCV || {};
(function(PCV) {


async function renderTemplates(b) {
  b.innerHTML = showSkeleton();
  try {
    const r = await fetchGet(EP.TEMPLATES());
    const list = unwrapList(r);
    let h = H.section('&#128195; VM Templates <button class="btn btn-g" onclick="showTemplateCreate()" style="margin-left:8px">+ Create Template</button> <button class="btn" onclick="loadTemplateHistory()" style="margin-left:4px">&#128203; History</button>');
    if (list.length === 0) { h += '<div class="empty-state" style="text-align:center;padding:40px 20px"><div style="font-size:48px;margin-bottom:12px;opacity:.5">&#128195;</div><div style="font-size:14px;color:var(--fg2);margin-bottom:16px">No templates found</div><button class="btn btn-g" onclick="showTemplateCreate()" class="text-12">+ Create Template</button></div>'; b.innerHTML = h; return; }
    h += '<table><thead><tr><th>Name</th><th>vCPU</th><th>Memory</th><th>Disk</th><th>OS</th><th>Actions</th></tr></thead><tbody>';
    list.forEach(t2 => {
      h += '<tr><td><b>' + escapeHtml(t2.name || '-') + '</b></td><td>' + (t2.vcpu || '-') + '</td><td>' + (t2.memory_mb || '-') + ' MB</td><td>' + (t2.disk_gb || '-') + ' GB</td><td>' + escapeHtml(t2.os_variant || '-') + '</td><td><button class="btn" style="font-size:10px;padding:3px 8px" onclick="templateUse(\'' + escapeHtml(t2.name) + '\')">Use</button> <button class="btn btn-r" style="font-size:10px;padding:3px 8px" onclick="templateDel(\'' + escapeHtml(t2.name) + '\')">' + t('btn.delete') + '</button></td></tr>';
    });
    h += '</tbody></table>';
    h += '<div id="tpl-history"></div>';
    b.innerHTML = h;
  } catch (e) { b.innerHTML = '<p class="color-red">' + escapeHtml(e.message) + '</p>'; }
}

function showTemplateCreate() {
  showModal('<h2>Create VM Template</h2><div class="fr"><label>Name</label><input id="tpl-name" placeholder="web-small"></div><div class="fr"><label>vCPU</label><input id="tpl-vcpu" type="number" value="2"></div><div class="fr"><label>Memory (MB)</label><input id="tpl-mem" type="number" value="2048"></div><div class="fr"><label>Disk (GB)</label><input id="tpl-disk" type="number" value="20"></div><div class="fr"><label>OS Variant</label><input id="tpl-os" value="ubuntu24.04"></div><div class="text-right mt-12"><button class="btn btn-g" onclick="doTemplateCreate()">' + t('btn.create') + '</button> <button class="btn btn-r" onclick="closeModal()">' + t('btn.cancel') + '</button></div>');
}

async function doTemplateCreate() {
  const name = document.getElementById('tpl-name')?.value;
  if (!name) { toast('Template name required', false); return; }
  try {
    const r = await fetchPost(EP.TEMPLATES(), { name, vcpu: +(document.getElementById('tpl-vcpu')?.value || 2), memory_mb: +(document.getElementById('tpl-mem')?.value || 2048), disk_gb: +(document.getElementById('tpl-disk')?.value || 20), os_variant: document.getElementById('tpl-os')?.value || '' });
    if (r.error) { toast(r.error.message || 'Failed', false); return; }
    toast('Template created: ' + name); addEvt('Template: ' + name); closeModal(); renderTemplates(document.getElementById('cb'));
  } catch (e) { toast(e.message, false); }
}

async function templateUse(name) {
  try { const r = await fetchGet(EP.TEMPLATE(name)); const d = unwrapData(r);
    wizData = { name: '', vcpu: d.vcpu || 2, mem: d.memory_mb || 2048, disk: d.disk_gb || 20, iso: '', bridge: 'pcvbr0' };
    wizStep = 1; renderWiz(); toast('Template loaded: ' + name);
  } catch (e) { toast(e.message, false); }
}

async function templateDel(name) {
  if (!await customConfirm(t('btn.delete'), 'Template: ' + name + '?')) return;
  try { await fetchDelete(EP.TEMPLATE(name)); toast('Template deleted'); renderTemplates(document.getElementById('cb')); } catch (e) { toast(e.message, false); }
}

async function loadTemplateHistory() {
  try {
    const r = await fetchGet(EP.TEMPLATE_HISTORY());
    const list = unwrapList(r);
    let h = '<h2>&#128203; Template History</h2>';
    if (list.length === 0) { h += '<p class="color-muted">No template changes recorded</p>'; }
    else {
      h += '<table><thead><tr><th>Timestamp</th><th>Action</th><th>Template</th><th>User</th></tr></thead><tbody>';
      list.forEach(e => { h += '<tr><td class="text-xs">' + escapeHtml(e.timestamp || '-') + '</td><td>' + H.badge(e.action || '-', e.action === 'create' ? 'g' : e.action === 'delete' ? 'r' : 'y') + '</td><td>' + escapeHtml(e.template || e.name || '-') + '</td><td>' + escapeHtml(e.user || '-') + '</td></tr>'; });
      h += '</tbody></table>';
    }
    h += '<div class="text-right mt-12"><button class="btn btn-r" onclick="closeModal()">' + t('btn.close') + '</button></div>';
    showModal(h);
  } catch (e) { toast('Template history error: ' + e.message, false); }
}


async function renderDocker(b) {
  b.innerHTML = showSkeleton();
  try {
    const r = await fetchGet(EP.DOCKER_LIST());
    const list = unwrapList(r);
    let h = H.section('&#128051; Docker/OCI Containers');
    h += '<div class="flex gap-6 mb-14"><button class="btn btn-g" onclick="showDockerPull()">&#128229; Pull Image</button><button class="btn btn-g" onclick="showDockerRun()">&#9654; Run Container</button></div>';
    if (list.length === 0) { h += '<div class="empty-state" style="text-align:center;padding:40px 20px"><div style="font-size:48px;margin-bottom:12px;opacity:.5">&#128051;</div><div style="font-size:14px;color:var(--fg2);margin-bottom:16px">No Docker containers running</div><button class="btn btn-g" onclick="showDockerPull()" class="text-12">Pull Image</button></div>'; b.innerHTML = h; return; }
    h += '<table><thead><tr><th>Name/ID</th><th>Image</th><th>State</th><th>Ports</th><th>Actions</th></tr></thead><tbody>';
    list.forEach(c => {
      h += '<tr><td><b>' + escapeHtml(c.name || c.id || '-') + '</b></td><td>' + escapeHtml(c.image || '-') + '</td><td>' + H.badge(c.state || c.status || '-', (c.state || '').toLowerCase() === 'running' ? 'g' : 'r') + '</td><td class="text-xs">' + escapeHtml(c.ports || '-') + '</td><td><button class="btn btn-r" style="font-size:10px;padding:3px 8px" onclick="dockerStop(\'' + escapeHtml(c.name || c.id || '') + '\')">Stop</button></td></tr>';
    });
    h += '</tbody></table>';
    b.innerHTML = h;
  } catch (e) { b.innerHTML = H.section('&#128051; Docker/OCI Containers') + '<div class="flex gap-6 mb-14"><button class="btn btn-g" onclick="showDockerPull()">&#128229; Pull Image</button><button class="btn btn-g" onclick="showDockerRun()">&#9654; Run Container</button></div><p class="color-muted">Docker/OCI backend not available. Containers will appear here when docker.list RPC is implemented.</p>'; }
}

function showDockerPull() {
  showModal('<h2>&#128229; Pull OCI Image</h2><div class="fr"><label>Image</label><input id="dk-image" placeholder="nginx:latest" class="flex-1"></div><div class="text-right mt-12"><button class="btn btn-g" onclick="doDockerPull()">Pull</button> <button class="btn btn-r" onclick="closeModal()">' + t('btn.cancel') + '</button></div>');
}

async function doDockerPull() {
  const img = document.getElementById('dk-image')?.value;
  if (!img) { toast('Image name required', false); return; }
  toast('Pulling ' + img + '...');
  try { const r = await fetchPost(EP.DOCKER_PULL(), { image: img });
    if (r.error) { toast('Pull failed: ' + (r.error.message || ''), false); return; }
    toast('Image pulled: ' + img); addEvt('Docker pull: ' + img); closeModal();
  } catch (e) { toast(e.message, false); }
}

function showDockerRun() {
  showModal('<h2>&#9654; Run OCI Container</h2><div class="fr"><label>Image</label><input id="dkr-image" placeholder="nginx:latest" class="flex-1"></div><div class="fr"><label>Name</label><input id="dkr-name" placeholder="my-container"></div><div class="fr"><label>Ports</label><input id="dkr-ports" placeholder="8080:80"></div><div class="fr"><label>Environment</label><input id="dkr-env" placeholder="KEY=VAL,KEY2=VAL2"></div><div class="text-right mt-12"><button class="btn btn-g" onclick="doDockerRun()">Run</button> <button class="btn btn-r" onclick="closeModal()">' + t('btn.cancel') + '</button></div>');
}

async function doDockerRun() {
  const img = document.getElementById('dkr-image')?.value;
  if (!img) { toast('Image required', false); return; }
  try { const r = await fetchPost(EP.DOCKER_RUN(), { image: img, name: document.getElementById('dkr-name')?.value || '', ports: document.getElementById('dkr-ports')?.value || '', env: document.getElementById('dkr-env')?.value || '' });
    if (r.error) { toast('Run failed: ' + (r.error.message || ''), false); return; }
    toast('Container started: ' + img); addEvt('Docker run: ' + img); closeModal(); renderDocker(document.getElementById('cb'));
  } catch (e) { toast(e.message, false); }
}

async function dockerStop(name) {
  if (!await customConfirm('Stop Container', name + '?')) return;
  try { const r = await fetchPost(EP.DOCKER_STOP(name), {});
    if (r.error) { toast('Stop failed: ' + (r.error.message || ''), false); return; }
    toast('Container stopped: ' + name); renderDocker(document.getElementById('cb'));
  } catch (e) { toast(e.message, false); }
}


async function renderTerraform(b) {
  b.innerHTML = showSkeleton();
  let h = H.section('&#127981; Terraform IaC Integration');
  h += '<div class="sg grid-2 mb-14">';
  h += H.card('&#128203; Terraform Plan', '<p class="stat-label mb-8">Preview infrastructure changes before applying.</p><div class="fr"><label>Config (HCL/JSON)</label><textarea id="tf-config" placeholder="resource \\"purecvisor_vm\\" \\"web\\" {\\n  name = \\"web-01\\"\\n  vcpu = 2\\n}" style="width:100%;min-height:100px;background:var(--bg);border:1px solid var(--border);color:var(--fg);border-radius:4px;padding:8px;font-family:monospace;font-size:11px"></textarea></div><div class="flex gap-6 mt-8"><button class="btn" onclick="tfPlan()">&#128203; Plan</button><button class="btn btn-g" onclick="tfApply()">&#9989; Apply</button></div><div id="tf-plan-result" class="mt-8"></div>');
  h += H.card('&#128202; Terraform State', '<div id="tf-state"><span class="spinner"></span> Loading state...</div>');
  h += '</div>';
  b.innerHTML = h;
  setTimeout(loadTfState, 50);
}

async function tfPlan() {
  const el = document.getElementById('tf-plan-result'); if (!el) return;
  const config = document.getElementById('tf-config')?.value;
  el.innerHTML = '<span class="spinner"></span> Planning...';
  try { const r = await fetchPost(EP.TERRAFORM_PLAN(), { config: config || '' });
    const d = unwrapData(r);
    el.innerHTML = '<pre style="background:var(--bg);padding:8px;border-radius:4px;font-size:11px;max-height:200px;overflow-y:auto;color:var(--green);white-space:pre-wrap">' + escapeHtml(d.plan || d.output || JSON.stringify(d, null, 2)) + '</pre>';
  } catch (e) { el.innerHTML = '<span class="color-red">Plan error: ' + escapeHtml(e.message) + '</span>'; }
}

async function tfApply() {
  if (!await customConfirm('Terraform Apply', 'Apply infrastructure changes?')) return;
  const el = document.getElementById('tf-plan-result'); if (el) el.innerHTML = '<span class="spinner"></span> Applying...';
  const config = document.getElementById('tf-config')?.value;
  try { const r = await fetchPost(EP.TERRAFORM_APPLY(), { config: config || '' });
    const d = unwrapData(r);
    if (el) el.innerHTML = '<pre style="background:var(--bg);padding:8px;border-radius:4px;font-size:11px;max-height:200px;overflow-y:auto;color:var(--accent);white-space:pre-wrap">' + escapeHtml(d.output || JSON.stringify(d, null, 2)) + '</pre>';
    toast('Terraform apply complete'); addEvt('Terraform apply');
  } catch (e) { if (el) el.innerHTML = '<span class="color-red">Apply error: ' + escapeHtml(e.message) + '</span>'; }
}

async function loadTfState() {
  const el = document.getElementById('tf-state'); if (!el) return;
  try { const r = await fetchGet(EP.TERRAFORM_STATE());
    const d = unwrapData(r);
    const resources = d.resources || d.state || [];
    if (Array.isArray(resources) && resources.length > 0) {
      let h = '<table class="text-11"><thead><tr><th>Type</th><th>Name</th><th>Status</th></tr></thead><tbody>';
      resources.forEach(res => { h += '<tr><td>' + escapeHtml(res.type || '-') + '</td><td>' + escapeHtml(res.name || '-') + '</td><td>' + H.badge(res.status || 'managed', 'g') + '</td></tr>'; });
      h += '</tbody></table>'; el.innerHTML = h;
    } else { el.innerHTML = '<p class="color-muted text-12">No Terraform state. Use Plan + Apply to manage infrastructure as code.</p>'; }
  } catch (e) { el.innerHTML = '<p class="color-muted text-12">Terraform state not available. Configure terraform.* RPC handlers to enable IaC.</p>'; }
}


async function configBackup() {
  toast('Backing up configuration...');
  try {
    const r = await fetchPost(EP.CONFIG_BACKUP(), {});
    if (r.error) { toast('Backup failed: ' + (r.error.message || ''), false); return; }
    toast('Config backup created'); addEvt('Config backup');
  } catch (e) { toast(e.message, false); }
}

async function configHistory() {
  try {
    const r = await fetchGet(EP.CONFIG_HISTORY());
    const list = unwrapList(r);
    let h = '<h2>&#128203; Configuration History</h2>';
    if (list.length === 0) { h += '<p class="color-muted">No configuration changes recorded</p>'; }
    else {
      h += '<table><thead><tr><th>Timestamp</th><th>Key</th><th>Old Value</th><th>New Value</th><th>User</th></tr></thead><tbody>';
      list.forEach(e => { h += '<tr><td class="text-xs">' + escapeHtml(e.timestamp || e.time || '-') + '</td><td>' + escapeHtml(e.key || e.param || '-') + '</td><td>' + escapeHtml(e.old_value || '-') + '</td><td class="color-accent">' + escapeHtml(e.new_value || e.value || '-') + '</td><td>' + escapeHtml(e.user || '-') + '</td></tr>'; });
      h += '</tbody></table>';
    }
    h += '<div class="text-right mt-12"><button class="btn btn-r" onclick="closeModal()">' + t('btn.close') + '</button></div>';
    showModal(h);
  } catch (e) { toast('Config history error: ' + e.message, false); }
}

async function renderConfigMgmt(b) {
  b.innerHTML = showSkeleton();
  var cfg = {};
  try { var r = await fetchGet(EP.CONFIG_DAEMON()); cfg = unwrapData(r) || r || {}; } catch(e) {}
  var stg = cfg.storage || {};
  var ctr = cfg.container || {};

  var h = H.section('&#9881; Configuration Management');


  h += '<h3 style="margin:16px 0 10px">&#128190; ' + _L('스토리지 풀 설정', 'Storage Pool Settings') + '</h3>';
  h += '<div class="sg grid-2 mb-14">';
  h += H.card('&#128190; VM Storage', ''
    + '<p class="stat-label" style="margin-bottom:10px;line-height:1.6">'
    + _L('ZFS Pool: ZFS 데이터셋 이름 (예: pcvpool/vms) — zvol 블록 디바이스로 VM 디스크 생성<br>'
       + 'Image Dir: ZFS 미사용 시 qcow2 파일 저장 경로 (예: /var/lib/libvirt/images)<br>'
       + 'ISO Dirs: ISO/IMG 파일 탐색 경로 (콤마 구분)',
         'ZFS Pool: ZFS dataset name (e.g. pcvpool/vms) — creates zvol block devices<br>'
       + 'Image Dir: qcow2 fallback path for non-ZFS (e.g. /var/lib/libvirt/images)<br>'
       + 'ISO Dirs: ISO/IMG scan paths (comma separated)')
    + '</p>'
    + '<div class="fr"><label style="min-width:140px">VM ZFS Pool</label><input id="cfg-zvol" value="' + escapeHtml(stg.zvol_pool || 'pcvpool/vms') + '" placeholder="pcvpool/vms" class="flex-1"></div>'
    + '<div class="fr"><label style="min-width:140px">Image Dir (qcow2)</label><input id="cfg-imgdir" value="' + escapeHtml(stg.image_dir || '/var/lib/libvirt/images') + '" placeholder="/var/lib/libvirt/images" class="flex-1"></div>'
    + '<div class="fr"><label style="min-width:140px">ISO Dirs</label><input id="cfg-iso" value="' + escapeHtml(stg.iso_dirs || '') + '" placeholder="/pcvpool/iso,/iso" class="flex-1"></div>'
    + '<button class="btn btn-g mt-8" onclick="saveStorageCfg(\'vm\')">&#128190; ' + _L('저장', 'Save') + '</button>'
    + '<div id="cfg-vm-result" style="margin-top:6px;font-size:11px"></div>');
  h += H.card('&#9783; Container Storage', ''
    + '<p class="stat-label" style="margin-bottom:10px;line-height:1.6">'
    + _L('ZFS Pool: 컨테이너 ZFS 데이터셋 (예: pcvpool/containers)<br>'
       + 'LXC Path: 컨테이너 설정/rootfs 저장 경로',
         'ZFS Pool: Container ZFS dataset (e.g. pcvpool/containers)<br>'
       + 'LXC Path: Container config/rootfs storage path')
    + '</p>'
    + '<div class="fr"><label style="min-width:140px">Container ZFS Pool</label><input id="cfg-ctrpool" value="' + escapeHtml(stg.container_pool || 'pcvpool/containers') + '" placeholder="pcvpool/containers" class="flex-1"></div>'
    + '<div class="fr"><label style="min-width:140px">LXC Path</label><input id="cfg-lxcpath" value="' + escapeHtml(ctr.lxc_path || '/var/lib/purecvisor/lxc') + '" placeholder="/var/lib/purecvisor/lxc" class="flex-1"></div>'
    + '<button class="btn btn-g mt-8" onclick="saveStorageCfg(\'ctr\')">&#128190; ' + _L('저장', 'Save') + '</button>'
    + '<div id="cfg-ctr-result" style="margin-top:6px;font-size:11px"></div>');
  h += '</div>';


  h += '<h3 style="margin:16px 0 10px">&#128203; ' + _L('설정 관리', 'Config Management') + '</h3>';
  h += '<div class="sg grid-2 mb-14">';
  h += H.card('&#128190; Config Backup', '<p class="stat-label mb-8">' + _L('현재 daemon.conf를 백업합니다.', 'Create a backup of current daemon.conf.') + '</p><button class="btn btn-g" onclick="configBackup()">&#128190; Create Backup</button><div id="cfg-backup-result" class="mt-8"></div>');
  h += H.card('&#128203; Config History', '<div id="cfg-history"><span class="spinner"></span> Loading...</div>');
  h += '</div>';
  b.innerHTML = h;
  setTimeout(loadConfigHistoryInline, 50);
}

async function saveStorageCfg(type) {
  var pairs = [];
  var resultEl;
  if (type === 'vm') {
    resultEl = document.getElementById('cfg-vm-result');

    var zvolVal = (document.getElementById('cfg-zvol')?.value || '').trim();
    if (zvolVal.startsWith('/')) {
      if (resultEl) resultEl.innerHTML = '<span class="color-red">&#9888; ' + _L(
        'ZFS Pool은 파일 경로가 아닌 ZFS 데이터셋 이름이어야 합니다 (예: pcvpool/vms).<br>파일시스템 경로에 저장하려면 Image Dir 필드를 사용하세요.',
        'ZFS Pool must be a ZFS dataset name (e.g. pcvpool/vms), not a path.<br>Use Image Dir for filesystem paths.') + '</span>';
      return;
    }
    pairs = [
      { section: 'storage', key: 'zvol_pool', value: zvolVal },
      { section: 'storage', key: 'image_dir', value: document.getElementById('cfg-imgdir')?.value },
      { section: 'storage', key: 'iso_dirs', value: document.getElementById('cfg-iso')?.value }
    ];
  } else {
    resultEl = document.getElementById('cfg-ctr-result');
    var ctrPoolVal = (document.getElementById('cfg-ctrpool')?.value || '').trim();
    if (ctrPoolVal.startsWith('/')) {
      if (resultEl) resultEl.innerHTML = '<span class="color-red">&#9888; ' + _L(
        'Container ZFS Pool은 ZFS 데이터셋 이름이어야 합니다 (예: pcvpool/containers).',
        'Container ZFS Pool must be a ZFS dataset name (e.g. pcvpool/containers).') + '</span>';
      return;
    }
    pairs = [
      { section: 'storage', key: 'container_pool', value: ctrPoolVal },
      { section: 'container', key: 'lxc_path', value: document.getElementById('cfg-lxcpath')?.value }
    ];
  }
  if (resultEl) resultEl.innerHTML = '<span class="spinner"></span> ' + _L('저장 중...', 'Saving...');
  try {
    for (var i = 0; i < pairs.length; i++) {
      if (pairs[i].value) await fetchPut(EP.CONFIG_DAEMON(), pairs[i]);
    }
    if (resultEl) resultEl.innerHTML = '<span class="color-green">&#9989; ' + _L('저장 완료 (재시작 시 적용)', 'Saved (restart to apply)') + '</span>';
    toast(_L('스토리지 설정 저장됨', 'Storage config saved'));
  } catch(e) {
    if (resultEl) resultEl.innerHTML = '<span class="color-red">&#10060; ' + escapeHtml(e.message) + '</span>';
    toast(e.message, false);
  }
}

async function loadConfigHistoryInline() {
  const el = document.getElementById('cfg-history'); if (!el) return;
  try {
    const r = await fetchGet(EP.CONFIG_HISTORY());
    const list = unwrapList(r);
    if (list.length === 0) { el.innerHTML = '<p class="color-muted text-12">No configuration changes recorded.</p>'; return; }
    let h = '<table class="text-11"><thead><tr><th>Time</th><th>Key</th><th>Value</th></tr></thead><tbody>';
    list.slice(0, 20).forEach(e => { h += '<tr><td class="text-xs">' + escapeHtml(e.timestamp || e.time || '-') + '</td><td>' + escapeHtml(e.key || '-') + '</td><td class="color-accent">' + escapeHtml(e.new_value || e.value || '-') + '</td></tr>'; });
    h += '</tbody></table>';
    if (list.length > 20) h += '<p class="stat-label">Showing 20 of ' + list.length + ' entries</p>';
    el.innerHTML = h;
  } catch (e) { el.innerHTML = '<p class="color-muted text-12">Config history not available.</p>'; }
}


function showImportOva() {
  showModal('<h2>&#128230; Import OVA</h2><div class="fr"><label>OVA Path</label><input id="ova-path" placeholder="/path/to/vm.ova" class="flex-1"></div><div class="fr"><label>VM Name</label><input id="ova-name" placeholder="imported-vm"></div><div class="fr"><label>Pool</label><input id="ova-pool" value="pcvpool/vms"></div><div class="text-right mt-12"><button class="btn btn-g" onclick="doImportOva()">Import</button> <button class="btn btn-r" onclick="closeModal()">' + t('btn.cancel') + '</button></div>');
}

async function doImportOva() {
  const path = document.getElementById('ova-path')?.value;
  const name = document.getElementById('ova-name')?.value;
  if (!path) { toast('OVA path required', false); return; }
  toast('Importing OVA...');
  try {
    const r = await fetchPost(EP.OVA_IMPORT(), { ova_path: path, name: name || '', pool: document.getElementById('ova-pool')?.value || '' });
    if (r.error) { toast('Import failed: ' + (r.error.message || ''), false); return; }
    toast('OVA import started' + (name ? ': ' + name : '')); addEvt('OVA import: ' + (name || path));
    closeModal(); setTimeout(loadAll, 3000);
  } catch (e) { toast(e.message, false); }
}


window.renderTemplates = renderTemplates;
window.showTemplateCreate = showTemplateCreate;
window.doTemplateCreate = doTemplateCreate;
window.templateUse = templateUse;
window.templateDel = templateDel;
window.loadTemplateHistory = loadTemplateHistory;
window.renderDocker = renderDocker;
window.showDockerPull = showDockerPull;
window.doDockerPull = doDockerPull;
window.showDockerRun = showDockerRun;
window.doDockerRun = doDockerRun;
window.dockerStop = dockerStop;
window.renderTerraform = renderTerraform;
window.tfPlan = tfPlan;
window.tfApply = tfApply;
window.loadTfState = loadTfState;
window.configBackup = configBackup;
window.configHistory = configHistory;
window.renderConfigMgmt = renderConfigMgmt;
window.loadConfigHistoryInline = loadConfigHistoryInline;
window.saveStorageCfg = saveStorageCfg;
window.showImportOva = showImportOva;
window.doImportOva = doImportOva;


async function doConfigReload() {
  if (!await customConfirm(_L('데몬 설정을 리로드하시겠습니까?\n(webhook, rate limit, alert 임계값 등이 갱신됩니다)',
      'Reload daemon configuration?\n(webhook, rate limit, alert thresholds will be refreshed)'))) return;
  try {
    await fetchPost(EP.CONFIG_RELOAD(), {});
    toast(_L('설정 리로드 완료', 'Configuration reloaded'), 's');
  } catch(e) { toast(_L('리로드 실패', 'Reload failed') + ': ' + (e.message || ''), 'e'); }
}


async function showBackupVerify() {
  var html = '<div class="form-group"><label>' + _L('스냅샷 이름', 'Snapshot Name') + '</label>';
  html += '<input id="verify-snap" class="input-field" placeholder="pcvpool/vms/web-prod@daily-20260401"></div>';
  showModal(_L('스냅샷 무결성 검증', 'Verify Snapshot Integrity'), html, async function() {
    var snap = document.getElementById('verify-snap').value.trim();
    if (!snap) { toast(_L('스냅샷 이름 필수', 'Snapshot name required'), 'w'); return; }
    try {
      var r = await fetchPost(EP.BACKUP_VERIFY(), { snapshot: snap });
      var d = unwrapData(r);
      toast('✅ ' + esc(d.snapshot) + ': ' + (d.integrity || 'ok'), 's');
    } catch(e) { toast(_L('검증 실패', 'Verification failed'), 'e'); }
  });
}


async function renderPersistentJobs(b) {
  b.innerHTML = showSkeleton();
  try {
    var r = await fetchGet(EP.JOBS_PERSIST());
    var list = unwrapList(r);
    var h = H.section(_L('영속 작업 목록', 'Persistent Jobs'));
    if (list.length === 0) {
      h += '<div class="empty-state" style="padding:30px;text-align:center"><div style="font-size:36px;opacity:.5">&#128203;</div>';
      h += '<div class="color-muted">' + _L('진행 중인 작업 없음', 'No pending jobs') + '</div></div>';
    } else {
      h += '<table class="data-table text-11"><thead><tr>';
      h += '<th>ID</th><th>' + _L('유형', 'Type') + '</th><th>' + _L('상태', 'Status') + '</th><th>' + _L('VM', 'VM') + '</th></tr></thead><tbody>';
      list.forEach(function(j) {
        h += '<tr><td>' + esc(j.job_id || '') + '</td><td>' + esc(j.type || '') + '</td>';
        h += '<td>' + esc(j.status || '') + '</td><td>' + esc(j.vm_name || '') + '</td></tr>';
      });
      h += '</tbody></table>';
    }
    b.innerHTML = h;
  } catch(e) { b.innerHTML = '<p class="color-muted">' + _L('로드 실패', 'Failed') + '</p>'; }
}


async function renderDbMigration(b) {
  b.innerHTML = showSkeleton();
  try {
    var r = await fetchGet(EP.DB_MIGRATION());
    var d = unwrapData(r);
    var h = H.section(_L('DB 스키마 상태', 'Database Schema Status'));
    h += '<div class="grid-3" style="gap:12px">';
    h += H.statCard(_L('스키마 버전', 'Schema Version'), d.schema_version || 1, '📋');
    h += H.statCard(_L('상태', 'Status'), d.status || 'ok', '✅');
    h += H.statCard('RBAC DB', d.rbac_db || '-', '🗄️');
    h += '</div>';
    b.innerHTML = h;
  } catch(e) { b.innerHTML = '<p class="color-muted">' + _L('로드 실패', 'Failed') + '</p>'; }
}


async function renderDeepHealth(b) {
  b.innerHTML = showSkeleton();
  try {
    var r = await fetchGet(EP.HEALTH_DEEP());
    var d = unwrapData(r);
    var h = H.section(_L('심화 헬스 체크', 'Deep Health Check'));
    h += '<div class="grid-3" style="gap:12px">';
    h += H.statCard('ZFS Pool', d.zfs_pool || '?', d.zfs_pool === 'ok' ? '🟢' : '🔴');
    h += H.statCard('nftables', (d.nftables_rules || 0) + _L('개 규칙', ' rules'), '🛡️');
    h += H.statCard(_L('전체', 'Overall'), d.status || '?', d.status === 'ok' ? '✅' : '⚠️');
    h += '</div>';
    b.innerHTML = h;
  } catch(e) { b.innerHTML = '<p class="color-muted">' + _L('로드 실패', 'Failed') + '</p>'; }
}

window.doConfigReload = doConfigReload;
window.showBackupVerify = showBackupVerify;
window.renderPersistentJobs = renderPersistentJobs;
window.renderDbMigration = renderDbMigration;
window.renderDeepHealth = renderDeepHealth;


PCV.advanced = {
  renderTemplates: renderTemplates,
  showTemplateCreate: showTemplateCreate,
  doTemplateCreate: doTemplateCreate,
  templateUse: templateUse,
  templateDel: templateDel,
  loadTemplateHistory: loadTemplateHistory,
  renderDocker: renderDocker,
  showDockerPull: showDockerPull,
  doDockerPull: doDockerPull,
  showDockerRun: showDockerRun,
  doDockerRun: doDockerRun,
  dockerStop: dockerStop,
  renderTerraform: renderTerraform,
  tfPlan: tfPlan,
  tfApply: tfApply,
  loadTfState: loadTfState,
  configBackup: configBackup,
  configHistory: configHistory,
  renderConfigMgmt: renderConfigMgmt,
  loadConfigHistoryInline: loadConfigHistoryInline,
  saveStorageCfg: saveStorageCfg,
  showImportOva: showImportOva,
  doImportOva: doImportOva,
  doConfigReload: doConfigReload,
  showBackupVerify: showBackupVerify,
  renderPersistentJobs: renderPersistentJobs,
  renderDbMigration: renderDbMigration,
  renderDeepHealth: renderDeepHealth
};

})(window.PCV);








window.PCV = window.PCV || {};
(function(PCV) {


async function _rpc(method, params) {
  var body = { jsonrpc: '2.0', method: method, params: params || {}, id: 'sh-' + Date.now() };
  var r = await fetchPost(EP.RPC(), body);
  return unwrapData(r);
}


function _fmtTime(ts) {
  if (!ts) return '-';
  var d = new Date(ts * 1000);
  var pad = function(n) { return n < 10 ? '0' + n : n; };
  return d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate()) +
         ' ' + pad(d.getHours()) + ':' + pad(d.getMinutes()) + ':' + pad(d.getSeconds());
}
function _fmtRelative(ts) {
  if (!ts) return '-';
  var diff = Math.floor(Date.now() / 1000) - ts;
  if (diff < 60) return diff + 's ago';
  if (diff < 3600) return Math.floor(diff / 60) + 'm ago';
  if (diff < 86400) return Math.floor(diff / 3600) + 'h ago';
  return Math.floor(diff / 86400) + 'd ago';
}


function _el(tag, attrs, children) {
  var e = document.createElement(tag);
  if (attrs) {
    Object.keys(attrs).forEach(function(k) {
      if (k === 'class') e.className = attrs[k];
      else if (k === 'text') e.textContent = attrs[k];
      else if (k === 'onclick') e.onclick = attrs[k];
      else if (k === 'title') e.title = attrs[k];
      else e.setAttribute(k, attrs[k]);
    });
  }
  if (children) {
    children.forEach(function(c) {
      if (c == null) return;
      if (typeof c === 'string') e.appendChild(document.createTextNode(c));
      else e.appendChild(c);
    });
  }
  return e;
}


var _state = {
  mode: null,
  pending: [],
  history: [],
  agentHistory: null,
  loading: false,
  lastRefresh: 0
};


async function refresh() {
  _state.loading = true;
  try {
    var pending = await _rpc('healing.pending', {});
    _state.pending = Array.isArray(pending) ? pending : [];
    var history = await _rpc('healing.history', {});
    _state.history = Array.isArray(history) ? history : [];
    var agentH = await _rpc('agent.history', {});
    _state.agentHistory = agentH;
    _state.lastRefresh = Date.now();
  } catch (e) {
    console.error('[selfhealing] refresh failed:', e);
  } finally {
    _state.loading = false;
  }
  render();
}


async function setMode(mode) {
  if (mode !== 'active' && mode !== 'dry_run') return;
  var msg = mode === 'active'
    ? '⚠ active 모드 전환 시 자동 액션이 실제로 실행됩니다.\n\n계속하시겠습니까?'
    : 'dry_run으로 복귀합니다 (자동 액션 미실행).';
  if (!confirm(msg)) return;
  try {
    var d = await _rpc('healing.set_mode', { mode: mode });
    if (d && d.mode) {
      _state.mode = d.mode;
      alert('모드 전환 완료: ' + d.mode);
      refresh();
    }
  } catch (e) { alert('모드 전환 실패: ' + (e.message || e)); }
}


async function approve(actionId) {
  if (!confirm('action_id=' + actionId + ' 승인 → 실제 실행됩니다.\n\n계속?')) return;
  try {
    await _rpc('ai.healing.approve', { action_id: actionId });
    alert('승인됨 (action_id=' + actionId + ')');
    refresh();
  } catch (e) { alert('승인 실패: ' + (e.message || e)); }
}
async function reject(actionId) {
  var reason = prompt('거부 사유 (선택):', '');
  try {
    await _rpc('ai.healing.reject', { action_id: actionId, reason: reason || 'manual' });
    alert('거부됨 (action_id=' + actionId + ')');
    refresh();
  } catch (e) { alert('거부 실패: ' + (e.message || e)); }
}
async function resetBaseline() {
  if (!confirm('Z-Score 통계를 모두 리셋합니다.\n50초간 anomaly 감지 일시 중단 후 새 baseline 학습.\n\n계속?')) return;
  try {
    var d = await _rpc('anomaly.reset_baseline', {});
    alert(d && d.message ? d.message : 'Baseline 리셋 완료');
  } catch (e) { alert('리셋 실패: ' + (e.message || e)); }
}
async function triggerAgent() {
  var ctx = prompt('AI Agent에 분석 요청 (context 메시지):',
                   'Manual trigger from Web UI at ' + new Date().toISOString());
  if (!ctx) return;
  try {
    var d = await _rpc('agent.compare_manual', { context: ctx });
    alert((d && d.note) || 'AI Agent 분석 요청 완료. agent.history로 결과 확인.');
    setTimeout(refresh, 12000);
  } catch (e) { alert('AI Agent 호출 실패: ' + (e.message || e)); }
}


function render() {
  var root = document.getElementById('selfhealing-panel');
  if (!root) return;


  while (root.firstChild) root.removeChild(root.firstChild);


  var modeBadge = _el('span', {
    class: 'sh-badge ' + (_state.mode === 'active' ? 'sh-active' : 'sh-dry'),
    text: _state.mode === 'active' ? 'ACTIVE' : 'DRY RUN'
  });
  var ctrls = _el('div', { class: 'sh-controls' }, [
    modeBadge,
    _el('button', { onclick: function() { setMode('active'); }, text: '→ active' }),
    _el('button', { onclick: function() { setMode('dry_run'); }, text: '→ dry_run' }),
    _el('button', { onclick: triggerAgent, title: 'AI Agent 직접 호출', text: '🤖 Agent 분석' }),
    _el('button', { onclick: resetBaseline, title: 'Z-Score 통계 리셋', text: '⟲ Baseline' }),
    _el('button', { onclick: refresh, text: '🔄 새로고침' }),
    _state.lastRefresh
      ? _el('span', { class: 'sh-time', text: '(' + _fmtRelative(Math.floor(_state.lastRefresh/1000)) + ')' })
      : null
  ]);
  root.appendChild(_el('div', { class: 'sh-header' }, [
    _el('h2', { text: '🛡 AI Ops Self-Healing' }),
    ctrls
  ]));


  var pendingSec = _el('section', { class: 'sh-section' }, [
    _el('h3', { text: '⏳ 승인 대기 (' + _state.pending.length + ')' })
  ]);
  if (_state.pending.length === 0) {
    pendingSec.appendChild(_el('p', { class: 'sh-empty', text: '대기 중인 액션 없음' }));
  } else {
    var thead = _el('thead', null, [
      _el('tr', null, ['ID','정책','액션','이유','시각','관리'].map(function(t) {
        return _el('th', { text: t });
      }))
    ]);
    var tbody = _el('tbody');
    _state.pending.forEach(function(p) {
      tbody.appendChild(_el('tr', null, [
        _el('td', null, [_el('code', { text: String(p.id) })]),
        _el('td', { text: p.policy || '' }),
        _el('td', null, [_el('span', {
          class: 'sh-action sh-act-' + (p.action || ''),
          text: p.action || ''
        })]),
        _el('td', { class: 'sh-reason', text: p.reason || '' }),
        _el('td', { text: _fmtRelative(p.ts) }),
        _el('td', null, [
          _el('button', { class: 'sh-approve', onclick: (function(id){
            return function() { approve(id); };
          })(p.id), text: '✓ 승인' }),
          _el('button', { class: 'sh-reject', onclick: (function(id){
            return function() { reject(id); };
          })(p.id), text: '✗ 거부' })
        ])
      ]));
    });
    pendingSec.appendChild(_el('table', { class: 'sh-table' }, [thead, tbody]));
  }
  root.appendChild(pendingSec);


  var historySec = _el('section', { class: 'sh-section' }, [
    _el('h3', { text: '📜 실행 이력 (' + _state.history.length + ')' })
  ]);
  if (_state.history.length === 0) {
    historySec.appendChild(_el('p', { class: 'sh-empty', text: '실행 이력 없음' }));
  } else {
    var thead2 = _el('thead', null, [
      _el('tr', null, ['시각','액션','대상','이유','결과','소요'].map(function(t) {
        return _el('th', { text: t });
      }))
    ]);
    var tbody2 = _el('tbody');
    _state.history.slice(0, 20).forEach(function(h) {
      tbody2.appendChild(_el('tr', null, [
        _el('td', { text: _fmtTime(h.timestamp) }),
        _el('td', null, [_el('span', {
          class: 'sh-action sh-act-' + (h.action || ''), text: h.action || ''
        })]),
        _el('td', { text: h.target || '' }),
        _el('td', { class: 'sh-reason', text: h.reason || '' }),
        _el('td', null, [_el('span', {
          class: 'sh-result sh-res-' + (h.result || ''), text: h.result || ''
        })]),
        _el('td', { text: (h.duration_ms || 0) + 'ms' })
      ]));
    });
    historySec.appendChild(_el('table', { class: 'sh-table' }, [thead2, tbody2]));
    if (_state.history.length > 20) {
      historySec.appendChild(_el('p', { class: 'sh-empty',
        text: '… 최근 20건만 표시 (전체 ' + _state.history.length + '건)' }));
    }
  }
  root.appendChild(historySec);


  var agentSec = _el('section', { class: 'sh-section' }, [
    _el('h3', { text: '🧠 AI Agent 최근 합의' })
  ]);
  var ah = _state.agentHistory;
  if (!ah || ah.status === 'no_data') {
    agentSec.appendChild(_el('p', { class: 'sh-empty',
      text: '합의 이력 없음 (활성 provider 0 또는 트리거 미발생)' }));
  } else {
    agentSec.appendChild(_el('p', null, [
      _el('strong', { text: '합의 액션: ' }),
      _el('code', { text: ah.consensus_action || '?' }),
      ' (신뢰도 ' + (ah.consensus_confidence || 0).toFixed(2) + ')'
    ]));
    agentSec.appendChild(_el('p', { text: '평균 latency: ' +
      (ah.avg_latency_ms || 0).toFixed(0) + 'ms' }));
    if (ah.results && ah.results.length) {
      var thead3 = _el('thead', null, [
        _el('tr', null, ['Provider','액션','신뢰도','긴급도','이유'].map(function(t) {
          return _el('th', { text: t });
        }))
      ]);
      var tbody3 = _el('tbody');
      ah.results.forEach(function(r) {
        tbody3.appendChild(_el('tr', null, [
          _el('td', null, [_el('strong', { text: r.provider || '?' })]),
          _el('td', null, [_el('span', {
            class: 'sh-action sh-act-' + (r.action || ''), text: r.action || '-'
          })]),
          _el('td', { text: (r.confidence || 0).toFixed(2) }),
          _el('td', { text: r.urgency || '-' }),
          _el('td', { class: 'sh-reason', text: r.reason || '-' })
        ]));
      });
      agentSec.appendChild(_el('table', { class: 'sh-table sh-agent-table' }, [thead3, tbody3]));
    }
  }
  root.appendChild(agentSec);
}




function renderSelfHealing(b) {
  if (!b) return;

  while (b.firstChild) b.removeChild(b.firstChild);

  var header = _el('div', { class: 'sh-page-header' }, [
    _el('h1', { text: '🛡 AI Self-Healing 관리' }),
    _el('p', { class: 'color-muted',
      text: '8개 정책 + AI Agent 합의 + 5중 안전장치. ADR-0020/0021 참조.' })
  ]);
  b.appendChild(header);
  b.appendChild(_el('div', { id: 'selfhealing-panel', class: 'hc' }));

  setTimeout(refresh, 50);
}
window.renderSelfHealing = renderSelfHealing;


PCV.selfhealing = {
  refresh:        refresh,
  setMode:        setMode,
  approve:        approve,
  reject:         reject,
  resetBaseline:  resetBaseline,
  triggerAgent:   triggerAgent,
  render:         render,
  renderPage:     renderSelfHealing,
  state:          _state
};

})(window.PCV);
