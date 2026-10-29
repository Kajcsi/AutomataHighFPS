# NieR:Automata High FPS Fix

Removes NieR:Automata's 60 FPS cap and fixes the game logic that breaks above
60 FPS, so the game plays exactly as it does at 60 FPS at any frame rate.

Supports the current Steam release (2021 "Become as Gods" build): both
`NieRAutomata.exe` and `NieRAutomataCompat.exe`. Everything is located by
byte-signature at runtime; if the game is updated and a signature stops
matching, that fix is skipped and logged instead of patching the wrong code.

## Install

1. Run `build.bat` (needs Visual Studio 2022 with the C++ workload), or use the
   prebuilt `build\dinput8.dll`.
2. Run `install.bat`. It copies `dinput8.dll` and `AutomataHighFPS.ini` into the
   game folder.
3. Start the game normally from Steam.

`install.bat` also works while the game is running; the new version is used from the next launch. `uninstall.bat` removes it again. Check `AutomataHighFPS.log` in the game folder
to see which patches were applied.

The DLL loads as `dinput8.dll`, so it doesn't conflict with ReShade or Special K
(those use `dxgi.dll`/`d3d11.dll`). Don't combine it with Special K's FAR
"Remove 60 FPS Cap" option, which patches the same code.

## Settings (`AutomataHighFPS.ini`)

| Setting | Default | Meaning |
|---|---|---|
| `TargetFPS` | `0` | Frame rate limit. `0` = no limit (with V-Sync on, the monitor refresh rate is the limit), `-1` = monitor refresh rate, or a number such as `141`. |
| `ToggleCapKey` | `0x7A` (F11) | Toggles a temporary 60 FPS cap. A beep confirms the toggle. |
| `FixFrameCounters` | `1` | Per-frame counters (hit-stop, transition cooldowns, …) tick at 60 Hz. |
| `FixMsTimers` | `1` | Quest/script millisecond timers stay accurate at any frame rate. |
| `FixSubsystems` | `1` | Fixes for individual game systems: movement, enemies, bullets, camera, NPCs, world objects, menus, flight unit, hacking, fishing. |
| `FixCloth` | `1` | Cloth and hair physics in 60 Hz steps with smooth in-between frames. |
| `MenuFPS` | `0` | Optional frame rate cap while a menu is open (`0` = no cap). |
| `UnlimitedLoading` | `1` | Removes the frame limit on loading screens, which shortens loads. |
| `ForceMaxRefreshRate` | `1` | Exclusive fullscreen uses the monitor's highest refresh rate instead of 60 Hz. |
| `JumpLog` (`[Debug]`) | `0` | Logs the player's jump/fall state, phase and height every frame to `AutomataHighFPS.log`. Takes effect while the game runs. |

With V-Sync on in the game's options, the frame rate is also capped by the
monitor's refresh rate.

## How it works

The engine measures each frame in "60 Hz frames": the time step is 1.0 at 60
FPS and 2.0 at 30 FPS, clamped to `[1.0, 2.0]`. That clamp is why simply
removing the frame limiter makes the game run too fast. Code that scales by
this step is correct at any frame rate once the clamp is lowered. Code that
assumes "one frame = 1/60 s" (integer counters, fixed per-frame movement,
truncated millisecond timers) is what breaks above 60 FPS.

The mod:

- Replaces the frame limiter's 60 FPS target with the configured one, while
  still honouring the game's own 30 FPS mode.
