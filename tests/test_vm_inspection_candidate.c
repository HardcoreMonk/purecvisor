                                      
                           
                                                                   
                                                                     
                                                              
                                                    
                                                  
   
#include "src/modules/virt/vm_inspection.h"
#include "src/modules/dispatcher/handler_vm_inspect.h"
#include "src/modules/dispatcher/rpc_utils.h"
#include "src/api/drain.h"
#include "src/modules/dispatcher/rpc_completion.h"
#include <libvirt/virterror.h>
#include <string.h>

typedef struct {
    virConnectPtr conn;
    gchar uuid[VIR_UUID_STRING_BUFLEN];
    gboolean real;
    gchar *xml;
    const gchar *after;
    gint lookup_error;
    gboolean wrong_uuid;
    gint persistent;
    gint active;
    gint next_active;
    gint state;
    gint next_state;
    guint lookup_calls, xml_calls, active_calls, state_calls, block_calls;
    gint xml_failure;
    gboolean block_failure;
    unsigned long long capacity, allocation;
    guint delay_ms;
    gboolean pool_failure;
    gint acquire_count, release_count;
    gchar *response;
    guint responses;
    guint audits;
    gint audit_code;
    gchar audit_subject[128], audit_target[64], audit_result[16];
} Fixture;

static Fixture f;
static const gchar *disk_xml = "<disk device='disk' type='file'><driver type='qcow2'/>"
    "<source file='/private/fixture.qcow2'/><target dev='vda'/><boot order='1'/></disk>";

                                                        
                                                                   
                                                                 
#if defined(__SANITIZE_ADDRESS__)
const char *__lsan_default_suppressions(void)
{
    return "leak:virClassNew\n";
}
#endif

                                                                                           
virDomainPtr __real_virDomainLookupByUUIDString(virConnectPtr, const char *);
char *__real_virDomainGetXMLDesc(virDomainPtr, unsigned int);
int __real_virDomainIsActive(virDomainPtr);
int __real_virDomainIsPersistent(virDomainPtr);
int __real_virDomainGetState(virDomainPtr, int *, int *, unsigned int);
int __real_virDomainGetBlockInfo(virDomainPtr, const char *, virDomainBlockInfoPtr, unsigned int);
int __real_virDomainGetUUIDString(virDomainPtr, char *);
int __real_virGetLastErrorCode(void);

virDomainPtr __wrap_virDomainLookupByUUIDString(virConnectPtr conn, const char *uuid)
{
    f.lookup_calls++;
    if (f.lookup_error) return NULL;
    return __real_virDomainLookupByUUIDString(conn, uuid);
}

int __wrap_virGetLastErrorCode(void)
{
    return f.lookup_error ? f.lookup_error : __real_virGetLastErrorCode();
}

int __wrap_virDomainGetUUIDString(virDomainPtr domain, char *uuid)
{
    if (f.wrong_uuid) {
        g_strlcpy(uuid, "00000000-0000-0000-0000-000000000099", VIR_UUID_STRING_BUFLEN);
        return 0;
    }
    return __real_virDomainGetUUIDString(domain, uuid);
}

char *__wrap_virDomainGetXMLDesc(virDomainPtr domain, unsigned int flags)
{
    g_assert_cmpuint(flags, ==, VIR_DOMAIN_XML_INACTIVE);
    f.xml_calls++;
    if (f.real) return __real_virDomainGetXMLDesc(domain, flags);
    if (f.delay_ms && f.xml_calls == 1) g_usleep(f.delay_ms * 1000);
    if ((gint)f.xml_calls == f.xml_failure) return NULL;
    return g_strdup(f.xml_calls > 1 && f.after ? f.after : f.xml);
}

int __wrap_virDomainIsPersistent(virDomainPtr domain)
{
    return f.real ? __real_virDomainIsPersistent(domain) : f.persistent;
}

int __wrap_virDomainIsActive(virDomainPtr domain)
{
    f.active_calls++;
    return f.real ? __real_virDomainIsActive(domain) :
        f.active_calls == 1 ? f.active : f.next_active;
}

int __wrap_virDomainGetState(virDomainPtr domain, int *state, int *reason, unsigned int flags)
{
    f.state_calls++;
    if (f.real) return __real_virDomainGetState(domain, state, reason, flags);
    *state = f.state_calls == 1 ? f.state : f.next_state;
    *reason = 0;
    return *state == -1 ? -1 : 0;
}

int __wrap_virDomainGetBlockInfo(virDomainPtr domain, const char *target,
                                virDomainBlockInfoPtr info, unsigned int flags)
{
    f.block_calls++;
    g_assert_null(strchr(target, '/'));
    g_assert_cmpuint(flags, ==, 0);
    if (f.real) return __real_virDomainGetBlockInfo(domain, target, info, flags);
    if (f.block_failure) return -1;
    info->capacity = f.capacity;
    info->allocation = f.allocation;
    return 0;
}

                                                               
virConnectPtr virt_conn_pool_acquire(void)
{
    g_atomic_int_inc(&f.acquire_count);
    if (f.pool_failure) return NULL;
    g_assert_cmpint(virConnectRef(f.conn), ==, 0);
    return f.conn;
}

