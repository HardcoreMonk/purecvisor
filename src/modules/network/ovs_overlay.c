   
                      
                                            
  
                           
                                                   
                                                    
                                        
  
          
                                                
                                              
                                                    
                                                
                                                         
                                                     
                                
  
         
                                                             
                                                      
        
                                                                  
                                                      
                                                                             

         
                                                   
                                                      
                                                     
        
                                                         
                                                                     
                                       
        
                                                             
                                                                       
                                                                       

  
                                                                       
            
                                             
                                                  
  
                                                       
                                             
          
  
                                          
                                                 
                                               
                                                    
                                                          
                                                
                                        
  
             

                                            

                                            

                                                                  

  
                    


  
             
                                                  
                                                 
  



  
          
                                      
                                         
  
         
                                              


                                                   
                                        
                                                                       
   
#include "api/drain.h"
#include "ovs_overlay.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_log.h"
#include "utils/pcv_config.h"                                             
#include "utils/pcv_validate.h"
#include "utils/pcv_worker_pool.h"                                               
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/fs.h>
#include <dirent.h>
#include <glib/gstdio.h>                             

#define OVERLAY_LOG_DOM   "ovs_overlay"
                                                     
                                                   
#define OVERLAY_META_DIR  "/var/lib/purecvisor/overlay"
#define OVERLAY_MAX       16
#define OVERLAY_MAX_PEERS 64
#define OVS_TIMEOUT_ARG   "--timeout=5"
#define OVS_OWNER_KEY     "pcv_overlay_owner"
#define OVS_NAME_KEY      "pcv_overlay_name"
#define OVERLAY_META_SCHEMA_VERSION 2
#define OVERLAY_META_MAX_BYTES (1024U * 1024U)
#define OVERLAY_META_MAX_DENTRIES (OVERLAY_MAX * 4)
#define OVERLAY_RESTORE_DEADLINE_USEC (30 * G_USEC_PER_SEC)
#define OVERLAY_RESTORE_CLEANUP_RESERVE_USEC (5 * G_USEC_PER_SEC)
#define OVERLAY_RESTORE_LAUNCHER_MARGIN_USEC G_USEC_PER_SEC

typedef struct {
    gchar    *name;
    gchar    *cidr;
    gchar    *owner_token;
    gint      vni;
    guint64   generation;
    GPtrArray *peers;                        
    gboolean  active;
} OverlayNet;

typedef struct {
    gchar *data;
    gsize len;
    dev_t claimed_dev;
    ino_t claimed_ino;
} OverlayMetaSnapshot;

typedef enum {
    OVERLAY_META_REPLACE_FAILED = 0,
    OVERLAY_META_REPLACE_COMMITTED,
    OVERLAY_META_REPLACE_COMMITTED_DEGRADED,
} OverlayMetaReplaceResult;

typedef struct OverlayMeta OverlayMeta;

typedef struct {
    gchar *uuid;
    gchar *name;
    gchar *owner;
    gchar *overlay_name;
    gchar *datapath_type;
    GPtrArray *ports;
} OverlayBulkBridge;

typedef struct {
    gchar *uuid;
    gchar *name;
    gchar *owner;
    gchar *overlay_name;
    GPtrArray *interfaces;
} OverlayBulkPort;

typedef struct {
    gchar *uuid;
    gchar *name;
    gchar *owner;
    gchar *overlay_name;
    gchar *type;
    GHashTable *options;
} OverlayBulkInterface;

typedef struct {
    GHashTable *bridges_by_name;
    GHashTable *bridges_by_uuid;
    GHashTable *ports_by_name;
    GHashTable *interfaces_by_name;
    GHashTable *ports_by_uuid;
    GHashTable *interfaces_by_uuid;



    GHashTable *interface_ref_counts;
    GHashTable *port_parent_counts;
    GHashTable *ipv4_by_name;
} OverlayBulkSnapshot;

static struct {
    gchar      *local_ip;
    gchar      *restore_error;
    guint       restore_rejected_count;
    guint64     restore_state_epoch;
    gboolean    restore_in_progress;
    gboolean    restore_accepts_mutation;
    OverlayNet  nets[OVERLAY_MAX];
    gint        count;
    GMutex      mu;
    gboolean    initialized;
} G = {0};



static PcvOverlayExecFn G_exec_hook = NULL;
static gchar *G_meta_dir_override = NULL;
static PcvOverlayRestoreSnapshotHook G_restore_snapshot_hook = NULL;
static gpointer G_restore_snapshot_hook_data = NULL;

                                                                          
static guint    g_overlay_reconcile_timer_id = 0;
static GMutex  g_overlay_reconcile_mu;
static GCond   g_overlay_reconcile_cond;
static guint   g_overlay_reconcile_inflight = 0;
static gboolean g_overlay_reconcile_stopping = FALSE;
static guint   g_overlay_public_inflight = 0;
static gboolean g_overlay_lifecycle_stopping = FALSE;
static PcvOverlayLifecycleTestHook G_lifecycle_test_hook = NULL;
static gpointer G_lifecycle_test_hook_data = NULL;
static PcvOverlayMetadataTestHook G_metadata_test_hook = NULL;
static gpointer G_metadata_test_hook_data = NULL;
static PcvOverlayDirSyncTestHook G_dir_sync_test_hook = NULL;
static gpointer G_dir_sync_test_hook_data = NULL;
static PcvOverlayMetadataStatTestHook G_metadata_stat_test_hook = NULL;
static gpointer G_metadata_stat_test_hook_data = NULL;
static gsize G_meta_max_bytes = OVERLAY_META_MAX_BYTES;
static gint64 G_restore_deadline_test_usec = 0;
static GPrivate G_restore_deadline = G_PRIVATE_INIT(g_free);
static GPrivate G_restore_hard_deadline = G_PRIVATE_INIT(g_free);


static GPrivate G_restore_bulk_snapshot = G_PRIVATE_INIT(NULL);

typedef struct {
    gint64 *deadline;
    gint64 saved;
    gboolean extended;
} OverlayCleanupDeadlineScope;

static gboolean _overlay_create_impl(const gchar *name, gint vni, const gchar *cidr,
                                     GError **error);
static gboolean _overlay_delete_impl(const gchar *name, GError **error);
static JsonArray *_overlay_list_impl(GError **error);
static JsonObject *_overlay_info_impl(const gchar *name, GError **error);
static gboolean _overlay_add_peer_impl(const gchar *name, const gchar *peer_tunnel_ip,
                                       GError **error);
static gboolean _overlay_remove_peer_impl(const gchar *name,
                                          const gchar *peer_tunnel_ip,
                                          GError **error);
static gboolean _metadata_snapshot_unchanged(
    const gchar *path, const OverlayMetaSnapshot *snapshot, GError **error);
static gboolean _quarantine_remove_expected(
    const gchar *path, const OverlayMetaSnapshot *snapshot, GError **error);
static gboolean _sync_meta_dir(GError **error);
static GPtrArray *_bridge_ports(const gchar *bridge, GError **error);
static gboolean _ptr_array_has_string(GPtrArray *values, const gchar *needle);
static const gchar *_peer_for_canonical_port(OverlayNet *net,
                                             const gchar *port_name);
static const gchar *_overlay_json_string(JsonNode *node);
static gboolean _audit_all_owned_actual_locked(GError **error);
static gboolean _audit_registry_metadata_locked(GError **error);
static void _overlay_restore_impl(void);
static OverlayBulkSnapshot *_bulk_snapshot_load(GError **error);
static void _bulk_snapshot_free(OverlayBulkSnapshot *snapshot);
static gboolean _bulk_net_exact(OverlayBulkSnapshot *snapshot,
                                OverlayNet *net, gboolean *missing_out,
                                GError **error);
static gboolean _bulk_global_exact(OverlayBulkSnapshot *snapshot,
                                   GError **error);
static gboolean _bulk_preflight_metadata_exact(
    OverlayBulkSnapshot *snapshot, GPtrArray *metas, GError **error);
static gboolean _validate_legacy_actual_locked(
    OverlayNet *net, gboolean *bridge_exists_out, GError **error);
static gboolean _bulk_legacy_relationships_exact(
    OverlayBulkSnapshot *snapshot, OverlayNet *net, GError **error);
static gboolean _overlay_meta_matches_net(OverlayMeta *meta, OverlayNet *net);
static OverlayBulkSnapshot *_restore_bulk_get(void);
static void _restore_bulk_replace(OverlayBulkSnapshot *snapshot);







static OverlayCleanupDeadlineScope
_restore_cleanup_deadline_enter(void)
{
    OverlayCleanupDeadlineScope scope = {
        .deadline = g_private_get(&G_restore_deadline),
    };
    if (!scope.deadline)
        return scope;
    scope.saved = *scope.deadline;
    gint64 *hard_deadline = g_private_get(&G_restore_hard_deadline);



    if (hard_deadline && *scope.deadline < *hard_deadline) {
        *scope.deadline = *hard_deadline;
        scope.extended = TRUE;
    }
    return scope;
}

static void
_restore_cleanup_deadline_leave(OverlayCleanupDeadlineScope *scope)
{
    if (scope->extended)
        *scope->deadline = scope->saved;
}







static gboolean
_restore_deadline_check(GError **error)
{
    gint64 *deadline = g_private_get(&G_restore_deadline);
    if (!deadline || g_get_monotonic_time() < *deadline)
        return TRUE;
    if (!error || !*error)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                    "Overlay restore/audit exceeded its total deadline");
    return FALSE;
}

static OverlayBulkSnapshot *
_restore_bulk_get(void)
{
    return g_private_get(&G_restore_bulk_snapshot);
}







static void
_restore_bulk_replace(OverlayBulkSnapshot *snapshot)
{
    OverlayBulkSnapshot *old = g_private_get(&G_restore_bulk_snapshot);
    if (old != snapshot)
        _bulk_snapshot_free(old);
    g_private_set(&G_restore_bulk_snapshot, snapshot);
}

                                                                     

   
                            
                                    
                                                 
  
                                               
                                          
                                 
  
                                       
   
static OverlayNet *
_find(const gchar *name)
{
    for (gint i = 0; i < G.count; i++)
        if (g_strcmp0(G.nets[i].name, name) == 0)
            return &G.nets[i];
    return NULL;
}

   

  





  


   
gboolean
pcv_overlay_validate_name(const gchar *name)
{
    return pcv_validate_bridge_name(name) && strlen(name) <= PCV_MAX_IFACE_NAME;
}

gboolean
pcv_overlay_validate_vni(gint64 vni)
{
    return vni >= PCV_OVERLAY_VNI_MIN && vni <= PCV_OVERLAY_VNI_MAX;
}

gboolean
pcv_overlay_validate_peer_ip(const gchar *peer_ip)
{
    if (!pcv_validate_ip_literal(peer_ip))
        return FALSE;

    GInetAddress *addr = g_inet_address_new_from_string(peer_ip);
    gboolean valid = addr &&
        g_inet_address_get_family(addr) == G_SOCKET_FAMILY_IPV4;
    g_clear_object(&addr);
    return valid;
}

gboolean
pcv_overlay_validate_cidr(const gchar *cidr)
{
    if (!cidr || !*cidr || !pcv_validate_cidr(cidr))
        return FALSE;
    gchar **parts = g_strsplit(cidr, "/", 3);
    gchar *end = NULL;
    gint64 prefix = parts[1] ? g_ascii_strtoll(parts[1], &end, 10) : -1;
    GInetAddress *addr = parts[0]
        ? g_inet_address_new_from_string(parts[0]) : NULL;
    gboolean valid = parts[0] && parts[1] && !parts[2] &&
        end && *end == '\0' && prefix >= 0 && prefix <= 32 && addr &&
        g_inet_address_get_family(addr) == G_SOCKET_FAMILY_IPV4;
    g_clear_object(&addr);
    g_strfreev(parts);
    return valid;
}

gchar *
pcv_overlay_peer_port_name(gint64 vni, const gchar *peer_ip)
{
    if (!pcv_overlay_validate_vni(vni) ||
        !pcv_overlay_validate_peer_ip(peer_ip))
        return NULL;

    GInetAddress *address = g_inet_address_new_from_string(peer_ip);
    const guint8 *bytes = g_inet_address_to_bytes(address);
    guint32 ipv4 = ((guint32)bytes[0] << 24) |
                   ((guint32)bytes[1] << 16) |
                   ((guint32)bytes[2] << 8) |
                   (guint32)bytes[3];
    gchar *name = g_strdup_printf("v%06x%08x", (guint)vni, ipv4);
    g_object_unref(address);
    return name;
}



static gchar *
_legacy_peer_port_name(const gchar *peer_ip)
{
    gchar **parts = g_strsplit(peer_ip, ".", -1);
    gchar *name = g_strv_length(parts) == 4
        ? g_strdup_printf("vxlan-%s-%s", parts[2], parts[3]) : NULL;
    g_strfreev(parts);
    return name;
}

   
                                                 
                       
                                
                                        
                                                    
                                    
  

                                  
  
                    
   
static gboolean
_run_argv_capture(const gchar * const *argv, gchar **stdout_out, GError **error)
{
    gint64 *deadline = g_private_get(&G_restore_deadline);
    gint64 remaining = deadline ? *deadline - g_get_monotonic_time() : 0;



    if (deadline && remaining < G_USEC_PER_SEC) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                    "Overlay restore/audit exceeded its total deadline");
        return FALSE;
    }






    guint child_timeout_sec = 5;
    if (deadline) {
        gint64 seconds = remaining / G_USEC_PER_SEC;
        child_timeout_sec = (guint)CLAMP(seconds, 1, 5);
    }

    const gchar * const *effective = argv;
    GPtrArray *bounded = NULL;
    gchar *deadline_timeout = NULL;
    if (g_strcmp0(argv[0], "ovs-vsctl") == 0 &&
        (!argv[1] || !g_str_has_prefix(argv[1], "--timeout="))) {
        bounded = g_ptr_array_new();
        g_ptr_array_add(bounded, (gpointer)argv[0]);
        if (deadline) {
            gint64 seconds = remaining / G_USEC_PER_SEC;
            seconds = CLAMP(seconds, 1, 5);
            deadline_timeout = g_strdup_printf("--timeout=%" G_GINT64_FORMAT,
                                               seconds);
        }
        g_ptr_array_add(bounded, deadline_timeout
            ? deadline_timeout : (gpointer)OVS_TIMEOUT_ARG);
        for (gint i = 1; argv[i]; i++)
            g_ptr_array_add(bounded, (gpointer)argv[i]);
        g_ptr_array_add(bounded, NULL);
        effective = (const gchar * const *)bounded->pdata;
    }

    if (G_exec_hook) {
        gboolean hooked = G_exec_hook(effective, stdout_out, error);
        if (bounded)
            g_ptr_array_free(bounded, TRUE);
        g_free(deadline_timeout);
        return hooked;
    }

    gchar *std_err = NULL;
    gboolean ok = pcv_spawn_sync_timeout(effective, stdout_out, &std_err,
                                         child_timeout_sec, error);
    if (!ok && std_err) {
                                                               
                                                       
        PCV_LOG_WARN(OVERLAY_LOG_DOM, "cmd failed: %s → %s", effective[0], std_err);
    }
    g_free(std_err);
    if (bounded)
        g_ptr_array_free(bounded, TRUE);
    g_free(deadline_timeout);
    return ok;
}

static const gchar *
_overlay_meta_dir(void)
{
    return G_meta_dir_override ? G_meta_dir_override : OVERLAY_META_DIR;
}

static gchar *
_overlay_meta_path(const gchar *name)
{
    gchar *filename = g_strdup_printf("overlay-%s.meta", name);
    gchar *path = g_build_filename(_overlay_meta_dir(), filename, NULL);
    g_free(filename);
    return path;
}

static gchar *
_overlay_tombstone_path(const gchar *name)
{
    gchar *filename = g_strdup_printf("overlay-%s.meta.deleting", name);
    gchar *path = g_build_filename(_overlay_meta_dir(), filename, NULL);
    g_free(filename);
    return path;
}

static gchar *
_overlay_update_path(const gchar *name)
{
    gchar *filename = g_strdup_printf("overlay-%s.meta.updating", name);
    gchar *path = g_build_filename(_overlay_meta_dir(), filename, NULL);
    g_free(filename);
    return path;
}



static gboolean
_path_entry_exists(const gchar *path)
{
    struct stat st;
    return g_lstat(path, &st) == 0 || errno != ENOENT;
}



                                                


   
static gboolean
_metadata_inventory_exact(const gchar *name, gboolean canonical_expected,
                          GError **error)
{
    gchar *canonical_name = g_strdup_printf("overlay-%s.meta", name);
    gchar *sidecar_prefix = g_strdup_printf("%s.", canonical_name);
    GDir *dir = g_dir_open(_overlay_meta_dir(), 0, error);
    if (!dir) {
        g_free(canonical_name); g_free(sidecar_prefix);
        return FALSE;
    }
    gboolean canonical_found = FALSE;
    gboolean residue_found = FALSE;
    const gchar *entry = NULL;
    while ((entry = g_dir_read_name(dir)) != NULL) {
        if (!_restore_deadline_check(error)) {
            canonical_found = FALSE;
            residue_found = TRUE;
            break;
        }
        if (g_strcmp0(entry, canonical_name) == 0)
            canonical_found = TRUE;
        else if (g_str_has_prefix(entry, sidecar_prefix))
            residue_found = TRUE;
    }
    g_dir_close(dir);
    gboolean exact = canonical_found == canonical_expected && !residue_found;
    if (!exact && (!error || !*error))
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "Overlay '%s' metadata inventory is not canonical-only",
                    name);
    g_free(canonical_name); g_free(sidecar_prefix);
    return exact;
}



                                             



   
static gboolean
_metadata_directory_cardinality_bounded(GError **error)
{
    GDir *dir = g_dir_open(_overlay_meta_dir(), 0, error);
    if (!dir)
        return FALSE;
    guint total = 0;
    guint canonical = 0;
    const gchar *entry = NULL;
    gboolean bounded = TRUE;
    while ((entry = g_dir_read_name(dir)) != NULL) {
        if (!_restore_deadline_check(error)) {
            bounded = FALSE;
            break;
        }
        if (g_strcmp0(entry, ".") == 0 || g_strcmp0(entry, "..") == 0)
            continue;
        total++;
        if (g_str_has_prefix(entry, "overlay-") &&
            g_str_has_suffix(entry, ".meta") &&
            strstr(entry, ".meta.") == NULL)
            canonical++;
        if (total > OVERLAY_META_MAX_DENTRIES || canonical > OVERLAY_MAX) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                        "Overlay metadata directory exceeds bounded cardinality (%u/%u)",
                        total, canonical);
            bounded = FALSE;
            break;
        }
    }
    g_dir_close(dir);
    return bounded;
}










static gboolean
_safe_read_metadata_full(const gchar *path, gchar **data_out, gsize *length_out,
                         struct stat *stat_out, GError **error)
{
    *data_out = NULL;
    *length_out = 0;
    gint fd = g_open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0);
    if (fd < 0) {
        gint saved_errno = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Cannot open overlay metadata '%s': %s", path,
                    g_strerror(saved_errno));
        return FALSE;
    }
    struct stat st = {0};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        st.st_nlink != 1 || st.st_size < 0 ||
        (guint64)st.st_size > G_meta_max_bytes ||
        (st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        close(fd);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "Overlay metadata '%s' is not a bounded owner-controlled regular file",
                    path);
        return FALSE;
    }

    gsize length = (gsize)st.st_size;
    gchar *data = g_malloc(length + 1);
    gsize offset = 0;
    while (offset < length) {
        if (!_restore_deadline_check(error)) {
            close(fd);
            g_free(data);
            return FALSE;
        }
        ssize_t n = read(fd, data + offset, length - offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            gint saved_errno = n < 0 ? errno : EIO;
            close(fd);
            g_free(data);
            g_set_error(error, G_FILE_ERROR,
                        g_file_error_from_errno(saved_errno),
                        "Cannot read overlay metadata '%s': %s", path,
                        g_strerror(saved_errno));
            return FALSE;
        }
        offset += (gsize)n;
    }
    struct stat after = {0};
    if (fstat(fd, &after) != 0 || after.st_dev != st.st_dev ||
        after.st_ino != st.st_ino || after.st_nlink != st.st_nlink ||
        after.st_mode != st.st_mode || after.st_uid != st.st_uid ||
        after.st_size != st.st_size ||
        after.st_mtim.tv_sec != st.st_mtim.tv_sec ||
        after.st_mtim.tv_nsec != st.st_mtim.tv_nsec ||
        after.st_ctim.tv_sec != st.st_ctim.tv_sec ||
        after.st_ctim.tv_nsec != st.st_ctim.tv_nsec) {
        close(fd);
        g_free(data);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "Overlay metadata '%s' changed while being read", path);
        return FALSE;
    }
    struct stat path_st = {0};
    if (g_lstat(path, &path_st) != 0 || !S_ISREG(path_st.st_mode) ||
        path_st.st_dev != after.st_dev || path_st.st_ino != after.st_ino) {
        close(fd);
        g_free(data);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "Overlay metadata '%s' pathname changed while being read",
                    path);
        return FALSE;
    }
    if (close(fd) != 0) {
        gint saved_errno = errno;
        g_free(data);
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Cannot close overlay metadata '%s': %s", path,
                    g_strerror(saved_errno));
        return FALSE;
    }
    data[length] = '\0';
    *data_out = data;
    *length_out = length;
    if (stat_out)
        *stat_out = after;
    return TRUE;
}

static gboolean
_safe_read_metadata(const gchar *path, gchar **data_out, gsize *length_out,
                    GError **error)
{
    return _safe_read_metadata_full(path, data_out, length_out, NULL, error);
}




static gboolean
_disabled_metadata_inventory_clean(GError **error)
{
    GError *local_error = NULL;
    GDir *dir = g_dir_open(_overlay_meta_dir(), 0, &local_error);
    if (!dir) {
        if (g_error_matches(local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            g_clear_error(&local_error);
            return TRUE;
        }
        g_propagate_error(error, local_error);
        return FALSE;
    }
    guint count = 0;
    const gchar *entry = NULL;
    while ((entry = g_dir_read_name(dir)) != NULL) {
        if (++count > OVERLAY_META_MAX_DENTRIES) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                        "Disabled overlay metadata inventory exceeds its bound");
            g_dir_close(dir);
            return FALSE;
        }
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "Overlay is disabled but durable metadata residue remains");
        g_dir_close(dir);
        return FALSE;
    }
    g_dir_close(dir);
    return TRUE;
}










static gboolean
_overlay_public_enter(const gchar *operation)
{
    gboolean entered = FALSE;
    g_mutex_lock(&g_overlay_reconcile_mu);
    if (G.initialized && !g_overlay_lifecycle_stopping) {
        g_overlay_public_inflight++;
        entered = TRUE;
    }
    g_mutex_unlock(&g_overlay_reconcile_mu);
    if (entered && G_lifecycle_test_hook)
        G_lifecycle_test_hook(operation, G_lifecycle_test_hook_data);
    return entered;
}







static void
_overlay_public_leave(void)
{
    g_mutex_lock(&g_overlay_reconcile_mu);
    g_assert(g_overlay_public_inflight > 0);
    g_overlay_public_inflight--;
    g_cond_broadcast(&g_overlay_reconcile_cond);
    g_mutex_unlock(&g_overlay_reconcile_mu);
}

static gboolean
_overlay_manager_is_initialized(void)
{
    g_mutex_lock(&g_overlay_reconcile_mu);
    gboolean initialized = G.initialized;
    g_mutex_unlock(&g_overlay_reconcile_mu);
    return initialized;
}


void
pcv_overlay_set_test_context(PcvOverlayExecFn exec_fn, const gchar *meta_dir)
{


    if (_overlay_manager_is_initialized()) {
        PCV_LOG_WARN(OVERLAY_LOG_DOM,
                     "refusing overlay test-context change while manager is active");
        return;
    }
    G_exec_hook = exec_fn;
    g_free(G_meta_dir_override);
    G_meta_dir_override = meta_dir ? g_strdup(meta_dir) : NULL;
}


void
pcv_overlay_set_restore_snapshot_test_hook(PcvOverlayRestoreSnapshotHook hook,
                                           gpointer user_data)
{
    if (_overlay_manager_is_initialized()) {
        PCV_LOG_WARN(OVERLAY_LOG_DOM,
                     "refusing restore snapshot hook change while manager is active");
        return;
    }
    G_restore_snapshot_hook = hook;
    G_restore_snapshot_hook_data = user_data;
}


void
pcv_overlay_set_lifecycle_test_hook(PcvOverlayLifecycleTestHook hook,
                                    gpointer user_data)
{
    if (_overlay_manager_is_initialized()) {
        PCV_LOG_WARN(OVERLAY_LOG_DOM,
                     "refusing lifecycle test hook change while manager is active");
        return;
    }
    G_lifecycle_test_hook = hook;
    G_lifecycle_test_hook_data = user_data;
}


void
pcv_overlay_set_metadata_test_hook(PcvOverlayMetadataTestHook hook,
                                   gpointer user_data)
{
    if (_overlay_manager_is_initialized()) {
        PCV_LOG_WARN(OVERLAY_LOG_DOM,
                     "refusing metadata test hook change while manager is active");
        return;
    }
    G_metadata_test_hook = hook;
    G_metadata_test_hook_data = user_data;
}


void
pcv_overlay_set_dir_sync_test_hook(PcvOverlayDirSyncTestHook hook,
                                   gpointer user_data)
{
    if (_overlay_manager_is_initialized()) {
        PCV_LOG_WARN(OVERLAY_LOG_DOM,
                     "refusing directory-sync hook change while manager is active");
        return;
    }
    G_dir_sync_test_hook = hook;
    G_dir_sync_test_hook_data = user_data;
}


void
pcv_overlay_set_metadata_stat_test_hook(PcvOverlayMetadataStatTestHook hook,
                                        gpointer user_data)
{
    if (_overlay_manager_is_initialized()) {
        PCV_LOG_WARN(OVERLAY_LOG_DOM,
                     "refusing metadata-stat hook change while manager is active");
        return;
    }
    G_metadata_stat_test_hook = hook;
    G_metadata_stat_test_hook_data = user_data;
}


void
pcv_overlay_set_metadata_limit_for_test(gsize max_bytes)
{
    if (_overlay_manager_is_initialized()) {
        PCV_LOG_WARN(OVERLAY_LOG_DOM,
                     "refusing metadata-limit change while manager is active");
        return;
    }
    G_meta_max_bytes = max_bytes ? max_bytes : OVERLAY_META_MAX_BYTES;
}


void
pcv_overlay_set_restore_deadline_for_test(gint64 milliseconds)
{
    if (_overlay_manager_is_initialized()) {
        PCV_LOG_WARN(OVERLAY_LOG_DOM,
                     "refusing restore-deadline change while manager is active");
        return;
    }
    G_restore_deadline_test_usec = milliseconds > 0
        ? milliseconds * 1000 : 0;
}

static gboolean
_run_argv(const gchar * const *argv, GError **error)
{
    return _run_argv_capture(argv, NULL, error);
}

static void
_argv_add(GPtrArray *argv, const gchar *value)
{
    g_ptr_array_add(argv, g_strdup(value));
}

static gboolean
_run_dynamic_argv(GPtrArray *argv, GError **error)
{
    g_ptr_array_add(argv, NULL);
    gboolean ok = _run_argv((const gchar * const *)argv->pdata, error);
    g_ptr_array_remove_index(argv, argv->len - 1);
    return ok;
}



static gboolean
_ovs_row_exists(const gchar *table, const gchar *column, const gchar *value,
                gboolean *exists, GError **error)
{
    gchar *predicate = g_strdup_printf("%s=%s", column, value);
    const gchar *argv[] = {
        "ovs-vsctl", "--bare", "--columns=name", "find", table, predicate, NULL
    };
    gchar *stdout_buf = NULL;
    gboolean ok = _run_argv_capture(argv, &stdout_buf, error);
    if (ok) {
        gchar *trimmed = stdout_buf ? g_strstrip(stdout_buf) : NULL;
        *exists = trimmed && *trimmed;
    }
    g_free(stdout_buf);
    g_free(predicate);
    return ok;
}

static GPtrArray *
_ovs_find_names(const gchar *table, const gchar *column, const gchar *value,
                GError **error)
{
    gchar *predicate = g_strdup_printf("%s=%s", column, value);
    const gchar *argv[] = {
        "ovs-vsctl", "--bare", "--columns=name", "find", table, predicate, NULL
    };
    gchar *output = NULL;
    if (!_run_argv_capture(argv, &output, error)) {
        g_free(predicate); g_free(output);
        return NULL;
    }
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    gchar **lines = g_strsplit(output ? output : "", "\n", -1);
    for (gint i = 0; lines[i]; i++) {
        gchar *name = g_strstrip(lines[i]);
        if (*name && !_ptr_array_has_string(names, name))
            g_ptr_array_add(names, g_strdup(name));
    }
    g_strfreev(lines);
    g_free(output);
    g_free(predicate);
    return names;
}

static GPtrArray *
_ovs_find_union(const gchar *table,
                const gchar *first_column, const gchar *first_value,
                const gchar *second_column, const gchar *second_value,
                GError **error)
{
    GPtrArray *result = _ovs_find_names(table, first_column, first_value, error);
    if (!result)
        return NULL;
    GPtrArray *second = _ovs_find_names(table, second_column, second_value, error);
    if (!second) {
        g_ptr_array_free(result, TRUE);
        return NULL;
    }
    for (guint i = 0; i < second->len; i++) {
        const gchar *name = g_ptr_array_index(second, i);
        if (!_ptr_array_has_string(result, name))
            g_ptr_array_add(result, g_strdup(name));
    }
    g_ptr_array_free(second, TRUE);
    return result;
}
















static gint
_open_owned_directory(const gchar *path, GError **error)
{
    struct stat path_stat = {0};
    if (g_lstat(path, &path_stat) != 0) {
        gint saved_errno = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Cannot inspect overlay directory '%s': %s", path,
                    g_strerror(saved_errno));
        return -1;
    }
    gint fd = g_open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);
    struct stat fd_stat = {0};
    if (fd < 0 || fstat(fd, &fd_stat) != 0 ||
        !S_ISDIR(path_stat.st_mode) || !S_ISDIR(fd_stat.st_mode) ||
        path_stat.st_dev != fd_stat.st_dev || path_stat.st_ino != fd_stat.st_ino ||
        fd_stat.st_uid != geteuid() ||
        (fd_stat.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        gint saved_errno = errno ? errno : EPERM;
        if (fd >= 0)
            close(fd);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "Overlay directory '%s' is not an owner-controlled directory: %s",
                    path, g_strerror(saved_errno));
        return -1;
    }
    return fd;
}







static gboolean
_sync_meta_parent(GError **error)
{
    gchar *parent = g_path_get_dirname(_overlay_meta_dir());
    gint parent_fd = g_open(parent,
                           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);
    if (parent_fd < 0) {
        gint saved_errno = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Cannot open overlay metadata parent '%s': %s", parent,
                    g_strerror(saved_errno));
        g_free(parent);
        return FALSE;
    }
    gboolean synced = TRUE;
    if (G_dir_sync_test_hook)
        synced = G_dir_sync_test_hook(error, G_dir_sync_test_hook_data);
    if (synced && fsync(parent_fd) != 0) {
        gint saved_errno = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Cannot sync overlay metadata parent '%s': %s", parent,
                    g_strerror(saved_errno));
        synced = FALSE;
    }
    if (close(parent_fd) != 0 && synced) {
        gint saved_errno = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Cannot close overlay metadata parent '%s': %s", parent,
                    g_strerror(saved_errno));
        synced = FALSE;
    }
    g_free(parent);
    return synced;
}







