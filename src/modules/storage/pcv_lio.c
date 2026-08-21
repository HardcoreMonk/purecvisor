                                
                                                                 
  
                           
                                                   
                                                    
                                        
                                
  
                                                    
                                                    
                                                      
                                                     
                                                         
                                             
                                      
  
                                                       
                                                                         
                                                                
                                                         
   
#include "modules/storage/pcv_lio.h"

#include "utils/pcv_log.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

                                                         
                                               
#define LIO_LOG_DOM     "lio"

                                              
                                                    
                                               
                                                    
#define LIO_LUN_DIRNAME     "lun_0"
#define LIO_PORTAL_NAME     "0.0.0.0:3260"

                                                  
  
                                                    
                                            
                                                           
                                                            
                                                     
                                            
                                              
  
                                                     
                                              
                                   
                                                   
  
                                             
                                                               
                                            
                                               
                                            
#define LIO_PURGE_MAX_DEPTH 2

                                              
                                                           
                                            
                                           
                                           
                                                       
                                                              
                                            
                                              
                                                    
       
  
                                                            
                                                           
                                                                   
                                             
                                                                          
                                                  
                                                 
                                                            
                                               
                                              
static gboolean
_lio_name_traversal_safe(const gchar *name)
{
    return name != NULL && name[0] != '\0'
        && strcmp(name, ".") != 0
        && strstr(name, "..") == NULL
        && strchr(name, '/') == NULL;
}

   
                                                           
  
                                                   
                                                         
                                                      
                                              
                      
  
                                              
                                                 
  
                                              
                                                       
                                                         
                                                                         
   
gchar *
pcv_lio_path_backstore(const gchar *root, const gchar *store)
{
    if (!_lio_name_traversal_safe(store))
        return NULL;

    return g_build_filename(root, "core", "iblock_0", store, NULL);
}

   
                                                     
  
                                                           
                                               
                                                   
                                                
  
                                                    
                                             
  
                               
                                                                     
                           
                                                        
   
gchar *
pcv_lio_path_target(const gchar *root, const gchar *iqn)
{
    if (!_lio_name_traversal_safe(iqn))
        return NULL;

    return g_build_filename(root, "iscsi", iqn, NULL);
}

   
                                                         
  
                                                  
                                                
                                                              
           
  
                                                  
                                   
  
                                                       
                            
                                                             
                                                           
   
gchar *
pcv_lio_path_tpg(const gchar *root, const gchar *iqn)
{
    gchar *target = pcv_lio_path_target(root, iqn);                  

    if (!target)
        return NULL;

    gchar *tpg = g_build_filename(target, "tpgt_1", NULL);

    g_free(target);
    return tpg;
}

   
                                                          
  
                                                      
                                               
                                               
  
                                                  
                                            
  
                               
                                                                 
                                                            
                                        
                                                         
   
