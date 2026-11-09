   
                           
                                                 
  
                           
                                                                                                  
                                                                                                                     
                                                                                                                  
                                                                   
  
                                         
   

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
pcv_rest_map_compute(PcvRestRequest *rq)
{
    SoupServerMessage *msg = rq->msg;
    GHashTable *query = rq->query;
    const gchar *method = rq->method;
    JsonObject *body = rq->body;
    gchar **segs = rq->segs;
    const gchar *resource = rq->resource;
    const gchar *name = rq->name;
    const gchar *action = rq->action;
    const gchar *sub = rq->sub;
                                                                     
                            
      
                                                   
                                                                  
                                                         
                                                           
      
              
                                                    
                                                               
      
                                                                     
                                                                                        
      
                                                   
                                                            
      
                                                                                       
                                                                    
                                                                                       
                                                                                   
                                                                                     
                                                                                     
                                                                                     
                                                                                     
                                                                                     
                                                                                     
                                                                                     
                                                                                      
                                                                                     
                                                                                     
                                                                                     
                                                                                     
                                                                                     
                                                                                     
                                                                                     
                                                                                     
                                                                                     
                                                                                     
                                                                                     
                                                                                     
                                                                                     
                                                                                     
                                                                                     
                                                                                     
                                                                                     
                                                                                     
                                                                                     
                                                                                     
                                                                                     
                                                                                     
                                                                                    
                                                                                       
                                                                
                                                                        

                                                                    
      
                                                       
                                                            
                                                        
                                
      
                          
                                                                
                                                       
                                               
      
              
                                              
                                                                              
                                                                
      
                                                 
                                                  
                                                      
      
                                                          
                                            
    if (g_strcmp0(resource, "rpc") == 0 && *name == '\0' &&
        g_strcmp0(method, "POST") == 0) {
        if (!body) {
            pcv_rest_send_error(msg, 400, "BAD_REQUEST", "JSON body required");
            return PCV_REST_STEP_CLEANUP;
        }
        JsonNode *bn = json_node_new(JSON_NODE_OBJECT);
        json_node_set_object(bn, body);
        rq->rpc = json_to_string(bn, FALSE);
        json_node_free(bn);
    }
    else
                                                       
    if (g_strcmp0(resource, "vms") == 0) {

        if (*name == '\0') {
                                                                
            if (g_strcmp0(method, "GET") == 0) {
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
                rq->rpc = pcv_rest_build_rpc("vm.list", pg);
                if (pg) json_object_unref(pg);
            }
                                                  
            else if (g_strcmp0(method, "POST") == 0) {
                const gchar *req[] = {"name"};
                if (!pcv_validate_required(msg, body, req, 1)) return PCV_REST_STEP_CLEANUP;
                rq->rpc = pcv_rest_build_rpc("vm.create", body);
            }

        } else if (*action == '\0') {
                                               
            if (g_strcmp0(method, "GET") == 0)
                rq->rpc = pcv_rest_build_rpc_name("vm.metrics", name);
                                                                                                           
            else if (g_strcmp0(method, "DELETE") == 0)
                rq->rpc = pcv_rest_build_rpc_name("vm.delete", name);

        } else if (g_strcmp0(action, "start") == 0) {
            rq->rpc = pcv_rest_build_rpc_name("vm.start", name);

        } else if (g_strcmp0(action, "stop") == 0) {
            rq->rpc = pcv_rest_build_rpc_name("vm.stop", name);

        } else if (g_strcmp0(action, "suspend") == 0 || g_strcmp0(action, "pause") == 0) {
            rq->rpc = pcv_rest_build_rpc_name("vm.pause", name);

        } else if (g_strcmp0(action, "resume") == 0) {
            rq->rpc = pcv_rest_build_rpc_name("vm.resume", name);

        } else if (g_strcmp0(action, "inspection") == 0) {
                                                                          
                                                               
                                                                  
            if (g_strcmp0(method, "GET") == 0 && g_strv_length(segs) == 3 &&
                (!query || g_hash_table_size(query) == 0) &&
                soup_server_message_get_request_body(msg)->length == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "uuid", name);
                rq->rpc = pcv_rest_build_rpc("vm.inspect", p);
                json_object_unref(p);
            }

        } else if (g_strcmp0(action, "metrics") == 0) {
            rq->rpc = pcv_rest_build_rpc_name("vm.metrics", name);

        } else if (g_strcmp0(action, "snapshot") == 0) {
                                                                 
            if (*sub == '\0' && g_strcmp0(method, "GET") == 0) {
                rq->rpc = pcv_rest_build_rpc_name("vm.snapshot.list", name);

                                                                    
            } else if (g_strcmp0(sub, "create") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", name);
                const gchar *sname = nullptr;
                if (body) {
                    if (json_object_has_member(body, "snapshot_name"))
                        sname = json_object_get_string_member(body, "snapshot_name");
                    else if (json_object_has_member(body, "snap_name"))
                        sname = json_object_get_string_member(body, "snap_name");
                }
                if (sname)
                    json_object_set_string_member(p, "snapshot_name", sname);
                rq->rpc = pcv_rest_build_rpc("vm.snapshot.create", p);
                json_object_unref(p);

                                                 
            } else if (g_strcmp0(sub, "rollback") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", name);
                const gchar *rname = nullptr;
                if (body) {
                    if (json_object_has_member(body, "snapshot_name"))
                        rname = json_object_get_string_member(body, "snapshot_name");
                    else if (json_object_has_member(body, "snap_name"))
                        rname = json_object_get_string_member(body, "snap_name");
                }
                if (rname)
                    json_object_set_string_member(p, "snapshot_name", rname);
                rq->rpc = pcv_rest_build_rpc("vm.snapshot.rollback", p);
                json_object_unref(p);

                                                                            
            } else if (g_strcmp0(sub, "delete_all") == 0 && g_strcmp0(method, "POST") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", name);
                if (body && json_object_has_member(body, "prefix"))
                    json_object_set_string_member(p, "prefix",
                        json_object_get_string_member(body, "prefix"));
                if (body && json_object_has_member(body, "keep_recent"))
                    json_object_set_int_member(p, "keep_recent",
                        json_object_get_int_member(body, "keep_recent"));
                rq->rpc = pcv_rest_build_rpc("vm.snapshot.delete_all", p);
                json_object_unref(p);

                                                                
                                                                           
                                                                            
                                                                              
                                                                 
            } else if (g_strcmp0(sub, "schedule") == 0) {
                if (g_strcmp0(method, "POST") == 0) {
                    JsonObject *p = body ? json_object_ref(body) : json_object_new();
                    json_object_set_string_member(p, "vm_name", name);
                    rq->rpc = pcv_rest_build_rpc("vm.snapshot.schedule.set", p);
                    json_object_unref(p);
                } else if (g_strcmp0(method, "GET") == 0) {
                    JsonObject *p = json_object_new();
                    json_object_set_string_member(p, "vm_name", name);
                    rq->rpc = pcv_rest_build_rpc("vm.snapshot.schedule.list", p);
                    json_object_unref(p);
                } else if (g_strcmp0(method, "DELETE") == 0) {
                    JsonObject *p = json_object_new();
                    json_object_set_string_member(p, "vm_name", name);
                    rq->rpc = pcv_rest_build_rpc("vm.snapshot.schedule.delete", p);
                    json_object_unref(p);
                }

                                                 
            } else if (*sub != '\0' && g_strcmp0(method, "DELETE") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", name);
                json_object_set_string_member(p, "snapshot_name", sub);
                rq->rpc = pcv_rest_build_rpc("vm.snapshot.delete", p);
                json_object_unref(p);
            }

                                                           
                                                              
                                                               
                                                               
                                                                 
        } else if (g_strcmp0(action, "nics") == 0) {
            if (*sub == '\0') {
                if (g_strcmp0(method, "GET") == 0) {
                    JsonObject *p = json_object_new();
                    json_object_set_string_member(p, "vm_id", name);
                    rq->rpc = pcv_rest_build_rpc("device.nic.list", p);
                    json_object_unref(p);
                } else if (g_strcmp0(method, "POST") == 0) {
                                                      
                    JsonObject *p = body
                        ? (json_object_ref(body), body)
                        : json_object_new();
                    json_object_set_string_member(p, "vm_id", name);
                    rq->rpc = pcv_rest_build_rpc("device.nic.attach", p);
                    json_object_unref(p);
                }
            } else if (g_strcmp0(method, "DELETE") == 0) {
                                                                     
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "vm_id", name);
                json_object_set_string_member(p, "mac",   sub);
                rq->rpc = pcv_rest_build_rpc("device.nic.detach", p);
                json_object_unref(p);
            }

                                                                
                                                         
                                                          
                                                          
                                                                 
        } else if (g_strcmp0(action, "usb") == 0) {
            if (g_strcmp0(method, "GET") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "vm_id", name);
                rq->rpc = pcv_rest_build_rpc("vm.usb.list", p);
                json_object_unref(p);
            } else if (g_strcmp0(method, "POST") == 0) {
                JsonObject *p = body
                    ? (json_object_ref(body), body)
                    : json_object_new();
                json_object_set_string_member(p, "vm_id", name);
                rq->rpc = pcv_rest_build_rpc("vm.usb.attach", p);
                json_object_unref(p);
            } else if (g_strcmp0(method, "DELETE") == 0) {
                JsonObject *p = body
                    ? (json_object_ref(body), body)
                    : json_object_new();
                json_object_set_string_member(p, "vm_id", name);
                rq->rpc = pcv_rest_build_rpc("vm.usb.detach", p);
                json_object_unref(p);
            }

                                                                
                                                          
                                                      
                                                                 
                                                              
                                                                                    
                                                                  
                                                                 
        } else if (g_strcmp0(action, "vcpu") == 0 && g_strcmp0(method, "PUT") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            gint vcpu_count = 0;
            if (pcv_rpc_params_get_int_alias(p, "vcpu_count", "vcpu", &vcpu_count) ||
                pcv_rpc_params_get_int_alias(p, "count", NULL, &vcpu_count)) {
                json_object_set_int_member(p, "vcpu_count", vcpu_count);
            }
            json_object_set_string_member(p, "vm_id", name);
            rq->rpc = pcv_rest_build_rpc("vm.set_vcpu", p);
            json_object_unref(p);

        } else if (g_strcmp0(action, "memory") == 0 && g_strcmp0(method, "PUT") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            json_object_set_string_member(p, "vm_id", name);
            rq->rpc = pcv_rest_build_rpc("vm.set_memory", p);
            json_object_unref(p);

        } else if (g_strcmp0(action, "delete-status") == 0 && g_strcmp0(method, "GET") == 0) {
            rq->rpc = pcv_rest_build_rpc_name("vm.delete.status", name);

        } else if (g_strcmp0(action, "rename") == 0 && g_strcmp0(method, "PUT") == 0) {
            if (!body || !json_object_has_member(body, "new_name")) {
                pcv_rest_send_error(msg, 400, "BAD_REQUEST", "new_name required");
                return PCV_REST_STEP_CLEANUP;
            }
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "name", name);
            json_object_set_string_member(p, "new_name",
                json_object_get_string_member(body, "new_name"));
            rq->rpc = pcv_rest_build_rpc("vm.rename", p);
            json_object_unref(p);

        } else if (g_strcmp0(action, "iso") == 0) {
            if (g_strcmp0(method, "POST") == 0) {
                JsonObject *p = body ? (json_object_ref(body), body)
                                     : json_object_new();
                json_object_set_string_member(p, "vm_id", name);
                rq->rpc = pcv_rest_build_rpc("vm.mount_iso", p);
                json_object_unref(p);
            } else if (g_strcmp0(method, "DELETE") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "vm_id", name);
                rq->rpc = pcv_rest_build_rpc("vm.eject", p);
                json_object_unref(p);
            }

                                                                 
        } else if (g_strcmp0(action, "clone") == 0 && g_strcmp0(method, "POST") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            json_object_set_string_member(p, "name", name);
            rq->rpc = pcv_rest_build_rpc("vm.clone", p);
            json_object_unref(p);

                                                                 
        } else if (g_strcmp0(action, "disk") == 0 && g_strcmp0(method, "PUT") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            json_object_set_string_member(p, "vm_id", name);
            rq->rpc = pcv_rest_build_rpc("vm.resize_disk", p);
            json_object_unref(p);

                                                                  
        } else if (g_strcmp0(action, "cpu-pin") == 0 && g_strcmp0(method, "PUT") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            json_object_set_string_member(p, "vm_id", name);
            rq->rpc = pcv_rest_build_rpc("vm.pin_vcpu", p);
            json_object_unref(p);

                                                                  
        } else if (g_strcmp0(action, "bandwidth") == 0 && g_strcmp0(method, "PUT") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            json_object_set_string_member(p, "vm_id", name);
            rq->rpc = pcv_rest_build_rpc("vm.set_bandwidth", p);
            json_object_unref(p);

                                                                   
        } else if (g_strcmp0(action, "export") == 0 && g_strcmp0(method, "POST") == 0) {
            rq->rpc = pcv_rest_build_rpc_name("vm.export.ova", name);

                                                                 
        } else if (g_strcmp0(action, "import") == 0 && g_strcmp0(method, "POST") == 0) {
            rq->rpc = pcv_rest_build_rpc("vm.import.ova", body);

                                                                   
        } else if (g_strcmp0(action, "memory-stats") == 0 && g_strcmp0(method, "GET") == 0) {
            rq->rpc = pcv_rest_build_rpc_name("vm.memory.stats", name);

                                                                            
        } else if (g_strcmp0(action, "snapshot-schedule") == 0 && g_strcmp0(method, "GET") == 0) {
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "name", name);
            rq->rpc = pcv_rest_build_rpc("snapshot.schedule.status", p);
            json_object_unref(p);

                                                                   
        } else if (g_strcmp0(action, "guest-ping") == 0 && g_strcmp0(method, "POST") == 0) {
            rq->rpc = pcv_rest_build_rpc_name("vm.guest.ping", name);

                                                                   
        } else if (g_strcmp0(action, "guest-agent") == 0 && g_strcmp0(method, "GET") == 0) {
            rq->rpc = pcv_rest_build_rpc_name("vm.guest.agent.status", name);

                                                                   
        } else if (g_strcmp0(action, "guest-agent-channel") == 0 && g_strcmp0(method, "POST") == 0) {
            rq->rpc = pcv_rest_build_rpc_name("vm.guest.agent.ensure_channel", name);

                                                                   
        } else if (g_strcmp0(action, "guest-shutdown") == 0 && g_strcmp0(method, "POST") == 0) {
            rq->rpc = pcv_rest_build_rpc_name("vm.guest.shutdown", name);

                                                                  
        } else if (g_strcmp0(action, "disk-usage") == 0 && g_strcmp0(method, "GET") == 0) {
            rq->rpc = pcv_rest_build_rpc_name("vm.guest.fsinfo", name);

                                                                  
        } else if (g_strcmp0(action, "disk-resize") == 0 && g_strcmp0(method, "POST") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            json_object_set_string_member(p, "name", name);
            rq->rpc = pcv_rest_build_rpc("vm.disk.live_resize", p);
            json_object_unref(p);

                                                                   
        } else if (g_strcmp0(action, "guest-exec") == 0 && g_strcmp0(method, "POST") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            json_object_set_string_member(p, "name", name);
            rq->rpc = pcv_rest_build_rpc("vm.guest.exec", p);
            json_object_unref(p);
        }
    }

                                                                
    else if (g_strcmp0(resource, "containers") == 0) {

        if (*name == '\0') {
            if (g_strcmp0(method, "GET") == 0) {
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
                rq->rpc = pcv_rest_build_rpc("container.list", pg);
                if (pg) json_object_unref(pg);
            } else if (g_strcmp0(method, "POST") == 0)
                rq->rpc = pcv_rest_build_rpc("container.create", body);

        } else if (*action == '\0' && g_strcmp0(method, "DELETE") == 0) {
            rq->rpc = pcv_rest_build_rpc_name("container.destroy", name);

        } else if (g_strcmp0(action, "start") == 0) {
            rq->rpc = pcv_rest_build_rpc_name("container.start", name);
        } else if (g_strcmp0(action, "stop") == 0) {
            rq->rpc = pcv_rest_build_rpc_name("container.stop", name);
        } else if (g_strcmp0(action, "metrics") == 0) {
            rq->rpc = pcv_rest_build_rpc_name("container.metrics", name);
        } else if (g_strcmp0(action, "exec") == 0) {
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "name", name);
            if (body && json_object_has_member(body, "command"))
                json_object_set_string_member(p, "cmd",
                    json_object_get_string_member(body, "command"));
            rq->rpc = pcv_rest_build_rpc("container.exec", p);
            json_object_unref(p);

                                                               
                                                                                
                                                                                  
                                                                                    
                                                                                  
                                                                 
        } else if (g_strcmp0(action, "snapshots") == 0) {
            if (*sub == '\0') {
                if (g_strcmp0(method, "GET") == 0) {
                    rq->rpc = pcv_rest_build_rpc_name("container.snapshot.list", name);
                } else if (g_strcmp0(method, "POST") == 0) {
                    JsonObject *p = body ? json_object_ref(body) : json_object_new();
                    json_object_set_string_member(p, "name", name);
                    rq->rpc = pcv_rest_build_rpc("container.snapshot.create", p);
                    json_object_unref(p);
                }
            } else if (g_strcmp0(sub, "rollback") == 0 && g_strcmp0(method, "POST") == 0) {
                JsonObject *p = body ? json_object_ref(body) : json_object_new();
                json_object_set_string_member(p, "name", name);
                rq->rpc = pcv_rest_build_rpc("container.snapshot.rollback", p);
                json_object_unref(p);
            } else if (*sub != '\0' && g_strcmp0(method, "DELETE") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", name);
                json_object_set_string_member(p, "snap_name", sub);
                rq->rpc = pcv_rest_build_rpc("container.snapshot.delete", p);
                json_object_unref(p);
            }

                                                                          
        } else if (g_strcmp0(action, "limits") == 0 && g_strcmp0(method, "PUT") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            json_object_set_string_member(p, "name", name);
            rq->rpc = pcv_rest_build_rpc("container.set_limits", p);
            json_object_unref(p);

                                                                    
        } else if (g_strcmp0(action, "nics") == 0) {
            if (*sub == '\0') {
                if (g_strcmp0(method, "GET") == 0) {
                    JsonObject *p = json_object_new();
                    json_object_set_string_member(p, "name", name);
                    rq->rpc = pcv_rest_build_rpc("container.nic.list", p);
                    json_object_unref(p);
                } else if (g_strcmp0(method, "POST") == 0) {
                    JsonObject *p = body ? (json_object_ref(body), body)
                                         : json_object_new();
                    json_object_set_string_member(p, "name", name);
                    rq->rpc = pcv_rest_build_rpc("container.nic.attach", p);
                    json_object_unref(p);
                }
            } else if (g_strcmp0(method, "DELETE") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", name);
                json_object_set_string_member(p, "nic_name", sub);
                rq->rpc = pcv_rest_build_rpc("container.nic.detach", p);
                json_object_unref(p);
            }

        } else if (g_strcmp0(action, "bandwidth") == 0 && g_strcmp0(method, "PUT") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            json_object_set_string_member(p, "name", name);
            rq->rpc = pcv_rest_build_rpc("container.set_bandwidth", p);
            json_object_unref(p);

                                                               
                                                                
                                                                
                                                                   
                                                                  
        } else if (g_strcmp0(action, "health") == 0) {
            if (g_strcmp0(method, "PUT") == 0) {
                JsonObject *p = body ? json_object_ref(body) : json_object_new();
                json_object_set_string_member(p, "name", name);
                rq->rpc = pcv_rest_build_rpc("container.health.set", p);
                json_object_unref(p);
            } else if (g_strcmp0(method, "GET") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", name);
                rq->rpc = pcv_rest_build_rpc("container.health.get", p);
                json_object_unref(p);
            } else if (g_strcmp0(method, "DELETE") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", name);
                rq->rpc = pcv_rest_build_rpc("container.health.delete", p);
                json_object_unref(p);
            }

                                                               
                                                                          
                                                                            
                                                                            
                                                                               
                                                                            
                                                       
                                                                  
        } else if (g_strcmp0(action, "volumes") == 0) {
            if (*sub == '\0') {
                if (g_strcmp0(method, "GET") == 0) {
                    JsonObject *p = json_object_new();
                    json_object_set_string_member(p, "name", name);
                    rq->rpc = pcv_rest_build_rpc("container.volume.list", p);
                    json_object_unref(p);
                } else if (g_strcmp0(method, "POST") == 0) {
                    JsonObject *p = body ? json_object_ref(body) : json_object_new();
                    json_object_set_string_member(p, "name", name);
                    rq->rpc = pcv_rest_build_rpc("container.volume.attach", p);
                    json_object_unref(p);
                } else if (g_strcmp0(method, "DELETE") == 0) {
                                                                       
                    JsonObject *p = pcv_rest_container_volume_detach_params(name, NULL, body);
                    rq->rpc = pcv_rest_build_rpc("container.volume.detach", p);
                    json_object_unref(p);
                }
            } else if (g_strcmp0(method, "DELETE") == 0) {
                                                                            
                                                                                  
                JsonObject *p = pcv_rest_container_volume_detach_params(name, segs + 3, body);
                rq->rpc = pcv_rest_build_rpc("container.volume.detach", p);
                json_object_unref(p);
            }

                                                               
                                                            
                                                           
                                                              
                                                                  
        } else if (g_strcmp0(action, "env") == 0) {
            if (g_strcmp0(method, "GET") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", name);
                rq->rpc = pcv_rest_build_rpc("container.env.list", p);
                json_object_unref(p);
            } else if (g_strcmp0(method, "PUT") == 0) {
                JsonObject *p = body ? json_object_ref(body) : json_object_new();
                json_object_set_string_member(p, "name", name);
                rq->rpc = pcv_rest_build_rpc("container.env.set", p);
                json_object_unref(p);
            } else if (g_strcmp0(method, "DELETE") == 0) {
                JsonObject *p = body ? json_object_ref(body) : json_object_new();
                json_object_set_string_member(p, "name", name);
                rq->rpc = pcv_rest_build_rpc("container.env.delete", p);
                json_object_unref(p);
            }

                                                                          
                                                                
        } else if (g_strcmp0(action, "clone") == 0 && g_strcmp0(method, "POST") == 0) {
            JsonObject *p = body ? json_object_ref(body) : json_object_new();
            json_object_set_string_member(p, "source", name);
            rq->rpc = pcv_rest_build_rpc("container.clone", p);
            json_object_unref(p);

                                                                                          
        } else if (g_strcmp0(action, "memory-stats") == 0 && g_strcmp0(method, "GET") == 0) {
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "name", name);
            rq->rpc = pcv_rest_build_rpc("container.memory.stats", p);
            json_object_unref(p);
        }
    }
    else
        return PCV_REST_STEP_UNMATCHED;
    return PCV_REST_STEP_NEXT;
}
