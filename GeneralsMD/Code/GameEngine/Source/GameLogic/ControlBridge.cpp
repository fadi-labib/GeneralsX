// SPDX-License-Identifier: GPL-3.0-only
#include "PreRTS.h"
#include "GameLogic/ControlBridge.h"
#include "GameLogic/TerrainLogic.h"     // TheTerrainLogic, Waypoint
#include "GameLogic/PolygonTrigger.h"
#include "GameLogic/GameLogic.h"        // TheGameLogic
#include "GameLogic/Object.h"           // Object, getShroudedStatus/getTemplate/isKindOf
#include "Common/Player.h"              // Player, Money, Energy
#include "Common/PlayerList.h"          // ThePlayerList
#include "Common/Team.h"                // ObjectIterateFunc, Team
#include "Common/ThingTemplate.h"       // ThingTemplate::getName
#include "Common/KindOf.h"              // KINDOF_*
#include <utility>
#include <vector>
#include <cstdlib>                      // atoi
#include <cstring>                      // strstr
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/threading.h>       // MAIN_THREAD_EM_ASM(_INT): proxy to the browser main thread
#endif

ControlBridge *TheControlBridge = NULL;

// ---------------------------------------------------------------------------
// Small hand-rolled JSON helpers. We deliberately avoid pulling a JSON library
// into the engine: observe() emits a compact object built with AsciiString
// concat (AsciiString::format has a 2048-char per-call cap, so we format small
// chunks and concat them into an unbounded accumulator).
// ---------------------------------------------------------------------------
static void jsonEscape(AsciiString& out, const char* s)
{
  for (; s && *s; ++s) {
    char c = *s;
    switch (c) {
      case '"':  out.concat("\\\""); break;
      case '\\': out.concat("\\\\"); break;
      case '\n': out.concat("\\n");  break;
      case '\r': out.concat("\\r");  break;
      case '\t': out.concat("\\t");  break;
      default:
        if ((unsigned char)c < 0x20) break;   // drop other control chars
        out.concat(c);
    }
  }
}

// A name->count tally that preserves first-seen order (small N, linear scan).
struct CountEntry { AsciiString name; Int count; };

static void bumpCount(std::vector<CountEntry>& v, const AsciiString& name)
{
  for (CountEntry& e : v) { if (e.name == name) { ++e.count; return; } }
  CountEntry e; e.name = name; e.count = 1; v.push_back(e);
}

static void writeCountMap(AsciiString& j, const std::vector<CountEntry>& v)
{
  j.concat('{');
  Bool first = TRUE;
  for (const CountEntry& e : v) {
    if (!first) j.concat(',');
    first = FALSE;
    j.concat('"'); jsonEscape(j, e.name.str()); j.concat("\":");
    AsciiString num; num.format("%d", e.count); j.concat(num);
  }
  j.concat('}');
}

// ---------------------------------------------------------------------------
// iterateObjects accumulators. ObjectIterateFunc is a C function pointer
// (void(*)(Object*, void*)), so the callbacks are free functions and the
// per-call state travels in a struct passed as userData.
// ---------------------------------------------------------------------------
namespace {

struct OwnAccum {
  std::vector<CountEntry> structures;   // building template name -> count
  std::vector<CountEntry> unitsByType;  // unit template name -> count
  Int unitTotal;
  Int harvesters;
};

void ownObjectCb(Object* obj, void* ud)
{
  OwnAccum* a = (OwnAccum*)ud;
  if (!obj) return;
  const ThingTemplate* tt = obj->getTemplate();
  if (!tt) return;
  const AsciiString& nm = tt->getName();
  if (obj->isKindOf(KINDOF_STRUCTURE)) {
    bumpCount(a->structures, nm);
  } else if (obj->isKindOf(KINDOF_INFANTRY) || obj->isKindOf(KINDOF_VEHICLE)
          || obj->isKindOf(KINDOF_AIRCRAFT) || obj->isKindOf(KINDOF_DOZER)) {
    bumpCount(a->unitsByType, nm);
    ++a->unitTotal;
    if (obj->isKindOf(KINDOF_HARVESTER)) ++a->harvesters;
  }
}

struct EnemyAccum {
  Int observerIndex;                    // whose fog we read through
  std::vector<CountEntry> structures;   // only currently-visible enemy buildings
  std::vector<CountEntry> army;         // only currently-visible enemy units
  Int visibleObjects;
};

void enemyObjectCb(Object* obj, void* ud)
{
  EnemyAccum* a = (EnemyAccum*)ud;
  if (!obj) return;
  // FOG GATE: read enemy objects only through the observer's own visibility,
  // exactly as the human UI does. Skip anything fogged or shrouded so the agent
  // cannot see unscouted enemies.
  ObjectShroudStatus s = obj->getShroudedStatus(a->observerIndex);
  if (s != OBJECTSHROUD_CLEAR && s != OBJECTSHROUD_PARTIAL_CLEAR) return;
  const ThingTemplate* tt = obj->getTemplate();
  if (!tt) return;
  const AsciiString& nm = tt->getName();
  if (obj->isKindOf(KINDOF_STRUCTURE)) bumpCount(a->structures, nm);
  else                                 bumpCount(a->army, nm);
  ++a->visibleObjects;
}

} // anonymous namespace

// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Minimal flat-envelope JSON reader. The wire request is the fixed shape
//   {"id":<int>,"op":"<str>","args":{...}}
// (settled in Task 2.1). We only need `id` (int) and `op` (string) here; `args`
// is left raw because only `observe` is wired this task. This is NOT a general
// JSON parser — it scans for a top-level "key": and reads the scalar that
// follows. The flat envelope puts id/op before args, so the first match wins.
// ---------------------------------------------------------------------------
static const char* jsonFindValue(const char* s, const char* key)
{
  AsciiString pat; pat.format("\"%s\"", key);
  const char* p = strstr(s, pat.str());
  if (!p) return NULL;
  p += pat.getLength();
  while (*p && *p != ':') ++p;          // skip to the ':'
  if (*p != ':') return NULL;
  ++p;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
  return p;                             // points at the value's first char
}

static Bool jsonReadInt(const char* s, const char* key, Int& out)
{
  const char* p = jsonFindValue(s, key);
  if (!p) return FALSE;
  out = atoi(p);
  return TRUE;
}

static Bool jsonReadStr(const char* s, const char* key, AsciiString& out)
{
  const char* p = jsonFindValue(s, key);
  if (!p || *p != '"') return FALSE;
  ++p;
  out.clear();
  for (; *p && *p != '"'; ++p) {
    if (*p == '\\' && p[1]) {
      ++p;
      switch (*p) {
        case 'n': out.concat('\n'); break;
        case 't': out.concat('\t'); break;
        case 'r': out.concat('\r'); break;
        default:  out.concat(*p);   break;   // \" \\ \/ etc. pass through literally
      }
    } else {
      out.concat(*p);
    }
  }
  return TRUE;
}

void ControlBridge::tick()
{
  if (!TheGameLogic || !ThePlayerList) return;

  // Default the bridge player once a match is up: prefer the local human, else
  // the first player that owns buildings. (Phase 4 formalizes the bridge player;
  // observing player 0 / the base owner is fine for the round-trip.)
  if (m_bridgePlayerIndex < 0) {
    Player* lp = ThePlayerList->getLocalPlayer();
    if (lp && lp->countBuildings() > 0) {
      m_bridgePlayerIndex = lp->getPlayerIndex();
    } else {
      for (Int pi = 0; pi < ThePlayerList->getPlayerCount(); ++pi) {
        Player* p = ThePlayerList->getNthPlayer(pi);
        if (p && p->countBuildings() > 0) { m_bridgePlayerIndex = p->getPlayerIndex(); break; }
      }
    }
  }

#ifdef __EMSCRIPTEN__
  // Drain at most one queued control request per frame. gxControl.poll() copies
  // the next request into our stack buffer and returns its byte length (0 =
  // none). v1 ops are tiny JSON; 8192 comfortably exceeds any request (the
  // browser poll() drops anything larger, so this must stay ample).
  char buf[8192];
  Int n = MAIN_THREAD_EM_ASM_INT(
    { return (typeof gxControl !== 'undefined') ? gxControl.poll($0, $1) : 0; },
    (Int)buf, (Int)sizeof(buf));
  if (n <= 0) return;
  if (n >= (Int)sizeof(buf)) n = (Int)sizeof(buf) - 1;
  buf[n] = 0;

  Int id = 0;
  AsciiString op;
  jsonReadInt(buf, "id", id);
  jsonReadStr(buf, "op", op);

  // Dispatch. Only `observe` is wired this task; every other op gets an explicit
  // "not implemented" reply (coverage, never a silent drop) — Phase 3 adds the
  // apply ops.
  AsciiString result;
  if (op == "observe") {
    Int who = (m_bridgePlayerIndex >= 0) ? m_bridgePlayerIndex : 0;
    result = observe(who);
    if (result.isEmpty()) result = "{}";
  } else {
    result = "{\"accepted\":false,\"reason\":\"op not implemented (Phase 3)\"}";
  }

  // Reply {"id":<id>,"result":<result>}. Built with concat because `result` can
  // exceed AsciiString::format's 2048-char per-call cap (observe() is large).
  AsciiString reply;
  reply.format("{\"id\":%d,\"result\":", id);
  reply.concat(result);
  reply.concat("}");

  MAIN_THREAD_EM_ASM(
    { if (typeof gxControl !== 'undefined') gxControl.deliver($0, $1); },
    (Int)reply.str(), (Int)reply.getLength());
#endif // __EMSCRIPTEN__
}

