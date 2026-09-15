














#include "host_hardware_inventory.h"

#include <errno.h>
#include <math.h>
#include <string.h>

#define PCV_HW_MAX_LOGICAL_CPUS 4096U
#define PCV_HW_MAX_NUMA_NODES 64U
#define PCV_HW_MAX_MEMORY_DEVICES 256U
#define PCV_HW_MAX_PCI_DEVICES 256U
#define PCV_HW_MAX_NVME_DEVICES 64U
#define PCV_HW_MAX_SENSORS 512U
#define PCV_HW_MAX_NETWORK_INTERFACES 128U
#define PCV_HW_MAX_TEXT_BYTES 256U
#define PCV_HW_MAX_SMBIOS_BYTES (8U * 1024U * 1024U)
#define PCV_HW_MAX_PCI_IDS_BYTES (16U * 1024U * 1024U)

typedef struct {
    GHashTable *vendors;
    GHashTable *devices;
} PcvPciIds;














static gint
compare_string_ptrs(gconstpointer left, gconstpointer right)
{
    const gchar *const *a = left;
    const gchar *const *b = right;
    return g_strcmp0(*a, *b);
}


static gchar *
root_path(const gchar *root, const gchar *relative)
{
    if (!root || root[0] == '\0' || g_strcmp0(root, "/") == 0)
        return g_strdup_printf("/%s", relative);
    return g_build_filename(root, relative, NULL);
}

static gchar *
clean_text(const gchar *raw)
{
    if (!raw)
        return NULL;

    gchar *valid = g_utf8_make_valid(raw, -1);
    g_strstrip(valid);
    for (gchar *p = valid; *p; p++) {
        if ((guchar)*p < 0x20)
            *p = ' ';
    }
    g_strstrip(valid);
    if (valid[0] == '\0') {
        g_free(valid);
        return NULL;
    }

    if (strlen(valid) > PCV_HW_MAX_TEXT_BYTES) {
        gchar *cut = g_strndup(valid, PCV_HW_MAX_TEXT_BYTES);
        while (!g_utf8_validate(cut, -1, NULL) && cut[0] != '\0')
            cut[strlen(cut) - 1] = '\0';
        g_free(valid);
        valid = cut;
    }
    return valid;
}

static gboolean
read_text_file(const gchar *path, gsize max_bytes, gchar **out)
{
    gchar *contents = NULL;
    gsize length = 0;
    if (!g_file_get_contents(path, &contents, &length, NULL))
        return FALSE;
    if (length > max_bytes) {
        g_free(contents);
        return FALSE;
    }

    gchar *cleaned = clean_text(contents);
    g_free(contents);
    if (!cleaned)
        return FALSE;
    *out = cleaned;
    return TRUE;
}

static gboolean
read_root_text(const gchar *root, const gchar *relative, gchar **out)
{
    gchar *path = root_path(root, relative);
    gboolean ok = read_text_file(path, PCV_HW_MAX_TEXT_BYTES, out);
    g_free(path);
    return ok;
}

static gboolean
read_int64_file(const gchar *path, gint64 *out)
{
    gchar *text = NULL;
    if (!read_text_file(path, 96, &text))
        return FALSE;

    errno = 0;
    gchar *end = NULL;
    gint64 value = g_ascii_strtoll(text, &end, 10);
    while (end && g_ascii_isspace(*end))
        end++;
    gboolean ok = errno == 0 && end && *end == '\0';
    g_free(text);
    if (ok)
        *out = value;
    return ok;
}

static gboolean
read_root_int64(const gchar *root, const gchar *relative, gint64 *out)
{
    gchar *path = root_path(root, relative);
    gboolean ok = read_int64_file(path, out);
    g_free(path);
    return ok;
}

static GPtrArray *
sorted_directory_names(const gchar *path)
{
    GDir *dir = g_dir_open(path, 0, NULL);
    if (!dir)
        return NULL;

    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    const gchar *name = NULL;
    while ((name = g_dir_read_name(dir)) != NULL)
        g_ptr_array_add(names, g_strdup(name));
    g_dir_close(dir);
    g_ptr_array_sort(names, compare_string_ptrs);
    return names;
}













static gboolean
ascii_digits_only(const gchar *text)
{
    if (!text || text[0] == '\0')
        return FALSE;
    for (const gchar *p = text; *p; p++) {
        if (!g_ascii_isdigit(*p))
            return FALSE;
    }
    return TRUE;
}

static gchar *
safe_symlink_basename(const gchar *path)
{
    gchar *target = g_file_read_link(path, NULL);
    if (!target)
        return NULL;
    gchar *base = g_path_get_basename(target);
    gchar *cleaned = clean_text(base);
    g_free(base);
    g_free(target);
    return cleaned;
}

static void
set_count_metadata(JsonObject *object, guint source_count, guint returned_count)
{
    json_object_set_int_member(object, "source_count", source_count);
    json_object_set_int_member(object, "returned_count", returned_count);
    json_object_set_boolean_member(object, "truncated", returned_count < source_count);
}

