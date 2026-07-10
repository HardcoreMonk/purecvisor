/**
 * @file ovs_overlay.c
 * @brief OVS VXLAN 오버레이 코어 -- 수동 오버레이/피어 관리
 *
 * ====================================================================
 * [아키텍처 위치]
 *   handler_overlay.c --> ovs_overlay (이 파일)
 *   main.c (부팅)     --> pcv_overlay_init / create
 *
 *   handler_overlay.c 에서 overlay.* RPC 6개를 처리할 때 이 모듈의
 *   함수를 호출한다. 또한 데몬 부팅 시 main.c 에서 자동 프로비저닝을
 *   수행한다.
 *
 * [담당 RPC 메서드] (6개, handler_overlay.c 경유)
 *   overlay.create     - OVS 브릿지 생성 + IP/CIDR 할당
 *   overlay.delete     - OVS 브릿지 삭제 + 메타데이터 정리
 *   overlay.list       - 등록된 오버레이 네트워크 목록 (JSON 배열)
 *   overlay.info       - 단일 오버레이 상세 정보 (VNI, CIDR, 피어 목록)
 *   overlay.add_peer   - VXLAN 터널 포트 추가 (단일 피어)
 *   overlay.remove_peer- VXLAN 터널 포트 제거
 *
 * [핵심 동작 흐름]
 *   1. pcv_overlay_init(local_ip): 로컬 터널 IP 설정 (eno2 대역)
 *   2. pcv_overlay_create(name, vni, cidr):
 *      ovs-vsctl add-br <name> --> ip addr add <cidr> --> ip link set up
 *   3. pcv_overlay_add_peer(name, peer_ip):
 *      ovs-vsctl add-port <name> vxlan-X-Y
 *        -- type=vxlan options:remote_ip=<peer> options:key=<vni>
 *
 * [VXLAN 터널 네이밍 규칙]
 *   "192.0.2.20" -> "vxlan-2-20" (마지막 두 옥텟 사용)
 *   OVS 포트 이름 15자 제한에 맞춰 간결하게 생성.
 *
 * [내부 상태 관리]
 *   전역 구조체 G 에 최대 OVERLAY_MAX(16)개 오버레이 네트워크를 관리.
 *   GMutex 로 동시 접근 보호. 메타데이터는 JSON 파일로 디스크에 영속화.
 *
 * [싱글/멀티 경계]
 *   이 파일은 에디션 공용 코어만 담당한다.
 *   - Single Edge: 수동 오버레이 생성/삭제/조회, 수동 peer add/remove
 *   - Cluster build : 위 공용 코어 + 별도 파일의 auto_mesh 자동화
 *
 * [의존 모듈]
 *   pcv_spawn.h - ovs-vsctl, ip 명령 실행
 *   pcv_log.h   - OVERLAY_LOG_DOM 도메인 로깅
 *
 * [주의사항]
 *   - OVS 포트 이름은 15자 제한 (Linux netdev 이름 제한).
 *   - VXLAN 터널은 eno2 (192.168.1.x) 대역 사용, eno1과 분리.
 *   - pcv_overlay_shutdown() 호출 시 OVS 브릿지는 삭제하지 않고
 *     인메모리 상태만 정리한다 (재부팅 시 OVS가 자체 복원).
 * ====================================================================
 */
#include "ovs_overlay.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_log.h"
#include <string.h>
#include <stdio.h>

#define OVERLAY_LOG_DOM   "ovs_overlay"
#define OVERLAY_META_DIR  "/var/run/purecvisor"
#define OVERLAY_MAX       16

typedef struct {
    gchar    *name;
    gchar    *cidr;
    gint      vni;
    GPtrArray *peers;   /* peer tunnel IPs */
    gboolean  active;
} OverlayNet;

static struct {
    gchar      *local_ip;
    OverlayNet  nets[OVERLAY_MAX];
    gint        count;
    GMutex      mu;
    gboolean    initialized;
} G = {0};

/* ── helpers ──────────────────────────────────────────────────── */

/**
 * _find — 이름으로 오버레이 네트워크 검색
 * @name: 오버레이 이름 (예: "pcvoverlay0")
 *
 * G.nets 배열에서 g_strcmp0()으로 이름 매칭. O(N) 선형 탐색.
 * 호출자는 반드시 G.mu 잠금 상태에서 호출해야 한다.
 *
 * @return 찾으면 OverlayNet 포인터, 없으면 NULL
 */
