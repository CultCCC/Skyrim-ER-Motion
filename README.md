# SDS Combat

An SKSE plugin for **Skyrim SE 1.5.97** that drives how NPCs turn and move during attacks, from annotations placed in
the animations themselves, much like the TAE events of FromSoftware bosses. Made for Skyrim Dragon Souls.

> **Skyrim SE 1.5.97 only.** The plugin is built for the Special Edition runtime 1.5.97 and uses SE Address Library
> IDs, offsets and structure layouts. It does not load on Anniversary Edition (1.6.x) or Skyrim VR; supporting them would
> need every engine address and offset re-found and tested for those runtimes.

Full guide (Russian): [docs/Guide_RU.md](docs/Guide_RU.md) · annotation reference: [docs/Annotations_RU.md](docs/Annotations_RU.md)
· Elden Ring root-motion research: [docs/EldenRing_Research_RU.md](docs/EldenRing_Research_RU.md)

## Features

**Target lock with strength.** NPCs have no lock unless a clip asks for one.

| Annotation | Effect |
|---|---|
| `SDS_NPC_TargetLockOn[.0..1]` | turn toward the player; the strength scales the turn rate (1 = as fast as the engine allows, 0 = frozen) |
| `SDS_NPC_TurnSpeed.0..1` | change the strength later in the same clip |
| `SDS_NPC_TargetLockOff` | no lock for the rest of the clip; the AI turns the NPC again |
| `SDS_NPC_StopRotation` | no turning at all, neither lock nor AI |
| `SDS_NPC_TargetLockReset` | forget what the clip set so far |

Priority: Reset > StopRotation > TargetLockOff > TargetLockOn; every flag ends with the clip that set it. The lock turns
NPCs through the AI turn speed, so root motion follows the new facing.

**Reach to target** (`BoostRootMotionToReachTarget` in Elden Ring). `SDS_NPC_ReachTarget.ref|min|max|arrive` …
`SDS_NPC_ReachTargetEnd` scales the forward root motion of that window by
`clamp(distance - arrive, min, max) / ref`, so the attack ends `arrive` units from its target instead of falling short
or running through it. The distance is measured once (a dodge still works); `ref`, `min` and `max` follow the NPC's
scale.

**No-bump windows.** `SDS_NPC_BumpOff` … `SDS_NPC_BumpOn` switch off the engine's attack movement magnetism for the
NPC through [Animation Motion Fix](https://github.com/max-su-2019/AnimationMotionFix) (graph variable
`AMF_bForbidMoveMagnetism`): the NPC runs on past the player instead of stopping on their collision, and does not shove
them.

**Root motion that follows the animation** (no annotations needed). While an NPC turns during an animation-driven
attack, the engine's root-motion direction can be 20–125° off, and bosses slide sideways. At start-up the plugin
catalogues every `.hkx` with AMR `animmotion` annotations (in the background, ~10 s for ~2300 animations). It
recognises the playing clip by its compressed animation data, because clip names cannot be read at runtime. Every frame
the direction of the NPC's step is then taken from the animation's path, relative to where the NPC faces now; the step
length stays the engine's. Clips it does not know fall back to straightening sideways steps while the lock is on.
Reactive Dodge Framework dodges are left alone.

**Debugging** (all off by default): lock / annotation / reach logging, arrows over NPCs through TrueHUD, a per-frame
movement CSV and hook counters, each one INI switch.

## Requirements

- Skyrim SE **1.5.97** (not AE 1.6.x, not VR)
- SKSE64 2.0.20 and Address Library for SKSE Plugins (SE version)
- Behavior Data Injector (registers the `SDS_NPC_*` events, `mod/SKSE/Plugins/BehaviorDataInjector/SDSCombat_BDI.json`)
- An `animmotion` root-motion framework: Animation Motion Revolution or Root Motion Revolution
- For the bump annotations: Animation Motion Fix (`DisableNpcMovementMagnetism = false`) and BFCO, which adds the
  variable to the humanoid graph
- TrueHUD, only for the debug arrows

Replaces `SDSNPCTargetLock.dll`, `SDSMotionDebug.dll` and Trigger Combat Behaviour's magnetism (legacy
`MagnetismStartTCB` / `MagnetismStopTCB` can still be read, `[Compatibility] bLegacyTCB`).

## Install

Copy `mod/` into a mod folder: `SKSE/Plugins/SDSCombat.dll` (from a release or your build), `SDSCombat.ini`,
`BehaviorDataInjector/SDSCombat_BDI.json`. Settings are documented in the INI and in the guide.

## Build

Visual Studio 2022 with the C++ workload, [xmake](https://xmake.io) 3.x, PowerShell.

```powershell
git clone --recurse-submodules https://github.com/<you>/SDSCombat
cd SDSCombat
powershell -ExecutionPolicy Bypass -File tools/build.ps1                   # -> mod/SKSE/Plugins/SDSCombat.dll
powershell -ExecutionPolicy Bypass -File tools/build.ps1 -Publish D:\Mods\SDSCombat   # also copy mod/ there
```

CommonLibSSE-NG ([alandtse/CommonLibVR](https://github.com/alandtse/CommonLibVR), branch `ng`) is the
`extern/commonlibsse-ng` submodule; `COMMONLIB_PATH` points the build at another copy. Only the SE runtime is built.

`tools/catalog_test` builds a stand-alone check of the animation catalog parser (no CommonLib):
`catalog_test.exe <file.hkx> ...` prints a file's key and path, `catalog_test.exe --build <Data\meshes>` builds a whole
catalog.

`tools/scripts/` holds Python helpers for annotating animations with hkanno64:
`hkx_animmotion_path.py` prints a clip's annotated path over time, `add_reach_target.py` places reach-to-target
windows before every `HitFrame`.

## Layout

| Path | What |
|---|---|
| `plugin/src/TargetLock.*` | annotations, lock, stop rotation, reach to target, bump windows, movement step hook |
| `plugin/src/AnimCatalog.*` | `.hkx` packfile parser and the animmotion catalog (plain C++) |
| `plugin/src/ClipUtil.*` | SEH-guarded reads from behavior clips |
| `plugin/src/Hooks.*` | clip generator, graph event, `Character::Update`, AI turn speed and `ModifyMovementData` hooks |
| `plugin/src/DebugDraw.*`, `Diag.*`, `MotionDebug.*`, `MotionFix.*` | debugging |
| `mod/` | the INI and BDI files of the mod |

## Credits

- **Newrite** — RotateAnnotation: the AI turn-speed hook site
  (`REL::ID(36365) + 0x356`) and the idea of turning NPCs through it, which the target lock is built on.
- **max-su-2019 (Maxsu)** — [Animation Motion Fix](https://github.com/max-su-2019/AnimationMotionFix): the map of the
  engine's movement update this plugin hooks into (`ModifyMovementData`), and the `AMF_bForbidMoveMagnetism` switch the
  bump annotations drive.
- **vinymayan** — [NPC Target Lock](https://github.com/vinymayan/NPC-Target-Lock): the annotation-driven NPC lock this
  one started from.
- **alandtse and the CommonLibSSE-NG contributors** — [CommonLibVR (ng)](https://github.com/alandtse/CommonLibVR).
- The authors of Animation Motion Revolution and Root Motion Revolution, whose `animmotion` annotations the root-motion
  catalog reads, and **opparco** for hkanno.

## License

GPL-3.0 (see [LICENSE](LICENSE)).
