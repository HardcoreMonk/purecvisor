                                                                                    
                                                                                   
                                                      
                                                                                  
                                                              
  
                                                                                         
                                                   
                                                                           
                                                                                        
                                                                                 
                                                                                                                  
                                                                       
                                                   
  
                                                                 
                                                           
#include <glib.h>
#include <string.h>
#include <json-glib/json-glib.h>
#include "modules/lxc/lxc_mountinfo.h"

                                                           
static gsize
parse_text(const gchar *src, PcvMountinfoLine *out, gsize cap, gboolean *trunc, gchar **keep)
{
    *keep = g_strdup(src);
    return pcv_mountinfo_parse(*keep, out, cap, trunc);
}

static void
test_parse_escapes(void)
{
    PcvMountinfoLine ln[4];
    gboolean trunc = TRUE;
    gchar *buf;
    gsize n = parse_text("36 25 0:62 /a\\040b /mnt/x\\011y rw,relatime shared:1 master:3 - zfs p/ds rw\n", ln, 4, &trunc, &buf);
    g_assert_cmpuint(n, ==, 1);
    g_assert_false(trunc);
    g_assert_cmpuint(ln[0].mnt_id, ==, 36);
    g_assert_cmpuint(ln[0].parent_id, ==, 25);                                              
    g_assert_cmpuint(ln[0].dev_major, ==, 0);
    g_assert_cmpuint(ln[0].dev_minor, ==, 62);
    g_assert_cmpstr(ln[0].root, ==, "/a b");
    g_assert_cmpstr(ln[0].mount_point, ==, "/mnt/x\ty");
    g_assert_cmpstr(ln[0].options, ==, "rw,relatime");
    g_assert_cmpstr(ln[0].optional, ==, "shared:1 master:3");
    g_free(buf);

    n = parse_text("7 1 8:1 /n\\012l /m\\134b rw - ext4 /dev/sda1 rw\n", ln, 4, &trunc, &buf);                             
    g_assert_cmpuint(n, ==, 1);
    g_assert_cmpstr(ln[0].root, ==, "/n\nl");
    g_assert_cmpstr(ln[0].mount_point, ==, "/m\\b");
    g_assert_cmpstr(ln[0].optional, ==, "");
    g_free(buf);
}

static void
test_parse_skips_malformed(void)
{
    PcvMountinfoLine ln[4];
    gboolean trunc;
    gchar *buf;
                                                                          
    gsize n = parse_text("1 2 0:3 / /a rw shared:1\n"
                         "4 5 0:6 / -\n"
                         "8 x9 0:6 / /bad rw - zfs p rw\n"
                         "36 25 0:62 / /ok rw - zfs p rw\n", ln, 4, &trunc, &buf);
    g_assert_cmpuint(n, ==, 1);
    g_assert_cmpuint(ln[0].mnt_id, ==, 36);
    g_assert_cmpstr(ln[0].mount_point, ==, "/ok");
    g_free(buf);
}

static void
test_parse_truncated(void)
{
    PcvMountinfoLine ln[2];
    gboolean trunc = FALSE;
    gchar *buf;
    gsize n = parse_text("1 0 0:1 / /a rw - x s rw\n2 0 0:1 / /b rw - x s rw\n3 0 0:1 / /c rw - x s rw\n", ln, 2, &trunc, &buf);
    g_assert_cmpuint(n, ==, 2);
    g_assert_true(trunc);
    g_assert_cmpstr(ln[1].mount_point, ==, "/b");
    g_free(buf);
}

                                        
static PcvMountinfoLine
mk(guint64 id, const char *mp)
{
    PcvMountinfoLine l = { .mnt_id = id, .root = "/", .mount_point = mp, .options = "rw", .optional = "" };
    return l;
}

