# 개발 단계별 검증 규칙

> **대상:** PureCVisor Single Edge
> **목적:** 기능 개발, 버그 수정, 릴리스 직전 검증을 같은 기준으로 운영하기 위한 공식 규칙
> **현행화 기준:** 2026-05-04
> **관련 문서:** [GUIDE.md](GUIDE.md), [DEVELOPER_INDEX.md](DEVELOPER_INDEX.md), [SOURCE_CODE_COMMENTING_STANDARD.md](SOURCE_CODE_COMMENTING_STANDARD.md), [SERVICE_FUNCTIONAL_TEST_SCENARIOS.md](SERVICE_FUNCTIONAL_TEST_SCENARIOS.md), [PUBLIC_RELEASE_BOUNDARY.md](PUBLIC_RELEASE_BOUNDARY.md), [ADR_INDEX.md](ADR_INDEX.md), `docs/adr/`

---

## 1. 문서 목적

이 문서는 PureCVisor Single Edge의 소스코드 변경이 언제, 어떤 깊이로 검증되어야 하는지를 단계별로 정의한다.

- `GUIDE.md`는 현재 리포에 남아 있는 기능 표면과 설치 전제를 설명한다.
- `PUBLIC_RELEASE_BOUNDARY.md`는 Single Edge 공개판의 허용/금지 표면을 정의한다.
- `ADR_INDEX.md`, `docs/adr/`는 설계 배경과 현재 적용 상태를 보강한다.

---

## 2. 핵심 원칙

1. 모든 변경은 구현 완료 후 한 번에 검증하지 않는다.
2. 가장 작은 단위에서 먼저 검증하고, 변경 영향 범위에 따라 상위 단계 검증으로 확장한다.
3. 실환경 영향이 있는 변경은 로컬 통과만으로 완료 판정을 내리지 않는다.
4. 릴리스 판단은 기능 수가 아니라 골든 시나리오와 운영 안정성 기준으로 내린다.
5. 검증되지 않은 항목은 `정상`이 아니라 `미확인`으로 취급한다.
6. 성능 테스트, 장시간 실행, API 응답 시간 측정은 기능 정합성 검증을 대체하지 않는다.
7. 의미 있는 소스 변경은 [SOURCE_CODE_COMMENTING_STANDARD.md](SOURCE_CODE_COMMENTING_STANDARD.md)의 주니어 개발자용 상세 설명과 비개발자용 영향 설명 기준을 함께 만족해야 한다.

---

## 3. 검증 단계

| 단계 | 이름 | 목적 | 대표 대상 |
|------|------|------|-----------|
| Level 1 | 로컬 코드 검증 | 코드 단위 결함과 회귀를 빠르게 차단 | 신규 함수, 핸들러, 파서, 유틸리티, 설정 검증 |
| Level 2 | 단일 노드 실행 검증 | 데몬 기동, API, 단일 노드 동작 확인 | Single Edge 기능 전반 |
| Level 3 | 실환경 단일 노드 검증 | 실제 호스트에서 서비스, UI, 운영 시나리오 재확인 | `purecvisorsd`, systemd, `/health`, UI, OVS/OVN, 장시간 실행 |
| Level 4 | 출시 게이트 검증 | 실제 배포 가능 여부 최종 판정 | 릴리스 후보 전체 |

---

### 3.1 서비스 기능 테스트 보강 기준

서비스 기능 시나리오는 [SERVICE_FUNCTIONAL_TEST_SCENARIOS.md](SERVICE_FUNCTIONAL_TEST_SCENARIOS.md)의 작성 기준을 따른다.

- 성공 응답은 기능 완료 증거가 아니다. 영속 상태, 데이터 무결성, 부작용, audit/log를 함께 확인한다.
- `accepted` 응답을 반환하는 비동기 기능은 accepted 응답, worker completion, audit, 최종 리소스 상태를 분리해서 대조한다.
- VM, storage, network, backup/restore, auth/RBAC처럼 운영 상태를 바꾸는 기능은 최소 1개 성공 경로와 1개 거부/실패 경로를 가진다.
- guest 내부 상태, storage backend 상태, network reachability처럼 시스템 경계를 넘는 기능은 host-side artifact만으로 완료 판정을 내리지 않는다.
- 성능 테스트나 longrun이 통과해도 기능 시나리오의 최종 상태와 cleanup 확인이 없으면 해당 기능은 `미확인`으로 남긴다.

