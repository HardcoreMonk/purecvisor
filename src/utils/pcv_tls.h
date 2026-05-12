
































#ifndef PURECVISOR_TLS_H
#define PURECVISOR_TLS_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS








typedef struct _PcvTlsCtx PcvTlsCtx;








PcvTlsCtx *pcv_tls_ctx_new(const gchar *cert, const gchar *key,
                             const gchar *ca, GError **error);


void        pcv_tls_ctx_free(PcvTlsCtx *ctx);


gboolean    pcv_tls_is_enabled(void);








JsonObject *pcv_tls_status(void);




const gchar *pcv_tls_get_cert_path(void);


const gchar *pcv_tls_get_key_path(void);








gboolean    pcv_tls_pki_init(const gchar *pki_dir, GError **error);








gint64      pcv_tls_get_cert_expiry_days(void);






void        pcv_tls_check_expiry_warning(void);









void        pcv_tls_init_from_config(void);

G_END_DECLS
#endif
