   
                              
                                                                       
  
                           
                                                                                
                                                                                      
                                                                                               
                                                                                        
                                                          
  
                                                                   
                              
                                                                         
                                                        
   
#include "replication_hostkey.h"
#include "../../utils/pcv_spawn.h"
#include "../../utils/pcv_log.h"
#include "../../utils/pcv_validate.h"
#include "../dispatcher/rpc_utils.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#define REPL_LOG_DOM "replication_hostkey"

static GMutex g_kh_mutex;                                               
static gchar *g_kh_path_override = NULL;                              

                                                             
static const gchar * const k_key_types[] = {
    "ssh-ed25519", "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384",
    "ecdsa-sha2-nistp521", "ssh-rsa", NULL
};

GQuark
pcv_repl_hostkey_error_quark(void)
{
    return g_quark_from_static_string("pcv-repl-hostkey-error-quark");
}

void
pcv_repl_host_key_free(PcvReplHostKey *key)
{
    if (!key)
        return;
    g_free(key->target);
    g_free(key->type);
    g_free(key->blob_b64);
    g_free(key->fingerprint);
    g_free(key);
}

                                                                                              
void
pcv_repl_known_hosts_set_path_for_test(const gchar *path)
{
    g_mutex_lock(&g_kh_mutex);
    g_free(g_kh_path_override);
    g_kh_path_override = g_strdup(path);
    g_mutex_unlock(&g_kh_mutex);
}

                                                                               
                                                              
static const gchar *
_kh_path(void)
{
    return g_kh_path_override ? g_kh_path_override : PCV_REPL_KNOWN_HOSTS_PATH;
}

                                                                          
                                                  
