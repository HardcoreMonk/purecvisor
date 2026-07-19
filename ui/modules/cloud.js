
window.PCV = window.PCV || {};
(function(PCV) {

var _cloudPollTimer = null;

window.addEventListener('beforeunload', function() {
  if (_cloudPollTimer) { clearInterval(_cloudPollTimer); _cloudPollTimer = null; }
});

function _cloudCleanupTimer() {
  if (_cloudPollTimer) { clearInterval(_cloudPollTimer); _cloudPollTimer = null; }
}
window._cloudCleanupTimer = _cloudCleanupTimer;

async function renderCloudMigration(b) {
  showSkeleton(b);

  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  var importPanel = el('div', { class: 'hc' },
    el('h4', { style: 'color:var(--accent)' }, '📥 Import (EC2 → PureCVisor)'),
    el('p', { class: 'stat-label', style: 'margin-bottom:10px' }, 'AWS EC2 AMI를 PureCVisor VM으로 가져옵니다. EBS→S3→다운로드→qcow2 변환→VM 생성'),
    el('div', { class: 'fr' }, el('label', { for: 'cm-imp-name' }, 'VM Name'), el('input', { id: 'cm-imp-name', placeholder: 'web-prod' })),
    el('div', { class: 'fr' }, el('label', { for: 'cm-imp-ami' }, 'AMI ID'), el('input', { id: 'cm-imp-ami', placeholder: 'ami-0abcdef1234' })),
    el('div', { class: 'fr' }, el('label', { for: 'cm-imp-region' }, 'Region'),
      el('select', { id: 'cm-imp-region', style: 'width:100%;padding:6px;background:var(--bg);border:1px solid var(--border);color:var(--fg);border-radius:4px' },
        el('option', { value: 'ap-northeast-2' }, 'ap-northeast-2 (Seoul)'),
        el('option', { value: 'us-east-1' }, 'us-east-1 (Virginia)'),
        el('option', { value: 'us-west-2' }, 'us-west-2 (Oregon)'),
        el('option', { value: 'eu-west-1' }, 'eu-west-1 (Ireland)'),
        el('option', { value: 'ap-southeast-1' }, 'ap-southeast-1 (Singapore)'))),
    el('div', { class: 'fr' }, el('label', { for: 'cm-imp-bucket' }, 'S3 Bucket'), el('input', { id: 'cm-imp-bucket', placeholder: 'pcv-migration' })),
    el('div', { class: 'fr' }, el('label', { for: 'cm-imp-vcpu' }, 'vCPU'), el('input', { id: 'cm-imp-vcpu', type: 'number', value: '2', min: '1', max: '64', style: 'width:80px' })),
    el('div', { class: 'fr' }, el('label', { for: 'cm-imp-mem' }, 'Memory (MB)'), el('input', { id: 'cm-imp-mem', type: 'number', value: '2048', style: 'width:100px' })),
    el('div', { class: 'fr' }, el('label', { for: 'cm-imp-br' }, 'Bridge'), el('input', { id: 'cm-imp-br', value: 'pcvbr0', style: 'width:120px' })),
    el('div', { class: 'fr' }, el('label', { for: 'cm-imp-mode' }, 'Mode'),
      el('select', { id: 'cm-imp-mode', style: 'width:100%;padding:6px;background:var(--bg);border:1px solid var(--border);color:var(--fg);border-radius:4px' },
        el('option', { value: 'standard' }, 'Standard (full download)'),
        el('option', { value: 'near-live' }, 'Near-Live (2-phase, minimal downtime)'))),
    el('button', { class: 'btn btn-g', onclick: 'cmDoImport()', style: 'margin-top:8px;width:100%' }, '📥 Start Import'));

  var exportPanel = el('div', { class: 'hc' },
    el('h4', { style: 'color:var(--green)' }, '📦 Export (PureCVisor → EC2)'),
    el('p', { class: 'stat-label', style: 'margin-bottom:10px' }, 'PureCVisor VM을 AWS EC2 AMI로 내보냅니다. qcow2→RAW→S3→AMI 등록'),
    el('div', { class: 'fr' }, el('label', { for: 'cm-exp-name' }, 'VM Name'),
      el('select', { id: 'cm-exp-name', style: 'width:100%;padding:6px;background:var(--bg);border:1px solid var(--border);color:var(--fg);border-radius:4px' },
        el('option', { value: '' }, t('loading')))),
    el('div', { class: 'fr' }, el('label', { for: 'cm-exp-region' }, 'Region'),
      el('select', { id: 'cm-exp-region', style: 'width:100%;padding:6px;background:var(--bg);border:1px solid var(--border);color:var(--fg);border-radius:4px' },
        el('option', { value: 'ap-northeast-2' }, 'ap-northeast-2 (Seoul)'),
        el('option', { value: 'us-east-1' }, 'us-east-1 (Virginia)'),
        el('option', { value: 'us-west-2' }, 'us-west-2 (Oregon)'),
        el('option', { value: 'eu-west-1' }, 'eu-west-1 (Ireland)'))),
    el('div', { class: 'fr' }, el('label', { for: 'cm-exp-bucket' }, 'S3 Bucket'), el('input', { id: 'cm-exp-bucket', placeholder: 'pcv-migration' })),
    el('div', { class: 'fr' }, el('label', { for: 'cm-exp-ami-name' }, 'AMI Name'), el('input', { id: 'cm-exp-ami-name', placeholder: 'web-prod-exported' })),
    el('div', { class: 'fr' }, el('label', { for: 'cm-exp-desc' }, 'Description'), el('input', { id: 'cm-exp-desc', placeholder: 'Exported from PureCVisor' })),
    el('button', { class: 'btn btn-g', onclick: 'cmDoExport()', style: 'margin-top:8px;width:100%' }, '📦 Start Export'));

  var jobsPanel = el('div', { class: 'hc', style: 'margin-bottom:14px' },
    el('h4', null, '📊 Migration Jobs'),
    el('div', { id: 'cm-jobs' }, el('span', { class: 'spinner' }), ' Loading...'));

  var pipelineCard = HN.card('🛠 Pipeline Reference',
    el('div', { style: 'font-size:11px;line-height:1.8;color:var(--fg2)' },
      el('b', { style: 'color:var(--accent)' }, 'Import:'), ' aws sts verify → ec2 export-image → S3 download → qemu-img convert → virt-customize → VM define', el('br'),
      el('b', { style: 'color:var(--green)' }, 'Export:'), ' qemu-img convert → S3 upload → ec2 import-image → AMI ready', el('br'),
      el('b', { style: 'color:var(--yellow)' }, 'Near-Live:'), ' Phase1 사전동기화(실행 중) → Phase2 델타전송(2~5분 중단)'));

  clearEl(b);
  b.appendChild(frag(
    HN.section('☁ Cloud Migration — AWS EC2 ↔ PureCVisor'),
    el('div', { class: 'sg grid-2 mb-14' }, importPanel, exportPanel),
    jobsPanel,
    pipelineCard
  ));

  try {
    const vl = vmList.length ? vmList : [];
    const sel = document.getElementById('cm-exp-name');
    if (sel && vl.length) {

      PCV.uxlib.clearEl(sel);
      vl.forEach(function(v) {
        sel.appendChild(PCV.uxlib.el('option', { value: v.name }, v.name + ' (' + v.state + ')'));
      });
    } else if (sel) { PCV.uxlib.clearEl(sel); sel.appendChild(PCV.uxlib.el('option', { value: '' }, 'No VMs')); }
  } catch (e) {  }

  cmLoadJobs();
  if (_cloudPollTimer) clearInterval(_cloudPollTimer);

  _cloudPollTimer = setInterval(() => { if (document.hidden) return; cmLoadJobs(); }, 5000);
}
window.renderCloudMigration = renderCloudMigration;

async function cmLoadJobs() {
  const el = document.getElementById('cm-jobs');
  if (!el) { if (_cloudPollTimer) { clearInterval(_cloudPollTimer); _cloudPollTimer = null; } return; }

  try {
    const r = await fetchGet(EP.CLOUD_JOBS());
    const jobs = unwrapList(r);
    if (!Array.isArray(jobs) || jobs.length === 0) {
      PCV.uxlib.clearEl(el);
      el.appendChild(PCV.uxlib.el('p', { class: 'color-muted', style: 'font-size:12px' }, 'No migration jobs. Start an Import or Export above.'));
      return;
    }

    var mk = PCV.uxlib.el;
    var thead = mk('thead', null, mk('tr', null,
      mk('th', null, 'VM'), mk('th', null, 'Dir'), mk('th', null, 'Status'),
      mk('th', null, 'Progress'), mk('th', null, 'Detail'), mk('th', null, 'Elapsed'), mk('th')));
    var tbody = mk('tbody', null, jobs.map(function(j) {
      const pct = j.progress_percent || 0;
      const st = j.status || '?';
      const color = st === 'done' ? 'var(--green)' : st === 'failed' ? 'var(--red)' : 'var(--accent)';
      const active = st !== 'done' && st !== 'failed';
      const awaitingCutover = st === 'awaiting_cutover';
      var actions = [];
      if (active && !awaitingCutover) actions.push(mk('button', { class: 'btn btn-r', style: 'font-size:10px;padding:2px 8px', onclick: "cmCancelJob('" + esc(j.name) + "')" }, 'Cancel'));
      if (awaitingCutover) actions.push(mk('button', { class: 'btn btn-g', style: 'font-size:10px;padding:2px 8px', onclick: "cmFinalize('" + esc(j.name) + "')" }, 'Finalize'));
      return mk('tr', null,
        mk('td', null, mk('b', null, j.name || '')),
        mk('td', null, HN.badge(j.direction || '?', j.direction === 'import' ? 'y' : 'g')),
        mk('td', null, HN.badge(st, st === 'done' ? 'g' : st === 'failed' ? 'r' : awaitingCutover ? 'y' : 'y')),
        mk('td', null, mk('div', { class: 'pb', style: 'min-width:120px' },
          mk('div', { class: 'pb-f', style: 'width:' + pct + '%;background:' + color }),
          mk('div', { class: 'pb-t' }, pct + '%'))),
        mk('td', { class: 'text-xs' }, j.detail || '-'),
        mk('td', { class: 'text-xs' }, (j.elapsed_sec || 0) + 's'),
        mk('td', null, actions));
    }));
    PCV.uxlib.clearEl(el);
    el.appendChild(mk('table', null, thead, tbody));
  } catch (e) {  }
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
