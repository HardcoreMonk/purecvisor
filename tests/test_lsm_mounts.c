                                                                                       
                                                                                 
                                                                         
                                                                                    
                                                               
                                                              
                                                       
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "utils/pcv_lsm_mounts.h"
#include "utils/pcv_bpf.h"                                                      
#include "bpf/pcv_bpf_shared.h"                                                         
#include "modules/security/security_store.h"

                                 
PcvLsmMountTracker *pcv_lsm_mount_tracker_new_for_test(int watch_fd, PcvLsmMountReader reader,
                                                       gpointer user_data,
                                                       const char *const *prefixes, gint64 now_us);
int pcv_bpf__rb_handle(void *ctx, void *data, size_t sz);
void pcv_sec_bounds_reset_for_test(gint64 now);

                                                                 
#define KDEV(ma, mi) ((((guint32)(ma)) << 20) | (guint32)(mi))
#define FO PCV_LSM_HOOK_FILE_OPEN

static const char *const k_prefixes[] = { "/pcvt/etc", "/pcvt/var/state", NULL };

                                                                           
                                                                      
                                                   
#define MI_BASE \
    "22 1 8:1 / / rw,relatime shared:1 - ext4 /dev/sda1 rw\n" \
    "30 22 0:38 / /tmp rw,nosuid,nodev shared:5 - tmpfs tmpfs rw\n" \
    "40 22 259:1 /state /pcvt/var/state rw,relatime shared:9 - ext4 /dev/nvme0n1p1 rw\n" \
    "41 40 0:52 / /pcvt/var/state/lxc/c1 rw shared:10 - zfs rpool/c1 rw\n" \
    "42 22 0:60 / /pcvt/etc\\040x rw shared:11 - tmpfs tmpfs rw\n" \
    "43 22 0:61 / /pcvt/other rw - tmpfs tmpfs rw\n" \
    "44 22 259:1 /data /srv/data rw - ext4 /dev/nvme0n1p1 rw\n" \
    "45 22 4096:5 / /pcvt/etc/bad-major rw - tmpfs tmpfs rw\n" \
    "46 22 1:1048576 / /pcvt/etc/bad-minor rw - tmpfs tmpfs rw\n" \
    "47 22 x:7 / /pcvt/etc/bad-text rw - tmpfs tmpfs rw\n" \
    "48 22 0:9\n" \
    "49 22 0:10 / relative rw - tmpfs tmpfs rw\n" \
    "\n"

                                                                     
typedef struct {
    const char *text;
    guint       calls;
} FakeMountinfo;

static gchar *
_fake_reader(gpointer ud)
{
    FakeMountinfo *f = ud;
    f->calls++;
    return f->text ? g_strdup(f->text) : NULL;
}

                                                                                  
                                                       
