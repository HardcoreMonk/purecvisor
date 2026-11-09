                                                                             
                                
                                                                           
  
                           
                                                                                  
                                                                                      
                                                                     
                                                                         
                                                              
                                                           
                                                                         
                                                                                                
                                                                                          
                                                                                               
                                                                              
  
                                                             
                                                        
                                                                                
#include "lxc_ns_exec.h"

#include <gio/gio.h>
#include <net/if.h>                                                                    

#include "api/drain.h"
#include "utils/pcv_log.h"
#include "utils/pcv_spawn.h"

#define NS_LOG_DOM "lxc_ns_exec"
                                                            
#define NS_WIRE_SEP "\x1f"
                                                                
#define NS_WIRE_PREFIX "config saved; live wiring not confirmed: "

typedef gboolean (*NsAbortFn)(void);

                                                                                   
static NsAbortFn G_ns_abort_fn_test;

                                                                               
                                        
static NsAbortFn
_ns_abort_fn(void)
{
    NsAbortFn fn = g_atomic_pointer_get(&G_ns_abort_fn_test);
    return fn ? fn : pcv_drain_is_terminating;
}

typedef void (*NsSpawnObserver)(const gchar * const *argv, PcvChildCapabilityProfile profile);

                                                             
static NsSpawnObserver G_ns_spawn_observer_test;

                                                                          
                                                                     
static gboolean
_ns_bounded(const gchar * const *argv, const PcvSpawnBound *bound, gchar **stdout_out,
            gchar **stderr_out, gint *exit_status, GError **error)
{
    NsSpawnObserver obs = g_atomic_pointer_get(&G_ns_spawn_observer_test);
    if (obs)
        obs(argv, bound->profile);
    return pcv_spawn_sync_bounded(argv, bound, stdout_out, stderr_out, exit_status, error);
}

                                                     
                                                                       
gchar **
pcv_lxc_attach_argv(const gchar *lxc_path, const gchar *name, const gchar * const *cmd)
{
    g_return_val_if_fail(lxc_path && *lxc_path && name && *name && cmd && cmd[0], NULL);
    const gchar *head[] = { "lxc-attach", "--clear-env", "--set-var", PCV_LXC_NS_PATH_ENV,
                            "-P", lxc_path, "-n", name, "--" };
    GPtrArray *a = g_ptr_array_new();
    for (gsize i = 0; i < G_N_ELEMENTS(head); i++)
        g_ptr_array_add(a, g_strdup(head[i]));
    for (gsize i = 0; cmd[i]; i++)                                        
        g_ptr_array_add(a, g_strdup(cmd[i]));
    g_ptr_array_add(a, NULL);
    return (gchar **)g_ptr_array_free(a, FALSE);
}

                                                   
                                                                  
gboolean
pcv_lxc_ns_run(const gchar * const *argv, guint timeout_sec, gsize max_output,
               gchar **stdout_out, gchar **stderr_out, gint *exit_status, GError **error)
{
                                                                                  
                                                                              
                                                                          
                                                                                                         
    const PcvSpawnBound bound = {
        .profile           = PCV_CHILD_CAP_RUNTIME,                                                     
        .timeout_sec       = timeout_sec,
        .max_output_bytes  = max_output,
        .cancellable       = NULL,
        .should_abort      = _ns_abort_fn(),
        .new_process_group = TRUE,
    };
    return _ns_bounded(argv, &bound, stdout_out, stderr_out, exit_status, error);
}

                                                                    
                                   
static const gchar *
_ns_stderr_or(gchar *err_out, const gchar *fallback)
{
    return err_out && *g_strstrip(err_out) ? err_out : fallback;
}

                                                                        
                                                                     
