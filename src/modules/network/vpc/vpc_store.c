   
                    
                                                
  
                       
                                                             
                                                  
  
             
                                                                       
                                                                                   
                                                                   
                                                                   
  
         
                                                      
                                                                                   
                                                              
                                                               
                                                               
                                                             
  
                                       
                                                                               
                                                                           
                                                                                    
                                                                                 
                                                                              
                                
  
                                       
                                                                                
                                                                 
                                                                     
                                                                    
                                       
  
                                         
                                                                            
                                                                          
                                                                     
                                        
  
                                                                                
                                                                   
                                                                           
                                                             
  
                                                                                  
                                                                                  
                                                                  
                                                             
  
                                                                            
                                                                           
                                                                  
                                    
  
                       
                                                                
                                                           
                                                      
  
                                        
                                                                         
                                                                                 
                                                                        
                                                                                
                                                
  
                              
                                                                        
                                                                     
                                                                 
                                                                       
                                       
  
                                       
                                                                                  
                                                                     
                                                               
                                                            
                                
  
                            
                                                                         
                                                                                       
                                                                      
                                                                      
                                                   
  
                         
                                                                            
                                                                     
                                                                          
                                                                   
                            
  
                                   
                                                                                
                                                                        
                                                                        
                                                               
                              
  
                               
                                                                 
                                                                            
                                                          
                                                                     
                      
  
                                       
                                                                        
                                                                        
                                                                                 
                                                                     
                                                                 
  
                                   
                                                                       
                                                                            
                                                                
                                                                   
                     
  
                                
                                                                       
                                                                                   
                                                                 
                                                                     
                                                             
  
                                 
                                                                                               
                                                                     
                                                                      
                                                                    
            
  
            
                                                                      
                                                                   
                                                                  
                                                                          
                     
  
                 
                                                                     
                                                              
                                                                     
                                      
  
         
                                                                                              
                                                                                
                                                                           
                      
                                                                  
                                                        
                                                                      
                                                                 
                                                
                                                                     
                                                           
                                                           
                                                  
  
                       
                                                         
                                                      
                                                           
                      
   
#include "vpc_store.h"

#include "vpc_model.h"

#include <sqlite3.h>

struct _PcvVpcStore {
    sqlite3 *db;
    GMutex mutex;
};

static const gchar *SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS vpcs("
    " id TEXT PRIMARY KEY, name TEXT NOT NULL, tenant TEXT NOT NULL,"
    " egress_mode TEXT NOT NULL CHECK(egress_mode IN ('nat','isolated')),"
    " backend TEXT NOT NULL DEFAULT 'linux' CHECK(backend IN ('linux','ovn')),"
    " state TEXT NOT NULL, revision INTEGER NOT NULL DEFAULT 1,"
    " last_error TEXT, created_at INTEGER NOT NULL DEFAULT(unixepoch()),"
    " updated_at INTEGER NOT NULL DEFAULT(unixepoch()),"
    " UNIQUE(tenant,name));"
    "CREATE TABLE IF NOT EXISTS subnets("
    " id TEXT PRIMARY KEY, vpc_id TEXT NOT NULL REFERENCES vpcs(id) ON DELETE RESTRICT,"
    " name TEXT NOT NULL, cidr TEXT NOT NULL, network_start INTEGER NOT NULL,"
    " network_end INTEGER NOT NULL, prefix INTEGER NOT NULL, gateway TEXT NOT NULL,"
    " allocation_start TEXT NOT NULL, allocation_end TEXT NOT NULL,"
    " dhcp_enabled INTEGER NOT NULL DEFAULT 1, mtu INTEGER NOT NULL,"
    " backend_ref TEXT NOT NULL UNIQUE, bridge_name TEXT UNIQUE,"
    " state TEXT NOT NULL, last_error TEXT,"
    " created_at INTEGER NOT NULL DEFAULT(unixepoch()),"
    " updated_at INTEGER NOT NULL DEFAULT(unixepoch()),"
    " UNIQUE(vpc_id,name), UNIQUE(cidr));"
    "CREATE INDEX IF NOT EXISTS idx_vpc_subnets_range ON subnets(network_start,network_end);"
    "CREATE TABLE IF NOT EXISTS attachments("
    " id TEXT PRIMARY KEY, subnet_id TEXT NOT NULL REFERENCES subnets(id) ON DELETE RESTRICT,"
    " vm_uuid TEXT NOT NULL, vm_name_snapshot TEXT NOT NULL, owner_subject TEXT NOT NULL,"
    " mac_address TEXT NOT NULL UNIQUE, ip_address TEXT NOT NULL UNIQUE,"
    " state TEXT NOT NULL, last_error TEXT, created_at INTEGER NOT NULL DEFAULT(unixepoch()),"
    " updated_at INTEGER NOT NULL DEFAULT(unixepoch()), UNIQUE(subnet_id,vm_uuid));"
    "CREATE TABLE IF NOT EXISTS service_publishes("
    " id TEXT PRIMARY KEY, vpc_id TEXT NOT NULL REFERENCES vpcs(id) ON DELETE RESTRICT,"
    " attachment_id TEXT NOT NULL REFERENCES attachments(id) ON DELETE RESTRICT,"
    " protocol TEXT NOT NULL CHECK(protocol IN ('tcp','udp')),"
    " listen_address TEXT NOT NULL, listen_port INTEGER NOT NULL, target_port INTEGER NOT NULL,"
    " allowed_sources TEXT NOT NULL, state TEXT NOT NULL, last_error TEXT,"
    " created_at INTEGER NOT NULL DEFAULT(unixepoch()),"
    " updated_at INTEGER NOT NULL DEFAULT(unixepoch()),"
    " UNIQUE(protocol,listen_address,listen_port));"
    "CREATE TABLE IF NOT EXISTS vpc_backend_bindings("
    " vpc_id TEXT PRIMARY KEY REFERENCES vpcs(id) ON DELETE CASCADE,"
    " backend_ref TEXT NOT NULL UNIQUE, edge_cidr TEXT NOT NULL UNIQUE,"
    " host_edge_ip TEXT NOT NULL UNIQUE, router_edge_ip TEXT NOT NULL UNIQUE,"
    " generation INTEGER NOT NULL, actual_revision INTEGER NOT NULL DEFAULT 0);"
    "PRAGMA user_version=2;";

#define PCV_VPC_SCHEMA_VERSION 2

static const gchar *MIGRATE_V1_TO_V2_SQL =
    "ALTER TABLE vpcs ADD COLUMN backend TEXT NOT NULL DEFAULT 'linux' "
    "CHECK(backend IN ('linux','ovn'));"
    "CREATE TABLE subnets_v2("
    " id TEXT PRIMARY KEY, vpc_id TEXT NOT NULL REFERENCES vpcs(id) ON DELETE RESTRICT,"
    " name TEXT NOT NULL, cidr TEXT NOT NULL, network_start INTEGER NOT NULL,"
    " network_end INTEGER NOT NULL, prefix INTEGER NOT NULL, gateway TEXT NOT NULL,"
    " allocation_start TEXT NOT NULL, allocation_end TEXT NOT NULL,"
    " dhcp_enabled INTEGER NOT NULL DEFAULT 1, mtu INTEGER NOT NULL,"
    " backend_ref TEXT NOT NULL UNIQUE, bridge_name TEXT UNIQUE, state TEXT NOT NULL,"
    " last_error TEXT, created_at INTEGER NOT NULL DEFAULT(unixepoch()),"
    " updated_at INTEGER NOT NULL DEFAULT(unixepoch()),"
    " UNIQUE(vpc_id,name), UNIQUE(cidr));"
    "INSERT INTO subnets_v2(id,vpc_id,name,cidr,network_start,network_end,prefix,gateway,"
    " allocation_start,allocation_end,dhcp_enabled,mtu,backend_ref,bridge_name,state,"
    " last_error,created_at,updated_at) SELECT id,vpc_id,name,cidr,network_start,network_end,"
    " prefix,gateway,allocation_start,allocation_end,dhcp_enabled,mtu,bridge_name,bridge_name,"
    " state,last_error,created_at,updated_at FROM subnets;"
    "DROP TABLE subnets;ALTER TABLE subnets_v2 RENAME TO subnets;"
    "CREATE INDEX idx_vpc_subnets_range ON subnets(network_start,network_end);"
    "CREATE TABLE vpc_backend_bindings("
    " vpc_id TEXT PRIMARY KEY REFERENCES vpcs(id) ON DELETE CASCADE,"
    " backend_ref TEXT NOT NULL UNIQUE, edge_cidr TEXT NOT NULL UNIQUE,"
    " host_edge_ip TEXT NOT NULL UNIQUE, router_edge_ip TEXT NOT NULL UNIQUE,"
    " generation INTEGER NOT NULL, actual_revision INTEGER NOT NULL DEFAULT 0);"
    "PRAGMA user_version=2;";

static void
_set_sql_error(PcvVpcStore *store, GError **error, const gchar *operation)
{
    g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO, "%s: %s",
                operation, store && store->db ? sqlite3_errmsg(store->db) : "DB unavailable");
}

static gboolean
_exec(PcvVpcStore *store, const gchar *sql, GError **error)
{
    gchar *message = NULL;
    gint rc = sqlite3_exec(store->db, sql, NULL, NULL, &message);
    if (rc == SQLITE_OK)
        return TRUE;
    g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO, "SQLite 실행 실패: %s",
                message ? message : sqlite3_errmsg(store->db));
    sqlite3_free(message);
    return FALSE;
}

static void
_rollback(PcvVpcStore *store)
{
    sqlite3_exec(store->db, "ROLLBACK", NULL, NULL, NULL);
}

static gboolean
_begin(PcvVpcStore *store, GError **error)
{
    return _exec(store, "BEGIN IMMEDIATE", error);
}

static gboolean
_commit(PcvVpcStore *store, GError **error)
{
    return _exec(store, "COMMIT", error);
}

static gboolean
_bind_text(sqlite3_stmt *stmt, gint column, const gchar *value)
{
    return sqlite3_bind_text(stmt, column, value, -1, SQLITE_TRANSIENT) == SQLITE_OK;
}

                                                                           
static gboolean
_bump_vpc_revision_locked(PcvVpcStore *store,
                          const gchar *vpc_id,
                          GError **error)
{
    sqlite3_stmt *stmt = NULL;
    gboolean ok = sqlite3_prepare_v2(store->db,
        "UPDATE vpcs SET revision=revision+1,updated_at=unixepoch() WHERE id=?",
        -1, &stmt, NULL) == SQLITE_OK && stmt;
    if (ok) {
        _bind_text(stmt, 1, vpc_id);
        ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(store->db) == 1;
    }
    if (!ok)
        _set_sql_error(store, error, "VPC revision 증가 실패");
    if (stmt)
        sqlite3_finalize(stmt);
    return ok;
}

