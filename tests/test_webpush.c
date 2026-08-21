                                                                                       
                                                                                                              
                                                                         
                                                                
                                         
                       
  
                                                         
  
                                              
                                                
                                           
                                              
            
  
         
                                                                 
                                                                  
                                                                     
                                                                
                                                                                 
                                                                           
                                                                   
                                                                 
                                                     
                                                                    
                                                                  
                                                                               
                                                                               
                                                                        
                                                                             
                                                                           
                                                                       
  
                                                    
                                                  
                                                             
                                                            
                                                              
                                                    
   
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <sqlite3.h>
#include <string.h>

#include "modules/daemons/pcv_webpush.h"
#include "modules/auth/pcv_rbac.h"                                      
#include "modules/daemons/alert_engine.h"                            
#include "modules/dispatcher/handler_auth.h"                               

                                                                       
                                                                             
                                                         
                                                              
                  
void     pcv_dispatcher_init_policy_map(void);
gboolean pcv_dispatcher_check_rbac(const gchar *method, gint caller_role);

                                                                 
#define T_P256DH "BCVxsr7N_eNgVRqvHtD0zTZsEc6-VV-JvLexhqUzORcxaOzi6-AYWXvTBHm4bjyPjs7Vd8pZGH6SRpkNtoIAiw4"
#define T_AUTH   "BTBZMqHH6r4Tts7J_aSIgg"

                                                         
#define T_EP_A   "https://203.0.113.10/push/aaa"
#define T_EP_B   "https://203.0.113.11/push/bbb"
#define T_EP_C   "https://203.0.113.12/push/ccc"

                                                            

static struct {
    GMutex   mu;
    GCond    cond;                          
    guint    calls;                 
    guint    status;                       
    gboolean block;                                                 
    gboolean released;
    guint    sleep_ms;                                         
    gchar   *last_auth;                               
    gchar   *last_urg;                       
    gsize    last_len;                
} H;

                                              
static guint
_hook(const gchar *endpoint, const guchar *body, gsize len,
      const gchar *vapid_auth, const gchar *urgency, guint ttl)
{
    (void)endpoint; (void)ttl;
                                                                 
    g_assert_nonnull(body);
    g_assert_cmpuint(len, >, 86);

    g_mutex_lock(&H.mu);
    H.calls++;
    H.last_len = len;
    g_free(H.last_auth);
    H.last_auth = g_strdup(vapid_auth);
    g_free(H.last_urg);
    H.last_urg = g_strdup(urgency);
    g_cond_broadcast(&H.cond);                                
    while (H.block && !H.released)                                   
        g_cond_wait(&H.cond, &H.mu);
    guint st = H.status;
    guint ms = H.sleep_ms;
    g_mutex_unlock(&H.mu);

    if (ms > 0)
        g_usleep((guint64)ms * 1000);                         
    return st;
}

static void
_hook_reset(guint status)
{
    g_mutex_lock(&H.mu);
    H.calls    = 0;
    H.status   = status;
    H.block    = FALSE;
    H.released = FALSE;
    H.sleep_ms = 0;
    H.last_len = 0;
    g_clear_pointer(&H.last_auth, g_free);
    g_clear_pointer(&H.last_urg, g_free);
    g_mutex_unlock(&H.mu);
}

static guint
_hook_calls(void)
{
    g_mutex_lock(&H.mu);
    guint n = H.calls;
    g_mutex_unlock(&H.mu);
    return n;
}

                                      
static void
_hook_wait_calls(guint n)
{
    g_mutex_lock(&H.mu);
    while (H.calls < n)
        g_cond_wait(&H.cond, &H.mu);
    g_mutex_unlock(&H.mu);
}

                       
static void
_hook_release(void)
{
    g_mutex_lock(&H.mu);
    H.released = TRUE;
    g_cond_broadcast(&H.cond);
    g_mutex_unlock(&H.mu);
}

                              
static void
_hook_set_blocking(void)
{
    g_mutex_lock(&H.mu);
    H.block    = TRUE;
    H.released = FALSE;
    g_mutex_unlock(&H.mu);
}

                                     
static void
_hook_set_delay(guint ms)
{
    g_mutex_lock(&H.mu);
    H.sleep_ms = ms;
    g_mutex_unlock(&H.mu);
}

                                                      

typedef struct {
    gchar *dir;
    gchar *db;
    gchar *pem;
} WpFix;

static void
_fix_up(WpFix *f, guint hook_status)
{
    GError *err = NULL;
    f->dir = g_dir_make_tmp("pcv_webpush_XXXXXX", &err);
    g_assert_no_error(err);
    f->db  = g_build_filename(f->dir, "webpush.db", NULL);
    f->pem = g_build_filename(f->dir, "vapid.pem", NULL);

    g_assert_true(pcv_webpush_init(f->db, f->pem, &err));
    g_assert_no_error(err);
    pcv_webpush_set_policy(TRUE, FALSE, NULL);
    pcv_webpush_set_post_hook(_hook);
    _hook_reset(hook_status);
}

