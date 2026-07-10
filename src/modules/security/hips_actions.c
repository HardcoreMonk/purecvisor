#include "modules/security/hips_actions.h"

#include "modules/auth/pcv_rbac.h"
#include "modules/security/security_policy.h"
#include "modules/security/security_store.h"
#include "utils/pcv_spawn.h"

#include <string.h>

/*
 * HIPS v1 is intentionally narrow: it proposes many responses, but only two
 * actions may execute automatically after admin approval. Everything else stays
 * manual_runbook until a new ADR expands the blast radius.
 */
static GQuark
hips_action_error_quark(void)
{
    return g_quark_from_static_string("pcv-hips-action");
}

gboolean
pcv_hips_action_is_executable(const gchar *action)
{
    /* Keep this allowlist small and explicit. UI badges and RPC validation use it. */
    return g_strcmp0(action, "block_ip") == 0
        || g_strcmp0(action, "revoke_api_key") == 0;
}

static gboolean
validate_ipv4_literal(const gchar *ip)
{
    /*
     * block_ip accepts only a literal IPv4 address. CIDR, hostnames, shell
     * metacharacters, and ranges are rejected before argv construction.
     */
    if (!ip || !*ip || strlen(ip) > 15) {
        return FALSE;
    }

    guint octets = 0;
    const gchar *p = ip;
    while (*p) {
        if (!g_ascii_isdigit(*p)) {
            return FALSE;
        }

        guint value = 0;
        guint digits = 0;
        while (g_ascii_isdigit(*p)) {
            value = (value * 10) + (guint)(*p - '0');
            digits++;
            if (digits > 3 || value > 255) {
                return FALSE;
            }
            p++;
        }
        if (digits == 0) {
            return FALSE;
        }

        octets++;
        if (*p == '.') {
            p++;
            if (!*p) {
                return FALSE;
            }
        } else if (*p != '\0') {
            return FALSE;
        }
    }

    return octets == 4;
}

gboolean
pcv_hips_action_validate_api_key_target(const gchar *client_name)
{
    /*
     * The revoke path targets RBAC client names, not filesystem paths. Rejecting
     * separators and traversal keeps the value inside the RBAC namespace.
     */
    if (!client_name || !*client_name || strlen(client_name) > 128) {
        return FALSE;
    }

    for (const gchar *p = client_name; *p; p++) {
        if (g_ascii_isalnum(*p) || *p == '_' || *p == '-' ||
            *p == '.' || *p == '@') {
            continue;
        }
        return FALSE;
    }
    if (strstr(client_name, "..")) {
        return FALSE;
    }
    return TRUE;
}

gboolean
pcv_hips_action_build_block_ip_argv(const gchar *ip,
                                    const gchar **argv,
                                    gsize argv_len)
{
    if (!validate_ipv4_literal(ip) || !argv || argv_len < 13) {
        return FALSE;
    }

    argv[0] = "nft";
    argv[1] = "add";
    argv[2] = "rule";
    argv[3] = "inet";
    argv[4] = "purecvisor";
    argv[5] = "input";
    argv[6] = "ip";
    argv[7] = "saddr";
    argv[8] = ip;
    argv[9] = "drop";
    argv[10] = "comment";
    argv[11] = "pcv-security-guard";
    argv[12] = NULL;
    return TRUE;
}

static gboolean
spawn_error_is_already_exists(const gchar *text)
{
    return text && (g_strrstr(text, "File exists") ||
                    g_strrstr(text, "already exists") ||
                    g_strrstr(text, "exists"));
}

static gboolean
run_nft_allow_exists(const gchar * const *argv,
                     const gchar *context,
                     GError **error)
{
    /*
     * Chain/table setup is idempotent. nft reports existing objects as errors,
     * but for approval replay safety that condition is a success.
     */
    g_autofree gchar *std_err = NULL;
    GError *local_error = NULL;
    if (pcv_spawn_sync(argv, NULL, &std_err, &local_error)) {
        return TRUE;
    }

    if (spawn_error_is_already_exists(std_err) ||
        (local_error && spawn_error_is_already_exists(local_error->message))) {
        g_clear_error(&local_error);
        return TRUE;
    }

    if (local_error) {
        g_propagate_prefixed_error(error, local_error, "%s: ", context);
    } else {
        g_set_error(error, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED,
                    "%s: %s", context, std_err ? std_err : "unknown error");
    }
    return FALSE;
}

