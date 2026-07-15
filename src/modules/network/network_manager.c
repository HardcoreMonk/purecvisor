/**
 * @file network_manager.c
 * @brief Linux Bridge 기반 네트워크 비동기 생성 및 제어 모듈
 *
 * ====================================================================
 * [아키텍처 위치]
 *   dispatcher.c  -->  network_manager (이 파일)  -->  network_firewall
 *                                                 -->  network_dhcp
 *                                                 -->  OVS (ovs-vsctl)
 *
 *   디스패처에서 network.* RPC 10개를 직접 라우팅받는 진입점이다.
 *   Linux Bridge / OVS 브릿지의 전체 생명주기를 관리한다.
 *
 * [담당 RPC 메서드] (10개)
 *   network.create      - 브릿지 생성 + 모드별 방화벽/DHCP 자동 설정
 *   network.delete       - 브릿지 삭제 (dnsmasq 정리, nftables 정리, 멱등)
 *   network.list         - 시스템 전체 브릿지 목록 (Linux Bridge + OVS + LXC)
 *   network.info         - 단일 브릿지 상세 정보 (모드, CIDR, 슬레이브 목록)
 *   network.mode_set     - 런타임 모드 변경 (nat/isolated/routed/bridge)
 *   network.bind_phys    - 물리 NIC을 브릿지에 슬레이브로 연결
 *   network.dhcp_toggle  - DHCP 활성/비활성 토글
 *   network.ovs.create   - OVS 브릿지 생성 (ovs-vsctl add-br)
 *   network.ovs.delete   - OVS 브릿지 삭제 (ovs-vsctl del-br)
 *   network.ovs.vxlan.add / .del - VXLAN 포트 추가/제거
 *
 * [핵심 패턴]
 *   - fire-and-forget 비동기: network.create 요청 시 JSON-RPC 응답을
 *     즉시 전송한 후 GTask 스레드에서 실제 브릿지 생성을 수행한다.
 *     콜백에서 send_response 호출 금지 (소켓 이미 닫힘).
 *   - 동시 요청 방어: g_net_inflight 해시테이블로 동일 bridge_name의
 *     중복 network.create를 차단한다 (GMutex 보호).
 *   - 멱등 삭제: network.delete에서 "Cannot find device" 에러를
 *     성공으로 처리하여 재시도가 안전하다.
 *
 * [모드별 동작 요약]
 *   nat      : 브릿지 생성 + nftables MASQUERADE + DHCP 시작 (기본값)
 *   isolated : 브릿지 생성 + forward DROP + DHCP 시작
 *   routed   : 브릿지 생성 + ip_forward 활성화, DHCP 미시작
 *   bridge   : 브릿지 생성 + 물리 NIC 슬레이브, CIDR/DHCP 없음
 *
 * [OVS 자동 감지]
 *   vm.create 시 network_bridge 파라미터가 OVS 브릿지이면
 *   ovs-vsctl br-exists 로 확인 후 VM XML에
 *   <virtualport type='openvswitch'/> 를 자동 추가한다.
 *
 * [메타데이터 파일]
 *   /var/run/purecvisor/dnsmasq-<bridge>.meta (JSON)
 *   브릿지 모드와 CIDR을 영속화하여 network.info 조회에 활용.
 *
 * [의존 모듈]
 *   network_firewall.c  - nftables 규칙 설정/해제
 *   network_dhcp.c      - dnsmasq 프로세스 관리
 *   pcv_validate.h      - bridge_name, CIDR 입력 검증
 *   pcv_spawn.h         - 외부 명령 실행 (ip, ovs-vsctl)
 *   rpc_utils.h         - JSON-RPC 응답 빌더
 *
 * [주의사항]
 *   - bridge 모드에서 cidr은 NULL로 전달됨 (IP 미할당).
 *   - network.delete 시 슬레이브 NIC의 nomaster 해제를 먼저 수행해야
 *     물리 NIC가 orphan 상태로 남지 않는다 (Fix #8).
 *   - OVS 브릿지와 Linux Bridge는 동일 인터페이스에서 구분 필요:
 *     ovs-vsctl br-exists 실패 시 Linux Bridge로 간주.
 * ====================================================================
 */
#include <stdio.h>
#include <glib.h>
#include <glib/gstdio.h>   /* g_mkdir_with_parents */
#include <gio/gio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#include "modules/network/network_manager.h"
#include "../../utils/pcv_validate.h"
#include "modules/dispatcher/rpc_utils.h"
#include "modules/network/network_firewall.h"
#include "modules/network/network_dhcp.h"
#include "../../utils/pcv_spawn.h"
#include "../../utils/pcv_log.h"
#include "../../utils/pcv_config.h"       /* NET-4: reconcile interval config */
#include "../../utils/pcv_worker_pool.h"  /* NET-4: reconcile 워커 오프로드 */
#include "vm_iface.h"
#include <json-glib/json-glib.h>

/* NET-4: QoS 재수화 주기 reconcile 타이머 상태 (security_group resync 선례 복제) */
static guint g_qos_reconcile_timer_id = 0;    /* g_timeout source id */
static gint  g_qos_reconcile_inflight = 0;    /* 중첩 방지 (g_atomic) */

/* ═══════════════════════════════════════════════════════════════════════
 * QoS 규칙 영속화 (BE-5) — JSON 파일에 tc 규칙을 저장/복원
 *
 * [주니어 참고 — tc(traffic control) QoS 개요]
 *   리눅스 tc 명령어는 네트워크 인터페이스의 트래픽 대역폭을 제어합니다.
 *   tc는 qdisc(큐잉 디스플린) → class(분류) → filter(필터) 3계층 구조입니다.
 *
 *   [HTB(Hierarchical Token Bucket) — 송신(egress) 제한]
 *     tc qdisc add dev vnet0 root handle 1: htb default 10
 *     tc class add dev vnet0 parent 1: classid 1:10 htb rate 100Mbit burst 256k
 *     → vnet0에서 나가는(VM→외부) 트래픽을 100Mbps로 제한
 *     → rate: 보장 대역폭, burst: 순간 허용 초과량
 *
 *   [Ingress Policing — 수신(ingress) 제한]
 *     tc qdisc add dev vnet0 ingress
 *     tc filter add dev vnet0 parent ffff: police rate 100mbit burst 256k drop
 *     → vnet0으로 들어오는(외부→VM) 트래픽을 100Mbps로 제한
 *     → 초과 트래픽은 drop(폐기) 처리
 *
 *   [per-VM QoS의 vnet 자동 매핑 (v1.0)]
 *     vm_name 파라미터로 VM 이름을 전달하면 _find_vm_vnet()이
 *     virsh domiflist로 해당 VM의 vnet 인터페이스(예: vnet0)를 자동으로 찾습니다.
 *     사용자가 vnet 번호를 외울 필요 없이 VM 이름만으로 QoS를 설정할 수 있습니다.
 *
 *   [QoS 영속화가 필요한 이유]
 *     tc qdisc 설정은 커널 메모리에만 존재하므로 데몬/OS 재시작 시 사라집니다.
 *     /var/lib/purecvisor/qos_rules.json에 인터페이스별 규칙을 저장하고,
 *     데몬 기동 시 pcv_qos_restore()로 복원합니다. (비휘발 — 재부팅 유지)
 *
 *   [Bridge VLAN 필터링 (v1.0)]
 *     pcvbr0 등 Linux Bridge에서 VLAN 태깅/필터링을 활성화하면
 *     브릿지 포트별로 VLAN을 분리할 수 있습니다.
 *     ip link set dev pcvbr0 type bridge vlan_filtering 1
 *     bridge vlan add dev vnet0 vid 100
 *     → 현재 network_manager에서는 기본 비활성 상태이며,
 *       필요 시 network.vlan.set RPC로 런타임 활성화 가능합니다.
 * ═══════════════════════════════════════════════════════════════════════ */
/* AF-N2: /var/run(tmpfs, 재부팅 휘발) → /var/lib(비휘발) 이전.
 * save/remove/restore 3곳이 이 매크로를 공유한다. */
#define QOS_PERSIST_PATH "/var/lib/purecvisor/qos_rules.json"

/**
 * _qos_ensure_dir — QoS 영속 파일의 부모 디렉토리 존재 보장
 *
 * QOS_PERSIST_PATH 가 /var/lib(비휘발)로 이전됨에 따라, 저장 전에
 * 부모 디렉토리(/var/lib/purecvisor)를 생성한다. 이미 존재하면 no-op.
 */
static void
_qos_ensure_dir(void)
{
    gchar *dir = g_path_get_dirname(QOS_PERSIST_PATH);
    if (g_mkdir_with_parents(dir, 0700) != 0) {
        PCV_LOG_WARN("QOS", "Cannot create dir %s: %s", dir, g_strerror(errno));
    }
    g_free(dir);
}

/**
 * _qos_persist_save — QoS 규칙을 JSON 파일에 저장 (영속화)
 *
 * [호출 시점] handle_network_qos_set()에서 tc 규칙 적용 성공 후 호출
 * [동작] 인터페이스+방향 키로 rate/burst 값을 JSON 파일에 기록
 * [파일] /var/lib/purecvisor/qos_rules.json
 * [멱등성] 동일 키로 재호출 시 기존 값을 덮어씀
 *
 * @param iface     인터페이스 이름 (예: "vnet0")
 * @param direction "egress" 또는 "ingress"
 * @param rate_mbps 대역폭 상한 (Mbps)
 * @param burst_kb  버스트 크기 (KB)
 */
static void
_qos_persist_save(const gchar *iface, const gchar *direction,
                  gint rate_mbps, gint burst_kb)
{
    _qos_ensure_dir();   /* AF-N2: /var/lib 이전에 따라 디렉토리 생성 보장 */

    /* 기존 규칙 파일 로드 */
    JsonParser *parser = json_parser_new();
    JsonObject *root = NULL;
    if (g_file_test(QOS_PERSIST_PATH, G_FILE_TEST_EXISTS)) {
        if (json_parser_load_from_file(parser, QOS_PERSIST_PATH, NULL)) {
            root = json_node_dup_object(json_parser_get_root(parser));
        }
    }
    if (!root) root = json_object_new();

    /* 인터페이스+방향 키로 규칙 저장 */
    gchar *key = g_strdup_printf("%s:%s", iface, direction);
    JsonObject *rule = json_object_new();
    json_object_set_string_member(rule, "interface", iface);
    json_object_set_string_member(rule, "direction", direction);
    json_object_set_int_member(rule, "rate_mbps", rate_mbps);
    json_object_set_int_member(rule, "burst_kb", burst_kb);
    json_object_set_object_member(root, key, rule);
    g_free(key);

    /* JSON 파일 저장 */
    JsonGenerator *gen = json_generator_new();
    json_generator_set_pretty(gen, TRUE);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, root);
    json_generator_set_root(gen, node);
    json_generator_to_file(gen, QOS_PERSIST_PATH, NULL);

    json_node_unref(node);
    json_object_unref(root);
    g_object_unref(gen);
    g_object_unref(parser);
}

/**
 * _qos_persist_remove — QoS 규칙을 JSON 파일에서 제거
 *
 * [호출 시점] network.qos.delete RPC 처리 시
 * [동작] direction이 "both"이면 egress + ingress 모두 제거
 * [멱등성] 키가 없어도 에러 없이 성공
 */
static void
_qos_persist_remove(const gchar *iface, const gchar *direction)
{
    if (!g_file_test(QOS_PERSIST_PATH, G_FILE_TEST_EXISTS)) return;

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_file(parser, QOS_PERSIST_PATH, NULL)) {
        g_object_unref(parser);
        return;
    }
    JsonNode *root_n = json_parser_get_root(parser);
    if (!root_n || json_node_get_node_type(root_n) != JSON_NODE_OBJECT) {
        g_object_unref(parser);
        return;
    }
    JsonObject *root = json_node_dup_object(root_n);

    /* "both"이면 egress + ingress 모두 제거 */
    if (g_strcmp0(direction, "both") == 0) {
        gchar *key_eg = g_strdup_printf("%s:egress", iface);
        gchar *key_in = g_strdup_printf("%s:ingress", iface);
        json_object_remove_member(root, key_eg);
        json_object_remove_member(root, key_in);
        g_free(key_eg); g_free(key_in);
    } else {
        gchar *key = g_strdup_printf("%s:%s", iface, direction);
        json_object_remove_member(root, key);
        g_free(key);
    }

    JsonGenerator *gen = json_generator_new();
    json_generator_set_pretty(gen, TRUE);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, root);
    json_generator_set_root(gen, node);
    json_generator_to_file(gen, QOS_PERSIST_PATH, NULL);

    json_node_unref(node);
    json_object_unref(root);
    g_object_unref(gen);
    g_object_unref(parser);
}

/**
 * pcv_qos_restore — 데몬 기동 시 영속화된 QoS 규칙을 복원
 *
 * [호출 시점] main.c 데몬 초기화 단계에서 호출
 * [동작] qos_rules.json에서 모든 규칙을 읽어 tc 명령으로 재적용
 * [외부 명령] tc qdisc/class/filter (egress HTB 또는 ingress policing)
 * [멱등성] tc replace 사용으로 이미 존재하는 규칙도 안전하게 교체
 */
