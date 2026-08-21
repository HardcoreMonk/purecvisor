   
                             
                                                       
  
                           
                                                   
                                                    
                                        
  
                                                           
                                                          
                                                         
                         
  
                                                              
                                     
   
#include "modules/security/pcv_suricata_rules.h"

#include "modules/security/pcv_suricata.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_log.h"
#include "modules/audit/pcv_audit.h"

#include <errno.h>
#include <glib/gstdio.h>

#define SURICATA_RULES_LOG_DOM "suricata-rules"

                                                                
                                                     
                        
#define SURICATA_RULES_DOWNLOAD_SPAWN_TIMEOUT_SEC 150
#define SURICATA_RULES_VALIDATE_TIMEOUT_SEC       120

                                                    
static GQuark
_rules_error_quark(void)
{
    return g_quark_from_static_string("pcv-suricata-rules");
}

                                                                            
typedef enum {
    PCV_SURICATA_RULES_ERR_SCHEME = 1,                             
    PCV_SURICATA_RULES_ERR_ABSENT,                                   
    PCV_SURICATA_RULES_ERR_RENAME,                            
    PCV_SURICATA_RULES_ERR_CHMOD,                                          
} PcvSuricataRulesErr;

                                                            

                                                          
                                             
static gboolean
_rules_download_real(const gchar *url, const gchar *tmp_path, GError **error)
{
                                                                             
    const gchar *argv[] = {"curl", "-fsSL", "--max-time", "120", "-o", tmp_path, url, NULL};
    gchar *errout = NULL;
    gboolean ok = pcv_spawn_sync_timeout(argv, NULL, &errout,
                                          SURICATA_RULES_DOWNLOAD_SPAWN_TIMEOUT_SEC, error);
    if (!ok) {
        PCV_LOG_ERROR(SURICATA_RULES_LOG_DOM, "룰 다운로드(curl) 실패(%s): %s",
                      url, errout ? errout : "no stderr");
    }
    g_free(errout);
    return ok;
}

                                      
                                                               