gboolean
pcv_lxc_ns_ip4_query(const gchar *lxc_path, const gchar *name, gchar **out, GError **error)
{
    g_return_val_if_fail(out, FALSE);
    *out = NULL;
    const gchar *cmd[] = { "ip", "-4", "-o", "addr", "show", NULL };
    gchar **argv = pcv_lxc_attach_argv(lxc_path, name, cmd);
    g_return_val_if_fail(argv, FALSE);
    gchar *std_out = NULL;
    gint status = -1;
                                                               
    gboolean ok = pcv_lxc_ns_run((const gchar * const *)argv, PCV_LXC_NS_READ_TIMEOUT_SEC,
                                 PCV_LXC_NS_OUTPUT_MAX, &std_out, NULL, &status, error);
    g_strfreev(argv);
    if (ok && status != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "ip -4 -o addr show inside '%s' exited %d", name, status);
        ok = FALSE;
    }
    if (ok)
        *out = g_steal_pointer(&std_out);
    g_free(std_out);
    return ok;
}

                                                                     
                                                                 
gboolean
pcv_lxc_ns_nic_remove(const gchar *lxc_path, const gchar *name, const gchar *nic,
                      gchar **warning_out, GError **error)
{
    if (warning_out)
        *warning_out = NULL;
    const gchar *cmd[] = { "ip", "link", "del", nic, NULL };
    gchar **argv = pcv_lxc_attach_argv(lxc_path, name, cmd);
    g_return_val_if_fail(argv, FALSE);
    gchar *err_out = NULL;
    gint status = -1;
    GError *run_err = NULL;
    gboolean ran = pcv_lxc_ns_run((const gchar * const *)argv, PCV_LXC_NS_CHANGE_TIMEOUT_SEC,
                                  PCV_LXC_NS_OUTPUT_MAX, NULL, &err_out, &status, &run_err);
    g_strfreev(argv);
    if (!ran)                                                     
        g_propagate_prefixed_error(error, run_err,
                                   "config removed; live interface removal not confirmed: ");
    else if (status != 0 && warning_out)                                          
        *warning_out = g_strdup_printf("live interface removal of '%s' exited %d: %s", nic, status,
                                       _ns_stderr_or(err_out, "(no stderr)"));
    g_free(err_out);
    return ran;
}

                                                                  
                                                                  
gchar **
pcv_lxc_ns_nic_wire_steps(const gchar *host_if, const gchar *ctr_if, const gchar *bridge,
                          pid_t init_pid)
{
    g_return_val_if_fail(host_if && ctr_if && bridge, NULL);
    gchar pid[24];
    g_snprintf(pid, sizeof(pid), "%d", (gint)init_pid);
    gchar **steps = g_new0(gchar *, 5);
    steps[0] = g_strjoin(NS_WIRE_SEP, "ip", "link", "add", host_if, "type", "veth",
                         "peer", "name", ctr_if, "netns", pid, NULL);
    steps[1] = g_strjoin(NS_WIRE_SEP, "ip", "link", "set", host_if, "master", bridge, NULL);
    steps[2] = g_strjoin(NS_WIRE_SEP, "ip", "link", "set", host_if, "up", NULL);
    steps[3] = g_strjoin(NS_WIRE_SEP, "nsenter", "-t", pid, "-n", "--",
                         "ip", "link", "set", ctr_if, "up", NULL);
    return steps;
}

                                                                     
                                                                                                      
                                                                                               
                                                                                        
                                                                                                          
                                                                        
                                                                             
                                                                 
                                                               
                                                                        
static gboolean
_ns_host_run(const gchar * const *argv, gboolean opens_netns, gboolean honor_abort, guint timeout_sec,
             gchar **stderr_out, gint *exit_status, GError **error)
{
    PcvSpawnBound bound = {
        .profile           = PCV_CHILD_CAP_BASE,
        .timeout_sec       = timeout_sec,
        .max_output_bytes  = PCV_LXC_NS_OUTPUT_MAX,
        .cancellable       = NULL,
        .should_abort      = honor_abort ? _ns_abort_fn() : NULL,
        .new_process_group = TRUE,
    };
    if (opens_netns)
        bound.profile = PCV_CHILD_CAP_RUNTIME;
    return _ns_bounded(argv, &bound, NULL, stderr_out, exit_status, error);
}

                                                         
                                   
