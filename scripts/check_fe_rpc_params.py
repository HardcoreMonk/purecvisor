#!/usr/bin/env python3
                          
                                                                          
                                                                                      
                                                                                                                                                                                          
 
                      
                                                                                                                                   
                                                                    

                                                 
                                                  
                                          
                                                                    
                                                       
                                                           
                                                       
                                                                      
                                                     
                                               
                                                                                       

         
                                     
                                                         

                                                  
                                                    
                                                        
                                                
                                              

                    
                                                                            
                                                
                                        
                                                
                                                         
                                                                   
                   
                                                                     
                                                           
                                                    
                                                 

          
                                                                   
                                                            
                                                                         
                                                           
                                               
                                                                  
                                                                   
                                  
                                                                             
                                                                         
                                                                         

          
                                             
                                                               
                                 

                 
                                            
                                     
                                                       
                                                      
                               

                
                                                 
                                                    
                                                                       
                                                        
                                                              
          
                                                 
                                         

                                                      
                            
                                             
                                                                
                                                                
                                    

                        
                                                    
                                 
                                                                
                                             
                                                                    
                                          
                                         

                                      
                                                      
                                                 
                                           

                                                             
                                                               
                                                          
                   

                                             
                                                                   
                                                         
                                                             

                                                          
                                                           
                                                                           
                           
                  
                                                                
                                                             
                                                            
                                               

                                                               
                                                                    
                                                                 
                                                                             
                                                
                                                                               
                                                        
                                                                    
                                                                   
                                                                 
                                                             
                                      

   
                                                                                
                                                                    
                                                                       
   
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rpc_extract

ROOT = Path(__file__).resolve().parent.parent
UI_DIR = ROOT / "ui"
UNCOVERED_FILE = ROOT / "scripts" / "fe_rpc_params_uncovered.txt"
VIOLATION_FILE = ROOT / "scripts" / "fe_rpc_params_baseline.txt"
MISSING_FILE = ROOT / "scripts" / "fe_rpc_params_missing_baseline.txt"

                                       
                                                                    
                                                           
                 
 
                                                             
                                                                          
                     
 
                                                              
                                                                          
                                                           
                                                                      
                                                     
                                    
EP_TO_RPC = {
    ("ALERTS_CONFIG", "PUT"): "alert.config.set",
    ("ALERT_SILENCE", "POST"): "alert.silence",
    ("AUTH_ROLE", "POST"): "auth.role.set",
    ("AUTH_SESSION_REVOKE", "POST"): "auth.session.revoke",



    ("AUTH_TOTP_RESET", "POST"): "auth.totp.reset",



    ("AUTH_APIKEY_CREATE", "POST"): "auth.apikey.create",
    ("AUTH_USERS", "POST"): "auth.user.create",
    ("AUTH_USERS", "DELETE"): "auth.user.delete",
    ("AUTH_USER", "DELETE"): "auth.user.delete",
    ("BACKUP_POLICIES", "POST"): "backup.policy.set",
    ("BACKUP_POLICIES", "DELETE"): "backup.policy.delete",
    ("BACKUP_VERIFY", "POST"): "backup.verify",
    ("CONFIG_BACKUP", "POST"): "config.backup",
    ("CONFIG_DAEMON", "PUT"): "daemon.config.set",
    ("CTR_BANDWIDTH", "PUT"): "container.set_bandwidth",
    ("CTR_CLONE", "POST"): "container.clone",
    ("CTR_LIMITS", "PUT"): "container.set_limits",
    ("CTR_LIST", "POST"): "container.create",
    ("CTR_NICS", "POST"): "container.nic.attach",
    ("CTR_SNAPSHOTS", "POST"): "container.snapshot.create",
    ("CTR_SNAP_ROLLBACK", "POST"): "container.snapshot.rollback",
    ("DPDK_BIND", "POST"): "dpdk.bind",
    ("DPDK_UNBIND", "POST"): "dpdk.unbind",
    ("NET_LIST", "POST"): "network.create",
    ("NET_MODE", "POST"): "network.mode_set",
    ("OVA_IMPORT", "POST"): "vm.import.ova",
    ("OVN_ACL", "POST"): "ovn.acl.add",
    ("PUSH_UNSUBSCRIBE", "POST"): "push.unsubscribe",
    ("SRIOV_ATTACH", "POST"): "sriov.attach",
    ("SRIOV_DETACH", "POST"): "sriov.detach",
    ("SRIOV_DISABLE", "POST"): "sriov.disable",
    ("SRIOV_ENABLE", "POST"): "sriov.enable",
    ("STORAGE_POOLS", "POST"): "storage.pool.create",
    ("STORAGE_POOLS", "DELETE"): "storage.pool.destroy",
    ("STORAGE_SCRUB", "POST"): "storage.pool.scrub",
    ("STORAGE_ZVOLS", "POST"): "storage.zvol.create",
    ("STORAGE_ZVOLS", "DELETE"): "storage.zvol.delete",
    ("SURICATA_POLICY", "PUT"): "suricata.policy.set",
    ("SURICATA_RULES_UPDATE", "POST"): "suricata.rules.update",
    ("SURICATA_IPS_ENABLE", "POST"): "suricata.ips.enable",
    ("SURICATA_IPS_DISABLE", "POST"): "suricata.ips.disable",
    ("SURICATA_IPS_DROP", "POST"): "suricata.ips.drop.add",
    ("SURICATA_IPS_DROP", "DELETE"): "suricata.ips.drop.remove",
    ("TEMPLATES", "POST"): "template.create",                                      
    ("VPC_LIST", "POST"): "vpc.create",                                           
    ("VPC_DETAIL", "DELETE"): "vpc.delete",                                       
    ("VPC_EGRESS", "POST"): "vpc.egress.set",                                     
    ("VPC_SUBNETS", "POST"): "vpc.subnet.create",                                 
    ("VPC_SUBNET", "DELETE"): "vpc.subnet.delete",                                
    ("VPC_ATTACHMENT_LIST", "POST"): "vpc.attachment.create",                     
    ("VPC_ATTACHMENT", "DELETE"): "vpc.attachment.delete",                        
    ("VPC_SERVICE_LIST", "POST"): "vpc.service.publish",                          
    ("VPC_SERVICE", "DELETE"): "vpc.service.unpublish",                           
    ("VPC_RECONCILE", "POST"): "vpc.reconcile",                                   
    ("VM_BANDWIDTH", "PUT"): "vm.set_bandwidth",                                   
    ("VM_CLONE", "POST"): "vm.clone",
    ("VM_CPU_PIN", "PUT"): "vm.pin_vcpu",                                          
    ("VM_CREATE", "POST"): "vm.create",
    ("VM_DISK", "PUT"): "vm.resize_disk",
    ("VM_DISK_RESIZE", "POST"): "vm.disk.live_resize",                             
    ("VM_GUEST_EXEC", "POST"): "vm.guest.exec",
    ("VM_ISO", "POST"): "vm.mount_iso",                                            
    ("VM_NICS", "POST"): "device.nic.attach",                                      

    ("VM_SNAPSHOT_DELETE_ALL", "POST"): "vm.snapshot.delete_all",
}

