
#include "pcv_validate.h"
#include <gio/gio.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static gboolean is_safe_token(const gchar *s, gsize max_len) {
    if (!s || *s == '\0') return FALSE;
    gsize len = strlen(s);
    if (len > max_len) return FALSE;
    for (const gchar *p = s; *p; p++) {
        if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_')
            return FALSE;
    }
    return TRUE;
}

static gboolean
is_ipv4_literal(const gchar *host)
{
    gchar **parts = g_strsplit(host, ".", 5);
    if (!parts || g_strv_length(parts) != 4) {
        g_strfreev(parts);
        return FALSE;
    }

    for (guint i = 0; i < 4; i++) {
        gsize len = strlen(parts[i]);
        if (len == 0 || len > 3) {
            g_strfreev(parts);
            return FALSE;
        }
        for (const gchar *p = parts[i]; *p; p++) {
            if (!g_ascii_isdigit(*p)) {
                g_strfreev(parts);
                return FALSE;
            }
        }
        gint value = atoi(parts[i]);
        if (value < 0 || value > 255) {
            g_strfreev(parts);
            return FALSE;
        }
    }

    g_strfreev(parts);
    return TRUE;
}

gboolean pcv_validate_vm_name(const gchar *name) {
    return is_safe_token(name, PCV_MAX_VM_NAME);
}

gboolean pcv_validate_snap_name(const gchar *name) {
    return is_safe_token(name, PCV_MAX_SNAP_NAME);
}

gboolean pcv_validate_bridge_name(const gchar *name) {
    return is_safe_token(name, PCV_MAX_BRIDGE_NAME);
}

gboolean
pcv_validate_remote_host(const gchar *host)
{
    if (!host || *host == '\0') return FALSE;

    gsize len = strlen(host);
    if (len > PCV_MAX_REMOTE_HOST) return FALSE;
    if (host[0] == '-' || host[0] == '.') return FALSE;
    if (g_str_has_suffix(host, ".")) return FALSE;

    gboolean digit_dot_only = TRUE;
    for (const gchar *p = host; *p; p++) {
        if (!g_ascii_isalnum(*p) && *p != '-' && *p != '.')
            return FALSE;
        if (!g_ascii_isdigit(*p) && *p != '.')
            digit_dot_only = FALSE;
    }

    if (digit_dot_only)
        return is_ipv4_literal(host);

    gchar **labels = g_strsplit(host, ".", -1);
    if (!labels) return FALSE;

    gboolean ok = TRUE;
    for (guint i = 0; labels[i]; i++) {
        const gchar *label = labels[i];
        gsize label_len = strlen(label);
        if (label_len == 0 || label_len > 63) {
            ok = FALSE;
            break;
        }
        if (!g_ascii_isalnum(label[0]) ||
            !g_ascii_isalnum(label[label_len - 1])) {
            ok = FALSE;
            break;
        }
        for (const gchar *p = label; *p; p++) {
            if (!g_ascii_isalnum(*p) && *p != '-') {
                ok = FALSE;
                break;
            }
        }
        if (!ok) break;
    }

    g_strfreev(labels);
    return ok;
}

gboolean
pcv_validate_ssh_user(const gchar *user)
{
    if (!user || *user == '\0') return FALSE;

    gsize len = strlen(user);
    if (len > PCV_MAX_SSH_USER) return FALSE;
    if (user[0] == '-') return FALSE;
    if (!g_ascii_isalnum(user[0]) && user[0] != '_') return FALSE;

    for (const gchar *p = user; *p; p++) {
        if (!g_ascii_isalnum(*p) && *p != '_' && *p != '.' && *p != '-')
            return FALSE;
    }

    return TRUE;
}

gboolean pcv_validate_iso_path(const gchar *path) {
    if (!path || *path == '\0') return FALSE;
    if (strlen(path) > PCV_MAX_ISO_PATH) return FALSE;

    if (path[0] != '/') return FALSE;

    if (strstr(path, "/../") != NULL) return FALSE;
    if (g_str_has_suffix(path, "/.."))  return FALSE;
    if (g_strcmp0(path, "..") == 0)     return FALSE;

    gchar *lower = g_ascii_strdown(path, -1);
    gboolean ext_ok = g_str_has_suffix(lower, ".iso") ||
                      g_str_has_suffix(lower, ".img");
    g_free(lower);
    if (!ext_ok) return FALSE;

    return TRUE;
}

