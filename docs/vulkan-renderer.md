# Vulkan renderer (single player) - plan and status

Goal: a Vulkan renderer for the single player games (Jedi Outcast and Jedi Academy) that looks the same as the OpenGL
one, as a **separate renderer module** (`rdsp-vulkan` for Jedi Academy, `rdjosp-vulkan` for Jedi Outcast). The OpenGL
renderer stays supported (including on Windows) and is what the game falls back to when Vulkan cannot be used. The
Vulkan renderer is the default; `cl_renderer` names the module the engine loads.

## How it is built

The 3D front end of `code/rd-vanilla` (BSP, shaders, models, Ghoul2, sky, fog, shadows, sprites, weather) is a few
hundred thousand lines of logic that only talk to OpenGL through about 70 distinct `qgl*` calls (fixed function:
texture units, texenv modulate/add/replace, blend, alpha test, depth, stencil, fog, clip plane, polygon offset,
matrices, immediate mode for 2D and debug drawing, vertex arrays for surfaces).

Rewriting every call site is what Quake II/III Vulkan ports did, and it is slow and risky for 46,000 lines. Instead:

* The Vulkan module **compiles the same `rd-vanilla` sources** (define `RD_VULKAN`) so bug fixes stay shared.
* `qgl.h` is replaced (`code/rd-vulkan/qgl_vk.h`) by a **fixed-function-over-Vulkan layer**. Every `qglXxx` call
  records state (blend, depth, stencil, texenv per unit, bound textures, matrices, array pointers, fog ...) and a draw
  call turns that state into a cached `VkPipeline` (one uber shader, state in pipeline / push constants) and records
  commands.
* Code that is OpenGL only (GLSL helpers, post processing FBO passes, NV register combiners, ARB programs) is compiled
  out under `RD_VULKAN` and re-done natively later.
* Once everything runs, hot paths (surface drawing) can be moved from the layer to direct Vulkan calls step by step.

## Steps (commit after each; tick when done)

- [x] 0. This plan.
- [x] 1. Engine plumbing: `GRAPHICS_API_VULKAN` window (SDL_WINDOW_VULKAN), renderer imports for instance extensions,
      surface creation, drawable size; `REF_API_VERSION` 20.
- [x] 2. CMake targets `rdsp-vulkan` / `rdjosp-vulkan` (option `BuildSPRdVulkan`, needs the Vulkan headers and `glslc`),
      `RD_VULKAN` build with a null `qgl` layer so it links and starts (blank window).
- [x] 3. Vulkan core: instance, device, swapchain, per-frame sync, depth/stencil (+MSAA) target, clear and present
      driven by `RE_BeginFrame` / `RE_EndFrame`; vsync and resize / `vid_restart`.
- [x] 4. Textures: `qglTexImage2D`, `TexSubImage2D`, mips, filters, wrap, anisotropy -> `VkImage` + samplers.
- [x] 5. 2D drawing (immediate mode + `DrawStretchPic`): menus and console visible.
- [x] 6. 3D: matrices, depth, blend, alpha test, cull, multitexture + texenv, fog, clip plane, polygon offset, scissor,
      stencil (shadows), depth range (weapon), vertex arrays.
- [x] 7. `ReadPixels` (screenshots), `CopyTexImage2D` / `CopyTexSubImage2D` (distortion effects), depth read back (flares), cinematics (`DrawStretchRaw`). Brightness and overbright are a pass over the finished picture (`RB_GammaPass`, as in the OpenGL renderers).
- [~] 8. Done: MSAA (with alpha to coverage and sample shading), anisotropy, vsync modes, `vid_restart` and mode changes, a pipeline cache kept between runs, 3440x1440 at 8x MSAA reaches the 250 fps cap. Run with the validation layer (`r_vkValidation 1`, needs the layer installed: `vulkan-validation-layers`). The Windows build (MinGW) runs under Wine. Still to do: a real Windows machine.
- [x] 9. Post effects (SSAO, bloom, SMAA, shader dynamic lights) natively in Vulkan (`vk_post.cpp`, shaders `post_*.frag`, `smaa_*.frag`, `dlight.*`); the Setup > More Video rows show with the Vulkan renderer too. Checked against the OpenGL renderer by screenshots (occlusion map, bloom, SMAA) and with 4x multisampling.
- [ ] 10. Docs, README, CI (build the module only when the Vulkan headers and `glslc` exist).

Shaders are GLSL compiled to SPIR-V at build time with `glslc`; the generated SPIR-V is also checked in as a header
(same idea as `tr_smaa_data.h`) so a Windows build without the Vulkan SDK still works.

The Windows version built on Linux with MinGW needs only the Vulkan headers: the renderer is linked against Windows'
Vulkan loader, `vulkan-1.dll`, which comes with every graphics driver, and MinGW toolchains seldom have its import
library, so the build makes one with `dlltool` from `rd-vulkan/vulkan-1.def`, the list of the functions the renderer
calls (a function it starts to call goes there too, or the Windows build fails to link).

## Using it

Build as usual (the module is built when the Vulkan headers are found; `-DBuildSPRdVulkan=OFF` / `-DBuildJK2SPRdVulkan=OFF`
skip it). It is the default renderer (`cl_renderer rdsp-vulkan` for Jedi Academy, `rdjosp-vulkan` for Jedi Outcast).
`cl_renderer` is saved; `+set cl_renderer rdsp-vanilla` (`rdjosp-vanilla`) goes back to OpenGL. Configs saved by older
versions that still name the OpenGL renderer are changed to the Vulkan one once (`com_configVersion` 4).