---

## 4. Level 1: 로컬 코드 검증

### 4.1 필수 대상

- 신규 함수 추가
- 기존 함수 로직 변경
- 핸들러 분기 추가
- 설정 파서/검증 로직 수정
- 테스트 가능한 버그 수정
- 에디션 경계 또는 Single Edge 실패 가드 변경

### 4.2 최소 검증 항목

- 관련 단위 테스트 또는 회귀 테스트 추가
- 대상 바이너리 빌드 성공
- 정적 품질 게이트 통과
- 수정 경로에 대한 최소 1회 실행 검증
- 소스 변경이면 [SOURCE_CODE_COMMENTING_STANDARD.md](SOURCE_CODE_COMMENTING_STANDARD.md)에 맞춰 주니어 개발자용 상세 설명과 비개발자용 영향 설명을 갱신

### 4.2.1 소스코드 작성 및 주석 표준 게이트

`src/`, `include/`, `ui/modules/`, `scripts/`, `tests/`에서 의미 있는 로직을 추가하거나 바꾸면 다음을 Level 1 검증에 포함한다.

```bash
bash tests/integration/test_source_commenting_standard.sh
rg -n "\b(TODO|FIXME|HACK|XXX)\b" src include tests ui/modules scripts
git diff --check
```

첫 번째 명령은 작성 표준 문서가 개발 문서 진입점과 검증 정책에 연결되어 있는지 확인한다. 두 번째 명령은 출력이 없어야 한다. 금지어 자체를 검사하는 테스트는 런타임 문자열 조합으로 작성해 부채 스캔과 충돌하지 않게 한다.

리뷰어는 다음 조건을 확인한다.

- 신규 또는 변경된 핵심 entry point가 caller 계약, ownership, cleanup, 실패 경로를 설명하는가?
- 사용자 데이터, 권한, 네트워크, 비동기 결과, 보안 경계에 닿는 변경에 비개발자용 영향 설명이 별도로 있는가?
- 주석이 ADR, Single Edge 공개 경계, 실제 구현과 충돌하지 않는가?

### 4.3 ADR-0018 정적 게이트

fire-and-forget RPC를 추가하거나 `accepted` 응답 후 worker/callback에서 실제 작업을 수행하는 경로를 바꾸면 다음 검증을 Level 1에 포함한다.

```bash
python3 scripts/check_audit_placement.py
python3 scripts/check_ova_async_result.py
```

`vm.export.ova` 또는 `vm.import.ova`를 바꾸면 위 검증에 더해 accepted 응답의 `job_id`, worker 최종 job 상태, audit `ok/fail`, WebSocket completion, 실제 OVA 파일 또는 target domain/disk/zvol 상태를 하나의 기능 시나리오로 대조한다. OVA export는 `qemu-img`/`tar` 실패가 `PCV_JOB_FAILED`와 audit `fail`로 끝나는지 확인해야 하고, OVA import는 zvol 생성 뒤 실패 시 `zfs destroy -R` cleanup이 수행되는지 확인해야 한다.

### 4.4 추가 정적 게이트

Supanova 테마 허용 목록이나 CSS 변형을 바꾸면 다음 검증을 Level 1에 포함한다.

```bash
python3 scripts/check_supanova_themes.py
```

Web UI 시각 규격, `DESIGN.md`, `ui/style.css`, `ui/samples/`, `ui/guide.html`,
`ui/guide-content.md`의 디자인 연결을 바꾸면 다음 검증을 Level 1에 포함한다.

```bash
python3 scripts/check_design_md.py
bash tests/integration/test_design_md_surface.sh
PCV_NO_DEPLOY=1 scripts/bundle-ui.sh
node --check ui/app.bundle.js
git diff --check
```

`docs/GUIDE.md`는 기능/운영 가이드로 유지하고, 색상 token, typography,
component state, dashboard density, table/card/button/modal 규칙은 루트
`DESIGN.md`를 단일 진실로 삼는다. `ui/samples/design-system-preview.html`은
이 시각 규격을 같은 `ui/style.css` 위에서 확인하는 preview HTML이다.

ZFS inflight lock, Prometheus exporter, distributed lock 경로를 바꾸면 다음 검증을 Level 1에 포함한다.

```bash
python3 scripts/check_zfs_inflight_metrics.py
```

ZFS inflight lock metric을 Web UI 모니터링에 노출하는 경로를 바꾸면 다음 검증도 Level 1에 포함한다.

