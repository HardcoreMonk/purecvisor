

















































































#include "vm_manager.h"
#include "virt_conn_pool.h"
#include "vm_config_builder.h"
#include "../storage/zfs_driver.h"
#include "../audit/pcv_audit.h"
#include "utils/pcv_config.h"
#include "api/ws_server.h"
#if PCV_CLUSTER_ENABLED
#include "modules/cluster/cluster_manager.h"
#endif
#include "../../utils/pcv_spawn.h"
#include "../../utils/pcv_log.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <libvirt-gobject/libvirt-gobject.h>
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

#define PCV_VM_METADATA_URI "urn:purecvisor:metadata"

























struct _PureCVisorVmManager {
    GObject parent_instance;
    GVirConnection *conn;
};



G_DEFINE_TYPE(PureCVisorVmManager, purecvisor_vm_manager, G_TYPE_OBJECT)







typedef enum {
    SIGNAL_VM_STARTED = 0,
    SIGNAL_VM_STOPPED,
    SIGNAL_VM_METRICS_UPDATED,
    N_SIGNALS
} PcvVmManagerSignalId;

static guint signals[N_SIGNALS] = { 0 };



















static gint _extract_vnc_port_from_domain(GVirDomain *dom) {
    GError *err = nullptr;
    GVirConfigDomain *config = nullptr;
    gchar *xml_data = nullptr;
    gint port = -1;


    config = gvir_domain_get_config(dom, 0, &err);
    if (err) {

        g_error_free(err);
        return -1;
    }

    xml_data = gvir_config_object_to_xml(GVIR_CONFIG_OBJECT(config));
    g_object_unref(config);

    if (!xml_data) return -1;









    GRegex *regex = g_regex_new("<graphics[^>]+port='(\\d+)'",
                                G_REGEX_CASELESS | G_REGEX_MULTILINE, 0, NULL);

    GMatchInfo *match_info;
    if (g_regex_match(regex, xml_data, 0, &match_info)) {
        gchar *port_str = g_match_info_fetch(match_info, 1);
        if (port_str) {
            port = (gint)g_ascii_strtoll(port_str, NULL, 10);
            g_free(port_str);
        }
    }

    g_match_info_free(match_info);
    g_regex_unref(regex);
    g_free(xml_data);

    return port;
}













static void purecvisor_vm_manager_finalize(GObject *object) {
    PureCVisorVmManager *self = PURECVISOR_VM_MANAGER(object);
    if (self->conn) {
        g_object_unref(self->conn);
    }
    G_OBJECT_CLASS(purecvisor_vm_manager_parent_class)->finalize(object);
}






static void purecvisor_vm_manager_class_init(PureCVisorVmManagerClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = purecvisor_vm_manager_finalize;













    signals[SIGNAL_VM_STARTED] =
        g_signal_new(PCV_VM_SIGNAL_STARTED,
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0,
                     NULL, NULL,
                     NULL,
                     G_TYPE_NONE,
                     1,
                     G_TYPE_STRING);









    signals[SIGNAL_VM_STOPPED] =
        g_signal_new(PCV_VM_SIGNAL_STOPPED,
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0,
                     NULL, NULL,
                     NULL,
                     G_TYPE_NONE,
                     1,
                     G_TYPE_STRING);











    signals[SIGNAL_VM_METRICS_UPDATED] =
        g_signal_new(PCV_VM_SIGNAL_METRICS_UPDATED,
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0,
                     NULL, NULL,
                     NULL,
                     G_TYPE_NONE,
                     1,
                     G_TYPE_POINTER);
}


static void purecvisor_vm_manager_init(PureCVisorVmManager *self) {
    self->conn = nullptr;
}








PureCVisorVmManager *purecvisor_vm_manager_new(GVirConnection *conn) {
    PureCVisorVmManager *self = g_object_new(PURECVISOR_TYPE_VM_MANAGER, NULL);
    if (conn) {
        self->conn = g_object_ref(conn);
    }
    return self;
}

















typedef struct {
    PureCVisorVmManager *manager;
    gchar *name;
    gint vcpu;
    gint ram_mb;
    gint disk_size_gb;
    gchar *disk_path;
    gchar *iso_path;
    gchar *network_bridge;
    gint   vlan_id;
    gchar *nic_type;
    gchar *pci_addr;
    gint boot_mode;
    gboolean tpm;
    gint cpu_mode;
    gboolean hugepages;
    gchar *storage_type;
    gchar *storage_pool;
    gchar *image_dir;
    gchar *base_image;
    gchar *owner;
} CreateVmTaskData;


static void create_vm_task_data_free(CreateVmTaskData *data) {
    if (data->manager) g_object_unref(data->manager);
    g_free(data->name);
    g_free(data->disk_path);
    g_free(data->iso_path);
    g_free(data->network_bridge);
    g_free(data->nic_type);
    g_free(data->pci_addr);
    g_free(data->storage_type);
    g_free(data->storage_pool);
    g_free(data->image_dir);
    g_free(data->base_image);
    g_free(data->owner);
    g_free(data);
}

static gchar *
_vm_xml_with_owner_metadata(const gchar *xml, const gchar *owner)
{









    if (!xml || !owner || !*owner)
        return g_strdup(xml);

    gchar *safe_owner = g_markup_escape_text(owner, -1);
    gchar *metadata = g_strdup_printf(
        "  <metadata>\n"
        "    <pcv:owner xmlns:pcv='%s'>%s</pcv:owner>\n"
        "  </metadata>\n",
        PCV_VM_METADATA_URI, safe_owner);
    g_free(safe_owner);

    const gchar *insert = strstr(xml, "</name>");
    gchar *patched = NULL;
    if (insert) {
        insert += strlen("</name>");
        patched = g_strdup_printf("%.*s%s%s",
                                  (gint)(insert - xml), xml,
                                  metadata, insert);
    } else {
        insert = strstr(xml, "<devices>");
        if (insert) {
            patched = g_strdup_printf("%.*s%s%s",
                                      (gint)(insert - xml), xml,
                                      metadata, insert);
        } else {
            const gchar *end = strstr(xml, "</domain>");
            patched = end
                ? g_strdup_printf("%.*s%s%s", (gint)(end - xml), xml, metadata, end)
                : g_strdup(xml);
        }
    }

    g_free(metadata);
    return patched;
}

















