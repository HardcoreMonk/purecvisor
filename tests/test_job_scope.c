   
                         
                                                                                      
  
                           
                                                                                                           
                                                                                                          
                                                                              
                                                                                                      
                                                                                                                 
                             
                                                                                 
                                                                                       
  
                                                                             
                               
   
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <sqlite3.h>

#include "modules/auth/pcv_rbac.h"
#include "modules/dispatcher/rpc_completion.h"
#include "modules/dispatcher/vm_delete_status.h"
#include "modules/resource/resource_gate.h"
#include "modules/resource/vm_read_scope.h"
#include "utils/pcv_job_queue.h"
#include "utils/pcv_job_recovery.h"

static gchar *g_dir, *g_db;

static void
_setup(void)
{
    g_dir = g_dir_make_tmp("pcv-jobscope-XXXXXX", NULL);
    g_assert_nonnull(g_dir);
    g_db = g_build_filename(g_dir, "jobs.db", NULL);
    g_setenv("PCV_JOBS_DB_PATH", g_db, TRUE);
    pcv_job_queue_init();
}

static void
_teardown(void)
{
    pcv_job_queue_shutdown();
    g_unsetenv("PCV_JOBS_DB_PATH");
    static const gchar *const SIDE[] = { "jobs.db", "jobs.db-wal", "jobs.db-shm" };
    for (guint i = 0; i < G_N_ELEMENTS(SIDE); i++) {
        gchar *p = g_build_filename(g_dir, SIDE[i], NULL);
        g_unlink(p);
        g_free(p);
    }
    gchar *journal = g_build_filename(g_dir, "jobs.db.journal", NULL);
    g_rmdir(journal);
    g_free(journal);
    g_assert_cmpint(g_rmdir(g_dir), ==, 0);
    g_clear_pointer(&g_dir, g_free);
    g_clear_pointer(&g_db, g_free);
}

static void
_insert(sqlite3 *db, const gchar *id, const gchar *actor, const gchar *actor_tenant)
{
    sqlite3_stmt *st = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(db,
        "INSERT INTO jobs(job_id,type,target,status,progress,created_at,updated_at,generation,method,outcome_known,actor,actor_tenant)"
        " VALUES(?,'vm.delete','web',2,100,100,100,'g','vm.delete',1,?,?)", -1, &st, NULL), ==, SQLITE_OK);
    sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, actor, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, actor_tenant, -1, SQLITE_TRANSIENT);
    g_assert_cmpint(sqlite3_step(st), ==, SQLITE_DONE);
    sqlite3_finalize(st);
}

                                                                   
static PcvVmReadScope *
_scope(gint role, const gchar *subject, const gchar *tenant, PcvJobScope *js)
{
    PcvResCaller c = { .role = role, .subject = (gchar *)subject, .tenant = (gchar *)tenant };
    PcvVmReadScope *s = pcv_vm_read_scope_new(&c);
    pcv_vm_read_job_scope(s, js);
    return s;
}

