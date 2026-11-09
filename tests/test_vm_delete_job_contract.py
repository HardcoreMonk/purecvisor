#!/usr/bin/env python3
                                                                                       
                                                                                      
                                                                               
                                                                        
import re
import unittest
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import rpc_extract              
ADMIN = ROOT / "src/modules/dispatcher/handler_vm_admin.c"


def strip_comments(text: str) -> str:
                                                      
    return re.sub(r"/\*.*?\*/|//[^\n]*", lambda m: re.sub(r"[^\n]", " ", m[0]), text, flags=re.S)


def body(text: str, signature: str) -> str:
    start = text.index(signature)
    open_brace = text.index("{", start)
    depth = 0
    for i in range(open_brace, len(text)):
        depth += (text[i] == "{") - (text[i] == "}")
        if depth == 0:
            return text[open_brace:i + 1]
    raise AssertionError(f"unterminated: {signature}")


class VmDeleteJobContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        src = strip_comments(rpc_extract.vm_family_text("vm_lifecycle"))
        cls.handler = body(src, "void handle_vm_delete_request(JsonObject *params")
        cls.callback = body(src, "_vm_delete_callback(GObject *src")
        cls.admin = body(strip_comments(ADMIN.read_text()), "void handle_vm_delete_status(JsonObject *params")

    def test_admission_after_lock_before_accepted_response(self):
        h = self.handler
        lock = h.index("pcv_vm_lock_acquire(vm_id, VM_OP_DELETING")
        admit = h.index('pcv_job_admit("vm.delete", pcv_vm_lock_key(op_lock), NULL, "vm.delete")')
        accepted = h.index('json_object_set_boolean_member(acc, "accepted", TRUE)')
        worker = h.index("g_task_run_in_thread(task, _vm_delete_worker)")
        self.assertLess(lock, admit)
        self.assertLess(admit, accepted)
        self.assertLess(accepted, worker)

    def test_accepted_response_is_object_with_four_fields(self):
        h = self.handler
        for key in ('"accepted", TRUE', '"name", pcv_vm_lock_key(op_lock)', '"job_id", pcv_job_id(job_ref)', '"status", "accepted"'):
            self.assertIn(key, h)
        self.assertNotIn('json_node_set_string(acc_node, "accepted")', h)                   
        self.assertIn("ctx->job_ref    = job_ref", h)
        self.assertIn("ctx->target     = g_strdup(pcv_vm_lock_key(op_lock))", h)

    def test_callback_finishes_job_on_both_branches(self):
        c = self.callback
        self.assertEqual(c.count("pcv_ws_finish_job(ctx->job_ref, PCV_JOB_COMPLETED"), 1)
        self.assertEqual(c.count("pcv_ws_finish_job(ctx->job_ref, PCV_JOB_FAILED"), 1)
        self.assertEqual(c.count('pcv_audit_log('), 2)                     
        self.assertNotIn("pure_uds_server_send_response", c)               
        release = c.index("pcv_vm_lock_release")
        self.assertLess(release, c.index("pcv_audit_log("))
        self.assertLess(c.index("pcv_ws_finish_job("), c.index("cmap_remove("))

    def test_status_reads_job_ledger(self):
        a = self.admin
        self.assertIn('pcv_job_find_latest("vm.delete", vm, &job)', a)
        self.assertIn("pcv_vm_delete_status_build(vm, job)", a)
        self.assertIn("PURE_RPC_ERR_INVALID_PARAMS", a)
        self.assertIn('"Job ledger unavailable"', a)
        self.assertNotIn("pcv_vm_delete_status_get", a)


if __name__ == "__main__":
    unittest.main()
