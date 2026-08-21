   
                            
                                                                                         
  
                           
                                                   
                                                    
                                        
  
                                               
                                                  
                                                  
  
                            
                                                   
                                                           
  
           
                                                                 
                                             
                                                       
                                             
                                                         
                                                             
                                                               
                                                   
   

  
                                                                  
  
                                                          
                                                   
                                                
                           
  
           
  
                                                  
                                                                 
                                    
                                                         
                                                    
                              
  
                
  
                                                              
                                                       
                                                      
                                                      
                                                             
  
               
  
                                                              
                                                              
                                                    
                  
                                                   
                                                                   
                                                    
                                                        
                                                  
  
                   
  
                                                                 
                                                               
                                 
                                                         
                                              
                                                          
                          
  
              
  
                                                                  
                                             
                                                       
                                                       
             
  
                           
  
                                               
                                                                
                                    
                                                        
                                            
                                               
                                               
  
           
  
                                                    
                                               
                                        
                                                                
                                                              
                 
  
                   
  
                                                    
                                     
                                                   
                                                       
                                                      
  
                                                                    
                                                                
                                             
                                                                             
   
#include "modules/storage/pcv_iscsi_node_db.h"
#include "utils/pcv_secure.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <stdint.h>
#include <stdio.h>                      
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PCV_ISCSI_DB_LOCK_WAIT_US 10000U
#define PCV_ISCSI_DB_DEFAULT_PORT 3260U
#define PCV_ISCSI_DB_MAX_RECORDS 256U
#define PCV_ISCSI_DB_MAX_RECORD_BYTES (1024U * 1024U)
#define PCV_ISCSI_DB_TEMP_PREFIX ".pcv-chap-"
#define PCV_ISCSI_DB_CREDENTIAL_MAX 255U

static const gchar *const AUTH_KEYS[] = {
    "node.session.auth.authmethod",
    "node.session.auth.username",
    "node.session.auth.password",
};

typedef struct {
    gchar *address;
    guint port;
} PcvIscsiPortal;

typedef struct {
    gint dir_fd;
    gint lock_fd;
    struct stat lock_stat;
    gboolean held;
} PcvIscsiDbLock;

typedef struct {
    gint parent_fd;
    gchar *name;
    gchar *display_path;
    gchar *original;
    gsize original_len;
    gchar *updated;
    gsize updated_len;
    gchar *temp_name;
    struct stat original_stat;
    gboolean committed;
} PcvIscsiRecordUpdate;

static void
_set_errno_error(GError **error, const gchar *action, const gchar *path,
                 gint saved_errno)
{
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved_errno),
                "iSCSI node DB %s 실패(%s): %s",
                action, path ? path : "unknown", g_strerror(saved_errno));
}

static gboolean
_is_single_component(const gchar *value)
{
    return value && *value && strcmp(value, ".") != 0 &&
           strcmp(value, "..") != 0 && strchr(value, '/') == NULL;
}

static gboolean
_parse_uint16(const gchar *text, guint *value_out)
{
    gchar *end = NULL;
    guint64 value;

    if (!text || !*text)
        return FALSE;
    errno = 0;
    value = g_ascii_strtoull(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || value == 0 || value > 65535)
        return FALSE;
    *value_out = (guint)value;
    return TRUE;
}

static gboolean
_parse_requested_portal(const gchar *text, PcvIscsiPortal *portal,
                        GError **error)
{
    const gchar *address_start = text;
    gsize address_len = 0;
    guint port = PCV_ISCSI_DB_DEFAULT_PORT;

    if (!text || !*text || strchr(text, ',')) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "iSCSI portal 주소 형식이 올바르지 않음");
        return FALSE;
    }

    if (text[0] == '[') {
        const gchar *close = strchr(text + 1, ']');
        if (!close || close == text + 1) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                "iSCSI IPv6 portal 괄호 형식이 올바르지 않음");
            return FALSE;
        }
        address_start = text + 1;
        address_len = (gsize)(close - address_start);
        if (close[1] != '\0') {
            if (close[1] != ':' || !_parse_uint16(close + 2, &port)) {
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                    "iSCSI portal 포트가 올바르지 않음");
                return FALSE;
            }
        }
    } else {
        guint colon_count = 0;
        const gchar *last_colon = NULL;
        for (const gchar *p = text; *p; p++) {
            if (*p == ':') {
                colon_count++;
                last_colon = p;
            }
        }
        if (colon_count == 1) {
            guint parsed_port = 0;
            if (!last_colon || last_colon == text ||
                !_parse_uint16(last_colon + 1, &parsed_port)) {
                g_set_error_literal(error, G_IO_ERROR,
                                    G_IO_ERROR_INVALID_ARGUMENT,
                                    "iSCSI portal 포트가 올바르지 않음");
                return FALSE;
            }
            address_len = (gsize)(last_colon - text);
            port = parsed_port;
        }
        if (address_len == 0)
            address_len = strlen(text);
    }

    portal->address = g_strndup(address_start, address_len);
    portal->port = port;
    if (!portal->address || !*portal->address) {
        g_clear_pointer(&portal->address, g_free);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "iSCSI portal 주소가 비어 있음");
        return FALSE;
    }
    return TRUE;
}

