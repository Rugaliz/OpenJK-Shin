# Vulkan renderer (single player) - plan and status

Goal: a Vulkan renderer for the single player games (Jedi Outcast and Jedi Academy) that looks the same as the OpenGL
one, as a **separate renderer module** (`rdsp-vulkan` for Jedi Academy, `rdjosp-vulkan` for Jedi Outcast). The OpenGL
renderer stays the default and stays supported (including on Windows). Pick the Vulkan one with
`+set cl_renderer rdsp-vulkan` (the engine already loads whichever module `cl_renderer` names).

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
- [x] 7. `ReadPixels` (screenshots), `CopyTexImage2D` / `CopyTexSubImage2D` (distortion effects), depth read back (flares), cinematics (`DrawStretchRaw`). Gamma is done by the renderer on the textures, as in the OpenGL one.
- [~] 8. Done: MSAA (with alpha to coverage and sample shading), anisotropy, vsync modes, `vid_restart` and mode changes, a pipeline cache kept between runs, 3440x1440 at 8x MSAA reaches the 250 fps cap. Still to do: run with the validation layer (`r_vkValidation 1`, needs the layer installed: `vulkan-validation-layers`), Windows / Wine check.
- [ ] 9. Post effects (SSAO, bloom, SMAA, shader dynamic lights) natively in Vulkan; menu rows show only when available.
- [ ] 10. Docs, README, CI (build the module only when the Vulkan headers and `glslc` exist).

Shaders are GLSL compiled to SPIR-V at build time with `glslc`; the generated SPIR-V is also checked in as a header
(same idea as `tr_smaa_data.h`) so a Windows build without the Vulkan SDK still works.

## Using it

Build as usual (the module is built when the Vulkan headers are found; `-DBuildSPRdVulkan=OFF` / `-DBuildJK2SPRdVulkan=OFF`
skip it), then start the game with `+set cl_renderer rdsp-vulkan` (Jedi Academy) or `+set cl_renderer rdjosp-vulkan`
(Jedi Outcast). `cl_renderer` is saved, `cl_renderer rdsp-vanilla` (`rdjosp-vanilla`) goes back to OpenGL.

| Setting | Default | What it does |
|---|---|---|
| `r_ext_multisample` | 0 | Multisampling (2, 4, 8, 16: the highest the graphics card has up to what is asked). Same setting as in OpenGL. |
| `r_swapInterval`, `r_adaptiveVsync` | as in OpenGL | Vertical sync; 0 shows frames as soon as they are done, 1 waits for the display (with `r_adaptiveVsync` a late frame tears instead of waiting a whole refresh). |
| `r_vkDevice` | -1 | Which graphics card (the numbers are listed in the log at start-up); -1 takes the best one that can show the window. |
| `r_vkValidation` | 0 | Turns on the Khronos validation layer if it is installed (for developers). |

The first run of a scene builds pipelines, which can be seen as short stutters; they are kept in `vk_pipeline_cache.bin`
(next to the config), so the next runs are smooth.

## What is not there yet

* Screen effects (ambient occlusion, bloom, SMAA) and shader dynamic lights: they exist in the OpenGL renderer only, the
  Setup > More Video rows for them do not show with the Vulkan one (step 9).
* The dynamic glow (needs old NVIDIA / ARB programs), as in OpenGL on current drivers.
* Texture compression (the textures are uploaded as RGBA8, the graphics memory use is higher than in OpenGL).
* Not tried on Windows yet.

## Status

See the checklist above; each step is its own commit.
