# Test harnesses

Small D3D11 programs that exercise the DLL on Microsoft's software renderer (WARP), so a change
can be checked before it goes near the game. Each one statically imports `D3DCOMPILER_43.dll`
(the mod) and `d3d11.dll`, just as `game.exe` does, so the DLL hooks them the same way.

| Harness | What it checks |
|---|---|
| `level_harness.cpp` | The level path: a low-res target upscaled into a 1280x720 one. Frame capture, snapshots and the copy-back swap. It also prints which module owns key context vtable slots before and after rendering, which is how the runtime's slot swapping was found |
| `parlor_harness.cpp` | The menu-scene pass: four draws from the real parlor texture sheet into 1920x1080, with the pass expected after draw 3 |

## Building

Run from a "x64 Native Tools" prompt (or after `vcvars64.bat`), in a scratch folder holding
copies of `shaderlog.cpp`, `shaderlog.def` and the harness:

    cl /O2 /EHsc /MT /LD shaderlog.cpp /Fe:D3DCOMPILER_43.dll user32.lib /link /DEF:shaderlog.def
    cl /EHsc /D_CRT_SECURE_NO_WARNINGS level_harness.cpp user32.lib
    cl /EHsc /D_CRT_SECURE_NO_WARNINGS parlor_harness.cpp user32.lib

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

With the release build, look for the same `CRT:` lines in `crt\log.txt`.