static gboolean
_meta_directory_stat(struct stat *stat_out, GError **error)
{
    gint fd = _open_owned_directory(_overlay_meta_dir(), error);
    if (fd < 0)
        return FALSE;
    if (fstat(fd, stat_out) != 0) {
        gint saved_errno = errno;
        close(fd);
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Cannot stat overlay metadata directory: %s",
                    g_strerror(saved_errno));
        return FALSE;
    }
    if (close(fd) != 0) {
        gint saved_errno = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Cannot close overlay metadata directory: %s",
                    g_strerror(saved_errno));
        return FALSE;
    }
    return TRUE;
}







static gboolean
_directory_stat_equal(const struct stat *left, const struct stat *right)
{
    return left->st_dev == right->st_dev && left->st_ino == right->st_ino &&
        left->st_mtim.tv_sec == right->st_mtim.tv_sec &&
        left->st_mtim.tv_nsec == right->st_mtim.tv_nsec &&
        left->st_ctim.tv_sec == right->st_ctim.tv_sec &&
        left->st_ctim.tv_nsec == right->st_ctim.tv_nsec;
}







static gboolean
_ensure_meta_dir(GError **error)
{
    const gchar *meta_dir = _overlay_meta_dir();
    struct stat before = {0};
    gboolean existed = g_lstat(meta_dir, &before) == 0;
    if (!existed && errno != ENOENT) {
        gint saved_errno = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Cannot inspect overlay metadata directory '%s': %s",
                    meta_dir, g_strerror(saved_errno));
        return FALSE;
    }
    if (g_mkdir_with_parents(meta_dir, 0700) != 0) {
        gint saved_errno = errno;
        PCV_LOG_WARN(OVERLAY_LOG_DOM, "Cannot create meta dir %s: %s",
                     meta_dir, g_strerror(saved_errno));
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Cannot create overlay metadata directory '%s': %s",
                    meta_dir, g_strerror(saved_errno));
        return FALSE;
    }




    gint dir_fd = _open_owned_directory(meta_dir, error);
    if (dir_fd < 0)
        return FALSE;
    if (close(dir_fd) != 0) {
        gint saved_errno = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Cannot close overlay metadata directory '%s': %s",
                    meta_dir, g_strerror(saved_errno));
        return FALSE;
    }

    if (!existed && !_sync_meta_parent(error))
        return FALSE;
    return TRUE;
}












static gchar *
_serialize_meta(OverlayNet *net, gsize *length_out, GError **error)
{
    if (net->generation < 1 || net->generation > (guint64)G_MAXINT64) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "Overlay metadata generation is outside signed JSON range");
        return NULL;
    }

    JsonObject *obj = json_object_new();
    json_object_set_int_member(obj, "schema_version", OVERLAY_META_SCHEMA_VERSION);
    json_object_set_string_member(obj, "name", net->name);
    json_object_set_int_member(obj, "vni", net->vni);
    json_object_set_string_member(obj, "cidr", net->cidr ? net->cidr : "");
    json_object_set_string_member(obj, "owner_token", net->owner_token);
    json_object_set_int_member(obj, "generation", (gint64)net->generation);


    JsonArray *peers = json_array_new();
    for (guint i = 0; i < net->peers->len; i++)
        json_array_add_string_element(peers, g_ptr_array_index(net->peers, i));
    json_object_set_array_member(obj, "peers", peers);



    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, obj);
    gchar *data = json_to_string(node, FALSE);
    *length_out = strlen(data);



    json_node_free(node);
    json_object_unref(obj);
    if (*length_out > G_meta_max_bytes) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                    "Overlay metadata would exceed the %" G_GSIZE_FORMAT
                    "-byte bounded read contract", G_meta_max_bytes);
        g_free(data);
        return NULL;
    }
    return data;
}




static gboolean
_metadata_peer_add_fits_locked(OverlayNet *net, const gchar *peer_ip,
                               GError **error)
{
    guint64 old_generation = net->generation;
    net->generation++;
    g_ptr_array_add(net->peers, g_strdup(peer_ip));
    gsize projected_length = 0;
    gchar *projected = _serialize_meta(net, &projected_length, error);
    g_ptr_array_remove_index(net->peers, net->peers->len - 1);
    net->generation = old_generation;
    g_free(projected);
    return projected != NULL;
}







static gboolean
_write_meta_exclusive(const gchar *path, const gchar *data, gsize length,
                      gboolean *cleanup_uncertain_out, GError **error)
{
    if (cleanup_uncertain_out)
        *cleanup_uncertain_out = FALSE;
    gint fd = g_open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        gint saved_errno = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Cannot exclusively create metadata '%s': %s", path,
                    g_strerror(saved_errno));
        return FALSE;
    }
    if (G_metadata_stat_test_hook &&
        !G_metadata_stat_test_hook(path, error,
                                   G_metadata_stat_test_hook_data)) {
        close(fd);



        return FALSE;
    }
    struct stat created_stat = {0};
    if (fstat(fd, &created_stat) != 0) {
        gint saved_errno = errno;
        close(fd);
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Cannot stat created metadata '%s': %s", path,
                    g_strerror(saved_errno));
        return FALSE;
    }
    gsize written = 0;
    gboolean ok = TRUE;
    while (written < length) {
        ssize_t n = write(fd, data + written, length - written);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            gint saved_errno = n < 0 ? errno : EIO;
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                        "Cannot write metadata '%s': %s", path,
                        g_strerror(saved_errno));
            ok = FALSE;
            break;
        }
        written += (gsize)n;
    }
    if (ok && fsync(fd) != 0) {
        gint saved_errno = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Cannot sync metadata '%s': %s", path,
                    g_strerror(saved_errno));
        ok = FALSE;
    }







    if (ok && G_metadata_stat_test_hook &&
        !G_metadata_stat_test_hook(path, error,
                                   G_metadata_stat_test_hook_data))
        ok = FALSE;
    struct stat post_before = {0};
    struct stat path_stat = {0};
    if (ok && (fstat(fd, &post_before) != 0 ||
               g_lstat(path, &path_stat) != 0)) {
        gint saved_errno = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Cannot verify committed metadata '%s': %s", path,
                    g_strerror(saved_errno));
        ok = FALSE;
    }
    if (ok && (!S_ISREG(post_before.st_mode) || post_before.st_uid != geteuid() ||
               (post_before.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
               post_before.st_nlink != 1 ||
               post_before.st_size != (off_t)length ||
               post_before.st_dev != created_stat.st_dev ||
               post_before.st_ino != created_stat.st_ino ||
               path_stat.st_dev != created_stat.st_dev ||
               path_stat.st_ino != created_stat.st_ino)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "Metadata inode or pathname changed during commit");
        ok = FALSE;
    }
    gchar *verify = ok ? g_malloc(length ? length : 1) : NULL;
    gsize verified = 0;
    while (ok && verified < length) {
        ssize_t n = pread(fd, verify + verified, length - verified,
                          (off_t)verified);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            gint saved_errno = n < 0 ? errno : EIO;
            g_set_error(error, G_FILE_ERROR,
                        g_file_error_from_errno(saved_errno),
                        "Cannot read back metadata '%s': %s", path,
                        g_strerror(saved_errno));
            ok = FALSE;
            break;
        }
        verified += (gsize)n;
    }
    if (ok && memcmp(verify, data, length) != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "Metadata contents changed during commit");
        ok = FALSE;
    }
    struct stat post_after = {0};
    if (ok && fstat(fd, &post_after) != 0) {
        gint saved_errno = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Cannot revalidate committed metadata '%s': %s", path,
                    g_strerror(saved_errno));
        ok = FALSE;
    }
    if (ok && (post_after.st_dev != post_before.st_dev ||
               post_after.st_ino != post_before.st_ino ||
               post_after.st_mode != post_before.st_mode ||
               post_after.st_uid != post_before.st_uid ||
               post_after.st_nlink != post_before.st_nlink ||
               post_after.st_size != post_before.st_size ||
               post_after.st_mtim.tv_sec != post_before.st_mtim.tv_sec ||
               post_after.st_mtim.tv_nsec != post_before.st_mtim.tv_nsec ||
               post_after.st_ctim.tv_sec != post_before.st_ctim.tv_sec ||
               post_after.st_ctim.tv_nsec != post_before.st_ctim.tv_nsec)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "Metadata inode changed while its contents were verified");
        ok = FALSE;
    }
    g_free(verify);
    if (close(fd) != 0 && ok) {
        gint saved_errno = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Cannot close metadata '%s': %s", path,
                    g_strerror(saved_errno));
        ok = FALSE;
    }
    if (!ok) {
        OverlayMetaSnapshot created = {
            .data = (gchar *)data,
            .len = length,
            .claimed_dev = created_stat.st_dev,
            .claimed_ino = created_stat.st_ino,
        };
        if (!_quarantine_remove_expected(path, &created, NULL)) {
            if (cleanup_uncertain_out)
                *cleanup_uncertain_out = TRUE;
            PCV_LOG_WARN(OVERLAY_LOG_DOM,
                         "failed metadata create retained safe residue: %s", path);
        }
    }
    return ok;
}







static gint
_rename_noreplace(const gchar *source, const gchar *target)
{
#ifdef SYS_renameat2
    return (gint)syscall(SYS_renameat2, AT_FDCWD, source,
                         AT_FDCWD, target, RENAME_NOREPLACE);
#else
    (void)source; (void)target;
    errno = ENOTSUP;
    return -1;
#endif
}







static gboolean
_read_expected_inode_bytes(const gchar *path,
                           const OverlayMetaSnapshot *snapshot)
{
    gint fd = g_open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0);
    if (fd < 0)
        return FALSE;
    struct stat before = {0};
    gboolean exact = fstat(fd, &before) == 0 &&
        S_ISREG(before.st_mode) && before.st_uid == geteuid() &&
        (before.st_mode & (S_IWGRP | S_IWOTH)) == 0 &&
        before.st_nlink >= 1 && before.st_nlink <= 2 &&
        before.st_dev == snapshot->claimed_dev &&
        before.st_ino == snapshot->claimed_ino &&
        before.st_size == (off_t)snapshot->len;
    gchar *data = exact ? g_malloc(snapshot->len ? snapshot->len : 1) : NULL;
    gsize read_len = 0;
    while (exact && read_len < snapshot->len) {
        ssize_t n = read(fd, data + read_len, snapshot->len - read_len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            exact = FALSE;
            break;
        }
        read_len += (gsize)n;
    }
    struct stat after = {0};
    exact = exact && read_len == snapshot->len &&
        memcmp(data, snapshot->data, snapshot->len) == 0 &&
        fstat(fd, &after) == 0 &&
        before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
        before.st_mode == after.st_mode && before.st_uid == after.st_uid &&
        before.st_nlink == after.st_nlink && before.st_size == after.st_size &&
        before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
        before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
        before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
        before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
    g_free(data);
    if (close(fd) != 0)
        exact = FALSE;
    return exact;
}





static gboolean
_quarantine_remove_expected(const gchar *path,
                            const OverlayMetaSnapshot *snapshot,
                            GError **error)
{
    gchar *nonce = g_uuid_string_random();
    gchar *quarantine = g_strdup_printf("%s.quarantine.%s", path, nonce);
    g_free(nonce);
    if (_rename_noreplace(path, quarantine) != 0) {
        gint saved_errno = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Cannot quarantine metadata residue '%s': %s", path,
                    g_strerror(saved_errno));
        g_free(quarantine);
        return FALSE;
    }
    struct stat captured = {0};
    gboolean inode_exact = g_lstat(quarantine, &captured) == 0 &&
        captured.st_dev == snapshot->claimed_dev &&
        captured.st_ino == snapshot->claimed_ino;
    gboolean content_exact = TRUE;
    if (inode_exact && snapshot->data)
        content_exact = _read_expected_inode_bytes(quarantine, snapshot);
    if (!inode_exact || !content_exact) {
        if (_rename_noreplace(quarantine, path) != 0)
            PCV_LOG_WARN(OVERLAY_LOG_DOM,
                         "foreign metadata dentry retained in quarantine: %s",
                         quarantine);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "Metadata dentry changed before cleanup; residue retained");
        g_free(quarantine);
        return FALSE;
    }
    if (g_remove(quarantine) != 0) {
        gint saved_errno = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Cannot remove quarantined metadata '%s': %s", quarantine,
                    g_strerror(saved_errno));
        g_free(quarantine);
        return FALSE;
    }
    gboolean synced = _sync_meta_dir(error);
    g_free(quarantine);
    return synced;
}




static void
_record_metadata_residue_locked(const gchar *reason)
{
    G.restore_state_epoch++;
    G.restore_rejected_count++;
    if (!G.restore_error)
        G.restore_error = g_strdup(reason ? reason
                                          : "overlay metadata transaction residue");
}







static void
_metadata_transaction_failed_locked(const gchar *name, const gchar *reason,
                                    GError **operation_error)
{
    gchar *primary = operation_error && *operation_error
        ? g_strdup((*operation_error)->message)
        : g_strdup("overlay metadata transaction failed");
    if (operation_error) {
        g_clear_error(operation_error);
        g_set_error(operation_error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "%s; persistent transaction residue retained for '%s'",
                    primary, name);
    }
    _record_metadata_residue_locked(reason);
    g_free(primary);
}







static gboolean
_mutation_state_is_clean_locked(GError **error)
{
    if (G.restore_in_progress && !G.restore_accepts_mutation) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "Overlay restore/audit is in progress");
        return FALSE;
    }
    if (G.restore_rejected_count == 0)
        return TRUE;
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Overlay mutation blocked by %u rejected resource(s): %s",
                G.restore_rejected_count,
                G.restore_error ? G.restore_error : "unknown");
    return FALSE;
}

static void
_mutation_committed_locked(void)
{
    G.restore_state_epoch++;
}





static void
_mutation_started_locked(void)
{
    G.restore_state_epoch++;
}

static gboolean
_sync_meta_dir(GError **error)
{
    if (G_dir_sync_test_hook &&
        !G_dir_sync_test_hook(error, G_dir_sync_test_hook_data))
        return FALSE;
    const gchar *dir = _overlay_meta_dir();
    gint fd = _open_owned_directory(dir, error);
    if (fd < 0)
        return FALSE;
    gboolean ok = fsync(fd) == 0;
    gint saved_errno = ok ? 0 : errno;
    if (close(fd) != 0 && ok) {
        ok = FALSE;
        saved_errno = errno;
    }
    if (!ok)
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Cannot sync metadata directory '%s': %s", dir,
                    g_strerror(saved_errno));
    return ok;
}









static gboolean
_restore_claimed_meta(const gchar *claim, const gchar *canonical,
                      const OverlayMetaSnapshot *snapshot)
{
    if (link(claim, canonical) != 0)
        return FALSE;
    if (!_quarantine_remove_expected(claim, snapshot, NULL))
        return FALSE;
    struct stat restored = {0};
    return g_lstat(canonical, &restored) == 0 &&
        S_ISREG(restored.st_mode) && restored.st_nlink == 1 &&
        restored.st_dev == snapshot->claimed_dev &&
        restored.st_ino == snapshot->claimed_ino &&
        _read_expected_inode_bytes(canonical, snapshot);
}







static gboolean
_claim_meta_snapshot(const gchar *canonical, const gchar *claim,
                     OverlayMetaSnapshot *snapshot, GError **error)
{
    struct stat before = {0};
    if (g_lstat(canonical, &before) != 0) {
        gint saved_errno = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Cannot stat canonical metadata '%s': %s", canonical,
                    g_strerror(saved_errno));
        return FALSE;
    }
    if (_rename_noreplace(canonical, claim) != 0) {
        gint saved_errno = errno;
        if (saved_errno == EEXIST)
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                        "Overlay metadata transaction residue exists");
        else
            g_set_error(error, G_FILE_ERROR,
                        g_file_error_from_errno(saved_errno),
                        "Cannot no-clobber claim metadata '%s': %s", canonical,
                        g_strerror(saved_errno));
        return FALSE;
    }
    snapshot->claimed_dev = before.st_dev;
    snapshot->claimed_ino = before.st_ino;
    if (!_sync_meta_dir(error)) {



        _record_metadata_residue_locked(
            "overlay metadata claim durability is uncertain");
        (void)_restore_claimed_meta(claim, canonical, snapshot);
        return FALSE;
    }
    if (_metadata_snapshot_unchanged(claim, snapshot, NULL))
        return TRUE;

    gboolean restored = _restore_claimed_meta(claim, canonical, snapshot);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                "Overlay metadata changed before atomic claim%s",
                restored ? "" : "; claim retained");
    return FALSE;
}

static OverlayMetaReplaceResult
_save_meta(OverlayNet *net, GError **error)
{
    if (!_ensure_meta_dir(error)) {
        _record_metadata_residue_locked(
            "overlay metadata directory ownership/durability check failed");
        return OVERLAY_META_REPLACE_FAILED;
    }
    gsize length = 0;
    gchar *data = _serialize_meta(net, &length, error);
    if (!data)
        return OVERLAY_META_REPLACE_FAILED;
    gchar *path = _overlay_meta_path(net->name);
    OverlayMetaReplaceResult result = OVERLAY_META_REPLACE_FAILED;
    gboolean cleanup_uncertain = FALSE;
    gboolean installed = _write_meta_exclusive(path, data, length,
                                                &cleanup_uncertain, error);
    if (!installed &&
        (cleanup_uncertain ||
         !_metadata_inventory_exact(net->name, FALSE, NULL)))
        _metadata_transaction_failed_locked(
            net->name,
            "overlay metadata create failed with retained canonical residue",
            error);
    if (installed) {
        result = OVERLAY_META_REPLACE_COMMITTED;
        GError *sync_error = NULL;
        if (!_sync_meta_dir(&sync_error)) {
            PCV_LOG_WARN(OVERLAY_LOG_DOM,
                         "metadata committed with directory sync error: %s",
                         sync_error ? sync_error->message : "unknown");
            _record_metadata_residue_locked(
                "overlay metadata commit durability requires inspection");
            if (error && !*error)
                g_propagate_error(error, sync_error);
            else
                g_clear_error(&sync_error);
            result = OVERLAY_META_REPLACE_COMMITTED_DEGRADED;
        }
    }
    g_free(path);
    g_free(data);
    return result;
}

struct OverlayMeta {
    gchar *name;
    gchar *cidr;
    gchar *owner_token;


    gchar *snapshot;
    gsize snapshot_len;
    gchar *path;
    struct stat snapshot_stat;
    gint vni;
    guint64 generation;
    GPtrArray *peers;
    gboolean legacy;
};

static void
_overlay_meta_clear(OverlayMeta *meta)
{
    if (!meta)
        return;
    g_free(meta->name);
    g_free(meta->cidr);
    g_free(meta->owner_token);
    g_free(meta->snapshot);
    g_free(meta->path);
    if (meta->peers)
        g_ptr_array_free(meta->peers, TRUE);
    memset(meta, 0, sizeof(*meta));
}

static gboolean
_json_string_member(JsonObject *obj, const gchar *key, const gchar **value)
{
    JsonNode *node = json_object_get_member(obj, key);
    if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_STRING)
        return FALSE;
    *value = json_node_get_string(node);
    return TRUE;
}

static gboolean
_json_int_member(JsonObject *obj, const gchar *key, gint64 *value)
{
    JsonNode *node = json_object_get_member(obj, key);
    if (!node || !JSON_NODE_HOLDS_VALUE(node))
        return FALSE;
    GType type = json_node_get_value_type(node);
    if (type != G_TYPE_INT64 && type != G_TYPE_INT && type != G_TYPE_LONG &&
        type != G_TYPE_UINT64 && type != G_TYPE_UINT && type != G_TYPE_ULONG)
        return FALSE;
    *value = json_node_get_int(node);
    return TRUE;
}









static gboolean
_overlay_meta_parse(const gchar *data, gssize len, OverlayMeta *meta,
                    GError **error)
{
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, data, len, error)) {
        g_object_unref(parser);
        return FALSE;
    }
    JsonNode *root_node = json_parser_get_root(parser);
    if (!root_node || !JSON_NODE_HOLDS_OBJECT(root_node)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "Overlay metadata root is not an object");
        g_object_unref(parser);
        return FALSE;
    }
    JsonObject *root = json_node_get_object(root_node);
    gboolean has_schema = json_object_has_member(root, "schema_version");
    gboolean has_owner = json_object_has_member(root, "owner_token");
    gboolean has_generation = json_object_has_member(root, "generation");
    gboolean legacy_shape = !has_schema && !has_owner && !has_generation;
    const gchar *legacy_keys[] = {"name", "vni", "cidr", "peers", NULL};
    const gchar *v2_keys[] = {"schema_version", "name", "vni", "cidr",
                              "owner_token", "generation", "peers", NULL};
    const gchar * const *allowed = legacy_shape ? legacy_keys : v2_keys;
    GList *members = json_object_get_members(root);
    for (GList *item = members; item; item = item->next) {
        gboolean known = FALSE;
        for (gint i = 0; allowed[i]; i++)
            known |= g_strcmp0(item->data, allowed[i]) == 0;
        if (!known) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Overlay metadata contains unknown key '%s'",
                        (const gchar *)item->data);
            g_list_free(members);
            g_object_unref(parser);
            return FALSE;
        }
    }
    g_list_free(members);

    const gchar *name = NULL;
    const gchar *cidr = "";
    gint64 vni = 0;
    if (!_json_string_member(root, "name", &name) ||
        !_json_int_member(root, "vni", &vni) ||
        !_json_string_member(root, "cidr", &cidr) ||
        !pcv_overlay_validate_name(name) || !pcv_overlay_validate_vni(vni) ||
        (*cidr && !pcv_overlay_validate_cidr(cidr))) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "Overlay metadata has invalid name, VNI, or CIDR");
        g_object_unref(parser);
        return FALSE;
    }

    const gchar *owner = NULL;
    gint64 schema = 0;
    gint64 generation = 0;
    if (has_schema || has_owner || has_generation) {
        if (!has_schema || !has_owner || !has_generation ||
            !_json_int_member(root, "schema_version", &schema) ||
            !_json_string_member(root, "owner_token", &owner) ||
            !_json_int_member(root, "generation", &generation) ||
            schema != OVERLAY_META_SCHEMA_VERSION || generation < 1 ||
            !g_uuid_string_is_valid(owner)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Overlay metadata ownership schema is invalid");
            g_object_unref(parser);
            return FALSE;
        }
        meta->owner_token = g_strdup(owner);
        meta->generation = (guint64)generation;
    } else {
        meta->legacy = TRUE;
    }

    meta->peers = g_ptr_array_new_with_free_func(g_free);
    JsonNode *peers_node = json_object_get_member(root, "peers");
    if (!peers_node || !JSON_NODE_HOLDS_ARRAY(peers_node)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "Overlay metadata peers is not an array");
        goto fail;
    }
    JsonArray *peers = json_node_get_array(peers_node);
    if (json_array_get_length(peers) > OVERLAY_MAX_PEERS) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                    "Overlay metadata exceeds the %u-peer Single Edge limit",
                    OVERLAY_MAX_PEERS);
        goto fail;
    }
    for (guint i = 0; i < json_array_get_length(peers); i++) {
        JsonNode *peer_node = json_array_get_element(peers, i);
        const gchar *peer = peer_node && JSON_NODE_HOLDS_VALUE(peer_node) &&
            json_node_get_value_type(peer_node) == G_TYPE_STRING
            ? json_node_get_string(peer_node) : NULL;
        if (!pcv_overlay_validate_peer_ip(peer) ||
            g_strcmp0(peer, G.local_ip) == 0) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Overlay metadata contains an invalid/self peer");
            goto fail;
        }
        for (guint j = 0; j < meta->peers->len; j++) {
            if (g_strcmp0(peer, g_ptr_array_index(meta->peers, j)) == 0) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Overlay metadata contains a duplicate peer");
                goto fail;
            }
        }
        g_ptr_array_add(meta->peers, g_strdup(peer));
    }
    meta->name = g_strdup(name);
    meta->cidr = g_strdup(cidr);
    meta->vni = (gint)vni;
    g_object_unref(parser);
    return TRUE;

fail:
    g_object_unref(parser);
    _overlay_meta_clear(meta);
    return FALSE;
}

static void
_overlay_meta_box_free(gpointer data)
{
    OverlayMeta *meta = data;
    if (!meta)
        return;
    _overlay_meta_clear(meta);
    g_free(meta);
}











static GPtrArray *
_metadata_directory_preflight(GError **error)
{
    GDir *dir = g_dir_open(_overlay_meta_dir(), 0, error);
    if (!dir)
        return NULL;
    GPtrArray *metas = g_ptr_array_new_with_free_func(_overlay_meta_box_free);
    const gchar *filename = NULL;
    gboolean valid = TRUE;
    while ((filename = g_dir_read_name(dir)) != NULL) {
        if (!_restore_deadline_check(error)) {
            valid = FALSE;
            break;
        }
        if (!g_str_has_prefix(filename, "overlay-") ||
            !g_str_has_suffix(filename, ".meta") ||
            strstr(filename, ".meta.") != NULL) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Overlay metadata directory contains non-canonical dentry '%s'",
                        filename);
            valid = FALSE;
            break;
        }
        gchar *path = g_build_filename(_overlay_meta_dir(), filename, NULL);
        gchar *snapshot = NULL;
        gsize snapshot_len = 0;
        OverlayMeta *meta = g_new0(OverlayMeta, 1);
        struct stat snapshot_stat = {0};
        valid = _safe_read_metadata_full(path, &snapshot, &snapshot_len,
                                         &snapshot_stat, error) &&
                _overlay_meta_parse(snapshot, (gssize)snapshot_len, meta,
                                    error);
        gchar *canonical = valid
            ? g_strdup_printf("overlay-%s.meta", meta->name) : NULL;
        valid = valid && g_strcmp0(canonical, filename) == 0;
        if (!valid && (!error || !*error))
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Overlay metadata filename/name mismatch: %s", filename);
        for (guint i = 0; valid && i < metas->len; i++) {
            OverlayMeta *other = g_ptr_array_index(metas, i);
            valid = g_strcmp0(other->name, meta->name) != 0 &&
                    other->vni != meta->vni &&
                    (!other->owner_token || !meta->owner_token ||
                     g_strcmp0(other->owner_token, meta->owner_token) != 0);
            if (!valid)
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                            "Overlay metadata has duplicate name, VNI, or owner token");
        }
        g_free(canonical);
        if (!valid) {
            g_free(snapshot);
            g_free(path);
            _overlay_meta_box_free(meta);
            break;
        }
        meta->snapshot = snapshot;
        meta->snapshot_len = snapshot_len;
        meta->path = path;
        meta->snapshot_stat = snapshot_stat;
        g_ptr_array_add(metas, meta);
    }
    g_dir_close(dir);
    if (!valid) {
        g_ptr_array_free(metas, TRUE);
        return NULL;
    }
    return metas;
}







static gboolean
_metadata_preflight_snapshots_unchanged(GPtrArray *metas, GError **error)
{
    for (guint i = 0; i < metas->len; i++) {
        if (!_restore_deadline_check(error))
            return FALSE;
        OverlayMeta *meta = g_ptr_array_index(metas, i);
        gchar *current = NULL;
        gsize current_len = 0;
        struct stat current_stat = {0};
        if (!_safe_read_metadata_full(meta->path, &current, &current_len,
                                      &current_stat, error)) {
            g_free(current);
            return FALSE;
        }
        gboolean exact = current_len == meta->snapshot_len &&
            current_stat.st_dev == meta->snapshot_stat.st_dev &&
            current_stat.st_ino == meta->snapshot_stat.st_ino &&
            current_stat.st_mode == meta->snapshot_stat.st_mode &&
            current_stat.st_uid == meta->snapshot_stat.st_uid &&
            current_stat.st_nlink == meta->snapshot_stat.st_nlink &&
            current_stat.st_size == meta->snapshot_stat.st_size &&
            memcmp(current, meta->snapshot, current_len) == 0;
        g_free(current);
        if (!exact) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                        "Overlay metadata changed after restore preflight");
            return FALSE;
        }
    }
    return TRUE;
}













void
pcv_overlay_init(const gchar *local_tunnel_ip)
{


    if (!local_tunnel_ip || !*local_tunnel_ip) {
        PCV_LOG_INFO(OVERLAY_LOG_DOM, "No tunnel IP configured — overlay disabled");
        return;
    }
    if (!pcv_overlay_validate_peer_ip(local_tunnel_ip)) {
        PCV_LOG_WARN(OVERLAY_LOG_DOM,
                     "Invalid IPv4 tunnel_ip '%s' — overlay disabled",
                     local_tunnel_ip);
        return;
    }
    g_mutex_lock(&g_overlay_reconcile_mu);
    if (G.initialized) {
        g_mutex_unlock(&g_overlay_reconcile_mu);
        PCV_LOG_WARN(OVERLAY_LOG_DOM, "Overlay manager is already initialized");
        return;
    }
    g_mutex_unlock(&g_overlay_reconcile_mu);

    g_mutex_init(&G.mu);
    G.local_ip = g_strdup(local_tunnel_ip);
    G.count = 0;
    g_clear_pointer(&G.restore_error, g_free);
    G.restore_rejected_count = 0;
    G.restore_in_progress = FALSE;
    g_mutex_lock(&g_overlay_reconcile_mu);
    g_overlay_lifecycle_stopping = FALSE;
    g_overlay_reconcile_stopping = FALSE;
    G.initialized = TRUE;
    g_mutex_unlock(&g_overlay_reconcile_mu);
    PCV_LOG_INFO(OVERLAY_LOG_DOM, "Overlay manager initialized (tunnel_ip=%s)", local_tunnel_ip);
}









void
pcv_overlay_shutdown(void)
{
    g_mutex_lock(&g_overlay_reconcile_mu);
    if (!G.initialized || g_overlay_lifecycle_stopping) {
        g_mutex_unlock(&g_overlay_reconcile_mu);
        return;
    }

    g_overlay_lifecycle_stopping = TRUE;
    g_mutex_unlock(&g_overlay_reconcile_mu);



    pcv_overlay_reconcile_timer_shutdown();
    g_mutex_lock(&g_overlay_reconcile_mu);
    while (g_overlay_public_inflight > 0)
        g_cond_wait(&g_overlay_reconcile_cond, &g_overlay_reconcile_mu);
    g_mutex_unlock(&g_overlay_reconcile_mu);

    g_mutex_lock(&G.mu);

    for (gint i = 0; i < G.count; i++) {
        g_free(G.nets[i].name);
        g_free(G.nets[i].cidr);
        g_free(G.nets[i].owner_token);
        g_ptr_array_free(G.nets[i].peers, TRUE);
    }
    G.count = 0;
    g_clear_pointer(&G.restore_error, g_free);
    G.restore_rejected_count = 0;
    G.restore_in_progress = FALSE;
    g_free(G.local_ip);
    G.local_ip = NULL;
    g_mutex_unlock(&G.mu);

    g_mutex_clear(&G.mu);
    g_mutex_lock(&g_overlay_reconcile_mu);
    G.initialized = FALSE;
    g_mutex_unlock(&g_overlay_reconcile_mu);
}



static gboolean _restore_one_snapshot(OverlayMeta *meta, const gchar *path,
                                      const gchar *snapshot, gsize snapshot_len,
                                      guint64 admission_epoch,
                                      gboolean *applied_out, GError **error);

static gboolean
_completed_delete_locked(const gchar *name)
{
    gchar *meta = _overlay_meta_path(name);
    gchar *tomb = _overlay_tombstone_path(name);
    gchar *update = _overlay_update_path(name);
    gboolean completed = !_find(name) &&
        !_path_entry_exists(meta) &&
        !_path_entry_exists(tomb) &&
        !_path_entry_exists(update) &&
        _metadata_inventory_exact(name, FALSE, NULL);
    g_free(meta); g_free(tomb); g_free(update);
    return completed;
}

