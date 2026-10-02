# Mad Max x Minecraft: agent rules (Spreadsheet Method)

`sheet/` is the single source of truth. Code is a projection of it. Schema: `sheet/SCHEMA.md`.

## Hard rules
1. **Read rows, not code.** Use `python tools/sheet.py` queries/grep on `sheet/*.tsv`. Only decompile a
   single function (`sheet.py decomp <fid>`) when a row is missing information you need.
2. **Never invent an interface.** Any function, type, field, enum, or signature you reference must already
   exist as a sheet row. If it doesn't, add the row first (status `raw`/`named`), then use it.
   `python tools/sheet.py validate` must pass before any commit.
3. **Claim before editing.** Run `sheet.py claim <sub> <agent>` before touching rows or code of a subsystem.
   Only edit rows whose `sub` you hold. Release with `sheet.py release <sub>` when done.
4. **One subsystem, one output file set.** Generated/handwritten code lives under `src/<sub>/`. Never edit
   another subsystem's folder.
5. **Coverage is a query.** "What's left?" = `python tools/sheet.py coverage`. Don't guess.
6. **Legal hygiene.** `decomp/` (raw Ghidra output) is gitignored and must never be committed or pasted into
   sheet cells or `src/`. The sheet stores our own names, signatures, and one-line specs. Port behaviour,
   not code.

## Workflow per subsystem
triage (name + ns) → type (sig, types/fields) → spec (1-line behaviour) → map (mc_map key) → impl → verify

## Tools
- Ghidra project: `madmax`, program `MadMax.exe` (ghidra-cli, `ghidra ... --project madmax --program MadMax.exe`)
- `python tools/sheet.py ingest`: refresh sheet from Ghidra (preserves human-edited columns)
- `python tools/sheet.py coverage [--by sub]`
- `python tools/sheet.py validate`
- `python tools/sheet.py claim|release <sub> [agent]`
- `python tools/sheet.py decomp <fid|addr>` → `decomp/<fid>.c` (cached)
- `python tools/sheet.py next <sub> [n]`: highest-value unfinished rows to work on
- `python tools/make_signatures.py`: after adding or changing any `MadMax.exe+...` address in
  `native/MadCraft.ini`, regenerate `native/src/Signatures.inc` (Steam/other builds find addresses
  by these). The plugin logs `addresses: GOG build; N/N signatures agree` when they're right.