SPECIAL_HANDLERS = {"vm.create": "handle_vm_create"}

                                                        
                                                               
                                               
EP_NOT_REST = {"RPC"}

                                                                            
                                                        
                      
RPC_CALL = re.compile(
    r"method\s*:\s*'([a-z0-9_.]+)'\s*,\s*params\s*:\s*\{([^{}]*)\}", re.S)
                                                                                
REST_CALL = re.compile(
    r"fetch(Post|Put|Delete)\s*\(\s*EP\.([A-Z0-9_]+)\s*\([^)]*\)\s*,\s*\{([^{}]*)\}", re.S)

REST_VAR_CALL = re.compile(
    r"fetch(Post|Put|Delete)\s*\(\s*EP\.([A-Z0-9_]+)\s*\([^)]*\)\s*,\s*([A-Za-z_$][\w$]*)\s*[,)]")
                                                               
                                                            
                                                       
                          
KEY = re.compile(r"""(?:^|,)\s*(?:'([A-Za-z_$][\w$]*)'|"([A-Za-z_$][\w$]*)"|([A-Za-z_$][\w$]*))\s*:""")
                                                                           
ROUTE = re.compile(
    r'g_hash_table_insert\(\s*g_rpc_routes\s*,\s*"([^"]+)"\s*,\s*\(gpointer\)\s*(\w+)\)')
CALLEE = re.compile(r'\b([A-Za-z_]\w*)\s*\(')
                                               
                                                                         
                                          
PARAM_DECL = re.compile(r'JsonObject\s*\*\s*(\w+)')


def be_read_re(var):
                                         

                                                                    
                                                                          
    return re.compile(r'(?:json_object_\w*member\w*|_json_\w+_member|_string_member|_int_member'
                      r'|pcv_rpc_params_get_(?:string|bool))\(\s*'
                      + re.escape(var) + r'\s*,\s*"([a-z0-9_]+)"')


def get_param_re(var):
                                                             

                                                                  
    return re.compile(r'_get_param\(\s*' + re.escape(var)
                      + r'\s*,\s*"([a-z0-9_]+)"\s*(?:,\s*"([a-z0-9_]+)"\s*)?\)')


def _top_level_items(body):

    items, cur, depth, i, n = [], [], 0, 0, len(body)
    while i < n:
        c = body[i]
        if c in "\"'`":
            j = i + 1
            while j < n and body[j] != c:
                j += 2 if body[j] == "\\" else 1
            cur.append(body[i:j + 1])
            i = j + 1
            continue
        if c == "/" and i + 1 < n and body[i + 1] in "/*":
            end = body.find("\n", i) if body[i + 1] == "/" else body.find("*/", i + 2)
            i = n if end < 0 else end + (0 if body[i + 1] == "/" else 2)
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        if c == "," and depth == 0:
            items.append("".join(cur))
            cur = []
        else:
            cur.append(c)
        i += 1
    items.append("".join(cur))
    return [it.strip() for it in items if it.strip()]

def object_keys(body):

    keys, complete = set(), True
    for item in _top_level_items(body):
        if item.startswith("...") or item.startswith("["):
            complete = False
            continue
        m = re.match(r"""(?:'([A-Za-z_$][\w$]*)'|"([A-Za-z_$][\w$]*)"|([A-Za-z_$][\w$]*))\s*(:|\(|$)""", item)
        if m:
            keys.add(m.group(1) or m.group(2) or m.group(3))
    return keys, complete

def keys_in(body):
                                                       
    return object_keys(body)[0]


def required_guard_re(var):
                                                                  
    return re.compile(r'!\s*json_object_has_member\(\s*' + re.escape(var)
                      + r'\s*,\s*"([a-z0-9_]+)"\s*\)')


                                                
                                                         
ERROR_RETURN = re.compile(r'PURE_RPC_ERR|build_error_response|_send_error|error_response')
ANY_HAS_MEMBER = re.compile(r'json_object_has_member\s*\(')
IF_HEAD = re.compile(r'\bif\s*\(')
                                                                            
                                          
RPC_INJECTED_KEYS = frozenset({"_pcv_caller_sub", "_pcv_caller_role", "_pcv_caller_tenant"})


                                                 
                                                          
                      
ENVELOPE_KEYS = {"jsonrpc", "id", "method", "params"}

                                 
                                      
                                                                 
                                          
                                                                 
                                                         
                                                           
                                                        
                                                   
C_CONTROL = {"if", "while", "for", "switch", "return", "sizeof", "do", "else"}
PARAM_SAFE_CALLEES = {"_get_param", "_wipe_json_string_member",
                      "_string_member", "_int_member",
                      "pcv_rpc_params_get_string", "pcv_rpc_params_get_bool",
                      "g_return_if_fail", "g_return_val_if_fail",
                      "g_assert", "g_assert_nonnull", "g_assert_true"}

                                             
                                                
                                                        
PARAM_HELPERS = {
    "_dispatcher_caller_subject": {"_pcv_caller_sub"},                  
    "_push_caller_username": {"_pcv_caller_sub"},                  
    "_dispatcher_caller_role": {"_pcv_caller_role"},                    
    "_get_pagination_params": {"offset", "limit"},                    
                                                                     
                                                           
                                                      
                                                  
    "_new_worker": {"tenant", "_pcv_caller_sub", "_pcv_caller_role"},
    "_read_scope": {"tenant", "_pcv_caller_sub", "_pcv_caller_role"},
    "_require_uuid": {"vpc_id", "subnet_id", "attachment_id", "publish_id"},
    "_require_revision": {"expected_revision"},
    "_source_array": {"allowed_sources"},
    "_enqueue_id_operation": {
        "tenant", "_pcv_caller_sub", "_pcv_caller_role",
        "vpc_id", "subnet_id", "attachment_id", "publish_id",
    },

    "pcv_res_gate": {"_pcv_caller_sub", "_pcv_caller_role", "_pcv_caller_tenant"},

    "pcv_res_caller_init": {"_pcv_caller_sub", "_pcv_caller_role", "_pcv_caller_tenant"},
    "pcv_res_request_can_see_usb": {"_pcv_caller_sub", "_pcv_caller_role", "_pcv_caller_tenant"},
}

