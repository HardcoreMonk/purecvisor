   
                               
                                                  
  
                                                       
                                                       
                                                    
                                                     
                                                   
                                                  
                                                       
  
                                                                 
                                                       
                                              
                                          
  
                           
                                                            
                                                                      
                                                           
                                                      
                                                   
  
                                                             
                                                
                                                           
                                                      
  
                 
                                              
                                               
                                                  
   

#include "rest_client_identity.h"

#include <gio/gio.h>
#include <libsoup/soup.h>
#include <string.h>

#define REQUEST_IDENTITY_DATA_KEY "pcv-rest-client-identity"

struct _PcvRestClientIdentity {
    GObject parent_instance;
    gchar *client_ip;
    gboolean external_https;
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
is_trusted_loopback(const gchar *peer_ip)
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
pcv_rest_resolve_client_ip(const gchar *peer_ip,
                           const gchar *x_real_ip,
                           const gchar *x_forwarded_for)
{
    g_autofree gchar *peer_canonical = canonical_ip(peer_ip);
    g_autofree gchar *forwarded = NULL;

                                                         
                                                
                                               
    if (!is_trusted_loopback(peer_ip)) {
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
pcv_rest_resolve_external_https(const gchar *peer_ip,
                                const gchar *x_forwarded_proto)
{
    g_autofree gchar *normalized = NULL;

                                                
                                                           
                                        
    if (!is_trusted_loopback(peer_ip) || x_forwarded_proto == NULL) {
        return FALSE;
    }

    normalized = g_strdup(x_forwarded_proto);
    g_strstrip(normalized);
    if (*normalized == '\0' || strchr(normalized, ',') != NULL) {
        return FALSE;
    }

    return g_ascii_strcasecmp(normalized, "https") == 0;
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

    SoupMessageHeaders *headers =
        soup_server_message_get_request_headers(message);
    const gchar *real_ip =
        soup_message_headers_get_one(headers, "X-Real-IP");
    const gchar *forwarded_for =
        soup_message_headers_get_one(headers, "X-Forwarded-For");
    const gchar *forwarded_proto =
        soup_message_headers_get_one(headers, "X-Forwarded-Proto");
    g_autofree gchar *resolved =
        pcv_rest_resolve_client_ip(peer_ip, real_ip, forwarded_for);

    g_free(identity->client_ip);
    identity->client_ip =
        resolved != NULL ? g_steal_pointer(&resolved) : g_strdup("unknown");
    identity->external_https =
        pcv_rest_resolve_external_https(peer_ip, forwarded_proto);
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
