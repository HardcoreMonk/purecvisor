   
                           
                                                                                                                    
  
                           
                                                                                                  
                                                                                                                     
                                                                                                                  
                                                               
  
                                      
   

#include "api/drain.h"
#include "api/rest_shutdown.h"
#include "api/rest_server.h"
#include "api/rest_middleware.h"
#include "api/rest_auth.h"
#include "api/rest_client_identity.h"
#include "utils/pcv_crypto.h"
#include "utils/pcv_jwt.h"
#include "utils/pcv_totp.h"
#include "utils/pcv_log.h"
#include "modules/daemons/prometheus_exporter.h"
#include "api/ws_server.h"
#include "utils/pcv_config.h"
#include "utils/pcv_tls.h"
#include "utils/pcv_bpf.h"
#include "utils/pcv_secure.h"
#include "modules/dispatcher/rpc_utils.h"
#if PCV_CLUSTER_ENABLED
#include "../modules/cluster/cluster_manager.h"
#endif
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <libvirt/libvirt.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>
#include <string.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <zlib.h>
#include "modules/network/ovn_manager.h"
#include "modules/network/dpdk_manager.h"
#include "utils/pcv_spawn.h"
#include "modules/auth/pcv_rbac.h"
#include "modules/audit/pcv_audit.h"
#include "modules/virt/circuit_breaker.h"
#include "purecvisor/version.h"
#include "api/rest_internal.h"
#include "api/rest_response.h"
#include "api/rest_request.h"
#include "api/rest_async.h"
#include "api/rest_transport.h"
#include "api/rest_request_auth.h"

                                                                                                                                                                                                                                  
                                         
