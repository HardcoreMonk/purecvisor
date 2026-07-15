#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include "modules/security/hips_actions.h"
#include "modules/security/security_store.h"

/*
 * HIPS tests guard the v1 safety boundary: only block_ip and revoke_api_key are
 * executable, pending approvals survive restart, and unsafe targets never build
 * argv for pcv_spawn_sync().
 */
static void
cleanup_security_db(const gchar *path)
{
    gchar *wal = g_strdup_printf("%s-wal", path);
    gchar *shm = g_strdup_printf("%s-shm", path);
    g_unlink(path);
    g_unlink(wal);
    g_unlink(shm);
    g_free(wal);
    g_free(shm);
}

static void
test_hips_action_executable_allowlist(void)
{
    g_assert_true(pcv_hips_action_is_executable("block_ip"));
    g_assert_true(pcv_hips_action_is_executable("revoke_api_key"));
    g_assert_false(pcv_hips_action_is_executable("lock_user"));
    g_assert_false(pcv_hips_action_is_executable("restart_service"));
    g_assert_false(pcv_hips_action_is_executable("quarantine_process"));
    g_assert_false(pcv_hips_action_is_executable("restore_config"));
}

static void
test_hips_action_pending_persistence(void)
{
    /* Pending action state must survive daemon restart before an admin decides. */
    gchar *path = g_strdup_printf("%s/pcv-hips-test-%u.db",
                                  g_get_tmp_dir(), g_random_int());
    cleanup_security_db(path);
    g_assert_true(pcv_security_store_open(path));

    PcvSecurityEvent ev = {0};
    g_strlcpy(ev.event_id, "sec-action-1", sizeof ev.event_id);
    ev.type = PCV_SECURITY_EVENT_AUTH_BRUTEFORCE;
    ev.target_kind = PCV_SECURITY_TARGET_IP;
    g_strlcpy(ev.target, "192.0.2.77", sizeof ev.target);
    g_strlcpy(ev.recommended_action, "block_ip", sizeof ev.recommended_action);
    ev.status = PCV_SECURITY_STATUS_ACTION_PENDING;

    JsonObject *pending = pcv_hips_action_build_pending(&ev);
    g_assert_nonnull(pending);
    g_assert_cmpstr(json_object_get_string_member(pending, "event_id"), ==, "sec-action-1");
    json_object_unref(pending);
    pcv_security_store_close();

    g_assert_true(pcv_security_store_open(path));
    JsonArray *list = pcv_hips_action_list_pending();
    g_assert_nonnull(list);
    g_assert_cmpuint(json_array_get_length(list), ==, 1);
    JsonObject *item = json_array_get_object_element(list, 0);
    g_assert_cmpstr(json_object_get_string_member(item, "event_id"), ==, "sec-action-1");
    g_assert_cmpstr(json_object_get_string_member(item, "action"), ==, "block_ip");
    g_assert_cmpstr(json_object_get_string_member(item, "target"), ==, "192.0.2.77");
    g_assert_cmpstr(json_object_get_string_member(item, "status"), ==, "pending");
    json_array_unref(list);

    pcv_security_store_close();
    cleanup_security_db(path);
    g_free(path);
}

static void
test_security_action_rejects_manual_runbook(void)
{
    GError *error = NULL;
    g_assert_false(pcv_hips_action_execute("lock_user", "alice", &error));
    g_assert_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE);
    g_clear_error(&error);
}

static void
test_security_action_block_ip_builds_argv(void)
{
    const gchar *argv[16] = {0};
    g_assert_true(pcv_hips_action_build_block_ip_argv("192.0.2.10",
                                                      argv, G_N_ELEMENTS(argv)));
    g_assert_cmpstr(argv[0], ==, "nft");
    g_assert_cmpstr(argv[1], ==, "add");
    g_assert_cmpstr(argv[4], ==, "purecvisor");
    g_assert_cmpstr(argv[5], ==, "input");
}

static void
test_security_action_revoke_api_key_uses_client_name(void)
{
    g_assert_true(pcv_hips_action_validate_api_key_target("grafana-scraper"));
    g_assert_false(pcv_hips_action_validate_api_key_target("../bad"));
}

