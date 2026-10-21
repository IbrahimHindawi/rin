"""Rewrite rin source from the old pointer and logic spellings to the new ones.

    x.&      ->  x&
    x.*      ->  x[0]
    &x       ->  x&        (prefix address-of; the operand chain moves left of it)
    and, or  ->  &&, ||
    1.0f, 10u      ->  1.0, 10       (the type now comes from context)
    10ULL, 10u64   ->  10'u64        (kept explicit: printf's ... has no context)

Comments, string and char literals, and `#` lines are left alone. In a .py file
only string literals that look like rin source are rewritten, so the test
runners' inline programs migrate and their Python does not.

Usage:
    python tools/rin_migrate.py [--check] PATH...

PATH may be a file or a directory (searched for *.rin). --check reports what
would change without writing. Anything the script cannot rewrite safely is
reported as `MANUAL` with a location; it is left as it was.

See docs/operators-and-conversions.md.
"""
from __future__ import annotations

import io
import sys
import tokenize
from pathlib import Path

def literal_suffix_split(text: str) -> tuple[str, str]:
    """Split a number token into its digits and any C suffix after them."""
    if "'" in text:
        return text, ""
    low = text.lower()
    if low.startswith(("0x", "0b")):
        digits = "0123456789abcdef" if low.startswith("0x") else "01"
        i = 2
        while i < len(text) and text[i].lower() in digits:
            i += 1
        return text[:i], text[i:]
    i = 0
    while i < len(text) and (text[i].isdigit() or text[i] == "."):
        i += 1
    if i < len(text) and text[i] in "eE":
        j = i + 1
        if j < len(text) and text[j] in "+-":
            j += 1
        if j < len(text) and text[j].isdigit():
            i = j
            while i < len(text) and text[i].isdigit():
                i += 1
    return text[:i], text[i:]


SUFFIX_REWRITE = {
    "f": "", "u": "",
    "ull": "'u64", "ul": "'u64", "llu": "'u64", "lu": "'u64", "u64": "'u64",
    "ll": "'i64", "l": "'i64", "i64": "'i64",
}


KEYWORDS_BEFORE_OPERAND = {
    "return", "case", "and", "or", "shl", "shr", "else", "do", "if", "while",
    "for", "switch", "goto", "default",
}


class Tok:
    __slots__ = ("kind", "text", "start", "end")

    def __init__(self, kind: str, text: str, start: int, end: int):
        self.kind = kind    # ws, comment, directive, string, char, ident, number, punct
        self.text = text
        self.start = start
        self.end = end


def lex(src: str) -> list[Tok]:
    out: list[Tok] = []
    i = 0
    n = len(src)
    line_start = True
    while i < n:
        c = src[i]
        start = i
        if c in " \t\r\n":
            while i < n and src[i] in " \t\r\n":
                if src[i] == "\n":
                    line_start = True
                i += 1
            out.append(Tok("ws", src[start:i], start, i))
            continue
        if c == "#" and line_start:
            while i < n:
                if src[i] == "\n":
                    if src[i - 1] == "\\":
                        i += 1
                        continue
                    break
                i += 1
            out.append(Tok("directive", src[start:i], start, i))
            continue
        line_start = False
        if src.startswith("//", i):
            while i < n and src[i] != "\n":
                i += 1
            out.append(Tok("comment", src[start:i], start, i))
            continue
        if src.startswith("/*", i):
            end = src.find("*/", i + 2)
            i = n if end < 0 else end + 2
            out.append(Tok("comment", src[start:i], start, i))
            continue
        if c == '"' or c == "'":
            i += 1
            while i < n and src[i] != c and src[i] != "\n":
                i += 2 if src[i] == "\\" else 1
            i = min(i + 1, n)
            out.append(Tok("string" if c == '"' else "char", src[start:i], start, i))
            continue
        if c.isalpha() or c == "_":
            while i < n and (src[i].isalnum() or src[i] == "_"):
                i += 1
            out.append(Tok("ident", src[start:i], start, i))
            continue
        if c.isdigit():
            while i < n and (src[i].isalnum() or src[i] == "_" or
                             (src[i] == "." and i + 1 < n and src[i + 1].isdigit()) or
                             # a signed exponent: 3.4e+38f
                             (src[i] in "+-" and src[i - 1] in "eE" and
                              not src[start:i].lower().startswith("0x") and
                              i + 1 < n and src[i + 1].isdigit())):
                i += 1
            if i < n and src[i] == "'" and i + 1 < n and src[i + 1].isalpha():
                i += 1
                while i < n and (src[i].isalnum() or src[i] == "_"):
                    i += 1
            out.append(Tok("number", src[start:i], start, i))
            continue
        for op in ("...", "->", "&&", "||", "&=", "|=", "==", "!=", "<=", ">=",
                   "+=", "-=", "*=", "/=", "%=", "^="):
            if src.startswith(op, i):
                i += len(op)
                break
        else:
            i += 1
        out.append(Tok("punct", src[start:i], start, i))
    return out