gchar *
pcv_lio_iqn_for_vm(const gchar *vm_name)
{
    if (!_lio_name_traversal_safe(vm_name))
        return NULL;

    return g_strdup_printf("%s:%s", PCV_LIO_IQN_PREFIX, vm_name);
}

   
                                                        
  
                                                            
                                                                
                                                
                                                      
                  
  
                                             
                                              
                                              
                                              
                              
  
                                                             
                                             
                              
                                               
                                                            
                                                        
                                        
                                                                          
                                         
   
gboolean
pcv_lio_mkdir_p(const gchar *path, GError **error)
{
                                                              
                                                        
                                                 
    if (g_mkdir_with_parents(path, 0755) != 0) {
        gint saved_errno = errno;

        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "configfs mkdir 실패(%s): %s", path, g_strerror(saved_errno));
        return FALSE;
    }

    return TRUE;
}

   
                                                                       
  
                                             
                                                              
                                        
                                                                      
                                      
                                                            
                                                  
                                                       
                              
  
                                          
                                                 
                                             
                                                  
                                     
  
                                                      
                                                 
                                                                
                                          
                                                      
                                                    
                                    
                                             
                                                                   
                                                            
                                                   
                                                       
                                                 
                                             
                                                             
   
gboolean
pcv_lio_write_attr(const gchar *dir, const gchar *attr,
                   const gchar *value, GError **error)
{
                                                           
                                                            
                                                       
                                             
                                                     
    gsize len = value ? strlen(value) : 0;

    if (len == 0) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                    "configfs 속성 쓰기 거부(%s): 빈 값은 커널 store 콜백을 "
                    "건너뛰어 성공처럼 보이지만 아무 것도 쓰지 않는다", attr);
        return FALSE;
    }

    gchar *full_path = g_build_filename(dir, attr, NULL);
    gchar *parent_dir = g_path_get_dirname(full_path);

                                                                     
                                                     
                                                           
    if (!pcv_lio_mkdir_p(parent_dir, error)) {
        g_free(parent_dir);
        g_free(full_path);
        return FALSE;
    }
    g_free(parent_dir);

                                                 
                                                             
                                                       
                                                   
                                                                
                                                                  
                                               
                                                                
                                                   
                                                  
                                                      
                                                 
                                                       
                                                     
    gint fd = open(full_path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    gint first_errno = errno;                               

    if (fd < 0 && first_errno == ENOENT) {
                                               
                                                    
                                  
        fd = open(full_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    }
    if (fd < 0) {
        gint saved_errno = errno;

                                                         
                                                      
                                                     
                                                 
        if (first_errno == ENOENT && saved_errno != ENOENT) {
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(first_errno),
                        "configfs 속성 파일 열기 실패(%s): 최초 open() 이 ENOENT"
                        "(파일 없음)였고, 폴백 O_CREAT 도 실패: %s",
                        full_path, g_strerror(saved_errno));
        } else {
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                        "configfs 속성 파일 열기 실패(%s): %s",
                        full_path, g_strerror(saved_errno));
        }
        g_free(full_path);
        return FALSE;
    }

                                                           
                                                  
                                               
                                                      
    ssize_t n = write(fd, value, len);
    gint saved_errno = errno;

    close(fd);

    if (n != (ssize_t)len) {
        if (n < 0) {
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                        "configfs 속성 쓰기 실패(%s): %s",
                        full_path, g_strerror(saved_errno));
        } else {
            g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                        "configfs 속성 부분 쓰기(%s): %zd/%zu 바이트만 전달됨",
                        full_path, n, len);
        }
        g_free(full_path);
        return FALSE;
    }

    g_free(full_path);
    return TRUE;
}

   
                                                           
  
                                                      
                                                          
                                                     
  
                                                        
                                            
                                                      
                                                
  
                                                
                                              
  
                                              
                            
                                
                                                           
                                                               
   
gchar *
pcv_lio_read_attr(const gchar *dir, const gchar *attr)
{
    gchar *full_path = g_build_filename(dir, attr, NULL);
    gchar *contents = NULL;
    gsize len = 0;

                                                            
                                                            
    gboolean ok = g_file_get_contents(full_path, &contents, &len, NULL);

    g_free(full_path);
    if (!ok)
        return NULL;

                                                          
                                         
    while (len > 0 && (contents[len - 1] == '\n' || contents[len - 1] == '\r'))
        contents[--len] = '\0';

    return contents;
}

   
                                                      
  
                                                           
                                               
                                                     
                                             
  
                                                
                                            
                                                
                                  
  
                                                 
                                               
                                             
                                               
                            
                                                          
                                           
   
gboolean
pcv_lio_rmdir(const gchar *path, GError **error)
{
    if (g_rmdir(path) != 0) {
        gint saved_errno = errno;

        if (saved_errno == ENOENT)
            return TRUE;                      

        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "configfs rmdir 실패(%s): %s", path, g_strerror(saved_errno));
        return FALSE;
    }

    return TRUE;
}

   
                                                       
  
                                                            
                                                        
                                                   
                                                            
                                                 
  
                                                 
                                                
                                                   
  
                                       
                                                          
                                      
   
gboolean
pcv_lio_available(const gchar *root)
{
                                                      
                                                            
                                                          
    return g_file_test(root, G_FILE_TEST_IS_DIR);
}

   
                                                                    
  
                                                               
                                                                  
                                               
                                                        
                                                             
                                                  
                              
  
                                                   
                                                   
                                                        
  
                                         
                                                     
                                     
                                                 
                    
                             
                                                          
                                            
   
