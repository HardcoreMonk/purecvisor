/* ==========================================================================
 * src/modules/lxc/lxc_driver.h
 * PureCVisor — LXC 컨테이너 드라이버 공개 API
 *
 * [파일 역할]
 *   LXC 시스템 컨테이너의 전체 생명주기(CRUD), 런타임 조작(start/stop/exec),
 *   메트릭 조회, ZFS 스냅샷 관리를 위한 공개 인터페이스를 선언합니다.
 *   handler_container.c(container.* RPC 핸들러)가 이 헤더의 API를 호출합니다.
 *
 * [아키텍처 위치]
 *   dispatcher.c -> handler_container.c -> 이 헤더의 API -> lxc_driver.c 구현
 *   스토리지: ZFS 데이터셋 per-container (pcvpool/containers/<name>)
 *   네트워크: network_manager.c 브릿지 재사용 (veth pair + bridge)
 *
 * [설계 원칙]
 *   생명주기(create/destroy) : GTask 비동기 (GSubprocess 기반, non-blocking)
 *   런타임(start/stop/state) : liblxc C API 직접 호출 (GTask 워커 래핑)
 *   메트릭(list/metrics)     : liblxc C API 동기 호출 (빠름)
 *   명령 실행(exec)          : pcv_spawn_sync 폴백 (lxc-attach, seccomp 우회)
 *   스냅샷(snapshot.*)       : GSubprocess (zfs snapshot/rollback/destroy/list)
 *
 * [주요 자료구조]
 *   PcvLxcState   — 컨테이너 런타임 상태 열거형 (STOPPED/RUNNING/FROZEN 등)
 *   PcvLxcInfo    — container.list 응답용 요약 정보 (이름/상태/IP/이미지)
 *   PcvLxcMetrics — container.metrics 응답용 실시간 메트릭 (CPU/메모리/네트워크)
 *
 * [메모리 관리]
 *   비동기 API: _async() 호출 후 _finish()에서 결과 수신 (GAsyncResult 패턴)
 *   PcvLxcInfo/PcvLxcMetrics: 전용 _free() 함수로 해제
 *   GPtrArray 반환: g_ptr_array_unref()로 해제 (free_func 자동 호출)
 *
 * [주의사항]
 *   - PCV_LXC_PATH, PCV_LXC_ZFS_BASE 상수가 모든 경로의 기준
 *   - seccomp 환경에서 liblxc get_ips() 실패 시 lxc-info -iH CLI 폴백 사용
 *   - 빌드 의존성: liblxc-dev (pkg-config: lxc)
 * ========================================================================== */

#ifndef PURECVISOR_LXC_DRIVER_H
#define PURECVISOR_LXC_DRIVER_H

#include <glib.h>
#include <gio/gio.h>
#include "../../utils/pcv_config.h"

G_BEGIN_DECLS

/* ──────────────────────────────────────────────────────────────────────────
 * 전역 경로 상수
 * ──────────────────────────────────────────────────────────────────────────*/
/** LXC 컨테이너 루트 경로 — daemon.conf [container] lxc_path (기본: /var/lib/purecvisor/lxc) */
#define PCV_LXC_PATH        (pcv_config_get_container_path())

/** ZFS 컨테이너 데이터셋 베이스 — daemon.conf [storage] container_pool (기본: pcvpool/containers) */
#define PCV_LXC_ZFS_BASE    (pcv_config_get_container_pool())

/** 컨테이너 기본 네트워크 브릿지 (PureVisor NAT 브릿지) */
#define PCV_LXC_DEFAULT_BRIDGE "virbr0"

/* ──────────────────────────────────────────────────────────────────────────
 * 자료구조
 * ──────────────────────────────────────────────────────────────────────────*/

/**
 * @brief 컨테이너 런타임 상태 열거형
 */
typedef enum {
    PCV_LXC_STATE_STOPPED  = 0,
    PCV_LXC_STATE_STARTING = 1,
    PCV_LXC_STATE_RUNNING  = 2,
    PCV_LXC_STATE_STOPPING = 3,
    PCV_LXC_STATE_FROZEN   = 4,
    PCV_LXC_STATE_UNKNOWN  = 99,
} PcvLxcState;

