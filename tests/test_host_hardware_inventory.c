












#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#include "modules/dispatcher/host_hardware_inventory.h"

static gchar *
fixture_path(const gchar *root, const gchar *relative)
{
    return g_build_filename(root, relative, NULL);
}

static void
fixture_write_bytes(const gchar *root, const gchar *relative,
                    const guint8 *bytes, gsize length)
{
    gchar *path = fixture_path(root, relative);
    gchar *parent = g_path_get_dirname(path);
    g_assert_cmpint(g_mkdir_with_parents(parent, 0700), ==, 0);
    g_assert_true(g_file_set_contents(path, (const gchar *)bytes, (gssize)length, NULL));
    g_free(parent);
    g_free(path);
}

static void
fixture_write(const gchar *root, const gchar *relative, const gchar *value)
{
    fixture_write_bytes(root, relative, (const guint8 *)value, strlen(value));
}

static void
fixture_mkdir(const gchar *root, const gchar *relative)
{
    gchar *path = fixture_path(root, relative);
    g_assert_cmpint(g_mkdir_with_parents(path, 0700), ==, 0);
    g_free(path);
}

static void
fixture_symlink(const gchar *root, const gchar *relative, const gchar *target)
{
    gchar *path = fixture_path(root, relative);
    gchar *parent = g_path_get_dirname(path);
    g_assert_cmpint(g_mkdir_with_parents(parent, 0700), ==, 0);
    g_assert_cmpint(symlink(target, path), ==, 0);
    g_free(parent);
    g_free(path);
}

static void
remove_tree(const gchar *path)
{
    if (g_file_test(path, G_FILE_TEST_IS_SYMLINK) || !g_file_test(path, G_FILE_TEST_IS_DIR)) {
        g_remove(path);
        return;
    }
    GDir *dir = g_dir_open(path, 0, NULL);
    if (dir) {
        const gchar *name = NULL;
        while ((name = g_dir_read_name(dir)) != NULL) {
            gchar *child = g_build_filename(path, name, NULL);
            remove_tree(child);
            g_free(child);
        }
        g_dir_close(dir);
    }
    g_rmdir(path);
}

static gchar *
inventory_to_json(JsonObject *inventory)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, inventory);
    JsonGenerator *generator = json_generator_new();
    json_generator_set_root(generator, node);
    gchar *json = json_generator_to_data(generator, NULL);
    g_object_unref(generator);
    json_node_free(node);
    return json;
}

static void
write_type17_fixture(const gchar *root)
{
    guint8 header[0x22] = { 0 };
    header[0] = 17;
    header[1] = sizeof(header);
    header[8] = 72;
    header[10] = 64;
    header[12] = 0x00;
    header[13] = 0x40;
    header[14] = 0x09;
    header[16] = 1;
    header[17] = 2;
    header[18] = 0x1a;
    header[21] = 0x60;
    header[22] = 0x09;
    header[23] = 3;
    header[24] = 4;
    header[25] = 5;
    header[26] = 6;
    header[32] = 0x55;
    header[33] = 0x08;
    static const gchar strings[] =
        "DIMM_A1\0BANK 0\0SK hynix\0TOP-SECRET-SERIAL\0"
        "TOP-SECRET-ASSET\0HMA42GR7MFR4N-UH\0\0";
    static const guint8 end_marker[] = { 127, 4, 0, 0, 0, 0 };
    GByteArray *table = g_byte_array_new();
    g_byte_array_append(table, header, sizeof(header));
    g_byte_array_append(table, (const guint8 *)strings, sizeof(strings) - 1);
    g_byte_array_append(table, end_marker, sizeof(end_marker));
    fixture_write_bytes(root, "sys/firmware/dmi/tables/DMI", table->data, table->len);
    g_byte_array_unref(table);
}