void
pcv_qos_restore(void)
{
    if (!g_file_test(QOS_PERSIST_PATH, G_FILE_TEST_EXISTS)) return;

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_file(parser, QOS_PERSIST_PATH, NULL)) {
        g_object_unref(parser);
        return;
    }

    JsonNode *qos_root_n = json_parser_get_root(parser);
    if (!qos_root_n || json_node_get_node_type(qos_root_n) != JSON_NODE_OBJECT) {
        g_warning("[QOS] %s is not a valid JSON object — skipping restore",
                  QOS_PERSIST_PATH);
        g_object_unref(parser);
        return;
    }
    JsonObject *root = json_node_get_object(qos_root_n);
    GList *members = json_object_get_members(root);
    gint restored = 0;

    for (GList *l = members; l; l = l->next) {
        const gchar *member_key = l->data;
        JsonObject *rule = json_object_get_object_member(root, member_key);
        const gchar *iface = json_object_get_string_member(rule, "interface");
        const gchar *dir   = json_object_get_string_member(rule, "direction");
        gint rate  = (gint)json_object_get_int_member(rule, "rate_mbps");
        gint burst = (gint)json_object_get_int_member(rule, "burst_kb");

        if (!iface || rate <= 0) continue;

        /* NET-4 존재게이트: 대상 vnet 이 아직 없으면 tc 를 쏘지 않고 skip.
         * (부팅 시 vnet 미생성 → tc 실패를 삼키고 restored++ 하던 거짓 카운터 교정.
         *  또한 주기 reconcile 의 test seam: /sys/class/net/<iface> 존재 여부로 제어.)
         * 늦게 생성된 vnet 은 다음 reconcile tick 에서 적용된다. */
        gchar *sys_path = g_strdup_printf("/sys/class/net/%s", iface);
        gboolean iface_present = g_file_test(sys_path, G_FILE_TEST_EXISTS);
        g_free(sys_path);
        if (!iface_present) {
            PCV_LOG_INFO("QOS", "QoS restore skip %s (%s): iface 미존재 — 다음 reconcile 재시도",
                         iface, dir ? dir : "egress");
            continue;
        }

        PCV_LOG_INFO("QOS", "Restoring QoS for %s (%s): %d Mbps, %d KB burst",
                     iface, dir ? dir : "egress", rate, burst);

        /* Egress HTB 복원 */
        if (!dir || g_strcmp0(dir, "egress") == 0) {
            const gchar *qdisc_argv[] = {"tc", "qdisc", "replace", "dev", iface,
                "root", "handle", "1:", "htb", "default", "10", NULL};
            pcv_spawn_sync(qdisc_argv, NULL, NULL, NULL);

            gchar *rate_str = g_strdup_printf("%dMbit", rate);
            gchar *burst_str = g_strdup_printf("%dk", burst);
            const gchar *class_argv[] = {"tc", "class", "replace", "dev", iface,
                "parent", "1:", "classid", "1:10", "htb",
                "rate", rate_str, "burst", burst_str, NULL};
            pcv_spawn_sync(class_argv, NULL, NULL, NULL);
            g_free(rate_str); g_free(burst_str);
        } else {
            /* Ingress policing 복원 */
            /* NET-4 멱등: 런타임 set 핸들러(2347)와 동일하게 del-then-add.
             * 기존 ingress qdisc 를 먼저 제거(best-effort)해야 주기 reconcile
             * 재실행 시 police 필터가 누적되지 않는다. */
            const gchar *ing_del_argv[] = {"tc", "qdisc", "del", "dev", iface,
                "ingress", NULL};
            pcv_spawn_sync(ing_del_argv, NULL, NULL, NULL);

            const gchar *ing_argv[] = {"tc", "qdisc", "add", "dev", iface,
                "ingress", NULL};
            pcv_spawn_sync(ing_argv, NULL, NULL, NULL);

            gchar *rate_str = g_strdup_printf("%dmbit", rate);
            gchar *burst_str = g_strdup_printf("%dk", burst);
            const gchar *filter_argv[] = {"tc", "filter", "add", "dev", iface,
                "parent", "ffff:", "protocol", "all", "u32",
                "match", "u32", "0", "0",
                "police", "rate", rate_str, "burst", burst_str,
                "action", "drop", NULL};
            pcv_spawn_sync(filter_argv, NULL, NULL, NULL);
            g_free(rate_str); g_free(burst_str);
        }
        restored++;
    }

    g_list_free(members);
    g_object_unref(parser);

    if (restored > 0)
        PCV_LOG_INFO("QOS", "Restored %d QoS rule(s) from %s",
                     restored, QOS_PERSIST_PATH);
}

/* PCV_SAFETY_CONTROL: qos-rehydrate — 부팅 후 늦게 생성된 vnet에도 persisted QoS를
 * 주기 reconcile로 최종 적용(부팅1회성 무동작 제거); 존재게이트로 무동작 카운터 교정 (NET-4) */
void
pcv_qos_reconcile(void)
{
    /* 존재게이트 + ingress del-then-add 멱등으로 restore 본문 재실행이 안전해졌다
     * (egress 는 tc replace 로 원래 멱등). 부팅1회성 restore 를 그대로 재호출한다. */
    pcv_qos_restore();
}

/* NET-4: reconcile 워커 — 블로킹 tc 실행 후 in-flight 플래그 리셋 (SG _sg_resync_worker 복제) */
static void
_qos_reconcile_worker(GTask *task, gpointer src, gpointer td, GCancellable *c)
{
    (void)src; (void)td; (void)c;
    pcv_qos_reconcile();
    g_atomic_int_set(&g_qos_reconcile_inflight, 0);
    g_task_return_boolean(task, TRUE);
}

/* NET-4: 타이머 tick — 메인 루프, 논블로킹. 이전 reconcile 진행 중이면 skip.
 * tc 는 블로킹이므로 worker pool 로 오프로드(GMainLoop 에서 실행 금지). */
static gboolean
_qos_reconcile_tick(gpointer data)
{
    (void)data;
    if (!g_atomic_int_compare_and_exchange(&g_qos_reconcile_inflight, 0, 1))
        return G_SOURCE_CONTINUE;   /* 이전 reconcile 아직 진행 중 → 이번 tick skip */
    GTask *t = g_task_new(NULL, NULL, NULL, NULL);
    pcv_worker_pool_push(t, _qos_reconcile_worker);
    g_object_unref(t);   /* worker pool 이 자체 ref 를 잡음 */
    return G_SOURCE_CONTINUE;
}

void
pcv_qos_reconcile_timer_init(void)
{
    gint interval = pcv_config_get_int("qos", "reconcile_interval_sec", 300);
    if (interval <= 0) {
        PCV_LOG_INFO("QOS", "QoS reconcile 타이머 비활성 (reconcile_interval_sec=%d)", interval);
        return;
    }
    g_qos_reconcile_timer_id = g_timeout_add_seconds((guint)interval, _qos_reconcile_tick, NULL);
    PCV_LOG_INFO("QOS", "QoS reconcile 타이머 등록 (%d초 주기)", interval);
}

void
pcv_qos_reconcile_timer_shutdown(void)
{
    if (g_qos_reconcile_timer_id) {
        g_source_remove(g_qos_reconcile_timer_id);
        g_qos_reconcile_timer_id = 0;
    }
}

typedef struct {
    gchar    *bridge_name;
    gchar    *cidr;
    gchar    *mode;
    gchar    *physical_if;
    gchar    *rpc_id;
    gchar    *dhcp_warning;   /* DHCP soft-fail 메시지 (NULL = 정상) */
    gboolean  dns_enabled;    /* [E5] TRUE이면 dnsmasq DNS 포워더 활성화 */
    gchar    *upstream_dns;   /* [E5] 업스트림 DNS 주소 (NULL → "8.8.8.8") */
    gchar    *ipv6_prefix;    /* IPv6 RA+DHCPv6 프리픽스 (NULL = IPv6 비활성) */
    gint      mtu;            /* [S-4] 브릿지 MTU (0이면 기본 1500) */
    UdsServer *server;
    GSocketConnection *connection;
} NetworkCtx;

/* ── [E6] 동시 network.create 중복 요청 방어 ──────────────────────────
 *
 * [왜 중복 방어가 필요한가?]
 *   브릿지 생성은 수 초가 걸리는 비동기 작업입니다. 같은 bridge_name으로
 *   두 요청이 동시에 들어오면 "ip link add" 명령이 두 번 실행되어
 *   "RTNETLINK: File exists" 에러가 발생합니다.
 *
 * [구현 방식]
 *   g_net_inflight 해시테이블에 생성 중인 bridge_name을 등록합니다.
 *   두 번째 요청은 해시테이블에서 감지되어 즉시 에러를 반환합니다.
 *   GTask 완료 콜백(network_action_callback)에서 항목을 제거합니다.
 *
 * [g_once_init_enter 패턴]
 *   해시테이블 초기화를 스레드 안전하게 한 번만 수행합니다.
 *   GLib의 표준 once 초기화 패턴입니다.
 */
static GHashTable *g_net_inflight = NULL;
static GMutex      g_net_inflight_mu;

static void _net_inflight_init_once(void) {
    static gsize initialized = 0;
    if (g_once_init_enter(&initialized)) {
        g_net_inflight = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        g_once_init_leave(&initialized, 1);
    }
}

/**
 * free_network_ctx — NetworkCtx 구조체 메모리 해제 콜백
 * @data: GTask의 task_data로 전달된 NetworkCtx 포인터
 *
 * GTask의 GDestroyNotify 콜백으로 등록되어 태스크 완료 시 자동 호출.
 * 모든 동적 할당 필드(bridge_name, cidr, mode 등)와 GObject 참조를 해제.
 */
static void free_network_ctx(gpointer data) {
    if (!data) return;
    NetworkCtx *ctx = (NetworkCtx *)data;
    g_free(ctx->bridge_name);
    g_free(ctx->cidr);
    g_free(ctx->mode);
    g_free(ctx->physical_if);
    g_free(ctx->rpc_id);
    g_free(ctx->dhcp_warning);
    g_free(ctx->upstream_dns);  /* [E5] */
    g_free(ctx->ipv6_prefix);   /* IPv6 RA+DHCPv6 */
    if (ctx->server) g_object_unref(ctx->server);
    if (ctx->connection) g_object_unref(ctx->connection);
    g_free(ctx);
}

/* =================================================================
 * [내부 유틸리티] Bridge 제어 함수들
 *
 * [주니어 참고 — Linux Bridge 생성 흐름]
 *   1. ip link add name <br> type bridge stp_state 0  → 브릿지 인터페이스 생성
 *      stp_state=0: STP(Spanning Tree Protocol) 비활성화 — VM 환경에서 불필요
 *   2. ip addr add <cidr> dev <br>  → 브릿지에 IP 주소 할당 (게이트웨이 역할)
 *      bridge 모드에서는 cidr=NULL이므로 IP 할당을 건너뜁니다
 *   3. ip link set dev <br> up  → 브릿지 인터페이스 활성화
 *
 * [pcv_spawn_sync vs pcv_spawn_fire]
 *   pcv_spawn_sync: 동기 실행, 성공/실패 반환 (중요 명령에 사용)
 *   pcv_spawn_fire: fire-and-forget, 결과 무시 (up 같은 부수적 명령에 사용)
 * ================================================================= */
/**
 * network_bridge_create — Linux Bridge 인터페이스를 새로 생성하고 IP를 할당
 * @bridge_name: 생성할 브릿지 이름 (예: "pcvbr0")
 * @cidr: 게이트웨이 IP/CIDR (예: "10.10.10.1/24" 또는 "fd00::1/64"), bridge 모드일 경우 NULL
 * @mtu: MTU 값 (0이면 기본 1500 적용)
 * @error: 에러 반환 포인터
 *
 * 실행 순서:
 *   1) ip link add name <br> type bridge stp_state 0
 *      stp_state=0: STP 비활성화 — VM 환경에서 불필요하며 수렴 지연만 발생
 *   2) ip addr add <cidr> dev <br> — 게이트웨이 IP 할당 (cidr이 NULL이면 건너뜀)
 *   3) ip link set dev <br> mtu <mtu> — MTU 설정 (기본 1500)
 *   4) ip link set dev <br> up — 인터페이스 활성화
 *
 * @return 성공 시 TRUE, 실패 시 FALSE + GError 설정
 */
gboolean network_bridge_create(const gchar *bridge_name, const gchar *cidr, gint mtu, GError **error) {

    // 1. 브릿지 생성
    {
        const gchar *argv[] = {"ip","link","add","name",bridge_name,"type","bridge","stp_state","0",NULL};
        gchar *std_err_local = NULL;
        if (!pcv_spawn_sync(argv, NULL, &std_err_local, error)) {
            if (error && !*error)
                g_set_error(error,G_IO_ERROR,G_IO_ERROR_FAILED,"Bridge creation failed: %s",
                            std_err_local ? std_err_local : "unknown");
            g_free(std_err_local); return FALSE;
        }
        g_free(std_err_local);
    }

    // 2. IP 할당 (Bridge 모드일 경우 cidr이 NULL로 들어옴)
    if (cidr && strlen(cidr) > 0) {
        const gchar *addr_argv[] = {"ip","addr","add",cidr,"dev",bridge_name,NULL};
        gchar *std_err_local = NULL;
        if (!pcv_spawn_sync(addr_argv, NULL, &std_err_local, error)) {
            if (error && !*error)
                g_set_error(error,G_IO_ERROR,G_IO_ERROR_FAILED,"IP assignment failed: %s",
                            std_err_local ? std_err_local : "unknown");
            g_free(std_err_local);
            /* 롤백: IP 할당 실패 시 생성된 브릿지 제거 (고아 리소스 방지) */
            const gchar *del[] = {"ip","link","del",bridge_name,NULL};
            pcv_spawn_fire(del);
            return FALSE;
        }
        g_free(std_err_local);
    }

    // 3. MTU 설정 (0이면 기본 1500 적용)
    // B4-C3 (Phase 2 fix): pcv_spawn_fire → pcv_spawn_sync 전환 + 실패 시 롤백.
    // 이전엔 결과 무시 → MTU 미설정 또는 UP 실패 시 비활성 브릿지가 고아로 잔존.
    {
        gint eff_mtu = (mtu > 0) ? mtu : 1500;
        gchar mtu_str[16];
        g_snprintf(mtu_str, sizeof(mtu_str), "%d", eff_mtu);
        const gchar *mtu_argv[] = {"ip","link","set","dev",bridge_name,"mtu",mtu_str,NULL};
        gchar *mtu_err = NULL;
        if (!pcv_spawn_sync(mtu_argv, NULL, &mtu_err, error)) {
            if (error && !*error)
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "MTU set failed: %s", mtu_err ? mtu_err : "unknown");
            g_free(mtu_err);
            /* 롤백: IP 제거 + 브릿지 삭제 */
            if (cidr && *cidr) {
                const gchar *unaddr[] = {"ip","addr","del",cidr,"dev",bridge_name,NULL};
                pcv_spawn_fire(unaddr);
            }
            const gchar *del[] = {"ip","link","del",bridge_name,NULL};
            pcv_spawn_fire(del);
            return FALSE;
        }
        g_free(mtu_err);
    }

    // 4. 브릿지 UP
    {
        const gchar *up_argv[] = {"ip","link","set","dev",bridge_name,"up",NULL};
        gchar *up_err = NULL;
        if (!pcv_spawn_sync(up_argv, NULL, &up_err, error)) {
            if (error && !*error)
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Bridge UP failed: %s", up_err ? up_err : "unknown");
            g_free(up_err);
            /* 롤백: IP 제거 + 브릿지 삭제 */
            if (cidr && *cidr) {
                const gchar *unaddr[] = {"ip","addr","del",cidr,"dev",bridge_name,NULL};
                pcv_spawn_fire(unaddr);
            }
            const gchar *del[] = {"ip","link","del",bridge_name,NULL};
            pcv_spawn_fire(del);
            return FALSE;
        }
        g_free(up_err);
    }

    return TRUE;
}

/**
 * network_bridge_bind_physical — 물리 NIC을 브릿지의 슬레이브로 연결
 * @bridge_name: 대상 브릿지 이름
 * @physical_if: 물리 NIC 이름 (예: "eno1")
 * @error: 에러 반환 포인터
 *
 * ip link set <physical_if> master <bridge_name> 으로 물리 NIC을 브릿지에 바인딩.
 * bridge 모드에서 VM이 물리 네트워크에 직접 참여(L2 브릿징)할 때 사용.
 *
 * [주의] 관리 NIC(eno1)을 바인딩하면 해당 NIC의 IP가 사라져
 *        SSH 접속이 끊어질 수 있음. 브릿지에 IP를 재할당해야 함.
 *
 * @return 성공 시 TRUE
 */
