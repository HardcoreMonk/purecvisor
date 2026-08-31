# 개발 단계별 검증 규칙

> **대상:** PureCVisor Single Edge
> **목적:** 기능 개발, 버그 수정, 릴리스 직전 검증을 같은 기준으로 운영하기 위한 공식 규칙
> **현행화 기준:** 2026-08-31
> **관련 문서:** [GUIDE.md](GUIDE.md), [PUBLIC_SOURCE_POLICY.md](PUBLIC_SOURCE_POLICY.md), [SERVICE_FUNCTIONAL_TEST_SCENARIOS.md](SERVICE_FUNCTIONAL_TEST_SCENARIOS.md), [PUBLIC_RELEASE_BOUNDARY.md](PUBLIC_RELEASE_BOUNDARY.md), [ADR_INDEX.md](ADR_INDEX.md), `docs/adr/`

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
7. 의미 있는 설계 근거는 ADR과 문서에 기록하고 공개 자체 소스에는 설명 주석을 추가하지 않는다.

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
- 소스 변경이면 [PUBLIC_SOURCE_POLICY.md](PUBLIC_SOURCE_POLICY.md)의 주석 제거 정책과 공개 경계를 확인

### 4.2.1 공개 소스 정책 게이트

자체 작성 C/H, UI JS/CSS/HTML, build/schema, ops/packaging, scripts와 tests를 바꾸면 다음 검증을 Level 1에 포함한다.

```bash
python3 scripts/strip_source_comments.py --check
bash tests/integration/test_public_comment_policy.sh
git diff --check
```

검사는 자체 소스의 설명 주석과 Python 문서화 문자열이 0건인지, UI 소스맵이 없는지 확인한다. `ui/vendor/`의 제3자 라이선스 고지와 실행에 필요한 shebang·AppArmor 전처리 지시문은 예외다.

리뷰어는 다음 조건을 확인한다.

- 신규 동작의 caller 계약, ownership, cleanup과 실패 경로가 테스트와 공개 문서에 반영됐는가?
- 사용자 데이터, 권한, 네트워크, 비동기 결과와 보안 경계의 변경 근거가 ADR 또는 계약 파일에 있는가?
- 공개 소스가 Single Edge 공개 경계와 충돌하지 않는가?

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

Web UI 시각 규격, `DESIGN.md`, `ui/style.css`, `ui/samples/`, `ui/docs.html`, `ui/guide.html`,
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

제품 UI의 시각 계층, 정보 구조, 사용자 흐름을 바꾸면 `DESIGN.md`와 관련 ADR을 근거로 변경 전후 화면을 검토한다. 실제 UI 변경 후에는 위 정적 게이트와 실브라우저 관측을 함께 기록한다.

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

`security.*` RPC, `src/modules/security/`, Security Events UI, CLI 보안 표면,
baseline, HIPS action, security DB schema를 바꾸면 Level 1에 다음을 포함한다.

```bash
make test
make check-rbac
bash tests/integration/test_security_cli_surface.sh
git diff --check
```

- security event JSON roundtrip 테스트
- baseline refresh audit 기록 테스트
- approve/dismiss RBAC 테스트
- `pcvctl security ...` 명령군 표면 테스트

fire-and-forget worker를 새로 추가하면 ADR-0018에 따라 `scripts/check_audit_placement.py`와
`scripts/check_ova_async_result.py`도 실행한다.

### 4.9 ADR-0023 `vm.clone` 안전 게이트

`vm.clone`, clone plan, libvirt XML disk source 판정, clone XML patch, ZFS snapshot/clone, qcow2/raw file copy, guest reset, Web UI clone 경로를 바꾸면 Level 1에 다음 검증을 포함한다.

