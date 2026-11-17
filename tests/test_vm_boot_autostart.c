   
                                 
                                                         
  
                                          
  
                           
                                                                     
                                                                                    
                                                                       
                                                            
                                                                
                                                                            
                                                                           
                                                                                    
                                                                                                            
  
                                                              
                                                        
                                                            
   
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "modules/virt/vm_boot_autostart.h"

#define CUR_ID  "6f1e2d3c-4b5a-4968-8776-655443322110"
#define FAKE_ID "00000000-0000-4000-8000-000000000000"                               

                                           
static gchar *mk_tmp(void)
{
    g_autoptr(GError) err = NULL;
    gchar *d = g_dir_make_tmp("pcv-boot-autostart-XXXXXX", &err);
    g_assert_no_error(err);
    return d;
}

                                                        
static void rm_tmp(const gchar *dir)
{
    g_chmod(dir, 0700);
    GDir *d = g_dir_open(dir, 0, NULL);
    if (d) {
        const gchar *n;
        while ((n = g_dir_read_name(d)) != NULL) {
            g_autofree gchar *p = g_build_filename(dir, n, NULL);
            if (g_file_test(p, G_FILE_TEST_IS_DIR))
                g_rmdir(p);
            else
                g_unlink(p);
        }
        g_dir_close(d);
    }
    g_rmdir(dir);
}

                                                                                       
typedef struct {
    gboolean        enabled;
    gboolean        null_id;
    int             marker;
    PcvBootDecision want;
    gboolean        want_write;
} DecideRow;

                                                                        
static void test_decide_table(void)
{
    static const DecideRow rows[] = {
        { TRUE,  FALSE, 0, PCV_BOOT_DECIDE_RECORD_ONLY, TRUE  },                    
        { TRUE,  FALSE, 1, PCV_BOOT_DECIDE_RECORD_ONLY, TRUE  },                       
        { TRUE,  FALSE, 2, PCV_BOOT_DECIDE_SAME_BOOT,   FALSE },              
        { TRUE,  FALSE, 3, PCV_BOOT_DECIDE_ACT,         FALSE },                        
        { FALSE, FALSE, 0, PCV_BOOT_DECIDE_DISABLED,    TRUE  },
        { FALSE, FALSE, 1, PCV_BOOT_DECIDE_DISABLED,    TRUE  },
        { FALSE, FALSE, 2, PCV_BOOT_DECIDE_SAME_BOOT,   FALSE },                        
        { FALSE, FALSE, 3, PCV_BOOT_DECIDE_DISABLED,    TRUE  },
        { TRUE,  TRUE,  3, PCV_BOOT_DECIDE_ERROR,       FALSE },                                        
        { FALSE, TRUE,  2, PCV_BOOT_DECIDE_ERROR,       FALSE },
    };
    for (guint i = 0; i < G_N_ELEMENTS(rows); i++) {
        const DecideRow *r = &rows[i];
        gchar *empty[] = { NULL };
        PcvBootMarker m = { 0 };
        switch (r->marker) {
        case 0: m.state = PCV_BOOT_MARKER_ABSENT; break;
        case 1: m.state = PCV_BOOT_MARKER_CORRUPT; break;
        case 2: m = (PcvBootMarker){ PCV_BOOT_MARKER_OK, (gchar *)CUR_ID, (gchar *)"act", empty }; break;
        default: m = (PcvBootMarker){ PCV_BOOT_MARKER_OK, (gchar *)FAKE_ID, (gchar *)"act", empty }; break;
        }
        gboolean write_now = !r->want_write;
        PcvBootDecision d = pcv_boot_autostart_decide(r->enabled, r->null_id ? NULL : CUR_ID, &m, &write_now);
        if (d != r->want || write_now != r->want_write)
            g_test_message("row %u: decision=%s write=%d", i, pcv_boot_autostart_decision_name(d), write_now);
        g_assert_cmpint(d, ==, r->want);
        g_assert_cmpint(write_now, ==, r->want_write);
    }
                                
    PcvBootMarker absent = { 0 };
    g_assert_cmpint(pcv_boot_autostart_decide(TRUE, CUR_ID, &absent, NULL), ==, PCV_BOOT_DECIDE_RECORD_ONLY);
                               
    g_assert_cmpstr(pcv_boot_autostart_decision_name(PCV_BOOT_DECIDE_RECORD_ONLY), ==, "record_only");
    g_assert_cmpstr(pcv_boot_autostart_decision_name(PCV_BOOT_DECIDE_SAME_BOOT), ==, "same_boot");
    g_assert_cmpstr(pcv_boot_autostart_decision_name(PCV_BOOT_DECIDE_ACT), ==, "act");
    g_assert_cmpstr(pcv_boot_autostart_decision_name(PCV_BOOT_DECIDE_DISABLED), ==, "disabled");
    g_assert_cmpstr(pcv_boot_autostart_decision_name(PCV_BOOT_DECIDE_ERROR), ==, "error");
}

                                                            
static gchar *put(const gchar *dir, const gchar *name, const gchar *text)
{
    gchar *p = g_build_filename(dir, name, NULL);
    g_autoptr(GError) err = NULL;
    g_assert_true(g_file_set_contents(p, text, -1, &err));
    g_assert_no_error(err);
    return p;
}

                                                                    
static void test_boot_id_read(void)
{
    g_autofree gchar *dir = mk_tmp();
    g_autofree gchar *ok = put(dir, "ok", CUR_ID "\n");
    g_autofree gchar *empty = put(dir, "empty", "");
    g_autofree gchar *junk = put(dir, "junk", "not-a-uuid\n");
    g_autofree gchar *two = put(dir, "two", CUR_ID "\n" CUR_ID "\n");
    g_autofree gchar *missing = g_build_filename(dir, "missing", NULL);

    g_autoptr(GError) err = NULL;
    g_autofree gchar *id = NULL;
    g_assert_true(pcv_boot_autostart_read_boot_id_at(ok, &id, &err));
    g_assert_no_error(err);
    g_assert_cmpstr(id, ==, CUR_ID);

    const gchar *bad[] = { empty, junk, two };
    for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
        g_autoptr(GError) e = NULL;
        gchar *out = (gchar *)"sentinel";
        g_assert_false(pcv_boot_autostart_read_boot_id_at(bad[i], &out, &e));
        g_assert_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
        g_assert_null(out);
    }
    g_autoptr(GError) e2 = NULL;
    gchar *out2 = (gchar *)"sentinel";
    g_assert_false(pcv_boot_autostart_read_boot_id_at(missing, &out2, &e2));
    g_assert_nonnull(e2);
    g_assert_null(out2);
    rm_tmp(dir);
}

                                                                               
static void test_marker_roundtrip(void)
{
    g_autofree gchar *dir = mk_tmp();
    g_autofree gchar *path = g_build_filename(dir, "vm_boot_autostart.json", NULL);
    const gchar *cands[] = { "vm-a", "vm-b", NULL };
    g_autoptr(GError) err = NULL;

    PcvBootMarker m = { 0 };
    pcv_boot_autostart_marker_read_at(path, &m);
    g_assert_cmpint(m.state, ==, PCV_BOOT_MARKER_ABSENT);

    g_assert_true(pcv_boot_autostart_marker_write_at(path, CUR_ID, PCV_BOOT_DECIDE_ACT, cands, &err));
    g_assert_no_error(err);
    pcv_boot_autostart_marker_read_at(path, &m);
    g_assert_cmpint(m.state, ==, PCV_BOOT_MARKER_OK);
    g_assert_cmpstr(m.boot_id, ==, CUR_ID);
    g_assert_cmpstr(m.decision, ==, "act");
    g_assert_nonnull(m.candidates);
    g_assert_cmpuint(g_strv_length(m.candidates), ==, 2);
    g_assert_cmpstr(m.candidates[0], ==, "vm-a");
    g_assert_cmpstr(m.candidates[1], ==, "vm-b");
    pcv_boot_autostart_marker_clear(&m);
    g_assert_cmpint(m.state, ==, PCV_BOOT_MARKER_ABSENT);
    g_assert_null(m.boot_id);

    GStatBuf st;
    g_assert_cmpint(g_stat(path, &st), ==, 0);
    g_assert_cmpint(st.st_mode & 0777, ==, 0600);

    g_autoptr(JsonParser) p = json_parser_new();
    g_assert_true(json_parser_load_from_file(p, path, NULL));
    JsonObject *o = json_node_get_object(json_parser_get_root(p));
    g_assert_cmpint(json_object_get_int_member(o, "version"), ==, 1);
    const gchar *at = json_object_get_string_member(o, "decided_at");
    g_assert_nonnull(at);
    g_autoptr(GDateTime) dt = g_date_time_new_from_iso8601(at, NULL);
    g_assert_nonnull(dt);

                                               
    g_assert_true(pcv_boot_autostart_marker_write_at(path, FAKE_ID, PCV_BOOT_DECIDE_RECORD_ONLY, NULL, &err));
    g_assert_no_error(err);
    pcv_boot_autostart_marker_read_at(path, &m);
    g_assert_cmpint(m.state, ==, PCV_BOOT_MARKER_OK);
    g_assert_cmpstr(m.boot_id, ==, FAKE_ID);
    g_assert_cmpstr(m.decision, ==, "record_only");
    g_assert_nonnull(m.candidates);
    g_assert_cmpuint(g_strv_length(m.candidates), ==, 0);
    pcv_boot_autostart_marker_clear(&m);
    g_assert_cmpint(g_stat(path, &st), ==, 0);
    g_assert_cmpint(st.st_mode & 0777, ==, 0600);

                                   
    g_autoptr(GError) e2 = NULL;
    g_assert_false(pcv_boot_autostart_marker_write_at(path, "nope", PCV_BOOT_DECIDE_ACT, NULL, &e2));
    g_assert_error(e2, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    pcv_boot_autostart_marker_clear(NULL);
    rm_tmp(dir);
}

                                                           
static void test_marker_corrupt(void)
{
    g_autofree gchar *dir = mk_tmp();
    static const gchar *const bad[] = {
        "{",
        "",
        "[]",
        "{\"version\":2,\"boot_id\":\"" CUR_ID "\",\"decision\":\"act\",\"candidates\":[]}",
        "{\"version\":\"1\",\"boot_id\":\"" CUR_ID "\",\"decision\":\"act\",\"candidates\":[]}",
        "{\"boot_id\":\"" CUR_ID "\",\"decision\":\"act\",\"candidates\":[]}",
        "{\"version\":1,\"boot_id\":\"not-a-uuid\",\"decision\":\"act\",\"candidates\":[]}",
        "{\"version\":1,\"decision\":\"act\",\"candidates\":[]}",
        "{\"version\":1,\"boot_id\":\"" CUR_ID "\",\"decision\":7,\"candidates\":[]}",
        "{\"version\":1,\"boot_id\":\"" CUR_ID "\",\"decision\":\"act\",\"candidates\":\"vm-a\"}",
        "{\"version\":1,\"boot_id\":\"" CUR_ID "\",\"decision\":\"act\",\"candidates\":[1]}",
        "{\"version\":1,\"boot_id\":\"" CUR_ID "\",\"decision\":\"act\"}",
    };
    for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
        g_autofree gchar *name = g_strdup_printf("m%u.json", i);
        g_autofree gchar *path = put(dir, name, bad[i]);
        PcvBootMarker m = { 0 };
        pcv_boot_autostart_marker_read_at(path, &m);
        if (m.state != PCV_BOOT_MARKER_CORRUPT)
            g_test_message("case %u not corrupt: %s", i, bad[i]);
        g_assert_cmpint(m.state, ==, PCV_BOOT_MARKER_CORRUPT);
        g_assert_null(m.boot_id);
        g_assert_null(m.candidates);
        pcv_boot_autostart_marker_clear(&m);
    }
                                                  
    g_autofree gchar *sub = g_build_filename(dir, "subdir", NULL);
    g_assert_cmpint(g_mkdir(sub, 0700), ==, 0);
    PcvBootMarker m = { 0 };
    pcv_boot_autostart_marker_read_at(sub, &m);
    g_assert_cmpint(m.state, ==, PCV_BOOT_MARKER_CORRUPT);
                         
    g_autofree gchar *good = put(dir, "good.json",
        "{\"version\":1,\"boot_id\":\"" CUR_ID "\",\"decision\":\"same_boot\",\"decided_at\":\"x\",\"candidates\":[\"a\"]}");
    pcv_boot_autostart_marker_read_at(good, &m);
    g_assert_cmpint(m.state, ==, PCV_BOOT_MARKER_OK);
    pcv_boot_autostart_marker_clear(&m);
    rm_tmp(dir);
}

                                                                                 
static void test_marker_write_fail(void)
{
    g_autofree gchar *dir = mk_tmp();
    g_autofree gchar *path = g_build_filename(dir, "vm_boot_autostart.json", NULL);
    g_autoptr(GError) err = NULL;

                                 
    g_assert_true(pcv_boot_autostart_marker_write_at(path, CUR_ID, PCV_BOOT_DECIDE_ACT, NULL, &err));
    g_assert_no_error(err);
    g_assert_cmpint(g_unlink(path), ==, 0);

                                                      
    if (geteuid() != 0) {
        g_assert_cmpint(g_chmod(dir, 0500), ==, 0);
        g_autoptr(GError) e = NULL;
        g_assert_false(pcv_boot_autostart_marker_write_at(path, CUR_ID, PCV_BOOT_DECIDE_ACT, NULL, &e));
        g_assert_nonnull(e);
        g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
        g_assert_cmpint(g_chmod(dir, 0700), ==, 0);
    } else {
        g_test_message("root: 0500 디렉터리 경우는 건너뛴다(부모가 일반 파일인 경우로 대신 확인)");
    }

                                            
    g_autofree gchar *plain = put(dir, "plain", "x");
    g_autofree gchar *under = g_build_filename(plain, "vm_boot_autostart.json", NULL);
    g_autoptr(GError) e3 = NULL;
    g_assert_false(pcv_boot_autostart_marker_write_at(under, CUR_ID, PCV_BOOT_DECIDE_ACT, NULL, &e3));
    g_assert_nonnull(e3);
    g_assert_false(g_file_test(under, G_FILE_TEST_EXISTS));
    rm_tmp(dir);
}

                                                                    

                                                                                 
static guint g_phys_calls = 0;                                                 
static gboolean fake_phys(const gchar *br, gchar **mode, GError **error)
{
    g_phys_calls++;
    *mode = NULL;
    if (g_strcmp0(br, "br0.100") == 0 || g_strcmp0(br, "bad name") == 0) {                               
        *mode = g_strdup("dedicated");
        return TRUE;
    }
    if (g_strcmp0(br, "br-phys0") == 0) { *mode = g_strdup("dedicated"); return TRUE; }
    if (g_strcmp0(br, "br-shared0") == 0) { *mode = g_strdup("shared"); return TRUE; }
    if (g_strcmp0(br, "br-broken") == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "broken desired state");
        return FALSE;
    }
    return TRUE;
}

                                                 
static gboolean fake_vpc(const gchar *br)
{
    return g_strcmp0(br, "pcvs0123456789a") == 0;
}

                                                                          
static gboolean fake_exists(const gchar *br)
{
    return g_strcmp0(br, "pcvlegacy9") != 0;
}

                                                                        
static gboolean fake_exists_no_nat(const gchar *br)
{
    return fake_exists(br) && g_strcmp0(br, "pcvnat0") != 0;
}

