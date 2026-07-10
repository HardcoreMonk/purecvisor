/* src/modules/virt/cancellable_map.h
 *
 * Sprint C-2 / GIO P4: VM name → GCancellable 해시맵
 *
 * [목적]
 *   GTask 워커 스레드에 전달된 GCancellable 을 VM 이름으로 조회·취소할 수 있게 함.
 *   사용 사례:
 *     - VM 삭제 요청 수신 시 진행 중인 create/start GTask 강제 취소
 *     - 데몬 graceful drain 시 진행 중인 장기 작업 일괄 취소
 *
 * [쓰레드 안전성]
 *   GMutex 으로 모든 맵 접근을 보호. cmap_* 함수는 어느 스레드에서도 호출 가능.
 */

#ifndef PURECVISOR_CANCELLABLE_MAP_H
#define PURECVISOR_CANCELLABLE_MAP_H

#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

/**
 * cmap_init:
 * 해시맵 초기화. main.c 에서 1회 호출.
 */
void cmap_init(void);

/**
 * cmap_shutdown:
 * 진행 중인 모든 작업 취소 후 해시맵 해제. main.c cleanup 에서 호출.
 */
void cmap_shutdown(void);

/**
 * cmap_register:
 * @vm_name:     VM 이름 (키).
 * @cancellable: GTask 에 전달할 GCancellable. 이미 동일 키가 있으면 교체.
 *
 * GCancellable 의 ref 를 맵이 소유. 호출자는 별도 ref 없이 g_object_unref 해도 됨.
 */
void cmap_register(const gchar *vm_name, GCancellable *cancellable);

/**
 * cmap_cancel:
 * @vm_name: 취소할 VM 이름.
 *
 * 해당 GCancellable 이 존재하면 g_cancellable_cancel() 호출.
 * 없으면 무시.
 */
void cmap_cancel(const gchar *vm_name);

/**
 * cmap_remove:
 * @vm_name: 제거할 VM 이름.
 *
 * 맵에서 항목 제거 + GCancellable unref.
 * GTask 완료 콜백에서 호출하여 맵 누수를 방지.
 */
void cmap_remove(const gchar *vm_name);

/**
 * cmap_cancel_all:
 * 등록된 모든 GCancellable 을 취소. drain 시작 시 호출.
 */
void cmap_cancel_all(void);

/**
 * cmap_size:
 * 현재 등록된 항목 수 반환 (진단/테스트용).
 */
guint cmap_size(void);

G_END_DECLS

#endif /* PURECVISOR_CANCELLABLE_MAP_H */
