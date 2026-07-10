/* tests/test_prometheus.c
 *
 * Prometheus exporter 유닛 테스트 — Counter 체크포인트 / 타입 불일치 /
 * 메트릭 이름 검증 / 레이블 형식 검증
 *
 * ============================================================================
 *  이 파일이 테스트하는 것
 * ============================================================================
 *  prometheus_exporter.c (src/modules/daemons/)의 Counter 체크포인트 직렬화
 *  로직과 입력 검증 규칙을 데몬 의존성 없이 단독으로 검증한다.
 *  핵심 테스트 케이스를 영역별로 등록한다.
 *
 *  1. Counter 체크포인트 save/restore 라운드트립
 *     - 여러 counter 값을 JSON으로 직렬화 후 역직렬화 — 값 일치 확인
 *     - 빈 counters 배열 — 정상 처리
 *     - 손상된 JSON (비-오브젝트 루트) — graceful 거부
 *
 *  2. Counter 체크포인트 손상 파일 처리
 *     - 루트가 배열 [] — 비-오브젝트 감지
 *     - null 문자열 파싱 — graceful 거부
 *     - 잘린 JSON — parse 실패 감지
 *
 *  3. 타입 불일치 감지
 *     - counter로 등록한 이름을 gauge로 재등록 시도 — 불일치 감지
 *     - 같은 타입 재등록 — 성공 (멱등성)
 *
 *  4. 메트릭 이름 검증
 *     - 유효한 이름: [a-zA-Z_:][a-zA-Z0-9_:]* — 통과
 *     - 숫자로 시작하는 이름 — 거부
 *     - 빈 이름 / 공백 포함 — 거부
 *
 *  5. 레이블 문자열 형식 검증
 *     - 유효한 레이블: key="value" 쌍 — 통과
 *     - bare token, 따옴표 미닫힘, 잘못된 key — 거부
 *     - 빈 레이블 문자열 — 통과 (레이블 없는 메트릭은 허용)
 *
 *  왜 DAEMON_SRCS를 직접 링크하지 않는가?
 *  → prometheus_exporter.c는 GMainLoop, pcv_log, ebpf_telemetry 등 데몬 전용
 *    의존성이 있어서 테스트 바이너리에 링크하면 수십 개 심볼 미해결 에러가
 *    발생한다. 대신 체크포인트/검증 로직을 이 파일에 재현한다.
 * ============================================================================
 */

#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include "modules/daemons/ebpf_telemetry.h"
#include "modules/daemons/prometheus_exporter.h"

#ifndef PCV_PROM_MAX_METRICS
#define PCV_PROM_MAX_METRICS 2048
#endif

#define PCV_PROM_LARGE_HOST_MIN_SLOTS ((256 * 8) + 512)

/* ── 체크포인트 JSON 구조 재현 ─────────────────────────────────────────────
 *
 * prometheus_exporter.c의 _counters_save_unlocked() / _counters_load()가
 * 사용하는 JSON 구조:
 *
 *   {
 *     "saved_at": <unix_timestamp>,
 *     "counters": [
 *       { "name": "rpc_total", "labels": "method=\"vm.list\"", "value": 42 },
 *       ...
 *     ]
 *   }
 */

/** Counter 엔트리를 표현하는 경량 구조체 (테스트 전용) */
typedef struct {
    const gchar *name;
    const gchar *labels;
    gdouble      value;
} CounterEntry;

/**
 * 여러 CounterEntry를 체크포인트 JSON 문자열로 직렬화한다.
 * prometheus_exporter.c의 _counters_save_unlocked() 로직을 재현.
 * 반환값은 g_free()로 해제해야 한다.
 */
static gchar *
_serialize_counters(const CounterEntry *entries, guint n_entries)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "saved_at");
    json_builder_add_int_value(b, (gint64)1713916800);  /* 고정 타임스탬프 (재현성) */
    json_builder_set_member_name(b, "counters");
    json_builder_begin_array(b);
    for (guint i = 0; i < n_entries; i++) {
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "name");
        json_builder_add_string_value(b, entries[i].name);
        json_builder_set_member_name(b, "labels");
        json_builder_add_string_value(b, entries[i].labels);
        json_builder_set_member_name(b, "value");
        json_builder_add_double_value(b, entries[i].value);
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
    return data;
}