static gboolean
_lookup_vpc_locked(PcvVpcStore *store,
                   const gchar *id,
                   const gchar *tenant,
                   gchar **mode_out,
                   gchar **backend_out,
                   gint64 *revision_out,
                   GError **error)
{
    sqlite3_stmt *stmt = NULL;
    const gchar *sql = tenant
        ? "SELECT egress_mode,backend,revision FROM vpcs WHERE id=? AND tenant=?"
        : "SELECT egress_mode,backend,revision FROM vpcs WHERE id=?";
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK || !stmt) {
        _set_sql_error(store, error, "VPC 조회 준비 실패");
        return FALSE;
    }
    _bind_text(stmt, 1, id);
    if (tenant) _bind_text(stmt, 2, tenant);
    gboolean found = sqlite3_step(stmt) == SQLITE_ROW;
    if (found) {
        if (mode_out) *mode_out = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));
        if (backend_out) *backend_out = g_strdup((const gchar *)sqlite3_column_text(stmt, 1));
        if (revision_out) *revision_out = sqlite3_column_int64(stmt, 2);
    }
    sqlite3_finalize(stmt);
    if (!found)
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_NOT_FOUND, "VPC를 찾을 수 없습니다");
    return found;
}

static gboolean
_check_revision(gint64 actual, gint64 expected, GError **error)
{
    if (expected < 0 || actual == expected)
        return TRUE;
    g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_STALE_REVISION,
                "VPC revision 충돌: expected=%" G_GINT64_FORMAT ", actual=%" G_GINT64_FORMAT,
                expected, actual);
    return FALSE;
}

PcvVpcStore *
pcv_vpc_store_open(const gchar *path, GError **error)
{
    if (!path || !*path) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_INVALID_ARGUMENT,
                    "VPC DB 경로가 필요합니다");
        return NULL;
    }
    PcvVpcStore *store = g_new0(PcvVpcStore, 1);
    g_mutex_init(&store->mutex);
    if (sqlite3_open_v2(path, &store->db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        NULL) != SQLITE_OK) {
        _set_sql_error(store, error, "VPC DB 열기 실패");
        pcv_vpc_store_free(store);
        return NULL;
    }
    sqlite3_busy_timeout(store->db, 5000);
    sqlite3_stmt *version_stmt = NULL;
    gint version = -1;
    if (sqlite3_prepare_v2(store->db, "PRAGMA user_version", -1,
                           &version_stmt, NULL) == SQLITE_OK &&
        sqlite3_step(version_stmt) == SQLITE_ROW)
        version = sqlite3_column_int(version_stmt, 0);
    if (version_stmt) sqlite3_finalize(version_stmt);
    if (version < 0 || version > PCV_VPC_SCHEMA_VERSION) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_STATE,
                    "지원하지 않는 VPC DB schema version입니다: %d", version);
        pcv_vpc_store_free(store);
        return NULL;
    }
                                                                                
                                                                          
                                                                  
                                        
                                                            
    gboolean ok = _exec(store, "PRAGMA journal_mode=WAL;PRAGMA foreign_keys=OFF;", error) &&
                  _begin(store, error);
    if (ok)
        ok = _exec(store, version == 1 ? MIGRATE_V1_TO_V2_SQL : SCHEMA_SQL, error);
    sqlite3_stmt *fk_stmt = NULL;
    if (ok && sqlite3_prepare_v2(store->db, "PRAGMA foreign_key_check", -1,
                                 &fk_stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(fk_stmt) == SQLITE_ROW) {
            g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_STATE,
                        "VPC DB migration 뒤 foreign key 불일치가 있습니다");
            ok = FALSE;
        }
    } else if (ok) {
        _set_sql_error(store, error, "VPC DB foreign key 검사 준비 실패");
        ok = FALSE;
    }
    if (fk_stmt)
        sqlite3_finalize(fk_stmt);
    if (ok) {
        if (!_commit(store, error)) {
            _rollback(store);
            ok = FALSE;
        }
    } else {
        _rollback(store);
    }
    if (ok)
        ok = _exec(store, "PRAGMA foreign_keys=ON;", error);
    if (!ok) {
        pcv_vpc_store_free(store);
        return NULL;
    }
    return store;
}

void
pcv_vpc_store_free(PcvVpcStore *store)
{
    if (!store) return;
    if (store->db) sqlite3_close(store->db);
    g_mutex_clear(&store->mutex);
    g_free(store);
}

gboolean
pcv_vpc_store_create_vpc(PcvVpcStore *store,
                         const gchar *name,
                         const gchar *tenant,
                         const gchar *egress_mode,
                         const gchar *backend,
                         gchar **id_out,
                         gint64 *revision_out,
                         GError **error)
{
    if (id_out) *id_out = NULL;
    if (!store || !pcv_vpc_name_is_valid(name) || !pcv_vpc_name_is_valid(tenant) ||
        !pcv_vpc_egress_mode_is_valid(egress_mode) || !pcv_vpc_backend_is_valid(backend)) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_INVALID_ARGUMENT,
                    "VPC name, tenant, egress_mode 또는 backend가 유효하지 않습니다");
        return FALSE;
    }
    g_autofree gchar *id = g_uuid_string_random();
    g_mutex_lock(&store->mutex);
    sqlite3_stmt *stmt = NULL;
    const gchar *sql =
        "INSERT INTO vpcs(id,name,tenant,egress_mode,backend,state) VALUES(?,?,?,?,?, 'CREATING')";
    gboolean ok = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) == SQLITE_OK && stmt &&
        _bind_text(stmt, 1, id) && _bind_text(stmt, 2, name) &&
        _bind_text(stmt, 3, tenant) && _bind_text(stmt, 4, egress_mode) &&
        _bind_text(stmt, 5, backend) &&
        sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) {
        if (sqlite3_extended_errcode(store->db) == SQLITE_CONSTRAINT_UNIQUE)
            g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT,
                        "tenant 안에 같은 VPC 이름이 있습니다");
        else
            _set_sql_error(store, error, "VPC 생성 실패");
    }
    if (stmt) sqlite3_finalize(stmt);
    g_mutex_unlock(&store->mutex);
    if (ok) {
        if (id_out) *id_out = g_steal_pointer(&id);
        if (revision_out) *revision_out = 1;
    }
    return ok;
}

gboolean
pcv_vpc_store_delete_vpc(PcvVpcStore *store,
                         const gchar *id,
                         const gchar *tenant,
                         GError **error)
{
    if (!store || !id) return FALSE;
    g_mutex_lock(&store->mutex);
    if (!_begin(store, error)) { g_mutex_unlock(&store->mutex); return FALSE; }
    gboolean ok = _lookup_vpc_locked(store, id, tenant, NULL, NULL, NULL, error);
    sqlite3_stmt *stmt = NULL;
    if (ok && sqlite3_prepare_v2(store->db,
            "SELECT 1 FROM subnets WHERE vpc_id=? LIMIT 1", -1, &stmt, NULL) == SQLITE_OK) {
        _bind_text(stmt, 1, id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT,
                        "subnet이 있는 VPC는 삭제할 수 없습니다");
            ok = FALSE;
        }
    } else if (ok) {
        _set_sql_error(store, error, "VPC child 조회 실패"); ok = FALSE;
    }
    if (stmt) sqlite3_finalize(stmt);
    stmt = NULL;
    if (ok && sqlite3_prepare_v2(store->db,
            "DELETE FROM vpcs WHERE id=?", -1, &stmt, NULL) == SQLITE_OK) {
        _bind_text(stmt, 1, id);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
    } else if (ok) ok = FALSE;
    if (stmt) sqlite3_finalize(stmt);
    if (ok) ok = _commit(store, error); else _rollback(store);
    g_mutex_unlock(&store->mutex);
    return ok;
}

gboolean
pcv_vpc_store_set_egress(PcvVpcStore *store,
                         const gchar *id,
                         const gchar *tenant,
                         const gchar *egress_mode,
                         gint64 expected_revision,
                         gint64 *revision_out,
                         GError **error)
{
    if (!store || !id || !pcv_vpc_egress_mode_is_valid(egress_mode)) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_INVALID_ARGUMENT,
                    "유효한 VPC와 egress_mode가 필요합니다");
        return FALSE;
    }
    g_mutex_lock(&store->mutex);
    if (!_begin(store, error)) { g_mutex_unlock(&store->mutex); return FALSE; }
    gint64 revision = 0;
    gboolean ok = _lookup_vpc_locked(store, id, tenant, NULL, NULL, &revision, error) &&
                  _check_revision(revision, expected_revision, error);
    sqlite3_stmt *stmt = NULL;
    if (ok && sqlite3_prepare_v2(store->db,
            "UPDATE vpcs SET egress_mode=?,revision=revision+1,updated_at=unixepoch() WHERE id=?",
            -1, &stmt, NULL) == SQLITE_OK) {
        _bind_text(stmt, 1, egress_mode); _bind_text(stmt, 2, id);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
    } else if (ok) { _set_sql_error(store, error, "egress 변경 준비 실패"); ok = FALSE; }
    if (stmt) sqlite3_finalize(stmt);
    if (ok) ok = _commit(store, error); else _rollback(store);
    g_mutex_unlock(&store->mutex);
    if (ok && revision_out) *revision_out = revision + 1;
    return ok;
}

static JsonObject *
_vpc_row_to_json(sqlite3_stmt *stmt)
{
    JsonObject *o = json_object_new();
    json_object_set_string_member(o, "id", (const gchar *)sqlite3_column_text(stmt, 0));
    json_object_set_string_member(o, "name", (const gchar *)sqlite3_column_text(stmt, 1));
    json_object_set_string_member(o, "tenant", (const gchar *)sqlite3_column_text(stmt, 2));
    json_object_set_string_member(o, "egress_mode", (const gchar *)sqlite3_column_text(stmt, 3));
    json_object_set_string_member(o, "backend", (const gchar *)sqlite3_column_text(stmt, 4));
    json_object_set_string_member(o, "state", (const gchar *)sqlite3_column_text(stmt, 5));
    json_object_set_int_member(o, "revision", sqlite3_column_int64(stmt, 6));
    if (sqlite3_column_type(stmt, 7) != SQLITE_NULL)
        json_object_set_string_member(o, "last_error", (const gchar *)sqlite3_column_text(stmt, 7));
    else
        json_object_set_null_member(o, "last_error");
    return o;
}