gchar *
pcv_repl_canonical_target(const gchar *target_node)
{
    if (!pcv_validate_remote_host(target_node))
        return NULL;
    return g_ascii_strdown(target_node, -1);
}

                                                                                    
                                                                                 
                                                                                                                           
gchar **
pcv_repl_ssh_argv(const gchar *ssh_user, const gchar *target_node,
                  const gchar * const *remote_argv)
{
    g_autofree gchar *canon = pcv_repl_canonical_target(target_node);
    if (!canon || !pcv_validate_ssh_user(ssh_user) || !remote_argv || !remote_argv[0])
        return NULL;
    g_mutex_lock(&g_kh_mutex);                                           
    g_autofree gchar *khf   = g_strdup_printf("UserKnownHostsFile=%s", _kh_path());
    g_mutex_unlock(&g_kh_mutex);
    g_autofree gchar *alias = g_strdup_printf("HostKeyAlias=%s", canon);
    g_autofree gchar *cto   = g_strdup_printf("ConnectTimeout=%d", PCV_REPL_CONNECT_TIMEOUT_SEC);
    g_autofree gchar *dest  = g_strdup_printf("%s@%s", ssh_user, canon);
    const gchar *fixed[] = {
        "ssh", "-F", "/dev/null",
        "-o", "BatchMode=yes",
        "-o", "StrictHostKeyChecking=yes",
        "-o", khf,
        "-o", "GlobalKnownHostsFile=/dev/null",
        "-o", alias,
        "-o", "CheckHostIP=no",
        "-o", "UpdateHostKeys=no",
        "-o", "IdentitiesOnly=yes",
        "-o", cto,
        "-i", PCV_REPL_SSH_KEY_PATH,
        dest,
    };
    GStrvBuilder *b = g_strv_builder_new();
    for (gsize i = 0; i < G_N_ELEMENTS(fixed); i++)
        g_strv_builder_add(b, fixed[i]);
    for (gsize i = 0; remote_argv[i]; i++)
        g_strv_builder_add(b, remote_argv[i]);
    gchar **argv = g_strv_builder_end(b);
    g_strv_builder_unref(b);
    return argv;
}

                                                                    
gchar **
pcv_repl_probe_argv(const gchar *ssh_user, const gchar *target_node)
{
    static const gchar * const cmd[] = { "true", NULL };
    return pcv_repl_ssh_argv(ssh_user, target_node, cmd);
}

                                                                       
static guchar *
_b64_decode_strict(const gchar *b64, gsize *out_len)
{
    gsize n = b64 ? strlen(b64) : 0;
    if (n == 0 || n % 4 != 0 || n > 16384)
        return NULL;
    for (gsize i = 0; i < n; i++) {
        gchar c = b64[i];
        if (c == '=') {
            if (i < n - 2 || (i + 1 < n && b64[i + 1] != '='))
                return NULL;
        } else if (!g_ascii_isalnum(c) && c != '+' && c != '/') {
            return NULL;
        }
    }
    guchar *raw = g_base64_decode(b64, out_len);
    gchar *again = g_base64_encode(raw, *out_len);
    gboolean same = *out_len > 0 && g_strcmp0(again, b64) == 0;
    g_free(again);
    if (!same) {
        g_free(raw);
        return NULL;
    }
    return raw;
}

                                                                
static gchar *
_blob_type(const guchar *raw, gsize len)
{
    if (len < 4)
        return NULL;
    guint32 n = ((guint32)raw[0] << 24) | ((guint32)raw[1] << 16) | ((guint32)raw[2] << 8) | raw[3];
    if (n == 0 || n > 64 || (gsize)n > len - 4)
        return NULL;
    return g_strndup((const gchar *)raw + 4, n);
}

                                                                            
gchar *
pcv_repl_fingerprint(const gchar *blob_b64)
{
    gsize len = 0;
    guchar *raw = _b64_decode_strict(blob_b64, &len);
    if (!raw)
        return NULL;
    guint8 digest[32];
    gsize dlen = sizeof digest;
    GChecksum *ck = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(ck, raw, (gssize)len);
    g_checksum_get_digest(ck, digest, &dlen);
    g_checksum_free(ck);
    g_free(raw);
    gchar *b64 = g_base64_encode(digest, dlen);
    gsize k = strlen(b64);
    while (k > 0 && b64[k - 1] == '=')
        b64[--k] = '\0';
    gchar *fp = g_strconcat("SHA256:", b64, NULL);
    g_free(b64);
    return fp;
}

                                                                                 
static PcvReplHostKey *
_key_new(const gchar *target, const gchar *type, const gchar *blob_b64)
{
    if (!type || !g_strv_contains(k_key_types, type))
        return NULL;
    gsize len = 0;
    guchar *raw = _b64_decode_strict(blob_b64, &len);
    if (!raw)
        return NULL;
    gchar *inner = _blob_type(raw, len);
    g_free(raw);
    gboolean ok = g_strcmp0(inner, type) == 0;
    g_free(inner);
    if (!ok)
        return NULL;
    PcvReplHostKey *k = g_new0(PcvReplHostKey, 1);
    k->target = g_strdup(target);
    k->type = g_strdup(type);
    k->blob_b64 = g_strdup(blob_b64);
    k->fingerprint = pcv_repl_fingerprint(blob_b64);
    return k;
}

static PcvReplHostKey *
_key_dup(const PcvReplHostKey *k)
{
    PcvReplHostKey *c = g_new0(PcvReplHostKey, 1);
    c->target = g_strdup(k->target);
    c->type = g_strdup(k->type);
    c->blob_b64 = g_strdup(k->blob_b64);
    c->fingerprint = g_strdup(k->fingerprint);
    return c;
}

                                                           
static gchar **
_three_fields(const gchar *line)
{
    gchar **raw = g_strsplit_set(line, " \t", -1);
    GStrvBuilder *b = g_strv_builder_new();
    guint n = 0;
    for (gchar **p = raw; *p; p++) {
        if (**p) {
            g_strv_builder_add(b, *p);
            n++;
        }
    }
    gchar **out = g_strv_builder_end(b);
    g_strv_builder_unref(b);
    g_strfreev(raw);
    if (n != 3) {
        g_strfreev(out);
        return NULL;
    }
    return out;
}

