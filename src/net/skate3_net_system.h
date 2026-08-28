#pragma once

// Skate 3 Recomp — online play: game integration layer.
//
// Owns the Skate3NetSession, runs it on a dedicated service thread, and bridges
// it to the game thread with thread-safe hand-off:
//   * game thread  -> SetLocalSkaterState()  (captured from the local player)
//   * net thread   -> per-peer receive buffers (filled from incoming packets)
//   * game thread  -> SampleRemoteSkaters()   (time-interpolated remote states)
//
// Running the transport on its own thread means we don't need to find and hook
// the game's per-frame update: ENet is serviced at a fixed cadence here, and
// all cross-thread state is a couple of small mutex-guarded structures. The
// game thread only ever touches guest memory (reading the local skater, driving
// remote skaters); it never calls ENet directly.
//
// Config is via cvars (see the .cpp): skate3_net_enable / _mode / _host /
// _port / _name. Nothing runs unless skate3_net_enable is set, so single-player
// is completely unaffected.

#include <cstdint>
#include <string>
#include <vector>

#include "skate3_net_protocol.h"
#include "skate3_net_session.h"

namespace skate3::net {

// A skater's skeleton pose (A1 bone replication): one reference skinned item's
// palette expressed ROOT-RELATIVE (world root removed), per bone as rotation +
// position. The render side reconstructs each remote body part from this. Fixed
// arrays (bounded by kMaxPoseBones) so this stays trivially copyable.
struct SkeletonPose {
  uint8_t bone_count = 0;
  Quat rot[kMaxPoseBones] = {};
  Vec3 pos[kMaxPoseBones] = {};
};

// A skater's FULL body pose (A1 full-body replication): the root-relative palette
// of every skinned body-part mesh, ordered by `rank` (mesh index-count
// descending) so peers pair mesh r<->mesh r even at different LOD. mesh_count is
// how many of `meshes` are valid. Each skater is several kMeshPose datagrams;
// this is the reassembled set. Sizeable POD (~kMaxSkaterMeshes palettes) but only
// a handful of remotes exist, so value semantics stay simple.
struct MultiMeshPose {
  uint8_t mesh_count = 0;
  SkeletonPose meshes[kMaxSkaterMeshes] = {};
  uint32_t keys[kMaxSkaterMeshes] = {};  // per-mesh ib_count (diagnostic/matching).
};

// A remote player's current interpolated presentation state, as handed to the
// renderer each frame.
struct RemoteSkaterView {
  PeerId id = kInvalidPeer;
  std::string name;
  std::string current_trick;   // last trick this peer landed (game HUD text).
  SkaterState state;     // interpolated to the render clock.
  uint32_t rtt_ms = 0;
  bool valid = false;    // false if the peer has no usable samples yet.
  MultiMeshPose pose;    // interpolated full-body pose; pose.mesh_count 0 = none.
  bool has_pose = false;
};

// A single scoreboard row for the game-mode HUD.
struct GameModeEntry {
  PeerId id = kInvalidPeer;   // kInvalidPeer/0 = the local player.
  std::string name;
  std::string current_trick;  // last trick landed (game HUD text; S.K.A.T.E.).
  uint32_t current = 0;       // live line score this instant.
  uint32_t best = 0;          // best line during the round.
  bool local = false;
};

// Snapshot of the online game-mode round for the HUD.
struct GameModeView {
  bool active = false;               // a round is running or showing results.
  GameMode mode = GameMode::kNone;
  GamePhase phase = GamePhase::kIdle;
  uint32_t remaining_ms = 0;         // time left in the current phase.
  std::vector<GameModeEntry> entries;  // sorted best desc.
  PeerId winner = kInvalidPeer;      // valid in kResults (highest best).
};

// One S.K.A.T.E. scoreboard row for the HUD.
struct SkateLetterRow {
  PeerId id = kInvalidPeer;
  std::string name;
  int letters = 0;   // 0-5 = how much of S-K-A-T-E is spelled.
  bool local = false;
};

// A party invite this local player has RECEIVED, still waiting for accept/leave.
struct PartyInviteEntry {
  std::string from;         // inviter's display name.
  uint32_t received_ms = 0; // local clock at receive time (for expiry sort).
};

// One row in the party status view (member of your party, incl. yourself).
struct PartyMember {
  PeerId id = kInvalidPeer;  // kInvalidPeer = the local player.
  std::string name;
  bool leader = false;       // this member is the party leader.
  bool local = false;
};

// Snapshot of the local party state for the HUD / menu.
struct PartyView {
  bool in_party = false;      // false = solo.
  bool is_private = false;    // party is private (non-members filtered out).
  bool you_are_leader = false;
  std::string leader_name;    // party leader's name ("" if solo).
  std::vector<PartyMember> members;         // sorted: leader first, then by name.
  std::vector<PartyInviteEntry> invites;    // pending invites TO the local player.
};

// Snapshot of the S.K.A.T.E. round for the HUD.
struct SkateView {
  bool active = false;
  SkatePhase phase = SkatePhase::kIdle;
  uint8_t round = 0;
  uint8_t rounds = 1;
  PeerId setter = kInvalidPeer;   // whose turn to set.
  PeerId current = kInvalidPeer;  // whose attempt is live now.
  std::string set_trick;          // the trick to match (kMatch).
  std::string message;            // announce-beat text (kAnnounce).
  uint32_t remaining_ms = 0;
  std::vector<SkateLetterRow> players;
  bool your_turn = false;   // local player is `current`.
  bool youre_next = false;  // local player is next up.
};

// Lightweight status snapshot for the diagnostics overlay.
struct NetStatus {
  bool active = false;
  SessionMode mode = SessionMode::kOffline;
  SessionState state = SessionState::kIdle;
  PeerId local_id = kInvalidPeer;
  uint32_t peer_count = 0;
  std::string last_error;
};

// Process-wide accessor. The instance is created on first use and torn down by
// Skate3NetShutdown().
class Skate3NetSystem;
Skate3NetSystem& Skate3Net();

// Startup / shutdown hooks, called from the app lifecycle (mirrors the
// Skate3Initialize* pattern used by the FOV/draw-distance patches).
void Skate3NetInitialize();
void Skate3NetShutdown();

// On-demand connect / disconnect, for triggering online play from the in-game
// Online menu instead of (or in addition to) launch flags. Connect reads the
// current net cvars and starts the session; Disconnect tears it down. Both are
// idempotent and safe to call from the game thread. Requires skate3_net_enable.
void Skate3NetConnect();
void Skate3NetDisconnect();

class Skate3NetSystem {
 public:
  Skate3NetSystem();
  ~Skate3NetSystem();