AsciiString ControlBridge::observe(Int playerIndex)
{
  AsciiString j;
  if (!ThePlayerList || !TheGameLogic) return j;
  Player* me = ThePlayerList->getNthPlayer(playerIndex);
  if (!me) return j;

  const UnsignedInt cash     = me->getMoney()  ? me->getMoney()->countMoney()      : 0;
  const Int         produced = me->getEnergy() ? me->getEnergy()->getProduction()  : 0;
  const Int         consumed = me->getEnergy() ? me->getEnergy()->getConsumption() : 0;
  const Int         surplus  = produced - consumed;
  const Int         income   = me->getMoney()  ? (Int)me->getMoney()->getCashPerMinute() : 0;

  OwnAccum own; own.unitTotal = 0; own.harvesters = 0;
  me->iterateObjects(ownObjectCb, &own);

  AsciiString tmp;

  // Top-level + self.
  tmp.format("{\"frame\":%u,\"speed\":%.2f,\"match\":\"ongoing\",\"self\":{\"faction\":\"",
             (unsigned)TheGameLogic->getFrame(), 1.0f);
  j.concat(tmp);
  jsonEscape(j, me->getSide().str());
  tmp.format("\",\"cash\":%u,\"power\":{\"produced\":%d,\"consumed\":%d,\"surplus\":%d},"
             "\"under_attack\":false}",
             (unsigned)cash, produced, consumed, surplus);
  j.concat(tmp);

  // economy (income + collector count are real; supply-dock detail is v2).
  tmp.format(",\"economy\":{\"supply_collectors\":%d,\"income_per_min\":%d}",
             own.harvesters, income);
  j.concat(tmp);

  // structures (template-name -> count tally).
  j.concat(",\"structures\":");
  writeCountMap(j, own.structures);

  // army (total + by-type tally; teams[] is populated in a later task).
  tmp.format(",\"army\":{\"total\":%d,\"by_type\":", own.unitTotal);
  j.concat(tmp);
  writeCountMap(j, own.unitsByType);
  j.concat(",\"teams\":[]}");

  // production (ProductionUpdate scrape is v2).
  j.concat(",\"production\":[]");

  // enemy — fog-limited. One entry per enemy player; tallies count ONLY objects
  // currently visible to the observer (see enemyObjectCb's shroud gate).
  j.concat(",\"enemy\":{\"players\":[");
  Bool firstEnemy = TRUE;
  for (Int pi = 0; pi < ThePlayerList->getPlayerCount(); ++pi) {
    Player* other = ThePlayerList->getNthPlayer(pi);
    if (!other || other == me) continue;
    if (me->getRelationship(other->getDefaultTeam()) != ENEMIES) continue;

    EnemyAccum ea; ea.observerIndex = playerIndex; ea.visibleObjects = 0;
    other->iterateObjects(enemyObjectCb, &ea);

    if (!firstEnemy) j.concat(',');
    firstEnemy = FALSE;
    tmp.format("{\"player\":%d,\"faction\":\"", pi);
    j.concat(tmp);
    jsonEscape(j, other->getSide().str());
    j.concat("\",\"last_seen\":{\"structures_seen\":");
    writeCountMap(j, ea.structures);
    j.concat(",\"army_seen\":");
    writeCountMap(j, ea.army);
    tmp.format("},\"visible_objects\":%d,\"threats_near_me\":[]}", ea.visibleObjects);
    j.concat(tmp);
  }
  j.concat("]}");

  // map — named regions derived at init() (waypoints + polygon triggers).
  j.concat(",\"map\":{\"regions\":[");
  Bool firstR = TRUE;
  for (const BridgeRegion& r : m_regions) {
    if (!firstR) j.concat(',');
    firstR = FALSE;
    j.concat('"'); jsonEscape(j, r.name.str()); j.concat('"');
  }
  j.concat("]}");

  j.concat("}");
  return j;
}

const BridgeRegion* ControlBridge::regionByName(const AsciiString& n) const
{
  for (const BridgeRegion& r : m_regions) if (r.name == n) return &r;
  return NULL;
}
