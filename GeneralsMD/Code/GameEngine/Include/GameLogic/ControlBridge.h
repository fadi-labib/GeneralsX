// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Common/AsciiString.h"
#include "Lib/BaseType.h"
#include "Common/GameType.h"   // Coord3D
#include <vector>

struct BridgeRegion { AsciiString name; Coord3D center; Real radius; };

class ControlBridge
{
public:
  void init();                       // enumerate regions from the loaded map
  void tick();                       // per-frame; drains the WS command queue (Phase 2)
  AsciiString observe(Int playerIndex);        // spec §4 JSON
  AsciiString apply(const AsciiString& op, const char* requestJson);  // Phase 3 write-ops
  const std::vector<BridgeRegion>& regions() const { return m_regions; }
  const BridgeRegion* regionByName(const AsciiString& n) const;
  Int bridgePlayerIndex() const { return m_bridgePlayerIndex; }
private:
  std::vector<BridgeRegion> m_regions;
  Int  m_bridgePlayerIndex = -1;
};
extern ControlBridge *TheControlBridge;