gboolean
pcv_hips_action_ensure_nft_input_chain(GError **error)
{
    const gchar *table_argv[] = {
        "nft", "add", "table", "inet", "purecvisor", NULL
    };
    const gchar *chain_argv[] = {
        "nft", "add", "chain", "inet", "purecvisor", "input",
        "{ type filter hook input priority filter; policy accept; }", NULL
    };

    return run_nft_allow_exists(table_argv, "ensure nft table", error) &&
           run_nft_allow_exists(chain_argv, "ensure nft input chain", error);
}

gboolean
pcv_hips_action_execute(const gchar *action, const gchar *target, GError **error)
{
    if (g_strcmp0(action, "block_ip") == 0) {
        const gchar *argv[16] = {0};
        if (!pcv_hips_action_build_block_ip_argv(target, argv, G_N_ELEMENTS(argv))) {
            g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                        "invalid block_ip target");
            return FALSE;
        }
        if (!pcv_hips_action_ensure_nft_input_chain(error)) {
            return FALSE;
        }
        return pcv_spawn_sync(argv, NULL, NULL, error);
    }

    if (g_strcmp0(action, "revoke_api_key") == 0) {
        if (!pcv_hips_action_validate_api_key_target(target)) {
            g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                        "invalid revoke_api_key target");
            return FALSE;
        }
        return pcv_rbac_apikey_revoke(target, error);
    }

    g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                "action '%s' is manual_runbook only in v1", action ? action : "");
    return FALSE;
}

JsonObject *
pcv_hips_action_build_pending(const PcvSecurityEvent *ev)
{
    if (!ev || !ev->event_id[0]) {
        return NULL;
    }

    const gchar *action = ev->recommended_action[0]
        ? ev->recommended_action
        : pcv_security_policy_recommend_action(ev);
    /*
     * Building a pending object also persists it. The UI must not show an
     * approval button for an action that would disappear on daemon restart.
     */
    GError *error = NULL;
    if (!pcv_security_store_upsert_pending_action(ev, action, 3600, &error)) {
        g_clear_error(&error);
        return NULL;
    }

    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "event_id", ev->event_id);
    json_object_set_string_member(obj, "action", action);
    json_object_set_string_member(obj, "target_kind",
                                  pcv_security_target_kind_to_string(ev->target_kind));
    json_object_set_string_member(obj, "target", ev->target);
    json_object_set_string_member(obj, "status", "pending");
    json_object_set_int_member(obj, "ttl_sec", 3600);
    return obj;
}

JsonArray *
pcv_hips_action_list_pending(void)
{
    return pcv_security_store_list_pending_actions();
}

gboolean
pcv_hips_action_approve(const gchar *event_id, const gchar *admin_user, GError **error)
{
    /*
     * Approval marks intent only. The dispatcher worker executes the side effect
     * first, then calls this function so failed nft/RBAC operations stay pending.
     */
    g_autoptr(JsonObject) action = pcv_security_store_get_action(event_id);
    if (!action) {
        g_set_error(error, hips_action_error_quark(), 1,
                    "pending action not found for event_id=%s",
                    event_id ? event_id : "");
        return FALSE;
    }

    const gchar *name = json_object_get_string_member(action, "action");
    if (!pcv_hips_action_is_executable(name)) {
        g_set_error(error, hips_action_error_quark(), 2,
                    "action '%s' is manual_runbook only in v1", name ? name : "");
        return FALSE;
    }

    return pcv_security_store_update_action_status(event_id, "approved",
                                                   admin_user, "", error);
}

gboolean
pcv_hips_action_dismiss(const gchar *event_id,
                        const gchar *admin_user,
                        const gchar *reason,
                        GError **error)
{
    return pcv_security_store_update_action_status(event_id, "dismissed",
                                                   admin_user, reason, error);
}
