"""Runs programs that may misbehave, in a VM, for docs/regions.md.

    tests/rin-safety/poison/*.rin          controls: must die with the report in their first line
    tests/rin-safety/attacks/*.rin         those in checks-clean.txt: no sanitizer report, no fault,
                                           unless the program is outside the claim (see docs/soundness.md)
    any .rin named on the command line     the same, for a new or quarantined program

By default the VM is a WSL2 distro, with no window: the host checks and translates, and the C goes
in over stdin. The guest has no Windows drives and no way to start a Windows program (see
/etc/wsl.conf there), and 8 GiB of memory with no swap (~/.wslconfig). It builds with its own clang, or gcc with --cc gcc
and runs each program as an unprivileged user with no network, in its own namespaces and cgroup.
Results come back over stdout as data, and only under names the host asked for.

With --sandbox it is a Windows Sandbox instead, which opens a window: the host also compiles, and
the guest has no network, no GPU, no clipboard, 8 GiB, the programs read-only and one results
folder. That is the one that runs them on Windows, as the course and the engine are built.

Either way nothing built runs on the host. A failed control fails the run, so a VM in which
nothing really ran cannot pass as clean.

    python tests/run_rin_vm.py [--sandbox] [--cc clang] [--controls-only] [--timeout 30] [--compare] [program.rin ...]

With --compare each program named on the command line is also built with -O2 and no
sanitizers, and the two must print the same: undefined behaviour shows up as a program that
means one thing unoptimised and another optimised.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import threading
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

WSL_DISTRO = "archlinux"
WSL_RUNNER_UID = 1500
# Four at once, and each cgroup's cap keeps the four inside the VM's 8 GiB.
GUEST_RUNNERS = 4
GUEST_MEMORY = "1750M"

# Runs as root in the guest, with the job already unpacked in its working directory.
GUEST_PY = r"""
import concurrent.futures, json, os, pathlib, shutil, subprocess, sys, threading

job = json.load(open("job.json"))
uid, timeout = job["uid"], job["timeout"]
top = pathlib.Path.cwd()
bins, logs, work = top / "bin", top / "logs", top / "work"
for path in (bins, logs, work):
    path.mkdir()
os.chown(bins, uid, uid)
os.chown(work, uid, uid)
top.chmod(0o755)
lock = threading.Lock()

def say(**fields):
    with lock:
        sys.stdout.write(json.dumps(fields) + "\n")
        sys.stdout.flush()

