   
                     
                                                               
  
                           
                                                   
                                                    
                                        
  
          
                                                    
                                                    
                                                    
                                                    
                                                      
  
                                                       
  
                                                   
                                                      
                                                              
                                     
                                                                    
                                              
                                                         
                                                      
                                                   
   
#include "cgroup_psi.h"
#include "prometheus_exporter.h"
#include "utils/pcv_log.h"

#include <stdio.h>
#include <string.h>

                                                      
#define CGROUP_PSI_LOG_DOM "cgroup_psi"

                                                        
#define CGROUP_PSI_TRUNC_WARN_INTERVAL_SEC 300

                                                                
#define CGROUP_PSI_FILE_BUF 512

                                                                  

                                                              
                                                    
gboolean
pcv_cgroup_psi_unescape(const gchar *raw, gchar *out, gsize out_sz)
{
    if (!raw || !out || out_sz == 0) return FALSE;

    gsize o = 0;
    for (const gchar *p = raw; *p; ) {
        guchar c;
        if (p[0] == '\\' && p[1] == 'x' &&
            g_ascii_isxdigit((guchar)p[2]) && g_ascii_isxdigit((guchar)p[3])) {
                                                                      
                                                        
                                                      
                                                         
            c = (guchar)((g_ascii_xdigit_value(p[2]) << 4) | g_ascii_xdigit_value(p[3]));
            p += 4;
        } else if (p[0] == '\\') {
                                                               
                                                                 
            return FALSE;
        } else {
            c = (guchar)*p++;
        }

                                                             
                                                     
                                           
        if (c < 0x20 || c == 0x7f || c == '"' || c == '\\') return FALSE;

                                                     
                                     
        if (o + 1 >= out_sz) return FALSE;
        out[o++] = (gchar)c;
    }
    out[o] = '\0';
    return o > 0;
}

                                                                  

                                                                              
                                               
gboolean
pcv_cgroup_psi_ref_from_machine_dir(const gchar *dirname, PcvCgroupPsiRef *out)
{
    static const gchar PFX[] = "machine-";
    static const gchar SFX[] = ".scope";

    if (!dirname || !out) return FALSE;
    if (!g_str_has_prefix(dirname, PFX)) return FALSE;
    if (!g_str_has_suffix(dirname, SFX)) return FALSE;

    gsize total = strlen(dirname);
                                                  
                                                 
    gsize body_len = total - (sizeof(PFX) - 1) - (sizeof(SFX) - 1);
    gchar body[PCV_CGROUP_PSI_RELPATH_MAX];
    if (body_len == 0 || body_len >= sizeof(body)) return FALSE;
    memcpy(body, dirname + (sizeof(PFX) - 1), body_len);
    body[body_len] = '\0';

    const gchar *p = body;
                                                               
                                                           
    PcvCgroupPsiKind kind = PCV_CGROUP_PSI_KIND_CONTAINER;
    if (g_str_has_prefix(body, "qemu\\x2d")) {
        kind = PCV_CGROUP_PSI_KIND_VM;
        p = body + strlen("qemu\\x2d");
    } else if (g_str_has_prefix(body, "lxc\\x2d")) {
        kind = PCV_CGROUP_PSI_KIND_CONTAINER;
        p = body + strlen("lxc\\x2d");
    }

    if (p != body) {
                                                                  
                                                      
                                                        
                                           
        const gchar *q = p;
        while (g_ascii_isdigit((guchar)*q)) q++;
        if (q != p && g_str_has_prefix(q, "\\x2d"))
            p = q + strlen("\\x2d");
    }

    if (!pcv_cgroup_psi_unescape(p, out->name, sizeof(out->name))) return FALSE;
    out->kind = kind;
    g_snprintf(out->rel_path, sizeof(out->rel_path), "machine.slice/%s", dirname);
    return TRUE;
}

                                                              
                                          
gboolean
pcv_cgroup_psi_ref_from_lxc_dir(const gchar *parent_rel, const gchar *dirname,
                                PcvCgroupPsiRef *out)
{
    static const gchar PAYLOAD_PFX[] = "lxc.payload.";

    if (!dirname || !out) return FALSE;

    const gchar *raw_name;
    if (!parent_rel || parent_rel[0] == '\0') {
                                                            
                                                                     
        if (!g_str_has_prefix(dirname, PAYLOAD_PFX)) return FALSE;
        raw_name = dirname + (sizeof(PAYLOAD_PFX) - 1);
    } else {
                                                                   
        raw_name = dirname;
    }
    if (raw_name[0] == '\0') return FALSE;

                                                        
                                              
    if (!pcv_cgroup_psi_unescape(raw_name, out->name, sizeof(out->name))) return FALSE;

    out->kind = PCV_CGROUP_PSI_KIND_CONTAINER;
    if (!parent_rel || parent_rel[0] == '\0')
        g_snprintf(out->rel_path, sizeof(out->rel_path), "%s", dirname);
    else
        g_snprintf(out->rel_path, sizeof(out->rel_path), "%s/%s", parent_rel, dirname);
    return TRUE;
}

                                                                     

                                                             
                                                
