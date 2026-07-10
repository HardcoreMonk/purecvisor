/**
 * @file handler_template.c
 * @brief VM 템플릿 관리 RPC 핸들러 — 목록/조회/생성/삭제 (4개 메서드)
 *
 * [아키텍처 위치]
 *   클라이언트 -> UDS/REST -> dispatcher.c ("template.*") -> handle_template_*()
 *                                                              -> vm_template.c (JSON 파일 기반)
 *
 * [처리하는 RPC 메서드] (4개)
 *   template.list   -> handle_template_list   : 전체 템플릿 목록 반환 (JSON 배열)
 *   template.get    -> handle_template_get    : 단일 템플릿 상세 조회
 *     - params: { "name": "<템플릿명>" }
 *   template.create -> handle_template_create : 새 템플릿 생성 또는 기존 템플릿 갱신 (UPSERT)
 *     - params: { "name", "vcpu", "memory_mb", "disk_gb", "os_variant", ... }
 *   template.delete -> handle_template_delete : 템플릿 삭제
 *     - params: { "name": "<템플릿명>" }
 *
 * [fire-and-forget 패턴 미사용]
 *   모든 핸들러가 동기 응답입니다.
 *   템플릿 데이터는 JSON 파일 I/O만 수행하므로 즉시 완료됩니다.
 *
 * [템플릿 활용 방법]
 *   vm.create 호출 시 "template" 파라미터로 템플릿 이름을 지정하면
 *   vcpu, memory_mb, disk_gb, os_variant 등이 자동으로 채워집니다.
 *   기본 제공 프리셋: ubuntu-small (1코어/1GB), ubuntu-medium (2코어/4GB),
 *                     ubuntu-large (4코어/8GB), cloud-init 자동 프로비저닝 지원.
 *
 * [주의사항]
 *   - template.create는 동일 이름이 이미 존재하면 덮어씁니다 (UPSERT 동작).
 *   - template.delete는 없는 템플릿을 삭제 시도하면 에러를 반환하지만,
 *     클라이언트 측 재시도는 안전합니다 (부작용 없음).
 *
 * [에러 코드]
 *   -32602 : 필수 파라미터 누락, 빈 문자열, 범위 초과 (vcpu < 1 등)
 *   -32000 : 파일 I/O 실패 등 내부 오류
 */

/*
 * --- 헤더 인클루드 설명 ---
 *
 * handler_template.h : 이 파일의 공개 함수 선언 (handle_template_list/get/create/delete)
 * rpc_utils.h        : JSON-RPC 응답 빌더 + 에러코드 상수 + UDS 전송 함수
 * vm_template.h      : VM 템플릿 코어 모듈 — PcvVmTemplate 구조체,
 *                      pcv_vm_template_list/get/create/delete/free 함수
 *                      템플릿 저장소: /etc/purecvisor/templates/ 디렉터리 내 JSON 파일
 * uds_server.h       : UdsServer 타입 + pure_uds_server_send_response() 함수
 * pcv_validate.h     : pcv_validate_vm_name() — 이름 검증 (영문숫자, -, _ 만 허용)
 *                      Command Injection 방어를 위한 필수 입력 검증 함수
 */
#include "handler_template.h"
#include "rpc_utils.h"
#include "../../modules/template/vm_template.h"
#include "../../api/uds_server.h"
#include "../../utils/pcv_validate.h"

#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>

/* ══════════════════════════════════════════════════════════════════════
 * 내부 헬퍼: PcvVmTemplate 구조체 → JsonObject 직렬화
 *
 * static 함수이므로 이 파일 내부에서만 호출 가능합니다.
 * template.list 와 template.get 양쪽에서 코드 중복 없이 재사용합니다.
 * ══════════════════════════════════════════════════════════════════════*/

/**
 * _tmpl_to_json_obj:
 * @t: PcvVmTemplate 구조체 포인터
 *
 * C 구조체를 JSON 객체로 직렬화하는 내부 헬퍼.
 * template.list 와 template.get 에서 공통 사용.
 *
 * NULL 필드 처리:
 *   - name, os_variant: NULL 이면 빈 문자열("") 로 대체 (필수 필드이므로 보통 NULL 아님)
 *   - iso_path, network_bridge, cloud_init_user_data, description: NULL 이면 JSON에 포함하지 않음
 *
 * 반환된 JsonObject 의 소유권은 호출자에게 있음 → json_array_add_object_element 등으로 이전.
 */
static JsonObject *
_tmpl_to_json_obj(const PcvVmTemplate *t)
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

