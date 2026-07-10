/**
 * @file nfv_manager.c
 * @brief NFV 매니저 — OVN 기반 가상 LB, FW 정책, 서비스 체인
 *
 * ====================================================================
 * [아키텍처 위치]
 *   handler_overlay.c --> nfv_manager (이 파일)
 *                         ovn_manager (OVN 가용성 확인)
 *
 *   NFV(Network Function Virtualization)의 3대 기능을 제공한다:
 *     1) 로드 밸런서 (LB): OVN의 ovn-nbctl lb-add/lb-del 래퍼
 *     2) 방화벽 정책 (FW): 이름 기반 ACL 정책 세트 (JSON 파일 영속화)
 *     3) 서비스 체인 (Chain): 패킷 경로를 정의하는 단계별 체인 (JSON 파일 영속화)
 *
 * [LB 동작 원리]
 *   OVN의 네이티브 로드 밸런서를 사용한다.
 *   ovn-nbctl lb-add <name> <vip>:<port> <backends>
 *   예: lb-add web-lb 10.0.0.100:80 10.0.0.10:80,10.0.0.11:80
 *   OVN이 Conntrack 기반으로 VIP 트래픽을 백엔드에 분산한다.
 *
 * [FW 정책 구조]
 *   JSON 파일: /var/run/purecvisor/nfv-policy-<name>.json
 *   구조: {"name":"...", "switch":"...", "rules":[]}
 *   실제 ACL 적용은 ovn_manager의 pcv_ovn_acl_add()를 통해 별도 수행.
 *   이 모듈은 정책 메타데이터의 CRUD만 담당한다.
 *
 * [서비스 체인 구조]
 *   JSON 파일: /var/run/purecvisor/nfv-chain-<name>.json
 *   구조: {"name":"...", "steps":[...]}
 *   steps 배열에 패킷이 거쳐야 할 VNF(Virtual Network Function)의
 *   순서와 설정을 정의한다.
 *
 * [Graceful Degradation]
 *   LB 기능은 pcv_ovn_is_available() 확인 후 동작한다.
 *   OVN 미설치 시: lb_create → GError 반환, lb_list → 빈 배열.
 *   FW/Chain은 JSON 파일 기반이므로 OVN 없이도 CRUD 가능.
 *
 * [의존 모듈]
 *   ovn_manager.h  - OVN 가용성 확인 (pcv_ovn_is_available)
 *   pcv_spawn.h    - ovn-nbctl 명령 실행
 *   pcv_log.h      - NFV_LOG_DOM 도메인 로깅
 *
 * [주의사항]
 *   - LB 삭제(lb-del)는 멱등: 2>/dev/null + "; true"로 에러 무시.
 *   - FW 정책/서비스 체인 JSON 파일은 /var/run/purecvisor/ 에 저장되므로
 *     시스템 재부팅 시 휘발된다. 영구 보존이 필요하면 /var/lib/ 이동 고려.
 *   - lb_create의 backends 형식: "ip:port,ip:port,..." (쉼표 구분).
 * ====================================================================
 */
#include "nfv_manager.h"
#include "ovn_manager.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_log.h"
#include "utils/pcv_validate.h"
#include <string.h>
#include <glib/gstdio.h>

#define NFV_LOG_DOM "nfv_manager"

/**
 * _valid_id — NFV 식별자(LB 이름 등) 화이트리스트 검증
 * 허용 문자: [a-zA-Z0-9_.:-]. 비어 있거나 선행 '-'(옵션 인젝션)면 거부.
 */
static gboolean
_valid_id(const gchar *s)
{
    if (!s || !*s) return FALSE;
    if (s[0] == '-') return FALSE;
    for (const gchar *p = s; *p; p++)
        if (!(g_ascii_isalnum((guchar)*p) ||
              *p == '_' || *p == '.' || *p == ':' || *p == '-'))
            return FALSE;
    return TRUE;
}

/**
 * _run_argv — argv 배열 기반 실행 (셸/재파싱 미경유, 인젝션 방지)
 * @argv: NULL 종단 인자 배열. 각 값은 하나의 argv 원소로 리터럴 전달된다.
 * @out: (nullable): 표준 출력을 받을 포인터
 * @error: (nullable): 에러 반환 포인터
 *
 * @return 성공 시 TRUE
 */