static gboolean
_scan_enoent_is_completed_delete(const gchar *filename)
{
    gsize len = strlen(filename);
    if (len <= strlen("overlay-") + strlen(".meta"))
        return FALSE;
    gchar *name = g_strndup(filename + strlen("overlay-"),
                            len - strlen("overlay-") - strlen(".meta"));
    if (!pcv_overlay_validate_name(name)) {
        g_free(name);
        return FALSE;
    }
    g_mutex_lock(&G.mu);
    gboolean completed = _completed_delete_locked(name);
    g_mutex_unlock(&G.mu);
    g_free(name);
    return completed;
}

static gboolean
_overlay_restore_enter(void)
{
    gboolean entered = FALSE;
    g_mutex_lock(&g_overlay_reconcile_mu);
    if (G.initialized && !g_overlay_lifecycle_stopping &&
        !g_overlay_reconcile_stopping && g_overlay_reconcile_inflight == 0) {
        g_overlay_reconcile_inflight = 1;
        entered = TRUE;
    }
    g_mutex_unlock(&g_overlay_reconcile_mu);
    return entered;
}

static void
_overlay_restore_leave(void)
{
    g_mutex_lock(&g_overlay_reconcile_mu);
    g_assert(g_overlay_reconcile_inflight == 1);
    g_overlay_reconcile_inflight = 0;
    g_cond_broadcast(&g_overlay_reconcile_cond);
    g_mutex_unlock(&g_overlay_reconcile_mu);
}

static gboolean
_overlay_restore_failure_update(guint rejected, const gchar *reason,
                                guint64 admission_epoch)
{
    gboolean applied = FALSE;
    g_mutex_lock(&G.mu);
    if (G.restore_state_epoch == admission_epoch) {
        g_free(G.restore_error);
        G.restore_error = rejected
            ? g_strdup(reason ? reason : "overlay metadata restore rejected") : NULL;
        G.restore_rejected_count = rejected;
        G.restore_in_progress = FALSE;
        G.restore_accepts_mutation = FALSE;
        G.restore_state_epoch++;
        applied = TRUE;
    }
    g_mutex_unlock(&G.mu);
    return applied;
}







static void
_overlay_restore_scan_impl(void)
{
    if (!G.initialized)
        return;

    guint scan_attempt = 0;
retry_scan:
    _restore_bulk_replace(NULL);
    g_mutex_lock(&G.mu);
    G.restore_in_progress = TRUE;
    G.restore_accepts_mutation = FALSE;
    guint64 admission_epoch = G.restore_state_epoch;
    g_mutex_unlock(&G.mu);

    GError *scan_error = NULL;
    if (!_ensure_meta_dir(&scan_error)) {
        (void)_overlay_restore_failure_update(1,
            scan_error ? scan_error->message : "overlay metadata directory unavailable",
            admission_epoch);
        g_clear_error(&scan_error);
        return;
    }
    if (!_sync_meta_parent(&scan_error)) {
        (void)_overlay_restore_failure_update(1,
            scan_error ? scan_error->message
                       : "overlay metadata parent directory sync failed",
            admission_epoch);
        g_clear_error(&scan_error);
        return;
    }
    if (!_metadata_directory_cardinality_bounded(&scan_error)) {
        (void)_overlay_restore_failure_update(
            1, scan_error ? scan_error->message
                          : "overlay metadata cardinality exceeds bound",
            admission_epoch);
        g_clear_error(&scan_error);
        return;
    }
    struct stat scan_directory_before = {0};
    if (!_meta_directory_stat(&scan_directory_before, &scan_error)) {
        (void)_overlay_restore_failure_update(1,
            scan_error ? scan_error->message
                       : "overlay metadata directory stat failed",
            admission_epoch);
        g_clear_error(&scan_error);
        return;
    }
    _restore_bulk_replace(_bulk_snapshot_load(&scan_error));
    if (!_restore_bulk_get()) {
        (void)_overlay_restore_failure_update(
            1, scan_error ? scan_error->message
                          : "overlay OVSDB snapshot unavailable",
            admission_epoch);
        g_clear_error(&scan_error);
        return;
    }





    GPtrArray *preflight_metas = _metadata_directory_preflight(&scan_error);
    gboolean preflight_clean = preflight_metas != NULL;
    if (preflight_clean) {
        g_mutex_lock(&G.mu);
        preflight_clean = _bulk_preflight_metadata_exact(
            _restore_bulk_get(), preflight_metas, &scan_error);
        g_mutex_unlock(&G.mu);
    }
    if (preflight_clean)
        preflight_clean = _metadata_preflight_snapshots_unchanged(
            preflight_metas, &scan_error);
    struct stat preflight_directory_after = {0};
    if (preflight_clean)
        preflight_clean = _meta_directory_stat(&preflight_directory_after,
                                               &scan_error) &&
            _directory_stat_equal(&scan_directory_before,
                                  &preflight_directory_after);
    if (!preflight_clean) {
        (void)_overlay_restore_failure_update(
            1, scan_error ? scan_error->message
                          : "overlay restore global preflight failed",
            admission_epoch);
        g_clear_error(&scan_error);
        if (preflight_metas)
            g_ptr_array_free(preflight_metas, TRUE);
        return;
    }
    g_ptr_array_free(preflight_metas, TRUE);

    const gchar *meta_dir = _overlay_meta_dir();
    GError *dir_error = NULL;
    GDir *dir = g_dir_open(meta_dir, 0, &dir_error);
    if (!dir) {
        PCV_LOG_WARN(OVERLAY_LOG_DOM, "overlay metadata scan failed: %s",
                     dir_error ? dir_error->message : "unknown");
        (void)_overlay_restore_failure_update(1,
            dir_error ? dir_error->message : "overlay metadata scan failed",
            admission_epoch);
        g_clear_error(&dir_error);
        return;
    }





    g_mutex_lock(&G.mu);
    G.restore_accepts_mutation = TRUE;
    g_mutex_unlock(&G.mu);
    guint restored = 0;
    guint rejected = 0;
    gchar *first_failure = NULL;
    const gchar *filename = NULL;
    while ((filename = g_dir_read_name(dir)) != NULL) {
        GError *deadline_error = NULL;
        if (!_restore_deadline_check(&deadline_error)) {
            rejected++;
            if (!first_failure)
                first_failure = g_strdup(deadline_error->message);
            g_clear_error(&deadline_error);
            break;
        }
        if (!g_str_has_prefix(filename, "overlay-"))
            continue;





        if (strstr(filename, ".meta.") != NULL) {
            gchar *tombstone = g_build_filename(meta_dir, filename, NULL);
            g_mutex_lock(&G.mu);
            gboolean remains = _path_entry_exists(tombstone);
            g_mutex_unlock(&G.mu);
            if (remains) {
                PCV_LOG_WARN(OVERLAY_LOG_DOM,
                             "overlay metadata transaction residue: %s", tombstone);
                rejected++;
                if (!first_failure)
                    first_failure = g_strdup(
                        "overlay metadata transaction residue requires recovery");
            }
            g_free(tombstone);
            continue;
        }
        if (!g_str_has_suffix(filename, ".meta"))
            continue;

        gchar *path = g_build_filename(meta_dir, filename, NULL);
        gchar *snapshot = NULL;
        gsize snapshot_len = 0;
        GError *error = NULL;
        OverlayMeta meta = {0};
        gboolean loaded = _safe_read_metadata(path, &snapshot, &snapshot_len,
                                              &error);
        if (!loaded && g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT) &&
            _scan_enoent_is_completed_delete(filename)) {
            g_clear_error(&error);
            g_free(snapshot);
            g_free(path);
            continue;
        }
        if (!loaded ||
            !_overlay_meta_parse(snapshot, (gssize)snapshot_len, &meta, &error)) {
            PCV_LOG_WARN(OVERLAY_LOG_DOM, "overlay metadata rejected (%s): %s",
                         path, error ? error->message : "unknown");
            rejected++;
            if (!first_failure)
                first_failure = g_strdup(error ? error->message
                                               : "overlay metadata parse failed");
            g_clear_error(&error);
            g_free(snapshot);
            g_free(path);
            continue;
        }

        gchar *canonical = g_strdup_printf("overlay-%s.meta", meta.name);
        if (g_strcmp0(canonical, filename) != 0) {
            PCV_LOG_WARN(OVERLAY_LOG_DOM,
                         "overlay metadata filename/name mismatch: %s", path);
            rejected++;
            if (!first_failure)
                first_failure = g_strdup("overlay metadata filename/name mismatch");
        } else {
            if (G_restore_snapshot_hook)
                G_restore_snapshot_hook(path, G_restore_snapshot_hook_data);
            gboolean applied = FALSE;
            if (_restore_one_snapshot(&meta, path, snapshot, snapshot_len,
                                      admission_epoch, &applied, &error)) {
                restored += applied;
            } else {
                PCV_LOG_WARN(OVERLAY_LOG_DOM, "overlay '%s' restore rejected: %s",
                             meta.name, error ? error->message : "unknown");
                rejected++;
                if (!first_failure)
                    first_failure = g_strdup(error ? error->message
                                                   : "overlay actual restore failed");
            }
        }
        g_clear_error(&error);
        g_free(canonical);
        _overlay_meta_clear(&meta);
        g_free(snapshot);
        g_free(path);
    }
    g_dir_close(dir);
    g_mutex_lock(&G.mu);
    G.restore_accepts_mutation = FALSE;
    g_mutex_unlock(&G.mu);





    if (restored > 0) {
        GError *rebase_error = NULL;
        if (!_meta_directory_stat(&scan_directory_before, &rebase_error)) {
            rejected++;
            if (!first_failure)
                first_failure = g_strdup(
                    rebase_error ? rebase_error->message
                                 : "overlay metadata directory rebase failed");
        }
        g_clear_error(&rebase_error);
    }



    _restore_bulk_replace(NULL);
    GError *actual_audit_error = NULL;
    _restore_bulk_replace(_bulk_snapshot_load(&actual_audit_error));
    g_mutex_lock(&G.mu);
    gboolean actual_inventory_clean = _restore_bulk_get() &&
        _audit_registry_metadata_locked(&actual_audit_error) &&
        _audit_all_owned_actual_locked(&actual_audit_error);
    g_mutex_unlock(&G.mu);
    if (!actual_inventory_clean) {
        rejected++;
        if (!first_failure)
            first_failure = g_strdup(
                actual_audit_error ? actual_audit_error->message
                                   : "overlay owned actual inventory audit failed");
        g_clear_error(&actual_audit_error);
    }
    GError *durability_error = NULL;
    gboolean durability_clean = _sync_meta_dir(&durability_error);
    if (!durability_clean) {
        rejected++;
        if (!first_failure)
            first_failure = g_strdup(
                durability_error ? durability_error->message
                                 : "overlay metadata directory sync failed");
        g_clear_error(&durability_error);
    }
    if (actual_inventory_clean && durability_clean) {
        GError *final_audit_error = NULL;
        g_mutex_lock(&G.mu);
        gboolean final_inventory_clean =
            _audit_registry_metadata_locked(&final_audit_error) &&
            _audit_all_owned_actual_locked(&final_audit_error);
        g_mutex_unlock(&G.mu);
        if (!final_inventory_clean) {
            rejected++;
            if (!first_failure)
                first_failure = g_strdup(
                    final_audit_error ? final_audit_error->message
                                      : "overlay metadata final audit failed");
        }
        g_clear_error(&final_audit_error);
    }
    struct stat scan_directory_after = {0};
    GError *directory_fence_error = NULL;
    if (!_meta_directory_stat(&scan_directory_after, &directory_fence_error) ||
        !_directory_stat_equal(&scan_directory_before, &scan_directory_after)) {
        rejected++;
        if (!first_failure)
            first_failure = g_strdup(
                directory_fence_error ? directory_fence_error->message
                                      : "overlay metadata directory changed during scan");
    }
    g_clear_error(&directory_fence_error);
    gboolean scan_applied = _overlay_restore_failure_update(
        rejected, first_failure, admission_epoch);
    g_free(first_failure);
    if (!scan_applied) {




        if (++scan_attempt < 3)
            goto retry_scan;
        g_mutex_lock(&G.mu);
        _record_metadata_residue_locked(
            "overlay metadata scan repeatedly invalidated by concurrent mutation");
        G.restore_in_progress = FALSE;
        g_mutex_unlock(&G.mu);
        return;
    }
    if (restored)
        PCV_LOG_INFO(OVERLAY_LOG_DOM, "Restored %u overlay network(s)", restored);
}







static void
_overlay_restore_impl(void)
{
    gint64 now = g_get_monotonic_time();
    gint64 budget = G_restore_deadline_test_usec > 0
        ? G_restore_deadline_test_usec : OVERLAY_RESTORE_DEADLINE_USEC;




    gint64 internal_budget = G_restore_deadline_test_usec > 0
        ? budget : budget - OVERLAY_RESTORE_LAUNCHER_MARGIN_USEC;
    gint64 reserve = G_restore_deadline_test_usec > 0
        ? MAX((gint64)1000, budget / 2)
        : (gint64)OVERLAY_RESTORE_CLEANUP_RESERVE_USEC;
    gint64 *deadline = g_new(gint64, 1);
    gint64 *hard_deadline = g_new(gint64, 1);
    *hard_deadline = now + internal_budget;
    *deadline = *hard_deadline - reserve;
    g_private_replace(&G_restore_deadline, deadline);
    g_private_replace(&G_restore_hard_deadline, hard_deadline);
    _overlay_restore_scan_impl();
    _restore_bulk_replace(NULL);
    g_private_replace(&G_restore_deadline, NULL);
    g_private_replace(&G_restore_hard_deadline, NULL);
}

void
pcv_overlay_restore(void)
{
    if (!_overlay_restore_enter())
        return;
    _overlay_restore_impl();
    _overlay_restore_leave();
}



void
pcv_overlay_reconcile(void)
{


    pcv_overlay_restore();
}

static void
_overlay_reconcile_worker(GTask *task, gpointer source, gpointer task_data,
                          GCancellable *cancellable)
{
    (void)source; (void)task_data; (void)cancellable;

    _overlay_restore_impl();
    _overlay_restore_leave();
    g_task_return_boolean(task, TRUE);
}

static gboolean
_overlay_reconcile_tick(gpointer data)
{
    (void)data;
    if (!_overlay_restore_enter())
        return G_SOURCE_CONTINUE;
    GTask *task = pcv_drain_task_new(NULL, NULL, NULL, NULL);
    pcv_worker_pool_push(task, _overlay_reconcile_worker);
    g_object_unref(task);
    return G_SOURCE_CONTINUE;
}

void
pcv_overlay_reconcile_timer_init(void)
{
    gint interval = pcv_config_get_int("overlay", "reconcile_interval_sec", 300);
    if (interval <= 0)
        return;

    g_mutex_lock(&g_overlay_reconcile_mu);
    if (G.initialized && !g_overlay_lifecycle_stopping &&
        !g_overlay_reconcile_stopping && g_overlay_reconcile_timer_id == 0)
        g_overlay_reconcile_timer_id =
            g_timeout_add_seconds((guint)interval, _overlay_reconcile_tick, NULL);
    g_mutex_unlock(&g_overlay_reconcile_mu);
}

void
pcv_overlay_reconcile_timer_shutdown(void)
{
    guint timer_id = 0;
    g_mutex_lock(&g_overlay_reconcile_mu);
    g_overlay_reconcile_stopping = TRUE;
    timer_id = g_overlay_reconcile_timer_id;
    g_overlay_reconcile_timer_id = 0;
    g_mutex_unlock(&g_overlay_reconcile_mu);
    if (timer_id)
        g_source_remove(timer_id);

    g_mutex_lock(&g_overlay_reconcile_mu);
    while (g_overlay_reconcile_inflight > 0)
        g_cond_wait(&g_overlay_reconcile_cond, &g_overlay_reconcile_mu);
    g_mutex_unlock(&g_overlay_reconcile_mu);
}












static gchar *
_ovs_scalar_normalize(gchar *value)
{
    if (!value)
        return g_strdup("");
    gchar *trimmed = g_strstrip(value);
    if (g_strcmp0(trimmed, "[]") == 0)
        return g_strdup("");
    gsize len = strlen(trimmed);
    if (len >= 2 && trimmed[0] == '"' && trimmed[len - 1] == '"')
        return g_strndup(trimmed + 1, len - 2);
    return g_strdup(trimmed);
}

static gboolean
_ovs_get_field(const gchar *table, const gchar *name, const gchar *column,
               gboolean *exists_out, gchar **value_out, GError **error)
{
    gboolean exists = FALSE;
    *exists_out = FALSE;
    *value_out = NULL;
    if (!_ovs_row_exists(table, "name", name, &exists, error))
        return FALSE;
    if (!exists)
        return TRUE;

    const gchar *argv[] = {
        "ovs-vsctl", "--bare", "--if-exists", "get", table, name, column, NULL
    };
    gchar *raw = NULL;
    if (!_run_argv_capture(argv, &raw, error)) {
        g_free(raw);
        return FALSE;
    }
    *exists_out = TRUE;
    *value_out = _ovs_scalar_normalize(raw);
    g_free(raw);
    return TRUE;
}

static gboolean
_ovs_owner_probe(const gchar *table, const gchar *name, gboolean *exists_out,
                 gchar **owner_out, GError **error)
{
    gchar *column = g_strdup_printf("external_ids:%s", OVS_OWNER_KEY);
    gboolean ok = _ovs_get_field(table, name, column, exists_out, owner_out, error);
    g_free(column);
    return ok;
}

static gboolean
_bridge_is_owned(const gchar *name, const gchar *owner_token,
                 gboolean *exists_out, GError **error)
{
    gchar *owner = NULL;
    gchar *overlay_name = NULL;
    gboolean exists = FALSE;
    if (!_ovs_owner_probe("Bridge", name, &exists, &owner, error))
        return FALSE;
    if (exists) {
        gchar *name_column = g_strdup_printf("external_ids:%s", OVS_NAME_KEY);
        gboolean name_exists = FALSE;
        if (!_ovs_get_field("Bridge", name, name_column, &name_exists,
                            &overlay_name, error)) {
            g_free(name_column);
            g_free(owner);
            return FALSE;
        }
        g_free(name_column);
        if (!name_exists)
            exists = FALSE;
    }
    *exists_out = exists;
    gboolean owned = !exists ||
        (g_strcmp0(owner, owner_token) == 0 &&
         g_strcmp0(overlay_name, name) == 0);
    if (exists && !owned)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "OVS bridge '%s' is not owned by this overlay", name);
    g_free(owner);
    g_free(overlay_name);
    return owned;
}

static gboolean
_interface_is_owned(OverlayNet *net, const gchar *port_name,
                    gboolean *exists_out, GError **error)
{
    gchar *owner = NULL;
    gchar *overlay_name = NULL;
    gboolean exists = FALSE;
    if (!_ovs_owner_probe("Interface", port_name, &exists, &owner, error))
        return FALSE;
    *exists_out = exists;
    if (!exists) {
        g_free(owner);
        return TRUE;
    }
    gchar *name_column = g_strdup_printf("external_ids:%s", OVS_NAME_KEY);
    gboolean name_exists = FALSE;
    if (!_ovs_get_field("Interface", port_name, name_column, &name_exists,
                        &overlay_name, error)) {
        g_free(name_column); g_free(owner);
        return FALSE;
    }
    g_free(name_column);
    const gchar *bridge_argv[] = {
        "ovs-vsctl", "--bare", "iface-to-br", port_name, NULL
    };
    gchar *bridge_raw = NULL;
    if (!_run_argv_capture(bridge_argv, &bridge_raw, error)) {
        g_free(owner); g_free(overlay_name); g_free(bridge_raw);
        return FALSE;
    }
    gchar *bridge = _ovs_scalar_normalize(bridge_raw);
    gboolean owned = name_exists &&
        g_strcmp0(owner, net->owner_token) == 0 &&
        g_strcmp0(overlay_name, net->name) == 0 &&
        g_strcmp0(bridge, net->name) == 0;
    if (!owned)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "OVS interface '%s' is not owned by overlay '%s'",
                    port_name, net->name);
    g_free(bridge_raw); g_free(bridge); g_free(owner); g_free(overlay_name);
    return owned;
}

static gboolean
_legacy_bridge_matches(const gchar *name, const gchar *cidr, GError **error)
{
    const gchar *argv[] = {"ip", "-o", "-4", "addr", "show", "dev", name, NULL};
    gchar *output = NULL;
    if (!_run_argv_capture(argv, &output, error)) {
        g_free(output);
        return FALSE;
    }
    guint address_count = 0;
    gboolean desired_found = FALSE;
    gchar **lines = g_strsplit(output ? output : "", "\n", -1);
    for (gint i = 0; lines[i]; i++) {
        gchar *line = g_strstrip(lines[i]);
        if (!*line)
            continue;
        gchar **tokens = g_strsplit_set(line, " \t", -1);
        for (gint j = 0; tokens[j]; j++) {
            if (!*tokens[j] || g_strcmp0(tokens[j], "inet") != 0)
                continue;
            gint next = j + 1;
            while (tokens[next] && !*tokens[next])
                next++;
            if (tokens[next]) {
                address_count++;
                desired_found |= cidr && *cidr &&
                                 g_strcmp0(tokens[next], cidr) == 0;
            }
            break;
        }
        g_strfreev(tokens);
    }
    g_strfreev(lines);
    gboolean match = (!cidr || !*cidr)
        ? address_count == 0 : address_count == 1 && desired_found;
    if (!match)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "Legacy bridge '%s' IPv4 address set does not exactly match metadata",
                    name);
    g_free(output);
    return match;
}

static gchar *
_expected_options_map(OverlayNet *net, const gchar *peer_ip)
{
    return g_strdup_printf("{key=%d,remote_ip=%s,local_ip=%s}",
                           net->vni, peer_ip, G.local_ip);
}






#define OVERLAY_BULK_MAX_BYTES (16U * 1024U * 1024U)
#define OVERLAY_BULK_MAX_ROWS 65536U

static void
_bulk_string_array_free(gpointer data)
{
    if (data)
        g_ptr_array_free(data, TRUE);
}

static void
_bulk_bridge_free(gpointer data)
{
    OverlayBulkBridge *row = data;
    if (!row) return;
    g_free(row->uuid); g_free(row->name); g_free(row->owner);
    g_free(row->overlay_name); g_free(row->datapath_type);
    if (row->ports) g_ptr_array_free(row->ports, TRUE);
    g_free(row);
}

static void
_bulk_port_free(gpointer data)
{
    OverlayBulkPort *row = data;
    if (!row) return;
    g_free(row->uuid); g_free(row->name); g_free(row->owner);
    g_free(row->overlay_name);
    if (row->interfaces) g_ptr_array_free(row->interfaces, TRUE);
    g_free(row);
}

static void
_bulk_interface_free(gpointer data)
{
    OverlayBulkInterface *row = data;
    if (!row) return;
    g_free(row->uuid); g_free(row->name); g_free(row->owner);
    g_free(row->overlay_name); g_free(row->type);
    if (row->options) g_hash_table_destroy(row->options);
    g_free(row);
}

static void
_bulk_snapshot_free(OverlayBulkSnapshot *snapshot)
{
    if (!snapshot) return;
    g_hash_table_destroy(snapshot->bridges_by_uuid);
    g_hash_table_destroy(snapshot->ports_by_uuid);
    g_hash_table_destroy(snapshot->interfaces_by_uuid);
    g_hash_table_destroy(snapshot->interface_ref_counts);
    g_hash_table_destroy(snapshot->port_parent_counts);
    g_hash_table_destroy(snapshot->bridges_by_name);
    g_hash_table_destroy(snapshot->ports_by_name);
    g_hash_table_destroy(snapshot->interfaces_by_name);
    g_hash_table_destroy(snapshot->ipv4_by_name);
    g_free(snapshot);
}

static void
_bulk_ref_increment(GHashTable *counts, const gchar *uuid)
{
    guint count = GPOINTER_TO_UINT(g_hash_table_lookup(counts, uuid));
    g_hash_table_replace(counts, g_strdup(uuid), GUINT_TO_POINTER(count + 1));
}







static gboolean
_bulk_validate_relationships(OverlayBulkSnapshot *snapshot, GError **error)
{
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, snapshot->ports_by_name);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        if (!_restore_deadline_check(error))
            return FALSE;
        OverlayBulkPort *port = value;
        for (guint i = 0; i < port->interfaces->len; i++) {
            const gchar *uuid = g_ptr_array_index(port->interfaces, i);
            if (!g_hash_table_contains(snapshot->interfaces_by_uuid, uuid))
                goto dangling;
            _bulk_ref_increment(snapshot->interface_ref_counts, uuid);
        }
    }
    g_hash_table_iter_init(&iter, snapshot->bridges_by_name);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        if (!_restore_deadline_check(error))
            return FALSE;
        OverlayBulkBridge *bridge = value;
        for (guint i = 0; i < bridge->ports->len; i++) {
            const gchar *uuid = g_ptr_array_index(bridge->ports, i);
            if (!g_hash_table_contains(snapshot->ports_by_uuid, uuid))
                goto dangling;
            _bulk_ref_increment(snapshot->port_parent_counts, uuid);
        }
    }
    return TRUE;

dangling:
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "OVSDB snapshot contains a dangling Port/Interface UUID");
    return FALSE;
}

static const gchar *
_bulk_string(JsonNode *node)
{
    return node && JSON_NODE_HOLDS_VALUE(node) &&
        json_node_get_value_type(node) == G_TYPE_STRING
        ? json_node_get_string(node) : NULL;
}

static gboolean
_bulk_uuid(JsonNode *node, gchar **uuid_out)
{
    if (!node || !JSON_NODE_HOLDS_ARRAY(node)) return FALSE;
    JsonArray *tag = json_node_get_array(node);
    if (json_array_get_length(tag) != 2 ||
        g_strcmp0(_bulk_string(json_array_get_element(tag, 0)), "uuid") != 0)
        return FALSE;
    const gchar *uuid = _bulk_string(json_array_get_element(tag, 1));
    if (!uuid || !g_uuid_string_is_valid(uuid)) return FALSE;
    *uuid_out = g_strdup(uuid);
    return TRUE;
}







static gboolean
_bulk_uuid_set(JsonNode *node, GPtrArray **set_out)
{
    GPtrArray *set = g_ptr_array_new_with_free_func(g_free);
    gchar *single = NULL;
    if (_bulk_uuid(node, &single)) {
        g_ptr_array_add(set, single);
        *set_out = set;
        return TRUE;
    }
    if (!node || !JSON_NODE_HOLDS_ARRAY(node)) goto fail;
    JsonArray *tag = json_node_get_array(node);
    if (json_array_get_length(tag) != 2 ||
        g_strcmp0(_bulk_string(json_array_get_element(tag, 0)), "set") != 0)
        goto fail;
    JsonNode *members_node = json_array_get_element(tag, 1);
    if (!members_node || !JSON_NODE_HOLDS_ARRAY(members_node)) goto fail;
    JsonArray *members = json_node_get_array(members_node);
    for (guint i = 0; i < json_array_get_length(members); i++) {
        gchar *uuid = NULL;
        if (!_bulk_uuid(json_array_get_element(members, i), &uuid) ||
            _ptr_array_has_string(set, uuid)) {
            g_free(uuid);
            goto fail;
        }
        g_ptr_array_add(set, uuid);
    }
    *set_out = set;
    return TRUE;
fail:
    g_ptr_array_free(set, TRUE);
    return FALSE;
}







static GHashTable *
_bulk_string_map(JsonNode *node)
{
    if (!node || !JSON_NODE_HOLDS_ARRAY(node)) return NULL;
    JsonArray *tag = json_node_get_array(node);
    if (json_array_get_length(tag) != 2 ||
        g_strcmp0(_bulk_string(json_array_get_element(tag, 0)), "map") != 0)
        return NULL;
    JsonNode *pairs_node = json_array_get_element(tag, 1);
    if (!pairs_node || !JSON_NODE_HOLDS_ARRAY(pairs_node)) return NULL;
    GHashTable *map = g_hash_table_new_full(g_str_hash, g_str_equal,
                                            g_free, g_free);
    JsonArray *pairs = json_node_get_array(pairs_node);
    for (guint i = 0; i < json_array_get_length(pairs); i++) {
        JsonNode *pair_node = json_array_get_element(pairs, i);
        if (!pair_node || !JSON_NODE_HOLDS_ARRAY(pair_node)) goto fail;
        JsonArray *pair = json_node_get_array(pair_node);
        if (json_array_get_length(pair) != 2) goto fail;
        const gchar *key = _bulk_string(json_array_get_element(pair, 0));
        const gchar *value = _bulk_string(json_array_get_element(pair, 1));
        if (!key || !value || g_hash_table_contains(map, key)) goto fail;
        g_hash_table_insert(map, g_strdup(key), g_strdup(value));
    }
    return map;
fail:
    g_hash_table_destroy(map);
    return NULL;
}







static gboolean
_bulk_optional_string(JsonNode *node, gchar **value_out)
{
    const gchar *value = _bulk_string(node);
    if (value) {
        *value_out = g_strdup(value);
        return TRUE;
    }
    if (!node || !JSON_NODE_HOLDS_ARRAY(node)) return FALSE;
    JsonArray *tag = json_node_get_array(node);
    if (json_array_get_length(tag) != 2)
        return FALSE;
    JsonNode *members_node = json_array_get_element(tag, 1);
    if (g_strcmp0(_bulk_string(json_array_get_element(tag, 0)), "set") != 0 ||
        !members_node || !JSON_NODE_HOLDS_ARRAY(members_node) ||
        json_array_get_length(json_node_get_array(members_node)) != 0)
        return FALSE;
    *value_out = g_strdup("");
    return TRUE;
}

static gboolean
_bulk_insert_unique(GHashTable *by_name, GHashTable *by_uuid,
                    const gchar *name, const gchar *uuid, gpointer row,
                    GError **error)
{
    if (!name || !*name || g_hash_table_contains(by_name, name) ||
        g_hash_table_contains(by_uuid, uuid)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "OVSDB snapshot contains duplicate/invalid name or UUID");
        return FALSE;
    }
    g_hash_table_insert(by_name, g_strdup(name), row);
    g_hash_table_insert(by_uuid, g_strdup(uuid), row);
    return TRUE;
}

