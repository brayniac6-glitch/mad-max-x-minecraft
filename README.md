# MadCraft: Mad Max × Minecraft

Play **Mad Max** as **Minecraft's Steve**. Mad Max and Minecraft run at the same time: you walk,
jump, sprint, swim and fly (elytra!) with Minecraft's movement, but you're in Mad Max's wasteland.
You can build in it, fight its War Boys with Minecraft weapons, and drive its cars.

Based on [SkyCraft](https://github.com/chasmlol/SkyCraft), which does the same for Skyrim (MIT licence).

## Features

- **Be Steve.** Max is hidden and Minecraft's player takes his place: first person (your Minecraft
  hand and items) or F5 third person (Steve with your armour and held items).
- **Minecraft movement on Mad Max's ground.** Terrain, rocks, buildings, ships and wrecks are solid.
- **Build in the wasteland.** Placed blocks stay put in Mad Max's world and hide properly behind its
  buildings and rocks.
- **Combat.** Minecraft weapons hurt Mad Max's NPCs, scaled like hitting a Minecraft mob (a wooden
  sword takes five hits, a diamond sword three). Crits, Sharpness, Strength, the attack cooldown and
  bows all count.
- **Dynamic light.** Torches, lanterns, glowstone, fire and lava light up Mad Max's own world, and so
  does a torch in your hand. Steve, your hand and your blocks darken in Mad Max's shadows, bunkers
  and nights.
- **Cars.** Press **F** at a car to get in and drive with Mad Max's controls. **F5** switches between
  first person from the driver's seat and Mad Max's chase camera.
- **Survival or creative, your inventory, your rules.** It's a real Minecraft world (the "MadCraft"
  world) that follows you around Mad Max.

## What you need

You need your **own bought copies of both games**. MadCraft contains no game files and checks for
real copies:

