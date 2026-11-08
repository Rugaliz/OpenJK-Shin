# Shadows and draw distance

## Stencil shadows (`cg_shadows 2`)

The stencil shadows ("volumetric" in the menus of some versions) of the original game are a shadow volume per
surface of every character, extruded down to the floor under it, and everything inside a volume is darkened by a flat
50%. They had several problems that have nothing to do with modern drivers, and are fixed in all four games (the
code is shared by the single player and the multiplayer renderers, `shared/renderer/tr_shadow_volumes.h`):

* **Characters darkened themselves.** The volume of the torso also contains the legs, and so on, so characters looked
  half black. Pixels that show a character are now left out of the darkening (`r_shadowSelf 1` brings the old
  behaviour back). This also removes the flickering where a volume and the model met.
* **Holes and stripes through walls.** Models split their vertices along texture seams, so their volumes were not
  closed. Vertices at the same position are now welded, and the walls of a volume come from an edge count that
  cancels shared edges, so open meshes and edges shared by more than two triangles work too. (There also was a hard
  limit of 32 edges per vertex.)
* **The shadow always fell straight down**, whatever the light did (the light direction only leaned it by 30%). It now
  follows the direction of the strongest light of the level at the character (`r_shadowTilt`, 1 = the real direction,
  up to 45-50 degrees; the old behaviour is about 0.3).
* **Flat darkness.** The shadow is as dark as the share of the level's light that comes from one direction, instead of
  always 50%, so shadows are weak in a dark room and stronger in bright light (`r_shadowStrength` scales it).
* **Dynamic lights turned and darkened the shadows.** The light at a character, as the game works it out, includes the
  dynamic lights (sabers, blaster bolts, explosions), and the nearest one outweighs the level's light: a lit saber
  turned the shadow of its wielder, and of everyone near, away from itself, at the darkest. The shadows now go by the
  level's light alone (the dynamic lights cast their own shadows with Ray Tracing). The direction was also read in the
  character's own axes as if they were the world's, so a shadow turned with the character.
* **Dynamic lights were darkened by the shadows.** The shadows darken the finished picture, and the dynamic lights had
  already added their light to it: a saber held over its wielder's shadow lost up to three quarters of its light
  there. Their light is now added after the shadows (the dynamic lights drawn by the shader, `r_dlightGLSL`, as with
  Ray Tracing), so a saber lights up the shadow under it.
* **Hard edges.** The volumes are drawn several times with a slightly different light direction and the results add
  up, so the edge is soft, wider the higher the part of the body is above the floor. `r_shadowSoftness` is the width
  (0 = hard edges), `r_shadowSamples` is the number of volumes (1 to 16, 8 by default). Cost: the volumes are small,
  eight of them are not measurable on a modern graphics card.
* **Shadows of characters with no floor under them** (the trace down failed, or the character is cloaked) extended to
  height 0 of the map. The shadow now only exists when there is a floor within 512 units (single player and Jedi
  Academy multiplayer, whose cgame instead made up a floor 25 units under the feet when there was none in 4096).
  The renderer also drops the shadow of any caster whose floor is more than 1024 units under it, for cgame modules
  that can't be changed.
* **Shadows of items and other models with no floor** (the MD3 models: pickups, weapons on the ground) were extended
  to height 0 of the map too, as no cgame gives them a floor: through every floor in between on a map above 0, and
  none at all on a map below 0. They now reach 128 units under the model, where the floor under a pickup is.
* **Your own shadow in first person** (Jedi Academy draws your body only for its shadow then) kept the shadows of
  others off whatever was in front of that invisible body. It is no longer counted as a visible character.
* **Jedi Outcast multiplayer** had no stencil shadows of characters at all: its cgame module finds the floor under a
  character but asks for it to be used only for the projected shadows (`cg_shadows 3`). The renderer of Jedi Outcast
  multiplayer uses the floor it found for the stencil shadows too.
* **Drawing.** The thousands of tiny immediate-mode strips are replaced by one batch of triangles per sample, and the
  shadows are rendered once at the end of the frame instead of per surface.
* `r_shadowRange` (how far from you shadows are drawn) is 2000 instead of 1000.

Cost in multiplayer: with 15 bots fighting, uncapped, about 440 frames per second without and 335 to 546 with them
(the scene moves), so well under a millisecond a frame.

Not done: shadows do not fall on walls or higher steps than the floor under the character (the volume ends at the
floor), and characters do not shadow each other (they are excluded from the darkening). The soft shadows below do the
first.

