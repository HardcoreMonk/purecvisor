   
                               
                                                                                
  
                                                       
                                                       
                                                    
                                                                       
                                                  
                         
                                                  
                                                       
  
                                                                 
                                                                       
                                                                  
                               
                                          
  
                           
                                                            
                                                                      
                                                           
                                                      
                                                   
  
                                                             
                                                
                                                           
                                                      
  
                 
                                              
                                               
                                                  
   

#include "rest_client_identity.h"

#include <gio/gio.h>
#include <libsoup/soup.h>
#include <string.h>

#include "utils/pcv_config.h"
#include "utils/pcv_crypto.h"
#include "utils/pcv_log.h"
#include "utils/pcv_secure.h"

#define IDENTITY_LOG_DOM "rest_identity"

#define REQUEST_IDENTITY_DATA_KEY "pcv-rest-client-identity"

struct _PcvRestClientIdentity {
    GObject parent_instance;
    gchar *client_ip;
    gboolean external_https;
    gboolean direct_local;
};

G_DEFINE_TYPE(PcvRestClientIdentity, pcv_rest_client_identity, G_TYPE_OBJECT)

                                                           
                                                               
                                                   
                               
static void
pcv_rest_client_identity_finalize(GObject *object)
{
    PcvRestClientIdentity *self = PCV_REST_CLIENT_IDENTITY(object);

    g_clear_pointer(&self->client_ip, g_free);
    G_OBJECT_CLASS(pcv_rest_client_identity_parent_class)->finalize(object);
}

                                                                
                                                   
static void
pcv_rest_client_identity_class_init(PcvRestClientIdentityClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = pcv_rest_client_identity_finalize;
}

                                                
                                                           
                                                
                                                       
static void
pcv_rest_client_identity_init(PcvRestClientIdentity *self)
{
    self->client_ip = g_strdup("unknown");
}

   
                                                                       
  
                                                          
                              
  
                                                  
                                                     
                      
  
                
                                                         
                                                                
                                                    
                                    
                                                             
                                             
                                                       
                                                                 
                                            
                                                          
                                                         
   
static gboolean
is_loopback_peer(const gchar *peer_ip)
{
    g_autoptr(GInetAddress) address = NULL;
    const guint8 *bytes;

    if (peer_ip == NULL) {
        return FALSE;
    }

                                                             
                                                    
    if (strchr(peer_ip, '%') != NULL) {
        return FALSE;
    }

    address = g_inet_address_new_from_string(peer_ip);
    if (address == NULL) {
        return FALSE;
    }

                                                       
                                                            
                                             
    bytes = g_inet_address_to_bytes(address);
    if (g_inet_address_get_family(address) == G_SOCKET_FAMILY_IPV4) {
        return bytes[0] == 127;
    }

    if (g_inet_address_get_family(address) == G_SOCKET_FAMILY_IPV6) {
        static const guint8 ipv6_loopback[16] = {
            0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 1
        };

        return memcmp(bytes, ipv6_loopback, sizeof(ipv6_loopback)) == 0;
    }

    return FALSE;
}

struct PcvRestProxyTrust {
    GPtrArray *masks;                                          
    gchar     *secret;                                           
};

                                                                 
PcvRestProxyTrust *
pcv_rest_proxy_trust_new(const gchar *trusted_proxies, const gchar *secret, GPtrArray **rejected_entries)
{
    PcvRestProxyTrust *trust = g_new0(PcvRestProxyTrust, 1);
    trust->masks = g_ptr_array_new_with_free_func(g_object_unref);
    trust->secret = (secret && *secret) ? g_strdup(secret) : NULL;
    if (rejected_entries)
        *rejected_entries = g_ptr_array_new_with_free_func(g_free);
    g_auto(GStrv) items = g_strsplit(trusted_proxies ? trusted_proxies : "", ",", -1);
    for (gchar **it = items; *it; it++) {
        g_strstrip(*it);
        if (**it == '\0')
            continue;
                                                                                 
                                                     
        GInetAddressMask *mask = strchr(*it, '%') ? NULL : g_inet_address_mask_new_from_string(*it, NULL);
        if (mask)
            g_ptr_array_add(trust->masks, mask);
        else if (rejected_entries)
            g_ptr_array_add(*rejected_entries, g_strdup(*it));
    }
    return trust;
}

             
void
pcv_rest_proxy_trust_free(PcvRestProxyTrust *trust)
{
    if (!trust)
        return;
    g_clear_pointer(&trust->masks, g_ptr_array_unref);
    pcv_secure_free_str(&trust->secret);
    g_free(trust);
}

                                                      
gboolean
pcv_rest_proxy_trusted(const PcvRestProxyTrust *trust, const gchar *peer_ip, const gchar *proxy_auth_header)
{
    if (!trust || !peer_ip || strchr(peer_ip, '%'))
        return FALSE;
    g_autoptr(GInetAddress) address = g_inet_address_new_from_string(peer_ip);
    if (!address)
        return FALSE;
    gboolean listed = FALSE;
    for (guint i = 0; i < trust->masks->len && !listed; i++)
        listed = g_inet_address_mask_matches(g_ptr_array_index(trust->masks, i), address);                      
    if (!listed)
        return FALSE;
    if (!trust->secret)
        return TRUE;
    return proxy_auth_header && pcv_secret_str_eq(proxy_auth_header, trust->secret);
}