static void
test_last_under_boundary(void)
{
    PcvMountinfoLine l[4] = {
        mk(10, "/v/lxc/c/rootfs"),                              
        mk(20, "/v/lxc/c/rootfs2/x"),                               
        mk(40, "/v/lxc/c/rootfs/mnt/a"),
        mk(41, "/v/lxc/c/rootfs/mnt/b"),
    };
    const char *pre = "/v/lxc/c/rootfs";
    g_assert_cmpint(pcv_mountinfo_last_under(l, 4, pre, NULL, 0), ==, 3);
    guint64 s1[] = { 41 };
    g_assert_cmpint(pcv_mountinfo_last_under(l, 4, pre, s1, 1), ==, 2);
    guint64 s2[] = { 40, 41 };
    g_assert_cmpint(pcv_mountinfo_last_under(l, 4, pre, s2, 2), ==, -1);
}

static void
test_last_under_shadowed_order(void)
{
                                                                                 
    PcvMountinfoLine l[2] = { mk(40, "/r/mnt/a/b"), mk(41, "/r/mnt/a") };
    g_assert_cmpint(pcv_mountinfo_last_under(l, 2, "/r", NULL, 0), ==, 1);
    guint64 skip[] = { 41 };
    g_assert_cmpint(pcv_mountinfo_last_under(l, 2, "/r", skip, 1), ==, 0);
    guint64 skip2[] = { 41, 40 };
    g_assert_cmpint(pcv_mountinfo_last_under(l, 2, "/r", skip2, 2), ==, -1);
}

                                 
static PcvMountinfoLine
mkp(guint64 id, guint64 parent, const char *mp)
{
    PcvMountinfoLine l = mk(id, mp);
    l.parent_id = parent;
    return l;
}

                                                                                                           
                                                                                         
static void
test_chain_moved_order(void)
{
    PcvMountinfoLine l[6] = { mkp(40, 41, "/r/m2/x"), mkp(41, 2, "/r/m2"), mkp(42, 40, "/r/m2/x/y"), mkp(43, 2, "/r/q"),
                              mkp(50, 51, "/r/c1"), mkp(51, 50, "/r/c2") };
    const gsize members[6] = { 0, 1, 2, 3, 4, 5 };
    guint depth[6];
    guint64 top[6];
    pcv_mountinfo_chain(l, members, 4, depth, top);
    g_assert_cmpuint(depth[0], ==, 1);
    g_assert_cmpuint(top[0], ==, 41);
    g_assert_cmpuint(depth[1], ==, 0);
    g_assert_cmpuint(top[1], ==, 41);
    g_assert_cmpuint(depth[2], ==, 2);
    g_assert_cmpuint(top[2], ==, 41);
    g_assert_cmpuint(depth[3], ==, 0);
    g_assert_cmpuint(top[3], ==, 43);
    const gsize only_child[1] = { 0 };                                       
    pcv_mountinfo_chain(l, only_child, 1, depth, top);
    g_assert_cmpuint(depth[0], ==, 0);
    g_assert_cmpuint(top[0], ==, 40);
    const gsize ring[2] = { 4, 5 };                                           
    pcv_mountinfo_chain(l, ring, 2, depth, top);
    g_assert_cmpuint(depth[0], ==, 2);
    g_assert_cmpuint(depth[1], ==, 2);
}

                                                                                           
                                                 
static void
test_top_at_moved_order(void)
{
    PcvMountinfoLine l[4] = { mkp(61, 60, "/s/k"), mkp(9, 1, "/s"), mkp(60, 9, "/s/k"), mkp(70, 9, "/s/other") };
    g_assert_cmpint(pcv_mountinfo_top_at(l, 4, "/s//k/"), ==, 0);
    g_assert_cmpint(pcv_mountinfo_top_at(l, 4, "/s/other"), ==, 3);
    g_assert_cmpint(pcv_mountinfo_top_at(l, 4, "/s/none"), ==, -1);
    PcvMountinfoLine one[1] = { mkp(60, 9, "/s/k") };
    g_assert_cmpint(pcv_mountinfo_top_at(one, 1, "/s/k"), ==, 0);
}

                                                                                                             
                                                                                                                      
