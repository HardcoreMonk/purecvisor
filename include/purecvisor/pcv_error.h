
#ifndef PURECVISOR_ERROR_H
#define PURECVISOR_ERROR_H

#include <glib.h>

G_BEGIN_DECLS

#define PCV_VM_ERROR   (pcv_vm_error_quark())
GQuark pcv_vm_error_quark(void);

typedef enum {
    PCV_VM_ERR_NOT_FOUND       = 1,
    PCV_VM_ERR_ALREADY_EXISTS  = 2,
    PCV_VM_ERR_BUSY            = 3,
    PCV_VM_ERR_INVALID_STATE   = 4,
    PCV_VM_ERR_LIBVIRT_FAILED  = 5,
    PCV_VM_ERR_XML_FAILED      = 6,
    PCV_VM_ERR_INTERNAL        = 7,
} PcvVmErrorCode;

#define PCV_LXC_ERROR  (pcv_lxc_error_quark())
GQuark pcv_lxc_error_quark(void);

typedef enum {
    PCV_LXC_ERR_NOT_FOUND      = 1,
    PCV_LXC_ERR_ALREADY_EXISTS = 2,
    PCV_LXC_ERR_NOT_RUNNING    = 3,
    PCV_LXC_ERR_CMD_FAILED     = 4,
    PCV_LXC_ERR_CONFIG_FAILED  = 5,
    PCV_LXC_ERR_INTERNAL       = 6,
} PcvLxcErrorCode;

#define PCV_VALIDATE_ERROR (pcv_validate_error_quark())
GQuark pcv_validate_error_quark(void);

typedef enum {
    PCV_VALIDATE_ERR_NAME      = 1,
    PCV_VALIDATE_ERR_SNAP_NAME = 2,
    PCV_VALIDATE_ERR_PATH      = 3,
    PCV_VALIDATE_ERR_BRIDGE    = 4,
    PCV_VALIDATE_ERR_RANGE     = 5,
    PCV_VALIDATE_ERR_IMAGE     = 6,
    PCV_VALIDATE_ERR_CMD       = 7,
} PcvValidateErrorCode;

#define PCV_STRDUP_PRINTF(fmt, ...) \
    ({ gchar *_s = g_strdup_printf((fmt), __VA_ARGS__); \
       if (G_UNLIKELY(!_s)) \
           g_warning("g_strdup_printf returned NULL at %s:%d", __FILE__, __LINE__); \
       _s; })

G_END_DECLS

#endif
