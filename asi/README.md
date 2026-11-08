# TS2 Hair Rendering Enhancer

An ASI plugin that shows the fine alpha detail of hair textures in **The Sims 2** (Direct3D 9) without introducing new visual artifacts where hair intersects other alpha-blended objects.

In the original game, hair pixels with an alpha value (opacity) of 60 or less out of 255 (about 24%) are discarded entirely, so strand edges look cut out and jagged. This project draws those faint pixels back as a soft edge that does not write depth. At intersections with other alpha-blended objects (glasses, windows, jewelry) the result therefore matches the original game: simply lowering the alpha test threshold would create new transparency halos there, and this does not. Textures and meshes are not modified.

Works only with game builds made before the Legacy Collection recompilation. For the Legacy Collection use the shader version.

**Status:** version 0.5.0, experimental. Feedback and test reports are welcome.

## How it works

1. The plugin hooks `IDirect3D9::CreateDevice`, then `Present` and `DrawIndexedPrimitive` of the created device.
2. A draw call is treated as vanilla hair when its render state matches the hair shader: alpha blending with
   `srcAlpha / invSrcAlpha`, depth test and depth write on, alpha test `GREATER` with reference `60`.
3. Each matching draw is issued twice over the same triangles (default `Mode=1`):
   - **solid pass**: alpha above `SolidAlphaRef`, depth write on (same footprint as vanilla);
   - **fringe pass**: alpha at or below `SolidAlphaRef`, depth write off, so soft edges never write depth.
4. Optional triangle sorting (`SortTriangles=1`, off by default) reorders the triangles of a draw far to near.
   For this it replicates the game's vertex-shader skinning on the CPU: the bone palette is read from
   vertex shader constants `c56..c235` (3 registers per bone: row 1, row 2, translation; row 3 is
   `cross(row1, row2)`; the 4th blend weight is `1 - w0 - w1 - w2`), and the depth key is the clip-space `w`
   from the 4th row of the camera-to-clip matrix in `c0..c3`. See `src/TS2HairRenderingEnhancer.h`.

Sorting is off by default because the solid pass writes depth, so a wrong order can cut holes into strands.

## Installation

Depending on which ASI loader you use:

- **Ultimate ASI Loader** ([GitHub](https://github.com/ThirteenAG/Ultimate-ASI-Loader)) or **TS2 Extender** ([GitHub](https://github.com/LazyDuchess/TS2-Extender)): Place `TS2HairRenderingEnhancer.asi` and `TS2HairRenderingEnhancer.ini` in the `TSBin` folder of your latest installed expansion pack, next to `dsound.dll`.
    
- **Sims2RPC** ([GitHub](https://github.com/LazyDuchess/Sims2RPC.com)): Place `TS2HairRenderingEnhancer.asi` and `TS2HairRenderingEnhancer.ini` in the `TSBin\mods` folder of your latest installed expansion pack.
    

The plugin writes `TS2HairRenderingEnhancer.log` next to the `.asi` file.

In game (Ctrl+Shift+): **F8** mod on/off, **F7** sorting on/off, **F6** mode 0/1.
All settings are explained in `TS2HairRenderingEnhancer.ini`.

## Building

The plugin is a 32-bit (x86) DLL renamed to `.asi`. It uses only the Windows and Direct3D 9 headers and
the C++ standard library (C++11 or newer).

Tested build (MinGW-w64 cross compiler, static runtime), run from this folder:

```
i686-w64-mingw32-g++ -O2 -shared -static -static-libgcc -static-libstdc++ -o TS2HairRenderingEnhancer.asi src/TS2HairRenderingEnhancer.cpp -Wl,--kill-at
```

Other x86 compilers (for example MSVC) have not been tested.

## Shader-only version

The same result can be produced by editing the game's shaders instead: see [`../shaders`](../shaders).
**Do not use the ASI and the shader version together**: the soft edge would be drawn twice.

## Research use

The code is meant to be readable and reusable for research into the Sims 2 renderer. The core in
`src/TS2HairRenderingEnhancer.h` has no D3D or Windows dependencies and can be tested on its own.

## License

GPL-3.0-or-later. See [LICENSE](../LICENSE).

This is an unofficial fan project. It is not affiliated with or endorsed by Electronic Arts or Maxis.
*The Sims* is a trademark of Electronic Arts Inc. No game files or game code are included in this repository.
