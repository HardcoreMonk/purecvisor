/**
 * @module api
 * @description PureCVisor API 통신 계층 — fetch 래퍼, WebSocket, 인증
 * ADR-0013: IIFE 모듈 스코프 전환 — window.PCV.api 네임스페이스
 */

/*
 * ===== api.js 모듈 개요 (주니어 개발자 필독) =====
 *
 * [역할]
 *   백엔드(REST API)와의 모든 HTTP/WS 통신을 담당한다.
 *   다른 모듈(vm.js, container.js 등)은 직접 fetch()를 호출하지 않고,
 *   이 모듈의 fetchGet/fetchPost/fetchPut/fetchDelete를 사용한다.
 *
 * [IIFE 패턴 (ADR-0013)]
 *   (function(PCV){ ... })(window.PCV) 로 감싸는 이유:
 *   1. 내부 변수(_refreshInProgress 등)가 window를 오염시키지 않는다.
 *   2. 모듈 끝에서 PCV.api = { ... }로 공개 API만 노출한다.
 *   3. 하위 호환용 window.fetchGet = fetchGet 심은 전환기 코드이며,
 *      ADR-0013 완료 후 제거 예정. 신규 코드는 PCV.api.fetchGet() 사용을 권장.
 *
 * [주요 함수]
 *   - fetchGet(url): GET + JWT 인증 + 401 시 자동 토큰 리프레시 + 12초 타임아웃.
 *   - fetchPost(url, body): POST. 동일 에러 처리 패턴.
 *   - unwrapData(r) / unwrapList(r): 백엔드 응답의 {data:...} 래핑을 벗기는 헬퍼.
 *     백엔드가 {data: [...]}, {result: [...]}, 또는 바로 [...]를 반환할 수 있어
 *     모든 케이스를 통일 처리한다.
 *   - connectWS(): WebSocket 연결 + 프로토콜 레벨 인증 (ADR-0010).
 *   - startAdaptivePolling(id, fn, ms): 탭 비가시 시 자동 중단되는 폴링.
 *
 * [fetch 에러 패턴 (P1-4 수정)]
 *   이전에는 !r.ok일 때 throw해서 호출부에서 catch가 필요했다.
 *   현재는 !r.ok일 때 r.json()을 시도하여 에러 JSON을 반환한다.
 *   → 호출부에서 if (r.error) 체크로 통일. throw는 네트워크 장애 시만 발생.
 *
 * [WS 프로토콜 인증 (ADR-0010)]
 *   WebSocket URL에 토큰을 넣지 않는다 (URL이 서버 로그에 기록되므로).
 *   대신 연결 후 첫 메시지로 {type:'auth', token:'...'} JSON을 보낸다.
 *   서버가 {type:'auth_ok'}를 보내면 인증 완료. 이후 메시지는 이벤트 데이터.
 *   auth_fail 시 WS는 닫기지 않고 배너 표시.
 *
 * [토큰 리프레시 흐름]
 *   1. fetchGet 등에서 401 수신
 *   2. _tryRefreshToken()으로 /auth/refresh 호출
 *   3. 성공 시 새 access_token을 window.authToken + sessionStorage에 저장
 *   4. 원래 요청을 새 토큰으로 자동 재시도
 *   5. 실패 시 _redirectToLogin()으로 로그인 화면 전환
 *   _refreshInProgress로 동시 다중 리프레시 방지 (Promise 공유).
 *
 * [흔한 실수]
 *   - fetchGet의 반환값을 바로 배열로 사용하면 안 된다.
 *     반드시 unwrapList(r) 또는 unwrapData(r)를 거쳐야 한다.
 *   - window.authToken이 빈 문자열이면 WS 연결이 2초 후 재시도한다.
 *     로그인 전에 connectWS를 호출하지 않도록 주의.
 *   - 적응형 폴링 시작 후 반드시 stopAdaptivePolling(id)로 정리해야 한다.
 *     안 하면 탭 전환 시 visibilitychange 리스너가 누적된다.
 */

/* ═══ DEFAULTS (app.js 로드 전 보장) ═══
 *  api.js가 app.js보다 먼저 <script>로 로드될 수 있으므로,
 *  app.js의 var 선언이 아직 없을 때를 대비한 방어 초기화. */
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