static void create_vm_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    CreateVmTaskData *data = (CreateVmTaskData *)task_data;
    GError *error = nullptr;
    PCV_LOG_INFO("vm_manager", "VM '%s' creation worker started (vcpu=%d ram=%dMB disk=%dGB stype=%s)",
                 data ? data->name : "(null)",
                 data ? data->vcpu : -1,
                 data ? data->ram_mb : -1,
                 data ? data->disk_size_gb : -1,
                 (data && data->storage_type) ? data->storage_type : "(auto)");


    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "vm.create cancelled before start");
        return;
    }

















    gint final_disk_size = (data->disk_size_gb > 0) ? data->disk_size_gb : 50;
    if (final_disk_size > 2048) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "disk_size_gb (%d) exceeds 2TB limit", final_disk_size);
        return;
    }
    const gchar *zvol_pool = (data->storage_pool && *data->storage_pool)
        ? data->storage_pool
        : pcv_config_get_zvol_pool();
    const gchar *image_dir = (data->image_dir && *data->image_dir)
        ? data->image_dir
        : pcv_config_get_image_dir();
    gchar *disk_path = nullptr;
    gboolean use_zvol = FALSE;







    const gchar *st = data->storage_type;
    gboolean use_file_raw = FALSE;

    if (st && g_strcmp0(st, "qcow2") == 0) {
        use_zvol = FALSE;
    } else if (st && g_strcmp0(st, "raw") == 0) {
        use_zvol = FALSE;
        use_file_raw = TRUE;
    } else if (st && g_strcmp0(st, "zvol") == 0) {

        const gchar *pool_chk_argv[] = {"zfs", "list", "-H",
                                         zvol_pool, NULL};
        gchar *chk_err = nullptr;
        GError *chk_e = nullptr;
        use_zvol = pcv_spawn_sync(pool_chk_argv, NULL, &chk_err, &chk_e);
        g_free(chk_err);
        if (chk_e) g_error_free(chk_e);
        if (!use_zvol) {
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                "storage_type 'zvol' requested but ZFS pool '%s' not found",
                zvol_pool);
            return;
        }
    } else {

        const gchar *pool_chk_argv[] = {"zfs", "list", "-H",
                                         zvol_pool, NULL};
        gchar *chk_err = nullptr;
        GError *chk_e = nullptr;
        use_zvol = pcv_spawn_sync(pool_chk_argv, NULL, &chk_err, &chk_e);
        g_free(chk_err);
        if (chk_e) g_error_free(chk_e);
    }


#if PCV_CLUSTER_ENABLED
    if (use_zvol && !pcv_cluster_check_zvol_fence()) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
            "zvol I/O fence: this node does not hold zvol pool ownership (ADR-0011). "
            "Only the cluster leader with confirmed ownership can create zvol-backed VMs.");
        return;
    }
