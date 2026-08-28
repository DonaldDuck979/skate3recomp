#include "skate3_net_system.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string_view>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>

namespace skate3::net {

// --- Config cvars -----------------------------------------------------------
// Defined here (one TU per cvar per the SDK rules). All default to the inert
// values so single-player is untouched unless the player opts in.
REXCVAR_DEFINE_BOOL(skate3_net_enable, false, "Net",
                    "Enable fan-made online play (M0: direct-IP LAN)");
REXCVAR_DEFINE_STRING(skate3_net_mode, "host", "Net",
                      "Online role: 'host' or 'client'");
REXCVAR_DEFINE_STRING(skate3_net_host, "127.0.0.1", "Net",
                      "Client mode: host IP/hostname to connect to");
REXCVAR_DEFINE_INT32(skate3_net_port, 34643, "Net",
                     "UDP port to host on / connect to");
REXCVAR_DEFINE_STRING(skate3_net_name, "Skater", "Net",
                      "Local display name shown to other players");
REXCVAR_DEFINE_BOOL(skate3_net_autostart, true, "Net",
                    "Connect to online play at boot (using skate3_net_mode/host) "
                    "as soon as skate3_net_enable is set. Turn off to arm online "
                    "play but wait for an explicit in-game trigger to connect.");

namespace {

// Steady monotonic clock in milliseconds — one domain shared by the send path
// and the receipt timestamps used for interpolation.
uint32_t SteadyNowMs() {
  using namespace std::chrono;
  static const steady_clock::time_point base = steady_clock::now();
  return static_cast<uint32_t>(
      duration_cast<milliseconds>(steady_clock::now() - base).count());
}

// Local state is transmitted at this rate regardless of how often the game
// updates it (design doc target: 20-30 Hz).
constexpr uint32_t kSendIntervalMs = 40;  // 25 Hz.

// Render remote skaters this far behind real receipt time so there is always a
// pair of samples to interpolate between (hides jitter and one dropped packet).
constexpr uint32_t kInterpDelayMs = 100;

// Keep a short history per peer; older than this behind the newest is pruned.
constexpr size_t kMaxSamplesPerPeer = 16;

// Game-mode phase durations (Milestone D).
constexpr uint32_t kCountdownMs = 3000;   // 3-2-1 before the active window.
constexpr uint32_t kResultsMs = 5000;     // winner popup shows, then auto-closes.

// --- small math (no engine dependency) -------------------------------------

float Lerp(float a, float b, float t) { return a + (b - a) * t; }

Vec3 LerpVec3(const Vec3& a, const Vec3& b, float t) {
  return {Lerp(a.x, b.x, t), Lerp(a.y, b.y, t), Lerp(a.z, b.z, t)};
}

// Shortest-arc quaternion interpolation (nlerp: cheap, stable, good enough for
// presentation of remote skaters; upgrade to slerp if wide-angle jumps show).
Quat NlerpQuat(Quat a, Quat b, float t) {
  float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
  if (dot < 0.0f) {  // take the shorter path.
    b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w;
  }
  Quat q{Lerp(a.x, b.x, t), Lerp(a.y, b.y, t), Lerp(a.z, b.z, t), Lerp(a.w, b.w, t)};
  float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (len > 1e-8f) {
    q.x /= len; q.y /= len; q.z /= len; q.w /= len;
  } else {
    q = {0, 0, 0, 1};
  }
  return q;
}

// Extract an orientation quaternion from the 3x3 rotation block of the world
// rows (R[r][c] = rows[r*4 + c]). Assumes near-orthonormal (skater world has no
// scale); the result is normalized. Handedness/transpose conventions cancel as
// long as the remote reconstruction inverts this — a knob to confirm during the
// two-peer visual pass.
Quat MatrixRowsToQuat(const float rows[12]) {
  const float m00 = rows[0], m01 = rows[1], m02 = rows[2];
  const float m10 = rows[4], m11 = rows[5], m12 = rows[6];
  const float m20 = rows[8], m21 = rows[9], m22 = rows[10];
  Quat q;
  float trace = m00 + m11 + m22;
  if (trace > 0.0f) {
    float s = 0.5f / std::sqrt(trace + 1.0f);
    q.w = 0.25f / s;
    q.x = (m21 - m12) * s;
    q.y = (m02 - m20) * s;
    q.z = (m10 - m01) * s;
  } else if (m00 > m11 && m00 > m22) {
    float s = 2.0f * std::sqrt(1.0f + m00 - m11 - m22);
    q.w = (m21 - m12) / s;
    q.x = 0.25f * s;
    q.y = (m01 + m10) / s;
    q.z = (m02 + m20) / s;
  } else if (m11 > m22) {
    float s = 2.0f * std::sqrt(1.0f + m11 - m00 - m22);
    q.w = (m02 - m20) / s;
    q.x = (m01 + m10) / s;
    q.y = 0.25f * s;
    q.z = (m12 + m21) / s;
  } else {
    float s = 2.0f * std::sqrt(1.0f + m22 - m00 - m11);
    q.w = (m10 - m01) / s;
    q.x = (m02 + m20) / s;
    q.y = (m12 + m21) / s;
    q.z = 0.25f * s;
  }
  float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (len > 1e-8f) { q.x /= len; q.y /= len; q.z /= len; q.w /= len; }
  else { q = {0, 0, 0, 1}; }
  return q;
}

struct TimedSample {
  uint32_t local_ms = 0;  // our steady clock at receipt (interpolation domain).
  SkaterState state;
};

// A full-body pose reassembled from its per-mesh datagrams, stamped with the
// clock at which the set completed (its own interpolation timeline).
struct TimedMeshPose {
  uint32_t local_ms = 0;
  uint32_t tick = 0;
  MultiMeshPose pose;
};

// Interpolate one skeleton pose toward another at t (per-bone nlerp/lerp). Both
// must share bone_count; caller guarantees that.
inline void LerpPose(const SkeletonPose& a, const SkeletonPose& b, float t,
                     SkeletonPose& out) {
  out.bone_count = a.bone_count;
  for (uint8_t i = 0; i < a.bone_count; ++i) {
    out.rot[i] = NlerpQuat(a.rot[i], b.rot[i], t);
    out.pos[i] = LerpVec3(a.pos[i], b.pos[i], t);
  }
}

// Interpolate a full-body pose at t, pairing meshes across the two sets by
// CONTENT ID (key) -- the mesh order within a set can change between network
// updates, so blending by array slot would morph one body part into another
// (stretched limbs). Holds a's pose for a mesh with no counterpart in b.
inline void LerpMultiMesh(const MultiMeshPose& a, const MultiMeshPose& b, float t,
                          MultiMeshPose& out) {
  out.mesh_count = a.mesh_count;
  for (uint8_t m = 0; m < a.mesh_count && m < kMaxSkaterMeshes; ++m) {
    out.keys[m] = a.keys[m];
    const SkeletonPose* bm = nullptr;
    for (uint8_t n = 0; n < b.mesh_count && n < kMaxSkaterMeshes; ++n) {
      if (b.keys[n] == a.keys[m]) { bm = &b.meshes[n]; break; }
    }
    if (bm && bm->bone_count == a.meshes[m].bone_count) {
      LerpPose(a.meshes[m], *bm, t, out.meshes[m]);
    } else {
      out.meshes[m] = a.meshes[m];  // no same-key counterpart; hold a's pose.
    }
  }
}

// Per-peer receive history + interpolation. Position and full-body pose ride
// SEPARATE timelines but are both sampled at the same render_ms, so the pose
// stays time-consistent with the root (the fix for motion warping) while
// position never stalls waiting on a pose to reassemble.
struct PeerBuffer {
  std::string name;
  std::string current_trick;          // last trick the peer landed (game name).
  std::string party_leader;           // this peer's party leader name (empty=solo).
  bool party_private = false;         // this peer's party privacy (leader's value wins).
  uint32_t land_seq = 0;              // land counter from this peer's TrickEvents
                                      // (name + count arrive together, no race).
  uint32_t rtt_ms = 0;
  std::deque<TimedSample> samples;    // state (position/orientation) ring.
  std::deque<TimedMeshPose> poses;    // reassembled full-body pose history.

  // Reassembly staging for the current in-flight tick (per-mesh datagrams arrive
  // separately and possibly out of order).
  bool stage_active = false;
  uint32_t stage_tick = 0;
  MultiMeshPose stage;
  bool stage_have[kMaxSkaterMeshes] = {};
  uint32_t last_committed_pose_tick = 0;

  void Push(uint32_t local_ms, const SkaterState& s) {
    // Drop out-of-order/duplicate ticks (unreliable channel can reorder).
    if (!samples.empty() && s.tick != 0 &&
        s.tick <= samples.back().state.tick && samples.back().state.tick != 0) {
      return;
    }
    samples.push_back({local_ms, s});
    while (samples.size() > kMaxSamplesPerPeer) samples.pop_front();
  }