static gboolean
_bulk_parse_ovs_rows(OverlayBulkSnapshot *snapshot, JsonArray *results,
                     GError **error)
{
    if (json_array_get_length(results) != 3) goto malformed;
    for (guint table_index = 0; table_index < 3; table_index++) {
        if (!_restore_deadline_check(error))
            return FALSE;
        JsonNode *result_node = json_array_get_element(results, table_index);
        if (!result_node || !JSON_NODE_HOLDS_OBJECT(result_node)) goto malformed;
        JsonObject *result = json_node_get_object(result_node);
        if (json_object_has_member(result, "error") ||
            !json_object_has_member(result, "rows")) goto malformed;
        JsonArray *rows = json_object_get_array_member(result, "rows");
        if (!rows || json_array_get_length(rows) > OVERLAY_BULK_MAX_ROWS)
            goto malformed;
        for (guint i = 0; i < json_array_get_length(rows); i++) {
            if (!_restore_deadline_check(error))
                return FALSE;
            JsonNode *row_node = json_array_get_element(rows, i);
            if (!row_node || !JSON_NODE_HOLDS_OBJECT(row_node)) goto malformed;
            JsonObject *object = json_node_get_object(row_node);
            const gchar *name = _bulk_string(
                json_object_get_member(object, "name"));
            gchar *uuid = NULL;
            if (!_bulk_uuid(json_object_get_member(object, "_uuid"), &uuid))
                goto malformed;
            if (table_index == 0) {
                OverlayBulkBridge *row = g_new0(OverlayBulkBridge, 1);
                row->uuid = uuid; row->name = g_strdup(name);
                GHashTable *ids = _bulk_string_map(
                    json_object_get_member(object, "external_ids"));
                gboolean valid = name && ids &&
                    _bulk_uuid_set(json_object_get_member(object, "ports"),
                                   &row->ports) &&
                    _bulk_optional_string(
                        json_object_get_member(object, "datapath_type"),
                        &row->datapath_type);
                if (ids) {
                    row->owner = g_strdup(g_hash_table_lookup(ids, OVS_OWNER_KEY));
                    row->overlay_name = g_strdup(
                        g_hash_table_lookup(ids, OVS_NAME_KEY));
                    g_hash_table_destroy(ids);
                }
                if (!valid || !_bulk_insert_unique(
                        snapshot->bridges_by_name,
                        snapshot->bridges_by_uuid, name, uuid, row, error)) {
                    _bulk_bridge_free(row);
                    return FALSE;
                }
            } else if (table_index == 1) {
                OverlayBulkPort *row = g_new0(OverlayBulkPort, 1);
                row->uuid = uuid; row->name = g_strdup(name);
                GHashTable *ids = _bulk_string_map(
                    json_object_get_member(object, "external_ids"));
                if (ids) {
                    row->owner = g_strdup(g_hash_table_lookup(ids, OVS_OWNER_KEY));
                    row->overlay_name = g_strdup(
                        g_hash_table_lookup(ids, OVS_NAME_KEY));
                    g_hash_table_destroy(ids);
                }
                if (!name || !ids || !_bulk_uuid_set(
                        json_object_get_member(object, "interfaces"),
                        &row->interfaces) ||
                    !_bulk_insert_unique(snapshot->ports_by_name,
                                         snapshot->ports_by_uuid,
                                         name, uuid, row, error)) {
                    _bulk_port_free(row);
                    return FALSE;
                }
            } else {
                OverlayBulkInterface *row = g_new0(OverlayBulkInterface, 1);
                row->uuid = uuid; row->name = g_strdup(name);
                GHashTable *ids = _bulk_string_map(
                    json_object_get_member(object, "external_ids"));
                row->options = _bulk_string_map(
                    json_object_get_member(object, "options"));
                gboolean valid = name && ids && row->options &&
                    _bulk_optional_string(json_object_get_member(object, "type"),
                                          &row->type);
                if (ids) {
                    row->owner = g_strdup(g_hash_table_lookup(ids, OVS_OWNER_KEY));
                    row->overlay_name = g_strdup(
                        g_hash_table_lookup(ids, OVS_NAME_KEY));
                    g_hash_table_destroy(ids);
                }
                if (!valid || !_bulk_insert_unique(
                        snapshot->interfaces_by_name,
                        snapshot->interfaces_by_uuid, name, uuid, row, error)) {
                    _bulk_interface_free(row);
                    return FALSE;
                }
            }
        }
    }
    return TRUE;
malformed:
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Malformed bounded OVSDB overlay snapshot");
    return FALSE;
}

static gboolean
_bulk_load_ipv4(OverlayBulkSnapshot *snapshot, GError **error)
{
    const gchar *argv[] = {"ip", "-j", "-4", "addr", "show", NULL};
    gchar *output = NULL;
    if (!_run_argv_capture(argv, &output, error)) {
        g_free(output);
        return FALSE;
    }
    if (!output || strlen(output) > OVERLAY_BULK_MAX_BYTES) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                    "Bounded IPv4 snapshot is empty or oversized");
        g_free(output);
        return FALSE;
    }
    JsonParser *parser = json_parser_new();
    gboolean ok = json_parser_load_from_data(parser, output, -1, error);
    JsonNode *root = ok ? json_parser_get_root(parser) : NULL;
    ok = ok && root && JSON_NODE_HOLDS_ARRAY(root);
    JsonArray *links = ok ? json_node_get_array(root) : NULL;
    if (ok && json_array_get_length(links) > OVERLAY_BULK_MAX_ROWS) ok = FALSE;
    for (guint i = 0; ok && i < json_array_get_length(links); i++) {
        if (!_restore_deadline_check(error)) {
            ok = FALSE;
            break;
        }
        JsonNode *link_node = json_array_get_element(links, i);
        if (!link_node || !JSON_NODE_HOLDS_OBJECT(link_node)) { ok = FALSE; break; }
        JsonObject *link = json_node_get_object(link_node);
        const gchar *ifname = _bulk_string(json_object_get_member(link, "ifname"));
        JsonArray *infos = json_object_get_array_member(link, "addr_info");
        if (!ifname || !infos || g_hash_table_contains(snapshot->ipv4_by_name,
                                                        ifname)) {
            ok = FALSE; break;
        }
        GPtrArray *cidrs = g_ptr_array_new_with_free_func(g_free);
        for (guint j = 0; ok && j < json_array_get_length(infos); j++) {
            if (!_restore_deadline_check(error)) {
                ok = FALSE;
                break;
            }
            JsonNode *info_node = json_array_get_element(infos, j);
            if (!info_node || !JSON_NODE_HOLDS_OBJECT(info_node)) { ok = FALSE; break; }
            JsonObject *info = json_node_get_object(info_node);
            const gchar *family = _bulk_string(json_object_get_member(info, "family"));
            if (g_strcmp0(family, "inet") != 0) continue;
            const gchar *local = _bulk_string(json_object_get_member(info, "local"));
            JsonNode *prefix_node = json_object_get_member(info, "prefixlen");
            if (!local || !prefix_node || !JSON_NODE_HOLDS_VALUE(prefix_node)) {
                ok = FALSE; break;
            }
            gint64 prefix = json_node_get_int(prefix_node);
            gchar *cidr = g_strdup_printf("%s/%" G_GINT64_FORMAT, local, prefix);
            if (prefix < 0 || prefix > 32 ||
                !pcv_validate_cidr(cidr) || _ptr_array_has_string(cidrs, cidr)) {
                g_free(cidr); ok = FALSE; break;
            }
            g_ptr_array_add(cidrs, cidr);
        }
        if (ok)
            g_hash_table_insert(snapshot->ipv4_by_name, g_strdup(ifname), cidrs);
        else
            g_ptr_array_free(cidrs, TRUE);
    }
    if (!ok && (!error || !*error))
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "Malformed bounded IPv4 overlay snapshot");
    g_object_unref(parser);
    g_free(output);
    return ok;
}







static OverlayBulkSnapshot *
_bulk_snapshot_load(GError **error)
{
    static const gchar transaction[] =
        "[\"Open_vSwitch\","
        "{\"op\":\"select\",\"table\":\"Bridge\",\"where\":[],\"columns\":[\"_uuid\",\"name\",\"ports\",\"external_ids\",\"datapath_type\"]},"
        "{\"op\":\"select\",\"table\":\"Port\",\"where\":[],\"columns\":[\"_uuid\",\"name\",\"interfaces\",\"external_ids\"]},"
        "{\"op\":\"select\",\"table\":\"Interface\",\"where\":[],\"columns\":[\"_uuid\",\"name\",\"type\",\"options\",\"external_ids\"]}]";
    const gchar *argv[] = {
        "ovsdb-client", OVS_TIMEOUT_ARG, "query", transaction, NULL
    };
    gchar *output = NULL;
    if (!_run_argv_capture(argv, &output, error)) {
        g_free(output); return NULL;
    }
    if (!output || strlen(output) > OVERLAY_BULK_MAX_BYTES) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                    "Bounded OVSDB overlay snapshot is empty or oversized");
        g_free(output); return NULL;
    }
    OverlayBulkSnapshot *snapshot = g_new0(OverlayBulkSnapshot, 1);
    snapshot->bridges_by_name = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, _bulk_bridge_free);
    snapshot->bridges_by_uuid = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, NULL);
    snapshot->ports_by_name = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, _bulk_port_free);
    snapshot->interfaces_by_name = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, _bulk_interface_free);
    snapshot->ports_by_uuid = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, NULL);
    snapshot->interfaces_by_uuid = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, NULL);
    snapshot->interface_ref_counts = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, NULL);
    snapshot->port_parent_counts = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, NULL);
    snapshot->ipv4_by_name = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, _bulk_string_array_free);
    JsonParser *parser = json_parser_new();
    gboolean ok = json_parser_load_from_data(parser, output, -1, error);
    JsonNode *root = ok ? json_parser_get_root(parser) : NULL;
    ok = ok && root && JSON_NODE_HOLDS_ARRAY(root) &&
         _bulk_parse_ovs_rows(snapshot, json_node_get_array(root), error) &&
         _bulk_validate_relationships(snapshot, error) &&
         _bulk_load_ipv4(snapshot, error);
    if (!ok && (!error || !*error))
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "Malformed bounded OVSDB overlay snapshot");
    g_object_unref(parser);
    g_free(output);
    if (!ok) {
        _bulk_snapshot_free(snapshot);
        return NULL;
    }
    return snapshot;
}

static gboolean
_kernel_link_exists(const gchar *name, gboolean *exists_out, GError **error)
{
    OverlayBulkSnapshot snapshot = {0};
    snapshot.ipv4_by_name = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, _bulk_string_array_free);
    gboolean ok = _bulk_load_ipv4(&snapshot, error);
    *exists_out = ok && g_hash_table_contains(snapshot.ipv4_by_name, name);
    g_hash_table_destroy(snapshot.ipv4_by_name);
    return ok;
}

static gboolean
_bulk_uuid_member(GPtrArray *values, const gchar *uuid)
{
    return values && _ptr_array_has_string(values, uuid);
}

static gboolean
_bulk_relationship_exact(OverlayBulkSnapshot *snapshot,
                         OverlayBulkBridge *bridge,
                         OverlayBulkPort *port,
                         OverlayBulkInterface *interface)
{
    return bridge && port && interface &&
        port->interfaces->len == 1 &&
        g_strcmp0(g_ptr_array_index(port->interfaces, 0), interface->uuid) == 0 &&
        _bulk_uuid_member(bridge->ports, port->uuid) &&
        GPOINTER_TO_UINT(g_hash_table_lookup(
            snapshot->interface_ref_counts, interface->uuid)) == 1 &&
        GPOINTER_TO_UINT(g_hash_table_lookup(
            snapshot->port_parent_counts, port->uuid)) == 1;
}







static gboolean
_bulk_net_exact(OverlayBulkSnapshot *snapshot, OverlayNet *net,
                gboolean *missing_out, GError **error)
{
    *missing_out = FALSE;
    OverlayBulkBridge *bridge = g_hash_table_lookup(snapshot->bridges_by_name,
                                                     net->name);
    if (!bridge) {
        gboolean marker_residue = FALSE;
        GHashTableIter iter; gpointer value = NULL;
        g_hash_table_iter_init(&iter, snapshot->bridges_by_name);
        while (g_hash_table_iter_next(&iter, NULL, &value)) {
            OverlayBulkBridge *row = value;
            marker_residue |= g_strcmp0(row->owner, net->owner_token) == 0 ||
                              g_strcmp0(row->overlay_name, net->name) == 0;
        }
        g_hash_table_iter_init(&iter, snapshot->interfaces_by_name);
        while (g_hash_table_iter_next(&iter, NULL, &value)) {
            OverlayBulkInterface *row = value;
            marker_residue |= g_strcmp0(row->owner, net->owner_token) == 0 ||
                              g_strcmp0(row->overlay_name, net->name) == 0;
        }
        g_hash_table_iter_init(&iter, snapshot->ports_by_name);
        while (g_hash_table_iter_next(&iter, NULL, &value)) {
            OverlayBulkPort *row = value;
            marker_residue |= g_strcmp0(row->owner, net->owner_token) == 0 ||
                              g_strcmp0(row->overlay_name, net->name) == 0;
        }
        gboolean named_residue =
            g_hash_table_contains(snapshot->ports_by_name, net->name) ||
            g_hash_table_contains(snapshot->interfaces_by_name, net->name) ||
            g_hash_table_contains(snapshot->ipv4_by_name, net->name);
        for (guint i = 0; !named_residue && i < net->peers->len; i++) {
            const gchar *peer = g_ptr_array_index(net->peers, i);
            gchar *canonical = pcv_overlay_peer_port_name(net->vni, peer);
            gchar *legacy = _legacy_peer_port_name(peer);
            named_residue =
                g_hash_table_contains(snapshot->ports_by_name, canonical) ||
                g_hash_table_contains(snapshot->interfaces_by_name, canonical) ||
                g_hash_table_contains(snapshot->ports_by_name, legacy) ||
                g_hash_table_contains(snapshot->interfaces_by_name, legacy);
            g_free(canonical);
            g_free(legacy);
        }
        if (!marker_residue && !named_residue) {
            *missing_out = TRUE;
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "Owned Bridge '%s' is absent", net->name);
        } else {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                        "Overlay '%s' has Bridge marker residue", net->name);
        }
        return FALSE;
    }
    if (g_strcmp0(bridge->owner, net->owner_token) != 0 ||
        g_strcmp0(bridge->overlay_name, net->name) != 0 ||
        (bridge->datapath_type && *bridge->datapath_type &&
         g_strcmp0(bridge->datapath_type, "system") != 0)) goto conflict;
    GHashTableIter bridge_iter; gpointer bridge_value = NULL;
    g_hash_table_iter_init(&bridge_iter, snapshot->bridges_by_name);
    while (g_hash_table_iter_next(&bridge_iter, NULL, &bridge_value)) {
        OverlayBulkBridge *row = bridge_value;
        if ((g_strcmp0(row->owner, net->owner_token) == 0 ||
             g_strcmp0(row->overlay_name, net->name) == 0) && row != bridge)
            goto conflict;
    }
    GPtrArray *cidrs = g_hash_table_lookup(snapshot->ipv4_by_name, net->name);
    guint cidr_count = cidrs ? cidrs->len : 0;
    if ((net->cidr && *net->cidr &&
         (cidr_count != 1 ||
          g_strcmp0(g_ptr_array_index(cidrs, 0), net->cidr) != 0)) ||
        ((!net->cidr || !*net->cidr) && cidr_count != 0))
        goto conflict;

    OverlayBulkPort *local_port = g_hash_table_lookup(snapshot->ports_by_name,
                                                       net->name);
    if (!local_port || local_port->interfaces->len != 1) goto conflict;
    OverlayBulkInterface *local_interface = g_hash_table_lookup(
        snapshot->interfaces_by_uuid,
        g_ptr_array_index(local_port->interfaces, 0));
    if (!_bulk_relationship_exact(snapshot, bridge, local_port,
                                  local_interface) ||
        g_strcmp0(local_interface->name, net->name) != 0 ||
        (local_port->owner && *local_port->owner) ||
        (local_port->overlay_name && *local_port->overlay_name) ||
        g_strcmp0(local_interface->type, "internal") != 0 ||
        (local_interface->owner && *local_interface->owner) ||
        (local_interface->overlay_name && *local_interface->overlay_name))
        goto conflict;




    for (guint i = 0; i < bridge->ports->len; i++) {
        if (!_restore_deadline_check(error))
            return FALSE;
        const gchar *port_uuid = g_ptr_array_index(bridge->ports, i);
        OverlayBulkPort *port = g_hash_table_lookup(snapshot->ports_by_uuid,
                                                     port_uuid);
        if (!port) goto conflict;
        if (port == local_port) continue;
        if (_peer_for_canonical_port(net, port->name)) continue;
        gboolean workload_name = g_str_has_prefix(port->name, "tap") ||
                                 g_str_has_prefix(port->name, "vnet");
        if (!workload_name || port->interfaces->len != 1) goto conflict;
        OverlayBulkInterface *interface = g_hash_table_lookup(
            snapshot->interfaces_by_uuid,
            g_ptr_array_index(port->interfaces, 0));
        if (!_bulk_relationship_exact(snapshot, bridge, port, interface) ||
            g_strcmp0(interface->name, port->name) != 0 ||
            (port->owner && *port->owner) ||
            (port->overlay_name && *port->overlay_name) ||
            (interface->owner && *interface->owner) ||
            (interface->overlay_name && *interface->overlay_name) ||
            (interface->type && *interface->type))
            goto conflict;
    }

    guint marked_count = 0;
    GHashTableIter iface_iter; gpointer iface_value = NULL;
    g_hash_table_iter_init(&iface_iter, snapshot->interfaces_by_name);
    while (g_hash_table_iter_next(&iface_iter, NULL, &iface_value)) {
        if (!_restore_deadline_check(error))
            return FALSE;
        OverlayBulkInterface *row = iface_value;
        if (g_strcmp0(row->owner, net->owner_token) != 0 &&
            g_strcmp0(row->overlay_name, net->name) != 0)
            continue;
        marked_count++;
        if (g_strcmp0(row->owner, net->owner_token) != 0 ||
            g_strcmp0(row->overlay_name, net->name) != 0 ||
            !_peer_for_canonical_port(net, row->name))
            goto conflict;
    }
    if (marked_count > net->peers->len)
        goto conflict;
    gboolean clean_missing = FALSE;
    for (guint i = 0; i < net->peers->len; i++) {
        if (!_restore_deadline_check(error))
            return FALSE;
        const gchar *peer = g_ptr_array_index(net->peers, i);
        gchar *name = pcv_overlay_peer_port_name(net->vni, peer);
        gchar *legacy = _legacy_peer_port_name(peer);
        OverlayBulkInterface *interface = g_hash_table_lookup(
            snapshot->interfaces_by_name, name);
        OverlayBulkPort *port = g_hash_table_lookup(snapshot->ports_by_name, name);
        gboolean legacy_absent =
            !g_hash_table_contains(snapshot->interfaces_by_name, legacy) &&
            !g_hash_table_contains(snapshot->ports_by_name, legacy);
        if (!interface && !port && legacy_absent) {
            clean_missing = TRUE;
            g_free(name);
            g_free(legacy);
            continue;
        }
        gboolean exact = legacy_absent &&
            _bulk_relationship_exact(snapshot, bridge, port, interface) &&
            (!port->owner || !*port->owner) &&
            (!port->overlay_name || !*port->overlay_name) &&
            g_strcmp0(interface->type, "vxlan") == 0 &&
            g_strcmp0(interface->owner, net->owner_token) == 0 &&
            g_strcmp0(interface->overlay_name, net->name) == 0 &&
            g_hash_table_size(interface->options) == 3;
        gchar *key = g_strdup_printf("%d", net->vni);
        exact = exact &&
            g_strcmp0(g_hash_table_lookup(interface->options, "key"), key) == 0 &&
            g_strcmp0(g_hash_table_lookup(interface->options, "remote_ip"), peer) == 0 &&
            g_strcmp0(g_hash_table_lookup(interface->options, "local_ip"), G.local_ip) == 0;
        g_free(key); g_free(name); g_free(legacy);
        if (!exact) goto conflict;
    }
    if (clean_missing || marked_count < net->peers->len) {
        *missing_out = TRUE;
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Overlay '%s' has only cleanly missing desired peers",
                    net->name);
        return FALSE;
    }
    return TRUE;
conflict:
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                "Overlay '%s' bulk actual snapshot is not exact", net->name);
    return FALSE;
}







static gboolean
_bulk_global_exact(OverlayBulkSnapshot *snapshot, GError **error)
{
    GHashTableIter iter; gpointer value = NULL;
    g_hash_table_iter_init(&iter, snapshot->bridges_by_name);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        if (!_restore_deadline_check(error))
            return FALSE;
        OverlayBulkBridge *row = value;
        if ((!row->owner || !*row->owner) &&
            (!row->overlay_name || !*row->overlay_name)) continue;
        OverlayNet *net = _find(row->overlay_name);
        if (!net || g_strcmp0(row->owner, net->owner_token) != 0 ||
            g_strcmp0(row->name, net->name) != 0) goto conflict;
    }
    g_hash_table_iter_init(&iter, snapshot->interfaces_by_name);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        if (!_restore_deadline_check(error))
            return FALSE;
        OverlayBulkInterface *row = value;
        if ((!row->owner || !*row->owner) &&
            (!row->overlay_name || !*row->overlay_name)) continue;
        OverlayNet *net = _find(row->overlay_name);
        if (!net || g_strcmp0(row->owner, net->owner_token) != 0 ||
            !_peer_for_canonical_port(net, row->name)) goto conflict;
    }
    g_hash_table_iter_init(&iter, snapshot->ports_by_name);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        if (!_restore_deadline_check(error))
            return FALSE;
        OverlayBulkPort *row = value;
        if ((row->owner && *row->owner) ||
            (row->overlay_name && *row->overlay_name))
            goto conflict;
    }
    return TRUE;
conflict:
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Unregistered or partial ownership marker in bulk OVSDB snapshot");
    return FALSE;
}

static OverlayMeta *
_preflight_meta_by_name(GPtrArray *metas, const gchar *name)
{
    for (guint i = 0; i < metas->len; i++) {
        OverlayMeta *meta = g_ptr_array_index(metas, i);
        if (g_strcmp0(meta->name, name) == 0)
            return meta;
    }
    return NULL;
}

static gboolean
_preflight_meta_has_port(OverlayMeta *meta, const gchar *port_name)
{
    for (guint i = 0; i < meta->peers->len; i++) {
        gchar *canonical = pcv_overlay_peer_port_name(
            meta->vni, g_ptr_array_index(meta->peers, i));
        gboolean match = g_strcmp0(canonical, port_name) == 0;
        g_free(canonical);
        if (match)
            return TRUE;
    }
    return FALSE;
}











static gboolean
_bulk_preflight_metadata_exact(OverlayBulkSnapshot *snapshot,
                               GPtrArray *metas, GError **error)
{
    for (gint i = 0; i < G.count; i++) {
        OverlayMeta *meta = _preflight_meta_by_name(metas, G.nets[i].name);
        if (!meta || !_overlay_meta_matches_net(meta, &G.nets[i]))
            goto conflict;
    }

    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, snapshot->bridges_by_name);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        if (!_restore_deadline_check(error))
            return FALSE;
        OverlayBulkBridge *row = value;
        gboolean marked = (row->owner && *row->owner) ||
                          (row->overlay_name && *row->overlay_name);
        if (!marked)
            continue;
        OverlayMeta *meta = _preflight_meta_by_name(metas, row->overlay_name);
        if (!meta || meta->legacy || !row->owner || !*row->owner ||
            !row->overlay_name || !*row->overlay_name ||
            g_strcmp0(meta->owner_token, row->owner) != 0 ||
            g_strcmp0(meta->name, row->name) != 0)
            goto conflict;
    }

    g_hash_table_iter_init(&iter, snapshot->interfaces_by_name);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        if (!_restore_deadline_check(error))
            return FALSE;
        OverlayBulkInterface *row = value;
        gboolean marked = (row->owner && *row->owner) ||
                          (row->overlay_name && *row->overlay_name);
        if (!marked)
            continue;
        OverlayMeta *meta = _preflight_meta_by_name(metas, row->overlay_name);
        if (!meta || meta->legacy || !row->owner || !*row->owner ||
            !row->overlay_name || !*row->overlay_name ||
            g_strcmp0(meta->owner_token, row->owner) != 0 ||
            !_preflight_meta_has_port(meta, row->name))
            goto conflict;
    }



    g_hash_table_iter_init(&iter, snapshot->ports_by_name);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        if (!_restore_deadline_check(error))
            return FALSE;
        OverlayBulkPort *row = value;
        if ((row->owner && *row->owner) ||
            (row->overlay_name && *row->overlay_name))
            goto conflict;
    }

    for (guint i = 0; i < metas->len; i++) {
        if (!_restore_deadline_check(error))
            return FALSE;
        OverlayMeta *meta = g_ptr_array_index(metas, i);
        OverlayNet candidate = {
            .name = meta->name,
            .cidr = meta->cidr,
            .owner_token = meta->legacy ? (gchar *)"" : meta->owner_token,
            .vni = meta->vni,
            .generation = meta->generation,
            .peers = meta->peers,
        };
        OverlayBulkBridge *bridge = g_hash_table_lookup(
            snapshot->bridges_by_name, candidate.name);
        if (meta->legacy && bridge) {
            gboolean bridge_exists = FALSE;
            if (!_validate_legacy_actual_locked(&candidate, &bridge_exists,
                                                error) || !bridge_exists)
                return FALSE;
            continue;
        }
        gboolean missing = FALSE;
        GError *candidate_error = NULL;
        if (_bulk_net_exact(snapshot, &candidate, &missing,
                            &candidate_error)) {
            g_clear_error(&candidate_error);
            continue;
        }
        if (missing) {
            g_clear_error(&candidate_error);
            continue;
        }
        if (candidate_error)
            g_propagate_error(error, candidate_error);
        else
            goto conflict;
        return FALSE;
    }
    return TRUE;

conflict:
    if (!error || !*error)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "Overlay restore ownership preflight is not exact");
    return FALSE;
}











static gboolean
_bulk_legacy_relationships_exact(OverlayBulkSnapshot *snapshot,
                                 OverlayNet *net, GError **error)
{
    OverlayBulkBridge *bridge = g_hash_table_lookup(snapshot->bridges_by_name,
                                                     net->name);
    if (!bridge)
        return TRUE;
    if ((bridge->owner && *bridge->owner) ||
        (bridge->overlay_name && *bridge->overlay_name) ||
        (bridge->datapath_type && *bridge->datapath_type &&
         g_strcmp0(bridge->datapath_type, "system") != 0) ||
        bridge->ports->len != net->peers->len + 1)
        goto conflict;

    OverlayBulkPort *local_port = g_hash_table_lookup(snapshot->ports_by_name,
                                                       net->name);
    OverlayBulkInterface *local_interface = local_port &&
        local_port->interfaces->len == 1
        ? g_hash_table_lookup(snapshot->interfaces_by_uuid,
                             g_ptr_array_index(local_port->interfaces, 0))
        : NULL;
    if (!_bulk_relationship_exact(snapshot, bridge, local_port,
                                  local_interface) ||
        g_strcmp0(local_interface->name, net->name) != 0 ||
        (local_port->owner && *local_port->owner) ||
        (local_port->overlay_name && *local_port->overlay_name) ||
        g_strcmp0(local_interface->type, "internal") != 0 ||
        (local_interface->owner && *local_interface->owner) ||
        (local_interface->overlay_name && *local_interface->overlay_name))
        goto conflict;

    for (guint i = 0; i < net->peers->len; i++) {
        const gchar *peer = g_ptr_array_index(net->peers, i);
        gchar *legacy = _legacy_peer_port_name(peer);
        gchar *canonical = pcv_overlay_peer_port_name(net->vni, peer);
        OverlayBulkPort *port = g_hash_table_lookup(snapshot->ports_by_name,
                                                     legacy);
        OverlayBulkInterface *interface = g_hash_table_lookup(
            snapshot->interfaces_by_name, legacy);
        gboolean exact =
            !g_hash_table_contains(snapshot->interfaces_by_name, canonical) &&
            _bulk_relationship_exact(snapshot, bridge, port, interface) &&
            (!port->owner || !*port->owner) &&
            (!port->overlay_name || !*port->overlay_name) &&
            (!interface->owner || !*interface->owner) &&
            (!interface->overlay_name || !*interface->overlay_name);
        g_free(canonical);
        g_free(legacy);
        if (!exact)
            goto conflict;
    }
    return TRUE;

conflict:
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                "Legacy overlay '%s' Port/Interface ownership topology is not exact",
                net->name);
    return FALSE;
}

static gboolean
_interface_options_exact(OverlayNet *net, const gchar *port_name,
                         const gchar *peer_ip, GError **error)
{






    const gchar *argv[] = {
        "ovs-vsctl", "--format=json", "--data=json", "--columns=options",
        "list", "Interface", port_name, NULL
    };
    gchar *output = NULL;
    if (!_run_argv_capture(argv, &output, error)) {
        g_free(output);
        return FALSE;
    }
    JsonParser *parser = json_parser_new();
    gboolean ok = json_parser_load_from_data(parser, output ? output : "",
                                             -1, error);
    if (!ok)
        goto out;
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root))
        goto malformed;
    JsonObject *table = json_node_get_object(root);
    JsonNode *headings_node = json_object_get_member(table, "headings");
    JsonNode *data_node = json_object_get_member(table, "data");
    if (!headings_node || !JSON_NODE_HOLDS_ARRAY(headings_node) ||
        !data_node || !JSON_NODE_HOLDS_ARRAY(data_node))
        goto malformed;
    JsonArray *headings = json_node_get_array(headings_node);
    JsonArray *data = json_node_get_array(data_node);
    if (json_array_get_length(headings) != 1 ||
        g_strcmp0(_overlay_json_string(json_array_get_element(headings, 0)),
                  "options") != 0 || json_array_get_length(data) != 1)
        goto malformed;
    JsonNode *row_node = json_array_get_element(data, 0);
    if (!row_node || !JSON_NODE_HOLDS_ARRAY(row_node))
        goto malformed;
    JsonArray *row = json_node_get_array(row_node);
    if (json_array_get_length(row) != 1)
        goto malformed;
    JsonNode *map_node = json_array_get_element(row, 0);
    if (!map_node || !JSON_NODE_HOLDS_ARRAY(map_node))
        goto malformed;
    JsonArray *map = json_node_get_array(map_node);
    if (json_array_get_length(map) != 2)
        goto malformed;
    JsonNode *tag_node = json_array_get_element(map, 0);
    JsonNode *pairs_node = json_array_get_element(map, 1);
    if (!tag_node || !JSON_NODE_HOLDS_VALUE(tag_node) ||
        json_node_get_value_type(tag_node) != G_TYPE_STRING ||
        g_strcmp0(json_node_get_string(tag_node), "map") != 0 ||
        !pairs_node || !JSON_NODE_HOLDS_ARRAY(pairs_node))
        goto malformed;
    JsonArray *pairs = json_node_get_array(pairs_node);
    if (json_array_get_length(pairs) != 3)
        goto mismatch;

    const gchar *expected_keys[] = {"key", "remote_ip", "local_ip"};
    gchar *expected_vni = g_strdup_printf("%d", net->vni);
    const gchar *expected_values[] = {expected_vni, peer_ip, G.local_ip};
    gboolean found[3] = {FALSE, FALSE, FALSE};
    for (guint i = 0; i < json_array_get_length(pairs); i++) {
        JsonNode *pair_node = json_array_get_element(pairs, i);
        if (!pair_node || !JSON_NODE_HOLDS_ARRAY(pair_node)) {
            g_free(expected_vni);
            goto malformed;
        }
        JsonArray *pair = json_node_get_array(pair_node);
        if (json_array_get_length(pair) != 2) {
            g_free(expected_vni);
            goto malformed;
        }
        JsonNode *key_node = json_array_get_element(pair, 0);
        JsonNode *value_node = json_array_get_element(pair, 1);
        if (!key_node || !value_node || !JSON_NODE_HOLDS_VALUE(key_node) ||
            !JSON_NODE_HOLDS_VALUE(value_node) ||
            json_node_get_value_type(key_node) != G_TYPE_STRING ||
            json_node_get_value_type(value_node) != G_TYPE_STRING) {
            g_free(expected_vni);
            goto malformed;
        }
        const gchar *key = json_node_get_string(key_node);
        const gchar *value = json_node_get_string(value_node);
        gint matched = -1;
        for (gint j = 0; j < 3; j++)
            if (g_strcmp0(key, expected_keys[j]) == 0)
                matched = j;
        if (matched < 0 || found[matched] ||
            g_strcmp0(value, expected_values[matched]) != 0) {
            g_free(expected_vni);
            goto mismatch;
        }
        found[matched] = TRUE;
    }
    ok = found[0] && found[1] && found[2];
    g_free(expected_vni);
    if (!ok)
        goto mismatch;
    goto out;

malformed:
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Malformed OVS Interface options JSON for '%s'", port_name);
    ok = FALSE;
    goto out;
mismatch:
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                "OVS Interface '%s' options map is not exact", port_name);
    ok = FALSE;
out:
    g_object_unref(parser);
    g_free(output);
    return ok;
}