#endif

    if (use_zvol) {

        gchar *zvol_name = g_strdup_printf("%s/%s",
                                            zvol_pool, data->name);
        gchar *zvol_dev  = g_strdup_printf("/dev/zvol/%s", zvol_name);


        {
            const gchar *chk_argv[] = {"zfs", "list", "-H", "-t", "volume",
                                        zvol_name, NULL};
            gchar *chk_err = nullptr;
            GError *chk_e = nullptr;
            gboolean exists = pcv_spawn_sync(chk_argv, NULL, &chk_err, &chk_e);
            g_free(chk_err);
            if (chk_e) g_error_free(chk_e);
            if (exists) {
                g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "ZFS dataset '%s' already exists — delete the VM first",
                    zvol_name);
                g_free(zvol_name); g_free(zvol_dev);
                return;
            }
        }

        gchar *size_str = g_strdup_printf("%dG", final_disk_size);
        const gchar *zfs_argv[] = {"zfs", "create", "-V", size_str,
                                    zvol_name, NULL};
        gchar *std_err = nullptr;

        if (!pcv_spawn_sync(zfs_argv, NULL, &std_err, &error)) {
            gchar *err_msg = error ? error->message
                                   : (std_err ? g_strstrip(std_err)
                                              : "Unknown ZFS error");
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "ZFS Provisioning Failed: %s", err_msg);
            if (error) g_error_free(error);
            g_free(std_err); g_free(size_str);
            g_free(zvol_name); g_free(zvol_dev);
            return;
        }
        g_free(std_err); g_free(size_str);
        disk_path = zvol_dev;
        g_free(zvol_name);
        PCV_LOG_INFO("vm_manager", "VM '%s': zvol disk created at %s (%dG)",
                     data->name, zvol_pool, final_disk_size);




        if (data->base_image && *data->base_image &&
            g_file_test(data->base_image, G_FILE_TEST_EXISTS)) {
            const gchar *udev_argv[] = {"udevadm", "settle", "--timeout=5", NULL};
            (void)pcv_spawn_sync(udev_argv, NULL, NULL, NULL);
            const gchar *conv_argv[] = {
                "qemu-img", "convert", "-f", "qcow2", "-O", "raw",
                data->base_image, disk_path, NULL
            };
            gchar *conv_err = NULL;
            gboolean conv_ok = pcv_spawn_sync(conv_argv, NULL, &conv_err, NULL);
            if (conv_ok) {
                PCV_LOG_INFO("vm_manager", "VM '%s': base image '%s' written to zvol",
                             data->name, data->base_image);
            } else {
                PCV_LOG_WARN("vm_manager", "VM '%s': base image write failed: %s",
                             data->name, conv_err ? conv_err : "unknown");
            }
            g_free(conv_err);
        }
    } else {

        const gchar *fmt = use_file_raw ? "raw" : "qcow2";
        const gchar *ext = use_file_raw ? "img" : "qcow2";

        if (!g_file_test(image_dir, G_FILE_TEST_IS_DIR)) {
            const gchar *mkdir_argv[] = {"mkdir", "-p", image_dir, NULL};
            (void)pcv_spawn_sync(mkdir_argv, NULL, NULL, NULL);
        }

        disk_path = g_strdup_printf("%s/%s.%s", image_dir, data->name, ext);


        if (g_file_test(disk_path, G_FILE_TEST_EXISTS)) {
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_EXISTS,
                "Disk image '%s' already exists — delete the VM first",
                disk_path);
            g_free(disk_path);
            return;
        }

        gchar *size_str = g_strdup_printf("%dG", final_disk_size);
        const gchar *qimg_argv[] = {"qemu-img", "create", "-f", fmt,
                                     disk_path, size_str, NULL};
        gchar *std_err = nullptr;

        if (!pcv_spawn_sync(qimg_argv, NULL, &std_err, &error)) {
            gchar *err_msg = error ? error->message
                                   : (std_err ? g_strstrip(std_err)
                                              : "Unknown qemu-img error");
            PCV_LOG_WARN("vm_manager", "VM '%s' qemu-img FAILED: %s (stderr=%s)",
                         data->name, err_msg, std_err ? std_err : "(none)");
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "%s Provisioning Failed: %s", fmt, err_msg);
            g_unlink(disk_path);
            if (error) g_error_free(error);
            g_free(std_err); g_free(size_str); g_free(disk_path);
            return;
        }
        g_free(std_err); g_free(size_str);
        PCV_LOG_INFO("vm_manager", "VM '%s': %s disk created at %s (%dG)",
                      data->name, fmt, disk_path, final_disk_size);
    }


    PureCVisorVmConfig *config = purecvisor_vm_config_new(data->name,
                                                          data->vcpu,
                                                          data->ram_mb);
    purecvisor_vm_config_set_disk(config, disk_path);

    if (data->iso_path) purecvisor_vm_config_set_iso(config, data->iso_path);
    if (data->cpu_mode > 0) purecvisor_vm_config_set_cpu_mode(config, data->cpu_mode);
    if (data->hugepages) purecvisor_vm_config_set_hugepages(config, TRUE);
    if (data->boot_mode > 0) purecvisor_vm_config_set_boot_mode(config, data->boot_mode);
    if (data->tpm) purecvisor_vm_config_set_tpm(config, TRUE);


    GVirConfigDomain *domain_config = purecvisor_vm_config_build(config);

    gchar *base_xml = gvir_config_object_to_xml(GVIR_CONFIG_OBJECT(domain_config));

































    gchar *final_xml = nullptr;
    if (data->network_bridge && strlen(data->network_bridge) > 0) {
        gchar *iface_xml = nullptr;
        const gchar *nic_type = data->nic_type ? data->nic_type : "bridge";

        if (g_strcmp0(nic_type, "dpdk") == 0) {



            iface_xml = g_strdup_printf(
                "    <interface type='vhostuser'>\n"
                "      <source type='unix' path='/var/run/purecvisor/vhost-%s.sock' mode='server'/>\n"
                "      <model type='virtio'/>\n"
                "      <driver queues='2'/>\n"
                "    </interface>\n",
                data->name);
        } else if (g_strcmp0(nic_type, "sriov") == 0 && data->pci_addr) {



            guint dom = 0, bus = 0, slot = 0, func = 0;
            sscanf(data->pci_addr, "%x:%x:%x.%x", &dom, &bus, &slot, &func);
            iface_xml = g_strdup_printf(
                "    <hostdev mode='subsystem' type='pci' managed='yes'>\n"
                "      <source>\n"
                "        <address domain='0x%04x' bus='0x%02x' slot='0x%02x' function='0x%x'/>\n"
                "      </source>\n"
                "    </hostdev>\n",
                dom, bus, slot, func);
        } else {

            gchar *vlan_xml = (data->vlan_id >= 1 && data->vlan_id <= 4094)
                ? g_strdup_printf("      <vlan><tag id=\"%d\"/></vlan>\n", data->vlan_id)
                : g_strdup("");




            const gchar *ovs_argv[] = {"ovs-vsctl", "br-exists", data->network_bridge, NULL};
            gboolean is_ovs = pcv_spawn_sync(ovs_argv, NULL, NULL, NULL);




            gchar *virtualport_xml = is_ovs
                ? g_strdup("      <virtualport type='openvswitch'/>\n")
                : g_strdup("");



            gchar *safe_bridge = g_markup_escape_text(data->network_bridge, -1);
            iface_xml = g_strdup_printf(
                "    <interface type='bridge'>\n"
                "      <source bridge='%s'/>\n"
                "%s"
                "      <model type='virtio'/>\n"
                "%s"
                "    </interface>\n",
                safe_bridge, virtualport_xml, vlan_xml);
            g_free(safe_bridge);
            g_free(virtualport_xml);
            g_free(vlan_xml);
        }


        gchar *insert_point = strstr(base_xml, "</devices>");
        if (insert_point) {
            gsize prefix_len = (gsize)(insert_point - base_xml);
            final_xml = g_strdup_printf("%.*s%s%s",
                                        (gint)prefix_len, base_xml,
                                        iface_xml,
                                        insert_point);
        } else {

            final_xml = g_strdup(base_xml);
        }
        g_free(iface_xml);
        g_free(base_xml);
    } else {

        final_xml = base_xml;
    }

    if (data->owner && *data->owner) {
        gchar *owned_xml = _vm_xml_with_owner_metadata(final_xml, data->owner);
        g_free(final_xml);
        final_xml = owned_xml;
    }










    {
        const gchar *old_video = "<model type=\"virtio\"/>";
        const gchar *new_video =
            "<model type=\"virtio\" vram=\"65536\" heads=\"1\">\n"
            "          <resolution x=\"1024\" y=\"768\"/>\n"
            "        </model>";
        gchar *pos = strstr(final_xml, old_video);
        if (pos) {
            gchar *patched = g_strdup_printf("%.*s%s%s",
                (gint)(pos - final_xml), final_xml,
                new_video, pos + strlen(old_video));
            g_free(final_xml);
            final_xml = patched;
        }
    }








    if (data->hugepages) {
        const gchar *hp_xml = "  <memoryBacking><hugepages><page size='2048' unit='KiB'/></hugepages></memoryBacking>\n";
        gchar *end = strstr(final_xml, "</domain>");
        if (end) {
            gchar *patched = g_strdup_printf("%.*s%s%s", (gint)(end - final_xml), final_xml, hp_xml, end);
            g_free(final_xml);
            final_xml = patched;
        }
    }

































    if (data->boot_mode >= 1) {
        const gchar *old_os = "<type machine=\"q35\">hvm</type>";


        const gchar *loader_path = nullptr;
        const gchar *nvram_tpl = nullptr;

        if (data->boot_mode == 2) {

            static const gchar *sb_loaders[] = {
                "/usr/share/OVMF/OVMF_CODE_4M.ms.fd",
                "/usr/share/OVMF/OVMF_CODE.secboot.fd",
                "/usr/share/edk2/ovmf/OVMF_CODE.secboot.fd",
                NULL
            };
            static const gchar *sb_nvrams[] = {
                "/usr/share/OVMF/OVMF_VARS_4M.ms.fd",
                "/usr/share/OVMF/OVMF_VARS.fd",
                "/usr/share/edk2/ovmf/OVMF_VARS.fd",
                NULL
            };
            for (gint i = 0; sb_loaders[i]; i++) {
                if (g_file_test(sb_loaders[i], G_FILE_TEST_EXISTS)) {
                    loader_path = sb_loaders[i];
                    nvram_tpl = sb_nvrams[i];
                    break;
                }
            }
            if (!loader_path) {
                loader_path = sb_loaders[0];
                nvram_tpl = sb_nvrams[0];
            }
        } else {

            static const gchar *uefi_loaders[] = {
                "/usr/share/OVMF/OVMF_CODE_4M.fd",
                "/usr/share/OVMF/OVMF_CODE.fd",
                "/usr/share/edk2/ovmf/OVMF_CODE.fd",
                NULL
            };
            static const gchar *uefi_nvrams[] = {
                "/usr/share/OVMF/OVMF_VARS_4M.fd",
                "/usr/share/OVMF/OVMF_VARS.fd",
                "/usr/share/edk2/ovmf/OVMF_VARS.fd",
                NULL
            };
            for (gint i = 0; uefi_loaders[i]; i++) {
                if (g_file_test(uefi_loaders[i], G_FILE_TEST_EXISTS)) {
                    loader_path = uefi_loaders[i];
                    nvram_tpl = uefi_nvrams[i];
                    break;
                }
            }
            if (!loader_path) {
                loader_path = uefi_loaders[0];
                nvram_tpl = uefi_nvrams[0];
            }
        }


        gchar *nvram_path = g_strdup_printf(
            "/var/lib/libvirt/qemu/nvram/%s_VARS.fd", data->name);


        const gchar *secure_attr = data->boot_mode == 2 ? " secure=\"yes\"" : "";
        gchar *new_os = g_strdup_printf(
            "<type machine=\"q35\">hvm</type>\n"
            "      <loader readonly=\"yes\" type=\"pflash\"%s>%s</loader>\n"
            "      <nvram template=\"%s\">%s</nvram>",
            secure_attr, loader_path, nvram_tpl, nvram_path);

        gchar *pos = strstr(final_xml, old_os);
        if (pos) {
            gchar *patched = g_strdup_printf("%.*s%s%s",
                (gint)(pos - final_xml), final_xml,
                new_os, pos + strlen(old_os));
            g_free(final_xml);
            final_xml = patched;
        }
        g_free(new_os);
        g_free(nvram_path);
    }





    if (data->tpm) {
        const gchar *tpm_xml =
            "    <tpm model='tpm-tis'>\n"
            "      <backend type='emulator' version='2.0'/>\n"
            "    </tpm>\n";
        gchar *insert = strstr(final_xml, "</devices>");
        if (insert) {
            gchar *patched = g_strdup_printf("%.*s%s%s",
                (gint)(insert - final_xml), final_xml,
                tpm_xml, insert);
            g_free(final_xml);
            final_xml = patched;
        }
    }




    {
        const gchar *wd_cfg = pcv_config_get_string("vm", "watchdog_enabled", "true");
        if (g_ascii_strcasecmp(wd_cfg, "true") == 0 ||
            g_ascii_strcasecmp(wd_cfg, "1") == 0 ||
            g_ascii_strcasecmp(wd_cfg, "yes") == 0) {
            const gchar *wd_xml =
                "    <watchdog model='i6300esb' action='reset'/>\n";
            gchar *insert = strstr(final_xml, "</devices>");
            if (insert) {
                gchar *patched = g_strdup_printf("%.*s%s%s",
                    (gint)(insert - final_xml), final_xml,
                    wd_xml, insert);
                g_free(final_xml);
                final_xml = patched;
            }
        }
    }





















    if (cancellable && g_cancellable_is_cancelled(cancellable)) {

        if (disk_path && *disk_path) {
            if (g_str_has_prefix(disk_path, "/dev/zvol/")) {
                gchar *zvol_name = g_strdup(disk_path + strlen("/dev/zvol/"));
                const gchar *zfs_argv[] = {"zfs", "destroy", "-f", zvol_name, NULL};
                (void)pcv_spawn_sync(zfs_argv, NULL, NULL, NULL);
                g_free(zvol_name);
            } else if (g_file_test(disk_path, G_FILE_TEST_EXISTS)) {
                g_unlink(disk_path);
            }
        }
        PCV_LOG_INFO("vm_manager", "VM '%s' creation cancelled before define", data->name);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
            "VM creation cancelled");
        g_free(final_xml);
        g_object_unref(domain_config);
        purecvisor_vm_config_free(config);
        g_free(disk_path);
        return;
    }

    virConnectPtr conn = virt_conn_pool_acquire();
    virDomainPtr dom = virDomainDefineXML(conn, final_xml);

    if (!dom) {





        virErrorPtr libvirt_err = virGetLastError();
        PCV_LOG_WARN("vm_manager", "virDomainDefineXML failed: %s",
                     libvirt_err ? libvirt_err->message : "unknown");


        if (disk_path && *disk_path) {
            if (g_str_has_prefix(disk_path, "/dev/zvol/")) {

                gchar *zvol_name = g_strdup(disk_path + strlen("/dev/zvol/"));
                const gchar *zfs_argv[] = {"zfs", "destroy", "-f", zvol_name, NULL};
                (void)pcv_spawn_sync(zfs_argv, NULL, NULL, NULL);
                g_free(zvol_name);
                PCV_LOG_WARN("vm_manager", "Rolled back zvol for failed VM define: %s", disk_path);
            } else {

                if (g_file_test(disk_path, G_FILE_TEST_EXISTS)) {
                    g_unlink(disk_path);
                    PCV_LOG_WARN("vm_manager", "Rolled back disk file for failed VM define: %s", disk_path);
                }
            }
        }

        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
            "VM operation failed — check server logs for details");
    } else {







        gchar *stored = virDomainGetXMLDesc(dom, 0);
        if (stored && final_xml) {

            const gchar *expected_br = strstr(final_xml, "<source bridge='");
            const gchar *actual_br   = strstr(stored,    "<source bridge='");
            gboolean bridge_match = TRUE;
            if (expected_br && actual_br) {
                expected_br += strlen("<source bridge='");
                actual_br   += strlen("<source bridge='");
                const gchar *e_end = strchr(expected_br, '\'');
                const gchar *a_end = strchr(actual_br,   '\'');
                if (e_end && a_end) {
                    gsize el = (gsize)(e_end - expected_br);
                    gsize al = (gsize)(a_end - actual_br);
                    if (el != al || strncmp(expected_br, actual_br, el) != 0) {
                        bridge_match = FALSE;
                        PCV_LOG_WARN("vm_manager",
                            "Post-define bridge mismatch for '%s' "
                            "(expected=%.*s, stored=%.*s) — redefining",
                            data->name, (int)el, expected_br, (int)al, actual_br);
                    }
                }
            }
            if (!bridge_match) {

                virDomainUndefine(dom);
                virDomainFree(dom);
                dom = virDomainDefineXML(conn, final_xml);
                if (!dom) {
                    PCV_LOG_WARN("vm_manager",
                        "Redefine after mismatch failed for '%s'", data->name);
                }
            }
        }
        g_free(stored);
        if (dom) {
            virDomainFree(dom);

#if PCV_CLUSTER_ENABLED
            pcv_cluster_sync_vm_xml(data->name);
#endif
            g_task_return_boolean(task, TRUE);
        } else {
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                "VM redefine failed after parameter mismatch");
        }
    }
















    virt_conn_pool_release(conn);
    g_free(final_xml);
    g_object_unref(domain_config);
    purecvisor_vm_config_free(config);
    g_free(disk_path);
}



















