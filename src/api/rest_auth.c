#include "rest_auth.h"

/*
 * REST 최초 부트스트랩 fallback 허용 조건.
 *
 * 이 함수는 daemon.conf의 관리자 계정이 아직 RBAC DB에 등록되지 않은
 * 첫 설치 상태만 복구하기 위한 보안 게이트다. 사용자가 이미 DB에 존재하면
 * (비번 회전 후 포함) fallback을 거부해야 옛 daemon.conf 비번이 상시
 * 우회 경로로 남는 SEC-2 백도어를 막을 수 있다.
 */
gboolean
pcv_rest_auth_should_fallback_bootstrap(const gchar *username,
                                        const gchar *password,
                                        const gchar *cfg_user,
                                        const gchar *cfg_pass,
                                        gboolean user_in_db)
{
    if (!username || !password || !cfg_user || !cfg_pass) return FALSE;
    if (g_strcmp0(username, cfg_user) != 0 ||
        g_strcmp0(password, cfg_pass) != 0) return FALSE;
    return !user_in_db;   /* 진짜 미시딩 상태에서만 비상 복구 허용 */
}