static void
test_stack_visible_and_chain_has(void)
{
    PcvMountinfoLine l[5] = { mkp(22, 21, "/r/a"), mkp(20, 2, "/r/a"), mkp(21, 20, "/r//a/"), mkp(23, 20, "/r/a/x"), mkp(30, 2, "/r/q") };
    l[1].optional = "master:1";
    l[2].optional = "master:1";
    l[0].optional = "shared:7";
    const gsize mem[5] = { 0, 1, 2, 3, 4 };
    gboolean sh = TRUE;
    g_assert_cmpuint(pcv_mountinfo_stack_visible(l, mem, 5, 1, &sh), ==, 0);
    g_assert_false(sh);                                                                       
    l[1].optional = "master:3 shared:9";                                         
    g_assert_cmpuint(pcv_mountinfo_stack_visible(l, mem, 5, 1, &sh), ==, 0);
    g_assert_true(sh);
    g_assert_cmpuint(pcv_mountinfo_stack_visible(l, mem, 5, 3, &sh), ==, 3);                        
    g_assert_false(sh);
    g_assert_cmpuint(pcv_mountinfo_stack_visible(l, mem, 5, 4, NULL), ==, 4);
    const guint64 done[1] = { 20 };
    g_assert_true(pcv_mountinfo_chain_has(l, mem, 5, 3, done, 1));                  
    g_assert_true(pcv_mountinfo_chain_has(l, mem, 5, 0, done, 1));                     
    g_assert_false(pcv_mountinfo_chain_has(l, mem, 5, 4, done, 1));                 
    g_assert_false(pcv_mountinfo_chain_has(l, mem, 5, 0, done, 0));
}

static void
test_normalize(void)
{
    char out[32];
    g_assert_true(pcv_mountinfo_path_normalize("//a//b/", out, sizeof out));
    g_assert_cmpstr(out, ==, "/a/b");
    g_assert_true(pcv_mountinfo_path_normalize("/", out, sizeof out));
    g_assert_cmpstr(out, ==, "/");
    g_assert_false(pcv_mountinfo_path_normalize("", out, sizeof out));
    g_assert_false(pcv_mountinfo_path_normalize("a", out, sizeof out));
    g_assert_false(pcv_mountinfo_path_normalize("/abcdef", out, 5));                                 
    g_assert_true(pcv_mountinfo_path_normalize("/abcd", out, 6));                      
}

static void
test_parse_dev_and_fmt(void)
{
    guint ma = 99, mi = 99;
    g_assert_true(pcv_mountinfo_parse_dev("259:12", &ma, &mi));
    g_assert_cmpuint(ma, ==, 259);
    g_assert_cmpuint(mi, ==, 12);
    g_assert_false(pcv_mountinfo_parse_dev("259", &ma, &mi));
    g_assert_false(pcv_mountinfo_parse_dev(":1", &ma, &mi));
    g_assert_false(pcv_mountinfo_parse_dev("1:", &ma, &mi));
    g_assert_false(pcv_mountinfo_parse_dev("1:2x", &ma, &mi));
    g_assert_false(pcv_mountinfo_parse_dev("99999999999:1", &ma, &mi));                  
    char b[24];
    g_assert_cmpuint(pcv_mountinfo_fmt_u64(0, b, sizeof b), ==, 1);
    g_assert_cmpstr(b, ==, "0");
    g_assert_cmpuint(pcv_mountinfo_fmt_u64(G_MAXUINT64, b, sizeof b), ==, 20);
    g_assert_cmpstr(b, ==, "18446744073709551615");
    g_assert_cmpuint(pcv_mountinfo_fmt_u64(12345, b, 5), ==, 0);                             
    g_assert_cmpuint(pcv_mountinfo_fmt_u64(12345, b, 6), ==, 5);
}