JsonArray *
pcv_vpc_store_list_vpcs(PcvVpcStore *store, const gchar *tenant, GError **error)
{
    if (!store) return NULL;
    JsonArray *array = json_array_new();
    g_mutex_lock(&store->mutex);
    sqlite3_stmt *stmt = NULL;
    const gchar *sql = tenant
        ? "SELECT id,name,tenant,egress_mode,backend,state,revision,last_error FROM vpcs WHERE tenant=? ORDER BY name"
        : "SELECT id,name,tenant,egress_mode,backend,state,revision,last_error FROM vpcs ORDER BY tenant,name";
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK || !stmt) {
        _set_sql_error(store, error, "VPC 목록 준비 실패");
        g_mutex_unlock(&store->mutex);
        json_array_unref(array);
        return NULL;
    }
    if (tenant) _bind_text(stmt, 1, tenant);
    while (sqlite3_step(stmt) == SQLITE_ROW)
        json_array_add_object_element(array, _vpc_row_to_json(stmt));
    sqlite3_finalize(stmt);
    g_mutex_unlock(&store->mutex);
    return array;
}

JsonObject *
pcv_vpc_store_get_vpc(PcvVpcStore *store,
                      const gchar *id,
                      const gchar *tenant,
                      GError **error)
{
    if (!store || !id) return NULL;
    g_mutex_lock(&store->mutex);
    sqlite3_stmt *stmt = NULL;
    const gchar *sql = tenant
        ? "SELECT id,name,tenant,egress_mode,backend,state,revision,last_error FROM vpcs WHERE id=? AND tenant=?"
        : "SELECT id,name,tenant,egress_mode,backend,state,revision,last_error FROM vpcs WHERE id=?";
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK || !stmt) {
        _set_sql_error(store, error, "VPC 상세 준비 실패");
        g_mutex_unlock(&store->mutex);
        return NULL;
    }
    _bind_text(stmt, 1, id); if (tenant) _bind_text(stmt, 2, tenant);
    JsonObject *object = sqlite3_step(stmt) == SQLITE_ROW ? _vpc_row_to_json(stmt) : NULL;
    sqlite3_finalize(stmt);
    if (!object)
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_NOT_FOUND, "VPC를 찾을 수 없습니다");
    g_mutex_unlock(&store->mutex);
    return object;
}

static JsonObject *
_subnet_row_to_json(sqlite3_stmt *stmt)
{
    JsonObject *o = json_object_new();
    json_object_set_string_member(o, "id", (const gchar *)sqlite3_column_text(stmt, 0));
    json_object_set_string_member(o, "vpc_id", (const gchar *)sqlite3_column_text(stmt, 1));
    json_object_set_string_member(o, "name", (const gchar *)sqlite3_column_text(stmt, 2));
    json_object_set_string_member(o, "cidr", (const gchar *)sqlite3_column_text(stmt, 3));
    json_object_set_string_member(o, "gateway", (const gchar *)sqlite3_column_text(stmt, 4));
    json_object_set_string_member(o, "allocation_start", (const gchar *)sqlite3_column_text(stmt, 5));
    json_object_set_string_member(o, "allocation_end", (const gchar *)sqlite3_column_text(stmt, 6));
    json_object_set_int_member(o, "mtu", sqlite3_column_int(stmt, 7));
    json_object_set_string_member(o, "backend", (const gchar *)sqlite3_column_text(stmt, 8));
    json_object_set_string_member(o, "backend_ref", (const gchar *)sqlite3_column_text(stmt, 9));
    if (sqlite3_column_type(stmt, 10) == SQLITE_NULL)
        json_object_set_null_member(o, "bridge_name");
    else
        json_object_set_string_member(o, "bridge_name", (const gchar *)sqlite3_column_text(stmt, 10));
    json_object_set_string_member(o, "state", (const gchar *)sqlite3_column_text(stmt, 11));
    if (sqlite3_column_type(stmt, 12) == SQLITE_NULL)
        json_object_set_null_member(o, "last_error");
    else
        json_object_set_string_member(o, "last_error", (const gchar *)sqlite3_column_text(stmt, 12));
    return o;
}

JsonArray *
pcv_vpc_store_list_subnets(PcvVpcStore *store,
                           const gchar *vpc_id,
                           const gchar *tenant,
                           GError **error)
{
    if (!store) return NULL;
    JsonArray *array = json_array_new();
    g_mutex_lock(&store->mutex);
    sqlite3_stmt *stmt = NULL;
    const gchar *sql =
        "SELECT s.id,s.vpc_id,s.name,s.cidr,s.gateway,s.allocation_start,s.allocation_end,"
        "s.mtu,v.backend,s.backend_ref,s.bridge_name,s.state,s.last_error "
        "FROM subnets s JOIN vpcs v ON v.id=s.vpc_id "
        "WHERE (? IS NULL OR s.vpc_id=?) AND (? IS NULL OR v.tenant=?) ORDER BY s.name";
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        _set_sql_error(store, error, "subnet 목록 준비 실패");
        g_mutex_unlock(&store->mutex); json_array_unref(array); return NULL;
    }
    if (vpc_id) { _bind_text(stmt, 1, vpc_id); _bind_text(stmt, 2, vpc_id); }
    else { sqlite3_bind_null(stmt, 1); sqlite3_bind_null(stmt, 2); }
    if (tenant) { _bind_text(stmt, 3, tenant); _bind_text(stmt, 4, tenant); }
    else { sqlite3_bind_null(stmt, 3); sqlite3_bind_null(stmt, 4); }
    while (sqlite3_step(stmt) == SQLITE_ROW)
        json_array_add_object_element(array, _subnet_row_to_json(stmt));
    sqlite3_finalize(stmt); g_mutex_unlock(&store->mutex);
    return array;
}

JsonObject *
pcv_vpc_store_get_subnet(PcvVpcStore *store,
                         const gchar *id,
                         const gchar *tenant,
                         GError **error)
{
    if (!store || !id) return NULL;
    g_mutex_lock(&store->mutex);
    sqlite3_stmt *stmt = NULL;
    const gchar *sql =
        "SELECT s.id,s.vpc_id,s.name,s.cidr,s.gateway,s.allocation_start,s.allocation_end,"
        "s.mtu,v.backend,s.backend_ref,s.bridge_name,s.state,s.last_error "
        "FROM subnets s JOIN vpcs v ON v.id=s.vpc_id "
        "WHERE s.id=? AND (? IS NULL OR v.tenant=?)";
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        _set_sql_error(store, error, "subnet 조회 준비 실패");
        g_mutex_unlock(&store->mutex); return NULL;
    }
    _bind_text(stmt, 1, id);
    if (tenant) { _bind_text(stmt, 2, tenant); _bind_text(stmt, 3, tenant); }
    else { sqlite3_bind_null(stmt, 2); sqlite3_bind_null(stmt, 3); }
    JsonObject *o = sqlite3_step(stmt) == SQLITE_ROW ? _subnet_row_to_json(stmt) : NULL;
    sqlite3_finalize(stmt); g_mutex_unlock(&store->mutex);
    if (!o) g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_NOT_FOUND, "subnet을 찾을 수 없습니다");
    return o;
}

static JsonObject *
_attachment_row_to_json(sqlite3_stmt *stmt)
{
    JsonObject *o = json_object_new();
    json_object_set_string_member(o, "id", (const gchar *)sqlite3_column_text(stmt, 0));
    json_object_set_string_member(o, "subnet_id", (const gchar *)sqlite3_column_text(stmt, 1));
    json_object_set_string_member(o, "vpc_id", (const gchar *)sqlite3_column_text(stmt, 2));
    json_object_set_string_member(o, "vm_uuid", (const gchar *)sqlite3_column_text(stmt, 3));
    json_object_set_string_member(o, "vm_name", (const gchar *)sqlite3_column_text(stmt, 4));
    json_object_set_string_member(o, "owner_subject", (const gchar *)sqlite3_column_text(stmt, 5));
    json_object_set_string_member(o, "mac_address", (const gchar *)sqlite3_column_text(stmt, 6));
    json_object_set_string_member(o, "ip_address", (const gchar *)sqlite3_column_text(stmt, 7));
    json_object_set_string_member(o, "backend", (const gchar *)sqlite3_column_text(stmt, 8));
    json_object_set_string_member(o, "backend_ref", (const gchar *)sqlite3_column_text(stmt, 9));
    if (sqlite3_column_type(stmt, 10) == SQLITE_NULL)
        json_object_set_null_member(o, "bridge_name");
    else
        json_object_set_string_member(o, "bridge_name", (const gchar *)sqlite3_column_text(stmt, 10));
    json_object_set_string_member(o, "state", (const gchar *)sqlite3_column_text(stmt, 11));
    if (sqlite3_column_type(stmt, 12) == SQLITE_NULL) json_object_set_null_member(o, "last_error");
    else json_object_set_string_member(o, "last_error", (const gchar *)sqlite3_column_text(stmt, 12));
    return o;
}

static JsonArray *
_attachments_query(PcvVpcStore *store,
                   const gchar *id,
                   const gchar *vpc_id,
                   const gchar *tenant,
                   GError **error)
{
    JsonArray *array = json_array_new();
    sqlite3_stmt *stmt = NULL;
    const gchar *sql =
        "SELECT a.id,a.subnet_id,s.vpc_id,a.vm_uuid,a.vm_name_snapshot,a.owner_subject,"
        "a.mac_address,a.ip_address,v.backend,s.backend_ref,s.bridge_name,a.state,a.last_error "
        "FROM attachments a "
        "JOIN subnets s ON s.id=a.subnet_id JOIN vpcs v ON v.id=s.vpc_id "
        "WHERE (? IS NULL OR a.id=?) AND (? IS NULL OR s.vpc_id=?) "
        "AND (? IS NULL OR v.tenant=?) ORDER BY a.created_at,a.id";
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        _set_sql_error(store, error, "attachment 조회 준비 실패"); json_array_unref(array); return NULL;
    }
    if (id) { _bind_text(stmt, 1, id); _bind_text(stmt, 2, id); }
    else { sqlite3_bind_null(stmt, 1); sqlite3_bind_null(stmt, 2); }
    if (vpc_id) { _bind_text(stmt, 3, vpc_id); _bind_text(stmt, 4, vpc_id); }
    else { sqlite3_bind_null(stmt, 3); sqlite3_bind_null(stmt, 4); }
    if (tenant) { _bind_text(stmt, 5, tenant); _bind_text(stmt, 6, tenant); }
    else { sqlite3_bind_null(stmt, 5); sqlite3_bind_null(stmt, 6); }
    while (sqlite3_step(stmt) == SQLITE_ROW)
        json_array_add_object_element(array, _attachment_row_to_json(stmt));
    sqlite3_finalize(stmt);
    return array;
}

