                                                                                     
                                                                                                          
                                                                       
                                                                         
                              
   
                         
                                                                
   
#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>

#include "modules/network/vpc/vpc_model.h"
#include "modules/network/vpc/vpc_store.h"

typedef struct {
    gchar *dir;
    gchar *db_path;
    PcvVpcStore *store;
} StoreFixture;

static gint64
vpc_revision(PcvVpcStore *store, const gchar *id, const gchar *tenant)
{
    g_autoptr(JsonObject) vpc = pcv_vpc_store_get_vpc(store, id, tenant, NULL);
    g_assert_nonnull(vpc);
    return json_object_get_int_member(vpc, "revision");
}

static void
fixture_setup(StoreFixture *f, gconstpointer unused)
{
    (void)unused;
    f->dir = g_dir_make_tmp("pcv-vpc-store-XXXXXX", NULL);
    g_assert_nonnull(f->dir);
    f->db_path = g_build_filename(f->dir, "vpc.db", NULL);
    f->store = pcv_vpc_store_open(f->db_path, NULL);
    g_assert_nonnull(f->store);
}

static void
fixture_teardown(StoreFixture *f, gconstpointer unused)
{
    (void)unused;
    pcv_vpc_store_free(f->store);
    g_autofree gchar *wal = g_strdup_printf("%s-wal", f->db_path);
    g_autofree gchar *shm = g_strdup_printf("%s-shm", f->db_path);
    g_remove(wal); g_remove(shm); g_remove(f->db_path); g_rmdir(f->dir);
    g_free(f->db_path); g_free(f->dir);
}

static void
test_store_tenant_list_and_revision(StoreFixture *f, gconstpointer unused)
{
    (void)unused;
    g_autofree gchar *a = NULL, *b = NULL;
    gint64 revision = 0;
    g_assert_true(pcv_vpc_store_create_vpc(f->store, "prod", "tenant-a", "nat", "linux",
                                           &a, &revision, NULL));
    g_assert_cmpint(revision, ==, 1);
    g_assert_true(pcv_vpc_store_create_vpc(f->store, "prod", "tenant-b", "isolated", "linux",
                                           &b, NULL, NULL));
    g_autoptr(JsonArray) own = pcv_vpc_store_list_vpcs(f->store, "tenant-a", NULL);
    g_assert_cmpuint(json_array_get_length(own), ==, 1);
    g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(own, 0), "id"), ==, a);

    g_assert_true(pcv_vpc_store_set_egress(f->store, a, "tenant-a", "isolated", 1,
                                           &revision, NULL));
    g_assert_cmpint(revision, ==, 2);
    g_autoptr(GError) error = NULL;
    g_assert_false(pcv_vpc_store_set_egress(f->store, a, "tenant-a", "nat", 1,
                                            NULL, &error));
    g_assert_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_STALE_REVISION);
}

static void
test_store_global_overlap_and_managed_bridge(StoreFixture *f, gconstpointer unused)
{
    (void)unused;
    g_autofree gchar *a = NULL, *b = NULL, *subnet = NULL, *bridge = NULL;
    g_assert_true(pcv_vpc_store_create_vpc(f->store, "a", "tenant-a", "nat", "linux", &a, NULL, NULL));
    g_assert_true(pcv_vpc_store_create_vpc(f->store, "b", "tenant-b", "nat", "linux", &b, NULL, NULL));
    g_assert_true(pcv_vpc_store_create_subnet(f->store, a, "tenant-a", "web",
                                              "10.44.0.0/24", 1500, 1,
                                              &subnet, &bridge, NULL, NULL));
    g_assert_true(pcv_vpc_store_bridge_is_managed(f->store, bridge));
    g_assert_false(pcv_vpc_store_bridge_is_managed(f->store, "pcvnat0"));
    g_autoptr(GError) error = NULL;
    g_assert_false(pcv_vpc_store_create_subnet(f->store, b, "tenant-b", "overlap",
                                               "10.44.0.128/25", 1500, 1,
                                               NULL, NULL, NULL, &error));
    g_assert_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT);
}