static const PcvBootBridgeOracle ORACLE = { "pcvnat0", fake_phys, fake_vpc, fake_exists };
static const PcvBootBridgeOracle ORACLE_NO_NAT = { "pcvnat0", fake_phys, fake_vpc, fake_exists_no_nat };

                                                                     
static void test_bridge_in_set(void)
{
    static const gchar *const not_s[] = { "virbr0", "br0", "br-int", "pcvx-unregistered", "pcvnat1", "br-broken" };
    for (guint i = 0; i < G_N_ELEMENTS(not_s); i++) {
        if (pcv_boot_autostart_bridge_in_set(&ORACLE, not_s[i]))
            g_test_message("in S unexpectedly: %s", not_s[i]);
        g_assert_false(pcv_boot_autostart_bridge_in_set(&ORACLE, not_s[i]));
    }
    static const gchar *const in_s[] = { "pcvnat0", "br-phys0", "br-shared0", "pcvs0123456789a" };
    for (guint i = 0; i < G_N_ELEMENTS(in_s); i++)
        g_assert_true(pcv_boot_autostart_bridge_in_set(&ORACLE, in_s[i]));
                                
    const PcvBootBridgeOracle other = { "pcvnat9", fake_phys, fake_vpc, fake_exists };
    g_assert_false(pcv_boot_autostart_bridge_in_set(&other, "pcvnat0"));
    g_assert_true(pcv_boot_autostart_bridge_in_set(&other, "pcvnat9"));
    g_assert_false(pcv_boot_autostart_bridge_in_set(&ORACLE, NULL));
    g_assert_false(pcv_boot_autostart_bridge_in_set(&ORACLE, ""));
    g_assert_false(pcv_boot_autostart_bridge_in_set(NULL, "pcvnat0"));
                                                                                           
    static const gchar *const odd[] = { "br0.100", "bad name" };
    for (guint i = 0; i < G_N_ELEMENTS(odd); i++) {
        guint before = g_phys_calls;
        g_assert_false(pcv_boot_autostart_bridge_in_set(&ORACLE, odd[i]));
        g_assert_cmpuint(g_phys_calls, ==, before);
    }
}

                                                               
                                                                          
