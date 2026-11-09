                                                                                  
                                                                                   
                                                                                 
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import rpc_extract              


def rename_body() -> str:
    text = rpc_extract.vm_family_text("vm_lifecycle")
    start = text.index("handle_vm_rename_request(JsonObject *params")
    end = text.index("\n}\n", start)
    return text[start:end]


class VmRenameLockContract(unittest.TestCase):
    def test_locks_both_names_before_state_change(self):
        body = rename_body()
        old_at = body.find("g_autoptr(PcvVmLock) old_lock = pcv_vm_lock_acquire(old_name, VM_OP_TUNING")
        new_at = body.find("pcv_vm_lock_acquire(new_name, VM_OP_CREATING")
        conn_at = body.find("virt_conn_pool_acquire()")
        self.assertGreater(old_at, 0, "옛 이름 TUNING 잠금이 g_autoptr로 없다")
        self.assertGreater(new_at, old_at, "새 이름 CREATING 잠금이 옛 이름 잠금 뒤에 없다")
        self.assertLess(new_at, conn_at, "잠금이 libvirt 연결·상태 변경보다 앞서야 한다")
        self.assertIn("g_autoptr(PcvVmLock) new_lock", body)

    def test_busy_rejection(self):
        body = rename_body()
        self.assertRegex(body, r"if \(!old_lock \|\| !new_lock\) \{\s*pcv_disp_send_rpc_error\(server, connection, rpc_id, PURE_RPC_ERR_BUSY")

    def test_no_manual_release(self):
        self.assertIsNone(re.search(r"pcv_vm_lock_release\((old|new)_lock\)", rename_body()))                         


if __name__ == "__main__":
    unittest.main()
