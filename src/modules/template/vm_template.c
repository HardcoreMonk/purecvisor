/**
 * @file vm_template.c
 * @brief VM 템플릿 관리 모듈 — JSON 파일 기반 CRUD + 내장 프리셋
 *
 * [파일 역할]
 *   VM 생성 시 사전 정의된 리소스 프로필(vCPU, 메모리, 디스크, OS, cloud-init 등)을
 *   JSON 파일로 저장/조회/삭제하는 CRUD 모듈입니다. DB 없이 파일 시스템 기반으로
 *   동작하여 단순성을 확보합니다. 초기화 시 내장 프리셋 3종을 자동 생성합니다.
 *
 * [아키텍처 위치]
 *   main.c (데몬 시작)
 *     -> pcv_vm_template_init() -> 디렉터리 생성 + 프리셋 자동 생성
 *   handler_template.c (template.* RPC 핸들러)
 *     -> pcv_vm_template_create/delete/get/list()  [이 파일]
 *   handler_vm_start.c (vm.create 시 템플릿 참조)
 *     -> pcv_vm_template_get(name) -> 템플릿 값으로 VM 생성 파라미터 구성
 *
 * [데이터 흐름 — 템플릿 기반 VM 생성]
 *   1. 사용자: pcvctl template create web --vcpu 2 --memory_mb 2048 ...
 *   2. handler_template.c -> pcv_vm_template_create() -> JSON 파일 저장
 *   3. 사용자: pcvctl vm create myvm --template web
 *   4. vm.create 핸들러 -> pcv_vm_template_get("web") -> PcvVmTemplate 반환
 *   5. 템플릿의 vcpu/memory/disk 값으로 virt-install 명령 구성
 *
 * [내장 프리셋 (init 시 파일이 없을 때만 자동 생성)]
 *   ubuntu-small:  2 vCPU,  2 GB,  20 GB  (개발/테스트)
 *   ubuntu-medium: 4 vCPU,  4 GB,  40 GB  (일반 서비스)
 *   ubuntu-large:  8 vCPU,  8 GB,  80 GB  (고성능 서비스)
 *   모두 ubuntu24.04, 관리형 기본 네트워크(pcvnat0), cloud-image 기반
 *
 * [저장 구조]
 *   경로: /etc/purecvisor/templates/<name>.json (1 템플릿 = 1 파일)
 *   직렬화: json-glib (JsonParser/JsonGenerator), pretty-print 4칸 들여쓰기
 *   역직렬화: 부분 JSON 허용 (존재하는 필드만 읽음, 하위 호환성)
 *
 * [핵심 패턴]
 *   - 멱등성: pcv_vm_template_delete()는 파일 미존재 시에도 TRUE 반환
 *   - 프리셋 보호: init()은 파일 존재 여부만 확인, 내용은 검증 안 함
 *     -> 사용자가 프리셋 JSON을 수동 편집하면 그 내용이 유지됨
 *   - 중복 방지: pcv_vm_template_create()는 동명 파일 존재 시 에러 반환
 *
 * [스레드 안전]
 *   파일 I/O 기반이므로 별도 Mutex 없음.
 *   동시 쓰기 빈도가 매우 낮아(관리자 수동 조작) 실질적 문제 없음.
 *
 * [주의사항]
 *   - TEMPLATE_DIR 상수 변경 시 daemon.conf 연동은 미구현 (하드코딩)
 *   - 파일 이름 = 템플릿 이름이므로 특수문자가 포함되면 파일시스템 문제 가능
 *     (pcv_validate에서 사전 검증 필요)
 */

#include "vm_template.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <errno.h>

/* ══════════════════════════════════════════════════════════════
 * [1] 상수
 * ══════════════════════════════════════════════════════════════ */

#define TEMPLATE_DIR "/etc/purecvisor/templates"  /* 템플릿 JSON 저장 디렉터리 */