static void test_iface_exists_at(void)
{
    g_autofree gchar *root = mk_tmp();
    static const gchar *const dirs[] = { "pcvnat0", "br0.100", "abcdefghijklmno", "abcdefghijklmnop", "a" };
    for (guint i = 0; i < G_N_ELEMENTS(dirs); i++) {
        g_autofree gchar *p = g_build_filename(root, dirs[i], NULL);
        g_assert_cmpint(g_mkdir(p, 0700), ==, 0);
    }
    g_autofree gchar *ab = g_build_filename(root, "a", "b", NULL);
    g_assert_cmpint(g_mkdir(ab, 0700), ==, 0);
    g_autofree gchar *plain = put(root, "plainfile", "x");
    g_assert_true(pcv_boot_autostart_iface_exists_at(root, "pcvnat0"));
    g_assert_true(pcv_boot_autostart_iface_exists_at(root, "br0.100"));                          
    g_assert_true(pcv_boot_autostart_iface_exists_at(root, "abcdefghijklmno"));                        
    static const gchar *const no[] = { "", ".", "..", "a/b", "abcdefghijklmnop", "missing0", "plainfile" };
    for (guint i = 0; i < G_N_ELEMENTS(no); i++) {
        if (pcv_boot_autostart_iface_exists_at(root, no[i]))
            g_test_message("exists unexpectedly: '%s'", no[i]);
        g_assert_false(pcv_boot_autostart_iface_exists_at(root, no[i]));
    }
    g_assert_false(pcv_boot_autostart_iface_exists_at(root, NULL));
    g_assert_false(pcv_boot_autostart_iface_exists_at(NULL, "pcvnat0"));
    g_rmdir(ab);
    rm_tmp(root);
}

