                                                                                                          
                                                                                   
                                                                          
                                                                                 
                                                            
                                                                            
                                            
#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <string.h>

#include "../src/modules/auth/pcv_rbac.h"
#include "../src/modules/auth/pcv_rbac_internal.h"
#include "../src/utils/pcv_jwt.h"

                                                                     
void pcv_rbac_set_pbkdf2_probe_for_test(void (*probe)(gint iter, gboolean db_locked));
void pcv_rbac_attempts_reset_for_test(guint cards_max, gint64 (*now_fn)(void));
void pcv_rbac_attempts_login_stats_for_test(guint *cards, gsize *key_bytes);
gint pcv_rbac_attempts_login_card_for_test(const gchar *username);
void pcv_rbac_attempts_ip_stats_for_test(guint *cards);

#define TARGET_ITER 600000
#define WRONG_PW "not-the-password"

static gchar *g_dir = NULL;
static gchar *g_db = NULL;
static gint g_calls = 0;                     
static gint64 g_iter_total = 0;              
static gint64 g_iter_unlocked = 0;                              
static gint64 g_fake_now = 0;                              

                                                                               
static void (*g_race_action)(void) = NULL;
static gint g_race_iter = 0;
static gint g_race_locked_hits = 0;                                                       

static void
probe(gint iter, gboolean db_locked)
{
    g_calls++;
    g_iter_total += iter;
    if (!db_locked) g_iter_unlocked += iter;
    if (g_race_action && iter == g_race_iter) {
        if (db_locked) {
            g_race_locked_hits++;
            return;
        }
        void (*action)(void) = g_race_action;
        g_race_action = NULL;                                    
        action();
    }
}

static void
probe_reset(void)
{
    g_calls = 0;
    g_iter_total = 0;
    g_iter_unlocked = 0;
}

static gint64
fake_now(void)
{
    return g_fake_now;
}

                                              
static void
advance_sec(gint sec)
{
    g_fake_now += (gint64)sec * G_USEC_PER_SEC;
}

static void
db_setup(void)
{
    g_dir = g_dir_make_tmp("pcv-login-enum-XXXXXX", NULL);
    g_db = g_build_filename(g_dir, "rbac.db", NULL);
    pcv_jwt_init("login-enum-test-secret-32-bytes!!!!!");
    pcv_rbac_init(g_db);
    pcv_rbac_attempts_reset_for_test(0, NULL);
    probe_reset();
    g_race_action = NULL;
    g_race_iter = 0;
    g_race_locked_hits = 0;
    pcv_rbac_set_pbkdf2_probe_for_test(probe);
}

static void
db_teardown(void)
{
    pcv_rbac_set_pbkdf2_probe_for_test(NULL);
    pcv_rbac_attempts_reset_for_test(0, NULL);
    pcv_rbac_shutdown();
    pcv_jwt_shutdown();
    gchar *wal = g_strconcat(g_db, "-wal", NULL), *shm = g_strconcat(g_db, "-shm", NULL);
    g_unlink(g_db); g_unlink(wal); g_unlink(shm);
    g_free(wal); g_free(shm);
    g_rmdir(g_dir);
    g_clear_pointer(&g_db, g_free);
    g_clear_pointer(&g_dir, g_free);
}

                                                        
static void
seed_row(const gchar *username, const gchar *password_hash)
{
    sqlite3 *raw = NULL;
    g_assert_cmpint(sqlite3_open(g_db, &raw), ==, SQLITE_OK);
    sqlite3_stmt *st = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(raw,
        "INSERT INTO users(username, password_hash, salt, role, tenant) VALUES(?,?,?,2,NULL);",
        -1, &st, NULL), ==, SQLITE_OK);
    sqlite3_bind_text(st, 1, username, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, password_hash, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, "0123456789abcdef0123456789abcdef", -1, SQLITE_STATIC);
    g_assert_cmpint(sqlite3_step(st), ==, SQLITE_DONE);
    sqlite3_finalize(st);
    sqlite3_close(raw);
}

