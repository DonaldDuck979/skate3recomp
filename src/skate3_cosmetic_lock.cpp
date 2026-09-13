// [cosmetics] Marker-hat lock.
//
// Every CAS outfit change goes through sub_82DDEDB0 / sub_82DDEEF8: r3 =
// character assembly, r4 = slot index, r5:r6 = 128-bit part id (stored at
// assembly + (slot + 1458) * 16). The tiara (hat slot 18) is the marker the
// renderer turns into the Tylenol bottle (skate3_native_scene.cpp), so it is
// reserved: without the key file (kTiaraKeyFile), picking it in the editor
// does nothing (the previous hat stays on).

#include <cstdint>
#include <filesystem>
#include <system_error>

#include "skate3_init.h"
#include "skate3_open_roam_guest.h"

namespace {

constexpr uint32_t kHatSlot = 18;
constexpr uint64_t kTiaraHi = 0xB43E61808B6BBE41ull;
constexpr uint64_t kTiaraLo = 0x00001E1903E38817ull;

// The tiara is unlocked only on a PC that has this key file (any profile).
constexpr const wchar_t* kTiaraKeyFile = L"E:\\pp\\poopoo\\you smell like a fish\\bottle-head.txt";

bool TiaraUnlocked() {
  std::error_code ec;
  return std::filesystem::is_regular_file(std::filesystem::path(kTiaraKeyFile), ec);
}

}  // namespace

bool Skate3Cosmetic_OnSetPart(PPCContext& ctx, uint8_t* base, int which) {
  (void)base;
  return which == 0 && ctx.r4.u32 == kHatSlot && ctx.r5.u64 == kTiaraHi &&
         ctx.r6.u64 == kTiaraLo && !TiaraUnlocked();
}
