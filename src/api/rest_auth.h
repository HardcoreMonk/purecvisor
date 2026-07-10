#ifndef PCV_REST_AUTH_H
#define PCV_REST_AUTH_H

#include <glib.h>

/*
 * REST 로그인 부트스트랩 fallback 판정 API.
 *
 * [비전공자 설명]
 * 새 서버를 처음 설치하면 RBAC 사용자 DB가 아직 준비되지 않았을 수 있습니다.
 * 이때 daemon.conf에 적힌 초기 관리자 계정으로 한 번 진입할 수 있게 하는
 * 비상 출입문이 필요합니다. 단, 이 출입문이 평소 권한 실패까지 우회하면
 * 보안이 무너지므로 rest_auth.c에서 매우 좁은 조건만 허용합니다.
 *
 * [주니어 참고]
 * rest_server.c는 로그인 실패 사유를 이 함수에 넘기고, 이 함수가 TRUE를
 * 반환할 때만 fallback JWT 발급을 진행합니다. 새 인증 실패 사유를 추가할
 * 때는 "초기 설치 복구"와 "일반 로그인 우회"를 반드시 구분해야 합니다.
 */
gboolean pcv_rest_auth_should_fallback_bootstrap(const gchar *username,
                                                 const gchar *password,
                                                 const gchar *cfg_user,
                                                 const gchar *cfg_pass,
                                                 const GError *rbac_error);

#endif /* PCV_REST_AUTH_H */
