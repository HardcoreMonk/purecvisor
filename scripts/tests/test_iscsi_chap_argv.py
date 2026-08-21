#!/usr/bin/env python3
                          
                                                            
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                          
                                      
from __future__ import annotations

import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SCRIPT_DIR))

from check_iscsi_chap_argv import scan_text              

GOOD = r'''
gboolean pcv_iscsi_initiator_connect(void) {
    gchar *chap_password = get_secret();
    const gchar *discovery[] = {"iscsiadm", "-m", "discoverydb", "--discover", NULL};
    if (!pcv_iscsi_chap_validate(chap_user, chap_password, error)) return FALSE;
    if (!pcv_iscsi_node_db_set_chap(iqn, portal, chap_user, chap_password, error))
        return FALSE;
    const gchar *login[] = {"iscsiadm", "-m", "node", "--login", NULL};
}
'''


def test_approved_shape_passes() -> None:
    assert scan_text(GOOD) == []


def test_password_argv_turns_red() -> None:
    bad = GOOD + r'''
const gchar *a_pass[] = {"iscsiadm", "-v", chap_password, NULL};
'''
    assert any("argv initializer" in item for item in scan_text(bad))


def test_iscsiadm_update_turns_red() -> None:
    bad = GOOD + r'''
const gchar *a_method[] = {"iscsiadm", "--op=update", "-v", "CHAP", NULL};
'''
    assert any("node update" in item for item in scan_text(bad))


def test_unguarded_node_db_call_turns_red() -> None:
    bad = GOOD.replace("if (!pcv_iscsi_node_db_set_chap", "if (pcv_iscsi_node_db_set_chap")
    assert any("node_db_set_chap" in item for item in scan_text(bad))


def test_missing_validation_turns_red() -> None:
    bad = GOOD.replace(
        "if (!pcv_iscsi_chap_validate(chap_user, chap_password, error)) return FALSE;",
        "pcv_iscsi_chap_validate(chap_user, chap_password, error);",
    )
    assert any("chap_validate" in item for item in scan_text(bad))


def test_legacy_nonpersistent_discovery_turns_red() -> None:
    bad = GOOD.replace('"discoverydb"', '"discovery"')
    assert any("discoverydb" in item for item in scan_text(bad))


def test_missing_discover_action_turns_red() -> None:
    bad = GOOD.replace(', "--discover"', '')
    assert any("discoverydb" in item for item in scan_text(bad))


def main() -> int:
    tests = [value for name, value in sorted(globals().items())
             if name.startswith("test_") and callable(value)]
    failed = 0
    for test in tests:
        try:
            test()
            print(f"OK   {test.__name__}")
        except AssertionError as error:
            failed += 1
            print(f"FAIL {test.__name__}: {error}")
    print(f"[test_iscsi_chap_argv] {len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