static PcvReplHostKey *
_kh_find(GPtrArray *entries, const gchar *canon)
{
    for (guint i = 0; i < entries->len; i++) {
        PcvReplHostKey *k = g_ptr_array_index(entries, i);
        if (g_str_equal(k->target, canon))
            return k;
    }
    return NULL;
}

                                                                     
                                                                           
static GPtrArray *
_kh_load_locked(GError **error)
{
    GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)pcv_repl_host_key_free);
    gchar *text = NULL;
    GError *e = NULL;
    if (!g_file_get_contents(_kh_path(), &text, NULL, &e)) {
        gboolean missing = g_error_matches(e, G_FILE_ERROR, G_FILE_ERROR_NOENT);
        if (!missing)
            g_set_error(error, pcv_repl_hostkey_error_quark(), PCV_REPL_HOSTKEY_ERROR_FAILED,
                        "cannot read %s: %s", _kh_path(), e->message);
        g_error_free(e);
        if (missing)
            return entries;
        g_ptr_array_unref(entries);
        return NULL;
    }
    gchar **lines = g_strsplit(text, "\n", -1);
    for (guint i = 0; lines[i]; i++) {
        gchar *line = g_strstrip(lines[i]);
        if (!*line || *line == '#')
            continue;
        gchar **f = _three_fields(line);
        gchar *canon = f ? pcv_repl_canonical_target(f[0]) : NULL;
        PcvReplHostKey *k = (canon && g_str_equal(canon, f[0]) && !_kh_find(entries, canon))
                            ? _key_new(canon, f[1], f[2]) : NULL;
        g_free(canon);
        g_strfreev(f);
        if (!k) {
            PCV_LOG_WARN(REPL_LOG_DOM, "%s:%u: ignoring malformed or duplicate entry", _kh_path(), i + 1);
            continue;
        }
        g_ptr_array_add(entries, k);
    }
    g_strfreev(lines);
    g_free(text);
    return entries;
}

                                                   
PcvReplHostKey *
pcv_repl_hostkey_lookup(const gchar *target_node, GError **error)
{
    g_autofree gchar *canon = pcv_repl_canonical_target(target_node);
    if (!canon) {
        g_set_error(error, pcv_repl_hostkey_error_quark(), PCV_REPL_HOSTKEY_ERROR_INVALID,
                    "invalid replication target '%s'", target_node ? target_node : "");
        return NULL;
    }
    g_mutex_lock(&g_kh_mutex);
    GPtrArray *entries = _kh_load_locked(error);
    PcvReplHostKey *hit = entries ? _kh_find(entries, canon) : NULL;
    PcvReplHostKey *copy = hit ? _key_dup(hit) : NULL;
    g_mutex_unlock(&g_kh_mutex);
    if (entries)
        g_ptr_array_unref(entries);
    return copy;
}

                                                                                
gboolean
pcv_repl_hostkey_require(const gchar *target_node, GError **error)
{
    GError *e = NULL;
    PcvReplHostKey *k = pcv_repl_hostkey_lookup(target_node, &e);
    if (k) {
        pcv_repl_host_key_free(k);
        return TRUE;
    }
    if (e) {
        g_propagate_error(error, e);
        return FALSE;
    }
    g_set_error(error, pcv_repl_hostkey_error_quark(), PCV_REPL_HOSTKEY_ERROR_UNTRUSTED,
                "HOSTKEY_UNTRUSTED: no trusted SSH host key for '%s' — run 'pcvctl backup hostkey scan %s', "
                "compare the fingerprint on the remote console, then 'pcvctl backup hostkey trust %s <SHA256:...>'",
                target_node, target_node, target_node);
    return FALSE;
}

                                                                            
PcvReplHostkeyErrorCode
pcv_repl_classify_ssh_stderr(const gchar *ssh_stderr)
{
    return (ssh_stderr && strstr(ssh_stderr, "Host key verification failed"))
           ? PCV_REPL_HOSTKEY_ERROR_MISMATCH : PCV_REPL_HOSTKEY_ERROR_FAILED;
}

                                                                                   
void
pcv_repl_set_ssh_error(GError **error, const gchar *target_node, const gchar *ssh_stderr)
{
    const gchar *t = target_node ? target_node : "";
    g_autofree gchar *detail = g_strstrip(g_strdup(ssh_stderr ? ssh_stderr : ""));
    if (pcv_repl_classify_ssh_stderr(ssh_stderr) == PCV_REPL_HOSTKEY_ERROR_MISMATCH)
        g_set_error(error, pcv_repl_hostkey_error_quark(), PCV_REPL_HOSTKEY_ERROR_MISMATCH,
                    "HOSTKEY_MISMATCH: SSH host key of '%s' does not match the trusted key — compare it on the "
                    "remote console (ssh-keygen -lf /etc/ssh/ssh_host_<type>_key.pub), then run "
                    "'pcvctl backup hostkey remove %s' and 'pcvctl backup hostkey trust %s <SHA256:...>'",
                    t, t, t);
    else
        g_set_error(error, pcv_repl_hostkey_error_quark(), PCV_REPL_HOSTKEY_ERROR_FAILED,
                    "ssh to '%s' failed: %s", t, *detail ? detail : "unknown error");
}

                                                                                   
gboolean
pcv_repl_hostkey_probe(const gchar *ssh_user, const gchar *target_node, GError **error)
{
    if (!pcv_repl_hostkey_require(target_node, error))
        return FALSE;
    g_auto(GStrv) argv = pcv_repl_probe_argv(ssh_user, target_node);
    if (!argv) {
        g_set_error(error, pcv_repl_hostkey_error_quark(), PCV_REPL_HOSTKEY_ERROR_INVALID,
                    "invalid replication SSH user or target");
        return FALSE;
    }
    g_autofree gchar *err_text = NULL;
    GError *spawn_err = NULL;
    gboolean ok = pcv_spawn_sync_timeout((const gchar * const *)argv, NULL, &err_text,
                                         PCV_REPL_PROBE_TIMEOUT_SEC, &spawn_err);
    if (!ok)
        pcv_repl_set_ssh_error(error, target_node,
                               err_text && *err_text ? err_text : spawn_err ? spawn_err->message : NULL);
    g_clear_error(&spawn_err);
    return ok;
}

                                                                       
                                                                                  
                                                                                 
GPtrArray *
pcv_repl_parse_keyscan(const gchar *output)
{
    GPtrArray *keys = g_ptr_array_new_with_free_func((GDestroyNotify)pcv_repl_host_key_free);
    gchar **lines = g_strsplit(output ? output : "", "\n", -1);
    for (gchar **l = lines; *l; l++) {
        gchar *line = g_strstrip(*l);
        if (!*line || *line == '#')
            continue;
        gchar **f = _three_fields(line);
        PcvReplHostKey *k = f ? _key_new(NULL, f[1], f[2]) : NULL;
        g_strfreev(f);
        for (guint i = 0; k && i < keys->len; i++) {
            const PcvReplHostKey *seen = g_ptr_array_index(keys, i);
            if (g_str_equal(seen->type, k->type) && g_str_equal(seen->blob_b64, k->blob_b64))
                g_clear_pointer(&k, pcv_repl_host_key_free);
        }
        if (k)
            g_ptr_array_add(keys, k);
    }
    g_strfreev(lines);
    return keys;
}

                                                                           
                                                                          
                                                                   
gboolean
pcv_repl_keyscan(const gchar *target_node, GPtrArray **out_keys, GError **error)
{
    *out_keys = NULL;
    g_autofree gchar *canon = pcv_repl_canonical_target(target_node);
    if (!canon) {
        g_set_error(error, pcv_repl_hostkey_error_quark(), PCV_REPL_HOSTKEY_ERROR_INVALID,
                    "invalid replication target '%s'", target_node ? target_node : "");
        return FALSE;
    }
    g_autofree gchar *t = g_strdup_printf("%d", PCV_REPL_CONNECT_TIMEOUT_SEC);
    const gchar *argv[] = { "ssh-keyscan", "-T", t, "-t", "ed25519,ecdsa,rsa", canon, NULL };
    g_autofree gchar *out = NULL;
    g_autofree gchar *err_text = NULL;
    GError *spawn_err = NULL;
    (void)pcv_spawn_sync_timeout(argv, &out, &err_text, PCV_REPL_SCAN_TIMEOUT_SEC, &spawn_err);
    GPtrArray *keys = pcv_repl_parse_keyscan(out);
    if (keys->len == 0) {
        gboolean missing = spawn_err && (g_error_matches(spawn_err, G_SPAWN_ERROR, G_SPAWN_ERROR_NOENT) ||
                                         g_error_matches(spawn_err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND));
        if (missing)
            g_set_error_literal(error, pcv_repl_hostkey_error_quark(), PCV_REPL_HOSTKEY_ERROR_FAILED,
                                "ssh-keyscan not found — install the OpenSSH client");
        else
            g_set_error(error, pcv_repl_hostkey_error_quark(), PCV_REPL_HOSTKEY_ERROR_FAILED,
                        "host key scan of '%s' returned no keys (unreachable or no sshd): %s", canon,
                        spawn_err ? spawn_err->message : err_text && *err_text ? g_strstrip(err_text) : "no output");
        g_clear_error(&spawn_err);
        g_ptr_array_unref(keys);
        return FALSE;
    }
    g_clear_error(&spawn_err);
    *out_keys = keys;
    return TRUE;
}

                                                                      
                                               
