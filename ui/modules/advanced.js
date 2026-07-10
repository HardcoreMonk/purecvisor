/* ═══════════════════════════════════════════════════════════════
   PureCVisor — modules/advanced.js
   Templates, Docker/OCI, Terraform, Config Management,
   OVA Import
   ADR-0013: IIFE module scope — PCV.advanced namespace
   ═══════════════════════════════════════════════════════════════ */
/*
 * Advanced screens are optional capability frontends. A missing backend should
 * render an explanatory empty state, while configured backends must still use
 * EP registry helpers and sanitizer paths before inserting returned data.
 */
window.PCV = window.PCV || {};
(function(PCV) {

/* ═══ TEMPLATES ═══ */
async function renderTemplates(b) {
  b.innerHTML = showSkeleton();
  try {
    const r = await fetchGet(EP.TEMPLATES());
    const list = unwrapList(r);
    let h = H.section('&#128195; VM Templates <button class="btn btn-g" onclick="showTemplateCreate()" style="margin-left:8px">+ Create Template</button> <button class="btn" onclick="loadTemplateHistory()" style="margin-left:4px">&#128203; History</button>');
    if (list.length === 0) { h += '<div class="empty-state" style="text-align:center;padding:40px 20px"><div style="font-size:48px;margin-bottom:12px;opacity:.5">&#128195;</div><div style="font-size:14px;color:var(--fg2);margin-bottom:16px">No templates found</div><button class="btn btn-g" onclick="showTemplateCreate()" class="text-12">+ Create Template</button></div>'; b.innerHTML = h; return; }
    h += '<table><thead><tr><th>Name</th><th>vCPU</th><th>Memory</th><th>Disk</th><th>OS</th><th>Actions</th></tr></thead><tbody>';
    list.forEach(t2 => {
      h += '<tr><td><b>' + escapeHtml(t2.name || '-') + '</b></td><td>' + (t2.vcpu || '-') + '</td><td>' + (t2.memory_mb || '-') + ' MB</td><td>' + (t2.disk_gb || '-') + ' GB</td><td>' + escapeHtml(t2.os_variant || '-') + '</td><td><button class="btn" style="font-size:10px;padding:3px 8px" onclick="templateUse(\'' + escapeHtml(t2.name) + '\')">Use</button> <button class="btn btn-r" style="font-size:10px;padding:3px 8px" onclick="templateDel(\'' + escapeHtml(t2.name) + '\')">' + t('btn.delete') + '</button></td></tr>';
    });
    h += '</tbody></table>';
    h += '<div id="tpl-history"></div>';
    b.innerHTML = h;
  } catch (e) { b.innerHTML = '<p class="color-red">' + escapeHtml(e.message) + '</p>'; }
}

function showTemplateCreate() {
  showModal('<h2>Create VM Template</h2><div class="fr"><label for="tpl-name">Name</label><input id="tpl-name" placeholder="web-small"></div><div class="fr"><label for="tpl-vcpu">vCPU</label><input id="tpl-vcpu" type="number" value="2"></div><div class="fr"><label for="tpl-mem">Memory (MB)</label><input id="tpl-mem" type="number" value="2048"></div><div class="fr"><label for="tpl-disk">Disk (GB)</label><input id="tpl-disk" type="number" value="20"></div><div class="fr"><label for="tpl-os">OS Variant</label><input id="tpl-os" value="ubuntu24.04"></div><div class="text-right mt-12"><button class="btn btn-g" onclick="doTemplateCreate()">' + t('btn.create') + '</button> <button class="btn btn-r" onclick="closeModal()">' + t('btn.cancel') + '</button></div>');
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
  try { await fetchDelete(EP.TEMPLATE(name)); toast('Template deleted'); renderTemplates(document.getElementById('cb')); } catch (e) { toast(e.message, false); }
}

async function loadTemplateHistory() {
  try {
    const r = await fetchGet(EP.TEMPLATE_HISTORY());
    const list = unwrapList(r);
    let h = '<h2>&#128203; Template History</h2>';
    if (list.length === 0) { h += '<p class="color-muted">No template changes recorded</p>'; }
    else {
      h += '<table><thead><tr><th>Timestamp</th><th>Action</th><th>Template</th><th>User</th></tr></thead><tbody>';
      list.forEach(e => { h += '<tr><td class="text-xs">' + escapeHtml(e.timestamp || '-') + '</td><td>' + H.badge(e.action || '-', e.action === 'create' ? 'g' : e.action === 'delete' ? 'r' : 'y') + '</td><td>' + escapeHtml(e.template || e.name || '-') + '</td><td>' + escapeHtml(e.user || '-') + '</td></tr>'; });
      h += '</tbody></table>';
    }
    h += '<div class="text-right mt-12"><button class="btn btn-r" onclick="closeModal()">' + t('btn.close') + '</button></div>';
    showModal(h);
  } catch (e) { toast('Template history error: ' + e.message, false); }
}

/* ═══ DOCKER/OCI CONTAINERS ═══ */
async function renderDocker(b) {
  b.innerHTML = showSkeleton();
  try {
    const r = await fetchGet(EP.DOCKER_LIST());
    const list = unwrapList(r);
    let h = H.section('&#128051; Docker/OCI Containers');
    h += '<div class="flex gap-6 mb-14"><button class="btn btn-g" onclick="showDockerPull()">&#128229; Pull Image</button><button class="btn btn-g" onclick="showDockerRun()">&#9654; Run Container</button></div>';
    if (list.length === 0) { h += '<div class="empty-state" style="text-align:center;padding:40px 20px"><div style="font-size:48px;margin-bottom:12px;opacity:.5">&#128051;</div><div style="font-size:14px;color:var(--fg2);margin-bottom:16px">No Docker containers running</div><button class="btn btn-g" onclick="showDockerPull()" class="text-12">Pull Image</button></div>'; b.innerHTML = h; return; }
    h += '<table><thead><tr><th>Name/ID</th><th>Image</th><th>State</th><th>Ports</th><th>Actions</th></tr></thead><tbody>';
    list.forEach(c => {
      h += '<tr><td><b>' + escapeHtml(c.name || c.id || '-') + '</b></td><td>' + escapeHtml(c.image || '-') + '</td><td>' + H.badge(c.state || c.status || '-', (c.state || '').toLowerCase() === 'running' ? 'g' : 'r') + '</td><td class="text-xs">' + escapeHtml(c.ports || '-') + '</td><td><button class="btn btn-r" style="font-size:10px;padding:3px 8px" onclick="dockerStop(\'' + escapeHtml(c.name || c.id || '') + '\')">Stop</button></td></tr>';
    });
    h += '</tbody></table>';
    b.innerHTML = h;
  } catch (e) { b.innerHTML = H.section('&#128051; Docker/OCI Containers') + '<div class="flex gap-6 mb-14"><button class="btn btn-g" onclick="showDockerPull()">&#128229; Pull Image</button><button class="btn btn-g" onclick="showDockerRun()">&#9654; Run Container</button></div><p class="color-muted">Docker/OCI backend not available. Containers will appear here when docker.list RPC is implemented.</p>'; }
}

function showDockerPull() {
  showModal('<h2>&#128229; Pull OCI Image</h2><div class="fr"><label for="dk-image">Image</label><input id="dk-image" placeholder="nginx:latest" class="flex-1"></div><div class="text-right mt-12"><button class="btn btn-g" onclick="doDockerPull()">Pull</button> <button class="btn btn-r" onclick="closeModal()">' + t('btn.cancel') + '</button></div>');
}

async function doDockerPull() {
  const img = document.getElementById('dk-image')?.value;
  if (!img) { toast('Image name required', false); return; }
  toast('Pulling ' + img + '...');
  try { const r = await fetchPost(EP.DOCKER_PULL(), { image: img });
    if (r.error) { toast('Pull failed: ' + (r.error.message || ''), false); return; }
    toast('Image pulled: ' + img); addEvt('Docker pull: ' + img); closeModal();
  } catch (e) { toast(e.message, false); }
}

function showDockerRun() {
  showModal('<h2>&#9654; Run OCI Container</h2><div class="fr"><label for="dkr-image">Image</label><input id="dkr-image" placeholder="nginx:latest" class="flex-1"></div><div class="fr"><label for="dkr-name">Name</label><input id="dkr-name" placeholder="my-container"></div><div class="fr"><label for="dkr-ports">Ports</label><input id="dkr-ports" placeholder="8080:80"></div><div class="fr"><label for="dkr-env">Environment</label><input id="dkr-env" placeholder="KEY=VAL,KEY2=VAL2"></div><div class="text-right mt-12"><button class="btn btn-g" onclick="doDockerRun()">Run</button> <button class="btn btn-r" onclick="closeModal()">' + t('btn.cancel') + '</button></div>');
}

async function doDockerRun() {
  const img = document.getElementById('dkr-image')?.value;
  if (!img) { toast('Image required', false); return; }
  try { const r = await fetchPost(EP.DOCKER_RUN(), { image: img, name: document.getElementById('dkr-name')?.value || '', ports: document.getElementById('dkr-ports')?.value || '', env: document.getElementById('dkr-env')?.value || '' });
    if (r.error) { toast('Run failed: ' + (r.error.message || ''), false); return; }
    toast('Container started: ' + img); addEvt('Docker run: ' + img); closeModal(); renderDocker(document.getElementById('cb'));
  } catch (e) { toast(e.message, false); }
}

async function dockerStop(name) {
  if (!await customConfirm('Stop Container', name + '?')) return;
  try { const r = await fetchPost(EP.DOCKER_STOP(name), {});
    if (r.error) { toast('Stop failed: ' + (r.error.message || ''), false); return; }
    toast('Container stopped: ' + name); renderDocker(document.getElementById('cb'));
  } catch (e) { toast(e.message, false); }
}

/* ═══ TERRAFORM IaC ═══ */
async function renderTerraform(b) {
  b.innerHTML = showSkeleton();
  let h = H.section('&#127981; Terraform IaC Integration');
  h += '<div class="sg grid-2 mb-14">';
  h += H.card('&#128203; Terraform Plan', '<p class="stat-label mb-8">Preview infrastructure changes before applying.</p><div class="fr"><label for="tf-config">Config (HCL/JSON)</label><textarea id="tf-config" placeholder="resource \\"purecvisor_vm\\" \\"web\\" {\\n  name = \\"web-01\\"\\n  vcpu = 2\\n}" style="width:100%;min-height:100px;background:var(--bg);border:1px solid var(--border);color:var(--fg);border-radius:4px;padding:8px;font-family:monospace;font-size:11px"></textarea></div><div class="flex gap-6 mt-8"><button class="btn" onclick="tfPlan()">&#128203; Plan</button><button class="btn btn-g" onclick="tfApply()">&#9989; Apply</button></div><div id="tf-plan-result" class="mt-8"></div>');
  h += H.card('&#128202; Terraform State', '<div id="tf-state"><span class="spinner"></span> Loading state...</div>');
  h += '</div>';
  b.innerHTML = h;
  setTimeout(loadTfState, 50);
}

async function tfPlan() {
  const el = document.getElementById('tf-plan-result'); if (!el) return;
  const config = document.getElementById('tf-config')?.value;
  el.innerHTML = '<span class="spinner"></span> Planning...';
  try { const r = await fetchPost(EP.TERRAFORM_PLAN(), { config: config || '' });
    const d = unwrapData(r);
    el.innerHTML = '<pre style="background:var(--bg);padding:8px;border-radius:4px;font-size:11px;max-height:200px;overflow-y:auto;color:var(--green);white-space:pre-wrap">' + escapeHtml(d.plan || d.output || JSON.stringify(d, null, 2)) + '</pre>';
  } catch (e) { el.innerHTML = '<span class="color-red">Plan error: ' + escapeHtml(e.message) + '</span>'; }
}

async function tfApply() {
  if (!await customConfirm('Terraform Apply', 'Apply infrastructure changes?')) return;
  const el = document.getElementById('tf-plan-result'); if (el) el.innerHTML = '<span class="spinner"></span> Applying...';
  const config = document.getElementById('tf-config')?.value;
  try { const r = await fetchPost(EP.TERRAFORM_APPLY(), { config: config || '' });
    const d = unwrapData(r);
    if (el) el.innerHTML = '<pre style="background:var(--bg);padding:8px;border-radius:4px;font-size:11px;max-height:200px;overflow-y:auto;color:var(--accent);white-space:pre-wrap">' + escapeHtml(d.output || JSON.stringify(d, null, 2)) + '</pre>';
    toast('Terraform apply complete'); addEvt('Terraform apply');
  } catch (e) { if (el) el.innerHTML = '<span class="color-red">Apply error: ' + escapeHtml(e.message) + '</span>'; }
}

async function loadTfState() {
  const el = document.getElementById('tf-state'); if (!el) return;
  try { const r = await fetchGet(EP.TERRAFORM_STATE());
    const d = unwrapData(r);
    const resources = d.resources || d.state || [];
    if (Array.isArray(resources) && resources.length > 0) {
      let h = '<table class="text-11"><thead><tr><th>Type</th><th>Name</th><th>Status</th></tr></thead><tbody>';
      resources.forEach(res => { h += '<tr><td>' + escapeHtml(res.type || '-') + '</td><td>' + escapeHtml(res.name || '-') + '</td><td>' + H.badge(res.status || 'managed', 'g') + '</td></tr>'; });
      h += '</tbody></table>'; el.innerHTML = h;
    } else { el.innerHTML = '<p class="color-muted text-12">No Terraform state. Use Plan + Apply to manage infrastructure as code.</p>'; }
  } catch (e) { el.innerHTML = '<p class="color-muted text-12">Terraform state not available. Configure terraform.* RPC handlers to enable IaC.</p>'; }
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
  try {
    const r = await fetchGet(EP.CONFIG_HISTORY());
    const list = unwrapList(r);
    let h = '<h2>&#128203; Configuration History</h2>';
    if (list.length === 0) { h += '<p class="color-muted">No configuration changes recorded</p>'; }
    else {
      h += '<table><thead><tr><th>Timestamp</th><th>Key</th><th>Old Value</th><th>New Value</th><th>User</th></tr></thead><tbody>';
      list.forEach(e => { h += '<tr><td class="text-xs">' + escapeHtml(e.timestamp || e.time || '-') + '</td><td>' + escapeHtml(e.key || e.param || '-') + '</td><td>' + escapeHtml(e.old_value || '-') + '</td><td class="color-accent">' + escapeHtml(e.new_value || e.value || '-') + '</td><td>' + escapeHtml(e.user || '-') + '</td></tr>'; });
      h += '</tbody></table>';
    }
    h += '<div class="text-right mt-12"><button class="btn btn-r" onclick="closeModal()">' + t('btn.close') + '</button></div>';
    showModal(h);
  } catch (e) { toast('Config history error: ' + e.message, false); }
}

async function renderConfigMgmt(b) {
  b.innerHTML = showSkeleton();
  var cfg = {};
  try { var r = await fetchGet(EP.CONFIG_DAEMON()); cfg = unwrapData(r) || r || {}; } catch(e) {}
  var stg = cfg.storage || {};
  var ctr = cfg.container || {};

  var h = H.section('&#9881; Configuration Management');

  /* 스토리지 풀 설정 */
  h += '<h3 style="margin:16px 0 10px">&#128190; ' + _L('스토리지 풀 설정', 'Storage Pool Settings') + '</h3>';
  h += '<div class="sg grid-2 mb-14">';
  h += H.card('&#128190; VM Storage', ''
    + '<p class="stat-label" style="margin-bottom:10px;line-height:1.6">'
    + _L('ZFS Pool: ZFS 데이터셋 이름 (예: pcvpool/vms) — zvol 블록 디바이스로 VM 디스크 생성<br>'
       + 'Image Dir: ZFS 미사용 시 qcow2 파일 저장 경로 (예: /var/lib/libvirt/images)<br>'
       + 'ISO Dirs: ISO/IMG 파일 탐색 경로 (콤마 구분)',
         'ZFS Pool: ZFS dataset name (e.g. pcvpool/vms) — creates zvol block devices<br>'
       + 'Image Dir: qcow2 fallback path for non-ZFS (e.g. /var/lib/libvirt/images)<br>'
       + 'ISO Dirs: ISO/IMG scan paths (comma separated)')
    + '</p>'
    + '<div class="fr"><label for="cfg-zvol" style="min-width:140px">VM ZFS Pool</label><input id="cfg-zvol" value="' + escapeHtml(stg.zvol_pool || 'pcvpool/vms') + '" placeholder="pcvpool/vms" class="flex-1"></div>'
    + '<div class="fr"><label for="cfg-imgdir" style="min-width:140px">Image Dir (qcow2)</label><input id="cfg-imgdir" value="' + escapeHtml(stg.image_dir || '/var/lib/libvirt/images') + '" placeholder="/var/lib/libvirt/images" class="flex-1"></div>'
    + '<div class="fr"><label for="cfg-iso" style="min-width:140px">ISO Dirs</label><input id="cfg-iso" value="' + escapeHtml(stg.iso_dirs || '') + '" placeholder="/pcvpool/iso,/iso" class="flex-1"></div>'
    + '<button class="btn btn-g mt-8" onclick="saveStorageCfg(\'vm\')">&#128190; ' + _L('저장', 'Save') + '</button>'
    + '<div id="cfg-vm-result" style="margin-top:6px;font-size:11px"></div>');
  h += H.card('&#9783; Container Storage', ''
    + '<p class="stat-label" style="margin-bottom:10px;line-height:1.6">'
    + _L('ZFS Pool: 컨테이너 ZFS 데이터셋 (예: pcvpool/containers)<br>'
       + 'LXC Path: 컨테이너 설정/rootfs 저장 경로',
         'ZFS Pool: Container ZFS dataset (e.g. pcvpool/containers)<br>'
       + 'LXC Path: Container config/rootfs storage path')
    + '</p>'
    + '<div class="fr"><label for="cfg-ctrpool" style="min-width:140px">Container ZFS Pool</label><input id="cfg-ctrpool" value="' + escapeHtml(stg.container_pool || 'pcvpool/containers') + '" placeholder="pcvpool/containers" class="flex-1"></div>'
    + '<div class="fr"><label for="cfg-lxcpath" style="min-width:140px">LXC Path</label><input id="cfg-lxcpath" value="' + escapeHtml(ctr.lxc_path || '/var/lib/purecvisor/lxc') + '" placeholder="/var/lib/purecvisor/lxc" class="flex-1"></div>'
    + '<button class="btn btn-g mt-8" onclick="saveStorageCfg(\'ctr\')">&#128190; ' + _L('저장', 'Save') + '</button>'
    + '<div id="cfg-ctr-result" style="margin-top:6px;font-size:11px"></div>');
  h += '</div>';

  /* 기존 백업/히스토리 */
  h += '<h3 style="margin:16px 0 10px">&#128203; ' + _L('설정 관리', 'Config Management') + '</h3>';
  h += '<div class="sg grid-2 mb-14">';
  h += H.card('&#128190; Config Backup', '<p class="stat-label mb-8">' + _L('현재 daemon.conf를 백업합니다.', 'Create a backup of current daemon.conf.') + '</p><button class="btn btn-g" onclick="configBackup()">&#128190; Create Backup</button><div id="cfg-backup-result" class="mt-8"></div>');
  h += H.card('&#128203; Config History', '<div id="cfg-history"><span class="spinner"></span> Loading...</div>');
  h += '</div>';
  b.innerHTML = h;
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
      if (resultEl) resultEl.innerHTML = '<span class="color-red">&#9888; ' + _L(
        'ZFS Pool은 파일 경로가 아닌 ZFS 데이터셋 이름이어야 합니다 (예: pcvpool/vms).<br>파일시스템 경로에 저장하려면 Image Dir 필드를 사용하세요.',
        'ZFS Pool must be a ZFS dataset name (e.g. pcvpool/vms), not a path.<br>Use Image Dir for filesystem paths.') + '</span>';
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
      if (resultEl) resultEl.innerHTML = '<span class="color-red">&#9888; ' + _L(
        'Container ZFS Pool은 ZFS 데이터셋 이름이어야 합니다 (예: pcvpool/containers).',
        'Container ZFS Pool must be a ZFS dataset name (e.g. pcvpool/containers).') + '</span>';
      return;
    }
    pairs = [
      { section: 'storage', key: 'container_pool', value: ctrPoolVal },
      { section: 'container', key: 'lxc_path', value: document.getElementById('cfg-lxcpath')?.value }
    ];
  }
  if (resultEl) resultEl.innerHTML = '<span class="spinner"></span> ' + _L('저장 중...', 'Saving...');
  try {
    for (var i = 0; i < pairs.length; i++) {
      if (pairs[i].value) await fetchPut(EP.CONFIG_DAEMON(), pairs[i]);
    }
    if (resultEl) resultEl.innerHTML = '<span class="color-green">&#9989; ' + _L('저장 완료 (재시작 시 적용)', 'Saved (restart to apply)') + '</span>';
    toast(_L('스토리지 설정 저장됨', 'Storage config saved'));
  } catch(e) {
    if (resultEl) resultEl.innerHTML = '<span class="color-red">&#10060; ' + escapeHtml(e.message) + '</span>';
    toast(e.message, false);
  }
}