static void
test_parse_devnum(void)
{
    PcvLsmMountSet *s = pcv_lsm_mount_set_parse(MI_BASE, k_prefixes);
    g_assert_nonnull(s);
                                                                         
    g_assert_cmpint(pcv_lsm_mount_set_classify(s, FO, 271581185u, "/pcvt/var/state/db"), ==,
                    PCV_LSM_CLASS_SUBMIT);
    g_assert_cmpint(pcv_lsm_mount_set_classify(s, FO, 264241152u, "/pcvt/var/state/db"), ==,
                    PCV_LSM_CLASS_FOREIGN_FS);
    g_assert_cmpint(pcv_lsm_mount_set_classify(s, FO, KDEV(0, 5), "/pcvt/etc/a"), ==,
                    PCV_LSM_CLASS_FOREIGN_FS);
    g_assert_cmpint(pcv_lsm_mount_set_classify(s, FO, KDEV(1, 0), "/pcvt/etc/a"), ==,
                    PCV_LSM_CLASS_FOREIGN_FS);
    g_assert_cmpint(pcv_lsm_mount_set_classify(s, FO, KDEV(0, 7), "/pcvt/etc/a"), ==,
                    PCV_LSM_CLASS_FOREIGN_FS);
    g_assert_cmpint(pcv_lsm_mount_set_classify(s, FO, KDEV(0, 10), "/pcvt/etc/a"), ==,
                    PCV_LSM_CLASS_FOREIGN_FS);
    pcv_lsm_mount_set_free(s);
    g_assert_null(pcv_lsm_mount_set_parse(NULL, k_prefixes));
}

                                               
static void
test_parse_escape(void)
{
    static const char *const pfx[] = { "/pcvt/a b", NULL };
    PcvLsmMountSet *s = pcv_lsm_mount_set_parse(
        "22 1 8:1 / / rw - ext4 /dev/sda1 rw\n"
        "50 22 0:77 / /pcvt/a\\040b rw - tmpfs tmpfs rw\n"
        "51 22 0:78 / /pcvt/a\\134b rw - tmpfs tmpfs rw\n", pfx);
    g_assert_cmpint(pcv_lsm_mount_set_classify(s, FO, KDEV(0, 77), "/pcvt/a b/f"), ==,
                    PCV_LSM_CLASS_SUBMIT);
                                                  
    g_assert_cmpint(pcv_lsm_mount_set_classify(s, FO, KDEV(0, 78), "/pcvt/a b/f"), ==,
                    PCV_LSM_CLASS_FOREIGN_FS);
    pcv_lsm_mount_set_free(s);
}

                                                                         
static void
test_set_nested_bind_and_unrelated(void)
{
    PcvLsmMountSet *s = pcv_lsm_mount_set_parse(MI_BASE, k_prefixes);
                                                                 
    g_assert_cmpint(pcv_lsm_mount_set_classify(s, FO, KDEV(8, 1), "/pcvt/etc/a.conf"), ==, PCV_LSM_CLASS_SUBMIT);
    g_assert_cmpint(pcv_lsm_mount_set_classify(s, FO, KDEV(0, 60), "/pcvt/etc x/y"), ==, PCV_LSM_CLASS_SUBMIT);
    g_assert_cmpint(pcv_lsm_mount_set_classify(s, FO, KDEV(259, 1), "/pcvt/etc/a.conf"), ==,
                    PCV_LSM_CLASS_FOREIGN_FS);                                    
                                                                
    g_assert_cmpint(pcv_lsm_mount_set_classify(s, FO, KDEV(259, 1), "/pcvt/var/state/db"), ==, PCV_LSM_CLASS_SUBMIT);
    g_assert_cmpint(pcv_lsm_mount_set_classify(s, FO, KDEV(0, 52), "/pcvt/var/state/lxc/c1/rootfs/f"), ==,
                    PCV_LSM_CLASS_SUBMIT);
    g_assert_cmpint(pcv_lsm_mount_set_classify(s, FO, KDEV(8, 1), "/pcvt/var/state/db"), ==, PCV_LSM_CLASS_SUBMIT);
                                                              
    g_assert_cmpint(pcv_lsm_mount_set_classify(s, FO, KDEV(0, 38), "/pcvt/etc/pki/node.crt.autogen.tmp.AB12CD"), ==,
                    PCV_LSM_CLASS_FOREIGN_FS);
    g_assert_cmpint(pcv_lsm_mount_set_classify(s, FO, KDEV(0, 38), "/pcvt/var/state/rbac.db"), ==,
                    PCV_LSM_CLASS_FOREIGN_FS);
    g_assert_cmpint(pcv_lsm_mount_set_classify(s, FO, KDEV(0, 61), "/pcvt/var/state/rbac.db"), ==,
                    PCV_LSM_CLASS_FOREIGN_FS);
    pcv_lsm_mount_set_free(s);
}

                                                                                     
                     
