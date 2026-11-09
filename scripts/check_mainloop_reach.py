#!/usr/bin/env python3
                          
                                                                       
                                                                                                    
                                                                               
                                                                     
                                                                    
 
                      
                                                                        
                                 
                                               
from __future__ import annotations

import argparse
import re
import sys
from collections import defaultdict, deque
from pathlib import Path

CALL_RE = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
IDENT_RE = re.compile(r"\b([A-Za-z_]\w*)\b")
MACRO_RE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)\(([^)]*)\)((?:[^\n]*\\\n)*[^\n]*)", re.M)
ROUTE_RE = re.compile(r'g_hash_table_insert\(\s*\w*routes?\w*\s*,\s*"([a-z0-9_.]+)"\s*,\s*\(gpointer\)\s*(\w+)\s*\)')
INCLUDE_C_RE = re.compile(r'^\s*#\s*include\s+"([^"]+\.c)"', re.M)
LXC_RE = re.compile(r"\b(\w+)\s*->\s*(\w+)\s*\(\s*\1\s*[,)]")
OFFLOAD_RE = re.compile(r'\{\s*"([a-z0-9_.]+)"\s*,\s*PCV_OFFLOAD_LANE_(\w+)\s*\}')

                                               
THREAD_APIS = {"g_task_run_in_thread", "g_task_run_in_thread_sync", "g_thread_new", "g_thread_try_new",
               "g_thread_pool_new", "pcv_worker_pool_push", "pthread_create", "pcv_poll_async_submit",
               "pcv_rest_async_worker", "pcv_virt_cleanup_bounded", "pcv_rest_defer_rendered"}
                                                                
VIR_LOCAL = {
    "virDomainFree", "virDomainRef", "virDomainGetName", "virDomainGetUUID", "virDomainGetUUIDString", "virDomainGetID",
    "virDomainGetConnect", "virConnectRef", "virConnectIsAlive", "virGetLastError", "virGetLastErrorMessage",
    "virGetLastErrorCode", "virGetLastErrorDomain", "virResetLastError", "virResetError", "virFreeError", "virCopyLastError",
    "virSetErrorFunc", "virConnSetErrorFunc", "virDomainSnapshotFree", "virNetworkFree", "virStoragePoolFree",
    "virStorageVolFree", "virNodeDeviceFree", "virSecretFree", "virInterfaceFree", "virDomainInterfaceFree",
    "virDomainStatsRecordListFree", "virEventRegisterDefaultImpl", "virEventRunDefaultImpl", "virEventRegisterImpl",
    "virInitialize", "virNetworkDHCPLeaseFree", "virDomainDiskErrorFree", "virDomainIOThreadInfoFree", "virDomainFSInfoFree",
    "virGetVersion", "virStreamFree", "virStreamRef",
}
VIR_LOCAL_PREFIX = ("virTypedParams",)
KINDS = {
    "virt-open": {"virConnectOpen", "virConnectOpenAuth", "virConnectOpenReadOnly", "pcv_virt_open_bounded"},
    "pool": {"virt_conn_pool_acquire", "virt_conn_pool_try_acquire"},
    "spawn-unbounded": {"pcv_spawn_sync", "pcv_spawn_sync_profile", "pcv_spawn_sync_status", "pcv_spawn_sync_env",
                        "pcv_spawn_sync_stdin", "pcv_spawn_pipe_sync", "g_spawn_sync", "g_spawn_command_line_sync",
                        "g_subprocess_communicate", "g_subprocess_communicate_utf8", "g_subprocess_wait",
                        "g_subprocess_wait_check"},
    "spawn-bounded": {"pcv_spawn_sync_bounded", "pcv_spawn_sync_timeout", "pcv_poll_guard_spawn"},
    "dns": {"getaddrinfo", "gethostbyname", "gethostbyname2", "getnameinfo", "g_resolver_lookup_by_name",
            "g_resolver_lookup_by_address", "g_resolver_lookup_records"},
    "sleep": {"g_usleep", "usleep", "sleep", "nanosleep", "clock_nanosleep"},
    "http": {"soup_session_send", "soup_session_send_and_read", "curl_easy_perform", "g_socket_client_connect_to_host",
             "g_socket_client_connect", "g_socket_connect"},
    "wait": {"g_thread_join", "pthread_join", "g_cond_wait", "g_cond_wait_until", "waitpid", "g_main_context_iteration"},
    "lxc": {"lxc_container_new"},
    "uds-sync": {"pcv_rest_rpc_over_uds", "pcv_rest_rpc_over_uds_timeout"},
                                                                                                   
                                                    
    "gvir": {"gvir_connection_open", "gvir_connection_fetch_domains", "gvir_domain_get_config", "gvir_domain_get_id"},
}
NAME_KIND = {n: k for k, ns in KINDS.items() for n in ns}
LXC_METHODS = {"state", "get_ips", "is_running", "start", "stop", "shutdown", "reboot", "create", "destroy", "clone",
               "snapshot", "snapshot_restore", "snapshot_list", "snapshot_destroy", "attach", "attach_run_wait", "is_defined",
               "load_config", "save_config", "set_config_item", "get_config_item", "init_pid", "freeze", "unfreeze",
               "wait", "add_device_node", "remove_device_node", "attach_interface", "detach_interface", "mount", "umount"}
                                   