async function loadConfigHistoryInline() {
  const el = document.getElementById('cfg-history'); if (!el) return;
  try {
    const r = await fetchGet(EP.CONFIG_HISTORY());
    const list = unwrapList(r);
    if (list.length === 0) { el.innerHTML = '<p class="color-muted text-12">No configuration changes recorded.</p>'; return; }
    let h = '<table class="text-11"><thead><tr><th>Time</th><th>Key</th><th>Value</th></tr></thead><tbody>';
    list.slice(0, 20).forEach(e => { h += '<tr><td class="text-xs">' + escapeHtml(e.timestamp || e.time || '-') + '</td><td>' + escapeHtml(e.key || '-') + '</td><td class="color-accent">' + escapeHtml(e.new_value || e.value || '-') + '</td></tr>'; });
    h += '</tbody></table>';
    if (list.length > 20) h += '<p class="stat-label">Showing 20 of ' + list.length + ' entries</p>';
    el.innerHTML = h;
  } catch (e) { el.innerHTML = '<p class="color-muted text-12">Config history not available.</p>'; }
}

/* ═══ OVA IMPORT ═══ */
function showImportOva() {
  showModal('<h2>&#128230; Import OVA</h2><div class="fr"><label for="ova-path">OVA Path</label><input id="ova-path" placeholder="/path/to/vm.ova" class="flex-1"></div><div class="fr"><label for="ova-name">VM Name</label><input id="ova-name" placeholder="imported-vm"></div><div class="fr"><label for="ova-pool">Pool</label><input id="ova-pool" value="pcvpool/vms"></div><div class="text-right mt-12"><button class="btn btn-g" onclick="doImportOva()">Import</button> <button class="btn btn-r" onclick="closeModal()">' + t('btn.cancel') + '</button></div>');
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
window.renderDocker = renderDocker;
window.showDockerPull = showDockerPull;
window.doDockerPull = doDockerPull;
window.showDockerRun = showDockerRun;
window.doDockerRun = doDockerRun;
window.dockerStop = dockerStop;
window.renderTerraform = renderTerraform;
window.tfPlan = tfPlan;
window.tfApply = tfApply;
window.loadTfState = loadTfState;
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
    await fetchPost(EP.CONFIG_RELOAD(), {});
    toast(_L('설정 리로드 완료', 'Configuration reloaded'), 's');
  } catch(e) { toast(_L('리로드 실패', 'Reload failed') + ': ' + (e.message || ''), 'e'); }
}