gboolean
pcv_cgroup_psi_parse(const gchar *text, PcvCgroupPsiSample *out)
{
    if (!text || !out) return FALSE;
    memset(out, 0, sizeof(*out));                                    

    for (const gchar *line = text; *line; ) {
        const gchar *eol = strchr(line, '\n');
        gsize len = eol ? (gsize)(eol - line) : strlen(line);

        gchar tmp[256];
        if (len < sizeof(tmp)) {
            memcpy(tmp, line, len);
            tmp[len] = '\0';

            gchar type[16];
            gdouble a10 = 0, a60 = 0, a300 = 0;
            unsigned long long total_us = 0;
                                                                 
                                                        
                                   
            if (sscanf(tmp, "%15s avg10=%lf avg60=%lf avg300=%lf total=%llu",
                       type, &a10, &a60, &a300, &total_us) == 5) {
                if (strcmp(type, "some") == 0) {
                    out->have_some      = TRUE;
                    out->some_avg10     = a10;
                    out->some_avg60     = a60;
                    out->some_avg300    = a300;
                                                        
                                                               
                    out->some_total_sec = (gdouble)total_us / 1e6;
                } else if (strcmp(type, "full") == 0) {
                    out->have_full      = TRUE;
                    out->full_avg10     = a10;
                    out->full_avg60     = a60;
                    out->full_avg300    = a300;
                    out->full_total_sec = (gdouble)total_us / 1e6;
                }
            }
        }

        if (!eol) break;
        line = eol + 1;
    }
    return out->have_some || out->have_full;
}

                                                                           

   
                                            
  
                                               
                   
  
                                                                        
                                                    
                             
  
                        
                     
                             
                                         
                               
   
static void
_append_ref(PcvCgroupPsiRef *out, gint max_out, gint *n, gboolean *trunc,
            const PcvCgroupPsiRef *cand)
{
    for (gint i = 0; i < *n; i++) {
        if (out[i].kind == cand->kind && strcmp(out[i].name, cand->name) == 0)
            return;                                 
    }
    if (*n >= max_out) { *trunc = TRUE; return; }
    out[(*n)++] = *cand;
}

   
                                                
  
                                               
                 
  
                                                        
                                                   
                                                                  
  
                                   
                                                    
                                                                         
                                               
   
static void
_scan_dir(const gchar *cgroup_root, const gchar *rel, gboolean machine,
          PcvCgroupPsiRef *out, gint max_out, gint *n, gboolean *trunc)
{
    gchar *dir_path = (rel && rel[0])
        ? g_build_filename(cgroup_root, rel, NULL)
        : g_strdup(cgroup_root);

    GDir *d = g_dir_open(dir_path, 0, NULL);
    if (!d) { g_free(dir_path); return; }

    const gchar *ent;
    while ((ent = g_dir_read_name(d)) != NULL) {
                                                               
                                                       
                                                
        gchar *child = g_build_filename(dir_path, ent, NULL);
        gboolean is_dir = g_file_test(child, G_FILE_TEST_IS_DIR);
        g_free(child);
        if (!is_dir) continue;

        PcvCgroupPsiRef r;
        gboolean ok = machine
            ? pcv_cgroup_psi_ref_from_machine_dir(ent, &r)
            : pcv_cgroup_psi_ref_from_lxc_dir(rel, ent, &r);
        if (!ok) continue;

                                                            
                                                                  
        _append_ref(out, max_out, n, trunc, &r);
    }

    g_dir_close(d);
    g_free(dir_path);
}

                                                            
                                               
gint
pcv_cgroup_psi_scan(const gchar *cgroup_root, PcvCgroupPsiRef *out,
                    gint max_out, gboolean *truncated)
{
    if (truncated) *truncated = FALSE;
    if (!cgroup_root || !out || max_out <= 0) return 0;

    gint n = 0;
    gboolean trunc = FALSE;

                                                                         
    _scan_dir(cgroup_root, "machine.slice", TRUE, out, max_out, &n, &trunc);
                                                         
    _scan_dir(cgroup_root, NULL, FALSE, out, max_out, &n, &trunc);
                                                    
    _scan_dir(cgroup_root, "lxc", FALSE, out, max_out, &n, &trunc);

    if (truncated) *truncated = trunc;
    return n;
}

                                                                         

                                                                            
                                    
