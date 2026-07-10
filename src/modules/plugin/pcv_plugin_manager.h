/* ==========================================================================
 * src/modules/plugin/pcv_plugin_manager.h
 * PureCVisor — 플러그인 매니저 공개 API
 *
 * [파일 역할]
 *   /etc/purecvisor/plugins.d/ 디렉터리의 .so 공유 라이브러리를
 *   런타임에 동적 로딩하고, 플러그인이 등록한 RPC 메서드를
 *   디스패처에서 호출할 수 있도록 레지스트리를 관리하는 모듈의 공개 인터페이스.
 *
 * [아키텍처 위치]
 *   main.c       -> pcv_plugin_manager_init() / shutdown()
 *   dispatcher.c -> pcv_plugin_has_handler() → TRUE면 pcv_plugin_dispatch()
 *
 * [플러그인 ABI 규약 (v1)]
 *   .so 파일은 반드시 다음 심볼을 export 해야 합니다:
 *     pcv_plugin_get_meta()   — 필수: PcvPluginMeta* 반환
 *     pcv_plugin_register()   — 필수: RPC 핸들러 등록
 *     pcv_plugin_shutdown()   — 선택: 종료 정리
 *   ABI 버전 불일치 시 로딩 거부 (PCV_PLUGIN_ABI_VERSION 확인)
 *
 * [제한사항]
 *   MAX_PLUGINS=16, MAX_METHODS=64 (고정 배열)
 *   런타임 언로드(pcv_plugin_unload)는 레지스트리 정리 미구현 (향후 개선)
 *
 * [메모리 관리]
 *   pcv_plugin_list() 반환 JsonArray: 호출자 json_array_unref()
 * ========================================================================== */

#ifndef PURECVISOR_PLUGIN_MANAGER_H
#define PURECVISOR_PLUGIN_MANAGER_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include <gio/gio.h>
#include "pcv_plugin_api.h"

G_BEGIN_DECLS

/** 플러그인 매니저 초기화 — 디렉터리 스캔 + 전체 .so 로딩 */
void       pcv_plugin_manager_init(const gchar *plugin_dir);

/** 플러그인 매니저 종료 — 역순 shutdown 콜백 호출 + GModule close */
void       pcv_plugin_manager_shutdown(void);

/** 레지스트리에 해당 RPC 메서드 핸들러가 있는지 확인 (dispatcher.c용) */
gboolean   pcv_plugin_has_handler(const gchar *method);

/** 레지스트리에서 핸들러를 찾아 실행 (has_handler 확인 후 호출) */
void       pcv_plugin_dispatch(const gchar *method, JsonObject *params,
                                const gchar *rpc_id, gpointer server,
                                GSocketConnection *connection);

/** 로딩된 플러그인 목록 — JsonArray [{name, version}] */
JsonArray *pcv_plugin_list(void);

/** 단일 플러그인 동적 로딩 (ABI 검증 포함) */
gboolean   pcv_plugin_load(const gchar *path, GError **error);

/** 플러그인 언로드 (shutdown 콜백 + GModule close, 레지스트리 정리 미구현) */
gboolean   pcv_plugin_unload(const gchar *name, GError **error);

G_END_DECLS

#endif /* PURECVISOR_PLUGIN_MANAGER_H */
