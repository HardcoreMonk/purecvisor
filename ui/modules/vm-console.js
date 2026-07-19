
window.PCV = window.PCV || {};
(function(PCV) {

async function renderConsole(b, v) {
  if (!v) return;
  var el = PCV.uxlib.el;
  var vncHtml = el('div', { class: 'text-center p-20' },
    el('p', { class: 'text-14' }, '🖨 ' + v.name),
    el('p', { class: 'stat-label mt-8' }, t('loading')));
  PCV.uxlib.clearEl(b);
  b.appendChild(el('div', { style: 'background:#000;border:1px solid var(--border);border-radius:var(--r);min-height:500px;height:calc(100vh - 200px);position:relative', id: 'vnc-frame' }, vncHtml));
  try {
    const r = await fetchGet(EP.VNC(v.name));
    const d = unwrapData(r);
    const addr = d.vnc_address || d.address || 'localhost';
    const port = d.vnc_port || d.port || '';
    if (port && v.state === 'running') {
      const frame = document.getElementById('vnc-frame');
      PCV.uxlib.clearEl(frame);
      frame.appendChild(el('div', { class: 'p-12' },
        el('div', { class: 'flex gap-12 items-center mb-12 flex-wrap' },
          HN.badge('VNC ' + t('vnc.connected'), 'g'),
          el('span', { class: 'text-13 font-600' }, addr + ':' + String(port)),
          el('button', { class: 'btn btn-g', onclick: "openNoVNCPopup('" + escapeAttr(addr) + "','" + escapeAttr(String(port)) + "','" + escapeAttr(v.name) + "')" }, '🖨 ' + t('vnc.open_popup')),
          el('button', { class: 'btn', onclick: "openNoVNC('" + escapeAttr(addr) + "','" + escapeAttr(String(port)) + "')" }, t('vnc.embedded')),
          el('button', { class: 'btn', onclick: "copyVncAddr('" + escapeAttr(String(port)) + "')" }, '📋 ' + t('vnc.copy_addr'))),
        el('div', { id: 'vnc-placeholder', style: 'background:#111;height:calc(100vh - 280px);min-height:400px;border-radius:var(--r);display:flex;align-items:center;justify-content:center;color:var(--fg2)' },
          el('div', { class: 'text-center' },
            el('p', { class: 'text-lg' }, '🖨 ' + v.name),
            el('p', { class: 'mt-8' }, '"' + t('vnc.open_popup') + '"'),
            el('p', { class: 'stat-label mt-4' }, 'VNC: ' + location.hostname + ':' + String(port))))));
    } else {
      const frame = document.getElementById('vnc-frame');
      PCV.uxlib.clearEl(frame);
      frame.appendChild(el('div', { class: 'text-center color-muted p-20' },
        el('p', { class: 'text-14' }, '🖨 ' + v.name),
        el('p', { class: 'mt-8' }, v.state === 'running' ? _L('VNC 포트를 사용할 수 없습니다', 'VNC port not available') : _L('VM이 중지 상태입니다', 'VM is stopped')),
        el('button', { class: 'btn mt-12', onclick: 'showVnc()' }, _L('VNC 확인', 'Check VNC'))));
    }
  } catch (e) {
    const frame = document.getElementById('vnc-frame');
    PCV.uxlib.clearEl(frame);
    frame.appendChild(el('div', { class: 'text-center color-muted p-20' },
      el('p', null, t('vnc.unavailable')),
      el('button', { class: 'btn mt-8', onclick: 'showVnc()' }, t('vnc.manual_check'))));
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
  var el = PCV.uxlib.el;
  PCV.uxlib.clearEl(frame);
  frame.appendChild(el('div', { class: 'flex gap-6 mb-6 items-center' },
    el('button', { class: 'btn', onclick: 'vncFullscreen()', title: escapeAttr(fullscreenText) }, '⛶ ' + fullscreenText),
    el('button', { class: 'btn', onclick: 'vncFitWindow()', title: escapeAttr(fitText) }, '🔬 ' + fitText),
    el('span', { id: 'vnc-res', class: 'stat-label' })));
  frame.appendChild(el('div', { id: 'vnc-container', style: 'width:100%;height:calc(100vh - 220px);min-height:500px;background:#000;border-radius:var(--r);position:relative' },
    el('div', { id: 'vnc-status', style: 'position:absolute;top:8px;left:8px;z-index:10;font-size:11px;color:var(--green);background:rgba(0,0,0,.7);padding:4px 10px;border-radius:4px' },
      el('span', { class: 'spinner' }),
      ' ' + loadingConnectingText + ' ' + statusEndpoint + '...'),
    vncIsoEjectTipHtml()));
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
  var el = PCV.uxlib.el;
  return el('div', { id: 'vnc-iso-tip', style: 'position:absolute;top:10px;right:10px;z-index:11;max-width:340px;background:rgba(8,12,18,.82);border:1px solid rgba(255,255,255,.18);border-radius:6px;padding:8px 10px;color:#e8f6ff;box-shadow:0 8px 28px rgba(0,0,0,.32);font-family:var(--font-ui,system-ui,sans-serif);pointer-events:none' },
    el('div', { style: 'font-size:11px;font-weight:700;color:#80eaff;margin-bottom:3px' }, t('vnc.iso_eject_title')),
    el('div', { style: 'font-size:11px;line-height:1.45;color:#d9e7ef' }, t('vnc.iso_eject_body')));
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

async function renderSnapshots(b, v) {
  if (!v) return;
  var el = PCV.uxlib.el;
  PCV.uxlib.clearEl(b);
  b.appendChild(el('div', null,
    el('div', { class: 'justify-between items-center mb-12' },
      el('h3', null, t('vm.snapshot') + ': ' + v.name),
      el('div', { class: 'flex gap-6' },
        el('button', { class: 'btn btn-g', onclick: 'takeSnap()' }, '+ ' + t('btn.create')),
        el('button', { class: 'btn btn-r', onclick: "snapDeleteAll('" + escapeAttr(v.name) + "')" }, '🗑 Delete All'))),
    el('div', { id: 'stree' },
      el('span', { class: 'spinner' }),
      ' ' + t('loading'))));
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
    if (!snaps.length) { PCV.uxlib.setMsg('stree', 'muted', { tag: 'p' }, t('snap.none')); return; }
    const el = PCV.uxlib.el;
    const tbody = el('tbody');
    snaps.forEach(s => {
      const safeName = v.name.replace(/'/g, "\\'");
      const safeSnap = s.name.replace(/'/g, "\\'");
      tbody.appendChild(el('tr', null,
        el('td', null, el('b', null, s.name)),
        el('td', { class: 'text-xs color-muted' }, s.time),
        el('td', { class: 'nowrap' },
          el('button', { class: 'btn', style: 'font-size:10px;padding:3px 8px;margin-right:4px', onclick: "snapRb('" + safeName + "','" + safeSnap + "')" }, t('snap.revert_confirm')),
          el('button', { class: 'btn btn-r', style: 'font-size:10px;padding:3px 8px', onclick: "snapDl('" + safeName + "','" + safeSnap + "')" }, t('btn.delete')))));
    });
    const table = el('table', null,
      el('thead', null, el('tr', null,
        el('th', null, 'Snapshot'), el('th', null, 'Created'), el('th', { class: 'w-140' }, 'Actions'))),
      tbody);
    const stree = document.getElementById('stree');
    PCV.uxlib.clearEl(stree);
    stree.appendChild(table);
  } catch (e) { PCV.uxlib.setMsg('stree', 'err', { tag: 'p' }, e.message); }
}

async function takeSnap() {
  const v = vmList[selectedVmIndex];
  if (!v) return;
  const on = v.state === 'running';
  const ts = new Date().toISOString().replace(/[-:T]/g, '').substring(0, 14);
  const defaultName = 'snap-' + ts;

  var mk = PCV.uxlib.el;
  showModal([
    mk('h2', { class: 'mb-14' }, '📷 Create Snapshot'),
    mk('div', { class: 'mb-14 p-10 border-muted rounded-md' },
      mk('div', { class: 'flex items-center gap-8 mb-8' },
        mk('span', { style: 'font-size:18px' }, '💻'),
        mk('div', null,
          mk('b', null, v.name),
          mk('div', { class: 'text-xs' },
            on ? mk('span', { class: 'color-green' }, 'Running') : mk('span', { class: 'color-muted' }, 'Stopped'),
            ' • ' + (v.vcpu || '?') + ' vCPU • ' + (v.memory_mb || '?') + ' MB'))),
      on ? mk('div', { class: 'text-xs', style: 'color:var(--yellow);padding:4px 8px;background:rgba(255,200,0,.08);border-radius:4px' }, '⚠ VM is running. Snapshot will capture live state (crash-consistent).') : null),
    mk('div', { class: 'mb-12' },
      mk('label', { for: 'snap-name-input', class: 'text-12 font-600' }, 'Snapshot Name'),
      mk('input', { id: 'snap-name-input', value: defaultName, class: 'w-full mt-4', oninput: 'snapNameValidate()', placeholder: 'alphanumeric, dash, underscore' })),
    mk('div', { id: 'snap-name-err', style: 'font-size:11px;min-height:16px;margin-bottom:8px' }),
    mk('div', { class: 'mb-14' },
      mk('label', { for: 'snap-desc-input', class: 'text-12 font-600' }, 'Description ', mk('span', { class: 'color-muted' }, '(optional)')),
      mk('input', { id: 'snap-desc-input', placeholder: 'e.g. Before upgrade, pre-migration backup', class: 'w-full mt-4' })),
    mk('div', { id: 'snap-preview', style: 'margin-bottom:14px;padding:10px;border:1px solid var(--border);border-radius:6px;font-size:11px' },
      mk('div', { class: 'color-muted mb-4 font-600' }, 'Preview'),
      mk('div', null, 'ZFS: ', mk('code', null, v.name + '@' + defaultName))),
    mk('div', { class: 'flex gap-6 justify-end' },
      mk('button', { class: 'btn', onclick: 'closeModal()' }, 'Cancel'),
      mk('button', { class: 'btn btn-g', id: 'snap-create-btn', onclick: 'snapCreateExec()' }, '📷 Create Snapshot'))
  ]);
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
  if (preview && v) {
    var mk = PCV.uxlib.el;
    PCV.uxlib.clearEl(preview);
    preview.appendChild(PCV.uxlib.frag(
      mk('div', { class: 'color-muted mb-4 font-600' }, 'Preview'),
      mk('div', null, 'ZFS: ', mk('code', null, v.name + '@' + n))));
  }
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

  var mk = PCV.uxlib.el;
  showModal([
    mk('h2', { class: 'mb-14' }, '⏪ Rollback Snapshot'),

    mk('div', { style: 'margin-bottom:14px;padding:12px;border:1px solid var(--red);border-radius:6px;background:rgba(255,60,60,.06)' },
      mk('div', { style: 'font-weight:700;color:var(--red);margin-bottom:6px' }, '⚠ Destructive Operation'),
      mk('div', { class: 'text-xs color-muted' }, 'This will revert the VM disk to the snapshot point-in-time. ', mk('b', null, 'All data written after this snapshot will be permanently lost.')),
      on ? mk('div', { class: 'text-xs', style: 'color:var(--yellow);margin-top:6px' }, '⚡ VM is currently ', mk('b', null, 'running'), ' — it will be ', mk('b', null, 'force-stopped'), ' before rollback, then automatically restarted.') : null),

    mk('div', { class: 'mb-14 p-10 border-muted rounded-md' },
      mk('div', { style: 'display:grid;grid-template-columns:100px 1fr;gap:4px 12px;font-size:12px' },
        mk('span', { class: 'color-muted' }, 'VM'),
        mk('span', null, mk('b', null, vm), ' ', on ? mk('span', { class: 'color-green' }, 'Running') : mk('span', { class: 'color-muted' }, 'Stopped')),
        mk('span', { class: 'color-muted' }, 'Snapshot'),
        mk('span', null, mk('code', null, s)),
        mk('span', { class: 'color-muted' }, 'ZFS Path'),
        mk('span', { class: 'text-xs' }, mk('code', null, vm + '@' + s)))),

    mk('div', { class: 'mb-14' },
      mk('label', { for: 'rb-confirm-input', class: 'text-12 font-600' }, 'Type VM name to confirm: ', mk('code', null, vm)),
      mk('input', { id: 'rb-confirm-input', placeholder: esc(vm), class: 'w-full mt-4', oninput: "rbValidate('" + escapeAttr(vm) + "')" })),
    mk('div', { class: 'flex gap-6 justify-end' },
      mk('button', { class: 'btn', onclick: 'closeModal()' }, 'Cancel'),
      mk('button', { class: 'btn btn-r', id: 'rb-exec-btn', disabled: '', onclick: "rbExec('" + vm.replace(/'/g, "\\'") + "','" + s.replace(/'/g, "\\'") + "')" }, '⏪ Rollback'))
  ]);
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

  var el = PCV.uxlib.el;
  showModal([
    el('h2', { class: 'mb-12' }, '🗑 Bulk Delete Snapshots'),
    el('div', { class: 'mb-12' },
      el('span', { class: 'color-muted' }, 'VM:'), ' ', el('b', null, vm), ' — ', el('span', { class: 'color-accent' }, snaps.length), ' snapshots'),
    el('div', { class: 'mb-14 p-10 border-muted rounded-md' },
      el('div', { style: 'margin-bottom:8px;font-weight:600;font-size:12px' }, 'Options'),
      el('div', { class: 'mb-8' },
        el('label', { for: 'sda-prefix', class: 'text-12' }, 'Prefix filter ', el('span', { class: 'color-muted' }, '(empty = all)')),
        el('input', { id: 'sda-prefix', placeholder: 'e.g. pcv-repl-', class: 'w-full mt-4', oninput: 'sdaPreview()' })),
      el('div', null,
        el('label', { for: 'sda-keep', class: 'text-12' }, 'Keep recent'),
        el('input', { id: 'sda-keep', type: 'number', min: '0', value: '0', style: 'width:80px;margin-left:8px', oninput: 'sdaPreview()' }), ' ', el('span', { class: 'color-muted text-xs' }, 'snapshots'))),
    el('div', { id: 'sda-preview', style: 'margin-bottom:14px;padding:10px;border:1px solid var(--border);border-radius:6px;max-height:200px;overflow-y:auto;font-size:11px' }),
    el('div', { class: 'flex gap-6 justify-end' },
      el('button', { class: 'btn', onclick: 'closeModal()' }, 'Cancel'),
      el('button', { class: 'btn btn-r', id: 'sda-exec-btn', onclick: "sdaExec('" + vm.replace(/'/g, "\\'") + "')" }, '🗑 Delete ', el('span', { id: 'sda-count' }, '0'), ' Snapshots'))
  ]);

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

  var mk = PCV.uxlib.el;
  var parts = [];
  parts.push(mk('div', { style: 'font-weight:600;margin-bottom:6px;color:var(--red)' }, 'Will DELETE: ' + delList.length));
  if (delList.length > 0) {
    delList.forEach(s => {
      parts.push(mk('div', { style: 'color:var(--red);padding:1px 0' }, '❌ ' + s.name + ' ', mk('span', { class: 'color-muted' }, s.time)));
    });
  } else {
    parts.push(mk('div', { class: 'color-muted' }, 'No snapshots match the criteria'));
  }
  if (keepList.length > 0) {
    parts.push(mk('div', { style: 'font-weight:600;margin-top:8px;margin-bottom:4px;color:var(--green)' }, 'Will KEEP: ' + keepList.length));
    keepList.forEach(s => {
      parts.push(mk('div', { style: 'color:var(--green);padding:1px 0' }, '✅ ' + s.name + ' ', mk('span', { class: 'color-muted' }, s.time)));
    });
  }
  PCV.uxlib.clearEl(el);
  el.appendChild(PCV.uxlib.frag(parts));
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

var perfLayout = 'auto';

function _vmcProgressBar(p, c) {
  var el = PCV.uxlib.el;
  var cl = p > 85 ? 'var(--red)' : p > 60 ? 'var(--yellow)' : 'var(--green)';
  var anim = p > 85 ? ' pulse-anim' : '';
  return el('div', { class: 'pb' + anim },
    el('div', { class: 'pb-f scan-anim', style: 'width:' + p + '%;background:' + (c || cl) }),
    el('div', { class: 'pb-t' }, p.toFixed(1) + '%'));
}

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
  var el = PCV.uxlib.el;
  var header = el('div', { class: 'justify-between items-center mb-12' },
    el('h3', null, t('tab.performance') + ': ' + v.name),
    el('div', { class: 'flex gap-6' },
      el('button', { class: 'tb ' + (perfLayout === 'auto' ? '' : 'btn'), onclick: "perfLayout='auto';renderPerformance(document.getElementById('cb'),vmList[selectedVmIndex])" }, '▦ Auto'),
      el('button', { class: 'tb ' + (perfLayout === 'manual' ? '' : 'btn'), onclick: "perfLayout='manual';renderPerformance(document.getElementById('cb'),vmList[selectedVmIndex])" }, '☰ Stack')));
  var perfGrid = el('div', { class: gridCls },
    HN.card('CPU Usage (60s) — ' + cpuPct.toFixed(1) + '%', [
      _vmcProgressBar(cpuPct),
      el('div', { style: 'position:relative;height:' + chartH + ';width:100%;margin-top:6px' }, el('canvas', { id: 'cg' }))
    ]),
    HN.card('Memory Usage (60s) — ' + memPct.toFixed(1) + '%', [
      _vmcProgressBar(memPct),
      el('div', { style: 'position:relative;height:' + chartH + ';width:100%;margin-top:6px' }, el('canvas', { id: 'mg' }))
    ]),
    HN.card('Disk IOPS', [
      HN.row(_L('읽기', 'Read'), el('span', { class: 'color-cyan' }, (metrics.disk_rd_req || 0).toLocaleString() + ' ops')),
      HN.row(_L('쓰기', 'Write'), el('span', { class: 'color-peach' }, (metrics.disk_wr_req || 0).toLocaleString() + ' ops')),
      HN.row('I/O Read', el('span', { class: 'color-cyan' }, formatBytes(metrics.disk_rd || 0))),
      HN.row('I/O Write', el('span', { class: 'color-peach' }, formatBytes(metrics.disk_wr || 0)))
    ]),
    HN.card('Network Packets', [
      HN.row('RX', [el('span', { class: 'color-yellow' }, formatBytes(metrics.net_rx || 0)), ' (' + (metrics.net_rx_pkts || 0).toLocaleString() + ' pps)']),
      HN.row('TX', [el('span', { class: 'color-yellow' }, formatBytes(metrics.net_tx || 0)), ' (' + (metrics.net_tx_pkts || 0).toLocaleString() + ' pps)'])
    ]));
  perfGrid.setAttribute('style', gridStyle);
  PCV.uxlib.clearEl(b);
  b.appendChild(header);
  b.appendChild(perfGrid);
  setTimeout(function() {
    createLineChart('cg', cpuHistory, 'CPU %', getChartColor('cpu'));
    createLineChart('mg', memHistory, 'MEM %', getChartColor('mem'));
  }, 30);
}

function renderTimeline(b, v) {
  if (!v) { PCV.uxlib.setMsg(b, 'muted', { tag: 'p' }, t('vm.select')); return; }
  var events = (eventLog || []).filter(function(e) {
    var msg = (e.msg || e.raw || '').toLowerCase();
    return msg.includes(v.name.toLowerCase());
  }).slice(-20);

  var el = PCV.uxlib.el;
  var parts = [];
  parts.push(el('h3', { class: 'mb-14' }, _L('타임라인', 'Timeline') + ': ' + v.name));
  if (events.length === 0) {
    parts.push(el('div', { class: 'empty-state', style: 'text-align:center;padding:30px' },
      el('div', { style: 'font-size:36px;opacity:.5' }, '🕑'),
      el('div', { class: 'color-muted mt-8' }, _L('이벤트 없음', 'No events yet'))));
  } else {
    var timeline = el('div', { style: 'position:relative;padding-left:24px;border-left:2px solid var(--border)' });
    events.forEach(function(e) {
      var msg = e.msg || e.raw || '';
      var isErr = msg.toLowerCase().includes('error') || msg.toLowerCase().includes('fail');
      var isOk = msg.toLowerCase().includes('start') || msg.toLowerCase().includes('created') || msg.toLowerCase().includes('completed');
      var color = isErr ? 'var(--red)' : isOk ? 'var(--green)' : 'var(--accent)';
      var icon = isErr ? '❌' : isOk ? '✅' : '🔸';
      timeline.appendChild(el('div', { style: 'position:relative;margin-bottom:14px' },
        el('div', { style: 'position:absolute;left:-30px;top:2px;width:12px;height:12px;border-radius:50%;background:' + color + ';border:2px solid var(--bg)' }),
        el('div', { style: 'font-size:10px;color:var(--fg2);margin-bottom:2px' }, e.time || ''),
        el('div', { style: 'font-size:12px;color:var(--fg);padding:6px 10px;background:var(--bg2);border-radius:4px;border-left:3px solid ' + color }, icon + ' ' + msg)));
    });
    parts.push(timeline);
  }
  PCV.uxlib.clearEl(b);
  b.appendChild(PCV.uxlib.frag(parts));
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
  var el = PCV.uxlib.el;
  var headRow = el('tr', null, el('th', null, _L('항목', 'Property')));
  selected.forEach(function(v) { headRow.appendChild(el('th', null, v.name)); });
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
  var bodyRows = props.map(function(p) {
    var tr = el('tr', null, el('td', { class: 'color-muted' }, el('b', null, p.label)));
    selected.forEach(function(v) {
      var val = v[p.key];
      if (p.key === 'state') val = HN.badge(val || '?', val === 'running' ? 'g' : 'r');
      else if (p.key === 'live_cpu_pct' || p.key === 'mem_percent') val = (val || 0).toFixed(1) + '%';
      else if (p.key === 'disk_rd' || p.key === 'disk_wr' || p.key === 'net_rx' || p.key === 'net_tx') val = formatBytes(val || 0);
      else val = String(val || '-');
      tr.appendChild(el('td', null, val));
    });
    return tr;
  });
  showModal([
    el('h2', null, _L('VM 비교', 'VM Comparison')),
    el('table', { class: 'text-12 w-full' },
      el('thead', null, headRow),
      el('tbody', null, bodyRows)),
    el('div', { class: 'text-right mt-14' },
      el('button', { class: 'btn', onclick: 'closeModal()' }, t('btn.close')))
  ]);
}

PCV.vm = Object.assign(PCV.vm || {}, {
  showVmCompare: showVmCompare,
  renderConsole: renderConsole,
  renderSnapshots: renderSnapshots,
  renderPerformance: renderPerformance,
  renderTimeline: renderTimeline,
  connectWS: connectWS
});

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
