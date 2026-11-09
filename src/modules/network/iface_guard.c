   
                      
                                                                      
  
                           
                                                                            
                                                                            
                                                          
                                                                  
                 
  
                                                           
                                           
   
#include "modules/network/iface_guard.h"
#include "modules/network/network_manager.h"
#include "../../utils/pcv_validate.h"
#include <errno.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>

                                                                
                                                     
typedef gboolean (*PcvIfaceAddrProbe)(const gchar *iface, gboolean *has_ipv4,
                                      gboolean *has_v6_global, gpointer user_data);
static PcvIfaceAddrProbe g_addr_probe;
static gpointer g_addr_probe_data;

void pcv_iface_guard_test_set_addr_probe(PcvIfaceAddrProbe probe, gpointer user_data);

void
pcv_iface_guard_test_set_addr_probe(PcvIfaceAddrProbe probe, gpointer user_data)
{
    g_addr_probe = probe;
    g_addr_probe_data = user_data;
}

                                                            
static GPtrArray *_route_fields(const gchar *line)
{
    GPtrArray *fields = g_ptr_array_new_with_free_func(g_free);
    gchar **raw = g_strsplit_set(line ? line : "", " \t\r", -1);
    for (guint i = 0; raw[i]; i++) {
        if (*raw[i]) g_ptr_array_add(fields, g_strdup(raw[i]));
    }
    g_strfreev(raw);
    return fields;
}

                                                                      
                                                      
                                                                                  
                                                                  
                                                                        
gboolean
pcv_iface_guard_route_has_default(const gchar *path, const gchar *iface, gboolean ipv6,
                                  gboolean *has_default, GError **error)
{
    gchar *contents = NULL;
    GError *read_error = NULL;
    *has_default = FALSE;
    if (!g_file_get_contents(path, &contents, NULL, &read_error)) {
        if (ipv6 && g_error_matches(read_error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            g_error_free(read_error);
            return TRUE;
        }
        g_propagate_error(error, read_error);
        return FALSE;
    }

    gchar **lines = g_strsplit(contents, "\n", -1);
    for (guint i = 0; lines[i]; i++) {
        GPtrArray *f = _route_fields(lines[i]);
        if (!ipv6 && f->len >= 2) {
            const gchar *row_iface = g_ptr_array_index(f, 0);
            const gchar *dest = g_ptr_array_index(f, 1);
            if (g_strcmp0(row_iface, iface) == 0 && g_strcmp0(dest, "00000000") == 0)
                *has_default = TRUE;
        } else if (ipv6 && f->len >= 3) {
            const gchar *dest = g_ptr_array_index(f, 0);
            const gchar *prefix = g_ptr_array_index(f, 1);
            const gchar *row_iface = g_ptr_array_index(f, f->len - 1);
            if (g_strcmp0(row_iface, iface) == 0
                && g_strcmp0(dest, "00000000000000000000000000000000") == 0
                && g_strcmp0(prefix, "00") == 0)
                *has_default = TRUE;
        }
        g_ptr_array_unref(f);
        if (*has_default) break;
    }
    g_strfreev(lines);
    g_free(contents);
    return TRUE;
}

gboolean
pcv_iface_guard_master_read(const gchar *sysfs_net_root, const gchar *iface,
                            gchar **master_out, GError **error)
{
    *master_out = NULL;
    gchar *path = g_build_filename(sysfs_net_root, iface, "master", NULL);
    GStatBuf st = {0};
    if (g_lstat(path, &st) != 0) {
        gint saved_errno = errno;
        g_free(path);
        if (saved_errno == ENOENT) return TRUE;
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved_errno),
                    "cannot inspect interface '%s' master: %s",
                    iface, g_strerror(saved_errno));
        return FALSE;
    }
    gchar *link = g_file_read_link(path, error);
    if (!link) {
        g_free(path);
        return FALSE;
    }
    *master_out = g_path_get_basename(link);
    g_free(link);
    g_free(path);
    return TRUE;
}

                                                                    
                                                                      
                                                                   
                                                  