```bash
python3 scripts/check_zfs_inflight_monitor.py
node --check ui/app.bundle.js
```

dispatcher RPC 등록 또는 RBAC 정책 매핑을 바꾸면 다음 검증을 함께 실행한다.

```bash
make check-rbac
```

이 검증은 내부적으로 `scripts/check_rbac_policies.py`를 실행한다. destructive RPC의 정책 매핑 누락뿐 아니라 `device.nic.attach`/`device.nic.detach`처럼 operator에게 열려야 하지만 VM owner-scope를 반드시 통과해야 하는 정책 계약도 검사한다. 해당 메서드가 admin-only로 되돌아가거나 owner-scope 대상에서 빠지면 회귀로 보고 실패해야 한다.

### 4.7 VM Guest Agent 조회/요약 UI 변경

`vm.guest.*` 조회 RPC, REST VM 상세 하위 경로, `ui/modules/vm.js` 요약 카드, qemu-guest-agent 기반 디스크 사용량 표시를 바꾸면 Level 1에 다음을 포함한다.

```bash
make single
make check-rbac
PCV_NO_DEPLOY=1 scripts/bundle-ui.sh
node --check ui/app.bundle.js
bash tests/integration/test_vm_disk_usage_surface.sh
git diff --check
```

실환경 검증에서는 실행 중 VM에 qemu-guest-agent가 설치되어 있어야 하며, Web UI `대시보드 > 요약 > <VM> > 디스크 사용량`과 REST `/api/v1/vms/{name}/disk-usage`, UDS `vm.guest.fsinfo` 응답이 같은 mountpoint/usage 값을 보여야 한다.

### 4.8 `security.*` HIDS/HIPS 변경

`security.*` RPC, `src/modules/security/`, Security Events UI, CLI/TUI 보안 표면,
baseline, HIPS action, security DB schema를 바꾸면 Level 1에 다음을 포함한다.

```bash
make test
make check-rbac
bash tests/integration/test_security_cli_tui_surface.sh
git diff --check
```

- security event JSON roundtrip 테스트
- baseline refresh audit 기록 테스트
- approve/dismiss RBAC 테스트
- `pcvctl security ...` 명령군과 `pcvtui` Security Guard 단축키 표면 테스트

fire-and-forget worker를 새로 추가하면 ADR-0018에 따라 `scripts/check_audit_placement.py`와
`scripts/check_ova_async_result.py`도 실행한다.

### 4.9 ADR-0023 `vm.clone` 안전 게이트

`vm.clone`, clone plan, libvirt XML disk source 판정, clone XML patch, ZFS snapshot/clone, qcow2/raw file copy, guest reset, Web UI clone 경로를 바꾸면 Level 1에 다음 검증을 포함한다.

```bash
make single
./test_runner -p /spawn_launcher
./test_runner -p /vm_clone_plan
make test
scripts/check_audit_placement.py
scripts/check_vm_clone_cleanup.py
PCV_NO_DEPLOY=1 scripts/bundle-ui.sh
python3 scripts/check_ui_bundle_fresh.py
for f in ui/app.js ui/modules/*.js ui/vendor/chart.umd.min.js ui/vendor/novnc/novnc.esm.js; do node -c "$f"; done
git diff --check -- src/api/dispatcher.c src/modules/virt/vm_clone_plan.c src/modules/virt/vm_clone_plan.h src/modules/storage/zfs_driver.c src/modules/storage/zfs_driver.h src/utils/pcv_spawn.c src/utils/pcv_spawn.h src/cli/purecvisorctl.c ui/modules/vm.js ui/app.bundle.js tests/test_spawn_launcher.c tests/test_vm_clone_plan.c scripts/check_vm_clone_cleanup.py docs/adr/0023-vm-clone-beta-safety-guard.md docs/DEVELOPMENT_VERIFICATION_POLICY.md docs/GUIDE.md docs/SOURCE_LOGIC_STEP_BY_STEP_GUIDE.md docs/DEVELOPER_INDEX.md docs/ADR_INDEX.md follower.md Makefile tests/test_main.c
```

필수 회귀 케이스:

