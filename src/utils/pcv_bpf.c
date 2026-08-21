   
                  
                                                                                     
  
                           
                                                   
                                                    
                                        
  
                                                       
                                                     
                                                         
                                                          
                                
  
                                                       
            
                                                                               
                                                              
                                                                   
                                                            
                                                                       
                                                                               
                                      
  
                                                                 
                                                       
                                                      
                                                                     
                                                       
  
                                                                
  
                                                                     
                                                           
                                        
                                                             
                                                             
                                     
                                                                    
                                                            
                                                                      
   
#include "utils/pcv_bpf.h"
#include "utils/pcv_log.h"                                     
#include "bpf/pcv_bpf_shared.h"                                                                    
#include "modules/security/security_event.h"                                  
#include "modules/security/security_store.h"                                       
#include "modules/daemons/prometheus_exporter.h"                                       
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>                                                    

#ifdef HAVE_LIBBPF
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include <fcntl.h>                                                                       
#include <errno.h>
#include <string.h>                                
#include <unistd.h>                                             
#endif

#define BPF_LOG_DOM "bpf"

                                   
#define PCV_BPF_PIN_BASE   "/sys/fs/bpf/purecvisor"
#define PCV_BPF_STATE_PATH "/var/lib/purecvisor/bpf_state.json"

static gboolean       g_sealed = FALSE;
static PcvBpfHealth   g_health = PCV_BPF_OK;
static gchar         *g_health_reason = NULL;              
static gint           g_loaded_count = 0;                                         
                                                                                            
                                                                              
                                                                                     
                                                                                            
                                                                                             
                                                                                          
                                                                                       

GQuark pcv_bpf_error_quark(void) {
    return g_quark_from_static_string("pcv-bpf-error-quark");
}

                                                     
                                                  
                                                            

                                                                
                                                              
gboolean pcv_bpf_caps_btf_path(const char *btf_path) {
    return g_file_test(btf_path, G_FILE_TEST_EXISTS);
}

                                                              
                                                      
                                   
                                                 
gboolean pcv_bpf_caps_lsm_path(const char *lsm_path) {
    gchar *buf = NULL; gsize len = 0;
    if (!g_file_get_contents(lsm_path, &buf, &len, NULL)) return FALSE;
                                              
                                                          
    gboolean found = FALSE;
    gchar **toks = g_strsplit(g_strstrip(buf), ",", -1);
    for (gchar **t = toks; *t; t++) {
        if (g_strcmp0(g_strstrip(*t), "bpf") == 0) { found = TRUE; break; }
    }
    g_strfreev(toks); g_free(buf);
    return found;
}

