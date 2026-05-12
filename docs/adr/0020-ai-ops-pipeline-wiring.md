# ADR-0020 — AI Ops 파이프라인 호출 체인 연결 규칙

- **상태**: Accepted (2026-04-14)
- **일자**: 2026-04-14
- **관련**: ADR-0018 (fire-and-forget audit 정책)
- **트리거**: BUG-20 — anomaly_detector/virt_events/workload_predict 3곳에서 `pcv_healing_on_*` 호출이 없어 AI Ops 파이프라인 전체가 dead code였음.

## 컨텍스트

AI Ops 아키텍처는 4단계 계층으로 설계되어 있다:

```
┌─────────────────┐    ┌─────────────────┐    ┌───────────────┐    ┌───────────┐
│ 이벤트 공급자    │    │ Self-Healing    │    │ AI Agent      │    │ 액션 실행  │
│ (Producer)      │───▶│ 정책 엔진       │───▶│ (합의)        │───▶│ migrate/  │
│                 │    │                 │    │               │    │ restart   │
│ - anomaly_det.  │    │ - 8개 정책      │    │ - 4 provider  │    │           │
│ - virt_events   │    │ - 5중 안전장치  │    │ - 가중 쿼럼   │    │           │
│ - workload_pre. │    │ - 승인 대기     │    │ - 60% quorum  │    │           │
└─────────────────┘    └─────────────────┘    └───────────────┘    └───────────┘
```

2026-04-14 BUG-20 조사에서 **Producer → 정책 엔진 호출이 0건**임을 확인:

```bash
$ grep -rn 'pcv_healing_on_anomaly\(' --include='*.c'
src/modules/ai/self_healing.c:610: pcv_healing_on_anomaly(...)  ← 정의만
# 호출 지점 0건
```

anomaly_detector는 Z-Score를 계산해 `purecvisor_anomaly_alerts_total` gauge를
증가시켰지만 정책 엔진에 이벤트를 전파하지 않았다. virt_events의 vm-stopped 이벤트
도 로그까지만 찍고 CPU allocator 해제 후 종료. workload_predict도 마찬가지.

결과: `healing.history` / `agent.history`가 영원히 비어있는 상태로 수개월 방치.

## 결정: Producer는 반드시 정책 엔진으로 이벤트를 전파한다

3곳의 이벤트 공급자는 자신의 감지/예측 결과를 **예외 없이** `self_healing`으로
전파한다. 호출 시점과 시그니처는 아래 규칙을 따른다:

### 규칙 1 — Anomaly 이벤트 (Z-Score 기반)

**위치**: `src/modules/ai/anomaly_detector.c::_emit_alert()`

`_emit_alert()`은 Z-Score 임계값을 초과한 메트릭에 대해 이미 Prometheus gauge
+ WebSocket broadcast + 감사 로그 3가지 경로로 알림을 출력한다. 여기에 정책 엔진
호출을 **4번째 경로**로 추가:

```c
// _emit_alert() 마지막 줄
pcv_healing_on_anomaly(m->name, value, z, m->threshold);
```

**전파 파라미터**: Prometheus 메트릭 이름 그대로 전달 (예: `node_hwmon_temp_celsius`).
정책은 `trigger_metric`에 메트릭 이름 prefix를 등록하고 `strstr` 매칭한다.

### 규칙 2 — VM Lifecycle 이벤트 (crash/stop)

**위치**: `src/modules/daemons/virt_events.c::handle_vm_death_in_main_thread()`

libvirt `VIR_DOMAIN_EVENT_STOPPED` / `CRASHED` 수신 시 CPU allocator 해제 직후
정책 엔진으로 synthetic anomaly 전파:

```c
pcv_healing_on_anomaly("vm-unresponsive", 1.0, 99.0, 0.0);
```

**규약**:
- `metric="vm-unresponsive"` — VM 라이프사이클 이벤트용 synthetic 식별자
- `value=1.0` — 고정값 (VM이 실제 정지되었다는 부울 의미)
- `zscore=99.0` — sentinel (정책 `trigger_zscore=0`을 항상 초과)
- `threshold=0.0` — 문서상의 임계값 (의미 없음)

대응 정책은 `trigger_metric="vm-unresponsive"` + `trigger_zscore=0`으로 등록되어야
한다. 이 규약이 없으면 vm-crash 이벤트가 어떤 정책과도 매칭되지 않는다.

### 규칙 3 — Workload 예측 이벤트

**위치**: `src/modules/ai/workload_predict.c::pcv_predict_evaluate()`

EMA + OLS로 5분 후 CPU/MEM 예측값 갱신 직후 정책 엔진 호출:

```c
// G.mu 해제 후에 호출 (데드락 방지)
gdouble cp = n->cpu_predicted_5m;
gdouble mp = n->mem_predicted_5m;
gdouble ct = n->cpu_trend;
gdouble mt = n->mem_trend;
g_mutex_unlock(&G.mu);
pcv_healing_on_prediction(cp, mp, ct, mt);
return;  // 이미 unlock됨
```

