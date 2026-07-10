/* ═══════════════════════════════════════════════════════════════
   PureCVisor — modules/security.js
   Native Host HIDS/HIPS event review UI
   ADR-0013: IIFE module scope — PCV.security namespace
   ═══════════════════════════════════════════════════════════════ */
window.PCV = window.PCV || {};
(function(PCV) {
  'use strict';

  var API = PCV.api || {};
  var currentEventId = '';
  var lastEvents = [];

  /*
   * Security UI is intentionally RPC-backed instead of using ad hoc REST paths:
   * the CLI, TUI, and browser all exercise the same RBAC, audit, and job result
   * contracts behind security.*.
   */
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
    if (sev === 'crit') return HN.badge('CRIT', 'r');
    if (sev === 'warn') return HN.badge('WARN', 'y');
    return HN.badge('INFO', 'g');
  }

  function badgeStatus(status) {
    if (status === 'resolved' || status === 'suppressed') return HN.badge(upper(status), 'g');
    if (status === 'action_pending') return HN.badge('PENDING', 'y');
    if (status === 'open') return HN.badge('OPEN', 'y');
    return HN.badge(status || '-', 'y');
  }

  function statusCard(title, value, hint, type) {
    var el = PCV.uxlib.el;
    var cls = type === 'r' ? 'color-red' : type === 'y' ? 'color-yellow' : 'color-green';
    return HN.card(title, [
      el('div', { class: 'stat-md ' + cls }, value),
      el('div', { class: 'stat-label mt-4' }, hint || '')
    ], 'text-center');
  }

  function renderStatusBar(status, pendingCount) {
    /*
     * First screen answers four operational questions: is Guard enabled, is the
     * baseline trusted, is there open risk, and is any action waiting for approval.
     */
    var el = PCV.uxlib.el;
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

    var grid = HN.grid(4,
      statusCard('Guard', upper(guard), guardHint, guard === 'enabled' ? 'g' : 'r'),
      statusCard('Baseline', upper(baseline), baselineHint, baseline === 'trusted' ? 'g' : 'y'),
      statusCard(_L('Open Risk', 'Open Risk'), risk, degraded ? _L('store degraded', 'store degraded') : _L('CRIT/WARN 가중치', 'CRIT/WARN weighted'), Number(risk) > 0 ? 'r' : 'g'),
      statusCard(_L('Pending Actions', 'Pending Actions'), pending, _L('수동 승인 전용', 'manual approval only'), Number(pending) > 0 ? 'y' : 'g'));

    var controlsRow = canRole('admin')
      ? el('label', { class: 'text-xs', style: 'display:flex;align-items:center;gap:6px' },
          el('input', { type: 'checkbox', 'data-sec-enabled': '1', checked: guard === 'enabled' ? '' : null }),
          ' ' + _L('Security Guard 활성', 'Enable Security Guard'))
      : el('span', { class: 'stat-label' }, _L('Guard 변경은 admin 권한이 필요합니다.', 'Changing Guard state requires admin.'));

    var bar = el('div', { class: 'hc mb-12' },
      el('div', { class: 'flex gap-8 flex-wrap items-center' },
        el('button', { class: 'btn', type: 'button', 'data-sec-refresh': '1' }, '⟳ ' + _L('새로고침', 'Refresh')),
        controlsRow));

    return [grid, bar];
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

  function optionNode(value, label) {
    return PCV.uxlib.el('option', { value: value }, label || value);
  }

  function renderFilters(events) {
    var el = PCV.uxlib.el;
    var sources = uniqueValues(events, 'source');
    var statuses = uniqueValues(events, 'status');
    return el('div', { class: 'hc mb-12' },
      el('div', { class: 'flex gap-8 flex-wrap items-end' },
        el('label', { class: 'text-xs' }, _L('심각도', 'Severity'),
          el('select', { id: 'sec-filter-sev', class: 'input-pcv' },
            optionNode('', _L('전체', 'All')),
            optionNode('crit', 'CRIT'), optionNode('warn', 'WARN'), optionNode('info', 'INFO'))),
        el('label', { class: 'text-xs' }, _L('소스', 'Source'),
          el('select', { id: 'sec-filter-source', class: 'input-pcv' },
            optionNode('', _L('전체', 'All')),
            sources.map(function(v) { return optionNode(v, v); }))),
        el('label', { class: 'text-xs' }, _L('상태', 'Status'),
          el('select', { id: 'sec-filter-status', class: 'input-pcv' },
            optionNode('', _L('전체', 'All')),
            statuses.map(function(v) { return optionNode(v, v); }))),
        el('label', { class: 'text-xs', style: 'flex:1;min-width:220px' }, _L('검색', 'Search'),
          el('input', { id: 'sec-filter-q', class: 'input-pcv', type: 'search', placeholder: 'target, summary, event_id', style: 'width:100%' }))));
  }

  function renderEventTable(events) {
    var el = PCV.uxlib.el;
    if (!events.length) {
      return el('div', { class: 'empty-state p-20 text-center' },
        el('div', { class: 'empty-title' }, _L('보안 이벤트 없음', 'No security events')),
        el('div', { class: 'empty-desc' }, _L('WARN/CRIT 이벤트는 audit target과 event_id를 공유합니다.', 'WARN/CRIT events share audit target with event_id.')));
    }

    var rows = events.map(function(ev) {
      var text = [
        ev.event_id, ev.source, ev.status, ev.target, ev.summary, ev.recommended_action
      ].join(' ').toLowerCase();
      return el('tr', {
        class: 'sec-event-row',
        'data-sec-event-id': ev.event_id || '',
        'data-sec-severity': ev.severity || '',
        'data-sec-source': ev.source || '',
        'data-sec-status': ev.status || '',
        'data-sec-text': text,
        tabindex: '0',
        style: 'cursor:pointer'
      },
        el('td', { class: 'color-muted nowrap' }, formatTs(ev.timestamp)),
        el('td', null, badgeSeverity(ev.severity || 'info')),
        el('td', null, ev.source || '-'),
        el('td', null, el('code', { class: 'text-xs' }, ev.target || '-')),
        el('td', null, ev.summary || '-'),
        el('td', null, badgeStatus(ev.status || '-')),
        el('td', null, ev.recommended_action || '-'));
    });

    return el('div', { style: 'overflow:auto;max-height:560px' },
      el('table', { class: 'data-table text-11' },
        el('thead', null, el('tr', null,
          el('th', null, _L('시간', 'Time')), el('th', null, _L('심각도', 'Severity')),
          el('th', null, _L('소스', 'Source')), el('th', null, _L('대상', 'Target')),
          el('th', null, _L('요약', 'Summary')), el('th', null, _L('상태', 'Status')),
          el('th', null, _L('권고', 'Action')))),
        el('tbody', null,
          rows,
          el('tr', { id: 'sec-filter-empty', style: 'display:none' },
            el('td', { colspan: '7', class: 'color-muted text-center p-12' }, _L('필터 조건과 일치하는 이벤트가 없습니다.', 'No events match the current filters.'))))));
  }

  function renderPendingActions(actions) {
    var el = PCV.uxlib.el;
    if (!actions.length) {
      return HN.card(_L('Pending Approval Queue', 'Pending Approval Queue'),
        el('div', { class: 'empty-state p-20 text-center' },
          el('div', { class: 'empty-title' }, _L('승인 대기 없음', 'No pending approvals'))));
    }

    var rows = actions.map(function(action) {
      var name = action.action || '';
      var executable = name === 'block_ip' || name === 'revoke_api_key';
      /*
       * The browser mirrors the backend allowlist but never relies on it for
       * safety. Backend RPC rejects non-executable actions again before running.
       */
      var controls = [];
      controls.push(executable
        ? (canRole('admin')
          ? el('button', { class: 'btn btn-r', type: 'button', 'data-sec-approve': action.event_id || '' }, _L('승인', 'Approve'))
          : el('span', { class: 'stat-label' }, _L('admin 승인 필요', 'admin approval required')))
        : el('span', { class: 'stat-label' }, _L('수동 runbook', 'manual runbook')));
      if (canRole('operator')) {
        controls.push(' ', el('button', { class: 'btn', type: 'button', 'data-sec-dismiss': action.event_id || '' }, _L('거부', 'Dismiss')));
      }
      return el('tr', { 'data-sec-action-event-id': action.event_id || '', style: 'cursor:pointer' },
        el('td', null, el('code', { class: 'text-xs' }, action.event_id || '-')),
        el('td', null, HN.badge(name || '-', executable ? 'r' : 'y')),
        el('td', null, el('code', { class: 'text-xs' }, action.target || '-')),
        el('td', null, String(action.ttl_sec || '-') + 's'),
        el('td', { class: 'nowrap' }, controls));
    });

    var body = el('div', { style: 'overflow:auto;max-height:260px' },
      el('table', { class: 'data-table text-11' },
        el('thead', null, el('tr', null,
          el('th', null, 'event_id'), el('th', null, _L('대응', 'Action')), el('th', null, _L('대상', 'Target')),
          el('th', null, 'TTL'), el('th', null, _L('처리', 'Controls')))),
        el('tbody', null, rows)));
    return HN.card(_L('Pending Approval Queue', 'Pending Approval Queue'), body);
  }

  function renderVerificationCard() {
    var el = PCV.uxlib.el;
    return HN.card(_L('Verification Gates', 'Verification Gates'), [
      HN.row('Viewer', HN.badge('READ', 'g')),
      HN.row('Operator', HN.badge('DISMISS', 'y')),
      HN.row('Admin', HN.badge('APPROVE', 'r')),
      HN.row('ADR-0018', HN.badge('ASYNC', 'g')),
      HN.row('Audit', el('code', null, 'target = event_id'))
    ]);
  }

  function renderShell(cfg, events, actions) {
    /* ADR-013 DOM-safe: 노드 반환 — 호출부가 clearEl+appendChild 로 삽입 (내부
     * 문자열 헬퍼 전체를 el/HN 노드 조립으로 전환한 캐스케이드, monitor.js 선례). */
    var el = PCV.uxlib.el, frag = PCV.uxlib.frag;
    var statusBar = renderStatusBar(cfg || {}, actions.length);
    return frag(
      HN.section('🛡 ' + _L('보안 이벤트', 'Security Events')),
      statusBar,
      renderFilters(events),
      el('div', { class: 'sg grid-2 mb-12' },
        HN.card(_L('Event Queue', 'Event Queue'), renderEventTable(events)),
        el('div', { id: 'security-detail', class: 'hc' },
          el('h4', null, _L('Selected Event', 'Selected Event')),
          el('p', { class: 'color-muted text-12' }, _L('이벤트를 선택하세요.', 'Select an event.')))),
      el('div', { class: 'sg grid-2' }, renderPendingActions(actions), renderVerificationCard())
    );
  }

  function applyFilters(root) {
    /*
     * Filtering is DOM-local so refresh always starts from the authoritative
     * backend list and never mutates lastEvents.
     */
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
    /*
     * Load config, event queue, and pending actions together. Splitting these
     * requests would let the cards and tables disagree during rapid approvals.
     */
    showSkeleton(b);
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
      PCV.uxlib.clearEl(b);
      b.appendChild(renderShell(cfg, events, actions));
      bindSecurityHandlers(b);
      applyFilters(b);
      if (currentEventId && events.some(function(ev) { return ev.event_id === currentEventId; })) {
        selectEvent(currentEventId);
      }
    } catch (e) {
      /* ADR-013 DOM-safe: 에러 카드를 el()로 조립. data-sec-refresh 훅은 유지되어
       * 이어지는 bindSecurityHandlers(b)가 Retry 버튼을 정상 바인딩한다. */
      var el = PCV.uxlib.el;
      PCV.uxlib.clearEl(b);
      b.appendChild(el('div', { class: 'hc' },
        el('h4', { class: 'color-red' }, _L('보안 이벤트 로드 실패', 'Failed to load security events')),
        el('p', { class: 'color-muted' }, e.message || ''),
        el('button', { class: 'btn', type: 'button', 'data-sec-refresh': '1' }, 'Retry')
      ));
      bindSecurityHandlers(b);
    }
  }

  function renderEventDetail(ev) {
    /* ADR-013 DOM-safe: 노드 반환 — 호출부가 clearEl+appendChild 로 삽입. */
    var el = PCV.uxlib.el;
    var action = ev.recommended_action || '';
    var executable = action === 'block_ip' || action === 'revoke_api_key';
    var controls = [];
    if (executable) {
      controls.push(canRole('admin')
        ? el('button', { class: 'btn btn-r', type: 'button', 'data-sec-approve': ev.event_id || '' }, _L('승인', 'Approve'))
        : el('span', { class: 'stat-label' }, _L('admin 승인 필요', 'admin approval required')));
    } else if (action === 'manual_runbook') {
      controls.push(el('span', { class: 'stat-label' }, _L('수동 runbook 후보입니다. 자동 실행하지 않습니다.', 'Manual runbook candidate. It will not execute automatically.')));
    }
    if (canRole('operator')) {
      controls.push(' ', el('button', { class: 'btn', type: 'button', 'data-sec-dismiss': ev.event_id || '' }, _L('거부', 'Dismiss')));
    }

    return PCV.uxlib.frag(
      el('h4', null, _L('Selected Event', 'Selected Event') + ' ', badgeSeverity(ev.severity || 'info')),
      el('div', { class: 'mb-8' }, el('b', null, ev.summary || ev.event_id || '-')),
      HN.row('event_id', el('code', null, ev.event_id || '')),
      HN.row(_L('소스', 'Source'), ev.source || '-'),
      HN.row(_L('대상 유형', 'Target Kind'), ev.target_kind || '-'),
      HN.row(_L('대상', 'Target'), el('code', { class: 'text-xs' }, ev.target || '-')),
      HN.row(_L('상태', 'Status'), badgeStatus(ev.status || '-')),
      HN.row(_L('신뢰도', 'Confidence'), String(ev.confidence || 0) + '%'),
      HN.row(_L('권고 대응', 'Recommended Response'), HN.badge(action || '-', executable ? 'r' : 'y')),
      HN.row(_L('감사 상관키', 'Audit Correlation'), el('code', null, 'security.event target=' + (ev.event_id || ''))),
      HN.row(_L('발생 횟수', 'Occurrences'), String(ev.occurrence_count || 1)),
      el('div', { class: 'mt-10 mb-8' }, el('b', { class: 'text-12' }, _L('Evidence', 'Evidence'))),
      el('pre', { class: 'stat-label', style: 'white-space:pre-wrap;overflow:auto;max-height:260px;margin:0' }, ev.evidence_json || '{}'),
      el('div', { class: 'flex gap-6 mt-10 flex-wrap' }, controls)
    );
  }

  async function selectEvent(eventId) {
    if (!eventId) return;
    currentEventId = eventId;
    var detail = document.getElementById('security-detail');
    if (!detail) return;
    showSkeleton(detail);
    try {
      var ev = await rpc('security.event.get', { event_id: eventId });
      PCV.uxlib.clearEl(detail);
      detail.appendChild(renderEventDetail(ev || {}));
      bindSecurityHandlers(detail);
      document.querySelectorAll('.sec-event-row').forEach(function(row) {
        row.classList.toggle('selected', row.getAttribute('data-sec-event-id') === eventId);
      });
    } catch (e) {
      PCV.uxlib.clearEl(detail);
      detail.appendChild(PCV.uxlib.el('p', { class: 'color-red' }, e.message || ''));
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
    /*
     * Approval starts an async job; the immediate success toast means accepted,
     * not completed. Final failure still arrives through the shared WS path.
     */
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