```bash
make single
./test_runner -r /spawn_launcher
./test_runner -r /vm_clone_plan
make test
scripts/check_audit_placement.py
scripts/check_vm_clone_cleanup.py
PCV_NO_DEPLOY=1 scripts/bundle-ui.sh
python3 scripts/check_ui_bundle_fresh.py
for f in ui/app.js ui/modules/*.js ui/vendor/chart.umd.min.js ui/vendor/novnc/novnc.esm.js; do node -c "$f"; done
git diff --check -- src/api/dispatcher.c src/modules/virt/vm_clone_plan.c src/modules/virt/vm_clone_plan.h src/modules/storage/zfs_driver.c src/modules/storage/zfs_driver.h src/utils/pcv_spawn.c src/utils/pcv_spawn.h src/cli/purecvisorctl.c ui/modules/vm.js ui/app.bundle.js tests/test_spawn_launcher.c tests/test_vm_clone_plan.c scripts/check_vm_clone_cleanup.py docs/adr/0023-vm-clone-beta-safety-guard.md docs/DEVELOPMENT_VERIFICATION_POLICY.md docs/GUIDE.md docs/ADR_INDEX.md Makefile tests/test_main.c
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
rg -n "iconify|code\.iconify|api\.iconify|api\.unisvg|api\.simplesvg|cdn\.jsdelivr|fonts\.googleapis|fonts\.gstatic" ui/index.html ui/docs.html ui/guide.html ui/app.bundle.js ui/sw.js ui/vendor
rg -n "sourceMappingURL" ui/index.html ui/docs.html ui/guide.html ui/sw.js ui/vendor
rg -n "customConfirm\([^\\n]*<[^\\n]*>|<br><b>|idx \|\| selectedVmIndex" ui/modules
rg -n '<base href="/ui/">' ui/index.html
rg -n 'replace\(/\^#\\/\?|renderOpsTriage' ui/modules/uxlib.js ui/modules/monitor.js ui/app.bundle.js
```

앞의 세 `rg` 명령은 출력이 없어야 한다. 공개 번들은 소스맵과 `sourceMappingURL`을
생성하지 않는다. 회귀 패턴 검사(`customConfirm` 등)는 소스
`ui/modules`가 집행 지점이다 — 한 줄 민파이 번들에서는 `[^\n]*` 패턴이 파일 전체와
무의미하게 매치되는 오탐만 낸다. 운영 CSP를 완화해 외부 아이콘 API, CDN
JavaScript, 외부 sourcemap fetch를 허용하는 방향은 기본 금지한다. 필요한 브라우저 런타임 자산은 `ui/vendor/`, 로컬 이미지, inline SVG로 고정하고, 공통 아이콘은 로컬 `ui/vendor/coolicons/coolicons.svg` 스프라이트를 우선한다.

라이브 데몬 대상 `tests/integration/test_frontend_api.sh`는 **호스트 인자가 필수**다
(`bash tests/integration/test_frontend_api.sh <host>` 또는 `PCV_TEST_BASE_URL`).
기본값 `192.0.2.19`는 TEST-NET-1 fail-closed 플레이스홀더라 인자 없이 실행하면
전 항목 HTTP 000으로 실패하는 것이 정상이다.

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

- `check-dead-exports`: 헤더 선언된 비-static pcv_* 함수 중 .c 사용처 0(정의만)인 dead export 노출·신규차단(래칫 `scripts/dead_exports_baseline.txt`). "배선 안 된 안전 함수"(SEC-1형) 재발 차단.

사용처 카운트는 `src/**/*.c`만 집계하고 `tests/`는 포함하지 않는다. 따라서 정의되고 유닛 테스트로만 호출될 뿐 프로덕션 경로에 배선되지 않은 `pcv_*` 함수는 dead export로 잡힌다. 의도된 export는 `scripts/dead_exports_baseline.txt`에 명시한다.

### 4.13 RPC param-key 계약 게이트

dispatcher 핸들러, CLI 소비 콜사이트, 또는 `contracts/rpc_params*.json`/게이트 스크립트를 바꾸면 Level 1에 다음 검증을 포함한다.

```bash
make check-rpc-param-contract
```

- `check-rpc-param-contract`: RPC param-key 계약(진리원 `contracts/rpc_params.json`, 래칫 `contracts/rpc_param_baseline.json`). CLI/FE 전송키 ⊇ 핸들러 required 검사.

### 4.13.1 pcvctl 종료상태 효과 테스트

