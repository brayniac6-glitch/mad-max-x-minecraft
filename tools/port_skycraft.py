#!/usr/bin/env python3
"""One-shot port of SkyCraft's game-agnostic parts (MIT, (c) 2026 chasmlol) into MadCraft.

Copies fabric/, protocol/ and the Prism launcher bundle from a SkyCraft checkout and renames
identifiers. Re-runnable: it overwrites the copied files, so run it only to re-sync from upstream.
Usage: python tools/port_skycraft.py <path-to-SkyCraft-checkout>
"""
import re, shutil, sys
from pathlib import Path

SRC = Path(sys.argv[1])
DST = Path(__file__).resolve().parent.parent

# Whole-word class renames first (the Sky* prefix also appears in Minecraft terms like skyLight,
# so only these exact class names are touched).
CLASSES = ["SkyClient", "SkyCollider", "SkyCraftClient", "SkyAtlas", "SkyCombat", "SkyLink", "SkyNet",
           "SkyClip", "SkyCollision", "SkyRay", "SkyTri", "SkyWater", "SkyRayTest", "SkyState", "SkyFlags"]
WORD = [(c, "Mad" + c[3:].replace("Craft", "Craft")) for c in CLASSES]
WORD = [(a, b if a != "SkyCraftClient" else "MadCraftClient") for a, b in WORD]
# Then plain substring renames, most specific first.
SUBS = [
    ("SkyrimActorEntity", "MadMaxActorEntity"),
    ("SKYRIM", "MADMAX"),
    ("Skyrim's", "Mad Max's"),
    ("Skyrim", "MadMax"),
    ("skyrim", "madmax"),
    ("SkyCraft_v1", "MadCraft_v1"),
    ("SkyCraft", "MadCraft"),
    ("skycraft", "madcraft"),
    ("SKYCRAFT", "MADCRAFT"),
    ("0x43594B53", "0x4344414D"),  # magic "SKYC" -> "MADC"
    ('"SKYC"', '"MADC"'),
    ("kOffSkyState", "kOffGameState"),
    ("OFF_SKY_STATE", "OFF_GAME_STATE"),
]
# Comments that only make sense in comments, e.g. "MadMax units" -> keep readable prose
PROSE = [("MadMax units", "Mad Max units"), ("MadMax NPC", "Mad Max NPC"), ("MadMax's", "Mad Max's"),
         ("MadMax player", "Mad Max player"), ("MadMax frame", "Mad Max frame"), ("MadMax world", "Mad Max world")]

TEXT_EXT = {".java", ".json", ".gradle", ".properties", ".h", ".cfg", ".txt", ".md", ".bat", ".sh", ".ps1"}


def rename_text(s):
    for a, b in WORD:
        s = re.sub(rf"\b{a}\b", b, s)
    for a, b in SUBS:
        s = s.replace(a, b)
    for a, b in PROSE:
        s = s.replace(a, b)
    return s


def rename_path(rel):
    p = str(rel).replace("\\", "/")
    for a, b in WORD:
        p = re.sub(rf"(?<![A-Za-z]){a}(?=\.java)", b, p)
    for a, b in SUBS:
        p = p.replace(a, b)
    return Path(p)


def copy_tree(sub, dst_sub=None, skip=()):
    base = SRC / sub
    n = 0
    for f in base.rglob("*"):
        if not f.is_file() or any(part in skip for part in f.parts):
            continue
        rel = rename_path(f.relative_to(base))
        out = DST / (dst_sub or sub) / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        if f.suffix in TEXT_EXT or f.name in ("gradlew",):
            out.write_text(rename_text(f.read_text(encoding="utf-8")), encoding="utf-8", newline="\n")
        else:
            shutil.copyfile(f, out)
        n += 1
    return n


print("fabric:", copy_tree("fabric", skip=("build", ".gradle", "run")))
print("protocol:", copy_tree("protocol"))
print("bundle:", copy_tree("tools/minecraft-bundle", "tools/minecraft-bundle"))
proto = DST / "protocol" / "skycraft_protocol.h"
if proto.exists():
    proto.replace(DST / "protocol" / "madcraft_protocol.h")
