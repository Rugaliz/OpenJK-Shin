# Binaural (HRTF) sound positioning

Setup > Sound > **HEADPHONE 3D (HRTF)** (cvar `s_hrtf`, default off) positions the sounds of the game in 3D around
your head when you are using headphones. On speakers leave it off.

The stock sound menu has a row for EAX, the hardware reverb of old Creative sound cards, which OpenJK's sound system
doesn't use. That row is used for this option instead.

## How it works

Normally a sound is positioned with plain left/right volume panning. A head-related transfer function (HRTF) instead
describes how a sound from a given direction is changed on its way to each ear: it arrives at the nearer ear earlier
(up to about 0.7 ms) and louder, and the head and outer ears colour it differently depending on where it came from.
With `s_hrtf 1` every sound that has a position in the world (sound effects, looping sounds, voices of characters, but
not the sounds that come from the player, announcements, music or videos) is filtered with the pair of impulse
responses for its direction, relative to where the player is looking, including from behind, above and below.

* `code/client/snd_hrtf.cpp` is the filtering, independent of the rest of the engine: it converts the measured
  responses to the output rate when it is first needed, interpolates between measured directions, and changes
  smoothly when a sound moves: the sound is put through the old and the new filter and faded from one to the other
  sample by sample, over as long as the last change took to come, so a sound that keeps moving (or a player who keeps
  turning) glides instead of stepping at each frame. The change is worked out from the sample time, so when the mixer
  mixes the same moment again in its next update (it mixes ahead) it comes out the same as before.
* `S_SpatializeOrigin` (snd_dma.cpp) works out the direction of each sound, `S_PaintChannelHRTF` (snd_mix.cpp) does the
  filtering while mixing. Distance attenuation is unchanged.
* The cost is a convolution of about 140 taps per ear for each positioned sound that is playing (both ears are done
  in one pass), twice that while it is changing direction. So that a big fight can't eat the frame rate, only the 32
  loudest positioned sounds are filtered binaurally (`S_HrtfLimit`, snd_dma.cpp); the rest, which are quieter than
  those, are panned as they are with `s_hrtf 0`. A sound that is filtered binaurally counts as half as loud again when
  they are ranked, so it doesn't change between the two as volumes go up and down.
* The loudness is matched to the old panning, on average over all directions, so switching it on doesn't change the
  overall volume.

## Credits and data

The impulse responses are the **diffuse-field equalised KEMAR dummy head measurements** of the MIT Media Lab
(44.1kHz, 128 samples per ear, 368 directions from -40 to +90 degrees elevation):

> This data is Copyright 1994 by the MIT Media Laboratory. It is provided free with no restrictions on use, provided
> the authors are cited when the data is used in any research or commercial application.
>
> Bill Gardner and Keith Martin, MIT Media Lab Machine Listening Group.

<https://sound.media.mit.edu/resources/KEMAR.html>

`code/client/snd_hrtf_data.cpp` is generated from that data with `tools/hrtf/gen_hrtf_data.py` (see the header of
that script for how to run it).