`src/cli/purecvisorctl.c`, `cli_rpc.[ch]`, `cli_output.[ch]` 또는 CLI 종료상태 테스트를
바꾸면 Level 1에 다음 검증을 포함한다.

```bash
make check-cli-exit-status
```

- ADR-0035의 정본은 `0=성공`, `1=실행·전송·프로토콜·RPC 실패`,
  `2=사용법·라우팅 오류`다.
- 테스트는 가짜 UDS에 production `bin/pcvctl`을 연결해 success, RPC error, malformed/empty
  response, missing socket, one-shot, JSON, batch의 실제 subprocess return code를 검증한다.
- Local VPC CLI는 같은 target에서 17개 action의 method/params, 네 출력 형식,
  accepted→terminal polling, failed/cancelled와 `--no-wait`를 추가 검증한다. 변경 명령의
  accepted만 보고 exit 0을 반환하거나 `jobs.get`을 제거하면 반드시 RED여야 한다.
- `vpc backends`는 read-only `vpc.backend.list`를 사용하고, `vpc create --backend ovn`은
  readiness가 false면 actual mutation 전에 실패해야 한다. backend 생략은 Linux와 동등해야 한다.
- stderr 문구나 정적 함수 존재만으로 통과시키지 않는다. 상태 분류 또는 route의 상태 소비를
  제거하면 RPC error가 다시 exit 0이 되어 반드시 RED여야 한다.

### 4.13.2 Local VPC CLI privileged 효과 게이트

설치본 `pcvctl`의 VPC mutation, terminal Job 대기 또는 Local VPC packet policy를 바꾼
경우 Level 3 운영 검증에 다음 명령을 포함한다.

```bash
PCV_VPC_CLI_LIVE=1 make test-vpc-cli-live
```

- 디스크 없는 임시 libvirt domain과 격리 namespace만 사용하며 사용자 VM·disk는 건드리지
  않는다.
- 설치본 CLI로 VPC·subnet·attachment를 만들고 같은 실제 ping이 `nat`에서는 성공하고
  `isolated` 전환 뒤 실패하는지 대조한다.
- root와 명시적 opt-in, `.100` 제외, CIDR·이름 충돌 preflight를 통과해야 실행한다.
- 성공과 실패 모두 namespace→attachment→VM→subnet→VPC 역순 cleanup 뒤 기준선으로
  돌아와야 한다. 실패 cleanup은 `PCV_VPC_CLI_LIVE_FAIL_AFTER=nat`로 검증할 수 있다.
- host networking과 libvirt를 실제 변경하므로 기본 `make test`, `check-all`, pre-commit에는
  포함하지 않는다.

### 4.13.3 Local VPC Web UI/API 계약 게이트

`src/api/rest_server.c`의 VPC REST adapter, `ui/modules/vpc.js`, VPC endpoint/nav/help 또는
관련 반응형 CSS를 바꾼 경우 Level 1에 다음 검증을 포함한다.

```bash
python3 scripts/tests/test_rpc_extract.py
node --test tests/ui/vpc.test.mjs
make check-rbac
make check-rpc-param-contract
make check-rpc-consumers
python3 scripts/check_help_counts.py
python3 scripts/check_ui_bundle_fresh.py
```

- route 추출 테스트는 기존 16개 VPC 작업과 backend capability GET이 누락·오매핑되지 않는지
  확인한다.
- UI 테스트는 aggregate/child 관계, VIEWER read-only, mutation payload, optimistic revision,
  backend readiness/capacity, 모달 내부 즉시·terminal 오류, 입력 보존과 1024/768/480px
  overflow를 실제 Chromium에서 검증한다.
- 제품 UI 구조나 CSS를 바꿨다면 별도 로컬 UI 리뷰와 4개 viewport 캡처를 남긴다.

### 4.14 JSON 파싱 초크포인트 게이트

데몬 경계 파일(ws/uds/grpc/dispatcher/rest_server 및 rpc_utils) 또는 게이트 스크립트/baseline을 바꾸면 Level 1에 다음 검증을 포함한다.

