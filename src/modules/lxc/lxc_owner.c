/* ==========================================================================
 * src/modules/lxc/lxc_owner.c
 * PureCVisor — 컨테이너 operator owner-scope 소유자 저장소 (B1 IDOR 시정)
 *
 * 소유자 subject를 <container_path>/<name>/purecvisor.owner 에 저장/조회한다.
 * 경로 규칙은 lxc_driver.c의 purecvisor.meta 기록(PCV_LXC_PATH =
 * pcv_config_get_container_path())과 동일하되, 별도 파일이라 기존 .meta(이미지
 * 태그) 포맷을 깨지 않는다.
 * ========================================================================== */

#include "lxc_owner.h"
#include "utils/pcv_config.h"
#include "utils/pcv_log.h"

#include <glib/gstdio.h>

#define LXC_OWNER_LOG_DOM "lxc_owner"

/* <container_path>/<name>/purecvisor.owner — meta_path 구성과 동일 규칙 */
static gchar *
_pcv_lxc_owner_path(const gchar *name)
{
    if (!name || !*name)
        return NULL;
    return g_strdup_printf("%s/%s/purecvisor.owner",
                           pcv_config_get_container_path(), name);
}

gboolean
pcv_lxc_stamp_owner(const gchar *name, const gchar *owner_sub)
{
    /* owner_sub 부재(예: UDS 직결 admin 요청) 시 스탬프하지 않는다 — 소유자 없는
     * 컨테이너는 VM 소유자 metadata 부재와 동일하게 fail-secure(operator deny,
     * admin만 조작)로 취급된다. */
    if (!name || !*name || !owner_sub || !*owner_sub)
        return FALSE;

    gchar *path = _pcv_lxc_owner_path(name);
    if (!path)
        return FALSE;

    GError *err = NULL;
    gboolean ok = g_file_set_contents(path, owner_sub, -1, &err);
    if (!ok) {
        PCV_LOG_WARN(LXC_OWNER_LOG_DOM,
                     "owner 스탬프 실패 '%s': %s",
                     name, err ? err->message : "unknown");
        g_clear_error(&err);
    }
    g_free(path);
    return ok;
}

gchar *
pcv_lxc_read_owner(const gchar *name)
{
    gchar *path = _pcv_lxc_owner_path(name);
    if (!path)
        return NULL;

    gchar *content = NULL;
    if (g_file_get_contents(path, &content, NULL, NULL) && content) {
        g_strstrip(content);
        if (content[0]) {
            g_free(path);
            return content;
        }
        g_free(content);
    }
    g_free(path);
    return NULL;
}