static void
test_store_attachment_publish_reference_guards(StoreFixture *f, gconstpointer unused)
{
    (void)unused;
    g_autofree gchar *vpc = NULL, *subnet = NULL, *publish = NULL;
    g_assert_true(pcv_vpc_store_create_vpc(f->store, "a", "tenant-a", "nat", "linux",
                                           &vpc, NULL, NULL));
    g_assert_true(pcv_vpc_store_create_subnet(f->store, vpc, "tenant-a", "web",
                                              "10.55.0.0/24", 1500, 1,
                                              &subnet, NULL, NULL, NULL));
    g_assert_cmpint(vpc_revision(f->store, vpc, "tenant-a"), ==, 2);
    g_autoptr(JsonObject) attachment = NULL;
    g_assert_true(pcv_vpc_store_allocate_attachment(
        f->store, subnet, "tenant-a", "11111111-1111-1111-1111-111111111111",
        "web-vm", "alice", NULL, &attachment, NULL));
    g_assert_cmpint(vpc_revision(f->store, vpc, "tenant-a"), ==, 3);
    const gchar *attachment_id = json_object_get_string_member(attachment, "id");
    const gchar *mac_address = json_object_get_string_member(attachment, "mac_address");
    g_assert_cmpstr(json_object_get_string_member(attachment, "ip_address"), ==, "10.55.0.2");
    g_assert_true(pcv_vpc_store_mac_is_managed(f->store, mac_address));
    g_assert_false(pcv_vpc_store_mac_is_managed(f->store, "02:00:00:00:00:ff"));
    g_assert_true(pcv_vpc_store_vm_is_attached(f->store, "web-vm"));
    g_assert_true(pcv_vpc_store_vm_is_attached(
        f->store, "11111111-1111-1111-1111-111111111111"));
    g_assert_true(pcv_vpc_store_set_resource_state(f->store, "attachments", attachment_id,
                                                   "ACTIVE", NULL, NULL));
    g_autoptr(GPtrArray) sources = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(sources, g_strdup("192.0.2.0/24"));
    g_assert_true(pcv_vpc_store_create_publish(f->store, attachment_id, "tenant-a", "tcp",
                                               "0.0.0.0", 8443, 443, sources,
                                               &publish, NULL));
    g_assert_cmpint(vpc_revision(f->store, vpc, "tenant-a"), ==, 4);
    g_assert_true(pcv_vpc_store_set_resource_state(f->store, "service_publishes", publish,
                                                   "ACTIVE", NULL, NULL));
    g_assert_true(pcv_vpc_store_vm_has_publish(f->store, "web-vm"));
    g_autoptr(GError) wildcard_error = NULL;
    g_assert_false(pcv_vpc_store_create_publish(
        f->store, attachment_id, "tenant-a", "tcp", "192.0.2.10", 8443, 444,
        sources, NULL, &wildcard_error));
    g_assert_error(wildcard_error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT);
    g_autoptr(PcvVpcPolicySnapshot) active_snapshot =
        pcv_vpc_store_policy_snapshot(f->store, NULL);
    g_assert_cmpuint(active_snapshot->publishes->len, ==, 1);
    g_assert_true(pcv_vpc_store_set_resource_state(f->store, "attachments", attachment_id,
                                                   "QUARANTINED", "test", NULL));
    g_assert_cmpint(vpc_revision(f->store, vpc, "tenant-a"), ==, 5);
    g_clear_pointer(&active_snapshot, pcv_vpc_policy_snapshot_free);
    active_snapshot = pcv_vpc_store_policy_snapshot(f->store, NULL);
    g_assert_cmpuint(active_snapshot->publishes->len, ==, 0);
    g_assert_true(pcv_vpc_store_set_resource_state(f->store, "attachments", attachment_id,
                                                   "ACTIVE", NULL, NULL));
    g_assert_cmpint(vpc_revision(f->store, vpc, "tenant-a"), ==, 6);

    g_autoptr(GError) error = NULL;
    g_assert_false(pcv_vpc_store_delete_attachment(f->store, attachment_id, "tenant-a", &error));
    g_assert_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT);
    g_clear_error(&error);
    g_assert_true(pcv_vpc_store_delete_publish(f->store, publish, "tenant-a", NULL));
    g_assert_cmpint(vpc_revision(f->store, vpc, "tenant-a"), ==, 7);
    g_assert_false(pcv_vpc_store_vm_has_publish(f->store, "web-vm"));
    g_assert_true(pcv_vpc_store_delete_attachment(f->store, attachment_id, "tenant-a", NULL));
    g_assert_cmpint(vpc_revision(f->store, vpc, "tenant-a"), ==, 8);
    g_assert_false(pcv_vpc_store_mac_is_managed(f->store, mac_address));
    g_assert_false(pcv_vpc_store_vm_is_attached(f->store, "web-vm"));
    g_assert_true(pcv_vpc_store_delete_subnet(f->store, subnet, "tenant-a", NULL));
    g_assert_cmpint(vpc_revision(f->store, vpc, "tenant-a"), ==, 9);
    g_assert_true(pcv_vpc_store_delete_vpc(f->store, vpc, "tenant-a", NULL));
}

