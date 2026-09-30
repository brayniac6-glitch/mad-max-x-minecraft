# Third-party notices

## SkyCraft

The Minecraft (Fabric) mod in `fabric/`, the shared-memory protocol in `protocol/`, the Prism
launcher bundle in `tools/minecraft-bundle/` and the design MadCraft follows are ported from
SkyCraft (https://github.com/chasmlol/SkyCraft), used under the MIT License:

```
MIT License

Copyright (c) 2026 chasmlol

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Notices carried over from SkyCraft


SkyCraft is MIT-licensed (see `LICENSE`). A release also contains, or is built from, the following.

## In the Skyrim plugin (`SkyCraft.dll`)

| Component | License | Source |
|---|---|---|
| CommonLibSSE-NG | MIT | https://github.com/alandtse/CommonLibVR/tree/ng |
| spdlog | MIT | https://github.com/gabime/spdlog |
| {fmt} | MIT | https://github.com/fmtlib/fmt |
| xbyak | BSD-3-Clause | https://github.com/herumi/xbyak |
| SimpleIni | MIT | https://github.com/brofield/simpleini |

## In the bundled Minecraft (`SkyCraft-Minecraft.zip`)

| Component | License | Source |
|---|---|---|
| Prism Launcher (unmodified portable Windows build) | GPL-3.0 | https://github.com/PrismLauncher/PrismLauncher (the version is in the bundle's `THIRD-PARTY.txt`) |
| Fabric API | Apache-2.0 | https://github.com/FabricMC/fabric |
| e4mc | MIT | https://github.com/vgskye/e4mc-minecraft-architectury |

The bundle carries Prism Launcher's full license text as `Prism/LICENSE-PrismLauncher.txt`.

## Not included

Minecraft, Java and Fabric Loader aren't included. Prism Launcher downloads them from Mojang,
the Java vendor and FabricMC after the player signs in with a Microsoft account that owns
Minecraft: Java Edition. Skyrim, SKSE and the Address Library aren't included either.

SkyCraft isn't affiliated with or endorsed by Mojang, Microsoft, Bethesda or ZeniMax.