void virt_conn_pool_release(virConnectPtr conn)
{
    g_atomic_int_inc(&f.release_count);
    virConnectClose(conn);
}

void pure_uds_server_send_response(UdsServer *server, GSocketConnection *connection,
                                  const gchar *response)
{
    g_assert_nonnull(server);
    g_assert_nonnull(connection);
    f.responses++;
    g_free(f.response);
    f.response = g_strdup(response);
}

                                                               
                                                      
void pcv_audit_log(const gchar *subject, const gchar *method, const gchar *target,
                  const gchar *result, gint code, gint64 duration, const gchar *source)
{
    g_assert_cmpstr(method, ==, "vm.inspect");
    g_assert_cmpstr(source, ==, "local");
    g_assert_cmpint(duration, >=, 0);
    f.audits++;
    f.audit_code = code;
    g_strlcpy(f.audit_subject, subject ? subject : "", sizeof(f.audit_subject));
    g_strlcpy(f.audit_target, target ? target : "", sizeof(f.audit_target));
    g_strlcpy(f.audit_result, result, sizeof(f.audit_result));
}

static void set_xml(const gchar *os, const gchar *devices)
{
    g_free(f.xml);
    f.xml = g_strdup_printf("<domain type='test'><uuid>%s</uuid>%s<devices>%s</devices></domain>",
                            f.uuid, os, devices);
}

static void reset(void)
{
    g_assert_cmpint(pcv_drain_get_work(), ==, 0);
    if (f.conn) virConnectClose(f.conn);
    g_free(f.xml); g_free(f.response);
    memset(&f, 0, sizeof(f));
    f.conn = virConnectOpen("test:///default");
    g_assert_nonnull(f.conn);
    virDomainPtr dom = virDomainLookupByName(f.conn, "test");
    g_assert_nonnull(dom);
    g_assert_cmpint(__real_virDomainGetUUIDString(dom, f.uuid), ==, 0);
    virDomainFree(dom);
    f.persistent = 1;
    f.state = f.next_state = VIR_DOMAIN_SHUTOFF;
    f.capacity = 21474836480ULL;
    f.allocation = 1073741824ULL;
    set_xml("<os firmware='bios'><type arch='x86_64'>hvm</type></os>", disk_xml);
}

static JsonNode *collect(void)
{
    GError *error = NULL;
    JsonNode *result = pcv_vm_inspection_collect(f.conn, f.uuid, NULL,
        g_get_monotonic_time() + 5 * G_USEC_PER_SEC, &error);
    g_assert_no_error(error);
    g_assert_nonnull(result);
    return result;
}

static JsonObject *facts(JsonNode *n) { return json_object_get_object_member(json_node_get_object(n), "facts"); }
static JsonObject *disk(JsonNode *n) { return json_array_get_object_element(json_object_get_array_member(facts(n), "disks"), 0); }
static const gchar *str(JsonObject *o, const gchar *k) { return json_object_get_string_member(o, k); }
static void assert_outcome(JsonNode *n, const gchar *outcome)
{
    g_assert_cmpstr(str(json_node_get_object(n), "outcome"), ==, outcome);
}

static void test_stable(void)
{
    reset();
    g_autoptr(JsonNode) result = collect();
    assert_outcome(result, "observed");
    g_assert_cmpstr(str(facts(result), "configuration_stability"), ==, "stable");
    g_autofree gchar *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, f.xml, -1);
    g_assert_cmpstr(str(facts(result), "configuration_sha256"), ==, digest);
    g_assert_cmpstr(str(facts(result), "firmware"), ==, "bios");
    g_assert_cmpstr(str(disk(result), "virtual_bytes"), ==, "21474836480");
    g_assert_true(json_object_get_boolean_member(disk(result), "boot"));
    g_assert_cmpstr(str(disk(result), "backing_chain"), ==, "unknown");
    g_assert_cmpstr(str(disk(result), "encryption"), ==, "unknown");
    g_autofree gchar *wire = json_to_string(result, FALSE);
    g_assert_null(strstr(wire, "/private"));
    g_assert_null(strstr(wire, "vda"));
    g_assert_null(strstr(wire, "<domain"));
    g_assert_cmpuint(strlen(wire), <, PCV_VM_INSPECTION_OUTPUT_LIMIT);
    g_assert_cmpuint(f.lookup_calls, ==, 1);
    g_assert_cmpuint(f.xml_calls, ==, 2);
}

                                                                   
static void save_native(JsonNode *result, const gchar *name)
{
    const gchar *directory = g_getenv("PCV_INSPECTION_EVIDENCE_DIR");
    if (!directory) return;
    g_autofree gchar *path = g_build_filename(directory, name, NULL);
    g_autofree gchar *wire = json_to_string(result, TRUE);
    GError *error = NULL;
    g_assert_true(g_file_set_contents(path, wire, -1, &error));
    g_assert_no_error(error);
}

