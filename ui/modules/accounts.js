/* ═══════════════════════════════════════════════════════════════
   PureCVisor — modules/accounts.js
   Account management, API management, AI Agent configuration
   ADR-0013: IIFE module scope — PCV.accounts namespace
   ═══════════════════════════════════════════════════════════════ */
/*
 * This module mixes three admin-facing surfaces: local accounts, API-key
 * management, and AI agent configuration. Access checks stay in the renderer
 * because the same routes can be reached through global search or restored tabs
 * after login state changes.
 *
 * Backend RBAC is still authoritative. The UI gate only avoids exposing account
 * editing controls to non-admin sessions and provides a stable fallback when
 * whoami is unavailable during early boot.
 */
window.PCV = window.PCV || {};
(function(PCV) {

var agentTab = 'providers';

/* ═══ ACCOUNTS ═══ */
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
  var mk = PCV.uxlib.el, clearEl = PCV.uxlib.clearEl;
  clearEl(b);
  b.appendChild(
    mk('div', { style: 'padding:40px;text-align:center' },
      mk('div', { style: 'font-size:44px;margin-bottom:12px' }, '🔒'),
      mk('h3', { style: 'color:var(--yellow);margin-bottom:10px' }, _L('관리자 전용 화면', 'Admin-only page')),
      mk('p', { class: 'color-muted', style: 'margin-bottom:14px' }, _L('계정과 API 키 관리는 admin 역할에서만 사용할 수 있습니다.', 'Account and API key management are available only to the admin role.')),
      mk('button', { class: 'btn', onclick: "navigateTo('dashboard')" }, _L('대시보드로 이동', 'Go to Dashboard'))));
}

async function renderAccounts(b) {
  if (!await isAdminAccountView()) {
    renderAdminOnlyNotice(b);
    return;
  }
  showSkeleton(b);
  try { const r = await fetchGet(EP.AUTH_USERS()); const l = unwrapList(r);
    var mk = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
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
          var actions;
          if (u.username !== 'admin') {
            /* 셀은 DataTable 노드 계약(9차) — 노드 배열로 조립, onclick은 문자열 유지 */
            actions = [
              mk('select', { id: 'role-' + u.username, 'aria-label': u.username + ' role', style: 'background:var(--bg);border:1px solid var(--border);color:var(--fg);border-radius:4px;padding:2px 6px;font-size:10px' },
                mk('option', u.role === 'viewer' ? { selected: '' } : null, 'viewer'),
                mk('option', u.role === 'operator' ? { selected: '' } : null, 'operator'),
                mk('option', u.role === 'admin' ? { selected: '' } : null, 'admin')),
              ' ',
              mk('button', { class: 'btn btn-xxs', onclick: "acctRole('" + escapeHtml(u.username) + "')" }, 'Set'),
              ' ',
              mk('button', { class: 'btn btn-r btn-xxs', onclick: "acctDel('" + escapeHtml(u.username) + "')" }, t('btn.delete'))
            ];
          } else {
            actions = mk('span', { class: 'stat-label' }, 'System admin');
          }
          return [
            mk('b', null, u.username),
            mk('span', { class: 'badge', style: 'border:1px solid ' + rc + ';color:' + rc }, u.role),
            u.tenant || '---',
            actions
          ];
        }),
        searchable: true,
        exportable: true,
        emptyText: 'No users'
      });
    }, 0);
    var heading = mk('h3', { class: 'mb-14' }, '👤 Account Management ', HN.badge('RBAC', 'y'));
    var card = HN.card(t('btn.create') + ' User', [
      mk('div', { class: 'fr' },
        mk('label', { for: 'acct-user' }, 'Username'),
        mk('input', { id: 'acct-user', placeholder: 'newuser' })),
      mk('div', { class: 'fr' },
        mk('label', { for: 'acct-pass' }, 'Password'),
        mk('input', { id: 'acct-pass', type: 'password', placeholder: 'password' })),
      mk('div', { class: 'fr' },
        mk('label', { for: 'acct-role' }, 'Role'),
        mk('select', { id: 'acct-role', style: 'width:100%;padding:6px;background:var(--bg);border:1px solid var(--border);color:var(--fg);border-radius:4px' },
          mk('option', null, 'viewer'),
          mk('option', { selected: '' }, 'operator'),
          mk('option', null, 'admin'))),
      mk('button', { class: 'btn btn-g mt-8 w-full', onclick: 'acctCreate()' }, t('btn.create') + ' User')
    ]);
    var grid = mk('div', { class: 'sg grid-2', style: 'gap:14px' },
      mk('div', null, mk('div', { id: 'acct-table' })),
      card);
    clearEl(b);
    b.appendChild(frag(heading, grid));
  } catch (e) { PCV.uxlib.setMsg(b, null, { tag: 'p', cls: 'color-red' }, 'Error loading accounts'); }
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

