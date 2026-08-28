#include "skate3_net_serialize.h"

#include <cmath>
#include <cstring>

namespace skate3::net {

// --- ByteWriter -------------------------------------------------------------

void ByteWriter::U8(uint8_t v) { out_.push_back(v); }

void ByteWriter::U16(uint16_t v) {
  out_.push_back(static_cast<uint8_t>(v & 0xFF));
  out_.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void ByteWriter::U32(uint32_t v) {
  out_.push_back(static_cast<uint8_t>(v & 0xFF));
  out_.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out_.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out_.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void ByteWriter::F32(float v) {
  uint32_t bits;
  std::memcpy(&bits, &v, sizeof(bits));
  U32(bits);
}

void ByteWriter::Bytes(const void* src, size_t count) {
  const auto* b = static_cast<const uint8_t*>(src);
  out_.insert(out_.end(), b, b + count);
}

// --- ByteReader -------------------------------------------------------------

uint8_t ByteReader::U8() {
  if (remaining_ < 1) {
    ok_ = false;
    return 0;
  }
  uint8_t v = *p_;
  ++p_;
  --remaining_;
  return v;
}

uint16_t ByteReader::U16() {
  if (remaining_ < 2) {
    ok_ = false;
    remaining_ = 0;
    return 0;
  }
  uint16_t v = static_cast<uint16_t>(p_[0]) |
               (static_cast<uint16_t>(p_[1]) << 8);
  p_ += 2;
  remaining_ -= 2;
  return v;
}

uint32_t ByteReader::U32() {
  if (remaining_ < 4) {
    ok_ = false;
    remaining_ = 0;
    return 0;
  }
  uint32_t v = static_cast<uint32_t>(p_[0]) |
               (static_cast<uint32_t>(p_[1]) << 8) |
               (static_cast<uint32_t>(p_[2]) << 16) |
               (static_cast<uint32_t>(p_[3]) << 24);
  p_ += 4;
  remaining_ -= 4;
  return v;
}

float ByteReader::F32() {
  uint32_t bits = U32();
  float v;
  std::memcpy(&v, &bits, sizeof(v));
  return v;
}

void ByteReader::Bytes(void* dst, size_t count) {
  if (remaining_ < count) {
    ok_ = false;
    remaining_ = 0;
    std::memset(dst, 0, count);
    return;
  }
  std::memcpy(dst, p_, count);
  p_ += count;
  remaining_ -= count;
}

// --- Header helpers ---------------------------------------------------------

namespace {

void WriteHeader(ByteWriter& w, MessageType type, PeerId sender) {
  w.U32(kProtocolMagic);
  w.U16(kProtocolVersion);
  w.U8(static_cast<uint8_t>(type));
  w.U16(sender);
}

void WriteVec3(ByteWriter& w, const Vec3& v) {
  w.F32(v.x);
  w.F32(v.y);
  w.F32(v.z);
}

void WriteQuat(ByteWriter& w, const Quat& q) {
  w.F32(q.x);
  w.F32(q.y);
  w.F32(q.z);
  w.F32(q.w);
}

Vec3 ReadVec3(ByteReader& r) {
  Vec3 v;
  v.x = r.F32();
  v.y = r.F32();
  v.z = r.F32();
  return v;
}

Quat ReadQuat(ByteReader& r) {
  Quat q;
  q.x = r.F32();
  q.y = r.F32();
  q.z = r.F32();
  q.w = r.F32();
  return q;
}

bool IsKnownType(uint8_t t) {
  switch (static_cast<MessageType>(t)) {
    case MessageType::kJoinRequest:
    case MessageType::kJoinAccept:
    case MessageType::kJoinReject:
    case MessageType::kPeerListUpdate:
    case MessageType::kDisconnect:
    case MessageType::kSkaterState:
    case MessageType::kTrickEvent:
    case MessageType::kBailEvent:
    case MessageType::kChatMessage:
    case MessageType::kMeshPose:
    case MessageType::kGameModeState:
    case MessageType::kSkateState:
    case MessageType::kPartyInvite:
    case MessageType::kPartyState:
    case MessageType::kPing:
    case MessageType::kPong:
      return true;
    default:
      return false;
  }
}

}  // namespace

// --- Encoders ---------------------------------------------------------------

size_t EncodeJoinRequest(const JoinRequest& m, PeerId sender, std::vector<uint8_t>& out) {
  out.clear();
  ByteWriter w(out);
  WriteHeader(w, MessageType::kJoinRequest, sender);
  w.U16(m.version);
  uint8_t n = m.name_length <= kMaxNameLength ? m.name_length
                                              : static_cast<uint8_t>(kMaxNameLength);
  w.U8(n);
  w.Bytes(m.name, kMaxNameLength);
  return out.size();
}

size_t EncodeJoinAccept(const JoinAccept& m, PeerId sender, std::vector<uint8_t>& out) {
  out.clear();
  ByteWriter w(out);
  WriteHeader(w, MessageType::kJoinAccept, sender);
  w.U16(m.assigned_id);
  w.U8(m.peer_count);
  return out.size();
}

size_t EncodeJoinReject(const JoinReject& m, PeerId sender, std::vector<uint8_t>& out) {
  out.clear();
  ByteWriter w(out);
  WriteHeader(w, MessageType::kJoinReject, sender);
  w.U8(static_cast<uint8_t>(m.reason));
  return out.size();
}

size_t EncodePeerListUpdate(const PeerListUpdate& m, PeerId sender, std::vector<uint8_t>& out) {
  out.clear();
  ByteWriter w(out);
  WriteHeader(w, MessageType::kPeerListUpdate, sender);
  uint8_t count = m.count <= kMaxPlayers ? m.count : static_cast<uint8_t>(kMaxPlayers);
  w.U8(count);
  for (uint8_t i = 0; i < count; ++i) {
    w.U16(m.peers[i].id);
    uint8_t n = m.peers[i].name_length <= kMaxNameLength
                    ? m.peers[i].name_length
                    : static_cast<uint8_t>(kMaxNameLength);
    w.U8(n);
    w.Bytes(m.peers[i].name, kMaxNameLength);
  }
  return out.size();
}

size_t EncodeDisconnect(const Disconnect& m, PeerId sender, std::vector<uint8_t>& out) {
  out.clear();
  ByteWriter w(out);
  WriteHeader(w, MessageType::kDisconnect, sender);
  w.U8(m.reason);
  return out.size();
}

size_t EncodeSkaterState(const SkaterState& m, PeerId sender, std::vector<uint8_t>& out) {
  out.clear();
  ByteWriter w(out);
  WriteHeader(w, MessageType::kSkaterState, sender);
  w.U32(m.tick);
  w.U32(m.time_ms);
  WriteVec3(w, m.position);
  WriteQuat(w, m.rotation);
  WriteVec3(w, m.velocity);
  w.U32(m.anim_id);
  w.U32(m.trick_id);
  w.U16(m.board_id);
  w.U8(m.flags);
  w.U32(m.score);
  return out.size();
}

size_t EncodeSkaterStatePose(const SkaterState& m, PeerId sender, const Quat* rot,
                             const Vec3* pos, uint8_t bone_count,
                             std::vector<uint8_t>& out) {
  EncodeSkaterState(m, sender, out);  // fixed fields first (also clears out).
  if (bone_count > kMaxPoseBones) bone_count = static_cast<uint8_t>(kMaxPoseBones);
  ByteWriter w(out);                  // append after the fixed state.
  w.U8(bone_count);
  for (uint8_t i = 0; i < bone_count; ++i) {
    w.U32(PackQuatSmallest3(rot[i]));
    w.U16(PackHalf(pos[i].x));
    w.U16(PackHalf(pos[i].y));
    w.U16(PackHalf(pos[i].z));
  }
  return out.size();
}

size_t EncodeMeshPose(const MeshPose& m, PeerId sender, const Quat* rot,
                      const Vec3* pos, std::vector<uint8_t>& out) {
  out.clear();
  ByteWriter w(out);
  WriteHeader(w, MessageType::kMeshPose, sender);
  uint8_t bone_count = m.bone_count;
  if (bone_count > kMaxPoseBones) bone_count = static_cast<uint8_t>(kMaxPoseBones);
  w.U32(m.tick);
  w.U32(m.key);
  w.U8(m.rank);
  w.U8(m.mesh_count);
  w.U8(bone_count);
  for (uint8_t i = 0; i < bone_count; ++i) {
    w.U32(PackQuatSmallest3(rot[i]));
    w.U16(PackHalf(pos[i].x));
    w.U16(PackHalf(pos[i].y));
    w.U16(PackHalf(pos[i].z));
  }
  return out.size();
}

size_t EncodeGameModeState(const GameModeState& m, PeerId sender,
                           std::vector<uint8_t>& out) {
  out.clear();
  ByteWriter w(out);
  WriteHeader(w, MessageType::kGameModeState, sender);
  w.U32(m.round_id);
  w.U8(m.mode);
  w.U8(m.phase);
  w.U32(m.phase_ms);
  return out.size();
}

size_t EncodeSkateState(const SkateState& m, PeerId sender,
                        std::vector<uint8_t>& out) {
  out.clear();
  ByteWriter w(out);
  WriteHeader(w, MessageType::kSkateState, sender);
  w.U32(m.round_id);
  w.U8(m.phase);
  w.U8(m.round);
  w.U32(m.setter);
  w.U32(m.current);
  w.U32(m.phase_ms);
  const uint8_t tl = m.set_trick_length <= kMaxTrickNameLength
                         ? m.set_trick_length
                         : static_cast<uint8_t>(kMaxTrickNameLength);
  w.U8(tl);
  w.Bytes(m.set_trick, kMaxTrickNameLength);
  const uint8_t ml = m.message_length <= 48 ? m.message_length : 48;
  w.U8(ml);
  w.Bytes(m.message, 48);
  const uint8_t pc =
      m.player_count <= kMaxPlayers ? m.player_count : static_cast<uint8_t>(kMaxPlayers);
  w.U8(pc);
  for (uint8_t i = 0; i < pc; ++i) {
    w.U32(m.players[i].id);
    w.U8(m.players[i].letters);
  }
  return out.size();
}

size_t EncodeTrickEvent(const TrickEvent& m, PeerId sender, std::vector<uint8_t>& out) {
  out.clear();
  ByteWriter w(out);
  WriteHeader(w, MessageType::kTrickEvent, sender);
  w.U32(m.trick_id);
  w.U32(m.score);
  w.U32(m.time_ms);
  const uint8_t n = m.name_length <= kMaxTrickNameLength
                        ? m.name_length
                        : static_cast<uint8_t>(kMaxTrickNameLength);
  w.U8(n);
  w.Bytes(m.name, kMaxTrickNameLength);
  return out.size();
}

size_t EncodeBailEvent(const BailEvent& m, PeerId sender, std::vector<uint8_t>& out) {
  out.clear();
  ByteWriter w(out);
  WriteHeader(w, MessageType::kBailEvent, sender);
  w.U32(m.time_ms);
  w.U8(m.severity);
  return out.size();
}

size_t EncodeChatMessage(const ChatMessage& m, PeerId sender, std::vector<uint8_t>& out) {
  out.clear();
  ByteWriter w(out);
  WriteHeader(w, MessageType::kChatMessage, sender);
  uint16_t len = m.length <= kMaxChatLength ? m.length
                                            : static_cast<uint16_t>(kMaxChatLength);
  w.U16(len);
  w.Bytes(m.text, len);  // only the valid bytes travel; length-prefixed.
  return out.size();
}

size_t EncodePartyInvite(const PartyInvite& m, PeerId sender,
                         std::vector<uint8_t>& out) {
  out.clear();
  ByteWriter w(out);
  WriteHeader(w, MessageType::kPartyInvite, sender);
  uint8_t in = m.inviter_name_length <= kMaxNameLength
                   ? m.inviter_name_length
                   : static_cast<uint8_t>(kMaxNameLength);
  uint8_t tn = m.target_name_length <= kMaxNameLength
                   ? m.target_name_length
                   : static_cast<uint8_t>(kMaxNameLength);
  w.U8(in);
  w.Bytes(m.inviter_name, kMaxNameLength);
  w.U8(tn);
  w.Bytes(m.target_name, kMaxNameLength);
  return out.size();
}

size_t EncodePartyState(const PartyState& m, PeerId sender,
                        std::vector<uint8_t>& out) {
  out.clear();
  ByteWriter w(out);
  WriteHeader(w, MessageType::kPartyState, sender);
  uint8_t n = m.leader_name_length <= kMaxNameLength
                  ? m.leader_name_length
                  : static_cast<uint8_t>(kMaxNameLength);
  w.U8(n);
  w.Bytes(m.leader_name, kMaxNameLength);
  w.U8(m.is_private ? 1 : 0);
  return out.size();
}

size_t EncodePing(const Ping& m, PeerId sender, std::vector<uint8_t>& out) {
  out.clear();
  ByteWriter w(out);
  WriteHeader(w, MessageType::kPing, sender);
  w.U32(m.token);
  w.U32(m.time_ms);
  return out.size();
}

size_t EncodePong(const Pong& m, PeerId sender, std::vector<uint8_t>& out) {
  out.clear();
  ByteWriter w(out);
  WriteHeader(w, MessageType::kPong, sender);
  w.U32(m.token);
  w.U32(m.ping_time_ms);
  return out.size();
}

// --- Decode -----------------------------------------------------------------

namespace {

bool DecodeJoinRequest(ByteReader& r, DecodedMessage& out) {
  out.join_request.version = r.U16();
  uint8_t n = r.U8();
  if (n > kMaxNameLength) return false;
  out.join_request.name_length = n;
  r.Bytes(out.join_request.name, kMaxNameLength);
  return r.ok();
}

bool DecodeJoinAccept(ByteReader& r, DecodedMessage& out) {
  out.join_accept.assigned_id = r.U16();
  out.join_accept.peer_count = r.U8();
  return r.ok();
}

bool DecodeJoinReject(ByteReader& r, DecodedMessage& out) {
  out.join_reject.reason = static_cast<RejectReason>(r.U8());
  return r.ok();
}

bool DecodePeerListUpdate(ByteReader& r, DecodedMessage& out) {
  uint8_t count = r.U8();
  if (count > kMaxPlayers) return false;
  out.peer_list.count = count;
  for (uint8_t i = 0; i < count; ++i) {
    out.peer_list.peers[i].id = r.U16();
    uint8_t n = r.U8();
    if (n > kMaxNameLength) return false;
    out.peer_list.peers[i].name_length = n;
    r.Bytes(out.peer_list.peers[i].name, kMaxNameLength);
  }
  return r.ok();
}

bool DecodeDisconnect(ByteReader& r, DecodedMessage& out) {
  out.disconnect.reason = r.U8();
  return r.ok();
}

bool DecodeSkaterState(ByteReader& r, DecodedMessage& out) {
  out.skater_state.tick = r.U32();
  out.skater_state.time_ms = r.U32();
  out.skater_state.position = ReadVec3(r);
  out.skater_state.rotation = ReadQuat(r);
  out.skater_state.velocity = ReadVec3(r);
  out.skater_state.anim_id = r.U32();
  out.skater_state.trick_id = r.U32();
  out.skater_state.board_id = r.U16();
  out.skater_state.flags = r.U8();
  out.skater_state.score = r.U32();
  if (!r.ok()) return false;
  // Optional trailing skeleton pose (A1): present iff bytes remain. Consuming it
  // here keeps DecodeMessage's "no trailing garbage" invariant intact.
  if (r.remaining() > 0) {
    const uint8_t n = r.U8();
    if (!r.ok() || n > kMaxPoseBones) return false;
    for (uint8_t i = 0; i < n; ++i) {
      out.pose_rot[i] = PackQuatSmallest3Unpack(r.U32());
      out.pose_pos[i].x = UnpackHalf(r.U16());
      out.pose_pos[i].y = UnpackHalf(r.U16());
      out.pose_pos[i].z = UnpackHalf(r.U16());
    }
    if (!r.ok()) return false;
    out.pose_bone_count = n;
  }
  return r.ok();
}

bool DecodeMeshPose(ByteReader& r, DecodedMessage& out) {
  out.mesh_pose.tick = r.U32();
  out.mesh_pose.key = r.U32();
  out.mesh_pose.rank = r.U8();
  out.mesh_pose.mesh_count = r.U8();
  const uint8_t n = r.U8();
  if (!r.ok() || n > kMaxPoseBones) return false;
  for (uint8_t i = 0; i < n; ++i) {
    out.mesh_pose_rot[i] = PackQuatSmallest3Unpack(r.U32());
    out.mesh_pose_pos[i].x = UnpackHalf(r.U16());
    out.mesh_pose_pos[i].y = UnpackHalf(r.U16());
    out.mesh_pose_pos[i].z = UnpackHalf(r.U16());
  }
  out.mesh_pose.bone_count = n;
  return r.ok();
}

bool DecodeGameModeState(ByteReader& r, DecodedMessage& out) {
  out.game_mode.round_id = r.U32();
  out.game_mode.mode = r.U8();
  out.game_mode.phase = r.U8();
  out.game_mode.phase_ms = r.U32();
  return r.ok();
}

bool DecodeSkateState(ByteReader& r, DecodedMessage& out) {
  SkateState& s = out.skate_state;
  s.round_id = r.U32();
  s.phase = r.U8();
  s.round = r.U8();
  s.setter = r.U32();
  s.current = r.U32();
  s.phase_ms = r.U32();
  uint8_t tl = r.U8();
  if (tl > kMaxTrickNameLength) tl = static_cast<uint8_t>(kMaxTrickNameLength);
  s.set_trick_length = tl;
  r.Bytes(s.set_trick, kMaxTrickNameLength);
  uint8_t ml = r.U8();
  if (ml > 48) ml = 48;
  s.message_length = ml;
  r.Bytes(s.message, 48);
  uint8_t pc = r.U8();
  if (pc > kMaxPlayers) pc = static_cast<uint8_t>(kMaxPlayers);
  s.player_count = pc;
  for (uint8_t i = 0; i < pc; ++i) {
    s.players[i].id = r.U32();
    s.players[i].letters = r.U8();
  }
  return r.ok();
}

bool DecodeTrickEvent(ByteReader& r, DecodedMessage& out) {
  out.trick_event.trick_id = r.U32();
  out.trick_event.score = r.U32();
  out.trick_event.time_ms = r.U32();
  uint8_t n = r.U8();
  if (n > kMaxTrickNameLength) n = static_cast<uint8_t>(kMaxTrickNameLength);
  out.trick_event.name_length = n;
  r.Bytes(out.trick_event.name, kMaxTrickNameLength);
  return r.ok();
}

bool DecodeBailEvent(ByteReader& r, DecodedMessage& out) {
  out.bail_event.time_ms = r.U32();
  out.bail_event.severity = r.U8();
  return r.ok();
}

bool DecodeChatMessage(ByteReader& r, DecodedMessage& out) {
  uint16_t len = r.U16();
  if (len > kMaxChatLength) return false;
  out.chat.length = len;
  r.Bytes(out.chat.text, len);
  return r.ok();
}

bool DecodePartyInvite(ByteReader& r, DecodedMessage& out) {
  uint8_t in = r.U8();
  if (in > kMaxNameLength) in = static_cast<uint8_t>(kMaxNameLength);
  out.party_invite.inviter_name_length = in;
  r.Bytes(out.party_invite.inviter_name, kMaxNameLength);
  uint8_t tn = r.U8();
  if (tn > kMaxNameLength) tn = static_cast<uint8_t>(kMaxNameLength);
  out.party_invite.target_name_length = tn;
  r.Bytes(out.party_invite.target_name, kMaxNameLength);
  return r.ok();
}

bool DecodePartyState(ByteReader& r, DecodedMessage& out) {
  uint8_t n = r.U8();
  if (n > kMaxNameLength) n = static_cast<uint8_t>(kMaxNameLength);
  out.party_state.leader_name_length = n;
  r.Bytes(out.party_state.leader_name, kMaxNameLength);
  out.party_state.is_private = r.U8();
  return r.ok();
}

bool DecodePing(ByteReader& r, DecodedMessage& out) {
  out.ping.token = r.U32();
  out.ping.time_ms = r.U32();
  return r.ok();
}

bool DecodePong(ByteReader& r, DecodedMessage& out) {
  out.pong.token = r.U32();
  out.pong.ping_time_ms = r.U32();
  return r.ok();
}

}  // namespace

bool DecodeMessage(const uint8_t* data, size_t size, DecodedMessage& out) {
  out = DecodedMessage{};
  if (data == nullptr || size == 0 || size > kMaxDatagramSize) return false;

  ByteReader r(data, size);
  uint32_t magic = r.U32();
  if (!r.ok() || magic != kProtocolMagic) return false;
  uint16_t version = r.U16();
  if (!r.ok() || version != kProtocolVersion) return false;
  uint8_t type = r.U8();
  if (!r.ok() || !IsKnownType(type)) return false;
  PeerId sender = r.U16();
  if (!r.ok()) return false;

  out.header.magic = magic;
  out.header.version = version;
  out.header.type = static_cast<MessageType>(type);
  out.header.sender = sender;

  bool decoded = false;
  switch (out.header.type) {
    case MessageType::kJoinRequest:    decoded = DecodeJoinRequest(r, out); break;
    case MessageType::kJoinAccept:     decoded = DecodeJoinAccept(r, out); break;
    case MessageType::kJoinReject:     decoded = DecodeJoinReject(r, out); break;
    case MessageType::kPeerListUpdate: decoded = DecodePeerListUpdate(r, out); break;
    case MessageType::kDisconnect:     decoded = DecodeDisconnect(r, out); break;
    case MessageType::kSkaterState:    decoded = DecodeSkaterState(r, out); break;
    case MessageType::kTrickEvent:     decoded = DecodeTrickEvent(r, out); break;
    case MessageType::kBailEvent:      decoded = DecodeBailEvent(r, out); break;
    case MessageType::kChatMessage:    decoded = DecodeChatMessage(r, out); break;
    case MessageType::kMeshPose:       decoded = DecodeMeshPose(r, out); break;
    case MessageType::kGameModeState:  decoded = DecodeGameModeState(r, out); break;
    case MessageType::kSkateState:     decoded = DecodeSkateState(r, out); break;
    case MessageType::kPartyInvite:    decoded = DecodePartyInvite(r, out); break;
    case MessageType::kPartyState:     decoded = DecodePartyState(r, out); break;
    case MessageType::kPing:           decoded = DecodePing(r, out); break;
    case MessageType::kPong:           decoded = DecodePong(r, out); break;
    default:                           return false;
  }
  // Reject trailing bytes: a valid datagram is consumed exactly.
  if (!decoded || !r.ok() || r.remaining() != 0) return false;
  return true;
}

// --- Bone-pose compression (milestone A1) -----------------------------------

namespace {

// IEEE-754 binary16 <-> float. Round-to-nearest on the way down. Handles
// zero/subnormal/inf; bone offsets are ordinary small magnitudes so the common
// path is the normal one.
uint16_t FloatToHalf(float f) {
  uint32_t x;
  std::memcpy(&x, &f, sizeof(x));
  const uint32_t sign = (x >> 16) & 0x8000u;
  const uint32_t bexp = (x >> 23) & 0xFFu;
  uint32_t mant = x & 0x7FFFFFu;
  if (bexp == 0xFFu) {  // inf / nan
    return static_cast<uint16_t>(sign | 0x7C00u | (mant ? 0x200u : 0u));
  }
  int32_t exp = static_cast<int32_t>(bexp) - 127 + 15;
  if (exp >= 0x1F) return static_cast<uint16_t>(sign | 0x7C00u);  // overflow->inf
  if (exp <= 0) {                                                 // subnormal
    if (exp < -10) return static_cast<uint16_t>(sign);
    mant |= 0x800000u;
    const uint32_t shift = static_cast<uint32_t>(14 - exp);
    uint16_t h = static_cast<uint16_t>(mant >> shift);
    if ((mant >> (shift - 1)) & 1u) h = static_cast<uint16_t>(h + 1);
    return static_cast<uint16_t>(sign | h);
  }
  uint16_t h = static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) |
                                     (mant >> 13));
  if (mant & 0x1000u) h = static_cast<uint16_t>(h + 1);  // round to nearest
  return h;
}

float HalfToFloat(uint16_t h) {
  const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
  uint32_t exp = (h >> 10) & 0x1Fu;
  uint32_t mant = h & 0x3FFu;
  uint32_t f;
  if (exp == 0) {
    if (mant == 0) {
      f = sign;
    } else {
      exp = 127 - 15 + 1;
      while (!(mant & 0x400u)) {
        mant <<= 1;
        --exp;
      }
      mant &= 0x3FFu;
      f = sign | (exp << 23) | (mant << 13);
    }
  } else if (exp == 0x1F) {
    f = sign | 0x7F800000u | (mant << 13);
  } else {
    f = sign | ((exp - 15 + 127) << 23) | (mant << 13);
  }
  float out;
  std::memcpy(&out, &f, sizeof(out));
  return out;
}

constexpr float kInvSqrt2 = 0.70710678118654752f;

}  // namespace

uint16_t PackHalf(float f) { return FloatToHalf(f); }
float UnpackHalf(uint16_t h) { return HalfToFloat(h); }

uint32_t PackQuatSmallest3(const Quat& q) {
  float c[4] = {q.x, q.y, q.z, q.w};
  // Index of the largest-magnitude component (the one we DON'T send).
  int max_i = 0;
  float max_a = -1.0f;
  for (int i = 0; i < 4; ++i) {
    const float a = c[i] < 0 ? -c[i] : c[i];
    if (a > max_a) {
      max_a = a;
      max_i = i;
    }
  }
  // q and -q are the same rotation; flip so the dropped component is >= 0, which
  // lets the decoder recover it as +sqrt(1 - sum of the other three squared).
  if (c[max_i] < 0.0f) {
    c[0] = -c[0];
    c[1] = -c[1];
    c[2] = -c[2];
    c[3] = -c[3];
  }
  uint32_t packed = static_cast<uint32_t>(max_i) << 30;
  int shift = 20;
  for (int i = 0; i < 4; ++i) {
    if (i == max_i) continue;
    float n = c[i] / kInvSqrt2;  // each dropped-set component is in [-1/√2, 1/√2]
    if (n < -1.0f) n = -1.0f;
    if (n > 1.0f) n = 1.0f;
    int q10 = static_cast<int>((n * 0.5f + 0.5f) * 1023.0f + 0.5f);
    if (q10 < 0) q10 = 0;
    if (q10 > 1023) q10 = 1023;
    packed |= static_cast<uint32_t>(q10) << shift;
    shift -= 10;
  }
  return packed;
}

Quat PackQuatSmallest3Unpack(uint32_t packed) {
  const int max_i = static_cast<int>((packed >> 30) & 0x3u);
  float c[4] = {0, 0, 0, 0};
  int shift = 20;
  float sumsq = 0.0f;
  for (int i = 0; i < 4; ++i) {
    if (i == max_i) continue;
    const int q10 = static_cast<int>((packed >> shift) & 0x3FFu);
    const float n = (static_cast<float>(q10) / 1023.0f) * 2.0f - 1.0f;  // [-1,1]
    c[i] = n * kInvSqrt2;
    sumsq += c[i] * c[i];
    shift -= 10;
  }
  const float rem = 1.0f - sumsq;
  c[max_i] = rem > 0.0f ? std::sqrt(rem) : 0.0f;
  Quat q;
  q.x = c[0];
  q.y = c[1];
  q.z = c[2];
  q.w = c[3];
  return q;
}

}  // namespace skate3::net
