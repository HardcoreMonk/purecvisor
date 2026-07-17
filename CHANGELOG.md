# Changelog — purecvisor-single

버전 문자열 단일 소스: `include/purecvisor/version.h` (`PCV_PRODUCT_VERSION`).
릴리스 태그: `vMAJOR.MINOR.PATCH`.

## v1.3.4 — 2026-07-17

잔여 하드닝 **G1-③ 후속** (PATCH) — AppArmor 프로필 **enforce-준비 보정**. 데몬/패키징 로직 무변경(프로필 규칙만, 여전히 **complain 모드**). enforce 검증 중 발견한 갭을 닫아 프로필을 enforce-ready로 만든 것. 검증: `apparmor_parser -Q` 문법 OK + 실서버 complain 리로드 후 **신규 would-deny 0**(기존 16,921→0) + 소스 spawn 전량 커버리지 확인.

### 하드닝 (enforce 검증 findings)
- **런타임 관측 갭(실서버 7h complain)**: `ebpf-telem` 스레드의 `/etc/libvirt/libvirt.conf` 읽기(~5s 주기, 16,921건)가 프로필 미허용 → enforce 시 DENIED 되었을 것. `/etc/libvirt/** r,` 추가. 기동 시 `/usr/bin/kmod` exec(modprobe 심링크 해소) 미허용 → `/{usr/,}bin/kmod Ux,` 추가.
- **정적 spawn 감사 갭(소스 전수)**: idle 관측이 못 본 write-heavy 경로의 exec 전환 9종 누락 발견 — libguestfs 4종(`virt-sysprep`·`virt-customize`·`virt-filesystems`·`guestfish`, VM clone/guest-reset ADR-0023), `curl`(이미지 다운로드), `gzip`/`zstd`(압축), `ssh`(마이그레이션). enforce 시 clone/backup/migrate 파손했을 것. `virt-install`을 `virt-* Ux` glob으로 확장 + 나머지 Ux 추가. **소스 spawn 55종 전량 커버(누락 0) 확인.**

### Upgrade notes
- **무영향(기본)**: 프로필은 여전히 complain(비차단). 데몬 계약·동작 무변경. 본 릴리스는 enforce 전환의 *전제*(프로필 완비)만 확보 — 실제 enforce 전환은 write-heavy 워크로드 complain 관측(무-DENIED)까지 완료 후 운영자 opt-in. 절차: `docs/operations/2026-07-17-apparmor-profile.md`.

## v1.3.3 — 2026-07-17

잔여 하드닝 **G1 ①②③** (PATCH) — 호스트 데몬 MAC 하드닝. 데몬 로직 무변경(로그 문구만). 검증: `make single` 0-warn + `make check-all` **20게이트** + `apparmor_parser -Q` exit 0.

### 하드닝
- **① 정직-로그** (`pcv_privdrop.c`): 권한격하 요약 로그가 `nnp=OK`로 표기해 NNP 활성 오인 소지였던 것을 `nnp=disabled(LXC-AppArmor) seccomp=disabled(LXC-inherit)`로 정정.
- **② ADR-0026**: seccomp/NNP 비활성 수용 + capabilities + AppArmor 채택 결정 명문화(LXC 상속-파손·NNP↔AppArmor 충돌·업계 관행[libvirt/Proxmox] — insecure-by-design → challenge+문서화, ADR-0025 규율).
- **③ AppArmor MAC 프로필** (`packaging/apparmor/usr.local.bin.purecvisorsd`): 접근 표면 전수 스윕(spawn 바이너리·경로·소켓·capability), 자식(qemu/lxc-start) `Ux` 전환으로 seccomp 상속-파손 회피. **complain 모드 배포**(비차단·기본 무영향, postinst complain 로드-only + `|| true` 가드), 실서버 검증 후 opt-in enforce. 검증 문서 `docs/operations/2026-07-17-apparmor-profile.md`.

### Upgrade notes
- **무영향(기본)**: AppArmor 프로필 complain 모드(비차단). 데몬 계약·동작 무변경(로그 문구만). enforce 전환은 실서버 complain 위반 로그 검증(무-DENIED 확인) 후 운영자 opt-in.

## v1.3.2 — 2026-07-17

잔여 하드닝 **C1+C2** (PATCH) — TLS 하드닝(mTLS 클라이언트 인증서 검증 + TLS 최소버전 고정). 둘 다 **opt-in·기본값 하위호환**(기본 배포 TLS off·nginx 종단이라 무영향). 검증: `make single` 0-warn + `make test` **673/0** + `make check-all` **20게이트** + 반사실 RED + 기동 라이브 확인.