/* ═══ API MANAGEMENT ═══ */
async function renderApiManagement(b) {
  if (!await isAdminAccountView()) {
    renderAdminOnlyNotice(b);
    return;
  }
  var mk = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  var header = mk('div', { class: 'flex items-center gap-10 mb-16' },
    mk('span', { class: 'neon-blink color-yellow' }, '>>'),
    mk('h2', { style: 'font-family:var(--font-display);font-size:16px' }, 'API Management'));
  var statGrid = HN.grid(4,
    HN.card('🔌 Total Endpoints', mk('div', { class: 'stat-xl color-accent', id: 'api-ep-count' }, '...')),
    HN.card('🔒 Auth', mk('div', { class: 'stat-md color-green' }, 'JWT HS256')),
    HN.card('👥 RBAC', mk('div', { class: 'stat-md', style: 'color:var(--magenta)' }, '3 Levels')),
    HN.card('⚡ Rate Limit', mk('div', { class: 'stat-md color-yellow', id: 'api-rl-count' }, '...')));
  var spacer = mk('div', { class: 'mb-16' });
  /* /health에서 동적 데이터 로드 */
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
  var jwtCard = HN.card(
    mk('span', { class: 'color-accent' }, '🔐 JWT Token — Quick Test'),
    [
      mk('div', { class: 'flex gap-8 items-center mb-8 flex-wrap' },
        mk('input', { 'aria-label': 'Username', id: 'apimgmt-user', value: 'admin', placeholder: 'Username', style: 'padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:6px;font-size:12px;width:140px' }),
        mk('input', { 'aria-label': 'Password', id: 'apimgmt-pass', type: 'password', value: 'admin', placeholder: 'Password', style: 'padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:6px;font-size:12px;width:140px' }),
        mk('button', { class: 'btn btn-g', onclick: 'apiMgmtGetToken()' }, '▶ Get Token'),
        mk('button', { class: 'btn', onclick: 'apiMgmtTestHealth()' }, '🟢 Health Check')),
      mk('div', { id: 'apimgmt-token-result', class: 'stat-label', style: 'word-break:break-all;max-height:60px;overflow:auto' })
    ], 'mb-14');
  var testerCard = HN.card(
    mk('span', { class: 'color-green' }, '🚀 API Request Tester'),
    [
      mk('div', { class: 'flex gap-8 items-center mb-8 flex-wrap' },
        mk('select', { id: 'apimgmt-method', 'aria-label': 'HTTP method', style: 'padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--accent);border-radius:6px;font-size:12px;font-weight:700' },
          mk('option', null, 'GET'),
          mk('option', null, 'POST'),
          mk('option', null, 'PUT'),
          mk('option', null, 'DELETE')),
        mk('input', { id: 'apimgmt-path', 'aria-label': 'API endpoint path', value: '/api/v1/vms', style: 'flex:1;min-width:200px;padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:6px;font-size:12px' }),
        mk('button', { class: 'btn btn-g', onclick: 'apiMgmtSend()' }, '▶ Send')),
      mk('textarea', { 'aria-label': 'Request body (JSON)', id: 'apimgmt-body', placeholder: 'Request body (JSON)', rows: '2', style: 'width:100%;padding:6px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:6px;font-size:11px;resize:vertical' }),
      mk('div', { id: 'apimgmt-result', style: 'background:var(--bg);border:1px solid var(--border);border-radius:6px;padding:10px;max-height:300px;overflow:auto;font-size:11px;color:var(--cyan);white-space:pre-wrap;display:none' })
    ], 'mb-14');
  var grpcCard = HN.card(
    mk('span', { class: 'color-yellow' }, '🔌 gRPC Server'),
    [
      mk('div', { id: 'grpc-status', class: 'text-12 color-muted' }, 'Checking...'),
      mk('div', { style: 'margin-top:6px;font-size:11px;color:var(--fg2)' },
        'Port: 50051 | Protocol: protobuf-c binary framing',
        mk('br'),
        'Transport: TCP (HTTP/2 planned)',
        mk('br'),
        'Config: daemon.conf ',
        mk('code', null, '[grpc] enabled=true'))
    ], 'mb-14');

  /* API Key Management (R-embed) — 레거시 임베드 블록(부재 필드 참조 +
   * apikey-list-area 가드 오타로 영구 스피너)을 제거하고, 계약정합
   * renderApiKeys 로직을 실제 컨테이너(#apikey-keys-area)에 렌더한다.
   * 생성은 renderApiKeys 의 '+ 새 키 생성' → showApiKeyCreate 모달 경로. */
  var apiKeysBlock = mk('div', { class: 'hc mb-14', id: 'apikey-keys-area' });

  var navRow = mk('div', { class: 'flex gap-8 flex-wrap' },
    mk('button', { class: 'btn', onclick: "navigateTo('apihelp')" }, '📖 Swagger API'),
    mk('button', { class: 'btn', onclick: "navigateTo('restguide')" }, '📜 REST API Guide'),
    mk('button', { class: 'btn', onclick: "navigateTo('accounts')" }, '👤 Accounts'));
  clearEl(b);
  b.appendChild(frag(header, statGrid, spacer, jwtCard, testerCard, grpcCard, apiKeysBlock, navRow));
  setTimeout(() => {
    const el = document.getElementById('grpc-status');
    if (!el) return;  /* DOM 교체된 경우 정상 종료 */
    clearEl(el);
    el.appendChild(frag(
      HN.badge('Config-based', 'y'),
      ' daemon.conf [grpc] enabled check required',
      mk('br'),
      mk('code', { class: 'text-xs color-cyan' }, 'pcvctl grpc status'),
      ' to verify from CLI'));
  }, 100);
  /* 계약정합 키 테이블 렌더 (R-embed). renderApiKeys 가 자체적으로
   * fetch → 컨테이너 clear/append 하므로 apiKeysBlock 에 직접 그린다. */
  renderApiKeys(apiKeysBlock);
}