gboolean
pcv_lio_ensure_fabric(const gchar *root, GError **error)
{
    gchar *iscsi_dir = g_build_filename(root, "iscsi", NULL);
    gboolean ok = pcv_lio_mkdir_p(iscsi_dir, error);                              

    g_free(iscsi_dir);
    return ok;
}

                                                                
  
                                                    
                                                 
                                                                           

                                                  
                                  
typedef struct {
    gchar *store;                                                       
    gchar *target;                                                    
    gchar *tpg;                                                          
    gchar *lun;                                                        
    gchar *lun_link;                                                           
    gchar *portal;                                                       
} PcvLioPaths;

                                                
                                           
                                                 
                                    
typedef struct {
    gboolean store;
    gboolean target;
    gboolean tpg;
    gboolean lun;
    gboolean lun_link;
    gboolean portal;
    gboolean tpg_enabled;
} PcvLioMade;

                                                            
                                                     
                                                  
static void
_lio_paths_clear(PcvLioPaths *paths)
{
    g_clear_pointer(&paths->store, g_free);
    g_clear_pointer(&paths->target, g_free);
    g_clear_pointer(&paths->tpg, g_free);
    g_clear_pointer(&paths->lun, g_free);
    g_clear_pointer(&paths->lun_link, g_free);
    g_clear_pointer(&paths->portal, g_free);
}

                                  
  
                                                                 
                                                        
                                                               
                                              
static gboolean
_lio_paths_build(const gchar *root, const gchar *vm_name,
                 PcvLioPaths *paths, GError **error)
{
    memset(paths, 0, sizeof(*paths));

    if (!root || !*root) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                    "LIO 경로 조립 거부: configfs 루트가 비어 있다");
        return FALSE;
    }

    gchar *iqn = pcv_lio_iqn_for_vm(vm_name);

    if (!iqn) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                    "LIO 경로 조립 거부: VM 이름이 비어 있거나 '/'·'..' 를 "
                    "포함한다(vm_name=%s)", vm_name ? vm_name : "(null)");
        return FALSE;
    }

    paths->store  = pcv_lio_path_backstore(root, vm_name);
    paths->target = pcv_lio_path_target(root, iqn);
    paths->tpg    = pcv_lio_path_tpg(root, iqn);
    g_free(iqn);

    if (!paths->store || !paths->target || !paths->tpg) {
                                                       
                                              
                      
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                    "LIO 경로 조립 거부: traversal 게이트가 경로를 거부했다"
                    "(vm_name=%s)", vm_name);
        _lio_paths_clear(paths);
        return FALSE;
    }

    paths->lun      = g_build_filename(paths->tpg, "lun", LIO_LUN_DIRNAME, NULL);
    paths->lun_link = g_build_filename(paths->lun, vm_name, NULL);
    paths->portal   = g_build_filename(paths->tpg, "np", LIO_PORTAL_NAME, NULL);
    return TRUE;
}

                                                              
                                         
