#pragma once

// Skate 3 Recomp — online play: wire protocol definitions.
//
// SCOPE / LEGAL: this is an ORIGINAL fan-made network protocol written from
// first principles. It does not talk to, impersonate, or reimplement any EA or
// Xbox Live / PSN service, nor any original Skate 3 online wire format (those
// are defunct and are not ours to reproduce). The recomp already ships a host
// sockets shim; this layer sits above it and shares world state between peers
// that each own a legally-obtained copy of the game.
//
// DESIGN: host-authoritative relay. Each client is authoritative for its own
// skater and streams that state; the host rebroadcasts to the other peers.
// Skater physics/tricks are simulated locally per player (as the original
// free-roam effectively did), so no lockstep or rollback is needed — remote
// skaters are presentation-only, driven by interpolation of received state.
//
// WIRE FORMAT: hand-rolled, explicitly little-endian, length-prefixed by the
// transport (ENet delivers whole datagrams). Every message begins with a
// MessageHeader carrying a protocol version and a type tag. All integers are
// serialized LE regardless of host endianness; see skate3_net_serialize.*.
// Keep this file free of engine/game headers so it stays unit-testable in
// isolation and cheap to fuzz — the deserializer is attacker-facing once a
// public relay exists.

#include <cstdint>

namespace skate3::net {

// Bump on ANY incompatible change to a struct below or the type enum. Peers
// with mismatched versions are rejected at the join handshake.
inline constexpr uint16_t kProtocolVersion = 4;  // v4: PartyInvite + PartyState.

// A four-byte magic guarding the first byte of every datagram, so stray or
// hostile packets are cheaply rejected before any field is trusted.
inline constexpr uint32_t kProtocolMagic = 0x334B5333;  // 'S3K3' (LE on wire)

// Hard caps used for validation while decoding untrusted input.
inline constexpr uint32_t kMaxPlayers = 8;         // M0 target is 2; headroom.
inline constexpr uint32_t kMaxNameLength = 32;     // display name, UTF-8 bytes.
inline constexpr uint32_t kMaxTrickNameLength = 40;  // trick name text bytes.
inline constexpr uint32_t kMaxChatLength = 256;    // chat message, UTF-8 bytes.
inline constexpr uint32_t kMaxDatagramSize = 1200; // stay under common MTU.
inline constexpr uint32_t kMaxPoseBones = 84;      // skater skinning palette size
                                                   // (A1 skeleton replication).
inline constexpr uint32_t kMaxSkaterMeshes = 16;   // skinned body-part meshes per
                                                   // skater (probe: ~11); headroom.

// Locally-assigned, session-scoped peer handle (0 = invalid/unassigned). Not
// an XUID and not stable across sessions — see the design doc's note on
// synthesizing local stand-ins for Xbox Live identity.
using PeerId = uint16_t;
inline constexpr PeerId kInvalidPeer = 0;

enum class MessageType : uint8_t {
  kInvalid = 0,

  // --- Session lifecycle (reliable) ---
  kJoinRequest = 1,   // client -> host: request to enter the session.
  kJoinAccept = 2,    // host -> client: accepted; assigns PeerId + snapshot.
  kJoinReject = 3,    // host -> client: refused (version, full, banned).
  kPeerListUpdate = 4,  // host -> all: current roster after a join/leave.
  kDisconnect = 5,    // either way: graceful leave with a reason.

  // --- Gameplay state ---
  kSkaterState = 16,  // owner -> host -> peers: high-frequency, UNRELIABLE.
  kTrickEvent = 17,   // owner -> host -> peers: combo/score event, RELIABLE.
  kBailEvent = 18,    // owner -> host -> peers: bail/Hall-of-Meat, RELIABLE.
  kChatMessage = 19,  // owner -> host -> peers: RELIABLE.
  kMeshPose = 20,     // owner -> host -> peers: ONE skinned mesh's skeleton pose,
                      // UNRELIABLE. A full skater is several of these per tick
                      // (one per body-part mesh) -- too big for one datagram, so
                      // each mesh ships separately and is reassembled by tick.
  kGameModeState = 21,  // mode-starter -> peers: online game-mode round state
                        // (Spot Battle etc.), RELIABLE. Broadcast on each phase
                        // change; receivers run a local timer + track scores.
  kSkateState = 22,     // S.K.A.T.E. authority -> peers: full turn/letters state
                        // (setter, current attempter, set trick, per-player
                        // letters), RELIABLE. Broadcast on each state change.
  kPartyInvite = 23,    // inviter -> everyone (targeted by name): "join my
                        // party". RELIABLE. Only the peer whose name matches
                        // `target_name` acts on it; others ignore.
  kPartyState = 24,     // owner -> host -> peers: this peer's current party
                        // leader name ("" = solo). Broadcast on change +
                        // periodically. RELIABLE.