static void
test_classify_rules(void)
{
    PcvLsmMountSet *s = pcv_lsm_mount_set_parse(MI_BASE, k_prefixes);
    g_assert_cmpint(pcv_lsm_mount_set_classify(s, FO, KDEV(8, 1), "/pcvt/etc/x"), ==, PCV_LSM_CLASS_SUBMIT);
    g_assert_cmpint(pcv_lsm_mount_set_classify(s, FO, KDEV(0, 38), "/pcvt/etc/x"), ==, PCV_LSM_CLASS_FOREIGN_FS);
    g_assert_cmpint(pcv_lsm_mount_set_classify(s, FO, 0, "/pcvt/etc/x"), ==, PCV_LSM_CLASS_SUBMIT);
    g_assert_cmpint(pcv_lsm_mount_set_classify(s, FO, KDEV(0, 38), "/srv/elsewhere"), ==, PCV_LSM_CLASS_SUBMIT);
    g_assert_cmpint(pcv_lsm_mount_set_classify(s, PCV_LSM_HOOK_BPRM, KDEV(0, 38), "/pcvt/etc/x"), ==,
                    PCV_LSM_CLASS_SUBMIT);
    g_assert_cmpint(pcv_lsm_mount_set_classify(s, FO, KDEV(0, 38), NULL), ==, PCV_LSM_CLASS_SUBMIT);
    g_assert_cmpint(pcv_lsm_mount_set_classify(NULL, FO, KDEV(0, 38), "/pcvt/etc/x"), ==, PCV_LSM_CLASS_SUBMIT);
    pcv_lsm_mount_set_free(s);
}

                                                                   
                                                                                           