void purecvisor_vm_manager_create_vm_async(PureCVisorVmManager *self,
                                           const gchar *name,
                                           gint vcpu,
                                           gint ram_mb,
                                           gint disk_size_gb,
                                           const gchar *iso_path,
                                           const gchar *network_bridge,
                                           gint         vlan_id,
                                           gint         boot_mode,
                                           gboolean     tpm,
                                           gint         cpu_mode,
                                           gboolean     hugepages,
                                           const gchar *storage_type,
                                           const gchar *storage_pool,
                                           const gchar *image_dir,
                                           const gchar *nic_type,
                                           const gchar *pci_addr,
                                           const gchar *base_image,
                                           const gchar *owner,
                                           GCancellable *cancellable,
                                           GAsyncReadyCallback callback,
                                           gpointer user_data) {































    GTask *task = g_task_new(self, cancellable, callback, user_data);
    CreateVmTaskData *data = g_new0(CreateVmTaskData, 1);

    data->manager = g_object_ref(self);
    data->name = g_strdup(name);
    data->vcpu = vcpu;
    data->ram_mb = ram_mb;
    data->disk_size_gb = disk_size_gb;
    data->iso_path = iso_path ? g_strdup(iso_path) : NULL;
    data->network_bridge = network_bridge ? g_strdup(network_bridge) : NULL;
    data->vlan_id = vlan_id;
    data->boot_mode = boot_mode;
    data->tpm = tpm;
    data->cpu_mode = cpu_mode;
    data->hugepages = hugepages;
    data->storage_type = storage_type ? g_strdup(storage_type) : NULL;
    data->storage_pool = storage_pool ? g_strdup(storage_pool) : NULL;
    data->image_dir = image_dir ? g_strdup(image_dir) : NULL;
    data->nic_type = nic_type ? g_strdup(nic_type) : NULL;
    data->pci_addr = pci_addr ? g_strdup(pci_addr) : NULL;
    data->base_image = base_image ? g_strdup(base_image) : NULL;
    data->owner = owner && *owner ? g_strdup(owner) : NULL;

    g_task_set_task_data(task, data, (GDestroyNotify)create_vm_task_data_free);
    g_task_run_in_thread(task, create_vm_thread);
    g_object_unref(task);
}








