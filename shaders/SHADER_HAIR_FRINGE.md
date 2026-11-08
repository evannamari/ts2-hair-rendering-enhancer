<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 evannamari. Covers the added lines described here and this text; see "License" at the end. -->

# Hair fringe pass: shader edits

Edited resources:

| Shader name     | Instance (high) | Instance |
| --------------- | --------------- | -------- |
| sims.matShad  | B8D92FFE        | FF703EE0 |
| DX9SimMaterials | 8852F2DC        | FFDA7307 |

**Goal.** Reproduce the default behaviour of the TS2 Hair Rendering Enhancer ASI with shader edits only. The solid part stays vanilla. Pixels that the vanilla shader discards (alpha <= threshold) are drawn in a second pass that does not write depth. Each pixel lands in exactly one of the two passes.

**Scope.** The second pass is created for every material that reaches `CreateSimStandardMaterialDX9` with `simHairFlag` true and `stdMatAlphaBlendMode` set to `blend`. In testing this includes accessories with alpha textures as well as hair. Which game code decides that a mesh takes the hair path has not been determined.

### File 1. sims.matShad

#### Change 1.1. New parameters (lines 373–376, after `setb simHairFlag false`)

```
# hair fringe pass (DX9): draw pixels with alpha <= hairSolidAlphaRef without depth write
# hairFringeEnabled false restores vanilla behaviour (hairSolidAlphaRef 60 is the vanilla alpha threshold)
setb hairFringeEnabled true
seti hairSolidAlphaRef 60
```

- `hairFringeEnabled` is the master switch. If `false`, the second pass is never created and hair renders as in vanilla.
- `hairSolidAlphaRef` is the alpha threshold. 60 is the vanilla value. It corresponds to `SolidAlphaRef` in the ASI.
- Variables in matshad are global, so the DX9 file can read them. They are read when the material is built; this works in practice (tested in game).

#### Change 1.2. `SimHairAlphaState`, blend branch (line 528)

**Before:**

```
alphaTest true 60
alphaTestFunction acceptIfGreater
```

**After:**

```
alphaTest true $hairSolidAlphaRef
alphaTestFunction acceptIfGreater
```

This is the solid pass: it accepts pixels with alpha above the threshold and writes depth. With 60 it is identical to vanilla. Moving the number into a variable keeps the solid and fringe thresholds in one place.

### File 2. DX9SimMaterials

Line numbers in this section refer to the vanilla-compatible version; in the CandK-compatible versions they are shifted. The line numbers for sims.matShad are the same in all versions.

In vanilla, `CreateSimStandardMaterialDX9` (line 187) contains one `pass … end #end the pass` block (original lines ~250–320) with fill mode, cube map variables, `NonStandardLighting`, the alpha-state selection, the vertex program `link`, the pixel program `assemble` and the samplers. The body is moved into a `define` so that it can be instantiated twice without duplicating ~70 lines.

#### Change 2.1. Call site (lines 250–262)

**Before:** the `pass … end #end the pass` block was inline.

**After:**

```
setb hairFringeActive false
if ($simHairFlag and $hairFringeEnabled and strcmp("${stdMatAlphaBlendMode}", "blend") = 0)
    setb hairFringeActive true
    seti hairFringeRef ($hairSolidAlphaRef + 1)
    # SimMaterialTransformSetupDX9 multiplies stdMatDiffCoef by simMaterialLightingScale: remember the original
    setv3 hairSavedDiffCoef (($stdMatDiffCoef).xyz)
endif

create SimStandardMainPassDX9(false)

if ($hairFringeActive)
    create SimStandardMainPassDX9(true)
endif
```

- `setb hairFringeActive false`: global variables persist from the previous material, so the flag is reset explicitly.
- The condition matches the branch in which `SimHairAlphaState` sets the reference value and depth write (blend mode, `simHairFlag` true). The ghost/shape-color case (hair fading when walls are cut) also enters here, because `SimMaterialSetup` forces blend mode in that case.
- `hairFringeRef = ref + 1`: the fringe uses `acceptIfLess`. For integer alpha, “alpha < 61” equals “alpha <= 60”. `acceptIfLessOrEqual` for the alpha test does not occur in the vanilla files, so it is not used.
- `hairSavedDiffCoef` stores `stdMatDiffCoef` before the first pass (see 2.2).
- `create SimStandardMainPassDX9(false)` is the ordinary pass for every material. With the flag active the same pass is created again with `true`, which is the fringe.