for _old, _new in rpc_extract.split_renames(ROOT).items():
    if _old in PARAM_SAFE_CALLEES:
        PARAM_SAFE_CALLEES = PARAM_SAFE_CALLEES | {_new}
    if _old in PARAM_HELPERS:
        PARAM_HELPERS[_new] = PARAM_HELPERS[_old]

_be_cache = {}
_req_cache = {}
_body_cache = {}
_src_cache = []
_rest_injected = None


def ui_sources():
                                 
    out = []
    for p in sorted(UI_DIR.rglob("*.js")):
        s = p.as_posix()
        if "node_modules" in s or "vendor" in s or "bundle" in s:
            continue
        out.append(p)
    return out


def _balanced_js(text, i):

    depth, n = 0, len(text)
    while i < n:
        c = text[i]
        if c in "\"'`":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            i = j + 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] in "/*":
            end = text.find("\n", i) if text[i + 1] == "/" else text.find("*/", i + 2)
            if end < 0:
                return -1
            i = end + (0 if text[i + 1] == "/" else 2)
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1

def resolve_var_body(text, name, call_pos):

    esc = re.escape(name)
    decls = list(re.finditer(r'\b(?:const|let|var)\s+' + esc + r'\s*=\s*\{', text[:call_pos]))
    if not decls:
        return None
    decl = decls[-1]
    open_at = decl.end() - 1
    close = _balanced_js(text, open_at)
    if close < 0 or close > call_pos:
        return None
    _, masked = mask_source(text[close + 1:call_pos])
    depth = 0
    for c in masked:
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth < 0:
                return None
    keys, complete = object_keys(text[open_at + 1:close])
    code, masked = mask_source(text[close + 1:call_pos])
    keys |= set(re.findall(r'(?<![\w$.])' + esc + r'\.([A-Za-z_$][\w$]*)\s*=(?!=)', code))
    keys |= set(re.findall(r'(?<![\w$.])' + esc + r"""\[\s*['"]([A-Za-z_$][\w$]*)['"]\s*\]\s*=(?!=)""", code))
    if re.search(r'(?<![\w$.])' + esc + r"""\[\s*[^'"\s]""", masked):
        complete = False
    for am in re.finditer(r'Object\.assign\(\s*' + esc + r'\s*,\s*(.)', code):
        if am.group(1) != "{":
            complete = False
            continue
        end = _balanced_js(code, am.start(1))
        if end < 0:
            complete = False
            continue
        extra, extra_complete = object_keys(code[am.start(1) + 1:end])
        keys |= extra
        complete = complete and extra_complete
    return keys, complete

def extract_sent(text):
                                    

                                                  
                             
    found = []
    for m in RPC_CALL.finditer(text):
        keys, complete = object_keys(m.group(2))
        found.append((m.group(1), keys - ENVELOPE_KEYS, False, complete))
    for m in REST_CALL.finditer(text):
        if m.group(2) in EP_NOT_REST:
            continue
        keys, complete = object_keys(m.group(3))
        found.append(((m.group(2), m.group(1).upper()), keys - ENVELOPE_KEYS, True, complete))
    for m in REST_VAR_CALL.finditer(text):
        if m.group(2) in EP_NOT_REST:
            continue
        target = (m.group(2), m.group(1).upper())
        resolved = resolve_var_body(text, m.group(3), m.start())
        if resolved is None:
            found.append((target, None, True, False))
            continue
        found.append((target, resolved[0] - ENVELOPE_KEYS, True, resolved[1]))
    return found


def label(target, is_ep):
                                              
    return f"EP.{target[0]}[{target[1]}]" if is_ep else target


def mask_source(text):
                                                            

                                                    
                                                  

                                                 
                                                                     
                                                     
    n = len(text)
    code = list(text)
    masked = list(text)
    i = 0
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if text[k] != "\n":
                    code[k] = masked[k] = " "
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                code[k] = masked[k] = " "
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == c:
                    j += 1
                    break
                j += 1
            for k in range(i + 1, max(i + 1, min(j, n) - 1)):
                if text[k] != "\n":
                    masked[k] = " "
            i = j
        else:
            i += 1
    return "".join(code), "".join(masked)


def _src_files():
                                                                    
    if not _src_cache:
        for src in sorted((ROOT / "src").rglob("*.c")):
            code, masked = mask_source(
                src.read_text(encoding="utf-8", errors="replace"))
            _src_cache.append((src, code, masked))
    return _src_cache


def _routes():
                                      

                                                                
                                                     
    disp = rpc_extract.dispatcher_route_file(ROOT)
    table = {}
    for path, code, _ in _src_files():
        if path != disp:
            continue
        for method, fn in ROUTE.findall(code):
            table[method] = fn
    for method, fn in SPECIAL_HANDLERS.items():
        table.setdefault(method, fn)
    return table


def _function_span(masked, fn):
                                                            

                                                                
                                          
                                                         
    fm = re.search(r'\b' + re.escape(fn) + r'\s*\(([^;]*?)\)\s*\{', masked)
    if not fm:
        return None
    pm = PARAM_DECL.search(fm.group(1))
    i, depth = fm.end() - 1, 0
    while i < len(masked):
        if masked[i] == "{":
            depth += 1
        elif masked[i] == "}":
            depth -= 1
            if depth == 0:
                break
        i += 1
    return fm.end(), i, (pm.group(1) if pm else None)


def forwards_params(body, var="params"):
                                                

                                                  
                                                   
                                       
                                                               
                                                  
    return bool(forwarded_calls(body, var))

def _strip_nested_readers(args, var):

    return re.sub(r'(?:json_object_\w+|(?:pcv_disp)?_json_\w+_member|pcv_rpc_params_get_\w+|_get_param)\s*\(\s*'
                  + re.escape(var) + r'\b', "(", args)