gboolean purecvisor_vm_manager_create_vm_finish(PureCVisorVmManager *manager __attribute__((unused)),
                                                GAsyncResult *res,
                                                GError **error) {
    return g_task_propagate_boolean(G_TASK(res), error);
}











typedef struct {
    PureCVisorVmManager *manager;
    gchar *name;
} LifecycleTaskData;


static void lifecycle_task_data_free(LifecycleTaskData *data) {
    if (data->manager) g_object_unref(data->manager);
    g_free(data->name);
    g_free(data);
}













static void start_vm_thread_impl(GTask *task,
                                 gpointer source_object __attribute__((unused)),
                                 gpointer task_data,
                                 GCancellable *cancellable __attribute__((unused))) {
    LifecycleTaskData *data = (LifecycleTaskData *)task_data;




    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to acquire libvirt connection");
        return;
    }

    virDomainPtr dom = virDomainLookupByName(conn, data->name);
    if (!dom) {
        virErrorPtr e = virGetLastError();
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                "VM not found: %s", e ? e->message : data->name);
        virt_conn_pool_release(conn);
        return;
    }

    int rc = virDomainCreate(dom);
    if (rc != 0) {
        virErrorPtr e = virGetLastError();
        PCV_LOG_WARN("vm_manager", "virDomainCreate failed for '%s': %s",
                     data->name, e ? e->message : "unknown error");
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "VM operation failed — check server logs for details");
    } else {
        g_task_return_boolean(task, TRUE);
    }

    virDomainFree(dom);
    virt_conn_pool_release(conn);
}










void purecvisor_vm_manager_start_vm_async(PureCVisorVmManager *self,
                                          const gchar *name,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data) {
    GTask *task = g_task_new(self, NULL, callback, user_data);
    LifecycleTaskData *data = g_new0(LifecycleTaskData, 1);
    data->manager = g_object_ref(self);
    data->name = g_strdup(name);

    g_task_set_task_data(task, data, (GDestroyNotify)lifecycle_task_data_free);
    g_task_run_in_thread(task, start_vm_thread_impl);
    g_object_unref(task);
}








gboolean purecvisor_vm_manager_start_vm_finish(PureCVisorVmManager *manager,
                                               GAsyncResult *res,
                                               GError **error) {
    gboolean ok = g_task_propagate_boolean(G_TASK(res), error);
    if (ok) {




        LifecycleTaskData *data = g_task_get_task_data(G_TASK(res));
        g_signal_emit(manager, signals[SIGNAL_VM_STARTED], 0, data->name);
    }
    return ok;
}



















static void stop_vm_thread_impl(GTask *task,
                                gpointer source_object __attribute__((unused)),
                                gpointer task_data,
                                GCancellable *cancellable __attribute__((unused))) {
    LifecycleTaskData *data = (LifecycleTaskData *)task_data;

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to acquire libvirt connection");
        return;
    }

    virDomainPtr dom = virDomainLookupByName(conn, data->name);
    if (!dom) {
        virErrorPtr e = virGetLastError();
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                "VM not found: %s", e ? e->message : data->name);
        virt_conn_pool_release(conn);
        return;
    }












    int rc = virDomainShutdown(dom);
    if (rc != 0) {

        if (virDomainDestroy(dom) < 0) {
            PCV_LOG_WARN("vm_manager", "virDomainDestroy failed for '%s'", data->name);

        }
        goto stop_done;
    }





    for (int poll_i = 0; poll_i < 30; poll_i++) {
        g_usleep(G_USEC_PER_SEC);
        int state = 0;
        if (virDomainGetState(dom, &state, NULL, 0) == 0) {
            if (state == VIR_DOMAIN_SHUTOFF) {
                PCV_LOG_INFO("vm_manager", "VM '%s' shut down gracefully after %ds",
                             data->name, poll_i + 1);
                goto stop_done;
            }
        }
    }
    PCV_LOG_WARN("vm_manager", "VM '%s' graceful shutdown timed out (30s) — force destroying",
                 data->name);
    if (virDomainDestroy(dom) < 0) {
        PCV_LOG_WARN("vm_manager", "virDomainDestroy (post-timeout) failed for '%s'", data->name);
    }

stop_done:

    g_task_return_boolean(task, TRUE);

    virDomainFree(dom);
    virt_conn_pool_release(conn);
}





void purecvisor_vm_manager_stop_vm_async(PureCVisorVmManager *self,
                                         const gchar *name,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data) {
    GTask *task = g_task_new(self, NULL, callback, user_data);
    LifecycleTaskData *data = g_new0(LifecycleTaskData, 1);
    data->manager = g_object_ref(self);
    data->name = g_strdup(name);

    g_task_set_task_data(task, data, (GDestroyNotify)lifecycle_task_data_free);
    g_task_run_in_thread(task, stop_vm_thread_impl);
    g_object_unref(task);
}







gboolean purecvisor_vm_manager_stop_vm_finish(PureCVisorVmManager *manager,
                                              GAsyncResult *res,
                                              GError **error) {
    gboolean ok = g_task_propagate_boolean(G_TASK(res), error);
    if (ok) {

        LifecycleTaskData *data = g_task_get_task_data(G_TASK(res));
        g_signal_emit(manager, signals[SIGNAL_VM_STOPPED], 0, data->name);
    }
    return ok;
}



















typedef struct {
    gchar *vm_name;
} ZfsDestroyData;

