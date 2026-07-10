#ifndef PURECVISOR_PROMETHEUS_EXPORTER_H
#define PURECVISOR_PROMETHEUS_EXPORTER_H

/**
 * @file prometheus_exporter.h
 * @brief Prometheus 메트릭 레지스트리 — Counter/Gauge 관리 + text format 렌더링
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  파일 역할
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   현재 에디션 데몬 내부의 모든 메트릭을 Prometheus text exposition format으로
 *   관리하고 렌더링하는 경량 레지스트리.
 *
 *   두 종류의 메트릭을 통합 관리한다:
 *     1. purecvisor_* (자체 메트릭 ~26개): RPC 호출 횟수, 응답 시간, 커넥션 풀 등
 *     2. node_* (호스트 메트릭 ~65개): ebpf_telemetry.c에서 push
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  Prometheus text exposition format 규격
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   Prometheus 서버가 HTTP GET /metrics를 통해 scrape하는 표준 텍스트 포맷:
 *
 *     metric_name{label="value"} 123.456
 *     metric_name 789.0
 *
 *   이 모듈은 # HELP / # TYPE 줄을 생략한다.
 *   (Prometheus가 이를 자동 유추할 수 있으며, 일부 기능만 제한됨)
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  메트릭 타입 설명
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   Counter: 단조 증가만 가능. 프로세스 재시작 시에만 리셋됨.
 *            예: purecvisor_rpc_requests_total{method="vm.list",status="ok"} 42
 *            pcv_prom_inc()로 1씩 증가.
 *
 *   Gauge:   임의의 값으로 설정 가능 (증가, 감소, 절대값).
 *            예: node_cpu_seconds_total{cpu="0",mode="idle"} 12345.6
 *            pcv_prom_gauge_set() 또는 pcv_prom_gauge_set_labels()로 설정.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  호출 흐름
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   dispatcher.c (RPC 처리 시작/종료)
 *     ├─ pcv_prom_rpc_start(method)       → (현재 no-op, 향후 inflight 카운터용)
 *     └─ pcv_prom_rpc_end(method, ok, ms) → rpc_requests_total++ & duration_ms 갱신
 *
 *   ebpf_telemetry.c (5초 수집 루프)
 *     └─ pcv_prom_gauge_set_labels(...)   → node_* 메트릭 ~65개 push
 *
 *   rest_server.c (GET /api/v1/metrics)
 *     └─ pcv_prom_render()               → 전체 메트릭 text format 출력
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  스레드 안전성
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   모든 공개 함수는 내부 GMutex로 보호됨. 임의 스레드에서 호출 가능.
 *   단, pcv_prom_init()은 메인 스레드에서 1회만, pcv_prom_shutdown()은 종료 시 1회만.
 */

#include <glib.h>

G_BEGIN_DECLS

/*
 * 256 CPU급 호스트에서는 node_cpu_seconds_total만 2048개(256 CPU x 8 mode)를
 * 사용한다. 기본 node/purecvisor 계열과 여유 슬롯을 포함해 4096을 기준으로 둔다.
 */
#define PCV_PROM_MAX_METRICS 4096

/* ── 생명주기 ── */

/** @brief 레지스트리 초기화. GMutex + 정적 메트릭(purecvisor_info) 등록. main.c에서 1회 호출. */
void pcv_prom_init(void);

/** @brief 레지스트리 종료. initialized=FALSE 설정 후 GMutex 해제. 데몬 종료 시 호출. */
void pcv_prom_shutdown(void);

/* ── Counter 증가 ── */

/**
 * @brief Counter 메트릭을 1 증가.
 * @param name       메트릭 이름 (예: "purecvisor_rpc_requests_total")
 * @param label_key  레이블 키 (NULL 허용)
 * @param label_val  레이블 값 (NULL 허용)
 *
 * 사용 예: pcv_prom_inc("purecvisor_rpc_requests_total", "method", "vm.list");
 */
void pcv_prom_inc(const gchar *name, const gchar *label_key, const gchar *label_val);

/* ── Gauge 설정 ── */

/**
 * @brief Gauge 메트릭에 값 설정 (단일 레이블 키-값).
 * @param name       메트릭 이름
 * @param label_key  레이블 키 (NULL이면 빈 문자열)
 * @param label_val  레이블 값 (NULL이면 빈 문자열)
 * @param value      설정할 값
 */
void pcv_prom_gauge_set(const gchar *name, const gchar *label_key,
                         const gchar *label_val, gdouble value);

/**
 * @brief Gauge 메트릭에 값 설정 (사전 조립된 레이블 문자열).
 *
 * ebpf_telemetry.c의 node_* 콜렉터들이 다중 레이블을 사용할 때 호출.
 * @param name    메트릭 이름
 * @param labels  조립된 레이블 문자열. 포맷: key1="val1",key2="val2" (NULL이면 빈 문자열)
 * @param value   설정할 값
 *
 * 사용 예: pcv_prom_gauge_set_labels("node_cpu_seconds_total", "cpu=\"0\",mode=\"idle\"", 12345.6);
 */
void pcv_prom_gauge_set_labels(const gchar *name, const gchar *labels,
                                gdouble value);

/* ── RPC 편의 래퍼 — dispatcher.c에서 호출 ── */

/** @brief RPC 요청 시작 기록. 현재 no-op. 향후 inflight 카운터용. */
void pcv_prom_rpc_start(const gchar *method);

/**
 * @brief RPC 요청 완료 기록 — rpc_requests_total++ & rpc_duration_ms 갱신.
 * @param method      RPC 메서드명 (예: "vm.list")
 * @param success     TRUE이면 status="ok", FALSE이면 status="error"
 * @param duration_ms 처리 소요 시간 (밀리초)
 */
void pcv_prom_rpc_end(const gchar *method, gboolean success, gdouble duration_ms);

/**
 * @brief ADR-0021 ZFS inflight 분산 락 획득 결과와 대기 시간을 기록한다.
 * @param pool_name 풀 이름. 현재 고카디널리티 방지를 위해 label에는 포함하지 않음.
 * @param op        작업 종류 (예: create, destroy, recv, rollback)
 * @param result    ok, busy, error 중 하나
 * @param wait_ms   락 획득 시도에 소요된 시간 (밀리초)
 */
void pcv_prom_zfs_inflight_lock_observe(const gchar *pool_name,
                                        const gchar *op,
                                        const gchar *result,
                                        gdouble wait_ms);

/* ── Prometheus text format 출력 ── */

/**
 * @brief 전체 메트릭을 Prometheus text exposition format으로 렌더링한다.
 * @return gchar* — 호출자가 소유권을 가짐. 사용 후 g_free() 필요.
 *
 * rest_server.c의 GET /api/v1/metrics 엔드포인트에서 호출.
 * Prometheus 서버가 이 출력을 scrape한다.
 */
gchar *pcv_prom_render(void);

G_END_DECLS

#endif /* PURECVISOR_PROMETHEUS_EXPORTER_H */