- source VM의 실제 disk source가 `daemon.conf [storage].zvol_pool`과 달라도 accepted 응답의 `source_disk`/`target_disk`가 libvirt XML 기준으로 계산된다.
- `template_prepared=true` 또는 `clone_safety_ack="template-prepared"`가 없으면 `guest_reset=true`로 target disk에 libguestfs 기반 guest reset을 실행해야 한다.
- data disk 0개, data disk 2개 이상, unsupported disk source는 거부된다.
- source VM이 power on 상태이면 storage type과 무관하게 preflight에서 거부된다.
- qcow2/raw는 `mode=full`에서만 허용하며, source file과 별도 target file path를 plan 단계에서 계산한다. target path는 원본 파일을 공유하면 안 된다.
- qcow2/raw file-copy primitive는 `qemu-img convert` argv 배열을 사용하고, source/target 동일 경로, 상대 경로, 존재하는 target을 거부한다.
- guest reset argv는 `virt-sysprep --format <raw|qcow2> -a <target>` 기반 identity reset, `guestfish` ext filesystem UUID 보정, `virt-customize` 기반 `/etc/fstab` UUID 참조 갱신과 Ubuntu `update-initramfs`, RHEL/Rocky `dracut`, grub 재생성, SELinux `/.autorelabel` 계약을 포함한다.
- clone XML은 name, UUID, disk source, MAC을 바꾸며 MAC 치환은 one-pass로 끝난다.
- `mode=full`의 zvol send/recv는 셸 파이프, 리다이렉션, `/tmp` 대용량 임시 파일이 아니라 `pcv_spawn_pipe_sync()` 스트리밍 경로를 사용한다.
- `mode=full` 성공 후 source 임시 snapshot은 정리하고, `mode=cow` snapshot은 target origin이므로 유지한다.
- snapshot 생성 이후 실패 경로는 target dataset을 먼저 best-effort 정리하고, source 임시 snapshot이 더 이상 origin으로 필요하지 않으면 best-effort 정리한다.
- accepted 응답은 worker completion과 같은 `job_id`를 포함한다.
- fire-and-forget worker callback은 성공/실패 audit와 WS completion을 남긴다.

실환경 확인 기준:

- `mode=cow`는 target zvol origin이 source snapshot을 가리키고, clone domain이 `shut off` persistent 상태여야 한다.
- `mode=full`은 target zvol origin이 `-`여야 하고, source 임시 snapshot이 남지 않아야 한다.
- 모든 성공 clone은 source VM `shut off` 상태에서 수행한다.
- qcow2/raw full clone은 target file 신규 생성, 원본 파일 미공유, clone domain `shut off` persistent 상태를 확인한다.
- `guest_reset=true` 실환경 검증 host에는 `libguestfs-tools`를 필수 설치한다. 이 패키지가 없으면 `virt-sysprep`, `virt-customize`, `virt-filesystems`, `guestfish` preflight 실패가 정상 결과다.
- 현재 필수 guest reset 실환경 기준은 Ubuntu 24.04 non-LVM qcow2/raw와 Ubuntu 24.04 LVM qcow2/raw/ZFS zvol full clone에서 target disk 독립성, `virt-sysprep` identity reset, ext filesystem UUID 분리, LVM PV/VG UUID 분리, `/etc/fstab` UUID 참조 갱신, boot artifact 재생성, hostname 변경, machine-id 재생성, clone VM boot smoke, audit `result=ok`를 확인하는 것이다.
- Rocky/RHEL LVM, SELinux enforcing boot smoke는 문서상 후속 검증 항목으로만 유지한다. 해당 계약을 직접 바꾸는 변경에서는 별도 실환경 검증을 다시 계획한다.
- 모든 모드에서 accepted 응답의 `source_disk`/`target_disk`/`job_id`/`guest_reset`, 원본/clone UUID와 MAC 분리, audit `result=ok`, 검증 리소스 정리를 확인한다.

### 4.10 Web UI 보안 헤더/정적 자산 게이트

Web UI shell, 번들, 공통 모달/확인창, Service Worker, PWA manifest, 외부 브라우저 자산, reverse proxy 보안 헤더를 바꾸면 Level 1에 다음 검증을 포함한다.

