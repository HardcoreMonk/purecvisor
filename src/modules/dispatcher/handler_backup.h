/**
 * @file handler_backup.h
 * @brief 백업 정책 관리 및 복원 RPC 핸들러 공개 인터페이스
 *
 * ============================================================================
 * [RPC 메서드 매핑] (5개)
 * ============================================================================
 *   backup.policy.set    -> handle_backup_policy_set    (동기 — 정책 등록/갱신)
 *   backup.policy.list   -> handle_backup_policy_list   (동기 — 전체 정책 목록)
 *   backup.policy.delete -> handle_backup_policy_delete (동기 — 정책 삭제)
 *   backup.history       -> handle_backup_history       (동기 — 스냅샷 이력)
 *   backup.restore       -> handle_backup_restore       (fire-and-forget — ZFS 롤백)
 *
 * ============================================================================
 * [핸들러 시그니처] (모든 디스패처 핸들러 공통)
 * ============================================================================
 *   void handler(JsonObject *params, const gchar *rpc_id,
 *                UdsServer *server, GSocketConnection *connection)
 *
 * ============================================================================
 * [호출 경로]
 * ============================================================================
 *   dispatcher.c -> handle_backup_*() -> backup_scheduler.c (ZFS 스냅샷 정책 DB)
 *
 * ============================================================================
 * [동기 vs 비동기 패턴 구분]
 * ============================================================================
 *   backup.restore/incremental/replicate는 fire-and-forget 비동기 패턴이고,
 *   policy.set/list/delete, history, verify 등은 동기 응답입니다.
 *
 *   동기 핸들러: 검증 → DB/ZFS 조작 → 결과 응답 전송
 *   fire-and-forget (restore/incremental/replicate):
 *     검증 → "accepted" 응답 전송(소켓 닫힘) → GTask 워커에서 장시간 작업 실행
 *     → 결과는 worker-result audit + WS 완료 이벤트로 기록(ADR-0018)
 *
 * ============================================================================
 * [CLI 사용 예시]
 * ============================================================================
 *   pcvctl backup set '*' --interval 24 --retention 7   (전체 VM 일일 백업, 7일 보존)
 *   pcvctl backup list                                   (정책 목록)
 *   pcvctl backup history web-prod                       (스냅샷 이력)
 *   pcvctl backup restore web-prod pcv-auto-20260326     (복원 요청)
 *
 * ============================================================================
 * [주의사항]
 * ============================================================================
 *   - UdsServer 전방 선언(typedef struct _UdsServer UdsServer)으로
 *     헤더 순환 참조를 방지합니다.
 *   - backup_scheduler.c가 5분 주기 GMainLoop 타이머로 정책을 확인하고
 *     자동 스냅샷을 생성합니다. 이 핸들러는 정책 등록/관리만 담당합니다.
 */

#ifndef PURECVISOR_HANDLER_BACKUP_H
#define PURECVISOR_HANDLER_BACKUP_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* UdsServer 전방 선언 (헤더 순환 참조 방지) */
typedef struct _UdsServer UdsServer;

/**
 * @brief 백업 정책 설정/갱신 — UPSERT 동작 (backup.policy.set)
 * @param params: {vm_name, interval_hours (>=1), retention_count (>=1)}
 *   vm_name에 '*' 지정 시 전체 VM 일괄 백업 정책.
 *   이미 정책이 존재하면 덮어쓰기 (UPSERT).
 * 반환: true (boolean)
 */
void handle_backup_policy_set(JsonObject       *params,
                               const gchar      *rpc_id,
                               UdsServer        *server,
                               GSocketConnection *connection);

/**
 * @brief 백업 정책 전체 목록 (backup.policy.list)
 * @param params: {} (불필요)
 * 반환: [{"vm_name","interval_hours","retention_count","enabled"}, ...]
 */
void handle_backup_policy_list(JsonObject       *params,
                                const gchar      *rpc_id,
                                UdsServer        *server,
                                GSocketConnection *connection);

/**
 * @brief 백업 정책 삭제 (backup.policy.delete)
 * @param params: {vm_name}
 * 반환: true (boolean). 미존재 정책 삭제 시 -32000 에러.
 */
