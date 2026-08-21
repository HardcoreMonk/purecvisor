                                                                  
                              
                                       
                                                  
                                                                     
   
                                                                           
  
                                                                               
                                                                               
                                         
  
                                              
                                                                                  
                                                                              
                                                      
  
                                        
                                                                         
                                                               
                          
  
                              
                                                       
                                                                       
  
                       
                                                     
                                                       
                                 
   
window.PCV = window.PCV || {};
(function (PCV) {
'use strict';

var state = {
  root: null,
  vpcs: [],
  backends: [],
  backendError: null,
  status: null,
  selectedId: null,
  detail: null,
  loadingDetail: false,
  busy: null,
  error: null
};

function L(ko, en) { return typeof _L === 'function' ? _L(ko, en) : ko; }
function dataOrThrow(response) {
  if (response && response.error) {
    throw new Error(response.error.message || response.error.code || L('요청 실패', 'Request failed'));
  }
  return typeof unwrapData === 'function' ? unwrapData(response) :
    (response && response.data !== undefined ? response.data : response);
}
function listOrThrow(response) {
  var data = dataOrThrow(response);
  return Array.isArray(data) ? data : [];
}
function shortId(value) {
  value = String(value || '');
  return value.length > 13 ? value.slice(0, 8) + '…' + value.slice(-4) : value;
}
function resourceTone(value) {
  var s = String(value || '').toUpperCase();
  if (s === 'ACTIVE' || s === 'COMPLETED') return 'pill-ok';
  if (s === 'ERROR' || s === 'FAILED' || s === 'QUARANTINED') return 'pill-crit';
  if (s === 'CREATING' || s === 'DELETING' || s === 'ALLOCATED' || s === 'RUNNING') return 'pill-warn';
  return 'pill-idle';
}
function pill(value) {
  return PCV.uxlib.el('span', { class: 'pill ' + resourceTone(value) }, String(value || 'UNKNOWN'));
}
function mono(value, title) {
  return PCV.uxlib.el('span', { class: 'vpc-mono', title: title || String(value || '') }, String(value || '—'));
}
function button(label, cls, handler, role, attrs) {
  var options = Object.assign({ class: 'btn ' + (cls || ''), type: 'button', onClick: handler }, attrs || {});
  if (role) options['data-role'] = role;
  return PCV.uxlib.el('button', options, label);
}
function applyRoles() {
  if (typeof applyRoleVisibility === 'function' && window.currentUser)
    applyRoleVisibility(window.currentUser.role);
}

async function _waitJob(jobId) {
  var deadline = Date.now() + 60000;
  while (Date.now() < deadline) {
    var response = await fetchGet(EP.JOB(jobId));
    if (response && response.error)
      throw new Error(response.error.message || response.error.code || L('Job 조회 실패', 'Job lookup failed'));
    var job = dataOrThrow(response);
    var status = job && job.status;
    if (status === 'completed') return job;
    if (status === 'failed' || status === 'cancelled') {
      var detail = job.detail || job.result || status;
      try {
        var parsed = typeof detail === 'string' ? JSON.parse(detail) : detail;
        detail = parsed && (parsed.error || parsed.message) || detail;
      } catch (_) {}
      throw new Error(String(detail));
    }
    await new Promise(function (resolve) { setTimeout(resolve, 100); });
  }
  throw new Error(L('작업 완료 확인 시간이 초과됐습니다', 'Timed out waiting for job completion'));
}

async function _runMutation(label, request, trigger, closeOnSuccess) {
  if (state.busy) return false;
  state.busy = label;
  if (trigger) {
    trigger.disabled = true;
    trigger.classList.add('is-loading');
  }
  _render();
  try {
    var accepted = dataOrThrow(await request());
    if (!accepted || accepted.status !== 'accepted' || !accepted.job_id)
      throw new Error(L('비동기 Job 접수 응답이 유효하지 않습니다', 'Invalid async job acceptance response'));
    var live = document.getElementById('vpc-live-status');
    if (live) live.textContent = label + ' · ' + accepted.job_id + ' · ' + L('완료 확인 중', 'checking completion');
    await _waitJob(accepted.job_id);
    if (closeOnSuccess && typeof closeModal === 'function') closeModal();
    if (typeof toast === 'function') toast(label + ' · ' + L('완료', 'completed'), true);
    if (typeof addEvt === 'function') addEvt(label + ' completed (' + accepted.job_id + ')');
    await _load(state.selectedId);
    return true;
  } catch (error) {
    var failure = error instanceof Error ? error : new Error(String(error));
                                                                     
                                                               
    if (!closeOnSuccess && typeof toast === 'function') toast(label + ': ' + failure.message, false);
    if (typeof addEvt === 'function') addEvt(label + ' failed: ' + failure.message);
    await _load(state.selectedId, true);
    if (closeOnSuccess) throw failure;
    return false;
  } finally {
    state.busy = null;
    if (trigger && trigger.isConnected) {
      trigger.disabled = false;
      trigger.classList.remove('is-loading');
    }
    _render();
  }
}

function formRow(label, control, help) {
  var el = PCV.uxlib.el;
  return el('div', { class: 'vpc-form-row' },
    el('label', { for: control.id }, label),
    control,
    help ? el('div', { class: 'vpc-form-help', id: control.id + '-help' }, help) : null);
}

                                                              
                                                                 
function subnetCapacity(value) {
  var match = String(value || '').trim().match(/^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})\/(\d{1,2})$/);
  if (!match) return null;
  var octets = match.slice(1, 5).map(Number), prefix = Number(match[5]);
  if (octets.some(function (part) { return part < 0 || part > 255; }) || prefix < 16 || prefix > 30)
    return null;
  var address = octets[0] * 16777216 + octets[1] * 65536 + octets[2] * 256 + octets[3];
  var size = Math.pow(2, 32 - prefix);
  var network = Math.floor(address / size) * size;
  if (address !== network) return null;
  function ipv4(number) {
    return [Math.floor(number / 16777216) % 256, Math.floor(number / 65536) % 256,
      Math.floor(number / 256) % 256, number % 256].join('.');
  }
  return {
    gateway: ipv4(network + 1),
    first: ipv4(network + 2),
    last: ipv4(network + size - 2),
    count: size - 3
  };
}

