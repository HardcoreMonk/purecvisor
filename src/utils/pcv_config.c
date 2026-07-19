
#include "pcv_config.h"
#include "pcv_log.h"
#include "pcv_jwt.h"

#include <glib.h>
#include <string.h>

#define CFG_LOG_DOM  "pcv_config"

#define CFG_GROUP    "daemon"

typedef struct {
    gchar   *socket_path;
    gchar   *libvirt_uri;
    gint     pool_max_conn;
    gint     drain_timeout;
    gchar   *db_path;
    gchar   *log_level;

    gint     rest_port;
    gchar   *admin_user;
    gchar   *admin_password;
    gchar   *jwt_secret;
    GKeyFile *kf;
    GHashTable *string_cache;
    GPtrArray *retired_string_caches;
    gboolean initialized;
    GRWLock  kf_lock;
} PcvConfig;

static PcvConfig g_cfg = { 0 };

static gchar *
_cfg_str(GKeyFile   *kf,
         const gchar *key,
         const gchar *env_var,
         const gchar *default_val)
{

    const gchar *env = g_getenv(env_var);
    if (env && env[0] != '\0')
        return g_strdup(env);

    if (kf) {
        GError *err = NULL;

        gchar *val = g_key_file_get_string(kf, CFG_GROUP, key, &err);
        if (val) {
            g_strstrip(val);
            return val;
        }
        if (err) { g_error_free(err); err = NULL; }

        gchar **groups = g_key_file_get_groups(kf, NULL);
        if (groups) {
            for (gchar **g = groups; *g; g++) {

                if (g_strcmp0(*g, CFG_GROUP) == 0) continue;
                val = g_key_file_get_string(kf, *g, key, &err);
                if (val) {
                    g_strstrip(val);
                    g_strfreev(groups);
                    return val;
                }
                if (err) { g_error_free(err); err = NULL; }
            }
            g_strfreev(groups);
        }
    }

    return g_strdup(default_val);
}

static gint
_cfg_int(GKeyFile   *kf,
         const gchar *key,
         const gchar *env_var,
         gint         default_val)
{

    const gchar *env = g_getenv(env_var);
    if (env && env[0] != '\0') {
        gchar *end = NULL;
        glong v = strtol(env, &end, 10);

        if (end != env && *end == '\0' && v > 0)
            return (gint)v;
    }

    if (kf) {
        GError *err = NULL;

        gint val = g_key_file_get_integer(kf, CFG_GROUP, key, &err);
        if (!err) return val;
        g_error_free(err); err = NULL;

        gchar **groups = g_key_file_get_groups(kf, NULL);
        if (groups) {
            for (gchar **g = groups; *g; g++) {
                if (g_strcmp0(*g, CFG_GROUP) == 0) continue;
                val = g_key_file_get_integer(kf, *g, key, &err);
                if (!err) { g_strfreev(groups); return val; }
                g_error_free(err); err = NULL;
            }
            g_strfreev(groups);
        }
    }

    return default_val;
}