```bash
make check-json-ingress
```

- `check-json-ingress`: 데몬 경계 5파일의 외부 JSON 파싱이 `pcv_rpc_parse_guarded` 초크포인트를 경유하는지 검사하고, 공개 기준의 불가피한 직접 파싱은 `scripts/json_ingress_baseline.txt`로 고정한다.

### 4.15 RPC 소비-완전성 + 고아 게이트

dispatcher 핸들러 등록, CLI/`rest_server.c`/`grpc_server.c`/Web UI(`ui/*.js`) 소비 콜사이트, 또는 `contracts/rpc_orphan_baseline.json`/게이트 스크립트를 바꾸면 Level 1에 다음 검증을 포함한다.

```bash
make check-rpc-consumers
```

- `check-rpc-consumers`는 CLI/FE-인라인/FE-passthrough/REST/gRPC 전 경로 소비를 대조하며(소비⊆등록), 등록됐으나 전 경로 미소비인 메서드를 고아로 래칫한다(`contracts/rpc_orphan_baseline.json`).
- **리뷰 체크리스트**: 신규 RPC 핸들러는 실인터페이스 소비자(CLI/FE-인라인/FE-passthrough/REST/gRPC 중 하나)를 동반하거나, 고아 baseline에 사유 주석(`reason`)으로 등재해야 한다 (ADR-0025 배선=완료).

### 4.16 안전통제 효과 테스트 레지스트리 게이트

안전 통제 구현 또는 `contracts/safety_controls(.json|_baseline.txt)`/게이트 스크립트를 바꾸면 Level 1에 다음 검증을 포함한다.

```bash
make check-safety-controls
```

- `check-safety-controls`: `contracts/safety_controls.json`에 등록된 안전 통제가 효과 테스트를 갖는지 검사해 "보고성공 무동작" 재발을 차단한다.
- **리뷰 체크리스트**: 새 안전 통제 추가 시 레지스트리 등록과 효과(무동작→실동작) 단언 테스트를 함께 작성한다.

### 4.17 raw 에러코드 리터럴 방지 게이트

`src/**/*.c` 또는 게이트 스크립트/baseline(`scripts/check_error_codes.py`, `scripts/error_codes_baseline.txt`)을 바꾸면 Level 1에 다음 검증을 포함한다.

```bash
make check-error-codes
```

- `check-error-codes`: RPC wire 에러코드 raw `-32xxx` 숫자 리터럴(문자열 리터럴과 enum 정의부 `rpc_utils.h` 제외)이 실코드에 신규 등장하는지 검사(래칫 `scripts/error_codes_baseline.txt`, 이상적으로 빈 파일)하고 구 병렬 enum `PCV_ERR_*` 재도입을 차단한다.
- **리뷰 체크리스트**: 신규 에러 응답/감사 로그 사이트는 raw `-32xxx` 리터럴이 아니라 `rpc_utils.h`의 canonical `PURE_RPC_ERR_*` 상수를 사용해야 한다(값 보존 필요 시에도 이름은 상수로).

### 4.18 컨테이너 operator owner-scope 게이트 (B1 / A01 IDOR)

`src/api/dispatcher.c`, `src/modules/dispatcher/handler_container.c`, `src/modules/lxc/lxc_owner.(c|h)`, 또는 게이트 스크립트/자기검증(`scripts/check_container_owner_scope.py`, `scripts/tests/test_container_owner_scope.py`)을 바꾸면 Level 1에 다음 검증을 포함한다.

```bash
make check-container-owner-scope
```

