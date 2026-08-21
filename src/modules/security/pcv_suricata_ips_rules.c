   
                                 
                                                                     
  
                           
                                                   
                                                    
                                        
  
                                                      
                                                            
                                                      
                                                   
                                                     
                      
  
                                                    
                                                         
                                                     
          
  
                                                  
                                                         
                                                       
                                                       
                                              
                                                    
                                          
                                                   
   
#include "modules/security/pcv_suricata_ips_rules.h"

#include "modules/security/pcv_suricata.h"                                          
#include "modules/security/pcv_suricata_rules.h"                                      
#include "modules/audit/pcv_audit.h"
#include "utils/pcv_log.h"
#include "utils/pcv_spawn.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>

#define IPS_RULES_LOG_DOM "suricata-ips-rules"
#define IPS_RULES_UNIT    "suricata-ips"

                                                         
                                                       
#define PCV_IPS_RULES_VALIDATE_TIMEOUT_SEC 45
                                                                
#define IPS_RULES_SYSTEMCTL_TIMEOUT_SEC     5

                                                      
                                      
                                                 
static gboolean
_line_first_token_is_alert(const gchar *line, gsize line_len)
{
    if (line_len < 5 || strncmp(line, "alert", 5) != 0)
        return FALSE;
                                                           
    return (line_len == 5) || g_ascii_isspace(line[5]);
}

                                                          
                                            
  
                                        
                                                  
                                                      
                                
                                                                        
                                                
                                                    
                                                    
                                                    
                                    
  
                                      
                                                         
                                                  
                                                  
                                          
                                              
                           
  
                                                        
                                                 
                                             
                     
                                             
                                                   
                               
static gboolean
_line_parse_sid(const gchar *line, gsize line_len, guint *out_sid)
{
    const gchar *line_end = line + line_len;
    const gchar *p = memchr(line, '(', line_len);
    if (!p)
        return FALSE;                                 
    p++;                                        

    while (p < line_end && *p != ')') {
        while (p < line_end && g_ascii_isspace(*p))                   
            p++;
        if (p >= line_end || *p == ')')
            break;

                                                            
                                   
        const gchar *key_start = p;
        while (p < line_end && *p != ':' && *p != ';' && *p != ')')
            p++;
        gsize key_len = (gsize)(p - key_start);
        gboolean is_sid = (key_len == 3 && strncmp(key_start, "sid", 3) == 0);

        if (p >= line_end || *p == ')')
            break;                                  

        if (*p == ';') {
            p++;                                             
            continue;
        }

                                   
        p++;

        if (is_sid) {
            const gchar *d = p;
            guint sid = 0;
            gboolean any_digit = FALSE;
            while (d < line_end && g_ascii_isdigit(*d)) {
                sid = sid * 10 + (guint)(*d - '0');
                d++;
                any_digit = TRUE;
            }
            if (any_digit && d < line_end && *d == ';') {
                *out_sid = sid;
                return TRUE;                     
            }
                                                       
                                                      
        }

                                                
                                                
                           
        gboolean in_quotes = FALSE;
        while (p < line_end) {
            if (in_quotes) {
                if (*p == '\\' && p + 1 < line_end) {                             
                    p += 2;
                    continue;
                }
                if (*p == '"')
                    in_quotes = FALSE;                         
                p++;
                continue;
            }
            if (*p == '"') {
                in_quotes = TRUE;
                p++;
                continue;
            }
            if (*p == ';') {
                p++;
                break;                                        
            }
            if (*p == ')')
                break;                                     
            p++;
        }
    }

    return FALSE;                                    
}

                                                             
gchar *
pcv_suricata_ips_rules_transform(const gchar *src, GHashTable *drop_sids, guint *out_converted)
{
    if (!src)
        return NULL;

    guint converted = 0;
    GString *out = g_string_sized_new(strlen(src) + 16);

    const gchar *p = src;
    while (*p != '\0') {
                                                         
                                                 
        const gchar *nl = strchr(p, '\n');
        const gchar *body_end = nl ? nl : p + strlen(p);
        gsize body_len = (gsize)(body_end - p);

                                                    
                                           
        gsize ws = 0;
        while (ws < body_len && g_ascii_isspace(p[ws]))
            ws++;
        gboolean blank_or_comment = (ws == body_len) || (p[ws] == '#');

        gboolean do_drop = FALSE;
        if (!blank_or_comment) {
            guint sid = 0;
                                                
                                      
                                                                   
            if (_line_parse_sid(p, body_len, &sid) &&
                g_hash_table_contains(drop_sids, GUINT_TO_POINTER(sid)) &&
                _line_first_token_is_alert(p, body_len))
                do_drop = TRUE;
        }

        if (do_drop) {
                                                                   
            g_string_append(out, "drop");
            g_string_append_len(out, p + 5, (gssize)(body_len - 5));
            converted++;
        } else {
            g_string_append_len(out, p, (gssize)body_len);
        }

        if (nl) {
            g_string_append_c(out, '\n');
            p = nl + 1;
        } else {
            p = body_end;                             
        }
    }

    if (out_converted)
        *out_converted = converted;

    return g_string_free(out, FALSE);
}

                                                     
  
                                                    
                                                           
                                

                                                    
static GQuark
_ips_rules_error_quark(void)
{
    return g_quark_from_static_string("pcv-suricata-ips-rules");
}

                                                         
                                      