static void
test_ident_concat(void)
{
    PcvVolIdent id;
    char buf[64];
    g_assert_true(pcv_vol_ident_from_mount(0, 62, "/", "/tank/vol", "/tank/vol", buf, sizeof buf, &id));
    g_assert_cmpstr(id.root, ==, "/");
    g_assert_true(id.root == buf);                                   
    g_assert_true(pcv_vol_ident_from_mount(0, 62, "/", "/tank/vol", "/tank/vol/sub", buf, sizeof buf, &id));
    g_assert_cmpstr(id.root, ==, "/sub");
    g_assert_true(pcv_vol_ident_from_mount(0, 62, "/", "/", "/srv/x", buf, sizeof buf, &id));
    g_assert_cmpstr(id.root, ==, "/srv/x");
    g_assert_true(pcv_vol_ident_from_mount(8, 1, "/base", "/mnt", "/mnt/a", buf, sizeof buf, &id));
    g_assert_cmpstr(id.root, ==, "/base/a");
    g_assert_cmpuint(id.major, ==, 8);
    g_assert_cmpuint(id.minor, ==, 1);
    g_assert_true(pcv_vol_ident_from_mount(0, 62, "//base/", "/mnt//", "/mnt//a//b/", buf, sizeof buf, &id));
    g_assert_cmpstr(id.root, ==, "/base/a/b");                           
    g_assert_false(pcv_vol_ident_from_mount(0, 62, "/", "/tank/vol", "/other", buf, sizeof buf, &id));
    g_assert_false(pcv_vol_ident_from_mount(0, 62, "/", "/tank/vol", "/tank/vol2", buf, sizeof buf, &id));           
    g_assert_false(pcv_vol_ident_from_mount(8, 1, "/base", "/mnt", "/mnt/a", buf, 7, &id));                              
    g_assert_true(pcv_vol_ident_from_mount(8, 1, "/base", "/mnt", "/mnt/a", buf, 8, &id));
    g_assert_cmpuint(pcv_vol_ident_root_cap("/base", "/mnt/a"), ==, 13);
}

                                                                                            
static gchar *
long_path(const gchar *prefix)
{
    GString *s = g_string_new(prefix);
    for (guint i = 0; i < 21; i++) {
        g_string_append_c(s, '/');
        for (guint k = 0; k < 200; k++)
            g_string_append_c(s, 'd');
    }
    return g_string_free(s, FALSE);
}

                                                                                                       
                                                                                                         
static void
test_long_paths(void)
{
    g_autofree gchar *deep = long_path("/vol"), *other = long_path("/other"), *tail = long_path(""), *src = g_strconcat("/tank", tail, NULL);
    g_assert_cmpuint(strlen(deep), ==, 4225);
    PcvVolIdent id = { .major = 0, .minor = 50, .root = "/vol" };
    g_assert_true(pcv_vol_ident_covers(&id, 0, 50, deep));
    g_assert_false(pcv_vol_ident_covers(&id, 0, 50, other));
    const PcvVolIdent *ids[1] = { &id };
    const char *dests[1] = { "mnt/a" };
    g_assert_cmpint(pcv_vol_attribute(ids, dests, 1, 0, 50, deep, "srv/deep"), ==, 0);
    g_assert_cmpint(pcv_vol_attribute(ids, dests, 1, 0, 50, deep, deep), ==, 0);                  
                                        
    PcvVolIdent got;
    gsize cap = pcv_vol_ident_root_cap("/", src);
    g_autofree gchar *buf = g_malloc(cap);
    g_assert_true(pcv_vol_ident_from_mount(0, 50, "/", "/tank", src, buf, cap, &got));
    g_assert_cmpstr(got.root, ==, tail);
    g_assert_true(pcv_vol_ident_covers(&got, 0, 50, tail));
    g_autofree gchar *under = g_strconcat(tail, "/x", NULL);
    g_assert_true(pcv_vol_ident_covers(&got, 0, 50, under));
                      
    g_autofree gchar *slashes = g_strnfill(5000, '/');
    g_autofree gchar *slashy = g_strconcat("/mnt", slashes, "a/", NULL);
    g_assert_true(pcv_mountinfo_path_equal(slashy, "/mnt/a"));
    g_assert_true(pcv_vol_dest_equal(slashy, "mnt/a"));
    g_assert_false(pcv_mountinfo_path_equal(deep, other));
    g_assert_true(pcv_mountinfo_path_equal(deep, deep));
}