JsonArray *
pcv_vpc_store_list_attachments(PcvVpcStore *store,
                               const gchar *vpc_id,
                               const gchar *tenant,
                               GError **error)
{
    if (!store) return NULL;
    g_mutex_lock(&store->mutex);
    JsonArray *array = _attachments_query(store, NULL, vpc_id, tenant, error);
    g_mutex_unlock(&store->mutex);
    return array;
}

JsonObject *
pcv_vpc_store_get_attachment(PcvVpcStore *store,
                             const gchar *id,
                             const gchar *tenant,
                             GError **error)
{
    if (!store || !id) return NULL;
    g_mutex_lock(&store->mutex);
    JsonArray *array = _attachments_query(store, id, NULL, tenant, error);
    JsonObject *o = NULL;
    if (array && json_array_get_length(array) == 1)
        o = json_object_ref(json_array_get_object_element(array, 0));
    if (array) json_array_unref(array);
    g_mutex_unlock(&store->mutex);
    if (!o && (!error || !*error))
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_NOT_FOUND, "attachment를 찾을 수 없습니다");
    return o;
}

static JsonObject *
_publish_row_to_json(sqlite3_stmt *stmt)
{
    JsonObject *o = json_object_new();
    json_object_set_string_member(o, "id", (const gchar *)sqlite3_column_text(stmt, 0));
    json_object_set_string_member(o, "vpc_id", (const gchar *)sqlite3_column_text(stmt, 1));
    json_object_set_string_member(o, "attachment_id", (const gchar *)sqlite3_column_text(stmt, 2));
    json_object_set_string_member(o, "protocol", (const gchar *)sqlite3_column_text(stmt, 3));
    json_object_set_string_member(o, "listen_address", (const gchar *)sqlite3_column_text(stmt, 4));
    json_object_set_int_member(o, "listen_port", sqlite3_column_int(stmt, 5));
    json_object_set_int_member(o, "target_port", sqlite3_column_int(stmt, 6));
    json_object_set_string_member(o, "allowed_sources_json", (const gchar *)sqlite3_column_text(stmt, 7));
    json_object_set_string_member(o, "target_ip", (const gchar *)sqlite3_column_text(stmt, 8));
    const gchar *backend = (const gchar *)sqlite3_column_text(stmt, 12);
    g_autofree gchar *edge_iface = g_strcmp0(backend, "ovn") == 0
        ? pcv_vpc_ovn_edge_iface_name_from_id((const gchar *)sqlite3_column_text(stmt, 1)) : NULL;
    json_object_set_string_member(o, "backend", backend);
    json_object_set_string_member(o, "target_bridge", edge_iface ? edge_iface
        : (const gchar *)sqlite3_column_text(stmt, 9));
    json_object_set_string_member(o, "state", (const gchar *)sqlite3_column_text(stmt, 10));
    if (sqlite3_column_type(stmt, 11) == SQLITE_NULL) json_object_set_null_member(o, "last_error");
    else json_object_set_string_member(o, "last_error", (const gchar *)sqlite3_column_text(stmt, 11));
    return o;
}

static JsonArray *
_publishes_query(PcvVpcStore *store,
                 const gchar *id,
                 const gchar *vpc_id,
                 const gchar *tenant,
                 GError **error)
{
    JsonArray *array = json_array_new();
    sqlite3_stmt *stmt = NULL;
    const gchar *sql =
        "SELECT p.id,p.vpc_id,p.attachment_id,p.protocol,p.listen_address,p.listen_port,"
        "p.target_port,p.allowed_sources,a.ip_address,s.bridge_name,p.state,p.last_error,v.backend "
        "FROM service_publishes p JOIN attachments a ON a.id=p.attachment_id "
        "JOIN subnets s ON s.id=a.subnet_id JOIN vpcs v ON v.id=p.vpc_id "
        "WHERE (? IS NULL OR p.id=?) AND (? IS NULL OR p.vpc_id=?) "
        "AND (? IS NULL OR v.tenant=?) ORDER BY p.created_at,p.id";
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        _set_sql_error(store, error, "Service Publish 조회 준비 실패"); json_array_unref(array); return NULL;
    }
    if (id) { _bind_text(stmt, 1, id); _bind_text(stmt, 2, id); }
    else { sqlite3_bind_null(stmt, 1); sqlite3_bind_null(stmt, 2); }
    if (vpc_id) { _bind_text(stmt, 3, vpc_id); _bind_text(stmt, 4, vpc_id); }
    else { sqlite3_bind_null(stmt, 3); sqlite3_bind_null(stmt, 4); }
    if (tenant) { _bind_text(stmt, 5, tenant); _bind_text(stmt, 6, tenant); }
    else { sqlite3_bind_null(stmt, 5); sqlite3_bind_null(stmt, 6); }
    while (sqlite3_step(stmt) == SQLITE_ROW)
        json_array_add_object_element(array, _publish_row_to_json(stmt));
    sqlite3_finalize(stmt);
    return array;
}

JsonArray *
pcv_vpc_store_list_publishes(PcvVpcStore *store,
                             const gchar *vpc_id,
                             const gchar *tenant,
                             GError **error)
{
    if (!store) return NULL;
    g_mutex_lock(&store->mutex);
    JsonArray *array = _publishes_query(store, NULL, vpc_id, tenant, error);
    g_mutex_unlock(&store->mutex);
    return array;
}

JsonObject *
pcv_vpc_store_get_publish(PcvVpcStore *store,
                          const gchar *id,
                          const gchar *tenant,
                          GError **error)
{
    if (!store || !id) return NULL;
    g_mutex_lock(&store->mutex);
    JsonArray *array = _publishes_query(store, id, NULL, tenant, error);
    JsonObject *o = NULL;
    if (array && json_array_get_length(array) == 1)
        o = json_object_ref(json_array_get_object_element(array, 0));
    if (array) json_array_unref(array);
    g_mutex_unlock(&store->mutex);
    if (!o && (!error || !*error))
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_NOT_FOUND,
                    "Service Publish를 찾을 수 없습니다");
    return o;
}

static JsonObject *
_binding_row_to_json(sqlite3_stmt *stmt)
{
    JsonObject *object = json_object_new();
    json_object_set_string_member(object, "vpc_id",
        (const gchar *)sqlite3_column_text(stmt, 0));
    json_object_set_string_member(object, "backend_ref",
        (const gchar *)sqlite3_column_text(stmt, 1));
    json_object_set_string_member(object, "edge_cidr",
        (const gchar *)sqlite3_column_text(stmt, 2));
    json_object_set_string_member(object, "host_edge_ip",
        (const gchar *)sqlite3_column_text(stmt, 3));
    json_object_set_string_member(object, "router_edge_ip",
        (const gchar *)sqlite3_column_text(stmt, 4));
    json_object_set_int_member(object, "generation", sqlite3_column_int64(stmt, 5));
    json_object_set_int_member(object, "actual_revision", sqlite3_column_int64(stmt, 6));
    return object;
}

static JsonObject *
_get_binding_locked(PcvVpcStore *store, const gchar *vpc_id)
{
    sqlite3_stmt *stmt = NULL;
    JsonObject *object = NULL;
    if (sqlite3_prepare_v2(store->db,
            "SELECT vpc_id,backend_ref,edge_cidr,host_edge_ip,router_edge_ip,generation,"
            "actual_revision FROM vpc_backend_bindings WHERE vpc_id=?",
            -1, &stmt, NULL) == SQLITE_OK) {
        _bind_text(stmt, 1, vpc_id);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            object = _binding_row_to_json(stmt);
    }
    if (stmt) sqlite3_finalize(stmt);
    return object;
}

JsonObject *
pcv_vpc_store_get_backend_binding(PcvVpcStore *store,
                                  const gchar *vpc_id,
                                  const gchar *tenant,
                                  GError **error)
{
    if (!store || !vpc_id) return NULL;
    g_mutex_lock(&store->mutex);
    gboolean found = _lookup_vpc_locked(
        store, vpc_id, tenant, NULL, NULL, NULL, error);
    JsonObject *object = found ? _get_binding_locked(store, vpc_id) : NULL;
    if (found && !object)
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_NOT_FOUND,
                    "VPC backend binding을 찾을 수 없습니다");
    g_mutex_unlock(&store->mutex);
    return object;
}

