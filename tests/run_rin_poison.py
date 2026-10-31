"""Arena poisoning, for docs/regions.md.

    tests/rin-safety/poison/*.rin   built with AddressSanitizer, must die with the error
                                    in their first line, `// expect: <part of the report>`
    tests/rin-safety/accept/*.rin   built with it, must still print their .expected

A poison program checks clean: each reaches freed memory through an `unsafe` block,
which is what poisoning is for. One more build, without the sanitizer, confirms that
RIN_ARENA_SCRIBBLE overwrites what a region gave back.

    python tests/run_rin_poison.py [--cc clang]
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
BUILD = ROOT / "build" / "rin_poison"


def run(cmd: list[str], env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env,
                          errors="replace")


# On Windows a sanitized program needs the runtime DLL from the compiler's resource directory.
def sanitizer_env(cc: str) -> dict[str, str] | None:
    if os.name != "nt":
        return None
    resource = run([cc, "-print-resource-dir"]).stdout.strip()
    env = dict(os.environ)
    env["PATH"] = str(Path(resource) / "lib" / "windows") + os.pathsep + env.get("PATH", "")
    return env


def build(source: Path, cc: str, flags: list[str], tag: str) -> tuple[Path | None, str]:
    BUILD.mkdir(parents=True, exist_ok=True)
    c_path = BUILD / f"{source.stem}.c"
    h_path = BUILD / f"{source.stem}.h"
    exe = BUILD / f"{source.stem}.{tag}{'.exe' if os.name == 'nt' else ''}"
    check = run([str(RIN_EXE), "check", str(source)])
    if check.returncode != 0:
        return None, f"check failed:\n{check.stdout}"
    translate = run([str(RIN_EXE), "compile", str(source), "-o", str(c_path), "--header", str(h_path)])
    if translate.returncode != 0:
        return None, f"compile failed:\n{translate.stdout}"
    defines = ["-D_CRT_SECURE_NO_WARNINGS"] if os.name == "nt" else []
    libs = [] if os.name == "nt" else ["-lm"]
    compiled = run([cc, "-std=c11", "-g", "-Wno-unused", *flags, *defines, str(c_path), "-I", str(ROOT / "src"),
                    "-I", str(ROOT / "src" / "std"), "-o", str(exe), *libs])
    if compiled.returncode != 0:
        return None, f"C compile failed:\n{compiled.stdout}"
    return exe, ""


ASAN = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]


def poisoned(source: Path, cc: str, env: dict[str, str] | None) -> str | None:
    first = source.read_text(encoding="utf-8").splitlines()[0]
    want = first.removeprefix("// expect:").strip()
    exe, why = build(source, cc, ASAN, "asan")
    if exe is None:
        return why
    ran = run([str(exe)], env)
    if ran.returncode == 0:
        return f"ran clean, wanted {want}:\n{ran.stdout}"
    if want not in ran.stdout:
        return f"died, but not with {want}:\n{ran.stdout[:1500]}"
    return None


def still_accepted(source: Path, cc: str, env: dict[str, str] | None) -> str | None:
    exe, why = build(source, cc, ASAN, "asan")
    if exe is None:
        return why
    ran = run([str(exe)], env)
    if ran.returncode != 0:
        return f"exit {ran.returncode}:\n{ran.stdout[:1500]}"
    expected = source.with_suffix(".expected").read_text(encoding="utf-8").replace("\r\n", "\n")
    if ran.stdout.replace("\r\n", "\n") != expected:
        return f"--- expected ---\n{expected}--- actual ---\n{ran.stdout}"
    return None


def scribbled(cc: str) -> str | None:
    source = SUITE / "poison" / "001-local-region-after-its-block.rin"
    exe, why = build(source, cc, ["-DRIN_ARENA_SCRIBBLE"], "scribble")
    if exe is None:
        return why
    ran = run([str(exe)])
    # Four bytes of 0xDD, as an i32.
    if ran.returncode != 0 or ran.stdout.strip() != "-572662307":
        return f"exit {ran.returncode}, wanted the scribble:\n{ran.stdout}"
    return None


# A Release build defines NDEBUG and optimises. The bounds checks must survive both.
def release_keeps_bounds(cc: str) -> str | None:
    source = SUITE / "poison" / "008-index-past-the-end.rin"
    exe, why = build(source, cc, ["-DNDEBUG", "-O2"], "release")
    if exe is None:
        return why
    ran = run([str(exe)])
    if ran.returncode == 0 or "out of bounds" not in ran.stdout:
        return f"exit {ran.returncode}, the check is gone:\n{ran.stdout}"
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="clang")
    args = parser.parse_args()
    env = sanitizer_env(args.cc)
    failures = 0
    total = 0
    for source in sorted((SUITE / "poison").glob("*.rin")):
        problem = poisoned(source, args.cc, env)
        total += 1
        failures += 1 if problem else 0
        print(f"{'FAIL' if problem else 'ok  '} poison/{source.name}")
        if problem:
            print(problem)
    for source in sorted((SUITE / "accept").glob("*.rin")):
        problem = still_accepted(source, args.cc, env)
        total += 1
        failures += 1 if problem else 0
        print(f"{'FAIL' if problem else 'ok  '} accept/{source.name} (asan)")
        if problem:
            print(problem)
    for label, problem in (("RIN_ARENA_SCRIBBLE", scribbled(args.cc)), ("bounds in a Release build", release_keeps_bounds(args.cc))):
        total += 1
        failures += 1 if problem else 0
        print(f"{'FAIL' if problem else 'ok  '} {label}")
        if problem:
            print(problem)
    print(f"{total - failures}/{total} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
