                                                                  
                               
                                   
                                   
                                                                     
  
                           
                                                                               
                                                                             
                                                                          
                                          
  
                                                                                
                                                                                  
                                                                                  
  
                        
                                              
                                                                           
                                                              
                                                           
                                                                
                                                                      
                                                    
                                    
  
                                                                   
                                                                 
   

                                           
                                                  
var _L = window._L = function(ko, en) {
  return (typeof I18N !== 'undefined' && I18N.getLang() === 'en') ? en : ko;
};

window.PCV = window.PCV || {};
(function(PCV) {

                              
                                                             
                                                          
                                                               
                                       
                                                                           
                                                      
                      
function renderHelp(b) {
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  var cfg = (typeof PCV !== 'undefined' && PCV.config) ? PCV.config : {};
  var RPC = cfg.RPC_COUNT || 304, REST = cfg.REST_COUNT || 229, METRICS = cfg.METRICS_COUNT || 155;

                                                                
  var featChips = el('div', { class: 'help-feat-chips' },
    el('span', { class: 'help-feat-chip' }, '🔐 ' + _L('테넌트 암호화 오버레이', 'Encrypted tenant overlay')),
    el('span', { class: 'help-feat-chip' }, '🚦 ' + _L('고급 QoS', 'Advanced QoS')),
    el('span', { class: 'help-feat-chip' }, '🔎 ' + _L('관측성 트레이싱', 'Observability tracing')),
    el('span', { class: 'help-feat-chip' }, '🛡 ' + _L('Suricata DPI/IPS', 'Suricata DPI/IPS')));
  var banner = el('div', { class: 'help-banner' },
    el('div', { class: 'help-banner-main' },
      el('div', { class: 'help-banner-title' }, 'PureCVisor 2.0 — ' + _L('전수 API 레퍼런스', 'Full API Reference')),
      el('div', { class: 'help-banner-sub' }, RPC + ' RPC · ' + REST + ' REST · 178 CLI · ' + _L('네임스페이스별 접이식 카탈로그', 'collapsible per-namespace catalog')),
      featChips),
    el('a', { href: '/ui/docs.html', target: '_blank', class: 'help-guide-btn' }, '📖 ' + _L('문서 홈', 'Open Docs')));

                                                
  var stats = el('div', { class: 'sg grid-3' },
    HN.card('⚙ ' + _L('시스템', 'System'), [
      HN.row(_L('RPC 메서드', 'RPC Methods'), RPC + '+'),
      HN.row(_L('REST 엔드포인트', 'REST Endpoints'), REST + '+'),
      HN.row(_L('CLI 커맨드', 'CLI Commands'), '178')]),
    HN.card('📈 ' + _L('모니터링', 'Monitoring'), [
      HN.row(_L('Prometheus 메트릭', 'Prometheus Metrics'), METRICS + '+'),
      HN.row(_L('메트릭 접두', 'Metric Prefixes'), 'node_* / purecvisor_*'),
      HN.row(_L('노출 엔드포인트', 'Exposed at'), el('code', null, '/metrics'))]),
    HN.card('💻 ' + _L('인프라', 'Infrastructure'), [
      HN.row(_L('에디션', 'Edition'), window.pcvClusterEnabled ? '3-Node Cluster HA' : _L('Single Edge (단일 노드)', 'Single Edge (single-node)')),
      HN.row(_L('Web UI 페이지', 'Web UI Pages'), '40'),
      HN.row(_L('i18n 키', 'i18n Keys'), '340+')]));

                                       
                                                                     
                                                      
                                                                   
                                                                       
                                                  
  var NS = [
                  
    { id: 'vm', ko: '가상 머신', en: 'Virtual Machines', rows: [
      ['vm.create', 1, 'vm create', 'summary', 'VM 생성 (virt-install + cloud-init)', 'Create VM (virt-install + cloud-init)'],
      ['vm.start', 1, 'vm start', 'summary/console', 'VM 부팅: NUMA vCPU 핀 + libvirt 도메인 시작', 'Boot VM: NUMA vCPU pin + libvirt domain start'],
      ['vm.stop', 1, 'vm stop', 'summary/console', '실행 도메인 종료/강제 파괴', 'Shut down / destroy running domain'],
      ['vm.pause', 1, 'vm pause', 'summary', 'VM 일시정지 (vCPU 동결)', 'Suspend VM (freeze vCPUs)'],
      ['vm.resume', 1, 'vm resume', 'summary', '일시정지된 VM 재개', 'Resume a paused VM'],
      ['vm.guest.ping', 0, 'vm guest-ping', 'summary', 'qemu-guest-agent 채널 핑', 'Ping qemu-guest-agent channel'],
      ['vm.guest.agent.status', 0, 'vm guest-agent-status', 'summary', '게스트 에이전트 채널/패키지 상태 조회', 'Inspect guest-agent channel/package status'],
      ['vm.guest.agent.ensure_channel', 1, 'vm guest-agent-ensure-channel', 'summary', 'qemu-guest-agent libvirt 채널 장치 추가', 'Add qemu-guest-agent libvirt channel device'],
      ['vm.guest.fsinfo', 0, '-', 'summary', '에이전트로 게스트 파일시스템/디스크 사용량 조회', 'Query guest filesystem/disk usage via agent'],
      ['vm.guest.exec', 2, 'vm guest-exec', '-', '에이전트로 게스트 내부 명령 실행 (관리자 전용)', 'Execute command inside guest via agent (admin-only)'],
      ['vm.guest.shutdown', 1, 'vm guest-shutdown', 'summary', '게스트 에이전트 통한 우아한 ACPI 종료', 'Graceful ACPI shutdown via guest agent'],
      ['vm.delete', 1, 'vm delete', 'summary', 'VM 삭제 (비동기; operator는 소유 범위)', 'Delete VM (async; owner-scoped for operator)'],
      ['vm.delete.status', 0, 'vm delete-status', 'summary', '비동기 VM 삭제 진행률 폴링', 'Poll async VM-delete progress'],
      ['vm.list', 0, 'vm list', 'dashboard', '전체 VM 목록 (offset/limit 페이징)', 'List all VMs (offset/limit paging)'],
      ['vm.limit', 1, 'vm limit', 'summary', 'cgroup CPU/MEM 제한 설정', 'Set cgroup CPU/MEM limits'],
      ['vm.metrics', 0, 'monitor metrics', 'summary/performance', 'VM별 CPU/MEM/디스크/네트워크 메트릭', 'Per-VM CPU/MEM/disk/net metrics'],
      ['vm.rename', 1, 'vm rename', 'summary', '중지된 VM 이름 변경 + 표준 디스크 경로', 'Rename shut-off VM + standard disk path'],
      ['vm.vnc', 1, 'vm vnc', 'console', '실행 중 VM의 VNC 포트 조회', 'Get VNC port for running VM'],
      ['get_vnc_info', 1, '-', 'console', 'VNC 접속 정보 (vm.vnc 레거시 별칭)', 'VNC access info (legacy alias of vm.vnc)'],
      ['vm.mount_iso', 1, 'iso mount', 'console', '가상 CD-ROM에 ISO 마운트', 'Mount ISO into virtual CD-ROM'],
      ['vm.eject', 1, 'vm eject / iso eject', 'console', '가상 CD-ROM에서 ISO 꺼내기', 'Eject ISO from virtual CD-ROM'],
      ['vm.import.ec2', 2, 'cloud import', 'cloud-migration', 'AWS EC2 AMI 임포트 (near-live 지원)', 'Import AWS EC2 AMI (near-live)'],
      ['vm.export.ec2', 2, 'cloud export', 'cloud-migration', 'VM을 EC2 AMI로 내보내기', 'Export VM to EC2 AMI'],
      ['vm.resize_disk', 1, 'vm disk-resize', 'summary', 'VM 디스크 리사이즈 (오프라인/표준)', 'Resize VM disk (offline/standard)'],
      ['vm.clone', 1, 'vm clone', 'summary', 'VM 복제 (cow/full; 게스트리셋/템플릿)', 'Clone VM (cow/full; guest-reset/template)'],
      ['vm.set_bandwidth', 1, 'vm bandwidth', 'summary', 'VM NIC 인/아웃바운드 대역폭 설정', 'Set VM NIC inbound/outbound bandwidth'],
      ['vm.snapshot.create', 1, 'snapshot create', 'snapshots', 'VM 디스크 ZFS 스냅샷 생성', 'Create ZFS snapshot of VM disk'],
      ['vm.snapshot.list', 0, 'snapshot list', 'snapshots', 'VM ZFS 스냅샷 목록', 'List VM ZFS snapshots'],
      ['vm.snapshot.rollback', 2, 'snapshot rollback', 'snapshots', 'VM을 스냅샷으로 롤백', 'Roll VM back to a snapshot'],
      ['vm.snapshot.delete', 2, 'snapshot delete', 'snapshots', 'VM 스냅샷 1건 삭제', 'Delete one VM snapshot'],
      ['vm.snapshot.delete_all', 2, 'vm snapshot-delete-all', 'snapshots', '스냅샷 일괄 삭제 (prefix/keep_recent)', 'Bulk-delete snapshots (prefix/keep_recent)'],
      ['vm.set_memory', 1, 'vm set-memory', 'summary', 'RAM 핫 설정 (MB)', 'Hot-set RAM (MB)'],
      ['vm.set_vcpu', 1, 'vm set-vcpu', 'summary', 'vCPU 개수 핫 설정', 'Hot-set vCPU count'],
      ['vm.pin_vcpu', 1, 'vm pin-vcpu', 'summary', 'vCPU를 호스트 cpuset에 핀', 'Pin vCPU to host cpuset'],
      ['vm.memory.stats', 0, 'vm memory-stats', 'performance', '메모리 balloon/RSS/swap 통계', 'Memory balloon/RSS/swap stats'],
      ['vm.cpu.stats', 0, 'vm cpu-stats', 'performance', 'CPU 시간 + vCPU별 통계', 'CPU time + per-vCPU stats'],
      ['vm.disk.live_resize', 1, '-', 'summary', '실행 도메인 디스크 라이브 리사이즈', 'Live-resize VM disk (running domain)'],
      ['vm.usb.attach', 1, 'vm usb-attach', 'summary', 'USB 호스트 장치 연결 (vendor/product)', 'Attach USB host device (vendor/product)'],
      ['vm.usb.detach', 1, 'vm usb-detach', 'summary', 'USB 호스트 장치 분리', 'Detach USB host device'],
      ['vm.usb.list', 0, 'vm usb-list', 'summary', 'VM에 연결된 USB hostdev 목록', 'List USB hostdevs attached to VM'],
      ['vm.blkio.set', 1, 'vm blkio-set / vm disk-throttle', 'summary', '블록 I/O 제한 설정 (bps/iops)', 'Set block I/O limits (bps/iops)'],
      ['vm.blkio.get', 0, 'vm blkio-get', 'summary', '블록 I/O 제한 조회', 'Get block I/O limits'],
      ['vm.import.status', 0, 'cloud status', 'cloud-migration', '클라우드 임포트 작업 상태 폴링', 'Poll cloud-import job status'],
      ['vm.export.status', 0, 'cloud status', 'cloud-migration', '클라우드 익스포트 작업 상태 폴링', 'Poll cloud-export job status'],
      ['vm.snapshot.schedule.set', 1, '-', 'snapshots', 'VM 자동 스냅샷 스케줄 설정', 'Set VM auto-snapshot schedule'],
      ['vm.snapshot.schedule.list', 0, '-', 'snapshots', 'VM 스냅샷 스케줄 목록', 'List VM snapshot schedules'],
      ['vm.snapshot.schedule.delete', 1, '-', 'snapshots', 'VM 스냅샷 스케줄 삭제', 'Delete a VM snapshot schedule'],
      ['vm.import.ova', 1, 'vm import-ova', 'cloud-migration', 'OVA 파일에서 VM 임포트', 'Import VM from OVA file'],
      ['vm.export.ova', 2, 'vm export-ova', 'cloud-migration', 'VM을 OVA 파일로 익스포트', 'Export VM to OVA file'],
      ['vm.batch', 1, 'vm batch', 'dashboard', '여러 VM 일괄 시작/중지', 'Batch start/stop multiple VMs'],
      ['vm.numa.info', 0, 'vm numa', '-', '호스트 NUMA/코어 할당 토폴로지', 'Host NUMA/core allocation topology'],
      ['vm.autostart', 1, 'vm autostart', 'summary', 'VM 자동시작 활성/비활성', 'Enable/disable VM autostart'],
      ['vm.sla.report', 0, 'vm sla', '-', 'VM 가동/다운타임 SLA 리포트', 'VM uptime/downtime SLA report'],
      ['vm.schedule.list', 0, 'vm schedule', '-', 'VM 전원 스케줄 목록 (백엔드 스텁)', 'List VM power schedules (backend stub)'],
      ['vm.schedule.set', 1, 'vm schedule', '-', 'VM 전원 스케줄 설정 (스텁: applied=false)', 'Set VM power schedule (backend stub)'],
      ['vm.billing.report', 0, 'billing report', '-', 'VM 과금 리포트 (미터링 백엔드 없음)', 'VM billing report (no metering backend)'],
      ['vm.security_group.set', 1, 'vm security-group', 'security-groups', 'VM에 보안 그룹 할당', 'Assign security group to VM'],
      ['vm.event.webhook.list', 0, 'webhook list', '-', 'VM 이벤트 웹훅 대상 목록', 'List VM-event webhook targets'],
    ]},
    { id: 'container', ko: '컨테이너 (LXC)', en: 'Containers (LXC)', rows: [
      ['container.create', 1, 'container create', 'containers', 'LXC 컨테이너 생성', 'Create LXC container'],
      ['container.destroy', 2, 'container destroy', 'containers', 'LXC 컨테이너 파괴', 'Destroy LXC container'],
      ['container.start', 1, 'container start', 'containers', '컨테이너 시작', 'Start container'],
      ['container.stop', 1, 'container stop', 'containers', '컨테이너 중지', 'Stop container'],
      ['container.list', 0, 'container list', 'containers', '컨테이너 목록 (페이징)', 'List containers (paged)'],
      ['container.metrics', 0, 'container metrics', 'containers', '컨테이너 CPU/MEM/리소스 사용량', 'Container CPU/MEM/resource usage'],
      ['container.exec', 2, 'container exec', 'containers', '컨테이너 내부 명령 실행', 'Exec command inside container'],
      ['container.snapshot.create', 2, 'container snapshot', 'containers', '컨테이너 ZFS 스냅샷 생성', 'Create container ZFS snapshot'],
      ['container.snapshot.rollback', 2, 'container snapshot', 'containers', '컨테이너를 스냅샷으로 롤백', 'Roll container back to snapshot'],
      ['container.snapshot.delete', 2, 'container snapshot', 'containers', '컨테이너 스냅샷 삭제', 'Delete container snapshot'],
      ['container.snapshot.list', 0, 'container snapshot', 'containers', '컨테이너 스냅샷 목록', 'List container snapshots'],
      ['container.set_limits', 2, 'container set-limits', 'containers', '메모리/cpu_quota 제한 설정', 'Set memory/cpu_quota limits'],
      ['container.nic.list', 0, 'container nic-list', 'containers', '컨테이너 NIC 목록', 'List container NICs'],
      ['container.nic.attach', 2, 'container nic-attach', 'containers', '컨테이너에 NIC 연결 (브릿지)', 'Attach NIC to container (bridge)'],
                                                               
                                                                             
      ['container.nic.detach', 2, 'container nic-detach', 'containers', '컨테이너에서 NIC 분리 (nic-name)', 'Detach NIC from container (nic-name)'],
      ['container.set_bandwidth', 2, 'container set-bandwidth', 'containers', '컨테이너 인/아웃바운드 대역폭 설정', 'Set container inbound/outbound bandwidth'],
      ['container.logs', 0, 'container logs', 'containers', '컨테이너 로그 tail (N줄)', 'Tail container logs (lines N)'],
      ['container.volume.attach', 2, 'container volume-attach', 'containers', '호스트 볼륨을 컨테이너에 연결', 'Attach host volume to container'],
      ['container.volume.detach', 2, 'container volume-detach', 'containers', '컨테이너에서 볼륨 분리', 'Detach volume from container'],
      ['container.volume.list', 0, 'container volume-list', 'containers', '컨테이너 볼륨 목록', 'List container volumes'],
      ['container.env.set', 2, 'container env-set', 'containers', '컨테이너 환경변수 설정', 'Set container env variable'],
      ['container.env.list', 0, 'container env-list', 'containers', '컨테이너 환경변수 목록', 'List container env variables'],
      ['container.env.delete', 2, 'container env-delete', 'containers', '컨테이너 환경변수 삭제', 'Delete container env variable'],
      ['container.health.set', 2, 'container health-set', 'containers', '컨테이너 헬스체크 설정 (type/target)', 'Set container health-check (type/target)'],
      ['container.health.get', 0, 'container health-get', 'containers', '컨테이너 헬스체크 설정 조회', 'Get container health-check config'],
      ['container.health.delete', 2, 'container health-delete', 'containers', '컨테이너 헬스체크 삭제', 'Delete container health-check'],
      ['container.clone', 1, '-', 'containers', '컨테이너 복제 (source→dest)', 'Clone container (source to dest)'],
      ['container.memory.stats', 0, '-', 'containers', '컨테이너 메모리 통계', 'Container memory statistics'],
      ['container.health.check', 0, 'container health-check', 'containers', '컨테이너 헬스 프로브 즉시 실행 (미등록→VIEWER)', 'Run container health probe now (unlisted defaults VIEWER)'],
    ]},
    { id: 'network', ko: '네트워크', en: 'Network', rows: [
      ['network.create', 2, 'network create', 'networks', '관리형 네트워크/전용 L2 브릿지 생성', 'Create managed network/dedicated L2 bridge'],
      ['network.delete', 2, 'network delete', 'networks', '브릿지 삭제', 'Delete bridge'],
      ['network.list', 0, 'network list', 'networks', '브릿지 목록', 'List bridges'],
      ['network.info', 0, '-', 'networks', '브릿지 상세/정보', 'Bridge detail/info'],
      ['network.mode_set', 2, 'network mode', 'networks', '비물리 브릿지 모드 변경', 'Change non-physical bridge mode'],
      ['network.bind_phys', 2, 'network bind', 'networks', '사용 중단 — network create --mode bridge 사용', 'Disabled — use network create --mode bridge'],
      ['network.dhcp_toggle', 2, 'network dhcp', 'networks', '비물리 브릿지 DHCP 토글', 'Toggle DHCP on non-physical bridge'],
      ['network.ovs.create', 2, '-', 'networks', 'OVS 브릿지 생성', 'Create OVS bridge'],
      ['network.ovs.delete', 2, '-', 'networks', 'OVS 브릿지 삭제', 'Delete OVS bridge'],
      ['network.ovs.vxlan.add', 2, '-', 'networks', 'OVS에 VXLAN 터널 포트 추가', 'Add VXLAN tunnel port to OVS'],
      ['network.ovs.vxlan.del', 2, '-', 'networks', 'VXLAN 터널 포트 삭제', 'Delete VXLAN tunnel port'],
      ['network.qos.set', 2, 'network qos-set', '-', '인터페이스별 QoS 레이트 설정', 'Set per-interface QoS rate'],
      ['network.qos.get', 0, 'network qos-get', '-', '인터페이스별 QoS 조회', 'Get per-interface QoS'],
      ['network.qos.remove', 2, 'network qos-remove', '-', '인터페이스별 QoS 제거', 'Remove per-interface QoS'],
    ]},
    { id: 'storage', ko: '스토리지', en: 'Storage', rows: [
      ['storage.pool.list', 0, 'storage pool', 'storage', 'ZFS 풀 목록', 'List ZFS pools'],
      ['storage.zvol.list', 0, 'storage zvol', 'storage', 'zvol 목록', 'List zvols'],
      ['storage.zvol.create', 2, 'storage zvol', 'storage', 'zvol 생성', 'Create zvol'],
      ['storage.zvol.delete', 2, 'storage zvol', 'storage', 'zvol 삭제', 'Delete zvol'],
      ['storage.pool.create', 2, '-', 'storage', 'ZFS 풀 생성', 'Create ZFS pool'],
      ['storage.pool.destroy', 2, '-', 'storage', 'ZFS 풀 파괴', 'Destroy ZFS pool'],
      ['storage.pool.scrub', 2, '-', 'storage', '풀 백그라운드 scrub 시작', 'Start background pool scrub'],
      ['storage.pool.health', 0, 'storage health', 'storage', '풀 상태 (state/errors)', 'Pool health (state/errors)'],
      ['storage.pool.forecast', 0, 'storage pool-forecast', 'storage', '풀 용량 소진 예측', 'Pool capacity-exhaustion forecast'],
    ]},
                      
    { id: 'tenant_overlay', ko: '테넌트 암호화 오버레이', en: 'Encrypted Tenant Overlay', isNew: true, rpcOnly: true, rows: [
      ['tenant_overlay.create', 2, '-', '-', '암호화 테넌트 오버레이 생성 + 서브넷 할당', 'Create encrypted tenant overlay + assign subnet'],
      ['tenant_overlay.delete', 2, '-', '-', '테넌트 오버레이 삭제', 'Delete tenant overlay'],
      ['tenant_overlay.list', 2, '-', '-', '테넌트 오버레이 목록', 'List tenant overlays'],
      ['tenant_overlay.get', 2, '-', '-', '테넌트 오버레이 상세 조회', 'Get one tenant overlay detail'],
      ['tenant_overlay.attach_vm', 2, '-', '-', 'VM을 테넌트 오버레이에 조인 (내부 IP 할당)', 'Join a VM to tenant overlay (assign internal IP)'],
      ['tenant_overlay.detach_vm', 2, '-', '-', '테넌트 오버레이에서 VM 제거', 'Remove VM from tenant overlay'],
    ]},
    { id: 'overlay', ko: '오버레이 메시', en: 'Overlay Mesh', isNew: true, rows: [
      ['overlay.create', 2, '-', 'overlay', '오버레이 네트워크 생성', 'Create overlay network'],
      ['overlay.delete', 2, '-', 'overlay', '오버레이 삭제', 'Delete overlay'],
      ['overlay.list', 0, '-', 'overlay', '오버레이 목록', 'List overlays'],
      ['overlay.info', 0, '-', 'overlay', '오버레이 상세', 'Overlay detail'],
      ['overlay.add_peer', 2, '-', 'overlay', '오버레이 메시에 피어 추가', 'Add peer to overlay mesh'],
      ['overlay.remove_peer', 2, '-', 'overlay', '오버레이에서 피어 제거', 'Remove peer from overlay'],
    ]},
    { id: 'qos', ko: 'QoS · 카오스', en: 'QoS & Chaos', isNew: true, rpcOnly: true, rows: [
      ['qos.vm.set', 2, '-', '-', 'VM별 대역폭 QoS 설정', 'Set per-VM bandwidth QoS'],
      ['qos.vm.get', 0, '-', '-', 'VM별 QoS 조회', 'Get per-VM QoS'],
      ['qos.tenant.set', 2, '-', '-', '테넌트별 QoS 설정', 'Set per-tenant QoS'],
      ['qos.tenant.get', 0, '-', '-', '테넌트별 QoS 조회', 'Get per-tenant QoS'],
      ['qos.stats', 0, '-', '-', 'QoS 집행 통계', 'QoS enforcement statistics'],
      ['qos.chaos.start', 2, '-', '-', 'netem 카오스(지연/손실) 폴트 주입 시작', 'Start netem chaos (delay/loss) fault injection'],
      ['qos.chaos.stop', 2, '-', '-', 'netem 카오스 중지', 'Stop netem chaos'],
      ['qos.chaos.status', 2, '-', '-', '카오스 상태 (민감→ADMIN)', 'Chaos status (sensitive, ADMIN)'],
    ]},
    { id: 'debug', ko: '트래픽 트레이싱', en: 'Traffic Tracing', isNew: true, rpcOnly: true, rows: [
      ['debug.trace.start', 2, '-', '-', '트래픽 트레이스 시작 (관리자; 전체 테넌트)', 'Start traffic trace (admin; all-tenant surface)'],
      ['debug.trace.stop', 2, '-', '-', '트레이스 중지', 'Stop trace'],
      ['debug.trace.status', 2, '-', '-', '트레이스 상태', 'Trace status'],
      ['debug.trace.list', 2, '-', '-', '트레이스 목록', 'List traces'],
      ['debug.trace.report', 2, '-', '-', '트레이스 리포트/병목 분석', 'Trace report/bottleneck analysis'],
    ]},
    { id: 'suricata', ko: 'Suricata IDS/IPS', en: 'Suricata IDS/IPS', isNew: true, rows: [
      ['suricata.policy.set', 2, '-', 'mon-security', 'Suricata 탐지 정책 설정', 'Set Suricata detection policy'],
      ['suricata.policy.get', 2, '-', 'mon-security', 'Suricata 정책 조회 (관리자: 전체 테넌트 뷰)', 'Get Suricata policy (admin: whole-tenant view)'],
      ['suricata.rules.update', 2, '-', 'mon-security', 'Suricata 룰 시그니처 업데이트', 'Update Suricata rule signatures'],
      ['suricata.status', 2, '-', 'mon-security', 'Suricata 엔진 상태', 'Suricata engine status'],
      ['suricata.ips.status', 2, '-', 'mon-security', 'IPS(인라인 차단) 상태 — degraded 포함', 'IPS (inline block) status — includes degraded'],
      ['suricata.ips.enable', 2, '-', 'mon-security', 'IPS 인라인 차단 활성화', 'Enable IPS inline blocking'],
      ['suricata.ips.disable', 2, '-', 'mon-security', 'IPS 인라인 차단 비활성화', 'Disable IPS inline blocking'],
      ['suricata.ips.drop.list', 2, '-', 'mon-security', 'IPS 차단 대상 SID 목록', 'List IPS drop-target SIDs'],
      ['suricata.ips.drop.add', 2, '-', 'mon-security', 'IPS 차단 대상 SID 추가(비동기)', 'Add IPS drop-target SIDs (async)'],
      ['suricata.ips.drop.remove', 2, '-', 'mon-security', 'IPS 차단 대상 SID 제거(비동기)', 'Remove IPS drop-target SIDs (async)'],
    ]},
    { id: 'vpc', ko: 'Local VPC', en: 'Local VPC', isNew: true, rows: [
      ['vpc.list', 0, 'vpc list', 'vpcs', 'VPC 목록', 'List VPCs'],
      ['vpc.get', 0, 'vpc get', 'vpcs', 'VPC 상세', 'Get VPC detail'],
      ['vpc.subnet.list', 0, 'vpc subnet-list', 'vpcs', 'VPC 서브넷 목록', 'List VPC subnets'],
      ['vpc.attachment.list', 0, 'vpc attachment-list', 'vpcs', 'VPC VM 연결 목록', 'List VPC VM attachments'],
      ['vpc.service.list', 0, 'vpc service-list', 'vpcs', '공개 서비스 목록', 'List published services'],
      ['vpc.status', 0, 'vpc status', 'vpcs', 'VPC reconcile 상태', 'Get VPC reconcile status'],
      ['vpc.backend.list', 0, 'vpc backends', 'vpcs', 'VPC backend 준비 상태·용량', 'Get VPC backend readiness and capacity'],
      ['vpc.create', 1, 'vpc create', 'vpcs', 'VPC + 첫 subnet 생성(비동기)', 'Create a VPC + initial subnet (async)'],
      ['vpc.delete', 2, 'vpc delete', 'vpcs', 'VPC 삭제(비동기)', 'Delete a VPC (async)'],
      ['vpc.egress.set', 1, 'vpc egress-set', 'vpcs', 'VPC egress 모드 설정', 'Set VPC egress mode'],
      ['vpc.subnet.create', 1, 'vpc subnet-create', 'vpcs', 'VPC 서브넷 생성', 'Create a VPC subnet'],
      ['vpc.subnet.delete', 1, 'vpc subnet-delete', 'vpcs', 'VPC 서브넷 삭제', 'Delete a VPC subnet'],
      ['vpc.attachment.create', 1, 'vpc attachment-create', 'vpcs', 'VM을 VPC 서브넷에 연결', 'Attach a VM to a VPC subnet'],
      ['vpc.attachment.delete', 1, 'vpc attachment-delete', 'vpcs', 'VPC VM 연결 해제', 'Detach a VM from a VPC'],
      ['vpc.service.publish', 1, 'vpc service-publish', 'vpcs', 'VM 서비스 공개', 'Publish a VM service'],
      ['vpc.service.unpublish', 1, 'vpc service-unpublish', 'vpcs', 'VM 서비스 공개 해제', 'Unpublish a VM service'],
      ['vpc.reconcile', 2, 'vpc reconcile', 'vpcs', 'VPC desired state 수렴', 'Reconcile VPC desired state'],
    ]},
                         
    { id: 'agent', ko: 'AI 에이전트', en: 'AI Agent', rows: [
      ['agent.config.get', 0, 'agent config', 'selfhealing', 'AI 에이전트 설정 조회', 'Get AI agent config'],
      ['agent.config.set', 2, 'agent set', 'selfhealing', 'AI 프로바이더/api_key/활성 설정', 'Set AI provider/api_key/enabled'],
      ['agent.history', 0, 'agent history', 'selfhealing', 'AI 합의/결정 이력', 'AI consensus/decision history'],
      ['agent.compare_manual', 2, '-', 'selfhealing', 'AI 진단 수동 트리거', 'Manually trigger AI diagnosis now'],
    ]},
    { id: 'ai', ko: 'AI 힐링 승인', en: 'AI Healing Approval', rows: [
      ['ai.healing.approve', 0, '-', 'selfhealing', 'AI 제안 힐링 승인 (미등록→VIEWER)', 'Approve AI-proposed healing (unlisted defaults VIEWER)'],
      ['ai.healing.reject', 0, '-', 'selfhealing', 'AI 제안 힐링 거절 (미등록→VIEWER)', 'Reject AI-proposed healing (unlisted defaults VIEWER)'],
    ]},
    { id: 'alert', ko: '알림', en: 'Alerts', rows: [
      ['alert.history', 0, 'alert list', 'mon-alerts', '알림 이력', 'Alert history'],
      ['alert.config.get', 0, 'alert config', 'mon-alerts', '알림 임계값/웹훅 조회', 'Get alert thresholds/webhook'],
      ['alert.config.set', 2, 'alert set', 'mon-alerts', '알림 임계값/웹훅 설정', 'Set alert thresholds/webhook'],
      ['alert.config.reload', 2, 'alert reload', 'mon-alerts', 'daemon.conf에서 알림 설정 재로드', 'Reload alert config from daemon.conf'],
      ['alert.action.list', 0, 'alert actions', 'mon-alerts', '알림 트리거 액션 목록', 'List alert-triggered actions'],
      ['alert.ack', 1, '-', 'mon-alerts', '개별 알림 확인 처리 (ACK)', 'Acknowledge a single alert'],
      ['alert.silence', 2, '-', 'mon-alerts', '알림을 일정 기간 무음화', 'Silence an alert for a window'],
      ['alert.silence.list', 0, '-', 'mon-alerts', '활성 무음(silence) 목록', 'List active silences'],
      ['alert.dlq.list', 2, '-', 'mon-alerts', '알림 웹훅 데드레터 큐 목록', 'List alert webhook dead-letter queue'],
                                                                             
                                                                        
    ]},
    { id: 'auth', ko: '인증 · RBAC', en: 'Auth & RBAC', rows: [
      ['auth.user.create', 2, 'auth create', 'accounts', 'RBAC 사용자 생성', 'Create RBAC user'],
      ['auth.user.list', 2, 'auth list', 'accounts', '사용자 목록', 'List users'],
      ['auth.user.delete', 2, 'auth delete', 'accounts', '사용자 삭제', 'Delete user'],
      ['auth.role.set', 2, 'auth role', 'accounts', '사용자 역할 설정', 'Set user role'],
      ['auth.apikey.create', 2, '-', 'apimgmt', 'API 키 생성 (이름/역할/만료)', 'Create API key (name/role/expiry)'],
      ['auth.apikey.list', 2, '-', 'apimgmt', 'API 키 목록', 'List API keys'],
      ['auth.apikey.revoke', 2, '-', 'apimgmt', 'API 키 폐기', 'Revoke an API key'],
      ['auth.session.revoke', 2, '-', 'apimgmt', '세션 1건 폐기 (jti 블랙리스트)', 'Revoke one session (jti blacklist)'],
      ['auth.user.sessions.revoke', 2, '-', 'apimgmt', '사용자의 모든 리프레시 세션 폐기', 'Revoke all refresh sessions of a user'],
      ['auth.totp.status', 0, '-', 'settings', '본인 TOTP 상태 조회', 'Get own TOTP status'],
      ['auth.totp.disable', 0, '-', 'settings', '본인 TOTP 해제', 'Disable own TOTP'],
      ['auth.totp.recovery.regenerate', 0, '-', 'settings', '본인 복구코드 재발급', 'Regenerate own recovery codes'],
      ['auth.totp.reset', 2, '-', 'accounts', '대상 사용자 TOTP 강제 초기화', 'Reset TOTP for a user'],
    ]},
    { id: 'backup', ko: '백업', en: 'Backup', rows: [
      ['backup.policy.set', 2, 'backup set', 'backup', 'VM 백업 정책 설정', 'Set VM backup policy'],
      ['backup.export_s3', 2, 'backup export-s3', 'backup', '백업 스냅샷 S3 내보내기', 'Export backup snapshot to S3'],
      ['backup.policy.list', 0, 'backup list', 'backup', '백업 정책 목록', 'List backup policies'],
      ['backup.policy.delete', 2, 'backup delete', 'backup', '백업 정책 삭제', 'Delete backup policy'],
      ['backup.history', 0, 'backup history', 'backup', 'VM 백업 이력', 'Backup history for VM'],
      ['backup.restore', 2, 'backup restore', 'backup', '백업 스냅샷에서 VM 복원', 'Restore VM from backup snapshot'],
      ['backup.incremental', 2, 'backup incremental', 'backup', '증분 백업 실행', 'Run incremental backup'],
      ['backup.verify', 2, 'backup verify', 'backup', '백업 무결성 검증', 'Verify backup integrity'],
      ['backup.replicate', 2, 'backup replicate', 'backup', '원격 대상으로 백업 복제', 'Replicate backup to remote target'],
      ['backup.snapshot.verify', 2, 'snapshot verify', 'backup', '스냅샷 존재/무결성 검증 (비동기)', 'Verify snapshot exists/intact (async)'],
    ]},
    { id: 'cloud', ko: '클라우드 마이그레이션', en: 'Cloud Migration', rows: [
      ['cloud.jobs.list', 1, 'cloud jobs', 'cloud-migration', '클라우드 마이그레이션 작업 목록', 'List cloud migration jobs'],
      ['cloud.job.cancel', 2, 'cloud cancel', 'cloud-migration', '마이그레이션 작업 취소', 'Cancel a migration job'],
    ]},
    { id: 'config', ko: '설정', en: 'Config', rows: [
      ['config.history', 0, 'config history', 'config-mgmt', '설정 변경 이력', 'Config-change history'],
      ['config.backup', 0, 'config backup', 'config-mgmt', '현재 설정 파일 스냅샷', 'Snapshot current config file'],
      ['config.reload', 2, '-', 'config-mgmt', '설정 핫 리로드 (SIGHUP)', 'Hot-reload config (SIGHUP)'],
    ]},
    { id: 'daemon', ko: '데몬', en: 'Daemon', rows: [
      ['daemon.version', 0, 'node version', 'host', '데몬 버전 문자열', 'Daemon version string'],
      ['daemon.update_check', 0, '-', 'host', '신규 버전 확인 (읽기 전용)', 'Check for newer version (read-only)'],
      ['daemon.config.get', 0, '-', 'config-mgmt', 'daemon.conf 설정 조회', 'Get daemon.conf settings'],
      ['daemon.config.set', 2, '-', 'config-mgmt', 'daemon.conf 설정 변경', 'Set daemon.conf settings'],
    ]},
    { id: 'device', ko: '장치 핫플러그', en: 'Device Hotplug', rows: [
      ['device.disk.attach', 2, 'device disk', 'summary', 'VM에 블록 장치 핫 연결', 'Hot-attach block device to VM'],
      ['device.disk.detach', 2, 'device disk', 'summary', '블록 장치 핫 분리', 'Hot-detach block device'],
      ['device.nic.list', 0, 'nic list', 'summary', 'VM NIC 목록', 'List VM NICs'],
      ['device.nic.attach', 1, 'nic add', 'summary', 'NIC 핫 연결 (operator: 소유 VM)', 'Hot-attach NIC (operator: own VM)'],
      ['device.nic.detach', 1, 'nic remove', 'summary', 'NIC 핫 분리 (operator: 소유 VM)', 'Hot-detach NIC (operator: own VM)'],
    ]},
    { id: 'dpdk', ko: 'DPDK', en: 'DPDK', rows: [
      ['dpdk.status', 0, 'dpdk status', 'dpdk', 'OVS-DPDK 상태', 'OVS-DPDK status'],
      ['dpdk.bind', 2, 'dpdk bind', 'dpdk', 'NIC을 DPDK 드라이버에 바인드', 'Bind NIC to DPDK driver'],
      ['dpdk.unbind', 2, 'dpdk unbind', 'dpdk', 'NIC을 DPDK에서 언바인드', 'Unbind NIC from DPDK'],
      ['dpdk.list', 0, 'dpdk list', 'dpdk', 'DPDK 장치 목록', 'List DPDK devices'],
      ['dpdk.bridge.create', 2, 'dpdk bridge', 'dpdk', 'DPDK 브릿지 생성', 'Create DPDK bridge'],
      ['dpdk.bridge.delete', 2, 'dpdk bridge', 'dpdk', 'DPDK 브릿지 삭제', 'Delete DPDK bridge'],
      ['dpdk.hugepage.info', 0, 'dpdk hugepage', 'dpdk', 'Hugepage 할당 정보', 'Hugepage allocation info'],
    ]},
    { id: 'gpu', ko: 'GPU', en: 'GPU', rows: [
      ['gpu.metrics', 0, 'gpu metrics', 'gpu', 'GPU 사용률 메트릭', 'GPU utilization metrics'],
      ['gpu.list', 0, 'gpu list', 'gpu', 'GPU 목록 (lspci)', 'List GPUs (lspci)'],
    ]},
    { id: 'healing', ko: '자가치유', en: 'Self-Healing', rows: [
      ['healing.history', 0, 'healing history', 'selfhealing', '자가치유 액션 이력', 'Self-healing action history'],
      ['healing.pending', 0, '-', 'selfhealing', '대기 중 힐링 액션 (BUG-21)', 'Pending healing actions (BUG-21)'],
      ['healing.set_mode', 2, '-', 'selfhealing', 'dry_run↔active 모드 전환 (관리자)', 'Switch dry_run/active mode (admin)'],
    ]},
    { id: 'iscsi', ko: 'iSCSI', en: 'iSCSI', rows: [
      ['iscsi.target.create', 2, '-', 'iscsi', 'iSCSI 타겟 생성 (커널 LIO — 외부 데몬 불요)', 'Create iSCSI target (kernel LIO — no external daemon)'],
      ['iscsi.target.delete', 2, '-', 'iscsi', 'iSCSI 타겟 삭제 (멱등)', 'Delete iSCSI target (idempotent)'],
      ['iscsi.target.list', 0, '-', 'iscsi', 'iSCSI 타겟 목록 (정본 식별자는 iqn)', 'List iSCSI targets (iqn is the canonical id)'],
      ['iscsi.connect', 2, '-', 'iscsi', '원격 iSCSI 타겟에 연결', 'Connect to remote iSCSI target'],
      ['iscsi.disconnect', 2, '-', 'iscsi', 'iSCSI 세션 연결 해제', 'Disconnect iSCSI session'],
    ]},
    { id: 'jobs', ko: '비동기 작업', en: 'Async Jobs', rows: [
      ['jobs.list', 0, 'job list', 'activity-log', '비동기 작업 목록 (페이징)', 'List async jobs (paged)'],
      ['jobs.get', 0, '-', 'activity-log', 'ID로 작업 1건 조회', 'Get one job by id'],
      ['jobs.status', 0, '-', 'activity-log', 'jobs.get 별칭 (ADR-0012)', 'Alias of jobs.get (ADR-0012)'],
      ['jobs.cancel', 0, '-', 'activity-log', '작업 취소 (미등록→VIEWER)', 'Cancel a job (unlisted defaults VIEWER)'],
      ['jobs.persist.list', 0, '-', 'activity-log', '영속 작업 목록 (SQLite cloud_jobs.db)', 'List persisted jobs (SQLite cloud_jobs.db)'],
    ]},
    { id: 'monitor', ko: '모니터링', en: 'Monitoring', rows: [
      ['monitor.metrics', 0, 'monitor metrics', 'mon-overview', '단일 VM CPU/MEM 사용량', 'Single-VM CPU/MEM usage'],
      ['monitor.fleet', 0, 'monitor fleet', 'mon-overview', '전체 플릿 통계 대시보드', 'Global fleet stats dashboard'],
      ['monitor.processes', 0, 'monitor processes', 'mon-hosts', '호스트 상위 프로세스 (type/top)', 'Top host processes (type/top)'],
    ]},
    { id: 'node', ko: '노드', en: 'Node', rows: [
      ['node.drain', 2, 'node drain', '-', '노드 드레인 (신규 RPC 수신 중단)', 'Drain node (stop accepting new RPCs)'],
      ['node.resume', 0, 'node resume', '-', '드레인 후 노드 재개 (미등록→VIEWER)', 'Resume node after drain (unlisted defaults VIEWER)'],
    ]},
    { id: 'ovn', ko: 'OVN SDN', en: 'OVN SDN', rows: [
      ['ovn.switch.create', 2, 'ovn switch', 'ovn', 'OVN 논리 스위치 생성', 'Create OVN logical switch'],
      ['ovn.switch.delete', 2, 'ovn switch', 'ovn', 'OVN 논리 스위치 삭제', 'Delete OVN logical switch'],
      ['ovn.switch.list', 0, 'ovn switch', 'ovn', '논리 스위치 목록', 'List logical switches'],
      ['ovn.switch.detail', 0, 'ovn switch', 'ovn', '스위치 상세 (ports/ACL/DHCP)', 'Switch detail (ports/ACL/DHCP)'],
      ['ovn.port.add', 2, '-', 'ovn', '논리 스위치 포트 추가', 'Add logical switch port'],
      ['ovn.port.remove', 2, '-', 'ovn', '논리 스위치 포트 제거', 'Remove logical switch port'],
      ['ovn.acl.add', 2, 'ovn acl', 'ovn', '스위치에 ACL(방화벽) 규칙 추가', 'Add ACL (firewall) rule to switch'],
      ['ovn.acl.list', 0, 'ovn acl', 'ovn', 'ACL 규칙 목록 (스위치별 선택)', 'List ACL rules (optionally per switch)'],
      ['ovn.router.create', 2, 'ovn router', 'ovn', 'OVN 논리 라우터 생성', 'Create OVN logical router'],
      ['ovn.router.delete', 2, 'ovn router', 'ovn', '논리 라우터 삭제', 'Delete logical router'],
      ['ovn.router.list', 0, 'ovn router', 'ovn', '논리 라우터 목록', 'List logical routers'],
      ['ovn.router.detail', 0, 'ovn router', 'ovn', '라우터 상세', 'Router detail'],
      ['ovn.router.add_port', 2, '-', 'ovn', '라우터 포트를 스위치에 연결', 'Attach router port to switch'],
                                                                      
                                                                   
                                                           
      ['ovn.dhcp.enable', 2, 'ovn dhcp', 'ovn', '서브넷에 분산 DHCP 활성화 (subnet+gateway)', 'Enable distributed DHCP on a subnet (subnet + gateway)'],
      ['ovn.nat.add', 2, 'ovn nat', 'ovn', 'SNAT/DNAT 규칙 추가', 'Add SNAT/DNAT rule'],
      ['ovn.nat.list', 0, 'ovn nat', 'ovn', 'NAT 규칙 목록', 'List NAT rules'],
      ['ovn.tenant.create', 2, '-', 'ovn', '격리된 테넌트 네트워크 세트 프로비저닝', 'Provision isolated tenant network set'],
      ['ovn.status', 0, 'ovn status', 'ovn', 'OVN 컨트롤러/서비스 상태', 'OVN controller/service status'],
    ]},
                                                                
                                                     
                                                    
    { id: 'push', ko: '브라우저 푸시', en: 'Browser Push', rows: [
      ['push.vapid.get', 0, '-', '-', '구독에 필요한 VAPID 공개키 조회 (Prefs 모달 토글이 호출)', 'Get the VAPID public key required to subscribe (called by the Prefs modal toggle)'],
      ['push.subscribe', 0, '-', '-', '브라우저 구독 등록 — 소유자는 호출자 신원으로 고정, endpoint 기준 upsert', 'Register browser subscription — owner pinned to caller identity, upsert by endpoint'],
      ['push.unsubscribe', 0, '-', '-', '구독 해지 (비-ADMIN 은 본인 소유 행만)', 'Unsubscribe (non-ADMIN removes own rows only)'],
      ['push.mine', 0, '-', '-', '본인 구독만 조회 — 인자 없음(대상은 호출자 신원으로 고정), endpoint 요약 + SHA-256 지문', 'List own subscriptions only — no params (target pinned to caller), endpoint summary + SHA-256 digest'],
      ['push.list', 2, '-', '-', '전체 구독 목록 (브라우저 키 p256dh/auth 는 미포함)', 'List all subscriptions (browser keys p256dh/auth never included)'],
      ['push.vapid.rotate', 2, '-', '-', 'VAPID 키 재생성 + 전 구독 폐기 (브라우저 재구독 필요)', 'Regenerate VAPID key + revoke every subscription (browsers must re-subscribe)'],
      ['push.test', 2, '-', '-', '테스트 알림 큐잉 (운영 진단·E2E 앵커)', 'Queue a test notification (ops diagnosis / E2E anchor)'],
    ]},
    { id: 'security', ko: '보안 (HIDS/HIPS)', en: 'Security (HIDS/HIPS)', rows: [
      ['security.event.list', 0, 'security events', 'mon-security', 'HIDS/HIPS 보안 이벤트 목록', 'List HIDS/HIPS security events'],
      ['security.event.get', 0, 'security event', 'mon-security', '보안 이벤트 1건 조회', 'Show one security event'],
      ['security.action.pending', 0, 'security pending', 'mon-security', '대기 중 HIPS 액션 목록', 'List pending HIPS actions'],
      ['security.action.approve', 2, 'security approve', 'mon-security', '대기 HIPS 액션 승인', 'Approve pending HIPS action'],
      ['security.action.dismiss', 1, 'security dismiss', 'mon-security', '대기 HIPS 액션 기각', 'Dismiss pending HIPS action'],
      ['security.baseline.status', 0, 'security baseline-status', 'mon-security', 'HIDS 파일 베이스라인 상태', 'HIDS file-baseline status'],
      ['security.baseline.refresh', 2, 'security baseline-refresh', 'mon-security', 'HIDS 파일 베이스라인 갱신', 'Refresh HIDS file baseline'],
      ['security.config.get', 0, 'security status', 'mon-security', 'Security Guard 설정 조회', 'Get Security Guard config'],
      ['security.config.set', 2, 'security enable/disable', 'mon-security', 'Security Guard 활성/비활성 + 설정', 'Enable/disable + configure Security Guard'],
    ]},
    { id: 'security_group', ko: '보안 그룹', en: 'Security Groups', rows: [
      ['security_group.create', 2, 'security-group create', 'security-groups', '보안 그룹 생성', 'Create security group'],
      ['security_group.list', 0, 'security-group list', 'security-groups', '보안 그룹 목록', 'List security groups'],
      ['security_group.delete', 2, 'security-group delete', 'security-groups', '보안 그룹 삭제', 'Delete security group'],
      ['security_group.rule.add', 0, 'security-group rule', 'security-groups', '규칙 추가 (미등록→VIEWER)', 'Add rule (unlisted defaults VIEWER)'],
      ['security_group.rule.remove', 0, '-', 'security-groups', '규칙 제거 (미등록→VIEWER)', 'Remove rule (unlisted defaults VIEWER)'],
      ['security_group.detach', 2, 'security-group detach', 'security-groups', 'VM에서 보안 그룹 분리', 'Detach security group from VM'],
    ]},
    { id: 'sriov', ko: 'SR-IOV', en: 'SR-IOV', rows: [
      ['sriov.status', 0, 'sriov status', 'sriov', 'SR-IOV PF/VF 상태', 'SR-IOV PF/VF status'],
      ['sriov.enable', 2, 'sriov enable', 'sriov', 'PF에 VF 활성화', 'Enable VFs on a PF'],
      ['sriov.disable', 2, 'sriov disable', 'sriov', 'VF 비활성화', 'Disable VFs'],
      ['sriov.list', 0, 'sriov list', 'sriov', 'VF 목록', 'List VFs'],
      ['sriov.set', 2, 'sriov set', 'sriov', 'VF 속성 설정', 'Set VF attributes'],
      ['sriov.attach', 2, 'sriov attach', 'sriov', 'VF를 VM에 연결', 'Attach VF to VM'],
      ['sriov.detach', 2, 'sriov detach', 'sriov', 'VF를 VM에서 분리', 'Detach VF from VM'],
    ]},
    { id: 'template', ko: '템플릿', en: 'Templates', rows: [
      ['template.list', 0, 'template list', 'templates', 'VM 템플릿 목록', 'List VM templates'],
      ['template.get', 0, 'template get', 'templates', '템플릿 상세 조회', 'Get template detail'],
      ['template.create', 2, 'template create', 'templates', 'VM 템플릿 생성', 'Create VM template'],
      ['template.delete', 2, 'template delete', 'templates', 'VM 템플릿 삭제', 'Delete VM template'],
      ['template.history', 0, 'template history', 'templates', '템플릿 변경 이력', 'Template change history'],
    ]},
    { id: 'webhook', ko: '웹훅 DLQ', en: 'Webhook DLQ', rows: [
      ['webhook.dlq.list', 0, 'webhook dlq', '-', '웹훅 데드레터 큐 목록', 'List webhook dead-letter queue'],
      ['webhook.dlq.retry', 0, 'webhook dlq', '-', '웹훅 DLQ 재전송 (미등록→VIEWER)', 'Retry webhook DLQ (unlisted defaults VIEWER)'],
    ]},
                        
    { id: 'singletons', ko: '기타 (싱글톤)', en: 'Misc (Singletons)', rows: [
      ['audit.search', 0, 'audit search', 'mon-audit', '감사 로그 검색 (user/from/to/method)', 'Search audit logs (user/from/to/method)'],
      ['snapshot.schedule.status', 0, 'snapshot schedule-status', 'snapshots', '전역 스냅샷 스케줄 상태', 'Global snapshot-schedule status'],
      ['quota.get', 0, '-', '-', '리소스 쿼터 사용량 조회', 'Get resource quota usage'],
      ['prometheus.sd', 0, 'prometheus sd', '-', 'Prometheus HTTP 서비스 디스커버리 타겟', 'Prometheus HTTP service-discovery targets'],
      ['pool.conninfo', 0, '-', '-', 'libvirt 커넥션 풀 상태 (idle/total/max)', 'libvirt connection-pool status (idle/total/max)'],
      ['nfv.lb.create', 2, '-', '-', 'NFV 로드밸런서 생성 (VIP→백엔드)', 'Create NFV load-balancer (VIP to backends)'],
      ['iso.list', 0, 'iso list', 'console', '사용 가능 ISO 이미지 목록', 'List available ISO images'],
      ['health.deep', 0, '-', '-', '심층 헬스 (ZFS 풀 + nftables 규칙)', 'Deep health (ZFS pool + nftables rules)'],
      ['db.migration.status', 0, '-', '-', '내부 DB 스키마 버전/위치', 'Internal DB schema version/location'],
      ['capacity.forecast', 0, 'capacity forecast', '-', '리소스 소진 용량 예측', 'Resource-exhaustion capacity forecast'],
      ['anomaly.reset_baseline', 2, '-', 'selfhealing', 'Z-Score 이상탐지 베이스라인 리셋 (관리자)', 'Reset Z-Score anomaly baseline (admin)'],
    ]},
  ];

                                                                 
                                                           
  function rbacBadge(n) { return n === 2 ? HN.statusPill('idle', 'ADMIN') : (n === 1 ? HN.statusPill('idle', 'OPERATOR') : HN.statusPill('idle', 'VIEWER')); }

                                                         
  var jump = el('div', { class: 'help-jump' }, NS.map(function(ns) {
    return el('button', { type: 'button', class: 'help-chip' + (ns.isNew ? ' help-chip-new' : ''), onclick: 'helpJump("' + ns.id + '")' },
      ns.id + ' ' + ns.rows.length);
  }));

  var searchBar = el('div', { class: 'mb-16' },
    el('input', { 'aria-label': t('search'), id: 'help-search', class: 'sb-search', placeholder: t('search'), oninput: 'filterHelp()', style: 'max-width:600px;font-size:15px;padding:10px 14px;border-radius:8px' }));

                                                                     
  var sections = NS.map(function(ns) {
    var collapsed = !ns.isNew;
    var head = el('h4', { class: 'color-accent help-ns-h', onclick: "this.closest('.help-ns').classList.toggle('ns-collapsed')" },
      el('span', { class: 'help-ns-id' }, ns.id),
      el('span', { class: 'help-ns-name' }, _L(ns.ko, ns.en)),
      el('span', { class: 'help-ns-count' }, String(ns.rows.length)),
      ns.isNew ? HN.statusPill('idle', '2.0') : null);
    var tbody = el('tbody', null, ns.rows.map(function(r) {
      return el('tr', { 'data-search': (r[0] + ' ' + r[2] + ' ' + r[4] + ' ' + r[5]).toLowerCase() },
        el('td', { class: 'help-m' }, r[0]),
        el('td', null, rbacBadge(r[1])),
        el('td', { class: 'help-cli' }, r[2]),
        el('td', null, r[3]),
        el('td', { class: 'color-muted' }, _L(r[4], r[5])));
    }));
    var table = el('table', { class: 'help-tbl' },
      el('thead', null, el('tr', null,
        el('th', null, 'RPC'), el('th', null, 'RBAC'), el('th', null, 'CLI'),
        el('th', null, 'Web UI'), el('th', null, _L('설명', 'Description')))),
      tbody);
    return el('div', { class: 'hc help-ns' + (collapsed ? ' ns-collapsed' : ''), id: 'help-ns-' + ns.id, 'data-default-collapsed': collapsed ? '1' : '0' },
      head,
      ns.rpcOnly ? el('div', { class: 'help-rpconly' }, _L('RPC 전용 — REST·CLI·UI 경로 없음 (POST /api/v1/rpc)', 'RPC-only — no REST/CLI/UI route (POST /api/v1/rpc)')) : null,
      table);
  });
  var content = el('div', { id: 'help-content' }, sections);

  clearEl(b);
  b.appendChild(frag(HN.pagehead({ title: _L('도움말 & 참조', 'Help & Reference') }), banner, stats, jump, searchBar, content));
}
window.renderHelp = renderHelp;

                                                    
function helpJump(id) {
  var e = document.getElementById('help-ns-' + id);
  if (!e) return;
  e.classList.remove('ns-collapsed');
  if (typeof e.scrollIntoView === 'function') e.scrollIntoView({ behavior: 'smooth', block: 'start' });
}
window.helpJump = helpJump;

                                                          
function filterHelp() {
  var input = document.getElementById('help-search');
  var q = input ? input.value.trim().toLowerCase() : '';
  document.querySelectorAll('#help-content .help-ns').forEach(function(sec) {
    if (!q) {
      sec.style.display = '';
      if (sec.dataset.defaultCollapsed === '1') sec.classList.add('ns-collapsed');
      else sec.classList.remove('ns-collapsed');
      sec.querySelectorAll('tr[data-search]').forEach(function(r) { r.style.display = ''; });
      return;
    }
    var any = false;
    sec.querySelectorAll('tr[data-search]').forEach(function(r) {
      var hit = r.dataset.search.indexOf(q) !== -1;
      r.style.display = hit ? '' : 'none';
      if (hit) any = true;
    });
    if (any) { sec.style.display = ''; sec.classList.remove('ns-collapsed'); }
    else { sec.style.display = 'none'; }
  });
}
window.filterHelp = filterHelp;


                         
                                                   
                                                  
                                                                    
                                                        
                                                                        
function renderSwaggerApi(b) {
  var el = PCV.uxlib.el, frag = PCV.uxlib.frag, clearEl = PCV.uxlib.clearEl;
  var mc = function(m) { return m === 'GET' ? '#61affe' : m === 'POST' ? '#49cc90' : m === 'DELETE' ? '#f93e3e' : m === 'PUT' ? '#fca130' : '#00f0ff'; };
  var METRICS = (typeof PCV !== 'undefined' && PCV.config) ? (PCV.config.METRICS_COUNT || 155) : 155;

                                                               
                                             
                                                              
                                                                        
                                                                   
                                                         
                                                            
                                                          
                           
  function rpc(method, ko, en, params) {
    return { m: 'POST', p: '/rpc', d: _L(ko, en) + ' (' + method + ')',
      body: '{"jsonrpc":"2.0","method":"' + method + '","params":' + (params || '{}') + ',"id":"1"}' };
  }

  var groups = [
                                                                    
    { id: 'health', name: _L('헬스 & 시스템', 'Health & System'), open: true, endpoints: [
      { m: 'GET', p: '/health', d: _L('라이브니스 프로브', 'Liveness probe'), auth: false },
      { m: 'GET', p: '/health/recent-errors', d: _L('인메모리 에러 링', 'In-memory error ring'), auth: false },
      { m: 'GET', p: '/health/deep', d: _L('심층 헬스 (ZFS 풀 + nftables)', 'Deep health (ZFS pool + nftables)') },
      { m: 'GET', p: '/version', d: _L('데몬 버전', 'Daemon version'), auth: false },
      { m: 'GET', p: '/update-check', d: _L('업데이트 확인', 'Update check'), auth: false },
      { m: 'GET', p: '/metrics', d: _L('Prometheus 메트릭 (' + METRICS + '개)', 'Prometheus metrics (' + METRICS + ')') },
    ]},
                                                      
    { id: 'auth', name: _L('인증 & RBAC', 'Auth & RBAC'), endpoints: [
      { m: 'POST', p: '/auth/token', d: _L('JWT 로그인', 'JWT login'), auth: false, body: '{"username":"admin","password":"configured-admin-password"}' },
      { m: 'POST', p: '/auth/refresh', d: _L('액세스 토큰 갱신', 'Refresh access token'), auth: false, body: '{"refresh_token":"..."}' },
      { m: 'POST', p: '/auth/logout', d: _L('로그아웃 (세션 폐기)', 'Logout (revoke session)') },
      { m: 'POST', p: '/auth/password', d: _L('비밀번호 변경', 'Change own password'), body: '{"old_password":"..","new_password":".."}' },
      { m: 'POST', p: '/auth/register', d: _L('자가 등록', 'Self-registration'), auth: false, body: '{"username":"..","password":".."}' },
      { m: 'GET', p: '/auth/whoami', d: _L('현재 사용자/역할/테넌트', 'Current user/role/tenant') },
      { m: 'GET', p: '/auth/users', d: _L('사용자 목록', 'User list') },
      { m: 'POST', p: '/auth/users', d: _L('사용자 생성', 'Create user'), body: '{"username":"bob","password":"..","role":"operator"}' },
      { m: 'DELETE', p: '/auth/users', d: _L('사용자 삭제', 'Delete user'), body: '{"username":"bob"}' },
      { m: 'POST', p: '/auth/role', d: _L('역할 설정', 'Set role'), body: '{"username":"bob","role":"admin"}' },
      { m: 'POST', p: '/auth/sessions/revoke', d: _L('세션 1건 폐기', 'Revoke one session'), body: '{"jti":"..."}' },
      { m: 'POST', p: '/auth/user-sessions/revoke', d: _L('사용자 전체 세션 폐기', 'Revoke all user sessions'), body: '{"username":"bob"}' },
      { m: 'GET', p: '/auth/apikeys', d: _L('API 키 목록', 'API key list') },
      { m: 'POST', p: '/auth/apikeys', d: _L('API 키 생성', 'Create API key'), body: '{"name":"ci","role":"viewer","expires_at":1750000000}' },
      { m: 'POST', p: '/auth/apikeys/{client_name}/revoke', d: _L('API 키 폐기', 'Revoke API key') },
      { m: 'POST', p: '/auth/totp/verify', d: _L('TOTP 코드 확인', 'Verify TOTP code'), body: '{"pending_token":"...","code":"123456"}' },
      { m: 'POST', p: '/auth/totp/enroll', d: _L('TOTP 등록 시작', 'Start TOTP enrollment'), body: '{"pending_token":"..."}' },
      { m: 'GET', p: '/auth/totp/status', d: _L('본인 TOTP 상태', 'Own TOTP status') },
      { m: 'POST', p: '/auth/totp/disable', d: _L('본인 TOTP 해제', 'Disable own TOTP'), body: '{"code":"123456"}' },
      { m: 'POST', p: '/auth/totp/reset', d: _L('사용자 TOTP 초기화', 'Reset user TOTP'), body: '{"username":"bob"}' },
      { m: 'POST', p: '/auth/totp/recovery/regenerate', d: _L('복구코드 재발급', 'Regenerate recovery codes'), body: '{"code":"123456"}' },
    ]},
                                           
    { id: 'vms', name: _L('가상 머신', 'Virtual Machines'), endpoints: [
      { m: 'GET', p: '/vms', d: _L('VM 목록', 'VM list') },
      { m: 'POST', p: '/vms', d: _L('VM 생성', 'Create VM'), body: '{"name":"web","vcpu":2,"memory_mb":2048}' },
      { m: 'GET', p: '/vms/{name}', d: _L('VM 메트릭/상세', 'VM metrics/detail') },
      { m: 'DELETE', p: '/vms/{name}', d: _L('VM 삭제', 'Delete VM') },
      { m: 'POST', p: '/vms/{name}/start', d: _L('시작', 'Start') },
      { m: 'POST', p: '/vms/{name}/stop', d: _L('중지', 'Stop') },
      { m: 'POST', p: '/vms/{name}/suspend', d: _L('일시정지', 'Pause/suspend') },
      { m: 'POST', p: '/vms/{name}/resume', d: _L('재개', 'Resume') },
      { m: 'GET', p: '/vms/{name}/metrics', d: _L('VM 메트릭', 'VM metrics') },
      { m: 'GET', p: '/vms/{name}/snapshot', d: _L('스냅샷 목록', 'Snapshot list') },
      { m: 'POST', p: '/vms/{name}/snapshot/create', d: _L('스냅샷 생성', 'Create snapshot'), body: '{"snapshot_name":"pre-up"}' },
      { m: 'POST', p: '/vms/{name}/snapshot/rollback', d: _L('스냅샷 롤백', 'Rollback snapshot'), body: '{"snapshot_name":"pre-up"}' },
      { m: 'POST', p: '/vms/{name}/snapshot/delete_all', d: _L('스냅샷 일괄 삭제', 'Bulk-delete snapshots'), body: '{"prefix":"auto-","keep_recent":3}' },
      { m: 'POST', p: '/vms/{name}/snapshot/schedule', d: _L('스냅샷 스케줄 설정', 'Set snapshot schedule'), body: '{"cron":"0 2 * * *","keep":7}' },
      { m: 'GET', p: '/vms/{name}/snapshot/schedule', d: _L('스냅샷 스케줄 조회', 'List snapshot schedule') },
      { m: 'DELETE', p: '/vms/{name}/snapshot/schedule', d: _L('스냅샷 스케줄 삭제', 'Delete snapshot schedule') },
      { m: 'DELETE', p: '/vms/{name}/snapshot/{snap}', d: _L('스냅샷 삭제', 'Delete snapshot') },
      { m: 'GET', p: '/vms/{name}/nics', d: _L('NIC 목록', 'NIC list') },
      { m: 'POST', p: '/vms/{name}/nics', d: _L('NIC 추가', 'Attach NIC'), body: '{"bridge":"br0","model":"virtio"}' },
      { m: 'DELETE', p: '/vms/{name}/nics/{mac}', d: _L('NIC 제거', 'Detach NIC') },
      { m: 'GET', p: '/vms/{name}/usb', d: _L('USB 목록', 'USB list') },
      { m: 'POST', p: '/vms/{name}/usb', d: _L('USB 연결', 'Attach USB'), body: '{"vendor_id":"1234","product_id":"5678"}' },
      { m: 'DELETE', p: '/vms/{name}/usb', d: _L('USB 분리', 'Detach USB'), body: '{"vendor_id":"1234","product_id":"5678"}' },
      { m: 'PUT', p: '/vms/{name}/vcpu', d: _L('vCPU 설정', 'Set vCPU'), body: '{"vcpu_count":4}' },
      { m: 'PUT', p: '/vms/{name}/memory', d: _L('메모리 설정', 'Set memory'), body: '{"memory_mb":4096}' },
      { m: 'GET', p: '/vms/{name}/delete-status', d: _L('삭제 진행률', 'Delete progress') },
      { m: 'PUT', p: '/vms/{name}/rename', d: _L('이름 변경', 'Rename VM'), body: '{"new_name":"web2"}' },
      { m: 'POST', p: '/vms/{name}/iso', d: _L('ISO 마운트', 'Mount ISO'), body: '{"iso_path":"/isos/x.iso"}' },
      { m: 'DELETE', p: '/vms/{name}/iso', d: _L('ISO 꺼내기', 'Eject ISO') },
      { m: 'POST', p: '/vms/{name}/clone', d: _L('VM 복제', 'Clone VM'), body: '{"dest":"web-clone","mode":"cow"}' },
      { m: 'PUT', p: '/vms/{name}/disk', d: _L('디스크 리사이즈', 'Resize disk'), body: '{"size_gb":50}' },
      { m: 'PUT', p: '/vms/{name}/cpu-pin', d: _L('vCPU 핀', 'Pin vCPU'), body: '{"vcpu":0,"cpuset":"2-3"}' },
      { m: 'PUT', p: '/vms/{name}/bandwidth', d: _L('대역폭 설정', 'Set bandwidth'), body: '{"inbound_kbps":1000,"outbound_kbps":1000}' },
      { m: 'POST', p: '/vms/{name}/export', d: _L('OVA 익스포트', 'Export OVA') },
      { m: 'POST', p: '/vms/import', d: _L('OVA 임포트', 'Import OVA'), body: '{"ova_path":"/x.ova","name":"web"}' },
      { m: 'POST', p: '/vms/{name}/import-ec2', d: _L('EC2 임포트', 'Import from EC2'), body: '{"ami_id":"ami-..."}' },
      { m: 'POST', p: '/vms/{name}/export-ec2', d: _L('EC2 익스포트', 'Export to EC2') },
      { m: 'GET', p: '/vms/{name}/import-status', d: _L('임포트 상태', 'Import status') },
      { m: 'GET', p: '/vms/{name}/export-status', d: _L('익스포트 상태', 'Export status') },
      { m: 'GET', p: '/vms/{name}/memory-stats', d: _L('메모리 통계', 'Memory stats') },
      { m: 'GET', p: '/vms/{name}/snapshot-schedule', d: _L('스냅샷 스케줄 상태', 'Snapshot schedule status') },
      { m: 'POST', p: '/vms/{name}/guest-ping', d: _L('게스트 에이전트 핑', 'Guest-agent ping') },
      { m: 'GET', p: '/vms/{name}/guest-agent', d: _L('게스트 에이전트 상태', 'Guest-agent status') },
      { m: 'POST', p: '/vms/{name}/guest-agent-channel', d: _L('게스트 채널 추가', 'Add guest-agent channel') },
      { m: 'POST', p: '/vms/{name}/guest-shutdown', d: _L('게스트 종료', 'Guest shutdown') },
      { m: 'GET', p: '/vms/{name}/disk-usage', d: _L('게스트 디스크 사용량', 'Guest disk usage') },
      { m: 'POST', p: '/vms/{name}/disk-resize', d: _L('라이브 디스크 리사이즈', 'Live disk resize'), body: '{"size_gb":50}' },
      { m: 'POST', p: '/vms/{name}/guest-exec', d: _L('게스트 명령 실행', 'Guest exec'), body: '{"cmd":"uptime"}' },
    ]},
                                                         
    { id: 'containers', name: _L('컨테이너 (LXC)', 'Containers (LXC)'), endpoints: [
      { m: 'GET', p: '/containers', d: _L('컨테이너 목록', 'Container list') },
      { m: 'POST', p: '/containers', d: _L('컨테이너 생성', 'Create container'), body: '{"name":"c1","template":"debian-12"}' },
      { m: 'DELETE', p: '/containers/{name}', d: _L('컨테이너 삭제', 'Destroy container') },
      { m: 'POST', p: '/containers/{name}/start', d: _L('시작', 'Start') },
      { m: 'POST', p: '/containers/{name}/stop', d: _L('중지', 'Stop') },
      { m: 'GET', p: '/containers/{name}/metrics', d: _L('메트릭', 'Metrics') },
      { m: 'POST', p: '/containers/{name}/exec', d: _L('명령 실행', 'Exec command'), body: '{"command":"ls"}' },
      { m: 'GET', p: '/containers/{name}/snapshots', d: _L('스냅샷 목록', 'Snapshot list') },
      { m: 'POST', p: '/containers/{name}/snapshots', d: _L('스냅샷 생성', 'Create snapshot'), body: '{"snap_name":"s1"}' },
      { m: 'POST', p: '/containers/{name}/snapshots/rollback', d: _L('스냅샷 롤백', 'Rollback snapshot'), body: '{"snap_name":"s1"}' },
      { m: 'DELETE', p: '/containers/{name}/snapshots/{snap}', d: _L('스냅샷 삭제', 'Delete snapshot') },
      { m: 'PUT', p: '/containers/{name}/limits', d: _L('리소스 제한', 'Set limits'), body: '{"memory_mb":512,"cpu_quota":50000}' },
      { m: 'GET', p: '/containers/{name}/nics', d: _L('NIC 목록', 'NIC list') },
      { m: 'POST', p: '/containers/{name}/nics', d: _L('NIC 연결', 'Attach NIC'), body: '{"bridge":"br0"}' },
      { m: 'DELETE', p: '/containers/{name}/nics/{nic}', d: _L('NIC 분리', 'Detach NIC') },
      { m: 'PUT', p: '/containers/{name}/bandwidth', d: _L('대역폭 설정', 'Set bandwidth'), body: '{"inbound":1000,"outbound":1000}' },
      { m: 'PUT', p: '/containers/{name}/health', d: _L('헬스체크 설정', 'Set health-check'), body: '{"type":"tcp","target":"80"}' },
      { m: 'GET', p: '/containers/{name}/health', d: _L('헬스체크 조회', 'Get health-check') },
      { m: 'DELETE', p: '/containers/{name}/health', d: _L('헬스체크 삭제', 'Delete health-check') },
      { m: 'GET', p: '/containers/{name}/volumes', d: _L('볼륨 목록', 'Volume list') },
      { m: 'POST', p: '/containers/{name}/volumes', d: _L('볼륨 연결', 'Attach volume'), body: '{"host_path":"/data","mount":"/mnt"}' },
      { m: 'DELETE', p: '/containers/{name}/volumes/{vol}', d: _L('볼륨 분리', 'Detach volume') },
      { m: 'GET', p: '/containers/{name}/env', d: _L('환경변수 목록', 'Env list') },
      { m: 'PUT', p: '/containers/{name}/env', d: _L('환경변수 설정', 'Set env'), body: '{"key":"FOO","value":"bar"}' },
      { m: 'DELETE', p: '/containers/{name}/env', d: _L('환경변수 삭제', 'Delete env'), body: '{"key":"FOO"}' },
      { m: 'POST', p: '/containers/{name}/clone', d: _L('컨테이너 복제', 'Clone container'), body: '{"dest":"c2"}' },
      { m: 'GET', p: '/containers/{name}/memory-stats', d: _L('메모리 통계', 'Memory stats') },
    ]},
                                            
    { id: 'networks', name: _L('네트워크', 'Networks'), endpoints: [
      { m: 'GET', p: '/networks', d: _L('네트워크 목록', 'Network list') },
      { m: 'POST', p: '/networks', d: _L('네트워크 생성', 'Create network'), body: '{"name":"br1","mode":"nat"}' },
      { m: 'GET', p: '/networks/{br}', d: _L('네트워크 상세', 'Network info') },
      { m: 'DELETE', p: '/networks/{br}', d: _L('네트워크 삭제', 'Delete network') },
      { m: 'POST', p: '/networks/{br}/mode', d: _L('모드 변경', 'Set mode'), body: '{"mode":"routed"}' },
      { m: 'PUT', p: '/networks/{iface}/qos', d: _L('인터페이스 QoS 설정', 'Set interface QoS'), body: '{"rate":"100mbit"}' },
      { m: 'GET', p: '/networks/{iface}/qos', d: _L('인터페이스 QoS 조회', 'Get interface QoS') },
      { m: 'DELETE', p: '/networks/{iface}/qos', d: _L('인터페이스 QoS 제거', 'Remove interface QoS') },
    ]},
                                                                             
    { id: 'vpcs', name: 'Local VPC', isNew: true, endpoints: [
      { m: 'GET', p: '/vpcs', d: _L('tenant VPC 목록', 'Tenant VPC list') },
      { m: 'POST', p: '/vpcs', d: _L('VPC와 첫 subnet 생성', 'Create VPC and initial subnet'), body: '{"tenant":"acme","name":"prod","backend":"ovn","egress_mode":"nat","subnet_name":"web","subnet_cidr":"10.60.10.0/24","subnet_mtu":1500}' },
      { m: 'GET', p: '/vpcs/backends', d: _L('backend 준비 상태와 주소 기반 용량', 'Backend readiness and address capacity') },
      { m: 'GET', p: '/vpcs/status', d: _L('reconcile 상태', 'Reconcile status') },
      { m: 'POST', p: '/vpcs/reconcile', d: _L('전체 desired state 수렴', 'Reconcile all desired state') },
      { m: 'GET', p: '/vpcs/{vpc_id}', d: _L('VPC aggregate 상세', 'VPC aggregate detail') },
      { m: 'DELETE', p: '/vpcs/{vpc_id}', d: _L('VPC 삭제', 'Delete VPC'), body: '{"tenant":"acme"}' },
      { m: 'POST', p: '/vpcs/{vpc_id}/egress', d: _L('egress 변경', 'Change egress'), body: '{"tenant":"acme","egress_mode":"isolated","expected_revision":3}' },
      { m: 'GET', p: '/vpcs/{vpc_id}/subnets', d: _L('subnet 목록', 'Subnet list') },
      { m: 'POST', p: '/vpcs/{vpc_id}/subnets', d: _L('subnet 생성', 'Create subnet'), body: '{"tenant":"acme","name":"web","cidr":"10.60.10.0/24","expected_revision":1}' },
      { m: 'DELETE', p: '/vpc-subnets/{subnet_id}', d: _L('subnet 삭제', 'Delete subnet'), body: '{"tenant":"acme"}' },
      { m: 'GET', p: '/vpcs/{vpc_id}/attachments', d: _L('VM 연결 목록', 'VM attachment list') },
      { m: 'POST', p: '/vpc-attachments', d: _L('정지 VM 연결', 'Attach stopped VM'), body: '{"tenant":"acme","subnet_id":"<uuid>","vm":"web-prod"}' },
      { m: 'DELETE', p: '/vpc-attachments/{attachment_id}', d: _L('VM 연결 해제', 'Detach VM'), body: '{"tenant":"acme"}' },
      { m: 'GET', p: '/vpcs/{vpc_id}/services', d: _L('게시 서비스 목록', 'Published service list') },
      { m: 'POST', p: '/vpc-services', d: 'Service Publish', body: '{"tenant":"acme","attachment_id":"<uuid>","protocol":"tcp","listen_address":"0.0.0.0","listen_port":8443,"target_port":443,"allowed_sources":["192.0.2.0/24"]}' },
      { m: 'DELETE', p: '/vpc-services/{publish_id}', d: _L('서비스 게시 해제', 'Unpublish service'), body: '{"tenant":"acme"}' },
    ]},
                                      
    { id: 'ovn', name: _L('OVN SDN', 'OVN SDN'), endpoints: [
      { m: 'GET', p: '/ovn/status', d: _L('OVN 상태', 'OVN status') },
      { m: 'GET', p: '/ovn/switches', d: _L('논리 스위치 목록', 'Logical switch list') },
      { m: 'POST', p: '/ovn/switches', d: _L('논리 스위치 생성', 'Create switch'), body: '{"name":"ls0"}' },
      { m: 'GET', p: '/ovn/routers', d: _L('논리 라우터 목록', 'Logical router list') },
      { m: 'POST', p: '/ovn/routers', d: _L('논리 라우터 생성', 'Create router'), body: '{"name":"lr0"}' },
      { m: 'GET', p: '/ovn/acl', d: _L('ACL 목록', 'ACL list'), body: '{"switch":"ls0"}' },
      { m: 'POST', p: '/ovn/acl', d: _L('ACL 추가', 'Add ACL'), body: '{"switch":"ls0","match":"..","action":"drop"}' },
      { m: 'GET', p: '/ovn/nat', d: _L('NAT 목록', 'NAT list'), body: '{"router":"lr0"}' },
      { m: 'POST', p: '/ovn/nat', d: _L('NAT 추가', 'Add NAT'), body: '{"router":"lr0","type":"snat"}' },
    ]},
                                          
    { id: 'storage', name: _L('스토리지 (ZFS)', 'Storage (ZFS)'), endpoints: [
      { m: 'GET', p: '/storage/pools', d: _L('ZFS 풀 목록', 'ZFS pool list') },
      { m: 'POST', p: '/storage/pools', d: _L('풀 생성', 'Create pool'), body: '{"name":"tank","devices":["sdb"]}' },
      { m: 'DELETE', p: '/storage/pools', d: _L('풀 파괴', 'Destroy pool'), body: '{"name":"tank"}' },
      { m: 'POST', p: '/storage/pools/scrub', d: _L('스크럽 시작', 'Start scrub'), body: '{"name":"tank"}' },
      { m: 'GET', p: '/storage/zvols', d: _L('zvol 목록', 'Zvol list') },
      { m: 'POST', p: '/storage/zvols', d: _L('zvol 생성', 'Create zvol'), body: '{"name":"tank/v1","size_gb":10}' },
      { m: 'DELETE', p: '/storage/zvols', d: _L('zvol 삭제', 'Delete zvol'), body: '{"name":"tank/v1"}' },
    ]},
                                      
    { id: 'iscsi', name: _L('iSCSI', 'iSCSI'), endpoints: [
      { m: 'GET', p: '/iscsi/targets', d: _L('타겟 목록', 'Target list') },
      { m: 'POST', p: '/iscsi/targets', d: _L('타겟 생성', 'Create target'), body: '{"vm_name":"web01","zvol_path":"/dev/zvol/tank/web01"}' },
      { m: 'DELETE', p: '/iscsi/targets', d: _L('타겟 삭제', 'Delete target'), body: '{"vm_name":"web01"}' },
      { m: 'POST', p: '/iscsi/connect', d: _L('타겟 연결', 'Connect target'), body: '{"target_ip":"192.0.2.50","vm_name":"web01"}' },
      { m: 'POST', p: '/iscsi/disconnect', d: _L('세션 해제', 'Disconnect session'), body: '{"target_ip":"192.0.2.50","vm_name":"web01"}' },
    ]},
                                               
    { id: 'overlay', name: _L('오버레이 메시', 'Overlay Mesh'), endpoints: [
      { m: 'GET', p: '/overlay', d: _L('오버레이 목록', 'Overlay list') },
    ]},
                                                
    { id: 'cloud', name: _L('클라우드 마이그레이션', 'Cloud Migration'), endpoints: [
      { m: 'GET', p: '/cloud/jobs', d: _L('마이그레이션 작업 목록', 'Migration job list') },
      { m: 'POST', p: '/cloud/cancel', d: _L('마이그레이션 취소', 'Cancel migration'), body: '{"job_id":"..."}' },
    ]},
                                                                                                      
    { id: 'monitoring', name: _L('모니터링 & 알림', 'Monitoring & Alerts'), endpoints: [
      { m: 'GET', p: '/monitor/metrics', d: _L('단일 VM 메트릭', 'Single-VM metrics') },
      { m: 'GET', p: '/monitor/fleet', d: _L('플릿 통계', 'Fleet stats') },
      { m: 'GET', p: '/processes', d: _L('호스트 프로세스', 'Host processes') },
      { m: 'GET', p: '/alerts', d: _L('알림 이력', 'Alert history') },
      { m: 'GET', p: '/alerts/config', d: _L('알림 설정 조회', 'Get alert config') },
      { m: 'PUT', p: '/alerts/config', d: _L('알림 설정 변경', 'Set alert config'), body: '{"cpu_threshold":90}' },
      { m: 'GET', p: '/alerts/actions', d: _L('알림 액션 목록', 'Alert actions') },
      { m: 'POST', p: '/alerts/silence', d: _L('알림 무음화', 'Silence alert'), body: '{"alert":"cpu","duration":3600}' },
      { m: 'GET', p: '/alerts/silences', d: _L('무음 목록', 'Silence list') },
      { m: 'GET', p: '/alerts/dlq', d: _L('알림 DLQ', 'Alert DLQ') },
      { m: 'POST', p: '/alerts/{id}/ack', d: _L('개별 알림 확인 처리', 'Acknowledge a single alert') },
      { m: 'GET', p: '/audit/search', d: _L('감사 로그 검색', 'Audit log search'), body: '{"user":"..","method":"..","limit":50}' },
      { m: 'GET', p: '/agent/config', d: _L('AI 에이전트 설정 조회', 'Get AI agent config') },
      { m: 'PUT', p: '/agent/config', d: _L('AI 에이전트 설정 변경', 'Set AI agent config'), body: '{"provider":"openai","enabled":true}' },
      { m: 'GET', p: '/agent/history', d: _L('AI 결정 이력', 'AI decision history') },
      { m: 'GET', p: '/prometheus/sd', d: _L('Prometheus SD 타겟', 'Prometheus SD targets') },
      { m: 'GET', p: '/pool/conninfo', d: _L('libvirt 커넥션 풀', 'libvirt connection pool') },
      { m: 'GET', p: '/db/migration', d: _L('DB 마이그레이션 상태', 'DB migration status') },
    ]},
                                                                
                                                                     
                                                                             
                                                                   
                                                           
                                                                      
    { id: 'push', name: _L('브라우저 푸시', 'Browser Push'), endpoints: [
      { m: 'GET', p: '/push/vapid', d: _L('VAPID 공개키 조회 (VIEWER)', 'Get VAPID public key (VIEWER)') },
      { m: 'POST', p: '/push/subscribe', d: _L('브라우저 구독 등록 (VIEWER, 본인 귀속)', 'Register browser subscription (VIEWER, self-scoped)'), body: '{"endpoint":"https://fcm.googleapis.com/fcm/send/cXY..","keys":{"p256dh":"BN1n..","auth":"k9Xz.."}}' },
      { m: 'POST', p: '/push/unsubscribe', d: _L('구독 해지 (VIEWER, 본인 귀속)', 'Unsubscribe (VIEWER, self-scoped)'), body: '{"endpoint":"https://fcm.googleapis.com/fcm/send/cXY.."}' },
      { m: 'GET', p: '/push/mine', d: _L('본인 구독 조회 (VIEWER, 인자 없음 — 요약 + SHA-256 지문)', 'List own subscriptions (VIEWER, no params — summary + SHA-256 digest)') },
      { m: 'GET', p: '/push/subscriptions', d: _L('전체 구독 목록 (ADMIN)', 'List all subscriptions (ADMIN)') },
      { m: 'POST', p: '/push/vapid/rotate', d: _L('VAPID 회전 + 전 구독 폐기 (ADMIN)', 'Rotate VAPID + revoke all subscriptions (ADMIN)') },
      { m: 'POST', p: '/push/test', d: _L('테스트 알림 큐잉 (ADMIN, username 생략 시 전체)', 'Queue test notification (ADMIN; omit username for all)'), body: '{"username":"admin"}' },
    ]},
                                    
    { id: 'jobs', name: _L('비동기 작업', 'Async Jobs'), endpoints: [
      { m: 'GET', p: '/jobs', d: _L('작업 목록', 'Job list') },
      { m: 'POST', p: '/jobs/{id}/cancel', d: _L('작업 취소', 'Cancel job') },
      { m: 'GET', p: '/jobs/persistent', d: _L('영속 작업 목록', 'Persistent jobs') },
      { m: 'GET', p: '/jobs/{id}', d: _L('작업 1건 조회', 'Get one job') },
    ]},
                                              
    { id: 'webhook', name: _L('웹훅 DLQ', 'Webhook DLQ'), endpoints: [
      { m: 'GET', p: '/webhook/dlq', d: _L('웹훅 DLQ 목록', 'Webhook DLQ list') },
      { m: 'POST', p: '/webhook/dlq', d: _L('웹훅 DLQ 재전송', 'Retry webhook DLQ') },
    ]},
                                    
    { id: 'dpdk', name: _L('DPDK', 'DPDK'), endpoints: [
      { m: 'GET', p: '/dpdk/status', d: _L('DPDK 상태', 'DPDK status') },
      { m: 'POST', p: '/dpdk/bind', d: _L('NIC 바인드', 'Bind NIC'), body: '{"pci_addr":"0000:01:00.0","driver":"vfio-pci"}' },
      { m: 'POST', p: '/dpdk/unbind', d: _L('NIC 언바인드', 'Unbind NIC'), body: '{"pci_addr":"0000:01:00.0"}' },
      { m: 'GET', p: '/dpdk/list', d: _L('DPDK 장치 목록', 'DPDK device list') },
      { m: 'GET', p: '/dpdk/hugepage', d: _L('Hugepage 정보', 'Hugepage info') },
      { m: 'POST', p: '/dpdk/bridge/create', d: _L('DPDK 브릿지 생성', 'Create DPDK bridge'), body: '{"name":"dpdk-br0"}' },
      { m: 'POST', p: '/dpdk/bridge/delete', d: _L('DPDK 브릿지 삭제', 'Delete DPDK bridge'), body: '{"name":"dpdk-br0"}' },
    ]},
                                       
    { id: 'sriov', name: _L('SR-IOV', 'SR-IOV'), endpoints: [
      { m: 'GET', p: '/sriov/status', d: _L('SR-IOV 상태', 'SR-IOV status') },
      { m: 'POST', p: '/sriov/enable', d: _L('VF 활성화', 'Enable VFs'), body: '{"pf":"eth0","num_vfs":8}' },
      { m: 'POST', p: '/sriov/disable', d: _L('VF 비활성화', 'Disable VFs'), body: '{"pf":"eth0"}' },
      { m: 'GET', p: '/sriov/list', d: _L('VF 목록', 'VF list') },
      { m: 'POST', p: '/sriov/set', d: _L('VF 속성 설정', 'Set VF attributes'), body: '{"pf":"eno2","vf_index":0,"mac":"52:54:00:12:34:56","vlan":100,"spoofchk":1}' },
      { m: 'POST', p: '/sriov/attach', d: _L('VF→VM 연결', 'Attach VF to VM'), body: '{"vm_name":"web-prod","pf":"eno2","vf_index":0}' },
      { m: 'POST', p: '/sriov/detach', d: _L('VF→VM 분리', 'Detach VF from VM'), body: '{"vm_name":"web-prod","pci_addr":"0000:03:10.0"}' },
    ]},
                                  
    { id: 'gpu', name: _L('GPU', 'GPU'), endpoints: [
      { m: 'GET', p: '/gpu/list', d: _L('GPU 장치 목록', 'GPU device list') },
      { m: 'GET', p: '/gpu/metrics', d: _L('GPU 메트릭', 'GPU metrics') },
    ]},
                                        
    { id: 'backup', name: _L('백업', 'Backup'), endpoints: [
      { m: 'GET', p: '/backup/policies', d: _L('백업 정책 목록', 'Backup policy list') },
      { m: 'POST', p: '/backup/policies', d: _L('백업 정책 설정', 'Set backup policy'), body: '{"vm":"web","schedule":"daily","keep":7}' },
      { m: 'DELETE', p: '/backup/policies', d: _L('백업 정책 삭제', 'Delete backup policy'), body: '{"vm":"web"}' },
      { m: 'GET', p: '/backup/history', d: _L('백업 이력', 'Backup history'), body: '{"vm":"web"}' },
      { m: 'POST', p: '/backup/incremental', d: _L('증분 백업', 'Incremental backup'), body: '{"vm":"web"}' },
      { m: 'POST', p: '/backup/verify', d: _L('백업 검증', 'Verify backup'), body: '{"vm":"web"}' },
      { m: 'POST', p: '/backup/replicate', d: _L('원격 복제', 'Replicate to remote'), body: '{"vm":"web","target":"host2","user":"root"}' },
      { m: 'POST', p: '/backup/export-s3', d: _L('S3 익스포트', 'Export to S3'), body: '{"vm":"web","bucket":".."}' },
    ]},
                                              
    { id: 'templates', name: _L('템플릿', 'Templates'), endpoints: [
      { m: 'GET', p: '/templates', d: _L('템플릿 목록', 'Template list') },
      { m: 'POST', p: '/templates', d: _L('템플릿 생성', 'Create template'), body: '{"name":"t1","vcpu":2}' },
      { m: 'GET', p: '/templates/history', d: _L('템플릿 이력', 'Template history') },
      { m: 'GET', p: '/templates/{name}', d: _L('템플릿 상세', 'Get template') },
      { m: 'DELETE', p: '/templates/{name}', d: _L('템플릿 삭제', 'Delete template') },
    ]},
                                                 
    { id: 'config', name: _L('설정 & 데몬', 'Config & Daemon'), endpoints: [
      { m: 'POST', p: '/config/backup', d: _L('설정 파일 백업', 'Backup config file') },
      { m: 'GET', p: '/config/backup', d: _L('설정 백업 조회', 'Get config backup') },
      { m: 'GET', p: '/config/history', d: _L('설정 변경 이력', 'Config history') },
      { m: 'POST', p: '/config/reload', d: _L('설정 핫 리로드', 'Hot-reload config') },
      { m: 'GET', p: '/config/daemon', d: _L('daemon.conf 조회', 'Get daemon.conf') },
      { m: 'PUT', p: '/config/daemon', d: _L('daemon.conf 변경', 'Set daemon.conf'), body: '{"key":"value"}' },
    ]},
                                                   
    { id: 'iso-vnc', name: _L('ISO & 콘솔', 'ISO & Console'), endpoints: [
      { m: 'GET', p: '/iso', d: _L('ISO 이미지 목록', 'ISO image list') },
      { m: 'GET', p: '/vnc/{vm_name}', d: _L('VNC 접속 정보', 'VNC connection info') },
    ]},
                                                             
    { id: 'tenant-overlay', name: _L('테넌트 암호화 오버레이', 'Encrypted Tenant Overlay'), isNew: true, rpcOnly: true, endpoints: [
      rpc('tenant_overlay.create', '암호화 오버레이 생성 + 서브넷 할당', 'Create encrypted overlay + subnet'),
      rpc('tenant_overlay.delete', '테넌트 오버레이 삭제', 'Delete tenant overlay'),
      rpc('tenant_overlay.list', '테넌트 오버레이 목록', 'List tenant overlays'),
      rpc('tenant_overlay.get', '테넌트 오버레이 상세', 'Get tenant overlay detail'),
      rpc('tenant_overlay.attach_vm', 'VM을 오버레이에 조인', 'Attach VM to overlay'),
      rpc('tenant_overlay.detach_vm', '오버레이에서 VM 제거', 'Detach VM from overlay'),
    ]},
    { id: 'qos', name: _L('QoS · 카오스', 'QoS & Chaos'), isNew: true, rpcOnly: true, endpoints: [
      rpc('qos.vm.set', 'VM별 대역폭 QoS 설정', 'Set per-VM bandwidth QoS'),
      rpc('qos.vm.get', 'VM별 QoS 조회', 'Get per-VM QoS'),
      rpc('qos.tenant.set', '테넌트별 QoS 설정', 'Set per-tenant QoS'),
      rpc('qos.tenant.get', '테넌트별 QoS 조회', 'Get per-tenant QoS'),
      rpc('qos.stats', 'QoS 집행 통계', 'QoS enforcement stats'),
      rpc('qos.chaos.start', 'netem 카오스 폴트 주입 시작', 'Start netem chaos fault injection'),
      rpc('qos.chaos.stop', 'netem 카오스 중지', 'Stop netem chaos'),
      rpc('qos.chaos.status', '카오스 상태', 'Chaos status'),
    ]},
    { id: 'trace', name: _L('트래픽 트레이싱', 'Traffic Tracing'), isNew: true, rpcOnly: true, endpoints: [
      rpc('debug.trace.start', '트래픽 트레이스 시작', 'Start traffic trace'),
      rpc('debug.trace.stop', '트레이스 중지', 'Stop trace'),
      rpc('debug.trace.status', '트레이스 상태', 'Trace status'),
      rpc('debug.trace.list', '트레이스 목록', 'List traces'),
      rpc('debug.trace.report', '트레이스 리포트/병목 분석', 'Trace report / bottleneck analysis'),
    ]},
    { id: 'suricata', name: _L('Suricata IDS/IPS', 'Suricata IDS/IPS'), isNew: true, endpoints: [
      { m: 'GET', p: '/suricata/status', d: _L('엔진 상태', 'Engine status') },
      { m: 'GET', p: '/suricata/policy', d: _L('탐지 정책 조회', 'Get detection policy') },
      { m: 'PUT', p: '/suricata/policy', d: _L('탐지 정책 설정', 'Set detection policy') },
      { m: 'POST', p: '/suricata/rules/update', d: _L('룰 시그니처 업데이트', 'Update rule signatures') },
      { m: 'GET', p: '/suricata/ips/status', d: _L('IPS 인라인 차단 상태', 'IPS inline-block status') },
      { m: 'POST', p: '/suricata/ips/enable', d: _L('IPS 인라인 차단 활성화', 'Enable IPS inline blocking') },
      { m: 'POST', p: '/suricata/ips/disable', d: _L('IPS 인라인 차단 비활성화', 'Disable IPS inline blocking') },
      { m: 'GET', p: '/suricata/ips/drop', d: _L('차단 대상 SID 목록', 'List drop-target SIDs') },
      { m: 'POST', p: '/suricata/ips/drop', d: _L('차단 대상 SID 추가', 'Add drop-target SIDs') },
      { m: 'DELETE', p: '/suricata/ips/drop', d: _L('차단 대상 SID 제거', 'Remove drop-target SIDs') },
    ]},
    { id: 'nfv', name: _L('NFV 로드밸런서', 'NFV Load Balancer'), rpcOnly: true, endpoints: [
      rpc('nfv.lb.create', 'NFV 로드밸런서 생성 (VIP→백엔드)', 'Create NFV load-balancer (VIP to backends)'),
    ]},
  ];

  var total = 0; groups.forEach(function(g) { total += g.endpoints.length; });
  var badges = el('div', { class: 'flex gap-10' }, HN.statusPill('idle', 'OpenAPI 3.0'), HN.statusPill('idle', total + ' ' + _L('엔드포인트', 'Endpoints')), HN.statusPill('idle', 'JWT + RBAC'));
  var heading = HN.pagehead({ title: 'PureCVisor 2.0 REST API', actions: [badges] });
  var statLine = el('div', { class: 'stat-label mb-12' }, _L('기본', 'Base'), ': ', el('code', null, '/api/v1'), ' | ', _L('인증', 'Auth'), ': ', el('code', null, 'Bearer JWT'));

                                                                       
  var jump = el('div', { class: 'help-jump' }, groups.map(function(g) {
    return el('button', { type: 'button', class: 'help-chip' + (g.isNew ? ' help-chip-new' : ''), onclick: 'swJump("sw-grp-' + g.id + '")' },
      g.id + ' ' + g.endpoints.length);
  }));

  var searchBar = el('div', { class: 'mb-12' },
    el('input', { 'aria-label': _L('엔드포인트 검색...', 'Search endpoints...'), id: 'sw-search', class: 'sb-search', placeholder: _L('엔드포인트 검색...', 'Search endpoints...'), oninput: 'filterSwagger()', style: 'max-width:500px;font-size:13px;padding:8px 12px;border-radius:6px' }));

                                                                   
  var groupNodes = groups.map(function(g) {
    var collapsed = !(g.isNew || g.open);
    var head = el('div', { class: 'sw-grp-h', onclick: "this.closest('.sw-group').classList.toggle('collapsed')" },
      el('span', { class: 'sw-grp-id' }, g.id),
      el('span', { class: 'sw-grp-name' }, g.name),
      el('span', { class: 'sw-grp-count' }, String(g.endpoints.length)),
      g.isNew ? HN.statusPill('idle', '2.0') : null);
    var epNodes = g.endpoints.map(function(e, i) {
      var id = 'sw-d-' + g.id + '-' + i;
      var row = el('div', { onclick: "document.getElementById('" + id + "').classList.toggle('hidden')", style: 'display:flex;align-items:center;padding:8px 12px;cursor:pointer;gap:10px;background:var(--bg2)' },
        el('span', { style: 'background:' + mc(e.m) + ';color:#fff;font-size:10px;font-weight:700;padding:2px 8px;border-radius:3px;min-width:52px;text-align:center' }, e.m),
        el('span', { style: 'font-family:monospace;font-size:12px' }, e.p),
        el('span', { class: 'stat-label', style: 'margin-left:auto' }, e.d),
        e.auth === false ? null : el('span', { style: 'font-size:9px;color:var(--yellow)' }, '🔒'));
      var detail = el('div', { id: id, class: 'hidden', style: 'padding:10px 12px;background:var(--bg3);font-size:11px' },
        e.body ? el('div', { class: 'mb-6' },
          el('b', null, _L('요청 본문:', 'Request Body:')),
          el('pre', { style: 'background:var(--bg);padding:8px;border-radius:4px;color:var(--green);overflow-x:auto' }, e.body)) : null,
        el('button', { class: 'btn', style: 'font-size:10px;padding:3px 10px', onclick: "swTry('" + e.m + "','" + e.p + "'," + (e.body ? "'" + e.body.replace(/'/g, "\\'") + "'" : 'null') + ")" }, '▶ ' + _L('실행', 'Try it')));
      return el('div', { class: 'mb-2 sw-ep', 'data-sw': (e.m + ' ' + e.p + ' ' + e.d).toLowerCase(), style: 'border:1px solid var(--border);border-radius:4px;overflow:hidden' }, row, detail);
    });
    return el('div', { class: 'sw-group' + (collapsed ? ' collapsed' : ''), id: 'sw-grp-' + g.id, 'data-default-collapsed': collapsed ? '1' : '0' },
      head,
      g.rpcOnly ? el('div', { class: 'sw-rpconly' }, _L('RPC 전용 — 전용 REST·CLI·UI 경로 없음 (POST /api/v1/rpc)', 'RPC-only — no dedicated REST/CLI/UI route (POST /api/v1/rpc)')) : null,
      epNodes);
  });
  var content = el('div', { id: 'sw-content' }, groupNodes);

  clearEl(b);
  b.appendChild(frag(heading, statLine, jump, searchBar, content));
}
window.renderSwaggerApi = renderSwaggerApi;

                                                
function swJump(id) {
  var e = document.getElementById(id);
  if (!e) return;
  e.classList.remove('collapsed');
  if (typeof e.scrollIntoView === 'function') e.scrollIntoView({ behavior: 'smooth', block: 'start' });
}
window.swJump = swJump;

                                              
                                                    
function filterSwagger() {
  var input = document.getElementById('sw-search');
  var q = input ? input.value.trim().toLowerCase() : '';
  document.querySelectorAll('#sw-content .sw-group').forEach(function(grp) {
    if (!q) {
      grp.style.display = '';
      if (grp.dataset.defaultCollapsed === '1') grp.classList.add('collapsed');
      else grp.classList.remove('collapsed');
      grp.querySelectorAll('.sw-ep').forEach(function(ep) { ep.style.display = ''; });
      return;
    }
    var any = false;
    grp.querySelectorAll('.sw-ep').forEach(function(ep) {
      var hit = ep.dataset.sw.indexOf(q) !== -1;
      ep.style.display = hit ? '' : 'none';
      if (hit) any = true;
    });
    if (any) { grp.style.display = ''; grp.classList.remove('collapsed'); }
    else { grp.style.display = 'none'; }
  });
}
window.filterSwagger = filterSwagger;

                                                                 
                                                         
                                                     
                                                 
                                                                  
                                     
async function swTry(m, p, body) {
  var url = API_BASE + p.replace(/\{[^}]+\}/g, 'test');
  try { var opts = { headers: { Authorization: 'Bearer ' + authToken } };
    if (m === 'POST' || m === 'PUT' || m === 'DELETE') { opts.method = m; opts.headers['Content-Type'] = 'application/json'; if (body) opts.body = body; }
    var r = await fetch(url, opts); var txt = await r.text(); var pretty = txt; try { pretty = JSON.stringify(JSON.parse(txt), null, 2); } catch (e) {                }
    var el = PCV.uxlib.el;
    showModal([
      el('h2', null, _L('응답', 'Response') + ': ' + m + ' ' + p),
      HN.row(_L('상태', 'Status'), el('span', { style: 'color:' + (r.ok ? 'var(--green)' : 'var(--red)') }, r.status)),
      el('pre', { style: 'background:var(--bg);padding:12px;border-radius:6px;max-height:400px;overflow:auto;font-size:11px;color:var(--cyan);white-space:pre-wrap' }, pretty),
      el('div', { style: 'text-align:right;margin-top:12px' },
        el('button', { class: 'btn', onclick: 'closeModal()' }, t('btn.close')))
    ]);
  } catch (e) { toast(_L('요청 실패', 'Request failed') + ': ' + e.message, false); }
}
window.swTry = swTry;

                                   
var kbdHelpOpen = false;
window.kbdHelpOpen = kbdHelpOpen;
var kbdDialog = null;                                    

function toggleKbdHelp() {
  if (kbdHelpOpen) { closeKbdHelp(); return; }
  kbdHelpOpen = true; window.kbdHelpOpen = kbdHelpOpen;
  var shortcuts = [
    ['Ctrl+K', _L('커맨드 팔레트', 'Command Palette')],
    ['Ctrl+N', _L('새 VM', 'New VM'), 'operator'],
    ['Ctrl+D', _L('VM 설정', 'VM Settings')],
    ['Ctrl+P', _L('환경설정', 'Preferences')],
    ['F11', _L('전체 화면', 'Fullscreen')],
    ['Escape', _L('대화상자 닫기', 'Close Dialog')],
    ['?', _L('이 도움말', 'This Help')],
    ['Ctrl+Shift+F', _L('통합 검색 (⌘K와 동일)', 'Unified Search (same as ⌘K)')],
    ['Ctrl+B', _L('사이드바 전환', 'Toggle Sidebar')],
  ];
                                                                               
  var el = PCV.uxlib.el;
  var box = el('div', { class: 'kbd-box' },
    el('h2', { class: 'kbd-title' }, _L('키보드 단축키', 'Keyboard Shortcuts')),
    el('div', { class: 'kbd-grid' },
      shortcuts.filter(function(s) {
        return !s[2] || (typeof pcvRoleAllows === 'function' && pcvRoleAllows(s[2]));
      }).map(function(s) {
        return el('div', { class: 'kbd-row' },
          el('span', { class: 'kbd-key' }, s[0]),
          el('span', { class: 'kbd-desc' }, s[1])
        );
      })
    ),
    el('div', { class: 'kbd-close' }, _L('? 또는 Esc 키로 닫기', 'Press ? or Esc to close'))
  );
  var mine = PCV.modalCore.openBare(box, {
    dialogClass: 'kbd-help',
                                                                         
    onClose: function () { if (kbdDialog !== mine) return; kbdHelpOpen = false; window.kbdHelpOpen = false; kbdDialog = null; }
  });
  kbdDialog = mine;
}
window.toggleKbdHelp = toggleKbdHelp;

                                                               
                                                
function closeKbdHelp() { if (kbdDialog) { var d = kbdDialog; kbdDialog = null; try { d.close(); } catch {} } kbdHelpOpen = false; window.kbdHelpOpen = false; }
window.closeKbdHelp = closeKbdHelp;

                                                         
PCV.help = {
  renderHelp: renderHelp,
  filterHelp: filterHelp,
  renderSwaggerApi: renderSwaggerApi,
  swJump: swJump,
  filterSwagger: filterSwagger,
  swTry: swTry,
  toggleKbdHelp: toggleKbdHelp,
  closeKbdHelp: closeKbdHelp
};
})(window.PCV);
