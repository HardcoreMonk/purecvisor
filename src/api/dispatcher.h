/* src/api/dispatcher.h */
#ifndef PURECVISOR_DISPATCHER_H
#define PURECVISOR_DISPATCHER_H

#include <glib-object.h>
#include <gio/gio.h>
#include "modules/virt/vm_manager.h" // VmManager 헤더 추가

G_BEGIN_DECLS

typedef struct _Dispatcher Dispatcher;

/**
 * Dispatcher 생성자
 */
Dispatcher* dispatcher_new(void);

/**
 * Dispatcher 소멸자
 */
void dispatcher_free(Dispatcher *self);

/**
 * 수신된 라인(JSON) 처리
 * @param self Dispatcher 인스턴스
 * @param stream 클라이언트와의 양방향 스트림 (응답 전송용)
 * @param line 수신된 JSON 문자열 (NULL-terminated)
 */
void dispatcher_process_line(Dispatcher *self, GIOStream *stream, const gchar *line);

// [New] VmManager 주입 함수
void dispatcher_set_vm_manager(Dispatcher *self, VmManager *mgr);

G_END_DECLS

#endif // PURECVISOR_DISPATCHER_H