static void
test_tracker_pollpri_grow_shrink(void)
{
    int sv[2];
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv), ==, 0);
    FakeMountinfo f = { MI_BASE, 0 };
    const gint64 t0 = g_get_monotonic_time();
    PcvLsmMountTracker *t = pcv_lsm_mount_tracker_new_for_test(sv[1], _fake_reader, &f, k_prefixes, t0);
    g_assert_nonnull(t);
    g_assert_cmpuint(f.calls, ==, 1);                                            

    const guint32 newdev = KDEV(0, 70);
    const char *newpath = "/pcvt/etc/newmnt/f";
    g_assert_cmpint(pcv_lsm_mount_tracker_classify(t, FO, newdev, newpath, t0), ==, PCV_LSM_CLASS_FOREIGN_FS);

                           
    g_assert_cmpint(pcv_lsm_mount_tracker_poll(t, -1, 0), ==, 0);
    g_assert_cmpuint(f.calls, ==, 1);

                              
    f.text = MI_BASE "60 22 0:70 / /pcvt/etc/newmnt rw - tmpfs tmpfs rw\n";
    g_assert_cmpint(send(sv[0], "x", 1, MSG_OOB), ==, 1);
    g_assert_cmpint(pcv_lsm_mount_tracker_poll(t, -1, 1000), ==, 0);
    g_assert_cmpuint(f.calls, ==, 2);
    char b;
    g_assert_cmpint(recv(sv[1], &b, 1, MSG_OOB), ==, 1);
    g_assert_cmpint(pcv_lsm_mount_tracker_classify(t, FO, newdev, newpath, t0), ==, PCV_LSM_CLASS_SUBMIT);

                                                                          
    f.text = MI_BASE;
    g_assert_cmpint(send(sv[0], "x", 1, MSG_OOB), ==, 1);
    g_assert_cmpint(pcv_lsm_mount_tracker_poll(t, -1, 1000), ==, 0);
    g_assert_cmpuint(f.calls, ==, 3);
    g_assert_cmpint(recv(sv[1], &b, 1, MSG_OOB), ==, 1);
    g_assert_cmpint(pcv_lsm_mount_tracker_classify(t, FO, newdev, newpath, t0), ==, PCV_LSM_CLASS_FOREIGN_FS);

                                                               
    int pp[2];
    g_assert_cmpint(pipe(pp), ==, 0);
    g_assert_cmpint(write(pp[1], "r", 1), ==, 1);
    g_assert_cmpint(pcv_lsm_mount_tracker_poll(t, pp[0], 1000), ==, 1);
    g_assert_cmpuint(f.calls, ==, 3);
    close(pp[0]);
    close(pp[1]);

    pcv_lsm_mount_tracker_free(t);                                 
    close(sv[0]);
    close(sv[1]);

                                                                     
                                                  
    const int dead_fd = 1000;
    g_assert_cmpint(fcntl(dead_fd, F_GETFD), ==, -1);
    FakeMountinfo d = { MI_BASE, 0 };
    PcvLsmMountTracker *n = pcv_lsm_mount_tracker_new_for_test(dead_fd, _fake_reader, &d, k_prefixes, t0);
    g_assert_cmpint(pcv_lsm_mount_tracker_poll(n, -1, 0), ==, 0);
    g_assert_cmpint(pcv_lsm_mount_tracker_poll(n, -1, 0), ==, 0);
    g_assert_cmpuint(d.calls, ==, 1);
    pcv_lsm_mount_tracker_free(n);
}

                                                                     
                                                                 
                                    
static void
test_tracker_reread_rate_limit(void)
{
    FakeMountinfo f = { MI_BASE, 0 };
    const gint64 T = 1000 * G_USEC_PER_SEC;
    PcvLsmMountTracker *t = pcv_lsm_mount_tracker_new_for_test(-1, _fake_reader, &f, k_prefixes, T);
    const guint32 dev = KDEV(0, 38);
    const char *p = "/pcvt/etc/late/x";

    g_assert_cmpint(pcv_lsm_mount_tracker_classify(t, FO, dev, p, T + 1), ==, PCV_LSM_CLASS_FOREIGN_FS);
    g_assert_cmpint(pcv_lsm_mount_tracker_classify(t, FO, dev, p, T + PCV_LSM_MOUNT_REREAD_MIN_US - 1), ==,
                    PCV_LSM_CLASS_FOREIGN_FS);
    g_assert_cmpuint(f.calls, ==, 1);
    g_assert_cmpint(pcv_lsm_mount_tracker_classify(t, FO, dev, p, T + PCV_LSM_MOUNT_REREAD_MIN_US), ==,
                    PCV_LSM_CLASS_FOREIGN_FS);
    g_assert_cmpuint(f.calls, ==, 2);
    g_assert_cmpint(pcv_lsm_mount_tracker_classify(t, FO, dev, p, T + PCV_LSM_MOUNT_REREAD_MIN_US + 1), ==,
                    PCV_LSM_CLASS_FOREIGN_FS);
    g_assert_cmpuint(f.calls, ==, 2);

                                                        
    f.text = MI_BASE "61 22 0:38 / /pcvt/etc/late rw - tmpfs tmpfs rw\n";
    g_assert_cmpint(pcv_lsm_mount_tracker_classify(t, FO, dev, p, T + 2 * PCV_LSM_MOUNT_REREAD_MIN_US + 1), ==,
                    PCV_LSM_CLASS_SUBMIT);
    g_assert_cmpuint(f.calls, ==, 3);
    g_assert_cmpint(pcv_lsm_mount_tracker_classify(t, FO, KDEV(8, 1), "/pcvt/etc/a", T + 10 * G_USEC_PER_SEC), ==,
                    PCV_LSM_CLASS_SUBMIT);
    g_assert_cmpuint(f.calls, ==, 3);
    pcv_lsm_mount_tracker_free(t);

                                                   
    FakeMountinfo g = { NULL, 0 };
    PcvLsmMountTracker *u = pcv_lsm_mount_tracker_new_for_test(-1, _fake_reader, &g, k_prefixes, T);
    g_assert_cmpuint(g.calls, ==, 1);
    g_assert_cmpint(pcv_lsm_mount_tracker_classify(u, FO, dev, "/pcvt/etc/x", T + 1), ==, PCV_LSM_CLASS_SUBMIT);
    g_assert_cmpuint(g.calls, ==, 1);
    g.text = MI_BASE;
    g_assert_cmpint(pcv_lsm_mount_tracker_classify(u, FO, dev, "/pcvt/etc/x", T + PCV_LSM_MOUNT_REREAD_MIN_US), ==,
                    PCV_LSM_CLASS_FOREIGN_FS);
    g_assert_cmpuint(g.calls, ==, 2);
    pcv_lsm_mount_tracker_free(u);
    g_assert_cmpint(pcv_lsm_mount_tracker_classify(NULL, FO, dev, "/pcvt/etc/x", T), ==, PCV_LSM_CLASS_SUBMIT);
}

                                
static guint
_lsm_event_count(void)
{
    JsonArray *a = pcv_security_store_list_events(0, 100, NULL, "lsm", NULL);
    guint n = a ? json_array_get_length(a) : 0;
    if (a) json_array_unref(a);
    return n;
}

                                    
static void
_feed(PcvLsmMountTracker *t, guint32 hook, guint32 s_dev, const char *path, size_t sz)
{
    struct pcv_lsm_event ev;
    memset(&ev, 0, sizeof ev);
    ev.ktime_ns = 1;
    ev.pid = 4242;
    ev.cgroup_id = 77;
    ev.hook = hook;
    g_strlcpy(ev.comm, "pcvt-proc", sizeof ev.comm);
    g_strlcpy(ev.path, path, sizeof ev.path);
    ev.s_dev = s_dev;
    g_assert_cmpint(pcv_bpf__rb_handle(t, &ev, sz), ==, 0);
}

                                                                     
                                           
