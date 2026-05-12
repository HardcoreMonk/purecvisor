










#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "purecvisor/pcv_validate.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {

    char *s = (char *)malloc(size + 1);
    if (!s) return 0;
    if (size) memcpy(s, data, size);
    s[size] = '\0';


    pcv_validate_vm_name(s);
    pcv_validate_snap_name(s);
    pcv_validate_bridge_name(s);
    pcv_validate_iso_path(s);
    pcv_validate_container_image(s);
    pcv_validate_exec_cmd(s);
    pcv_validate_pci_addr(s);
    pcv_validate_cidr(s);


    if (size >= 8) {
        gint64 n;
        memcpy(&n, data, sizeof(n));
        pcv_validate_memory_mb(n);
        pcv_validate_vcpu(n);
        pcv_validate_disk_gb(n);
    }


    GError *err = NULL;
    pcv_validate_vm_create_params(s, 2, 1024, 10, NULL, NULL, &err);
    if (err) g_error_free(err);

    err = NULL;
    pcv_validate_network_create_params(s, "nat", s, NULL, &err);
    if (err) g_error_free(err);

    free(s);
    return 0;
}