static JsonObject *
collect_cpu_topology(const gchar *root, const gchar **coverage)
{












    JsonObject *cpu = json_object_new();
    JsonArray *numa = json_array_new();
    GHashTable *packages = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GHashTable *cores = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GHashTable *thread_counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    gchar *cpu_dir_path = root_path(root, "sys/devices/system/cpu");
    GPtrArray *names = sorted_directory_names(cpu_dir_path);
    g_free(cpu_dir_path);

    guint logical = 0;
    if (names) {
        for (guint i = 0; i < names->len; i++) {
            const gchar *name = g_ptr_array_index(names, i);
            if (!g_str_has_prefix(name, "cpu") || !ascii_digits_only(name + 3))
                continue;

            gint64 online = 1;
            gchar *online_rel = g_strdup_printf("sys/devices/system/cpu/%s/online", name);
            if (read_root_int64(root, online_rel, &online) && online != 1) {
                g_free(online_rel);
                continue;
            }
            g_free(online_rel);

            gint64 package_id = 0;
            gint64 core_id = 0;
            gchar *package_rel = g_strdup_printf(
                "sys/devices/system/cpu/%s/topology/physical_package_id", name);
            gchar *core_rel = g_strdup_printf(
                "sys/devices/system/cpu/%s/topology/core_id", name);
            gboolean valid = read_root_int64(root, package_rel, &package_id)
                && read_root_int64(root, core_rel, &core_id);
            g_free(package_rel);
            g_free(core_rel);
            if (!valid || package_id < 0 || core_id < 0)
                continue;

            logical++;
            if (logical > PCV_HW_MAX_LOGICAL_CPUS)
                continue;

            gchar *package_key = g_strdup_printf("%" G_GINT64_FORMAT, package_id);
            g_hash_table_add(packages, package_key);
            gchar *core_key = g_strdup_printf("%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT,
                                              package_id, core_id);
            if (!g_hash_table_contains(cores, core_key))
                g_hash_table_add(cores, g_strdup(core_key));
            guint count = GPOINTER_TO_UINT(g_hash_table_lookup(thread_counts, core_key));
            g_hash_table_replace(thread_counts, core_key, GUINT_TO_POINTER(count + 1));
        }
        g_ptr_array_free(names, TRUE);
    }

    guint max_threads = 0;
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, thread_counts);
    while (g_hash_table_iter_next(&iter, NULL, &value))
        max_threads = MAX(max_threads, GPOINTER_TO_UINT(value));

    json_object_set_int_member(cpu, "logical_processors",
                               MIN(logical, PCV_HW_MAX_LOGICAL_CPUS));
    json_object_set_int_member(cpu, "physical_cores", g_hash_table_size(cores));
    json_object_set_int_member(cpu, "sockets", g_hash_table_size(packages));
    if (max_threads > 0)
        json_object_set_int_member(cpu, "threads_per_core", max_threads);
    json_object_set_boolean_member(cpu, "logical_processors_truncated",
                                   logical > PCV_HW_MAX_LOGICAL_CPUS);

    gchar *node_dir_path = root_path(root, "sys/devices/system/node");
    GPtrArray *node_names = sorted_directory_names(node_dir_path);
    g_free(node_dir_path);
    guint node_source_count = 0;
    guint node_returned_count = 0;
    if (node_names) {
        for (guint i = 0; i < node_names->len; i++) {
            const gchar *name = g_ptr_array_index(node_names, i);
            if (!g_str_has_prefix(name, "node") || !ascii_digits_only(name + 4))
                continue;
            node_source_count++;
            if (node_returned_count >= PCV_HW_MAX_NUMA_NODES)
                continue;
            gchar *rel = g_strdup_printf("sys/devices/system/node/%s/cpulist", name);
            gchar *cpu_list = NULL;
            if (read_root_text(root, rel, &cpu_list)) {
                JsonObject *node = json_object_new();
                json_object_set_int_member(node, "id", g_ascii_strtoll(name + 4, NULL, 10));
                json_object_set_string_member(node, "cpu_list", cpu_list);
                json_array_add_object_element(numa, node);
                node_returned_count++;
                g_free(cpu_list);
            }
            g_free(rel);
        }
        g_ptr_array_free(node_names, TRUE);
    }
    json_object_set_array_member(cpu, "numa_nodes", numa);
    json_object_set_int_member(cpu, "numa_source_count", node_source_count);
    json_object_set_int_member(cpu, "numa_returned_count", node_returned_count);
    json_object_set_boolean_member(cpu, "numa_truncated",
                                   node_source_count > node_returned_count);

    *coverage = logical > 0 ? "available" : "unavailable";
    g_hash_table_unref(packages);
    g_hash_table_unref(cores);
    g_hash_table_unref(thread_counts);
    return cpu;
}

static const gchar *
chassis_type_name(gint64 type)
{
    switch (type & 0x7f) {
    case 3: return "desktop";
    case 4: return "low_profile_desktop";
    case 7: return "tower";
    case 17: return "main_server_chassis";
    case 23: return "rack_mount_chassis";
    case 28: return "blade";
    case 29: return "blade_enclosure";
    default: return "other";
    }
}

static JsonObject *
collect_platform(const gchar *root, const gchar **coverage)
{










    static const struct {
        const gchar *member;
        const gchar *file;
    } fields[] = {
        { "system_vendor", "sys_vendor" },
        { "product_name", "product_name" },
        { "board_vendor", "board_vendor" },
        { "board_name", "board_name" },
        { "board_version", "board_version" },
        { "bios_vendor", "bios_vendor" },
        { "bios_version", "bios_version" },
        { "bios_date", "bios_date" },
    };
    JsonObject *platform = json_object_new();
    guint observed = 0;
    for (guint i = 0; i < G_N_ELEMENTS(fields); i++) {
        gchar *rel = g_strdup_printf("sys/class/dmi/id/%s", fields[i].file);
        gchar *value = NULL;
        if (read_root_text(root, rel, &value)) {
            json_object_set_string_member(platform, fields[i].member, value);
            observed++;
            g_free(value);
        }
        g_free(rel);
    }
    gint64 chassis_type = 0;
    if (read_root_int64(root, "sys/class/dmi/id/chassis_type", &chassis_type)) {
        json_object_set_string_member(platform, "chassis_type", chassis_type_name(chassis_type));
        json_object_set_int_member(platform, "chassis_type_code", chassis_type & 0x7f);
        observed++;
    }
    *coverage = observed > 0 ? "available" : "unavailable";
    return platform;
}