gboolean network_bridge_bind_physical(const gchar *bridge_name, const gchar *physical_if, GError **error) {
    {
        const gchar *argv[] = {"ip","link","set",physical_if,"master",bridge_name,NULL};
        gchar *std_err_local = NULL;
        if (!pcv_spawn_sync(argv, NULL, &std_err_local, error)) {
            if (error && !*error)
                g_set_error(error,G_IO_ERROR,G_IO_ERROR_FAILED,
                            "Failed to bind physical NIC '%s': %s",
                            physical_if, std_err_local ? std_err_local : "unknown");
            g_free(std_err_local); return FALSE;
        }
        g_free(std_err_local);
    }
    { const gchar *a[] = {"ip","link","set","dev",physical_if,"up",NULL}; pcv_spawn_fire(a); }

    return TRUE;
}

/**
 * network_bridge_delete — 브릿지 인터페이스 삭제 (멱등)
 *
 * [삭제 순서가 중요한 이유]
 *   1. dnsmasq 프로세스 종료 (PID 파일로 식별)
 *   2. 관련 파일 삭제 (conf, pid, leases, meta)
 *   3. 슬레이브 NIC의 master 해제 (nomaster)
 *      → 이것을 먼저 하지 않으면 물리 NIC가 orphan 상태로 남아
 *        네트워크 연결이 끊어질 수 있습니다 (Fix #8)
 *   4. ip link delete <br> type bridge
 *
 * [멱등 삭제]
 *   "Cannot find device" 에러가 발생하면 (이미 삭제된 브릿지)
 *   에러를 무시하고 성공으로 처리합니다. 재시도가 안전합니다.
 */
gboolean network_bridge_delete(const gchar *bridge_name, GError **error) {
    gchar *pid_path  = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.pid",    bridge_name);
    gchar *conf_path = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.conf",   bridge_name);
    gchar *lease_path= g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.leases", bridge_name);
    gchar *meta_path = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.meta",   bridge_name);
    { const gchar *a[] = {"pkill","-F",pid_path,NULL}; pcv_spawn_fire(a); }
    remove(conf_path); remove(pid_path); remove(lease_path); remove(meta_path);
    g_free(pid_path); g_free(conf_path); g_free(lease_path); g_free(meta_path);

    /* [Fix #8] bridge 모드로 생성된 경우 슬레이브 NIC의 마스터 해제.
     * ip link delete bridge 전에 처리해야 NIC가 orphan 상태로 남지 않음. */
    {
        gchar *brif_path = g_strdup_printf("/sys/class/net/%s/brif", bridge_name);
        GDir  *dir = g_dir_open(brif_path, 0, NULL);
        if (dir) {
            const gchar *slave;
            while ((slave = g_dir_read_name(dir)) != NULL) {
                const gchar *nm[] = {"ip","link","set",slave,"nomaster",NULL};
                pcv_spawn_fire(nm);
            }
            g_dir_close(dir);
        }
        g_free(brif_path);
    }

    const gchar *del_argv[] = {"ip","link","delete",bridge_name,"type","bridge",NULL};
    gchar *std_err = NULL;
    pcv_spawn_sync(del_argv, NULL, &std_err, error);

    /* [Fix #5] 에러 핸들링 역전 수정:
     * 이전: std_err != NULL 조건 → std_err=NULL인 실패(권한 오류 등)를 놓침
     * 이후: *error 설정 여부가 1차 판단 기준. "Cannot find device"는 멱등 성공 처리. */
    if (error && *error) {
        const gchar *msg = (*error)->message ? (*error)->message : "";
        if (strstr(msg, "Cannot find device") || strstr(msg, "does not exist")) {
            /* 이미 없는 브릿지는 삭제 성공으로 간주 (멱등) */
            g_error_free(*error);
            *error = NULL;
        } else {
            g_free(std_err);
            return FALSE;
        }
    }

    g_free(std_err);
    return TRUE;
}

/* ── [E3] 네트워크 메타데이터 퍼시스턴스 ──────────────────────────────
 *
 * 브릿지 생성 모드는 커널 브릿지 오브젝트에 저장되지 않으므로
 * PCV_NETWORK_RUNDIR/dnsmasq-<br>.meta JSON 파일로 영속화.
 *
 * 저장: network.create 성공 후
 * 삭제: network.delete 시 (Fix #8에서 meta_path 삭제 포함)
 * 읽기: handle_network_list_request / handle_network_info_request
 * ──────────────────────────────────────────────────────────────── */
static void _network_meta_save(const gchar *bridge_name, const gchar *mode, const gchar *cidr) {
    gchar *meta_path = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.meta", bridge_name);
    gchar *content   = g_strdup_printf(
        "{\"mode\":\"%s\",\"cidr\":\"%s\"}",
        mode  ? mode  : "nat",
        cidr  ? cidr  : "");
    if (g_file_set_contents(meta_path, content, -1, NULL)) {
        /* B4-C1 (Phase 1 fix): 0644 default → 0600 — 브릿지 모드/CIDR
         * 정보 노출 차단. dnsmasq meta는 운영자만 읽어야 한다. */
        if (chmod(meta_path, 0600) != 0) {
            PCV_LOG_WARN("network", "chmod 0600 failed on %s: %s",
                         meta_path, g_strerror(errno));
        }
    }
    g_free(meta_path);
    g_free(content);
}

/**
 * _network_meta_get_mode — 브릿지의 네트워크 모드를 반환
 *
 * [모드 결정 우선순위]
 *   1. meta 파일 존재 시: JSON에서 mode 필드 읽기
 *   2. meta 파일 없는 외부 브릿지: 이름 패턴으로 추론
 *      - lxcbr*  → "nat" (LXC 기본 NAT 브릿지)
 *      - virbr*  → "nat" (libvirt 기본 NAT 브릿지)
 *      - docker0 → "nat" (Docker 기본 NAT 브릿지)
 *      - pcvbr*  → "bridge" (PureCVisor 물리 NIC 브릿지)
 *      - pcvoverlay* → "bridge" (OVS 오버레이 브릿지)
 *   3. /etc/lxc/default.conf에서 lxc.net.0.link 매칭 시 → "nat"
 *   4. /sys/class/net/<br>/brif/에 비-vnet 슬레이브 존재 시 → "bridge"
 *   5. 모두 실패 시 → "unknown"
 *
 * [왜 이렇게 복잡한가?]
 *   PureCVisor가 생성하지 않은 외부 브릿지(lxcbr0, virbr0, docker0 등)도
 *   network.list에 표시되어야 하므로 meta 파일 없이도 모드를 추론해야 합니다.
 */
/* pcv_network_meta_save — bootstrap 등 외부 생성 경로용 export 래퍼 (VP-5 잔여).
 * network create 경로 밖에서 만든 브릿지(예: 기본 네트워크 pcvnat0)도 meta를
 * 남겨야 network list의 mode 컬럼이 unknown으로 표시되지 않는다. */
void pcv_network_meta_save(const gchar *bridge_name, const gchar *mode, const gchar *cidr) {
    _network_meta_save(bridge_name, mode, cidr);
}

static gchar *_network_meta_get_mode(const gchar *bridge_name) {
    gchar *meta_path = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.meta", bridge_name);
    gchar *content   = NULL;
    gchar *mode_out  = NULL;

    if (g_file_get_contents(meta_path, &content, NULL, NULL) && content) {
        /* 간단한 JSON 파싱: "mode":"<value>" */
        const gchar *hit = strstr(content, "\"mode\":\"");
        if (hit) {
            hit += 8; /* skip "mode":"  */
            const gchar *end = strchr(hit, '"');
            if (end) {
                mode_out = g_strndup(hit, end - hit);
            }
        }
        g_free(content);
    }
    g_free(meta_path);

    /* meta 파일 없는 외부 브릿지 (lxcbr0, virbr0 등) — mode 추론 */
    if (!mode_out) {
        /* lxcbr0: LXC 기본 브릿지 — NAT 모드 */
        if (g_str_has_prefix(bridge_name, "lxcbr"))
            mode_out = g_strdup("nat");
        /* virbr: libvirt 기본 NAT 브릿지 */
        else if (g_str_has_prefix(bridge_name, "virbr"))
            mode_out = g_strdup("nat");
        /* docker0: Docker NAT 브릿지 */
        else if (g_strcmp0(bridge_name, "docker0") == 0)
            mode_out = g_strdup("nat");
        /* br-: Docker 사용자 브릿지 */
        else if (g_str_has_prefix(bridge_name, "br-"))
            mode_out = g_strdup("bridge");
        /* pcvbr0, pcvbr1...: PureCVisor 물리 NIC 브릿지 */
        else if (g_str_has_prefix(bridge_name, "pcvbr"))
            mode_out = g_strdup("bridge");
        /* pcvoverlay: OVS 오버레이 브릿지 */
        else if (g_str_has_prefix(bridge_name, "pcvoverlay"))
            mode_out = g_strdup("bridge");
        else {
            /* /etc/lxc/default.conf 파싱 */
            gchar *lxc_conf = NULL;
            if (g_file_get_contents("/etc/lxc/default.conf", &lxc_conf, NULL, NULL)) {
                gchar *needle = g_strdup_printf("lxc.net.0.link = %s", bridge_name);
                if (strstr(lxc_conf, needle))
                    mode_out = g_strdup("nat");
                g_free(needle);
                g_free(lxc_conf);
            }
            /* 물리 인터페이스 슬레이브 감지 → bridge 모드
             * /sys/class/net/<br>/brif/ 에서 슬레이브 목록을 조회합니다.
             * vnet*, tap*, veth* 는 VM/컨테이너 가상 인터페이스이므로 제외하고,
             * 그 외 이름(eno1, eth0 등)이 발견되면 물리 NIC가 브릿지에
             * 바인딩된 것이므로 bridge 모드로 판정합니다. */
            if (!mode_out) {
                gchar *brif = g_strdup_printf("/sys/class/net/%s/brif", bridge_name);
                GDir *d = g_dir_open(brif, 0, NULL);
                if (d) {
                    const gchar *ifn;
                    while ((ifn = g_dir_read_name(d))) {
                        /* 가상 인터페이스(VM/컨테이너)가 아닌 물리 NIC 발견 */
                        if (!g_str_has_prefix(ifn, "vnet") &&
                            !g_str_has_prefix(ifn, "tap") &&
                            !g_str_has_prefix(ifn, "veth")) {
                            mode_out = g_strdup("bridge");
                            break;
                        }
                    }
                    g_dir_close(d);
                }
                g_free(brif);
            }
            if (!mode_out)
                mode_out = g_strdup("unknown");
        }
    }

    return mode_out;
}

/* =================================================================
 * [워커 스레드] GTask 기반 비동기 실행 로직
 *
 * [모드별 동작 요약]
 *   nat (기본):
 *     브릿지 생성 → nftables MASQUERADE + FORWARD 규칙 → dnsmasq DHCP 시작
 *     VM에서 외부 인터넷 접근 가능 (NAT 변환)
 *
 *   isolated:
 *     브릿지 생성 → nftables FORWARD DROP → dnsmasq DHCP 시작
 *     같은 브릿지의 VM끼리만 통신 가능, 외부 차단
 *
 *   routed:
 *     브릿지 생성 → ip_forward 활성화 → DHCP 없음
 *     정적 라우팅 환경, 외부 라우터가 경로를 알고 있어야 함
 *
 *   bridge:
 *     브릿지 생성 → 물리 NIC 슬레이브 연결 → IP 없음, DHCP 없음
 *     VM이 물리 네트워크에 직접 참여 (L2 브릿징)
 *     physical_if 파라미터 필수 (예: eno1)
 *
 * [DHCP soft-fail 패턴]
 *   DHCP(dnsmasq) 시작 실패는 브릿지 생성 자체를 실패시키지 않습니다.
 *   libvirt의 virbr0 등이 이미 포트 67을 점유하고 있는 환경에서
 *   dnsmasq 충돌이 발생할 수 있기 때문입니다.
 *   실패 시 dhcp_warning 메시지를 응답에 포함하여 사용자에게 알립니다.
 * ================================================================= */
/**
 * network_create_worker — 브릿지 생성 GTask 워커 스레드
 * @task: GTask 객체 (g_task_return_*()로 결과 반환)
 * @source_obj: (unused)
 * @task_data: NetworkCtx 구조체 포인터
 * @cancellable: (unused)
 *
 * GTask 스레드 풀에서 실행되는 비동기 워커.
 * 모드(nat/isolated/routed/bridge)에 따라:
 *   1) network_bridge_create()로 브릿지 생성
 *   2) 방화벽 규칙 적용 (network_firewall_setup_*)
 *   3) DHCP 시작 (nat/isolated만, soft-fail 패턴)
 *   4) 물리 NIC 바인딩 (bridge 모드만)
 *   5) 메타데이터 파일 저장
 *
 * [DHCP soft-fail] dnsmasq 시작 실패 시 브릿지 생성은 성공 처리하고
 * dhcp_warning 메시지만 응답에 포함 (libvirt dnsmasq와 포트 67 충돌 대응).
 */