/**
 * JSON 문자열에서 counters 배열을 역직렬화하여 GArray에 담아 반환한다.
 * prometheus_exporter.c의 _counters_load() 로직을 재현.
 * 반환값은 g_array_free(result, TRUE)로 해제해야 한다.
 * 파싱 실패 시 NULL 반환.
 */
typedef struct {
    gchar   name[128];
    gchar   labels[128];
    gdouble value;
} RestoredCounter;

static GArray *
_deserialize_counters(const gchar *json_str)
{
    if (!json_str) return NULL;

    JsonParser *jp = json_parser_new();
    GError *err = NULL;
    if (!json_parser_load_from_data(jp, json_str, -1, &err)) {
        g_clear_error(&err);
        g_object_unref(jp);
        return NULL;
    }

    JsonNode *root_n = json_parser_get_root(jp);
    /* B12-M2 패턴: 루트가 오브젝트인지 먼저 검증 */
    if (!root_n || !JSON_NODE_HOLDS_OBJECT(root_n)) {
        g_object_unref(jp);
        return NULL;
    }

    JsonObject *root = json_node_get_object(root_n);
    if (!json_object_has_member(root, "counters")) {
        g_object_unref(jp);
        return NULL;
    }

    JsonArray *arr = json_object_get_array_member(root, "counters");
    if (!arr) {
        g_object_unref(jp);
        return NULL;
    }

    GArray *result = g_array_new(FALSE, TRUE, sizeof(RestoredCounter));
    guint n = json_array_get_length(arr);
    for (guint i = 0; i < n; i++) {
        JsonObject *o = json_array_get_object_element(arr, i);
        if (!o) continue;
        const gchar *name   = json_object_get_string_member(o, "name");
        const gchar *labels = json_object_get_string_member(o, "labels");
        gdouble      value  = json_object_get_double_member(o, "value");
        if (!name || !labels) continue;
        RestoredCounter rc = {0};
        g_strlcpy(rc.name,   name,   sizeof(rc.name));
        g_strlcpy(rc.labels, labels, sizeof(rc.labels));
        rc.value = value;
        g_array_append_val(result, rc);
    }

    g_object_unref(jp);
    return result;
}

/* ── 1. Counter 체크포인트 save/restore 라운드트립 ─────────────────────── */

/** 여러 counter를 직렬화 후 역직렬화하면 값이 일치해야 한다 */
static void test_checkpoint_roundtrip_multiple(void) {
    const CounterEntry entries[] = {
        { "purecvisor_rpc_requests_total", "method=\"vm.list\",status=\"ok\"",  42.0 },
        { "purecvisor_rpc_requests_total", "method=\"vm.start\",status=\"ok\"", 7.0  },
        { "purecvisor_rpc_errors_total",   "method=\"vm.delete\"",              3.0  },
    };
    const guint n = G_N_ELEMENTS(entries);

    gchar *json_str = _serialize_counters(entries, n);
    g_assert_nonnull(json_str);

    GArray *restored = _deserialize_counters(json_str);
    g_assert_nonnull(restored);
    g_assert_cmpuint(restored->len, ==, n);

    for (guint i = 0; i < n; i++) {
        RestoredCounter *rc = &g_array_index(restored, RestoredCounter, i);
        g_assert_cmpstr(rc->name,   ==, entries[i].name);
        g_assert_cmpstr(rc->labels, ==, entries[i].labels);
        g_assert_cmpfloat_with_epsilon(rc->value, entries[i].value, 1e-9);
    }

    g_array_free(restored, TRUE);
    g_free(json_str);
}

/** 빈 counters 배열 — 역직렬화 성공, 엔트리 수 0 */
static void test_checkpoint_roundtrip_empty(void) {
    gchar *json_str = _serialize_counters(NULL, 0);
    g_assert_nonnull(json_str);

    GArray *restored = _deserialize_counters(json_str);
    g_assert_nonnull(restored);
    g_assert_cmpuint(restored->len, ==, 0);

    g_array_free(restored, TRUE);
    g_free(json_str);
}