static OverlayNet *
_find(const gchar *name)
{
    for (gint i = 0; i < G.count; i++)
        if (g_strcmp0(G.nets[i].name, name) == 0)
            return &G.nets[i];
    return NULL;
}

/**
 * _peer_port_name — 피어 IP에서 VXLAN 포트 이름 생성
 * @peer_ip: 피어 터널 IP (예: "192.0.2.20")
 *
 * OVS 포트 이름은 15자 제한(Linux netdev 제한)이므로
 * 마지막 두 옥텟만 사용하여 "vxlan-2-20" 형태로 생성한다.
 *
 * @return (transfer full): 생성된 포트 이름 (호출자 g_free)
 */
static gchar *
_peer_port_name(const gchar *peer_ip)
{
    /* "192.0.2.20" → "vxlan-2-20" (last two octets) */
    gchar **parts = g_strsplit(peer_ip, ".", -1);
    gchar *name = NULL;
    if (g_strv_length(parts) == 4)
        name = g_strdup_printf("vxlan-%s-%s", parts[2], parts[3]);
    else
        name = g_strdup_printf("vxlan-%s", peer_ip);
    g_strfreev(parts);
    return name;
}

/**
 * _run_argv — argv 배열 기반 프로세스 실행 (셸 미경유, 인젝션 방지)
 * @argv: NULL 종단 인자 배열
 * @error: (nullable): 에러 반환 포인터
 *
 * pcv_spawn_sync()를 직접 호출. /bin/sh -c를 경유하지 않습니다.
 * 실패 시 stderr를 PCV_LOG_WARN으로 기록.
 *
 * @return 성공 시 TRUE
 */
static gboolean
_run_argv(const gchar * const *argv, GError **error)
{
    gchar *std_err = NULL;
    gboolean ok = pcv_spawn_sync(argv, NULL, &std_err, error);
    if (!ok && std_err) {
        PCV_LOG_WARN(OVERLAY_LOG_DOM, "cmd failed: %s → %s", argv[0], std_err);
    }
    g_free(std_err);
    return ok;
}

/**
 * _save_meta — 오버레이 메타데이터를 JSON 파일로 영속화
 * @net: 저장할 오버레이 네트워크 구조체
 *
 * /var/run/purecvisor/overlay-<name>.meta 파일에 이름, VNI, CIDR,
 * 피어 목록을 JSON 형식으로 저장한다.
 * 호출자는 G.mu 잠금 상태에서 호출해야 한다.
 */
static void
_save_meta(OverlayNet *net)
{
    gchar *path = g_strdup_printf(OVERLAY_META_DIR "/overlay-%s.meta", net->name);
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "name", net->name);
    json_object_set_int_member(obj, "vni", net->vni);
    json_object_set_string_member(obj, "cidr", net->cidr ? net->cidr : "");

    JsonArray *peers = json_array_new();
    for (guint i = 0; i < net->peers->len; i++)
        json_array_add_string_element(peers, g_ptr_array_index(net->peers, i));
    json_object_set_array_member(obj, "peers", peers);

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, obj);
    gchar *data = json_to_string(node, FALSE);
    g_file_set_contents(path, data, -1, NULL);

    g_free(data);
    json_node_free(node);
    json_object_unref(obj);
    g_free(path);
}

/* ── lifecycle ────────────────────────────────────────────────── */

/**
 * pcv_overlay_init — 오버레이 매니저 초기화
 * @local_tunnel_ip: 로컬 터널 엔드포인트 IP (eno2 대역, 예: "192.0.2.19")
 *
 * NULL 또는 빈 문자열이면 오버레이 기능을 비활성화한다.
 * daemon.conf [overlay] local_ip 키에서 읽어 main.c에서 호출.
 */
void
pcv_overlay_init(const gchar *local_tunnel_ip)
{
    if (!local_tunnel_ip || !*local_tunnel_ip) {
        PCV_LOG_INFO(OVERLAY_LOG_DOM, "No tunnel IP configured — overlay disabled");
        return;
    }
    g_mutex_init(&G.mu);
    G.local_ip = g_strdup(local_tunnel_ip);
    G.count = 0;
    G.initialized = TRUE;
    PCV_LOG_INFO(OVERLAY_LOG_DOM, "Overlay manager initialized (tunnel_ip=%s)", local_tunnel_ip);
}