static gboolean
_addresses_equal(const gchar *left, const gchar *right)
{
    if (g_strcmp0(left, right) == 0)
        return TRUE;

    GInetAddress *left_ip = g_inet_address_new_from_string(left);
    GInetAddress *right_ip = g_inet_address_new_from_string(right);
    gboolean equal = left_ip && right_ip && g_inet_address_equal(left_ip, right_ip);

    g_clear_object(&right_ip);
    g_clear_object(&left_ip);
    if (equal)
        return TRUE;
    return g_ascii_strcasecmp(left, right) == 0;
}

static gboolean
_credential_is_valid(const gchar *value)
{
    return value && *value && strlen(value) <= PCV_ISCSI_DB_CREDENTIAL_MAX &&
           !g_str_has_prefix(value, "NULL") && !strchr(value, '\n') &&
           !strchr(value, '\r');
}

static void
_lock_clear(PcvIscsiDbLock *lock)
{
    if (lock->lock_fd >= 0)
        close(lock->lock_fd);
    if (lock->dir_fd >= 0)
        close(lock->dir_fd);
    lock->lock_fd = -1;
    lock->dir_fd = -1;
    lock->held = FALSE;
}

                                                                         
static gboolean
_lock_acquire(const gchar *lock_root, guint timeout_ms,
              PcvIscsiDbLock *lock, GError **error)
{
    lock->dir_fd = -1;
    lock->lock_fd = -1;

    if (g_mkdir_with_parents(lock_root, 0770) != 0) {
        _set_errno_error(error, "lock 디렉터리 생성", lock_root, errno);
        return FALSE;
    }
    lock->dir_fd = open(lock_root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (lock->dir_fd < 0) {
        _set_errno_error(error, "lock 디렉터리 열기", lock_root, errno);
        return FALSE;
    }

    lock->lock_fd = openat(lock->dir_fd, "lock",
                           O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0666);
    if (lock->lock_fd < 0) {
        _set_errno_error(error, "lock anchor 열기", lock_root, errno);
        _lock_clear(lock);
        return FALSE;
    }
    if (fstat(lock->lock_fd, &lock->lock_stat) != 0 ||
        !S_ISREG(lock->lock_stat.st_mode)) {
        gint saved_errno = errno ? errno : EINVAL;
        _set_errno_error(error, "lock anchor 검증", lock_root, saved_errno);
        _lock_clear(lock);
        return FALSE;
    }

    gint64 deadline = g_get_monotonic_time() + ((gint64)timeout_ms * 1000);
    for (;;) {
        if (linkat(lock->dir_fd, "lock", lock->dir_fd, "lock.write", 0) == 0)
            break;
        gint saved_errno = errno;
        if (saved_errno != EEXIST) {
            _set_errno_error(error, "write lock 획득", lock_root, saved_errno);
            _lock_clear(lock);
            return FALSE;
        }
        gint64 remaining = deadline - g_get_monotonic_time();
        if (remaining <= 0) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                "iSCSI node DB write lock 대기시간 초과");
            _lock_clear(lock);
            return FALSE;
        }
        g_usleep((gulong)MIN((gint64)PCV_ISCSI_DB_LOCK_WAIT_US, remaining));
    }

    struct stat write_stat;
    if (fstatat(lock->dir_fd, "lock.write", &write_stat,
                AT_SYMLINK_NOFOLLOW) != 0 ||
        write_stat.st_dev != lock->lock_stat.st_dev ||
        write_stat.st_ino != lock->lock_stat.st_ino) {
        gint saved_errno = errno ? errno : EINVAL;
        unlinkat(lock->dir_fd, "lock.write", 0);
        _set_errno_error(error, "write lock identity 검증", lock_root, saved_errno);
        _lock_clear(lock);
        return FALSE;
    }
    lock->held = TRUE;
    return TRUE;
}