gboolean
pcv_repl_fingerprint_valid(const gchar *fingerprint)
{
    if (!fingerprint || !g_str_has_prefix(fingerprint, "SHA256:") || strlen(fingerprint) != 7 + 43)
        return FALSE;
    for (const gchar *p = fingerprint + 7; *p; p++)
        if (!g_ascii_isalnum(*p) && *p != '+' && *p != '/')
            return FALSE;
    return TRUE;
}

                                                                       
static gboolean
_kh_save_locked(GPtrArray *entries, GError **error)
{
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < entries->len; i++) {
        const PcvReplHostKey *k = g_ptr_array_index(entries, i);
        g_string_append_printf(s, "%s %s %s\n", k->target, k->type, k->blob_b64);
    }
    GError *e = NULL;
    gboolean ok = g_file_set_contents_full(_kh_path(), s->str, (gssize)s->len,
                                           G_FILE_SET_CONTENTS_CONSISTENT, 0600, &e);
    g_string_free(s, TRUE);
    if (!ok) {
        g_set_error(error, pcv_repl_hostkey_error_quark(), PCV_REPL_HOSTKEY_ERROR_FAILED,
                    "cannot write %s: %s", _kh_path(), e->message);
        g_error_free(e);
    }
    return ok;
}

                                                                            
                                                                         
                                                   
gboolean
pcv_repl_hostkey_add(const gchar *target_node, const PcvReplHostKey *key,
                     gboolean *out_added, GError **error)
{
    if (out_added)
        *out_added = FALSE;
    g_autofree gchar *canon = pcv_repl_canonical_target(target_node);
    PcvReplHostKey *fresh = (canon && key) ? _key_new(canon, key->type, key->blob_b64) : NULL;
    if (!fresh) {
        g_set_error(error, pcv_repl_hostkey_error_quark(), PCV_REPL_HOSTKEY_ERROR_INVALID,
                    "invalid replication target or host key");
        return FALSE;
    }
    g_mutex_lock(&g_kh_mutex);
    GPtrArray *entries = _kh_load_locked(error);
    gboolean ok = entries != NULL;
    PcvReplHostKey *cur = ok ? _kh_find(entries, canon) : NULL;
    if (cur && !(g_str_equal(cur->type, fresh->type) && g_str_equal(cur->blob_b64, fresh->blob_b64))) {
        g_set_error(error, pcv_repl_hostkey_error_quark(), PCV_REPL_HOSTKEY_ERROR_CONFLICT,
                    "a different host key (%s %s) is already trusted for '%s' — run "
                    "'pcvctl backup hostkey remove %s' first", cur->type, cur->fingerprint, canon, canon);
        ok = FALSE;
    } else if (ok && !cur) {
        g_ptr_array_add(entries, g_steal_pointer(&fresh));
        ok = _kh_save_locked(entries, error);
        if (ok && out_added)
            *out_added = TRUE;
    }
    g_mutex_unlock(&g_kh_mutex);
    if (entries)
        g_ptr_array_unref(entries);
    pcv_repl_host_key_free(fresh);
    return ok;
}

                                                                       
                                                                           
                                                                   
gboolean
pcv_repl_hostkey_remove(const gchar *target_node, GError **error)
{
    g_autofree gchar *canon = pcv_repl_canonical_target(target_node);
    if (!canon) {
        g_set_error(error, pcv_repl_hostkey_error_quark(), PCV_REPL_HOSTKEY_ERROR_INVALID,
                    "invalid replication target '%s'", target_node ? target_node : "");
        return FALSE;
    }
    g_mutex_lock(&g_kh_mutex);
    GPtrArray *entries = _kh_load_locked(error);
    gboolean ok = entries != NULL;
    PcvReplHostKey *cur = ok ? _kh_find(entries, canon) : NULL;
    if (ok && !cur) {
        g_set_error(error, pcv_repl_hostkey_error_quark(), PCV_REPL_HOSTKEY_ERROR_NOT_FOUND,
                    "no trusted host key for '%s'", canon);
        ok = FALSE;
    } else if (ok) {
        g_ptr_array_remove(entries, cur);
        ok = _kh_save_locked(entries, error);
    }
    g_mutex_unlock(&g_kh_mutex);
    if (entries)
        g_ptr_array_unref(entries);
    return ok;
}

                                                               
                                           
