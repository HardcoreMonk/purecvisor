   
                           
                                       
                                                              
                                                        
                                                
                                         
   
#ifndef PCV_JOB_RECOVERY_H
#define PCV_JOB_RECOVERY_H
#include "pcv_job_queue.h"
G_BEGIN_DECLS
typedef struct _PcvJobRef PcvJobRef;
typedef void (*PcvJobFinished)(const PcvJobRef *ref, PcvJobStatus status,
                              const gchar *error, gboolean result_persisted,
                              gboolean recovery_pending, const gchar *reason,
                              gpointer user_data);
                                                       
                                                                
PcvJobRef *pcv_job_admit(const gchar *type, const gchar *target,
                        const gchar *params, const gchar *method);
                                           
PcvJobRef *pcv_job_ref(PcvJobRef *ref);
void pcv_job_unref(PcvJobRef *ref);
                                          
const gchar *pcv_job_id(const PcvJobRef *ref);
const gchar *pcv_job_generation(const PcvJobRef *ref);
const gchar *pcv_job_method(const PcvJobRef *ref);
const gchar *pcv_job_target(const PcvJobRef *ref);
                                                   
gboolean pcv_job_progress(PcvJobRef *ref, gint progress, const gchar *detail);
                                                        
                                                                      
                                                         
void pcv_job_finish_async(PcvJobRef *ref, PcvJobStatus status,
                          const gchar *result, const gchar *error,
                          PcvJobFinished callback, gpointer data,
                          GDestroyNotify destroy);
                                                        
                                                                
void pcv_job_recovery_init(const gchar *db_path);
void pcv_job_recovery_shutdown(void);
gboolean pcv_job_recovery_pending(const gchar *id);
gboolean pcv_job_recovery_protected(const gchar *id);
                                                     
gchar *pcv_job_recovery_reason(const gchar *id);
G_END_DECLS
#endif