static void _zfs_destroy_data_free(gpointer p) {
    ZfsDestroyData *d = p;
    if (!d) return;
    g_free(d->vm_name);
    g_free(d);
}



constexpr int ZFS_RETRY_MAX = 5;
static const guint ZFS_RETRY_MS[ZFS_RETRY_MAX] = {500, 1000, 2000, 4000, 8000};













static GHashTable *g_delete_status = nullptr;
static GMutex      g_delete_status_mu;


static void _delete_status_set(const gchar *vm, const gchar *status) {
    g_mutex_lock(&g_delete_status_mu);
    if (!g_delete_status)
        g_delete_status = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_replace(g_delete_status, g_strdup(vm), g_strdup(status));
    g_mutex_unlock(&g_delete_status_mu);
}

















const gchar *pcv_vm_delete_status_get(const gchar *vm) {
    if (!vm) return "unknown";
    g_mutex_lock(&g_delete_status_mu);
    const gchar *st = g_delete_status
        ? g_hash_table_lookup(g_delete_status, vm) : NULL;
    g_mutex_unlock(&g_delete_status_mu);
    return st ? st : "not_found";
}


void pcv_vm_manager_cleanup(void) {
    g_mutex_lock(&g_delete_status_mu);
    if (g_delete_status) {
        g_hash_table_destroy(g_delete_status);
        g_delete_status = nullptr;
    }
    g_mutex_unlock(&g_delete_status_mu);
}
















static void
_zfs_destroy_thread(GTask    *task __attribute__((unused)),
                    gpointer  source_object __attribute__((unused)),
                    gpointer  task_data,
                    GCancellable *cancellable __attribute__((unused)))
{
    ZfsDestroyData *d = (ZfsDestroyData *)task_data;
    GError *err = nullptr;
    _delete_status_set(d->vm_name, "deleting");


    gchar *qcow2_path = g_strdup_printf("%s/%s.qcow2",
                                         pcv_config_get_image_dir(), d->vm_name);
    if (g_file_test(qcow2_path, G_FILE_TEST_EXISTS)) {
        if (g_unlink(qcow2_path) == 0) {
            PCV_LOG_INFO("vm_manager",
                         "qcow2 disk removed: %s", qcow2_path);
        } else {
            PCV_LOG_WARN("vm_manager",
                         "qcow2 disk remove failed: %s", qcow2_path);
        }
        g_free(qcow2_path);

    } else {
        g_free(qcow2_path);
    }


    gchar *zfs_dataset = g_strdup_printf("%s/%s", pcv_config_get_zvol_pool(), d->vm_name);
    const gchar *check_argv[] = {"zfs", "list", "-H", "-o", "name",
                                 zfs_dataset, NULL};
    gboolean exists = pcv_spawn_sync(check_argv, NULL, NULL, NULL);

    if (!exists) {
        PCV_LOG_WARN("vm_manager",
                     "ZFS dataset '%s' not found — skipped", zfs_dataset);
        g_free(zfs_dataset);
        goto cleanup;
    }

    PCV_LOG_INFO("vm_manager", "ZFS destroy (bg): %s", zfs_dataset);






    for (guint attempt = 0; attempt < ZFS_RETRY_MAX; attempt++) {

        if (attempt > 0) {
            PCV_LOG_INFO("vm_manager",
                         "ZFS destroy retry %u/%u for '%s' (wait %ums)",
                         attempt, ZFS_RETRY_MAX - 1,
                         zfs_dataset, ZFS_RETRY_MS[attempt - 1]);
            g_usleep((gulong)ZFS_RETRY_MS[attempt - 1] * 1000UL);
        }

        g_clear_error(&err);
        gboolean ok = purecvisor_zfs_destroy_volume(pcv_config_get_zvol_pool(),
                                                     d->vm_name, &err);
        if (ok) {
            PCV_LOG_INFO("vm_manager",
                         "ZFS dataset removed: %s (attempt %u)",
                         zfs_dataset, attempt + 1);
            goto zfs_cleanup;
        }


        gboolean is_busy = err &&
            (strstr(err->message, "dataset is busy") != nullptr ||
             strstr(err->message, "busy")             != nullptr ||
             strstr(err->message, "EBUSY")            != nullptr);

        if (!is_busy) {
            PCV_LOG_WARN("vm_manager",
                         "ZFS destroy failed (non-retryable) for %s: %s",
                         zfs_dataset, err ? err->message : "unknown");
            goto zfs_cleanup;
        }

        PCV_LOG_WARN("vm_manager",
                     "ZFS destroy: device busy for '%s', will retry",
                     zfs_dataset);
    }


    PCV_LOG_WARN("vm_manager",
                 "ZFS destroy gave up after %u attempts for '%s': %s",
                 ZFS_RETRY_MAX, zfs_dataset,
                 err ? err->message : "unknown");

zfs_cleanup:
    g_free(zfs_dataset);
cleanup:

    _delete_status_set(d->vm_name, err ? "failed" : "done");

#if PCV_CLUSTER_ENABLED
    pcv_cluster_remove_vm_xml(d->vm_name);
#endif
    if (err) g_error_free(err);

}
















static void delete_vm_thread_impl(GTask *task,
                                  gpointer source_object __attribute__((unused)),
                                  gpointer task_data,
                                  GCancellable *cancellable __attribute__((unused))) {
    LifecycleTaskData *data = (LifecycleTaskData *)task_data;
    GError *err __attribute__((unused)) = nullptr;





    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to acquire libvirt connection");
        return;
    }

    virDomainPtr dom = virDomainLookupByName(conn, data->name);
    if (dom) {






        virDomainState state = VIR_DOMAIN_NOSTATE;
        int reason = 0;
        virDomainGetState(dom, (int *)&state, &reason, 0);
        if (state == VIR_DOMAIN_RUNNING || state == VIR_DOMAIN_PAUSED) {
            virDomainDestroy(dom);
        }







        int rc = virDomainUndefineFlags(dom, VIR_DOMAIN_UNDEFINE_NVRAM |
                                             VIR_DOMAIN_UNDEFINE_SNAPSHOTS_METADATA);
        if (rc != 0) {

            rc = virDomainUndefine(dom);
        }
        virDomainFree(dom);

        if (rc != 0) {
            virErrorPtr e = virGetLastError();
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "virDomainUndefine failed: %s",
                                    e ? e->message : "unknown error");
            virt_conn_pool_release(conn);
            return;
        }
    }


    virt_conn_pool_release(conn);


























    _delete_status_set(data->name, "pending");
    ZfsDestroyData *zd = g_new0(ZfsDestroyData, 1);
    zd->vm_name = g_strdup(data->name);

    GTask *zfs_task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(zfs_task, zd, _zfs_destroy_data_free);
    g_task_run_in_thread(zfs_task, _zfs_destroy_thread);
    g_object_unref(zfs_task);

    g_task_return_boolean(task, TRUE);
}





