# Changelog — purecvisor-single

버전 문자열 단일 소스: `include/purecvisor/version.h` (`PCV_PRODUCT_VERSION`).
릴리스 태그: `vMAJOR.MINOR.PATCH`.

## v1.2.1 — 2026-07-11

v1.2.0 post-release 전수 감사(`docs/operations/2026-07-11-arch-audit-v120.md`)가 확증한 HIGH 6건 중 시정 3건 + 재발 방지 게이트 3건. 전부 하위호환(런타임 API/config 변경 없음; 게이트는 CI 전용). 검증: 전 커밋 `make single` 0-warning + `make test` **614/0** + `make check-all` **5게이트 PASS**(RBAC·RPC consumers·dead-exports·param-contract·json-ingress) + 격리 데몬 E2E(시정 3 + DISP-1, 다수 네거티브 컨트롤).

> ⚠️ **인증/락/파싱 행위 변경 포함** — 배포 전 `### Upgrade notes` 확인. 특히: 비번 회전 후 옛 daemon.conf 자격증명 거부, 강제 로그아웃 실동작, 외부 JSON 파싱 깊이(≤128)·크기(≤1MB) 거부.

### Security fixes (감사 확증 HIGH — 인증/락 실배선)
- **SEC-2 부트스트랩 fallback 백도어 차단** — daemon.conf 관리자 비번이 비번 회전 후에도 병렬 자격증명이 되던 백도어를 차단. fallback 판정을 에러 메시지가 아니라 사용자 RBAC DB 존재 여부(`pcv_rbac_user_exists`, 3-상태 fail-secure)로 교체 — `_ensure_admin_user`가 부팅 시 admin을 시딩하므로 회전 후 옛 비번이 fallback을 발화시키지 못한다. 진짜 첫 설치(admin 부재)에서만 복구 fallback 허용 + 감사 이벤트. E2E: 회전 후 옛 비번 401, 첫설치 복구 보존.
- **SEC-1 세션 취소 실동작** — `auth.session.revoke`가 아무도 읽지 않는 죽은 rbac blacklist에 쓰던 것을, `pcv_jwt_verify`가 실제 소비하는 라이브 jwt blacklist(`pcv_jwt_blacklist_add`)에 배선. 강제 로그아웃이 실제로 토큰을 무효화한다. 죽은 `pcv_rbac_session_revoke`/`_is_revoked` 삭제. E2E: revoke 후 토큰 401.
- **CMP-1 VM 락 교차 unlock 차단** — 공유 콜백 `vm_action_callback`이 락을 획득하지 않은 op(pause/resume/limit)에서도 무조건 `unlock_vm_operation`을 호출해 동시 `vm.delete`의 락을 지우던 결함(AF-P1 직렬화 무력화)을, `holds_lock` 플래그로 조건부화(락을 획득하는 stop만 해제). 결정적 E2E: DELETING 락 보유 중 vm.limit → 락 잔존.
- **DISP-1 원격 미인증 크래시 구조적 해소** — 외부 입력 JSON 파싱을 단일 초크포인트 래퍼 `pcv_rpc_parse_guarded`(깊이 ≤128 + 크기 ≤1MB 선검사 후 파싱)로 좁히고, WS 사전인증 파싱을 이전. 깊게 중첩된 미인증 텍스트 프레임이 파싱 전 거부되어 스택 오버플로우 크래시 불가. E2E 네거티브 컨트롤: 수정 되돌리면 10만 중첩 프레임에 SIGSEGV, 수정본은 데몬 생존.

### CI 계약 게이트 (재발 방지 — 정적/래칫, 런타임 비영향)
- **check-dead-exports** — 헤더 선언 비-static `pcv_*` 함수 중 `.c` 사용처 0(정의만)인 dead export 노출·신규 차단(baseline 153). SEC-1형 "배선 안 된 안전 함수" 재발 차단.
- **check-rpc-param-contract** — RPC param-key 계약(진리원 `contracts/rpc_params.json`, 래칫 `contracts/rpc_param_baseline.json`). CLI/TUI/FE 전송키 ⊇ 핸들러 required 검사로 "메서드는 맞지만 param 불일치→-32602 무동작" 클래스(감사 CLI-1~16) 차단.
- **check-json-ingress** — 데몬 경계 5파일의 외부 JSON 파싱이 `pcv_rpc_parse_guarded` 경유(또는 `PCV_PARSE_TRUSTED` waiver)인지 검사(baseline 0). DISP-1형 깊이가드 누락 재발 차단.
- 세 게이트 모두 `make check-all` + `scripts/pre-commit`에 통합.

