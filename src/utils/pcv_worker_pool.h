
#ifndef PCV_WORKER_POOL_H
#define PCV_WORKER_POOL_H

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

void pcv_worker_pool_init(void);

void pcv_worker_pool_shutdown(void);

void pcv_worker_pool_push(GTask *task, GTaskThreadFunc func);

guint pcv_worker_pool_get_pending(void);

G_END_DECLS

#endif
