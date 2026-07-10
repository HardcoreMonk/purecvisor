/* ═══════════════════════════════════════════════════════════════
   PureCVisor — modules/vm-lifecycle.js
   Bulk, Power/Delete, Create Wizard, Settings, NIC, Clone, Export OVA
   ADR-0013: IIFE module scope — vm.js에서 분할 (pure-move)
   ═══════════════════════════════════════════════════════════════ */
window.PCV = window.PCV || {};
(function(PCV) {

/* ═══ BULK ACTIONS ═══ */
function showBulkActions() {
  if (checkedVms.size === 0) { toast(_L('VM을 선택하세요', 'Select VMs first'), false); return; }
  var count = checkedVms.size;
  var names = Array.from(checkedVms).map(function(idx) { return vmList[idx] ? vmList[idx].name : '?'; }).join(', ');
  var h = '<h2>' + _L('일괄 작업', 'Bulk Actions') + ' (' + count + ' VMs)</h2>';
  h += '<p class="mb-12 color-muted text-xs">' + esc(names) + '</p>';
  h += '<div class="sg grid-2">';
  h += H.card('&#9654; ' + _L('일괄 시작', 'Start All'), '<button class="btn btn-g w-full" onclick="bulkAction(\'start\')">' + t('power.start') + ' ' + count + ' VMs</button>');
  h += H.card('&#9632; ' + _L('일괄 중지', 'Stop All'), '<button class="btn btn-r w-full" onclick="bulkAction(\'stop\')">' + t('power.stop') + ' ' + count + ' VMs</button>');
  h += H.card('&#128247; ' + _L('일괄 스냅샷', 'Snapshot All'), '<input aria-label="snap-' + Date.now() + '" id="bulk-snap-name" placeholder="snap-' + Date.now() + '" class="w-full mb-6"><button class="btn w-full" onclick="bulkSnapshot()">' + t('snap.created') + '</button>');
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
    if (ps) PCV.uxlib.setMsg(ps, 'loading', null, (i + 1) + '/' + names.length + ' — ' + names[i]);
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
    if (ps) PCV.uxlib.setMsg(ps, 'loading', null, (i + 1) + '/' + names.length + ' — ' + names[i]);
    try { await fetchPost(EP.VM_SNAPSHOT_CREATE(names[i]), { snap_name: snapName }); } catch (e) { /* continue */ }
  }
  if (ps) PCV.uxlib.setMsg(ps, null, null, '✅ ' + _L('완료', 'Done') + ': ' + names.length + ' snapshots');
  addEvt('Bulk snapshot: ' + snapName + ' → ' + names.join(', '));
  setTimeout(function() { closeModal(); loadAll(); }, 2000);
}

/* ADR-0018: VM 액션 실패 시 사용자에게 사유를 보여주는 헬퍼.
 * /health/recent-errors 에서 audit DB의 최근 worker 실패 fail 레코드를 가져와 표시.
 * 자동 닫기 없음 — 사용자가 [닫기] 버튼을 눌러야 닫힌다. */
async function showVmFailureDetail(statusEl, progEl, vmName, actionLabel) {
  if (progEl) { progEl.style.width = '100%'; progEl.style.background = 'var(--red)'; }
  /* notification center 영구 기록 (모달 닫혀도 사용자가 추후 확인 가능) */
  if (typeof addNotification === 'function') {
    addNotification('error',
      (actionLabel || 'VM action') + ' failed: ' + vmName,
      _L('30초 내 상태 전이 미확인 — audit DB 확인 필요', 'State change not confirmed within 30s — check audit DB'));
  }
  if (typeof addEvt === 'function') {
    addEvt('FAIL ' + (actionLabel || 'action') + ' ' + vmName + ' — state change timeout');
  }
  /* 정적 i18n 라벨 + escapeHtml(vmName) — XSS 안전 */
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
  /* 비동기 사유 조회 */
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

/* ═══ POWER / VM DELETE ═══
 *  vmPower(action)는 진행 모달을 즉시 표시한 뒤 fetch → 상태 폴링 패턴을 사용.
 *  백엔드가 fire-and-forget이므로 API 응답은 "accepted"일 뿐 완료가 아니다.
 *  따라서 2초 간격으로 최대 5회 VM 상태를 폴링하여 실제 전이를 확인한다.
 *  vmDel도 비슷하게 삭제 후 VM이 목록에서 사라질 때까지 폴링한다. */
async function vmPower(a) {
  const v = vmList[selectedVmIndex]; if (!v) return;
  var actionLabels = {
    start: { icon: '&#9654;', label: _L('시작', 'Start'), past: _L('시작됨', 'Started'), color: 'var(--green)' },
    stop: { icon: '&#9632;', label: _L('중지', 'Stop'), past: _L('중지됨', 'Stopped'), color: 'var(--red)' },
    suspend: { icon: '&#10074;&#10074;', label: _L('일시정지', 'Pause'), past: _L('일시정지됨', 'Paused'), color: 'var(--yellow)' },
    resume: { icon: '&#9654;&#9654;', label: _L('재개', 'Resume'), past: _L('재개됨', 'Resumed'), color: 'var(--green)' }
  };
  var al = actionLabels[a] || { icon: '&#9881;', label: a, past: a, color: 'var(--accent)' };
  /* 진행 모달 표시 */
  showModal('<div class="text-center p-20">'
    + '<div style="font-size:48px;margin-bottom:12px">' + al.icon + '</div>'
    + '<h2 class="mb-8">' + escapeHtml(v.name) + '</h2>'
    + '<div class="prog-bar"><div class="prog-fill" id="pwr-p" style="width:30%;background:' + al.color + '"></div></div>'
    + '<div class="prog-status" id="pwr-s" class="mt-10"><span class="spinner"></span> ' + al.label + ' ' + _L('진행 중...', 'in progress...') + '</div>'
    + '</div>');
  try {
    var pf = document.getElementById('pwr-p'), ps = document.getElementById('pwr-s');
    var r = await fetchPost(EP.VM_ACTION(v.name, a), {});
    /* API 에러 응답 체크 */
    if (r && r.error) {
      if (pf) { pf.style.width = '100%'; pf.style.background = 'var(--red)'; }
      if (ps) PCV.uxlib.setMsg(ps, null, null, '❌ ' + (r.error.message || 'Failed'));
      setTimeout(closeModal, 3000);
      return;
    }
    if (pf) pf.style.width = '60%';
    if (ps) PCV.uxlib.setMsg(ps, 'loading', null, _L('상태 확인 중...', 'Verifying state...'));
    /* W7 fix: 실제 VM 상태 폴링 최대 15회(30초), 2초 간격 — 느린 환경 허용 */
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
      if (ps) PCV.uxlib.setMsg(ps, null, null, '✅ ' + al.past);
      addEvt(v.name + ' ' + al.past);
      setTimeout(function() { closeModal(); loadAll(); }, 2000);
    } else {
      /* ADR-0018 fix: 자동 닫기 금지. 사용자가 명시적으로 [닫기]를 눌러야 함.
       * 백엔드 워커 실패 사유는 /health/recent-errors 에서 비동기 fetch */
      await showVmFailureDetail(ps, pf, v.name, al.label);
      loadAll();
    }
  } catch (e) {
    var pf2 = document.getElementById('pwr-p'), ps2 = document.getElementById('pwr-s');
    if (pf2) { pf2.style.width = '100%'; pf2.style.background = 'var(--red)'; }
    var errMsg = e.name === 'AbortError' ? _L('타임아웃 (10초)', 'timeout (10s)') : escapeHtml(e.message);
    /* ADR-0018: 자동 닫기 제거 — 사용자가 [닫기] 버튼을 명시적으로 눌러야 함 */
    var btnHtml = '<div class="mt-12"><button class="btn" onclick="closeModal()">' + t('btn.close') + '</button></div>';
    if (ps2) ps2.innerHTML = '&#10060; ' + _L('실패', 'Failed') + ': ' + errMsg + btnHtml;
  }
}

/* C4 fix: 공통 destroyConfirm 패턴으로 통일 (스냅샷 롤백과 동일 UX 수준) */
async function vmDel() {
  const v = vmList[selectedVmIndex]; if (!v) return;
  if (typeof destroyConfirm !== 'function') {
    /* fallback — uxlib 미로드 환경 */
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

/* C5 fix: DELETE 응답이 에러여도 실제 상태 폴링을 끝까지 수행 (서버는 계속 진행 중일 수 있음).
   W7-equivalent: 폴링을 10회(20초)로 확장. */
async function doVmDel(n) {
  showModal('<h2 class="color-red">&#9888; Deleting VM</h2><p><b class="color-accent">' + escapeHtml(n) + '</b></p><div class="prog-bar"><div class="prog-fill" id="dv-p" class="w-pct-10"></div></div><div class="prog-status" id="dv-s"><span class="spinner"></span>Sending delete request...</div>');
  const pf = document.getElementById('dv-p'), ps = document.getElementById('dv-s');
  var deleteError = null;
  try {
    if (pf) pf.style.width = '30%';
    const d = await fetchDelete(EP.VM_DETAIL(n)).catch(function(e) { return { error: { message: e && e.message || 'Network error' } }; });
    if (d && d.error) {
      deleteError = d.error.message || 'Failed';
      /* 에러여도 폴링 수행 — 서버가 백그라운드로 처리 완료했을 수 있음 */
      if (ps) PCV.uxlib.setMsg(ps, null, null, PCV.uxlib.el('span', { class: 'spinner' }), '⚠ ' + deleteError + _L(' — 서버 상태 확인 중...', ' — polling server state...'));
    } else {
      if (ps) PCV.uxlib.setMsg(ps, null, null, PCV.uxlib.el('span', { class: 'spinner' }), 'Waiting for zvol cleanup...');
    }
    if (pf) pf.style.width = '50%';
    for (let i = 0; i < 10; i++) {
      await new Promise(r => setTimeout(r, 2000));
      if (pf) pf.style.width = Math.min(95, 55 + i * 4) + '%';
      if (ps && !deleteError) PCV.uxlib.setMsg(ps, null, null, PCV.uxlib.el('span', { class: 'spinner' }), 'Cleaning up (' + (i + 1) + '/10)...');
      try {
        const vl = await fetchGet(EP.VM_LIST());
        const vms = unwrapList(vl);
        if (!vms.find(x => x.name === n)) {
          /* VM이 목록에서 사라짐 → 실제 삭제 성공 */
          if (pf) { pf.style.width = '100%'; pf.style.background = 'var(--green)'; }
          if (ps) PCV.uxlib.setMsg(ps, null, null, '✅ ' + t('vm.deleted'));
          toast(t('vm.deleted'));
          addEvt(t('vm.deleted') + ': ' + n);
          setTimeout(function() { closeModal(); loadAll(); }, 1500);
          return;
        }
      } catch (e) { if(typeof _DEBUG !== 'undefined' && _DEBUG) console.warn('vl:', e.message); }
    }
    /* 폴링 타임아웃 — 아직 목록에 남아있음 */
    if (pf) { pf.style.width = '100%'; pf.style.background = 'var(--yellow)'; }
    if (deleteError) {
      if (ps) PCV.uxlib.setMsg(ps, null, null, '❌ ' + deleteError);
      toast('&#10060; ' + escapeHtml(deleteError), false);
    } else {
      if (ps) PCV.uxlib.setMsg(ps, null, null, '⚠ ' + _L('삭제가 오래 걸리고 있습니다', 'Delete taking longer than expected'));
      toast(_L('삭제가 오래 걸리고 있습니다. 잠시 후 새로고침하세요.', 'Delete taking longer than expected — refresh shortly.'), false);
    }
    setTimeout(function() { closeModal(); loadAll(); }, 2000);
  } catch (e) {
    if (pf) { pf.style.width = '100%'; pf.style.background = 'var(--red)'; }
    if (ps) PCV.uxlib.setMsg(ps, null, null, '❌ ' + (e.message || 'Unknown error'));
    toast(e.message || 'Unknown error', false);
    setTimeout(closeModal, 3000);
  }
}

/* ═══ VM CREATE WIZARD ═══
 *  3단계 위자드: 1) 이름 2) CPU/메모리 3) 디스크/ISO/네트워크
 *  wizData에 각 단계의 입력값을 누적. wizSave()로 현재 단계 저장.
 *  step 1→2 이동 시 VM 이름 정규식 검증 (/^[a-zA-Z0-9][a-zA-Z0-9_-]{0,63}$/).
 *  doCreate()에서 fire-and-forget 패턴: 모달 닫고 → 1초 폴링으로 생성 확인.
 *  최대 20초(20회) 폴링 후 미확인이면 사용자에게 안내. */
/* M4 fix: storage_type 기본값을 'auto'로 → 백엔드 자동 감지 사용.
   사용자가 명시적으로 선택하지 않으면 body에서 storage_type 제거 (backend fallback 활용). */
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
  /* Step 1 → 2 이동 시 VM 이름 검증 */
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
  if (wizStep === 1) h += `<div class="fr"><label for="wn">VM Name</label><input id="wn" value="${escapeHtml(wizData.name)}" placeholder="my-vm"></div><div class="text-right mt-12"><button class="btn" onclick="wizGo(2)">${t('btn.next')} &rarr;</button></div>`;
  else if (wizStep === 2) h += `<div class="fr"><label for="wc">vCPU</label><input id="wc" type="number" value="${escapeHtml(String(wizData.vcpu))}"></div><div class="fr"><label for="wm">Memory MB</label><input id="wm" type="number" value="${escapeHtml(String(wizData.mem))}"></div><div class="text-right mt-12"><button class="tb" onclick="wizGo(1)">&larr; ${t('btn.prev')}</button> <button class="btn" onclick="wizGo(3)">${t('btn.next')} &rarr;</button></div>`;
  else {
    const stSel = wizData.storage_type || 'auto';
    const poolVal = wizData.storage_pool || 'pcvpool/vms';
    const imageDirVal = wizData.image_dir || '/var/lib/libvirt/images';
    const isFileStorage = stSel === 'qcow2' || stSel === 'raw';
    const pools = Array.isArray(wizData.storage_pools) ? wizData.storage_pools : [];
    const poolOptions = pools.map(function(p) {
      return '<option value="' + escapeHtml(p) + '"' + (p === poolVal ? ' selected' : '') + '>' + escapeHtml(p) + '</option>';
    }).join('');
    h += `<div class="fr"><label for="wd">Disk GB</label><input id="wd" type="number" value="${escapeHtml(String(wizData.disk))}"></div>`
      + `<div class="fr"><label for="wst">${_L('스토리지 타입', 'Storage Type')}</label>`
      + `<select id="wst" onchange="wizStorageChanged(true)">`
      + `<option value="auto"${stSel === 'auto' ? ' selected' : ''}>${_L('자동 (서버 감지)', 'Auto (server detected)')}</option>`
      + `<option value="zvol"${stSel === 'zvol' ? ' selected' : ''}>ZFS zvol — ${_L('블록 디바이스, 고성능', 'Block device, high performance')}</option>`
      + `<option value="qcow2"${stSel === 'qcow2' ? ' selected' : ''}>qcow2 — ${_L('파일 기반, 스냅샷/씬 프로비저닝', 'File based, snapshot/thin provisioning')}</option>`
      + `<option value="raw"${stSel === 'raw' ? ' selected' : ''}>raw — ${_L('파일 기반, 최대 I/O 성능', 'File based, maximum I/O performance')}</option>`
      + `</select></div>`
      + (isFileStorage
        ? `<div class="fr"><label for="widir">${_L('저장 위치', 'Storage Location')}</label><input id="widir" value="${escapeHtml(imageDirVal)}" placeholder="/var/lib/libvirt/images" oninput="wizStorageChanged(false)"></div>`
        : `<div class="fr"><label for="wspool">${_L('저장 위치', 'Storage Location')}</label><div class="flex gap-6 flex-1"><input id="wspool" value="${escapeHtml(poolVal)}" placeholder="pcvpool/vms" class="flex-1" oninput="wizStorageChanged(false)">`
          + `<select id="wspick" onchange="wizPickStoragePool()" aria-label="${escapeHtml(_L('사용 가능한 ZFS 풀', 'Available ZFS pools'))}" title="${escapeHtml(_L('사용 가능한 ZFS 풀', 'Available ZFS pools'))}"><option value="">${escapeHtml(_L('풀 선택', 'Pool'))}</option>${poolOptions}</select>`
          + `<button class="btn text-xs" onclick="wizLoadStorageTargets(true)">${_L('새로고침', 'Refresh')}</button></div></div>`)
      + (isFileStorage ? `<input id="wspool" type="hidden" value="${escapeHtml(poolVal)}">` : `<input id="widir" type="hidden" value="${escapeHtml(imageDirVal)}">`)
      + `<div class="stat-label mb-8" id="wstorage-preview">${escapeHtml(wizStoragePreview())}</div>`
      + `<div class="fr"><label for="wi">ISO Image</label><div class="flex gap-6"><input id="wi" value="${escapeHtml(wizData.iso)}" placeholder="ISO path..." class="flex-1"><button class="btn" onclick="browseISO()">Browse</button></div></div>`
      + `<div class="fr"><label for="wb">Network</label><div class="flex gap-6 flex-1"><select id="wb"><option value="${escapeHtml(wizData.bridge)}">${t('loading')}</option></select><button class="btn text-xs" onclick="wizLoadNets()">Refresh</button></div></div>`
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

/* M3 fix: 네트워크 목록 조회 실패 시 명시적 토스트 + 수동 입력 힌트
   (이전엔 virbr0 하드코딩으로 숨김 → 브릿지 없는 환경에서 생성 실패) */
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
  h += '<input aria-label="Direct path..." id="iso-manual-path" placeholder="Direct path..." style="flex:1;padding:8px 10px;background:var(--bg3);border:1px solid var(--border);border-radius:6px;color:var(--fg);font-size:12px" onkeydown="if(event.key===\'Enter\')isoSelectManual()">';
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
  } catch (e) { const el = document.getElementById('iso-modal-list'); if (el) PCV.uxlib.setMsg(el, null, { tag: 'div', cls: 'p-12 color-red' }, '❌ Error: ' + e.message); }
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
  /* M3 (cont.): 네트워크 브릿지 필수 — wizLoadNets에서 선택된 값이 있어야 함 */
  if (!d.bridge) {
    toast(_L('네트워크 브릿지를 선택하세요', 'Select a network bridge'), false); return;
  }
  /* Client-side 수치 가드 (서버는 W5에서 엄격 검증) */
  if (!(d.vcpu >= 1 && d.vcpu <= 256)) {
    toast(_L('vCPU 는 1~256 사이', 'vCPU must be between 1 and 256'), false); return;
  }
  if (!(d.mem >= 256 && d.mem <= 1048576)) {
    toast(_L('메모리는 256~1048576 MB 사이', 'Memory must be 256~1048576 MB'), false); return;
  }
  if (!(d.disk >= 1 && d.disk <= 65536)) {
    toast(_L('디스크는 1~65536 GB 사이', 'Disk must be 1~65536 GB'), false); return;
  }

  /* 모달 즉시 닫고 백그라운드 처리 */
  if (typeof clearFormDirty === 'function') clearFormDirty('vm-create');
  closeModal(true);
  toast('&#128187; ' + escapeHtml(name) + ' ' + _L('생성 시작...', 'Creating...'), 's');

  try {
    /* M4: storage_type='auto'일 때 body에서 제외 → 백엔드가 ZFS 풀 감지 후 qcow2 폴백 */
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
    /* W8 fix: fire-and-forget 백엔드 — 워커가 libvirt define 마칠 때까지 폴링 (최대 20초)
       절대 타임아웃 25초 + 연속 에러 5회로 이중 안전장치 → interval 누수 방지 */
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

/* ═══ SETTINGS ═══ */
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
  return `<h4>Identity</h4><div class="fr"><label for="vm-current">${_L('현재 이름', 'Current')}</label><input id="vm-current" value="${escapeAttr(v?.name || '')}" disabled style="opacity:.65"></div><div class="fr"><label for="rn-new">${_L('새 이름', 'New')}</label><input id="rn-new" value="${escapeAttr(v?.name || '')}" maxlength="64" ${blocked ? 'disabled' : ''}></div>${blocked ? '<p class="color-yellow text-xs">' + _L('정지 상태에서만 변경할 수 있습니다.', 'Rename requires the VM to be shut off.') + '</p>' : ''}<button class="btn btn-g" onclick="doVmRename()" ${blocked ? 'disabled' : ''}>${_L('이름 변경', 'Rename')}</button>`;
}
function hwCpu() { return `<h4>CPU</h4><div class="fr"><label for="sc">vCPU</label><input id="sc" type="number" value="${escapeHtml(String(vmList[selectedVmIndex]?.vcpu || 2))}"></div><button class="btn" onclick="doSet('vcpu')">${t('btn.apply')}</button>`; }
function hwMem() { return `<h4>Memory</h4><div class="fr"><label for="sm">MB</label><input id="sm" type="number" value="${escapeHtml(String(vmList[selectedVmIndex]?.memory_mb || 2048))}"></div><button class="btn" onclick="doSet('memory')">${t('btn.apply')}</button>`; }
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
    PCV.uxlib.setMsg(current, null, { cls: clean ? 'color-green' : 'color-muted' }, clean || _L('비어 있음', 'Empty'));
  }
}

function hwCd() {
  const current = _currentCdromPath(vmList[selectedVmIndex]);
  return `<h4>CD/DVD</h4>`
    + `<div class="fr"><label>${_L('현재 ISO', 'Current ISO')}</label><div id="cdrom-current" class="flex-1">${current ? '<span class="color-green">' + escapeHtml(current) + '</span>' : '<span class="color-muted">' + _L('비어 있음', 'Empty') + '</span>'}</div></div>`
    + `<div class="fr"><label for="si">ISO</label><div class="flex gap-6 flex-1"><input id="si" placeholder="ISO path..." class="flex-1" value="${escapeAttr(current)}"><button class="btn" onclick="browseISOForMount()">&#128194; Browse</button></div></div>`
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
        files.forEach(iso => { const p = String(iso.path || iso.name || ''); const fn = (iso.name || p).replace(/^.*\//, ''); const sz = iso.size_mb ? iso.size_mb + 'MB' : '';
          h += '<div onclick="selectISOForMount(' + escapeAttr(JSON.stringify(p)) + ')" style="padding:7px 12px;cursor:pointer;font-size:12px;display:flex;align-items:center;gap:6px;border-bottom:1px solid var(--border)" onmouseover="this.style.background=\'var(--bg3)\'" onmouseout="this.style.background=\'\'">';
          h += '<span class="color-accent">&#128191;</span><span class="flex-1">' + escapeHtml(fn) + '</span><span class="color-muted text-xs">' + escapeHtml(sz) + '</span></div>'; }); }); }
    h += '</div><div class="text-right mt-10"><button class="btn btn-r" onclick="closeModal()">' + t('btn.cancel') + '</button></div>';
    showModal(h); } catch (e) { toast('ISO list error: ' + e.message, false); }
}