```bash
PCV_NO_DEPLOY=1 scripts/bundle-ui.sh
python3 scripts/check_ui_bundle_fresh.py
for f in ui/app.js ui/modules/*.js ui/vendor/chart.umd.min.js ui/vendor/novnc/novnc.esm.js; do node -c "$f"; done
git diff --check -- ui scripts src/api/rest_server.c
rg -n "iconify|code\.iconify|api\.iconify|api\.unisvg|api\.simplesvg|cdn\.jsdelivr|fonts\.googleapis|fonts\.gstatic|sourceMappingURL" ui/index.html ui/guide.html ui/app.bundle.js ui/sw.js ui/vendor
rg -n "customConfirm\([^\\n]*<[^\\n]*>|<br><b>|idx \|\| selectedVmIndex" ui/modules ui/app.bundle.js
rg -n '<base href="/ui/">' ui/index.html
rg -n 'replace\(/\^#\\/\?|renderOpsTriage' ui/modules/uxlib.js ui/modules/monitor.js ui/app.bundle.js
```

두 `rg` 명령은 출력이 없어야 한다. 운영 CSP를 완화해 외부 아이콘 API, CDN JavaScript, sourcemap fetch를 허용하는 방향은 기본 금지한다. 필요한 브라우저 런타임 자산은 `ui/vendor/`, 로컬 이미지, inline SVG로 고정하고, 공통 아이콘은 로컬 `ui/vendor/coolicons/coolicons.svg` 스프라이트를 우선한다.

다음 회귀도 함께 확인한다.

- `customConfirm()`에는 HTML 조각을 넘기지 않는다. 강조나 줄바꿈이 필요하면 호출부는 plain text와 `\n`만 넘기고, helper가 escape 후 줄바꿈만 렌더링한다.
- 리스트 행 버튼처럼 index `0`이 유효한 호출부는 `idx || selectedVmIndex`가 아니라 `idx ?? selectedVmIndex`를 사용한다.
- `manifest.json`의 아이콘 파일이 배포 스크립트와 Service Worker 정적 자산 목록에 포함되어 있다.
- `ui/vendor/chart.umd.min.js`에 `sourceMappingURL` 주석이 남아 있지 않다.
- `ui/vendor/novnc/novnc.esm.js`에 외부 CDN import나 `sourceMappingURL` 주석이 남아 있지 않다.
- Pretendard CSS/woff2처럼 브라우저 런타임에 필요한 폰트는 CDN이 아니라 `ui/vendor/`에서 제공한다.
- `/ui`와 `/ui/` 진입점이 같은 자산 경로를 쓰도록 `index.html`의 `<base href="/ui/">`를 유지한다.
- 해시 라우터는 `#/page`와 `#page`를 모두 처리해야 한다. 공개 안내 링크가 `/ui#ops-triage`로 들어와도 `운영 이벤트 센터`가 렌더링되어야 한다.
- metrics raw fetch는 `Authorization: Bearer` 헤더를 사용한다.
- WebSocket 인증 메시지는 현재 열린 local socket 인스턴스에만 전송한다.
- `src/api/rest_server.c`의 정적 파일 MIME 테이블은 PWA 이미지 확장자를 `image/*`로 반환한다.

### 4.11 제품 버전 노출 게이트

제품 버전 표기, 릴리스 번호, UI cache-busting query string, 버전 API 응답을 바꾸면 Level 1에 다음 검증을 포함한다.

```bash
make single
PCV_NO_DEPLOY=1 scripts/bundle-ui.sh
python3 scripts/check_ui_bundle_fresh.py
node --check ui/app.js
node --check ui/app.bundle.js
./test_runner -p /hotreload/version/format
./test_runner -p /prometheus/label_format/valid
bin/pcvctl --version
git diff --check -- include src ui scripts tests docs
rg -n -P "(?<![0-9.])(?:1\.0\.0|1\.0\.8(?:\.\d+)?|1\.0\.9(?:\.\d+)?|1\.2\.0|1\.3\.9|4\.4\.4)(?![0-9.])" src include ui scripts tests Makefile -g '!ui/vendor/**'
```

마지막 `rg` 명령은 출력이 없어야 한다. 버전을 실제로 올리는 릴리스에서는 검색 패턴을 이전 제품 버전 문자열로 바꿔 과거 표기가 남지 않았는지 확인한다.

제품 버전 단일 소스는 `include/purecvisor/version.h`의 `PCV_PRODUCT_VERSION`이다. 다음 노출 지점은 같은 릴리스 단위로 맞춘다.

- CLI `pcvctl --version`
- `/api/v1/health`의 `version`
- `/api/v1/version`의 `version`
- hot reload version getter
- Prometheus `purecvisor_info{version=...}`
- Web UI `window.PCV.config.VERSION`
- `ui/index.html`, `ui/guide.html`의 정적 자산 query string
- generated `ui/app.bundle.js`, `ui/sw.js`
- OpenAPI generator와 plugin/test 예제의 제품 버전 샘플

