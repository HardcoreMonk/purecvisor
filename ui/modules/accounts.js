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
          var actions;
          if (u.username !== 'admin') {
            actions = '<select id="role-' + escapeHtml(u.username) + '" aria-label="' + escapeHtml(u.username) + ' role" style="background:var(--bg);border:1px solid var(--border);color:var(--fg);border-radius:4px;padding:2px 6px;font-size:10px"><option ' + (u.role === 'viewer' ? 'selected' : '') + '>viewer</option><option ' + (u.role === 'operator' ? 'selected' : '') + '>operator</option><option ' + (u.role === 'admin' ? 'selected' : '') + '>admin</option></select> <button class="btn btn-xxs" onclick="acctRole(\'' + escapeHtml(u.username) + '\')">Set</button> <button class="btn btn-r btn-xxs" onclick="acctDel(\'' + escapeHtml(u.username) + '\')">' + t('btn.delete') + '</button>';
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
    h += H.card(t('btn.create') + ' User', '<div class="fr"><label for="acct-user">Username</label><input id="acct-user" placeholder="newuser"></div><div class="fr"><label for="acct-pass">Password</label><input id="acct-pass" type="password" placeholder="password"></div><div class="fr"><label for="acct-role">Role</label><select id="acct-role" style="width:100%;padding:6px;background:var(--bg);border:1px solid var(--border);color:var(--fg);border-radius:4px"><option>viewer</option><option selected>operator</option><option>admin</option></select></div><button class="btn btn-g mt-8 w-full" onclick="acctCreate()">' + t('btn.create') + ' User</button>');
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

/* ═══ API MANAGEMENT ═══ */
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
  h += H.card('<span class="color-accent">&#128272; JWT Token — Quick Test</span>', '<div class="flex gap-8 items-center mb-8 flex-wrap"><input aria-label="Username" id="apimgmt-user" value="admin" placeholder="Username" style="padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:6px;font-size:12px;width:140px"><input aria-label="Password" id="apimgmt-pass" type="password" value="admin" placeholder="Password" style="padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:6px;font-size:12px;width:140px"><button class="btn btn-g" onclick="apiMgmtGetToken()">&#9654; Get Token</button><button class="btn" onclick="apiMgmtTestHealth()">&#128994; Health Check</button></div><div id="apimgmt-token-result" class="stat-label" style="word-break:break-all;max-height:60px;overflow:auto"></div>', 'mb-14');
  h += H.card('<span class="color-green">&#128640; API Request Tester</span>', '<div class="flex gap-8 items-center mb-8 flex-wrap"><select id="apimgmt-method" aria-label="HTTP method" style="padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--accent);border-radius:6px;font-size:12px;font-weight:700"><option>GET</option><option>POST</option><option>PUT</option><option>DELETE</option></select><input id="apimgmt-path" aria-label="API endpoint path" value="/api/v1/vms" style="flex:1;min-width:200px;padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:6px;font-size:12px"><button class="btn btn-g" onclick="apiMgmtSend()">&#9654; Send</button></div><textarea aria-label="Request body (JSON)" id="apimgmt-body" placeholder="Request body (JSON)" rows="2" style="width:100%;padding:6px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:6px;font-size:11px;resize:vertical"></textarea><div id="apimgmt-result" style="background:var(--bg);border:1px solid var(--border);border-radius:6px;padding:10px;max-height:300px;overflow:auto;font-size:11px;color:var(--cyan);white-space:pre-wrap;display:none"></div>', 'mb-14');
  h += H.card('<span class="color-yellow">&#128268; gRPC Server</span>', '<div id="grpc-status" class="text-12 color-muted">Checking...</div><div style="margin-top:6px;font-size:11px;color:var(--fg2)">Port: 50051 | Protocol: protobuf-c binary framing<br>Transport: TCP (HTTP/2 planned)<br>Config: daemon.conf <code>[grpc] enabled=true</code></div>', 'mb-14');

  /* API Key Management */
  h += '<div class="hc mb-14"><h4>&#128273; API Keys</h4>';
  h += '<p class="stat-label" style="margin-bottom:10px">Create and manage API keys for programmatic access. Keys use the same RBAC as user tokens.</p>';
  h += '<div style="display:flex;gap:8px;flex-wrap:wrap;margin-bottom:12px">';
  h += '<input aria-label="Key description (e.g. CI pipeline)" id="apikey-desc" placeholder="Key description (e.g. CI pipeline)" style="flex:1;min-width:180px;padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:6px;font-size:12px">';
  h += '<input id="apikey-expiry" aria-label="Expiry (days)" type="number" value="90" min="1" max="365" style="width:80px;padding:6px 10px;background:var(--bg3);border:1px solid var(--border);color:var(--fg);border-radius:6px;font-size:12px" title="Expiry (days)">';
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
    if (!el) return;  /* DOM 교체된 경우 정상 종료 */
    el.innerHTML = H.badge('Config-based', 'y') + ' daemon.conf [grpc] enabled check required<br><code class="text-xs color-cyan">pcvctl grpc status</code> to verify from CLI';
  }, 100);
  setTimeout(() => {
    /* 모달/페이지 닫힌 후 호출 방지 */
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

/* ═══ AI AGENT ═══ */
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
    h += '<div class="hc mb-10"><h4 class="justify-between items-center"><span>' + ico + ' ' + escapeHtml(p.name) + '</span><label style="display:flex;align-items:center;gap:6px;cursor:pointer;font-size:10px"><input type="checkbox" id="agen' + i + '" ' + (p.enabled ? 'checked' : '') + ' style="accent-color:var(--accent)">' + (p.enabled ? '<span class="color-green">ENABLED</span>' : '<span class="color-muted">DISABLED</span>') + '</label></h4>';
    h += '<div class="fr"><label for="agm' + i + '">Model</label><input id="agm' + i + '" value="' + escapeAttr(p.model || '') + '" class="text-11"></div>';
    h += '<div class="fr"><label for="agk' + i + '">API Key</label><div class="flex gap-4 flex-1"><input id="agk' + i + '" type="password" value="' + escapeAttr(p.api_key || '') + '" class="text-11 flex-1"><button class="btn" onclick="toggleKeyVis(' + i + ')" style="font-size:10px;padding:4px 8px" id="agt' + i + '">Show</button></div></div>';
    h += '<div class="fr"><label for="age' + i + '">Endpoint</label><input id="age' + i + '" value="' + escapeAttr(p.endpoint || '') + '" class="text-11"></div>';
    h += '<div class="flex gap-6 mt-8"><button class="btn" onclick="testProvider(' + i + ',\'' + escapeAttr(p.name) + '\')" style="font-size:10px;padding:4px 10px">&#9889; Test</button>';
    h += (i === 0 ? '<button class="btn" onclick="testAllProviders()" style="font-size:10px;padding:4px 10px">&#9889; Test All</button>' : '');
    h += '<span id="agr' + i + '" class="text-11"></span></div></div>'; });
  h += '<div class="flex gap-8 justify-end mt-14"><button class="btn btn-g" onclick="saveAgentConfig()">' + t('btn.save') + ' All</button><button class="btn btn-r" onclick="closeModal()">' + t('btn.cancel') + '</button></div>';
  b.innerHTML = h;
}