static void
write_complete_fixture(const gchar *root)
{
    for (guint cpu = 0; cpu < 4; cpu++) {
        gchar *package = g_strdup_printf(
            "sys/devices/system/cpu/cpu%u/topology/physical_package_id", cpu);
        gchar *core = g_strdup_printf("sys/devices/system/cpu/cpu%u/topology/core_id", cpu);
        fixture_write(root, package, cpu < 2 ? "0\n" : "1\n");
        fixture_write(root, core, "0\n");
        g_free(package);
        g_free(core);
    }
    fixture_write(root, "sys/devices/system/node/node0/cpulist", "0-1\n");
    fixture_write(root, "sys/devices/system/node/node1/cpulist", "2-3\n");
    fixture_write(root, "sys/class/dmi/id/sys_vendor", "Supermicro\n");
    fixture_write(root, "sys/class/dmi/id/product_name", "Super Server\n");
    fixture_write(root, "sys/class/dmi/id/board_name", "X10DRL-C\n");
    fixture_write(root, "sys/class/dmi/id/bios_version", "3.2\n");
    fixture_write(root, "sys/class/dmi/id/chassis_type", "23\n");
    fixture_write(root, "sys/class/dmi/id/product_serial", "FORBIDDEN-DMI-SERIAL\n");
    fixture_write(root, "sys/class/dmi/id/product_uuid", "FORBIDDEN-DMI-UUID\n");
    write_type17_fixture(root);

    fixture_write(root, "sys/bus/pci/devices/0000:01:00.0/class", "0x020000\n");
    fixture_write(root, "sys/bus/pci/devices/0000:01:00.0/vendor", "0x8086\n");
    fixture_write(root, "sys/bus/pci/devices/0000:01:00.0/device", "0x1533\n");
    fixture_write(root, "sys/bus/pci/devices/0000:01:00.0/revision", "0x03\n");
    fixture_symlink(root, "sys/bus/pci/devices/0000:01:00.0/driver",
                    "../../../../bus/pci/drivers/igb");
    fixture_symlink(root, "sys/bus/pci/devices/0000:01:00.0/iommu_group",
                    "../../../../kernel/iommu_groups/42");
    fixture_write(root, "usr/share/misc/pci.ids",
                  "8086  Intel Corporation\n\t1533  I210 Gigabit Network Connection\n");

    fixture_write(root, "sys/class/nvme/nvme0/model", "Samsung SSD 990 PRO 2TB\n");
    fixture_write(root, "sys/class/nvme/nvme0/firmware_rev", "5B2QJXD7\n");
    fixture_write(root, "sys/class/nvme/nvme0/state", "live\n");
    fixture_write(root, "sys/block/nvme0n1/size", "3907029168\n");

    fixture_write(root, "sys/class/hwmon/hwmon0/name", "coretemp\n");
    fixture_write(root, "sys/class/hwmon/hwmon0/temp1_input", "42000\n");
    fixture_write(root, "sys/class/hwmon/hwmon0/temp1_label", "Package id 0\n");
    fixture_write(root, "sys/class/hwmon/hwmon0/temp1_crit", "100000\n");
    fixture_symlink(root, "sys/class/hwmon/hwmon0/device", "../../../devices/platform/coretemp.0");
    fixture_write(root, "sys/class/hwmon/hwmon1/name", "power_meter\n");
    fixture_write(root, "sys/class/hwmon/hwmon1/power1_average", "97000000\n");
    fixture_write(root, "sys/class/hwmon/hwmon1/power1_accuracy", "97.500%\n");
    fixture_symlink(root, "sys/class/hwmon/hwmon1/device", "../../../devices/platform/power_meter.0");

    fixture_write(root, "sys/class/net/eno1/operstate", "up\n");
    fixture_write(root, "sys/class/net/eno1/carrier", "1\n");
    fixture_write(root, "sys/class/net/eno1/speed", "1000\n");
    fixture_write(root, "sys/class/net/eno1/duplex", "full\n");
    fixture_write(root, "sys/class/net/eno1/address", "aa:bb:cc:dd:ee:ff\n");
    fixture_symlink(root, "sys/class/net/eno1/device",
                    "../../../bus/pci/devices/0000:01:00.0");
}

