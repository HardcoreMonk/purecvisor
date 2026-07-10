/**
 * @file handler_storage.h
 * @brief ZFS 스토리지 RPC 핸들러 공개 인터페이스
 *
 * ──────────────────────────────────────────────────────────────
 * [아키텍처 내 위치]
 *   dispatcher.c → 이 파일의 핸들러 → pcv_spawn_sync("zfs ...") / zfs_driver.c
 *   ZFS 풀 및 zvol(블록 디바이스) 관리 RPC를 처리한다.
 *
 * [RPC 메서드 매핑] (4개)
 *   storage.pool.list   -> handle_storage_pool_list_request   (동기 — ZFS 풀 목록)
 *   storage.zvol.list   -> handle_storage_zvol_list_request   (동기 — zvol 목록)
 *   storage.zvol.create -> handle_storage_zvol_create_request (동기 — zvol 생성)
 *   storage.zvol.delete -> handle_storage_zvol_delete_request (동기 — zvol 삭제)
 *
 * [REST API 매핑]
 *   GET    /api/v1/storage/pools  -> storage.pool.list
 *   GET    /api/v1/storage/zvols  -> storage.zvol.list
 *   POST   /api/v1/storage/zvols  -> storage.zvol.create
 *   DELETE /api/v1/storage/zvols  -> storage.zvol.delete
 *
 * [핸들러 시그니처] (모든 디스패처 핸들러 공통)
 *   void handler(JsonObject *params, const gchar *rpc_id,
 *                UdsServer *server, GSocketConnection *connection)
 *
 * [호출 경로]
 *   dispatcher.c -> handle_storage_*() -> pcv_spawn_sync("zfs list/create/destroy")
 *
 * [ZFS 명령어 대응]
 *   pool.list   : zfs list -H -o name,used,avail,refer,mountpoint -t filesystem
 *   zvol.list   : zfs list -H -o name,used,volsize,refer -t volume -r <pool>
 *   zvol.create : zfs create -V <size>G <pool/name>
 *   zvol.delete : zfs destroy <pool/name>
 *
 * [스토리지 환경]
 *   기본 풀: pcvpool/vms (daemon.conf [storage] zvol_pool 오버라이드 가능)
 *   3노드 각 8TB SSD, recordsize=64K, primarycache=metadata
 * ──────────────────────────────────────────────────────────────
 */
#ifndef HANDLER_STORAGE_H
#define HANDLER_STORAGE_H

#include <json-glib/json-glib.h>
#include <gio/gio.h>
#include "api/uds_server.h"

/**
 * @brief ZFS 풀 목록을 조회한다 (동기).
 *
 * zfs list 명령으로 시스템의 모든 ZFS filesystem을 나열한다.
 * 각 풀의 이름, 사용량, 가용 공간, 마운트포인트를 JSON 배열로 반환.
 *
 * @param params  {} (파라미터 없음)
 * 응답 예시: [{"name":"pcvpool","used":"1.2T","avail":"5.7T","mountpoint":"/pcvpool"}, ...]
 */
void handle_storage_pool_list_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief zvol(ZFS 블록 디바이스) 목록을 조회한다 (동기).
 *
 * 설정된 zvol_pool (기본 pcvpool/vms) 하위의 모든 ZFS volume을 나열.
 * VM 디스크로 사용되는 /dev/zvol/<pool>/<name> 디바이스에 대응.
 *
 * @param params  {} (파라미터 없음) 또는 {"pool":"<pool_name>"} (특정 풀 지정)
 * 응답 예시: [{"name":"pcvpool/vms/web-prod","used":"10G","volsize":"50G"}, ...]
 */
void handle_storage_zvol_list_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief 새 zvol을 생성한다 (동기).
 *
 * VM 디스크용 ZFS block volume을 생성한다. 생성된 zvol은
 * /dev/zvol/<pool>/<name> 경로로 VM에 블록 디바이스로 마운트할 수 있다.
 *
 * @param params  {"name":"<zvol_name>", "size_gb":<int>}
 *                - name: zvol 이름 (pcv_validate_vm_name으로 검증)
 *                - size_gb: 디스크 크기(GB), 기본 50GB
 */
void handle_storage_zvol_create_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief zvol을 삭제한다 (동기).
 *
 * 지정된 zvol을 zfs destroy로 제거한다. 연결된 VM이 있으면 먼저 분리해야 한다.
 * 멱등성: 존재하지 않는 zvol 삭제 요청은 성공으로 처리 (재시도 안전).
 *
 * @param params  {"name":"<zvol_name>"} — 삭제할 zvol 이름
 */
void handle_storage_zvol_delete_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief ZFS 풀을 생성한다 (동기).
 *
 * zpool create 명령으로 새 풀을 생성한다.
 * ashift=12, compression=lz4, atime=off 기본 옵션 적용.
 *
 * @param params  {"name":"<pool>", "vdev_type":"mirror|raidz|raidz2|...", "disks":["sdb","sdc"]}
 */
void handle_storage_pool_create_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief ZFS 풀을 삭제한다 (동기).
 *
 * zpool destroy -f 명령으로 풀을 강제 삭제한다.
 * 풀 내 모든 데이터셋/스냅샷이 즉시 영구 삭제되므로 주의.
 *
 * @param params  {"name":"<pool>"}
 */
void handle_storage_pool_destroy_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief ZFS 풀 스크럽을 시작한다 (동기).
 *
 * zpool scrub 명령으로 데이터 무결성 검증을 시작한다.
 * 백그라운드에서 실행되며 완료까지 수 시간이 걸릴 수 있다.
 *
 * @param params  {"name":"<pool>"}
 */
void handle_storage_pool_scrub_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief ISO 파일 목록을 반환한다 (동기).
 * daemon.conf [storage] iso_dirs에서 .iso/.img 파일을 스캔한다.
 */
void handle_iso_list(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

#endif // HANDLER_STORAGE_H