gboolean pcv_validate_base_image_path(const gchar *path) {
    if (!path || *path == '\0') return FALSE;
    if (strlen(path) > PCV_MAX_ISO_PATH) return FALSE;

    if (path[0] != '/') return FALSE;

    if (strstr(path, "/../") != NULL) return FALSE;
    if (g_str_has_suffix(path, "/.."))  return FALSE;
    if (g_strcmp0(path, "..") == 0)     return FALSE;

    gchar *lower = g_ascii_strdown(path, -1);
    gboolean ext_ok = g_str_has_suffix(lower, ".qcow2") ||
                      g_str_has_suffix(lower, ".qcow")  ||
                      g_str_has_suffix(lower, ".img")   ||
                      g_str_has_suffix(lower, ".raw");
    g_free(lower);
    if (!ext_ok) return FALSE;

    return TRUE;
}

gboolean pcv_validate_memory_mb(gint64 mb) {
    return mb >= PCV_MIN_MEMORY_MB && mb <= PCV_MAX_MEMORY_MB;
}

gboolean pcv_validate_vcpu(gint64 count) {
    return count >= PCV_MIN_VCPU && count <= PCV_MAX_VCPU;
}

gboolean pcv_validate_disk_gb(gint64 gb) {
    return gb >= PCV_MIN_DISK_GB && gb <= PCV_MAX_DISK_GB;
}

gboolean pcv_validate_vm_create_params(const gchar  *name,
                                       gint64        vcpu,
                                       gint64        memory_mb,
                                       gint64        disk_gb,
                                       const gchar  *iso_path,
                                       const gchar  *bridge,
                                       GError      **error) {
    if (!pcv_validate_vm_name(name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Invalid VM name '%s': must be 1-%d chars [a-zA-Z0-9_-]",
            name ? name : "(null)", PCV_MAX_VM_NAME);
        return FALSE;
    }
    if (!pcv_validate_vcpu(vcpu)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Invalid vcpu %" G_GINT64_FORMAT ": must be %d-%d",
            vcpu, PCV_MIN_VCPU, PCV_MAX_VCPU);
        return FALSE;
    }
    if (!pcv_validate_memory_mb(memory_mb)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Invalid memory_mb %" G_GINT64_FORMAT ": must be %d-%d",
            memory_mb, PCV_MIN_MEMORY_MB, PCV_MAX_MEMORY_MB);
        return FALSE;
    }
    if (!pcv_validate_disk_gb(disk_gb)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Invalid disk_size_gb %" G_GINT64_FORMAT ": must be %d-%d",
            disk_gb, PCV_MIN_DISK_GB, PCV_MAX_DISK_GB);
        return FALSE;
    }

    if (iso_path && !pcv_validate_iso_path(iso_path)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Invalid iso_path '%s': must be absolute path without '..'", iso_path);
        return FALSE;
    }

    if (bridge && !pcv_validate_bridge_name(bridge)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Invalid network_bridge '%s': must be 1-%d chars [a-zA-Z0-9_-]",
            bridge, PCV_MAX_BRIDGE_NAME);
        return FALSE;
    }
    return TRUE;
}

gboolean
pcv_validate_container_image(const gchar *image)
{
    if (!image) return FALSE;

    gsize len = strlen(image);
    if (len == 0 || len > PCV_MAX_CONTAINER_IMAGE) return FALSE;

    const gchar *colon = strchr(image, ':');
    if (!colon || colon == image) return FALSE;
    if (*(colon + 1) == '\0') return FALSE;

    for (const gchar *p = image; p < colon; p++) {
        gchar c = *p;

        if (p == image && !(c >= 'a' && c <= 'z')) return FALSE;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return FALSE;
    }

    for (const gchar *p = colon + 1; *p; p++) {
        gchar c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
              || c == '.' || c == '_' || c == '-'))
            return FALSE;
    }

    return TRUE;
}

