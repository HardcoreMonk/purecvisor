




window.PCV = window.PCV || {};
(function(PCV) {

var I18N = {
  _lang: localStorage.getItem('pcv-lang') || 'ko',
  _data: {
    ko: {

      'login': '로그인',
      'logout': '로그아웃',
      'username': '사용자명',
      'password': '비밀번호',
      'login.title': '보안 로그인',
      'login.required': '사용자명과 비밀번호를 입력하세요.',
      'login.failed': '인증 실패',
      'login.error': '연결 오류',
      'login.invalid_credentials': '아이디 또는 비밀번호가 올바르지 않습니다.',
      'login.too_many': '로그인 시도가 많습니다. 잠시 후 다시 시도하세요.',
      'login.service_unavailable': '로그인 서비스가 일시적으로 응답하지 않습니다.',
      'login.server_error': '로그인 서버가 요청을 처리하지 못했습니다.',
      'login.bad_response': '로그인 서버가 올바른 응답을 반환하지 않았습니다.',
      'login.timeout': '로그인 요청 시간이 초과되었습니다.',
      'login.network': '로그인 서버에 연결할 수 없습니다.',
      'login.tls.secure': 'TLS Secured Connection',
      'login.tls.insecure': 'Unsecured Connection',
      'login.tls.switch': 'Switch to HTTPS',
      'logged.in': '로그인 완료',
      'logged.out': '로그아웃 완료',


      'register.title': '회원가입',
      'register.user_ph': '사용자명 (3-32, a-z 0-9 _)',
      'register.pass_ph': '비밀번호 (8자 이상)',
      'register.pass2_ph': '비밀번호 확인',
      'register.note': '가입 후 기본 권한: VIEWER (조회 전용). 관리자가 권한 승격 가능.',
      'register.ok': '가입 성공! 로그인 화면으로 돌아갑니다.',
      'register.err_user': '사용자명은 3-32자 (a-z, 0-9, _)',
      'register.err_pass': '비밀번호는 8자 이상',
      'register.err_match': '비밀번호 확인이 일치하지 않습니다',
      'register.err_same': '비밀번호는 사용자명과 달라야 합니다',
      'register.err_disabled': '회원가입이 비활성화되어 있습니다 (관리자에게 문의)',
      'changepw.title': '비밀번호 변경',
      'changepw.old_ph': '현재 비밀번호',
      'changepw.new_ph': '새 비밀번호 (8자 이상)',
      'changepw.new2_ph': '새 비밀번호 확인',
      'changepw.note': '변경 후 다른 디바이스의 모든 세션이 자동 로그아웃됩니다.',
      'changepw.ok': '변경 완료. 3초 후 다시 로그인합니다.',
      'changepw.err_old': '현재 비밀번호를 입력하세요',
      'changepw.err_short': '새 비밀번호는 8자 이상',
      'changepw.err_match': '새 비밀번호 확인이 일치하지 않습니다',
      'changepw.err_same': '새 비밀번호는 현재와 달라야 합니다',
      'btn.register': '가입',
      'btn.change': '변경',
      'msg.processing': '처리 중...',
      'msg.network_err': '네트워크 오류',
      'monitor.cluster_timeline': '리소스 흐름 (최근 5분)',


      'nav.vm_library': 'VM 자산',
      'nav.infrastructure': 'Infrastructure',
      'nav.monitor': 'Monitor',
      'nav.networks': 'Networks',
      'nav.storage': 'Storage',
      'nav.containers': '컨테이너',
      'nav.host': 'Host',
      'nav.cluster_ha': 'Cluster HA',
      'nav.ovn': 'OVN',
      'nav.accounts': 'Accounts',
      'nav.api_mgmt': 'API Management',
      'nav.swagger': 'Swagger API',
      'nav.rest_guide': 'REST API Guide',
      'nav.help': 'Help',
      'nav.service_guide': 'Service Guide',


      'mon.overview': 'Overview',
      'mon.cluster': 'Cluster',
      'mon.hosts': 'Hosts',
      'mon.vms': 'VMs',
      'mon.storage': 'Storage',
      'mon.alerts': 'Alerts',


      'tab.summary': '요약',
      'tab.console': '콘솔',
      'tab.snapshots': '스냅샷',
      'tab.performance': '성능',
      'tab.timeline': '타임라인',


      'vm.new': '새 VM',
      'vm.create': 'VM 생성',
      'vm.creating': 'VM 생성 중',
      'vm.created': 'VM 생성 완료',
      'vm.delete': 'VM 삭제',
      'vm.deleted': 'VM 삭제됨',
      'vm.delete.confirm': '이 작업은 VM과 zvol을 영구 삭제합니다.',
      'vm.delete.type_name': '확인을 위해 VM 이름을 입력하세요:',
      'vm.name_mismatch': '이름 불일치 — 삭제 취소됨',
      'vm.select': 'VM Library에서 VM을 선택하세요',
      'vm.settings': '설정',
      'vm.snapshot': '스냅샷',


      'power.start': '시작',
      'power.stop': '중지',
      'power.pause': '일시정지',
      'power.resume': '재개',


      'btn.create': '생성',
      'btn.delete': '삭제',
      'btn.save': '저장',
      'btn.cancel': '취소',
      'btn.close': '닫기',
      'btn.apply': '적용',
      'btn.next': '다음',
      'btn.prev': '이전',
      'btn.edit': '편집',
      'btn.confirm': '확인',
      'btn.stop_selected': '선택 항목 중지',


      'net.new': '네트워크 추가',
      'net.created': '네트워크 생성됨',
      'net.deleted': '네트워크 삭제됨',
      'net.mode.nat': 'NAT — 자동 MASQUERADE + DHCP',
      'net.mode.isolated': 'Isolated — VM 간 통신만',
      'net.mode.routed': 'Routed — 정적 라우팅',
      'net.mode.bridge': 'Bridge — 물리 NIC 바인딩',
      'net.phys_required': 'Bridge 모드에서는 물리 NIC 선택이 필요합니다',


      'ctr.new': '생성 CT',
      'ctr.select': '컨테이너를 선택하세요',
      'ctr.starting': '컨테이너 시작 중',
      'ctr.stopping': '컨테이너 중지 중',
      'ctr.destroying': '컨테이너 삭제 중',
      'ctr.destroyed': '컨테이너 삭제됨',
      'ctr.delete.confirm': '컨테이너와 rootfs가 영구 삭제됩니다.',
      'ctr.delete.type_name': '확인을 위해 컨테이너 이름을 입력하세요:',
      'ctr.console.stopped': '콘솔 접근을 위해 먼저 컨테이너를 시작하세요.',


      'stg.zvol_created': 'Zvol 생성됨',
      'stg.zvol_destroyed': 'Zvol 삭제됨',


      'snap.name_prompt': '스냅샷 이름:',
      'snap.created': '스냅샷 생성됨',
      'snap.reverted': '되돌리기 완료',
      'snap.deleted': '스냅샷 삭제됨',
      'snap.none': '스냅샷 없음',
      'snap.revert_confirm': '되돌리기 하시겠습니까?',


      'alert.enabled': '알림 엔진 활성 중',
      'alert.disabled': '알림 비활성 — enabled=true로 활성화',
      'alert.saved': '설정 저장됨',


      'vnc.popup_blocked': '팝업이 차단되었습니다. 브라우저 설정에서 허용하세요.',
      'vnc.open_popup': 'noVNC 새 창 열기',
      'vnc.embedded': '내장 noVNC',
      'vnc.copy_addr': '주소 복사',
      'vnc.addr_copied': 'VNC 주소 복사됨',
      'vnc.connected': '연결됨',
      'vnc.disconnected': '연결 끊김',
      'vnc.fullscreen': '전체 화면',
      'vnc.fit': '화면 맞춤',
      'vnc.unavailable': 'VNC를 사용할 수 없습니다',
      'vnc.manual_check': '수동 확인',
      'vnc.loading_connecting': 'noVNC 로딩 및 연결 중',
      'vnc.remote': '원격 화면',
      'vnc.error_suffix': '(오류)',
      'vnc.security_failure': '보안 협상 실패',
      'vnc.reconnect': '다시 연결',
      'vnc.connecting': '연결 중',
      'vnc.iso_eject_title': '설치 완료 후',
      'vnc.iso_eject_body': 'VM 설정 > CD/DVD (SATA)에서 Eject 후 콘솔에서 Enter',


      'loading': '로딩 중...',
      'error': '오류',
      'connected': 'Connected',
      'not_connected': 'Not connected',
      'no_vm': 'No VM',
      'filter': '이름으로 찾기',
      'search': 'Search...',
      'events.waiting': '이벤트 대기 중...',
      'events.clear': '초기화',
      'events.log': '이벤트 로그',


      'nic.added': 'NIC 추가됨',
      'nic.removed': 'NIC 제거됨',


      'iso.mounted': '마운트됨',
      'iso.ejected': '꺼내기 완료',
      'iso.browser_title': 'ISO 이미지 탐색기',
      'iso.browser_desc': '마운트할 ISO/IMG 이미지를 선택하세요.',
      'iso.not_found': 'ISO/IMG 파일을 찾을 수 없습니다',
      'iso.path_required': 'ISO 경로를 선택하세요.',


      'backup.policy_set': '백업 정책 설정됨',


      'auth.user_created': '사용자 생성됨',
      'auth.user_deleted': '사용자 삭제됨',
      'auth.role_changed': '역할 변경됨',
      'auth.required': 'Username and password required',


      'agent.config_saved': 'AI Agent 설정 저장됨',
      'agent.test_connection': 'Test Connection',


      'ws.connected': 'WebSocket 연결됨',
      'ws.live': 'Live',


      'nav.templates': '템플릿',
      'nav.config': '설정 관리',
      'nav.docker': 'Docker/OCI',
      'nav.terraform': 'Terraform',
      'nav.federation': '페더레이션',
      'vm.clone': 'VM 복제',
      'vm.disk_resize': '디스크 리사이즈',
      'vm.cpu_pin': 'CPU 피닝',
      'vm.bandwidth': '대역폭 QoS',
      'vm.export_ova': 'OVA 내보내기',
      'vm.import_ova': 'OVA 가져오기',
      'storage.pool_create': '풀 생성',
      'storage.pool_destroy': '풀 삭제',
      'storage.pool_scrub': '풀 스크럽',
      'ctr.set_limits': '리소스 제한 설정',
      'cluster.drain': '노드 드레인',
      'cluster.resume': '노드 재개',
      'cluster.maintenance_enter': '유지보수 모드 진입',
      'cluster.maintenance_exit': '유지보수 모드 해제',
      'cluster.quota': '리소스 쿼터',
      'config.backup': '설정 백업',
      'config.history': '설정 변경 이력',
      'template.history': '템플릿 이력',

      'menu.file': '파일',
      'menu.edit': '편집',
      'menu.view': '보기',
      'menu.go': '이동',
      'menu.run': '실행',
      'menu.terminal': '관제',
      'menu.accounts': '계정',
      'menu.help': '도움말',

      'menu.file.new_vm': '새 VM...',
      'menu.file.new_network': '새 네트워크...',
      'menu.file.new_pool': '새 풀...',
      'menu.file.new_zvol': '새 Zvol...',
      'menu.file.import_ova': 'OVA 가져오기...',
      'menu.file.connect': '서버 연결...',
      'menu.file.preferences': '환경설정',

      'menu.edit.vm_settings': 'VM 설정...',
      'menu.edit.snapshots': '스냅샷...',
      'menu.edit.agent_config': 'AI Agent 설정...',
      'menu.edit.config_mgmt': '설정 관리',

      'menu.view.toggle_sidebar': '사이드바 전환',
      'menu.view.toggle_panel': '패널 전환',
      'menu.view.fullscreen': '전체 화면',
      'menu.view.vnc_console': 'VNC 콘솔...',
      'menu.view.color_theme': '테마 변경',
      'menu.view.cmd_palette': '커맨드 팔레트',
      'menu.view.vm_library': 'VM 자산',
      'menu.view.container_library': '컨테이너',
      'menu.view.cluster': '클러스터',
      'menu.view.infrastructure': '운영',

      'menu.go.networks': '네트워크',
      'menu.go.storage': '스토리지',
      'menu.go.lxc': 'LXC 컨테이너',
      'menu.go.host': '호스트',
      'menu.go.ovn': 'OVN SDN',
      'menu.go.security_groups': '보안 그룹',
      'menu.go.gpu': 'GPU 장치',
      'menu.go.templates': '템플릿',
      'menu.go.docker': 'Docker/OCI',
      'menu.go.federation': '페더레이션',
      'menu.go.cloud_migration': '클라우드 마이그레이션',
      'menu.go.terraform': 'Terraform IaC',

      'menu.run.power_on': '전원 켜기',
      'menu.run.power_off': '전원 끄기',
      'menu.run.suspend': '일시 정지',
      'menu.run.resume': '재개',
      'menu.run.drain': '노드 드레인...',
      'menu.run.resume_node': '노드 재개...',
      'menu.run.maintenance_enter': '유지보수 진입...',
      'menu.run.maintenance_exit': '유지보수 해제...',

      'menu.terminal.new': '새 터미널',
      'menu.terminal.events': '이벤트',
      'menu.terminal.alerts': '알림',
      'menu.terminal.output': '출력',
      'menu.terminal.cluster_ha': '클러스터 HA',
      'menu.terminal.cluster_mon': '클러스터 모니터링',
      'menu.terminal.mon_overview': '운영 개요',
      'menu.terminal.mon_hosts': '호스트 상태',
      'menu.terminal.security_events': '보안 이벤트',
      'menu.terminal.audit_log': '감사 로그',

      'menu.accounts.users': '사용자 & 역할 (RBAC)',
      'menu.accounts.api_mgmt': 'API 관리',

      'menu.help.welcome': '시작하기',
      'menu.help.cmd_ref': '명령어 참조',
      'menu.help.rest_guide': 'REST API 가이드',
      'menu.help.swagger': 'Swagger API',
      'menu.help.about': '정보',
      'menu.help.updates': '업데이트 확인...',

      'menu.run.drain_node': '노드 드레인...',
      'menu.run.enter_maintenance': '유지보수 진입...',
      'menu.run.exit_maintenance': '유지보수 해제...',
      'menu.terminal.new_terminal': '새 터미널',
      'menu.terminal.cluster_monitoring': '클러스터 모니터링',
      'menu.terminal.monitor_overview': '운영 개요',
      'menu.terminal.host_monitor': '호스트 상태',
      'menu.accounts.users_roles': '사용자 & 역할 (RBAC)',
      'menu.help.cmd_reference': '명령어 참조',
      'menu.help.complete_guide': '완벽 가이드',
      'menu.help.check_updates': '업데이트 확인...',
      'menu.go.containers': 'LXC 컨테이너',

      'status.running': '실행 중',
      'status.stopped': '중지됨',
      'status.paused': '일시 정지',

      'sidebar.vm': 'VM 자산',
      'sidebar.container': '컨테이너',
      'sidebar.cluster': 'CLUSTER',
      'sidebar.infra': '운영',

      'toolbar.snap': '스냅샷',
      'toolbar.settings': '설정',
      'toolbar.stop_selected': '선택 항목 중지',

      'panel.terminal': '터미널',
      'panel.events': '이벤트',
      'panel.alerts': '알림',
      'panel.security_events': '보안 이벤트',
      'panel.output': '출력',

      'msg.name_required': '이름은 필수입니다',
      'msg.no_containers': '컨테이너가 없습니다',
      'msg.no_snapshots': '삭제할 스냅샷 없음',
      'msg.invalid_snap_name': '잘못된 스냅샷 이름',
      'msg.invalid_ami': '잘못된 AMI ID 형식',
      'msg.not_implemented': '아직 구현되지 않았습니다',
      'msg.nameserver_added': '네임서버 추가됨',
      'msg.reboot_error': '재부팅 오류',
      'msg.pool_name_required': '풀 이름 필수',
      'msg.theme_exported': '테마 내보내기 완료',
      'msg.theme_imported': '테마 가져오기 완료',
      'msg.invalid_theme': '잘못된 테마 파일',
      'msg.popup_blocked': '팝업이 차단되었습니다',
      'msg.delete_all': '전체 삭제',
      'msg.zen_mode': 'Zen 모드 — Escape를 눌러 종료',
      'msg.clone_started': '복제 시작됨',
      'msg.clone_name_prompt': '새 VM 이름:',
      'msg.export_started': 'OVA 내보내기 시작됨',
      'msg.nic_bridge_prompt': '브릿지 (기본 pcvbr0):',
      'msg.mac_prompt': '제거할 MAC 주소:',
      'msg.snap_creating': '스냅샷 생성 중...',
      'msg.snap_deleting': '스냅샷 삭제 중...',
      'msg.snap_rolling_back': '스냅샷 롤백 중...',
      'msg.vm_creating': 'VM 생성 중...',
      'msg.disk_resize_prompt': '새 크기 (GB):',
      'msg.bandwidth_prompt': '대역폭 (예: 100mbit):',
      'msg.cpu_pin_prompt': 'CPU 코어 (예: 0-3):',
      'msg.settings_saved': '설정 저장됨',
      'msg.iso_mounted': 'ISO 마운트됨',
      'msg.iso_ejected': 'ISO 꺼내기 완료',


      'session.mgmt': '세션 관리',
      'session.revoke': '세션 강제 해제',
      'apikey.mgmt': 'API 키 관리',
      'apikey.create': '새 키 생성',
      'apikey.revoke': '키 폐기',
      'alert.silence': '알림 음소거',
      'alert.silence.new': '새 음소거',
      'alert.routing': '알림 라우팅 설정',
      'alert.routing.save': '라우팅 저장',
      'config.reload': '설정 리로드',
      'config.reload.confirm': '데몬 설정을 리로드하시겠습니까?',
      'backup.verify': '스냅샷 무결성 검증',
      'jobs.persistent': '영속 작업 목록',
      'db.migration': 'DB 스키마 상태',
      'container.clone': '컨테이너 클론',
      'container.memory.detail': '메모리 상세',
      'container.health': '헬스 체크',
      'pool.conninfo': '커넥션 풀 상태',
      'health.deep': '심화 헬스 체크',
      'empty.no_silences': '활성 음소거 없음',
      'empty.no_jobs': '진행 중인 작업 없음',
      'empty.no_apikeys': '등록된 API 키 없습니다',
    },

    en: {
      'login': 'Login',
      'logout': 'Logout',
      'username': 'Username',
      'password': 'Password',
      'login.title': 'Secure Login',
      'login.required': 'Username and password required',
      'login.failed': 'Authentication failed',
      'login.error': 'Connection error',
      'login.invalid_credentials': 'Invalid username or password.',
      'login.too_many': 'Too many login attempts. Try again later.',
      'login.service_unavailable': 'Login service is temporarily unavailable.',
      'login.server_error': 'Login server could not process the request.',
      'login.bad_response': 'Login server returned an invalid response.',
      'login.timeout': 'Login request timed out.',
      'login.network': 'Could not connect to the login server.',
      'login.tls.secure': 'TLS Secured Connection',
      'login.tls.insecure': 'Unsecured Connection',
      'login.tls.switch': 'Switch to HTTPS',
      'logged.in': 'Logged in',
      'logged.out': 'Logged out',


      'register.title': 'Register',
      'register.user_ph': 'Username (3-32, a-z 0-9 _)',
      'register.pass_ph': 'Password (8+ chars)',
      'register.pass2_ph': 'Confirm password',
      'register.note': 'Default role after signup: VIEWER (read-only). Admin can promote.',
      'register.ok': 'Registration succeeded. Returning to login.',
      'register.err_user': 'Username must be 3-32 chars (a-z, 0-9, _)',
      'register.err_pass': 'Password must be at least 8 characters',
      'register.err_match': 'Password confirmation does not match',
      'register.err_same': 'Password must differ from username',
      'register.err_disabled': 'Self-registration is disabled (contact admin)',
      'changepw.title': 'Change Password',
      'changepw.old_ph': 'Current password',
      'changepw.new_ph': 'New password (8+ chars)',
      'changepw.new2_ph': 'Confirm new password',
      'changepw.note': 'Session will be invalidated after change. Re-login required.',
      'changepw.ok': 'Password changed. Please log in again.',
      'changepw.err_old': 'Current password is incorrect',
      'changepw.err_short': 'New password must be at least 8 characters',
      'changepw.err_match': 'New password confirmation does not match',
      'changepw.err_same': 'New password must differ from current',
      'btn.register': 'Register',
      'btn.change': 'Change',
      'msg.processing': 'Processing...',
      'msg.network_err': 'Network error',
      'monitor.cluster_timeline': 'Resource Flow (last 5 min)',

      'nav.vm_library': 'VM Inventory',
      'nav.infrastructure': 'Infrastructure',
      'nav.monitor': 'Monitor',
      'nav.networks': 'Networks',
      'nav.storage': 'Storage',
      'nav.containers': 'Containers',
      'nav.host': 'Host',
      'nav.cluster_ha': 'Cluster HA',
      'nav.ovn': 'OVN',
      'nav.accounts': 'Accounts',
      'nav.api_mgmt': 'API Management',
      'nav.swagger': 'Swagger API',
      'nav.rest_guide': 'REST API Guide',
      'nav.help': 'Help',
      'nav.service_guide': 'Service Guide',

      'mon.overview': 'Overview',
      'mon.cluster': 'Cluster',
      'mon.hosts': 'Hosts',
      'mon.vms': 'VMs',
      'mon.storage': 'Storage',
      'mon.alerts': 'Alerts',

      'tab.summary': 'Summary',
      'tab.console': 'Console',
      'tab.snapshots': 'Snapshots',
      'tab.performance': 'Performance',
      'tab.timeline': 'Timeline',

      'vm.new': 'New VM',
      'vm.create': 'Create VM',
      'vm.creating': 'Creating VM',
      'vm.created': 'VM created',
      'vm.delete': 'Delete VM',
      'vm.deleted': 'VM deleted',
      'vm.delete.confirm': 'This will permanently delete the VM and its zvol.',
      'vm.delete.type_name': 'Type the VM name to confirm:',
      'vm.name_mismatch': 'Name mismatch — deletion cancelled',
      'vm.select': 'Select a VM from the library',
      'vm.settings': 'Settings',
      'vm.snapshot': 'Snapshot',

      'power.start': 'Start',
      'power.stop': 'Stop',
      'power.pause': 'Pause',
      'power.resume': 'Resume',

      'btn.create': 'Create',
      'btn.delete': 'Delete',
      'btn.save': 'Save',
      'btn.cancel': 'Cancel',
      'btn.close': 'Close',
      'btn.apply': 'Apply',
      'btn.next': 'Next',
      'btn.prev': 'Previous',
      'btn.edit': 'Edit',
      'btn.confirm': 'Confirm',
      'btn.stop_selected': 'Stop Selected',

      'net.new': 'Add Network',
      'net.created': 'Network created',
      'net.deleted': 'Network deleted',
      'net.mode.nat': 'NAT — Auto MASQUERADE + DHCP',
      'net.mode.isolated': 'Isolated — VM-to-VM only',
      'net.mode.routed': 'Routed — Static routing',
      'net.mode.bridge': 'Bridge — Physical NIC binding',
      'net.phys_required': 'Physical NIC selection required for bridge mode',

      'ctr.new': 'Create CT',
      'ctr.select': 'Select a container',
      'ctr.starting': 'Starting container',
      'ctr.stopping': 'Stopping container',
      'ctr.destroying': 'Destroying container',
      'ctr.destroyed': 'Container destroyed',
      'ctr.delete.confirm': 'Container and rootfs will be permanently destroyed.',
      'ctr.delete.type_name': 'Type the container name to confirm:',
      'ctr.console.stopped': 'Start the container first to access console.',

      'stg.zvol_created': 'Zvol created',
      'stg.zvol_destroyed': 'Zvol destroyed',

      'snap.name_prompt': 'Snapshot name:',
      'snap.created': 'Snapshot created',
      'snap.reverted': 'Reverted',
      'snap.deleted': 'Snapshot deleted',
      'snap.none': 'No snapshots',
      'snap.revert_confirm': 'Revert to this snapshot?',

      'alert.enabled': 'Alert engine is actively monitoring',
      'alert.disabled': 'Alert engine disabled — set enabled=true to activate',
      'alert.saved': 'Configuration saved',

      'vnc.popup_blocked': 'Popup blocked. Please allow popups in browser settings.',
      'vnc.open_popup': 'Open noVNC in New Window',
      'vnc.embedded': 'Embedded noVNC',
      'vnc.copy_addr': 'Copy Address',
      'vnc.addr_copied': 'VNC address copied',
      'vnc.connected': 'Connected',
      'vnc.disconnected': 'Disconnected',
      'vnc.fullscreen': 'Fullscreen',
      'vnc.fit': 'Fit',
      'vnc.unavailable': 'VNC unavailable',
      'vnc.manual_check': 'Manual Check',
      'vnc.loading_connecting': 'Loading noVNC and connecting',
      'vnc.remote': 'Remote',
      'vnc.error_suffix': '(error)',
      'vnc.security_failure': 'Security failure',
      'vnc.reconnect': 'Reconnect',
      'vnc.connecting': 'Connecting',
      'vnc.iso_eject_title': 'After installation',
      'vnc.iso_eject_body': 'VM Settings > CD/DVD (SATA) > Eject, then press Enter in the console',

      'loading': 'Loading...',
      'error': 'Error',
      'connected': 'Connected',
      'not_connected': 'Not connected',
      'no_vm': 'No VM',
      'filter': 'Filter by name',
      'search': 'Search...',
      'events.waiting': 'Waiting for events...',
      'events.clear': 'Clear',
      'events.log': 'Event Log',

      'nic.added': 'NIC added',
      'nic.removed': 'NIC removed',

      'iso.mounted': 'Mounted',
      'iso.ejected': 'Ejected',
      'iso.browser_title': 'ISO Image Browser',
      'iso.browser_desc': 'Select an ISO/IMG image to mount.',
      'iso.not_found': 'No ISO/IMG files found',
      'iso.path_required': 'Select an ISO path.',

      'backup.policy_set': 'Backup policy set',

      'auth.user_created': 'User created',
      'auth.user_deleted': 'User deleted',
      'auth.role_changed': 'Role changed',
      'auth.required': 'Username and password required',

      'agent.config_saved': 'AI Agent config saved',
      'agent.test_connection': 'Test Connection',

      'ws.connected': 'WebSocket connected',
      'ws.live': 'Live',


      'nav.templates': 'Templates',
      'nav.config': 'Config',
      'nav.docker': 'Docker/OCI',
      'nav.terraform': 'Terraform',
      'nav.federation': 'Federation',
      'vm.clone': 'Clone VM',
      'vm.disk_resize': 'Disk Resize',
      'vm.cpu_pin': 'CPU Pinning',
      'vm.bandwidth': 'Bandwidth QoS',
      'vm.export_ova': 'Export OVA',
      'vm.import_ova': 'Import OVA',
      'storage.pool_create': 'Create Pool',
      'storage.pool_destroy': 'Destroy Pool',
      'storage.pool_scrub': 'Pool Scrub',
      'ctr.set_limits': 'Set Resource Limits',
      'cluster.drain': 'Drain Node',
      'cluster.resume': 'Resume Node',
      'cluster.maintenance_enter': 'Enter Maintenance',
      'cluster.maintenance_exit': 'Exit Maintenance',
      'cluster.quota': 'Resource Quota',
      'config.backup': 'Config Backup',
      'config.history': 'Config History',
      'template.history': 'Template History',

      'menu.file': 'File',
      'menu.edit': 'Edit',
      'menu.view': 'View',
      'menu.go': 'Go',
      'menu.run': 'Run',
      'menu.terminal': 'Operations',
      'menu.accounts': 'Accounts',
      'menu.help': 'Help',

      'menu.file.new_vm': 'New VM...',
      'menu.file.new_network': 'New Network...',
      'menu.file.new_pool': 'New Pool...',
      'menu.file.new_zvol': 'New Zvol...',
      'menu.file.import_ova': 'Import OVA...',
      'menu.file.connect': 'Connect to Server...',
      'menu.file.preferences': 'Preferences',
      'menu.edit.vm_settings': 'VM Settings...',
      'menu.edit.snapshots': 'Snapshots...',
      'menu.edit.agent_config': 'AI Agent Config...',
      'menu.edit.config_mgmt': 'Config Management',
      'menu.view.toggle_sidebar': 'Toggle Sidebar',
      'menu.view.toggle_panel': 'Toggle Panel',
      'menu.view.fullscreen': 'Full Screen',
      'menu.view.vnc_console': 'VNC Console...',
      'menu.view.color_theme': 'Color Theme',
      'menu.view.cmd_palette': 'Command Palette',
      'menu.view.vm_library': 'VM Inventory',
      'menu.view.container_library': 'Containers',
      'menu.view.cluster': 'Cluster',
      'menu.view.infrastructure': 'Operations',
      'menu.go.networks': 'Networks',
      'menu.go.storage': 'Storage',
      'menu.go.lxc': 'LXC Containers',
      'menu.go.host': 'Host',
      'menu.go.ovn': 'OVN SDN',
      'menu.go.security_groups': 'Security Groups',
      'menu.go.gpu': 'GPU Devices',
      'menu.go.templates': 'Templates',
      'menu.go.docker': 'Docker/OCI',
      'menu.go.federation': 'Federation',
      'menu.go.cloud_migration': 'Cloud Migration',
      'menu.go.terraform': 'Terraform IaC',
      'menu.run.power_on': 'Power On',
      'menu.run.power_off': 'Power Off',
      'menu.run.suspend': 'Suspend (Pause)',
      'menu.run.resume': 'Resume',
      'menu.run.drain': 'Drain Node...',
      'menu.run.resume_node': 'Resume Node...',
      'menu.run.maintenance_enter': 'Enter Maintenance...',
      'menu.run.maintenance_exit': 'Exit Maintenance...',
      'menu.terminal.new': 'New Terminal',
      'menu.terminal.events': 'Events',
      'menu.terminal.alerts': 'Alerts',
      'menu.terminal.output': 'Output',
      'menu.terminal.cluster_ha': 'Cluster HA',
      'menu.terminal.cluster_mon': 'Cluster Monitoring',
      'menu.terminal.mon_overview': 'Operations Overview',
      'menu.terminal.mon_hosts': 'Host Health',
      'menu.terminal.security_events': 'Security Events',
      'menu.terminal.audit_log': 'Audit Log',
      'menu.accounts.users': 'Users & Roles (RBAC)',
      'menu.accounts.api_mgmt': 'API Management',
      'menu.help.welcome': 'Welcome',
      'menu.help.cmd_ref': 'Command Reference',
      'menu.help.rest_guide': 'REST API Guide',
      'menu.help.swagger': 'Swagger API',
      'menu.help.about': 'About',
      'menu.help.updates': 'Check for Updates...',

      'menu.run.drain_node': 'Drain Node...',
      'menu.run.enter_maintenance': 'Enter Maintenance...',
      'menu.run.exit_maintenance': 'Exit Maintenance...',
      'menu.terminal.new_terminal': 'New Terminal',
      'menu.terminal.cluster_monitoring': 'Cluster Monitoring',
      'menu.terminal.monitor_overview': 'Operations Overview',
      'menu.terminal.host_monitor': 'Host Health',
      'menu.accounts.users_roles': 'Users & Roles (RBAC)',
      'menu.help.cmd_reference': 'Command Reference',
      'menu.help.complete_guide': 'Complete Guide',
      'menu.help.check_updates': 'Check for Updates...',
      'menu.go.containers': 'LXC Containers',

      'status.running': 'Running',
      'status.stopped': 'Stopped',
      'status.paused': 'Paused',
      'sidebar.vm': 'VM Inventory',
      'sidebar.container': 'Containers',
      'sidebar.cluster': 'CLUSTER',
      'sidebar.infra': 'Ops',
      'toolbar.snap': 'Snapshots',
      'toolbar.settings': 'Settings',
      'toolbar.stop_selected': 'Stop Selected',
      'panel.terminal': 'Terminal',
      'panel.events': 'Events',
      'panel.alerts': 'Alerts',
      'panel.security_events': 'Security Events',
      'panel.output': 'Output',
      'msg.name_required': 'Name is required',
      'msg.no_containers': 'No containers found',
      'msg.no_snapshots': 'No snapshots to delete',
      'msg.invalid_snap_name': 'Invalid snapshot name',
      'msg.invalid_ami': 'Invalid AMI ID format',
      'msg.not_implemented': 'Not yet implemented',
      'msg.nameserver_added': 'Nameserver added',
      'msg.reboot_error': 'Reboot error',
      'msg.pool_name_required': 'Pool name required',
      'msg.theme_exported': 'Theme exported',
      'msg.theme_imported': 'Theme imported',
      'msg.invalid_theme': 'Invalid theme file',
      'msg.popup_blocked': 'Popup blocked',
      'msg.delete_all': 'Delete All',
      'msg.zen_mode': 'Zen Mode — press Escape to exit',
      'msg.clone_started': 'Clone started',
      'msg.clone_name_prompt': 'New VM name:',
      'msg.export_started': 'OVA export started',
      'msg.nic_bridge_prompt': 'Bridge (default pcvbr0):',
      'msg.mac_prompt': 'MAC address to remove:',
      'msg.snap_creating': 'Creating snapshot...',
      'msg.snap_deleting': 'Deleting snapshot...',
      'msg.snap_rolling_back': 'Rolling back snapshot...',
      'msg.vm_creating': 'Creating VM...',
      'msg.disk_resize_prompt': 'New size (GB):',
      'msg.bandwidth_prompt': 'Bandwidth (e.g. 100mbit):',
      'msg.cpu_pin_prompt': 'CPU cores (e.g. 0-3):',
      'msg.settings_saved': 'Settings saved',
      'msg.iso_mounted': 'ISO mounted',
      'msg.iso_ejected': 'ISO ejected',


      'session.mgmt': 'Session Management',
      'session.revoke': 'Force Logout',
      'apikey.mgmt': 'API Key Management',
      'apikey.create': 'New Key',
      'apikey.revoke': 'Revoke Key',
      'alert.silence': 'Alert Silence',
      'alert.silence.new': 'New Silence',
      'alert.routing': 'Alert Routing Config',
      'alert.routing.save': 'Save Routing',
      'config.reload': 'Reload Config',
      'config.reload.confirm': 'Reload daemon configuration?',
      'backup.verify': 'Verify Snapshot Integrity',
      'jobs.persistent': 'Persistent Jobs',
      'db.migration': 'DB Schema Status',
      'container.clone': 'Clone Container',
      'container.memory.detail': 'Memory Details',
      'container.health': 'Health Check',
      'pool.conninfo': 'Connection Pool Status',
      'health.deep': 'Deep Health Check',
      'empty.no_silences': 'No active silences',
      'empty.no_jobs': 'No pending jobs',
      'empty.no_apikeys': 'No API keys registered',
    }
  },







  t(key, params) {
    const lang = this._data[this._lang] || this._data.ko;
    let str = lang[key] || this._data.ko[key] || key;
    if (params) {
      Object.entries(params).forEach(([k, v]) => {
        str = str.replace(`{${k}}`, v);
      });
    }
    return str;
  },


  getLang() { return this._lang; },


  setLang(lang) {
    if (this._data[lang]) {
      this._lang = lang;
      localStorage.setItem('pcv-lang', lang);
      document.documentElement.lang = lang;
      return true;
    }
    return false;
  },


  toggle() {
    this.setLang(this._lang === 'ko' ? 'en' : 'ko');
    return this._lang;
  },


  getLanguages() {
    return Object.keys(this._data);
  }
};


function t(key, params) { return I18N.t(key, params); }


function applyI18n() {
  document.querySelectorAll('[data-i18n]').forEach(function(el) {
    var key = el.getAttribute('data-i18n');
    var translated = t(key);

    var icons = Array.prototype.slice.call(el.children).filter(function(child) {
      return child.classList && (child.classList.contains('ci-icon') || child.classList.contains('pcv-icon'));
    });
    var shortcut = el.querySelector('.shortcut');
    var badge = el.querySelector('.panel-tab-badge');
    el.textContent = translated;
    icons.forEach(function(icon) { el.insertBefore(icon, el.firstChild); });
    if (shortcut) el.appendChild(shortcut);
    if (badge) el.appendChild(badge);
  });

  document.querySelectorAll('[data-i18n-placeholder]').forEach(function(el) {
    el.placeholder = t(el.getAttribute('data-i18n-placeholder'));
  });
}


PCV.i18n = I18N;


window.I18N = I18N;
window.t = t;
window.applyI18n = applyI18n;
})(window.PCV);