  // --- Keepalive / diagnostics (unreliable) ---
  kPing = 32,         // sender -> receiver: RTT probe, echoes a token.
  kPong = 33,         // reply to kPing with the same token + send time.
};

enum class RejectReason : uint8_t {
  kNone = 0,
  kVersionMismatch = 1,
  kSessionFull = 2,
  kBadName = 3,
  kInternalError = 4,
};

// Fixed-point-free: transforms travel as IEEE-754 floats (bit-cast to u32 for
// portable LE serialization). Skate 3's world is a single city instance, so a
// world/instance id is not carried in M0; add one if multiple instances land.

struct Vec3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

// Unit quaternion for orientation. Sent whole (16 bytes); smallest-three
// compression is a later bandwidth optimization, not an M0 concern.
struct Quat {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float w = 1.0f;
};

struct MessageHeader {
  uint32_t magic = kProtocolMagic;
  uint16_t version = kProtocolVersion;
  MessageType type = MessageType::kInvalid;
  PeerId sender = kInvalidPeer;  // filled by the sender; re-stamped by the host.
};

// --- Session lifecycle payloads ---

struct JoinRequest {
  uint16_t version = kProtocolVersion;  // duplicated for pre-accept checks.
  char name[kMaxNameLength] = {};       // NUL-padded UTF-8; not NUL-guaranteed.
  uint8_t name_length = 0;              // valid bytes in name[].
};

struct JoinAccept {
  PeerId assigned_id = kInvalidPeer;  // the client's session-scoped handle.
  uint8_t peer_count = 0;             // entries following in the roster.
  // The roster itself is delivered via a kPeerListUpdate immediately after.
};

struct JoinReject {
  RejectReason reason = RejectReason::kNone;
};

struct PeerInfo {
  PeerId id = kInvalidPeer;
  char name[kMaxNameLength] = {};
  uint8_t name_length = 0;
};

struct PeerListUpdate {
  uint8_t count = 0;                    // number of valid entries in peers[].
  PeerInfo peers[kMaxPlayers] = {};
};

struct Disconnect {
  uint8_t reason = 0;  // opaque application reason code; 0 = normal leave.
};

// --- Gameplay payloads ---

// The per-tick presentation state for one skater. Sent unreliable+sequenced at
// ~20-30 Hz; receivers interpolate between the last two by timestamp. Field
// meanings are transport-level only — the mapping to concrete game entity
// fields lives in the replication layer, not here.
struct SkaterState {
  uint32_t tick = 0;       // sender's monotonically increasing state counter.
  uint32_t time_ms = 0;    // sender clock at capture, for interpolation.
  Vec3 position;
  Quat rotation;
  Vec3 velocity;           // for dead-reckoning/extrapolation on loss.
  uint32_t anim_id = 0;    // opaque animation/pose selector (game-defined).
  uint32_t trick_id = 0;   // opaque current-trick selector (0 = none).
  uint16_t board_id = 0;   // equipped board/deck selector.
  uint8_t flags = 0;       // bit0: grounded, bit1: bailing, bit2: grinding.
  uint32_t score = 0;      // current line/combo score (game-mode scoring, v2).
};

// One skinned body-part mesh's skeleton pose for a given tick (A1 full-body
// replication). The 84-bone palette of all ~11 meshes doesn't fit one datagram,
// so each mesh is sent as its own kMeshPose and the receiver reassembles a full
// set by matching `tick`. `rank` orders meshes by index-count descending on both
// peers (LOD-robust pairing: the receiver applies rank r's palette to its own
// rank-r mesh). Bones are root-relative, compressed exactly like SkaterState's
// optional pose block (smallest-three quat + 3x float16 position).
struct MeshPose {
  uint32_t tick = 0;        // matches the SkaterState tick this pose belongs to.
  uint32_t key = 0;         // CONTENT id of this mesh (its index count) -- the
                            // receiver pairs its own mesh with the SAME key, an
                            // exact identity that can't be mismatched by draw
                            // order / mesh-set / LOD-rank drift.
  uint8_t rank = 0;         // ordering slot (ib_count desc) for reassembly arrays.
  uint8_t mesh_count = 0;   // total skinned meshes the sender is streaming.
  uint8_t bone_count = 0;   // bones in this mesh's palette (<= kMaxPoseBones).
  // Bones follow on the wire (rot smallest-three u32 + pos 3x float16).
};

// Online game-mode round state (Milestone D). The player who starts a mode is
// the round authority: it advances phases and broadcasts this on each change;
// receivers adopt the phase and run a local timer for `phase_ms`. Best-line
// scores are computed locally from the already-replicated per-player scores, so
// only the round shell needs syncing.
enum class GameMode : uint8_t { kNone = 0, kSpotBattle = 1, kSkate = 2 };

// S.K.A.T.E. phases (authority drives; followers display + run a local timer).
enum class SkatePhase : uint8_t {
  kIdle = 0,
  kCountdown = 1,   // 3s lead-in before round 1.
  kSet = 2,         // the setter has 25s to land a NEW trick (the one to match).
  kMatch = 3,       // each other player, in turn, must land the set trick.
  kResults = 4,     // round/game over.
  kAnnounce = 5,    // brief paced beat showing a result message (Set / Repeated
                    // Trick / matched / +letter) before the next turn.
};

// One player's S.K.A.T.E. standing (letters spelled so far, 0-5 = S-K-A-T-E).
struct SkatePlayer {
  uint32_t id = 0;
  uint8_t letters = 0;   // 5 = spelled SKATE = eliminated.
};

// Full S.K.A.T.E. round state, broadcast by the authority on each change.
struct SkateState {
  uint32_t round_id = 0;
  uint8_t phase = 0;                 // SkatePhase.
  uint8_t round = 0;                 // current round number (1..N).
  uint32_t setter = 0;               // whose turn to SET the trick.
  uint32_t current = 0;              // whose attempt is live right now.
  uint32_t phase_ms = 0;             // remaining time in this phase.
  char set_trick[kMaxTrickNameLength] = {};  // the trick to match (kMatch).
  uint8_t set_trick_length = 0;
  char message[48] = {};             // announce-beat text (kAnnounce).
  uint8_t message_length = 0;
  uint8_t player_count = 0;
  SkatePlayer players[kMaxPlayers] = {};
};
// Party (v4). Parties are identified by the LEADER's display name (a party of
// N members is "everyone whose party_leader field equals X"; the leader
// themself has party_leader == their own name). Solo = party_leader empty.
// - kPartyInvite is sent by the inviter naming a target by display name. Only
//   the peer whose current name matches acts on it; everyone else ignores.
// - kPartyState is a small per-peer heartbeat carrying that peer's current
//   party_leader, broadcast on change + periodically so late-joiners catch up.
struct PartyInvite {
  char inviter_name[kMaxNameLength] = {};   // sender's display name.
  uint8_t inviter_name_length = 0;
  char target_name[kMaxNameLength] = {};    // who this invite is for.
  uint8_t target_name_length = 0;
};

struct PartyState {
  char leader_name[kMaxNameLength] = {};    // "" = solo.
  uint8_t leader_name_length = 0;
  // Party is PRIVATE: non-members are filtered out of this peer's world (not
  // rendered, not included in game-mode rounds). Set by the leader; members
  // inherit the leader's value for filtering, so a leader flip is authoritative.
  uint8_t is_private = 0;
};

enum class GamePhase : uint8_t { kIdle = 0, kCountdown = 1, kActive = 2, kResults = 3 };
struct GameModeState {
  uint32_t round_id = 0;   // increments per round; dedupes/ignores stale rounds.
  uint8_t mode = 0;        // GameMode.
  uint8_t phase = 0;       // GamePhase.
  uint32_t phase_ms = 0;   // duration of this phase from the authority's start.
};

struct TrickEvent {
  uint32_t trick_id = 0;
  uint32_t score = 0;
  uint32_t time_ms = 0;
  // v3: the exact trick NAME as the game reports it ("Kickflip", "FS 360 Pop
  // Shuvit", ...), read from the game's HUD-trick memory. Authoritative trick
  // identity for S.K.A.T.E. matching + on-screen display; sent on each change.
  char name[kMaxTrickNameLength] = {};
  uint8_t name_length = 0;
};

struct BailEvent {
  uint32_t time_ms = 0;
  uint8_t severity = 0;  // 0-255 crash intensity; presentation hint only.
};

struct ChatMessage {
  uint16_t length = 0;                 // valid bytes in text[].
  char text[kMaxChatLength] = {};      // UTF-8; not NUL-guaranteed.
};

// --- Keepalive payloads ---

struct Ping {
  uint32_t token = 0;    // echoed back in Pong to match request/reply.
  uint32_t time_ms = 0;  // sender clock, for one-way delay estimation.
};

struct Pong {
  uint32_t token = 0;      // copied from the Ping.
  uint32_t ping_time_ms = 0;  // copied Ping.time_ms so the pinger computes RTT.
};

}  // namespace skate3::net
