   
                        
                                               
  
                           
                                                   
                                                    
                                        
  
                                                           
                                                      
                                                     
                                                   
             
  
                                                                       
              
                                                                       
           
                                                         
                                                                               
                                                                     
                                                                
  
                
                                                       
                                                          
                                                                  
                                             
  
                                                                       
         
                                                                       
                                                                 
                                     
                                                    
                                  
                                      
                                                           
  
                     
                                               
                                           
                                         
                                        
  
                                                                       
                          
                                                                       
                                            
                                           
  
           
                        
                                                               
                           
  
           
                                          
                        
  
                   
                                  
                                     
  
                                                                       
           
                                                                       
                                      
                                                              
                                      
                    
  
                                                                       
           
                                                                       
                            
                                                       
                                         
   
                                   
  
                                    
  
                                             
                                                  
   

#include "cpu_allocator.h"
#include "../../utils/pcv_log.h"
#include "../../utils/pcv_config.h"
#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <libvirt/libvirt.h>

                                                          
                                                                       
                                                                       
                                                                   
                                                  
                                                    
                                                         
                                                                 

                                                       

   
                                        
  
                                                          
                                          
  
             
                                               
                                             
                                                               
                                                               
                                                       
                                          
  
              
                                                            
                                              
                                    
                                           
                                    
                                  
  
                         
                                            
                                      
                                               
                             
  
                        
                                                               
                                               
   
typedef struct {
    guint     logical_id;                                                     
    guint     physical_id;                                               
    guint     numa_node;                                  
    gboolean  is_isolated;                                          
    gchar    *owner_vm_id;                                         
} CoreSlot;

   
                                           
  
                                                     
                                    
  
                            
                                                   
                                    
                                                 
                                                     
  
            
                                           
                                                  
                                     
  
                               
                                                              
                                                      
   
struct _CpuAllocator {
    GArray  *cores;                                          
    GMutex   mutex;                           
};

                                                         

   
                         
  
                                
  
                                 
                                               
  
               
                                       
                                                         
                                                
  
                     
                                                 
                                                                 
                                                         
  
                                                         
   
CpuAllocator *cpu_allocator_new(void) {
    CpuAllocator *alloc = g_new0(CpuAllocator, 1);
    alloc->cores = g_array_new(FALSE, TRUE, sizeof(CoreSlot));
    g_mutex_init(&alloc->mutex);
    return alloc;
}

   
                                                           
  
                                         
  
          
                               
                                                         
                                          
                                  
                 
  
                            
                        
                                            
  
                                             
   
void cpu_allocator_free(CpuAllocator *alloc) {
    if (!alloc) return;
    g_mutex_lock(&alloc->mutex);

                               
    for (guint i = 0; i < alloc->cores->len; i++) {
        CoreSlot *slot = &g_array_index(alloc->cores, CoreSlot, i);
        g_free(slot->owner_vm_id);                                         
    }
    g_array_free(alloc->cores, TRUE);                      

    g_mutex_unlock(&alloc->mutex);
    g_mutex_clear(&alloc->mutex);                     
    g_free(alloc);                                
}

                                                       

   
             
                                 
  
                                                    
  
                                          
                                    
   
static gint cmp_guint(gconstpointer a, gconstpointer b) {
    guint x = *(const guint *)a;
    guint y = *(const guint *)b;
    return (x > y) - (x < y);
}

   
                                                         
  
                                                                
                                                           
  
       
                                                       
                                                            
                                                         
                                                 
                                                                    
                                                        
                                      
  
                                           
                                                    
   