static gboolean
_lock_release(PcvIscsiDbLock *lock, GError **error)
{
    gboolean ok = TRUE;

    if (lock->held) {
        struct stat write_stat;
        if (fstatat(lock->dir_fd, "lock.write", &write_stat,
                    AT_SYMLINK_NOFOLLOW) != 0 ||
            write_stat.st_dev != lock->lock_stat.st_dev ||
            write_stat.st_ino != lock->lock_stat.st_ino) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "iSCSI node DB write lock identity가 변경됨");
            ok = FALSE;
        } else if (unlinkat(lock->dir_fd, "lock.write", 0) != 0) {
            _set_errno_error(error, "write lock 해제", "lock.write", errno);
            ok = FALSE;
        }
    }
    _lock_clear(lock);
    return ok;
}

static void
_record_update_free(gpointer data)
{
    PcvIscsiRecordUpdate *record = data;

    if (!record)
        return;
    if (record->temp_name && record->parent_fd >= 0)
        unlinkat(record->parent_fd, record->temp_name, 0);
    if (record->original) {
        pcv_secure_wipe(record->original, record->original_len);
        g_free(record->original);
    }
    if (record->updated) {
        pcv_secure_wipe(record->updated, record->updated_len);
        g_free(record->updated);
    }
    if (record->parent_fd >= 0)
        close(record->parent_fd);
    g_free(record->temp_name);
    g_free(record->display_path);
    g_free(record->name);
    g_free(record);
}

static gboolean
_add_record(GPtrArray *records, gint parent_fd, const gchar *name,
            const gchar *display_path, GError **error)
{
    if (records->len >= PCV_ISCSI_DB_MAX_RECORDS) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_TOO_MANY_OPEN_FILES,
                            "iSCSI node DB 일치 record 수 상한 초과");
        return FALSE;
    }
    PcvIscsiRecordUpdate *record = g_new0(PcvIscsiRecordUpdate, 1);
    record->parent_fd = fcntl(parent_fd, F_DUPFD_CLOEXEC, 3);
    if (record->parent_fd < 0) {
        _set_errno_error(error, "record parent 복제", display_path, errno);
        g_free(record);
        return FALSE;
    }
    record->name = g_strdup(name);
    record->display_path = g_strdup(display_path);
    g_ptr_array_add(records, record);
    return TRUE;
}

static gboolean
_parse_record_portal_name(const gchar *name, gchar **address_out,
                          guint *port_out, gboolean *new_layout_out)
{
    gchar **parts = g_strsplit(name, ",", -1);
    guint count = g_strv_length(parts);
    guint port = 0;
    gboolean ok = (count == 2 || count == 3) && parts[0][0] != '\0' &&
                  _parse_uint16(parts[1], &port);

    if (ok && count == 3) {
        gchar *end = NULL;
        guint64 tpgt = 0;

                                                       
                                                            
        if (strcmp(parts[2], "-1") != 0) {
            errno = 0;
            tpgt = g_ascii_strtoull(parts[2], &end, 10);
            ok = errno == 0 && end && *parts[2] && *end == '\0' &&
                 parts[2][0] != '+' && tpgt <= G_MAXUINT16;
        }
    }
    if (ok) {
        *address_out = g_strdup(parts[0]);
        *port_out = port;
        *new_layout_out = count == 3;
    }
    g_strfreev(parts);
    return ok;
}

static gboolean
_remove_stale_temp(gint dir_fd, const gchar *name, const gchar *display,
                   GError **error)
{
    if (!g_str_has_prefix(name, PCV_ISCSI_DB_TEMP_PREFIX))
        return TRUE;
    if (unlinkat(dir_fd, name, 0) != 0 && errno != ENOENT) {
        _set_errno_error(error, "stale temp 정리", display, errno);
        return FALSE;
    }
    return TRUE;
}

