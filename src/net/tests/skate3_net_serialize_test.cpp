// Standalone round-trip and hostile-input tests for the wire protocol.
//
// No engine, game, or test-framework dependency: builds with just a C++
// compiler over src/net/skate3_net_serialize.cpp. Run manually during dev, and
// (later) wired into CI. The malformed-input cases matter as much as the happy
// path — DecodeMessage is what a public relay exposes to the internet.
//
// Build (from repo root), e.g.:
//   clang++ -std=c++20 -I src/net src/net/skate3_net_serialize.cpp \
//           src/net/tests/skate3_net_serialize_test.cpp -o net_test && ./net_test

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "skate3_net_serialize.h"

using namespace skate3::net;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* what) {
  ++g_checks;
  if (!cond) {
    ++g_failures;
    std::printf("  FAIL: %s\n", what);
  }
}

bool FloatEq(float a, float b) { return std::fabs(a - b) <= 1e-6f; }

// --- Round-trip cases -------------------------------------------------------

void TestSkaterStateRoundTrip() {
  SkaterState in;
  in.tick = 12345;
  in.time_ms = 987654;
  in.position = {1.5f, -2.25f, 100.0f};
  in.rotation = {0.0f, 0.70710678f, 0.0f, 0.70710678f};
  in.velocity = {-3.0f, 0.0f, 12.5f};
  in.anim_id = 0xABCDEF01;
  in.trick_id = 42;
  in.board_id = 7;
  in.flags = 0b101;
  in.score = 1234567;

  std::vector<uint8_t> buf;
  size_t n = EncodeSkaterState(in, /*sender=*/3, buf);
  Check(n == buf.size(), "skater: encode returns buffer size");

  DecodedMessage out;
  Check(DecodeMessage(buf.data(), buf.size(), out), "skater: decodes");
  Check(out.header.type == MessageType::kSkaterState, "skater: type tag");
  Check(out.header.sender == 3, "skater: sender");
  const SkaterState& s = out.skater_state;
  Check(s.tick == in.tick, "skater: tick");
  Check(s.time_ms == in.time_ms, "skater: time_ms");
  Check(FloatEq(s.position.x, in.position.x) && FloatEq(s.position.y, in.position.y) &&
            FloatEq(s.position.z, in.position.z), "skater: position");
  Check(FloatEq(s.rotation.y, in.rotation.y) && FloatEq(s.rotation.w, in.rotation.w),
        "skater: rotation");
  Check(FloatEq(s.velocity.z, in.velocity.z), "skater: velocity");
  Check(s.anim_id == in.anim_id, "skater: anim_id");
  Check(s.trick_id == in.trick_id, "skater: trick_id");
  Check(s.board_id == in.board_id, "skater: board_id");
  Check(s.flags == in.flags, "skater: flags");
  Check(s.score == in.score, "skater: score");
}

void TestJoinRequestRoundTrip() {
  JoinRequest in;
  const char* name = "TonyH";
  in.name_length = static_cast<uint8_t>(std::strlen(name));
  std::memcpy(in.name, name, in.name_length);

  std::vector<uint8_t> buf;
  EncodeJoinRequest(in, kInvalidPeer, buf);
  DecodedMessage out;
  Check(DecodeMessage(buf.data(), buf.size(), out), "join: decodes");
  Check(out.header.type == MessageType::kJoinRequest, "join: type tag");
  Check(out.join_request.name_length == in.name_length, "join: name_length");
  Check(std::memcmp(out.join_request.name, in.name, in.name_length) == 0, "join: name bytes");
  Check(out.join_request.version == kProtocolVersion, "join: version echoed");
}

void TestPeerListRoundTrip() {
  PeerListUpdate in;
  in.count = 3;
  for (uint8_t i = 0; i < in.count; ++i) {
    in.peers[i].id = static_cast<PeerId>(100 + i);
    char nm[8];
    std::snprintf(nm, sizeof(nm), "P%d", i);
    in.peers[i].name_length = static_cast<uint8_t>(std::strlen(nm));
    std::memcpy(in.peers[i].name, nm, in.peers[i].name_length);
  }

  std::vector<uint8_t> buf;
  EncodePeerListUpdate(in, 1, buf);
  DecodedMessage out;
  Check(DecodeMessage(buf.data(), buf.size(), out), "peerlist: decodes");
  Check(out.peer_list.count == 3, "peerlist: count");
  Check(out.peer_list.peers[2].id == 102, "peerlist: third id");
  Check(out.peer_list.peers[1].name[0] == 'P', "peerlist: second name");
}