/* ══════════════════════════════════════════════════════════════════════
 * template.list
 * ══════════════════════════════════════════════════════════════════════*/

/**
 * handle_template_list:
 * @params: 사용하지 않음 (빈 객체 허용)
 * @rpc_id: 요청 ID
 * @server: UDS 서버 인스턴스
 * @conn: 클라이언트 소켓 연결
 *
 * 전체 템플릿 목록을 JSON 배열로 반환.
 * 반환 형식: [{"name","vcpu","memory_mb","disk_gb","os_variant",...}, ...]
 *
 * 메모리 해제 순서 주의:
 *   resp 전송 후 g_ptr_array_unref(list) → PcvVmTemplate 내부 문자열도 해제됨.
 *   JSON 직렬화 시 json_object_set_string_member 가 문자열을 복사하므로
 *   list 해제 시점과 무관하게 안전.
 */
void
handle_template_list(JsonObject *params, const gchar *rpc_id,
                     UdsServer *server, GSocketConnection *conn)
{
    (void)params;  /* 파라미터 불필요 — 컴파일러 unused 경고 억제 */

    GPtrArray *list = pcv_vm_template_list();  /* 전체 템플릿 조회 */

    JsonArray *arr = json_array_new();
    for (guint i = 0; i < list->len; i++) {
        PcvVmTemplate *t = g_ptr_array_index(list, i);
        /* _tmpl_to_json_obj 반환 객체의 소유권 → arr 에 이전 */
        json_array_add_object_element(arr, _tmpl_to_json_obj(t));
    }

    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, arr);  /* arr 소유권 → node 이전 */

    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, conn, resp);
    g_free(resp);

    g_ptr_array_unref(list);
}

/* ══════════════════════════════════════════════════════════════════════
 * template.get
 * ══════════════════════════════════════════════════════════════════════*/

/**
 * handle_template_get:
 * @params: JSON-RPC params — {"name"}
 * @rpc_id: 요청 ID
 * @server: UDS 서버 인스턴스
 * @conn: 클라이언트 소켓 연결
 *
 * 필수 파라미터: name (비어 있으면 -32602 에러)
 * 존재하지 않는 템플릿 → -32602 에러 (not found)
 * 성공 시 반환: 단일 템플릿 JSON 객체
 *
 * 메모리: 사용 후 pcv_vm_template_free(t) 로 구조체 해제 필수.
 */
void
handle_template_get(JsonObject *params, const gchar *rpc_id,
                    UdsServer *server, GSocketConnection *conn)
{
    if (!json_object_has_member(params, "name")) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required parameter: name");
        pure_uds_server_send_response(server, conn, resp);
        g_free(resp);
        return;
    }

    const gchar *name = json_object_get_string_member(params, "name");

    /*
     * 이름 검증 3단계:
     *   1. !name        : json_object_get_string_member가 NULL 반환 (타입 불일치 등)
     *   2. name[0]=='\0': 빈 문자열 거부
     *   3. pcv_validate_vm_name(name): Command Injection 방어 — 영문숫자, -, _ 만 허용
     *      예: "my-template" OK, "../../../etc/passwd" REJECT
     *
     * pcv_validate_vm_name은 시스템 명령에 전달될 수 있는 이름의 안전성을 보장합니다.
     * 템플릿 이름이 파일 경로(/etc/purecvisor/templates/<name>.json)에 사용되므로
     * 경로 탈출(path traversal) 공격도 함께 방어합니다.
     */
    if (!name || name[0] == '\0' || !pcv_validate_vm_name(name)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Parameter 'name' must be a valid identifier (alphanumeric, -, _)");
        pure_uds_server_send_response(server, conn, resp);
        g_free(resp);
        return;
    }

    /*
     * pcv_vm_template_get: 이름으로 템플릿 조회.
     * /etc/purecvisor/templates/<name>.json 파일을 읽어 PcvVmTemplate 구조체로 반환.
     * 미존재 시 NULL 반환 (GError가 아닌 NULL 패턴).
     * 반환된 구조체는 사용 후 pcv_vm_template_free()로 해제해야 합니다.
     */
    PcvVmTemplate *t = pcv_vm_template_get(name);
    if (!t) {
        gchar *msg = g_strdup_printf("Template not found: %s", name);
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS, msg);
        pure_uds_server_send_response(server, conn, resp);
        g_free(resp);
        g_free(msg);
        return;
    }

    JsonObject *obj = _tmpl_to_json_obj(t);  /* 구조체 → JSON 변환 (문자열 복사됨) */
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);  /* obj 소유권 → node 이전 */

    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, conn, resp);
    g_free(resp);

    pcv_vm_template_free(t);  /* JSON 직렬화 완료 후 원본 구조체 해제 */
}