function modalForm(title, rows, submitLabel, onSubmit, opts) {
  var el = PCV.uxlib.el;
  var titleId = 'vpc-modal-title-' + Date.now();
  var error = el('div', { class: 'vpc-form-error', role: 'alert', 'aria-live': 'assertive', tabindex: '-1' });
  var submit = button(submitLabel, 'btn-primary', null, null, { type: 'submit', 'data-vpc-submit': '' });
  var form = el('form', {
    class: 'vpc-form',
    onSubmit: async function (event) {
      event.preventDefault();
      error.textContent = '';
      try { await onSubmit(form, submit); }
      catch (e) { error.textContent = e.message || String(e); error.focus(); }
    }
  }, rows, error,
    el('div', { class: 'vpc-form-actions' },
      button(L('취소', 'Cancel'), 'btn-soft', function () { closeModal(); }), submit));
  var dialog = showModal([el('h2', { id: titleId }, title), form], { wide: !!(opts && opts.wide) });
  if (dialog) dialog.setAttribute('aria-labelledby', titleId);
  return dialog;
}

function showCreateVpc() {
  var el = PCV.uxlib.el;
  var count = Number(state.status && state.status.vpc_count) || 0;
  var name = el('input', { id: 'vpc-create-name', class: 'input-pcv-lg', required: '', autofocus: '', pattern: '[A-Za-z0-9][A-Za-z0-9._-]{0,62}', 'aria-describedby': 'vpc-create-name-help' });
  var tenant = el('input', { id: 'vpc-create-tenant', class: 'input-pcv-lg', required: '', pattern: '[A-Za-z0-9][A-Za-z0-9._-]{0,62}', 'aria-describedby': 'vpc-create-tenant-help' });
  var backendItems = state.backends.length ? state.backends : [
    { id: 'linux', label: 'Linux bridge', ready: true, current_vpcs: count, allocatable_vpcs: null }
  ];
  var backend = el('select', { id: 'vpc-create-backend', class: 'input-pcv-lg',
    'aria-describedby': 'vpc-create-backend-help vpc-create-backend-status vpc-create-backend-unavailable' },
    backendItems.map(function (item) {
      var suffix = item.ready ? '' : L(' · 사용 불가', ' · unavailable');
      return el('option', { value: item.id, disabled: item.ready ? null : '' }, item.label + suffix);
    }));
  var backendNote = el('div', { class: 'vpc-contract-note', id: 'vpc-create-backend-status', role: 'status', 'aria-live': 'polite' });
  function updateBackendNote() {
    var item = backendItems.find(function (candidate) { return candidate.id === backend.value; }) || backendItems[0];
    var limit = item.allocatable_vpcs === null || item.allocatable_vpcs === undefined
      ? L('제품 고정 상한 없음', 'no fixed product limit')
      : L('추가 할당 가능 ', 'allocatable ') + item.allocatable_vpcs + L('개', '');
    backendNote.textContent = (item.label || item.id) + ' · ' +
      L('현재 ', 'current ') + Number(item.current_vpcs || 0) + L('개', '') + ' · ' + limit +
      (item.reason ? ' · ' + item.reason : '');
  }
  backend.addEventListener('change', updateBackendNote);
  updateBackendNote();
  var backendUnavailable = el('div', {
    class: 'vpc-contract-note vpc-contract-note-warn vpc-backend-unavailable',
    id: 'vpc-create-backend-unavailable'
  }, backendItems.filter(function (item) { return !item.ready; }).map(function (item) {
    return el('span', null, (item.label || item.id) + ' · ' + L('사용 불가', 'unavailable') + ' · ' +
      (item.reason || L('준비 상태 검사를 통과하지 못했습니다', 'readiness checks failed')));
  }));
  var mode = el('select', { id: 'vpc-create-egress', class: 'input-pcv-lg', 'aria-describedby': 'vpc-create-egress-help' },
    el('option', { value: 'nat' }, 'NAT'), el('option', { value: 'isolated' }, 'ISOLATED'));
  var subnetName = el('input', { id: 'vpc-create-subnet-name', class: 'input-pcv-lg', required: '', pattern: '[A-Za-z0-9][A-Za-z0-9._-]{0,62}', 'aria-describedby': 'vpc-create-subnet-name-help' });
  var subnetCidr = el('input', { id: 'vpc-create-subnet-cidr', class: 'input-pcv-lg', required: '', placeholder: '10.60.10.0/24', inputmode: 'text', 'aria-describedby': 'vpc-create-subnet-cidr-help' });
  var subnetMtu = el('input', { id: 'vpc-create-subnet-mtu', class: 'input-pcv-lg', type: 'number', required: '',
    min: '68', max: '9216', value: '1500', 'aria-describedby': 'vpc-create-subnet-mtu-help' });
  var capacity = el('div', { class: 'vpc-contract-note', id: 'vpc-create-subnet-capacity', role: 'status', 'aria-live': 'polite' },
    L('canonical IPv4 network /16~30을 입력하면 gateway와 VM 할당 범위를 계산합니다.', 'Enter a canonical IPv4 network /16–/30 to calculate its gateway and VM allocation range.'));
  subnetCidr.addEventListener('input', function () {
    var result = subnetCapacity(subnetCidr.value);
    capacity.textContent = result
      ? L('Gateway ', 'Gateway ') + result.gateway + L(' · VM ', ' · VM ') + result.first + '–' + result.last +
        L(' · 할당 가능 ', ' · assignable ') + result.count + L('개', ' addresses')
      : L('canonical IPv4 network /16~30을 입력하세요. 서버가 host·다른 subnet 중첩을 최종 검사합니다.', 'Enter a canonical IPv4 network /16–/30. The server makes the final host/subnet overlap check.');
  });
  modalForm(L('Local VPC 생성', 'Create Local VPC'), [
    el('fieldset', { class: 'vpc-form-group' },
      el('legend', null, 'VPC'),
      el('div', { class: 'vpc-form-grid' },
        formRow(L('이름', 'Name'), name, L('tenant 안에서 고유한 운영 이름', 'Unique operational name within the tenant')),
        formRow('Tenant', tenant, L('관리자도 mutation scope를 명시합니다.', 'Admins must explicitly select mutation scope.')),
        formRow('Backend', backend, L('생성 뒤 변경할 수 없습니다. OVN은 OVS br-int를 사용합니다.', 'Immutable after creation. OVN uses OVS br-int.')),
        formRow('Egress', mode, L('NAT는 outbound 허용, ISOLATED는 외부 통신 차단', 'NAT allows outbound; ISOLATED blocks external traffic')))),
    state.backendError ? el('div', { class: 'vpc-error vpc-backend-warning', role: 'alert' },
      L('Backend 상태를 읽지 못해 Linux만 선택할 수 있습니다: ', 'Could not load backend status; only Linux is available: ') + state.backendError) : null,
    backendNote,
    backendUnavailable,
    el('fieldset', { class: 'vpc-form-group' },
      el('legend', null, L('첫 Subnet', 'Initial subnet')),
      el('div', { class: 'vpc-form-grid' },
        formRow(L('Subnet 이름', 'Subnet name'), subnetName, L('VPC 안에서 구분할 운영 이름', 'Operational name within the VPC')),
        formRow('IPv4 CIDR', subnetCidr, L('host와 다른 VPC CIDR과 겹칠 수 없습니다.', 'Must not overlap host or another VPC CIDR.')),
        formRow('MTU', subnetMtu, L('기본값 1500 · 허용 범위 68~9216', 'Default 1500 · allowed range 68–9216'))),
      capacity),
    el('div', { class: 'vpc-contract-note' },
      el('strong', null, L('현재 VPC ', 'Current VPCs ') + count + L('개 · 제품 고정 상한 없음', ' · no fixed product limit')),
      el('span', null, L('VPC는 주소가 없는 경계이며, 입력한 첫 subnet을 같은 Job에서 함께 생성합니다.', 'The VPC is an addressless boundary; its initial subnet is created in the same job.')),
      el('span', null, L('/24 예시: gateway를 제외하고 VM에 253개 주소를 할당할 수 있습니다.', '/24 example: 253 addresses are assignable to VMs after reserving the gateway.')))
  ], L('VPC + Subnet 생성', 'Create VPC + subnet'), async function (_, submit) {
    var mtuValue = Number(subnetMtu.value);
    if (!name.value.trim() || !tenant.value.trim() || !subnetName.value.trim() ||
        !subnetCidr.value.trim() || !Number.isInteger(mtuValue) || !subnetCapacity(subnetCidr.value))
      throw new Error(L('VPC와 첫 subnet 입력을 확인하세요', 'Check the VPC and initial subnet values'));
    await _runMutation(L('VPC + Subnet 생성', 'Create VPC + subnet'), function () {
      return fetchPost(EP.VPC_LIST(), {
        name: name.value.trim(), tenant: tenant.value.trim(), egress_mode: mode.value, backend: backend.value,
        subnet_name: subnetName.value.trim(), subnet_cidr: subnetCidr.value.trim(), subnet_mtu: mtuValue
      });
    }, submit, true);
  }, { wide: true });
}