def forwarded_calls(body, var):

    token = re.compile(r'\b' + re.escape(var) + r'\b')
    calls = []
    for m in CALLEE.finditer(body):
        name = m.group(1)
        if (name in C_CONTROL or name in PARAM_SAFE_CALLEES or name in PARAM_HELPERS
                or name.startswith("json_object_")
                or re.fullmatch(r"(?:pcv_disp)?_json_\w+_member", name)):
            continue
        close = _balanced(body, m.end() - 1, "(", ")")
        if close < 0:
            continue
        args = _strip_nested_readers(body[m.end():close], var)
        if not token.search(args):
            continue
        pos, depth, start, found = 0, 0, 0, None
        for k, c in enumerate(args + ","):
            if c in "([{":
                depth += 1
            elif c in ")]}":
                depth -= 1
            elif c == "," and depth == 0:
                if token.fullmatch(args[start:k].strip()):
                    found = pos
                pos += 1
                start = k + 1
        calls.append((name, found))
    return calls

def _callee_read_keys(name, arg_pos, depth):

    if depth <= 0 or arg_pos is None:
        return None
    macro = re.compile(r'^[ \t]*#[ \t]*define[ \t]+' + re.escape(name) + r'\(([^)]*)\)((?:.*\\\n)*.*)', re.M)
    for _, code, masked in _src_files():
        if name not in masked:
            continue
        mm = macro.search(code)
        if mm:
            formals = [a.strip() for a in mm.group(1).split(",")]
            if arg_pos >= len(formals):
                return None
            return _read_keys_in(mm.group(2), mm.group(2), formals[arg_pos], depth - 1)

        fm = re.search(r'\b' + re.escape(name) + r'\s*\(([^;(){}]*)\)\s*\{', masked)
        if not fm:
            continue
        formals = [a.strip() for a in fm.group(1).split(",")]
        if arg_pos >= len(formals):
            return None
        decl = PARAM_DECL.fullmatch(formals[arg_pos].replace("const ", "").strip())
        if not decl:
            return None
        end = _balanced(masked, fm.end() - 1, "{", "}")
        if end < 0:
            return None
        return _read_keys_in(code[fm.end():end], masked[fm.end():end], decl.group(1), depth - 1)
    return None

def _read_keys_in(code, masked, var, depth):

    if re.search(r'json_object_(?:get_members|foreach_member)\s*\(\s*' + re.escape(var) + r'\b', code):
        return None
    keys = set(be_read_re(var).findall(code))

    keys |= set(re.findall(r'\b\w+\(\s*' + re.escape(var) + r'\s*,\s*"([a-z0-9_]+)"', code))
    for primary, fallback in get_param_re(var).findall(code):
        keys.add(primary)
        if fallback:
            keys.add(fallback)
    for helper, helper_keys in PARAM_HELPERS.items():
        if re.search(r'\b' + helper + r'\(\s*' + re.escape(var) + r'\s*[,)]', code):
            keys |= helper_keys
    for name, pos in forwarded_calls(masked, var):
        sub = _callee_read_keys(name, pos, depth)
        if sub is None:
            return None
        keys |= sub
    return keys


def _handler_body(method):
                                                         

                                                         
                                                     
                                                           
    if method in _body_cache:
        return _body_cache[method]
    fn = _routes().get(method)
    result = None
    if fn:
        for _, code, masked in _src_files():
            if fn not in masked:
                continue
            span = _function_span(masked, fn)
            if span is None:
                continue
            start, end, var = span
            if var is not None:
                result = (code[start:end], masked[start:end], var)
            break
    _body_cache[method] = result
    return result

FORWARD_DEPTH = 4

SCHEMA_READERS = {
    "alert.config.set": ("src/modules/daemons/alert_engine.c",
                         r'g_strcmp0\(key,\s*"([a-z0-9_]+)"\)', {"expected_revision"}),
}


def be_read_keys(method):
                                            

                                                                
                                                       
                                           
    if method in _be_cache:
        return _be_cache[method]
    if method in SCHEMA_READERS:
        rel, pattern, extra = SCHEMA_READERS[method]
        text, _ = mask_source((ROOT / rel).read_text(encoding="utf-8", errors="replace"))
        _be_cache[method] = set(re.findall(pattern, text)) | extra
        return _be_cache[method]
    span = _handler_body(method)
    result = None
    if span is not None:
        code, masked, var = span
        result = _read_keys_in(code, masked, var, FORWARD_DEPTH)
    _be_cache[method] = result
    return result


def _balanced(text, i, op, cl):
                                                 

                                                   
    depth = 0
    while i < len(text):
        if text[i] == op:
            depth += 1
        elif text[i] == cl:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def required_keys_in(code, masked, var):
                                                                  

                                                      
                                                         
                                                  
                                       
    req = set()
    neg = required_guard_re(var)
    for m in IF_HEAD.finditer(masked):
        op = m.end() - 1
        cl = _balanced(masked, op, "(", ")")
        if cl < 0:
            continue
        cond_code, cond_masked = code[op:cl + 1], masked[op:cl + 1]
        keys = neg.findall(cond_code)
        if not keys or "&&" in cond_masked:
            continue
        if len(ANY_HAS_MEMBER.findall(cond_masked)) != len(keys):
            continue
        j = cl + 1
        while j < len(masked) and masked[j].isspace():
            j += 1
        if j < len(masked) and masked[j] == "{":
            end = _balanced(masked, j, "{", "}")
            end = len(masked) if end < 0 else end + 1
        else:
            end = masked.find(";", j)
            end = len(masked) if end < 0 else end + 1
        if not re.search(r'\breturn\b', masked[j:end]):
            continue
        if not ERROR_RETURN.search(code[j:end]):
            continue
        req |= set(keys)
    return req


def be_required_keys(method):
                                                            
    if method in _req_cache:
        return _req_cache[method]
    span = _handler_body(method)
    _req_cache[method] = None if span is None or be_read_keys(method) is None else required_keys_in(*span)
    return _req_cache[method]


def rest_injected_keys():
                                                          

                                                          
                                                    
                                                    
    global _rest_injected
    if _rest_injected is None:

        code, _ = mask_source(rpc_extract.rest_family_text(ROOT))
        _rest_injected = frozenset(
            re.findall(r'json_object_set_\w+_member\(\s*\w+\s*,\s*"([a-z0-9_]+)"', code))
    return _rest_injected


