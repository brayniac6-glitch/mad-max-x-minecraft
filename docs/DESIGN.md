# MadCraft design

Play Mad Max as a Minecraft player (default Steve): Minecraft's movement, inventory, HUD, blocks and
combat inside Mad Max's wasteland. A port of [SkyCraft](https://github.com/chasmlol/SkyCraft)'s
design (MIT, chasmlol) from Skyrim to Mad Max; read SkyCraft's `docs/DESIGN.md` for the full
rationale. This file records only what differs.

## Core principle (unchanged)

Neither game is rewritten. Minecraft runs its own logic (movement, combat, inventory, blocks,
rendering of its things); Mad Max runs its world (terrain, War Boys, convoys, missions, driving).
The two mods only translate. If we re-implement a Minecraft mechanic in C++ or a Mad Max mechanic
in Java, the design has gone wrong.

## Components

```
MadMax.exe                                          javaw.exe (Minecraft 26.3 + Fabric)
  dinput8.dll = MadCraft native plugin  <── shm ──>   madcraft Fabric mod (ported from SkyCraft)
    Input    : DirectInput keyboard/mouse wrap          input handlers
    Game     : puppet Max, teleport handshake           real MC LocalPlayer physics
    Overlay  : Present hook, MC HUD/hand on top         offscreen HUD/hand/GUI layers
    MadMax   : every Mad Max memory access (ini hooks)  mirror world (void, MC blocks only)
```

`protocol/madcraft_protocol.h` is SkyCraft's protocol, renamed (magic `MADC`, mapping
`Local\MadCraft_v1`). Its Java mirror is `fabric/.../link/Proto.java`.

## What differs from SkyCraft

| Topic | SkyCraft (Skyrim) | MadCraft (Mad Max) |
|---|---|---|
| Loader | SKSE plugin | `dinput8.dll` proxy in the game folder (Mad Max imports DirectInput 8) |
| Game API | CommonLibSSE-NG + Address Library | None exists: hook points found in Ghidra, listed in `sheet/hooks.tsv`, configured as pointer chains in `MadCraft.ini [Hooks]` |
| Input | Skyrim input event sinks, MenuControls/PlayerControls hooks | Wrap the game's own DirectInput devices; blank them for the game while MC drives |
| Swap chain | `BSGraphics::Renderer` | Present found from a probe swap chain, hooked with MinHook |
| Units | 70 Skyrim units per block, Z up | Apex: metres, Y up. 1 unit per block, axes configurable until verified |
| Worlds | Worldspaces + interior cells | One open world (`worldId = 1`) |
| Player view | First person | Mad Max is third person: Steve is drawn whole (SkyCraft's avatar path, `kRenAvatar`) |
| Vehicles | Horses (Skyrim takes over) | The Magnum Opus stays Mad Max's: while driving (or F8), Mad Max has the controls and Minecraft follows Max |
| Skin | The player's own | Default Steve (forced on the MC side) |

## Phases

| # | Phase | Done when | Needs (hooks.tsv) |
|---|---|---|---|
| 0 | Link | Handshake; walking in MC moves Max over a flat floor at Max's ground height; MC HUD over Mad Max | H01 PlayerMatrix, H02 InVehicle |
| 1 | Walk the wasteland | Real Mad Max collision (Apex raycast field), game-thread update, Max's own movement off, camera | H03-H07 |
| 2 | Steve | Max's mesh hidden, Steve drawn in third person, time of day synced | H06, H08, H13 |
| 3 | Combat | War Boys as MC proxy entities; hits both ways | H09-H11 |
| 4 | Blocks | Place/break blocks on wasteland surfaces, depth-composited | H12 + SkyCraft WorldRender port |
| 5 | Polish | Save snapshots, loot bridge (scrap <-> iron), sandstorms -> MC weather | |

## Reverse engineering workflow (the Spreadsheet Method)

`sheet/` is the source of truth. A hook goes `open -> found -> verified` in `hooks.tsv`; its `fid`
joins to `functions.tsv`. Agents claim a subsystem (`tools/sheet.py claim`) before editing, and
`tools/sheet.py validate` must pass. Raw decompiler output stays in `decomp/` (gitignored).