#define DOMX(ifaces) "<domain type='kvm'><name>t</name><devices>" ifaces "</devices></domain>"
#define BR(n)        "<interface type='bridge'><source bridge='" n "'/><model type='virtio'/></interface>"
#define OVS(n)       "<interface type='bridge'><source bridge='" n "'/><virtualport type='openvswitch'>" \
                     "<parameters interfaceid='5f2b8c1e-0000-4000-8000-000000000001'/></virtualport></interface>"
#define NETW         "<interface type='network'><source network='default'/></interface>"
#define DIRECT       "<interface type='direct'><source dev='eno1' mode='bridge'/></interface>"
#define ETHER        "<interface type='ethernet'><target dev='tap0'/></interface>"
#define VHOST        "<interface type='vhostuser'><source type='unix' path='/run/x.sock' mode='server'/></interface>"

                                         
typedef struct {
    const gchar                *xml;
    const PcvBootBridgeOracle *o;
    PcvBootClass                want;
    const gchar                *detail;                      
} ClassRow;

                                                                                     
static void test_classify_table(void)
{
                                                                                         
    const ClassRow rows[] = {
        { DOMX(BR("pcvnat0")),                    &ORACLE,        PCV_BOOT_CLASS_CANDIDATE,      NULL },
        { DOMX(BR("virbr0")),                     &ORACLE,        PCV_BOOT_CLASS_NOT_PCV,        "virbr0" },
        { DOMX(BR("pcvnat0") BR("virbr0")),       &ORACLE,        PCV_BOOT_CLASS_CANDIDATE,      NULL },
        { DOMX(BR("pcvnat0") BR("pcvlegacy9")),   &ORACLE,        PCV_BOOT_CLASS_BRIDGE_MISSING, "pcvlegacy9" },
        { DOMX(BR("pcvlegacy9")),                 &ORACLE,        PCV_BOOT_CLASS_NOT_PCV,        "pcvlegacy9" },
        { DOMX(BR("pcvs0123456789a")),            &ORACLE,        PCV_BOOT_CLASS_CANDIDATE,      NULL },
        { DOMX(BR("br-phys0")),                   &ORACLE,        PCV_BOOT_CLASS_CANDIDATE,      NULL },
        { DOMX(BR("br-shared0")),                 &ORACLE,        PCV_BOOT_CLASS_CANDIDATE,      NULL },
        { DOMX(BR("br0")),                        &ORACLE,        PCV_BOOT_CLASS_NOT_PCV,        "br0" },
        { DOMX(OVS("br-int")),                    &ORACLE,        PCV_BOOT_CLASS_NOT_PCV,        "br-int" },
        { DOMX(NETW),                             &ORACLE,        PCV_BOOT_CLASS_NOT_PCV,        "no bridge" },
        { DOMX(DIRECT),                           &ORACLE,        PCV_BOOT_CLASS_NOT_PCV,        "no bridge" },
        { DOMX(ETHER),                            &ORACLE,        PCV_BOOT_CLASS_NOT_PCV,        "no bridge" },
        { DOMX(VHOST),                            &ORACLE,        PCV_BOOT_CLASS_NOT_PCV,        "no bridge" },
        { DOMX(BR("pcvnat0") NETW DIRECT ETHER VHOST), &ORACLE,   PCV_BOOT_CLASS_CANDIDATE,      NULL },
        { DOMX(""),                               &ORACLE,        PCV_BOOT_CLASS_NOT_PCV,        "no bridge" },
        { "<domain><devices>",                    &ORACLE,        PCV_BOOT_CLASS_XML_ERROR,      NULL },
        { "",                                     &ORACLE,        PCV_BOOT_CLASS_XML_ERROR,      NULL },
                                                           
        { DOMX(BR("pcvnat0")),                    &ORACLE_NO_NAT, PCV_BOOT_CLASS_BRIDGE_MISSING, "pcvnat0" },
        { DOMX(BR("pcvs0123456789a")),            &ORACLE_NO_NAT, PCV_BOOT_CLASS_CANDIDATE,      NULL },
    };
    for (guint i = 0; i < G_N_ELEMENTS(rows); i++) {
        g_autofree gchar *detail = NULL;
        PcvBootClass k = pcv_boot_autostart_classify(VIR_DOMAIN_SHUTOFF, VIR_DOMAIN_SHUTOFF_FAILED, rows[i].xml,
                                                     rows[i].o, &detail);
        if (k != rows[i].want)
            g_test_message("row %u: %s (%s)", i, pcv_boot_autostart_class_name(k), detail ? detail : "-");
        g_assert_cmpint(k, ==, rows[i].want);
        if (rows[i].detail)
            g_assert_cmpstr(detail, ==, rows[i].detail);
    }
    g_assert_cmpint(pcv_boot_autostart_classify(VIR_DOMAIN_SHUTOFF, VIR_DOMAIN_SHUTOFF_FAILED, NULL, &ORACLE, NULL), ==,
                    PCV_BOOT_CLASS_XML_ERROR);

                                                              
    static const int reasons[] = {
        VIR_DOMAIN_SHUTOFF_UNKNOWN, VIR_DOMAIN_SHUTOFF_SHUTDOWN, VIR_DOMAIN_SHUTOFF_DESTROYED,
        VIR_DOMAIN_SHUTOFF_CRASHED, VIR_DOMAIN_SHUTOFF_MIGRATED, VIR_DOMAIN_SHUTOFF_SAVED,
        VIR_DOMAIN_SHUTOFF_FAILED, VIR_DOMAIN_SHUTOFF_FROM_SNAPSHOT, VIR_DOMAIN_SHUTOFF_DAEMON,
    };
    static const int states[] = { VIR_DOMAIN_SHUTOFF, VIR_DOMAIN_RUNNING, VIR_DOMAIN_PAUSED };
    for (guint s = 0; s < G_N_ELEMENTS(states); s++) {
        for (guint r = 0; r < G_N_ELEMENTS(reasons); r++) {
            gboolean allowed = reasons[r] == VIR_DOMAIN_SHUTOFF_FAILED;                                   
            g_assert_cmpint(pcv_boot_autostart_reason_allowed(reasons[r]), ==, allowed);
            PcvBootClass want = states[s] != VIR_DOMAIN_SHUTOFF ? PCV_BOOT_CLASS_ALREADY_ACTIVE
                              : allowed ? PCV_BOOT_CLASS_CANDIDATE : PCV_BOOT_CLASS_STATE_REASON;
            PcvBootClass k = pcv_boot_autostart_classify(states[s], reasons[r], DOMX(BR("pcvnat0")), &ORACLE, NULL);
            if (k != want)
                g_test_message("state %d reason %d: %s", states[s], reasons[r], pcv_boot_autostart_class_name(k));
            g_assert_cmpint(k, ==, want);
            PcvBootClass kb = pcv_boot_autostart_classify(states[s], reasons[r], "<broken", &ORACLE, NULL);
            g_assert_cmpint(kb, ==, want == PCV_BOOT_CLASS_CANDIDATE ? PCV_BOOT_CLASS_XML_ERROR : want);
        }
    }
                      
    g_assert_cmpstr(pcv_boot_autostart_class_name(PCV_BOOT_CLASS_CANDIDATE), ==, "candidate");
    g_assert_cmpstr(pcv_boot_autostart_class_name(PCV_BOOT_CLASS_NOT_PCV), ==, "not_pcv");
    g_assert_cmpstr(pcv_boot_autostart_class_name(PCV_BOOT_CLASS_BRIDGE_MISSING), ==, "skipped_bridge_missing");
    g_assert_cmpstr(pcv_boot_autostart_class_name(PCV_BOOT_CLASS_STATE_REASON), ==, "skipped_state_reason");
    g_assert_cmpstr(pcv_boot_autostart_class_name(PCV_BOOT_CLASS_ALREADY_ACTIVE), ==, "already_active");
    g_assert_cmpstr(pcv_boot_autostart_class_name(PCV_BOOT_CLASS_XML_ERROR), ==, "xml_error");
}

                                                   
static gchar *test_dom_xml(const gchar *name, const gchar *ifaces)
{
    return g_strdup_printf("<domain type='test'><name>%s</name><memory>65536</memory><vcpu>1</vcpu>"
                           "<os><type>hvm</type></os><devices>%s</devices></domain>", name, ifaces);
}

                                                
static gboolean has_name(GPtrArray *a, const gchar *name)
{
    for (guint i = 0; a && i < a->len; i++)
        if (g_strcmp0(g_ptr_array_index(a, i), name) == 0)
            return TRUE;
    return FALSE;
}

                                                       
static guint count_prefixed(GPtrArray *a)
{
    guint n = 0;
    for (guint i = 0; a && i < a->len; i++)
        if (g_str_has_prefix(g_ptr_array_index(a, i), "pcvboot-"))
            n++;
    return n;
}

                                                                                 
                                                                                
                                                                   
static void test_scan_test_driver(void)
{
    virConnectPtr conn = virConnectOpen("test:///default");
    g_assert_nonnull(conn);

    PcvBootScanCounts base = { 0 };
    g_autoptr(GError) err = NULL;
    GPtrArray *before = pcv_boot_autostart_scan(conn, &ORACLE, &base, &err);
    g_assert_no_error(err);
    g_assert_nonnull(before);
    g_assert_cmpuint(count_prefixed(before), ==, 0);
    g_ptr_array_unref(before);

    static const struct { const gchar *name; const gchar *ifaces; gboolean autostart; } defs[] = {
        { "pcvboot-nat-auto",     BR("pcvnat0"),                  TRUE  },
        { "pcvboot-virbr-auto",   BR("virbr0"),                   TRUE  },
        { "pcvboot-nat-noauto",   BR("pcvnat0"),                  FALSE },
        { "pcvboot-missing-auto", BR("pcvnat0") BR("pcvlegacy9"), TRUE  },
    };
    virDomainPtr doms[G_N_ELEMENTS(defs)] = { 0 };
    for (guint i = 0; i < G_N_ELEMENTS(defs); i++) {
        g_autofree gchar *xml = test_dom_xml(defs[i].name, defs[i].ifaces);
        doms[i] = virDomainDefineXML(conn, xml);
        g_assert_nonnull(doms[i]);
        if (defs[i].autostart)
            g_assert_cmpint(virDomainSetAutostart(doms[i], 1), ==, 0);
    }

                                                                                                 
                                                                                         
                                                                                                  
                                                         
    PcvBootScanCounts c = { 0 };
    GPtrArray *cands = pcv_boot_autostart_scan(conn, &ORACLE, &c, &err);
    g_assert_no_error(err);
    g_assert_nonnull(cands);
    g_assert_false(has_name(cands, "pcvboot-nat-auto"));
    g_assert_cmpuint(count_prefixed(cands), ==, 0);
    g_assert_cmpuint(c.state_reason - base.state_reason, ==, 3);
    g_assert_cmpuint(c.not_pcv - base.not_pcv, ==, 0);
    g_assert_cmpuint(c.bridge_missing - base.bridge_missing, ==, 0);
    g_assert_cmpuint(c.query_error - base.query_error, ==, 0);
    g_ptr_array_unref(cands);

    for (guint i = 0; i < G_N_ELEMENTS(defs); i++) {
        g_assert_cmpint(virDomainUndefine(doms[i]), ==, 0);
        virDomainFree(doms[i]);
    }
                               
    g_autoptr(GError) e2 = NULL;
    g_assert_null(pcv_boot_autostart_scan(NULL, &ORACLE, NULL, &e2));
    g_assert_nonnull(e2);
    virConnectClose(conn);
}

                                                                

                                                           
static void test_recheck_table(void)
{
    g_assert_cmpint(pcv_boot_autostart_recheck(VIR_DOMAIN_RUNNING, 1), ==, PCV_BOOT_CLASS_ALREADY_ACTIVE);
    g_assert_cmpint(pcv_boot_autostart_recheck(VIR_DOMAIN_PAUSED, 0), ==, PCV_BOOT_CLASS_ALREADY_ACTIVE);
    g_assert_cmpint(pcv_boot_autostart_recheck(VIR_DOMAIN_SHUTOFF, VIR_DOMAIN_SHUTOFF_SHUTDOWN), ==,
                    PCV_BOOT_CLASS_STATE_REASON);
    g_assert_cmpint(pcv_boot_autostart_recheck(VIR_DOMAIN_SHUTOFF, VIR_DOMAIN_SHUTOFF_DESTROYED), ==,
                    PCV_BOOT_CLASS_STATE_REASON);
    g_assert_cmpint(pcv_boot_autostart_recheck(VIR_DOMAIN_SHUTOFF, VIR_DOMAIN_SHUTOFF_UNKNOWN), ==,
                    PCV_BOOT_CLASS_STATE_REASON);                                                  
    g_assert_cmpint(pcv_boot_autostart_recheck(VIR_DOMAIN_SHUTOFF, VIR_DOMAIN_SHUTOFF_FAILED), ==,
                    PCV_BOOT_CLASS_CANDIDATE);
}

                                              
                                                              