static gboolean
_collect_iface_records(GPtrArray *records, gint portal_fd,
                       const gchar *target_iqn, const gchar *portal_name,
                       GError **error)
{
    gint scan_fd = fcntl(portal_fd, F_DUPFD_CLOEXEC, 3);
    DIR *dir = scan_fd >= 0 ? fdopendir(scan_fd) : NULL;
    if (!dir) {
        if (scan_fd >= 0)
            close(scan_fd);
        _set_errno_error(error, "iface 디렉터리 열기", portal_name, errno);
        return FALSE;
    }

    gboolean ok = TRUE;
    struct dirent *entry;
    while (ok && (entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        gchar *display = g_build_filename(target_iqn, portal_name,
                                          entry->d_name, NULL);
        if (g_str_has_prefix(entry->d_name, PCV_ISCSI_DB_TEMP_PREFIX)) {
            ok = _remove_stale_temp(portal_fd, entry->d_name, display, error);
            g_free(display);
            continue;
        }

        struct stat st;
        if (fstatat(portal_fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            _set_errno_error(error, "iface record 검사", display, errno);
            ok = FALSE;
        } else if (!S_ISREG(st.st_mode)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "iSCSI iface record가 regular file이 아님(%s)", display);
            ok = FALSE;
        } else {
            ok = _add_record(records, portal_fd, entry->d_name, display, error);
        }
        g_free(display);
    }
    closedir(dir);
    return ok;
}

static gboolean
_collect_records(const gchar *node_root, const gchar *target_iqn,
                 const PcvIscsiPortal *request, GPtrArray *records,
                 GError **error)
{
    gint root_fd = open(node_root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (root_fd < 0) {
        _set_errno_error(error, "node root 열기", node_root, errno);
        return FALSE;
    }

    gboolean found_target = FALSE;
    gint scan_fd = fcntl(root_fd, F_DUPFD_CLOEXEC, 3);
    DIR *root_dir = scan_fd >= 0 ? fdopendir(scan_fd) : NULL;
    if (!root_dir) {
        if (scan_fd >= 0)
            close(scan_fd);
        _set_errno_error(error, "node root 열거", node_root, errno);
        close(root_fd);
        return FALSE;
    }
    struct dirent *entry;
    while ((entry = readdir(root_dir)) != NULL) {
        if (strcmp(entry->d_name, target_iqn) == 0) {
            found_target = TRUE;
            break;
        }
    }
    closedir(root_dir);
    if (!found_target) {
        close(root_fd);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "iSCSI node target record를 찾지 못함(%s)", target_iqn);
        return FALSE;
    }

    gint target_fd = openat(root_fd, target_iqn,
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    close(root_fd);
    if (target_fd < 0) {
        _set_errno_error(error, "target 디렉터리 열기", target_iqn, errno);
        return FALSE;
    }

    scan_fd = fcntl(target_fd, F_DUPFD_CLOEXEC, 3);
    DIR *target_dir = scan_fd >= 0 ? fdopendir(scan_fd) : NULL;
    if (!target_dir) {
        if (scan_fd >= 0)
            close(scan_fd);
        _set_errno_error(error, "target 디렉터리 열거", target_iqn, errno);
        close(target_fd);
        return FALSE;
    }

    gboolean ok = TRUE;
    while (ok && (entry = readdir(target_dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        gchar *display = g_build_filename(target_iqn, entry->d_name, NULL);
        if (g_str_has_prefix(entry->d_name, PCV_ISCSI_DB_TEMP_PREFIX)) {
            ok = _remove_stale_temp(target_fd, entry->d_name, display, error);
            g_free(display);
            continue;
        }

        gchar *address = NULL;
        guint port = 0;
        gboolean new_layout = FALSE;
        if (!_parse_record_portal_name(entry->d_name, &address, &port,
                                       &new_layout) ||
            port != request->port || !_addresses_equal(address, request->address)) {
            g_free(address);
            g_free(display);
            continue;
        }
        g_free(address);

        struct stat st;
        if (fstatat(target_fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            _set_errno_error(error, "portal record 검사", display, errno);
            ok = FALSE;
        } else if (new_layout && S_ISDIR(st.st_mode)) {
            gint portal_fd = openat(target_fd, entry->d_name,
                                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (portal_fd < 0) {
                _set_errno_error(error, "portal 디렉터리 열기", display, errno);
                ok = FALSE;
            } else {
                ok = _collect_iface_records(records, portal_fd, target_iqn,
                                            entry->d_name, error);
                close(portal_fd);
            }
        } else if (!new_layout && S_ISREG(st.st_mode)) {
            ok = _add_record(records, target_fd, entry->d_name, display, error);
        } else {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "iSCSI portal record layout이 올바르지 않음(%s)", display);
            ok = FALSE;
        }
        g_free(display);
    }
    closedir(target_dir);
    close(target_fd);

    if (ok && records->len == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "iSCSI target/portal에 일치하는 node record가 없음(%s)",
                    target_iqn);
        ok = FALSE;
    }
    return ok;
}

static gint
_record_compare(gconstpointer left, gconstpointer right)
{
    const PcvIscsiRecordUpdate *a = *(PcvIscsiRecordUpdate * const *)left;
    const PcvIscsiRecordUpdate *b = *(PcvIscsiRecordUpdate * const *)right;
    return g_strcmp0(a->display_path, b->display_path);
}

static gboolean
_read_record(PcvIscsiRecordUpdate *record, GError **error)
{
    gint fd = openat(record->parent_fd, record->name,
                     O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        _set_errno_error(error, "record 열기", record->display_path, errno);
        return FALSE;
    }
    if (fstat(fd, &record->original_stat) != 0 ||
        !S_ISREG(record->original_stat.st_mode) ||
        record->original_stat.st_nlink != 1 ||
        record->original_stat.st_size < 0 ||
        (guint64)record->original_stat.st_size > PCV_ISCSI_DB_MAX_RECORD_BYTES) {
        gint saved_errno = errno ? errno : EINVAL;
        _set_errno_error(error, "record metadata 검증", record->display_path,
                         saved_errno);
        close(fd);
        return FALSE;
    }

    record->original_len = (gsize)record->original_stat.st_size;
    record->original = g_malloc0(record->original_len + 1);
    gsize offset = 0;
    while (offset < record->original_len) {
        ssize_t n = read(fd, record->original + offset,
                         record->original_len - offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            gint saved_errno = n < 0 ? errno : EIO;
            _set_errno_error(error, "record 읽기", record->display_path,
                             saved_errno);
            close(fd);
            return FALSE;
        }
        offset += (gsize)n;
    }
    gchar extra;
    ssize_t extra_len;
    do {
        extra_len = read(fd, &extra, 1);
    } while (extra_len < 0 && errno == EINTR);
    close(fd);
    if (extra_len != 0 || memchr(record->original, '\0', record->original_len)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "iSCSI node record가 읽는 동안 변했거나 NUL을 포함함(%s)",
                    record->display_path);
        return FALSE;
    }
    return TRUE;
}

static gboolean
_line_key_value(const gchar *line, gsize line_len, const gchar *key,
                const gchar **value_out, gsize *value_len_out)
{
    const gchar *p = line;
    const gchar *end = line + line_len;
    gsize key_len = strlen(key);

    while (p < end && (*p == ' ' || *p == '\t'))
        p++;
    if ((gsize)(end - p) < key_len || memcmp(p, key, key_len) != 0)
        return FALSE;
    p += key_len;
    while (p < end && (*p == ' ' || *p == '\t'))
        p++;
    if (p >= end || *p != '=')
        return FALSE;
    p++;
    while (p < end && (*p == ' ' || *p == '\t'))
        p++;
    while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
        end--;
    *value_out = p;
    *value_len_out = (gsize)(end - p);
    return TRUE;
}

static gboolean
_validate_identity(PcvIscsiRecordUpdate *record, const gchar *target_iqn,
                   const PcvIscsiPortal *request, GError **error)
{
    guint target_count = 0;
    guint address_count = 0;
    guint port_count = 0;
    gboolean target_ok = FALSE;
    gboolean address_ok = FALSE;
    gboolean port_ok = FALSE;

    for (gsize pos = 0; pos < record->original_len;) {
        gsize start = pos;
        while (pos < record->original_len && record->original[pos] != '\n')
            pos++;
        gsize len = pos - start;
        if (pos < record->original_len)
            pos++;
        const gchar *value = NULL;
        gsize value_len = 0;
        if (_line_key_value(record->original + start, len, "node.name",
                            &value, &value_len)) {
            target_count++;
            target_ok = value_len == strlen(target_iqn) &&
                        memcmp(value, target_iqn, value_len) == 0;
        } else if (_line_key_value(record->original + start, len,
                                   "node.conn[0].address", &value, &value_len)) {
            address_count++;
            gchar *address = g_strndup(value, value_len);
            address_ok = _addresses_equal(address, request->address);
            g_free(address);
        } else if (_line_key_value(record->original + start, len,
                                   "node.conn[0].port", &value, &value_len)) {
            port_count++;
            gchar *port_text = g_strndup(value, value_len);
            guint port = 0;
            port_ok = _parse_uint16(port_text, &port) && port == request->port;
            g_free(port_text);
        }
    }
    if (target_count != 1 || address_count != 1 || port_count != 1 ||
        !target_ok || !address_ok || !port_ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "iSCSI node record identity가 경로와 일치하지 않음(%s)",
                    record->display_path);
        return FALSE;
    }
    return TRUE;
}

static void
_buffer_append(gchar *buffer, gsize *used, const gchar *data, gsize len)
{
    memcpy(buffer + *used, data, len);
    *used += len;
}

                                                                   
static gboolean
_build_updated_record(PcvIscsiRecordUpdate *record, const gchar *username,
                      const gchar *password, GError **error)
{
    const gchar *values[] = {"CHAP", username, password};
    guint seen[G_N_ELEMENTS(AUTH_KEYS)] = {0};
    gsize extra = 8;

    for (guint i = 0; i < G_N_ELEMENTS(AUTH_KEYS); i++) {
        gsize line_len = strlen(AUTH_KEYS[i]) + 3 + strlen(values[i]) + 1;
        if (G_MAXSIZE - extra < line_len ||
            G_MAXSIZE - record->original_len < extra + line_len) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                                "iSCSI node record 크기 계산 overflow");
            return FALSE;
        }
        extra += line_len;
    }
    gsize capacity = record->original_len + extra;
    gchar *updated = g_malloc0(capacity + 1);
    gsize used = 0;

    for (gsize pos = 0; pos < record->original_len;) {
        gsize start = pos;
        while (pos < record->original_len && record->original[pos] != '\n')
            pos++;
        gsize content_len = pos - start;
        if (pos < record->original_len)
            pos++;
        gsize raw_len = pos - start;
        gint matched = -1;
        for (guint i = 0; i < G_N_ELEMENTS(AUTH_KEYS); i++) {
            const gchar *unused_value = NULL;
            gsize unused_len = 0;
            if (_line_key_value(record->original + start, content_len,
                                AUTH_KEYS[i], &unused_value, &unused_len)) {
                matched = (gint)i;
                seen[i]++;
                break;
            }
        }
        if (matched < 0) {
            _buffer_append(updated, &used, record->original + start, raw_len);
            continue;
        }
        if (seen[matched] > 1) {
            pcv_secure_wipe(updated, used);
            g_free(updated);
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "iSCSI node record에 중복 CHAP key가 있음(%s)",
                        record->display_path);
            return FALSE;
        }
        _buffer_append(updated, &used, AUTH_KEYS[matched],
                       strlen(AUTH_KEYS[matched]));
        _buffer_append(updated, &used, " = ", 3);
        _buffer_append(updated, &used, values[matched], strlen(values[matched]));
        _buffer_append(updated, &used, "\n", 1);
    }

    for (guint i = 0; i < G_N_ELEMENTS(AUTH_KEYS); i++) {
        if (seen[i])
            continue;
        if (used > 0 && updated[used - 1] != '\n')
            _buffer_append(updated, &used, "\n", 1);
        _buffer_append(updated, &used, AUTH_KEYS[i], strlen(AUTH_KEYS[i]));
        _buffer_append(updated, &used, " = ", 3);
        _buffer_append(updated, &used, values[i], strlen(values[i]));
        _buffer_append(updated, &used, "\n", 1);
    }
    updated[used] = '\0';
    record->updated = updated;
    record->updated_len = used;
    return TRUE;
}