- `check-container-owner-scope`: VM operator owner-scope(자기 소유 VM만 조작)를 컨테이너로 미러한 접근통제를 검사. ① 강제(dispatcher): `_container_method_requires_owner_scope` 세트에 `container.start`/`container.stop`/`container.clone`이 모두 포함되고, 게이트 함수(`_lookup_container_owner`/`_container_owner_matches_caller`/`_container_owner_scoped_method_allowed`)가 정의·디스패치 배선되어 있는지. ② 스탬프(handler): `container.create` 성공 경로가 `pcv_lxc_stamp_owner`로 소유자를 기록하는지. ③ 저장소(lxc_owner): `pcv_lxc_stamp_owner`/`pcv_lxc_read_owner` 정의 + `purecvisor.owner` 파일 규칙 실재. 소유자는 libvirt domain이 없는 컨테이너 특성상 `<container_path>/<name>/purecvisor.owner`에 저장한다(VM은 domain XML `pcv:owner`). 자기검증 `scripts/tests/test_container_owner_scope.py`가 세트 제거·배선 제거·스탬프 제거·저장소 제거 각각에 대해 반사실 RED를 확인.
- **하위호환 주의**: `.owner` 파일이 없는 기존 컨테이너(및 UDS 직결 admin 생성분)는 operator 접근이 거부되고 admin만 조작·재스탬프할 수 있다(VM 소유자 metadata 부재와 동일 fail-secure). upgrade 시 operator는 기존 컨테이너 접근을 잃으므로 admin 재스탬프/재생성이 필요하다.
- **리뷰 체크리스트**: operator가 단일 컨테이너를 조작하는 신규 메서드를 추가하면 owner-scope 세트(`_container_method_requires_owner_scope`)에 포함하거나, 제외 사유(admin-only 등)를 남긴다. `container.clone`은 RBAC 정책 테이블에 min-role 매핑이 없어 현재 VIEWER 기본으로 처리되므로, owner-scope는 operator 교차테넌트만 차단한다(별도 RBAC min-role 매핑 필요 — 후속).

### 4.19 런타임 전제 배포 게이트

`scripts/install-runtime-prereqs.sh`, `scripts/deploy.sh`, 런타임 인증 자산 또는
BPF 배포 계약을 바꾸면 Level 1에 다음 검증을 포함한다.

```bash
make check-runtime-prereqs
```

두 integration test는 임시 디렉터리에 fixture BPF 자산을 합성하므로 BPF를
다시 빌드하거나 실제 호스트에 배포하지 않는다. 실제 배포 경로에서는 이와 별도로
현재 `build/bpf/` 자산을 preflight하고, 원격 접속 전에 helper의 `--verify-only`
검증을 실행한다. 게이트는 다음 계약을 함께 확인한다.

- `PKI` 디렉터리와 daemon 설정 권한을 교정하되, 유효한 인증서·개인 키와
  `JWT` secret을 포함한 기존 자산의 내용은 재배포에서도 보존한다.
- BPF manifest schema와 각 객체의 `BPF SHA-256`을 설치 전에 검증하고,
  불일치나 잘못된 입력은 설치된 BPF generation을 변경하지 않은 채 실패한다.
- `--verify-only`는 staging 자산만 검사하며 설치 대상 파일시스템에 접근하지
  않는다. 검증 실패는 SSH/SCP와 로컬 `sudo` 실행 전에 배포를 중단한다.
- 원격 staging은 mode `0700`으로 만들고, 성공·실패·중단 모든 경로에서
  helper와 BPF 자산을 정리한다. helper 실패 뒤에는 서비스를 시작하지 않는다.

`make check-all`은 이 bounded fixture 검증을 `check-runtime-prereqs` 게이트로
실행한다(서수는 적지 않는다 — 게이트가 늘 때마다 어긋난다. 정본은 Makefile 의
`check-all:` 의존 목록이다). 따라서 런타임 전제 또는 배포 경로 변경은 개별 게이트와
상위 게이트를 모두 통과해야 완료로 판정한다.

### 4.20 공유 물리 NIC 브리지 데이터패스 게이트

ADR-0044의 shared controller, BPF classifier, portal/state/reconcile 또는 VM bridge NIC XML을
바꾸면 root 격리 네임스페이스에서 실제 커널 packet path를 실행한다.

```bash
make bpf test_runner
sudo env PCV_SHARED_BRIDGE_LIVE=1 \
  ./test_runner -p /network/shared_bridge_packet_path --verbose
```

