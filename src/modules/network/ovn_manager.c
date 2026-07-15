/**
 * @file ovn_manager.c
 * @brief OVN SDN 코어 -- 로컬 논리 네트워크 제어
 *
 * ====================================================================
 * [아키텍처 위치]
 *   handler_overlay.c --> ovn_manager (이 파일)
 *   main.c (부팅)     --> pcv_ovn_init
 *   vm_manager.c      --> pcv_ovn_vm_port_setup / cleanup
 *
 *   handler_overlay.c 에서 ovn.* RPC 16개를 처리할 때 이 모듈의
 *   함수를 호출한다. OVN(Open Virtual Network)의 Northbound DB를
 *   ovn-nbctl CLI 래퍼로 제어하는 SDN 컨트롤 플레인이다.
 *
 * [담당 RPC 메서드] (16개, handler_overlay.c 경유)
 *   ovn.switch.create  - 논리 스위치 생성 (ovn-nbctl ls-add)
 *   ovn.switch.delete  - 논리 스위치 삭제 (멱등: 없으면 성공)
 *   ovn.switch.list    - 논리 스위치 목록
 *   ovn.port.add       - 논리 스위치 포트 추가 (MAC/IP 바인딩)
 *   ovn.port.remove    - 논리 스위치 포트 제거
 *   ovn.acl.add        - ACL 보안 규칙 추가 (방향/우선순위/매치/액션)
 *   ovn.acl.list       - ACL 규칙 목록 조회
 *   ovn.router.create  - 논리 라우터 생성 (ovn-nbctl lr-add)
 *   ovn.router.delete  - 논리 라우터 삭제
 *   ovn.router.list    - 논리 라우터 목록
 *   ovn.router.add_port - 라우터-스위치 연결 (lrp-add + lsp peer 설정)
 *   ovn.dhcp.enable    - 분산 DHCP 옵션 생성 (dhcp-options-create)
 *   ovn.nat.add        - NAT 규칙 추가 (snat/dnat/dnat_and_snat)
 *   ovn.nat.list       - NAT 규칙 목록
 *   ovn.tenant.create  - 멀티테넌트 일괄 생성 (LS + ACL + DHCP)
 *   ovn.status         - OVN 클러스터 상태 조회
 *
 * [Phase 구현 이력]
 *   Phase 1: 논리 스위치 (ls-add, lsp-add, lsp-del) + 포트 관리
 *   Phase 2: ACL 보안 그룹 + 분산 DHCP + 멀티테넌트 격리
 *   Phase 3: 논리 라우터 (lr-add, lrp-add) + NAT + 라우터-스위치 연결
 *
 * [Graceful Degradation]
 *   pcv_ovn_init() 에서 ovn-nbctl 존재 여부를 확인한다.
 *   미설치 시 g_ovn_available=FALSE 로 설정되며:
 *     - list 계열 함수: 빈 JsonArray 반환
 *     - create/delete 계열: 에러 반환 또는 멱등 성공
 *     - status: available=false 포함 JSON 반환
 *   테스트(test_ovn.c) 에서 이 동작을 검증한다.
 *
 * [싱글/멀티 경계]
 *   이 파일은 에디션 공용 OVN 코어만 담당한다.
 *   - Single Edge: 상태 조회 + switch/router/ACL/NAT/DHCP/tenant/vm_port
 *   - Cluster build : 위 공용 코어 + 별도 파일의 encap/auto_provision 자동화
 *
 * [멀티테넌트 격리]
 *   pcv_ovn_tenant_create(tenant, subnet) 호출 시:
 *     1. 테넌트 전용 논리 스위치 생성 (이름: <tenant>-ls)
 *     2. 기본 ACL 규칙 추가 (같은 테넌트 내 트래픽만 허용)
 *     3. 분산 DHCP 활성화
 *
 * [VM 포트 라이프사이클]
 *   vm.create 시 OVN 스위치가 설정되어 있으면:
 *     pcv_ovn_vm_port_setup() -> OVN 포트 생성 + iface-id 반환
 *   vm.delete 시:
 *     pcv_ovn_vm_port_cleanup() -> OVN 포트 정리
 *
 * [내부 헬퍼]
 *   _run_argv(argv, out, error): pcv_spawn_sync 직접 호출 (셸/재파싱 미경유).
 *     각 사용자 값은 하나의 argv 원소로 ovn-nbctl 에 리터럴 전달된다.
 *   _valid_ovn_id(s): 식별자 화이트리스트([a-zA-Z0-9_.:-], 선행 '-' 거부).
 *
 * [의존 모듈]
 *   pcv_spawn.h  - ovn-nbctl, ovs-vsctl 명령 실행
 *   pcv_log.h    - OVN_LOG_DOM 도메인 로깅
 *   pcv_config.h - daemon.conf [ovn] 섹션 읽기
 *
 * [주의사항]
 *   - ovn-nbctl 명령은 OVN Northbound DB 연결이 필요하다.
 *     ovsdb-server 가 실행 중이어야 한다.
 *   - router.add_port 는 lrp-add + lsp-add + lsp-set-type=router +
 *     lsp-set-addresses=router 를 원자적으로 수행해야 한다.
 *     중간 실패 시 불완전 상태가 될 수 있으므로 에러 로그 확인 필요.
 *   - NAT type 은 "snat", "dnat", "dnat_and_snat" 중 하나.
 * ====================================================================
 */
#include "ovn_manager.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_log.h"
#include "utils/pcv_validate.h"
#include "../../utils/pcv_config.h"
#include <string.h>

#define OVN_LOG_DOM "ovn_mgr"

static gboolean g_ovn_available = FALSE;

/* ── helpers ──────────────────────────────────────────────────── */

/**
 * _valid_ovn_id — OVN 식별자(스위치/라우터/포트/테넌트 이름) 화이트리스트 검증
 * @s: 검사할 식별자
 *
 * 허용 문자: [a-zA-Z0-9_.:-]. 비어 있으면 거부하고, 선행 '-'는
 * ovn-nbctl 옵션 인젝션을 막기 위해 거부한다.
 * argv 배열 실행과 함께 심층 방어(defense-in-depth)로 사용한다.
 *
 * Returns: 유효하면 TRUE
 */