async function apiMgmtGetToken() { const u = document.getElementById('apimgmt-user').value, p = document.getElementById('apimgmt-pass').value; const el = document.getElementById('apimgmt-token-result');
  var mk = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  try { const r = await fetch(EP.AUTH_TOKEN(), { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ username: u, password: p }) }); const d = await r.json();
    if (d.access_token) { clearEl(el); el.appendChild(frag(mk('span', { class: 'color-green' }, '✅ Token:'), ' ', mk('code', { class: 'color-accent' }, d.access_token.substring(0, 50) + '...'))); }
    else { PCV.uxlib.setMsg(el, null, { cls: 'color-red' }, '❌ ' + JSON.stringify(d)); }
  } catch (e) { PCV.uxlib.setMsg(el, null, { cls: 'color-red' }, '❌ ' + e.message); } }

async function apiMgmtTestHealth() { const el = document.getElementById('apimgmt-token-result');
  var mk = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  try { const r = await fetchGet(EP.HEALTH()); const d = unwrapData(r); clearEl(el); el.appendChild(frag(mk('span', { class: 'color-green' }, '✅ Status: ' + (d.status || 'ok')), ' | edition: ' + (window.PCV_UI_EDITION || 'single'))); } catch (e) { PCV.uxlib.setMsg(el, null, { cls: 'color-red' }, '❌ ' + e.message); } }

async function apiMgmtSend() { const m = document.getElementById('apimgmt-method').value, path = document.getElementById('apimgmt-path').value; const body = document.getElementById('apimgmt-body').value, el = document.getElementById('apimgmt-result');
  var mk = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  el.style.display = 'block'; el.textContent = t('loading');
  try { const opts = { method: m, headers: {} }; if (authToken) opts.headers['Authorization'] = 'Bearer ' + authToken;
    if ((m === 'POST' || m === 'PUT') && body) { opts.headers['Content-Type'] = 'application/json'; opts.body = body; }
    const r = await fetch(location.origin + path, opts); const txt = await r.text(); let pretty = txt; try { pretty = JSON.stringify(JSON.parse(txt), null, 2); } catch (e) { if(_DEBUG) console.warn('apiMgmtSend:', e.message); }
    clearEl(el);
    el.appendChild(frag(
      mk('div', { class: 'mb-6' },
        HN.badge(String(r.status), r.ok ? 'g' : 'r'),
        ' ',
        mk('span', { class: 'color-muted' }, m + ' ' + path)),
      mk('pre', { style: 'white-space:pre-wrap' }, pretty)));
  } catch (e) { PCV.uxlib.setMsg(el, null, { cls: 'color-red' }, 'Error: ' + e.message); } }

/* ═══ AI AGENT ═══ */
async function showAgentConfig() {
  try { const r = await fetchGet(EP.AGENT_CONFIG()); const d = unwrapData(r); window._agentCfg = d;
    var el = PCV.uxlib.el;
    var tabs = ['providers', 'settings', 'history', 'status'].map(t2 => el('div', {
      onclick: 'agentTab=\'' + t2 + '\';showAgentConfig()',
      style: 'padding:8px 16px;font-size:12px;cursor:pointer;border-bottom:2px solid ' + (agentTab === t2 ? 'var(--accent)' : 'transparent') + ';color:' + (agentTab === t2 ? 'var(--accent)' : 'var(--fg2)') + ';font-weight:' + (agentTab === t2 ? '600' : '400')
    }, { providers: 'Providers', settings: t('vm.settings'), history: 'History', status: 'Status' }[t2]));
    showModal([
      el('h2', null, '🤖 AI Agent Configuration'),
      el('div', { class: 'flex', style: 'border-bottom:1px solid var(--border);margin-bottom:12px;gap:2px' }, tabs),
      el('div', { id: 'agent-tab-body' })
    ]);
    setTimeout(() => renderAgentTab(d), 50);
  } catch (e) { toast('Failed to load agent config: ' + e.message, false); }
}
function renderAgentTab(d) { const b = document.getElementById('agent-tab-body'); if (!b) return; if (agentTab === 'providers') renderAgentProviders(b, d); else if (agentTab === 'settings') renderAgentSettings(b, d); else if (agentTab === 'history') renderAgentHistory(b); else if (agentTab === 'status') renderAgentStatus(b, d); }