  // Absorb one mesh's pose; commit a complete TimedMeshPose once every mesh for
  // the tick has arrived (order-independent across the per-mesh datagrams).
  void PushMesh(uint32_t local_ms, uint32_t tick, uint32_t key, uint8_t rank,
                uint8_t mesh_count, const SkeletonPose& mesh) {
    if (mesh_count == 0 || mesh_count > kMaxSkaterMeshes || rank >= mesh_count) {
      return;
    }
    if (!stage_active || tick != stage_tick) {
      stage_active = true;
      stage_tick = tick;
      stage = MultiMeshPose{};
      for (bool& h : stage_have) h = false;
    }
    stage.mesh_count = mesh_count;
    stage.meshes[rank] = mesh;
    stage.keys[rank] = key;   // content id used for cross-instance pairing.
    stage_have[rank] = true;
    for (uint8_t i = 0; i < mesh_count; ++i) {
      if (!stage_have[i]) return;  // set not complete yet.
    }
    if (tick <= last_committed_pose_tick) return;  // stale/duplicate set.
    poses.push_back({local_ms, tick, stage});
    last_committed_pose_tick = tick;
    while (poses.size() > kMaxSamplesPerPeer) poses.pop_front();
  }

  // Interpolate position/orientation to render_ms. Returns false if empty.
  bool Sample(uint32_t render_ms, SkaterState& out) const {
    if (samples.empty()) return false;
    if (samples.size() == 1) { out = samples.front().state; return true; }
    if (render_ms <= samples.front().local_ms) {
      out = samples.front().state;
      return true;
    }
    const TimedSample& newest = samples.back();
    if (render_ms >= newest.local_ms) {
      float dt = (render_ms - newest.local_ms) / 1000.0f;
      dt = std::min(dt, 0.25f);  // cap extrapolation to avoid runaway.
      out = newest.state;
      out.position.x += newest.state.velocity.x * dt;
      out.position.y += newest.state.velocity.y * dt;
      out.position.z += newest.state.velocity.z * dt;
      return true;
    }
    for (size_t i = 1; i < samples.size(); ++i) {
      const TimedSample& b = samples[i];
      if (render_ms <= b.local_ms) {
        const TimedSample& a = samples[i - 1];
        uint32_t span = b.local_ms - a.local_ms;
        float t = span > 0 ? float(render_ms - a.local_ms) / float(span) : 0.0f;
        out = a.state;
        out.position = LerpVec3(a.state.position, b.state.position, t);
        out.velocity = LerpVec3(a.state.velocity, b.state.velocity, t);
        out.rotation = NlerpQuat(a.state.rotation, b.state.rotation, t);
        out.anim_id = b.state.anim_id;
        out.trick_id = b.state.trick_id;
        out.board_id = b.state.board_id;
        out.flags = b.state.flags;
        return true;
      }
    }
    out = newest.state;
    return true;
  }

  // Interpolate the full-body pose to the SAME render_ms as Sample(). Returns
  // false if no pose set has been reassembled yet.
  bool SamplePose(uint32_t render_ms, MultiMeshPose& out) const {
    if (poses.empty()) return false;
    if (poses.size() == 1) { out = poses.front().pose; return true; }
    if (render_ms <= poses.front().local_ms) { out = poses.front().pose; return true; }
    const TimedMeshPose& newest = poses.back();
    if (render_ms >= newest.local_ms) { out = newest.pose; return true; }
    for (size_t i = 1; i < poses.size(); ++i) {
      const TimedMeshPose& b = poses[i];
      if (render_ms <= b.local_ms) {
        const TimedMeshPose& a = poses[i - 1];
        uint32_t span = b.local_ms - a.local_ms;
        float t = span > 0 ? float(render_ms - a.local_ms) / float(span) : 0.0f;
        if (a.pose.mesh_count == b.pose.mesh_count) {
          LerpMultiMesh(a.pose, b.pose, t, out);
        } else {
          out = b.pose;
        }
        return true;
      }
    }
    out = newest.pose;
    return true;
  }
};

}  // namespace

// --- Impl -------------------------------------------------------------------

struct Skate3NetSystem::Impl {
  Skate3NetSession session;
  std::thread thread;
  std::atomic<bool> running{false};

  std::mutex mutex;                       // guards the fields below.
  SkaterState local_state;                // latest published local skater.
  bool have_local = false;
  MultiMeshPose local_meshes;             // latest published full-body pose (A1).
  bool have_local_meshes = false;
  uint32_t local_score = 0;               // latest line/combo score (game modes).
  std::string local_trick;                // latest local trick name (game HUD).
  std::string last_sent_trick;            // last trick broadcast (send-loop side).
  uint32_t local_land_seq = 0;            // monotonic; game-thread bumps on each land.
  bool pending_land_send = false;         // a land happened -> send a TrickEvent now.

  // --- Party (v4). Guarded by `mutex`. --------------------------------------
  // `party_leader` is the local player's current party leader NAME:
  //   ""              = solo
  //   local_name      = you are the leader of your own party
  //   other's name    = you're a member of their party
  std::string party_leader;
  bool party_private = false;         // set by the leader; broadcast in PartyState.
  bool last_sent_party_private = false;
  std::string last_sent_party_leader = "\x01";  // sentinel: force first broadcast.
  uint32_t last_party_broadcast_ms = 0;
  // Pending invites received by the local player (from -> received_ms). Bounded
  // small (kMaxPlayers) and time-expired in TrimPartyInvites().
  std::vector<std::pair<std::string, uint32_t>> party_invites;
  static constexpr uint32_t kPartyInviteExpiryMs = 60 * 1000;  // 60s.
  static constexpr uint32_t kPartyRebroadcastMs = 3000;        // heartbeat.


  // Online game mode (Milestone D). Guarded by `mutex`.
  GameMode gm_mode = GameMode::kNone;
  GamePhase gm_phase = GamePhase::kIdle;
  uint32_t gm_round_id = 0;
  uint32_t gm_phase_end_ms = 0;           // local clock when this phase ends.
  uint32_t gm_active_ms = 60000;          // active-window duration this round.
  uint8_t gm_rounds = 1;                   // total rounds requested (1-6).
  uint8_t gm_round = 0;                    // current round number (1-based).
  bool gm_authority = false;              // we run this round's timer/broadcasts.
  bool gm_broadcast_pending = false;      // game thread asked for a (re)broadcast.
  std::map<PeerId, uint32_t> gm_best;     // best line per player (kInvalidPeer=local).
  PeerId gm_winner = kInvalidPeer;

  // --- S.K.A.T.E. mode state (guarded by `mutex`). ---
  bool sk_active = false;
  bool sk_authority = false;               // this instance runs the turn logic.
  SkatePhase sk_phase = SkatePhase::kIdle;
  uint32_t sk_round_id = 0;
  uint8_t sk_round = 0;
  uint8_t sk_rounds = 1;                    // rounds setting (1-3).
  PeerId sk_setter = kInvalidPeer;         // whose turn to SET.
  PeerId sk_current = kInvalidPeer;        // whose attempt is live.
  std::string sk_set_trick;                // the trick to match (kMatch).
  uint32_t sk_phase_end_ms = 0;
  uint8_t sk_sets_this_round = 0;
  std::map<PeerId, int> sk_letters;        // 0-5 (5 = out).
  std::deque<std::string> sk_played;       // rolling used-trick board.
  std::vector<PeerId> sk_queue;            // matchers left to attempt this set.
  // Land-counter baseline per player: captured at attempt start. LAND fires
  // when that player's PlayerLandSeq(id) grows past this. Trick-name buffer
  // change is the sole signal (score is not read for S.K.A.T.E. at all).
  std::map<PeerId, uint32_t> sk_base_land_seq;
  uint32_t sk_diag_ms = 0;                  // rate-limit for [skate-diag] logging.
  bool sk_broadcast_pending = false;
  SkateState sk_view;                      // last full state (for the HUD).
  std::string sk_message;                  // current announce-beat text.
  enum class SkAfter { kNone, kBeginMatch, kNextMatcher, kAdvanceSetter, kEnd };
  SkAfter sk_after = SkAfter::kNone;       // what to do when an announce ends.
  static constexpr uint32_t kSkateAnnounceMs = 1800;
  static constexpr uint32_t kSkateSetMs = 25000;
  static constexpr uint32_t kSkateCountdownMs = 3000;
  static constexpr uint32_t kSkateResultsMs = 8000;

  // Settle detection (auto-freeze): last resting position per player + the clock
  // of the most recent movement by ANY player.
  Vec3 mt_local_pos;
  bool mt_local_init = false;
  std::map<PeerId, Vec3> mt_peer_pos;
  uint32_t mt_last_move_ms = 0;

