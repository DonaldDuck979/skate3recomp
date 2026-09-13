#include "skate3_oob_watch.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/function_dispatcher.h>

#include "skate3_init.h"  // REX_FUNC, sub_*
#include "skate3_open_roam_guest.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Open Roam: full-map online freeskate.
//
// skate3_oob_kill_mode bits (0 = stock game):
//    64  drop the online-area snap-back teleports; bail respawns land where
//        you are instead of back in the old play area
//   128  collision streams around the player online (generated 9/31/42.cpp)
//  1024  custom out-of-bounds markers + hide the "You are leaving the online
//        area!" warning (generated 20.cpp)
// Tested setting: 1216 (64|128|1024).
REXCVAR_DEFINE_INT32(skate3_oob_kill_mode, 0, "Skate 3",
                     "Open Roam (full-map online freeskate) bits: 64 no snap-back, "
                     "128 collision everywhere, 1024 out-of-bounds markers + no "
                     "warning text. 1216 = all. 0 = stock.")
    .range(0, 2047)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

extern "C" {
// Live player world position + "in world long enough" flag, published each
// frame by the scene builder (skate3_native_scene.cpp). Spawn placement uses
// the same teleport paths, so the filters stay off until the player is live.
float g_skate3_player_pos[3] = {0.0f, 0.0f, 0.0f};
int g_skate3_player_pos_valid = 0;
int g_skate3_player_live = 0;
// Read by patched generated code; set once at startup from the cvar.
int g_skate3_sim_follow_player = 0;  // bit 128 (generated 9/31/42.cpp)
int g_skate3_marker_unlock = 0;      // bit 1024 (generated 20.cpp)
}

// Controller state exported by rexruntime (xam_input.cpp).
extern "C" __declspec(dllimport) unsigned long long g_rex_marker_return_tick;  // LB+Up last held
extern "C" __declspec(dllimport) unsigned int g_rex_last_buttons;

