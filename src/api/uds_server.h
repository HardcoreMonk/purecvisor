/* src/api/uds_server.h */
#ifndef PURECVISOR_UDS_SERVER_H
#define PURECVISOR_UDS_SERVER_H

// GObject 의존성 포함
#include <glib-object.h>
#include "modules/virt/vm_manager.h" // 추가

typedef struct _UdsServer UdsServer;

// 생성자 (서버 시작 포함)
UdsServer* uds_server_new(const char *socket_path);

// 소멸자 (서버 중지 포함)
void uds_server_free(UdsServer *server);

// [New] VmManager 설정
void uds_server_set_vm_manager(UdsServer *server, VmManager *mgr);

#endif // PURECVISOR_UDS_SERVER_H