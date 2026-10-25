"""Drive rasm_lsp.py over stdio the way an editor does, on examples/2.1.hello_triangle.

    python test_lsp.py            (needs a rin with `asm --index`; set RASM_RIN to pick one)

Each check opens the lesson, asks one question at a position found by searching
the text, and compares a piece of the answer. Prints what failed; exits 1 then.
"""
import json
import os
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
LESSON = HERE.parents[1] / "examples" / "2.1.hello_triangle" / "hello_triangle.rasm"


class Client:
    def __init__(self):
        self.p = subprocess.Popen([sys.executable, "-u", str(HERE / "rasm_lsp.py")],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.next_id = 0
        self.responses = {}
        self.notifications = []
        self.cv = threading.Condition()
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        out = self.p.stdout
        while True:
            length = None
            while True:
                h = out.readline()
                if not h:
                    return
                h = h.strip()
                if not h:
                    break
                if h.lower().startswith(b"content-length:"):
                    length = int(h.split(b":")[1])
            msg = json.loads(out.read(length))
            with self.cv:
                if "id" in msg and "method" not in msg:
                    self.responses[msg["id"]] = msg
                else:
                    self.notifications.append(msg)
                self.cv.notify_all()

    def send(self, msg):
        body = json.dumps(msg).encode()
        self.p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.p.stdin.flush()

    def request(self, method, params):
        self.next_id += 1
        i = self.next_id
        self.send({"jsonrpc": "2.0", "id": i, "method": method, "params": params})
        with self.cv:
            self.cv.wait_for(lambda: i in self.responses, timeout=30)
            return self.responses.get(i, {}).get("result")

    def notify(self, method, params):
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def diagnostics(self, uri, timeout=30):
        end = time.time() + timeout
        with self.cv:
            while time.time() < end:
                for n in reversed(self.notifications):
                    if n.get("method") == "textDocument/publishDiagnostics" and n["params"]["uri"] == uri:
                        return n["params"]["diagnostics"]
                self.cv.wait(0.2)
        return None


def main():
    text = LESSON.read_text(encoding="utf-8")
    lines = text.split("\n")
    uri = LESSON.as_uri()
    c = Client()
    c.request("initialize", {"processId": None, "rootUri": None, "capabilities": {}})
    c.notify("textDocument/didOpen", {"textDocument": {"uri": uri, "languageId": "rasm", "version": 1, "text": text}})
    failures = []

    def pos(needle, offset=0, nth=0):
        """The position of `needle` (its nth occurrence) plus offset characters."""
        count = 0
        for i, l in enumerate(lines):
            start = 0
            while True:
                j = l.find(needle, start)
                if j < 0:
                    break
                if count == nth:
                    return {"line": i, "character": j + offset}
                count += 1
                start = j + 1
        raise SystemExit(f"not in the lesson: {needle}")

    def td(p):
        return {"textDocument": {"uri": uri}, "position": p}

    def hover(needle, offset, expect):
        r = c.request("textDocument/hover", td(pos(needle, offset)))
        got = r["contents"]["value"] if r else ""
        if expect not in got:
            failures.append(f"hover {needle!r}+{offset}: wanted {expect!r}, got {got[:300]!r}")

    diags = c.diagnostics(uri)
    if diags != []:
        failures.append(f"diagnostics on the clean lesson: {diags}")

    hover("    mov buffer_desc.ByteWidth, r8d", 5, "Move")                        # the mnemonic
    hover("mov buffer_desc.ByteWidth, r8d", 18, "offset 0 in `buffer_desc`")    # a field path
    hover("mov buffer_desc.BindFlags, ecx", 18, "dword ptr [rip + buffer_desc + 8]")
    hover("mov buffer_desc.BindFlags, ecx", 29, "ecx")                           # a register
    hover("mov buffer_desc.BindFlags, ecx", 29, "1st integer or pointer argument")
    hover("buffer_desc: D3D11_BUFFER_DESC", 2, "24 bytes")                       # a global and its layout
    hover("swap_desc: DXGI_SWAP_CHAIN_DESC", 13, "OutputWindow")                 # a struct
    hover("#define SCR_WIDTH 1280", 9, "= 1280")
    hover("invoke(AdjustWindowRect, rect&, WINDOW_STYLE, 0)", 34, "0xCA0000")   # a #define's value
    hover("invoke(CreateWindowExA", 8, "[rsp + 32]")                             # an API's registers
    hover("ID3D11DeviceVtbl.CreateBuffer", 20, "[r10 + 24]")                     # a vtable slot
    hover("D3D11_BIND_FLAG.D3D11_BIND_VERTEX_BUFFER", 20, "= 1")                 # an enum member
    hover("    invoke(GetModuleHandleA, 0)", 6, "invoke: macro(f, a1)")         # a macro
    hover("jnz window_created", 6, "label in `main`")
    hover("movss", 2, "SSE") if "movss" in text else None

    r = c.request("textDocument/definition", td(pos("call process_input", 7)))
    if not r or r["range"]["start"]["line"] != pos("process_input: proc")["line"]:
        failures.append(f"definition of process_input: {r}")
    r = c.request("textDocument/definition", td(pos("jnz window_created", 6)))
    if not r or r["range"]["start"]["line"] != pos("    window_created:")["line"]:
        failures.append(f"definition of a label: {r}")

    r = c.request("textDocument/completion", td(pos("mov buffer_desc.ByteWidth", 16)))
    labels = [i["label"] for i in (r or {}).get("items", [])]
    if "ByteWidth" not in labels or "BindFlags" not in labels:
        failures.append(f"field completion: {labels[:10]}")

    r = c.request("textDocument/signatureHelp", td(pos("invoke(CreateWindowExA, 0, ", 27)))
    sig = r["signatures"][0]["label"] if r else ""
    if "rdx lpClassName" not in sig or r.get("activeParameter") != 1:
        failures.append(f"signature help: {r}")

    r = c.request("textDocument/documentSymbol", {"textDocument": {"uri": uri}})
    names = [s["name"] for s in r or []]
    if "main" not in names or "window_proc" not in names:
        failures.append(f"symbols: {names[:12]}")

    # an error typed into the buffer comes back as a diagnostic on its line
    broken = text.replace("mov buffer_desc.ByteWidth, r8d", "mov buffer_desc.ByteWidht, r8d")
    c.notifications.clear()
    c.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": 2}, "contentChanges": [{"text": broken}]})
    diags = c.diagnostics(uri) or []
    want = pos("mov buffer_desc.ByteWidth, r8d")["line"]
    if not any(d["range"]["start"]["line"] == want and "ByteWidht" in d["message"] for d in diags):
        failures.append(f"diagnostic for a typo: {diags}")

    c.request("shutdown", None)
    c.notify("exit", None)
    for f in failures:
        print("FAIL", f)
    print(f"rasm_lsp: {'ok' if not failures else str(len(failures)) + ' failed'}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
