// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Common/AsciiString.h"
#include "Lib/BaseType.h"
#include "Common/GameType.h"   // Coord3D
#include <vector>

class Player;

struct BridgeRegion { AsciiString name; Coord3D center; Real radius; };

class ControlBridge
{
public:
  void init(Bool active = TRUE);     // enumerate regions from the loaded map; active=FALSE (multiplayer) makes tick() inert
  void tick();                       // per-frame; drains the WS command queue (Phase 2)
  AsciiString observe(Int playerIndex);        // spec §4 JSON
  AsciiString apply(const AsciiString& op, const char* requestJson);  // Phase 3 write-ops
  const std::vector<BridgeRegion>& regions() const { return m_regions; }
  const BridgeRegion* regionByName(const AsciiString& n) const;
  Int bridgePlayerIndex() const { return m_bridgePlayerIndex; }
private:
  std::vector<BridgeRegion> m_regions;
  std::vector<BridgeRegion> m_starts;    // Player_N_Start waypoints, in player-number order (Task 8)
  std::vector<Int> m_startNum;           // waypoint number N of each m_starts entry (Player_N_Start)
  Int  m_myStart = -1;                   // index into m_starts nearest our command center; -1 unresolved
  // Round B: which player each start belongs to, resolved per player from its command center
  // (ours -> m_myStart, each ENEMIES player -> one enemy start). -1 = nobody resolved (empty,
  // allied, or an enemy whose command center was never found). Cached for the whole match.
  std::vector<Int> m_startOwner;         // parallel to m_starts: owning player index or -1
  std::vector<Int> m_enemyStarts;        // m_starts indices in enemy_start_N order (N = position + 1)
  void resolveMyStart(Player* me);       // lazily resolves m_myStart on first use after bind
  void resolveStarts(Player* me);        // resolveMyStart, then each enemy player's start (lazy, cached)
  Int  m_bridgePlayerIndex = -1;
  Bool m_active = TRUE;
  struct LossCounters { Int unitsLost, buildingsLost, unitsDestroyed, buildingsDestroyed, unitsBuilt, buildingsBuilt; };
  LossCounters m_lastLosses = {0,0,0,0,0,0};
  Bool m_haveLastLosses = false;
  // Advanced by init() on every map load (shell map included) and sent as "match_id" on every
  // observe reply, so a client can tell matches apart without guessing from frame numbers.
  // Set to max(previous + 1, wall-clock seconds), so it keeps increasing across page reloads
  // too (assuming the wall clock does not step backwards). 0 until the first map load.
  UnsignedInt m_matchId = 0;
};
extern ControlBridge *TheControlBridge;