function renderAgentProviders(b, d) {
  var mk = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  const provs = d.providers || [];
  var cards = provs.map((p, i) => {
    const ico = { Claude: '🤖', OpenAI: '⚡', Gemini: '💎', Ollama: '🐚' }[p.name] || '⚙';
    return mk('div', { class: 'hc mb-10' },
      mk('h4', { class: 'justify-between items-center' },
        mk('span', null, ico + ' ' + p.name),
        mk('label', { style: 'display:flex;align-items:center;gap:6px;cursor:pointer;font-size:10px' },
          mk('input', { type: 'checkbox', id: 'agen' + i, checked: p.enabled ? '' : null, style: 'accent-color:var(--accent)' }),
          p.enabled ? mk('span', { class: 'color-green' }, 'ENABLED') : mk('span', { class: 'color-muted' }, 'DISABLED'))),
      mk('div', { class: 'fr' },
        mk('label', { for: 'agm' + i }, 'Model'),
        mk('input', { id: 'agm' + i, value: p.model || '', class: 'text-11' })),
      mk('div', { class: 'fr' },
        mk('label', { for: 'agk' + i }, 'API Key'),
        mk('div', { class: 'flex gap-4 flex-1' },
          mk('input', { id: 'agk' + i, type: 'password', value: p.api_key || '', class: 'text-11 flex-1' }),
          mk('button', { class: 'btn', onclick: 'toggleKeyVis(' + i + ')', style: 'font-size:10px;padding:4px 8px', id: 'agt' + i }, 'Show'))),
      mk('div', { class: 'fr' },
        mk('label', { for: 'age' + i }, 'Endpoint'),
        mk('input', { id: 'age' + i, value: p.endpoint || '', class: 'text-11' })),
      mk('div', { class: 'flex gap-6 mt-8' },
        mk('button', { class: 'btn', onclick: "testProvider(" + i + ",'" + escapeAttr(p.name) + "')", style: 'font-size:10px;padding:4px 10px' }, '⚡ Test'),
        i === 0 ? mk('button', { class: 'btn', onclick: 'testAllProviders()', style: 'font-size:10px;padding:4px 10px' }, '⚡ Test All') : null,
        mk('span', { id: 'agr' + i, class: 'text-11' })));
  });
  var footer = mk('div', { class: 'flex gap-8 justify-end mt-14' },
    mk('button', { class: 'btn btn-g', onclick: 'saveAgentConfig()' }, t('btn.save') + ' All'),
    mk('button', { class: 'btn btn-r', onclick: 'closeModal()' }, t('btn.cancel')));
  clearEl(b);
  b.appendChild(frag(cards, footer));
}

function renderAgentSettings(b, d) {
  var mk = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  const en = ((d.providers || []).filter(p => p.enabled).length);
  var general = mk('div', { class: 'hc mb-10' },
    mk('h4', null, '⚙ General ' + t('vm.settings')),
    mk('div', { class: 'fr' },
      mk('label', { for: 'ag-rate' }, 'Rate Limit'),
      mk('div', { class: 'flex gap-6 items-center flex-1' },
        mk('input', { id: 'ag-rate', type: 'number', value: (d.rate_limit_sec || 300), class: 'text-11 w-80' }),
        mk('span', { class: 'stat-label' }, 'seconds between queries'))),
    mk('div', { class: 'fr' },
      mk('label', { for: 'ag-timeout' }, 'Timeout'),
      mk('div', { class: 'flex gap-6 items-center flex-1' },
        mk('input', { id: 'ag-timeout', type: 'number', value: (d.timeout_sec || 10), class: 'text-11 w-80' }),
        mk('span', { class: 'stat-label' }, 'seconds per request'))));
  var stats = mk('div', { class: 'hc mb-10' },
    mk('h4', null, '📊 Statistics'),
    HN.row('Total Queries', mk('span', { class: 'color-accent' }, (d.total_queries || 0))),
    HN.row('Active Providers', mk('span', { class: 'color-green' }, en + ' / ' + (d.providers || []).length)));
  var footer = mk('div', { class: 'flex gap-8 justify-end mt-14' },
    mk('button', { class: 'btn btn-g', onclick: 'saveAgentSettings()' }, t('btn.save')),
    mk('button', { class: 'btn btn-r', onclick: 'closeModal()' }, t('btn.cancel')));
  clearEl(b);
  b.appendChild(frag(general, stats, footer));
}

async function renderAgentHistory(b) {
  var mk = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  PCV.uxlib.setMsg(b, 'loading', { tag: 'div', cls: 'text-center p-20' }, t('loading'));
  try { const r = await fetchGet(EP.AGENT_HISTORY()); const d = unwrapData(r);
    if (!d || !d.consensus) { clearEl(b); b.appendChild(HN.card('📊 History', mk('p', { class: 'color-muted text-12 mt-8' }, 'No data yet.'))); return; }
    var consensusRows = [
      HN.row('Action', mk('span', { class: 'color-accent' }, d.consensus)),
      HN.row('Confidence', mk('span', { class: 'color-green' }, (d.confidence || 0).toFixed(2))),
      HN.row('Avg Latency', (d.avg_latency_ms || 0).toFixed(0) + ' ms')
    ];
    if (d.timestamp) consensusRows.push(HN.row('Timestamp', mk('span', { class: 'text-xs' }, new Date(d.timestamp * 1000).toLocaleString())));
    var parts = [mk('div', { class: 'hc mb-10' }, mk('h4', null, '📊 Last Consensus Result'), consensusRows)];
    if (d.providers && d.providers.length) {
      var provRows = d.providers.map(p =>
        mk('tr', null,
          mk('td', null, mk('b', null, p.provider), mk('br'), mk('span', { class: 'stat-label' }, p.model)),
          mk('td', null, p.action),
          mk('td', null, (p.target_vm || '-'), p.from_node ? [' ', mk('span', { class: 'color-muted' }, p.from_node + '→' + p.to_node)] : null),
          mk('td', { class: 'color-green' }, (p.confidence || 0).toFixed(2)),
          mk('td', null, (p.latency_ms || 0).toFixed(0) + 'ms'),
          mk('td', null, HN.badge(p.urgency || '-', p.urgency === 'high' ? 'r' : p.urgency === 'medium' ? 'y' : 'g')),
          mk('td', null, p.success ? mk('span', { class: 'color-green' }, 'OK') : mk('span', { class: 'color-red' }, p.error || 'FAIL'))));
      var provTable = mk('table', null,
        mk('thead', null, mk('tr', null,
          mk('th', null, 'Provider'), mk('th', null, 'Action'), mk('th', null, 'Target'),
          mk('th', null, 'Confidence'), mk('th', null, 'Latency'), mk('th', null, 'Urgency'), mk('th', null, 'Status'))),
        mk('tbody', null, provRows));
      parts.push(HN.card('🤖 Per-Provider Results', provTable));
      if (d.providers[0] && d.providers[0].reason) {
        var reasoningKids = [mk('h4', null, '💬 Reasoning')];
        d.providers.forEach(p => { if (p.reason) reasoningKids.push(mk('div', { class: 'mb-6' }, mk('b', { class: 'color-accent' }, p.provider + ':'), ' ', mk('span', { class: 'text-11' }, p.reason))); });
        parts.push(mk('div', { class: 'hc mt-10' }, reasoningKids));
      }
    }
    clearEl(b);
    b.appendChild(frag(parts));
  } catch (e) { clearEl(b); b.appendChild(HN.card('', mk('p', { class: 'color-red' }, 'Failed: ' + e.message))); }
}