gboolean pcv_bpf_caps_btf(void) { return pcv_bpf_caps_btf_path("/sys/kernel/btf/vmlinux"); }
gboolean pcv_bpf_caps_lsm(void) { return pcv_bpf_caps_lsm_path("/sys/kernel/security/lsm"); }

                                                
                                                               
                                                         
static const char *j_str(JsonObject *o, const char *key) {
    if (json_object_has_member(o, key)) {
        JsonNode *n = json_object_get_member(o, key);
        if (n && JSON_NODE_HOLDS_VALUE(n)) {
            const char *s = json_node_get_string(n);
            if (s) return s;
        }
    }
    return "";
}

                                                   
                                  
                                
                                 
                               
                                 
                                                       
  
                                                                 
                                                  
                                                   
                                                
   
PcvBpfRehydrateAction pcv_bpf_rehydrate_decide(
    const char *state_sha, gboolean pin_exists,
    gboolean state_exists, const char *manifest_sha)
{
    if (!pin_exists && !state_exists) return PCV_BPF_REHYDRATE_FRESH;                  
    if (pin_exists && !state_exists)  return PCV_BPF_REHYDRATE_ORPHAN;                  
    if (!pin_exists && state_exists)  return PCV_BPF_REHYDRATE_FRESH;                                
                                                                                   
    if (g_strcmp0(state_sha, manifest_sha) == 0) return PCV_BPF_REHYDRATE_REATTACH;                     
    return PCV_BPF_REHYDRATE_UPGRADE;                           
}

                                                               
                                                                   
                                                            
                                                        
                                                                    
                                                      
                                       
                                                  
                                                
GPtrArray *pcv_bpf_manifest_load(const char *store_dir, GError **error) {
    if (!store_dir) {
        g_set_error(error, PCV_BPF_ERROR, PCV_BPF_ERROR_MANIFEST,
                    "manifest_load: store_dir is NULL");
        return NULL;
    }
    gchar *path = g_build_filename(store_dir, "manifest.json", NULL);
    JsonParser *p = json_parser_new();
    GError *lerr = NULL;
    if (!json_parser_load_from_file(p, path, &lerr)) {
        g_set_error(error, PCV_BPF_ERROR, PCV_BPF_ERROR_MANIFEST,
                    "manifest load %s: %s", path, lerr ? lerr->message : "unknown");
        g_clear_error(&lerr);
        g_object_unref(p); g_free(path);
        return NULL;
    }
    JsonNode *root = json_parser_get_root(p);
    if (!root || !JSON_NODE_HOLDS_ARRAY(root)) {
        g_set_error(error, PCV_BPF_ERROR, PCV_BPF_ERROR_MANIFEST,
                    "manifest %s: root is not a JSON array", path);
        g_object_unref(p); g_free(path);
        return NULL;
    }
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    JsonArray *arr = json_node_get_array(root);
    guint n = json_array_get_length(arr);
    for (guint i = 0; i < n; i++) {
        JsonNode *en = json_array_get_element(arr, i);
        if (!en || !JSON_NODE_HOLDS_OBJECT(en)) continue;                                      
        JsonObject *o = json_node_get_object(en);
        PcvBpfManifestEntry *e = g_new0(PcvBpfManifestEntry, 1);
        g_strlcpy(e->name,    j_str(o, "name"),               sizeof e->name);
                                                                  
                                                                                
                                                           
                                                         
                                                           
                                           
        if (e->name[0] == '\0' ||
            g_strstr_len(e->name, -1, "/") ||
            g_strstr_len(e->name, -1, "..")) {
            PCV_LOG_WARN(BPF_LOG_DOM,
                "manifest 엔트리[%u] name='%s' 거부 — 경로 순회 위험('/' 또는 '..' 또는 빈 이름)",
                i, e->name);
            g_free(e);
            continue;
        }
                                                         
        g_strlcpy(e->file,    j_str(o, "file"),               sizeof e->file);
        g_strlcpy(e->sha256,  j_str(o, "sha256"),             sizeof e->sha256);
        g_strlcpy(e->min_ver, j_str(o, "min_daemon_version"), sizeof e->min_ver);
        g_strlcpy(e->loader,  j_str(o, "loader"),             sizeof e->loader);
        if (e->loader[0] == '\0') g_strlcpy(e->loader, "auto", sizeof e->loader);
                                                              
        if (json_object_has_member(o, "requires")) {
            JsonNode *rn = json_object_get_member(o, "requires");
            if (rn && JSON_NODE_HOLDS_ARRAY(rn)) {
                JsonArray *req = json_node_get_array(rn);
                for (guint j = 0; j < json_array_get_length(req); j++) {
                    const char *r = json_array_get_string_element(req, j);
                    if (g_strcmp0(r, "btf") == 0)          e->req_btf = TRUE;
                    else if (g_strcmp0(r, "lsm-bpf") == 0) e->req_lsm = TRUE;
                }
            }
        }
        g_ptr_array_add(out, e);
    }
    g_object_unref(p); g_free(path);
    return out;
}

                                                            
                                                                        
                                                                     
                                                        
                                                             
                                                      
                                         
gboolean pcv_bpf_verify_sha(const char *store_dir,
                            const PcvBpfManifestEntry *e, GError **error) {
    if (!store_dir || !e) {
        g_set_error(error, PCV_BPF_ERROR, PCV_BPF_ERROR_SHA_MISMATCH,
                    "verify_sha: NULL argument");
        return FALSE;
    }
    gchar *path = g_build_filename(store_dir, e->file, NULL);
    gchar *data = NULL; gsize len = 0;
    GError *lerr = NULL;
    if (!g_file_get_contents(path, &data, &len, &lerr)) {
        g_set_error(error, PCV_BPF_ERROR, PCV_BPF_ERROR_SHA_MISMATCH,
                    "verify_sha: cannot read %s: %s",
                    path, lerr ? lerr->message : "unknown");
        g_clear_error(&lerr); g_free(path);
        return FALSE;
    }
    gchar *got = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                             (const guchar *)data, len);
    gboolean ok = (g_ascii_strcasecmp(got, e->sha256) == 0);
    if (!ok) {
        g_set_error(error, PCV_BPF_ERROR, PCV_BPF_ERROR_SHA_MISMATCH,
                    "verify_sha: %s sha256 mismatch (manifest=%s actual=%s)",
                    e->file, e->sha256, got);
    }
    g_free(got); g_free(data); g_free(path);
    return ok;
}

                                                                 
                                                            
                                                    
                                                          
                                                              
                                                                 
                                                            
                                                      
                                
                                                                 
                                                     
                                                       
                                         
guint pcv_bpf_count_pinned_links_path(const char *pindir) {
    guint n = 0;
    GDir *d = g_dir_open(pindir, 0, NULL);
    if (!d) return 0;
    const char *name;
    while ((name = g_dir_read_name(d)))
        if (g_str_has_prefix(name, "link_")) n++;                              
    g_dir_close(d);
    return n;
}

