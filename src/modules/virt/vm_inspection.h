                           
                           
                                                              
                                                                            
                                                                       
                                                    
                                       
   
#ifndef PCV_VM_INSPECTION_H
#define PCV_VM_INSPECTION_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <libvirt/libvirt.h>

#define PCV_VM_INSPECTION_BUILD "vm-inspection-candidate-0.1.0"
#define PCV_VM_INSPECTION_XML_LIMIT (1024U * 1024U)
#define PCV_VM_INSPECTION_DISK_LIMIT 64U
#define PCV_VM_INSPECTION_OUTPUT_LIMIT (256U * 1024U)

typedef enum {
    PCV_VM_INSPECTION_INVALID,
    PCV_VM_INSPECTION_READ_FAILED
} PcvVmInspectionError;

                                                         
GQuark pcv_vm_inspection_error_quark(void);

                                                    
gboolean pcv_vm_inspection_uuid_valid(const gchar *uuid);

                                                                      
                                                               
                                                        
JsonNode *pcv_vm_inspection_collect(virConnectPtr conn, const gchar *uuid,
                                   GCancellable *cancel, gint64 deadline_us,
                                   GError **error);

#endif