static void
test_dest_equal(void)
{
    g_assert_true(pcv_vol_dest_equal("mnt/a", "/mnt/a"));
    g_assert_true(pcv_vol_dest_equal("/mnt//a/", "mnt/a"));
    g_assert_true(pcv_vol_dest_equal("/", ""));
    g_assert_false(pcv_vol_dest_equal("mnt/a", "mnt/ab"));
    g_assert_false(pcv_vol_dest_equal("mnt/a", "mnt"));
    g_assert_false(pcv_vol_dest_equal(NULL, "mnt"));
                                                                                                       
    static const struct { const char *a, *b; gboolean eq; } dot[] = {
        { "mnt/./a", "mnt/a", TRUE }, { "/./mnt/a/.", "mnt/a", TRUE }, { "mnt/a/./b", "/mnt//a/b/", TRUE }, { ".", "", TRUE },
        { "./", "/", TRUE }, { "mnt/.a", "mnt/a", FALSE }, { "mnt/a.", "mnt/a", FALSE }, { "mnt/..", "mnt", FALSE },
        { "mnt/./ab", "mnt/a", FALSE }, { "mnt/.", "mnt/a", FALSE },
    };
    for (guint i = 0; i < G_N_ELEMENTS(dot); i++) {
        g_test_message("dest_equal('%s', '%s')", dot[i].a, dot[i].b);
        g_assert_cmpint(pcv_vol_dest_equal(dot[i].a, dot[i].b), ==, dot[i].eq);
        g_assert_cmpint(pcv_vol_dest_equal(dot[i].b, dot[i].a), ==, dot[i].eq);           
    }
    g_assert_true(pcv_mountinfo_path_equal("/", "//"));
    g_assert_false(pcv_mountinfo_path_equal("a", "a"));                         
}

static void
test_covers_table(void)
{
    PcvVolIdent id = { .major = 0, .minor = 62, .root = "/data" };
    g_assert_true(pcv_vol_ident_covers(&id, 0, 62, "/data"));
    g_assert_true(pcv_vol_ident_covers(&id, 0, 62, "/data/x"));
    g_assert_false(pcv_vol_ident_covers(&id, 0, 62, "/data2"));
    g_assert_false(pcv_vol_ident_covers(&id, 0, 63, "/data"));
    g_assert_true(pcv_vol_ident_covers(&id, 0, 62, "//data//x/"));
    id.root = "/";
    g_assert_true(pcv_vol_ident_covers(&id, 0, 62, "/whatever"));
    g_assert_false(pcv_vol_ident_covers(&id, 1, 62, "/whatever"));
}

static void
test_attribute_longest(void)
{
    PcvVolIdent a = { .major = 0, .minor = 62, .root = "/data" }, b = { .major = 0, .minor = 62, .root = "/data/db" };
    const PcvVolIdent *ids[2] = { &a, &b };
    const char *dests[2] = { "mnt/a", "mnt/b" };
    g_assert_cmpint(pcv_vol_attribute(ids, dests, 2, 0, 62, "/data/db/x", "mnt/b"), ==, 1);
    g_assert_cmpint(pcv_vol_attribute(ids, dests, 2, 0, 62, "/data/x", "mnt/a"), ==, 0);
    g_assert_cmpint(pcv_vol_attribute(ids, dests, 2, 0, 62, "/elsewhere", "mnt/a"), ==, PCV_VOL_ATTR_NONE);

                                       
    const PcvVolIdent *same[2] = { &a, &a };
    const char *d2[2] = { "mnt/a", "mnt/c" };
    g_assert_cmpint(pcv_vol_attribute(same, d2, 2, 0, 62, "/data/x", "mnt/c"), ==, 1);
    g_assert_cmpint(pcv_vol_attribute(same, d2, 2, 0, 62, "/data/x", "mnt/z"), ==, PCV_VOL_ATTR_AMBIGUOUS);
                                                 
    const char *d3[2] = { "mnt/a", "mnt/a" };
    g_assert_cmpint(pcv_vol_attribute(same, d3, 2, 0, 62, "/data/x", "mnt/a"), ==, 0);
                               
    const PcvVolIdent *holes[2] = { NULL, &b };
    g_assert_cmpint(pcv_vol_attribute(holes, dests, 2, 0, 62, "/data/db", "mnt/a"), ==, 1);
}

                          

static void
read_ident(JsonObject *o, PcvVolIdent *id)
{
    id->major = (guint)json_object_get_int_member(o, "major");
    id->minor = (guint)json_object_get_int_member(o, "minor");
    id->root = json_object_get_string_member(o, "root");                        
}