static void
test_store_rejects_future_schema_without_rewrite(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("pcv-vpc-version-XXXXXX", NULL);
    g_assert_nonnull(dir);
    g_autofree gchar *path = g_build_filename(dir, "vpc.db", NULL);
    sqlite3 *db = NULL;
    g_assert_cmpint(sqlite3_open(path, &db), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(db, "PRAGMA user_version=99", NULL, NULL, NULL), ==, SQLITE_OK);
    sqlite3_close(db);

    g_autoptr(GError) error = NULL;
    PcvVpcStore *store = pcv_vpc_store_open(path, &error);
    g_assert_null(store);
    g_assert_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_STATE);

    db = NULL;
    g_assert_cmpint(sqlite3_open(path, &db), ==, SQLITE_OK);
    sqlite3_stmt *stmt = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, NULL), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_ROW);
    g_assert_cmpint(sqlite3_column_int(stmt, 0), ==, 99);
    sqlite3_finalize(stmt); sqlite3_close(db);
    g_remove(path); g_rmdir(dir);
}

static void
test_store_policy_snapshot_uses_only_active_edges(StoreFixture *f, gconstpointer unused)
{
    (void)unused;
    g_autofree gchar *vpc = NULL, *subnet = NULL;
    g_assert_true(pcv_vpc_store_create_vpc(f->store, "a", "tenant-a", "nat", "linux", &vpc, NULL, NULL));
    g_assert_true(pcv_vpc_store_create_subnet(f->store, vpc, "tenant-a", "web",
                                              "10.66.0.0/24", 1500, 1,
                                              &subnet, NULL, NULL, NULL));
    g_autoptr(JsonObject) attachment = NULL;
    g_assert_true(pcv_vpc_store_allocate_attachment(
        f->store, subnet, "tenant-a", "22222222-2222-2222-2222-222222222222",
        "web-vm", "alice", NULL, &attachment, NULL));
    g_autoptr(PcvVpcPolicySnapshot) snapshot = pcv_vpc_store_policy_snapshot(f->store, NULL);
    g_assert_cmpuint(snapshot->vpcs->len, ==, 1);
    PcvVpcPolicyVpc *pv = g_ptr_array_index(snapshot->vpcs, 0);
    g_assert_cmpuint(pv->subnets->len, ==, 1);
    PcvVpcPolicySubnet *ps = g_ptr_array_index(pv->subnets, 0);
    g_assert_cmpuint(ps->attachments->len, ==, 0);
    g_assert_true(pcv_vpc_store_set_resource_state(
        f->store, "attachments", json_object_get_string_member(attachment, "id"),
        "ACTIVE", NULL, NULL));
    g_clear_pointer(&snapshot, pcv_vpc_policy_snapshot_free);
    snapshot = pcv_vpc_store_policy_snapshot(f->store, NULL);
    pv = g_ptr_array_index(snapshot->vpcs, 0);
    ps = g_ptr_array_index(pv->subnets, 0);
    g_assert_cmpuint(ps->attachments->len, ==, 1);
}

