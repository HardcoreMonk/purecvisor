/**
 * @file prometheus_exporter.c
 * @brief Prometheus 메트릭 레지스트리 — Counter/Gauge + text format 렌더링
 *
 * ============================================================================
 *  파일 역할
 * ============================================================================
 *  현재 에디션 데몬 내부의 애플리케이션 레벨 메트릭(RPC 호출 횟수, 응답 시간 등)을
 *  Prometheus text exposition format으로 렌더링하는 경량 레지스트리.
 *
 *  이 모듈은 node_exporter 호환 호스트 메트릭(ebpf_telemetry.c에서 130개 수집)과
 *  별도로, purecvisor 자체의 동작 메트릭(purecvisor_* 27개)을 관리한다.
 *
 * ============================================================================
 *  아키텍처 위치
 * ============================================================================
 *
 *   dispatcher.c (RPC 처리 시작/종료)
 *     ├─ pcv_prom_rpc_start(method)     → inflight 카운터 (예약)
 *     └─ pcv_prom_rpc_end(method, ok, ms) → 요청 카운터 + 응답 시간 기록
 *
 *   ebpf_telemetry.c (5초 수집 루프)
 *     └─ pcv_prom_gauge_set_labels(...)  → node_* 메트릭 130개 push
 *
 *   rest_server.c (GET /api/v1/metrics)
 *     └─ pcv_prom_render()              → 전체 메트릭 text format 출력
 *
 * ============================================================================
 *  메트릭 항목 (purecvisor_* 자체 메트릭)
 * ============================================================================
 *   purecvisor_info{version="1.0"}                Gauge  상수 1 (버전 표시용)
 *   purecvisor_rpc_requests_total{method,status}   Counter  RPC 호출 누적 횟수
 *   purecvisor_rpc_duration_ms{method}              Gauge  최근 RPC 응답 시간 (ms)
 *
 * ============================================================================
 *  스레드 안전성
 * ============================================================================
 *   GMutex(G.mu)로 레지스트리 배열 전체를 보호한다.
 *   - 쓰기(inc/gauge_set): 락 잡고 → 값 갱신 → 해제
 *   - 읽기(render): 락 잡고 → text format 빌드 → 해제
 *   카운터 증가(inc)는 H-OPT-1 GHashTable O(1) 룩업 + 잠금.
 *
 * ============================================================================
 *  내부 자료구조
 * ============================================================================
 *   - PromMetric 구조체 배열 (고정 크기 PCV_PROM_MAX_METRICS)
 *   - name + labels 조합이 유일 키 (예: "purecvisor_rpc_requests_total" + "method=\"vm.list\",status=\"ok\"")
 *   - _find_or_create()로 이미 존재하면 인덱스 반환, 없으면 새 슬롯 할당 (H-OPT-1 O(1))
 *   - GHashTable G.index: "name\x01labels" → 슬롯 인덱스 매핑 (O(1) 룩업)
 *
 * ============================================================================
 *  Prometheus text format 출력 규격
 * ============================================================================
 *   각 메트릭은 한 줄로 출력:
 *     metric_name{label="value"} 123.456
 *   또는 레이블이 없으면:
 *     metric_name 123.456
 *
 *   이 포맷은 Prometheus 서버가 /metrics 엔드포인트를 scrape할 때 파싱하는 표준.
 *   # HELP, # TYPE 줄은 생략 (Prometheus가 없어도 동작하지만, 일부 기능 제한).
 *
 * ============================================================================
 *  의존성
 * ============================================================================
 *   - GLib 2.0 (GMutex, g_snprintf, g_strlcpy, g_string_*)
 *   - utils/pcv_log.h (PCV_LOG_INFO)
 */
#include "prometheus_exporter.h"
#include "purecvisor/version.h"
#include "utils/pcv_log.h"
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <errno.h>  /* B12-M1: g_strerror(errno) in mkdir check */

/*
 * ============================================================================
 *  [주니어 개발자 필독] Prometheus 메트릭 레지스트리 핵심 개념
 * ============================================================================
 *
 *  Counter vs Gauge:
 *    Counter: 단조 증가만 가능 (요청 횟수, 에러 횟수). 리셋은 재시작으로만.
 *    Gauge: 임의 값 설정 가능 (CPU%, 메모리, 현재 연결 수).
 *
 *  text format 출력:
 *    Prometheus 서버가 /metrics를 scrape할 때 사용하는 표준 형식:
 *      metric_name{label="value"} 123.456
 *    이 모듈은 PromMetric 배열에서 이 형식의 문자열을 생성합니다.
 *
 *  H-OPT-1 O(1) 룩업:
 *    name+labels 조합으로 기존 메트릭을 찾을 때 GHashTable(G.index)을 사용합니다.
 *    복합 키 "name\x01labels"를 해시 테이블에서 O(1)으로 조회합니다.
 * ============================================================================
 */

