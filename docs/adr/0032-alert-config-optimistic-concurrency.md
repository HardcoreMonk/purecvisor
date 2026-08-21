# ADR-0032: 알림 설정 변경은 revision 비교와 전체 유효성 검사를 한 원자 구간에서 수행한다

날짜: 2026-07-24
상태: Verified
Single Edge 적용 상태: 활성

## 맥락

`alert.config.set`은 부분 설정을 받지만, 기존 구현은 요청에 포함된 필드를 전역 상태에
하나씩 즉시 복사했다. 이 방식에는 두 종류의 운영 위험이 있었다.

1. 두 운영자가 같은 설정을 읽고 차례로 저장하면 뒤 요청이 앞 요청을 조용히 덮어쓸 수 있다.
2. 한 요청 안에서 앞 필드는 반영되고 뒤 필드 검증이 실패하면 Warning/Critical 쌍과
   Webhook 라우팅이 서로 다른 세대의 값으로 남을 수 있다.

동시에 `daemon.conf` 시작·재로드 경로와 런타임 RPC 경로가 서로 다른 필드 집합과
검증 규칙을 사용했다. 비활성 설정은 시작 시 임계값과 Webhook 값을 읽지 않았고,
런타임 활성/비활성 토글은 평가 스레드 생성 여부와 결합되어 중복 워커 또는 외부
보안·운영 이벤트 유실 위험을 만들었다.

## 결정

### 1. process-local revision과 compare-and-set

- 알림 설정은 프로세스 시작 시 `config_revision=1`로 시작한다.
- `alert.config.set`은 양의 정수 `expected_revision`을 반드시 받는다.
- 엔진은 `state_mu -> config_lock` 순서로 두 lock을 잡고 현재 revision 비교, 기존 설정
  snapshot, 부분 patch overlay, 전체 유효성 검사, commit, revision 증가, episode
  reset을 순서대로 수행한다.
- 성공한 commit만 revision을 정확히 1 증가시킨다.
- 잘못된 입력은 JSON-RPC `-32602`, 오래된 revision은 canonical
  `PURE_RPC_ERR_CONFLICT(-32002)`로 반환한다. 두 경우 모두 설정, revision,
  평가 상태를 바꾸지 않는다.
- CLI `alert set`은 먼저 `alert.config.get`으로 revision을 읽고 SET을 한 번만 보낸다.
  GET 실패나 기형 응답에는 SET을 보내지 않으며 conflict를 자동 재시도하지 않는다.

### 2. 전체 유효성 규칙

부분 patch는 기존 설정 사본에 덮어쓴 뒤 최종 후보 전체를 검증한다.

- CPU, Memory, Disk, DataPool 임계값은 각각 `0..100`이며
  `Warning < Critical`이어야 한다.
- `eval_period`는 `5..600`초, `dedup_window`는 0 이상이다.
- Webhook 형식은 `slack`, `telegram`, `generic`만 허용한다.
- Webhook URL은 비어 있거나 host를 가진 절대 `http`/`https` URL이어야 한다.
  설정 commit 경로에서는 DNS 조회를 하지 않는다.
- 고정 버퍼에 NUL까지 들어가지 않는 긴 문자열, 잘못된 JSON 타입, 알 수 없는 키,
  조회 전용 키, 빈 patch는 요청 전체를 거절한다.
- 복합 규칙은 최대 개수, 허용 키, 타입, 메트릭, 연산자, 심각도, 임계값을 모두 검사한다.

### 3. daemon source와 런타임 상태

- 시작과 `alert.config.reload`는 비활성 상태에서도 모든 알림 필드를 읽어 같은 validator에
  전달한다.
- daemon source 숫자는 원문을 strict하게 파싱한다. 키가 없을 때만 기본값을 사용하며,
  `cpu_warn=abc`처럼 존재하지만 잘못된 값은 기본값으로 숨기지 않고 source 전체를 거절한다.
- 시작 source가 잘못되면 안전 기본값(`enabled=false`)을 revision 1에 적용하고
  `daemon_config_valid=false`, `daemon_config_error=invalid_alert_config`를 노출한다.
- reload source가 잘못되면 현재 런타임 설정과 revision을 보존하고 source 경고만 갱신한다.
- 유효한 reload만 전체 설정을 commit하고 revision을 1 증가시키며 source 경고를 해제한다.
- `webhook_secret`은 비밀 설정 경로로 읽고, 로그와 오류 문자열에 값 또는 원문 입력을
  포함하지 않는다.