/**
 * @brief container.list 응답용 컨테이너 요약 정보
 */
typedef struct {
    gchar       *name;       /**< 컨테이너 이름 */
    PcvLxcState  state;      /**< 런타임 상태 */
    gchar       *state_str;  /**< 상태 문자열 ("RUNNING", "STOPPED", ...) */
    gchar       *ip_addr;    /**< IP 주소 ("N/A" if not running or no IP) */
    gchar       *image;      /**< 기반 이미지 (lxc config에서 읽음) */
} PcvLxcInfo;

/**
 * @brief container.metrics 응답용 실시간 메트릭
 */
typedef struct {
    gchar    *name;
    gchar    *state_str;
    guint64   mem_used_bytes;   /**< 현재 메모리 사용량 (bytes) */
    guint64   mem_limit_bytes;  /**< 메모리 제한 (bytes, 0 = unlimited) */
    guint64   cpu_time_ns;      /**< 누적 CPU 사용 시간 (nanoseconds) */
    gdouble   cpu_percent;      /**< CPU 사용률 % (두 시점 delta, 0 if not available) */
    guint64   net_rx_bytes;     /**< 네트워크 수신 누적 바이트 */
    guint64   net_tx_bytes;     /**< 네트워크 송신 누적 바이트 */
    gchar    *ip_addr;
    pid_t     init_pid;         /**< 컨테이너 init PID (0 if not running) */
} PcvLxcMetrics;

/* ──────────────────────────────────────────────────────────────────────────
 * 생명주기 API (비동기 — GTask + GSubprocess)
 * ──────────────────────────────────────────────────────────────────────────*/

/**
 * @brief 컨테이너 생성 (비동기)
 *
 * 실행 순서:
 *   1. zfs create -o mountpoint=<PCV_LXC_PATH>/<name> <PCV_LXC_ZFS_BASE>/<name>
 *   2. lxc-create -P <PCV_LXC_PATH> -n <name> -t download -- -d <distro> -r <release> -a amd64
 *   3. liblxc C API: memory/cpu/network 설정 → save_config()
 *
 * @param name           컨테이너 이름 (알파뉴메릭 + '-', '_')
 * @param image          이미지 문자열 ("ubuntu:22.04", "debian:12", "alpine:3.19")
 * @param memory_mb      메모리 제한 MB (0 = 512 기본값)
 * @param vcpu_count     vCPU 수 (0 = 1 기본값)
 * @param network_bridge 연결할 브릿지 이름 (NULL = PCV_LXC_DEFAULT_BRIDGE)
 */
void     pcv_lxc_create_async   (const gchar        *name,
                                  const gchar        *image,
                                  guint               memory_mb,
                                  guint               vcpu_count,
                                  const gchar        *network_bridge,
                                  GCancellable       *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer            user_data);

/**
 * @brief 컨테이너 생성 (비동기, rootless 옵션 포함) (C-6)
 *
 * pcv_lxc_create_async와 동일하되 rootless 파라미터 추가:
 * @param rootless  -1=daemon.conf 전역 설정 사용, 0=privileged 강제, 1=rootless 강제
 *                  rootless=1 시 user namespace ID 매핑 자동 설정 (lxc.idmap)
 *                  /etc/subuid,subgid 미설정 시 privileged로 graceful fallback
 */
void     pcv_lxc_create_async_full(const gchar        *name,
                                  const gchar        *image,
                                  guint               memory_mb,
                                  guint               vcpu_count,
                                  const gchar        *network_bridge,
                                  gint                rootless,
                                  GCancellable       *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer            user_data);

gboolean pcv_lxc_create_finish  (GAsyncResult *result, GError **error);

/**
 * @brief 컨테이너 삭제 (비동기)
 *
 * 실행 순서:
 *   1. 실행 중이면 강제 중지 (c->stop)
 *   2. lxc-destroy -P <path> -n <name> -f
 *   3. zfs destroy -r <PCV_LXC_ZFS_BASE>/<name>
 */
