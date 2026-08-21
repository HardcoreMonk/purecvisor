# ADR-0035: pcvctl은 성공·실행 실패·사용법 오류를 종료상태로 구분한다

날짜: 2026-08-09
상태: Verified
Single Edge 적용 상태: 활성, production 구현·로컬 전체 게이트·운영 CLI 0/1/2 실측 통과

## 맥락

`pcvctl`의 route handler는 `void`이며 `route_exec()`는 handler 호출 후 항상 0을 반환한다.
UDS 연결 실패나 JSON-RPC 거절이 출력에는 보이더라도 운영 스크립트는 성공으로 판단한다.
이는 `set -e`, `&&`, 조건 분기와 CI의 실패 탐지를 무력화했고 CLI 파라미터 불일치가 장기간
숨은 근본 원인이 됐다.

## 결정

1. `pcvctl` 기본 종료상태는 `0=성공`, `1=실행·전송·프로토콜·RPC 실패`, `2=사용법·라우팅
   오류`로 한다.
2. `--strict-exit` 옵트인이나 호환성 유예 없이 즉시 기본 동작을 전환한다.
3. JSON-RPC 개별 오류코드는 프로세스 코드로 세분화하지 않고 응답 본문에 보존한다.
4. route 매칭 후 현재 명령 상태를 usage 후보로 시작하고, 첫 RPC가 시작되면 success 후보로
   바꾼다. runtime failure는 명령 종료까지 sticky다.
5. RPC 계층이 transport와 JSON-RPC envelope를 판정하며 출력 계층은 상태 정본이 아니다.
6. `void` handler 구조는 유지한다. RPC를 사용하지 않는 진단/검증 명령만 상태를 명시적으로
   기록한다.
7. batch는 모든 줄을 계속 실행하고 마지막 실패 코드를 반환한다. REPL은 개별 실패 때문에
   세션을 종료하지 않는다.

## 결과

- shell과 CI가 실패 응답을 non-zero로 탐지한다.
- 기존 exit 0 오판에 의존한 스크립트의 분기가 바뀌는 의도된 breaking change다.
- 중앙 분류로 출력 형식과 176개 handler의 개별 구현에 따른 상태 차이를 줄인다.
- code 1이 상세 원인을 구분하지 않으므로 필요한 호출자는 JSON 응답과 stderr를 함께 본다.

## 관련

- `tests/`의 CLI 종료 상태 효과 테스트
