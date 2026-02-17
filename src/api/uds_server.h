/* src/api/uds_server.h */
#ifndef PURECVISOR_UDS_SERVER_H
#define PURECVISOR_UDS_SERVER_H

#include <glib-object.h>
#include "../modules/virt/vm_manager.h" 

G_BEGIN_DECLS

/* * [수정됨] 두 번째 인자를 'purecvisor_uds_server'로 변경했습니다.
 * 이제 이 매크로는 'purecvisor_uds_server_get_type' 함수를 선언합니다.
 * 이는 C 파일의 G_DEFINE_TYPE 생성 결과와 정확히 일치합니다.
 */
#define PURECVISOR_TYPE_UDS_SERVER (purecvisor_uds_server_get_type())

G_DECLARE_FINAL_TYPE(UdsServer, purecvisor_uds_server, PURECVISOR, UDS_SERVER, GObject)

UdsServer *uds_server_new(const gchar *socket_path);

void uds_server_set_vm_manager(UdsServer *self, VmManager *mgr);
gboolean uds_server_start(UdsServer *self, GError **error);
void uds_server_stop(UdsServer *self);

G_END_DECLS

#endif /* PURECVISOR_UDS_SERVER_H */