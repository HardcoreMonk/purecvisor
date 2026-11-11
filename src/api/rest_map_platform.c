   
                            
                                                                                                                             
  
                           
                                                                                                  
                                                                                                                     
                                                                                                                  
                                                               
  
                                                           
   

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
pcv_rest_map_platform(PcvRestRequest *rq)
{
    SoupServerMessage *msg = rq->msg;
    GHashTable *query = rq->query;
    const gchar *method = rq->method;
    JsonObject *body = rq->body;
    gchar *subject = rq->subject;
    gint key_role = rq->key_role;
    const gchar *resource = rq->resource;
    const gchar *name = rq->name;
    const gchar *action = rq->action;
    const gchar *sub = rq->sub;
                                                              
                                                 
                                                         
                                                 
                                                   
                                                   
                                                                  
                                                             
    if (g_strcmp0(resource, "networks") == 0) {
        if (*name == '\0') {
            if (g_strcmp0(method, "GET") == 0)
                rq->rpc = pcv_rest_build_rpc("network.list", NULL);
            else if (g_strcmp0(method, "POST") == 0)
                rq->rpc = pcv_rest_build_rpc("network.create", body);
        } else if (g_strcmp0(name, "host-baseline") == 0 && *action == '\0' &&
                   g_strcmp0(method, "GET") == 0) {
                                                                 
                                                                         
            rq->rpc = pcv_rest_build_rpc("network.host.info", NULL);
        } else if (*name != '\0') {
                                                                      
                                                                
                                                          
                                                                   
            if (g_strcmp0(action, "mode") == 0 && g_strcmp0(method, "POST") == 0) {
                                   
                JsonObject *p = body ? json_object_ref(body) : json_object_new();
                json_object_set_string_member(p, "name", name);
                rq->rpc = pcv_rest_build_rpc("network.mode_set", p);
                json_object_unref(p);

                                                              
            } else if (g_strcmp0(action, "qos") == 0) {
                if (g_strcmp0(method, "PUT") == 0) {
                    JsonObject *p = body ? json_object_ref(body) : json_object_new();
                    json_object_set_string_member(p, "interface", name);
                    rq->rpc = pcv_rest_build_rpc("network.qos.set", p);
                    json_object_unref(p);
                } else if (g_strcmp0(method, "GET") == 0) {
                    JsonObject *p = json_object_new();
                    json_object_set_string_member(p, "interface", name);
                    rq->rpc = pcv_rest_build_rpc("network.qos.get", p);
                    json_object_unref(p);
                } else if (g_strcmp0(method, "DELETE") == 0) {
                    JsonObject *p = json_object_new();
                    json_object_set_string_member(p, "interface", name);
                    rq->rpc = pcv_rest_build_rpc("network.qos.remove", p);
                    json_object_unref(p);
                }

            } else if (*action == '\0' &&
                       g_strcmp0(method, "GET") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "bridge_name", name);
                rq->rpc = pcv_rest_build_rpc("network.info", p);
                json_object_unref(p);
            } else if (*action == '\0' &&
                       g_strcmp0(method, "DELETE") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "bridge_name", name);
                rq->rpc = pcv_rest_build_rpc("network.delete", p);
                json_object_unref(p);
            }
        }
    }

                                                               
                                                                 
                                                                    
                                                           
      
                       
                                
                              
                                
                                 
                                       
                                        
                                                     
                                   
                                          
                                       
                                                                 
    else if (g_strcmp0(resource, "vpcs") == 0) {
        if (*name == '\0') {
            if (g_strcmp0(method, "GET") == 0) {
                JsonObject *p = pcv_rest_vpc_params(NULL, query, NULL, NULL);
                rq->rpc = pcv_rest_build_rpc("vpc.list", p);
                json_object_unref(p);
            } else if (g_strcmp0(method, "POST") == 0) {
                rq->rpc = pcv_rest_build_rpc("vpc.create", body);
            }
        } else if (g_strcmp0(name, "status") == 0 && *action == '\0' &&
                   g_strcmp0(method, "GET") == 0) {
            JsonObject *p = pcv_rest_vpc_params(NULL, query, NULL, NULL);
            rq->rpc = pcv_rest_build_rpc("vpc.status", p);
            json_object_unref(p);
        } else if (g_strcmp0(name, "backends") == 0 && *action == '\0' &&
                   g_strcmp0(method, "GET") == 0) {
            JsonObject *p = pcv_rest_vpc_params(NULL, query, NULL, NULL);
            rq->rpc = pcv_rest_build_rpc("vpc.backend.list", p);
            json_object_unref(p);
        } else if (g_strcmp0(name, "reconcile") == 0 && *action == '\0' &&
                   g_strcmp0(method, "POST") == 0) {
            rq->rpc = pcv_rest_build_rpc("vpc.reconcile", body);
        } else if (*name != '\0' && *action == '\0') {
            JsonObject *p = pcv_rest_vpc_params(body, query, "vpc_id", name);
            if (g_strcmp0(method, "GET") == 0)
                rq->rpc = pcv_rest_build_rpc("vpc.get", p);
            else if (g_strcmp0(method, "DELETE") == 0)
                rq->rpc = pcv_rest_build_rpc("vpc.delete", p);
            json_object_unref(p);
        } else if (g_strcmp0(action, "egress") == 0 &&
                   g_strcmp0(method, "POST") == 0) {
            JsonObject *p = pcv_rest_vpc_params(body, query, "vpc_id", name);
            rq->rpc = pcv_rest_build_rpc("vpc.egress.set", p);
            json_object_unref(p);
        } else if (g_strcmp0(action, "subnets") == 0) {
            JsonObject *p = pcv_rest_vpc_params(body, query, "vpc_id", name);
            if (g_strcmp0(method, "GET") == 0)
                rq->rpc = pcv_rest_build_rpc("vpc.subnet.list", p);
            else if (g_strcmp0(method, "POST") == 0)
                rq->rpc = pcv_rest_build_rpc("vpc.subnet.create", p);
            json_object_unref(p);
        } else if (g_strcmp0(action, "attachments") == 0 &&
                   g_strcmp0(method, "GET") == 0) {
            JsonObject *p = pcv_rest_vpc_params(NULL, query, "vpc_id", name);
            rq->rpc = pcv_rest_build_rpc("vpc.attachment.list", p);
            json_object_unref(p);
        } else if (g_strcmp0(action, "services") == 0 &&
                   g_strcmp0(method, "GET") == 0) {
            JsonObject *p = pcv_rest_vpc_params(NULL, query, "vpc_id", name);
            rq->rpc = pcv_rest_build_rpc("vpc.service.list", p);
            json_object_unref(p);
        }
    }
    else if (g_strcmp0(resource, "vpc-subnets") == 0) {
        if (*name != '\0' &&
            *action == '\0' &&
            g_strcmp0(method, "DELETE") == 0) {
            JsonObject *p = pcv_rest_vpc_params(body, query, "subnet_id", name);
            rq->rpc = pcv_rest_build_rpc("vpc.subnet.delete", p);
            json_object_unref(p);
        }
    }
    else if (g_strcmp0(resource, "vpc-attachments") == 0) {
        if (*name == '\0' &&
            g_strcmp0(method, "POST") == 0) {
            rq->rpc = pcv_rest_build_rpc("vpc.attachment.create", body);
        } else if (*name != '\0' &&
                   *action == '\0' &&
                   g_strcmp0(method, "DELETE") == 0) {
            JsonObject *p = pcv_rest_vpc_params(body, query, "attachment_id", name);
            rq->rpc = pcv_rest_build_rpc("vpc.attachment.delete", p);
            json_object_unref(p);
        }
    }
    else if (g_strcmp0(resource, "vpc-services") == 0) {
        if (*name == '\0' &&
            g_strcmp0(method, "POST") == 0) {
            rq->rpc = pcv_rest_build_rpc("vpc.service.publish", body);
        } else if (*name != '\0' &&
                   *action == '\0' &&
                   g_strcmp0(method, "DELETE") == 0) {
            JsonObject *p = pcv_rest_vpc_params(body, query, "publish_id", name);
            rq->rpc = pcv_rest_build_rpc("vpc.service.unpublish", p);
            json_object_unref(p);
        }
    }

                                                                
    else if (g_strcmp0(resource, "storage") == 0) {
                                                    
        if (g_strcmp0(name, "pools") == 0) {
            if (*action == '\0') {
                if (g_strcmp0(method, "GET") == 0)
                    rq->rpc = pcv_rest_build_rpc("storage.pool.list", NULL);
                                                                    
                else if (g_strcmp0(method, "POST") == 0)
                    rq->rpc = pcv_rest_build_rpc("storage.pool.create", body);
                                                                                      
                else if (g_strcmp0(method, "DELETE") == 0)
                    rq->rpc = pcv_rest_build_rpc("storage.pool.destroy", body);
            }
                                                                     
            else if (g_strcmp0(action, "scrub") == 0 && g_strcmp0(method, "POST") == 0)
                rq->rpc = pcv_rest_build_rpc("storage.pool.scrub", body);
        }
        else if (g_strcmp0(name, "zvols") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rq->rpc = pcv_rest_build_rpc("storage.zvol.list", body);
            else if (g_strcmp0(method, "POST") == 0)
                rq->rpc = pcv_rest_build_rpc("storage.zvol.create", body);
            else if (g_strcmp0(method, "DELETE") == 0)
                rq->rpc = pcv_rest_build_rpc("storage.zvol.delete", body);
        }
    }

                                                              
                                                  
                                                
                                                  
                                                
                                                         
                                                         
                                                         
                                                             
    else if (g_strcmp0(resource, "dpdk") == 0) {
        if (g_strcmp0(name, "status") == 0)
            rq->rpc = pcv_rest_build_rpc("dpdk.status", NULL);
        else if (g_strcmp0(name, "bind") == 0 && g_strcmp0(method, "POST") == 0)
            rq->rpc = pcv_rest_build_rpc("dpdk.bind", body);
        else if (g_strcmp0(name, "unbind") == 0 && g_strcmp0(method, "POST") == 0)
            rq->rpc = pcv_rest_build_rpc("dpdk.unbind", body);
        else if (g_strcmp0(name, "list") == 0)
            rq->rpc = pcv_rest_build_rpc("dpdk.list", NULL);
        else if (g_strcmp0(name, "hugepage") == 0)
            rq->rpc = pcv_rest_build_rpc("dpdk.hugepage.info", NULL);
        else if (g_strcmp0(name, "bridge") == 0) {
            if (g_strcmp0(action, "create") == 0 && g_strcmp0(method, "POST") == 0)
                rq->rpc = pcv_rest_build_rpc("dpdk.bridge.create", body);
            else if (g_strcmp0(action, "delete") == 0 && g_strcmp0(method, "POST") == 0)
                rq->rpc = pcv_rest_build_rpc("dpdk.bridge.delete", body);
        }
    }

                                                               
                                                   
                                                   
                                                    
                                                 
                                                
                                                   
                                                   
                                                             
    else if (g_strcmp0(resource, "sriov") == 0) {
        if (g_strcmp0(name, "status") == 0)
            rq->rpc = pcv_rest_build_rpc("sriov.status", NULL);
        else if (g_strcmp0(name, "enable") == 0 && g_strcmp0(method, "POST") == 0)
            rq->rpc = pcv_rest_build_rpc("sriov.enable", body);
        else if (g_strcmp0(name, "disable") == 0 && g_strcmp0(method, "POST") == 0)
            rq->rpc = pcv_rest_build_rpc("sriov.disable", body);
        else if (g_strcmp0(name, "list") == 0)
            rq->rpc = pcv_rest_build_rpc("sriov.list", body);
        else if (g_strcmp0(name, "set") == 0 && g_strcmp0(method, "POST") == 0)
            rq->rpc = pcv_rest_build_rpc("sriov.set", body);
        else if (g_strcmp0(name, "attach") == 0 && g_strcmp0(method, "POST") == 0)
            rq->rpc = pcv_rest_build_rpc("sriov.attach", body);
        else if (g_strcmp0(name, "detach") == 0 && g_strcmp0(method, "POST") == 0)
            rq->rpc = pcv_rest_build_rpc("sriov.detach", body);
    }

                                                           
    else if (g_strcmp0(resource, "backup") == 0) {
        if (g_strcmp0(name, "policies") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rq->rpc = pcv_rest_build_rpc("backup.policy.list", NULL);
            else if (g_strcmp0(method, "POST") == 0)
                rq->rpc = pcv_rest_build_rpc("backup.policy.set", body);
            else if (g_strcmp0(method, "DELETE") == 0)
                rq->rpc = pcv_rest_build_rpc("backup.policy.delete", body);
        } else if (g_strcmp0(name, "history") == 0) {
            rq->rpc = pcv_rest_build_rpc("backup.history", body);

                                                               
                                                         
                                                    
                                                       
                                                                  
        } else if (g_strcmp0(name, "incremental") == 0 && g_strcmp0(method, "POST") == 0) {
            rq->rpc = pcv_rest_build_rpc("backup.incremental", body);
        } else if (g_strcmp0(name, "verify") == 0 && g_strcmp0(method, "POST") == 0) {
            rq->rpc = pcv_rest_build_rpc("backup.verify", body);
        } else if (g_strcmp0(name, "replicate") == 0 && g_strcmp0(method, "POST") == 0) {
            rq->rpc = pcv_rest_build_rpc("backup.replicate", body);
        }
    }

                                                           
    else if (g_strcmp0(resource, "ovn") == 0) {
        if (g_strcmp0(name, "status") == 0)
            rq->rpc = pcv_rest_build_rpc("ovn.status", NULL);
        else if (g_strcmp0(name, "switches") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rq->rpc = pcv_rest_build_rpc("ovn.switch.list", NULL);
            else if (g_strcmp0(method, "POST") == 0)
                rq->rpc = pcv_rest_build_rpc("ovn.switch.create", body);
        } else if (g_strcmp0(name, "routers") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rq->rpc = pcv_rest_build_rpc("ovn.router.list", NULL);
            else if (g_strcmp0(method, "POST") == 0)
                rq->rpc = pcv_rest_build_rpc("ovn.router.create", body);
        } else if (g_strcmp0(name, "acl") == 0) {
            if (g_strcmp0(method, "GET") == 0) {
                                                                                
                JsonObject *p = json_object_new();
                const gchar *sw = query ? g_hash_table_lookup(query, "switch") : NULL;
                if (sw && *sw)
                    json_object_set_string_member(p, "switch", sw);
                rq->rpc = pcv_rest_build_rpc("ovn.acl.list", p);
                json_object_unref(p);
            } else if (g_strcmp0(method, "POST") == 0)
                rq->rpc = pcv_rest_build_rpc("ovn.acl.add", body);
        } else if (g_strcmp0(name, "nat") == 0) {
            if (g_strcmp0(method, "GET") == 0) {
                                                                            
                JsonObject *p = json_object_new();
                const gchar *router = query ? g_hash_table_lookup(query, "router") : NULL;
                if (router && *router)
                    json_object_set_string_member(p, "router", router);
                rq->rpc = pcv_rest_build_rpc("ovn.nat.list", p);
                json_object_unref(p);
            } else if (g_strcmp0(method, "POST") == 0)
                rq->rpc = pcv_rest_build_rpc("ovn.nat.add", body);
        }
    }

                                                        
    else if (g_strcmp0(resource, "iso") == 0 && g_strcmp0(method, "GET") == 0) {
        rq->rpc = pcv_rest_build_rpc("iso.list", NULL);
    }

                                                         
    else if (g_strcmp0(resource, "iscsi") == 0) {
        if (g_strcmp0(name, "targets") == 0 && g_strcmp0(method, "GET") == 0)
            rq->rpc = pcv_rest_build_rpc("iscsi.target.list", NULL);
        else if (g_strcmp0(name, "targets") == 0 && g_strcmp0(method, "POST") == 0)
            rq->rpc = pcv_rest_build_rpc("iscsi.target.create", body);
        else if (g_strcmp0(name, "targets") == 0 && g_strcmp0(method, "DELETE") == 0)
            rq->rpc = pcv_rest_build_rpc("iscsi.target.delete", body);
        else if (g_strcmp0(name, "connect") == 0 && g_strcmp0(method, "POST") == 0)
            rq->rpc = pcv_rest_build_rpc("iscsi.connect", body);
        else if (g_strcmp0(name, "disconnect") == 0 && g_strcmp0(method, "POST") == 0)
            rq->rpc = pcv_rest_build_rpc("iscsi.disconnect", body);
    }

                                                     
    else if (g_strcmp0(resource, "overlay") == 0 && g_strcmp0(method, "GET") == 0) {
        rq->rpc = pcv_rest_build_rpc("overlay.list", NULL);
    }

                                                        
    else if (g_strcmp0(resource, "auth") == 0) {
        if (g_strcmp0(name, "whoami") == 0 && g_strcmp0(method, "GET") == 0) {
                                                                         
                                                                       
            PcvRole role = (key_role >= 0)
                ? (PcvRole)key_role
                : (PcvRole)rq->subject_role;                                                   
            const gchar *role_str = pcv_rbac_role_to_str(role);
            const gchar *tenant = pcv_rbac_get_tenant(subject);
            gchar *resp = g_strdup_printf(
                "{\"data\":{\"username\":\"%s\",\"role\":\"%s\",\"tenant\":\"%s\"}}",
                subject, role_str, tenant ? tenant : "---");
            pcv_rest_send_json(msg, 200, resp);
            g_free(resp);
            return PCV_REST_STEP_CLEANUP;
        } else if (g_strcmp0(name, "users") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rq->rpc = pcv_rest_build_rpc("auth.user.list", NULL);
            else if (g_strcmp0(method, "POST") == 0)
                rq->rpc = pcv_rest_build_rpc("auth.user.create", body);
            else if (g_strcmp0(method, "DELETE") == 0)
                rq->rpc = pcv_rest_build_rpc("auth.user.delete", body);
        } else if (g_strcmp0(name, "role") == 0 && g_strcmp0(method, "POST") == 0) {
            rq->rpc = pcv_rest_build_rpc("auth.role.set", body);
        } else if (g_strcmp0(name, "totp") == 0) {
                                                                           
                                                                  
                                                                  
                                                                      
                                                          
                                       
            if (g_strcmp0(action, "status") == 0 && g_strcmp0(method, "GET") == 0) {
                                                                         
                                                                    
                                                                           
                rq->rpc = pcv_rest_build_rpc("auth.totp.status", body);
            } else if (g_strcmp0(action, "disable") == 0 && g_strcmp0(method, "POST") == 0) {
                                                                                  
                rq->rpc = pcv_rest_build_rpc("auth.totp.disable", body);
            } else if (g_strcmp0(action, "reset") == 0 && g_strcmp0(method, "POST") == 0) {
                                                                                         
                rq->rpc = pcv_rest_build_rpc("auth.totp.reset", body);
            } else if (g_strcmp0(action, "recovery") == 0 && g_strcmp0(sub, "regenerate") == 0
                       && g_strcmp0(method, "POST") == 0) {
                                                                                      
                                      
                rq->rpc = pcv_rest_build_rpc("auth.totp.recovery.regenerate", body);
            }
        } else if (g_strcmp0(name, "sessions") == 0
                   && g_strcmp0(action, "revoke") == 0
                   && g_strcmp0(method, "POST") == 0) {
                                                                                          
            rq->rpc = pcv_rest_build_rpc("auth.session.revoke", body);
        } else if (g_strcmp0(name, "user-sessions") == 0
                   && g_strcmp0(action, "revoke") == 0
                   && g_strcmp0(method, "POST") == 0) {
                                                                                           
                                                                                    
            rq->rpc = pcv_rest_build_rpc("auth.user.sessions.revoke", body);
        } else if (g_strcmp0(name, "apikeys") == 0) {
                                                              
                                                                      
                                                     
            if (g_strcmp0(action, "") == 0 && g_strcmp0(method, "GET") == 0) {
                                                          
                rq->rpc = pcv_rest_build_rpc("auth.apikey.list", NULL);
            } else if (g_strcmp0(action, "") == 0 && g_strcmp0(method, "POST") == 0) {
                                                          
                                                                                 
                rq->rpc = pcv_rest_build_rpc("auth.apikey.create", body);
            } else if (g_strcmp0(sub, "revoke") == 0 && g_strcmp0(method, "POST") == 0) {
                                                                                  
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "client_name", action);
                rq->rpc = pcv_rest_build_rpc("auth.apikey.revoke", p);
                json_object_unref(p);
            }
        }
    }

                                                          
    else if (g_strcmp0(resource, "config") == 0) {
        if (g_strcmp0(name, "backup") == 0) {
            if (g_strcmp0(method, "POST") == 0)
                rq->rpc = pcv_rest_build_rpc("config.backup", body);
            else if (g_strcmp0(method, "GET") == 0)
                rq->rpc = pcv_rest_build_rpc("config.backup", NULL);
        }
        else if (g_strcmp0(name, "history") == 0 && g_strcmp0(method, "GET") == 0)
            rq->rpc = pcv_rest_build_rpc("config.history", NULL);
                                                                       
        else if (g_strcmp0(name, "reload") == 0 && g_strcmp0(method, "POST") == 0)
            rq->rpc = pcv_rest_build_rpc("config.reload", NULL);
                                                    
        else if (g_strcmp0(name, "daemon") == 0 && g_strcmp0(method, "GET") == 0)
            rq->rpc = pcv_rest_build_rpc("daemon.config.get", body);
                                                    
        else if (g_strcmp0(name, "daemon") == 0 && g_strcmp0(method, "PUT") == 0)
            rq->rpc = pcv_rest_build_rpc("daemon.config.set", body);
    }

                                                        
    else if (g_strcmp0(resource, "templates") == 0) {
        if (g_strcmp0(name, "history") == 0 && g_strcmp0(method, "GET") == 0)
            rq->rpc = pcv_rest_build_rpc("template.history", NULL);
        else if (*name == '\0') {
            if (g_strcmp0(method, "GET") == 0)
                rq->rpc = pcv_rest_build_rpc("template.list", NULL);
            else if (g_strcmp0(method, "POST") == 0)
                rq->rpc = pcv_rest_build_rpc("template.create", body);
        } else if (*name != '\0') {
            if (g_strcmp0(method, "GET") == 0)
                rq->rpc = pcv_rest_build_rpc_name("template.get", name);
            else if (g_strcmp0(method, "DELETE") == 0)
                rq->rpc = pcv_rest_build_rpc_name("template.delete", name);
        }
    }

                                                        
    else if (g_strcmp0(resource, "gpu") == 0) {
        if (g_strcmp0(name, "list") == 0 && g_strcmp0(method, "GET") == 0)
            rq->rpc = pcv_rest_build_rpc("gpu.list", body);
        else if (g_strcmp0(name, "metrics") == 0 && g_strcmp0(method, "GET") == 0)
            rq->rpc = pcv_rest_build_rpc("gpu.metrics", NULL);
        else if (g_strcmp0(name, "attach") == 0 && g_strcmp0(method, "POST") == 0)
            rq->rpc = pcv_rest_build_rpc("device.gpu.attach", body);
        else if (g_strcmp0(name, "detach") == 0 && g_strcmp0(method, "POST") == 0)
            rq->rpc = pcv_rest_build_rpc("device.gpu.detach", body);
    }

                                                                     
    else if (g_strcmp0(resource, "host") == 0) {
        if (g_strcmp0(name, "devices") == 0 && g_strcmp0(method, "GET") == 0)
            rq->rpc = pcv_rest_build_rpc("host.device.list", NULL);
    }

                                                            
    else if (g_strcmp0(resource, "vnc") == 0 && *name != '\0') {
        JsonObject *p = json_object_new();
        json_object_set_string_member(p, "vm_id", name);
        rq->rpc = pcv_rest_build_rpc("get_vnc_info", p);
        json_object_unref(p);
    }
    else
        return PCV_REST_STEP_UNMATCHED;
    return PCV_REST_STEP_NEXT;
}
