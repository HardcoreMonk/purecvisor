








                                                                                         
                                                                                    
                                                                       
                                                                         
                                                              
  
                                                                                    
  
                                                              
                                                       
                                                    
                                                       
                                                         
  
                                                                         
                                           
  
                                                        
                                                          
                                                                
                                                  
                                                         
  
                                                                       
                                                                  
                                                      
                                                          
  
                                                
                                                                           
                                                           
                                                                  
                                                      
                                                
                                           
                                                                        
                                      
   

#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>                         
#include "utils/pcv_bpf.h"
#include "modules/security/security_event.h"
#include "modules/security/security_store.h"
#include "test_host_isolation.h"
#ifdef HAVE_LIBBPF
#include <bpf/libbpf.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include "bpf/pcv_bpf_shared.h"
#include "utils/pcv_lsm_mounts.h"

int pcv_bpf__rb_handle(void *ctx, void *data, size_t sz);
PcvLsmMountTracker *pcv_lsm_mount_tracker_new_for_test(int watch_fd, PcvLsmMountReader reader,
                                                       gpointer user_data,
                                                       const char *const *prefixes, gint64 now_us);
#endif

#define PCV_T4_STORE   "/usr/lib/purecvisor/bpf"
#define PCV_T4_PINDIR  "/sys/fs/bpf/purecvisor/pcv_lsm"
                                                              
                                                          
                                                  
                                  
#define PCV_T4_STATE   "/var/lib/purecvisor/bpf_state.json"










static gboolean
_bpf_live_opted_in(void)
{
    if (pcv_test_live_opt_in("PCV_BPF_LIVE"))
        return TRUE;
    g_test_skip("PCV_BPF_LIVE=1 opt-in 필요 — 실 bpffs pin·bpf_state.json·커널 LSM attach를 바꾸므로 "
                "purecvisorsd 정지 후 전용 호스트에서만 실행한다");
    return FALSE;
}

  
                                            
                                                     
                                                
   
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
    if (!_bpf_live_opted_in())
        return;
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
    if (!_bpf_live_opted_in())
        return;
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
    if (!_bpf_live_opted_in())
        return;
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
    if (!_bpf_live_opted_in())
        return;
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

#ifdef HAVE_LIBBPF

#define PCV_T5_BUILD_OBJ   "build/bpf/pcv_lsm.bpf.o"
#define PCV_T5_COVER_DIR   "/etc/purecvisor"
#define PCV_T5_PROBE_PATH  PCV_T5_COVER_DIR "/pcv-t5-sdev-probe"
#define PCV_T5_OVL_NAME    "pcv-t5-overlay-probe"
#define PCV_T5_OVL_PATH    PCV_T5_COVER_DIR "/" PCV_T5_OVL_NAME
#define PCV_T5_OVL_LOWER1  "/tmp/pcv-t5-lower1"
#define PCV_T5_OVL_LOWER2  "/tmp/pcv-t5-lower2"

static G_GNUC_NORETURN void
_t5_child_open_on_private_tmpfs(int wfd, int rfd)
{
    if (unshare(CLONE_NEWNS) != 0) _exit(10);
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) _exit(11);
    if (mount("tmpfs", PCV_T5_COVER_DIR, "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC,
              "mode=0700,size=64k") != 0) _exit(12);
    struct stat st;
    if (stat(PCV_T5_COVER_DIR, &st) != 0) _exit(13);
    guint32 dev = ((guint32)major(st.st_dev) << 20) | (guint32)minor(st.st_dev);
    if (write(wfd, &dev, sizeof dev) != (ssize_t)sizeof dev) _exit(14);
    int fd = open(PCV_T5_PROBE_PATH, O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0) _exit(15);
    close(fd);
    char b;
    while (read(rfd, &b, 1) < 0 && errno == EINTR) { }
    _exit(0);
}

static G_GNUC_NORETURN void
_t5_child_open_through_private_overlay(int wfd, int rfd)
{
    if (unshare(CLONE_NEWNS) != 0) _exit(20);
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) _exit(21);
    if (mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, "mode=0700,size=64k") != 0) _exit(22);
    if (mkdir(PCV_T5_OVL_LOWER1, 0700) != 0 || mkdir(PCV_T5_OVL_LOWER2, 0700) != 0) _exit(23);
    int lf = open(PCV_T5_OVL_LOWER1 "/" PCV_T5_OVL_NAME, O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    if (lf < 0) _exit(24);
    close(lf);
    if (mount("overlay", PCV_T5_COVER_DIR, "overlay", MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC,
              "lowerdir=" PCV_T5_OVL_LOWER1 ":" PCV_T5_OVL_LOWER2) != 0) _exit(25);
    struct stat st;
    if (stat(PCV_T5_COVER_DIR, &st) != 0) _exit(26);
    guint32 dev = ((guint32)major(st.st_dev) << 20) | (guint32)minor(st.st_dev);
    if (write(wfd, &dev, sizeof dev) != (ssize_t)sizeof dev) _exit(27);
    int fd = open(PCV_T5_OVL_PATH, O_RDONLY | O_CLOEXEC);
    if (fd < 0) _exit(28);
    close(fd);
    char b;
    while (read(rfd, &b, 1) < 0 && errno == EINTR) { }
    _exit(0);
}

