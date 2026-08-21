   
                        
                                                        
  
                           
                                                   
                                                    
                                        
  
                       
                                                        
                                                       
  
                               
                                
  
            
                                               
                                                   
                                         
  
           
                                                    
                                                         
                                                            
                                                   
                                                     
                                                  
                  
  
                                                
                                                                               
                                                  
                                                                             
                                                       
  
                                                  
                                  
                                                                      
                                   
                                     
  
                                                                       
  
                                                     
                                               
  
                                                      
                                                       
                                                      
                                            
                                                                     
                                                                      
                                      
                                                       
                                                                 
                                                                
                                                             
                                                       
                                                                     
                                                              
   
#include "iscsi_manager.h"
#include "modules/storage/pcv_iscsi_node_db.h"
#include "modules/storage/pcv_lio.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_log.h"
#include "utils/pcv_config.h"
#include "utils/pcv_secure.h"
#include <string.h>

                                              
#define ISCSI_LOG_DOM   "iscsi_mgr"

                                                 
                                                            
                                              
                                     
                                                    
                                                         
                                                    
#define ISCSI_IQN_PFX   PCV_LIO_IQN_PREFIX

                       
                                                                   
                                                                       
                                                                     
                                                     
                                                    
                                                     
                             
                                                   
                                                    
static const gchar *const ISCSI_LIO_MODULES[] = {
    "target_core_mod",                                                                
    "iscsi_target_mod",                                         
                                                                             
    "target_core_iblock",                                           
};

                                          
#define ISCSI_MAX_TID   64

                                
                                                                             
                                                  
                                               
#define ISCSI_CHAP_MAX_LEN  255

                                 
                                                                 
#define ISCSI_CHAP_NULL_TOKEN  "NULL"

                                                                
                             
                            
                                            
                                     
                         
                                                                        

   
               
                         
                                                      
                                                  
                                                             
                        
   
typedef struct {
    gchar *vm_name;                                   
    gint   tid;                                              
} IscsiTarget;

                                     
static struct {
    IscsiTarget targets[ISCSI_MAX_TID];                 
    gint        count;                                     
    gint        next_tid;                                      
    GMutex      mu;                                         
    gboolean    initialized;                              
} G = {0};

                                                                    
                                                           
                                                     

   
             
                                                
                                           
                       
                                                                   
                                      
                                                     
                             
   
static gboolean
_run_argv(const gchar *const *argv, gchar **out, GError **error)
{
    gchar *std_err = NULL;
    gboolean ok = pcv_spawn_sync(argv, out, &std_err, error);
    if (!ok && std_err)
        PCV_LOG_WARN(ISCSI_LOG_DOM, "cmd failed: %s → %s",
                     (argv && argv[0]) ? argv[0] : "?", std_err);
    g_free(std_err);
    return ok;
}

   
                      
                                              
                                         
                                                              
                                      
                                                      
                                          
   
static gchar *
_find_iscsi_device(const gchar *target_ip)
{
    const gchar *dir = "/dev/disk/by-path";
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return NULL;
    gchar *found = NULL;
    const gchar *name;
                                                                          
    while ((name = g_dir_read_name(d))) {
        if (g_strstr_len(name, -1, target_ip) && g_str_has_suffix(name, "lun-1")) {
            found = g_build_filename(dir, name, NULL);                               
            break;                                  
        }
    }
    g_dir_close(d);
    return found;
}

   
                
                                               
                            
  
                           
                                       
  
                                         
   
static IscsiTarget *
_find_target(const gchar *vm_name)
{
    for (gint i = 0; i < G.count; i++)
        if (g_strcmp0(G.targets[i].vm_name, vm_name) == 0)
            return &G.targets[i];
    return NULL;
}

   
                  
                                                  
                                               
                                                   
                                                  
                                            
                             
  
                                             
   
static gboolean
_chap_value_ok(const gchar *value, const gchar *label, GError **error)
{
                                                       
                                                
                                                   
    if (g_str_has_prefix(value, ISCSI_CHAP_NULL_TOKEN)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "iSCSI CHAP 설정 거부: %s 가 \"NULL\" 로 시작한다 — 커널이 해당 "
                    "자격을 미설정으로 되돌려 인증이 조용히 무력화된다", label);
        return FALSE;
    }

                                                    
                                       
    if (strlen(value) > ISCSI_CHAP_MAX_LEN) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "iSCSI CHAP 설정 거부: %s 가 %d자를 넘는다 — 커널이 조용히 "
                    "절단해 저장한다", label, ISCSI_CHAP_MAX_LEN);
        return FALSE;
    }

                                                              
                                                   
                             
    if (strpbrk(value, "\r\n") != NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "iSCSI CHAP 설정 거부: %s 에 개행 문자가 섞여 있다 — configfs 는 "
                    "개행을 제거하지 않으므로 자격의 일부가 된다", label);
        return FALSE;
    }

    return TRUE;
}

   
                           
                                                
                                              
                                 
  
                                   
                                                          
                                                     
                                                        
  
                                                
                                               
   