GArray *cpu_allocator_parse_cpulist(const gchar *cpulist) {
    GArray *out = g_array_new(FALSE, FALSE, sizeof(guint));
    if (!cpulist) return out;                                        

    gchar *trimmed = g_strdup(cpulist);
    g_strstrip(trimmed);                                             
    if (*trimmed == '\0') { g_free(trimmed); return out; }                 

    gboolean ok = TRUE;
    gchar **tokens = g_strsplit(trimmed, ",", -1);
    for (gchar **t = tokens; *t && ok; t++) {
        gchar *tok = g_strstrip(*t);
        if (*tok == '\0') continue;                              
        gchar *dash = strchr(tok, '-');
        if (dash) {                                               
            *dash = '\0';
            gchar *end_a = NULL, *end_b = NULL;
            guint64 lo = g_ascii_strtoull(tok, &end_a, 10);
            guint64 hi = g_ascii_strtoull(dash + 1, &end_b, 10);
            if (end_a == tok || *end_a != '\0' ||                    
                end_b == dash + 1 || *end_b != '\0' ||                  
                lo > hi) {                                    
                ok = FALSE; break;
            }
            for (guint64 v = lo; v <= hi; v++) {
                guint gv = (guint)v;
                g_array_append_val(out, gv);
            }
        } else {                                                
            gchar *end = NULL;
            guint64 v = g_ascii_strtoull(tok, &end, 10);
            if (end == tok || *end != '\0') { ok = FALSE; break; }
            guint gv = (guint)v;
            g_array_append_val(out, gv);
        }
    }
    g_strfreev(tokens);
    g_free(trimmed);

    if (!ok) { g_array_unref(out); return NULL; }                     

                                                   
    g_array_sort(out, cmp_guint);
    for (guint i = 1; i < out->len; ) {
        if (g_array_index(out, guint, i) == g_array_index(out, guint, i - 1)) {
            g_array_remove_index(out, i);                               
        } else {
            i++;
        }
    }
    return out;
}

   
                                                     
  
                                                          
                                           
  
                                                       
  
                                                                                
                                  
                                                   
   