/** 로그 도메인 태그. journalctl에서 "prom_export"로 필터링 가능. */
#define PROM_LOG_DOM "prom_export"

/**
 * @struct PromMetric
 * @brief 단일 메트릭의 이름/레이블/값을 저장하는 구조체
 *
 * name + labels 조합이 유일 키 역할을 한다.
 * 예: name="node_cpu_seconds_total", labels="cpu=\"0\",mode=\"idle\""
 */
typedef struct {
    gchar  name[128];      /**< 메트릭 이름 (예: "purecvisor_rpc_requests_total") */
    gchar  labels[128];    /**< 레이블 문자열 (예: "method=\"vm.list\",status=\"ok\"") */
    /* Prometheus spec uses float64 for all values — gdouble matches this.
     * Precision loss occurs only after 2^53 (~9e15) increments — effectively never. */
    gdouble value;         /**< 현재 값. Counter는 누적, Gauge는 최신 값. */
    gboolean is_counter;   /**< TRUE=Counter(증가만 가능), FALSE=Gauge(임의 설정) */
    gint64  last_update;   /**< B12-C3: 마지막 갱신 monotonic time (us) — stale 감지용 */
} PromMetric;

/**
 * @brief 모듈 전역 상태. 메트릭 레지스트리 + 동기화 뮤텍스.
 *
 * {0}으로 zero-initialize. initialized가 FALSE이면 모든 공개 함수가 조기 반환.
 *
 * H-OPT-1: `index` 필드는 "name\x01labels" 복합 키 → 슬롯 인덱스(GINT_TO_POINTER)
 * 매핑 해시 테이블이다. _find_or_create()의 O(n) 선형 스캔을 O(1)으로 단축한다.
 * G.mu 락 내부에서만 접근하므로 별도 동기화 불필요.
 */
static struct {
    PromMetric  metrics[PCV_PROM_MAX_METRICS];  /**< 고정 크기 메트릭 슬롯 배열 */
    gint        count;                 /**< 현재 사용 중인 슬롯 수 */
    GMutex      mu;                    /**< 레지스트리 동시 접근 보호용 뮤텍스 */
    gboolean    initialized;           /**< init() 호출 여부. 이중 사용 방지 가드. */
    GHashTable *index;                 /**< H-OPT-1: "name\x01labels" → slot idx (O(1) lookup) */
} G = {0};

/**
 * @brief name+labels 조합으로 기존 메트릭을 찾거나, 없으면 새 슬롯을 할당한다.
 *
 * H-OPT-1: GHashTable(G.index)을 이용한 O(1) 복합 키 룩업.
 * 이전 구현은 O(n) 선형 스캔이었으며, 2048슬롯 + 고빈도 eBPF 갱신 시
 * 락 보유 시간이 누적됐다. 복합 키를 "name\x01labels" 형태로 구성한다.
 * \x01(SOH)은 메트릭 이름·레이블 양쪽에서 절대 나타나지 않으므로 안전한 구분자다.
 *
 * 슬롯이 가득 차면 -1을 반환하여 호출자가 값 갱신을 포기하게 한다.
 *
 * @param name        메트릭 이름 (예: "purecvisor_rpc_requests_total")
 * @param labels      레이블 문자열 (예: "method=\"vm.list\"")
 * @param is_counter  TRUE이면 Counter, FALSE이면 Gauge로 타입 설정 (새 슬롯에만 적용)
 * @return gint       찾았거나 새로 할당한 인덱스. 실패 시 -1.
 *
 * 호출 컨텍스트: 항상 G.mu 락 내부에서 호출됨. 직접 락을 잡지 않음.
 */