gboolean
pcv_iscsi_chap_validate(const gchar *chap_user, const gchar *chap_password,
                        GError **error)
{
    gboolean has_user = (chap_user     != NULL && *chap_user     != '\0');
    gboolean has_pass = (chap_password != NULL && *chap_password != '\0');

    if (!has_user && !has_pass)
        return TRUE;                            

                                                          
                                                              
                                                       
                                                              
    if (!has_user || !has_pass) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "iSCSI CHAP 설정이 반쪽이다([iscsi] chap_user %s, chap_password %s) "
                    "— 인증 없이 익스포트하지 않도록 타겟 생성을 거부한다. 둘 다 설정하거나 "
                    "둘 다 비워라(chap_password 가 암호화 저장된 경우 복호 실패도 이 경로다)",
                    has_user ? "있음" : "없음", has_pass ? "있음" : "없음");
        return FALSE;
    }

    return _chap_value_ok(chap_user, "chap_user", error)
        && _chap_value_ok(chap_password, "chap_password", error);
}

                                                             

   
                  
                                                 
                                                   
                                                   
                              
                                       
                                          
  
                    
                                                                    
                                                  
                                                                   
                                                                  
                                                              
                                                                        
                                                  
                                                                   
                                                                     
                                                       
                                                         
                                                        
                                                                       
                                             
  
                                                   
                                                     
                                                                   
                                                                       
                                                 
                                                         
                                                       
                                                         
                                                            
                               
  
                                       
                                                        
                                                         
  
                                             
                                                 
  
                                                            
                                                       
   
void
pcv_iscsi_init(void)
{
    pcv_iscsi_init_at(PCV_LIO_ROOT);
}

   
                     
                                                
                                                             
                                
  
                                                     
                                                
                                          
  
                                                       
   
void
pcv_iscsi_init_at(const gchar *root)
{
    g_mutex_init(&G.mu);
    G.count = 0;
    G.next_tid = 1;
    G.initialized = TRUE;

    const gchar *how = "즉시";                                

    if (!pcv_lio_available(root)) {
                                                       
                                                                      
        for (gsize i = 0; i < G_N_ELEMENTS(ISCSI_LIO_MODULES); i++) {
            const gchar *argv[] = { "modprobe", ISCSI_LIO_MODULES[i], NULL };
            GError *e = NULL;

            if (!pcv_spawn_sync(argv, NULL, NULL, &e)) {
                PCV_LOG_WARN(ISCSI_LOG_DOM, "커널 모듈 로드 실패(보조 경로, %s): %s",
                             ISCSI_LIO_MODULES[i], e ? e->message : "unknown");
                g_clear_error(&e);
            }
        }
        how = "보조 modprobe 후";
    }

    if (!pcv_lio_available(root)) {
        PCV_LOG_WARN(ISCSI_LOG_DOM,
                     "iSCSI 매니저 degraded — 커널 LIO 미가용(%s 부재: configfs 미마운트 "
                     "또는 target_core_mod 미로드). 타겟 생성이 실패한다. "
                     "/etc/modules-load.d/purecvisor-lio.conf 와 configfs 마운트를 "
                     "확인하라(이니시에이터 기능은 영향 없음)",
                     root);
        return;
    }

                                                            
                                                             
                                                         
                                                        
                              
    GError *fabric_err = NULL;

    if (pcv_lio_ensure_fabric(root, &fabric_err)) {
        PCV_LOG_INFO(ISCSI_LOG_DOM,
                     "iSCSI 매니저 초기화 완료 — 커널 LIO 사용 가능(%s, %s, iscsi fabric 등록됨)",
                     root, how);
    } else {
        PCV_LOG_WARN(ISCSI_LOG_DOM,
                     "iSCSI 매니저 degraded — %s 는 있으나 iSCSI fabric 등록 실패(%s). "
                     "iscsi_target_mod 가 로드되지 않았을 가능성이 가장 크다"
                     "(커널은 이 mkdir 에서 request_module 자동 로드를 시도한다). "
                     "modprobe iscsi_target_mod 와 "
                     "/etc/modules-load.d/purecvisor-lio.conf 를 확인하라"
                     "(이니시에이터 기능은 영향 없음)",
                     root, fabric_err->message);
        g_clear_error(&fabric_err);
    }
}

   
                      
                                             
                                
                                           
  
                                                   
                                           
                                                  
                                        
   
void
pcv_iscsi_shutdown(void)
{
    if (!G.initialized) return;                                           
    g_mutex_lock(&G.mu);
    for (gint i = 0; i < G.count; i++)
        g_free(G.targets[i].vm_name);                                 
    G.count = 0;
    g_mutex_unlock(&G.mu);
    g_mutex_clear(&G.mu);                                              
    G.initialized = FALSE;                                    
}

                                                            

   
                           
                                                     
                                                
                                          
  
                                                           
                                                        
                                                                     
                                                    
                                                  
  
                                                               
                                                                        
                                                           
                                                                 
                                  
  
                                              
                                                        
                                                    
  
                                                                 
                                                         
  
                                                        
                                                                           
                                    
                              
   
