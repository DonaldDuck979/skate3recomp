#pragma once

namespace rex::runtime {
class FunctionDispatcher;
}

// Open Roam: full-map online freeskate (no snap-back, collision everywhere,
// out-of-bounds markers, no leaving-area warning). Controlled by cvar
// skate3_oob_kill_mode; see skate3_oob_watch.cpp for the bits.
namespace skate3::oob_watch {

// Install the Open Roam hooks. Call once at startup with the runtime
// dispatcher (reads skate3_oob_kill_mode; does nothing when it is 0).
void InstallKillHook(rex::runtime::FunctionDispatcher* dispatcher);

}  // namespace skate3::oob_watch