static void test_native_running(void)
{
    reset(); f.real = TRUE;
    g_autoptr(JsonNode) result = collect();
    assert_outcome(result, "observed");
    g_assert_cmpstr(str(facts(result), "power"), ==, "running");
    g_assert_cmpstr(str(facts(result), "configuration_stability"), ==, "stable");
    g_assert_true(json_object_get_null_member(disk(result), "virtual_bytes"));
    g_assert_cmpuint(f.block_calls, ==, 0);
    save_native(result, "native-running.json");
}

static void test_native_stopped(void)
{
    reset(); f.real = TRUE;
                                                                    
    virDomainPtr domain = virDomainLookupByName(f.conn, "test");
    g_assert_cmpint(virDomainDestroy(domain), ==, 0);
    virDomainFree(domain);
    g_autoptr(JsonNode) result = collect();
    assert_outcome(result, "observed");
    g_assert_cmpstr(str(facts(result), "power"), ==, "stopped");
    g_assert_cmpstr(str(facts(result), "configuration_stability"), ==, "stable");
    g_assert_cmpuint(f.block_calls, ==, 1);
    save_native(result, "native-stopped.json");
}

static void test_invalid_uuid(void)
{
    reset();
    const gchar *bad[] = {NULL, "test", "", "00112233-4455-6677-8899-AABBCCDDEEFF", "../fixture"};
    for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
        GError *error = NULL;
        JsonNode *n = pcv_vm_inspection_collect(f.conn, bad[i], NULL, G_MAXINT64, &error);
        g_assert_null(n);
        g_assert_error(error, pcv_vm_inspection_error_quark(), PCV_VM_INSPECTION_INVALID);
        g_clear_error(&error);
    }
    g_assert_cmpuint(f.lookup_calls, ==, 0);
}

static void test_lookup_failure(void)
{
    reset();
    int codes[] = {VIR_ERR_NO_DOMAIN, VIR_ERR_SYSTEM_ERROR, VIR_ERR_NO_SUPPORT};
    const gchar *outcomes[] = {"not_observed", "unavailable", "unavailable"};
    const gchar *failures[] = {NULL, "READ_FAILED", "UNSUPPORTED"};
    for (guint i = 0; i < G_N_ELEMENTS(codes); i++) {
        f.lookup_error = codes[i];
        g_autoptr(JsonNode) n = collect();
        assert_outcome(n, outcomes[i]);
        JsonObject *o = json_node_get_object(n);
        if (failures[i]) g_assert_cmpstr(str(o, "failure"), ==, failures[i]);
        else g_assert_true(json_object_get_null_member(o, "failure"));
        g_assert_true(json_object_get_null_member(o, "facts"));
    }
    g_assert_cmpuint(f.lookup_calls, ==, 3);
    g_assert_cmpuint(f.xml_calls, ==, 0);
}

static void test_wrong_identity(void)
{
    reset(); f.wrong_uuid = TRUE;
    g_autoptr(JsonNode) n = collect(); assert_outcome(n, "unavailable");
    g_assert_cmpuint(f.xml_calls, ==, 0);
}

static void test_transient(void)
{
    reset(); f.persistent = 0;
    g_autoptr(JsonNode) n = collect(); assert_outcome(n, "unavailable");
    g_assert_cmpstr(str(json_node_get_object(n), "failure"), ==, "UNSUPPORTED");
    g_assert_cmpuint(f.xml_calls, ==, 0);
}

static void test_changed(void)
{
    reset(); f.after = "<domain/>";
    g_autoptr(JsonNode) n = collect();
    g_assert_cmpstr(str(facts(n), "configuration_stability"), ==, "changed");
    g_assert_true(json_object_get_null_member(facts(n), "configuration_sha256"));
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(facts(n), "disks")), ==, 0);
}

static void test_second_failure(void)
{
    reset(); f.xml_failure = 2;
    g_autoptr(JsonNode) n = collect();
    g_assert_cmpstr(str(facts(n), "configuration_stability"), ==, "unknown");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(facts(n), "disks")), ==, 0);
}

static void test_first_failure(void)
{
    reset(); f.xml_failure = 1;
    g_autoptr(JsonNode) n = collect(); assert_outcome(n, "unavailable");
    g_assert_cmpuint(f.block_calls, ==, 0);
}