static gint
_find_or_create(const gchar *name, const gchar *labels, gboolean is_counter)
{
    /* H-OPT-1: O(1) hash lookup — guard for pre-init calls (_counters_load
     * invoked from pcv_prom_init before index is created; fall through to
     * legacy path in that unlikely window). */
    gint64 now = g_get_monotonic_time();
    if (G.index) {
        /* Build composite key: "name\x01labels" */
        gchar *key = g_strdup_printf("%s\x01%s", name, labels ? labels : "");
        gpointer stored;
        gboolean found = g_hash_table_lookup_extended(G.index, key, NULL, &stored);
        if (found) {
            gint idx = GPOINTER_TO_INT(stored);
            G.metrics[idx].last_update = now;  /* B12-C3: touch */
            g_free(key);
            return idx;
        }
        /* Not found — create new slot */
        if (G.count >= PCV_PROM_MAX_METRICS) {
            /* B12-M1: 풀 포화 시 최초 1회만 WARN — 로그 폭주 방지 */
            static gboolean warned = FALSE;
            if (!warned) {
                warned = TRUE;
                PCV_LOG_WARN(PROM_LOG_DOM,
                    "Metric pool saturated (%d) — dropping new labels. "
                    "Consider increasing MAX_METRICS or reducing label cardinality",
                    PCV_PROM_MAX_METRICS);
            }
            g_free(key);
            return -1;
        }
        gint idx = G.count++;
        g_strlcpy(G.metrics[idx].name,   name,             sizeof(G.metrics[idx].name));
        g_strlcpy(G.metrics[idx].labels, labels ? labels : "", sizeof(G.metrics[idx].labels));
        G.metrics[idx].value      = 0;
        G.metrics[idx].is_counter = is_counter;
        G.metrics[idx].last_update = now;
        /* key ownership transferred to G.index (g_free key_destroy) */
        g_hash_table_insert(G.index, key, GINT_TO_POINTER(idx));
        return idx;
    }

    /* H-OPT-1: fallback O(n) path — only reachable during _counters_load()
     * which runs before G.index is created in pcv_prom_init(). */
    for (gint i = 0; i < G.count; i++) {
        if (g_strcmp0(G.metrics[i].name, name) == 0 &&
            g_strcmp0(G.metrics[i].labels, labels) == 0) {
            G.metrics[i].last_update = now;  /* B12-C3: touch */
            return i;
        }
    }
    if (G.count >= PCV_PROM_MAX_METRICS) {
        static gboolean warned = FALSE;
        if (!warned) {
            warned = TRUE;
            PCV_LOG_WARN(PROM_LOG_DOM,
                "Metric pool saturated (%d) — dropping new labels. "
                "Consider increasing MAX_METRICS or reducing label cardinality",
                PCV_PROM_MAX_METRICS);
        }
        return -1;
    }
    gint idx = G.count++;
    g_strlcpy(G.metrics[idx].name,   name,             sizeof(G.metrics[idx].name));
    g_strlcpy(G.metrics[idx].labels, labels ? labels : "", sizeof(G.metrics[idx].labels));
    G.metrics[idx].value      = 0;
    G.metrics[idx].is_counter = is_counter;
    G.metrics[idx].last_update = now;
    return idx;
}

/**
 * B12-C3: VM 삭제/정지 후 stale 메트릭 라벨 자동 제거.
 * render 시점에 TTL 초과(기본 5분) + high-cardinality label을 가진 메트릭 제거.
 * counter는 보존 (누적 값 손실 방지), gauge만 제거 대상.
 */
#define PCV_PROM_STALE_TTL_US (5 * 60 * G_USEC_PER_SEC)  /* 5분 */

/* B12-C2: Counter 체크포인트 파일 경로 + 주기 */
#define PCV_PROM_CHECKPOINT_PATH "/var/lib/purecvisor/prom_counters.json"
#define PCV_PROM_CHECKPOINT_INTERVAL_SEC 60
static guint g_checkpoint_timer = 0;

/**
 * _counters_save_unlocked:
 * 모든 Counter 메트릭을 JSON 파일에 저장합니다.
 * 데몬 재시작 후에도 누적 값이 유지되어 Prometheus rate() 계산이
 * reset으로 오해되지 않도록 합니다.
 * 호출 시점에 G.mu 락이 잡혀 있어야 합니다.
 */
