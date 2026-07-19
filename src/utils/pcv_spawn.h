
#ifndef PURECVISOR_SPAWN_H
#define PURECVISOR_SPAWN_H

#include <glib.h>

G_BEGIN_DECLS

void pcv_spawn_launcher_init(void);

void pcv_spawn_launcher_shutdown(void);

gboolean pcv_spawn_sync(const gchar * const *argv,
                        gchar              **stdout_out,
                        gchar              **stderr_out,
                        GError             **error);

gboolean pcv_spawn_sync_timeout(const gchar * const *argv,
                                gchar **stdout_out, gchar **stderr_out,
                                guint timeout_sec, GError **error);

gboolean pcv_spawn_sync_env(const gchar * const *argv,
                            const gchar * const *envp,
                            gchar              **stdout_out,
                            gchar              **stderr_out,
                            GError             **error);

void pcv_spawn_fire(const gchar * const *argv);

gboolean pcv_spawn_pipe_sync(const gchar * const *producer_argv,
                             const gchar * const *consumer_argv,
                             gchar              **consumer_stdout_out,
                             gchar              **combined_stderr_out,
                             GError             **error);

G_END_DECLS

#endif
