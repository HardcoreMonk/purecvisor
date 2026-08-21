                                                                                         
                                                                                    
                                                                       
                                                                         
                                                              
  
                                                                                    
  
                                                              
                                                       
                                                    
                                                       
                                                         
  
                                                                         
                                           
  
                                                        
                                                          
                                                                
                                                  
                                                         
  
                                                                       
                                                                  
                                                      
                                                          
  
                                                
                                                                           
                                                           
                                                                  
                                                      
                                                
                                           
                                                                        
                                      
   
#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>                         
#include "utils/pcv_bpf.h"
#include "modules/security/security_event.h"
#include "modules/security/security_store.h"

#define PCV_T4_STORE   "/usr/lib/purecvisor/bpf"
#define PCV_T4_PINDIR  "/sys/fs/bpf/purecvisor/pcv_lsm"
                                                              
                                                          
                                                  
                                  
#define PCV_T4_STATE   "/var/lib/purecvisor/bpf_state.json"

  
                                            
                                                     
                                                
   
static void test_lsm_source_string(void) {
    g_assert_cmpstr(pcv_security_source_to_string(PCV_SECURITY_SOURCE_LSM), ==, "lsm");
    g_assert_cmpstr(pcv_security_type_to_string(PCV_SECURITY_EVENT_FILE_ACCESS_SENSITIVE),
                    ==, "file_access_sensitive");
}

static void test_bpf_load_and_pin_root(void) {
    if (geteuid() != 0) {
        g_test_skip("root 필요 — BPF 로드/핀은 CAP_BPF/CAP_SYS_ADMIN 요구");
        return;
    }
    if (!pcv_bpf_caps_btf() || !pcv_bpf_caps_lsm()) {
        g_test_skip("BTF 또는 lsm=bpf 부재 — LSM BPF 로드 불가(커널 lsm=...,bpf 재부팅 선행)");
        return;
    }

                                                          
    gchar *manifest = g_build_filename(PCV_T4_STORE, "manifest.json", NULL);
    gboolean have_store = g_file_test(manifest, G_FILE_TEST_EXISTS);
    g_free(manifest);
    if (!have_store) {
        g_test_skip("BPF 스토어 미설치 — make bpf-install 선행 필요");
        return;
    }

    GError *err = NULL;
    g_assert_true(pcv_bpf_init(PCV_T4_STORE, &err));
    g_assert_no_error(err);
    if (pcv_bpf_health() != PCV_BPF_OK) {
                                                         
        g_test_skip("eBPF 로드 능력 부재(libbpf 미컴파일 또는 degraded)");
        pcv_bpf_shutdown();
        return;
    }

                                             
                                                                            
    g_assert_true(pcv_bpf_rehydrate(PCV_T4_STORE, &err));
    g_assert_no_error(err);

                                  
    g_assert_true(g_file_test(PCV_T4_PINDIR, G_FILE_TEST_IS_DIR));

    pcv_bpf_shutdown();
}

  
                                         
                                                                     
                                                        
                                               
                                             
  
                                                      
            
                        
                                                          
                                          
                                                    
                                                   
   
static void test_bpf_ringbuf_roundtrip_root(void) {
    if (geteuid() != 0) {
        g_test_skip("root 필요 — BPF 로드/핀/ringbuf 소비는 CAP_BPF/CAP_SYS_ADMIN 요구");
        return;
    }
    if (!pcv_bpf_caps_btf() || !pcv_bpf_caps_lsm()) {
        g_test_skip("BTF 또는 lsm=bpf 부재 — LSM BPF 로드 불가(커널 lsm=...,bpf 재부팅 선행)");
        return;
    }
    gchar *manifest = g_build_filename(PCV_T4_STORE, "manifest.json", NULL);
    gboolean have_store = g_file_test(manifest, G_FILE_TEST_EXISTS);
    g_free(manifest);
    if (!have_store) {
        g_test_skip("BPF 스토어 미설치 — make bpf-install 선행 필요");
        return;
    }

                                                                                  
    gchar *db_path = g_strdup_printf("%s/pcv-bpf-ringbuf-test-%u.db",
                                     g_get_tmp_dir(), g_random_int());
    g_assert_true(pcv_security_store_open(db_path));

    GError *err = NULL;
    g_assert_true(pcv_bpf_init(PCV_T4_STORE, &err));
    g_assert_no_error(err);
    if (pcv_bpf_health() != PCV_BPF_OK) {
        g_test_skip("eBPF 로드 능력 부재(libbpf 미컴파일 또는 degraded)");
        pcv_bpf_shutdown();
        pcv_security_store_close();
        g_unlink(db_path);
        g_free(db_path);
        return;
    }
    g_assert_true(pcv_bpf_rehydrate(PCV_T4_STORE, &err));
    g_assert_no_error(err);

    pcv_bpf_consumer_start();

    const gchar *evil = "/tmp/pcv_test_evil";
    g_assert_true(g_file_set_contents(evil, "#!/bin/sh\nexit 0\n", -1, NULL));
    g_chmod(evil, 0755);
    gchar *argv[] = { (gchar *)evil, NULL };
    g_spawn_sync(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL, NULL, NULL);

                                                 
    JsonArray *events = NULL;
    for (int i = 0; i < 30; i++) {
        g_usleep(100 * 1000);
        events = pcv_security_store_list_events(0, 10, NULL, "lsm", NULL);
        if (events && json_array_get_length(events) >= 1) break;
        json_array_unref(events);
        events = NULL;
    }
    g_assert_nonnull(events);
    g_assert_cmpuint(json_array_get_length(events), >=, 1);
    json_array_unref(events);

    pcv_bpf_consumer_stop();
    pcv_bpf_shutdown();
    pcv_security_store_close();
    g_unlink(evil);
    g_unlink(db_path);
    g_free(db_path);
}

                                                              
                                                                  
