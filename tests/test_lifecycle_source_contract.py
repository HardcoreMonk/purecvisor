#!/usr/bin/env python3
                          
                                                                                                            
                                                                                            
                                                                                         
                                                                                       
 
                      
                                                                        
                                                                      
                       
                                
from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "split"))
import c_blocks              


def body_span(src: str, name: str) -> tuple[int, int]:
    m = c_blocks.masked(src)
    for b in c_blocks.scan(src)[0]:
        if b.name == name:
            return b.body_open, b.body_close + 1
    raise AssertionError(f"{name} 없음")


def body(src: str, name: str) -> str:
    lo, hi = body_span(src, name)
    return src[lo:hi]


def code_strings(src: str) -> str:
                                                               
    kinds = c_blocks.classify(src)
    return "".join(ch if k != "m" or ch == "\n" else " " for ch, k in zip(src, kinds))


def argv_var(seg: str, head: str) -> str | None:
                                                                       
    m = re.search(r"const\s+gchar\s*\*\s*(\w+)\s*\[\]\s*=\s*\{\s*" + head, seg)
    return m.group(1) if m else None


def spawn_at(seg: str, var: str | None) -> int:
                                                             
    if not var:
        return -1
    m = re.search(r"g_spawn_sync\s*\(\s*NULL\s*,\s*\(gchar\s*\*\*\)\s*" + re.escape(var) + r"\b", seg)
    return m.start() if m else -1


def stmt(seg: str, at: int) -> str:
                                                                      
    if at < 0:
        return ""
    end = c_blocks.masked(seg).find(";", at)
    return seg[at:end + 1] if end >= 0 else seg[at:]


def spawn_call(seg: str, var: str | None) -> str:
                                                                       
    return stmt(seg, spawn_at(seg, var))


def cleanup_problems(src: str) -> list[str]:
                                                                                                      
    out: list[str] = []
    code = code_strings(src)
    try:
        rc = body(code, "request_cleanup")
        poll = body(code, "poll_delete_status")
    except AssertionError as e:
        return [str(e)]

                                                                                          
    stop = argv_var(rc, r'"virsh",\s*"-c",\s*test_uri,\s*"destroy",\s*test_vm_name\s*,\s*NULL')
    undef = argv_var(rc, r'"virsh",\s*"-c",\s*test_uri,\s*"undefine",\s*test_vm_name')
    if spawn_at(rc, stop) < 0:
        out.append("request_cleanup이 켜진 도메인을 virsh destroy로 끄지 않는다")
    elif spawn_at(rc, undef) < 0 or spawn_at(rc, stop) > spawn_at(rc, undef):
        out.append("request_cleanup의 virsh destroy가 undefine보다 앞에 없다")

                                                                                                          
    try:
        dd = body(code, "destroy_test_dataset")
    except AssertionError as e:
        out.append(str(e))
        dd = ""
    settle = argv_var(dd, r'"udevadm",\s*"settle",\s*"--timeout=5"')
    zdestroy = argv_var(dd, r'"zfs",\s*"destroy",\s*"-r",\s*dataset')
    if dd and (spawn_at(dd, settle) < 0 or spawn_at(dd, zdestroy) < 0 or spawn_at(dd, settle) > spawn_at(dd, zdestroy)):
        out.append("destroy_test_dataset이 udevadm settle 뒤 zfs destroy -r을 부르지 않는다")
    if re.search(r'"zfs",\s*"destroy"', rc) or re.search(r'"zfs",\s*"destroy"', poll):
        out.append("zfs destroy를 destroy_test_dataset 밖에서 직접 부른다")
    if "destroy_test_dataset(" not in rc:
        out.append("request_cleanup이 destroy_test_dataset을 부르지 않는다")
    if "destroy_test_dataset(" not in poll or '"dataset is busy"' not in poll:
        out.append("poll_delete_status가 `dataset is busy`일 때 destroy_test_dataset을 다시 부르지 않는다")

                                                         
    call = spawn_call(dd, zdestroy)
    if dd and (not call or "G_SPAWN_STDERR_TO_DEV_NULL" in call or "&last_destroy_stderr" not in call):
        out.append("destroy_test_dataset의 zfs destroy가 stderr를 last_destroy_stderr로 받지 않는다")
    fail = stmt(poll, poll.find('"[FAIL] Delete storage cleanup'))
    if "last_destroy_rc" not in fail or "last_destroy_stderr" not in fail:
        out.append("정리 FAIL 줄에 마지막 zfs destroy의 rc·stderr가 없다")
    return out


