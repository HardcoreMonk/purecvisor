   
                            
                                                                     
  
                           
                                                         
                                                                    
                                                                         
                                                                       
                                                                        
                                                               
  
                       
                                                      
                                                     
                                                  
  
                                                        
   
#include "modules/network/pcv_shared_bridge.h"

#include <errno.h>
#include <net/if.h>
#include <string.h>
#include <unistd.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

#include "bpf/pcv_shared_bridge.h"
#include "utils/pcv_bpf.h"
#include "utils/pcv_log.h"

#ifdef HAVE_LIBBPF
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#endif

#define SHARED_LOG_DOM "shared-bridge"
#define PCV_BPF_PIN_BASE "/sys/fs/bpf/purecvisor"

static gchar g_shared_sha[65];
static gboolean g_shared_prepared;

#ifdef HAVE_LIBBPF
static GMutex g_shared_bpf_mu;
static gchar *g_shared_pin_dir;

static const gchar *const g_program_names[] = {
    "pcv_phys_ing",
    "pcv_phys_eg",
    "pcv_portal_ing",
};

static const gchar *const g_map_names[] = {
    "pcv_sh_links",
    "pcv_sh_guests",
    "pcv_sh_stats",
};
#endif

  
                                                                        
  
                                                             
                                                        
                                                          
                                                 
  
                                                 
   
void
pcv_shared_bridge_portal_names(const gchar *bridge_name,
                               gchar bridge_end[16],
                               gchar portal_end[16])
{
    guint32 hash = g_str_hash(bridge_name ? bridge_name : "");
    g_snprintf(bridge_end, 16, "psb%08x", hash);
    g_snprintf(portal_end, 16, "psp%08x", hash);
}

#ifdef HAVE_LIBBPF
                                                     
                                                     
                                                  
static gboolean
_pin_set_complete(const gchar *pin_dir)
{
    for (guint i = 0; i < G_N_ELEMENTS(g_program_names); i++) {
        gchar *path = g_build_filename(pin_dir, g_program_names[i], NULL);
        int fd = bpf_obj_get(path);
        g_free(path);
        if (fd < 0) return FALSE;
        close(fd);
    }
    for (guint i = 0; i < G_N_ELEMENTS(g_map_names); i++) {
        gchar *path = g_build_filename(pin_dir, g_map_names[i], NULL);
        int fd = bpf_obj_get(path);
        g_free(path);
        if (fd < 0) return FALSE;
        close(fd);
    }
    return TRUE;
}

static void
_remove_known_pins(const gchar *pin_dir)
{
    for (guint i = 0; i < G_N_ELEMENTS(g_program_names); i++) {
        gchar *path = g_build_filename(pin_dir, g_program_names[i], NULL);
        g_unlink(path);
        g_free(path);
    }
    for (guint i = 0; i < G_N_ELEMENTS(g_map_names); i++) {
        gchar *path = g_build_filename(pin_dir, g_map_names[i], NULL);
        g_unlink(path);
        g_free(path);
    }
    g_rmdir(pin_dir);
}

static const PcvBpfManifestEntry *
_find_network_entry(GPtrArray *manifest)
{
    for (guint i = 0; manifest && i < manifest->len; i++) {
        const PcvBpfManifestEntry *entry = g_ptr_array_index(manifest, i);
        if (g_strcmp0(entry->loader, "network-tc") == 0
            && g_strcmp0(entry->name, "pcv_shared_bridge") == 0)
            return entry;
    }
    return NULL;
}

