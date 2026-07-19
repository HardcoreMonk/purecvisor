#ifndef PURECVISOR_PROMETHEUS_EXPORTER_H
#define PURECVISOR_PROMETHEUS_EXPORTER_H

#include <glib.h>

G_BEGIN_DECLS

#define PCV_PROM_MAX_METRICS 4096

void pcv_prom_init(void);

void pcv_prom_shutdown(void);

void pcv_prom_inc(const gchar *name, const gchar *label_key, const gchar *label_val);

void pcv_prom_gauge_set(const gchar *name, const gchar *label_key,
                         const gchar *label_val, gdouble value);

void pcv_prom_gauge_set_labels(const gchar *name, const gchar *labels,
                                gdouble value);

void pcv_prom_rpc_start(const gchar *method);

void pcv_prom_rpc_end(const gchar *method, gboolean success, gdouble duration_ms);

void pcv_prom_zfs_inflight_lock_observe(const gchar *pool_name,
                                        const gchar *op,
                                        const gchar *result,
                                        gdouble wait_ms);

gchar *pcv_prom_render(void);

G_END_DECLS

#endif