gboolean
pcv_validate_exec_cmd(const gchar *cmd)
{
    if (!cmd) return FALSE;

    gsize len = strlen(cmd);
    if (len == 0 || len > PCV_MAX_EXEC_CMD) return FALSE;

    for (gsize i = 0; i < len; i++) {
        if (cmd[i] == '\0') return FALSE;
    }

    return TRUE;
}

#include <sys/stat.h>
#include <errno.h>

static gboolean _validate_ipv4_cidr(const gchar *ip_part, gint prefix)
{
    if (prefix > 32) return FALSE;

    gchar **octs = g_strsplit(ip_part, ".", 5);

    if (!octs || g_strv_length(octs) != 4) {
        g_strfreev(octs);
        return FALSE;
    }
    for (int i = 0; i < 4; i++) {

        for (const gchar *p = octs[i]; *p; p++)
            if (!g_ascii_isdigit(*p)) { g_strfreev(octs); return FALSE; }

        gint v = atoi(octs[i]);
        if (v < 0 || v > 255) { g_strfreev(octs); return FALSE; }
    }
    g_strfreev(octs);
    return TRUE;
}

static gboolean _validate_ipv6_cidr(const gchar *ip_part, gint prefix)
{
    if (prefix > 128) return FALSE;
    if (!ip_part || strlen(ip_part) == 0) return FALSE;

    const gchar *dcolon = strstr(ip_part, "::");
    gboolean has_dcolon = (dcolon != NULL);
    if (has_dcolon) {

        if (strstr(dcolon + 2, "::") != NULL) return FALSE;
    }

    gchar **groups = g_strsplit(ip_part, ":", -1);
    if (!groups) return FALSE;

    guint n_groups = g_strv_length(groups);

    guint non_empty = 0;
    for (guint i = 0; i < n_groups; i++) {
        if (strlen(groups[i]) > 0) non_empty++;
    }

    if (!has_dcolon && non_empty != 8) {
        g_strfreev(groups);
        return FALSE;
    }
    if (has_dcolon && non_empty > 7) {
        g_strfreev(groups);
        return FALSE;
    }

    for (guint i = 0; i < n_groups; i++) {
        const gchar *g = groups[i];
        gsize len = strlen(g);
        if (len == 0) continue;
        if (len > 4) { g_strfreev(groups); return FALSE; }
        for (const gchar *p = g; *p; p++) {
            if (!g_ascii_isxdigit(*p)) { g_strfreev(groups); return FALSE; }
        }
    }

    g_strfreev(groups);
    return TRUE;
}

static gboolean
_is_safe_private_ipv4(const gchar *ip_str)
{
    guint o[4];
    if (sscanf(ip_str, "%u.%u.%u.%u", &o[0], &o[1], &o[2], &o[3]) != 4) return FALSE;
    if (o[0] > 255 || o[1] > 255 || o[2] > 255 || o[3] > 255) return FALSE;

    if (o[0] == 10) return TRUE;
    if (o[0] == 172 && o[1] >= 16 && o[1] <= 31) return TRUE;
    if (o[0] == 192 && o[1] == 168) return TRUE;

    if (o[0] == 100 && o[1] >= 64 && o[1] <= 127) return TRUE;
    return FALSE;
}

gboolean pcv_validate_private_cidr(const gchar *cidr)
{
    if (!pcv_validate_cidr(cidr)) return FALSE;
    const gchar *slash = g_strrstr(cidr, "/");
    if (!slash) return FALSE;
    gchar *ip_part = g_strndup(cidr, (gsize)(slash - cidr));
    gboolean result = FALSE;
    if (strchr(ip_part, '.')) {
        result = _is_safe_private_ipv4(ip_part);
    } else if (strchr(ip_part, ':')) {

        result = (g_ascii_strncasecmp(ip_part, "fc", 2) == 0 ||
                  g_ascii_strncasecmp(ip_part, "fd", 2) == 0);
    }
    g_free(ip_part);
    return result;
}

