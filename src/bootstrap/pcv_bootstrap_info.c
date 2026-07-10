#include "pcv_bootstrap.h"

/*
 * 현재 빌드의 에디션 식별값.
 *
 * [왜 별도 파일인가?]
 * main.c, REST health 응답, UI 표시가 모두 같은 값을 보도록 단일 소스를 둡니다.
 * 문자열을 여러 곳에 직접 쓰면 한쪽만 "single", 다른 쪽은 "single_edge"처럼
 * 갈라져 운영자가 현재 서버 모드를 잘못 판단할 수 있습니다.
 */
static const PcvBootstrapEditionInfo g_bootstrap_info = {
    .edition_name = "single",
    .cluster_enabled = FALSE,
};

const PcvBootstrapEditionInfo *
pcv_bootstrap_get_edition_info(void)
{
    return &g_bootstrap_info;
}