gboolean
pcv_iscsi_target_create(const gchar *vm_name, const gchar *zvol_path, GError **error)
{
    return pcv_iscsi_target_create_at(PCV_LIO_ROOT, vm_name, zvol_path, error);
}

   
                              
                                               
                                                              
                                            
  
                                                               
                                                        
                                                         
                                                               
  
                                                       
   
gboolean
pcv_iscsi_target_create_at(const gchar *root, const gchar *vm_name,
                           const gchar *zvol_path, GError **error)
{
    if (!G.initialized) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "iSCSI not initialized");
        return FALSE;
    }

                                                         
                                                   
                                                                   
                                                  
                                                      
      
                                                               
                                                  
                                                                
                                                       
                                                           
                                                            
    if (!pcv_lio_available(root)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                    "커널 LIO 미가용(%s 부재) — 이 노드는 degraded 다. "
                    "target_core_mod·iscsi_target_mod·target_core_iblock 모듈 로드와 "
                    "configfs 마운트를 확인하라(권한 문제가 아니다)", root);
        return FALSE;
    }

                                              
    g_mutex_lock(&G.mu);
    if (_find_target(vm_name)) {                                    
        g_mutex_unlock(&G.mu);
        return TRUE;                  
    }
    if (G.count >= ISCSI_MAX_TID) {                       
        g_mutex_unlock(&G.mu);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Max iSCSI targets reached");
        return FALSE;
    }

    gint tid = G.next_tid++;                                        

                                                    
                                                 
                                                
                                             
    const gchar *chap_user = pcv_config_get_string("iscsi", "chap_user", NULL);
    gchar *chap_password = pcv_config_get_secret("iscsi", "chap_password", NULL);

                                                                  
                                                                
                                                    
                                                  
                                                                  
                                                    
                                                            
                                       
    gboolean created = pcv_iscsi_chap_validate(chap_user, chap_password, error)
        && pcv_lio_target_create_at(root, vm_name, zvol_path,
                                    chap_user, chap_password, error);

    pcv_secure_free_str(&chap_password);                                        

    if (!created) {
        g_mutex_unlock(&G.mu);
                                                         
                                                                 
        return FALSE;
    }

                                        
                                                        
    IscsiTarget *t = &G.targets[G.count++];                           
    t->vm_name = g_strdup(vm_name);                                             
    t->tid = tid;

    g_mutex_unlock(&G.mu);

                                                             
                                                      
    gchar *iqn = pcv_lio_iqn_for_vm(vm_name);

    PCV_LOG_INFO(ISCSI_LOG_DOM, "iSCSI 타겟 생성 완료: tid=%d iqn=%s backing=%s",
                 tid, iqn ? iqn : "(이름 거부됨)", zvol_path);
    g_free(iqn);
    return TRUE;
}

   
                           
                                            
                                                 
            
                                                          
                                                           
  
                                          
                                                 
                                                
                                               
                                                         
                                  
  
                                                                
                                                               
                                        
                                                                     
                                                   
                                                       
  
                                               
                                              
  
                                                                 
                                                         
  
                       
                                  
                                                            
   
gboolean
pcv_iscsi_target_delete(const gchar *vm_name, GError **error)
{
    return pcv_iscsi_target_delete_at(PCV_LIO_ROOT, vm_name, error);
}

   
                              
                                               
                                                              
                                            
  
                                                       
                                                      
                                                
                                                       
                                                                  
  
                                                       
   