def compile_one(item):
    stem = item["stem"]
    common = ["-std=c11", "-g", *job["quiet"], "-ffp-contract=off", f"gen/{stem}.c", "-I", "gen", "-I", "src", "-I", "src/std", "-lm"]
    builds = [([*job["asan"], *common], stem)]
    if item["compare"]:
        builds.append((["-O2", *common], f"{stem}__o2"))
    made = []
    for flags, out in builds:
        done = subprocess.run([job["cc"], *flags, "-o", f"bin/{out}"], user=uid, group=uid, text=True, errors="replace",
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if done.returncode != 0:
            say(stage="cc", name=stem, text=done.stdout[:800])
            return []
        made.append(out)
    return made

CHAIN = (
    "mount --make-rprivate / && umount -l /usr/lib/wsl/drivers; umount -l /usr/lib/wsl/lib 2>/dev/null; "
    'exec setpriv --reuid=$0 --regid=$0 --clear-groups --no-new-privs '
    "prlimit --fsize=67108864:67108864 --core=0:0 -- timeout -k 2 $1 $2 x"
)

def run_one(stem):
    home = work / stem
    home.mkdir()
    os.chown(home, uid, uid)
    say(stage="started", name=stem)
    san = f"log_path={home}/san:log_exe_name=0"
    env = {"PATH": "/usr/bin", "ASAN_OPTIONS": f"detect_leaks=0:allocator_may_return_null=1:detect_stack_use_after_return=1:{san}",
           "UBSAN_OPTIONS": f"print_stacktrace=0:{san}"}
    command = ["systemd-run", "--scope", "--quiet", "--collect", f"-pMemoryMax={job['memory']}", "-pMemorySwapMax=0", "-pTasksMax=64",
               "--", "unshare", "-n", "-m", "-i", "-u", "-p", "-f", "--mount-proc", "--kill-child",
               "sh", "-c", CHAIN, str(uid), str(timeout), str(bins / stem)]
    with open(logs / f"{stem}.out", "wb") as out, open(logs / f"{stem}.err", "wb") as err:
        p = subprocess.Popen(command, cwd=home, env=env, stdin=subprocess.DEVNULL, stdout=out, stderr=err)
        try:
            code = p.wait(timeout + 15)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
            code = 124
    code = 128 - code if code < 0 else code
    texts = {}
    for suffix in ("out", "err"):
        with open(logs / f"{stem}.{suffix}", "rb") as f:
            texts[suffix] = f.read(65536).decode("utf-8", "replace")
        (logs / f"{stem}.{suffix}").unlink()
    # The sanitizers' own words, which the program cannot write or hide.
    san_text = ""
    for report in sorted(home.glob("san.*")):
        try:
            with open(report, "rb") as f:
                san_text += f.read(65536).decode("utf-8", "replace")
        except OSError:
            pass
    texts["san"] = san_text[:65536]
    shutil.rmtree(home, ignore_errors=True)
    say(stage="exit", name=stem, code="TIMEOUT" if code == 124 else str(code), **texts)

say(stage="booted")
with concurrent.futures.ThreadPoolExecutor(max(2, (os.cpu_count() or 4) // 2)) as pool:
    made = [out for outs in pool.map(compile_one, job["programs"]) for out in outs]
say(stage="built", count=len(made))
with concurrent.futures.ThreadPoolExecutor(job["runners"]) as pool:
    list(pool.map(run_one, made))
say(stage="done")
"""

# Warnings are noise here, but a wrong type must stay the error it is by default: the emitter
# getting one wrong is a bug to see, and silencing it makes a binary that means nothing.
# gcc takes neither clang's blanket switch nor its names for these.
GCC_QUIET = ["-w", "-Werror=int-conversion", "-Werror=incompatible-pointer-types"]
C_QUIET = ["-Wno-everything", "-Werror=int-conversion", "-Werror=incompatible-pointer-types", "-Wno-error=incompatible-pointer-types-discards-qualifiers",
           "-Werror=incompatible-function-pointer-types", "-Werror=implicit-function-declaration"]
# float-cast-overflow is not in `undefined`, and a float reaching an integer without rin's
# saturating cast is what it would catch.
ASAN = ["-fsanitize=address,undefined,float-cast-overflow", "-fno-sanitize-recover=all"]
# 0xC0000005 access violation, 0xC0000374 heap corruption. Not 0xC0000409: abort() exits with
# it on Windows, and a checked failure is meant to end that way. On Linux the same: abort is 134
# and stays out; 132 illegal instruction, 135 bus error, 136 arithmetic, 139 segmentation fault.
FAULTS = {"C0000005", "C0000374", "132", "135", "136", "139"}
REPORTS_SEEN: list[str] = []  # controls whose report reached the program's stream instead of log_path
# The loader gave up before main: nothing was tested.
DID_NOT_START = {"C0000135", "C0000142", "C000007B", "126", "127"}
# Linux, from outside the program: its cgroup's memory cap killed it, or its output hit the file cap.
OUT_OF_MEMORY = "137"
OUTPUT_CAPPED = "153"
EXITED_CLEANLY = ("00000000", "0")


def run(cmd: list[str], cwd: Path = ROOT) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    env.pop("RIN_UNSAFE", None)
    return subprocess.run(cmd, cwd=cwd, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          errors="replace")


def build(source: Path, job: Path, gen: Path, cc: str | None, compare: bool = False) -> str | None:
    """With no cc, only translate: the guest compiles."""
    c_path = gen / f"{source.stem}.c"
    check = run([str(RIN_EXE), "check", str(source)], cwd=source.parent)
    if check.returncode != 0:
        return f"rejected by rin check, nothing to run:\n{check.stdout[:800]}"
    translate = run([str(RIN_EXE), "compile", str(source), "-o", str(c_path), "--header", str(gen / f"{source.stem}.h")],
                    cwd=source.parent)
    if translate.returncode != 0:
        return f"rin compile failed:\n{translate.stdout[:800]}"
    if cc is None:
        return None
    common =["-std=c11", "-g", *C_QUIET, "-ffp-contract=off", "-D_CRT_SECURE_NO_WARNINGS", str(c_path),
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


def run_in_wsl(stems: list[str], compare: set[str], gen: Path, results: Path, timeout: int, cc: str = "clang") -> tuple[str, list[str]]:
    """Sends the C in and writes what comes back where the sandbox guest would have. The guest is
    what is under test, so its lines are data: only a name we asked for gets a file."""
    expected = set(stems) | {f"{stem}__o2" for stem in compare}
    job = {"uid": WSL_RUNNER_UID, "timeout": timeout, "memory": GUEST_MEMORY, "runners": GUEST_RUNNERS, "asan": ASAN, "quiet": C_QUIET if cc == "clang" else GCC_QUIET, "cc": cc,
           "programs": [{"stem": stem, "compare": stem in compare} for stem in stems]}
    # No shell on the way in: `--exec`, or wsl hands this line to a shell that expands it first.
    script = "rm -rf /var/tmp/rin_vm && mkdir -p /var/tmp/rin_vm && cd /var/tmp/rin_vm && tar -xf - && exec python3 -u guest.py"
    guest = subprocess.Popen(["wsl", "-d", WSL_DISTRO, "-u", "root", "--exec", "sh", "-c", script],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                             creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))

    def send() -> None:
        try:
            with tarfile.open(fileobj=guest.stdin, mode="w|") as tar:
                for name, text in (("guest.py", GUEST_PY), ("job.json", json.dumps(job))):
                    data = text.encode()
                    info = tarfile.TarInfo(name)
                    info.size = len(data)
                    tar.addfile(info, io.BytesIO(data))
                tar.add(gen, "gen")
                tar.add(ROOT / "src" / "std", "src/std")
        except OSError:
            pass  # A guest that died says so by its output ending.
        finally:
            try:
                guest.stdin.close()
            except OSError:
                pass

    threading.Thread(target=send, daemon=True).start()
    deadline = 300 + timeout * (len(expected) + 1)
    timed_out = threading.Event()

    def give_up() -> None:
        timed_out.set()
        subprocess.run(["wsl", "--terminate", WSL_DISTRO], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))

    watchdog = threading.Timer(deadline, give_up)
    watchdog.start()
    uncompiled: list[str] = []
    state = "guest died"
    for raw in guest.stdout:
        try:
            line = json.loads(raw.decode("utf-8", "replace"))
            stage, name = line["stage"], line.get("name")
        except (ValueError, KeyError, TypeError):
            continue
        if stage == "booted":
            (results / "booted").write_text("ok")
        elif stage == "done":
            state = "done"
        elif name not in expected:
            continue
        elif stage == "cc":
            uncompiled.append(name)
            print(f"C compile failed in the guest: {name}\n{str(line.get('text'))[:800]}")
        elif stage == "started":
            (results / f"{name}.started").write_text("ok")
        elif stage == "exit":
            (results / f"{name}.exit").write_text(str(line.get("code"))[:16])
            for suffix in ("out", "err", "san"):
                (results / f"{name}.{suffix}").write_text(str(line.get(suffix, ""))[:65536], encoding="utf-8")
    guest.wait()
    watchdog.cancel()
    return ("timed out" if timed_out.is_set() else state), uncompiled


def code_only(source: str) -> str:
    """The program's code with every string, character literal and comment blanked, by reading it as rin does."""
    out = []
    i, n = 0, len(source)
    while i < n:
        c = source[i]
        two = source[i:i + 2]
        if two == "//":
            while i < n and source[i] != "\n":
                i += 1
            continue
        if two == "/*":
            j = source.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(" " * (source.count("\n", i, j)))
            out.append("\n" * source.count("\n", i, j))
            i = j
            continue
        if c in "\"'":
            quote = c
            j = i + 1
            while j < n and source[j] != quote and source[j] != "\n":
                j += 2 if source[j] == "\\" else 1
            out.append(quote + quote)
            i = min(j + 1, n)
            continue
        out.append(c)
        i += 1
    return "".join(out)


def outside_the_claim(source: str) -> bool:
    """docs/soundness.md excludes what a program takes on itself: code, not a comment saying the word."""
    if re.search(r"^\s*#.*// unsafe", source, re.M):
        return True
    code = code_only(source)
    # A global with no value is C's, whatever it says it is; a region is rin's own. A field is not a global.
    depth = 0
    for line in code.split("\n"):
        if depth == 0 and re.match(r"^\w+\s*:(?!\s*region\b)[^=\n(]*;\s*$", line):
            return True
        depth += line.count("{") - line.count("}")
    return re.search(r"\bunsafe\b|\[\s*external\w*\s*\]|\bcinclude\b", code) is not None


def judge(name: str, results: Path, want: str | None, own_unsafe: bool) -> tuple[str, str]:
    if not (results / f"{name}.started").exists():
        return "NOT RUN", ""
    out = ""
    for suffix in (".out", ".err"):
        path = results / f"{name}{suffix}"
        if path.exists():
            out += path.read_text(encoding="utf-8", errors="replace")
    san_path = results / f"{name}.san"
    san = san_path.read_text(encoding="utf-8", errors="replace") if san_path.exists() else ""
    exit_path = results / f"{name}.exit"
    if not exit_path.exists():
        return "KILLED THE GUEST", out
    code = exit_path.read_text().strip()
    if code in DID_NOT_START:
        return f"DID NOT START {code}", out
    if want is not None:
        if want in san or want in out:
            if want not in san and ("AddressSanitizer" in out or "runtime error" in out):
                REPORTS_SEEN.append(name)  # the report came through the program's stream, not log_path
            return "ok", out + san
        return f"CONTROL FAILED exit {code}", out + san
    # The sanitizer's own report is the verdict; what the program printed cannot forge or mask it.
    if "AddressSanitizer: stack-overflow" in san:
        # The guard page stopped it, as in Rust; nothing past the stack was touched.
        return "stack overflow", san
    if san.strip() or code in FAULTS:
        # An `unsafe` block is where the program took the job over from the checker.
        return ("outside the claim", out + san) if own_unsafe else ("HOLE", out + san)
    if "AddressSanitizer" in out or "runtime error" in out:
        # The words without a report of its own: a program that prints them. The controls prove that a real
        # report reaches the file, so this is not a hole; it is said, so that nobody reads the words as one.
        return "ok (prints sanitizer words)", out
    if code == "TIMEOUT":
        return "timeout", out
    if code == "C00000FD":
        return "stack overflow", out
    if code == OUT_OF_MEMORY:
        return "out of memory", out
    if code == OUTPUT_CAPPED:
        return "output capped", out
    return "ok", out


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("programs", nargs="*", type=Path)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--controls-only", action="store_true")
    parser.add_argument("--timeout", type=int, default=30, help="seconds per program")
    parser.add_argument("--compare", action="store_true", help="also build named programs at -O2 and compare")
    parser.add_argument("--dir", type=Path, help="every .rin in this folder counts as named on the command line")
    parser.add_argument("--sandbox", action="store_true", help="Windows Sandbox instead of WSL2; opens a window")
    args = parser.parse_args()
    if args.dir:
        args.programs += sorted(args.dir.glob("*.rin"))
    if args.sandbox:
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
    host_cc = args.cc if args.sandbox else None
    if not args.sandbox and args.cc not in ("clang", "gcc"):
        print(f"rin-vm: the WSL guest has clang and gcc, not {args.cc}")
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
        for source, problem in zip(wanted, pool.map(lambda s: build(s, job, gen, host_cc, args.compare and s in named), wanted)):
            if problem:
                print(f"skip {source.name}: {problem.splitlines()[0]}")
                # A program named on the command line that the checker rejects is a result, not a failure.
                if source in controls or "rejected" not in problem:
                    failures.append(source.name)
                    print(problem)
            else:
                built[source.stem] = wanted[source]
                own_unsafe[source.stem] = outside_the_claim(source.read_text(encoding="utf-8"))
                if args.compare and source in named:
                    compared.append(source.stem)
    print(f"rin-vm: {'built' if args.sandbox else 'translated'} {len(built)} programs on the host; none of them has run here")

    if args.sandbox:
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
    else:
        refused: list[str] = []
        state, uncompiled = run_in_wsl(sorted(built), set(compared), gen, results, args.timeout, args.cc)
        print(f"rin-vm: guest {state}")
        for name in uncompiled:
            # No binary exists, so nothing ran and nothing went wrong at run time; a control must run.
            if built.get(name) is not None:
                failures.append(name)
            refused.append(name)
            built.pop(name, None)
            compared[:] = [c for c in compared if c != name]
        if state == "guest died":
            failures.append("the guest died")
    if not (results / "booted").exists():
        print("rin-vm: the guest never ran its script; nothing was tested")
        return 1
    if state == "timed out":
        failures.append("the run timed out")

    counts: dict[str, int] = {}
    for name, want in sorted(built.items()):
        verdict, out = judge(name, results, want, own_unsafe[name])
        counts[verdict] = counts.get(verdict, 0) + 1
        if not verdict.startswith("ok"):
            print(f"{verdict}: {name}")
            if verdict not in ("stack overflow", "timeout", "outside the claim", "out of memory", "output capped"):
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
        stopped = base[0] not in EXITED_CLEANLY and opt[0] not in EXITED_CLEANLY
        if base != opt and not stopped:
            differed += 1
            failures.append(name)
            print(f"DIFFERS: {name}: unoptimised exit {base[0]}, -O2 exit {opt[0]}\n    {base[1][:200]!r}\n    {opt[1][:200]!r}")
    if not args.sandbox and refused:
        print(f"rin-vm: {len(refused)} accepted by rin check but refused by the C compiler, so not tested: " + " ".join(sorted(refused)))
    if compared:
        print(f"rin-vm: compared {len(compared)} programs at -O0 and -O2, {differed} differ")
    if REPORTS_SEEN:
        print(f"rin-vm: WARNING: {len(REPORTS_SEEN)} control reports reached the program's stream and not log_path; "
              f"the sanitizer runtime is not writing its file, and holes would be judged from text: {REPORTS_SEEN[:3]}")
        failures.append("sanitizer reports not in log_path")

    print(f"rin-vm: results in {results}")
    if not failures and not args.sandbox:
        # A run's translated C is the bulk of it, and only a failure needs it looked at.
        shutil.rmtree(gen, ignore_errors=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
