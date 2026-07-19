
#include <glib.h>
#include <stdio.h>

typedef enum {
    VM_OP_STARTING = 1,
    VM_OP_STOPPING,
    VM_OP_DELETING,
    VM_OP_TUNING,
    VM_OP_SNAPSHOT
} VmPendingOp;

void init_pending_state_machine(void);

gboolean lock_vm_operation(const gchar *vm_id, VmPendingOp op, gchar **out_error_msg);

void unlock_vm_operation(const gchar *vm_id);

static GHashTable *vm_pending_locks = NULL;

void init_pending_state_machine(void) {
    if (vm_pending_locks != NULL) {
        g_warning("Pending state machine is already initialized.");
        return;
    }

    vm_pending_locks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_message("✅ [Core] Lock-Free VM Pending State Machine initialized.");
}

gboolean lock_vm_operation(const gchar *vm_id, VmPendingOp op, gchar **out_error_msg) {
    if (G_UNLIKELY(vm_pending_locks == NULL)) {
        if (out_error_msg) *out_error_msg = g_strdup("State machine not initialized.");
        return FALSE;
    }

    if (g_hash_table_contains(vm_pending_locks, vm_id)) {

        VmPendingOp current_op = (VmPendingOp)GPOINTER_TO_INT(g_hash_table_lookup(vm_pending_locks, vm_id));

        if (out_error_msg) {
            *out_error_msg = g_strdup_printf(
                "Conflict: VM '%s' is currently busy with operation code %d. Please try again later.",
                vm_id, current_op
            );
        }
        g_debug("🔒 [State] Lock denied for VM %s (Currently executing op: %d)", vm_id, current_op);
        return FALSE;
    }

    g_hash_table_insert(vm_pending_locks, g_strdup(vm_id), GINT_TO_POINTER(op));
    g_debug("🔓 [State] Lock acquired for VM %s (Op: %d)", vm_id, op);

    return TRUE;
}

void unlock_vm_operation(const gchar *vm_id) {
    if (G_UNLIKELY(vm_pending_locks == NULL)) return;

    gboolean removed = g_hash_table_remove(vm_pending_locks, vm_id);

    if (removed) {
        g_debug("🔓 [State] Lock released for VM %s", vm_id);
    } else {

        g_warning("⚠️ [State] Attempted to unlock VM %s, but no lock was found.", vm_id);
    }
}
