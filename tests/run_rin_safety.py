"""The safety suite, for docs/regions.md.

    tests/rin-safety/accept/*.rin   must check, compile, run, and print their .expected
    tests/rin-safety/reject/*.rin   must fail `rin check` with the message in their
                                    first line, `// expect: <part of the error>`; a
                                    first line of `// always:` marks a rule of the
                                    region blocks themselves, which --unsafe keeps

Both halves matter: the accept programs are what safe code looks like, the
reject programs are the bugs the rules exist to catch. Run with --unsafe to
confirm the reject programs are only rejected by the safety rules: each must
then check clean.

    python tests/run_rin_safety.py [--cc clang] [--bless] [--unsafe]
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RIN_EXE = ROOT / "build" / "rin.exe"
SUITE = ROOT / "tests" / "rin-safety"
BUILD = ROOT / "build" / "rin_safety"


def run(cmd: list[str], cwd: Path = ROOT) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, cwd=cwd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def accept(source: Path, cc: str, bless: bool) -> str | None:
    BUILD.mkdir(parents=True, exist_ok=True)
    c_path = BUILD / f"{source.stem}.c"
    h_path = BUILD / f"{source.stem}.h"
    exe = BUILD / source.stem
    check = run([str(RIN_EXE), "check", str(source)])
    if check.returncode != 0:
        return f"check failed:\n{check.stdout}"
    translate = run([str(RIN_EXE), "compile", str(source), "-o", str(c_path), "--header", str(h_path)])
    if translate.returncode != 0:
        return f"compile failed:\n{translate.stdout}"
    compiled = run([cc, "-std=c11", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused", str(c_path),
                    "-I", str(ROOT / "src"), "-I", str(ROOT / "src" / "std"), "-o", str(exe), "-lm"])
    if compiled.returncode != 0:
        return f"C compile failed:\n{compiled.stdout}"
    ran = run([str(exe)])
    if ran.returncode != 0:
        return f"exit {ran.returncode}:\n{ran.stdout}"
    expected_path = source.with_suffix(".expected")
    if bless:
        expected_path.write_text(ran.stdout, encoding="utf-8", newline="\n")
    if not expected_path.exists():
        return f"missing {expected_path.name}; run with --bless\ngot:\n{ran.stdout}"
    expected = expected_path.read_text(encoding="utf-8").replace("\r\n", "\n")
    if ran.stdout != expected:
        return f"--- expected ---\n{expected}--- actual ---\n{ran.stdout}"
    return None


def reject(source: Path, unsafe: bool) -> str | None:
    first = source.read_text(encoding="utf-8").splitlines()[0]
    always = first.startswith("// always:")
    if not first.startswith("// expect:") and not always:
        return "first line must be '// expect: <part of the error>' or '// always: <part of the error>'"
    want = first.split(":", 1)[1].strip()
    args = [str(RIN_EXE), "check", str(source)]
    if unsafe:
        args.insert(2, "--unsafe")
    out = run(args)
    if unsafe and not always:
        return None if out.returncode == 0 else f"should check clean with --unsafe:\n{out.stdout}"
    if out.returncode == 0:
        return "checked clean; it should have been rejected"
    if want not in out.stdout:
        return f"rejected, but not for the expected reason\nwanted: {want}\ngot:\n{out.stdout}"
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default=os.environ.get("CC", "clang"))
    parser.add_argument("--bless", action="store_true")
    parser.add_argument("--unsafe", action="store_true", help="the reject programs must check clean with --unsafe")
    args = parser.parse_args()
    if not RIN_EXE.exists():
        print(f"rin-safety: missing compiler {RIN_EXE}")
        return 1
    failures = 0
    accepts = sorted((SUITE / "accept").glob("*.rin"))
    rejects = sorted((SUITE / "reject").glob("*.rin"))
    if not args.unsafe:
        for source in accepts:
            problem = accept(source, args.cc, args.bless)
            print(f"{'FAIL' if problem else 'ok  '} accept/{source.name}")
            if problem:
                failures += 1
                print("    " + problem.replace("\n", "\n    "))
    for source in rejects:
        problem = reject(source, args.unsafe)
        print(f"{'FAIL' if problem else 'ok  '} reject/{source.name}")
        if problem:
            failures += 1
            print("    " + problem.replace("\n", "\n    "))
    total = (0 if args.unsafe else len(accepts)) + len(rejects)
    print(f"rin-safety: {total - failures} of {total} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
