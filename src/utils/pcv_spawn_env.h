#ifndef PURECVISOR_SPAWN_ENV_H
#define PURECVISOR_SPAWN_ENV_H

#include <gio/gio.h>

G_BEGIN_DECLS

void pcv_spawn_env_apply(GSubprocessLauncher *launcher);

G_END_DECLS

#endif