  Skate3NetSystem(const Skate3NetSystem&) = delete;
  Skate3NetSystem& operator=(const Skate3NetSystem&) = delete;

  // Boot hook: if skate3_net_enable is true, arms online play. When
  // skate3_net_autostart is also true (default), immediately Connect()s so the
  // launch-flag workflow is unchanged; when autostart is false, stays armed and
  // waits for an explicit Connect() (e.g. from the in-game Online menu).
  void Initialize();

  // Reads the current net cvars (mode/host/port/name) and starts the session +
  // service thread. Idempotent (no-op if already active). No-op unless
  // skate3_net_enable is set. Safe to call from the game thread.
  void Connect();

  // Stops the service thread and the session. Idempotent.
  void Shutdown();

  bool active() const { return active_; }

  // --- Game thread API -----------------------------------------------------

  // Publish the local player's latest presentation state. Thread-safe; the
  // service thread transmits it at the configured tick rate. `time_ms` should
  // be a monotonic local clock (used only for the sender's own sequencing).
  void SetLocalSkaterState(const SkaterState& state);

  // Publish the local player's current line/combo score (read from the game each
  // frame); sent with the next state tick for game-mode scoring. Thread-safe.
  void SetLocalScore(uint32_t score);
  uint32_t LocalScore() const;  // last published local score (for the HUD).

  // The local player's current trick name (exact game HUD text, e.g. "Kickflip",
  // "FS 360 Pop Shuvit"). Broadcast to peers on each change; the authoritative
  // trick identity for S.K.A.T.E. matching + on-screen display.
  void SetLocalTrick(const std::string& trick);
  std::string LocalTrick() const;

  // Monotonic land-counter: increments on every buffer transition to a new
  // non-empty trick name (in scene.cpp's per-frame trick-buffer read). Rides
  // on SkaterState.trick_id so peers see it via the existing state stream;
  // S.K.A.T.E. adjudication uses "counter increased since attempt-start
  // baseline" as the LAND signal (robust to two identical tricks in a row
  // where the buffer name doesn't change). No wire-format change. Thread-safe.
  void SetLocalLandSeq(uint32_t seq);