/** 루트가 오브젝트가 아닌 경우 — _deserialize_counters가 NULL을 반환해야 한다 */
static void test_checkpoint_corrupted_non_object_root(void) {
    /* 배열 루트: Prometheus 체크포인트 포맷이 아님 */
    const gchar *bad_json = "[{\"name\":\"x\",\"labels\":\"\",\"value\":1}]";
    GArray *result = _deserialize_counters(bad_json);
    /* 루트가 오브젝트가 아니므로 NULL이어야 한다 */
    g_assert_null(result);
    if (result) g_array_free(result, TRUE);
}

/* ── 2. Counter 체크포인트 손상 파일 처리 ─────────────────────────────── */

/** 루트가 JSON 배열 [] — 비-오브젝트이므로 graceful 거부 */
static void test_checkpoint_corrupted_array_root(void) {
    const gchar *json_str = "[]";
    GArray *result = _deserialize_counters(json_str);
    g_assert_null(result);
    if (result) g_array_free(result, TRUE);
}

/** "null" 문자열 — 파싱은 성공하지만 오브젝트가 아니므로 거부 */
static void test_checkpoint_corrupted_null_root(void) {
    const gchar *json_str = "null";
    GArray *result = _deserialize_counters(json_str);
    g_assert_null(result);
    if (result) g_array_free(result, TRUE);
}

/** 잘린 JSON — json_parser_load_from_data가 FALSE를 반환해야 한다 */
static void test_checkpoint_corrupted_truncated(void) {
    const gchar *json_str = "{\"counters\":{";  /* 잘린 JSON */
    GArray *result = _deserialize_counters(json_str);
    /* 파싱 실패 → NULL */
    g_assert_null(result);
    if (result) g_array_free(result, TRUE);
}

/* ── 3. 타입 불일치 감지 ────────────────────────────────────────────────
 *
 * prometheus_exporter.c의 PromMetric.is_counter 필드를 기반으로,
 * 같은 이름을 다른 타입으로 재등록할 때 불일치를 감지하는 로직을 재현한다.
 */

typedef struct {
    gchar    name[128];
    gboolean is_counter;
} MetricReg;

#define MAX_TEST_METRICS 32

typedef struct {
    MetricReg entries[MAX_TEST_METRICS];
    gint      count;
} TestRegistry;

/**
 * 테스트 레지스트리에 메트릭을 등록한다.
 * - 이름이 이미 존재하고 타입이 다르면 FALSE 반환 (타입 불일치)
 * - 이름이 없으면 새 슬롯 등록 후 TRUE 반환
 * - 이름이 있고 타입이 같으면 TRUE 반환 (멱등 재등록)
 */
static gboolean
_registry_register(TestRegistry *reg, const gchar *name, gboolean is_counter)
{
    if (!name || name[0] == '\0') return FALSE;
    for (gint i = 0; i < reg->count; i++) {
        if (g_strcmp0(reg->entries[i].name, name) == 0) {
            /* 이름 충돌: 타입 일치 여부 확인 */
            return reg->entries[i].is_counter == is_counter;
        }
    }
    if (reg->count >= MAX_TEST_METRICS) return FALSE;
    gint idx = reg->count++;
    g_strlcpy(reg->entries[idx].name, name, sizeof(reg->entries[idx].name));
    reg->entries[idx].is_counter = is_counter;
    return TRUE;
}

/** counter로 등록한 이름을 gauge로 재등록하면 FALSE */
static void test_type_mismatch_counter_then_gauge(void) {
    TestRegistry reg = {0};
    /* 1차: counter 등록 — 성공 */
    g_assert_true(_registry_register(&reg, "rpc_total", TRUE));
    /* 2차: 같은 이름 gauge 등록 — 타입 불일치, 실패 */
    g_assert_false(_registry_register(&reg, "rpc_total", FALSE));
}

/** 같은 타입(counter)으로 재등록하면 TRUE (멱등성) */
static void test_type_same_reregister(void) {
    TestRegistry reg = {0};
    g_assert_true(_registry_register(&reg, "rpc_total", TRUE));
    /* 같은 타입 재등록 — 성공 */
    g_assert_true(_registry_register(&reg, "rpc_total", TRUE));
    /* 등록된 슬롯 수는 1이어야 한다 */
    g_assert_cmpint(reg.count, ==, 1);
}