/* ══════════════════════════════════════════════════════════════
 * [2] 내부 헬퍼 — 경로 생성 / JSON 직렬화·역직렬화
 * ══════════════════════════════════════════════════════════════ */

/**
 * _template_path:
 * @name: 템플릿 이름 (예: "ubuntu-small")
 *
 * 템플릿 이름을 파일 경로로 변환합니다.
 * 예: "ubuntu-small" → "/etc/purecvisor/templates/ubuntu-small.json"
 *
 * Returns: (transfer full): 파일 경로 문자열 (g_free 필요)
 */
static gchar *
_template_path(const gchar *name)
{
    return g_strdup_printf("%s/%s.json", TEMPLATE_DIR, name);
}

/**
 * _template_to_json:
 * @t: 직렬화할 템플릿 구조체
 *
 * PcvVmTemplate 구조체를 JsonObject로 변환합니다.
 * 필수 필드(name, vcpu, memory_mb, disk_gb, os_variant)는 항상 포함하고,
 * 선택 필드(iso_path, network_bridge, cloud_init_user_data, description)는
 * NULL이 아닐 때만 포함합니다.
 *
 * Returns: (transfer full): JsonObject* (json_object_unref 필요)
 */
static JsonObject *
_template_to_json(const PcvVmTemplate *t)
{
    JsonObject *obj = json_object_new();

    json_object_set_string_member(obj, "name", t->name ? t->name : "");
    json_object_set_int_member(obj, "vcpu", t->vcpu);
    json_object_set_int_member(obj, "memory_mb", t->memory_mb);
    json_object_set_int_member(obj, "disk_gb", t->disk_gb);
    json_object_set_string_member(obj, "os_variant",
                                  t->os_variant ? t->os_variant : "");

    if (t->iso_path)
        json_object_set_string_member(obj, "iso_path", t->iso_path);
    if (t->network_bridge)
        json_object_set_string_member(obj, "network_bridge", t->network_bridge);
    if (t->cloud_init_user_data)
        json_object_set_string_member(obj, "cloud_init_user_data",
                                      t->cloud_init_user_data);
    if (t->description)
        json_object_set_string_member(obj, "description", t->description);

    return obj;
}

/**
 * _parse_template_file:
 * @path: JSON 파일 절대 경로
 *
 * JSON 파일을 읽어 PcvVmTemplate 구조체로 역직렬화합니다.
 * 각 필드는 존재할 때만 읽으므로, 부분 JSON도 허용됩니다 (하위 호환성).
 *
 * Returns: (transfer full): 새로 할당된 PcvVmTemplate* (pcv_vm_template_free 필요)
 *   파싱 실패 시 NULL
 */
static PcvVmTemplate *
_parse_template_file(const gchar *path)
{
    JsonParser *parser = json_parser_new();
    GError *err = NULL;

    if (!json_parser_load_from_file(parser, path, &err)) {
        g_printerr("[template] Failed to parse %s: %s\n",
                    path, err->message);
        g_error_free(err);
        g_object_unref(parser);
        return NULL;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_printerr("[template] Invalid JSON in %s\n", path);
        g_object_unref(parser);
        return NULL;
    }

    JsonObject *obj = json_node_get_object(root);

    PcvVmTemplate *t = g_new0(PcvVmTemplate, 1);

    if (json_object_has_member(obj, "name"))
        t->name = g_strdup(json_object_get_string_member(obj, "name"));
    if (json_object_has_member(obj, "vcpu"))
        t->vcpu = (gint)json_object_get_int_member(obj, "vcpu");
    if (json_object_has_member(obj, "memory_mb"))
        t->memory_mb = (gint)json_object_get_int_member(obj, "memory_mb");
    if (json_object_has_member(obj, "disk_gb"))
        t->disk_gb = (gint)json_object_get_int_member(obj, "disk_gb");
    if (json_object_has_member(obj, "os_variant"))
        t->os_variant = g_strdup(json_object_get_string_member(obj, "os_variant"));
    if (json_object_has_member(obj, "iso_path"))
        t->iso_path = g_strdup(json_object_get_string_member(obj, "iso_path"));
    if (json_object_has_member(obj, "network_bridge"))
        t->network_bridge = g_strdup(json_object_get_string_member(obj, "network_bridge"));
    if (json_object_has_member(obj, "cloud_init_user_data"))
        t->cloud_init_user_data = g_strdup(json_object_get_string_member(obj, "cloud_init_user_data"));
    if (json_object_has_member(obj, "description"))
        t->description = g_strdup(json_object_get_string_member(obj, "description"));

    g_object_unref(parser);
    return t;
}