gboolean
pcv_iscsi_target_delete_at(const gchar *root, const gchar *vm_name, GError **error)
{
    if (!G.initialized) return TRUE;

    g_mutex_lock(&G.mu);
    IscsiTarget *t = _find_target(vm_name);                                

    if (!pcv_lio_target_delete_at(root, vm_name, error)) {
                                                    
                                                       
                                         
        PCV_LOG_WARN(ISCSI_LOG_DOM,
                     "LIO 타겟 삭제 실패(vm=%s) — 커널 configfs 에 잔존할 수 있다",
                     vm_name ? vm_name : "(null)");
        g_mutex_unlock(&G.mu);
        return FALSE;
    }

    if (!t) {                                            
        g_mutex_unlock(&G.mu);
        return TRUE;
    }

    g_free(t->vm_name);                                  
                                                   
                                      
                                                                    
    gint idx = (gint)(t - G.targets);                          
    if (idx < G.count - 1)                                            
        G.targets[idx] = G.targets[G.count - 1];
    G.count--;                                      

    g_mutex_unlock(&G.mu);
    PCV_LOG_INFO(ISCSI_LOG_DOM, "iSCSI 타겟 삭제 완료: %s", vm_name);
    return TRUE;
}

   
                         
                                                     
                                   
                                                                          
  
                                                     
                                                      
                                                         
                      
  
                                            
   
JsonArray *
pcv_iscsi_target_list(void)
{
    JsonArray *arr = json_array_new();
    if (!G.initialized) return arr;                                         

    g_mutex_lock(&G.mu);
                                           
    for (gint i = 0; i < G.count; i++) {
        JsonObject *obj = json_object_new();
        json_object_set_string_member(obj, "vm_name", G.targets[i].vm_name);
        json_object_set_int_member(obj, "tid", G.targets[i].tid);
        gchar *iqn = pcv_lio_iqn_for_vm(G.targets[i].vm_name);                         
                                                               
                                                                  
        json_object_set_string_member(obj, "iqn", iqn ? iqn : "");
        g_free(iqn);                                              
        json_array_add_object_element(arr, obj);
    }
    g_mutex_unlock(&G.mu);
    return arr;
}

                                                        
                                                         
                                                
                                                                       
                                                                
                                                       
                                                             
                                                              
                                                  
   

                                                   

   
                               
                                               
                                                
                                          
  
         
                                                           
                                                                 
                                                 
                                                 
  
                                                       
                                                     
                                                             
                                        
                                       
                                  
   
gboolean
pcv_iscsi_initiator_connect(const gchar *target_ip, const gchar *vm_name,
                             gchar **device_path, GError **error)
{
    gchar *iqn = g_strdup_printf("%s:%s", ISCSI_IQN_PFX, vm_name);

                                                           
                                                                
                                            

                                                        
                                                            
                                                                     
                                                               
                                                                    
    const gchar *disc[] = { "iscsiadm", "-m", "discoverydb", "-t", "sendtargets",
                            "-p", target_ip, "--discover", NULL };
    _run_argv(disc, NULL, NULL);                                               

                                                                   
      
                                                                    
                                                                         
                                                              
                                                     
                                                
                                                      
                                           
    {
        const gchar *chap_user = pcv_config_get_string("iscsi", "chap_user", NULL);
        gchar *chap_password = pcv_config_get_secret("iscsi", "chap_password", NULL);
        if (!pcv_iscsi_chap_validate(chap_user, chap_password, error)) {
            pcv_secure_free_str(&chap_password);
            g_free(iqn);
            return FALSE;
        }
        if (chap_user && chap_password && *chap_user && *chap_password) {
            if (!pcv_iscsi_node_db_set_chap(iqn, target_ip, chap_user,
                                             chap_password, error)) {
                pcv_secure_free_str(&chap_password);
                g_free(iqn);
                return FALSE;
            }
            PCV_LOG_INFO(ISCSI_LOG_DOM,
                         "CHAP auth configured in open-iscsi node DB for %s",
                         target_ip);
        }
        pcv_secure_free_str(&chap_password);                               
    }

                                                  
                                                         
    const gchar *login[] = { "iscsiadm", "-m", "node", "--targetname", iqn,
                             "--portal", target_ip, "--login", NULL };
    if (!_run_argv(login, NULL, error)) {                                                              
        g_free(iqn);
        return FALSE;
    }

                                                                         
                                                        
    if (device_path) {                                         
        *device_path = _find_iscsi_device(target_ip);                                   
    }

    PCV_LOG_INFO(ISCSI_LOG_DOM, "iSCSI initiator connected: %s@%s", iqn, target_ip);
    g_free(iqn);
    return TRUE;
}

   
                                  
                                                     
                         
                                      
  
                                  
                                             
                                    
                                   
   
gboolean
pcv_iscsi_initiator_disconnect(const gchar *target_ip, const gchar *vm_name,
                                GError **error)
{
    gchar *iqn = g_strdup_printf("%s:%s", ISCSI_IQN_PFX, vm_name);
                                                      
    const gchar *logout[] = { "iscsiadm", "-m", "node", "--targetname", iqn,
                              "--portal", target_ip, "--logout", NULL };
    gboolean ok = _run_argv(logout, NULL, error);
    g_free(iqn);

    if (ok)
        PCV_LOG_INFO(ISCSI_LOG_DOM, "iSCSI initiator disconnected: %s@%s", vm_name, target_ip);
    return ok;
}