#ifdef HAVE_LIBBPF
                                                             
                                       
                                                               

                                                         
  
                                                               
                                                             
                                                                   
                                                        
                                             
                                                            
                              
                                                          
                                                  
static guint64 pcv_bpf__self_cgroup_id(void) {
    gchar *content = NULL;
    if (!g_file_get_contents("/proc/self/cgroup", &content, NULL, NULL)) return 0;
    gchar **lines = g_strsplit(content, "\n", -1);
    g_free(content);
    gchar *cgpath = NULL;
    for (gchar **l = lines; *l; l++)
        if (g_str_has_prefix(*l, "0::")) { cgpath = g_strdup(*l + 3); break; }
    g_strfreev(lines);
    if (!cgpath) return 0;
    gchar *full = g_build_filename("/sys/fs/cgroup", g_strstrip(cgpath), NULL);
    g_free(cgpath);

                                                              
    struct file_handle *fh = g_malloc0(sizeof(*fh) + MAX_HANDLE_SZ);
    fh->handle_bytes = MAX_HANDLE_SZ;
    int mount_id = 0;
    guint64 cgid = 0;
                                                                         
    if (name_to_handle_at(AT_FDCWD, full, fh, &mount_id, 0) == 0 &&
        fh->handle_bytes >= sizeof(guint64)) {
        memcpy(&cgid, fh->f_handle, sizeof cgid);
    } else {
        PCV_LOG_WARN(BPF_LOG_DOM, "cgroup id 취득 실패 (%s): %s",
                     full, g_strerror(errno));
    }
    g_free(fh); g_free(full);
    return cgid;
}

                             
                                                                 
                                          
static gboolean pcv_bpf__ensure_dir(const char *path, GError **error) {
    if (g_mkdir_with_parents(path, 0700) != 0) {
        g_set_error(error, PCV_BPF_ERROR, PCV_BPF_ERROR_PIN,
                    "mkdir %s: %s", path, g_strerror(errno));
        return FALSE;
    }
    return TRUE;
}

                                                 
                                                      
                                                         
                                                  