void TestChatRoundTrip() {
  ChatMessage in;
  const char* text = "kickflip down the 7 set";
  in.length = static_cast<uint16_t>(std::strlen(text));
  std::memcpy(in.text, text, in.length);

  std::vector<uint8_t> buf;
  EncodeChatMessage(in, 5, buf);
  DecodedMessage out;
  Check(DecodeMessage(buf.data(), buf.size(), out), "chat: decodes");
  Check(out.chat.length == in.length, "chat: length");
  Check(std::memcmp(out.chat.text, in.text, in.length) == 0, "chat: text bytes");
}

void TestPingPongAndSmallMessages() {
  Ping p{0xDEADBEEF, 5000};
  std::vector<uint8_t> buf;
  EncodePing(p, 2, buf);
  DecodedMessage out;
  Check(DecodeMessage(buf.data(), buf.size(), out), "ping: decodes");
  Check(out.ping.token == 0xDEADBEEF, "ping: token");

  TrickEvent t{999, 250000, 61000};
  EncodeTrickEvent(t, 2, buf);
  Check(DecodeMessage(buf.data(), buf.size(), out), "trick: decodes");
  Check(out.trick_event.score == 250000, "trick: score");

  JoinReject jr{RejectReason::kSessionFull};
  EncodeJoinReject(jr, 1, buf);
  Check(DecodeMessage(buf.data(), buf.size(), out), "reject: decodes");
  Check(out.join_reject.reason == RejectReason::kSessionFull, "reject: reason");
}

// --- Deterministic little-endian layout -------------------------------------

void TestLittleEndianLayout() {
  Ping p{0x11223344, 0x55667788};
  std::vector<uint8_t> buf;
  EncodePing(p, 0x0102, buf);
  // header: magic(4 LE) version(2 LE) type(1) sender(2 LE), then token(4 LE)...
  Check(buf.size() >= 9 + 8, "layout: size");
  Check(buf[0] == 0x33 && buf[1] == 0x53 && buf[2] == 0x4B && buf[3] == 0x33,
        "layout: magic LE bytes ('S3K3')");
  Check(buf[4] == (kProtocolVersion & 0xFF), "layout: version low byte");
  Check(buf[6] == static_cast<uint8_t>(MessageType::kPing), "layout: type byte");
  Check(buf[7] == 0x02 && buf[8] == 0x01, "layout: sender LE");
  Check(buf[9] == 0x44 && buf[10] == 0x33 && buf[11] == 0x22 && buf[12] == 0x11,
        "layout: token LE");
}

// --- Hostile / malformed input ----------------------------------------------