gboolean
pcv_suricata_rules_validate(const gchar *rules_path, GError **error)
{
                                                 
                                                                
    gchar *bin = g_find_program_in_path("suricata");
    if (!bin) {
        g_set_error(error, _rules_error_quark(), PCV_SURICATA_RULES_ERR_ABSENT,
                    "suricata binary not found — rules validation unavailable "
                    "(미설치 degraded)");
        return FALSE;
    }
    g_free(bin);

    const gchar *argv[] = {"suricata", "-T", "-c", PCV_SURICATA_CONFIG_PATH,
                            "-S", rules_path, NULL};
    gchar *errout = NULL;
    gboolean ok = pcv_spawn_sync_timeout(argv, NULL, &errout,
                                          SURICATA_RULES_VALIDATE_TIMEOUT_SEC, error);
    if (!ok) {
        PCV_LOG_ERROR(SURICATA_RULES_LOG_DOM, "suricata -T 검증 실패(%s): %s",
                      rules_path, errout ? errout : "unknown error");
    }
    g_free(errout);
    return ok;
}

                                                                   

                                                
                                                  
                                                             
gboolean
pcv_suricata_rules_update_at(const gchar *rules_dir, const gchar *url,
                              const gchar *admin, PcvSuricataRulesHooks *hooks,
                              GError **error)
{
    g_return_val_if_fail(rules_dir != NULL, FALSE);
    g_return_val_if_fail(url != NULL, FALSE);

    const gchar *actor = admin ? admin : "system";                                     
    gint64 start_us = g_get_monotonic_time();                                 
    gboolean success = FALSE;
    gboolean bak_created = FALSE;                                         
    GError *local_err = NULL;

                                                           
    PcvSuricataRulesDownloadFn download_fn =
        (hooks && hooks->download) ? hooks->download : _rules_download_real;
    PcvSuricataRulesValidateFn validate_fn =
        (hooks && hooks->validate) ? hooks->validate : pcv_suricata_rules_validate;
    PcvSuricataRulesReloadFn reload_fn =
        (hooks && hooks->reload) ? hooks->reload : pcv_suricata_reload;

    gchar *rules_path = g_build_filename(rules_dir, PCV_SURICATA_RULES_FILE, NULL);
    gchar *tmp_path = g_strdup_printf("%s.tmp", rules_path);
    gchar *bak_path = g_strdup_printf("%s.bak", rules_path);

                                                            
                                                      
                                                  
                                                       
    if (!g_str_has_prefix(url, "http://") && !g_str_has_prefix(url, "https://")) {
        g_set_error(&local_err, _rules_error_quark(), PCV_SURICATA_RULES_ERR_SCHEME,
                    "지원하지 않는 URL 스킴(http/https만 허용): %s", url);
        goto done;
    }

                                                
    if (!download_fn(url, tmp_path, &local_err)) {
        PCV_LOG_ERROR(SURICATA_RULES_LOG_DOM, "룰 다운로드 실패(%s): %s",
                      url, local_err ? local_err->message : "unknown error");
        g_unlink(tmp_path);
        goto done;
    }

                                                         
                                                                      
                                                                 
                                                         
                                                          
                                                                               
                                                            
                                                              
                                              
                                                                        
                                                            
                                                        
    if (g_chmod(tmp_path, 0644) != 0) {
        gint eno = errno;
        g_set_error(&local_err, _rules_error_quark(), PCV_SURICATA_RULES_ERR_CHMOD,
                    "룰셋 tmp 모드 0644 고정 실패: %s (%s)", tmp_path, g_strerror(eno));
        PCV_LOG_ERROR(SURICATA_RULES_LOG_DOM,
                      "%s — 엔진(uid 997)이 읽지 못하므로 교체 중단", local_err->message);
        g_unlink(tmp_path);
        goto done;
    }

                                                           
    if (!validate_fn(tmp_path, &local_err)) {
        PCV_LOG_ERROR(SURICATA_RULES_LOG_DOM,
                      "룰 검증 실패(fail-safe, 기존 rules 무변경): %s",
                      local_err ? local_err->message : "unknown error");
        g_unlink(tmp_path);
        goto done;
    }

                                                       
                                                 
    if (g_file_test(rules_path, G_FILE_TEST_EXISTS)) {
        gchar *old_contents = NULL;
        gsize old_len = 0;
        if (!g_file_get_contents(rules_path, &old_contents, &old_len, &local_err)) {
            PCV_LOG_ERROR(SURICATA_RULES_LOG_DOM,
                          "기존 rules 읽기 실패(백업 불가, 교체 중단): %s",
                          local_err ? local_err->message : "unknown error");
            g_unlink(tmp_path);
            goto done;
        }
        gboolean bak_ok = g_file_set_contents(bak_path, old_contents, old_len, &local_err);
        g_free(old_contents);
        if (!bak_ok) {
            PCV_LOG_ERROR(SURICATA_RULES_LOG_DOM,
                          "백업(bak) 기록 실패(교체 중단): %s",
                          local_err ? local_err->message : "unknown error");
            g_unlink(tmp_path);
            goto done;
        }
                                                                         
                                                            
                                                   
        if (g_chmod(bak_path, 0644) != 0) {
            gint eno = errno;
            g_set_error(&local_err, _rules_error_quark(), PCV_SURICATA_RULES_ERR_CHMOD,
                        "백업(bak) 모드 0644 고정 실패: %s (%s)", bak_path, g_strerror(eno));
            PCV_LOG_ERROR(SURICATA_RULES_LOG_DOM,
                          "%s — 복원 시 엔진이 읽지 못하므로 교체 중단", local_err->message);
            g_unlink(bak_path);
            g_unlink(tmp_path);
            goto done;
        }
        bak_created = TRUE;
    }

                                                          
                                                  
                                     
    if (g_rename(tmp_path, rules_path) != 0) {
        gint eno = errno;
        g_set_error(&local_err, _rules_error_quark(), PCV_SURICATA_RULES_ERR_RENAME,
                    "원자 교체 rename 실패: %s -> %s (%s)",
                    tmp_path, rules_path, g_strerror(eno));
        PCV_LOG_ERROR(SURICATA_RULES_LOG_DOM, "%s", local_err->message);
        if (bak_created) g_unlink(bak_path);
        g_unlink(tmp_path);
        goto done;
    }

                                                                  
                                                  
                                                    
                                                 
                                               
    if (!reload_fn(&local_err)) {
        PCV_LOG_ERROR(SURICATA_RULES_LOG_DOM,
                      "reload 실패 — bak 복원 시도: %s",
                      local_err ? local_err->message : "unknown error");
        if (bak_created) {
            if (g_rename(bak_path, rules_path) == 0) {
                GError *retry_err = NULL;
                if (!reload_fn(&retry_err)) {
                    PCV_LOG_ERROR(SURICATA_RULES_LOG_DOM,
                                  "bak 복원 후 reload 재시도도 실패 — suricata 엔진은 "
                                  "구 룰셋을 메모리에 유지한 채입니다(수동 확인 필요): %s",
                                  retry_err ? retry_err->message : "unknown error");
                    g_clear_error(&retry_err);
                }
            } else {
                gint eno2 = errno;
                PCV_LOG_ERROR(SURICATA_RULES_LOG_DOM,
                              "bak 복원 rename도 실패(%s) — rules가 신규 내용으로 "
                              "남아있으나 reload가 반영되지 않았을 수 있습니다(수동 확인 필요)",
                              g_strerror(eno2));
            }
        } else {
            PCV_LOG_ERROR(SURICATA_RULES_LOG_DOM,
                          "첫 설치 직후 reload 실패 — 복원할 이전 rules가 없습니다"
                          "(수동 확인 필요)");
        }
        goto done;
    }

    success = TRUE;

done:
    {
        gint64 duration_ms = (g_get_monotonic_time() - start_us) / 1000;
        if (success) {
            pcv_audit_log(actor, "suricata.rules.update", url, "ok", 0, duration_ms, "local");
        } else {
            gint code = local_err ? local_err->code : -1;
            pcv_audit_log(actor, "suricata.rules.update", url, "fail", code, duration_ms, "local");
        }
    }

    if (local_err) {
        g_propagate_error(error, local_err);
    }
    g_free(rules_path);
    g_free(tmp_path);
    g_free(bak_path);
    return success;
}

                                                            
                                                                
gboolean
pcv_suricata_rules_update(const gchar *source_url, const gchar *admin, GError **error)
{
                                                  
                                                          
                                                   
                               
    gint64 start_us = g_get_monotonic_time();
    gchar *bin = g_find_program_in_path("suricata");
    if (!bin) {
        g_set_error(error, _rules_error_quark(), PCV_SURICATA_RULES_ERR_ABSENT,
                    "suricata binary not found — rules update unavailable "
                    "(미설치 degraded, 다운로드 시도하지 않음)");
        gint64 duration_ms = (g_get_monotonic_time() - start_us) / 1000;
        pcv_audit_log(admin ? admin : "system", "suricata.rules.update", source_url,
                      "fail", PCV_SURICATA_RULES_ERR_ABSENT, duration_ms, "local");
        return FALSE;
    }
    g_free(bin);

    return pcv_suricata_rules_update_at(PCV_SURICATA_RULES_DIR, source_url, admin, NULL, error);
}
