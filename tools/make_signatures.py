"""Byte-pattern signatures for every MadMax.exe address MadCraft uses (native/MadCraft.ini [Hooks]
and [Combat]), so the plugin finds them in other builds of the game (Steam) too.

    python tools/make_signatures.py            -> native/src/Signatures.inc

Needs the Ghidra bridge (project madmax) for disassembly, and the GOG MadMax.exe the sheet was built
from. Each address becomes one of:
  - function: the first instructions of the function;
  - global:   an instruction that references it RIP-relatively (+ following instructions); the
              plugin reads the address back out of the matched instruction;
  - vtable:   the class's RTTI name and the vtable's object offset (no bytes needed).
Relative displacements (calls, jumps, RIP-relative operands) are wildcarded, and each pattern is
grown until it occurs exactly once in .text. Patterns are a few dozen opcode bytes, not code.
"""
import json, re, struct, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = Path(r"C:/Program Files/GOG Galaxy/Games/Mad Max/MadMax.exe")
INI = ROOT / "native" / "MadCraft.ini"
OUT = ROOT / "native" / "src" / "Signatures.inc"
GHIDRA = ["--project", "madmax", "--program", "MadMax.exe"]

data = EXE.read_bytes()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
optsz = struct.unpack_from("<H", data, pe + 20)[0]
IMAGE_BASE = struct.unpack_from("<Q", data, pe + 24 + 24)[0]
sections = []
o = pe + 24 + optsz
for _ in range(nsec):
    name = data[o:o + 8].rstrip(b"\0").decode()
    vsize, va, rsize, raw = struct.unpack_from("<IIII", data, o + 8)
    sections.append((name, va, vsize, raw, rsize))
    o += 40


def rva_to_off(rva):
    for _, va, vs, raw, rs in sections:
        if va <= rva < va + max(vs, rs):
            return rva - va + raw
    raise ValueError(hex(rva))


def sec(name):
    return next(s for s in sections if s[0] == name)


TEXT = sec(".text")
TEXT_BYTES = data[TEXT[3]:TEXT[3] + TEXT[4]]


def ghidra(*args):
    out = subprocess.run(["ghidra", *args, *GHIDRA, "--json"], capture_output=True, text=True, timeout=600)
    txt = out.stdout.strip()
    if out.returncode != 0 or not txt:
        sys.exit(f"ghidra {' '.join(args)} failed: {out.stderr[-500:]}")
    return json.loads(txt[txt.index(txt.lstrip()[0]):])


def disasm(va, n):
    return ghidra("disasm", hex(va), "-n", str(n))


def masked(ins):
    """Instruction bytes with relocatable 4-byte displacements (and 8-byte absolute addresses) as None."""
    b = bytes.fromhex(ins["bytes"])
    addr = int(ins["address"], 16)
    out = list(b)
    for t in re.findall(r"0x(14[0-9a-fA-F]{7})", " ".join(ins["operands"])):
        target = int(t, 16)
        rel = (target - (addr + len(b))) & 0xFFFFFFFF
        for needle, width in ((struct.pack("<I", rel), 4), (struct.pack("<Q", target), 8)):
            i = b.find(needle)
            if i >= 0:
                for k in range(i, i + width):
                    out[k] = None
    return out


def regex(pattern):
    return re.compile(b"".join(b"." if x is None else re.escape(bytes([x])) for x in pattern), re.DOTALL)


def count(pattern):
    return sum(1 for _ in regex(pattern).finditer(TEXT_BYTES))


def grow(start_va, extra_check=None, max_ins=24):
    """Pattern from start_va on, instruction by instruction, until it's unique in .text."""
    pattern, ins_list = [], disasm(start_va, max_ins)
    for ins in ins_list:
        pattern += masked(ins)
        fixed = sum(1 for x in pattern if x is not None)
        if fixed >= 12 and count(pattern) == 1:
            return pattern, ins_list
    return None, ins_list


def fmt(pattern):
    return " ".join("??" if x is None else f"{x:02X}" for x in pattern)


# ---- what the ini uses -------------------------------------------------------------------------
ini = INI.read_text(encoding="utf-8")
rvas = sorted({int(m, 16) for m in re.findall(r"MadMax\.exe\+([0-9A-Fa-f]+)", ini)})
data_lo, data_hi = sec(".data")[1], sec(".data")[1] + sec(".data")[2]
rdata_lo, rdata_hi = sec(".rdata")[1], sec(".rdata")[1] + sec(".rdata")[2]

entries = []
for rva in rvas:
    va = IMAGE_BASE + rva
    if rdata_lo <= rva < rdata_hi:
        # A vtable: its RTTI complete object locator sits just before it.
        col = struct.unpack_from("<Q", data, rva_to_off(rva - 8))[0] - IMAGE_BASE
        sig, offset, _cd, td, _ch, _self = struct.unpack_from("<IIIiii", data, rva_to_off(col))
        name = data[rva_to_off(td + 0x10):].split(b"\0")[0].decode()
        entries.append(("vtable", rva, name, offset, None, 0, 0))
        print(f"{rva:08X} vtable {name} @+{offset:X}")
    elif data_lo <= rva < data_hi:
        refs = ghidra("x-ref", "to", hex(va))
        best = None
        for ref in refs[:60]:
            frm = int(ref["from"], 16)
            if not (TEXT[1] <= frm - IMAGE_BASE < TEXT[1] + TEXT[2]):
                continue
            pattern, ins = grow(frm)
            if not pattern:
                continue
            first = ins[0]
            b = bytes.fromhex(first["bytes"])
            rel = struct.pack("<I", (va - (frm + len(b))) & 0xFFFFFFFF)
            disp = b.find(rel)
            if disp < 0:
                continue
            # The shortest unique one: long patterns run through code that's more likely to differ.
            if best is None or len(pattern) < len(best[0]):
                best = (pattern, disp, len(b), frm)
            if len(pattern) <= 20:
                break
        if best is None:
            sys.exit(f"no unique reference pattern for global {rva:X}")
        pattern, disp, ilen, frm = best
        entries.append(("global", rva, None, 0, pattern, disp, ilen))
        print(f"{rva:08X} global via {frm - IMAGE_BASE:08X}: {fmt(pattern)}")
    else:
        pattern, _ = grow(va)
        if not pattern:
            sys.exit(f"no unique pattern for function {rva:X}")
        entries.append(("function", rva, None, 0, pattern, 0, 0))
        print(f"{rva:08X} function: {fmt(pattern)}")

lines = ["// Generated by tools/make_signatures.py from the GOG MadMax.exe (build 565D5965). Do not edit.",
         "// { kind, GOG rva, RTTI name (vtable), vtable offset, pattern, disp offset, instruction length }"]
for kind, rva, name, offset, pattern, disp, ilen in entries:
    lines.append(f'{{ Kind::{kind}, 0x{rva:X}, {json.dumps(name or "")}, 0x{offset:X}, "{fmt(pattern) if pattern else ""}", {disp}, {ilen} }},')
OUT.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
print(f"{len(entries)} signatures -> {OUT.relative_to(ROOT)}")
