# GxBake asset baking

Dolphin converts a game folder's source assets into the GameCube-native formats of the Xash3D
GameCube port with [GxBake](https://github.com/timfox/gxbake), as the folder is loaded. Nothing has
to be run by hand: put sources in `assets/` and boot the folder.

```
mygame/
  sys/main.dol             the folder game, as before
  files/valve/...          files that go on the disc unchanged
  assets/valve/...         GxBake sources; their outputs appear under files/valve/
```

Each top-level folder of `assets/` is a gamedir, converted the way `gxbake convert assets/valve -o
files/valve` would convert it. The outputs are added to the virtual disc (an output replaces a file of
the same name in `files/`). Sources are not put on the disc, except:

- GoldSrc runtime files (`.bsp`, `.wad`, `.mdl`, `.spr`) stay on the disc, and their packs are added;
- `.wav` files under a `sound/` folder stay as they are (game sound effects); other `.wav` files
  become `media/<name>.gcpcm`;
- files GxBake does not read (configs, scripts and so on) go on the disc as they are.

## What is converted where

Converted inside Dolphin (C++ ports in `Source/Core/GxBake`, byte-identical to the Python tool;
`Source/UnitTests/Core/GxBakeTest.cpp` checks reference hashes):

| Source | Output |
| --- | --- |
| PNG, TGA (8/24/32-bit), BMP (8/24/32-bit) | one entry each in `media/textures.gct` (CMPR, power of two, mips, 1-bit alpha) |
| six `<name>{rt,bk,lf,ft,up,dn}` images | `media/sky_<name><side>.cmp` |
| PCM WAV (8/16/24/32-bit) | `media/<name>.gcpcm` |
| Half-Life sprite (`.spr`, v2) | `sprites/<name>.spr`, halved (alpha-tested strokes kept); multi-frame sprites copied |
| WAD3 (`.wad`) | `media/<name>.gct`: every miptex, HUD picture and font |
| GoldSrc map (`.bsp`, v30) | `media/wt_<map>.gct` from the map and its WADs, plus `media/sky_<sky><side>.cmp` from `gfx/env/` |
| GoldSrc studio model (`.mdl`) | `gc_studio/<name>.mdl` (mesh only) and `gc_studio/<name>t.mdl` (skins, 64 px max) |
| Quake sprite (`.spr`, v1), Quake II sprite (`.sp2`) | `sprites/<name>.spr`: a Half-Life (v2) sprite, halved like the others |
| Quake WAD2 (`.wad`) | `media/<name>.gct`: every miptex, coloured by Quake's palette |
| Quake map (`.bsp`, v29) | `media/wt_<map>.gct` from the map's own textures |
| Quake alias model (`.mdl`, IDPO) | `models/<name>.mdl` (studio model of the first frame and skin), plus the two `gc_studio/` files |
| Source model (`.mdl`, IDST v44-49, with its `.vvd` and `.vtx`) | `models/<name>.mdl` (bones, LOD 0, skin family 0, the sequences stored in the `.mdl`, helper bones for blended weights), plus the two `gc_studio/` files |

GoldSrc files also go on the disc as they are. A map's WADs are found like the Python tool finds
them: every WAD in the map's gamedir, then the map's `"wad"` list, `halflife.wad`,
`hlbasics.wad`, `decals.wad` and `dod_*.wad` from the gamedir's `dod`/`valve` siblings and the
`DODGC_FULL_DOD`, `DODGC_WT_WAD_ROOT` and `HL_VALVE_DIR` folders. A map is baked again when any of
those WADs or its sky images change. Quake files use Quake's palette, `gfx/palette.lmp` found
loose or inside a `.pak` in the file's folder or up to five folders above it (or their `id1/`);
without one they are baked in greyscale with a warning. A Quake II sprite's frames are read from
the PCX files it names (or their `baseq2/` copies). A Source model's skins are found through its
`.vmt` files (`$basetexture`) under the nearest `materials/` folder, and the model is baked again
when its `.vvd`, `.vtx`, `.vmt` or `.vtf` files change. The CMPR cache (`python-cmpr/` in the cache folder) uses the
Python tool's keys and layout, so both share it.

Everything else GxBake reads (JPEG and other image formats, panoramas and cubemap crosses, other
3D model formats, Source maps and textures, Source 2 assets, compressed audio,
video) is handed to the GxBake
Python tool, one call per folder, when it is installed (`pip install git+https://github.com/timfox/gxbake`, or point
`PythonCommand` at it). Its outputs are merged the same way; images still go into the one
`media/textures.gct`, in GxBake's order.

The Python tool resamples WAVs with ffmpeg when ffmpeg is installed; Dolphin always uses GxBake's
ffmpeg-free path (linear resampling), so for WAVs that are not 48 kHz the two can differ slightly.

## Caching and live updates

Outputs are cached in `User/Cache/GxBake/`, keyed by each source's size and modification time,
so only changed sources are converted again. While the game runs, Dolphin watches `assets/` and
re-bakes once a second after a change. An output whose size stays the same (a texture redrawn at
the same size, a sky side, a sound of the same length) is replaced in place and the game reads the
new data next time it loads the file. An output whose size changes waits for the next boot, since
the disc's file table cannot change while the game runs.

`dolphin-tool bake -i mygame -o staged` runs the same bake and copies the resulting tree out, for
building a disc image or checking the output.

## Settings

In `Dolphin.ini`:

```ini
[GxBake]
Enabled = True          ; bake assets/ when a folder game is loaded
Live = True             ; re-bake while the game runs
PythonFallback = True   ; use the Python tool for the other formats
PythonCommand =         ; e.g. "python3 -m gxbake"; empty finds gxbake on PATH
```