static guint
_ns_left_sec(gint64 deadline)
{
    gint64 left = deadline - g_get_monotonic_time();
    return left > 0 ? (guint)((left + G_USEC_PER_SEC - 1) / G_USEC_PER_SEC) : 0;
}

                                                                                     
                                                                                                   
                                                                                               
                                                                              
                                                                 
static gboolean
_ns_wire_step(const gchar * const *argv, gboolean opens_netns, gint64 deadline, gboolean *exited,
              GError **error)
{
    *exited = FALSE;
    guint left = _ns_left_sec(deadline);
    if (left == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "wiring budget %us exhausted",
                    (guint)PCV_LXC_NS_WIRING_BUDGET_SEC);
        return FALSE;
    }
    gchar *err_out = NULL;
    gint status = -1;
    gboolean ran = g_str_equal(argv[0], "nsenter")
        ? pcv_lxc_ns_run(argv, left, PCV_LXC_NS_OUTPUT_MAX, NULL, &err_out, &status, error)
        : _ns_host_run(argv, opens_netns, TRUE, left, &err_out, &status, error);
    *exited = ran;
    if (ran && status != 0)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "exit %d: %s", status,
                    _ns_stderr_or(err_out, "(no stderr)"));
    g_free(err_out);
    return ran && status == 0;
}

                                                                         
          
                                                                  
gboolean
pcv_lxc_ns_nic_wire(const gchar *host_if, const gchar *ctr_if, const gchar *bridge,
                    pid_t init_pid, GError **error)
{
    g_return_val_if_fail(host_if && ctr_if && bridge, FALSE);
    if (init_pid <= 1) {                                                     
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, NS_WIRE_PREFIX "no init pid");
        return FALSE;
    }
                                                                               
                                                                      
                    
    if (if_nametoindex(host_if) != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    NS_WIRE_PREFIX "host interface %s already exists", host_if);
        return FALSE;
    }
    const gint64 deadline = g_get_monotonic_time() +
                            (gint64)PCV_LXC_NS_WIRING_BUDGET_SEC * G_USEC_PER_SEC;
    gchar **steps = pcv_lxc_ns_nic_wire_steps(host_if, ctr_if, bridge, init_pid);
    GError *step_err = NULL;
    gboolean nothing_made = FALSE;                                            
    for (guint i = 0; steps[i] && !step_err; i++) {
        gchar **argv = g_strsplit(steps[i], NS_WIRE_SEP, -1);
        gboolean exited = FALSE;
        if (!_ns_wire_step((const gchar * const *)argv, i == 0, deadline, &exited, &step_err)) {
            nothing_made = i == 0 && exited;
            gchar *line = g_strjoinv(" ", argv);
            g_prefix_error(&step_err, NS_WIRE_PREFIX "%s: ", line);
            g_free(line);
        }
        g_strfreev(argv);
    }
    g_strfreev(steps);
    if (!step_err)
        return TRUE;

                                                                              
                                                                           
    if (!nothing_made) {
                                                                           
                                                                            
        const gchar *del[] = { "ip", "link", "del", host_if, NULL };
        guint left = _ns_left_sec(deadline);
        gint status = -1;
        GError *del_err = NULL;
        if (!_ns_host_run(del, FALSE, FALSE, left ? left : 1, NULL, &status, &del_err) || status != 0)
            PCV_LOG_WARN(NS_LOG_DOM, "nic wiring cleanup 'ip link del %s' not confirmed: %s", host_if,
                         del_err ? del_err->message : "non-zero exit");
        g_clear_error(&del_err);
    }
    g_propagate_error(error, step_err);
    return FALSE;
}

                                                             
                                                                            