static gboolean
_ovs_row_uuid(const gchar *table, const gchar *name, gchar **uuid_out,
              GError **error)
{
    gboolean exists = FALSE;
    gchar *uuid = NULL;
    if (!_ovs_get_field(table, name, "_uuid", &exists, &uuid, error))
        return FALSE;
    if (!exists || !uuid || !g_uuid_string_is_valid(uuid)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "OVS %s '%s' has no valid UUID", table, name);
        g_free(uuid);
        return FALSE;
    }
    *uuid_out = uuid;
    return TRUE;
}












static gboolean
_port_interface_exact_condition(const gchar *name, gchar **condition_out,
                                GError **error)
{
    gchar *interface_uuid = NULL;
    if (!_ovs_row_uuid("Interface", name, &interface_uuid, error))
        return FALSE;

    gboolean port_exists = FALSE;
    gchar *interfaces = NULL;
    if (!_ovs_get_field("Port", name, "interfaces", &port_exists,
                        &interfaces, error)) {
        g_free(interface_uuid);
        return FALSE;
    }
    gchar *singleton = g_strdup_printf("[%s]", interface_uuid);
    gboolean exact = port_exists &&
        (g_strcmp0(interfaces, interface_uuid) == 0 ||
         g_strcmp0(interfaces, singleton) == 0);
    if (!exact)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "OVS Port '%s' does not contain its exact same-name Interface",
                    name);
    if (exact && condition_out)
        *condition_out = g_strdup_printf("interfaces=[%s]", interface_uuid);
    g_free(singleton);
    g_free(interfaces);
    g_free(interface_uuid);
    return exact;
}




static gboolean
_bridge_ports_exact_condition(const gchar *bridge, GPtrArray *child_names,
                              gchar **condition_out, GError **error)
{
    GPtrArray *uuids = g_ptr_array_new_with_free_func(g_free);
    gchar *local_uuid = NULL;
    if (!_ovs_row_uuid("Port", bridge, &local_uuid, error)) {
        g_ptr_array_free(uuids, TRUE);
        return FALSE;
    }
    g_ptr_array_add(uuids, local_uuid);
    if (child_names) {
        for (guint i = 0; i < child_names->len; i++) {
            gchar *uuid = NULL;
            if (!_ovs_row_uuid("Port", g_ptr_array_index(child_names, i),
                               &uuid, error)) {
                g_ptr_array_free(uuids, TRUE);
                return FALSE;
            }
            g_ptr_array_add(uuids, uuid);
        }
    }

    GString *condition = g_string_new("ports=[");
    for (guint i = 0; i < uuids->len; i++) {
        if (i)
            g_string_append_c(condition, ',');
        g_string_append(condition, g_ptr_array_index(uuids, i));
    }
    g_string_append_c(condition, ']');
    *condition_out = g_string_free(condition, FALSE);
    g_ptr_array_free(uuids, TRUE);
    return TRUE;
}

static gboolean
_bridge_current_ports_condition(const gchar *bridge,
                                const gchar *required_child,
                                gchar **condition_out, GError **error)
{
    GPtrArray *children = _bridge_ports(bridge, error);
    if (!children)
        return FALSE;
    if (required_child && !_ptr_array_has_string(children, required_child)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "OVS Port '%s' is no longer attached to Bridge '%s'",
                    required_child, bridge);
        g_ptr_array_free(children, TRUE);
        return FALSE;
    }
    gboolean ok = _bridge_ports_exact_condition(bridge, children,
                                                condition_out, error);
    g_ptr_array_free(children, TRUE);
    return ok;
}

static const gchar *
_peer_for_canonical_port(OverlayNet *net, const gchar *port_name)
{
    for (guint i = 0; i < net->peers->len; i++) {
        const gchar *peer = g_ptr_array_index(net->peers, i);
        gchar *expected = pcv_overlay_peer_port_name(net->vni, peer);
        gboolean matches = g_strcmp0(expected, port_name) == 0;
        g_free(expected);
        if (matches)
            return peer;
    }
    return NULL;
}







static gboolean
_unmarked_workload_child_exact(OverlayNet *net, const gchar *port,
                               GError **error)
{
    if (!g_str_has_prefix(port, "tap") && !g_str_has_prefix(port, "vnet"))
        goto conflict;
    gchar *interfaces_condition = NULL;
    if (!_port_interface_exact_condition(port, &interfaces_condition, error)) {
        g_free(interfaces_condition);
        return FALSE;
    }
    g_free(interfaces_condition);

    const gchar *tables[] = {"Port", "Port", "Interface", "Interface",
                             "Interface"};
    gchar *owner_column = g_strdup_printf("external_ids:%s", OVS_OWNER_KEY);
    gchar *name_column = g_strdup_printf("external_ids:%s", OVS_NAME_KEY);
    const gchar *columns[] = {owner_column, name_column, owner_column,
                              name_column, "type"};
    gchar *values[G_N_ELEMENTS(tables)] = {0};
    gboolean exact = TRUE;
    for (guint i = 0; exact && i < G_N_ELEMENTS(tables); i++) {
        gboolean exists = FALSE;
        exact = _ovs_get_field(tables[i], port, columns[i], &exists,
                               &values[i], error) && exists &&
                (!values[i] || !*values[i]);
    }
    const gchar *bridge_argv[] = {
        "ovs-vsctl", "--bare", "iface-to-br", port, NULL
    };
    gchar *bridge_raw = NULL;
    if (exact)
        exact = _run_argv_capture(bridge_argv, &bridge_raw, error);
    gchar *bridge = exact ? _ovs_scalar_normalize(bridge_raw) : NULL;
    exact = exact && g_strcmp0(bridge, net->name) == 0;
    for (guint i = 0; i < G_N_ELEMENTS(values); i++)
        g_free(values[i]);
    g_free(bridge_raw);
    g_free(bridge);
    g_free(owner_column);
    g_free(name_column);
    if (exact)
        return TRUE;

conflict:
    if (!error || !*error)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "Overlay '%s' has a non-workload or marked child Port '%s'",
                    net->name, port);
    return FALSE;
}




static gboolean
_validate_owned_actual_inventory_locked(OverlayNet *net, gboolean require_all,
                                        gboolean require_exact_options,
                                        GError **error)
{




    if (require_all && require_exact_options) {
        OverlayBulkSnapshot *bulk = _restore_bulk_get();
        gboolean owns_bulk = FALSE;
        if (!bulk) {
            bulk = _bulk_snapshot_load(error);
            owns_bulk = TRUE;
        }
        gboolean missing = FALSE;
        gboolean exact = bulk && _bulk_net_exact(bulk, net, &missing, error);
        if (owns_bulk)
            _bulk_snapshot_free(bulk);
        return exact;
    }

    GPtrArray *marked = _ovs_find_union(
        "Interface", "external_ids:" OVS_OWNER_KEY, net->owner_token,
        "external_ids:" OVS_NAME_KEY, net->name, error);
    if (!marked)
        return FALSE;

    GPtrArray *marked_bridges = _ovs_find_union(
        "Bridge", "external_ids:" OVS_OWNER_KEY, net->owner_token,
        "external_ids:" OVS_NAME_KEY, net->name, error);
    if (!marked_bridges) {
        g_ptr_array_free(marked, TRUE);
        return FALSE;
    }

    gboolean bridge_exists = FALSE;
    if (!_bridge_is_owned(net->name, net->owner_token, &bridge_exists, error)) {
        g_ptr_array_free(marked, TRUE);
        g_ptr_array_free(marked_bridges, TRUE);
        return FALSE;
    }
    gboolean bridge_inventory_exact = bridge_exists
        ? marked_bridges->len == 1 &&
          g_strcmp0(g_ptr_array_index(marked_bridges, 0), net->name) == 0
        : marked_bridges->len == 0;
    if (!bridge_inventory_exact) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "Overlay '%s' Bridge ownership inventory is not exact",
                    net->name);
        g_ptr_array_free(marked, TRUE);
        g_ptr_array_free(marked_bridges, TRUE);
        return FALSE;
    }
    g_ptr_array_free(marked_bridges, TRUE);
    if (!bridge_exists) {
        gboolean clean = marked->len == 0 && !require_all;
        if (!clean)
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                        "Overlay '%s' has marked Interface residue without its owned Bridge",
                        net->name);
        g_ptr_array_free(marked, TRUE);
        return clean;
    }

    GPtrArray *children = _bridge_ports(net->name, error);
    if (!children) {
        g_ptr_array_free(marked, TRUE);
        return FALSE;
    }





    gboolean exact = !require_all || marked->len == net->peers->len;
    for (guint i = 0; exact && i < marked->len; i++) {
        const gchar *port = g_ptr_array_index(marked, i);
        const gchar *peer = _peer_for_canonical_port(net, port);
        gboolean exists = FALSE;
        gchar *port_interfaces_cond = NULL;
        exact = peer && _ptr_array_has_string(children, port) &&
                _interface_is_owned(net, port, &exists, error) && exists &&
                _port_interface_exact_condition(port,
                                                &port_interfaces_cond, error);
        if (exact && require_exact_options) {
            gboolean row_exists = FALSE;
            gchar *type = NULL;
            exact = _ovs_get_field("Interface", port, "type", &row_exists,
                                   &type, error) && row_exists &&
                    g_strcmp0(type, "vxlan") == 0 &&
                    _interface_options_exact(net, port, peer, error);
            g_free(type);
        }
        g_free(port_interfaces_cond);
    }
    for (guint i = 0; exact && require_all && i < net->peers->len; i++) {
        gchar *canonical = pcv_overlay_peer_port_name(
            net->vni, g_ptr_array_index(net->peers, i));
        exact = _ptr_array_has_string(marked, canonical) &&
                _ptr_array_has_string(children, canonical);
        g_free(canonical);
    }
    for (guint i = 0; exact && i < children->len; i++) {
        const gchar *child = g_ptr_array_index(children, i);
        if (_peer_for_canonical_port(net, child))
            exact = _ptr_array_has_string(marked, child);
        else
            exact = _unmarked_workload_child_exact(net, child, error);
    }

    for (guint i = 0; exact && i < net->peers->len; i++) {
        gchar *legacy = _legacy_peer_port_name(g_ptr_array_index(net->peers, i));
        gboolean legacy_exists = FALSE;
        exact = _ovs_row_exists("Interface", "name", legacy,
                                &legacy_exists, error) && !legacy_exists;
        g_free(legacy);
    }
    if (!exact && (!error || !*error))
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "Overlay '%s' actual Interface inventory is not a closed desired set",
                    net->name);
    g_ptr_array_free(children, TRUE);
    g_ptr_array_free(marked, TRUE);
    return exact;
}

static gboolean
_delete_owned_empty_bridge(const gchar *name, const gchar *owner_token,
                           GError **error)
{
    gchar *ports_cond = NULL;
    if (!_bridge_ports_exact_condition(name, NULL, &ports_cond, error))
        return FALSE;
    gchar *owner_cond = g_strdup_printf("external_ids:%s=%s", OVS_OWNER_KEY,
                                        owner_token);
    gchar *name_cond = g_strdup_printf("external_ids:%s=%s", OVS_NAME_KEY, name);
    const gchar *argv[] = {
        "ovs-vsctl", "--", "wait-until", "Bridge", name,
        owner_cond, name_cond, ports_cond,
        "--", "del-br", name, NULL
    };
    gboolean ok = _run_argv(argv, error);
    g_free(owner_cond);
    g_free(name_cond);
    g_free(ports_cond);
    return ok;
}

static void
_actual_rollback_failed_locked(const gchar *name, const gchar *operation,
                               GError **operation_error,
                               GError *rollback_error)
{
    const gchar *rollback_message = rollback_error
        ? rollback_error->message : "unknown rollback failure";
    gchar *primary = operation_error && *operation_error
        ? g_strdup((*operation_error)->message)
        : g_strdup("overlay operation failed");
    if (operation_error) {
        g_clear_error(operation_error);
        g_set_error(operation_error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "%s; %s rollback left owned actual residue: %s",
                    primary, operation, rollback_message);
    }
    PCV_LOG_WARN(OVERLAY_LOG_DOM,
                 "%s rollback left owned actual residue for '%s': %s",
                 operation, name, rollback_message);
    _record_metadata_residue_locked(
        "overlay actual rollback left owned-state residue");
    g_free(primary);
}

static void
_actual_outcome_uncertain_locked(const gchar *name, const gchar *operation,
                                 GError **operation_error,
                                 const gchar *detail)
{
    GQuark domain = operation_error && *operation_error
        ? (*operation_error)->domain : G_IO_ERROR;
    gint code = operation_error && *operation_error
        ? (*operation_error)->code : G_IO_ERROR_FAILED;
    gchar *primary = operation_error && *operation_error
        ? g_strdup((*operation_error)->message)
        : g_strdup("overlay operation failed");
    if (operation_error) {
        g_clear_error(operation_error);
        g_set_error(operation_error, domain, code,
                    "%s; %s actual outcome is uncertain: %s", primary,
                    operation, detail ? detail : "readback failed");
    }
    _record_metadata_residue_locked(
        "overlay actual mutation outcome requires recovery");
    g_free(primary);
}

static gboolean
_rollback_bridge(const gchar *name, const gchar *owner_token,
                 GError **operation_error)
{
    OverlayCleanupDeadlineScope cleanup_scope =
        _restore_cleanup_deadline_enter();
    GError *rollback_error = NULL;
    if (_delete_owned_empty_bridge(name, owner_token, &rollback_error)) {
        _restore_cleanup_deadline_leave(&cleanup_scope);
        return TRUE;
    }
    gboolean still_exists = FALSE;
    GError *readback_error = NULL;
    if (_ovs_row_exists("Bridge", "name", name, &still_exists,
                        &readback_error) && !still_exists) {
        g_clear_error(&rollback_error);
        g_clear_error(&readback_error);
        _restore_cleanup_deadline_leave(&cleanup_scope);
        return TRUE;
    }
    g_clear_error(&readback_error);
    _actual_rollback_failed_locked(name, "bridge", operation_error,
                                   rollback_error);
    g_clear_error(&rollback_error);
    _restore_cleanup_deadline_leave(&cleanup_scope);
    return FALSE;
}

static gboolean
_ensure_bridge_actual_locked(const gchar *name, const gchar *cidr,
                             const gchar *owner_token, gboolean *created_out,
                             GError **error)
{
    gboolean exists = FALSE;
    gchar *owner = NULL;
    gchar *actual_name = NULL;
    *created_out = FALSE;
    if (!_ovs_owner_probe("Bridge", name, &exists, &owner, error))
        return FALSE;
    if (exists) {
        gchar *column = g_strdup_printf("external_ids:%s", OVS_NAME_KEY);
        gboolean row_exists = FALSE;
        if (!_ovs_get_field("Bridge", name, column, &row_exists, &actual_name,
                            error)) {
            g_free(column); g_free(owner);
            return FALSE;
        }
        g_free(column);
    }

    if (!exists) {
        gboolean kernel_exists = FALSE;
        if (!_kernel_link_exists(name, &kernel_exists, error)) {
            g_free(owner);
            g_free(actual_name);
            return FALSE;
        }
        if (kernel_exists) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                        "Kernel link '%s' already exists outside the owned OVS Bridge",
                        name);
            g_free(owner);
            g_free(actual_name);
            return FALSE;
        }
    }

    gchar *owner_opt = g_strdup_printf("external_ids:%s=%s", OVS_OWNER_KEY,
                                       owner_token);
    gchar *name_opt = g_strdup_printf("external_ids:%s=%s", OVS_NAME_KEY, name);
    if (!exists) {


        const gchar *add_argv[] = {
            "ovs-vsctl", "--", "add-br", name,
            "--", "set", "Bridge", name, owner_opt, name_opt, NULL
        };
        if (!_run_argv(add_argv, error)) {




            OverlayCleanupDeadlineScope cleanup_scope =
                _restore_cleanup_deadline_enter();
            GError *probe_error = NULL;
            gboolean after_exists = FALSE;
            gchar *after_owner = NULL;
            gchar *after_name = NULL;
            gboolean probe_ok = _ovs_owner_probe(
                "Bridge", name, &after_exists, &after_owner, &probe_error);
            if (probe_ok && after_exists) {
                gchar *column = g_strdup_printf("external_ids:%s",
                                                OVS_NAME_KEY);
                gboolean row_exists = FALSE;
                probe_ok = _ovs_get_field("Bridge", name, column,
                                          &row_exists, &after_name,
                                          &probe_error) && row_exists;
                g_free(column);
            }
            gboolean applied = probe_ok && after_exists &&
                g_strcmp0(after_owner, owner_token) == 0 &&
                g_strcmp0(after_name, name) == 0;
            gboolean unchanged_foreign = probe_ok &&
                (!after_exists ||
                 (g_strcmp0(after_owner, owner_token) != 0 &&
                  g_strcmp0(after_name, name) != 0));
            if (applied) {
                *created_out = TRUE;
                (void)_rollback_bridge(name, owner_token, error);
            } else if (!unchanged_foreign) {
                _actual_outcome_uncertain_locked(
                    name, "bridge create", error,
                    probe_error ? probe_error->message
                                : "partial ownership markers");
            }
            g_clear_error(&probe_error);
            g_free(after_owner);
            g_free(after_name);
            _restore_cleanup_deadline_leave(&cleanup_scope);
            g_free(owner_opt); g_free(name_opt); g_free(owner); g_free(actual_name);
            return FALSE;
        }
        *created_out = TRUE;
    } else if (g_strcmp0(owner, owner_token) == 0 &&
               g_strcmp0(actual_name, name) == 0) {
        gchar *owner_cond = g_strdup_printf("external_ids:%s=%s", OVS_OWNER_KEY,
                                            owner_token);
        gchar *name_cond = g_strdup_printf("external_ids:%s=%s", OVS_NAME_KEY,
                                           name);
        const gchar *exact_argv[] = {
            "ovs-vsctl", "--", "wait-until", "Bridge", name,
            owner_cond, name_cond,
            "--", "set", "Bridge", name, owner_opt, name_opt, NULL
        };
        gboolean ok = _run_argv(exact_argv, error);
        g_free(owner_cond);
        g_free(name_cond);
        if (!ok) {
            g_free(owner_opt); g_free(name_opt); g_free(owner); g_free(actual_name);
            return FALSE;
        }
    } else {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "OVS bridge '%s' ownership token mismatch", name);
        g_free(owner_opt); g_free(name_opt); g_free(owner); g_free(actual_name);
        return FALSE;
    }
    g_free(owner_opt); g_free(name_opt); g_free(owner); g_free(actual_name);





    if (!*created_out)
        return _legacy_bridge_matches(name, cidr, error);

    const gchar *up_argv[] = {"ip", "link", "set", name, "up", NULL};
    if (!_run_argv(up_argv, error)) {
        if (*created_out)
            _rollback_bridge(name, owner_token, error);
        return FALSE;
    }
    if (cidr && *cidr) {
        const gchar *addr_argv[] = {"ip", "addr", "replace", cidr, "dev", name, NULL};
        if (!_run_argv(addr_argv, error)) {
            if (*created_out)
                _rollback_bridge(name, owner_token, error);
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean
_legacy_peer_matches(OverlayNet *net, const gchar *legacy_name,
                     const gchar *peer_ip, GError **error)
{
    gchar *port_interfaces_cond = NULL;
    if (!_port_interface_exact_condition(legacy_name,
                                         &port_interfaces_cond, error)) {
        g_free(port_interfaces_cond);
        return FALSE;
    }
    g_free(port_interfaces_cond);
    const gchar *bridge_argv[] = {"ovs-vsctl", "--bare", "iface-to-br", legacy_name, NULL};
    gchar *bridge_raw = NULL;
    if (!_run_argv_capture(bridge_argv, &bridge_raw, error)) {
        g_free(bridge_raw);
        return FALSE;
    }
    gchar *bridge = _ovs_scalar_normalize(bridge_raw);
    g_free(bridge_raw);
    if (g_strcmp0(bridge, net->name) != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "Legacy VXLAN port '%s' belongs to another bridge", legacy_name);
        g_free(bridge);
        return FALSE;
    }
    g_free(bridge);

    gboolean exists = FALSE;
    gchar *type = NULL;
    if (!_ovs_get_field("Interface", legacy_name, "type", &exists, &type,
                        error)) {
        g_free(type);
        return FALSE;
    }
    gboolean exact = exists && g_strcmp0(type, "vxlan") == 0;
    g_free(type);
    if (!exact) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "Legacy VXLAN port '%s' type mismatch", legacy_name);
        return FALSE;
    }
    return _interface_options_exact(net, legacy_name, peer_ip, error);
}

typedef struct {
    gboolean exists;
    gchar *port_name;
    gchar *type;
    gchar *options;
} OverlayPeerActualSnapshot;

typedef enum {
    OVERLAY_ACTUAL_UNCERTAIN = 0,
    OVERLAY_ACTUAL_EXACT,
    OVERLAY_ACTUAL_ABSENT,
} OverlayActualOutcome;

static void
_peer_actual_snapshot_clear(OverlayPeerActualSnapshot *snapshot)
{
    g_free(snapshot->port_name);
    g_free(snapshot->type);
    g_free(snapshot->options);
    memset(snapshot, 0, sizeof(*snapshot));
}

static gboolean
_peer_actual_snapshot(OverlayNet *net, const gchar *peer_ip,
                      OverlayPeerActualSnapshot *snapshot, GError **error)
{
    snapshot->port_name = pcv_overlay_peer_port_name(net->vni, peer_ip);
    if (!_interface_is_owned(net, snapshot->port_name, &snapshot->exists, error))
        return FALSE;
    if (!snapshot->exists)
        return TRUE;

    gboolean row_exists = FALSE;
    if (!_ovs_get_field("Interface", snapshot->port_name, "type", &row_exists,
                        &snapshot->type, error) || !row_exists)
        return FALSE;
    const gchar *argv[] = {
        "ovs-vsctl", "get", "Interface", snapshot->port_name, "options", NULL
    };
    gchar *raw = NULL;
    if (!_run_argv_capture(argv, &raw, error)) {
        g_free(raw);
        return FALSE;
    }
    snapshot->options = g_strdup(g_strstrip(raw));
    g_free(raw);
    if (!snapshot->options || !*snapshot->options) {
        g_free(snapshot->options);
        snapshot->options = g_strdup("{}");
    }
    return TRUE;
}





static OverlayActualOutcome
_peer_actual_outcome(OverlayNet *net, const gchar *peer_ip, GError **error)
{
    gchar *port = pcv_overlay_peer_port_name(net->vni, peer_ip);
    gboolean interface_exists = FALSE;
    if (!_ovs_row_exists("Interface", "name", port, &interface_exists, error)) {
        g_free(port);
        return OVERLAY_ACTUAL_UNCERTAIN;
    }
    if (!interface_exists) {
        gboolean port_exists = FALSE;
        gboolean ok = _ovs_row_exists("Port", "name", port, &port_exists,
                                      error);
        g_free(port);
        return ok && !port_exists ? OVERLAY_ACTUAL_ABSENT
                                  : OVERLAY_ACTUAL_UNCERTAIN;
    }

    gboolean owned = FALSE;
    gchar *membership = NULL;
    gboolean row_exists = FALSE;
    gchar *type = NULL;
    gboolean exact = _interface_is_owned(net, port, &owned, error) && owned &&
        _port_interface_exact_condition(port, &membership, error) &&
        _ovs_get_field("Interface", port, "type", &row_exists, &type,
                       error) && row_exists &&
        g_strcmp0(type, "vxlan") == 0 &&
        _interface_options_exact(net, port, peer_ip, error);
    g_free(type);
    g_free(membership);
    g_free(port);
    return exact ? OVERLAY_ACTUAL_EXACT : OVERLAY_ACTUAL_UNCERTAIN;
}

static gboolean
_peer_actual_restore(OverlayNet *net,
                     const OverlayPeerActualSnapshot *snapshot,
                     GError **error)
{
    if (!snapshot->exists)
        return TRUE;
    gchar *ports_cond = NULL;
    if (!_bridge_current_ports_condition(net->name, snapshot->port_name,
                                         &ports_cond, error))
        return FALSE;
    gchar *port_interfaces_cond = NULL;
    if (!_port_interface_exact_condition(snapshot->port_name,
                                         &port_interfaces_cond, error)) {
        g_free(ports_cond);
        return FALSE;
    }
    gchar *owner_cond = g_strdup_printf("external_ids:%s=%s", OVS_OWNER_KEY,
                                        net->owner_token);
    gchar *name_cond = g_strdup_printf("external_ids:%s=%s", OVS_NAME_KEY,
                                       net->name);
    gchar *type_arg = snapshot->type && *snapshot->type
        ? g_strdup_printf("type=%s", snapshot->type) : g_strdup("type=\"\"");
    gchar *options_arg = g_strdup_printf("options=%s", snapshot->options);
    const gchar *argv[] = {
        "ovs-vsctl", "--", "wait-until", "Bridge", net->name,
        owner_cond, name_cond, ports_cond,
        "--", "wait-until", "Port", snapshot->port_name,
        port_interfaces_cond,
        "--", "wait-until", "Interface", snapshot->port_name,
        owner_cond, name_cond,
        "--", "set", "Interface", snapshot->port_name,
        type_arg, options_arg, NULL
    };
    gboolean ok = _run_argv(argv, error);
    g_free(owner_cond); g_free(name_cond); g_free(ports_cond);
    g_free(port_interfaces_cond);
    g_free(type_arg); g_free(options_arg);
    return ok;
}

static gboolean
_ensure_peer_actual_locked(OverlayNet *net, const gchar *peer_ip,
                           gboolean *created_out, GError **error)
{
    gchar *port_name = pcv_overlay_peer_port_name(net->vni, peer_ip);
    gchar *legacy_name = _legacy_peer_port_name(peer_ip);
    gboolean exists = FALSE;
    *created_out = FALSE;
    if (!_interface_is_owned(net, port_name, &exists, error)) {
        g_free(port_name); g_free(legacy_name);
        return FALSE;
    }

    gchar *expected_options = _expected_options_map(net, peer_ip);
    gchar *options_opt = g_strdup_printf("options=%s", expected_options);
    g_free(expected_options);
    gchar *owner_opt = g_strdup_printf("external_ids:%s=%s", OVS_OWNER_KEY,
                                       net->owner_token);
    gchar *name_opt = g_strdup_printf("external_ids:%s=%s", OVS_NAME_KEY, net->name);
    gchar *bridge_owner_cond = g_strdup_printf("external_ids:%s=%s", OVS_OWNER_KEY,
                                               net->owner_token);
    gchar *bridge_name_cond = g_strdup_printf("external_ids:%s=%s", OVS_NAME_KEY,
                                              net->name);
    gchar *ports_cond = NULL;
    gchar *port_interfaces_cond = NULL;
    gboolean ok = FALSE;
    if (!_bridge_current_ports_condition(net->name,
                                         exists ? port_name : NULL,
                                         &ports_cond, error))
        goto out;
    if (exists) {
        gboolean row_exists = FALSE;
        gchar *type = NULL;
        if (!_ovs_get_field("Interface", port_name, "type", &row_exists,
                            &type, error)) {
            g_free(type);
            goto out;
        }
        gboolean actual_exact = row_exists &&
            g_strcmp0(type, "vxlan") == 0 &&
            _interface_options_exact(net, port_name, peer_ip, error);
        g_free(type);
        if (!actual_exact) {
            if (!error || !*error)
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                            "OVS Interface '%s' type/options drifted", port_name);
            goto out;
        }
        if (!_port_interface_exact_condition(port_name,
                                             &port_interfaces_cond, error))
            goto out;
        gchar *owner_cond = g_strdup_printf("external_ids:%s=%s", OVS_OWNER_KEY,
                                            net->owner_token);
        gchar *name_cond = g_strdup_printf("external_ids:%s=%s", OVS_NAME_KEY,
                                           net->name);
        const gchar *argv[] = {
            "ovs-vsctl", "--", "wait-until", "Bridge", net->name,
            bridge_owner_cond, bridge_name_cond, ports_cond,
            "--", "wait-until", "Port", port_name, port_interfaces_cond,
            "--", "wait-until", "Interface", port_name,
            owner_cond, name_cond, "type=vxlan", options_opt, NULL
        };
        ok = _run_argv(argv, error);
        g_free(owner_cond);
        g_free(name_cond);
    } else {
        gboolean legacy_exists = FALSE;
        gchar *legacy_owner = NULL;
        if (!_ovs_owner_probe("Interface", legacy_name, &legacy_exists,
                              &legacy_owner, error)) {
            g_free(legacy_owner);
            goto out;
        }
        if (legacy_exists) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                        "Legacy VXLAN port '%s' requires restart migration",
                        legacy_name);
            g_free(legacy_owner);
            goto out;
        } else {
            const gchar *add_argv[] = {
                "ovs-vsctl", "--", "wait-until", "Bridge", net->name,
                bridge_owner_cond, bridge_name_cond, ports_cond,
                "--", "add-port", net->name, port_name,
                "--", "set", "Interface", port_name, "type=vxlan",
                options_opt, owner_opt, name_opt, NULL
            };
            ok = _run_argv(add_argv, error);
            if (!ok) {




                OverlayCleanupDeadlineScope cleanup_scope =
                    _restore_cleanup_deadline_enter();
                GError *probe_error = NULL;
                gboolean after_exists = FALSE;
                gchar *after_owner = NULL;
                gchar *after_name = NULL;
                gboolean probe_ok = _ovs_owner_probe(
                    "Interface", port_name, &after_exists, &after_owner,
                    &probe_error);
                if (probe_ok && after_exists) {
                    gchar *column = g_strdup_printf("external_ids:%s",
                                                    OVS_NAME_KEY);
                    gboolean row_exists = FALSE;
                    probe_ok = _ovs_get_field(
                        "Interface", port_name, column, &row_exists,
                        &after_name, &probe_error) && row_exists;
                    g_free(column);
                }
                gboolean markers_exact = probe_ok && after_exists &&
                    g_strcmp0(after_owner, net->owner_token) == 0 &&
                    g_strcmp0(after_name, net->name) == 0;
                gboolean applied = FALSE;
                if (markers_exact) {
                    gboolean row_exists = FALSE;
                    gchar *type = NULL;
                    gchar *membership = NULL;
                    applied = _ovs_get_field(
                                  "Interface", port_name, "type",
                                  &row_exists, &type, &probe_error) &&
                              row_exists && g_strcmp0(type, "vxlan") == 0 &&
                              _interface_options_exact(net, port_name,
                                                       peer_ip,
                                                       &probe_error) &&
                              _port_interface_exact_condition(
                                  port_name, &membership, &probe_error);
                    g_free(type);
                    g_free(membership);
                }
                gboolean unchanged_foreign = probe_ok &&
                    (!after_exists ||
                     (!markers_exact &&
                      g_strcmp0(after_owner, net->owner_token) != 0 &&
                      g_strcmp0(after_name, net->name) != 0));
                if (applied)
                    *created_out = TRUE;
                else if (!unchanged_foreign)
                    _actual_outcome_uncertain_locked(
                        net->name, "peer create", error,
                        probe_error ? probe_error->message
                                    : "partial peer actual state");
                g_clear_error(&probe_error);
                g_free(after_owner);
                g_free(after_name);
                _restore_cleanup_deadline_leave(&cleanup_scope);
            }
        }
        g_free(legacy_owner);
        if (ok)
            *created_out = TRUE;
    }

out:
    g_free(options_opt);
    g_free(owner_opt); g_free(name_opt); g_free(bridge_owner_cond);
    g_free(bridge_name_cond); g_free(ports_cond); g_free(port_interfaces_cond);
    g_free(port_name); g_free(legacy_name);
    return ok;
}

