#include "rest_auth.h"

#include <gio/gio.h>

/*
 * REST 최초 부트스트랩 fallback 허용 조건.
 *
 * 이 함수는 daemon.conf의 관리자 계정이 아직 RBAC DB에 등록되지 않은
 * 첫 설치 상태만 복구하기 위한 보안 게이트다. 계정 잠금, DB 장애, 다른
 * 권한 거부 사유까지 JWT 직접 발급으로 우회하면 brute-force 방어와 RBAC
 * 정책이 깨지므로 조건을 의도적으로 좁게 유지한다.
 */
gboolean
pcv_rest_auth_should_fallback_bootstrap(const gchar *username,
                                        const gchar *password,
                                        const gchar *cfg_user,
                                        const gchar *cfg_pass,
                                        const GError *rbac_error)
{
    if (!username || !password || !cfg_user || !cfg_pass) {
        return FALSE;
    }

    if (g_strcmp0(username, cfg_user) != 0 ||
        g_strcmp0(password, cfg_pass) != 0) {
        return FALSE;
    }

    if (!rbac_error) {
        return TRUE;
    }

    if (rbac_error->domain != G_IO_ERROR ||
        rbac_error->code != G_IO_ERROR_PERMISSION_DENIED) {
        return FALSE;
    }

    return g_strcmp0(rbac_error->message, "Invalid credentials") == 0;
}
