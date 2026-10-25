# 0055. Audio spatialisation: listener-frame voices, attenuation, Doppler, air, occlusion

- **Date:** 2026-10-25
- **Status:** Accepted

## Context

CLAUDE.md §8.10 asks for "3D: distance attenuation curves, equal-power
panning, Doppler, occlusion via physics raycasts, and HRTF/ambisonics
later". The audio module (rank 6) cannot see the scene or physics; the
runtime (Phase 12 part F) will connect them. The engine's world positions
are 64-bit (§8.2), and a game world is large, so 32-bit world positions on
the audio thread would lose precision far from the origin, as they would
on the GPU.

## Decision

1. **Voices are placed in the listener's frame.**
   - A spatial voice (`NphAudioPlayDesc.spatial`) has a position and a
     velocity relative to the listener, in the listener's axes (ADR-0008:
     +x right, +y up, −z ahead), in f32.
   - The runtime computes them each frame from 64-bit world positions, as
     the renderer is camera-relative, so precision holds anywhere.
   - The audio thread has no listener and no world: each block it works
     out each spatial voice's gains, pitch, direction, and filter corners
     from its place (`mix_voice_spatial`), before the ranking.

2. **Distance.** `NphAudioAttenuation` gives:
   - an inverse law (OpenAL's "inverse distance clamped": min / (min +
     factor (d − min)), physical at factor 1, flat beyond the maximum);
   - a linear fade to silence at the maximum;
   - a designer's curve of up to eight points, linear in decibels between
     them;
   - or none.

   The default is inverse from 1 m to 50 m. The ranking (ADR-0053) uses
   the louder of a voice's direct and send gains, so far voices go
   virtual.

3. **Direction and size.**
   - The azimuth is panned by pan.h (equal-power stereo by its sine, VBAP
     on surround). Elevation folds into the horizontal plane until HRTF.
   - A source of radius `size` spreads by the angle it subtends: π at its
     surface, half of all directions, widening continuously to all around
     at its centre.
   - A multichannel clip is its own layout turned to face the voice's
     direction, collapsing to a point far away and opening to the layout's
     own angles from a spread of 60°.

4. **Doppler** is first order in the relative velocity: a pitch ratio of
   c / (c + v_r), where v_r is the rate at which the distance grows. It is
   clamped to [0.5, 2], with 2 for a supersonic approach. c is the mixer's
   `speed_of_sound` (343 m/s), and a per-voice factor scales the effect.
   The ratio multiplies the resampler's step, so the voice stays phase
   continuous.

5. **Air absorption** follows ISO 9613-1 at 20 °C and 50% relative
   humidity.
   - Over the top octaves, its loss grows as the square of the frequency.
     So does a one-pole low-pass's, while a second-order filter's grows as
     the fourth power.
   - Each spatial voice has a one-pole whose coefficient is solved exactly,
     for the digital filter (a quadratic in the pole), to lose the
     standard's 0.0766 dB per metre at 8 kHz. The coefficient ramps across
     each block.
   - Below 16 kHz rates the match frequency moves to 0.35 of the rate,
     with the loss scaled by the square of the frequency.
   - The filter's 4 kHz loss is within 0.03 dB of the standard's for the
     first 20 m.

6. **Occlusion and obstruction** are the runtime's rays' verdicts, 0 to 1:
   - **Occlusion** is sound through a wall. The direct sound and the sends
     lose up to 24 dB, and both lose their highs from 20 kHz down to
     700 Hz, through the voice's own low-pass (the lower of its corner and
     the user's).
   - **Obstruction** is the direct path alone blocked. The direct sound
     loses up to 12 dB and its highs down to 1 kHz, through a low-pass on
     the direct path only. The sends, which are the room's reflections,
     take the voice before it and keep their level.
   - Corners glide in log frequency with the amount, sample by sample, and
     switch on and off by crossfade, like every voice filter (ADR-0053).

7. **Sends take part of the distance.** A send's gain takes the distance
   attenuation raised to `send_distance` (0.5 by default). A room's
   reverberation thus falls more slowly than the direct sound, and far
   sources sound more distant.

## Alternatives considered

- **World positions on the audio thread, with a listener.** The audio
  thread would need 64-bit positions, or would lose precision far from the
  origin. The runtime already has every position and the listener; one
  subtraction and rotation per voice per frame is negligible there.
- **Exact two-velocity Doppler**, f(c − v_l·u)/(c − v_s·u). For a
  moving source and a still listener the first-order form is exact; when
  the listener moves, the two differ by (v/c)² (1% at a tenth of the speed
  of sound, 34 m/s). The exact form needs world velocities, which the
  listener-frame design hides.
- **A second-order air filter.** Its loss grows as the fourth power of the
  frequency. Matched at 8 kHz, it loses 0.05 dB at 4 kHz over 10 m where
  the air loses 0.23 dB.
- **Taking the one-pole coefficient from the analog corner.** The mixer's
  one-pole is impulse-invariant, so over 100 m it would lose 7.26 dB at
  8 kHz instead of the standard's 7.66.
- **One low-pass for obstruction and occlusion alike.** It is simpler, but
  an obstructed source would lose its room's reverberation, which is
  exactly what tells a listener the source is round a corner and not
  behind a wall.
- **HRTF or ambisonics now.** §8.10 places them later. The panners'
  interface (azimuth, spread) is where they will plug in.

## Consequences

- **Measured**: Doppler puts a 1 kHz source receding at a tenth of the
  speed of sound at 909.07 Hz (909.09 expected), and approaching at
  1111.08 Hz (1111.11 expected).
  - The air takes exactly 7.66 dB from 8 kHz over 100 m, and 0.09 dB from
    500 Hz.
  - Obstruction takes 12.01 dB from the direct path and 0.00 from the
    send; occlusion takes 24.02 dB from both.
  - A source circling the listener, moving and occluded every block, bends
    the signal less than a still tone does.
- **Cost**: 64 moving 3D voices take 230 µs per block, against 177 µs for
  64 plain resampled ones (Profile, Ryzen 7 5800X): about 0.8 µs per voice
  for Doppler's resampling, the air, the filters, and a spread pan.
- The runtime must update every spatial voice's position each frame (part
  F), including the listener's own motion, and cast the occlusion rays.
- Elevation is ignored until HRTF.
