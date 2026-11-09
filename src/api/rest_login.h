   
                     
                                                                                     
  
                           
                                                                                                    
                                                                                                     
                                                                                                  
                                        
  
                                                                     
   
#ifndef PCV_REST_LOGIN_H
#define PCV_REST_LOGIN_H

#include <glib.h>
#include <libsoup/soup.h>

G_BEGIN_DECLS

                                                                    
#define PCV_REST_LOGIN_WORKERS      2
#define PCV_REST_LOGIN_INFLIGHT_MAX 12
                                                        
#define PCV_REST_LOGIN_BUSY_RETRY_SEC 2

                                                                  
typedef struct {
    gchar *username;
    gchar *password;
    gchar *client_ip;
    gchar *cfg_user;
    gchar *cfg_pass;
} PcvRestLoginRequest;

                                                                                              
typedef struct {
    guint        status;
    gchar       *json;
    const gchar *err_code;            
    gchar       *err_msg;
    gint         retry_after;
} PcvRestLoginResult;

                                                                           
void pcv_rest_login_run(const PcvRestLoginRequest *in, PcvRestLoginResult *out);
                         
void pcv_rest_login_result_clear(PcvRestLoginResult *out);

                                                                              
                                                                 
void pcv_rest_login_start(SoupServerMessage *msg, GMainContext *ctx,
                          const gchar *username, const gchar *password, const gchar *client_ip);

                                                             
void pcv_rest_login_pool_shutdown(void);

G_END_DECLS

#endif                       
