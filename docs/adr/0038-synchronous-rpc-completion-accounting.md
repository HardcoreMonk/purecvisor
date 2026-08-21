# ADR-0038: 동기 RPC 완료 결과를 canonical response envelope에서 집계한다

- **상태:** Verified
- **일자:** 2026-08-11
- **Single Edge 적용 상태:** 운영 배포와 라이브 효과 검증 완료
- **관련:** ADR-0018, ADR-0025, AUDIT-F2

## Context

dispatcher는 동기 handler가 실제로 보낸 JSON-RPC response를 관측하지 않고 audit와
Prometheus를 항상 성공으로 끝냈다. 핸들러 반환형을 전면 변경하면 수백 개 route와
fire-and-forget callback 계약을 동시에 흔든다. 반면 모든 정상 handler response는 canonical
success/error builder를 통과한다.

## Decision

1. dispatcher가 요청마다 thread-local completion scope를 연고 닫는다.
2. canonical response builder가 현재 scope에 success 또는 error code를 기록한다.
3. dispatcher는 관측된 envelope 결과로 동기 RPC audit와 Prometheus success/error를
   결정한다.
4. 같은 method의 직접 audit가 scope 안에서 발생한 경우 dispatcher 자동 audit만 생략한다.
   다른 method·semantic event와 worker thread audit는 억제하지 않는다.
5. 동기 요청에서 response가 관측되지 않으면 성공으로 추정하지 않고 internal failure로
   집계한다.
6. fire-and-forget 실제 결과 audit는 계속 ADR-0018의 worker callback이 소유한다.
7. 응답이 callback에서 나중에 오는 async 메서드는 반환 시점 미관측을 정상 dispatch로
   집계한다. 단, 즉시 validation error envelope가 관측되면 그 오류를 그대로 보존한다.

## Consequences

- 핸들러 ABI를 유지하면서 실제 wire 결과와 audit/metric이 일치한다.
- response builder와 audit entry point가 completion 관측 hook을 갖는다.
- 새 응답 생성 우회는 단순 스타일 위반이 아니라 관측 무결성 결함이므로 기존 canonical
  builder 규칙과 테스트가 함께 차단해야 한다.

## Verification

성공·invalid params·handler 내부 실패를 production dispatcher로 실행해 wire error code,
audit result/code, Prometheus label이 일치함을 검증한다. 관측 hook 제거 시 실패 요청이 다시
성공으로 기록되는 반사실을 유지한다.

2026-08-12 운영 설치본에서 정상·invalid params·내부 실패의 wire/audit/metric 일치와
async 성공 metric을 실측했다. completion 5/5, 전체 C 1330/1330, audit startup 5/5와
계약 게이트와 배포 identity 검증을 통과해 `Verified`로 승격했다.