static void
_counters_save_unlocked(void)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "saved_at");
    json_builder_add_int_value(b, (gint64)g_get_real_time() / G_USEC_PER_SEC);
    json_builder_set_member_name(b, "counters");
    json_builder_begin_array(b);
    for (gint i = 0; i < G.count; i++) {
        if (!G.metrics[i].is_counter) continue;
        if (G.metrics[i].value <= 0.0) continue;
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "name");
        json_builder_add_string_value(b, G.metrics[i].name);
        json_builder_set_member_name(b, "labels");
        json_builder_add_string_value(b, G.metrics[i].labels);
        json_builder_set_member_name(b, "value");
        json_builder_add_double_value(b, G.metrics[i].value);
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
    json_builder_end_object(b);

    JsonNode *root = json_builder_get_root(b);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, root);
    gchar *data = json_generator_to_data(gen, NULL);
    json_node_free(root);
    g_object_unref(gen);
    g_object_unref(b);

    /* atomic write: tmp → rename */
    /* B12-M1: check mkdir result; log warning on failure and skip write */
    if (g_mkdir_with_parents("/var/lib/purecvisor", 0755) != 0) {
        PCV_LOG_WARN(PROM_LOG_DOM,
            "Cannot create checkpoint dir /var/lib/purecvisor: %s",
            g_strerror(errno));
        g_free(data);
        return;
    }
    gchar *tmp_path = g_strdup_printf("%s.tmp", PCV_PROM_CHECKPOINT_PATH);
    GError *werr = NULL;
    if (g_file_set_contents(tmp_path, data, -1, &werr)) {
        if (g_rename(tmp_path, PCV_PROM_CHECKPOINT_PATH) != 0) {
            PCV_LOG_WARN(PROM_LOG_DOM, "checkpoint rename failed: %s",
                         g_strerror(errno));
        }
    } else {
        PCV_LOG_WARN(PROM_LOG_DOM, "checkpoint write failed: %s",
                     werr ? werr->message : "unknown");
        g_clear_error(&werr);
    }
    g_free(tmp_path);
    g_free(data);
}

/**
 * _counters_load:
 * 부팅 시 체크포인트 파일을 읽어 Counter 값을 복원합니다.
 */
static void
_counters_load(void)
{
    gchar *content = NULL;
    gsize len = 0;
    if (!g_file_get_contents(PCV_PROM_CHECKPOINT_PATH, &content, &len, NULL)) {
        return;  /* 첫 기동이거나 파일 없음 — 정상 */
    }

    JsonParser *jp = json_parser_new();
    if (!json_parser_load_from_data(jp, content, (gssize)len, NULL)) {
        g_object_unref(jp);
        g_free(content);
        PCV_LOG_WARN(PROM_LOG_DOM, "checkpoint parse failed — discarding");
        return;
    }

    /* B12-M2: validate root node before dereferencing */
    JsonNode *root_n = json_parser_get_root(jp);
    if (!root_n || !JSON_NODE_HOLDS_OBJECT(root_n)) {
        PCV_LOG_WARN(PROM_LOG_DOM,
            "checkpoint root is not an object — discarding");
        g_object_unref(jp);
        g_free(content);
        return;
    }
    JsonObject *root = json_node_get_object(root_n);
    if (!json_object_has_member(root, "counters")) {
        g_object_unref(jp);
        g_free(content);
        return;
    }

    JsonArray *arr = json_object_get_array_member(root, "counters");
    guint n = json_array_get_length(arr);
    gint restored = 0;
    /* G.mu는 호출자(pcv_prom_init)에서 잡고 호출 */
    for (guint i = 0; i < n; i++) {
        JsonObject *o = json_array_get_object_element(arr, i);
        if (!o) continue;
        const gchar *name = json_object_get_string_member(o, "name");
        const gchar *labels = json_object_get_string_member(o, "labels");
        gdouble value = json_object_get_double_member(o, "value");
        if (!name || !labels) continue;
        gint idx = _find_or_create(name, labels, TRUE);
        if (idx >= 0) {
            G.metrics[idx].value = value;
            restored++;
        }
    }

    g_object_unref(jp);
    g_free(content);
    PCV_LOG_INFO(PROM_LOG_DOM, "Counter checkpoint restored (%d entries)",
                 restored);
}

/**
 * _checkpoint_timer_cb:
 * 60초 주기로 Counter 값을 디스크에 저장합니다.
 */
static gboolean
_checkpoint_timer_cb(gpointer data __attribute__((unused)))
{
    /* B12-M4: use g_atomic_int_get to read G.initialized without a data race */
    if (!g_atomic_int_get(&G.initialized)) return G_SOURCE_REMOVE;
    g_mutex_lock(&G.mu);
    _counters_save_unlocked();
    g_mutex_unlock(&G.mu);
    return G_SOURCE_CONTINUE;
}

