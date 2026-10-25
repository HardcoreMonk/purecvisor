#ifndef PURECVISOR_LXC_NS_EXEC_H
#define PURECVISOR_LXC_NS_EXEC_H

#include <glib.h>
#include <sys/types.h>

#include "utils/pcv_privdrop.h"

G_BEGIN_DECLS

#define PCV_LXC_NS_PATH_ENV          "PATH=/usr/sbin:/usr/bin:/sbin:/bin"

#define PCV_LXC_NS_READ_TIMEOUT_SEC   3

#define PCV_LXC_NS_CHANGE_TIMEOUT_SEC 5

#define PCV_LXC_NS_WIRING_BUDGET_SEC  6

#define PCV_LXC_EXEC_TIMEOUT_SEC      60

#define PCV_LXC_NS_OUTPUT_MAX         ((gsize)64 * 1024)

#define PCV_LXC_EXEC_OUTPUT_MAX       ((gsize)4 * 1024 * 1024)

gchar  **pcv_lxc_attach_argv(const gchar *lxc_path, const gchar *name, const gchar * const *cmd);

gboolean pcv_lxc_ns_run(const gchar * const *argv, guint timeout_sec, gsize max_output,
                        gchar **stdout_out, gchar **stderr_out, gint *exit_status, GError **error);

gboolean pcv_lxc_ns_ip4_query(const gchar *lxc_path, const gchar *name, gchar **out, GError **error);

gboolean pcv_lxc_ns_nic_remove(const gchar *lxc_path, const gchar *name, const gchar *nic,
                               gchar **warning_out, GError **error);

gchar  **pcv_lxc_ns_nic_wire_steps(const gchar *host_if, const gchar *ctr_if, const gchar *bridge,
                                   pid_t init_pid);

gboolean pcv_lxc_ns_nic_wire(const gchar *host_if, const gchar *ctr_if, const gchar *bridge,
                             pid_t init_pid, GError **error);

void     pcv_lxc_ns_set_abort_fn_for_test(gboolean (*fn)(void));

void     pcv_lxc_ns_set_spawn_observer_for_test(void (*fn)(const gchar * const *argv,
                                                           PcvChildCapabilityProfile profile));

G_END_DECLS

#endif
