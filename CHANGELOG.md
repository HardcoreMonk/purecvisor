# Changelog — purecvisor-single

버전 문자열 단일 소스: `include/purecvisor/version.h` (`PCV_PRODUCT_VERSION`).
릴리스 태그: `vMAJOR.MINOR.PATCH`.

## v1.1.5 — 2026-07-10

깨진 모달 6곳 기능 복원 + deb 패키징 style.css 누락 핫픽스 패치 릴리스 (데몬 코드 무변경 — UI·빌드 자산만). 래칫 **15→9**.

### UI / Fixes
- 사장된 3-인자 `showModal(title, body, cb)` 모달 6곳 복원 — 시그니처가 존재한 적 없어 제목만 렌더되던 깨진 기능을 노드 body+콜백 배선으로 실동작 복원: container 클론·메모리 상세, advanced 백업 검증, monitor 음소거 생성, accounts API 키 생성×2. 실브라우저 프로브로 제목+폼입력+확인/취소 렌더 검증.

### UI / DOM-safe (zone ADR-013)
- ui.js `createDataTable`/`renderSortableTable` 노드 반환화(셀 Node|배열|문자열 오버로드, `_dtCellText` 검색·CSV 정합) — ui.js innerHTML 3→1(레거시 `_setModalBody`만). accounts DataTable 소비부 4셀 노드화, help.js 정적 대형 템플릿 4곳(helppage/serviceguide/restguide/apihelp) 노드화(전사 353/353 `_L`쌍 일치).

### Packaging
- **deb UI 스테이징 `ui/*.css` 누락 보정** — 1.1.1~1.1.4 전 deb에서 index.html이 참조하는 `style.css` 미포함(신규 머신 deb 설치 시 UI 무스타일 렌더). cp glob이 `2>/dev/null || true`로 실패를 삼켜 4개 릴리스 동안 무증상이던 구조에 필수 자산 6종(index/style/app.bundle/sw/i18n/manifest) 스테이징 검증 게이트 추가 — 누락 시 빌드 즉시 실패.
- 기배포 호스트는 구버전 잔존 `style.css`가 결함을 가리므로(실측: 4/30·5/7자 stale CSS를 v1.1.4 UI와 조합 서빙) v1.1.5 설치로 정정 — SW `CACHE_NAME` bump가 클라이언트 프리캐시의 stale CSS도 함께 해소.

## v1.1.4 — 2026-07-08

innerHTML→DOM-safe 전환 에픽 6~8차 완결 패치 릴리스 (데몬 코드 무변경 — UI·빌드 자산만). 래칫 **165→15** (에픽 누계 422→15, −96%).

### UI / Security hardening (zone ADR-013)
- 6차: advanced·accounts·network·app.js 4모듈 HN 노드화(H.* 호출 178곳) — renderDashboard·renderOvn 대형 렌더 포함. 골든 스냅샷 diff 34/36 바이트 동일 검증.
- 7차: 잔여 9모듈(container·vm-lifecycle·nav·vm·vm-console·vm-guest·storage·cloud·security) — VM 목록 중심 렌더(스파크라인·roving tabindex 보존), 전역 크롬(브레드크럼·알림센터·커맨드팔레트), security 렌더 체인 통째 노드화. body 단위 골든 diff 32/36 동일.
- 8차: **showModal 노드 계약** — body `Node|배열` 표준(문자열은 레거시 단일 엔트리), 모달 스택 라이브 노드 보존(중첩 복원 시 입력 상태 유지), 소비부 55곳 전환, hw* 설정 체인 9종 노드 반환화, Modal.show 스캐폴드 노드화.
- 잔존 15: 레거시 문자열 엔트리 2, DataTable 문자열 컴포넌트 2, help 정적 템플릿 4, 의도적 직렬화-복사 관용구 3(split-view·popup — 리스너 소거 계약) 등.

### UI / Design
- 대시보드 아이콘 coolicons SVG(ci-*) 통일 — 컬러/모노크롬 이모지 혼용(OS 폰트 의존) 제거, 크기 위계(타일 30px/칩·카드 상속)와 타일 색상 아이덴티티 유지.

### UI / Fixes
- Terraform textarea placeholder 수복 — 비이스케이프 따옴표로 placeholder 절단+쓰레기 속성 8개가 생기던 malformed DOM.
- (기지 이슈 목록화) 사장된 3-인자 `showModal(title, body, cb)` 호출 6곳 — 컨테이너 클론/메모리 상세, 백업 검증, 알림 음소거 생성, API 키 생성 모달이 제목만 렌더되던 기존 깨진 기능. 복원은 후속 배치.