/** 256 CPU급 호스트의 node_cpu_seconds_total + 기본 node/purecvisor 메트릭을 담을 수 있어야 한다 */
static void test_registry_capacity_large_host_budget(void) {
    g_assert_cmpint(PCV_PROM_MAX_METRICS, >=, PCV_PROM_LARGE_HOST_MIN_SLOTS);
}

/** /proc/stat 집계 행 "cpu  ..."는 코어별 metric label로 쓰면 안 된다 */
static void test_node_cpu_parser_skips_aggregate_line(void) {
    g_assert_false(pcv_ebpf_proc_stat_is_cpu_core_line(
        "cpu  3726 0 556809 739 0 7 0 0 0 0"));
    g_assert_true(pcv_ebpf_proc_stat_is_cpu_core_line(
        "cpu0 34 0 101 355351 8 0 0 0 0 0"));
}

/* ── 4. 메트릭 이름 검증 ────────────────────────────────────────────────
 *
 * Prometheus 규격: 메트릭 이름은 [a-zA-Z_:][a-zA-Z0-9_:]* 패턴이어야 한다.
 * https://prometheus.io/docs/concepts/data_model/#metric-names-and-labels
 */

/**
 * 메트릭 이름이 Prometheus 규격에 맞는지 검증한다.
 * 규격: 첫 글자는 [a-zA-Z_:], 이후는 [a-zA-Z0-9_:]*
 */
static gboolean
_validate_metric_name(const gchar *name)
{
    if (!name || name[0] == '\0') return FALSE;

    /* 첫 글자: 알파벳, 밑줄, 콜론만 허용 */
    if (!g_ascii_isalpha(name[0]) && name[0] != '_' && name[0] != ':')
        return FALSE;

    /* 이후 글자: 알파벳, 숫자, 밑줄, 콜론만 허용 */
    for (gsize i = 1; name[i] != '\0'; i++) {
        if (!g_ascii_isalnum(name[i]) && name[i] != '_' && name[i] != ':')
            return FALSE;
    }
    return TRUE;
}

/** 유효한 메트릭 이름들 — 전부 통과해야 한다 */
static void test_metric_name_valid(void) {
    g_assert_true(_validate_metric_name("rpc_total"));
    g_assert_true(_validate_metric_name("purecvisor_rpc_requests_total"));
    g_assert_true(_validate_metric_name("node_cpu_seconds_total"));
    g_assert_true(_validate_metric_name("_private_metric"));
    g_assert_true(_validate_metric_name(":colon_prefix"));
    g_assert_true(_validate_metric_name("CamelCase123"));
}

/** 유효하지 않은 메트릭 이름들 — 전부 거부해야 한다 */
static void test_metric_name_invalid(void) {
    /* 숫자로 시작 */
    g_assert_false(_validate_metric_name("9rpc_total"));
    g_assert_false(_validate_metric_name("0counter"));
    /* 빈 이름 */
    g_assert_false(_validate_metric_name(""));
    g_assert_false(_validate_metric_name(NULL));
    /* 공백 포함 */
    g_assert_false(_validate_metric_name("rpc total"));
    g_assert_false(_validate_metric_name("rpc-total"));   /* 하이픈 금지 */
    g_assert_false(_validate_metric_name("rpc.total"));   /* 점 금지 */
}

/* ── 5. 레이블 문자열 형식 검증 ─────────────────────────────────────────
 *
 * prometheus_exporter.c의 pcv_prom_inc()가 생성하는 레이블 형식:
 *   key="value"
 *   key1="value1",key2="value2"
 *
 * 검증 규칙:
 *   - 빈 문자열은 허용 (레이블 없는 메트릭)
 *   - key가 존재하면 ="..." 패턴이어야 한다
 *   - 따옴표가 열리면 반드시 닫혀야 한다
 *   - bare token만 전달하는 형태는 금지한다
 */

/**
 * 레이블 문자열이 key="value" 또는 key1="value1",key2="value2" 형식인지 검증한다.
 * 빈 문자열 허용. bare token만 전달하면 Prometheus scrape 전체가 실패하므로 거부한다.
 */