The closing lines of the shader (`end # end shader`, `endif`, `end`, `enddef`) now precede the new `define` in the file. A diff shows this as a deletion plus an addition; the structure is unchanged.

#### Change 2.2. New `define SimStandardMainPassDX9(isFringe)` (from line 277)

Inside is the old pass body unchanged, plus two insertions.

**Insertion A. Reset of global variables (lines 281–286):**

```
# fringe pass: undo what the main pass changed in global variables
if (&isFringe)
    seti perPixelLightCount 0
    seti totalLights ($distantLightCount + $pointLightCount + $spotLightCount)
    setv3 stdMatDiffCoef (($hairSavedDiffCoef).xyz)
endif
```

Building the first pass mutates global variables. Without restoring them, the second pass would differ from the first:

- `totalLights`: `StandardLightFragmentsDX9` sets it to 0 when there are four lights or fewer, and `SimMaterialTransformSetupDX9` selects the AmbientOnly lighting mode when it is 0. Without the recomputation the fringe would be lit by ambient only.
- `stdMatDiffCoef`: `SimMaterialTransformSetupDX9` multiplies it by `simMaterialLightingScale` (0.8) on every call. The second pass would get 0.64 instead of 0.8 and look darker than the solid part, so the saved value is restored.
- `perPixelLightCount`: the light fragments increment it, so it is reset to 0.

**Insertion B. Alpha state (lines 301–312).**

**Before:**

```
if ($simHairFlag)
   create SimHairAlphaState($stdMatAlphaBlendMode)
else
   create StandardShaderFBAlphaState($stdMatAlphaBlendMode)
endif
```

**After:**

```
if (&isFringe)
    # hair fringe: only pixels the solid pass rejected (alpha <= $hairSolidAlphaRef), no depth write
    alphaBlend srcFactor(srcAlpha) add dstFactor(invSrcAlpha)
    depthTest true -enableDepthWrite false
    depthTestFunction acceptIfLessOrEqual
    alphaTest true $hairFringeRef
    alphaTestFunction acceptIfLess
elseif ($simHairFlag)
   create SimHairAlphaState($stdMatAlphaBlendMode)
else
   create StandardShaderFBAlphaState($stdMatAlphaBlendMode)
endif
```

- `alphaBlend src/invSrc`: the same blend as the solid pass, so the pixel colour is identical.
- `depthTest true -enableDepthWrite false`: depth is tested but not written, so a semi-transparent edge no longer occupies depth for objects drawn later on layer +17. This removes halos and holes.
- `depthTestFunction acceptIfLessOrEqual`: the fringe lies on the same triangles as the solid pass, so their depths coincide. A strict “less” could reject pixels. This function is already used in vanilla files.
- `alphaTest … acceptIfLess`: takes exactly the pixels the solid pass rejected.

The vertex program, pixel program, samplers and fill mode are identical in both passes, so the colour output is identical. The `elseif` and `else` branches for other materials are unchanged.

### Changing the alpha reference

Edit one number in `sims.matShad`, line 376:

```
seti hairSolidAlphaRef 60
```

It is used in three places:

1. the solid pass: `alphaTest true $hairSolidAlphaRef` (`sims.matShad`, line 528);
2. the fringe threshold: `seti hairFringeRef ($hairSolidAlphaRef + 1)` (`DX9SimMaterials`, line 254);
3. the fringe pass state: `alphaTest true $hairFringeRef` (`DX9SimMaterials`, line 306).

As in the ASI, the solid pass takes alpha above the threshold and writes depth; the fringe takes alpha at or below it and does not. The higher the threshold, the fewer pixels write depth and the wider the soft edge.

Notes:

- Do not use 255. The fringe threshold would be 256, outside the alpha-test range, and the solid pass would accept nothing. The safe maximum is 254.
- 60 restores vanilla behaviour for the solid pass. To disable the fringe entirely, set `setb hairFringeEnabled false`.
- A per-material override via `addParam hairSolidAlphaRef <value>` should be possible but has not been tested for hair materials from packages. Editing the global value in `sims.matShad` is the reliable way.
- `SimHairAlphaState` is also used by the fixed-function path. Changing `hairSolidAlphaRef` changes the solid threshold there too, but no fringe pass exists on that path.

## License

The lines added by these edits and this document are released under GPL-3.0-or-later (see the `LICENSE` file in the repository root).
The shader files themselves (`sims.matShad`, `DX9SimMaterials`) are the property of Electronic Arts / Maxis and are not
covered by this license. This repository does not contain them; apply the edits to your own copy of the game files.