def problems(src: str) -> list[str]:
    out: list[str] = []
    masked = c_blocks.masked(src)

                                      
    hits = [m.start() for m in re.finditer(r"pcv_config_get_zvol_pool\s*\(\s*\)", masked)]
    if len(hits) != 1:
        out.append(f"pcv_config_get_zvol_pool()가 {len(hits)}번 있다 (정확히 1번, test_pool 대입 안에만)")
    else:
        stmt_start = max(masked.rfind(";", 0, hits[0]), masked.rfind("{", 0, hits[0]), masked.rfind("}", 0, hits[0]))
        if not re.match(r"\s*test_pool\s*=", masked[stmt_start + 1:hits[0]]):
            out.append("pcv_config_get_zvol_pool()가 test_pool 대입 밖에 있다")

                                        
    for fn in ("request_cleanup", "poll_delete_status"):
        try:
            b = body(src, fn)
        except AssertionError as e:
            out.append(str(e))
            continue
        if not re.search(r'g_strdup_printf\("%s/%s",\s*test_pool,\s*test_vm_name\)', b):
            out.append(f"{fn}의 데이터셋 문자열이 test_pool로 만들어지지 않는다")

                             
    try:
        mb = body(src, "main")
    except AssertionError as e:
        return out + [str(e)]
    call = mb.find("purecvisor_vm_manager_create_vm_async(")
    if call < 0:
        out.append("main에 purecvisor_vm_manager_create_vm_async( 호출이 없다")
        call = len(mb)
    elif "test_pool" not in mb[call:mb.find(");", call)]:
        out.append("Create 인자에 test_pool이 없다")

                                  
    z = mb.find('"zfs", "list"')
    fatal = mb.find("[FATAL] Storage pool dataset")
    if z < 0 or "test_pool" not in mb[z:z + 80]:
        out.append('main에 "zfs", "list", ... test_pool 확인이 없다')
    elif z > call:
        out.append("풀 확인이 Create 호출보다 뒤에 있다")
    if fatal < 0:
        out.append("main에 [FATAL] Storage pool dataset 거절이 없다")
    elif fatal > call:
        out.append("[FATAL] 풀 거절이 Create 호출보다 뒤에 있다")

                
    init = mb.find("virt_conn_pool_init(4)")
    gvir = mb.find("gvir_init_object")
    shut = mb.find("virt_conn_pool_shutdown()")
    ret = mb.rfind("return exit_status")
    if init < 0 or gvir < 0 or init < gvir:
        out.append("virt_conn_pool_init(4)가 gvir_init_object 뒤에 없다")
    if "gvir_connection_open(" in mb:
        out.append("main에 gvir_connection_open(이 되살아났다(VM-MANAGER-GVIR-UNUSED)")
    if shut < 0 or ret < 0 or shut > ret:
        out.append("virt_conn_pool_shutdown()이 return exit_status 앞에 없다")

                     
    if "pcv_vnc_port_from_domain_xml(" not in masked:
        out.append("pcv_vnc_port_from_domain_xml( 호출이 없다")
    if "virDomainIsActive(" not in masked:
        out.append("virDomainIsActive( 호출이 없다")
    if "list_vms" in src:
        out.append("지워질 list_vms를 아직 쓴다")
    return out + cleanup_problems(src)


def swap_in(src: str, fn: str, old: str, new: str) -> str:
    lo, hi = body_span(src, fn)
    seg = src[lo:hi]
    assert old in seg, (fn, old)
    return src[:lo] + seg.replace(old, new) + src[hi:]