static gboolean
_valid_ovn_id(const gchar *s)
{
    if (!s || !*s) return FALSE;
    if (s[0] == '-') return FALSE;   /* 선행 '-' → 옵션 인젝션 방지 */
    for (const gchar *p = s; *p; p++) {
        if (!(g_ascii_isalnum((guchar)*p) ||
              *p == '_' || *p == '.' || *p == ':' || *p == '-'))
            return FALSE;
    }
    return TRUE;
}

/** pcv_ovn_valid_id — _valid_ovn_id 의 테스트용 공개 래퍼 (test_ovn.c). */
gboolean pcv_ovn_valid_id(const gchar *s) { return _valid_ovn_id(s); }

/**
 * _run_argv — argv 배열 기반 ovn-nbctl 실행 (셸/재파싱 미경유)
 * @argv: NULL 종단 인자 배열. 각 사용자 값은 정확히 하나의 argv 원소이며
 *        ovn-nbctl 이 리터럴로 취급한다 (재토큰화 없음 → 인젝션 불가).
 * @out: (nullable): 표준 출력을 받을 포인터
 * @error: (nullable): 에러 반환 포인터
 *
 * pcv_spawn_sync()를 직접 호출한다. 이전 _run()/g_shell_parse_argv() 방식은
 * 명령 문자열을 셸 규칙으로 재토큰화하여 공백/따옴표/'--' 가 추가 argv 로
 * 분할될 수 있었다 — 이 함수는 그 재파싱 단계를 제거한다.
 *
 * Returns: 성공 시 TRUE
 */
static gboolean
_run_argv(const gchar * const *argv, gchar **out, GError **error)
{
    gchar *std_err = NULL;
    gboolean ok = pcv_spawn_sync(argv, out, &std_err, error);
    if (!ok && std_err && *std_err)
        PCV_LOG_WARN(OVN_LOG_DOM, "ovn-nbctl failed: %s → %s", argv[0], std_err);
    g_free(std_err);
    return ok;
}

/* ── lifecycle ────────────────────────────────────────────────── */

/**
 * pcv_ovn_init — OVN 매니저 초기화
 *
 * ovn-nbctl --version 명령으로 OVN 설치 여부를 확인.
 * 미설치 시 g_ovn_available=FALSE로 graceful degradation 모드 진입.
 * 데몬 시작 시 main.c에서 호출.
 */
void pcv_ovn_init(void)
{
    /* argv 직접 실행 — 셸 미경유. ovn-nbctl 미설치 시 조용히 비가용 처리. */
    const gchar *argv[] = {"ovn-nbctl", "--version", NULL};
    gchar *out = NULL, *errout = NULL;
    g_ovn_available = pcv_spawn_sync(argv, &out, &errout, NULL);
    g_free(out);
    g_free(errout);
    if (g_ovn_available)
        PCV_LOG_INFO(OVN_LOG_DOM, "OVN available");
    else
        PCV_LOG_INFO(OVN_LOG_DOM, "OVN not installed — OVN features disabled");
}

/** pcv_ovn_shutdown — OVN 매니저 종료. g_ovn_available 플래그를 FALSE로 리셋. */
void pcv_ovn_shutdown(void)
{
    if (g_ovn_available)
        PCV_LOG_INFO(OVN_LOG_DOM, "OVN manager shutdown");
    g_ovn_available = FALSE;
}

/** pcv_ovn_is_available — OVN 가용 여부 반환 (ovn-nbctl 존재 여부 기반) */
gboolean pcv_ovn_is_available(void) { return g_ovn_available; }

/* ── Phase 1: 논리 스위치 ─────────────────────────────────────── */

/**
 * pcv_ovn_switch_create — OVN 논리 스위치 생성 (멱등)
 * @name: 스위치 이름 (예: "pcv-ls0")
 * @subnet: 서브넷 CIDR (로깅용, OVN 자체에는 전달되지 않음)
 * @error: 에러 반환 포인터
 *
 * ovn-nbctl --may-exist ls-add 실행. OVN 미가용 시 GError 반환.
 *
 * @return 성공 시 TRUE
 */
gboolean
pcv_ovn_switch_create(const gchar *name, const gchar *subnet, GError **error)
{
    if (!g_ovn_available) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "OVN not available");
        return FALSE;
    }
    if (!_valid_ovn_id(name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid switch name");
        return FALSE;
    }
    const gchar *argv[] = {"ovn-nbctl", "--may-exist", "ls-add", name, NULL};
    gboolean ok = _run_argv(argv, NULL, error);
    if (ok)
        PCV_LOG_INFO(OVN_LOG_DOM, "Logical switch '%s' created (subnet=%s)", name, subnet ? subnet : "-");
    return ok;
}

/**
 * pcv_ovn_switch_delete — OVN 논리 스위치 삭제 (멱등)
 * @name: 삭제할 스위치 이름
 * @error: 에러 반환 포인터
 *
 * --if-exists 플래그로 미존재 스위치도 에러 없이 성공 처리.
 * OVN 미가용 시 TRUE 반환 (graceful degradation).
 *
 * @return 성공 시 TRUE
 */
