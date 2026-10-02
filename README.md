# MadCraft: Mad Max × Minecraft
![image alt](https://github.com/brayniac6-glitch/mad-max-x-minecraft/blob/ce42237e496d2f352a837946c94994868ecc5c30/screenshot.png)
Play Mad Max in Minecraft

Based on [SkyCraft](https://github.com/chasmlol/SkyCraft), which does the same for Skyrim (MIT licence).

## Features

 TO ACTUALLY PLAY AS STEVE PRESS F8 THAT ALLOWS YOU TO ACTUALLY MOVE AROUND IN THE WORLD IF YOU GET STUCK AT ANY POINT PRESS F8 AGAIN AND YOU WILL BE REGULAR MAX (But the steve model will be on top of him)

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

### The easy way: let an AI do it

If you use an AI assistant that can work on your PC (for example **Claude Code** or the **Claude
desktop app**, or any AI agent that can run commands and download files), give it this
repository's link and it can read this README and install MadCraft for you. Copy and paste:

> Install MadCraft for me from https://github.com/brayniac6-glitch/mad-max-x-minecraft. Read the
> README, download the latest release zip, install Fabric Loader for Minecraft 26.3, put Fabric API
> and the MadCraft jar in my `.minecraft\mods` folder, and copy the Mad Max files into my Mad Max
> folder (find where `MadMax.exe` is). Tell me what you did and anything I still need to do.

Things to know:
- The AI still needs **your own copies** of both games installed. It can't buy them, sign you in,
  or get around the ownership checks. You start Minecraft and sign in yourself.
- Only use this repository's link. Other copies of MadCraft could be modified.
- A good assistant tells you which files it copied and where, so you can undo it (see *uninstall*
  below).

### Or do it yourself

#### 1. Minecraft (Fabric 26.3)

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

#### 2. Mad Max (GOG)

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

MadCraft is released under the **MIT License**, the same license as SkyCraft, which it's derived
from. `LICENSE` carries SkyCraft's license text unchanged, with chasmlol's copyright notice kept
alongside this project's, as the MIT License requires. Other components (MinHook in the Mad Max
plugin) and their licenses are listed in `THIRD-PARTY-NOTICES.md`.
