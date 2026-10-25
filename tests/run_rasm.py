"""rasm: `rin asm` end to end.

Every tests/rasm/*.rasm is assembled and run; its stdout must equal the
.expected beside it, and its PDB must map code to lines of the .rasm (the
reason rasm exists: stepping through the source in a debugger). Every
tests/rasm/errors/*.rasm must fail to assemble with the text in its .error.

Needs clang (the assembler, the data file's compiler and the linker driver);
skips itself when there is none.
"""
from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RIN = ROOT / "build" / "rin.exe"
CASES = ROOT / "tests" / "rasm"
PDBUTIL = Path(r"C:\Program Files\LLVM\bin\llvm-pdbutil.exe")


def main() -> int:
    if not shutil.which("clang"):
        print("rasm: skipped, no clang on PATH")
        return 0
    failures: list[str] = []
    runs = 0
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        for src in sorted(CASES.glob("*.rasm")):
            runs += 1
            exe = work / "build" / (src.stem + ".exe")
            r = subprocess.run([str(RIN), "asm", str(src), "-o", str(exe)], cwd=work,
                               capture_output=True, text=True)
            if r.returncode != 0:
                failures.append(f"{src.name}: rin asm failed\n{r.stdout}{r.stderr}")
                continue
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            expected = src.with_suffix(".expected").read_text(encoding="utf-8")
            if run.stdout != expected:
                failures.append(f"{src.name}: output differs\n--- expected\n{expected}--- got\n{run.stdout}")
                continue
            if PDBUTIL.exists():
                pdb = exe.with_suffix(".pdb")
                lines = subprocess.run([str(PDBUTIL), "dump", "-l", str(pdb)], capture_output=True, text=True)
                if src.name not in lines.stdout.replace("\\", "/"):
                    failures.append(f"{src.name}: the PDB's line table does not name the .rasm")
        # --index, the editor's view: basics.rasm's layout and symbols as the assembler sees them
        runs += 1
        index = work / "index.json"
        src = CASES / "basics.rasm"
        r = subprocess.run([str(RIN), "asm", str(src), "--index", str(index)], cwd=work, capture_output=True, text=True)
        try:
            ix = json.loads(index.read_text(encoding="utf-8"))
            body = next(s for s in ix["structs"] if s["name"] == "body")
            mass = next(f for f in body["fields"] if f["name"] == "mass")
            main_proc = next(p for p in ix["procs"] if p["name"] == "main")
            ok = (r.returncode == 0 and body["size"] == 32 and mass["offset"] == 16
                  and any(lb["name"] == "sum" for lb in main_proc["labels"])
                  and any(m["name"] == "say" and len(m["params"]) == 2 for m in ix["macros"])
                  and any(d["name"] == "COUNT" and d.get("value") == 4 for d in ix["defines"]))
        except (OSError, ValueError, StopIteration, KeyError):
            ok = False
        if not ok:
            failures.append(f"--index: basics.rasm's index is missing or wrong\n{r.stdout}{r.stderr}")
        for src in sorted((CASES / "errors").glob("*.rasm")):
            runs += 1
            r = subprocess.run([str(RIN), "asm", str(src), "--no-link"], cwd=work,
                               capture_output=True, text=True)
            want = src.with_suffix(".error").read_text(encoding="utf-8").strip()
            if r.returncode == 0:
                failures.append(f"errors/{src.name}: assembled, but should have failed with '{want}'")
            elif want not in r.stdout + r.stderr:
                failures.append(f"errors/{src.name}: expected '{want}', got\n{r.stdout}{r.stderr}")
    if failures:
        print("\n".join(failures))
        print(f"rasm: {runs - len(failures)}/{runs} passed")
        return 1
    print(f"rasm: {runs}/{runs} passed (assembled, run, and debuggable at .rasm lines)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
