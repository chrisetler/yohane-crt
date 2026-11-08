# Test harnesses

Small D3D11 programs that exercise the DLL on Microsoft's software renderer (WARP), so a change
can be checked before it goes near the game. Each one statically imports `D3DCOMPILER_43.dll`
(the mod) and `d3d11.dll`, just as `game.exe` does, so the DLL hooks them the same way.

| Harness | What it checks |
|---|---|
| `level_harness.cpp` | The level path: a low-res target upscaled into a 1280x720 one. Frame capture, snapshots and the copy-back swap. It also prints which module owns key context vtable slots before and after rendering, which is how the runtime's slot swapping was found |
| `parlor_harness.cpp` | The menu-scene pass: four draws from the real parlor texture sheet into 1920x1080, with the pass expected after draw 3 |
| `hdr_harness.cpp` | HDR output: the swap chain comes back FP16 while the game's working image stays 8-bit, the filter is redrawn into the highlight layer, the final scale is found, and the final pass adds highlights only where the HUD hasn't covered the filter |

## Building

Run from a "x64 Native Tools" prompt (or after `vcvars64.bat`), in a scratch folder holding
copies of `shaderlog.cpp`, `shaderlog.def` and the harness:

    cl /O2 /EHsc /MT /LD shaderlog.cpp /Fe:D3DCOMPILER_43.dll user32.lib /link /DEF:shaderlog.def
    cl /EHsc /D_CRT_SECURE_NO_WARNINGS level_harness.cpp user32.lib
    cl /EHsc /D_CRT_SECURE_NO_WARNINGS parlor_harness.cpp user32.lib
    cl /EHsc hdr_harness.cpp user32.lib

Add `/DCRT_RELEASE` to the first line to test the release build. Keep the `D3DCOMPILER_43.lib`
import library the first line produces, since the harnesses link against it (`build.bat`
deletes it). Put a `crt\` folder with the filter `.hlsl` files next to the executables.

## Running

The DLL only loads the filters once it sees its target shader, so point it at a shader the
harness creates. These hashes are stable as long as the shader source in the harness doesn't
change:

    set SHADERLOG_CRT_TARGET=ps_d605bb2d5ca58381   & rem level harness (its own pixel shader)
    set SHADERLOG_CRT_TARGET=ps_75a0b984e0125f58   & rem parlor harness (an unused dummy shader)
    set SHADERLOG_ARM=draws                        & rem diagnostic build: capture frame 1 like Shift+F11

Expected results:

- **Level harness:** in `shaderlog\frame_01.txt`, the harness's draws are marked `->CRT`.
- **Parlor harness:** `atlas.bmp` must be next to the executable. That's the
  `snap_NN_SS_input_tex_N_2048x2048_f87.bmp` from a Shift+F11 capture in the parlor; it's game art,
  so it isn't in the repo. The log should show `texture 2048x2048 fingerprint e5dab5b2e840e42d =
  fortune parlor` and `CRT: filtering scene "fortune parlor"`, and the trace should show
  `SCENE PASS (fortune parlor)` after the third draw. The `after_draw_*` snapshot taken right after
  the pass shows the filtered image. The fourth draw then covers it, because this harness draws
  full-screen every time.

- **HDR harness:** needs `crt\hdr.ini` with `enabled=1`, `paper_white=200`, `peak=1000`,
  `crt_boost=1.5`, and `tests\hdr_test.hlsl` copied in as the only filter (`crt\test.hlsl`). Run it
  once to print its copy shader's hash, then again with `SHADERLOG_CRT_TARGET` set to it
  (currently `ps_d605bb2d5ca58381`, the same as the level harness). Expected output: back buffer
  format `10` (FP16), working image format `87` (8-bit) holding `255 128 255 255`, and for both
  buffers `HUD side: 0.000 2.500 0.000   filter side: 5.520 0.548 2.500`. That is: the HUD half
  at paper white with no highlight, and on the filter half red at 1.5x white rolled off toward
  the 1000-nit peak. The log should show
  `HDR: highlight layer 64x64`, `filter writing the highlight layer` and `highlights on
  (final scale viewport 128x128 ...)`.

With the release build, look for the same `CRT:` lines in `crt\log.txt`.
