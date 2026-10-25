"""rasm_lsp: a language server for rasm, the assembler inside the rin compiler.

Everything it knows comes from the assembler itself: on every change it runs

    rin asm <file.rasm> --index <tmp.json> --stdin-path <file.rasm>

with the editor's text on stdin. That checks the program as a build would (its
errors become diagnostics) and writes what the program declares: procs and
their labels, macros, #defines with their values, globals with their types,
every struct with its layout (the Windows and D3D11 ones included), enums,
aliases, and the signatures of the Windows API. The server answers from the
last index:

    hover        an instruction (summary, operand forms, ISA), a register
                 (and its Win64 role), a global or a field path with its type,
                 offset and size, a struct's layout, a #define's value, an
                 enum member's value, a macro's body, a call's registers
    definition   procs, labels, macros, globals, structs, enums, #defines, imports
    completion   mnemonics and macros at the start of a line; fields after `.`;
                 registers, globals, constants and procs elsewhere
    signature    invoke(f, ...) and vcall(obj, Vtbl.Method, ...) show which
                 register or stack slot each argument lands in
    symbols      the procs (with their labels), macros, globals, types of a file

Instruction and register notes come from x86_64.json (gen_x86_docs.py).

    python -u rasm_lsp.py            stdio, for an editor
    python rasm_lsp.py --check f.rasm   print the diagnostics and an index summary

rin is found as $RASM_RIN, then $RIN_HOME/rin.exe, then `rin` on PATH, then
the build/rin.exe of the rin checkout this file is in (rin/rasm/editors/lsp).
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
from pathlib import Path
from urllib.parse import quote, unquote, urlparse

HERE = Path(__file__).resolve().parent
DEBOUNCE_SECONDS = 0.3


# ---------------------------------------------------------------------------
# paths and the compiler
# ---------------------------------------------------------------------------

def find_rin() -> str | None:
    for candidate in (
        os.environ.get("RASM_RIN"),
        os.path.join(os.environ["RIN_HOME"], "rin.exe") if os.environ.get("RIN_HOME") else None,
        shutil.which("rin"),
        str(HERE.parents[2] / "build" / "rin.exe"),
    ):
        if candidate and os.path.isfile(candidate):
            return candidate
    return None


def uri_to_path(uri: str) -> str:
    p = urlparse(uri)
    path = unquote(p.path)
    if os.name == "nt" and re.match(r"^/[A-Za-z]:", path):
        path = path[1:]
    return os.path.normpath(path)


def path_to_uri(path: str) -> str:
    path = os.path.abspath(path).replace("\\", "/")
    if not path.startswith("/"):
        path = "/" + path
    return "file://" + quote(path, safe="/:")


def same_path(a: str, b: str) -> bool:
    return os.path.normcase(os.path.normpath(a)) == os.path.normcase(os.path.normpath(b))


DIAG_RE = re.compile(r"^(?P<path>.+?):(?P<line>\d+):(?P<col>\d+): (?P<kind>[a-z ]*error|warning): (?P<msg>.*)$")


def run_index(rin: str, path: str, text: str) -> tuple[dict | None, list[dict], str]:
    """Index one file. Returns the index (or None), the diagnostics, and any output that was not one."""
    fd, out = tempfile.mkstemp(suffix=".json", prefix="rasm_index_")
    os.close(fd)
    try:
        r = subprocess.run(
            [rin, "asm", path, "--index", out, "--stdin-path", path],
            input=text.encode("utf-8"),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            cwd=os.path.dirname(path) or None,
            timeout=30,
        )
        output = r.stdout.decode("utf-8", errors="replace")
        diags, other = [], []
        for line in output.splitlines():
            m = DIAG_RE.match(line.rstrip())
            if m:
                diags.append({
                    "path": m["path"],
                    "line": int(m["line"]),
                    "col": int(m["col"]),
                    "severity": 2 if m["kind"] == "warning" else 1,
                    "message": m["msg"],
                })
            elif line.strip() and not line.startswith(" ") and not line.startswith("rin asm:"):
                other.append(line)
        index = None
        if os.path.getsize(out) > 0:
            with open(out, encoding="utf-8") as f:
                index = json.load(f)
        return index, diags, "\n".join(other)
    except (OSError, subprocess.SubprocessError, json.JSONDecodeError) as e:
        return None, [], str(e)
    finally:
        try:
            os.remove(out)
        except OSError:
            pass


# ---------------------------------------------------------------------------
# what the assembler said, made searchable
# ---------------------------------------------------------------------------

PRIMITIVE_SIZES = {
    "u8": 1, "i8": 1, "c8": 1, "b8": 1, "char": 1, "bool": 1,
    "u16": 2, "i16": 2, "c16": 2,
    "u32": 4, "i32": 4, "b32": 4, "f32": 4, "c32": 4,
    "u64": 8, "i64": 8, "f64": 8, "usize": 8, "isize": 8,
    "void": 0,
}
SIZE_WORD = {1: "byte", 2: "word", 4: "dword", 8: "qword", 16: "xmmword", 32: "ymmword"}


def split_top(text: str, sep: str = ",") -> list[str]:
    parts, depth, cur = [], 0, ""
    for ch in text:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == sep and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        parts.append(cur)
    return [p.strip() for p in parts]


class Index:
    def __init__(self, data: dict):
        self.data = data
        self.files = data.get("files", [])
        self.procs = {p["name"]: p for p in data.get("procs", [])}
        self.macros: dict[str, list[dict]] = {}
        for m in data.get("macros", []):
            self.macros.setdefault(m["name"], []).append(m)
        for overloads in self.macros.values():
            overloads.sort(key=lambda m: len(m["params"]))
        self.defines = {d["name"]: d for d in data.get("defines", [])}
        self.globals = {g["name"]: g for g in data.get("globals", [])}
        self.structs = {s["name"]: s for s in data.get("structs", [])}
        self.enums = {e["name"]: e for e in data.get("enums", [])}
        self.aliases = {a["name"]: a for a in data.get("aliases", [])}
        self.functions = {f["name"]: f for f in data.get("functions", [])}
        # C enums put their members in the global scope as well
        self.enum_members: dict[str, tuple[dict, dict]] = {}
        for e in data.get("enums", []):
            for m in e.get("members", []):
                self.enum_members.setdefault(m["name"], (e, m))

    # types -----------------------------------------------------------------

    def resolve_alias(self, name: str) -> str:
        seen = 0
        while name in self.aliases and seen < 32:
            nxt = self.aliases[name]["type"].strip()
            if nxt.startswith("const "):
                nxt = nxt[6:]
            if nxt == name:
                break
            name = nxt
            seen += 1
        return name

    def parse_type(self, t: str) -> tuple:
        t = t.strip()
        while t.startswith("const "):
            t = t[6:].strip()
        if t.startswith("[*]") or t.startswith("*"):
            return ("ptr", t[3:] if t.startswith("[*]") else t[1:])
        m = re.match(r"^\[([^\]]*)\](.*)$", t)
        if m:
            return ("array", self.count_of(m.group(1)), m.group(2))
        if t.startswith("proc"):
            return ("proc", t)
        name = self.resolve_alias(t)
        if name != t:
            return self.parse_type(name)
        return ("name", t)

    def count_of(self, text: str) -> int | None:
        text = text.strip()
        if re.fullmatch(r"\d+", text):
            return int(text)
        if re.fullmatch(r"0[xX][0-9a-fA-F]+", text):
            return int(text, 16)
        d = self.defines.get(text)
        return d.get("value") if d else None

    def size_of(self, t: str) -> int | None:
        k = self.parse_type(t)
        if k[0] in ("ptr", "proc"):
            return 8
        if k[0] == "array":
            elem = self.size_of(k[2])
            return elem * k[1] if elem is not None and k[1] is not None else None
        name = k[1]
        if name in PRIMITIVE_SIZES:
            return PRIMITIVE_SIZES[name]
        if name in self.structs:
            return self.structs[name].get("size")
        if name in self.enums:
            return 4
        a = self.aliases.get(name)
        if a and "size" in a:
            return a["size"]
        return None

    def struct_of(self, t: str) -> dict | None:
        k = self.parse_type(t)
        return self.structs.get(k[1]) if k[0] == "name" else None

    def field(self, struct: dict, name: str) -> dict | None:
        for f in struct.get("fields", []):
            if f["name"] == name:
                return f
        return None

    # paths -----------------------------------------------------------------

    def walk(self, root_type: str, steps: list[tuple[str, str]]) -> tuple[str | None, int, str]:
        """Follow fields and constant indexes from a type. -> (type, offset, error)."""
        t, off = root_type, 0
        for kind, value in steps:
            if kind == "index":
                k = self.parse_type(t)
                if k[0] != "array":
                    return None, off, f"not an array: {t}"
                n = self.count_of(value)
                size = self.size_of(k[2])
                if n is None or size is None:
                    return k[2], off, ""
                off += n * size
                t = k[2]
            else:
                s = self.struct_of(t)
                if not s:
                    return None, off, f"{t} has no fields"
                f = self.field(s, value)
                if not f:
                    return None, off, f"{s['name']} has no field '{value}'"
                off += f["offset"]
                t = f["type"]
        return t, off, ""


def parse_steps(text: str) -> tuple[str, list[tuple[str, str]]]:
    """`bodies[1].pos.z` -> ("bodies", [("index", "1"), ("field", "pos"), ("field", "z")])."""
    m = re.match(r"^([A-Za-z_]\w*)", text)
    if not m:
        return "", []
    root, rest, steps = m.group(1), text[m.end():], []
    for sm in re.finditer(r"\.([A-Za-z_]\w*)|\[([^\]]*)\]", rest):
        if sm.group(1):
            steps.append(("field", sm.group(1)))
        else:
            steps.append(("index", sm.group(2).strip()))
    return root, steps


# ---------------------------------------------------------------------------
# instructions and registers
# ---------------------------------------------------------------------------

def load_x86() -> tuple[dict, dict]:
    try:
        with open(HERE / "x86_64.json", encoding="utf-8") as f:
            d = json.load(f)
        return d.get("instructions", {}), d.get("registers", {})
    except (OSError, json.JSONDecodeError):
        return {}, {}


INSTRUCTIONS, REGISTERS = load_x86()

GP64 = ["rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp"] + [f"r{i}" for i in range(8, 16)]
SUBREGS: dict[str, str] = {}
for _r, _subs in {
    "rax": ["eax", "ax", "al", "ah"], "rbx": ["ebx", "bx", "bl", "bh"], "rcx": ["ecx", "cx", "cl", "ch"],
    "rdx": ["edx", "dx", "dl", "dh"], "rsi": ["esi", "si", "sil"], "rdi": ["edi", "di", "dil"],
    "rbp": ["ebp", "bp", "bpl"], "rsp": ["esp", "sp", "spl"],
}.items():
    for _s in _subs:
        SUBREGS[_s] = _r
for _i in range(8, 16):
    for _suffix in ("d", "w", "b"):
        SUBREGS[f"r{_i}{_suffix}"] = f"r{_i}"
ALL_REGISTERS = set(GP64) | set(SUBREGS) | {f"xmm{i}" for i in range(16)} | {f"ymm{i}" for i in range(16)} | {"rip"}

WIN64_ROLE = {
    "rax": "return value; volatile",
    "rcx": "1st integer or pointer argument; volatile",
    "rdx": "2nd integer or pointer argument; volatile",
    "r8": "3rd integer or pointer argument; volatile",
    "r9": "4th integer or pointer argument; volatile",
    "r10": "volatile; vcall loads the vtable through it",
    "r11": "volatile; invoke and vcall move stack arguments through it",
    "rbx": "callee-saved: push it before use, pop it after",
    "rbp": "callee-saved: push it before use, pop it after",
    "rsi": "callee-saved: push it before use, pop it after",
    "rdi": "callee-saved: push it before use, pop it after",
    "rsp": "the stack pointer: 16-aligned at a call, with 32 bytes of shadow space above it for the callee",
    "rip": "the instruction pointer: globals are addressed relative to it, [rip + x]",
}
for _i in range(12, 16):
    WIN64_ROLE[f"r{_i}"] = "callee-saved: push it before use, pop it after"
for _i in range(16):
    if _i < 4:
        WIN64_ROLE[f"xmm{_i}"] = f"float argument {_i + 1} (by position: the {['1st', '2nd', '3rd', '4th'][_i]} argument, whatever the others are)" + ("; float return value" if _i == 0 else "") + "; volatile"
    elif _i < 6:
        WIN64_ROLE[f"xmm{_i}"] = "volatile"
    else:
        WIN64_ROLE[f"xmm{_i}"] = "callee-saved (its low 128 bits): save it before use"

ARG_REGS = ["rcx", "rdx", "r8", "r9"]
FLOAT_TYPES = {"f32", "f64", "FLOAT", "float", "double"}


def arg_slot(i: int, type_text: str = "") -> str:
    if i < 4:
        return f"xmm{i}" if type_text.strip() in FLOAT_TYPES else ARG_REGS[i]
    return f"[rsp + {32 + 8 * (i - 4)}]"


def reference_url(mnemonic: str) -> str:
    m = mnemonic.lower()
    if re.fullmatch(r"j(?!mp$)[a-z]+", m):
        m = "jcc"
    elif m.startswith("cmov"):
        m = "cmovcc"
    elif m.startswith("set") and len(m) <= 6:
        m = "setcc"
    return f"https://www.felixcloutier.com/x86/{m}"


# ---------------------------------------------------------------------------
# reading the line under the cursor
# ---------------------------------------------------------------------------

def strip_comment(line: str) -> str:
    out, q = "", None
    i = 0
    while i < len(line):
        ch = line[i]
        if q:
            out += ch
            if ch == "\\" and i + 1 < len(line):
                out += line[i + 1]
                i += 1
            elif ch == q:
                q = None
        elif ch in "\"'":
            q = ch
            out += ch
        elif ch == ";":
            break
        else:
            out += ch
        i += 1
    return out


PATH_RE = re.compile(r"(\[[^\]]*\])?([A-Za-z_]\w*)((?:\[[^\]]*\]|\.[A-Za-z_]\w*)*)(&?)")


def expression_at(line: str, col: int) -> dict | None:
    """The name or path under the cursor: its text, the part up to the hovered segment, and its kind."""
    code = strip_comment(line)
    for m in PATH_RE.finditer(code):
        if not (m.start() <= col <= m.end()):
            continue
        base = m.group(1)
        if base and col < m.start(2):
            inner = base[1:-1]
            for rm in re.finditer(r"[A-Za-z_]\w*", inner):
                if m.start() + 1 + rm.start() <= col <= m.start() + 1 + rm.end():
                    return {"text": rm.group(0), "upto": rm.group(0), "base": None, "addr": False, "start": m.start()}
            return None
        full = m.group(2) + m.group(3)
        # the hovered segment: cut the path at the end of the segment holding the cursor
        rel = col - m.start(2)
        upto = full
        for sm in re.finditer(r"\.[A-Za-z_]\w*|\[[^\]]*\]", full):
            if sm.start() >= rel:
                upto = full[:sm.start()]
                break
        return {"text": full, "upto": upto, "base": base[1:-1].strip() if base else None,
                "addr": bool(m.group(4)), "start": m.start()}
    return None


def first_word(line: str) -> tuple[str, int] | None:
    m = re.match(r"^\s*([A-Za-z_]\w*)", strip_comment(line))
    return (m.group(1), m.start(1)) if m else None


def is_declaration_line(line: str) -> bool:
    return bool(re.match(r"^\s*[A-Za-z_]\w*\s*:", strip_comment(line)))


# ---------------------------------------------------------------------------
# the server
# ---------------------------------------------------------------------------

class Doc:
    def __init__(self, uri: str, text: str, version: int):
        self.uri = uri
        self.path = uri_to_path(uri)
        self.text = text
        self.version = version
        self.index: Index | None = None
        self.generation = 0

    def line(self, n: int) -> str:
        lines = self.text.split("\n")
        return lines[n].rstrip("\r") if 0 <= n < len(lines) else ""


class Server:
    def __init__(self):
        self.out = sys.stdout.buffer
        self.lock = threading.Lock()
        self.docs: dict[str, Doc] = {}
        self.rin = find_rin()
        self.timers: dict[str, threading.Timer] = {}
        self.diag_uris: dict[str, set[str]] = {}  # doc uri -> uris it published diagnostics to
        self.warned = False

    # transport -------------------------------------------------------------

    def send(self, msg: dict) -> None:
        body = json.dumps(msg).encode("utf-8")
        with self.lock:
            self.out.write(b"Content-Length: %d\r\n\r\n" % len(body))
            self.out.write(body)
            self.out.flush()

    def reply(self, id_, result) -> None:
        self.send({"jsonrpc": "2.0", "id": id_, "result": result})

    def notify(self, method: str, params: dict) -> None:
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def show(self, message: str, kind: int = 2) -> None:
        self.notify("window/showMessage", {"type": kind, "message": message})

    def serve(self) -> None:
        inp = sys.stdin.buffer
        while True:
            length = None
            while True:
                header = inp.readline()
                if not header:
                    return
                header = header.strip()
                if not header:
                    break
                if header.lower().startswith(b"content-length:"):
                    length = int(header.split(b":", 1)[1])
            if length is None:
                continue
            msg = json.loads(inp.read(length).decode("utf-8"))
            try:
                self.handle(msg)
            except Exception as e:  # one bad request must not take the server down
                if "id" in msg:
                    self.send({"jsonrpc": "2.0", "id": msg["id"], "error": {"code": -32603, "message": str(e)}})

    # indexing ----------------------------------------------------------------

    def schedule(self, doc: Doc, delay: float = DEBOUNCE_SECONDS) -> None:
        old = self.timers.pop(doc.uri, None)
        if old:
            old.cancel()
        doc.generation += 1
        gen = doc.generation
        t = threading.Timer(delay, self.reindex, args=(doc, gen))
        t.daemon = True
        self.timers[doc.uri] = t
        t.start()

    def reindex(self, doc: Doc, generation: int) -> None:
        if not self.rin:
            if not self.warned:
                self.warned = True
                self.show("rasm: rin.exe not found (set RASM_RIN or RIN_HOME)")
            return
        data, diags, other = run_index(self.rin, doc.path, doc.text)
        if generation != doc.generation:
            return
        if data is not None:
            doc.index = Index(data)
        elif other and "unknown option --index" in other and not self.warned:
            self.warned = True
            self.show(f"rasm: {self.rin} has no `asm --index`; rebuild or update rin")
        by_uri: dict[str, list] = {doc.uri: []}
        for d in diags:
            uri = doc.uri if same_path(d["path"], doc.path) else path_to_uri(d["path"])
            line, col = max(d["line"] - 1, 0), max(d["col"] - 1, 0)
            by_uri.setdefault(uri, []).append({
                "range": {"start": {"line": line, "character": col}, "end": {"line": line, "character": col + 1}},
                "severity": d["severity"],
                "source": "rasm",
                "message": d["message"],
            })
        for uri in self.diag_uris.get(doc.uri, set()) - set(by_uri):
            self.notify("textDocument/publishDiagnostics", {"uri": uri, "diagnostics": []})
        for uri, items in by_uri.items():
            self.notify("textDocument/publishDiagnostics", {"uri": uri, "diagnostics": items})
        self.diag_uris[doc.uri] = set(by_uri)

    def index_for(self, doc: Doc) -> Index | None:
        if doc.index is None:
            # first request before the first index: build it now
            data, _, _ = run_index(self.rin, doc.path, doc.text) if self.rin else (None, [], "")
            if data is not None:
                doc.index = Index(data)
        return doc.index

    # requests ----------------------------------------------------------------

    def handle(self, msg: dict) -> None:
        method = msg.get("method")
        params = msg.get("params") or {}
        id_ = msg.get("id")
        if method == "initialize":
            self.reply(id_, {
                "capabilities": {
                    "textDocumentSync": {"openClose": True, "change": 1, "save": {"includeText": False}},
                    "hoverProvider": True,
                    "definitionProvider": True,
                    "documentSymbolProvider": True,
                    "completionProvider": {"triggerCharacters": ["."]},
                    "signatureHelpProvider": {"triggerCharacters": ["(", ","]},
                },
                "serverInfo": {"name": "rasm_lsp", "version": "0.1"},
            })
        elif method == "shutdown":
            self.reply(id_, None)
        elif method == "exit":
            sys.exit(0)
        elif method == "textDocument/didOpen":
            td = params["textDocument"]
            doc = Doc(td["uri"], td["text"], td.get("version", 0))
            self.docs[doc.uri] = doc
            self.schedule(doc, 0)
        elif method == "textDocument/didChange":
            doc = self.docs.get(params["textDocument"]["uri"])
            if doc and params.get("contentChanges"):
                doc.text = params["contentChanges"][-1]["text"]
                doc.version = params["textDocument"].get("version", doc.version)
                self.schedule(doc)
        elif method == "textDocument/didSave":
            doc = self.docs.get(params["textDocument"]["uri"])
            if doc:
                self.schedule(doc, 0)
        elif method == "textDocument/didClose":
            doc = self.docs.pop(params["textDocument"]["uri"], None)
            if doc:
                for uri in self.diag_uris.pop(doc.uri, set()):
                    self.notify("textDocument/publishDiagnostics", {"uri": uri, "diagnostics": []})
        elif method == "textDocument/hover":
            self.reply(id_, self.hover(params))
        elif method == "textDocument/definition":
            self.reply(id_, self.definition(params))
        elif method == "textDocument/completion":
            self.reply(id_, self.completion(params))
        elif method == "textDocument/signatureHelp":
            self.reply(id_, self.signature_help(params))
        elif method == "textDocument/documentSymbol":
            self.reply(id_, self.document_symbols(params))
        elif id_ is not None:
            self.reply(id_, None)

    def doc_at(self, params: dict) -> tuple[Doc | None, int, int]:
        doc = self.docs.get(params["textDocument"]["uri"])
        pos = params.get("position", {})
        return doc, pos.get("line", 0), pos.get("character", 0)

    def current_proc(self, ix: Index, doc: Doc, line1: int) -> dict | None:
        for p in ix.procs.values():
            if same_path(p["path"], doc.path) and p["line"] <= line1 <= p.get("end", p["line"]) + 1:
                return p
        return None

    # hover -------------------------------------------------------------------

    def hover(self, params: dict):
        doc, line, col = self.doc_at(params)
        if not doc:
            return None
        text = doc.line(line)
        md = self.describe(doc, text, line, col)
        return {"contents": {"kind": "markdown", "value": md}} if md else None

    def describe(self, doc: Doc, text: str, line: int, col: int) -> str | None:
        ix = self.index_for(doc)
        imp = re.match(r'^\s*(import|cinclude)\s+"([^"]+)"', text)
        if imp:
            return f"`{imp.group(1)} \"{imp.group(2)}\"`"
        fw = first_word(text)
        e = expression_at(text, col)
        if not e:
            return None
        name = e["upto"]
        # the mnemonic: the first word of a line that is not a declaration or a label
        if fw and fw[1] <= col <= fw[1] + len(fw[0]) and not is_declaration_line(text):
            m = fw[0].lower()
            if ix and fw[0] in ix.macros:
                return self.describe_macro(ix, fw[0])
            if m in INSTRUCTIONS:
                return self.describe_instruction(m)
        low = name.lower()
        if low in ALL_REGISTERS and "." not in name:
            return self.describe_register(low)
        if low in INSTRUCTIONS and "." not in name and not ix:
            return self.describe_instruction(low)
        if not ix:
            return None
        if e["base"] is not None:
            return self.describe_based(ix, e)
        return self.describe_name(ix, doc, name, e, line)

    def describe_instruction(self, m: str) -> str:
        info = INSTRUCTIONS[m]
        forms = " · ".join(f"`{f}`" if f else "`(no operands)`" for f in info.get("forms", [])[:12])
        isa = ", ".join(info.get("isa", [])) or "base x86-64"
        return f"**{m}** — {info.get('summary', '')}\n\n{forms}\n\n{isa} · [reference]({reference_url(m)})"

    def describe_register(self, r: str) -> str:
        info = REGISTERS.get(r, {})
        parent = SUBREGS.get(r)
        lines = [f"**{r}** — {info.get('type', 'register')}, {info.get('width', '')}".rstrip(", ")]
        if info.get("description"):
            lines.append(info["description"])
        if parent:
            lines.append(f"Part of **{parent}**." + (" Writing it zeroes the top 32 bits of " + parent + "." if r.startswith("e") or r.endswith("d") else ""))
        role = WIN64_ROLE.get(parent or r)
        if role:
            lines.append(f"Win64: {role}")
        return "\n\n".join(lines)

    def describe_macro(self, ix: Index, name: str) -> str:
        parts = []
        for m in ix.macros[name]:
            body = "\n".join("    " + b for b in m.get("body", [])[:14])
            more = "\n    ..." if len(m.get("body", [])) > 14 else ""
            parts.append(f"```rasm\n{name}: macro({', '.join(m['params'])})\n{body}{more}\n```\n{self.where(m)}")
        return "\n\n".join(parts)

    def where(self, item: dict) -> str:
        p = item.get("path")
        return f"*{os.path.basename(p)}:{item.get('line')}*" if p else ""

    def describe_type(self, ix: Index, t: str) -> str:
        size = ix.size_of(t)
        resolved = ix.parse_type(t)
        note = ""
        if resolved[0] == "name" and resolved[1] != t.replace("const ", "").strip():
            note = f" = {resolved[1]}"
        return f"`{t}`{note}" + (f", {size} byte{'s' if size != 1 else ''}" if size is not None else "")

    def describe_struct(self, ix: Index, s: dict, limit: int = 40) -> str:
        kind = "union" if s.get("union") else "struct"
        head = f"**{s['name']}**: {kind}, {s.get('size', '?')} bytes, aligned {s.get('align', '?')}"
        rows = ["| offset | field | type | size |", "|---:|---|---|---:|"]
        for f in s.get("fields", [])[:limit]:
            rows.append(f"| {f['offset']} | {f['name']} | `{f['type']}` | {f.get('size', '')} |")
        if len(s.get("fields", [])) > limit:
            rows.append(f"| | ... {len(s['fields']) - limit} more | | |")
        return head + "\n\n" + "\n".join(rows) + "\n\n" + self.where(s)

    def describe_function(self, ix: Index, f: dict) -> str:
        params = f.get("params", [])
        sig = f"{f['name']}({', '.join(p['name'] + ': ' + p['type'] for p in params)}) -> {f.get('ret', 'void')}"
        rows = []
        for i, p in enumerate(params):
            rows.append(f"    {arg_slot(i, ix.resolve_alias(p['type'])):<12} {p['name']}: {p['type']}")
        stack = max(0, len(params) - 4) * 8
        frame = f"\n\nNeeds `32 + {stack}` bytes below rsp for the call (shadow space + stack arguments)." if params else ""
        return f"```\n{sig}\n```\n```\n" + "\n".join(rows) + "\n```" + frame + "\n\n" + self.where(f)

    def describe_name(self, ix: Index, doc: Doc, name: str, e: dict, line: int) -> str | None:
        root, steps = parse_steps(name)
        # Enum.Member, Vtbl.Method, Struct.field
        if steps and steps[0][0] == "field":
            member = steps[0][1]
            if root in ix.enums:
                for m in ix.enums[root]["members"]:
                    if m["name"] == member and "value" in m:
                        v = m["value"]
                        return f"**{root}.{member}** = {v}" + (f" (0x{v & 0xFFFFFFFF:X})" if v < 0 or v > 9 else "")
            if root in ix.structs and root not in ix.globals:
                t, off, err = ix.walk(root, steps)
                if err:
                    return err
                note = ""
                if root.endswith("Vtbl"):
                    note = f"\n\nmethod {off // 8} of the vtable: `call qword ptr [r10 + {off}]`"
                return f"**{name}**: {self.describe_type(ix, t)}, at offset {off}{note}"
        if root in ix.globals and not steps:
            g = ix.globals[root]
            out = f"**{root}**: {self.describe_type(ix, g['type'])}"
            if e["addr"]:
                out += f"\n\n`{root}&` is its address, a 64-bit constant: `mov r64, {root}&` assembles to `movabs r64, offset {root}`"
            s = ix.struct_of(g["type"])
            if s:
                out += "\n\n" + self.describe_struct(ix, s, 24)
            else:
                out += "\n\n" + self.where(g)
            return out
        if root in ix.globals:
            g = ix.globals[root]
            t, off, err = ix.walk(g["type"], steps)
            if err:
                return err
            size = ix.size_of(t) if t else None
            operand = f"{SIZE_WORD.get(size, '')} ptr [rip + {root}{' + ' + str(off) if off else ''}]".strip()
            return f"**{name}**: {self.describe_type(ix, t)}\n\noffset {off} in `{root}`: `{operand}`"
        if root in ix.defines and not steps:
            d = ix.defines[root]
            v = d.get("value")
            val = f" = {v}" + (f" (0x{v & 0xFFFFFFFFFFFFFFFF:X})" if v is not None and (v < 0 or v > 9) else "") if v is not None else ""
            return f"`#define {root} {d['text']}`{val}\n\n{self.where(d)}"
        if root in ix.procs and not steps:
            p = ix.procs[root]
            labels = ", ".join(lb["name"] for lb in p.get("labels", [])[:20])
            f = ix.functions.get(root)
            regs = ""
            if f and f.get("params"):
                regs = "\n```\n" + "\n".join(f"    {arg_slot(i, ix.resolve_alias(q['type'])):<12} {q['name']}: {q['type']}" for i, q in enumerate(f["params"])) + "\n```"
            return f"```rasm\n{p['sig'].strip()}\n```{regs}\n\n{self.where(p)}" + (f" · labels: {labels}" if labels else "")
        if root in ix.functions and not steps:
            return self.describe_function(ix, ix.functions[root])
        if root in ix.macros and not steps:
            return self.describe_macro(ix, root)
        if root in ix.structs and not steps:
            return self.describe_struct(ix, ix.structs[root])
        if root in ix.enums and not steps:
            en = ix.enums[root]
            rows = [f"    {m['name']} = {m.get('value', '?')}" for m in en["members"][:30]]
            if len(en["members"]) > 30:
                rows.append(f"    ... {len(en['members']) - 30} more")
            return f"**{root}**: enum\n```\n" + "\n".join(rows) + "\n```\n" + self.where(en)
        if root in ix.aliases and not steps:
            a = ix.aliases[root]
            return f"**{root}** = {self.describe_type(ix, a['type'])}\n\n{self.where(a)}"
        if root in ix.enum_members and not steps:
            en, m = ix.enum_members[root]
            return f"**{root}** = {m.get('value', '?')}, of enum {en['name']}"
        proc = self.current_proc(ix, doc, line + 1)
        if proc:
            for lb in proc.get("labels", []):
                if lb["name"] == root:
                    return f"**{root}**: label in `{proc['name']}`, line {lb['line']}"
        return None

    def describe_based(self, ix: Index, e: dict) -> str | None:
        # [reg].T.f.g: a typed path through a register
        root, steps = parse_steps(e["upto"])
        if root not in ix.structs:
            return None
        t, off, err = ix.walk(root, steps)
        if err:
            return err
        size = ix.size_of(t) if t else None
        operand = f"{SIZE_WORD.get(size, '')} ptr [{e['base']}{' + ' + str(off) if off else ''}]".strip()
        if not steps:
            return self.describe_struct(ix, ix.structs[root], 24)
        return f"**[{e['base']}].{e['upto']}**: {self.describe_type(ix, t)}\n\n`{operand}`"

    # definition -------------------------------------------------------------

    def definition(self, params: dict):
        doc, line, col = self.doc_at(params)
        if not doc:
            return None
        text = doc.line(line)
        imp = re.match(r'^\s*(?:import)\s+"([^"]+)"', text)
        if imp:
            target = os.path.normpath(os.path.join(os.path.dirname(doc.path), imp.group(1)))
            return {"uri": path_to_uri(target), "range": self.range_at(1, 1)}
        ix = self.index_for(doc)
        e = expression_at(text, col)
        if not ix or not e:
            return None
        root, steps = parse_steps(e["upto"])
        fw = first_word(text)
        if fw and fw[1] <= col <= fw[1] + len(fw[0]) and fw[0] in ix.macros:
            return [self.location(m) for m in ix.macros[fw[0]]]
        proc = self.current_proc(ix, doc, line + 1)
        if proc and not steps:
            for lb in proc.get("labels", []):
                if lb["name"] == root:
                    return {"uri": path_to_uri(proc["path"]), "range": self.range_at(lb["line"], lb.get("col", 1))}
        if e["base"] is not None and root in ix.structs:
            return self.location(ix.structs[root])
        if steps and root in ix.globals:
            t, _, _ = ix.walk(ix.globals[root]["type"], steps[:-1])
            s = ix.struct_of(t) if t else None
            if s:
                return self.location(s)
        for table in (ix.procs, ix.globals, ix.defines, ix.structs, ix.enums, ix.aliases, ix.functions):
            if root in table and table[root].get("path"):
                return self.location(table[root])
        if root in ix.macros:
            return [self.location(m) for m in ix.macros[root]]
        if root in ix.enum_members:
            en, m = ix.enum_members[root]
            return {"uri": path_to_uri(en["path"]), "range": self.range_at(m.get("line", en["line"]), 1)}
        return None

    def range_at(self, line1: int, col1: int) -> dict:
        p = {"line": max(line1 - 1, 0), "character": max(col1 - 1, 0)}
        return {"start": p, "end": p}

    def location(self, item: dict) -> dict:
        return {"uri": path_to_uri(item["path"]), "range": self.range_at(item.get("line", 1), item.get("col", 1))}

    # completion -------------------------------------------------------------

    def completion(self, params: dict):
        doc, line, col = self.doc_at(params)
        if not doc:
            return None
        before = strip_comment(doc.line(line)[:col])
        ix = self.index_for(doc)
        items: list[dict] = []

        def add(label: str, kind: int, detail: str = "") -> None:
            items.append({"label": label, "kind": kind, "detail": detail})

        dot = re.search(r"(\[[^\]]*\])?([A-Za-z_]\w*(?:\[[^\]]*\]|\.[A-Za-z_]\w*)*)\.(\w*)$", before)
        if dot and ix:
            path = dot.group(2)
            root, steps = parse_steps(path)
            if root in ix.enums and not steps:
                for m in ix.enums[root]["members"]:
                    add(m["name"], 20, str(m.get("value", "")))
                return {"isIncomplete": False, "items": items}
            t = None
            if dot.group(1) is not None or (root in ix.structs and root not in ix.globals):
                t, _, _ = ix.walk(root, steps)
            elif root in ix.globals:
                t, _, _ = ix.walk(ix.globals[root]["type"], steps)
            s = ix.struct_of(t) if t else None
            if s:
                for f in s.get("fields", []):
                    add(f["name"], 5, f"{f['type']} @ {f['offset']}")
            return {"isIncomplete": False, "items": items}

        at_start = re.fullmatch(r"\s*\w*", before) is not None
        if at_start:
            for m in INSTRUCTIONS:
                add(m, 14, INSTRUCTIONS[m].get("summary", ""))
            if ix:
                for name, overloads in ix.macros.items():
                    add(name, 15, " | ".join(f"({', '.join(o['params'])})" for o in overloads))
            return {"isIncomplete": False, "items": items}

        for r in sorted(ALL_REGISTERS):
            add(r, 6, WIN64_ROLE.get(SUBREGS.get(r, r), ""))
        for w in ("byte", "word", "dword", "qword", "xmmword", "ymmword", "ptr", "sizeof", "offsetof", "cast"):
            add(w, 14)
        if ix:
            proc = self.current_proc(ix, doc, line + 1)
            if proc:
                for lb in proc.get("labels", []):
                    add(lb["name"], 18, "label")
            for g in ix.globals.values():
                add(g["name"], 6, g["type"])
            for p in ix.procs.values():
                add(p["name"], 3, p["sig"].strip())
            for f in ix.functions.values():
                if f["name"] not in ix.procs:
                    add(f["name"], 3, "Windows API")
            for d in ix.defines.values():
                add(d["name"], 21, d["text"])
            for name in ix.structs:
                add(name, 22, "struct")
            for name in ix.enums:
                add(name, 13, "enum")
            for name in ix.macros:
                add(name, 15, "macro")
        return {"isIncomplete": False, "items": items}

    # signature help ---------------------------------------------------------

    def signature_help(self, params: dict):
        doc, line, col = self.doc_at(params)
        if not doc:
            return None
        ix = self.index_for(doc)
        if not ix:
            return None
        before = strip_comment(doc.line(line)[:col])
        # the innermost open call: name( ... with no closing paren
        depth, open_at = 0, -1
        for i in range(len(before) - 1, -1, -1):
            ch = before[i]
            if ch == ")":
                depth += 1
            elif ch == "(":
                if depth == 0:
                    open_at = i
                    break
                depth -= 1
        if open_at < 0:
            return None
        m = re.search(r"([A-Za-z_]\w*)\s*$", before[:open_at])
        if not m:
            return None
        callee = m.group(1)
        args = split_top(before[open_at + 1:] + "\x00")
        active = len(args) - 1
        if callee == "invoke" and args and args[0].strip("\x00") in ix.functions:
            f = ix.functions[args[0].strip("\x00")]
            return self.call_signature(ix, f["name"], f["params"], active - 1, f.get("ret", ""), 0)
        if callee == "vcall" and len(args) >= 2:
            root, steps = parse_steps(args[1].strip("\x00"))
            if root in ix.structs and steps:
                t, _, _ = ix.walk(root, steps)
                if t:
                    ps = self.proc_type_params(t)
                    return self.call_signature(ix, args[1].strip("\x00"), ps, active - 2 + 1, "", 1)
        if callee in ix.macros:
            overloads = ix.macros[callee]
            sigs = [{"label": f"{callee}({', '.join(o['params'])})",
                     "parameters": [{"label": p} for p in o["params"]]} for o in overloads]
            best = next((i for i, o in enumerate(overloads) if len(o["params"]) > active), len(overloads) - 1)
            return {"signatures": sigs, "activeSignature": best, "activeParameter": active}
        if callee in ix.functions:
            f = ix.functions[callee]
            return self.call_signature(ix, callee, f["params"], active, f.get("ret", ""), 0)
        return None

    def proc_type_params(self, t: str) -> list[dict]:
        m = re.search(r"proc\s*\((.*)\)", t)
        if not m:
            return []
        out = []
        for part in split_top(m.group(1)):
            if ":" in part:
                n, ty = part.split(":", 1)
                out.append({"name": n.strip(), "type": ty.strip()})
        return out

    def call_signature(self, ix: Index, name: str, ps: list[dict], active: int, ret: str, first_shown: int) -> dict:
        labels = [f"{arg_slot(i, ix.resolve_alias(p['type']))} {p['name']}: {p['type']}" for i, p in enumerate(ps)]
        shown = labels[first_shown:]
        label = f"{name}(" + ", ".join(shown) + ")" + (f" -> {ret}" if ret else "")
        doc = "this (the object) goes in rcx" if first_shown else ""
        return {"signatures": [{"label": label, "documentation": doc, "parameters": [{"label": s} for s in shown]}],
                "activeSignature": 0, "activeParameter": max(active - first_shown, 0)}

    # symbols -----------------------------------------------------------------

    def document_symbols(self, params: dict):
        doc = self.docs.get(params["textDocument"]["uri"])
        if not doc:
            return []
        ix = self.index_for(doc)
        if not ix:
            return []
        out = []

        def sym(name: str, kind: int, line1: int, end1: int | None = None, children=None, detail: str = "") -> dict:
            r = {"start": {"line": line1 - 1, "character": 0}, "end": {"line": (end1 or line1) - 1, "character": 0}}
            s = {"name": name, "kind": kind, "range": r, "selectionRange": r, "detail": detail}
            if children:
                s["children"] = children
            return s

        mine = lambda item: item.get("path") and same_path(item["path"], doc.path)
        for p in ix.procs.values():
            if mine(p):
                kids = [sym(lb["name"], 20, lb["line"]) for lb in p.get("labels", [])]
                out.append(sym(p["name"], 12, p["line"], p.get("end", p["line"]) + 1, kids, p["sig"].strip()))
        for overloads in ix.macros.values():
            for m in overloads:
                if mine(m):
                    out.append(sym(m["name"], 12, m["line"], None, None, f"macro({', '.join(m['params'])})"))
        for g in ix.globals.values():
            if mine(g):
                out.append(sym(g["name"], 13, g["line"], None, None, g["type"]))
        for s in ix.structs.values():
            if mine(s):
                out.append(sym(s["name"], 23, s["line"], None, None, f"{s.get('size', '?')} bytes"))
        for e in ix.enums.values():
            if mine(e):
                out.append(sym(e["name"], 10, e["line"]))
        for d in ix.defines.values():
            if mine(d):
                out.append(sym(d["name"], 14, d["line"], None, None, d["text"]))
        out.sort(key=lambda s: s["range"]["start"]["line"])
        return out


# ---------------------------------------------------------------------------

def check(paths: list[str]) -> int:
    rin = find_rin()
    if not rin:
        print("rin.exe not found")
        return 2
    status = 0
    for p in paths:
        p = os.path.abspath(p)
        with open(p, encoding="utf-8") as f:
            text = f.read()
        data, diags, other = run_index(rin, p, text)
        for d in diags:
            print(f"{d['path']}:{d['line']}:{d['col']}: {d['message']}")
            status = 1
        if other:
            print(other)
        if data:
            print(f"{os.path.basename(p)}: " + ", ".join(f"{len(v)} {k}" for k, v in data.items()))
    return status


def main() -> None:
    if len(sys.argv) > 2 and sys.argv[1] == "--check":
        sys.exit(check(sys.argv[2:]))
    Server().serve()


if __name__ == "__main__":
    main()