def load_uncovered_annotated():
                                                  

                                                      
                                             
    out = {}
    if not UNCOVERED_FILE.exists():
        return out
    for line in UNCOVERED_FILE.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        name, sep, why = line.partition("#")
        out[name.strip()] = why.strip() if sep else ""
    return out


def load_uncovered():
    return set(load_uncovered_annotated())


def scan(sources=None, resolve_read=None, resolve_required=None):
                                           

                                           
                                                 
                                          

                                                             
                                                                      
                                                                   

                                                                
                                                   
                                                        
                                                      
    if sources is None:
        sources = [(p.relative_to(ROOT).as_posix(),
                    p.read_text(encoding="utf-8", errors="replace"))
                   for p in ui_sources()]
    if resolve_read is None:
        resolve_read = be_read_keys
    if resolve_required is None:
        resolve_required = be_required_keys
    violations, missing, uncovered = [], [], set()
    for name, text in sources:
        for target, keys, is_ep, complete in extract_sent(text):
            method = EP_TO_RPC.get(target) if is_ep else target
            if method is None or keys is None:
                uncovered.add(label(target, is_ep))
                continue
            read = resolve_read(method)
            if read is None:
                uncovered.add(label(target, is_ep))
                continue
            extra = keys - read
            if extra:
                violations.append((name, method, sorted(extra), sorted(read)))
            if not complete:

                uncovered.add(label(target, is_ep))
                continue
            required = resolve_required(method)
            if required:
                                                                
                                                      
                injected = rest_injected_keys() if is_ep else RPC_INJECTED_KEYS
                lacking = required - keys - injected
                if lacking:
                    missing.append((name, method, sorted(lacking), sorted(required)))
    violations.sort()
    missing.sort()
    return violations, missing, uncovered


def load_pipe_baseline(path):
                                                                       

                                                           
    if not path.exists():
        return set()
    out = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        f, method, keys = line.split("|")
        out.add((f, method, frozenset(k.strip() for k in keys.split(",") if k.strip())))
    return out


def load_violation_baseline():
                                        
    return load_pipe_baseline(VIOLATION_FILE)


def load_missing_baseline():
                                        
    return load_pipe_baseline(MISSING_FILE)


def run_check():
    violations, missing, uncovered = scan()
    baseline = load_uncovered()
    vbase = load_violation_baseline()
    mbase = load_missing_baseline()
    print(f"[check-fe-rpc-params] 호출부 검사 완료 — 정방향 위반 {len(violations)}건"
          f"(래칫 기준 {len(vbase)}), 역방향 위반 {len(missing)}건"
          f"(래칫 기준 {len(mbase)}), 미커버 EP {len(uncovered)}개"
          f"(래칫 기준 {len(baseline)})")
    rc = 0
    new_v = [v for v in violations
             if (v[0], v[1], frozenset(v[2])) not in vbase]
    fixed_v = vbase - {(v[0], v[1], frozenset(v[2])) for v in violations}
    if new_v:
        print("\n[FAIL] UI 가 보내는데 백엔드가 읽지 않는 파라미터(baseline 밖 신규):")
        for f, method, extra, read in new_v:
            print(f"  - {f}  {method}  보냄={extra}  서버가 읽는 키={read}")
        print("\n시정: UI 를 백엔드 계약에 맞춰라. 지금 못 고칠 정당한 사유가 있으면 "
              f"{VIOLATION_FILE.name} 에 사유와 함께 등재(단, 총량은 줄어들기만 해야 한다).")
        rc = 1
    for f, method, keys in sorted(fixed_v):
        print(f"[INFO] baseline 위반이 해소됨(제거 권장): {f} {method} {sorted(keys)}")
    new_m = [m for m in missing
             if (m[0], m[1], frozenset(m[2])) not in mbase]
    fixed_m = mbase - {(m[0], m[1], frozenset(m[2])) for m in missing}
    if new_m:
        print("\n[FAIL] 백엔드가 필수로 요구하는데 UI 가 보내지 않는 파라미터"
              f"({MISSING_FILE.name} 밖 신규):")
        for f, method, lack, required in new_m:
            print(f"  - {f}  {method}  누락={lack}  서버 필수 키={required}")
        print("\n시정: 그 키를 요청 본문에 실어라(빠지면 서버가 -32602 로 거절한다). "
              f"지금 못 고칠 정당한 사유가 있으면 {MISSING_FILE.name} 에 사유와 함께 "
              "등재(단, 총량은 줄어들기만 해야 한다).")
        rc = 1
    for f, method, keys in sorted(fixed_m):
        print(f"[INFO] 역방향 baseline 위반이 해소됨(제거 권장): {f} {method} {sorted(keys)}")
    new_unc = sorted(uncovered - baseline)
    if new_unc:
        print("\n[FAIL] 게이트가 커버하지 못하는 신규 EP(래칫 증가):")
        for u in new_unc:
            print(f"  - {u}")
        print("\n시정: EP_TO_RPC 에 매핑을 추가하거나, 정당한 예외면 "
              f"{UNCOVERED_FILE.name} 에 등재(단, 총량은 줄어들기만 해야 한다).")
        rc = 1
    stale = sorted(baseline - uncovered)
    for s in stale:
        print(f"[INFO] uncovered 항목이 더 이상 나타나지 않음(prune 권장): {s}")
    if rc == 0:
        print("[PASS] FE 요청 파라미터가 전부 백엔드 계약 안에 있다(정·역 양방향)")
    return rc


