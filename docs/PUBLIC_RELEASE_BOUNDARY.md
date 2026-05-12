# Single Edge 공개 릴리스 경계

> **대상:** `purecvisor` Single Edge
> **현행화 기준:** 2026-05-08
> **판정 목적:** 공개 전에 Single Edge 공개 범위 밖 기능이 산출물, 소스, 문서에서 기능 절차로 노출되지 않는지 확인한다.

---

## 1. 기본 판정

`purecvisor`는 Linux/KVM 기반 Single Edge 공개판이다. 이 리포에서 유지하는 기능은 단일 노드 하이퍼바이저 운영, 로컬 VM/컨테이너/스토리지/네트워크/백업/인증/UI 기능으로 제한한다.

범위 밖 기능은 이 리포에서 출시 판정을 내리지 않는다. 별도 상용판과 비공개판의 출시 판정은 해당 독립 리포에서 수행해야 한다.

표준 공개 운영 URL은 `https://purecvisor.example.com`이다. `https://purecvisor-compat.example.com`는 호환 공개 엔드포인트로 유지할 수 있지만, 문서 예시와 신규 운영 기준은 `purecvisor.example.com`를 우선한다. 호환 엔드포인트를 운영할 때도 같은 Single Edge 산출물, 같은 UI bundle/service worker 해시, 같은 공개판 경계를 만족해야 한다.

---

## 2. Single Edge 허용 표면

- 빌드 타깃: `make single`, `make test`, `make release`
- 데몬: `purecvisorsd`
- 실행 모델: 단일 프로세스 + `GMainLoop`
- REST/UDS: 단일 노드 API와 JSON-RPC
- UI: Single Edge 메뉴, Single Edge endpoint registry, `운영 > 이벤트 센터` 같은 단일 노드 운영 triage 화면
- 운영 스크립트: 로컬 단일 노드 설치와 선택적 원격 단일 노드 배포
- 실패 가드: `make multi`는 잘못된 에디션 빌드를 차단하기 위해 exit code `2`로 실패해야 한다.

---

## 3. 공개판 금지 표면

다음 항목은 `purecvisor` 공개판에 기능 구현 또는 사용자 절차로 포함되면 안 된다.

- Multi Edge 전용 데몬명: `purecvisormd`
- Multi 빌드 권장 또는 성공 경로: `make multi`, `EDITION=multi`
- 클러스터 구현 파일: `src/modules/cluster/`, `src/modules/dispatcher/handler_cluster.*`
- 라이브 마이그레이션 구현 파일: `src/modules/virt/vm_migrate.*`
- Multi overlay 자동화 파일: `ovn_multi_auto.c`, `ovs_overlay_multi_auto.c`
- UI 클러스터 모듈: `ui/modules/cluster.js`
- 공개 산출물의 Multi/Cluster RPC 표식: `vm.migrate`, `cluster.*`, `federation.site.*`

예외:

- `Makefile`의 `multi` 실패 가드처럼 금지 표면을 차단하기 위한 문구는 허용한다.
- ADR과 독립 리포 생성 배경 문서의 역사 기록은 허용하되, 현재 운영 절차로 읽히지 않도록 [ADR_INDEX.md](ADR_INDEX.md)나 해당 문서 상단에서 상태를 분명히 표시해야 한다.
- `capabilities.cluster=false`처럼 Single Edge 상태 판정을 위해 필요한 필드명은 허용한다.

---

## 4. 필수 검증

공개 릴리스 직전에는 최소한 다음을 통과해야 한다.

```bash
make clean
make single
PCV_NO_DEPLOY=1 scripts/bundle-ui.sh
python3 scripts/check_ui_bundle_fresh.py
node --check ui/app.bundle.js
python3 scripts/check_xss.py
python3 scripts/check_design_md.py
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

위 명령은 매칭이 없어 `rg` exit code `1`을 반환해야 정상이다. 단, 문서 검증에서는 이 파일과 [ADR_INDEX.md](ADR_INDEX.md)의 경계 설명 문구는 예외로 취급한다.

---

## 5. `/health` 판정 기준

Single Edge `/api/v1/health`는 다음을 만족해야 한다.

- `service`: `purecvisorsd`
- `capabilities.cluster`: `false`
- 레거시 `[cluster]` 설정이 남아 있어도 Single Edge capability가 `true`로 바뀌지 않음

---

## 6. 문서 판정 기준

공개 문서는 Single Edge 사용자가 그대로 따라 할 수 있어야 한다.

- 설치와 실행 예시는 `purecvisorsd` 기준으로 작성한다.
- 범위 밖 기능은 구현 방법이 아니라 “Single Edge 공개판 제외 범위”로만 언급한다.
- 클러스터 HA, 페더레이션, 라이브 마이그레이션 절차는 Single Edge 공개 문서의 기능 장으로 작성하지 않는다.
- `docs/GUIDE.md`와 `ui/guide-content.md`는 같은 기능 표면을 설명해야 한다.
- `README.md`는 빠른 시작과 경계 요약만 담고 상세 운영 절차는 `docs/GUIDE.md`로 연결한다.

---

## 7. 공개 저장소 생성 기준

LinkedIn/GitHub 공개용 저장소는 기존 개발 저장소의 `.git` 이력을 그대로 공개하지 않는다.

기본 공개 경로:

1. 현재 HEAD에서 민감정보 검색과 공개 범위 검증을 통과한다.
2. `git archive HEAD`로 소스 트리만 추출한다.
3. 추출한 디렉터리에서 `git init` 후 첫 커밋을 만든다.
4. 새 공개 저장소 remote에 push한다.

이 방식은 기존 commit hash, 운영 실기록, 과거 실수, 내부 작업 로그가 공개 저장소 object로 유입되는 일을 차단하기 위한 기준이다. 기존 개발 저장소의 remote를 그대로 public으로 전환하지 않는다.