static void network_create_worker(GTask *task, gpointer source_obj, gpointer task_data, GCancellable *cancellable) {
    NetworkCtx *ctx = (NetworkCtx *)task_data;
    GError *error = NULL;

    /* cidr은 bridge 모드 외 필수 */
    const gchar *cidr = (g_strcmp0(ctx->mode, "bridge") == 0) ? NULL : ctx->cidr;

    if (!network_bridge_create(ctx->bridge_name, cidr, ctx->mtu, &error)) {
        g_task_return_error(task, error);
        return;
    }

    if (g_strcmp0(ctx->mode, "nat") == 0 || ctx->mode == NULL) {
        /* nat (기본) — firewall은 hard, DHCP는 soft (포트 67 충돌 환경 대응) */
        if (!network_firewall_setup_nat(ctx->bridge_name, ctx->cidr, &error)) {
            g_task_return_error(task, error);
            return;
        }
        GError *dhcp_err = NULL;
        if (!network_dhcp_start_ex(ctx->bridge_name, ctx->cidr,
                                ctx->dns_enabled, ctx->upstream_dns, &dhcp_err)) {
            /* [E5] DHCP 실패는 브릿지 생성 자체를 실패시키지 않음
             * (libvirt dnsmasq 등이 port 67 점유 시 발생)
             * 경고 메시지만 기록하고 성공 응답 반환 */
            ctx->dhcp_warning = g_strdup(dhcp_err ? dhcp_err->message : "dnsmasq start failed");
            g_error_free(dhcp_err);
        }
    }
    else if (g_strcmp0(ctx->mode, "isolated") == 0) {
        /* isolated — firewall hard, DHCP soft */
        if (!network_firewall_setup_isolated(ctx->bridge_name, ctx->cidr, &error)) {
            g_task_return_error(task, error);
            return;
        }
        GError *dhcp_err = NULL;
        if (!network_dhcp_start_ex(ctx->bridge_name, ctx->cidr,
                                ctx->dns_enabled, ctx->upstream_dns, &dhcp_err)) {
            ctx->dhcp_warning = g_strdup(dhcp_err ? dhcp_err->message : "dnsmasq start failed");
            g_error_free(dhcp_err);
        }
    }
    else if (g_strcmp0(ctx->mode, "routed") == 0) {
        /* routed — masquerade 없이 IP forward만 (DHCP 없음) */
        if (!network_firewall_setup_routed(ctx->bridge_name, ctx->cidr, &error)) {
            g_task_return_error(task, error);
            return;
        }
    }
    else if (g_strcmp0(ctx->mode, "bridge") == 0) {
        if (!ctx->physical_if) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Missing 'physical_if' for bridge mode");
            g_task_return_error(task, error);
            return;
        }
        if (!network_bridge_bind_physical(ctx->bridge_name, ctx->physical_if, &error)) {
            g_task_return_error(task, error);
            return;
        }
    }

    /* IPv6 RA+DHCPv6: ipv6_prefix가 있으면 IPv6 DHCP 설정 추가 (soft-fail) */
    /* [V4] 방어적 이중 검증: 핸들러에서 이미 거부되지만, 잘못된 prefix가
     * v6_gw_cidr → "ip -6 addr add" argv 로 흘러가는 것을 워커 단계에서도 차단.
     * 검증 실패 시 IPv6 설정 전체를 건너뛰고 경고만 남긴다 (soft-fail). */
    if (ctx->ipv6_prefix && ctx->ipv6_prefix[0]
        && !pcv_validate_ipv6_prefix(ctx->ipv6_prefix)) {
        gchar *warn = g_strdup("IPv6 setup skipped: invalid ipv6_prefix");
        if (ctx->dhcp_warning) {
            gchar *merged = g_strdup_printf("%s; %s", ctx->dhcp_warning, warn);
            g_free(ctx->dhcp_warning);
            ctx->dhcp_warning = merged;
            g_free(warn);
        } else {
            ctx->dhcp_warning = warn;
        }
    }
    else if (ctx->ipv6_prefix && ctx->ipv6_prefix[0]) {
        /* 브릿지에 IPv6 주소 할당: prefix::1/prefix_len */
        {
            const gchar *slash = g_strrstr(ctx->ipv6_prefix, "/");
            gchar *v6_gw_cidr = NULL;
            gchar *prefix_base = slash
                ? g_strndup(ctx->ipv6_prefix, (gsize)(slash - ctx->ipv6_prefix))
                : g_strdup(ctx->ipv6_prefix);
            if (g_str_has_suffix(prefix_base, "::"))
                v6_gw_cidr = g_strdup_printf("%s1%s", prefix_base, slash ? slash : "/64");
            else if (g_str_has_suffix(prefix_base, ":"))
                v6_gw_cidr = g_strdup_printf("%s:1%s", prefix_base, slash ? slash : "/64");
            else
                v6_gw_cidr = g_strdup_printf("%s::1%s", prefix_base, slash ? slash : "/64");
            const gchar *v6_argv[] = {"ip", "-6", "addr", "add", v6_gw_cidr,
                "dev", ctx->bridge_name, NULL};
            pcv_spawn_fire(v6_argv);
            g_free(prefix_base);
            g_free(v6_gw_cidr);
        }
        GError *v6_err = NULL;
        if (!network_dhcp_start_v6(ctx->bridge_name, ctx->ipv6_prefix, &v6_err)) {
            gchar *warn = g_strdup_printf("IPv6 DHCP soft-fail: %s",
                v6_err ? v6_err->message : "unknown");
            if (ctx->dhcp_warning) {
                gchar *merged = g_strdup_printf("%s; %s", ctx->dhcp_warning, warn);
                g_free(ctx->dhcp_warning);
                ctx->dhcp_warning = merged;
                g_free(warn);
            } else {
                ctx->dhcp_warning = warn;
            }
            if (v6_err) g_error_free(v6_err);
        }
    }

    /* [E3] 생성 성공 후 mode 메타데이터 저장 */
    _network_meta_save(ctx->bridge_name,
                       ctx->mode ? ctx->mode : "nat",
                       ctx->cidr);
    g_task_return_boolean(task, TRUE);
}

/**
 * network_delete_worker — 브릿지 삭제 GTask 워커 스레드
 * @task: GTask 객체
 * @source_obj: (unused)
 * @task_data: NetworkCtx 구조체 (bridge_name 포함)
 * @cancellable: (unused)
 *
 * 삭제 순서:
 *   1) nftables 규칙 제거 (network_firewall_teardown)
 *   2) network_bridge_delete()로 브릿지 삭제 (dnsmasq 종료 + 파일 정리 + 슬레이브 해제 포함)
 */
static void network_delete_worker(GTask *task, gpointer source_obj, gpointer task_data, GCancellable *cancellable) {
    NetworkCtx *ctx = (NetworkCtx *)task_data;
    GError *error = NULL;

    /* [B-3 수정] 브릿지 삭제 전 nftables 규칙 먼저 제거 */
    network_firewall_teardown(ctx->bridge_name, NULL);

    if (!network_bridge_delete(ctx->bridge_name, &error)) {
        g_task_return_error(task, error);
        return;
    }
    g_task_return_boolean(task, TRUE);
}

/**
 * network_action_callback — GTask 완료 후 JSON-RPC 응답 전송 콜백
 * @source_obj: (unused)
 * @res: GAsyncResult (GTask로 캐스팅)
 * @user_data: NetworkCtx 구조체 포인터
 *
 * GTask의 GAsyncReadyCallback. 워커 스레드 완료 후 메인 루프에서 실행.
 *
 * [동작]
 *   - 실패 시: pure_rpc_build_error_response()로 에러 응답 전송
 *   - 성공 시: {bridge, status:"created"/"deleted", dhcp_warning?} 응답 전송
 *   - create 완료 시: g_net_inflight 해시테이블에서 항목 제거 (중복 방어 해제)
 *
 * [is_delete 플래그] GTask 객체에 g_object_set_data()로 태깅된 값으로
 * create/delete를 구분. delete에는 inflight 해제 불필요.
 */
static void network_action_callback(GObject *source_obj, GAsyncResult *res, gpointer user_data) {
    GTask *task = G_TASK(res);
    NetworkCtx *ctx = (NetworkCtx *)user_data;
    GError *error = NULL;

    if (!g_task_propagate_boolean(task, &error)) {
        gchar *err_resp = pure_rpc_build_error_response(ctx->rpc_id, -32000, error->message);
        pure_uds_server_send_response(ctx->server, ctx->connection, err_resp);
        g_free(err_resp);
        g_error_free(error);
    } else {
        JsonObject *result_obj = json_object_new();
        json_object_set_string_member(result_obj, "bridge", ctx->bridge_name);
        
        // 🚀 GTask 객체에 심어둔 "is_delete" 플래그를 정확하게 읽어와서 판단합니다.
        gboolean is_delete = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(task), "is_delete"));
        /* [E6] create 완료 시 inflight 항목 제거 */
        if (!is_delete && g_net_inflight) {
            g_mutex_lock(&g_net_inflight_mu);
            g_hash_table_remove(g_net_inflight, ctx->bridge_name);
            g_mutex_unlock(&g_net_inflight_mu);
        }
        
        if (is_delete) {
            json_object_set_string_member(result_obj, "status", "deleted");
        } else {
            json_object_set_string_member(result_obj, "status", "created");
            /* DHCP soft-fail 경고 포함 (NULL이면 필드 없음) */
            if (ctx->dhcp_warning)
                json_object_set_string_member(result_obj, "dhcp_warning", ctx->dhcp_warning);
        }

        JsonNode *result_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(result_node, result_obj);
        
        gchar *succ_resp = pure_rpc_build_success_response(ctx->rpc_id, result_node);
        pure_uds_server_send_response(ctx->server, ctx->connection, succ_resp);
        g_free(succ_resp);
    }
}

/* =================================================================
 * [디스패처 진입점] JSON-RPC 요청 라우팅
 *
 * 아래 handle_network_*_request() 함수들은 dispatcher.c의 else-if 체인에서
 * 메서드명으로 라우팅되어 호출된다. 모든 핸들러의 시그니처는 동일:
 *   (JsonObject *params, const gchar *rpc_id, UdsServer *, GSocketConnection *)
 * ================================================================= */

/**
 * handle_network_create_request — network.create RPC 진입점
 * @params: {bridge_name, mode?, cidr?, physical_if?, dns_enabled?, upstream_dns?}
 * @rpc_id: JSON-RPC 요청 ID
 * @server: UDS 서버 참조
 * @connection: 클라이언트 소켓 연결
 *
 * fire-and-forget 패턴:
 *   1) 파라미터 검증 (pcv_validate_network_create_params)
 *   2) 중복 생성 방어 (g_net_inflight 해시테이블 확인)
 *   3) GTask 워커 스레드 실행 (network_create_worker)
 *   → 응답은 network_action_callback에서 전송
 *
 * [주의] 이 함수에서 send_response를 호출하지 않음.
 * 콜백에서 전송하는 패턴 (소켓이 아직 열려있는 상태).
 */
void handle_network_create_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    if (!params || !json_object_has_member(params, "bridge_name")) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32602, "Missing parameter: bridge_name");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
    }

    /* [Fix #1 / Fix #6 / E1] 파라미터 추출 후 통합 검증 */
    const gchar *br_name  = json_object_get_string_member(params, "bridge_name");
    const gchar *mode_raw = json_object_has_member(params, "mode")
                            ? json_object_get_string_member(params, "mode") : NULL;
    const gchar *cidr_raw = json_object_has_member(params, "cidr")
                            ? json_object_get_string_member(params, "cidr") : NULL;
    const gchar *phys_raw = json_object_has_member(params, "physical_if")
                            ? json_object_get_string_member(params, "physical_if") : NULL;

    GError *validate_err = NULL;
    if (!pcv_validate_network_create_params(br_name, mode_raw, cidr_raw, phys_raw, &validate_err)) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32602,
            validate_err ? validate_err->message : "Invalid parameters");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        if (validate_err) g_error_free(validate_err);
        return;
    }

    NetworkCtx *ctx = g_new0(NetworkCtx, 1);
    ctx->bridge_name  = g_strdup(br_name);
    if (cidr_raw)  ctx->cidr         = g_strdup(cidr_raw);
    if (mode_raw)  ctx->mode         = g_strdup(mode_raw);
    if (phys_raw)  ctx->physical_if  = g_strdup(phys_raw);
    /* [S-4] MTU 설정 (기본 1500, 미지정 시 0 → network_bridge_create에서 1500 적용)
     * B4-W5 (Phase 4): 범위 검증 — IEEE 802.3 최소 68, jumbo 9000 초과 거부 */
    ctx->mtu = json_object_has_member(params, "mtu")
               ? (gint)json_object_get_int_member(params, "mtu") : 0;
    if (ctx->mtu != 0 && (ctx->mtu < 68 || ctx->mtu > 9216)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
            "Invalid mtu — must be between 68 and 9216 (or 0 for default 1500)");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        free_network_ctx(ctx);
        return;
    }
    /* [E5] DNS 활성화 옵션 */
    ctx->dns_enabled  = json_object_has_member(params, "dns_enabled")
                        && json_object_get_boolean_member(params, "dns_enabled");
    if (json_object_has_member(params, "upstream_dns"))
        ctx->upstream_dns = g_strdup(json_object_get_string_member(params, "upstream_dns"));
    /* IPv6 RA+DHCPv6: 선택 파라미터 */
    if (json_object_has_member(params, "ipv6_prefix"))
        ctx->ipv6_prefix = g_strdup(json_object_get_string_member(params, "ipv6_prefix"));

    /* [V4] ipv6_prefix RCE 방어: 검증 없이 사용되면 dnsmasq .conf 인젝션 →
     * dhcp-script 를 통한 root RCE. 화이트리스트 검증 실패 시 즉시 거부한다.
     * (network_dhcp_start_v6 및 아래 v6 gw-build 에도 방어적 이중 검증 존재.) */
    if (ctx->ipv6_prefix && ctx->ipv6_prefix[0]
        && !pcv_validate_ipv6_prefix(ctx->ipv6_prefix)) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32602,
            "Invalid ipv6_prefix — must be <ipv6-literal>/<0..128>, no spaces/newlines");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        free_network_ctx(ctx);
        return;
    }

    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

    /* [E6] 동시 생성 요청 중복 방어 */
    _net_inflight_init_once();
    g_mutex_lock(&g_net_inflight_mu);
    if (g_hash_table_contains(g_net_inflight, br_name)) {
        g_mutex_unlock(&g_net_inflight_mu);
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32000,
            "Network creation already in progress for this bridge");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        free_network_ctx(ctx);
        return;
    }
    g_hash_table_insert(g_net_inflight, g_strdup(br_name), GINT_TO_POINTER(TRUE));
    g_mutex_unlock(&g_net_inflight_mu);

    GTask *task = g_task_new(NULL, NULL, network_action_callback, ctx);

    // 🚀 [중요] 여기에도 청소부를 연결해 줍니다!
    g_task_set_task_data(task, ctx, free_network_ctx);

    // 🚀 [추가] 이것은 생성(Create) 워커임을 명시적으로 태깅
    g_object_set_data(G_OBJECT(task), "is_delete", GINT_TO_POINTER(FALSE));
    
    g_task_run_in_thread(task, network_create_worker);
    g_object_unref(task);
}

/**
 * handle_network_delete_request — network.delete RPC 진입점
 * @params: {bridge_name}
 * @rpc_id: JSON-RPC 요청 ID
 * @server: UDS 서버 참조
 * @connection: 클라이언트 소켓 연결
 *
 * GTask 워커(network_delete_worker)에서 비동기 삭제 수행.
 * 응답은 network_action_callback에서 전송.
 */
void handle_network_delete_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    if (!params || !json_object_has_member(params, "bridge_name")) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32602, "Missing parameter: bridge_name");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
    }

    const gchar *br = json_object_get_string_member(params, "bridge_name");

    /* [V11] bridge_name 을 sysfs 경로(/sys/class/net/<br>/brif) 구성 및 argv
     * spawn 이전에 화이트리스트 검증 (defense-in-depth). */
    if (!pcv_validate_bridge_name(br)) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32602,
            "Invalid bridge name");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
    }

    /* B4-M10 (Phase 4): 활성 VM 슬레이브 검사 — 사용 중인 브릿지는 삭제 거부.
     * /sys/class/net/<br>/brif 에 vnet* 엔트리가 있으면 VM이 연결되어 있는 것. */
    {
        gboolean force = json_object_has_member(params, "force")
                       ? json_object_get_boolean_member(params, "force") : FALSE;
        if (!force) {
            gchar *brif_dir = g_strdup_printf("/sys/class/net/%s/brif", br);
            GDir *d = g_dir_open(brif_dir, 0, NULL);
            g_free(brif_dir);
            if (d) {
                const gchar *entry;
                gboolean has_vnet = FALSE;
                while ((entry = g_dir_read_name(d))) {
                    if (g_str_has_prefix(entry, "vnet")) { has_vnet = TRUE; break; }
                }
                g_dir_close(d);
                if (has_vnet) {
                    gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32000,
                        "Bridge has active VM interfaces — stop VMs or pass force=true");
                    pure_uds_server_send_response(server, connection, err_resp);
                    g_free(err_resp);
                    return;
                }
            }
        }
    }

    NetworkCtx *ctx = g_new0(NetworkCtx, 1);
    ctx->bridge_name = g_strdup(br);
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

    GTask *task = g_task_new(NULL, NULL, network_action_callback, ctx);

    // 🚀 [중요] 여기에도 청소부를 연결해 줍니다!
    g_task_set_task_data(task, ctx, free_network_ctx);

    // 🚀 [추가] 이것은 삭제(Delete) 워커임을 명시적으로 태깅
    g_object_set_data(G_OBJECT(task), "is_delete", GINT_TO_POINTER(TRUE));
    
    g_task_run_in_thread(task, network_delete_worker);
    g_object_unref(task);
}
/* ═══════════════════════════════════════════════════════════════
 * Sprint F: 네트워크 관리 강화
 *
 * handle_network_list_request  — 시스템 브릿지 목록 + 상태 조회
 * handle_network_info_request  — 특정 브릿지 상세 정보 (IP, slave, 상태)
 * ═══════════════════════════════════════════════════════════════ */