static gchar *
_t5_text_reader(gpointer ud)
{
    return g_strdup(ud);
}

typedef struct {
    pid_t       want_pid;
    const char *want_path;
    gboolean    seen;
    struct pcv_lsm_event rec;
} PcvT5Capture;

static int
_t5_capture(void *ctx, void *data, size_t sz)
{
    PcvT5Capture *c = ctx;
    if (c->seen || c->want_pid <= 0 || sz < sizeof(struct pcv_lsm_event)) return 0;
    const struct pcv_lsm_event *ev = data;
    if (ev->hook == PCV_LSM_HOOK_FILE_OPEN && (pid_t)ev->pid == c->want_pid &&
        strncmp(ev->path, c->want_path, sizeof ev->path) == 0) {
        memcpy(&c->rec, ev, sizeof c->rec);
        c->seen = TRUE;
    }
    return 0;
}

typedef struct {
    gboolean seen;
    struct pcv_lsm_event rec;
    guint32  want_dev;
    ssize_t  dev_read;
    int      status;
    gchar   *child_mi;
    gchar   *self_mi;
} PcvT5Run;

static void
_t5_run_child(struct ring_buffer *rb, PcvT5Capture *cap, const char *want_path,
              void (*child)(int wfd, int rfd), PcvT5Run *out)
{
    memset(out, 0, sizeof *out);
    memset(cap, 0, sizeof *cap);
    cap->want_path = want_path;
    int p[2], q[2];
    g_assert_cmpint(pipe2(p, O_CLOEXEC), ==, 0);
    g_assert_cmpint(pipe2(q, O_CLOEXEC), ==, 0);
    pid_t pid = fork();
    g_assert_cmpint(pid, >=, 0);
    if (pid == 0) {
        close(p[0]);
        close(q[1]);
        child(p[1], q[0]);
        _exit(9);
    }
    close(p[1]);
    close(q[0]);
    out->dev_read = read(p[0], &out->want_dev, sizeof out->want_dev);
    close(p[0]);

    cap->want_pid = pid;
    for (int i = 0; i < 30 && !cap->seen; i++)
        ring_buffer__poll(rb, 100);
    gchar *mi_path = g_strdup_printf("/proc/%d/mountinfo", (int)pid);
    if (!g_file_get_contents(mi_path, &out->child_mi, NULL, NULL)) out->child_mi = NULL;
    if (!g_file_get_contents("/proc/self/mountinfo", &out->self_mi, NULL, NULL)) out->self_mi = NULL;
    g_free(mi_path);
    close(q[1]);
    g_assert_cmpint(waitpid(pid, &out->status, 0), ==, pid);
    out->seen = cap->seen;
    out->rec = cap->rec;
    cap->want_pid = 0;
}
#endif