- GET과 SET 성공 응답도 비밀키 원문을 반환하지 않는다.
  조회 전용 `webhook_secret_configured` boolean만 노출하며 이를 SET patch에
  되돌려 보내면 요청 전체를 거절한다.

### 4. 동시성 및 lifecycle

- config reader는 짧은 reader lock 안에서 문자열과 복합 규칙까지 포함한 소유 snapshot을
  만든 뒤 JSON 생성과 네트워크 작업을 lock 밖에서 수행한다.
- 설정 epoch의 lock 순서는 `state_mu -> config_lock`으로 고정한다. 설정 commit,
  revision 증가, metric/composite episode reset은 두 lock을 함께 보유한 원자 구간에서
  끝내며 evaluator도 같은 순서로 fresh config snapshot을 취한다.
- 히스토리는 mutex 안에서 연속 snapshot으로 복사하고 JSON 직렬화는 lock 밖에서 수행한다.
- per-VM Webhook lookup은 hash 내부 포인터가 아니라 lock 안에서 복제한 문자열을 반환한다.
- 비동기 Webhook context는 이벤트 발생 시 선택한 URL, 형식, chat ID, secret을 소유한다.
- 평가 워커는 init당 정확히 하나만 유지한다. `enabled=false`는 자동 metric 평가만 멈추며,
  보안 이벤트, 직접 운영 이벤트, ACK, 에스컬레이션 기록 경로는 계속 동작한다.
- 설정 변경과 종료는 condition variable로 워커를 즉시 깨운다. 유효한 commit만 평가
  episode 상태를 초기화한다.

## 결과

- 동시 관리자의 lost update가 명시적 conflict로 바뀌고, 실패 요청은 부분 변경을 남기지 않는다.
- 시작, reload, 런타임 SET이 동일한 필드·검증 계약을 사용한다.
- reader는 한 세대의 설정만 관찰하며 느린 JSON/Webhook 작업이 writer lock을 오래 점유하지 않는다.
- revision은 프로세스 재시작 뒤 1로 돌아간다. 영속 세대 번호나 자동 merge는 이 결정의 범위가 아니다.
- CLI는 conflict를 사용자에게 보여 주고 재실행을 요구한다. 암묵적 재시도와 last-write-wins는
  의도적으로 채택하지 않는다.

## 검증

- 실제 `alert_engine.c`를 링크한 GLib 테스트가 유효/무효 경계, 원자적 거절, stale revision,
  daemon source fallback, 동시 config reader/writer, 히스토리 writer/reader/ACK,
  per-VM Webhook lookup, 비활성 평가 중단, 외부 이벤트 보존, 단일 워커를 검증한다.
- 가짜 UDS 통합 테스트가 CLI의 정확한 GET→SET 요청 trace와 no-SET/no-retry 실패 경로를 검증한다.
- 라이브 daemon 통합 시나리오는 Client A 성공, Client B stale conflict, 잘못된 임계값 거절,
  실패 뒤 상태 불변, CLI 성공 시 revision 1회 증가를 검증한다.
- live daemon 전용 gate는 다음 명령으로 실행한다. root와 명시 승인 없이는 거절하고,
  제외 노드 `192.0.2.100`에서도 거절한다. 실행 전 `daemon.conf`를 root 전용 백업으로
  보존하며 실패·시그널을 포함한 모든 종료 경로에서 원본 파일과 서비스를 복구한다.
  VM·스토리지·호스트 네트워크는 변경하지 않는다.

  ```bash
  sudo env PCV_R7_ALERT_LIVE=1 \
    bash tests/integration/test_alert_config_live.sh
  ```

- 전용 gate에서 Client A 성공과 Client B stale conflict, 무효 patch 원자 거절,
  설치 CLI GET→SET 1회 증가, 유효/무효 reload의
  commit·보존, 무효 startup 안전 기본값, 재시작 revision=1, 원본 파일 SHA-256 일치와
  서비스 active를 확인했다. 따라서 lifecycle을 `Verified`로 승격한다.

## 관련

- ADR-0001: 단일 프로세스 + GMainLoop
- ADR-0025: 검증은 반사실을 동반한다
- `src/modules/daemons/alert_engine.c`
- `tests/test_alert_basic.c`
- `tests/integration/test_alert_config_cli_revision.sh`