STOP = (KINDS["spawn-unbounded"] | KINDS["spawn-bounded"] | KINDS["pool"] | KINDS["uds-sync"]
        | {"pcv_spawn_fire", "pcv_virt_open_bounded"})
SKIP_FILES = {"src/utils/pcv_spawn.c", "src/modules/virt/virt_conn_pool.c", "src/modules/virt/pcv_virt_open.c"}
CRITICAL_MUTEX = {"g_mutation_mu"}                                                 
                                                          
SOURCE_APIS = {"g_timeout_add": (1, None), "g_timeout_add_seconds": (1, None), "g_timeout_add_full": (2, None),
               "g_timeout_add_seconds_full": (2, None), "g_idle_add": (0, None), "g_idle_add_full": (1, None),
               "g_unix_signal_add": (1, None), "g_unix_fd_add": (2, None), "g_main_context_invoke": (1, 0),
               "g_main_context_invoke_full": (2, 0), "pcv_drain_invoke": (1, 0), "pcv_drain_idle": (1, 0),
               "pcv_drain_try_invoke": (2, 0), "pcv_drain_timeout": (1, None),
               "virConnectDomainEventRegisterAny": (3, None)}
                                            
REF_NOT_MAIN = {"_post_real": "pcv_webpush.c: 배치 구조체의 함수 포인터로 저장돼 _batch_worker(GTask 스레드)에서만 호출",
                                                                                                         
                                                                                   
                                                                                                 
                "_ctr_clone_release_locks": "handler_container_net.c: pcv_lxc_clone_async의 해제 훅 — 복제 워커(GTask 스레드)의 "
                                            "_clone_task_return에서만 호출"}
                                                                            
MAIN_ONLY = {"pcv_poll_async_submit", "pcv_singleflight_join", "pcv_singleflight_complete", "pcv_guest_agent_status_submit",
             "get_vm_metrics"}                                                                  
                                                                            
GUARD_OUT = {"pcv_spawn_sync_status", "pcv_spawn_sync_profile", "pcv_spawn_sync_env", "pcv_spawn_sync_stdin",
             "pcv_spawn_pipe_sync", "g_spawn_sync", "g_spawn_command_line_sync", "g_subprocess_communicate",
             "g_subprocess_communicate_utf8", "g_subprocess_wait", "g_subprocess_wait_check"}
CRITICAL_ZERO = True                                                                                               
CRITICAL_KINDS = {"mutex-critical", "dns"}
OPEN_TOKEN_ENFORCED = True                                                                
OPEN_TOKEN_FILE = "src/modules/virt/pcv_virt_open.c"
MIN_FUNCTIONS, MIN_ROUTES, MIN_FILES = 3000, 300, 200
SENTINEL_FILES = ("src/modules/dispatcher/handler_vm_device.c", "src/modules/virt/vm_ops.c", "src/api/dispatcher.c")
SENTINEL_FUNCTIONS = ("purecvisor_dispatcher_dispatch", "handle_monitor_fleet_rpc", "_on_request")


def sink_kind(name: str) -> str | None:
    if name in NAME_KIND:
        return NAME_KIND[name]
    if re.fullmatch(r"vir[A-Z]\w+", name) and name not in VIR_LOCAL and not name.startswith(VIR_LOCAL_PREFIX):
        return "libvirt"
    return None


class Fn:
    __slots__ = ("file", "name", "static", "body", "calls", "refs", "sinks", "thread_refs", "params", "raw", "decl")


