#pragma once

// Fixes for individual game systems (UI, quests, slow motion, fishing, movement,
// flight unit, hacking). Installed after the core patches.
void InstallSubsystemPatches();

// Per-area installers (one file each).
void FixFlightUnit();
void FixHacking();
void FixMovementExtra();

// Gravity step used by the player and enemy integrator patches (exposed for tests):
// returns the vel.y to store and corrects the position at entity+0x50 in free fall.
float ExactGravity(unsigned char* entity, float vbefore, float vanilla, float g, float d, bool grounded);