gboolean
pcv_ovn_switch_delete(const gchar *name, GError **error)
{
    if (!g_ovn_available) return TRUE;
    if (!_valid_ovn_id(name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid switch name");
        return FALSE;
    }
    const gchar *argv[] = {"ovn-nbctl", "--if-exists", "ls-del", name, NULL};
    return _run_argv(argv, NULL, error);
}

/**
 * pcv_ovn_switch_list — 등록된 OVN 논리 스위치 목록 조회
 *
 * ovn-nbctl ls-list 출력에서 "UUID (name)" 형식을 파싱하여
 * 이름만 추출하여 JsonArray로 반환.
 * OVN 미가용 시 빈 배열 반환.
 *
 * @return (transfer full): [{name: "pcv-ls0"}, ...] JsonObject 배열
 */
JsonArray *
pcv_ovn_switch_list(void)
{
    JsonArray *arr = json_array_new();
    if (!g_ovn_available) return arr;

    gchar *out = NULL;
    const gchar *argv[] = {"ovn-nbctl", "ls-list", NULL};
    if (_run_argv(argv, &out, NULL) && out) {
        /* 출력 형식: "UUID (name)" — 괄호 안에서 이름 추출 */
        gchar **lines = g_strsplit(g_strstrip(out), "\n", -1);
        for (gchar **l = lines; *l; l++) {
            if (!**l) continue;
            gchar *lp = strchr(*l, '(');
            gchar *rp = lp ? strchr(lp, ')') : NULL;
            if (lp && rp) {
                gchar *name = g_strndup(lp + 1, rp - lp - 1);
                JsonObject *obj = json_object_new();
                json_object_set_string_member(obj, "name", name);
                json_array_add_object_element(arr, obj);
                g_free(name);
            }
        }
        g_strfreev(lines);
    }
    g_free(out);
    return arr;
}

/**
 * pcv_ovn_port_add:
 * @sw: 논리 스위치 이름
 * @port: 포트 이름
 * @mac: (nullable): MAC 주소 (예: "00:00:00:00:00:01")
 * @ip: (nullable): IP 주소 (예: "10.200.0.10")
 * @error: 에러 반환 포인터
 *
 * OVN 논리 스위치에 포트를 추가.
 * mac과 ip가 모두 주어지면 lsp-set-addresses로 주소도 설정한다.
 * --may-exist 플래그로 멱등 동작.
 */
gboolean
pcv_ovn_port_add(const gchar *sw, const gchar *port, const gchar *mac, const gchar *ip, GError **error)
{
    if (!g_ovn_available) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "OVN not available");
        return FALSE;
    }
    if (!_valid_ovn_id(sw) || !_valid_ovn_id(port)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid switch or port name");
        return FALSE;
    }
    const gchar *argv1[] = {"ovn-nbctl", "--may-exist", "lsp-add", sw, port, NULL};
    gboolean ok = _run_argv(argv1, NULL, error);
    if (!ok) return FALSE;

    if (mac && ip) {
        if (!pcv_validate_mac(mac) || !pcv_validate_ip_literal(ip)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid mac or ip");
            return FALSE;
        }
        /* "mac ip" 는 lsp-set-addresses 의 단일 인자 (하나의 argv 원소) */
        gchar *addr = g_strdup_printf("%s %s", mac, ip);
        const gchar *argv2[] = {"ovn-nbctl", "lsp-set-addresses", port, addr, NULL};
        ok = _run_argv(argv2, NULL, error);
        g_free(addr);
    }
    return ok;
}

/**
 * pcv_ovn_port_remove — OVN 논리 스위치 포트 제거 (멱등)
 * @sw: (unused) 스위치 이름 — OVN은 포트 이름만으로 식별
 * @port: 제거할 포트 이름
 * @error: 에러 반환 포인터
 *
 * --if-exists lsp-del 로 멱등 삭제. OVN 미가용 시 TRUE 반환.
 *
 * @return 성공 시 TRUE
 */
gboolean
pcv_ovn_port_remove(const gchar *sw, const gchar *port, GError **error)
{
    (void)sw;
    if (!g_ovn_available) return TRUE;
    if (!_valid_ovn_id(port)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid port name");
        return FALSE;
    }
    const gchar *argv[] = {"ovn-nbctl", "--if-exists", "lsp-del", port, NULL};
    return _run_argv(argv, NULL, error);
}

/* ── Phase 2: ACL + DHCP ─────────────────────────────────────── */

/**
 * pcv_ovn_acl_add — OVN ACL(접근 제어 목록) 규칙 추가
 * @sw: 대상 논리 스위치 이름
 * @direction: 방향 ("to-lport" 또는 "from-lport")
 * @priority: 우선순위 (0~32767, 높을수록 먼저 평가)
 * @match: 매칭 조건 (OVN match 표현식, 예: "inport == @web && ip")
 * @action: 액션 ("allow", "allow-related", "drop", "reject")
 * @error: 에러 반환 포인터
 *
 * ovn-nbctl acl-add 실행. OVN 미가용 시 GError 반환.
 *
 * @return 성공 시 TRUE
 */
gboolean
pcv_ovn_acl_add(const gchar *sw, const gchar *direction, gint priority,
                 const gchar *match, const gchar *action, GError **error)
{
    if (!g_ovn_available) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "OVN not available");
        return FALSE;
    }
    /* sw 는 식별자, direction/action 은 화이트리스트. match 는 단일 argv 원소이므로
     * 공백/특수문자를 포함한 OVN match 표현식이 그대로 리터럴 전달된다 (안전). */
    if (!_valid_ovn_id(sw) || !match ||
        !(g_strcmp0(direction, "to-lport") == 0 || g_strcmp0(direction, "from-lport") == 0)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid switch/direction/match");
        return FALSE;
    }
    if (!(g_strcmp0(action, "allow") == 0 || g_strcmp0(action, "allow-related") == 0 ||
          g_strcmp0(action, "drop") == 0 || g_strcmp0(action, "reject") == 0)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid action");
        return FALSE;
    }
    gchar *pri = g_strdup_printf("%d", priority);
    const gchar *argv[] = {"ovn-nbctl", "acl-add", sw, direction, pri, match, action, NULL};
    gboolean ok = _run_argv(argv, NULL, error);
    g_free(pri);
    return ok;
}

/**
 * pcv_ovn_acl_delete — OVN ACL 규칙 삭제 (멱등)
 * @sw: 대상 논리 스위치 이름
 * @direction: 방향
 * @priority: 우선순위
 * @match: 매칭 조건
 * @error: 에러 반환 포인터
 *
 * OVN 미가용 시 TRUE 반환 (graceful degradation).
 *
 * @return 성공 시 TRUE
 */