/* ══════════════════════════════════════════════════════════════════════
 * template.create
 * ══════════════════════════════════════════════════════════════════════*/

/**
 * handle_template_create:
 * @params: JSON-RPC params — {"name","vcpu","memory_mb","disk_gb","os_variant",
 *                              "iso_path"?,"network_bridge"?,"cloud_init_user_data"?,"description"?}
 * @rpc_id: 요청 ID
 * @server: UDS 서버 인스턴스
 * @conn: 클라이언트 소켓 연결
 *
 * 필수 파라미터: name, vcpu, memory_mb, disk_gb, os_variant
 * 선택 파라미터: iso_path, network_bridge, cloud_init_user_data, description
 *
 * 값 범위 검증:
 *   - vcpu:      1 ~ 128 (물리 코어 수 초과 방지)
 *   - memory_mb: 128 ~ 1,048,576 (128MB ~ 1TB)
 *   - disk_gb:   1 ~ 65,536 (1GB ~ 64TB)
 *   → 범위 밖이면 -32602 에러 반환
 *
 * 동일 이름 템플릿이 존재하면 덮어쓰기 (UPSERT).
 * 성공 시 반환: {"status":"created", "name":"..."}
 */
void
handle_template_create(JsonObject *params, const gchar *rpc_id,
                       UdsServer *server, GSocketConnection *conn)
{
    /*
     * 필수 파라미터 일괄 검증 — 배열 순회로 누락된 키를 즉시 탐지.
     *
     * G_N_ELEMENTS(required): 배열 원소 수를 컴파일 타임에 계산하는 GLib 매크로.
     * sizeof(required)/sizeof(required[0]) 와 동일하지만 타입 안전합니다.
     *
     * 이 패턴은 필수 필드가 많을 때 if 중첩을 줄이는 데 유용합니다.
     * 누락된 필드명을 에러 메시지에 포함하여 클라이언트가 어떤 필드를 빠뜨렸는지 알 수 있습니다.
     */
    const gchar *required[] = {"name", "vcpu", "memory_mb", "disk_gb", "os_variant"};
    for (gsize i = 0; i < G_N_ELEMENTS(required); i++) {
        if (!json_object_has_member(params, required[i])) {
            gchar *msg = g_strdup_printf("Missing required parameter: %s",
                                         required[i]);
            gchar *resp = pure_rpc_build_error_response(
                rpc_id, PURE_RPC_ERR_INVALID_PARAMS, msg);
            pure_uds_server_send_response(server, conn, resp);
            g_free(resp);
            g_free(msg);
            return;
        }
    }

    const gchar *name = json_object_get_string_member(params, "name");
    if (!name || name[0] == '\0' || !pcv_validate_vm_name(name)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Parameter 'name' must be a valid identifier (alphanumeric, -, _)");
        pure_uds_server_send_response(server, conn, resp);
        g_free(resp);
        return;
    }

    /*
     * 스택에 PcvVmTemplate 구조체를 구성합니다 (힙 할당 불필요, g_new/g_free 불필요).
     *
     * 중요 설계 포인트:
     *   - 문자열 포인터(.name, .os_variant 등)는 JSON params 객체가 소유합니다.
     *   - pcv_vm_template_create() 내부에서 필요한 문자열을 g_strdup()으로 복사하므로
     *     이 함수가 반환된 후 params가 해제되어도 안전합니다.
     *   - (gchar *) 캐스트: json_object_get_string_member()가 const gchar*를 반환하지만,
     *     PcvVmTemplate 의 필드가 gchar* 이므로 const를 제거합니다.
     *     실제 수정은 발생하지 않으므로 안전한 캐스트입니다.
     */
    PcvVmTemplate tmpl = {
        .name       = (gchar *)name,
        .vcpu       = (gint)json_object_get_int_member(params, "vcpu"),
        .memory_mb  = (gint)json_object_get_int_member(params, "memory_mb"),
        .disk_gb    = (gint)json_object_get_int_member(params, "disk_gb"),
        .os_variant = (gchar *)json_object_get_string_member(params, "os_variant"),
        .iso_path           = NULL,
        .network_bridge     = NULL,
        .cloud_init_user_data = NULL,
        .description        = NULL,
    };

    /*
     * 선택적(optional) 파라미터 처리.
     * has_member() 확인 후 접근하는 이유: 존재하지 않는 키에 get_string_member를 호출하면
     * GLib이 경고 로그를 출력하고 NULL을 반환합니다. 동작에는 문제없지만
     * 불필요한 로그 노이즈를 줄이기 위해 존재 여부를 먼저 확인합니다.
     */
    if (json_object_has_member(params, "iso_path"))
        tmpl.iso_path = (gchar *)json_object_get_string_member(params, "iso_path");
    if (json_object_has_member(params, "network_bridge"))
        tmpl.network_bridge = (gchar *)json_object_get_string_member(params, "network_bridge");
    if (json_object_has_member(params, "cloud_init_user_data"))
        tmpl.cloud_init_user_data = (gchar *)json_object_get_string_member(params, "cloud_init_user_data");
    if (json_object_has_member(params, "description"))
        tmpl.description = (gchar *)json_object_get_string_member(params, "description");

    /*
     * 값 범위 검증 — 비현실적인 리소스 요청 방지.
     *
     * 범위 상한 근거:
     *   - vcpu 128     : 현존 최대 서버 소켓 수 기준 (실무에서 64코어 이상 드뭄)
     *   - memory_mb 1048576 : 1TB (= 1024*1024 MB). 엔터프라이즈 서버 최대 RAM
     *   - disk_gb 65536     : 64TB. 단일 ZFS zvol 실질 상한
     *
     * 이 검증이 없으면:
     *   - vcpu=0 → libvirt 도메인 생성 시 에러
     *   - memory_mb=-1 → integer overflow 위험
     *   - disk_gb=999999 → ZFS zvol 생성 시 디스크 부족으로 장시간 블록
     */
    if (tmpl.vcpu < 1 || tmpl.vcpu > 128) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "vcpu must be between 1 and 128");
        pure_uds_server_send_response(server, conn, resp);
        g_free(resp);
        return;
    }
    if (tmpl.memory_mb < 128 || tmpl.memory_mb > 1048576) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "memory_mb must be between 128 and 1048576");
        pure_uds_server_send_response(server, conn, resp);
        g_free(resp);
        return;
    }
    if (tmpl.disk_gb < 1 || tmpl.disk_gb > 65536) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "disk_gb must be between 1 and 65536");
        pure_uds_server_send_response(server, conn, resp);
        g_free(resp);
        return;
    }

    GError *err = NULL;
    if (!pcv_vm_template_create(&tmpl, &err)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
            err ? err->message : "Template creation failed");
        pure_uds_server_send_response(server, conn, resp);
        g_free(resp);
        if (err) g_error_free(err);
        return;
    }

    /* 성공 응답: 생성된 템플릿 반환 */
    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "created");
    json_object_set_string_member(res, "name", name);

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);

    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, conn, resp);
    g_free(resp);
}