## Soft shadows (`cg_shadows 4`)

Setup > More Video > Shadows > Soft, in all the menus of single player and of Jedi Academy multiplayer (whose menus
had no row for the shadows, it is added there). Jedi Outcast multiplayer runs the original game modules, which only
know the values up to 3: `r_shadowMaps 1` makes its `cg_shadows 2` soft instead. The value is only offered where the
renderer can draw them (OpenGL with shaders, or Vulkan); where it can't, `cg_shadows 4` draws the stencil shadows.

These are shadow maps: every character is drawn as seen from its light into a small depth map (128 x 128, how high
towards the light the character is at each point), and every pixel of the view is compared with it. A point that is
below the character, seen from the light, is in its shadow. So:

* the shadow has the shape the character has from the light, and falls on the floor, on walls, on steps higher than
  the floor under the character and on slopes;
* its edge is soft, and softer the further the point is from the character (sharp at the feet, blurred at the head's
  shadow), as with a real light of some size (`r_shadowSoftness` is the width, as for the stencil shadows);
* darkness, light direction and the floor limit are the same as for the stencil shadows (`r_shadowStrength`,
  `r_shadowTilt`), characters are still not darkened by shadows.

How: the triangles of a character that face its light are the ones the stencil shadows already collect. They are
drawn on the CPU into the depth maps, 16 of them in one 512 x 512 texture (the 16 characters nearest to you get a
shadow), because the Vulkan renderer can only draw into the picture. Each character then takes one pass over the part
of the view its shadow can reach, which finds the position of each pixel from the depth buffer, searches the map for
what is above it (5 samples) and filters the edge with 12 samples over a radius from that distance. The code is
shared by all the renderers (`shared/renderer/tr_shadow_volumes.h` for the maps, `tr_post.h` for the OpenGL pass,
`code/rd-vulkan/shaders/post_shadow.frag` for the Vulkan one). The views in mirrors and portals have no soft shadows.

Cost: in Jedi Academy multiplayer at 1280 x 720 with five characters in view, about 0.9 ms a frame (0.4 ms of it on
the CPU), against 1.2 ms for the stencil shadows.

## Ray traced shadows (`r_rayTracing`)