/* ═══ TOKEN REFRESH ═══ */
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
  /* pcv_pass는 더 이상 저장하지 않음 (보안 고도화) */
  if (window.wsConnection) window.wsConnection.close();
  pcvSetLoginVisible(true);
  var la = document.getElementById('la');
  if (la) la.classList.remove('hidden');
  var us = document.getElementById('us');
  if (us) { us.classList.add('hidden'); us.style.display = 'none'; }
}

/* 타임아웃 설정: 백엔드 REST→RPC는 per-method 타임아웃이 최대 60초이지만,
 * 대부분의 엔드포인트는 8초 이내 응답한다. 12초 = 백엔드 8초 + 네트워크 여유.
 * AbortController로 구현하여 브라우저가 소켓을 강제 해제한다. */
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
      /* Update auth header with new token and retry */
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

/* ═══ API HELPERS ═══
 * [에러 반환 패턴]
 *   모든 fetchXxx 함수는 HTTP 에러(4xx/5xx) 시 throw하지 않고,
 *   r.json()을 시도하여 {error: {code, message}} 형태의 JSON을 반환한다.
 *   이렇게 하면 호출부에서 try/catch 없이 if(r.error)로 분기할 수 있다.
 *   네트워크 장애(DNS 실패, 타임아웃 등)만 throw가 발생한다.
 *
 *   r.json().catch()는 응답 body가 JSON이 아닌 경우(예: nginx 502 HTML)를
 *   방어한다 — 이때 합성 에러 객체를 직접 만들어 반환한다. */
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

/* ═══ WEBSOCKET ═══
 * [연결 흐름]
 *   1. authToken 없으면 2초 후 재시도 (로그인 대기).
 *   2. 5회 실패 시 배너 표시하고 포기. 배너의 "Retry" 클릭으로 수동 재시도.
 *   3. 지수 백오프: 1s, 2s, 4s, 8s, 16s (최대 30초).
 *
 * [인증 프로토콜 (ADR-0010)]
 *   URL에 토큰을 넣으면 프록시/CDN 로그에 노출될 위험이 있다.
 *   따라서 onopen 시 첫 메시지로 {type:'auth', token:JWT}를 보내고,
 *   서버 응답 {type:'auth_ok'}을 받은 후에만 이벤트 메시지를 처리한다.
 *   _pcvAuthDone 플래그로 인증 전/후 메시지를 구분한다. */