function showEgress() {
  if (!state.detail) return;
  var el = PCV.uxlib.el, detail = state.detail;
  var mode = el('select', { id: 'vpc-egress-mode', class: 'input-pcv-lg', 'aria-describedby': 'vpc-egress-mode-help' },
    el('option', { value: 'nat', selected: detail.egress_mode === 'nat' ? '' : null }, 'NAT'),
    el('option', { value: 'isolated', selected: detail.egress_mode === 'isolated' ? '' : null }, 'ISOLATED'));
  modalForm(L('Egress 변경', 'Change egress'), [
    formRow('Mode', mode, L('ISOLATED 전환은 기존 외부 통신을 즉시 차단합니다.', 'Switching to ISOLATED immediately blocks external traffic.')),
    PCV.uxlib.el('div', { class: 'vpc-contract-note' }, 'revision ' + detail.revision)
  ], L('변경 적용', 'Apply change'), async function (_, submit) {
    if (mode.value === detail.egress_mode) throw new Error(L('변경된 값이 없습니다', 'No changes to apply'));
    await _runMutation(L('Egress 변경', 'Change egress'), function () {
      return fetchPost(EP.VPC_EGRESS(detail.id), { tenant: detail.tenant, egress_mode: mode.value, expected_revision: detail.revision });
    }, submit, true);
  });
}

