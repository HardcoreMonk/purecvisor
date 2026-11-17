#!/usr/bin/env python3
                          
                                                                                                         
                                                                                   
                                                                 
 
                      
                                                                 
                               
from __future__ import annotations

import importlib.util
import subprocess
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
GATE = REPO / "scripts/check_owner_reader.py"
SPEC = importlib.util.spec_from_file_location("check_owner_reader", GATE)
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)


class OwnerReaderGate(unittest.TestCase):
    def test_real_tree_passes(self):
        r = subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True, cwd=REPO)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_self_test(self):
        r = subprocess.run([sys.executable, str(GATE), "--self-test"], capture_output=True, text=True, cwd=REPO)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_new_reader_outside_allowed_fails(self):
        texts = gate._tree_texts(REPO)
                                                                       
        rel = "src/modules/resource/resource_domain_refs.c"
        bad = dict(texts)
        bad[rel] = texts[rel] + '\nstatic int x(xmlNode *cur) { return node_is(cur, "owner"); }\n'
        self.assertTrue(gate.violations(bad))
                                          
        rel = "src/api/vnc_proxy.h"
        bad = dict(texts)
        bad[rel] = texts[rel] + "\nstatic void y(void *c) { xmlXPathEvalExpression(BAD_CAST \"/domain/metadata/*[local-name()='owner']\", c); }\n"
        self.assertTrue(gate.violations(bad))

    def test_wired_into_check_all_via_check_rbac(self):
                                                                               
        text = (REPO / "Makefile").read_text()
        check_all = next(l for l in text.splitlines() if l.startswith("check-all:")).split(":", 1)[1].split()
        self.assertIn("check-rbac", check_all)
        recipe = text[text.index("\ncheck-rbac:") + 1:]
        recipe = recipe[:recipe.index("\n\n")]
        for line in ("\t@python3 scripts/check_owner_reader.py --self-test", "\t@python3 scripts/check_owner_reader.py\n",
                     "\t@python3 scripts/tests/test_check_owner_reader.py"):
            self.assertIn(line, recipe + "\n")
        self.assertNotIn("check-owner-reader", check_all)                  

    def test_allowed_and_benign_pass(self):
        texts = {
            "src/modules/virt/vm_owner.c": 'static int f(void){ return xmlStrEqual(n->name, BAD_CAST "owner"); }',
            "src/x.c": '/* xmlStrcmp(cur->name, BAD_CAST "owner") */\nstatic void g(void){ json_object_set_string_member(o, "owner", v); }',
        }
        self.assertEqual(gate.violations(texts), [])


if __name__ == "__main__":
    unittest.main()