void
pcv_config_init(void)
{
    if (g_cfg.initialized)
        pcv_config_shutdown();

    g_rw_lock_init(&g_cfg.kf_lock);
    g_cfg.string_cache = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, g_free);
    g_cfg.retired_string_caches =
        g_ptr_array_new_with_free_func((GDestroyNotify)g_hash_table_destroy);

    const gchar *cfg_env = g_getenv("PCV_CONFIG_PATH");
    const gchar *cfg_path = (cfg_env && *cfg_env) ? cfg_env : PCV_CONFIG_FILE_PATH;
    GKeyFile *kf   = g_key_file_new();
    GError   *err  = NULL;
    gboolean  loaded = g_key_file_load_from_file(
        kf, cfg_path,
        G_KEY_FILE_NONE, &err);

    if (!loaded) {
        if (err->code != G_FILE_ERROR_NOENT) {

            PCV_LOG_WARN(CFG_LOG_DOM,
                         "Failed to parse config file '%s': %s — using defaults",
                         cfg_path, err->message);
        } else {

            PCV_LOG_INFO(CFG_LOG_DOM,
                         "Config file not found ('%s') — using defaults",
                         cfg_path);
        }
        g_error_free(err);
        g_key_file_free(kf);
        kf = NULL;
    }

    g_cfg.socket_path   = _cfg_str(kf, "socket_path",   "PURECVISOR_SOCKET_PATH",
                                   PCV_DEFAULT_SOCKET_PATH);
    g_cfg.libvirt_uri   = _cfg_str(kf, "libvirt_uri",   "PURECVISOR_LIBVIRT_URI",
                                   PCV_DEFAULT_LIBVIRT_URI);
    g_cfg.db_path       = _cfg_str(kf, "db_path",       "PURECVISOR_DB_PATH",
                                   PCV_DEFAULT_DB_PATH);
    g_cfg.log_level     = _cfg_str(kf, "log_level",     "PURECVISOR_LOG_LEVEL",
                                   PCV_DEFAULT_LOG_LEVEL);
    g_cfg.pool_max_conn = _cfg_int(kf, "pool_max_conn", "PURECVISOR_POOL_MAX_CONN",
                                   PCV_DEFAULT_POOL_MAX_CONN);
    g_cfg.drain_timeout = _cfg_int(kf, "drain_timeout", "PURECVISOR_DRAIN_TIMEOUT",
                                   PCV_DEFAULT_DRAIN_TIMEOUT);

    g_cfg.rest_port      = _cfg_int(kf, "rest_port",       "PURECVISOR_REST_PORT",
                                    PCV_DEFAULT_REST_PORT);
    g_cfg.admin_user     = _cfg_str(kf, "admin_user",      "PURECVISOR_ADMIN_USER",
                                    PCV_DEFAULT_ADMIN_USER);
    g_cfg.admin_password = _cfg_str(kf, "admin_password",  "PURECVISOR_ADMIN_PASSWORD",
                                    PCV_DEFAULT_ADMIN_PASSWORD);
    g_cfg.jwt_secret     = _cfg_str(kf, "jwt_secret",      "PURECVISOR_JWT_SECRET",
                                    PCV_DEFAULT_JWT_SECRET);

    g_cfg.kf = kf;

    if (g_cfg.rest_port < 1 || g_cfg.rest_port > 65535) {
        PCV_LOG_WARN(CFG_LOG_DOM, "rest_port=%d out of range [1-65535] — using default %d",
                     g_cfg.rest_port, PCV_DEFAULT_REST_PORT);
        g_cfg.rest_port = PCV_DEFAULT_REST_PORT;
    }

    if (g_cfg.drain_timeout < 5) {
        PCV_LOG_WARN(CFG_LOG_DOM, "drain_timeout=%d too low — using minimum 5",
                     g_cfg.drain_timeout);
        g_cfg.drain_timeout = 5;
    }

    if (g_cfg.pool_max_conn < 1 || g_cfg.pool_max_conn > 64) {
        PCV_LOG_WARN(CFG_LOG_DOM, "pool_max_conn=%d out of range [1-64] — using default %d",
                     g_cfg.pool_max_conn, PCV_DEFAULT_POOL_MAX_CONN);
        g_cfg.pool_max_conn = PCV_DEFAULT_POOL_MAX_CONN;
    }

    {
        const gchar *valid_levels[] = {"error", "warn", "info", "debug", "trace", NULL};
        gboolean valid = FALSE;
        for (int i = 0; valid_levels[i]; i++) {
            if (g_strcmp0(g_cfg.log_level, valid_levels[i]) == 0) { valid = TRUE; break; }
        }
        if (!valid) {
            PCV_LOG_WARN(CFG_LOG_DOM, "log_level='%s' invalid — using default '%s'",
                         g_cfg.log_level, PCV_DEFAULT_LOG_LEVEL);
            g_free(g_cfg.log_level);
            g_cfg.log_level = g_strdup(PCV_DEFAULT_LOG_LEVEL);
        }
    }

    if (!g_cfg.admin_password || !*g_cfg.admin_password) {
        PCV_LOG_WARN(CFG_LOG_DOM,
            "admin_password is not configured — bootstrap admin auto-create is disabled "
            "until /etc/purecvisor/daemon.conf or PURECVISOR_ADMIN_PASSWORD is set");
    } else if (strlen(g_cfg.admin_password) < 12) {
        PCV_LOG_WARN(CFG_LOG_DOM,
                     "admin_password is shorter than 12 characters — consider a stronger password");
    } else if (g_strcmp0(g_cfg.admin_password, "admin") == 0 ||
               g_strcmp0(g_cfg.admin_password, "password") == 0) {
        PCV_LOG_WARN(CFG_LOG_DOM,
            "SECURITY: admin_password uses a commonly known example value — "
            "change it in /etc/purecvisor/daemon.conf before production use");
    }

    if (g_cfg.jwt_secret && *g_cfg.jwt_secret && strlen(g_cfg.jwt_secret) < 32) {
        PCV_LOG_WARN(CFG_LOG_DOM,
                     "jwt_secret is shorter than 32 bytes — HMAC-SHA256 security may be weakened");
    }

    if (kf) {
        gchar **keys = g_key_file_get_keys(kf, "daemon", nullptr, nullptr);
        if (keys) {
            const gchar *known[] = {
                "socket_path", "rest_port", "drain_timeout", "pool_max_conn",
                "log_level", "libvirt_uri", "jwt_secret", "db_path",
                "admin_user", "admin_password", nullptr
            };
            for (gchar **k = keys; *k; k++) {
                gboolean found = FALSE;
                for (const gchar **kn = known; *kn; kn++) {
                    if (g_strcmp0(*k, *kn) == 0) { found = TRUE; break; }
                }
                if (!found)
                    PCV_LOG_WARN(CFG_LOG_DOM,
                                 "Unknown key in [daemon]: '%s' (ignored)", *k);
            }
            g_strfreev(keys);
        }
    }

    {
        gchar *sock_dir = g_path_get_dirname(g_cfg.socket_path);
        if (g_mkdir_with_parents(sock_dir, 0700) != 0) {
            g_critical("[%s] FATAL: Cannot create socket directory '%s' — daemon cannot start",
                       CFG_LOG_DOM, sock_dir);
            g_free(sock_dir);
            exit(1);
        }
        g_free(sock_dir);
    }

    {
        gchar *db_dir = g_path_get_dirname(g_cfg.db_path);
        if (g_mkdir_with_parents(db_dir, 0755) != 0) {
            g_critical("[%s] FATAL: Cannot create db directory '%s' — daemon cannot start",
                       CFG_LOG_DOM, db_dir);
            g_free(db_dir);
            exit(1);
        }
        g_free(db_dir);
    }

    {
        const gchar *etcd_ep = pcv_config_get_string("cluster", "etcd_endpoints", "");
        if (etcd_ep && etcd_ep[0] != '\0') {
            if (!g_str_has_prefix(etcd_ep, "http://") &&
                !g_str_has_prefix(etcd_ep, "https://")) {
                PCV_LOG_WARN(CFG_LOG_DOM,
                    "etcd_endpoints='%s' does not start with http(s):// — cluster features may fail",
                    etcd_ep);
            }
        }
    }

    {
        const gchar *img_dir = pcv_config_get_string("storage", "image_dir",
                                                      "/var/lib/libvirt/images");
        if (!g_file_test(img_dir, G_FILE_TEST_IS_DIR)) {
            if (g_mkdir_with_parents(img_dir, 0755) == 0) {
                PCV_LOG_INFO(CFG_LOG_DOM, "Created image_dir: %s", img_dir);
            } else {
                PCV_LOG_WARN(CFG_LOG_DOM,
                    "image_dir '%s' does not exist and cannot be created — qcow2 fallback may fail",
                    img_dir);
            }
        }
    }

    g_cfg.initialized = TRUE;

    pcv_config_dump();

    if (!loaded) {
        GKeyFile *sample = g_key_file_new();
        g_key_file_set_comment(sample, NULL, NULL,
            " PureCVisor daemon configuration\n"
            " Generated automatically — edit as needed", NULL);
        g_key_file_set_string (sample, CFG_GROUP, "socket_path",   g_cfg.socket_path);
        g_key_file_set_string (sample, CFG_GROUP, "libvirt_uri",   g_cfg.libvirt_uri);
        g_key_file_set_integer(sample, CFG_GROUP, "pool_max_conn", g_cfg.pool_max_conn);
        g_key_file_set_integer(sample, CFG_GROUP, "drain_timeout", g_cfg.drain_timeout);
        g_key_file_set_string (sample, CFG_GROUP, "db_path",       g_cfg.db_path);
        g_key_file_set_string (sample, CFG_GROUP, "log_level",     g_cfg.log_level);
        g_key_file_set_integer(sample, CFG_GROUP, "rest_port",     g_cfg.rest_port);
        g_key_file_set_string (sample, CFG_GROUP, "admin_user",    g_cfg.admin_user);

        g_key_file_set_string (sample, CFG_GROUP, "admin_password", "");
        g_key_file_set_comment(sample, CFG_GROUP, "admin_password",
            " REQUIRED: set before first bootstrap login or use PURECVISOR_ADMIN_PASSWORD", NULL);
        g_key_file_set_string (sample, CFG_GROUP, "jwt_secret",    "");
        g_key_file_set_comment(sample, CFG_GROUP, "jwt_secret",
            " Optional: leave empty to generate a random in-memory key on startup", NULL);

        g_key_file_set_integer(sample, "security_group", "resync_interval_sec", 300);
        g_key_file_set_comment (sample, "security_group", "resync_interval_sec",
            " SG vnet 캐시 주기 재동기화 간격(초). 기본 300. 0 또는 음수면 타이머 비활성.", NULL);

        if (g_mkdir_with_parents("/etc/purecvisor", 0755) == 0) {
            GError *save_err = NULL;
            if (!g_key_file_save_to_file(sample, PCV_CONFIG_FILE_PATH, &save_err)) {
                PCV_LOG_WARN(CFG_LOG_DOM, "Could not write sample config: %s",
                             save_err->message);
                g_error_free(save_err);
            } else {
                PCV_LOG_INFO(CFG_LOG_DOM,
                             "Sample config written to '%s'", cfg_path);
            }
        }
        g_key_file_free(sample);
    }
}