typedef struct {
    GHashTable *recheck;                                                 
    GHashTable *submit_as;                                                            
    GPtrArray  *submitted;                   
    GPtrArray  *reported;                      
    GHashTable *counts;                        
    guint       finished;
    PcvBootRunTally tally;
    PcvBootRun *run;                                       
    guint       in_flight;                                      
    guint       max_in_flight;
} QFix;

static gboolean g_q_terminating = FALSE;

                              
static gboolean q_recheck(const gchar *vm, int *state_out, int *reason_out, gpointer ud)
{
    QFix *f = ud;
    const gchar *v = g_hash_table_lookup(f->recheck, vm);
    if (!v)
        return FALSE;
    g_assert_cmpint(sscanf(v, "%d:%d", state_out, reason_out), ==, 2);
    return TRUE;
}

                                    
static PcvBootSubmit q_submit(const gchar *vm, PcvBootRun *run, gchar **reason_out, gpointer ud)
{
    QFix *f = ud;
    g_ptr_array_add(f->submitted, g_strdup(vm));
    f->run = run;
    gpointer p = g_hash_table_lookup(f->submit_as, vm);
    PcvBootSubmit r = p ? (PcvBootSubmit)(GPOINTER_TO_INT(p) - 1) : PCV_BOOT_SUBMIT_ASYNC;
    if (r == PCV_BOOT_SUBMIT_ASYNC) {
        f->in_flight++;
        f->max_in_flight = MAX(f->max_in_flight, f->in_flight);
    } else if (r == PCV_BOOT_SUBMIT_REJECTED) {
        *reason_out = g_strdup("precheck failed");
    } else if (r == PCV_BOOT_SUBMIT_LOCK_BUSY) {
        *reason_out = g_strdup("VM 'x' is already locked");
    }
    return r;
}

                                  
static void q_report(const gchar *vm, const gchar *reason, gpointer ud)
{
    QFix *f = ud;
    g_ptr_array_add(f->reported, g_strdup_printf("%s|%s", vm, reason));
}

                                  
static gboolean q_terminating(void)
{
    return g_q_terminating;
}

                        
static void q_count(const gchar *result, gpointer ud)
{
    QFix *f = ud;
    guint n = GPOINTER_TO_UINT(g_hash_table_lookup(f->counts, result));
    g_hash_table_insert(f->counts, g_strdup(result), GUINT_TO_POINTER(n + 1));
}

                        
static void q_finished(const PcvBootRunTally *t, gpointer ud)
{
    QFix *f = ud;
    f->finished++;
    f->tally = *t;
    f->run = NULL;                         
}