static gboolean
_run_argv(const gchar * const *argv, gchar **out, GError **error)
{
    gchar *se = NULL;
    gboolean ok = pcv_spawn_sync(argv, out, &se, error);
    if (!ok) PCV_LOG_WARN(NFV_LOG_DOM, "cmd failed: %s err=%s", argv[0], se ? se : "");
    g_free(se);
    return ok;
}
static gboolean
_run_shell(const gchar *cmd, gchar **out, GError **error)
{
    const gchar *argv[] = {"/bin/sh", "-c", cmd, NULL};
    gchar *se = NULL;
    gboolean ok = pcv_spawn_sync(argv, out, &se, error);
    if (!ok) PCV_LOG_WARN(NFV_LOG_DOM, "cmd failed: %s err=%s", cmd, se ? se : "");
    g_free(se);
    return ok;
}

/** pcv_nfv_init — NFV 매니저 초기화. 데몬 시작 시 main.c에서 호출. */
void pcv_nfv_init(void) { PCV_LOG_INFO(NFV_LOG_DOM, "NFV manager initialized"); }

/** pcv_nfv_shutdown — NFV 매니저 종료. 현재 정리할 상태 없음. */
void pcv_nfv_shutdown(void) {}

/* ══════════════════════════════════════════════════════════════
 * [LB] OVN 네이티브 로드 밸런서 관리
 *   ovn-nbctl lb-add/lb-del/lb-list 래퍼.
 *   OVN이 Conntrack 기반으로 VIP → 백엔드 트래픽을 분산한다.
 * ══════════════════════════════════════════════════════════════ */

/**
 * pcv_nfv_lb_create — OVN 로드 밸런서 생성
 * @name: LB 이름 (예: "web-lb")
 * @vip: Virtual IP 주소 (예: "10.0.0.100")
 * @port: 서비스 포트 (예: 80)
 * @backends: 백엔드 목록 (예: "10.0.0.10:80,10.0.0.11:80")
 * @error: 에러 반환 포인터
 *
 * ovn-nbctl lb-add <name> <vip>:<port> <backends> 명령을 실행한다.
 * OVN 미가용 시 GError 반환.
 *
 * @return 성공 시 TRUE
 */
gboolean pcv_nfv_lb_create(const gchar *name, const gchar *vip, gint port,
                            const gchar *backends, GError **error)
{
    if (!name || !vip || !backends) {
        g_set_error(error, g_quark_from_static_string("nfv"), 1, "name, vip, backends required");
        return FALSE;
    }
    if (!pcv_ovn_is_available()) {
        g_set_error(error, g_quark_from_static_string("nfv"), 2, "OVN not available");
        return FALSE;
    }
    /* 심층 방어: name 식별자, vip IP, port 범위 검증. backends 는 dispatcher 가
     * ip:port 요소별로 검증해 조인한 문자열이며 단일 argv 원소로 전달된다. */
    if (!_valid_id(name) || !pcv_validate_ip_literal(vip) || !pcv_validate_port(port)) {
        g_set_error(error, g_quark_from_static_string("nfv"), 3, "invalid name, vip, or port");
        return FALSE;
    }
    gchar *vip_port = g_strdup_printf("%s:%d", vip, port);
    const gchar *argv[] = {"ovn-nbctl", "lb-add", name, vip_port, backends, NULL};
    gboolean ok = _run_argv(argv, NULL, error);
    g_free(vip_port);
    if (ok) PCV_LOG_INFO(NFV_LOG_DOM, "LB '%s' created (vip=%s:%d)", name, vip, port);
    return ok;
}

/**
 * pcv_nfv_lb_delete — OVN 로드 밸런서 삭제 (멱등)
 * @name: 삭제할 LB 이름
 * @error: 에러 반환 포인터
 *
 * ovn-nbctl lb-del 실행. 2>/dev/null + "; true"로 이미 없는 LB도 성공 처리.
 *
 * @return 성공 시 TRUE
 */
gboolean pcv_nfv_lb_delete(const gchar *name, GError **error)
{
    if (!name) { g_set_error(error, g_quark_from_static_string("nfv"), 1, "name required"); return FALSE; }
    if (!pcv_ovn_is_available()) return TRUE;  /* OVN 미가용 → 멱등 성공 */
    if (!_valid_id(name)) { g_set_error(error, g_quark_from_static_string("nfv"), 3, "invalid name"); return FALSE; }
    /* 멱등 삭제: --if-exists 로 존재하지 않는 LB 도 성공 처리 (셸 미경유) */
    const gchar *argv[] = {"ovn-nbctl", "--if-exists", "lb-del", name, NULL};
    return _run_argv(argv, NULL, error);
}