void
pcv_config_shutdown(void)
{
    if (!g_cfg.initialized) return;
    g_free(g_cfg.socket_path);
    g_free(g_cfg.libvirt_uri);
    g_free(g_cfg.db_path);
    g_free(g_cfg.log_level);
    g_free(g_cfg.admin_user);
    g_free(g_cfg.admin_password);
    g_free(g_cfg.jwt_secret);
    if (g_cfg.string_cache) g_hash_table_destroy(g_cfg.string_cache);
    if (g_cfg.retired_string_caches) g_ptr_array_free(g_cfg.retired_string_caches, TRUE);
    if (g_cfg.kf) g_key_file_free(g_cfg.kf);
    g_rw_lock_clear(&g_cfg.kf_lock);

    memset(&g_cfg, 0, sizeof(g_cfg));
}

const gchar *pcv_config_get_socket_path(void)   { return g_cfg.socket_path   ? g_cfg.socket_path   : PCV_DEFAULT_SOCKET_PATH; }
const gchar *pcv_config_get_libvirt_uri(void)   { return g_cfg.libvirt_uri   ? g_cfg.libvirt_uri   : PCV_DEFAULT_LIBVIRT_URI; }
const gchar *pcv_config_get_db_path(void)       { return g_cfg.db_path       ? g_cfg.db_path       : PCV_DEFAULT_DB_PATH; }
const gchar *pcv_config_get_log_level(void)     { return g_cfg.log_level     ? g_cfg.log_level     : PCV_DEFAULT_LOG_LEVEL; }
gint         pcv_config_get_pool_max_conn(void) {
    gint v = g_cfg.pool_max_conn;
    return (v >= 1 && v <= 64) ? v : PCV_DEFAULT_POOL_MAX_CONN;
}
gint         pcv_config_get_drain_timeout(void) {
    gint v = g_cfg.drain_timeout;
    return (v >= 5) ? v : PCV_DEFAULT_DRAIN_TIMEOUT;
}