GPtrArray *
pcv_repl_hostkey_list(GError **error)
{
    g_mutex_lock(&g_kh_mutex);
    GPtrArray *entries = _kh_load_locked(error);
    g_mutex_unlock(&g_kh_mutex);
    return entries;
}

                                                                 
                                              
gint
pcv_repl_hostkey_rpc_code(const GError *error)
{
    if (!error || error->domain != pcv_repl_hostkey_error_quark())
        return PURE_RPC_ERR_ZFS_OPERATION;
    switch ((PcvReplHostkeyErrorCode)error->code) {
    case PCV_REPL_HOSTKEY_ERROR_INVALID:   return PURE_RPC_ERR_INVALID_PARAMS;
    case PCV_REPL_HOSTKEY_ERROR_UNTRUSTED: return PURE_RPC_ERR_HOSTKEY_UNTRUSTED;
    case PCV_REPL_HOSTKEY_ERROR_MISMATCH:  return PURE_RPC_ERR_HOSTKEY_MISMATCH;
    case PCV_REPL_HOSTKEY_ERROR_CONFLICT:  return PURE_RPC_ERR_CONFLICT;
    case PCV_REPL_HOSTKEY_ERROR_NOT_FOUND: return PURE_RPC_ERR_NOT_FOUND;
    case PCV_REPL_HOSTKEY_ERROR_FAILED:    break;
    }
    return PURE_RPC_ERR_ZFS_OPERATION;
}
