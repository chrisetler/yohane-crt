# Yohane CRT: development notes

How the game renders, how the mod hooks in, and the tools used to work it out. For installing
and using the mod, see [README.md](README.md).

## The game

- Inti Creates' in-house engine, DirectX 11. `game.exe` also carries a DX9 path, which this mod
  doesn't handle.
- `Data/` is about 1,000 encrypted files under hashed names. There are no known tools to unpack
  them, and nothing there is edited.
- Shaders ship **precompiled** in that encrypted data. `game.exe` statically imports
  `D3DCompile` and `D3DReflect` from `D3DCOMPILER_43.dll` but never calls them in play.
- That import is the way in. `D3DCOMPILER_43.dll` isn't a KnownDLL, so a copy in the game
  folder loads first, before the game creates its D3D11 device.

Investigated on Steam build 12487807 with a 2560x1440 window.

## Rendering pipeline

Traced from frame captures (see the diagnostic build below). Per frame, in order:

1. **480x270 layers** (1080p ÷ 4). The main layer, with its own depth buffer, holds the
   **background only**: canopy, trunks, background scenery. A second 480x270 layer is drawn from
   the sprite sheet and read as the mask for the water caustics shader.
2. **Upscale**: one draw (`ps_e40e…`, sample × tint) scales the background 4× into the 1920x1080
   working image.
3. **The rest of the level, drawn directly at 1080p**: Yohane, enemies, effects, ground tiles and
   foreground scenery (`ps_88e6…` from the same sprite sheet, plus effect sprites `ps_affb…`).
   They're still 4× pixel art, but positioned on the 1080p grid.
4. **Water distortion**: `ps_6ab7…` reads the working image into a scratch target, then
   `ps_164fc6f26724bc56` copies it back. **This copy is where the CRT filter goes.** It runs
   in every level frame, including scenes with no water, and it's the last point where the image
   holds the whole level and nothing else.
5. **Glow**: downsample to 480x270 and on down to 60x34, blur each level, add back (`ps_1308…`).
6. **HUD, menus, dialogue boxes and full-screen cutscene art**, all drawn at 1080p
   (`ps_790d…`, `ps_d41d…`). In cutscenes the 480x270 layer is empty.
7. **Final scale** of the 1080p image to the window (`ps_e40e…` again).

Filtering at step 2 would only reach the background. `ps_e40e` is shared by the upscale, the glow
downsamples and the final scale, so it can't be swapped by shader alone. `ps_164f…` is used
only for the step 4 copy, so the mod swaps it whenever the game binds it.

The CRT filter therefore runs at 1080p, and the game stretches the result to the window. At
1440p that 1.33× stretch softens fine phosphor masks. Running the filter at window resolution
would mean moving it to step 7 and drawing the HUD (step 6) into a separate layer, which this
version doesn't attempt.

A 1280x2160 target and a backbuffer copy at the start of each frame aren't part of the game's
pipeline. They appear to be Steam's game recording.

## Menu-style scenes

Some scenes are pixel art drawn entirely by the UI shader (`ps_790d…`) in step 6, with no level
layer and no copy-back, so the swap above never fires. The fortune parlor is one: per-draw
captures (Shift+F11) show the room background as one full-screen draw, then Yohane, then
Lailaps, then the menu panel and text, with the first three and the panel all from the same
2048×2048 sheet. The room art is on the same 4× grid as the levels (every colour run a multiple of
4 pixels, all edges at x and y ≡ 0 mod 4), so the same filters work unchanged.

For these, `kScenes` in `shaderlog.cpp` lists each scene's sheet by a fingerprint of its contents
(FNV-1a over a 256×64 patch of mip 0 at width/4, height/8), plus how many draws from it come
before the menu. The fingerprint is read back from the GPU once per texture, and only for
textures of a listed size. After that many draws into the current target in a frame, the DLL
copies the target, draws the selected filter over it with its own full-screen vertex shader
(depth off, blending off), and restores the game's shaders, input layout, topology, PS slot 0,
blend, depth-stencil and rasterizer state. The menu is then drawn on top, unfiltered.

| Scene | Sheet | Fingerprint | Draws before menu |
|---|---|---|---|
| Fortune parlor | 2048×2048 | `e5dab5b2e840e42d` | 3 (room, Yohane, Lailaps) |

