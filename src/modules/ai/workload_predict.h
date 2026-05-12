



























#ifndef PURECVISOR_WORKLOAD_PREDICT_H
#define PURECVISOR_WORKLOAD_PREDICT_H

#include <glib.h>

G_BEGIN_DECLS


void pcv_predict_init(void);


void pcv_predict_shutdown(void);







void pcv_predict_evaluate(void);







gchar *pcv_predict_get_forecast_json(void);

G_END_DECLS

#endif