void     pcv_lxc_destroy_async  (const gchar        *name,
                                  GCancellable       *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer            user_data);
gboolean pcv_lxc_destroy_finish (GAsyncResult *result, GError **error);

/**
 * @brief 컨테이너 클론 (비동기, fire-and-forget)
 *
 * lxc-copy -n <source> -N <target> -B zfs -P <path>
 * ZFS Copy-on-Write 기반 빠른 복제. GTask 워커에서 비동기 실행.
 *
 * @param source 원본 컨테이너 이름
 * @param target 대상 컨테이너 이름
 * @return TRUE 작업 수락, FALSE 입력 오류
 */
void     pcv_lxc_clone_async    (const gchar        *source,
                                  const gchar        *target,
                                  GCancellable       *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer            user_data);
gboolean pcv_lxc_clone_finish   (GAsyncResult *result, GError **error);
gboolean pcv_lxc_clone          (const gchar *source, const gchar *target);

/* B1(IDOR 시정): operator owner-scope 소유자 저장소.
 * 테스트 바이너리도 링크하는 self-contained 추출 TU(src/modules/lxc/lxc_owner.c).
 * pcv_lxc_stamp_owner / pcv_lxc_read_owner 선언은 lxc_owner.h에 있다. lxc_driver.h를
 * include하는 기존 소비자(handler_container.c, dispatcher.c)가 그대로 볼 수 있도록
 * 여기서 재노출한다. */
#include "lxc_owner.h"

/* ──────────────────────────────────────────────────────────────────────────
 * 런타임 API (비동기 래핑 — GTask 워커로 liblxc 호출)
 * ──────────────────────────────────────────────────────────────────────────*/

/**
 * @brief 컨테이너 시작 (비동기)
 * liblxc: c->want_daemonize(TRUE) → c->start(0, NULL)
 */
void     pcv_lxc_start_async    (const gchar        *name,
                                  GCancellable       *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer            user_data);
gboolean pcv_lxc_start_finish   (GAsyncResult *result, GError **error);

/**
 * @brief 컨테이너 중지 (비동기)
 * force=FALSE: c->shutdown(30초) → 타임아웃 시 c->stop()
 * force=TRUE:  c->stop() 즉시
 */
void     pcv_lxc_stop_async     (const gchar        *name,
                                  gboolean            force,
                                  GCancellable       *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer            user_data);
gboolean pcv_lxc_stop_finish    (GAsyncResult *result, GError **error);

/* ──────────────────────────────────────────────────────────────────────────
 * 조회 API
 * ──────────────────────────────────────────────────────────────────────────*/

/** @brief 전체 컨테이너 목록 (GPtrArray<PcvLxcInfo*>) */
GPtrArray      *pcv_lxc_list        (GError **error);

/** @brief 단일 컨테이너 실시간 메트릭 조회 */
PcvLxcMetrics  *pcv_lxc_get_metrics (const gchar *name, GError **error);

/** @brief 컨테이너 상태 문자열 반환 ("RUNNING" | "STOPPED" | ...) — 호출자 g_free() */
gchar          *pcv_lxc_get_state   (const gchar *name);

/* ──────────────────────────────────────────────────────────────────────────
 * Exec API
 * ──────────────────────────────────────────────────────────────────────────*/

/**
 * @brief 컨테이너 내부에서 명령 실행 (비동기)
 * lxc-attach -P <path> -n <name> -- <argv[0]> [argv[1]...]
 * @return stdout 출력 문자열 (호출자 g_free())
 */
void  pcv_lxc_exec_async  (const gchar        *name,
                            const gchar       **argv,
                            GCancellable       *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer            user_data);
gchar *pcv_lxc_exec_finish (GAsyncResult *result, GError **error);

/* ──────────────────────────────────────────────────────────────────────────
 * ZFS 스냅샷 API (컨테이너 전용 경로: pcvpool/containers/<n>@<snap>)
 * ──────────────────────────────────────────────────────────────────────────*/

