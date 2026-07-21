// SPDX-License-Identifier: GPL-3.0-only
#include "PreRTS.h"
#include "GameLogic/ControlBridge.h"
#include "GameLogic/TerrainLogic.h"     // TheTerrainLogic, Waypoint
#include "GameLogic/PolygonTrigger.h"

ControlBridge *TheControlBridge = NULL;

void ControlBridge::init()
{
  m_regions.clear();
  for (Waypoint *w = TheTerrainLogic ? TheTerrainLogic->getFirstWaypoint() : NULL;
       w; w = w->getNext()) {
    BridgeRegion r; r.name = w->getName(); r.center = *w->getLocation(); r.radius = 0.0f;
    m_regions.push_back(r);
  }
  for (PolygonTrigger *t = PolygonTrigger::getFirstPolygonTrigger(); t; t = t->getNext()) {
    BridgeRegion r; r.name = t->getTriggerName();
    t->getCenterPoint(&r.center); r.radius = t->getRadius();
    m_regions.push_back(r);
  }
  m_regionsBuilt = true;
  fprintf(stderr, "[BRIDGE] %zu regions derived\n", m_regions.size());
  fflush(stderr);
}

void ControlBridge::tick()
{
  // Phase 2: drains the WS command queue. No-op for now (Task 1.1 is
  // region derivation only).
}

AsciiString ControlBridge::observe(Int playerIndex)
{
  // Phase 2/spec §4: full JSON observation. Not implemented in Task 1.1.
  (void)playerIndex;
  return AsciiString::TheEmptyString;
}

const BridgeRegion* ControlBridge::regionByName(const AsciiString& n) const
{
  for (const BridgeRegion& r : m_regions) if (r.name == n) return &r;
  return NULL;
}
