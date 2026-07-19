
#ifndef PURECVISOR_SELF_HEALING_H
#define PURECVISOR_SELF_HEALING_H

#include <glib.h>

G_BEGIN_DECLS

void pcv_healing_init(void);

void pcv_healing_shutdown(void);

void pcv_healing_on_anomaly(const gchar *metric, gdouble value,
                             gdouble zscore, gdouble threshold,
                             const gchar *target_vm);

void pcv_healing_on_prediction(gdouble cpu_pred, gdouble mem_pred,
                                gdouble cpu_trend, gdouble mem_trend);

gchar *pcv_healing_get_pending_json(void);

void pcv_healing_approve(gint action_id);

void pcv_healing_dismiss(gint action_id);

gchar *pcv_healing_get_history_json(void);

void pcv_healing_set_mode(gboolean dry_run);

gboolean pcv_healing_get_mode(void);

G_END_DECLS

#endif
