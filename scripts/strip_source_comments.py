#!/usr/bin/env python3
import argparse
import ast
import io
import re
import sys
import tokenize
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SKIP_PARTS = {".astro", ".git", "dist", "node_modules", "vendor"}
CLIKE_SUFFIXES = {".c", ".h", ".proto", ".css"}
JAVASCRIPT_SUFFIXES = {".js", ".mjs"}
PYTHON_SUFFIXES = {".py"}
HTML_SUFFIXES = {".html", ".xml", ".svg"}
HASH_SUFFIXES = {".sh", ".service", ".conf", ".template", ".sample", ".supp", ".yaml", ".yml", ".toml", ".gitignore"}
HASH_NAMES = {"Makefile", "pre-commit", "commit-msg", "purecvisor.example.com", "pcv-apparmor", "usr.local.bin.purecvisorsd"}


def mask(text, start, end):
    return "".join("\n" if c == "\n" else " " for c in text[start:end])


def strip_clike(text, line_comments=True, block_comments=True):
    out = []
    i = 0
    state = "code"
    quote = ""
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if state == "code":
            if ch in {'"', "'", "`"}:
                state = "string"
                quote = ch
                out.append(ch)
                i += 1
                continue
            if line_comments and ch == "/" and nxt == "/":
                j = text.find("\n", i)
                if j < 0:
                    j = len(text)
                out.append(mask(text, i, j))
                i = j
                continue
            if block_comments and ch == "/" and nxt == "*":
                j = text.find("*/", i + 2)
                if j < 0:
                    j = len(text) - 2
                end = min(len(text), j + 2)
                out.append(mask(text, i, end))
                i = end
                continue
            out.append(ch)
            i += 1
            continue
        out.append(ch)
        if ch == "\\" and i + 1 < len(text):
            out.append(text[i + 1])
            i += 2
            continue
        if ch == quote:
            state = "code"
        i += 1
    return "".join(out)


def python_spans(text):
    spans = []
    source_lines = text.splitlines()

    def character_column(row, byte_column):
        if row < 1 or row > len(source_lines):
            return byte_column
        raw = source_lines[row - 1].encode("utf-8", errors="surrogateescape")
        return len(raw[:byte_column].decode("utf-8", errors="ignore"))

    try:
        for tok in tokenize.generate_tokens(io.StringIO(text).readline):
            if tok.type == tokenize.COMMENT:
                if tok.start[1] == 0 and tok.string.startswith("#!"):
                    continue
                spans.append((tok.start, tok.end))
    except (IndentationError, tokenize.TokenError):
        pass
    try:
        tree = ast.parse(text)
    except SyntaxError:
        return spans
    nodes = [tree]
    nodes.extend(n for n in ast.walk(tree) if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)))
    for node in nodes:
        body = getattr(node, "body", [])
        if not body:
            continue
        first = body[0]
        if isinstance(first, ast.Expr) and isinstance(first.value, ast.Constant) and isinstance(first.value.value, str):
            spans.append((
                (first.lineno, character_column(first.lineno, first.col_offset)),
                (first.end_lineno, character_column(first.end_lineno, first.end_col_offset)),
            ))
    return spans


def apply_line_spans(text, spans):
    lines = text.splitlines(keepends=True)
    for (sr, sc), (er, ec) in spans:
        for row in range(sr - 1, er):
            if row >= len(lines):
                continue
            start = sc if row == sr - 1 else 0
            end = ec if row == er - 1 else len(lines[row].rstrip("\r\n"))
            lines[row] = lines[row][:start] + " " * max(0, end - start) + lines[row][end:]
    return "".join(lines)


def strip_python(text):
    return apply_line_spans(text, python_spans(text))


def strip_hash(text, apparmor=False):
    out = []
    state = "code"
    for line_no, line in enumerate(text.splitlines(keepends=True)):
        bare = line.lstrip()
        if bare.startswith("#!"):
            out.append(line)
            continue
        if apparmor and re.match(r"#(?:include|abi|if|else|endif)\b", bare):
            first = line.find("#")
            second = line.find("#", first + 1)
            if second >= 0:
                end = len(line)
                while end and line[end - 1] in "\r\n":
                    end -= 1
                line = line[:second] + " " * (end - second) + line[end:]
            out.append(line)
            continue
        chars = list(line)
        i = 0
        state = "code"
        quote = ""
        while i < len(chars):
            ch = chars[i]
            if state == "code":
                if ch in {'"', "'", "`"}:
                    state = "string"
                    quote = ch
                    i += 1
                    continue
                if ch == "#" and (i == 0 or chars[i - 1].isspace() or chars[i - 1] in ";|&()"):
                    end = len(chars)
                    while end and chars[end - 1] in "\r\n":
                        end -= 1
                    chars[i:end] = [" "] * (end - i)
                    break
                if ch == "\\":
                    i += 2
                    continue
                i += 1
                continue
            if ch == "\\" and state != "single":
                i += 2
                continue
            if ch == quote:
                state = "code"
            i += 1
        out.append("".join(chars))
    return "".join(out)


def strip_html(text):
    text = re.sub(r"<!--.*?-->", lambda m: mask(text, m.start(), m.end()), text, flags=re.DOTALL)
    text = re.sub(
        r"(<style\b[^>]*>)(.*?)(</style\s*>)",
        lambda m: m.group(1) + strip_clike(m.group(2), line_comments=False) + m.group(3),
        text,
        flags=re.IGNORECASE | re.DOTALL,
    )
    text = re.sub(
        r"(<script\b[^>]*>)(.*?)(</script\s*>)",
        lambda m: m.group(1) + strip_clike(m.group(2)) + m.group(3),
        text,
        flags=re.IGNORECASE | re.DOTALL,
    )
    return text


def classify(path):
    if any(part in SKIP_PARTS for part in path.parts):
        return None
    if path.suffix in PYTHON_SUFFIXES:
        return "python"
    if path.suffix in HTML_SUFFIXES:
        return "html"
    if path.suffix in JAVASCRIPT_SUFFIXES:
        return "javascript"
    if path.suffix in CLIKE_SUFFIXES:
        return "clike"
    if path.suffix in HASH_SUFFIXES or path.name in HASH_NAMES:
        return "hash"
    return None


def transform(path, kind):
    text = path.read_text(errors="surrogateescape")
    if kind == "python":
        return text, strip_python(text)
    if kind == "html":
        return text, strip_html(text)
    if kind == "clike":
        return text, strip_clike(text, line_comments=path.suffix != ".css")
    if kind == "javascript":
        return text, strip_clike(text)
    return text, strip_hash(text, apparmor="apparmor" in path.parts or path.name == "pcv-apparmor")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    changed = []
    for path in sorted(ROOT.rglob("*")):
        if not path.is_file():
            continue
        rel = path.relative_to(ROOT)
        kind = classify(rel)
        if not kind:
            continue
        if kind == "javascript":
            continue
        before, after = transform(path, kind)
        if before == after:
            continue
        changed.append(str(rel))
        if not args.check:
            path.write_text(after, errors="surrogateescape")
    if args.check:
        for rel in changed:
            print(rel)
        if changed:
            print(f"comment-bearing first-party files: {len(changed)}", file=sys.stderr)
            return 1
        print("first-party source comments: 0")
        return 0
    print(f"stripped files: {len(changed)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