JsonObject *
pcv_vpc_store_ensure_ovn_binding(PcvVpcStore *store,
                                 const gchar *vpc_id,
                                 const gchar *tenant,
                                 const gchar *transit_pool,
                                 GError **error)
{
    PcvVpcIpv4Cidr pool = {0};
    g_autofree gchar *canonical = NULL;
    PcvVpcIpv4Cidr shared = {0};
    if (!store || !vpc_id ||
        !pcv_vpc_cidr_parse(transit_pool, &pool, &canonical, error) ||
        g_strcmp0(canonical, transit_pool) != 0 || pool.prefix < 16 || pool.prefix > 28 ||
        !pcv_vpc_cidr_parse("100.64.0.0/10", &shared, NULL, NULL) ||
        pool.network < shared.network || pool.broadcast > shared.broadcast) {
        if (error && !*error)
            g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_INVALID_ARGUMENT,
                        "OVN edge transit pool은 canonical RFC6598 /16~/28이어야 합니다");
        return NULL;
    }

    g_mutex_lock(&store->mutex);
    if (!_begin(store, error)) { g_mutex_unlock(&store->mutex); return NULL; }
    g_autofree gchar *backend = NULL;
    gint64 revision = 0;
    gboolean ok = _lookup_vpc_locked(
        store, vpc_id, tenant, NULL, &backend, &revision, error);
    if (ok && g_strcmp0(backend, "ovn") != 0) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_STATE,
                    "Linux VPC에는 OVN backend binding을 만들 수 없습니다");
        ok = FALSE;
    }
    JsonObject *object = ok ? _get_binding_locked(store, vpc_id) : NULL;
    guint32 selected = 0;
    for (guint32 network = pool.network; ok && !object && network <= pool.broadcast - 3;
         network += 4) {
        g_autofree gchar *candidate_ip = pcv_vpc_ipv4_to_string(network);
        g_autofree gchar *candidate = g_strdup_printf("%s/30", candidate_ip);
        sqlite3_stmt *used = NULL;
        gboolean prepared = sqlite3_prepare_v2(store->db,
            "SELECT 1 FROM vpc_backend_bindings WHERE edge_cidr=? LIMIT 1",
            -1, &used, NULL) == SQLITE_OK;
        if (prepared) {
            _bind_text(used, 1, candidate);
            if (sqlite3_step(used) != SQLITE_ROW)
                selected = network;
        } else {
            _set_sql_error(store, error, "OVN edge 예약 조회 실패");
            ok = FALSE;
        }
        if (used) sqlite3_finalize(used);
        if (selected || network > G_MAXUINT32 - 4)
            break;
    }
    if (ok && !object && selected == 0) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT,
                    "OVN edge transit pool이 소진되었습니다");
        ok = FALSE;
    }
    if (ok && !object) {
        g_autofree gchar *network_ip = pcv_vpc_ipv4_to_string(selected);
        g_autofree gchar *edge_cidr = g_strdup_printf("%s/30", network_ip);
        g_autofree gchar *host_ip = pcv_vpc_ipv4_to_string(selected + 1);
        g_autofree gchar *router_ip = pcv_vpc_ipv4_to_string(selected + 2);
        g_autofree gchar *router = pcv_vpc_ovn_router_name_from_id(vpc_id);
        sqlite3_stmt *insert = NULL;
        ok = sqlite3_prepare_v2(store->db,
            "INSERT INTO vpc_backend_bindings(vpc_id,backend_ref,edge_cidr,host_edge_ip,"
            "router_edge_ip,generation) VALUES(?,?,?,?,?,?)",
            -1, &insert, NULL) == SQLITE_OK;
        if (ok) {
            _bind_text(insert, 1, vpc_id); _bind_text(insert, 2, router);
            _bind_text(insert, 3, edge_cidr); _bind_text(insert, 4, host_ip);
            _bind_text(insert, 5, router_ip); sqlite3_bind_int64(insert, 6, revision);
            ok = sqlite3_step(insert) == SQLITE_DONE;
        }
        if (!ok) _set_sql_error(store, error, "OVN edge binding 생성 실패");
        if (insert) sqlite3_finalize(insert);
        if (ok) object = _get_binding_locked(store, vpc_id);
    }
    if (ok) ok = _commit(store, error); else _rollback(store);
    g_mutex_unlock(&store->mutex);
    if (!ok) g_clear_pointer(&object, json_object_unref);
    return object;
}

gboolean
pcv_vpc_store_create_subnet(PcvVpcStore *store,
                            const gchar *vpc_id,
                            const gchar *tenant,
                            const gchar *name,
                            const gchar *cidr,
                            gint mtu,
                            gint64 expected_revision,
                            gchar **id_out,
                            gchar **bridge_out,
                            gint64 *revision_out,
                            GError **error)
{
    if (id_out)
        *id_out = NULL;
    if (bridge_out)
        *bridge_out = NULL;
    PcvVpcIpv4Cidr parsed = {0};
    g_autofree gchar *canonical = NULL;
    if (!store || !vpc_id || !pcv_vpc_name_is_valid(name) || mtu < 68 || mtu > 9216) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_INVALID_ARGUMENT,
                    "subnet 필수 값, 이름 또는 MTU가 유효하지 않습니다");
        return FALSE;
    }
    if (!pcv_vpc_subnet_cidr_parse(cidr, &parsed, &canonical, error))
        return FALSE;
    guint32 first = 0, last = 0;
    pcv_vpc_cidr_usable_range(&parsed, &first, &last);
    guint32 gateway_value = first;
    guint32 pool_start_value = first + 1;
    g_autofree gchar *gateway = pcv_vpc_ipv4_to_string(gateway_value);
    g_autofree gchar *pool_start = pcv_vpc_ipv4_to_string(pool_start_value);
    g_autofree gchar *pool_end = pcv_vpc_ipv4_to_string(last);
    g_autofree gchar *id = g_uuid_string_random();
    g_autofree gchar *bridge = NULL;
    g_autofree gchar *backend_ref = NULL;

    g_mutex_lock(&store->mutex);
    if (!_begin(store, error)) { g_mutex_unlock(&store->mutex); return FALSE; }
    gint64 revision = 0;
    g_autofree gchar *backend = NULL;
    gboolean ok = _lookup_vpc_locked(store, vpc_id, tenant, NULL, &backend, &revision, error) &&
                  _check_revision(revision, expected_revision, error);
    if (ok && g_strcmp0(backend, "linux") == 0) {
        bridge = pcv_vpc_bridge_name_from_id(id);
        backend_ref = g_strdup(bridge);
    } else if (ok) {
        backend_ref = pcv_vpc_ovn_switch_name_from_id(id);
    }
    sqlite3_stmt *stmt = NULL;
    if (ok && sqlite3_prepare_v2(store->db,
            "SELECT cidr FROM subnets WHERE network_start<=? AND network_end>=? LIMIT 1",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, parsed.broadcast);
        sqlite3_bind_int64(stmt, 2, parsed.network);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT,
                        "기존 subnet CIDR과 중첩됩니다: %s", sqlite3_column_text(stmt, 0));
            ok = FALSE;
        }
    } else if (ok) { _set_sql_error(store, error, "CIDR 중첩 조회 준비 실패"); ok = FALSE; }
    if (stmt) sqlite3_finalize(stmt);
    stmt = NULL;
    const gchar *insert =
        "INSERT INTO subnets(id,vpc_id,name,cidr,network_start,network_end,prefix,gateway,"
        "allocation_start,allocation_end,mtu,backend_ref,bridge_name,state) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?, 'CREATING')";
    if (ok && sqlite3_prepare_v2(store->db, insert, -1, &stmt, NULL) == SQLITE_OK) {
        _bind_text(stmt, 1, id); _bind_text(stmt, 2, vpc_id); _bind_text(stmt, 3, name);
        _bind_text(stmt, 4, canonical); sqlite3_bind_int64(stmt, 5, parsed.network);
        sqlite3_bind_int64(stmt, 6, parsed.broadcast); sqlite3_bind_int(stmt, 7, parsed.prefix);
        _bind_text(stmt, 8, gateway); _bind_text(stmt, 9, pool_start); _bind_text(stmt, 10, pool_end);
        sqlite3_bind_int(stmt, 11, mtu); _bind_text(stmt, 12, backend_ref);
        if (bridge) _bind_text(stmt, 13, bridge); else sqlite3_bind_null(stmt, 13);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        if (!ok) _set_sql_error(store, error, "subnet 생성 실패");
    } else if (ok) { _set_sql_error(store, error, "subnet 생성 준비 실패"); ok = FALSE; }
    if (stmt) sqlite3_finalize(stmt);
    if (ok)
        ok = _bump_vpc_revision_locked(store, vpc_id, error);
    if (ok) ok = _commit(store, error); else _rollback(store);
    g_mutex_unlock(&store->mutex);
    if (ok) {
        if (id_out) *id_out = g_steal_pointer(&id);
        if (bridge_out) *bridge_out = g_strdup(backend_ref);
        if (revision_out) *revision_out = revision + 1;
    }
    return ok;
}

gboolean
pcv_vpc_store_delete_subnet(PcvVpcStore *store,
                            const gchar *id,
                            const gchar *tenant,
                            GError **error)
{
    if (!store || !id) return FALSE;
    g_mutex_lock(&store->mutex);
    if (!_begin(store, error)) { g_mutex_unlock(&store->mutex); return FALSE; }
    sqlite3_stmt *stmt = NULL;
    const gchar *lookup =
        "SELECT s.vpc_id FROM subnets s JOIN vpcs v ON v.id=s.vpc_id "
        "WHERE s.id=? AND (? IS NULL OR v.tenant=?)";
    gboolean ok = sqlite3_prepare_v2(store->db, lookup, -1, &stmt, NULL) == SQLITE_OK;
    if (ok) {
        _bind_text(stmt, 1, id);
        if (tenant) { _bind_text(stmt, 2, tenant); _bind_text(stmt, 3, tenant); }
        else { sqlite3_bind_null(stmt, 2); sqlite3_bind_null(stmt, 3); }
        ok = sqlite3_step(stmt) == SQLITE_ROW;
    }
    g_autofree gchar *vpc_id = ok ? g_strdup((const gchar *)sqlite3_column_text(stmt, 0)) : NULL;
    if (stmt) sqlite3_finalize(stmt);
    if (!ok) g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_NOT_FOUND, "subnet을 찾을 수 없습니다");
    stmt = NULL;
    if (ok && sqlite3_prepare_v2(store->db,
            "SELECT 1 FROM attachments WHERE subnet_id=? LIMIT 1", -1, &stmt, NULL) == SQLITE_OK) {
        _bind_text(stmt, 1, id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT,
                        "attachment가 있는 subnet은 삭제할 수 없습니다"); ok = FALSE;
        }
    } else if (ok) ok = FALSE;
    if (stmt) sqlite3_finalize(stmt);
    stmt = NULL;
    if (ok && sqlite3_prepare_v2(store->db,
            "DELETE FROM subnets WHERE id=?", -1, &stmt, NULL) == SQLITE_OK) {
        _bind_text(stmt, 1, id); ok = sqlite3_step(stmt) == SQLITE_DONE;
    } else if (ok) ok = FALSE;
    if (stmt) sqlite3_finalize(stmt);
    if (ok)
        ok = _bump_vpc_revision_locked(store, vpc_id, error);
    if (ok) ok = _commit(store, error); else _rollback(store);
    g_mutex_unlock(&store->mutex);
    return ok;
}

static gboolean
_ip_is_allocated_locked(PcvVpcStore *store, const gchar *ip)
{
    sqlite3_stmt *stmt = NULL;
    gboolean used = TRUE;
    if (sqlite3_prepare_v2(store->db,
            "SELECT 1 FROM attachments WHERE ip_address=? LIMIT 1", -1, &stmt, NULL) == SQLITE_OK) {
        _bind_text(stmt, 1, ip); used = sqlite3_step(stmt) == SQLITE_ROW;
    }
    if (stmt) sqlite3_finalize(stmt);
    return used;
}

