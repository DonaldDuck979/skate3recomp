// Loopback integration test for Skate3NetSession over real ENet sockets.
//
// Spins up a host and a client on 127.0.0.1, drives their Tick() pumps, and
// asserts the full path works: connect -> join handshake -> assigned id ->
// bidirectional SkaterState delivery with authoritative sender stamping. This
// is the closest thing to the two-machine LAN test that runs on one box with no
// game code.
//
// Build (from repo root):
//   clang++ -std=c++20 -I src/net -I third_party/enet/include \
//     src/net/skate3_net_serialize.cpp src/net/skate3_net_session.cpp \
//     third_party/enet/*.c src/net/tests/skate3_net_session_test.cpp \
//     -lws2_32 -lwinmm -o net_session_test && ./net_session_test

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

#include "skate3_net_session.h"

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

uint32_t NowMs() {
  using namespace std::chrono;
  return static_cast<uint32_t>(
      duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// Pump both sessions until `done` returns true or `budget_ms` elapses.
template <typename Pred>
bool PumpUntil(Skate3NetSession& a, Skate3NetSession& b, Pred done, uint32_t budget_ms) {
  uint32_t start = NowMs();
  while (NowMs() - start < budget_ms) {
    a.Tick(NowMs());
    b.Tick(NowMs());
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  // One last pump so anything delivered on the final iteration is processed.
  a.Tick(NowMs());
  b.Tick(NowMs());
  return done();
}

}  // namespace

int main() {
  std::printf("skate3 net session loopback test\n");

  constexpr uint16_t kPort = 34655;  // arbitrary high port for the test.

  Skate3NetSession host;
  Skate3NetSession client;

  // Capture what each side receives.
  bool host_got_state = false;
  PeerId host_state_from = kInvalidPeer;
  SkaterState host_state{};
  host.SetMessageHandler([&](PeerId from, const DecodedMessage& m) {
    if (m.header.type == MessageType::kSkaterState) {
      host_got_state = true;
      host_state_from = from;
      host_state = m.skater_state;
    }
  });

  bool client_got_state = false;
  PeerId client_state_from = kInvalidPeer;
  SkaterState client_state{};
  client.SetMessageHandler([&](PeerId from, const DecodedMessage& m) {
    if (m.header.type == MessageType::kSkaterState) {
      client_got_state = true;
      client_state_from = from;
      client_state = m.skater_state;
    }
  });

  bool host_saw_join = false;
  host.SetPeerEventHandler([&](const RemotePeer& rp, bool joined) {
    if (joined && rp.name == "Client") host_saw_join = true;
  });

  // Start host.
  SessionConfig hcfg;
  hcfg.mode = SessionMode::kHost;
  hcfg.port = kPort;
  hcfg.display_name = "Host";
  hcfg.max_players = 2;
  Check(host.Start(hcfg), "host starts");
  Check(host.state() == SessionState::kConnected, "host is connected immediately");
  Check(host.local_id() == 1, "host local id is 1");

  // Start client.
  SessionConfig ccfg;
  ccfg.mode = SessionMode::kClient;
  ccfg.host_address = "127.0.0.1";
  ccfg.port = kPort;
  ccfg.display_name = "Client";
  Check(client.Start(ccfg), "client starts");

  // Drive the handshake.
  bool connected = PumpUntil(host, client,
      [&] { return client.state() == SessionState::kConnected; }, 3000);
  Check(connected, "client reaches connected state");
  Check(client.local_id() == 2, "client assigned id 2 by host");
  Check(host_saw_join, "host observed client join with correct name");

  // Host -> client skater state.
  SkaterState hs;
  hs.tick = 7;
  hs.time_ms = 1234;
  hs.position = {10.0f, 20.0f, 30.0f};
  hs.trick_id = 55;
  hs.flags = 1;
  host.SendSkaterState(hs);
  bool got_on_client = PumpUntil(host, client, [&] { return client_got_state; }, 2000);
  Check(got_on_client, "client receives host skater state");
  Check(client_state_from == 1, "client sees host state stamped from id 1");
  Check(client_state.tick == 7 && client_state.trick_id == 55, "host state fields intact");
  Check(client_state.position.z == 30.0f, "host state position intact");

  // Client -> host skater state.
  SkaterState cs;
  cs.tick = 99;
  cs.time_ms = 5678;
  cs.position = {-1.0f, -2.0f, -3.0f};
  cs.board_id = 12;
  client.SendSkaterState(cs);
  bool got_on_host = PumpUntil(host, client, [&] { return host_got_state; }, 2000);
  Check(got_on_host, "host receives client skater state");
  Check(host_state_from == 2, "host sees client state stamped from id 2");
  Check(host_state.tick == 99 && host_state.board_id == 12, "client state fields intact");

  // Clean shutdown.
  client.Stop();
  PumpUntil(host, client, [&] { return host.peers().empty(); }, 2000);
  Check(host.peers().empty(), "host roster empties after client leaves");
  host.Stop();

  std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