function showSubnetCreate() {
  if (!state.detail) return;
  var el = PCV.uxlib.el, detail = state.detail;
  var name = el('input', { id: 'vpc-subnet-name', class: 'input-pcv-lg', required: '', autofocus: '' });
  var cidr = el('input', { id: 'vpc-subnet-cidr', class: 'input-pcv-lg', required: '', placeholder: '10.60.10.0/24', inputmode: 'text' });
  var mtu = el('input', { id: 'vpc-subnet-mtu', class: 'input-pcv-lg', type: 'number', min: '68', max: '9216', value: '1500' });
  var capacity = el('div', { class: 'vpc-contract-note', id: 'vpc-subnet-capacity', role: 'status', 'aria-live': 'polite' },
    L('canonical IPv4 network /16~30을 입력하면 gateway와 VM 할당 범위를 계산합니다.', 'Enter a canonical IPv4 network /16–/30 to calculate its gateway and VM allocation range.'));
  cidr.addEventListener('input', function () {
    var result = subnetCapacity(cidr.value);
    capacity.textContent = result
      ? L('Gateway ', 'Gateway ') + result.gateway + L(' · VM ', ' · VM ') + result.first + '–' + result.last +
        L(' · 할당 가능 ', ' · assignable ') + result.count + L('개', ' addresses')
      : L('canonical IPv4 network /16~30을 입력하세요. 서버가 host·다른 subnet 중첩을 최종 검사합니다.', 'Enter a canonical IPv4 network /16–/30. The server makes the final host/subnet overlap check.');
  });
  modalForm(L('Subnet 생성', 'Create subnet'), [
    formRow(L('이름', 'Name'), name), formRow('IPv4 CIDR', cidr, L('host와 다른 VPC CIDR과 겹칠 수 없습니다.', 'Must not overlap host or another VPC CIDR.')),
    capacity, formRow('MTU', mtu), el('div', { class: 'vpc-contract-note' }, 'revision ' + detail.revision)
  ], L('Subnet 생성', 'Create subnet'), async function (_, submit) {
    var mtuValue = Number(mtu.value);
    if (!name.value.trim() || !cidr.value.trim() || !Number.isInteger(mtuValue)) throw new Error(L('입력을 확인하세요', 'Check the form values'));
    await _runMutation(L('Subnet 생성', 'Create subnet'), function () {
      return fetchPost(EP.VPC_SUBNETS(detail.id), { tenant: detail.tenant, name: name.value.trim(), cidr: cidr.value.trim(), mtu: mtuValue, expected_revision: detail.revision });
    }, submit, true);
  });
}