static void
test_record_handler_wiring(void)
{
    gchar *db = g_strdup_printf("%s/pcv-lsm-mounts-%u.db", g_get_tmp_dir(), g_random_int());
    pcv_sec_bounds_reset_for_test(0);
    g_assert_true(pcv_security_store_open(db));

    FakeMountinfo f = { MI_BASE, 0 };
    PcvLsmMountTracker *t = pcv_lsm_mount_tracker_new_for_test(-1, _fake_reader, &f, k_prefixes,
                                                               g_get_monotonic_time());
    const guint64 c0 = pcv_bpf_lsm_foreign_fs_total();
    const guint n0 = _lsm_event_count();
    const size_t full = sizeof(struct pcv_lsm_event);

    _feed(t, FO, KDEV(8, 1), "/pcvt/etc/same.conf", full);                                     
    g_assert_cmpuint(_lsm_event_count(), ==, n0 + 1);
    _feed(t, FO, KDEV(0, 38), "/pcvt/etc/pki/node.crt.autogen.tmp.Q1W2E3", full);                  
    g_assert_cmpuint(_lsm_event_count(), ==, n0 + 1);
    g_assert_cmpuint(pcv_bpf_lsm_foreign_fs_total(), ==, c0 + 1);
    _feed(t, FO, 0, "/pcvt/etc/unknown.conf", full);                                             
    g_assert_cmpuint(_lsm_event_count(), ==, n0 + 2);
    _feed(t, PCV_LSM_HOOK_BPRM, 0, "/pcvt/bin/tool", full);                                    
    g_assert_cmpuint(_lsm_event_count(), ==, n0 + 3);
    _feed(t, FO, KDEV(0, 38), "/pcvt/var/state/truncated.db", full - 4);                          
    g_assert_cmpuint(_lsm_event_count(), ==, n0 + 3);
    g_assert_cmpuint(pcv_bpf_lsm_foreign_fs_total(), ==, c0 + 1);
    _feed(NULL, FO, KDEV(0, 38), "/pcvt/var/state/no-tracker.db", full);                             
    g_assert_cmpuint(_lsm_event_count(), ==, n0 + 4);
    g_assert_cmpuint(pcv_bpf_lsm_foreign_fs_total(), ==, c0 + 1);
    g_assert_cmpuint(f.calls, ==, 1);                                      

    pcv_lsm_mount_tracker_free(t);
    pcv_security_store_close();
    g_unlink(db);
    gchar *wal = g_strconcat(db, "-wal", NULL), *shm = g_strconcat(db, "-shm", NULL);
    g_unlink(wal);
    g_unlink(shm);
    g_free(wal);
    g_free(shm);
    g_free(db);
    pcv_sec_bounds_reset_for_test(0);
}

void test_lsm_mounts_register(void);
void
test_lsm_mounts_register(void)
{
    g_test_add_func("/lsm_mounts/parse_devnum", test_parse_devnum);
    g_test_add_func("/lsm_mounts/parse_escape", test_parse_escape);
    g_test_add_func("/lsm_mounts/set_nested_bind_unrelated", test_set_nested_bind_and_unrelated);
    g_test_add_func("/lsm_mounts/classify_rules", test_classify_rules);
    g_test_add_func("/lsm_mounts/tracker_pollpri_grow_shrink", test_tracker_pollpri_grow_shrink);
    g_test_add_func("/lsm_mounts/tracker_reread_rate_limit", test_tracker_reread_rate_limit);
    g_test_add_func("/lsm_mounts/record_handler_wiring", test_record_handler_wiring);
}
