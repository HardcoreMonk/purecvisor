/* src/api/uds_server.h */
#ifndef PURECVISOR_UDS_SERVER_H
#define PURECVISOR_UDS_SERVER_H

// GObject 의존성 포함
#include <glib-object.h>

typedef struct _UdsServer UdsServer;

// 생성자 (서버 시작 포함)
UdsServer* uds_server_new(const char *socket_path);

// 소멸자 (서버 중지 포함)
void uds_server_free(UdsServer *server);

#endif // PURECVISOR_UDS_SERVER_H