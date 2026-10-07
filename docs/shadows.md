# Shadows and draw distance

## Stencil shadows (`cg_shadows 2`)

The stencil shadows ("volumetric" in the menus of some versions) of the original game are a shadow volume per
surface of every character, extruded down to the floor under it, and everything inside a volume is darkened by a flat
50%. They had several problems that have nothing to do with modern drivers, and are fixed in the single player games:

* **Characters darkened themselves.** The volume of the torso also contains the legs, and so on, so characters looked
  half black. Pixels that show a character are now left out of the darkening (`r_shadowSelf 1` brings the old
  behaviour back). This also removes the flickering where a volume and the model met.
* **Holes and stripes through walls.** Models split their vertices along texture seams, so their volumes were not
  closed. Vertices at the same position are now welded, and the walls of a volume come from an edge count that
  cancels shared edges, so open meshes and edges shared by more than two triangles work too. (There also was a hard
  limit of 32 edges per vertex.)
* **The shadow always fell straight down**, whatever the light did (the light direction only leaned it by 30%). It now
  follows the direction of the strongest light at the character (`r_shadowTilt`, 1 = the real direction, up to
  45-50 degrees; the old behaviour is about 0.3).
* **Flat darkness.** The shadow is as dark as the share of the light that comes from one direction, instead of
  always 50%, so shadows are weak in a dark room and stronger in bright light (`r_shadowStrength` scales it).
* **Hard edges.** The volumes are drawn several times with a slightly different light direction and the results add
  up, so the edge is soft, wider the higher the part of the body is above the floor. `r_shadowSoftness` is the width
  (0 = hard edges), `r_shadowSamples` is the number of volumes (1 to 16, 8 by default). Cost: the volumes are small,
  eight of them are not measurable on a modern graphics card.
* **Shadows of characters with no floor under them** (the trace down failed, or the character is cloaked) extended to
  height 0 of the map. The shadow now only exists when there is a floor within 512 units.
* **Drawing.** The thousands of tiny immediate-mode strips are replaced by one batch of triangles per sample, and the
  shadows are rendered once at the end of the frame instead of per surface.
* `r_shadowRange` (how far from you shadows are drawn) is 2000 instead of 1000.

Not done: shadows do not fall on walls or higher steps than the floor under the character (the volume ends at the
floor), and characters do not shadow each other (they are excluded from the darkening). Both would come from shadow
maps, which need a shader based renderer.

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