/* ═══ BACKUP SNAPSHOT VERIFY ═══ */
async function showBackupVerify() {
  var html = '<div class="form-group"><label for="verify-snap">' + _L('스냅샷 이름', 'Snapshot Name') + '</label>';
  html += '<input id="verify-snap" class="input-field" placeholder="pcvpool/vms/web-prod@daily-20260401"></div>';
  showModal(_L('스냅샷 무결성 검증', 'Verify Snapshot Integrity'), html, async function() {
    var snap = document.getElementById('verify-snap').value.trim();
    if (!snap) { toast(_L('스냅샷 이름 필수', 'Snapshot name required'), 'w'); return; }
    try {
      var r = await fetchPost(EP.BACKUP_VERIFY(), { snapshot: snap });
      var d = unwrapData(r);
      toast('✅ ' + esc(d.snapshot) + ': ' + (d.integrity || 'ok'), 's');
    } catch(e) { toast(_L('검증 실패', 'Verification failed'), 'e'); }
  });
}

/* ═══ PERSISTENT JOBS ═══ */
async function renderPersistentJobs(b) {
  b.innerHTML = showSkeleton();
  try {
    var r = await fetchGet(EP.JOBS_PERSIST());
    var list = unwrapList(r);
    var h = H.section(_L('영속 작업 목록', 'Persistent Jobs'));
    if (list.length === 0) {
      h += '<div class="empty-state" style="padding:30px;text-align:center"><div style="font-size:36px;opacity:.5">&#128203;</div>';
      h += '<div class="color-muted">' + _L('진행 중인 작업 없음', 'No pending jobs') + '</div></div>';
    } else {
      h += '<table class="data-table text-11"><thead><tr>';
      h += '<th>ID</th><th>' + _L('유형', 'Type') + '</th><th>' + _L('상태', 'Status') + '</th><th>' + _L('VM', 'VM') + '</th></tr></thead><tbody>';
      list.forEach(function(j) {
        h += '<tr><td>' + esc(j.job_id || '') + '</td><td>' + esc(j.type || '') + '</td>';
        h += '<td>' + esc(j.status || '') + '</td><td>' + esc(j.vm_name || '') + '</td></tr>';
      });
      h += '</tbody></table>';
    }
    b.innerHTML = h;
  } catch(e) { b.innerHTML = '<p class="color-muted">' + _L('로드 실패', 'Failed') + '</p>'; }
}

