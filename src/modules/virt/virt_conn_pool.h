















#ifndef PURECVISOR_VIRT_CONN_POOL_H
#define PURECVISOR_VIRT_CONN_POOL_H

#include <glib.h>
#include <libvirt/libvirt.h>

G_BEGIN_DECLS







void virt_conn_pool_init(guint max_size);









virConnectPtr virt_conn_pool_acquire(void);







void virt_conn_pool_release(virConnectPtr conn);





void virt_conn_pool_shutdown(void);







void virt_conn_pool_stats(guint *out_idle, guint *out_total, guint *out_max);





gdouble virt_conn_pool_wait_avg_seconds(void);

G_END_DECLS

#endif