gboolean cpu_allocator_cpulist_contains(const GArray *list, guint logical_id) {
    if (!list) return FALSE;
    for (guint i = 0; i < list->len; i++) {
        if (g_array_index(list, guint, i) == logical_id) return TRUE;
    }
    return FALSE;
}

                                                       

   
                                            
  
                                                   
                        
  
                                                               
                                                              
  
                           
                                               
                                                  
                                        
  
                           
                                       
                                                       
                                             
  
                                         
                                                                
                                                           
                                         
                                                   
   
void cpu_allocator_add_core(CpuAllocator *alloc,
                             guint logical_id,
                             guint physical_id,
                             guint numa_node,
                             gboolean is_isolated) {
    if (!alloc) return;
    g_mutex_lock(&alloc->mutex);

                                                  
    CoreSlot slot = {
        .logical_id  = logical_id,
        .physical_id = physical_id,
        .numa_node   = numa_node,
        .is_isolated = is_isolated,
        .owner_vm_id = NULL,                           
    };
    g_array_append_val(alloc->cores, slot);                    

    g_mutex_unlock(&alloc->mutex);
    g_message("[cpu_allocator] Registered CPU %u (phys=%u, numa=%u, isolated=%s)",
              logical_id, physical_id, numa_node, is_isolated ? "YES" : "NO");
}

                                                        

   
                             
  
                                                    
                                                       
                                                   
  
                
                                           
                                     
                                                                    
                                                    
                                                            
                            
                                
                                                
  
                             
                                 
                                   
                                  
                             
                                           
  
                          
                                                         
                                          
                                          
  
                    
                                                         
                                                  
                                           
  
                            
                                 
                               
                             
                                                         
                                  
   
gboolean cpu_allocator_allocate_exclusive(CpuAllocator *alloc,
                                          const gchar  *vm_id,
                                          guint         numa_node,
                                          guint         vcpu_count,
                                          GArray      **out_cpus,
                                          gint         *out_numa_node) {
    if (!alloc || !vm_id || vcpu_count == 0) return FALSE;

    g_mutex_lock(&alloc->mutex);

                                                                 
                                                           
                                               
                                                     
                                                            
    {
        GArray *existing = g_array_new(FALSE, FALSE, sizeof(guint));
        gint first_numa = -1; gboolean all_same = TRUE;
        for (guint i = 0; i < alloc->cores->len; i++) {
            CoreSlot *slot = &g_array_index(alloc->cores, CoreSlot, i);
            if (slot->owner_vm_id && g_strcmp0(slot->owner_vm_id, vm_id) == 0) {
                guint lid = slot->logical_id; g_array_append_val(existing, lid);
                if (first_numa < 0) first_numa = (gint)slot->numa_node;
                else if ((gint)slot->numa_node != first_numa) all_same = FALSE;
            }
        }
        if (existing->len > 0) {
            if (out_numa_node) *out_numa_node = all_same ? first_numa : -1;
            *out_cpus = existing;
            g_mutex_unlock(&alloc->mutex);
            g_message("[cpu_allocator] VM '%s' already owns %u core(s) — idempotent no-op", vm_id, existing->len);
            return TRUE;
        }
        g_array_free(existing, TRUE);
    }

      
                                   
      
                  
                                        
                                     
                                                          
       
    GPtrArray *candidates = g_ptr_array_new();
    for (guint i = 0; i < alloc->cores->len && candidates->len < vcpu_count; i++) {
        CoreSlot *slot = &g_array_index(alloc->cores, CoreSlot, i);
                                              
        if (slot->is_isolated && slot->owner_vm_id == NULL && slot->numa_node == numa_node) {
            g_ptr_array_add(candidates, slot);
        }
    }

      
                                                
      
                                
                                              
                                         
                                  
      
                                 
                                            
                                    
                                            
                                    
                                         
                               
       
                                         
                                                       
                                                            
                                                               
                                                         
                                                     
    if (candidates->len < vcpu_count) {
        g_ptr_array_free(candidates, TRUE);                   
        candidates = g_ptr_array_new();

                                             
        for (guint i = 0; i < alloc->cores->len && candidates->len < vcpu_count; i++) {
            CoreSlot *slot = &g_array_index(alloc->cores, CoreSlot, i);
            if (slot->is_isolated && slot->owner_vm_id == NULL && slot->numa_node == numa_node) {
                g_ptr_array_add(candidates, slot);
            }
        }

                                                       
        for (guint i = 0; i < alloc->cores->len && candidates->len < vcpu_count; i++) {
            CoreSlot *slot = &g_array_index(alloc->cores, CoreSlot, i);
            if (slot->is_isolated && slot->owner_vm_id == NULL && slot->numa_node != numa_node) {
                g_ptr_array_add(candidates, slot);
            }
        }
    }

    if (candidates->len < vcpu_count) {
                                             
                                               
                                                       
        g_warning("[cpu_allocator] Not enough isolated cores for VM '%s': "
                  "need %u, available %u", vm_id, vcpu_count, candidates->len);
        g_ptr_array_free(candidates, TRUE);
        g_mutex_unlock(&alloc->mutex);
        return FALSE;
    }

      
                                                        
      
                            
                                           
                            
                                             
       
    GArray *result = g_array_new(FALSE, FALSE, sizeof(guint));
    for (guint i = 0; i < vcpu_count; i++) {
        CoreSlot *slot = g_ptr_array_index(candidates, i);
        slot->owner_vm_id = g_strdup(vm_id);                       
        guint lid = slot->logical_id;
        g_array_append_val(result, lid);                                 
    }

                                                 
                                               
                                                   
    gint actual_numa = -1;
    if (out_numa_node) {
        CoreSlot *first = g_ptr_array_index(candidates, 0);
        gboolean all_same = TRUE;
        for (guint i = 1; i < vcpu_count; i++) {
            CoreSlot *s = g_ptr_array_index(candidates, i);
            if (s->numa_node != first->numa_node) {
                all_same = FALSE;
                break;
            }
        }
        actual_numa = all_same ? (gint)first->numa_node : -1;
        *out_numa_node = actual_numa;
    }

                                                                     
    g_ptr_array_free(candidates, TRUE);
    g_mutex_unlock(&alloc->mutex);

    if (out_numa_node && actual_numa >= 0)
        g_message("[cpu_allocator] Allocated %u core(s) to VM '%s' on NUMA node %d",
                  vcpu_count, vm_id, actual_numa);
    else
        g_message("[cpu_allocator] Allocated %u core(s) to VM '%s' (cross-NUMA)",
                  vcpu_count, vm_id);
    *out_cpus = result;                     
    return TRUE;
}

                                                         

   
                               
  
                                                    
                                                     
  
                                                     
                                
  
             
                                    
                                          
                 
  
                                        
                                                            
                                                       
                                            
                                   
                                                         
                                                         
  
                        
                                              
                                                
  
                          
                                                            
                                                             
                                                                   
                                                       
                                             
                             
                                                
  
                                   
                                      
   