static void test_active_change(void)
{
    reset(); f.next_active = 1; f.next_state = VIR_DOMAIN_RUNNING;
    g_autoptr(JsonNode) n = collect();
    g_assert_cmpstr(str(facts(n), "configuration_stability"), ==, "changed");
    g_assert_cmpstr(str(facts(n), "power"), ==, "transitioning");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(facts(n), "disks")), ==, 0);
}

static void test_active_unknown(void)
{
    reset(); f.next_active = -1;
    g_autoptr(JsonNode) n = collect();
    g_assert_cmpstr(str(facts(n), "configuration_stability"), ==, "unknown");
}

static void test_state_unknown(void)
{
    reset(); f.state = -1;
    g_autoptr(JsonNode) n = collect();
    g_assert_cmpstr(str(facts(n), "power"), ==, "unknown");
}

static void test_block_failure(void)
{
    reset(); f.block_failure = TRUE;
    g_autoptr(JsonNode) n = collect();
    g_assert_cmpstr(str(facts(n), "disk_enumeration"), ==, "complete");
    g_assert_true(json_object_get_null_member(disk(n), "virtual_bytes"));
}

static void test_block_unknown_values(void)
{
    reset(); f.capacity = G_MAXUINT64; f.allocation = 0;
    g_autoptr(JsonNode) n = collect();
    g_assert_true(json_object_get_null_member(disk(n), "virtual_bytes"));
    g_assert_true(json_object_get_null_member(disk(n), "allocated_bytes"));
}

static void test_uefi_os_unknown(void)
{
    reset();
    set_xml("<os firmware='efi'><type arch='aarch64'>hvm</type>"
            "<loader secure='yes' type='pflash'/></os>", disk_xml);
    g_autoptr(JsonNode) n = collect();
    g_assert_cmpstr(str(facts(n), "firmware"), ==, "uefi");
    g_assert_cmpstr(str(facts(n), "secure_boot"), ==, "unknown");
    g_assert_cmpstr(str(json_object_get_object_member(facts(n), "os"), "family"), ==, "unknown");
}

static void test_legacy_boot_unknown(void)
{
    reset(); set_xml("<os><boot dev='hd'/><boot dev='cdrom'/></os>", disk_xml);
    g_autoptr(JsonNode) n = collect();
    g_assert_true(json_object_get_null_member(disk(n), "boot"));
}

static void test_multi_disk(void)
{
    reset();
    g_autofree gchar *devices = g_strconcat(disk_xml,
        "<disk device='disk'><driver type='raw'/><target dev='vdb'/></disk>"
        "<disk device='cdrom'><target dev='sda'/></disk>", NULL);
    set_xml("<os/>", devices);
    g_autoptr(JsonNode) n = collect();
    JsonArray *disks = json_object_get_array_member(facts(n), "disks");
    g_assert_cmpuint(json_array_get_length(disks), ==, 2);
    g_assert_true(json_object_get_boolean_member(json_array_get_object_element(disks, 0), "boot"));
    g_assert_false(json_object_get_boolean_member(json_array_get_object_element(disks, 1), "boot"));
    g_assert_cmpstr(str(json_array_get_object_element(disks, 0), "disk_id"), !=,
                    str(json_array_get_object_element(disks, 1), "disk_id"));
}

static void test_duplicate_boot(void)
{
    reset();
    g_autofree gchar *devices = g_strconcat(disk_xml,
        "<interface><boot order='1'/></interface>", NULL);
    set_xml("<os/>", devices);
    g_autoptr(JsonNode) n = collect();
    g_assert_true(json_object_get_null_member(disk(n), "boot"));
}

static void test_partial_disks(void)
{
    reset();
    g_autofree gchar *devices = g_strconcat(disk_xml, disk_xml,
        "<disk device='disk'><target dev='/not/a/target'/></disk>", NULL);
    set_xml("<os/>", devices);
    g_autoptr(JsonNode) n = collect();
    g_assert_cmpstr(str(facts(n), "disk_enumeration"), ==, "partial");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(facts(n), "disks")), ==, 1);
}

static void test_disk_limit(void)
{
    reset(); GString *disks = g_string_new(NULL);
    for (guint i = 0; i <= PCV_VM_INSPECTION_DISK_LIMIT; i++)
        g_string_append_printf(disks, "<disk device='disk'><target dev='vd%u'/></disk>", i);
    set_xml("<os/>", disks->str); g_string_free(disks, TRUE);
    g_autoptr(JsonNode) n = collect();
    g_assert_cmpstr(str(facts(n), "disk_enumeration"), ==, "partial");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(facts(n), "disks")), ==, 0);
    g_assert_cmpuint(f.block_calls, ==, 0);
}

static void test_invalid_xml(void)
{
    const gchar *bad[] = {"<domain>", "<domain/>",
        "<!DOCTYPE domain [<!ENTITY x 'hidden'>]><domain/>",
        "<domain><uuid>00000000-0000-0000-0000-000000000099</uuid></domain>"};
    for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
        reset(); g_free(f.xml); f.xml = g_strdup(bad[i]);
        g_autoptr(JsonNode) n = collect(); assert_outcome(n, "unavailable");
        g_assert_cmpuint(f.block_calls, ==, 0);
    }
}