static gboolean
_lio_attr_is(const gchar *dir, const gchar *attr, const gchar *expected)
{
    gchar *got = pcv_lio_read_attr(dir, attr);
    gboolean same = (g_strcmp0(got, expected) == 0);

    g_free(got);
    return same;
}

                                                               
  
                                                   
                                                          
  
                                                                 
                                                        
                                                                        
                                                          
                                                                           
                                      
  
                                                                               
                                
  
                                                                                  
                                                                                  
                     
                                                     
  
                  
                                                            
                                                            
                                                                  
                                               
  
                                                           
                                                      
                                                                
                                             
  
                                                           
   
static gchar *
_lio_kernel_bound_udev_path(const gchar *store)
{
    static const gchar KEY[] = "UDEV PATH: ";
    gchar *info = pcv_lio_read_attr(store, "info");

    if (!info)
        return NULL;                                             

    const gchar *at = strstr(info, KEY);

    if (!at) {
        g_free(info);
        return NULL;                                             
    }
    at += sizeof(KEY) - 1;

                                                  
                                            
    const gchar *end = at;

    while (*end && *end != '\n' && *end != '\r'
           && !(end[0] == ' ' && end[1] == ' '))
        end++;

    gchar *bound = (end > at) ? g_strndup(at, (gsize)(end - at)) : NULL;

    g_free(info);
    return bound;                                        
}

                                      
  
                                                   
                                                      
                                        
  
                                                  
                                                         
                                         
static gboolean
_lio_purge_children(const gchar *path, guint depth)
{
    if (depth == 0)
        return FALSE;

    GDir *dir = g_dir_open(path, 0, NULL);

    if (!dir)
        return FALSE;

    gboolean ok = TRUE;
    const gchar *name;

    while ((name = g_dir_read_name(dir)) != NULL) {
        gchar *child = g_build_filename(path, name, NULL);
        GStatBuf st;

        if (g_lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (!_lio_purge_children(child, depth - 1) || g_rmdir(child) != 0)
                ok = FALSE;
        } else if (g_unlink(child) != 0) {
            ok = FALSE;
        }
        g_free(child);
    }

    g_dir_close(dir);
    return ok;
}

                             
  
                                                     
                                                       
                                                    
                                                         
                                                       
                                                     
                  
  
                                                
                                                             
                                                           
                                    
                                               
                                                                    
                           
                                                            
                                                  
                                                     
                      
                                                     
                                                 
                                                   
                                  
  
                                                       
                
static gboolean
_lio_rmdir_node(const gchar *path, GError **error)
{
    if (g_rmdir(path) == 0)
        return TRUE;

    gint saved_errno = errno;

    if (saved_errno == ENOENT)
        return TRUE;                  

    if (saved_errno == ENOTEMPTY || saved_errno == EEXIST)
        (void)_lio_purge_children(path, LIO_PURGE_MAX_DEPTH);

    return pcv_lio_rmdir(path, error);
}

                                               
  
                                                
                                           