static void
_fix_down(WpFix *f)
{
    pcv_webpush_shutdown();
    pcv_webpush_set_post_hook(NULL);                              

                                   
    gchar *wal = g_strconcat(f->db, "-wal", NULL);
    gchar *shm = g_strconcat(f->db, "-shm", NULL);
    g_remove(wal);
    g_remove(shm);
    g_remove(f->db);
    g_remove(f->pem);
    g_rmdir(f->dir);
    g_free(wal);
    g_free(shm);
    g_free(f->db);
    g_free(f->pem);
    g_free(f->dir);
    memset(f, 0, sizeof *f);
}

                       
static guint
_list_len(void)
{
    JsonArray *a = pcv_webpush_list();
    g_assert_nonnull(a);
    guint n = json_array_get_length(a);
    json_array_unref(a);
    return n;
}

                                        
static gchar *
_list0_str(const gchar *member)
{
    JsonArray *a = pcv_webpush_list();
    gchar     *v = NULL;
    if (json_array_get_length(a) > 0) {
        JsonObject *o = json_array_get_object_element(a, 0);
        v = g_strdup(json_object_get_string_member(o, member));
    }
    json_array_unref(a);
    return v;
}

  
                                             
                                                          
                                         
                            
   
static gint64
_db_rowid_of(const gchar *db_path, const gchar *endpoint)
{
    sqlite3 *db = NULL;
    g_assert_cmpint(sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL),
                    ==, SQLITE_OK);

    sqlite3_stmt *st = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(db, "SELECT id FROM subscriptions"
                                           " WHERE endpoint=?", -1, &st, NULL),
                    ==, SQLITE_OK);
    sqlite3_bind_text(st, 1, endpoint, -1, SQLITE_TRANSIENT);

    gint64 id = -1;
    if (sqlite3_step(st) == SQLITE_ROW)
        id = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return id;
}

                                 
static gint64
_list0_int(const gchar *member)
{
    JsonArray *a = pcv_webpush_list();
    gint64     v = -1;
    if (json_array_get_length(a) > 0) {
        JsonObject *o = json_array_get_object_element(a, 0);
        v = json_object_get_int_member(o, member);
    }
    json_array_unref(a);
    return v;
}

                                                               

  
                                   
                                                      
                                                      
                                           
   