async function showAttachmentCreate() {
  if (!state.detail) return;
  var el = PCV.uxlib.el, detail = state.detail, subnets = detail.subnets || [];
  if (!subnets.length) { toast(L('먼저 subnet을 생성하세요', 'Create a subnet first'), false); return; }
  var vms = [];
  try {
    var vmResponse = await fetchGet(EP.VM_LIST());
    if (vmResponse && vmResponse.error)
      throw new Error(vmResponse.error.message || L('VM 목록 조회 실패', 'VM list failed'));
    vms = listOrThrow(vmResponse).filter(function (vm) { return String(vm.state || '').toLowerCase() !== 'running'; });
  }
  catch (_) {}
  var subnet = el('select', { id: 'vpc-attach-subnet', class: 'input-pcv-lg' }, subnets.map(function (s) { return el('option', { value: s.id }, s.name + ' · ' + s.cidr); }));
  var vm = el('input', { id: 'vpc-attach-vm', class: 'input-pcv-lg', required: '', autofocus: '', list: 'vpc-stopped-vms' });
  var datalist = el('datalist', { id: 'vpc-stopped-vms' }, vms.map(function (v) { return el('option', { value: v.name }); }));
  var ip = el('input', { id: 'vpc-attach-ip', class: 'input-pcv-lg', placeholder: L('자동 할당', 'Automatic allocation') });
  modalForm(L('정지 VM 연결', 'Attach stopped VM'), [
    formRow('Subnet', subnet), formRow('VM', vm, L('persistent XML 안전을 위해 정지 VM만 연결할 수 있습니다.', 'Only stopped VMs can be attached safely.')), datalist,
    formRow(L('요청 IP', 'Requested IP'), ip, L('비우면 subnet pool에서 자동 할당', 'Leave empty for automatic allocation'))
  ], L('VM 연결', 'Attach VM'), async function (_, submit) {
    if (!vm.value.trim()) throw new Error(L('VM 이름이 필요합니다', 'VM name is required'));
    var body = { tenant: detail.tenant, subnet_id: subnet.value, vm: vm.value.trim() };
    if (ip.value.trim()) body.ip_address = ip.value.trim();
    await _runMutation(L('VM 연결', 'Attach VM'), function () { return fetchPost(EP.VPC_ATTACHMENT_LIST(), body); }, submit, true);
  }, { wide: true });
}

function showPublish(attachment) {
  var el = PCV.uxlib.el, detail = state.detail;
  if (detail.egress_mode !== 'nat' || attachment.state !== 'ACTIVE') {
    toast(L('ACTIVE attachment가 있는 NAT VPC에서만 게시할 수 있습니다', 'Publishing requires an ACTIVE attachment in a NAT VPC'), false); return;
  }
  var protocol = el('select', { id: 'vpc-publish-protocol', class: 'input-pcv-lg' }, el('option', { value: 'tcp' }, 'TCP'), el('option', { value: 'udp' }, 'UDP'));
  var address = el('input', { id: 'vpc-publish-address', class: 'input-pcv-lg', value: '0.0.0.0', required: '' });
  var listen = el('input', { id: 'vpc-publish-listen', class: 'input-pcv-lg', type: 'number', min: '1', max: '65535', required: '' });
  var target = el('input', { id: 'vpc-publish-target', class: 'input-pcv-lg', type: 'number', min: '1', max: '65535', required: '' });
  var sources = el('textarea', { id: 'vpc-publish-sources', class: 'input-pcv-lg', rows: '3', required: '', placeholder: '192.0.2.0/24\n198.51.100.0/24', 'aria-describedby': 'vpc-publish-sources-help' });
  modalForm(L('Service Publish', 'Publish service'), [
    formRow('Protocol', protocol), formRow(L('수신 주소', 'Listen address'), address),
    formRow(L('수신 포트', 'Listen port'), listen), formRow(L('VM 포트', 'Target port'), target),
    formRow(L('허용 Source CIDR', 'Allowed source CIDRs'), sources, L('한 줄에 canonical CIDR 하나 이상', 'One canonical CIDR per line; at least one required'))
  ], L('서비스 게시', 'Publish service'), async function (_, submit) {
    var allowed = sources.value.split(/\n|,/).map(function (v) { return v.trim(); }).filter(Boolean);
    var listenPort = Number(listen.value), targetPort = Number(target.value);
    if (!allowed.length || !Number.isInteger(listenPort) || !Number.isInteger(targetPort)) throw new Error(L('포트와 source CIDR을 확인하세요', 'Check ports and source CIDRs'));
    await _runMutation(L('Service Publish', 'Publish service'), function () {
      return fetchPost(EP.VPC_SERVICE_LIST(), { tenant: detail.tenant, attachment_id: attachment.id, protocol: protocol.value, listen_address: address.value.trim(), listen_port: listenPort, target_port: targetPort, allowed_sources: allowed });
    }, submit, true);
  }, { wide: true });
}