static void qfix_init(QFix *f)
{
    memset(f, 0, sizeof *f);
    f->recheck = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);
    f->submit_as = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);
    f->submitted = g_ptr_array_new_with_free_func(g_free);
    f->reported = g_ptr_array_new_with_free_func(g_free);
    f->counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_q_terminating = FALSE;
}

static void qfix_clear(QFix *f)
{
    g_hash_table_unref(f->recheck);
    g_hash_table_unref(f->submit_as);
    g_ptr_array_unref(f->submitted);
    g_ptr_array_unref(f->reported);
    g_hash_table_unref(f->counts);
    g_q_terminating = FALSE;
}

static guint qcount(QFix *f, const gchar *result)
{
    return GPOINTER_TO_UINT(g_hash_table_lookup(f->counts, result));
}

static PcvBootRunOps qops(QFix *f)
{
    return (PcvBootRunOps){ q_recheck, q_submit, q_report, q_terminating, q_count, q_finished, f };
}

                                           
static void q_done(QFix *f, gboolean ok)
{
    g_assert_nonnull(f->run);
    g_assert_cmpuint(f->in_flight, ==, 1);
    f->in_flight--;
    pcv_boot_autostart_item_done(f->run, ok);
}

                                                    
static void test_queue_order(void)
{
    QFix f;
    qfix_init(&f);
    const gchar *vms[] = { "a", "b", "c" };
    PcvBootRunOps ops = qops(&f);
    pcv_boot_autostart_run_queue(vms, 3, &ops);
    g_assert_cmpuint(f.submitted->len, ==, 1);
    q_done(&f, TRUE);
    g_assert_cmpuint(f.submitted->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(f.submitted, 1), ==, "b");
    q_done(&f, TRUE);
    g_assert_cmpuint(f.submitted->len, ==, 3);
    g_assert_cmpuint(f.finished, ==, 0);
    q_done(&f, FALSE);
    g_assert_cmpuint(f.finished, ==, 1);
    g_assert_cmpuint(f.max_in_flight, ==, 1);
    g_assert_cmpuint(f.tally.started, ==, 2);
    g_assert_cmpuint(f.tally.failed, ==, 1);
    g_assert_cmpuint(qcount(&f, "started"), ==, 2);
    g_assert_cmpuint(qcount(&f, "failed"), ==, 1);
    g_assert_cmpuint(f.reported->len, ==, 0);
    qfix_clear(&f);
}

                                                                      
static void test_queue_skip_paths(void)
{
    QFix f;
    qfix_init(&f);
    g_hash_table_insert(f.recheck, (gpointer)"run", (gpointer)"1:1");                         
    g_hash_table_insert(f.recheck, (gpointer)"down", (gpointer)"5:1");                                 
    g_hash_table_insert(f.recheck, (gpointer)"failed", (gpointer)"5:6");                                  
    const gchar *vms[] = { "run", "down", "gone", "failed" };                                           
    PcvBootRunOps ops = qops(&f);
    pcv_boot_autostart_run_queue(vms, 4, &ops);
    g_assert_cmpuint(f.submitted->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(f.submitted, 0), ==, "gone");
    q_done(&f, TRUE);
    g_assert_cmpuint(f.submitted->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(f.submitted, 1), ==, "failed");
    q_done(&f, TRUE);
    g_assert_cmpuint(f.finished, ==, 1);
    g_assert_cmpuint(f.tally.already_active, ==, 1);
    g_assert_cmpuint(f.tally.skipped_state_reason, ==, 1);
    g_assert_cmpuint(f.tally.started, ==, 2);
    g_assert_cmpuint(qcount(&f, "already_active"), ==, 1);
    g_assert_cmpuint(qcount(&f, "skipped_state_reason"), ==, 1);
    g_assert_cmpuint(f.reported->len, ==, 0);
    qfix_clear(&f);
}

                                                                     
static void test_queue_lock_busy_no_report(void)
{
    QFix f;
    qfix_init(&f);
    g_hash_table_insert(f.submit_as, (gpointer)"busy", GINT_TO_POINTER(PCV_BOOT_SUBMIT_LOCK_BUSY + 1));
    const gchar *vms[] = { "busy", "next" };
    PcvBootRunOps ops = qops(&f);
    pcv_boot_autostart_run_queue(vms, 2, &ops);
    g_assert_cmpuint(f.submitted->len, ==, 2);                               
    g_assert_cmpstr(g_ptr_array_index(f.submitted, 1), ==, "next");
    q_done(&f, TRUE);
    g_assert_cmpuint(f.finished, ==, 1);
    g_assert_cmpuint(f.tally.lock_busy, ==, 1);
    g_assert_cmpuint(f.tally.rejected, ==, 0);
    g_assert_cmpuint(qcount(&f, "lock_busy"), ==, 1);
    g_assert_cmpuint(qcount(&f, "rejected"), ==, 0);
    g_assert_cmpuint(f.reported->len, ==, 0);
    qfix_clear(&f);
}

                                                                         
static void test_queue_terminating(void)
{
    QFix f;
    qfix_init(&f);
    const gchar *vms[] = { "a", "b", "c" };
    PcvBootRunOps ops = qops(&f);
    pcv_boot_autostart_run_queue(vms, 3, &ops);
    g_assert_cmpuint(f.submitted->len, ==, 1);
    g_q_terminating = TRUE;
    q_done(&f, TRUE);
    g_assert_cmpuint(f.submitted->len, ==, 1);
    g_assert_cmpuint(f.reported->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(f.reported, 0), ==, "b|daemon shutting down");
    g_assert_cmpstr(g_ptr_array_index(f.reported, 1), ==, "c|daemon shutting down");
    g_assert_cmpuint(f.finished, ==, 1);
    g_assert_cmpuint(f.tally.started, ==, 1);
    g_assert_cmpuint(f.tally.rejected, ==, 2);
    g_assert_cmpuint(qcount(&f, "rejected"), ==, 2);
    qfix_clear(&f);
}

                                                               
static void test_queue_summary_once(void)
{
    QFix f;
    qfix_init(&f);
    g_hash_table_insert(f.submit_as, (gpointer)"x", GINT_TO_POINTER(PCV_BOOT_SUBMIT_ALREADY_ACTIVE + 1));
    g_hash_table_insert(f.submit_as, (gpointer)"y", GINT_TO_POINTER(PCV_BOOT_SUBMIT_ALREADY_ACTIVE + 1));
    const gchar *all_sync[] = { "x", "y" };
    PcvBootRunOps ops = qops(&f);
    pcv_boot_autostart_run_queue(all_sync, 2, &ops);
    g_assert_cmpuint(f.finished, ==, 1);
    g_assert_cmpuint(f.tally.already_active, ==, 2);
    g_assert_cmpuint(f.reported->len, ==, 0);
    qfix_clear(&f);

                                                
    qfix_init(&f);
    g_hash_table_insert(f.submit_as, (gpointer)"rej", GINT_TO_POINTER(PCV_BOOT_SUBMIT_REJECTED + 1));
    g_hash_table_insert(f.submit_as, (gpointer)"busy", GINT_TO_POINTER(PCV_BOOT_SUBMIT_LOCK_BUSY + 1));
    const gchar *mixed[] = { "a", "rej", "busy", "z" };
    ops = qops(&f);
    pcv_boot_autostart_run_queue(mixed, 4, &ops);
    g_assert_cmpuint(f.finished, ==, 0);
    q_done(&f, FALSE);                                                                     
    g_assert_cmpuint(f.finished, ==, 0);
    g_assert_cmpuint(f.submitted->len, ==, 4);
    q_done(&f, TRUE);
    g_assert_cmpuint(f.finished, ==, 1);
    g_assert_cmpuint(f.tally.started, ==, 1);
    g_assert_cmpuint(f.tally.failed, ==, 1);
    g_assert_cmpuint(f.tally.rejected, ==, 1);
    g_assert_cmpuint(f.tally.lock_busy, ==, 1);
    g_assert_cmpuint(f.reported->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(f.reported, 0), ==, "rej|precheck failed");
    qfix_clear(&f);

                              
    qfix_init(&f);
    g_hash_table_insert(f.submit_as, (gpointer)"rej", GINT_TO_POINTER(PCV_BOOT_SUBMIT_REJECTED + 1));
    const gchar *tail_rej[] = { "a", "rej" };
    ops = qops(&f);
    pcv_boot_autostart_run_queue(tail_rej, 2, &ops);
    q_done(&f, TRUE);
    g_assert_cmpuint(f.finished, ==, 1);
    g_assert_cmpuint(f.tally.rejected, ==, 1);
    qfix_clear(&f);
}

                                                       
static void test_queue_empty(void)
{
    QFix f;
    qfix_init(&f);
    PcvBootRunOps ops = qops(&f);
    pcv_boot_autostart_run_queue(NULL, 0, &ops);
    g_assert_cmpuint(f.finished, ==, 1);
    g_assert_cmpuint(f.tally.started + f.tally.failed + f.tally.rejected + f.tally.already_active +
                     f.tally.lock_busy + f.tally.skipped_state_reason, ==, 0);
    g_assert_cmpuint(f.submitted->len, ==, 0);
    g_assert_cmpuint(g_hash_table_size(f.counts), ==, 0);
    qfix_clear(&f);
}

                                                                                                           
                                                                                                
                                                              
                                                                        