void     pcv_lxc_snapshot_create_async   (const gchar *name, const gchar *snap_name,
                                           GCancellable *c, GAsyncReadyCallback cb,
                                           gpointer user_data);
gboolean pcv_lxc_snapshot_create_finish  (GAsyncResult *result, GError **error);

void     pcv_lxc_snapshot_rollback_async (const gchar *name, const gchar *snap_name,
                                           GCancellable *c, GAsyncReadyCallback cb,
                                           gpointer user_data);
gboolean pcv_lxc_snapshot_rollback_finish(GAsyncResult *result, GError **error);

void     pcv_lxc_snapshot_delete_async   (const gchar *name, const gchar *snap_name,
                                           GCancellable *c, GAsyncReadyCallback cb,
                                           gpointer user_data);
gboolean pcv_lxc_snapshot_delete_finish  (GAsyncResult *result, GError **error);

/**
 * @brief 스냅샷 목록 조회 (비동기)
 * @return GPtrArray<gchar*> — 스냅샷 이름 목록
 */
void       pcv_lxc_snapshot_list_async  (const gchar *name,
                                          GCancellable *c, GAsyncReadyCallback cb,
                                          gpointer user_data);
GPtrArray *pcv_lxc_snapshot_list_finish (GAsyncResult *result, GError **error);

/* ──────────────────────────────────────────────────────────────────────────
 * cgroup v2 리소스 제한 (container.set_limits RPC)
 * ──────────────────────────────────────────────────────────────────────────*/

/**
 * @brief 컨테이너의 CPU/메모리/I/O/PID 리소스 제한 설정 (cgroup v2 + v1 자동 감지)
 *
 * 실행 중인 컨테이너: cgroup 파일에 직접 기록 (즉시 적용, 재시작 불필요)
 *   - cgroup v2: lxc.payload.<name>/cpu.max, cpu.weight, memory.max,
 *                memory.low, memory.high, io.max, pids.max
 *   - cgroup v1: cpu/lxc/<name>/cpu.cfs_quota_us, memory/lxc/<name>/memory.limit_in_bytes
 * 정지된 컨테이너: LXC config 파일만 업데이트 (다음 시작 시 적용)
 *
 * @param name           컨테이너 이름
 * @param cpu_percent    CPU 제한 퍼센트 (0 = 변경 안 함, 100 = 1코어, 200 = 2코어)
 * @param memory_mb      메모리 제한 MB (0 = 변경 안 함)
 * @param cpu_weight     CPU weight 1-10000 (0 = 변경 안 함, cgroup v2 cpu.weight)
 * @param memory_low_mb  메모리 소프트 제한 MB (0 = 변경 안 함, cgroup v2 memory.low)
 * @param memory_high_mb 메모리 하드 제한 MB (0 = 변경 안 함, cgroup v2 memory.high)
 * @param io_read_bps    I/O 읽기 대역폭 제한 bytes/sec (0 = 변경 안 함, cgroup v2 io.max)
 * @param pids_max       최대 프로세스 수 (0 = 변경 안 함, cgroup v2 pids.max)
 * @param error          에러 반환 (nullable)
 * @return TRUE 성공, FALSE 실패
 */
gboolean pcv_lxc_set_resource_limits(const gchar *name, gint cpu_percent,
                                      gint memory_mb, gint cpu_weight,
                                      gint memory_low_mb, gint memory_high_mb,
                                      gint64 io_read_bps, gint pids_max,
                                      GError **error);

/* ──────────────────────────────────────────────────────────────────────────
 * 네트워크 관리 API (VM NIC 기능과 동등)
 * ──────────────────────────────────────────────────────────────────────────*/

/** NIC 정보 구조체 */
typedef struct {
    gchar *name;     /**< 인터페이스 이름 (eth0, eth1, ...) */
    gchar *type;     /**< "veth" */
    gchar *bridge;   /**< 연결된 브릿지 (pcvbr0, virbr0, ...) */
    gchar *hwaddr;   /**< MAC 주소 */
    gchar *ipv4;     /**< IPv4 주소 (실행 중일 때만) */
    gchar *veth_peer; /**< 호스트측 veth 이름 */
} PcvLxcNicInfo;