static void
test_complete_fixture_normalizes_and_excludes_unique_identifiers(void)
{
    GError *error = NULL;
    gchar *root = g_dir_make_tmp("pcv-hardware-fixture-XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(root);
    write_complete_fixture(root);

    JsonObject *inventory = pcv_host_hardware_inventory_collect(root);
    g_assert_cmpint(json_object_get_int_member(inventory, "schema_version"), ==, 1);
    g_assert_cmpstr(json_object_get_string_member(inventory, "privacy_profile"), ==,
                    "viewer-safe-v1");
    JsonObject *cpu = json_object_get_object_member(inventory, "cpu_topology");
    g_assert_cmpint(json_object_get_int_member(cpu, "logical_processors"), ==, 4);
    g_assert_cmpint(json_object_get_int_member(cpu, "physical_cores"), ==, 2);
    g_assert_cmpint(json_object_get_int_member(cpu, "sockets"), ==, 2);
    g_assert_cmpint(json_object_get_int_member(cpu, "threads_per_core"), ==, 2);
    g_assert_cmpuint(json_array_get_length(
        json_object_get_array_member(cpu, "numa_nodes")), ==, 2);

    JsonObject *platform = json_object_get_object_member(inventory, "platform");
    g_assert_cmpstr(json_object_get_string_member(platform, "board_name"), ==, "X10DRL-C");
    g_assert_cmpstr(json_object_get_string_member(platform, "chassis_type"), ==,
                    "rack_mount_chassis");
    JsonObject *memory = json_object_get_object_member(inventory, "memory");
    g_assert_true(json_object_get_boolean_member(memory, "ecc_detected"));
    JsonObject *dimm = json_array_get_object_element(
        json_object_get_array_member(memory, "devices"), 0);
    g_assert_cmpint(json_object_get_int_member(dimm, "size_bytes"), ==,
                    16LL * 1024LL * 1024LL * 1024LL);
    g_assert_cmpstr(json_object_get_string_member(dimm, "manufacturer"), ==, "SK hynix");

    JsonObject *pci = json_object_get_object_member(inventory, "pci");
    JsonObject *nic_controller = json_array_get_object_element(
        json_object_get_array_member(pci, "devices"), 0);
    g_assert_cmpstr(json_object_get_string_member(nic_controller, "device_name"), ==,
                    "I210 Gigabit Network Connection");
    JsonObject *nvme = json_object_get_object_member(inventory, "nvme");
    JsonObject *ssd = json_array_get_object_element(
        json_object_get_array_member(nvme, "devices"), 0);
    g_assert_cmpstr(json_object_get_string_member(ssd, "state"), ==, "live");
    g_assert_false(json_object_get_boolean_member(
        json_object_get_object_member(ssd, "health"), "smart_available"));

    JsonObject *sensors = json_object_get_object_member(inventory, "sensors");
    g_assert_cmpint(json_object_get_int_member(sensors, "returned_count"), ==, 2);
    JsonObject *power = json_array_get_object_element(
        json_object_get_array_member(sensors, "items"), 1);
    g_assert_cmpfloat(json_object_get_double_member(power, "accuracy_percent"), ==, 97.5);
    JsonObject *network = json_object_get_object_member(inventory, "network");
    JsonObject *eno1 = json_array_get_object_element(
        json_object_get_array_member(network, "interfaces"), 0);
    g_assert_true(json_object_get_boolean_member(eno1, "carrier"));
    g_assert_cmpint(json_object_get_int_member(eno1, "speed_mbps"), ==, 1000);

    gchar *json = inventory_to_json(inventory);
    g_assert_null(strstr(json, "TOP-SECRET-SERIAL"));
    g_assert_null(strstr(json, "TOP-SECRET-ASSET"));
    g_assert_null(strstr(json, "FORBIDDEN-DMI-SERIAL"));
    g_assert_null(strstr(json, "FORBIDDEN-DMI-UUID"));
    g_assert_null(strstr(json, "aa:bb:cc:dd:ee:ff"));
    g_assert_null(strstr(json, "product_serial"));
    g_assert_null(strstr(json, "product_uuid"));
    g_free(json);
    remove_tree(root);
    g_free(root);
}

static void
test_missing_sources_are_explicitly_unavailable(void)
{
    GError *error = NULL;
    gchar *root = g_dir_make_tmp("pcv-hardware-empty-XXXXXX", &error);
    g_assert_no_error(error);
    JsonObject *inventory = pcv_host_hardware_inventory_collect(root);
    JsonObject *coverage = json_object_get_object_member(inventory, "coverage");
    g_assert_cmpstr(json_object_get_string_member(coverage, "cpu_topology"), ==, "unavailable");
    g_assert_cmpstr(json_object_get_string_member(coverage, "memory"), ==, "unavailable");
    g_assert_cmpstr(json_object_get_string_member(coverage, "sensors"), ==, "unavailable");
    JsonObject *memory = json_object_get_object_member(inventory, "memory");
    g_assert_cmpuint(json_array_get_length(
        json_object_get_array_member(memory, "devices")), ==, 0);
    json_object_unref(inventory);
    remove_tree(root);
    g_free(root);
}

static void
test_pci_inventory_is_bounded_and_reports_truncation(void)
{
    GError *error = NULL;
    gchar *root = g_dir_make_tmp("pcv-hardware-cap-XXXXXX", &error);
    g_assert_no_error(error);
    for (guint i = 0; i < 260; i++) {
        gchar *relative = g_strdup_printf("sys/bus/pci/devices/0000:%02x:%02x.%x",
                                          i / 32, (i / 8) % 4, i % 8);
        fixture_mkdir(root, relative);
        g_free(relative);
    }
    JsonObject *inventory = pcv_host_hardware_inventory_collect(root);
    JsonObject *pci = json_object_get_object_member(inventory, "pci");
    g_assert_cmpint(json_object_get_int_member(pci, "source_count"), ==, 260);
    g_assert_cmpint(json_object_get_int_member(pci, "returned_count"), ==, 256);
    g_assert_true(json_object_get_boolean_member(pci, "truncated"));
    g_assert_cmpuint(json_array_get_length(
        json_object_get_array_member(pci, "devices")), ==, 256);
    json_object_unref(inventory);
    remove_tree(root);
    g_free(root);
}

void
test_host_hardware_inventory_register(void)
{
    g_test_add_func("/host_hardware/normalize_and_privacy",
                    test_complete_fixture_normalizes_and_excludes_unique_identifiers);
    g_test_add_func("/host_hardware/missing_sources",
                    test_missing_sources_are_explicitly_unavailable);
    g_test_add_func("/host_hardware/pci_bounded",
                    test_pci_inventory_is_bounded_and_reports_truncation);
}

#ifdef PCV_HOST_HARDWARE_STANDALONE
int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    test_host_hardware_inventory_register();
    return g_test_run();
}
#endif
