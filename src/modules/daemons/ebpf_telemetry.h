
#ifndef PURECVISOR_EBPF_TELEMETRY_H
#define PURECVISOR_EBPF_TELEMETRY_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

static inline gboolean
pcv_ebpf_proc_stat_is_cpu_core_line(const gchar *line)
{
    return line &&
           line[0] == 'c' &&
           line[1] == 'p' &&
           line[2] == 'u' &&
           g_ascii_isdigit((guchar)line[3]);
}

void        pcv_ebpf_telemetry_init(void);

void        pcv_ebpf_telemetry_shutdown(void);

JsonObject *pcv_ebpf_telemetry_get_host(void);

JsonObject *pcv_ebpf_telemetry_get_vm(const gchar *vm_name);

JsonArray  *pcv_ebpf_telemetry_get_all_vms(void);

G_END_DECLS

#endif