void TestMalformedInput() {
  DecodedMessage out;

  Check(!DecodeMessage(nullptr, 0, out), "malformed: null/empty rejected");

  uint8_t one = 0x33;
  Check(!DecodeMessage(&one, 1, out), "malformed: too short for header");

  // Valid ping, then corrupt each guard in turn.
  std::vector<uint8_t> good;
  EncodePing(Ping{1, 2}, 1, good);

  std::vector<uint8_t> bad_magic = good;
  bad_magic[0] ^= 0xFF;
  Check(!DecodeMessage(bad_magic.data(), bad_magic.size(), out), "malformed: bad magic");

  std::vector<uint8_t> bad_version = good;
  bad_version[4] = 0xEE;  // version low byte
  Check(!DecodeMessage(bad_version.data(), bad_version.size(), out), "malformed: bad version");

  std::vector<uint8_t> bad_type = good;
  bad_type[6] = 0x7F;  // unknown type tag
  Check(!DecodeMessage(bad_type.data(), bad_type.size(), out), "malformed: unknown type");

  std::vector<uint8_t> truncated(good.begin(), good.begin() + good.size() - 2);
  Check(!DecodeMessage(truncated.data(), truncated.size(), out), "malformed: truncated payload");

  std::vector<uint8_t> trailing = good;
  trailing.push_back(0xAA);  // extra byte after a complete message
  Check(!DecodeMessage(trailing.data(), trailing.size(), out), "malformed: trailing garbage");

  // Oversized declared name_length in a JoinRequest must be rejected, not
  // trusted into an over-read.
  JoinRequest jr;
  jr.name_length = 5;
  std::vector<uint8_t> jbuf;
  EncodeJoinRequest(jr, 0, jbuf);
  jbuf[7] = 0xFF;  // name_length field position: header(9) - 2 (version u16)... set below
  // header is 9 bytes; then version(2) at [9..10], name_length at [11].
  // Recompute precisely instead of guessing:
  {
    std::vector<uint8_t> j2;
    EncodeJoinRequest(jr, 0, j2);
    j2[11] = static_cast<uint8_t>(kMaxNameLength + 1);
    Check(!DecodeMessage(j2.data(), j2.size(), out), "malformed: oversized name_length");
  }

  // Oversized peer count.
  PeerListUpdate pl;
  pl.count = 1;
  std::vector<uint8_t> pbuf;
  EncodePeerListUpdate(pl, 0, pbuf);
  pbuf[9] = static_cast<uint8_t>(kMaxPlayers + 1);  // count byte right after header
  Check(!DecodeMessage(pbuf.data(), pbuf.size(), out), "malformed: oversized peer count");

  // Oversized chat length claiming more bytes than present.
  ChatMessage cm;
  cm.length = 3;
  std::memcpy(cm.text, "abc", 3);
  std::vector<uint8_t> cbuf;
  EncodeChatMessage(cm, 0, cbuf);
  cbuf[9] = 0xFF;  // length low byte
  cbuf[10] = 0xFF; // length high byte -> huge, exceeds cap and buffer
  Check(!DecodeMessage(cbuf.data(), cbuf.size(), out), "malformed: oversized chat length");
}

// --- Bone-pose compression (milestone A1) -----------------------------------

void TestBoneCompression() {
  // Quaternion smallest-three: decoded rotation must match closely (compare via
  // |dot|, since q and -q are the same rotation) and stay ~unit length.
  Quat qs[] = {
      {0, 0, 0, 1},
      {0.5f, 0.5f, 0.5f, 0.5f},
      {0.70710678f, 0, 0, 0.70710678f},
      {-0.183f, 0.365f, -0.548f, 0.730f},
      {0, 0.70710678f, 0, -0.70710678f},
      {0.1f, -0.2f, 0.3f, -0.9f},
  };
  for (Quat q : qs) {
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    q.x /= n; q.y /= n; q.z /= n; q.w /= n;
    const Quat d = PackQuatSmallest3Unpack(PackQuatSmallest3(q));
    float dot = q.x * d.x + q.y * d.y + q.z * d.z + q.w * d.w;
    if (dot < 0) dot = -dot;
    Check(dot > 0.999f, "quat smallest-three round-trip within tolerance");
    const float dn = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z + d.w * d.w);
    Check(std::fabs(dn - 1.0f) < 0.01f, "decoded quat stays ~unit length");
  }
  // Half-float round-trip across typical root-relative bone offset magnitudes.
  float fs[] = {0.0f, 1.0f, -1.0f, 12.5f, -37.25f, 128.0f, -200.5f, 0.03125f};
  for (float f : fs) {
    const float r = UnpackHalf(PackHalf(f));
    const float tol = 0.02f + 0.001f * std::fabs(f);
    Check(std::fabs(r - f) <= tol, "half-float round-trip within tolerance");
  }
  // One bone is 4 (quat) + 6 (three halves) = 10 bytes on the wire; 84 bones =
  // 840, comfortably under kMaxDatagramSize (1200) with room for the header.
  Check(84u * 10u < kMaxDatagramSize, "84-bone pose fits one datagram");
}

