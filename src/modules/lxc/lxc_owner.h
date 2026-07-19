
#ifndef PURECVISOR_LXC_OWNER_H
#define PURECVISOR_LXC_OWNER_H

#include <glib.h>

G_BEGIN_DECLS

gboolean pcv_lxc_stamp_owner(const gchar *name, const gchar *owner_sub);

gchar   *pcv_lxc_read_owner(const gchar *name);

G_END_DECLS

#endif