static void
test_security_action_block_ip_rejects_invalid_targets(void)
{
    const gchar *argv[16] = {0};
    g_assert_false(pcv_hips_action_build_block_ip_argv("192.0.2.10/32",
                                                       argv, G_N_ELEMENTS(argv)));
    g_assert_false(pcv_hips_action_build_block_ip_argv("192.0.2.10;reboot",
                                                       argv, G_N_ELEMENTS(argv)));
    g_assert_false(pcv_hips_action_build_block_ip_argv("",
                                                       argv, G_N_ELEMENTS(argv)));
}

/* SEC-4: 만료된 pending 승인이 부작용(nft/키폐기)을 실행하기 전에 차단되는지 검증한다.
 * spy_execute가 실제 nft/RBAC를 대체하므로 root 불필요 — execute_fn 주입 seam으로
 * 만료(=0회 호출)와 비만료(=1회 호출)를 결정적으로 구분한다. */
static gint g_spy_execute_calls;

static gboolean
spy_execute(const gchar *action, const gchar *target, GError **error)
{
    (void)action;
    (void)target;
    (void)error;
    g_spy_execute_calls++;
    return TRUE;
}

/* test-only 클록 시드 (헤더 미선언; set_retention_cap_for_test 관례로 forward-decl). */
void pcv_security_store_set_now_for_test(gint64 t);

static void
test_hips_expired_pending_blocks_side_effect(void)
{
    gchar *path = g_strdup_printf("%s/pcv-hips-exp-%u.db",
                                  g_get_tmp_dir(), g_random_int());
    cleanup_security_db(path);
    g_assert_true(pcv_security_store_open(path));

    PcvSecurityEvent ev = {0};
    g_strlcpy(ev.event_id, "sec-exp-1", sizeof ev.event_id);
    ev.type = PCV_SECURITY_EVENT_AUTH_BRUTEFORCE;
    ev.target_kind = PCV_SECURITY_TARGET_IP;
    g_strlcpy(ev.target, "192.0.2.99", sizeof ev.target);

    /* 대조군: 비만료(now < expires_at) → execute_fn 호출 + approve 경로 온전. */
    pcv_security_store_set_now_for_test(1000);
    g_assert_true(pcv_security_store_upsert_pending_action(&ev, "block_ip", 3600, NULL)); /* expires_at=4600 */
    pcv_security_store_set_now_for_test(2000);
    g_spy_execute_calls = 0;
    GError *err = NULL;
    g_assert_true(pcv_hips_action_run_approval("sec-exp-1", "block_ip", "192.0.2.99",
                                               "admin", spy_execute, &err));
    g_assert_no_error(err);
    g_assert_cmpint(g_spy_execute_calls, ==, 1);

    /* SEC-4: 재-arm 후 만료 경계 초과(now > expires_at) → 부작용 미실행 + pending 잔존. */
    pcv_security_store_set_now_for_test(1000);
    g_assert_true(pcv_security_store_upsert_pending_action(&ev, "block_ip", 3600, NULL)); /* expires_at=4600 */
    pcv_security_store_set_now_for_test(5000);
    g_spy_execute_calls = 0;
    g_assert_false(pcv_hips_action_run_approval("sec-exp-1", "block_ip", "192.0.2.99",
                                                "admin", spy_execute, &err));
    g_assert_cmpint(g_spy_execute_calls, ==, 0);
    g_clear_error(&err);

    JsonObject *action = pcv_security_store_get_action("sec-exp-1");
    g_assert_nonnull(action);
    g_assert_cmpstr(json_object_get_string_member(action, "status"), ==, "pending");
    json_object_unref(action);

    pcv_security_store_set_now_for_test(0); /* 실클록 복원 */
    pcv_security_store_close();
    cleanup_security_db(path);
    g_free(path);
}

void
test_security_actions_register(void)
{
    g_test_add_func("/security/actions/executable-allowlist",
                    test_hips_action_executable_allowlist);
    g_test_add_func("/security/actions/pending-persistence",
                    test_hips_action_pending_persistence);
    g_test_add_func("/security/actions/rejects-manual-runbook",
                    test_security_action_rejects_manual_runbook);
    g_test_add_func("/security/actions/block-ip-builds-argv",
                    test_security_action_block_ip_builds_argv);
    g_test_add_func("/security/actions/revoke-api-key-target",
                    test_security_action_revoke_api_key_uses_client_name);
    g_test_add_func("/security/actions/block-ip-rejects-invalid-targets",
                    test_security_action_block_ip_rejects_invalid_targets);
    g_test_add_func("/security_actions/hips_expired_blocks_side_effect",
                    test_hips_expired_pending_blocks_side_effect);
}