Setup > More Video > Ray Tracing: Off, Medium (`r_rayTracing 1`) or High (`r_rayTracing 2`), in single player with the
Vulkan renderer on a graphics card with ray tracing (`r_rtAvail` 1, see
[the Vulkan renderer](vulkan-renderer.md#ray-tracing-for-shadows-in-progress)). It is a setting of its own, apart from
Shadows: with it on, the shadows of the characters are ray traced whenever Shadows is Volumetric or Soft (with None or
Simple there are none to trace), and the dynamic lights get shadows whatever Shadows is. (It used to be a choice of the
Shadows row, `cg_shadows 5`; a settings file that has it is turned into Soft with Ray Tracing at Medium.)

The characters are found as for the shadow maps (the floor under them, how dark, the area their shadow can fall in),
but instead of a 128 x 128 depth map each, all their triangles go into a ray tracing structure every frame, and from
every pixel in a character's area rays, spread over the size of its light, are traced against it. So the shadow is
exact at any size and distance: fingers, the hilt of a saber, the gap between the legs, with no blocks, soft at the
edges. It is done in one pass for all the characters, and blurred along the surfaces at full size (the blur of the
ambient occlusion, which stops at edges in depth):

* **Medium**: at half the size of the view (a quarter of the pixels), two rays a pixel for each light, and two more
  between them where the first two disagree (the edge of a shadow: elsewhere the character is wholly in the way of the
  light, or wholly not).
* **High**: at the full size of the view, four rays a pixel for each light, and four more where they disagree. The
  edges are smoother and finer, with less grain.

**The lamps of the level.** The levels are lit by lamps (the "light" entities the map compiler lit them with), and the
lighting of every surface was worked out from them when the level was made: it stays as it is, with the shadows the
walls and pillars cast. The shadows of the characters, which move, are what is drawn live. Where the level still has
its lamps (every Jedi Outcast level, 300 to 1,400 of them; only 3 of the 34 Jedi Academy levels, which were compiled
without them), a character's shadow is cast from each of the up to four lamps that light it most and that it can see
(the brightest at the character as the map compiler worked it out, colour and spotlight cones included): a soft shadow
away from each, as dark as that lamp's share of the light. Standing between two lamps casts two faint shadows; walking
past a lamp turns its shadow around the character. What a character can see is found with the game's own collision
traces, and kept for the 16 units cube it stands in, so one that stands still costs nothing. Where there are no lamps,
the shadow comes from the one direction of the level's light grid (the strongest light at the character, as worked out
when the level was made), fainter and softer than a lamp's as it is a guess. `r_rtLamps 0` uses the grid's direction
everywhere.

**Contact shadows.** Around the feet of every character, short rays from the floor all around (32 units) find how much
of the light from no one direction the character takes away close to it: the darkening where a foot meets the floor or
a hand rests on a crate, which grounds the character even where the direction of the light is not known. Three rays
at Medium, four at High, only within the character's size of its middle.

With the lamps and the contact shadows, on a Jedi Outcast level at 3440 x 1440 with a character holding a lit saber
close to the camera (its shadow areas take much of the view), a frame took 9.3 ms at Medium and 13.9 ms at High,
against 8.8 and 12.0 ms with one shadow from the grid's direction.

At 3440 x 1440 with 8x multisampling on a Radeon RX 7800 XT, nine characters in view, a frame took 6.1 ms with the soft
shadows (shadow maps), 5.0 ms with Medium and 6.1 ms with High; with one of them holding a lit saber, 6.3, 5.9 and
8.0 ms. Medium is the cheaper of the two kinds of soft shadows on a card with ray tracing.

Cost: with seven characters in view at 1280 x 720 on a Radeon RX 7800 XT, about 1.4 ms a frame (0.5 of it building the
structure of 19,500 triangles), against 0.34 ms of graphics card time for the shadow maps (which draw their maps on the
processor instead). The rays scale with the number of pixels in the characters' shadow areas.

The light of sabers, blasters and explosions (the dynamic lights) gets shadows too with Ray Tracing: before a lit pixel is
lit, a ray goes from it to the light through the level and its doors and lifts, and where something is in between, the
light stops there. So it no longer comes through walls and floors (a saber in the next room lit the floor on this side,
an explosion above lit the room below), and pillars and crates keep it off what is behind them. The ray stops 12 units
short of the light, so that the wall an explosion sits on is still lit by it, and the hand on a saber's hilt does not
hide the blade. With Ray Tracing, the dynamic lights are always drawn by the shader (as with `r_dlightGLSL` 1, or 2 if
that is set).

A lightsaber or a blaster bolt is a light along a line (see [post-processing](post-processing.md)): rays go to points
spread along it, so its shadows are soft, as a long light makes them: four at Medium, eight at High. Every other one
goes first, over the whole line; only where they disagree, at the edge of a shadow, do the ones between them go too
(elsewhere the whole line is seen, or hidden, from the pixel). At 3440x1440 with nine characters in view, one lit saber costs 1.0 ms a frame on a Radeon RX 7800 XT (it was
1.45 ms with four rays everywhere), a blaster firing about 0.2 ms. Characters block these lights too, the
one holding the saber as well: a duel throws the fighters' shadows on the walls and the floor. The characters are
known only after the dynamic lights are drawn, so these rays see them as they were the frame before (at 60 frames a
second and more that is not to be seen); from a character's own skin the rays start 8 units out, so that the frame
before's copy of it does not shadow it where it has moved since. A light at a point gets one ray.

Grates, fences and foliage are flat surfaces whose texture is cut out by an alpha test (or, for see-through ones,
blended by its alpha). For the rays they block only where the texture is there: a light behind a grate throws the
grate's pattern, leaves throw the shapes of leaves. The alpha of every such texture of the level (up to 256 of them)
is kept small, 128 x 128, in an atlas made when the level's first frame needs it, with the texture coordinates of
their triangles; on yavin1b that takes 90 ms once. Textures that move (tcMod scroll, rotate, ...) block nothing.

## Draw distance

* `r_surfaceSpriteRange` (default 3): grass, bushes and other "surface sprites" fade out at a distance set per
  shader in the game data, 300 to 500 units for most of Jedi Academy, which was chosen for the hardware of 2002. The
  value multiplies it. Two things limited it further: a field of view wider than the standard one shortened the
  distance (so every widescreen display lost range), that no longer happens. The number of sprites grows with the
  square of the range; 4 is still free on a modern machine.
* `r_lodscale` (default 20, was 10): characters and models drop to coarser models of detail twice as far away. The
  limit of the value for characters is 50 instead of 20. A saved config with the old default 10 is changed once.

Not changed: the far clip plane and the fog distance are set by every map (`distanceCull`), and effects can have a
cull range in their effect files.