#define ZERO_HEX "0000000000000000000000000000000000000000000000000000000000000000"

                                                          
static gchar *
check_fail_msg(const gchar *username)
{
    GError *err = NULL;
    g_assert_false(pcv_rbac_password_check(username, WRONG_PW, &err));
    g_assert_nonnull(err);
    gchar *msg = g_strdup(err->message);
    g_error_free(err);
    return msg;
}

                                                                               
static void
test_absent_costs_one_pbkdf2_unlocked(void)
{
    db_setup();
    g_autofree gchar *msg = check_fail_msg("ghost-a");
    g_assert_cmpstr(msg, ==, "Invalid credentials");
    g_assert_cmpint(g_calls, ==, 1);
    g_assert_cmpint(g_iter_total, ==, TARGET_ITER);
    g_assert_cmpint(g_iter_unlocked, ==, TARGET_ITER);

                               
    probe_reset();
    GError *err = NULL;
    g_assert_null(pcv_rbac_authenticate("ghost-b", WRONG_PW, &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_assert_cmpstr(err->message, ==, "Invalid credentials");
    g_clear_error(&err);
    g_assert_cmpint(g_calls, ==, 1);
    g_assert_cmpint(g_iter_total, ==, TARGET_ITER);
    g_assert_cmpint(g_iter_unlocked, ==, TARGET_ITER);
    db_teardown();
}

                                                                          
                                                              
static void
test_existing_rows_cost_same_total(void)
{
    db_setup();
    seed_row("row-current", "pbkdf2:600000:" ZERO_HEX);
    seed_row("row-legacy-iter", "pbkdf2:" ZERO_HEX);
    seed_row("row-sha256", ZERO_HEX);
    seed_row("row-malformed", "pbkdf2:not-a-valid-hash");

    g_autofree gchar *m1 = check_fail_msg("row-current");
    g_assert_cmpstr(m1, ==, "Invalid credentials");
    g_assert_cmpint(g_calls, ==, 1);
    g_assert_cmpint(g_iter_total, ==, TARGET_ITER);
    g_assert_cmpint(g_iter_unlocked, ==, TARGET_ITER);                                                             

    probe_reset();
    g_autofree gchar *m2 = check_fail_msg("row-legacy-iter");
    g_assert_cmpstr(m2, ==, "Invalid credentials");
    g_assert_cmpint(g_calls, ==, 2);
    g_assert_cmpint(g_iter_total, ==, TARGET_ITER);
    g_assert_cmpint(g_iter_unlocked, ==, TARGET_ITER);

    probe_reset();
    g_autofree gchar *m3 = check_fail_msg("row-sha256");
    g_assert_cmpstr(m3, ==, "Invalid credentials");
    g_assert_cmpint(g_iter_total, ==, TARGET_ITER);
    g_assert_cmpint(g_iter_unlocked, ==, TARGET_ITER);

    probe_reset();
    g_autofree gchar *m4 = check_fail_msg("row-malformed");
    g_assert_cmpstr(m4, ==, "Invalid credentials");
    g_assert_cmpint(g_iter_total, ==, TARGET_ITER);
    g_assert_cmpint(g_iter_unlocked, ==, TARGET_ITER);
    db_teardown();
}

                                                                                     

#define RACE_OLD_PW "Old-Passw0rd!race"
#define RACE_NEW_PW "New-Passw0rd!race"

static void
race_change_password(void)
{
    GError *err = NULL;
    g_assert_true(pcv_rbac_change_password("race-user", RACE_OLD_PW, RACE_NEW_PW, &err));
    g_assert_no_error(err);
}

static void
race_delete_recreate(void)
{
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_delete("race-user", &err));
    g_assert_no_error(err);
    g_assert_true(pcv_rbac_user_create("race-user", RACE_NEW_PW, PCV_ROLE_VIEWER, NULL, &err));
    g_assert_no_error(err);
}

static void
race_delete_only(void)
{
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_delete("race-user", &err));
    g_assert_no_error(err);
}

                                               
static void
assert_old_password_lost(void)
{
    GError *err = NULL;
    g_assert_false(pcv_rbac_password_check("race-user", RACE_OLD_PW, &err));
    g_assert_cmpint(g_race_locked_hits, ==, 0);
    g_assert_null(g_race_action);                      
    g_assert_nonnull(err);
    g_assert_cmpstr(err->message, ==, "Invalid credentials");
    g_clear_error(&err);
}

                                         
static void
test_race_password_changed_during_hash(void)
{
    db_setup();
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("race-user", RACE_OLD_PW, PCV_ROLE_VIEWER, NULL, &err));
    g_race_iter = TARGET_ITER;
    g_race_action = race_change_password;
    assert_old_password_lost();
    g_assert_true(pcv_rbac_password_check("race-user", RACE_NEW_PW, &err));
    g_assert_no_error(err);
    db_teardown();
}

                                                    
static void
test_race_rehash_keeps_new_password(void)
{
    db_setup();
                                                               
    const gchar *salt = "0123456789abcdef0123456789abcdef";
    g_autofree gchar *salted = g_strconcat(salt, RACE_OLD_PW, NULL);
    g_autofree gchar *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, salted, -1);
    seed_row("race-user", digest);
    g_race_iter = TARGET_ITER;
    g_race_action = race_change_password;
    assert_old_password_lost();
    GError *err = NULL;
    g_assert_true(pcv_rbac_password_check("race-user", RACE_NEW_PW, &err));
    g_assert_no_error(err);
    db_teardown();
}

                                                 
static void
test_race_delete_recreate_during_hash(void)
{
    db_setup();
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("race-user", RACE_OLD_PW, PCV_ROLE_VIEWER, NULL, &err));
    g_race_iter = TARGET_ITER;
    g_race_action = race_delete_recreate;
    assert_old_password_lost();
    g_assert_true(pcv_rbac_password_check("race-user", RACE_NEW_PW, &err));
    g_assert_no_error(err);
    db_teardown();
}

                            
static void
test_race_delete_during_hash(void)
{
    db_setup();
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("race-user", RACE_OLD_PW, PCV_ROLE_VIEWER, NULL, &err));
    g_race_iter = TARGET_ITER;
    g_race_action = race_delete_only;
    assert_old_password_lost();
    db_teardown();
}

                                                                                 
static void
test_check_subject_returns_verified_row(void)
{
    db_setup();
    const gchar *salt = "0123456789abcdef0123456789abcdef";
    g_autofree gchar *salted = g_strconcat(salt, RACE_OLD_PW, NULL);
    g_autofree gchar *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, salted, -1);
    seed_row("subject-row", digest);
    GError *err = NULL;
    PcvRole role = PCV_ROLE_VIEWER;
    g_autofree gchar *nonce = NULL;
    g_assert_true(pcv_rbac_password_check_subject("subject-row", RACE_OLD_PW, &role, &nonce, &err));
    g_assert_no_error(err);
    PcvRole want_role = PCV_ROLE_VIEWER;
    g_autofree gchar *want_nonce = NULL;
    g_assert_cmpint(pcv_rbac_lookup_subject("subject-row", &want_role, &want_nonce), ==, PCV_USER_PRESENT);
    g_assert_cmpint(role, ==, want_role);
    g_assert_cmpstr(nonce, ==, want_nonce);
                                              
    probe_reset();
    g_assert_true(pcv_rbac_password_check("subject-row", RACE_OLD_PW, &err));
    g_assert_cmpint(g_calls, ==, 1);
    g_assert_cmpint(g_iter_total, ==, TARGET_ITER);
    g_assert_cmpint(g_iter_unlocked, ==, TARGET_ITER);
    db_teardown();
}

                                                                          
static void
test_lockout_sequence_parity(void)
{
    db_setup();
    seed_row("row-lock", "pbkdf2:600000:" ZERO_HEX);
    const gchar *names[] = { "row-lock", "ghost-lock" };
    for (guint n = 0; n < G_N_ELEMENTS(names); n++) {
        const gchar *name = names[n];
        for (int i = 0; i < 4; i++)
            pcv_rbac_attempts_login_record(name, FALSE);
        g_assert_false(pcv_rbac_is_locked(name));

        probe_reset();
        g_autofree gchar *fifth = check_fail_msg(name);
        g_assert_cmpstr(fifth, ==, "Invalid credentials");
        g_assert_cmpint(g_calls, ==, 1);
        g_assert_true(pcv_rbac_is_locked(name));
        g_assert_cmpint(pcv_rbac_get_remaining_lockout(name), >, 0);

        probe_reset();
        g_autofree gchar *locked = check_fail_msg(name);
        g_assert_true(g_str_has_prefix(locked, "Account locked — retry after "));
        g_assert_cmpint(g_calls, ==, 0);                              
    }
    db_teardown();
}

                              
static void
test_authenticate_records_absent_failure(void)
{
    db_setup();
    for (int i = 0; i < 4; i++)
        pcv_rbac_attempts_login_record("ghost-auth", FALSE);
    GError *err = NULL;
    g_assert_null(pcv_rbac_authenticate("ghost-auth", WRONG_PW, &err));
    g_clear_error(&err);
    g_assert_true(pcv_rbac_is_locked("ghost-auth"));
    db_teardown();
}

                                                  
static void
test_check_and_success_keep_no_card(void)
{
    pcv_rbac_attempts_reset_for_test(0, NULL);
    guint cards = 99;
    gsize bytes = 0;
    for (int i = 0; i < 64; i++) {
        g_autofree gchar *name = g_strdup_printf("probe-%d", i);
        g_assert_false(pcv_rbac_is_locked(name));
        g_assert_false(pcv_rbac_attempts_login_locked(name, NULL));
        pcv_rbac_attempts_login_record(name, TRUE);
    }
    pcv_rbac_attempts_login_stats_for_test(&cards, &bytes);
    g_assert_cmpuint(cards, ==, 0);

                                                 
    pcv_rbac_attempts_login_record("back", FALSE);
    g_assert_cmpint(pcv_rbac_attempts_login_card_for_test("back"), ==, 1);
    pcv_rbac_attempts_login_record("back", TRUE);
    g_assert_cmpint(pcv_rbac_attempts_login_card_for_test("back"), ==, -1);
    pcv_rbac_attempts_reset_for_test(0, NULL);
}

                                   
static void
test_cards_stay_under_cap(void)
{
    pcv_rbac_attempts_reset_for_test(8, fake_now);
    for (int i = 0; i < 200; i++) {
        g_autofree gchar *name = g_strdup_printf("flood-%d", i);
        advance_sec(1);
        pcv_rbac_attempts_login_record(name, FALSE);
        guint cards = 0;
        gsize bytes = 0;
        pcv_rbac_attempts_login_stats_for_test(&cards, &bytes);
        g_assert_cmpuint(cards, <=, 8);
    }
    g_assert_cmpint(pcv_rbac_attempts_login_card_for_test("flood-199"), ==, 1);                      
    pcv_rbac_attempts_reset_for_test(0, NULL);
}

                                       
static void
test_evicts_oldest_unlocked(void)
{
    pcv_rbac_attempts_reset_for_test(3, fake_now);
    advance_sec(1); pcv_rbac_attempts_login_record("old", FALSE);
    advance_sec(1); pcv_rbac_attempts_login_record("mid", FALSE);
    advance_sec(1); pcv_rbac_attempts_login_record("new", FALSE);
    advance_sec(1); pcv_rbac_attempts_login_record("old", FALSE);                              
    advance_sec(1); pcv_rbac_attempts_login_record("next", FALSE);
    g_assert_cmpint(pcv_rbac_attempts_login_card_for_test("mid"), ==, -1);
    g_assert_cmpint(pcv_rbac_attempts_login_card_for_test("old"), ==, 2);
    g_assert_cmpint(pcv_rbac_attempts_login_card_for_test("new"), ==, 1);
    g_assert_cmpint(pcv_rbac_attempts_login_card_for_test("next"), ==, 1);
    pcv_rbac_attempts_reset_for_test(0, NULL);
}

                                            
static void
test_locked_card_survives_older(void)
{
    pcv_rbac_attempts_reset_for_test(2, fake_now);
    advance_sec(1);
    for (int i = 0; i < 5; i++)
        pcv_rbac_attempts_login_record("locked", FALSE);
    advance_sec(1); pcv_rbac_attempts_login_record("young", FALSE);
    advance_sec(1); pcv_rbac_attempts_login_record("next", FALSE);
    g_assert_true(pcv_rbac_is_locked("locked"));
    g_assert_cmpint(pcv_rbac_attempts_login_card_for_test("young"), ==, -1);
    g_assert_cmpint(pcv_rbac_attempts_login_card_for_test("next"), ==, 1);
    pcv_rbac_attempts_reset_for_test(0, NULL);
}

                                                                     
static void
test_all_locked_records_nothing_then_expiry_frees(void)
{
    pcv_rbac_attempts_reset_for_test(2, fake_now);
    advance_sec(1);
    for (int i = 0; i < 5; i++)
        pcv_rbac_attempts_login_record("lock-a", FALSE);
    advance_sec(1);
    for (int i = 0; i < 5; i++)
        pcv_rbac_attempts_login_record("lock-b", FALSE);

    for (int i = 0; i < 6; i++)
        pcv_rbac_attempts_login_record("late", FALSE);
    g_assert_cmpint(pcv_rbac_attempts_login_card_for_test("late"), ==, -1);
    g_assert_false(pcv_rbac_is_locked("late"));
    g_assert_true(pcv_rbac_is_locked("lock-a"));
    g_assert_true(pcv_rbac_is_locked("lock-b"));
    guint cards = 0;
    gsize bytes = 0;
    pcv_rbac_attempts_login_stats_for_test(&cards, &bytes);
    g_assert_cmpuint(cards, ==, 2);

                                                  
    gint before = pcv_rbac_attempts_login_card_for_test("lock-a");
    pcv_rbac_attempts_login_record("lock-a", FALSE);
    g_assert_cmpint(pcv_rbac_attempts_login_card_for_test("lock-a"), ==, before + 1);

                                                                    
    advance_sec(31);
    pcv_rbac_attempts_login_record("late", FALSE);
    g_assert_cmpint(pcv_rbac_attempts_login_card_for_test("late"), ==, 1);
    g_assert_cmpint(pcv_rbac_attempts_login_card_for_test("lock-b"), ==, -1);
    g_assert_true(pcv_rbac_is_locked("lock-a"));
    pcv_rbac_attempts_reset_for_test(0, NULL);
}

                                                                
static void
test_long_names_bounded_and_distinct(void)
{
    pcv_rbac_attempts_reset_for_test(8, fake_now);
    gchar *long_a = g_strnfill(1024 * 1024, 'a');
    gchar *long_b = g_strdup(long_a);
    long_b[1024 * 1024 - 1] = 'b';
    advance_sec(1);
    for (int i = 0; i < 5; i++)
        pcv_rbac_attempts_login_record(long_a, FALSE);
    g_assert_true(pcv_rbac_is_locked(long_a));
    g_assert_cmpint(pcv_rbac_get_remaining_lockout(long_a), >, 0);
    g_assert_false(pcv_rbac_is_locked(long_b));
    pcv_rbac_attempts_login_record(long_b, FALSE);
    guint cards = 0;
    gsize bytes = 0;
    pcv_rbac_attempts_login_stats_for_test(&cards, &bytes);
    g_assert_cmpuint(cards, ==, 2);
    g_assert_cmpuint(bytes, <=, 2 * 256);
    pcv_rbac_attempts_login_record(long_a, TRUE);
    g_assert_false(pcv_rbac_is_locked(long_a));
    g_free(long_a);
    g_free(long_b);
    pcv_rbac_attempts_reset_for_test(0, NULL);
}

                                                                 
static void
test_ip_table_bounded_and_locked_kept(void)
{
    pcv_rbac_attempts_reset_for_test(3, fake_now);
    guint cards = 99;
    for (int i = 0; i < 16; i++) {
        g_autofree gchar *ip = g_strdup_printf("198.51.100.%d", i);
        g_assert_cmpint(pcv_rbac_get_ip_remaining_lockout(ip), ==, 0);
    }
    pcv_rbac_attempts_ip_stats_for_test(&cards);
    g_assert_cmpuint(cards, ==, 0);                        
    pcv_rbac_ip_record_auth_failure("198.51.100.200");
    pcv_rbac_ip_record_auth_success("198.51.100.200");
    pcv_rbac_attempts_ip_stats_for_test(&cards);
    g_assert_cmpuint(cards, ==, 0);                    

    advance_sec(1);
    for (int i = 0; i < 20; i++)
        pcv_rbac_ip_record_auth_failure("203.0.113.1");              
    g_assert_cmpint(pcv_rbac_get_ip_remaining_lockout("203.0.113.1"), >, 0);
    for (int i = 0; i < 64; i++) {
        g_autofree gchar *ip = g_strdup_printf("198.51.100.%d", i);
        advance_sec(1);
        pcv_rbac_ip_record_auth_failure(ip);
        pcv_rbac_attempts_ip_stats_for_test(&cards);
        g_assert_cmpuint(cards, <=, 3);
    }
    g_assert_cmpint(pcv_rbac_get_ip_remaining_lockout("203.0.113.1"), >, 0);                    

                      
    pcv_rbac_ip_record_auth_success("203.0.113.1");
    g_assert_cmpint(pcv_rbac_get_ip_remaining_lockout("203.0.113.1"), ==, 0);
    pcv_rbac_attempts_reset_for_test(0, NULL);
}

                                                  
static gpointer
hammer(gpointer data)
{
    gint id = GPOINTER_TO_INT(data);
    for (int i = 0; i < 2000; i++) {
        g_autofree gchar *name = g_strdup_printf("t%d-%d", id, i % 300);
        pcv_rbac_attempts_login_record(name, i % 7 == 0);
        (void)pcv_rbac_get_remaining_lockout(name);
        (void)pcv_rbac_is_locked(name);
    }
    return NULL;
}

