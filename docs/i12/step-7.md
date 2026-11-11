# I12 step 7: the sound of the room

ADR-0021 §5; roadmap I12 step 7. A room was silent but for its clips. Now the hero TV hums, and hisses while it
shows a picture, the hero sign's neon buzzes, the arcade murmurs, the study's clock ticks, the walker's steps change
on the rug, an item clacks as it leaves its shelf, its case opens on Inspect, the cartridge clicks home, and the menus
tick as the focus moves.

## What was built

- **Synthesized, never sampled** (`audio/synth.hpp`, in `rv_audio_model`): twelve sounds from deterministic recipes
  (lowbias32 noise, one-pole and band-pass filters, sines; no `rand()`, no clock): five loops (the mains hum of 50 Hz
  and four harmonics with a 0.5 Hz beat, the tube's high-passed hiss, the neon's 100 Hz buzz with five crackles, a 4 s
  murmur, the clock's tick and tock) and seven one-shots (three footsteps by surface, heel and toe, wood brightest and
  the rug dullest; the clack, the case's latch, hinge and lid, the insertion's slide and click, the menus' tick). Every
  sound's peak is half of full scale; a loop's tones complete whole cycles and its filters run round it twice, so its
  last sample flows into its first. The provenance registry gains nothing.
- **The mixer's loops and one-shots** (`audio/mixer.hpp`): loops (`addSampleSource`, `setSampleControl`: the clips'
  ramps, a loop holding its place while silent), one-shots in a fixed pool of 16 voices (`trigger`, the oldest stolen,
  never an allocation or a free on the output's thread), and three buses (`Bus::Video`, `Ambience`, `Effects`, each
  ramped as the master). The duck now applies per sound: a one-shot that does not duck (the menus' tick) is mixed apart
  and plays over it. The clips' mix is unchanged bit for bit (their bus at 1, the duck in the same product).
- **The room's tone as data.** A pack's `sound` block (`hum`, `hiss`, `buzz`, `murmur`, `clock`, gains in [0, 2])
  becomes the profile's `RoomTone` and the scene's `sound`, as step 6's air does: a room's scene's environment, a
  floor's room areas (the hall and the scene have none). The classic and 90s rooms keep the defaults (hum, hiss, buzz);
  the lounge's monitor hums at 0.6; the arcade adds its murmur, the study its clock (its buzz at 0.6), the handhelds'
  hum is 0.5, and the hub has only a soft murmur. The scene goldens and digests move by that block alone.
- **The policy, GPU-free** (`app/room_sound_policy.hpp`): the emitters of each area (the hum and, while the hero
  shows a picture, the hiss 0.25 m over `fx:hero.crt`; the buzz at `fx:hero.sign`; the murmur and the clock 1.6 m over
  the area's centre), each heard whole within 2 m, then as 2 m / d but never below a quarter anywhere in its room (the room's reverberant field: the clips' steeper `audio::spatialGain` left the hum all but silent from the doorway), panned by `panForAzimuth`, 6 dB lower
  while a clip is heard, silent outside the eye's area; the floor under the walker (the rugs' and the runner's
  footprints, else parquet); a step every 0.72 m walked (0.95 m faster than 2.4 m/s), the first half a stride after
  setting off, none while the walker is held and none across a teleport; the interaction's edges (free roam and focus:
  the clack; focus and inspect: the case; into launching: the click).
- **`RoomSound`** (`app/src/room_sound.cpp`): a mixer loop per emitter on the ambience bus, taken again by the next
  scene's emitter of the same sound (so a ride cross-fades instead of cutting), the steps and the edges as one-shots on
  the effects bus, the tick when a navigation action is pressed while a menu is open. Each sound is synthesized the first
  time it is needed (four variations per footstep). It lives as long as the audio engine.
- **Settings and report.** Impostazioni's audio rows gain "Suoni d'ambiente" and "Effetti sonori" (`video.audio.ambience`,
  `video.audio.effects`, which `video.json` already read); the mixer takes the master and the three buses apart. The
  run report's `audio.room`: the emitters, the most heard at once, the one-shots, the steps, the ticks and the first
  2048 one-shots (frame, time, sound, gain, pan).

## Decisions and deviations

- The tick is a press of a navigation action while a menu is open, not a change of the focus the RmlUi documents keep:
  the same moments in practice, without a counter through the UI module.
- The clock ticks at the study's centre as ADR-0021 §5 says; its wall clock comes in step 8.
- Sound is not motion: `reduceMotion` leaves it as it is; `--audio off` and `RV_AUDIO=off` build no engine and so no
  room sound, and `RV_AUDIO=null` keeps every automated run silent (its mix only captured).
- The interaction's sounds play at the eye, centred: the item is always in front of it then.

## Measured

**Tests:** unit 2676/2676 (Debug); the GPU suite green but for the known cases (the two `process_runner` cases under the allowlist, two `xr` cases of I11 steps 11 and 12 not on this branch, the `xr` pillarbox case, which passes alone); the GPU case `world look: the room's sound` (the snes room's tone in the mix, the steps one per
onset while the walker walks, silence before it sets off).

**The goldens:** none moves (sound never shows); the scene goldens and digests by the `sound` block.

**Performance** (Release, the reference machine):

| Path | p50 | p99 | Acceptable (p50, p99) |
|---|--:|--:|---|
| Desktop benchmark, snes-stress at 1920 x 1080 | 2.75 ms | 3.15 ms | 16.7 ms, 33.3 ms |
| The same, two clips playing | 2.81 ms | 3.52 ms | 16.7 ms, 33.3 ms |
| VR benchmark, the fake headset at 72 Hz | 6.58 ms | 7.14 ms | 13.9 ms, 20 ms |
| VR benchmark at 120 Hz, the fast profile | 4.37 ms | 4.94 ms | informative |

## Follow-ups (BACKLOG)

- The interaction's sounds at the item's place rather than the eye's.
- A per-surface variation of the steps' level (a rug quieter than the parquet beyond its brightness).
