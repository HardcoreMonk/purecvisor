/**
 * @file pcv_crypto.h
 * @brief 상수시간 암호 비교 유틸 — SEC-8 클래스 타이밍 공격 방어
 *
 * REST/gRPC 등 여러 인증 경로가 공유하는 중립 crypto 유틸이다.
 * (SEC-8에서 rest_auth.c에 만들었던 pcv_secret_str_eq를 여기로 이전 —
 *  gRPC가 REST auth 헤더를 include하는 레이어링 스멜을 없애기 위함.)
 *
 * [include 경로]
 *   src/utils/ 내부: #include "pcv_crypto.h"
 *   다른 디렉터리:   #include "utils/pcv_crypto.h"
 */
#ifndef PCV_CRYPTO_H
#define PCV_CRYPTO_H

#include <glib.h>

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

#endif /* PCV_CRYPTO_H */