/**
 * pcv_overlay_shutdown — 오버레이 매니저 종료
 *
 * 인메모리 상태(이름, CIDR, 피어 목록)만 해제한다.
 * OVS 브릿지 자체는 삭제하지 않는다 (재부팅 시 OVS가 자체 복원).
 */
void
pcv_overlay_shutdown(void)
{
    if (!G.initialized) return;
    g_mutex_lock(&G.mu);
    for (gint i = 0; i < G.count; i++) {
        g_free(G.nets[i].name);
        g_free(G.nets[i].cidr);
        g_ptr_array_free(G.nets[i].peers, TRUE);
    }
    G.count = 0;
    g_free(G.local_ip);
    g_mutex_unlock(&G.mu);
    g_mutex_clear(&G.mu);
    G.initialized = FALSE;
}

/* ── overlay CRUD ─────────────────────────────────────────────── */

/**
 * pcv_overlay_create — OVS 오버레이 네트워크 생성 (멱등)
 * @name: 오버레이 브릿지 이름 (예: "pcvoverlay0")
 * @vni: VXLAN Network Identifier (기본: 100)
 * @cidr: 게이트웨이 IP/CIDR (예: "10.100.0.1/24"), NULL이면 IP 미할당
 * @error: 에러 반환 포인터
 *
 * 실행 순서:
 *   1) ovs-vsctl --may-exist add-br <name> (OVS 브릿지 생성)
 *   2) ip link set <name> up (인터페이스 활성화)
 *   3) ip addr add <cidr> dev <name> (게이트웨이 IP 할당)
 *   4) 인메모리 등록 + 메타데이터 파일 저장
 *
 * 이미 같은 이름의 오버레이가 존재하면 TRUE 반환 (멱등).
 * OVERLAY_MAX(16)개 제한 초과 시 GError 반환.
 *
 * @return 성공 시 TRUE
 */
gboolean
pcv_overlay_create(const gchar *name, gint vni, const gchar *cidr, GError **error)
{
    if (!G.initialized) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Overlay not initialized");
        return FALSE;
    }

    g_mutex_lock(&G.mu);
    if (_find(name)) {
        g_mutex_unlock(&G.mu);
        return TRUE;  /* idempotent */
    }
    if (G.count >= OVERLAY_MAX) {
        g_mutex_unlock(&G.mu);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Max overlays reached");
        return FALSE;
    }

    /* 1. Create OVS bridge */
    { const gchar *a[] = {"ovs-vsctl","--may-exist","add-br",name,NULL};
      if (!_run_argv(a, error)) { g_mutex_unlock(&G.mu); return FALSE; } }

    /* 2. Set bridge up */
    { const gchar *a[] = {"ip","link","set",name,"up",NULL};
      _run_argv(a, NULL); }

    /* 3. Assign gateway IP if CIDR provided */
    if (cidr && *cidr) {
        const gchar *a[] = {"ip","addr","add",cidr,"dev",name,NULL};
        _run_argv(a, NULL);  /* soft-fail if already assigned */
    }

    /* 4. Register in memory */
    OverlayNet *net = &G.nets[G.count++];
    net->name   = g_strdup(name);
    net->cidr   = g_strdup(cidr ? cidr : "");
    net->vni    = vni > 0 ? vni : 100;
    net->peers  = g_ptr_array_new_with_free_func(g_free);
    net->active = TRUE;

    _save_meta(net);
    g_mutex_unlock(&G.mu);

    PCV_LOG_INFO(OVERLAY_LOG_DOM, "Overlay '%s' created (VNI=%d, CIDR=%s)", name, net->vni, cidr ? cidr : "-");
    return TRUE;
}

/**
 * pcv_overlay_delete — OVS 오버레이 네트워크 삭제 (멱등)
 * @name: 삭제할 오버레이 이름
 * @error: 에러 반환 포인터
 *
 * OVS 브릿지 삭제 + 메타 파일 삭제 + 인메모리 배열에서 제거.
 * 배열 중간 삭제 시 마지막 요소를 빈 자리로 이동 (swap-remove).
 * 존재하지 않는 이름에 대해서도 TRUE 반환 (멱등).
 *
 * @return 성공 시 TRUE
 */