function renderAgentStatus(b, d) {
  var mk = PCV.uxlib.el, clearEl = PCV.uxlib.clearEl;
  const provs = d.providers || []; const en = provs.filter(p => p.enabled);
  var statusCard = HN.card('🟢 Agent Status', [
    mk('div', { class: 'stat-xl', style: 'color:' + (en.length > 0 ? 'var(--green)' : 'var(--fg2)') + ';margin:8px 0' }, en.length > 0 ? 'ACTIVE' : 'INACTIVE'),
    HN.row('Enabled Providers', en.length + ' / ' + provs.length),
    HN.row('Total Queries', d.total_queries || 0)
  ]);
  var provCards = provs.map(p => {
    const ico = { Claude: '🤖', OpenAI: '⚡', Gemini: '💎', Ollama: '🐚' }[p.name] || '⚙';
    return HN.card(ico + ' ' + p.name, [
      mk('div', { style: 'font-size:14px;font-weight:700;color:' + (p.enabled ? 'var(--green)' : 'var(--fg2)') + ';margin:4px 0' }, p.enabled ? 'ONLINE' : 'OFFLINE'),
      HN.row('Model', mk('span', { class: 'text-xs' }, p.model || '-')),
      HN.row('API Key', mk('span', { class: 'text-xs' }, p.api_key && p.api_key !== '' ? 'Configured' : 'Not set'))
    ]);
  });
  clearEl(b);
  b.appendChild(mk('div', { class: 'sg' }, statusCard, provCards));
}

function toggleKeyVis(i) { const el = document.getElementById('agk' + i); const bt = document.getElementById('agt' + i); if (el.type === 'password') { el.type = 'text'; bt.textContent = 'Hide'; } else { el.type = 'password'; bt.textContent = 'Show'; } }