static guint16
read_le16(const guint8 *bytes)
{
    return (guint16)bytes[0] | ((guint16)bytes[1] << 8);
}













static guint32
read_le32(const guint8 *bytes)
{
    return (guint32)bytes[0] | ((guint32)bytes[1] << 8)
        | ((guint32)bytes[2] << 16) | ((guint32)bytes[3] << 24);
}

static gchar *
smbios_string(const guint8 *strings, const guint8 *end, guint index)
{
    if (index == 0)
        return NULL;
    guint current = 1;
    const guint8 *cursor = strings;
    while (cursor < end && *cursor != 0) {
        const guint8 *nul = memchr(cursor, 0, (gsize)(end - cursor));
        if (!nul)
            return NULL;
        if (current == index) {
            gchar *raw = g_strndup((const gchar *)cursor, (gsize)(nul - cursor));
            gchar *cleaned = clean_text(raw);
            g_free(raw);
            return cleaned;
        }
        cursor = nul + 1;
        current++;
    }
    return NULL;
}

static const gchar *
memory_type_name(guint8 type)
{
    switch (type) {
    case 0x0f: return "SDRAM";
    case 0x12: return "DDR";
    case 0x13: return "DDR2";
    case 0x18: return "DDR3";
    case 0x1a: return "DDR4";
    case 0x1b: return "LPDDR";
    case 0x1c: return "LPDDR2";
    case 0x1d: return "LPDDR3";
    case 0x1e: return "LPDDR4";
    case 0x22: return "DDR5";
    case 0x23: return "LPDDR5";
    default: return "unknown";
    }
}

static const gchar *
memory_form_factor_name(guint8 factor)
{
    switch (factor) {
    case 0x08: return "proprietary_card";
    case 0x09: return "DIMM";
    case 0x0a: return "TSOP";
    case 0x0d: return "SODIMM";
    case 0x0f: return "FB-DIMM";
    default: return "other";
    }
}

static void
set_optional_smbios_string(JsonObject *object, const gchar *member,
                           const guint8 *strings, const guint8 *end, guint index)
{
    gchar *value = smbios_string(strings, end, index);
    if (value) {
        json_object_set_string_member(object, member, value);
        g_free(value);
    }
}

static JsonObject *
collect_memory(const gchar *root, const gchar **coverage)
{










    JsonObject *memory = json_object_new();
    JsonArray *devices = json_array_new();
    gchar *path = root_path(root, "sys/firmware/dmi/tables/DMI");
    gchar *contents = NULL;
    gsize length = 0;
    gboolean loaded = g_file_get_contents(path, &contents, &length, NULL)
        && length <= PCV_HW_MAX_SMBIOS_BYTES;
    g_free(path);

    guint source_count = 0;
    guint returned_count = 0;
    guint populated_count = 0;
    gboolean ecc_known = FALSE;
    gboolean ecc_detected = FALSE;
    if (loaded) {
        const guint8 *bytes = (const guint8 *)contents;
        gsize offset = 0;
        while (offset + 4 <= length) {
            guint8 type = bytes[offset];
            guint8 header_length = bytes[offset + 1];
            if (header_length < 4 || offset + header_length > length)
                break;
            gsize string_start = offset + header_length;
            gsize next = string_start;
            while (next + 1 < length
                   && !(bytes[next] == 0 && bytes[next + 1] == 0))
                next++;
            if (next + 1 >= length)
                break;

            if (type == 17 && header_length >= 0x15) {
                source_count++;
                guint16 raw_size = read_le16(bytes + offset + 0x0c);
                gboolean populated = raw_size != 0 && raw_size != 0xffff;
                if (populated)
                    populated_count++;
                if (returned_count < PCV_HW_MAX_MEMORY_DEVICES) {
                    JsonObject *device = json_object_new();
                    json_object_set_boolean_member(device, "populated", populated);
                    const guint8 *strings = bytes + string_start;
                    const guint8 *strings_end = bytes + next;
                    set_optional_smbios_string(device, "device_locator", strings, strings_end,
                                               bytes[offset + 0x10]);
                    set_optional_smbios_string(device, "bank_locator", strings, strings_end,
                                               bytes[offset + 0x11]);
                    json_object_set_string_member(device, "form_factor",
                                                  memory_form_factor_name(bytes[offset + 0x0e]));
                    json_object_set_string_member(device, "memory_type",
                                                  memory_type_name(bytes[offset + 0x12]));

                    if (populated && raw_size != 0xffff) {
                        guint64 size_bytes = 0;
                        if (raw_size == 0x7fff && header_length >= 0x20) {
                            size_bytes = (guint64)read_le32(bytes + offset + 0x1c)
                                * 1024ULL * 1024ULL;
                        } else if ((raw_size & 0x8000) != 0) {
                            size_bytes = (guint64)(raw_size & 0x7fff) * 1024ULL;
                        } else {
                            size_bytes = (guint64)raw_size * 1024ULL * 1024ULL;
                        }
                        if (size_bytes <= G_MAXINT64)
                            json_object_set_int_member(device, "size_bytes", (gint64)size_bytes);
                    }

                    guint16 total_width = read_le16(bytes + offset + 0x08);
                    guint16 data_width = read_le16(bytes + offset + 0x0a);
                    if (total_width != 0xffff && data_width != 0xffff) {
                        json_object_set_int_member(device, "total_width_bits", total_width);
                        json_object_set_int_member(device, "data_width_bits", data_width);
                        if (populated) {
                            gboolean ecc = total_width > data_width;
                            json_object_set_boolean_member(device, "ecc", ecc);
                            ecc_known = TRUE;
                            ecc_detected = ecc_detected || ecc;
                        }
                    }
                    if (header_length >= 0x17) {
                        guint16 speed = read_le16(bytes + offset + 0x15);
                        if (speed != 0 && speed != 0xffff)
                            json_object_set_int_member(device, "speed_mt_s", speed);
                    }
                    if (header_length > 0x17)
                        set_optional_smbios_string(device, "manufacturer", strings, strings_end,
                                                   bytes[offset + 0x17]);

                    if (header_length > 0x1a)
                        set_optional_smbios_string(device, "part_number", strings, strings_end,
                                                   bytes[offset + 0x1a]);
                    if (header_length >= 0x22) {
                        guint16 configured = read_le16(bytes + offset + 0x20);
                        if (configured != 0 && configured != 0xffff)
                            json_object_set_int_member(device, "configured_speed_mt_s", configured);
                    }
                    json_array_add_object_element(devices, device);
                    returned_count++;
                }
            }
            offset = next + 2;
            if (type == 127)
                break;
        }
    }
    g_free(contents);

    json_object_set_int_member(memory, "slot_count", source_count);
    json_object_set_int_member(memory, "populated_slot_count", populated_count);
    if (ecc_known)
        json_object_set_boolean_member(memory, "ecc_detected", ecc_detected);
    json_object_set_array_member(memory, "devices", devices);
    set_count_metadata(memory, source_count, returned_count);
    *coverage = loaded ? "available" : "unavailable";
    return memory;
}