### Tooling / Docs
- verify 스킬: body 단위 골든 diff(속성 정렬 정준화)·비로그인 스윕 함정·모달 프로브 레시피.
- L4 규약: showModal 노드 표준, ui.js↔uxlib 로드 순서, HN/msg 표준 경로.

---

## v1.1.3 — 2026-07-07

innerHTML→DOM-safe 전환 에픽 1~5차 배치 패치 릴리스 (데몬 코드 무변경 — UI·빌드 자산만).

### UI / Security hardening (zone ADR-013)
- innerHTML 실사용 사이트 래칫 **422→165 (−61%)** — api.js·uxlib.js·monitor.js 완전 클린.
  - 1차: 꼬리 7모듈 19사이트. 2차: storage.js 14사이트 + showCtxMenu plain-text 계약. 3차: 공유 헬퍼 노드화(showSkeleton/emptyStatePro, 소비처 35건).
  - 4차: 상태 메시지 표준 헬퍼 `PCV.uxlib.msg/setMsg` 신설 + 11모듈 메시지 원라이너 164사이트 일소 (escapeHtml 이중 이스케이프 제거, 엔티티→글리프).
  - 5차: H 동형 노드 빌더 `HN`(card/row/badge/grid/section/statCard) 신설 + monitor.js 전면 노드화(25→0 — `h +=` 누적 렌더 17싱크, 내부 헬퍼 8종, SVG는 createElementNS). 골든 스냅샷 diff 36탭 구조 동일 검증.

### UI / Fixes
- WS 재연결 배너 Retry가 시도 카운터를 리셋하지 못해 배너가 즉시 재생성되던 버그 수정 (module-scope 카운터 직접 리셋).
- `H.statCard` 정의 누락 수복 — 커넥션 풀/DB 스키마 패널이 API 성공 시에도 항상 '로드 실패'를 표시하던 문제(try/catch가 TypeError 은폐) 해소.

### Tooling / Docs
- verify 스킬에 골든 스냅샷 diff 레시피(전환 전후 36탭 렌더 직렬화 비교)·evaluate 재시도 래퍼 추가.
- L4 CLAUDE.md에 DOM-safe 표준 상위 경로(msg/setMsg·HN)와 모듈 로드 순서 함정 명문화.

---

## v1.1.2 — 2026-07-07

프론트엔드 전수조사 백로그 #1~#5 완결 패치 릴리스 (데몬 코드 무변경 — UI·빌드 자산만).

### UI / Accessibility
- 메뉴바 WAI-ARIA roving tabindex 완성: top-level 8개 단일 탭 스톱(활성만 tabindex=0), 드롭다운 `.mi` 45개 JS 일괄 tabindex=-1, Enter/Space/↓ 열기→첫 항목 포커스, ↑/↓ 순환, 드롭다운 내 ←/→ 인접 메뉴 이동, Esc 닫기+복귀. activity-bar 아이콘 6개 키보드 도달 가능.
- vm.js F1 전역 keydown 위젯 가드(defaultPrevented + 메뉴바/role 위젯 제외) — 메뉴 Enter가 VM Summary로 화면을 가로채던 충돌 근본 해소. j/k/Enter VM 탐색 불변.

### UI / Architecture
- vm.js(2643 LOC)를 vm / vm-console / vm-lifecycle / vm-guest 4모듈로 분할 — 조각별 완결 IIFE(ADR-0013), export 재배치(PCV.vm 41키·window shim 99개 보존), 본문 순수 이동.
- DOM-safe 기반(zone ADR-013): `PCV.uxlib.el/frag/clearEl` 빌더(HTML 파싱 경로 없음), `npm run lint:domsafe` 가시성 래칫, 프로젝트 CLAUDE.md invariant 신설.

### Tooling / Docs
- UI 실브라우저 검증 레시피 `.claude/skills/verify/SKILL.md` (목 API + Playwright).
- 배포 handoff: `docs/operations/2026-07-07-frontend-batch-deploy-handoff.md`.

---

## v1.1.1 — 2026-07-06

패키징·빌드 위생 패치 릴리스 (신규 기능 없음). 런타임 버전 표시를 full semver(1.1.1)로 정합.

