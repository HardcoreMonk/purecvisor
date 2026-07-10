/* ==========================================================================
 * src/modules/plugin/pcv_plugin_api.h
 * PureCVisor — 플러그인 공개 ABI (v1) — .so가 구현해야 할 인터페이스
 *
 * [파일 역할]
 *   외부 플러그인(.so)이 PureCVisor 데몬에 RPC 핸들러를 등록하기 위해
 *   사용하는 ABI(Application Binary Interface) 계약서.
 *   플러그인 개발자는 이 헤더만 include하여 플러그인을 작성합니다.
 *
 * [플러그인 개발 가이드]
 *   1. 이 헤더를 include
 *   2. pcv_plugin_get_meta() 구현 — PcvPluginMeta 반환 (이름/버전/ABI버전)
 *   3. pcv_plugin_register(PcvPluginRegistry*) 구현
 *      — pcv_plugin_registry_add()로 RPC 메서드 등록
 *   4. (선택) pcv_plugin_shutdown() 구현 — 종료 시 정리
 *   5. gcc -shared -o myplugin.so myplugin.c `pkg-config --cflags --libs ...`
 *   6. /etc/purecvisor/plugins.d/ 에 .so 파일 배치
 *   7. 데몬 재시작 시 자동 로딩
 *
 * [ABI 호환성]
 *   PCV_PLUGIN_ABI_VERSION이 데몬과 플러그인 간 일치해야 로딩됨.
 *   향후 ABI 변경 시 버전 번호를 올려 하위 호환성을 보호함.
 *
 * [핸들러 시그니처]
 *   모든 RPC 핸들러는 동일한 함수 시그니처(PcvRpcHandler)를 가짐:
 *   (JsonObject *params, const gchar *rpc_id, gpointer server,
 *    GSocketConnection *connection)
 *   디스패처의 기존 핸들러와 동일한 패턴으로 fire-and-forget 응답 가능.
 * ========================================================================== */

#ifndef PURECVISOR_PLUGIN_API_H
#define PURECVISOR_PLUGIN_API_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include <gio/gio.h>

/** ABI 버전 — 데몬과 플러그인 간 이 값이 일치해야 로딩 허용 */
#define PCV_PLUGIN_ABI_VERSION 1

/**
 * PcvPluginMeta:
 * 플러그인 메타데이터 — pcv_plugin_get_meta()가 반환하는 정적 구조체.
 *
 * @name:        플러그인 이름 (로그/목록에 표시)
 * @version:     버전 문자열 (예: "1.0")
 * @author:      개발자 이름/이메일
 * @abi_version: ABI 버전 (반드시 PCV_PLUGIN_ABI_VERSION과 동일해야 함)
 */
typedef struct {
    const gchar *name;
    const gchar *version;
    const gchar *author;
    guint        abi_version;
} PcvPluginMeta;

/**
 * PcvRpcHandler:
 * RPC 핸들러 함수 포인터 타입 — 디스패처의 모든 핸들러 공통 시그니처.
 *
 * @params:     JSON-RPC params 객체
 * @rpc_id:     JSON-RPC 요청 ID (응답 빌드용)
 * @server:     UDS 서버 포인터 (pure_uds_server_send_response용)
 * @connection: 소켓 연결 (응답 전송용, 전송 후 자동 닫힘)
 */
typedef void (*PcvRpcHandler)(JsonObject *params, const gchar *rpc_id,
                               gpointer server, GSocketConnection *connection);

/** 불투명(opaque) 레지스트리 타입 — pcv_plugin_registry_add()에서만 사용 */
typedef struct _PcvPluginRegistry PcvPluginRegistry;

/* ── 플러그인이 export해야 할 함수 타입 ───────────────────────── */

/** 필수: 플러그인 메타데이터 반환 (정적 구조체 포인터) */
typedef const PcvPluginMeta* (*PcvPluginGetMetaFunc)(void);

/** 필수: RPC 메서드 등록 — pcv_plugin_registry_add() 호출 */
typedef void (*PcvPluginRegisterFunc)(PcvPluginRegistry *registry);

/** 선택: 데몬 종료 시 정리 콜백 */
typedef void (*PcvPluginShutdownFunc)(void);

/* ── 레지스트리 API (플러그인이 register 콜백 내부에서 호출) ──── */

/**
 * pcv_plugin_registry_add:
 * @reg:         레지스트리 포인터 (register 콜백에 전달됨)
 * @method_name: 등록할 RPC 메서드 이름 (예: "myplugin.hello")
 * @handler:     핸들러 함수 포인터
 *
 * 플러그인의 RPC 메서드를 디스패처에 등록합니다.
 * 등록된 메서드는 dispatcher.c의 else-if 체인 끝에서 검색됩니다.
 */
void pcv_plugin_registry_add(PcvPluginRegistry *reg,
                              const gchar *method_name,
                              PcvRpcHandler handler);

#endif /* PURECVISOR_PLUGIN_API_H */
