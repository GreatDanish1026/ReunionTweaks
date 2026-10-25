# ReunionTweaks 0.1.0

Graphics add-on for **Life is Strange: Reunion** (PC, Steam), for ReShade with add-on support.
Raises the game's lighting, shadow, character and detail settings beyond what its menu offers,
live, from an in-game overlay.

- **Lighting** (software Lumen): bounce light traced against each object instead of a coarse
  whole-scene approximation, finer lighting probes, reflections on rougher surfaces and on
  glass and water.
- **Shadows**: sharper lamp and character shadows, smoother soft shadows, longer shadow
  distance, full-resolution character soft shadows.
- **Characters**: cutscene-quality character models in gameplay (the game drops a detail level
  outside cutscenes), full-resolution skin shading, higher-quality strand hair.
- **Detail**: texture memory beyond the game's fixed 1.4 GB, 16x anisotropic filtering, draw
  distance, finer geometry, finer volumetric fog, higher-precision TSR history.
- **Post-processing**: switches for film grain, lens flares, depth of field and chromatic
  aberration.
- **Quality presets**: *Ultra+* (everything beyond Epic that costs little; about 15% slower)
  and *Cinematic* (everything at its highest; about 2.4x the frame time). Measured on an RTX
  5080 at 4K with 65% resolution scale in one scene. **Measure preset costs** runs each one
  for a few seconds and shows its frame rate on your PC; **Measure each option** ranks every
  option by cost.
- **F8** switches every tweak off and on, to compare.

The game has no hardware ray tracing (its ray tracing shaders weren't shipped), so nothing here
uses it. Every option starts at the game's own value, and its tooltip says what the menu uses.
Set the game's own graphics menu to Epic first; the options go beyond it.

## Requirements

- Life is Strange: Reunion for PC (Steam build 22706033 tested). If the game's code doesn't
  match, the add-on changes nothing and says so in the overlay and in `ReunionTweaks.log`.
- [ReShade](https://reshade.me) 6.8 or newer **with full add-on support**, installed for
  `Iris-Win64-Shipping.exe` with DirectX 10/11/12.

## Install

1. Install ReShade with add-on support into `...\LifeisStrangeReunion\Iris\Binaries\Win64`.
2. Copy `ReunionTweaks.addon64` into the same folder.
3. Start the game and press **Home** to open ReShade. The settings are on the **Add-ons** tab,
   under ReunionTweaks.

Settings are saved in `ReShade.ini` under `[ReunionTweaks]`. Nothing else is written to disk
(apart from the log and the optional settings dump) and no game code is patched; deleting the
`.addon64` file removes the mod completely.

## Building

`.\build.ps1` builds `bin\ReunionTweaks.addon64` (needs Visual Studio 2022 with the C++ tools).
`.\build.ps1 -Install` also copies it into the game folder (close the game first). See
`docs/FINDINGS.md` for how the game's settings were found.

## License

MIT (see `LICENSE`). Third-party headers: see `THIRD_PARTY_NOTICES.txt`.