이 테스트는 사용자 NIC를 사용하지 않고 veth·network namespace를 만들며 host↔LAN 연결을
TC-BPF 적용 전·중·해제 후 모두 확인한다. host↔guest, LAN↔guest, guest↔LAN과 upstream
dnsmasq DHCP DORA 중 하나라도 실패하면 shared 배포를 중단한다. 실제 노드 Level 3에서는
추가로 생성 전후 host NIC의 master, 주소, IPv4/IPv6 route, DNS, MAC, MTU 불변과 상위
스위치의 다중 MAC 허용 여부를 독립적으로 기록한다.

검증용 guest MAC과 DHCP 주소는 실행 자원이다. namespace/tap/veth와 함께 정리하고 영구 VM
identity 또는 제품 예약 주소처럼 문서화하지 않는다. daemon restart 뒤 desired state와
owned filter가 멱등 수렴하는지 확인하되, 실제 KVM VM NIC와 host reboot를 통과하기 전에는
ADR을 `Verified`로 승격하지 않는다. 결정 기준은 ADR-0043과 ADR-0044를 따른다.

### 4.21 nginx 외부 TLS 종료 역할 분리

전송 모드, 프록시 신원 해석, nginx 설치 트랜잭션 또는 systemd 시작 정책을 바꾸면
Level 1에서 다음 명령을 모두 실행한다.

```bash
make test_runner
./test_runner -r /rest_transport
python3 scripts/tests/test_proxy_identity.py
bash tests/integration/test_nginx_termination_install.sh
make check-runtime-prereqs
```

Level 2에서는 daemon이 루프백 HTTP만 수신하고 health가 `status=ok`,
`mode=external_termination`, `enabled=false`, `degraded=false`,
`status=disabled_by_config`를 반환하는지 확인한다. 비루프백 peer의
`X-Forwarded-For`와 `X-Forwarded-Proto`가 무시되는 실패 시나리오도 포함한다.

Level 3에서는 LAN HTTPS 호출, IP 미준비 시 `exit 75` 재시도, nginx 문법 오류 시
`exit 1`과 `RestartPreventExitStatus=1` 재시작 방지, 실패 rollback, 기존
인증서·키 보존을 실제 노드에서 확인한다. nginx 패키지 업데이트 뒤에는 vendor
유닛의 `ExecStartPre`와 PureCVisor drop-in의 합성 결과를 다시 확인한다.

`PCV-NGINX-TRUST-BOUNDARY: host-loopback`을 전제로 하며,
`PCV-NGINX-COUNTERFACTUAL: untrusted-local-process` 검증에서는 신뢰하지 않는
로컬 프로세스가 루프백에 직접 접속하면 전달 헤더를 위조할 수 있음을 확인한다.
그런 프로세스를 허용하는 호스트는 외부 종료 모드 부적합으로 판정한다. 운영
검증에는 sandbox/MAC·로컬 방화벽·network namespace 중 적용한 직접 접근 제한과,
원격 peer의 전달 헤더가 신뢰되지 않는 대조군을 포함한다.

### 4.22 iSCSI initiator CHAP node DB 갱신

`iscsi_manager.c`, `pcv_iscsi_node_db.[ch]`, open-iscsi node DB/lock 계약 또는 CHAP argv
회귀 게이트를 바꾸면 Level 1에서 다음 명령을 모두 실행한다.

```bash
make test_runner
./test_runner -r /iscsi/node_db -v
./test_runner -r /iscsi -v
make check-iscsi-chap-argv
python3 scripts/check_secret_wipe.py
```

fixture는 임시 node/lock root만 사용하며 실제 `/var/lib/iscsi`·`/etc/iscsi`, 세션, 운영 노드를 변경하지
않는다. 같은 target/address/port의 모든 TPGT·iface 갱신, 0600 atomic replace, 실행 중 오류
rollback, lock timeout, symlink·중복 key·무매치 거부와 IPv4/IPv6 portal 파싱을 확인한다.

Level 3은 별도 승인된 격리 호스트에서 정상·실패 CHAP login/logout과 cleanup을 수행하고,
실행 중 `/proc` argv, journal/audit, private temp 잔여를 독립 관측한다. 제외되거나 승인되지
않은 운영 호스트는 검증 대상으로 사용하지 않으며, 이 live 증거 전에는 관련 spec/ADR을
`Verified`로 승격하지 않는다.