  // Register that the local player LANDED a trick, with its FULL combined name
  // (base flip + rotation, reconstructed in scene.cpp). Atomically sets the
  // local trick name AND bumps the land counter, and queues a reliable
  // TrickEvent carrying BOTH -- so peers receive the name and the land count
  // together (no race between the two). This is the authoritative S.K.A.T.E.
  // land signal. Thread-safe.
  void RegisterLocalLand(const std::string& trick_name);

  // --- Online game modes (Milestone D) -------------------------------------
  // Start a Spot Battle (this instance becomes the authority: it runs the
  // countdown/timer and broadcasts phase changes). duration_s = the active
  // skate window per round; rounds = how many back-to-back rounds to run
  // (1-6). No-op if a round is already running. Thread-safe.
  void StartSpotBattle(uint32_t duration_s, uint32_t rounds = 1);
  // Current round snapshot for the HUD (empty/inactive when no round).
  GameModeView GetGameModeView() const;

  // Start a S.K.A.T.E. game (this instance becomes the authority: runs turn
  // order, set/match detection, letters, and broadcasts state). rounds = 1-3.
  // No-op if a S.K.A.T.E. game is already running. Thread-safe.
  void StartSkate(uint32_t rounds);
  // Current S.K.A.T.E. snapshot for the HUD (inactive when no game).
  SkateView GetSkateView() const;

  // --- Party (v4) ---------------------------------------------------------
  // Invite a peer by display name. The invite is broadcast reliably; only the
  // peer whose CURRENT name matches acts on it. No-op if the target name is
  // empty. Thread-safe.
  void InvitePlayer(const std::string& target_name);
  // Accept a pending invite from `from_name`. Sets the local player's party
  // leader to that name and broadcasts the change. Thread-safe.
  void AcceptInvite(const std::string& from_name);
  // Leave the current party (become solo). Thread-safe.
  void LeaveParty();
  // Toggle the party's PRIVATE flag. Only meaningful for the party leader;
  // members inherit the leader's value. When private, non-party peers are
  // filtered out (not rendered, not included in game-mode rounds). No-op if
  // the local player isn't the leader. Thread-safe.
  void SetPartyPrivate(bool is_private);
  // True when the local player is in a private party (regardless of role) --
  // the render bridge + game-mode filters read this to hide non-members.
  // Thread-safe.
  bool InPrivateParty() const;
  // Snapshot of the local party state for the menu + HUD. Thread-safe.
  PartyView GetPartyView() const;
  // Cheap same-party check for the HUDs (marks other members with a star).
  bool InSameParty(PeerId id) const;

  // True when NO player (local or remote) has moved beyond a small threshold for
  // at least `ms` milliseconds -- i.e. everyone has found a spot and settled.
  // Used to auto-freeze everyone (S.K.A.T.E. turn start). Uses the positions
  // already tracked by the service thread.
  bool AllPlayersSettled(uint32_t ms) const;

  // Publish the local player's full-body pose: every skinned mesh's root-relative
  // palette, ordered by rank (mesh index-count descending). Streamed one kMeshPose
  // datagram per mesh at the tick rate (A1 full-body replication). Thread-safe.
  void SetLocalSkaterMeshPoses(const MultiMeshPose& poses);

  // Convenience capture from the game's world matrix rows (m_MatLtoWTrans
  // layout: 3 affine rows, translation in components 3/7/11). Extracts position
  // and orientation, derives velocity from the previous capture, stamps a tick,
  // and publishes via SetLocalSkaterState. Call once per frame on the game
  // thread. No-op when inactive.
  void CaptureLocalFromWorldRows(const float rows[12]);

  // Monotonic millisecond clock in the SAME domain used to timestamp received
  // samples. Pass this to SampleRemoteSkaters so interpolation lines up.
  uint32_t NowMs() const;

  // Sample every known remote skater, interpolated to `render_now_ms` (a
  // monotonic local clock in the SAME domain the net thread stamps receipts
  // with — see the .cpp; callers pass a steady millisecond clock). Thread-safe.
  std::vector<RemoteSkaterView> SampleRemoteSkaters(uint32_t render_now_ms);

  // Fire-and-forget reliable events from the local player.
  void SendTrick(const TrickEvent& ev);
  void SendBail(const BailEvent& ev);
  void SendChat(const std::string& text);

  // --- Diagnostics ---------------------------------------------------------
  NetStatus Status();

 private:
  struct Impl;
  Impl* impl_ = nullptr;   // hides <thread>/<mutex>/session from this header.
  bool active_ = false;
};

}  // namespace skate3::net
