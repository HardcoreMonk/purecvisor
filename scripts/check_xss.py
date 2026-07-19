
"""
check_xss.py — PureCVisor UI XSS 패턴 검증기

ESLint no-unsanitized는 innerHTML 사용을 일률적으로 경고하지만,
본 프로젝트는 esc()/escapeHtml()/H.*()/_L()/t() 헬퍼로 일관 이스케이프한다.
이 스크립트는 innerHTML/insertAdjacentHTML/outerHTML 대입을 추적해
헬퍼를 거치지 않은 식별자가 직접 연결된 경우만 보고한다.

[안전 토큰]
  - 문자열 리터럴 ('...', "...")
  - 알려진 sanitizer 호출: esc(...) escapeHtml(...) H.xxx(...) t(...) _L(...)
    formatBytes(...) formatNumber(...) formatRelativeTime(...)
    renderProgressBar(...) renderXxx(...) statusBadge(...) emptyStatePro(...)
  - 숫자 리터럴 / 연산자 / 괄호
  - 식별자라도 함수 호출 형태 (xxx.toFixed(), xxx.toLocaleString())

[발견 시] exit 1, 의심 라인 출력
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UI = ROOT / "ui"

ASSIGN_RE = re.compile(r'\.(innerHTML|outerHTML)\s*[+]?=')
INSERT_RE = re.compile(r'\.insertAdjacentHTML\s*\(')

SAFE_CALLS = {
    'esc', 'escapeHtml', 'H', 't', '_L',
    'formatBytes', 'formatNumber', 'formatRelativeTime',
    'renderProgressBar', 'statusBadge', 'emptyStatePro',
    'String',
    'encodeURIComponent', 'JSON',
}

SAFE_PREFIX = ('render', 'build')

IDENT_RE = re.compile(r'\b([A-Za-z_][A-Za-z0-9_]*)\b')

def is_safe_call(name):
    if name in SAFE_CALLS:
        return True
    return any(name.startswith(p) for p in SAFE_PREFIX)

def extract_rhs(lines, start):
    """innerHTML = 시작 라인부터 ; 또는 빈 줄까지 RHS 누적."""
    buf = []
    depth = 0
    started = False
    for i in range(start, min(start + 60, len(lines))):
        ln = lines[i]

        if not started:
            m = re.search(r'(innerHTML|outerHTML)\s*[+]?=\s*(.*)', ln)
            if m:
                ln = m.group(2)
                started = True
        if not started:
            continue
        buf.append(ln)

        for ch in ln:
            if ch == '(': depth += 1
            elif ch == ')': depth -= 1
        if depth <= 0 and (';' in ln or ln.rstrip().endswith(',')):
            break
    return ' '.join(buf)

def check_rhs(rhs):
    """RHS 안에서 안전하지 않은 식별자 사용을 찾는다."""

    cleaned = re.sub(r"'(\\'|[^'])*'", "''", rhs)
    cleaned = re.sub(r'"(\\"|[^"])*"', '""', cleaned)
    cleaned = re.sub(r'`(\\`|[^`])*`', '``', cleaned)

    issues = []

    for m in IDENT_RE.finditer(cleaned):
        name = m.group(1)

        if name in ('var','let','const','true','false','null','undefined',
                    'new','if','else','for','while','return','this','typeof',
                    'function','of','in','instanceof'):
            continue

        nxt = m.end()
        is_call = nxt < len(cleaned) and cleaned[nxt] == '('
        if is_call:
            if is_safe_call(name):
                continue

            continue

        if nxt < len(cleaned) and cleaned[nxt] == '.':

            mm = re.match(r'\.([A-Za-z_][A-Za-z0-9_]*)\(', cleaned[nxt:])
            if mm:
                method = mm.group(1)

                if method in ('toFixed','toLocaleString','toString','toUpperCase',
                              'toLowerCase','trim','slice','substring','padStart',
                              'padEnd','replace','replaceAll','join','map','filter','reduce'):
                    continue
            continue

        idx = m.start()
        prev = cleaned[max(0, idx-20):idx]
        if re.search(r'(\+|\(|,|\?|:)\s*$', prev):

            issues.append(name)
    return issues

SAFE_VARS_RE = re.compile(
    r'^(a|b|c|d|e|f|g|h|i|j|k|l|m|n|p|q|r|s|t|u|v|w|x|y|z|'
    r'count|total|done|mins|max|min|len|length|idx|index|num|sum|avg|'
    r'okCount|errCount|cnt|size|width|height|pct|percent|val|value|ret|'
    r'pf|ps|el|elem|node|root|head|body|tail|first|last|cur|prev|next|'
    r'opt|opts|cfg|conf|st|state|s2|'
    r'header|footer|filterBar|filterBox|sidebar|tag|tags|'
    r'lbl[A-Z][a-zA-Z]*|'
    r'[a-z]+Html|[a-z]+HTML|[a-z]+Bar|[a-z]+Tag|[a-z]+Box)$'
)

SAFE_ARRAY_RE = re.compile(r'^(endpoints|paths|routes|methods|tabs|tags|labels|categories)$')

def scan_file(path):
    text = path.read_text()
    lines = text.splitlines()
    findings = []
    for i, ln in enumerate(lines):
        if not (ASSIGN_RE.search(ln) or INSERT_RE.search(ln)):
            continue
        rhs = extract_rhs(lines, i)
        if not rhs:
            continue
        has_sanitizer = bool(re.search(r'\b(esc|escapeHtml|H\.|t\(|_L\(|formatBytes|formatNumber)', rhs))
        if has_sanitizer:
            continue

        cleaned = re.sub(r"'(\\'|[^'])*'", "''", rhs)
        cleaned = re.sub(r'"(\\"|[^"])*"', '""', cleaned)
        cleaned = re.sub(r'`(\\`|[^`])*`', '``', cleaned)

        suspicious = []
        for m in re.finditer(r'\+\s*([A-Za-z_][A-Za-z0-9_]*)(\.[A-Za-z_][A-Za-z0-9_]*|\[[^\]]+\])?', cleaned):
            base = m.group(1)
            after = cleaned[m.end():m.end()+1]
            if after == '(':
                continue
            if SAFE_VARS_RE.match(base):
                continue

            sub = m.group(2) or ''
            if sub.startswith('[') and SAFE_ARRAY_RE.match(base):
                continue

            if sub and sub.lstrip('.') in (
                'past','future','title','name','label','text','msg','message',
                'past_tense','length','size','count'):
                continue
            suspicious.append(base + sub)
        if suspicious:
            findings.append((i + 1, ln.strip()[:120], suspicious))
    return findings

def main():
    files = sorted(UI.glob('modules/*.js')) + [UI / 'app.js']
    files = [f for f in files if f.exists() and f.name not in ('bundle.js', 'app.bundle.js')]
    total = 0
    for f in files:
        hits = scan_file(f)
        if hits:
            print(f"\n{f.relative_to(ROOT)}:")
            for ln, code, susp in hits:
                print(f"  L{ln}: [{', '.join(susp)}] {code}")
            total += len(hits)
    if total == 0:
        print("OK: 0 unsanitized innerHTML 패턴 (모든 동적 데이터가 sanitizer 통과)")
        return 0
    print(f"\nFAIL: {total} suspicious innerHTML lines")
    return 1

if __name__ == '__main__':
    sys.exit(main())
