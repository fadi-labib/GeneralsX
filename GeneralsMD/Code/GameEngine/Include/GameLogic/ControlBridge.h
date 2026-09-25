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
  Int  m_myStart = -1;                   // index into m_starts nearest our command center; -1 unresolved
  void resolveMyStart(Player* me);       // lazily resolves m_myStart on first use after bind
  Int  m_bridgePlayerIndex = -1;
  Bool m_active = TRUE;
  struct LossCounters { Int unitsLost, buildingsLost, unitsDestroyed, buildingsDestroyed, unitsBuilt, buildingsBuilt; };
  LossCounters m_lastLosses = {0,0,0,0,0,0};
  Bool m_haveLastLosses = false;
  // Bumped by init() on every map load (shell map included) and sent as "match_id" on every
  // observe reply, so a client can tell matches apart without guessing from frame numbers.
  // Only ever increases within one engine process; starts at 0 on construction.
  UnsignedInt m_matchId = 0;
};
extern ControlBridge *TheControlBridge;