static void
_lio_rollback(const PcvLioPaths *paths, const PcvLioMade *made,
              const gchar *vm_name)
{
    GError *e = NULL;

    if (made->tpg_enabled && !pcv_lio_write_attr(paths->tpg, "enable", "0", &e)) {
        PCV_LOG_WARN(LIO_LOG_DOM, "롤백 경고(%s): TPG 비활성화 실패 — %s",
                     vm_name, e->message);
        g_clear_error(&e);
    }
    if (made->lun_link && g_unlink(paths->lun_link) != 0 && errno != ENOENT)
        PCV_LOG_WARN(LIO_LOG_DOM, "롤백 경고(%s): LUN 매핑 해제 실패(%s) — %s",
                     vm_name, paths->lun_link, g_strerror(errno));
    if (made->lun && !_lio_rmdir_node(paths->lun, &e)) {
        PCV_LOG_WARN(LIO_LOG_DOM, "롤백 경고(%s): %s", vm_name, e->message);
        g_clear_error(&e);
    }
    if (made->portal && !_lio_rmdir_node(paths->portal, &e)) {
        PCV_LOG_WARN(LIO_LOG_DOM, "롤백 경고(%s): %s", vm_name, e->message);
        g_clear_error(&e);
    }
    if (made->tpg && !_lio_rmdir_node(paths->tpg, &e)) {
        PCV_LOG_WARN(LIO_LOG_DOM, "롤백 경고(%s): %s", vm_name, e->message);
        g_clear_error(&e);
    }
    if (made->target && !_lio_rmdir_node(paths->target, &e)) {
        PCV_LOG_WARN(LIO_LOG_DOM, "롤백 경고(%s): %s", vm_name, e->message);
        g_clear_error(&e);
    }
    if (made->store && !_lio_rmdir_node(paths->store, &e)) {
        PCV_LOG_WARN(LIO_LOG_DOM, "롤백 경고(%s): %s", vm_name, e->message);
        g_clear_error(&e);
    }
}

   
                                                                
  
                                                  
                                                 
  
                                                        
                                              
                                                               
                                               
  
                                                   
                                                       
                                               
                                               
                                                 
                                    
  
       
                                                                  
                                                   
                                                                 
                                                      
                                                     
                                                
                                   
                                                      
                                                  
                                                           
                                                           
                                                    
                                            
                                                 
                                                                   
                                                          
                                                
                                                  
                                              
                                                                      
                                                         
                                                     
                                                     
                                                            
                                           
   
gboolean
pcv_lio_target_create_at(const gchar *root, const gchar *vm_name,
                         const gchar *zvol_path,
                         const gchar *chap_user, const gchar *chap_password,
                         GError **error)
{
    PcvLioPaths paths;

    if (!_lio_paths_build(root, vm_name, &paths, error))
        return FALSE;

    PcvLioMade made = { 0 };
    gchar *control_value = NULL;
    gboolean ok = FALSE;

    if (!zvol_path || !*zvol_path) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                    "LIO 타겟 생성 거부(%s): 백킹 디바이스 경로가 비어 있다",
                    vm_name);
        goto out;
    }

                                                                    
                                                            
                                                 
    made.store = !g_file_test(paths.store, G_FILE_TEST_EXISTS);
    if (!pcv_lio_mkdir_p(paths.store, error))
        goto out;

                                                                    
                                                        
                                                                         
                                                
                                                              
                                                                      
                                                 
                                                                 
                                               
                                                             
                                                   
                    
      
                                                            
                                                           
                                                            
                             
      
                                                             
                                                       
                                                              
                                       
                                                            
                                                            
                                                    
                                                    
                                                       
                   
      
                                                                 
                                                       
                                               
                                             
                              
                                                         
      
                                                             
                                                            
                                                         
                                                 
                                              
                                                         
                                                            
                                                          
                                                            
                                                     
                                                    
    if (_lio_attr_is(paths.store, "enable", "1")) {
        gboolean from_kernel = TRUE;
        gchar *bound = _lio_kernel_bound_udev_path(paths.store);

        if (!bound) {                                   
            from_kernel = FALSE;
            bound = pcv_lio_read_attr(paths.store, "udev_path");
        }

        if (bound && *bound && g_strcmp0(bound, zvol_path) != 0) {
            g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_EXIST,
                        "LIO 타겟 생성 거부(%s): 백스토어가 이미 활성 상태로 다른 "
                        "디바이스에 묶여 있다(현재=%s, 요청=%s, 출처=%s). 커널은 활성 "
                        "IBLOCK 을 teardown 없이 다시 묶지 못하므로 먼저 타깃을 삭제하라",
                        vm_name, bound, zvol_path,
                        from_kernel ? "커널 실바인딩(info)" : "udev_path 속성(대리 지표)");
            g_free(bound);
            goto out;
        }
        g_free(bound);
    }

    if (!pcv_lio_write_attr(paths.store, "udev_path", zvol_path, error))
        goto out;

    if (!_lio_attr_is(paths.store, "enable", "1")) {
                                                           
                                                                     
                                                                 
                                           
                                                                           
                                   
          
                                                       
                                                
                                                                      
        control_value = g_strdup_printf("udev_path=%s", zvol_path);
        if (!pcv_lio_write_attr(paths.store, "control", control_value, error))
            goto out;
        if (!pcv_lio_write_attr(paths.store, "enable", "1", error))
            goto out;
    }

                                                                        
    made.target = !g_file_test(paths.target, G_FILE_TEST_EXISTS);
    if (!pcv_lio_mkdir_p(paths.target, error))
        goto out;
    made.tpg = !g_file_test(paths.tpg, G_FILE_TEST_EXISTS);
    if (!pcv_lio_mkdir_p(paths.tpg, error))
        goto out;

                                                                   
                                                
                                                    
                              
    made.lun = !g_file_test(paths.lun, G_FILE_TEST_EXISTS);
    if (!pcv_lio_mkdir_p(paths.lun, error))
        goto out;

    if (!g_file_test(paths.lun_link, G_FILE_TEST_IS_SYMLINK)) {
        if (symlink(paths.store, paths.lun_link) != 0) {
            gint saved_errno = errno;

            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                        "LUN 매핑 심볼릭 링크 생성 실패(%s → %s): %s",
                        paths.lun_link, paths.store, g_strerror(saved_errno));
            goto out;
        }
        made.lun_link = TRUE;
    }

                                                                
    made.portal = !g_file_test(paths.portal, G_FILE_TEST_EXISTS);
    if (!pcv_lio_mkdir_p(paths.portal, error))
        goto out;

                                                                     
                                                                   
                                                   
                                             
      
                                                                
                                                          
                                                                         
                                                                  
                                                               
                                                               
                                                      
                                                     
    if (!pcv_lio_write_attr(paths.tpg, "attrib/generate_node_acls", "1", error))
        goto out;
    if (!pcv_lio_write_attr(paths.tpg, "attrib/demo_mode_write_protect", "0", error))
        goto out;

                                                                
                                                
                                             
                                               
                                                             
                                     
                                                                    
                                         
      
                                                 
                                                               
                                                             
                                                  
                                                            
                                                                  
                                              
                                                                   
                                                         
                              
      
                                              
                                                                      
                                                                         
                                                              
                                                              
                                           
                                                
                                                       
                                               
                                                        
                                                
                                                          
      
                                       
                                                               
                                                  
                                                
                                                   
                                                
                                               
    if (chap_user && *chap_user && chap_password && *chap_password) {
        if (!pcv_lio_write_attr(paths.tpg, "auth/userid", chap_user, error))
            goto out;
        if (!pcv_lio_write_attr(paths.tpg, "auth/password", chap_password, error))
            goto out;
        if (!pcv_lio_write_attr(paths.tpg, "attrib/authentication", "1", error))
            goto out;
    } else if (!pcv_lio_write_attr(paths.tpg, "attrib/authentication", "0", error)) {
        goto out;
    }

                                                                   
                                                        
    if (!_lio_attr_is(paths.tpg, "enable", "1")) {
        if (!pcv_lio_write_attr(paths.tpg, "enable", "1", error))
            goto out;
        made.tpg_enabled = TRUE;
    }

    ok = TRUE;