static void
test_store_ovn_binding_and_backend_neutral_subnet(StoreFixture *f, gconstpointer unused)
{
    (void)unused;
    g_autofree gchar *first = NULL;
    g_autofree gchar *second = NULL;
    g_assert_true(pcv_vpc_store_create_vpc(
        f->store, "ovn-a", "tenant-a", "nat", "ovn", &first, NULL, NULL));
    g_assert_true(pcv_vpc_store_create_vpc(
        f->store, "ovn-b", "tenant-a", "isolated", "ovn", &second, NULL, NULL));
    g_autoptr(JsonObject) first_binding = pcv_vpc_store_ensure_ovn_binding(
        f->store, first, "tenant-a", "100.64.0.0/28", NULL);
    g_autoptr(JsonObject) second_binding = pcv_vpc_store_ensure_ovn_binding(
        f->store, second, "tenant-a", "100.64.0.0/28", NULL);
    g_assert_nonnull(first_binding);
    g_assert_nonnull(second_binding);
    g_assert_cmpstr(json_object_get_string_member(first_binding, "edge_cidr"), ==,
                    "100.64.0.0/30");
    g_assert_cmpstr(json_object_get_string_member(first_binding, "host_edge_ip"), ==,
                    "100.64.0.1");
    g_assert_cmpstr(json_object_get_string_member(first_binding, "router_edge_ip"), ==,
                    "100.64.0.2");
    g_assert_cmpstr(json_object_get_string_member(second_binding, "edge_cidr"), ==,
                    "100.64.0.4/30");

    g_autofree gchar *subnet_id = NULL;
    g_autofree gchar *backend_ref = NULL;
    g_assert_true(pcv_vpc_store_create_subnet(
        f->store, first, "tenant-a", "web", "10.77.0.0/24", 1450, 1,
        &subnet_id, &backend_ref, NULL, NULL));
    g_autoptr(JsonObject) subnet = pcv_vpc_store_get_subnet(
        f->store, subnet_id, "tenant-a", NULL);
    g_assert_cmpstr(json_object_get_string_member(subnet, "backend"), ==, "ovn");
    g_assert_cmpstr(json_object_get_string_member(subnet, "backend_ref"), ==, backend_ref);
    g_assert_true(json_object_get_null_member(subnet, "bridge_name"));
    g_assert_true(g_str_has_prefix(backend_ref, "pcvv_ls_"));

    g_autoptr(GPtrArray) managed = pcv_vpc_store_list_managed_bridges(f->store, NULL);
    g_assert_cmpuint(managed->len, ==, 2);
    for (guint i = 0; i < managed->len; i++)
        g_assert_true(g_str_has_prefix(g_ptr_array_index(managed, i), "pcve"));

    g_autoptr(PcvVpcPolicySnapshot) snapshot = pcv_vpc_store_policy_snapshot(f->store, NULL);
    PcvVpcPolicyVpc *policy_vpc = NULL;
    for (guint i = 0; i < snapshot->vpcs->len; i++) {
        PcvVpcPolicyVpc *candidate = g_ptr_array_index(snapshot->vpcs, i);
        if (g_strcmp0(candidate->id, first) == 0) {
            policy_vpc = candidate;
            break;
        }
    }
    g_assert_nonnull(policy_vpc);
    g_assert_cmpstr(policy_vpc->backend, ==, "ovn");
    g_assert_true(g_str_has_prefix(policy_vpc->edge_interface, "pcve"));
    g_assert_cmpuint(policy_vpc->subnets->len, ==, 1);
    PcvVpcPolicySubnet *policy_subnet = g_ptr_array_index(policy_vpc->subnets, 0);
    g_assert_cmpstr(policy_subnet->backend, ==, "ovn");
    g_assert_null(policy_subnet->bridge_name);
}