static gboolean
_write_all(gint fd, const gchar *data, gsize len)
{
    gsize offset = 0;
    while (offset < len) {
        ssize_t n = write(fd, data + offset, len - offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return FALSE;
        offset += (gsize)n;
    }
    return TRUE;
}

static gboolean
_stage_bytes(PcvIscsiRecordUpdate *record, const gchar *bytes, gsize len,
             GError **error)
{
    g_clear_pointer(&record->temp_name, g_free);
    gint fd = -1;
    for (guint attempt = 0; attempt < 32; attempt++) {
        gchar candidate[96];
        g_snprintf(candidate, sizeof(candidate), "%s%ld-%08x-%u",
                   PCV_ISCSI_DB_TEMP_PREFIX, (long)getpid(), g_random_int(), attempt);
        fd = openat(record->parent_fd, candidate,
                    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0) {
            record->temp_name = g_strdup(candidate);
            break;
        }
        if (errno != EEXIST)
            break;
    }
    if (fd < 0) {
        _set_errno_error(error, "private temp 생성", record->display_path, errno);
        return FALSE;
    }

    gboolean ok = TRUE;
    if (fchmod(fd, 0600) != 0) {
        _set_errno_error(error, "private temp 권한 설정", record->display_path,
                         errno);
        ok = FALSE;
    } else if (geteuid() == 0 &&
               fchown(fd, record->original_stat.st_uid,
                      record->original_stat.st_gid) != 0) {
        _set_errno_error(error, "private temp 소유권 설정", record->display_path,
                         errno);
        ok = FALSE;
    } else if (!_write_all(fd, bytes, len)) {
        _set_errno_error(error, "private temp 쓰기", record->display_path,
                         errno ? errno : EIO);
        ok = FALSE;
    } else if (fsync(fd) != 0) {
        _set_errno_error(error, "private temp fsync", record->display_path, errno);
        ok = FALSE;
    }
    if (close(fd) != 0 && ok) {
        _set_errno_error(error, "private temp close", record->display_path, errno);
        ok = FALSE;
    }
    if (!ok && record->temp_name) {
        unlinkat(record->parent_fd, record->temp_name, 0);
        g_clear_pointer(&record->temp_name, g_free);
    }
    return ok;
}

static gboolean
_stage_record(PcvIscsiRecordUpdate *record, const gchar *target_iqn,
              const PcvIscsiPortal *request, const gchar *username,
              const gchar *password, GError **error)
{
    return _read_record(record, error) &&
           _validate_identity(record, target_iqn, request, error) &&
           _build_updated_record(record, username, password, error) &&
           _stage_bytes(record, record->updated, record->updated_len, error);
}

static gboolean
_original_identity_unchanged(PcvIscsiRecordUpdate *record, GError **error)
{
    struct stat current;
    if (fstatat(record->parent_fd, record->name, &current,
                AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(current.st_mode) ||
        current.st_dev != record->original_stat.st_dev ||
        current.st_ino != record->original_stat.st_ino) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "iSCSI node record identity가 commit 전에 변경됨(%s)",
                    record->display_path);
        return FALSE;
    }
    return TRUE;
}

