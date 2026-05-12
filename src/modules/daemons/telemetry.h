





















































































#ifndef PURECVISOR_DAEMONS_TELEMETRY_H
#define PURECVISOR_DAEMONS_TELEMETRY_H

#include <glib.h>
#include "../virt/vm_manager.h"


G_BEGIN_DECLS




























typedef struct {
    guint64 cpu_time_ns;


    guint64 rx_bytes;
    guint64 tx_bytes;
    guint64 rx_packets;
    guint64 tx_packets;
    guint64 rx_errs;
    guint64 tx_errs;
    guint64 rx_drop;
    guint64 tx_drop;
} VmMetrics;



























void init_telemetry_daemon(PureCVisorVmManager *vm_manager);




















VmMetrics* get_vm_metrics(const gchar *vm_id);

G_END_DECLS

#endif