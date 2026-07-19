
#ifndef PURECVISOR_ANOMALY_DETECTOR_H
#define PURECVISOR_ANOMALY_DETECTOR_H

#include <glib.h>

G_BEGIN_DECLS

void pcv_anomaly_init(void);

void pcv_anomaly_shutdown(void);

void pcv_anomaly_evaluate(void);

gchar *pcv_anomaly_get_recent_json(void);

void pcv_anomaly_reset_baseline(void);

G_END_DECLS

#endif