gboolean
pcv_cgroup_psi_read(const gchar *cgroup_root, const PcvCgroupPsiRef *ref,
                    const gchar *resource, PcvCgroupPsiSample *out)
{
    if (!cgroup_root || !ref || !resource || !out) return FALSE;

    gchar fname[64];
    g_snprintf(fname, sizeof(fname), "%s.pressure", resource);
    gchar *path = g_build_filename(cgroup_root, ref->rel_path, fname, NULL);

                                                    
                                                                  
    FILE *f = fopen(path, "r");
    g_free(path);
    if (!f) return FALSE;                                             

    gchar buf[CGROUP_PSI_FILE_BUF];
    size_t rd = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[rd] = '\0';

    return pcv_cgroup_psi_parse(buf, out);
}

                                                                    

                                                                    
                              
void
pcv_cgroup_psi_format_labels(const PcvCgroupPsiRef *ref, gchar *out, gsize out_sz)
{
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!ref) return;
    g_snprintf(out, out_sz, "vm_name=\"%s\",kind=\"%s\"",
               ref->name,
               ref->kind == PCV_CGROUP_PSI_KIND_VM ? "vm" : "container");
}

                                                    
                              
void
pcv_cgroup_psi_format_metric(const gchar *resource, const gchar *type,
                             const gchar *suffix, gchar *out, gsize out_sz)
{
    if (!out || out_sz == 0) return;
    g_snprintf(out, out_sz, "purecvisor_cgroup_pressure_%s_%s_%s",
               resource ? resource : "", type ? type : "", suffix ? suffix : "");
}

                                                                        

   
                                                   
  
                                                      
  
                                                                     
                                                        
                                                           
  
                                      
                                
                                                         
                                
                              
   
static void
_push_sample(const gchar *resource, const gchar *type, const gchar *labels,
             gdouble avg10, gdouble total)
{
    gchar metric[128];
    pcv_cgroup_psi_format_metric(resource, type, "seconds_total", metric, sizeof(metric));
    pcv_prom_gauge_set_labels(metric, labels, total);
    pcv_cgroup_psi_format_metric(resource, type, "avg10", metric, sizeof(metric));
    pcv_prom_gauge_set_labels(metric, labels, avg10);
}

                                                    
                                                      
gint
pcv_cgroup_psi_collect(const gchar *cgroup_root)
{
    static const gchar *const RESOURCES[] = { "cpu", "io", "memory" };
                                                           
    static gint64 last_trunc_warn_us = 0;

    const gchar *root = (cgroup_root && cgroup_root[0])
        ? cgroup_root : PCV_CGROUP_PSI_ROOT_DEFAULT;

    PcvCgroupPsiRef refs[PCV_CGROUP_PSI_MAX_ENTRIES];
    gboolean truncated = FALSE;
    gint n = pcv_cgroup_psi_scan(root, refs, PCV_CGROUP_PSI_MAX_ENTRIES, &truncated);

    gint samples = 0;
    for (gint i = 0; i < n; i++) {
        gchar labels[256];
        pcv_cgroup_psi_format_labels(&refs[i], labels, sizeof(labels));

                                                         
                                                     
                                                          
        if (!pcv_prom_labels_are_high_cardinality(labels)) {
            PCV_LOG_WARN(CGROUP_PSI_LOG_DOM,
                "label '%s' is not sweepable by stale-gauge TTL — metrics will leak "
                "after guest '%s' disappears", labels, refs[i].name);
        }

        for (gsize r = 0; r < G_N_ELEMENTS(RESOURCES); r++) {
            PcvCgroupPsiSample s;
            if (!pcv_cgroup_psi_read(root, &refs[i], RESOURCES[r], &s)) continue;
            if (s.have_some)
                _push_sample(RESOURCES[r], "some", labels, s.some_avg10, s.some_total_sec);
            if (s.have_full)
                _push_sample(RESOURCES[r], "full", labels, s.full_avg10, s.full_total_sec);
            samples++;
        }
    }

                                                      
                                                   
    pcv_prom_gauge_set_labels("purecvisor_cgroup_pressure_tracked", "", (gdouble)n);
    pcv_prom_gauge_set_labels("purecvisor_cgroup_pressure_scan_truncated", "",
                              truncated ? 1.0 : 0.0);

    if (truncated) {
        gint64 now = g_get_monotonic_time();
        if (now - last_trunc_warn_us >=
            (gint64)CGROUP_PSI_TRUNC_WARN_INTERVAL_SEC * G_USEC_PER_SEC) {
            last_trunc_warn_us = now;
                                                               
                                                       
                                                                   
            PCV_LOG_WARN(CGROUP_PSI_LOG_DOM,
                "cgroup PSI scan truncated at %d entries — some guests are not "
                "observed (raise PCV_CGROUP_PSI_MAX_ENTRIES or reduce guest count)",
                PCV_CGROUP_PSI_MAX_ENTRIES);
        }
    }

    return samples;
}