static gboolean
is_hex4_prefix(const gchar *text)
{
    if (!text)
        return FALSE;
    for (guint i = 0; i < 4; i++) {
        if (!g_ascii_isxdigit(text[i]))
            return FALSE;
    }
    return g_ascii_isspace(text[4]);
}

static gchar *
normalized_hex_id(const gchar *raw, guint digits)
{
    if (!raw)
        return NULL;
    const gchar *start = g_str_has_prefix(raw, "0x") ? raw + 2 : raw;
    if (strlen(start) < digits)
        return NULL;
    for (guint i = 0; i < digits; i++) {
        if (!g_ascii_isxdigit(start[i]))
            return NULL;
    }
    return g_ascii_strdown(start, (gssize)digits);
}

static PcvPciIds *
load_pci_ids(const gchar *root)
{










    PcvPciIds *ids = g_new0(PcvPciIds, 1);
    ids->vendors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    ids->devices = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    const gchar *candidates[] = { "usr/share/misc/pci.ids", "usr/share/hwdata/pci.ids" };
    gchar *contents = NULL;
    gsize length = 0;
    for (guint i = 0; i < G_N_ELEMENTS(candidates); i++) {
        gchar *path = root_path(root, candidates[i]);
        gboolean ok = g_file_get_contents(path, &contents, &length, NULL);
        g_free(path);
        if (ok && length <= PCV_HW_MAX_PCI_IDS_BYTES)
            break;
        g_clear_pointer(&contents, g_free);
    }
    if (!contents)
        return ids;

    gchar **lines = g_strsplit(contents, "\n", -1);
    gchar *current_vendor = NULL;
    for (guint i = 0; lines[i] != NULL; i++) {
        const gchar *line = lines[i];
        if (line[0] != '\t' && is_hex4_prefix(line)) {
            g_free(current_vendor);
            current_vendor = g_ascii_strdown(line, 4);
            gchar *name = clean_text(line + 5);
            if (name)
                g_hash_table_replace(ids->vendors, g_strdup(current_vendor), name);
        } else if (line[0] == '\t' && line[1] != '\t'
                   && current_vendor && is_hex4_prefix(line + 1)) {
            gchar *device_id = g_ascii_strdown(line + 1, 4);
            gchar *key = g_strdup_printf("%s:%s", current_vendor, device_id);
            gchar *name = clean_text(line + 6);
            if (name)
                g_hash_table_replace(ids->devices, key, name);
            else
                g_free(key);
            g_free(device_id);
        }
    }
    g_free(current_vendor);
    g_strfreev(lines);
    g_free(contents);
    return ids;
}

static void
free_pci_ids(PcvPciIds *ids)
{
    if (!ids)
        return;
    g_hash_table_unref(ids->vendors);
    g_hash_table_unref(ids->devices);
    g_free(ids);
}

static const gchar *
pci_class_name(const gchar *class_id)
{
    if (!class_id || strlen(class_id) < 2)
        return "unknown";
    gchar major_text[3] = { class_id[0], class_id[1], '\0' };
    guint major = (guint)g_ascii_strtoull(major_text, NULL, 16);
    switch (major) {
    case 0x01: return "mass_storage";
    case 0x02: return "network";
    case 0x03: return "display";
    case 0x04: return "multimedia";
    case 0x05: return "memory";
    case 0x06: return "bridge";
    case 0x07: return "communication";
    case 0x08: return "system_peripheral";
    case 0x09: return "input";
    case 0x0b: return "processor";
    case 0x0c: return "serial_bus";
    case 0x0d: return "wireless";
    case 0x10: return "encryption";
    case 0x11: return "signal_processing";
    case 0x12: return "processing_accelerator";
    case 0x13: return "instrumentation";
    case 0x40: return "coprocessor";
    default: return "other";
    }
}