### 보안 시정
- **mTLS 클라이언트 인증서 검증 (C1, A02 / ASVS V12)**: `[tls] client_auth`(`none`/`request`/`require`, 기본 `none`) — `request`/`require` + `ca_path` 설정 시 CA 검증 DB(`g_tls_file_database`) + auth-mode(REQUESTED/REQUIRED) 배선. 기존엔 `ca_path` 저장만·미검증(평가 A02). `require`+CA 세팅 실패 시 **fail-secure**(무검증 mTLS로 HTTPS 미개시).
- **TLS 최소버전 고정 (C2, A02 / V11)**: `[tls] min_version`(`1.2`/`1.3`, 기본 `1.2`) → GnuTLS priority(`G_TLS_GNUTLS_PRIORITY`) 고정(config 우선 overwrite) → **SSL3/TLS1.0/1.1 차단**(데몬 TLS + 아웃바운드 webhook/AI).

### 재발방지 게이트 (ADR-0025 반사실)
- `check-mtls-wiring` · `check-tls-min-version` (check-all 18→**20**) 반사실 RED.

### Upgrade notes
- **무영향(기본)**: `client_auth` 기본 none · `min_version` 1.2(범용). 아웃바운드 TLS도 1.2+로 협상(SSL3/TLS1.0/1.1 사용처 없어 무영향).

## v1.3.1 — 2026-07-17

잔여 하드닝 우선순위 **B1** (PATCH) — 컨테이너 IDOR 시정(operator owner-scope). OWASP/ISMS-P 평가 A01 잔여(owner-scope가 `vm.*`만 커버 → operator 교차테넌트 컨테이너 조작). 검증: `make single` 0-warn + `make test` **673/0** + `make check-all` **18게이트** + 반사실 RED.

### 보안 시정
- **컨테이너 operator owner-scope (A01 / ASVS V8)**: VM의 operator owner-scope(자기 소유만 조작·소유자 부재 시 deny·admin 복구)를 **컨테이너에 미러**. `container.start`/`stop`/`clone`에 소유자 검사 — `container.create` 시 caller sub를 `<lxc_path>/<name>/purecvisor.owner`에 스탬프, operator는 소유 일치 시만 조작(admin 우회). clone은 **source owner** 검사. `container.clone`을 RBAC 정책에 **명시적 operator 등록**(미등록 시 경로별 default 상이[UDS=VIEWER/REST=ADMIN] 해소, `vm.clone` 동형).
- 게이트 `check-container-owner-scope`(check-all 17→**18**) 반사실 RED. owner 저장은 별도 순수 TU `lxc_owner.c`(데몬+test 공통 링크).

### Upgrade notes
- **행동 변화**: operator 계정은 이제 **자기 소유(생성) 컨테이너만** start/stop/clone 가능. **기존 컨테이너(`.owner` 부재)는 operator 접근 상실 → admin 전용**(VM 소유자-부재와 동일 fail-secure). admin 재생성 또는 owner 재스탬프로 복구. admin은 무영향.

## v1.3.0 — 2026-07-16

보안 시정 트랙 **Wave C** (MINOR — **배포 계약 변경**). OWASP/ISMS-P 평가의 최고위험 갭 2건(로컬 UDS 접근통제·전송 강제) + 게이트 2종(check-all 15→**17**). 배포 **P2**. **Upgrade notes 필독.** 검증: `make single` 0-warn + `make test` **668/0** + `make check-all` **17게이트** + 반사실 RED + pcvctl/nginx 라이브 실증.

### 보안 시정
- **UDS 로컬 접근통제 (A01 / ASVS V8)**: UDS 소켓을 self-defending으로 — 소켓 `0666→0660`, 디렉토리 `0700`, 각 연결에 **SO_PEERCRED 피어 UID 검증**(비-root 거부, fail-closed). io_uring + GSocketService **양 accept 경로** 적용. **접근 게이트 전용**(role 미설정 → Wave B gRPC bounded role 보존). 기존엔 외부 systemd `UMask`에만 의존 → 이제 데몬 자체 방어.
- **전송 강제 (A02 / V12)**: 평문 HTTP 바인딩이 config `[server] bind_plaintext`(**기본 `loopback`**)를 존중 → 기본 `127.0.0.1`만 바인딩(nginx 루프백·로컬 probe 도달, **외부 평문 차단**). TLS는 `0.0.0.0` 유지(외부 HTTPS). 외부 평문 필요 시 `bind_plaintext=all`.

### 재발방지 게이트 (ADR-0025 반사실)
- `check-uds-authz` · `check-transport-bind`를 `check-all`(15→**17**) + pre-commit 편입, 반사실 RED.