static void test_xml_limit(void)
{
    reset(); g_free(f.xml);
    f.xml = g_strnfill(PCV_VM_INSPECTION_XML_LIMIT + 1, 'x');
    g_autoptr(JsonNode) n = collect(); assert_outcome(n, "unavailable");
}

static void test_deadline(void)
{
    reset(); GError *error = NULL;
    g_autoptr(JsonNode) n = pcv_vm_inspection_collect(f.conn, f.uuid, NULL, 0, &error);
    g_assert_no_error(error); assert_outcome(n, "unavailable");
    g_assert_cmpstr(str(json_node_get_object(n), "failure"), ==, "TIMEOUT");
    g_assert_cmpuint(f.lookup_calls, ==, 0);
}

static void test_cancel(void)
{
    reset(); g_autoptr(GCancellable) cancel = g_cancellable_new();
    g_cancellable_cancel(cancel); GError *error = NULL;
    g_autoptr(JsonNode) n = pcv_vm_inspection_collect(f.conn, f.uuid, cancel, G_MAXINT64, &error);
    g_assert_no_error(error); assert_outcome(n, "unavailable");
    g_assert_cmpuint(f.lookup_calls, ==, 0);
}

static void test_deadline_after_read(void)
{
    reset(); f.delay_ms = 30; GError *error = NULL;
    g_autoptr(JsonNode) n = pcv_vm_inspection_collect(f.conn, f.uuid, NULL,
        g_get_monotonic_time() + 10000, &error);
    g_assert_no_error(error); assert_outcome(n, "unavailable");
    g_assert_cmpstr(str(json_node_get_object(n), "failure"), ==, "TIMEOUT");
    g_assert_cmpuint(f.xml_calls, ==, 1);
    g_assert_cmpuint(f.block_calls, ==, 0);
}

static void test_native_not_observed(void)
{
    reset(); f.real = TRUE;
    GError *error = NULL;
    g_autoptr(JsonNode) n = pcv_vm_inspection_collect(f.conn,
        "00000000-0000-0000-0000-000000000099", NULL, G_MAXINT64, &error);
    g_assert_no_error(error); assert_outcome(n, "not_observed");
    g_assert_cmpuint(f.lookup_calls, ==, 1);
    g_assert_cmpuint(f.xml_calls, ==, 0);
}

static void test_declared_backing_encryption(void)
{
    reset(); set_xml("<os/>", "<disk device='disk'><target dev='vda'/>"
        "<backingStore><source file='/private/base'/></backingStore>"
        "<encryption><secret uuid='private-secret'/></encryption></disk>");
    g_autoptr(JsonNode) n = collect();
    g_assert_cmpstr(str(disk(n), "backing_chain"), ==, "present");
    g_assert_cmpstr(str(disk(n), "encryption"), ==, "encrypted");
    g_autofree gchar *wire = json_to_string(n, FALSE);
    g_assert_null(strstr(wire, "private"));
}

static void test_empty_devices(void)
{
    reset(); set_xml("<os/>", "");
    g_autoptr(JsonNode) n = collect();
    g_assert_cmpstr(str(facts(n), "disk_enumeration"), ==, "complete");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(facts(n), "disks")), ==, 0);
}

                                                                 
static void wait_context(GMainContext *context, gboolean wait_drain)
{
    gint64 until = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    while ((!f.responses || (wait_drain && pcv_drain_get_work())) &&
           g_get_monotonic_time() < until) {
        while (g_main_context_iteration(context, FALSE));
        g_usleep(1000);
    }
    g_assert_cmpuint(f.responses, ==, 1);
    if (wait_drain) g_assert_cmpint(pcv_drain_get_work(), ==, 0);
}

static void test_handler_invalid(void)
{
    reset();
    g_autoptr(GObject) server = g_object_new(G_TYPE_OBJECT, NULL);
    g_autoptr(GObject) connection = g_object_new(G_TYPE_OBJECT, NULL);
    const gchar *bad[] = {"{}", "{\"uuid\":null}", "{\"uuid\":42}", "{\"uuid\":{}}",
                          "{\"uuid\":\"test\"}"};
    for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
        g_autoptr(JsonNode) n = json_from_string(bad[i], NULL);
        handle_vm_inspect_request(json_node_get_object(n), "invalid", (UdsServer *)server,
                                  (GSocketConnection *)connection);
        g_autoptr(JsonNode) response = json_from_string(f.response, NULL);
        JsonObject *err = json_object_get_object_member(json_node_get_object(response), "error");
        g_assert_cmpint(json_object_get_int_member(err, "code"), ==, PURE_RPC_ERR_INVALID_PARAMS);
    }
    g_autoptr(JsonObject) params = json_object_new();
    json_object_set_string_member(params, "uuid", f.uuid);
    json_object_set_string_member(params, "name", "extra");
    handle_vm_inspect_request(params, "extra", (UdsServer *)server, (GSocketConnection *)connection);
    g_assert_cmpint(f.acquire_count, ==, 0);
    g_assert_cmpuint(f.audits, ==, G_N_ELEMENTS(bad) + 1);
    g_assert_cmpint(f.audit_code, ==, PURE_RPC_ERR_INVALID_PARAMS);
}

