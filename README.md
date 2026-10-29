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

`uninstall.bat` removes it again. Check `AutomataHighFPS.log` in the game folder
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
| `FixSubsystems` | `1` | Hacking, menus, flight unit, fishing and movement fixes. |
| `FixCloth` | `1` | Cloth and hair physics in 60 Hz steps with smooth in-between frames. |
| `MenuFPS` | `0` | Optional frame rate cap while a menu is open (`0` = no cap). |
| `UnlimitedLoading` | `1` | Removes the frame limit on loading screens, which shortens loads. |
| `ForceMaxRefreshRate` | `1` | Exclusive fullscreen uses the monitor's highest refresh rate instead of 60 Hz. |

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

About 270 patches, located by signature in both executables:

| Area | What was frame-dependent | Fix |
|---|---|---|
| Game speed | Time step clamped to ≥ 1 frame, so the whole game sped up | Clamp removed; frame limiter target configurable |
| Hit-stop | Hit-stop delay and duration counted in frames (global time nodes and the player's own) | 60 Hz ticks |
| Quest scripts | `TimeCountAction` and other millisecond timers truncated each frame and stopped advancing above ~1000 FPS | Exact per-frame milliseconds; 60 FPS timing kept |
| Events and dialogue | Cutscene fade and caption lead-ins, caption auto-close, Yes/No input lock, forced-walk, fast-travel hold, elevator, timed HUD gauge (fixed 1/60 s per frame); NPC talk release; bunker waits; object-load wait; placed-item countdown; transition cooldowns | Scaled per frame / 60 Hz ticks |
| Slow motion | Finisher slow-mo, BGM slow and delayed slow-mo requests counted in frames | 60 Hz ticks |
| Menus and UI | Menu key-repeat (pad, keyboard, mouse), HUD fades, option-text marquee, rumble length | 60 Hz ticks / scaled smoothing |
| Player movement | Glide spin-kick drift, air control, attack damping and plunge, jump/attack press buffers, jump and glide arcs, vertical free-move mode, scripted lift, slope landing check, A2 evade turn, assorted per-frame player counters | Scaled / exact gravity / 60 Hz ticks |
| Pod | Follow smoothing (57 sites), sonar ping interval, scripted pod timers, bob ramp | Scaled / 60 Hz ticks |
| Enemies | Navmesh pull-back (robots leaving bounds), fall smoothing, gravity, hacked-robot attack damping | Scaled / exact gravity |
| Flight unit | Rail movement and smoothing, ship movement, tilt and turn rates, behind-view battles, curve-following enemies, wingmen, shooter bullets and homing, lasers, scroll speed changes | Scaled (no frame cap) |
| Hacking (and Ending E credits) | Ship and enemy speed smoothing and movement, respawn waits, spins, wave patterns, gun sweeps, hit cooldowns, items, bombs, knock-back | Scaled / 60 Hz ticks (no frame cap) |
| Fishing | Bobber settle | Scaled |
| 9S hack gauge | Decay per frame | Scaled |
| Cloth and hair | The solver steps once per frame with per-step damping and constraint passes, so cloth turned stiffer and slower | Simulated in real 60 Hz steps; frames in between show a partial-step preview that is rolled back before the next step |

## Known limitations

These are still frame-dependent:

- **Camera**: the third-person camera modes ramp and smooth some values per
  frame, so the camera settles slightly faster at high frame rates.
- **Regular bullets and enemy AI**: the shared bullet code's decay, wobble and
  homing for non-shooter bullets, and enemy AI beyond the systems listed above,
  have not been fully swept.

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