static PcvRestProxyTrust *g_proxy_trust;                                  

                                              
static const PcvRestProxyTrust *
_active_trust(void)
{
    PcvRestProxyTrust *installed = g_atomic_pointer_get(&g_proxy_trust);
    if (installed)
        return installed;
    static gsize once = 0;
    static PcvRestProxyTrust *fallback;
    if (g_once_init_enter(&once)) {
        fallback = pcv_rest_proxy_trust_new("127.0.0.1", NULL, NULL);
        g_once_init_leave(&once, 1);
    }
    return fallback;
}

             
void
pcv_rest_proxy_trust_install(PcvRestProxyTrust *trust)
{
                                                                                                      
    PcvRestProxyTrust *old = g_atomic_pointer_get(&g_proxy_trust);
    g_atomic_pointer_set(&g_proxy_trust, trust);
    pcv_rest_proxy_trust_free(old);
}

                                                                         
void
pcv_rest_proxy_trust_load_config(void)
{
                                                                                          
                                             
    g_autofree gchar *raw_list = pcv_config_dup_raw_value("server", "trusted_proxies");
    gchar *secret = pcv_config_get_secret("server", "proxy_auth_secret", "");
                                                                                 
    gboolean secret_written = pcv_config_get_string("server", "proxy_auth_secret", NULL) != NULL;
    GPtrArray *rejected = NULL;
    PcvRestProxyTrust *trust = pcv_rest_proxy_trust_new(raw_list ? raw_list : "127.0.0.1", secret, &rejected);
    pcv_secure_free_str(&secret);
    for (guint i = 0; i < rejected->len; i++)
        PCV_LOG_WARN(IDENTITY_LOG_DOM, "[server] trusted_proxies 항목 무시(IP 또는 CIDR 아님, 예: 127.0.0.0/8): %s",
                     (const gchar *)g_ptr_array_index(rejected, i));
    g_ptr_array_unref(rejected);
    if (trust->masks->len == 0)
        PCV_LOG_WARN(IDENTITY_LOG_DOM, "[server] trusted_proxies가 비어 전달 헤더를 아무도 신뢰하지 않는다 — nginx 뒤 사용자는 127.0.0.1 하나로 보인다");
    else if (!trust->secret && secret_written)
        PCV_LOG_WARN(IDENTITY_LOG_DOM, "[server] proxy_auth_secret가 설정됐지만 복호화하지 못했다(ENC/ENC2 — machine-id 변경?) — 비밀 게이트 없이 목록만 신뢰한다(값을 다시 쓰거나 설치기 재실행)");
    else if (!trust->secret)
        PCV_LOG_WARN(IDENTITY_LOG_DOM, "[server] proxy_auth_secret 미설정 — 로컬 계정이 클라이언트 신원을 위조할 수 있다(설치기 재실행 또는 proxy_auth_secret 설정)");
    pcv_rest_proxy_trust_install(trust);
}

   
                                                  
  
                                                     
                                              
                
  
                                            
                                                       
                                               
                                          
                                            
                                            
   