static void pcv_bpf__rmrf(const char *path) {
    GDir *d = g_dir_open(path, 0, NULL);
    if (d) {
        const char *name;
        while ((name = g_dir_read_name(d))) {
            gchar *child = g_build_filename(path, name, NULL);
                                                              
            if (g_file_test(child, G_FILE_TEST_IS_DIR) &&
                !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
                pcv_bpf__rmrf(child);                   
            else
                g_unlink(child);                         
            g_free(child);
        }
        g_dir_close(d);
    }
    g_rmdir(path);
}

                                                          
                                                          
                                                          
                                                            
static gchar *pcv_bpf__state_lookup_sha(const char *name) {
    if (!g_file_test(PCV_BPF_STATE_PATH, G_FILE_TEST_EXISTS)) return NULL;
    JsonParser *p = json_parser_new();
    gchar *sha = NULL;
    if (json_parser_load_from_file(p, PCV_BPF_STATE_PATH, NULL)) {
        JsonNode *root = json_parser_get_root(p);
        if (root && JSON_NODE_HOLDS_ARRAY(root)) {
            JsonArray *arr = json_node_get_array(root);
            for (guint i = 0; i < json_array_get_length(arr); i++) {
                JsonObject *o = json_array_get_object_element(arr, i);
                if (o && g_strcmp0(j_str(o, "name"), name) == 0) {
                    const char *s = j_str(o, "sha256");
                    sha = (s && *s) ? g_strdup(s) : NULL;
                    break;
                }
            }
        }
    }
    g_object_unref(p);
    return sha;
}

                                                       
                                                        
                                                         
                                    
                                                                
                                                     
                             
static gboolean pcv_bpf__state_upsert(const PcvBpfManifestEntry *e,
                                      GPtrArray *pins, GError **error) {
    gchar *dir = g_path_get_dirname(PCV_BPF_STATE_PATH);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    JsonArray *out = json_array_new();
    if (g_file_test(PCV_BPF_STATE_PATH, G_FILE_TEST_EXISTS)) {
        JsonParser *p = json_parser_new();
        if (json_parser_load_from_file(p, PCV_BPF_STATE_PATH, NULL)) {
            JsonNode *root = json_parser_get_root(p);
            if (root && JSON_NODE_HOLDS_ARRAY(root)) {
                JsonArray *arr = json_node_get_array(root);
                for (guint i = 0; i < json_array_get_length(arr); i++) {
                    JsonObject *o = json_array_get_object_element(arr, i);
                    if (o && g_strcmp0(j_str(o, "name"), e->name) == 0)
                        continue;                   
                    json_array_add_element(out,
                        json_node_copy(json_array_get_element(arr, i)));
                }
            }
        }
        g_object_unref(p);
    }

                                     
    JsonObject *rec = json_object_new();
    json_object_set_string_member(rec, "name", e->name);
    json_object_set_string_member(rec, "sha256", e->sha256);
    JsonArray *pinarr = json_array_new();
    if (pins)
        for (guint i = 0; i < pins->len; i++)                             
            json_array_add_string_element(pinarr, g_ptr_array_index(pins, i));
    json_object_set_array_member(rec, "pins", pinarr);
    json_object_set_int_member(rec, "loaded_at", g_get_real_time() / G_USEC_PER_SEC);
    json_array_add_object_element(out, rec);

    JsonNode *rootn = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(rootn, out);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, rootn);
    json_generator_set_pretty(gen, TRUE);
    gboolean ok = json_generator_to_file(gen, PCV_BPF_STATE_PATH, error);
    g_object_unref(gen);
    json_node_free(rootn);                       
    return ok;
}

                                                             
                                                         
                                                            
                                                               
  
                                                             
                                                  
                                                   
                                                                   
                                                         
                                                            
static gboolean pcv_bpf__populate_scoping_maps(int cgroup_map_fd, GError **error) {
    gboolean ok = TRUE;

                                                                     
    if (cgroup_map_fd >= 0) {
        guint64 cgid = pcv_bpf__self_cgroup_id();
        if (cgid) {
            guint32 key = 0;
            if (bpf_map_update_elem(cgroup_map_fd, &key, &cgid, BPF_ANY) != 0) {
                PCV_LOG_WARN(BPF_LOG_DOM, "pcv_daemon_cgroup 기록 실패: %s",
                             g_strerror(errno));
                ok = FALSE;
            }
        } else {
            PCV_LOG_WARN(BPF_LOG_DOM,
                         "daemon cgroup id 미취득 — 스코핑 비활성(광역 감사)");
            ok = FALSE;
        }
    }

                                                           
                                                                        
                                                             
                                                                 
                                                                  

    if (!ok)
        g_set_error(error, PCV_BPF_ERROR, PCV_BPF_ERROR_PIN,
                    "스코핑 맵(pcv_daemon_cgroup) 갱신 실패");
    return ok;
}

                              
                                                          
                                                                      
                                                                           
                                                                 
                                                                  
                                           
                                                 
                                                        
                                                   
                                                    
gboolean pcv_bpf_load_and_pin(const char *store_dir,
                              const PcvBpfManifestEntry *entry, GError **error) {
                                                              
                                                                  
                                                    
    if (g_sealed) {
        g_set_error(error, PCV_BPF_ERROR, PCV_BPF_ERROR_LOAD,
                    "BPF load window sealed — 런타임 로드/attach 거부 (부팅 후)");
        return FALSE;
    }
    if (!store_dir || !entry) {
        g_set_error(error, PCV_BPF_ERROR, PCV_BPF_ERROR_LOAD, "load_and_pin: NULL argument");
        return FALSE;
    }

                   
    if (!pcv_bpf_verify_sha(store_dir, entry, error)) return FALSE;

                                                      
    gchar *pindir = g_build_filename(PCV_BPF_PIN_BASE, entry->name, NULL);
    if (!pcv_bpf__ensure_dir(pindir, error)) { g_free(pindir); return FALSE; }

                                                  
    gchar *obj_path = g_build_filename(store_dir, entry->file, NULL);
    struct bpf_object *obj = bpf_object__open_file(obj_path, NULL);
    if (!obj) {
        g_set_error(error, PCV_BPF_ERROR, PCV_BPF_ERROR_LOAD,
                    "bpf_object__open_file(%s): %s", obj_path, g_strerror(errno));
        g_free(obj_path); g_free(pindir);
        return FALSE;
    }
    int lret = bpf_object__load(obj);
    if (lret) {
        g_set_error(error, PCV_BPF_ERROR, PCV_BPF_ERROR_LOAD,
                    "bpf_object__load(%s): %s", entry->file, g_strerror(-lret));
        bpf_object__close(obj); g_free(obj_path); g_free(pindir);
        return FALSE;
    }

    GPtrArray *pins = g_ptr_array_new_with_free_func(g_free);
    gboolean ok = TRUE;

                                                    
                                                             
                                                       
                                                               
    pcv_bpf__populate_scoping_maps(
        bpf_object__find_map_fd_by_name(obj, "pcv_daemon_cgroup"), NULL);

                                 
                                            
                                                 
                                                          
                            
                                                                     
                                                    
                                                       
                                                       
    guint this_loaded = 0;
    struct bpf_program *prog;
    bpf_object__for_each_program(prog, obj) {
        struct bpf_link *link = bpf_program__attach(prog);
        if (!link) {
            g_set_error(error, PCV_BPF_ERROR, PCV_BPF_ERROR_PIN,
                        "attach %s: %s", bpf_program__name(prog), g_strerror(errno));
            ok = FALSE; break;
        }
        gchar *lp = g_strdup_printf("%s/link_%s", pindir, bpf_program__name(prog));
        int pret = bpf_link__pin(link, lp);
        if (pret) {
            g_set_error(error, PCV_BPF_ERROR, PCV_BPF_ERROR_PIN,
                        "pin link %s: %s", lp, g_strerror(-pret));
            bpf_link__destroy(link); g_free(lp);
            ok = FALSE; break;
        }
        g_ptr_array_add(pins, g_strdup(lp));
                                                                
        bpf_link__destroy(link);
        g_free(lp);
        this_loaded++;
    }

                                                               
                                                 
    if (ok) {
        struct bpf_map *map;
        bpf_object__for_each_map(map, obj) {
            const char *mname = bpf_map__name(map);
            if (!mname || strchr(mname, '.')) continue;
            gchar *mp = g_build_filename(pindir, mname, NULL);
            int pret = bpf_map__pin(map, mp);
                                                               
            if (pret && pret != -EEXIST) {
                g_set_error(error, PCV_BPF_ERROR, PCV_BPF_ERROR_PIN,
                            "pin map %s: %s", mp, g_strerror(-pret));
                g_free(mp); ok = FALSE; break;
            }
            g_ptr_array_add(pins, g_strdup(mp));
            g_free(mp);
        }
    }

                  
    if (ok && !pcv_bpf__state_upsert(entry, pins, error))
        ok = FALSE;

                                                         
                                                      
    if (!ok)
        pcv_bpf__rmrf(pindir);
    else
        g_loaded_count += this_loaded;                                       

                                                    
                                                          
    bpf_object__close(obj);
    g_ptr_array_unref(pins);
    g_free(obj_path); g_free(pindir);
    return ok;
}

                                                       
                                                           
                                                       
                                                     
                                                                     
                                                       
                                                        
gboolean pcv_bpf_rehydrate(const char *store_dir, GError **error) {
                                                                     
                                                           
    if (g_sealed) {
        g_set_error(error, PCV_BPF_ERROR, PCV_BPF_ERROR_LOAD,
                    "BPF load window sealed — 런타임 로드/attach 거부 (부팅 후)");
        return FALSE;
    }
    GError *lerr = NULL;
    GPtrArray *manifest = pcv_bpf_manifest_load(store_dir, &lerr);
    if (!manifest) {
        g_propagate_error(error, lerr);
        return FALSE;
    }

    gboolean all_ok = TRUE;
    for (guint i = 0; i < manifest->len; i++) {
        const PcvBpfManifestEntry *e = g_ptr_array_index(manifest, i);
                                                                                
                                                                           
                                                               
        if (g_strcmp0(e->loader, "network-tc") == 0) {
            PCV_LOG_INFO(BPF_LOG_DOM,
                         "rehydrate[%s]: network-tc loader에 위임", e->name);
            continue;
        }
                                                               
        gchar   *pindir       = g_build_filename(PCV_BPF_PIN_BASE, e->name, NULL);
        gboolean pin_exists   = g_file_test(pindir, G_FILE_TEST_IS_DIR);
        gchar   *state_sha    = pcv_bpf__state_lookup_sha(e->name);
        gboolean state_exists = (state_sha != NULL);

        PcvBpfRehydrateAction act =
            pcv_bpf_rehydrate_decide(state_sha, pin_exists, state_exists, e->sha256);

        GError  *eerr = NULL;
        gboolean ok   = TRUE;
        switch (act) {
        case PCV_BPF_REHYDRATE_FRESH:
            if (state_exists && !pin_exists)
                PCV_LOG_WARN(BPF_LOG_DOM,
                    "rehydrate[%s]: state 존재하나 핀 부재 — 외부 개입 흔적, FRESH 재로드",
                    e->name);
            else
                PCV_LOG_INFO(BPF_LOG_DOM, "rehydrate[%s]: FRESH — 신규 로드·핀", e->name);
            ok = pcv_bpf_load_and_pin(store_dir, e, &eerr);
            break;
        case PCV_BPF_REHYDRATE_REATTACH:
                                                              
                                                                 
                                                                   
                                                             
                                                                
            PCV_LOG_INFO(BPF_LOG_DOM, "rehydrate[%s]: REATTACH — 기존 핀 재사용", e->name);
            g_loaded_count += pcv_bpf_count_pinned_links_path(pindir);
                                                                               
                                                     
                                                                    
                                                                       
                                                             
                                                                     
            {
                gchar *cg_path = g_build_filename(pindir, "pcv_daemon_cgroup", NULL);
                int cg_fd = bpf_obj_get(cg_path);
                if (cg_fd >= 0) {
                    if (!pcv_bpf__populate_scoping_maps(cg_fd, &eerr)) {
                        PCV_LOG_WARN(BPF_LOG_DOM,
                            "rehydrate[%s]: REATTACH 스코핑 맵 갱신 실패 — degrade: %s",
                            e->name, eerr ? eerr->message : "unknown");
                        g_clear_error(&eerr);
                    }
                    close(cg_fd);
                } else {
                    PCV_LOG_WARN(BPF_LOG_DOM,
                        "rehydrate[%s]: REATTACH 스코핑 핀 획득 실패(cg=%d) — 갱신 생략",
                        e->name, cg_fd);
                }
                g_free(cg_path);
            }
            break;
        case PCV_BPF_REHYDRATE_UPGRADE:
            PCV_LOG_INFO(BPF_LOG_DOM,
                         "rehydrate[%s]: UPGRADE — 구 핀 unpin 후 재로드", e->name);
            pcv_bpf__rmrf(pindir);
            ok = pcv_bpf_load_and_pin(store_dir, e, &eerr);
            break;
        case PCV_BPF_REHYDRATE_ORPHAN:
            PCV_LOG_WARN(BPF_LOG_DOM,
                         "rehydrate[%s]: ORPHAN — stale 핀 정리 후 FRESH", e->name);
            pcv_bpf__rmrf(pindir);
            ok = pcv_bpf_load_and_pin(store_dir, e, &eerr);
            break;
        }

        if (!ok) {
                                                                 
                                                             
                                                                                
                                                            
            gboolean tamper = (eerr && eerr->domain == PCV_BPF_ERROR &&
                               eerr->code == PCV_BPF_ERROR_SHA_MISMATCH);
            if (tamper)
                PCV_LOG_ERROR(BPF_LOG_DOM,
                    "rehydrate[%s] 무결성 실패(.o sha256 불일치 — 변조 의심): %s",
                    e->name, eerr->message);
            else
                PCV_LOG_WARN(BPF_LOG_DOM, "rehydrate[%s] 실패: %s",
                             e->name, eerr ? eerr->message : "unknown");
                                         
            if (error && !*error && eerr) { *error = eerr; eerr = NULL; }
            g_clear_error(&eerr);
            all_ok = FALSE;
        }
        g_free(state_sha);
        g_free(pindir);
    }
    g_ptr_array_unref(manifest);
    return all_ok;
}