gboolean
pcv_ovn_acl_delete(const gchar *sw, const gchar *direction, gint priority,
                    const gchar *match, GError **error)
{
    if (!g_ovn_available) return TRUE;
    if (!_valid_ovn_id(sw) || !match ||
        !(g_strcmp0(direction, "to-lport") == 0 || g_strcmp0(direction, "from-lport") == 0)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid switch/direction/match");
        return FALSE;
    }
    gchar *pri = g_strdup_printf("%d", priority);
    const gchar *argv[] = {"ovn-nbctl", "acl-del", sw, direction, pri, match, NULL};
    gboolean ok = _run_argv(argv, NULL, error);
    g_free(pri);
    return ok;
}

/**
 * pcv_ovn_acl_list — OVN 논리 스위치의 ACL 규칙 목록 조회
 * @sw: 대상 논리 스위치 이름
 *
 * ovn-nbctl acl-list 출력을 줄 단위로 파싱하여 문자열 배열로 반환.
 * OVN 미가용 또는 sw=NULL 시 빈 배열 반환.
 *
 * @return (transfer full): 문자열 JsonArray ["to-lport 1000 ... allow", ...]
 */
JsonArray *
pcv_ovn_acl_list(const gchar *sw)
{
    JsonArray *arr = json_array_new();
    if (!g_ovn_available || !sw) return arr;
    if (!_valid_ovn_id(sw)) return arr;
    const gchar *argv[] = {"ovn-nbctl", "acl-list", sw, NULL};
    gchar *out = NULL;
    if (_run_argv(argv, &out, NULL) && out) {
        gchar **lines = g_strsplit(g_strstrip(out), "\n", -1);
        for (gchar **l = lines; *l; l++) {
            if (!**l) continue;
            json_array_add_string_element(arr, *l);
        }
        g_strfreev(lines);
    }
    g_free(out);
    return arr;
}

/**
 * pcv_ovn_dhcp_enable — OVN 분산 DHCP 옵션 생성
 * @subnet: 서브넷 CIDR (예: "10.200.0.0/24")
 * @gw: 게이트웨이 IP (예: "10.200.0.1")
 * @error: 에러 반환 포인터
 *
 * 실행 순서:
 *   1) ovn-nbctl dhcp-options-create <subnet> → UUID 반환
 *   2) dhcp-options-set-options <uuid> lease_time=3600 router=<gw> server_id=<gw>
 *
 * OVN의 분산 DHCP는 각 hypervisor의 ovn-controller가 로컬에서 DHCP 응답을
 * 생성하므로 별도 dnsmasq 프로세스가 불필요하다.
 *
 * @return 성공 시 TRUE
 */
gboolean
pcv_ovn_dhcp_enable(const gchar *subnet, const gchar *gw, GError **error)
{
    if (!g_ovn_available) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "OVN not available");
        return FALSE;
    }
    if (!pcv_validate_cidr(subnet) || !pcv_validate_ip_literal(gw)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid subnet or gateway");
        return FALSE;
    }
    const gchar *argv[] = {"ovn-nbctl", "dhcp-options-create", subnet, NULL};
    gchar *out = NULL;
    gboolean ok = _run_argv(argv, &out, error);
    if (ok && out && *out) {
        gchar *uuid = g_strstrip(out);
        gchar *router_opt   = g_strdup_printf("router=%s", gw);
        gchar *serverid_opt = g_strdup_printf("server_id=%s", gw);
        const gchar *argv2[] = {"ovn-nbctl", "dhcp-options-set-options", uuid,
                                "lease_time=3600", router_opt, serverid_opt,
                                "server_mac=00:00:00:00:00:01", NULL};
        _run_argv(argv2, NULL, NULL);
        g_free(router_opt);
        g_free(serverid_opt);
    }
    g_free(out);
    return ok;
}

/* ── Phase 3: 논리 라우터 ─────────────────────────────────────── */

/**
 * pcv_ovn_router_create — OVN 논리 라우터 생성 (멱등)
 * @name: 라우터 이름 (예: "pcv-lr0")
 * @error: 에러 반환 포인터
 *
 * ovn-nbctl --may-exist lr-add 실행. OVN 미가용 시 GError 반환.
 * OVN 논리 라우터는 스위치 간 L3 라우팅 + NAT를 담당.
 *
 * @return 성공 시 TRUE
 */
gboolean
pcv_ovn_router_create(const gchar *name, GError **error)
{
    if (!g_ovn_available) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "OVN not available");
        return FALSE;
    }
    if (!_valid_ovn_id(name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid router name");
        return FALSE;
    }
    const gchar *argv[] = {"ovn-nbctl", "--may-exist", "lr-add", name, NULL};
    return _run_argv(argv, NULL, error);
}

/**
 * pcv_ovn_router_delete — OVN 논리 라우터 삭제 (멱등)
 * @name: 삭제할 라우터 이름
 * @error: 에러 반환 포인터
 *
 * --if-exists lr-del 로 멱등 삭제. OVN 미가용 시 TRUE 반환.
 *
 * @return 성공 시 TRUE
 */
