#!/usr/bin/env python3
                          
                                                                      
                                                                                      
                                                                                                                            
 
                      
                                                                                                                               
                                                            

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


FRONTEND_CONST = re.compile(
    r"const\s+NETWORK_MODE_SET_MODES\s*=\s*Object\.freeze\(\[([^\]]+)\]\)"
)
STRING_LITERAL = re.compile(r"['\"]([^'\"]+)['\"]")
BACKEND_COMPARE = re.compile(
    r'g_strcmp0\(\s*mode\s*,\s*"([^"]+)"\s*\)\s*!=\s*0'
)


def function_body(source: str, name: str) -> str:
                                                
    match = re.search(r"\b" + re.escape(name) + r"\s*\([^)]*\)\s*\{", source, re.S)
    if not match:
        return ""
    start = source.find("{", match.start())
    depth = 0
    for index in range(start, len(source)):
        char = source[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    return ""


def frontend_modes(source: str) -> set[str]:
    match = FRONTEND_CONST.search(source)
    return set(STRING_LITERAL.findall(match.group(1))) if match else set()


def backend_modes(source: str) -> set[str]:
    body = function_body(source, "handle_network_mode_set_request")
    return set(BACKEND_COMPARE.findall(body))


def analyze(frontend: str, backend: str) -> list[str]:
    errors: list[str] = []
    fe_modes = frontend_modes(frontend)
    be_modes = backend_modes(backend)
    if not fe_modes:
        errors.append("프론트 NETWORK_MODE_SET_MODES 추출 실패")
    if not be_modes:
        errors.append("백엔드 network.mode_set whitelist 추출 실패")
    if fe_modes != be_modes:
        errors.append(
            "mode enum 불일치: "
            f"frontend-only={sorted(fe_modes - be_modes)}, "
            f"backend-only={sorted(be_modes - fe_modes)}"
        )
    if "NETWORK_MODE_SET_MODES.map" not in frontend:
        errors.append("편집 option이 NETWORK_MODE_SET_MODES에서 파생되지 않음")
    if "NETWORK_MODE_SET_MODES.indexOf(mode) === -1" not in frontend:
        errors.append("doNetEdit의 허용 mode 전송 가드 누락")
    body = function_body(backend, "handle_network_mode_set_request")
    if "network_dhcp_stop(br, &error)" not in body:
        errors.append("routed 전환의 동기 DHCP stop 계약 누락")
    if "network_dhcp_start(br, cidr, &dhcp_err)" not in body:
        errors.append("nat/isolated 전환의 DHCP 재수렴 계약 누락")
    if "g_file_test(pid_chk, G_FILE_TEST_EXISTS)" in body:
        errors.append("PID 파일 존재를 DHCP 생존으로 오인하는 구 구현 잔존")
    return errors


def analyze_bootstrap(bootstrap: str) -> list[str]:
                                             
    errors: list[str] = []
    body = function_body(bootstrap, "pcv_bootstrap_init_runtime_network")
    if not body:
        return ["Single Edge runtime network bootstrap 추출 실패"]
    if "network_dhcp_start_ex(def_br, def_cidr, TRUE, NULL, &net_err)" not in body:
        errors.append("기본 NAT DHCP+DNS 재수렴 호출 누락")
    if "dhcp_up" in body:
        errors.append("기본 NAT DHCP를 순간 생존값으로 건너뛰는 TOCTOU 게이트 잔존")
    return errors


OWNED_RULE_ARGV = re.compile(
    r'"add"\s*,\s*"rule"\s*,\s*"inet"\s*,\s*"purecvisor"\s*,\s*"(forward|postrouting|guest_input)"')
OWNED_RULE_TEXT = re.compile(r"add rule inet purecvisor (forward|postrouting|guest_input)\b")

FOREIGN_TOKENS = re.compile(
    r'(GFW_T\s*"\s*(input|ips)\b|inet purecvisor (input|ips)\b|pcv_blocked|delete table|flush table|flush ruleset)')

def strip_c_comments(text: str) -> str:

    return re.sub(r"//[^\n]*", " ", re.sub(r"/\*.*?\*/", " ", text, flags=re.S))

def analyze_guest_fw(network_sources: dict[str, str], backend: str, bootstrap: str) -> list[str]:

    errors: list[str] = []
    builder = network_sources.get("network_firewall_nft.c")
    if builder is None:
        errors.append("legacy 게스트 방화벽 빌더(network_firewall_nft.c) 없음")
    elif FOREIGN_TOKENS.search(strip_c_comments(builder)):
        errors.append("빌더가 다른 소유자 객체(input·ips·pcv_blocked·테이블)를 비우거나 지움")
    for name, text in sorted(network_sources.items()):
        if name == "network_firewall_nft.c":
            continue
        code = strip_c_comments(text)
        if OWNED_RULE_ARGV.search(code) or OWNED_RULE_TEXT.search(code):
            errors.append(f"{name}: legacy forward/postrouting/guest_input 규칙을 빌더 밖에서 nft add rule로 만듦")
    body = function_body(backend, "handle_network_mode_set_request")
    if "network_firewall_teardown(" in body:
        errors.append("mode_set이 teardown 뒤 setup으로 전환(규칙 없는 틈)")
    boot = function_body(bootstrap, "pcv_bootstrap_init_runtime_network")
    if "network_firewall_reconcile(" not in boot:
        errors.append("기동 legacy 방화벽 reconcile 호출 누락")
    if "nat-%s.ok" in boot:
        errors.append("NAT 마커 가드 잔존(재시작 뒤 새 규칙 미적용)")
    return errors

def self_test() -> list[str]:
    good_fe = """
const NETWORK_MODE_SET_MODES = Object.freeze(['nat', 'isolated', 'routed']);
NETWORK_MODE_SET_MODES.map(function (mode) { return mode; });
if (NETWORK_MODE_SET_MODES.indexOf(mode) === -1) return;
"""
    good_be = """
void handle_network_mode_set_request(JsonObject *params) {
  if (g_strcmp0(mode, "nat") != 0 &&
      g_strcmp0(mode, "isolated") != 0 &&
      g_strcmp0(mode, "routed") != 0) return;
  if (g_strcmp0(mode, "routed") == 0 && !network_dhcp_stop(br, &error)) return;
  if (g_strcmp0(mode, "nat") == 0 || g_strcmp0(mode, "isolated") == 0) {
    network_dhcp_start(br, cidr, &dhcp_err);
  }
}
"""
    good_bootstrap = """
void pcv_bootstrap_init_runtime_network(void) {
  network_firewall_reconcile(&gfw_err);
  if (network_dhcp_start_ex(def_br, def_cidr, TRUE, NULL, &net_err)) return;
}
"""
    failures: list[str] = []
    if analyze(good_fe, good_be):
        failures.append("일치 fixture를 거부함")
    if analyze_bootstrap(good_bootstrap):
        failures.append("DHCP 재수렴 bootstrap fixture를 거부함")
    if not analyze(good_fe.replace("'routed'", "'routed', 'bridge'"), good_be):
        failures.append("프론트 초과 enum을 허용함")
    if not analyze(good_fe, good_be.replace(
            'g_strcmp0(mode, "routed") != 0',
            'g_strcmp0(mode, "routed") != 0 && g_strcmp0(mode, "bridge") != 0')):
        failures.append("백엔드 초과 enum을 허용함")
    if not analyze(good_fe.replace("NETWORK_MODE_SET_MODES.map", "[].map"), good_be):
        failures.append("option 파생 단절을 허용함")
    if not analyze(good_fe.replace("NETWORK_MODE_SET_MODES.indexOf(mode)", "[].indexOf(mode)"), good_be):
        failures.append("전송 가드 단절을 허용함")
    if not analyze(good_fe, good_be.replace("network_dhcp_stop", "legacy_dhcp_stop")):
        failures.append("routed DHCP stop 단절을 허용함")
    if not analyze(good_fe, good_be.replace("network_dhcp_start", "legacy_dhcp_start")):
        failures.append("nat/isolated DHCP 재수렴 단절을 허용함")
    if not analyze(good_fe, good_be.replace(
            "network_dhcp_start(br, cidr, &dhcp_err);",
            "if (!g_file_test(pid_chk, G_FILE_TEST_EXISTS)) "
            "network_dhcp_start(br, cidr, &dhcp_err);")):
        failures.append("PID 파일 생존 오판 회귀를 허용함")
    if not analyze_bootstrap(good_bootstrap.replace(
            "if (network_dhcp_start_ex(def_br, def_cidr, TRUE, NULL, &net_err)) return;",
            "if (!dhcp_up) { network_dhcp_start_ex(def_br, def_cidr, TRUE, NULL, &net_err); }")):
        failures.append("bootstrap 순간 생존 TOCTOU 회귀를 허용함")
    good_net = {
        "network_firewall_nft.c": 'g_string_append(s, "flush chain " GFW_T " guest_input\\n");',
        "network_firewall.c": 'const gchar *argv[] = {"nft", "-f", "-", NULL};',
    }
    if analyze_guest_fw(good_net, good_be, good_bootstrap):
        failures.append("D2 정상 fixture를 거부함")
    bad = dict(good_net, **{"network_firewall.c":
        'const gchar *a[] = {"nft","add","rule","inet","purecvisor","forward","iifname",iif,"accept",NULL};'})
    if not analyze_guest_fw(bad, good_be, good_bootstrap):
        failures.append("빌더 밖 forward 규칙 생성을 허용함")
    if not analyze_guest_fw(good_net, good_be.replace(
            "  if (g_strcmp0(mode, \"nat\") == 0 ||",
            "  network_firewall_teardown(br, NULL);\n  if (g_strcmp0(mode, \"nat\") == 0 ||"), good_bootstrap):
        failures.append("mode_set teardown 틈 회귀를 허용함")
    if not analyze_guest_fw(good_net, good_be, good_bootstrap.replace("network_firewall_reconcile(&gfw_err);", "")):
        failures.append("기동 reconcile 누락을 허용함")
    if not analyze_guest_fw(good_net, good_be, good_bootstrap.replace(
            "  network_firewall_reconcile(&gfw_err);\n",
            "  network_firewall_reconcile(&gfw_err);\n"
            "  g_autofree gchar *mk = g_strdup_printf(PCV_NETWORK_RUNDIR \"/nat-%s.ok\", def_br);\n")):
        failures.append("NAT 마커 가드 잔존을 허용함")
    if not analyze_guest_fw(dict(good_net, **{"network_firewall_nft.c": '"flush chain " GFW_T " input\\n"'}),
                            good_be, good_bootstrap):
        failures.append("빌더의 HIPS input 체인 flush를 허용함")
    if analyze_guest_fw(dict(good_net, **{"network_firewall_nft.c": '/* HIPS의 input·pcv_blocked는 건드리지 않는다 */'}),
                        good_be, good_bootstrap):
        failures.append("빌더 주석 속 설명을 규칙으로 오인함")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        errors = self_test()
        if errors:
            for error in errors:
                print(f"FAIL {error}")
            return 1
        print("[check-network-mode-contract] self-test PASS: enum/DHCP/D2 반사실 15종")
        return 0

    root = args.root.resolve()
    frontend_path = root / "ui/modules/network.js"
    backend_path = root / "src/modules/network/network_manager.c"
    bootstrap_path = root / "src/bootstrap/pcv_bootstrap_single.c"
    if (not frontend_path.is_file() or not backend_path.is_file()
            or not bootstrap_path.is_file()):
        print("FAIL 계약 입력 파일 누락")
        return 1
    errors = analyze(
        frontend_path.read_text(encoding="utf-8"),
        backend_path.read_text(encoding="utf-8"),
    )
    bootstrap_text = bootstrap_path.read_text(encoding="utf-8")
    errors.extend(analyze_bootstrap(bootstrap_text))
    network_dir = root / "src/modules/network"
    network_sources = {p.name: p.read_text(encoding="utf-8") for p in network_dir.glob("*.c")}
    errors.extend(analyze_guest_fw(network_sources, backend_path.read_text(encoding="utf-8"), bootstrap_text))
    if errors:
        for error in errors:
            print(f"FAIL {error}")
        print(f"[check-network-mode-contract] FAIL: {len(errors)}건")
        return 1
    modes = sorted(frontend_modes(frontend_path.read_text(encoding="utf-8")))
    print(f"[check-network-mode-contract] PASS: FE=BE {','.join(modes)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