gboolean pcv_validate_cidr(const gchar *cidr)
{
    if (!cidr || strlen(cidr) > PCV_MAX_CIDR_LEN) return FALSE;

    const gchar *slash = g_strrstr(cidr, "/");
    if (!slash || slash == cidr) return FALSE;

    for (const gchar *p = slash + 1; *p; p++)
        if (!g_ascii_isdigit(*p)) return FALSE;
    if (*(slash + 1) == '\0') return FALSE;

    gint prefix = atoi(slash + 1);
    if (prefix < 0 || prefix > 128) return FALSE;

    gchar *ip_part = g_strndup(cidr, (gsize)(slash - cidr));

    gboolean result;
    if (strchr(ip_part, '.') != NULL) {
        result = _validate_ipv4_cidr(ip_part, prefix);
    } else if (strchr(ip_part, ':') != NULL) {
        result = _validate_ipv6_cidr(ip_part, prefix);
    } else {
        result = FALSE;
    }

    g_free(ip_part);
    return result;
}

gboolean pcv_validate_network_create_params(const gchar  *bridge_name,
                                            const gchar  *mode,
                                            const gchar  *cidr,
                                            const gchar  *physical_if,
                                            GError      **error)
{

    if (!pcv_validate_bridge_name(bridge_name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Invalid bridge_name '%s': 1-%d chars [a-zA-Z0-9_-]",
            bridge_name ? bridge_name : "(null)", PCV_MAX_BRIDGE_NAME);
        return FALSE;
    }

    if (mode && g_strcmp0(mode,"nat") != 0 && g_strcmp0(mode,"isolated") != 0
             && g_strcmp0(mode,"routed") != 0 && g_strcmp0(mode,"bridge") != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Invalid mode '%s': must be nat | isolated | routed | bridge", mode);
        return FALSE;
    }

    const gchar *eff_mode = mode ? mode : "nat";
    if (g_strcmp0(eff_mode, "bridge") != 0) {
        if (!cidr || !pcv_validate_private_cidr(cidr)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "Invalid or non-private cidr '%s': must be RFC1918 (10/8, 172.16/12, 192.168/16), "
                "RFC6598 (100.64/10), or fc00::/7 — public/link-local/multicast 거부",
                cidr ? cidr : "(null)");
            return FALSE;
        }
    }

    if (g_strcmp0(eff_mode, "bridge") == 0) {
        if (!physical_if || !pcv_validate_bridge_name(physical_if)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "Invalid or missing physical_if '%s' for bridge mode",
                physical_if ? physical_if : "(null)");
            return FALSE;
        }
    }

    return TRUE;
}

gboolean pcv_validate_pci_addr(const gchar *addr) {
    if (!addr || *addr == '\0') return FALSE;
    gsize len = strlen(addr);
    if (len > PCV_MAX_PCI_ADDR) return FALSE;

    if (strstr(addr, "..")) return FALSE;

    guint d, b, s, f;
    gint consumed = 0;
    gint n = sscanf(addr, "%x:%x:%x.%x%n", &d, &b, &s, &f, &consumed);
    if (n != 4) return FALSE;
    if ((gsize)consumed != len) return FALSE;

    if (d > 0xFFFF || b > 0xFF || s > 0x1F || f > 0x7) return FALSE;

    return TRUE;
}

void pcv_network_rundir_init(void)
{

    if (g_mkdir_with_parents(PCV_NETWORK_RUNDIR, 0700) < 0) {
        g_printerr("[network] Warning: cannot create rundir '%s': %s\n",
                   PCV_NETWORK_RUNDIR, g_strerror(errno));
    }
}

gboolean pcv_validate_disk_size_gb(gint size) {
    return size >= 1 && size <= 2048;
}

gboolean pcv_validate_port(gint port) {
    return port >= 1 && port <= 65535;
}

gboolean pcv_validate_zvol_name(const gchar *name) {
    if (!name || *name == '\0') return FALSE;
    gsize len = strlen(name);
    if (len > 64) return FALSE;

    if (strstr(name, "..") != NULL) return FALSE;

    if (!g_ascii_isalnum(name[0])) return FALSE;

    for (gsize i = 1; i < len; i++) {
        gchar c = name[i];
        if (!g_ascii_isalnum(c) && c != '_' && c != '.' && c != '-')
            return FALSE;
    }

    return TRUE;
}