gboolean
pcv_vpc_store_allocate_attachment(PcvVpcStore *store,
                                  const gchar *subnet_id,
                                  const gchar *tenant,
                                  const gchar *vm_uuid,
                                  const gchar *vm_name,
                                  const gchar *owner_subject,
                                  const gchar *requested_ip,
                                  JsonObject **attachment_out,
                                  GError **error)
{
    if (attachment_out) *attachment_out = NULL;
    if (!store || !subnet_id || !vm_uuid || !vm_name || !owner_subject) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_INVALID_ARGUMENT,
                    "attachment 필수 값이 없습니다"); return FALSE;
    }
    g_mutex_lock(&store->mutex);
    if (!_begin(store, error)) { g_mutex_unlock(&store->mutex); return FALSE; }
    sqlite3_stmt *stmt = NULL;
    const gchar *sql =
        "SELECT s.vpc_id,s.network_start,s.network_end,v.backend,s.backend_ref,s.bridge_name "
        "FROM subnets s JOIN vpcs v ON v.id=s.vpc_id WHERE s.id=? AND (? IS NULL OR v.tenant=?)";
    gboolean ok = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) == SQLITE_OK;
    if (ok) {
        _bind_text(stmt, 1, subnet_id);
        if (tenant) { _bind_text(stmt, 2, tenant); _bind_text(stmt, 3, tenant); }
        else { sqlite3_bind_null(stmt, 2); sqlite3_bind_null(stmt, 3); }
        ok = sqlite3_step(stmt) == SQLITE_ROW;
    }
    g_autofree gchar *vpc_id = ok ? g_strdup((const gchar *)sqlite3_column_text(stmt, 0)) : NULL;
    guint32 start = ok ? (guint32)sqlite3_column_int64(stmt, 1) : 0;
    guint32 end = ok ? (guint32)sqlite3_column_int64(stmt, 2) : 0;
    g_autofree gchar *backend = ok ? g_strdup((const gchar *)sqlite3_column_text(stmt, 3)) : NULL;
    g_autofree gchar *backend_ref = ok ? g_strdup((const gchar *)sqlite3_column_text(stmt, 4)) : NULL;
    g_autofree gchar *bridge = ok && sqlite3_column_type(stmt, 5) != SQLITE_NULL
        ? g_strdup((const gchar *)sqlite3_column_text(stmt, 5)) : NULL;
    if (stmt) sqlite3_finalize(stmt);
    if (!ok) g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_NOT_FOUND, "subnet을 찾을 수 없습니다");

                                                                         
    stmt = NULL;
    if (ok && sqlite3_prepare_v2(store->db,
            "SELECT s.vpc_id FROM attachments a JOIN subnets s ON s.id=a.subnet_id "
            "WHERE a.vm_uuid=? AND s.vpc_id<>? LIMIT 1", -1, &stmt, NULL) == SQLITE_OK) {
        _bind_text(stmt, 1, vm_uuid); _bind_text(stmt, 2, vpc_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT,
                        "VM은 서로 다른 VPC에 동시에 연결할 수 없습니다"); ok = FALSE;
        }
    } else if (ok) ok = FALSE;
    if (stmt) sqlite3_finalize(stmt);

    g_autofree gchar *ip = NULL;
    if (ok && requested_ip) {
        PcvVpcIpv4Cidr range = { .network = start, .broadcast = end, .prefix = 0 };
        g_autofree gchar *host_cidr = g_strdup_printf("%s/32", requested_ip);
        PcvVpcIpv4Cidr host = {0};
        if (!pcv_vpc_cidr_parse(host_cidr, &host, NULL, NULL) ||
            !pcv_vpc_cidr_contains_ip(&range, requested_ip) || host.network <= start + 1 ||
            host.network >= end || _ip_is_allocated_locked(store, requested_ip)) {
            g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT,
                        "요청 IP를 할당할 수 없습니다"); ok = FALSE;
        } else ip = g_strdup(requested_ip);
    }
    if (ok && !ip) {
        for (guint32 candidate = start + 2; candidate < end; candidate++) {
            g_autofree gchar *text = pcv_vpc_ipv4_to_string(candidate);
            if (!_ip_is_allocated_locked(store, text)) { ip = g_strdup(text); break; }
            if (candidate == G_MAXUINT32) break;
        }
        if (!ip) { g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT,
                               "subnet IP pool이 소진되었습니다"); ok = FALSE; }
    }
    g_autofree gchar *id = ok ? g_uuid_string_random() : NULL;
    g_autofree gchar *mac = ok ? pcv_vpc_mac_from_id(id) : NULL;
    stmt = NULL;
    if (ok && sqlite3_prepare_v2(store->db,
            "INSERT INTO attachments(id,subnet_id,vm_uuid,vm_name_snapshot,owner_subject,"
            "mac_address,ip_address,state) VALUES(?,?,?,?,?,?,?, 'ALLOCATED')",
            -1, &stmt, NULL) == SQLITE_OK) {
        _bind_text(stmt, 1, id); _bind_text(stmt, 2, subnet_id); _bind_text(stmt, 3, vm_uuid);
        _bind_text(stmt, 4, vm_name); _bind_text(stmt, 5, owner_subject);
        _bind_text(stmt, 6, mac); _bind_text(stmt, 7, ip);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        if (!ok) _set_sql_error(store, error, "attachment 예약 실패");
    } else if (ok) ok = FALSE;
    if (stmt) sqlite3_finalize(stmt);
    if (ok)
        ok = _bump_vpc_revision_locked(store, vpc_id, error);
    if (ok) ok = _commit(store, error); else _rollback(store);
    g_mutex_unlock(&store->mutex);
    if (!ok) return FALSE;
    JsonObject *object = json_object_new();
    json_object_set_string_member(object, "id", id);
    json_object_set_string_member(object, "vpc_id", vpc_id);
    json_object_set_string_member(object, "subnet_id", subnet_id);
    json_object_set_string_member(object, "vm_uuid", vm_uuid);
    json_object_set_string_member(object, "vm_name", vm_name);
    json_object_set_string_member(object, "owner_subject", owner_subject);
    json_object_set_string_member(object, "mac_address", mac);
    json_object_set_string_member(object, "ip_address", ip);
    json_object_set_string_member(object, "backend", backend);
    json_object_set_string_member(object, "backend_ref", backend_ref);
    if (bridge) json_object_set_string_member(object, "bridge_name", bridge);
    else json_object_set_null_member(object, "bridge_name");
    json_object_set_string_member(object, "state", "ALLOCATED");
    if (attachment_out) *attachment_out = object; else json_object_unref(object);
    return TRUE;
}

gboolean
pcv_vpc_store_delete_attachment(PcvVpcStore *store,
                                const gchar *id,
                                const gchar *tenant,
                                GError **error)
{
    if (!store || !id) return FALSE;
    g_mutex_lock(&store->mutex);
    if (!_begin(store, error)) { g_mutex_unlock(&store->mutex); return FALSE; }
    sqlite3_stmt *stmt = NULL;
    const gchar *lookup =
        "SELECT s.vpc_id FROM attachments a JOIN subnets s ON s.id=a.subnet_id "
        "JOIN vpcs v ON v.id=s.vpc_id WHERE a.id=? AND (? IS NULL OR v.tenant=?)";
    gboolean ok = sqlite3_prepare_v2(store->db, lookup, -1, &stmt, NULL) == SQLITE_OK;
    if (ok) {
        _bind_text(stmt, 1, id);
        if (tenant) { _bind_text(stmt, 2, tenant); _bind_text(stmt, 3, tenant); }
        else { sqlite3_bind_null(stmt, 2); sqlite3_bind_null(stmt, 3); }
        ok = sqlite3_step(stmt) == SQLITE_ROW;
    }
    g_autofree gchar *vpc_id = ok ? g_strdup((const gchar *)sqlite3_column_text(stmt, 0)) : NULL;
    if (stmt) sqlite3_finalize(stmt);
    if (!ok) g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_NOT_FOUND, "attachment를 찾을 수 없습니다");
    stmt = NULL;
    if (ok && sqlite3_prepare_v2(store->db,
            "SELECT 1 FROM service_publishes WHERE attachment_id=? LIMIT 1", -1, &stmt, NULL) == SQLITE_OK) {
        _bind_text(stmt, 1, id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT,
                        "Service Publish가 참조하는 attachment는 삭제할 수 없습니다"); ok = FALSE;
        }
    } else if (ok) ok = FALSE;
    if (stmt) sqlite3_finalize(stmt);
    stmt = NULL;
    if (ok && sqlite3_prepare_v2(store->db,
            "DELETE FROM attachments WHERE id=?", -1, &stmt, NULL) == SQLITE_OK) {
        _bind_text(stmt, 1, id); ok = sqlite3_step(stmt) == SQLITE_DONE;
    } else if (ok) ok = FALSE;
    if (stmt) sqlite3_finalize(stmt);
    if (ok)
        ok = _bump_vpc_revision_locked(store, vpc_id, error);
    if (ok) ok = _commit(store, error); else _rollback(store);
    g_mutex_unlock(&store->mutex);
    return ok;
}

static gchar *
_sources_to_json(GPtrArray *sources)
{
    JsonArray *array = json_array_new();
    for (guint i = 0; sources && i < sources->len; i++)
        json_array_add_string_element(array, g_ptr_array_index(sources, i));
    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, array);
    JsonGenerator *generator = json_generator_new();
    json_generator_set_root(generator, node);
    gchar *text = json_generator_to_data(generator, NULL);
    g_object_unref(generator); json_node_free(node);
    return text;
}