out:
    g_free(control_value);
    if (!ok)
        _lio_rollback(&paths, &made, vm_name ? vm_name : "(null)");
    _lio_paths_clear(&paths);
    return ok;
}

   
                                                        
  
                                                     
                                                 
                                              
                                                
                                     
  
                                                           
                                                         
                                             
                                       
  
                                                                 
                                                    
  
                                                  
                                                              
                                         
                                                      
                                              
                                                        
                                              
                                                       
                               
                                                         
                                                 
                                                         
   
gboolean
pcv_lio_target_delete_at(const gchar *root, const gchar *vm_name, GError **error)
{
    PcvLioPaths paths;

    if (!_lio_paths_build(root, vm_name, &paths, error))
        return FALSE;

    gboolean ok = TRUE;

                                                                    
                                                          
                                                    
                                              
                                             
    if (g_file_test(paths.tpg, G_FILE_TEST_IS_DIR)
        && !_lio_attr_is(paths.tpg, "enable", "0")
        && !pcv_lio_write_attr(paths.tpg, "enable", "0", error))
        ok = FALSE;

                                                               
                                                         
    if (ok && g_unlink(paths.lun_link) != 0 && errno != ENOENT) {
        gint saved_errno = errno;

        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "LUN 매핑 해제 실패(%s): %s",
                    paths.lun_link, g_strerror(saved_errno));
        ok = FALSE;
    }

                                                                      
                                                      
                                            
                                                               
                            
    if (ok && !_lio_rmdir_node(paths.lun, error))
        ok = FALSE;
    if (ok && !_lio_rmdir_node(paths.portal, error))
        ok = FALSE;
    if (ok && !_lio_rmdir_node(paths.tpg, error))
        ok = FALSE;
    if (ok && !_lio_rmdir_node(paths.target, error))
        ok = FALSE;
    if (ok && !_lio_rmdir_node(paths.store, error))
        ok = FALSE;

    _lio_paths_clear(&paths);
    return ok;
}
