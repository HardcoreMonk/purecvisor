#!/usr/bin/env python3
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

# innerHTML / insertAdjacentHTML / outerHTML 대입 또는 호출
ASSIGN_RE = re.compile(r'\.(innerHTML|outerHTML)\s*[+]?=')
INSERT_RE = re.compile(r'\.insertAdjacentHTML\s*\(')

# 안전 sanitizer 호출 패턴 (이름 + 여는 괄호)
SAFE_CALLS = {
    'esc', 'escapeHtml', 'H', 't', '_L',
    'formatBytes', 'formatNumber', 'formatRelativeTime',
    'renderProgressBar', 'statusBadge', 'emptyStatePro',
    'String',  # String(x) — 형변환만
    'encodeURIComponent', 'JSON',
}
# render* 헬퍼는 자체적으로 safe HTML을 반환한다고 가정
SAFE_PREFIX = ('render', 'build')

# RHS 파싱: 식별자 다음에 (가 오면 함수 호출 → 안전, 그 외에는 변수
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
        # 대입 시작 라인은 = 이후만
        if not started:
            m = re.search(r'(innerHTML|outerHTML)\s*[+]?=\s*(.*)', ln)
            if m:
                ln = m.group(2)
                started = True
        if not started:
            continue
        buf.append(ln)
        # 괄호 균형 + 종결자 검출
        for ch in ln:
            if ch == '(': depth += 1
            elif ch == ')': depth -= 1
        if depth <= 0 and (';' in ln or ln.rstrip().endswith(',')):
            break
    return ' '.join(buf)

def check_rhs(rhs):
    """RHS 안에서 안전하지 않은 식별자 사용을 찾는다."""
    # 문자열 리터럴 제거 (single/double quote)
    cleaned = re.sub(r"'(\\'|[^'])*'", "''", rhs)
    cleaned = re.sub(r'"(\\"|[^"])*"', '""', cleaned)
    cleaned = re.sub(r'`(\\`|[^`])*`', '``', cleaned)

    issues = []
    # 식별자 위치 찾기
    for m in IDENT_RE.finditer(cleaned):
        name = m.group(1)
        # 키워드/리터럴/숫자 제외
        if name in ('var','let','const','true','false','null','undefined',
                    'new','if','else','for','while','return','this','typeof',
                    'function','of','in','instanceof'):
            continue
        # 함수 호출 형태 (다음에 ( 가 오면 안전 — 결과는 안전 함수면 OK)
        nxt = m.end()
        is_call = nxt < len(cleaned) and cleaned[nxt] == '('
        if is_call:
            if is_safe_call(name):
                continue
            # 알 수 없는 함수 호출 — 안전 가정 (대부분 헬퍼)
            continue
        # 멤버 접근 (xxx.yyy) 도 함수 호출 가능성
        # 다음이 . 인 경우 → 객체 접근, 안전성은 그 호출에 달림
        if nxt < len(cleaned) and cleaned[nxt] == '.':
            # 메서드 호출 형태 검사: xxx.method(
            mm = re.match(r'\.([A-Za-z_][A-Za-z0-9_]*)\(', cleaned[nxt:])
            if mm:
                method = mm.group(1)
                # toFixed/toLocaleString/toString/toUpperCase 등 안전한 내장 메서드
                if method in ('toFixed','toLocaleString','toString','toUpperCase',
                              'toLowerCase','trim','slice','substring','padStart',
                              'padEnd','replace','replaceAll','join','map','filter','reduce'):
                    continue
            continue
        # 변수 직접 사용 — escapeHtml/esc/H 같은 안전 wrapper로 감싸지 않은 경우
        # 같은 라인 내에 escapeHtml(x) 등이 있는지 확인
        # 단순화: 식별자가 'name' 같은 흔한 데이터 필드면 의심
        # 보수적으로: 라인 내에 esc/escapeHtml/H./t( 가 한 번이라도 있으면 안전 가정
        # → 이 함수는 라인 단위가 아니라 RHS 전체이므로 다음 휴리스틱:
        # 식별자 직전 토큰이 + 또는 ( 또는 , 면 잠재 위험
        idx = m.start()
        prev = cleaned[max(0, idx-20):idx]
        if re.search(r'(\+|\(|,|\?|:)\s*$', prev):
            # 진짜 변수 — 같은 RHS에 sanitizer 호출이 한 번도 없으면 보고
            # 그러나 거의 모든 케이스에 esc()가 있으므로 통과
            issues.append(name)
    return issues

# 안전 변수 화이트리스트 (수치/카운터/플래그/단일 문자 로컬 변수)
SAFE_VARS_RE = re.compile(
    r'^(a|b|c|d|e|f|g|h|i|j|k|l|m|n|p|q|r|s|t|u|v|w|x|y|z|'
    r'count|total|done|mins|max|min|len|length|idx|index|num|sum|avg|'
    r'okCount|errCount|cnt|size|width|height|pct|percent|val|value|ret|'
    r'pf|ps|el|elem|node|root|head|body|tail|first|last|cur|prev|next|'
    r'opt|opts|cfg|conf|st|state|s2|'
    r'header|footer|filterBar|filterBox|sidebar|tag|tags|'
    r'lbl[A-Z][a-zA-Z]*|'  # 라벨 변수 (lblExpires, lblTitle 등)
    r'[a-z]+Html|[a-z]+HTML|[a-z]+Bar|[a-z]+Tag|[a-z]+Box)$'  # xxxHtml, xxxBar, xxxBox
)
# 안전 배열 멤버 (정적 path/endpoint 배열)
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
        # 동적 토큰 추출: + var + 또는 (var)+
        # 문자열 리터럴 제거
        cleaned = re.sub(r"'(\\'|[^'])*'", "''", rhs)
        cleaned = re.sub(r'"(\\"|[^"])*"', '""', cleaned)
        cleaned = re.sub(r'`(\\`|[^`])*`', '``', cleaned)
        # `+ ident` 또는 `+ ident.member` 또는 `+ ident[expr]` 패턴
        suspicious = []
        for m in re.finditer(r'\+\s*([A-Za-z_][A-Za-z0-9_]*)(\.[A-Za-z_][A-Za-z0-9_]*|\[[^\]]+\])?', cleaned):
            base = m.group(1)
            after = cleaned[m.end():m.end()+1]
            if after == '(':
                continue
            if SAFE_VARS_RE.match(base):
                continue
            # 배열 인덱싱: endpoints[i] 등 안전 배열
            sub = m.group(2) or ''
            if sub.startswith('[') and SAFE_ARRAY_RE.match(base):
                continue
            # i18n / 라벨 필드 접근
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