namespace {

constexpr unsigned kBtnDpadDown = 0x0002;
constexpr unsigned kBtnLB = 0x0100;

inline float GuestF32(uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  v = __builtin_bswap32(v);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

inline void PutGuestVec3(uint8_t* base, uint32_t addr, const float* v) {
  for (int k = 0; k < 3; ++k) {
    uint32_t u;
    std::memcpy(&u, &v[k], 4);
    u = __builtin_bswap32(u);
    std::memcpy(base + addr + k * 4, &u, 4);
  }
}

inline bool FiltersActive() {
  return g_skate3_player_live && g_skate3_player_pos_valid;
}

// Last marker return that went through (target + tick). A bail/fall respawn
// shortly after a marker return goes back to the marker (see RequestTeleport).
float g_marker_return_target[3] = {0, 0, 0};
uint64_t g_marker_return_tick = 0;

// Custom out-of-bounds marker. The game refuses to set a marker, or teleport
// to one, outside the online area (no message is sent at all). So LB+Down
// while out of bounds stores the position here, and LB+Up while out of bounds
// retargets the next snap-back the game sends (it keeps re-sending them while
// you're out) to that position. "Out of bounds" = a snap-back was dropped in
// the last second.
uint64_t g_last_snap_drop_tick = 0;
bool g_custom_marker_valid = false;
float g_custom_marker[3] = {0, 0, 0};

bool IsOutOfBoundsNow() {
  return g_last_snap_drop_tick != 0 && GetTickCount64() - g_last_snap_drop_tick < 1000ull;
}

void PollMarkerSet() {
  if (!g_skate3_marker_unlock || !FiltersActive()) return;
  static unsigned s_prev = 0;
  const unsigned b = g_rex_last_buttons;
  const bool combo = (b & kBtnLB) && (b & kBtnDpadDown);
  const bool prev_combo = (s_prev & kBtnLB) && (s_prev & kBtnDpadDown);
  s_prev = b;
  if (!combo || prev_combo) return;
  if (IsOutOfBoundsNow()) {
    g_custom_marker[0] = g_skate3_player_pos[0];
    g_custom_marker[1] = g_skate3_player_pos[1] + 1.0f;
    g_custom_marker[2] = g_skate3_player_pos[2];
    g_custom_marker_valid = true;
    REXLOG_INFO("[open-roam] out-of-bounds marker set at ({:.1f},{:.1f},{:.1f})",
                g_custom_marker[0], g_custom_marker[1], g_custom_marker[2]);
  } else {
    g_custom_marker_valid = false;  // in bounds the game sets its own marker
  }
}

}  // namespace

// sub_82DFEC90: online-area controller ticker (vtable call). Used only as a
// per-frame guest-thread tick for the marker input.
extern "C" REX_FUNC(Skate3OobKill_TickHook) {
  PollMarkerSet();
  sub_82DFEC90(ctx, base);
}

// sub_825926F8(obj, r4 = target matrix): RequestTeleport. Called from
// sub_82592518 (lr 825926EC) for bail/fall respawns, which online also pull
// you back to the old play area once you're far out.
extern "C" REX_FUNC(Skate3OobKill_RequestTeleportHook) {
  if ((REXCVAR_GET(skate3_oob_kill_mode) & 64) && FiltersActive() &&
      static_cast<uint32_t>(ctx.lr) == 0x825926ECu && ctx.r4.u32 != 0) {
    const uint32_t m = ctx.r4.u32;
    const float dx = GuestF32(base, m + 48) - g_skate3_player_pos[0];
    const float dy = GuestF32(base, m + 52) - g_skate3_player_pos[1];
    const float dz = GuestF32(base, m + 56) - g_skate3_player_pos[2];
    const float d2 = dx * dx + dy * dy + dz * dz;
    // Player well below the target = fell through the world (e.g. collision
    // not loaded yet after a long marker return). Don't land that at the
    // (underground) player spot: go back to the marker if one was just used,
    // otherwise let the game's respawn through.
    const bool underground = dy > 25.0f;
    const bool recent_marker =
        g_marker_return_tick != 0 && GetTickCount64() - g_marker_return_tick < 20000ull;
    if (underground && recent_marker) {
      const float mt[3] = {g_marker_return_target[0], g_marker_return_target[1] + 1.5f,
                           g_marker_return_target[2]};
      PutGuestVec3(base, m + 48, mt);
      g_marker_return_tick = 0;
    } else if (!underground && d2 > 40.0f * 40.0f && dy < 175.0f) {
      // Long pull back to the play area: keep the respawn (dropping it sent
      // you to spawn) but get up where you bailed, like offline.
      PutGuestVec3(base, m + 48, g_skate3_player_pos);
    }
  }
  sub_825926F8(ctx, base);
}

// sub_82597A70(r3 = skater component, r4 = message): skater message handler.
// A type-5 message is a teleport (target position at +58/+62/+66). The online
// snap-back and the LB+Up marker return use the identical message, so they're
// told apart by target and input.
extern "C" REX_FUNC(Skate3OobKill_SkaterMsgHook) {
  if ((REXCVAR_GET(skate3_oob_kill_mode) & 64) && FiltersActive()) {
    const uint32_t msg = ctx.r4.u32;
    uint32_t type;
    std::memcpy(&type, base + msg, 4);
    if (__builtin_bswap32(type) == 5u) {
      const float tx = GuestF32(base, msg + 58), ty = GuestF32(base, msg + 62),
                  tz = GuestF32(base, msg + 66);
      const float dx = tx - g_skate3_player_pos[0], dy = ty - g_skate3_player_pos[1],
                  dz = tz - g_skate3_player_pos[2];
      const float d2 = dx * dx + dy * dy + dz * dz;
      // Online-area controller "out of area / return active" state (+921 bit
      // 0x80 or countdown 1..3 at controller 0x4046A820).
      uint32_t oob_t;
      std::memcpy(&oob_t, base + 0x4046ABB4u, 4);
      oob_t = __builtin_bswap32(oob_t);
      const bool oob_state = ((*(base + 0x4046ABB9u) & 0x80u) != 0) || (oob_t >= 1u && oob_t <= 3u);
      // Snap-backs are level hops back inside the line.
      const bool short_hop = d2 > 1.0f && d2 < 60.0f * 60.0f && (dy < 0 ? -dy : dy) < 8.0f;
      // The map spans a lot of height, so only a target far above the player
      // is a real fall-through-the-world respawn.
      const bool fall_respawn = dy >= 175.0f;
      // LB+Up held in the last second = the player asked for the marker.
      const bool player_marker_return = GetTickCount64() - g_rex_marker_return_tick < 1000ull;
      // The game re-sends the snap every frame while you're out, always to
      // the same "last in-bounds" target, and the hop grows past 60u the
      // farther out you get. Remember dropped targets and keep dropping them.
      static std::mutex s_tm;
      static float s_tgt[16][3];
      static int s_ntgt = 0, s_tnext = 0;
      bool known_snap_target = false;
      {
        std::lock_guard<std::mutex> lk(s_tm);
        for (int k = 0; k < s_ntgt; ++k) {
          const float ex = s_tgt[k][0] - tx, ey = s_tgt[k][1] - ty, ez = s_tgt[k][2] - tz;
          if (ex * ex + ey * ey + ez * ez < 0.0025f) { known_snap_target = true; break; }
        }
        if (short_hop && !known_snap_target && !player_marker_return) {
          s_tgt[s_tnext][0] = tx; s_tgt[s_tnext][1] = ty; s_tgt[s_tnext][2] = tz;
          s_tnext = (s_tnext + 1) % 16;
          if (s_ntgt < 16) ++s_ntgt;
        }
      }
      // Input only vouches for teleports that aren't a known snap target
      // (pressing LB+Up near the line must not let the snap-back through).
      const bool vouched = player_marker_return && !known_snap_target;
      const bool drop = !vouched &&
                        (short_hop || (known_snap_target && !fall_respawn) ||
                         (oob_state && !fall_respawn && d2 > 1.0f));
      if (vouched) {
        g_marker_return_target[0] = tx;
        g_marker_return_target[1] = ty;
        g_marker_return_target[2] = tz;
        g_marker_return_tick = GetTickCount64();
      }
      if (drop) {
        g_last_snap_drop_tick = GetTickCount64();
        // LB+Up out of bounds with a custom marker: send this snap-back to
        // the marker instead of dropping it (once per press).
        static uint64_t s_used_press = 0;
        const uint64_t press = g_rex_marker_return_tick;
        if (g_skate3_marker_unlock && g_custom_marker_valid && press != 0 &&
            press != s_used_press && GetTickCount64() - press < 1500ull) {
          s_used_press = press;
          PutGuestVec3(base, msg + 58, g_custom_marker);
          std::memcpy(g_marker_return_target, g_custom_marker, sizeof(g_custom_marker));
          g_marker_return_tick = GetTickCount64();
          sub_82597A70(ctx, base);
        }
        return;
      }
    }
  }
  sub_82597A70(ctx, base);
}

// ---- Generated-code call sites (see skate3_open_roam_guest.h) --------------

namespace {
inline uint32_t LoadU32(uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return __builtin_bswap32(v);
}
inline void StoreU32(uint8_t* base, uint32_t addr, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + addr, &value, 4);
}
}  // namespace