### Upgrade notes ⚠️ 배포 계약 변경
- **비-root UDS 클라이언트 거부**: `pcvctl`/gRPC는 root(sudo)라 **무영향**. 비-root 로컬 프로세스의 UDS 접근은 차단(P2 의도).
- **평문 `:8080` 기본 loopback**: 외부에서 직접 `http://<host>:8080` 접근 차단. **TLS/nginx(`:443`) 경유로 전환** 필요. 외부 평문 유지가 필요하면 `daemon.conf [server] bind_plaintext=all`. **크로스호스트 `:8080` health probe는 `https:443`로 이관 필요.**

## v1.2.9 — 2026-07-16

보안 시정 트랙 **Wave B**(PATCH) — OWASP/ISMS-P 평가의 중위험 갭 4건 + 재발방지 게이트 4종(check-all 11→**15**). 배포 **P2** 기준. 데몬 wire 계약 무변경 · 전 마이그레이션 투명(하위호환). 검증: `make single` 0-warn + `make test` **668/0** + `make check-all` **15게이트** + 반사실 RED.

### 보안 시정
- **gRPC RBAC (A01 / ASVS V8)**: gRPC를 암묵 ADMIN에서 **인증 + bounded role**로 강등 — 무토큰 기동 거부(`[grpc] enabled`인데 `auth_token` 미설정 시 gRPC 미기동), UDS 전달 시 `_pcv_caller_role`(`[grpc] role`, 기본 `operator`)+`_pcv_caller_sub="grpc"` 주입(클라이언트 위조 키 제거). *(잔여: 악의적 UDS 클라이언트 role 위조는 Wave C/Item 1.)*
- **SSRF allowlist (A10 / V4)**: 아웃바운드(webhook/AI/S3 endpoint)에 **resolve된 실주소 기준** 링크로컬(169.254 클라우드 메타데이터·fe80) 차단 — substring denylist 우회·인코딩(십진/16진/DNS) 우회 무력화. 루프백·RFC1918·공인은 허용(로컬 OLLAMA·내부망 정합). `pcv_url_target_allowed` 헬퍼. *(잔여: DNS-rebind TOCTOU.)*
- **감사 로그 위변조 방지 (A09 / 2.9)**: `audit_log`에 SHA-256 **해시체인**(`prev_hash`/`rec_hash`) — 필드 변조·행 삭제·재배열 검출. 기동 시 체인 자기검증(`pcv_audit_verify_chain`). *(평문 SHA256의 로컬 root 재계산 한계는 후속 HMAC/외부앵커.)*
- **RNG/PBKDF2 하드닝 (A02 / V11)**: `/dev/urandom` 실패 시 비암호 PRNG(`g_random`) 폴백 제거 → `RAND_bytes`, 실패 시 **fail-closed**(abort). PBKDF2 반복수 100k→**600k**(iter를 해시에 임베딩 `pbkdf2:<iter>:<hex>` → 레거시 100k 해시 무중단 검증 + 로그인 시 600k 점진 재해시, **락아웃 0**).

### 재발방지 게이트 (ADR-0025 반사실 — 제거 시 RED)
- `check-grpc-authz` · `check-ssrf-target-guard` · `check-audit-hashchain` · `check-rng-safe`를 `check-all`(11→**15**) + pre-commit 편입.

### Upgrade notes
- **무중단**(전 마이그레이션 투명): `audit_log` 컬럼 자동 ALTER · PBKDF2 해시 로그인 시 자동 재해시(기존 검증 무영향) · gRPC off-by-default. **행동 변화 1건**: `[grpc] enabled=true`인데 `auth_token` 미설정이면 gRPC 미기동(P2 무인증 제어평면 금지) — 무토큰 gRPC는 `auth_token` 설정 필요.

## v1.2.8 — 2026-07-16

보안 시정 트랙 **Wave A**(PATCH) — OWASP/ISMS-P 평가([`docs/operations/2026-07-16-security-assessment-owasp-ismsp.md`])의 저위험·하위호환 HIGH급 갭 3건 + 재발방지 게이트 3종. 배포 프로파일 **P2(다중사용자/공유호스트)** 기준. 검증: `make single` 0-warning + `make test` **663/0** + `make check-all` **11게이트**(신규 3 포함) + 반사실 RED 실증.

