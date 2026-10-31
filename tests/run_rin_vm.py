"""Runs programs that may misbehave, in a throwaway Windows Sandbox, for docs/regions.md.

    tests/rin-safety/poison/*.rin          controls: must die with the report in their first line
    tests/rin-safety/attacks/*.rin         those in checks-clean.txt: no sanitizer report, no fault,
                                           unless the program has an `unsafe` of its own
    any .rin named on the command line     the same, for a new or quarantined program

Everything is built here and nothing built runs here: the host only checks and compiles. The
sandbox has no network, no GPU, no clipboard and 8 GiB of memory, sees the programs read-only,
and can write to one results folder. A failed control fails the run, so a sandbox in which
nothing really ran cannot pass as clean.

    python tests/run_rin_vm.py [--cc clang] [--controls-only] [--timeout 30] [--compare] [program.rin ...]

With --compare each program named on the command line is also built with -O2 and no
sanitizers, and the two must print the same: undefined behaviour shows up as a program that
means one thing unoptimised and another optimised.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RIN_EXE = ROOT / "build" / "rin.exe"
SUITE = ROOT / "tests" / "rin-safety"
WORK = ROOT / "build" / "rin_vm"
SANDBOX_EXE = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "WindowsSandbox.exe"

GUEST_SCRIPT = r"""
$job = 'C:\job'; $res = 'C:\results'; $timeout = @TIMEOUT@ * 1000
New-Item -ItemType Directory -Force C:\work | Out-Null
Set-Content "$res\booted" 'ok'
foreach ($exe in Get-ChildItem "$job\*.exe") {
    $name = $exe.BaseName
    Set-Content "$res\$name.started" 'ok'
    # Into the guest's own disk first: a program printing forever must not fill the host's.
    $p = Start-Process -FilePath $exe.FullName -ArgumentList 'x' -WorkingDirectory C:\work -NoNewWindow -PassThru `
        -RedirectStandardOutput "C:\work\$name.out" -RedirectStandardError "C:\work\$name.err"
    $null = $p.Handle
    if ($p.WaitForExit($timeout)) {
        Set-Content "$res\$name.exit" ('{0:X8}' -f $p.ExitCode)
    } else {
        Stop-Process -Id $p.Id -Force
        Start-Sleep -Milliseconds 200
        Set-Content "$res\$name.exit" 'TIMEOUT'
    }
    foreach ($suffix in '.out', '.err') {
        $from = [IO.File]::OpenRead("C:\work\$name$suffix")
        $buffer = New-Object byte[] 65536
        $count = $from.Read($buffer, 0, 65536)
        $from.Close()
        $to = [IO.File]::Create("$res\$name$suffix")
        $to.Write($buffer, 0, $count)
        $to.Close()
        Remove-Item "C:\work\$name$suffix"
    }
}
Set-Content "$res\DONE" 'ok'
shutdown.exe /s /t 0
"""

WSB = """<Configuration>
  <VGpu>Disable</VGpu>
  <Networking>Disable</Networking>
  <AudioInput>Disable</AudioInput>
  <VideoInput>Disable</VideoInput>
  <PrinterRedirection>Disable</PrinterRedirection>
  <ClipboardRedirection>Disable</ClipboardRedirection>
  <MemoryInMB>8192</MemoryInMB>
  <MappedFolders>
    <MappedFolder><HostFolder>{job}</HostFolder><SandboxFolder>C:\\job</SandboxFolder><ReadOnly>true</ReadOnly></MappedFolder>
    <MappedFolder><HostFolder>{results}</HostFolder><SandboxFolder>C:\\results</SandboxFolder><ReadOnly>false</ReadOnly></MappedFolder>
  </MappedFolders>
  <LogonCommand>
    <Command>powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\\job\\run.ps1</Command>
  </LogonCommand>
</Configuration>
"""

ASAN = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
# 0xC0000005 access violation, 0xC0000374 heap corruption. Not 0xC0000409: abort() exits with
# it on Windows, and a checked failure is meant to end that way.
FAULTS = {"C0000005", "C0000374"}
# The loader gave up before main: nothing was tested.
DID_NOT_START = {"C0000135", "C0000142", "C000007B"}


def run(cmd: list[str], cwd: Path = ROOT) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    env.pop("RIN_UNSAFE", None)
    return subprocess.run(cmd, cwd=cwd, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          errors="replace")


def build(source: Path, job: Path, gen: Path, cc: str, compare: bool = False) -> str | None:
    c_path = gen / f"{source.stem}.c"
    check = run([str(RIN_EXE), "check", str(source)], cwd=source.parent)
    if check.returncode != 0:
        return f"rejected by rin check, nothing to run:\n{check.stdout[:800]}"
    translate = run([str(RIN_EXE), "compile", str(source), "-o", str(c_path), "--header", str(gen / f"{source.stem}.h")],
                    cwd=source.parent)
    if translate.returncode != 0:
        return f"rin compile failed:\n{translate.stdout[:800]}"
    common = ["-std=c11", "-g", "-Wno-everything", "-ffp-contract=off", "-D_CRT_SECURE_NO_WARNINGS", str(c_path),
              "-I", str(ROOT / "src"), "-I", str(ROOT / "src" / "std")]
    compiled = run([cc, *ASAN, *common, "-o", str(job / f"{source.stem}.exe")])
    if compiled.returncode != 0:
        return f"C compile failed:\n{compiled.stdout[:800]}"
    if compare:
        compiled = run([cc, "-O2", *common, "-o", str(job / f"{source.stem}__o2.exe")])
        if compiled.returncode != 0:
            return f"C compile failed at -O2:\n{compiled.stdout[:800]}"
    return None


def copy_runtime(job: Path, cc: str) -> None:
    # A fresh sandbox has neither the sanitizer runtime nor the C++ runtime it loads.
    resource = run([cc, "-print-resource-dir"]).stdout.strip()
    shutil.copy2(Path(resource) / "lib" / "windows" / "clang_rt.asan_dynamic-x86_64.dll", job)
    system32 = SANDBOX_EXE.parent
    for dll in ("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll"):
        shutil.copy2(system32 / dll, job)


def sandbox_running() -> bool:
    # The classic app and the updated one name their processes differently.
    out = run(["tasklist", "/NH"]).stdout
    return "WindowsSandbox" in out or "vmmemSandbox" in out


def wait_for_guest(results: Path, deadline: float) -> str:
    seen_running = False
    while time.time() < deadline:
        if (results / "DONE").exists():
            return "done"
        if sandbox_running():
            seen_running = True
        elif seen_running:
            # Give the results folder a moment before calling it a death.
            time.sleep(3)
            return "done" if (results / "DONE").exists() else "guest died"
        time.sleep(2)
    return "timed out"


def judge(name: str, results: Path, want: str | None, own_unsafe: bool) -> tuple[str, str]:
    if not (results / f"{name}.started").exists():
        return "NOT RUN", ""
    out = ""
    for suffix in (".out", ".err"):
        path = results / f"{name}{suffix}"
        if path.exists():
            out += path.read_text(encoding="utf-8", errors="replace")
    exit_path = results / f"{name}.exit"
    if not exit_path.exists():
        return "KILLED THE GUEST", out
    code = exit_path.read_text().strip()
    if code in DID_NOT_START:
        return f"DID NOT START {code}", out
    if want is not None:
        return ("ok", out) if want in out else (f"CONTROL FAILED exit {code}", out)
    if "AddressSanitizer: stack-overflow" in out:
        # The guard page stopped it, as in Rust; nothing past the stack was touched.
        return "stack overflow", out
    if "AddressSanitizer" in out or "runtime error" in out or code in FAULTS:
        # An `unsafe` block is where the program took the job over from the checker.
        return ("own unsafe", out) if own_unsafe else ("HOLE", out)
    if code == "TIMEOUT":
        return "timeout", out
    if code == "C00000FD":
        return "stack overflow", out
    return "ok", out


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("programs", nargs="*", type=Path)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--controls-only", action="store_true")
    parser.add_argument("--timeout", type=int, default=30, help="seconds per program")
    parser.add_argument("--compare", action="store_true", help="also build named programs at -O2 and compare")
    parser.add_argument("--dir", type=Path, help="every .rin in this folder counts as named on the command line")
    args = parser.parse_args()
    if args.dir:
        args.programs += sorted(args.dir.glob("*.rin"))
    if not SANDBOX_EXE.exists():
        print("rin-vm: Windows Sandbox is not enabled")
        return 1
    # The last run's sandbox can take a while to close.
    for _ in range(30):
        if not sandbox_running():
            break
        time.sleep(2)
    else:
        print("rin-vm: a sandbox is already running; Windows allows one at a time")
        return 1

    # Named by run, so a results folder still locked by the last sandbox cannot get in the way.
    stamp = time.strftime("%Y%m%d-%H%M%S")
    base = WORK / stamp
    job, results, gen = base / "job", base / "results", base / "gen"
    for path in (job, results, gen):
        path.mkdir(parents=True)

    controls = {}
    for source in sorted((SUITE / "poison").glob("*.rin")):
        controls[source] = source.read_text(encoding="utf-8").splitlines()[0].removeprefix("// expect:").strip()
    programs: list[Path] = [p.resolve() for p in args.programs]
    if not args.controls_only:
        clean = (SUITE / "attacks" / "checks-clean.txt").read_text().split()
        programs += [SUITE / "attacks" / name for name in clean]

    wanted = {source: want for source, want in controls.items()}
    wanted.update({source: None for source in programs})
    failures: list[str] = []
    built: dict[str, str | None] = {}
    own_unsafe: dict[str, bool] = {}
    compared: list[str] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(2, (os.cpu_count() or 4) // 2)) as pool:
        named = set(p.resolve() for p in args.programs)
        for source, problem in zip(wanted, pool.map(lambda s: build(s, job, gen, args.cc, args.compare and s in named), wanted)):
            if problem:
                print(f"skip {source.name}: {problem.splitlines()[0]}")
                # A program named on the command line that the checker rejects is a result, not a failure.
                if source in controls or "rejected" not in problem:
                    failures.append(source.name)
                    print(problem)
            else:
                built[source.stem] = wanted[source]
                own_unsafe[source.stem] = "unsafe" in source.read_text(encoding="utf-8")
                if args.compare and source in named:
                    compared.append(source.stem)
    print(f"rin-vm: built {len(built)} programs on the host; none of them has run here")

    copy_runtime(job, args.cc)
    (job / "run.ps1").write_text(GUEST_SCRIPT.replace("@TIMEOUT@", str(args.timeout)), encoding="utf-8")
    wsb = base / "rin.wsb"
    wsb.write_text(WSB.format(job=job, results=results), encoding="utf-8")

    subprocess.Popen([str(SANDBOX_EXE), str(wsb)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # Boot, then each program's limit; most finish in well under a second.
    deadline = time.time() + 300 + args.timeout * (len(built) + len(compared))
    state = wait_for_guest(results, deadline)
    print(f"rin-vm: guest {state}")
    # Windows allows one sandbox at a time, so this one is ours; its host side can outlive the guest.
    time.sleep(5)
    for image in ("WindowsSandboxClient.exe", "WindowsSandbox.exe"):
        run(["taskkill", "/F", "/IM", image])
    if not (results / "booted").exists():
        print("rin-vm: the guest never ran its script; nothing was tested")
        return 1
    if state == "timed out":
        failures.append("the run timed out")

    counts: dict[str, int] = {}
    for name, want in sorted(built.items()):
        verdict, out = judge(name, results, want, own_unsafe[name])
        counts[verdict] = counts.get(verdict, 0) + 1
        if verdict != "ok":
            print(f"{verdict}: {name}")
            if verdict not in ("stack overflow", "timeout", "own unsafe"):
                failures.append(name)
                print("    " + out[:1200].strip().replace("\n", "\n    "))
    print("rin-vm: " + ", ".join(f"{count} {verdict}" for verdict, count in sorted(counts.items())))
    differed = 0
    for name in compared:
        def outcome(stem: str) -> tuple[str, str]:
            exit_path, out_path = results / f"{stem}.exit", results / f"{stem}.out"
            return (exit_path.read_text().strip() if exit_path.exists() else "?",
                    out_path.read_text(errors="replace") if out_path.exists() else "")
        base, opt = outcome(name), outcome(f"{name}__o2")
        # A sanitizer stops with exit 1 where a plain build aborts; both were stopped by a check.
        stopped = base[0] not in ("00000000",) and opt[0] not in ("00000000",)
        if base != opt and not stopped:
            differed += 1
            failures.append(name)
            print(f"DIFFERS: {name}: unoptimised exit {base[0]}, -O2 exit {opt[0]}\n    {base[1][:200]!r}\n    {opt[1][:200]!r}")
    if compared:
        print(f"rin-vm: compared {len(compared)} programs at -O0 and -O2, {differed} differ")
    print(f"rin-vm: results in {results}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
