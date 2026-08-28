#pragma once

// Skate 3 Recomp — online play: wire (de)serialization.
//
// Explicit little-endian encoding independent of host byte order, with a
// bounds-checked reader so decoding never reads past the input. Every decode
// path validates the protocol magic, version, and per-field length caps from
// skate3_net_protocol.h before trusting a byte: this is the surface a public
// relay exposes to the internet, so "return false" beats "trust and crash".
//
// Dependency-free on purpose (only the STL): the whole layer builds and runs
// as a standalone unit test with no engine or game headers.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "skate3_net_protocol.h"

namespace skate3::net {

// --- Low-level LE cursors ---------------------------------------------------

// Appends fields little-endian to a growable byte buffer. Never fails; the
// caller bounds total size against kMaxDatagramSize before sending.
class ByteWriter {
 public:
  explicit ByteWriter(std::vector<uint8_t>& out) : out_(out) {}

  void U8(uint8_t v);
  void U16(uint16_t v);
  void U32(uint32_t v);
  void F32(float v);
  // Writes exactly `count` raw bytes from src (used for fixed-width char[]).
  void Bytes(const void* src, size_t count);

 private:
  std::vector<uint8_t>& out_;
};

// Reads fields little-endian from a fixed input span. Any read that would
// exceed the span sets ok_ = false and yields zero; callers check ok() once at
// the end, so a truncated packet fails cleanly instead of overreading.
class ByteReader {
 public:
  ByteReader(const uint8_t* data, size_t size) : p_(data), remaining_(size) {}

  uint8_t U8();
  uint16_t U16();
  uint32_t U32();
  float F32();
  // Copies exactly `count` bytes into dst; on underflow zero-fills dst and
  // marks the reader failed.
  void Bytes(void* dst, size_t count);

  bool ok() const { return ok_; }
  size_t remaining() const { return remaining_; }

 private:
  const uint8_t* p_;
  size_t remaining_;
  bool ok_ = true;
};

// --- Message framing ---------------------------------------------------------

// A single decoded datagram: the header plus whichever payload its type names.
// All payloads are trivially copyable PODs, so a flat struct (rather than a
// union) keeps decoding branch-free of lifetime concerns.
struct DecodedMessage {
  MessageHeader header;
  JoinRequest join_request;
  JoinAccept join_accept;
  JoinReject join_reject;
  PeerListUpdate peer_list;
  Disconnect disconnect;
  SkaterState skater_state;
  TrickEvent trick_event;
  BailEvent bail_event;
  ChatMessage chat;
  Ping ping;
  Pong pong;

  // Optional root-relative skeleton pose riding on a kSkaterState message (A1).
  // pose_bone_count == 0 means the sender didn't include a pose. Decoded from
  // the packed 10-bytes/bone form; the render side turns each (rot,pos) back
  // into an affine and reconstructs the remote's skeleton (see the render
  // bridge). Bounded by kMaxPoseBones so it's a fixed POD, not a heap alloc.
  uint8_t pose_bone_count = 0;
  Quat pose_rot[kMaxPoseBones] = {};
  Vec3 pose_pos[kMaxPoseBones] = {};

  // Populated for a kMeshPose message (A1 full-body replication): one skinned
  // mesh's root-relative palette, tagged with its tick/rank so the receiver can
  // reassemble a full multi-mesh set. mesh_pose_rot/pos hold `mesh_pose.bone_count`
  // bones.
  MeshPose mesh_pose;
  Quat mesh_pose_rot[kMaxPoseBones] = {};
  Vec3 mesh_pose_pos[kMaxPoseBones] = {};

