#!/usr/bin/env python3
                          
                                                                                                 
                                               
                                                                                                               
 
                      
                                                                      
                                                                              
                                                                          

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HELPER = "src/utils/pcv_zfs_exec.c"
LOCK_IMPLS = ("src/utils/pcv_zfs_lock.c", "src/utils/pcv_zfs_lock.h")
                                                                  
REMOTE_EXEMPT = {("src/modules/backup/backup_scheduler.c", "destroy")}

TOKEN_RE = re.compile(
    r'/\*.*?\*/|//[^\n]*|\'(?:\\.|[^\'\\])*\'|"(?:\\.|[^"\\])*"', re.S)
LOCK_CALL_RE = re.compile(r"\bpcv_zfs_pool_(?:lock|unlock)\s*\(")
SHELL_RE = re.compile(r"^zfs\s+(create|destroy)\s+[-%\"']")


def scan_text(text: str):
                                                              
    literals = []
    masked = []
    pos = 0
    for m in TOKEN_RE.finditer(text):
        masked.append(text[pos:m.start()])
        token = m.group()
        if token.startswith('"'):
            literals.append((m.start(), token[1:-1]))
        masked.append(re.sub(r"[^\n]", " ", token))
        pos = m.end()
    masked.append(text[pos:])
    code = "".join(masked)

    def line(offset: int) -> int:
        return text.count("\n", 0, offset) + 1

    hits = []
    for i, (offset, value) in enumerate(literals):
        shell = SHELL_RE.match(value)
        if shell and "fail" not in value:                                               
            hits.append((line(offset), f"셸 문자열 zfs {shell.group(1)}", shell.group(1), False))
        if value != "zfs" or i + 1 >= len(literals):
            continue
        next_offset, next_value = literals[i + 1]
        if next_value in ("create", "destroy") and next_offset - offset < 200:
            remote = i > 0 and literals[i - 1][1] == "sudo"
            hits.append((line(offset), f'argv "zfs", "{next_value}"', next_value, remote))
    for m in LOCK_CALL_RE.finditer(code):
        hits.append((line(m.start()), "pcv_zfs_pool_lock/unlock 직접 호출", "lock", False))
    return hits


def check_file(rel: str, text: str) -> list[str]:
    errors = []
    for lineno, what, op, remote in scan_text(text):
        if rel == HELPER:
            continue
        if op == "lock" and rel in LOCK_IMPLS:
            continue
        if remote and (rel, op) in REMOTE_EXEMPT:
            continue
        errors.append(f"{rel}:{lineno}: {what} — utils/pcv_zfs_exec.h의 창구를 쓴다")
    return errors


def check_helper(text: str) -> list[str]:
                                                                
    match = re.search(r"_zfs_run_locked\([^)]*\)\s*\{(.*?)\n\}", text, re.S)
    if not match:
        return [f"{HELPER}: _zfs_run_locked가 없다"]
    body = match.group(1)
    lock = body.find("pcv_zfs_pool_lock(")
    spawn = body.find("pcv_spawn_sync(")
    unlock = body.find("pcv_zfs_pool_unlock(")
    if not 0 <= lock < spawn < unlock:
        return [f"{HELPER}: 잠금 → pcv_spawn_sync → 해제 순서가 아니다"]
    between = body[lock:unlock]
    calls = [c for c in re.findall(r"\b([A-Za-z_]\w*)\s*\(", between)
             if c not in ("pcv_zfs_pool_lock", "_zfs_lock_wait_ms", "pcv_spawn_sync", "if", "g_autofree")]
    if calls:
        return [f"{HELPER}: 락 안에서 zfs 명령 말고 다른 호출이 돈다: {calls}"]
    if "return" in body[spawn:unlock]:
        return [f"{HELPER}: spawn과 해제 사이에 return이 있어 락이 샌다"]
    return []