#define BAD_TENANT "t a"

                                                    
static void
test_where_equals_decide(void)
{
    static const gchar *const ctenants[] = { "ta", NULL, BAD_TENANT };
                                                                                   
    static const struct { const gchar *actor; const gchar *tenant; gboolean self; } owners[] = {
        { NULL, NULL, FALSE }, { "alice", NULL, TRUE }, { "bob", "ta", FALSE },
        { "carol", "tb", FALSE }, { "dave", NULL, FALSE }, { "grpc", "ta", FALSE },
    };
    _setup();
    sqlite3 *db = NULL;
    g_assert_cmpint(sqlite3_open(g_db, &db), ==, SQLITE_OK);
    guint rows = 0;
    for (gint role = PCV_ROLE_VIEWER; role <= PCV_ROLE_ADMIN; role++) {
        for (guint o = 0; o < G_N_ELEMENTS(owners); o++) {
            for (guint t = 0; t < G_N_ELEMENTS(ctenants); t++) {
                const gchar *at = owners[o].self ? ctenants[t] : owners[o].tenant;
                gchar *id = g_strdup_printf("job-%08x", 0x100 + rows);
                _insert(db, id, owners[o].actor, at);
                PcvJobScope js;
                PcvVmReadScope *s = _scope(role, "alice", ctenants[t], &js);
                JsonObject *got = pcv_job_get(id, &js);
                const gboolean want = pcv_vm_read_decide(role, "alice", ctenants[t], owners[o].actor, at);
                if ((got != NULL) != want)
                    g_error("row %u role %d actor %s tenant %s: ledger %d decide %d", rows, role,
                            owners[o].actor ? owners[o].actor : "(null)", ctenants[t] ? ctenants[t] : "(null)", got != NULL, want);
                if (got)
                    json_object_unref(got);
                pcv_vm_read_scope_free(s);
                g_free(id);
                rows++;
            }
        }
    }
    g_assert_cmpuint(rows, ==, 54);
    sqlite3_close(db);
    _teardown();
}

                                                                            
static void
test_admin_actor_tenant_null(void)
{
    _setup();
    JsonParser *p = json_parser_new();
    g_assert_true(json_parser_load_from_data(p, "{\"_pcv_caller_role\":2,\"_pcv_caller_sub\":\"adm\",\"_pcv_caller_tenant\":\"ta\"}",
                                             -1, NULL));
    JsonObject *params = json_node_get_object(json_parser_get_root(p));
    pcv_rpc_completion_begin("vm.delete");
    pcv_rpc_completion_set_actor("adm");
    pcv_rpc_completion_set_actor_tenant(pcv_res_actor_tenant(params, PCV_ROLE_ADMIN));
    PcvJobRef *ref = pcv_job_admit("vm.delete", "web", NULL, "vm.delete");
    (void)pcv_rpc_completion_finish();
    g_assert_nonnull(ref);
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    g_assert_cmpint(sqlite3_open(g_db, &db), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_prepare_v2(db, "SELECT actor, actor_tenant IS NULL FROM jobs WHERE job_id=?", -1, &st, NULL), ==, SQLITE_OK);
    sqlite3_bind_text(st, 1, pcv_job_id(ref), -1, SQLITE_TRANSIENT);
    g_assert_cmpint(sqlite3_step(st), ==, SQLITE_ROW);
    g_assert_cmpstr((const gchar *)sqlite3_column_text(st, 0), ==, "adm");
    g_assert_cmpint(sqlite3_column_int(st, 1), ==, 1);
    sqlite3_finalize(st);
    sqlite3_close(db);

    static const struct { gint role; const gchar *sub; const gchar *tenant; gboolean want; } cases[] = {
        { PCV_ROLE_VIEWER, "zed", "ta", FALSE },                                     
        { PCV_ROLE_OPERATOR, "adm", NULL, TRUE },              
        { PCV_ROLE_ADMIN, "root", NULL, TRUE },
    };
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        PcvJobScope js;
        PcvVmReadScope *s = _scope(cases[i].role, cases[i].sub, cases[i].tenant, &js);
        JsonObject *got = pcv_job_get(pcv_job_id(ref), &js);
        g_assert_cmpint(got != NULL, ==, cases[i].want);
        if (got)
            json_object_unref(got);
        pcv_vm_read_scope_free(s);
    }
    pcv_job_unref(ref);
    g_object_unref(p);
    _teardown();
}

                                                                            
static void
test_not_found_same(void)
{
    _setup();
    sqlite3 *db = NULL;
    g_assert_cmpint(sqlite3_open(g_db, &db), ==, SQLITE_OK);
    _insert(db, "job-0000aa01", "carol", "tb");
    sqlite3_close(db);
    PcvJobScope js;
    PcvVmReadScope *s = _scope(PCV_ROLE_VIEWER, "zed", "ta", &js);
    JsonObject *hidden = NULL, *absent = NULL;
    g_assert_true(pcv_job_find_latest("vm.delete", "web", &js, &hidden));
    g_assert_true(pcv_job_find_latest("vm.delete", "nothing", &js, &absent));
    g_assert_null(hidden);
    g_assert_null(absent);
    JsonNode *a = json_node_new(JSON_NODE_OBJECT), *b = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(a, pcv_vm_delete_status_build("web", hidden));
    json_node_take_object(b, pcv_vm_delete_status_build("web", absent));
    gchar *sa = json_to_string(a, FALSE), *sb = json_to_string(b, FALSE);
    g_assert_cmpstr(sa, ==, sb);
    g_assert_cmpstr(sa, ==, "{\"name\":\"web\",\"status\":\"not_found\"}");
    g_free(sa);
    g_free(sb);
    json_node_unref(a);
    json_node_unref(b);
    pcv_vm_read_scope_free(s);
    _teardown();
}

void test_job_scope_register(void);
void
test_job_scope_register(void)
{
    g_test_add_func("/job_queue/where_equals_decide", test_where_equals_decide);
    g_test_add_func("/job_queue/admin_actor_tenant_null", test_admin_actor_tenant_null);
    g_test_add_func("/vm_delete_status/not_found_same", test_not_found_same);
}