static gint
_sweep_stale_gauges(void)
{
    gint64 now = g_get_monotonic_time();
    gint removed = 0;
    /* 뒤에서부터 역순 swap-remove */
    for (gint i = G.count - 1; i >= 0; i--) {
        if (G.metrics[i].is_counter) continue;  /* counter 보존 */
        if (now - G.metrics[i].last_update < PCV_PROM_STALE_TTL_US) continue;
        /* high-cardinality 의심: vm="..." 또는 vm_id="..." 라벨 포함 */
        if (!strstr(G.metrics[i].labels, "vm=") &&
            !strstr(G.metrics[i].labels, "vm_id=") &&
            !strstr(G.metrics[i].labels, "vm_name=")) continue;

        /* H-OPT-1: remove deleted slot from index */
        if (G.index) {
            gchar *del_key = g_strdup_printf("%s\x01%s",
                                             G.metrics[i].name,
                                             G.metrics[i].labels);
            g_hash_table_remove(G.index, del_key);
            g_free(del_key);
        }

        if (i < G.count - 1) {
            /* H-OPT-1: the last slot moves to position i — update its index entry */
            if (G.index) {
                gchar *moved_key = g_strdup_printf("%s\x01%s",
                                                   G.metrics[G.count - 1].name,
                                                   G.metrics[G.count - 1].labels);
                g_hash_table_insert(G.index, moved_key, GINT_TO_POINTER(i));
                /* moved_key ownership transferred; old key already removed above */
            }
            G.metrics[i] = G.metrics[G.count - 1];
        }
        G.count--;
        removed++;
    }
    return removed;
}

/**
 * @brief Prometheus 레지스트리를 초기화한다.
 *
 * GMutex를 초기화하고, 정적 메트릭(purecvisor_info{version})을 등록한다.
 * main.c에서 데몬 시작 시 1회 호출.
 *
 * 이 함수 호출 전까지 inc/gauge_set/render는 모두 no-op이다.
 */
void pcv_prom_init(void)
{
    g_mutex_init(&G.mu);
    G.count = 0;
    /* B12-M4: use g_atomic_int_set so _checkpoint_timer_cb sees the write */
    g_atomic_int_set(&G.initialized, TRUE);

    /* H-OPT-1: create the name\x01labels → slot-index hash table before
     * acquiring the lock and calling _find_or_create().  Key strings are
     * heap-allocated composites owned by the table (g_free destructor).
     * Values are slot indices as GINT_TO_POINTER — no value destructor needed.
     * G.index is live from this point, so all subsequent _find_or_create()
     * calls (including those from _counters_load()) use the O(1) path. */
    G.index = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    /* 정적 메트릭: 버전 정보 */
    g_mutex_lock(&G.mu);
    gchar *info_labels = g_strdup_printf("version=\"%s\"", PCV_PRODUCT_VERSION);
    gint idx = _find_or_create("purecvisor_info", info_labels, FALSE);
    g_free(info_labels);
    if (idx >= 0) G.metrics[idx].value = 1;

    /* B12-C2: Counter 체크포인트 복원 — 데몬 재시작 시 Prometheus rate()
     * 계산이 reset으로 오해되지 않도록 이전 누적 값을 복원합니다. */
    _counters_load();
    g_mutex_unlock(&G.mu);

    /* 60초 주기 체크포인트 저장 */
    g_checkpoint_timer = g_timeout_add_seconds(
        PCV_PROM_CHECKPOINT_INTERVAL_SEC, _checkpoint_timer_cb, NULL);

    PCV_LOG_INFO(PROM_LOG_DOM,
                 "Prometheus exporter initialized (counter checkpoint=%ds)",
                 PCV_PROM_CHECKPOINT_INTERVAL_SEC);
}

/**
 * @brief Prometheus 레지스트리를 종료한다.
 *
 * initialized를 FALSE로 설정하여 이후 모든 공개 함수를 no-op으로 만든 뒤,
 * GMutex를 해제한다. 데몬 종료(drain) 시 호출.
 */