static gboolean
_iface_upper_check(const gchar *sysroot, const gchar *iface,
                   const gchar *expected_master, GError **error)
{
    gchar *dir_path = g_build_filename(sysroot, iface, NULL);
    GDir *dir = g_dir_open(dir_path, 0, error);
    g_free(dir_path);
    if (!dir) return FALSE;
    gboolean ok = TRUE;
    const gchar *name;
    while (ok && (name = g_dir_read_name(dir))) {
        if (!g_str_has_prefix(name, "upper_")) continue;
        const gchar *upper = name + strlen("upper_");
        if (g_strcmp0(upper, expected_master) == 0) continue;
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "interface '%s' carries upper device '%s' (VLAN, macvlan, bond, or bridge)",
                    iface, upper);
        ok = FALSE;
    }
    g_dir_close(dir);
    return ok;
}

                                                           
static gboolean
_iface_addr_inventory(const gchar *iface, gboolean *has_ipv4, gboolean *has_v6_global)
{
    struct ifaddrs *ifaddr = NULL;
    if (getifaddrs(&ifaddr) != 0) return FALSE;
    for (struct ifaddrs *it = ifaddr; it; it = it->ifa_next) {
        if (!it->ifa_addr || g_strcmp0(it->ifa_name, iface) != 0) continue;
        int family = it->ifa_addr->sa_family;
        if (family == AF_INET) *has_ipv4 = TRUE;
        if (family == AF_INET6) {
            const struct sockaddr_in6 *sa6 = (const struct sockaddr_in6 *)it->ifa_addr;
            if (!IN6_IS_ADDR_LINKLOCAL(&sa6->sin6_addr)) *has_v6_global = TRUE;
        }
    }
    freeifaddrs(ifaddr);
    return TRUE;
}