def self_test():
                                         
    ok = True
    sent = extract_sent(
        "await fetchPost(EP.RPC(), { jsonrpc:'2.0', method:'a.b', params:{ x: 1, y: 2 }, id:'i' });")
    if not sent or sent[0][0] != "a.b" or sent[0][1] != {"x", "y"}:
        print(f"[SELF-TEST FAIL] 직접 RPC 호출 추출 실패: {sent}")
        ok = False
    sent = extract_sent("var r = await fetchPost(EP.FOO(n), { k1: a, k2: b });")
    if not sent or sent[0][0] != ("FOO", "POST") or sent[0][1] != {"k1", "k2"}:
        print(f"[SELF-TEST FAIL] REST 본문 호출 추출 실패: {sent}")
        ok = False
                                               
    sent = extract_sent("await fetchDelete(EP.FOO(), { k: 1 });")
    if not sent or sent[0][0] != ("FOO", "DELETE"):
        print(f"[SELF-TEST FAIL] HTTP 동사 구분 실패: {sent}")
        ok = False
    if be_read_re("params").findall('json_object_has_member(params, "username")') != ["username"]:
        print("[SELF-TEST FAIL] BE read-key 추출 실패")
        ok = False
    if be_read_re("params").findall('json_object_get_string_member(params, "snap_name")') != ["snap_name"]:
        print("[SELF-TEST FAIL] BE getter read-key 추출 실패")
        ok = False
                                           
    if ROUTE.findall(
            '    g_hash_table_insert(g_rpc_routes, "a.b",   (gpointer)handle_a_b);') \
            != [("a.b", "handle_a_b")]:
        print("[SELF-TEST FAIL] 라우팅 표 추출 실패")
        ok = False
                                       
    src = ('static void h(JsonObject *params, const gchar *id);\n'
           'static void h(JsonObject *params, const gchar *id)\n{\n'
           '    if (x) { json_object_get_int_member(params, "n"); }\n}\n'
           'void other(void) { json_object_get_int_member(params, "outside"); }\n')
    code, masked = mask_source(src)
    span = _function_span(masked, "h")
    if span is None or span[2] != "params" \
            or set(be_read_re("params").findall(code[span[0]:span[1]])) != {"n"}:
        print(f"[SELF-TEST FAIL] 함수 본문 절단 실패: {span}")
        ok = False
                                            
    code2, masked2 = mask_source(
        'void g(JsonObject *p, const gchar *id)\n{\n'
        '    json_object_get_string_member(p, "pci_addr");\n}\n')
    span2 = _function_span(masked2, "g")
    if span2 is None or span2[2] != "p" \
            or set(be_read_re(span2[2]).findall(code2[span2[0]:span2[1]])) != {"pci_addr"}:
        print(f"[SELF-TEST FAIL] 인자명이 params 가 아닌 핸들러의 키 추출 실패: {span2}")
        ok = False
                                      
    code, masked = mask_source(
        '/* params 를 그대로 넘긴다 */\n'
        'send(rpc_id, "Invalid params: vm_name required");\n'
        'json_object_get_string_member(params, "vm_name");\n')
    if len(masked) != len(code) or "vm_name required" in masked:
        print("[SELF-TEST FAIL] 문자열 리터럴 마스킹 실패")
        ok = False
    if forwards_params(masked):
        print("[SELF-TEST FAIL] 주석·에러 메시지 속 'params' 를 전달로 오인")
        ok = False
    if set(be_read_re("params").findall(code)) != {"vm_name"}:
        print("[SELF-TEST FAIL] 마스킹 후 키 리터럴이 소실됨")
        ok = False
                                             
    if set(sum((list(t) for t in get_param_re("params").findall(
            '_get_param(params, "snapshot_name", "snap_name");')), [])) \
            != {"snapshot_name", "snap_name"}:
        print("[SELF-TEST FAIL] _get_param 키 추출 실패")
        ok = False
                                            
    if forwards_params('if (!params || !json_object_has_member(params, "k")) return;\n'
                       '(void)params;\n'
                       'const gchar *v = _get_param(params, "a", "b");\n'
                       'foo(v);\n'):
        print("[SELF-TEST FAIL] 판독기·가드를 params 전달로 오인")
        ok = False
    if not forwards_params('gboolean ok = pcv_security_group_rule_add(sg_name, params);'):
        print("[SELF-TEST FAIL] params 통째 전달을 감지하지 못함")
        ok = False
                                 
    if forwards_params('_get_pagination_params(params, &off, &lim);'):
        print("[SELF-TEST FAIL] 고정키 헬퍼를 params 전달로 오인")
        ok = False
                            
    if be_read_re("params").findall('_json_string_member(params, "endpoint")') != ["endpoint"]:
        print("[SELF-TEST FAIL] _json_*_member 판독기 추출 실패")
        ok = False
                                                             
    typed = 'pcv_rpc_params_get_string(params, "hwaddr"); pcv_rpc_params_get_bool(params, "readonly", TRUE, &ro);'
    if set(be_read_re("params").findall(typed)) != {"hwaddr", "readonly"} or forwards_params(typed):
        print("[SELF-TEST FAIL] pcv_rpc_params_get_* 판독기 추출 실패 또는 params 전달로 오인")
        ok = False

    if set(be_read_re("params").findall(
            '_string_member(params, "egress_mode"); _int_member(params, "listen_port", &p);')) \
            != {"egress_mode", "listen_port"}:
        print("[SELF-TEST FAIL] Local VPC typed member 판독기 추출 실패")
        ok = False
    if forwards_params('_new_worker(params, OP, "vpc.create", TRUE, &error);'):
        print("[SELF-TEST FAIL] Local VPC 고정 계약 helper를 params 전달로 오인")
        ok = False
    if PARAM_HELPERS.get("_require_revision") != {"expected_revision"} \
            or "allowed_sources" not in PARAM_HELPERS.get("_source_array", set()):
        print("[SELF-TEST FAIL] Local VPC helper key 계약이 소실됨")
        ok = False
                                                   
    sent = extract_sent("fetchPost(EP.OVN_ACL(), { 'switch': sw, \"match\": m, action: a });")
    if not sent or sent[0][1] != {"switch", "match", "action"}:
        print(f"[SELF-TEST FAIL] 따옴표 키 추출 실패: {sent}")
        ok = False
                                            
    vk = keys_in("a: 'x: y', b: c ? 'd' : 'e'")
    if vk != {"a", "b"}:
        print(f"[SELF-TEST FAIL] 값 위치 문자열을 키로 오인: {sorted(vk)}")
        ok = False

                                                                     
    def _req(src, var="params"):
        return required_keys_in(*mask_source(src), var)

                                     
    if _req('void h(JsonObject *params) {\n'
            '  if (!json_object_has_member(params, "username")) {\n'
            '    gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "x");\n'
            '    send(r); g_free(r); return;\n'
            '  }\n}\n') != {"username"}:
        print("[SELF-TEST FAIL] 필수 키(가드+에러 리턴) 추출 실패")
        ok = False
                                                 
    if _req('if (!params || !json_object_has_member(params, "a")\n'
            '    || !json_object_has_member(params, "b")) {\n'
            '  gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "x");\n'
            '  return;\n}\n') != {"a", "b"}:
        print("[SELF-TEST FAIL] || 다중 가드의 필수 키 추출 실패")
        ok = False
                                                       
    if _req('const gchar *d = json_object_has_member(params, "direction")\n'
            '  ? json_object_get_string_member(params, "direction") : "to-lport";\n'):
        print("[SELF-TEST FAIL] 선택 파라미터(has_member ? get : 기본값)를 필수로 셌다")
        ok = False
                                
    if _req('if (!json_object_has_member(params, "k")) { k = 0; return; }\n'):
        print("[SELF-TEST FAIL] 에러 응답 없는 가드를 필수로 셌다")
        ok = False
                              
    if _req('if (!json_object_has_member(params, "k")) {\n'
            '  g_warning("no k: %s", PURE_RPC_ERR_INVALID_PARAMS_STR);\n}\n'):
        print("[SELF-TEST FAIL] return 없는 가드를 필수로 셌다")
        ok = False
                                    
    if _req('if (!json_object_has_member(params, "a")\n'
            '    && !json_object_has_member(params, "b")) {\n'
            '  gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "x");\n'
            '  return;\n}\n'):
        print("[SELF-TEST FAIL] && 조건 가드를 개별 필수로 셌다")
        ok = False
                                       
    if _req('if (!json_object_has_member(other, "k")) {\n'
            '  gchar *r = pure_rpc_build_error_response(id, PURE_RPC_ERR_INVALID_PARAMS, "x");\n'
            '  return;\n}\n'):
        print("[SELF-TEST FAIL] 다른 객체의 가드를 이 핸들러의 필수 키로 셌다")
        ok = False

                                                             
                                                
                                                     
                                                        
    def none_required(_method):
                                          
        return set()

                                                          
                                                               
    v, miss, _ = scan([("t.js", "fetchPost(EP.RPC(), { method:'t.approve', "
                                "params:{ index: i }, id:'x' });")],
                      lambda m: {"action_id"}, none_required)
    if [(f, meth, extra) for f, meth, extra, _ in v] != [("t.js", "t.approve", ["index"])] or miss:
        print(f"[SELF-TEST FAIL] 위반 양성 판정이 어긋난다: {v} / {miss}")
        ok = False

                                                            
                                           
                                                  
    v, miss, _ = scan([("t.js", "fetchPost(EP.RPC(), { method:'t.opt', "
                                "params:{ a: 1 }, id:'x' });")],
                      lambda m: {"a", "b", "c"}, none_required)
    if v or miss:
        print(f"[SELF-TEST FAIL] 선택 파라미터가 위반으로 잡힌다: {v} / {miss}")
        ok = False

                                                 
                                   
    v, miss, _ = scan([("t.js", "fetchPost(EP.RPC(), { method:'t.exact', "
                                "params:{ a: 1, b: 2 }, id:'x' });")],
                      lambda m: {"a", "b"}, lambda m: {"a", "b"})
    if v or miss:
        print(f"[SELF-TEST FAIL] 보내는 키와 읽는 키가 같은데 위반으로 잡힌다: {v} / {miss}")
        ok = False

                                                               
    v, miss, unc = scan([("t.js", "fetchPost(EP.RPC(), { method:'t.opaque', "
                                  "params:{ a: 1 }, id:'x' });")],
                        lambda m: None, lambda m: {"zzz"})
    if v or miss or unc != {"t.opaque"}:
        print(f"[SELF-TEST FAIL] 커버 불가 메서드 처리가 어긋난다: {v} / {miss} / {unc}")
        ok = False

                                                     
    v, miss, _ = scan([("t.js", "fetchPost(EP.RPC(), { method:'t.del', "
                                "params:{ a: 1 }, id:'x' });")],
                      lambda m: {"a", "username"}, lambda m: {"a", "username"})
    if v or [(f, meth, lack) for f, meth, lack, _ in miss] \
            != [("t.js", "t.del", ["username"])]:
        print(f"[SELF-TEST FAIL] 역방향 양성 판정이 어긋난다: {v} / {miss}")
        ok = False

                                                                   
                                                                     
    inj = rest_injected_keys()
    if "name" not in inj or "username" in inj:
        print("[SELF-TEST FAIL] REST 주입 키 표본이 바뀌었다 — 아래 (6) 의 표본 키를 "
              f"rest_server.c 현황에 맞게 고를 것(name in inj={'name' in inj}, "
              f"username in inj={'username' in inj})")
        ok = False
    v, miss, _ = scan([("t.js", "fetchPost(EP.AUTH_USERS(), { role: r });")],
                      lambda m: {"role", "name", "username"},
                      lambda m: {"name", "username"})
    if v or [(f, meth, lack) for f, meth, lack, _ in miss] \
            != [("t.js", "auth.user.create", ["username"])]:
        print(f"[SELF-TEST FAIL] REST 주입 키 제외가 어긋난다: {v} / {miss}")
        ok = False

                                                           
                                       
    v, miss, _ = scan([("t.js", "fetchPost(EP.RPC(), { method:'t.direct', "
                                "params:{ a: 1 }, id:'x' });")],
                      lambda m: {"a", "name"}, lambda m: {"name"})
    if [(f, meth, lack) for f, meth, lack, _ in miss] != [("t.js", "t.direct", ["name"])]:
        print(f"[SELF-TEST FAIL] RPC 직결에 REST 주입 제외가 잘못 적용된다: {miss}")
        ok = False

                             
    sent = extract_sent("async function f(n) {\n  const body = { a: 1, 'b': 2, n };\n"
                        "  if (n) body.c = 3;\n  body['d'] = 4;\n"
                        "  await fetchPost(EP.FOO(n), body);\n}\n")
    if not sent or sent[0][0] != ("FOO", "POST") or sent[0][1] != {"a", "b", "n", "c", "d"} or sent[0][3] is not True:
        print(f"[SELF-TEST FAIL] 변수 본문 추출 실패: {sent}")
        ok = False

    sent = extract_sent("function f() { var p = { a: { inner: 1 }, b: [ { deep: 2 } ] }; fetchPut(EP.FOO(), p); }")
    if not sent or sent[0][1] != {"a", "b"}:
        print(f"[SELF-TEST FAIL] 중첩 객체 키를 본문 키로 셌다: {sent}")
        ok = False

    sent = extract_sent("function g() { var body = { wrong: 1 }; use(body); }\n"
                        "function f(body) { fetchPost(EP.FOO(), body); }\n")
    if not sent or sent[0][1] is not None:
        print(f"[SELF-TEST FAIL] 다른 함수의 지역 변수를 본문으로 읽었다: {sent}")
        ok = False

    sent = extract_sent("function f() { var body = build(); fetchPost(EP.FOO(), body); }")
    if not sent or sent[0][1] is not None:
        print(f"[SELF-TEST FAIL] 비리터럴 본문을 해석했다: {sent}")
        ok = False

    sent = extract_sent("function f(k) { var b = { a: 1, ...rest }; fetchPost(EP.FOO(), b); }")
    if not sent or sent[0][1] != {"a"} or sent[0][3] is not False:
        print(f"[SELF-TEST FAIL] 스프레드 본문의 complete 판정 실패: {sent}")
        ok = False
    v, miss, unc = scan([("t.js", "function f(k) { var b = { a: 1 }; b[k] = 2; "
                                  "fetchPost(EP.AUTH_USERS(), b); }")],
                        lambda m: {"a", "username"}, lambda m: {"username"})
    if v or miss:
        print(f"[SELF-TEST FAIL] 키를 다 모르는 본문에 역방향을 적용했다: {v} / {miss}")
        ok = False
    if unc != {"EP.AUTH_USERS[POST]"}:
        print(f"[SELF-TEST FAIL] 키를 다 모르는 본문이 uncovered 로 가지 않는다: {unc}")
        ok = False

    v, _, unc = scan([("t.js", "function f(k) { var b = { bogus: 1 }; b[k] = 2; "
                               "fetchPost(EP.AUTH_USERS(), b); }")],
                     lambda m: {"a"}, lambda m: set())
    if [(f, meth, extra) for f, meth, extra, _ in v] != [("t.js", "auth.user.create", ["bogus"])] \
            or unc != {"EP.AUTH_USERS[POST]"}:
        print(f"[SELF-TEST FAIL] 키를 다 모르는 본문의 정방향 판정이 어긋난다: {v} / {unc}")
        ok = False

    sent = extract_sent("fetchPost(EP.RPC(), { method:'a.b', params:{ x: 1, ...rest }, id:'i' });")
    if not sent or sent[0][1] != {"x"} or sent[0][3] is not False:
        print(f"[SELF-TEST FAIL] RPC params 스프레드의 complete 판정 실패: {sent}")
        ok = False

    v, miss, unc = scan([("t.js", "function f() { const b = { role: 1, bogus: 2 }; "
                                  "fetchPost(EP.AUTH_USERS(), b); }")],
                        lambda m: {"role", "username"}, lambda m: {"username"})
    if [(f, meth, extra) for f, meth, extra, _ in v] != [("t.js", "auth.user.create", ["bogus"])] \
            or [(f, meth, lack) for f, meth, lack, _ in miss] != [("t.js", "auth.user.create", ["username"])] or unc:
        print(f"[SELF-TEST FAIL] 변수 본문 scan 판정이 어긋난다: {v} / {miss} / {unc}")
        ok = False
    v, miss, unc = scan([("t.js", "function f(b) { fetchPost(EP.AUTH_USERS(), b); }")],
                        lambda m: {"role"}, lambda m: set())
    if v or miss or unc != {"EP.AUTH_USERS[POST]"}:
        print(f"[SELF-TEST FAIL] 해석 못 한 변수 본문이 uncovered 로 가지 않는다: {v} / {miss} / {unc}")
        ok = False

    if EP_TO_RPC.get(("CTR_LIST", "POST")) != "container.create":
        print("[SELF-TEST FAIL] EP_TO_RPC 에 CTR_LIST POST 가 없다")
        ok = False

    if forwards_params('x = g_strdup(json_object_get_string_member(params, "k"));'):
        print("[SELF-TEST FAIL] 중첩 판독기를 params 전달로 오인")
        ok = False
    if not forwards_params('g_strdup(helper(params));'):
        print("[SELF-TEST FAIL] 중첩 호출의 params 전달을 놓쳤다")
        ok = False
    if forwarded_calls('helper(a, params, b);', "params") != [("helper", 1)]:
        print("[SELF-TEST FAIL] 넘긴 인자 위치를 못 읽었다")
        ok = False
    if forwarded_calls('helper(wrap(params), b);', "params") != [("helper", None), ("wrap", 0)]:
        print(f"[SELF-TEST FAIL] 표현식 안의 params 판정이 어긋난다: {forwarded_calls('helper(wrap(params), b);', 'params')}")
        ok = False
    if _read_keys_in('json_object_get_members(params);', 'json_object_get_members(params);', "params", 1) is not None:
        print("[SELF-TEST FAIL] 멤버 열거 본문을 키 0개로 읽었다(커버 불가여야 한다)")
        ok = False

    if extract_sent("fetchPost(EP.FOO(), { jsonrpc: '2.0', k: 1 });")[0][1] != {"k"}:
        print("[SELF-TEST FAIL] JSON-RPC 봉투 필드가 params 키로 셈해진다")
        ok = False
    print("[SELF-TEST PASS] 게이트 추출·판정이 정확" if ok else "[SELF-TEST FAILED]")
    return 0 if ok else 1