class Graph:
    def __init__(self, root: Path):
        self.root = root
        sys.path.insert(0, str(root / "scripts" / "split"))
        import c_blocks                                                  
        self.cb = c_blocks
        self.fns: dict[tuple[str, str], Fn] = {}
        self.by_name: dict[str, list[Fn]] = defaultdict(list)
        self.tu: dict[str, str] = {}
        self.files = self._source_files()
        self._load()
        self._analyse()
        self.thread_entry = self._thread_entries()

    def _source_files(self) -> list[str]:
        mk = (self.root / "Makefile").read_text()
        listed = {p for p in re.findall(r"(src/[\w/.-]+\.c)\b", mk)
                  if not p.startswith(("src/cli/", "src/bpf/")) and (self.root / p).exists()}
        for p in sorted(listed):
            self.tu[p] = p
            for inc in INCLUDE_C_RE.findall((self.root / p).read_text(errors="replace")):
                q = str(Path(p).parent / inc)
                if (self.root / q).exists():
                    listed.add(q)
                    self.tu[q] = p
        return sorted(listed)

    def _add(self, rel, name, static, body, params, raw="", decl=""):
        key = (rel, name)
        if key in self.fns:                                         
            self.fns[key].body += "\n" + body
            return
        f = Fn()
        f.file, f.name, f.static, f.body, f.params = rel, name, static, body, params
        f.raw, f.decl = raw, decl
        self.fns[key] = f
        self.by_name[name].append(f)

    def _load(self):
        for rel in self.files:
            text = (self.root / rel).read_text(errors="replace")
            m = self.cb.masked(text)
            for b in self.cb.scan(text)[0]:
                params = IDENT_RE.findall(m[b.paren_open:m.find(")", b.paren_open)])
                self._add(rel, b.name, b.is_static, m[b.body_open:b.body_close], params,
                          text[b.doc_start:b.body_close], m[b.decl_start:b.paren_open])
        headers = [str(p.relative_to(self.root)) for d in ("src", "include") for p in (self.root / d).rglob("*.h")]
        for rel in self.files + headers:
            text = (self.root / rel).read_text(errors="replace")
            for mt in MACRO_RE.finditer(self.cb.masked(text)):
                if mt.group(1) not in self.by_name and CALL_RE.search(mt.group(3)):
                    self._add(rel, mt.group(1), False, mt.group(3), [])

    def resolve(self, caller: str, name: str) -> Fn | None:
        hit = self.fns.get((caller, name))
        if hit:
            return hit
        tu = self.tu.get(caller, caller)
        cands = self.by_name.get(name, [])
        for f in cands:
            if self.tu.get(f.file, f.file) == tu:
                return f
        for f in cands:
            if not f.static:
                return f
        return None

    def _thread_spans(self, f: Fn, apis: set[str]):
        spans, names = [], set()
        for mt in CALL_RE.finditer(f.body):
            if mt.group(1) in apis:
                i, d = mt.end(), 1
                while i < len(f.body) and d:
                    d += (f.body[i] == "(") - (f.body[i] == ")")
                    i += 1
                spans.append((mt.start(), i))
                names |= set(IDENT_RE.findall(f.body[mt.end():i]))
        return spans, names

    def _analyse(self):
                                                                         
        apis = set(THREAD_APIS)
        changed = True
        while changed:
            changed = False
            for f in self.fns.values():
                if f.name in apis:
                    continue
                for mt in re.finditer(r"\b(?:g_task_run_in_thread|pcv_worker_pool_push)\s*\(\s*\w+\s*,\s*(\w+)\s*\)", f.body):
                    if mt.group(1) in f.params:
                        apis.add(f.name)
                        changed = True
                        break
        self.thread_apis = apis
        for f in self.fns.values():
            spans, names = self._thread_spans(f, apis)
            f.thread_refs = names
            called = set()
            f.calls, f.refs, f.sinks = [], [], []
            for mt in CALL_RE.finditer(f.body):
                n = mt.group(1)
                called.add(mt.start(1))
                kind = sink_kind(n)
                if kind:
                    f.sinks.append((kind, n))
                if n in self.by_name:
                    f.calls.append(n)
            for mt in LXC_RE.finditer(f.body):
                if mt.group(2) in LXC_METHODS:
                    f.sinks.append(("lxc-method", "->" + mt.group(2)))
            for mu in CRITICAL_MUTEX:
                if re.search(r"g_mutex_lock\(\s*&\s*" + mu + r"\s*\)", f.body):
                    f.sinks.append(("mutex-critical", mu))
            for mt in IDENT_RE.finditer(f.body):
                n = mt.group(1)
                if mt.start(1) in called or n not in self.by_name or n in REF_NOT_MAIN:
                    continue
                if any(a <= mt.start(1) < b for a, b in spans):
                    continue
                f.refs.append(n)

    def _thread_entries(self) -> set[str]:
        out = set()
        for f in self.fns.values():
            out |= f.thread_refs & set(self.by_name)
        return out

    def walk(self, start: Fn, refs: bool = True) -> dict:
        seen = {(start.file, start.name): None}
        q = deque([start])
        while q:
            f = q.popleft()
            todo = [(n, True) for n in f.calls] + ([(n, False) for n in f.refs] if refs else [])
            for n, is_call in todo:
                if n in STOP:
                    continue
                g = self.resolve(f.file, n)
                if g is None or (g.file, g.name) in seen or (not is_call and g.name in self.thread_entry):
                    continue
                seen[(g.file, g.name)] = (f.file, f.name)
                q.append(g)
        return seen

    def routes(self) -> dict[str, str]:
        out = {}
        for rel in ("src/api/dispatcher.c", "src/bootstrap/pcv_rpc_bootstrap_single.c"):
            out.update(ROUTE_RE.findall((self.root / rel).read_text()))
        out.setdefault("vm.create", "handle_vm_create")
        return out

    def offload_table(self) -> dict[str, str]:
        p = self.root / "src/modules/dispatcher/rpc_offload.c"
        if not p.exists():
            return {}
                                                                                        
        text = p.read_text()
        code = "".join(ch if k != "m" or ch == "\n" else " " for ch, k in zip(text, self.cb.classify(text)))
        return dict(OFFLOAD_RE.findall(code))

    def main_sources(self) -> dict[str, Fn]:
        out = {}
        for f in self.fns.values():
            for mt in CALL_RE.finditer(f.body):
                spec = SOURCE_APIS.get(mt.group(1))
                if not spec:
                    continue
                i, d, args, cur = mt.end(), 1, [], ""
                while i < len(f.body) and d:
                    ch = f.body[i]
                    d += (ch == "(") - (ch == ")")
                    if d == 1 and ch == ",":
                        args.append(cur)
                        cur = ""
                    elif d:
                        cur += ch
                    i += 1
                args.append(cur)
                if spec[1] is not None and spec[1] < len(args) and args[spec[1]].strip() not in ("NULL", "nullptr"):
                    continue                        
                if spec[0] < len(args):
                    for t in IDENT_RE.findall(args[spec[0]]):
                        g = self.resolve(f.file, t)
                        if g:
                            out[t] = g
        return out

    def sinks_of(self, reach: dict):
        for key in reach:
            f = self.fns[key]
            if f.file in SKIP_FILES:
                continue
            for kind, sink in f.sinks:
                yield kind, f.name, sink

    def chain(self, reach: dict, fname: str) -> str:
        for key in reach:
            if key[1] == fname:
                out, k = [], key
                while k:
                    out.append(k[1])
                    k = reach[k]
                return " <- ".join(out)
        return fname


