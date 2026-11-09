   
                          
                                                                                   
  
                           
                                                                             
                                                                                                              
                                                                                                             
                                                                                      
                                                                                       
                                                              
                                                                                             
                                                                              
                                  
  
                                                                             
                                      
   
#include "pcv_rbac.h"
#include "pcv_rbac_internal.h"

#include <glib.h>
#include <sqlite3.h>

   
                       
                        
  
                     
                                                
                                                                       
  
                                                    
                                                
  
             
                                         
                                            
                                       
  
                                                                   
                                                                                          
   
const gchar *
pcv_rbac_get_tenant(const gchar *username)
{
                                                      
                                                      
                                                     
    static __thread gchar t_tenant[256];

    if (!username || !*username) return NULL;

    sqlite3 *db = pcv_rbac_db_lock();

    sqlite3_stmt *stmt = nullptr;
    int rc = db ? sqlite3_prepare_v2(db,
        "SELECT tenant FROM users WHERE username = ?;",
        -1, &stmt, NULL) : SQLITE_MISUSE;                                 

    if (rc != SQLITE_OK) {
        pcv_rbac_db_unlock();
        return NULL;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);

    const gchar *result = nullptr;
    if (rc == SQLITE_ROW) {
        const gchar *val = (const gchar *)sqlite3_column_text(stmt, 0);
        if (val) {
            g_strlcpy(t_tenant, val, sizeof(t_tenant));
            result = t_tenant;
        }
    }

    sqlite3_finalize(stmt);
    pcv_rbac_db_unlock();

    return result;
}

   
                                                      
  
                                                                                                           
                                                                                                          
                                                                   
                                                                                 
   
PcvRbacTenantResult
pcv_rbac_user_set_tenant_if_empty(const gchar *username, const gchar *tenant)
{
    if (!username || !*username || !tenant || !*tenant)
        return PCV_RBAC_TENANT_ERROR;

    sqlite3 *db = pcv_rbac_db_lock();
    if (!db) {
        pcv_rbac_db_unlock();
        return PCV_RBAC_TENANT_ERROR;
    }

    PcvRbacTenantResult result = PCV_RBAC_TENANT_ERROR;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db,
            "UPDATE users SET tenant = ?1 WHERE username = ?2 AND (tenant IS NULL OR tenant = '');",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, tenant, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, username, -1, SQLITE_STATIC);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        stmt = nullptr;
        if (rc == SQLITE_DONE && sqlite3_changes(db) == 1)
            result = PCV_RBAC_TENANT_SET;
        else if (rc == SQLITE_DONE &&
                 sqlite3_prepare_v2(db, "SELECT tenant FROM users WHERE username = ?;", -1, &stmt, NULL) == SQLITE_OK) {
                                                            
            sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
            rc = sqlite3_step(stmt);
            if (rc == SQLITE_ROW)
                result = PCV_RBAC_TENANT_ALREADY;
            else if (rc == SQLITE_DONE)
                result = PCV_RBAC_TENANT_NO_USER;
            sqlite3_finalize(stmt);
        }
    }
    pcv_rbac_db_unlock();
    return result;
}
