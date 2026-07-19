
#ifndef PURECVISOR_VALIDATE_H
#define PURECVISOR_VALIDATE_H

#include <glib.h>

#define PCV_NETWORK_RUNDIR   "/var/run/purecvisor/network"

#define PCV_MAX_VM_NAME      64
#define PCV_MAX_SNAP_NAME    128
#define PCV_MAX_BRIDGE_NAME  16
#define PCV_MAX_IFACE_NAME   15
#define PCV_MAX_ISO_PATH     512
#define PCV_MAX_CIDR_LEN     49
#define PCV_MAX_REMOTE_HOST  253
#define PCV_MAX_SSH_USER     64

#define PCV_MAX_CONTAINER_IMAGE  128
#define PCV_MAX_EXEC_CMD         1024

#define PCV_MAX_PCI_ADDR         16

#define PCV_MIN_MEMORY_MB    128
#define PCV_MAX_MEMORY_MB    (1024 * 1024)
#define PCV_MIN_VCPU         1
#define PCV_MAX_VCPU         256
#define PCV_MIN_DISK_GB      1
#define PCV_MAX_DISK_GB      65536

[[nodiscard]] gboolean pcv_validate_vm_name(const gchar *name);

gboolean pcv_validate_snap_name(const gchar *name);

gboolean pcv_validate_bridge_name(const gchar *name);

gboolean pcv_validate_remote_host(const gchar *host);

gboolean pcv_validate_ssh_user(const gchar *user);

gboolean pcv_validate_cidr(const gchar *cidr);

gboolean pcv_validate_private_cidr(const gchar *cidr);

gboolean pcv_validate_network_create_params(const gchar  *bridge_name,
                                            const gchar  *mode,
                                            const gchar  *cidr,
                                            const gchar  *physical_if,
                                            GError      **error);

void pcv_network_rundir_init(void);

gboolean pcv_validate_iso_path(const gchar *path);

gboolean pcv_validate_base_image_path(const gchar *path);

gboolean pcv_validate_memory_mb(gint64 mb);
gboolean pcv_validate_vcpu(gint64 count);
gboolean pcv_validate_disk_gb(gint64 gb);

gboolean pcv_validate_vm_create_params(const gchar  *name,
                                       gint64        vcpu,
                                       gint64        memory_mb,
                                       gint64        disk_gb,
                                       const gchar  *iso_path,
                                       const gchar  *bridge,
                                       GError      **error);

gboolean pcv_validate_container_image(const gchar *image);

gboolean pcv_validate_exec_cmd(const gchar *cmd);

gboolean pcv_validate_pci_addr(const gchar *addr);

gboolean pcv_validate_disk_size_gb(gint size);

gboolean pcv_validate_port(gint port);

gboolean pcv_validate_zvol_name(const gchar *name);

gboolean pcv_validate_iface_name(const gchar *name);

gboolean pcv_validate_mac(const gchar *mac);

gboolean pcv_validate_ip_literal(const gchar *ip);

gboolean pcv_validate_ipv6_prefix(const gchar *prefix);

gboolean pcv_validate_l4_proto(const gchar *proto);

gboolean pcv_validate_password_complexity(const gchar *password,
                                          const gchar **reason);

#endif