void purecvisor_vm_manager_delete_vm_async(PureCVisorVmManager *self,
                                           const gchar *name,
                                           GAsyncReadyCallback callback,
                                           gpointer user_data) {
    GTask *task = g_task_new(self, NULL, callback, user_data);
    LifecycleTaskData *data = g_new0(LifecycleTaskData, 1);
    data->manager = g_object_ref(self);
    data->name = g_strdup(name);

    g_task_set_task_data(task, data, (GDestroyNotify)lifecycle_task_data_free);
    g_task_run_in_thread(task, delete_vm_thread_impl);
    g_object_unref(task);
}


gboolean purecvisor_vm_manager_delete_vm_finish(PureCVisorVmManager *manager __attribute__((unused)),
                                                GAsyncResult *res,
                                                GError **error) {
    return g_task_propagate_boolean(G_TASK(res), error);
}

























static void list_vms_thread(GTask *task,
                            gpointer source_object __attribute__((unused)),
                            gpointer task_data,
                            GCancellable *cancellable __attribute__((unused))) {
    PureCVisorVmManager *self = PURECVISOR_VM_MANAGER(task_data);
    GList *domains, *l;
    JsonBuilder *builder = json_builder_new();
    GError *err = nullptr;

    json_builder_begin_array(builder);



    if (!gvir_connection_fetch_domains(self->conn, NULL, &err)) {
        if (err) g_error_free(err);
    }


    domains = gvir_connection_get_domains(self->conn);






    for (l = domains; l != nullptr; l = l->next) {
        GVirDomain *dom = GVIR_DOMAIN(l->data);
        const gchar *name = gvir_domain_get_name(dom);
        const gchar *uuid = gvir_domain_get_uuid(dom);





        gint dom_id = gvir_domain_get_id(dom, NULL);

        const gchar *state_str = "shutoff";
        gboolean is_active = FALSE;

        if (dom_id > 0) {
            state_str = "running";
            is_active = TRUE;
        }


        gint vnc_port = -1;
        if (is_active) {
            vnc_port = _extract_vnc_port_from_domain(dom);
        }


        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, name);

        json_builder_set_member_name(builder, "uuid");
        json_builder_add_string_value(builder, uuid);

        json_builder_set_member_name(builder, "state");
        json_builder_add_string_value(builder, state_str);

        json_builder_set_member_name(builder, "vnc_port");
        if (vnc_port > 0) {
            json_builder_add_int_value(builder, vnc_port);
        } else {
            json_builder_add_null_value(builder);
        }
        json_builder_end_object(builder);
    }





    g_list_free_full(domains, (GDestroyNotify)g_object_unref);
    json_builder_end_array(builder);







    JsonNode *root = json_builder_get_root(builder);
    g_object_unref(builder);

    g_task_return_pointer(task, root, (GDestroyNotify)json_node_free);
}





void purecvisor_vm_manager_list_vms_async(PureCVisorVmManager *self,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data) {
    GTask *task = g_task_new(self, NULL, callback, user_data);



    g_task_set_task_data(task, g_object_ref(self), (GDestroyNotify)g_object_unref);

    g_task_run_in_thread(task, list_vms_thread);
    g_object_unref(task);
}







JsonNode *purecvisor_vm_manager_list_vms_finish(PureCVisorVmManager *manager __attribute__((unused)),
                                                GAsyncResult *res,
                                                GError **error) {
    return g_task_propagate_pointer(G_TASK(res), error);
}















typedef struct {
    gchar *vm_name;
    guint target_value;
} ResourceTuningData;


static void resource_tuning_data_free(ResourceTuningData *data) {
    if (data) {
        g_free(data->vm_name);
        g_free(data);
    }
}








static void set_memory_thread_impl(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    ResourceTuningData *data = (ResourceTuningData *)task_data;


    virConnectPtr raw_conn = virt_conn_pool_acquire();
    if (!raw_conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to open raw libvirt connection");
        return;
    }


    virDomainPtr raw_domain = virDomainLookupByName(raw_conn, data->vm_name);
    if (!raw_domain) {
        virt_conn_pool_release(raw_conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "VM '%s' not found", data->vm_name);
        return;
    }















    guint memory_kb = data->target_value * 1024;
    int ret = virDomainSetMemoryFlags(raw_domain, memory_kb, VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG);

    if (ret < 0) {
        virErrorPtr vir_err = virGetLastError();
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Memory tuning failed: %s", vir_err ? vir_err->message : "Unknown error");
    } else {
        g_task_return_boolean(task, TRUE);
    }


    virDomainFree(raw_domain);
    virt_conn_pool_release(raw_conn);
}







void purecvisor_vm_manager_set_memory_async(PureCVisorVmManager *self, const gchar *name, guint memory_mb, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data) {
    GTask *task = g_task_new(self, cancellable, callback, user_data);
    ResourceTuningData *data = g_new0(ResourceTuningData, 1);
    data->vm_name = g_strdup(name);
    data->target_value = memory_mb;

    g_task_set_task_data(task, data, (GDestroyNotify)resource_tuning_data_free);
    g_task_run_in_thread(task, set_memory_thread_impl);
    g_object_unref(task);
}


gboolean purecvisor_vm_manager_set_memory_finish(PureCVisorVmManager *self, GAsyncResult *res, GError **error) {
    return g_task_propagate_boolean(G_TASK(res), error);
}









static void set_vcpu_thread_impl(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    ResourceTuningData *data = (ResourceTuningData *)task_data;


    virConnectPtr raw_conn = virt_conn_pool_acquire();
    if (!raw_conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to open raw libvirt connection");
        return;
    }

    virDomainPtr raw_domain = virDomainLookupByName(raw_conn, data->vm_name);
    if (!raw_domain) {
        virt_conn_pool_release(raw_conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "VM '%s' not found", data->vm_name);
        return;
    }


    int ret = virDomainSetVcpusFlags(raw_domain, data->target_value, VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG);

    if (ret < 0) {
        virErrorPtr vir_err = virGetLastError();
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "vCPU tuning failed: %s", vir_err ? vir_err->message : "Unknown error");
    } else {
        g_task_return_boolean(task, TRUE);
    }

    virDomainFree(raw_domain);
    virt_conn_pool_release(raw_conn);
}







void purecvisor_vm_manager_set_vcpu_async(PureCVisorVmManager *self, const gchar *name, guint vcpu_count, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data) {
    GTask *task = g_task_new(self, cancellable, callback, user_data);
    ResourceTuningData *data = g_new0(ResourceTuningData, 1);
    data->vm_name = g_strdup(name);
    data->target_value = vcpu_count;

    g_task_set_task_data(task, data, (GDestroyNotify)resource_tuning_data_free);
    g_task_run_in_thread(task, set_vcpu_thread_impl);
    g_object_unref(task);
}


gboolean purecvisor_vm_manager_set_vcpu_finish(PureCVisorVmManager *self, GAsyncResult *res, GError **error) {
    return g_task_propagate_boolean(G_TASK(res), error);
}










typedef struct {
    gchar *name;
    gint   new_size_gb;
    gchar *target;
} ResizeDiskData;