/**
 * _json_object_to_string:
 * @obj: 직렬화할 JsonObject
 *
 * JsonObject를 사람이 읽기 편한 pretty-print JSON 문자열로 변환합니다.
 * 들여쓰기 4칸으로 포맷합니다.
 *
 * Returns: (transfer full): JSON 문자열 (g_free 필요)
 */
static gchar *
_json_object_to_string(JsonObject *obj)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, obj);

    JsonGenerator *gen = json_generator_new();
    json_generator_set_pretty(gen, TRUE);
    json_generator_set_indent(gen, 4);
    json_generator_set_root(gen, node);

    gchar *data = json_generator_to_data(gen, NULL);

    g_object_unref(gen);
    json_node_free(node);

    return data;
}

/* ══════════════════════════════════════════════════════════════
 * [3] 내장 프리셋 정의 — 초기 설치 시 자동 생성되는 기본 템플릿
 * ══════════════════════════════════════════════════════════════ */

/**
 * PresetDef:
 * 내장 프리셋을 정의하는 컴파일 타임 상수 구조체.
 * const 포인터이므로 해제 불필요 (정적 문자열 리터럴).
 */
typedef struct {
    const gchar *name;
    gint         vcpu;
    gint         memory_mb;
    gint         disk_gb;
    const gchar *os_variant;
    const gchar *iso_path;
    const gchar *network_bridge;
    const gchar *description;
} PresetDef;

static const PresetDef builtin_presets[] = {
    {
        "ubuntu-small", 2, 2048, 20,
        "ubuntu24.04",
        "/pcvpool/iso/ubuntu-24.04-server-cloudimg-amd64.img",
        NULL,   /* VP-1: 브릿지 미지정 → vm.create resolve가 관리형 기본 네트워크(pcvnat0)로 부착 */
        "Ubuntu 24.04 Small Instance (2 vCPU, 2 GB, 20 GB)"
    },
    {
        "ubuntu-medium", 4, 4096, 40,
        "ubuntu24.04",
        "/pcvpool/iso/ubuntu-24.04-server-cloudimg-amd64.img",
        NULL,   /* VP-1: 브릿지 미지정 → vm.create resolve가 관리형 기본 네트워크(pcvnat0)로 부착 */
        "Ubuntu 24.04 Medium Instance (4 vCPU, 4 GB, 40 GB)"
    },
    {
        "ubuntu-large", 8, 8192, 80,
        "ubuntu24.04",
        "/pcvpool/iso/ubuntu-24.04-server-cloudimg-amd64.img",
        NULL,   /* VP-1: 브릿지 미지정 → vm.create resolve가 관리형 기본 네트워크(pcvnat0)로 부착 */
        "Ubuntu 24.04 Large Instance (8 vCPU, 8 GB, 80 GB)"
    },
};

/* 배열 크기를 컴파일 타임에 계산 — 새 프리셋 추가 시 자동 반영 */
#define N_PRESETS (sizeof(builtin_presets) / sizeof(builtin_presets[0]))

/* ══════════════════════════════════════════════════════════════
 * [4] 공개 API — 초기화 / 종료
 * ══════════════════════════════════════════════════════════════ */

