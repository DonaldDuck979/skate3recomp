#pragma once

// Skate 3 Recomp — online play: remote skater entity management.
//
// Manages lifecycle and state updates for "ghost" skater entities representing
// remote players. Each remote peer gets a presentation entity that the renderer
// displays; this module synchronizes that entity's transform, animation, and
// appearance with samples from the network layer.
//
// Design pattern: observer that listens to Skate3NetSystem events (peer join/leave)
// and update samples, driving a per-peer entity state machine.
//
// SCOPE: M0 is basic entity creation with position/rotation sync.
// Customization (clothes, boards) is post-M0.

#include <cstdint>
#include <map>
#include <vector>

#include "skate3_net_protocol.h"
#include "skate3_net_system.h"

namespace skate3::net {

// Per-remote-peer ghost entity state.
struct RemoteSkaterEntity {
  PeerId peer_id = kInvalidPeer;
  uint32_t entity_address = 0;    // game-side entity object address in guest memory.
  bool valid = false;             // entity was successfully created.
  bool has_custom = false;        // (future) customization data applied.
};

// Manager for remote skater entities. Integrates with Skate3NetSystem to:
// - Create ghost entities when peers join
// - Update entity transforms from interpolated samples each frame
// - Destroy entities when peers disconnect
class RemoteSkaterEntityManager {
 public:
  RemoteSkaterEntityManager();
  ~RemoteSkaterEntityManager();

  RemoteSkaterEntityManager(const RemoteSkaterEntityManager&) = delete;
  RemoteSkaterEntityManager& operator=(const RemoteSkaterEntityManager&) = delete;

  // Initialization: called once at startup (or when net system activates).
  // Registers for peer join/disconnect callbacks.
  void Initialize(Skate3NetSystem* net_system);

  // Cleanup: called at shutdown (or when net system deactivates).
  void Shutdown();

  // Per-frame update (called from game loop after SampleRemoteSkaters):
  // Applies interpolated state to each active remote skater entity.
  // @param samples  Vector of remote skater views from Skate3NetSystem
  void UpdateRemoteSkaters(const std::vector<RemoteSkaterView>& samples);

  // Query the entity address for a peer (for debugging/diagnostics).
  uint32_t GetEntityAddress(PeerId peer_id) const;

  // Introspection (for diagnostics overlay).
  size_t ActiveEntityCount() const { return entities_.size(); }

 private:
  // Callbacks wired to Skate3NetSystem peer events.
  void OnPeerJoined(const RemotePeer& peer);
  void OnPeerLeft(PeerId peer_id);

  // Core operations (game-specific, to be implemented once we know the entity API).
  // Returns the guest-memory address of the created entity, or 0 on failure.
  uint32_t CreateGhostSkaterEntity(const RemotePeer& peer);

  // Update a ghost entity's world transform in guest memory.
  // @param entity_addr  Guest-memory address of the entity
  // @param state        Interpolated skater state from network
  void UpdateEntityTransform(uint32_t entity_addr, const SkaterState& state);

  // Update animation/pose state (opaque to this module; passed through network state).
  void UpdateEntityAnimation(uint32_t entity_addr, const SkaterState& state);

  // Destroy a ghost entity.
  void DestroyGhostSkaterEntity(uint32_t entity_addr);

  Skate3NetSystem* net_system_ = nullptr;
  std::map<PeerId, RemoteSkaterEntity> entities_;

  // Stale sample detection: track which peers we've seen data for this frame
  // (to detect departed peers).
  std::vector<PeerId> peers_this_frame_;
};

// Global accessor (mirrors Skate3NetSystem pattern).
RemoteSkaterEntityManager& RemoteSkaterNetEntities();

// Lifecycle management (called from app layer).
void RemoteSkaterEntityManagerInitialize();
void RemoteSkaterEntityManagerShutdown();

}  // namespace skate3::net