typedef enum {
    PCV_IPS_RULES_ERR_ABSENT = 1,                               
    PCV_IPS_RULES_ERR_RENAME,                            
    PCV_IPS_RULES_ERR_CHMOD,                                
} PcvIpsRulesErr;

                                                         
                                                         
static gboolean
_ips_rules_validate_real(const gchar *rules_path, GError **error)
{
                                                                 
    gchar *bin = g_find_program_in_path("suricata");
    if (!bin) {
        g_set_error(error, _ips_rules_error_quark(), PCV_IPS_RULES_ERR_ABSENT,
                    "suricata binary not found — IPS 파생 룰셋 검증 불가(미설치 degraded)");
        return FALSE;
    }
    g_free(bin);

                                                            
                                                                  
    const gchar *argv[] = {"suricata", "-T", "-c", PCV_SURICATA_IPS_CONFIG_PATH,
                            "-S", rules_path, NULL};
    gchar *errout = NULL;
    gboolean ok = pcv_spawn_sync_timeout(argv, NULL, &errout,
                                          PCV_IPS_RULES_VALIDATE_TIMEOUT_SEC, error);
    if (!ok) {
        PCV_LOG_ERROR(IPS_RULES_LOG_DOM, "IPS 파생 룰셋 suricata -T 검증 실패(%s): %s",
                      rules_path, errout ? errout : "unknown error");
    }
    g_free(errout);
    return ok;
}

                                                     
                                                             
                                            
                                                   
                                      
static gboolean
_ips_rules_reload_real(GError **error)
{
                                                           
                                                                  
                                                            
    const gchar *probe[] = {"systemctl", "is-active", IPS_RULES_UNIT, NULL};
    gchar *out = NULL;
    pcv_spawn_sync_timeout(probe, &out, NULL, IPS_RULES_SYSTEMCTL_TIMEOUT_SEC, NULL);
    PcvSuricataState st = pcv_suricata_state_from_output(out);
    g_free(out);

    if (st != PCV_SURICATA_ACTIVE) {
                                                           
                                                             
                                            
        PCV_LOG_INFO(IPS_RULES_LOG_DOM,
                     "suricata-ips 유닛이 active 아님(%s) — reload 생략(다음 start 가 "
                     "새 파생 룰셋을 로드)", pcv_suricata_state_str(st));
        return TRUE;
    }

    const gchar *argv[] = {"systemctl", "reload", IPS_RULES_UNIT, NULL};
    gchar *errout = NULL;
    gboolean ok = pcv_spawn_sync_timeout(argv, NULL, &errout,
                                          IPS_RULES_SYSTEMCTL_TIMEOUT_SEC, error);
    if (!ok) {
        PCV_LOG_ERROR(IPS_RULES_LOG_DOM, "suricata-ips reload 실패: %s",
                      errout ? errout : "unknown error");
    }
    g_free(errout);
    return ok;
}

                                                 
                                                
                                                
                                     