/**
 * pcv_vm_template_init:
 *
 * 템플릿 모듈을 초기화합니다:
 *   1) /etc/purecvisor/templates/ 디렉터리 생성 (없으면)
 *   2) 내장 프리셋 3개를 파일로 생성 (이미 있으면 건너뜀)
 *
 * 프리셋은 파일이 존재하는지만 확인하고, 내용은 검증하지 않습니다.
 * 사용자가 프리셋 파일을 수정했다면 그 내용이 유지됩니다.
 */
void
pcv_vm_template_init(void)
{
    /* 디렉터리 생성 (재귀) — 부모 경로가 없어도 자동 생성 */
    if (g_mkdir_with_parents(TEMPLATE_DIR, 0755) != 0) {
        g_printerr("[template] Failed to create %s: %s\n",
                    TEMPLATE_DIR, g_strerror(errno));
        return;
    }

    /* 내장 프리셋 — 파일이 없을 때만 생성 (멱등성 보장) */
    for (gsize i = 0; i < N_PRESETS; i++) {
        const PresetDef *p = &builtin_presets[i];
        gchar *path = _template_path(p->name);

        if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
            PcvVmTemplate tmpl = {
                .name            = (gchar *)p->name,
                .vcpu            = p->vcpu,
                .memory_mb       = p->memory_mb,
                .disk_gb         = p->disk_gb,
                .os_variant      = (gchar *)p->os_variant,
                .iso_path        = (gchar *)p->iso_path,
                .network_bridge  = (gchar *)p->network_bridge,
                .cloud_init_user_data = NULL,
                .description     = (gchar *)p->description,
            };
            GError *err = NULL;
            if (!pcv_vm_template_create(&tmpl, &err)) {
                g_printerr("[template] Preset '%s' creation failed: %s\n",
                            p->name, err ? err->message : "unknown");
                if (err) g_error_free(err);
            } else {
                g_print("[template] Created built-in preset: %s\n", p->name);
            }
        }
        g_free(path);
    }

    g_print("[template] Initialized (%s)\n", TEMPLATE_DIR);
}

void
pcv_vm_template_shutdown(void)
{
    /* 현재 no-op — 향후 캐시 해제 등 */
}

/* ══════════════════════════════════════════════════════════════
 * [5] 공개 API — 템플릿 CRUD
 * ══════════════════════════════════════════════════════════════ */

/**
 * pcv_vm_template_create:
 * @tmpl:  저장할 템플릿 구조체 (name 필수)
 * @error: GError 반환
 *
 * 템플릿을 JSON 파일로 저장합니다.
 * 동명 파일이 이미 있으면 에러를 반환합니다 (덮어쓰기 방지).
 *
 * 내부 흐름:
 *   PcvVmTemplate → JsonObject(_template_to_json) → 문자열 → 파일 쓰기
 *
 * Returns: 성공 시 TRUE
 */
gboolean
pcv_vm_template_create(PcvVmTemplate *tmpl, GError **error)
{
    g_return_val_if_fail(tmpl != NULL, FALSE);
    g_return_val_if_fail(tmpl->name != NULL && tmpl->name[0] != '\0', FALSE);

    /* ISO 경로 검증 — 절대 경로 필수, 경로 순회 차단 */
    if (tmpl->iso_path && (strstr(tmpl->iso_path, "..") || tmpl->iso_path[0] != '/')) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                    "iso_path must be absolute and must not contain '..': %s",
                    tmpl->iso_path);
        return FALSE;
    }

    /* 중복 확인 */
    gchar *existing = _template_path(tmpl->name);
    if (g_file_test(existing, G_FILE_TEST_EXISTS)) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_EXIST,
                    "Template already exists: %s", tmpl->name);
        g_free(existing);
        return FALSE;
    }
    g_free(existing);

    /* 디렉터리 보장 */
    if (g_mkdir_with_parents(TEMPLATE_DIR, 0755) != 0) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                    "Cannot create directory %s: %s",
                    TEMPLATE_DIR, g_strerror(errno));
        return FALSE;
    }

    JsonObject *obj = _template_to_json(tmpl);
    gchar *data = _json_object_to_string(obj);
    json_object_unref(obj);

    gchar *path = _template_path(tmpl->name);
    gboolean ok = g_file_set_contents(path, data, -1, error);

    if (ok)
        g_print("[template] Saved: %s\n", path);

    g_free(path);
    g_free(data);
    return ok;
}