### Upgrade notes
- **비번 회전 후 옛 자격증명 무효화**: SEC-2 이후 daemon.conf `[daemon] admin_password`는 admin이 RBAC DB에 부재일 때(진짜 첫 설치)만 fallback 복구에 쓰인다. 회전(change_password)한 배포에서는 옛 daemon.conf 비번으로 로그인이 거부(401)된다 — 정상. bootstrap admin 자체 회전은 지원 API 부재(REST `/auth/password` 403), daemon.conf 편집으로 관리(후속 검토 대상).
- **강제 로그아웃 실동작**: SEC-1 이후 `auth.session.revoke`가 토큰을 즉시 무효화한다(TTL 900s). 기존에 무동작에 의존한 흐름은 없음(원래 버그).
- **외부 JSON 파싱 거부**: 깊이 >128 또는 크기 >1MB인 외부 프레임/바디는 파싱 전 거부된다(WS는 조용히 무시, REST/dispatcher는 에러 응답). 정상 페이로드는 무영향.

### 알려진 잔여 (후속 트랙)
- SEC-2 fallback 성공 감사가 `pcv_jwt_sign` 성공 확인 전 `result=ok`를 기록(fail-closed라 토큰 미발급 시 이중 감사행만; 계획 스니펫이 이 위치 명시 — 후속 판단).
- param-contract 게이트: vm.eject·device.disk.attach는 문서화된 false-negative(WARN), FE param 추출 미구현(현재 FE 소비 메서드 0). dead-export 게이트: src-scope 한정(tests 미포함, 의도).
- 별도 트랙: NET-1(dpdk.bind), 게이트 #4(안전통제 효과 테스트), refresh-remint 차단, MED/LOW findings.

## v1.2.0 — 2026-07-11

8도메인 전수 아키텍처 감사(`arch-audit-final`)의 Tier0~2 시정 + 후속 판단 트랜치 통합. **데몬 라인 첫 대규모 릴리스** — "정의만 되고 실제로는 동작하지 않던" 다수의 안전 통제를 실배선했다. 전부 하위호환(신규 config는 기본값 존재, apikey `client_name` 레거시 fallback 유지). 검증: 전 커밋 `make single` 0-warning + `make test` **607/0** + `make check-all` PASS + 실-VM/ZFS/재부팅 E2E(AF-1/AF-S4/AF-N2·N3/apikey 계약).

> ⚠️ **동작 변경 포함 릴리스** — 배포 전 `### Upgrade notes` 확인 필수. 특히 self-healing(기본 `dry_run`)·API 키 만료 집행·백업 리텐션·apikey DB 마이그레이션.

### Security hardening (Tier0 — 원격 크래시·RCE·데이터 파괴 차단)
- **미인증 요청 크래시 방어** — 루트 타입·method NULL 가드(`-32600`), 파싱 실패 `-32700`. 인증 전 임의 페이로드로 데몬을 죽일 수 있던 표면 차단.
- **JSON 중첩 깊이 폭탄 차단** — `pcv_rpc_json_depth_ok`(≤128)를 dispatcher·`_parse_body`에 배선.
- **컨테이너 NIC 셸인젝션 RCE 차단** — `container.nic` 4핸들러의 vm_name/bridge/iface 검증(SEC-F1).
- **ZFS 데이터 파괴 방어** — zvol 재귀 파괴 화이트리스트+타입 확인(AF-S1), 클라우드 데이터안전 3종(AF-S5 import 덮어쓰기 거부·AF-S3 실행중 export 거부·AF-S2 near-live 거짓성공 정정).