static void
test_concurrent_stays_under_cap(void)
{
    pcv_rbac_attempts_reset_for_test(64, NULL);
    GThread *t[4];
    for (int i = 0; i < 4; i++)
        t[i] = g_thread_new("attempts", hammer, GINT_TO_POINTER(i));
    for (int i = 0; i < 4; i++)
        g_thread_join(t[i]);
    guint cards = 0;
    gsize bytes = 0;
    pcv_rbac_attempts_login_stats_for_test(&cards, &bytes);
    g_assert_cmpuint(cards, <=, 64);
    pcv_rbac_attempts_reset_for_test(0, NULL);
}

void
test_rbac_login_enum_register(void)
{
    g_test_add_func("/rbac/login_enum/absent_costs_one_pbkdf2_unlocked", test_absent_costs_one_pbkdf2_unlocked);
    g_test_add_func("/rbac/login_enum/existing_rows_cost_same_total", test_existing_rows_cost_same_total);
    g_test_add_func("/rbac/login_enum/race_password_changed_during_hash", test_race_password_changed_during_hash);
    g_test_add_func("/rbac/login_enum/race_rehash_keeps_new_password", test_race_rehash_keeps_new_password);
    g_test_add_func("/rbac/login_enum/race_delete_recreate_during_hash", test_race_delete_recreate_during_hash);
    g_test_add_func("/rbac/login_enum/race_delete_during_hash", test_race_delete_during_hash);
    g_test_add_func("/rbac/login_enum/check_subject_returns_verified_row", test_check_subject_returns_verified_row);
    g_test_add_func("/rbac/login_enum/lockout_sequence_parity", test_lockout_sequence_parity);
    g_test_add_func("/rbac/login_enum/authenticate_records_absent_failure", test_authenticate_records_absent_failure);
    g_test_add_func("/rbac/login_enum/check_and_success_keep_no_card", test_check_and_success_keep_no_card);
    g_test_add_func("/rbac/login_enum/cards_stay_under_cap", test_cards_stay_under_cap);
    g_test_add_func("/rbac/login_enum/evicts_oldest_unlocked", test_evicts_oldest_unlocked);
    g_test_add_func("/rbac/login_enum/locked_card_survives_older", test_locked_card_survives_older);
    g_test_add_func("/rbac/login_enum/all_locked_records_nothing_then_expiry_frees",
                    test_all_locked_records_nothing_then_expiry_frees);
    g_test_add_func("/rbac/login_enum/long_names_bounded_and_distinct", test_long_names_bounded_and_distinct);
    g_test_add_func("/rbac/login_enum/ip_table_bounded_and_locked_kept", test_ip_table_bounded_and_locked_kept);
    g_test_add_func("/rbac/login_enum/concurrent_stays_under_cap", test_concurrent_stays_under_cap);
}