static void
run_ident_case(JsonObject *c)
{
    PcvVolIdent got;
    const gchar *fs_root = json_object_get_string_member(c, "fs_root"), *source = json_object_get_string_member(c, "source");
    gsize cap = pcv_vol_ident_root_cap(fs_root, source);
    g_autofree gchar *buf = g_malloc(cap);
    gboolean ok = pcv_vol_ident_from_mount((guint)json_object_get_int_member(c, "major"), (guint)json_object_get_int_member(c, "minor"),
                                           fs_root, json_object_get_string_member(c, "mount_point"), source, buf, cap, &got);
    if (json_object_get_null_member(c, "expect")) {
        g_assert_false(ok);
        return;
    }
    PcvVolIdent want;
    read_ident(json_object_get_object_member(c, "expect"), &want);
    g_assert_true(ok);
    g_assert_cmpuint(got.major, ==, want.major);
    g_assert_cmpuint(got.minor, ==, want.minor);
    g_assert_cmpstr(got.root, ==, want.root);
}

static void
run_covers_case(JsonObject *c)
{
    PcvVolIdent id;
    read_ident(json_object_get_object_member(c, "id"), &id);
    gboolean got = pcv_vol_ident_covers(&id, (guint)json_object_get_int_member(c, "major"), (guint)json_object_get_int_member(c, "minor"),
                                        json_object_get_string_member(c, "root"));
    g_assert_cmpint(got, ==, json_object_get_boolean_member(c, "expect"));
}

static void
run_attribute_case(JsonObject *c)
{
    JsonArray *lines = json_object_get_array_member(c, "lines");
    guint n = json_array_get_length(lines);
    g_assert_cmpuint(n, <=, 8);
    PcvVolIdent store[8];
    const PcvVolIdent *ids[8];
    const char *dests[8];
    for (guint i = 0; i < n; i++) {
        JsonObject *l = json_array_get_object_element(lines, i);
        if (json_object_get_null_member(l, "ident")) {
            ids[i] = NULL;
        } else {
            read_ident(json_object_get_object_member(l, "ident"), &store[i]);
            ids[i] = &store[i];
        }
        dests[i] = json_object_get_string_member(l, "dest");
    }
    gint got = pcv_vol_attribute(ids, dests, n, (guint)json_object_get_int_member(c, "major"), (guint)json_object_get_int_member(c, "minor"),
                                 json_object_get_string_member(c, "root"), json_object_get_string_member(c, "mount_dest"));
    g_assert_cmpint(got, ==, (gint)json_object_get_int_member(c, "expect"));
}

