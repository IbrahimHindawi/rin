"""Shadow refusals with their source lines, grouped by message, for the course and the engine."""
import glob, os, re, subprocess, sys, collections
import pathlib
ROOT = pathlib.Path(__file__).resolve().parent.parent
RIN = str(ROOT / 'build' / 'rin.exe')
files = sys.argv[1:] or (glob.glob(str(ROOT.parent / 'rin-dod' / 'ecs' / 'safe' / '**' / '*.rin'), recursive=True) + glob.glob(str(ROOT.parent / 'rin-dod' / 'src' / 'safe' / '**' / '*.rin'), recursive=True) +
                         glob.glob(str(ROOT.parent / 'rin-dod' / 'dojo' / 'safe' / '**' / '*.rin'), recursive=True) + [str(ROOT.parent / 'njinn' / 'src' / 'gin_win32.rin')])
groups = collections.defaultdict(list)
seen = set()
for f in sorted(files):
    r = subprocess.run([RIN, 'check', f, '--max-errors=1000'], capture_output=True, text=True, errors='replace', env=dict(os.environ, RIN_SHADOW='1'), cwd=os.path.dirname(f))
    for l in (r.stdout + r.stderr).splitlines():
        m = re.match(r'shadow: (.*?):(\d+):(\d+): (.*)', l)
        if not m: continue
        path, line, msg = m.group(1), int(m.group(2)), m.group(4)
        key = (path, line)
        if key in seen: continue
        seen.add(key)
        try:
            src = open(path, encoding='utf8', errors='replace').read().split('\n')[line - 1].strip()
        except Exception:
            src = '?'
        groups[re.sub(r"'[^']*'", "'_'", msg)[:60]].append(f'{os.path.basename(path)}:{line}: {src[:95]}')
for k, v in sorted(groups.items(), key=lambda x: -len(x[1])):
    print(f'== {len(v)}  {k}')
    for e in v[:int(os.environ.get("SHOW", "8"))]: print('   ', e)