gint         pcv_config_get_rest_port(void) {
    gint v = g_cfg.rest_port;
    return (v >= 1 && v <= 65535) ? v : PCV_DEFAULT_REST_PORT;
}
const gchar *pcv_config_get_admin_user(void)     { return g_cfg.admin_user     ? g_cfg.admin_user     : PCV_DEFAULT_ADMIN_USER; }
const gchar *pcv_config_get_admin_password(void) { return g_cfg.admin_password ? g_cfg.admin_password : PCV_DEFAULT_ADMIN_PASSWORD; }
const gchar *pcv_config_get_jwt_secret(void)     { return g_cfg.jwt_secret     ? g_cfg.jwt_secret     : PCV_DEFAULT_JWT_SECRET; }

void
pcv_config_dump(void)
{
    PCV_LOG_INFO(CFG_LOG_DOM,
                 "Config: socket=%s uri=%s pool=%d drain=%ds db=%s log=%s",
                 g_cfg.socket_path, g_cfg.libvirt_uri,
                 g_cfg.pool_max_conn, g_cfg.drain_timeout,
                 g_cfg.db_path, g_cfg.log_level);
    PCV_LOG_INFO(CFG_LOG_DOM,
                 "Config(REST): port=%d admin_user=%s jwt_secret=%s",
                 g_cfg.rest_port, g_cfg.admin_user,
                 (g_cfg.jwt_secret && *g_cfg.jwt_secret) ? "(set)" : "(random)");
}