#include "../../utils/pcv_log.h"
#include <net/if.h>

#define NET_LOG_DOM "network"

/* ── 내부 헬퍼: ip link show type bridge 파싱 ─────────────── */

/**
 * _read_bridge_slaves — 브릿지에 연결된 슬레이브 인터페이스 목록 반환
 * @bridge_name: 브릿지 이름
 *
 * /sys/class/net/<br>/brif/ 디렉토리를 읽어 슬레이브 인터페이스 이름을
 * JsonArray에 담아 반환. vnetN(VM), tapN, vethN 등 모든 슬레이브 포함.
 * 디렉토리 미존재 시 빈 배열 반환.
 *
 * @return (transfer full): 문자열 배열 ["vnet0", "eno1", ...]
 */
static JsonArray *_read_bridge_slaves(const gchar *bridge_name)
{
    JsonArray *arr  = json_array_new();
    gchar     *path = g_strdup_printf("/sys/class/net/%s/brif", bridge_name);
    GDir      *dir  = g_dir_open(path, 0, NULL);

    if (dir) {
        const gchar *entry;
        while ((entry = g_dir_read_name(dir)) != NULL)
            json_array_add_string_element(arr, entry);
        g_dir_close(dir);
    }

    g_free(path);
    return arr;
}

/**
 * _bridge_carrier — 브릿지 인터페이스의 링크 상태 반환
 * @bridge_name: 브릿지 이름
 *
 * /sys/class/net/<br>/carrier 파일을 읽어 "up"/"down"/"unknown" 반환.
 * carrier=1이면 링크 활성, 0이면 비활성.
 *
 * [주의] 반환값은 정적 문자열 — g_free() 금지.
 *
 * @return "up", "down", 또는 "unknown"
 */
static const gchar *_bridge_carrier(const gchar *bridge_name)
{
    gchar *path = g_strdup_printf("/sys/class/net/%s/carrier", bridge_name);
    gchar *content = NULL;
    gboolean ok = g_file_get_contents(path, &content, NULL, NULL);
    g_free(path);

    const gchar *state = "unknown";
    if (ok) {
        g_strstrip(content);
        state = (content[0] == '1') ? "up" : "down";
        g_free(content);
    }
    return state;
}

/**
 * _pid_file_process_alive — PID 파일이 가리키는 프로세스 생존 여부 확인
 *
 * [왜 별도 함수인가?]
 *   PureCVisor 자체 dnsmasq와 libvirt dnsmasq 모두 "PID 파일 → /proc/<pid>"
 *   패턴으로 실행 상태를 확인합니다. 한 곳에 모아두면 stale PID 파일 때문에
 *   DHCP가 켜진 것으로 오판하는 회귀를 줄일 수 있습니다.
 */
static gboolean
_pid_file_process_alive(const gchar *pid_path)
{
    gchar *pid_str = NULL;
    gboolean alive = FALSE;

    if (!pid_path || !g_file_get_contents(pid_path, &pid_str, NULL, NULL)) {
        return FALSE;
    }

    g_strstrip(pid_str);
    if (pid_str[0] != '\0') {
        gchar *proc_path = g_strdup_printf("/proc/%s/status", pid_str);
        alive = g_file_test(proc_path, G_FILE_TEST_EXISTS);
        g_free(proc_path);
    }

    g_free(pid_str);
    return alive;
}

/**
 * _libvirt_dhcp_active_for_bridge — libvirt 기본 NAT DHCP 상태 감지
 *
 * libvirt의 default 네트워크는 PureCVisor가 만든 dnsmasq PID 파일을 쓰지 않고
 * /var/lib/libvirt/dnsmasq/<network>.conf|pid 형태로 별도 관리합니다.
 * 이 경로를 보지 않으면 실제 virbr0 DHCP가 켜져 있어도 Web 인벤토리에서
 * OFF로 표시됩니다.
 */
static gboolean
_libvirt_dhcp_active_for_bridge(const gchar *bridge_name)
{
    GDir *dir = NULL;
    const gchar *entry = NULL;
    gboolean active = FALSE;

    if (!bridge_name || bridge_name[0] == '\0') {
        return FALSE;
    }

    dir = g_dir_open("/var/lib/libvirt/dnsmasq", 0, NULL);
    if (!dir) {
        return FALSE;
    }

    while ((entry = g_dir_read_name(dir)) != NULL) {
        if (!g_str_has_suffix(entry, ".conf")) {
            continue;
        }

        gchar *conf_path = g_build_filename("/var/lib/libvirt/dnsmasq", entry, NULL);
        gchar *content = NULL;
        if (!g_file_get_contents(conf_path, &content, NULL, NULL) || !content) {
            g_free(conf_path);
            continue;
        }

        gchar *iface_line = g_strdup_printf("interface=%s\n", bridge_name);
        gboolean matches_bridge = strstr(content, iface_line) != NULL;
        gboolean has_dhcp_range = strstr(content, "\ndhcp-range=") != NULL ||
                                  g_str_has_prefix(content, "dhcp-range=");
        g_free(iface_line);
        g_free(content);

        if (matches_bridge && has_dhcp_range) {
            gchar *network_name = g_strndup(entry, strlen(entry) - strlen(".conf"));
            gchar *pid_file = g_strdup_printf("%s.pid", network_name);
            gchar *pid_path = g_build_filename("/var/lib/libvirt/dnsmasq", pid_file, NULL);
            active = _pid_file_process_alive(pid_path);
            g_free(pid_path);
            g_free(pid_file);
            g_free(network_name);
        }

        g_free(conf_path);
        if (active) {
            break;
        }
    }

    g_dir_close(dir);
    return active;
}

/**
 * _dhcp_socket_active_for_bridge — UDP/67 리스닝 상태 보조 확인
 *
 * libvirt 버전이나 배포판에 따라 dnsmasq 파일 배치가 다를 수 있으므로,
 * 마지막 안전망으로 ss 출력에서 해당 브릿지에 바인딩된 DHCP 서버를 확인합니다.
 * 예: "0.0.0.0%virbr0:67".
 */
static gboolean
_dhcp_socket_active_for_bridge(const gchar *bridge_name)
{
    const gchar *argv[] = {"ss", "-lun", NULL};
    gchar *stdout_buf = NULL;
    gboolean active = FALSE;

    if (!bridge_name || bridge_name[0] == '\0') {
        return FALSE;
    }

    if (!pcv_spawn_sync(argv, &stdout_buf, NULL, NULL) || !stdout_buf) {
        g_free(stdout_buf);
        return FALSE;
    }

    gchar *needle = g_strdup_printf("%%%s:67", bridge_name);
    active = strstr(stdout_buf, needle) != NULL;
    g_free(needle);
    g_free(stdout_buf);
    return active;
}

/**
 * _network_dhcp_active — 브릿지 DHCP 활성 상태 통합 판정
 *
 * 판정 순서:
 *   1. PureCVisor가 생성한 dnsmasq PID 파일
 *   2. libvirt가 관리하는 dnsmasq PID/config 파일
 *   3. 실제 UDP 67 리스닝 소켓
 *
 * 이렇게 해야 자체 생성 네트워크와 libvirt 기본 NAT 네트워크(virbr0)를
 * 같은 network.list 응답에서 일관되게 표시할 수 있습니다.
 */
static gboolean
_network_dhcp_active(const gchar *bridge_name)
{
    gchar *pid_path = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.pid", bridge_name);
    gboolean active = _pid_file_process_alive(pid_path);
    g_free(pid_path);

    if (active) {
        return TRUE;
    }

    if (_libvirt_dhcp_active_for_bridge(bridge_name)) {
        return TRUE;
    }

    return _dhcp_socket_active_for_bridge(bridge_name);
}

/**
 * _get_bridge_ip — 브릿지의 IPv4 주소/CIDR 추출
 * @bridge_name: 브릿지 이름
 *
 * ip -4 addr show dev <br> 출력에서 "inet X.X.X.X/XX" 패턴을 파싱하여
 * 첫 번째 IPv4 주소를 반환. 주소 미할당 시 빈 문자열("") 반환.
 *
 * @return (transfer full): "192.0.2.10/24" 형태 (호출자 g_free)
 */
static gchar *_get_bridge_ip(const gchar *bridge_name)
{
    const gchar *argv[] = {"ip", "-4", "addr", "show",
                            "dev", bridge_name, NULL};
    gchar *stdout_buf = NULL;
    pcv_spawn_sync(argv, &stdout_buf, NULL, NULL);
    if (!stdout_buf) return g_strdup("");

    /* "inet 192.168.X.X/24" 패턴에서 주소 추출 */
    const gchar *hit = strstr(stdout_buf, "inet ");
    gchar *result = g_strdup("");
    if (hit) {
        hit += 5;
        const gchar *end = strchr(hit, ' ');
        if (!end) end = strchr(hit, '\n');
        if (end) {
            g_free(result);
            result = g_strndup(hit, end - hit);
        }
    }
    g_free(stdout_buf);
    return result;
}

/**
 * handle_network_list_request — network.list RPC 진입점
 * @params: (unused) 파라미터 없음
 * @rpc_id: JSON-RPC 요청 ID
 * @server: UDS 서버 참조
 * @connection: 클라이언트 소켓 연결
 *
 * 시스템 전체 브릿지 목록을 조회하여 JSON 배열로 반환.
 *
 * 각 브릿지 객체 구조:
 *   {name, state, ip_cidr, mode, dhcp, phys, subnet, slaves:[...]}
 *
 * [동작 흐름]
 *   1) ip -o link show type bridge 실행 → 브릿지 이름 목록 추출
 *   2) 각 브릿지에 대해:
 *      - _get_bridge_ip(): IP/CIDR 조회
 *      - _read_bridge_slaves(): 슬레이브 인터페이스 목록
 *      - _network_meta_get_mode(): 네트워크 모드 판별 (meta 파일 또는 이름 패턴)
 *      - PureCVisor/libvirt dnsmasq와 UDP 67 리스너로 DHCP 동작 여부 확인
 *      - brif/ 디렉토리에서 물리 업링크 NIC 감지
 *      - IP/CIDR에서 서브넷 주소 계산 (호스트비트 제거)
 */