static void test_bpf_file_open_roundtrip_root(void) {
#ifndef HAVE_LIBBPF
    g_test_skip("libbpf 미컴파일 — file_open 왕복 불가");
#else
    if (geteuid() != 0) {
        g_test_skip("root 필요 — BPF 로드/attach 는 CAP_BPF/CAP_SYS_ADMIN 요구");
        return;
    }
    if (!_bpf_live_opted_in())
        return;
    if (!pcv_bpf_caps_btf() || !pcv_bpf_caps_lsm()) {
        g_test_skip("BTF 또는 lsm=bpf 부재 — LSM BPF 로드 불가(커널 lsm=...,bpf 재부팅 선행)");
        return;
    }
    if (!g_file_test(PCV_T5_BUILD_OBJ, G_FILE_TEST_IS_REGULAR)) {
        g_test_skip("빌드 트리 BPF 객체(" PCV_T5_BUILD_OBJ ") 없음 — make bpf 선행");
        return;
    }
    if (!g_file_test(PCV_T5_COVER_DIR, G_FILE_TEST_IS_DIR)) {
        g_test_skip("감시 접두사 디렉터리(" PCV_T5_COVER_DIR ") 없음 — 사설 tmpfs 로 덮을 자리가 없다");
        return;
    }

    struct bpf_object *obj = bpf_object__open_file(PCV_T5_BUILD_OBJ, NULL);
    g_assert_nonnull(obj);
    struct bpf_program *bprm = bpf_object__find_program_by_name(obj, "pcv_bprm");
    if (bprm) bpf_program__set_autoload(bprm, false);
    int lret = bpf_object__load(obj);
    if (lret) g_printerr("bpf_object__load: %s\n", g_strerror(-lret));
    g_assert_cmpint(lret, ==, 0);
    struct bpf_program *fo = bpf_object__find_program_by_name(obj, "pcv_file_open");
    g_assert_nonnull(fo);
    struct bpf_link *link = bpf_program__attach(fo);
    g_assert_nonnull(link);

    PcvT5Capture cap = { 0 };
    struct ring_buffer *rb = ring_buffer__new(
        bpf_object__find_map_fd_by_name(obj, "pcv_lsm_events"), _t5_capture, &cap, NULL);
    g_assert_nonnull(rb);

    PcvT5Run tmpfs_run, ovl_run;
    _t5_run_child(rb, &cap, PCV_T5_PROBE_PATH, _t5_child_open_on_private_tmpfs, &tmpfs_run);
    _t5_run_child(rb, &cap, PCV_T5_OVL_PATH, _t5_child_open_through_private_overlay, &ovl_run);

    ring_buffer__free(rb);
    bpf_link__destroy(link);
    bpf_object__close(obj);

    g_assert_true(WIFEXITED(tmpfs_run.status));
    g_assert_cmpint(WEXITSTATUS(tmpfs_run.status), ==, 0);
    g_assert_cmpint(tmpfs_run.dev_read, ==, (ssize_t)sizeof tmpfs_run.want_dev);
    g_assert_cmpuint(tmpfs_run.want_dev, !=, 0);
    g_assert_true(tmpfs_run.seen);
    g_assert_cmpuint(tmpfs_run.rec.s_dev, !=, 0);
    g_assert_cmpuint(tmpfs_run.rec.s_dev, ==, tmpfs_run.want_dev);

    static const char *const t5_prefixes[] = { PCV_T5_COVER_DIR, NULL };
    g_assert_nonnull(tmpfs_run.child_mi);
    g_assert_nonnull(tmpfs_run.self_mi);
    PcvLsmMountSet *cs = pcv_lsm_mount_set_parse(tmpfs_run.child_mi, t5_prefixes);
    PcvLsmMountSet *ps = pcv_lsm_mount_set_parse(tmpfs_run.self_mi, t5_prefixes);
    struct pcv_lsm_event *trec = &tmpfs_run.rec;
    g_assert_cmpint(pcv_lsm_mount_set_classify(cs, trec->hook, trec->s_dev, trec->path), ==,
                    PCV_LSM_CLASS_SUBMIT);
    g_assert_cmpint(pcv_lsm_mount_set_classify(ps, trec->hook, trec->s_dev, trec->path), ==,
                    PCV_LSM_CLASS_FOREIGN_FS);

    PcvLsmMountTracker *mt = pcv_lsm_mount_tracker_new_for_test(-1, _t5_text_reader, tmpfs_run.self_mi, t5_prefixes,
                                                                g_get_monotonic_time());
    const guint64 c0 = pcv_bpf_lsm_foreign_fs_total();
    g_assert_cmpint(pcv_bpf__rb_handle(mt, trec, sizeof *trec), ==, 0);
    g_assert_cmpuint(pcv_bpf_lsm_foreign_fs_total(), ==, c0 + 1);
    pcv_lsm_mount_tracker_free(mt);

    g_assert_true(WIFEXITED(ovl_run.status));
    g_assert_cmpint(WEXITSTATUS(ovl_run.status), ==, 0);
    g_assert_cmpint(ovl_run.dev_read, ==, (ssize_t)sizeof ovl_run.want_dev);
    g_assert_cmpuint(ovl_run.want_dev, !=, 0);
    g_assert_true(ovl_run.seen);
    const struct pcv_lsm_event *orec = &ovl_run.rec;
    g_assert_cmpuint(orec->s_dev, ==, 0);
    g_assert_cmpint(pcv_lsm_mount_set_classify(ps, orec->hook, ovl_run.want_dev, orec->path), ==,
                    PCV_LSM_CLASS_FOREIGN_FS);
    g_assert_cmpint(pcv_lsm_mount_set_classify(ps, orec->hook, orec->s_dev, orec->path), ==,
                    PCV_LSM_CLASS_SUBMIT);

    pcv_lsm_mount_set_free(cs);
    pcv_lsm_mount_set_free(ps);
    g_free(tmpfs_run.child_mi);
    g_free(tmpfs_run.self_mi);
    g_free(ovl_run.child_mi);
    g_free(ovl_run.self_mi);
#endif
}

void test_bpf_integration_register(void) {
    g_test_add_func("/bpf/load_and_pin", test_bpf_load_and_pin_root);
    g_test_add_func("/bpf/lsm_source_string", test_lsm_source_string);
    g_test_add_func("/bpf/ringbuf_roundtrip", test_bpf_ringbuf_roundtrip_root);
    g_test_add_func("/bpf/pin_survives_reload", test_bpf_pin_survives_reload_root);
    g_test_add_func("/bpf/detach_order", test_bpf_detach_order_root);
    g_test_add_func("/bpf/file_open_roundtrip", test_bpf_file_open_roundtrip_root);
}
