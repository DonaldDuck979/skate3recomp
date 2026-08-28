#include "skate3_net_session.h"

#include <cstring>

#include <enet/enet.h>

namespace skate3::net {

namespace {

// Two ENet channels: 0 reliable-ordered (handshake/events/chat), 1 unreliable-
// sequenced (skater state, ping). Sequencing on channel 1 lets ENet drop stale
// state packets; the replication layer additionally orders by timestamp.
constexpr enet_uint8 kChannelReliable = 0;
constexpr enet_uint8 kChannelUnreliable = 1;
constexpr size_t kChannelCount = 2;

// Byte offset of the PeerId `sender` field within an encoded datagram:
// magic(4) + version(2) + type(1). The host re-stamps this on relay.
constexpr size_t kSenderOffset = 7;

// The host reserves id 1 for itself; clients get 2, 3, ... .
constexpr PeerId kHostLocalId = 1;

// ENet global init is process-wide; ref-count so multiple sessions are safe.
int g_enet_refcount = 0;

bool EnetGlobalInit() {
  if (g_enet_refcount == 0) {
    if (enet_initialize() != 0) return false;
  }
  ++g_enet_refcount;
  return true;
}

void EnetGlobalShutdown() {
  if (g_enet_refcount > 0 && --g_enet_refcount == 0) {
    enet_deinitialize();
  }
}

PeerId PeerIdFromData(void* data) {
  return static_cast<PeerId>(reinterpret_cast<uintptr_t>(data));
}

void SetPeerData(ENetPeer* peer, PeerId id) {
  peer->data = reinterpret_cast<void*>(static_cast<uintptr_t>(id));
}

}  // namespace

Skate3NetSession::Skate3NetSession() = default;

Skate3NetSession::~Skate3NetSession() { Stop(); }

bool Skate3NetSession::Start(const SessionConfig& config) {
  Stop();
  config_ = config;
  if (config_.max_players > kMaxPlayers) config_.max_players = kMaxPlayers;
  last_error_.clear();
  peers_.clear();
  local_id_ = kInvalidPeer;
  next_peer_id_ = 2;

  if (config_.mode == SessionMode::kOffline) {
    state_ = SessionState::kIdle;
    return true;
  }

  if (!EnetGlobalInit()) {
    last_error_ = "enet_initialize failed";
    state_ = SessionState::kFailed;
    return false;
  }

  if (config_.mode == SessionMode::kHost) {
    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = config_.port;
    ENetHost* host = enet_host_create(&address, config_.max_players,
                                      kChannelCount, 0, 0);
    if (host == nullptr) {
      last_error_ = "enet_host_create (host bind) failed";
      state_ = SessionState::kFailed;
      EnetGlobalShutdown();
      return false;
    }
    enet_host_ = host;
    local_id_ = kHostLocalId;
    state_ = SessionState::kConnected;  // host is immediately in-session.
    return true;
  }

  // Client.
  ENetHost* host = enet_host_create(nullptr, 1, kChannelCount, 0, 0);
  if (host == nullptr) {
    last_error_ = "enet_host_create (client) failed";
    state_ = SessionState::kFailed;
    EnetGlobalShutdown();
    return false;
  }
  enet_host_ = host;

  ENetAddress address;
  if (enet_address_set_host(&address, config_.host_address.c_str()) != 0) {
    last_error_ = "could not resolve host address: " + config_.host_address;
    state_ = SessionState::kFailed;
    enet_host_destroy(host);
    enet_host_ = nullptr;
    EnetGlobalShutdown();
    return false;
  }
  address.port = config_.port;

  ENetPeer* server = enet_host_connect(host, &address, kChannelCount, 0);
  if (server == nullptr) {
    last_error_ = "enet_host_connect failed (no peer slot)";
    state_ = SessionState::kFailed;
    enet_host_destroy(host);
    enet_host_ = nullptr;
    EnetGlobalShutdown();
    return false;
  }
  enet_server_ = server;
  state_ = SessionState::kConnecting;
  return true;
}

void Skate3NetSession::Stop() {
  if (enet_host_ != nullptr) {
    auto* host = static_cast<ENetHost*>(enet_host_);
    // Best-effort graceful disconnect of every live peer.
    for (size_t i = 0; i < host->peerCount; ++i) {
      ENetPeer* peer = &host->peers[i];
      if (peer->state == ENET_PEER_STATE_CONNECTED) {
        enet_peer_disconnect_now(peer, 0);
      }
    }
    enet_host_destroy(host);
    enet_host_ = nullptr;
    enet_server_ = nullptr;
    EnetGlobalShutdown();
  }
  if (state_ != SessionState::kFailed) state_ = SessionState::kIdle;
  peers_.clear();
  local_id_ = kInvalidPeer;
}

void Skate3NetSession::Tick(uint32_t /*now_ms*/) {
  if (enet_host_ == nullptr) return;
  auto* host = static_cast<ENetHost*>(enet_host_);

  ENetEvent event;
  while (enet_host_service(host, &event, 0) > 0) {
    switch (event.type) {
      case ENET_EVENT_TYPE_CONNECT:
        HandleConnect(event.peer);
        break;
      case ENET_EVENT_TYPE_RECEIVE:
        HandleReceive(event.peer, event.packet->data, event.packet->dataLength);
        enet_packet_destroy(event.packet);
        break;
      case ENET_EVENT_TYPE_DISCONNECT:
        HandleDisconnect(event.peer);
        break;
      default:
        break;
    }
  }
  RefreshLinkStats();
}

// --- Connection lifecycle ---------------------------------------------------

void Skate3NetSession::HandleConnect(void* enet_peer_v) {
  auto* peer = static_cast<ENetPeer*>(enet_peer_v);

  if (config_.mode == SessionMode::kHost) {
    // Assign this client a session id now; its name arrives via JoinRequest.
    PeerId id = next_peer_id_++;
    SetPeerData(peer, id);
    RemotePeer rp;
    rp.id = id;
    rp.connected = true;
    peers_.push_back(rp);
    // Roster/name broadcast happens once the JoinRequest is validated.
    return;
  }

  // Client: connected to the host. Send our JoinRequest.
  SetPeerData(peer, kHostLocalId);
  JoinRequest jr;
  jr.version = kProtocolVersion;
  size_t n = config_.display_name.size();
  if (n > kMaxNameLength) n = kMaxNameLength;
  jr.name_length = static_cast<uint8_t>(n);
  std::memcpy(jr.name, config_.display_name.data(), n);
  std::vector<uint8_t> bytes;
  EncodeJoinRequest(jr, kInvalidPeer, bytes);
  SendRawTo(peer, bytes, /*reliable=*/true);
  // Stay kConnecting until JoinAccept arrives.
}

void Skate3NetSession::HandleDisconnect(void* enet_peer_v) {
  auto* peer = static_cast<ENetPeer*>(enet_peer_v);
  PeerId id = PeerIdFromData(peer->data);
  peer->data = nullptr;

  if (config_.mode == SessionMode::kClient) {
    state_ = SessionState::kDisconnected;
    if (on_peer_event_) {
      for (const RemotePeer& rp : peers_) on_peer_event_(rp, /*joined=*/false);
    }
    peers_.clear();
    return;
  }

  // Host: drop the peer from the roster and tell everyone else.
  RemotePeer dropped;
  bool found = false;
  for (size_t i = 0; i < peers_.size(); ++i) {
    if (peers_[i].id == id) {
      dropped = peers_[i];
      found = true;
      peers_.erase(peers_.begin() + i);
      break;
    }
  }
  if (found && on_peer_event_) on_peer_event_(dropped, /*joined=*/false);

  // Rebroadcast the updated roster.
  PeerListUpdate pl;
  pl.count = 0;
  for (const RemotePeer& rp : peers_) {
    if (pl.count >= kMaxPlayers) break;
    PeerInfo& pi = pl.peers[pl.count];
    pi.id = rp.id;
    size_t nn = rp.name.size() > kMaxNameLength ? kMaxNameLength : rp.name.size();
    pi.name_length = static_cast<uint8_t>(nn);
    std::memcpy(pi.name, rp.name.data(), nn);
    ++pl.count;
  }
  std::vector<uint8_t> bytes;
  EncodePeerListUpdate(pl, kHostLocalId, bytes);
  SendRaw(bytes, /*reliable=*/true, /*exclude=*/kInvalidPeer);
}

// --- Receive path -----------------------------------------------------------

void Skate3NetSession::HandleReceive(void* enet_peer_v, const uint8_t* data, size_t size) {
  auto* peer = static_cast<ENetPeer*>(enet_peer_v);
  PeerId link_id = PeerIdFromData(peer->data);

  DecodedMessage msg;
  if (!DecodeMessage(data, size, msg)) {
    // Malformed/hostile datagram: ignore. (A public relay would also rate-
    // limit the source here.)
    return;
  }

  if (config_.mode == SessionMode::kHost) {
    switch (msg.header.type) {
      case MessageType::kJoinRequest: {
        RemotePeer* rp = FindPeer(link_id);
        if (rp == nullptr) return;
        if (msg.join_request.version != kProtocolVersion) {
          JoinReject jr{RejectReason::kVersionMismatch};
          std::vector<uint8_t> b;
          EncodeJoinReject(jr, kHostLocalId, b);
          SendRawTo(peer, b, /*reliable=*/true);
          enet_peer_disconnect_later(peer, 0);
          return;
        }
        size_t nn = msg.join_request.name_length;
        if (nn > kMaxNameLength) nn = kMaxNameLength;
        rp->name.assign(msg.join_request.name, msg.join_request.name + nn);

        JoinAccept acc;
        acc.assigned_id = link_id;
        acc.peer_count = static_cast<uint8_t>(peers_.size());
        std::vector<uint8_t> b;
        EncodeJoinAccept(acc, kHostLocalId, b);
        SendRawTo(peer, b, /*reliable=*/true);

        // Broadcast the new roster to everyone.
        PeerListUpdate pl;
        pl.count = 0;
        for (const RemotePeer& r : peers_) {
          if (pl.count >= kMaxPlayers) break;
          PeerInfo& pi = pl.peers[pl.count];
          pi.id = r.id;
          size_t ln = r.name.size() > kMaxNameLength ? kMaxNameLength : r.name.size();
          pi.name_length = static_cast<uint8_t>(ln);
          std::memcpy(pi.name, r.name.data(), ln);
          ++pl.count;
        }
        std::vector<uint8_t> pb;
        EncodePeerListUpdate(pl, kHostLocalId, pb);
        SendRaw(pb, /*reliable=*/true, /*exclude=*/kInvalidPeer);

        if (on_peer_event_) on_peer_event_(*rp, /*joined=*/true);
        return;
      }
      case MessageType::kSkaterState:
      case MessageType::kMeshPose:
      case MessageType::kGameModeState:
      case MessageType::kSkateState:
      case MessageType::kPartyInvite:
      case MessageType::kPartyState:
      case MessageType::kTrickEvent:
      case MessageType::kBailEvent:
      case MessageType::kChatMessage:
      case MessageType::kPing:
      case MessageType::kPong:
        // Deliver locally (host renders/handles it) AND relay to other clients.
        if (on_message_) on_message_(link_id, msg);
        HostRelay(link_id, msg, data, size);
        return;
      default:
        return;  // clients don't send accept/reject/peerlist to the host.
    }
  }

  // Client.
  switch (msg.header.type) {
    case MessageType::kJoinAccept:
      local_id_ = msg.join_accept.assigned_id;
      state_ = SessionState::kConnected;
      return;
    case MessageType::kJoinReject:
      last_error_ = "join rejected by host";
      state_ = SessionState::kFailed;
      enet_peer_disconnect_later(static_cast<ENetPeer*>(enet_server_), 0);
      return;
    case MessageType::kPeerListUpdate: {
      // Rebuild the roster from the authoritative list (excluding ourselves),
      // firing join events for entries we hadn't seen.
      std::vector<RemotePeer> updated;
      for (uint8_t i = 0; i < msg.peer_list.count; ++i) {
        const PeerInfo& pi = msg.peer_list.peers[i];
        if (pi.id == local_id_) continue;
        RemotePeer rp;
        rp.id = pi.id;
        size_t ln = pi.name_length > kMaxNameLength ? kMaxNameLength : pi.name_length;
        rp.name.assign(pi.name, pi.name + ln);
        rp.connected = true;
        updated.push_back(rp);
      }
      if (on_peer_event_) {
        for (const RemotePeer& nu : updated) {
          if (FindPeer(nu.id) == nullptr) on_peer_event_(nu, /*joined=*/true);
        }
      }
      peers_.swap(updated);
      return;
    }
    case MessageType::kSkaterState:
    case MessageType::kMeshPose:
    case MessageType::kGameModeState:
    case MessageType::kSkateState:
    case MessageType::kPartyInvite:
    case MessageType::kPartyState:
    case MessageType::kTrickEvent:
    case MessageType::kBailEvent:
    case MessageType::kChatMessage:
    case MessageType::kPing:
    case MessageType::kPong:
      if (on_message_) on_message_(msg.header.sender, msg);
      return;
    default:
      return;
  }
}

void Skate3NetSession::HostRelay(PeerId from, const DecodedMessage& /*msg*/,
                                 const uint8_t* data, size_t size) {
  // Re-stamp the sender to the authoritative id and forward the exact payload
  // to every client except the originator. Reliable for events/chat, unreliable
  // for state/ping (matching the channel the game chose to send on).
  std::vector<uint8_t> relay(data, data + size);
  if (relay.size() > kSenderOffset + 1) {
    relay[kSenderOffset] = static_cast<uint8_t>(from & 0xFF);
    relay[kSenderOffset + 1] = static_cast<uint8_t>((from >> 8) & 0xFF);
  }
  MessageType type = static_cast<MessageType>(relay.size() > 6 ? relay[6] : 0);
  bool reliable = (type == MessageType::kTrickEvent ||
                   type == MessageType::kBailEvent ||
                   type == MessageType::kChatMessage ||
                   type == MessageType::kGameModeState ||
                   type == MessageType::kSkateState ||
                   type == MessageType::kPartyInvite ||
                   type == MessageType::kPartyState);
  SendRaw(relay, reliable, /*exclude=*/from);
}

// --- Sending ----------------------------------------------------------------

void Skate3NetSession::SendRawTo(void* enet_peer_v, const std::vector<uint8_t>& bytes,
                                 bool reliable) {
  if (enet_peer_v == nullptr || bytes.empty() || bytes.size() > kMaxDatagramSize) return;
  auto* peer = static_cast<ENetPeer*>(enet_peer_v);
  enet_uint32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : 0;
  ENetPacket* packet = enet_packet_create(bytes.data(), bytes.size(), flags);
  if (packet == nullptr) return;
  enet_uint8 channel = reliable ? kChannelReliable : kChannelUnreliable;
  if (enet_peer_send(peer, channel, packet) != 0) {
    enet_packet_destroy(packet);  // send failed: we still own the packet.
  }
}

void Skate3NetSession::SendRaw(const std::vector<uint8_t>& bytes, bool reliable,
                               PeerId exclude) {
  if (enet_host_ == nullptr || bytes.empty() || bytes.size() > kMaxDatagramSize) return;

  if (config_.mode == SessionMode::kClient) {
    SendRawTo(enet_server_, bytes, reliable);
    return;
  }

  // Host: send to each connected client except `exclude`.
  auto* host = static_cast<ENetHost*>(enet_host_);
  for (size_t i = 0; i < host->peerCount; ++i) {
    ENetPeer* peer = &host->peers[i];
    if (peer->state != ENET_PEER_STATE_CONNECTED) continue;
    if (exclude != kInvalidPeer && PeerIdFromData(peer->data) == exclude) continue;
    SendRawTo(peer, bytes, reliable);
  }
}

void Skate3NetSession::SendSkaterState(const SkaterState& state) {
  if (state_ != SessionState::kConnected) return;
  std::vector<uint8_t> b;
  EncodeSkaterState(state, local_id_, b);
  SendRaw(b, /*reliable=*/false, /*exclude=*/kInvalidPeer);
}

void Skate3NetSession::SendSkaterStatePose(const SkaterState& state, const Quat* rot,
                                           const Vec3* pos, uint8_t bone_count) {
  if (state_ != SessionState::kConnected) return;
  std::vector<uint8_t> b;
  EncodeSkaterStatePose(state, local_id_, rot, pos, bone_count, b);
  SendRaw(b, /*reliable=*/false, /*exclude=*/kInvalidPeer);
}

void Skate3NetSession::SendMeshPose(const MeshPose& mesh, const Quat* rot,
                                    const Vec3* pos) {
  if (state_ != SessionState::kConnected) return;
  std::vector<uint8_t> b;
  EncodeMeshPose(mesh, local_id_, rot, pos, b);
  SendRaw(b, /*reliable=*/false, /*exclude=*/kInvalidPeer);
}

void Skate3NetSession::SendGameModeState(const GameModeState& gm) {
  if (state_ != SessionState::kConnected) return;
  std::vector<uint8_t> b;
  EncodeGameModeState(gm, local_id_, b);
  SendRaw(b, /*reliable=*/true, /*exclude=*/kInvalidPeer);
}

void Skate3NetSession::SendSkateState(const SkateState& s) {
  if (state_ != SessionState::kConnected) return;
  std::vector<uint8_t> b;
  EncodeSkateState(s, local_id_, b);
  SendRaw(b, /*reliable=*/true, /*exclude=*/kInvalidPeer);
}

void Skate3NetSession::SendPing(const Ping& ping) {
  if (state_ != SessionState::kConnected) return;
  std::vector<uint8_t> b;
  EncodePing(ping, local_id_, b);
  SendRaw(b, /*reliable=*/false, /*exclude=*/kInvalidPeer);
}

void Skate3NetSession::SendPong(const Pong& pong) {
  if (state_ != SessionState::kConnected) return;
  std::vector<uint8_t> b;
  EncodePong(pong, local_id_, b);
  SendRaw(b, /*reliable=*/false, /*exclude=*/kInvalidPeer);
}

void Skate3NetSession::SendTrickEvent(const TrickEvent& ev) {
  if (state_ != SessionState::kConnected) return;
  std::vector<uint8_t> b;
  EncodeTrickEvent(ev, local_id_, b);
  SendRaw(b, /*reliable=*/true, /*exclude=*/kInvalidPeer);
}

void Skate3NetSession::SendBailEvent(const BailEvent& ev) {
  if (state_ != SessionState::kConnected) return;
  std::vector<uint8_t> b;
  EncodeBailEvent(ev, local_id_, b);
  SendRaw(b, /*reliable=*/true, /*exclude=*/kInvalidPeer);
}

void Skate3NetSession::SendChat(const ChatMessage& msg) {
  if (state_ != SessionState::kConnected) return;
  std::vector<uint8_t> b;
  EncodeChatMessage(msg, local_id_, b);
  SendRaw(b, /*reliable=*/true, /*exclude=*/kInvalidPeer);
}

void Skate3NetSession::SendPartyInvite(const PartyInvite& inv) {
  if (state_ != SessionState::kConnected) return;
  std::vector<uint8_t> b;
  EncodePartyInvite(inv, local_id_, b);
  SendRaw(b, /*reliable=*/true, /*exclude=*/kInvalidPeer);
}

void Skate3NetSession::SendPartyState(const PartyState& ps) {
  if (state_ != SessionState::kConnected) return;
  std::vector<uint8_t> b;
  EncodePartyState(ps, local_id_, b);
  SendRaw(b, /*reliable=*/true, /*exclude=*/kInvalidPeer);
}

// --- Stats / lookup ---------------------------------------------------------

void Skate3NetSession::RefreshLinkStats() {
  if (enet_host_ == nullptr) return;
  auto* host = static_cast<ENetHost*>(enet_host_);
  for (size_t i = 0; i < host->peerCount; ++i) {
    ENetPeer* peer = &host->peers[i];
    if (peer->state != ENET_PEER_STATE_CONNECTED) continue;
    PeerId id = PeerIdFromData(peer->data);
    RemotePeer* rp = FindPeer(id);
    if (rp == nullptr) continue;
    rp->rtt_ms = peer->roundTripTime;
    rp->packet_loss = peer->packetLoss;
    rp->connected = true;
  }
}

RemotePeer* Skate3NetSession::FindPeer(PeerId id) {
  for (RemotePeer& rp : peers_) {
    if (rp.id == id) return &rp;
  }
  return nullptr;
}

}  // namespace skate3::net
