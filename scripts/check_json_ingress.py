#!/usr/bin/env python3
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rpc_extract
from check_mainloop_guest_agent import function_spans, mask_code

ROOT = Path(__file__).resolve().parent.parent
INGRESS = ["src/api/ws_server.c", "src/api/uds_server.c", "src/api/grpc_server.c",
           "src/api/rest_server.c"]
RAW_RE = re.compile(r'\bjson_parser_load_from_data\s*\(')
TRUSTED_FILE = ROOT / "contracts" / "json_ingress_trusted.json"
FIELDS = {"file", "function", "data", "reason"}
FILE_SCOPE = "<file-scope>"


def _call_args(code: str, start: int) -> list:
    args, depth, cur = [], 1, []
    i = start
    while i < len(code):
        ch = code[i]
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
            if depth == 0:
                args.append("".join(cur))
                return [re.sub(r"\s+", " ", a).strip() for a in args]
        if ch == "," and depth == 1:
            args.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
        i += 1
    raise ValueError(f"닫히지 않은 호출 인자(줄 {code.count(chr(10), 0, start) + 1})")


def find_sites(text: str) -> list:
    code = mask_code(text)
    spans = function_spans(code)
    sites = []
    for m in RAW_RE.finditer(code):
        pos = m.start()
        func = next((name for name, s, e in spans if s <= pos <= e), FILE_SCOPE)
        args = _call_args(code, m.end())
        data = args[1] if len(args) > 1 else ""
        sites.append((func, data, code.count("\n", 0, pos) + 1))
    return sites


def ingress_files() -> list:
    rels = list(INGRESS)
    rels += [p.relative_to(ROOT).as_posix() for p in rpc_extract.rest_family(ROOT)]
    rels += [p.relative_to(ROOT).as_posix() for p in rpc_extract.dispatcher_family(ROOT)]
    return list(dict.fromkeys(rels))


def load_trusted(entries, files) -> dict:
    if not isinstance(entries, list):
        raise ValueError("신뢰 목록은 사이트 객체의 배열이어야 한다")
    trusted = {}
    for i, spec in enumerate(entries):
        if not isinstance(spec, dict) or set(spec) != FIELDS:
            raise ValueError(f"항목 {i}: 필드는 정확히 {sorted(FIELDS)} 이어야 한다")
        for field in sorted(FIELDS):
            if not isinstance(spec[field], str) or not spec[field].strip():
                raise ValueError(f"항목 {i}: {field} 가 비어 있다")
        if spec["file"] not in files:
            raise ValueError(f"항목 {i}: 경계 파일이 아니다: {spec['file']}")
        key = (spec["file"], spec["function"], spec["data"])
        if key in trusted:
            raise ValueError(f"항목 {i}: 중복 사이트 {key[0]} {key[1]}({key[2]})")
        trusted[key] = spec["reason"]
    return trusted


def evaluate(texts: dict, trusted: dict) -> list:
    reasons = []
    seen = {}
    for rel, text in texts.items():
        try:
            sites = find_sites(text)
        except ValueError as exc:
            reasons.append(f"{rel}: 함수 경계를 해석할 수 없다(fail-closed) — {exc}")
            for key in trusted:
                if key[0] == rel:
                    seen[key] = 1
            continue
        for func, data, line in sites:
            key = (rel, func, data)
            seen[key] = seen.get(key, 0) + 1
            if key not in trusted:
                reasons.append(f"{rel}:{line} {func}() 의 raw json_parser_load_from_data({data}) 가 "
                               f"신뢰 목록에 없다 — pcv_rpc_parse_guarded 경유 또는 근거와 함께 "
                               f"{TRUSTED_FILE.relative_to(ROOT).as_posix()} 등록")
            elif seen[key] == 2:
                reasons.append(f"{rel}:{line} {func}() 가 {data} 를 두 번 이상 raw 파싱한다 — "
                               f"신뢰 항목 하나는 사이트 하나만 면제한다")
    for key in sorted(set(trusted) - set(seen)):
        reasons.append(f"stale 신뢰 항목: {key[0]} {key[1]}({key[2]}) — 그 사이트가 더 이상 없다")
    return reasons


def main() -> int:
    try:
        files = ingress_files()
        texts = {rel: (ROOT / rel).read_text(errors="replace")
                 for rel in files if (ROOT / rel).exists()}
        trusted = load_trusted(json.loads(TRUSTED_FILE.read_text()), files)
    except (OSError, ValueError) as exc:
        print(f"[FAIL] JSON 파싱 신뢰 목록 계약: {exc}", file=sys.stderr)
        return 1
    reasons = evaluate(texts, trusted)
    total = 0
    for text in texts.values():
        try:
            total += len(find_sites(text))
        except ValueError:
            pass
    print(f"[check-json-ingress] 경계 파일 raw 파싱 {total} / 신뢰 목록 {len(trusted)}")
    if reasons:
        print(f"[FAIL] 경계 raw JSON 파싱 계약 위반 {len(reasons)}건:", file=sys.stderr)
        for r in reasons:
            print(f"  - {r}", file=sys.stderr)
        return 1
    print("[PASS] 경계 raw 파싱이 모두 사이트별 신뢰 근거와 일치")
    return 0


if __name__ == "__main__":
    sys.exit(main())