const gchar *
pcv_config_get_string(const gchar *section, const gchar *key, const gchar *def)
{
    if (!section || !key)
        return def;

    if (!g_cfg.initialized)
        return def;

    g_rw_lock_writer_lock(&g_cfg.kf_lock);
    if (!g_cfg.kf) {
        g_rw_lock_writer_unlock(&g_cfg.kf_lock);
        return def;
    }

    if (!g_cfg.string_cache) {
        g_cfg.string_cache = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                   g_free, g_free);
    }

    gchar *cache_key = g_strdup_printf("%s\x1f%s", section, key);
    const gchar *cached = g_hash_table_lookup(g_cfg.string_cache, cache_key);
    if (cached) {
        g_free(cache_key);
        g_rw_lock_writer_unlock(&g_cfg.kf_lock);
        return cached;
    }

    GError *err = NULL;
    gchar *val = g_key_file_get_string(g_cfg.kf, section, key, &err);
    if (err) {
        g_error_free(err);
        g_free(cache_key);
        g_rw_lock_writer_unlock(&g_cfg.kf_lock);
        return def;
    }
    if (!val || !*val) {
        g_free(val);
        g_free(cache_key);
        g_rw_lock_writer_unlock(&g_cfg.kf_lock);
        return def;
    }

    g_hash_table_insert(g_cfg.string_cache, cache_key, val);
    g_rw_lock_writer_unlock(&g_cfg.kf_lock);
    return val;
}

gint
pcv_config_get_int(const gchar *section, const gchar *key, gint def)
{
    if (!g_cfg.kf) return def;

    g_rw_lock_reader_lock(&g_cfg.kf_lock);
    GError *err = NULL;
    gint val = g_key_file_get_integer(g_cfg.kf, section, key, &err);
    g_rw_lock_reader_unlock(&g_cfg.kf_lock);
    if (err) {
        g_error_free(err);
        return def;
    }
    return val;
}

const gchar *
pcv_config_get_zvol_pool(void)
{
    return pcv_config_get_string("storage", "zvol_pool", "pcvpool/vms");
}

const gchar *
pcv_config_get_container_pool(void)
{
    return pcv_config_get_string("storage", "container_pool", "pcvpool/containers");
}

const gchar *
pcv_config_get_container_path(void)
{
    return pcv_config_get_string("container", "lxc_path", "/var/lib/purecvisor/lxc");
}

const gchar *
pcv_config_get_image_dir(void)
{
    return pcv_config_get_string("storage", "image_dir", "/var/lib/libvirt/images");
}

const gchar *
pcv_config_get_iso_dirs(void)
{
    return pcv_config_get_string("storage", "iso_dirs",
                                 "/pcvpool/iso,/var/lib/libvirt/images,/iso");
}

const gchar *
pcv_config_get_ssh_user(void)
{
    return pcv_config_get_string("cluster", "ssh_user", "pcvdev");
}