static gboolean
valid_pci_bdf(const gchar *name)
{
    if (!name || strlen(name) != 12)
        return FALSE;
    static const guint separators[] = { 4, 7, 10 };
    for (guint i = 0; i < 12; i++) {
        if (i == separators[0] || i == separators[1]) {
            if (name[i] != ':') return FALSE;
        } else if (i == separators[2]) {
            if (name[i] != '.') return FALSE;
        } else if (!g_ascii_isxdigit(name[i])) {
            return FALSE;
        }
    }
    return TRUE;
}

static void
set_sysfs_hex_member(JsonObject *object, const gchar *root, const gchar *relative,
                     const gchar *member, guint digits, gchar **normalized_out)
{
    gchar *raw = NULL;
    if (!read_root_text(root, relative, &raw))
        return;
    gchar *normalized = normalized_hex_id(raw, digits);
    g_free(raw);
    if (!normalized)
        return;
    json_object_set_string_member(object, member, normalized);
    if (normalized_out)
        *normalized_out = normalized;
    else
        g_free(normalized);
}

static JsonObject *
collect_pci(const gchar *root, const gchar **coverage)
{











    JsonObject *pci = json_object_new();
    JsonArray *devices = json_array_new();
    gchar *dir_path = root_path(root, "sys/bus/pci/devices");
    GPtrArray *names = sorted_directory_names(dir_path);
    g_free(dir_path);
    PcvPciIds *ids = load_pci_ids(root);
    guint source_count = 0;
    guint returned_count = 0;
    if (names) {
        for (guint i = 0; i < names->len; i++) {
            const gchar *bdf = g_ptr_array_index(names, i);
            if (!valid_pci_bdf(bdf))
                continue;
            source_count++;
            if (returned_count >= PCV_HW_MAX_PCI_DEVICES)
                continue;

            JsonObject *device = json_object_new();
            json_object_set_string_member(device, "bdf", bdf);
            gchar *relative = g_strdup_printf("sys/bus/pci/devices/%s/class", bdf);
            gchar *class_id = NULL;
            set_sysfs_hex_member(device, root, relative, "class_code", 6, &class_id);
            g_free(relative);
            if (class_id)
                json_object_set_string_member(device, "class_name", pci_class_name(class_id));

            gchar *vendor_id = NULL;
            gchar *device_id = NULL;
            relative = g_strdup_printf("sys/bus/pci/devices/%s/vendor", bdf);
            set_sysfs_hex_member(device, root, relative, "vendor_id", 4, &vendor_id);
            g_free(relative);
            relative = g_strdup_printf("sys/bus/pci/devices/%s/device", bdf);
            set_sysfs_hex_member(device, root, relative, "device_id", 4, &device_id);
            g_free(relative);
            relative = g_strdup_printf("sys/bus/pci/devices/%s/subsystem_vendor", bdf);
            set_sysfs_hex_member(device, root, relative, "subsystem_vendor_id", 4, NULL);
            g_free(relative);
            relative = g_strdup_printf("sys/bus/pci/devices/%s/subsystem_device", bdf);
            set_sysfs_hex_member(device, root, relative, "subsystem_device_id", 4, NULL);
            g_free(relative);
            relative = g_strdup_printf("sys/bus/pci/devices/%s/revision", bdf);
            set_sysfs_hex_member(device, root, relative, "revision", 2, NULL);
            g_free(relative);

            if (vendor_id) {
                const gchar *vendor_name = g_hash_table_lookup(ids->vendors, vendor_id);
                if (vendor_name)
                    json_object_set_string_member(device, "vendor_name", vendor_name);
            }
            if (vendor_id && device_id) {
                gchar *key = g_strdup_printf("%s:%s", vendor_id, device_id);
                const gchar *device_name = g_hash_table_lookup(ids->devices, key);
                if (device_name)
                    json_object_set_string_member(device, "device_name", device_name);
                g_free(key);
            }
            g_free(vendor_id);
            g_free(device_id);
            g_free(class_id);

            gchar *driver_path = root_path(root, "sys/bus/pci/devices");
            gchar *driver_link = g_build_filename(driver_path, bdf, "driver", NULL);
            gchar *driver = safe_symlink_basename(driver_link);
            if (driver) {
                json_object_set_string_member(device, "driver", driver);
                g_free(driver);
            }
            gchar *iommu_link = g_build_filename(driver_path, bdf, "iommu_group", NULL);
            gchar *iommu = safe_symlink_basename(iommu_link);
            if (iommu && ascii_digits_only(iommu))
                json_object_set_int_member(device, "iommu_group",
                                           g_ascii_strtoll(iommu, NULL, 10));
            g_free(iommu);
            g_free(iommu_link);
            g_free(driver_link);
            g_free(driver_path);

            json_array_add_object_element(devices, device);
            returned_count++;
        }
        g_ptr_array_free(names, TRUE);
    }
    free_pci_ids(ids);
    json_object_set_array_member(pci, "devices", devices);
    set_count_metadata(pci, source_count, returned_count);
    *coverage = names ? "available" : "unavailable";
    return pci;
}

static gboolean
valid_nvme_controller_name(const gchar *name)
{
    return g_str_has_prefix(name, "nvme") && ascii_digits_only(name + 4);
}