gboolean
pcv_ovn_router_delete(const gchar *name, GError **error)
{
    if (!g_ovn_available) return TRUE;
    if (!_valid_ovn_id(name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid router name");
        return FALSE;
    }
    const gchar *argv[] = {"ovn-nbctl", "--if-exists", "lr-del", name, NULL};
    return _run_argv(argv, NULL, error);
}

/**
 * pcv_ovn_router_add_port:
 * @router: 논리 라우터 이름
 * @sw: 연결할 논리 스위치 이름
 * @mac: 라우터 포트 MAC 주소
 * @cidr: 라우터 포트 네트워크 CIDR (예: "10.200.0.1/24")
 * @error: 에러 반환 포인터
 *
 * 논리 라우터와 스위치를 연결하는 포트 쌍을 생성.
 *
 * 생성 단계 (5개 ovn-nbctl 명령):
 *   1) lrp-add: 라우터 측 포트 "rtr-<sw>" 생성 (MAC + CIDR)
 *   2) lsp-add: 스위치 측 포트 "lnk-<sw>" 생성
 *   3) lsp-set-type router: 스위치 포트 타입을 'router'로 설정
 *   4) lsp-set-addresses router: 주소를 'router'로 설정
 *   5) lsp-set-options: router-port 옵션으로 라우터 포트 연결
 *
 * --may-exist 플래그로 멱등 동작.
 */
gboolean
pcv_ovn_router_add_port(const gchar *router, const gchar *sw,
                         const gchar *mac, const gchar *cidr, GError **error)
{
    if (!g_ovn_available) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "OVN not available");
        return FALSE;
    }
    if (!_valid_ovn_id(router) || !_valid_ovn_id(sw) ||
        !pcv_validate_mac(mac) || !pcv_validate_cidr(cidr)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid router/switch/mac/cidr");
        return FALSE;
    }
    gchar *rport = g_strdup_printf("rtr-%s", sw);
    const gchar *argv1[] = {"ovn-nbctl", "--may-exist", "lrp-add", router, rport, mac, cidr, NULL};
    gboolean ok = _run_argv(argv1, NULL, error);

    if (ok) {
        gchar *lport = g_strdup_printf("lnk-%s", sw);
        const gchar *argv2[] = {"ovn-nbctl", "--may-exist", "lsp-add", sw, lport, NULL};
        _run_argv(argv2, NULL, NULL);
        const gchar *argv3[] = {"ovn-nbctl", "lsp-set-type", lport, "router", NULL};
        _run_argv(argv3, NULL, NULL);
        const gchar *argv4[] = {"ovn-nbctl", "lsp-set-addresses", lport, "router", NULL};
        _run_argv(argv4, NULL, NULL);
        gchar *ropt = g_strdup_printf("router-port=%s", rport);
        const gchar *argv5[] = {"ovn-nbctl", "lsp-set-options", lport, ropt, NULL};
        _run_argv(argv5, NULL, NULL);
        g_free(ropt);
        g_free(lport);
    }
    g_free(rport);
    return ok;
}

/* ── router extras ────────────────────────────────────────────── */

/**
 * pcv_ovn_router_remove_port:
 * @router: 라우터 이름 (현재 미사용, 향후 검증용)
 * @port: 제거할 라우터 포트 이름
 * @error: 에러 반환 포인터
 *
 * 논리 라우터에서 포트를 제거 (ovn-nbctl lrp-del).
 * --if-exists 플래그로 멱등 동작.
 */
gboolean
pcv_ovn_router_remove_port(const gchar *router, const gchar *port, GError **error)
{
    (void)router;
    if (!g_ovn_available) return TRUE;
    if (!_valid_ovn_id(port)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid port name");
        return FALSE;
    }
    const gchar *argv[] = {"ovn-nbctl", "--if-exists", "lrp-del", port, NULL};
    return _run_argv(argv, NULL, error);
}

/**
 * pcv_ovn_router_list:
 *
 * 등록된 모든 논리 라우터 목록 조회 (ovn-nbctl lr-list).
 * 출력 형식 "UUID (name)"에서 이름만 추출하여 JsonArray로 반환.
 *
 * Returns: (transfer full): JsonObject 배열 [{name: "..."}, ...]
 */
JsonArray *
pcv_ovn_router_list(void)
{
    JsonArray *arr = json_array_new();
    if (!g_ovn_available) return arr;

    gchar *out = NULL;
    const gchar *argv[] = {"ovn-nbctl", "lr-list", NULL};
    if (_run_argv(argv, &out, NULL) && out) {
        gchar **lines = g_strsplit(g_strstrip(out), "\n", -1);
        for (gchar **l = lines; *l; l++) {
            if (!**l) continue;
            gchar *lp = strchr(*l, '(');
            gchar *rp = lp ? strchr(lp, ')') : NULL;
            if (lp && rp) {
                gchar *name = g_strndup(lp + 1, rp - lp - 1);
                JsonObject *obj = json_object_new();
                json_object_set_string_member(obj, "name", name);
                json_array_add_object_element(arr, obj);
                g_free(name);
            }
        }
        g_strfreev(lines);
    }
    g_free(out);
    return arr;
}

/* ── NAT ──────────────────────────────────────────────────────── */

/**
 * pcv_ovn_nat_add:
 * @router: 라우터 이름
 * @type: NAT 타입 ("snat", "dnat", "dnat_and_snat")
 * @external_ip: 외부 IP (물리 네트워크 측)
 * @logical_ip: 내부 IP 또는 서브넷 (논리 네트워크 측)
 * @error: 에러 반환 포인터
 *
 * 논리 라우터에 NAT 규칙 추가 (ovn-nbctl lr-nat-add).
 * SNAT: 내부→외부 소스 주소 변환 (인터넷 접근)
 * DNAT: 외부→내부 목적지 주소 변환 (포트 포워딩)
 */
gboolean
pcv_ovn_nat_add(const gchar *router, const gchar *type,
                 const gchar *external_ip, const gchar *logical_ip, GError **error)
{
    if (!g_ovn_available) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "OVN not available");
        return FALSE;
    }
    /* logical_ip 는 단일 IP 또는 서브넷(CIDR) 둘 다 허용된다. */
    if (!_valid_ovn_id(router) ||
        !(g_strcmp0(type, "snat") == 0 || g_strcmp0(type, "dnat") == 0 ||
          g_strcmp0(type, "dnat_and_snat") == 0) ||
        !pcv_validate_ip_literal(external_ip) ||
        !(pcv_validate_ip_literal(logical_ip) || pcv_validate_cidr(logical_ip))) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid router/type/ip");
        return FALSE;
    }
    const gchar *argv[] = {"ovn-nbctl", "lr-nat-add", router, type, external_ip, logical_ip, NULL};
    gboolean ok = _run_argv(argv, NULL, error);
    if (ok)
        PCV_LOG_INFO(OVN_LOG_DOM, "NAT %s added: router=%s ext=%s log=%s",
                     type, router, external_ip, logical_ip);
    return ok;
}