static void test_handler_success(void)
{
    reset();
    g_autoptr(GMainContext) context = g_main_context_new();
    g_main_context_push_thread_default(context);
    g_autoptr(GObject) server = g_object_new(G_TYPE_OBJECT, NULL);
    g_autoptr(GObject) connection = g_object_new(G_TYPE_OBJECT, NULL);
    g_autoptr(JsonObject) params = json_object_new();
    json_object_set_string_member(params, "uuid", f.uuid);
    handle_vm_inspect_request(params, "read-1", (UdsServer *)server, (GSocketConnection *)connection);
    wait_context(context, TRUE);
    g_autoptr(JsonNode) response = json_from_string(f.response, NULL);
    JsonObject *envelope = json_node_get_object(response);
    g_assert_cmpstr(str(envelope, "id"), ==, "read-1");
    g_assert_cmpstr(str(json_object_get_object_member(envelope, "result"), "outcome"), ==, "observed");
    g_assert_cmpint(f.acquire_count, ==, 1);
    g_assert_cmpint(f.release_count, ==, 1);
    g_assert_cmpuint(f.audits, ==, 1);
    g_assert_cmpstr(f.audit_result, ==, "ok");
    g_assert_cmpstr(f.audit_target, ==, f.uuid);
    g_main_context_pop_thread_default(context);
}

static void mark_freed(gpointer data, GObject *object)
{
    (void)object;
    *(gboolean *)data = TRUE;
}

static void test_handler_timeout(void)
{
    reset(); f.delay_ms = 500;
    g_autoptr(GMainContext) context = g_main_context_new();
    g_main_context_push_thread_default(context);
    gboolean server_freed = FALSE, connection_freed = FALSE;
    GObject *server = g_object_new(G_TYPE_OBJECT, NULL);
    GObject *connection = g_object_new(G_TYPE_OBJECT, NULL);
    g_object_weak_ref(server, mark_freed, &server_freed);
    g_object_weak_ref(connection, mark_freed, &connection_freed);
    g_autoptr(JsonObject) params = json_object_new();
    json_object_set_string_member(params, "uuid", f.uuid);
    handle_vm_inspect_request(params, "slow-1", (UdsServer *)server, (GSocketConnection *)connection);
    g_object_unref(server); g_object_unref(connection);
    wait_context(context, FALSE);
    g_assert_false(server_freed);
    g_assert_false(connection_freed);
    g_assert_cmpint(pcv_drain_get_work(), >, 0);
    g_assert_cmpint(g_atomic_int_get(&f.release_count), ==, 0);
    g_autoptr(JsonNode) response = json_from_string(f.response, NULL);
    JsonObject *err = json_object_get_object_member(json_node_get_object(response), "error");
    g_assert_cmpint(json_object_get_int_member(err, "code"), ==, PURE_RPC_ERR_TIMEOUT);
    wait_context(context, TRUE);
    g_assert_true(server_freed);
    g_assert_true(connection_freed);
    g_assert_cmpint(g_atomic_int_get(&f.release_count), ==, 1);
    g_assert_cmpuint(f.responses, ==, 1);
    g_assert_cmpuint(f.audits, ==, 1);
    g_assert_cmpint(f.audit_code, ==, PURE_RPC_ERR_TIMEOUT);
    g_main_context_pop_thread_default(context);
}