void pcv_lxc_nic_info_free(PcvLxcNicInfo *nic);

/**
 * @brief 컨테이너 NIC 목록 조회
 * lxc.net.X 설정 파싱 + 실행 중이면 ip addr 정보 병합
 * @return GPtrArray<PcvLxcNicInfo*> (호출자 g_ptr_array_unref)
 */
GPtrArray *pcv_lxc_nic_list(const gchar *name, GError **error);

/**
 * @brief 컨테이너에 NIC 추가 (veth pair)
 * lxc.net.N 설정 추가 + 실행 중이면 lxc-device add
 * @param bridge 연결할 브릿지 (NULL = PCV_LXC_DEFAULT_BRIDGE)
 * @param hwaddr MAC 주소 (NULL = 자동 생성)
 */
gboolean pcv_lxc_nic_attach(const gchar *name, const gchar *bridge,
                              const gchar *hwaddr, GError **error);

/**
 * @brief 컨테이너에서 NIC 제거
 * lxc.net.N 설정 제거 + 실행 중이면 ip link del
 * @param nic_name 인터페이스 이름 (eth0, eth1, ...)
 */
gboolean pcv_lxc_nic_detach(const gchar *name, const gchar *nic_name,
                              GError **error);

/**
 * @brief 컨테이너 NIC 대역폭 제한 (tc qdisc)
 * @param nic_name 인터페이스 이름 (NULL = eth0)
 * @param inbound_kbps  인바운드 제한 Kbps (0 = 제거)
 * @param outbound_kbps 아웃바운드 제한 Kbps (0 = 제거)
 */
gboolean pcv_lxc_set_bandwidth(const gchar *name, const gchar *nic_name,
                                 guint inbound_kbps, guint outbound_kbps,
                                 GError **error);

/* ──────────────────────────────────────────────────────────────────────────
 * CRIU 체크포인트/복원 (CE-A9)
 * ──────────────────────────────────────────────────────────────────────────*/

/**
 * @brief CRIU 기반 컨테이너 체크포인트 (상태 저장)
 * @param name            컨테이너 이름
 * @param checkpoint_dir  체크포인트 이미지 저장 디렉터리
 * @return TRUE 성공, FALSE 실패 (CRIU 미설치 포함)
 */
gboolean pcv_lxc_checkpoint(const gchar *name, const gchar *checkpoint_dir);

/**
 * @brief CRIU 기반 컨테이너 복원
 * @param name            컨테이너 이름
 * @param checkpoint_dir  체크포인트 이미지 디렉터리
 * @return TRUE 성공, FALSE 실패
 */
gboolean pcv_lxc_restore(const gchar *name, const gchar *checkpoint_dir);

/* ──────────────────────────────────────────────────────────────────────────
 * Seccomp 프로파일 관리 (R-7)
 * ──────────────────────────────────────────────────────────────────────────*/

/**
 * @brief 컨테이너에 seccomp 프로파일 적용
 * @param name         컨테이너 이름
 * @param profile_name 프로파일 이름 (/etc/purecvisor/seccomp/<name>.seccomp)
 * @return TRUE 성공, FALSE 실패 (프로파일 미존재 등)
 */
gboolean pcv_lxc_set_seccomp_profile(const gchar *name, const gchar *profile_name);

/**
 * @brief 컨테이너의 현재 seccomp 프로파일 조회
 * @param name 컨테이너 이름
 * @return 프로파일 경로 문자열 (호출자 g_free 필수), 미설정 시 NULL
 */
gchar *pcv_lxc_get_seccomp_profile(const gchar *name);

/* ──────────────────────────────────────────────────────────────────────────
 * 메모리 해제
 * ──────────────────────────────────────────────────────────────────────────*/
void pcv_lxc_info_free    (PcvLxcInfo    *info);
void pcv_lxc_metrics_free (PcvLxcMetrics *metrics);

G_END_DECLS

#endif /* PURECVISOR_LXC_DRIVER_H */