void pcv_prom_shutdown(void)
{
    /* B12-M4: remove timer source BEFORE clearing G.initialized so that a
     * concurrent timer fire sees initialized=TRUE and completes cleanly,
     * then the atomic set below prevents any subsequent spurious fires. */
    if (g_checkpoint_timer > 0) {
        g_source_remove(g_checkpoint_timer);
        g_checkpoint_timer = 0;
    }
    if (g_atomic_int_get(&G.initialized)) {
        g_mutex_lock(&G.mu);
        _counters_save_unlocked();
        g_mutex_unlock(&G.mu);
    }
    /* B12-M4: atomic clear so _checkpoint_timer_cb cannot race on the flag */
    g_atomic_int_set(&G.initialized, FALSE);
    /* H-OPT-1: destroy the composite-key index; g_free key_destroy handles
     * all heap-allocated key strings inserted by _find_or_create(). */
    if (G.index) {
        g_hash_table_destroy(G.index);
        G.index = NULL;
    }
    g_mutex_clear(&G.mu);
}

/**
 * @brief Counter 메트릭을 1 증가시킨다.
 *
 * name + label_key="label_val" 조합의 Counter를 찾아(없으면 생성) 값을 1.0 더한다.
 * Counter는 단조 증가만 가능하며, 리셋은 프로세스 재시작으로만 발생한다.
 *
 * 사용 예:
 *   pcv_prom_inc("purecvisor_rpc_requests_total", "method", "vm.list");
 *   → purecvisor_rpc_requests_total{method="vm.list"} += 1
 *
 * @param name       메트릭 이름
 * @param label_key  레이블 키 (NULL이면 빈 문자열)
 * @param label_val  레이블 값 (NULL이면 빈 문자열)
 *
 * 스레드 안전: 내부 G.mu 락으로 보호됨.
 */
void pcv_prom_inc(const gchar *name, const gchar *label_key, const gchar *label_val)
{
    if (!G.initialized) return;
    gchar labels[128];
    g_snprintf(labels, sizeof(labels), "%s=\"%s\"",
               label_key ? label_key : "", label_val ? label_val : "");

    g_mutex_lock(&G.mu);
    gint idx = _find_or_create(name, labels, TRUE);
    if (idx >= 0) G.metrics[idx].value += 1.0;
    g_mutex_unlock(&G.mu);
}

/**
 * @brief Gauge 메트릭에 값을 설정한다 (단일 레이블 키-값 쌍).
 *
 * Gauge는 Counter와 달리 임의의 값으로 설정할 수 있다 (증가/감소/절대값).
 *
 * @param name       메트릭 이름
 * @param label_key  레이블 키 (NULL이면 빈 문자열)
 * @param label_val  레이블 값 (NULL이면 빈 문자열)
 * @param value      설정할 값
 *
 * 스레드 안전: 내부 G.mu 락으로 보호됨.
 */
void pcv_prom_gauge_set(const gchar *name, const gchar *label_key,
                         const gchar *label_val, gdouble value)
{
    if (!G.initialized) return;
    gchar labels[128];
    g_snprintf(labels, sizeof(labels), "%s=\"%s\"",
               label_key ? label_key : "", label_val ? label_val : "");

    g_mutex_lock(&G.mu);
    gint idx = _find_or_create(name, labels, FALSE);
    if (idx >= 0) G.metrics[idx].value = value;
    g_mutex_unlock(&G.mu);
}

/**
 * @brief Gauge 메트릭에 값을 설정한다 (레이블 문자열 직접 전달).
 *
 * pcv_prom_gauge_set()와 유사하지만, 이미 조립된 레이블 문자열을 직접 받는다.
 * ebpf_telemetry.c의 node_* 콜렉터들이 다중 레이블을 사용할 때 호출한다.
 *
 * 사용 예:
 *   pcv_prom_gauge_set_labels("node_cpu_seconds_total", "cpu=\"0\",mode=\"idle\"", 12345.6);
 *
 * @param name    메트릭 이름
 * @param labels  조립된 레이블 문자열 (NULL이면 빈 문자열). 포맷: key1="val1",key2="val2"
 * @param value   설정할 값
 *
 * 스레드 안전: 내부 G.mu 락으로 보호됨.
 */
void pcv_prom_gauge_set_labels(const gchar *name, const gchar *labels,
                                gdouble value)
{
    if (!G.initialized) return;
    g_mutex_lock(&G.mu);
    gint idx = _find_or_create(name, labels ? labels : "", FALSE);
    if (idx >= 0) G.metrics[idx].value = value;
    g_mutex_unlock(&G.mu);
}

/**
 * @brief RPC 요청 시작을 기록한다 (현재 미구현, 향후 inflight 카운터용).
 *
 * dispatcher.c에서 RPC 처리 시작 시점에 호출된다.
 * 향후 purecvisor_rpc_inflight{method} Gauge를 추가하여
 * 현재 처리 중인 요청 수를 추적할 수 있다.
 *
 * @param method  RPC 메서드명 (예: "vm.list")
 */