void handle_network_list_request(JsonObject *params __attribute__((unused)),
                                  const gchar *rpc_id,
                                  UdsServer *server,
                                  GSocketConnection *connection)
{
    const gchar *argv[] = {"ip", "-o", "link", "show",
                            "type", "bridge", NULL};
    gchar *stdout_buf = NULL;
    GError *err = NULL;

    if (!pcv_spawn_sync(argv, &stdout_buf, NULL, &err)) {
        const gchar *msg = err ? err->message : "ip link failed";
        gchar *resp = pure_rpc_build_error_response(rpc_id, -32000, msg);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        if (err) g_error_free(err);
        return;
    }

    JsonArray *bridges = json_array_new();

    /* ip -o link 출력: "N: brname: <FLAGS> ..." (한 줄 per 인터페이스) */
    gchar **lines = g_strsplit(stdout_buf ? g_strstrip(stdout_buf) : "", "\n", -1);
    for (gchar **l = lines; *l; l++) {
        if (!**l) continue;

        /* 인터페이스 이름 추출: "N: NAME: ..." */
        const gchar *colon = strchr(*l, ':');
        if (!colon) continue;
        colon++; /* skip first colon (index) */
        while (*colon == ' ') colon++;
        const gchar *name_end = strchr(colon, ':');
        if (!name_end) continue;
        gchar *br_name = g_strndup(colon, name_end - colon);
        g_strstrip(br_name);

        /* @ 이후 제거 (e.g. "br0@eth0" → "br0") */
        gchar *at = strchr(br_name, '@');
        if (at) *at = '\0';

        /* UP/DOWN 상태 */
        gboolean is_up = (strstr(*l, "UP") != NULL);

        /* IP 주소 */
        gchar     *ip_cidr = _get_bridge_ip(br_name);
        JsonArray *slaves  = _read_bridge_slaves(br_name);

        /* [E3] meta 파일에서 mode 읽기 */
        gchar *br_mode = _network_meta_get_mode(br_name);

        /* ── K1: DHCP 동작 여부
         * PureCVisor 자체 dnsmasq뿐 아니라 libvirt 기본 NAT(virbr0)도 포함해
         * 실제 게스트가 DHCP를 받을 수 있는지를 기준으로 표시합니다. */
        gboolean dhcp_on = _network_dhcp_active(br_name);

        /* ── K1: 물리 업링크 — /sys/class/net/<br>/brif/ 에서 비-vnet 인터페이스 추출 ── */
        gchar *phys_uplink = g_strdup("-");
        {
            gchar *brif_path = g_strdup_printf("/sys/class/net/%s/brif", br_name);
            GDir  *brif_dir  = g_dir_open(brif_path, 0, NULL);
            if (brif_dir) {
                const gchar *ifname;
                while ((ifname = g_dir_read_name(brif_dir)) != NULL) {
                    /* vnet*, tap* 는 VM 가상 인터페이스 → 제외 */
                    if (!g_str_has_prefix(ifname, "vnet") &&
                        !g_str_has_prefix(ifname, "tap")  &&
                        !g_str_has_prefix(ifname, "veth")) {
                        g_free(phys_uplink);
                        phys_uplink = g_strdup(ifname);
                        break;
                    }
                }
                g_dir_close(brif_dir);
            }
            g_free(brif_path);
        }

        /* ── K1: 서브넷 주소 — ip_cidr 에서 호스트비트 제거 ──
         * 예: "10.10.0.1/24" → "10.10.0.0/24"
         * [알고리즘]
         *   1) IP를 옥텟으로 분리 → 32비트 정수로 변환
         *   2) prefix 길이로 서브넷 마스크 생성: ~0u << (32 - prefix)
         *      예: /24 → 0xFFFFFF00 (상위 24비트 1)
         *   3) IP & 마스크 = 서브넷 주소 (호스트비트 제거) */
        gchar *subnet = g_strdup("-");
        if (ip_cidr && ip_cidr[0] && g_strcmp0(ip_cidr, "") != 0) {
            gchar **cidr_parts = g_strsplit(ip_cidr, "/", 2);
            if (cidr_parts[0] && cidr_parts[1]) {
                gchar **octets = g_strsplit(cidr_parts[0], ".", 4);
                int prefix = atoi(cidr_parts[1]);
                if (octets[0]&&octets[1]&&octets[2]&&octets[3]) {
                    /* 4개 옥텟을 32비트 정수로 합침 (네트워크 바이트 순서) */
                    unsigned int addr =
                        ((unsigned)atoi(octets[0]) << 24) |
                        ((unsigned)atoi(octets[1]) << 16) |
                        ((unsigned)atoi(octets[2]) <<  8) |
                         (unsigned)atoi(octets[3]);
                    /* 서브넷 마스크 생성: prefix 길이만큼 상위 비트를 1로 채움 */
                    unsigned int mask = (prefix == 0) ? 0 : (~0u << (32 - prefix));
                    /* 비트 AND로 호스트 부분을 0으로 만들어 서브넷 주소 추출 */
                    unsigned int net  = addr & mask;
                    g_free(subnet);
                    subnet = g_strdup_printf("%u.%u.%u.%u/%d",
                        (net>>24)&0xFF, (net>>16)&0xFF,
                        (net>>8)&0xFF,  net&0xFF, prefix);
                }
                g_strfreev(octets);
            }
            g_strfreev(cidr_parts);
        }

        JsonObject *obj = json_object_new();
        json_object_set_string_member (obj, "name",       br_name);
        json_object_set_string_member (obj, "state",      is_up ? "up" : "down");
        json_object_set_string_member (obj, "ip_cidr",    ip_cidr);
        json_object_set_string_member (obj, "mode",       br_mode);
        json_object_set_boolean_member(obj, "dhcp",       dhcp_on);
        json_object_set_string_member (obj, "phys",       phys_uplink);
        json_object_set_string_member (obj, "subnet",     subnet);
        g_free(phys_uplink);
        g_free(subnet);
        g_free(br_mode);

        JsonNode *slaves_node = json_node_new(JSON_NODE_ARRAY);
        json_node_take_array(slaves_node, slaves);
        json_object_set_member(obj, "slaves", slaves_node);

        JsonNode *node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(node, obj);
        json_array_add_element(bridges, node);

        g_free(br_name);
        g_free(ip_cidr);
    }

    g_strfreev(lines);
    g_free(stdout_buf);

    JsonNode *result = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(result, bridges);
    gchar *resp = pure_rpc_build_success_response(rpc_id, result);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

/**
 * handle_network_info_request — network.info RPC 진입점
 * @params: {bridge_name}
 * @rpc_id: JSON-RPC 요청 ID
 * @server: UDS 서버 참조
 * @connection: 클라이언트 소켓 연결
 *
 * 단일 브릿지의 상세 정보 반환:
 *   {name, state, ip_cidr, mode, dhcp_active, slave_count, slaves:[...]}
 *
 * /sys/class/net/<br> 디렉토리 존재 여부로 브릿지 존재 확인.
 * 미존재 시 "Bridge not found" 에러 반환.
 */
void handle_network_info_request(JsonObject *params,
                                  const gchar *rpc_id,
                                  UdsServer *server,
                                  GSocketConnection *connection)
{
    if (!params || !json_object_has_member(params, "bridge_name")) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, -32602,
                          "Missing parameter: bridge_name");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    const gchar *br_name = json_object_get_string_member(params,
                               "bridge_name");

    /* 존재 여부 확인 */
    gchar *sys_path = g_strdup_printf("/sys/class/net/%s", br_name);
    gboolean exists = g_file_test(sys_path, G_FILE_TEST_IS_DIR);
    g_free(sys_path);

    if (!exists) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, -32000,
                          "Bridge not found");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    gchar     *ip_cidr  = _get_bridge_ip(br_name);
    JsonArray *slaves   = _read_bridge_slaves(br_name);
    const gchar *carrier = _bridge_carrier(br_name);

    /* DHCP 활성 여부.
     * 리스 파일 존재만으로 판단하면 아직 lease가 없거나 libvirt가 관리하는
     * virbr0 DHCP를 OFF로 오판하므로 network.list와 같은 통합 판정을 사용합니다. */
    gboolean has_dhcp = _network_dhcp_active(br_name);

    /* slave 수 */
    guint slave_count = json_array_get_length(slaves);

    /* [E3] meta에서 mode 읽기 */
    gchar *info_mode = _network_meta_get_mode(br_name);

    JsonObject *info = json_object_new();
    json_object_set_string_member(info, "name",        br_name);
    json_object_set_string_member(info, "state",       carrier);
    json_object_set_string_member(info, "ip_cidr",     ip_cidr);
    json_object_set_string_member(info, "mode",        info_mode);
    json_object_set_boolean_member(info, "dhcp_active", has_dhcp);
    json_object_set_int_member(info, "slave_count", (gint64)slave_count);
    g_free(info_mode);

    JsonNode *slaves_node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(slaves_node, slaves);
    json_object_set_member(info, "slaves", slaves_node);

    JsonNode *result = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(result, info);
    gchar *resp = pure_rpc_build_success_response(rpc_id, result);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
    g_free(ip_cidr);

    PCV_LOG_INFO(NET_LOG_DOM, "network.info: %s → %s slaves=%u",
                 br_name, carrier, slave_count);
}

/* ═══════════════════════════════════════════════════════════════
 * Sprint G: handle_network_mode_set_request
 *
 *   JSON-RPC: network.mode_set
 *   params:   { "name": "br-dev", "mode": "isolated" }
 *   modes:    nat | isolated | routed
 *
 *   동작: 기존 규칙 teardown → 새 모드 규칙 적용
 *         DHCP는 재기동하지 않음 (이미 실행 중 가정)
 *         mode=routed 이면 DHCP도 중단 (dnsmasq pkill)
 * ═══════════════════════════════════════════════════════════════ */
void handle_network_mode_set_request(JsonObject *params, const gchar *rpc_id,
                                     UdsServer *server,
                                     GSocketConnection *connection) {


    if (!params
        || !json_object_has_member(params, "name")
        || !json_object_has_member(params, "mode")) {
        gchar *e = pure_rpc_build_error_response(rpc_id, -32602,
            "Missing parameters: name, mode");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }

    const gchar *br   = json_object_get_string_member(params, "name");
    const gchar *mode = json_object_get_string_member(params, "mode");
    const gchar *cidr = json_object_has_member(params, "cidr")
                        ? json_object_get_string_member(params, "cidr") : NULL;

    /* [V10] bridge_name 을 어떤 경로(PID 파일 등) 구성이나 사용보다 먼저 검증.
     * routed 분기(아래)가 검증 전에 dnsmasq-<br>.pid 경로를 구성해 pkill -F 로
     * path traversal 을 유발할 수 있으므로, 핸들러 최상단에서 화이트리스트 검증. */
    if (!pcv_validate_bridge_name(br)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, -32602,
            "Invalid bridge name");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }

    /* 유효 모드 검증 */
    if (g_strcmp0(mode, "nat")      != 0 &&
        g_strcmp0(mode, "isolated") != 0 &&
        g_strcmp0(mode, "routed")   != 0) {
        gchar *e = pure_rpc_build_error_response(rpc_id, -32602,
            "Invalid mode: must be nat | isolated | routed");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }

    /* cidr 필수 (nat/isolated/routed 전부) */
    if (!cidr || strlen(cidr) == 0) {
        /* cidr 없으면 /sys/class/net/<br>/인터페이스에서 시도 */
        /* 단순화: 오류 반환 */
        gchar *e = pure_rpc_build_error_response(rpc_id, -32602,
            "Missing parameter: cidr (required for mode change)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }

    /* [V8] cidr 은 network_firewall_setup_* → _cidr_to_subnet → nft argv 로
     * 흘러가므로, network.create 와 동일하게 사설 대역 화이트리스트로 검증해
     * nft 규칙 인젝션을 차단한다. */
    if (!pcv_validate_private_cidr(cidr)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, -32602,
            "Invalid cidr — must be a private CIDR (RFC1918/RFC6598/fc00::/7)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }

    GError *error = NULL;

    /* 1. 기존 규칙 teardown */
    network_firewall_teardown(br, NULL);

    /* 2. 새 모드 규칙 적용 */
    gboolean ok = FALSE;
    if      (g_strcmp0(mode, "nat")      == 0) ok = network_firewall_setup_nat     (br, cidr, &error);
    else if (g_strcmp0(mode, "isolated") == 0) ok = network_firewall_setup_isolated(br, cidr, &error);
    else if (g_strcmp0(mode, "routed")   == 0) {
        /* routed 모드: DHCP 불필요 → dnsmasq 중단 */
        gchar *pid = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.pid", br);  /* [E1] */
        const gchar *kill_a[] = {"pkill", "-F", pid, NULL};
        pcv_spawn_fire(kill_a);
        g_free(pid);
        ok = network_firewall_setup_routed(br, cidr, &error);
    }

    if (!ok) {
        const gchar *msg = error ? error->message : "firewall setup failed";
        gchar *e = pure_rpc_build_error_response(rpc_id, -32000, msg);
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        if (error) g_error_free(error);
        return;
    }

    /* [E2] mode 전환 후 DHCP 상태 일관성 보장.
     * 예) routed(DHCP 없음) → nat 전환 시 dnsmasq가 죽어있으면 재기동.
     * 반대로 nat → routed 는 위에서 이미 pkill 처리됨. */
    if (ok && (g_strcmp0(mode, "nat") == 0 || g_strcmp0(mode, "isolated") == 0)) {
        gchar    *pid_chk = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.pid", br);
        gboolean  dhcp_alive = g_file_test(pid_chk, G_FILE_TEST_EXISTS);
        g_free(pid_chk);
        if (!dhcp_alive) {
            GError *dhcp_err = NULL;
            if (!network_dhcp_start(br, cidr, &dhcp_err)) {
                /* DHCP soft-fail: 경고만 로그, 모드 전환 자체는 성공 */
                PCV_LOG_INFO(NET_LOG_DOM,
                    "network.mode_set: DHCP restart soft-fail for %s: %s",
                    br, dhcp_err ? dhcp_err->message : "unknown");
                if (dhcp_err) g_error_free(dhcp_err);
            }
        }
    }

    /* [E3] mode 전환 후 meta 파일 업데이트 */
    _network_meta_save(br, mode, cidr);

    /* 3. 성공 응답 */
    JsonObject *res_obj = json_object_new();
    json_object_set_string_member(res_obj, "bridge", br);
    json_object_set_string_member(res_obj, "mode",   mode);
    JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(res_node, res_obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    PCV_LOG_INFO(NET_LOG_DOM, "network.mode_set: %s → %s", br, mode);
}

/* ══════════════════════════════════════════════════════════════════════════
 * [P0-Fix#4] handle_network_bind_phys_request
 *   RPC: network.bind_phys
 *   params: { "bridge": "pcvbr0", "iface": "eno1" }
 *
 *   물리 NIC을 지정 브릿지의 업링크(슬레이브)로 바인딩합니다.
 *   내부적으로 기존 network_bridge_bind_physical() 헬퍼를 재사용합니다.
 * ══════════════════════════════════════════════════════════════════════════ */
void handle_network_bind_phys_request(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection)
{
    const gchar *br    = json_object_has_member(params, "bridge")
                         ? json_object_get_string_member(params, "bridge") : NULL;
    const gchar *iface = json_object_has_member(params, "iface")
                         ? json_object_get_string_member(params, "iface")  : NULL;

    if (!br || !strlen(br) || !iface || !strlen(iface)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, -32602,
                       "Missing params: 'bridge' and 'iface' are required");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }

    /* [V11] bridge/iface 를 argv spawn(ip link ...) 이전에 화이트리스트 검증. */
    if (!pcv_validate_bridge_name(br) || !pcv_validate_iface_name(iface)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, -32602,
                       "Invalid bridge or iface name");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }

    GError *error = NULL;
    if (!network_bridge_bind_physical(br, iface, &error)) {
        const gchar *msg = error ? error->message : "bind failed";
        gchar *e = pure_rpc_build_error_response(rpc_id, -32000, msg);
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        if (error) g_error_free(error);
        return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "bridge", br);
    json_object_set_string_member(res, "iface",  iface);
    json_object_set_string_member(res, "status", "bound");
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    PCV_LOG_INFO(NET_LOG_DOM, "network.bind_phys: %s → %s", iface, br);
}

/* ══════════════════════════════════════════════════════════════════════════
 * [P0-Fix#4] handle_network_dhcp_toggle_request
 *   RPC: network.dhcp_toggle
 *   params: { "bridge": "pcvbr0", "enable": true|false }
 *
 *   DHCP(dnsmasq)를 브릿지에 대해 시작 또는 중단합니다.
 *   enable=true  → network_dhcp_start() (브릿지 메타에서 CIDR 자동 읽기)
 *   enable=false → pkill -F dnsmasq-<br>.pid
 * ══════════════════════════════════════════════════════════════════════════ */
void handle_network_dhcp_toggle_request(JsonObject *params, const gchar *rpc_id,
                                        UdsServer *server, GSocketConnection *connection)
{
    const gchar *br = json_object_has_member(params, "bridge")
                      ? json_object_get_string_member(params, "bridge") : NULL;
    if (!br || !strlen(br)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, -32602,
                       "Missing param: 'bridge' is required");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }

    /* [V10] bridge_name 을 meta/pid 경로 구성 및 pkill -F 이전에 검증해
     * path traversal(dnsmasq-<br>.pid) 을 차단한다. */
    if (!pcv_validate_bridge_name(br)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, -32602,
                       "Invalid bridge name");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }

    gboolean enable = TRUE;
    if (json_object_has_member(params, "enable"))
        enable = json_object_get_boolean_member(params, "enable");

    if (enable) {
        /* CIDR를 메타 파일에서 읽어 dnsmasq 기동 */
        gchar *meta_path = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.meta", br);
        gchar *cidr = NULL;
        GError *meta_err = NULL;
        gchar  *meta_data = NULL;

        if (g_file_get_contents(meta_path, &meta_data, NULL, &meta_err)) {
            JsonParser *p = json_parser_new();
            if (json_parser_load_from_data(p, meta_data, -1, NULL)) {
                JsonObject *mo = json_node_get_object(json_parser_get_root(p));
                if (json_object_has_member(mo, "cidr"))
                    cidr = g_strdup(json_object_get_string_member(mo, "cidr"));
            }
            g_object_unref(p);
            g_free(meta_data);
        }
        if (meta_err) g_error_free(meta_err);
        g_free(meta_path);

        if (!cidr || !strlen(cidr)) {
            g_free(cidr);
            gchar *e = pure_rpc_build_error_response(rpc_id, -32000,
                           "Cannot read CIDR from bridge meta — set mode first");
            pure_uds_server_send_response(server, connection, e);
            g_free(e);
            return;
        }

        GError *dhcp_err = NULL;
        if (!network_dhcp_start(br, cidr, &dhcp_err)) {
            const gchar *msg = dhcp_err ? dhcp_err->message : "dnsmasq start failed";
            gchar *e = pure_rpc_build_error_response(rpc_id, -32000, msg);
            pure_uds_server_send_response(server, connection, e);
            g_free(e);
            if (dhcp_err) g_error_free(dhcp_err);
            g_free(cidr);
            return;
        }
        g_free(cidr);
        PCV_LOG_INFO(NET_LOG_DOM, "network.dhcp_toggle: DHCP started on %s", br);
    } else {
        /* dnsmasq 중단 */
        gchar *pid_path = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.pid", br);
        const gchar *kill_argv[] = {"pkill", "-F", pid_path, NULL};
        pcv_spawn_fire(kill_argv);
        g_free(pid_path);
        PCV_LOG_INFO(NET_LOG_DOM, "network.dhcp_toggle: DHCP stopped on %s", br);
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "bridge", br);
    json_object_set_boolean_member(res, "dhcp",  enable);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

