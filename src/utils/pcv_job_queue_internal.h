   
                                 
                                                   
                                                         
                                                      
   
#ifndef PCV_JOB_QUEUE_INTERNAL_H
#define PCV_JOB_QUEUE_INTERNAL_H
#include "pcv_job_recovery.h"
typedef enum { PCV_JOB_APPLIED, PCV_JOB_RETRY, PCV_JOB_CONFLICT } PcvJobApply;
                                                             
gchar *pcv_job_db_admit(const gchar *type, const gchar *target, const gchar *params,
                       const gchar *method, const gchar *generation);
                                                    
gboolean pcv_job_db_progress(const PcvJobRef *ref, gint progress, const gchar *detail);
PcvJobApply pcv_job_db_apply(const PcvJobRef *ref, PcvJobStatus status,
                            const gchar *result);
                                                        
                                                                 
gboolean pcv_job_db_reconcile(GHashTable *durable_ids);
#endif