static void test_handler_pool_failure(void)
{
    reset(); f.pool_failure = TRUE;
    g_autoptr(GMainContext) context = g_main_context_new();
    g_main_context_push_thread_default(context);
    g_autoptr(GObject) server = g_object_new(G_TYPE_OBJECT, NULL);
    g_autoptr(GObject) connection = g_object_new(G_TYPE_OBJECT, NULL);
    g_autoptr(JsonObject) params = json_object_new();
    json_object_set_string_member(params, "uuid", f.uuid);
    handle_vm_inspect_request(params, "unavailable", (UdsServer *)server, (GSocketConnection *)connection);
    wait_context(context, TRUE);
    g_autoptr(JsonNode) response = json_from_string(f.response, NULL);
    JsonObject *result = json_object_get_object_member(json_node_get_object(response), "result");
    g_assert_cmpstr(str(result, "outcome"), ==, "unavailable");
    g_assert_cmpstr(str(result, "failure"), ==, "READ_FAILED");
    g_assert_cmpint(f.acquire_count, ==, 1);
    g_assert_cmpint(f.release_count, ==, 0);
    g_assert_cmpuint(f.lookup_calls, ==, 0);
    g_assert_cmpuint(f.audits, ==, 1);
    g_assert_cmpstr(f.audit_result, ==, "fail");
    g_main_context_pop_thread_default(context);
}

                                                                
static void test_handler_bridge_metadata(void)
{
    reset();
    g_autoptr(GMainContext) context = g_main_context_new();
    g_main_context_push_thread_default(context);
    g_autoptr(GObject) server = g_object_new(G_TYPE_OBJECT, NULL);
    g_autoptr(GObject) connection = g_object_new(G_TYPE_OBJECT, NULL);
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "uuid", f.uuid);
    json_object_set_string_member(params, "_pcv_caller_sub", "inspection-admin");
    json_object_set_int_member(params, "_pcv_caller_role", 2);
                                                       
    pcv_rpc_completion_begin("vm.inspect");
    pcv_rpc_completion_set_actor("scope-admin");
    handle_vm_inspect_request(params, "bridge", (UdsServer *)server, (GSocketConnection *)connection);
    (void)pcv_rpc_completion_finish();
    json_object_set_string_member(params, "_pcv_caller_sub", "changed-after-dispatch");
    json_object_unref(params);
    wait_context(context, TRUE);
    g_assert_cmpuint(f.audits, ==, 1);
    g_assert_cmpstr(f.audit_subject, ==, "scope-admin");
    g_assert_cmpstr(f.audit_result, ==, "ok");
    g_main_context_pop_thread_default(context);
}

static void test_handler_invalid_metadata(void)
{
    reset();
    g_autoptr(GObject) server = g_object_new(G_TYPE_OBJECT, NULL);
    g_autoptr(GObject) connection = g_object_new(G_TYPE_OBJECT, NULL);
    const gchar *keys[] = {"_pcv_caller_sub", "_pcv_caller_role", "extra"};
    for (guint i = 0; i < G_N_ELEMENTS(keys); i++) {
        g_autoptr(JsonObject) params = json_object_new();
        json_object_set_string_member(params, "uuid", f.uuid);
        json_object_set_boolean_member(params, keys[i], TRUE);
        handle_vm_inspect_request(params, "bad-meta", (UdsServer *)server, (GSocketConnection *)connection);
        g_assert_cmpint(f.audit_code, ==, PURE_RPC_ERR_INVALID_PARAMS);
    }
    g_assert_cmpuint(f.audits, ==, 3);
    g_assert_cmpint(f.acquire_count, ==, 0);
}

                                                                                          
                                                 
                                                                               
static void test_handler_grpc_tenant_metadata(void)
{
    reset();
    g_autoptr(GMainContext) context = g_main_context_new();
    g_main_context_push_thread_default(context);
    g_autoptr(GObject) server = g_object_new(G_TYPE_OBJECT, NULL);
    g_autoptr(GObject) connection = g_object_new(G_TYPE_OBJECT, NULL);
    for (guint i = 0; i < 2; i++) {                                                              
        f.responses = 0;
        g_autoptr(JsonObject) params = json_object_new();
        json_object_set_string_member(params, "uuid", f.uuid);
        json_object_set_string_member(params, "_pcv_caller_sub", "grpc");
        json_object_set_int_member(params, "_pcv_caller_role", 2);
        if (i == 0)
            json_object_set_null_member(params, "_pcv_caller_tenant");
        else
            json_object_set_string_member(params, "_pcv_caller_tenant", "ta");
        handle_vm_inspect_request(params, "grpc-tenant", (UdsServer *)server, (GSocketConnection *)connection);
        wait_context(context, TRUE);
        g_autoptr(JsonNode) response = json_from_string(f.response, NULL);
        JsonObject *envelope = json_node_get_object(response);
        g_assert_false(json_object_has_member(envelope, "error"));
        g_assert_cmpstr(str(json_object_get_object_member(envelope, "result"), "outcome"), ==, "observed");
    }
    g_assert_cmpint(f.acquire_count, ==, 2);
    g_assert_cmpuint(f.audits, ==, 2);

                                                 
    f.responses = 0;
    g_autoptr(JsonObject) bad = json_object_new();
    json_object_set_string_member(bad, "uuid", f.uuid);
    json_object_set_int_member(bad, "_pcv_caller_tenant", 7);
    handle_vm_inspect_request(bad, "grpc-tenant-bad", (UdsServer *)server, (GSocketConnection *)connection);
    g_assert_cmpuint(f.responses, ==, 1);
    g_autoptr(JsonNode) response = json_from_string(f.response, NULL);
    JsonObject *err = json_object_get_object_member(json_node_get_object(response), "error");
    g_assert_cmpint(json_object_get_int_member(err, "code"), ==, PURE_RPC_ERR_INVALID_PARAMS);
    g_assert_cmpint(f.audit_code, ==, PURE_RPC_ERR_INVALID_PARAMS);
    g_assert_cmpint(f.acquire_count, ==, 2);
    g_main_context_pop_thread_default(context);
}