/**
 * pcv_ovn_nat_delete:
 * @router: 라우터 이름
 * @type: NAT 타입
 * @external_ip: 외부 IP
 * @logical_ip: (미사용) 내부 IP — OVN은 type+external_ip로 식별
 * @error: 에러 반환 포인터
 *
 * 논리 라우터에서 NAT 규칙 삭제. 멱등.
 */
gboolean
pcv_ovn_nat_delete(const gchar *router, const gchar *type,
                    const gchar *external_ip, const gchar *logical_ip, GError **error)
{
    (void)logical_ip;
    if (!g_ovn_available) return TRUE;
    if (!_valid_ovn_id(router) ||
        !(g_strcmp0(type, "snat") == 0 || g_strcmp0(type, "dnat") == 0 ||
          g_strcmp0(type, "dnat_and_snat") == 0) ||
        !pcv_validate_ip_literal(external_ip)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid router/type/ip");
        return FALSE;
    }
    const gchar *argv[] = {"ovn-nbctl", "lr-nat-del", router, type, external_ip, NULL};
    return _run_argv(argv, NULL, error);
}

/**
 * pcv_ovn_nat_list — 논리 라우터의 NAT 규칙 목록 조회
 * @router: 라우터 이름
 *
 * ovn-nbctl lr-nat-list 출력을 줄 단위 파싱하여 문자열 배열 반환.
 * OVN 미가용 또는 router=NULL 시 빈 배열 반환.
 *
 * @return (transfer full): 문자열 JsonArray
 */
JsonArray *
pcv_ovn_nat_list(const gchar *router)
{
    JsonArray *arr = json_array_new();
    if (!g_ovn_available || !router) return arr;
    if (!_valid_ovn_id(router)) return arr;

    const gchar *argv[] = {"ovn-nbctl", "lr-nat-list", router, NULL};
    gchar *out = NULL;
    if (_run_argv(argv, &out, NULL) && out) {
        gchar **lines = g_strsplit(g_strstrip(out), "\n", -1);
        for (gchar **l = lines; *l; l++) {
            if (!**l) continue;
            json_array_add_string_element(arr, *l);
        }
        g_strfreev(lines);
    }
    g_free(out);
    return arr;
}

/* ── DHCP list ────────────────────────────────────────────────── */

/**
 * pcv_ovn_dhcp_list — OVN DHCP 옵션 목록 조회
 *
 * ovn-nbctl dhcp-options-list 출력을 줄 단위로 반환.
 * OVN 미가용 시 빈 배열 반환.
 *
 * @return (transfer full): 문자열 JsonArray
 */
JsonArray *
pcv_ovn_dhcp_list(void)
{
    JsonArray *arr = json_array_new();
    if (!g_ovn_available) return arr;

    gchar *out = NULL;
    const gchar *argv[] = {"ovn-nbctl", "dhcp-options-list", NULL};
    if (_run_argv(argv, &out, NULL) && out) {
        gchar **lines = g_strsplit(g_strstrip(out), "\n", -1);
        for (gchar **l = lines; *l; l++) {
            if (!**l) continue;
            json_array_add_string_element(arr, *l);
        }
        g_strfreev(lines);
    }
    g_free(out);
    return arr;
}

/* ── Multi-tenant ─────────────────────────────────────────────── */

/**
 * pcv_ovn_tenant_create:
 * @tenant: 테넌트 이름 (예: "team-alpha")
 * @subnet: 테넌트 전용 서브넷 CIDR (예: "10.201.0.0/24")
 * @error: 에러 반환 포인터
 *
 * 멀티테넌트 격리 환경 일괄 생성.
 *
 * 생성 순서:
 *   1) 논리 스위치 "tenant-<name>-ls" 생성
 *   2) 기본 ACL 추가 — 내부 트래픽 allow (to-lport + from-lport, 우선순위 1000)
 *   3) DHCP 옵션 생성 — 서브넷에서 x.x.x.1 게이트웨이 자동 추출
 *
 * 테넌트 간 격리: 각 테넌트의 ACL이 자체 스위치 포트만 허용하므로
 * 다른 테넌트 트래픽은 기본 정책(drop)에 의해 차단된다.
 */
gboolean
pcv_ovn_tenant_create(const gchar *tenant, const gchar *subnet, GError **error)
{
    if (!g_ovn_available) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "OVN not available");
        return FALSE;
    }
    if (!tenant || !subnet) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "tenant and subnet are required");
        return FALSE;
    }
    if (!_valid_ovn_id(tenant) || !pcv_validate_cidr(subnet)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid tenant or subnet");
        return FALSE;
    }

    /* 1) 논리 스위치 "tenant-<name>-ls" 생성 */
    gchar *sw_name = g_strdup_printf("tenant-%s-ls", tenant);
    gboolean ok = pcv_ovn_switch_create(sw_name, subnet, error);
    if (!ok) {
        g_free(sw_name);
        return FALSE;
    }

    /* 2) 기본 ACL — 내부 allow */
    GError *acl_err = NULL;
    gchar *match_in = g_strdup_printf("inport == @%s && ip", sw_name);
    pcv_ovn_acl_add(sw_name, "to-lport", 1000, match_in, "allow", &acl_err);
    g_free(match_in);
    g_clear_error(&acl_err);

    gchar *match_out = g_strdup_printf("outport == @%s && ip", sw_name);
    pcv_ovn_acl_add(sw_name, "from-lport", 1000, match_out, "allow", &acl_err);
    g_free(match_out);
    g_clear_error(&acl_err);

    /* 3) DHCP 옵션 */
    /* subnet에서 gateway IP 추출: x.x.x.1 */
    gchar **parts = g_strsplit(subnet, "/", 2);
    if (parts[0]) {
        gchar **octets = g_strsplit(parts[0], ".", 4);
        if (octets[0] && octets[1] && octets[2]) {
            gchar *gw = g_strdup_printf("%s.%s.%s.1", octets[0], octets[1], octets[2]);
            pcv_ovn_dhcp_enable(subnet, gw, NULL);
            g_free(gw);
        }
        g_strfreev(octets);
    }
    g_strfreev(parts);

    PCV_LOG_INFO(OVN_LOG_DOM, "Tenant '%s' created: sw=%s subnet=%s", tenant, sw_name, subnet);
    g_free(sw_name);
    return TRUE;
}