void Skate3OpenRoam_FocusUpdate(PPCContext& ctx, uint8_t* base) {
  if (!g_skate3_sim_follow_player || (ctx.r29.u32 & 0xFF) != 0) return;  // off, or forced focus
  const uint32_t channel = ctx.r31.u32, pos = ctx.r30.u32;
  // A long focus jump after spawn (marker return, respawn) must reload
  // collision at the destination right away or you land on nothing. Only once
  // the player has been in-world a while (doing this during load crashed).
  if (g_skate3_player_live) {
    const float ox = GuestF32(base, channel), oz = GuestF32(base, channel + 8);
    const float jx = GuestF32(base, pos) - ox, jz = GuestF32(base, pos + 8) - oz;
    if ((ox != 0.0f || oz != 0.0f) && jx * jx + jz * jz > 100.0f * 100.0f) {
      ctx.r29.u64 = 1;
      return;
    }
  }
  // Online the collision channel carries a fixed cell list (+40 count, +44..
  // entries = the play-area cells) instead of streaming around the focus.
  // Offline the count is 0; clear it so it streams around the player.
  if (LoadU32(base, channel + 40) != 0) StoreU32(base, channel + 40, 0);
  // +16 = lock count (r11 holds it here). A forced focus at spawn locks the
  // channel; offline it is released shortly after, online never. Release it.
  if (ctx.r11.u32 != 0) {
    ctx.r11.u64 = 0;
    StoreU32(base, channel + 16, 0);
  }
}

bool Skate3OpenRoam_SimFollow() { return g_skate3_sim_follow_player != 0; }

bool Skate3OpenRoam_HideAreaWarning() { return g_skate3_marker_unlock != 0; }

bool Skate3OpenRoam_IgnoreMarkerDisable(PPCContext& ctx, uint8_t* base) {
  // A disable pushes a requester id into the blocker list at +72..+76 (markers
  // work only while it is empty); the online-area script disables markers when
  // you leave the area. Ignore it and keep the list empty.
  if (!g_skate3_marker_unlock || !g_skate3_player_live) return false;
  StoreU32(base, ctx.r3.u32 + 76, LoadU32(base, ctx.r3.u32 + 72));
  return true;
}

namespace skate3::oob_watch {

void InstallKillHook(rex::runtime::FunctionDispatcher* dispatcher) {
  const int mode = REXCVAR_GET(skate3_oob_kill_mode);
  g_skate3_sim_follow_player = (mode & 128) ? 1 : 0;
  g_skate3_marker_unlock = (mode & 1024) ? 1 : 0;
  if (dispatcher == nullptr || mode == 0) return;
  const bool tick = dispatcher->SetFunction(0x82DFEC90u, &Skate3OobKill_TickHook);
  const bool rtp = dispatcher->SetFunction(0x825926F8u, &Skate3OobKill_RequestTeleportHook);
  const bool msg = dispatcher->SetFunction(0x82597A70u, &Skate3OobKill_SkaterMsgHook);
  REXLOG_INFO("[open-roam] kill_mode={} hooks: tick={} respawn={} teleport-msg={}", mode, tick,
              rtp, msg);
}

}  // namespace skate3::oob_watch