function renderAgentSettings(b, d) {
  let h = '<div class="hc mb-10"><h4>&#9881; General ' + t('vm.settings') + '</h4>';
  h += '<div class="fr"><label for="ag-rate">Rate Limit</label><div class="flex gap-6 items-center flex-1"><input id="ag-rate" type="number" value="' + (d.rate_limit_sec || 300) + '" class="text-11 w-80"><span class="stat-label">seconds between queries</span></div></div>';
  h += '<div class="fr"><label for="ag-timeout">Timeout</label><div class="flex gap-6 items-center flex-1"><input id="ag-timeout" type="number" value="' + (d.timeout_sec || 10) + '" class="text-11 w-80"><span class="stat-label">seconds per request</span></div></div></div>';
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

/* ═══ API PERFORMANCE ═══ */
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

/* ═══ API ACTIVITY LOG ═══ */
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

/* ═══ SESSION MANAGEMENT (백엔드 4차) ═══ */
async function renderSessions(b) {
  b.innerHTML = showSkeleton();
  var h = H.section(_L('세션 관리', 'Session Management'));
  h += '<div class="mb-8">' + _L('활성 JWT 세션을 관리합니다.', 'Manage active JWT sessions.') + '</div>';
  h += '<div class="flex gap-8 mb-12">';
  h += '<input aria-label="' + _L('세션 JTI 입력', 'Enter session JTI') + '" id="revoke-jti" placeholder="' + _L('세션 JTI 입력', 'Enter session JTI') + '" class="input-field flex-1">';
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

/* ═══ API KEY FULL CRUD (백엔드 4차) ═══ */
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
  var html = '<div class="form-group"><label for="ak-name">' + _L('클라이언트 이름', 'Client Name') + '</label>';
  html += '<input id="ak-name" class="input-field" placeholder="grafana-scraper"></div>';
  html += '<div class="form-group"><label for="ak-role">' + _L('역할', 'Role') + '</label>';
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