  void TrackMovement(uint32_t now) {  // caller holds `mutex`.
    constexpr float kMoveSq = 1.0f;   // >1 unit of travel = "moved".
    auto moved = [](const Vec3& a, const Vec3& b) {
      const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
      return (dx * dx + dy * dy + dz * dz) > kMoveSq;
    };
    if (have_local) {
      if (!mt_local_init || moved(local_state.position, mt_local_pos)) {
        mt_last_move_ms = now;
        mt_local_pos = local_state.position;
        mt_local_init = true;
      }
    }
    for (auto& [id, buf] : peers) {
      if (buf.samples.empty()) continue;
      const Vec3& p = buf.samples.back().state.position;
      auto it = mt_peer_pos.find(id);
      if (it == mt_peer_pos.end() || moved(p, it->second)) {
        mt_last_move_ms = now;
        mt_peer_pos[id] = p;
      }
    }
  }

  // Broadcast the current round shell (service thread only -- keeps all enet
  // sends on one thread). Assumes `mutex` held by the caller.
  void BroadcastMode(GamePhase phase, uint32_t phase_ms) {
    GameModeState gm;
    gm.round_id = gm_round_id;
    gm.mode = static_cast<uint8_t>(gm_mode);
    gm.phase = static_cast<uint8_t>(phase);
    gm.phase_ms = phase_ms;
    session.SendGameModeState(gm);
  }

  void ComputeWinner() {
    uint32_t best = 0;
    gm_winner = kInvalidPeer;
    for (const auto& [id, v] : gm_best) {
      if (v > best) { best = v; gm_winner = id; }
    }
  }

  // Advance the round + track best scores. Service thread; locks `mutex`.
  void TickMode(uint32_t now) {
    std::lock_guard<std::mutex> lock(mutex);
    TrackMovement(now);  // always: powers AllPlayersSettled (settle-freeze).
    SkateTick(now);      // S.K.A.T.E. turn logic (authority only; no-op else).
    if (gm_broadcast_pending) {
      uint32_t rem = gm_phase_end_ms > now ? gm_phase_end_ms - now : 0;
      BroadcastMode(gm_phase, rem);
      gm_broadcast_pending = false;
    }
    if (gm_phase == GamePhase::kIdle) return;
    if (gm_phase == GamePhase::kActive) {
      auto track = [&](PeerId id, uint32_t s) {
        uint32_t& b = gm_best[id];
        if (s > b) b = s;
      };
      track(kInvalidPeer, local_score);
      for (auto& [id, buf] : peers) {
        if (!PartyIncludes(id)) continue;   // private party -> exclude outsiders.
        uint32_t s = buf.samples.empty() ? 0u : buf.samples.back().state.score;
        track(id, s);
      }
    }
    if (now < gm_phase_end_ms) return;
    // Phase timer elapsed -> advance (authority also broadcasts the change).
    if (gm_phase == GamePhase::kCountdown) {
      gm_phase = GamePhase::kActive;
      gm_phase_end_ms = now + gm_active_ms;
      gm_best.clear();
      if (gm_authority) BroadcastMode(GamePhase::kActive, gm_active_ms);
    } else if (gm_phase == GamePhase::kActive) {
      ComputeWinner();
      gm_phase = GamePhase::kResults;
      gm_phase_end_ms = now + kResultsMs;
      if (gm_authority) BroadcastMode(GamePhase::kResults, kResultsMs);
    } else {  // kResults -> next round or idle
      if (gm_authority && gm_round < gm_rounds) {
        // More rounds to go: kick off the next round. Followers pick this up
        // via the newer round_id (they reset their best-line scores on the
        // next kActive), so no extra protocol field is needed.
        ++gm_round;
        ++gm_round_id;
        gm_phase = GamePhase::kCountdown;
        gm_phase_end_ms = now + kCountdownMs;
        gm_best.clear();
        gm_winner = kInvalidPeer;
        BroadcastMode(GamePhase::kCountdown, kCountdownMs);
      } else {
        gm_phase = GamePhase::kIdle;
        gm_mode = GameMode::kNone;
        gm_authority = false;
      }
    }
  }

  // Private-party filter (caller holds `mutex`): when the local player is in a
  // private party, non-party peers are excluded from game-mode participation.
  // Local player is ALWAYS included; empty-party or public-party = include all.
  bool PartyIncludes(PeerId id) const {
    if (party_leader.empty() || !party_private) return true;
    if (id == session.local_id() || id == kInvalidPeer) return true;
    auto it = peers.find(id);
    if (it == peers.end()) return false;
    return it->second.party_leader == party_leader;
  }

  // ---------------- S.K.A.T.E. (service thread; caller holds `mutex`) --------
  std::vector<PeerId> SkateOrder() {  // fixed turn order = all players by id.
    std::vector<PeerId> v;
    v.push_back(session.local_id());
    for (const auto& [id, buf] : peers) {
      if (PartyIncludes(id)) v.push_back(id);
    }
    std::sort(v.begin(), v.end());
    return v;
  }
  uint32_t PlayerScore(PeerId id) {
    if (id == session.local_id()) return local_score;
    auto it = peers.find(id);
    return (it != peers.end() && !it->second.samples.empty())
               ? it->second.samples.back().state.score : 0u;
  }
  std::string PlayerTrick(PeerId id) {
    if (id == session.local_id()) return local_trick;
    auto it = peers.find(id);
    return it != peers.end() ? it->second.current_trick : std::string();
  }
  // Monotonic land counter. For a peer it comes from that peer's TrickEvents
  // (each land sends one reliable TrickEvent carrying the combined trick name
  // AND this counter together -- so PlayerTrick(id) and PlayerLandSeq(id) are
  // always consistent, no race between a separate name and count message).
  uint32_t PlayerLandSeq(PeerId id) {
    if (id == session.local_id()) return local_land_seq;
    auto it = peers.find(id);
    return it != peers.end() ? it->second.land_seq : 0u;
  }
  bool SkateEliminated(PeerId id) { return sk_letters[id] >= 5; }
  int SkateActiveCount() {
    int c = 0;
    for (PeerId id : SkateOrder()) if (!SkateEliminated(id)) ++c;
    return c;
  }
  PeerId SkateNextActive(PeerId id) {  // next non-eliminated after id (wraps).
    auto order = SkateOrder();
    if (order.empty()) return id;
    const int n = static_cast<int>(order.size());
    int start = 0;
    for (int i = 0; i < n; ++i) if (order[i] == id) { start = i; break; }
    for (int k = 1; k <= n; ++k) {
      PeerId c = order[(start + k) % n];
      if (!SkateEliminated(c)) return c;
    }
    return id;
  }
  // The ONLY land signal is the per-player monotonic land counter: it bumps
  // in scene.cpp whenever THAT PLAYER's HUD trick-name buffer transitions to
  // a new non-empty value, and rides on SkaterState.trick_id so peers see it
  // via the existing state stream. Setter's turn watches setter's counter;
  // matcher's watches matcher's -- no crossover, no score fallback.
  bool SkateLanded(PeerId id, std::string& trick_out) {
    const std::string ct = PlayerTrick(id);
    if (ct.empty()) return false;
    const uint32_t cs = PlayerLandSeq(id);
    auto bit = sk_base_land_seq.find(id);
    const uint32_t bseq = bit == sk_base_land_seq.end() ? 0u : bit->second;
    if (cs > bseq) { trick_out = ct; return true; }
    return false;
  }
  void SkateResetBaseline(PeerId id) {
    sk_base_land_seq[id] = PlayerLandSeq(id);
  }
  // Rate-limited (~2/s) diagnostic: shows the watched player's land counter vs
  // the attempt-start baseline + whether they're local or remote, so a
  // "match never registers" bug is immediately visible in the log.
  void SkateDiag(const char* phase, PeerId watched, uint32_t now) {
    if (!rex::cvar::Query<bool>("skate3_trick_debug")) return;  // off in release.
    if (now - sk_diag_ms < 500) return;
    sk_diag_ms = now;
    const bool is_local = (watched == session.local_id());
    auto bit = sk_base_land_seq.find(watched);
    const uint32_t base = bit == sk_base_land_seq.end() ? 0u : bit->second;
    REXLOG_INFO("[skate-diag] {} watched={} local={} seq={} base={} trick='{}'",
                phase, watched, is_local ? 1 : 0, PlayerLandSeq(watched), base,
                PlayerTrick(watched));
  }