static gboolean
_rollback_peer(OverlayNet *net, const gchar *peer_ip, GError **error)
{
    gchar *port_name = pcv_overlay_peer_port_name(net->vni, peer_ip);
    gchar *ports_cond = NULL;
    if (!_bridge_current_ports_condition(net->name, port_name,
                                         &ports_cond, error)) {
        g_free(port_name);
        return FALSE;
    }
    gchar *port_interfaces_cond = NULL;
    if (!_port_interface_exact_condition(port_name, &port_interfaces_cond,
                                         error)) {
        g_free(ports_cond);
        g_free(port_name);
        return FALSE;
    }
    gchar *owner_cond = g_strdup_printf("external_ids:%s=%s", OVS_OWNER_KEY,
                                        net->owner_token);
    gchar *name_cond = g_strdup_printf("external_ids:%s=%s", OVS_NAME_KEY,
                                       net->name);
    const gchar *argv[] = {
        "ovs-vsctl", "--", "wait-until", "Bridge", net->name,
        owner_cond, name_cond, ports_cond,
        "--", "wait-until", "Port", port_name, port_interfaces_cond,
        "--", "wait-until", "Interface", port_name, owner_cond, name_cond,
        "--", "--if-exists", "del-port", net->name, port_name, NULL
    };
    gboolean ok = _run_argv(argv, error);
    g_free(owner_cond);
    g_free(name_cond);
    g_free(ports_cond);
    g_free(port_interfaces_cond);
    g_free(port_name);
    return ok;
}

static gboolean
_rollback_peer_change(OverlayNet *net, const gchar *peer_ip,
                      gboolean created,
                      const OverlayPeerActualSnapshot *before,
                      GError **operation_error)
{
    OverlayCleanupDeadlineScope cleanup_scope =
        _restore_cleanup_deadline_enter();
    GError *rollback_error = NULL;
    gboolean ok = TRUE;
    if (created) {
        ok = _rollback_peer(net, peer_ip, &rollback_error);
        if (!ok) {
            GError *readback_error = NULL;
            if (_peer_actual_outcome(net, peer_ip, &readback_error) ==
                OVERLAY_ACTUAL_ABSENT) {
                g_clear_error(&rollback_error);
                ok = TRUE;
            }
            g_clear_error(&readback_error);
        }
    } else if (before->exists) {
        ok = _peer_actual_restore(net, before, &rollback_error);
    }
    if (ok) {
        _restore_cleanup_deadline_leave(&cleanup_scope);
        return TRUE;
    }
    _actual_rollback_failed_locked(net->name, "peer", operation_error,
                                   rollback_error);
    g_clear_error(&rollback_error);
    _restore_cleanup_deadline_leave(&cleanup_scope);
    return FALSE;
}

static gboolean
_rollback_created_actual(OverlayNet *net, gboolean bridge_created,
                         GPtrArray *created_peers, GError **operation_error)
{
    OverlayCleanupDeadlineScope cleanup_scope =
        _restore_cleanup_deadline_enter();
    GError *rollback_error = NULL;
    for (gint i = (gint)created_peers->len - 1; i >= 0; i--) {
        const gchar *peer = g_ptr_array_index(created_peers, (guint)i);
        if (!_rollback_peer(net, peer, &rollback_error)) {
            GError *readback_error = NULL;
            if (_peer_actual_outcome(net, peer, &readback_error) ==
                OVERLAY_ACTUAL_ABSENT) {
                g_clear_error(&rollback_error);
                g_clear_error(&readback_error);
                continue;
            }
            g_clear_error(&readback_error);
            break;
        }
    }
    if (!rollback_error && bridge_created &&
        !_delete_owned_empty_bridge(net->name, net->owner_token,
                                    &rollback_error)) {

    }
    if (!rollback_error) {
        _restore_cleanup_deadline_leave(&cleanup_scope);
        return TRUE;
    }

    gchar *primary = operation_error && *operation_error
        ? g_strdup((*operation_error)->message) : g_strdup("overlay operation failed");
    if (operation_error)
        g_clear_error(operation_error);
    g_set_error(operation_error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "%s; rollback left owned actual residue: %s", primary,
                rollback_error->message);
    PCV_LOG_WARN(OVERLAY_LOG_DOM,
                 "overlay actual rollback residue for '%s': %s", net->name,
                 rollback_error->message);
    _record_metadata_residue_locked(
        "overlay actual rollback left owned-state residue");
    g_free(primary);
    g_clear_error(&rollback_error);
    _restore_cleanup_deadline_leave(&cleanup_scope);
    return FALSE;
}







static gboolean
_restore_overlay_actual_locked(OverlayNet *net, gboolean *bridge_created_out,
                               GPtrArray **created_peers_out, GError **error)
{
    gboolean bridge_created = FALSE;
    GPtrArray *created_peers = g_ptr_array_new_with_free_func(g_free);
    OverlayBulkSnapshot *bulk = _restore_bulk_get();
    gboolean owns_bulk = FALSE;
    if (!bulk) {




        bulk = _bulk_snapshot_load(error);
        if (!bulk) {
            g_ptr_array_free(created_peers, TRUE);
            return FALSE;
        }





        if (g_private_get(&G_restore_deadline))
            _restore_bulk_replace(bulk);
        else
            owns_bulk = TRUE;
    }
    if (bulk) {
        gboolean missing = FALSE;
        gboolean exact = _bulk_net_exact(bulk, net, &missing, error);
        if (owns_bulk)
            _bulk_snapshot_free(bulk);
        if (exact) {
            net->active = TRUE;
            *bridge_created_out = FALSE;
            *created_peers_out = created_peers;
            return TRUE;
        }
        if (!missing) {
            g_ptr_array_free(created_peers, TRUE);
            return FALSE;
        }
        g_clear_error(error);
        _restore_bulk_replace(NULL);
    }
    if (!_validate_owned_actual_inventory_locked(net, FALSE, TRUE, error)) {
        g_ptr_array_free(created_peers, TRUE);
        return FALSE;
    }
    if (!_ensure_bridge_actual_locked(net->name, net->cidr, net->owner_token,
                                      &bridge_created, error)) {
        g_ptr_array_free(created_peers, TRUE);
        return FALSE;
    }
    for (guint i = 0; i < net->peers->len; i++) {
        const gchar *peer = g_ptr_array_index(net->peers, i);
        gboolean peer_created = FALSE;
        if (!_ensure_peer_actual_locked(net, peer, &peer_created, error)) {
            if (peer_created)
                g_ptr_array_add(created_peers, g_strdup(peer));
            _rollback_created_actual(net, bridge_created, created_peers, error);
            g_ptr_array_free(created_peers, TRUE);
            return FALSE;
        }
        if (peer_created)
            g_ptr_array_add(created_peers, g_strdup(peer));
    }
    if (!_validate_owned_actual_inventory_locked(net, TRUE, TRUE, error)) {
        _rollback_created_actual(net, bridge_created, created_peers, error);
        g_ptr_array_free(created_peers, TRUE);
        return FALSE;
    }
    net->active = TRUE;
    *bridge_created_out = bridge_created;
    *created_peers_out = created_peers;
    return TRUE;
}

static GPtrArray *
_bridge_ports(const gchar *bridge, GError **error)
{
    const gchar *argv[] = {"ovs-vsctl", "--bare", "list-ports", bridge, NULL};
    gchar *output = NULL;
    if (!_run_argv_capture(argv, &output, error)) {
        g_free(output);
        return NULL;
    }
    GPtrArray *ports = g_ptr_array_new_with_free_func(g_free);
    gchar **lines = g_strsplit(output ? output : "", "\n", -1);
    for (gint i = 0; lines[i]; i++) {
        gchar *line = g_strstrip(lines[i]);
        if (*line)
            g_ptr_array_add(ports, g_strdup(line));
    }
    g_strfreev(lines);
    g_free(output);
    return ports;
}

static gboolean
_ptr_array_has_string(GPtrArray *values, const gchar *needle)
{
    for (guint i = 0; i < values->len; i++)
        if (g_strcmp0(g_ptr_array_index(values, i), needle) == 0)
            return TRUE;
    return FALSE;
}




static gboolean
_validate_legacy_actual_locked(OverlayNet *net, gboolean *bridge_exists_out,
                               GError **error)
{
    gboolean bridge_exists = FALSE;
    gchar *bridge_owner = NULL;
    if (!_ovs_owner_probe("Bridge", net->name, &bridge_exists, &bridge_owner, error))
        return FALSE;
    *bridge_exists_out = bridge_exists;
    if (!bridge_exists) {
        g_free(bridge_owner);
        return TRUE;
    }
    gchar *name_column = g_strdup_printf("external_ids:%s", OVS_NAME_KEY);
    gboolean ignored_exists = FALSE;
    gchar *bridge_name = NULL;
    if (!_ovs_get_field("Bridge", net->name, name_column, &ignored_exists,
                        &bridge_name, error)) {
        g_free(name_column); g_free(bridge_owner);
        return FALSE;
    }
    g_free(name_column);
    if ((bridge_owner && *bridge_owner) || (bridge_name && *bridge_name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "Legacy bridge '%s' already has ownership metadata", net->name);
        g_free(bridge_owner); g_free(bridge_name);
        return FALSE;
    }
    g_free(bridge_owner); g_free(bridge_name);
    gboolean bridge_row_exists = FALSE;
    gchar *datapath_type = NULL;
    gchar *legacy_owner = NULL;
    if (!_ovs_get_field("Bridge", net->name, "datapath_type",
                        &bridge_row_exists, &datapath_type, error) ||
        !_ovs_get_field("Bridge", net->name,
                        "external_ids:purecvisor-owner",
                        &bridge_row_exists, &legacy_owner, error)) {
        g_free(datapath_type); g_free(legacy_owner);
        return FALSE;
    }
    gboolean bridge_shape_safe =
        (!datapath_type || !*datapath_type ||
         g_strcmp0(datapath_type, "system") == 0) &&
        (!legacy_owner || !*legacy_owner);
    if (!bridge_shape_safe)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "Legacy bridge '%s' belongs to another PureCVisor datapath",
                    net->name);
    g_free(datapath_type); g_free(legacy_owner);
    if (!bridge_shape_safe)
        return FALSE;
    OverlayBulkSnapshot *bulk = _restore_bulk_get();
    gboolean owns_bulk = FALSE;
    if (!bulk) {
        bulk = _bulk_snapshot_load(error);
        owns_bulk = TRUE;
    }
    gboolean topology_exact = bulk &&
        _bulk_legacy_relationships_exact(bulk, net, error);
    if (owns_bulk)
        _bulk_snapshot_free(bulk);
    if (!topology_exact)
        return FALSE;
    if (!_legacy_bridge_matches(net->name, net->cidr, error))
        return FALSE;

    GPtrArray *expected = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < net->peers->len; i++) {
        const gchar *peer = g_ptr_array_index(net->peers, i);
        gchar *legacy = _legacy_peer_port_name(peer);
        gchar *canonical = pcv_overlay_peer_port_name(net->vni, peer);
        if (_ptr_array_has_string(expected, legacy)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                        "Legacy peer names collide at '%s'", legacy);
            g_free(legacy); g_free(canonical);
            g_ptr_array_free(expected, TRUE);
            return FALSE;
        }
        gboolean canonical_exists = FALSE;
        if (!_ovs_row_exists("Interface", "name", canonical,
                             &canonical_exists, error)) {
            g_free(legacy); g_free(canonical);
            g_ptr_array_free(expected, TRUE);
            return FALSE;
        }
        if (canonical_exists) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                        "Canonical VXLAN interface '%s' already exists", canonical);
            g_free(legacy); g_free(canonical);
            g_ptr_array_free(expected, TRUE);
            return FALSE;
        }
        gboolean legacy_exists = FALSE;
        gchar *legacy_owner = NULL;
        if (!_ovs_owner_probe("Interface", legacy, &legacy_exists,
                              &legacy_owner, error) || !legacy_exists ||
            (legacy_owner && *legacy_owner)) {
            if (!error || !*error)
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                            "Legacy VXLAN interface '%s' is absent or owned", legacy);
            g_free(legacy_owner); g_free(legacy); g_free(canonical);
            g_ptr_array_free(expected, TRUE);
            return FALSE;
        }
        g_free(legacy_owner);
        gchar *other_owner = NULL;
        if (!_ovs_get_field("Interface", legacy,
                            "external_ids:purecvisor-owner",
                            &ignored_exists, &other_owner, error) ||
            (other_owner && *other_owner)) {
            if (!error || !*error)
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                            "Legacy VXLAN interface '%s' belongs to another product owner",
                            legacy);
            g_free(other_owner); g_free(legacy); g_free(canonical);
            g_ptr_array_free(expected, TRUE);
            return FALSE;
        }
        g_free(other_owner);
        gchar *legacy_name = NULL;
        name_column = g_strdup_printf("external_ids:%s", OVS_NAME_KEY);
        if (!_ovs_get_field("Interface", legacy, name_column, &ignored_exists,
                            &legacy_name, error)) {
            g_free(name_column); g_free(legacy_name); g_free(legacy);
            g_free(canonical); g_ptr_array_free(expected, TRUE);
            return FALSE;
        }
        g_free(name_column);
        if ((legacy_name && *legacy_name) ||
            !_legacy_peer_matches(net, legacy, peer, error)) {
            if (!error || !*error)
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                            "Legacy VXLAN interface '%s' is not exact", legacy);
            g_free(legacy_name); g_free(legacy); g_free(canonical);
            g_ptr_array_free(expected, TRUE);
            return FALSE;
        }
        g_free(legacy_name);
        g_ptr_array_add(expected, legacy);
        g_free(canonical);
    }

    GPtrArray *actual = _bridge_ports(net->name, error);
    if (!actual) {
        g_ptr_array_free(expected, TRUE);
        return FALSE;
    }
    gboolean exact = actual->len == expected->len;
    for (guint i = 0; exact && i < actual->len; i++)
        exact = _ptr_array_has_string(expected, g_ptr_array_index(actual, i));
    if (!exact)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "Legacy bridge '%s' contains non-metadata child ports", net->name);
    g_ptr_array_free(actual, TRUE);
    g_ptr_array_free(expected, TRUE);
    return exact;
}

static void
_append_peer_set_args(GPtrArray *argv, OverlayNet *net, const gchar *port,
                      const gchar *peer, gboolean include_owner)
{
    gchar *options = _expected_options_map(net, peer);
    gchar *options_arg = g_strdup_printf("options=%s", options);
    _argv_add(argv, "--"); _argv_add(argv, "set"); _argv_add(argv, "Interface");
    _argv_add(argv, port); _argv_add(argv, "type=vxlan");
    _argv_add(argv, options_arg);
    if (include_owner) {
        gchar *owner = g_strdup_printf("external_ids:%s=%s", OVS_OWNER_KEY,
                                       net->owner_token);
        gchar *name = g_strdup_printf("external_ids:%s=%s", OVS_NAME_KEY,
                                      net->name);
        _argv_add(argv, owner); _argv_add(argv, name);
        g_free(owner); g_free(name);
    }
    g_free(options);
    g_free(options_arg);
}

static gboolean _rollback_legacy_migration(OverlayNet *net,
                                           GError **operation_error);

static gboolean
_validate_migrated_actual_locked(OverlayNet *net, GError **error)
{
    gboolean bridge_exists = FALSE;
    if (!_bridge_is_owned(net->name, net->owner_token, &bridge_exists, error) ||
        !bridge_exists) {
        if (!error || !*error)
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "Migrated bridge '%s' is absent", net->name);
        return FALSE;
    }
    gboolean bridge_row_exists = FALSE;
    gchar *datapath_type = NULL;
    gchar *legacy_owner = NULL;
    if (!_ovs_get_field("Bridge", net->name, "datapath_type",
                        &bridge_row_exists, &datapath_type, error) ||
        !bridge_row_exists ||
        !_ovs_get_field("Bridge", net->name,
                        "external_ids:purecvisor-owner",
                        &bridge_row_exists, &legacy_owner, error)) {
        g_free(datapath_type);
        g_free(legacy_owner);
        return FALSE;
    }
    gboolean bridge_namespace_exact =
        (!datapath_type || !*datapath_type ||
         g_strcmp0(datapath_type, "system") == 0) &&
        (!legacy_owner || !*legacy_owner);
    g_free(datapath_type);
    g_free(legacy_owner);
    if (!bridge_namespace_exact) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "Migrated bridge '%s' changed datapath/legacy ownership",
                    net->name);
        return FALSE;
    }
    if (!_legacy_bridge_matches(net->name, net->cidr, error))
        return FALSE;

    GPtrArray *ports = _bridge_ports(net->name, error);
    if (!ports)
        return FALSE;
    if (ports->len != net->peers->len) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "Migrated bridge '%s' child set changed", net->name);
        g_ptr_array_free(ports, TRUE);
        return FALSE;
    }
    for (guint i = 0; i < net->peers->len; i++) {
        const gchar *peer = g_ptr_array_index(net->peers, i);
        gchar *port = pcv_overlay_peer_port_name(net->vni, peer);
        gboolean exists = FALSE;
        if (!_ptr_array_has_string(ports, port) ||
            !_interface_is_owned(net, port, &exists, error) || !exists) {
            if ((!error || !*error) && !_ptr_array_has_string(ports, port))
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                            "Migrated peer '%s' is not in the exact child set", port);
            g_free(port);
            g_ptr_array_free(ports, TRUE);
            return FALSE;
        }
        gchar *port_interfaces_cond = NULL;
        if (!_port_interface_exact_condition(port, &port_interfaces_cond,
                                             error)) {
            g_free(port_interfaces_cond);
            g_free(port);
            g_ptr_array_free(ports, TRUE);
            return FALSE;
        }
        g_free(port_interfaces_cond);
        gchar *peer_legacy_owner = NULL;
        gboolean row_exists = FALSE;
        if (!_ovs_get_field("Interface", port,
                            "external_ids:purecvisor-owner", &row_exists,
                            &peer_legacy_owner, error) ||
            (peer_legacy_owner && *peer_legacy_owner)) {
            if (!error || !*error)
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                            "Migrated peer '%s' acquired foreign ownership",
                            port);
            g_free(peer_legacy_owner);
            g_free(port);
            g_ptr_array_free(ports, TRUE);
            return FALSE;
        }
        g_free(peer_legacy_owner);
        row_exists = FALSE;
        gchar *type = NULL;
        if (!_ovs_get_field("Interface", port, "type", &row_exists,
                            &type, error) || !row_exists ||
            g_strcmp0(type, "vxlan") != 0 ||
            !_interface_options_exact(net, port, peer, error)) {
            if (!error || !*error)
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Migrated peer '%s' is not exact", port);
            g_free(type); g_free(port);
            g_ptr_array_free(ports, TRUE);
            return FALSE;
        }
        g_free(type);
        g_free(port);
    }
    g_ptr_array_free(ports, TRUE);
    return TRUE;
}

static gboolean
_migrate_legacy_actual_locked(OverlayNet *net, GError **error)
{
    gboolean bridge_exists = FALSE;
    if (!_validate_legacy_actual_locked(net, &bridge_exists, error) ||
        !bridge_exists)
        return FALSE;

    GPtrArray *legacy_children = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < net->peers->len; i++)
        g_ptr_array_add(legacy_children,
                        _legacy_peer_port_name(g_ptr_array_index(net->peers, i)));
    gchar *ports_cond = NULL;
    if (!_bridge_ports_exact_condition(net->name, legacy_children,
                                       &ports_cond, error)) {
        g_ptr_array_free(legacy_children, TRUE);
        return FALSE;
    }
    g_ptr_array_free(legacy_children, TRUE);





    gboolean bridge_row_exists = FALSE;
    gchar *datapath_type = NULL;
    gchar *legacy_bridge_owner = NULL;
    if (!_ovs_get_field("Bridge", net->name, "datapath_type",
                        &bridge_row_exists, &datapath_type, error) ||
        !bridge_row_exists ||
        !_ovs_get_field("Bridge", net->name,
                        "external_ids:purecvisor-owner", &bridge_row_exists,
                        &legacy_bridge_owner, error)) {
        g_free(datapath_type);
        g_free(legacy_bridge_owner);
        g_free(ports_cond);
        return FALSE;
    }
    gboolean legacy_namespace_exact =
        (!datapath_type || !*datapath_type ||
         g_strcmp0(datapath_type, "system") == 0) &&
        (!legacy_bridge_owner || !*legacy_bridge_owner);
    if (!legacy_namespace_exact) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "Legacy bridge '%s' datapath/owner changed before migration",
                    net->name);
        g_free(datapath_type);
        g_free(legacy_bridge_owner);
        g_free(ports_cond);
        return FALSE;
    }
    gchar *datapath_cond = datapath_type && *datapath_type
        ? g_strdup_printf("datapath_type=%s", datapath_type)
        : g_strdup("datapath_type=[]");
    gchar *empty_legacy_owner =
        g_strdup("external_ids:purecvisor-owner=[]");
    g_free(datapath_type);
    g_free(legacy_bridge_owner);

    GPtrArray *legacy_port_conditions =
        g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < net->peers->len; i++) {
        gchar *legacy =
            _legacy_peer_port_name(g_ptr_array_index(net->peers, i));
        gchar *condition = NULL;
        gboolean exact = _port_interface_exact_condition(
            legacy, &condition, error);
        g_free(legacy);
        if (!exact) {
            g_ptr_array_free(legacy_port_conditions, TRUE);
            g_free(datapath_cond);
            g_free(empty_legacy_owner);
            g_free(ports_cond);
            return FALSE;
        }
        g_ptr_array_add(legacy_port_conditions, condition);
    }

    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    _argv_add(argv, "ovs-vsctl");
    gchar *empty_owner = g_strdup_printf("external_ids:%s=[]", OVS_OWNER_KEY);
    gchar *empty_name = g_strdup_printf("external_ids:%s=[]", OVS_NAME_KEY);
    gchar *owner = g_strdup_printf("external_ids:%s=%s", OVS_OWNER_KEY,
                                   net->owner_token);
    gchar *name = g_strdup_printf("external_ids:%s=%s", OVS_NAME_KEY, net->name);
    _argv_add(argv, "--"); _argv_add(argv, "wait-until"); _argv_add(argv, "Bridge");
    _argv_add(argv, net->name); _argv_add(argv, empty_owner); _argv_add(argv, empty_name);
    _argv_add(argv, datapath_cond); _argv_add(argv, empty_legacy_owner);
    _argv_add(argv, ports_cond);
    _argv_add(argv, "--"); _argv_add(argv, "set"); _argv_add(argv, "Bridge");
    _argv_add(argv, net->name); _argv_add(argv, owner); _argv_add(argv, name);
    for (guint i = 0; i < net->peers->len; i++) {
        const gchar *peer = g_ptr_array_index(net->peers, i);
        gchar *legacy = _legacy_peer_port_name(peer);
        gchar *canonical = pcv_overlay_peer_port_name(net->vni, peer);
        gchar *options = _expected_options_map(net, peer);
        gchar *options_cond = g_strdup_printf("options=%s", options);
        const gchar *port_interfaces_cond =
            g_ptr_array_index(legacy_port_conditions, i);
        _argv_add(argv, "--"); _argv_add(argv, "wait-until");
        _argv_add(argv, "Interface"); _argv_add(argv, legacy);
        _argv_add(argv, "type=vxlan"); _argv_add(argv, options_cond);
        _argv_add(argv, empty_owner); _argv_add(argv, empty_name);
        _argv_add(argv, empty_legacy_owner);
        _argv_add(argv, "--"); _argv_add(argv, "wait-until");
        _argv_add(argv, "Port"); _argv_add(argv, legacy);
        _argv_add(argv, port_interfaces_cond);
        _argv_add(argv, "--"); _argv_add(argv, "add-port");
        _argv_add(argv, net->name); _argv_add(argv, canonical);
        _append_peer_set_args(argv, net, canonical, peer, TRUE);
        _argv_add(argv, "--"); _argv_add(argv, "del-port");
        _argv_add(argv, net->name); _argv_add(argv, legacy);
        g_free(options); g_free(options_cond);
        g_free(legacy); g_free(canonical);
    }
    gboolean ok = _run_dynamic_argv(argv, error);
    g_ptr_array_free(argv, TRUE);
    g_ptr_array_free(legacy_port_conditions, TRUE);
    g_free(empty_owner); g_free(empty_name); g_free(owner); g_free(name);
    g_free(datapath_cond); g_free(empty_legacy_owner);
    g_free(ports_cond);
    if (!ok) {
        OverlayCleanupDeadlineScope cleanup_scope =
            _restore_cleanup_deadline_enter();
        GError *readback_error = NULL;
        gboolean applied = _validate_migrated_actual_locked(
            net, &readback_error);
        if (applied) {


            g_clear_error(error);
            g_clear_error(&readback_error);
            ok = TRUE;
        } else {
            g_clear_error(&readback_error);
            gboolean bridge_exists = FALSE;
            gboolean unchanged = _validate_legacy_actual_locked(
                net, &bridge_exists, &readback_error) && bridge_exists;
            if (!unchanged)
                _actual_outcome_uncertain_locked(
                    net->name, "legacy migration", error,
                    readback_error ? readback_error->message
                                   : "neither legacy nor migrated state is exact");
            g_clear_error(&readback_error);
        }
        _restore_cleanup_deadline_leave(&cleanup_scope);
        if (!ok)
            return FALSE;
    }



    if (!_validate_migrated_actual_locked(net, error)) {
        _rollback_legacy_migration(net, error);
        return FALSE;
    }





    return TRUE;
}

static gboolean
_rollback_legacy_migration(OverlayNet *net, GError **operation_error)
{
    OverlayCleanupDeadlineScope cleanup_scope =
        _restore_cleanup_deadline_enter();
    GError *rollback_error = NULL;
    GPtrArray *canonical_children = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < net->peers->len; i++)
        g_ptr_array_add(canonical_children,
                        pcv_overlay_peer_port_name(
                            net->vni, g_ptr_array_index(net->peers, i)));
    gchar *ports_cond = NULL;
    if (!_bridge_ports_exact_condition(net->name, canonical_children,
                                       &ports_cond, &rollback_error)) {
        g_ptr_array_free(canonical_children, TRUE);
        _actual_rollback_failed_locked(net->name, "legacy migration",
                                       operation_error, rollback_error);
        g_clear_error(&rollback_error);
        _restore_cleanup_deadline_leave(&cleanup_scope);
        return FALSE;
    }
    g_ptr_array_free(canonical_children, TRUE);

    GPtrArray *canonical_port_conditions =
        g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < net->peers->len; i++) {
        gchar *canonical = pcv_overlay_peer_port_name(
            net->vni, g_ptr_array_index(net->peers, i));
        gchar *condition = NULL;
        gboolean exact = _port_interface_exact_condition(
            canonical, &condition, &rollback_error);
        g_free(canonical);
        if (!exact) {
            g_ptr_array_free(canonical_port_conditions, TRUE);
            g_free(ports_cond);
            _actual_rollback_failed_locked(net->name, "legacy migration",
                                           operation_error, rollback_error);
            g_clear_error(&rollback_error);
            _restore_cleanup_deadline_leave(&cleanup_scope);
            return FALSE;
        }
        g_ptr_array_add(canonical_port_conditions, condition);
    }

    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    _argv_add(argv, "ovs-vsctl");
    gchar *owner_cond = g_strdup_printf("external_ids:%s=%s", OVS_OWNER_KEY,
                                        net->owner_token);
    gchar *name_cond = g_strdup_printf("external_ids:%s=%s", OVS_NAME_KEY,
                                       net->name);
    _argv_add(argv, "--"); _argv_add(argv, "wait-until"); _argv_add(argv, "Bridge");
    _argv_add(argv, net->name); _argv_add(argv, owner_cond); _argv_add(argv, name_cond);
    _argv_add(argv, ports_cond);
    _argv_add(argv, "--"); _argv_add(argv, "remove"); _argv_add(argv, "Bridge");
    _argv_add(argv, net->name); _argv_add(argv, "external_ids"); _argv_add(argv, OVS_OWNER_KEY);
    _argv_add(argv, "--"); _argv_add(argv, "remove"); _argv_add(argv, "Bridge");
    _argv_add(argv, net->name); _argv_add(argv, "external_ids"); _argv_add(argv, OVS_NAME_KEY);
    for (guint i = 0; i < net->peers->len; i++) {
        const gchar *peer = g_ptr_array_index(net->peers, i);
        gchar *legacy = _legacy_peer_port_name(peer);
        gchar *canonical = pcv_overlay_peer_port_name(net->vni, peer);
        const gchar *port_interfaces_cond =
            g_ptr_array_index(canonical_port_conditions, i);
        _argv_add(argv, "--"); _argv_add(argv, "wait-until");
        _argv_add(argv, "Interface"); _argv_add(argv, canonical);
        _argv_add(argv, owner_cond); _argv_add(argv, name_cond);
        _argv_add(argv, "--"); _argv_add(argv, "wait-until");
        _argv_add(argv, "Port"); _argv_add(argv, canonical);
        _argv_add(argv, port_interfaces_cond);
        _argv_add(argv, "--"); _argv_add(argv, "add-port");
        _argv_add(argv, net->name); _argv_add(argv, legacy);
        _append_peer_set_args(argv, net, legacy, peer, FALSE);
        _argv_add(argv, "--"); _argv_add(argv, "del-port");
        _argv_add(argv, net->name); _argv_add(argv, canonical);
        g_free(legacy); g_free(canonical);
    }
    gboolean ok = _run_dynamic_argv(argv, &rollback_error);
    g_ptr_array_free(argv, TRUE);
    g_ptr_array_free(canonical_port_conditions, TRUE);
    g_free(owner_cond);
    g_free(name_cond);
    g_free(ports_cond);
    if (!ok) {
        GError *readback_error = NULL;
        gboolean bridge_exists = FALSE;
        gboolean restored = _validate_legacy_actual_locked(
            net, &bridge_exists, &readback_error) && bridge_exists;
        if (restored) {
            g_clear_error(&rollback_error);
            g_clear_error(&readback_error);
            _restore_cleanup_deadline_leave(&cleanup_scope);
            return TRUE;
        }
        g_clear_error(&readback_error);
        _actual_rollback_failed_locked(net->name, "legacy migration",
                                       operation_error, rollback_error);
        g_clear_error(&rollback_error);
        _restore_cleanup_deadline_leave(&cleanup_scope);
        return FALSE;
    }
    _restore_cleanup_deadline_leave(&cleanup_scope);
    return TRUE;
}

