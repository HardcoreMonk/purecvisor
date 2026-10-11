#!/usr/bin/env python3


















from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import check_test_hermetic as gate

ROOT = Path(__file__).resolve().parents[2]
GATE = ROOT / "scripts" / "check_test_hermetic.py"
BASE_COMMIT = "ed147de"


def _violations(source: str, header: str = "") -> list[gate.Violation]:

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        src_dir = tmp_path / "src"
        src_dir.mkdir()
        if header:
            (src_dir / "fixture.h").write_text(header, encoding="utf-8")
        c_file = tmp_path / "test_fixture.c"
        c_file.write_text(source, encoding="utf-8")
        macros = gate.production_macros(src_dir)
        return gate.analyze_file(c_file, "test_fixture.c", macros)


def _wrap(body: str, extra: str = "") -> str:

    return (
        "#include <glib.h>\n"
        f"{extra}\n"
        "static void test_leaf(void) {\n"
        f"{body}\n"
        "}\n"
        "void test_fixture_register(void) {\n"
        '    g_test_add_func("/fixture/leaf", test_leaf);\n'
        "}\n"
    )


def test_gate_passes_on_current_tree():

    r = subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr


def test_unguarded_production_literal_is_red():

    v = _violations(_wrap('    FILE *fp = fopen("/var/log/purecvisor/audit.log", "a");'))
    assert [(x.func, x.kind, x.token) for x in v] == [
        ("test_leaf", "production-path", "/var/log/purecvisor")]


def test_opt_in_guard_in_leaf_is_green():

    body = ('    if (!pcv_test_live_opt_in("PCV_AUDIT_LIVE")) { g_test_skip("x"); return; }\n'
            '    FILE *fp = fopen("/var/log/purecvisor/audit.log", "a");')
    assert _violations(_wrap(body)) == []


def test_non_live_env_name_is_not_a_guard():

    body = ('    if (g_strcmp0(g_getenv("PCV_FOO"), "1") != 0) return;\n'
            '    g_file_set_contents("/var/lib/purecvisor/x.db", "", 0, NULL);')
    assert len(_violations(_wrap(body))) == 1


def test_guard_via_helper_and_helper_paths():

    helpers = (
        "static const char *gate_reason(void) {\n"
        '    if (!pcv_test_live_opt_in("PCV_TRACE_LIVE")) return "opt-in";\n'
        "    return NULL;\n}\n"
        "static void rm_root(void) {\n"
        '    g_remove("/var/run/purecvisor/trace/x");\n}\n'
    )
    guarded = _wrap("    if (gate_reason()) return;\n    rm_root();", helpers)
    assert _violations(guarded) == []
    unguarded = guarded.replace(
        "void test_fixture_register(void) {",
        "static void test_other(void) { rm_root(); }\nvoid test_fixture_register(void) {\n"
        '    g_test_add_func("/fixture/other", test_other);')
    found = _violations(unguarded)
    assert [(x.func, x.token) for x in found] == [("rm_root", "/var/run/purecvisor")], found


def test_comments_are_ignored():

    body = ("    /* 운영 /var/lib/purecvisor/security_groups.db 를 쓰지 않는다 */\n"
            "    // systemctl kill suricata 는 opt-in 전용\n"
            "    int x = 0; (void)x;")
    assert _violations(_wrap(body)) == []


def test_host_commands():

    argv_kill = '    const char *argv[] = { "systemctl", "kill", "suricata", NULL };'
    assert [x.token for x in _violations(_wrap(argv_kill))] == ["systemctl kill"]
    shell = '    (void)system("virsh undefine pcv-vm >/dev/null 2>&1");'
    assert [x.token for x in _violations(_wrap(shell))] == ["virsh undefine"]
    query = '    const char *argv[] = { "systemctl", "is-active", "suricata", NULL };'
    assert _violations(_wrap(query)) == []
    compare = '    if (g_strcmp0(argv0, "ovs-vsctl") == 0 && has(argv, "add-br")) return;'
    assert _violations(_wrap(compare)) == [], "fake exec의 argv 비교는 실행이 아니다"


def test_production_macros_from_headers_and_local_defines():

    header = ('#define PCV_FIX_ROOT "/var/run/purecvisor/fix"\n'
              '#define PCV_FIX_PID PCV_FIX_ROOT "/x.pid"\n')
    body = "    g_unlink(PCV_FIX_PID);"
    assert [x.token for x in _violations(_wrap(body), header)] == ["PCV_FIX_PID"]
    local = '#define T_STATE "/var/lib/purecvisor/bpf_state.json"\n'
    found = _violations(_wrap("    g_unlink(T_STATE);", local))
    assert [(x.kind, x.token) for x in found] == [("production-macro", "T_STATE")]


