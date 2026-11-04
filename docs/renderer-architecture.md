# Renderer Architecture

The renderer is split into two parts: the front end and the back end.

## Front end
The front end is what cgame and UI modules communicate with. It's responsibility is to build up the scene as it is in the current frame to render as well as the view point from which to render. No GL calls are invoked at any point in this part of the code.

When the scene has been built up, the entities to draw are submitted into a queue to be read by the backend. Entities are sorted on a number of parameters.

Every surface to draw is given a sort key: its shader (in drawing order), the entity it belongs to, its fog and its
dynamic lights, packed into a 64 bit number (`QSORT_*` in `tr_local.h`) and sorted with a radix sort over the bytes
in use. It was a 32 bit number with 11 bits for the entity, so a scene could hold at most 2047 entities, which a big
fight with many effects could reach: what came after was not drawn. With 13 bits a scene holds 8191 entities and
262144 surfaces (`MAX_REFENTITIES`, `MAX_DRAWSURFS`), in the OpenGL and Vulkan renderers of single player and the
OpenGL renderer of multiplayer. The experimental rend2 renderer keeps its own 32 bit key and 2047 entities.

## Back end
The back end is responsible for drawing all of the entities submitted to it using OpenGL.