def significant(toks: list[Tok], i: int, step: int) -> int:
    """Index of the next non-whitespace, non-comment token from i in direction step."""
    while 0 <= i < len(toks) and toks[i].kind in ("ws", "comment"):
        i += step
    return i


def prev_significant(toks: list[Tok], i: int, lo: int) -> Tok | None:
    j = significant(toks, i - 1, -1)
    if j < lo:
        return None
    # `T<>` (a reflection record) ends an operand even though `>` does not.
    if toks[j].text == ">" and j - 1 >= lo and toks[j - 1].text == "<":
        return Tok("ident", "<>", toks[j - 1].start, toks[j].end)
    # So does `x.*` or `x.&`, when another postfix step follows it.
    if toks[j].text in ("*", "&") and j - 1 >= lo and toks[j - 1].text == ".":
        return Tok("ident", "." + toks[j].text, toks[j - 1].start, toks[j].end)
    return toks[j]


def ends_operand(t: Tok | None) -> bool:
    if t is None:
        return False
    if t.kind in ("number", "string", "char"):
        return True
    if t.kind == "ident":
        return t.text not in KEYWORDS_BEFORE_OPERAND
    return t.text in (")", "]")


def starts_operand(t: Tok | None) -> bool:
    if t is None:
        return False
    if t.kind in ("ident", "number", "string", "char"):
        return t.text not in ("and", "or")
    return t.text in ("(", "{", "&", "!", "-", "~")


def skip_balanced(toks: list[Tok], i: int) -> int:
    """toks[i] is an opening bracket; return the index just past its match, or -1."""
    pairs = {"(": ")", "[": "]", "{": "}"}
    stack = [pairs[toks[i].text]]
    i += 1
    while i < len(toks):
        t = toks[i]
        if t.kind == "punct":
            if t.text in pairs:
                stack.append(pairs[t.text])
            elif stack and t.text == stack[-1]:
                stack.pop()
                if not stack:
                    return i + 1
        i += 1
    return -1


def operand_chain_end(toks: list[Tok], i: int) -> int:
    """toks[i] begins a prefix-& operand. Return the index just past its postfix
    chain (identifier or parenthesised expression, then .f / [..] / (..) / <>),
    or -1 if it is not a shape this script rewrites."""
    t = toks[i]
    if t.kind == "ident":
        j = i + 1
    elif t.text == "(":
        j = skip_balanced(toks, i)
        if j < 0:
            return -1
    else:
        return -1
    while j < len(toks):
        t = toks[j]
        if t.text == "." and j + 1 < len(toks) and toks[j + 1].kind == "ident":
            j += 2
        elif t.text == "." and j + 1 < len(toks) and toks[j + 1].text in ("&", "*"):
            j += 2
        elif t.text in ("[", "("):
            k = skip_balanced(toks, j)
            if k < 0:
                return -1
            j = k
        elif t.text == "<" and j + 1 < len(toks) and toks[j + 1].text == ">":
            j += 2
        else:
            break
    return j


