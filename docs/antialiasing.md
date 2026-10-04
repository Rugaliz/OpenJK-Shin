# Antialiasing and texture filtering

All of this is in the renderer that Jedi Outcast and Jedi Academy single player share (`code/rd-vanilla`), and plain
OpenGL 1.x plus a few extensions that every driver has had for years.

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
| `r_simpleMipMaps` | 2 | How the smaller sizes of the textures are made: 0 the blur of the original Quake 3 filter, 1 average of 2x2 pixels (what the game used), 2 Lanczos (3 lobes windowed sinc, `R_MipMapLanczos` in tr_image.cpp). Latched. Costs about a third of a second more per level load. |

`gfxinfo` shows what is in use ("multisampling: 4x, alpha to coverage").