function hwNic() { const v = vmList[selectedVmIndex]; const html = `<h4>Network</h4><div id="nic-list">${t('loading')}</div><div class="mt-8"><div class="fr"><label for="nic-br">Bridge</label><input id="nic-br" value="pcvbr0"><button class="btn btn-g" onclick="nicAdd()">+ Add NIC</button></div></div>`; setTimeout(() => loadNics(v?.name), 50); return html; }
async function loadNics(n) { if (!n) return; try { const r = await fetchGet(EP.VM_NICS(n)); const l = unwrapList(r); let h = '<table><thead><tr><th>MAC</th><th>Bridge</th><th>Model</th><th>IP</th><th>DNS</th><th></th></tr></thead><tbody>'; l.forEach(c => { const dns = c.dns === 'off' ? 'OFF' : (c.dns || '-'); h += `<tr><td>${escapeHtml(c.mac || '-')}</td><td>${escapeHtml(c.bridge || c.source || '-')}</td><td>${escapeHtml(c.model || 'virtio')}</td><td>${escapeHtml(c.ip || '-')}</td><td>${escapeHtml(dns)}</td><td><button class="btn btn-r text-9" onclick="nicDel('${escapeAttr(n)}','${escapeAttr(c.mac)}')">${t('btn.delete')}</button></td></tr>`; }); h += '</tbody></table>'; const el = document.getElementById('nic-list'); if (el) el.innerHTML = l.length ? h : '<p class="color-muted">No NICs</p>'; } catch (e) { if(_DEBUG) console.warn('loadNics:', e.message); } }
async function nicAdd() { const v = vmList[selectedVmIndex]; if (!v) return; try { await fetchPost(EP.VM_NICS(v.name), { bridge: document.getElementById('nic-br')?.value || 'pcvbr0' }); toast(t('nic.added')); loadNics(v.name); } catch (e) { toast(e.message, false); } }
async function nicDel(n, mac) { if (!await customConfirm(t('btn.delete'), 'NIC ' + mac + '?')) return; try { await fetchDelete(EP.VM_NIC_DETACH(n, mac)); toast(t('nic.removed')); loadNics(n); } catch (e) { toast(e.message, false); } }