  // James's rule: a S.K.A.T.E. SET must be a real flip/rotation trick, not a
  // grab, grind, slide, dark-catch/slide, or a front/back flip variant. Any
  // trick whose game HUD name contains one of these keywords is excluded.
  // Substring match runs against the exact game trick text (see
  // 0x401427fd via skate3_trick_addr / auto-scan) -- so rotations like
  // "Frontside Boardslide" or "Overcrook Grind 360" still register as
  // excluded via their family word. Setter landing an excluded trick =
  // invalid set = advance to the next setter (no letter, just move on).
  static bool SkateIsExcludedTrick(const std::string& t) {
    if (t.empty()) return true;   // no trick at all = nothing to set.
    // Case-insensitive substring check (game reports mixed case).
    auto icontains = [&](const char* needle) -> bool {
      const size_t nl = std::strlen(needle);
      if (nl > t.size()) return false;
      for (size_t i = 0; i + nl <= t.size(); ++i) {
        bool ok = true;
        for (size_t k = 0; k < nl; ++k) {
          char a = t[i + k], b = needle[k];
          if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
          if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + 32);
          if (a != b) { ok = false; break; }
        }
        if (ok) return true;
      }
      return false;
    };
    // Family keywords -- broad enough to catch every rotation/variant.
    static constexpr const char* kExcludedKeywords[] = {
        "grab",       "grind",     "slide",      "dark",
        "front flip", "frontflip", "back flip",  "backflip",
        "manual",     "boneless",  "no comply",  "no-comply",
        // Specific grab names (some Skate 3 tricks don't include "grab"):
        "indy",       "melon",     "method",     "mute",
        "sacktap",    "tuck",      "judo",       "stalefish",
        "rocket",     "roast",     "sad",        "christ",
        "airwalk",    "nose grab", "tail grab",  "seat belt",
        // Grind families that don't always include the word "grind":
        "5-0",        "50-50",     "smith",      "feeble",
        "crooked",    "overcrook", "willy",      "salad",
        "losi",       "nose pick", "nosepick",   "5.0",
    };
    for (const char* kw : kExcludedKeywords) {
      if (icontains(kw)) return true;
    }
    return false;
  }

  void BuildSkateView() {
    SkateState& v = sk_view;
    v.round_id = sk_round_id;
    v.phase = static_cast<uint8_t>(sk_phase);
    v.round = sk_round;
    v.setter = sk_setter;
    v.current = sk_current;
    const uint32_t now = SteadyNowMs();
    v.phase_ms = sk_phase_end_ms > now ? sk_phase_end_ms - now : 0;
    const uint8_t tl = static_cast<uint8_t>(
        std::min<size_t>(sk_set_trick.size(), kMaxTrickNameLength));
    std::memset(v.set_trick, 0, sizeof(v.set_trick));
    std::memcpy(v.set_trick, sk_set_trick.data(), tl);
    v.set_trick_length = tl;
    const uint8_t ml = static_cast<uint8_t>(std::min<size_t>(sk_message.size(), 48));
    std::memset(v.message, 0, sizeof(v.message));
    std::memcpy(v.message, sk_message.data(), ml);
    v.message_length = ml;
    auto order = SkateOrder();
    const uint8_t pc =
        static_cast<uint8_t>(std::min<size_t>(order.size(), kMaxPlayers));
    v.player_count = pc;
    for (uint8_t i = 0; i < pc; ++i) {
      v.players[i].id = order[i];
      v.players[i].letters = static_cast<uint8_t>(sk_letters[order[i]]);
    }
  }
  void BroadcastSkate() { BuildSkateView(); session.SendSkateState(sk_view); }

  void StartSkateAuthority(uint8_t rounds, uint32_t now) {
    sk_active = true;
    sk_authority = true;
    sk_rounds = rounds < 1 ? 1 : (rounds > 3 ? 3 : rounds);
    sk_round = 0;
    sk_round_id++;
    sk_letters.clear();
    sk_played.clear();
    sk_set_trick.clear();
    sk_sets_this_round = 0;
    sk_phase = SkatePhase::kCountdown;
    sk_phase_end_ms = now + kSkateCountdownMs;
    gm_mode = GameMode::kNone;  // stop any Spot Battle.
    gm_phase = GamePhase::kIdle;
    BroadcastSkate();
  }
  std::string SkateName(PeerId id) {
    std::string base;
    if (id == session.local_id())
      base = rex::cvar::Query<std::string>("skate3_net_name");
    return DisplayName(id, base);
  }
  void SkateAnnounce(const std::string& msg, uint32_t ms, SkAfter after,
                     uint32_t now) {
    sk_message = msg;
    sk_after = after;
    sk_phase = SkatePhase::kAnnounce;
    sk_phase_end_ms = now + ms;
    BroadcastSkate();
  }
  void SkateResume(uint32_t now) {
    const SkAfter a = sk_after;
    sk_after = SkAfter::kNone;
    sk_message.clear();
    switch (a) {
      case SkAfter::kBeginMatch:
        sk_phase = SkatePhase::kMatch;
        sk_current = sk_queue.empty() ? sk_setter : sk_queue.front();
        sk_phase_end_ms = now + kSkateSetMs;
        SkateResetBaseline(sk_current);
        BroadcastSkate();
        break;
      case SkAfter::kNextMatcher:
        if (SkateActiveCount() <= 1) {
          sk_phase = SkatePhase::kResults;
          sk_phase_end_ms = now + kSkateResultsMs;
          BroadcastSkate();
        } else if (sk_queue.empty()) {
          SkateAdvanceSetter(now);
        } else {
          sk_phase = SkatePhase::kMatch;
          sk_current = sk_queue.front();
          sk_phase_end_ms = now + kSkateSetMs;
          SkateResetBaseline(sk_current);
          BroadcastSkate();
        }
        break;
      case SkAfter::kAdvanceSetter:
        SkateAdvanceSetter(now);
        break;
      default:
        break;
    }
  }
  void SkateBeginSet(uint32_t now) {
    sk_phase = SkatePhase::kSet;
    sk_current = sk_setter;
    sk_set_trick.clear();
    sk_phase_end_ms = now + kSkateSetMs;
    SkateResetBaseline(sk_setter);
    BroadcastSkate();
  }
  // Round is over when only one active player remains OR everyone spelled
  // SKATE. Each ROUND is a complete elimination game; sk_rounds runs multiple
  // back-to-back rounds where letters RESET between them (James's rule --
  // otherwise letters carrying over across rounds makes rounds meaningless).
  bool SkateRoundOver() const {
    int active = 0;
    // Count active (non-eliminated) across the whole roster. Copy of
    // SkateActiveCount but doesn't rely on the mutable operator[] on letters.
    if (sk_letters_get(session.local_id()) < 5) ++active;
    for (const auto& [id, buf] : peers) {
      if (sk_letters_get(id) < 5) ++active;
    }
    return active <= 1;
  }
  // const-safe letter lookup (map operator[] on const is not usable).
  int sk_letters_get(PeerId id) const {
    auto it = sk_letters.find(id);
    return it == sk_letters.end() ? 0 : it->second;
  }
  void SkateAdvanceSetter(uint32_t now) {
    // Round-end check: if only one non-eliminated player remains, this round
    // is finished. Either advance to a fresh round (with letters cleared) or
    // end the game.
    if (SkateRoundOver()) {
      if (sk_round < sk_rounds) {
        // Next round -- clear letters, reset the played-list, keep the roster.
        ++sk_round;
        sk_letters.clear();
        sk_played.clear();
        sk_set_trick.clear();
        auto order = SkateOrder();
        sk_setter = order.empty() ? kInvalidPeer : order.front();
        SkateAnnounce("Round " + std::to_string(sk_round) + " -- new SKATE",
                      3000, SkAfter::kAdvanceSetter, now);
        return;
      }
      // Last round complete -- results.
      sk_phase = SkatePhase::kResults;
      sk_phase_end_ms = now + kSkateResultsMs;
      BroadcastSkate();
      return;
    }
    // Round not over yet -- next non-eliminated setter goes.
    sk_setter = SkateNextActive(sk_setter);
    SkateBeginSet(now);
  }
  void SkateTick(uint32_t now) {  // authority only.
    if (!sk_active || !sk_authority) return;
    if (sk_broadcast_pending) { BroadcastSkate(); sk_broadcast_pending = false; }
    switch (sk_phase) {
      case SkatePhase::kCountdown:
        if (now >= sk_phase_end_ms) {
          sk_round = 1;
          sk_letters.clear();          // fresh SKATE this round.
          sk_played.clear();
          auto order = SkateOrder();
          sk_setter = order.empty() ? kInvalidPeer : order.front();
          SkateBeginSet(now);
        }
        break;
      case SkatePhase::kSet: {
        std::string t;
        SkateDiag("SET", sk_setter, now);
        if (SkateLanded(sk_setter, t)) {
          REXLOG_INFO("[skate] setter={} landed='{}' excluded={}",
                      sk_setter, t, SkateIsExcludedTrick(t) ? 1 : 0);
          // James's rule: reject grabs / grinds / slides / dark-* /
          // front+back flips / manuals as valid SETS. The trick still
          // "registered" (we saw it), but it doesn't count -- setter's
          // turn passes to the next player. NO letter on the setter.
          // Rebaseline so their NEXT trick attempt registers cleanly.
          if (SkateIsExcludedTrick(t)) {
            SkateResetBaseline(sk_setter);
            SkateAnnounce(SkateName(sk_setter) + " tried " + t +
                              " -- not a valid set",
                          4300, SkAfter::kAdvanceSetter, now);
            break;
          }
          bool used = false;
          for (const auto& p : sk_played) if (p == t) { used = true; break; }
          if (used) {  // trick already on the board = invalid, paced beat.
            SkateResetBaseline(sk_setter);   // let them try again next round.
            SkateAnnounce("Repeated Trick!  " + t, 4900,
                          SkAfter::kAdvanceSetter, now);
          } else {
            sk_set_trick = t;
            sk_played.push_back(t);
            while (sk_played.size() > 8) sk_played.pop_front();
            sk_queue.clear();
            for (PeerId id : SkateOrder())
              if (id != sk_setter && !SkateEliminated(id)) sk_queue.push_back(id);
            if (sk_queue.empty()) {
              SkateAnnounce(SkateName(sk_setter) + " set  " + t, 4500,
                            SkAfter::kAdvanceSetter, now);
            } else {
              SkateAnnounce(SkateName(sk_setter) + " set:  " + t, 4900,
                            SkAfter::kBeginMatch, now);
            }
          }
        } else if (now >= sk_phase_end_ms) {
          SkateAnnounce(SkateName(sk_setter) + " set nothing", 4300,
                        SkAfter::kAdvanceSetter, now);
        }
        break;
      }
      case SkatePhase::kMatch: {
        std::string t, msg;
        bool done = false;
        SkateDiag("MATCH", sk_current, now);
        if (SkateLanded(sk_current, t)) {
          REXLOG_INFO("[skate] matcher={} landed='{}' set='{}'",
                      sk_current, t, sk_set_trick);
          if (t == sk_set_trick) {
            msg = SkateName(sk_current) + " matched:  " + t;
          } else if (SkateIsExcludedTrick(t)) {
            // Excluded trick (grab/grind/dark/etc.) still registers as a
            // land, but it's clearly not the set trick -- letter.
            sk_letters[sk_current]++;
            msg = SkateName(sk_current) + " did " + t +
                  " (not the set) -- letter";
          } else {
            sk_letters[sk_current]++;
            msg = SkateName(sk_current) + " did " + t + " -- letter";
          }
          done = true;
        } else if (now >= sk_phase_end_ms) {
          sk_letters[sk_current]++;
          msg = SkateName(sk_current) + " missed -- letter";
          done = true;
        }
        if (done) {
          if (!sk_queue.empty()) sk_queue.erase(sk_queue.begin());
          SkateAnnounce(msg, 4700, SkAfter::kNextMatcher, now);
        }
        break;
      }
      case SkatePhase::kAnnounce:
        if (now >= sk_phase_end_ms) SkateResume(now);
        break;
      case SkatePhase::kResults:
        if (now >= sk_phase_end_ms) {
          sk_phase = SkatePhase::kIdle;
          sk_active = false;
          sk_authority = false;
          BroadcastSkate();
        }
        break;
      default: break;
    }
  }