gboolean
pcv_lxc_ns_host_veth_name(const gchar *ctr_name, guint nic_index, gchar out[16])
{
    g_return_val_if_fail(ctr_name && out, FALSE);
    if (nic_index >= 16)
        return FALSE;
    g_autofree gchar *sum = g_compute_checksum_for_string(G_CHECKSUM_SHA256, ctr_name, -1);
    g_snprintf(out, 16, "vh%.8sn%u", sum, nic_index);                                
    return TRUE;
}

                                                           
                                                             
PcvLxcHostVethResult
pcv_lxc_ns_host_veth_remove(const gchar *host_if, GError **error)
{
    g_return_val_if_fail(host_if, PCV_LXC_HOST_VETH_FAILED);
    if (if_nametoindex(host_if) == 0)
        return PCV_LXC_HOST_VETH_ABSENT;
    const gchar *del[] = { "ip", "link", "del", host_if, NULL };
    gchar *err_out = NULL;
    gint status = -1;
    GError *run_err = NULL;
    gboolean ran = _ns_host_run(del, FALSE, FALSE, PCV_LXC_NS_CHANGE_TIMEOUT_SEC, &err_out, &status,
                                &run_err);
    PcvLxcHostVethResult res = PCV_LXC_HOST_VETH_REMOVED;
    if (!ran) {
        g_propagate_error(error, run_err);
        res = PCV_LXC_HOST_VETH_FAILED;
    } else if (status != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "ip link del %s exited %d: %s", host_if,
                    status, _ns_stderr_or(err_out, "(no stderr)"));
        res = PCV_LXC_HOST_VETH_FAILED;
    }
    g_free(err_out);
    return res;
}

                                                                                      
                                                                             
gboolean
pcv_lxc_ns_nic_detach_host_leftover(const gchar *name, guint nic_index, gchar **warning_out,
                                    GError **error)
{
    gchar host_if[16];
    if (!pcv_lxc_ns_host_veth_name(name, nic_index, host_if))
        return FALSE;
    GError *rm_err = NULL;
    PcvLxcHostVethResult r = pcv_lxc_ns_host_veth_remove(host_if, &rm_err);
    if (r == PCV_LXC_HOST_VETH_REMOVED) {
        if (warning_out) {
                                                                                                        
            const gchar *cause = error && *error ? (*error)->message : "unknown";
            static const gchar prefix[] = "config removed; live interface removal not confirmed: ";
            if (g_str_has_prefix(cause, prefix))
                cause += sizeof(prefix) - 1;
            *warning_out = g_strdup_printf(
                "live interface removal not confirmed in the container (%s); removed host interface %s",
                cause, host_if);
        }
        g_clear_error(error);
        return TRUE;
    }
    if (r == PCV_LXC_HOST_VETH_FAILED) {
        PCV_LOG_WARN(NS_LOG_DOM, "host veth %s removal not confirmed: %s", host_if,
                     rm_err ? rm_err->message : "unknown");
    } else {                                                                   
        gchar legacy[24];
        g_snprintf(legacy, sizeof(legacy), "eth%u-host", nic_index);
        if (error && *error && if_nametoindex(legacy) != 0) {
            g_autofree gchar *msg = g_strdup_printf(
                "%s; legacy host interface %s left in place (it may belong to another container)",
                (*error)->message, legacy);
            GQuark dom = (*error)->domain;
            gint code = (*error)->code;
            g_clear_error(error);
            g_set_error_literal(error, dom, code, msg);
        }
    }
    g_clear_error(&rm_err);
    return FALSE;
}

                                                                          
                                                    
                                                            
void
pcv_lxc_ns_set_abort_fn_for_test(gboolean (*fn)(void))
{
    g_atomic_pointer_set(&G_ns_abort_fn_test, fn);
}

                                                                            
                                                             
                                                            
void
pcv_lxc_ns_set_spawn_observer_for_test(void (*fn)(const gchar * const *argv,
                                                  PcvChildCapabilityProfile profile))
{
    g_atomic_pointer_set(&G_ns_spawn_observer_test, fn);
}