static void
test_fixture_cases(void)
{
    GError *err = NULL;
    JsonParser *p = json_parser_new();
    g_assert_true(json_parser_load_from_file(p, "tests/fixtures/lxc_volume_ident/cases.json", &err));
    g_assert_no_error(err);
    JsonArray *cases = json_object_get_array_member(json_node_get_object(json_parser_get_root(p)), "cases");
    guint n = json_array_get_length(cases), seen[3] = { 0, 0, 0 };
    g_assert_cmpuint(n, >=, 20);
    for (guint i = 0; i < n; i++) {
        JsonObject *c = json_array_get_object_element(cases, i);
        const gchar *kind = json_object_get_string_member(c, "kind");
        if (g_str_equal(kind, "ident")) {
            seen[0]++;
            run_ident_case(c);
        } else if (g_str_equal(kind, "covers")) {
            seen[1]++;
            run_covers_case(c);
        } else if (g_str_equal(kind, "attribute")) {
            seen[2]++;
            run_attribute_case(c);
        } else {
            g_error("unknown fixture kind: %s", kind);
        }
    }
    g_assert_cmpuint(seen[0], >, 0);
    g_assert_cmpuint(seen[1], >, 0);
    g_assert_cmpuint(seen[2], >, 0);
    g_object_unref(p);
}

                                                                                                          
                                                                                                               
                                                                                           
static void
test_proc_cgroup_v2_under(void)
{
    const struct { const char *text, *cg; gboolean want; const char *why; } rows[] = {
        { "0::/lxc.payload.c1\n", "/lxc.payload.c1", TRUE, "컨테이너 cgroup 자신" },
        { "0::/lxc.payload.c1/init.scope\n", "/lxc.payload.c1", TRUE, "그 아래(systemd init.scope)" },
        { "0::/lxc.payload.c1", "/lxc.payload.c1", TRUE, "끝 줄바꿈 없음" },
        { "12:pids:/lxc.payload.c1\n1:name=systemd:/x\n0::/lxc.payload.c1/a/b\n", "/lxc.payload.c1", TRUE, "v1 줄 뒤의 v2 줄" },
        { "0::/lxc.payload.c10\n", "/lxc.payload.c1", FALSE, "접두만 같은 다른 컨테이너" },
        { "0::/system.slice/purecvisorsd.service\n", "/lxc.payload.c1", FALSE, "호스트 서비스(번호를 다시 받은 프로세스)" },
        { "0::/\n", "/lxc.payload.c1", FALSE, "루트 cgroup" },
        { "0::/lxc.payload.c1 (deleted)\n", "/lxc.payload.c1", FALSE, "지워진 cgroup 표시" },
        { "12:pids:/lxc.payload.c1\n", "/lxc.payload.c1", FALSE, "v2 줄 없음(v1만)" },
        { "", "/lxc.payload.c1", FALSE, "빈 본문" },
        { "0::/lxc.payload.c1\n", "/", FALSE, "cgroup 인자 루트" },
        { "0::/lxc.payload.c1\n", "lxc.payload.c1", FALSE, "cgroup 인자 상대" },
        { "0::/lxc.payload.c1/x\n", "/lxc.payload.c1/", FALSE, "cgroup 인자 끝 '/'" },
        { "0::/lxc.payload.c1\n", "", FALSE, "cgroup 인자 빈 값" },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(rows); i++) {
        g_test_message("row %" G_GSIZE_FORMAT ": %s", i, rows[i].why);
        g_assert_cmpint(pcv_proc_cgroup_v2_under(rows[i].text, rows[i].cg), ==, rows[i].want);
    }
    g_assert_false(pcv_proc_cgroup_v2_under(NULL, "/lxc.payload.c1"));
    g_assert_false(pcv_proc_cgroup_v2_under("0::/lxc.payload.c1\n", NULL));
}

void
test_lxc_mountinfo_register(void)
{
    g_test_add_func("/lxc_mountinfo/parse_escapes", test_parse_escapes);
    g_test_add_func("/lxc_mountinfo/parse_skips_malformed", test_parse_skips_malformed);
    g_test_add_func("/lxc_mountinfo/parse_truncated", test_parse_truncated);
    g_test_add_func("/lxc_mountinfo/last_under_boundary", test_last_under_boundary);
    g_test_add_func("/lxc_mountinfo/last_under_shadowed_order", test_last_under_shadowed_order);
    g_test_add_func("/lxc_mountinfo/chain_moved_order", test_chain_moved_order);
    g_test_add_func("/lxc_mountinfo/top_at_moved_order", test_top_at_moved_order);
    g_test_add_func("/lxc_mountinfo/stack_visible_and_chain_has", test_stack_visible_and_chain_has);
    g_test_add_func("/lxc_mountinfo/normalize", test_normalize);
    g_test_add_func("/lxc_mountinfo/parse_dev_and_fmt", test_parse_dev_and_fmt);
    g_test_add_func("/lxc_mountinfo/ident_concat", test_ident_concat);
    g_test_add_func("/lxc_mountinfo/covers_table", test_covers_table);
    g_test_add_func("/lxc_mountinfo/long_paths", test_long_paths);
    g_test_add_func("/lxc_mountinfo/dest_equal", test_dest_equal);
    g_test_add_func("/lxc_mountinfo/attribute_longest", test_attribute_longest);
    g_test_add_func("/lxc_mountinfo/fixture_cases", test_fixture_cases);
    g_test_add_func("/lxc_mountinfo/proc_cgroup_v2_under", test_proc_cgroup_v2_under);
}
