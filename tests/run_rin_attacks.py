"""The attack corpus, for docs/regions.md.

    tests/rin-safety/attacks/r1-*.rin   round one: the checker's rules (62 programs)
    tests/rin-safety/attacks/r2-*.rin   round two: the rules added after round one (68)
    tests/rin-safety/attacks/r3-*.rin   round three: std's trusted procs (73)
    tests/rin-safety/attacks/checks-clean.txt
                                        the programs that pass `rin check` on purpose:
                                        correct programs, and ones that end in a
                                        checked run-time abort

Every program here was written to get a pointer past the checker and then
fault. Each must be rejected by `rin check`, unless it is listed in
checks-clean.txt, in which case it must still check clean (a rejection there
is a new false positive). With --run, the clean ones are also compiled with
the sanitizers and run: a sanitizer report is a hole.

Add yours: drop a program in, run this, and if it checks clean and faults,
you have found something. Then it becomes a rule and a reject test.

    python tests/run_rin_attacks.py [--run] [--cc gcc]
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RIN_EXE = ROOT / "build" / "rin.exe"
ATTACKS = ROOT / "tests" / "rin-safety" / "attacks"
BUILD = ROOT / "build" / "rin_attacks"


def run(cmd: list[str], cwd: Path = ROOT) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    env.pop("RIN_UNSAFE", None)
    return subprocess.run(cmd, cwd=cwd, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run", action="store_true", help="also build and run the programs that check clean, under the sanitizers")
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    args = parser.parse_args()
    if not RIN_EXE.exists():
        print(f"rin-attacks: missing compiler {RIN_EXE}")
        return 1
    clean = set((ATTACKS / "checks-clean.txt").read_text().split())
    failures = 0
    programs = sorted(ATTACKS.glob("*.rin"))
    for source in programs:
        out = run([str(RIN_EXE), "check", str(source)], cwd=ATTACKS)
        checked = out.returncode == 0
        if source.name in clean and not checked:
            failures += 1
            print(f"FAIL {source.name}: used to check clean, now rejected (a new false positive?)\n    "
                  + out.stdout.strip().replace("\n", "\n    "))
            continue
        if source.name not in clean and checked:
            failures += 1
            print(f"FAIL {source.name}: checks clean; it was written to be rejected")
            continue
        if checked and args.run:
            BUILD.mkdir(parents=True, exist_ok=True)
            c_path = BUILD / f"{source.stem}.c"
            exe = BUILD / source.stem
            translate = run([str(RIN_EXE), "compile", str(source), "-o", str(c_path), "--header", str(BUILD / f"{source.stem}.h")], cwd=ATTACKS)
            if translate.returncode != 0:
                failures += 1
                print(f"FAIL {source.name}: compile failed\n    " + translate.stdout.strip().replace("\n", "\n    "))
                continue
            compiled = run([args.cc, "-std=c11", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-Wno-unused",
                            str(c_path), "-I", str(ROOT / "src"), "-I", str(ROOT / "src" / "std"), "-o", str(exe), "-lm"])
            if compiled.returncode != 0:
                failures += 1
                print(f"FAIL {source.name}: C compile failed\n    " + compiled.stdout.strip().replace("\n", "\n    ")[:800])
                continue
            ran = run([str(exe), "x"], cwd=ATTACKS)
            if "runtime error" in ran.stdout or "AddressSanitizer" in ran.stdout:
                failures += 1
                print(f"FAIL {source.name}: HOLE, the sanitizer fired on a program that checked clean\n    "
                      + ran.stdout.strip().replace("\n", "\n    ")[:1200])
                continue
        print(f"ok   {source.name}" + ("  (checks clean by design)" if checked else ""))
    print(f"rin-attacks: {len(programs) - failures} of {len(programs)} as expected")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