gboolean
pcv_overlay_delete(const gchar *name, GError **error)
{
    if (!G.initialized) return TRUE;

    g_mutex_lock(&G.mu);
    OverlayNet *net = _find(name);
    if (!net) {
        g_mutex_unlock(&G.mu);
        return TRUE;  /* idempotent */
    }

    /* Remove OVS bridge */
    { const gchar *a[] = {"ovs-vsctl","--if-exists","del-br",name,NULL};
      _run_argv(a, error); }

    /* Remove meta file */
    gchar *meta = g_strdup_printf(OVERLAY_META_DIR "/overlay-%s.meta", name);
    remove(meta);
    g_free(meta);

    /* Remove from array */
    g_free(net->name);
    g_free(net->cidr);
    g_ptr_array_free(net->peers, TRUE);
    gint idx = (gint)(net - G.nets);
    if (idx < G.count - 1)
        G.nets[idx] = G.nets[G.count - 1];
    G.count--;

    g_mutex_unlock(&G.mu);
    PCV_LOG_INFO(OVERLAY_LOG_DOM, "Overlay '%s' deleted", name);
    return TRUE;
}

/**
 * pcv_overlay_list — 등록된 오버레이 네트워크 목록 조회
 *
 * 인메모리 배열을 순회하여 각 오버레이의 이름, VNI, CIDR,
 * 피어 수, 활성 상태를 JsonArray로 반환한다.
 *
 * @return (transfer full): JsonObject 배열 [{name, vni, cidr, peer_count, active}, ...]
 */
JsonArray *
pcv_overlay_list(void)
{
    JsonArray *arr = json_array_new();
    if (!G.initialized) return arr;

    g_mutex_lock(&G.mu);
    for (gint i = 0; i < G.count; i++) {
        OverlayNet *net = &G.nets[i];
        JsonObject *obj = json_object_new();
        json_object_set_string_member(obj, "name", net->name);
        json_object_set_int_member(obj, "vni", net->vni);
        json_object_set_string_member(obj, "cidr", net->cidr);
        json_object_set_int_member(obj, "peer_count", net->peers->len);
        json_object_set_boolean_member(obj, "active", net->active);
        json_array_add_object_element(arr, obj);
    }
    g_mutex_unlock(&G.mu);
    return arr;
}

/**
 * pcv_overlay_info — 단일 오버레이 상세 정보 조회
 * @name: 오버레이 이름
 *
 * 지정된 오버레이의 이름, VNI, CIDR, 로컬 터널 IP, 피어 목록을 반환.
 * 오버레이가 비활성화되었거나 이름을 찾을 수 없으면 error 필드 포함 객체 반환.
 *
 * @return (transfer full): 상세 정보 JsonObject (호출자 unref)
 */
JsonObject *
pcv_overlay_info(const gchar *name)
{
    JsonObject *obj = json_object_new();
    if (!G.initialized) {
        json_object_set_string_member(obj, "error", "overlay disabled");
        return obj;
    }

    g_mutex_lock(&G.mu);
    OverlayNet *net = _find(name);
    if (!net) {
        g_mutex_unlock(&G.mu);
        json_object_set_string_member(obj, "error", "not found");
        return obj;
    }

    json_object_set_string_member(obj, "name", net->name);
    json_object_set_int_member(obj, "vni", net->vni);
    json_object_set_string_member(obj, "cidr", net->cidr);
    json_object_set_string_member(obj, "local_tunnel_ip", G.local_ip);

    JsonArray *peers = json_array_new();
    for (guint i = 0; i < net->peers->len; i++)
        json_array_add_string_element(peers, g_ptr_array_index(net->peers, i));
    json_object_set_array_member(obj, "peers", peers);

    g_mutex_unlock(&G.mu);
    return obj;
}

/* ── VXLAN peer management ────────────────────────────────────── */

/**
 * pcv_overlay_add_peer — VXLAN 터널 포트 추가 (멱등)
 * @name: 오버레이 이름
 * @peer_tunnel_ip: 피어의 터널 IP (eno2 대역)
 * @error: 에러 반환 포인터
 *
 * ovs-vsctl로 VXLAN 포트를 추가한다:
 *   add-port <overlay> vxlan-X-Y -- set interface type=vxlan
 *     options:key=<vni> options:remote_ip=<peer_ip>
 *
 * 이미 동일 피어가 등록되어 있으면 TRUE 반환 (멱등).
 * 성공 시 피어 목록에 추가하고 메타 파일을 갱신한다.
 *
 * @return 성공 시 TRUE
 */
