# Worker threads and faster level loading (single player)

The engine was written for one processor core, and most of it still is: the zone memory, the cvars and the file
system are not safe to use from two threads. Making the game itself parallel (the game code, the physics, the
renderer's front end) would mean rewriting those, and is not something that can be done safely on top of 20 year old
code. What can be done is to give the work that is made of independent pieces to a few worker threads while the main
thread waits, or does something else. That is where the time of loading a level goes, and this is what was done.

`com_jobThreads` (saved, default -1) is how many worker threads there are: -1 uses what the processor has (up to 7),
0 turns them off and the engine works as before, any other number limits them. Changing it needs a restart.

## What runs on worker threads

* **Mip levels** of textures (`R_MipMap`, both the Lanczos one that is the default and the blur one): the rows of a
  picture are shared between the threads. Results are exactly the same as before; the blur filter was also rewritten as
  two passes of four taps instead of one of sixteen (checked to give the same bytes).
* **Sample rate conversion of sounds** (`ResampleSfx`), the part of loading a level that grew when the mixer started
  to run at the sound card's own rate: output samples are shared between the threads, and the filter's dot product uses
  SSE. Loading the sounds of a level went from about 410 ms to 190 ms (`developer 1` prints it as "Sound: loaded").
* **Reading ahead the pictures of a level** (`rd-common/tr_image_prefetch.cpp`). Decoding JPEG and TGA files was
  almost half of loading a level, and every picture is independent. A level does not say which pictures it needs until
  its shaders have been read, so the list comes from the last time the level was loaded: it is kept in
  `loadlists/<map>.txt` next to the config (one file name per line, it grows to cover everything the level has asked
  for, delete the folder to start over). When a level starts to load, those files are read by the main thread (the file
  system belongs to it) and decoded by the workers; when the renderer asks for a picture it is usually done. A picture
  that is not in the list, or has not been read yet, is loaded as always, so a wrong list costs nothing but a little
  memory (at most about 640 MB are read ahead at a time). The first time a level is loaded there is no list and
  nothing changes. Pictures come out byte for byte the same as the normal loaders (checked on every picture of a
  level).

## What it does to loading

Measured on the author's machine (16 threads), Jedi Outcast `yavin_temple` from a save, with sound: about 2.1 s
before, 1.45 s with the lists in place. Jedi Academy `yavin1b`: 1.7 s and 1.27 s. What remains is on the main thread:
inflating the files out of the pk3 archives, decoding the MP3 voice and music (the decoder keeps global state and
cannot run on two threads), loading models, and the game code starting the level's entities.

## Reading the screen back (saves and loading screens)

Opening the menu in a level, saving, and the dissolve at the end of a loading screen read the whole screen back. With
the Vulkan renderer that took over 100 ms at 3440x1440 because the buffer was in memory the CPU reads slowly and was made
anew each time; it is cached memory kept between calls now (about 18 ms). `r_vkProfile 1` prints every place where the
Vulkan renderer waits for the GPU for more than 2 ms, which is how to look for stutter.

## What this is not

It does not make the frames faster: drawing is one thread of the engine, and the graphics card is the part that
works in parallel there. Loading only gets as fast as its serial part allows (the list above).