gboolean
pcv_config_reload(void)
{
    GKeyFile *kf = g_key_file_new();
    GError   *err = NULL;

    if (!g_key_file_load_from_file(kf, PCV_CONFIG_FILE_PATH,
                                   G_KEY_FILE_NONE, &err)) {
        PCV_LOG_WARN(CFG_LOG_DOM,
                     "Config reload failed: %s", err->message);
        g_error_free(err);
        g_key_file_free(kf);
        return FALSE;
    }

    GHashTable *new_cache = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                  g_free, g_free);

    g_rw_lock_writer_lock(&g_cfg.kf_lock);
    GKeyFile *old_kf = g_cfg.kf;
    GHashTable *old_cache = g_cfg.string_cache;
    g_cfg.kf = kf;
    g_cfg.string_cache = new_cache;
    if (!g_cfg.retired_string_caches) {
        g_cfg.retired_string_caches =
            g_ptr_array_new_with_free_func((GDestroyNotify)g_hash_table_destroy);
    }
    if (old_cache)
        g_ptr_array_add(g_cfg.retired_string_caches, old_cache);
    g_rw_lock_writer_unlock(&g_cfg.kf_lock);
    if (old_kf) g_key_file_free(old_kf);

    {
        GError *le = NULL;
        gchar *new_level = g_key_file_get_string(kf, CFG_GROUP, "log_level", &le);
        if (new_level) {
            g_strstrip(new_level);
            if (g_strcmp0(new_level, g_cfg.log_level) != 0) {
                PCV_LOG_INFO(CFG_LOG_DOM, "Reload: log_level '%s' → '%s'",
                             g_cfg.log_level, new_level);
                g_free(g_cfg.log_level);
                g_cfg.log_level = new_level;
            } else {
                g_free(new_level);
            }
        }
        if (le) g_error_free(le);
    }

    {
        GError *pe = NULL;
        gint new_pool = g_key_file_get_integer(kf, CFG_GROUP, "pool_max_conn", &pe);
        if (!pe && new_pool >= 1 && new_pool <= 64) {
            g_cfg.pool_max_conn = new_pool;
            PCV_LOG_INFO(CFG_LOG_DOM, "Reload: pool_max_conn → %d", new_pool);
        }
        if (pe) g_error_free(pe);
    }

    {
        GError *de = NULL;
        gint new_drain = g_key_file_get_integer(kf, CFG_GROUP, "drain_timeout", &de);
        if (!de && new_drain >= 5) {
            g_cfg.drain_timeout = new_drain;
            PCV_LOG_INFO(CFG_LOG_DOM, "Reload: drain_timeout → %d", new_drain);
        }
        if (de) g_error_free(de);
    }

    {
        gchar *new_secret = pcv_config_get_secret("auth", "jwt_secret", NULL);
        if (new_secret && *new_secret) {
            pcv_jwt_update_secret(new_secret);
            PCV_LOG_INFO(CFG_LOG_DOM,
                         "Reload: jwt_secret updated (existing tokens invalidated)");
        }
        g_free(new_secret);
    }

#if PCV_CLUSTER_ENABLED
    {
        extern void pcv_cluster_notify_config_reload(void);
        pcv_cluster_notify_config_reload();
    }
#endif

    PCV_LOG_INFO(CFG_LOG_DOM,
                 "Config reloaded — [alert], rate_limit, etcd_timeout, "
                 "log_level, pool_max_conn, drain_timeout now reflect daemon.conf on disk");
    return TRUE;
}

#include <openssl/evp.h>
#include <openssl/sha.h>

#define PCV_SECRET_KEY_LEN   32

#define PCV_SECRET_IV_LEN    12

#define PCV_SECRET_TAG_LEN   16

#define PCV_SECRET_PBKDF2_ITER 100000

static gboolean
_derive_master_key(guchar *out_key, guchar *out_iv)
{
    gchar  *machine_id = NULL;
    gsize   mid_len    = 0;

    if (!g_file_get_contents("/etc/machine-id", &machine_id, &mid_len, NULL)) {
        PCV_LOG_WARN(CFG_LOG_DOM, "Cannot read /etc/machine-id — secret decryption unavailable");
        return FALSE;
    }

    g_strstrip(machine_id);
    mid_len = strlen(machine_id);

    static const guchar salt[] = "purecvisor-config-v1";
    if (PKCS5_PBKDF2_HMAC(machine_id, (int)mid_len,
                           salt, sizeof(salt) - 1,
                           PCV_SECRET_PBKDF2_ITER, EVP_sha256(),
                           PCV_SECRET_KEY_LEN, out_key) != 1) {
        g_free(machine_id);
        return FALSE;
    }

    {
        guchar sha_buf[SHA256_DIGEST_LENGTH];
        EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(md_ctx, EVP_sha256(), NULL);
        EVP_DigestUpdate(md_ctx, machine_id, mid_len);
        EVP_DigestUpdate(md_ctx, "iv-derivation", 13);
        EVP_DigestFinal_ex(md_ctx, sha_buf, NULL);
        EVP_MD_CTX_free(md_ctx);
        memcpy(out_iv, sha_buf, PCV_SECRET_IV_LEN);
    }

    g_free(machine_id);
    return TRUE;
}