**When Vulkan cannot be used** (the module is missing, there is no Vulkan driver, no device can show the window), the
game loads the OpenGL renderer for that session and says so: a red warning with the reason is drawn at the top of the
screen in the menus and on loading screens, and for the first 30 seconds of playing (`cl_rendererWarning`; the reason
is also in the log and in `cl_rendererFailure`). The saved `cl_renderer` is left alone, so the next start tries Vulkan
again. Choosing the OpenGL renderer yourself shows no warning.

| Setting | Default | What it does |
|---|---|---|
| `r_ext_multisample` | 0 | Multisampling (2, 4, 8, 16: the highest the graphics card has up to what is asked). Same setting as in OpenGL. |
| `r_swapInterval`, `r_adaptiveVsync` | as in OpenGL | Vertical sync; 0 shows frames as soon as they are done, 1 waits for the display (with `r_adaptiveVsync` a late frame tears instead of waiting a whole refresh). |
| `r_vkDevice` | -1 | Which graphics card (the numbers are listed in the log at start-up); -1 takes the best one that can show the window. |
| `r_vkValidation` | 0 | Turns on the Khronos validation layer if it is installed (for developers). |
| `r_vkRayTracing` | 1 | Turns ray tracing on where the graphics card has it (see below); 0 never does. Needs a restart. |
| `r_rtDebug` | 0 | Shows what the ray tracing sees (for developers, see below). |

The first run of a scene builds pipelines, which can be seen as short stutters; they are kept in `vk_pipeline_cache.bin`
(next to the config), so the next runs are smooth.

## Ray tracing (for shadows, in progress)

Where the graphics card has ray tracing, the Vulkan renderer turns it on: the standard Khronos extensions
`VK_KHR_acceleration_structure` and `VK_KHR_ray_query` (with `VK_KHR_deferred_host_operations` and buffer device
addresses), which the ray tracing hardware of every vendor has: AMD RX 6000 and newer, NVIDIA RTX, Intel Arc, on
Windows and on Linux (Mesa's RADV and ANV, NVIDIA's driver). They are used for shadows: shaders ask whether the line
from a point to a light is blocked, searching acceleration structures (a bottom level one holds triangles, a top
level one places them in the world). Graphics cards without it keep the soft shadows of the shadow maps.

Done so far (`code/rd-vulkan/vk_rt.cpp`): the device is made with the extensions and features where it has them (and
again without them if that fails), buffers with device addresses, building the two kinds of structure, and a test at
start-up that builds a one-triangle scene and waits for it; the log says `...using ray tracing (VK_KHR_ray_query)` when
it worked, and `r_rtAvail` is 1 (the menus will look at it; the OpenGL renderers set it to 0).

The scene (`VK_RT_Scene`): the level and each of its brush models (doors, lifts, platforms) get a bottom level
structure the first time a frame needs them, kept until the next level loads. Only what blocks light goes in: solid
surfaces drawn as they are, so no sky, nothing see-through, nothing cut out by an alpha test (gratings, foliage: for
now light goes through them) and nothing whose vertices move. Each frame a top level structure of its own places the
level and the brush models among the entities of the view where they are now. On mp/ffa3 the level is 33,708
triangles, made in 2 ms on the first frame.

`r_rtDebug 1` darkens the view where a ray from the pixel towards the level's sun is blocked (or the surface faces away
from it), `r_rtDebug 2` shows only that. The result should match the shadows the level's own lighting has
(`r_lightmap 1` shows that lighting alone), with hard edges.

The characters' shadows (`r_rayTracing`, More Video > Ray Tracing, see [shadows](shadows.md)): every frame the
triangles of the characters go into a bottom level structure built for speed of building, placed by the frame's top
level structure (mask 0x04, the level is 0x01 and its brush models 0x02); `post_rt_shadow.frag` reads the casters from
a buffer by its device address; they have a top level structure of their own, built when the shadows are drawn.

The shadows of the dynamic lights (with `r_rayTracing`): the level's top level structure is built when a view
begins (`VK_RT_BeginView`, before anything is drawn), and the dynamic light program is drawn with
`dlight_rt.frag` (dlight.frag built with `RAY_QUERY`), which gets the structure in a second descriptor set and the
transform from the surface's space to the world in 64 bytes of push constants after the usual 128 (every graphics card
with ray tracing has room for 256). Cards without ray tracing never load that shader.

Alpha tested surfaces (grates, foliage): their triangles go into bottom level structures of their own that are not
opaque, so a ray query hands them to the shader as candidates; `shaders/rt_alpha.glsl` finds the texture coordinates
of the point (a storage buffer of triangles, from the instance's custom index and the primitive index) and confirms the
hit where the texture's alpha passes the shader's test, read from an atlas of the level's alpha textures
(`VK_RT_PrepareLevel`, at the level's first frame with ray tracing). The dynamic lights' set and the post passes' set
have the atlas and the buffer next to the structure.

## What is not there yet

* The dynamic glow (needs old NVIDIA / ARB programs), as in OpenGL on current drivers.
* Texture compression (the textures are uploaded as RGBA8, the graphics memory use is higher than in OpenGL).
* Tried on Windows only under Wine so far (the MinGW build, with ray tracing, on RADV).

## Status

See the checklist above; each step is its own commit.
