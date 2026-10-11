# ADR-0079: dispatcher 기본 거부와 VM 대상 별칭 단일 대상 규칙

- **상태:** Implemented
- **일자:** 2026-10-11
- **적용:** 공개 Single Edge 소스 2.0.1. 구현 반영이며 전체 환경 인증을 뜻하지 않는다.
- **관계:** ADR-0019(UDS 우회 정책과 메서드명 기반 RBAC)를 보강한다.

## 원칙

권한 게이트는 실제로 실행될 대상과 같은 값을 판정한다. 게이트가 읽는 값과 실행 경로가 읽는
값이 갈리면 게이트를 통과한 요청이 다른 대상에 실행된다. 판정할 수 없거나 모호한 요청은
허용하지 않는다.

## 결정

1. `src/api/dispatcher.c`의 정책표(`g_method_policies`)에 없는 메서드는 ADMIN만 실행한다.
   이전 기본값은 VIEWER였다. VIEWER 행(값 0)과 "행 없음"은 `g_hash_table_lookup_extended()`로
   구분한다. 전환 시점에 정책 행이 없던 라우트 72개는 REST 앞단(`src/modules/auth/pcv_rbac.c`)과
   같은 등급으로 명시 매핑했다. 새 라우트는 정책 행을 넣지 않으면 ADMIN으로 잠긴다.
2. owner-scope 대상 VM 요청은 대상 VM을 하나만 가리켜야 한다. 별칭 키(`name`, `vm_id`,
   `vm_name`, `vm`, `vm.clone`의 `source`)의 비어 있지 않은 값이 서로 다르거나 문자열이 아니면
   역할과 무관하게 `-32602`로 거절하고 audit에 `fail`(source `rbac`)로 기록한다. 그러면 핸들러가
   어느 키를 읽든 소유자 확인을 받은 VM이다. `vm.batch`(대상은 `vms` 배열)는 적용하지 않는다.
   새 이름 키(`new_name`, `clone_name`, `target`)와 스냅샷 이름은 별칭이 아니다.
3. REST `/api/v1/rpc` 권한 판정은 원문 문자열 검색이 아니라 파싱된 최상위 객체의 문자열 멤버
   `method`로 한다(`pcv_rest_rpc_envelope_method()`). 누락은 400 `Missing method field`,
   비객체·비문자열·빈 값은 400 `Malformed method field`다. 타임아웃 오버라이드도 같은 값을 쓴다.

## 영향

- REST 경로의 실효 권한은 max(REST, dispatcher)라 바뀌지 않는다.
- REST를 거치지 않는 경로는 더 엄격해진다. gRPC의 `AuditSearch`(`audit.search`)와
  `Version`(`daemon.version`)은 ADMIN role이 필요하고, 정책 행이 없는 플러그인 메서드도 ADMIN
  전용이다.
- 상충 별칭을 보내는 외부 스크립트는 ADMIN이어도 `-32602`를 받는다.

## 검증

- `tests/test_vpc_rbac.c`: `/dispatcher/rbac/default_deny_unmapped`,
  `/dispatcher/rbac/formerly_unmapped_match_rest`(72개 행을 임시 사용자 DB의 실제 REST 판정과 대조),
  `/dispatcher/rbac/vm_target_alias_single_target`.
- `tests/test_rest_transport.c`: `/rest_transport/rpc_method_gate`.
- `make check-rbac`: 라우트 ⊆ 정책표, owner-scope 앞 별칭 검사 배선, REST 게이트의 envelope
  파싱 배선과 원문 문자열 method 추출 재도입 금지. 반례 fixture는
  `scripts/tests/test_check_rbac_policies.py`.
