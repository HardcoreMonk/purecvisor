#!/usr/bin/env python3




import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PACKAGE = "purecvisor-single-lifecycle-fixture"


@unittest.skipUnless(os.geteuid() == 0, "격리 dpkg lifecycle은 root chroot 권한 필요")
class GeneratedConfigLifecycle(unittest.TestCase):
    def test_real_dpkg_lifecycle_in_chroot(self):
        source = (ROOT / "packaging/deb/build-deb.sh").read_text()
        scripts = {}
        for name, tag in [("postinst", "POST"), ("prerm", "PRE"), ("postrm", "PRM")]:
            scripts[name] = re.search(
                r'cat > "\$STAGE/DEBIAN/' + name + r'" <<\'' + tag + r"'\n(.*?)\n" + tag,
                source, re.S).group(1) + "\n"
        busybox = shutil.which("busybox")
        self.assertIsNotNone(busybox, "정적 busybox가 필요하다")

        linkage = subprocess.run(["ldd", busybox], capture_output=True, text=True)
        self.assertNotIn("=>", linkage.stdout, "정적 busybox로 실행해야 한다")
        with tempfile.TemporaryDirectory(prefix="pcv-dpkg-lifecycle-") as directory:
            base = Path(directory)
            root = base / "root"
            for path in ["bin", "usr/bin", "etc", "dev", "var/lib/dpkg", "tmp"]:
                (root / path).mkdir(parents=True, exist_ok=True)
            shutil.copy2(busybox, root / "bin/busybox")
            for command in ["sh", "mkdir", "install", "mktemp", "rm", "mv", "cp", "chmod", "chown", "ln"]:
                (root / "bin" / command).symlink_to("busybox")
            for command in ["systemctl", "getent", "groupadd"]:
                path = root / "usr/bin" / command
                path.write_text("#!/bin/sh\nexit 0\n")
                path.chmod(0o755)
            (root / "etc/passwd").write_text("root:x:0:0:root:/root:/bin/sh\n")
            (root / "etc/group").write_text("root:x:0:\n")
            (root / "dev/null").touch()
            (root / "var/lib/dpkg/status").touch()

            def run(*arguments):
                result = subprocess.run(arguments, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                print(result.stdout.strip())
                return result

            def build(version):
                stage = base / ("stage-" + version)
                metadata = stage / "DEBIAN"
                metadata.mkdir(parents=True)
                (metadata / "control").write_text(
                    f"Package: {PACKAGE}\nVersion: {version}\nArchitecture: all\n"
                    "Maintainer: Local fixture <fixture@localhost>\n"
                    "Description: Isolated exact-maintainer-script lifecycle fixture\n")
                for name, text in scripts.items():
                    (metadata / name).write_text(text)
                    (metadata / name).chmod(0o755)
                files = {
                    "etc/purecvisor/daemon.conf.sample": f"sample-version={version}\n",
                    "etc/apparmor.d/usr.local.bin.purecvisorsd": "# fixture profile\n",
                    "usr/local/share/purecvisor/fallback/.defaults/maintenance-status.json": "{}\n",
                }
                for path, text in files.items():
                    target = stage / path
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_text(text)
                (metadata / "conffiles").write_text("".join("/" + p + "\n" for p in files if p.startswith("etc/")))
                package = base / (version + ".deb")
                run("dpkg-deb", "--build", "--root-owner-group", str(stage), str(package))
                return str(package)

            def dpkg(*arguments):
                return run("dpkg", "--root=" + str(root), "--force-confnew", *arguments)

            first, second = build("1.0"), build("2.0")
            config = root / "etc/purecvisor/daemon.conf"
            dpkg("--install", first)
            self.assertEqual(config.read_text(), "sample-version=1.0\n")
            self.assertEqual(config.stat().st_mode & 0o777, 0o600)
            for name, text in scripts.items():
                self.assertEqual((root / f"var/lib/dpkg/info/{PACKAGE}.{name}").read_text(), text)
            config.write_text("operator-customized=true\n")
            retained = [root / "var/lib/purecvisor/jobs.db", root / "var/log/purecvisor/audit.log",
                        root / "etc/purecvisor/pki/fixture.crt"]
            for path in retained:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("retain-this-data\n")
            dpkg("--install", second)
            self.assertEqual(config.read_text(), "operator-customized=true\n")
            self.assertEqual(config.stat().st_mode & 0o777, 0o600)
            dpkg("--remove", PACKAGE)
            self.assertEqual(config.read_text(), "operator-customized=true\n")
            dpkg("--purge", PACKAGE)
            self.assertFalse(config.exists())
            for path in retained:
                self.assertEqual(path.read_text(), "retain-this-data\n")
            dpkg("--install", second)
            self.assertEqual(config.read_text(), "sample-version=2.0\n")
            dpkg("--purge", PACKAGE)
            dpkg("--purge", PACKAGE)
            self.assertFalse(config.exists())
            print("PASS: configure -> upgrade -> remove -> purge -> reinstall -> repeated purge")


if __name__ == "__main__":
    unittest.main()
