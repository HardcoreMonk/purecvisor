/* ═══════════════════════════════════════════════════════════════
   PureCVisor — modules/cloud.js
   Cloud Migration (AWS EC2 <-> PureCVisor)
   ADR-0013: IIFE module scope — PCV.cloud namespace
   ═══════════════════════════════════════════════════════════════ */
window.PCV = window.PCV || {};
(function(PCV) {

var _cloudPollTimer = null;

/* 페이지 이탈 시 타이머 정리 (FE-4: 폴 타이머 누수 방지) */
window.addEventListener('beforeunload', function() {
  if (_cloudPollTimer) { clearInterval(_cloudPollTimer); _cloudPollTimer = null; }
});

/* 네비게이션 변경 시 타이머 정리 */
function _cloudCleanupTimer() {
  if (_cloudPollTimer) { clearInterval(_cloudPollTimer); _cloudPollTimer = null; }
}
window._cloudCleanupTimer = _cloudCleanupTimer;

async function renderCloudMigration(b) {
  b.innerHTML = showSkeleton();
  let h = H.section('&#9729; Cloud Migration — AWS EC2 &#8596; PureCVisor');
  h += '<div class="sg grid-2 mb-14">';

  /* Import 폼 */
  h += '<div class="hc"><h4 style="color:var(--accent)">&#128229; Import (EC2 &#8594; PureCVisor)</h4>';
  h += '<p class="stat-label" style="margin-bottom:10px">AWS EC2 AMI를 PureCVisor VM으로 가져옵니다. EBS→S3→다운로드→qcow2 변환→VM 생성</p>';
  h += '<div class="fr"><label>VM Name</label><input id="cm-imp-name" placeholder="web-prod"></div>';
  h += '<div class="fr"><label>AMI ID</label><input id="cm-imp-ami" placeholder="ami-0abcdef1234"></div>';
  h += '<div class="fr"><label>Region</label><select id="cm-imp-region" style="width:100%;padding:6px;background:var(--bg);border:1px solid var(--border);color:var(--fg);border-radius:4px"><option value="ap-northeast-2">ap-northeast-2 (Seoul)</option><option value="us-east-1">us-east-1 (Virginia)</option><option value="us-west-2">us-west-2 (Oregon)</option><option value="eu-west-1">eu-west-1 (Ireland)</option><option value="ap-southeast-1">ap-southeast-1 (Singapore)</option></select></div>';
  h += '<div class="fr"><label>S3 Bucket</label><input id="cm-imp-bucket" placeholder="pcv-migration"></div>';
  h += '<div class="fr"><label>vCPU</label><input id="cm-imp-vcpu" type="number" value="2" min="1" max="64" style="width:80px"></div>';
  h += '<div class="fr"><label>Memory (MB)</label><input id="cm-imp-mem" type="number" value="2048" style="width:100px"></div>';
  h += '<div class="fr"><label>Bridge</label><input id="cm-imp-br" value="pcvbr0" style="width:120px"></div>';
  h += '<div class="fr"><label>Mode</label><select id="cm-imp-mode" style="width:100%;padding:6px;background:var(--bg);border:1px solid var(--border);color:var(--fg);border-radius:4px"><option value="standard">Standard (full download)</option><option value="near-live">Near-Live (2-phase, minimal downtime)</option></select></div>';
  h += '<button class="btn btn-g" onclick="cmDoImport()" style="margin-top:8px;width:100%">&#128229; Start Import</button>';
  h += '</div>';

  /* Export 폼 */
  h += '<div class="hc"><h4 style="color:var(--green)">&#128230; Export (PureCVisor &#8594; EC2)</h4>';
  h += '<p class="stat-label" style="margin-bottom:10px">PureCVisor VM을 AWS EC2 AMI로 내보냅니다. qcow2→RAW→S3→AMI 등록</p>';
  h += '<div class="fr"><label>VM Name</label><select id="cm-exp-name" style="width:100%;padding:6px;background:var(--bg);border:1px solid var(--border);color:var(--fg);border-radius:4px"><option value="">' + t('loading') + '</option></select></div>';
  h += '<div class="fr"><label>Region</label><select id="cm-exp-region" style="width:100%;padding:6px;background:var(--bg);border:1px solid var(--border);color:var(--fg);border-radius:4px"><option value="ap-northeast-2">ap-northeast-2 (Seoul)</option><option value="us-east-1">us-east-1 (Virginia)</option><option value="us-west-2">us-west-2 (Oregon)</option><option value="eu-west-1">eu-west-1 (Ireland)</option></select></div>';
  h += '<div class="fr"><label>S3 Bucket</label><input id="cm-exp-bucket" placeholder="pcv-migration"></div>';
  h += '<div class="fr"><label>AMI Name</label><input id="cm-exp-ami-name" placeholder="web-prod-exported"></div>';
  h += '<div class="fr"><label>Description</label><input id="cm-exp-desc" placeholder="Exported from PureCVisor"></div>';
  h += '<button class="btn btn-g" onclick="cmDoExport()" style="margin-top:8px;width:100%">&#128230; Start Export</button>';
  h += '</div></div>';

  /* 진행 상태 */
  h += '<div class="hc" style="margin-bottom:14px"><h4>&#128202; Migration Jobs</h4>';
  h += '<div id="cm-jobs"><span class="spinner"></span> Loading...</div></div>';

  /* 파이프라인 다이어그램 */
  h += H.card('&#128736; Pipeline Reference', '<div style="font-size:11px;line-height:1.8;color:var(--fg2)">'
    + '<b style="color:var(--accent)">Import:</b> aws sts verify &#8594; ec2 export-image &#8594; S3 download &#8594; qemu-img convert &#8594; virt-customize &#8594; VM define<br>'
    + '<b style="color:var(--green)">Export:</b> qemu-img convert &#8594; S3 upload &#8594; ec2 import-image &#8594; AMI ready<br>'
    + '<b style="color:var(--yellow)">Near-Live:</b> Phase1 사전동기화(실행 중) &#8594; Phase2 델타전송(2~5분 중단)</div>');

  b.innerHTML = h;

  /* VM 목록 로드 → Export 드롭다운 */
  try {
    const vl = vmList.length ? vmList : [];
    const sel = document.getElementById('cm-exp-name');
    if (sel && vl.length) {
      sel.innerHTML = vl.map(v => '<option value="' + escapeHtml(v.name) + '">' + escapeHtml(v.name) + ' (' + v.state + ')</option>').join('');
    } else if (sel) { sel.innerHTML = '<option value="">No VMs</option>'; }
  } catch (e) { /* ignore */ }

  /* 작업 상태 로드 + 폴링 시작 */
  cmLoadJobs();
  if (_cloudPollTimer) clearInterval(_cloudPollTimer);
  _cloudPollTimer = setInterval(cmLoadJobs, 5000);
}
window.renderCloudMigration = renderCloudMigration;

async function cmLoadJobs() {
  const el = document.getElementById('cm-jobs');
  if (!el) { if (_cloudPollTimer) { clearInterval(_cloudPollTimer); _cloudPollTimer = null; } return; }

  try {
    const r = await fetchGet(EP.CLOUD_JOBS());
    const jobs = unwrapList(r);
    if (!Array.isArray(jobs) || jobs.length === 0) {
      el.innerHTML = '<p class="color-muted" style="font-size:12px">No migration jobs. Start an Import or Export above.</p>';
      return;
    }

    let html = '<table><thead><tr><th>VM</th><th>Dir</th><th>Status</th><th>Progress</th><th>Detail</th><th>Elapsed</th><th></th></tr></thead><tbody>';
    for (const j of jobs) {
      const pct = j.progress_percent || 0;
      const st = j.status || '?';
      const color = st === 'done' ? 'var(--green)' : st === 'failed' ? 'var(--red)' : 'var(--accent)';
      const active = st !== 'done' && st !== 'failed';
      const awaitingCutover = st === 'awaiting_cutover';
      const cancelBtn = active && !awaitingCutover
        ? '<button class="btn btn-r" style="font-size:10px;padding:2px 8px" onclick="cmCancelJob(\'' + esc(j.name) + '\')">Cancel</button>'
        : '';
      const finalizeBtn = awaitingCutover
        ? '<button class="btn btn-g" style="font-size:10px;padding:2px 8px" onclick="cmFinalize(\'' + esc(j.name) + '\')">Finalize</button>'
        : '';
      html += '<tr>'
        + '<td><b>' + esc(j.name || '') + '</b></td>'
        + '<td>' + H.badge(j.direction || '?', j.direction === 'import' ? 'y' : 'g') + '</td>'
        + '<td>' + H.badge(st, st === 'done' ? 'g' : st === 'failed' ? 'r' : awaitingCutover ? 'y' : 'y') + '</td>'
        + '<td><div class="pb" style="min-width:120px"><div class="pb-f" style="width:' + pct + '%;background:' + color + '"></div><div class="pb-t">' + pct + '%</div></div></td>'
        + '<td class="text-xs">' + esc(j.detail || '-') + '</td>'
        + '<td class="text-xs">' + (j.elapsed_sec || 0) + 's</td>'
        + '<td>' + cancelBtn + finalizeBtn + '</td>'
        + '</tr>';
    }
    html += '</tbody></table>';
    el.innerHTML = html;
  } catch (e) { /* ignore polling errors */ }
}
window.cmLoadJobs = cmLoadJobs;

async function cmCancelJob(name) {
  if (!await customConfirm('Cancel migration job for ' + name + '?')) return;
  try {
    const r = await fetchPost(EP.CLOUD_CANCEL(), { name });
    if (r.error) { toast('Cancel failed: ' + (r.error.message || ''), false); return; }
    toast('Cancel requested: ' + name);
    cmLoadJobs();
  } catch (e) { toast('Cancel error: ' + e.message, false); }
}
window.cmCancelJob = cmCancelJob;

async function cmDoImport() {
  const name = document.getElementById('cm-imp-name')?.value;
  const ami = document.getElementById('cm-imp-ami')?.value;
  if (!name || !ami) { toast(t('msg.name_required'), false); return; }
  if (!/^ami-[a-f0-9]{8,17}$/.test(ami)) { toast(t('msg.invalid_ami'), false); return; }
  const body = {
    ami_id: ami,
    aws_region: document.getElementById('cm-imp-region')?.value || 'ap-northeast-2',
    s3_bucket: document.getElementById('cm-imp-bucket')?.value || '',
    vcpu: parseInt(document.getElementById('cm-imp-vcpu')?.value) || 2,
    memory_mb: parseInt(document.getElementById('cm-imp-mem')?.value) || 2048,
    network_bridge: document.getElementById('cm-imp-br')?.value || 'pcvbr0',
    mode: document.getElementById('cm-imp-mode')?.value || 'standard'
  };
  try {
    const r = await fetchPost(EP.CLOUD_IMPORT(name), body);
    if (r.error) { toast('Import failed: ' + (r.error.message || ''), false); return; }
    const d = unwrapData(r);
    toast('Import started: ' + name + ' (job: ' + (d.job_id || '') + ')');
    addEvt('Cloud Import started — ' + name + ' \u2190 AMI ' + ami);
    cmLoadJobs();
  } catch (e) { toast('Import error: ' + e.message, false); }
}
window.cmDoImport = cmDoImport;

async function cmDoExport() {
  const name = document.getElementById('cm-exp-name')?.value;
  if (!name) { toast('VM Name required', false); return; }
  const body = {
    aws_region: document.getElementById('cm-exp-region')?.value || 'ap-northeast-2',
    s3_bucket: document.getElementById('cm-exp-bucket')?.value || '',
    ami_name: document.getElementById('cm-exp-ami-name')?.value || '',
    ami_description: document.getElementById('cm-exp-desc')?.value || ''
  };
  try {
    const r = await fetchPost(EP.CLOUD_EXPORT(name), body);
    if (r.error) { toast('Export failed: ' + (r.error.message || ''), false); return; }
    const d = unwrapData(r);
    toast('Export started: ' + name + ' (job: ' + (d.job_id || '') + ')');
    addEvt('Cloud Export started — ' + name + ' \u2192 EC2 AMI');
    cmLoadJobs();
  } catch (e) { toast('Export error: ' + e.message, false); }
}
window.cmDoExport = cmDoExport;

async function cmFinalize(name) {
  if (!await customConfirm('Finalize Near-Live Import for ' + name + '?',
    'This will stop the EC2 instance, download delta changes, and start the VM in PureCVisor. Downtime: ~2-5 minutes.')) return;
  try {
    const r = await fetchPost(EP.CLOUD_IMPORT(name), { finalize: true });
    if (r.error) { toast('Finalize failed: ' + (r.error.message || ''), false); return; }
    toast('Finalize started: ' + name);
    cmLoadJobs();
  } catch (e) { toast('Finalize error: ' + e.message, false); }
}
window.cmFinalize = cmFinalize;

/* ═══ PCV.cloud namespace export ═══ */
PCV.cloud = {
  renderCloudMigration: renderCloudMigration,
  cmLoadJobs: cmLoadJobs,
  cmCancelJob: cmCancelJob,
  cmDoImport: cmDoImport,
  cmDoExport: cmDoExport,
  cmFinalize: cmFinalize,
  _cloudCleanupTimer: _cloudCleanupTimer
};

})(window.PCV);
