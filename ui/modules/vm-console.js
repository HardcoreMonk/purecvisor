/* ═══════════════════════════════════════════════════════════════
   PureCVisor — modules/vm-console.js
   CONSOLE/VNC, Snapshots, Performance, Timeline, Compare
   ADR-0013: IIFE module scope — vm.js에서 분할 (pure-move)
   ═══════════════════════════════════════════════════════════════ */
window.PCV = window.PCV || {};
(function(PCV) {

/* ═══ CONSOLE / VNC ═══
 *  [VNC 연결 흐름]
 *    1. EP.VNC(name)으로 VNC 포트 조회 (백엔드: virDomainGetXMLDesc에서 추출)
 *    2. VM running + 포트 있으면 → 로컬 noVNC ESM import로 RFB 객체 생성
 *    3. WS URL: ws(s)://host/api/v1/ws/vnc?port=XXXX
 *       (ws_server.c가 WS 프레임 ↔ VNC TCP 패킷 변환)
 *    4. 팝업(openNoVNCPopup) vs 임베디드(openNoVNC) 두 가지 모드 지원
 *    5. <script type="module"> 동적 삽입으로 ESM import 수행 */
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
</script></body></html>`);
  w.document.close();
  toast(vmName + ' VNC ' + t('vnc.open_popup'));
}

function copyVncAddr(port) {
  const addr = location.hostname + ':' + port;
  navigator.clipboard.writeText(addr).then(() => toast(t('vnc.addr_copied') + ': ' + addr)).catch(() => toast(addr, true));
}

/* ═══ SNAPSHOTS ═══
 *  [백엔드 응답 형식]
 *    "pcvpool/vms/web-prod@snap-20260410\t2026-04-10 15:30:00" 형태의 문자열 배열.
 *    @ 뒤가 스냅샷 이름, \t 뒤가 생성 시간.
 *  [롤백 안전 장치]
 *    파괴적 작업이므로 VM 이름을 직접 타이핑해야 실행 가능 (rbValidate).
 *  [일괄 삭제]
 *    snapDeleteAll → prefix 필터 + keep_recent → 미리보기(sdaPreview) → 실행(sdaExec). */
async function renderSnapshots(b, v) {
  if (!v) return;
  b.innerHTML = '<div><div class="justify-between items-center mb-12"><h3>' + t('vm.snapshot') + ': ' + esc(v.name) + '</h3><div class="flex gap-6"><button class="btn btn-g" onclick="takeSnap()" class="text-12">+ ' + t('btn.create') + '</button><button class="btn btn-r" onclick="snapDeleteAll(\'' + escapeAttr(v.name) + '\')" class="text-12">&#128465; Delete All</button></div></div><div id="stree"><span class="spinner"></span> ' + t('loading') + '</div></div>';
  try {
    const r = await fetchGet(EP.VM_SNAPSHOT_LIST(v.name));
    const raw = unwrapList(r);
    /* 백엔드: "pool/vm@snapname\tdate" 문자열 배열 파싱 */
    const snaps = raw.map(s => {
      if (typeof s === 'string') {
        const [full, time] = s.split('\t');
        const atIdx = full.lastIndexOf('@');
        return { name: atIdx >= 0 ? full.substring(atIdx + 1) : full, full_path: full, time: time || '' };
      }
      return { name: s.name || s, full_path: s.name || s, time: s.creation_time || '' };
    });
    if (!snaps.length) { PCV.uxlib.setMsg('stree', 'muted', { tag: 'p' }, t('snap.none')); return; }
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
  } catch (e) { PCV.uxlib.setMsg('stree', 'err', { tag: 'p' }, e.message); }
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

  h += '<div class="mb-12"><label for="snap-name-input" class="text-12 font-600">Snapshot Name</label>';
  h += '<input id="snap-name-input" value="' + defaultName + '" class="w-full mt-4" oninput="snapNameValidate()" placeholder="alphanumeric, dash, underscore"></div>';
  h += '<div id="snap-name-err" style="font-size:11px;min-height:16px;margin-bottom:8px"></div>';

  h += '<div class="mb-14"><label for="snap-desc-input" class="text-12 font-600">Description <span class="color-muted">(optional)</span></label>';
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
    if (!n) PCV.uxlib.setMsg(err, 'err', null, 'Name is required');
    else if (!valid) PCV.uxlib.setMsg(err, 'err', null, 'Invalid characters (use a-z, 0-9, dash, underscore)');
    else PCV.uxlib.setMsg(err, 'ok', null, '✅ Valid');
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
  if (btn) { btn.disabled = true; PCV.uxlib.setMsg(btn, 'loading', null, 'Creating...'); }
  try {
    const r = await fetchPost(EP.VM_SNAPSHOT_CREATE(v.name), { snap_name: n });
    if (r && r.error) { toast('Create failed: ' + (r.error.message || ''), false); if (btn) { btn.disabled = false; PCV.uxlib.setMsg(btn, null, null, '📷 Retry'); } return; }
    toast(t('snap.created') + ': ' + n);
    addEvt('VM Snapshot created — ' + v.name + '@' + n);
    closeModal();
    renderSnapshots(document.getElementById('cb'), v);
  } catch (e) { toast('Error: ' + e.message, false); if (btn) { btn.disabled = false; PCV.uxlib.setMsg(btn, null, null, '📷 Retry'); } }
}

async function snapRb(vm, s) {
  const v = vmList.find(x => x.name === vm);
  const on = v && v.state === 'running';

  let h = '<h2 class="mb-14">&#9194; Rollback Snapshot</h2>';

  /* 경고 배너 */
  h += '<div style="margin-bottom:14px;padding:12px;border:1px solid var(--red);border-radius:6px;background:rgba(255,60,60,.06)">';
  h += '<div style="font-weight:700;color:var(--red);margin-bottom:6px">&#9888; Destructive Operation</div>';
  h += '<div class="text-xs color-muted">This will revert the VM disk to the snapshot point-in-time. <b>All data written after this snapshot will be permanently lost.</b></div>';
  if (on) h += '<div class="text-xs" style="color:var(--yellow);margin-top:6px">&#9889; VM is currently <b>running</b> — it will be <b>force-stopped</b> before rollback, then automatically restarted.</div>';
  h += '</div>';

  /* 상세 정보 */
  h += '<div class="mb-14 p-10 border-muted rounded-md">';
  h += '<div style="display:grid;grid-template-columns:100px 1fr;gap:4px 12px;font-size:12px">';
  h += '<span class="color-muted">VM</span><span><b>' + esc(vm) + '</b> ' + (on ? '<span class="color-green">Running</span>' : '<span class="color-muted">Stopped</span>') + '</span>';
  h += '<span class="color-muted">Snapshot</span><span><code>' + esc(s) + '</code></span>';
  h += '<span class="color-muted">ZFS Path</span><span class="text-xs"><code>' + esc(vm) + '@' + esc(s) + '</code></span>';
  h += '</div></div>';

  /* 확인 입력 */
  h += '<div class="mb-14"><label for="rb-confirm-input" class="text-12 font-600">Type VM name to confirm: <code>' + esc(vm) + '</code></label>';
  h += '<input id="rb-confirm-input" placeholder="' + esc(vm) + '" class="w-full mt-4" oninput="rbValidate(\'' + escapeAttr(vm) + '\')"></div>';

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
  if (btn) { btn.disabled = true; PCV.uxlib.setMsg(btn, 'loading', null, 'Rolling back...'); }
  try {
    const r = await fetchPost(EP.VM_SNAPSHOT_ROLLBACK(vm), { snap_name: s });
    if (r && r.error) { toast('Rollback failed: ' + (r.error.message || ''), false); if (btn) { btn.disabled = false; PCV.uxlib.setMsg(btn, null, null, '⏪ Retry'); } return; }
    toast('Rollback accepted: ' + vm + '@' + s);
    addEvt('VM Snapshot rollback — ' + vm + '@' + s);
    closeModal();
    renderSnapshots(document.getElementById('cb'), vmList[selectedVmIndex]);
  } catch (e) { toast('Error: ' + e.message, false); if (btn) { btn.disabled = false; PCV.uxlib.setMsg(btn, null, null, '⏪ Retry'); } }
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
  /* 1. 스냅샷 목록 조회 */
  let snaps;
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

  /* 2. 모달 UI */
  let h = '<h2 class="mb-12">&#128465; Bulk Delete Snapshots</h2>';
  h += '<div class="mb-12"><span class="color-muted">VM:</span> <b>' + esc(vm) + '</b> &mdash; <span class="color-accent">' + snaps.length + '</span> snapshots</div>';

  h += '<div class="mb-14 p-10 border-muted rounded-md">';
  h += '<div style="margin-bottom:8px;font-weight:600;font-size:12px">Options</div>';
  h += '<div class="mb-8"><label for="sda-prefix" class="text-12">Prefix filter <span class="color-muted">(empty = all)</span></label>';
  h += '<input id="sda-prefix" placeholder="e.g. pcv-repl-" class="w-full mt-4" oninput="sdaPreview()"></div>';
  h += '<div><label for="sda-keep" class="text-12">Keep recent</label>';
  h += '<input id="sda-keep" type="number" min="0" value="0" style="width:80px;margin-left:8px" oninput="sdaPreview()"> <span class="color-muted text-xs">snapshots</span></div>';
  h += '</div>';

  h += '<div id="sda-preview" style="margin-bottom:14px;padding:10px;border:1px solid var(--border);border-radius:6px;max-height:200px;overflow-y:auto;font-size:11px"></div>';

  h += '<div class="flex gap-6 justify-end">';
  h += '<button class="btn" onclick="closeModal()">Cancel</button>';
  h += '<button class="btn btn-r" id="sda-exec-btn" onclick="sdaExec(\'' + vm.replace(/'/g, "\\'") + '\')">&#128465; Delete <span id="sda-count">0</span> Snapshots</button>';
  h += '</div>';

  showModal(h);

  /* 스냅샷 데이터를 전역에 임시 저장 */
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

  /* 필터 적용 */
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
  if (btn) { btn.disabled = true; PCV.uxlib.setMsg(btn, 'loading', null, 'Deleting...'); }
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
  if (btn) { btn.disabled = false; PCV.uxlib.setMsg(btn, null, null, '🗑 Done'); }
}

/* ═══ PERFORMANCE ═══ */
var perfLayout = 'auto';

async function renderPerformance(b, v) {
  if (!v) return;
  /* 실시간 메트릭 조회 */
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

/* ═══ VM TIMELINE ═══ */
function renderTimeline(b, v) {
  if (!v) { PCV.uxlib.setMsg(b, 'muted', { tag: 'p' }, t('vm.select')); return; }
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
/* ═══ VM COMPARE ═══ */
async function showVmCompare() {
  if (checkedVms.size < 2) { toast(_L('비교할 VM을 2개 이상 선택하세요', 'Select 2+ VMs to compare'), false); return; }
  var selected = vmList.filter(function(v, idx) { return checkedVms.has(idx); }).slice(0, 4);
  /* 실시간 메트릭을 병합 */
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
/* ═══ EXPORT TO PCV NAMESPACE (ADR-0013) ═══
 *  PCV.vm에 등록되는 함수가 이 모듈의 공식 인터페이스.
 *  아래 BACKWARD COMPAT SHIMS는 HTML onclick과 다른 모듈의
 *  window.render() 등 직접 참조를 위한 전환기 코드.
 *  신규 코드에서는 PCV.vm.render() 사용을 권장. */
PCV.vm = Object.assign(PCV.vm || {}, {
  showVmCompare: showVmCompare,
  renderConsole: renderConsole,
  renderSnapshots: renderSnapshots,
  renderPerformance: renderPerformance,
  renderTimeline: renderTimeline,
  connectWS: connectWS
});

/* ═══ BACKWARD COMPAT SHIMS (ADR-0013: remove after full transition) ═══ */
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
window.showVmCompare = showVmCompare;

})(window.PCV);