/* =================================================================
 * Phase T-1: OVS 드라이버 — ovs-vsctl 래퍼
 *
 * [OVS(Open vSwitch)란?]
 *   소프트웨어 가상 스위치로, Linux Bridge보다 고급 기능을 제공합니다.
 *   VXLAN 터널, QoS, 미러링, OpenFlow 등을 지원합니다.
 *   PureCVisor에서는 3노드 VXLAN 오버레이 네트워크에 사용됩니다.
 *
 * [OVS 자동 감지 (vm.create 시)]
 *   VM 생성 시 network_bridge 파라미터가 OVS 브릿지인지 확인합니다:
 *     ovs-vsctl br-exists <bridge> → 종료 코드 0이면 OVS 브릿지
 *   OVS 브릿지이면 VM XML에 <virtualport type='openvswitch'/>를 자동 추가합니다.
 *   이것이 없으면 VM이 OVS 브릿지에서 네트워크 연결이 안 됩니다.
 *   (실전 배포 중 OVS VM 시작 실패 버그의 원인이었음)
 *
 * [멱등 명령어]
 *   --may-exist (add-br): 이미 존재하면 에러 없이 성공
 *   --if-exists (del-br/del-port): 없으면 에러 없이 성공
 * ================================================================= */

/**
 * handle_network_ovs_create_request — network.ovs.create RPC 진입점
 * @params: {bridge} OVS 브릿지 이름
 * @rpc_id: JSON-RPC 요청 ID
 * @server: UDS 서버 참조
 * @connection: 클라이언트 소켓 연결
 *
 * ovs-vsctl --may-exist add-br 로 OVS 브릿지를 생성하고 활성화.
 * --may-exist 플래그로 이미 존재하는 브릿지는 에러 없이 성공 처리 (멱등).
 */
void handle_network_ovs_create_request(JsonObject *params, const gchar *rpc_id,
                                       UdsServer *server, GSocketConnection *connection)
{
    const gchar *br = json_object_has_member(params, "bridge")
        ? json_object_get_string_member(params, "bridge") : NULL;
    if (!br || br[0] == '\0') {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
            "Missing parameter: bridge");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    /* [V11] bridge_name 을 ovs-vsctl argv 이전에 화이트리스트 검증. */
    if (!pcv_validate_bridge_name(br)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
            "Invalid bridge name");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    const gchar *argv[] = {"ovs-vsctl", "--may-exist", "add-br", br, NULL};
    gchar *std_err = NULL;
    GError *error = NULL;

    if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
        gchar *msg = g_strdup_printf("OVS bridge create failed: %s",
            error ? error->message : (std_err ? std_err : "unknown"));
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32000, msg);
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp); g_free(msg);
        if (error) g_error_free(error);
        g_free(std_err); return;
    }
    g_free(std_err);

    /* 브릿지 활성화 */
    const gchar *up_argv[] = {"ip", "link", "set", br, "up", NULL};
    pcv_spawn_sync(up_argv, NULL, NULL, NULL);

    g_message("[OVS] Bridge '%s' created.", br);

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "success");
    json_object_set_string_member(res, "bridge", br);
    json_object_set_string_member(res, "type", "ovs");
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

/**
 * handle_network_ovs_delete_request — network.ovs.delete RPC 진입점
 * @params: {bridge} 삭제할 OVS 브릿지 이름
 * @rpc_id: JSON-RPC 요청 ID
 * @server: UDS 서버 참조
 * @connection: 클라이언트 소켓 연결
 *
 * ovs-vsctl --if-exists del-br 로 OVS 브릿지를 삭제.
 * --if-exists 플래그로 미존재 브릿지도 에러 없이 성공 처리 (멱등).
 */
void handle_network_ovs_delete_request(JsonObject *params, const gchar *rpc_id,
                                       UdsServer *server, GSocketConnection *connection)
{
    const gchar *br = json_object_has_member(params, "bridge")
        ? json_object_get_string_member(params, "bridge") : NULL;
    if (!br || br[0] == '\0') {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
            "Missing parameter: bridge");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    /* [V11] bridge_name 을 ovs-vsctl argv 이전에 화이트리스트 검증. */
    if (!pcv_validate_bridge_name(br)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
            "Invalid bridge name");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    const gchar *argv[] = {"ovs-vsctl", "--if-exists", "del-br", br, NULL};
    gchar *std_err = NULL;
    GError *error = NULL;

    if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
        gchar *msg = g_strdup_printf("OVS bridge delete failed: %s",
            error ? error->message : (std_err ? std_err : "unknown"));
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32000, msg);
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp); g_free(msg);
        if (error) g_error_free(error);
        g_free(std_err); return;
    }
    g_free(std_err);

    g_message("[OVS] Bridge '%s' deleted.", br);

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "success");
    json_object_set_string_member(res, "bridge", br);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

/* ── network.ovs.vxlan.add — VXLAN 포트 추가 ───────────────────── */
/*
 * [VXLAN이란?]
 *   L2 프레임을 UDP(포트 4789)로 캡슐화하여 L3 네트워크 위에 가상 L2 네트워크를
 *   구성하는 터널링 프로토콜입니다. VNI(VXLAN Network Identifier)로 테넌트를 분리합니다.
 *
 *   PureCVisor에서는 3노드 클러스터 간 pcvoverlay0 브릿지에 VXLAN 터널을 추가하여
 *   VM이 노드를 넘어 동일 L2 세그먼트에서 통신할 수 있게 합니다.
 *
 * [ovs-vsctl 명령 형식]
 *   ovs-vsctl --may-exist add-port <br> <port> \
 *     -- set interface <port> type=vxlan options:key=<vni> options:remote_ip=<ip>
 */
void handle_network_ovs_vxlan_add_request(JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *connection)
{
    const gchar *br       = json_object_has_member(params, "bridge")
        ? json_object_get_string_member(params, "bridge") : NULL;
    const gchar *port     = json_object_has_member(params, "port_name")
        ? json_object_get_string_member(params, "port_name") : NULL;
    const gchar *remote   = json_object_has_member(params, "remote_ip")
        ? json_object_get_string_member(params, "remote_ip") : NULL;
    gint64 vni = json_object_has_member(params, "vni")
        ? json_object_get_int_member(params, "vni") : 100;

    if (!br || !port || !remote) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
            "Required: bridge, port_name, remote_ip");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    /* [V11] bridge_name / remote_ip 를 ovs-vsctl argv 이전에 화이트리스트 검증.
     * (sink 는 argv 배열이라 shell 인젝션은 이미 차단되나, defense-in-depth.) */
    if (!pcv_validate_bridge_name(br) || !pcv_validate_ip_literal(remote)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
            "Invalid bridge name or remote_ip");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    gchar *vni_str = g_strdup_printf("%ld", (long)vni);
    const gchar *argv[] = {
        "ovs-vsctl", "--may-exist", "add-port", br, port,
        "--", "set", "interface", port,
        "type=vxlan",
        NULL, NULL, NULL, NULL  /* options 슬롯 */
    };
    /* ovs-vsctl 은 options:key=val 형태로 전달 */
    gchar *opt_key    = g_strdup_printf("options:key=%s", vni_str);
    gchar *opt_remote = g_strdup_printf("options:remote_ip=%s", remote);
    argv[10] = opt_key;
    argv[11] = opt_remote;
    argv[12] = NULL;

    gchar *std_err = NULL;
    GError *error = NULL;

    if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
        gchar *msg = g_strdup_printf("OVS VXLAN add failed: %s",
            error ? error->message : (std_err ? std_err : "unknown"));
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32000, msg);
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp); g_free(msg);
        if (error) g_error_free(error);
        g_free(std_err); g_free(vni_str);
        g_free(opt_key); g_free(opt_remote); return;
    }
    g_free(std_err);

    g_message("[OVS] VXLAN port '%s' added to '%s' (remote=%s, vni=%ld)",
              port, br, remote, (long)vni);

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "success");
    json_object_set_string_member(res, "bridge", br);
    json_object_set_string_member(res, "port", port);
    json_object_set_string_member(res, "remote_ip", remote);
    json_object_set_int_member(res, "vni", vni);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
    g_free(vni_str); g_free(opt_key); g_free(opt_remote);
}

/**
 * handle_network_ovs_vxlan_del_request — network.ovs.vxlan.del RPC 진입점
 * @params: {bridge, port_name}
 * @rpc_id: JSON-RPC 요청 ID
 * @server: UDS 서버 참조
 * @connection: 클라이언트 소켓 연결
 *
 * ovs-vsctl --if-exists del-port 로 VXLAN 터널 포트 삭제 (멱등).
 */
void handle_network_ovs_vxlan_del_request(JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *connection)
{
    const gchar *br   = json_object_has_member(params, "bridge")
        ? json_object_get_string_member(params, "bridge") : NULL;
    const gchar *port = json_object_has_member(params, "port_name")
        ? json_object_get_string_member(params, "port_name") : NULL;

    if (!br || !port) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
            "Required: bridge, port_name");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    /* [V11] bridge_name 을 ovs-vsctl argv 이전에 화이트리스트 검증. */
    if (!pcv_validate_bridge_name(br)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
            "Invalid bridge name");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    const gchar *argv[] = {"ovs-vsctl", "--if-exists", "del-port", br, port, NULL};
    gchar *std_err = NULL;
    GError *error = NULL;

    if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
        gchar *msg = g_strdup_printf("OVS VXLAN del failed: %s",
            error ? error->message : (std_err ? std_err : "unknown"));
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32000, msg);
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp); g_free(msg);
        if (error) g_error_free(error);
        g_free(std_err); return;
    }
    g_free(std_err);

    g_message("[OVS] VXLAN port '%s' removed from '%s'", port, br);

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "success");
    json_object_set_string_member(res, "bridge", br);
    json_object_set_string_member(res, "port", port);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

/* ══════════════════════════════════════════════════════════════════════
 * BE-A18: VM 이름으로 vnet 인터페이스 자동 조회
 *
 * virsh domiflist <vm> 의 출력에서 첫 번째 vnet 인터페이스를 반환한다.
 * VM QoS 설정 시 vm_name 파라미터로 인터페이스를 자동 해석하는 데 사용.
 * ══════════════════════════════════════════════════════════════════════ */
static gchar *
_find_vm_vnet(const gchar *vm_name)
{
    GPtrArray *ifaces = pcv_vm_iface_list(vm_name);
    gchar *vnet = (ifaces->len > 0)
        ? g_strdup(g_ptr_array_index(ifaces, 0)) : NULL;
    g_ptr_array_unref(ifaces);
    return vnet;
}

/* ══════════════════════════════════════════════════════════════════════
 * network.qos.set — tc(traffic control) HTB 대역폭 제한 (동기)
 *
 * [파라미터]
 *   interface   : 타겟 인터페이스 이름 (예: "vnet0", "pcvbr1") — 필수
 *   rate_mbps   : 대역폭 상한 Mbit/s (필수, > 0)
 *   burst_kb    : 버스트 크기 KB (선택, 기본 256)
 *   direction   : "egress" (기본값, HTB) 또는 "ingress" (tc ingress policing)
 *
 * [구현]
 *   egress: HTB(Hierarchical Token Bucket) qdisc를 사용하여
 *           root → class 1:10 으로 대역폭을 제한한다.
 *           replace 명령 사용으로 기존 설정이 있어도 멱등 적용.
 *   ingress: tc ingress qdisc + u32 police 필터로 수신 대역폭 폴리싱.
 *            rate 초과 트래픽은 drop 처리.
 * ══════════════════════════════════════════════════════════════════════ */
