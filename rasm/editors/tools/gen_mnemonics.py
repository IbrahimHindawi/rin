#!/usr/bin/env python3
"""Generate rasm's x64 mnemonic list for the editor grammars.

Reads asm-lsp's opcode database (x86_64.xml, from the Opcodes project) and
writes the mnemonics into:

    editors/nvim/syntax/rasm.vim            between the GENERATED MNEMONICS markers
    editors/textmate/rasm.tmLanguage.json   repository.instruction.match

    python editors/tools/gen_mnemonics.py [--xml PATH] [--list]

--list prints the mnemonics, one per line, and writes nothing.

The XML has the general-purpose, SSE..SSE4.2, AVX, AVX2, FMA, BMI and AVX-512
instructions but not x87, the string instructions, the rep/lock prefixes or a
few system ones (leave, loop, pushfq, cli, hlt). Those are in EXTRA below,
because rasm's own examples use some of them (math.rasm's sinf is fld, fsin,
fstp).
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

HERE = Path(__file__).resolve().parent
EDITORS = HERE.parent
VIM_FILE = EDITORS / "nvim" / "syntax" / "rasm.vim"
TM_FILE = EDITORS / "textmate" / "rasm.tmLanguage.json"
DEFAULT_XML = Path("C:/devel/asm-lsp/docs_store/opcodes/x86_64.xml")

# Instruction sets left out: AMD's abandoned extensions, Xeon Phi only, and AMX
# (its tmm registers are not rasm registers). An instruction is kept when any
# of its forms belongs to a set not listed here, or to no set (the base ISA).
EXCLUDED_ISA = {
    "3dnow!", "3dnow!+", "FEMMS", "XOP", "FMA4", "TBM",
    "AVX512ER", "AVX512PF", "PREFETCHWT1",
    "AMX-TILE", "AMX-INT8", "AMX-BF16", "AMX-FP16", "AMX-COMPLEX",
}

PREFIXES = ["rep", "repe", "repz", "repne", "repnz", "lock"]

# Not in x86_64.xml.
EXTRA = """
    movsb movsw movsd movsq stosb stosw stosd stosq lodsb lodsw lodsd lodsq
    scasb scasw scasd scasq cmpsb cmpsw cmpsd cmpsq insb insw insd outsb outsw outsd
    leave enter loop loope loopz loopne loopnz pushf pushfq popf popfq
    cli sti hlt int1 iretq sysret sysenter sysexit ud0 ud1 retf retn xlat xlatb
    wait fwait movabs in out swapgs invlpg wbinvd rdmsr wrmsr xsetbv
    xsave xsave64 xrstor xrstor64 xsaveopt xsavec xsaves xrstors
    fxsave fxsave64 fxrstor fxrstor64
    f2xm1 fabs fadd faddp fiadd fbld fbstp fchs fclex fnclex
    fcmovb fcmove fcmovbe fcmovu fcmovnb fcmovne fcmovnbe fcmovnu
    fcom fcomp fcompp fcomi fcomip fucom fucomp fucompp fucomi fucomip ficom ficomp
    fcos fdecstp fdiv fdivp fidiv fdivr fdivrp fidivr ffree fincstp finit fninit
    fld fild fld1 fldl2t fldl2e fldpi fldlg2 fldln2 fldz fldcw fldenv
    fmul fmulp fimul fnop fpatan fprem fprem1 fptan frndint frstor fsave fnsave
    fscale fsin fsincos fsqrt fst fstp fist fistp fisttp fstcw fnstcw fstenv fnstenv
    fstsw fnstsw fsub fsubp fisub fsubr fsubrp fisubr ftst fxam fxch fxtract fyl2x fyl2xp1
