/**
 * @file pcv_crypto.c
 * @brief 상수시간 암호 비교 유틸 구현 — SEC-8 클래스
 */
#include "pcv_crypto.h"

#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <string.h>

/* SEC-8: 상수시간 비밀 문자열 비교. 양측 SHA-256 후 CRYPTO_memcmp로
 * 길이·내용 타이밍 무누출. NULL 인자 → FALSE. */
gboolean
pcv_secret_str_eq(const gchar *a, const gchar *b)
{
    if (!a || !b) return FALSE;
    unsigned char da[SHA256_DIGEST_LENGTH], db[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)a, strlen(a), da);
    SHA256((const unsigned char *)b, strlen(b), db);
    return CRYPTO_memcmp(da, db, SHA256_DIGEST_LENGTH) == 0;
}
