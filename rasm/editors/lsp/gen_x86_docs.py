"""Generate x86_64.json, the instruction and register notes rasm_lsp.py shows on hover.

The data is asm-lsp's (https://github.com/bergercookie/asm-lsp, BSD-2-Clause),
whose x86-64 instruction set comes from the Opcodes package
(https://github.com/Maratyszcza/Opcodes, BSD-2-Clause). Kept per Intel mnemonic:
the summary, the ISA extensions it needs, and its operand forms.

    python gen_x86_docs.py [path to the asm-lsp checkout]     (default C:\\devel\\asm-lsp)
"""
import json
import os
import sys
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else r"C:\devel\asm-lsp"
    store = os.path.join(root, "docs_store")
    instructions = {}
    tree = ET.parse(os.path.join(store, "opcodes", "x86_64.xml"))
    for ins in tree.getroot().iter("Instruction"):
        name = ins.get("name", "").lower()
        if not name:
            continue
        forms, isas = [], []
        for form in ins.iter("InstructionForm"):
            ops = ", ".join(op.get("type", "?") for op in form.iter("Operand"))
            if ops not in forms:
                forms.append(ops)
            for isa in form.iter("ISA"):
                i = isa.get("id")
                if i and i not in isas:
                    isas.append(i)
        entry = instructions.setdefault(name, {"summary": ins.get("summary", ""), "isa": [], "forms": []})
        for f in forms:
            if f not in entry["forms"]:
                entry["forms"].append(f)
        for i in isas:
            if i not in entry["isa"]:
                entry["isa"].append(i)

    registers = {}
    tree = ET.parse(os.path.join(store, "registers", "x86_64.xml"))
    for reg in tree.getroot().iter("Register"):
        name = reg.get("name", "").lower()
        if name:
            registers[name] = {
                "description": reg.get("description", ""),
                "type": reg.get("type", ""),
                "width": reg.get("width", ""),
            }

    out = os.path.join(HERE, "x86_64.json")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        json.dump({"source": "asm-lsp docs_store (BSD-2-Clause), from the Opcodes package (BSD-2-Clause)",
                   "instructions": instructions, "registers": registers}, f, separators=(",", ":"), sort_keys=True)
    print(f"wrote {out}: {len(instructions)} instructions, {len(registers)} registers")


if __name__ == "__main__":
    main()