static gboolean
_audit_marker_table_locked(const gchar *table, GError **error)
{
    const gchar *argv[] = {
        "ovs-vsctl", "--format=json", "--columns=name,external_ids",
        "list", table, NULL
    };
    gchar *output = NULL;
    if (!_run_argv_capture(argv, &output, error)) {
        g_free(output);
        return FALSE;
    }
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, output ? output : "", -1, error)) {
        g_object_unref(parser); g_free(output);
        return FALSE;
    }
    JsonNode *root_node = json_parser_get_root(parser);
    if (!root_node || !JSON_NODE_HOLDS_OBJECT(root_node))
        goto malformed;
    JsonObject *root = json_node_get_object(root_node);
    JsonArray *headings = json_object_get_array_member(root, "headings");
    JsonArray *data = json_object_get_array_member(root, "data");
    if (!headings || !data)
        goto malformed;
    gint name_index = -1;
    gint ids_index = -1;
    for (guint i = 0; i < json_array_get_length(headings); i++) {
        const gchar *heading =
            _overlay_json_string(json_array_get_element(headings, i));
        if (g_strcmp0(heading, "name") == 0)
            name_index = (gint)i;
        else if (g_strcmp0(heading, "external_ids") == 0)
            ids_index = (gint)i;
    }
    if (name_index < 0 || ids_index < 0)
        goto malformed;

    for (guint r = 0; r < json_array_get_length(data); r++) {
        JsonNode *row_node = json_array_get_element(data, r);
        if (!row_node || !JSON_NODE_HOLDS_ARRAY(row_node))
            goto malformed;
        JsonArray *row = json_node_get_array(row_node);
        if ((guint)MAX(name_index, ids_index) >= json_array_get_length(row))
            goto malformed;
        const gchar *row_name =
            _overlay_json_string(json_array_get_element(row, (guint)name_index));
        JsonNode *ids_node = json_array_get_element(row, (guint)ids_index);
        if (!row_name || !ids_node || !JSON_NODE_HOLDS_ARRAY(ids_node))
            goto malformed;
        JsonArray *ids = json_node_get_array(ids_node);
        if (json_array_get_length(ids) != 2 ||
            g_strcmp0(_overlay_json_string(json_array_get_element(ids, 0)),
                      "map") != 0)
            goto malformed;
        JsonNode *pairs_node = json_array_get_element(ids, 1);
        if (!pairs_node || !JSON_NODE_HOLDS_ARRAY(pairs_node))
            goto malformed;
        JsonArray *pairs = json_node_get_array(pairs_node);
        const gchar *owner = NULL;
        const gchar *overlay_name = NULL;
        for (guint p = 0; p < json_array_get_length(pairs); p++) {
            JsonNode *pair_node = json_array_get_element(pairs, p);
            if (!pair_node || !JSON_NODE_HOLDS_ARRAY(pair_node))
                goto malformed;
            JsonArray *pair = json_node_get_array(pair_node);
            if (json_array_get_length(pair) != 2)
                goto malformed;
            const gchar *key =
                _overlay_json_string(json_array_get_element(pair, 0));
            const gchar *value =
                _overlay_json_string(json_array_get_element(pair, 1));
            if (!key || !value)
                goto malformed;
            if (g_strcmp0(key, OVS_OWNER_KEY) == 0)
                owner = value;
            else if (g_strcmp0(key, OVS_NAME_KEY) == 0)
                overlay_name = value;
        }
        if ((!owner || !*owner) && (!overlay_name || !*overlay_name))
            continue;

        OverlayNet *owned = NULL;
        for (gint i = 0; i < G.count; i++) {
            if (g_strcmp0(owner, G.nets[i].owner_token) == 0 &&
                g_strcmp0(overlay_name, G.nets[i].name) == 0) {
                owned = &G.nets[i];
                break;
            }
        }
        if (!owned || (g_strcmp0(table, "Bridge") == 0 &&
                       g_strcmp0(row_name, owned->name) != 0)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Unregistered or partial overlay ownership marker on %s '%s'",
                        table, row_name);
            g_object_unref(parser); g_free(output);
            return FALSE;
        }
    }
    g_object_unref(parser);
    g_free(output);
    return TRUE;

malformed:
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Malformed ovs-vsctl %s ownership inventory", table);
    g_object_unref(parser);
    g_free(output);
    return FALSE;
}

static gboolean
_audit_all_owned_actual_locked(GError **error)
{
    OverlayBulkSnapshot *bulk = _restore_bulk_get();
    if (bulk) {
        if (!_bulk_global_exact(bulk, error))
            return FALSE;
        for (gint i = 0; i < G.count; i++) {
            gboolean missing = FALSE;
            if (!_bulk_net_exact(bulk, &G.nets[i],
                                 &missing, error))
                return FALSE;
        }
        return TRUE;
    }
    if (!_audit_marker_table_locked("Bridge", error) ||
        !_audit_marker_table_locked("Interface", error))
        return FALSE;
    for (gint i = 0; i < G.count; i++)
        if (!_validate_owned_actual_inventory_locked(&G.nets[i], TRUE, TRUE,
                                                     error))
            return FALSE;
    return TRUE;
}

static gboolean
_overlay_meta_matches_net(OverlayMeta *meta, OverlayNet *net)
{
    if (meta->legacy || g_strcmp0(meta->name, net->name) != 0 ||
        g_strcmp0(meta->cidr, net->cidr) != 0 || meta->vni != net->vni ||
        g_strcmp0(meta->owner_token, net->owner_token) != 0 ||
        meta->generation != net->generation ||
        meta->peers->len != net->peers->len)
        return FALSE;
    for (guint i = 0; i < net->peers->len; i++)
        if (g_strcmp0(g_ptr_array_index(meta->peers, i),
                      g_ptr_array_index(net->peers, i)) != 0)
            return FALSE;
    return TRUE;
}

static void
_overlay_meta_snapshot_clear(OverlayMetaSnapshot *snapshot)
{
    g_free(snapshot->data);
    memset(snapshot, 0, sizeof(*snapshot));
}







                                                        
                                                     

static gboolean
_canonical_meta_snapshot(OverlayNet *net, OverlayMetaSnapshot *snapshot,
                         GError **error)
{
    if (!_metadata_inventory_exact(net->name, TRUE, error))
        return FALSE;
    gchar *path = _overlay_meta_path(net->name);
    gchar *tomb = _overlay_tombstone_path(net->name);
    gchar *update = _overlay_update_path(net->name);
    if (_path_entry_exists(tomb) || _path_entry_exists(update)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "Overlay '%s' has metadata transaction residue", net->name);
        g_free(path); g_free(tomb); g_free(update);
        return FALSE;
    }
    g_free(tomb);
    g_free(update);
    OverlayMeta parsed = {0};
    GError *local_error = NULL;
    gboolean loaded = _safe_read_metadata(path, &snapshot->data, &snapshot->len,
                                          &local_error);
    gboolean valid = loaded &&
        _overlay_meta_parse(snapshot->data, (gssize)snapshot->len, &parsed,
                            &local_error);
    if (valid && !_overlay_meta_matches_net(&parsed, net)) {
        g_set_error(&local_error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "Canonical metadata for overlay '%s' does not match memory",
                    net->name);
        valid = FALSE;
    }
    if (!valid) {
        _overlay_meta_snapshot_clear(snapshot);
        g_propagate_error(error, local_error);
    }
    _overlay_meta_clear(&parsed);
    g_free(path);
    return valid;
}





static gboolean
_audit_registry_metadata_locked(GError **error)
{
    gint dir_fd = _open_owned_directory(_overlay_meta_dir(), error);
    if (dir_fd < 0)
        return FALSE;
    struct stat before = {0};
    if (fstat(dir_fd, &before) != 0) {
        gint saved_errno = errno;
        close(dir_fd);
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Cannot stat overlay metadata directory: %s",
                    g_strerror(saved_errno));
        return FALSE;
    }
    gint scan_fd = dup(dir_fd);
    DIR *dir = scan_fd >= 0 ? fdopendir(scan_fd) : NULL;
    if (!dir) {
        gint saved_errno = errno;
        if (scan_fd >= 0)
            close(scan_fd);
        close(dir_fd);
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Cannot enumerate overlay metadata directory: %s",
                    g_strerror(saved_errno));
        return FALSE;
    }

    GHashTable *canonical_names =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gboolean exact = TRUE;
    struct dirent *entry = NULL;
    while (exact && (entry = readdir(dir)) != NULL) {
        if (!_restore_deadline_check(error)) {
            exact = FALSE;
            break;
        }
        const gchar *filename = entry->d_name;
        if (g_strcmp0(filename, ".") == 0 || g_strcmp0(filename, "..") == 0 ||
            !g_str_has_prefix(filename, "overlay-"))
            continue;
        gsize length = strlen(filename);
        gsize prefix_length = strlen("overlay-");
        gsize suffix_length = strlen(".meta");
        if (length <= prefix_length + suffix_length ||
            !g_str_has_suffix(filename, ".meta")) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Overlay metadata sidecar/unknown dentry '%s' requires recovery",
                        filename);
            exact = FALSE;
            break;
        }
        gchar *name = g_strndup(filename + prefix_length,
                                length - prefix_length - suffix_length);
        if (!pcv_overlay_validate_name(name) || !_find(name) ||
            g_hash_table_contains(canonical_names, name)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Overlay canonical metadata '%s' has no unique registry owner",
                        filename);
            g_free(name);
            exact = FALSE;
            break;
        }
        g_hash_table_add(canonical_names, name);
    }
    closedir(dir);

    if (exact && g_hash_table_size(canonical_names) != (guint)G.count) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "Overlay registry/canonical metadata counts do not match");
        exact = FALSE;
    }
    for (gint i = 0; i < G.count; i++) {
        if (!exact)
            break;
        if (!_restore_deadline_check(error)) {
            exact = FALSE;
            break;
        }
        if (!g_hash_table_contains(canonical_names, G.nets[i].name)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Overlay '%s' has no canonical metadata", G.nets[i].name);
            exact = FALSE;
            break;
        }
        OverlayMetaSnapshot snapshot = {0};
        if (!_canonical_meta_snapshot(&G.nets[i], &snapshot, error)) {
            _overlay_meta_snapshot_clear(&snapshot);
            exact = FALSE;
            break;
        }
        _overlay_meta_snapshot_clear(&snapshot);
    }
    g_hash_table_destroy(canonical_names);

    struct stat after = {0};
    struct stat path_after = {0};
    gboolean stable = exact && fstat(dir_fd, &after) == 0 &&
        g_lstat(_overlay_meta_dir(), &path_after) == 0 &&
        before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
        after.st_dev == path_after.st_dev && after.st_ino == path_after.st_ino &&
        before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
        before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
        before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
        before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
    if (close(dir_fd) != 0)
        stable = FALSE;
    if (!stable && exact)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "Overlay metadata directory changed during final audit");
    return stable;
}

                                         




   
static gboolean
_metadata_snapshot_unchanged(const gchar *path,
                             const OverlayMetaSnapshot *snapshot,
                             GError **error)
{
    gchar *current = NULL;
    gsize current_len = 0;
    GError *read_error = NULL;
    gboolean loaded = _safe_read_metadata(path, &current, &current_len,
                                          &read_error);
    gboolean exact = loaded && current_len == snapshot->len &&
                     memcmp(current, snapshot->data, snapshot->len) == 0;
    if (!exact) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "Overlay metadata changed during mutation");
        g_clear_error(&read_error);
    }
    g_free(current);
    return exact;
}

static gboolean
_canonical_snapshot_unchanged(OverlayNet *net,
                              const OverlayMetaSnapshot *snapshot,
                              GError **error)
{
    gchar *path = _overlay_meta_path(net->name);
    gboolean exact = _metadata_snapshot_unchanged(path, snapshot, error);
    g_free(path);
    return exact;
}


                                                     




static OverlayMetaReplaceResult
_replace_meta_if_snapshot(OverlayNet *net,
                          OverlayMetaSnapshot *snapshot,
                          GError **error)
{
    if (!_ensure_meta_dir(error)) {
        _record_metadata_residue_locked(
            "overlay metadata directory ownership/durability check failed");
        return OVERLAY_META_REPLACE_FAILED;
    }
    gchar *canonical = _overlay_meta_path(net->name);
    gchar *update = _overlay_update_path(net->name);
    if (!_claim_meta_snapshot(canonical, update, snapshot, error)) {
        if (!_metadata_inventory_exact(net->name, TRUE, NULL))
            _metadata_transaction_failed_locked(
                net->name,
                "overlay metadata claim failed with transaction residue",
                error);
        g_free(canonical); g_free(update);
        return OVERLAY_META_REPLACE_FAILED;
    }

    gsize length = 0;
    gchar *data = _serialize_meta(net, &length, error);
    gboolean cleanup_uncertain = FALSE;
    gboolean installed = data &&
        _write_meta_exclusive(canonical, data, length,
                              &cleanup_uncertain, error);
    g_free(data);
    if (!installed) {


        if (!_path_entry_exists(canonical) &&
            !_restore_claimed_meta(update, canonical, snapshot))
            PCV_LOG_WARN(OVERLAY_LOG_DOM,
                         "metadata rollback claim retained: %s", update);
        if (cleanup_uncertain ||
            !_metadata_inventory_exact(net->name, TRUE, NULL))
            _metadata_transaction_failed_locked(
                net->name,
                "overlay metadata install failed with transaction residue",
                error);
        g_free(canonical); g_free(update);
        return OVERLAY_META_REPLACE_FAILED;
    }

    if (G_metadata_test_hook)
        G_metadata_test_hook(net->name, canonical, update,
                             G_metadata_test_hook_data);




    gboolean degraded = FALSE;
    GError *cleanup_error = NULL;
    if (!_sync_meta_dir(&cleanup_error)) {
        degraded = TRUE;
    } else {
        GError *unlink_error = NULL;
        if (!_quarantine_remove_expected(update, snapshot, &unlink_error)) {
            if (!cleanup_error)
                cleanup_error = unlink_error;
            else
                g_clear_error(&unlink_error);
            degraded = TRUE;
        }
    }
    if (degraded) {
        const gchar *message = cleanup_error ? cleanup_error->message
                                             : "metadata cleanup failed";
        PCV_LOG_WARN(OVERLAY_LOG_DOM,
                     "metadata committed with transaction residue: %s", message);
        _record_metadata_residue_locked(
            "overlay metadata update committed with transaction residue");
        if (error && !*error)
            g_propagate_error(error, cleanup_error);
        else
            g_clear_error(&cleanup_error);
        g_free(canonical); g_free(update);
        return OVERLAY_META_REPLACE_COMMITTED_DEGRADED;
    }
    g_free(canonical); g_free(update);
    return OVERLAY_META_REPLACE_COMMITTED;
}







static gboolean
_metadata_paths_still_absent(const gchar *name, GError **error)
{
    gchar *meta = _overlay_meta_path(name);
    gchar *tomb = _overlay_tombstone_path(name);
    gchar *update = _overlay_update_path(name);
    gboolean absent = !_path_entry_exists(meta) &&
                      !_path_entry_exists(tomb) &&
                      !_path_entry_exists(update) &&
                      _metadata_inventory_exact(name, FALSE, NULL);
    if (!absent)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "Overlay '%s' metadata appeared during create", name);
    g_free(meta);
    g_free(tomb);
    g_free(update);
    return absent;
}

static void
_overlay_net_clear(OverlayNet *net)
{
    g_free(net->name); g_free(net->cidr); g_free(net->owner_token);
    if (net->peers)
        g_ptr_array_free(net->peers, TRUE);
    memset(net, 0, sizeof(*net));
}



                                                             



static gboolean
_restore_one_snapshot(OverlayMeta *meta, const gchar *path,
                      const gchar *snapshot, gsize snapshot_len,
                      guint64 admission_epoch,
                      gboolean *applied_out, GError **error)
{
    *applied_out = FALSE;
    g_mutex_lock(&G.mu);



    if (G.restore_state_epoch != admission_epoch) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "Overlay restore snapshot invalidated by concurrent mutation");
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    OverlayNet *existing = _find(meta->name);
    if (existing) {
        gboolean exact = _overlay_meta_matches_net(meta, existing);
        OverlayBulkSnapshot *bulk = _restore_bulk_get();
        if (exact && bulk) {
            gboolean missing = FALSE;
            exact = _bulk_net_exact(bulk, existing,
                                    &missing, error);
        } else if (exact) {
            exact = _validate_owned_actual_inventory_locked(existing, TRUE,
                                                            TRUE, error);
        }
        g_mutex_unlock(&G.mu);
        if (!exact && (!error || !*error))
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                        "Loaded overlay '%s' differs from canonical metadata",
                        meta->name);
        return exact;
    }

    gchar *current = NULL;
    gsize current_len = 0;
    GError *read_error = NULL;
    gboolean loaded = _safe_read_metadata(path, &current, &current_len,
                                          &read_error);
    if (!loaded && g_error_matches(read_error, G_FILE_ERROR, G_FILE_ERROR_NOENT) &&
        _completed_delete_locked(meta->name)) {
        g_clear_error(&read_error);
        g_free(current);
        g_mutex_unlock(&G.mu);
        return TRUE;
    }
    if (!loaded || current_len != snapshot_len ||
        memcmp(current, snapshot, snapshot_len) != 0) {
        if (read_error)
            g_propagate_error(error, read_error);
        else
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                        "Overlay metadata changed while restore was parsing");
        g_free(current);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    g_free(current);



    if (!_metadata_inventory_exact(meta->name, TRUE, error)) {
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    if (G.count >= OVERLAY_MAX) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                    "Maximum overlay count reached");
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    for (gint i = 0; i < G.count; i++) {
        if (G.nets[i].vni == meta->vni) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                        "VNI %d is already loaded", meta->vni);
            g_mutex_unlock(&G.mu);
            return FALSE;
        }
    }

    OverlayNet staged = {
        .name = g_strdup(meta->name),
        .cidr = g_strdup(meta->cidr),
        .owner_token = meta->legacy ? g_uuid_string_random()
                                   : g_strdup(meta->owner_token),
        .vni = meta->vni,
        .generation = meta->legacy ? 1 : meta->generation,
        .peers = g_ptr_array_new_with_free_func(g_free),
        .active = FALSE,
    };
    for (guint i = 0; i < meta->peers->len; i++)
        g_ptr_array_add(staged.peers,
                        g_strdup(g_ptr_array_index(meta->peers, i)));

    gboolean bridge_created = FALSE;
    gboolean migrated = FALSE;
    gboolean metadata_degraded = FALSE;
    GPtrArray *created_peers = NULL;
    gboolean ok = FALSE;
    OverlayMetaSnapshot admission = {
        .data = (gchar *)snapshot,
        .len = snapshot_len,
    };
    if (meta->legacy) {
        gboolean bridge_exists = FALSE;
        if (!_validate_legacy_actual_locked(&staged, &bridge_exists, error))
            goto out;
        if (bridge_exists) {
            if (!_migrate_legacy_actual_locked(&staged, error))
                goto out;
            migrated = TRUE;
            staged.active = TRUE;
        } else if (!_restore_overlay_actual_locked(&staged, &bridge_created,
                                                   &created_peers, error)) {
            goto out;
        }
        OverlayMetaReplaceResult replace_result =
            _replace_meta_if_snapshot(&staged, &admission, error);
        if (replace_result == OVERLAY_META_REPLACE_FAILED) {
            if (migrated)
                _rollback_legacy_migration(&staged, error);
            else if (created_peers)
                _rollback_created_actual(&staged, bridge_created,
                                         created_peers, error);
            goto out;
        }
        metadata_degraded =
            replace_result == OVERLAY_META_REPLACE_COMMITTED_DEGRADED;
    } else if (!_restore_overlay_actual_locked(&staged, &bridge_created,
                                               &created_peers, error)) {
        goto out;
    }

    if (!meta->legacy &&
        !_metadata_snapshot_unchanged(path, &admission, error)) {
        if (created_peers)
            _rollback_created_actual(&staged, bridge_created,
                                     created_peers, error);
        goto out;
    }

    G.nets[G.count++] = staged;
    memset(&staged, 0, sizeof(staged));
    ok = !metadata_degraded;
    *applied_out = TRUE;
out:
    if (created_peers)
        g_ptr_array_free(created_peers, TRUE);
    _overlay_net_clear(&staged);
    g_mutex_unlock(&G.mu);
    return ok;
}







gboolean
pcv_overlay_create(const gchar *name, gint vni, const gchar *cidr, GError **error)
{
    const gchar *normalized_cidr = (cidr && *cidr) ? cidr : "";
                                                          
    if (!pcv_overlay_validate_name(name) || !pcv_overlay_validate_vni(vni) ||
        (*normalized_cidr && !pcv_overlay_validate_cidr(normalized_cidr)))
        return _overlay_create_impl(name, vni, cidr, error);
    if (!_overlay_public_enter("create")) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Overlay not initialized or shutting down");
        return FALSE;
    }
    gboolean ok = _overlay_create_impl(name, vni, cidr, error);
    _overlay_public_leave();
    return ok;
}

static gboolean
_overlay_create_impl(const gchar *name, gint vni, const gchar *cidr,
                     GError **error)
{
    const gchar *normalized_cidr = (cidr && *cidr) ? cidr : "";
    if (!pcv_overlay_validate_name(name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid overlay name");
        return FALSE;
    }
    if (!pcv_overlay_validate_vni(vni)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "VNI must be between %d and %d",
                    PCV_OVERLAY_VNI_MIN, PCV_OVERLAY_VNI_MAX);
        return FALSE;
    }
    if (*normalized_cidr && !pcv_overlay_validate_cidr(normalized_cidr)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid overlay CIDR");
        return FALSE;
    }
    if (!G.initialized) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Overlay not initialized");
        return FALSE;
    }

    g_mutex_lock(&G.mu);
    if (!_mutation_state_is_clean_locked(error)) {
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    OverlayNet *existing = _find(name);
    if (existing) {
        if (existing->vni != vni ||
            g_strcmp0(existing->cidr, normalized_cidr) != 0) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                        "Overlay '%s' already exists with VNI=%d CIDR=%s",
                        name, existing->vni,
                        *existing->cidr ? existing->cidr : "-");
            g_mutex_unlock(&G.mu);
            return FALSE;
        }
        OverlayMetaSnapshot snapshot = {0};
        if (!_canonical_meta_snapshot(existing, &snapshot, error)) {
            g_mutex_unlock(&G.mu);
            return FALSE;
        }
        gboolean bridge_created = FALSE;
        GPtrArray *created_peers = NULL;
        _mutation_started_locked();
        if (!_restore_overlay_actual_locked(existing, &bridge_created,
                                            &created_peers, error)) {
            _overlay_meta_snapshot_clear(&snapshot);
            existing->active = FALSE;
            g_mutex_unlock(&G.mu);
            return FALSE;
        }
        if (!_canonical_snapshot_unchanged(existing, &snapshot, error)) {
            _rollback_created_actual(existing, bridge_created, created_peers,
                                     error);
            g_ptr_array_free(created_peers, TRUE);
            _overlay_meta_snapshot_clear(&snapshot);
            existing->active = !bridge_created;
            g_mutex_unlock(&G.mu);
            return FALSE;
        }
        g_ptr_array_free(created_peers, TRUE);
        _overlay_meta_snapshot_clear(&snapshot);
        existing->active = TRUE;
        g_mutex_unlock(&G.mu);
        return TRUE;
    }

    if (G.count >= OVERLAY_MAX) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                    "Maximum overlay count reached");
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    for (gint i = 0; i < G.count; i++) {
        if (G.nets[i].vni == vni) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                        "VNI %d is already used by overlay '%s'",
                        vni, G.nets[i].name);
            g_mutex_unlock(&G.mu);
            return FALSE;
        }
    }

    gchar *meta_path = _overlay_meta_path(name);
    gchar *tombstone_path = _overlay_tombstone_path(name);
    gchar *update_path = _overlay_update_path(name);
    gboolean residue = _path_entry_exists(meta_path) ||
                       _path_entry_exists(tombstone_path) ||
                       _path_entry_exists(update_path) ||
                       !_metadata_inventory_exact(name, FALSE, NULL);
    g_free(meta_path);
    g_free(tombstone_path);
    g_free(update_path);
    if (residue) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "Overlay '%s' has persistent state pending recovery", name);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }

    OverlayNet staged = {
        .name = g_strdup(name),
        .cidr = g_strdup(normalized_cidr),
        .owner_token = g_uuid_string_random(),
        .vni = vni,
        .generation = 1,
        .peers = g_ptr_array_new_with_free_func(g_free),
        .active = TRUE,
    };
    gboolean bridge_created = FALSE;
    _mutation_started_locked();
    if (!_ensure_bridge_actual_locked(name, normalized_cidr, staged.owner_token,
                                      &bridge_created, error)) {
        _overlay_net_clear(&staged);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    if (!_metadata_paths_still_absent(name, error)) {
        if (bridge_created)
            _rollback_bridge(name, staged.owner_token, error);
        _overlay_net_clear(&staged);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    OverlayMetaReplaceResult save_result = _save_meta(&staged, error);
    if (save_result == OVERLAY_META_REPLACE_FAILED) {
        if (bridge_created)
            _rollback_bridge(name, staged.owner_token, error);
        _overlay_net_clear(&staged);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }

    G.nets[G.count++] = staged;
    _mutation_committed_locked();
    g_mutex_unlock(&G.mu);
    if (save_result == OVERLAY_META_REPLACE_COMMITTED_DEGRADED)
        return FALSE;
    PCV_LOG_INFO(OVERLAY_LOG_DOM, "Overlay '%s' created (VNI=%d, CIDR=%s)",
                 name, vni, *normalized_cidr ? normalized_cidr : "-");
    return TRUE;
}

static gboolean
_overlay_residue_absent(const gchar *name, GError **error)
{
    gchar *meta = _overlay_meta_path(name);
    gchar *tomb = _overlay_tombstone_path(name);
    gchar *update = _overlay_update_path(name);
    if (_path_entry_exists(meta) ||
        _path_entry_exists(tomb) ||
        _path_entry_exists(update) ||
        !_metadata_inventory_exact(name, FALSE, NULL)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "Overlay '%s' has persistent metadata residue", name);
        g_free(meta); g_free(tomb); g_free(update);
        return FALSE;
    }
    g_free(meta); g_free(tomb); g_free(update);

    gboolean exists = FALSE;
    if (!_ovs_row_exists("Bridge", "name", name, &exists, error))
        return FALSE;
    if (exists) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "Overlay '%s' has an unregistered OVS bridge", name);
        return FALSE;
    }
    if (!_ovs_row_exists("Interface", "external_ids:" OVS_NAME_KEY,
                         name, &exists, error))
        return FALSE;
    if (exists) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "Overlay '%s' has unregistered OVS interfaces", name);
        return FALSE;
    }
    return TRUE;
}

static gboolean
_overlay_interfaces_absent(OverlayNet *net, GError **error)
{
    GPtrArray *owned_or_named = _ovs_find_union(
        "Interface", "external_ids:" OVS_OWNER_KEY, net->owner_token,
        "external_ids:" OVS_NAME_KEY, net->name, error);
    if (!owned_or_named)
        return FALSE;
    if (owned_or_named->len > 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "Overlay '%s' still has product-marked Interface residue",
                    net->name);
        g_ptr_array_free(owned_or_named, TRUE);
        return FALSE;
    }
    g_ptr_array_free(owned_or_named, TRUE);
    for (guint i = 0; i < net->peers->len; i++) {
        const gchar *peer = g_ptr_array_index(net->peers, i);
        gchar *canonical = pcv_overlay_peer_port_name(net->vni, peer);
        gchar *legacy = _legacy_peer_port_name(peer);
        gboolean canonical_exists = FALSE;
        gboolean legacy_exists = FALSE;
        gboolean ok = _ovs_row_exists("Interface", "name", canonical,
                                      &canonical_exists, error) &&
                      _ovs_row_exists("Interface", "name", legacy,
                                      &legacy_exists, error);
        if (ok && (canonical_exists || legacy_exists))
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                        "Overlay '%s' still has expected Interface residue",
                        net->name);
        g_free(canonical);
        g_free(legacy);
        if (!ok || canonical_exists || legacy_exists)
            return FALSE;
    }
    return TRUE;
}

static gboolean
_overlay_owned_rows_absent(OverlayNet *net, GError **error)
{
    GPtrArray *bridges = _ovs_find_union(
        "Bridge", "external_ids:" OVS_OWNER_KEY, net->owner_token,
        "external_ids:" OVS_NAME_KEY, net->name, error);
    if (!bridges)
        return FALSE;
    GPtrArray *interfaces = _ovs_find_union(
        "Interface", "external_ids:" OVS_OWNER_KEY, net->owner_token,
        "external_ids:" OVS_NAME_KEY, net->name, error);
    if (!interfaces) {
        g_ptr_array_free(bridges, TRUE);
        return FALSE;
    }
    gboolean absent = bridges->len == 0 && interfaces->len == 0;
    if (!absent)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "Overlay '%s' owned OVS residue appeared during delete",
                    net->name);
    g_ptr_array_free(bridges, TRUE);
    g_ptr_array_free(interfaces, TRUE);
    return absent;
}

   

  


   
gboolean
pcv_overlay_delete(const gchar *name, GError **error)
{
    if (!pcv_overlay_validate_name(name))
        return _overlay_delete_impl(name, error);
    if (!_overlay_public_enter("delete")) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Overlay not initialized or shutting down");
        return FALSE;
    }
    gboolean ok = _overlay_delete_impl(name, error);
    _overlay_public_leave();
    return ok;
}

static gboolean
_overlay_delete_impl(const gchar *name, GError **error)
{
    if (!pcv_overlay_validate_name(name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid overlay name");
        return FALSE;
    }
    if (!G.initialized) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Overlay not initialized");
        return FALSE;
    }

    g_mutex_lock(&G.mu);
    if (!_mutation_state_is_clean_locked(error)) {
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    OverlayNet *net = _find(name);
    if (!net) {
        gboolean absent = _overlay_residue_absent(name, error);
        g_mutex_unlock(&G.mu);
        return absent;
    }
    OverlayMetaSnapshot meta_snapshot = {0};
    if (!_canonical_meta_snapshot(net, &meta_snapshot, error)) {
        g_mutex_unlock(&G.mu);
        return FALSE;
    }

    gboolean bridge_exists = FALSE;
    if (!_bridge_is_owned(name, net->owner_token, &bridge_exists, error)) {
        _overlay_meta_snapshot_clear(&meta_snapshot);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    if (bridge_exists) {



        GPtrArray *children = _bridge_ports(name, error);
        if (!children) {
            _overlay_meta_snapshot_clear(&meta_snapshot);
            g_mutex_unlock(&G.mu);
            return FALSE;
        }
        if (children->len > 0) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                        "Overlay '%s' still has child ports; remove peers/VM ports first",
                        name);
            g_ptr_array_free(children, TRUE);
            _overlay_meta_snapshot_clear(&meta_snapshot);
            g_mutex_unlock(&G.mu);
            return FALSE;
        }
        g_ptr_array_free(children, TRUE);
    }
    if (net->peers->len > 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "Overlay '%s' still has desired peers; remove them first", name);
        _overlay_meta_snapshot_clear(&meta_snapshot);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    if (!_validate_owned_actual_inventory_locked(net, bridge_exists, TRUE,
                                                  error)) {
        _overlay_meta_snapshot_clear(&meta_snapshot);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }


    if (!_overlay_interfaces_absent(net, error)) {
        _overlay_meta_snapshot_clear(&meta_snapshot);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }

    gchar *meta = _overlay_meta_path(name);
    gchar *tomb = _overlay_tombstone_path(name);
    gchar *update = _overlay_update_path(name);
    if (_path_entry_exists(tomb) ||
        _path_entry_exists(update)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "Overlay '%s' already has metadata transaction residue", name);
        g_free(meta); g_free(tomb); g_free(update);
        _overlay_meta_snapshot_clear(&meta_snapshot);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    _mutation_started_locked();
    if (!_claim_meta_snapshot(meta, tomb, &meta_snapshot, error)) {
        if (!_metadata_inventory_exact(name, TRUE, NULL))
            _metadata_transaction_failed_locked(
                name,
                "overlay delete metadata claim failed with transaction residue",
                error);
        g_free(meta); g_free(tomb); g_free(update);
        _overlay_meta_snapshot_clear(&meta_snapshot);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }

    if (bridge_exists) {
        if (!_delete_owned_empty_bridge(name, net->owner_token, error)) {
            OverlayCleanupDeadlineScope cleanup_scope =
                _restore_cleanup_deadline_enter();
            GError *readback_error = NULL;
            gboolean after_exists = FALSE;
            gboolean owned_probe = _bridge_is_owned(
                name, net->owner_token, &after_exists, &readback_error);
            OverlayActualOutcome outcome = OVERLAY_ACTUAL_UNCERTAIN;
            if (owned_probe && !after_exists &&
                _overlay_owned_rows_absent(net, &readback_error)) {
                outcome = OVERLAY_ACTUAL_ABSENT;
            } else if (owned_probe && after_exists) {




                outcome = OVERLAY_ACTUAL_EXACT;
            }
            _restore_cleanup_deadline_leave(&cleanup_scope);
            if (outcome == OVERLAY_ACTUAL_ABSENT) {



                g_clear_error(error);
                g_clear_error(&readback_error);
            } else if (outcome == OVERLAY_ACTUAL_EXACT) {
            gboolean rollback_durable = FALSE;
            if (_path_entry_exists(meta)) {
                PCV_LOG_WARN(OVERLAY_LOG_DOM,
                             "delete rollback preserved tombstone because canonical metadata reappeared: '%s'",
                             meta);
            } else {
                rollback_durable =
                    _restore_claimed_meta(tomb, meta, &meta_snapshot);
                if (!rollback_durable)
                    PCV_LOG_WARN(OVERLAY_LOG_DOM,
                                 "delete rollback: cannot durably restore metadata '%s'",
                                 meta);
            }
            if (!rollback_durable)
                _metadata_transaction_failed_locked(
                    name,
                    "overlay delete actual failed with metadata rollback residue",
                    error);
            g_free(meta); g_free(tomb); g_free(update);
            _overlay_meta_snapshot_clear(&meta_snapshot);
            net->active = FALSE;
            g_mutex_unlock(&G.mu);
            return FALSE;
            } else {
                _actual_outcome_uncertain_locked(
                    name, "overlay delete", error,
                    readback_error ? readback_error->message
                                   : "partial bridge actual state");
                g_clear_error(&readback_error);
                _record_metadata_residue_locked(
                    "overlay delete retained tombstone after uncertain actual result");
                g_free(meta); g_free(tomb); g_free(update);
                _overlay_meta_snapshot_clear(&meta_snapshot);
                net->active = FALSE;
                g_mutex_unlock(&G.mu);
                return FALSE;
            }
            g_clear_error(&readback_error);
        }
    }

    if (_path_entry_exists(meta)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "Overlay metadata reappeared during delete; tombstone retained");
        _metadata_transaction_failed_locked(
            name,
            "overlay delete actual committed but canonical metadata reappeared",
            error);
        g_free(meta); g_free(tomb); g_free(update);
        _overlay_meta_snapshot_clear(&meta_snapshot);
        net->active = FALSE;
        g_mutex_unlock(&G.mu);
        return FALSE;
    }

    if (!_overlay_owned_rows_absent(net, error)) {
        _record_metadata_residue_locked(
            "overlay delete retained tombstone after owned OVS residue appeared");
        net->active = FALSE;
        g_free(meta); g_free(tomb); g_free(update);
        _overlay_meta_snapshot_clear(&meta_snapshot);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }

    if (G_metadata_test_hook)
        G_metadata_test_hook(name, meta, tomb, G_metadata_test_hook_data);
    gboolean tomb_removed =
        _quarantine_remove_expected(tomb, &meta_snapshot, error);
    if (!tomb_removed)
        _record_metadata_residue_locked(
            "overlay delete committed with tombstone cleanup residue");

    gint idx = (gint)(net - G.nets);
    gint last = G.count - 1;
    _overlay_net_clear(net);
    if (idx != last) {
        G.nets[idx] = G.nets[last];
        memset(&G.nets[last], 0, sizeof(G.nets[last]));
    }
    G.count--;
    _mutation_committed_locked();
    g_free(meta); g_free(tomb); g_free(update);
    _overlay_meta_snapshot_clear(&meta_snapshot);
    g_mutex_unlock(&G.mu);
    if (tomb_removed)
        PCV_LOG_INFO(OVERLAY_LOG_DOM, "Overlay '%s' deleted", name);
    return tomb_removed;
}