static void test_handler_connection_subject(void)
{
    reset();
    g_autoptr(GMainContext) context = g_main_context_new();
    g_main_context_push_thread_default(context);
    g_autoptr(GObject) server = g_object_new(G_TYPE_OBJECT, NULL);
    g_autoptr(GObject) connection = g_object_new(G_TYPE_OBJECT, NULL);
    g_object_set_data(connection, "pcv-caller-sub", "server-admin");
    g_autoptr(JsonObject) params = json_object_new();
    json_object_set_string_member(params, "uuid", f.uuid);
    json_object_set_string_member(params, "_pcv_caller_sub", "bridge-admin");
    pcv_rpc_completion_begin("vm.inspect");
    pcv_rpc_completion_set_actor("scope-admin");
    handle_vm_inspect_request(params, "identity", (UdsServer *)server, (GSocketConnection *)connection);
    (void)pcv_rpc_completion_finish();
    wait_context(context, TRUE);
    g_assert_cmpstr(f.audit_subject, ==, "scope-admin");
                               
    f.responses = 0;
    handle_vm_inspect_request(params, "identity2", (UdsServer *)server, (GSocketConnection *)connection);
    wait_context(context, TRUE);
    g_assert_cmpstr(f.audit_subject, ==, "local-admin");
    g_assert_cmpuint(f.audits, ==, 2);
    g_main_context_pop_thread_default(context);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    pcv_drain_init();
    g_test_add_func("/inspection/stable", test_stable);
    g_test_add_func("/inspection/native-running", test_native_running);
    g_test_add_func("/inspection/native-stopped", test_native_stopped);
    g_test_add_func("/inspection/invalid-uuid", test_invalid_uuid);
    g_test_add_func("/inspection/lookup-failure", test_lookup_failure);
    g_test_add_func("/inspection/wrong-identity", test_wrong_identity);
    g_test_add_func("/inspection/transient", test_transient);
    g_test_add_func("/inspection/changed", test_changed);
    g_test_add_func("/inspection/second-failure", test_second_failure);
    g_test_add_func("/inspection/first-failure", test_first_failure);
    g_test_add_func("/inspection/active-change", test_active_change);
    g_test_add_func("/inspection/active-unknown", test_active_unknown);
    g_test_add_func("/inspection/state-unknown", test_state_unknown);
    g_test_add_func("/inspection/block-failure", test_block_failure);
    g_test_add_func("/inspection/block-unknown-values", test_block_unknown_values);
    g_test_add_func("/inspection/uefi-os-unknown", test_uefi_os_unknown);
    g_test_add_func("/inspection/legacy-boot-unknown", test_legacy_boot_unknown);
    g_test_add_func("/inspection/multi-disk", test_multi_disk);
    g_test_add_func("/inspection/duplicate-boot", test_duplicate_boot);
    g_test_add_func("/inspection/partial-disks", test_partial_disks);
    g_test_add_func("/inspection/disk-limit", test_disk_limit);
    g_test_add_func("/inspection/invalid-xml", test_invalid_xml);
    g_test_add_func("/inspection/xml-limit", test_xml_limit);
    g_test_add_func("/inspection/deadline", test_deadline);
    g_test_add_func("/inspection/cancel", test_cancel);
    g_test_add_func("/inspection/deadline-after-read", test_deadline_after_read);
    g_test_add_func("/inspection/native-not-observed", test_native_not_observed);
    g_test_add_func("/inspection/declared-backing-encryption", test_declared_backing_encryption);
    g_test_add_func("/inspection/empty-devices", test_empty_devices);
    g_test_add_func("/inspection/handler-invalid", test_handler_invalid);
    g_test_add_func("/inspection/handler-success", test_handler_success);
    g_test_add_func("/inspection/handler-timeout", test_handler_timeout);
    g_test_add_func("/inspection/handler-pool-failure", test_handler_pool_failure);
    g_test_add_func("/inspection/handler-bridge-metadata", test_handler_bridge_metadata);
    g_test_add_func("/inspection/handler-invalid-metadata", test_handler_invalid_metadata);
    g_test_add_func("/inspection/handler-grpc-tenant-metadata", test_handler_grpc_tenant_metadata);
    g_test_add_func("/inspection/handler-connection-subject", test_handler_connection_subject);
    int status = g_test_run();
    g_assert_cmpint(pcv_drain_get_work(), ==, 0);
    if (f.conn) virConnectClose(f.conn);
    g_free(f.xml); g_free(f.response);
    pcv_drain_shutdown();
    return status;
}
