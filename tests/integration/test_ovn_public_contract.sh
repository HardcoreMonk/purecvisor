#!/usr/bin/env bash







set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

python3 - "$root_dir" <<'PY'
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])


def read(path: str) -> str:
    return (root / path).read_text(encoding="utf-8")


def between(text: str, start: str, end: str) -> str:
    begin = text.index(start)
    finish = text.index(end, begin + len(start))
    return text[begin:finish]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


handler = read("src/modules/dispatcher/handler_overlay.c")
owner_guard = between(handler, "_reject_local_vpc_owned_ovn", "void handle_ovn_switch_create")
create_handler = between(handler, "void handle_ovn_switch_create", "void handle_ovn_switch_delete")
delete_handler = between(handler, "void handle_ovn_switch_delete", "void handle_ovn_switch_list")
acl_list_handler = between(handler, "void handle_ovn_acl_list", "void handle_ovn_router_create")

require(re.search(r"if\s*\(\s*!name\s*\)", delete_handler) is not None,
        "ovn.switch.delete must reject a missing name")
require(re.search(r"if\s*\(\s*!pcv_ovn_switch_delete\s*\(\s*name\s*,\s*&\w+\s*\)\s*\)",
                  delete_handler) is not None,
        "ovn.switch.delete must branch on the manager result")
require(delete_handler.index("pure_rpc_build_error_response") <
        delete_handler.index('json_object_set_string_member(res,"status","deleted")'),
        "ovn.switch.delete must send an error before any success envelope")
require('json_object_has_member(p,"subnet")' in create_handler,
        "ovn.switch.create must detect the retired subnet member")
require("PURE_RPC_ERR_INVALID_PARAMS" in create_handler and
        create_handler.index('json_object_has_member(p,"subnet")') <
        create_handler.index("pcv_ovn_switch_create"),
        "ovn.switch.create must reject subnet before manager mutation")
require(re.search(r"if\s*\(\s*!sw\s*\)", acl_list_handler) is not None and
        "PURE_RPC_ERR_INVALID_PARAMS" in acl_list_handler,
        "ovn.acl.list must enforce its required switch filter")
require("g_error_matches(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT)" in owner_guard and
        "PURE_RPC_ERR_INVALID_PARAMS" in owner_guard,
        "invalid OVN IDs must remain Invalid params through the ownership guard")

manager_c = read("src/modules/network/ovn_manager.c")
manager_h = read("src/modules/network/ovn_manager.h")
create_manager = between(manager_c, "gboolean\npcv_ovn_switch_create(", "gboolean\npcv_ovn_switch_delete(")
require(re.search(r"pcv_ovn_switch_create\s*\(\s*const gchar \*name\s*,\s*GError \*\*error\s*\)",
                  create_manager) is not None,
        "manager switch-create signature must be name + error only")
require("subnet" not in create_manager,
        "manager switch-create must not retain a logging-only subnet argument")
require("pcv_ovn_switch_create(const gchar *name, GError **error)" in manager_h,
        "public manager header must expose the name-only switch-create contract")
require("pcv_ovn_switch_create(const gchar *name, const gchar *subnet" not in manager_h,
        "legacy manager subnet signature remains")

cli = read("src/cli/purecvisorctl.c")
cli_switch = between(cli, "void cmd_ovn_switch", "void cmd_ovn_router")
require("pcvctl ovn switch create <name>" in cli_switch,
        "CLI must advertise the name-only switch-create command")
require("--subnet" not in cli_switch,
        "CLI must not advertise or parse the no-op switch --subnet option")
require(re.search(r"action,\s*\"create\"\).*?argc\s*!=\s*4", cli_switch, re.S) is not None,
        "CLI switch create must reject trailing unsupported arguments")

help_js = read("ui/modules/help.js")
require("'ACL 규칙 목록 (switch 필수)'" in help_js and
        "'List ACL rules (switch required)'" in help_js,
        "help catalog must describe switch as the required ACL list filter")
require("{ m: 'GET', p: '/ovn/acl?switch=ls0'" in help_js,
        "API explorer ACL GET must show its query string")
require("{ m: 'GET', p: '/ovn/nat?router=lr0'" in help_js,
        "API explorer NAT GET must show its query string")
require("{ m: 'GET', p: '/ovn/acl'," not in help_js and
        "{ m: 'GET', p: '/ovn/nat'," not in help_js,
        "API explorer must not retain body-oriented ACL/NAT GET entries")

required_cli = (
    "pcvctl ovn switch create ls-web",
    "pcvctl ovn router create lr-main",
    "pcvctl ovn acl add ls-web to-lport 100 'tcp.dst == 80' allow",
    "pcvctl ovn acl list ls-web",
    "pcvctl ovn nat list lr-main",
    "pcvctl ovn dhcp enable 10.0.1.0/24 10.0.1.1 --switch ls-web",
)
retired_cli_patterns = (
    r"pcvctl ovn switch create[^\n]*--subnet",
    r"pcvctl ovn switch create --name",
    r"pcvctl ovn switch detail",
    r"pcvctl ovn router create --name",
    r"pcvctl ovn router add-port",
    r"pcvctl ovn acl (?:add[^\n]*--direction|remove)",
    r"pcvctl ovn nat (?:add|remove)",
    r"pcvctl ovn dhcp (?:set|list)",
)
for guide_path in ("docs/GUIDE.md", "ui/guide-content.md"):
    guide = read(guide_path)
    ovn_section = between(guide, "### 6.6 OVN SDN", "### 6.7 보안 그룹")
    for command in required_cli:
        require(command in ovn_section, f"{guide_path}: missing real CLI example: {command}")
    for pattern in retired_cli_patterns:
        require(re.search(pattern, ovn_section) is None,
                f"{guide_path}: retired or unsupported CLI example remains: {pattern}")

print("OVN public contract: PASS")
PY