async function testProvider(i, name) {
  var mk = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  const rEl = document.getElementById('agr' + i); const key = document.getElementById('agk' + i).value; const model = document.getElementById('agm' + i).value; const endpoint = document.getElementById('age' + i).value;
  if (!key || key.startsWith('***')) { PCV.uxlib.setMsg(rEl, null, { cls: 'color-red' }, 'No API key set'); return; }
  PCV.uxlib.setMsg(rEl, 'loading', null, 'Testing...');
  try { let ok = false, detail = ''; const t0 = performance.now();
    if (name === 'Claude') { const r = await fetch(endpoint || 'https://api.anthropic.com/v1/messages', { method: 'POST', headers: { 'Content-Type': 'application/json', 'x-api-key': key, 'anthropic-version': '2023-06-01' }, body: JSON.stringify({ model: model || 'claude-sonnet-4-20250514', max_tokens: 1, messages: [{ role: 'user', content: 'ping' }] }) }); ok = r.ok || r.status === 400; detail = r.ok ? 'OK' : 'HTTP ' + r.status; }
    else if (name === 'OpenAI') { const r = await fetch((endpoint || 'https://api.openai.com/v1') + '/models', { headers: { Authorization: 'Bearer ' + key } }); ok = r.ok; detail = r.ok ? 'OK' : 'HTTP ' + r.status; }
    else if (name === 'Gemini') { const r = await fetch((endpoint || 'https://generativelanguage.googleapis.com/v1beta') + '/models?key=' + key); ok = r.ok; detail = r.ok ? 'OK' : 'HTTP ' + r.status; }
    else if (name === 'Ollama') { const r = await fetch((endpoint || 'http://localhost:11434') + '/api/tags'); ok = r.ok; detail = r.ok ? 'OK' : 'HTTP ' + r.status; }
    const ms = Math.round(performance.now() - t0);
    if (ok) { clearEl(rEl); rEl.appendChild(frag(HN.badge('Connected', 'g'), ' ', mk('span', { class: 'stat-label' }, ms + 'ms'))); }
    else { clearEl(rEl); rEl.appendChild(frag(HN.badge('Failed', 'r'), ' ', mk('span', { class: 'stat-label' }, detail))); }
  } catch (e) { clearEl(rEl); rEl.appendChild(frag(HN.badge('Error', 'r'), ' ', mk('span', { class: 'text-xs color-red' }, e.message))); }
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

/* ═══ API PERFORMANCE ═══ */
async function renderApiPerf(b) {
  var mk = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  showSkeleton(b);
  var endpoints = ['/vms', '/containers', '/networks', '/storage/pools', '/health', '/alerts', '/processes'];
  var results = [];

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
  var grid = mk('div', { class: 'sg grid-3' },
    HN.card(_L('평균 응답', 'Average'), mk('div', { class: 'stat-lg ' + (avg < 100 ? 'color-green' : avg < 500 ? 'color-yellow' : 'color-red') }, avg.toFixed(0) + 'ms')),
    HN.card(_L('최고', 'Fastest'), mk('div', { class: 'stat-lg color-green' }, Math.min.apply(null, results.map(function(r){return r.time;})) + 'ms')),
    HN.card(_L('최저', 'Slowest'), mk('div', { class: 'stat-lg color-red' }, Math.max.apply(null, results.map(function(r){return r.time;})) + 'ms')));

  results.sort(function(a, b) { return a.time - b.time; });
  var rows = results.map(function(r) {
    var grade = r.time < 50 ? 'A+' : r.time < 100 ? 'A' : r.time < 300 ? 'B' : r.time < 500 ? 'C' : 'D';
    var gradeColor = r.time < 100 ? 'g' : r.time < 300 ? 'y' : 'r';
    return mk('tr', null,
      mk('td', null, mk('code', null, r.endpoint)),
      mk('td', null, mk('b', null, r.time + 'ms')),
      mk('td', null, HN.badge(r.status, r.status === 'ok' ? 'g' : 'r')),
      mk('td', null, HN.badge(grade, gradeColor)));
  });
  var table = mk('table', { class: 'text-12 mt-12' },
    mk('thead', null, mk('tr', null,
      mk('th', null, 'Endpoint'),
      mk('th', null, _L('응답 시간', 'Response')),
      mk('th', null, _L('상태', 'Status')),
      mk('th', null, _L('등급', 'Grade')))),
    mk('tbody', null, rows));
  var rerunBtn = mk('button', { class: 'btn mt-12', onclick: "renderApiPerf(document.getElementById('cb'))" }, '🔄 ' + _L('재측정', 'Re-run'));
  var benchBtn = mk('button', { class: 'btn', onclick: 'runApiBenchmark()', style: 'margin-left:8px' }, '⚡ ' + _L('벤치마크', 'Benchmark') + ' (5x)');
  clearEl(b);
  b.appendChild(frag(HN.section(_L('API 응답 시간', 'API Response Times')), grid, table, rerunBtn, benchBtn));
}
window.renderApiPerf = renderApiPerf;

async function runApiBenchmark() {
  var endpoints = ['/vms', '/containers', '/networks', '/storage/pools', '/health', '/alerts'];
  var iterations = 5;
  var el = PCV.uxlib.el;
  /* 원본 prog-fill div 는 class 속성이 중복(class="prog-fill"..class="w-pct-0")이라
   * HTML 파서상 첫 class="prog-fill" 만 적용되고 w-pct-0 는 무시됨 — 렌더 동등 보존.
   * (진행률은 done 루프에서 bench-prog.style.width 로 직접 설정.) */
  showModal([
    el('h2', null, '⚡ ' + _L('벤치마크 실행 중', 'Running Benchmark')),
    el('div', { class: 'prog-bar' }, el('div', { class: 'prog-fill', id: 'bench-prog' })),
    el('div', { id: 'bench-st', class: 'prog-status' }, el('span', { class: 'spinner' }), ' 0/' + (endpoints.length * iterations))
  ]);
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
      if (ps) PCV.uxlib.setMsg(ps, 'loading', null, done + '/' + total + ' — ' + endpoints[i]);
    }
  }
  var mk = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  var rows = Object.keys(results).map(function(ep) {
    var times = results[ep].sort(function(a,b){return a-b;});
    var avg = Math.round(times.reduce(function(s,t2){return s+t2;},0) / times.length);
    var p95 = times[Math.floor(times.length * 0.95)] || times[times.length-1];
    return mk('tr', null,
      mk('td', null, mk('code', null, ep)),
      mk('td', null, mk('b', null, avg + 'ms')),
      mk('td', { class: 'color-green' }, times[0] + 'ms'),
      mk('td', { class: 'color-red' }, times[times.length-1] + 'ms'),
      mk('td', null, p95 + 'ms'));
  });
  var table = mk('table', { class: 'text-12' },
    mk('thead', null, mk('tr', null,
      mk('th', null, 'Endpoint'), mk('th', null, 'Avg'), mk('th', null, 'Min'), mk('th', null, 'Max'), mk('th', null, 'P95'))),
    mk('tbody', null, rows));
  var footer = mk('div', { class: 'text-right mt-12' }, mk('button', { class: 'btn', onclick: 'closeModal()' }, t('btn.close')));
  var mc = document.getElementById('mc');
  if (mc) { clearEl(mc); mc.appendChild(frag(mk('h2', null, '⚡ ' + _L('벤치마크 결과', 'Benchmark Results')), table, footer)); }
}
window.runApiBenchmark = runApiBenchmark;