static gchar *
canonical_ip(const gchar *value)
{
    g_autoptr(GInetAddress) address = NULL;

    if (value == NULL || *value == '\0') {
        return NULL;
    }

                                                               
                                                                     
                                                       
                                                                     
                                                      
    if (strchr(value, '%') != NULL) {
        return NULL;
    }

    address = g_inet_address_new_from_string(value);
    return address != NULL ? g_inet_address_to_string(address) : NULL;
}

   
                                                               
  
                                                     
                                               
                       
  
                                          
                                      
                                                    
   
static gchar *
canonical_trimmed_ip(const gchar *value)
{
    g_autofree gchar *trimmed = NULL;

    if (value == NULL) {
        return NULL;
    }

    trimmed = g_strdup(value);
    g_strstrip(trimmed);
                                                
                                              
    if (*trimmed == '\0' || strchr(trimmed, ',') != NULL) {
        return NULL;
    }

    return canonical_ip(trimmed);
}

   
                                                            
  
                                              
                                             
                                             
                                            
  
                                               
                                                   
                                     
  
                                                       
                                   
                                          
                                                    
                                                            
   
static gchar *
resolve_xff(const gchar *x_forwarded_for)
{
    g_auto(GStrv) entries = NULL;
    g_autofree gchar *rightmost = NULL;

    if (x_forwarded_for == NULL || *x_forwarded_for == '\0') {
        return NULL;
    }

                                                 
                                                 
                                              
    entries = g_strsplit(x_forwarded_for, ",", -1);
    for (gsize i = 0; entries[i] != NULL; i++) {
        g_autofree gchar *canonical = NULL;

        g_strstrip(entries[i]);
        if (*entries[i] == '\0') {
            return NULL;
        }

        canonical = canonical_ip(entries[i]);
        if (canonical == NULL) {
            return NULL;
        }

        g_free(rightmost);
        rightmost = g_steal_pointer(&canonical);
    }

    return g_steal_pointer(&rightmost);
}

   
                                                                 
  
                                                     
                                                                     
  
                                                
                                                 
                                                 
                                    
  
       
                                                  
                                                     
                                            
                                                 
                                                                          
                                                                          
                                                                           
                                                            
                                                              
                                                        
                                                   
                                                                  
                       
   
gchar *
pcv_rest_resolve_client_ip(gboolean trusted,
                           const gchar *peer_ip,
                           const gchar *x_real_ip,
                           const gchar *x_forwarded_for)
{
    g_autofree gchar *peer_canonical = canonical_ip(peer_ip);
    g_autofree gchar *forwarded = NULL;

                                                         
                                                
                                               
    if (!trusted) {
        return peer_canonical != NULL
            ? g_steal_pointer(&peer_canonical)
            : g_strdup(peer_ip);
    }

    forwarded = canonical_trimmed_ip(x_real_ip);
    if (forwarded != NULL) {
        return g_steal_pointer(&forwarded);
    }

    forwarded = resolve_xff(x_forwarded_for);
    if (forwarded != NULL) {
        return g_steal_pointer(&forwarded);
    }

    return peer_canonical != NULL
        ? g_steal_pointer(&peer_canonical)
        : g_strdup(peer_ip);
}

   
                                                             
  
                                                     
                                                                     
                                                    
                                    
  
                                                     
                                                   
                              
  
                                                         
                                 
                                                                         
                                                               
                                                      
   
gboolean
pcv_rest_resolve_external_https(gboolean trusted,
                                const gchar *x_forwarded_proto)
{
    g_autofree gchar *normalized = NULL;

                                                
                                                           
                                        
    if (!trusted || x_forwarded_proto == NULL) {
        return FALSE;
    }

    normalized = g_strdup(x_forwarded_proto);
    g_strstrip(normalized);
    if (*normalized == '\0' || strchr(normalized, ',') != NULL) {
        return FALSE;
    }

    return g_ascii_strcasecmp(normalized, "https") == 0;
}

                                                             
gboolean
pcv_rest_is_direct_local(const gchar *peer_ip, gboolean has_forwarding_header)
{
    return !has_forwarding_header && is_loopback_peer(peer_ip);
}

   
                                                       
  
                                                         
                                                                        
                                              
  
                                                        
                                                                 
                                                 
                                                       
                                               
                                          
                                                         
                                                    
   
