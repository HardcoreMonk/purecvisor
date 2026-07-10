/* ═══════════════════════════════════════════════════════════════
   PureCVisor — modules/help.js
   Help, REST Guide, Service Guide, Swagger API, Keyboard Help
   한국어/영어 동시 지원 (I18N.getLang() 기반)
   ═══════════════════════════════════════════════════════════════ */
/*
 * Help is a documentation surface inside the app shell, but it must still obey
 * runtime contracts: all labels pass through _L(), endpoint counts come from
 * PCV.config when available, and generated tables stay searchable without
 * rebinding listeners after every render.
 *
 * The module intentionally keeps buildHelpData() local to renderHelp() until it
 * exports the function for integration checks. That lets the Single Edge filter
 * verify visible help content without coupling to the rest of the navigation
 * lifecycle.
 */

/* ADR-0013 IIFE 전환 후 _L 공유를 위해 IIFE 바깥에 선언
   (13개 모듈이 free identifier로 _L 호출 — 전역 스코프 필수) */
var _L = window._L = function(ko, en) {
  return (typeof I18N !== 'undefined' && I18N.getLang() === 'en') ? en : ko;
};

window.PCV = window.PCV || {};
(function(PCV) {

/* ═══ HELP & REFERENCE ═══ */
function renderHelp(b) {
  var h = H.sectionLg(_L('도움말 & 참조', 'Help & Reference'));
  h += '<div style="margin-bottom:20px;padding:16px 20px;background:linear-gradient(135deg,rgba(0,240,255,0.08),rgba(0,255,136,0.05));border:1px solid var(--accent);border-radius:8px;display:flex;align-items:center;gap:16px;flex-wrap:wrap">'
    + '<div style="flex:1;min-width:200px"><div style="font-size:15px;font-weight:600;color:var(--accent);margin-bottom:4px">' + _L('PureCVisor 완벽 가이드', 'PureCVisor Complete Guide') + '</div>'
    + '<div style="font-size:12px;color:var(--fg2)">' + _L('18개 챕터, 설치부터 트러블슈팅까지 전체 문서를 ReadTheDocs 스타일로 탐색하세요.', 'Browse all 18 chapters from installation to troubleshooting in a ReadTheDocs-style viewer.') + '</div></div>'
    + '<a href="/ui/guide.html" target="_blank" style="display:inline-flex;align-items:center;gap:6px;padding:8px 20px;background:var(--accent);color:#000;border-radius:6px;font-size:13px;font-weight:600;text-decoration:none;white-space:nowrap">&#128214; ' + _L('가이드 열기', 'Open Guide') + '</a></div>';
  h += '<div class="mb-16"><input aria-label="' + t('search') + '" id="help-search" class="sb-search" placeholder="' + t('search') + '" oninput="filterHelp()" style="max-width:600px;font-size:15px;padding:10px 14px;border-radius:8px"></div>';
  h += '<div id="help-content">';
  var helpData = buildHelpData();
  function buildHelpData() {
    var data = [
    { cat: _L('VM 관리', 'VM Management'), items: [
      { cmd: 'vm.list / vm.create / vm.delete', cli: 'pcvctl vm list/create/delete', tui: 'F1', web: _L('VM 라이브러리', 'VM Library'), desc: _L('VM 라이프사이클 — 생성, 시작, 중지, 삭제, 복제, 내보내기 (operator는 소유 VM 한정)', 'VM lifecycle — create, start, stop, delete, clone, export (operators are limited to owned VMs)') },
      { cmd: 'vm.start / vm.stop / vm.pause / vm.resume', cli: 'pcvctl vm start/stop', tui: 'F1: s/x/p', web: _L('전원 버튼 + 컨텍스트 메뉴', 'Power buttons + Context menu'), desc: _L('전원 제어 (실시간 프로그레스 모달)', 'Power control with real-time progress modal') },
      { cmd: 'vm.snapshot.create/list/rollback/delete/delete_all', cli: 'pcvctl vm snapshot ...', tui: 'F1: S', web: _L('스냅샷 탭', 'Snapshots tab'), desc: _L('ZFS 스냅샷 CRUD + 일괄 삭제 + 니어라이브', 'ZFS snapshot CRUD + bulk delete + near-live') },
      { cmd: 'vm.set_vcpu / vm.set_memory / vm.resize_disk', cli: 'pcvctl vm set-vcpu/set-mem', tui: '-', web: _L('설정 모달', 'Settings modal'), desc: _L('핫 리소스 조정', 'Hot resource adjustment') },
      { cmd: 'device.nic.list/attach/detach', cli: 'pcvctl nic list/add/remove', tui: 'F1: N/+/-', web: _L('NIC 관리자', 'NIC Manager'), desc: _L('NIC 핫플러그', 'NIC hotplug') },
      { cmd: 'vm.mount_iso / vm.eject', cli: 'pcvctl iso mount/eject', tui: '-', web: _L('설정 > CD', 'Settings > CD'), desc: _L('ISO 마운트/꺼내기', 'ISO mount/eject') },
      { cmd: 'vm.create (nic_type/pci_addr)', cli: 'pcvctl vm create --nic-type sriov', tui: '-', web: _L('VM 생성 모달', 'VM Create modal'), desc: _L('NIC 타입 선택 (bridge/dpdk/sriov) + PCI 주소', 'NIC type selection (bridge/dpdk/sriov) + PCI address') },
    ]},
    { cat: _L('컨테이너 (LXC)', 'Containers (LXC)'), items: [
      { cmd: 'container.list/create/start/stop/destroy', cli: 'pcvctl container ...', tui: 'F4', web: _L('컨테이너 라이브러리', 'Container Library'), desc: _L('LXC 라이프사이클 (프로그레스 모달)', 'LXC lifecycle with progress modal') },
      { cmd: 'container.exec', cli: 'pcvctl container exec', tui: 'F4: E', web: _L('콘솔 탭', 'Console tab'), desc: _L('컨테이너 명령 실행', 'Execute command in container') },
      { cmd: 'container.snapshot.create/rollback/delete', cli: 'pcvctl container snap ...', tui: 'F4: 3', web: _L('스냅샷 탭', 'Snapshots tab'), desc: _L('컨테이너 ZFS 스냅샷', 'Container ZFS snapshots') },
      { cmd: 'container.nic.list/attach/detach', cli: 'pcvctl container nic ...', tui: 'F4: N', web: _L('네트워크 탭', 'Network tab'), desc: _L('컨테이너 NIC 관리', 'Container NIC management') },
      { cmd: 'container.set_limits / set_bandwidth', cli: 'pcvctl container limits', tui: 'F4: L', web: _L('리소스 탭', 'Resources tab'), desc: _L('CPU/메모리 제한, 대역폭 QoS', 'CPU/memory limits, bandwidth QoS') },
      { cmd: 'container.clone', cli: 'pcvctl container clone', tui: '-', web: _L('컨테이너 탭', 'Container tab'), desc: _L('LXC 컨테이너 클론 (lxc-copy, ZFS 기반)', 'LXC container clone (lxc-copy, ZFS-backed)') },
      { cmd: 'container.checkpoint/restore', cli: 'pcvctl container checkpoint/restore', tui: '-', web: '-', desc: _L('CRIU 체크포인트/복원', 'CRIU checkpoint/restore') },
    ]},
    { cat: _L('네트워크', 'Network'), items: [
      { cmd: 'network.create/delete/list/info', cli: 'pcvctl network ...', tui: 'F2', web: _L('네트워크 페이지', 'Networks page'), desc: _L('NAT/격리/라우팅/브릿지 네트워크 CRUD', 'NAT/Isolated/Routed/Bridge network CRUD') },
      { cmd: 'ovn.status/switch.*/router.*/acl.*/nat.*', cli: 'pcvctl ovn ...', tui: 'F7', web: 'OVN SDN', desc: _L('OVN 논리 스위치/라우터, ACL, NAT', 'OVN logical switches, routers, ACL, NAT') },
      { cmd: 'overlay.list', cli: 'pcvctl overlay list', tui: 'F6: o', web: _L('오버레이 네트워크', 'Overlay Networks'), desc: _L('VXLAN 오버레이 메시', 'VXLAN overlay mesh') },
      { cmd: 'security_group.*', cli: 'pcvctl sg ...', tui: 'F7: G', web: _L('보안 그룹', 'Security Groups'), desc: _L('NFV 보안 그룹 규칙', 'NFV security group rules') },
    ]},
    { cat: _L('스토리지', 'Storage'), items: [
      { cmd: 'storage.pool.list/create/destroy/scrub', cli: 'pcvctl storage pool ...', tui: 'F3', web: _L('스토리지 페이지', 'Storage page'), desc: _L('ZFS 풀 관리', 'ZFS pool management') },
      { cmd: 'storage.zvol.list/create/delete', cli: 'pcvctl storage zvol ...', tui: 'F3: z/Z', web: _L('스토리지 페이지', 'Storage page'), desc: _L('ZFS zvol 관리', 'ZFS zvol management') },
      { cmd: 'iscsi.target.list', cli: 'pcvctl iscsi list', tui: 'F3: I', web: _L('iSCSI 타겟', 'iSCSI Targets'), desc: _L('iSCSI 타겟 관리', 'iSCSI target management') },
      { cmd: 'storage.pool.health', cli: 'pcvctl storage pool health', tui: 'F3: h', web: _L('스토리지 페이지', 'Storage page'), desc: _L('ZFS 풀 상태/scrub/용량 모니터링', 'ZFS pool health/scrub/capacity monitoring') },
      { cmd: 'zfs.promote', cli: 'pcvctl storage promote', tui: '-', web: '-', desc: _L('ZFS 클론 독립화 (promote)', 'ZFS clone promote to independent dataset') },
    ]},
    ];
    return data;
  }
  window.buildHelpData = buildHelpData;
  var helpDataDetail = [
    { cat: _L('모니터링', 'Monitoring'), items: [
      { cmd: 'telemetry.host / monitor.fleet', cli: 'pcvctl monitor ...', tui: 'F5', web: _L('모니터링 6탭', 'Monitoring 6 tabs'), desc: _L('CPU/메모리/디스크/네트워크 실시간 메트릭', 'CPU/Mem/Disk/Net real-time metrics') },
      { cmd: 'alert.config.get/set / alert.history', cli: 'pcvctl alert ...', tui: 'F5: a', web: _L('알림 페이지', 'Alerts page'), desc: _L('알림 엔진 설정 + 이력', 'Alert engine config + history') },
      { cmd: 'audit.search', cli: 'pcvctl audit ...', tui: 'F5: d', web: _L('감사 로그', 'Audit Log'), desc: _L('감사 로그 검색', 'Audit log search') },
      { cmd: 'alert.acknowledge / alert.sla', cli: 'pcvctl alert ack/sla', tui: '-', web: _L('알림 페이지', 'Alerts page'), desc: _L('알림 확인(ACK) + VM SLA 추적', 'Alert acknowledge + VM SLA tracking') },
      { cmd: 'ai.healing.pending/approve/reject', cli: 'pcvctl agent approve/reject', tui: '-', web: _L('모니터링 Overview', 'Monitoring Overview'), desc: _L('자가치유 대기 액션 승인/거절', 'Self-healing pending action approve/reject') },
    ]},
    { cat: _L('클라우드 마이그레이션', 'Cloud Migration'), items: [
      { cmd: 'vm.import.ec2 / vm.export.ec2', cli: 'pcvctl cloud import/export', tui: 'F6: w', web: _L('클라우드 마이그레이션', 'Cloud Migration'), desc: _L('AWS EC2 ↔ PureCVisor VM 이전', 'AWS EC2 ↔ PureCVisor VM migration') },
      { cmd: 'vm.import.ec2 (mode=near-live)', cli: 'pcvctl cloud import --mode near-live', tui: '-', web: _L('Import 모드 선택', 'Import Mode selector'), desc: _L('니어라이브 2단계 (Phase 1 + Finalize)', 'Near-Live 2-Phase (Phase 1 + Finalize)') },
      { cmd: 'cloud.jobs.list / cloud.job.cancel', cli: 'pcvctl cloud jobs/cancel', tui: 'F6: w', web: _L('클라우드 마이그레이션', 'Cloud Migration'), desc: _L('마이그레이션 작업 관리', 'Migration job management') },
    ]},
    { cat: _L('고급 기능', 'Advanced'), items: [
      { cmd: 'dpdk.status/bind/unbind/list', cli: 'pcvctl dpdk ...', tui: '-', web: 'DPDK', desc: _L('OVS-DPDK NIC 바인딩', 'OVS-DPDK NIC binding') },
      { cmd: 'sriov.status/enable/disable/attach/detach', cli: 'pcvctl sriov ...', tui: '-', web: 'SR-IOV', desc: _L('SR-IOV VF 관리', 'SR-IOV VF management') },
      { cmd: 'gpu.list / gpu.metrics', cli: 'pcvctl gpu ...', tui: 'F6: g', web: 'GPU', desc: _L('GPU 인벤토리 + 메트릭', 'GPU inventory + metrics') },
      { cmd: 'template.list/create/get/delete', cli: 'pcvctl template ...', tui: 'F6: t', web: _L('템플릿', 'Templates'), desc: _L('VM 템플릿 관리', 'VM template management') },
      { cmd: 'backup.policy.list/set/delete', cli: 'pcvctl backup ...', tui: 'F3: b', web: _L('백업', 'Backup'), desc: _L('백업 정책 + 이력', 'Backup policy + history') },
      { cmd: 'agent.config.get/set / agent.history', cli: 'pcvctl agent ...', tui: '-', web: _L('AI 에이전트', 'AI Agent'), desc: _L('AI 에이전트 멀티 프로바이더 합의', 'AI Agent multi-provider consensus') },
    ]},
    { cat: _L('백업 & 복원', 'Backup & Restore'), items: [
      { cmd: 'backup.list/set/remove', cli: 'pcvctl backup list/set', tui: 'F3: b', web: _L('백업 페이지', 'Backup page'), desc: _L('백업 정책 CRUD (주기/보존/활성화)', 'Backup policy CRUD (interval/retention/enable)') },
      { cmd: 'backup.history', cli: 'pcvctl backup history', tui: '-', web: _L('백업 페이지', 'Backup page'), desc: _L('스냅샷 히스토리 조회', 'Snapshot history query') },
      { cmd: 'backup.restore', cli: 'pcvctl backup restore', tui: '-', web: _L('백업 페이지', 'Backup page'), desc: _L('ZFS 스냅샷 롤백 복원', 'ZFS snapshot rollback restore') },
      { cmd: 'backup.replicate', cli: 'pcvctl backup replicate', tui: '-', web: '-', desc: _L('ZFS 원격 복제 (SSH, 원격 보존 정책)', 'ZFS remote replication (SSH, remote retention)') },
    ]},
  ];
  helpData = helpData.concat(helpDataDetail);
  helpData.forEach(function(cat) {
    h += '<div class="hc mb-14"><h4 class="color-accent">' + cat.cat + '</h4><table style="font-size:12px"><thead><tr><th>RPC</th><th>CLI</th><th>TUI</th><th>Web UI</th><th>' + _L('설명', 'Description') + '</th></tr></thead><tbody>';
    cat.items.forEach(function(i) { h += '<tr data-search="' + (i.cmd + ' ' + i.cli + ' ' + i.desc).toLowerCase() + '"><td style="color:var(--accent);font-family:var(--font-mono);font-size:11px">' + i.cmd + '</td><td style="font-size:11px">' + i.cli + '</td><td>' + i.tui + '</td><td>' + i.web + '</td><td class="color-muted">' + i.desc + '</td></tr>'; });
    h += '</tbody></table></div>';
  });
  h += '</div>';
  h += '<div class="sg grid-3">';
  h += H.card(_L('&#9881; 시스템', '&#9881; System'), H.row(_L('RPC 메서드', 'RPC Methods'), (typeof PCV !== 'undefined' ? PCV.config.RPC_COUNT : 264) + '+') + H.row(_L('REST 엔드포인트', 'REST Endpoints'), (typeof PCV !== 'undefined' ? PCV.config.REST_COUNT : 195) + '+') + H.row(_L('CLI 커맨드', 'CLI Commands'), '168'));
  h += H.card(_L('&#128200; 모니터링', '&#128200; Monitoring'), H.row('node_*', '126') + H.row('purecvisor_*', '44') + H.row(_L('Prometheus 합계', 'Total Prometheus'), '' + (typeof PCV !== 'undefined' ? PCV.config.METRICS_COUNT : 170)));
  h += H.card(_L('&#128187; 인프라', '&#128187; Infrastructure'), H.row(_L('3노드 클러스터', '3-Node Cluster'), 'HA Active') + H.row(_L('Web UI 모듈', 'Web UI Modules'), '20 (12K LOC)') + H.row(_L('i18n 키', 'i18n Keys'), '280+'));
  h += '</div>';
  b.innerHTML = h;
}
window.renderHelp = renderHelp;

function filterHelp() { var q = document.getElementById('help-search').value.toLowerCase(); document.querySelectorAll('#help-content tr[data-search]').forEach(function(r) { r.style.display = !q || r.dataset.search.includes(q) ? '' : 'none'; }); }
window.filterHelp = filterHelp;

/* ═══ REST API GUIDE ═══ */
function renderRestGuide(b) {
  var h = H.sectionLg(_L('REST API 가이드', 'REST API Guide'));
  h += '<div class="sg grid-2">';
  h += H.card(_L('&#128274; 인증', '&#128274; Authentication'), '<div style="font-size:13px;line-height:1.8">'
    + '<div style="border-left:3px solid var(--accent);padding-left:12px;margin-bottom:10px"><b>1. ' + _L('로그인', 'Login') + '</b><pre style="background:var(--bg);padding:8px;border-radius:4px;font-size:11px;color:var(--green);margin-top:4px">curl -X POST /api/v1/auth/token \\\n  -d \'{"username":"admin","password":"&lt;configured-admin-password&gt;"}\'</pre></div>'
    + '<div style="border-left:3px solid var(--green);padding-left:12px;margin-bottom:10px"><b>2. ' + _L('토큰 사용', 'Use Token') + '</b><pre style="background:var(--bg);padding:8px;border-radius:4px;font-size:11px;color:var(--green);margin-top:4px">curl -H "Authorization: Bearer eyJ..." \\\n  /api/v1/vms</pre></div>'
    + '<div style="border-left:3px solid var(--yellow);padding-left:12px"><b>3. ' + _L('쓰기 작업', 'Write Operations') + '</b><pre style="background:var(--bg);padding:8px;border-radius:4px;font-size:11px;color:var(--green);margin-top:4px">curl -X POST \\\n  -H "Authorization: Bearer eyJ..." \\\n  /api/v1/vms/web-prod/start</pre></div></div>');
  h += H.card(_L('&#128100; RBAC 역할', '&#128100; RBAC Roles'),
    '<table style="font-size:13px;width:100%"><thead><tr><th>' + _L('역할', 'Role') + '</th><th>' + _L('레벨', 'Level') + '</th><th>' + _L('권한', 'Permissions') + '</th></tr></thead><tbody>'
    + '<tr><td>' + H.badge('VIEWER', 'g') + '</td><td>0</td><td>' + _L('읽기 전용 (GET)', 'Read-only (GET endpoints)') + '</td></tr>'
    + '<tr><td>' + H.badge('OPERATOR', 'y') + '</td><td>1</td><td>' + _L('VM/컨테이너 운영, VM action은 생성자 범위', 'VM/Container operations, VM actions scoped to creator') + '</td></tr>'
    + '<tr><td>' + H.badge('ADMIN', 'r') + '</td><td>2</td><td>' + _L('전체 관리자', 'Full access') + '</td></tr></tbody></table>'
    + '<div style="margin-top:10px;font-size:12px;color:var(--fg2)">' + _L('내장 기본 비밀번호 없음. 첫 로그인 전 daemon.conf 또는 PURECVISOR_ADMIN_PASSWORD로 bootstrap 비밀번호를 설정합니다.', 'No built-in default password. Set the bootstrap password in daemon.conf or PURECVISOR_ADMIN_PASSWORD before first login.') + '</div>'
    + '<div style="margin-top:6px;font-size:12px;color:var(--fg2)">' + _L('OPERATOR는 libvirt domain metadata의 owner가 본인인 VM에만 시작, 중지, 삭제, 스냅샷, VNC, 일괄 작업을 수행할 수 있습니다.', 'OPERATOR can start, stop, delete, snapshot, access VNC, and batch-operate only VMs whose libvirt domain metadata owner matches the caller.') + '</div>');
  h += '</div>';

  h += '<div class="sg grid-3">';
  h += H.card(_L('&#128737; 보안', '&#128737; Security'), H.row(_L('속도 제한', 'Rate Limit'), _L('600 IP / 1200 유저 / 60 인증', '600 IP / 1200 user / 60 auth')) + H.row(_L('JWT 알고리즘', 'JWT Algorithm'), 'HS256') + H.row(_L('JWT 만료', 'JWT Expiry'), '900s (15min) + refresh 7d') + H.row('CORS', _L('화이트리스트', 'Whitelist')) + H.row('ETag', _L('GET 응답 조건부 캐싱 (304)', 'GET conditional caching (304)')) + H.row(_L('JWT IP 바인딩', 'JWT IP Binding'), _L('선택적 클라이언트 IP 검증', 'Optional client IP verification')));
  h += H.card(_L('&#127760; 엔드포인트', '&#127760; Endpoints'), H.row(_L('기본 URL', 'Base URL'), '<code>/api/v1</code>') + H.row(_L('포트', 'Port'), '80 / 443') + H.row(_L('합계', 'Total'), (typeof PCV !== 'undefined' ? PCV.config.REST_COUNT : 195) + '+') + H.row('WebSocket', '<code>/ws/events</code>'));
  h += H.card(_L('&#128196; 응답 형식', '&#128196; Response Format'), H.row(_L('성공', 'Success'), '<code>{"data": ...}</code>') + H.row(_L('에러', 'Error'), '<code>{"error": {code, message}}</code>') + H.row('Content-Type', 'application/json') + H.row(_L('캐시', 'Cache'), _L('ETag + max-age=5 (GET) / no-store (POST)', 'ETag + max-age=5 (GET) / no-store (POST)')) + H.row(_L('페이지네이션', 'Pagination'), 'X-Total-Count + Link rel="next/prev"'));
  h += '</div>';

  h += H.section('curl ' + _L('예제', 'Examples'));
  var examples = [
    { title: _L('VM 목록', 'List VMs'), cmd: 'curl -s -H "Authorization: Bearer $TOKEN" \\\n  http://HOST/api/v1/vms | jq' },
    { title: _L('VM 생성', 'Create VM'), cmd: 'curl -X POST -H "Authorization: Bearer $TOKEN" \\\n  -H "Content-Type: application/json" \\\n  -d \'{"name":"web","vcpu":2,"memory_mb":2048,"disk_size_gb":20}\' \\\n  http://HOST/api/v1/vms' },
    { title: _L('스냅샷 생성', 'Snapshot Create'), cmd: 'curl -X POST -H "Authorization: Bearer $TOKEN" \\\n  -d \'{"snap_name":"backup-1"}\' \\\n  http://HOST/api/v1/vms/web/snapshot/create' },
    { title: _L('컨테이너 실행', 'Container Exec'), cmd: 'curl -X POST -H "Authorization: Bearer $TOKEN" \\\n  -d \'{"command":"hostname -I"}\' \\\n  http://HOST/api/v1/containers/app-ctr/exec' },
    { title: _L('클라우드 임포트 (니어라이브)', 'Cloud Import (Near-Live)'), cmd: 'curl -X POST -H "Authorization: Bearer $TOKEN" \\\n  -d \'{"name":"web","ami_id":"ami-0abc","mode":"near-live"}\' \\\n  http://HOST/api/v1/vms/web/import-ec2' },
    { title: _L('WebSocket 이벤트', 'WebSocket Events'), cmd: 'wscat -c "ws://HOST/api/v1/ws/events?token=$TOKEN"' },
  ];
  h += '<div class="sg grid-2">';
  examples.forEach(function(ex) {
    h += H.card(ex.title, '<pre style="background:var(--bg);padding:10px;border-radius:4px;font-size:11px;color:var(--green);overflow-x:auto;white-space:pre-wrap">' + esc(ex.cmd) + '</pre>');
  });
  h += '</div>';
  b.innerHTML = h;
}
window.renderRestGuide = renderRestGuide;

/* ═══ SERVICE GUIDE ═══ */
function renderServiceGuide(b) {
  var h = H.sectionLg(_L('PureCVisor 서비스 가이드', 'PureCVisor Service Guide'));
  h += '<div class="mb-16"><input aria-label="' + t('search') + '" id="guide-search" class="sb-search" placeholder="' + t('search') + '" oninput="filterGuide()" style="max-width:600px;font-size:15px;padding:10px 14px;border-radius:8px"></div>';
  h += '<div id="guide-content">';

  var services = [
    { title: _L('빠른 시작', 'Quick Start'), icon: '&#128640;', sections: [
      { sub: _L('5분 설정', '5-Minute Setup'), content:
        '<ol style="font-size:13px;line-height:2;padding-left:18px">'
        + '<li>' + _L('로그인', 'Login') + ': <code>http://NODE_IP/ui/</code> (admin / configured password)</li>'
        + '<li>' + _L('VM 생성: Ctrl+N → 이름, vCPU, 메모리, 디스크 → 생성', 'Create VM: Ctrl+N → Name, vCPU, Memory, Disk → Create') + '</li>'
        + '<li>' + _L('VM 시작: VM 선택 → 시작 버튼 또는 우클릭 → 시작', 'Start VM: Select VM → Start button or right-click → Start') + '</li>'
        + '<li>' + _L('VNC 콘솔: VM 선택 → 콘솔 탭 → noVNC', 'VNC Console: Select VM → Console tab → noVNC') + '</li>'
        + '<li>' + _L('모니터링: INFRA 사이드바 → 모니터링 Overview', 'Monitor: INFRA sidebar → Monitoring Overview') + '</li></ol>' },
      { sub: _L('CLI 빠른 시작', 'CLI Quick Start'), content:
        '<pre style="background:var(--bg);padding:12px;border-radius:6px;font-size:11px;color:var(--green);overflow-x:auto">'
        + '# ' + _L('로그인', 'Login') + '\n'
        + 'TOKEN=$(curl -s -X POST http://localhost/api/v1/auth/token \\\n'
        + '  -d \'{"username":"admin","password":"configured-admin-password"}\' | jq -r .access_token)\n\n'
        + '# VM\n'
        + 'pcvctl vm list\n'
        + 'pcvctl vm create web --vcpu 2 --memory_mb 2048 --disk_size_gb 20\n'
        + 'pcvctl vm start web\n'
        + 'pcvctl vm stop web\n\n'
        + '# ' + _L('컨테이너', 'Container') + '\n'
        + 'pcvctl container list\n'
        + 'pcvctl container exec app-ctr "hostname -I"\n\n'
        + '# ' + _L('모니터링', 'Monitoring') + '\n'
        + 'pcvctl monitor fleet\n'
        + 'pcvctl alert list</pre>' },
    ]},
    { title: _L('아키텍처', 'Architecture'), icon: '&#127959;', sections: [
      { sub: _L('시스템 개요', 'System Overview'), content:
        '<pre style="background:var(--bg);padding:12px;border-radius:6px;font-size:11px;color:var(--accent);overflow-x:auto">'
        + _L('클라이언트', 'Client') + ' (pcvctl / pcvtui / Web UI / REST API)\n'
        + '         |\n'
        + '   UDS ' + _L('서버', 'Server') + ' (JSON-RPC 2.0) | REST ' + _L('서버', 'Server') + ' (HTTP+JWT)\n'
        + '         |\n'
        + '   ' + _L('디스패처', 'Dispatcher') + ' (' + (typeof PCV !== 'undefined' ? PCV.config.RPC_COUNT : 264) + '+ RPC ' + _L('메서드', 'methods') + ')\n'
        + '     method policy / RBAC / VM owner-scope\n'
        + '         |\n'
        + '   ' + _L('핸들러 계층', 'Handler Layer') + ' (dispatcher/*.c)\n'
        + '         |\n'
        + '   ' + _L('코어 모듈', 'Core Modules') + ' (vm_manager, network, zfs, lxc)\n'
        + '         |\n'
        + '   ' + _L('시스템', 'System') + ' (libvirt, nftables, dnsmasq, ZFS, LXC)</pre>' },
      { sub: _L('기술 스택', 'Tech Stack'), content:
        '<table style="font-size:12px;width:100%"><tbody>'
        + '<tr><td class="color-muted">' + _L('언어', 'Language') + '</td><td>C23 (gnu23)</td></tr>'
        + '<tr><td class="color-muted">' + _L('이벤트 루프', 'Event Loop') + '</td><td>GMainLoop (GLib)</td></tr>'
        + '<tr><td class="color-muted">' + _L('하이퍼바이저', 'Hypervisor') + '</td><td>KVM/QEMU via libvirt</td></tr>'
        + '<tr><td class="color-muted">' + _L('컨테이너', 'Container') + '</td><td>LXC (liblxc)</td></tr>'
        + '<tr><td class="color-muted">' + _L('스토리지', 'Storage') + '</td><td>ZFS (zvol + snapshots)</td></tr>'
        + '<tr><td class="color-muted">' + _L('네트워크', 'Network') + '</td><td>nftables + OVS + OVN</td></tr>'
        + '<tr><td class="color-muted">REST</td><td>libsoup3 (HTTP/HTTPS)</td></tr>'
        + '<tr><td class="color-muted">' + _L('비동기 I/O', 'Async I/O') + '</td><td>io_uring</td></tr>'
        + '<tr><td class="color-muted">' + _L('인증', 'Auth') + '</td><td>JWT HS256 + RBAC + VM owner-scope</td></tr>'
        + '<tr><td class="color-muted">' + _L('모니터링', 'Monitoring') + '</td><td>' + _L('자체 node_exporter (' + (typeof PCV !== 'undefined' ? PCV.config.METRICS_COUNT : 170) + ' 메트릭)', 'Self node_exporter (' + (typeof PCV !== 'undefined' ? PCV.config.METRICS_COUNT : 170) + ' metrics)') + '</td></tr>'
        + '<tr><td class="color-muted">Web UI</td><td>Vanilla JS (Single Edge modules)</td></tr>'
        + '</tbody></table>' },
    ]},
    { title: _L('가상 머신', 'Virtual Machines'), icon: '&#128187;', sections: [
      { sub: _L('라이프사이클', 'Lifecycle'), content:
        H.row(_L('생성', 'Create'), _L('virt-install + XML 폴백, cloud-init 지원', 'virt-install + XML fallback, cloud-init support'))
        + H.row(_L('시작/중지', 'Start/Stop'), _L('virDomainCreate / virDomainShutdown (graceful 30초 → 강제)', 'virDomainCreate / virDomainShutdown (graceful 30s → force)'))
        + H.row(_L('일시정지/재개', 'Pause/Resume'), 'virDomainSuspend / virDomainResume')
        + H.row(_L('삭제', 'Delete'), _L('virDomainUndefine + ZFS zvol 삭제 (비동기)', 'virDomainUndefine + ZFS zvol destroy (async)'))
        + H.row(_L('복제', 'Clone'), _L('ZFS clone + 새 도메인 정의', 'ZFS clone + new domain define'))
        + H.row(_L('가져오기/내보내기', 'Import/Export'), _L('qcow2 기반 VM 이미지 가져오기와 내보내기', 'qcow2-based VM image import and export')) },
      { sub: _L('스냅샷', 'Snapshots'), content:
        H.row(_L('생성', 'Create'), _L('ZFS 스냅샷 (크래시 일관성, 실행/중지 상태)', 'ZFS snapshot (crash-consistent, live or stopped)'))
        + H.row(_L('롤백', 'Rollback'), _L('VM 중지 → zfs rollback -r → 재시작 (fire-and-forget)', 'VM stop → zfs rollback -r → restart (fire-and-forget)'))
        + H.row(_L('일괄 삭제', 'Bulk Delete'), _L('vm.snapshot.delete_all — prefix 필터 + keep_recent', 'vm.snapshot.delete_all — prefix filter + keep_recent'))
        + H.row('UI', _L('생성 모달 (검증+미리보기) + 롤백 (이름 타이핑 확인) + 일괄 삭제 (미리보기)', 'Create modal (validation+preview) + Rollback (name typing confirm) + Bulk delete (preview)')) },
      { sub: _L('핫플러그', 'Hotplug'), content:
        H.row('NIC', _L('device.nic.attach/detach — 브릿지 + 모델 (virtio)', 'device.nic.attach/detach — bridge + model (virtio)'))
        + H.row('ISO', _L('vm.mount_iso / vm.eject — 핫 마운트/꺼내기', 'vm.mount_iso / vm.eject — hot mount/eject'))
        + H.row('vCPU', _L('vm.set_vcpu — 라이브 조정', 'vm.set_vcpu — live adjust'))
        + H.row(_L('메모리', 'Memory'), _L('vm.set_memory — 라이브 조정 (balloon)', 'vm.set_memory — live adjust (balloon)'))
        + H.row(_L('디스크', 'Disk'), _L('vm.resize_disk — qemu-img resize + virDomainBlockResize', 'vm.resize_disk — qemu-img resize + virDomainBlockResize')) },
    ]},
    { title: _L('클라우드 마이그레이션', 'Cloud Migration'), icon: '&#9729;', sections: [
      { sub: _L('AWS EC2 임포트', 'AWS EC2 Import'), content:
        '<div style="font-size:13px;line-height:1.8">'
        + '<b>' + _L('표준 임포트 (6단계):', 'Standard Import (6 stages):') + '</b>'
        + '<ol style="padding-left:18px;margin:4px 0"><li>' + _L('AWS 자격증명 검증', 'AWS credential validation') + '</li><li>' + _L('AMI → S3 내보내기', 'AMI → S3 export') + '</li><li>' + _L('진행률 폴링', 'Progress polling') + '</li><li>' + _L('S3 다운로드', 'S3 download') + '</li><li>' + _L('RAW → qcow2 변환', 'RAW → qcow2 conversion') + '</li><li>' + _L('VM 정의 + 시작', 'VM define + start') + '</li></ol>'
        + '<b>' + _L('니어라이브 임포트 (2단계):', 'Near-Live Import (2 phases):') + '</b>'
        + '<ol style="padding-left:18px;margin:4px 0"><li><span class="color-green">' + _L('Phase 1', 'Phase 1') + '</span>: ' + _L('사전동기화 (기본 이미지 다운로드, 다운타임 0)', 'Pre-sync (base image download, no downtime)') + '</li>'
        + '<li><span class="color-yellow">' + _L('Phase 2', 'Phase 2') + '</span>: ' + _L('최종전환 (EC2 중지 → 델타 스냅샷 → 리베이스 → VM 시작, ~2-5분)', 'Finalize (EC2 stop → delta snapshot → rebase → VM start, ~2-5min)') + '</li></ol></div>' },
    ]},
    { title: _L('보안', 'Security'), icon: '&#128274;', sections: [
      { sub: _L('보안 강화', 'Hardening'), content:
        H.row('XSS', _L('escapeHtml() — 모든 사용자 입력', 'escapeHtml() — all user input'))
        + H.row('CORS', _L('화이트리스트 모드', 'Whitelist mode'))
        + H.row('RBAC', _L('디스패처 메서드 정책 + operator VM owner metadata 검사', 'Dispatcher method policy + operator VM owner metadata check'))
        + H.row(_L('속도 제한', 'Rate Limit'), _L('600 IP / 1200 유저 / 60 인증', '600 IP / 1200 user / 60 auth'))
        + H.row('SQL', _L('Prepared statements', 'Prepared statements'))
        + H.row(_L('경로 순회', 'Path Traversal'), _L('realpath() 검증', 'realpath() validation'))
        + H.row(_L('명령 주입', 'CMD Injection'), _L('pcv_spawn_sync() argv 배열 (쉘 없음)', 'pcv_spawn_sync() argv array (no shell)'))
        + H.row(_L('패스워드', 'Password'), 'PBKDF2 (HMAC-SHA256) + ' + _L('자동 마이그레이션', 'auto-migration'))
        + H.row('Seccomp', _L('시스콜 필터링', 'Syscall filtering'))
        + H.row('WebSocket', _L('JWT 인증 + 300초 유휴 타임아웃', 'JWT auth + 300s idle timeout'))
        + H.row(_L('보안 그룹', 'Security Groups'), _L('SQLite 영속화 + default-deny + 포트 범위', 'SQLite persistence + default-deny + port ranges'))
        + H.row(_L('시크릿', 'Secrets'), _L('PCV_SECRET_* 환경변수 우선 로드', 'PCV_SECRET_* env var priority')) },
    ]},
    { title: _L('설정', 'Configuration'), icon: '&#9881;', sections: [
      { sub: 'daemon.conf', content:
        '<pre style="background:var(--bg);padding:10px;border-radius:6px;font-size:11px;color:var(--green);overflow-x:auto">'
        + '[server]\nport = 80\ndrain_timeout = 30\n\n[tls]\nenabled = false\n\n[storage]\nzvol_pool = pcvpool/vms\nimage_dir = /var/lib/libvirt/images\niso_dirs = /pcvpool/iso,/var/lib/libvirt/images\n\n[alert]\nenabled = true\ncpu_warn = 80\ncpu_crit = 95\ndata_pool_warn = 80\ndata_pool_crit = 90\nwebhook_url = https://hooks.slack.com/...\nwebhook_format = slack\n\n[cpu]\nallow_overcommit = false</pre>' },
      { sub: _L('서비스 관리', 'Service Management'), content:
        '<pre style="background:var(--bg);padding:10px;border-radius:6px;font-size:11px;color:var(--green);overflow-x:auto">'
        + 'sudo systemctl start purecvisorsd  # Single Edge\nsudo systemctl status purecvisorsd\njournalctl -u purecvisorsd -f\n\n# ' + _L('수동 RPC 테스트', 'Manual RPC test') + '\n'
        + 'echo \'{"jsonrpc":"2.0","method":"vm.list","params":{},"id":"1"}\' \\\n  | nc -U /var/run/purecvisor/daemon.sock | jq</pre>' },
    ]},
    { title: _L('트러블슈팅', 'Troubleshooting'), icon: '&#128295;', sections: [
      { sub: _L('자주 발생하는 문제', 'Common Issues'), content:
        '<table style="font-size:12px;width:100%"><thead><tr><th>' + _L('증상', 'Symptom') + '</th><th>' + _L('원인', 'Cause') + '</th><th>' + _L('해결', 'Fix') + '</th></tr></thead><tbody>'
        + '<tr><td>' + _L('VM 상태 "unknown"', 'VM state "unknown"') + '</td><td>' + _L('libvirt 연결 끊김', 'libvirt connection lost') + '</td><td><code>systemctl restart purecvisorsd</code></td></tr>'
        + '<tr><td>/health ' + _L('느림 (30초)', 'slow (30s)') + '</td><td>' + _L('libvirt 프로브 타임아웃', 'libvirt probe timeout') + '</td><td><code>systemctl status libvirtd</code></td></tr>'
        + '<tr><td>' + _L('스냅샷 500 에러', 'Snapshot 500 error') + '</td><td>' + _L('스냅샷 과다 (>1000개)', 'Too many snapshots (>1000)') + '</td><td><code>pcvctl vm snapshot delete-all --keep 10</code></td></tr>'
        + '<tr><td>' + _L('컨테이너 IP 대기중', 'Container IP pending') + '</td><td>' + _L('DHCP 지연', 'DHCP delay') + '</td><td>' + _L('10-15초 대기 또는 브릿지 설정 확인', 'Wait 10-15s or check bridge config') + '</td></tr>'
        + '<tr><td>REST 403 Forbidden</td><td>' + _L('RBAC 역할 부족 또는 VM owner 불일치', 'RBAC role insufficient or VM owner mismatch') + '</td><td><code>pcvctl auth list</code></td></tr>'
        + '</tbody></table>' },
      { sub: _L('디버그 명령어', 'Debug Commands'), content:
        '<pre style="background:var(--bg);padding:10px;border-radius:6px;font-size:11px;color:var(--green);overflow-x:auto">'
        + '# ' + _L('데몬 상태', 'Daemon status') + '\n'
        + 'sudo systemctl status purecvisorsd\njournalctl -u purecvisorsd --since "5 min ago"\n\n'
        + '# libvirt\nsudo virsh list --all\njournalctl -u libvirtd -n 50\n\n'
        + '# ' + _L('네트워크', 'Network') + '\n'
        + 'ip link show type bridge && brctl show\nsudo nft list table inet purecvisor\nsudo ovs-vsctl show\n\n'
        + '# ZFS\nzpool status pcvpool\nzfs list -t snapshot -r pcvpool/vms</pre>' },
    ]},
  ];

  services.forEach(function(s) {
    var searchText = (s.title + ' ' + s.sections.map(function(x){return x.sub;}).join(' ')).toLowerCase().replace(/[^a-z0-9가-힣\s]/g, '');
    h += '<div class="hc mb-14" data-guide="' + searchText + '">';
    h += '<h4 style="font-size:16px;color:var(--accent);margin-bottom:12px;cursor:pointer" onclick="this.parentElement.classList.toggle(\'guide-collapsed\')">' + s.icon + ' ' + s.title + ' <span class="color-muted" style="font-size:11px">(' + s.sections.length + ' ' + _L('섹션', 'sections') + ')</span></h4>';
    s.sections.forEach(function(sec) {
      h += '<div style="margin-bottom:14px;padding-left:12px;border-left:2px solid var(--border)">';
      h += '<div style="font-weight:600;font-size:13px;margin-bottom:6px;color:var(--fg)">' + sec.sub + '</div>';
      h += '<div style="font-size:12px;color:var(--fg2)">' + sec.content + '</div>';
      h += '</div>';
    });
    h += '</div>';
  });
  h += '</div>';
  b.innerHTML = h;
}
window.renderServiceGuide = renderServiceGuide;

function filterGuide() { var q = document.getElementById('guide-search').value.toLowerCase(); document.querySelectorAll('#guide-content .hc[data-guide]').forEach(function(c) { c.style.display = !q || c.dataset.guide.includes(q) ? '' : 'none'; }); }
window.filterGuide = filterGuide;

/* ═══ SWAGGER API ═══ */
function renderSwaggerApi(b) {
  var mc = function(m) { return m === 'GET' ? '#61affe' : m === 'POST' ? '#49cc90' : m === 'DELETE' ? '#f93e3e' : m === 'PUT' ? '#fca130' : '#00f0ff'; };
  var endpoints = [
    { tag: 'Health & Auth (4)', endpoints: [
      { m: 'GET', p: '/health', d: _L('심층 헬스체크 (6개 서브시스템)', 'Deep health probe (6 subsystems)'), auth: false },
      { m: 'GET', p: '/metrics', d: _L('Prometheus 메트릭 (' + (typeof PCV !== 'undefined' ? PCV.config.METRICS_COUNT : 170) + '개)', 'Prometheus metrics (' + (typeof PCV !== 'undefined' ? PCV.config.METRICS_COUNT : 170) + ')'), auth: false },
      { m: 'POST', p: '/auth/token', d: _L('JWT 로그인', 'JWT login'), auth: false, body: '{"username":"admin","password":"configured-admin-password"}' },
      { m: 'GET', p: '/auth/users', d: _L('사용자 목록 (RBAC)', 'User list (RBAC)') },
    ]},
    { tag: 'VMs (22)', endpoints: [
      { m: 'GET', p: '/vms', d: _L('VM 목록', 'VM list') },
      { m: 'POST', p: '/vms', d: _L('VM 생성', 'Create VM'), body: '{"name":"web","vcpu":2,"memory_mb":2048,"disk_size_gb":20}' },
      { m: 'DELETE', p: '/vms/{name}', d: _L('VM 삭제', 'Delete VM') },
      { m: 'POST', p: '/vms/{name}/start', d: _L('VM 시작', 'Start VM') },
      { m: 'POST', p: '/vms/{name}/stop', d: _L('VM 중지', 'Stop VM') },
      { m: 'POST', p: '/vms/{name}/suspend', d: _L('VM 일시정지', 'Pause VM') },
      { m: 'POST', p: '/vms/{name}/resume', d: _L('VM 재개', 'Resume VM') },
      { m: 'GET', p: '/vms/{name}/snapshot', d: _L('스냅샷 목록', 'Snapshot list') },
      { m: 'POST', p: '/vms/{name}/snapshot/create', d: _L('스냅샷 생성', 'Create snapshot'), body: '{"snap_name":"backup-1"}' },
      { m: 'POST', p: '/vms/{name}/snapshot/rollback', d: _L('스냅샷 롤백', 'Rollback snapshot'), body: '{"snap_name":"backup-1"}' },
      { m: 'DELETE', p: '/vms/{name}/snapshot/{snap}', d: _L('스냅샷 삭제', 'Delete snapshot') },
      { m: 'POST', p: '/vms/{name}/snapshot/delete_all', d: _L('일괄 삭제', 'Bulk delete'), body: '{"prefix":"pcv-repl-","keep_recent":5}' },
      { m: 'GET', p: '/vms/{name}/nics', d: _L('NIC 목록', 'NIC list') },
      { m: 'POST', p: '/vms/{name}/nics', d: _L('NIC 추가', 'NIC attach'), body: '{"bridge":"pcvbr0","model":"virtio"}' },
      { m: 'DELETE', p: '/vms/{name}/nics/{mac}', d: _L('NIC 제거', 'NIC detach') },
      { m: 'PUT', p: '/vms/{name}/vcpu', d: _L('vCPU 설정', 'Set vCPU'), body: '{"vcpu_count":4}' },
      { m: 'PUT', p: '/vms/{name}/memory', d: _L('메모리 설정', 'Set memory'), body: '{"memory_mb":4096}' },
      { m: 'POST', p: '/vms/{name}/clone', d: _L('VM 복제', 'Clone VM'), body: '{"new_name":"web-clone"}' },
      { m: 'POST', p: '/vms/{name}/iso', d: _L('ISO 마운트', 'Mount ISO'), body: '{"iso_path":"/iso/ubuntu.iso"}' },
      { m: 'DELETE', p: '/vms/{name}/iso', d: _L('ISO 꺼내기', 'Eject ISO') },
      { m: 'GET', p: '/vms/{name}/delete-status', d: _L('삭제 진행률', 'Delete progress') },
      { m: 'PUT', p: '/vms/{name}/bandwidth', d: _L('대역폭 QoS', 'Bandwidth QoS'), body: '{"rate":"100mbit"}' },
    ]},
    { tag: _L('컨테이너 (17)', 'Containers (17)'), endpoints: [
      { m: 'GET', p: '/containers', d: _L('컨테이너 목록', 'Container list') },
      { m: 'POST', p: '/containers', d: _L('컨테이너 생성', 'Create container'), body: '{"name":"app","dist":"ubuntu","release":"24.04"}' },
      { m: 'DELETE', p: '/containers/{name}', d: _L('컨테이너 삭제', 'Destroy container') },
      { m: 'POST', p: '/containers/{name}/start', d: _L('시작', 'Start') },
      { m: 'POST', p: '/containers/{name}/stop', d: _L('중지', 'Stop') },
      { m: 'POST', p: '/containers/{name}/exec', d: _L('명령 실행', 'Exec command'), body: '{"command":"hostname"}' },
      { m: 'GET', p: '/containers/{name}/snapshots', d: _L('스냅샷 목록', 'Snapshot list') },
      { m: 'POST', p: '/containers/{name}/snapshots', d: _L('스냅샷 생성', 'Create snapshot'), body: '{"snap_name":"snap-1"}' },
      { m: 'GET', p: '/containers/{name}/nics', d: _L('NIC 목록', 'NIC list') },
      { m: 'PUT', p: '/containers/{name}/limits', d: _L('리소스 제한', 'Set limits'), body: '{"cpu_limit":"2","memory_limit":"512M"}' },
      { m: 'POST', p: '/rpc', d: _L('컨테이너 클론 (container.clone)', 'Container clone (container.clone)'), body: '{"jsonrpc":"2.0","method":"container.clone","params":{"name":"app","new_name":"app-clone"},"id":"1"}' },
      { m: 'POST', p: '/rpc', d: _L('컨테이너 체크포인트 (container.checkpoint)', 'Container checkpoint (container.checkpoint)'), body: '{"jsonrpc":"2.0","method":"container.checkpoint","params":{"name":"app"},"id":"1"}' },
      { m: 'POST', p: '/rpc', d: _L('컨테이너 복원 (container.restore)', 'Container restore (container.restore)'), body: '{"jsonrpc":"2.0","method":"container.restore","params":{"name":"app"},"id":"1"}' },
    ]},
    { tag: _L('네트워크 (5)', 'Networks (5)'), endpoints: [
      { m: 'GET', p: '/networks', d: _L('네트워크 목록', 'Network list') },
      { m: 'POST', p: '/networks', d: _L('네트워크 생성', 'Create network'), body: '{"name":"br1","mode":"nat","cidr":"10.10.0.1/24","dhcp":true}' },
      { m: 'DELETE', p: '/networks/{br}', d: _L('네트워크 삭제', 'Delete network') },
    ]},
    { tag: _L('스토리지 (7)', 'Storage (7)'), endpoints: [
      { m: 'GET', p: '/storage/pools', d: _L('ZFS 풀 목록', 'ZFS pools') },
      { m: 'POST', p: '/storage/pools', d: _L('풀 생성', 'Create pool') },
      { m: 'GET', p: '/storage/zvols', d: _L('Zvol 목록', 'Zvol list') },
      { m: 'POST', p: '/storage/zvols', d: _L('Zvol 생성', 'Create zvol'), body: '{"name":"data","size":"20G"}' },
    ]},
    { tag: _L('클라우드 마이그레이션 (5)', 'Cloud Migration (5)'), endpoints: [
      { m: 'POST', p: '/vms/{name}/import-ec2', d: _L('EC2에서 임포트', 'Import from EC2'), body: '{"ami_id":"ami-0abc","mode":"near-live"}' },
      { m: 'POST', p: '/vms/{name}/export-ec2', d: _L('EC2로 내보내기', 'Export to EC2'), body: '{"s3_bucket":"my-bucket"}' },
      { m: 'GET', p: '/vms/{name}/import-status', d: _L('임포트 진행률', 'Import progress') },
      { m: 'GET', p: '/cloud/jobs', d: _L('마이그레이션 작업 목록', 'Migration job list') },
      { m: 'POST', p: '/cloud/cancel', d: _L('마이그레이션 취소', 'Cancel migration'), body: '{"name":"web"}' },
    ]},
    { tag: _L('모니터링 (11)', 'Monitoring (11)'), endpoints: [
      { m: 'GET', p: '/processes', d: _L('프로세스 목록', 'Process list') },
      { m: 'GET', p: '/alerts', d: _L('알림 이력', 'Alert history') },
      { m: 'GET', p: '/alerts/config', d: _L('알림 설정', 'Alert config') },
      { m: 'PUT', p: '/alerts/config', d: _L('알림 설정 변경', 'Update alert config'), body: '{"cpu_warn":80,"cpu_crit":95}' },
      { m: 'GET', p: '/alerts/sla/{vm}', d: _L('VM SLA 추적', 'VM SLA tracking') },
      { m: 'POST', p: '/rpc', d: _L('알림 확인 (alert.acknowledge)', 'Alert acknowledge (alert.acknowledge)'), body: '{"jsonrpc":"2.0","method":"alert.acknowledge","params":{"alert_id":"..."},"id":"1"}' },
      { m: 'POST', p: '/rpc', d: _L('자가치유 대기/승인/거절 (ai.healing.*)', 'Self-healing pending/approve/reject (ai.healing.*)'), body: '{"jsonrpc":"2.0","method":"ai.healing.pending","params":{},"id":"1"}' },
      { m: 'GET', p: '/audit/search', d: _L('감사 로그 검색', 'Audit log search') },
      { m: 'GET', p: '/iso', d: _L('ISO 파일 목록', 'ISO file list') },
    ]},
    { tag: 'DPDK & SR-IOV (12)', endpoints: [
      { m: 'GET', p: '/dpdk/status', d: _L('DPDK 상태', 'DPDK status') },
      { m: 'POST', p: '/dpdk/bind', d: _L('NIC 바인드', 'Bind NIC'), body: '{"pci_addr":"0000:03:00.0","driver":"vfio-pci"}' },
      { m: 'POST', p: '/dpdk/unbind', d: _L('NIC 언바인드', 'Unbind NIC') },
      { m: 'GET', p: '/sriov/status', d: _L('SR-IOV 상태', 'SR-IOV status') },
      { m: 'POST', p: '/sriov/enable', d: _L('VF 활성화', 'Enable VFs'), body: '{"pf":"enp3s0f0","num_vfs":4}' },
      { m: 'POST', p: '/sriov/attach', d: _L('VF→VM 연결', 'Attach VF to VM'), body: '{"vm_name":"web","pci_addr":"0000:03:10.0"}' },
    ]},
    { tag: _L('백업 & 보안 그룹 (5)', 'Backup & Security Groups (5)'), endpoints: [
      { m: 'POST', p: '/rpc', d: _L('백업 정책 목록 (backup.list)', 'Backup policy list (backup.list)'), body: '{"jsonrpc":"2.0","method":"backup.list","params":{},"id":"1"}' },
      { m: 'POST', p: '/rpc', d: _L('백업 정책 설정 (backup.set)', 'Backup policy set (backup.set)'), body: '{"jsonrpc":"2.0","method":"backup.set","params":{"vm_name":"web","interval_hours":24,"retention":7},"id":"1"}' },
      { m: 'POST', p: '/rpc', d: _L('백업 복원 (backup.restore)', 'Backup restore (backup.restore)'), body: '{"jsonrpc":"2.0","method":"backup.restore","params":{"vm_name":"web","snap_name":"auto-2026-04-01"},"id":"1"}' },
      { m: 'POST', p: '/rpc', d: _L('보안 그룹 규칙 삭제 (security_group.rule.remove)', 'Security group rule remove (security_group.rule.remove)'), body: '{"jsonrpc":"2.0","method":"security_group.rule.remove","params":{"group":"default","rule_id":"r-001"},"id":"1"}' },
    ]},
    { tag: 'GPU (3)', endpoints: [
      { m: 'GET', p: '/gpu/list', d: _L('GPU 장치 목록', 'GPU device list') },
      { m: 'GET', p: '/gpu/metrics', d: _L('GPU 메트릭', 'GPU metrics') },
    ]},
  ];
  var total = 0; endpoints.forEach(function(g) { total += g.endpoints.length; });
  var h = '<h3 style="font-family:var(--font-display);margin-bottom:8px">&#128214; PureCVisor REST API</h3>';
  h += '<div class="flex gap-10 mb-8">' + H.badge('OpenAPI 3.0', 'g') + H.badge(total + ' ' + _L('엔드포인트', 'Endpoints'), 'y') + H.badge('JWT + RBAC', 'r') + '</div>';
  h += '<div class="stat-label mb-12">' + _L('기본', 'Base') + ': <code>/api/v1</code> | ' + _L('인증', 'Auth') + ': <code>Bearer JWT</code></div>';
  h += '<div class="mb-12"><input aria-label="' + _L('엔드포인트 검색...', 'Search endpoints...') + '" id="sw-search" class="sb-search" placeholder="' + _L('엔드포인트 검색...', 'Search endpoints...') + '" oninput="filterSwagger()" style="max-width:500px;font-size:13px;padding:8px 12px;border-radius:6px"></div>';
  endpoints.forEach(function(g) {
    h += '<div class="mb-16 sw-group"><div style="font-family:var(--font-display);font-size:11px;font-weight:700;color:var(--accent);text-transform:uppercase;letter-spacing:.08em;border-bottom:1px solid rgba(0,240,255,.15);padding:6px 0;margin-bottom:4px">' + g.tag + '</div>';
    g.endpoints.forEach(function(e, i) { var id = 'sw-' + g.tag.replace(/\W/g, '') + i;
      h += '<div class="mb-2 sw-ep" data-sw="' + (e.m + ' ' + e.p + ' ' + e.d).toLowerCase() + '" style="border:1px solid var(--border);border-radius:4px;overflow:hidden"><div onclick="document.getElementById(\'' + id + '\').classList.toggle(\'hidden\')" style="display:flex;align-items:center;padding:8px 12px;cursor:pointer;gap:10px;background:var(--bg2)"><span style="background:' + mc(e.m) + ';color:#fff;font-size:10px;font-weight:700;padding:2px 8px;border-radius:3px;min-width:52px;text-align:center">' + e.m + '</span><span style="font-family:monospace;font-size:12px">' + e.p + '</span><span class="stat-label" style="margin-left:auto">' + e.d + '</span>' + (e.auth === false ? '' : '<span style="font-size:9px;color:var(--yellow)">&#128274;</span>') + '</div>';
      h += '<div id="' + id + '" class="hidden" style="padding:10px 12px;background:var(--bg3);font-size:11px">';
      if (e.body) h += '<div class="mb-6"><b>' + _L('요청 본문:', 'Request Body:') + '</b><pre style="background:var(--bg);padding:8px;border-radius:4px;color:var(--green);overflow-x:auto">' + e.body + '</pre></div>';
      h += '<button class="btn" style="font-size:10px;padding:3px 10px" onclick="swTry(\'' + e.m + '\',\'' + e.p + '\',' + (e.body ? '\'' + e.body.replace(/'/g, "\\'") + '\'' : 'null') + ')">&#9654; ' + _L('실행', 'Try it') + '</button></div></div>'; });
    h += '</div>';
  });
  b.innerHTML = h;
}
window.renderSwaggerApi = renderSwaggerApi;

function filterSwagger() {
  var q = (document.getElementById('sw-search')?.value || '').toLowerCase();
  document.querySelectorAll('.sw-ep').forEach(function(el) { el.style.display = !q || el.dataset.sw.includes(q) ? '' : 'none'; });
}
window.filterSwagger = filterSwagger;

async function swTry(m, p, body) {
  var url = API_BASE + p.replace(/\{[^}]+\}/g, 'test');
  try { var opts = { headers: { Authorization: 'Bearer ' + authToken } };
    if (m === 'POST' || m === 'PUT' || m === 'DELETE') { opts.method = m; opts.headers['Content-Type'] = 'application/json'; if (body) opts.body = body; }
    var r = await fetch(url, opts); var txt = await r.text(); var pretty = txt; try { pretty = JSON.stringify(JSON.parse(txt), null, 2); } catch (e) { /* not JSON */ }
    showModal('<h2>' + _L('응답', 'Response') + ': ' + m + ' ' + p + '</h2>' + H.row(_L('상태', 'Status'), '<span style="color:' + (r.ok ? 'var(--green)' : 'var(--red)') + '">' + r.status + '</span>') + '<pre style="background:var(--bg);padding:12px;border-radius:6px;max-height:400px;overflow:auto;font-size:11px;color:var(--cyan);white-space:pre-wrap">' + pretty.replace(/</g, '&lt;') + '</pre><div style="text-align:right;margin-top:12px"><button class="btn" onclick="closeModal()">' + t('btn.close') + '</button></div>');
  } catch (e) { toast(_L('요청 실패', 'Request failed') + ': ' + e.message, false); }
}
window.swTry = swTry;

/* ═══ KEYBOARD HELP OVERLAY ═══ */
var kbdHelpOpen = false;
window.kbdHelpOpen = kbdHelpOpen;

function toggleKbdHelp() {
  if (kbdHelpOpen) { closeKbdHelp(); return; }
  kbdHelpOpen = true; window.kbdHelpOpen = kbdHelpOpen;
  var shortcuts = [
    ['Ctrl+K', _L('커맨드 팔레트', 'Command Palette')],
    ['Ctrl+N', _L('새 VM', 'New VM')],
    ['Ctrl+D', _L('VM 설정', 'VM Settings')],
    ['Ctrl+P', _L('환경설정', 'Preferences')],
    ['F11', _L('전체 화면', 'Fullscreen')],
    ['Escape', _L('대화상자 닫기', 'Close Dialog')],
    ['?', _L('이 도움말', 'This Help')],
    ['Ctrl+Shift+F', _L('전역 검색', 'Global Search')],
    ['Ctrl+B', _L('사이드바 전환', 'Toggle Sidebar')],
  ];
  var ov = document.createElement('div');
  ov.id = 'kbd-help-overlay'; ov.className = 'kbd-overlay';
  ov.onclick = function(e) { if (e.target === ov) closeKbdHelp(); };
  /* ADR-013 DOM-safe: kbd 오버레이를 el()로 조립 (문자열 innerHTML 제거). */
  var el = PCV.uxlib.el;
  ov.appendChild(el('div', { class: 'kbd-box' },
    el('div', { class: 'kbd-title' }, _L('키보드 단축키', 'Keyboard Shortcuts')),
    el('div', { class: 'kbd-grid' },
      shortcuts.map(function(s) {
        return el('div', { class: 'kbd-row' },
          el('span', { class: 'kbd-key' }, s[0]),
          el('span', { class: 'kbd-desc' }, s[1])
        );
      })
    ),
    el('div', { class: 'kbd-close' }, _L('? 또는 Esc 키로 닫기', 'Press ? or Esc to close'))
  ));
  document.body.appendChild(ov);
}
window.toggleKbdHelp = toggleKbdHelp;

function closeKbdHelp() { kbdHelpOpen = false; window.kbdHelpOpen = kbdHelpOpen; var el = document.getElementById('kbd-help-overlay'); if (el) el.remove(); }
window.closeKbdHelp = closeKbdHelp;

/* ── PCV.help namespace export ────────────────────── */
PCV.help = {
  renderHelp: renderHelp,
  filterHelp: filterHelp,
  renderRestGuide: renderRestGuide,
  renderServiceGuide: renderServiceGuide,
  filterGuide: filterGuide,
  renderSwaggerApi: renderSwaggerApi,
  filterSwagger: filterSwagger,
  swTry: swTry,
  toggleKbdHelp: toggleKbdHelp,
  closeKbdHelp: closeKbdHelp
};
})(window.PCV);