function hwAP() { const html = `<h4>AutoProtect (Backup Policy)</h4><div id="bp-list">${t('loading')}</div><div class="mt-8"><div class="fr"><label for="bp-vm">VM</label><input id="bp-vm" value="${escapeHtml(vmList[selectedVmIndex]?.name || '*')}"></div><div class="fr"><label for="bp-int">Interval (h)</label><input id="bp-int" type="number" value="24"></div><div class="fr"><label for="bp-ret">Retention</label><input id="bp-ret" type="number" value="7"></div><button class="btn btn-g" onclick="bpSet()">Set Policy</button></div>`; setTimeout(loadBP, 50); return html; }
async function loadBP() { try { const r = await fetchGet(EP.BACKUP_POLICIES()); const l = unwrapList(r); let h = '<table><thead><tr><th>VM</th><th>Interval</th><th>Retention</th></tr></thead><tbody>'; if (Array.isArray(l)) l.forEach(p => { h += `<tr><td>${escapeHtml(p.vm_name || p.name || '-')}</td><td>${escapeHtml(String(p.interval_hours || p.interval || '-'))}h</td><td>${escapeHtml(String(p.retention || '-'))}</td></tr>`; }); h += '</tbody></table>'; const el = document.getElementById('bp-list'); if (el) el.innerHTML = l.length ? h : '<p class="color-muted">No policies</p>'; } catch (e) { if(_DEBUG) console.warn('loadBP:', e.message); } }
async function bpSet() { try { await fetchPost(EP.BACKUP_POLICIES(), { vm_name: document.getElementById('bp-vm')?.value || '*', interval: +(document.getElementById('bp-int')?.value || 24), retention: +(document.getElementById('bp-ret')?.value || 7) }); toast(t('backup.policy_set')); loadBP(); } catch (e) { toast(e.message, false); } }

