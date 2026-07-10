#include "pcv_bootstrap.h"

/*
 * Single Edge RPC 확장 등록 지점.
 *
 * 현재 Single Edge는 공통 dispatcher가 등록한 RPC만 사용하므로 이 파일의
 * 함수들은 의도적으로 비어 있습니다. 빈 구현을 유지하면 main/dispatcher는
 * "에디션별 추가 RPC가 있을 수도 있다"는 같은 호출 흐름을 쓰면서, Single
 * 빌드에서는 새 route나 async method를 노출하지 않습니다.
 *
 * [주니어 참고]
 * 여기에 코드를 추가한다는 것은 외부 API 표면이 늘어난다는 뜻입니다.
 * 새 메서드를 등록할 때는 RBAC 최소 role, audit, fire-and-forget 결과 채널,
 * docs/ADR_INDEX.md의 적용 상태를 함께 확인해야 합니다.
 */
void
pcv_bootstrap_register_async_methods(GHashTable *async_methods)
{
    (void)async_methods;
}

void
pcv_bootstrap_register_rpc_routes(GHashTable *rpc_routes)
{
    (void)rpc_routes;
}