  GameModeState game_mode;  // populated for a kGameModeState message.
  SkateState skate_state;   // populated for a kSkateState message.
  PartyInvite party_invite; // populated for a kPartyInvite message.
  PartyState party_state;   // populated for a kPartyState message.
};

// Encoders: prepend a MessageHeader of the right type, then the payload. Each
// clears `out` and returns the encoded size (bytes). The header's magic and
// version come from the constants; `sender` is taken from the argument.
size_t EncodeJoinRequest(const JoinRequest& m, PeerId sender, std::vector<uint8_t>& out);
size_t EncodeJoinAccept(const JoinAccept& m, PeerId sender, std::vector<uint8_t>& out);
size_t EncodeJoinReject(const JoinReject& m, PeerId sender, std::vector<uint8_t>& out);
size_t EncodePeerListUpdate(const PeerListUpdate& m, PeerId sender, std::vector<uint8_t>& out);
size_t EncodeDisconnect(const Disconnect& m, PeerId sender, std::vector<uint8_t>& out);
size_t EncodeSkaterState(const SkaterState& m, PeerId sender, std::vector<uint8_t>& out);
// Same wire message as EncodeSkaterState, plus a trailing packed skeleton pose
// (root-relative). `rot`/`pos` are `bone_count` entries (<= kMaxPoseBones), each
// packed to 10 bytes. Decoders that predate this transparently... no: the count
// byte + bones are consumed by DecodeSkaterState, which reads them iff bytes
// remain after the fixed fields. bone_count 0 is identical to EncodeSkaterState.
size_t EncodeSkaterStatePose(const SkaterState& m, PeerId sender, const Quat* rot,
                             const Vec3* pos, uint8_t bone_count,
                             std::vector<uint8_t>& out);
// One skinned mesh's skeleton pose (A1 full-body). Header + MeshPose fields +
// `bone_count` packed bones (10 bytes each). Sent unreliable, one per mesh.
size_t EncodeMeshPose(const MeshPose& m, PeerId sender, const Quat* rot,
                      const Vec3* pos, std::vector<uint8_t>& out);
size_t EncodeSkateState(const SkateState& m, PeerId sender,
                        std::vector<uint8_t>& out);
size_t EncodeGameModeState(const GameModeState& m, PeerId sender,
                           std::vector<uint8_t>& out);
size_t EncodePartyInvite(const PartyInvite& m, PeerId sender,
                         std::vector<uint8_t>& out);
size_t EncodePartyState(const PartyState& m, PeerId sender,
                        std::vector<uint8_t>& out);
size_t EncodeTrickEvent(const TrickEvent& m, PeerId sender, std::vector<uint8_t>& out);
size_t EncodeBailEvent(const BailEvent& m, PeerId sender, std::vector<uint8_t>& out);
size_t EncodeChatMessage(const ChatMessage& m, PeerId sender, std::vector<uint8_t>& out);
size_t EncodePing(const Ping& m, PeerId sender, std::vector<uint8_t>& out);
size_t EncodePong(const Pong& m, PeerId sender, std::vector<uint8_t>& out);

// --- Bone-pose compression (for skeleton replication, milestone A1) ---------
//
// One bone travels as 10 bytes: rotation as a unit quaternion packed
// "smallest-three" (a 2-bit index of the largest component + three 10-bit signed
// components = 32 bits), and position as three IEEE-754 half-floats (16 bits
// each). Lossy but well within a few thousandths of a unit / degree, which is
// invisible on an interpolated remote skater. Pure functions, no engine deps, so
// the round-trip is unit-tested (see the net tests).
uint32_t PackQuatSmallest3(const Quat& q);   // q assumed ~unit length.
Quat PackQuatSmallest3Unpack(uint32_t packed);
uint16_t PackHalf(float f);
float UnpackHalf(uint16_t h);

// Top-level decode: validates magic/version, reads the header, dispatches to
// the matching payload decoder, and confirms no trailing garbage. Returns true
// only for a fully valid datagram; `out.header.type` names the populated
// payload. Safe to call on arbitrary/hostile bytes.
bool DecodeMessage(const uint8_t* data, size_t size, DecodedMessage& out);

}  // namespace skate3::net