**락 순서 규칙**: `pcv_healing_on_prediction()` 내부에서 `G.mu` (healing 뮤텍스)
을 잡기 때문에, workload_predict의 뮤텍스(`G.mu`)를 먼저 해제한 뒤 호출해야 한다.
**두 모듈의 뮤텍스가 같은 이름이므로 혼동 주의** — 중첩 획득 금지.

### 규칙 4 — Sentinel 정책 설계

`vm-unresponsive` 같은 synthetic metric을 받는 정책은 다음 필드 규약을 지킨다:

- `trigger_metric="<metric-name>"` — **절대 빈 문자열 금지**
  (빈 문자열은 `pcv_healing_on_anomaly`에서 `continue`로 즉시 스킵)
- `trigger_zscore=0` — synthetic 값이 항상 매칭되도록
- `require_approval=FALSE` — automatic recovery (VM 재시작은 안전한 액션)
- `cooldown_sec>=300` — 재시작 루프 방지

## 근거

- **계층 분리 원칙**: Producer(감지)와 Policy Engine(판정)이 직접 링크되면 정책
  변경이 Producer에 영향. `pcv_healing_on_anomaly()` 공개 API를 경유하여 느슨한
  결합 유지.
- **테스트 가능성**: Producer를 unit test에서 독립 테스트 가능. Policy Engine도
  synthetic 이벤트로 독립 테스트 가능.
- **감사 추적**: 모든 이벤트가 `pcv_audit` → `healing.history`에 기록되어
  사후 분석 가능.

## 대안 검토

### 대안 A — Observer 패턴 (GSignal)

GLib GObject의 signal 메커니즘으로 Producer → Consumer 느슨 결합.

**기각 사유**:
- GSignal은 GObject 기반이나 PureCVisor AI 모듈은 C 구조체 기반
- 타입 등록 오버헤드 (g_signal_new + marshaller) 대비 이득 미미
- 1:N 브로드캐스트 불필요 (각 이벤트는 정확히 1개 consumer만 필요)

### 대안 B — Event bus (GAsyncQueue)

독립 GThread가 queue를 소비하며 정책 엔진 호출.

**기각 사유**:
- 현재 공급자 3곳 모두 자신의 스레드 또는 메인 루프에서 동작 중
- queue 도입은 레이턴시 증가 + 순서 보장 복잡성만 추가
- 동기 직접 호출로 충분 (Z-Score 계산 자체가 5초 주기라 부하 없음)

### 대안 C — 매 Producer 사이클에서 polling

정책 엔진이 주기적으로 공급자 상태를 읽음.

**기각 사유**:
- 이벤트 누락 가능 (polling 간격 사이 스파이크 miss)
- 공급자별 상태 API 추가 구현 비용
- push 모델이 pull 모델보다 반응성 우수

## 영향

### 긍정적
- AI Ops 전체 파이프라인이 최초로 end-to-end 동작 (BUG-20 해결)
- `healing.history`, `agent.history`, `healing.pending` 모두 live 데이터 기록
- 카오스 테스트에서 자가치유 경로 실측 검증 가능

### 부정적
- **synthetic sentinel 규약 추가 학습 부담** — `vm-unresponsive`처럼 Z-Score
  의미가 없는 이벤트도 `pcv_healing_on_anomaly(metric, val, z, threshold)`
  시그니처에 끼워 넣어야 함. 대안으로 `pcv_healing_on_vm_event()` 같은 전용
  API를 추가할 수 있으나 시그니처 파편화 부담이 더 크다고 판단.
- **락 순서 규칙** — workload_predict 뮤텍스를 해제한 뒤 healing 호출해야 하는
  제약. 코드 리뷰에서 실수하기 쉬움. 코멘트로 명시.

## 적용 이력

| 날짜 | 액션 | 관련 버그 |
|------|------|----------|
| 2026-04-14 | 규칙 1~3 구현 + `vm-unresponsive` 정책 `trigger_metric` 수정 | BUG-20 |
| 2026-04-14 | live 3노드에서 anomaly/predict/vm-crash 3경로 모두 `healing.history` 기록 확인 | - |

## 향후 작업

- [ ] Producer 추가 시 이 ADR을 README처럼 참조하게 하는 공개 문서 링크
- [ ] 복합 조건(2개+ 정책 동시 트리거) → AI Agent 합의 호출의 live 검증 환경
      (현재 stress-ng로 disk_io/network Z-Score 유도가 어려움 — 전용 fault
      injection 도구 필요)
- [ ] `vm-unresponsive` 외 VM lifecycle 이벤트 확장 (ex: `vm-reboot-loop`,
      `vm-migration-failed` 등)