def collect(g: Graph):
                                                           
    rows, reaches = set(), {}
    offload = g.offload_table()
    routes = g.routes()
    problems, violations = [], []
    for method, handler in sorted(routes.items()):
        start = g.resolve("src/api/dispatcher.c", handler)
        if start is None:
            problems.append(f"라우트 {method}의 핸들러 {handler}를 찾지 못했습니다")
            continue
        reach = g.walk(start)
        if method in offload:
            bad = {n for k in reach for n in g.fns[k].calls if n in MAIN_ONLY}
            if bad:
                violations.append(f"오프로드 라우트 {method}가 메인 전용 API {sorted(bad)}에 닿습니다")
            names = {n for k in reach for _, n in g.fns[k].sinks}
            lane = offload[method]
            if lane == "HOST_READ" and names & GUARD_OUT:
                violations.append(f"{method}(HOST_READ)가 가드 밖 spawn {sorted(names & GUARD_OUT)}에 닿습니다")
            spawns = {n for k, _, n in g.sinks_of(reach) if k.startswith("spawn")}
            if lane == "LIBVIRT_READ" and spawns:
                violations.append(f"{method}(LIBVIRT_READ)가 spawn {sorted(spawns)}에 닿습니다")
            continue
        root = "route:" + method
        reaches[root] = reach
        rows |= {("main", root, *s) for s in g.sinks_of(reach)}
    disp = g.resolve("src/api/dispatcher.c", "purecvisor_dispatcher_dispatch")
    if disp:
        reach = g.walk(disp, refs=False)
        reaches["dispatch"] = reach
        rows |= {("main", "dispatch", *s) for s in g.sinks_of(reach)}
    for name, fn in sorted(g.main_sources().items()):
        reach = g.walk(fn)
        root = "source:" + name
        reaches[root] = reach
        rows |= {("main", root, *s) for s in g.sinks_of(reach)}
    rest = g.resolve("src/api/rest_server.c", "_on_request")
    if rest:
        reach = g.walk(rest)
        reaches["rest:_on_request"] = reach
        rows |= {("rest", "rest:_on_request", *s) for s in g.sinks_of(reach)}
    if CRITICAL_ZERO:
        violations += [f"메인 도달 금지: {fmt(r)}" for r in sorted(rows) if r[2] in CRITICAL_KINDS]
    if OPEN_TOKEN_ENFORCED:                                                                           
        for rel in g.files:
            if rel != OPEN_TOKEN_FILE and re.search(r"\bvirConnectOpen(?:ReadOnly|Auth)?\b",
                                                    g.cb.masked((g.root / rel).read_text(errors="replace"))):
                violations.append(f"virConnectOpen은 {OPEN_TOKEN_FILE} 밖 금지: {rel}")
    return rows, reaches, problems, violations, routes, offload