""".split()

VIM_BEGIN = '" BEGIN GENERATED MNEMONICS'
VIM_END = '" END GENERATED MNEMONICS'


def mnemonics_from_xml(path: Path) -> set[str]:
    names: set[str] = set()
    for inst in ET.parse(path).getroot().iter("Instruction"):
        name = inst.get("name", "").lower()
        if not re.fullmatch(r"[a-z][a-z0-9]*", name):
            print(f"skipping odd name {name!r}", file=sys.stderr)
            continue
        keep = False
        for form in inst.iter("InstructionForm"):
            isas = {isa.get("id") for isa in form.iter("ISA")}
            if not isas or isas - EXCLUDED_ISA:
                keep = True
                break
        if keep:
            names.add(name)
    return names


def trie_regex(words: list[str]) -> str:
    """An alternation of words, factored by common prefix. Used with a \\b after
    it, so the order of branches does not matter: the engine backtracks."""
    trie: dict = {}
    for w in words:
        node = trie
        for ch in w:
            node = node.setdefault(ch, {})
        node[""] = {}

    def emit(node: dict) -> str:
        ends = "" in node
        branches = [ch + emit(child) for ch, child in sorted(node.items()) if ch]
        if not branches:
            return ""
        if not ends:
            return branches[0] if len(branches) == 1 else "(?:" + "|".join(branches) + ")"
        if len(branches) == 1 and len(branches[0]) == 1:
            return branches[0] + "?"
        return "(?:" + "|".join(branches) + ")?"

    return emit(trie)


def vim_block(words: list[str]) -> str:
    lines = [VIM_BEGIN, f'" {len(words)} mnemonics; see editors/tools/gen_mnemonics.py']
    line = "syn keyword rasmMnemonic contained"
    for w in words:
        if len(line) + 1 + len(w) > 100:
            lines.append(line)
            line = "syn keyword rasmMnemonic contained"
        line += " " + w
    lines.append(line)
    lines.append(VIM_END)
    return "\n".join(lines)


def write_vim(words: list[str]) -> None:
    text = VIM_FILE.read_text(encoding="utf-8")
    start = text.index(VIM_BEGIN)
    end = text.index(VIM_END) + len(VIM_END)
    VIM_FILE.write_text(text[:start] + vim_block(words) + text[end:], encoding="utf-8", newline="\n")


def write_tm(words: list[str]) -> None:
    grammar = json.loads(TM_FILE.read_text(encoding="utf-8"))
    prefixes = "|".join(PREFIXES)
    grammar["repository"]["instruction"]["match"] = (
        rf"^\s*(?:({prefixes})\s+)?({trie_regex(words)})\b(?!\s*[:(])"
    )
    TM_FILE.write_text(json.dumps(grammar, indent=2, ensure_ascii=False) + "\n",
                       encoding="utf-8", newline="\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--xml", type=Path, default=DEFAULT_XML, help="path to asm-lsp's x86_64.xml")
    parser.add_argument("--list", action="store_true", help="print the mnemonics and write nothing")
    args = parser.parse_args()

    from_xml = mnemonics_from_xml(args.xml)
    words = sorted((from_xml | set(EXTRA)) - set(PREFIXES))

    # The trie must accept exactly the list: every word, and no prefix of a
    # word that is not itself a word.
    rx = re.compile(rf"(?:{trie_regex(words)})\Z")
    wordset = set(words)
    rejected = [w for w in words if not rx.match(w)]
    accepted = sorted({w[:i] for w in words for i in range(1, len(w))} - wordset)
    accepted = [p for p in accepted if rx.match(p)]
    if rejected or accepted:
        print(f"trie regex rejects {rejected[:10]}, accepts {accepted[:10]}", file=sys.stderr)
        return 1

    if args.list:
        print("\n".join(words))
        return 0

    write_vim(words)
    write_tm(words)
    print(f"{len(from_xml)} from {args.xml.name}, {len(set(EXTRA) - from_xml)} extra, {len(words)} total")
    print(f"wrote {VIM_FILE}")
    print(f"wrote {TM_FILE}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