### 보안 시정
- **CORS 앵커 검증 (A05 / ASVS V3·V13)**: 오리진 검증의 비앵커 substring 매칭(`http://192.168.evil.com`·`https://<host>.attacker.com` 우회) 제거 → **정확 일치**(루프백 + same-host)만 허용. 검증된 origin만 echo + `Vary:Origin`, `"*"`+credentials 조합 금지. (`_cors_origin_allowed` 헬퍼)
- **자격증명 로깅 마스킹 (A09 / V14·V16)**: 감사 본문 로그 마스킹이 `/auth/*` **경로만** 커버하던 것을 **본문 민감키**(password/secret/token/api_key 등 10종) 기반으로 확장 → `/api/v1/rpc` 경유 `auth.user.create` 등 **평문 비밀번호 로깅 차단**. (`_body_has_secret` 헬퍼)
- **아웃바운드 리다이렉트 금지 (A10 / V4)**: webhook(`alert_engine.c`)·AI(`ai_agent.c`) 아웃바운드에 `SOUP_MESSAGE_NO_REDIRECT` — 302 리다이렉트로 SSRF allowlist/denylist 우회 차단.

### 재발방지 게이트 (ADR-0025 반사실 — 제거 시 RED)
- `check-cors-anchor` · `check-secret-logging` · `check-ssrf-guard`를 `check-all`(8→**11게이트**) + pre-commit에 편입. 각 self-test + 반사실 RED 실증.

### Upgrade notes
- **무중단** — 데몬 wire 계약 무변경. CORS는 정당한 same-host/loopback 브라우저 클라이언트에 영향 없음(공격 오리진만 차단). 감사 로그 마스킹 확대는 로그 출력만 변경. *(내부망 cross-origin이 필요하면 향후 config allowlist — Wave B/C.)*

## v1.2.7 — 2026-07-16

실행 중 vCPU 증가가 VM 최대치(maxVcpus)를 초과할 때의 프론트 안내 개선(PATCH). v1.2.6 Fix B(축소→config-only)에 이어 vCPU 조정 UX 완결. **UI 전용 변경** — 데몬 로직 무변경(1.2.6과 동일, 버전 문자열만). 검증: `make single` 0-warning + `make check-all` **8게이트 PASS** + 실브라우저(목 API + Playwright).

### 프론트엔드
- **max-초과 증가 안내**: 실행 중 `vm.set_vcpu` 증가가 `-32000 "requested vcpus is greater than max allowable vcpus"`로 실패(부팅 시 고정된 maxVcpus 초과)할 때, libvirt 원문 대신 명확 안내("요청 N vCPU가 이 VM의 최대치(M)를 초과합니다. 최대 vCPU는 VM 정지 후 재구성해야 합니다.") — 메시지에서 `N > M` 값 추출. 축소(hotpluggable→config-only 제안)·max-이내 증가(라이브 성공)와 함께 **vCPU 조정 UX 3분기 완결**. Fix B 축소 경로 회귀 없음(실브라우저 확인).

### Upgrade notes
- **무중단** — UI 전용, 데몬 wire 계약·로직 동일.

## v1.2.6 — 2026-07-16

프론트엔드 결함 시정 + 실행 중 vCPU 축소 UX(Fix B) + toast 레벨 인지 개선(PATCH). 사용자 실사용 신고(웹에서 vCPU 4→2가 "통과" 표시되나 미변경)에서 출발한 **"성공 보고, 실제 실패" 클래스**를 프론트 전수 스윕. **데몬 wire 계약은 하위호환 추가만**(신규 옵션 param `apply`, 미지정 시 현행 동작). 검증: `make single` 0-warning + `make test` **663/0** + `make check-all` **8게이트 PASS** + 실브라우저(목 API + Playwright) 검증.

