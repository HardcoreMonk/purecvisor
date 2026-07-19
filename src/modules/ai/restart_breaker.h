
#ifndef PURECVISOR_RESTART_BREAKER_H
#define PURECVISOR_RESTART_BREAKER_H

#include <glib.h>
#include "modules/virt/circuit_breaker.h"

G_BEGIN_DECLS

#define RESTART_BREAKER_THRESHOLD_DEFAULT     3
#define RESTART_BREAKER_COOLDOWN_SEC_DEFAULT  1800

void rb_init(void);

void rb_shutdown(void);

void rb_configure(gint threshold, gint cooldown_sec);

gint rb_get_threshold(void);
gint rb_get_cooldown_sec(void);

[[nodiscard]] gboolean rb_allow(const gchar *uuid);

void rb_record(const gchar *uuid, gboolean success);

void rb_release_probe(const gchar *uuid);

CbState rb_state(const gchar *uuid);

gint rb_failure_count(const gchar *uuid);

G_END_DECLS

#endif
