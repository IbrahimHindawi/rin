"""Writes random programs in rin's safe subset, for docs/regions.md.

Each program has no `unsafe` of its own, stays within moderate memory, and folds what it
computes into a checksum it prints. It leans on the places a safe language tends to break:
extreme integer values, slices replaced during an index, a value and its container passed
together, regions opened inside regions, nullable fields, noalias kernels, containers
growing while they are read.

The oracle is tests/run_rin_vm.py --compare, in a sandbox: a sanitizer report or a fault means
the checker or std let something through, and an unoptimised build that disagrees with an
optimised one means undefined behaviour. A checked stop (out of bounds, division by zero) is
a correct answer, not a finding. Programs the checker refuses are counted, since a generator
that only writes refused programs would test nothing.

    python tests/gen_rin_safe.py --out DIR [--count 200] [--seed 1]
"""
from __future__ import annotations

import argparse
import random
from pathlib import Path

INTS = {
    "i8": ["0", "1", "-1", "127", "-128", "100"],
    "i16": ["0", "1", "-1", "32767", "-32768", "256"],
    "i32": ["0", "1", "-1", "2147483647", "-2147483647", "65536", "46341"],
    "i64": ["0", "1", "-1", "9223372036854775807", "-9223372036854775807", "4294967296"],
    "u8": ["0", "1", "255", "128"],
    "u16": ["0", "1", "65535", "32768"],
    "u32": ["0", "1", "4294967295", "2147483648"],
    "u64": ["0", "1", "9223372036854775807", "4294967296"],
}
FLOATS = {"f32": ["0.0", "1.5", "-2.5", "1000000.0"], "f64": ["0.0", "1.5", "-2.5", "1e300", "-1e300"]}


