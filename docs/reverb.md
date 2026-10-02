# The sound of the room (reverb)

Setup > Sound > **ROOM ECHO (REVERB)** (cvar `s_reverb`, default on) gives sounds the echo of the room they are in. It
also muffles sounds that have a wall between them and the listener, and everything while under water.
`s_reverbLevel` (default 1) scales how much reverb there is.

The old games had this with the EAX hardware of the sound cards of the time, using hand made zones that came with each
level (`eagle/*.eal`). Those rely on a closed library to tell which zone the listener is in, so they can't be used
here. This version works out the room as the game runs instead, which has the advantage that it works for any level,
including ones that were never given zones.

## How it works

* **The room** (`S_AnalyseRoom`, snd_dma.cpp): ten times a second 26 rays are sent out from the listener through the
  collision data of the level. How many hit something (the rest escape: outdoors, windows to the sky), how far they
  go, how close the nearest wall is and what the surfaces are made of give the size of the room and how much of the
  sound it soaks up. The decay time follows from Sabine's rule for the reverberation time of a room (0.161 V / A,
  with the volume over the surface worked out from the mean distance to the walls). Open places get little or none.
  Many levels of Jedi Outcast don't say what their surfaces are made of, so a neutral value is used for those.
* **The reverb** (snd_reverb.cpp, independent of the rest of the engine): the sounds that are to have reverb are
  mixed into one signal (each sends a share of itself that falls off with distance more slowly than the direct sound
  does), which goes through a few early reflections and a feedback delay network of eight lines with a low pass in
  each, so the high frequencies fade faster than the low ones. The decay time of the room is followed smoothly.
  Methods: Schroeder, "Natural sounding artificial reverberation" (1962); Jot and Chaigne, "Digital delay networks for
  designing artificial reverberators" (1991). It measures within 2% of the wanted decay time from 0.3 to 5 seconds
  at 22, 44.1 and 48kHz.
* **Walls** (`S_UpdateObstruction`): a few times a second a line from the listener to each sound is checked against
  the level; if something is in the way the sound is filtered (the highs go) and made a bit quieter, gradually. The
  reverb of the room is not affected, so a sound in the next room is heard muffled and echoing.
* **Under water**: everything is filtered down to 800Hz, and the reverb is dull and short.
* The mixing is in snd_mix.cpp (`S_PaintMono`, `S_PaintChannelHRTF`, `S_SendToReverb`, and the end of `S_PaintChannels`).
  Music, videos and the sounds that come from the player's own entity (apart from sounds they make) and announcements
  have no reverb or muffling.

## Limits

* Doors and other moving parts of a level aren't in the collision data used, so they don't muffle sound.
* The first sound of a level can be heard before the room is first looked at (a tenth of a second).
* Only the stereo output (and the HRTF output) has it.
