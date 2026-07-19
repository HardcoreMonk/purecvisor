
#ifndef PURECVISOR_PRIVDROP_H
#define PURECVISOR_PRIVDROP_H

#include <glib.h>

G_BEGIN_DECLS

gboolean pcv_privdrop_capabilities(void);

gboolean pcv_privdrop_no_new_privs(void);

gboolean pcv_privdrop_seccomp(void);

void pcv_privdrop_apply_all(void);

G_END_DECLS

#endif