static gboolean
_validate_label_format(const gchar *labels)
{
    if (!labels) return FALSE;
    if (labels[0] == '\0') return TRUE;   /* 빈 레이블은 허용 */

    const gchar *p = labels;
    while (*p != '\0') {
        if (!g_ascii_isalpha(*p) && *p != '_') return FALSE;
        p++;

        while (g_ascii_isalnum(*p) || *p == '_') p++;
        if (p[0] != '=' || p[1] != '"') return FALSE;
        p += 2;

        while (*p != '\0') {
            if (*p == '\\' && p[1] != '\0') {
                p += 2;
                continue;
            }
            if (*p == '"') break;
            p++;
        }
        if (*p != '"') return FALSE;
        p++;

        if (*p == '\0') return TRUE;
        if (*p != ',') return FALSE;
        p++;
        if (*p == '\0') return FALSE;
    }

    return TRUE;
}

/** 유효한 레이블 형식 — 통과해야 한다 */
static void test_label_format_valid(void) {
    /* 빈 레이블 (레이블 없는 메트릭) */
    g_assert_true(_validate_label_format(""));
    /* 단일 key="value" */
    g_assert_true(_validate_label_format("method=\"vm.list\""));
    /* 복합 레이블 */
    g_assert_true(_validate_label_format(
        "method=\"vm.list\",status=\"ok\""));
    /* 공백 포함 값 */
    g_assert_true(_validate_label_format(
        "version=\"1.0 beta\""));
    /* self_healing dry-run policy처럼 하이픈을 값에 포함하는 레이블 */
    g_assert_true(_validate_label_format(
        "policy=\"vm-unresponsive\""));
}

/** 유효하지 않은 레이블 형식 — 거부해야 한다 */
static void test_label_format_invalid(void) {
    /* 따옴표 미닫힘 (홀수 개) */
    g_assert_false(_validate_label_format("method=\"vm.list"));
    g_assert_false(_validate_label_format(
        "method=\"vm.list\",status=\"ok"));
    /* bare token은 레이블 key가 없어 Prometheus text format이 아니다 */
    g_assert_false(_validate_label_format("vm-unresponsive"));
    g_assert_false(_validate_label_format("policy"));
    /* 레이블 key에는 하이픈을 사용할 수 없다 */
    g_assert_false(_validate_label_format("policy-name=\"vm-unresponsive\""));
    /* NULL 입력 */
    g_assert_false(_validate_label_format(NULL));
}

/* ── 등록 ────────────────────────────────────────────────────────────────
 * test_main.c에서 호출. g_test_add_func()로 GLib 테스트 프레임워크에 등록.
 */
void test_prometheus_register(void) {
    /* 1. Counter 체크포인트 라운드트립 */
    g_test_add_func("/prometheus/checkpoint/roundtrip_multiple",
                    test_checkpoint_roundtrip_multiple);
    g_test_add_func("/prometheus/checkpoint/roundtrip_empty",
                    test_checkpoint_roundtrip_empty);
    g_test_add_func("/prometheus/checkpoint/corrupted_non_object_root",
                    test_checkpoint_corrupted_non_object_root);

    /* 2. 손상 파일 처리 */
    g_test_add_func("/prometheus/checkpoint/corrupted_array_root",
                    test_checkpoint_corrupted_array_root);
    g_test_add_func("/prometheus/checkpoint/corrupted_null_root",
                    test_checkpoint_corrupted_null_root);
    g_test_add_func("/prometheus/checkpoint/corrupted_truncated",
                    test_checkpoint_corrupted_truncated);

    /* 3. 타입 불일치 감지 */
    g_test_add_func("/prometheus/registry/type_mismatch_counter_then_gauge",
                    test_type_mismatch_counter_then_gauge);
    g_test_add_func("/prometheus/registry/type_same_reregister",
                    test_type_same_reregister);
    g_test_add_func("/prometheus/registry/capacity_large_host_budget",
                    test_registry_capacity_large_host_budget);
    g_test_add_func("/prometheus/node_cpu_parser/skips_aggregate_line",
                    test_node_cpu_parser_skips_aggregate_line);

    /* 4. 메트릭 이름 검증 */
    g_test_add_func("/prometheus/metric_name/valid",
                    test_metric_name_valid);
    g_test_add_func("/prometheus/metric_name/invalid",
                    test_metric_name_invalid);

    /* 5. 레이블 형식 검증 */
    g_test_add_func("/prometheus/label_format/valid",
                    test_label_format_valid);
    g_test_add_func("/prometheus/label_format/invalid",
                    test_label_format_invalid);
}