class LifecycleContract(unittest.TestCase):
    src = (ROOT / "tests/test_lifecycle.c").read_text()
    makefile = (ROOT / "Makefile").read_text()

    def test_current_tree(self):
        self.assertEqual(problems(self.src), [])

    def test_reverted_cleanup_pool_red(self):
        self.assertTrue(problems(swap_in(self.src, "request_cleanup", "test_pool", "pcv_config_get_zvol_pool()")))

    def test_reverted_poll_pool_red(self):
        self.assertTrue(problems(swap_in(self.src, "poll_delete_status", "test_pool", "pcv_config_get_zvol_pool()")))

    def test_missing_pool_check_red(self):
        self.assertTrue(problems(self.src.replace("[FATAL] Storage pool dataset", "[INFO] Storage pool dataset")))

    def test_pool_checked_before_create(self):
        mb = body(self.src, "main")
        self.assertLess(mb.find('"zfs", "list"'), mb.find("purecvisor_vm_manager_create_vm_async("))

    def test_reverted_stop_before_undefine_red(self):
                                                                                
        no_stop = swap_in(self.src, "request_cleanup", '"destroy", test_vm_name', '"dominfo", test_vm_name')
        self.assertIn("request_cleanup이 켜진 도메인을 virsh destroy로 끄지 않는다", problems(no_stop))
        lo, hi = body_span(self.src, "request_cleanup")
        seg = self.src[lo:hi]
        s_at = seg.index("(void)g_spawn_sync(NULL, (gchar **)stop_domain")
        u_at = seg.index("(void)g_spawn_sync(NULL, (gchar **)undefine")
        stop_call, undef_call = stmt(seg, s_at), stmt(seg, u_at)
        late = self.src[:lo] + seg.replace(stop_call, "@@").replace(undef_call, stop_call).replace("@@", undef_call) + self.src[hi:]
        self.assertIn("request_cleanup의 virsh destroy가 undefine보다 앞에 없다", problems(late))

    def test_reverted_settle_and_busy_retry_red(self):
                                                                                                   
        no_settle = swap_in(self.src, "destroy_test_dataset", '"udevadm", "settle"', '"true", "settle"')
        self.assertIn("destroy_test_dataset이 udevadm settle 뒤 zfs destroy -r을 부르지 않는다", problems(no_settle))
        no_retry = swap_in(self.src, "poll_delete_status", '"dataset is busy"', '"dataset is gone"')
        self.assertIn("poll_delete_status가 `dataset is busy`일 때 destroy_test_dataset을 다시 부르지 않는다", problems(no_retry))
        inline = swap_in(self.src, "request_cleanup", "(void)destroy_test_dataset(dataset);",
                         'const gchar *d[] = { "zfs", "destroy", "-r", dataset, NULL }; (void)d;')
        self.assertIn("zfs destroy를 destroy_test_dataset 밖에서 직접 부른다", problems(inline))

    def test_reverted_stderr_surfacing_red(self):
                                                                                  
        dropped = swap_in(self.src, "destroy_test_dataset", "&last_destroy_stderr, &wait_status", "NULL, &wait_status")
        self.assertIn("destroy_test_dataset의 zfs destroy가 stderr를 last_destroy_stderr로 받지 않는다", problems(dropped))
        lo, hi = body_span(self.src, "poll_delete_status")
        seg = self.src[lo:hi]
        f_at = seg.index('g_printerr("[FAIL] Delete storage cleanup')
        f_stmt = stmt(seg, f_at)
        silent = self.src[:lo] + seg.replace(f_stmt, 'g_printerr("[FAIL] Delete storage cleanup did not remove dataset %s.\\n", dataset);') + self.src[hi:]
        self.assertIn("정리 FAIL 줄에 마지막 zfs destroy의 rc·stderr가 없다", problems(silent))

    def test_cleanup_checks_ignore_comments(self):
                                                         
        lo, hi = body_span(self.src, "poll_delete_status")
        seg = self.src[lo:hi].replace('strstr(last_destroy_stderr, "dataset is busy")',
                                      'strstr(last_destroy_stderr, "x") /* "dataset is busy" */')
        self.assertIn("poll_delete_status가 `dataset is busy`일 때 destroy_test_dataset을 다시 부르지 않는다",
                      problems(self.src[:lo] + seg + self.src[hi:]))

    def test_missing_conn_pool_init_red(self):
        self.assertTrue(problems(self.src.replace("virt_conn_pool_init(4)", "(void)0")))

    def test_make_target_links_without_test_main(self):
        m = self.makefile
        self.assertIn("\ntest-lifecycle-bin:", m)
        self.assertRegex(m, r"LIFECYCLE_TEST_OBJS\s*=[^\n]*filter-out tests/test_main\.o")
        recipe = m[m.index("\ncheck-standalone-builds:"):]
        recipe = recipe[:recipe.index("\n\n")]
        self.assertIn("tests/test_lifecycle_source_contract.py", recipe)


if __name__ == "__main__":
    unittest.main()
