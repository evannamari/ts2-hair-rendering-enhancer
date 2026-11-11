# TS2 Hair Rendering Enhancer

Shows the fine alpha detail of hair textures in The Sims 2 without introducing new visual artifacts where hair intersects other alpha-blended objects.

In the original game, hair pixels with an alpha value (opacity) of 60 or less out of 255 (about 24%) are discarded entirely, so strand edges look cut out and jagged. This project draws those faint pixels back as a soft edge that does not write depth. At intersections with other alpha-blended objects (glasses, windows, jewelry) the result therefore matches the original game: simply lowering the alpha test threshold would create new transparency halos there, and this does not. Textures and meshes are not modified.

## Two versions, same default result

|                   | ASI plugin                                                        | Shader version                                                                                                |
| ----------------- | ----------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------- |
| Folder            | [`asi`](asi)                                                      | [`shaders`](shaders)                                                                                          |
| How it works      | Hooks Direct3D 9 and draws each hair draw call in two passes      | Adds a second pass to the game's own hair shader. DirectX 9 shader path                                       |
| Needs             | An ASI loader or similar DLL injector                             | A modified shader package (shared separately, see the link below); the `shaders` folder documents the changes |
| Works in Bodyshop | No                                                                | Yes                                                                                                           |
| Game versions     | Ultimate Collection or any Windows build except Legacy Collection | All Windows builds, including the Legacy Collection                                                           |
| Settings          | `.ini` file, hotkeys, log                                         | Two values in the shader                                                                                      |

Use **one** of them, not both: the soft edge would be drawn twice.

ASI downloads are on the [Releases](../../releases) page. The shader package is shared separately: https://evannamari.tumblr.com/post/833020029518921728/ts2-enhanced-hair-shaders.

## Status

ASI plugin: version 0.5.0. Shader version: version 0.1.0. Both are experimental. Feedback and test reports are welcome.

## Credits

- **evannamari**: idea, design decisions, testing in game, release.
- **Claude** (AI assistant by Anthropic): code and documentation written with AI assistance.

## License

GPL-3.0-or-later, see [LICENSE](LICENSE). For the shader version the license covers only the added lines and the
documentation; the shader files of the game belong to Electronic Arts / Maxis and are not included.

This is an unofficial fan project. It is not affiliated with or endorsed by Electronic Arts or Maxis.
*The Sims* is a trademark of Electronic Arts Inc.