def guard_empty(g: Graph, rows, routes) -> list[str]:
    errs = []
    if len(g.fns) < MIN_FUNCTIONS:
        errs.append(f"함수 {len(g.fns)}개 < 하한 {MIN_FUNCTIONS}")
    if len(routes) < MIN_ROUTES:
        errs.append(f"라우트 {len(routes)}개 < 하한 {MIN_ROUTES}")
    if len(g.files) < MIN_FILES:
        errs.append(f"소스 {len(g.files)}개 < 하한 {MIN_FILES}")
    errs += [f"표지 파일 없음: {p}" for p in SENTINEL_FILES if p not in g.files]
    errs += [f"표지 함수 없음: {n}" for n in SENTINEL_FUNCTIONS if n not in g.by_name]
    errs += [f"REF_NOT_MAIN 낡음: {n}" for n in REF_NOT_MAIN if n not in g.by_name]
    if not rows:
        errs.append("도달 집합이 비었습니다(빈 집합으로 통과하지 않는다)")
    return errs


def crosscheck(g: Graph, ci_dir: Path) -> int:
                                                                                                     
    edge = re.compile(r'edge: \{ sourcename: "([^"]+)" targetname: "([^"]+)"(?: label: "([^"]*)")? \}')
    node = re.compile(r'node: \{ title: "([^"]+)" label: "[^"]*"(.*)\}')
    edges, defined = defaultdict(set), defaultdict(list)
    files = list(ci_dir.glob("*.ci"))
    if len(files) < MIN_FILES:
        print(f"[crosscheck] .ci {len(files)}개 < {MIN_FILES}", file=sys.stderr)
        return 2
    for ci in files:
        for line in ci.read_text(errors="replace").splitlines():
            m = edge.match(line)
            if m:
                edges[m.group(1)].add((m.group(2), m.group(3) or ""))
                continue
            m = node.match(line)
            if m and "ellipse" not in m.group(2):
                defined[m.group(1).split(":")[-1]].append(m.group(1))
    titles = {t for ts in defined.values() for t in ts}
    kinds = ("libvirt", "pool", "spawn-unbounded", "spawn-bounded", "dns", "sleep", "wait", "http", "virt-open")
    diff = 0
    routes = g.routes()
    for method, handler in sorted(routes.items()):
        start = g.resolve("src/api/dispatcher.c", handler)
        text = {(k, n) for k, _, n in g.sinks_of(g.walk(start, refs=False)) if k in kinds}
        seen, q, comp = set(defined.get(handler, [])), list(defined.get(handler, [])), set()
        while q:
            for y, label in edges.get(q.pop(), ()):
                n = y.split(":")[-1]
                k = sink_kind(n)
                if k in kinds and not any(label.startswith(f) for f in SKIP_FILES):
                    comp.add((k, n))
                if n not in STOP and y in titles and y not in seen:
                    seen.add(y)
                    q.append(y)
        if text != comp:
            diff += 1
            print(f"DIFF {method}: 텍스트만 {sorted(text - comp)[:4]} 컴파일러만 {sorted(comp - text)[:4]}")
    print(f"[crosscheck] 라우트 {len(routes)}개 중 차이 {diff}개")
    return 1 if diff else 0


MAIN_COMMENT_RE = re.compile(r"메인 ?스레드|메인 ?루프|main ?thread|Main Thread|단일 ?스레드|GMainLoop 스레드")
PTR_RETURN_RE = re.compile(r"\breturn\s+(?:\([^)]*\)\s*)?(g_hash_table_lookup|g_ptr_array_index|g_list_nth_data|g_queue_peek_\w+)\s*\(")
LOCK_RE = re.compile(r"\b(g_mutex_lock|g_rec_mutex_lock|g_rw_lock_writer_lock|g_rw_lock_reader_lock|g_mutex_locker_new)\s*\(\s*&?\s*([^()]+?(?:\(\))?)\s*\)")
UNLOCK_RE = re.compile(r"\b(?:g_mutex_unlock|g_rec_mutex_unlock|g_rw_lock_writer_unlock|g_rw_lock_reader_unlock)\s*\(\s*&?\s*([^()]+?)\s*\)")
GUARD_TOKENS = ("g_mutex_lock", "G_LOCK", "g_atomic", "g_once", "atomic_", "g_rw_lock", "g_rec_mutex", "g_mutex_locker")


def _file_statics(g: Graph) -> dict[str, set[str]]:
    statics: dict[str, set[str]] = {}
    for rel in g.files:
        names = set()
        for d in g.cb.scan((g.root / rel).read_text(errors="replace"))[1]:
            x = g.cb.masked(d.text).strip()
            head = re.sub(r"\[[^\]]*\]", "", x.split("=")[0]).rstrip(" ;\n")
            if not x.startswith("static") or re.match(r"static\s+const\b", x) or "(" in head:
                continue
            m = re.search(r"([A-Za-z_]\w*)\s*$", head)
            if m:
                names.add(m.group(1))
        statics[rel] = names
    return statics