static void
test_uninit_noop(void)
{
    GError *err = NULL;

    g_assert_false(pcv_webpush_subscribe("alice", T_EP_A, T_P256DH, T_AUTH, &err));
    g_assert_nonnull(err);
    g_clear_error(&err);

    g_assert_false(pcv_webpush_unsubscribe(T_EP_A, NULL, &err));
    g_clear_error(&err);

    g_assert_false(pcv_webpush_send_test(NULL, &err));
    g_clear_error(&err);

    g_assert_cmpuint(pcv_webpush_vapid_rotate(&err), ==, 0);
    g_clear_error(&err);

    g_assert_null(pcv_webpush_vapid_public());
    g_assert_cmpuint(pcv_webpush_remove_user("alice"), ==, 0);
    g_assert_cmpuint(_list_len(), ==, 0);                           

                                            
    pcv_webpush_notify("cpu", TRUE, "테스트");
    g_assert_true(pcv_webpush_wait_idle());

                                                                    
                                                                    
    g_assert_false(pcv_webpush_init("", "", &err));
    g_assert_nonnull(err);
    g_clear_error(&err);
    g_assert_false(pcv_webpush_init("/proc/pcv-no-such-dir/wp.db",
                                    "/proc/pcv-no-such-dir/vapid.pem", &err));
    g_assert_nonnull(err);
    g_clear_error(&err);

                             
    pcv_webpush_shutdown();
}

                                  
static void
test_subscribe_crud(void)
{
    WpFix   f   = {0};
    GError *err = NULL;
    _fix_up(&f, 201);

    g_assert_true(pcv_webpush_subscribe("alice", T_EP_A, T_P256DH, T_AUTH, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(_list_len(), ==, 1);

    gchar *user = _list0_str("username");
    g_assert_cmpstr(user, ==, "alice");
    g_free(user);

                                                               
    JsonArray  *a = pcv_webpush_list();
    JsonObject *o = json_array_get_object_element(a, 0);
    g_assert_false(json_object_has_member(o, "p256dh"));
    g_assert_false(json_object_has_member(o, "auth"));
    g_assert_cmpint(json_object_get_int_member(o, "fail_count"), ==, 0);
    g_assert_cmpint(json_object_get_int_member(o, "last_ok_at"), ==, 0);
    g_assert_cmpint(json_object_get_int_member(o, "created_at"), >, 0);
    json_array_unref(a);

    g_assert_true(pcv_webpush_unsubscribe(T_EP_A, "alice", &err));
    g_assert_no_error(err);
    g_assert_cmpuint(_list_len(), ==, 0);

    _fix_down(&f);
}

                                                
static void
test_subscribe_upsert(void)
{
    WpFix   f   = {0};
    GError *err = NULL;
    _fix_up(&f, 201);

    g_assert_true(pcv_webpush_subscribe("alice", T_EP_A, T_P256DH, T_AUTH, &err));
    g_assert_true(pcv_webpush_subscribe("bob", T_EP_A, T_P256DH, T_AUTH, &err));
    g_assert_no_error(err);

    g_assert_cmpuint(_list_len(), ==, 1);
    gchar *user = _list0_str("username");
    g_assert_cmpstr(user, ==, "bob");
    g_free(user);

    _fix_down(&f);
}

                                            
static void
test_unsubscribe_owner_scope(void)
{
    WpFix   f   = {0};
    GError *err = NULL;
    _fix_up(&f, 201);

    g_assert_true(pcv_webpush_subscribe("alice", T_EP_A, T_P256DH, T_AUTH, &err));
    g_assert_no_error(err);

    g_assert_false(pcv_webpush_unsubscribe(T_EP_A, "bob", &err));
    g_assert_nonnull(err);
    g_clear_error(&err);
    g_assert_cmpuint(_list_len(), ==, 1);

                                             
    g_assert_true(pcv_webpush_unsubscribe(T_EP_A, NULL, &err));
    g_assert_cmpuint(_list_len(), ==, 0);

    _fix_down(&f);
}

  
                                                        
                                                 
                            
   
static void
test_ssrf_guard(void)
{
    static const gchar *blocked[] = {
        "http://push.example.com/x",                        
        "https://127.0.0.1/x",                         
        "https://192.168.254.1/x",                                  
        "https://10.0.0.5/x",                              
        "https://169.254.169.254/latest",                           
        "https://[::ffff:192.168.254.1]/x",                    
        "https://[::1]/x",                                  
        "https://[fd00::1]/x",                              
        "https://100.64.0.1/x",                          
        "https://0.0.0.0/x",                                 
        "",                                              
        "not-a-url",                                     
    };

    for (gsize i = 0; i < G_N_ELEMENTS(blocked); i++) {
        GError *err = NULL;
        g_assert_false(pcv_webpush_endpoint_allowed(blocked[i], &err));
        g_assert_nonnull(err);                       
        g_clear_error(&err);
    }

    GError *err = NULL;
    g_assert_true(pcv_webpush_endpoint_allowed(T_EP_A, &err));
    g_assert_no_error(err);

                                         
    WpFix f = {0};
    _fix_up(&f, 201);
    g_assert_false(pcv_webpush_subscribe("alice", "https://127.0.0.1/x",
                                         T_P256DH, T_AUTH, &err));
    g_assert_nonnull(err);
    g_clear_error(&err);
    g_assert_cmpuint(_list_len(), ==, 0);
    _fix_down(&f);
}

                                            
static void
test_notify_prune_410(void)
{
    WpFix   f   = {0};
    GError *err = NULL;
    _fix_up(&f, 410);

    g_assert_true(pcv_webpush_subscribe("alice", T_EP_A, T_P256DH, T_AUTH, &err));
    g_assert_no_error(err);

    pcv_webpush_notify("cpu", TRUE, "CPU 95%");
    g_assert_true(pcv_webpush_wait_idle());                                  

    g_assert_cmpuint(_hook_calls(), ==, 1);
    g_assert_cmpuint(_list_len(), ==, 0);                 

                                     
    g_mutex_lock(&H.mu);
    g_assert_cmpstr(H.last_urg, ==, "high");
    g_assert_nonnull(H.last_auth);
    g_assert_true(g_str_has_prefix(H.last_auth, "vapid t="));
    g_assert_nonnull(strstr(H.last_auth, ", k="));
    g_mutex_unlock(&H.mu);

                                   
    _hook_reset(500);
    g_assert_true(pcv_webpush_subscribe("alice", T_EP_A, T_P256DH, T_AUTH, &err));
    pcv_webpush_notify("cpu", FALSE, "CPU 85%");
    g_assert_true(pcv_webpush_wait_idle());

    g_assert_cmpuint(_hook_calls(), ==, 1);
    g_assert_cmpuint(_list_len(), ==, 1);
    g_assert_cmpint(_list0_int("fail_count"), ==, 1);
    g_assert_cmpint(_list0_int("last_ok_at"), ==, 0);

                                 
    g_mutex_lock(&H.mu);
    g_assert_cmpstr(H.last_urg, ==, "normal");
    g_mutex_unlock(&H.mu);

                                             
    _hook_reset(201);
    pcv_webpush_notify("cpu", TRUE, "CPU 96%");
    g_assert_true(pcv_webpush_wait_idle());
    g_assert_cmpuint(_list_len(), ==, 1);
    g_assert_cmpint(_list0_int("fail_count"), ==, 0);
    g_assert_cmpint(_list0_int("last_ok_at"), >, 0);

    _fix_down(&f);
}

                                                             
static void
test_notify_severity_filter(void)
{
    WpFix   f   = {0};
    GError *err = NULL;
    _fix_up(&f, 201);

    g_assert_true(pcv_webpush_subscribe("alice", T_EP_A, T_P256DH, T_AUTH, &err));
    g_assert_no_error(err);

    pcv_webpush_set_policy(TRUE, TRUE, NULL);               
    pcv_webpush_notify("cpu", FALSE, "WARN 은 걸러진다");
    g_assert_true(pcv_webpush_wait_idle());
    g_assert_cmpuint(_hook_calls(), ==, 0);

    pcv_webpush_notify("cpu", TRUE, "CRIT 은 나간다");
    g_assert_true(pcv_webpush_wait_idle());
    g_assert_cmpuint(_hook_calls(), ==, 1);

    pcv_webpush_set_policy(FALSE, FALSE, NULL);              
    _hook_reset(201);
    pcv_webpush_notify("cpu", TRUE, "off 면 안 나간다");
    g_assert_true(pcv_webpush_wait_idle());
    g_assert_cmpuint(_hook_calls(), ==, 0);

                                             
    g_assert_true(pcv_webpush_send_test(NULL, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_webpush_wait_idle());
    g_assert_cmpuint(_hook_calls(), ==, 1);

    _fix_down(&f);
}

                                      
static void
test_vapid_rotate(void)
{
    WpFix   f   = {0};
    GError *err = NULL;
    _fix_up(&f, 201);

    gchar *pub1 = pcv_webpush_vapid_public();
    g_assert_nonnull(pub1);
    g_assert_cmpuint(strlen(pub1), >, 80);                            
    g_assert_true(g_file_test(f.pem, G_FILE_TEST_EXISTS));

    g_assert_true(pcv_webpush_subscribe("alice", T_EP_A, T_P256DH, T_AUTH, &err));
    g_assert_true(pcv_webpush_subscribe("bob", T_EP_B, T_P256DH, T_AUTH, &err));
    g_assert_no_error(err);

    g_assert_cmpuint(pcv_webpush_vapid_rotate(&err), ==, 2);
    g_assert_no_error(err);
    g_assert_cmpuint(_list_len(), ==, 0);

    gchar *pub2 = pcv_webpush_vapid_public();
    g_assert_nonnull(pub2);
    g_assert_cmpstr(pub1, !=, pub2);

                                                  
    pcv_webpush_shutdown();
    g_assert_true(pcv_webpush_init(f.db, f.pem, &err));
    g_assert_no_error(err);
    gchar *pub3 = pcv_webpush_vapid_public();
    g_assert_cmpstr(pub2, ==, pub3);

    g_free(pub1);
    g_free(pub2);
    g_free(pub3);
    _fix_down(&f);
}

                                         
static void
test_remove_user_cascade(void)
{
    WpFix   f   = {0};
    GError *err = NULL;
    _fix_up(&f, 201);

    g_assert_true(pcv_webpush_subscribe("alice", T_EP_A, T_P256DH, T_AUTH, &err));
    g_assert_true(pcv_webpush_subscribe("alice", T_EP_B, T_P256DH, T_AUTH, &err));
    g_assert_true(pcv_webpush_subscribe("bob", T_EP_C, T_P256DH, T_AUTH, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(_list_len(), ==, 3);

    g_assert_cmpuint(pcv_webpush_remove_user("alice"), ==, 2);
    g_assert_cmpuint(_list_len(), ==, 1);

    gchar *user = _list0_str("username");
    g_assert_cmpstr(user, ==, "bob");
    g_free(user);

                     
    g_assert_cmpuint(pcv_webpush_remove_user("nobody"), ==, 0);
    g_assert_cmpuint(_list_len(), ==, 1);

    _fix_down(&f);
}

  
                                                    
  
                                                   
                                                    
  
                                                        
                                                                  
                                                          
                                                         
                                         
   
#define WP_TEST_SLOW_MS 300u
#define WP_TEST_SLOW_N  5u

static void
test_shutdown_bounded(void)
{
    WpFix   f   = {0};
    GError *err = NULL;
    _fix_up(&f, 201);

    for (guint i = 0; i < WP_TEST_SLOW_N; i++) {
        gchar *ep = g_strdup_printf("https://203.0.113.%u/push/slow%u", 20 + i, i);
        g_assert_true(pcv_webpush_subscribe("alice", ep, T_P256DH, T_AUTH, &err));
        g_assert_no_error(err);
        g_free(ep);
    }

    _hook_set_delay(WP_TEST_SLOW_MS);
    pcv_webpush_notify("cpu", TRUE, "느린 푸시 서비스");
    _hook_wait_calls(1);                                    

    gint64 t0 = g_get_monotonic_time();
    pcv_webpush_shutdown();                                        
    gint64 elapsed_ms = (g_get_monotonic_time() - t0) / 1000;

                                     
    g_assert_cmpuint(_hook_calls(), ==, 1);
                                               
    g_assert_cmpint(elapsed_ms, <, (gint)(WP_TEST_SLOW_MS * WP_TEST_SLOW_N) - 500);

    _fix_down(&f);
}

  
                                           
  
                                               
                                                   
                                
  
                                                             
                                                                      
                                                     
                     
   
static void
test_prune_aba_rowid_reuse(void)
{
    WpFix   f   = {0};
    GError *err = NULL;
    _fix_up(&f, 410);                                    

    g_assert_true(pcv_webpush_subscribe("alice", T_EP_A, T_P256DH, T_AUTH, &err));
    g_assert_no_error(err);
    gint64 old_id = _db_rowid_of(f.db, T_EP_A);
    g_assert_cmpint(old_id, >, 0);

    _hook_set_blocking();
    pcv_webpush_notify("cpu", TRUE, "발송 중 해지·재구독");
    _hook_wait_calls(1);                                     

                                             
    g_assert_true(pcv_webpush_unsubscribe(T_EP_A, NULL, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_webpush_subscribe("bob", T_EP_B, T_P256DH, T_AUTH, &err));
    g_assert_no_error(err);

                                                  
    gint64 new_id = _db_rowid_of(f.db, T_EP_B);
    g_assert_cmpint(new_id, ==, old_id);

    _hook_release();
    g_assert_true(pcv_webpush_wait_idle());

                                                   
    g_assert_cmpuint(_list_len(), ==, 1);
    gchar *user = _list0_str("username");
    g_assert_cmpstr(user, ==, "bob");
    g_free(user);
    gchar *ep = _list0_str("endpoint");
    g_assert_cmpstr(ep, ==, T_EP_B);
    g_free(ep);

    _fix_down(&f);
}

                                
  
                                                
                                                
                                         
                                                         
                                                      
                                            
   
static void
test_rbac_policy(void)
{
                                                       
    pcv_dispatcher_init_policy_map();

    static const struct { const gchar *method; gint min_role; } TBL[] = {
        { "push.vapid.get",    0 },                             
        { "push.subscribe",    0 },                         
        { "push.unsubscribe",  0 },                                     
        { "push.list",         2 },                            
        { "push.vapid.rotate", 2 },                        
        { "push.test",         2 },                               
    };

    for (gsize i = 0; i < G_N_ELEMENTS(TBL); i++) {
        for (gint role = 0; role <= 2; role++) {
            gboolean expect = (role >= TBL[i].min_role);
            g_assert_cmpint(pcv_dispatcher_check_rbac(TBL[i].method, role),
                            ==, expect);
        }
    }

                                                    
                                                   
    g_assert_false(pcv_dispatcher_check_rbac("push.list", 1));
    g_assert_false(pcv_dispatcher_check_rbac("push.vapid.rotate", 1));
    g_assert_false(pcv_dispatcher_check_rbac("push.test", 1));
}

                                                 
  
                                                  
                                               
                  
                                                                          
                                                                 
                                                         
                                                            
                                                           
                                               
                                                                 
                 
   
static void
test_rbac_rest_gate_agreement(void)
{
    gchar *dir = g_dir_make_tmp("pcv_webpush_rbac_XXXXXX", NULL);
    g_assert_nonnull(dir);
    gchar *db = g_build_filename(dir, "rbac.db", NULL);
    pcv_rbac_init(db);

    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("wp_viewer", "pw", PCV_ROLE_VIEWER, NULL, &err));
    g_assert_no_error(err);

                                                 
                                                                    
                                                    
                                                        
    g_assert_true(pcv_rbac_check_permission("wp_viewer", "push.vapid.get"));
    g_assert_true(pcv_rbac_check_permission("wp_viewer", "push.subscribe"));
    g_assert_true(pcv_rbac_check_permission("wp_viewer", "push.unsubscribe"));
    g_assert_true(pcv_rbac_check_permission("wp_viewer", "push.mine"));

                                                            
                                                              
                                               
    pcv_dispatcher_init_policy_map();
    g_assert_false(pcv_rbac_check_permission("wp_viewer", "push.vapid.rotate"));
    g_assert_false(pcv_rbac_check_permission("wp_viewer", "push.test"));
    g_assert_false(pcv_dispatcher_check_rbac("push.list", PCV_ROLE_VIEWER));
                                                                 
                                                
    g_assert_true(pcv_dispatcher_check_rbac("push.mine", PCV_ROLE_VIEWER));

    pcv_rbac_shutdown();

    gchar *wal = g_strconcat(db, "-wal", NULL);
    gchar *shm = g_strconcat(db, "-shm", NULL);
    g_remove(wal);
    g_remove(shm);
    g_remove(db);
    g_rmdir(dir);
    g_free(wal);
    g_free(shm);
    g_free(db);
    g_free(dir);
}

                                                            
                                                               
                                                       
                                                
static gchar *
_ep_n(guint i)
{
    g_assert_cmpuint(i, <=, 154u);
    return g_strdup_printf("https://203.0.113.%u/push/e%u", 100u + i, i);
}

  
                        
  
                                               
                                               
                                          
  
                                                  
                                 
                                                    
                                              
                                                   
   
static void
test_subscribe_per_user_cap(void)
{
    WpFix   f   = {0};
    GError *err = NULL;
    _fix_up(&f, 201);

                      
    for (guint i = 0; i < PCV_WEBPUSH_MAX_SUBS_PER_USER; i++) {
        gchar *ep = _ep_n(i);
        g_assert_true(pcv_webpush_subscribe("alice", ep, T_P256DH, T_AUTH, &err));
        g_assert_no_error(err);
        g_free(ep);
    }
    g_assert_cmpuint(_list_len(), ==, PCV_WEBPUSH_MAX_SUBS_PER_USER);

                                                     
                                                                      
                                                        
    gchar *over = _ep_n(PCV_WEBPUSH_MAX_SUBS_PER_USER);
    g_assert_false(pcv_webpush_subscribe("alice", over, T_P256DH, T_AUTH, &err));
    g_assert_error(err, pcv_webpush_error_quark(), WP_ERR_LIMIT);
    g_clear_error(&err);
    g_assert_cmpuint(_list_len(), ==, PCV_WEBPUSH_MAX_SUBS_PER_USER);

                                                         
    gchar *own = _ep_n(0);
    g_assert_true(pcv_webpush_subscribe("alice", own, T_P256DH, T_AUTH, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(_list_len(), ==, PCV_WEBPUSH_MAX_SUBS_PER_USER);

                                       
    g_assert_true(pcv_webpush_subscribe("bob", over, T_P256DH, T_AUTH, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(_list_len(), ==, PCV_WEBPUSH_MAX_SUBS_PER_USER + 1);

                                                   
                                              
    g_assert_true(pcv_webpush_unsubscribe(own, "alice", &err));
    g_assert_no_error(err);
    gchar *again = _ep_n(PCV_WEBPUSH_MAX_SUBS_PER_USER + 1);
    g_assert_true(pcv_webpush_subscribe("alice", again, T_P256DH, T_AUTH, &err));
    g_assert_no_error(err);

    g_free(again);
    g_free(own);
    g_free(over);
    _fix_down(&f);
}

  
                                            
  
                                                                 
                                                      
                                               
            
                                                       
                         
   
static void
test_subscribe_cap_not_bypassed_by_takeover(void)
{
    WpFix   f   = {0};
    GError *err = NULL;
    _fix_up(&f, 201);

                                      
    gchar *shared = _ep_n(50);
    g_assert_true(pcv_webpush_subscribe("bob", shared, T_P256DH, T_AUTH, &err));
    g_assert_no_error(err);

                           
    for (guint i = 0; i < PCV_WEBPUSH_MAX_SUBS_PER_USER; i++) {
        gchar *ep = _ep_n(i);
        g_assert_true(pcv_webpush_subscribe("alice", ep, T_P256DH, T_AUTH, &err));
        g_assert_no_error(err);
        g_free(ep);
    }

                                              
                                                                  
    g_assert_false(pcv_webpush_subscribe("alice", shared, T_P256DH, T_AUTH, &err));
    g_assert_error(err, pcv_webpush_error_quark(), WP_ERR_LIMIT);
    g_clear_error(&err);

                                                  
                                                                    
    gchar *owner = _list0_str("username");
    g_assert_cmpstr(owner, ==, "bob");
    g_free(owner);
    g_assert_cmpuint(_list_len(), ==, PCV_WEBPUSH_MAX_SUBS_PER_USER + 1);

    g_free(shared);
    _fix_down(&f);
}

  
                            
  
                                                
                                                  
  
                                                              
                                                            
                                                      
   
static void
test_list_mine_owner_scope_and_digest(void)
{
    WpFix   f   = {0};
    GError *err = NULL;
    _fix_up(&f, 201);

    g_assert_true(pcv_webpush_subscribe("alice", T_EP_A, T_P256DH, T_AUTH, &err));
    g_assert_true(pcv_webpush_subscribe("alice", T_EP_B, T_P256DH, T_AUTH, &err));
    g_assert_true(pcv_webpush_subscribe("bob",   T_EP_C, T_P256DH, T_AUTH, &err));
    g_assert_no_error(err);

    JsonArray *a = pcv_webpush_list_mine("alice");
    g_assert_cmpuint(json_array_get_length(a), ==, 2);

    JsonObject *o = json_array_get_object_element(a, 0);
                                                
    g_assert_false(json_object_has_member(o, "username"));
    g_assert_false(json_object_has_member(o, "p256dh"));
    g_assert_false(json_object_has_member(o, "auth"));

                                                                   
    gchar *want = g_compute_checksum_for_string(G_CHECKSUM_SHA256, T_EP_A, -1);
    g_assert_cmpstr(json_object_get_string_member(o, "digest"), ==, want);
    g_free(want);

                                                         
    const gchar *ep = json_object_get_string_member(o, "endpoint");
    g_assert_cmpuint(strlen(ep), <=, 80);
    json_array_unref(a);

                          
    JsonArray *b = pcv_webpush_list_mine("bob");
    g_assert_cmpuint(json_array_get_length(b), ==, 1);
    json_array_unref(b);

                                         
    JsonArray *none = pcv_webpush_list_mine(NULL);
    g_assert_cmpuint(json_array_get_length(none), ==, 0);
    json_array_unref(none);
    JsonArray *empty = pcv_webpush_list_mine("");
    g_assert_cmpuint(json_array_get_length(empty), ==, 0);
    json_array_unref(empty);

                                                    
    JsonArray *nobody = pcv_webpush_list_mine("carol");
    g_assert_nonnull(nobody);
    g_assert_cmpuint(json_array_get_length(nobody), ==, 0);
    json_array_unref(nobody);

    _fix_down(&f);
}

                                   
  
                                              
                        
                                                                
                                                 
                                             
   
static void
test_subscribe_param_guard(void)
{
    WpFix   f   = {0};
    GError *err = NULL;
    _fix_up(&f, 201);

                                                                
    g_assert_false(pcv_webpush_subscribe(NULL,    T_EP_A, T_P256DH, T_AUTH, &err));
    g_clear_error(&err);
    g_assert_false(pcv_webpush_subscribe("",      T_EP_A, T_P256DH, T_AUTH, &err));
    g_clear_error(&err);
    g_assert_false(pcv_webpush_subscribe("alice", NULL,   T_P256DH, T_AUTH, &err));
    g_clear_error(&err);
    g_assert_false(pcv_webpush_subscribe("alice", "",     T_P256DH, T_AUTH, &err));
    g_clear_error(&err);
    g_assert_false(pcv_webpush_subscribe("alice", T_EP_A, NULL,     T_AUTH, &err));
    g_clear_error(&err);
    g_assert_false(pcv_webpush_subscribe("alice", T_EP_A, "",       T_AUTH, &err));
    g_clear_error(&err);
    g_assert_false(pcv_webpush_subscribe("alice", T_EP_A, T_P256DH, NULL,   &err));
    g_clear_error(&err);
    g_assert_false(pcv_webpush_subscribe("alice", T_EP_A, T_P256DH, "",     &err));
    g_clear_error(&err);

    g_assert_cmpuint(_list_len(), ==, 0);

                                                
                                                            
    g_assert_true(pcv_webpush_subscribe("alice", T_EP_A, T_P256DH, T_AUTH, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(_list_len(), ==, 1);

    _fix_down(&f);
}

  
                                                 
  
                                                 
                                                         
                  
  
                                                              
                                                      
                                                             
                     
                                                 
                                                     
                                                            
                                                         
                                         
                                                        
                                                           
   
static void
test_notify_no_subs_keeps_no_key(void)
{
    WpFix   f   = {0};
    GError *err = NULL;
    _fix_up(&f, 201);

                                                         
    g_assert_false(g_file_test(f.pem, G_FILE_TEST_EXISTS));
    g_assert_cmpuint(_list_len(), ==, 0);

                                                       
    pcv_webpush_notify("cpu", FALSE, "구독 0 — 아무 일도 없어야 한다");
    pcv_webpush_notify("cpu", TRUE, "구독 0 — CRIT 도 마찬가지");
    g_assert_true(pcv_webpush_wait_idle());

    g_assert_false(g_file_test(f.pem, G_FILE_TEST_EXISTS));
    g_assert_cmpuint(_hook_calls(), ==, 0);

                                                   
    g_assert_true(pcv_webpush_subscribe("alice", T_EP_A, T_P256DH, T_AUTH, &err));
    g_assert_no_error(err);
    pcv_webpush_notify("cpu", TRUE, "구독 1 — 이제는 나간다");
    g_assert_true(pcv_webpush_wait_idle());

    g_assert_true(g_file_test(f.pem, G_FILE_TEST_EXISTS));
    g_assert_cmpuint(_hook_calls(), ==, 1);

    _fix_down(&f);
}

  
                                               
  
                                               
                                           
  
                                                  
                                                                         
               
                                                            
                                                       
                                
                                                               
                                                    
   
static void
test_alert_fire_event_reaches_push(void)
{
    WpFix   f   = {0};
    GError *err = NULL;
    _fix_up(&f, 201);

    g_assert_true(pcv_webpush_subscribe("alice", T_EP_A, T_P256DH, T_AUTH, &err));
    g_assert_no_error(err);

                                                        
                                                      
                                                    
                         
    pcv_alert_engine_init();
    pcv_alert_fire_event("sp2b-push-seam", TRUE, 1.0, "배선 회귀 감시용 경보");
    g_assert_true(pcv_webpush_wait_idle());
    g_assert_cmpuint(_hook_calls(), ==, 1);

                                                       
                                        
    pcv_webpush_set_policy(FALSE, FALSE, NULL);
    _hook_reset(201);
    pcv_alert_fire_event("sp2b-push-seam", TRUE, 2.0, "off 면 안 나간다");
    g_assert_true(pcv_webpush_wait_idle());
    g_assert_cmpuint(_hook_calls(), ==, 0);

    pcv_alert_engine_shutdown();
    _fix_down(&f);
}

  
                                                  
  
                                              
                                                 
                                   
  
                                                    
                                                                  
                                                      
                                                                
                                         
                                                     
                                                   
   
static void
test_user_delete_cascades_push(void)
{
    WpFix   f   = {0};
    GError *err = NULL;
    _fix_up(&f, 201);

                                                          
    gchar *rbac_db = g_build_filename(f.dir, "rbac.db", NULL);
    pcv_rbac_init(rbac_db);
    g_assert_true(pcv_rbac_user_create("carol", "Str0ng-P@ssw0rd", PCV_ROLE_VIEWER,
                                       NULL, &err));
    g_assert_no_error(err);

    g_assert_true(pcv_webpush_subscribe("carol", T_EP_A, T_P256DH, T_AUTH, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_webpush_subscribe("bob", T_EP_B, T_P256DH, T_AUTH, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(_list_len(), ==, 2);

    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "username", "carol");
    handle_auth_user_delete(params, "1", NULL, NULL);
    json_object_unref(params);

                                 
    g_assert_cmpint(pcv_rbac_user_exists("carol"), ==, PCV_USER_ABSENT);
    g_assert_cmpuint(_list_len(), ==, 1);
    gchar *owner = _list0_str("username");
    g_assert_cmpstr(owner, ==, "bob");
    g_free(owner);

    pcv_rbac_shutdown();
    g_remove(rbac_db);
    g_free(rbac_db);
    _fix_down(&f);
}

void
test_webpush_register(void)
{
                                                             
    g_test_add_func("/webpush/uninit_noop", test_uninit_noop);
    g_test_add_func("/webpush/subscribe_crud", test_subscribe_crud);
    g_test_add_func("/webpush/subscribe_upsert", test_subscribe_upsert);
    g_test_add_func("/webpush/unsubscribe_owner_scope", test_unsubscribe_owner_scope);
    g_test_add_func("/webpush/ssrf_guard", test_ssrf_guard);
    g_test_add_func("/webpush/notify_prune_410", test_notify_prune_410);
    g_test_add_func("/webpush/notify_severity_filter", test_notify_severity_filter);
    g_test_add_func("/webpush/vapid_rotate", test_vapid_rotate);
    g_test_add_func("/webpush/remove_user_cascade", test_remove_user_cascade);
    g_test_add_func("/webpush/prune_aba_rowid_reuse", test_prune_aba_rowid_reuse);
    g_test_add_func("/webpush/shutdown_bounded", test_shutdown_bounded);
                                                            
    g_test_add_func("/webpush/rbac_policy", test_rbac_policy);
    g_test_add_func("/webpush/rbac_rest_gate_agreement", test_rbac_rest_gate_agreement);
    g_test_add_func("/webpush/subscribe_param_guard", test_subscribe_param_guard);
    g_test_add_func("/webpush/subscribe_per_user_cap", test_subscribe_per_user_cap);
    g_test_add_func("/webpush/subscribe_cap_not_bypassed_by_takeover",
                    test_subscribe_cap_not_bypassed_by_takeover);
    g_test_add_func("/webpush/list_mine_owner_scope_and_digest",
                    test_list_mine_owner_scope_and_digest);
                                                            
    g_test_add_func("/webpush/notify_no_subs_keeps_no_key",
                    test_notify_no_subs_keeps_no_key);
    g_test_add_func("/webpush/alert_fire_event_reaches_push",
                    test_alert_fire_event_reaches_push);
    g_test_add_func("/webpush/user_delete_cascades_push",
                    test_user_delete_cascades_push);
}