gboolean pcv_validate_iface_name(const gchar *name) {
    if (!name || *name == '\0') return FALSE;

    gsize len = strlen(name);
    if (len > PCV_MAX_IFACE_NAME) return FALSE;

    if (name[0] == '-') return FALSE;

    for (const gchar *p = name; *p; p++) {
        gchar c = *p;
        if (!g_ascii_isalnum(c) && c != '_' && c != '.' && c != '-')
            return FALSE;
    }
    return TRUE;
}

gboolean pcv_validate_mac(const gchar *mac) {
    if (!mac) return FALSE;
    if (strlen(mac) != 17) return FALSE;

    for (gint i = 0; i < 17; i++) {
        if (i % 3 == 2) {

            if (mac[i] != ':') return FALSE;
        } else {

            if (!g_ascii_isxdigit(mac[i])) return FALSE;
        }
    }
    return TRUE;
}

gboolean pcv_validate_ip_literal(const gchar *ip) {
    if (!ip || *ip == '\0') return FALSE;

    struct in_addr  a4;
    struct in6_addr a6;
    if (inet_pton(AF_INET, ip, &a4) == 1) return TRUE;
    if (inet_pton(AF_INET6, ip, &a6) == 1) return TRUE;
    return FALSE;
}

gboolean pcv_validate_ipv6_prefix(const gchar *prefix) {
    if (!prefix || *prefix == '\0') return FALSE;

    for (const gchar *p = prefix; *p; p++) {
        if (*p == '\n' || *p == ' ') return FALSE;
    }

    const gchar *slash = g_strrstr(prefix, "/");
    if (!slash || slash == prefix) return FALSE;

    if (*(slash + 1) == '\0') return FALSE;
    for (const gchar *p = slash + 1; *p; p++)
        if (!g_ascii_isdigit(*p)) return FALSE;
    gint plen = atoi(slash + 1);
    if (plen < 0 || plen > 128) return FALSE;

    gchar *addr = g_strndup(prefix, (gsize)(slash - prefix));

    gboolean ok = TRUE;
    for (const gchar *p = addr; *p; p++) {
        if (!g_ascii_isxdigit(*p) && *p != ':') { ok = FALSE; break; }
    }

    if (ok) {
        struct in6_addr a6;
        if (inet_pton(AF_INET6, addr, &a6) != 1) ok = FALSE;
    }

    g_free(addr);
    return ok;
}

gboolean pcv_validate_l4_proto(const gchar *proto) {
    if (!proto) return FALSE;
    return g_strcmp0(proto, "tcp") == 0 ||
           g_strcmp0(proto, "udp") == 0 ||
           g_strcmp0(proto, "icmp") == 0;
}

gboolean pcv_validate_password_complexity(const gchar *password,
                                          const gchar **reason) {
    if (!password) {
        if (reason) *reason = "Password is required";
        return FALSE;
    }

    if (strlen(password) < 12) {
        if (reason)
            *reason = "Password must be at least 12 characters long";
        return FALSE;
    }

    gboolean has_lower = FALSE, has_upper = FALSE,
             has_digit = FALSE, has_special = FALSE;
    for (const guchar *p = (const guchar *)password; *p; p++) {
        guchar c = *p;
        if (c >= 'a' && c <= 'z')       has_lower = TRUE;
        else if (c >= 'A' && c <= 'Z')  has_upper = TRUE;
        else if (c >= '0' && c <= '9')  has_digit = TRUE;
        else if (c > ' ' && c < 127)    has_special = TRUE;
    }

    gint classes = (has_lower ? 1 : 0) + (has_upper ? 1 : 0)
                 + (has_digit ? 1 : 0) + (has_special ? 1 : 0);
    if (classes < 3) {
        if (reason)
            *reason = "Password must include at least 3 of 4 character "
                      "classes: lowercase, uppercase, digit, special";
        return FALSE;
    }

    return TRUE;
}