def test_sensitive_and_seam_calls():

    assert [x.token for x in _violations(_wrap("    pcv_trace_purge_all();"))] == [
        "pcv_trace_purge_all"]
    sla = '    pcv_qos_tenant_sla_set("beta", 1, 2, NULL, NULL, NULL);'
    assert [x.kind for x in _violations(_wrap(sla))] == ["seam-required"]
    seamed = '    pcv_qos_set_tenant_sla_path_for_test(path);\n' + sla
    assert _violations(_wrap(seamed)) == []
    overlay = "    JsonArray *a = pcv_overlay_list(NULL); (void)a;"
    assert [x.token for x in _violations(_wrap(overlay))] == ["pcv_overlay_list"]


def test_pre_fix_test_files_are_red():

    expected = {
        "test_security_group.c": ("production-path", "/var/lib/purecvisor"),
        "test_suricata_integration.c": ("host-command", "systemctl kill"),
        "test_bpf_integration.c": ("sensitive-call", "pcv_bpf_rehydrate"),
        "test_ovn.c": ("seam-required", "pcv_overlay_list"),
        "test_trace_integration.c": ("sensitive-call", "pcv_trace_purge_all"),
        "test_qos_integration.c": ("seam-required", "pcv_qos_tenant_sla_set"),
        "test_tenant_overlay.c": ("sensitive-call", "network_dhcp_reserve_overlay_ip"),
    }
    with tempfile.TemporaryDirectory() as tmp:
        macros = gate.production_macros(ROOT / "src")
        for name, (kind, token) in expected.items():
            r = subprocess.run(["git", "-C", str(ROOT), "show", f"{BASE_COMMIT}:tests/{name}"],
                               capture_output=True, text=True)
            if r.returncode != 0:
                print(f"SKIP {name}: 기준 커밋 {BASE_COMMIT} 없음")
                continue
            path = Path(tmp) / name
            path.write_text(r.stdout, encoding="utf-8")
            found = {(v.kind, v.token) for v in gate.analyze_file(path, name, macros)}
            assert (kind, token) in found, (name, found)
            current = gate.analyze_file(ROOT / "tests" / name, f"tests/{name}", macros)
            unallowed = [v for v in current if v.key() not in gate.ALLOWLIST]
            assert unallowed == [], (name, unallowed)


def test_link_seam_contract():

    makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
    sources = {src: (ROOT / src).read_text(encoding="utf-8") for _, _, src, _ in gate.LINK_SEAMS}
    assert gate.link_seam_problems(makefile, sources) == []

    no_filter = makefile.replace("$(filter-out src/modules/ai/self_healing.o $(TEST_HOST_PATH_SEAM_REPLACED),",
                                 "$(filter-out src/modules/ai/self_healing.o,")
    assert no_filter != makefile
    assert gate.link_seam_problems(no_filter, sources)

    no_seam_objs = makefile.replace("TEST_OBJS  += $(TEST_HOST_PATH_SEAM_OBJS)\n", "")
    assert no_seam_objs != makefile
    assert gate.link_seam_problems(no_seam_objs, sources)

    no_flag = makefile.replace("-DPCV_TEST_HOST_PATH_SEAM_MAP ", "")
    assert no_flag != makefile
    assert gate.link_seam_problems(no_flag, sources)

    for _, _, src, override in gate.LINK_SEAMS:
        mutated = dict(sources)
        mutated[src] = sources[src].replace(f"#ifdef {override}", "#if 0")
        assert mutated[src] != sources[src]
        assert any(src in p for p in gate.link_seam_problems(makefile, mutated))


def test_stale_and_weak_allowlist_are_red():

    with tempfile.TemporaryDirectory() as tmp:
        c_file = Path(tmp) / "test_fixture.c"
        c_file.write_text(_wrap("    int x = 0; (void)x;"), encoding="utf-8")
        rel = gate._relative(c_file)
        stale = {(rel, "test_leaf", "/var/lib/purecvisor"): "더 이상 쓰지 않는 fixture 문자열 사유"}
        assert gate.run([c_file], ROOT / "src", stale) == 1
        assert gate.run([c_file], ROOT / "src", {}) == 0
        c_file.write_text(_wrap('    const char *p = "/etc/purecvisor/x";'), encoding="utf-8")
        weak = {(rel, "test_leaf", "/etc/purecvisor"): "짧음"}
        assert gate.run([c_file], ROOT / "src", weak) == 1
        good = {(rel, "test_leaf", "/etc/purecvisor"): "경로 검증 함수 입력 문자열 — 파일 접근 없음"}
        assert gate.run([c_file], ROOT / "src", good) == 0


if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items())
             if k.startswith("test_") and callable(v)]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"OK   {t.__name__}")
        except AssertionError as e:
            failed += 1
            print(f"FAIL {t.__name__}: {e}")
    print(f"[test_test_hermetic] {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