#else                                                          
                                                      
                                                    
                                       

gboolean pcv_bpf_load_and_pin(const char *store_dir,
                              const PcvBpfManifestEntry *entry, GError **error) {
    (void)store_dir; (void)entry;                           
                                                              
                                            
    if (g_sealed) {
        g_set_error(error, PCV_BPF_ERROR, PCV_BPF_ERROR_LOAD,
                    "BPF load window sealed — 런타임 로드/attach 거부 (부팅 후)");
        return FALSE;
    }
    g_set_error(error, PCV_BPF_ERROR, PCV_BPF_ERROR_NO_LIBBPF,
                "libbpf not compiled in — cannot load BPF object "
                "(install libbpf-dev + rebuild)");
    return FALSE;
}

                                                                          
                                                                     
                                                           
gboolean pcv_bpf_rehydrate(const char *store_dir, GError **error) {
    (void)store_dir;
    if (g_sealed) {                                   
        g_set_error(error, PCV_BPF_ERROR, PCV_BPF_ERROR_LOAD,
                    "BPF load window sealed — 런타임 로드/attach 거부 (부팅 후)");
        return FALSE;
    }
    g_set_error(error, PCV_BPF_ERROR, PCV_BPF_ERROR_NO_LIBBPF,
                "libbpf not compiled in — rehydrate unavailable");
    return FALSE;
}

