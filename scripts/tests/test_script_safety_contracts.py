#!/usr/bin/env python3
                          
                                                                     
                                                                           
                                                                        
                                                                         
                                                     
 
                      
                                                        
                                                               

from __future__ import annotations

import os
import re
import shutil
import socket
import stat
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent.parent
LOGQUERY = ROOT / "scripts" / "logquery.sh"
BACKUP = ROOT / "scripts" / "backup-projects.sh"
CUSTOM_IMAGE = ROOT / "ops" / "custom-images" / "build-ubuntu-webperf-autoinstall.sh"
CREATE_ISSUES = ROOT / "scripts" / "create_github_issues.sh"
RELEASE = ROOT / "scripts" / "release.sh"
RUN_AUTO_TESTS = ROOT / "scripts" / "run_auto_tests.sh"


def _write_executable(path: Path, body: str) -> None:
                                                       
    path.write_text(body, encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


def _run(script: Path, *args: str, cwd: Path, env: dict[str, str]) -> subprocess.CompletedProcess[str]:
                                                                       
    return subprocess.run(
        ["bash", str(script), *args],
        cwd=cwd,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def test_logquery_uses_argv_and_literal_filters() -> None:
                                                              
    with tempfile.TemporaryDirectory(prefix="pcv-logquery-contract-") as tmp:
        base = Path(tmp)
        fakebin = base / "bin"
        fakebin.mkdir()
        argv_log = base / "sudo.argv"
        marker = base / "injected"
        _write_executable(
            fakebin / "sudo",
            "#!/bin/sh\n"
            "printf '<%s>\\n' \"$@\" >\"$PCV_ARGV_LOG\"\n"
            "printf '%s\\n' '\"lvl\":\"WARN\",\"dom\":\"dispatcher\"' "
            "'\"lvl\":\"WARN\",\"dom\":\"auth\"'\n",
        )
        env = os.environ.copy()
        env.update({"PATH": f"{fakebin}:{env['PATH']}", "PCV_ARGV_LOG": str(argv_log)})

        since = f"now; touch {marker}"
        result = _run(LOGQUERY, "-s", since, "-d", "dispatcher", cwd=base, env=env)
        assert result.returncode == 0, result.stderr
        assert not marker.exists(), "SINCE 문자열이 shell command로 실행됨"
        logged = argv_log.read_text(encoding="utf-8")
        assert f"<{since}>" in logged, logged
        assert '"dom":"dispatcher"' in result.stdout
        assert '"dom":"auth"' not in result.stdout

        argv_log.unlink()
        result = _run(LOGQUERY, "-d", f"x;touch{marker}", cwd=base, env=env)
        assert result.returncode == 2
        assert not argv_log.exists(), "invalid DOMAIN 뒤에도 journal command가 실행됨"
        assert not marker.exists()


def test_custom_image_rejects_canonical_and_inode_aliases_before_writes() -> None:
                                                                           
    with tempfile.TemporaryDirectory(prefix="pcv-custom-image-contract-") as tmp:
        base = Path(tmp)
        fakebin = base / "bin"
        fakebin.mkdir()
        tool_log = base / "image-tools.log"
        for tool in ("xorriso", "openssl", "sed"):
            _write_executable(
                fakebin / tool,
                "#!/bin/sh\n"
                f"printf '%s\\n' '{tool}' >>\"$PCV_IMAGE_TOOL_LOG\"\n"
                "exit 97\n",
            )

        source = base / "source.iso"
        source.write_bytes(b"immutable-source")
        alias_root = base / "alias-root"
        alias_root.symlink_to(base, target_is_directory=True)
        env_base = os.environ.copy()
        env_base.update({
            "PATH": f"{fakebin}:{env_base['PATH']}",
            "PCV_IMAGE_TOOL_LOG": str(tool_log),
            "PCV_WEBPERF_PASSWORD": "fixture-password",
        })

        def reject(output: Path, credential: Path, work_name: str) -> subprocess.CompletedProcess[str]:
            env = env_base.copy()
            env.update({
                "PCV_WEBPERF_CRED_FILE": str(credential),
                "PCV_WEBPERF_ISO_WORKDIR": str(base / work_name),
            })
            return _run(CUSTOM_IMAGE, str(source), str(output), cwd=base, env=env)

                                                                          
        result = reject(alias_root / source.name, base / "cred-one.txt", "work-one")
        assert result.returncode == 2, result.stdout + result.stderr
        assert source.read_bytes() == b"immutable-source"

        output = base / "existing-output.iso"
        output.write_bytes(b"immutable-output")
        result = reject(output, alias_root / output.name, "work-two")
        assert result.returncode == 2, result.stdout + result.stderr
        assert output.read_bytes() == b"immutable-output"

        hardlink_output = base / "source-hardlink.iso"
        os.link(source, hardlink_output)
        result = reject(hardlink_output, base / "cred-three.txt", "work-three")
        assert result.returncode == 2, result.stdout + result.stderr
        assert source.read_bytes() == b"immutable-source"

        hardlink_credential = base / "source-hardlink.cred"
        os.link(source, hardlink_credential)
        result = reject(base / "new-output.iso", hardlink_credential, "work-four")
        assert result.returncode == 2, result.stdout + result.stderr
        assert source.read_bytes() == b"immutable-source"

        sidecar_source = base / "sidecar-output.iso.sha256"
        sidecar_source.write_bytes(b"immutable-sidecar-source")
        env = env_base.copy()
        env.update({
            "PCV_WEBPERF_CRED_FILE": str(base / "cred-five.txt"),
            "PCV_WEBPERF_ISO_WORKDIR": str(base / "work-five"),
        })
        result = _run(
            CUSTOM_IMAGE, str(sidecar_source), str(base / "sidecar-output.iso"),
            cwd=base, env=env,
        )
        assert result.returncode == 2, result.stdout + result.stderr
        assert sidecar_source.read_bytes() == b"immutable-sidecar-source"

                                                                    
                                                                       
        unowned_work = base / "unowned-empty-work"
        unowned_work.mkdir(mode=0o750)
        before = unowned_work.stat()
        env = env_base.copy()
        env.update({
            "PCV_WEBPERF_CRED_FILE": str(base / "cred-six.txt"),
            "PCV_WEBPERF_ISO_WORKDIR": str(unowned_work),
        })
        result = _run(
            CUSTOM_IMAGE, str(source), str(base / "safe-output.iso"), cwd=base, env=env
        )
        assert result.returncode == 2, result.stdout + result.stderr
        after = unowned_work.stat()
        assert (after.st_dev, after.st_ino, stat.S_IMODE(after.st_mode)) == (
            before.st_dev, before.st_ino, stat.S_IMODE(before.st_mode)
        )
        assert not tool_log.exists(), "alias 거부 뒤 image 생성 도구가 실행됨"
        assert not any(base.glob("work-*")), "alias 거부 전에 disposable workdir를 생성함"


def test_backup_cleanup_owns_only_prefixed_root_dataset_snapshots() -> None:
                                                                                
    with tempfile.TemporaryDirectory(prefix="pcv-backup-contract-") as tmp:
        base = Path(tmp)
        fakebin = base / "bin"
        fakebin.mkdir()
        destroy_log = base / "destroy.log"
        _write_executable(
            fakebin / "date",
            "#!/bin/sh\n"
            "if [ \"${1:-}\" = -d ]; then\n"
            "  case \"${2:-}\" in\n"
            "    '7 days ago') echo 1000 ;;\n"
            "    old) echo 100 ;;\n"
            "    *) exit 23 ;;\n"
            "  esac\n"
            "else\n"
            "  echo 12:00:00\n"
            "fi\n",
        )
        _write_executable(
            fakebin / "zfs",
            "#!/bin/sh\n"
            "case \"${1:-}\" in\n"
            "  list)\n"
            "    printf '%s\\n' 'pcvpool/projects@backup-owned old' "
            "'pcvpool/projects@manual-keep old' 'pcvpool/projects/child@backup-child old' ;;\n"
            "  destroy) printf '%s\\n' \"${2:-}\" >>\"$PCV_DESTROY_LOG\" ;;\n"
            "esac\n",
        )
        env = os.environ.copy()
        env.update({"PATH": f"{fakebin}:{env['PATH']}", "PCV_DESTROY_LOG": str(destroy_log)})

        result = _run(BACKUP, "--cleanup", "7", cwd=base, env=env)
        assert result.returncode == 0, result.stderr
        assert destroy_log.read_text(encoding="utf-8").splitlines() == [
            "pcvpool/projects@backup-owned"
        ]


def test_backup_cleanup_rejects_days_and_propagates_zfs_failures() -> None:
                                                                 
    with tempfile.TemporaryDirectory(prefix="pcv-backup-failure-contract-") as tmp:
        base = Path(tmp)
        fakebin = base / "bin"
        fakebin.mkdir()
        zfs_log = base / "zfs.log"
        destroy_log = base / "destroy.log"
        _write_executable(
            fakebin / "date",
            "#!/bin/sh\n"
            "if [ \"${1:-}\" = -d ]; then\n"
            "  case \"${2:-}\" in\n"
            "    '7 days ago') echo 1000 ;;\n"
            "    old) echo 100 ;;\n"
            "    malformed) exit 23 ;;\n"
            "    *) exit 24 ;;\n"
            "  esac\n"
            "else\n"
            "  echo 12:00:00\n"
            "fi\n",
        )
        _write_executable(
            fakebin / "zfs",
            "#!/bin/sh\n"
            "printf '%s\\n' \"$*\" >>\"$PCV_ZFS_LOG\"\n"
            "case \"${PCV_ZFS_MODE:-ok}:${1:-}\" in\n"
            "  list-fail:list) exit 41 ;;\n"
            "  parse-fail:list) printf '%s\\n' 'pcvpool/projects@backup-old old' "
            "'pcvpool/projects@backup-bad malformed' ;;\n"
            "  destroy-fail:list) printf '%s\\n' 'pcvpool/projects@backup-old old' ;;\n"
            "  destroy-fail:destroy) printf '%s\\n' \"${2:-}\" >>\"$PCV_DESTROY_LOG\"; exit 42 ;;\n"
            "  *) exit 0 ;;\n"
            "esac\n",
        )
        env = os.environ.copy()
        env.update({
            "PATH": f"{fakebin}:{env['PATH']}",
            "PCV_ZFS_LOG": str(zfs_log),
            "PCV_DESTROY_LOG": str(destroy_log),
        })

        for args in (("--cleanup",), ("--cleanup", "0"), ("--cleanup", "-1"), ("--cleanup", "nope")):
            if zfs_log.exists():
                zfs_log.unlink()
            result = _run(BACKUP, *args, cwd=base, env=env)
            assert result.returncode == 2, (args, result.stdout, result.stderr)
            assert not zfs_log.exists(), f"invalid cleanup days가 ZFS를 호출함: {args}"

        env["PCV_ZFS_MODE"] = "list-fail"
        result = _run(BACKUP, "--list", cwd=base, env=env)
        assert result.returncode != 0, result.stdout + result.stderr
        result = _run(BACKUP, "--cleanup", "7", cwd=base, env=env)
        assert result.returncode != 0, result.stdout + result.stderr

        env["PCV_ZFS_MODE"] = "parse-fail"
        result = _run(BACKUP, "--cleanup", "7", cwd=base, env=env)
        assert result.returncode != 0, result.stdout + result.stderr
        assert not destroy_log.exists(), "해석 불가 creation을 epoch 0으로 간주해 삭제함"

        env["PCV_ZFS_MODE"] = "destroy-fail"
        result = _run(BACKUP, "--cleanup", "7", cwd=base, env=env)
        assert result.returncode != 0, result.stdout + result.stderr
        assert destroy_log.read_text(encoding="utf-8").splitlines() == [
            "pcvpool/projects@backup-old"
        ]


def test_backup_snapshot_creation_failure_propagates() -> None:
                                                           
    with tempfile.TemporaryDirectory(prefix="pcv-backup-snapshot-contract-") as tmp:
        base = Path(tmp)
        source = base / "projects"
        source.mkdir()
        fakebin = base / "bin"
        fakebin.mkdir()
        zfs_log = base / "zfs.log"
        _write_executable(
            fakebin / "date",
            "#!/bin/sh\n"
            "case \"${1:-}\" in\n"
            "  -d) [ \"${2:-}\" = '30 days ago' ] && echo 1000 || exit 24 ;;\n"
            "  +%s) echo 2000 ;;\n"
            "  +%Y%m%d-%H%M%S) echo 20260813-120000 ;;\n"
            "  *) echo 12:00:00 ;;\n"
            "esac\n",
        )
        _write_executable(fakebin / "du", "#!/bin/sh\nprintf '%s\\n' '1K fixture'\n")
        _write_executable(fakebin / "rsync", "#!/bin/sh\nexit 0\n")
        _write_executable(
            fakebin / "zfs",
            "#!/bin/sh\n"
            "printf '%s\\n' \"$*\" >>\"$PCV_ZFS_LOG\"\n"
            "case \"${1:-}:$*\" in\n"
            "  snapshot:*) exit 42 ;;\n"
            "  list:*'-o avail'*) printf '%s\\n' 10G ;;\n"
            "  list:*'-t snapshot'*) exit 0 ;;\n"
            "  list:*) exit 0 ;;\n"
            "esac\n",
        )
        env = os.environ.copy()
        env.update({
            "PATH": f"{fakebin}:{env['PATH']}",
            "PCV_ZFS_LOG": str(zfs_log),
            "PURECVISOR_PROJECT_BACKUP_SRC": str(source),
        })

        result = _run(BACKUP, "--zfs", cwd=base, env=env)
        assert result.returncode != 0, result.stdout + result.stderr
        assert "ZFS 스냅샷 생성 실패" in result.stdout + result.stderr
        assert "snapshot pcvpool/projects@backup-20260813-120000" in zfs_log.read_text(
            encoding="utf-8"
        )
        assert "-t snapshot" not in zfs_log.read_text(encoding="utf-8"), (
            "새 snapshot 생성 실패 뒤 기존 복구점 retention을 실행함"
        )


def test_create_issues_requires_approval_before_any_gh_call_and_uses_origin() -> None:
                                                            
    with tempfile.TemporaryDirectory(prefix="pcv-issues-contract-") as tmp:
        base = Path(tmp)
        fakebin = base / "bin"
        fakebin.mkdir()
        gh_log = base / "gh.log"
        _write_executable(
            fakebin / "git",
            "#!/bin/sh\n"
            "[ \"${1:-}:${2:-}:${3:-}\" = 'config:--get:remote.origin.url' ] && "
            "printf '%s\\n' 'https://github.com/Acme/current.git'\n",
        )
        _write_executable(
            fakebin / "gh",
            "#!/bin/sh\n"
            "printf '%s\\n' \"$*\" >>\"$PCV_GH_LOG\"\n"
            "case \"${1:-}:${2:-}\" in\n"
            "  auth:status) exit 0 ;;\n"
            "  label:list) printf '%s\\n' bug feature docs P1-critical P2-major P3-minor resolved ;;\n"
            "  api:repos/Acme/current/milestones) printf '%s\\n' Sprint-Bug-Fixes-2026-03 ;;\n"
            "  issue:list)\n"
            "    prev=\n"
            "    for arg in \"$@\"; do\n"
            "      if [ \"$prev\" = --search ]; then\n"
            "        value=${arg% in:title}; value=${value#\\\"}; value=${value%\\\"}\n"
            "        printf '%s\\n' \"$value\"; exit 0\n"
            "      fi\n"
            "      prev=\"$arg\"\n"
            "    done ;;\n"
            "  *) exit 97 ;;\n"
            "esac\n",
        )
        env = os.environ.copy()
        env.pop("PCV_GITHUB_REPO", None)
        env.update({"PATH": f"{fakebin}:{env['PATH']}", "PCV_GH_LOG": str(gh_log)})

        result = _run(CREATE_ISSUES, cwd=base, env=env)
        assert result.returncode == 2
        assert "Acme/current" in result.stdout + result.stderr
        assert not gh_log.exists(), "--yes 없는 확인이 gh를 호출함"

        result = _run(CREATE_ISSUES, "--yes", cwd=base, env=env)
        assert result.returncode == 0, result.stderr
        calls = gh_log.read_text(encoding="utf-8")
        assert "repos/Acme/current/milestones" in calls
        assert "--repo Acme/current" in calls
        assert "HardcoreMonk/purecvisor" not in calls


def test_release_executes_only_canonical_v_semver_tag() -> None:
                                                    
    with tempfile.TemporaryDirectory(prefix="pcv-release-contract-") as tmp:
        base = Path(tmp)
        fakebin = base / "bin"
        fakebin.mkdir()
        git_log = base / "git.log"
        _write_executable(
            fakebin / "git",
            "#!/bin/sh\n"
            "printf '%s :: %s\\n' \"$PWD\" \"$*\" >>\"$PCV_GIT_LOG\"\n"
            "if [ \"${1:-}\" = status ] && [ -n \"${PCV_GIT_DIRTY:-}\" ]; then\n"
            "  printf '%s\\n' ' M tracked-file'\n"
            "fi\n"
            "exit 0\n",
        )
        env = os.environ.copy()
        env.update({"PATH": f"{fakebin}:{env['PATH']}", "PCV_GIT_LOG": str(git_log)})
        release_dir = base / "release"
        release_dir.mkdir()
        sentinel = release_dir / "keep-existing-artifact"
        sentinel.write_text("preserve\n", encoding="utf-8")

        result = _run(RELEASE, "2.0", "--tag-only", cwd=base, env=env)
        assert result.returncode == 2
        assert not git_log.exists(), "invalid version이 local tag 경로까지 도달함"
        assert sentinel.read_text(encoding="utf-8") == "preserve\n"

        result = _run(RELEASE, "9.9.9", "--tag-only", cwd=base, env=env)
        assert result.returncode == 2
        assert not git_log.exists(), "version.h와 다른 tag가 생성 경로까지 도달함"

        result = _run(RELEASE, "2.0.0", "--tag-onl", cwd=base, env=env)
        assert result.returncode == 2
        assert not git_log.exists(), "option 오타가 full release 또는 tag 경로까지 도달함"

        dirty_env = env | {"PCV_GIT_DIRTY": "1"}
        result = _run(RELEASE, "2.0.0", "--tag-only", cwd=base, env=dirty_env)
        assert result.returncode != 0
        assert "tag -l" not in git_log.read_text(encoding="utf-8")
        git_log.unlink()

        result = _run(RELEASE, "2.0.0", "--tag-only", cwd=base, env=env)
        assert result.returncode == 0, result.stderr
        assert sentinel.read_text(encoding="utf-8") == "preserve\n"
        calls = git_log.read_text(encoding="utf-8")
        assert "tag -l v2.0.0" in calls
        assert "tag -s v2.0.0" in calls
        assert "v2.0.0-single" not in calls
        assert str(ROOT) in calls, "외부 CWD에서 그 저장소를 태깅함"


def test_release_full_mode_runs_mandatory_public_gates() -> None:
                                                           
    with tempfile.TemporaryDirectory(prefix="pcv-release-full-contract-") as tmp:
        base = Path(tmp)
        project = base / "project"
        fakebin = base / "bin"
        gate_log = base / "gates.log"
        for directory in (
            project / "scripts", project / "include/purecvisor", project / "bin",
            project / "ui", project / "tests/integration", fakebin,
        ):
            directory.mkdir(parents=True, exist_ok=True)
        shutil.copy2(RELEASE, project / "scripts/release.sh")
        (project / "include/purecvisor/version.h").write_text(
            '#define PCV_PRODUCT_VERSION "2.0.0"\n', encoding="utf-8"
        )
        (project / "ui/app.bundle.js").write_text("/* fixture */\n", encoding="utf-8")
        (project / "ui/sw.js").write_text("/* fixture */\n", encoding="utf-8")

        _write_executable(
            fakebin / "make",
            "#!/bin/sh\n"
            "printf 'make %s\\n' \"$*\" >>\"$PCV_RELEASE_GATE_LOG\"\n"
            "if [ \"${1:-}\" = release ]; then\n"
            "  mkdir -p bin\n"
            "  if [ -n \"${PCV_STAGE_FORBIDDEN:-}\" ]; then\n"
            "    printf 'cluster.forbidden' >bin/purecvisorsd\n"
            "  else\n"
            "    printf daemon >bin/purecvisorsd\n"
            "  fi\n"
            "  printf cli >bin/pcvctl\n"
            "elif [ \"${1:-}\" = test ] && [ -n \"${PCV_STAGE_FORBIDDEN:-}\" ]; then\n"
            "  printf daemon-after-stage >bin/purecvisorsd\n"
            "fi\n",
        )
        for relative in (
            "scripts/bundle-ui.sh",
            "tests/integration/test_single_ovn_ovs_layout.sh",
            "tests/integration/test_single_ui_surface.sh",
            "tests/integration/test_single_backend_build_boundaries.sh",
        ):
            _write_executable(
                project / relative,
                "#!/bin/sh\n"
                f"printf '%s\\n' '{relative}' >>\"$PCV_RELEASE_GATE_LOG\"\n",
            )
        for relative in (
            "scripts/check_ui_bundle_fresh.py",
            "scripts/check_xss.py",
            "scripts/check_design_md.py",
        ):
            (project / relative).write_text(
                "from pathlib import Path\n"
                f"p=Path({str(relative)!r})\n"
                "with Path(__import__('os').environ['PCV_RELEASE_GATE_LOG']).open('a') as f: f.write(str(p)+'\\n')\n",
                encoding="utf-8",
            )
        _write_executable(
            fakebin / "git",
            "#!/bin/sh\n"
            "printf 'git %s\\n' \"$*\" >>\"$PCV_RELEASE_GATE_LOG\"\n",
        )
        _write_executable(
            fakebin / "strings",
            "#!/bin/sh\n"
            "printf 'strings %s\\n' \"${1:-}\" >>\"$PCV_RELEASE_GATE_LOG\"\n"
            "if [ -n \"${PCV_FORBIDDEN_STRING:-}\" ]; then\n"
            "  printf '%s\\n' 'cluster.forbidden'\n"
            "else\n"
            "  cat \"${1:?missing artifact}\"\n"
            "fi\n",
        )
        _write_executable(fakebin / "gpg", "#!/bin/sh\nexit 1\n")

        env = os.environ.copy()
        env.update({
            "PATH": f"{fakebin}:{env['PATH']}",
            "PCV_RELEASE_GATE_LOG": str(gate_log),
        })
        result = _run(project / "scripts/release.sh", "2.0.0", cwd=base, env=env)
        assert result.returncode == 0, result.stdout + result.stderr
        calls = gate_log.read_text(encoding="utf-8")
        for expected in (
            "make clean", "make release", "make test", "make check-all",
            "scripts/bundle-ui.sh", "scripts/check_ui_bundle_fresh.py",
            "scripts/check_xss.py", "scripts/check_design_md.py",
            "tests/integration/test_single_ovn_ovs_layout.sh",
            "tests/integration/test_single_ui_surface.sh",
            "tests/integration/test_single_backend_build_boundaries.sh",
            "git tag -l v2.0.0", "git tag -s v2.0.0",
        ):
            assert expected in calls, (expected, calls)
        assert (project / "release/purecvisorsd").read_text(encoding="utf-8") == "daemon"
        assert (project / "release/pcvctl").read_text(encoding="utf-8") == "cli"
        string_calls = [line for line in calls.splitlines() if line.startswith("strings ")]
        assert len(string_calls) == 2, string_calls
        assert all(f"strings {project}/bin/" not in line for line in string_calls), string_calls

        before_forbidden = gate_log.read_text(encoding="utf-8")
        forbidden_env = env | {"PCV_FORBIDDEN_STRING": "1"}
        result = _run(project / "scripts/release.sh", "2.0.0", cwd=base, env=forbidden_env)
        assert result.returncode != 0
        forbidden_calls = gate_log.read_text(encoding="utf-8")[len(before_forbidden):]
        assert "git tag -l" not in forbidden_calls, "금지 문자열 뒤 tag 경로까지 도달함"

                                                                  
                                                     
        before_staged = gate_log.read_text(encoding="utf-8")
        staged_env = env | {"PCV_STAGE_FORBIDDEN": "1"}
        result = _run(project / "scripts/release.sh", "2.0.0", cwd=base, env=staged_env)
        assert result.returncode != 0, result.stdout + result.stderr
        assert (project / "bin/purecvisorsd").read_text(encoding="utf-8") == (
            "daemon-after-stage"
        )
        staged_calls = gate_log.read_text(encoding="utf-8")[len(before_staged):]
        assert "git tag -l" not in staged_calls, "staging 금지 문자열 뒤 tag 경로까지 도달함"


def test_run_auto_tests_dispatches_argv_without_eval() -> None:
                                                           
    with tempfile.TemporaryDirectory(prefix="pcv-run-auto-contract-") as tmp:
        base = Path(tmp)
        fakebin = base / "bin"
        fakebin.mkdir()
        make_log = base / "make.log"
        marker = base / "injected"
        _write_executable(
            fakebin / "make",
            "#!/bin/sh\n"
            "printf '%s\\n' \"$*\" >>\"$PCV_MAKE_LOG\"\n"
            "[ \"$*\" = 'test-auto' ] || exit 43\n"
            "printf '%s\\n' 'PASS fake unit suite'\n",
        )
        env = os.environ.copy()
        env.update({"PATH": f"{fakebin}:{env['PATH']}", "PCV_MAKE_LOG": str(make_log)})

        result = _run(RUN_AUTO_TESTS, "--tier", "0", cwd=base, env=env)
        assert result.returncode == 0, result.stdout + result.stderr
        assert make_log.read_text(encoding="utf-8").splitlines() == ["test-auto"]
        assert "T0: command not found" not in result.stdout + result.stderr

        make_log.unlink()
        hostile_host = f"localhost;touch {marker}"
        result = _run(
            RUN_AUTO_TESTS, "--tier", "0", "--host", hostile_host, cwd=base, env=env
        )
        assert result.returncode == 2
        assert not make_log.exists(), "invalid host 뒤에도 make가 실행됨"
        assert not marker.exists(), "host 문자열이 shell command로 실행됨"

        auto_dry = subprocess.run(
            ["make", "-n", "test-auto"], cwd=ROOT, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
        )
        assert auto_dry.returncode == 0, auto_dry.stdout + auto_dry.stderr
        assert "sudo ./test_runner -v -s /dpdk/bridge_delete/idempotent" in auto_dry.stdout
        full_dry = subprocess.run(
            ["make", "-n", "test"], cwd=ROOT, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
        )
        assert full_dry.returncode == 0, full_dry.stdout + full_dry.stderr
        assert "sudo ./test_runner -v > test_results.txt" in full_dry.stdout
        assert "-s /dpdk/bridge_delete/idempotent" not in full_dry.stdout


def test_run_auto_tests_tier2_cleanup_parser_fails_closed() -> None:
                                                                
    with tempfile.TemporaryDirectory(prefix="pcv-run-auto-tier2-parser-") as tmp:
        base = Path(tmp)
        fakebin = base / "bin"
        fakebin.mkdir()
        socket_path = base / "daemon.sock"
        sudo_log = base / "sudo.log"
        uds = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        uds.bind(str(socket_path))
        try:
            _write_executable(
                fakebin / "sudo",
                "#!/bin/sh\n"
                "printf '%s\\n' \"$*\" >>\"$PCV_SUDO_LOG\"\n"
                "printf '%s\\n' 'PASS fake Tier 2 command'\n",
            )
            _write_executable(
                fakebin / "nc",
                "#!/bin/sh\n"
                "case \"${PCV_NC_MODE:-raw}\" in\n"
                "  raw) printf '%s' \"${PCV_NC_RESPONSE:?}\" ;;\n"
                "  unrelated)\n"
                "    printf '%s' '{\"result\":[{\"username\":\"test-manual\"},"
                "{\"username\":\"pcv-op-other-run\"},"
                "{\"username\":\"pcv-view-other-run\"},"
                "{\"username\":\"pcv-bad-other-run\"}]}' ;;\n"
                "  leaked)\n"
                "    run_id=$(sed -n 's/.*PCV_TEST_RUN_ID=\\([^ ]*\\).*/\\1/p' "
                "\"$PCV_SUDO_LOG\" | tail -n 1)\n"
                "    [ -n \"$run_id\" ] || exit 70\n"
                "    printf '{\"result\":[{\"username\":\"pcv-op-%s\"},"
                "{\"username\":\"pcv-view-%s\"},"
                "{\"username\":\"pcv-bad-%s\"},"
                "{\"username\":\"test-manual\"},"
                "{\"username\":\"pcv-op-other-run\"}]}' "
                "\"$run_id\" \"$run_id\" \"$run_id\" ;;\n"
                "  *) exit 71 ;;\n"
                "esac\n",
            )
            env = os.environ.copy()
            env.update({
                "PATH": f"{fakebin}:{env['PATH']}",
                "PCV_TEST_SOCKET_PATH": str(socket_path),
                "PCV_SUDO_LOG": str(sudo_log),
                                                                  
                "PCV_TEST_RUN_ID": "../../caller-controlled",
            })
            env.pop("PCV_NC_MODE", None)

            for envelope in ('{"result":[]}', '{"data":[]}'):
                sudo_log.unlink(missing_ok=True)
                result = _run(
                    RUN_AUTO_TESTS,
                    "--tier", "2", "--ci",
                    cwd=base,
                    env=env | {"PCV_NC_RESPONSE": envelope},
                )
                assert result.returncode == 0, result.stdout + result.stderr
                assert "테스트 사용자 잔류 없음" in result.stdout
                sudo_calls = sudo_log.read_text(encoding="utf-8").splitlines()
                assert len(sudo_calls) == 2, sudo_calls
                run_ids = []
                for call in sudo_calls:
                    match = re.search(r"(?:^| )PCV_TEST_RUN_ID=([A-Za-z0-9_-]+)(?: |$)", call)
                    assert match, call
                    run_ids.append(match.group(1))
                assert run_ids[0] == run_ids[1]
                assert run_ids[0].startswith("auto-") and len(run_ids[0]) <= 40
                assert sudo_calls == [
                    f"env PCV_TEST_SOCKET_PATH={socket_path} "
                    f"PCV_TEST_RUN_ID={run_ids[0]} bash "
                    f"{ROOT}/tests/integration/test_rbac_template_backup.sh",
                    f"env PCV_TEST_SOCKET_PATH={socket_path} "
                    f"PCV_TEST_RUN_ID={run_ids[0]} bash "
                    f"{ROOT}/tests/integration/test_core_enhancement.sh",
                ]

            rejected = (
                '{"error":{"code":-32603},"result":[]}',
                '{"jsonrpc":"2.0","id":"1"}',
                '{"result":{}}',
            )
            for envelope in rejected:
                sudo_log.unlink(missing_ok=True)
                result = _run(
                    RUN_AUTO_TESTS,
                    "--tier", "2", "--ci",
                    cwd=base,
                    env=env | {"PCV_NC_RESPONSE": envelope},
                )
                assert result.returncode != 0, (envelope, result.stdout, result.stderr)
                assert "Tier 2 cleanup 조회 실패" in result.stdout
                assert "테스트 사용자 잔류 없음" not in result.stdout

                                                           
                                                    
                                                 
            sudo_log.unlink(missing_ok=True)
            unrelated = _run(
                RUN_AUTO_TESTS,
                "--tier", "2", "--ci",
                cwd=base,
                env=env | {"PCV_NC_MODE": "unrelated", "PCV_NC_RESPONSE": "unused"},
            )
            assert unrelated.returncode == 0, unrelated.stdout + unrelated.stderr
            assert "테스트 사용자 잔류 없음" in unrelated.stdout

            sudo_log.unlink(missing_ok=True)
            leaked = _run(
                RUN_AUTO_TESTS,
                "--tier", "2", "--ci",
                cwd=base,
                env=env | {"PCV_NC_MODE": "leaked", "PCV_NC_RESPONSE": "unused"},
            )
            assert leaked.returncode != 0, leaked.stdout + leaked.stderr
            assert "Tier 2 테스트 사용자 3건 잔류" in leaked.stdout
            assert "테스트 사용자 잔류 없음" not in leaked.stdout

            for child in (
                ROOT / "tests/integration/test_rbac_template_backup.sh",
                ROOT / "tests/integration/test_core_enhancement.sh",
            ):
                assert 'SOCKET_PATH="${PCV_TEST_SOCKET_PATH:-' in child.read_text(
                    encoding="utf-8"
                ), f"{child.name}가 전달된 격리 UDS를 사용하지 않음"
        finally:
            uds.close()


if __name__ == "__main__":
    tests = [value for name, value in sorted(globals().items()) if name.startswith("test_")]
    failed = 0
    for test in tests:
        try:
            test()
            print(f"PASS {test.__name__}")
        except Exception as exc:                                              
            failed += 1
            print(f"FAIL {test.__name__}: {exc}")
    print(f"[test-script-safety-contracts] {len(tests) - failed}/{len(tests)} passed")
    raise SystemExit(1 if failed else 0)
