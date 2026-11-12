# Screen effects and shader lighting

The renderers of all four games (single player `code/rd-vanilla` and its Vulkan version, `docs/vulkan-renderer.md`,
and the multiplayer one, `codemp/rd-vanilla`, for Jedi Academy and Jedi Outcast) are the original fixed function
OpenGL 1.x ones. They can now also run
GLSL shader programs where that is useful, and every use keeps the original way of drawing as its fallback: if the
graphics card or driver does not support what is needed, or a shader does not build, the old drawing is used and
nothing else changes. All of it is off by default, nothing changes unless you turn it on. The menu is the easy way; the settings can also be typed in the
console (the key left of 1, or Shift+Esc) or put in your config file, and they are saved.

You do not need the console: **Setup > More Video** has a row for each of them (Ambient Occlusion, Bloom, SMAA Edge
Smoothing, Shader Dynamic Lights, Light Shafts, Eye Adaptation), which take effect at once. A row only shows up if what it needs is there
(SMAA needs OpenGL 3.3, the rest OpenGL 2.0). To make room, the rows of that page are a little closer together than
before, and in Jedi Academy the panel is taller. Jedi Academy multiplayer has the rows on the More Video page of Setup
both in the main menu (where the slider for the brightness moves up a little to make room) and in the game menu. Jedi
Outcast multiplayer uses the menus that came with the game, which can't be changed: there it is the console or the
config file. (The other Jedi Academy multiplayer renderer, `rd-rend2`, has effects of its own and does not use these.)

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
| `r_sharpen` | 0 | **Sharpening** (contrast adaptive sharpening, the method of AMD's FidelityFX CAS), 0 (off) to 1 (strongest). Brings out the detail of the textures, especially with SMAA or multisampling, without halos: each pixel is sharpened by as much as the pixels around it leave room for before they would clip, so flat and busy areas get the most and edges that are already sharp the least. Console only: nothing in the picture is softened by the renderer (no temporal anti-aliasing or upscaling), so it has no row in the menus. |
| `r_lightShafts` | 0 | **Light shafts**: rays of sunlight, on levels whose sky gives a sun. The sky near the sun is blurred along lines towards the sun (the scattering of light as a post process of Mitchell, GPU Gems 3, chapter 13), so where something stands in front of the sky the rays have gaps: they come through windows and gratings, between pillars and around the edges of roofs, and the sun gets a soft glow. Only the brighter parts of the sky give rays; they fade out as the sun leaves the view. 1 is the normal strength, up to 2. Done at a quarter of the size of the view, about the cost of bloom. Setup > More Video > Light Shafts: Off, On. |
| `r_eyeAdaptation` | 0 | **Eye adaptation**: dark places brighten as your eyes get used to them, over a second or two, and very bright ones dim a little; coming out into the light the view adjusts faster than going into the dark. The games draw in the 0 to 1 range of the screen, so there is no wider range to choose an exposure from as HDR rendering would: the average brightness of the view (the mean of its logarithm) is measured on the graphics card, read back a few frames later without waiting for it, and the exposure follows it, from 0.8 to 1.8, applied with a curve that keeps black black and white white (`1 - (1 - colour) ^ exposure`), so nothing clips. Normally lit scenes stay close to the original look. The HUD and menus are not affected. 1 is the normal strength, up to 2. Setup > More Video > Eye Adaptation: Off, On. |
| `r_postDebug` | 0 | For testing: 1 runs the effects' plumbing with nothing in it (the picture must not change), 2 shows the occlusion alone. |

The effects run one after the other: occlusion, bloom, SMAA, then sharpening. Cost on a Radeon RX 7800 XT at 3440x1440 with 8x
multisampling: not measurable (about 135 fps with and without, with a noisy counter).

## Shader dynamic lights (`r_dlightGLSL`, default 0)

The light of lightsabers, blaster bolts, explosions and so on (the "dynamic lights") is drawn as a texture projected
onto every triangle. With `r_dlightGLSL 1` it is a shader instead: every pixel is lit by its own distance to the
light and by the angle between its surface and the light, with the surface normals smoothed across triangles. On
flat walls this looks very close to the original; the difference is on curved surfaces and characters, where the
light no longer shows the triangles. The old look is one setting away, and also used when the shader does not build.

`r_dlightGLSL 2` (Shader Dynamic Lights: Detailed) also lights the bumps of the surface. The games have no normal maps,
so the surface texture is read as a height (its brightness, the brightest texel `DLIGHT_BUMP_DEPTH`, 1.5 units, above
the darkest), and its slope across each pixel tilts the normal, with the surface gradient method of Mikkelsen ("Bump
Mapping Unparametrized Surfaces on the GPU", 2010), which needs no tangents, so it works on every surface the game
has. Rock, brick and floor tiles get relief where a saber or an explosion lights them; the overall brightness stays
the same (within 1%). It costs three texture reads a lit pixel instead of one.

### Lightsabers and blaster bolts, as in the films

With the shader dynamic lights (and with Ray Tracing, which uses them), a lightsaber is a light along its whole
blade, not a point at its middle: what it lights is lit from the nearest point of the blade, so a blade held along a
wall throws a streak of light down the wall, and its glow reaches about 2.5 times the blade's length (1.8 times the
radius the game gives it, `DLIGHT_LINE_REACH`) and is a quarter brighter (`DLIGHT_LINE_BRIGHTNESS`). Blaster bolts
light what they fly past (none of the game's weapons gave their bolts a light): a short light along each bolt in its
colour (red for the blasters and pistols, green for the bowcaster and TIE fighters, blue for the DEMP 2, ...), and a
flash of 150 ms where one hits a wall or a body (not for what explodes, whose effect has its own light). The game
modules ask for these with a new call, `AddLineLightToScene` (`cgi_R_AddLineLightToScene` in single player,
`trap->ext.R_AddLineLightToScene` in Jedi Academy multiplayer); with the projected dynamic lights, or a renderer that
does not have it (rend2), it is the light at the middle the game always had. Jedi Outcast multiplayer, which runs the
original game modules, keeps its lights as they were.

## Soft particles (`r_softParticles`, default 1, Vulkan)

The sprites of effects (smoke, sparks, explosions, the glow of sabers and blaster bolts) are flat pictures, and where
one goes into a wall or the floor it used to end in a hard straight line. They now fade out within 6 units in front of
what is behind them, so smoke rolls along the floor and a glow touching a wall has no edge: once the opaque surfaces
of a view are drawn, their depth is copied, and a blended stage of an effect (not of a model or of the level, nor a
mark laid on a surface, like a scorch) reads it and fades its alpha or its colour, as its blending allows. The effects
themselves are unchanged. The depth is only kept when such an effect is in the view, in the pause of the drawing
the shadows make (about 0.3 to 0.5 ms a frame at 3440 x 1440 with 8x multisampling on a Radeon RX 7800 XT). The Vulkan
renderer only, so far; 0 turns it off.

The effects' pass also dithers the picture it stores (8 bits a colour): a noise of up to one step on either side, so
that the smooth gradients it makes (a bloom halo, a dark room the eye adaptation brightens) show no steps.

## Brightness and overbright (`r_gamma`, `r_overBrightBits`)

The original games set the brightness (the Brightness slider of the video setup, `r_gamma`) with the gamma ramps of the graphics
card, which changed everything on the screen at once, and used the same ramps for overbright lighting: with
`r_overBrightBits 1` the game draws at half brightness and the ramps double it, so lighting can be up to twice as
bright as white. SDL3, which the game now uses for its window, has no gamma ramps; until now the brightness was baked
into the textures as they loaded (only the textures, not the lighting or the menus, and only after a restart), and
overbright was off.

All renderers (OpenGL and Vulkan, single player and multiplayer, both games) now do what the ramps did in a pass over
the finished picture, just before it is shown, with the same table (rounding included): the slider takes effect at
once and brightens everything, overbright works, in a window too (where the original never allowed it, since the
ramps would have brightened the whole desktop), and screenshots look like the screen. At brightness 1 without
overbright, the defaults, the pass is skipped. Checked by reading the picture back before and after the pass and
comparing it with the table applied in software: no pixel differs, in all five renderers. The OpenGL version needs
GLSL; without it the old way (textures) is used. In multiplayer it replaces the older `r_gammaShaders`.

## Credits

Contrast adaptive sharpening is AMD's method (FidelityFX CAS, GPUOpen, published under the MIT license); the shader
here is written from its description (`Post_Sharpen` in `shared/renderer/tr_post.h`, `post_sharpen.frag` for
Vulkan) and runs on any graphics card.

The light shafts follow Kenny Mitchell's "Volumetric Light Scattering as a Post-Process" (GPU Gems 3, chapter 13);
the shaders are written here (`Post_LightShafts` in `shared/renderer/tr_post.h`, `post_shafts.frag` for Vulkan).


SMAA is Subpixel Morphological Antialiasing by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and
Diego Gutierrez (https://github.com/iryoku/smaa). Its shader code and lookup textures are used as they published
them, under the MIT license (the notice is in `shared/renderer/tr_smaa_data.h`).

## The code

The OpenGL renderers of single player and multiplayer share it: `shared/renderer/tr_glsl.h` (loading the GLSL
functions, building programs), `shared/renderer/tr_post.h` (the screen effects) and `shared/renderer/tr_dlight_glsl.h`
(the shader dynamic lights), each included once by a short file of each renderer (`tr_glsl.cpp`, `tr_post.cpp`,
`tr_shade.cpp`) after its own `tr_local.h`. The Vulkan renderer has its own versions in `vk_post.cpp`.