#endif                  

                                                          
                                                                 
                                                               
                                                                        
                                                                      
                                                   
                                                       
                                                              
gboolean pcv_bpf_init(const char *store_dir, GError **error) {
    (void)error;                                                 

    if (!pcv_bpf_caps_btf()) {
        g_health = PCV_BPF_DEGRADED_NO_BTF;
        g_free(g_health_reason);
        g_health_reason = g_strdup("no BTF (/sys/kernel/btf/vmlinux) — eBPF subsystem disabled");
        PCV_LOG_WARN(BPF_LOG_DOM, "eBPF DEGRADED: %s", g_health_reason);
        return TRUE;                                  
    }

#ifndef HAVE_LIBBPF
                                                        
                                          
    g_health = PCV_BPF_DEGRADED_NO_BTF;
    g_free(g_health_reason);
    g_health_reason = g_strdup("libbpf not compiled in — eBPF load disabled "
                               "(install libbpf-dev + rebuild)");
    PCV_LOG_WARN(BPF_LOG_DOM, "eBPF DEGRADED: %s", g_health_reason);
    return TRUE;
#else
                                                             
                                                  
                                                          
    if (!pcv_bpf_caps_lsm()) {
        g_health = PCV_BPF_DEGRADED_NO_LSM;
        g_free(g_health_reason);
        g_health_reason = g_strdup("lsm= lacks 'bpf' — LSM audit disabled (add lsm=...,bpf + reboot)");
        PCV_LOG_WARN(BPF_LOG_DOM, "BPF-LSM DEGRADED: %s", g_health_reason);
        return TRUE;
    }

                                                           
                                                                  
                                                                 
                                                              
    if (store_dir) {
        GError *merr = NULL;
        GPtrArray *m = pcv_bpf_manifest_load(store_dir, &merr);
        if (!m) {
            PCV_LOG_WARN(BPF_LOG_DOM,
                         "manifest 로드 실패(%s) — 배포 시 설치 예정: %s",
                         store_dir, merr ? merr->message : "unknown");
            g_clear_error(&merr);
        } else {
            PCV_LOG_INFO(BPF_LOG_DOM, "manifest OK — %u program(s) from %s",
                         m->len, store_dir);
            g_ptr_array_unref(m);
        }
    }

    g_health = PCV_BPF_OK;
    g_free(g_health_reason); g_health_reason = NULL;
    return TRUE;
#endif
}

                                                                    
                                                  
                                                              
void         pcv_bpf_seal(void)        { g_sealed = TRUE; }
gboolean     pcv_bpf_is_sealed(void)   { return g_sealed; }
PcvBpfHealth pcv_bpf_health(void)      { return g_health; }
const char  *pcv_bpf_health_reason(void){ return g_health_reason; }                          

                                                        
                                                   
                            
void pcv_bpf_shutdown(void) {
                                                      
                                                        
                                                               
                                                  
                               
    g_free(g_health_reason); g_health_reason = NULL;
}

                                                             
                                     
                                                             
                                                       
                                                     
                                             
                                                     
                                           
  
                                                         
                                                 
                                                       
   
#ifdef HAVE_LIBBPF

                                                                              
#define PCV_BPF_LSM_ENTRY_NAME  "pcv_lsm"
#define PCV_BPF_LSM_RINGBUF_MAP "pcv_lsm_events"