/**
 * pcv_vm_template_delete:
 * @name:  삭제할 템플릿 이름
 * @error: GError 반환
 *
 * 해당 이름의 JSON 파일을 삭제합니다.
 * 멱등성: 파일이 없어도 TRUE를 반환합니다 (재시도 안전).
 *
 * Returns: 성공 시 TRUE
 */
gboolean
pcv_vm_template_delete(const gchar *name, GError **error)
{
    g_return_val_if_fail(name != NULL && name[0] != '\0', FALSE);

    gchar *path = _template_path(name);

    /* 멱등성: 파일이 없어도 성공 */
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_free(path);
        return TRUE;
    }

    if (g_unlink(path) != 0) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                    "Failed to delete %s: %s", path, g_strerror(errno));
        g_free(path);
        return FALSE;
    }

    g_print("[template] Deleted: %s\n", path);
    g_free(path);
    return TRUE;
}

/**
 * pcv_vm_template_get:
 * @name: 조회할 템플릿 이름
 *
 * 이름으로 단일 템플릿을 조회합니다.
 * 파일이 존재하면 파싱하여 반환하고, 없으면 NULL을 반환합니다.
 *
 * Returns: (transfer full): PcvVmTemplate* (pcv_vm_template_free 필요), 없으면 NULL
 */
PcvVmTemplate *
pcv_vm_template_get(const gchar *name)
{
    g_return_val_if_fail(name != NULL, NULL);

    gchar *path = _template_path(name);
    PcvVmTemplate *t = NULL;

    if (g_file_test(path, G_FILE_TEST_EXISTS))
        t = _parse_template_file(path);

    g_free(path);
    return t;
}

/**
 * pcv_vm_template_list:
 *
 * TEMPLATE_DIR의 모든 .json 파일을 스캔하여 템플릿 목록을 반환합니다.
 * 파싱 실패한 파일은 건너뛰고, 정상 파싱된 것만 포함합니다.
 *
 * Returns: (transfer full): GPtrArray of PcvVmTemplate*
 *   호출자가 g_ptr_array_unref()로 해제해야 합니다.
 */
GPtrArray *
pcv_vm_template_list(void)
{
    GPtrArray *arr = g_ptr_array_new_with_free_func(
                         (GDestroyNotify)pcv_vm_template_free);

    GDir *dir = g_dir_open(TEMPLATE_DIR, 0, NULL);
    if (!dir)
        return arr;

    const gchar *entry;
    while ((entry = g_dir_read_name(dir)) != NULL) {
        if (!g_str_has_suffix(entry, ".json"))
            continue;

        gchar *path = g_build_filename(TEMPLATE_DIR, entry, NULL);
        PcvVmTemplate *t = _parse_template_file(path);
        g_free(path);

        if (t)
            g_ptr_array_add(arr, t);
    }

    g_dir_close(dir);
    return arr;
}

/**
 * pcv_vm_template_free:
 * @t: 해제할 템플릿 구조체 (NULL 안전)
 *
 * PcvVmTemplate의 모든 동적 할당 멤버와 구조체 자체를 해제합니다.
 * GPtrArray의 free_func으로 등록되어 g_ptr_array_unref() 시 자동 호출됩니다.
 */
void
pcv_vm_template_free(PcvVmTemplate *t)
{
    if (!t) return;
    g_free(t->name);
    g_free(t->os_variant);
    g_free(t->iso_path);
    g_free(t->network_bridge);
    g_free(t->cloud_init_user_data);
    g_free(t->description);
    g_free(t);
}