static gboolean
_commit_record(PcvIscsiRecordUpdate *record, GError **error)
{
    if (!_original_identity_unchanged(record, error))
        return FALSE;
    if (renameat(record->parent_fd, record->temp_name,
                 record->parent_fd, record->name) != 0) {
        _set_errno_error(error, "record rename", record->display_path, errno);
        return FALSE;
    }
    g_clear_pointer(&record->temp_name, g_free);
    record->committed = TRUE;
    if (fsync(record->parent_fd) != 0) {
        _set_errno_error(error, "record parent fsync", record->display_path, errno);
        return FALSE;
    }
    return TRUE;
}

                                                                    
static gboolean
_rollback_records(GPtrArray *records)
{
    gboolean all_ok = TRUE;
    for (guint i = records->len; i > 0; i--) {
        PcvIscsiRecordUpdate *record = g_ptr_array_index(records, i - 1);
        if (!record->committed)
            continue;
        GError *ignored = NULL;
        if (!_stage_bytes(record, record->original, record->original_len, &ignored) ||
            renameat(record->parent_fd, record->temp_name,
                     record->parent_fd, record->name) != 0) {
            all_ok = FALSE;
            if (record->temp_name)
                unlinkat(record->parent_fd, record->temp_name, 0);
            g_clear_pointer(&record->temp_name, g_free);
        } else {
            g_clear_pointer(&record->temp_name, g_free);
            record->committed = FALSE;
            if (fsync(record->parent_fd) != 0)
                all_ok = FALSE;
        }
        g_clear_error(&ignored);
    }
    return all_ok;
}