static JsonObject *
collect_nvme(const gchar *root, const gchar **coverage)
{










    JsonObject *nvme = json_object_new();
    JsonArray *devices = json_array_new();
    gchar *dir_path = root_path(root, "sys/class/nvme");
    GPtrArray *names = sorted_directory_names(dir_path);
    g_free(dir_path);
    gchar *block_path = root_path(root, "sys/block");
    GPtrArray *block_names = sorted_directory_names(block_path);
    g_free(block_path);
    guint source_count = 0;
    guint returned_count = 0;
    if (names) {
        for (guint i = 0; i < names->len; i++) {
            const gchar *name = g_ptr_array_index(names, i);
            if (!valid_nvme_controller_name(name))
                continue;
            source_count++;
            if (returned_count >= PCV_HW_MAX_NVME_DEVICES)
                continue;
            JsonObject *device = json_object_new();
            json_object_set_string_member(device, "id", name);
            const struct { const gchar *member; const gchar *file; } fields[] = {
                { "model", "model" }, { "firmware", "firmware_rev" }, { "state", "state" },
            };
            for (guint f = 0; f < G_N_ELEMENTS(fields); f++) {
                gchar *rel = g_strdup_printf("sys/class/nvme/%s/%s", name, fields[f].file);
                gchar *value = NULL;
                if (read_root_text(root, rel, &value)) {
                    json_object_set_string_member(device, fields[f].member, value);
                    g_free(value);
                }
                g_free(rel);
            }

            guint64 capacity = 0;
            if (block_names) {
                for (guint b = 0; b < block_names->len; b++) {
                    const gchar *block = g_ptr_array_index(block_names, b);
                    gsize prefix_len = strlen(name);
                    if (!g_str_has_prefix(block, name) || block[prefix_len] != 'n'
                        || !ascii_digits_only(block + prefix_len + 1))
                        continue;
                    gchar *rel = g_strdup_printf("sys/block/%s/size", block);
                    gint64 sectors = 0;
                    if (read_root_int64(root, rel, &sectors) && sectors > 0
                        && (guint64)sectors <= G_MAXUINT64 / 512ULL)
                        capacity += (guint64)sectors * 512ULL;
                    g_free(rel);
                }
            }
            if (capacity > 0 && capacity <= G_MAXINT64)
                json_object_set_int_member(device, "capacity_bytes", (gint64)capacity);
            JsonObject *health = json_object_new();
            if (json_object_has_member(device, "state"))
                json_object_set_string_member(health, "controller_state",
                    json_object_get_string_member(device, "state"));
            json_object_set_boolean_member(health, "smart_available", FALSE);
            json_object_set_string_member(health, "smart_status", "not_exposed_by_sysfs_v1");
            json_object_set_object_member(device, "health", health);
            json_array_add_object_element(devices, device);
            returned_count++;
        }
        g_ptr_array_free(names, TRUE);
    }
    if (block_names)
        g_ptr_array_free(block_names, TRUE);
    json_object_set_array_member(nvme, "devices", devices);
    set_count_metadata(nvme, source_count, returned_count);
    *coverage = names ? (source_count > 0 ? "partial" : "available") : "unavailable";
    return nvme;
}

static gboolean
read_scaled_channel(const gchar *root, const gchar *relative, gdouble scale,
                    gdouble minimum, gdouble maximum, gdouble *out)
{
    gint64 raw = 0;
    if (!read_root_int64(root, relative, &raw))
        return FALSE;
    gdouble value = (gdouble)raw / scale;
    if (!isfinite(value) || value < minimum || value > maximum)
        return FALSE;
    *out = value;
    return TRUE;
}













static void
set_optional_scaled(JsonObject *object, const gchar *root, const gchar *base,
                    const gchar *file, const gchar *member, gdouble scale,
                    gdouble minimum, gdouble maximum)
{
    gchar *rel = g_strdup_printf("%s/%s", base, file);
    gdouble value = 0;
    if (read_scaled_channel(root, rel, scale, minimum, maximum, &value))
        json_object_set_double_member(object, member, value);
    g_free(rel);
}



static void
set_optional_accuracy(JsonObject *object, const gchar *root, const gchar *base,
                      const gchar *file)
{
    gchar *rel = g_strdup_printf("%s/%s", base, file);
    gchar *path = root_path(root, rel);
    gchar *text = NULL;
    if (read_text_file(path, 96, &text)) {
        g_strstrip(text);
        gchar *percent = strchr(text, '%');
        if (percent)
            *percent = '\0';
        errno = 0;
        gchar *end = NULL;
        gdouble value = g_ascii_strtod(text, &end);
        while (end && g_ascii_isspace(*end))
            end++;
        if (errno == 0 && end && *end == '\0' && isfinite(value)
            && value >= 0.0 && value <= 100.0)
            json_object_set_double_member(object, "accuracy_percent", value);
    }
    g_free(text);
    g_free(path);
    g_free(rel);
}

static gchar *
hwmon_device_ref(const gchar *root, const gchar *hwmon_name, const gchar *chip)
{
    gchar *base = root_path(root, "sys/class/hwmon");
    gchar *link = g_build_filename(base, hwmon_name, "device", NULL);
    gchar *ref = safe_symlink_basename(link);
    g_free(link);
    g_free(base);
    return ref ? ref : g_strdup(chip);
}

