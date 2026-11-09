#!/usr/bin/env python3
                                                                                   
                                                                           
                                                                                      
import sys
import unittest
from pathlib import Path

D = Path(__file__).resolve().parents[1] / "src/modules/dispatcher"
sys.path.insert(0, str(D.parents[2] / "scripts"))
import rpc_extract              
MSG = '"VM name must not look like a UUID"'

                                                        
CASES = [
    ("handler_vm_create.c", "void handle_vm_create(PureCVisorDispatcher *self, JsonObject *params", "name",
     ["virt_conn_pool_acquire()", "pcv_spawn_sync_timeout(", "pcv_vm_lock_acquire("]),
    ("handler_vm_clone.c", "void handle_vm_clone(JsonObject *params", "clone_name",
     ["pcv_vpc_vm_is_attached(", "pcv_vm_lock_acquire(", "virt_conn_pool_acquire()"]),
    ("handler_vm_lifecycle.c", "handle_vm_rename_request(JsonObject *params", "new_name",
     ["pcv_vm_lock_acquire(", "virt_conn_pool_acquire()"]),
    ("handler_vm_ova.c", "void handle_vm_import_ova(JsonObject *params", "name",
     ["realpath(", "pcv_vm_lock_acquire(", "virt_conn_pool_acquire()"]),
]


def body(fname: str, anchor: str) -> str:
                                                                                                         
    text = rpc_extract.vm_family_text("vm_lifecycle") if fname == "handler_vm_lifecycle.c" else (D / fname).read_text()
    start = text.index(anchor)
    return text[start:text.index("\n}\n", start)]


class NewVmNameUuidContract(unittest.TestCase):
    def test_each_entry_rejects_uuid_names_before_effects(self):
        for fname, anchor, var, later in CASES:
            with self.subTest(fname=fname):
                b = body(fname, anchor)
                call = f"pcv_validate_new_vm_name({var})"
                at = b.find(call)
                self.assertGreater(at, 0, f"{fname}: {call} 없음")
                self.assertIn(MSG, b[at:at + 600], f"{fname}: 검사 뒤 거부 메시지 없음")
                self.assertIn("PURE_RPC_ERR_INVALID_PARAMS", b[at:at + 600], f"{fname}: -32602 아님")
                present = [a for a in later if a in b]
                self.assertTrue(present, f"{fname}: 비교할 효과 앵커가 본문에 없음")
                for a in present:
                    self.assertLess(at, b.find(a), f"{fname}: 검사가 {a}보다 뒤에 있다")

    def test_lookup_names_keep_lookup_validator(self):
                                                             
        self.assertIn("pcv_validate_vm_name(source)", body(*CASES[1][:2]))
        self.assertIn("pcv_validate_vm_name(old_name)", body(*CASES[2][:2]))
        for fname, anchor, _, _ in CASES:
            self.assertNotIn("pcv_validate_new_vm_name(source)", body(fname, anchor))
            self.assertNotIn("pcv_validate_new_vm_name(old_name)", body(fname, anchor))


if __name__ == "__main__":
    unittest.main()