def main_reach(g: Graph, exclude: set[str]) -> dict:
                                                        
    out = {}
    routes = g.routes()
    roots = [("route:" + m, g.resolve("src/api/dispatcher.c", h)) for m, h in routes.items() if m not in exclude]
    roots.append(("dispatch", g.resolve("src/api/dispatcher.c", "purecvisor_dispatcher_dispatch")))
    roots += [("source:" + n, f) for n, f in g.main_sources().items()]
    for name, fn in roots:
        if fn:
            for key in g.walk(fn, refs=not name == "dispatch"):
                out.setdefault(key, name)
    return out


def shared_state(g: Graph, method: str, exclude: set[str]) -> int:
                                                                                 
                                     
    start = g.resolve("src/api/dispatcher.c", g.routes().get(method, ""))
    if start is None:
        print(f"라우트 없음: {method}", file=sys.stderr)
        return 2
    statics = _file_statics(g)
    main = main_reach(g, exclude | {method})
    local = re.compile(r"^\s*static\s+(?!const\b)(?!__thread\b)[^;()=]*?\b([A-Za-z_]\w*)\s*(?:=|;|\[)", re.M)
    for key in g.walk(start):
        f = g.fns[key]
        ids = set(IDENT_RE.findall(f.body))
        if not any(t in f.body for t in GUARD_TOKENS):
            for v in sorted(statics.get(f.file, set()) & ids):
                write = re.compile(r"\b" + re.escape(v) + r"\b(?:\.\w+|->\w+|\[[^\]]*\])*\s*=[^=]|(?:destroy|free|unref|clear|remove|insert|replace|add)\w*\s*\([^;]*\b" + re.escape(v) + r"\b")
                writers = [o.name + ("*" if (o.file, o.name) in main else "") for o in g.fns.values()
                           if o.file == f.file and write.search(o.body)]
                print(f"unlocked\t{Path(f.file).name}:{v}\t{f.name}\t쓰는 함수(*=메인): {','.join(writers) or '-'}")
        for m in local.finditer(f.body):
            print(f"local-static\t{Path(f.file).name}:{m.group(1)}\t{f.name}")
        if "*" in f.decl and (PTR_RETURN_RE.search(f.body)):
            print(f"ptr-return\t{Path(f.file).name}\t{f.name}")
        hit = MAIN_COMMENT_RE.search(f.raw)
        if hit:
            line = f.raw[f.raw.rfind("\n", 0, hit.start()) + 1:f.raw.find("\n", hit.end())].strip()[:110]
            print(f"comment\t{Path(f.file).name}\t{f.name}\t{line}")
    return 0


def lock_regions(f: Fn):
                                                   
    for m in LOCK_RE.finditer(f.body):
        name = m.group(2)
        end = len(f.body)
        for u in UNLOCK_RE.finditer(f.body, m.end()):
            if u.group(1) == name:
                end = u.start()
                break
        yield name, m.end(), end


def lock_audit(g: Graph, exclude: set[str]) -> int:
                                                                                 
    blocking = {"libvirt", "pool", "spawn-unbounded", "spawn-bounded", "dns", "http", "sleep", "lxc", "lxc-method", "virt-open", "uds-sync"}
    closure: dict[tuple[str, str], set] = {}
    def sinks_under(f: Fn, name: str):
        kind = sink_kind(name)
        if kind in blocking:
            return {(kind, name)}
        g2 = g.resolve(f.file, name)
        if g2 is None:
            return set()
        key = (g2.file, g2.name)
        if key not in closure:
            closure[key] = {(k, n) for k, _, n in g.sinks_of(g.walk(g2, refs=False)) if k in blocking}
        return closure[key]
    holders = defaultdict(list)
    acquirers = defaultdict(list)
    for f in g.fns.values():
        for name, a, b in lock_regions(f):
            key = f"{Path(f.file).name}:{name}"
            acquirers[key].append(f)
            found = set()
            for mt in CALL_RE.finditer(f.body[a:b]):
                found |= sinks_under(f, mt.group(1))
            if found:
                holders[key].append((f.name, sorted(found)[:3]))
    main = main_reach(g, exclude)
    routes = g.routes()
    moved = {m: set(g.walk(g.resolve("src/api/dispatcher.c", routes[m]))) for m in exclude if m in routes}
    print("잠금\t차단 표지를 지나는 보유 함수\t메인 획득 함수(대표 root)\t보유 함수에 닿는 옮긴 라우트")
    for key, hs in sorted(holders.items()):
        hnames = {h for h, _ in hs}
        main_acq = sorted({f"{f.name}({main[(f.file, f.name)]})" for f in acquirers[key] if (f.file, f.name) in main})
        hit = sorted(m for m, r in moved.items() if any(k[1] in hnames and Path(k[0]).name == key.split(":")[0] for k in r))
        print(f"{key}\t{'; '.join(f'{h}:{s}' for h, s in hs)[:200]}\t{','.join(main_acq)[:300] or '-'}\t{','.join(hit) or '-'}")
    return 0