static struct {
    volatile gint running;                                                     
    GThread *thread;
    int      rb_fd;
} g_consumer = { 0, NULL, -1 };

                                                           
                                                    
                                      
                                                                     
                                                            
                                                      
                                         
static int
pcv_bpf__rb_handle(void *ctx, void *data, size_t sz)
{
    (void)ctx;
    if (sz < sizeof(struct pcv_lsm_event)) return 0;                       
    const struct pcv_lsm_event *ev = data;

    PcvSecurityEvent se = {0};
                                                                  
                                                      
                                                  
                                                    
                                                         
    se.timestamp = g_get_real_time() / G_USEC_PER_SEC;
    se.source = PCV_SECURITY_SOURCE_LSM;
    se.severity = PCV_SECURITY_SEVERITY_WARN;
    se.confidence = 50;                                                
                                                    
    if (ev->hook == PCV_LSM_HOOK_BPRM) {
        se.type = PCV_SECURITY_EVENT_PROCESS_SUSPICIOUS;
        se.target_kind = PCV_SECURITY_TARGET_PROCESS;
    } else {
        se.type = PCV_SECURITY_EVENT_FILE_ACCESS_SENSITIVE;
        se.target_kind = PCV_SECURITY_TARGET_FILE;
    }
    g_strlcpy(se.target, ev->path[0] ? ev->path : ev->comm, sizeof se.target);
    g_snprintf(se.summary, sizeof se.summary,
        "LSM %s: comm=%s pid=%u path=%s",
        ev->hook == PCV_LSM_HOOK_BPRM ? "exec" : "file",
        ev->comm, ev->pid, ev->path);
    g_snprintf(se.evidence_json, sizeof se.evidence_json,
        "{\"cgroup_id\":%llu,\"pid\":%u,\"hook\":%u,\"ktime_ns\":%llu}",
        (unsigned long long)ev->cgroup_id, ev->pid, ev->hook,
        (unsigned long long)ev->ktime_ns);                               
    se.status = PCV_SECURITY_STATUS_OPEN;
    pcv_security_event_make_id(&se, "lsm");

    GError *err = NULL;
    if (!pcv_security_submit_event(&se, &err)) {                                  
        PCV_LOG_WARN(BPF_LOG_DOM, "LSM 이벤트 제출 실패: %s",
                     err ? err->message : "?");
        g_clear_error(&err);
    }
    return 0;
}

                                                               
                                                                        
                                           
                                                            