void handle_network_qos_set(JsonObject *params, const gchar *rpc_id,
                             UdsServer *server, GSocketConnection *connection)
{
    const gchar *iface = json_object_has_member(params, "interface")
        ? json_object_get_string_member(params, "interface") : NULL;
    gint rate_mbps = json_object_has_member(params, "rate_mbps")
        ? (gint)json_object_get_int_member(params, "rate_mbps") : 0;
    gint burst_kb = json_object_has_member(params, "burst_kb")
        ? (gint)json_object_get_int_member(params, "burst_kb") : 256;
    const gchar *direction = json_object_has_member(params, "direction")
        ? json_object_get_string_member(params, "direction") : "egress";

    /* BE-A18: vm_name으로 vnet 인터페이스 자동 조회 */
    gchar *auto_iface = NULL;
    if ((!iface || !iface[0]) && json_object_has_member(params, "vm_name")) {
        const gchar *vm_name = json_object_get_string_member(params, "vm_name");
        if (vm_name && *vm_name) {
            /* [V11] vm_name 을 virsh domiflist argv 이전에 화이트리스트 검증. */
            if (!pcv_validate_vm_name(vm_name)) {
                gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
                    "Invalid vm_name");
                pure_uds_server_send_response(server, connection, err);
                g_free(err); g_free(auto_iface); return;
            }
            auto_iface = _find_vm_vnet(vm_name);
            if (auto_iface) {
                iface = auto_iface;
                PCV_LOG_INFO("NET", "Resolved vm_name '%s' → interface '%s'",
                             vm_name, iface);
            }
        }
    }

    if (!iface || !iface[0]) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
            "Missing required parameter: interface (or vm_name)");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); g_free(auto_iface); return;
    }
    /* [V11] 최종 iface(직접 지정 or vm_name 해석 결과)를 tc argv 이전에 검증. */
    if (!pcv_validate_iface_name(iface)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
            "Invalid interface name");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); g_free(auto_iface); return;
    }
    if (rate_mbps <= 0) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
            "rate_mbps must be > 0");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); g_free(auto_iface); return;
    }
    if (burst_kb <= 0) burst_kb = 256;

    /* direction 검증: egress 또는 ingress만 허용 */
    if (g_strcmp0(direction, "egress") != 0 && g_strcmp0(direction, "ingress") != 0) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
            "direction must be 'egress' or 'ingress'");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); g_free(auto_iface); return;
    }

    if (g_strcmp0(direction, "ingress") == 0) {
        /* ── Ingress Policing ────────────────────────────────────
         * Step 1: ingress qdisc 생성 (handle ffff:)
         *   기존 ingress qdisc가 있으면 먼저 삭제 후 재생성 (멱등) */
        {
            const gchar *del_argv[] = {"tc", "qdisc", "del", "dev", iface,
                "ingress", NULL};
            gchar *std_err = NULL;
            GError *error = NULL;
            /* 삭제 실패는 무시 (ingress qdisc 미존재 시) */
            pcv_spawn_sync(del_argv, NULL, &std_err, &error);
            g_free(std_err);
            if (error) g_error_free(error);
        }
        {
            const gchar *argv[] = {"tc", "qdisc", "add", "dev", iface,
                "handle", "ffff:", "ingress", NULL};
            gchar *std_err = NULL;
            GError *error = NULL;
            if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
                gchar *msg = g_strdup_printf("tc ingress qdisc failed: %s",
                    error ? error->message : (std_err ? std_err : "unknown"));
                gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32000, msg);
                pure_uds_server_send_response(server, connection, err_resp);
                g_free(err_resp); g_free(msg);
                if (error) g_error_free(error);
                g_free(std_err); g_free(auto_iface); return;  /* [V17] leak fix */
            }
            g_free(std_err);
        }

        /* Step 2: u32 police 필터 — rate 초과 트래픽 drop
         *   protocol all: 모든 프로토콜 대상
         *   u32 match u32 0 0: 모든 패킷 매칭 (와일드카드)
         *   police rate <N>mbit burst <N>k drop: 폴리싱 룰 */
        {
            gchar *rate = g_strdup_printf("%dmbit", rate_mbps);
            gchar *burst = g_strdup_printf("%dk", burst_kb);
            const gchar *argv[] = {"tc", "filter", "add", "dev", iface,
                "parent", "ffff:", "protocol", "all",
                "u32", "match", "u32", "0", "0",
                "police", "rate", rate, "burst", burst, "drop", NULL};
            gchar *std_err = NULL;
            GError *error = NULL;
            gboolean ok = pcv_spawn_sync(argv, NULL, &std_err, &error);
            g_free(rate); g_free(burst);
            if (!ok) {
                gchar *msg = g_strdup_printf("tc ingress filter failed: %s",
                    error ? error->message : (std_err ? std_err : "unknown"));
                gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32000, msg);
                pure_uds_server_send_response(server, connection, err_resp);
                g_free(err_resp); g_free(msg);
                if (error) g_error_free(error);
                g_free(std_err); g_free(auto_iface); return;  /* [V17] leak fix */
            }
            g_free(std_err);
        }

        g_message("[QoS] Set ingress policing %dMbit burst=%dk on %s",
                  rate_mbps, burst_kb, iface);
    } else {
        /* ── Egress HTB (기존 로직) ──────────────────────────────
         * 1. root qdisc 생성 (HTB = Hierarchical Token Bucket)
         *    replace: 기존 qdisc가 있으면 교체, 없으면 생성 (멱등)
         *    handle 1: — 이 qdisc의 식별자 (class에서 parent 1:로 참조)
         *    default 10 — 분류되지 않은 트래픽은 class 1:10으로 전달 */
        {
            const gchar *argv[] = {"tc", "qdisc", "replace", "dev", iface,
                "root", "handle", "1:", "htb", "default", "10", NULL};
            gchar *std_err = NULL;
            GError *error = NULL;
            if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
                gchar *msg = g_strdup_printf("tc qdisc failed: %s",
                    error ? error->message : (std_err ? std_err : "unknown"));
                gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32000, msg);
                pure_uds_server_send_response(server, connection, err_resp);
                g_free(err_resp); g_free(msg);
                if (error) g_error_free(error);
                g_free(std_err); g_free(auto_iface); return;  /* [V17] leak fix */
            }
            g_free(std_err);
        }

        /* 2. HTB class 생성 — 실제 대역폭 제한을 적용하는 단위
         *    parent 1: classid 1:10 — root qdisc 아래 class 1:10
         *    rate: 보장 대역폭 (Mbit/s), burst: 순간 허용 초과량 (KB)
         *    replace: 기존 class가 있으면 갱신, 없으면 생성 (멱등) */
        {
            gchar *rate = g_strdup_printf("%dMbit", rate_mbps);
            gchar *burst = g_strdup_printf("%dk", burst_kb);
            const gchar *argv[] = {"tc", "class", "replace", "dev", iface,
                "parent", "1:", "classid", "1:10", "htb",
                "rate", rate, "burst", burst, NULL};
            gchar *std_err = NULL;
            GError *error = NULL;
            gboolean ok = pcv_spawn_sync(argv, NULL, &std_err, &error);
            g_free(rate); g_free(burst);
            if (!ok) {
                gchar *msg = g_strdup_printf("tc class failed: %s",
                    error ? error->message : (std_err ? std_err : "unknown"));
                gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32000, msg);
                pure_uds_server_send_response(server, connection, err_resp);
                g_free(err_resp); g_free(msg);
                if (error) g_error_free(error);
                g_free(std_err); g_free(auto_iface); return;  /* [V17] leak fix */
            }
            g_free(std_err);
        }

        g_message("[QoS] Set egress %dMbit burst=%dk on %s", rate_mbps, burst_kb, iface);
    }

    /* QoS 규칙 영속화 (BE-5) — tc 적용 성공 시 JSON 파일에 저장 */
    _qos_persist_save(iface, direction, rate_mbps, burst_kb);

    JsonObject *qos_res = json_object_new();
    json_object_set_string_member(qos_res, "status", "ok");
    json_object_set_string_member(qos_res, "interface", iface);
    json_object_set_string_member(qos_res, "direction", direction);
    json_object_set_int_member(qos_res, "rate_mbps", rate_mbps);
    json_object_set_int_member(qos_res, "burst_kb", burst_kb);
    JsonNode *qos_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(qos_node, qos_res);
    gchar *qos_resp = pure_rpc_build_success_response(rpc_id, qos_node);
    pure_uds_server_send_response(server, connection, qos_resp);
    g_free(qos_resp);
    g_free(auto_iface);  /* BE-A18: 자동 조회된 인터페이스 해제 */
}

/* ══════════════════════════════════════════════════════════════════════
 * network.qos.get — tc 통계 조회 (동기)
 *
 * [파라미터]
 *   interface : 타겟 인터페이스 이름 (필수)
 * ══════════════════════════════════════════════════════════════════════ */
void handle_network_qos_get(JsonObject *params, const gchar *rpc_id,
                             UdsServer *server, GSocketConnection *connection)
{
    const gchar *iface = json_object_has_member(params, "interface")
        ? json_object_get_string_member(params, "interface") : NULL;

    if (!iface || !iface[0]) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
            "Missing required parameter: interface");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    const gchar *argv[] = {"tc", "-s", "class", "show", "dev", iface, NULL};
    gchar *stdout_buf = NULL;
    gchar *std_err = NULL;
    GError *error = NULL;

    if (!pcv_spawn_sync(argv, &stdout_buf, &std_err, &error)) {
        gchar *msg = g_strdup_printf("tc query failed: %s",
            error ? error->message : (std_err ? std_err : "unknown"));
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32000, msg);
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp); g_free(msg);
        if (error) g_error_free(error);
        g_free(stdout_buf); g_free(std_err); return;
    }
    g_free(std_err);

    JsonObject *qget_res = json_object_new();
    json_object_set_string_member(qget_res, "interface", iface);
    json_object_set_string_member(qget_res, "tc_output", stdout_buf ? stdout_buf : "");
    json_object_set_boolean_member(qget_res, "egress_active",
        stdout_buf && strstr(stdout_buf, "htb") != NULL);
    g_free(stdout_buf);

    /* ingress qdisc 상태도 조회 */
    {
        const gchar *ing_argv[] = {"tc", "qdisc", "show", "dev", iface, "ingress", NULL};
        gchar *ing_stdout = NULL;
        gchar *ing_stderr = NULL;
        GError *ing_error = NULL;
        gboolean ing_ok = pcv_spawn_sync(ing_argv, &ing_stdout, &ing_stderr, &ing_error);
        json_object_set_boolean_member(qget_res, "ingress_active",
            ing_ok && ing_stdout && strstr(ing_stdout, "ingress") != NULL);
        g_free(ing_stdout); g_free(ing_stderr);
        if (ing_error) g_error_free(ing_error);
    }

    JsonNode *qget_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(qget_node, qget_res);
    gchar *qget_resp = pure_rpc_build_success_response(rpc_id, qget_node);
    pure_uds_server_send_response(server, connection, qget_resp);
    g_free(qget_resp);
}

/* ══════════════════════════════════════════════════════════════════════
 * network.qos.remove — tc qdisc 제거 (동기, 멱등)
 *
 * [파라미터]
 *   interface : 타겟 인터페이스 이름 (필수)
 *   direction : "egress" (기본), "ingress", 또는 "all" (양방향 제거)
 * ══════════════════════════════════════════════════════════════════════ */
void handle_network_qos_remove(JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *connection)
{
    const gchar *iface = json_object_has_member(params, "interface")
        ? json_object_get_string_member(params, "interface") : NULL;
    const gchar *direction = json_object_has_member(params, "direction")
        ? json_object_get_string_member(params, "direction") : "egress";

    if (!iface || !iface[0]) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
            "Missing required parameter: interface");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    gboolean remove_egress  = (g_strcmp0(direction, "egress") == 0 || g_strcmp0(direction, "all") == 0);
    gboolean remove_ingress = (g_strcmp0(direction, "ingress") == 0 || g_strcmp0(direction, "all") == 0);

    /* egress qdisc 제거 (root) */
    if (remove_egress) {
        const gchar *argv[] = {"tc", "qdisc", "del", "dev", iface, "root", NULL};
        gchar *std_err = NULL;
        GError *error = NULL;

        if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
            /* 멱등: qdisc 미존재 시 성공 처리 */
            const gchar *emsg = error ? error->message : (std_err ? std_err : "");
            if (strstr(emsg, "No such file") || strstr(emsg, "Cannot delete")) {
                if (error) g_error_free(error);
                g_free(std_err);
            } else {
                gchar *msg = g_strdup_printf("tc qdisc del failed: %s", emsg);
                gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32000, msg);
                pure_uds_server_send_response(server, connection, err_resp);
                g_free(err_resp); g_free(msg);
                if (error) g_error_free(error);
                g_free(std_err); return;
            }
        } else {
            g_free(std_err);
        }
    }

    /* ingress qdisc 제거 */
    if (remove_ingress) {
        const gchar *argv[] = {"tc", "qdisc", "del", "dev", iface, "ingress", NULL};
        gchar *std_err = NULL;
        GError *error = NULL;

        if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
            /* 멱등: ingress qdisc 미존재 시 성공 처리 */
            const gchar *emsg = error ? error->message : (std_err ? std_err : "");
            if (!strstr(emsg, "No such file") && !strstr(emsg, "Cannot delete")
                && !strstr(emsg, "Invalid argument")) {
                gchar *msg = g_strdup_printf("tc ingress del failed: %s", emsg);
                gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32000, msg);
                pure_uds_server_send_response(server, connection, err_resp);
                g_free(err_resp); g_free(msg);
                if (error) g_error_free(error);
                g_free(std_err); return;
            }
            if (error) g_error_free(error);
            g_free(std_err);
        } else {
            g_free(std_err);
        }
    }

    g_message("[QoS] Removed tc qdisc (%s) on %s", direction, iface);

    /* 영속화된 QoS 규칙도 제거 (BE-5) */
    _qos_persist_remove(iface,
        g_strcmp0(direction, "all") == 0 ? "both" : direction);

    JsonObject *qrm_res = json_object_new();
    json_object_set_string_member(qrm_res, "status", "removed");
    json_object_set_string_member(qrm_res, "interface", iface);
    JsonNode *qrm_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(qrm_node, qrm_res);
    gchar *qrm_resp = pure_rpc_build_success_response(rpc_id, qrm_node);
    pure_uds_server_send_response(server, connection, qrm_resp);
    g_free(qrm_resp);
}

/* ══════════════════════════════════════════════════════════════════════
 * BE-A19: Bridge VLAN Filtering — VLAN 태그 자동 설정
 *
 * Linux bridge의 VLAN filtering을 활성화하고 인터페이스에 VLAN ID를 할당한다.
 * sysfs를 통해 vlan_filtering 활성화 후 bridge vlan 명령으로 VID를 추가한다.
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * pcv_bridge_vlan_add:
 * @bridge:  브릿지 이름 (예: "pcvbr0")
 * @iface:   VLAN을 할당할 인터페이스 (예: "vnet0")
 * @vlan_id: VLAN ID (1-4094)
 *
 * Returns: 성공 시 TRUE
 */
gboolean
pcv_bridge_vlan_add(const gchar *bridge, const gchar *iface, gint vlan_id)
{
    if (!bridge || !iface || vlan_id < 1 || vlan_id > 4094) return FALSE;

    /* sysfs로 VLAN filtering 활성화 */
    gchar *filter_path = g_strdup_printf("/sys/class/net/%s/bridge/vlan_filtering",
                                          bridge);
    GError *werr = NULL;
    if (!g_file_set_contents(filter_path, "1", -1, &werr)) {
        PCV_LOG_WARN("NET", "Failed to enable VLAN filtering on %s: %s",
                     bridge, werr ? werr->message : "unknown");
        if (werr) g_error_free(werr);
        g_free(filter_path);
        return FALSE;
    }
    g_free(filter_path);

    /* bridge vlan add dev <iface> vid <vlan_id> */
    gchar *vid = g_strdup_printf("%d", vlan_id);
    const gchar *argv[] = {"bridge", "vlan", "add", "dev", iface,
                           "vid", vid, NULL};
    gchar *std_err = NULL;
    GError *error = NULL;
    gboolean ok = pcv_spawn_sync(argv, NULL, &std_err, &error);
    g_free(vid);

    if (ok) {
        PCV_LOG_INFO("NET", "Added VLAN %d to %s on bridge %s",
                     vlan_id, iface, bridge);
    } else {
        PCV_LOG_WARN("NET", "Failed to add VLAN %d to %s: %s",
                     vlan_id, iface,
                     error ? error->message : (std_err ? std_err : "unknown"));
    }
    if (error) g_error_free(error);
    g_free(std_err);
    return ok;
}

/**
 * pcv_bridge_vlan_remove:
 * @bridge:  브릿지 이름
 * @iface:   인터페이스 이름
 * @vlan_id: 제거할 VLAN ID
 *
 * Returns: 성공 시 TRUE (멱등: 미존재 시에도 TRUE)
 */
gboolean
pcv_bridge_vlan_remove(const gchar *bridge, const gchar *iface, gint vlan_id)
{
    if (!bridge || !iface || vlan_id < 1 || vlan_id > 4094) return FALSE;

    gchar *vid = g_strdup_printf("%d", vlan_id);
    const gchar *argv[] = {"bridge", "vlan", "del", "dev", iface,
                           "vid", vid, NULL};
    gchar *std_err = NULL;
    GError *error = NULL;
    gboolean ok = pcv_spawn_sync(argv, NULL, &std_err, &error);
    g_free(vid);

    if (ok) {
        PCV_LOG_INFO("NET", "Removed VLAN %d from %s on bridge %s",
                     vlan_id, iface, bridge);
    } else {
        /* 멱등 처리: 미존재 VLAN은 성공으로 간주 */
        const gchar *emsg = error ? error->message : (std_err ? std_err : "");
        if (strstr(emsg, "No such") || strstr(emsg, "does not exist")) {
            ok = TRUE;
        } else {
            PCV_LOG_WARN("NET", "Failed to remove VLAN %d from %s: %s",
                         vlan_id, iface, emsg);
        }
    }
    if (error) g_error_free(error);
    g_free(std_err);
    return ok;
}
