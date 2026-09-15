#!/usr/bin/env python3

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
GATE = "tests/integration/test_single_ui_surface.sh"
MONITOR = "ui/modules/monitor.js"
MANIFEST = "packaging/ui-assets.manifest"
BUNDLE = "ui/app.bundle.js"
MAP = "ui/app.bundle.js.map"
REQUIRED_FILES = (
    GATE, MANIFEST, BUNDLE, "scripts/deploy.sh", "src/api/dispatcher.c",
    "src/api/rest_server.c", "ui/index.html", "ui/app.js", "ui/i18n.js",
    "ui/modules/endpoints.js", "ui/modules/uxlib.js", "ui/modules/nav.js",
    "ui/modules/help.js", "ui/modules/advanced.js", "ui/modules/vm.js",
    "ui/modules/accounts.js", MONITOR, "ui/modules/shell.js",
    "ui/modules/storage.js", "ui/modules/network.js",
)
OPTIONAL_FILES = (MAP, "docs/purecvisor_ovn_demo_architecture.svg")


class SingleUISurfaceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="pcv-single-ui-surface-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        for name in REQUIRED_FILES + OPTIONAL_FILES:
            source = ROOT / name
            if name in OPTIONAL_FILES and not source.exists() and not source.is_symlink():
                continue
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            if name == MAP and source.is_symlink():
                target.symlink_to("fixture-missing-source-map")
            else:
                shutil.copy2(source, target)

    def run_gate(self):
        return subprocess.run(
            ["bash", GATE], cwd=self.root, capture_output=True,
            text=True, encoding="utf-8", timeout=20,
        )

    def replace_once(self, name, before, after):
        target = self.root / name
        text = target.read_text(encoding="utf-8")
        self.assertEqual(text.count(before), 1, f"fixture anchor drift: {before}")
        target.write_text(text.replace(before, after, 1), encoding="utf-8")

    def assert_rejected(self, reason):
        result = self.run_gate()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertEqual(result.stderr.strip(), "FAIL: " + reason)

    def test_current_tree_passes(self):
        result = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stderr, "")
        self.assertEqual(result.stdout, "PASS: single UI surface source boundaries found\n")

    def test_real_event_card_removed_fails(self):
        self.replace_once(MONITOR, "cardHead('실제 이벤트 조회', '서버 기록')",
                          "cardHead('이벤트', '서버 기록')")
        self.assert_rejected("operations triage screen must render the actual server event card")

    def test_alert_button_unwired_with_label_preserved_fails(self):
        self.replace_once(MONITOR, "onClick: function() { navigateTo('mon-alerts'); }",
                          "onClick: function() {}")
        self.assert_rejected("operations triage alert button must navigate to mon-alerts")

    def test_audit_button_unwired_with_other_audit_link_preserved_fails(self):
        self.replace_once(MONITOR, "onClick: function() { navigateTo('mon-audit'); }",
                          "onClick: function() {}")
        self.assert_rejected("operations triage audit button must navigate to mon-audit")

    def test_command_button_unwired_with_label_preserved_fails(self):
        self.replace_once(MONITOR, "onclick: 'openCmdPalette()'", "onclick: 'void(0)'")
        self.assert_rejected("operations triage action button must open the command palette")

    def test_command_button_label_removed_fails(self):
        self.replace_once(MONITOR, "'ci-icon'), '조치 선택'", "'ci-icon'), '다른 동작'")
        self.assert_rejected("operations triage action button must expose the current action label")

    def test_event_card_outside_renderer_does_not_mask_removed_card(self):
        before = "cardHead('실제 이벤트 조회', '서버 기록')"
        self.replace_once(MONITOR, before, "cardHead('이벤트', '서버 기록')")
        with (self.root / MONITOR).open("a", encoding="utf-8") as source:
            source.write("\nfunction unrelatedEventCard() { " + before + "; }\n")
        self.assert_rejected("operations triage screen must render the actual server event card")

    def test_public_source_map_file_reintroduced_fails(self):
        (self.root / MAP).write_text('{"version":3,"sources":[]}', encoding="utf-8")
        self.assert_rejected("public UI must not ship the runtime source map file")

    def test_public_dangling_source_map_symlink_fails(self):
        (self.root / MAP).symlink_to("missing-source-map")
        self.assert_rejected("public UI must not ship the runtime source map file")

    def test_public_source_map_manifest_entry_reintroduced_fails(self):
        with (self.root / MANIFEST).open("a", encoding="utf-8") as manifest:
            manifest.write(MAP + " " + MAP + " replace\n")
        self.assert_rejected("public UI manifest must not deploy source maps")

    def test_public_runtime_source_map_reference_reintroduced_fails(self):
        with (self.root / BUNDLE).open("a", encoding="utf-8") as bundle:
            bundle.write("\n//# sourceMapping" + "URL=app.bundle.js.map\n")
        self.assert_rejected("public runtime bundle must not reference a source map")


if __name__ == "__main__":
    unittest.main(verbosity=2)
