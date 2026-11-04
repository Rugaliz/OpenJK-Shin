# Antialiasing and texture filtering

All of this is in the renderer that Jedi Outcast and Jedi Academy single player share (`code/rd-vanilla`), and plain
OpenGL 1.x plus a few extensions that every driver has had for years. The multiplayer renderer (`codemp/rd-vanilla`,
both games) has the same cvars below. Jedi Academy multiplayer has the menu rows too (`codemp/ui`: the Anti-Aliasing
row is added under Video Sync, in the main and the in-game menu, and the Anisotropic Filter slider becomes the same
list); Jedi Outcast multiplayer runs the game's own menus, so there they are console settings. The Lanczos filter itself is
shared by both renderers (`shared/renderer/tr_mip_lanczos.h`).

## Menu

* Setup > Video > **Anti-Aliasing** (Off, 2x, 4x, 8x), takes effect with Apply Changes. The Video Quality presets set
  it too (High 4x, Normal 2x, Fast and Fastest off). It uses the row of the old menu for compressed textures, which was
  not shown.
* Setup > More Video > **Anisotropic Filter** (Off, then 2x up to what the graphics card can do). It used to be an
  on/off switch in Jedi Outcast and a slider that took any number in Jedi Academy.

## Cvars

| cvar | default | |
|------|---------|--|
| `r_ext_multisample` | 0 | MSAA samples (latched). When the display can't do that many, the game tries half as many, down to none, and sets the cvar to what it got. |
| `r_alphaToCoverage` | 1 | With MSAA on, the edges of cut out surfaces (leaves, fences, grates: shaders with `alphaFunc GE128`) are smoothed by turning their alpha into coverage of the samples instead of testing it. The other alpha tests are left as they are. |
| `r_sampleShading` | 0 | 0 to 1: how much of the samples of a pixel are shaded separately (`GL_ARB_sample_shading`). 1 also smooths textures and cut outs, and is slow. Latched, needs MSAA. |
| `r_ext_texture_filter_anisotropic` | 16 | Anisotropic filtering level, was there before. |
| `r_simpleMipMaps` | 2 | How the smaller sizes of the textures are made: 0 the blur of the original Quake 3 filter, 1 average of 2x2 pixels (what the game used), 2 Lanczos (3 lobes windowed sinc, `R_MipMapLanczos` in shared/renderer/tr_mip_lanczos.h). Latched. Costs about a third of a second more per level load. |

`gfxinfo` shows what is in use ("multisampling: 4x, alpha to coverage").

## Smoother lightmaps (`r_smoothLightmaps`, default 1)

The light of a level is stored in lightmaps, 128 x 128 texels to a page, a few texels per metre, and drawn with linear
filtering: between two texels the light changes along a straight line, and at each texel the slope changes at once.
So the edges of shadows and the pools of light show the texel grid as creases, steps and diamonds.

As a level loads, every page is made 4 times larger with a bicubic filter (Catmull-Rom), which goes through the value
of every texel, so the light stays as it was made and only the way it changes between texels becomes smooth. A cubic
curve swings past the values on either side of a sharp change, which would draw a bright or dark line along the edge
of a shadow, so what it gives is kept between the two texels it lies between. A page holds the lightmaps of many
surfaces side by side, and a bicubic filter reaches further than a linear one: each surface's rectangle is filtered on
its own, so no light is brought in from its neighbours. The larger pages are drawn with linear filtering and mipmaps
as before.

It costs nothing while playing, works the same in every renderer (OpenGL and Vulkan, single player and multiplayer),
adds a few milliseconds per page to loading a level (60 to 85 msec for the 26 pages of `mp/ffa3`) and takes 16 times
the memory for the lightmaps (a megabyte a page). `r_smoothLightmaps 0` and a map restart bring back the originals.
The code is `shared/renderer/tr_lightmap_smooth.h`.