| | Requirement |
|---|---|
| **Mad Max** | The **GOG** PC version (build `565D5965`). MadCraft checks for a genuine Steam/GOG install and for this exact build. On any other build it stays off (with a message) instead of risking a crash. The Steam version isn't supported yet because its exe differs. |
| **Minecraft** | **Minecraft: Java Edition**, bought, started from the **official launcher** and signed in with your **Microsoft account**. MadCraft stays off on offline or cracked launchers. |
| **Minecraft version** | **26.3** with **[Fabric Loader](https://fabricmc.net/use/installer/) 0.19.5+** and **[Fabric API](https://modrinth.com/mod/fabric-api) 0.161.0+26.3** |
| **PC** | Windows 10/11 64-bit, DirectX 11, **16 GB RAM** recommended (both games run at once) |

## Download

Get **`MadCraft-0.1.0.zip`** from this repository's **[Releases](../../releases)** page. Inside:

```
Mad Max folder/
    dinput8.dll              <- the Mad Max plugin
    madcraft/MadCraft.ini    <- settings
Minecraft mods folder/
    madcraft-0.1.0.jar       <- the Minecraft mod
```

## Install

### 1. Minecraft (Fabric 26.3)

1. Open the official Minecraft Launcher once and run **26.3** so the version is downloaded.
2. Download and run the **[Fabric Installer](https://fabricmc.net/use/installer/)**. Choose
   Minecraft **26.3**, Loader **0.19.5** or newer, and click **Install**. A **Fabric 26.3**
   profile appears in the launcher.
3. Open your mods folder: press `Win + R`, type `%appdata%\.minecraft\mods` and press Enter. Create
   the `mods` folder if it doesn't exist.
4. Put these in it:
   - **Fabric API** for 26.3 ([Modrinth](https://modrinth.com/mod/fabric-api/versions) or
     [CurseForge](https://www.curseforge.com/minecraft/mc-mods/fabric-api))
   - **`madcraft-0.1.0.jar`** from the zip's `Minecraft mods folder`

### 2. Mad Max (GOG)

1. Find your Mad Max folder (the one with `MadMax.exe`). For GOG Galaxy it's usually
   `C:\Program Files\GOG Galaxy\Games\Mad Max` (Galaxy: Mad Max → *Manage installation* → *Show folder*).
2. Copy **everything inside** the zip's `Mad Max folder` into it: `dinput8.dll` next to
   `MadMax.exe`, and the `madcraft` folder.

To uninstall, delete `dinput8.dll` and the `madcraft` folder from the Mad Max folder, and the jar
from your mods folder.

## Play

1. Start **Minecraft** from the official launcher with the **Fabric 26.3** profile and wait for the
   title screen. You don't need to open a world.
2. Start **Mad Max** (Galaxy or `MadMax.exe`) and load or start your save.
3. Once Mad Max is in game, Minecraft links up by itself. It opens its "MadCraft" world, its window
   hides, and its HUD appears over Mad Max.
4. Press **F8** to take over as Steve.

Tip: start Minecraft first. It takes about as long to load as Mad Max takes to reach its menu.

## Controls

| Key | What it does |
|---|---|
| **F8** | Switch between **Minecraft controls** (you're Steve) and **Mad Max controls** (menus, the Magnum Opus garage, anything Minecraft can't do) |
| **Esc** | Mad Max's pause menu (the controls go to Mad Max; F8 to come back) |
| **O** | Minecraft's own menu (options, Open to LAN) |
| **E**, number keys, mouse | Minecraft's inventory, hotbar, attack/use, as usual |
| **F5** | Minecraft's camera views (first person / behind / in front) |
| **F** | At a car: get in. In a car: get out |
| **F5** *(in a car)* | First person from the driver's seat ↔ Mad Max's chase camera |
| **Page Up / Page Down**, **Home / End** *(first person in a car)* | Move your eyes up/down, forward/back (saved) |
| Driving | Mad Max's own car controls (W/S, A/D, boost, guns...) |

## Settings

Everything is in **`<Mad Max>\madcraft\MadCraft.ini`**, commented. The useful ones:

| Setting | What it does |
|---|---|
| `[Render] iMaxFps = 90` | Mad Max's frame cap. Both games share your GPU; lower it if Steve or the hand look choppy. |
| `[Render] fTorchIntensity`, `bDynamicLights` | Torch brightness, or turn dynamic light off |
| `[Render] fShadeDaylight`, `bPlayerShade`, `bBlockShade` | How soon Steve and blocks darken in the dark (higher = sooner) |
| `[Combat] fNpcMinecraftHealth = 20` | How tough Mad Max NPCs are against Minecraft weapons (higher = tougher) |
| `[Camera] bFirstPerson`, `bHideMax` | First person from Steve's eyes; hide Max's model |
| `[Vehicle] fFirstPersonEyeY` | First-person eye height in cars |

## Troubleshooting

- **A "MadCraft is off" message when Mad Max starts.** Your Mad Max isn't the supported GOG build,
  or isn't a Steam/GOG install. Mad Max still runs normally without MadCraft.
- **A "MadCraft is off" popup in Minecraft.** Start Minecraft from the **official launcher**, signed
  in with the Microsoft account that owns it.
- **Nothing links up.** Check that Minecraft is on the **Fabric 26.3** profile with Fabric API and
  `madcraft-0.1.0.jar` in `mods`, then load a save in Mad Max (the main menu doesn't link). The log is
  `<Mad Max>\madcraft\MadCraft.log`. Minecraft's own log is `%appdata%\.minecraft\logs\latest.log`.
- **"Not enough memory" or crashes when starting.** Both games are big. Close other programs; 16 GB
  RAM is recommended.
- **Mouse won't click Mad Max's menus.** Press **F8** (or Esc) so Mad Max has the controls.
- **Fell through the ground.** It recovers by itself within a couple of seconds. If not, F8 twice.
- **Choppy hand or Steve.** Lower `[Render] iMaxFps` (try 60).

## Known limits

- GOG Mad Max only, for now.
- Torch light shines through walls (no shadows from Minecraft lights yet).
- Other cars and people are not solid for Steve (only the world and props).
- Mad Max's clock isn't read yet; Minecraft stays at noon (Mad Max's own lighting is unaffected).

## Building from source

- `native/`: the Mad Max plugin (C++20, CMake, Visual Studio 2022+ Build Tools). Run
  `cmake -B build -A x64` then `cmake --build build --config Release`, which gives
  `build/Release/dinput8.dll`.
- `fabric/`: the Minecraft mod (Java 25, Gradle/Loom). `gradlew build` gives
  `build/libs/madcraft-<version>.jar`. `gradlew runClient` is a development client (no account
  check).
- `protocol/`: the shared-memory layout both sides use. `sheet/` and `tools/`: the reverse
  engineering notes (Spreadsheet Method, see `CLAUDE.md`).

## Legal

MadCraft is a fan-made mod, **not affiliated with or endorsed by** Mojang Studios, Microsoft, Warner
Bros. Games or Avalanche Studios. *Minecraft* is a trademark of Mojang/Microsoft and *Mad Max* of
Warner Bros. No game code or assets are included. You need your own legally bought copies of both
games. Minecraft mods follow the [Minecraft Usage Guidelines](https://www.minecraft.net/usage-guidelines).

MadCraft's own code is MIT-licensed (see `LICENSE`). It's derived from SkyCraft by chasmlol (MIT),
see `THIRD-PARTY-NOTICES.md`.