/* ══════════════════════════════════════════════════════════════════════
 * template.delete
 * ══════════════════════════════════════════════════════════════════════*/

/**
 * handle_template_delete:
 * @params: JSON-RPC params — {"name"}
 * @rpc_id: 요청 ID
 * @server: UDS 서버 인스턴스
 * @conn: 클라이언트 소켓 연결
 *
 * 필수 파라미터: name (비어 있으면 -32602 에러)
 * 존재하지 않는 템플릿 삭제 시: pcv_vm_template_delete 가 GError 반환 → -32000 에러
 * 성공 시 반환: {"status":"deleted", "name":"..."}
 */
void
handle_template_delete(JsonObject *params, const gchar *rpc_id,
                       UdsServer *server, GSocketConnection *conn)
{
    if (!json_object_has_member(params, "name")) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required parameter: name");
        pure_uds_server_send_response(server, conn, resp);
        g_free(resp);
        return;
    }

    const gchar *name = json_object_get_string_member(params, "name");
    if (!name || name[0] == '\0' || !pcv_validate_vm_name(name)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Parameter 'name' must be a valid identifier (alphanumeric, -, _)");
        pure_uds_server_send_response(server, conn, resp);
        g_free(resp);
        return;
    }

    GError *err = NULL;
    if (!pcv_vm_template_delete(name, &err)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
            err ? err->message : "Template deletion failed");
        pure_uds_server_send_response(server, conn, resp);
        g_free(resp);
        if (err) g_error_free(err);
        return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "deleted");
    json_object_set_string_member(res, "name", name);

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);

    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, conn, resp);
    g_free(resp);
}
