
#include "pcv_crypto.h"

#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <string.h>

gboolean
pcv_secret_str_eq(const gchar *a, const gchar *b)
{
    if (!a || !b) return FALSE;
    unsigned char da[SHA256_DIGEST_LENGTH], db[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)a, strlen(a), da);
    SHA256((const unsigned char *)b, strlen(b), db);
    return CRYPTO_memcmp(da, db, SHA256_DIGEST_LENGTH) == 0;
}
