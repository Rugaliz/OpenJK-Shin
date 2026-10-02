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
  smoothly when a sound moves.
* `S_SpatializeOrigin` (snd_dma.cpp) works out the direction of each sound, `S_PaintChannelHRTF` (snd_mix.cpp) does the
  filtering while mixing. Distance attenuation is unchanged.
* The cost is a convolution of about 140 taps per ear for each positioned sound that is playing.
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