static gboolean
_load_and_pin(const gchar *store_dir,
              const PcvBpfManifestEntry *entry,
              const gchar *pin_dir,
              GError **error)
{
    gchar *object_path = g_build_filename(store_dir, entry->file, NULL);
    struct bpf_object *object = bpf_object__open_file(object_path, NULL);
    if (!object) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "cannot open shared bridge BPF object '%s': %s",
                    object_path, g_strerror(errno));
        g_free(object_path);
        return FALSE;
    }
    int rc = bpf_object__load(object);
    if (rc != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "cannot load shared bridge BPF object: %s", g_strerror(-rc));
        bpf_object__close(object);
        g_free(object_path);
        return FALSE;
    }
    if (g_mkdir_with_parents(PCV_BPF_PIN_BASE, 0700) != 0
        || g_mkdir(pin_dir, 0700) != 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "cannot create shared bridge BPF pin directory: %s",
                    g_strerror(errno));
        bpf_object__close(object);
        g_free(object_path);
        return FALSE;
    }

    gboolean ok = TRUE;
    for (guint i = 0; i < G_N_ELEMENTS(g_program_names) && ok; i++) {
        struct bpf_program *program =
            bpf_object__find_program_by_name(object, g_program_names[i]);
        gchar *pin = g_build_filename(pin_dir, g_program_names[i], NULL);
        rc = program ? bpf_program__pin(program, pin) : -ENOENT;
        if (rc != 0) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "cannot pin shared bridge program '%s': %s",
                        g_program_names[i], g_strerror(-rc));
            ok = FALSE;
        }
        g_free(pin);
    }
    for (guint i = 0; i < G_N_ELEMENTS(g_map_names) && ok; i++) {
        struct bpf_map *map = bpf_object__find_map_by_name(object, g_map_names[i]);
        gchar *pin = g_build_filename(pin_dir, g_map_names[i], NULL);
        rc = map ? bpf_map__pin(map, pin) : -ENOENT;
        if (rc != 0) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "cannot pin shared bridge map '%s': %s",
                        g_map_names[i], g_strerror(-rc));
            ok = FALSE;
        }
        g_free(pin);
    }
    bpf_object__close(object);
    g_free(object_path);
    if (!ok) _remove_known_pins(pin_dir);
    return ok;
}

                                                                   
                                                                        
                                           
static int
_open_pin(const gchar *name)
{
    if (!g_shared_pin_dir) return -ENOENT;
    gchar *path = g_build_filename(g_shared_pin_dir, name, NULL);
    int fd = bpf_obj_get(path);
    g_free(path);
    return fd;
}

static gboolean
_program_id(int fd, __u32 *id_out, GError **error)
{
    struct bpf_prog_info info = {};
    __u32 length = sizeof(info);
    if (bpf_obj_get_info_by_fd(fd, &info, &length) != 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "cannot inspect pinned shared bridge program: %s",
                    g_strerror(errno));
        return FALSE;
    }
    *id_out = info.id;
    return TRUE;
}