static JsonObject *
collect_sensors(const gchar *root, const gchar **coverage)
{










    JsonObject *sensors = json_object_new();
    JsonArray *items = json_array_new();
    gchar *dir_path = root_path(root, "sys/class/hwmon");
    GPtrArray *names = sorted_directory_names(dir_path);
    g_free(dir_path);
    guint source_count = 0;
    guint returned_count = 0;
    if (names) {
        for (guint i = 0; i < names->len; i++) {
            const gchar *hwmon = g_ptr_array_index(names, i);
            gchar *name_rel = g_strdup_printf("sys/class/hwmon/%s/name", hwmon);
            gchar *chip = NULL;
            gboolean has_chip = read_root_text(root, name_rel, &chip);
            g_free(name_rel);
            if (!has_chip)
                continue;
            gchar *device_ref = hwmon_device_ref(root, hwmon, chip);
            gchar *base = g_strdup_printf("sys/class/hwmon/%s", hwmon);

            for (guint channel = 1; channel <= 64; channel++) {
                gchar *input = g_strdup_printf("temp%u_input", channel);
                gchar *input_rel = g_strdup_printf("%s/%s", base, input);
                gdouble value_c = 0;
                gboolean readable = read_scaled_channel(root, input_rel, 1000.0,
                                                        -100.0, 300.0, &value_c);
                g_free(input_rel);
                if (!readable) {
                    g_free(input);
                    continue;
                }
                source_count++;
                if (returned_count < PCV_HW_MAX_SENSORS) {
                    JsonObject *sensor = json_object_new();
                    gchar *id = g_strdup_printf("%s@%s/temp%u", chip, device_ref, channel);
                    json_object_set_string_member(sensor, "id", id);
                    json_object_set_string_member(sensor, "chip", chip);
                    json_object_set_string_member(sensor, "device", device_ref);
                    json_object_set_string_member(sensor, "kind", "temperature");
                    json_object_set_double_member(sensor, "value_c", value_c);
                    gchar *label_file = g_strdup_printf("temp%u_label", channel);
                    gchar *label_rel = g_strdup_printf("%s/%s", base, label_file);
                    gchar *label = NULL;
                    if (read_root_text(root, label_rel, &label)) {
                        json_object_set_string_member(sensor, "label", label);
                        g_free(label);
                    } else {
                        json_object_set_string_member(sensor, "label", input);
                    }
                    g_free(label_rel);
                    g_free(label_file);
                    gchar *max_file = g_strdup_printf("temp%u_max", channel);
                    gchar *crit_file = g_strdup_printf("temp%u_crit", channel);
                    set_optional_scaled(sensor, root, base, max_file, "maximum_c",
                                        1000.0, -100.0, 300.0);
                    set_optional_scaled(sensor, root, base, crit_file, "critical_c",
                                        1000.0, -100.0, 300.0);
                    g_free(max_file);
                    g_free(crit_file);
                    json_array_add_object_element(items, sensor);
                    returned_count++;
                    g_free(id);
                }
                g_free(input);
            }

            for (guint channel = 1; channel <= 32; channel++) {
                gchar *input_file = g_strdup_printf("power%u_input", channel);
                gchar *average_file = g_strdup_printf("power%u_average", channel);
                gchar *input_rel = g_strdup_printf("%s/%s", base, input_file);
                gchar *average_rel = g_strdup_printf("%s/%s", base, average_file);
                gdouble value_w = 0;
                const gchar *measurement = "input";
                gboolean readable = read_scaled_channel(root, input_rel, 1000000.0,
                                                        0.0, 10000000.0, &value_w);
                if (!readable) {
                    readable = read_scaled_channel(root, average_rel, 1000000.0,
                                                   0.0, 10000000.0, &value_w);
                    measurement = "average";
                }
                g_free(input_rel);
                g_free(average_rel);
                if (!readable) {
                    g_free(input_file);
                    g_free(average_file);
                    continue;
                }
                source_count++;
                if (returned_count < PCV_HW_MAX_SENSORS) {
                    JsonObject *sensor = json_object_new();
                    gchar *id = g_strdup_printf("%s@%s/power%u", chip, device_ref, channel);
                    json_object_set_string_member(sensor, "id", id);
                    json_object_set_string_member(sensor, "chip", chip);
                    json_object_set_string_member(sensor, "device", device_ref);
                    json_object_set_string_member(sensor, "kind", "power");
                    json_object_set_string_member(sensor, "measurement", measurement);
                    json_object_set_double_member(sensor, "value_w", value_w);
                    gchar *label_file = g_strdup_printf("power%u_label", channel);
                    gchar *label_rel = g_strdup_printf("%s/%s", base, label_file);
                    gchar *label = NULL;
                    if (read_root_text(root, label_rel, &label)) {
                        json_object_set_string_member(sensor, "label", label);
                        g_free(label);
                    } else {
                        json_object_set_string_member(sensor, "label", input_file);
                    }
                    g_free(label_rel);
                    g_free(label_file);
                    gchar *crit_file = g_strdup_printf("power%u_crit", channel);
                    gchar *cap_file = g_strdup_printf("power%u_cap", channel);
                    gchar *accuracy_file = g_strdup_printf("power%u_accuracy", channel);
                    set_optional_scaled(sensor, root, base, crit_file, "critical_w",
                                        1000000.0, 0.0, 10000000.0);
                    set_optional_scaled(sensor, root, base, cap_file, "cap_w",
                                        1000000.0, 0.0, 10000000.0);
                    set_optional_accuracy(sensor, root, base, accuracy_file);
                    g_free(crit_file);
                    g_free(cap_file);
                    g_free(accuracy_file);
                    json_array_add_object_element(items, sensor);
                    returned_count++;
                    g_free(id);
                }
                g_free(input_file);
                g_free(average_file);
            }
            g_free(base);
            g_free(device_ref);
            g_free(chip);
        }
        g_ptr_array_free(names, TRUE);
    }
    json_object_set_array_member(sensors, "items", items);
    set_count_metadata(sensors, source_count, returned_count);
    *coverage = names ? "available" : "unavailable";
    return sensors;
}

