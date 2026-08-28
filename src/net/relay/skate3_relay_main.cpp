// Skate 3 Recomp — headless online relay.
//
// A dedicated, render-free hub for fan-made online play. It runs the SAME
// Skate3NetSession in host mode that an in-game host would, but it has no local
// skater and never sends state of its own — it purely accepts client
// connections and relays their skater/trick/chat traffic between each other
// (Skate3NetSession::HostRelay does this automatically inside Tick()).
//
// Why: it lets every player connect OUT to one public box (e.g. a cheap VPS)
// instead of one player having to port-forward their router. It renders nothing
// and reads no input, so it runs fine on a headless / GPU-less server — exactly
// the environments where the full game can't run.
//
// Players join it as normal clients: set skate3_net_mode='client' and
// skate3_net_host='<relay ip>'. The relay itself is invisible in-game (it never
// sends a SkaterState, so clients have no samples for it and skip drawing it).
//
// Usage: skate3_relay [port] [max_players]
//   port         UDP port to listen on           (default 34643)
//   max_players  max concurrent clients, 1..8     (default 8)

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "skate3_net_protocol.h"
#include "skate3_net_session.h"

namespace {

std::atomic<bool> g_running{true};

void OnSignal(int /*sig*/) { g_running.store(false, std::memory_order_relaxed); }

// Monotonic millisecond clock (same domain the session expects from Tick()).
uint32_t SteadyNowMs() {
  using namespace std::chrono;
  static const steady_clock::time_point base = steady_clock::now();
  return static_cast<uint32_t>(
      duration_cast<milliseconds>(steady_clock::now() - base).count());
}

}  // namespace

int main(int argc, char** argv) {
  uint16_t port = 34643;
  uint32_t max_players = skate3::net::kMaxPlayers;
  if (argc > 1) {
    int p = std::atoi(argv[1]);
    if (p > 0 && p <= 65535) port = static_cast<uint16_t>(p);
  }
  if (argc > 2) {
    int mp = std::atoi(argv[2]);
    if (mp >= 1 && mp <= static_cast<int>(skate3::net::kMaxPlayers)) {
      max_players = static_cast<uint32_t>(mp);
    }
  }

  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);

  skate3::net::Skate3NetSession session;
  session.SetPeerEventHandler(
      [](const skate3::net::RemotePeer& peer, bool joined) {
        std::printf("[relay] player %s: id=%u name=%s\n",
                    joined ? "JOINED" : "left",
                    static_cast<unsigned>(peer.id),
                    peer.name.empty() ? "?" : peer.name.c_str());
        std::fflush(stdout);
      });

  skate3::net::SessionConfig cfg;
  cfg.mode = skate3::net::SessionMode::kHost;
  cfg.port = port;
  cfg.display_name = "relay";
  cfg.max_players = max_players;

  if (!session.Start(cfg)) {
    std::printf("[relay] FAILED to start on UDP %u: %s\n", port,
                session.last_error().c_str());
    return 1;
  }

  std::printf("[relay] listening on UDP %u (up to %u players). Ctrl+C to stop.\n",
              static_cast<unsigned>(port), static_cast<unsigned>(max_players));
  std::fflush(stdout);

  uint32_t last_report = 0;
  size_t last_count = SIZE_MAX;
  while (g_running.load(std::memory_order_relaxed)) {
    const uint32_t now = SteadyNowMs();
    session.Tick(now);

    // Occasional heartbeat, and an immediate line whenever the count changes.
    const size_t count = session.peers().size();
    if (count != last_count || now - last_report >= 15000) {
      last_count = count;
      last_report = now;
      std::printf("[relay] %zu player(s) connected\n", count);
      std::fflush(stdout);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  std::printf("[relay] shutting down\n");
  session.Stop();
  return 0;
}