PcvRestStep
pcv_rest_map_observe(PcvRestRequest *rq)
{
    SoupServerMessage *msg = rq->msg;
    GHashTable *query = rq->query;
    const gchar *method = rq->method;
    JsonObject *body = rq->body;
    const gchar *resource = rq->resource;
    const gchar *name = rq->name;
    const gchar *action = rq->action;
                                                                
    if (g_strcmp0(resource, "monitor") == 0) {
        if (g_strcmp0(method, "GET") != 0) {
            rq->rpc = NULL;
        } else if (g_strcmp0(name, "metrics") == 0)
            rq->rpc = pcv_rest_build_rpc("monitor.metrics", NULL);
        else if (g_strcmp0(name, "fleet") == 0)
            rq->rpc = pcv_rest_build_rpc("monitor.fleet", NULL);
        
    }

                                                               
    else if (g_strcmp0(resource, "alerts") == 0) {
        if (g_strcmp0(name, "config") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rq->rpc = pcv_rest_build_rpc("alert.config.get", NULL);
            else if (g_strcmp0(method, "PUT") == 0)
                rq->rpc = pcv_rest_build_rpc("alert.config.set", body);
        } else if (g_strcmp0(name, "actions") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rq->rpc = pcv_rest_build_rpc("alert.action.list", NULL);
        } else if (g_strcmp0(name, "silence") == 0) {
                                                              
            if (g_strcmp0(method, "POST") == 0)
                rq->rpc = pcv_rest_build_rpc("alert.silence", body);
        } else if (g_strcmp0(name, "silences") == 0) {
                                                        
                                                                     
            if (g_strcmp0(method, "GET") == 0)
                rq->rpc = pcv_rest_build_rpc("alert.silence.list", NULL);
        } else if (g_strcmp0(name, "dlq") == 0) {
                                                                             
                                                                       
            if (g_strcmp0(method, "GET") == 0)
                rq->rpc = pcv_rest_build_rpc("alert.dlq.list", NULL);
        }  else if (*name != '\0' && g_strcmp0(action, "ack") == 0
                   && g_strcmp0(method, "POST") == 0) {
                                                          
                                                                   
                                                         
                                                                  
                                        
            JsonObject *p = json_object_new();
            json_object_set_int_member(p, "alert_id", g_ascii_strtoll(name, NULL, 10));
            rq->rpc = pcv_rest_build_rpc("alert.ack", p);
            json_object_unref(p);
        } else {
            if (g_strcmp0(method, "GET") == 0)
                rq->rpc = pcv_rest_build_rpc("alert.history", NULL);
        }
    }

                                                          
                                                                   
                                                                    
                                                       
                                                                
                                                       
    else if (g_strcmp0(resource, "suricata") == 0) {
                                                
                                                       
        if (g_strcmp0(name, "status") == 0 && *action == '\0' &&
            g_strcmp0(method, "GET") == 0) {
            rq->rpc = pcv_rest_build_rpc("suricata.status", NULL);
        } else if (g_strcmp0(name, "policy") == 0 && *action == '\0') {
            if (g_strcmp0(method, "GET") == 0)
                rq->rpc = pcv_rest_build_rpc("suricata.policy.get", NULL);
            else if (g_strcmp0(method, "PUT") == 0)
                rq->rpc = pcv_rest_build_rpc("suricata.policy.set", body);
        } else if (g_strcmp0(name, "rules") == 0 &&
                   g_strcmp0(action, "update") == 0 &&
                   g_strcmp0(method, "POST") == 0) {
            rq->rpc = pcv_rest_build_rpc("suricata.rules.update", body);
        } else if (g_strcmp0(name, "ips") == 0) {
            if (g_strcmp0(action, "status") == 0 &&
                g_strcmp0(method, "GET") == 0) {
                rq->rpc = pcv_rest_build_rpc("suricata.ips.status", NULL);
            } else if (g_strcmp0(action, "enable") == 0 &&
                       g_strcmp0(method, "POST") == 0) {
                rq->rpc = pcv_rest_build_rpc("suricata.ips.enable", body);
            } else if (g_strcmp0(action, "disable") == 0 &&
                       g_strcmp0(method, "POST") == 0) {
                rq->rpc = pcv_rest_build_rpc("suricata.ips.disable", body);
            } else if (g_strcmp0(action, "drop") == 0) {
                if (g_strcmp0(method, "GET") == 0)
                    rq->rpc = pcv_rest_build_rpc("suricata.ips.drop.list", NULL);
                else if (g_strcmp0(method, "POST") == 0)
                    rq->rpc = pcv_rest_build_rpc("suricata.ips.drop.add", body);
                else if (g_strcmp0(method, "DELETE") == 0)
                    rq->rpc = pcv_rest_build_rpc("suricata.ips.drop.remove", body);
            }
        }
    }

    else if (g_strcmp0(resource, "push") == 0) {
        if (g_strcmp0(name, "vapid") == 0) {
                                                                   
            if (*action == '\0' && g_strcmp0(method, "GET") == 0)
                rq->rpc = pcv_rest_build_rpc("push.vapid.get", NULL);
                                                                          
            else if (g_strcmp0(action, "rotate") == 0 && g_strcmp0(method, "POST") == 0)
                rq->rpc = pcv_rest_build_rpc("push.vapid.rotate", NULL);
        } else if (g_strcmp0(name, "subscribe") == 0 && g_strcmp0(method, "POST") == 0) {
                                                                                    
            rq->rpc = pcv_rest_build_rpc("push.subscribe", body);
        } else if (g_strcmp0(name, "unsubscribe") == 0 && g_strcmp0(method, "POST") == 0) {
            rq->rpc = pcv_rest_build_rpc("push.unsubscribe", body);
        } else if (g_strcmp0(name, "test") == 0 && g_strcmp0(method, "POST") == 0) {
            rq->rpc = pcv_rest_build_rpc("push.test", body);
        } else if (g_strcmp0(name, "subscriptions") == 0 && g_strcmp0(method, "GET") == 0) {
            rq->rpc = pcv_rest_build_rpc("push.list", NULL);
        } else if (g_strcmp0(name, "mine") == 0 && g_strcmp0(method, "GET") == 0) {
                                                                   
                                                            
                                                            
            rq->rpc = pcv_rest_build_rpc("push.mine", NULL);
        }
    }

                                                              
    else if (g_strcmp0(resource, "prometheus") == 0) {
        if (g_strcmp0(name, "sd") == 0 && g_strcmp0(method, "GET") == 0)
            rq->rpc = pcv_rest_build_rpc("prometheus.sd", NULL);
    }

                                                              
                                                                 
    else if (g_strcmp0(resource, "health") == 0) {
        if (g_strcmp0(name, "deep") == 0 && g_strcmp0(method, "GET") == 0)
            rq->rpc = pcv_rest_build_rpc("health.deep", NULL);
    }

                                                               
    else if (g_strcmp0(resource, "pool") == 0) {
        if (g_strcmp0(name, "conninfo") == 0 && g_strcmp0(method, "GET") == 0)
            rq->rpc = pcv_rest_build_rpc("pool.conninfo", NULL);
    }

                                                               
    else if (g_strcmp0(resource, "db") == 0) {
        if (g_strcmp0(name, "migration") == 0 && g_strcmp0(method, "GET") == 0)
            rq->rpc = pcv_rest_build_rpc("db.migration.status", NULL);
    }

                                                               
    else if (g_strcmp0(resource, "audit") == 0) {
        if (g_strcmp0(name, "search") == 0 && g_strcmp0(method, "GET") == 0) {
            JsonObject *p = pcv_rest_audit_search_params(msg, query, body);
            rq->rpc = pcv_rest_build_rpc("audit.search", p);
            json_object_unref(p);
        }
    }

                                                               
    else if (g_strcmp0(resource, "jobs") == 0) {
        if (*name == '\0' && g_strcmp0(method, "GET") == 0) {
            JsonObject *pg = nullptr;
            if (query) {
                const gchar *q_off = g_hash_table_lookup(query, "offset");
                const gchar *q_lim = g_hash_table_lookup(query, "limit");
                if (q_off || q_lim) {
                    pg = json_object_new();
                    if (q_off) json_object_set_int_member(pg, "offset", g_ascii_strtoll(q_off, NULL, 10));
                    if (q_lim) json_object_set_int_member(pg, "limit", g_ascii_strtoll(q_lim, NULL, 10));
                }
            }
            rq->rpc = pcv_rest_build_rpc("jobs.list", pg);
            if (pg) json_object_unref(pg);
        } else if (*name != '\0' && g_strcmp0(action, "cancel") == 0
                   && g_strcmp0(method, "POST") == 0) {
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "job_id", name);
            rq->rpc = pcv_rest_build_rpc("jobs.cancel", p);
            json_object_unref(p);
        } else if (g_strcmp0(name, "persistent") == 0 && *action == '\0'
                   && g_strcmp0(method, "GET") == 0) {
                                                                
                                                     
        } else if (*name != '\0' && *action == '\0'
                   && g_strcmp0(method, "GET") == 0) {
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "job_id", name);
            rq->rpc = pcv_rest_build_rpc("jobs.get", p);
            json_object_unref(p);
        }
    }

                                                               
    else if (g_strcmp0(resource, "webhook") == 0) {
        if (g_strcmp0(name, "dlq") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rq->rpc = pcv_rest_build_rpc("webhook.dlq.list", NULL);
            else if (g_strcmp0(method, "POST") == 0)
                rq->rpc = pcv_rest_build_rpc("webhook.dlq.retry", NULL);
        }
    }

                                                               
    else if (g_strcmp0(resource, "processes") == 0) {
        if (g_strcmp0(method, "GET") == 0)
            rq->rpc = pcv_rest_build_rpc("monitor.processes", NULL);
    }

                                                           
    else if (g_strcmp0(resource, "agent") == 0) {
        if (g_strcmp0(name, "config") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rq->rpc = pcv_rest_build_rpc("agent.config.get", NULL);
            else if (g_strcmp0(method, "PUT") == 0)
                rq->rpc = pcv_rest_build_rpc("agent.config.set", body);
        } else if (g_strcmp0(name, "history") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rq->rpc = pcv_rest_build_rpc("agent.history", NULL);
        } else {
                                                    
            if (g_strcmp0(method, "GET") == 0)
                rq->rpc = pcv_rest_build_rpc("agent.config.get", NULL);
        }
    }
    else
        return PCV_REST_STEP_UNMATCHED;
    return PCV_REST_STEP_NEXT;
}