void TestSkaterStatePoseRoundTrip() {
  SkaterState in;
  in.tick = 5;
  in.time_ms = 1234;
  in.position = {1, 2, 3};
  in.rotation = {0, 0, 0, 1};
  in.velocity = {0.5f, 0, -0.5f};
  in.anim_id = 7;
  in.board_id = 2;
  in.flags = 1;
  const uint8_t N = 84;
  Quat rot[84];
  Vec3 pos[84];
  for (int i = 0; i < N; ++i) {
    Quat q{static_cast<float>(i % 3) * 0.1f, 0.2f, -0.1f, 0.9f};
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    q.x /= n; q.y /= n; q.z /= n; q.w /= n;
    rot[i] = q;
    pos[i] = {static_cast<float>(i) * 0.5f, -static_cast<float>(i) * 0.25f,
              static_cast<float>(i % 7)};
  }
  std::vector<uint8_t> buf;
  const size_t sz = EncodeSkaterStatePose(in, 3, rot, pos, N, buf);
  Check(sz <= kMaxDatagramSize, "84-bone pose message fits datagram cap");
  DecodedMessage out;
  Check(DecodeMessage(buf.data(), buf.size(), out), "pose message decodes cleanly");
  Check(out.header.type == MessageType::kSkaterState, "pose msg type = skater state");
  Check(out.pose_bone_count == N, "pose bone count round-trips");
  Check(out.skater_state.tick == 5 && out.skater_state.board_id == 2,
        "state fields intact alongside pose");
  float dot = rot[10].x * out.pose_rot[10].x + rot[10].y * out.pose_rot[10].y +
              rot[10].z * out.pose_rot[10].z + rot[10].w * out.pose_rot[10].w;
  if (dot < 0) dot = -dot;
  Check(dot > 0.999f, "pose bone rotation round-trips");
  Check(std::fabs(pos[10].x - out.pose_pos[10].x) <= 0.05f + 0.001f * pos[10].x,
        "pose bone position round-trips");
  // A plain state (no pose) must still decode, with zero bones.
  std::vector<uint8_t> buf2;
  EncodeSkaterState(in, 3, buf2);
  DecodedMessage out2;
  Check(DecodeMessage(buf2.data(), buf2.size(), out2),
        "plain state (no pose) still decodes");
  Check(out2.pose_bone_count == 0, "no-pose message reports zero bones");
}

void TestMeshPoseRoundTrip() {
  MeshPose in;
  in.tick = 42;
  in.key = 9222;
  in.rank = 3;
  in.mesh_count = 11;
  in.bone_count = 84;
  Quat rot[84];
  Vec3 pos[84];
  for (int i = 0; i < 84; ++i) {
    Quat q{static_cast<float>(i % 5) * 0.1f, -0.2f, 0.15f, 0.85f};
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    q.x /= n; q.y /= n; q.z /= n; q.w /= n;
    rot[i] = q;
    pos[i] = {static_cast<float>(i) * 0.3f, static_cast<float>(i % 4),
              -static_cast<float>(i) * 0.2f};
  }
  std::vector<uint8_t> buf;
  const size_t sz = EncodeMeshPose(in, 7, rot, pos, buf);
  Check(sz <= kMaxDatagramSize, "one mesh pose fits datagram cap");
  DecodedMessage out;
  Check(DecodeMessage(buf.data(), buf.size(), out), "mesh pose decodes cleanly");
  Check(out.header.type == MessageType::kMeshPose, "mesh pose msg type");
  Check(out.mesh_pose.tick == 42 && out.mesh_pose.key == 9222 &&
        out.mesh_pose.rank == 3 && out.mesh_pose.mesh_count == 11 &&
        out.mesh_pose.bone_count == 84,
        "mesh pose header fields round-trip");
  float dot = rot[20].x * out.mesh_pose_rot[20].x +
              rot[20].y * out.mesh_pose_rot[20].y +
              rot[20].z * out.mesh_pose_rot[20].z +
              rot[20].w * out.mesh_pose_rot[20].w;
  if (dot < 0) dot = -dot;
  Check(dot > 0.999f, "mesh pose bone rotation round-trips");
  Check(std::fabs(pos[20].x - out.mesh_pose_pos[20].x) <= 0.05f + 0.001f * pos[20].x,
        "mesh pose bone position round-trips");
}

}  // namespace

int main() {
  std::printf("skate3 net protocol tests\n");
  TestSkaterStateRoundTrip();
  TestJoinRequestRoundTrip();
  TestPeerListRoundTrip();
  TestChatRoundTrip();
  TestPingPongAndSmallMessages();
  TestLittleEndianLayout();
  TestMalformedInput();
  TestBoneCompression();
  TestSkaterStatePoseRoundTrip();
  TestMeshPoseRoundTrip();

  std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