static void test_reason_unknown_after_libvirtd_restart_skipped(void)
{
    g_autofree gchar *detail = NULL;
    g_assert_cmpint(pcv_boot_autostart_classify(VIR_DOMAIN_SHUTOFF, VIR_DOMAIN_SHUTOFF_UNKNOWN, DOMX(BR("pcvnat0")), &ORACLE,
                                                &detail), ==, PCV_BOOT_CLASS_STATE_REASON);
    g_assert_cmpstr(detail, ==, "reason=0");
    g_assert_cmpint(pcv_boot_autostart_classify(VIR_DOMAIN_SHUTOFF, VIR_DOMAIN_SHUTOFF_FAILED, DOMX(BR("pcvnat0")), &ORACLE,
                                                NULL), ==, PCV_BOOT_CLASS_CANDIDATE);
    g_assert_false(pcv_boot_autostart_reason_allowed(VIR_DOMAIN_SHUTOFF_UNKNOWN));
    g_assert_true(pcv_boot_autostart_reason_allowed(VIR_DOMAIN_SHUTOFF_FAILED));
    g_assert_cmpint(pcv_boot_autostart_recheck(VIR_DOMAIN_SHUTOFF, VIR_DOMAIN_SHUTOFF_UNKNOWN), ==, PCV_BOOT_CLASS_STATE_REASON);

                                                                                                   
    QFix f;
    qfix_init(&f);
    g_hash_table_insert(f.recheck, (gpointer)"reset", (gpointer)"5:0");                                 
    g_hash_table_insert(f.recheck, (gpointer)"failed", (gpointer)"5:6");                            
    const gchar *vms[] = { "reset", "failed" };
    PcvBootRunOps ops = qops(&f);
    pcv_boot_autostart_run_queue(vms, 2, &ops);
    g_assert_cmpuint(f.submitted->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(f.submitted, 0), ==, "failed");
    q_done(&f, TRUE);
    g_assert_cmpuint(f.finished, ==, 1);
    g_assert_cmpuint(f.tally.skipped_state_reason, ==, 1);
    g_assert_cmpuint(f.tally.started, ==, 1);
    g_assert_cmpuint(qcount(&f, "skipped_state_reason"), ==, 1);
    g_assert_cmpuint(f.reported->len, ==, 0);
    qfix_clear(&f);
}

