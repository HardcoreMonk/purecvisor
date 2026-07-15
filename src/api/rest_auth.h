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
 * fallback은 사용자가 RBAC DB에 부재(진짜 첫 설치)일 때만 허용한다. _ensure_admin_user가
 * 부팅 시 관리자를 시딩하므로, 비번 회전 후에는 사용자가 존재해 fallback이 발화하지 않는다
 * (옛 daemon.conf 비번 거부). user_in_db 는 호출부(rest_server)가 pcv_rbac_user_exists 로
 * 판정해 전달한다(UNKNOWN=존재 취급, fail-secure).
 */
gboolean pcv_rest_auth_should_fallback_bootstrap(const gchar *username,
                                                 const gchar *password,
                                                 const gchar *cfg_user,
                                                 const gchar *cfg_pass,
                                                 gboolean user_in_db);

/*
 * SEC-8: 상수시간 비밀 문자열 비교.
 *
 * [비전공자 설명]
 * g_strcmp0 같은 일반 문자열 비교는 앞에서부터 다른 글자를 만나는 즉시
 * 멈추기 때문에, 정답과 몇 글자가 일치하는지에 따라 걸리는 시간이 미세하게
 * 달라집니다. 공격자는 이 시간차를 반복 측정해 비밀번호를 한 글자씩 추측할
 * 수 있습니다(타이밍 공격). 이 함수는 양쪽 문자열을 먼저 SHA-256으로
 * 고정 길이 해시로 만든 뒤, 항상 전체를 끝까지 비교하는 CRYPTO_memcmp로
 * 다이제스트를 대조해 그런 시간차를 없앱니다.
 *
 * NULL 인자(양쪽 또는 한쪽) → FALSE.
 */
gboolean pcv_secret_str_eq(const gchar *a, const gchar *b);

#endif /* PCV_REST_AUTH_H */
