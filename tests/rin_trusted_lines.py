"""The trusted base that safe code can actually reach: start at the std procs safe code can call, follow
calls by name, and count the lines inside `unsafe { }` and in `proc[unsafe]` bodies that are reached."""
import glob, os, re, sys
STD = sys.argv[1] if len(sys.argv) > 1 else 'C:/devel/rin/src/std'
HEAD = re.compile(r'^(\w+(?:<[^>]*>\w*)?): proc(\[[^\]]*\])?(<[^>]*>)?\((.*?)\)(?: -> (.*?))? = \{')
procs = []
for f in glob.glob(STD + '/*.rin'):
    lines = open(f, encoding='utf8').read().split('\n')
    i = 0
    while i < len(lines):
        m = HEAD.match(lines[i])
        if not m:
            i += 1
            continue
        d = 0
        j = i
        started = False
        in_str = False
        while j < len(lines):
            for c in re.sub(r'"(\\.|[^"\\])*"', '""', lines[j]):
                if c == '{': d += 1; started = True
                elif c == '}': d -= 1
            if started and d == 0:
                break
            j += 1
        body = lines[i:j + 1]
        name = re.sub(r'<[^>]*>', '', m.group(1))
        attrs = m.group(2) or ''
        params = m.group(4)
        text = '\n'.join(body)
        whole_unsafe = attrs and 'unsafe' in attrs
        n = 0
        if whole_unsafe:
            n = len(body)
        else:
            pos = 0
            while True:
                u = re.compile(r'\bunsafe\s*\{').search(text, pos)
                if not u: break
                dd = 1; k = u.end()
                while k < len(text) and dd:
                    if text[k] == '{': dd += 1
                    elif text[k] == '}': dd -= 1
                    k += 1
                n += text[u.end():k].count('\n') + 1
                pos = k
        calls = set(re.sub(r'<[^>]*>', '', c) for c in re.findall(r'\b(\w+(?:<[^<>()]*>\w*)?)\(', text))
        # safe code holds no raw arena and no raw pointer of its own to give
        callable_ = not attrs and '*memops_arena' not in params and '^memops_arena' not in params
        callable_ = callable_ and not re.search(r'\*proc|\^void', params or '')
        procs.append(dict(file=os.path.basename(f), name=name, attrs=attrs, callable=callable_, n=n, calls=calls, generic=m.group(3) or ''))
        i = j + 1
by = {}
for p in procs:
    by.setdefault(p['name'], []).append(p)
seen = set()
work = [p for p in procs if p['callable']]
for p in work: seen.add(id(p))
while work:
    p = work.pop()
    for c in p['calls']:
        for q in by.get(c, []):
            if id(q) not in seen:
                seen.add(id(q)); work.append(q)
tot = sum(p['n'] for p in procs)
reach = [p for p in procs if id(p) in seen]
print('all trusted lines', tot, 'reachable from safe code', sum(p['n'] for p in reach), 'procs', len(procs), 'reached', len(reach))
per = {}
un = {}
for p in procs:
    (per if id(p) in seen else un).setdefault(p['file'], 0)
    (per if id(p) in seen else un)[p['file']] += p['n']
print('reachable:', sorted(per.items(), key=lambda x: -x[1]))
print('not reachable:', sorted(((k, v) for k, v in un.items() if v), key=lambda x: -x[1]))
