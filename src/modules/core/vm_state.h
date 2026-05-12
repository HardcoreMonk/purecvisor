




































































#ifndef PURECVISOR_VM_STATE_H
#define PURECVISOR_VM_STATE_H

#include <glib.h>

G_BEGIN_DECLS


























typedef enum {
    VM_OP_NONE      = 0,
    VM_OP_STARTING  = 1,
    VM_OP_STOPPING  = 2,
    VM_OP_DELETING  = 3,
    VM_OP_CREATING  = 4,
    VM_OP_TUNING    = 5,
    VM_OP_SNAPSHOT  = 6,
    VM_OP_MIGRATING = 7,
} VmPendingOp;

















void init_pending_state_machine(void);



















[[nodiscard]] gboolean lock_vm_operation(const gchar *vm_id, gint op_type, gchar **err_msg);











void unlock_vm_operation(const gchar *vm_id);










void shutdown_pending_state_machine(void);









gint pcv_vm_state_get_lock_count(void);









gint pcv_vm_state_cleanup_expired(void);









typedef enum {
    PCV_VM_UNKNOWN   = 0,
    PCV_VM_RUNNING   = 1,
    PCV_VM_STOPPED   = 2,
    PCV_VM_PAUSED    = 3,
    PCV_VM_MIGRATING = 4,
    PCV_VM_ERROR     = 5,
} PcvVmRuntimeState;







PcvVmRuntimeState pcv_vm_state_get_runtime(const gchar *vm_name);







const gchar *pcv_vm_state_runtime_str(PcvVmRuntimeState state);

G_END_DECLS

#endif