void cpu_allocator_free_vm_cores(CpuAllocator *alloc, const gchar *vm_id) {
    if (!alloc || !vm_id) return;

    g_mutex_lock(&alloc->mutex);

                                            
                                                  
    guint freed = 0;
    for (guint i = 0; i < alloc->cores->len; i++) {
        CoreSlot *slot = &g_array_index(alloc->cores, CoreSlot, i);
        if (slot->owner_vm_id && g_strcmp0(slot->owner_vm_id, vm_id) == 0) {
            g_free(slot->owner_vm_id);                         
            slot->owner_vm_id = NULL;                    
            freed++;
        }
    }

    g_mutex_unlock(&alloc->mutex);

    if (freed > 0)
        g_message("[cpu_allocator] Freed %u core(s) from VM '%s'", freed, vm_id);
    else
                                                                         
                                                                 
                                                               
                                                              
                                                                     
                                                                    
                                                               
                                    
        g_debug("[cpu_allocator] free_vm_cores: no cores found for VM '%s' (idempotent no-op)", vm_id);
}

                                                    

   
                                                  
  
                                                 
                                    
  
                                             
                                     
  
                             
                                      
                              
   
void cpu_allocator_mark_used(CpuAllocator *alloc, guint logical_id, const gchar *vm_name) {
    if (!alloc || !vm_name) return;
    g_mutex_lock(&alloc->mutex);
    for (guint i = 0; i < alloc->cores->len; i++) {
        CoreSlot *slot = &g_array_index(alloc->cores, CoreSlot, i);
        if (slot->logical_id == logical_id) {
            if (slot->owner_vm_id) {
                                                                   
                                                        
                if (g_strcmp0(slot->owner_vm_id, vm_name) != 0) {
                    PCV_LOG_WARN("cpu_allocator",
                        "Core %u already owned by '%s', re-marking for '%s'",
                        logical_id, slot->owner_vm_id, vm_name);
                }
                g_free(slot->owner_vm_id);
            }
            slot->owner_vm_id = g_strdup(vm_name);
            break;
        }
    }
    g_mutex_unlock(&alloc->mutex);
}

                                                         

                                                                                  
#define PCV_RECONCILE_MAX_CPUS 256

   
                                                              
  
                                                     
                                                      
                               
  
                                               
                                                  
                                                                  
                                                              
                                                     
                          
                                                   
                                          
                                                   
  
                       
                             
   
void
cpu_allocator_reconcile(CpuAllocator *alloc, virConnectPtr conn)
{
    if (!alloc || !conn) return;

    virDomainPtr *domains = NULL;
    int n = virConnectListAllDomains(conn, &domains, VIR_CONNECT_LIST_DOMAINS_ACTIVE);
    if (n <= 0 || !domains) return;

    guint total_reconciled = 0;
    for (int i = 0; i < n; i++) {
        const char *name = virDomainGetName(domains[i]);
        int ncpus = virDomainGetVcpusFlags(domains[i], VIR_DOMAIN_AFFECT_LIVE);
        if (ncpus <= 0) { virDomainFree(domains[i]); continue; }

                                                                         
                                                        
                                                                             
                                                                
                                                                           
                                 
        int maplen = VIR_CPU_MAPLEN(PCV_RECONCILE_MAX_CPUS);
        unsigned char *cpumaps = g_new0(unsigned char, (gsize)ncpus * maplen);

        if (virDomainGetVcpuPinInfo(domains[i], ncpus, cpumaps, maplen,
                                    VIR_DOMAIN_AFFECT_LIVE) >= 0) {
            guint vm_cores = 0;
            for (int v = 0; v < ncpus; v++) {
                                                                       
                int pinned_cpu = -1, set_count = 0;
                for (int c = 0; c < maplen * 8; c++) {
                    if (VIR_CPU_USABLE(cpumaps, maplen, v, c)) {
                        pinned_cpu = c;
                        if (++set_count > 1) break;
                    }
                }
                                                                 
                                                                   
                if (set_count == 1) {                        
                    cpu_allocator_mark_used(alloc, (guint)pinned_cpu, name);
                    vm_cores++;
                }
                                                                    
            }
            if (vm_cores > 0) {
                PCV_LOG_INFO("cpu_allocator",
                    "Reconciled %u exclusively-pinned vCPU(s) for VM '%s'", vm_cores, name);
                total_reconciled += vm_cores;
            }
        }

        g_free(cpumaps);
        virDomainFree(domains[i]);
    }
                                                                    
                                                     
    free(domains);

    PCV_LOG_INFO("cpu_allocator",
        "Reconcile complete: %u core(s) recovered from %d running VM(s)",
        total_reconciled, n);
}

                                                        

   
                                          
  
                                                   
                                  
  
                                                          
  
          
                                                
                                                     
                                                     
                                                       
                                                        
                                                
  
          
                                                         
                                                         
  
           
                                            
                                    
  
                                   
   