gboolean
pcv_suricata_ips_rules_apply_at(const gchar *src_path, const gchar *out_path,
                                 GHashTable *drop_sids, const gchar *admin,
                                 const PcvIpsRulesHooks *hooks, GError **error)
{
    g_return_val_if_fail(src_path != NULL, FALSE);
    g_return_val_if_fail(out_path != NULL, FALSE);

    const gchar *actor = admin ? admin : "system";                                     
    gint64 start_us = g_get_monotonic_time();                                 
    gboolean success = FALSE;
    gboolean bak_created = FALSE;                                         
    GError *local_err = NULL;

                                                       
    PcvIpsRulesValidateFn validate_fn =
        (hooks && hooks->validate) ? hooks->validate : _ips_rules_validate_real;
    PcvIpsRulesReloadFn reload_fn =
        (hooks && hooks->reload) ? hooks->reload : _ips_rules_reload_real;

                                                                     
    gchar *tmp_path = g_strdup_printf("%s.tmp", out_path);
    gchar *bak_path = g_strdup_printf("%s.bak", out_path);
    gchar *src_text = NULL;
    gchar *derived = NULL;
    guint converted = 0;                                        

                                                                
    if (!g_file_get_contents(src_path, &src_text, NULL, &local_err)) {
        PCV_LOG_ERROR(IPS_RULES_LOG_DOM, "원본 룰 읽기 실패(%s): %s",
                      src_path, local_err ? local_err->message : "unknown error");
        goto done;
    }

                                               
    derived = pcv_suricata_ips_rules_transform(src_text, drop_sids, &converted);
    PCV_LOG_INFO(IPS_RULES_LOG_DOM, "파생 룰셋 생성: %u개 룰 alert→drop (원본 %s)",
                 converted, src_path);

                                             
    if (!g_file_set_contents(tmp_path, derived, -1, &local_err)) {
        PCV_LOG_ERROR(IPS_RULES_LOG_DOM, "파생 룰셋 tmp 쓰기 실패(%s): %s",
                      tmp_path, local_err ? local_err->message : "unknown error");
        g_unlink(tmp_path);
        goto done;
    }
                                                         
                                                             
                                                               
                                                       
                                                     
                                                         
                                         
                                                           
                                                                
                                               
                                             
    if (g_chmod(tmp_path, 0644) != 0) {
        gint eno = errno;
        g_set_error(&local_err, _ips_rules_error_quark(), PCV_IPS_RULES_ERR_CHMOD,
                    "파생 룰셋 tmp 모드 0644 고정 실패: %s (%s)", tmp_path, g_strerror(eno));
        PCV_LOG_ERROR(IPS_RULES_LOG_DOM, "%s — 엔진(uid 997)이 읽지 못하므로 교체 중단",
                      local_err->message);
        g_unlink(tmp_path);
        goto done;
    }

                                                           
    if (!validate_fn(tmp_path, &local_err)) {
        PCV_LOG_ERROR(IPS_RULES_LOG_DOM,
                      "파생 룰셋 검증 실패(fail-safe, 기존 파생본 무변경): %s",
                      local_err ? local_err->message : "unknown error");
        g_unlink(tmp_path);
        goto done;
    }

                                                       
                                               
    if (g_file_test(out_path, G_FILE_TEST_EXISTS)) {
        gchar *old_contents = NULL;
        gsize old_len = 0;
        if (!g_file_get_contents(out_path, &old_contents, &old_len, &local_err)) {
            PCV_LOG_ERROR(IPS_RULES_LOG_DOM,
                          "기존 파생본 읽기 실패(백업 불가, 교체 중단): %s",
                          local_err ? local_err->message : "unknown error");
            g_unlink(tmp_path);
            goto done;
        }
        gboolean bak_ok = g_file_set_contents(bak_path, old_contents, (gssize)old_len, &local_err);
        g_free(old_contents);
        if (!bak_ok) {
            PCV_LOG_ERROR(IPS_RULES_LOG_DOM, "백업(bak) 기록 실패(교체 중단): %s",
                          local_err ? local_err->message : "unknown error");
            g_unlink(tmp_path);
            goto done;
        }
                                                                    
                                                       
                                                           
        if (g_chmod(bak_path, 0644) != 0) {
            gint eno = errno;
            g_set_error(&local_err, _ips_rules_error_quark(), PCV_IPS_RULES_ERR_CHMOD,
                        "백업(bak) 모드 0644 고정 실패: %s (%s)", bak_path, g_strerror(eno));
            PCV_LOG_ERROR(IPS_RULES_LOG_DOM,
                          "%s — 롤백 시 엔진이 읽지 못하므로 교체 중단", local_err->message);
            g_unlink(bak_path);
            g_unlink(tmp_path);
            goto done;
        }
        bak_created = TRUE;
    }

                                                      
                                                       
    if (g_rename(tmp_path, out_path) != 0) {
        gint eno = errno;
        g_set_error(&local_err, _ips_rules_error_quark(), PCV_IPS_RULES_ERR_RENAME,
                    "파생 룰셋 원자 교체 rename 실패: %s -> %s (%s)",
                    tmp_path, out_path, g_strerror(eno));
        PCV_LOG_ERROR(IPS_RULES_LOG_DOM, "%s", local_err->message);
        if (bak_created) g_unlink(bak_path);
        g_unlink(tmp_path);
        goto done;
    }

                                                                 
                                                 
                                                     
                                                    
                                                     
    if (!reload_fn(&local_err)) {
        PCV_LOG_ERROR(IPS_RULES_LOG_DOM, "reload 실패 — 파생본 bak 복원 시도: %s",
                      local_err ? local_err->message : "unknown error");
        if (bak_created) {
            if (g_rename(bak_path, out_path) == 0) {
                GError *retry_err = NULL;
                if (!reload_fn(&retry_err)) {
                    PCV_LOG_ERROR(IPS_RULES_LOG_DOM,
                                  "bak 복원 후 reload 재시도도 실패 — suricata-ips 엔진은 "
                                  "구 파생 룰셋을 메모리에 유지한 채입니다(수동 확인 필요): %s",
                                  retry_err ? retry_err->message : "unknown error");
                    g_clear_error(&retry_err);
                }
            } else {
                gint eno2 = errno;
                PCV_LOG_ERROR(IPS_RULES_LOG_DOM,
                              "bak 복원 rename 도 실패(%s) — 파생본이 신규 내용으로 "
                              "남아있으나 reload 가 반영되지 않았을 수 있습니다(수동 확인 필요)",
                              g_strerror(eno2));
            }
        } else {
            PCV_LOG_ERROR(IPS_RULES_LOG_DOM,
                          "최초 생성 직후 reload 실패 — 복원할 이전 파생본이 없습니다"
                          "(수동 확인 필요)");
        }
        goto done;
    }

    success = TRUE;

done:
    {
                                                       
                                                          
        gint64 duration_ms = (g_get_monotonic_time() - start_us) / 1000;
        if (success) {
            pcv_audit_log(actor, "suricata.ips.rules.apply", out_path, "ok", 0,
                          duration_ms, "local");
        } else {
            gint code = local_err ? local_err->code : -1;
            pcv_audit_log(actor, "suricata.ips.rules.apply", out_path, "fail", code,
                          duration_ms, "local");
        }
    }

    if (local_err) {
        g_propagate_error(error, local_err);
    }
    g_free(src_text);
    g_free(derived);
    g_free(tmp_path);
    g_free(bak_path);
    return success;
}

                                                
  
                                                                
                                                        
                                                       
                                               
                                          
  
                                                     
                                                   
                                             
                                       
                                                    
  
                                                          
static GMutex g_ips_rules_apply_mu;

                                                  
                                               
                                                    
gboolean
pcv_suricata_ips_rules_apply(GHashTable *drop_sids, const gchar *admin, GError **error)
{
    g_mutex_lock(&g_ips_rules_apply_mu);
    gboolean ok = pcv_suricata_ips_rules_apply_at(PCV_SURICATA_RULES_PATH,
                                                  PCV_IPS_RULES_PATH,
                                                  drop_sids, admin, NULL, error);
    g_mutex_unlock(&g_ips_rules_apply_mu);
    return ok;
}

                                   
                                                
                                             
gboolean
pcv_suricata_ips_rules_stale_at(const gchar *src_path, const gchar *out_path)
{
    if (!src_path || !out_path)
        return FALSE;

    GStatBuf src_st, out_st;
                                                    
    if (g_stat(out_path, &out_st) != 0)
        return FALSE;
                                                     
    if (g_stat(src_path, &src_st) != 0)
        return FALSE;

                                                    
    if (src_st.st_mtim.tv_sec != out_st.st_mtim.tv_sec)
        return src_st.st_mtim.tv_sec > out_st.st_mtim.tv_sec;
    return src_st.st_mtim.tv_nsec > out_st.st_mtim.tv_nsec;
}

                               
                                        
gboolean
pcv_suricata_ips_rules_stale(void)
{
    return pcv_suricata_ips_rules_stale_at(PCV_SURICATA_RULES_PATH, PCV_IPS_RULES_PATH);
}
