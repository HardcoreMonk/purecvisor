
#ifndef PURECVISOR_CONFIG_H
#define PURECVISOR_CONFIG_H

#include <glib.h>

G_BEGIN_DECLS

#define PCV_DEFAULT_SOCKET_PATH   "/var/run/purecvisor/daemon.sock"

#define PCV_DEFAULT_LIBVIRT_URI   "qemu:///system"

#define PCV_DEFAULT_POOL_MAX_CONN 8

#define PCV_DEFAULT_DRAIN_TIMEOUT 30

#define PCV_DEFAULT_DB_PATH       "/var/lib/purecvisor/vm_state.db"

#define PCV_DEFAULT_LOG_LEVEL     "info"

#define PCV_CONFIG_FILE_PATH      "/etc/purecvisor/daemon.conf"

void pcv_config_init(void);

void pcv_config_shutdown(void);

const gchar *pcv_config_get_socket_path(void);

const gchar *pcv_config_get_libvirt_uri(void);

gint         pcv_config_get_pool_max_conn(void);

gint         pcv_config_get_drain_timeout(void);

const gchar *pcv_config_get_db_path(void);

const gchar *pcv_config_get_log_level(void);

void pcv_config_dump(void);

G_END_DECLS

#endif

#define PCV_DEFAULT_REST_PORT      80

#define PCV_DEFAULT_ADMIN_USER     "admin"

#define PCV_DEFAULT_ADMIN_PASSWORD ""

#define PCV_DEFAULT_JWT_SECRET     ""

gint         pcv_config_get_rest_port(void);

const gchar *pcv_config_get_admin_user(void);

const gchar *pcv_config_get_admin_password(void);

const gchar *pcv_config_get_jwt_secret(void);

const gchar *pcv_config_get_string(const gchar *section, const gchar *key, const gchar *def);
gint         pcv_config_get_int(const gchar *section, const gchar *key, gint def);

const gchar *pcv_config_get_zvol_pool(void);
const gchar *pcv_config_get_container_pool(void);
const gchar *pcv_config_get_container_path(void);
const gchar *pcv_config_get_image_dir(void);
const gchar *pcv_config_get_iso_dirs(void);

const gchar *pcv_config_get_ssh_user(void);

gboolean pcv_config_reload(void);

[[nodiscard]] gchar *pcv_config_get_secret(const gchar *group, const gchar *key,
                              const gchar *fallback);

gchar *pcv_config_encrypt_value(const gchar *plaintext);