static gchar *_first_link_pin_path(const gchar *pindir) {
    GDir *d = g_dir_open(pindir, 0, NULL);
    if (!d) return NULL;
    const char *name;
    gchar *found = NULL;
    while ((name = g_dir_read_name(d))) {
        if (g_str_has_prefix(name, "link_")) {
            found = g_build_filename(pindir, name, NULL);
            break;
        }
    }
    g_dir_close(d);
    return found;
}

  
                              
  
                                                                  
                                                            
                                                                         
                                                               
                                                               
                                                        
                                                   
                                                
   
static void test_bpf_pin_survives_reload_root(void) {
    if (geteuid() != 0) {
        g_test_skip("root 필요 — BPF 로드/핀은 CAP_BPF/CAP_SYS_ADMIN 요구");
        return;
    }
    if (!pcv_bpf_caps_btf() || !pcv_bpf_caps_lsm()) {
        g_test_skip("BTF 또는 lsm=bpf 부재 — LSM BPF 로드 불가(커널 lsm=...,bpf 재부팅 선행)");
        return;
    }
    gchar *manifest = g_build_filename(PCV_T4_STORE, "manifest.json", NULL);
    gboolean have_store = g_file_test(manifest, G_FILE_TEST_EXISTS);
    g_free(manifest);
    if (!have_store) {
        g_test_skip("BPF 스토어 미설치 — make bpf-install 선행 필요");
        return;
    }

    GError *err = NULL;
    g_assert_true(pcv_bpf_init(PCV_T4_STORE, &err));
    g_assert_no_error(err);
    if (pcv_bpf_health() != PCV_BPF_OK) {
        g_test_skip("eBPF 로드 능력 부재(libbpf 미컴파일 또는 degraded)");
        pcv_bpf_shutdown();
        return;
    }

    g_assert_true(pcv_bpf_rehydrate(PCV_T4_STORE, &err));
    g_assert_no_error(err);
    g_assert_true(g_file_test(PCV_T4_PINDIR, G_FILE_TEST_IS_DIR));

    guint links_before = pcv_bpf_count_pinned_links_path(PCV_T4_PINDIR);
    g_assert_cmpuint(links_before, >, 0);

    gchar *link_path = _first_link_pin_path(PCV_T4_PINDIR);
    g_assert_nonnull(link_path);
    GStatBuf st_before;
    g_assert_cmpint(g_stat(link_path, &st_before), ==, 0);

                                                       
                                                       
             
    pcv_bpf_shutdown();
    g_assert_true(g_file_test(PCV_T4_PINDIR, G_FILE_TEST_IS_DIR));
    g_assert_cmpuint(pcv_bpf_count_pinned_links_path(PCV_T4_PINDIR), ==, links_before);
    GStatBuf st_after_shutdown;
    g_assert_cmpint(g_stat(link_path, &st_after_shutdown), ==, 0);
    g_assert_cmpint(st_before.st_ino, ==, st_after_shutdown.st_ino);

                                           
    g_assert_true(pcv_bpf_init(PCV_T4_STORE, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_bpf_rehydrate(PCV_T4_STORE, &err));
    g_assert_no_error(err);

    g_assert_cmpuint(pcv_bpf_count_pinned_links_path(PCV_T4_PINDIR), ==, links_before);
    GStatBuf st_after_reattach;
    g_assert_cmpint(g_stat(link_path, &st_after_reattach), ==, 0);
    g_assert_cmpint(st_before.st_ino, ==, st_after_reattach.st_ino);

    g_free(link_path);
    pcv_bpf_shutdown();
}

  
                                                                
  
                                                            
                                                    
                                                              
                                                
                                            
                                                        
  
                                                              
                                                                
                                                               
                                                                      
                                                      
                                                         
                                              
                                                              
                                              
   
static void test_bpf_detach_order_root(void) {
    if (geteuid() != 0) {
        g_test_skip("root 필요 — BPF 로드/핀은 CAP_BPF/CAP_SYS_ADMIN 요구");
        return;
    }
    if (!pcv_bpf_caps_btf() || !pcv_bpf_caps_lsm()) {
        g_test_skip("BTF 또는 lsm=bpf 부재 — LSM BPF 로드 불가(커널 lsm=...,bpf 재부팅 선행)");
        return;
    }
    gchar *manifest_path = g_build_filename(PCV_T4_STORE, "manifest.json", NULL);
    gboolean have_store = g_file_test(manifest_path, G_FILE_TEST_EXISTS);
    g_free(manifest_path);
    if (!have_store) {
        g_test_skip("BPF 스토어 미설치 — make bpf-install 선행 필요");
        return;
    }

    GError *err = NULL;
    g_assert_true(pcv_bpf_init(PCV_T4_STORE, &err));
    g_assert_no_error(err);
    if (pcv_bpf_health() != PCV_BPF_OK) {
        g_test_skip("eBPF 로드 능력 부재(libbpf 미컴파일 또는 degraded)");
        pcv_bpf_shutdown();
        return;
    }

    g_assert_true(pcv_bpf_rehydrate(PCV_T4_STORE, &err));
    g_assert_no_error(err);
    g_assert_true(g_file_test(PCV_T4_PINDIR, G_FILE_TEST_IS_DIR));

                                                                     
                                                         
    guint links_before = pcv_bpf_count_pinned_links_path(PCV_T4_PINDIR);
    g_assert_cmpuint(links_before, >=, 2);
    if (!g_file_test(PCV_T4_STATE, G_FILE_TEST_EXISTS)) {
        g_test_skip("bpf_state.json 미기록 — 원장 조작 전제 불충족(예상 밖 환경)");
        pcv_bpf_shutdown();
        return;
    }

                                               
    GHashTable *ino_before = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    GDir *d = g_dir_open(PCV_T4_PINDIR, 0, NULL);
    g_assert_nonnull(d);
    const char *name;
    while ((name = g_dir_read_name(d))) {
        if (!g_str_has_prefix(name, "link_")) continue;
        gchar *p = g_build_filename(PCV_T4_PINDIR, name, NULL);
        GStatBuf st;
        g_assert_cmpint(g_stat(p, &st), ==, 0);
        g_hash_table_insert(ino_before, g_strdup(name),
                            g_strdup_printf("%llu", (unsigned long long)st.st_ino));
        g_free(p);
    }
    g_dir_close(d);
    g_assert_cmpuint(g_hash_table_size(ino_before), ==, links_before);

                                                                
                                                                  
                                                              
                                                     
                                                              
                                                          
                                                        
                                                       
                                                      
                                               
                                                       
                                      
    gchar *state_backup_path = g_strdup_printf("%s.pcv-t8-bak", PCV_T4_STATE);
    gchar *state_orig = NULL;
    gsize  state_orig_len = 0;
    g_assert_true(g_file_get_contents(PCV_T4_STATE, &state_orig, &state_orig_len, NULL));
    g_assert_true(g_file_set_contents(state_backup_path, state_orig, state_orig_len, NULL));

                                                                 
    JsonParser *jp = json_parser_new();
    g_assert_true(json_parser_load_from_file(jp, PCV_T4_STATE, NULL));
    JsonNode *root = json_parser_get_root(jp);
    g_assert_true(root && JSON_NODE_HOLDS_ARRAY(root));
    JsonArray *arr = json_node_get_array(root);
    gboolean patched = FALSE;
    for (guint i = 0; i < json_array_get_length(arr); i++) {
        JsonObject *o = json_array_get_object_element(arr, i);
        if (o && g_strcmp0(json_object_get_string_member(o, "name"), "pcv_lsm") == 0) {
            json_object_set_string_member(o, "sha256",
                "0000000000000000000000000000000000000000000000000000000000ff");
            patched = TRUE;
        }
    }
    g_assert_true(patched);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, root);
    gboolean wrote = json_generator_to_file(gen, PCV_T4_STATE, NULL);
    g_object_unref(gen);
    g_assert_true(wrote);
    g_object_unref(jp);

                                                          
                 
    g_assert_true(pcv_bpf_rehydrate(PCV_T4_STORE, &err));
    g_assert_no_error(err);

                                                             
                                                                
                                                       
                                                        
    g_assert_true(g_file_set_contents(PCV_T4_STATE, state_orig, state_orig_len, NULL));
    g_unlink(state_backup_path);
    g_free(state_orig);
    g_free(state_backup_path);

    guint links_after = pcv_bpf_count_pinned_links_path(PCV_T4_PINDIR);
    g_assert_cmpuint(links_after, ==, links_before);

                                                       
                                                    
    guint reverified = 0;
    d = g_dir_open(PCV_T4_PINDIR, 0, NULL);
    g_assert_nonnull(d);
    while ((name = g_dir_read_name(d))) {
        if (!g_str_has_prefix(name, "link_")) continue;
        const char *old_ino = g_hash_table_lookup(ino_before, name);
        g_assert_nonnull(old_ino);
        gchar *p = g_build_filename(PCV_T4_PINDIR, name, NULL);
        GStatBuf st;
        g_assert_cmpint(g_stat(p, &st), ==, 0);
        gchar *new_ino = g_strdup_printf("%llu", (unsigned long long)st.st_ino);
        g_assert_cmpstr(new_ino, !=, old_ino);
        g_free(new_ino);
        g_free(p);
        reverified++;
    }
    g_dir_close(d);
    g_assert_cmpuint(reverified, ==, links_before);
    g_hash_table_unref(ino_before);

    pcv_bpf_shutdown();
}

void test_bpf_integration_register(void) {
    g_test_add_func("/bpf/load_and_pin", test_bpf_load_and_pin_root);
    g_test_add_func("/bpf/lsm_source_string", test_lsm_source_string);
    g_test_add_func("/bpf/ringbuf_roundtrip", test_bpf_ringbuf_roundtrip_root);
    g_test_add_func("/bpf/pin_survives_reload", test_bpf_pin_survives_reload_root);
    g_test_add_func("/bpf/detach_order", test_bpf_detach_order_root);
}
