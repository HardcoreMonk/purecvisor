(function (PCV) {
  'use strict';

  var KIND = { baseline: ['기록 시작', 'History start'], created: ['생성', 'Created'], changed: ['변경', 'Changed'],
               undefined: ['삭제', 'Deleted'], resized: ['디스크 크기 요청', 'Disk size request'] };
  var CAUSE = { libvirt_defined: ['정의 변경', 'Definition changed'], libvirt_undefined: ['정의 삭제', 'Definition removed'],
                device_changed: ['장치 변경', 'Device changed'], metadata_changed: ['메타데이터 변경', 'Metadata changed'],
                rpc: ['요청', 'Request'], reconcile: ['대조 스캔', 'Reconcile scan'], startup: ['기동 확인', 'Startup check'],
                reconnect: ['재연결 확인', 'Reconnect check'] };
  var GAP = { shutdown: ['데몬 중지', 'daemon stopped'], listener_lost: ['libvirt 연결 끊김', 'libvirt connection lost'],
              crash_inferred: ['비정상 종료 추정', 'abnormal exit (inferred)'], store_write_failed: ['저장 실패', 'store write failed'],
              disabled: ['기록 꺼짐', 'recording disabled'], capture_failed: ['이 VM 읽기 실패', 'VM read failed'],
              pruned: ['보관 기간 정리', 'pruned by retention'] };
  var SPAN_MS = 7 * 86400000;

  function L(pair) { return _L(pair[0], pair[1]); }

  function fmtTime(ms) {
    var d = new Date(ms), now = new Date(Date.now());
    var p = function (n) { return (n < 10 ? '0' : '') + n; };
    var hm = p(d.getHours()) + ':' + p(d.getMinutes());
    if (d.toDateString() === now.toDateString()) return hm;
    return d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate()) + ' ' + hm;
  }

  function rowTime(e) {
    if (e.window_start) return fmtTime(e.window_start) + ' ~ ' + fmtTime(e.observed_at) + ' ' + _L('사이', 'window');
    return fmtTime(e.observed_at);
  }

  function actorText(e) {
    var who;
    if (e.actor_expired) who = _L('행위자 기록 만료(30일)', 'Actor record expired (30 days)');
    else if (e.actor_hidden) who = _L('다른 주체', 'Another principal');
    else if (e.attribution === 'rpc_ambiguous') who = _L('여러 요청 중 하나', 'One of several requests');
    else if (e.actor) who = e.actor;
    else who = _L('요청 귀속 없음', 'No request attribution');
    if (e.actor_method && e.attribution !== 'rpc_ambiguous') who += ' · ' + e.actor_method;
    if (e.actor_result === 'fail') who += ' · ' + _L('실패한 요청 중 바뀜', 'Changed during a failed request');
    return who;
  }

  function pathLabel(path) {
    var m = /^disk\[([^\]]+)\]\.requested_size_bytes$/.exec(path || '');
    return m ? _L('요청 크기', 'Requested size') + ' (' + m[1] + ')' : path;
  }

  function valueText(path, v) {
    if (/^(live\.)?opaque\./.test(path || '')) return v === null || v === undefined ? '—' : _L('바뀜(내용 비공개)', 'Changed (content hidden)');
    if (v === null || v === undefined) return '—';
    return typeof v === 'string' ? v : JSON.stringify(v);
  }

  function unwrap(r) {
    if (!r || r.error) return { err: (r && r.error && r.error.message) || _L('요청 실패', 'Request failed') };
    return { data: (typeof unwrapData === 'function') ? unwrapData(r) : (r.data || r.result || r) };
  }

  function detailNode(d) {
    var el = PCV.uxlib.el;
    var rows = (d.changes || []).map(function (c) {
      return el('tr', null, el('td', { class: 'mono' }, pathLabel(c.path)),
        el('td', null, valueText(c.path, c.before)), el('td', null, valueText(c.path, c.after)));
    });
    var table = el('table', { class: 'tbl vmch-detail' },
      el('thead', null, el('tr', null, el('th', null, _L('경로', 'Path')), el('th', null, _L('이전', 'Before')),
        el('th', null, _L('이후', 'After')))),
      el('tbody', null, rows.length ? rows : el('tr', null, el('td', { colspan: '3' }, _L('바뀐 칸 없음', 'No field changes')))));
    return PCV.uxlib.frag(el('div', { style: 'overflow-x:auto' }, table),
      PCV.uxlib.msg('muted', { tag: 'div', size: '11px', style: 'margin-top:4px' },
        d.prev_id ? _L('직전 기록 있음', 'Previous record available')
                  : _L('직전 기록 없음(권한 밖이거나 정리됨)', 'No previous record (out of scope or pruned)')),
      d.truncated ? PCV.uxlib.msg('warn', { tag: 'div', size: '11px' }, _L('응답 크기 상한으로 변경 목록이 잘렸습니다', 'Changes truncated by response size cap')) : null);
  }

  function entryNode(e, state, idx) {
    var el = PCV.uxlib.el;
    var did = 'vmch-d-' + state.seq + '-' + idx;
    var detail = el('div', { id: did, role: 'region', 'aria-label': _L('변경 상세', 'Change detail'), hidden: 'hidden',
      class: 'vmch-detail-box', style: 'padding:6px 0 6px 18px' });
    var paths = (e.changes || []).slice(0, 3).map(function (c) { return pathLabel(c.path); });
    var more = (e.change_count || 0) - paths.length;
    var summary = paths.join(', ') + (more > 0 ? ' ' + _L('외 ', '+') + more : '');
    var kind = KIND[e.kind] || [e.kind, e.kind];
    var causeBase = String(e.cause || '').split('+')[0];
    var causeExtra = String(e.cause || '').slice(causeBase.length);
    var btn = el('button', { type: 'button', class: 'vmch-row', 'aria-expanded': 'false', 'aria-controls': did,
      style: 'display:flex;flex-wrap:wrap;gap:6px 10px;align-items:center;width:100%;text-align:left;background:none;border:0;' +
             'padding:6px 4px;color:var(--fg);cursor:pointer;font:inherit' },
      el('span', { class: 'mono', style: 'font-size:12px;color:var(--fg2)' }, rowTime(e)),
      HN.statusPill(e.kind === 'undefined' ? 'warn' : 'idle', L(kind)),
      el('span', { style: 'font-size:12px' }, (CAUSE[causeBase] ? L(CAUSE[causeBase]) : causeBase) + (causeExtra ? ' (' + causeExtra + ')' : '')),
      (e.scope === 'live' || e.scope === 'both') ? HN.statusPill('warn', _L('live — 재시작 때 사라짐', 'live — gone on restart')) : null,
      el('span', { style: 'font-size:12px;color:var(--fg2)' }, actorText(e)),
      summary ? el('span', { class: 'mono', style: 'font-size:11px;color:var(--fg2)' }, summary) : null);
    btn.addEventListener('click', function () {
      var open = btn.getAttribute('aria-expanded') === 'true';
      btn.setAttribute('aria-expanded', open ? 'false' : 'true');
      if (open) { detail.hidden = true; return; }
      detail.hidden = false;
      if (state.cache[e.id]) { PCV.uxlib.clearEl(detail); detail.appendChild(detailNode(state.cache[e.id])); return; }
      PCV.uxlib.setMsg(detail, 'loading', null, _L('불러오는 중', 'Loading'));
      fetchGet(EP.VM_CONFIG_HISTORY_GET(e.id)).then(function (r) {
        var u = unwrap(r);
        if (u.err) { PCV.uxlib.setMsg(detail, 'err', null, u.err); return; }
        state.cache[e.id] = u.data;
        PCV.uxlib.clearEl(detail);
        detail.appendChild(detailNode(u.data));
      }).catch(function () { PCV.uxlib.setMsg(detail, 'err', null, _L('요청 실패', 'Request failed')); });
    });
    return el('li', { class: 'vmch-item', style: 'border-bottom:1px solid var(--border)' }, btn, detail);
  }

  function gapNode(g) {
    var reason = GAP[g.reason] ? L(GAP[g.reason]) : g.reason;
    return PCV.uxlib.el('li', { class: 'vmch-gap', style: 'border:1px dashed var(--border);border-radius:4px;margin:4px 0;' +
      'padding:4px 8px;font-size:12px;color:var(--fg2)' },
      _L('기록 공백 ', 'Recording gap ') + fmtTime(g.start) + '–' + fmtTime(g.end) + ' · ' + reason);
  }

  function appendMerged(list, entries, gaps, state) {
    var items = entries.map(function (e, i) { return { t: e.observed_at, node: entryNode(e, state, state.count + i) }; })
      .concat(gaps.map(function (g) { return { t: g.end, node: gapNode(g) }; }));
    items.sort(function (a, b) { return b.t - a.t; });
    items.forEach(function (it) { list.appendChild(it.node); });
    state.count += entries.length;
  }

  function storeLine(store) {
    var parts = [_L('보관 ', 'Retention ') + (store.retention_days || '?') + _L('일', ' days')];
    if (store.oldest_at) parts.push(_L('가장 오래된 기록 ', 'Oldest record ') + fmtTime(store.oldest_at));
    if (store.pruned_before) parts.push(_L('정리 경계 ', 'Pruned before ') + fmtTime(store.pruned_before));
    if (store.state === 'degraded') parts.push(_L('저장 지연 — 일부 기록이 늦게 쓰일 수 있음', 'Store degraded — records may be delayed'));
    if (store.state === 'over_cap') parts.push(_L('용량 상한 — 오래된 기록이 더 일찍 정리됨', 'Over capacity — older records pruned early'));
    return parts.join(' · ');
  }

  function emptyText(store) {
    if (store.state === 'disabled') return _L('구성 변경 이력이 꺼져 있습니다(daemon.conf [vm_config_history] enabled).', 'Configuration history is disabled (daemon.conf [vm_config_history] enabled).');
    if (store.state === 'unavailable') return _L('구성 변경 이력 저장소를 열 수 없습니다 — 데몬 로그의 [vm-cfg-hist] 줄을 확인하세요.', 'The configuration history store is unavailable — check [vm-cfg-hist] lines in the daemon log.');
    if (!store.oldest_at) return _L('아직 기록이 없습니다 — 데몬이 처음 VM을 확인하면 \'기록 시작\' 행이 생깁니다.', 'No records yet — a \'History start\' row appears when the daemon first checks the VM.');
    return _L('이 기간(최근 7일)에 기록된 구성 변경이 없습니다.', 'No configuration changes recorded in this period (last 7 days).');
  }

  function load(box, vm, state, cursor) {
    var q = { vm: vm.name, since: Date.now() - SPAN_MS, until: Date.now(), limit: 50 };
    if (cursor) q.cursor = cursor;
    if (!cursor) { PCV.uxlib.clearEl(state.list); PCV.uxlib.setMsg(state.status, 'loading', null, _L('불러오는 중', 'Loading')); state.count = 0; }
    state.more.hidden = true;
    return fetchGet(EP.VM_CONFIG_HISTORY(q)).then(function (r) {
      var u = unwrap(r);
      if (u.err) { PCV.uxlib.setMsg(state.status, 'err', null, u.err); return; }
      var d = u.data || {};
      var store = d.store || {};
      var entries = d.entries || [];
      appendMerged(state.list, entries, cursor ? [] : (d.gaps || []), state);
      if (!cursor && entries.length === 0) PCV.uxlib.setMsg(state.status, 'muted', { tag: 'div' }, emptyText(store));
      else if (!cursor) PCV.uxlib.setMsg(state.status, store.state === 'ok' ? 'muted' : 'warn', { tag: 'div', size: '11px' }, storeLine(store));
      state.cursor = d.next_cursor || null;
      state.more.hidden = !state.cursor;
    }).catch(function () { PCV.uxlib.setMsg(state.status, 'err', null, _L('요청 실패', 'Request failed')); });
  }

  var seq = 0;

  function render(container, vm) {
    var el = PCV.uxlib.el;
    var state = { seq: ++seq, cache: {}, count: 0, cursor: null };
    state.status = el('div', { class: 'vmch-status', 'aria-live': 'polite', style: 'margin:4px 0 6px' });
    state.list = el('ol', { class: 'vmch-list', style: 'list-style:none;margin:0;padding:0' });
    state.more = el('button', { type: 'button', class: 'btn btn-sm', hidden: 'hidden', style: 'margin-top:8px' }, _L('더 보기', 'Load more'));
    var refresh = el('button', { type: 'button', class: 'btn btn-sm' }, _L('새로고침', 'Refresh'));
    refresh.addEventListener('click', function () { load(container, vm, state, null); });
    state.more.addEventListener('click', function () { if (state.cursor) load(container, vm, state, state.cursor); });
    var card = HN.card(_L('구성 변경(서버 기록)', 'Configuration changes (server record)'),
      [el('div', { style: 'display:flex;gap:8px;align-items:center;flex-wrap:wrap' }, refresh, state.status), state.list, state.more],
      'vmch-card');
    PCV.uxlib.clearEl(container);
    container.appendChild(card);
    return load(container, vm, state, null);
  }

  PCV.vmConfigHistory = { render: render };
  window.PCV = PCV;
})(window.PCV = window.PCV || {});