class Migrator:
    def __init__(self, label: str):
        self.label = label
        self.counts = {".&": 0, ".*": 0, "&x": 0, "and": 0, "or": 0, "suffix": 0}
        self.manual: list[str] = []

    def where(self, src: str, offset: int, base_line: int) -> str:
        line = src.count("\n", 0, offset) + base_line
        return f"{self.label}:{line}"

    def rewrite(self, src: str, base_line: int = 1) -> str:
        toks = lex(src)
        return self.render(toks, 0, len(toks), src, base_line)

    def render(self, toks: list[Tok], lo: int, hi: int, src: str, base_line: int) -> str:
        out: list[str] = []
        i = lo
        while i < hi:
            t = toks[i]
            if t.kind == "number":
                digits, suffix = literal_suffix_split(t.text)
                if suffix and suffix.lower() in SUFFIX_REWRITE:
                    out.append(digits + SUFFIX_REWRITE[suffix.lower()])
                    self.counts["suffix"] += 1
                    i += 1
                    continue
                if suffix:
                    self.manual.append(f"{self.where(src, t.start, base_line)}: unknown literal "
                                       f"suffix '{suffix}' on '{t.text}'")
            if t.kind == "ident" and t.text in ("and", "or"):
                out.append("&&" if t.text == "and" else "||")
                self.counts[t.text] += 1
                i += 1
                continue
            if t.text == "." and i + 1 < hi and toks[i + 1].text in ("&", "*") and \
                    ends_operand(prev_significant(toks, i, lo)):
                if toks[i + 1].text == "*":
                    out.append("[0]")
                    self.counts[".*"] += 1
                else:
                    out.append("&")
                    self.counts[".&"] += 1
                    nxt = significant(toks, i + 2, 1)
                    if nxt < len(toks) and starts_operand(toks[nxt]):
                        self.manual.append(
                            f"{self.where(src, t.start, base_line)}: '.&' is followed by an "
                            f"operand ('{toks[nxt].text}'); 'x& {toks[nxt].text}' would read "
                            f"as bitwise AND -- parenthesise it as '(x&)'")
                i += 2
                continue
            if t.text == "&":
                if not ends_operand(prev_significant(toks, i, lo)):
                    first = significant(toks, i + 1, 1)
                    end = operand_chain_end(toks, first) if first < hi else -1
                    if end < 0 or end > hi:
                        self.manual.append(
                            f"{self.where(src, t.start, base_line)}: prefix '&' before an "
                            f"operand this script does not rewrite")
                        out.append(t.text)
                        i += 1
                        continue
                    inner = self.render(toks, first, end, src, base_line)
                    out.append(inner + "&")
                    self.counts["&x"] += 1
                    nxt = significant(toks, end, 1)
                    if nxt < len(toks) and starts_operand(toks[nxt]):
                        self.manual.append(
                            f"{self.where(src, t.start, base_line)}: rewritten address is "
                            f"followed by an operand ('{toks[nxt].text}') and would read as "
                            f"bitwise AND")
                    i = end
                    continue
            out.append(t.text)
            i += 1
        return "".join(out)


def looks_like_rin(text: str) -> bool:
    return any(m in text for m in (": proc", ":proc", ": struct", ":struct", ": enum"))


def migrate_python(src: str, m: Migrator) -> str:
    """Rewrite rin source held in plain (non-f) string literals of a .py file."""
    lines = src.splitlines(keepends=True)
    offsets = [0]
    for ln in lines:
        offsets.append(offsets[-1] + len(ln))
    edits: list[tuple[int, int, str]] = []
    for tok in tokenize.generate_tokens(io.StringIO(src).readline):
        if tok.type != tokenize.STRING:
            continue
        text = tok.string
        q = 0
        while q < len(text) and text[q] not in "'\"":
            q += 1
        prefix = text[:q].lower()
        if "f" in prefix or "b" in prefix:
            continue
        quote = text[q:q + 3] if text[q:q + 3] in ("'''", '"""') else text[q]
        body = text[q + len(quote):len(text) - len(quote)]
        if not looks_like_rin(body):
            continue
        new_body = m.rewrite(body, base_line=tok.start[0])
        if new_body != body:
            start = offsets[tok.start[0] - 1] + tok.start[1]
            end = offsets[tok.end[0] - 1] + tok.end[1]
            edits.append((start, end, text[:q + len(quote)] + new_body + quote))
    for start, end, new in reversed(edits):
        src = src[:start] + new + src[end:]
    return src


def migrate_file(path: Path, check: bool) -> tuple[Migrator, bool]:
    raw = path.read_bytes().decode("utf-8")
    m = Migrator(str(path))
    new = migrate_python(raw, m) if path.suffix == ".py" else m.rewrite(raw)
    changed = new != raw
    if changed and not check:
        path.write_bytes(new.encode("utf-8"))
    return m, changed


def main(argv: list[str]) -> int:
    check = "--check" in argv
    paths: list[Path] = []
    for a in argv:
        if a == "--check":
            continue
        p = Path(a)
        paths.extend(sorted(p.rglob("*.rin")) if p.is_dir() else [p])
    totals = {".&": 0, ".*": 0, "&x": 0, "and": 0, "or": 0, "suffix": 0}
    manual: list[str] = []
    changed_files = 0
    for p in paths:
        m, changed = migrate_file(p, check)
        changed_files += changed
        for k, v in m.counts.items():
            totals[k] += v
        manual.extend(m.manual)
    for line in manual:
        print("MANUAL", line)
    summary = ", ".join(f"{k} {v}" for k, v in totals.items())
    verb = "would change" if check else "changed"
    print(f"{verb} {changed_files} of {len(paths)} files: {summary}; {len(manual)} manual")
    return 1 if manual else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
