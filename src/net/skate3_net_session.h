#pragma once

// Skate 3 Recomp — online play: session/transport layer over ENet.
//
// Wraps ENet (reliable + unreliable UDP) behind a small, engine-free interface.
// This is the ONLY header that the game integration needs; ENet itself is
// included only in the .cpp, so no game translation unit pulls in Winsock or
// ENet symbols.
//
// Model (host-authoritative relay, per the design doc): one peer hosts. Clients
// connect to the host, run a join handshake, and each streams its own skater
// state. The host re-stamps the sender and rebroadcasts gameplay traffic to the
// other clients. No EA/Xbox Live service is involved; addresses are plain
// IP:port supplied by the user (M0 is direct-IP LAN).

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "skate3_net_protocol.h"
#include "skate3_net_serialize.h"

namespace skate3::net {

enum class SessionMode : uint8_t { kOffline = 0, kHost, kClient };

enum class SessionState : uint8_t {
  kIdle = 0,       // not started.
  kConnecting,     // client: ENet connect in flight / awaiting JoinAccept.
  kConnected,      // in a session (host is always kConnected once started).
  kFailed,         // connect refused/timed out, or a fatal transport error.
  kDisconnected,   // cleanly left / was dropped.
};

struct SessionConfig {
  SessionMode mode = SessionMode::kOffline;
  std::string host_address;   // client only: the host's IP or hostname.
  uint16_t port = 34643;      // host: bind port; client: host's port.
  std::string display_name;   // local player's chosen name (<= kMaxNameLength).
  uint32_t max_players = 2;    // M0 target; capped at kMaxPlayers.
};

// A remote peer as tracked locally (roster entry + live link stats).
struct RemotePeer {
  PeerId id = kInvalidPeer;
  std::string name;
  uint32_t rtt_ms = 0;        // smoothed round-trip time from ENet.
  uint32_t packet_loss = 0;   // ENet's fixed-point loss (0..ENET_PEER_PACKET_LOSS_SCALE).
  bool connected = false;
};

// Delivered-message callback. `from` is the originating peer (host re-stamps it
// on relay). The DecodedMessage names which payload is populated by its header
// type. Invoked from Tick() on the calling (game) thread — never concurrently.
using MessageHandler = std::function<void(PeerId from, const DecodedMessage&)>;

// Optional connect/disconnect notifications for the roster/UI and the XNotify
// bridge. Invoked from Tick() on the calling thread.
using PeerEventHandler = std::function<void(const RemotePeer&, bool joined)>;

class Skate3NetSession {
 public:
  Skate3NetSession();
  ~Skate3NetSession();

  Skate3NetSession(const Skate3NetSession&) = delete;
  Skate3NetSession& operator=(const Skate3NetSession&) = delete;

  // Brings up ENet and starts hosting or connecting per `config`. Returns false
  // if the transport could not be created (bad bind, ENet init failure). Safe
  // to call after Stop(); calling while active Stops first.
  bool Start(const SessionConfig& config);

  // Tears down the session and releases the ENet host. Idempotent.
  void Stop();

  // Pump the transport: service ENet, dispatch received messages to the handler
  // and connect/disconnect events to the peer handler, run host relay, and
  // refresh per-peer RTT/loss. Call once per frame with a monotonic clock in
  // milliseconds. Cheap when offline.
  void Tick(uint32_t now_ms);

  // --- Sending -------------------------------------------------------------
  // Unreliable (channel 1): high-frequency, latest-wins. Dropped if not active.
  void SendSkaterState(const SkaterState& state);
  // Same, plus a packed root-relative skeleton pose (A1 bone replication).
  // rot/pos are bone_count entries (<= kMaxPoseBones); bone_count 0 == plain state.
  void SendSkaterStatePose(const SkaterState& state, const Quat* rot,
                           const Vec3* pos, uint8_t bone_count);
  // One skinned mesh's skeleton pose (A1 full-body). Sent unreliable, one call
  // per body-part mesh; the receiver reassembles a full set by tick.
  void SendMeshPose(const MeshPose& mesh, const Quat* rot, const Vec3* pos);
  // Online game-mode round state (Spot Battle etc.). Reliable.
  void SendGameModeState(const GameModeState& gm);
  void SendSkateState(const SkateState& s);
  void SendPing(const Ping& ping);
  void SendPong(const Pong& pong);
  // Reliable (channel 0): important events. Dropped if not active.
  void SendTrickEvent(const TrickEvent& ev);
  void SendBailEvent(const BailEvent& ev);
  void SendChat(const ChatMessage& msg);
  // Party (v4). Both reliable; invites are targeted-by-name (all peers
  // receive, only the matching one acts), states are per-peer broadcast.
  void SendPartyInvite(const PartyInvite& inv);
  void SendPartyState(const PartyState& ps);

  // --- Wiring --------------------------------------------------------------
  void SetMessageHandler(MessageHandler handler) { on_message_ = std::move(handler); }
  void SetPeerEventHandler(PeerEventHandler handler) { on_peer_event_ = std::move(handler); }

  // --- Introspection (for the diagnostics overlay) -------------------------
  SessionMode mode() const { return config_.mode; }
  SessionState state() const { return state_; }
  PeerId local_id() const { return local_id_; }
  const std::vector<RemotePeer>& peers() const { return peers_; }
  const std::string& last_error() const { return last_error_; }

 private:
  // Opaque ENet handles kept behind void* so this header stays ENet-free.
  void* enet_host_ = nullptr;   // ENetHost*
  void* enet_server_ = nullptr; // ENetPeer* to the host (client mode only).

  SessionConfig config_;
  SessionState state_ = SessionState::kIdle;
  PeerId local_id_ = kInvalidPeer;   // our session-scoped id (assigned by host).
  PeerId next_peer_id_ = 1;          // host: id allocator.
  std::vector<RemotePeer> peers_;
  MessageHandler on_message_;
  PeerEventHandler on_peer_event_;
  std::string last_error_;

  // Internal helpers (implemented in the .cpp).
  void SendRaw(const std::vector<uint8_t>& bytes, bool reliable, PeerId exclude);
  void SendRawTo(void* enet_peer, const std::vector<uint8_t>& bytes, bool reliable);
  void HandleConnect(void* enet_peer);
  void HandleDisconnect(void* enet_peer);
  void HandleReceive(void* enet_peer, const uint8_t* data, size_t size);
  void HostRelay(PeerId from, const DecodedMessage& msg, const uint8_t* data, size_t size);
  void RefreshLinkStats();
  RemotePeer* FindPeer(PeerId id);
};

}  // namespace skate3::net
