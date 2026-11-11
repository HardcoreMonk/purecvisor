   
                          
                                                                    
  
                           
                                                                                      
                                                                                
                                                                  
                                                                                    
                                                                                 
                                
                                                                      
  
                                                                 
                                           
  
            
                                                                                                             
  
                                                               
   
#ifndef PURECVISOR_DISPATCHER_HOTPLUG_USB_XML_H
#define PURECVISOR_DISPATCHER_HOTPLUG_USB_XML_H

#include <glib.h>

G_BEGIN_DECLS

   
                                                                     
                                                                          
                                                                             
                                                                 
                                                                                                    
                                                                     
                                                                                                               
                                                        
                                                                            
   
typedef struct {
    gchar    *vendor_id;
    gchar    *product_id;
    guint16   vendor;
    guint16   product;
    gboolean  live;
    gboolean  persistent;
    gboolean  missing;
    gchar    *startup_policy;
    guint     live_bus;
    guint     live_device;
} PcvUsbHostdevEntry;

   
                                                                         
                                                                                    
                                                                 
                                                                                  
                                                                    
                                                                                           
   
gboolean pcv_usb_hostdev_list(const gchar *live_xml,
                              const gchar *config_xml,
                              GPtrArray **entries_out,
                              GError **error);

   
                                                                                                                 
                                                
                                                                      
   
typedef enum {
    PCV_USB_POLICY_MANDATORY,
    PCV_USB_POLICY_REQUISITE,
    PCV_USB_POLICY_OPTIONAL,
} PcvUsbPolicy;

                                                                                                       
                                             
gboolean pcv_usb_policy_parse(const gchar *s, PcvUsbPolicy *out);

                                                           
const gchar *pcv_usb_policy_name(PcvUsbPolicy policy);

                                                                          
                                                                       
guint16 pcv_usb_id_value(const gchar *validated_id);

                                                                                                  
                                                                                          
                                            
gchar *pcv_usb_hostdev_xml(guint16 vendor, guint16 product, const PcvUsbPolicy *policy_or_null);

   
                                                                                                        
                                                                                                      
                                                   
                                                
   
typedef enum {
    PCV_USB_ATTACH_NOOP,
    PCV_USB_ATTACH_TWO_STEP,
    PCV_USB_ATTACH_LIVE_ONLY,
    PCV_USB_ATTACH_CONFIG_LEGACY,
    PCV_USB_ATTACH_CONFIG_STOPPED,
} PcvUsbAttachKind;

                                                                                            
                                                                     
                                                            
GQuark pcv_usb_error_quark(void);
#define PCV_USB_ERROR (pcv_usb_error_quark())
typedef enum {
    PCV_USB_ERROR_CONFLICT,
    PCV_USB_ERROR_NOT_ATTACHED,
} PcvUsbError;

   
                                                                                                   
                                                                                               
                                                                                     
                                                                                   
                                                                             
                                                                                                              
                                                                                                                     
                                                                                       
                                                                                             
                                                                     
   
gboolean pcv_usb_attach_plan(const gchar *live_xml,
                             const gchar *config_xml,
                             gboolean is_active,
                             gboolean has_persistent,
                             guint16 vendor,
                             guint16 product,
                             PcvUsbPolicy policy,
                             gboolean persist,
                             const gchar *vm_name,
                             PcvUsbAttachKind *kind_out,
                             gchar **persistent_policy_out,
                             GError **error);

   
                                                                                                        
                                                                                                 
                                                                                                           
                                                                                               
                                                                                
                                                          
   
gboolean pcv_usb_detach_plan(const gchar *live_xml,
                             const gchar *config_xml,
                             gboolean is_active,
                             gboolean has_persistent,
                             guint16 vendor,
                             guint16 product,
                             const gchar *vm_name,
                             gboolean *live_out,
                             gboolean *config_out,
                             GError **error);

G_END_DECLS

#endif                                              