static gboolean
_tc_attach_owned(int ifindex,
                 enum bpf_tc_attach_point point,
                 __u32 handle,
                 int program_fd,
                 GError **error)
{
    LIBBPF_OPTS(bpf_tc_hook, hook,
        .ifindex = ifindex,
        .attach_point = point);
    int rc = bpf_tc_hook_create(&hook);
    if (rc != 0 && rc != -EEXIST) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "cannot create clsact on ifindex %d: %s", ifindex,
                    g_strerror(-rc));
        return FALSE;
    }

    LIBBPF_OPTS(bpf_tc_opts, query,
        .handle = handle,
        .priority = PCV_SHARED_TC_PRIORITY);
    rc = bpf_tc_query(&hook, &query);
    gboolean replace = rc == 0;
                                                                      
                                                                        
                                          
    if (rc != 0 && rc != -ENOENT && rc != -EINVAL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "cannot inspect reserved TC filter on ifindex %d: %s",
                    ifindex, g_strerror(-rc));
        return FALSE;
    }
    if (replace) {
        __u32 expected_id = 0;
        if (!_program_id(program_fd, &expected_id, error)) return FALSE;
        if (query.prog_id != expected_id) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                        "reserved TC handle collision on ifindex %d (owner program %u)",
                        ifindex, query.prog_id);
            return FALSE;
        }
    }

    LIBBPF_OPTS(bpf_tc_opts, attach,
        .prog_fd = program_fd,
        .flags = replace ? BPF_TC_F_REPLACE : 0,
        .handle = handle,
        .priority = PCV_SHARED_TC_PRIORITY);
    rc = bpf_tc_attach(&hook, &attach);
    if (rc != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "cannot attach shared bridge TC filter on ifindex %d: %s",
                    ifindex, g_strerror(-rc));
        return FALSE;
    }
    return TRUE;
}

  
                                                                     
  
                                                    
                                                             
                                             
  
                                                  
   
static gboolean
_tc_detach_owned(int ifindex,
                 enum bpf_tc_attach_point point,
                 __u32 handle,
                 int expected_program_fd,
                 GError **error)
{
    LIBBPF_OPTS(bpf_tc_hook, hook,
        .ifindex = ifindex,
        .attach_point = point);
    LIBBPF_OPTS(bpf_tc_opts, query,
        .handle = handle,
        .priority = PCV_SHARED_TC_PRIORITY);
    int rc = bpf_tc_query(&hook, &query);
    if (rc == -ENOENT || rc == -EINVAL) return TRUE;
    if (rc != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "cannot inspect shared bridge TC filter on ifindex %d: %s",
                    ifindex, g_strerror(-rc));
        return FALSE;
    }
    __u32 expected_id = 0;
    if (!_program_id(expected_program_fd, &expected_id, error)) return FALSE;
    if (query.prog_id != expected_id) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "refusing to detach foreign TC filter on ifindex %d (program %u)",
                    ifindex, query.prog_id);
        return FALSE;
    }
    LIBBPF_OPTS(bpf_tc_opts, detach,
        .handle = handle,
        .priority = PCV_SHARED_TC_PRIORITY);
    rc = bpf_tc_detach(&hook, &detach);
    if (rc != 0 && rc != -ENOENT) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "cannot detach shared bridge TC filter on ifindex %d: %s",
                    ifindex, g_strerror(-rc));
        return FALSE;
    }
    return TRUE;
}

static gboolean
_link_config_update(int map_fd,
                    __u32 key,
                    const struct pcv_shared_link_config *config,
                    GError **error)
{
    if (bpf_map_update_elem(map_fd, &key, config, BPF_ANY) != 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "cannot update shared bridge link map for ifindex %u: %s",
                    key, g_strerror(errno));
        return FALSE;
    }
    return TRUE;
}
#endif

  
                                                                          
  
                                                                      
                                                              
                                                             
                                                    
  
                                                    
                             
   
gboolean
pcv_shared_bridge_bpf_prepare(const gchar *store_dir, GError **error)
{
#ifndef HAVE_LIBBPF
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "shared uplink requires a libbpf-enabled build");
    return FALSE;
#else
    g_mutex_lock(&g_shared_bpf_mu);
    GError *local = NULL;
    GPtrArray *manifest = pcv_bpf_manifest_load(store_dir, &local);
    if (!manifest) {
        g_propagate_error(error, local);
        g_mutex_unlock(&g_shared_bpf_mu);
        return FALSE;
    }
    const PcvBpfManifestEntry *entry = _find_network_entry(manifest);
    if (!entry || strlen(entry->sha256) != 64) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "shared bridge BPF manifest entry is missing or invalid");
        g_ptr_array_unref(manifest);
        g_mutex_unlock(&g_shared_bpf_mu);
        return FALSE;
    }
    if (!pcv_bpf_verify_sha(store_dir, entry, error)) {
        g_ptr_array_unref(manifest);
        g_mutex_unlock(&g_shared_bpf_mu);
        return FALSE;
    }
    gchar generation[17] = {0};
    memcpy(generation, entry->sha256, 16);
    gchar *dir_name = g_strdup_printf("pcv_shared_bridge_%s", generation);
    gchar *pin_dir = g_build_filename(PCV_BPF_PIN_BASE, dir_name, NULL);
    g_free(dir_name);
    if (g_file_test(pin_dir, G_FILE_TEST_IS_DIR) && !_pin_set_complete(pin_dir))
        _remove_known_pins(pin_dir);
    if (!g_file_test(pin_dir, G_FILE_TEST_IS_DIR)
        && !_load_and_pin(store_dir, entry, pin_dir, error)) {
        g_free(pin_dir);
        g_ptr_array_unref(manifest);
        g_mutex_unlock(&g_shared_bpf_mu);
        return FALSE;
    }

    g_free(g_shared_pin_dir);
    g_shared_pin_dir = pin_dir;
    g_strlcpy(g_shared_sha, entry->sha256, sizeof(g_shared_sha));
    g_shared_prepared = TRUE;
    PCV_LOG_INFO(SHARED_LOG_DOM, "shared bridge BPF prepared sha=%.*s", 16, g_shared_sha);
    g_ptr_array_unref(manifest);
    g_mutex_unlock(&g_shared_bpf_mu);
    return TRUE;