/**
 * pcv_ovn_tenant_delete:
 * @tenant: 삭제할 테넌트 이름
 * @error: 에러 반환 포인터
 *
 * 테넌트 환경 삭제. "tenant-<name>-ls" 스위치를 제거한다.
 * 스위치 삭제 시 연결된 포트/ACL도 OVN에 의해 자동 정리.
 * 멱등 — 이미 삭제된 테넌트에 대해서도 TRUE 반환.
 */
gboolean
pcv_ovn_tenant_delete(const gchar *tenant, GError **error)
{
    if (!g_ovn_available) return TRUE;
    if (!tenant) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "tenant is required");
        return FALSE;
    }

    gchar *sw_name = g_strdup_printf("tenant-%s-ls", tenant);
    gboolean ok = pcv_ovn_switch_delete(sw_name, error);
    g_free(sw_name);

    if (ok)
        PCV_LOG_INFO(OVN_LOG_DOM, "Tenant '%s' deleted", tenant);
    return ok;
}

/* ── VM port lifecycle ────────────────────────────────────────── */

/**
 * pcv_ovn_vm_port_setup:
 * @sw: 연결할 논리 스위치 이름
 * @vm_name: VM 이름 (포트 이름 "vm-<name>"으로 생성)
 * @mac: (nullable): MAC 주소
 * @ip: (nullable): IP 주소
 * @iface_id_out: (nullable)(out): 생성된 포트 이름 반환 (호출자 g_free)
 * @error: 에러 반환 포인터
 *
 * VM 생성 시 OVN 논리 포트를 자동 설정.
 *
 * 설정 순서:
 *   1) "vm-<name>" 포트를 스위치에 추가 (pcv_ovn_port_add)
 *   2) port-security 설정 (MAC+IP 바인딩으로 스푸핑 방지)
 *   3) iface_id를 반환 (OVS 인테그레이션 브릿지에서 참조)
 *
 * 호출 시점: vm.create 핸들러에서 OVN 활성화 시.
 */
gboolean
pcv_ovn_vm_port_setup(const gchar *sw, const gchar *vm_name,
                       const gchar *mac, const gchar *ip,
                       gchar **iface_id_out, GError **error)
{
    if (!g_ovn_available) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "OVN not available");
        return FALSE;
    }
    if (!sw || !vm_name) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "sw and vm_name are required");
        return FALSE;
    }

    gchar *port = g_strdup_printf("vm-%s", vm_name);

    /* 1) 포트 추가 */
    gboolean ok = pcv_ovn_port_add(sw, port, mac, ip, error);
    if (!ok) {
        g_free(port);
        return FALSE;
    }

    /* 2) port-security 설정 ("mac ip" 는 단일 argv 원소) */
    if (mac && ip && pcv_validate_mac(mac) && pcv_validate_ip_literal(ip)) {
        gchar *secval = g_strdup_printf("%s %s", mac, ip);
        const gchar *argv[] = {"ovn-nbctl", "lsp-set-port-security", port, secval, NULL};
        _run_argv(argv, NULL, NULL);
        g_free(secval);
    }

    /* 3) iface_id 반환 */
    if (iface_id_out)
        *iface_id_out = g_strdup(port);

    PCV_LOG_INFO(OVN_LOG_DOM, "VM port setup: sw=%s port=%s mac=%s ip=%s",
                 sw, port, mac ? mac : "-", ip ? ip : "-");
    g_free(port);
    return TRUE;
}

/**
 * pcv_ovn_vm_port_cleanup:
 * @vm_name: 정리할 VM 이름
 * @error: 에러 반환 포인터
 *
 * VM 삭제 시 OVN 논리 포트 "vm-<name>"을 자동 제거.
 * 멱등 — 포트가 이미 없어도 TRUE 반환.
 * 호출 시점: vm.delete 핸들러에서 OVN 활성화 시.
 */
gboolean
pcv_ovn_vm_port_cleanup(const gchar *vm_name, GError **error)
{
    if (!g_ovn_available) return TRUE;
    if (!vm_name) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "vm_name is required");
        return FALSE;
    }

    gchar *port = g_strdup_printf("vm-%s", vm_name);
    gboolean ok = pcv_ovn_port_remove(NULL, port, error);
    g_free(port);

    if (ok)
        PCV_LOG_INFO(OVN_LOG_DOM, "VM port cleanup: vm=%s", vm_name);
    return ok;
}

/* ── detail — REST API용 ──────────────────────────────────── */

/**
 * pcv_ovn_switch_detail:
 * @name: 논리 스위치 이름
 *
 * REST API용 스위치 상세 정보 조회.
 *
 * 반환 JsonObject 구조:
 *   {
 *     "name": "pcv-ls0",
 *     "port_count": 3,
 *     "ports": ["vm-web", "vm-db", "lnk-pcv-ls0"],
 *     "acl_count": 2,
 *     "acls": ["to-lport 1000 ... allow", ...]
 *   }
 *
 * 포트 목록: ovn-nbctl lsp-list에서 UUID (name) 형식 파싱.
 * ACL 목록: pcv_ovn_acl_list() 재사용.
 *
 * Returns: (transfer full): 호출자가 json_object_unref()로 해제
 */