void pcv_prom_rpc_start(const gchar *method)
{
    if (!G.initialized || !method) return;
    /* inflight 카운터 (선택적) — 향후 구현 예정 */
}

/**
 * @brief RPC 요청 완료를 기록한다 — 요청 카운터 증가 + 응답 시간 갱신.
 *
 * dispatcher.c에서 RPC 처리 완료 시점에 호출된다.
 * 두 가지 메트릭을 동시에 갱신한다:
 *   1. purecvisor_rpc_requests_total{method,status} — Counter, +1
 *   2. purecvisor_rpc_duration_ms{method} — Gauge, 최신 값으로 덮어씀
 *
 * @param method      RPC 메서드명 (예: "vm.list")
 * @param success     TRUE이면 status="ok", FALSE이면 status="error"
 * @param duration_ms 처리 소요 시간 (밀리초)
 *
 * 스레드 안전: 내부 G.mu 락으로 보호됨.
 */
void pcv_prom_rpc_end(const gchar *method, gboolean success, gdouble duration_ms)
{
    if (!G.initialized || !method) return;

    /* rpc_requests_total{method,status} */
    gchar labels[128];
    g_snprintf(labels, sizeof(labels), "method=\"%s\",status=\"%s\"",
               method, success ? "ok" : "error");

    g_mutex_lock(&G.mu);
    gint idx = _find_or_create("purecvisor_rpc_requests_total", labels, TRUE);
    if (idx >= 0) G.metrics[idx].value += 1.0;

    /* rpc_duration_ms{method} — 최근 값 */
    gchar dur_labels[128];
    g_snprintf(dur_labels, sizeof(dur_labels), "method=\"%s\"", method);
    gint didx = _find_or_create("purecvisor_rpc_duration_ms", dur_labels, FALSE);
    if (didx >= 0) G.metrics[didx].value = duration_ms;
    g_mutex_unlock(&G.mu);
}

/**
 * @brief ADR-0021 ZFS inflight lock 결과와 대기 시간 히스토그램 계열을 기록한다.
 *
 * 기존 exporter는 Counter/Gauge만 저장하므로 Prometheus histogram-compatible
 * family(_bucket/_sum/_count)를 Counter 슬롯으로 직접 구성한다.
 */
void pcv_prom_zfs_inflight_lock_observe(const gchar *pool_name,
                                        const gchar *op,
                                        const gchar *result,
                                        gdouble wait_ms)
{
    (void)pool_name;  /* pool label은 고카디널리티 방지를 위해 보류 */
    if (!G.initialized || !result) return;
    const gchar *safe_op = (op && *op) ? op : "unknown";
    const gchar *safe_result = *result ? result : "unknown";
    if (wait_ms < 0.0) wait_ms = 0.0;

    gchar labels[128];
    g_snprintf(labels, sizeof(labels), "op=\"%s\",result=\"%s\"",
               safe_op, safe_result);

    g_mutex_lock(&G.mu);
    gint idx = _find_or_create(
        "purecvisor_zfs_inflight_lock_acquired_total", labels, TRUE);
    if (idx >= 0) G.metrics[idx].value += 1.0;

    static const gdouble buckets[] = {10.0, 50.0, 100.0, 500.0, 1000.0, 5000.0};
    for (guint i = 0; i < G_N_ELEMENTS(buckets); i++) {
        gchar bucket_labels[160];
        g_snprintf(bucket_labels, sizeof(bucket_labels),
                   "op=\"%s\",result=\"%s\",le=\"%.0f\"",
                   safe_op, safe_result, buckets[i]);
        gint bidx = _find_or_create(
            "purecvisor_zfs_inflight_lock_wait_ms_bucket", bucket_labels, TRUE);
        if (bidx >= 0 && wait_ms <= buckets[i]) G.metrics[bidx].value += 1.0;
    }

    gchar inf_labels[160];
    g_snprintf(inf_labels, sizeof(inf_labels),
               "op=\"%s\",result=\"%s\",le=\"+Inf\"",
               safe_op, safe_result);
    gint inf_idx = _find_or_create(
        "purecvisor_zfs_inflight_lock_wait_ms_bucket", inf_labels, TRUE);
    if (inf_idx >= 0) G.metrics[inf_idx].value += 1.0;

    gint sum_idx = _find_or_create(
        "purecvisor_zfs_inflight_lock_wait_ms_sum", labels, TRUE);
    if (sum_idx >= 0) G.metrics[sum_idx].value += wait_ms;
    gint count_idx = _find_or_create(
        "purecvisor_zfs_inflight_lock_wait_ms_count", labels, TRUE);
    if (count_idx >= 0) G.metrics[count_idx].value += 1.0;
    g_mutex_unlock(&G.mu);
}

