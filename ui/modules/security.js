




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