제품 버전으로 오인하지 말아야 하는 값은 별도 계약으로 유지한다. `/api/v1`, gRPC `purecvisor.v1`, OpenAPI `3.0.3`, Prometheus text format `0.0.4`, XML declaration, libvirt/ZFS/zlib/커널/패키지 버전, 외부 API header, IP/CIDR 예시는 제품 버전 통일 대상이 아니다.

### 4.12 dead export 정적 게이트

헤더 선언 함수 정의, `.c` 사용처, 또는 게이트 스크립트/baseline을 바꾸면 Level 1에 다음 검증을 포함한다.

```bash
make check-dead-exports
```

- `check-dead-exports`: 헤더 선언된 비-static pcv_* 함수 중 .c 사용처 0(정의만)인 dead export 노출·신규차단(래칫 `scripts/dead_exports_baseline.txt`). "배선 안 된 안전 함수"(SEC-1형) 재발 차단. 설계: `docs/superpowers/specs/2026-07-11-dead-export-gate-design.md`.

사용처 카운트는 `src/**/*.c`만 집계하고 `tests/`는 포함하지 않는다. 따라서 정의되고 유닛 테스트로만 호출될 뿐 프로덕션 경로에 배선되지 않은 `pcv_*` 함수는 (의도한 대로) dead export로 잡힌다 — 이 게이트가 겨냥하는 SEC-1급 사례이며, 실제로 의도된 export라면 `PCV_DEAD_EXPORT_OK` waiver로 해소한다.

### 4.13 RPC param-key 계약 게이트

dispatcher 핸들러, CLI/TUI 소비 콜사이트, 또는 `contracts/rpc_params*.json`/게이트 스크립트를 바꾸면 Level 1에 다음 검증을 포함한다.

```bash
make check-rpc-param-contract
```

- `check-rpc-param-contract`: RPC param-key 계약(진리원 `contracts/rpc_params.json`, 래칫 `contracts/rpc_param_baseline.json`). CLI/TUI/FE 전송키 ⊇ 핸들러 required 검사. 설계: `docs/superpowers/specs/2026-07-11-rpc-param-contract-gate-design.md`.

### 4.14 JSON 파싱 초크포인트 게이트

데몬 경계 파일(ws/uds/grpc/dispatcher/rest_server 및 rpc_utils) 또는 게이트 스크립트/baseline을 바꾸면 Level 1에 다음 검증을 포함한다.

```bash
make check-json-ingress
```

- `check-json-ingress`: 데몬 경계 5파일의 외부 JSON 파싱이 pcv_rpc_parse_guarded 초크포인트 경유(또는 PCV_PARSE_TRUSTED waiver)인지 검사. 깊이 가드 누락(원격 크래시) 재발 차단. 설계: `docs/superpowers/specs/2026-07-11-json-ingress-chokepoint-gate-design.md`.

---

## 5. Level 2: 단일 노드 실행 검증

### 5.1 필수 대상

- REST API 변경
- UDS/JSON-RPC 변경
- 인증/RBAC 변경
- VM lifecycle, storage, network, backup/restore 공통 기능 변경
- UI가 의존하는 `/health`, capability, 목록 조회 API 변경
- Single Edge 전용 기능 변경

### 5.2 최소 검증 항목

- 대상 서비스 기동 성공
- `/api/v1/health` 정상 응답
- 관련 API 또는 RPC 호출 정상 응답
- 관련 기능의 영속 상태, 부작용, audit/log 결과가 기대값과 일치
- 비동기 기능이면 accepted 응답과 worker 완료 결과를 같은 `job_id`로 대조
- journal 기준 치명 에러 없음
- 수동 또는 통합 테스트 1회 이상 통과

### 5.3 Single Edge 기준 핵심 확인

- `purecvisorsd` active
- `/health`의 `service`와 `capabilities.cluster=false` 일치
- VM lifecycle, storage, network, backup/restore, auth/rbac 중 관련 기능 확인
- 공개판 범위 밖 UI/API가 Single Edge 산출물에 기능 절차로 포함되지 않음

---

## 6. Level 3: 실환경 단일 노드 검증

### 6.1 필수 대상