#endif
}

gboolean
pcv_shared_bridge_bpf_is_prepared(void)
{
    return g_shared_prepared;
}

const gchar *
pcv_shared_bridge_bpf_sha256(void)
{
    return g_shared_prepared ? g_shared_sha : NULL;
}

  
                                                                                 
  
                                                                      
                                                       
                                                            
                                                                      
                                                    
  
                                                   
   
gboolean
pcv_shared_bridge_attach(const gchar *physical_if,
                         const gchar *portal_if,
                         const guint8 host_mac[6],
                         guint32 mtu,
                         guint32 generation,
                         GError **error)
{
#ifndef HAVE_LIBBPF
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "shared uplink requires a libbpf-enabled build");
    return FALSE;
#else
    if (!g_shared_prepared || !physical_if || !portal_if || !host_mac
        || generation == 0 || mtu == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "shared bridge BPF is not prepared or attach parameters are invalid");
        return FALSE;
    }
    __u32 physical = if_nametoindex(physical_if);
    __u32 portal = if_nametoindex(portal_if);
    if (physical == 0 || portal == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "shared bridge physical or portal interface does not exist");
        return FALSE;
    }

    g_mutex_lock(&g_shared_bpf_mu);
    int links_fd = _open_pin("pcv_sh_links");
    int phys_ing_fd = _open_pin("pcv_phys_ing");
    int phys_eg_fd = _open_pin("pcv_phys_eg");
    int portal_fd = _open_pin("pcv_portal_ing");
    if (links_fd < 0 || phys_ing_fd < 0 || phys_eg_fd < 0 || portal_fd < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "shared bridge BPF pins are incomplete");
        goto fail;
    }

    struct pcv_shared_link_config physical_cfg = {
        .revision = PCV_SHARED_BPF_REVISION,
        .generation = generation,
        .active = 0,
        .role = PCV_SHARED_ROLE_PHYSICAL,
        .peer_ifindex = portal,
        .physical_ifindex = physical,
        .mtu = mtu,
    };
    struct pcv_shared_link_config portal_cfg = physical_cfg;
    portal_cfg.role = PCV_SHARED_ROLE_PORTAL;
    portal_cfg.peer_ifindex = physical;
    memcpy(physical_cfg.host_mac, host_mac, 6);
    memcpy(portal_cfg.host_mac, host_mac, 6);
    if (!_link_config_update(links_fd, physical, &physical_cfg, error)
        || !_link_config_update(links_fd, portal, &portal_cfg, error))
        goto fail;
    if (!_tc_attach_owned((int)physical, BPF_TC_INGRESS,
                          PCV_SHARED_TC_HANDLE_PHYS_INGRESS, phys_ing_fd, error)
        || !_tc_attach_owned((int)physical, BPF_TC_EGRESS,
                             PCV_SHARED_TC_HANDLE_PHYS_EGRESS, phys_eg_fd, error)
        || !_tc_attach_owned((int)portal, BPF_TC_INGRESS,
                             PCV_SHARED_TC_HANDLE_PORTAL, portal_fd, error))
        goto rollback;

    portal_cfg.active = 1;
    physical_cfg.active = 1;
    if (!_link_config_update(links_fd, portal, &portal_cfg, error)
        || !_link_config_update(links_fd, physical, &physical_cfg, error))
        goto rollback;
    close(portal_fd); close(phys_eg_fd); close(phys_ing_fd); close(links_fd);
    g_mutex_unlock(&g_shared_bpf_mu);
    return TRUE;