/* ADR-0018: WS job.complete fail 이벤트 처리 — 진행 중 모달이 있으면 사유 surface */
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
  /* 토스트는 즉시 사라지므로 notification center에 영구 기록 */
  var rawMethod = m.method || '?';
  var rawError = m.error || 'unknown';
  var target = '';
  if (m.job_id) {
    /* job_id 형식: "vm.start:vmname" */
    var colonIdx = m.job_id.indexOf(':');
    if (colonIdx > 0) target = m.job_id.substring(colonIdx + 1);
  }
  var title = rawMethod + (target ? ' failed: ' + target : ' failed');
  /* addNotification 1회만 호출 — toast()도 내부적으로 addNotification을 부르므로
   * 두 가지를 모두 호출하면 알림 센터에 중복 항목이 쌓인다. */
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
      /* ADR-013 DOM-safe: 배너 텍스트/버튼을 문자열 innerHTML 대신 el/frag 조립.
       * Retry는 모듈 변수 _wsReconnectAttempt(위 임계 검사가 읽는 카운터)를
       * 직접 리셋한다 — 구 인라인 onclick은 window._wsReconnectAttempt(별개
       * 전역)를 리셋해 임계가 유지된 채 connectWS 재진입 → 배너가 즉시
       * 재생성되던 버그가 있었다. 클로저는 IIFE 스코프라 직접 접근 가능. */
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
  /* ADR-0010: 프로토콜 레벨 인증 — URL에 토큰 미포함, 첫 메시지로 인증 */
  const ws = new WebSocket(p + '//' + location.host + window.API_BASE + '/ws/events');
  window.wsConnection = ws;
  ws.onopen = () => {
    /* 연결 즉시 인증 메시지 전송 */
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
    /* ADR-0010: auth 응답 처리 */
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
      } catch (_) { /* not JSON */ }
    }
    try {
      const m = JSON.parse(e.data);
      window.addEvt('WS Event — type: ' + m.type + (m.name ? ', target: ' + m.name : '') + (m.node ? ', node: ' + m.node : ''));
      /* F4: Flash affected VM/CTR in sidebar */
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
      /* ADR-0018: 비동기 워커 실패 이벤트 — vmPower 모달이 열려있으면 즉시 사유 표시
       * pcv_ws_broadcast가 {type, ts, payload}로 wrap하므로 payload 안에서 status 추출 */
      if (m.type === 'job.complete') {
        var jobPayload = m.payload || m;  /* 호환성: 직접 또는 payload 안 */
        if (jobPayload && jobPayload.status === 'fail') {
          try { _onJobFailureEvent(jobPayload); } catch (_) {}
        }
      }
      if (m.type && (m.type.startsWith('vm-') || m.type.startsWith('vm.'))) {
        /* W6 fix: 이벤트 타입별 선택적 refetch — 불필요한 전체 목록 재조회 제거
           - vm.state_changed / vm-state: 해당 VM 1건만 업데이트 시도, 실패 시 전체
           - vm.created / vm-create: 전체 재조회 (새 VM이 목록에 없음)
           - vm.deleted / vm-delete: 전체 재조회
           - 기타: 전체 재조회 */
        var doFullRefresh = function(retry) {
          fetchGet(window.API_BASE + '/vms').then(function(r) {
            var list = unwrapList(r);
            window.vmList = list;
            if (window.selectedVmIndex >= list.length) window.selectedVmIndex = 0;
            window.lastLoadTime = Date.now();
            if (typeof render === 'function') render(true);
          }).catch(function() {
            /* 실패 시 3초 후 최대 2회 재시도 (exponential 제한) */
            if (retry < 2) {
              setTimeout(function() { doFullRefresh(retry + 1); }, 3000 * (retry + 1));
            }
          });
        };
        var eventType = m.type;
        var isStateOnly = (eventType === 'vm.state_changed' || eventType === 'vm-state');
        if (isStateOnly && m.name && window.vmList) {
          /* 개별 VM 상태만 패치 (전체 refetch 회피) */
          var idx = window.vmList.findIndex(function(v){ return v && v.name === m.name; });
          if (idx >= 0 && m.state) {
            window.vmList[idx].state = m.state;
            if (typeof render === 'function') render(true);
            return;  /* refetch 불필요 */
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
    } catch (x) { /* non-JSON WS message */ }
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
    /*
     * nginx/프록시 오류는 HTML을 돌려줄 수 있다. 로그인 화면에는 JSON parser의
     * 원시 오류 문구나 HTML 일부를 노출하지 않고 상태만 전달한다.
     */
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

/* ═══ LOGIN / LOGOUT ═══ */
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
      /* pcv_pass 저장 제거 — 평문 비밀번호 sessionStorage 노출 방지 */
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
      /* #14/#15: hash 라우팅 + role 가시성 적용 */
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
      /* pcv_pass 저장 제거 — 평문 비밀번호 sessionStorage 노출 방지 */
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
  /* pcv_pass는 더 이상 저장하지 않음 (보안 고도화) */
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

/* ═══ SESSION EXPIRY WARNING ═══ */
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
        /* Expired */
        clearInterval(_sessionCheckInterval);
        _redirectToLogin();
        toast(typeof _L === 'function' ? _L('세션 만료 — 다시 로그인하세요', 'Session expired — please login again') : 'Session expired', false);
      } else if (remaining < 300000) {
        /* 5 min warning — try refresh */
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
        /* Auto refresh attempt — refresh token 기반 (평문 비밀번호 미사용) */
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
    } catch (e) { /* invalid JWT */ }
  }, 30000);
}