JsonObject *
pcv_ovn_switch_detail(const gchar *name)
{
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "name", name ? name : "");
    if (!g_ovn_available || !name) return obj;
    if (!_valid_ovn_id(name)) return obj;

    /* 포트 목록: ovn-nbctl lsp-list <switch> */
    {
        const gchar *argv[] = {"ovn-nbctl", "lsp-list", name, NULL};
        gchar *out = NULL;
        JsonArray *ports = json_array_new();
        if (_run_argv(argv, &out, NULL) && out) {
            gchar **lines = g_strsplit(g_strstrip(out), "\n", -1);
            for (gchar **l = lines; *l; l++) {
                if (!**l) continue;
                /* 형식: UUID (port-name) */
                gchar *lp = strchr(*l, '(');
                gchar *rp = lp ? strchr(lp, ')') : NULL;
                if (lp && rp) {
                    gchar *pname = g_strndup(lp + 1, rp - lp - 1);
                    json_array_add_string_element(ports, pname);
                    g_free(pname);
                }
            }
            g_strfreev(lines);
        }
        g_free(out);
        json_object_set_int_member(obj, "port_count", (gint64)json_array_get_length(ports));
        json_object_set_array_member(obj, "ports", ports);
    }

    /* ACL 목록: pcv_ovn_acl_list 재사용 */
    {
        JsonArray *acls = pcv_ovn_acl_list(name);
        json_object_set_int_member(obj, "acl_count", (gint64)json_array_get_length(acls));
        json_object_set_array_member(obj, "acls", acls);
    }

    return obj;
}

/**
 * pcv_ovn_router_detail:
 * @name: 논리 라우터 이름
 *
 * REST API용 라우터 상세 정보 조회.
 *
 * 반환 JsonObject 구조:
 *   {
 *     "name": "pcv-lr0",
 *     "port_count": 1,
 *     "ports": [{"name":"rtr-pcv-ls0", "mac":"...", "networks":"..."}],
 *     "nat_count": 2,
 *     "nats": ["snat 192.0.2.10 10.200.0.0/24", ...]
 *   }
 *
 * 포트: ovn-nbctl lrp-list에서 파싱, 각 포트의 MAC/networks를 개별 조회.
 * NAT: pcv_ovn_nat_list() 재사용.
 *
 * Returns: (transfer full): 호출자가 json_object_unref()로 해제
 */
JsonObject *
pcv_ovn_router_detail(const gchar *name)
{
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "name", name ? name : "");
    if (!g_ovn_available || !name) return obj;
    if (!_valid_ovn_id(name)) return obj;

    /* 포트 목록: ovn-nbctl lrp-list <router> */
    {
        const gchar *argv[] = {"ovn-nbctl", "lrp-list", name, NULL};
        gchar *out = NULL;
        JsonArray *ports = json_array_new();
        if (_run_argv(argv, &out, NULL) && out) {
            gchar **lines = g_strsplit(g_strstrip(out), "\n", -1);
            for (gchar **l = lines; *l; l++) {
                if (!**l) continue;
                /* 형식: UUID (port-name) */
                gchar *lp = strchr(*l, '(');
                gchar *rp = lp ? strchr(lp, ')') : NULL;
                if (lp && rp) {
                    gchar *pname = g_strndup(lp + 1, rp - lp - 1);
                    JsonObject *pobj = json_object_new();
                    json_object_set_string_member(pobj, "name", pname);

                    /* MAC (pname 은 OVN 출력에서 파싱된 포트 이름) */
                    const gchar *margv[] = {"ovn-nbctl", "get", "Logical_Router_Port", pname, "mac", NULL};
                    gchar *mac_out = NULL;
                    if (_run_argv(margv, &mac_out, NULL) && mac_out)
                        json_object_set_string_member(pobj, "mac", g_strstrip(mac_out));
                    g_free(mac_out);

                    /* Networks */
                    const gchar *nargv[] = {"ovn-nbctl", "get", "Logical_Router_Port", pname, "networks", NULL};
                    gchar *net_out = NULL;
                    if (_run_argv(nargv, &net_out, NULL) && net_out)
                        json_object_set_string_member(pobj, "networks", g_strstrip(net_out));
                    g_free(net_out);

                    json_array_add_object_element(ports, pobj);
                    g_free(pname);
                }
            }
            g_strfreev(lines);
        }
        g_free(out);
        json_object_set_int_member(obj, "port_count", (gint64)json_array_get_length(ports));
        json_object_set_array_member(obj, "ports", ports);
    }

    /* NAT 목록: pcv_ovn_nat_list 재사용 */
    {
        JsonArray *nats = pcv_ovn_nat_list(name);
        json_object_set_int_member(obj, "nat_count", (gint64)json_array_get_length(nats));
        json_object_set_array_member(obj, "nats", nats);
    }

    return obj;
}

/* ── status ───────────────────────────────────────────────────── */

/**
 * pcv_ovn_status:
 *
 * OVN 전체 상태를 JSON 객체로 반환.
 * CLI의 "pcvctl ovn status" 및 REST /api/v1/ovn/status에서 사용.
 *
 * 반환 JsonObject:
 *   {
 *     "available": true,
 *     "version": "OVN ...",
 *     "switch_count": 3,
 *     "router_count": 1
 *   }
 *
 * OVN 미설치 시 available=false만 반환.
 *
 * Returns: (transfer full): 호출자가 json_object_unref()로 해제
 */
JsonObject *
pcv_ovn_status(void)
{
    JsonObject *obj = json_object_new();
    json_object_set_boolean_member(obj, "available", g_ovn_available);

    if (g_ovn_available) {
        const gchar *va[] = {"ovn-nbctl", "--version", NULL};
        gchar *out = NULL;
        if (_run_argv(va, &out, NULL) && out) {
            gchar *nl = strchr(out, '\n');   /* 첫 줄만 사용 (head -1 대체) */
            if (nl) *nl = '\0';
            json_object_set_string_member(obj, "version", g_strstrip(out));
        }
        g_free(out);

        JsonArray *switches = pcv_ovn_switch_list();
        json_object_set_int_member(obj, "switch_count", json_array_get_length(switches));
        json_array_unref(switches);

        JsonArray *routers = pcv_ovn_router_list();
        json_object_set_int_member(obj, "router_count", json_array_get_length(routers));
        json_array_unref(routers);
    }
    return obj;
}