- `scripts/deploy.sh`, systemd, 서비스 기동 경로 변경
- `/health`, 인증, bootstrap admin, 전용 admin 흐름 변경
- Web UI 재구성, 번들, PWA, 정적 자산 배포 변경
- OVS/OVN, overlay, backup/restore, longrun 같은 운영 검증 축 변경
- 출시 직전 재인증이 필요한 변경

### 6.2 최소 검증 항목

- 대상 호스트에서 `purecvisorsd` active
- `/api/v1/health` 정상 응답
- 관련 UI 또는 API 실제 호출 성공
- journal 기준 치명 오류 없음
- bootstrap admin 또는 전용 admin 인증 흐름 확인
- 변경 영향 축의 수동 시나리오 1회 이상 성공
- 변경 영향 축의 서비스 기능 시나리오에서 최종 상태, 데이터 무결성, 실패/거부 경로, cleanup 확인
- UI/PWA 변경 시 `/ui`, `/ui/`, `manifest.json`, `icon-192.png`, `icon-512.png`, vendored JS의 HTTP 상태와 MIME 확인
- UI 라우팅 변경 시 `/ui#ops-triage`와 `/ui#/ops-triage` 같은 hash deep link가 같은 화면으로 들어가는지 브라우저에서 확인
- UI/PWA 변경 시 로컬 파일 검증과 공개 URL 검증을 분리하고, 표준 도메인 `purecvisor.example.com`와 호환 도메인 `purecvisor-compat.example.com`를 운영 중이면 두 공개 `app.bundle.js`/`sw.js` 해시가 배포 산출물과 같은지 확인
- reverse proxy 보안 헤더 변경 시 CSP, Permissions-Policy, `X-Content-Type-Options` 실제 응답 확인
- `vm.clone` XML/disk 경로 변경 시 accepted 응답의 `source_disk`/`target_disk`/`job_id`/`guest_reset`, clone domain의 `shut off` persistent 상태, zvol CoW origin 또는 zvol full clone `origin=-`, full clone source 임시 snapshot 정리, qcow2/raw target file 독립성, guest reset 결과, audit `result=ok`, 데몬 CPU 안정 상태 확인

### 6.3 현재 기준 핵심 시나리오

1. 신규 빌드 단일 노드 배포
2. systemd 서비스 active 확인
3. `/health` 응답과 edition 표면 확인
4. 인증과 주요 API 흐름 확인
5. UI 또는 CLI의 핵심 운영 시나리오 확인
6. journal과 장시간 작업 결과 확인
7. UI 정적 자산 변경 시 브라우저 콘솔 기준 CSP/PWA/WebSocket/metrics 회귀 확인
8. 공개 도메인이 NAT 또는 reverse proxy 뒤에 있으면 `/api/v1/health`의 `node_name`, live bundle hash, 서비스 host를 대조해 실제 검증 대상이 운영 서버인지 확인한다. 2026-05-04 기준 표준 공개 URL은 `https://purecvisor.example.com`이고, edge nginx 호스트는 `pcv-prod-node-1`(`192.0.2.10`)이다. 호환 엔드포인트 `https://purecvisor-compat.example.com`를 함께 운영하면 `pcv-prod-node-2`(`192.0.2.20`)의 UI bundle/service worker 해시도 같은 릴리스 산출물인지 확인한다.
9. `vm.clone` 변경 시 준비된 zvol 템플릿으로 clone을 1회 이상 수행하고, 모든 성공 clone은 source VM shutoff를 확인한다. 변경한 모드가 `cow`이면 source snapshot origin 유지, `full`이면 target `origin=-`와 source 임시 snapshot 정리를 확인한다. qcow2/raw 또는 guest reset을 바꾼 경우 Ubuntu 24.04 non-LVM qcow2/raw와 Ubuntu 24.04 LVM qcow2/raw/ZFS zvol 기준으로 target disk 독립성, guest reset 결과, clone VM 자동 시작 금지, 수동 boot smoke 성공을 함께 확인한다.

### 6.4 완료 기준

다음이 모두 확인되어야 Level 3 완료다.

- 실제 서비스가 떠 있다.
- 변경 경로가 실환경에서 재현 가능하게 검증됐다.
- 운영 로그에 새로운 치명 오류가 없다.
- 결과가 실행 로그나 run log 형태로 남아 있다.

---

## 7. Level 4: 출시 게이트 검증

최소 다음 코어 기능 축을 기준으로 판단한다.