def _load_baseline(path: Path) -> set:
    return {tuple(line.split("\t")) for line in path.read_text().splitlines() if line and not line.startswith("#")}


def fmt(row) -> str:
    return "\t".join(row)


def _write_tree(root: Path, handlers: dict[str, str], routes: dict[str, str]) -> None:
                                                                              
    (root / "scripts/split").mkdir(parents=True, exist_ok=True)
    (root / "scripts/split/c_blocks.py").write_text((Path(__file__).resolve().parent / "split/c_blocks.py").read_text())
    (root / "src/api").mkdir(parents=True, exist_ok=True)
    (root / "src/bootstrap").mkdir(parents=True, exist_ok=True)
    (root / "src/bootstrap/pcv_rpc_bootstrap_single.c").write_text("")
    inserts = "".join(f'    g_hash_table_insert(routes, "{m}", (gpointer){h});\n' for m, h in routes.items())
    (root / "src/api/dispatcher.c").write_text(
        "void purecvisor_dispatcher_dispatch(void)\n{\n}\n"
        "static void _reg(void)\n{\n" + inserts + "}\n"
        "void handle_vm_create(void)\n{\n}\n")
    for rel, text in handlers.items():
        (root / rel).parent.mkdir(parents=True, exist_ok=True)
        (root / rel).write_text(text)
    listed = sorted(p for p in handlers if p.endswith(".c")) + ["src/api/dispatcher.c"]
    (root / "Makefile").write_text("SRCS = " + " ".join(listed) + "\n")