async function confirmMutation(title, message, label, request) {
  if (!await customConfirm(title, message)) return;
  await _runMutation(label, request, null, false);
}
function deleteVpc(vpc) {
  return confirmMutation(L('VPC 삭제', 'Delete VPC'),
    vpc.name + ' (' + vpc.tenant + ')\n' + L('모든 child resource를 먼저 제거해야 하며 되돌릴 수 없습니다.', 'All child resources must be removed first. This cannot be undone.'),
    L('VPC 삭제', 'Delete VPC'), function () { return fetchDelete(EP.VPC_DETAIL(vpc.id), { tenant: vpc.tenant }); });
}
function deleteSubnet(subnet) {
  return confirmMutation(L('Subnet 삭제', 'Delete subnet'), subnet.name + ' · ' + subnet.cidr + '\n' + L('연결된 VM이 없어야 합니다.', 'No VM attachments may remain.'),
    L('Subnet 삭제', 'Delete subnet'), function () { return fetchDelete(EP.VPC_SUBNET(subnet.id), { tenant: state.detail.tenant }); });
}
function deleteAttachment(attachment) {
  return confirmMutation(L('VM 연결 해제', 'Detach VM'), attachment.vm_name + ' · ' + attachment.ip_address + '\n' + L('게시 서비스를 먼저 해제해야 합니다.', 'Published services must be removed first.'),
    L('VM 연결 해제', 'Detach VM'), function () { return fetchDelete(EP.VPC_ATTACHMENT(attachment.id), { tenant: state.detail.tenant }); });
}
function deleteService(service) {
  return confirmMutation(L('Service Publish 해제', 'Unpublish service'), service.protocol.toUpperCase() + ' ' + service.listen_address + ':' + service.listen_port,
    L('Service Publish 해제', 'Unpublish service'), function () { return fetchDelete(EP.VPC_SERVICE(service.id), { tenant: state.detail.tenant }); });
}

function summaryCard(label, value, note, tone) {
  var el = PCV.uxlib.el;
  return el('div', { class: 'hc vpc-summary ' + (tone || '') },
    el('div', { class: 'vpc-summary-label' }, label),
    el('div', { class: 'vpc-summary-value' }, String(value)),
    el('div', { class: 'vpc-summary-note' }, note || ''));
}
function actionsHead(title, action) {
  var el = PCV.uxlib.el;
  return el('div', { class: 'vpc-section-head' }, el('div', null, el('h3', null, title)), action || null);
}
function table(headers, rows, emptyText, cls) {
  var el = PCV.uxlib.el;
  if (!rows.length) return el('div', { class: 'vpc-inline-empty' }, emptyText);
  return el('div', { class: 'vpc-table-wrap' },
    el('table', { class: 'table-sticky card-mobile vpc-table ' + (cls || '') },
      el('thead', null, el('tr', null, headers.map(function (h) { return el('th', null, h); }))),
      el('tbody', null, rows)));
}
function cell(label, child) { return PCV.uxlib.el('td', { 'data-label': label }, child); }

function renderVpcTable() {
  var el = PCV.uxlib.el;
  return el('section', { class: 'hc vpc-list-card', 'aria-labelledby': 'vpc-list-title' },
    actionsHead(L('VPC 인벤토리', 'VPC inventory')),
    table([L('이름', 'Name'), 'Tenant', 'Backend', 'Egress', L('상태', 'State'), 'Revision', 'ID', L('작업', 'Actions')],
      state.vpcs.map(function (vpc) {
        var selected = vpc.id === state.selectedId;
        return el('tr', { class: selected ? 'vpc-row-selected' : '', 'aria-current': selected ? 'true' : null },
          cell(L('이름', 'Name'), button(vpc.name, 'vpc-select', function () { _select(vpc.id); }, null, { 'aria-pressed': selected ? 'true' : 'false' })),
          cell('Tenant', vpc.tenant), cell('Backend', mono(String(vpc.backend || 'linux').toUpperCase())),
          cell('Egress', mono(String(vpc.egress_mode || '').toUpperCase())),
          cell(L('상태', 'State'), pill(vpc.state)), cell('Revision', mono(vpc.revision)),
          cell('ID', mono(shortId(vpc.id), vpc.id)),
          cell(L('작업', 'Actions'), el('div', { class: 'vpc-row-actions' },
            button(L('Egress', 'Egress'), 'btn-soft btn-xs', function () { state.selectedId = vpc.id; _select(vpc.id, showEgress); }, 'OPERATOR,ADMIN'),
            button(L('삭제', 'Delete'), 'btn-r btn-xs', function () { deleteVpc(vpc); }, 'ADMIN'))));
      }), L('아직 생성된 VPC가 없습니다.', 'No VPC has been created yet.')));
}