gboolean
pcv_iface_guard_check_dedicated(const gchar *physical_if,
                                const gchar *expected_master,
                                const gchar *sysfs_net_root,
                                const gchar *proc_root,
                                gboolean *was_up,
                                GError **error)
{
    const gchar *sysroot = sysfs_net_root ? sysfs_net_root : "/sys/class/net";
    const gchar *procroot = proc_root ? proc_root : "/proc";
    gboolean real_host_view = !sysfs_net_root && !proc_root;
    if (was_up) *was_up = FALSE;

    if (!pcv_validate_iface_name(physical_if) || g_strcmp0(physical_if, "lo") == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "physical_if must be a non-loopback interface");
        return FALSE;
    }

    gchar *iface_path = g_build_filename(sysroot, physical_if, NULL);
    gchar *type_path = g_build_filename(iface_path, "type", NULL);
    gchar *flags_path = g_build_filename(iface_path, "flags", NULL);
    gchar *device_path = g_build_filename(iface_path, "device", NULL);
    gchar *wireless_path = g_build_filename(iface_path, "wireless", NULL);
    gchar *bridge_path = g_build_filename(iface_path, "bridge", NULL);
    gchar *bonding_path = g_build_filename(iface_path, "bonding", NULL);
    gchar *vlan_path = g_build_filename(procroot, "net", "vlan", physical_if, NULL);
    gboolean ok = FALSE;
    gchar *contents = NULL;

    if (!g_file_test(iface_path, G_FILE_TEST_IS_DIR)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "physical interface '%s' does not exist", physical_if);
        goto out;
    }
    if (!g_file_get_contents(type_path, &contents, NULL, error)) goto out;
    g_strstrip(contents);
    if (g_strcmp0(contents, "1") != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "interface '%s' is not an Ethernet uplink", physical_if);
        goto out;
    }
    g_clear_pointer(&contents, g_free);
    if (!g_file_test(device_path, G_FILE_TEST_EXISTS)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "interface '%s' is not a physical device", physical_if);
        goto out;
    }
    if (g_file_test(wireless_path, G_FILE_TEST_EXISTS)
        || g_file_test(bridge_path, G_FILE_TEST_EXISTS)
        || g_file_test(bonding_path, G_FILE_TEST_EXISTS)
        || g_file_test(vlan_path, G_FILE_TEST_EXISTS)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "interface '%s' is wireless, bonded, VLAN, or already a bridge",
                    physical_if);
        goto out;
    }
    gchar *master = NULL;
    if (!pcv_iface_guard_master_read(sysroot, physical_if, &master, error)) goto out;
    if (master && g_strcmp0(master, expected_master) != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "interface '%s' already belongs to master '%s'", physical_if, master);
        g_free(master);
        goto out;
    }
    g_free(master);
    if (!_iface_upper_check(sysroot, physical_if, expected_master, error)) goto out;

    if (!g_file_get_contents(flags_path, &contents, NULL, error)) goto out;
    gchar *end = NULL;
    guint64 flags = g_ascii_strtoull(contents, &end, 0);
    if (end == contents) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "cannot determine administrative state of '%s'", physical_if);
        goto out;
    }
    if (was_up) *was_up = (flags & IFF_UP) != 0;
    g_clear_pointer(&contents, g_free);

    gchar *route4 = g_build_filename(procroot, "net", "route", NULL);
    gchar *route6 = g_build_filename(procroot, "net", "ipv6_route", NULL);
    gboolean has_default = FALSE;
    if (!pcv_iface_guard_route_has_default(route4, physical_if, FALSE, &has_default, error)) {
        g_free(route4); g_free(route6); goto out;
    }
    if (has_default) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "interface '%s' owns the IPv4 default route and may be the management NIC",
                    physical_if);
        g_free(route4); g_free(route6); goto out;
    }
    if (!pcv_iface_guard_route_has_default(route6, physical_if, TRUE, &has_default, error)) {
        g_free(route4); g_free(route6); goto out;
    }
    g_free(route4); g_free(route6);
    if (has_default) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "interface '%s' owns the IPv6 default route and may be the management NIC",
                    physical_if);
        goto out;
    }

                                                                                 
                                                             
    if (real_host_view || g_addr_probe) {
        gboolean has_ipv4 = FALSE, has_v6_global = FALSE;
        gboolean known = g_addr_probe
            ? g_addr_probe(physical_if, &has_ipv4, &has_v6_global, g_addr_probe_data)
            : _iface_addr_inventory(physical_if, &has_ipv4, &has_v6_global);
        if (!pcv_network_iface_address_facts_dedicated(
                physical_if, known, has_ipv4, has_v6_global, error))
            goto out;
    }

    ok = TRUE;
out:
    g_free(contents);
    g_free(iface_path);
    g_free(type_path);
    g_free(flags_path);
    g_free(device_path);
    g_free(wireless_path);
    g_free(bridge_path);
    g_free(bonding_path);
    g_free(vlan_path);
    return ok;
}

gboolean
pcv_network_iface_address_facts_dedicated(const gchar *physical_if,
                                          gboolean inventory_known,
                                          gboolean has_ipv4,
                                          gboolean has_non_link_local_ipv6,
                                          GError **error)
{
    if (!inventory_known) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "cannot inspect addresses on interface '%s'", physical_if);
        return FALSE;
    }
    if (has_ipv4) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "interface '%s' has a host IPv4 address; dedicated uplink required",
                    physical_if);
        return FALSE;
    }
    if (has_non_link_local_ipv6) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "interface '%s' has a non-link-local host IPv6 address",
                    physical_if);
        return FALSE;
    }
    return TRUE;
}

gboolean
pcv_network_iface_preflight_dedicated(const gchar *physical_if,
                                      const gchar *sysfs_net_root,
                                      const gchar *proc_root,
                                      gboolean *was_up,
                                      GError **error)
{
    return pcv_iface_guard_check_dedicated(physical_if, NULL, sysfs_net_root, proc_root,
                                      was_up, error);
}