static JsonObject *
collect_network(const gchar *root, const gchar **coverage)
{










    JsonObject *network = json_object_new();
    JsonArray *interfaces = json_array_new();
    gchar *dir_path = root_path(root, "sys/class/net");
    GPtrArray *names = sorted_directory_names(dir_path);
    g_free(dir_path);
    guint source_count = 0;
    guint returned_count = 0;
    if (names) {
        for (guint i = 0; i < names->len; i++) {
            const gchar *name = g_ptr_array_index(names, i);
            gchar *device_base = root_path(root, "sys/class/net");
            gchar *device_link = g_build_filename(device_base, name, "device", NULL);
            gchar *device_ref = safe_symlink_basename(device_link);
            g_free(device_link);
            g_free(device_base);
            if (!device_ref)
                continue;
            source_count++;
            if (returned_count >= PCV_HW_MAX_NETWORK_INTERFACES) {
                g_free(device_ref);
                continue;
            }
            JsonObject *interface = json_object_new();
            json_object_set_string_member(interface, "name", name);
            if (valid_pci_bdf(device_ref))
                json_object_set_string_member(interface, "pci_bdf", device_ref);
            gchar *relative = g_strdup_printf("sys/class/net/%s/operstate", name);
            gchar *state = NULL;
            if (read_root_text(root, relative, &state)) {
                json_object_set_string_member(interface, "state", state);
                g_free(state);
            }
            g_free(relative);
            gint64 value = 0;
            relative = g_strdup_printf("sys/class/net/%s/carrier", name);
            if (read_root_int64(root, relative, &value) && (value == 0 || value == 1))
                json_object_set_boolean_member(interface, "carrier", value == 1);
            g_free(relative);
            relative = g_strdup_printf("sys/class/net/%s/speed", name);
            if (read_root_int64(root, relative, &value) && value > 0 && value <= 10000000)
                json_object_set_int_member(interface, "speed_mbps", value);
            g_free(relative);
            relative = g_strdup_printf("sys/class/net/%s/duplex", name);
            gchar *duplex = NULL;
            if (read_root_text(root, relative, &duplex)) {
                gchar *lower = g_ascii_strdown(duplex, -1);
                if (g_strcmp0(lower, "full") == 0 || g_strcmp0(lower, "half") == 0)
                    json_object_set_string_member(interface, "duplex", lower);
                g_free(lower);
                g_free(duplex);
            }
            g_free(relative);
            device_base = root_path(root, "sys/class/net");
            gchar *driver_link = g_build_filename(device_base, name, "device", "driver", NULL);
            gchar *driver = safe_symlink_basename(driver_link);
            if (driver) {
                json_object_set_string_member(interface, "driver", driver);
                g_free(driver);
            }
            g_free(driver_link);
            g_free(device_base);
            g_free(device_ref);
            json_array_add_object_element(interfaces, interface);
            returned_count++;
        }
        g_ptr_array_free(names, TRUE);
    }
    json_object_set_array_member(network, "interfaces", interfaces);
    set_count_metadata(network, source_count, returned_count);
    *coverage = names ? "available" : "unavailable";
    return network;
}

JsonObject *
pcv_host_hardware_inventory_collect(const gchar *root)
{











    const gchar *cpu_coverage = "unavailable";
    const gchar *platform_coverage = "unavailable";
    const gchar *memory_coverage = "unavailable";
    const gchar *pci_coverage = "unavailable";
    const gchar *nvme_coverage = "unavailable";
    const gchar *sensor_coverage = "unavailable";
    const gchar *network_coverage = "unavailable";

    JsonObject *inventory = json_object_new();
    json_object_set_int_member(inventory, "schema_version", 1);
    json_object_set_string_member(inventory, "privacy_profile", "viewer-safe-v1");
    json_object_set_int_member(inventory, "collected_at_unix_ms", g_get_real_time() / 1000);

    JsonObject *limits = json_object_new();
    json_object_set_int_member(limits, "logical_processors", PCV_HW_MAX_LOGICAL_CPUS);
    json_object_set_int_member(limits, "numa_nodes", PCV_HW_MAX_NUMA_NODES);
    json_object_set_int_member(limits, "memory_devices", PCV_HW_MAX_MEMORY_DEVICES);
    json_object_set_int_member(limits, "pci_devices", PCV_HW_MAX_PCI_DEVICES);
    json_object_set_int_member(limits, "nvme_devices", PCV_HW_MAX_NVME_DEVICES);
    json_object_set_int_member(limits, "sensors", PCV_HW_MAX_SENSORS);
    json_object_set_int_member(limits, "network_interfaces", PCV_HW_MAX_NETWORK_INTERFACES);
    json_object_set_object_member(inventory, "limits", limits);

    json_object_set_object_member(inventory, "cpu_topology",
                                  collect_cpu_topology(root, &cpu_coverage));
    json_object_set_object_member(inventory, "platform",
                                  collect_platform(root, &platform_coverage));
    json_object_set_object_member(inventory, "memory",
                                  collect_memory(root, &memory_coverage));
    json_object_set_object_member(inventory, "pci", collect_pci(root, &pci_coverage));
    json_object_set_object_member(inventory, "nvme", collect_nvme(root, &nvme_coverage));
    json_object_set_object_member(inventory, "sensors",
                                  collect_sensors(root, &sensor_coverage));
    json_object_set_object_member(inventory, "network",
                                  collect_network(root, &network_coverage));

    JsonObject *coverage = json_object_new();
    json_object_set_string_member(coverage, "cpu_topology", cpu_coverage);
    json_object_set_string_member(coverage, "platform", platform_coverage);
    json_object_set_string_member(coverage, "memory", memory_coverage);
    json_object_set_string_member(coverage, "pci", pci_coverage);
    json_object_set_string_member(coverage, "nvme", nvme_coverage);
    json_object_set_string_member(coverage, "sensors", sensor_coverage);
    json_object_set_string_member(coverage, "network", network_coverage);
    json_object_set_object_member(inventory, "coverage", coverage);
    return inventory;
}
