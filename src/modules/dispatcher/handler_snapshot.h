/**
 * @file handler_snapshot.h
 * @brief VM ZFS 스냅샷 RPC 핸들러 공개 인터페이스
 *
 * ──────────────────────────────────────────────────────────────
 * [아키텍처 내 위치]
 *   dispatcher.c → 이 파일의 핸들러 → zfs_driver.c (ZFS 스냅샷 조작)
 *   VM의 ZFS zvol 스냅샷을 생성/조회/롤백/삭제한다.
 *
 * [RPC 메서드 매핑] (4개)
 *   vm.snapshot.create   -> handle_vm_snapshot_create   (동기 — zfs snapshot)
 *   vm.snapshot.list     -> handle_vm_snapshot_list     (동기 — zfs list -t snapshot)
 *   vm.snapshot.rollback -> handle_vm_snapshot_rollback (fire-and-forget — VM 중지→zfs rollback→재기동)
 *   vm.snapshot.delete   -> handle_vm_snapshot_delete   (동기 — zfs destroy snapshot)
 *
 * [REST API 매핑]
 *   GET    /api/v1/vms/{name}/snapshot            -> vm.snapshot.list
 *   POST   /api/v1/vms/{name}/snapshot/create     -> vm.snapshot.create
 *   POST   /api/v1/vms/{name}/snapshot/rollback   -> vm.snapshot.rollback
 *   DELETE /api/v1/vms/{name}/snapshot/{snap}      -> vm.snapshot.delete
 *
 * [핸들러 시그니처] (모든 디스패처 핸들러 공통)
 *   void handler(JsonObject *params, const gchar *rpc_id,
 *                UdsServer *server, GSocketConnection *connection)
 *
 * [호출 경로]
 *   dispatcher.c -> handle_vm_snapshot_*() -> zfs_driver.c (ZFS 스냅샷 조작)
 *                                          -> virt_conn_pool.c (rollback 시 VM 제어)
 *
 * [롤백 동작 상세]
 *   vm.snapshot.rollback는 fire-and-forget 패턴:
 *   1. 즉시 "accepted" 응답 전송
 *   2. GTask 백그라운드에서:
 *      a. VM이 실행 중이면 virDomainDestroy로 강제 종료
 *      b. zfs rollback -r <zvol>@<snapshot> 실행
 *      c. VM 재기동 (virDomainCreate)
 * ──────────────────────────────────────────────────────────────
 */
#ifndef PURECVISOR_HANDLER_SNAPSHOT_H
#define PURECVISOR_HANDLER_SNAPSHOT_H

#include <glib.h>
#include <gio/gio.h>                 /* GSocketConnection 타입 참조 */
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* UdsServer 전방 선언 (헤더 순환 참조 방지) */
typedef struct _UdsServer UdsServer;

/**
 * @brief VM ZFS 스냅샷을 생성한다 (vm.snapshot.create, 동기).
 * @param params  {"name":"<vm_name>", "snapshot":"<snap_name>"}
 *                snap_name 생략 시 타임스탬프 기반 이름 자동 생성
 */
void handle_vm_snapshot_create(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief VM ZFS 스냅샷 목록을 조회한다 (vm.snapshot.list, 동기).
 * @param params  {"name":"<vm_name>"}
 * 응답: JSON 배열 [{"snapshot":"snap1", "used":"128K", "creation":"2026-03-20"}, ...]
 */
void handle_vm_snapshot_list(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief VM을 ZFS 스냅샷 시점으로 롤백한다 (vm.snapshot.rollback, fire-and-forget).
 * @param params  {"name":"<vm_name>", "snapshot":"<snap_name>"}
 * 주의: VM이 실행 중이면 강제 종료 후 롤백, 이후 자동 재기동
 */
void handle_vm_snapshot_rollback(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief VM ZFS 스냅샷을 삭제한다 (vm.snapshot.delete, 동기).
 * @param params  {"name":"<vm_name>", "snapshot":"<snap_name>"}
 * 멱등성: 존재하지 않는 스냅샷 삭제 시 성공 반환
 */
void handle_vm_snapshot_delete(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief VM ZFS 스냅샷 일괄 삭제 (vm.snapshot.delete_all, 동기).
 * @param params  {"name":"<vm_name>", "prefix":"pcv-repl-"(선택), "keep_recent":10(선택)}
 *   prefix: 해당 접두사 스냅샷만 삭제 (NULL이면 전체)
 *   keep_recent: 최근 N개 보존 (0이면 전부 삭제)
 * 응답: {"deleted":N, "total_before":M, "remaining":R}
 */
void handle_vm_snapshot_delete_all(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

G_END_DECLS

#endif /* PURECVISOR_HANDLER_SNAPSHOT_H */
