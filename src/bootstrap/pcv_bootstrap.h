#ifndef PCV_BOOTSTRAP_H
#define PCV_BOOTSTRAP_H

#include <glib.h>

/*
 * 에디션별 부트스트랩 경계.
 *
 * 이 저장소는 Single Edge 빌드가 기준이지만, 공통 코드에는 과거 Multi/Cluster
 * 에디션에서 사용하던 진입점 이름이 일부 남아 있습니다. main.c와 dispatcher.c가
 * 에디션별 파일명을 직접 알지 않도록 이 헤더가 "공통 계약서" 역할을 합니다.
 *
 * [비전공자 설명]
 * 같은 제품 화면에 "클러스터 상태" 버튼이 있더라도 Single Edge 서버는 여러
 * 서버를 묶는 기능을 제공하지 않습니다. 그래서 아래 함수들은 Single 빌드에서
 * 실제 클러스터를 시작하지 않고, "이 기능은 꺼져 있다"는 안정적인 응답을
 * 돌려주도록 연결됩니다.
 *
 * [주니어 참고]
 * 새 런타임 초기화 단계를 추가할 때는 main.c에 에디션 분기를 흩뿌리지 말고
 * 이 bootstrap 계층에 함수를 추가하세요. 그래야 Single 전용 no-op/stub과
 * 향후 다른 에디션 구현을 같은 호출 순서로 유지할 수 있습니다.
 */
typedef struct {
    const gchar *edition_name;
    gboolean cluster_enabled;
} PcvBootstrapEditionInfo;

const PcvBootstrapEditionInfo *pcv_bootstrap_get_edition_info(void);
const gchar *pcv_bootstrap_get_daemon_binary_path(void);

void pcv_bootstrap_init_cluster_manager(void);
void pcv_bootstrap_init_scheduler_proxy(void);
void pcv_bootstrap_init_federation(void);
void pcv_bootstrap_init_runtime_network(void);
void pcv_bootstrap_register_async_methods(GHashTable *async_methods);
void pcv_bootstrap_register_rpc_routes(GHashTable *rpc_routes);
void pcv_bootstrap_shutdown_cluster_stack(void);

#endif