### 프론트엔드 (거짓 성공 제거)
- **doSet r.error 미검사 클래스 스윕**: `api.js` 계약(`fetchXxx`는 본문 에러에 throw 안 함 → 호출부 `if(r.error)` 필수)을 위반해 데몬 거부를 삼키고 성공 토스트를 띄우던 변이 호출 **28개 사이트** 시정(PR #40). `toast(x,'e')` 초록-렌더 7건 → 빨강 정정. bulk 3종(action/snapshot/stop) 거짓 "완료" 제거.
- **toast() 레벨 인지**: 2번째 인자를 불리언(하위호환) 또는 레벨 문자열(`'e'`/`'w'`/`'s'`)로 인지 — 경고=노랑(`.t-warn`). 기존 `toast(m,'w')`가 초록으로 렌더되던 풋건 해소.

### Fix B — 실행 중 vCPU 축소 경로 (하위호환 계약 추가)
- 실행 중 VM의 vCPU를 부팅 수 아래로 줄이는 live decrease는 QEMU/KVM 제약(-32000 "failed to find appropriate hotpluggable vcpus")으로 불가하다. 데몬은 이를 정직하게 반환(버그 아님).
- `vm.set_vcpu`/`vm.set_memory`에 옵션 `apply` 추가: `"config"` → 실행 중이어도 **CONFIG-only(다음 부팅 반영)**. 미지정/`"live"` → 현행(실행 중 LIVE|CONFIG, 정지 CONFIG). 완전 하위호환. 플래그 결정을 순수 함수 `pcv_hotplug_compute_affect_flags`로 추출(ADR-0025 별도 TU `hotplug_affect_policy.c`) + 반사실 유닛테스트(config_only 분기 제거 시 RED).
- 프론트: 라이브 축소 실패 감지 시 확인 다이얼로그 → `apply:"config"` 재전송 → "다음 재시작 시 N vCPU로 적용" 경고 토스트. 라이브값은 미갱신(다음 부팅 반영).

### Upgrade notes
- **무중단** — 신규 `apply` param은 옵션(미지정 시 완전 현행). 에러코드·RPC·config 동일. UI 정직화(실행 중 vCPU 축소는 이제 명확한 에러 또는 config-only 경로로 안내).

## v1.2.5 — 2026-07-16

배치별 code-review follow-up 하드닝 릴리스(PATCH). 감사 시정 각 배치 최종 리뷰의 비차단 code-review 항목을 실코드 그라운딩 후 시정 + audit 배치 게이트 CI 편입. **런타임 계약 무변경**(하드닝 — fail-secure는 오류 경로만, 신규 락은 경합 시에만 `-32004`(busy)). 검증: 전 커밋 `make single` 0-warning + `make test` **0 not ok** + `make check-all` **8게이트 PASS**.

### 동시성 / 락 하드닝
- **CMP-7** clone source 락 **실 갱신**(`pcv_vm_lock_renew` pid-소유권 게이트 + 60s 하트비트) — 대형 full-copy clone이 락 TTL(300/600s) 초과 시 stale 탈취되던 창 폐쇄.
- **CMP-2** 코어 회수(**실버그**): crash 경로 키불일치(이름 vs UUID)·graceful STOPPED 무해제 두 창을 종단정지 콜백으로 폐쇄.
- **`vm.resize_disk`** VM_OP_TUNING 락(동시 delete/hotplug 직렬화). **NET-5** overlay reconcile↔teardown zombie 부활 레이스를 단일 락순서 meta 재확인으로 폐쇄. **AIO-7** silence 리더 g_once 배리어 통일.

### 보안 fail-secure
- **SEC-4** HIPS 승인 만료 판정(`is_expired`) DB 오류 3갈래를 fail-open → **fail-secure**(만료 취급). **SEC-3** freeze-effective 마이그레이션 PRAGMA 읽기/쓰기 rc 확인(판정 불가 시 skip — admin 설정 role 매부팅 클로버 방지).

### 검증 규율 / 게이트
- **CMP-3** 라이브 `vm.create` base_image 검증 배선. **NET-1** getifaddrs 실패 reason 오귀속 시정.
- **check-audit-placement**(ADR-0018 async registry/audit/WS completion 계약)를 `check-all` **8번째 게이트** + pre-commit로 편입 — 스크립트만 있고 CI 미연결이던 갭 시정(`_mt` WS completion regex 회귀 포함). apikey role-cap(발급 상한) 술어 회귀 테스트.

### Upgrade notes
- **무중단**(계약 무변경) — 에러코드·RPC·config 동일. 경합 시 일부 mutating 작업이 `-32004`(busy) 반환 가능(신규 VM_OP 락 — 클라이언트 재시도로 해소).

## v1.2.4 — 2026-07-15

감사 시정 트랙 **종결** 릴리스(PATCH). v1.2.0 감사 큐 최후 항목(DISP-6) + 마지막 미검증 안전통제 승격 + SEC-8 잔여 클래스-스윕. **데몬 wire 계약 무변경** — 에러코드 통일은 값-보존, 자가치유 승격은 테스트/seam 추출, gRPC 상수시간화는 행위 동일. 검증: 전 커밋 `make single` 0-warning + `make test` **0 not ok** + `make check-all` **7게이트 PASS** + 안전통제 **14/14 tested**.

### 코드 위생 / 하드닝
- **DISP-6 에러코드 통일(A2-6)**: 두 병렬 enum(`PURE_RPC_ERR_*` vs `dispatcher.c PCV_ERR_*`)을 canonical 일원화(+신규 BUSY/-32004·NOT_FOUND/-32005·FORBIDDEN/-32006) · raw 리터럴 ~500 → named 상수(**값·메시지 정확 보존 = wire 무변경**) · REST→HTTP 매핑 상수화 · 신규 raw 리터럴 방지 게이트(`check_error_codes.py`, check-all **7번째**+pre-commit). 오버로드 값(-32000/1/2 dual-meaning)은 known-limit(값 병합=계약변경은 이연).
- **SEC-8 클래스-스윕**: gRPC auth token 비교를 상수시간화(`g_strcmp0`→`pcv_secret_str_eq`). 헬퍼를 중립 `utils/pcv_crypto.{c,h}`로 이전(레이어링 정리).
- **self-healing-restart tested 승격**: VM 재시작 결정 로직(running-guard+`virDomainCreate`)을 libvirt-무의존 seam으로 추출 + 스파이 효과테스트(반사실 RED). **착지 후 14 안전통제 전부 tested(untested 0).**

### Upgrade notes
- **런타임 계약 무변경** — 에러코드 wire 값·메시지 동일, gRPC 인증 동작 동일, 자가치유 행위 동일. 무중단 업그레이드.

## v1.2.3 — 2026-07-15

리뷰-기반 감사 시정 릴리스(PATCH). v1.2.0 post-release 전수 감사의 **MED/LOW findings 전량 + NET-1(HIGH)** 시정을 subagent-driven-development(태스크별 구현+리뷰+반사실, 최종 Opus whole-branch)로 착지. 대부분 데몬 계약 불변이나 **신규 ADMIN RPC 1개**·**API Key role 집행 변경(SEC-3, 1회 마이그레이션)**·**CLI param-key 정합**·**graceful-drain/dpdk 가드 실동작**을 포함 — 배포 전 `### Upgrade notes` 확인. 검증: 전 커밋 `make single` 0-warning + `make test` **640/0** + `make check-all` **6게이트 PASS**(RBAC·RPC consumers·dead-exports·param-contract·json-ingress·safety-controls) + 격리 데몬 효과-테스트 다수(ADR-0025 반사실 RED-on-removal). 안전통제 **14 tested**, untested = self-healing-restart 1.

### 보안 (auth · privesc · 타이밍)
- **SEC-3** API Key privesc 차단 — 실효 role을 저장 role로 집행(client_name 라이브 파생 제거). freeze-effective 마이그레이션(`PRAGMA user_version` 1회, 권한 변동 0). `apikey-role-enforce` tested.
- **SEC-4** HIPS 승인 워커가 만료 후 execute하던 부작용 차단(execute 앞 `is_expired`). `hips-approval-expiry` tested.
- **SEC-8** 부트스트랩 비번 비교 상수시간화(SHA256 + `CRYPTO_memcmp`).
- **SEC-6** apikey table ensure를 `g_rbac_mutex` 안으로(SQLite 직렬화 불변식 정합).
- **refresh-remint** 신규 ADMIN RPC `auth.user.sessions.revoke {username}` — 비번 회전 후 세션 re-mint 봉쇄. `user-sessions-revoke` tested.

### 네트워크
- **NET-1 (HIGH)** `dpdk.bind` 관리 NIC 보호 가드 — 관리/기본경로 NIC(UP+IPv4 또는 기본경로 dev)의 vfio-pci 바인딩 거부(`-32602`), 호스트 네트워크/SSH 붕괴 방지. 전용 DPDK NIC(커널 미관리)만 허용. fail-secure.
- **NET-2** isolated 방화벽 DROP 룰 실패 전파. **NET-4/5** QoS/overlay 재수화 부팅1회성→주기 reconcile. **NET-3** `sriov.disable` sysfs write 실패 전파.

### 스토리지
- **STO-2** 스냅샷 prune 데이터유실 — `pcv-` 시스템 네임스페이스 예약(`backup-retention` tested). **STO-5** `backup.incremental` 워커 오프로드 · **STO-1** env strv 누수.
- **STO-3** export 가드 `domstate` 조회 실패를 fail-secure(거부)로. **STO-4** iSCSI CHAP account/bind `_run`→`_run_argv`(비번 재토큰화 제거).

### 락/동시성 (VM_OP · AI-Ops · audit)
- **CMP-3** 라이브 `vm.create` 파라미터 검증 배선(iso_path, `vm-create-iso-validation` tested) · **CMP-7** `vm.clone` TOCTOU 락 · **CMP-10** 12 mutating hotplug VM_OP 락 · **CMP-2** 재start 코어 누수 idempotent.
- **AIO-2** 재시작 브레이커 0-피드백 프로브 토큰 회수(복구봉쇄 해소) · **AIO-1** anomaly 전용 뮤텍스 · **AIO-4** DLQ 재시도 락 보유 중 동기 HTTP 제거 · **AIO-3** `alert.silence` casefold(`alert-silence` tested).
- **AIO-7** silence 지연초기화 `g_once` · **AIO-11** agent config `G.mu` · **AIO-10** audit `dropped_count` 원자화(전용 뮤텍스).

### 디스패처 / 제어평면
- **DISP-4** graceful-drain 실배선(수락 inc / cleanup dec 1:1, drain 중 read 후 거부) + **node.resume 화이트리스트**(제어평면 brick 풋건 시정). `graceful-drain` tested.
- **DISP-3** io_uring `submit` 반환 검사 — recv 누수 정리 + accept 재무장(shutdown 가드) + 잔존 SQE nop 무해화.
- **DISP-12b** gRPC 4MB 스택 버퍼 heap화 · **CMP-9** 함수포인터 UB 캐스트 제거.

### CLI / FE 계약
- **CLI-17~24** `pcvctl` JSON-RPC param-key 불일치 거짓성공 시정(6 rename + node.drain/disk.attach 2 배선). 게이트 #1 WARN 30→22.
- **FE-4/5/6** selfhealing/DLQ 거짓성공 제거·라우트 정정 · **FE-1/2/3** apikey 관리 UI 계약정합(R-embed·죽은코드 제거·role/만료/revoke).

### 게이트 / 검증 규율
- **안전통제 효과-테스트 레지스트리 게이트** — 마커 ⊆ `contracts/safety_controls.json`, tested → 실 effect_test, dup-key 감지, tested ∉ baseline 강제, 반사실 self-test. `check-all` + pre-commit 통합.
- rest_server 응답 길이 drift 시정(403 over-read `.rodata` 누출 + 400 절단). self_healing 10정책 docstring · role enum 주석 정정.

### Upgrade notes
- **API Key role 집행(SEC-3)**: 기존 키의 실효 role이 저장 role로 동결. freeze-effective 마이그레이션 자동 1회(`PRAGMA user_version`, 권한 변동 0 — 현 실효값 동결). 재시작 시 자동 적용.
- **신규 ADMIN RPC**: `auth.user.sessions.revoke {username}`.
- **CLI param-key(CLI-17~24)**: `acl.list` switch_name→switch · `storage.pool.health` name→pool · `health.set` interval→interval_sec · `nic.attach` mac→hwaddr · set_limits cpu_quota→cpu_percent · set_bandwidth inbound/outbound→inbound_kbps/outbound_kbps. (구 키는 param-key 불일치로 애초에 `-32602` 무동작이었어 실사용 영향 없음.)
- **graceful-drain 실동작(DISP-4)**: `node.drain`이 이제 실제 inflight 대기 후 종료, `node.resume`로 해제.
- **dpdk.bind 가드(NET-1)**: 관리/기본경로 NIC bind 시도는 `-32602` 거부.

## v1.2.2 — 2026-07-14

리뷰-기반 시정 릴리스(PATCH). 데몬 RPC/REST/config 계약은 전부 불변이며, 감사 확증 테마 "보고성공 무동작"에 대한 **ADR-0025 반사실 검증 규율** 도입 + 무동작 스텁 2건 실배선 + 감사 정확성 수정 + TUI(중복 운영 표면) 제거로 구성된다. 검증: 전 커밋 `make single` 0-warning + `make test` **619/0** + `make check-all` **5게이트 PASS**(RBAC·RPC consumers·dead-exports·param-contract·json-ingress) + 격리 데몬 효과-테스트(snapshot.verify 8/8 반사실 · vm.batch 6/6 팬아웃·per-VM 감사).

> ⚠️ **인터페이스 제거 + 무동작 스텁 실동작화 포함** — 배포 전 `### Upgrade notes` 확인. 특히: `pcvtui` 바이너리 제거, `backup.snapshot.verify`·`vm.batch`(v1.2.1에 이미 등록된 라우트)가 이제 실제로 동작(옛 no-op 응답 아님), `vm.batch` action whitelist({start,stop}).

### 운영 표면 축소 (TUI 완전 제거)
- **`pcvtui` 제거** — 터미널 UI(`src/tui`, ~8,528 LOC) + `pcvtui` 바이너리 + Makefile 타깃·deb/deploy/release 참조·게이트의 TUI 소비 추출을 전량 제거(중복 운영 표면 정리). 데몬 RPC·REST·Web UI·`pcvctl` 은 불변. monitor RPC 응답 id `tui-req`→`monitor-fleet` 개명. (부수: main 부터 있던 `ui/sw.js` git 충돌마커/깨진 서비스워커 수정.)

### 무동작 스텁 실배선 (ADR-0025 배선=완료 — v1.2.1 등록 라우트의 no-op → 실효과)
- **`backup.snapshot.verify`** — 옛 스텁은 zfs 명령 문자열만 조립하고 하드코딩 `exists:true` 반환. 이제 async-result GTask 워커가 `zfs list -t snapshot`(셸 미경유 argv)로 실 존재 판정, 존재 시 `zfs get -H -o value written`(property-read)로 integrity 뒷받침(verified/degraded/missing). CLI `pcvctl snapshot verify` 배선.
- **`vm.batch`** — 옛 스텁은 action 을 수행하지 않고 "accepted" 만 반환. 이제 whitelist action({start,stop})에 한해 존재하는 각 VM 에 `purecvisor_vm_manager_<action>_async` 팬아웃 + 존재 검증(미존재→rejected[]) + 집계 응답 `{action,accepted[],rejected[]}`. whitelist 밖 action(pause 등)은 `-32602`. CLI `pcvctl vm batch <start|stop> <vm...>` 배선.

### 감사 정확성 수정 (ADR-0025 "보고성공 무동작" 직접 대응)
- **I-1 `backup.snapshot.verify` 감사 계약** — async-result 인데 `g_async_methods` 미등록이라 디스패처가 dispatch 시점 무조건 `audit "ok"`(param 누락 에러도 "ok")를 남기던 결함. async 등록 + 완료콜백/조기검증 경로에서 실결과 audit(형제 async 핸들러 관례).
- **M-1 `vm.batch` per-VM 감사** — 팬아웃이 NULL 콜백(fire-and-forget)이라 개별 VM start/stop 실패가 audit·클라 어디에도 노출 안 되던 갭. per-VM 완료 콜백이 `finish()`로 결과 propagate(vm-started/stopped 시그널 emit 계약 복원) 후 per-VM audit.

### CI 계약 게이트 (ADR-0025 — 정적/래칫, 런타임 비영향)
- **소비-완전성 + 고아 게이트**(`check-rpc-consumers`) — RPC 소비를 전 경로(CLI 리터럴·`security_request` 래퍼·FE generic /rpc·REST 브릿지 `_build_rpc`·gRPC·tests/)에서 추출, "소비 ⊆ 등록" + 고아(등록−소비 ⊆ `contracts/rpc_orphan_baseline.json`) 불변식. 반사실 self-test 5종(위반 주입→FAIL) + dead-candidate 정직 self-check(test-covered 를 dead 로 표기→MISLABEL FAIL). 배선된 2 메서드 baseline 제거로 dead-candidate 0.
- **ADR-0025** 반사실 검증 규율 문서화 — 모든 통제·게이트는 위반 시 RED 되는 반사실 동반 + 게이트 self-test 의무 + "효과·배선=완료" 정의. 신규 게이트 필수 상속.

### 테스트 실효성 (ADR-0025 자기적용)
- **I-2** — `snapshot.verify`/`vm.batch` 유닛 테스트가 프로덕션 아닌 복제 로직을 검증하던 것을, 테스트 가능한 로직을 링크 가능한 TU(`src/api/snapshot_verify_probe.{c,h}`·`vm_batch_policy.{c,h}`)로 추출해 데몬+test_runner 양쪽 링크 → 유닛 테스트가 실 프로덕션 함수 호출(nm 로 단일 정의 공유 실증).

### Upgrade notes
- **`pcvtui` 제거**: 이 릴리스는 `pcvtui` 바이너리를 설치하지 않는다. `.deb` 업그레이드 시 dpkg 가 옛 `pcvtui` 를 자동 제거하며, 수동 배포(`deploy.sh`)도 stale `pcvtui` 를 정리한다. pcvtui 로 하던 작업은 Web UI(`:8080/ui/`)·`pcvctl`·REST API 로 대체.
- **`backup.snapshot.verify`·`vm.batch` 실동작화**: 두 메서드가 이제 실제 효과를 낸다. 옛 no-op 응답(`exists:true` 고정 / 무조건 accepted)에 의존한 흐름은 없음(원래 무동작 스텁). `vm.batch` 는 `{start,stop}` 만 허용(그 외 `-32602 "unsupported batch action"`), 존재하지 않는 VM 은 `rejected[]`.
- **런타임 API/config 무변경**: RPC envelope·인증·config 스키마 불변. 게이트는 CI 전용(런타임 비영향).

### 알려진 잔여 (후속 트랙)
- vm.batch 존재검증이 GMainLoop 동기(단일 노드 허용, 원격/경합 시 워커 이관이 정석 — 후속 리팩터). `g_dispatch_vm_manager` 싱글턴은 단일 디스패처 불변식(다중 인스턴스 시 `-32000` degrade, 크래시 아님).
- 효과-테스트(bwrap/userns 의존)는 해당 환경 부재 시 SKIP — CI 커버리지 확보는 후속.
- 별도 트랙: NET-1(dpdk.bind), 게이트 #4(안전통제 효과 테스트), refresh-remint 차단, MED/LOW findings.

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