def self_test() -> int:
                                                                                               
    import tempfile                 
    global MIN_FUNCTIONS, MIN_ROUTES, MIN_FILES, SENTINEL_FILES, SENTINEL_FUNCTIONS, REF_NOT_MAIN
    saved = (MIN_FUNCTIONS, MIN_ROUTES, MIN_FILES, SENTINEL_FILES, SENTINEL_FUNCTIONS, REF_NOT_MAIN)
    MIN_FUNCTIONS, MIN_ROUTES, MIN_FILES, SENTINEL_FILES, SENTINEL_FUNCTIONS, REF_NOT_MAIN = 1, 1, 1, (), (), {}
    routes = {"a.get": "h"}
    cases = [
        ("직접 호출 표지", "void h(void)\n{\n    g_usleep(1);\n}\n", True),
        ("매크로에 숨은 표지", "#define M() virt_conn_pool_acquire()\nvoid h(void)\n{\n    M();\n}\n", True),
        ("스레드 시작 인자는 따라가지 않는다",
         "static void w(void *d)\n{\n    g_usleep(1);\n}\nvoid h(void)\n{\n    g_task_run_in_thread(t, w);\n}\n", False),
        ("완료 콜백은 따라간다",
         "static void cb(void *d)\n{\n    g_usleep(1);\n}\nvoid h(void)\n{\n    pcv_drain_task_new(NULL, NULL, cb, NULL);\n}\n", True),
        ("주석·문자열은 표지가 아니다", '/* g_usleep(1) */ void h(void)\n{\n    s = "g_usleep(1)";\n}\n', False),
    ]
    try:
        for label, text, want in cases:
            with tempfile.TemporaryDirectory() as td:
                root = Path(td)
                _write_tree(root, {"src/h.c": text}, routes)
                rows = collect(Graph(root))[0]
                got = any(r[1] == "route:a.get" for r in rows)
                if got != want:
                    print(f"[SELF-TEST FAIL] {label}: 기대 {want}, 실제 {got}", file=sys.stderr)
                    return 1
        with tempfile.TemporaryDirectory() as td:                                 
            root = Path(td)
            _write_tree(root, {"src/h.c": "void h(void)\n{\n    g_usleep(1);\n}\n",
                               "src/modules/dispatcher/rpc_offload.c": '/* { "a.get", PCV_OFFLOAD_LANE_LIBVIRT_READ }, */\n'}, routes)
            if not any(r[1] == "route:a.get" for r in collect(Graph(root))[0]):
                print("[SELF-TEST FAIL] 주석 처리한 오프로드 표 줄을 오프로드로 셌다", file=sys.stderr)
                return 1
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write_tree(root, {"src/h.c": "void h(void)\n{\n}\n"}, {"a.get": "없는_핸들러"})
            g = Graph(root)
            if not collect(g)[2]:
                print("[SELF-TEST FAIL] 미해석 라우트를 못 잡음", file=sys.stderr)
                return 1
            if not guard_empty(g, set(), {}):
                print("[SELF-TEST FAIL] 빈 집합을 못 잡음", file=sys.stderr)
                return 1
    finally:
        MIN_FUNCTIONS, MIN_ROUTES, MIN_FILES, SENTINEL_FILES, SENTINEL_FUNCTIONS, REF_NOT_MAIN = saved
    print("[PASS] check_mainloop_reach --self-test")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=str(Path(__file__).resolve().parent.parent))
    ap.add_argument("--baseline")
    ap.add_argument("--tighten", action="store_true", help="기준선을 줄인다(새 행이 있으면 rc 1)")
    ap.add_argument("--init-baseline", action="store_true", help="기준선이 없을 때 한 번만 만든다")
    ap.add_argument("--rebaseline", metavar="REASON", help="함수 이름 변경 등으로 행 키가 바뀔 때. 사유 필수")
    ap.add_argument("--lock-audit", action="store_true", help="차단 표지를 지나며 쥔 잠금과 메인 경로의 같은 잠금 획득(검토 보조)")
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--shared-state", metavar="METHOD", help="라우트의 공유 상태 접근을 보고한다(검토 보조)")
    ap.add_argument("--crosscheck", metavar="CI_DIR", help="gcc -fcallgraph-info .ci 디렉터리와 대조(1회성)")
    ap.add_argument("--self-test", action="store_true", help="합성 트리로 게이트 자체를 검증한다")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    root = Path(args.root).resolve()
    base = Path(args.baseline) if args.baseline else root / "scripts/mainloop_reach_baseline.txt"
    g = Graph(root)
    if args.shared_state:
        return shared_state(g, args.shared_state, set(g.offload_table()))
    if args.lock_audit:
        return lock_audit(g, set(g.offload_table()))
    if args.crosscheck:
        return crosscheck(g, Path(args.crosscheck))
    rows, reaches, problems, violations, routes, offload = collect(g)
    errs = guard_empty(g, rows, routes) + problems
    if errs:
        print("[check-mainloop-reach] 해석 실패:\n  " + "\n  ".join(errs), file=sys.stderr)
        return 2
    if violations:
        print("[check-mainloop-reach] FAIL:\n  " + "\n  ".join(violations))
        return 1
    route_roots = {r[1] for r in rows if r[1].startswith("route:") and r[2] != "spawn-bounded"}
    summary = (f"함수 {len(g.fns)}개, 라우트 {len(routes)}개(오프로드 {len(offload)}), 차단 도달 라우트 {len(route_roots)}개, "
               f"행 {len(rows)}개")
    if args.report:
        print(summary)
        for row in sorted(rows):
            print(fmt(row))
        return 0
    if args.rebaseline is not None and not args.rebaseline.strip():
        print("[check-mainloop-reach] --rebaseline은 사유가 필요하다", file=sys.stderr)
        return 2
    if args.tighten or args.init_baseline or args.rebaseline:
        known = _load_baseline(base) if base.exists() else None
        if args.tighten:
            if known is None:
                print(f"[check-mainloop-reach] 기준선 없음(--init-baseline): {base}", file=sys.stderr)
                return 2
            grown = rows - known
            if grown:
                for r in sorted(grown):
                    print(f"FAIL --tighten은 줄이기만 한다. 새 행: {fmt(r)}\n     사슬: {g.chain(reaches[r[1]], r[3])}")
                return 1
        elif args.init_baseline and known is not None:
            print("[check-mainloop-reach] 기준선이 이미 있다", file=sys.stderr)
            return 2
        header = "# mainloop reach baseline — --tighten(감소만), 이름 변경은 --rebaseline '<사유>'\n"
        if args.rebaseline:
            added = rows - (known or set())
            print(f"[check-mainloop-reach] --rebaseline 사유: {args.rebaseline}; 추가 {len(added)}행은 커밋 메시지에 그대로 적는다")
            for r in sorted(added):
                print("  +", fmt(r))
        base.write_text(header + "".join(fmt(r) + "\n" for r in sorted(rows)))
        print(f"[check-mainloop-reach] 기준선을 썼습니다: {summary}")
        return 0
    if not base.exists():
        print(f"[check-mainloop-reach] 기준선 없음: {base}", file=sys.stderr)
        return 2
    known = _load_baseline(base)
    if not known:
        print("[check-mainloop-reach] 기준선이 비었습니다", file=sys.stderr)
        return 2
    new, stale = rows - known, known - rows
    for r in sorted(new):
        print(f"FAIL 새 차단 도달: {fmt(r)}\n     사슬: {g.chain(reaches[r[1]], r[3])}")
    for r in sorted(stale):
        print(f"FAIL 낡은 기준선(줄었으면 --tighten): {fmt(r)}")
    if new or stale:
        return 1
    print(f"[check-mainloop-reach] PASS: {summary}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