def self_test() -> int:
    bad = [
        ('const gchar *a[] = {"zfs", "destroy", "-r", x, NULL};', "plain destroy"),
        ('const gchar *a[] = {\n  "zfs", "create",\n  "-V", s, x, NULL };', "multiline create"),
        ('_argv_add(a, "zfs"); _argv_add(a, "destroy");', "builder"),
        ('g_ptr_array_add(a, (gchar *)"zfs");\ng_ptr_array_add(a, (gchar *)"create");', "ptr array"),
        ('cmd = g_strdup_printf("zfs destroy -r %s", x);', "shell string"),
        ('if (!pcv_zfs_pool_lock(p, "x", 1, e)) return;', "direct lock"),
        ('const gchar *a[] = {"sudo", "zfs", "destroy", x, NULL};', "sudo elsewhere"),
    ]
    for text, name in bad:
        if not check_file("src/modules/x/y.c", text):
            print(f"FAIL self-test: 잡아야 할 모양을 놓쳤다: {name}", file=sys.stderr)
            return 1
    good = [
        '/* {"zfs", "destroy", x} 설명 */ int a;',
        '// "zfs", "create"\nint a;',
        'const gchar *a[] = {"zfs", "list", "-H", x, NULL};',
        'const gchar *a[] = {"zfs", "snapshot", x, NULL};',
        'PCV_LOG_WARN("d", "zfs destroy failed: %s", x);',
        'PCV_LOG_WARN("d", "zfs create failed", x);',
    ]
    for text in good:
        if check_file("src/modules/x/y.c", text):
            print(f"FAIL self-test: 오탐: {text}", file=sys.stderr)
            return 1
    if check_file("src/modules/backup/backup_scheduler.c", 'char *a[] = {"sudo", "zfs", "destroy", x};'):
        print("FAIL self-test: 원격 예외가 동작하지 않는다", file=sys.stderr)
        return 1
    if not check_file("src/modules/backup/backup_scheduler.c", 'char *a[] = {"zfs", "destroy", x};'):
        print("FAIL self-test: 원격 예외가 로컬 argv까지 넓어졌다", file=sys.stderr)
        return 1
    good_helper = """_zfs_run_locked(const gchar *op)
{
    if (!pcv_zfs_pool_lock(target, op, _zfs_lock_wait_ms(target), error))
        return FALSE;
    g_autofree gchar *captured = NULL;
    const gboolean ok = pcv_spawn_sync(argv, NULL, &captured, error);
    pcv_zfs_pool_unlock(target);
    return ok;
}
"""
    if check_helper(good_helper):
        print(f"FAIL self-test: 창구 오탐: {check_helper(good_helper)}", file=sys.stderr)
        return 1
    for mutant, name in (
        (good_helper.replace("    pcv_zfs_pool_unlock(target);\n", ""), "unlock 없음"),
        (good_helper.replace("if (!pcv_zfs_pool_lock(target, op, _zfs_lock_wait_ms(target), error))\n        return FALSE;\n", ""), "lock 없음"),
        (good_helper.replace("    pcv_zfs_pool_unlock(target);\n", "    if (!ok) return FALSE;\n    pcv_zfs_pool_unlock(target);\n"), "실패 경로 누수"),
        (good_helper.replace("    const gboolean ok =", "    g_usleep(1000);\n    const gboolean ok ="), "락 안 대기"),
    ):
        if not check_helper(mutant):
            print(f"FAIL self-test: 창구 변형을 놓쳤다: {name}", file=sys.stderr)
            return 1
    print("[PASS] zfs mutation argv gate self-test")
    return 0


def main(argv: list[str]) -> int:
    if "--self-test" in argv:
        return self_test()
    errors: list[str] = []
    for path in sorted((ROOT / "src").rglob("*")):
        if path.suffix not in (".c", ".h"):
            continue
        rel = path.relative_to(ROOT).as_posix()
        errors += check_file(rel, path.read_text(encoding="utf-8", errors="replace"))
    helper = ROOT / HELPER
    if helper.exists():
        errors += check_helper(helper.read_text(encoding="utf-8"))
    else:
        errors.append(f"{HELPER}: 창구 파일이 없다")
    if errors:
        print("FAIL: zfs create·destroy 단일 창구 계약 위반", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1
    print("[PASS] zfs create·destroy argv는 pcv_zfs_exec.c에만 있다")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