void handle_backup_policy_delete(JsonObject       *params,
                                  const gchar      *rpc_id,
                                  UdsServer        *server,
                                  GSocketConnection *connection);

/**
 * @brief 자동 백업 스냅샷 이력 조회 (backup.history)
 * @param params: {vm_name}
 * 반환: ["snap1", "snap2", ...] (ZFS 스냅샷 이름 문자열 배열)
 */
void handle_backup_history(JsonObject       *params,
                            const gchar      *rpc_id,
                            UdsServer        *server,
                            GSocketConnection *connection);

/**
 * @brief 스냅샷 복원 — fire-and-forget 비동기 패턴 (backup.restore)
 * @param params: {vm_name, snapshot_name}
 *
 * 즉시 {"status":"accepted"} 응답 전송 후 GTask 워커 스레드에서 ZFS rollback 실행.
 * ZFS rollback은 디스크 크기에 따라 수 초~수 분 소요되므로 비동기 처리 필수.
 * 워커 결과는 journalctl 로그에만 기록됩니다 (콜백에서 send_response 호출 금지).
 */
void handle_backup_restore(JsonObject       *params,
                            const gchar      *rpc_id,
                            UdsServer        *server,
                            GSocketConnection *connection);

/**
 * @brief 증분 백업 생성 (backup.incremental)
 * @param params: {name}
 *
 * 최신 스냅샷 대비 증분 스냅샷을 생성하고 증분 스트림을 파일로 저장.
 * fire-and-forget(STO-5): 즉시 {status:accepted, vm_name} 응답 후 GTask에서 실행,
 * 결과는 audit/WS 완료 이벤트로 기록.
 */
void handle_backup_incremental(JsonObject       *params,
                                const gchar      *rpc_id,
                                UdsServer        *server,
                                GSocketConnection *connection);

/**
 * @brief 백업 스냅샷 무결성 검증 (backup.verify)
 * @param params: {name, snapshot}
 *
 * zfs send -n (dry-run) 으로 스냅샷 무결성을 확인.
 * 동기 응답: {verified, snapshot, size_bytes, integrity}
 */
void handle_backup_verify(JsonObject       *params,
                           const gchar      *rpc_id,
                           UdsServer        *server,
                           GSocketConnection *connection);

/**
 * @brief 크로스 노드 백업 복제 — fire-and-forget 비동기 패턴 (backup.replicate)
 * @param params: {name, target_node, ssh_user (optional)}
 *
 * 즉시 {"status":"accepted"} 응답 전송 후 GTask 워커 스레드에서 ZFS send/recv 실행.
 * 대상에 기존 스냅샷이 있으면 증분, 없으면 풀 전송.
 */
void handle_backup_replicate(JsonObject       *params,
                              const gchar      *rpc_id,
                              UdsServer        *server,
                              GSocketConnection *connection);

/**
 * @brief 스냅샷 스케줄 설정 — backup policy set 래퍼 (vm.snapshot.schedule.set)
 * @param params: {vm_name, interval_hours (>=1), retention_count (>=1)}
 * 반환: true (boolean)
 */
void handle_snapshot_schedule_set(JsonObject       *params,
                                  const gchar      *rpc_id,
                                  UdsServer        *server,
                                  GSocketConnection *connection);

/**
 * @brief 스냅샷 스케줄 목록 조회 — backup policy list 래퍼 (vm.snapshot.schedule.list)
 * @param params: {} (불필요)
 * 반환: [{"vm_name","interval_hours","retention_count","enabled"}, ...]
 */
void handle_snapshot_schedule_list(JsonObject       *params,
                                   const gchar      *rpc_id,
                                   UdsServer        *server,
                                   GSocketConnection *connection);

/**
 * @brief 스냅샷 스케줄 삭제 — backup policy delete 래퍼 (vm.snapshot.schedule.delete)
 * @param params: {vm_name}
 * 반환: true (boolean). 미존재 시 -32000 에러.
 */
void handle_snapshot_schedule_delete(JsonObject       *params,
                                     const gchar      *rpc_id,
                                     UdsServer        *server,
                                     GSocketConnection *connection);

G_END_DECLS

#endif /* PURECVISOR_HANDLER_BACKUP_H */