### Self-Healing / AI Ops
- **VM 자동 재시작 실배선 (AF-1)** — 크래시 이벤트(`CRASHED`/비정상 `STOPPED`) → UUID 조회 → running-guard → `virDomainCreate`. 이전에는 콜체인에 VM 타깃이 관통되지 않아 완전 no-op이었다. 정상 종료(graceful shutdown)는 크래시 게이트(A6-6)에 걸러져 오재시작하지 않는다. **안전판: `[ai] mode` 기본 `dry_run`** — 실 재시작은 `active` 명시 전환 시에만.
- **재시작 서킷브레이커 (신규)** — 반복 재시작 실패를 VM 단위로 격리 차단. 연속 실패 임계 초과 → OPEN(cooldown 동안 skip, audit `result=skipped reason=breaker-open`) → HALF_OPEN 1프로브 → 성공 시 복귀. 무한 재시작 루프 방지.
- **AI 합의 최소 정족수 (A6-7)** — 이상탐지 응답자 1명일 때 1/1=100% 합의로 조치를 승격하던 결함 수정. `[ai] min_quorum`(기본 2) 미만이면 저신뢰로 판정해 알림만.
- **알림 음소거 실집행 (AF-O2)** — `alert.silence`가 정의만 되고 발화 경로에서 확인되지 않아 무동작이던 것을, 발화 진입부에서 음소거 확인 후 건너뛰도록 배선.
- **이상탐지 메트릭·파서 정합 (AF-O1)** — host cpu/mem 사용률을 레지스트리에 push(죽은 메트릭에 걸려 트리거 자체가 안 되던 정책 활성화) + 파서 `#` 주석줄 오매칭 수정. AI 캐시 키를 cpu/mem 사용률만 양자화(단조증가 카운터 포함으로 상시 miss→유료 API 재질의하던 것 해소, A6-8).
- **self-healing 셔터다운 배선 (AF-3)**.

### Storage / Backup
- **백업 스냅샷 리텐션 (AF-S4) — 풀 잠식 방지** — `s3-`/`incr-` 스냅샷이 리텐션 없이 무한 누적→ZFS 풀 잠식→전 VM I/O 정지 위험이던 것을, 최신 N개(`[backup] s3_retention_count`/`incr_retention_count`, 기본 7)만 보존하고 초과분 자동 prune. 증분은 base 보존 위해 send 후, S3는 생성 직후 prune.
- **VM 오퍼레이션 락 (AF-P1)** — create/delete/tuning(set_memory·set_vcpu)/snapshot(create·rollback·delete·delete_all)에 VM 단위 직렬화 락. 무락 경합 + create의 무제한 `zfs list`가 ZFS hang 시 전 데몬 프리즈를 유발할 수 있던 위험 제거.
- **create dup-check 논블로킹 (NEW-2)** — 메인스레드 `zfs list`에 타임아웃(hang → 데몬 프리즈 차단).
- **S3 자격증명 격리 (M-9)** — AWS 자격증명을 전역 `environ` 오염 없이 호출 단위 `envp`로 전달.
- **iSCSI 안전화 (M-2/M-3)** — 셸 연산자 리터럴 제거 + 삭제 반환값 반영.

### Network
- **QoS·오버레이 재수화 (AF-N2/N3)** — tc 대역제한·VXLAN 오버레이가 `/var/run`(tmpfs)라 재부팅 시 소실되던 것을 `/var/lib`(비휘발)로 이전 + 부팅 스테이지 자동 복원(멱등 재적용). 실 재부팅 E2E로 OVS/ovsdb 기동 후 재적용 정합 실증.

