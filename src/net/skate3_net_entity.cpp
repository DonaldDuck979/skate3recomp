#include "skate3_net_entity.h"

#include <algorithm>
#include <cstring>

#include <rex/logging.h>

namespace skate3::net {

namespace {

RemoteSkaterEntityManager* g_instance = nullptr;

}  // namespace

RemoteSkaterEntityManager::RemoteSkaterEntityManager() = default;

RemoteSkaterEntityManager::~RemoteSkaterEntityManager() { Shutdown(); }

void RemoteSkaterEntityManager::Initialize(Skate3NetSystem* net_system) {
  if (net_system_ != nullptr) return;  // Already initialized.

  net_system_ = net_system;
  if (!net_system_) {
    REXLOG_WARN("[net-entity] Initialize called without a net system");
    return;
  }

  REXLOG_INFO("[net-entity] Remote skater entity manager initialized");
}

void RemoteSkaterEntityManager::Shutdown() {
  // Destroy all remaining remote entities.
  std::vector<PeerId> to_destroy;
  for (const auto& pair : entities_) {
    if (pair.second.valid) {
      to_destroy.push_back(pair.first);
    }
  }
  for (PeerId id : to_destroy) {
    OnPeerLeft(id);
  }

  net_system_ = nullptr;
  REXLOG_INFO("[net-entity] Remote skater entity manager shut down");
}

void RemoteSkaterEntityManager::UpdateRemoteSkaters(
    const std::vector<RemoteSkaterView>& samples) {
  if (!net_system_ || !net_system_->active()) {
    return;
  }

  // Track which peers have samples this frame.
  peers_this_frame_.clear();
  for (const auto& view : samples) {
    if (!view.valid) continue;
    peers_this_frame_.push_back(view.id);

    auto it = entities_.find(view.id);
    if (it == entities_.end()) {
      // New peer without an entity yet; will be created via OnPeerJoined
      // callback. This shouldn't happen in normal flow, but be safe.
      REXLOG_WARN("[net-entity] Skipped update for peer {} (entity not yet created)", view.id);
      continue;
    }

    RemoteSkaterEntity& entity = it->second;
    if (!entity.valid) {
      REXLOG_WARN("[net-entity] Skipped update for peer {} (entity creation failed)", view.id);
      continue;
    }

    // Apply the interpolated state to the entity.
    UpdateEntityTransform(entity.entity_address, view.state);
    UpdateEntityAnimation(entity.entity_address, view.state);
  }

  // TODO(M1): Detect and handle stale peers that disappeared mid-frame
  // (peers we had entities for but got no samples).
}

uint32_t RemoteSkaterEntityManager::GetEntityAddress(PeerId peer_id) const {
  auto it = entities_.find(peer_id);
  if (it == entities_.end()) {
    return 0;
  }
  return it->second.valid ? it->second.entity_address : 0;
}

void RemoteSkaterEntityManager::OnPeerJoined(const RemotePeer& peer) {
  if (entities_.find(peer.id) != entities_.end()) {
    REXLOG_WARN("[net-entity] Peer {} join: already have an entity", peer.id);
    return;
  }

  RemoteSkaterEntity entity;
  entity.peer_id = peer.id;
  entity.entity_address = CreateGhostSkaterEntity(peer);
  entity.valid = (entity.entity_address != 0);

  if (entity.valid) {
    REXLOG_INFO("[net-entity] Created ghost entity for peer {} at 0x{:08X}", peer.id,
                entity.entity_address);
  } else {
    REXLOG_ERROR("[net-entity] Failed to create ghost entity for peer {}", peer.id);
  }

  entities_[peer.id] = entity;
}

void RemoteSkaterEntityManager::OnPeerLeft(PeerId peer_id) {
  auto it = entities_.find(peer_id);
  if (it == entities_.end()) {
    REXLOG_WARN("[net-entity] Peer {} left: no entity found", peer_id);
    return;
  }

  RemoteSkaterEntity& entity = it->second;
  if (entity.valid) {
    DestroyGhostSkaterEntity(entity.entity_address);
    REXLOG_INFO("[net-entity] Destroyed ghost entity for peer {}", peer_id);
  }

  entities_.erase(it);
}

uint32_t RemoteSkaterEntityManager::CreateGhostSkaterEntity(const RemotePeer& peer) {
  // TODO: Implement using the game's entity creation API.
  // This is a placeholder that logs but returns 0 (failure).
  //
  // Rough sketch of what needs to happen:
  // 1. Find or allocate a Sk8::SkaterPresEntity object
  // 2. Set up model/appearance data (for now, use default/local skater model)
  // 3. Register with the render system to make it visible
  // 4. Return the guest-memory address of the entity object
  //
  // Hook point: likely need to call into the game's entity factory or
  // find the local skater entity template and clone it.
  //
  REXLOG_INFO("[net-entity] [STUB] CreateGhostSkaterEntity for peer {} ({})", peer.id,
              peer.name);
  return 0;
}

void RemoteSkaterEntityManager::UpdateEntityTransform(uint32_t entity_addr,
                                                       const SkaterState& state) {
  // TODO: Implement using the game's entity API.
  // Likely need to:
  // 1. Cast entity_addr to the game's entity type (uint8_t* base + entity_addr)
  // 2. Find the m_MatLtoWTrans offset (entity+416 per entity tracking header)
  // 3. Write the position and rotation to that matrix location
  // 4. Signal the render system that the entity is dirty
  //
  // For now, silent no-op. Once hooked, log sparingly to avoid spam.
  (void)entity_addr;
  (void)state;
}

void RemoteSkaterEntityManager::UpdateEntityAnimation(uint32_t entity_addr,
                                                      const SkaterState& state) {
  // TODO: Implement animation state updates.
  // - anim_id: likely drives animation playback (needs game API to set pose)
  // - trick_id: visual feedback for trick in progress (optional M0)
  // - board_id: update equipped board model (optional M0)
  // - flags: state flags (grounded, bailing, grinding) for pose hints
  //
  // For now, silent no-op.
  (void)entity_addr;
  (void)state;
}

void RemoteSkaterEntityManager::DestroyGhostSkaterEntity(uint32_t entity_addr) {
  // TODO: Implement using the game's entity destruction API.
  // Likely need to:
  // 1. Find/validate the entity at entity_addr
  // 2. Remove from render views
  // 3. Free the object
  // 4. Signal cleanup
  //
  // For now, silent no-op.
  (void)entity_addr;
}

// --- Global instance management ---

RemoteSkaterEntityManager& RemoteSkaterNetEntities() {
  static RemoteSkaterEntityManager instance;
  return instance;
}

void RemoteSkaterEntityManagerInitialize() {
  auto& mgr = RemoteSkaterNetEntities();
  auto& net_sys = skate3::net::Skate3Net();
  mgr.Initialize(&net_sys);
}

void RemoteSkaterEntityManagerShutdown() {
  auto& mgr = RemoteSkaterNetEntities();
  mgr.Shutdown();
}

}  // namespace skate3::net