/* ═══ DB MIGRATION STATUS ═══ */
async function renderDbMigration(b) {
  b.innerHTML = showSkeleton();
  try {
    var r = await fetchGet(EP.DB_MIGRATION());
    var d = unwrapData(r);
    var h = H.section(_L('DB 스키마 상태', 'Database Schema Status'));
    h += '<div class="grid-3" style="gap:12px">';
    h += H.statCard(_L('스키마 버전', 'Schema Version'), d.schema_version || 1, '📋');
    h += H.statCard(_L('상태', 'Status'), d.status || 'ok', '✅');
    h += H.statCard('RBAC DB', d.rbac_db || '-', '🗄️');
    h += '</div>';
    b.innerHTML = h;
  } catch(e) { b.innerHTML = '<p class="color-muted">' + _L('로드 실패', 'Failed') + '</p>'; }
}

/* ═══ DEEP HEALTH (확장) ═══ */
async function renderDeepHealth(b) {
  b.innerHTML = showSkeleton();
  try {
    var r = await fetchGet(EP.HEALTH_DEEP());
    var d = unwrapData(r);
    var h = H.section(_L('심화 헬스 체크', 'Deep Health Check'));
    h += '<div class="grid-3" style="gap:12px">';
    h += H.statCard('ZFS Pool', d.zfs_pool || '?', d.zfs_pool === 'ok' ? '🟢' : '🔴');
    h += H.statCard('nftables', (d.nftables_rules || 0) + _L('개 규칙', ' rules'), '🛡️');
    h += H.statCard(_L('전체', 'Overall'), d.status || '?', d.status === 'ok' ? '✅' : '⚠️');
    h += '</div>';
    b.innerHTML = h;
  } catch(e) { b.innerHTML = '<p class="color-muted">' + _L('로드 실패', 'Failed') + '</p>'; }
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
  renderDocker: renderDocker,
  showDockerPull: showDockerPull,
  doDockerPull: doDockerPull,
  showDockerRun: showDockerRun,
  doDockerRun: doDockerRun,
  dockerStop: dockerStop,
  renderTerraform: renderTerraform,
  tfPlan: tfPlan,
  tfApply: tfApply,
  loadTfState: loadTfState,
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