  std::map<PeerId, PeerBuffer> peers;     // remote receive buffers.

  // Local capture bookkeeping (game thread only; no lock needed).
  Vec3 prev_local_pos;
  uint32_t prev_local_ms = 0;
  bool have_prev = false;
  uint32_t local_tick = 0;

  void OnMessage(PeerId from, const DecodedMessage& msg) {
    // Runs on the service thread (inside session.Tick).
    std::lock_guard<std::mutex> lock(mutex);
    switch (msg.header.type) {
      case MessageType::kSkaterState: {
        PeerBuffer& pb = peers[from];
        pb.Push(SteadyNowMs(), msg.skater_state);
        break;
      }
      case MessageType::kMeshPose: {
        PeerBuffer& pb = peers[from];
        SkeletonPose mesh;
        mesh.bone_count = msg.mesh_pose.bone_count;
        for (uint8_t i = 0; i < msg.mesh_pose.bone_count; ++i) {
          mesh.rot[i] = msg.mesh_pose_rot[i];
          mesh.pos[i] = msg.mesh_pose_pos[i];
        }
        pb.PushMesh(SteadyNowMs(), msg.mesh_pose.tick, msg.mesh_pose.key,
                    msg.mesh_pose.rank, msg.mesh_pose.mesh_count, mesh);
        // [a1-net] rate-limited receive diagnostic (~1/s across meshes).
        static uint32_t s_recv_log = 0;
        if ((++s_recv_log % 250) == 0) {
          REXLOG_INFO("[a1-net] recv mesh pose id={} rank={}/{} bones={}",
                      from, msg.mesh_pose.rank, msg.mesh_pose.mesh_count,
                      msg.mesh_pose.bone_count);
        }
        break;
      }
      case MessageType::kGameModeState: {
        // Adopt a round started/advanced by another peer (the authority). We
        // become a follower: run a local timer from phase_ms; best-line scores
        // are tracked locally from the replicated per-player scores.
        const GameModeState& gm = msg.game_mode;
        const uint32_t now = SteadyNowMs();
        const bool newer_round = gm.round_id > gm_round_id;
        const bool same_round_adv = gm.round_id == gm_round_id &&
                                    gm.phase >= static_cast<uint8_t>(gm_phase);
        if (newer_round || same_round_adv) {
          if (newer_round) { gm_best.clear(); gm_winner = kInvalidPeer; }
          gm_round_id = gm.round_id;
          gm_mode = static_cast<GameMode>(gm.mode);
          const GamePhase new_phase = static_cast<GamePhase>(gm.phase);
          if (new_phase == GamePhase::kActive && gm_phase != GamePhase::kActive) {
            gm_best.clear();  // reset scores when the active window opens.
          }
          gm_phase = new_phase;
          gm_authority = false;
          gm_phase_end_ms = now + gm.phase_ms;
          if (new_phase == GamePhase::kResults) ComputeWinner();
        }
        break;
      }
      case MessageType::kChatMessage: {
        // M0: log chat; a chat UI comes later. Explicit length: the buffer is
        // not guaranteed NUL-terminated.
        size_t n = msg.chat.length <= kMaxChatLength ? msg.chat.length : kMaxChatLength;
        REXLOG_INFO("[net] chat from {}: {}", from, std::string_view(msg.chat.text, n));
        break;
      }
      case MessageType::kTrickEvent: {
        // mutex already held for the whole OnMessage. Each TrickEvent = one
        // LAND: store the combined trick name AND the land counter together
        // (trick_id carries the sender's land_seq), so S.K.A.T.E. adjudication
        // reads a consistent (name, count) pair.
        uint8_t n = msg.trick_event.name_length;
        if (n > kMaxTrickNameLength) n = kMaxTrickNameLength;
        peers[from].current_trick.assign(msg.trick_event.name, n);
        peers[from].land_seq = msg.trick_event.trick_id;
        break;
      }
      case MessageType::kSkateState: {
        // Follower: adopt the authority's S.K.A.T.E. state + run a local timer.
        if (!sk_authority) {
          sk_view = msg.skate_state;
          sk_active = SkatePhase(sk_view.phase) != SkatePhase::kIdle;
          sk_phase = static_cast<SkatePhase>(sk_view.phase);
          sk_round = sk_view.round;
          sk_setter = sk_view.setter;
          sk_current = sk_view.current;
          sk_set_trick.assign(sk_view.set_trick, sk_view.set_trick_length);
          sk_message.assign(sk_view.message, sk_view.message_length);
          sk_phase_end_ms = SteadyNowMs() + sk_view.phase_ms;
        }
        break;
      }
      case MessageType::kBailEvent:
        // Consumed by the replication layer once events drive presentation.
        break;
      case MessageType::kPartyInvite: {
        // Only act if the invite targets the local player's CURRENT name (the
        // one skate3_net_name resolves to). Everyone else silently ignores.
        const uint8_t tn = std::min<uint8_t>(msg.party_invite.target_name_length,
                                             kMaxNameLength);
        const uint8_t in = std::min<uint8_t>(msg.party_invite.inviter_name_length,
                                             kMaxNameLength);
        std::string target(msg.party_invite.target_name, tn);
        std::string inviter(msg.party_invite.inviter_name, in);
        std::string me = rex::cvar::Query<std::string>("skate3_net_name");
        if (!target.empty() && !inviter.empty() && target == me) {
          const uint32_t now = SteadyNowMs();
          // Dedupe: refresh the timestamp on an existing invite from the same
          // inviter rather than adding a second entry.
          bool refreshed = false;
          for (auto& p : party_invites) {
            if (p.first == inviter) { p.second = now; refreshed = true; break; }
          }
          if (!refreshed) party_invites.emplace_back(inviter, now);
          if (party_invites.size() > kMaxPlayers) party_invites.erase(party_invites.begin());
          REXLOG_INFO("[net] party invite from '{}'", inviter);
        }
        break;
      }
      case MessageType::kPartyState: {
        const uint8_t n = std::min<uint8_t>(msg.party_state.leader_name_length,
                                            kMaxNameLength);
        peers[from].party_leader.assign(msg.party_state.leader_name, n);
        peers[from].party_private = (msg.party_state.is_private != 0);
        // If the leader of MY party just went solo (or left this session), fall
        // back to solo so nobody stays orphaned in a dead party.
        if (!party_leader.empty()) {
          std::string me = rex::cvar::Query<std::string>("skate3_net_name");
          if (party_leader != me && peers[from].name == party_leader &&
              peers[from].party_leader != party_leader) {
            party_leader.clear();
            REXLOG_INFO("[net] party leader '{}' left; local player is solo",
                        peers[from].name);
          }
        }
        break;
      }
      default:
        break;
    }
  }