rollback:
    physical_cfg.active = 0;
    portal_cfg.active = 0;
    _link_config_update(links_fd, physical, &physical_cfg, NULL);
    _link_config_update(links_fd, portal, &portal_cfg, NULL);
    _tc_detach_owned((int)portal, BPF_TC_INGRESS,
                     PCV_SHARED_TC_HANDLE_PORTAL, portal_fd, NULL);
    _tc_detach_owned((int)physical, BPF_TC_EGRESS,
                     PCV_SHARED_TC_HANDLE_PHYS_EGRESS, phys_eg_fd, NULL);
    _tc_detach_owned((int)physical, BPF_TC_INGRESS,
                     PCV_SHARED_TC_HANDLE_PHYS_INGRESS, phys_ing_fd, NULL);
    bpf_map_delete_elem(links_fd, &portal);
    bpf_map_delete_elem(links_fd, &physical);
fail:
    if (portal_fd >= 0) close(portal_fd);
    if (phys_eg_fd >= 0) close(phys_eg_fd);
    if (phys_ing_fd >= 0) close(phys_ing_fd);
    if (links_fd >= 0) close(links_fd);
    g_mutex_unlock(&g_shared_bpf_mu);
    return FALSE;
#endif
}

  
                                                                         
                                                               
                                                             
                                                          
  
                                                   
                                        
   
gboolean
pcv_shared_bridge_detach(const gchar *physical_if,
                         const gchar *portal_if,
                         GError **error)
{
#ifndef HAVE_LIBBPF
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "shared uplink requires a libbpf-enabled build");
    return FALSE;
#else
    __u32 physical = if_nametoindex(physical_if);
    __u32 portal = if_nametoindex(portal_if);
    if (physical == 0) return TRUE;
    g_mutex_lock(&g_shared_bpf_mu);
    int links_fd = _open_pin("pcv_sh_links");
    int phys_ing_fd = _open_pin("pcv_phys_ing");
    int phys_eg_fd = _open_pin("pcv_phys_eg");
    int portal_fd = _open_pin("pcv_portal_ing");
    if (links_fd < 0 || phys_ing_fd < 0 || phys_eg_fd < 0 || portal_fd < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "shared bridge BPF pins are incomplete during detach");
        goto out_fail;
    }
    struct pcv_shared_link_config cfg = {};
    if (bpf_map_lookup_elem(links_fd, &physical, &cfg) == 0) {
        cfg.active = 0;
        _link_config_update(links_fd, physical, &cfg, NULL);
    }
    if (portal != 0 && bpf_map_lookup_elem(links_fd, &portal, &cfg) == 0) {
        cfg.active = 0;
        _link_config_update(links_fd, portal, &cfg, NULL);
    }
    if (portal != 0
        && !_tc_detach_owned((int)portal, BPF_TC_INGRESS,
                             PCV_SHARED_TC_HANDLE_PORTAL, portal_fd, error))
        goto out_fail;
    if (!_tc_detach_owned((int)physical, BPF_TC_EGRESS,
                          PCV_SHARED_TC_HANDLE_PHYS_EGRESS, phys_eg_fd, error)
        || !_tc_detach_owned((int)physical, BPF_TC_INGRESS,
                             PCV_SHARED_TC_HANDLE_PHYS_INGRESS, phys_ing_fd, error))
        goto out_fail;
    if (portal != 0) bpf_map_delete_elem(links_fd, &portal);
    bpf_map_delete_elem(links_fd, &physical);
    close(portal_fd); close(phys_eg_fd); close(phys_ing_fd); close(links_fd);
    g_mutex_unlock(&g_shared_bpf_mu);
    return TRUE;
out_fail:
    if (portal_fd >= 0) close(portal_fd);
    if (phys_eg_fd >= 0) close(phys_eg_fd);
    if (phys_ing_fd >= 0) close(phys_ing_fd);
    if (links_fd >= 0) close(links_fd);
    g_mutex_unlock(&g_shared_bpf_mu);
    return FALSE;
#endif
}