static void resize_disk_data_free(ResizeDiskData *d) {
    g_free(d->name);
    g_free(d->target);
    g_free(d);
}

static void
audit_resize_disk_success(ResizeDiskData *d)
{
    gchar *target = g_strdup_printf("%s:%s", d->name, d->target ? d->target : "vda");
    gchar *job_id = g_strdup_printf("vm.resize_disk:%s", target);
    pcv_audit_log(NULL, "vm.resize_disk", target, "ok", 0, 0, "local");
    pcv_ws_broadcast_job_complete(job_id, "vm.resize_disk",
                                  "completed", NULL);
    g_free(job_id);
    g_free(target);
}

static void
audit_resize_disk_failure(ResizeDiskData *d, const gchar *error_msg)
{
    gchar *target = g_strdup_printf("%s:%s", d->name, d->target ? d->target : "vda");
    gchar *job_id = g_strdup_printf("vm.resize_disk:%s", target);
    pcv_audit_log(NULL, "vm.resize_disk", target, "fail", -32000, 0, "local");
    pcv_ws_broadcast_job_complete(job_id, "vm.resize_disk",
                                  "failed", error_msg ? error_msg : "unknown");
    g_free(job_id);
    g_free(target);
}











static void resize_disk_thread(GTask *task, gpointer source_object __attribute__((unused)),
                                gpointer task_data, GCancellable *cancel __attribute__((unused)))
{
    ResizeDiskData *d = (ResizeDiskData *)task_data;
    GError *error = nullptr;

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        audit_resize_disk_failure(d, "Failed to acquire libvirt connection");
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
            "Failed to acquire libvirt connection");
        return;
    }

    virDomainPtr dom = virDomainLookupByName(conn, d->name);
    if (!dom) {
        gchar *msg = g_strdup_printf("VM '%s' not found", d->name);
        audit_resize_disk_failure(d, msg);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
            "VM '%s' not found", d->name);
        virt_conn_pool_release(conn);
        g_free(msg);
        return;
    }















    gchar *xml = virDomainGetXMLDesc(dom, 0);
    gboolean is_zvol = FALSE;
    gchar *disk_source = nullptr;

    if (xml) {

        gchar *dev_tag = strstr(xml, "<source dev='");
        if (dev_tag) {
            dev_tag += strlen("<source dev='");
            gchar *end = strchr(dev_tag, '\'');
            if (end) {
                disk_source = g_strndup(dev_tag, (gsize)(end - dev_tag));
                is_zvol = g_str_has_prefix(disk_source, "/dev/zvol/") ||
                          g_str_has_prefix(disk_source, "/dev/zd");
            }
        }

        if (!disk_source) {
            gchar *file_tag = strstr(xml, "<source file='");
            if (file_tag) {
                file_tag += strlen("<source file='");
                gchar *end = strchr(file_tag, '\'');
                if (end) {
                    disk_source = g_strndup(file_tag, (gsize)(end - file_tag));
                    is_zvol = FALSE;
                }
            }
        }
        g_free(xml);
    }

    if (!disk_source) {
        gchar *msg = g_strdup_printf("Cannot determine disk source for VM '%s'", d->name);
        audit_resize_disk_failure(d, msg);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
            "Cannot determine disk source for VM '%s'", d->name);
        g_free(msg);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        return;
    }


    if (is_zvol) {

        const gchar *dataset = disk_source + strlen("/dev/zvol/");
        gchar *size_str = g_strdup_printf("%dG", d->new_size_gb);
        gchar *prop_str = g_strdup_printf("volsize=%s", size_str);
        const gchar *argv[] = {"zfs", "set", prop_str, (gchar *)dataset, NULL};
        gchar *std_err = nullptr;

        if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
            const gchar *err_msg = error ? error->message : (std_err ? std_err : "unknown");
            gchar *msg = g_strdup_printf("zfs set volsize failed: %s", err_msg);
            audit_resize_disk_failure(d, msg);
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                "zfs set volsize failed: %s",
                err_msg);
            if (error) g_error_free(error);
            g_free(std_err);
            g_free(size_str);
            g_free(prop_str);
            g_free(disk_source);
            virDomainFree(dom);
            virt_conn_pool_release(conn);
            g_free(msg);
            return;
        }
        g_free(std_err);
        g_free(size_str);
        g_free(prop_str);
    } else {

        gchar *size_str = g_strdup_printf("%dG", d->new_size_gb);
        const gchar *argv[] = {"qemu-img", "resize", disk_source, size_str, NULL};
        gchar *std_err = nullptr;

        if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
            const gchar *err_msg = error ? error->message : (std_err ? std_err : "unknown");
            gchar *msg = g_strdup_printf("qemu-img resize failed: %s", err_msg);
            audit_resize_disk_failure(d, msg);
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                "qemu-img resize failed: %s",
                err_msg);
            if (error) g_error_free(error);
            g_free(std_err);
            g_free(size_str);
            g_free(disk_source);
            virDomainFree(dom);
            virt_conn_pool_release(conn);
            g_free(msg);
            return;
        }
        g_free(std_err);
        g_free(size_str);
    }

















    virDomainInfo info;
    if (virDomainGetInfo(dom, &info) == 0 && info.state == VIR_DOMAIN_RUNNING) {
        const gchar *target = d->target ? d->target : "vda";
        unsigned long long new_size_kb = (unsigned long long)d->new_size_gb * 1024ULL * 1024ULL;
        int rc = virDomainBlockResize(dom, target, new_size_kb, 0);
        if (rc < 0) {
            virErrorPtr e = virGetLastError();
            PCV_LOG_WARN("vm_manager", "virDomainBlockResize failed for '%s': %s",
                         d->name, e ? e->message : "unknown");

        }
    }

    PCV_LOG_INFO("vm_manager", "VM '%s': disk resized to %dG (%s)",
                  d->name, d->new_size_gb, is_zvol ? "zvol" : "qcow2");

    g_free(disk_source);
    virDomainFree(dom);
    virt_conn_pool_release(conn);
    audit_resize_disk_success(d);
    g_task_return_boolean(task, TRUE);
}






void purecvisor_vm_resize_disk(const gchar *name, gint new_size_gb, const gchar *target) {
    ResizeDiskData *d = g_new0(ResizeDiskData, 1);
    d->name = g_strdup(name);
    d->new_size_gb = new_size_gb;
    d->target = target ? g_strdup(target) : g_strdup("vda");

    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, d, (GDestroyNotify)resize_disk_data_free);
    g_task_run_in_thread(task, resize_disk_thread);
    g_object_unref(task);
}























void
purecvisor_vm_manager_emit_metrics_updated(PureCVisorVmManager *self,
                                           GHashTable          *cache)
{
    g_return_if_fail(PURECVISOR_IS_VM_MANAGER(self));
    g_signal_emit(self, signals[SIGNAL_VM_METRICS_UPDATED], 0, cache);
}
