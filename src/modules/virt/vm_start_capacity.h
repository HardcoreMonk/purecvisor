





#ifndef PCV_VM_START_CAPACITY_H
#define PCV_VM_START_CAPACITY_H

#include <gio/gio.h>
#include <libvirt/libvirt.h>



typedef gboolean (*PcvVmStartPrepare)(virDomainPtr *domain_io, gpointer data,
                                     GError **error);





gboolean pcv_vm_start_with_capacity(virDomainPtr *domain_io,
                                     const gchar *sysfs_root,
                                     PcvVmStartPrepare prepare,
                                     gpointer prepare_data,
                                     GError **error);

#endif