/**
 * pcv_nfv_lb_list — OVN 로드 밸런서 목록 조회
 *
 * ovn-nbctl lb-list 출력을 줄 단위로 파싱하여 JsonArray로 반환.
 * OVN 미설치 시 빈 배열 반환 (graceful degradation).
 *
 * @return (transfer full): JsonObject 배열 [{entry: "..."}, ...]
 */
JsonArray *pcv_nfv_lb_list(void)
{
    JsonArray *arr = json_array_new();
    if (!pcv_ovn_is_available()) return arr;  /* OVN 미가용 시 빈 배열 */
    gchar *out = NULL;
    if (_run_shell("ovn-nbctl lb-list 2>/dev/null", &out, NULL) && out) {
        gchar **lines = g_strsplit(out, "\n", -1);
        for (gint i = 0; lines[i] && lines[i][0]; i++) {
            JsonObject *lb = json_object_new();
            json_object_set_string_member(lb, "entry", lines[i]);
            json_array_add_object_element(arr, lb);
        }
        g_strfreev(lines);
    }
    g_free(out);
    return arr;
}

/* ══════════════════════════════════════════════════════════════
 * [FW] 방화벽 정책 세트 관리
 *   JSON 파일 기반 메타데이터 CRUD.
 *   실제 ACL 적용은 ovn_manager의 pcv_ovn_acl_add()로 별도 수행.
 *   이 모듈은 정책의 "정의"만 관리한다.
 * ══════════════════════════════════════════════════════════════ */

/**
 * pcv_nfv_fw_policy_create — 방화벽 정책 세트 생성
 * @name: 정책 이름 (예: "web-fw")
 * @sw: 연결할 OVN 논리 스위치 이름
 * @error: 에러 반환 포인터
 *
 * /var/run/purecvisor/nfv-policy-<name>.json 파일을 생성한다.
 * rules 배열은 빈 상태로 초기화되며, 이후 ACL 규칙을 추가하여 사용.
 *
 * @return 성공 시 TRUE
 */
gboolean pcv_nfv_fw_policy_create(const gchar *name, const gchar *sw, GError **error)
{
    if (!name || !sw) { g_set_error(error, g_quark_from_static_string("nfv"), 1, "name and switch required"); return FALSE; }
    /* ACL 정책 세트 = 이름 기반 메타데이터 저장 (JSON 파일) */
    gchar *path = g_strdup_printf("/var/run/purecvisor/nfv-policy-%s.json", name);
    gchar *content = g_strdup_printf("{\"name\":\"%s\",\"switch\":\"%s\",\"rules\":[]}", name, sw);
    gboolean ok = g_file_set_contents(path, content, -1, error);
    g_free(content); g_free(path);
    if (ok) PCV_LOG_INFO(NFV_LOG_DOM, "FW policy '%s' created for switch '%s'", name, sw);
    return ok;
}

/**
 * pcv_nfv_fw_policy_delete — 방화벽 정책 세트 삭제 (멱등)
 * @name: 삭제할 정책 이름
 * @error: 에러 반환 포인터
 *
 * JSON 파일을 삭제한다. g_unlink()는 파일 미존재 시 에러 없이 반환.
 *
 * @return 항상 TRUE (멱등)
 */
gboolean pcv_nfv_fw_policy_delete(const gchar *name, GError **error)
{
    if (!name) { g_set_error(error, g_quark_from_static_string("nfv"), 1, "name required"); return FALSE; }
    gchar *path = g_strdup_printf("/var/run/purecvisor/nfv-policy-%s.json", name);
    g_unlink(path);  /* 파일 미존재 시 에러 없이 반환 → 멱등 */
    g_free(path);
    return TRUE;
}

/**
 * pcv_nfv_fw_policy_list — 등록된 방화벽 정책 목록 조회
 * @sw: (미사용) 향후 스위치별 필터링용으로 예약
 *
 * /var/run/purecvisor/ 디렉토리에서 "nfv-policy-*.json" 패턴의
 * 파일을 스캔하여 JSON 파싱 후 배열로 반환한다.
 *
 * @return (transfer full): 정책 JSON 객체 배열
 */