- `VM lifecycle`
- `storage`
- `network`
- `backup/restore`
- `auth/rbac`

골든 시나리오:

- VM 시작 성공
- 내부 접속 성공
- 내부 서비스 정상 동작
- 데이터 무결성 유지
- 내부/외부 네트워크 연동 정상
- 장애 이후 후속 lifecycle 작업 가능
- 기능별 영속 상태, 데이터 무결성, audit/log, cleanup 기준 통과
- 성능/longrun 결과와 기능 정합성 결과의 분리 기록

위 항목 중 하나라도 실패하면 출시를 중단한다.

---

## 8. 변경 유형별 필수 검증 매핑

| 변경 유형 | Level 1 | Level 2 | Level 3 | Level 4 |
|----------|---------|---------|---------|---------|
| 유틸리티/파서 수정 | 필수 | 선택 | 불필요 | 릴리스 시 포함 |
| REST/UDS 핸들러 수정 | 필수 | 필수 | 조건부 | 릴리스 시 포함 |
| Single Edge UI/API capability 수정 | 필수 | 필수 | 불필요 | 릴리스 시 포함 |
| VM lifecycle / storage / network / backup / auth 변경 | 필수 | 필수 | 조건부 | 필수 |
| 서비스 기능 시나리오 변경/누락 보강 | 필수 | 조건부 | 조건부 | 릴리스 시 포함 |
| `vm.clone` / clone plan / libvirt XML patch 변경 | `/vm_clone_plan` + `make test` + audit placement + cleanup guard 필수 | 필수 | 실제 zvol 원본 기준 clone 1회 필수. 모든 성공 clone의 source VM shutoff 확인 필수. qcow2/raw/guest reset 변경 시 Ubuntu 24.04 non-LVM qcow2/raw와 Ubuntu 24.04 LVM qcow2/raw/ZFS zvol full clone 검증 필수. Rocky/RHEL/SELinux enforcing은 문서상 후속 항목으로 유지 | 필수 |
| UI 시각 규격 / DESIGN.md / samples 변경 | `check_design_md.py` + `test_design_md_surface.sh` 필수 | 선택 | 조건부 | 릴리스 시 포함 |
| UI shell / 번들 / 정적 자산 변경 | 필수 | 필수 | 필수 | 릴리스 시 포함 |
| 배포 스크립트 / systemd / 서비스명 변경 | 필수 | 필수 | 필수 | 릴리스 시 포함 |
| 공개 릴리스 경계 변경 | 필수 | 조건부 | 조건부 | 필수 |
| 문서만 변경 | `git diff --check` 필수 | 불필요 | 불필요 | 릴리스 시 포함 |

---

## 9. 공개 릴리스 경계 검증

공개 릴리스 직전에는 최소한 다음을 통과해야 한다.

```bash
make clean
make single
PCV_NO_DEPLOY=1 scripts/bundle-ui.sh
python3 scripts/check_ui_bundle_fresh.py
node --check ui/app.bundle.js
python3 scripts/check_xss.py
tests/integration/test_single_ovn_ovs_layout.sh
tests/integration/test_single_ui_surface.sh
tests/integration/test_single_backend_build_boundaries.sh
```

산출물에는 다음 문자열이 없어야 한다.

```bash
strings bin/purecvisorsd | rg 'purecvisormd|make multi|vm\.migrate|cluster\.|federation\.site'
strings bin/pcvctl       | rg 'purecvisormd|make multi|vm\.migrate|cluster\.|federation\.site'
strings bin/pcvtui       | rg 'purecvisormd|make multi|vm\.migrate|cluster\.|federation\.site'
```

위 명령은 매칭이 없어 `rg` exit code `1`을 반환해야 정상이다. 문서 검증에서는 [PUBLIC_RELEASE_BOUNDARY.md](PUBLIC_RELEASE_BOUNDARY.md)와 [ADR_INDEX.md](ADR_INDEX.md)의 경계 설명 문구를 예외로 취급한다.

---

## 10. 증거 기록 규칙

검증은 통과 여부만 적지 않는다. 최소 다음 증거를 남긴다.

- 실행한 명령
- 실행 환경
- 성공/실패 exit code
- 실패 시 로그 위치와 원인
- 수동 검증이면 재현 가능한 입력과 기대 결과
- 서비스 기능 시나리오이면 시나리오 ID, 사전 조건, 최종 상태, 데이터 무결성, cleanup 확인 결과
