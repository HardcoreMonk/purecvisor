   
                           
                                                                                           
  
                           
                                                                                                         
                                                                                                
                                                                                                   
                                                               
                                                                                                    
                                                                                       
                                                                                              
                                                                                                      
                                                                              
                                                                       
  
                                                                             
                                                                  
  
            
                                                                                 
                                                               
   
#ifndef PURECVISOR_DISPATCHER_HOST_DEVICE_SCAN_H
#define PURECVISOR_DISPATCHER_HOST_DEVICE_SCAN_H

#include <glib.h>

#include "modules/dispatcher/hw_ids.h"

G_BEGIN_DECLS

                                                       
                                                         
typedef enum {
    PCV_HOST_USB_HUB       = 1u << 0,                                          
    PCV_HOST_USB_STORAGE   = 1u << 1,                       
    PCV_HOST_USB_BLUETOOTH = 1u << 2,                                                               
    PCV_HOST_USB_KEYBOARD  = 1u << 3,                                   
    PCV_HOST_USB_MOUSE     = 1u << 4,                                   
    PCV_HOST_USB_AUDIO     = 1u << 5,                       
} PcvHostUsbTrait;

                                                                                              
                                                  
typedef struct {
    gchar *cls;
    gchar *sub;
    gchar *proto;
    gchar *driver;
} PcvHostUsbIface;

   
                                                                                                              
                                                                                                     
                                                                                                  
                                                          
                                                     
   
typedef struct {
    gchar     *port;
    guint16    vendor;
    guint16    product;
    guint      bus;
    guint      devnum;
    gchar     *device_class;
    gchar     *manufacturer;
    gchar     *product_name;
    GPtrArray *ifaces;
    guint      traits;
    gboolean   host_driver_bound;
    gboolean   duplicate_on_host;
} PcvHostUsbDevice;

                                                                                                                  
                                                                                                        
                                                             
typedef struct {
    gchar *bdf;
    gchar *class_code;
    gchar *vendor_id;
    gchar *device_id;
    gchar *driver;
    gchar *iommu_group;
} PcvHostPciFunc;

   
                                                                                                                             
                                                                                                                
                                                                                        
                                                                                     
                                                                      
   
typedef struct {
    PcvHostPciFunc fn;
    gboolean       boot_vga;
    gboolean       display_connected;
    gboolean       host_display;
    gboolean       group_safe;
    gboolean       group_vfio_ready;
    GPtrArray     *companions;
} PcvHostGpu;

   
                                                                                               
                                                                                                 
                                                                                   
                                                   
                                                                                        
                             
   
typedef struct {
    GPtrArray  *usb;
    GPtrArray  *gpus;
    GHashTable *pci_bdfs;
    gboolean    usb_absent;
    gboolean    pci_absent;
    PcvHwIds   *usb_ids;
    PcvHwIds   *pci_ids;
} PcvHostScan;

   
                                                                                                            
                                                                                                           
                                                                                                                   
                                                                                               
                   
                                                                         
   
gboolean pcv_host_device_scan(const gchar *root, PcvHostScan **out, GError **error);

                                                                        
void pcv_host_device_scan_free(PcvHostScan *scan);

   
                                                                                                
                                                                                           
                                                                                                        
                                        
                                                    
   
gchar *pcv_host_usb_name(const PcvHwIds *usb_ids, guint16 vendor, guint16 product, const gchar *manufacturer,
                         const gchar *product_name, gchar **vendor_name_out);

G_END_DECLS

#endif                                               
