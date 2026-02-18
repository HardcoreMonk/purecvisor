/* src/api/dispatcher.h */
#ifndef PURECVISOR_DISPATCHER_H
#define PURECVISOR_DISPATCHER_H

#include <glib-object.h>
#include <gio/gio.h>
#include "../modules/virt/vm_manager.h" // VmManager 헤더 추가

G_BEGIN_DECLS

#define PURECVISOR_TYPE_DISPATCHER (purecvisor_dispatcher_get_type())

/* [CRITICAL FIX] Type Declaration & Forward Declaration */
typedef struct _PureCVisorDispatcher PureCVisorDispatcher;
G_DECLARE_FINAL_TYPE(PureCVisorDispatcher, purecvisor_dispatcher, PURECVISOR, DISPATCHER, GObject)

PureCVisorDispatcher *purecvisor_dispatcher_new(void);

void purecvisor_dispatcher_dispatch(PureCVisorDispatcher *self,
                                   JsonNode *request_node,
                                   GOutputStream *output);

// [FIX] Old type 'VmManager' removed, use 'PureCVisorVmManager'
// This function might not be needed if 'new' handles initialization internally,
// but keeping definition correct just in case.
void dispatcher_set_vm_manager(PureCVisorDispatcher *self, PureCVisorVmManager *mgr);

G_END_DECLS

#endif // PURECVISOR_DISPATCHER_H