JsonArray *pcv_nfv_fw_policy_list(const gchar *sw __attribute__((unused)))
{
    JsonArray *arr = json_array_new();
    GDir *dir = g_dir_open("/var/run/purecvisor", 0, NULL);
    if (!dir) return arr;
    const gchar *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        if (g_str_has_prefix(name, "nfv-policy-") && g_str_has_suffix(name, ".json")) {
            gchar *path = g_build_filename("/var/run/purecvisor", name, NULL);
            gchar *content = NULL;
            if (g_file_get_contents(path, &content, NULL, NULL) && content) {
                JsonParser *p = json_parser_new();
                if (json_parser_load_from_data(p, content, -1, NULL))
                    json_array_add_element(arr, json_node_copy(json_parser_get_root(p)));
                g_object_unref(p);
            }
            g_free(content); g_free(path);
        }
    }
    g_dir_close(dir);
    return arr;
}

/* ══════════════════════════════════════════════════════════════
 * [Chain] 서비스 체인 관리
 *   패킷이 거쳐야 할 VNF(Virtual Network Function) 순서를 정의.
 *   JSON 파일 기반 영속화. OVN 없이도 CRUD 가능.
 *
 *   예: FW → IDS → LB 순서로 패킷을 처리하는 체인
 *     steps: [{"vnf":"fw","port":"vm-fw"}, {"vnf":"lb","port":"vm-lb"}]
 * ══════════════════════════════════════════════════════════════ */

/**
 * pcv_nfv_chain_create — 서비스 체인 생성
 * @name: 체인 이름 (예: "web-chain")
 * @steps_json: (nullable): VNF 단계 JSON 배열 문자열 (NULL이면 빈 배열)
 * @error: 에러 반환 포인터
 *
 * /var/run/purecvisor/nfv-chain-<name>.json 파일을 생성한다.
 *
 * @return 성공 시 TRUE
 */
gboolean pcv_nfv_chain_create(const gchar *name, const gchar *steps_json, GError **error)
{
    if (!name) { g_set_error(error, g_quark_from_static_string("nfv"), 1, "name required"); return FALSE; }
    gchar *path = g_strdup_printf("/var/run/purecvisor/nfv-chain-%s.json", name);
    gchar *content = g_strdup_printf("{\"name\":\"%s\",\"steps\":%s}", name, steps_json ? steps_json : "[]");
    gboolean ok = g_file_set_contents(path, content, -1, error);
    g_free(content); g_free(path);
    if (ok) PCV_LOG_INFO(NFV_LOG_DOM, "Service chain '%s' created", name);
    return ok;
}

/**
 * pcv_nfv_chain_delete — 서비스 체인 삭제 (멱등)
 * @name: 삭제할 체인 이름
 * @error: 에러 반환 포인터
 *
 * @return 항상 TRUE (멱등)
 */
gboolean pcv_nfv_chain_delete(const gchar *name, GError **error)
{
    if (!name) { g_set_error(error, g_quark_from_static_string("nfv"), 1, "name required"); return FALSE; }
    gchar *path = g_strdup_printf("/var/run/purecvisor/nfv-chain-%s.json", name);
    g_unlink(path);  /* 파일 미존재 시에도 에러 없음 → 멱등 */
    g_free(path);
    return TRUE;
}

/**
 * pcv_nfv_chain_list — 등록된 서비스 체인 목록 조회
 *
 * /var/run/purecvisor/ 디렉토리에서 "nfv-chain-*.json" 패턴의
 * 파일을 스캔하여 JSON 파싱 후 배열로 반환한다.
 *
 * @return (transfer full): 체인 JSON 객체 배열
 */
JsonArray *pcv_nfv_chain_list(void)
{
    JsonArray *arr = json_array_new();
    GDir *dir = g_dir_open("/var/run/purecvisor", 0, NULL);
    if (!dir) return arr;
    const gchar *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        if (g_str_has_prefix(name, "nfv-chain-") && g_str_has_suffix(name, ".json")) {
            gchar *path = g_build_filename("/var/run/purecvisor", name, NULL);
            gchar *content = NULL;
            if (g_file_get_contents(path, &content, NULL, NULL) && content) {
                JsonParser *p = json_parser_new();
                if (json_parser_load_from_data(p, content, -1, NULL))
                    json_array_add_element(arr, json_node_copy(json_parser_get_root(p)));
                g_object_unref(p);
            }
            g_free(content); g_free(path);
        }
    }
    g_dir_close(dir);
    return arr;
}
