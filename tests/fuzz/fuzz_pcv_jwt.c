/**
 * @file fuzz_pcv_jwt.c
 * @brief libFuzzer harness for pcv_jwt_verify — JWT 파싱/검증 퍼징
 *
 * 대상: base64url 디코더 + JSON payload 파서 + HMAC 검증
 * 실측 위협: 만료된 토큰, 손상된 base64, malformed JSON, sub 클레임 누락
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "utils/pcv_jwt.h"

/* pcv_log 의존성 차단 — 퍼징 중 로깅 비활성화 (stub) */
void _pcv_log(int level, const char *module, const char *file, int line,
              const char *func, const char *fmt, ...) {
    (void)level; (void)module; (void)file; (void)line; (void)func; (void)fmt;
}

static int g_initialized = 0;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!g_initialized) {
        pcv_jwt_init("fuzz-test-secret-32-bytes-padding!");
        g_initialized = 1;
    }

    char *s = (char *)malloc(size + 1);
    if (!s) return 0;
    if (size) memcpy(s, data, size);
    s[size] = '\0';

    GError *err = NULL;
    gchar *sub = pcv_jwt_verify(s, &err);
    if (sub) g_free(sub);
    if (err) g_error_free(err);

    /* IP 바인딩 경로 */
    err = NULL;
    sub = pcv_jwt_verify_with_ip(s, "127.0.0.1", &err);
    if (sub) g_free(sub);
    if (err) g_error_free(err);

    free(s);
    return 0;
}