  void OnPeerEvent(const RemotePeer& peer, bool joined) {
    std::lock_guard<std::mutex> lock(mutex);
    if (joined) {
      peers[peer.id].name = peer.name;
      REXLOG_INFO("[net] player joined: id={} name={}", peer.id, peer.name);
    } else {
      peers.erase(peer.id);
      REXLOG_INFO("[net] player left: id={}", peer.id);
    }
  }

  // Display name for a player, disambiguating DUPLICATE names with a
  // join-order suffix: two "Bob"s become "Bob (1)" (joined first, lower id)
  // and "Bob (2)". Unique names get no suffix. The roster (local player + all
  // peers) is identical on every instance and ids are globally assigned, so
  // every client computes the same suffixes. Caller holds `mutex`.
  std::string DisplayName(PeerId id, const std::string& base_in) const {
    const PeerId lid = session.local_id();
    std::string lname = rex::cvar::Query<std::string>("skate3_net_name");
    if (lname.empty()) lname = "Player " + std::to_string(lid);
    auto name_of = [&](PeerId pid) -> std::string {
      if (pid == lid) return lname;
      auto it = peers.find(pid);
      return (it != peers.end() && !it->second.name.empty())
                 ? it->second.name
                 : ("Player " + std::to_string(pid));
    };
    const std::string base = base_in.empty() ? name_of(id) : base_in;
    std::vector<PeerId> same;
    if (lname == base) same.push_back(lid);
    for (const auto& [pid, buf] : peers) {
      const std::string n =
          buf.name.empty() ? ("Player " + std::to_string(pid)) : buf.name;
      if (n == base) same.push_back(pid);
    }
    if (same.size() <= 1) return base;
    std::sort(same.begin(), same.end());
    int rank = 1;
    for (size_t i = 0; i < same.size(); ++i) {
      if (same[i] == id) { rank = static_cast<int>(i) + 1; break; }
    }
    return base + " (" + std::to_string(rank) + ")";
  }

  void Run() {
    uint32_t last_send = 0;
    while (running.load(std::memory_order_relaxed)) {
      uint32_t now = SteadyNowMs();
      session.Tick(now);
      TickMode(now);  // advance game-mode round + track best scores.

      if (now - last_send >= kSendIntervalMs) {
        last_send = now;
        SkaterState to_send;
        MultiMeshPose meshes_to_send;
        bool send = false;
        bool send_meshes = false;
        bool send_trick = false;
        std::string trick_to_send;
        uint32_t land_seq_to_send = 0;
        bool send_party_state = false;
        std::string party_state_to_send;
        {
          std::lock_guard<std::mutex> lock(mutex);
          if (have_local) {
            to_send = local_state;
            to_send.score = local_score;  // ride the latest score on the state.
            to_send.trick_id = local_land_seq;  // monotonic S.K.A.T.E. land counter.
            send = true;
          }
          // Send a TrickEvent per LAND (queued by RegisterLocalLand), carrying
          // the combined trick name + the land counter together. Reliable, so
          // no land is dropped and the peer's (name,count) stay consistent.
          if (pending_land_send) {
            trick_to_send = local_trick;
            land_seq_to_send = local_land_seq;
            last_sent_trick = local_trick;
            pending_land_send = false;
            send_trick = true;
          }
          // Party heartbeat: on change immediately, or every kPartyRebroadcastMs
          // so a fresh joiner learns the roster's party state without waiting.
          if (party_leader != last_sent_party_leader ||
              party_private != last_sent_party_private ||
              (now - last_party_broadcast_ms) >= Impl::kPartyRebroadcastMs) {
            party_state_to_send = party_leader;
            last_sent_party_leader = party_leader;
            last_sent_party_private = party_private;
            last_party_broadcast_ms = now;
            send_party_state = true;
          }
          // Trim expired invites in the same critical section (cheap, bounded).
          for (auto it = party_invites.begin(); it != party_invites.end();) {
            if (now - it->second > Impl::kPartyInviteExpiryMs) {
              it = party_invites.erase(it);
            } else {
              ++it;
            }
          }
          if (have_local_meshes && local_meshes.mesh_count > 0) {
            meshes_to_send = local_meshes;
            send_meshes = true;
          }
          // Refresh RTT from the session roster for the diagnostics overlay.
          for (const RemotePeer& rp : session.peers()) {
            auto it = peers.find(rp.id);
            if (it != peers.end()) it->second.rtt_ms = rp.rtt_ms;
          }
        }
        if (send) {
          // Position-only state first: presence + the mirror fallback must never
          // depend on the (much larger) pose stream arriving. Then stream each
          // body-part mesh's pose as its own datagram, all tagged with this tick
          // so the receiver reassembles a full set (A1 full-body). A dropped mesh
          // just means that frame's set doesn't commit -- position still flows.
          session.SendSkaterState(to_send);
          if (send_meshes) {
            uint8_t n = meshes_to_send.mesh_count;
            if (n > kMaxSkaterMeshes) n = static_cast<uint8_t>(kMaxSkaterMeshes);
            for (uint8_t i = 0; i < n; ++i) {
              MeshPose mp;
              mp.tick = to_send.tick;
              mp.key = meshes_to_send.keys[i];
              mp.rank = i;
              mp.mesh_count = n;
              mp.bone_count = meshes_to_send.meshes[i].bone_count;
              session.SendMeshPose(mp, meshes_to_send.meshes[i].rot,
                                   meshes_to_send.meshes[i].pos);
            }
          }
          // [a1-net] rate-limited send diagnostic (~1/s at 25 Hz).
          static uint32_t s_send_log = 0;
          if (send_trick) {
            TrickEvent ev;
            ev.time_ms = now;
            ev.trick_id = land_seq_to_send;  // carries the land counter.
            uint8_t n = static_cast<uint8_t>(
                std::min<size_t>(trick_to_send.size(), kMaxTrickNameLength));
            std::memcpy(ev.name, trick_to_send.data(), n);
            ev.name_length = n;
            session.SendTrickEvent(ev);
            REXLOG_INFO("[net] sent land '{}' (#{})", trick_to_send,
                        land_seq_to_send);
          }
          if (send_party_state) {
            PartyState ps;
            uint8_t n = static_cast<uint8_t>(
                std::min<size_t>(party_state_to_send.size(), kMaxNameLength));
            std::memcpy(ps.leader_name, party_state_to_send.data(), n);
            ps.leader_name_length = n;
            ps.is_private = last_sent_party_private ? 1 : 0;
            session.SendPartyState(ps);
          }
          if ((++s_send_log % 25) == 0) {
            REXLOG_INFO("[a1-net] send: pos + meshes={} (bones/mesh={})",
                        send_meshes ? meshes_to_send.mesh_count : 0,
                        send_meshes ? meshes_to_send.meshes[0].bone_count : 0);
          }
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }
};

// --- Skate3NetSystem --------------------------------------------------------

Skate3NetSystem::Skate3NetSystem() : impl_(new Impl()) {}

Skate3NetSystem::~Skate3NetSystem() {
  Shutdown();
  delete impl_;
  impl_ = nullptr;
}

void Skate3NetSystem::Initialize() {
  if (active_) return;
  if (!rex::cvar::Query<bool>("skate3_net_enable")) {
    REXLOG_INFO("[net] online play disabled (set skate3_net_enable=true to enable)");
    return;
  }
  if (rex::cvar::Query<bool>("skate3_net_autostart")) {
    Connect();
  } else {
    REXLOG_INFO("[net] online play armed; waiting for in-game trigger "
                "(skate3_net_autostart=false)");
  }
}

void Skate3NetSystem::Connect() {
  if (active_) return;
  if (!rex::cvar::Query<bool>("skate3_net_enable")) {
    REXLOG_INFO("[net] connect ignored: online play disabled "
                "(set skate3_net_enable=true)");
    return;
  }

  SessionConfig cfg;
  std::string mode = rex::cvar::Query<std::string>("skate3_net_mode");
  cfg.mode = (mode == "client") ? SessionMode::kClient : SessionMode::kHost;
  cfg.host_address = rex::cvar::Query<std::string>("skate3_net_host");
  cfg.port = static_cast<uint16_t>(rex::cvar::Query<int32_t>("skate3_net_port"));
  cfg.display_name = rex::cvar::Query<std::string>("skate3_net_name");
  cfg.max_players = 2;  // M0.

  impl_->session.SetMessageHandler(
      [this](PeerId from, const DecodedMessage& m) { impl_->OnMessage(from, m); });
  impl_->session.SetPeerEventHandler(
      [this](const RemotePeer& p, bool joined) { impl_->OnPeerEvent(p, joined); });

  if (!impl_->session.Start(cfg)) {
    REXLOG_WARN("[net] session failed to start: {}", impl_->session.last_error());
    return;
  }

  impl_->running.store(true, std::memory_order_relaxed);
  impl_->thread = std::thread([this] { impl_->Run(); });
  active_ = true;
  REXLOG_INFO("[net] online play active: mode={} port={} name={}",
              mode, cfg.port, cfg.display_name);
}

void Skate3NetSystem::Shutdown() {
  if (!active_ && !impl_->running.load()) return;
  impl_->running.store(false, std::memory_order_relaxed);
  if (impl_->thread.joinable()) impl_->thread.join();
  impl_->session.Stop();
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->peers.clear();
    impl_->have_local = false;
  }
  active_ = false;
}

void Skate3NetSystem::SetLocalSkaterState(const SkaterState& state) {
  if (!active_) return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->local_state = state;
  impl_->have_local = true;
}

void Skate3NetSystem::SetLocalSkaterMeshPoses(const MultiMeshPose& poses) {
  if (!active_) return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->local_meshes = poses;
  impl_->have_local_meshes = poses.mesh_count > 0;
}

void Skate3NetSystem::SetLocalScore(uint32_t score) {
  if (!active_) return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->local_score = score;
}

uint32_t Skate3NetSystem::LocalScore() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->local_score;
}

void Skate3NetSystem::SetLocalTrick(const std::string& trick) {
  if (!active_) return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->local_trick = trick;  // service-thread send loop broadcasts on change.
}

std::string Skate3NetSystem::LocalTrick() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->local_trick;
}