gboolean
pcv_overlay_add_peer(const gchar *name, const gchar *peer_tunnel_ip, GError **error)
{
    if (!G.initialized || !peer_tunnel_ip || !*peer_tunnel_ip) return FALSE;

    g_mutex_lock(&G.mu);
    OverlayNet *net = _find(name);
    if (!net) {
        g_mutex_unlock(&G.mu);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Overlay '%s' not found", name);
        return FALSE;
    }

    /* Check if already added */
    for (guint i = 0; i < net->peers->len; i++) {
        if (g_strcmp0(g_ptr_array_index(net->peers, i), peer_tunnel_ip) == 0) {
            g_mutex_unlock(&G.mu);
            return TRUE;  /* idempotent */
        }
    }

    /* Add VXLAN port */
    gchar *port_name = _peer_port_name(peer_tunnel_ip);
    gchar *key_opt = g_strdup_printf("options:key=%d", net->vni);
    gchar *rip_opt = g_strdup_printf("options:remote_ip=%s", peer_tunnel_ip);
    const gchar *a[] = {"ovs-vsctl","--may-exist","add-port",net->name,port_name,
                         "--","set","interface",port_name,"type=vxlan",key_opt,rip_opt,NULL};
    gboolean ok = _run_argv(a, error);
    g_free(key_opt);
    g_free(rip_opt);

    if (ok) {
        g_ptr_array_add(net->peers, g_strdup(peer_tunnel_ip));
        _save_meta(net);
        PCV_LOG_INFO(OVERLAY_LOG_DOM, "VXLAN peer %s added to '%s' (port=%s, VNI=%d)",
                     peer_tunnel_ip, net->name, port_name, net->vni);
    }
    g_free(port_name);
    g_mutex_unlock(&G.mu);
    return ok;
}

/**
 * pcv_overlay_remove_peer — VXLAN 터널 포트 제거 (멱등)
 * @name: 오버레이 이름
 * @peer_tunnel_ip: 제거할 피어 IP
 * @error: 에러 반환 포인터
 *
 * OVS에서 포트를 삭제하고 인메모리 피어 배열에서도 제거한다.
 * 오버레이 미초기화 또는 이름 미발견 시 TRUE 반환 (멱등).
 *
 * @return 성공 시 TRUE
 */
gboolean
pcv_overlay_remove_peer(const gchar *name, const gchar *peer_tunnel_ip, GError **error)
{
    if (!G.initialized) return TRUE;

    g_mutex_lock(&G.mu);
    OverlayNet *net = _find(name);
    if (!net) { g_mutex_unlock(&G.mu); return TRUE; }

    gchar *port_name = _peer_port_name(peer_tunnel_ip);
    { const gchar *a[] = {"ovs-vsctl","--if-exists","del-port",net->name,port_name,NULL};
      _run_argv(a, error); }
    g_free(port_name);

    /* Remove from peers array */
    for (guint i = 0; i < net->peers->len; i++) {
        if (g_strcmp0(g_ptr_array_index(net->peers, i), peer_tunnel_ip) == 0) {
            g_ptr_array_remove_index(net->peers, i);
            break;
        }
    }
    _save_meta(net);
    g_mutex_unlock(&G.mu);
    return TRUE;
}

/**
 * pcv_overlay_auto_mesh — CSV 피어 목록으로 VXLAN 풀 메시 자동 구성
 * @name: 오버레이 이름
 * @peers_csv: 쉼표 구분 피어 IP 목록 (예: "192.0.2.20,192.0.2.21")
 * @error: 에러 반환 포인터
 *
 * CSV를 파싱하여 각 피어에 대해 pcv_overlay_add_peer()를 호출한다.
 * 자기 자신의 IP(G.local_ip)는 자동으로 건너뛴다.
 * 개별 피어 추가 실패 시 경고 로그만 남기고 나머지 피어는 계속 처리한다.
 *
 * daemon.conf [overlay] peers 키에서 읽어 main.c 부팅 시 호출.
 *
 * @return 항상 TRUE (개별 실패는 비치명적)
 */