class Gen:
    def __init__(self, seed: int):
        self.r = random.Random(seed)
        self.n = 0
        self.helpers: list[str] = []
        self.int_helpers: list[tuple[str, str]] = []
        self.noalias: list[str] = []

    def name(self, stem: str) -> str:
        self.n += 1
        return f"{stem}{self.n}"

    def int_expr(self, env: dict, t: str, d: int = 0) -> str:
        r = self.r
        vars_of = env["ints"].get(t, [])
        c = r.random()
        if d > 3 or c < 0.2:
            if vars_of and r.random() < 0.7:
                return r.choice(vars_of)
            return f"cast({r.choice(INTS[t])}, {t})"
        if c < 0.55:
            op = r.choice(["+", "-", "*", "&", "|", "^", "+", "-", "*"])
            return f"({self.int_expr(env, t, d + 1)} {op} {self.int_expr(env, t, d + 1)})"
        if c < 0.63:
            # Almost never zero: a stop ends the program, and most of it should run.
            op = r.choice(["/", "%"])
            guard = "" if r.random() < 0.03 else f" | cast(1, {t})"
            return f"({self.int_expr(env, t, d + 1)} {op} ({self.int_expr(env, t, d + 1)}{guard}))"
        if c < 0.7:
            op = r.choice(["shl", "shr"])
            return f"({self.int_expr(env, t, d + 1)} {op} {self.int_expr(env, 'i32', d + 2)})"
        if c < 0.78:
            other = r.choice(list(INTS))
            return f"cast({self.int_expr(env, other, d + 1)}, {t})"
        if c < 0.84:
            return f"cast({self.float_expr(env, r.choice(list(FLOATS)), d + 1)}, {t})"
        if c < 0.88 and t[0] == "i" and t in ("i32", "i64"):
            return f"(-{self.int_expr(env, t, d + 1)})"
        if c < 0.94 and env["slices"] and t == "i32":
            s = r.choice(env["slices"])
            return f"{s}[{self.index(env, s)}]"
        if env["int_calls"] and t == "i32":
            f = r.choice(env["int_calls"])
            return f"{f}({self.int_expr(env, 'i32', d + 2)}, {self.int_expr(env, 'i32', d + 2)})"
        return f"cast({r.choice(INTS[t])}, {t})"

    def index(self, env: dict, s: str) -> str:
        e = self.int_expr(env, "u64", 3)
        # Now and then a raw index, which a bounds check must stop.
        if self.r.random() < 0.02:
            return e
        return f"{e} % {s}.length"

    def float_expr(self, env: dict, t: str, d: int = 0) -> str:
        r = self.r
        vars_of = env["floats"].get(t, [])
        c = r.random()
        if d > 3 or c < 0.35:
            if vars_of and r.random() < 0.6:
                return r.choice(vars_of)
            return f"cast({r.choice(FLOATS[t])}, {t})"
        if c < 0.75:
            return f"({self.float_expr(env, t, d + 1)} {r.choice(['+', '-', '*'])} {self.float_expr(env, t, d + 1)})"
        return f"cast({self.int_expr(env, r.choice(list(INTS)), d + 1)}, {t})"

    def mix(self, expr: str) -> str:
        return f"mix(cast({expr}, u64));"

    def block(self, env: dict, depth: int, count: int) -> list[str]:
        out: list[str] = []
        local = {
            "ints": {k: list(v) for k, v in env["ints"].items()},
            "floats": {k: list(v) for k, v in env["floats"].items()},
            "slices": list(env["slices"]),
            "int_calls": env["int_calls"],
            "boxes": list(env["boxes"]),
            "nodes": list(env["nodes"]),
            "vecs": list(env["vecs"]),
            "region": env["region"],
            "main": env["main"],
        }
        for _ in range(count):
            out += self.stmt(local, depth)
        return out

    def stmt(self, env: dict, depth: int) -> list[str]:
        r = self.r
        c = r.random()
        region = env["region"]
        if c < 0.14:
            t = r.choice(list(INTS))
            n = self.name("v")
            line = f"{n}: {t} = {self.int_expr(env, t)};"
            env["ints"].setdefault(t, []).append(n)
            return [line]
        if c < 0.2:
            t = r.choice(list(FLOATS))
            n = self.name("x")
            line = f"{n}: {t} = {self.float_expr(env, t)};"
            env["floats"].setdefault(t, []).append(n)
            return [line]
        if c < 0.34:
            t = r.choice(list(INTS))
            if not env["ints"].get(t):
                return [self.mix(self.int_expr(env, t))]
            v = r.choice(env["ints"][t])
            op = r.choice(["=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "shl=", "shr="])
            if op in ("shl=", "shr="):
                rhs = self.int_expr(env, "i32", 2)
            elif op in ("/=", "%="):
                rhs = f"({self.int_expr(env, t, 2)} | cast(1, {t}))"
            else:
                rhs = self.int_expr(env, t, 1)
            return [f"{v} {op} {rhs};", self.mix(v)]
        if c < 0.42:
            return [self.mix(self.int_expr(env, r.choice(list(INTS))))]
        if c < 0.48 and depth < 3:
            return [f"if ({self.int_expr(env, 'i32', 2)} < {self.int_expr(env, 'i32', 2)}) {{"] + \
                self.block(env, depth + 1, 2) + ["} else {"] + self.block(env, depth + 1, 2) + ["}"]
        if c < 0.55 and depth < 2:
            i = self.name("idx")
            k = r.randint(1, 5)
            inner = dict(env)
            inner["ints"] = {t: list(v) for t, v in env["ints"].items()}
            inner["ints"].setdefault("i32", [])
            return [f"for ({i}: i32 = 0; {i} < {k}; {i} += 1) {{"] + self.block(inner, depth + 1, 2) + ["}"]
        if c < 0.62:
            s = self.name("s")
            n = r.randint(1, 64)
            lines = [f"{s}: []i32 in {region} = new_slice<i32>({region}, {n});"]
            env["slices"].append(s)
            return lines
        if c < 0.68 and env["slices"]:
            s = r.choice(env["slices"])
            return [f"{s}[{self.index(env, s)}] = {self.int_expr(env, 'i32', 1)};"]
        if c < 0.73 and env["slices"]:
            s = r.choice(env["slices"])
            a, b = self.name("a"), self.name("b")
            sub = self.name("s")
            lines = [
                f"{a}: u64 = {self.int_expr(env, 'u64', 3)} % ({s}.length + 1);",
                f"{b}: u64 = {a} + {self.int_expr(env, 'u64', 3)} % ({s}.length + 1 - {a});",
                f"{sub}: []i32 in {region} = {s}[{a}..{b}];",
                self.mix(f"{sub}.length"),
            ]
            return lines
        if c < 0.79 and env["main"] and depth < 3:
            # A region inside a region: what is made in it stays in it.
            t = self.name("tmp")
            inner = dict(env)
            inner["region"] = t
            inner["slices"] = []
            inner["boxes"] = []
            inner["nodes"] = []
            inner["vecs"] = []
            inner["ints"] = {k: list(v) for k, v in env["ints"].items()}
            inner["floats"] = {k: list(v) for k, v in env["floats"].items()}
            return [f"{t}: region of {region} = {{"] + self.block(inner, depth + 1, 4) + ["}"]
        if c < 0.84 and region == "level":
            b = self.name("box")
            lines = [f"{b}: *box in level = make_box(level, {r.randint(1, 32)});"]
            env["boxes"].append(b)
            return lines
        if c < 0.89 and env["boxes"]:
            b = r.choice(env["boxes"])
            kind = r.random()
            if kind < 0.5:
                # A call beside the index replaces the slice being indexed.
                return [f"{b}*.xs[swap_box(level, {b}, {r.randint(0, 40)})] = {self.int_expr(env, 'i32', 2)};"]
            if kind < 0.8:
                return [self.mix(f"{b}*.xs[swap_box(level, {b}, {r.randint(0, 40)})]")]
            return [self.mix(f"sum_box({b}, {b})")]
        if c < 0.93 and region == "level":
            if env["nodes"] and r.random() < 0.6:
                n = r.choice(env["nodes"])
                other = r.choice(env["nodes"])
                kind = r.randint(0, 3)
                if kind == 0:
                    return [f"{n}*.next = {other};"]
                if kind == 1:
                    return [f"{n}*.next = null;"]
                if kind == 2:
                    return [f"if ({n}*.next) {{ {self.mix(f'{n}*.next*.v')} }}"]
                return [self.mix(f"walk({n})")]
            n = self.name("n")
            env["nodes"].append(n)
            return [f"{n}: *node in level = new<node>(level);", f"{n}*.v = {self.int_expr(env, 'i32', 2)};"]
        if c < 0.97 and region == "level":
            if env["vecs"] and r.random() < 0.8:
                v = r.choice(env["vecs"])
                kind = r.randint(0, 3)
                if kind == 0:
                    return [f"_ = vec<i32>push({v}, {self.int_expr(env, 'i32', 2)});"]
                if kind == 1:
                    return [f"if ({v}*.length > 0) {{ {self.mix(f'vec<i32>at({v}, {self.int_expr(env, chr(117) + chr(54) + chr(52), 3)} % {v}*.length)*')} }}"]
                if kind == 2:
                    return [f"grow_while_reading({v});"]
                return [self.mix(f"{v}*.length")]
            v = self.name("vs")
            env["vecs"].append(v)
            return [f"{v}: *vec<i32> in level = vec<i32>make(level, {r.randint(0, 8)});"]
        if self.noalias and region == "level":
            f = r.choice(self.noalias)
            a, b = self.name("na"), self.name("nb")
            n = r.randint(1, 32)
            # Now and then the same slice under another name, which the check on entry must stop.
            second = f"{a}[0..{n}]" if r.random() < 0.02 else b
            return [
                f"{a}: []f32 in level = new_slice<f32>(level, {n});",
                f"{b}: []f32 in level = new_slice<f32>(level, {n});",
                f"{f}({a}, {second}, {self.float_expr(env, 'f32', 2)});",
                self.mix(f"{a}[0]"),
            ]
        return [self.mix(self.int_expr(env, "u32"))]

    def program(self) -> str:
        r = self.r
        empty = {"ints": {}, "floats": {}, "slices": [], "int_calls": [], "boxes": [], "nodes": [], "vecs": [],
                 "region": "level", "main": False}
        for _ in range(r.randint(1, 4)):
            f = self.name("fn")
            env = dict(empty)
            env["ints"] = {"i32": ["a", "b"]}
            env["floats"] = {}
            env["slices"] = []
            env["int_calls"] = [h for h, _ in self.int_helpers]
            body = [self.mix(self.int_expr(env, "i32", 1)), f"return {self.int_expr(env, 'i32', 1)};"]
            self.helpers.append(f"{f}: proc(a: i32, b: i32) -> i32 = {{\n    " + "\n    ".join(body) + "\n}\n")
            self.int_helpers.append((f, ""))
        for _ in range(r.randint(0, 2)):
            f = self.name("kernel")
            op = r.choice(["+=", "-=", "*="])
            self.helpers.append(
                f"{f}: proc(a: noalias []f32, b: []const f32, k: f32) -> void = {{\n"
                f"    for (i: u64 = 0; i < a.length && i < b.length; i += 1) {{ a[i] {op} b[i] * k; }}\n}}\n")
            self.noalias.append(f)
        env = dict(empty)
        env["main"] = True
        env["int_calls"] = [h for h, _ in self.int_helpers]
        env["ints"] = {}
        env["floats"] = {}
        env["slices"] = []
        env["boxes"] = []
        env["nodes"] = []
        env["vecs"] = []
        lines = self.block(env, 0, r.randint(15, 45))
        body = "\n        ".join(lines)
        helpers = "\n".join(self.helpers)
        return f"""import "std/print.rin"
import "std/vec.rin"

level: region;

chk: u64 = 14695981039346656037;
mix: proc(v: u64) -> void = {{ chk = (chk ^ v) * 1099511628211; }}

box: struct = {{ xs: []i32 in level; }}
node: struct = {{ v: i32; next: ?*node in level; }}

make_box: proc(r: region level, n: u64) -> *box in level = {{
    b: *box in level = new<box>(r, {{ .xs = new_slice<i32>(r, n) }});
    return b;
}}
swap_box: proc(r: region level, b: *box in level, n: u64) -> u64 = {{
    m: u64 = n + 1;
    b*.xs = new_slice<i32>(r, m);
    return n / 2;
}}
sum_box: proc(a: *box in level, b: *const box in level) -> i32 = {{
    t: i32 = 0;
    for (i: u64 = 0; i < a*.xs.length && i < b*.xs.length; i += 1) {{ a*.xs[i] += 1; t += b*.xs[i]; }}
    return t;
}}
walk: proc(n: *node in level) -> i32 = {{
    t: i32 = 0;
    p: ?*node in level = n;
    for (i: i32 = 0; i < 16 && p != null; i += 1) {{
        q: *node in level = must(p);
        t += q*.v;
        p = q*.next;
    }}
    return t;
}}
grow_while_reading: proc(v: *vec<i32> in level) -> void = {{
    n: u64 = v*.length;
    for (i: u64 = 0; i < n && i < 8; i += 1) {{
        x: i32 = vec<i32>at(v, i)*;
        _ = vec<i32>push(v, x + 1);
    }}
}}

{helpers}
main: proc() -> i32 = {{
    level: region = {{
        {body}
    }}
    printfmt("{{}}\\n", chk);
    return 0;
}}
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--count", type=int, default=200)
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    for i in range(args.count):
        seed = args.seed * 1000003 + i
        (args.out / f"g{seed}.rin").write_text(Gen(seed).program(), encoding="utf-8", newline="\n")
    print(f"gen-rin-safe: wrote {args.count} programs to {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