### Build / Packaging
- `make deb` 타깃 통합: `packaging/deb/build-deb.sh` 로 release 바이너리+UI+systemd 유닛+config sample 을 `dist/purecvisor-single_<ver>_amd64.deb` 로 조립. 버전은 version.h 파생, Depends 는 ldd→dpkg-query 자동 산출.
- UI 번들 결정화: `ui-bundle` 헤더의 `date` 타임스탬프 제거 → version.h 파생 버전+LOC. `make ui-bundle`/`make deb` 반복 실행 시 워킹 트리 diff 0.
- 빌드 의존 추적: Makefile `.d`(`-MMD`) 를 `-include` 하여 헤더(version.h 등) 변경이 증분 빌드에 반영.

### Fixes
- 기동 배너 버전을 리터럴("v1.0")에서 `PCV_PRODUCT_VERSION` 매크로로 (CLI/REST 와 일치).

### Docs
- GUIDE 2.2 `.deb` 바이너리 설치 절차(방법 A) 신설.

---

## v1.1.0 — 2026-07-06

1.0 안정화 사이클 결과를 minor 릴리스로 패키징. 신규 네트워크 기능 + dogfooding 수정 + 하드닝/operate 잔여 소진.
상세 근거: `docs/operations/2026-07-06-session-split-1.0-stabilization-handoff.md`, `docs/operations/2026-07-05-vp-series-release-handoff.md`, `docs/operations/2026-07-05-vm-provisioning-dogfooding-findings.md`.

### Features
- **관리형 기본 NAT 네트워크 `pcvnat0`** (VP-1): 데몬 기동 시 브릿지+NAT(nftables)+DHCP/DNS(dnsmasq)를 멱등 보장. `vm create` 에서 `--network_bridge` 미지정 시 자동 부착, `none` 으로 opt-out. 신규 `[network]` config 섹션(`default_bridge`/`default_subnet`/`default_ensure`/`firewall_integration`).
- **호스트 방화벽 자동 공존** (VP-6): UFW/iptables-DROP 감지 후 게스트 포워딩·DHCP·DNS 경로를 자동 개통(AUDIT 추적). `[network] firewall_integration = auto|off`. firewalld 는 감지·경고.
- **guest-agent 채널 기본 포함** (VP-2): `vm create` 도메인 XML 에 virtio-serial 채널을 무조건 추가 → `guest-ping`/`guest-exec`/`guest-shutdown` 이 self-created VM 에서 동작.
- **SG scoped-nft 재설계**: bridge `pcv_sg` 스코프 체인 + vnet 캐시(라이프사이클 evict/주기 resync/NIC 핫플러그 훅) + DB fail-closed 가드. followups(I2-R1~R3, R4~R12, M-2~M-10) 전량 소진.

### Fixes
- guest-exec exitcode 신뢰성 (VP-3): CLI 조회 키 정정 + 데몬 `guest-exec-status` 폴링 루프(exited 판정).
- CLI 견고성: `vm delete-status` 값-노드 파싱(VP-4), `network list` 키 `name`/`ip_cidr`(VP-5), `security-group rule add` direction `in`/`out` 별칭 정규화(VP-7), `vm create --help/-h` 가드 + 플래그형 이름 거부(VP-8).
- dnsmasq 데몬 재시작 관통 생존 (VP-6/B-3): KillMode=process + 생존 게이트 `kill(0)`→`/proc/<pid>/comm`(privdrop CAP_KILL 상실 대응). DHCP 공백 0.
- 기본 네트워크 DHCP 를 DNS 포워더 포함으로 기동 — 게스트 이름 해석 가능.

### Hardening
- launcher shutdown TOCTOU 를 spawn 뮤텍스로 동기화 (B-1).
- `evidence_json` 오버플로 가드를 프로듀서+역직렬화 site 공용 함수로 통일 (B-2).

### Ops / Docs
- `daemon.conf` `[network]`/`[security_group]` 레퍼런스 + 구 `[vm] default_bridge` 폐기 마이그레이션 노트 (B-4).
- 무인증 `/api/v1/health` liveness 모니터링 연결 + `rest_port` 충돌(REST 다운) 복구 (B-6).
- 테스트 config 격리 `PCV_CONFIG_PATH` (B-5).

### Known / Deferred
- VP-9 (클라우드 이미지 lsilogic SCSI 컨트롤러): 표준 pcvctl 경로 무영향 — 2.0 클라우드 이미지 지원 시 virtio-scsi 기본 검토.

### Migration
- `daemon.conf` 의 구 `[vm] default_bridge` 는 폐기 → `[network] default_bridge`(기본 `pcvnat0`)로 이관. 구 키는 조용히 무시됨.
- 신규 배포에서 호스트 방화벽 자동 개통이 싫으면 `[network] firewall_integration = off`.