static gchar *
_decrypt_aes_gcm(const gchar *b64_ciphertext)
{
    guchar key[PCV_SECRET_KEY_LEN], iv[PCV_SECRET_IV_LEN];
    if (!_derive_master_key(key, iv))
        return NULL;

    gsize raw_len = 0;
    guchar *raw = g_base64_decode(b64_ciphertext, &raw_len);
    if (!raw || raw_len < PCV_SECRET_TAG_LEN + 1) {
        g_free(raw);
        return NULL;
    }

    const guchar *tag        = raw;
    const guchar *ciphertext = raw + PCV_SECRET_TAG_LEN;
    int cipher_len = (int)(raw_len - PCV_SECRET_TAG_LEN);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { g_free(raw); return NULL; }

    guchar *plaintext = g_malloc0((gsize)cipher_len + 1);
    int out_len = 0, final_len = 0;
    gboolean ok = FALSE;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, key, iv) == 1 &&
        EVP_DecryptUpdate(ctx, plaintext, &out_len, ciphertext, cipher_len) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, PCV_SECRET_TAG_LEN,
                            (void *)(guintptr)tag) == 1 &&
        EVP_DecryptFinal_ex(ctx, plaintext + out_len, &final_len) == 1) {
        plaintext[out_len + final_len] = '\0';
        ok = TRUE;
    }

    EVP_CIPHER_CTX_free(ctx);
    g_free(raw);

    if (!ok) {
        PCV_LOG_WARN(CFG_LOG_DOM, "AES-256-GCM decryption failed — check machine-id or ciphertext");
        g_free(plaintext);
        return NULL;
    }
    return (gchar *)plaintext;
}

gchar *
pcv_config_get_secret(const gchar *group, const gchar *key,
                       const gchar *fallback)
{

    gchar *env_name = g_strdup_printf("PCV_SECRET_%s_%s", group, key);
    for (gchar *p = env_name; *p; p++) {
        *p = g_ascii_toupper(*p);
        if (*p == '-') *p = '_';
    }
    const gchar *env_val = g_getenv(env_name);
    g_free(env_name);
    if (env_val && *env_val) {
        PCV_LOG_DEBUG(CFG_LOG_DOM, "Secret [%s] %s loaded from environment variable", group, key);
        return g_strdup(env_val);
    }

    const gchar *raw = pcv_config_get_string(group, key, NULL);
    if (!raw || !*raw)
        return g_strdup(fallback);

    if (g_str_has_prefix(raw, "ENC:")) {
        gchar *decrypted = _decrypt_aes_gcm(raw + 4);
        return decrypted ? decrypted : g_strdup(fallback);
    }

    return g_strdup(raw);
}

gchar *
pcv_config_encrypt_value(const gchar *plaintext)
{
    if (!plaintext) return NULL;

    guchar key[PCV_SECRET_KEY_LEN], iv[PCV_SECRET_IV_LEN];
    if (!_derive_master_key(key, iv))
        return NULL;

    int pt_len = (int)strlen(plaintext);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return NULL;

    guchar *out_buf = g_malloc0(PCV_SECRET_TAG_LEN + (gsize)pt_len + EVP_MAX_BLOCK_LENGTH);
    guchar *ciphertext = out_buf + PCV_SECRET_TAG_LEN;
    int out_len = 0, final_len = 0;
    guchar tag[PCV_SECRET_TAG_LEN];
    gboolean ok = FALSE;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, key, iv) == 1 &&
        EVP_EncryptUpdate(ctx, ciphertext, &out_len, (const guchar *)plaintext, pt_len) == 1 &&
        EVP_EncryptFinal_ex(ctx, ciphertext + out_len, &final_len) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, PCV_SECRET_TAG_LEN, tag) == 1) {
        ok = TRUE;
    }

    EVP_CIPHER_CTX_free(ctx);

    if (!ok) {
        g_free(out_buf);
        return NULL;
    }

    memcpy(out_buf, tag, PCV_SECRET_TAG_LEN);
    gsize total_len = PCV_SECRET_TAG_LEN + (gsize)(out_len + final_len);

    gchar *b64 = g_base64_encode(out_buf, total_len);
    g_free(out_buf);

    gchar *result = g_strdup_printf("ENC:%s", b64);
    g_free(b64);
    return result;
}