/* ═══ API ACTIVITY LOG ═══ */
function renderActivityLog(b) {
  var mk = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  var log = (eventLog || []).filter(function(e) { return e && e.msg; });
  var toolbar = mk('div', { class: 'flex gap-6 mb-8' },
    mk('span', { class: 'color-muted text-xs' }, log.length + ' ' + _L('건', 'requests')),
    mk('button', { class: 'btn btn-xs', onclick: "window.eventLog=[];renderActivityLog(document.getElementById('cb'))" }, 'Clear'));
  var body;
  if (log.length === 0) {
    body = mk('div', { class: 'empty-state p-20 text-center' },
      mk('div', { style: 'font-size:36px;opacity:.5' }, '📄'),
      mk('div', { class: 'color-muted' }, _L('기록 없음', 'No activity yet')));
  } else {
    var rows = log.slice().reverse().map(function(l) {
      var timeStr = l.ts ? new Date(l.ts).toLocaleTimeString() : '';
      var msg = l.msg || String(l);
      var isApi = msg.includes('API') || msg.includes('fetch') || msg.includes('GET') || msg.includes('POST') || msg.includes('Auth') || msg.includes('WS');
      return mk('tr', null,
        mk('td', { class: 'color-muted' }, timeStr),
        mk('td', null, mk('span', isApi ? { class: 'color-accent' } : null, msg)));
    });
    body = mk('table', { class: 'text-11' },
      mk('thead', null, mk('tr', null,
        mk('th', null, _L('시각', 'Time')),
        mk('th', null, _L('이벤트', 'Event')))),
      mk('tbody', null, rows));
  }
  clearEl(b);
  b.appendChild(frag(HN.section(_L('API 활동 로그', 'API Activity Log')), toolbar, body));
}
window.renderActivityLog = renderActivityLog;

/* ═══ SESSION MANAGEMENT (백엔드 4차) ═══ */
async function renderSessions(b) {
  var mk = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  showSkeleton(b);
  var controls = mk('div', { class: 'flex gap-8 mb-12' },
    mk('input', { 'aria-label': _L('세션 JTI 입력', 'Enter session JTI'), id: 'revoke-jti', placeholder: _L('세션 JTI 입력', 'Enter session JTI'), class: 'input-field flex-1' }),
    mk('button', { class: 'btn btn-r', onclick: 'revokeSession()', 'aria-label': _L('세션 강제 해제', 'Revoke session') }, _L('강제 해제', 'Force Logout')));
  var empty = mk('div', { class: 'empty-state p-20 text-center' },
    mk('div', { style: 'font-size:36px;opacity:.5' }, '🔒'),
    mk('div', { class: 'color-muted' }, _L('JTI를 입력하여 특정 세션을 무효화합니다', 'Enter JTI to revoke a specific session')));
  clearEl(b);
  b.appendChild(frag(
    HN.section(_L('세션 관리', 'Session Management')),
    mk('div', { class: 'mb-8' }, _L('활성 JWT 세션을 관리합니다.', 'Manage active JWT sessions.')),
    controls,
    empty));
}
async function revokeSession() {
  var jti = document.getElementById('revoke-jti');
  if (!jti || !jti.value.trim()) { toast(_L('JTI를 입력하세요', 'Enter JTI'), 'w'); return; }
  if (!await customConfirm(_L('이 세션을 강제 해제하시겠습니까?', 'Force logout this session?'))) return;
  try {
    const r = await fetchPost(EP.AUTH_SESSION_REVOKE(), { jti: jti.value.trim() });
    if (r && r.error) { toast(_L('실패', 'Failed') + ': ' + (r.error.message || ''), false); return; }
    toast(_L('세션이 해제되었습니다', 'Session revoked'), 's');
    jti.value = '';
  } catch(e) { toast(_L('실패', 'Failed') + ': ' + (e.message || ''), false); }
}

/* ═══ API KEY FULL CRUD (백엔드 4차) ═══ */
async function renderApiKeys(b) {
  showSkeleton(b);
  try {
    var r = await fetchGet(EP.AUTH_APIKEY_LIST());
    var list = unwrapList(r);
    var mk = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
    var createBtn = mk('button', { class: 'btn mb-8', onclick: 'showApiKeyCreate()', 'aria-label': _L('새 API 키 생성', 'Create new API key') }, '+ ' + _L('새 키 생성', 'New Key'));
    var body;
    if (list.length === 0) {
      body = mk('div', { class: 'empty-state p-20 text-center' },
        mk('div', { style: 'font-size:36px;opacity:.5' }, '🔑'),
        mk('div', { class: 'color-muted' }, _L('등록된 API 키가 없습니다', 'No API keys registered')));
    } else {
      var rows = list.map(function(k) {
        var st = HN.badge(k.revoked ? _L('폐기', 'Revoked') : _L('활성', 'Active'), k.revoked ? 'r' : 'g');
        /* expires_at 은 epoch 초, 0/부재 = 무기한. new Date() 는 ms 기준이므로 *1000. */
        var expCell;
        if (!k.expires_at) {
          expCell = mk('td', { class: 'color-muted' }, _L('무기한', 'Never'));
        } else if (k.expires_at * 1000 < Date.now()) {
          expCell = mk('td', null, HN.badge(_L('만료', 'Expired'), 'r'));
        } else {
          expCell = mk('td', { class: 'color-muted' }, new Date(k.expires_at * 1000).toLocaleDateString());
        }
        return mk('tr', null,
          mk('td', null, mk('b', null, k.client_name)),
          mk('td', null, ['viewer','operator','admin'][k.role] || '?'),
          mk('td', { class: 'color-muted' }, k.created_at || ''),
          mk('td', { class: 'color-muted' }, k.last_used_at || _L('미사용', 'Never')),
          expCell,
          mk('td', null, st),
          mk('td', null, k.revoked ? null : mk('button', { class: 'btn btn-r btn-xxs', onclick: function() { revokeApiKey(k.client_name); }, 'aria-label': _L('키 폐기', 'Revoke key') }, _L('폐기', 'Revoke'))));
      });
      body = mk('table', { class: 'data-table text-11' },
        mk('thead', null, mk('tr', null,
          mk('th', null, _L('클라이언트', 'Client')),
          mk('th', null, _L('역할', 'Role')),
          mk('th', null, _L('생성일', 'Created')),
          mk('th', null, _L('최종 사용', 'Last Used')),
          mk('th', null, _L('만료', 'Expiry')),
          mk('th', null, _L('상태', 'Status')),
          mk('th'))),
        mk('tbody', null, rows));
    }
    clearEl(b);
    b.appendChild(frag(HN.section(_L('API 키 관리', 'API Key Management')), createBtn, body));
  } catch(e) { PCV.uxlib.setMsg(b, null, { tag: 'p', cls: 'color-muted' }, _L('로드 실패', 'Failed')); }
}
/* 폼(이름 + 설명 + 역할 + 만료일)을 노드로 구성하고, 확인 버튼은 app.js 의 표준
 * apiKeyCreate() 를 재사용한다 — apiKeyCreate 가 #apikey-name / #apikey-desc /
 * #apikey-role / #apikey-expiry 를 읽어 POST 후 결과 키를 #apikey-new-result 에
 * 인라인 렌더한다 (중복 구현 금지). 생성된 키가 모달 안에 노출되도록
 * #apikey-new-result 컨테이너를 포함.
 * BE 계약: name(필수, client_name)+role(옵션, default 1, {0,1,2} 且 role≤caller)+
 * description(옵션)+expires_at(옵션, epoch 초). SEC-3: 저장 role 이 실효 grant 이므로
 * caller 역할을 초과하는 옵션은 노출하지 않는다(whoami 로 caller 역할 취득, 기본 operator). */