void cpu_allocator_dump(CpuAllocator *alloc) {
    if (!alloc) return;
    g_mutex_lock(&alloc->mutex);

    g_message("[cpu_allocator] === Core Allocation Map ===");
    for (guint i = 0; i < alloc->cores->len; i++) {
        CoreSlot *slot = &g_array_index(alloc->cores, CoreSlot, i);
        g_message("  CPU %2u (phys=%u, numa=%u, isolated=%s) → %s",
                  slot->logical_id,
                  slot->physical_id,
                  slot->numa_node,
                  slot->is_isolated ? "YES" : "NO ",
                  slot->owner_vm_id ? slot->owner_vm_id : "[free]");
    }
    g_message("[cpu_allocator] ===========================");

    g_mutex_unlock(&alloc->mutex);
}

   
                               
                                   
  
                                                    
                                                       
  
          
      
                        
                      
                                                                             
                                                                            
         
                 
                                                             
                                              
            
        
      
  
                                        
                                                        
   
JsonObject *cpu_allocator_get_numa_info(CpuAllocator *alloc) {
    JsonObject *result = json_object_new();
    JsonArray *cores_arr = json_array_new();

    if (!alloc) {
        json_object_set_int_member(result, "total_cores", 0);
        json_object_set_array_member(result, "numa_nodes", json_array_new());
        json_object_set_array_member(result, "cores", cores_arr);
        return result;
    }

    g_mutex_lock(&alloc->mutex);

                                                      
    GHashTable *numa_stats = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);

                                                                              
                                                                           
                                                                                      
                                                     
                                                        
                                                       
    typedef struct { guint total; guint isolated; guint allocated; guint free_cores; } NumaStat;

    for (guint i = 0; i < alloc->cores->len; i++) {
        CoreSlot *slot = &g_array_index(alloc->cores, CoreSlot, i);

                      
        JsonObject *core_obj = json_object_new();
        json_object_set_int_member(core_obj, "logical_id", slot->logical_id);
        json_object_set_int_member(core_obj, "physical_id", slot->physical_id);
        json_object_set_int_member(core_obj, "numa_node", slot->numa_node);
        json_object_set_boolean_member(core_obj, "isolated", slot->is_isolated);
        if (slot->owner_vm_id)
            json_object_set_string_member(core_obj, "owner", slot->owner_vm_id);
        else
            json_object_set_null_member(core_obj, "owner");
        json_array_add_object_element(cores_arr, core_obj);

                        
        NumaStat *ns = g_hash_table_lookup(numa_stats, GUINT_TO_POINTER(slot->numa_node));
        if (!ns) {
            ns = g_new0(NumaStat, 1);
            g_hash_table_insert(numa_stats, GUINT_TO_POINTER(slot->numa_node), ns);
        }
        ns->total++;
        if (slot->is_isolated) {
            ns->isolated++;
            if (slot->owner_vm_id) ns->allocated++;                            
            else                   ns->free_cores++;                            
        }
    }

    json_object_set_int_member(result, "total_cores", (gint64)alloc->cores->len);
    json_object_set_array_member(result, "cores", cores_arr);

                       
    JsonArray *numa_arr = json_array_new();
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, numa_stats);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        NumaStat *ns = value;
        JsonObject *nobj = json_object_new();
        json_object_set_int_member(nobj, "node", (gint64)GPOINTER_TO_UINT(key));
        json_object_set_int_member(nobj, "total", ns->total);
        json_object_set_int_member(nobj, "isolated", ns->isolated);
        json_object_set_int_member(nobj, "allocated", ns->allocated);
        json_object_set_int_member(nobj, "free", ns->free_cores);                               
        json_array_add_object_element(numa_arr, nobj);
    }
    json_object_set_array_member(result, "numa_nodes", numa_arr);

    g_hash_table_destroy(numa_stats);
    g_mutex_unlock(&alloc->mutex);
    return result;
}