static gpointer
pcv_bpf__consumer_thread(gpointer arg)
{
    struct ring_buffer *rb = arg;
    guint poll_err_count = 0;
    PCV_LOG_INFO(BPF_LOG_DOM, "LSM ringbuf 소비 스레드 시작");
    while (g_atomic_int_get(&g_consumer.running)) {
                                                         
                                                       
        int n = ring_buffer__poll(rb, 200);
        if (n < 0 && n != -EINTR) {
                                                      
                                                 
                                                     
            poll_err_count++;
            if (poll_err_count == 1) {
                PCV_LOG_WARN(BPF_LOG_DOM, "ring_buffer__poll 오류: %s", g_strerror(-n));
            }
            g_usleep(200 * 1000);
        } else {
            poll_err_count = 0;
        }
    }
    ring_buffer__free(rb);                                             
    PCV_LOG_INFO(BPF_LOG_DOM, "LSM ringbuf 소비 스레드 종료");
    return NULL;
}

                                                                  
                                                                    
                                                                 
                                                     
                             
void
pcv_bpf_consumer_start(void)
{
    if (g_consumer.thread) {                         
        PCV_LOG_WARN(BPF_LOG_DOM, "consumer_start: 이미 실행 중 — 무시");
        return;
    }

    gchar *rb_path = g_build_filename(PCV_BPF_PIN_BASE, PCV_BPF_LSM_ENTRY_NAME,
                                      PCV_BPF_LSM_RINGBUF_MAP, NULL);
    int rb_fd = bpf_obj_get(rb_path);
    if (rb_fd < 0) {
                                                           
                                                      
        PCV_LOG_WARN(BPF_LOG_DOM,
                     "ringbuf 핀(%s) 획득 실패(%s) — LSM 이벤트 소비 비활성",
                     rb_path, g_strerror(errno));
        g_free(rb_path);
        return;
    }
    g_free(rb_path);

    struct ring_buffer *rb = ring_buffer__new(rb_fd, pcv_bpf__rb_handle, NULL, NULL);
    if (!rb) {
        PCV_LOG_WARN(BPF_LOG_DOM, "ring_buffer__new 실패 — LSM 이벤트 소비 비활성");
        close(rb_fd);
        return;
    }

    g_consumer.rb_fd = rb_fd;
    g_atomic_int_set(&g_consumer.running, 1);
    g_consumer.thread = g_thread_new("bpf-lsm-rb", pcv_bpf__consumer_thread, rb);
}

                                                                 
                                                               
                                                           
                                                             
void
pcv_bpf_consumer_stop(void)
{
    if (!g_consumer.thread) return;
    g_atomic_int_set(&g_consumer.running, 0);                                
    g_thread_join(g_consumer.thread);                              
    g_consumer.thread = NULL;
    close(g_consumer.rb_fd);
    g_consumer.rb_fd = -1;
}

#else                                         
                                                        
                                              

void pcv_bpf_consumer_start(void) {
    PCV_LOG_WARN(BPF_LOG_DOM,
                 "libbpf not compiled in — LSM ringbuf consumer unavailable");
}

void pcv_bpf_consumer_stop(void) {
                                                  
}

#endif                  

                                                             
                                             
                                                             
                                                           
                                                   
   
                                                                           
                                                                       
                                                              
                                                      
                                                 
void
pcv_bpf_metrics_tick(void)
{
    pcv_prom_gauge_set_labels("pcv_bpf_programs_loaded", "", (gdouble)g_loaded_count);
    pcv_prom_gauge_set_labels("pcv_bpf_degraded", "subsystem=\"btf\"",
        pcv_bpf_health() == PCV_BPF_DEGRADED_NO_BTF ? 1.0 : 0.0);
    pcv_prom_gauge_set_labels("pcv_bpf_degraded", "subsystem=\"lsm\"",
        pcv_bpf_health() == PCV_BPF_DEGRADED_NO_LSM ? 1.0 : 0.0);

                                                                                         
                                                                    
                                                               
                                                       
                                                                       
                                                       
                                         
    guint64 emitted = 0, dropped = 0;
#ifdef HAVE_LIBBPF
    gchar *cpath = g_build_filename(PCV_BPF_PIN_BASE, PCV_BPF_LSM_ENTRY_NAME,
                                    "pcv_lsm_counters", NULL);
    int cfd = bpf_obj_get(cpath);
    g_free(cpath);
    if (cfd >= 0) {
        guint32 k0 = 0, k1 = 1;
        guint64 v0 = 0, v1 = 0;
        if (bpf_map_lookup_elem(cfd, &k0, &v0) == 0) emitted = v0;
        if (bpf_map_lookup_elem(cfd, &k1, &v1) == 0) dropped = v1;
        close(cfd);
    }
#endif
    pcv_prom_gauge_set_labels("pcv_bpf_lsm_events_total", "", (gdouble)emitted);
    pcv_prom_gauge_set_labels("pcv_bpf_ringbuf_dropped_total", "", (gdouble)dropped);
}