/* ═══ SESSION RESTORE ═══ */
function restoreSession() {
  if (!window.authToken) {
    /* 토큰 없음 → 로그인 페이지 표시 */
    pcvSetLoginVisible(true);
    var la = document.getElementById('la');
    if (la) la.classList.remove('hidden');
    return;
  }
  const savedUser = sessionStorage.getItem('pcv_user') || 'admin';
  /* refresh token으로 세션 복원 (평문 비밀번호 미사용) */
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
    /* pcv_pass는 더 이상 저장하지 않음 (보안 고도화) */
    window.authToken = '';
  });
}

/* ═══ API ACTIVITY LOGGING ═══ */
var _apiActivityLog = [];

/* ═══ K2: PERFORMANCE METRICS ═══ */
var _perfMetrics = {
  pageLoadTime: 0,
  firstContentfulPaint: 0,
  apiCallCount: 0,
  apiTotalTime: 0,
  avgApiTime: 0
};

/* Collect page load timing */
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

/* ═══ RESPONSE UNWRAP HELPERS ═══ */

/**
 * unwrapData — 백엔드 응답에서 실제 데이터를 추출
 *
 * 백엔드는 RPC 결과를 {data: ...}로 래핑하지만, 일부 엔드포인트는
 * {result: ...} 또는 raw 값을 반환할 수 있음. 이 함수가 모든 케이스를 처리.
 *
 * @param {*} r - fetch 응답 JSON
 * @returns {*} 언래핑된 데이터
 */
function unwrapData(r) {
  if (r == null) return r;
  if (r.data !== undefined) return r.data;
  if (r.result !== undefined) return r.result;
  return r;
}

/**
 * unwrapList — 백엔드 응답에서 배열 데이터를 추출
 *
 * 목록 엔드포인트용. 배열이 아니면 빈 배열 반환.
 *
 * @param {*} r - fetch 응답 JSON
 * @returns {Array} 언래핑된 배열 (또는 빈 배열)
 */
function unwrapList(r) {
  if (Array.isArray(r)) return r;
  var d = unwrapData(r);
  return Array.isArray(d) ? d : [];
}

/* ═══ 적응형 폴링 (Adaptive Polling) — 탭 비가시 시 중단 ═══
 *  document.hidden이 true면 콜백 실행을 건너뛴다.
 *  왜 필요한가: 백그라운드 탭에서 10초마다 /vms를 호출하면
 *  서버에 불필요한 부하를 주고, per-user rate limit (1200 req/min)에
 *  도달할 수 있다. visibilitychange 리스너로 탭 복귀 시 즉시 갱신. */
var _pollingTimers = {};
function startAdaptivePolling(id, fn, intervalMs) {
  stopAdaptivePolling(id);
  var run = function() {
    if (document.hidden) return;  /* 탭 비가시 시 스킵 */
    fn();
  };
  run();
  _pollingTimers[id] = setInterval(run, intervalMs);
  /* Visibility API: 탭 전환 시 즉시 반영 — 리스너 ref를 저장해 stop에서 즉시 해제 */
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

/* ═══ K2: FETCH INTERCEPTOR FOR PERF METRICS ═══ */
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

/* ═══ EXPORT TO PCV NAMESPACE (ADR-0013) ═══
 *  PCV.api에 등록되는 것이 이 모듈의 공식 인터페이스이다.
 *  _로 시작하는 함수(_tryRefreshToken 등)도 디버깅용으로 노출하지만,
 *  외부 모듈에서 직접 호출은 비권장. */
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

/* ═══ BACKWARD COMPAT SHIMS (ADR-0013: remove after full transition) ═══
 *  app.js와 HTML onclick="fetchGet(...)"에서 window 직접 참조를 사용하므로,
 *  전환 완료 전까지 window에도 같은 함수를 등록한다.
 *  신규 코드에서는 PCV.api.fetchGet()을 사용하라.
 *  이 심 코드를 제거하려면 먼저 모든 HTML onclick과 다른 모듈의
 *  window.fetchGet 참조를 PCV.api.fetchGet으로 변경해야 한다. */
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
/* window._wsReconnectAttempt 잔재 export 제거 — 구 인라인 onclick 전용이었고
 * Retry 클로저가 모듈 변수를 직접 리셋하므로 더 이상 소비처 없음. */

})(window.PCV);