### Auth / Security
- **API 키 스키마 단일화 (F8)** — 이중 스키마(schema#1/#2)를 schema#2 canonical로 통합 + 멱등 마이그레이션(dead 함수 4 제거).
- **API 키 생성 계약 정합 + 만료 집행 (apikey.create)** — 생성 파라미터를 `name`(정본)+`client_name`(레거시 fallback)+`description`+`expires_at`(epoch)로 확장, description/expires_at 컬럼 멱등 추가. **만료 집행**: 인증 술어 `expires_at = 0 OR expires_at > now`(0=무기한, 만료 초과 시 거부) — verify·role 조회 양 경로. FE 생성 폼 name 필드 + 전송 페이로드 정합(실브라우저 검증).
- **OPERATOR 권한 상승 차단 (SEC-F3)** — 호스트 파괴 가능 메서드 17개를 ADMIN 정책에 매핑(FE 실버튼 노출이라 시급).
- **보안 액션 TTL 집행 (NEW-A2)** — 만료된 pending HIPS 액션이 무기한 승인 가능하던 것 차단.

### Contract / API
- **FE→BE 계약 라우트 배선 (AF-C1)** — FE가 호출하지만 404/오라우팅이던 REST 10라우트 브리지 + apikey REST(list/revoke, F8 통합 후 활성).
- **미구현 RPC/CLI 핸들러 배선 (AF-C2)** — numa.info/autostart/sla.report/capacity.forecast 실배선(`-32601` 해소), schedule/billing은 backing 부재라 `not_available` 정직 스텁.
- **CLI 백업 커맨드 복구** — `pcvctl backup incremental`·`export-s3`·`verify`·`replicate`가 파라미터 키 불일치로 **도입 이래 무동작**(`-32602`)이던 것을 복구(E2E 발견).
- **alert DLQ 노출 (AF-C4 발굴)** — `alert.dlq.list`/`retry` RPC 등록 + REST GET.

### Observability / Audit
- **fire-and-forget WS 마샬링 (A2-2)** — 워커 스레드에서 발화하던 9개 표면의 WS 브로드캐스트를 메인 컨텍스트로 마샬링(libsoup 스레드 어피니티 위반 해소).
- **audit 이벤트 발생시각 분리 (A6-9)** — `event_ts`(발생시각)를 기록시각과 별도 기록.
- **알림 에스컬레이션 (A6-3/A6-4)** — 에스컬레이션 JSON 래핑 + webhook_secret/crit_url 로드.
- **process_monitor PID 누수 (A6-10)** 수정.

### Internal / Cleanup
- **storage_tier dead 함수 전면삭제 (M-1)** — 호출부 0인 9함수(list/info/create/auto_select/migrate/qos set·get·delete + shutdown)·dead 헬퍼·미사용 include 제거(≈410줄 감축). init(살아있는 유일 경로) 보존.
- **Docker/OCI·Terraform 프론트 잔재 완전 제거 (NEW-D1)** — 불완전 제거로 `#docker` 직접 진입 시 404이던 미배선 route.
- storage.tier.set 고아 정책 제거(M-1, 모듈 keep) / container.js IIFE 밖 재대입 3줄 삭제(AF-F3).

### Tooling / CI
- **RPC 소비⊆등록 계약 게이트 (AF-C4)** — `check_rpc_consumers.py`: FE/CLI/TUI가 소비하는 모든 RPC가 등록됐는지 검사(계약 파손=404/-32601 재발 차단). `make check-all` 집계(RBAC+RPC) + pre-commit 훅 승격 — 파손 유발 커밋 자체 차단.

### Config (신규 키 — 전부 기본값 존재, 무설정 시 종전 동작 유지)
- `[ai] mode`(기본 `dry_run`) — self-healing 실행 모드. `active`로 전환해야 실제 조치.
- `[ai] min_quorum`(기본 2) — AI 합의 최소 응답자 수.
- `[ai] restart_breaker_threshold`(기본 3) / `restart_breaker_cooldown_sec`(기본 1800) — 재시작 서킷브레이커.
- `[backup] s3_retention_count` / `incr_retention_count`(각 기본 7) — 백업 스냅샷 보존 개수.
- `[overlay] tunnel_ip` — 오버레이 활성화(미설정 시 오버레이 비활성).

### Upgrade notes
- **DB 마이그레이션은 전진 전용** — api_keys 스키마 단일화(F8) + description/expires_at 컬럼 추가는 멱등 ALTER로 안전하나 되돌릴 수 없다. **배포 전 `rbac.db` 백업 권장**. 구버전(1.1.x) 데몬으로 롤백 시 새 컬럼을 무시(기능 저하만, 크래시 아님).
- **API 키 만료 집행** — 기존 무기한 키는 `expires_at=0`이라 영향 없음. 만료일이 설정된 키는 이제 실제로 만료 후 거부되므로, 만료됐지만 사용 중이던 키가 있으면 재발급 필요.
- **self-healing은 기본 dry_run** — 자동 재시작을 원하면 `[ai] mode=active` 전환. 켤 때 서킷브레이커 기본값(3회/1800초)으로 재부팅 루프가 차단됨을 확인.
- **백업 리텐션 기본 7** — 기존에 8개 이상 `s3-`/`incr-` 스냅샷을 쌓아둔 VM은 다음 백업 실행 시 오래된 것부터 7개로 정리된다. 더 긴 보존이 필요하면 배포 전 config로 상향.
- 데몬 재시작(다운타임 수 초) 수반. `.deb` postinst는 재시작하지 않으므로 `systemctl restart purecvisorsd` 필수.

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