void test_vm_boot_autostart_register(void)
{
    g_test_add_func("/vm_boot_autostart/decide_table", test_decide_table);
    g_test_add_func("/vm_boot_autostart/boot_id_read", test_boot_id_read);
    g_test_add_func("/vm_boot_autostart/marker_roundtrip", test_marker_roundtrip);
    g_test_add_func("/vm_boot_autostart/marker_corrupt", test_marker_corrupt);
    g_test_add_func("/vm_boot_autostart/marker_write_fail", test_marker_write_fail);
    g_test_add_func("/vm_boot_autostart/bridge_in_set", test_bridge_in_set);
    g_test_add_func("/vm_boot_autostart/iface_exists_at", test_iface_exists_at);
    g_test_add_func("/vm_boot_autostart/classify_table", test_classify_table);
    g_test_add_func("/vm_boot_autostart/scan_test_driver", test_scan_test_driver);
    g_test_add_func("/vm_boot_autostart/recheck_table", test_recheck_table);
    g_test_add_func("/vm_boot_autostart/queue_order", test_queue_order);
    g_test_add_func("/vm_boot_autostart/queue_skip_paths", test_queue_skip_paths);
    g_test_add_func("/vm_boot_autostart/queue_lock_busy_no_report", test_queue_lock_busy_no_report);
    g_test_add_func("/vm_boot_autostart/queue_terminating", test_queue_terminating);
    g_test_add_func("/vm_boot_autostart/queue_summary_once", test_queue_summary_once);
    g_test_add_func("/vm_boot_autostart/queue_empty", test_queue_empty);
    g_test_add_func("/vm_boot_autostart/reason/unknown_after_libvirtd_restart_skipped",
                    test_reason_unknown_after_libvirtd_restart_skipped);
}