static void
test_store_migrates_v1_linux_rows_atomically(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("pcv-vpc-migrate-XXXXXX", NULL);
    g_assert_nonnull(dir);
    g_autofree gchar *path = g_build_filename(dir, "vpc.db", NULL);
    sqlite3 *db = NULL;
    g_assert_cmpint(sqlite3_open(path, &db), ==, SQLITE_OK);
    const gchar *v1_sql =
        "PRAGMA foreign_keys=ON;"
        "CREATE TABLE vpcs(id TEXT PRIMARY KEY,name TEXT NOT NULL,tenant TEXT NOT NULL,"
        "egress_mode TEXT NOT NULL,state TEXT NOT NULL,revision INTEGER NOT NULL DEFAULT 1,"
        "last_error TEXT,created_at INTEGER NOT NULL DEFAULT(unixepoch()),"
        "updated_at INTEGER NOT NULL DEFAULT(unixepoch()),UNIQUE(tenant,name));"
        "CREATE TABLE subnets(id TEXT PRIMARY KEY,vpc_id TEXT NOT NULL REFERENCES vpcs(id) ON DELETE RESTRICT,"
        "name TEXT NOT NULL,cidr TEXT NOT NULL,network_start INTEGER NOT NULL,network_end INTEGER NOT NULL,"
        "prefix INTEGER NOT NULL,gateway TEXT NOT NULL,allocation_start TEXT NOT NULL,allocation_end TEXT NOT NULL,"
        "dhcp_enabled INTEGER NOT NULL DEFAULT 1,mtu INTEGER NOT NULL,bridge_name TEXT NOT NULL UNIQUE,"
        "state TEXT NOT NULL,last_error TEXT,created_at INTEGER NOT NULL DEFAULT(unixepoch()),"
        "updated_at INTEGER NOT NULL DEFAULT(unixepoch()),UNIQUE(vpc_id,name),UNIQUE(cidr));"
        "CREATE INDEX idx_vpc_subnets_range ON subnets(network_start,network_end);"
        "CREATE TABLE attachments(id TEXT PRIMARY KEY,subnet_id TEXT NOT NULL REFERENCES subnets(id) ON DELETE RESTRICT,"
        "vm_uuid TEXT NOT NULL,vm_name_snapshot TEXT NOT NULL,owner_subject TEXT NOT NULL,"
        "mac_address TEXT NOT NULL UNIQUE,ip_address TEXT NOT NULL UNIQUE,state TEXT NOT NULL,last_error TEXT,"
        "created_at INTEGER NOT NULL DEFAULT(unixepoch()),updated_at INTEGER NOT NULL DEFAULT(unixepoch()),"
        "UNIQUE(subnet_id,vm_uuid));"
        "CREATE TABLE service_publishes(id TEXT PRIMARY KEY,vpc_id TEXT NOT NULL REFERENCES vpcs(id) ON DELETE RESTRICT,"
        "attachment_id TEXT NOT NULL REFERENCES attachments(id) ON DELETE RESTRICT,protocol TEXT NOT NULL,"
        "listen_address TEXT NOT NULL,listen_port INTEGER NOT NULL,target_port INTEGER NOT NULL,"
        "allowed_sources TEXT NOT NULL,state TEXT NOT NULL,last_error TEXT,"
        "created_at INTEGER NOT NULL DEFAULT(unixepoch()),updated_at INTEGER NOT NULL DEFAULT(unixepoch()),"
        "UNIQUE(protocol,listen_address,listen_port));"
        "INSERT INTO vpcs(id,name,tenant,egress_mode,state,revision) VALUES('v1','legacy','acme','nat','ACTIVE',4);"
        "INSERT INTO subnets(id,vpc_id,name,cidr,network_start,network_end,prefix,gateway,allocation_start,"
        "allocation_end,mtu,bridge_name,state) VALUES('s1','v1','web','10.88.0.0/24',173539328,"
        "173539583,24,'10.88.0.1','10.88.0.2','10.88.0.254',1500,'pcvslegacy','ACTIVE');"
        "INSERT INTO attachments(id,subnet_id,vm_uuid,vm_name_snapshot,owner_subject,mac_address,ip_address,state)"
        "VALUES('a1','s1','11111111-1111-1111-1111-111111111111','legacy-vm','alice',"
        "'02:00:00:00:00:11','10.88.0.2','ACTIVE');"
        "PRAGMA user_version=1;";
    g_assert_cmpint(sqlite3_exec(db, v1_sql, NULL, NULL, NULL), ==, SQLITE_OK);
    sqlite3_close(db);

    g_autoptr(GError) error = NULL;
    PcvVpcStore *store = pcv_vpc_store_open(path, &error);
    g_assert_no_error(error);
    g_assert_nonnull(store);
    g_autoptr(JsonObject) vpc = pcv_vpc_store_get_vpc(store, "v1", "acme", NULL);
    g_autoptr(JsonObject) subnet = pcv_vpc_store_get_subnet(store, "s1", "acme", NULL);
    g_autoptr(JsonObject) attachment = pcv_vpc_store_get_attachment(store, "a1", "acme", NULL);
    g_assert_cmpstr(json_object_get_string_member(vpc, "backend"), ==, "linux");
    g_assert_cmpstr(json_object_get_string_member(subnet, "backend_ref"), ==, "pcvslegacy");
    g_assert_cmpstr(json_object_get_string_member(subnet, "bridge_name"), ==, "pcvslegacy");
    g_assert_cmpstr(json_object_get_string_member(attachment, "backend"), ==, "linux");
    pcv_vpc_store_free(store);

    db = NULL;
    g_assert_cmpint(sqlite3_open(path, &db), ==, SQLITE_OK);
    sqlite3_stmt *stmt = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, NULL), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_ROW);
    g_assert_cmpint(sqlite3_column_int(stmt, 0), ==, 2);
    sqlite3_finalize(stmt);
    stmt = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(db, "PRAGMA foreign_key_check", -1, &stmt, NULL), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_DONE);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    g_remove(path);
    g_rmdir(dir);
}

void
test_vpc_store_register(void)
{
    g_test_add("/vpc/store/tenant_revision", StoreFixture, NULL,
               fixture_setup, test_store_tenant_list_and_revision, fixture_teardown);
    g_test_add("/vpc/store/global_overlap", StoreFixture, NULL,
               fixture_setup, test_store_global_overlap_and_managed_bridge, fixture_teardown);
    g_test_add("/vpc/store/reference_guards", StoreFixture, NULL,
               fixture_setup, test_store_attachment_publish_reference_guards, fixture_teardown);
    g_test_add("/vpc/store/policy_snapshot", StoreFixture, NULL,
               fixture_setup, test_store_policy_snapshot_uses_only_active_edges, fixture_teardown);
    g_test_add("/vpc/store/ovn_binding", StoreFixture, NULL,
               fixture_setup, test_store_ovn_binding_and_backend_neutral_subnet, fixture_teardown);
    g_test_add_func("/vpc/store/migrate_v1", test_store_migrates_v1_linux_rows_atomically);
    g_test_add_func("/vpc/store/future_schema", test_store_rejects_future_schema_without_rewrite);
}
