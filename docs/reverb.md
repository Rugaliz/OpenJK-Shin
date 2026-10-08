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
  sound it soaks up. A room needs a ceiling: how many of the rays that point upwards hit something (not the sky)
  within 25 metres says whether this is a room or the outdoors, and the reverb is scaled by it, so a canyon or a
  street under the open sky has almost none (a little echo from the nearest walls) however close the cliffs are. The decay time follows from Sabine's rule for the reverberation time of a room (0.161 V / A,
  with the volume over the surface worked out from the mean distance to the walls). Open places get little or none.
  Many levels of Jedi Outcast don't say what their surfaces are made of, so a neutral value is used for those.
* **The reverb** (snd_reverb.cpp, independent of the rest of the engine): the sounds that are to have reverb are
  mixed into one signal (each sends a share of itself that falls off with distance more slowly than the direct sound
  does), which goes through a few early reflections and a feedback delay network of eight lines with a low pass in
  each, so the high frequencies fade faster than the low ones. The decay time of the room is followed smoothly, and
  the delays and levels are ramped within each block of sound rather than changed in steps (a step is a click).
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

## How it fits into the mixer

The mixer of the engine (from Quake 3) does not mix each moment of sound once: every update, once per frame, it mixes
everything from just ahead of what is being played to a fifth of a second ahead again, so that what is ahead can
change with the game. That is fine for adding sounds up, but the reverb remembers what went into it and has to go over
each moment once, in order. So:

* what the sounds send to the reverb is kept by time, and the reverb is only run a little way ahead, past where the
  next update will start mixing (further ahead there is no reverb until it is time for it, which is only heard if the
  game stalls for longer than that);
* the reverb as it was where an update started mixing is remembered, and the next update goes back to it and runs the
  reverb again from there, over what was sent to it (which has been played by then), up to where it starts mixing
  itself. A sound that starts in between is therefore all heard in the reverb, from its very start: when the reverb
  just carried on from where it had got to, the first frame or two of every sound (the attack of a gunshot, and all of
  a short click) never reached it.

Beyond what is mixed again every update the mixer keeps a reserve, mixed once (see `S_Update_`), which is only played if
a frame takes too long. It has no reverb (that is run for what is mixed again, and picks up after the long frame).

The other things that remember the signal: the muffling of a sound behind a wall and the headphone positioning take
what came before from the sound itself (and the headphone filter changes direction by the sample time, so mixing the
same moment again gives the same result), and the filter for under water continues from what it made for the moment
before.

Voices are MP3s that are decoded as they are played, so what comes before the part that is mixed is read from the
decoder's window of recent samples (it keeps a good deal). Before that was done a muffled voice (a wall in the way)
started its filter from the wrong value every frame, which was heard as crackling: about 15 times as many clicks in a
test.

## Cutscenes

While a scripted cutscene plays (the `inCutscene` cvar) the listener is the camera, not someone standing in a room, so
dialogue is heard as in a film: voices get no reverb (that includes the player's own voice, which otherwise always had
some) and nothing is muffled by walls between the camera and the speaker, which a camera placed behind a bar does a lot.

## Limits

* Doors and other moving parts of a level aren't in the collision data used, so they don't muffle sound.
* The first sound of a level can be heard before the room is first looked at (a tenth of a second).
* Only the stereo output (and the HRTF output) has it.