typedef struct {
    gchar *name;
    gchar *cidr;
    gchar *owner_token;
    gchar *local_ip;
    gint vni;
    guint peer_count;
    GPtrArray *peers;
} OverlayView;

static void
_overlay_view_clear(OverlayView *view)
{
    g_free(view->name); g_free(view->cidr); g_free(view->owner_token);
    g_free(view->local_ip);
    if (view->peers)
        g_ptr_array_free(view->peers, TRUE);
    memset(view, 0, sizeof(*view));
}


                                                



   
static void
_overlay_view_copy(OverlayView *view, OverlayNet *net, gboolean with_peers)
{
    view->name = g_strdup(net->name);
    view->cidr = g_strdup(net->cidr);
    view->owner_token = g_strdup(net->owner_token);
    view->local_ip = g_strdup(G.local_ip);
    view->vni = net->vni;
    view->peer_count = net->peers->len;
    if (with_peers) {
        view->peers = g_ptr_array_new_with_free_func(g_free);
        for (guint i = 0; i < net->peers->len; i++)
            g_ptr_array_add(view->peers,
                            g_strdup(g_ptr_array_index(net->peers, i)));
    }
}

static const gchar *
_overlay_json_string(JsonNode *node)
{
    return node && JSON_NODE_HOLDS_VALUE(node) &&
        json_node_get_value_type(node) == G_TYPE_STRING
        ? json_node_get_string(node) : NULL;
}




static gboolean
_overlay_probe_views(OverlayView *views, gint count, gboolean *active,
                     GError **error)
{
    if (count == 0)
        return TRUE;
    const gchar *argv[] = {
        "ovs-vsctl", "--format=json", "--columns=name,external_ids",
        "list", "Bridge", NULL
    };
    gchar *output = NULL;
    if (!_run_argv_capture(argv, &output, error)) {
        g_free(output);
        return FALSE;
    }

    JsonParser *parser = json_parser_new();
    gboolean ok = FALSE;
    if (!json_parser_load_from_data(parser, output ? output : "", -1, error))
        goto out;
    JsonNode *root_node = json_parser_get_root(parser);
    if (!root_node || !JSON_NODE_HOLDS_OBJECT(root_node))
        goto malformed;
    JsonObject *root = json_node_get_object(root_node);
    JsonNode *headings_node = json_object_get_member(root, "headings");
    JsonNode *data_node = json_object_get_member(root, "data");
    if (!headings_node || !JSON_NODE_HOLDS_ARRAY(headings_node) ||
        !data_node || !JSON_NODE_HOLDS_ARRAY(data_node))
        goto malformed;

    JsonArray *headings = json_node_get_array(headings_node);
    gint name_index = -1;
    gint ids_index = -1;
    for (guint i = 0; i < json_array_get_length(headings); i++) {
        const gchar *heading =
            _overlay_json_string(json_array_get_element(headings, i));
        if (g_strcmp0(heading, "name") == 0)
            name_index = (gint)i;
        else if (g_strcmp0(heading, "external_ids") == 0)
            ids_index = (gint)i;
    }
    if (name_index < 0 || ids_index < 0)
        goto malformed;

    JsonArray *data = json_node_get_array(data_node);
    for (guint row_index = 0; row_index < json_array_get_length(data); row_index++) {
        JsonNode *row_node = json_array_get_element(data, row_index);
        if (!row_node || !JSON_NODE_HOLDS_ARRAY(row_node))
            goto malformed;
        JsonArray *row = json_node_get_array(row_node);
        guint required = (guint)MAX(name_index, ids_index);
        if (json_array_get_length(row) <= required)
            goto malformed;
        const gchar *bridge_name =
            _overlay_json_string(json_array_get_element(row, (guint)name_index));
        JsonNode *ids_node = json_array_get_element(row, (guint)ids_index);
        if (!bridge_name || !ids_node || !JSON_NODE_HOLDS_ARRAY(ids_node))
            goto malformed;

        gint view_index = -1;
        for (gint i = 0; i < count; i++)
            if (g_strcmp0(bridge_name, views[i].name) == 0) {
                view_index = i;
                break;
            }
        if (view_index < 0)
            continue;

        JsonArray *ids = json_node_get_array(ids_node);
        if (json_array_get_length(ids) != 2 ||
            g_strcmp0(_overlay_json_string(json_array_get_element(ids, 0)),
                      "map") != 0)
            goto malformed;
        JsonNode *pairs_node = json_array_get_element(ids, 1);
        if (!pairs_node || !JSON_NODE_HOLDS_ARRAY(pairs_node))
            goto malformed;
        JsonArray *pairs = json_node_get_array(pairs_node);
        const gchar *owner = NULL;
        const gchar *overlay_name = NULL;
        for (guint i = 0; i < json_array_get_length(pairs); i++) {
            JsonNode *pair_node = json_array_get_element(pairs, i);
            if (!pair_node || !JSON_NODE_HOLDS_ARRAY(pair_node))
                goto malformed;
            JsonArray *pair = json_node_get_array(pair_node);
            if (json_array_get_length(pair) != 2)
                goto malformed;
            const gchar *key =
                _overlay_json_string(json_array_get_element(pair, 0));
            const gchar *value =
                _overlay_json_string(json_array_get_element(pair, 1));
            if (!key || !value)
                goto malformed;
            if (g_strcmp0(key, OVS_OWNER_KEY) == 0)
                owner = value;
            else if (g_strcmp0(key, OVS_NAME_KEY) == 0)
                overlay_name = value;
        }
        if (g_strcmp0(owner, views[view_index].owner_token) != 0 ||
            g_strcmp0(overlay_name, views[view_index].name) != 0) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                        "OVS bridge '%s' ownership token mismatch", bridge_name);
            goto out;
        }
        active[view_index] = TRUE;
    }
    ok = TRUE;
    goto out;

malformed:
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Malformed ovs-vsctl Bridge JSON snapshot");
out:
    g_object_unref(parser);
    g_free(output);
    return ok;
}



JsonArray *
pcv_overlay_list(GError **error)
{
    if (!_overlay_public_enter("list")) {
        if (!_disabled_metadata_inventory_clean(error))
            return NULL;
        return json_array_new();
    }
    JsonArray *result = _overlay_list_impl(error);
    _overlay_public_leave();
    return result;
}






   
static JsonArray *
_overlay_list_impl(GError **error)
{
    JsonArray *result = json_array_new();
    if (!G.initialized)
        return result;

    OverlayView views[OVERLAY_MAX] = {0};
    gint count = 0;
    g_mutex_lock(&G.mu);
    if (G.restore_in_progress) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "Overlay restore/audit is in progress");
        g_mutex_unlock(&G.mu);
        json_array_unref(result);
        return NULL;
    }
    if (G.restore_rejected_count > 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "Overlay restore has %u rejected resource(s): %s",
                    G.restore_rejected_count,
                    G.restore_error ? G.restore_error : "unknown");
        g_mutex_unlock(&G.mu);
        json_array_unref(result);
        return NULL;
    }
    count = G.count;
    for (gint i = 0; i < count; i++)
        _overlay_view_copy(&views[i], &G.nets[i], FALSE);
    g_mutex_unlock(&G.mu);

    gboolean active[OVERLAY_MAX] = {0};
    if (!_overlay_probe_views(views, count, active, error)) {
        for (gint i = 0; i < count; i++)
            _overlay_view_clear(&views[i]);
        json_array_unref(result);
        return NULL;
    }
    for (gint i = 0; i < count; i++) {
        JsonObject *row = json_object_new();
        json_object_set_string_member(row, "name", views[i].name);
        json_object_set_int_member(row, "vni", views[i].vni);
        json_object_set_string_member(row, "cidr", views[i].cidr);
        json_object_set_int_member(row, "peer_count", views[i].peer_count);
        json_object_set_boolean_member(row, "active", active[i]);
        json_array_add_object_element(result, row);
    }
    for (gint i = 0; i < count; i++)
        _overlay_view_clear(&views[i]);
    return result;
}

JsonObject *
pcv_overlay_info(const gchar *name, GError **error)
{
    if (!pcv_overlay_validate_name(name)) {
        JsonObject *invalid = json_object_new();
        json_object_set_string_member(invalid, "error", "invalid name");
        return invalid;
    }
    if (!_overlay_public_enter("info")) {
        if (!_disabled_metadata_inventory_clean(error))
            return NULL;
        JsonObject *disabled = json_object_new();
        json_object_set_string_member(disabled, "error", "overlay disabled");
        return disabled;
    }
    JsonObject *result = _overlay_info_impl(name, error);
    _overlay_public_leave();
    return result;
}

static JsonObject *
_overlay_info_impl(const gchar *name, GError **error)
{
    JsonObject *result = json_object_new();
    if (!pcv_overlay_validate_name(name)) {
        json_object_set_string_member(result, "error", "invalid name");
        return result;
    }
    if (!G.initialized) {
        json_object_set_string_member(result, "error", "overlay disabled");
        return result;
    }

    OverlayView view = {0};
    g_mutex_lock(&G.mu);
    if (G.restore_in_progress) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "Overlay restore/audit is in progress");
        g_mutex_unlock(&G.mu);
        json_object_unref(result);
        return NULL;
    }
    if (G.restore_rejected_count > 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "Overlay restore has %u rejected resource(s): %s",
                    G.restore_rejected_count,
                    G.restore_error ? G.restore_error : "unknown");
        g_mutex_unlock(&G.mu);
        json_object_unref(result);
        return NULL;
    }
    OverlayNet *net = _find(name);
    if (!net) {
        g_mutex_unlock(&G.mu);
        json_object_set_string_member(result, "error", "not found");
        return result;
    }
    _overlay_view_copy(&view, net, TRUE);
    g_mutex_unlock(&G.mu);

    gboolean exists = FALSE;
    if (!_overlay_probe_views(&view, 1, &exists, error)) {
        _overlay_view_clear(&view);
        json_object_unref(result);
        return NULL;
    }
    json_object_set_string_member(result, "name", view.name);
    json_object_set_int_member(result, "vni", view.vni);
    json_object_set_string_member(result, "cidr", view.cidr);
    json_object_set_string_member(result, "local_tunnel_ip", view.local_ip);
    json_object_set_boolean_member(result, "active", exists);
    JsonArray *peers = json_array_new();
    for (guint i = 0; i < view.peers->len; i++)
        json_array_add_string_element(peers, g_ptr_array_index(view.peers, i));
    json_object_set_array_member(result, "peers", peers);
    _overlay_view_clear(&view);
    return result;
}
                                                                     

gboolean
pcv_overlay_add_peer(const gchar *name, const gchar *peer_tunnel_ip,
                     GError **error)
{
    if (!pcv_overlay_validate_name(name) ||
        !pcv_overlay_validate_peer_ip(peer_tunnel_ip))
        return _overlay_add_peer_impl(name, peer_tunnel_ip, error);
    if (!_overlay_public_enter("add_peer")) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Overlay not initialized or shutting down");
        return FALSE;
    }
    gboolean ok = _overlay_add_peer_impl(name, peer_tunnel_ip, error);
    _overlay_public_leave();
    return ok;
}

static gboolean
_overlay_add_peer_impl(const gchar *name, const gchar *peer_tunnel_ip,
                       GError **error)
{
    if (!pcv_overlay_validate_name(name) ||
        !pcv_overlay_validate_peer_ip(peer_tunnel_ip)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid overlay name or IPv4 peer_ip");
        return FALSE;
    }
    if (!G.initialized) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Overlay not initialized");
        return FALSE;
    }
    if (g_strcmp0(peer_tunnel_ip, G.local_ip) == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "peer_ip must not equal local tunnel_ip");
        return FALSE;
    }

    g_mutex_lock(&G.mu);
    if (!_mutation_state_is_clean_locked(error)) {
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    OverlayNet *net = _find(name);
    if (!net) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Overlay '%s' not found", name);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    OverlayMetaSnapshot meta_snapshot = {0};
    if (!_canonical_meta_snapshot(net, &meta_snapshot, error)) {
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    gboolean bridge_exists = FALSE;
    if (!_bridge_is_owned(net->name, net->owner_token, &bridge_exists, error) ||
        !bridge_exists) {
        if (!error || !*error)
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "Owned bridge '%s' is absent", net->name);
        _overlay_meta_snapshot_clear(&meta_snapshot);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    if (!_validate_owned_actual_inventory_locked(net, TRUE, TRUE, error)) {
        _overlay_meta_snapshot_clear(&meta_snapshot);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }

    for (guint i = 0; i < net->peers->len; i++) {
        if (g_strcmp0(g_ptr_array_index(net->peers, i), peer_tunnel_ip) == 0) {
            OverlayPeerActualSnapshot before = {0};
            if (!_peer_actual_snapshot(net, peer_tunnel_ip, &before, error)) {
                _peer_actual_snapshot_clear(&before);
                _overlay_meta_snapshot_clear(&meta_snapshot);
                g_mutex_unlock(&G.mu);
                return FALSE;
            }
            gboolean created = FALSE;
            _mutation_started_locked();
            gboolean ok = _ensure_peer_actual_locked(net, peer_tunnel_ip,
                                                       &created, error);
            if (ok && !_canonical_snapshot_unchanged(net, &meta_snapshot,
                                                     error)) {
                _rollback_peer_change(net, peer_tunnel_ip, created, &before,
                                      error);
                ok = FALSE;
            }
            _peer_actual_snapshot_clear(&before);
            _overlay_meta_snapshot_clear(&meta_snapshot);
            g_mutex_unlock(&G.mu);
            return ok;
        }
    }

    if (net->generation >= (guint64)G_MAXINT64) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                    "Overlay '%s' generation cannot advance", net->name);
        _overlay_meta_snapshot_clear(&meta_snapshot);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    if (net->peers->len >= OVERLAY_MAX_PEERS) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                    "Overlay '%s' reached the %u-peer Single Edge limit",
                    net->name, OVERLAY_MAX_PEERS);
        _overlay_meta_snapshot_clear(&meta_snapshot);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    if (!_metadata_peer_add_fits_locked(net, peer_tunnel_ip, error)) {
        _overlay_meta_snapshot_clear(&meta_snapshot);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }

    OverlayPeerActualSnapshot before = {0};
    if (!_peer_actual_snapshot(net, peer_tunnel_ip, &before, error)) {
        _peer_actual_snapshot_clear(&before);
        _overlay_meta_snapshot_clear(&meta_snapshot);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    if (before.exists) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "Peer '%s' has owned actual residue outside desired metadata",
                    peer_tunnel_ip);
        _peer_actual_snapshot_clear(&before);
        _overlay_meta_snapshot_clear(&meta_snapshot);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    gboolean port_created = FALSE;
    _mutation_started_locked();
    if (!_ensure_peer_actual_locked(net, peer_tunnel_ip,
                                    &port_created, error)) {
        if (port_created)
            _rollback_peer_change(net, peer_tunnel_ip, TRUE, &before,
                                  error);
        _peer_actual_snapshot_clear(&before);
        _overlay_meta_snapshot_clear(&meta_snapshot);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }




    gchar *created_port = pcv_overlay_peer_port_name(net->vni, peer_tunnel_ip);
    gchar *created_ports_cond = NULL;
    gchar *created_interfaces_cond = NULL;
    gboolean created_exists = FALSE;
    gboolean created_exact =
        _interface_is_owned(net, created_port, &created_exists, error) &&
        created_exists &&
        _bridge_current_ports_condition(net->name, created_port,
                                        &created_ports_cond, error) &&
        _port_interface_exact_condition(created_port,
                                        &created_interfaces_cond, error) &&
        _interface_options_exact(net, created_port, peer_tunnel_ip, error);
    g_free(created_ports_cond);
    g_free(created_interfaces_cond);
    g_free(created_port);
    if (!created_exact) {
        _rollback_peer_change(net, peer_tunnel_ip, port_created, &before,
                              error);
        _peer_actual_snapshot_clear(&before);
        _overlay_meta_snapshot_clear(&meta_snapshot);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }

    guint64 old_generation = net->generation;
    net->generation++;
    g_ptr_array_add(net->peers, g_strdup(peer_tunnel_ip));
    OverlayMetaReplaceResult replace_result =
        _replace_meta_if_snapshot(net, &meta_snapshot, error);
    if (replace_result == OVERLAY_META_REPLACE_FAILED) {
        g_ptr_array_remove_index(net->peers, net->peers->len - 1);
        net->generation = old_generation;
        _rollback_peer_change(net, peer_tunnel_ip, port_created, &before,
                              error);
        _peer_actual_snapshot_clear(&before);
        _overlay_meta_snapshot_clear(&meta_snapshot);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    PCV_LOG_INFO(OVERLAY_LOG_DOM,
                 "VXLAN peer %s added to '%s' (VNI=%d, local_ip=%s)",
                 peer_tunnel_ip, net->name, net->vni, G.local_ip);
    _mutation_committed_locked();
    _peer_actual_snapshot_clear(&before);
    _overlay_meta_snapshot_clear(&meta_snapshot);
    g_mutex_unlock(&G.mu);
    return replace_result == OVERLAY_META_REPLACE_COMMITTED;
}

gboolean
pcv_overlay_remove_peer(const gchar *name, const gchar *peer_tunnel_ip,
                        GError **error)
{
    if (!pcv_overlay_validate_name(name) ||
        !pcv_overlay_validate_peer_ip(peer_tunnel_ip))
        return _overlay_remove_peer_impl(name, peer_tunnel_ip, error);
    if (!_overlay_public_enter("remove_peer")) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Overlay not initialized or shutting down");
        return FALSE;
    }
    gboolean ok = _overlay_remove_peer_impl(name, peer_tunnel_ip, error);
    _overlay_public_leave();
    return ok;
}

static gboolean
_overlay_remove_peer_impl(const gchar *name, const gchar *peer_tunnel_ip,
                          GError **error)
{
    if (!pcv_overlay_validate_name(name) ||
        !pcv_overlay_validate_peer_ip(peer_tunnel_ip)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid overlay name or IPv4 peer_ip");
        return FALSE;
    }
    if (!G.initialized) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Overlay not initialized");
        return FALSE;
    }

    g_mutex_lock(&G.mu);
    if (!_mutation_state_is_clean_locked(error)) {
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    OverlayNet *net = _find(name);
    if (!net) {
        gboolean absent = _overlay_residue_absent(name, error);
        g_mutex_unlock(&G.mu);
        return absent;
    }
    OverlayMetaSnapshot meta_snapshot = {0};
    if (!_canonical_meta_snapshot(net, &meta_snapshot, error)) {
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    if (!_validate_owned_actual_inventory_locked(net, TRUE, TRUE, error)) {
        _overlay_meta_snapshot_clear(&meta_snapshot);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }

    guint peer_index = G_MAXUINT;
    for (guint i = 0; i < net->peers->len; i++) {
        if (g_strcmp0(g_ptr_array_index(net->peers, i), peer_tunnel_ip) == 0) {
            peer_index = i;
            break;
        }
    }
    if (peer_index == G_MAXUINT) {
        gchar *canonical = pcv_overlay_peer_port_name(net->vni, peer_tunnel_ip);
        gboolean canonical_exists = FALSE;
        if (!_interface_is_owned(net, canonical, &canonical_exists, error)) {
            g_free(canonical);
            _overlay_meta_snapshot_clear(&meta_snapshot);
            g_mutex_unlock(&G.mu);
            return FALSE;
        }
        if (canonical_exists) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                        "Peer '%s' has an owned actual residue outside metadata",
                        peer_tunnel_ip);
            g_free(canonical);
            _overlay_meta_snapshot_clear(&meta_snapshot);
            g_mutex_unlock(&G.mu);
            return FALSE;
        }
        g_free(canonical);
        gchar *legacy = _legacy_peer_port_name(peer_tunnel_ip);
        gboolean legacy_exists = FALSE;
        if (!_ovs_row_exists("Interface", "name", legacy, &legacy_exists, error)) {
            g_free(legacy);
            _overlay_meta_snapshot_clear(&meta_snapshot);
            g_mutex_unlock(&G.mu);
            return FALSE;
        }
        if (legacy_exists) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                        "Legacy peer '%s' remains outside metadata", legacy);
            g_free(legacy);
            _overlay_meta_snapshot_clear(&meta_snapshot);
            g_mutex_unlock(&G.mu);
            return FALSE;
        }
        g_free(legacy);
        _overlay_meta_snapshot_clear(&meta_snapshot);
        g_mutex_unlock(&G.mu);
        return TRUE;
    }

    if (net->generation >= (guint64)G_MAXINT64) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                    "Overlay '%s' generation cannot advance", net->name);
        _overlay_meta_snapshot_clear(&meta_snapshot);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }

    gboolean bridge_exists = FALSE;
    if (!_bridge_is_owned(net->name, net->owner_token, &bridge_exists, error) ||
        !bridge_exists) {
        if (!error || !*error)
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "Owned bridge '%s' is absent", net->name);
        _overlay_meta_snapshot_clear(&meta_snapshot);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }

    gchar *port_name = pcv_overlay_peer_port_name(net->vni, peer_tunnel_ip);
    gboolean port_exists = FALSE;
    if (!_interface_is_owned(net, port_name, &port_exists, error)) {
        g_free(port_name);
        _overlay_meta_snapshot_clear(&meta_snapshot);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    if (!port_exists) {
        gchar *legacy = _legacy_peer_port_name(peer_tunnel_ip);
        gboolean legacy_exists = FALSE;
        if (!_ovs_row_exists("Interface", "name", legacy, &legacy_exists, error)) {
            g_free(legacy); g_free(port_name);
            _overlay_meta_snapshot_clear(&meta_snapshot);
            g_mutex_unlock(&G.mu);
            return FALSE;
        }
        if (legacy_exists) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                        "Legacy peer '%s' remains; refusing implicit cleanup", legacy);
            g_free(legacy); g_free(port_name);
            _overlay_meta_snapshot_clear(&meta_snapshot);
            g_mutex_unlock(&G.mu);
            return FALSE;
        }
        g_free(legacy);
    }

    gchar *ports_cond = NULL;
    gchar *port_interfaces_cond = NULL;
    if (port_exists &&
        !_bridge_current_ports_condition(net->name, port_name,
                                         &ports_cond, error)) {
        g_free(port_name);
        _overlay_meta_snapshot_clear(&meta_snapshot);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    if (port_exists &&
        !_port_interface_exact_condition(port_name,
                                         &port_interfaces_cond, error)) {
        g_free(port_name); g_free(ports_cond);
        _overlay_meta_snapshot_clear(&meta_snapshot);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    gchar *peer_options_cond = NULL;
    if (port_exists) {
        gboolean row_exists = FALSE;
        gchar *type = NULL;
        if (!_ovs_get_field("Interface", port_name, "type", &row_exists,
                            &type, error)) {
            g_free(type); g_free(port_name); g_free(ports_cond);
            g_free(port_interfaces_cond);
            _overlay_meta_snapshot_clear(&meta_snapshot);
            g_mutex_unlock(&G.mu);
            return FALSE;
        }
        gboolean actual_exact = row_exists &&
            g_strcmp0(type, "vxlan") == 0 &&
            _interface_options_exact(net, port_name, peer_tunnel_ip, error);
        g_free(type);
        if (!actual_exact) {
            if (!error || !*error)
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                            "OVS Interface '%s' type/options drifted", port_name);
            g_free(port_name); g_free(ports_cond);
            g_free(port_interfaces_cond);
            _overlay_meta_snapshot_clear(&meta_snapshot);
            g_mutex_unlock(&G.mu);
            return FALSE;
        }
        gchar *expected_options = _expected_options_map(net, peer_tunnel_ip);
        peer_options_cond = g_strdup_printf("options=%s", expected_options);
        g_free(expected_options);
    }

    gchar *removed_peer = g_strdup(peer_tunnel_ip);
    guint64 old_generation = net->generation;
    _mutation_started_locked();
    g_ptr_array_remove_index(net->peers, peer_index);
    net->generation++;
    OverlayMetaReplaceResult replace_result =
        _replace_meta_if_snapshot(net, &meta_snapshot, error);
    if (replace_result == OVERLAY_META_REPLACE_FAILED) {
        g_ptr_array_insert(net->peers, (gint)peer_index, removed_peer);
        net->generation = old_generation;
        g_free(port_name); g_free(ports_cond); g_free(peer_options_cond);
        g_free(port_interfaces_cond);
        _overlay_meta_snapshot_clear(&meta_snapshot);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    gboolean metadata_degraded =
        replace_result == OVERLAY_META_REPLACE_COMMITTED_DEGRADED;
    gchar *degraded_message = NULL;
    if (metadata_degraded) {
        degraded_message = g_strdup(error && *error ? (*error)->message
                                                     : "metadata transaction residue");
        if (error)
            g_clear_error(error);
    }
    OverlayMetaSnapshot committed_snapshot = {0};
    if (!_canonical_meta_snapshot(net, &committed_snapshot, error)) {
        _record_metadata_residue_locked(
            "overlay remove committed metadata but canonical readback failed");
        g_free(removed_peer);
        g_free(degraded_message);
        g_free(port_name); g_free(ports_cond); g_free(peer_options_cond);
        g_free(port_interfaces_cond);
        _overlay_meta_snapshot_clear(&meta_snapshot);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }

    if (port_exists) {
        gchar *owner_cond = g_strdup_printf("external_ids:%s=%s",
                                            OVS_OWNER_KEY, net->owner_token);
        gchar *name_cond = g_strdup_printf("external_ids:%s=%s",
                                           OVS_NAME_KEY, net->name);
        const gchar *argv[] = {
            "ovs-vsctl", "--", "wait-until", "Bridge", net->name,
            owner_cond, name_cond, ports_cond,
            "--", "wait-until", "Port", port_name,
            port_interfaces_cond,
            "--", "wait-until", "Interface", port_name,
            owner_cond, name_cond, "type=vxlan", peer_options_cond,
            "--", "del-port", net->name, port_name, NULL
        };
        if (!_run_argv(argv, error)) {
            OverlayCleanupDeadlineScope cleanup_scope =
                _restore_cleanup_deadline_enter();
            GError *readback_error = NULL;
            OverlayActualOutcome outcome = _peer_actual_outcome(
                net, peer_tunnel_ip, &readback_error);
            _restore_cleanup_deadline_leave(&cleanup_scope);
            if (outcome == OVERLAY_ACTUAL_ABSENT) {



                g_clear_error(error);
                g_clear_error(&readback_error);
            } else if (outcome == OVERLAY_ACTUAL_EXACT &&
                       !metadata_degraded) {
                g_ptr_array_insert(net->peers, (gint)peer_index, removed_peer);
                removed_peer = NULL;
                net->generation = old_generation;
                GError *rollback_error = NULL;
                OverlayMetaReplaceResult rollback_result =
                    _replace_meta_if_snapshot(net, &committed_snapshot,
                                              &rollback_error);
                if (rollback_result != OVERLAY_META_REPLACE_COMMITTED) {
                    _record_metadata_residue_locked(
                        "overlay remove actual failed and metadata rollback is degraded");
                    PCV_LOG_WARN(OVERLAY_LOG_DOM,
                                 "remove rollback metadata failed for '%s': %s",
                                 net->name,
                                 rollback_error ? rollback_error->message : "unknown");
                }
                g_clear_error(&rollback_error);
            } else if (outcome == OVERLAY_ACTUAL_EXACT) {
                _record_metadata_residue_locked(
                    "overlay remove metadata committed but actual removal failed");
            } else {
                _actual_outcome_uncertain_locked(
                    net->name, "peer remove", error,
                    readback_error ? readback_error->message
                                   : "partial peer actual state");
            }
            g_clear_error(&readback_error);
            if (outcome != OVERLAY_ACTUAL_ABSENT) {
                g_free(removed_peer);
                g_free(degraded_message);
                g_free(owner_cond); g_free(name_cond); g_free(port_name);
                g_free(peer_options_cond);
                g_free(port_interfaces_cond);
                g_free(ports_cond);
                _overlay_meta_snapshot_clear(&committed_snapshot);
                _overlay_meta_snapshot_clear(&meta_snapshot);
                g_mutex_unlock(&G.mu);
                return FALSE;
            }
        }
        g_free(owner_cond); g_free(name_cond);
    }

    g_free(removed_peer);
    if (metadata_degraded && error && !*error)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Overlay remove committed but requires recovery: %s",
                    degraded_message ? degraded_message : "metadata residue");
    g_free(degraded_message);
    g_free(port_name);
    g_free(ports_cond);
    g_free(peer_options_cond);
    g_free(port_interfaces_cond);
    _overlay_meta_snapshot_clear(&committed_snapshot);
    _overlay_meta_snapshot_clear(&meta_snapshot);
    _mutation_committed_locked();
    g_mutex_unlock(&G.mu);
    return !metadata_degraded;
}
