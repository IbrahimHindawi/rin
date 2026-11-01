"""Run the shadow checker (RIN_SHADOW=1) over sets of programs and tabulate the old verdict against its refusals.

    python tests/shadow_run.py [accept|reject|holes|clean|generated|course|engine]...

RIN_GEN names the folder of generated programs (tests/gen_rin_safe.py); SHOW the number of examples."""
import glob, os, re, subprocess, sys, collections
import pathlib
ROOT = pathlib.Path(__file__).resolve().parent.parent
RIN = str(ROOT / 'build' / 'rin.exe')
SP = os.path.dirname(os.path.abspath(__file__))
T = str(ROOT / 'tests' / 'rin-safety')
show = int(os.environ.get('SHOW', '6'))
def run(path):
    env = dict(os.environ, RIN_SHADOW='1')
    r = subprocess.run([RIN, 'check', path, '--max-errors=1000'], capture_output=True, text=True, errors='replace', env=env, cwd=os.path.dirname(path))
    text = r.stdout + r.stderr
    shadow = [l for l in text.splitlines() if l.startswith('shadow:')]
    old_errors = len(re.findall(r':\d+:\d+: \w+ error:', text))
    parse = any(' parse error:' in l or ' lexer error:' in l for l in text.splitlines())
    return r.returncode, old_errors, parse, shadow
sets = {
 'accept': glob.glob(T + r'\accept\*.rin'),
 'reject': glob.glob(T + r'\reject\*.rin'),
 'holes r13-r20': [f for f in glob.glob(T + r'\attacks\r1[3-9]-*.rin') + glob.glob(T + r'\attacks\r20-*.rin') if 'r18-' not in f],
 'attacks clean by design': [os.path.join(T, 'attacks', n) for n in open(os.path.join(T, 'attacks', 'checks-clean.txt')).read().split()],
 'generated': glob.glob(os.path.join(os.environ.get('RIN_GEN', os.path.join(SP, 'gen')), '*.rin')),
 'course safe': [f for f in glob.glob(str(ROOT.parent / 'rin-dod' / 'ecs' / 'safe' / '**' / '*.rin'), recursive=True) + glob.glob(str(ROOT.parent / 'rin-dod' / 'src' / 'safe' / '**' / '*.rin'), recursive=True) + glob.glob(str(ROOT.parent / 'rin-dod' / 'dojo' / 'safe' / '**' / '*.rin'), recursive=True)],
 'engine': [str(ROOT.parent / 'njinn' / 'src' / 'gin_win32.rin')],
}
only = sys.argv[1:]
for name, files in sets.items():
    if only and not any(o in name for o in only): continue
    files = sorted(files)
    agree_ok = agree_bad = shadow_only = old_only = parse_only = 0
    lines = collections.Counter()
    examples = []
    for f in files:
        rc, old, parse, sh = run(f)
        if parse and not sh: parse_only += 1; continue
        if rc == 0 and not sh: agree_ok += 1
        elif rc != 0 and sh: agree_bad += 1
        elif rc == 0 and sh:
            shadow_only += 1
            for l in sh[:2]: examples.append(os.path.basename(f) + ' | ' + re.sub(r'^shadow: .*?:(\d+):(\d+): ', r'\1:\2: ', l)[:150])
        else: old_only += 1
        for l in sh: lines[re.sub(r"'[^']*'", "'_'", l.split(': ', 2)[-1])[:70]] += 1
    print(f'== {name}: {len(files)} files | both accept {agree_ok} | both refuse {agree_bad} | shadow refuses, old accepts {shadow_only} | old refuses, shadow silent {old_only} | parse-refused {parse_only}')
    for k, v in lines.most_common(8): print(f'   {v:4} {k}')
    for e in examples[:show]: print('   ex ', e)