### 4.23 호스트 네트워크 기준선과 generic OVN 계약 게이트

호스트 네트워크 inventory, `src/api/rest_server.c`의 network/OVN adapter, dispatcher의
`ovn.*` 등록, OVN manager/handler, CLI·Web UI 도움말 또는 공개 네트워크 가이드를 바꾸면
Level 1에 다음 검증을 포함한다.

```bash
make test
make check-rbac
bash tests/integration/test_ovn_sdn.sh
bash tests/integration/test_single_ui_surface.sh
```

정적·격리 검증은 다음 계약을 함께 확인한다.

- `GET /api/v1/networks/host-baseline`과 Web UI `호스트 네트워크 기준선`이 관리
  interface/IP, IPv4 main route·connected CIDR, Linux bridge/port, OVS bridge/port와 현재
  tenant VPC CIDR을 읽기 전용으로 노출한다.
- dispatcher에 등록된 generic OVN은 정확히 18개다. 상태 1, switch 4, port 2, ACL 2,
  router 5, DHCP 1, NAT 2, tenant 1의 합계와 메서드 이름을 동시에 대조한다.
- logical switch 생성은 L2 리소스만 만들며 subnet 입력을 받지 않는다. DHCP는
  `ovn.dhcp.enable`로 분리하고 switch ownership marker를 기록한다.
- switch 삭제는 같은 ownership marker의 DHCP option을 한 transaction에서 정리하고,
  다른 switch와 foreign 행을 보존한다. 모호한 조회·비정상 UUID·안전 상한 초과는 실제
  mutation 전에 거부한다.
- 인증 REST `GET /api/v1/ovn/acl?switch=...`와
  `GET /api/v1/ovn/nat?router=...`가 filter를 canonical RPC params에 전달한다. 누락·빈
  filter는 전체 목록으로 넓히지 않고 canonical `-32602`로 거부한다.
- 미등록 ACL/NAT/DHCP/tenant 역동작, router port 제거, 미완성 OVN/NFV Load Balancer와
  production caller가 없는 VM 자동 포트 helper를 UI·CLI·도움말·가이드에서 사용자
  기능으로 노출하지 않는다.

Level 3의 capability-gated 검증은 승인된 격리 호스트에서만 다음처럼 실행한다.

```bash
sudo env PCV_OVN_LIVE=1 bash tests/integration/test_ovn_live.sh
sudo env PCV_OVN_REST_FILTERS_LIVE=1 bash tests/integration/test_ovn_rest_filters_live.sh
sudo env PCV_OVN_DHCP_CLEANUP_LIVE=1 bash tests/integration/test_ovn_switch_dhcp_cleanup_live.sh
```

실환경 결과는 C0 기준선, C1 switch/port/DHCP, C2 router/L3/NAT, C3 ACL/tenant/filter,
C4 ownership cleanup 순서로 기록한다. 정상 경로의 emergency cleanup과 종료 residue가
0이어야 한다. NAT용 external gateway port fixture는 멀티 노드 gateway scheduling의
제품 근거로 사용하지 않는다. 이 generic OVN 결과는 Local VPC OVN backend의 부팅 KVM,
Linux/OVN 공존, controller/host reboot와 전 단계 fault injection을 대신하지 않는다.

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
- UI/PWA 변경 시 `/ui`, `/ui/`, `docs.html`, `guide.html`, `guide-content.md`, `manifest.json`, `icon-192.png`, `icon-512.png`, vendored JS의 HTTP 상태와 MIME 확인
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
10. physical shared bridge 변경 시 생성 전후와 daemon restart 뒤 host NIC의 master·주소·route·DNS·MAC·MTU를 비교하고, 독립 guest MAC의 upstream DHCP·gateway·host 통신을 확인한다. 검증용 guest namespace/tap/veth와 MAC·주소는 정리하며 제품 `pcvbr0` desired state를 남길지 삭제할지는 handoff에 명시한다.
11. Local VPC OVN backend 변경 시 schema migration, NB/SB/northd/controller/chassis/`br-int`
    readiness, 제품 소유 external ID, LS/LR/DHCP/LSP/Port Group/ACL, host edge와 공통 nft를
    대조한다. 부팅 KVM DHCP·L2/L3, Linux/OVN 공존, daemon/controller/host restart와 단계별
    fault injection이 없으면 `Implemented` 이상으로 올릴 수 있어도 공개 지원이나
    `Verified`로 판정하지 않는다.