Adding a scene: take a Shift+F11 capture in it, find the sheet the art comes from among the
`input_*` snapshots, count the draws before the first menu element in the `after_draw_*`
snapshots, and compute the fingerprint from the sheet's snapshot (the diagnostic build also logs
`texture WxH fingerprint …` for every texture of a listed size).

Known limits: the count assumes the scene always draws the same number of things before its menu.
The death screen also skips the copy-back on some frames; it's a single sprite and isn't
handled.

## The DLL (`shaderlog/shaderlog.cpp`)

On load, it patches game.exe's import of `D3D11CreateDevice`, then hooks the device it returns
(`CreatePixelShader`), the immediate context (`PSSetShader`) and the swap chain (`Present`, for
the keys). The two compiler exports are forwarded to the real System32 DLL.

It also hooks the context's draw calls, `PSSetShaderResources`, `OMSetRenderTargets` and the
device's `CreateTexture2D`, for the menu-scene pass described above.

When the game creates a pixel shader whose bytecode hashes (FNV-1a 64) to
`ps_164fc6f26724bc56`, the DLL compiles every `<game>\crt\*.hlsl` with `d3dcompiler_47` and
substitutes the selected one in `PSSetShader` whenever the game binds that shader. A game update
that changes that shader means no match. The release build then logs a warning after about
10 seconds and does nothing else.

**vtable note:** the D3D11 runtime keeps the immediate context's vtable in writable memory and
swaps Draw/Copy/Clear entries for different implementations as pipeline state changes, which
silently undoes plain vtable hooks. `Present` re-applies the hooks every frame, and during a
diagnostic capture every hook re-applies them all after each call. A hook always calls whatever
function the runtime most recently put in its slot.

### Builds

`shaderlog\build.bat` (Visual Studio 2022, x64) produces both:

- **Release** (`release\D3DCOMPILER_43.dll`, built with `/DCRT_RELEASE`): the CRT swap and its
  keys only. Logs to `<game>\crt\log.txt`, rewritten each launch.
- **Diagnostic** (`D3DCOMPILER_43.dll`): also writes everything below to `<game>\shaderlog\`.

| Diagnostic output | Contents |
|---|---|
| `log.txt` | Hook status, swap chain size, every render target created (size, format), every shader as it's created |
| `shaders\vs_<hash>.dxbc`, `ps_<hash>.dxbc` | Each shader's bytecode as handed to the GPU (already decrypted) |
| `shaders\*.asm` | Disassembly of each, including resource names where the shader wasn't stripped |
| `frame_NN.txt` | **F10**: one full frame in order: render-target switches, clears, copies, and every draw with its shaders, target, viewport and input textures. Identical runs collapse to `(xN)`. Textures get `tex#N` ids, which are stable within a session but not across sessions |
| `snap_NN_SS_*.bmp` | **F11**: the same trace plus a BMP of each render target at the moment the game switches away from it. A second-long hitch and ~100 MB per press |
| `snap_NN_SS_after_draw_*.bmp`, `…input_*.bmp` | **Shift+F11**: also the full-size target after every draw (up to 120) and each input texture the first time it's used |

Test hooks, read from the environment at load: `SHADERLOG_ARM=1` captures the first frame
without a key press, and `SHADERLOG_ARM=draws` does so like Shift+F11 (diagnostic build only). `SHADERLOG_CRT_TARGET=ps_<hash>` swaps a different
shader, for testing against a harness.

## Filters (`crt/`)

