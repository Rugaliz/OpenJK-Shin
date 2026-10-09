# Screen effects and shader lighting (single player)

The single player renderer (`code/rd-vanilla`) is the original fixed function OpenGL 1.x one. It can now also run
GLSL shader programs where that is useful, and every use keeps the original way of drawing as its fallback: if the
graphics card or driver does not support what is needed, or a shader does not build, the old drawing is used and
nothing else changes. All of it is off by default, nothing changes unless you turn it on. Type the settings in the
console (the key left of 1, or Shift+Esc) or put them in your config file; they are saved.

`r_glsl 0` (then `vid_restart`) turns all of it off. The start-up log says what was found: `...using GLSL`,
`...using post processing`.

## The screen effects

They run on the finished 3D view, before the HUD, menus and text are drawn, so those stay crisp. They do not run on
the model views of the menus, on portals or on the sky portal. The weapon in front of the player and the sky are left
out of the occlusion. Needs OpenGL 2.0 and framebuffer objects (every graphics card of the last fifteen years).

| Setting | Default | What it does |
|---|---|---|
| `r_ssao` | 0 | **Ambient occlusion.** Darkens corners, crevices and the places where things meet, as light would be blocked there. The original lighting is baked in the maps and knows nothing of that, so this is the effect that adds the most depth. It stays soft and does not change the colours. |
| `r_ssaoRadius` | 48 | How far from a pixel, in game units, a surface still shades it. |
| `r_ssaoStrength` | 2 | How dark. 1 is weak, 3 is strong. |
| `r_bloom` | 0 | **Bloom.** A soft glow around the brightest parts of the picture: lights, glowing panels, explosions. Bright areas are not clipped to white. |
| `r_bloomThreshold` | 0.8 | How bright (0 to 1) a pixel has to be to glow. Lower makes more of the picture glow. |
| `r_bloomIntensity` | 0.5 | How strong the glow is. |
| `r_smaa` | 0 | **SMAA**, a high quality edge smoothing that works on what the multisampling (the Anti-Aliasing setting) leaves behind, like the edges that textures and shaders make. Unlike FXAA it only touches the edges it finds, so textures stay as sharp as they are. Needs OpenGL 3.3. Use it with or without multisampling. |
| `r_postDebug` | 0 | For testing: 1 runs the effects' plumbing with nothing in it (the picture must not change), 2 shows the occlusion alone. |

The effects run one after the other: occlusion, bloom, then SMAA. Cost on a Radeon RX 7800 XT at 3440x1440 with 8x
multisampling: not measurable (about 135 fps with and without, with a noisy counter).

## Shader dynamic lights (`r_dlightGLSL`, default 0)

The light of lightsabers, blaster bolts, explosions and so on (the "dynamic lights") is drawn as a texture projected
onto every triangle. With `r_dlightGLSL 1` it is a shader instead: every pixel is lit by its own distance to the
light and by the angle between its surface and the light, with the surface normals smoothed across triangles. On
flat walls this looks very close to the original; the difference is on curved surfaces and characters, where the
light no longer shows the triangles. The old look is one setting away, and also used when the shader does not build.

## Credits

SMAA is Subpixel Morphological Antialiasing by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and
Diego Gutierrez (https://github.com/iryoku/smaa). Its shader code and lookup textures are used as they published
them, under the MIT license (the notice is in `code/rd-vanilla/tr_smaa_data.h`).