12. generic OVN 또는 네트워크 inventory 변경 시 host baseline을 먼저 기록하고 C0~C4에서
    `NET-OVN-01~07`, 등록 RPC 18개, DHCP ownership cleanup, REST ACL/NAT filter,
    canonical `-32602`와 최종 residue 0을 대조한다.

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
| Local VPC backend/schema/OVN ownership 변경 | C model/store/policy/adapter + CLI 17 action + UI/backend capability + RBAC·audit 필수 | schema migration·reconcile 필수 | OVN 제품 packet·ownership·cleanup 필수, `Verified`는 부팅 KVM·공존·controller/host reboot·fault injection까지 | 필수 |
| host baseline / generic OVN RPC·DHCP·REST filter 변경 | 정확한 18 RPC inventory + `test_ovn_sdn.sh` + UI/RBAC 경계 필수 | host baseline·canonical 오류·소유 cleanup 필수 | C0~C4 `NET-OVN-01~07`, packet·filter·residue 0 필수 | 필수 |
| physical `bridge/dedicated`·`bridge/shared` controller/BPF/VM NIC 변경 | `make bpf test_runner` + shared packet-path + 관련 반사실 C/UI 게이트 필수 | reconcile·inventory 필수 | host 불변 비교·upstream DHCP 필수, `Verified`는 실제 KVM VM·reboot까지 | 필수 |
| 서비스 기능 시나리오 변경/누락 보강 | 필수 | 조건부 | 조건부 | 릴리스 시 포함 |
| `vm.clone` / clone plan / libvirt XML patch 변경 | `/vm_clone_plan` + `make test` + audit placement + cleanup guard 필수 | 필수 | 실제 zvol 원본 기준 clone 1회 필수. 모든 성공 clone의 source VM shutoff 확인 필수. qcow2/raw/guest reset 변경 시 Ubuntu 24.04 non-LVM qcow2/raw와 Ubuntu 24.04 LVM qcow2/raw/ZFS zvol full clone 검증 필수. Rocky/RHEL/SELinux enforcing은 문서상 후속 항목으로 유지 | 필수 |
| UI 시각 규격 / DESIGN.md / samples 변경 | `check_design_md.py` + `test_design_md_surface.sh` 필수 | 선택 | 조건부 | 릴리스 시 포함 |
| UI shell / 번들 / 정적 자산 변경 | 필수 | 필수 | 필수 | 릴리스 시 포함 |
| 배포 스크립트 / systemd / 서비스명 변경 | 필수 | 필수 | 필수 | 릴리스 시 포함 |
| 공개 릴리스 경계 변경 | 필수 | 조건부 | 조건부 | 필수 |
| 문서만 변경 | `git diff --check` 필수 | 불필요 | 불필요 | 릴리스 시 포함 |
| 공개 Pages 문서·navigation·계약 변경 | `cd site && npm run check` 필수 | 불필요 | Pages 배포 시 live route 확인 | 릴리스 시 포함 |

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

`tests/integration/test_single_ui_surface.sh`는 일반 로컬 OVN 관리 표면을 보존하면서 종료된
공개 OVN 데모 endpoint·도메인·고정 자산·전용 SVG가 부활하지 않는지도 함께 검증한다.

산출물에는 다음 문자열이 없어야 한다.

```bash
strings bin/purecvisorsd | rg 'purecvisormd|make multi|vm\.migrate|cluster\.|federation\.site'
strings bin/pcvctl       | rg 'purecvisormd|make multi|vm\.migrate|cluster\.|federation\.site'
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
