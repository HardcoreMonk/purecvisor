#!/usr/bin/env python3
import pathlib
import resource
import shlex
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]


def main():
    source = ROOT / "src/modules/lxc/lxc_storage.c"
    assert source.is_file(), "production LXC storage module is not implemented"
    flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "gio-2.0"], text=True))
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with tempfile.TemporaryDirectory(prefix="pcv-storage-test-") as directory:
        binary = pathlib.Path(directory) / "storage-test"
        command = ["gcc", "-std=gnu2x", "-Wall", "-Wextra", "-Werror",
                        "-g", "-I", str(ROOT / "src"),
                        str(ROOT / "scripts/tests/lxc_storage_fixture.c"),
                        "-o", str(binary), *flags]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)
        mutant_dir = pathlib.Path(directory) / "mutant"
        mutant_source = mutant_dir / "modules/lxc/lxc_storage.c"
        mutant_source.parent.mkdir(parents=True)
        production = source.read_text()
        needle = "return (!g_strcmp0(fs, identity->fs)"
        assert production.count(needle) == 1
        mutant_source.write_text(production.replace(
            needle, "return TRUE || (!g_strcmp0(fs, identity->fs)"))
        (mutant_source.parent / "lxc_storage.h").write_text(
            source.with_suffix(".h").read_text())
        subprocess.run([*command[:1], "-I", str(mutant_dir), *command[1:]], check=True)
        result = subprocess.run([str(binary), "-p", "/storage/record-and-tamper"],
                                capture_output=True, text=True)
        assert result.returncode != 0 and "should be FALSE" in result.stderr, result
        print("OK counterfactual: bypassed actual identity comparison is rejected")


if __name__ == "__main__":
    main()