gboolean
pcv_vpc_store_create_publish(PcvVpcStore *store,
                             const gchar *attachment_id,
                             const gchar *tenant,
                             const gchar *protocol,
                             const gchar *listen_address,
                             gint listen_port,
                             gint target_port,
                             GPtrArray *allowed_sources,
                             gchar **id_out,
                             GError **error)
{
    if (id_out) *id_out = NULL;
    if (!store || !attachment_id || !pcv_vpc_protocol_is_valid(protocol) ||
        !pcv_vpc_port_is_valid(listen_port) || !pcv_vpc_port_is_valid(target_port) ||
        !allowed_sources || allowed_sources->len == 0) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_INVALID_ARGUMENT,
                    "Service Publish 입력이 유효하지 않습니다"); return FALSE;
    }
    PcvVpcIpv4Cidr address_cidr = {0};
    g_autofree gchar *listen_cidr = g_strdup_printf("%s/32", listen_address);
    if (!pcv_vpc_cidr_parse(listen_cidr, &address_cidr, NULL, error)) return FALSE;
    for (guint i = 0; i < allowed_sources->len; i++) {
        PcvVpcIpv4Cidr source = {0}; g_autofree gchar *canonical = NULL;
        const gchar *source_text = g_ptr_array_index(allowed_sources, i);
        if (!pcv_vpc_cidr_parse(source_text, &source, &canonical, error))
            return FALSE;
        if (g_strcmp0(canonical, source_text) != 0) {
            g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_INVALID_ARGUMENT,
                        "allowed source CIDR은 canonical 형식이어야 합니다: %s", source_text);
            return FALSE;
        }
    }
    g_autofree gchar *sources_json = _sources_to_json(allowed_sources);
    g_autofree gchar *id = g_uuid_string_random();
    g_mutex_lock(&store->mutex);
    if (!_begin(store, error)) { g_mutex_unlock(&store->mutex); return FALSE; }
    sqlite3_stmt *stmt = NULL;
    const gchar *lookup =
        "SELECT s.vpc_id,v.egress_mode,a.state FROM attachments a "
        "JOIN subnets s ON s.id=a.subnet_id JOIN vpcs v ON v.id=s.vpc_id "
        "WHERE a.id=? AND (? IS NULL OR v.tenant=?)";
    gboolean ok = sqlite3_prepare_v2(store->db, lookup, -1, &stmt, NULL) == SQLITE_OK;
    if (ok) {
        _bind_text(stmt, 1, attachment_id);
        if (tenant) { _bind_text(stmt, 2, tenant); _bind_text(stmt, 3, tenant); }
        else { sqlite3_bind_null(stmt, 2); sqlite3_bind_null(stmt, 3); }
        ok = sqlite3_step(stmt) == SQLITE_ROW;
    }
    g_autofree gchar *vpc_id = ok ? g_strdup((const gchar *)sqlite3_column_text(stmt, 0)) : NULL;
    g_autofree gchar *mode = ok ? g_strdup((const gchar *)sqlite3_column_text(stmt, 1)) : NULL;
    g_autofree gchar *state = ok ? g_strdup((const gchar *)sqlite3_column_text(stmt, 2)) : NULL;
    if (stmt) sqlite3_finalize(stmt);
    if (!ok) g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_NOT_FOUND, "attachment를 찾을 수 없습니다");
    if (ok && (g_strcmp0(mode, "nat") != 0 || g_strcmp0(state, "ACTIVE") != 0)) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_STATE,
                    "ACTIVE attachment가 있는 NAT VPC에서만 publish할 수 있습니다"); ok = FALSE;
    }
    stmt = NULL;
    if (ok && sqlite3_prepare_v2(store->db,
            "SELECT 1 FROM service_publishes WHERE protocol=? AND listen_port=? "
            "AND (listen_address=? OR listen_address='0.0.0.0' OR ?='0.0.0.0') LIMIT 1",
            -1, &stmt, NULL) == SQLITE_OK) {
        _bind_text(stmt, 1, protocol); sqlite3_bind_int(stmt, 2, listen_port);
        _bind_text(stmt, 3, listen_address); _bind_text(stmt, 4, listen_address);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT,
                        "wildcard를 포함한 host address/port/protocol publish 충돌입니다");
            ok = FALSE;
        }
    } else if (ok) {
        _set_sql_error(store, error, "Service Publish 충돌 조회 실패");
        ok = FALSE;
    }
    if (stmt) sqlite3_finalize(stmt);
    stmt = NULL;
    const gchar *insert =
        "INSERT INTO service_publishes(id,vpc_id,attachment_id,protocol,listen_address,listen_port,"
        "target_port,allowed_sources,state) VALUES(?,?,?,?,?,?,?,?, 'CREATING')";
    if (ok && sqlite3_prepare_v2(store->db, insert, -1, &stmt, NULL) == SQLITE_OK) {
        _bind_text(stmt, 1, id); _bind_text(stmt, 2, vpc_id); _bind_text(stmt, 3, attachment_id);
        _bind_text(stmt, 4, protocol); _bind_text(stmt, 5, listen_address);
        sqlite3_bind_int(stmt, 6, listen_port); sqlite3_bind_int(stmt, 7, target_port);
        _bind_text(stmt, 8, sources_json); ok = sqlite3_step(stmt) == SQLITE_DONE;
        if (!ok) {
            if (sqlite3_extended_errcode(store->db) == SQLITE_CONSTRAINT_UNIQUE)
                g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT,
                            "host address/port/protocol publish가 이미 있습니다");
            else _set_sql_error(store, error, "Service Publish 생성 실패");
        }
    } else if (ok) {
        _set_sql_error(store, error, "Service Publish 생성 준비 실패");
        ok = FALSE;
    }
    if (stmt) sqlite3_finalize(stmt);
    if (ok)
        ok = _bump_vpc_revision_locked(store, vpc_id, error);
    if (ok) ok = _commit(store, error); else _rollback(store);
    g_mutex_unlock(&store->mutex);
    if (ok && id_out) *id_out = g_steal_pointer(&id);
    return ok;
}

gboolean
pcv_vpc_store_delete_publish(PcvVpcStore *store,
                             const gchar *id,
                             const gchar *tenant,
                             GError **error)
{
    if (!store || !id) return FALSE;
    g_mutex_lock(&store->mutex);
    if (!_begin(store, error)) { g_mutex_unlock(&store->mutex); return FALSE; }
    sqlite3_stmt *stmt = NULL;
    const gchar *lookup =
        "SELECT p.vpc_id FROM service_publishes p JOIN vpcs v ON v.id=p.vpc_id "
        "WHERE p.id=? AND (? IS NULL OR v.tenant=?)";
    gboolean ok = sqlite3_prepare_v2(store->db, lookup, -1, &stmt, NULL) == SQLITE_OK && stmt;
    if (ok) {
        _bind_text(stmt, 1, id);
        if (tenant) { _bind_text(stmt, 2, tenant); _bind_text(stmt, 3, tenant); }
        else { sqlite3_bind_null(stmt, 2); sqlite3_bind_null(stmt, 3); }
        ok = sqlite3_step(stmt) == SQLITE_ROW;
    }
    g_autofree gchar *vpc_id = ok
        ? g_strdup((const gchar *)sqlite3_column_text(stmt, 0)) : NULL;
    if (stmt) sqlite3_finalize(stmt);
    if (!ok && (!error || !*error))
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_NOT_FOUND,
                    "Service Publish를 찾을 수 없습니다");
    stmt = NULL;
    if (ok && sqlite3_prepare_v2(store->db,
            "DELETE FROM service_publishes WHERE id=?", -1, &stmt, NULL) == SQLITE_OK) {
        _bind_text(stmt, 1, id);
        ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(store->db) == 1;
    } else if (ok) {
        _set_sql_error(store, error, "Service Publish 삭제 준비 실패");
        ok = FALSE;
    }
    if (stmt) sqlite3_finalize(stmt);
    if (ok)
        ok = _bump_vpc_revision_locked(store, vpc_id, error);
    if (ok) ok = _commit(store, error); else _rollback(store);
    g_mutex_unlock(&store->mutex);
    return ok;
}

gboolean
pcv_vpc_store_set_resource_state(PcvVpcStore *store,
                                 const gchar *table,
                                 const gchar *id,
                                 const gchar *state,
                                 const gchar *last_error,
                                 GError **error)
{
    if (!store || !id || !state ||
        (g_strcmp0(table, "vpcs") != 0 && g_strcmp0(table, "subnets") != 0 &&
         g_strcmp0(table, "attachments") != 0 && g_strcmp0(table, "service_publishes") != 0)) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_INVALID_ARGUMENT,
                    "resource state 입력이 유효하지 않습니다"); return FALSE;
    }
    const gchar *lookup_sql = NULL;
    if (g_strcmp0(table, "vpcs") == 0)
        lookup_sql = "SELECT state,id FROM vpcs WHERE id=?";
    else if (g_strcmp0(table, "subnets") == 0)
        lookup_sql = "SELECT state,vpc_id FROM subnets WHERE id=?";
    else if (g_strcmp0(table, "attachments") == 0)
        lookup_sql =
            "SELECT a.state,s.vpc_id FROM attachments a "
            "JOIN subnets s ON s.id=a.subnet_id WHERE a.id=?";
    else
        lookup_sql = "SELECT state,vpc_id FROM service_publishes WHERE id=?";

    g_autofree gchar *sql = g_strdup_printf(
        "UPDATE %s SET state=?,last_error=?,updated_at=unixepoch() WHERE id=?", table);
    g_mutex_lock(&store->mutex);
    if (!_begin(store, error)) {
        g_mutex_unlock(&store->mutex);
        return FALSE;
    }
    sqlite3_stmt *stmt = NULL;
    gboolean ok = sqlite3_prepare_v2(store->db, lookup_sql, -1, &stmt, NULL) == SQLITE_OK;
    if (ok) {
        _bind_text(stmt, 1, id);
        ok = sqlite3_step(stmt) == SQLITE_ROW;
    }
    g_autofree gchar *old_state = ok
        ? g_strdup((const gchar *)sqlite3_column_text(stmt, 0)) : NULL;
    g_autofree gchar *vpc_id = ok
        ? g_strdup((const gchar *)sqlite3_column_text(stmt, 1)) : NULL;
    if (stmt) sqlite3_finalize(stmt);
    stmt = NULL;
    if (!ok) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_NOT_FOUND,
                    "state를 변경할 VPC resource를 찾을 수 없습니다");
    } else if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK || !stmt) {
        _set_sql_error(store, error, "resource state 변경 준비 실패");
        ok = FALSE;
    }
    if (ok) {
        _bind_text(stmt, 1, state);
        if (last_error) _bind_text(stmt, 2, last_error); else sqlite3_bind_null(stmt, 2);
        _bind_text(stmt, 3, id);
        ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(store->db) == 1;
        if (!ok)
            _set_sql_error(store, error, "resource state 변경 실패");
    }
    if (stmt) sqlite3_finalize(stmt);

                                                                     
    gboolean quarantine_transition =
        ok && g_strcmp0(old_state, state) != 0 &&
        (g_strcmp0(old_state, "QUARANTINED") == 0 ||
         g_strcmp0(state, "QUARANTINED") == 0);
    if (quarantine_transition)
        ok = _bump_vpc_revision_locked(store, vpc_id, error);
    if (ok) ok = _commit(store, error); else _rollback(store);
    g_mutex_unlock(&store->mutex);
    return ok;
}

gboolean
pcv_vpc_store_bridge_is_managed(PcvVpcStore *store, const gchar *bridge_name)
{
    if (!store || !bridge_name) return FALSE;
    g_mutex_lock(&store->mutex);
    sqlite3_stmt *stmt = NULL;
    gboolean managed = FALSE;
    if (sqlite3_prepare_v2(store->db,
            "SELECT 1 FROM subnets WHERE bridge_name=? LIMIT 1", -1, &stmt, NULL) == SQLITE_OK) {
        _bind_text(stmt, 1, bridge_name); managed = sqlite3_step(stmt) == SQLITE_ROW;
    }
    if (stmt) sqlite3_finalize(stmt);
    g_mutex_unlock(&store->mutex);
    return managed;
}

