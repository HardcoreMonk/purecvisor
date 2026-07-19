
window.PCV = window.PCV || {};
(function(PCV) {

async function _rpc(method, params) {
  var body = { jsonrpc: '2.0', method: method, params: params || {}, id: 'sh-' + Date.now() };
  var r = await fetchPost(EP.RPC(), body);
  var d = unwrapData(r);
  if (d && d.error) throw new Error(d.error.message || d.error.code || 'RPC error');
  return d;
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
