# Life is Strange: Reunion: findings

Game: Life is Strange: Reunion (Deck Nine), Steam app 2624870, build 22706033. Unreal Engine 5
(project `Iris`), most likely 5.6: the exe has 5.6-era cvars (`r.MegaLights.EnableForProject`,
`r.Lumen.ScreenProbeGather.ShortRangeAO.HardwareRayTracing`, `r.Nanite.Tessellation`,
`r.Nanite.AllowWorkGraphMaterials`) and none of 5.7's (`r.Nanite.Voxels`). The version string is
stripped (file version `UE5-CL-0`). DirectX 12 (Agility SDK `D3D12Core.dll` shipped).

Paths:
- Game: `E:\SteamLibrary\steamapps\common\LifeisStrangeReunion`
- Executable: `Iris\Binaries\Win64\Iris-Win64-Shipping.exe` (656 MB; `LifeIsStrangeReunion.exe`
  in the root is a launcher stub)
- User config: `%LOCALAPPDATA%\Iris\Saved_Steam_<steamid>\Config\Windows\` (`Engine.ini`,
  `GameUserSettings.ini`; the game's own settings are under `[/Script/D9Runtime.D9GameUserSettings]`).
  No `Logs` folder: the shipping build doesn't write a log.

## No upscaler plugins

Only Unreal's TSR. No DLSS, Streamline, NGX, FSR or XeSS code or strings in the exe
(`FiberStreamline*` is hair grooms). The menu has Resolution Scale and Secondary Resolution
Scale. The user's config: 3840x2160, `ResolutionScale=0.65`, HDR on at `HDRDisplayOutputNits=1000`.

## Protected exe

The sections are renamed and there is a large writable+executable section (anti-tamper):

| section | flags | holds |
|---|---|---|
| `.sdata` (0x1000, 130 MB) | R X | the code (plain on disk) |
| `.xtls` | R | read-only data: cvar name strings, vtables |
| `.00cfg` | R W | data (the console manager singleton is here) |
| `.debug$P` (470 MB) | R W X | protection code |

So the add-on picks sections by flags (code = executable and not writable; read-only data =
readable, not executable, not writable) and never patches code (tamper checks could crash the
game).

## Console variable layout

Same as UE 5.5.4 (DirectiveTweaks), read from the game's own call sites in `.sdata`:

- 5 `lea` of `L"r.ScreenPercentage"`; 3 are followed by `call [rax+0xb8]` (FindConsoleVariable),
  and all 3 load the singleton from the same address (RVA 0xa813650).
- After `call [rax+0xb8]` the next virtual call is `+0x68` (AsVariableInt, 426 times), `+0xb0`
  (GetInt, 72), `+0xb8` (GetFloat, 52), `+0x70` (AsVariableFloat, 38), `+0xc0` (GetString),
  `+0xa8` (GetBool).
- `mov r8d, 0x0D000000` / 0x0E / 0x0C (SetBy) are followed by `call [rax+0x98]` (Set).

Game-specific cvars: `D9.*` (Deck Nine), including character detail levels
(`D9.SigMan.MinLOD.*`, `D9.SigMan.Buckets.*`), `D9.EnableShadowCastingPriority`,
`D9.ForceArcadeQuality`, `D9.EmulateLowEnd`.

## No hardware ray tracing

`r.RayTracing=0` at start (a read-only startup setting), although `r.RayTracing.Enable`,
`r.Lumen.HardwareRayTracing` and `r.Lumen.Reflections.HardwareRayTracing` all read 1: the
project has ray tracing off. Forcing it with `r.RayTracing=1` under
`[/script/engine.renderersettings]` in Engine.ini stops the game at start (2026-10-25):

    LowLevelFatalError [ShaderCompiler.cpp:4578] Missing global shader
    FNiagaraCollisionRayTraceMiss's permutation 0, Please make sure cooking was successful.

So no ray tracing shaders were cooked; Lumen always runs in software. The add-on counts
acceleration structure builds and DispatchRays (Advanced tab): 0 at the title screen.
MegaLights (`r.MegaLights.EnableForProject=0`) is off for the same reason (it needs ray tracing).

## Probe

The add-on writes every relevant cvar with its value to `ReunionTweaks-probe.txt` (5 s and 60 s
after connecting, and from the Advanced tab). `docs/probe-title.txt` is the first run at the
title screen, with the menu at High for shadows, GI, view distance, foliage, shading and
landscape and Epic for the rest. Nothing changed between 5 s and 60 s.

The user raised Shadows and GI to Epic between runs. Values by menu tier (seen in the two runs):

| cvar | High (2) | Epic (3) |
|---|---|---|
| `r.Lumen.ScreenProbeGather.DownsampleFactor` | 32 | 16 |
| `r.Lumen.ScreenProbeGather.RadianceCache.NumProbesToTraceBudget` | 300 | 300 |
| `r.Lumen.TraceMeshSDFs.Allow` | 0 | 0 (global SDF only, at every tier) |
| `r.Shadow.Virtual.ResolutionLodBiasDirectional` / `...DirectionalMoving` | 0.5 / 1 | -0.5 / 0 |
| `r.Shadow.Virtual.ResolutionLodBiasLocal` / `...LocalMoving` | 0.25 / 1 | 0 / 0.5 |
| `r.Shadow.Virtual.SMRT.RayCount*` / `SamplesPerRay*` | 4 / 2 | 8 / 4 |
| `r.Shadow.DistanceScale` | 0.85 | 1 |
| `r.VolumetricFog.GridPixelSize` / `GridSizeZ` | 16 / 64 | 8 / 128 |
| `r.ViewDistanceScale`, `foliage.DensityScale` (ViewDistance/Foliage at High) | 0.8, 0.8 | - |

Fixed at every tier: `r.Shadow.Virtual.MaxPhysicalPages=4096`, `r.Streaming.PoolSize=1400`,
`r.MaxAnisotropy=8`, `r.SSS.Checkerboard=2`, hair 4 MSAA samples / 2048 deep shadow,
`r.CapsuleShadowsFullResolution=0`, `r.TSR.History.R11G11B10=1` (TSR history 200%, update
quality 3 already), `r.Lumen.TranslucencyReflections.FrontLayer.Enable=0`,
`r.Lumen.Reflections.MaxRoughnessToTrace=-1` (post-process volume decides), `r.FilmGrain=1`.
HDR: `r.HDR.Display.MaxLuminance=1000` follows the menu's `HDRDisplayOutputNits` (no patch
needed, unlike Directive 8020), `OutputDevice=3` (ST-2084), `ColorGamut=2` (Rec.2020),
`SceneColorMultiplier=1.5`, UI 300 nits.

Deck Nine's significance manager keeps characters one LOD below full outside cutscenes:
`D9.SigMan.MinLOD.Default.Cinematics=0`, `.Highest/.High/.Mid=1`; background people
(`Chumps`) `.Highest/.High=1`, `.Mid/.Low=2`.

## Options test

2026-10-25, every option at its highest (Cinematic preset plus all post-processing switches),
title screen: 42 values applied, 0 not found, 0 not taken, and the game didn't change any of
them back over 60 s. Costs and visuals not checked in a scene yet.
