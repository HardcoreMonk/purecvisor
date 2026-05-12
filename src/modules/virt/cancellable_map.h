













#ifndef PURECVISOR_CANCELLABLE_MAP_H
#define PURECVISOR_CANCELLABLE_MAP_H

#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS





void cmap_init(void);





void cmap_shutdown(void);








void cmap_register(const gchar *vm_name, GCancellable *cancellable);








void cmap_cancel(const gchar *vm_name);








void cmap_remove(const gchar *vm_name);





void cmap_cancel_all(void);





guint cmap_size(void);

G_END_DECLS

#endif
