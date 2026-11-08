# TS2 Hair Rendering Enhancer - shader version

Shader-only version of [TS2 Hair Rendering Enhancer](..): shows the fine alpha detail of hair textures in The Sims 2 without introducing new visual artifacts where hair intersects other alpha-blended objects. It reproduces the default behaviour of the ASI plugin by adding a second pass to the hair path of `CreateSimStandardMaterialDX9` (`DX9SimMaterials`, DirectX 9 path only) and two parameters in `sims.matShad`.

- `SHADER_HAIR_FRINGE.md` explains every change line by line and how to change the alpha threshold.
- `hair_fringe_pass.patch` contains the same changes as a patch.

**Do not use this together with the ASI plugin**: the soft edge would be drawn twice.

## License

The added lines and the documentation are GPL-3.0-or-later (see the `LICENSE` file in the repository root). The shader files being edited belong to
Electronic Arts / Maxis and are not included here and not covered by this license.
This is an unofficial fan project, not affiliated with or endorsed by Electronic Arts or Maxis.
