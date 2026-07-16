/* ═══════════════════════════════════════════════════════════════
   PureCVisor — modules/advanced.js
   Templates, Config Management, OVA Import
   ADR-0013: IIFE module scope — PCV.advanced namespace
   ═══════════════════════════════════════════════════════════════ */
/*
 * Advanced screens are optional capability frontends. A missing backend should
 * render an explanatory empty state, while configured backends must still use
 * EP registry helpers and sanitizer paths before inserting returned data.
 */
window.PCV = window.PCV || {};
(function(PCV) {

/* ADR-013 DOM-safe: i18n 설명 문자열에 포함된 <br> 마크업을 [텍스트, <br> 노드, ...]
 * 배열로 분해. innerHTML 경로에서 <br> 가 요소로 파싱되던 것을 노드 조립에서 동형
 * 재현한다. (PCV.uxlib 는 늦게 로드되므로 el() 은 호출 시점에 해소.) */
function _splitBr(s) {
  var out = [];
  String(s).split('<br>').forEach(function (seg, i) {
    if (i > 0) out.push(PCV.uxlib.el('br'));
    out.push(seg);
  });
  return out;
}

/* ═══ TEMPLATES ═══ */
async function renderTemplates(b) {
  showSkeleton(b);
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  try {
    const r = await fetchGet(EP.TEMPLATES());
    const list = unwrapList(r);
    var parts = [HN.section([
      '\u{1F4C3} VM Templates ',
      el('button', { class: 'btn btn-g', onclick: 'showTemplateCreate()', style: 'margin-left:8px' }, '+ Create Template'),
      ' ',
      el('button', { class: 'btn', onclick: 'loadTemplateHistory()', style: 'margin-left:4px' }, '\u{1F4CB} History')
    ])];
    if (list.length === 0) {
      parts.push(el('div', { class: 'empty-state', style: 'text-align:center;padding:40px 20px' },
        el('div', { style: 'font-size:48px;margin-bottom:12px;opacity:.5' }, '\u{1F4C3}'),
        el('div', { style: 'font-size:14px;color:var(--fg2);margin-bottom:16px' }, 'No templates found'),
        el('button', { class: 'btn btn-g', onclick: 'showTemplateCreate()' }, '+ Create Template')));
      clearEl(b); b.appendChild(frag(parts)); return;
    }
    var rows = list.map(t2 => el('tr', null,
      el('td', null, el('b', null, t2.name || '-')),
      el('td', null, t2.vcpu || '-'),
      el('td', null, (t2.memory_mb || '-') + ' MB'),
      el('td', null, (t2.disk_gb || '-') + ' GB'),
      el('td', null, t2.os_variant || '-'),
      el('td', null,
        el('button', { class: 'btn', style: 'font-size:10px;padding:3px 8px', onclick: "templateUse('" + escapeHtml(t2.name) + "')" }, 'Use'),
        ' ',
        el('button', { class: 'btn btn-r', style: 'font-size:10px;padding:3px 8px', onclick: "templateDel('" + escapeHtml(t2.name) + "')" }, t('btn.delete')))));
    parts.push(el('table', null,
      el('thead', null, el('tr', null,
        el('th', null, 'Name'), el('th', null, 'vCPU'), el('th', null, 'Memory'), el('th', null, 'Disk'), el('th', null, 'OS'), el('th', null, 'Actions'))),
      el('tbody', null, rows)));
    parts.push(el('div', { id: 'tpl-history' }));
    clearEl(b); b.appendChild(frag(parts));
  } catch (e) { PCV.uxlib.setMsg(b, null, { tag: 'p', cls: 'color-red' }, e.message); }
}

function showTemplateCreate() {
  var el = PCV.uxlib.el;
  showModal([
    el('h2', null, 'Create VM Template'),
    el('div', { class: 'fr' }, el('label', { for: 'tpl-name' }, 'Name'), el('input', { id: 'tpl-name', placeholder: 'web-small' })),
    el('div', { class: 'fr' }, el('label', { for: 'tpl-vcpu' }, 'vCPU'), el('input', { id: 'tpl-vcpu', type: 'number', value: '2' })),
    el('div', { class: 'fr' }, el('label', { for: 'tpl-mem' }, 'Memory (MB)'), el('input', { id: 'tpl-mem', type: 'number', value: '2048' })),
    el('div', { class: 'fr' }, el('label', { for: 'tpl-disk' }, 'Disk (GB)'), el('input', { id: 'tpl-disk', type: 'number', value: '20' })),
    el('div', { class: 'fr' }, el('label', { for: 'tpl-os' }, 'OS Variant'), el('input', { id: 'tpl-os', value: 'ubuntu24.04' })),
    el('div', { class: 'text-right mt-12' },
      el('button', { class: 'btn btn-g', onclick: 'doTemplateCreate()' }, t('btn.create')),
      ' ',
      el('button', { class: 'btn btn-r', onclick: 'closeModal()' }, t('btn.cancel')))
  ]);
}

async function doTemplateCreate() {
  const name = document.getElementById('tpl-name')?.value;
  if (!name) { toast('Template name required', false); return; }
  try {
    const r = await fetchPost(EP.TEMPLATES(), { name, vcpu: +(document.getElementById('tpl-vcpu')?.value || 2), memory_mb: +(document.getElementById('tpl-mem')?.value || 2048), disk_gb: +(document.getElementById('tpl-disk')?.value || 20), os_variant: document.getElementById('tpl-os')?.value || '' });
    if (r.error) { toast(r.error.message || 'Failed', false); return; }
    toast('Template created: ' + name); addEvt('Template: ' + name); closeModal(); renderTemplates(document.getElementById('cb'));
  } catch (e) { toast(e.message, false); }
}

async function templateUse(name) {
  try { const r = await fetchGet(EP.TEMPLATE(name)); const d = unwrapData(r);
    wizData = { name: '', vcpu: d.vcpu || 2, mem: d.memory_mb || 2048, disk: d.disk_gb || 20, iso: '', bridge: 'pcvbr0' };
    wizStep = 1; renderWiz(); toast('Template loaded: ' + name);
  } catch (e) { toast(e.message, false); }
}

async function templateDel(name) {
  if (!await customConfirm(t('btn.delete'), 'Template: ' + name + '?')) return;
  try { const r = await fetchDelete(EP.TEMPLATE(name)); if (r && r.error) { toast(r.error.message || 'Delete failed', false); return; } toast('Template deleted'); renderTemplates(document.getElementById('cb')); } catch (e) { toast(e.message, false); }
}

async function loadTemplateHistory() {
  var el = PCV.uxlib.el;
  try {
    const r = await fetchGet(EP.TEMPLATE_HISTORY());
    const list = unwrapList(r);
    var parts = [el('h2', null, '\u{1F4CB} Template History')];
    if (list.length === 0) { parts.push(el('p', { class: 'color-muted' }, 'No template changes recorded')); }
    else {
      var rows = list.map(e => el('tr', null,
        el('td', { class: 'text-xs' }, e.timestamp || '-'),
        el('td', null, HN.badge(e.action || '-', e.action === 'create' ? 'g' : e.action === 'delete' ? 'r' : 'y')),
        el('td', null, e.template || e.name || '-'),
        el('td', null, e.user || '-')));
      parts.push(el('table', null,
        el('thead', null, el('tr', null,
          el('th', null, 'Timestamp'), el('th', null, 'Action'), el('th', null, 'Template'), el('th', null, 'User'))),
        el('tbody', null, rows)));
    }
    parts.push(el('div', { class: 'text-right mt-12' },
      el('button', { class: 'btn btn-r', onclick: 'closeModal()' }, t('btn.close'))));
    showModal(parts);
  } catch (e) { toast('Template history error: ' + e.message, false); }
}

/* ═══ CONFIG MANAGEMENT ═══ */
async function configBackup() {
  toast('Backing up configuration...');
  try {
    const r = await fetchPost(EP.CONFIG_BACKUP(), {});
    if (r.error) { toast('Backup failed: ' + (r.error.message || ''), false); return; }
    toast('Config backup created'); addEvt('Config backup');
  } catch (e) { toast(e.message, false); }
}

async function configHistory() {
  var el = PCV.uxlib.el;
  try {
    const r = await fetchGet(EP.CONFIG_HISTORY());
    const list = unwrapList(r);
    var parts = [el('h2', null, '\u{1F4CB} Configuration History')];
    if (list.length === 0) { parts.push(el('p', { class: 'color-muted' }, 'No configuration changes recorded')); }
    else {
      var rows = list.map(e => el('tr', null,
        el('td', { class: 'text-xs' }, e.timestamp || e.time || '-'),
        el('td', null, e.key || e.param || '-'),
        el('td', null, e.old_value || '-'),
        el('td', { class: 'color-accent' }, e.new_value || e.value || '-'),
        el('td', null, e.user || '-')));
      parts.push(el('table', null,
        el('thead', null, el('tr', null,
          el('th', null, 'Timestamp'), el('th', null, 'Key'), el('th', null, 'Old Value'), el('th', null, 'New Value'), el('th', null, 'User'))),
        el('tbody', null, rows)));
    }
    parts.push(el('div', { class: 'text-right mt-12' },
      el('button', { class: 'btn btn-r', onclick: 'closeModal()' }, t('btn.close'))));
    showModal(parts);
  } catch (e) { toast('Config history error: ' + e.message, false); }
}

async function renderConfigMgmt(b) {
  showSkeleton(b);
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  var cfg = {};
  try { var r = await fetchGet(EP.CONFIG_DAEMON()); cfg = unwrapData(r) || r || {}; } catch(e) {}
  var stg = cfg.storage || {};
  var ctr = cfg.container || {};

  clearEl(b);
  b.appendChild(frag(
    HN.section('\u{2699} Configuration Management'),

    /* 스토리지 풀 설정 */
    el('h3', { style: 'margin:16px 0 10px' }, '\u{1F4BE} ' + _L('스토리지 풀 설정', 'Storage Pool Settings')),
    el('div', { class: 'sg grid-2 mb-14' },
      HN.card('\u{1F4BE} VM Storage', [
        el('p', { class: 'stat-label', style: 'margin-bottom:10px;line-height:1.6' },
          _splitBr(_L('ZFS Pool: ZFS 데이터셋 이름 (예: pcvpool/vms) — zvol 블록 디바이스로 VM 디스크 생성<br>'
             + 'Image Dir: ZFS 미사용 시 qcow2 파일 저장 경로 (예: /var/lib/libvirt/images)<br>'
             + 'ISO Dirs: ISO/IMG 파일 탐색 경로 (콤마 구분)',
               'ZFS Pool: ZFS dataset name (e.g. pcvpool/vms) — creates zvol block devices<br>'
             + 'Image Dir: qcow2 fallback path for non-ZFS (e.g. /var/lib/libvirt/images)<br>'
             + 'ISO Dirs: ISO/IMG scan paths (comma separated)'))),
        el('div', { class: 'fr' },
          el('label', { for: 'cfg-zvol', style: 'min-width:140px' }, 'VM ZFS Pool'),
          el('input', { id: 'cfg-zvol', value: escapeHtml(stg.zvol_pool || 'pcvpool/vms'), placeholder: 'pcvpool/vms', class: 'flex-1' })),
        el('div', { class: 'fr' },
          el('label', { for: 'cfg-imgdir', style: 'min-width:140px' }, 'Image Dir (qcow2)'),
          el('input', { id: 'cfg-imgdir', value: escapeHtml(stg.image_dir || '/var/lib/libvirt/images'), placeholder: '/var/lib/libvirt/images', class: 'flex-1' })),
        el('div', { class: 'fr' },
          el('label', { for: 'cfg-iso', style: 'min-width:140px' }, 'ISO Dirs'),
          el('input', { id: 'cfg-iso', value: escapeHtml(stg.iso_dirs || ''), placeholder: '/pcvpool/iso,/iso', class: 'flex-1' })),
        el('button', { class: 'btn btn-g mt-8', onclick: "saveStorageCfg('vm')" }, '\u{1F4BE} ' + _L('저장', 'Save')),
        el('div', { id: 'cfg-vm-result', style: 'margin-top:6px;font-size:11px' })
      ]),
      HN.card('\u{2637} Container Storage', [
        el('p', { class: 'stat-label', style: 'margin-bottom:10px;line-height:1.6' },
          _splitBr(_L('ZFS Pool: 컨테이너 ZFS 데이터셋 (예: pcvpool/containers)<br>'
             + 'LXC Path: 컨테이너 설정/rootfs 저장 경로',
               'ZFS Pool: Container ZFS dataset (e.g. pcvpool/containers)<br>'
             + 'LXC Path: Container config/rootfs storage path'))),
        el('div', { class: 'fr' },
          el('label', { for: 'cfg-ctrpool', style: 'min-width:140px' }, 'Container ZFS Pool'),
          el('input', { id: 'cfg-ctrpool', value: escapeHtml(stg.container_pool || 'pcvpool/containers'), placeholder: 'pcvpool/containers', class: 'flex-1' })),
        el('div', { class: 'fr' },
          el('label', { for: 'cfg-lxcpath', style: 'min-width:140px' }, 'LXC Path'),
          el('input', { id: 'cfg-lxcpath', value: escapeHtml(ctr.lxc_path || '/var/lib/purecvisor/lxc'), placeholder: '/var/lib/purecvisor/lxc', class: 'flex-1' })),
        el('button', { class: 'btn btn-g mt-8', onclick: "saveStorageCfg('ctr')" }, '\u{1F4BE} ' + _L('저장', 'Save')),
        el('div', { id: 'cfg-ctr-result', style: 'margin-top:6px;font-size:11px' })
      ])),

    /* 기존 백업/히스토리 */
    el('h3', { style: 'margin:16px 0 10px' }, '\u{1F4CB} ' + _L('설정 관리', 'Config Management')),
    el('div', { class: 'sg grid-2 mb-14' },
      HN.card('\u{1F4BE} Config Backup', [
        el('p', { class: 'stat-label mb-8' }, _L('현재 daemon.conf를 백업합니다.', 'Create a backup of current daemon.conf.')),
        el('button', { class: 'btn btn-g', onclick: 'configBackup()' }, '\u{1F4BE} Create Backup'),
        el('div', { id: 'cfg-backup-result', class: 'mt-8' })
      ]),
      HN.card('\u{1F4CB} Config History', [
        el('div', { id: 'cfg-history' }, el('span', { class: 'spinner' }), ' Loading...')
      ]))
  ));
  setTimeout(loadConfigHistoryInline, 50);
}

async function saveStorageCfg(type) {
  var pairs;
  var resultEl;
  if (type === 'vm') {
    resultEl = document.getElementById('cfg-vm-result');
    /* ZFS Pool 필드 검증: /로 시작하면 경고 */
    var zvolVal = (document.getElementById('cfg-zvol')?.value || '').trim();
    if (zvolVal.startsWith('/')) {
      if (resultEl) PCV.uxlib.setMsg(resultEl, null, { cls: 'color-red' },
        '⚠ ', _L('ZFS Pool은 파일 경로가 아닌 ZFS 데이터셋 이름이어야 합니다 (예: pcvpool/vms).', 'ZFS Pool must be a ZFS dataset name (e.g. pcvpool/vms), not a path.'),
        PCV.uxlib.el('br'),
        _L('파일시스템 경로에 저장하려면 Image Dir 필드를 사용하세요.', 'Use Image Dir for filesystem paths.'));
      return;
    }
    pairs = [
      { section: 'storage', key: 'zvol_pool', value: zvolVal },
      { section: 'storage', key: 'image_dir', value: document.getElementById('cfg-imgdir')?.value },
      { section: 'storage', key: 'iso_dirs', value: document.getElementById('cfg-iso')?.value }
    ];
  } else {
    resultEl = document.getElementById('cfg-ctr-result');
    var ctrPoolVal = (document.getElementById('cfg-ctrpool')?.value || '').trim();
    if (ctrPoolVal.startsWith('/')) {
      if (resultEl) PCV.uxlib.setMsg(resultEl, null, { cls: 'color-red' },
        '⚠ ', _L('Container ZFS Pool은 ZFS 데이터셋 이름이어야 합니다 (예: pcvpool/containers).', 'Container ZFS Pool must be a ZFS dataset name (e.g. pcvpool/containers).'));
      return;
    }
    pairs = [
      { section: 'storage', key: 'container_pool', value: ctrPoolVal },
      { section: 'container', key: 'lxc_path', value: document.getElementById('cfg-lxcpath')?.value }
    ];
  }
  if (resultEl) PCV.uxlib.setMsg(resultEl, 'loading', null, _L('저장 중...', 'Saving...'));
  try {
    for (var i = 0; i < pairs.length; i++) {
      if (pairs[i].value) {
        var r = await fetchPut(EP.CONFIG_DAEMON(), pairs[i]);
        if (r && r.error) {
          if (resultEl) PCV.uxlib.setMsg(resultEl, null, { cls: 'color-red' }, '❌ ', r.error.message || _L('저장 실패', 'Save failed'));
          toast(r.error.message || _L('저장 실패', 'Save failed'), false);
          return;
        }
      }
    }
    if (resultEl) PCV.uxlib.setMsg(resultEl, null, { cls: 'color-green' }, '✅ ', _L('저장 완료 (재시작 시 적용)', 'Saved (restart to apply)'));
    toast(_L('스토리지 설정 저장됨', 'Storage config saved'));
  } catch(e) {
    if (resultEl) PCV.uxlib.setMsg(resultEl, null, { cls: 'color-red' }, '❌ ', e.message);
    toast(e.message, false);
  }
}

async function loadConfigHistoryInline() {
  const box = document.getElementById('cfg-history'); if (!box) return;
  var el = PCV.uxlib.el, clearEl = PCV.uxlib.clearEl;
  try {
    const r = await fetchGet(EP.CONFIG_HISTORY());
    const list = unwrapList(r);
    if (list.length === 0) { PCV.uxlib.setMsg(box, null, { tag: 'p', cls: 'color-muted text-12' }, 'No configuration changes recorded.'); return; }
    var rows = list.slice(0, 20).map(e => el('tr', null,
      el('td', { class: 'text-xs' }, e.timestamp || e.time || '-'),
      el('td', null, e.key || '-'),
      el('td', { class: 'color-accent' }, e.new_value || e.value || '-')));
    clearEl(box);
    box.appendChild(el('table', { class: 'text-11' },
      el('thead', null, el('tr', null, el('th', null, 'Time'), el('th', null, 'Key'), el('th', null, 'Value'))),
      el('tbody', null, rows)));
    if (list.length > 20) box.appendChild(el('p', { class: 'stat-label' }, 'Showing 20 of ' + list.length + ' entries'));
  } catch (e) { PCV.uxlib.setMsg(box, null, { tag: 'p', cls: 'color-muted text-12' }, 'Config history not available.'); }
}

/* ═══ OVA IMPORT ═══ */
function showImportOva() {
  var el = PCV.uxlib.el;
  showModal([
    el('h2', null, '\u{1F4E6} Import OVA'),
    el('div', { class: 'fr' }, el('label', { for: 'ova-path' }, 'OVA Path'), el('input', { id: 'ova-path', placeholder: '/path/to/vm.ova', class: 'flex-1' })),
    el('div', { class: 'fr' }, el('label', { for: 'ova-name' }, 'VM Name'), el('input', { id: 'ova-name', placeholder: 'imported-vm' })),
    el('div', { class: 'fr' }, el('label', { for: 'ova-pool' }, 'Pool'), el('input', { id: 'ova-pool', value: 'pcvpool/vms' })),
    el('div', { class: 'text-right mt-12' },
      el('button', { class: 'btn btn-g', onclick: 'doImportOva()' }, 'Import'),
      ' ',
      el('button', { class: 'btn btn-r', onclick: 'closeModal()' }, t('btn.cancel')))
  ]);
}

async function doImportOva() {
  const path = document.getElementById('ova-path')?.value;
  const name = document.getElementById('ova-name')?.value;
  if (!path) { toast('OVA path required', false); return; }
  toast('Importing OVA...');
  try {
    const r = await fetchPost(EP.OVA_IMPORT(), { ova_path: path, name: name || '', pool: document.getElementById('ova-pool')?.value || '' });
    if (r.error) { toast('Import failed: ' + (r.error.message || ''), false); return; }
    toast('OVA import started' + (name ? ': ' + name : '')); addEvt('OVA import: ' + (name || path));
    closeModal(); setTimeout(loadAll, 3000);
  } catch (e) { toast(e.message, false); }
}

/* ═══ WINDOW REGISTRATIONS ═══ */
window.renderTemplates = renderTemplates;
window.showTemplateCreate = showTemplateCreate;
window.doTemplateCreate = doTemplateCreate;
window.templateUse = templateUse;
window.templateDel = templateDel;
window.loadTemplateHistory = loadTemplateHistory;
window.configBackup = configBackup;
window.configHistory = configHistory;
window.renderConfigMgmt = renderConfigMgmt;
window.loadConfigHistoryInline = loadConfigHistoryInline;
window.saveStorageCfg = saveStorageCfg;
window.showImportOva = showImportOva;
window.doImportOva = doImportOva;

/* ═══ CONFIG RELOAD (백엔드 4차) ═══ */
async function doConfigReload() {
  if (!await customConfirm(_L('데몬 설정을 리로드하시겠습니까?\n(webhook, rate limit, alert 임계값 등이 갱신됩니다)',
      'Reload daemon configuration?\n(webhook, rate limit, alert thresholds will be refreshed)'))) return;
  try {
    const r = await fetchPost(EP.CONFIG_RELOAD(), {});
    if (r && r.error) { toast(_L('리로드 실패', 'Reload failed') + ': ' + (r.error.message || ''), false); return; }
    toast(_L('설정 리로드 완료', 'Configuration reloaded'), 's');
  } catch(e) { toast(_L('리로드 실패', 'Reload failed') + ': ' + (e.message || ''), false); }
}

/* ═══ BACKUP SNAPSHOT VERIFY ═══ */
function showBackupVerify() {
  var mk = PCV.uxlib.el;
  var snapInput = mk('input', { id: 'verify-snap', placeholder: 'pcvpool/vms/web-prod@daily-20260401' });
  showModal([
    mk('h2', null, _L('스냅샷 무결성 검증', 'Verify Snapshot Integrity')),
    mk('div', { class: 'fr' },
      mk('label', { for: 'verify-snap' }, _L('스냅샷 이름', 'Snapshot Name')),
      snapInput),
    mk('div', { class: 'text-right mt-12' },
      mk('button', { class: 'btn btn-g', onClick: async function() {
        var snap = snapInput.value.trim();
        if (!snap) { toast(_L('스냅샷 이름 필수', 'Snapshot name required'), 'w'); return; }
        try {
          var r = await fetchPost(EP.BACKUP_VERIFY(), { snapshot: snap });
          if (r && r.error) { toast(_L('검증 실패', 'Verification failed') + ': ' + (r.error.message || ''), false); return; }
          var d = unwrapData(r) || {};
          toast('✅ ' + esc(d.snapshot || snap) + ': ' + (d.integrity || 'ok'), 's');
          closeModal();
        } catch(e) { toast(_L('검증 실패', 'Verification failed'), false); }
      } }, _L('검증', 'Verify')),
      ' ',
      mk('button', { class: 'btn btn-r', onclick: 'closeModal()' }, t('btn.cancel')))
  ]);
}

/* ═══ PERSISTENT JOBS ═══ */
async function renderPersistentJobs(b) {
  showSkeleton(b);
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  try {
    var r = await fetchGet(EP.JOBS_PERSIST());
    var list = unwrapList(r);
    var parts = [HN.section(_L('영속 작업 목록', 'Persistent Jobs'))];
    if (list.length === 0) {
      parts.push(el('div', { class: 'empty-state', style: 'padding:30px;text-align:center' },
        el('div', { style: 'font-size:36px;opacity:.5' }, '\u{1F4CB}'),
        el('div', { class: 'color-muted' }, _L('진행 중인 작업 없음', 'No pending jobs'))));
    } else {
      var rows = list.map(function(j) {
        return el('tr', null,
          el('td', null, j.job_id || ''),
          el('td', null, j.type || ''),
          el('td', null, j.status || ''),
          el('td', null, j.vm_name || ''));
      });
      parts.push(el('table', { class: 'data-table text-11' },
        el('thead', null, el('tr', null,
          el('th', null, 'ID'), el('th', null, _L('유형', 'Type')), el('th', null, _L('상태', 'Status')), el('th', null, _L('VM', 'VM')))),
        el('tbody', null, rows)));
    }
    clearEl(b); b.appendChild(frag(parts));
  } catch(e) { PCV.uxlib.setMsg(b, null, { tag: 'p', cls: 'color-muted' }, _L('로드 실패', 'Failed')); }
}

/* ═══ DB MIGRATION STATUS ═══ */
async function renderDbMigration(b) {
  showSkeleton(b);
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  try {
    var r = await fetchGet(EP.DB_MIGRATION());
    var d = unwrapData(r);
    clearEl(b);
    b.appendChild(frag(
      HN.section(_L('DB 스키마 상태', 'Database Schema Status')),
      el('div', { class: 'grid-3', style: 'gap:12px' },
        HN.statCard(_L('스키마 버전', 'Schema Version'), d.schema_version || 1, '📋'),
        HN.statCard(_L('상태', 'Status'), d.status || 'ok', '✅'),
        HN.statCard('RBAC DB', d.rbac_db || '-', '🗄️'))));
  } catch(e) { PCV.uxlib.setMsg(b, null, { tag: 'p', cls: 'color-muted' }, _L('로드 실패', 'Failed')); }
}

/* ═══ DEEP HEALTH (확장) ═══ */
async function renderDeepHealth(b) {
  showSkeleton(b);
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  try {
    var r = await fetchGet(EP.HEALTH_DEEP());
    var d = unwrapData(r);
    clearEl(b);
    b.appendChild(frag(
      HN.section(_L('심화 헬스 체크', 'Deep Health Check')),
      el('div', { class: 'grid-3', style: 'gap:12px' },
        HN.statCard('ZFS Pool', d.zfs_pool || '?', d.zfs_pool === 'ok' ? '🟢' : '🔴'),
        HN.statCard('nftables', (d.nftables_rules || 0) + _L('개 규칙', ' rules'), '🛡️'),
        HN.statCard(_L('전체', 'Overall'), d.status || '?', d.status === 'ok' ? '✅' : '⚠️'))));
  } catch(e) { PCV.uxlib.setMsg(b, null, { tag: 'p', cls: 'color-muted' }, _L('로드 실패', 'Failed')); }
}

window.doConfigReload = doConfigReload;
window.showBackupVerify = showBackupVerify;
window.renderPersistentJobs = renderPersistentJobs;
window.renderDbMigration = renderDbMigration;
window.renderDeepHealth = renderDeepHealth;

/* ═══ PCV.advanced namespace export ═══ */
PCV.advanced = {
  renderTemplates: renderTemplates,
  showTemplateCreate: showTemplateCreate,
  doTemplateCreate: doTemplateCreate,
  templateUse: templateUse,
  templateDel: templateDel,
  loadTemplateHistory: loadTemplateHistory,
  configBackup: configBackup,
  configHistory: configHistory,
  renderConfigMgmt: renderConfigMgmt,
  loadConfigHistoryInline: loadConfigHistoryInline,
  saveStorageCfg: saveStorageCfg,
  showImportOva: showImportOva,
  doImportOva: doImportOva,
  doConfigReload: doConfigReload,
  showBackupVerify: showBackupVerify,
  renderPersistentJobs: renderPersistentJobs,
  renderDbMigration: renderDbMigration,
  renderDeepHealth: renderDeepHealth
};

})(window.PCV);