function renderDetail() {
  var el = PCV.uxlib.el;
  if (!state.selectedId) return el('section', { class: 'hc empty-state vpc-detail-empty' },
    el('div', { class: 'empty-icon', 'aria-hidden': 'true' }, '◎'),
    el('div', { class: 'empty-title' }, L('VPC를 선택하세요', 'Select a VPC')),
    el('div', { class: 'empty-desc' }, L('목록에서 VPC를 선택하면 subnet, VM 연결과 게시 서비스를 확인할 수 있습니다.', 'Select a VPC to inspect subnets, VM attachments, and published services.')));
  if (state.loadingDetail) return el('section', { class: 'hc vpc-detail-loading', 'aria-busy': 'true' }, showSkeleton(null, 3));
  if (!state.detail) return el('section', { class: 'hc empty-state vpc-detail-empty' },
    el('div', { class: 'empty-icon', 'aria-hidden': 'true' }, '!'),
    el('div', { class: 'empty-title' }, L('VPC 상세를 불러오지 못했습니다', 'VPC detail is unavailable')),
    el('div', { class: 'empty-desc' }, L('위 오류를 확인한 뒤 다시 시도하세요.', 'Review the error above, then retry.')));
  var d = state.detail, subnets = d.subnets || [], attachments = d.attachments || [], services = d.service_publishes || [];
  return el('section', { class: 'vpc-detail', 'aria-labelledby': 'vpc-detail-title' },
    el('div', { class: 'hc vpc-detail-summary' },
      actionsHead(d.name + ' · ' + d.tenant, el('div', { class: 'vpc-detail-actions' },
        button(L('Egress 변경', 'Change egress'), 'btn-soft', showEgress, 'OPERATOR,ADMIN'),
        button(L('새로고침', 'Refresh'), 'btn-soft', function () { _select(d.id); }))),
      el('div', { class: 'vpc-detail-facts' },
        el('div', null, el('span', null, 'ID'), mono(d.id)),
        el('div', null, el('span', null, 'Backend'), mono(String(d.backend || 'linux').toUpperCase())),
        el('div', null, el('span', null, 'Egress'), mono(String(d.egress_mode || '').toUpperCase())),
        el('div', null, el('span', null, 'Revision'), mono(d.revision)),
        el('div', null, el('span', null, L('상태', 'State')), pill(d.state)))),
    el('div', { class: 'hc vpc-child-section' },
      actionsHead(L('Subnets', 'Subnets'), button('+ ' + L('Subnet', 'Subnet'), 'btn-primary', showSubnetCreate, 'OPERATOR,ADMIN')),
      table([L('이름', 'Name'), 'CIDR', 'Gateway', 'MTU', L('네트워크 참조', 'Network ref'), L('상태', 'State'), L('작업', 'Actions')], subnets.map(function (s) {
        return el('tr', null, cell(L('이름', 'Name'), s.name), cell('CIDR', mono(s.cidr)), cell('Gateway', mono(s.gateway)), cell('MTU', mono(s.mtu)), cell(L('네트워크 참조', 'Network ref'), mono(s.backend_ref || s.bridge_name)), cell(L('상태', 'State'), pill(s.state)), cell(L('작업', 'Actions'), button(L('삭제', 'Delete'), 'btn-r btn-xs', function () { deleteSubnet(s); }, 'OPERATOR,ADMIN')));
      }), L('Subnet이 없습니다. VM 연결 전에 subnet을 생성하세요.', 'No subnet. Create one before attaching a VM.'))),
    el('div', { class: 'hc vpc-child-section' },
      actionsHead(L('VM 연결', 'VM attachments'), button('+ ' + L('정지 VM 연결', 'Attach stopped VM'), 'btn-primary', showAttachmentCreate, 'OPERATOR,ADMIN')),
      table(['VM', 'Subnet', 'IP', 'MAC', L('상태', 'State'), L('작업', 'Actions')], attachments.map(function (a) {
        return el('tr', null, cell('VM', a.vm_name), cell('Subnet', mono(shortId(a.subnet_id), a.subnet_id)), cell('IP', mono(a.ip_address)), cell('MAC', mono(a.mac_address)), cell(L('상태', 'State'), pill(a.state)), cell(L('작업', 'Actions'), el('div', { class: 'vpc-row-actions' },
          button(L('서비스 게시', 'Publish'), 'btn-soft btn-xs', function () { showPublish(a); }, 'OPERATOR,ADMIN'),
          button(L('연결 해제', 'Detach'), 'btn-r btn-xs', function () { deleteAttachment(a); }, 'OPERATOR,ADMIN'))));
      }), L('연결된 VM이 없습니다.', 'No VM attachment.'))),
    el('div', { class: 'hc vpc-child-section' },
      actionsHead('Service Publish'),
      table([L('수신', 'Listen'), L('대상', 'Target'), L('허용 Source', 'Allowed sources'), L('상태', 'State'), L('작업', 'Actions')], services.map(function (s) {
        var sources = s.allowed_sources_json || '[]';
        try { sources = JSON.parse(sources).join(', '); } catch (_) {}
        return el('tr', null, cell(L('수신', 'Listen'), mono(String(s.protocol || '').toUpperCase() + ' ' + s.listen_address + ':' + s.listen_port)), cell(L('대상', 'Target'), mono(s.target_ip + ':' + s.target_port)), cell(L('허용 Source', 'Allowed sources'), mono(sources)), cell(L('상태', 'State'), pill(s.state)), cell(L('작업', 'Actions'), button(L('게시 해제', 'Unpublish'), 'btn-r btn-xs', function () { deleteService(s); }, 'OPERATOR,ADMIN')));
      }), L('게시된 서비스가 없습니다.', 'No published service.'))));
}