void Skate3NetSystem::SetLocalLandSeq(uint32_t seq) {
  if (!active_) return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->local_land_seq = seq;  // rides on SkaterState.trick_id at next send.
}

void Skate3NetSystem::RegisterLocalLand(const std::string& trick_name) {
  if (!active_) return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->local_trick = trick_name;     // the full combined name (base+rotation).
  ++impl_->local_land_seq;             // one land.
  impl_->pending_land_send = true;     // service thread sends a TrickEvent now.
}

bool Skate3NetSystem::AllPlayersSettled(uint32_t ms) const {
  if (!active_) return false;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->mt_local_init && impl_->mt_peer_pos.empty()) return false;
  const uint32_t now = SteadyNowMs();
  return impl_->mt_last_move_ms != 0 &&
         (now - impl_->mt_last_move_ms) >= ms;
}

void Skate3NetSystem::StartSpotBattle(uint32_t duration_s, uint32_t rounds) {
  if (!active_) return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->gm_phase != GamePhase::kIdle) return;  // a round is already running.
  impl_->gm_mode = GameMode::kSpotBattle;
  impl_->gm_phase = GamePhase::kCountdown;
  impl_->gm_round_id++;
  impl_->gm_rounds = static_cast<uint8_t>(std::clamp<uint32_t>(rounds, 1u, 6u));
  impl_->gm_round = 1;
  impl_->gm_active_ms = (duration_s == 0 ? 60u : duration_s) * 1000u;
  impl_->gm_phase_end_ms = SteadyNowMs() + kCountdownMs;
  impl_->gm_authority = true;
  impl_->gm_best.clear();
  impl_->gm_winner = kInvalidPeer;
  impl_->gm_broadcast_pending = true;  // service thread emits the broadcast.
  REXLOG_INFO("[net] Spot Battle started ({}s x {} round(s))", duration_s,
              impl_->gm_rounds);
}

void Skate3NetSystem::StartSkate(uint32_t rounds) {
  if (!active_) return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->sk_active) return;  // already running.
  impl_->StartSkateAuthority(static_cast<uint8_t>(rounds), SteadyNowMs());
  REXLOG_INFO("[net] S.K.A.T.E. started (rounds={})", impl_->sk_rounds);
}

// --- Party (v4) -------------------------------------------------------------

void Skate3NetSystem::InvitePlayer(const std::string& target_name) {
  if (!active_ || target_name.empty()) return;
  std::string me = rex::cvar::Query<std::string>("skate3_net_name");
  if (target_name == me) return;  // can't invite yourself.
  // If we're not already in a party, we IMPLICITLY become the leader of our
  // own party the moment we invite someone -- so members joining our party
  // find its leader in the roster (us).
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->party_leader.empty()) {
      impl_->party_leader = me;
    }
  }
  PartyInvite inv;
  uint8_t in = static_cast<uint8_t>(std::min<size_t>(me.size(), kMaxNameLength));
  uint8_t tn = static_cast<uint8_t>(std::min<size_t>(target_name.size(), kMaxNameLength));
  std::memcpy(inv.inviter_name, me.data(), in);
  inv.inviter_name_length = in;
  std::memcpy(inv.target_name, target_name.data(), tn);
  inv.target_name_length = tn;
  impl_->session.SendPartyInvite(inv);
  REXLOG_INFO("[net] party invite sent to '{}'", target_name);
}

void Skate3NetSystem::AcceptInvite(const std::string& from_name) {
  if (!active_ || from_name.empty()) return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  // Only accept an invite we actually received.
  bool found = false;
  for (auto it = impl_->party_invites.begin(); it != impl_->party_invites.end();) {
    if (it->first == from_name) { it = impl_->party_invites.erase(it); found = true; }
    else { ++it; }
  }
  if (!found) return;
  impl_->party_leader = from_name;  // join their party; send-loop broadcasts.
  // Inherit the leader's current privacy so filters apply immediately (before
  // the next PartyState heartbeat lands).
  for (const auto& [pid, buf] : impl_->peers) {
    if (buf.name == from_name) { impl_->party_private = buf.party_private; break; }
  }
  REXLOG_INFO("[net] joined party led by '{}'", from_name);
}

void Skate3NetSystem::LeaveParty() {
  if (!active_) return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->party_leader.empty()) return;
  impl_->party_leader.clear();  // solo; send-loop broadcasts the change.
  impl_->party_private = false; // clear privacy so we don't stay in stealth solo.
  REXLOG_INFO("[net] left party");
}

void Skate3NetSystem::SetPartyPrivate(bool is_private) {
  if (!active_) return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::string me = rex::cvar::Query<std::string>("skate3_net_name");
  // Only the leader can toggle privacy. If solo, becoming private implicitly
  // makes you a solo private party (leader of yourself) so the filter applies.
  if (impl_->party_leader.empty()) {
    impl_->party_leader = me;  // implicit solo party.
  } else if (impl_->party_leader != me) {
    return;  // members can't flip a leader's setting.
  }
  impl_->party_private = is_private;
  REXLOG_INFO("[net] party privacy -> {}", is_private ? "PRIVATE" : "public");
}

bool Skate3NetSystem::InPrivateParty() const {
  if (!active_) return false;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->party_leader.empty()) return false;
  std::string me = rex::cvar::Query<std::string>("skate3_net_name");
  if (impl_->party_leader == me) {
    return impl_->party_private;  // you're the leader -- your flag rules.
  }
  // You're a member: find the leader in the roster and use their flag.
  for (const auto& [pid, buf] : impl_->peers) {
    if (buf.name == impl_->party_leader) return buf.party_private;
  }
  // Leader not in roster (yet, or dropped) -- fall back to our own last-seen.
  return impl_->party_private;
}

bool Skate3NetSystem::InSameParty(PeerId id) const {
  if (!active_ || id == kInvalidPeer) return false;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->party_leader.empty()) return false;
  auto it = impl_->peers.find(id);
  if (it == impl_->peers.end()) return false;
  return it->second.party_leader == impl_->party_leader;
}