static PcvRestClientIdentity *
identity_new_for_message(SoupServerMessage *message)
{
                                              
                                                    
                                               
    PcvRestClientIdentity *identity =
        g_object_new(PCV_TYPE_REST_CLIENT_IDENTITY, NULL);
    GSocketAddress *remote =
        soup_server_message_get_remote_address(message);
    g_autofree gchar *peer_ip = NULL;

    if (remote != NULL && G_IS_INET_SOCKET_ADDRESS(remote)) {
        GInetAddress *address =
            g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(remote));
        peer_ip = g_inet_address_to_string(address);
    }

    SoupMessageHeaders *headers = soup_server_message_get_request_headers(message);
    const gchar *real_ip = soup_message_headers_get_one(headers, "X-Real-IP");
    const gchar *forwarded_for = soup_message_headers_get_one(headers, "X-Forwarded-For");
    const gchar *forwarded_proto = soup_message_headers_get_one(headers, "X-Forwarded-Proto");
    const gchar *forwarded = soup_message_headers_get_one(headers, "Forwarded");
    const gchar *proxy_auth = soup_message_headers_get_one(headers, "X-PCV-Proxy-Auth");
                                                                              
    gboolean trusted = pcv_rest_proxy_trusted(_active_trust(), peer_ip, proxy_auth);
    g_autofree gchar *resolved = pcv_rest_resolve_client_ip(trusted, peer_ip, real_ip, forwarded_for);

    g_free(identity->client_ip);
    identity->client_ip = resolved != NULL ? g_steal_pointer(&resolved) : g_strdup("unknown");
    identity->external_https = pcv_rest_resolve_external_https(trusted, forwarded_proto);
                                                                       
    identity->direct_local = pcv_rest_is_direct_local(
        peer_ip, real_ip || forwarded_for || forwarded_proto || forwarded || proxy_auth);
    return identity;
}

   
                                                                
  
                                                          
                                            
                                                          
  
                                                       
                                                           
                                                  
                                                        
                                                         
                                       
                                                                   
                                 
   
const PcvRestClientIdentity *
pcv_rest_client_identity_get(SoupServerMessage *request)
{
    g_return_val_if_fail(SOUP_IS_SERVER_MESSAGE(request), NULL);

    PcvRestClientIdentity *identity =
        g_object_get_data(G_OBJECT(request), REQUEST_IDENTITY_DATA_KEY);
    if (identity != NULL)
        return identity;

                                              
                                                        
    identity = identity_new_for_message(request);
    g_object_set_data_full(G_OBJECT(request),
                           REQUEST_IDENTITY_DATA_KEY,
                           identity,
                           g_object_unref);
    return identity;
}

                                                
                                                            
                                         
const gchar *
pcv_rest_client_identity_get_client_ip(
    const PcvRestClientIdentity *identity)
{
    g_return_val_if_fail(
        PCV_IS_REST_CLIENT_IDENTITY((gpointer)identity), "unknown");

    return identity->client_ip;
}

                                                    
                                                     
gboolean
pcv_rest_client_identity_is_external_https(
    const PcvRestClientIdentity *identity)
{
    g_return_val_if_fail(
        PCV_IS_REST_CLIENT_IDENTITY((gpointer)identity), FALSE);

    return identity->external_https;
}

                                                                                
gboolean
pcv_rest_client_identity_is_direct_local(const PcvRestClientIdentity *identity)
{
    g_return_val_if_fail(PCV_IS_REST_CLIENT_IDENTITY((gpointer)identity), FALSE);
    return identity->direct_local;
}

   
                                                               
  
                                               
                                                     
                                                  
  
                                                 
                                                
                                    
  
                                                 
                                                  
                              
                                                          
                                                           
                     
                                                
                                       
                                                      
                                                        
                                                   
                                                 
   
gboolean
pcv_client_identity_admission_try(GHashTable *counts,
                                  const gchar *identity_key,
                                  gint limit)
{
    g_return_val_if_fail(counts != NULL, FALSE);
    g_return_val_if_fail(identity_key != NULL, FALSE);

    gint count =
        GPOINTER_TO_INT(g_hash_table_lookup(counts, identity_key));
    if (count >= limit)
        return FALSE;

    g_hash_table_insert(counts, g_strdup(identity_key),
                        GINT_TO_POINTER(count + 1));
    return TRUE;
}