function showRenameVm() {
  const v = vmList[selectedVmIndex]; if (!v) return;
  const blocked = _vmRenameBlocked(v);
  showModal('<h2>' + _L('VM 이름 변경', 'Rename VM') + ': ' + escapeHtml(v.name) + '</h2>'
    + '<div class="fr"><label for="vm-current-2">' + _L('현재 이름', 'Current') + '</label><input id="vm-current-2" value="' + escapeAttr(v.name) + '" disabled style="opacity:.65"></div>'
    + '<div class="fr"><label for="rn-new">' + _L('새 이름', 'New') + '</label><input id="rn-new" value="' + escapeAttr(v.name) + '" maxlength="64" ' + (blocked ? 'disabled' : '') + '></div>'
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
    if (statusEl) PCV.uxlib.setMsg(statusEl, 'loading', null, _L('변경 중...', 'Renaming...'));
    const r = await fetchPut(EP.VM_RENAME(v.name), { new_name: next });
    if (r && r.error) {
      const msg = r.error.message || t('error');
      if (statusEl) PCV.uxlib.setMsg(statusEl, null, null, '❌ ' + msg);
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
    if (statusEl) PCV.uxlib.setMsg(statusEl, null, null, '❌ ' + (e.message || 'Failed'));
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

/* ═══ SNAPSHOT SHORTCUT ═══ */
function showSnap() { currentTab = 'snapshots'; document.querySelectorAll('#ct button').forEach(b => { b.classList.remove('active'); if (b.dataset.t === 'snapshots') b.classList.add('active'); }); renderContent(); }

/* ═══ NIC MANAGER ═══ */
async function showNicMgr() { const v = vmList[selectedVmIndex]; if (!v) return; showModal(`<h2>NIC: ${escapeHtml(v.name)}</h2><div id="nic-mgr">${t('loading')}</div><div class="mt-10"><div class="fr"><label for="nm-br">Bridge</label><input id="nm-br" value="pcvbr0"><button class="btn btn-g" onclick="nmAdd()">+ Add</button></div></div><div class="text-right mt-12"><button class="btn btn-r" onclick="closeModal()">${t('btn.close')}</button></div>`);
  try { const r = await fetchGet(EP.VM_NICS(v.name)); const l = unwrapList(r); let h = '<table><thead><tr><th>MAC</th><th>Bridge</th><th>Model</th><th>IP</th><th>DNS</th><th></th></tr></thead><tbody>'; l.forEach(c => { const dns = c.dns === 'off' ? 'OFF' : (c.dns || '-'); h += `<tr><td>${escapeHtml(c.mac || '-')}</td><td>${escapeHtml(c.bridge || c.source || '-')}</td><td>${escapeHtml(c.model || 'virtio')}</td><td>${escapeHtml(c.ip || '-')}</td><td>${escapeHtml(dns)}</td><td><button class="btn btn-r text-9" onclick="nmDel('${escapeAttr(c.mac)}')">${t('btn.delete')}</button></td></tr>`; }); document.getElementById('nic-mgr').innerHTML = l.length ? h + '</tbody></table>' : '<p class="color-muted">No NICs</p>'; } catch (e) { PCV.uxlib.setMsg('nic-mgr', null, null, t('error')); } }
async function nmAdd() { const v = vmList[selectedVmIndex]; if (!v) return; try { await fetchPost(EP.VM_NICS(v.name), { bridge: document.getElementById('nm-br')?.value || 'pcvbr0' }); toast(t('nic.added')); showNicMgr(); } catch (e) { toast(e.message, false); } }
async function nmDel(mac) { const v = vmList[selectedVmIndex]; if (!v || !await customConfirm(t('btn.delete'), mac + '?')) return; try { await fetchDelete(EP.VM_NIC_DETACH(v.name, mac)); toast(t('nic.removed')); showNicMgr(); } catch (e) { toast(e.message, false); } }

/* ═══ VNC MODAL ═══ */
async function showVnc() { const v = vmList[selectedVmIndex]; if (!v) return; showModal(`<h2>VNC: ${escapeHtml(v.name)}</h2><div id="vnc-info">${t('loading')}</div><div class="text-right mt-12"><button class="btn btn-r" onclick="closeModal()">${t('btn.close')}</button></div>`);
  try { const r = await fetchGet(EP.VNC(v.name)); const d = unwrapData(r); const stBadge = v.state === 'running' ? H.badge('Available', 'g') : H.badge('VM stopped', 'r'); document.getElementById('vnc-info').innerHTML = H.card('', H.row('Address', escapeHtml(d.vnc_address || d.address || 'localhost')) + H.row('Port', escapeHtml(String(d.vnc_port || d.port || '-'))) + H.row('Status', stBadge)); } catch (e) { document.getElementById('vnc-info').innerHTML = H.card('', '<p class="color-muted">VNC info unavailable</p>'); } }

/* ═══ VM CLONE ═══ */
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
    + '<div class="fr"><label for="vm-clone-name">' + _L('새 VM 이름', 'New VM name') + '</label><input id="vm-clone-name" class="input-field" value="' + escapeAttr(suggested) + '"></div>'
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

/* ═══ VM DISK RESIZE ═══ */
function hwDisk() {
  return '<h4>&#128190; Disk Resize</h4><div class="fr"><label for="sd-size">New Size (GB)</label><input id="sd-size" type="number" value="40" placeholder="40"></div><div class="fr"><label for="sd-path">Disk Path</label><input id="sd-path" placeholder="vda (optional)"></div><button class="btn btn-g" onclick="doDiskResize()">' + t('btn.apply') + '</button><p class="stat-label mt-8">Live disk resize (qemu-img resize). VM can be running.</p>';
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

/* ═══ VM DELETE STATUS ═══ */
async function vmDeleteStatus(name) {
  try {
    const r = await fetchGet(EP.VM_DELETE_STATUS(name));
    const d = unwrapData(r);
    return d.status || 'unknown';
  } catch (e) { return 'unknown'; }
}

/* ═══ VM EXPORT OVA ═══ */
async function vmExportOva(idx) {
  const v = vmList[idx ?? selectedVmIndex]; if (!v) return;
  if (!await customConfirm('Export OVA', _L('VM을 OVA 파일로 내보내시겠습니까?', 'Export ' + v.name + ' as OVA file?') + '\n' + v.name)) return;
  showModal('<h2>&#128230; Export OVA</h2><p class="mb-8"><b class="color-accent">' + escapeHtml(v.name) + '</b></p><div class="prog-bar"><div class="prog-fill" id="ova-p" class="w-pct-10"></div></div><div class="prog-status" id="ova-s"><span class="spinner"></span> ' + _L('내보내기 시작 중...', 'Starting export...') + '</div>');
  try {
    var pf = document.getElementById('ova-p'), ps = document.getElementById('ova-s');
    pf.style.width = '30%'; PCV.uxlib.setMsg(ps, 'loading', null, _L('OVA 변환 요청 중...', 'Requesting OVA conversion...'));
    var r = await fetchPost(EP.VM_EXPORT(v.name), {});
    if (r.error) { pf.style.background = 'var(--red)'; pf.style.width = '100%'; PCV.uxlib.setMsg(ps, null, null, '❌ ' + (r.error.message || 'Export failed')); return; }
    pf.style.width = '70%'; PCV.uxlib.setMsg(ps, 'loading', null, _L('변환 진행 중...', 'Converting...'));
    var d = unwrapData(r) || r;
    var path = d.path || d.ova_path || '';
    /* 상태 폴링 (export-status 있으면) */
    for (var pi = 0; pi < 5; pi++) {
      await new Promise(function(res) { setTimeout(res, 2000); });
      pf.style.width = (75 + pi * 5) + '%';
      try { var st = await fetchGet(EP.VM_DETAIL(v.name) + '/export-status'); var sd = unwrapData(st) || st; if (sd.status === 'done' || sd.status === 'completed') break; } catch(e) { break; }
    }
    pf.style.width = '100%'; pf.style.background = 'var(--green)';
    PCV.uxlib.setMsg(ps, null, null,
      '✅ ' + _L('내보내기 완료', 'Export completed'),
      path ? PCV.uxlib.el('br') : null,
      path ? PCV.uxlib.el('span', { class: 'text-xs color-muted' }, path) : null);
    toast('&#128230; ' + v.name + ' OVA ' + _L('내보내기 완료', 'export completed'));
    addEvt('OVA export: ' + v.name + (path ? ' → ' + path : ''));
  } catch (e) {
    var pf2 = document.getElementById('ova-p'), ps2 = document.getElementById('ova-s');
    if (pf2) { pf2.style.background = 'var(--red)'; pf2.style.width = '100%'; }
    if (ps2) PCV.uxlib.setMsg(ps2, null, null, '❌ ' + e.message);
    toast(e.message, false);
  }
}

/* ═══ EXPORT TO PCV NAMESPACE (ADR-0013) ═══
 *  PCV.vm에 등록되는 함수가 이 모듈의 공식 인터페이스.
 *  아래 BACKWARD COMPAT SHIMS는 HTML onclick과 다른 모듈의
 *  window.render() 등 직접 참조를 위한 전환기 코드.
 *  신규 코드에서는 PCV.vm.render() 사용을 권장. */
PCV.vm = Object.assign(PCV.vm || {}, {
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
  showBulkActions: showBulkActions,
  bulkAction: bulkAction,
  bulkSnapshot: bulkSnapshot,
});

/* ═══ BACKWARD COMPAT SHIMS (ADR-0013: remove after full transition) ═══ */
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
window.showBulkActions = showBulkActions;
window.bulkAction = bulkAction;
window.bulkSnapshot = bulkSnapshot;

})(window.PCV);
