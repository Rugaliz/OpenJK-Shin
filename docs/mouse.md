# Mouse sensitivity and DPI

The turn speed is `m_yaw * sensitivity` degrees per count the mouse reports, and the menu slider of the game went from 2
to 30. That was made for the mice of 2002 (400 DPI). A mouse of today has 800 to 16000, so at 1600 DPI the lowest
setting was already four times too fast.

## What there is now

* **Setup > Controls > Mouse/Joystick > Mouse DPI** (cvar `in_mouseDPI`, default 400): tell the game the DPI of the
  mouse (it is on the box or in the software of the mouse, most have a button that cycles through a few).
  Sensitivity is scaled by `400 / DPI`, so the same sensitivity is the same physical speed on any mouse and the old
  configs, which were all for 400 DPI, keep working. The row is the one of the old menu for force feedback.
* The **Sensitivity** slider goes from 0.05 to 30 and moves by ratios (logarithmic), so the low end, where today's mice
  need it, is not squeezed into the first few pixels.
* The default `sensitivity` is 1.5 (it was 5). A saved config keeps its own value.

Games can't ask a mouse for its DPI (SDL, the OS and USB don't report it), so it has to be told.

Not changed: there is no mouse acceleration (`cl_mouseAccel` 0) and the mouse is read raw (relative mode), so what
remains is the numbers above.