function _render() {
  if (!state.root || !state.root.isConnected) return;
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  var s = state.status || {};
  clearEl(state.root);
  state.root.appendChild(frag(
    HN.pagehead({
      title: 'Local VPC',
      desc: L('tenant별 격리 네트워크, 정지 VM 연결과 제한형 외부 게시를 운영합니다.', 'Operate tenant-isolated networks, stopped-VM attachments, and restricted service publishing.'),
      actions: [
        button(L('전체 Reconcile', 'Reconcile all'), 'btn-soft', function () {
          confirmMutation(L('전체 VPC Reconcile', 'Reconcile all VPCs'), L('모든 VPC desired state를 host에 다시 수렴합니다.', 'Re-apply all VPC desired state to the host.'), L('전체 Reconcile', 'Reconcile all'), function () { return fetchPost(EP.VPC_RECONCILE(), {}); });
        }, 'ADMIN'),
        button('+ ' + L('VPC 생성', 'Create VPC'), 'btn-primary', showCreateVpc, 'OPERATOR,ADMIN')
      ]
    }),
    el('div', { id: 'vpc-live-status', class: 'vpc-live-status', role: 'status', 'aria-live': 'polite' }, state.busy ? state.busy + ' · ' + L('작업 확인 중', 'checking job completion') : ''),
    state.error ? el('div', { class: 'vpc-error', role: 'alert' },
      el('span', null, state.error), button(L('다시 시도', 'Retry'), 'btn-soft', function () { _load(state.selectedId); })) : null,
    el('div', { class: 'sg grid-4 vpc-summary-grid' },
      summaryCard(L('컨트롤러 상태', 'Controller health'), s.healthy ? L('정상', 'Healthy') : L('확인 필요', 'Needs attention'), s.reconcile_required ? L('reconcile 필요', 'reconcile required') : L('desired/actual 일치', 'desired/actual aligned'), s.healthy ? 'vpc-summary-ok' : 'vpc-summary-warn'),
      summaryCard('VPC', s.vpc_count || 0, L('tenant aggregate', 'tenant aggregates')),
      summaryCard('Subnet / VM', (s.subnet_count || 0) + ' / ' + (s.attachment_count || 0), L('subnet · attachment', 'subnets · attachments')),
      summaryCard('Service Publish', s.service_publish_count || 0, L('제한형 inbound', 'restricted inbound'))),
    state.vpcs.length === 0 ? el('section', { class: 'hc empty-state vpc-empty' },
      el('div', { class: 'empty-icon', 'aria-hidden': 'true' }, '◎'),
      el('div', { class: 'empty-title' }, L('Local VPC가 없습니다', 'No Local VPC yet')),
      el('div', { class: 'empty-desc' }, L('tenant와 egress를 정해 첫 격리 네트워크를 생성하세요.', 'Choose a tenant and egress mode to create the first isolated network.')),
      button('+ ' + L('VPC 생성', 'Create VPC'), 'btn-primary', showCreateVpc, 'OPERATOR,ADMIN')) : renderVpcTable(),
    state.vpcs.length ? renderDetail() : null));
  applyRoles();
}

async function _select(id, after) {
  state.selectedId = id;
  state.detail = null;
  state.loadingDetail = true;
  _render();
  try {
    var response = await fetchGet(EP.VPC_DETAIL(id));
    if (response && response.error)
      throw new Error(response.error.message || response.error.code || L('VPC 상세 조회 실패', 'VPC detail failed'));
    state.detail = dataOrThrow(response);
    state.error = null;
  } catch (error) {
    state.error = error.message || String(error);
  } finally {
    state.loadingDetail = false;
    _render();
    if (after && state.detail) after();
  }
}

async function _load(preferredId, quiet) {
  state.error = null;
  try {
    var responses = await Promise.all([fetchGet(EP.VPC_LIST()), fetchGet(EP.VPC_STATUS())]);
    state.vpcs = listOrThrow(responses[0]);
    state.status = dataOrThrow(responses[1]) || {};
    try {
      var backendResponse = await fetchGet(EP.VPC_BACKENDS());
      if (backendResponse && backendResponse.error)
        throw new Error(backendResponse.error.message || backendResponse.error.code || L('Backend 상태 조회 실패', 'Backend status failed'));
      state.backends = listOrThrow(backendResponse);
      state.backendError = null;
    } catch (backendError) {
      state.backendError = backendError.message || String(backendError);
      state.backends = [{ id: 'linux', label: 'Linux bridge', ready: true, current_vpcs: state.vpcs.length, allocatable_vpcs: null }];
    }
    var keep = preferredId && state.vpcs.some(function (v) { return v.id === preferredId; });
    state.selectedId = keep ? preferredId : (state.vpcs[0] && state.vpcs[0].id || null);
    state.detail = null;
    _render();
    if (state.selectedId) await _select(state.selectedId);
  } catch (error) {
    state.error = error.message || String(error);
    if (!quiet) state.vpcs = [];
    _render();
  }
}

async function renderVpcs(root) {
  state.root = root;
  state.vpcs = [];
  state.backends = [];
  state.backendError = null;
  state.status = null;
  state.detail = null;
  state.error = null;
  if (root) showSkeleton(root, 4);
  await _load(state.selectedId);
}

PCV.vpc = {
  render: renderVpcs,
  refresh: function () { return _load(state.selectedId); },
  state: state,
  _waitJob: _waitJob
};
window.renderVpcs = renderVpcs;
})(window.PCV);