- Removes the 1.0 lower clamp of the time step, so the game runs in real time.
- Re-points every frame-dependent routine it knows about to one of these
  per-frame values (e = this frame's share of a 60 Hz frame, 1.0 at 60 FPS):
  - **60 Hz ticks** for integer frame counters (1 at ≤ 60 FPS, otherwise 0 or 1
    from an accumulator), so a 20-frame window lasts 1/3 s at any frame rate;
  - **c·e** for amounts added once per frame (movement steps, ramps, spins);
  - **k^e** for per-frame decay (`v *= k`);
  - **1−(1−k)^e** for per-frame smoothing (`v += (target − v) · k`),
    including smoothing factors stored per object, rescaled at runtime;
  - an exact gravity step that reproduces the 60 FPS jump/fall/glide arcs.

At 60 FPS or below every replacement value equals the original constant, and
with a 60 FPS limit the time step is clamped exactly as in vanilla, so the mod
changes nothing there. All values are recomputed once per frame.

## Fixes

About 890 patches, located by signature in both executables:

| Area | What was frame-dependent | Fix |
|---|---|---|
| Game speed | Time step clamped to ≥ 1 frame, so the whole game sped up | Clamp removed; frame limiter target configurable |
| Hit-stop | Hit-stop delay and duration counted in frames (global time nodes and the player's own) | 60 Hz ticks |
| Quest scripts | `TimeCountAction` and other millisecond timers truncated each frame and stopped advancing above ~1000 FPS | Exact per-frame milliseconds; 60 FPS timing kept |
| Events and dialogue | Cutscene fade and caption lead-ins, caption auto-close, Yes/No input lock, forced-walk, fast-travel hold, elevator, timed HUD gauge (fixed 1/60 s per frame); NPC talk release; bunker waits; object-load wait; placed-item countdown; transition cooldowns | Scaled per frame / 60 Hz ticks |
| Slow motion | Finisher slow-mo, BGM slow and delayed slow-mo requests counted in frames | 60 Hz ticks |
| Menus and UI | Menu key-repeat (pad, keyboard, mouse), HUD fades, option-text marquee, rumble length | 60 Hz ticks / scaled smoothing |
| Player movement | Glide spin-kick drift, air control, attack damping and plunge, jump/attack and button press buffers, jump and glide arcs, vertical free-move mode, scripted lift, slope landing check, A2 evade turn, animation-blend and animation-speed smoothing, scripted states (dive, flight-unit attach, push, hover sway, fall return, stage laser), beam length, assorted per-frame player counters | Scaled / exact gravity / 60 Hz ticks |
| Pod | Follow smoothing, orbit angle, follow-factor ramp and target smoothing, bone and arm aim, sonar ping interval and smoothing, scripted pod timers, bob ramp, program spins and beam growth, Pod B laser burst, keep-alive and beam lengths, skill model lifetimes | Scaled / 60 Hz ticks |
| Enemies | Common movement smoothing (yaw, angle, speed), strafing, keep-distance and approach steps, drift, navmesh pull-back, hit pull-in, RunState steps, hacked-robot stick decay, fall smoothing, gravity, boss part spins and smoothing, hit windows, attack cooldowns, waits, slow-motion override and time-scale request counters, hacked-robot attack damping | Scaled / exact gravity / 60 Hz ticks |
| Bullets | Arc-shot gravity and speed-up, spin, homing turn rate and fraction, speed decay, wobble, guided-line counter and snap, gravity wells, ring bombs, beams, and bullet types that moved a fixed distance per frame | Scaled |
| Camera | Every third-person camera mode's follow-factor ramps, angle, distance and offset smoothing, approach fractions and step limits, angular impulses, target lag, aim offsets, recentre, snap and skip-update frame counters, the mode-transition blend, event and scripted cameras (path camera, moveOffset, side-view path mode), camera shake (integrator steps, random sign changes on 60 Hz ticks); fractions the camera already scaled by the frame time use the exact 60 FPS equivalent | Scaled / 60 Hz ticks |
| NPCs and companions | 9S/A2 yaw, animation blend, step and follow smoothing, scripted entrances, anchor and navmesh pulls, creature NPC turning, leaning, speed ramps and path movement, foot IK, the 9S thrown weapon, fish | Scaled |
| World | Map gimmick timers (doors, elevators, steam pipes), item and corpse lifetimes, item magnet, drop timers, environment and light transitions, phase-flow and manager timers, scrolling sections, see-through fades, ambient audio smoothing and ray sampling, combat hit and phase windows, particle emit counters | Scaled / 60 Hz ticks |
| HUD and menus | Boss HP gauge, operating-ratio and pollution gauges, popups and slides, core-menu waits, weapon list and item-shortcut counters | Scaled / 60 Hz ticks |
| Flight unit | Rail movement and smoothing, ship movement, tilt and turn rates, behind-view battles, curve-following enemies, wingmen, shooter bullets and homing, lasers, scroll speed changes | Scaled (no frame cap) |
| Hacking (and Ending E credits) | Ship and enemy speed smoothing and movement, respawn waits, spins, wave patterns, gun sweeps, hit cooldowns, items, bombs, knock-back | Scaled / 60 Hz ticks (no frame cap) |
| Animation inputs | Playback runs on the frame time, but some states smoothed or ramped the playback speed, blend parameters and root-motion scale once per update (player run and turn states, an escort blend ramp, the Baa000 mount speed, shared enemy turn states) | Scaled |
| Fishing | Bobber settle | Scaled |
| 9S hack gauge | Decay per frame | Scaled |
| Frame-time-scaled smoothing | Turn and approach fractions the game multiplies by the frame time (shared enemy and NPC turn helpers, turn-to-point, pod follow and orbit, animals, player flight and states, camera modes) scale only linearly, so they still settled faster above 60 FPS | Exact 60 FPS equivalent, identical at 60 FPS, slow motion kept |
| Cloth and hair | The solver steps once per frame with per-step damping and constraint passes, so cloth turned stiffer and slower | Simulated in real 60 Hz steps; frames in between show a partial-step preview that is rolled back before the next step |

If something misbehaves, press F11 to drop to 60 FPS while you get past it, and
please report where it happened.

## Building and testing

- `build.bat` builds `build\dinput8.dll` (static CRT, no runtime needed).
- The large patch sets live in `analysis\*.tsv` (one row per site: signature,
  offset, primitive, formula). `tools\gen_patch_tables.py` re-verifies every
  signature against both decrypted executables and generates
  `src\generated\*.inc`, embedding the original bytes so the DLL refuses to
  patch anything unexpected.
- `tests\build_tests.bat` builds `tests\build\harness.exe`, an offline test
  that maps a SteamStub-decrypted copy of your own game executable (not
  included; the game's `.text` section is encrypted on disk) without running it,
  applies
  every patch, and calls the patched game functions with simulated frame times
  (for example: the time step at 144 FPS, hit-stop counters, quest timers at
  2000 FPS). `tests\disasm_patches.py` disassembles every patch site and code
  cave from the harness output for review.