gboolean
pcv_vpc_store_mac_is_managed(PcvVpcStore *store, const gchar *mac_address)
{
    if (!store || !mac_address) return FALSE;
    g_mutex_lock(&store->mutex);
    sqlite3_stmt *stmt = NULL;
    gboolean managed = FALSE;
    if (sqlite3_prepare_v2(store->db,
            "SELECT 1 FROM attachments WHERE lower(mac_address)=lower(?) LIMIT 1",
            -1, &stmt, NULL) == SQLITE_OK) {
        _bind_text(stmt, 1, mac_address);
        managed = sqlite3_step(stmt) == SQLITE_ROW;
    }
    if (stmt) sqlite3_finalize(stmt);
    g_mutex_unlock(&store->mutex);
    return managed;
}

gboolean
pcv_vpc_store_vm_is_attached(PcvVpcStore *store, const gchar *vm_identifier)
{
    if (!store || !vm_identifier) return FALSE;
    g_mutex_lock(&store->mutex);
    sqlite3_stmt *stmt = NULL;
    gboolean attached = FALSE;
    if (sqlite3_prepare_v2(store->db,
            "SELECT 1 FROM attachments WHERE vm_uuid=? OR vm_name_snapshot=? LIMIT 1",
            -1, &stmt, NULL) == SQLITE_OK) {
        _bind_text(stmt, 1, vm_identifier);
        _bind_text(stmt, 2, vm_identifier);
        attached = sqlite3_step(stmt) == SQLITE_ROW;
    }
    if (stmt) sqlite3_finalize(stmt);
    g_mutex_unlock(&store->mutex);
    return attached;
}

gboolean
pcv_vpc_store_vm_has_publish(PcvVpcStore *store, const gchar *vm_identifier)
{
    if (!store || !vm_identifier) return FALSE;
    g_mutex_lock(&store->mutex);
    sqlite3_stmt *stmt = NULL;
    gboolean published = FALSE;
    if (sqlite3_prepare_v2(store->db,
            "SELECT 1 FROM service_publishes p "
            "JOIN attachments a ON a.id=p.attachment_id "
            "WHERE (a.vm_uuid=? OR a.vm_name_snapshot=?) AND p.state<>'DELETING' LIMIT 1",
            -1, &stmt, NULL) == SQLITE_OK) {
        _bind_text(stmt, 1, vm_identifier);
        _bind_text(stmt, 2, vm_identifier);
        published = sqlite3_step(stmt) == SQLITE_ROW;
    }
    if (stmt) sqlite3_finalize(stmt);
    g_mutex_unlock(&store->mutex);
    return published;
}

GPtrArray *
pcv_vpc_store_list_managed_bridges(PcvVpcStore *store, GError **error)
{
    if (!store) return NULL;
    GPtrArray *array = g_ptr_array_new_with_free_func(g_free);
    g_mutex_lock(&store->mutex);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db,
            "SELECT bridge_name FROM subnets WHERE bridge_name IS NOT NULL ORDER BY bridge_name",
            -1, &stmt, NULL) != SQLITE_OK) {
        _set_sql_error(store, error, "managed bridge 목록 실패");
        g_mutex_unlock(&store->mutex); g_ptr_array_unref(array); return NULL;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW)
        g_ptr_array_add(array, g_strdup((const gchar *)sqlite3_column_text(stmt, 0)));
    sqlite3_finalize(stmt); stmt = NULL;
    if (sqlite3_prepare_v2(store->db,
            "SELECT vpc_id FROM vpc_backend_bindings ORDER BY vpc_id",
            -1, &stmt, NULL) != SQLITE_OK) {
        _set_sql_error(store, error, "managed OVN edge 목록 실패");
        g_mutex_unlock(&store->mutex); g_ptr_array_unref(array); return NULL;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW)
        g_ptr_array_add(array, pcv_vpc_ovn_edge_iface_name_from_id(
            (const gchar *)sqlite3_column_text(stmt, 0)));
    sqlite3_finalize(stmt); g_mutex_unlock(&store->mutex);
    return array;
}

static PcvVpcPolicyVpc *
_find_policy_vpc(PcvVpcPolicySnapshot *snapshot, const gchar *id)
{
    for (guint i = 0; i < snapshot->vpcs->len; i++) {
        PcvVpcPolicyVpc *v = g_ptr_array_index(snapshot->vpcs, i);
        if (g_strcmp0(v->id, id) == 0) return v;
    }
    return NULL;
}

static PcvVpcPolicySubnet *
_find_policy_subnet(PcvVpcPolicySnapshot *snapshot, const gchar *id)
{
    for (guint i = 0; i < snapshot->vpcs->len; i++) {
        PcvVpcPolicyVpc *v = g_ptr_array_index(snapshot->vpcs, i);
        for (guint j = 0; j < v->subnets->len; j++) {
            PcvVpcPolicySubnet *s = g_ptr_array_index(v->subnets, j);
            if (g_strcmp0(s->id, id) == 0) return s;
        }
    }
    return NULL;
}

static void
_parse_sources_into(const gchar *text, GPtrArray *out)
{
    JsonParser *parser = json_parser_new();
    if (text && json_parser_load_from_data(parser, text, -1, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        if (root && JSON_NODE_HOLDS_ARRAY(root)) {
            JsonArray *array = json_node_get_array(root);
            for (guint i = 0; i < json_array_get_length(array); i++)
                g_ptr_array_add(out, g_strdup(json_array_get_string_element(array, i)));
        }
    }
    g_object_unref(parser);
}

PcvVpcPolicySnapshot *
pcv_vpc_store_policy_snapshot(PcvVpcStore *store, GError **error)
{
    if (!store) return NULL;
    PcvVpcPolicySnapshot *snapshot = pcv_vpc_policy_snapshot_new();
    g_mutex_lock(&store->mutex);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db,
            "SELECT id,egress_mode,backend FROM vpcs WHERE state<>'DELETING' ORDER BY id",
            -1, &stmt, NULL) != SQLITE_OK) goto sql_fail;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        PcvVpcPolicyVpc *vpc = pcv_vpc_policy_vpc_new(
            (const gchar *)sqlite3_column_text(stmt, 0),
            (const gchar *)sqlite3_column_text(stmt, 1));
        g_free(vpc->backend);
        vpc->backend = g_strdup((const gchar *)sqlite3_column_text(stmt, 2));
        if (g_strcmp0(vpc->backend, "ovn") == 0)
            vpc->edge_interface = pcv_vpc_ovn_edge_iface_name_from_id(vpc->id);
        g_ptr_array_add(snapshot->vpcs, vpc);
    }
    sqlite3_finalize(stmt); stmt = NULL;
    if (sqlite3_prepare_v2(store->db,
            "SELECT s.id,s.vpc_id,s.bridge_name,s.cidr,s.gateway,v.backend FROM subnets s "
            "JOIN vpcs v ON v.id=s.vpc_id WHERE s.state<>'DELETING' ORDER BY s.id",
            -1, &stmt, NULL) != SQLITE_OK) goto sql_fail;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        PcvVpcPolicyVpc *v = _find_policy_vpc(snapshot, (const gchar *)sqlite3_column_text(stmt, 1));
        if (v) {
            PcvVpcPolicySubnet *subnet = pcv_vpc_policy_subnet_new(
                (const gchar *)sqlite3_column_text(stmt, 0),
                sqlite3_column_type(stmt, 2) == SQLITE_NULL ? NULL
                    : (const gchar *)sqlite3_column_text(stmt, 2),
                (const gchar *)sqlite3_column_text(stmt, 3),
                (const gchar *)sqlite3_column_text(stmt, 4));
            g_free(subnet->backend);
            subnet->backend = g_strdup((const gchar *)sqlite3_column_text(stmt, 5));
            g_ptr_array_add(v->subnets, subnet);
        }
    }
    sqlite3_finalize(stmt); stmt = NULL;
    if (sqlite3_prepare_v2(store->db,
            "SELECT subnet_id,ip_address,mac_address FROM attachments WHERE state='ACTIVE' ORDER BY id",
            -1, &stmt, NULL) != SQLITE_OK) goto sql_fail;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        PcvVpcPolicySubnet *s = _find_policy_subnet(snapshot,
            (const gchar *)sqlite3_column_text(stmt, 0));
        if (s) g_ptr_array_add(s->attachments, pcv_vpc_policy_attachment_new(
            (const gchar *)sqlite3_column_text(stmt, 1),
            (const gchar *)sqlite3_column_text(stmt, 2)));
    }
    sqlite3_finalize(stmt); stmt = NULL;
    if (sqlite3_prepare_v2(store->db,
            "SELECT p.protocol,p.listen_address,p.listen_port,p.target_port,p.allowed_sources,"
            "a.ip_address,s.bridge_name,p.vpc_id,v.backend FROM service_publishes p "
            "JOIN attachments a ON a.id=p.attachment_id JOIN subnets s ON s.id=a.subnet_id "
            "JOIN vpcs v ON v.id=p.vpc_id "
            "WHERE p.state='ACTIVE' AND a.state='ACTIVE' ORDER BY p.id",
            -1, &stmt, NULL) != SQLITE_OK) goto sql_fail;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        g_autofree gchar *target_interface = g_strcmp0(
            (const gchar *)sqlite3_column_text(stmt, 8), "ovn") == 0
            ? pcv_vpc_ovn_edge_iface_name_from_id((const gchar *)sqlite3_column_text(stmt, 7))
            : g_strdup((const gchar *)sqlite3_column_text(stmt, 6));
        PcvVpcPolicyPublish *p = pcv_vpc_policy_publish_new(
            (const gchar *)sqlite3_column_text(stmt, 0),
            (const gchar *)sqlite3_column_text(stmt, 1),
            (guint16)sqlite3_column_int(stmt, 2),
            (const gchar *)sqlite3_column_text(stmt, 5),
            (guint16)sqlite3_column_int(stmt, 3),
            target_interface);
        _parse_sources_into((const gchar *)sqlite3_column_text(stmt, 4), p->allowed_sources);
        g_ptr_array_add(snapshot->publishes, p);
    }
    sqlite3_finalize(stmt); g_mutex_unlock(&store->mutex);
    return snapshot;

sql_fail:
    if (stmt) sqlite3_finalize(stmt);
    _set_sql_error(store, error, "VPC policy snapshot 실패");
    g_mutex_unlock(&store->mutex);
    pcv_vpc_policy_snapshot_free(snapshot);
    return NULL;
}