gboolean
pcv_iscsi_node_db_set_chap_at(const gchar *node_root,
                               const gchar *lock_root,
                               guint lock_timeout_ms,
                               const gchar *target_iqn,
                               const gchar *portal,
                               const gchar *username,
                               const gchar *password,
                               const PcvIscsiNodeDbHooks *hooks,
                               GError **error)
{
    if (!node_root || !*node_root || !lock_root || !*lock_root ||
        !_is_single_component(target_iqn) || !_credential_is_valid(username) ||
        !_credential_is_valid(password)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "iSCSI node DB CHAP 입력 계약 위반");
        return FALSE;
    }

    PcvIscsiPortal request = {0};
    if (!_parse_requested_portal(portal, &request, error))
        return FALSE;

    PcvIscsiDbLock lock = {.dir_fd = -1, .lock_fd = -1};
    GPtrArray *records = g_ptr_array_new_with_free_func(_record_update_free);
    GError *local_error = NULL;
    gboolean success = FALSE;

    if (!_lock_acquire(lock_root, lock_timeout_ms, &lock, &local_error))
        goto out;
    if (!_collect_records(node_root, target_iqn, &request, records, &local_error))
        goto out;
    g_ptr_array_sort(records, _record_compare);

    for (guint i = 0; i < records->len; i++) {
        PcvIscsiRecordUpdate *record = g_ptr_array_index(records, i);
        if (!_stage_record(record, target_iqn, &request, username, password,
                           &local_error))
            goto out;
    }

    for (guint i = 0; i < records->len; i++) {
        PcvIscsiRecordUpdate *record = g_ptr_array_index(records, i);
        if (hooks && hooks->before_commit) {
            GError *hook_error = NULL;
            if (!hooks->before_commit(i, record->display_path, hooks->user_data,
                                      &hook_error)) {
                g_clear_error(&hook_error);
                g_set_error_literal(&local_error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "iSCSI node DB commit hook가 갱신을 거부함");
                goto out;
            }
            g_clear_error(&hook_error);
        }
        if (!_commit_record(record, &local_error))
            goto out;
    }
    success = TRUE;