/**
 * @brief 전체 메트릭을 Prometheus text exposition format으로 렌더링한다.
 *
 * REST API의 GET /api/v1/metrics 엔드포인트에서 호출되며,
 * Prometheus 서버가 이 출력을 scrape한다.
 *
 * 출력 형식 (각 메트릭 1줄):
 *   레이블 있음: metric_name{key="val"} 123.456
 *   레이블 없음: metric_name 123.456
 *
 * 레이블이 빈 문자열이거나 '='로 시작하면 레이블 없는 형식으로 출력한다.
 * (빈 label_key + 빈 label_val → labels="=\"\"" → '='로 시작 → 레이블 생략)
 *
 * @return gchar* — 호출자가 소유권을 가짐. 사용 후 g_free() 필요.
 *                  initialized=FALSE이면 설명 주석 한 줄만 반환.
 *
 * 스레드 안전: 내부 G.mu 락으로 보호됨.
 */
gchar *pcv_prom_render(void)
{
    if (!G.initialized)
        return g_strdup("# purecvisor prometheus exporter not initialized\n");

    GString *buf = g_string_new("");

    g_mutex_lock(&G.mu);
    /* B12-C3: stale VM 라벨 제거 (5분 이상 갱신 없는 gauge) */
    gint sweeped = _sweep_stale_gauges();
    if (sweeped > 0) {
        PCV_LOG_INFO("prom_exporter",
            "Swept %d stale gauge metrics (VM labels, TTL=5min)", sweeped);
    }

    /* B12-W6 (1.0): 동일 metric name이 서로 다른 is_counter 값으로 등록되면
     * Prometheus가 type mismatch로 스크레이프를 거부한다. 첫 등장 슬롯의 타입을
     * authoritative로 간주하고, 나머지 슬롯이 mismatch면 WARN 로그로 알린다.
     * 또한 `# TYPE` 한 줄을 metric name별 1회씩 방출하여 스크레이퍼가 올바르게
     * 분류하도록 한다.
     *
     * 1.0 최적화: 이전 구현은 각 슬롯마다 앞쪽 슬롯을 다시 선형 스캔하는
     * O(n²) 패턴이었음 (n=2048 worst-case ≈ 4M ops, 15s 주기 scrape에서 누적
     * 부담). 이 패스는 GHashTable에 first-seen idx를 한 번만 기록해 O(n)으로
     * 단축한다. */
    /* B12-M3: use g_hash_table_new_full with g_free so that each inserted key
     * (g_strdup of m->name) is owned by the table and freed on destroy,
     * rather than storing a raw pointer into the metrics array that could
     * be invalidated by a swap-remove in _sweep_stale_gauges. */
    GHashTable *first_seen = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, NULL);
    for (gint i = 0; i < G.count; i++) {
        PromMetric *m = &G.metrics[i];
        gpointer val;
        gboolean exists = g_hash_table_lookup_extended(
            first_seen, m->name, NULL, &val);
        if (!exists) {
            g_hash_table_insert(first_seen, g_strdup(m->name), GINT_TO_POINTER(i));
            g_string_append_printf(buf, "# TYPE %s %s\n", m->name,
                m->is_counter ? "counter" : "gauge");
        } else {
            gint j = GPOINTER_TO_INT(val);
            if (G.metrics[j].is_counter != m->is_counter) {
                PCV_LOG_WARN(PROM_LOG_DOM,
                    "Metric type mismatch: %s (slot %d=%s, slot %d=%s) — "
                    "using first",
                    m->name, j,
                    G.metrics[j].is_counter ? "counter" : "gauge",
                    i, m->is_counter ? "counter" : "gauge");
            }
        }
        if (m->labels[0] && m->labels[0] != '=')
            g_string_append_printf(buf, "%s{%s} %.6g\n", m->name, m->labels, m->value);
        else
            g_string_append_printf(buf, "%s %.6g\n", m->name, m->value);
    }
    g_hash_table_destroy(first_seen);
    g_mutex_unlock(&G.mu);

    return g_string_free(buf, FALSE);
}
