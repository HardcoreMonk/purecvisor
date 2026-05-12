









































#ifndef PURECVISOR_DISPATCHER_HANDLER_VM_HOTPLUG_H
#define PURECVISOR_DISPATCHER_HANDLER_VM_HOTPLUG_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include "api/uds_server.h"

G_BEGIN_DECLS





void handle_vm_set_memory_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);





void handle_vm_set_vcpu_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);





void handle_device_disk_attach(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);





void handle_device_disk_detach(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);






void handle_device_nic_list  (JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);






void handle_device_nic_attach(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);





void handle_device_nic_detach(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);





void handle_vm_mount_iso(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);





void handle_vm_eject_iso(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);








void handle_vm_pin_vcpu(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);







void handle_vm_set_bandwidth(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);










void handle_vm_memory_stats_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);









void handle_vm_cpu_stats_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);








void handle_vm_disk_live_resize_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);









void handle_vm_blkio_set(JsonObject *params, const gchar *rpc_id,
                          UdsServer *server, GSocketConnection *connection);








void handle_vm_blkio_get(JsonObject *params, const gchar *rpc_id,
                          UdsServer *server, GSocketConnection *connection);











void handle_vm_usb_attach(JsonObject *params, const gchar *rpc_id,
                           UdsServer *server, GSocketConnection *connection);





void handle_vm_usb_detach(JsonObject *params, const gchar *rpc_id,
                           UdsServer *server, GSocketConnection *connection);






void handle_vm_usb_list(JsonObject *params, const gchar *rpc_id,
                         UdsServer *server, GSocketConnection *connection);

G_END_DECLS

#endif
