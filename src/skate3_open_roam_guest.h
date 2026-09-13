#pragma once

#include <cstdint>

// Open Roam (full-map online freeskate) call sites patched into the generated
// recompilation by cmake/ApplySkate3CodegenPatches.cmake. Implemented in
// skate3_oob_watch.cpp; all are no-ops unless cvar skate3_oob_kill_mode enables
// the matching feature. Include after skate3_init.h (needs PPCContext).

// sub_8247C100 (streaming-channel focus setter), before its lock test. Lets
// the collision channel stream around the player online: releases the lock,
// drops the fixed play-area cell list, and forces a refocus on long jumps.
void Skate3OpenRoam_FocusUpdate(PPCContext& ctx, uint8_t* base);

// sub_8272AB80 / sub_8285CDC0: true = run the collision streamer that follows
// the local player even in an online session.
bool Skate3OpenRoam_SimFollow();

// sub_825DE4E0: true = don't show "You are leaving the online area!".
bool Skate3OpenRoam_HideAreaWarning();

// sub_82D55710 (session-marker disable): true = ignore the disable.
bool Skate3OpenRoam_IgnoreMarkerDisable(PPCContext& ctx, uint8_t* base);

// [cosmetics] sub_82DDEDB0 / sub_82DDEEF8 (character part set: r3 = character
// assembly, r4 = slot, r5:r6 = 128-bit part id). The marker-hat lock
// (skate3_cosmetic_lock.cpp); true = skip the part set.
bool Skate3Cosmetic_OnSetPart(PPCContext& ctx, uint8_t* base, int which);