Every `.hlsl` in `<game>\crt\` is a filter option, ordered `crt.hlsl` first, then alphabetical.
F7 cycles, F8 recompiles all (a file that fails keeps its previous version), F9 toggles. The
choice is saved to `<game>\crt\selected.txt`. With no saved choice, `crt-geom.hlsl` is selected
(`kDefaultCrt`).

A filter is a `ps_5_0` pixel shader with entry point `main`, and its input struct must match the
game's vertex shader output, register for register:

```hlsl
struct PSIn {
    float4 pos : SV_Position;
    float4 c0  : COLOR0;
    float4 c1  : COLOR1;
    float2 uv  : TEXCOORD0;
};
```

The finished 1080p level image is bound at `t0`. Output alpha should be passed through from `t0`,
as the copy it replaces does.

- `crt.hlsl`, **Basic**: written for this mod. It rebuilds each 480x270 pixel from the middle 2x2
  of its 4x4 block and draws Gaussian scanlines with brightness-dependent width.
- `crt-geom.hlsl`: crt-geom with curvature off and `lum` 0.3, `monitorgamma` 2.4 and `DOTMASK` 0.2
  for brightness. It starts with an **adapter** that presents the 1080p frame as the 480x270
  `Source` texture RetroArch shaders expect (each texel is the average of its 4x4 block) and maps
  GLSL names to HLSL. To add another RetroArch shader, copy the adapter and paste a port below it,
  renaming the port's `main`.

`crt/crtpreview.cpp` renders a filter over a captured 1920x1080 snapshot on the software
rasteriser (WARP) and writes a BMP, for trying settings without the game:

    crtpreview crt.hlsl snap_01_09_....bmp out.bmp

## RetroArch ports (`crt/compare/`)

HLSL ports of crt-lottes, crt-easymode, crt-gdv-mini-ultra (guest) and crt-geom, from the
`crt/shaders/` folder of RetroArch's [slang-shaders](https://github.com/libretro/slang-shaders).
They keep their shipped defaults except for curvature, which is off. `common.hlsli` supplies the
bindings and the GLSL-to-HLSL mappings. These ports read a real 480x270 texture, unlike the
in-game adapter. `crtcompare.cpp` runs one over a BMP at any output size:

    crtcompare common.hlsli crt-lottes.hlsl level480.bmp out.bmp 2560 1440 [NAME=VALUE ...]

Make the 480x270 input from an F11 snapshot with ffmpeg: `-vf scale=480:270:flags=area -pix_fmt bgra`.

## Testing

`tests/` holds two WARP harnesses that load the DLL the same way the game does: one for the level
path, one for the parlor scene pass. Run both after any change to the hooks.
[tests/README.md](tests/README.md) has the build commands, the environment settings that point the
filter at the harness's shaders, and the expected output.

## Packaging

`package.ps1` runs the build and assembles `dist\YohaneCRT-<version>.zip`: the release DLL, the
two filters, `YohaneCRT-README.txt` (from `dist-readme.txt`, with the version, URL and DLL hash
filled in), `YohaneCRT-LICENSE.txt` and the full source in `YohaneCRT-source\`. It also writes
`dist\SHA256SUMS.txt`. The version comes from `CRT_VERSION` in `shaderlog.cpp`.

    powershell -ExecutionPolicy Bypass -File package.ps1 -Url https://github.com/chrisetler/yohane-crt

The DLL's hash changes on every build, so always publish the zip and `SHA256SUMS.txt` from the
same run.

## Releasing

Where the mod is published:

| Place | Link | Notes |
|---|---|---|
| GitHub | https://github.com/chrisetler/yohane-crt | Source and releases (v1.0.0 on 2026-11-07) |
| Nexus Mods | game id 10985 | The game was added to Nexus by this mod's upload; it was awaiting admin approval as of 2026-11-07. The header banner is `screenshots/nexus-banner.png` (1300x372) |
| PCGamingWiki | [Yohane the Parhelion: Blaze in the Deepblue](https://www.pcgamingwiki.com/wiki/Yohane_the_Parhelion:_Blaze_in_the_Deepblue) | A `===Yohane CRT===` entry under *Other information*, after *Middleware*, with an install `Fixbox` |

For a new version:

1. Bump `CRT_VERSION` in `shaderlog/shaderlog.cpp` and update the tested Steam build in
   `README.md` and `dist-readme.txt` if it changed. The current build ID is in
   `steamapps/appmanifest_2404370.acf`.
2. Run the harnesses (see Testing), then test the release DLL in the game: F7, F9, a level and the
   parlor.
3. Commit and push.
4. Run `package.ps1 -Url https://github.com/chrisetler/yohane-crt`.
5. `gh release create v<version> dist/YohaneCRT-<version>.zip dist/SHA256SUMS.txt --title "Yohane CRT <version>"`
   with notes covering what changed, install and the DLL hash. Then download the zip back and
   check its hash against `SHA256SUMS.txt`.
6. Upload the same zip to Nexus as a new file version, and update the PCGamingWiki entry if the
   install steps changed.

This folder is on a network share, so git needs a `safe.directory` entry for it (already in the
global git config on the dev PC). Commits use the GitHub no-reply address, set in this repo's
local config.

**If a game update breaks it:** the release log says the copy-back shader never appeared. Install
the diagnostic build, take F10/F11 captures in a level, and find the new hash of the copy-back
shader (the full-screen draw after the water pass, before the glow chain). Update `g_copyBackPS`.
Check the parlor fingerprint too, with a Shift+F11 capture there.