def main():
    if "--self-test" in sys.argv:
        return self_test()
    if "--generate" in sys.argv:
        _, _, uncovered = scan()
        prev = load_uncovered_annotated()
        width = max((len(u) for u in uncovered), default=0)
        lines = []
        for u in sorted(uncovered):
            why = prev.get(u, "")
            lines.append(f"{u:<{width}}  # {why}" if why else u)
        UNCOVERED_FILE.write_text(
            "# check_fe_rpc_params 가 정적으로 커버하지 못한 호출 대상 명단(단조감소 래칫).\n"
            "# EP_TO_RPC 매핑을 추가하거나 호출부·핸들러를 정적 추출 가능한 형태로 바꾸면\n"
            "# 이 명단에서 제거한다. 총량은 줄어들기만 해야 한다(증가 시 게이트 FAIL).\n"
            "#\n"
            "# 형식: <호출대상>  # <커버 못 하는 이유>\n"
            "#   EP.NAME[VERB] — REST 본문 호출, NAME 은 ui/modules/endpoints.js 레지스트리 키\n"
            "#   a.b.c         — 직접 JSON-RPC 호출의 메서드명\n"
            "# 사유는 손으로 적는다. --generate 는 살아남은 항목의 사유를 그대로 옮겨 적는다.\n"
            + "\n".join(lines) + "\n", encoding="utf-8")
        print(f"[generate] uncovered {len(uncovered)}건 기록")
        return 0
    return run_check()


if __name__ == "__main__":
    sys.exit(main())