PartyView Skate3NetSystem::GetPartyView() const {
  PartyView v;
  if (!active_) return v;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::string me = rex::cvar::Query<std::string>("skate3_net_name");
  v.leader_name = impl_->party_leader;
  v.in_party = !v.leader_name.empty();
  v.you_are_leader = v.in_party && v.leader_name == me;
  // Effective privacy = leader's flag. When you're solo, effective = false;
  // when you're the leader, your own flag; else look up the leader in peers.
  if (v.in_party) {
    if (v.you_are_leader) {
      v.is_private = impl_->party_private;
    } else {
      v.is_private = impl_->party_private;  // fallback if leader missing.
      for (const auto& [pid, buf] : impl_->peers) {
        if (buf.name == v.leader_name) { v.is_private = buf.party_private; break; }
      }
    }
  }

  if (v.in_party) {
    // Roster: everyone whose party_leader matches ours (leader + members).
    auto add = [&](PeerId id, const std::string& name, bool local) {
      PartyMember m;
      m.id = id;
      m.name = name;
      m.local = local;
      m.leader = (name == v.leader_name);
      v.members.push_back(std::move(m));
    };
    if (impl_->party_leader == me || impl_->party_leader.empty()) {
      // Local is in this party (either the leader or empty-leader safety).
      add(kInvalidPeer, me, true);
    } else {
      add(kInvalidPeer, me, true);
    }
    for (const auto& [pid, buf] : impl_->peers) {
      if (buf.party_leader == v.leader_name) {
        add(pid, buf.name.empty() ? ("Player " + std::to_string(pid)) : buf.name,
            false);
      }
    }
    // Leader first, then members alphabetical.
    std::sort(v.members.begin(), v.members.end(),
              [](const PartyMember& a, const PartyMember& b) {
                if (a.leader != b.leader) return a.leader;
                return a.name < b.name;
              });
  }

  for (const auto& p : impl_->party_invites) {
    PartyInviteEntry ie;
    ie.from = p.first;
    ie.received_ms = p.second;
    v.invites.push_back(std::move(ie));
  }
  return v;
}

SkateView Skate3NetSystem::GetSkateView() const {
  SkateView out;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->sk_active) return out;
  const SkateState& s = impl_->sk_view;
  out.active = true;
  out.phase = static_cast<SkatePhase>(s.phase);
  out.round = s.round;
  out.rounds = impl_->sk_rounds;
  out.setter = s.setter;
  out.current = s.current;
  out.set_trick.assign(s.set_trick, s.set_trick_length);
  out.message.assign(s.message, s.message_length);
  const uint32_t now = SteadyNowMs();
  out.remaining_ms =
      impl_->sk_phase_end_ms > now ? impl_->sk_phase_end_ms - now : 0;
  const PeerId lid = impl_->session.local_id();
  out.your_turn = (s.current == lid);
  // "you're next": local is the next matcher in the queue (or next setter).
  for (uint8_t i = 0; i + 1 < s.player_count; ++i) {
    if (s.players[i].id == s.current && s.players[i + 1].id == lid) {
      out.youre_next = true;
      break;
    }
  }
  for (uint8_t i = 0; i < s.player_count; ++i) {
    SkateLetterRow row;
    row.id = s.players[i].id;
    row.local = (s.players[i].id == lid);
    row.letters = s.players[i].letters;
    row.name = row.local ? std::string("You")
                         : impl_->DisplayName(s.players[i].id, std::string());
    out.players.push_back(std::move(row));
  }
  return out;
}

GameModeView Skate3NetSystem::GetGameModeView() const {
  GameModeView v;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  v.mode = impl_->gm_mode;
  v.phase = impl_->gm_phase;
  v.active = impl_->gm_phase != GamePhase::kIdle;
  if (!v.active) return v;
  const uint32_t now = SteadyNowMs();
  v.remaining_ms = impl_->gm_phase_end_ms > now ? impl_->gm_phase_end_ms - now : 0;
  v.winner = impl_->gm_winner;
  // Build rows for every player we know about (local + roster), pulling live
  // and best scores.
  auto best_of = [&](PeerId id) -> uint32_t {
    auto it = impl_->gm_best.find(id);
    return it == impl_->gm_best.end() ? 0u : it->second;
  };
  GameModeEntry me;
  me.id = kInvalidPeer;
  me.local = true;
  me.name = "You";
  me.current_trick = impl_->local_trick;
  me.current = impl_->local_score;
  me.best = best_of(kInvalidPeer);
  v.entries.push_back(std::move(me));
  for (const auto& [id, buf] : impl_->peers) {
    if (!impl_->PartyIncludes(id)) continue;  // private party -> HUD hides outsiders.
    GameModeEntry e;
    e.id = id;
    e.name = impl_->DisplayName(id, buf.name);
    e.current_trick = buf.current_trick;
    e.current = buf.samples.empty() ? 0u : buf.samples.back().state.score;
    e.best = best_of(id);
    v.entries.push_back(std::move(e));
  }
  std::sort(v.entries.begin(), v.entries.end(),
            [](const GameModeEntry& a, const GameModeEntry& b) {
              return a.best > b.best;
            });
  return v;
}

void Skate3NetSystem::CaptureLocalFromWorldRows(const float rows[12]) {
  if (!active_) return;
  SkaterState s;
  s.position = {rows[3], rows[7], rows[11]};
  s.rotation = MatrixRowsToQuat(rows);
  uint32_t now = SteadyNowMs();
  s.time_ms = now;
  s.tick = ++impl_->local_tick;
  if (impl_->have_prev) {
    uint32_t dt = now - impl_->prev_local_ms;
    if (dt > 0 && dt < 1000) {
      float inv = 1000.0f / static_cast<float>(dt);
      s.velocity = {(s.position.x - impl_->prev_local_pos.x) * inv,
                    (s.position.y - impl_->prev_local_pos.y) * inv,
                    (s.position.z - impl_->prev_local_pos.z) * inv};
    }
  }
  impl_->prev_local_pos = s.position;
  impl_->prev_local_ms = now;
  impl_->have_prev = true;
  // anim_id/trick_id/board_id/flags are left 0 in M0: position + orientation
  // replication first, animation/trick state layered on once it renders.
  SetLocalSkaterState(s);
}

uint32_t Skate3NetSystem::NowMs() const { return SteadyNowMs(); }

std::vector<RemoteSkaterView> Skate3NetSystem::SampleRemoteSkaters(uint32_t render_now_ms) {
  std::vector<RemoteSkaterView> out;
  if (!active_) return out;
  uint32_t render_ms = render_now_ms > kInterpDelayMs ? render_now_ms - kInterpDelayMs : 0;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  out.reserve(impl_->peers.size());
  for (auto& [id, buf] : impl_->peers) {
    RemoteSkaterView v;
    v.id = id;
    v.name = impl_->DisplayName(id, buf.name);
    v.current_trick = buf.current_trick;
    v.rtt_ms = buf.rtt_ms;
    v.valid = buf.Sample(render_ms, v.state);
    v.has_pose = buf.SamplePose(render_ms, v.pose);
    out.push_back(std::move(v));
  }
  return out;
}

void Skate3NetSystem::SendTrick(const TrickEvent& ev) {
  if (active_) impl_->session.SendTrickEvent(ev);
}

void Skate3NetSystem::SendBail(const BailEvent& ev) {
  if (active_) impl_->session.SendBailEvent(ev);
}

void Skate3NetSystem::SendChat(const std::string& text) {
  if (!active_) return;
  ChatMessage msg;
  size_t n = std::min(text.size(), static_cast<size_t>(kMaxChatLength));
  msg.length = static_cast<uint16_t>(n);
  std::memcpy(msg.text, text.data(), n);
  impl_->session.SendChat(msg);
}

NetStatus Skate3NetSystem::Status() {
  NetStatus s;
  s.active = active_;
  s.mode = impl_->session.mode();
  s.state = impl_->session.state();
  s.local_id = impl_->session.local_id();
  s.last_error = impl_->session.last_error();
  std::lock_guard<std::mutex> lock(impl_->mutex);
  s.peer_count = static_cast<uint32_t>(impl_->peers.size());
  return s;
}

// --- Process-wide instance + lifecycle hooks --------------------------------

Skate3NetSystem& Skate3Net() {
  static Skate3NetSystem instance;
  return instance;
}

void Skate3NetInitialize() { Skate3Net().Initialize(); }
void Skate3NetShutdown() { Skate3Net().Shutdown(); }
void Skate3NetConnect() { Skate3Net().Connect(); }
void Skate3NetDisconnect() { Skate3Net().Shutdown(); }

}  // namespace skate3::net