async function showApiKeyCreate() {
  var mk = PCV.uxlib.el;
  var ROLE_KEYS = ['viewer', 'operator', 'admin'];
  var ROLE_LABELS = [
    _L('뷰어 (읽기)', 'viewer (read)'),
    _L('오퍼레이터 (읽기/쓰기)', 'operator (read/write)'),
    _L('관리자 (전체)', 'admin (full)')
  ];
  var callerLvl = ROLE_KEYS.indexOf(await getCurrentAccountRole());
  if (callerLvl < 0) callerLvl = 0;              /* 미상 → 최소 권한만 허용 */
  var defaultLvl = Math.min(1, callerLvl);       /* 기본 operator, caller 초과 금지 */
  var roleSelect = mk('select', { id: 'apikey-role', 'aria-label': _L('키 역할', 'Key role') });
  for (var lvl = 0; lvl <= callerLvl; lvl++) {
    roleSelect.appendChild(mk('option', { value: String(lvl), selected: lvl === defaultLvl ? 'selected' : false }, ROLE_LABELS[lvl]));
  }
  showModal([
    mk('h2', null, _L('API 키 생성', 'Create API Key')),
    mk('div', { class: 'fr' },
      mk('label', { for: 'apikey-name' }, _L('이름', 'Name')),
      mk('input', { id: 'apikey-name', placeholder: _L('예: ci-파이프라인', 'e.g. ci-pipeline') })),
    mk('div', { class: 'fr' },
      mk('label', { for: 'apikey-desc' }, _L('설명', 'Description')),
      mk('input', { id: 'apikey-desc', placeholder: _L('예: CI 배포 자동화', 'e.g. CI deploy automation') })),
    mk('div', { class: 'fr' },
      mk('label', { for: 'apikey-role' }, _L('역할', 'Role')),
      roleSelect),
    mk('div', { class: 'fr' },
      mk('label', { for: 'apikey-expiry' }, _L('만료 (일)', 'Expiry (days)')),
      mk('input', { id: 'apikey-expiry', type: 'number', value: '90', min: '1', max: '365' })),
    mk('div', { id: 'apikey-new-result', class: 'break-all text-11', style: 'display:none;margin:8px 0;padding:10px;border:1px solid var(--green);border-radius:6px;background:rgba(0,255,0,.04)' }),
    mk('div', { class: 'text-right mt-12' },
      mk('button', { class: 'btn btn-g', onclick: 'apiKeyCreate()' }, t('btn.create')),
      ' ',
      mk('button', { class: 'btn btn-r', onclick: 'closeModal()' }, t('btn.cancel')))
  ]);
}
async function revokeApiKey(name) {
  if (!await customConfirm(_L('이 API 키를 폐기하시겠습니까?', 'Revoke this API key?') + '\n' + name)) return;
  try {
    const r = await fetchPost(EP.AUTH_APIKEY_REVOKE(name), {});
    if (r && r.error) { toast(_L('실패', 'Failed') + ': ' + (r.error.message || ''), false); return; }
    toast(_L('키 폐기 완료', 'Key revoked'), 's');
    /* R-embed: 키 테이블은 API Management 페이지의 #apikey-keys-area 서브
     * 컨테이너에 렌더된다. cb(전체 페이지)로 다시 그리면 JWT/tester/gRPC 카드가
     * 사라지므로 반드시 서브 컨테이너로 리프레시한다. */
    var area = document.getElementById('apikey-keys-area');
    if (area) renderApiKeys(area);
  } catch(e) { toast(_L('실패', 'Failed'), false); }
}

/* ═══ WINDOW REGISTRATIONS ═══ */
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

/* ═══ PCV.accounts namespace export ═══ */
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