out:
    if (!success && !_rollback_records(records)) {
        g_clear_error(&local_error);
        g_set_error_literal(&local_error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "iSCSI node DB 갱신 실패 후 rollback이 불완전함");
    }
    if (lock.held) {
        GError *unlock_error = NULL;
        if (!_lock_release(&lock, &unlock_error)) {
            if (success || !local_error) {
                g_clear_error(&local_error);
                local_error = unlock_error;
                unlock_error = NULL;
            }
            success = FALSE;
        }
        g_clear_error(&unlock_error);
    } else {
        _lock_clear(&lock);
    }

    g_ptr_array_unref(records);
    g_free(request.address);
    if (!success) {
        if (error)
            g_propagate_error(error, local_error);
        else
            g_clear_error(&local_error);
    } else {
        g_clear_error(&local_error);
    }
    return success;
}

gboolean
pcv_iscsi_node_db_set_chap_candidates_at(const gchar *primary_node_root,
                                          const gchar *legacy_node_root,
                                          const gchar *lock_root,
                                          guint lock_timeout_ms,
                                          const gchar *target_iqn,
                                          const gchar *portal,
                                          const gchar *username,
                                          const gchar *password,
                                          GError **error)
{
    if (!primary_node_root || !*primary_node_root ||
        !legacy_node_root || !*legacy_node_root ||
        !_is_single_component(target_iqn)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "iSCSI node DB root 선택 입력 계약 위반");
        return FALSE;
    }

    const gchar *candidates[] = {primary_node_root, legacy_node_root};
    const gchar *selected = NULL;
    for (guint i = 0; i < G_N_ELEMENTS(candidates); i++) {
        gchar *target_path = g_build_filename(candidates[i], target_iqn, NULL);
        struct stat st;
        if (lstat(target_path, &st) == 0) {
            if (!S_ISDIR(st.st_mode)) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "iSCSI node DB target root가 안전한 디렉터리가 아님(%s)",
                            target_path);
                g_free(target_path);
                return FALSE;
            }
            if (selected) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                            "iSCSI node DB target이 primary/legacy root에 모두 존재함(%s)",
                            target_iqn);
                g_free(target_path);
                return FALSE;
            }
            selected = candidates[i];
        } else if (errno != ENOENT && errno != ENOTDIR) {
            _set_errno_error(error, "candidate target 확인", target_path, errno);
            g_free(target_path);
            return FALSE;
        }
        g_free(target_path);
    }

    if (!selected) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "iSCSI node DB target record가 배포 root에 없음(%s)",
                    target_iqn);
        return FALSE;
    }

    return pcv_iscsi_node_db_set_chap_at(
        selected, lock_root, lock_timeout_ms, target_iqn, portal,
        username, password, NULL, error);
}

gboolean
pcv_iscsi_node_db_set_chap(const gchar *target_iqn,
                            const gchar *portal,
                            const gchar *username,
                            const gchar *password,
                            GError **error)
{
    return pcv_iscsi_node_db_set_chap_candidates_at(
        PCV_ISCSI_NODE_DB_PRIMARY_ROOT, PCV_ISCSI_NODE_DB_LEGACY_ROOT,
        PCV_ISCSI_NODE_DB_LOCK_ROOT,
        PCV_ISCSI_NODE_DB_LOCK_TIMEOUT_MS, target_iqn, portal,
        username, password, error);
}
